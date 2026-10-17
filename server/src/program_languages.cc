// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "program_languages.h"

#include <chrono>
#include <fstream>
#include <system_error>

#include <windows.h>

#include <json.hpp>

#include <cxxime/data_path.h>

namespace cxxime {
namespace server {

namespace {

constexpr int kFormat = 1;

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                        static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), &result[0], size);
    return result;
}

long long now_seconds() {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());
}

std::wstring file_path() {
    return utf8_to_wide(user_data_path("program-languages.json"));
}

}  // namespace

void ProgramLanguages::load() {
    std::ifstream file(file_path());
    if (!file.is_open()) return;
    nlohmann::json root;
    try {
        file >> root;
    } catch (const nlohmann::json::exception&) {
        return;  // damaged: an empty memory
    }
    if (!root.is_object() || root.value("format", 0) != kFormat ||
        !root.contains("programs") || !root["programs"].is_object()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    programs_.clear();
    for (auto& [program, value] : root["programs"].items()) {
        if (!value.is_object()) continue;
        Entry entry;
        entry.language = value.value("language", std::string{});
        entry.last = value.value("last", 0LL);
        if (entry.language != "chinese" && entry.language != "english") continue;
        programs_[program] = entry;
    }
}

std::string ProgramLanguages::lookup(const std::string& program) const {
    if (program.empty()) return {};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = programs_.find(program);
    return found == programs_.end() ? std::string{} : found->second.language;
}

void ProgramLanguages::remember(const std::string& program, const std::string& language) {
    if (program.empty() || (language != "chinese" && language != "english")) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = programs_.find(program);
    if (found != programs_.end() && found->second.language == language) {
        found->second.last = now_seconds();  // no write: only which one is recent changed
        return;
    }
    programs_[program] = Entry{language, now_seconds()};
    while (programs_.size() > kMaxPrograms) {
        auto oldest = programs_.begin();
        for (auto it = programs_.begin(); it != programs_.end(); ++it) {
            if (it->second.last < oldest->second.last) oldest = it;
        }
        programs_.erase(oldest);
    }
    write_locked();
}

void ProgramLanguages::write_locked() const {
    nlohmann::json root;
    root["format"] = kFormat;
    for (const auto& [program, entry] : programs_) {
        root["programs"][program] = {{"language", entry.language}, {"last", entry.last}};
    }
    // UTF-8 without a BOM (data/README.md), written aside and moved over the old one so a crash
    // halfway through cannot leave a half file behind.
    const std::string text = root.dump(2) + "\n";
    const std::wstring path = file_path();
    const std::wstring temporary = path + L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out.good()) {
            out.close();
            DeleteFileW(temporary.c_str());
            return;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
    }
}

}  // namespace server
}  // namespace cxxime
