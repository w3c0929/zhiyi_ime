// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The voice recognisers that need sherpa-onnx (Fun-ASR-Nano, Qwen3-ASR), in a process of their
// own: sherpa-onnx is built against a newer ONNX Runtime than the DirectML one zhiyu-server.exe
// loads for the recommendation model, and Windows keeps one DLL per name in a process. The
// server starts voice-runtime\zhiyu-voice-worker.exe and talks to it over stdin/stdout:
//
//   request  = [uint32 little-endian length][UTF-8 JSON]
//   response = [uint32 little-endian length][UTF-8 JSON]
//
//   {"op":"load","engine":"funasr-nano"|"qwen3-asr","paths":{...},"threads":4,
//    "vad":"...\\silero_vad.onnx","segment_seconds":15,"max_segment_seconds":20,
//    "language":"auto","itn":true}
//   {"op":"transcribe","pcm16":"<base64 little-endian int16 16 kHz>"}
//   {"op":"bench","pcm16":"...","repeats":3}
//   {"op":"shutdown"}
//
// A response is {"ok":true,...} or {"ok":false,"error":"..."}. VAD (sherpa-onnx's Silero)
// splits a recording into segments of at most max_segment_seconds; the LLM exports share a
// 512-token cache with the audio and drop audio past it, so the cap matters.

#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"
#include "json.hpp"

namespace {

constexpr int kSampleRate = 16000;
constexpr int kVadWindow = 512;
constexpr std::size_t kMaxPayload = 64u * 1024u * 1024u;

std::string Base64Decode(const std::string& text) {
    static int8_t table[256];
    static bool ready = false;
    if (!ready) {
        std::memset(table, -1, sizeof(table));
        const char* alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[static_cast<unsigned char>(alphabet[i])] = i;
        ready = true;
    }
    std::string out;
    out.reserve(text.size() * 3 / 4);
    uint32_t bits = 0;
    int count = 0;
    for (const char ch : text) {
        const int value = table[static_cast<unsigned char>(ch)];
        if (value < 0) continue;  // '=' and whitespace
        bits = (bits << 6) | static_cast<uint32_t>(value);
        if (++count == 4) {
            out.push_back(static_cast<char>((bits >> 16) & 0xFF));
            out.push_back(static_cast<char>((bits >> 8) & 0xFF));
            out.push_back(static_cast<char>(bits & 0xFF));
            bits = 0;
            count = 0;
        }
    }
    if (count == 2) {
        out.push_back(static_cast<char>((bits >> 4) & 0xFF));
    } else if (count == 3) {
        out.push_back(static_cast<char>((bits >> 10) & 0xFF));
        out.push_back(static_cast<char>((bits >> 2) & 0xFF));
    }
    return out;
}

bool ReadExact(void* buffer, std::size_t size) {
    return std::fread(buffer, 1, size, stdin) == size;
}

bool WriteExact(const void* buffer, std::size_t size) {
    return std::fwrite(buffer, 1, size, stdout) == size;
}

bool ReadMessage(std::string* payload) {
    uint8_t header[4];
    if (!ReadExact(header, sizeof(header))) return false;
    const uint32_t length = static_cast<uint32_t>(header[0]) |
                            (static_cast<uint32_t>(header[1]) << 8) |
                            (static_cast<uint32_t>(header[2]) << 16) |
                            (static_cast<uint32_t>(header[3]) << 24);
    if (length == 0 || length > kMaxPayload) return false;
    payload->resize(length);
    return ReadExact(payload->data(), length);
}

void WriteMessage(const std::string& payload) {
    const uint32_t length = static_cast<uint32_t>(payload.size());
    uint8_t header[4] = {static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8),
                         static_cast<uint8_t>(length >> 16), static_cast<uint8_t>(length >> 24)};
    WriteExact(header, sizeof(header));
    WriteExact(payload.data(), payload.size());
    std::fflush(stdout);
}

std::string PathOf(const nlohmann::json& json, const char* key) {
    if (!json.contains(key) || !json[key].is_string()) return {};
    return json[key].get<std::string>();
}

class Recognizer {
public:
    ~Recognizer() { Reset(); }

