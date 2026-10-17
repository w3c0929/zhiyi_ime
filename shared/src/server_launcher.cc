// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/server_launcher.h>

#include <cstddef>
#include <string>

#include <windows.h>

#include <cxxime/logging.h>

#include "settings_launcher_util.h"

namespace cxxime {
namespace {

constexpr wchar_t kInstallRegistryKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ZhiyuIME";
constexpr wchar_t kInstallerMutexName[] = L"Global\\ZhiyuIME.Installation";

bool mutex_exists(const wchar_t* name) {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (mutex) {
        CloseHandle(mutex);
        return true;
    }
    // Access denied also means it exists.
    return GetLastError() == ERROR_ACCESS_DENIED;
}

// A medium integrity program outside an AppContainer, like Explorer.
bool process_may_start_server() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    DWORD app_container = 0;
    DWORD size = 0;
    bool allowed = GetTokenInformation(token, TokenIsAppContainer, &app_container,
                                       sizeof(app_container), &size) &&
                   app_container == 0;
    if (allowed) {
        alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) +
                                                  SECURITY_MAX_SID_SIZE] = {};
        auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
        allowed = GetTokenInformation(token, TokenIntegrityLevel, label, sizeof(buffer), &size) !=
                  FALSE;
        if (allowed) {
            PSID sid = label->Label.Sid;
            const DWORD level =
                *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
            allowed = level >= SECURITY_MANDATORY_MEDIUM_RID &&
                      level < SECURITY_MANDATORY_HIGH_RID;
        }
    }
    CloseHandle(token);
    return allowed;
}

// The installed version directory, as the registry holds it (the installer keeps it on the active
// version; no trailing slash). Empty when there is no install or the key cannot be read.
std::wstring read_install_location() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kInstallRegistryKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS) {
        return {};
    }
    wchar_t install_path[MAX_PATH] = {};
    DWORD value_type = 0;
    DWORD value_bytes = sizeof(install_path);
    const LONG result = RegQueryValueExW(key, L"InstallLocation", nullptr, &value_type,
                                         reinterpret_cast<BYTE*>(install_path), &value_bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || value_type != REG_SZ || value_bytes > sizeof(install_path)) {
        return {};
    }
    return install_path;  // the buffer is zero-filled, so it is NUL-terminated
}

std::wstring registered_server_path() {
    const std::wstring install_location = read_install_location();
    std::wstring settings_path;
    if (install_location.empty() ||
        !build_registered_settings_path(install_location.c_str(),
                                        (install_location.size() + 1) * sizeof(wchar_t),
                                        &settings_path)) {
        return {};
    }
    // The programs are installed side by side.
    const std::wstring path =
        settings_path.substr(0, settings_path.find_last_of(L'\\') + 1) + L"zhiyu-server.exe";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return {};
    }
    return path;
}

} // namespace

bool start_server_on_demand() {
    if (mutex_exists(kServerInstanceMutex) || mutex_exists(kInstallerMutexName) ||
        !process_may_start_server()) {
        return false;
    }
    const std::wstring path = registered_server_path();
    if (path.empty()) {
        CXXIME_LOG(L"%s", L"server_launch result=0 reason=active_path_unavailable");
        return false;
    }
    const std::wstring directory = path.substr(0, path.find_last_of(L'\\'));
    std::wstring command_line = L"\"" + path + L"\"";
    STARTUPINFOW startup_info = {};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info = {};
    if (!CreateProcessW(path.c_str(), &command_line[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, directory.c_str(),
                        &startup_info, &process_info)) {
        CXXIME_LOG(L"server_launch result=0 error=%lu", GetLastError());
        return false;
    }
    CloseHandle(process_info.hProcess);
    CloseHandle(process_info.hThread);
    CXXIME_LOG(L"%s", L"server_launch result=1");
    return true;
}

std::string active_install_location() {
    const std::wstring install_location = read_install_location();
    if (install_location.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, install_location.data(),
                                         static_cast<int>(install_location.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, install_location.data(),
                        static_cast<int>(install_location.size()), result.data(), size, nullptr,
                        nullptr);
    return result;
}

} // namespace cxxime
