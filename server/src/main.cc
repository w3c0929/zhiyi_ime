// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include "server_app.h"

#include <cstdio>
#include <string>

#include <objbase.h>
#include <shellapi.h>

#include <cxxime/ai_client.h>
#include <cxxime/config.h>
#include <cxxime/data_path.h>
#include <cxxime/query_trace.h>
#include <cxxime/server_launcher.h>

static std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], len, nullptr, nullptr);
    return result;
}

static std::string get_arg(int argc, LPWSTR* argv, const std::wstring& flag) {
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] == flag)
            return wide_to_utf8(argv[i + 1]);
    }
    return {};
}

static bool has_flag(int argc, LPWSTR* argv, const std::wstring& flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == flag)
            return true;
    }
    return false;
}

// A diagnostic: exercise the online AI path (ai_client.h) from the command line, the same code
// the selection translation uses. The key is DPAPI-protected and unprotected again on the way,
// so this checks the stored-key path too.
//
//   zhiyu-server.exe --ai-test --key <api key> [--provider id] [--base-url url]
//                    [--model name] [--text "text to translate"]
static int run_ai_test(int argc, LPWSTR* argv) {
    // The server is a windowed program: borrow the console that started it, or the report is lost.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* console = nullptr;
        freopen_s(&console, "CONOUT$", "w", stdout);
        freopen_s(&console, "CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(CP_UTF8);
    cxxime::AiConfig config;
    config.enabled = true;
    const std::string provider = get_arg(argc, argv, L"--provider");
    config.provider = provider.empty() ? "deepseek" : provider;
    config.base_url = get_arg(argc, argv, L"--base-url");
    config.model = get_arg(argc, argv, L"--model");
    const std::string key = get_arg(argc, argv, L"--key");
    if (key.empty()) {
        printf("usage: zhiyu-server.exe --ai-test --key <api key> [--provider id] [--base-url "
               "url] [--model name] [--text \"text\"]\n");
        return 2;
    }
    const std::string stored = cxxime::ai::protect_secret(key);
    if (stored.empty()) {
        printf("the key could not be protected (DPAPI)\n");
        return 2;
    }
    config.keys.push_back(stored);
    std::string text = get_arg(argc, argv, L"--text");
    if (text.empty()) text = "今天天气很好，我们去公园散步吧。";
    std::string out;
    std::string error;
    if (!cxxime::ai::translate(config, text, "en", &out, &error, nullptr)) {
        printf("failed: %s\n",
               error.empty() ? cxxime::ai::last_error().c_str() : error.c_str());
        return 1;
    }
    printf("provider=%s model=%s\n%s\n", config.provider.c_str(),
           cxxime::ai::effective_model(config).c_str(), out.c_str());
    // A windowed program's stdout is easy to lose; --out keeps the report for reading.
    const std::string out_path = get_arg(argc, argv, L"--out");
    if (!out_path.empty()) {
        const std::wstring wide_path(out_path.begin(), out_path.end());
        if (FILE* file = _wfopen(wide_path.c_str(), L"wb")) {
            fprintf(file, "provider=%s model=%s\n%s\n", config.provider.c_str(),
                    cxxime::ai::effective_model(config).c_str(), out.c_str());
            fclose(file);
        }
    }
    return 0;
}

// startup.autostart is off: the sign-in Run entry (--autostart) does not start the server; the
// input method starts it when it is switched to.
static bool autostart_disabled(const std::string& config_path) {
    cxxime::Config config;
    if (!config.load(config_path.empty() ? cxxime::data_path("default.json") : config_path))
        return false;
    config.load_user(cxxime::user_data_path("default.json"));
    return !config.autostart;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) {
        return 1;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    std::string dict_path;
    std::string config_path;
    bool autostart = false;
    if (argv) {
        // --data sets base data directory (overrides compile-time/default path)
        std::string data_dir = get_arg(argc, argv, L"--data");
        if (!data_dir.empty())
            cxxime::set_data_dir(data_dir);

        dict_path = get_arg(argc, argv, L"--dict");
        config_path = get_arg(argc, argv, L"--config");
        autostart = has_flag(argc, argv, L"--autostart");
        if (has_flag(argc, argv, L"--ai-test")) {
            const int test = run_ai_test(argc, argv);
            LocalFree(argv);
            CoUninitialize();
            return test;
        }
        LocalFree(argv);
    }

    // Settings is moving the data folder: load nothing until it is done (the input method may
    // start the server meanwhile from any program).
    if (HANDLE moving = OpenMutexW(SYNCHRONIZE, FALSE, cxxime::kDataMoveMutex)) {
        const DWORD waited = WaitForSingleObject(moving, INFINITE);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) ReleaseMutex(moving);
        CloseHandle(moving);
    }

    // One server per sign-in session: the input method may start it from several programs at
    // once (server_launcher.h).
    HANDLE instance = CreateMutexW(nullptr, TRUE, cxxime::kServerInstanceMutex);
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS ||
        (autostart && autostart_disabled(config_path))) {
        if (instance)
            CloseHandle(instance);
        CoUninitialize();
        return 0;
    }

    ServerApp app;
    if (!app.initialize(dict_path, config_path)) {
        CloseHandle(instance);
        CoUninitialize();
        return 1;
    }
    app.run();
    app.finalize();

    // Shutdown async trace writer (flush remaining entries)
    cxxime::QueryTrace::shutdown();
    CloseHandle(instance);
    CoUninitialize();

    return 0;
}
