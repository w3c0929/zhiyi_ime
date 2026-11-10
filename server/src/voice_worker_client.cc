// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include "voice_worker_client.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <vector>

#include <json.hpp>

namespace {

std::wstring WorkerPath() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) return {};
    std::wstring path(buffer, length);
    path.erase(path.find_last_of(L"\\/") + 1);
    path += L"voice-runtime\\zhiyu-voice-worker.exe";
    return path;
}

std::string Base64Encode(const void* data, std::size_t size) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= size) {
        const uint32_t value = (static_cast<uint32_t>(bytes[i]) << 16) |
                               (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                               static_cast<uint32_t>(bytes[i + 2]);
        out.push_back(alphabet[(value >> 18) & 63]);
        out.push_back(alphabet[(value >> 12) & 63]);
        out.push_back(alphabet[(value >> 6) & 63]);
        out.push_back(alphabet[value & 63]);
        i += 3;
    }
    if (i < size) {
        uint32_t value = static_cast<uint32_t>(bytes[i]) << 16;
        if (i + 1 < size) value |= static_cast<uint32_t>(bytes[i + 1]) << 8;
        out.push_back(alphabet[(value >> 18) & 63]);
        out.push_back(alphabet[(value >> 12) & 63]);
        out.push_back(i + 1 < size ? alphabet[(value >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length,
                        nullptr, nullptr);
    return out;
}

}  // namespace

VoiceWorkerClient::~VoiceWorkerClient() { Reset(); }

bool VoiceWorkerClient::Start(std::string* error) {
    if (process_) return true;
    const std::wstring exe = WorkerPath();
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (error) *error = "voice worker not found: " + WideToUtf8(exe);
        return false;
    }
    SECURITY_ATTRIBUTES attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE stdout_write = nullptr;
    HANDLE stdin_read = nullptr;
    if (!CreatePipe(&stdout_read_, &stdout_write, &attributes, 0) ||
        !SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stdin_read, &stdin_write_, &attributes, 0) ||
        !SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0)) {
        if (error) *error = "cannot create the worker pipes";
        Reset();
        return false;
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = nullptr;
    PROCESS_INFORMATION info = {};
    std::wstring command = L"\"" + exe + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL started =
        CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    if (!started) {
        if (error) *error = "cannot start the voice worker";
        Reset();
        return false;
    }
    CloseHandle(info.hThread);
    process_ = info.hProcess;
    return true;
}

bool VoiceWorkerClient::WriteAll(const void* buffer, std::size_t size, std::string* error) {
    const auto* bytes = static_cast<const char*>(buffer);
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(stdin_write_, bytes, static_cast<DWORD>(size), &written, nullptr)) {
            if (error) *error = "cannot write to the voice worker";
            return false;
        }
        bytes += written;
        size -= written;
    }
    return true;
}

bool VoiceWorkerClient::ReadExact(void* buffer, std::size_t size, int timeout_ms,
                                  std::string* error) {
    auto* bytes = static_cast<char*>(buffer);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::size_t read = 0;
    while (read < size) {
        DWORD available = 0;
        if (!PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &available, nullptr)) {
            if (error) *error = "the voice worker closed its pipe";
            return false;
        }
        if (available == 0) {
            if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
                if (error) *error = "the voice worker exited";
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                if (error) *error = "the voice worker did not answer in time";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>((std::min<std::size_t>)(size - read, available));
        if (!ReadFile(stdout_read_, bytes + read, want, &got, nullptr) || got == 0) {
            if (error) *error = "cannot read from the voice worker";
            return false;
        }
        read += got;
    }
    return true;
}

bool VoiceWorkerClient::Request(const std::string& json, std::string* response, int timeout_ms,
                                std::string* error) {
    const uint32_t length = static_cast<uint32_t>(json.size());
    const uint8_t header[4] = {static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8),
                               static_cast<uint8_t>(length >> 16),
                               static_cast<uint8_t>(length >> 24)};
    if (!WriteAll(header, sizeof(header), error) || !WriteAll(json.data(), json.size(), error)) {
        return false;
    }
    uint8_t reply_header[4];
    if (!ReadExact(reply_header, sizeof(reply_header), timeout_ms, error)) return false;
    const uint32_t reply_length = static_cast<uint32_t>(reply_header[0]) |
                                  (static_cast<uint32_t>(reply_header[1]) << 8) |
                                  (static_cast<uint32_t>(reply_header[2]) << 16) |
                                  (static_cast<uint32_t>(reply_header[3]) << 24);
    if (reply_length == 0 || reply_length > 64u * 1024u * 1024u) {
        if (error) *error = "bad answer from the voice worker";
        return false;
    }
    response->resize(reply_length);
    return ReadExact(response->data(), reply_length, timeout_ms, error);
}

