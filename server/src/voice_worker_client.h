// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The server side of voice-runtime\zhiyu-voice-worker.exe (voice_worker/voice_worker.cc):
// the sherpa-onnx recognisers (Fun-ASR-Nano, Qwen3-ASR) run there because sherpa-onnx needs a
// newer ONNX Runtime than the DirectML one the server loads for the recommendation model.
#ifndef CXXIME_SERVER_VOICE_WORKER_CLIENT_H_
#define CXXIME_SERVER_VOICE_WORKER_CLIENT_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include <windows.h>

#include <cxxime/voice_files.h>

class VoiceWorkerClient {
public:
    VoiceWorkerClient() = default;
    ~VoiceWorkerClient();
    VoiceWorkerClient(const VoiceWorkerClient&) = delete;
    VoiceWorkerClient& operator=(const VoiceWorkerClient&) = delete;

    // Loads `model` in the worker, starting the process when needed.
    bool Load(const cxxime::voice::InstalledVoiceModel& model, int threads, std::string* error);
    bool Transcribe(const float* samples, std::size_t count, std::string* text,
                    std::string* error);
    // Stops the worker (next Load starts a new one).
    void Reset();
    bool loaded() const { return process_ != nullptr; }
    const std::string& model_id() const { return model_id_; }

private:
    bool Start(std::string* error);
    bool Request(const std::string& json, std::string* response, int timeout_ms,
                 std::string* error);
    bool ReadExact(void* buffer, std::size_t size, int timeout_ms, std::string* error);
    bool WriteAll(const void* buffer, std::size_t size, std::string* error);

    HANDLE process_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    std::string model_id_;
};

#endif  // CXXIME_SERVER_VOICE_WORKER_CLIENT_H_