    bool Load(const nlohmann::json& request, std::string* error) {
        Reset();
        const std::string engine = PathOf(request, "engine");
        const auto paths = request.contains("paths") && request["paths"].is_object()
                               ? request["paths"]
                               : nlohmann::json::object();
        const int threads = request.value("threads", 4);
        const float segment = request.value("segment_seconds", 20.0f);
        const float max_segment = request.value("max_segment_seconds", segment);
        language_ = request.value("language", std::string("auto"));
        itn_ = request.value("itn", true);

        SherpaOnnxOfflineRecognizerConfig config;
        std::memset(&config, 0, sizeof(config));
        config.feat_config.sample_rate = kSampleRate;
        config.feat_config.feature_dim = 80;
        config.model_config.num_threads = threads;
        config.model_config.provider = "cpu";
        config.model_config.debug = 0;
        config.decoding_method = "greedy_search";

        if (engine == "funasr-nano") {
            config.model_config.model_type = "funasr_nano";
            config.model_config.funasr_nano.encoder_adaptor =
                CopyString(paths.value("encoder_adaptor", std::string()));
            config.model_config.funasr_nano.llm = CopyString(paths.value("llm", std::string()));
            config.model_config.funasr_nano.embedding =
                CopyString(paths.value("embedding", std::string()));
            config.model_config.funasr_nano.tokenizer =
                CopyString(paths.value("tokenizer", std::string()));
            config.model_config.funasr_nano.max_new_tokens = 256;
            config.model_config.funasr_nano.temperature = 0.0f;
            config.model_config.funasr_nano.top_p = 0.0f;
            config.model_config.funasr_nano.seed = 42;
            config.model_config.funasr_nano.language = CopyString(language_);
            config.model_config.funasr_nano.itn = itn_ ? 1 : 0;
        } else if (engine == "qwen3-asr") {
            config.model_config.model_type = "qwen3_asr";
            config.model_config.qwen3_asr.conv_frontend =
                CopyString(paths.value("conv_frontend", std::string()));
            config.model_config.qwen3_asr.encoder =
                CopyString(paths.value("encoder", std::string()));
            config.model_config.qwen3_asr.decoder =
                CopyString(paths.value("decoder", std::string()));
            config.model_config.qwen3_asr.tokenizer =
                CopyString(paths.value("tokenizer", std::string()));
            config.model_config.qwen3_asr.max_total_len = 512;
            config.model_config.qwen3_asr.max_new_tokens = 256;
            config.model_config.qwen3_asr.temperature = 0.0f;
            config.model_config.qwen3_asr.top_p = 0.0f;
            config.model_config.qwen3_asr.seed = 42;
        } else {
            *error = "unknown engine: " + engine;
            return false;
        }

        recognizer_ = SherpaOnnxCreateOfflineRecognizer(&config);
        if (!recognizer_) {
            *error = "cannot load the model (see the paths and files)";
            return false;
        }

        const std::string vad_path = PathOf(request, "vad");
        if (!vad_path.empty()) {
            SherpaOnnxVadModelConfig vad;
            std::memset(&vad, 0, sizeof(vad));
            vad.silero_vad.model = CopyString(vad_path);
            vad.silero_vad.threshold = 0.5f;
            vad.silero_vad.min_silence_duration = 0.5f;
            vad.silero_vad.min_speech_duration = 0.25f;
            vad.silero_vad.max_speech_duration = segment;
            vad.silero_vad.window_size = kVadWindow;
            vad.sample_rate = kSampleRate;
            vad.num_threads = threads;
            vad.provider = "cpu";
            vad.debug = 0;
            vad_ = SherpaOnnxCreateVoiceActivityDetector(&vad, max_segment + 2.0f);
            if (!vad_) {
                *error = "cannot load the VAD";
                Reset();
                return false;
            }
        }
        engine_ = engine;
        return true;
    }

    bool Transcribe(const std::string& pcm16, std::string* text, std::string* error) {
        if (!recognizer_) {
            *error = "the model is not loaded";
            return false;
        }
        const std::vector<float> samples = ToFloat(pcm16);
        if (samples.empty()) {
            text->clear();
            return true;
        }
        text->clear();
        if (vad_) {
            SherpaOnnxVoiceActivityDetectorReset(vad_);
            std::size_t offset = 0;
            while (offset < samples.size()) {
                const int n = static_cast<int>(
                    (std::min)(samples.size() - offset, static_cast<std::size_t>(kVadWindow)));
                SherpaOnnxVoiceActivityDetectorAcceptWaveform(vad_, samples.data() + offset, n);
                offset += static_cast<std::size_t>(n);
            }
            SherpaOnnxVoiceActivityDetectorFlush(vad_);
            while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
                const SherpaOnnxSpeechSegment* segment =
                    SherpaOnnxVoiceActivityDetectorFront(vad_);
                if (!segment) break;
                std::string piece;
                const bool ok = Decode(segment->samples, static_cast<int>(segment->n), &piece,
                                       error);
                SherpaOnnxDestroySpeechSegment(segment);
                SherpaOnnxVoiceActivityDetectorPop(vad_);
                if (!ok) return false;
                text->append(piece);
            }
            return true;
        }
        return Decode(samples.data(), static_cast<int>(samples.size()), text, error);
    }

    bool Bench(const std::string& pcm16, int repeats, double* ms, std::string* error) {
        const std::vector<float> samples = ToFloat(pcm16);
        if (!recognizer_ || samples.empty()) {
            *error = "nothing to decode";
            return false;
        }
        LARGE_INTEGER frequency, start, end;
        QueryPerformanceFrequency(&frequency);
        for (int i = 0; i < repeats; ++i) {
            std::string text;
            QueryPerformanceCounter(&start);
            if (!Decode(samples.data(), static_cast<int>(samples.size()), &text, error)) {
                return false;
            }
            QueryPerformanceCounter(&end);
            *ms = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 /
                  static_cast<double>(frequency.QuadPart);
        }
        return true;
    }