bool VoiceWorkerClient::Load(const cxxime::voice::InstalledVoiceModel& model, int threads,
                             std::string* error) {
    if (!Start(error)) return false;
    if (model_id_ == model.id) return true;
    nlohmann::json paths = nlohmann::json::object();
    if (model.engine == "funasr-nano") {
        paths["encoder_adaptor"] = WideToUtf8(model.encoder_adaptor);
        paths["llm"] = WideToUtf8(model.llm);
        paths["embedding"] = WideToUtf8(model.embedding);
        paths["tokenizer"] = WideToUtf8(model.tokenizer);
    } else if (model.engine == "qwen3-asr") {
        paths["conv_frontend"] = WideToUtf8(model.conv_frontend);
        paths["encoder"] = WideToUtf8(model.encoder);
        paths["decoder"] = WideToUtf8(model.decoder);
        paths["tokenizer"] = WideToUtf8(model.tokenizer);
    } else {
        if (error) *error = "the worker has no recogniser for " + model.engine;
        return false;
    }
    const nlohmann::json request{{"op", "load"},
                                 {"engine", model.engine},
                                 {"paths", paths},
                                 {"threads", threads},
                                 {"vad", WideToUtf8(model.vad)},
                                 {"segment_seconds", model.engine == "qwen3-asr" ? 20 : 15},
                                 {"max_segment_seconds", model.engine == "qwen3-asr" ? 25 : 20},
                                 {"language", "auto"},
                                 {"itn", true}};
    std::string response;
    if (!Request(request.dump(), &response, 180000, error)) {
        Reset();
        return false;
    }
    const nlohmann::json reply = nlohmann::json::parse(response, nullptr, false);
    if (!reply.is_object() || !reply.value("ok", false)) {
        if (error) *error = reply.is_object() ? reply.value("error", std::string("load failed"))
                                              : std::string("bad answer from the voice worker");
        Reset();
        return false;
    }
    model_id_ = model.id;
    return true;
}

bool VoiceWorkerClient::Transcribe(const float* samples, std::size_t count, std::string* text,
                                   std::string* error) {
    if (!process_ || !text) {
        if (error) *error = "the voice worker is not loaded";
        return false;
    }
    std::vector<int16_t> pcm(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float value = (std::max)(-1.0f, (std::min)(1.0f, samples[i]));
        pcm[i] = static_cast<int16_t>(value * 32767.0f);
    }
    const nlohmann::json request{
        {"op", "transcribe"},
        {"pcm16", Base64Encode(pcm.data(), pcm.size() * sizeof(int16_t))}};
    std::string response;
    if (!Request(request.dump(), &response, 300000, error)) {
        Reset();
        return false;
    }
    const nlohmann::json reply = nlohmann::json::parse(response, nullptr, false);
    if (!reply.is_object() || !reply.value("ok", false)) {
        if (error) *error = reply.is_object()
                                ? reply.value("error", std::string("recognition failed"))
                                : std::string("bad answer from the voice worker");
        return false;
    }
    *text = reply.value("text", std::string());
    return true;
}

void VoiceWorkerClient::Reset() {
    if (process_ && stdin_write_) {
        const std::string shutdown = R"({"op":"shutdown"})";
        const uint32_t length = static_cast<uint32_t>(shutdown.size());
        const uint8_t header[4] = {static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8),
                                   static_cast<uint8_t>(length >> 16),
                                   static_cast<uint8_t>(length >> 24)};
        DWORD written = 0;
        WriteFile(stdin_write_, header, sizeof(header), &written, nullptr);
        WriteFile(stdin_write_, shutdown.data(), static_cast<DWORD>(shutdown.size()), &written,
                  nullptr);
        WaitForSingleObject(process_, 2000);
    }
    if (process_) {
        TerminateProcess(process_, 0);
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (stdin_write_) {
        CloseHandle(stdin_write_);
        stdin_write_ = nullptr;
    }
    if (stdout_read_) {
        CloseHandle(stdout_read_);
        stdout_read_ = nullptr;
    }
    model_id_.clear();
}
