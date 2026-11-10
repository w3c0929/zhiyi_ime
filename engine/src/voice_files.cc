// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
#include <cxxime/voice_files.h>

#include <windows.h>

#include <filesystem>
#include <fstream>

#include <json.hpp>

#include <cxxime/data_path.h>

namespace cxxime {
namespace voice {

namespace {

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// A plain folder name (v<N>): no separators or "..".
bool safe_dir(const std::string& dir) {
    return !dir.empty() && dir.size() <= 32 && dir.find_first_of("\\/:") == std::string::npos &&
           dir.find("..") == std::string::npos;
}

}  // namespace

bool find_installed(InstalledModel* installed) {
    const std::wstring root = wide(local_data_dir()) + wide(kVoiceDir) + L"\\";
    std::ifstream file{std::filesystem::path(root + wide(kVoiceInstalled))};
    const nlohmann::json state = nlohmann::json::parse(file, nullptr, false);
    if (!state.is_object() || !state.contains("dir") || !state["dir"].is_string() ||
        !safe_dir(state["dir"].get<std::string>())) {
        return false;
    }
    InstalledModel found;
    found.dir = root + wide(state["dir"].get<std::string>()) + L"\\";
    found.model = found.dir + wide(kVoiceModelFile);
    found.tokens = found.dir + wide(kVoiceTokensFile);
    found.vad = found.dir + wide(kVoiceVadFile);
    found.version = state.value("version", 0u);
    if (!file_exists(found.model) || !file_exists(found.tokens) || !file_exists(found.vad)) return false;
    if (installed) *installed = std::move(found);
    return true;
}

namespace {

bool dir_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

}  // namespace

std::wstring model_directory(const std::string& dir) {
    if (dir.empty()) return {};
    return wide(local_data_dir()) + wide(kVoiceDir) + L"\\" + wide(kVoiceModelsDir) + L"\\" +
           wide(dir) + L"\\";
}

bool find_model(const std::string& id, InstalledVoiceModel* model) {
    const std::string wanted = id.empty() ? std::string(kVoiceSenseVoiceId) : id;
    InstalledVoiceModel found;
    found.id = wanted;
    if (wanted == kVoiceSenseVoiceId) {
        InstalledModel legacy;
        if (!find_installed(&legacy)) return false;
        found.engine = "sensevoice";
        found.model = legacy.model;
        found.tokens = legacy.tokens;
        found.vad = legacy.vad;
        if (model) *model = std::move(found);
        return true;
    }
    if (wanted == kVoiceFunAsrId) {
        found.dir = model_directory("funasr-nano");
        found.engine = "funasr-nano";
        found.encoder_adaptor = found.dir + L"encoder_adaptor.int8.onnx";
        found.llm = found.dir + L"llm.int8.onnx";
        found.embedding = found.dir + L"embedding.int8.onnx";
        found.tokenizer = found.dir + L"Qwen3-0.6B";
        found.vad = found.dir + wide(kVoiceVadFile);
        if (!file_exists(found.encoder_adaptor) || !file_exists(found.llm) ||
            !file_exists(found.embedding) || !dir_exists(found.tokenizer) ||
            !file_exists(found.vad)) {
            return false;
        }
    } else if (wanted == kVoiceQwen3Id) {
        found.dir = model_directory("qwen3-asr");
        found.engine = "qwen3-asr";
        found.conv_frontend = found.dir + L"conv_frontend.onnx";
        found.encoder = found.dir + L"encoder.int8.onnx";
        found.decoder = found.dir + L"decoder.int8.onnx";
        found.tokenizer = found.dir + L"tokenizer";
        found.vad = found.dir + wide(kVoiceVadFile);
        if (!file_exists(found.conv_frontend) || !file_exists(found.encoder) ||
            !file_exists(found.decoder) || !dir_exists(found.tokenizer) ||
            !file_exists(found.vad)) {
            return false;
        }
    } else {
        return false;
    }
    if (model) *model = std::move(found);
    return true;
}

}  // namespace voice
}  // namespace cxxime