private:
    static const char* CopyString(const std::string& value) {
        // sherpa-onnx reads the strings during the create call; the vectors below keep them alive.
        static thread_local std::vector<std::string> storage;
        storage.push_back(value);
        return storage.back().c_str();
    }

    static std::vector<float> ToFloat(const std::string& bytes) {
        std::vector<float> samples(bytes.size() / 2);
        const auto* pcm = reinterpret_cast<const int16_t*>(bytes.data());
        for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = pcm[i] / 32768.0f;
        return samples;
    }

    bool Decode(const float* samples, int count, std::string* text, std::string* error) {
        const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(recognizer_);
        if (!stream) {
            *error = "cannot create a stream";
            return false;
        }
        SherpaOnnxAcceptWaveformOffline(stream, kSampleRate, samples, count);
        SherpaOnnxDecodeOfflineStream(recognizer_, stream);
        const SherpaOnnxOfflineRecognizerResult* result =
            SherpaOnnxGetOfflineStreamResult(stream);
        if (!result) {
            SherpaOnnxDestroyOfflineStream(stream);
            *error = "no result";
            return false;
        }
        text->assign(result->text ? result->text : "");
        while (!text->empty() && (text->back() == '\n' || text->back() == ' ')) text->pop_back();
        SherpaOnnxDestroyOfflineRecognizerResult(result);
        SherpaOnnxDestroyOfflineStream(stream);
        return true;
    }

    void Reset() {
        if (vad_) SherpaOnnxDestroyVoiceActivityDetector(vad_);
        if (recognizer_) SherpaOnnxDestroyOfflineRecognizer(recognizer_);
        vad_ = nullptr;
        recognizer_ = nullptr;
    }

    const SherpaOnnxOfflineRecognizer* recognizer_ = nullptr;
    const SherpaOnnxVoiceActivityDetector* vad_ = nullptr;
    std::string engine_;
    std::string language_ = "auto";
    bool itn_ = true;
};

}  // namespace

int main() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    Recognizer recognizer;
    std::string payload;
    while (ReadMessage(&payload)) {
        nlohmann::json request;
        try {
            request = nlohmann::json::parse(payload);
        } catch (const std::exception& e) {
            WriteMessage(nlohmann::json{{"ok", false}, {"error", e.what()}}.dump());
            continue;
        }
        const std::string op = request.value("op", std::string());
        nlohmann::json response{{"ok", true}};
        std::string error;
        if (op == "load") {
            if (!recognizer.Load(request, &error)) {
                response = {{"ok", false}, {"error", error}};
            }
        } else if (op == "transcribe") {
            std::string text;
            const std::string pcm = Base64Decode(request.value("pcm16", std::string()));
            if (!recognizer.Transcribe(pcm, &text, &error)) {
                response = {{"ok", false}, {"error", error}};
            } else {
                response["text"] = text;
            }
        } else if (op == "bench") {
            double ms = 0.0;
            const std::string pcm = Base64Decode(request.value("pcm16", std::string()));
            if (!recognizer.Bench(pcm, request.value("repeats", 3), &ms, &error)) {
                response = {{"ok", false}, {"error", error}};
            } else {
                response["ms"] = ms;
            }
        } else if (op == "shutdown") {
            WriteMessage(nlohmann::json{{"ok", true}}.dump());
            break;
        } else {
            response = {{"ok", false}, {"error", "unknown op: " + op}};
        }
        WriteMessage(response.dump());
    }
    return 0;
}
