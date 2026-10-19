// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "translate_service.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include <cxxime/ai_client.h>

namespace {

// What a selection is translated into comes from the settings page (Learning, the row of the
// language the selection is in — Config::gloss_targets); which language that is has to be
// guessed from the text itself.

// How often a whole UTF-8 sequence occurs in `text`.
int count_of(const std::string& text, const char* needle) {
    int count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

// French, German and Spanish share the Latin alphabet: a few characters that belong to one of
// them, and a handful of common words, have to tell them apart. English scores zero and stays
// English.
int latin_score(const std::string& text, const std::vector<std::pair<std::string, int>>& letters,
                const std::vector<std::string>& words) {
    int score = 0;
    for (const auto& [letter, weight] : letters) {
        score += weight * count_of(text, letter.c_str());
    }
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    for (size_t start = 0; start < lower.size();) {
        if (!std::isalpha(static_cast<unsigned char>(lower[start]))) {
            ++start;
            continue;
        }
        size_t end = start;
        while (end < lower.size() && std::isalpha(static_cast<unsigned char>(lower[end]))) ++end;
        if (std::find(words.begin(), words.end(), lower.substr(start, end - start)) != words.end()) {
            ++score;
        }
        start = end;
    }
    return score;
}

std::string detect_latin_language(const std::string& text) {
    // 3 for a character of this language alone, 1 for one it shares with a neighbour. "ü" is
    // German and Spanish, the accents are French and Spanish.
    const std::vector<std::pair<std::string, int>> spanish = {
        {"\xC3\xB1", 3}, {"\xC2\xBF", 3}, {"\xC2\xA1", 3}, {"\xC3\xB3", 1}, {"\xC3\xAD", 1}};
    const std::vector<std::pair<std::string, int>> german = {
        {"\xC3\x9F", 3}, {"\xC3\xA4", 1}, {"\xC3\xB6", 1}, {"\xC3\xBC", 1}};
    const std::vector<std::pair<std::string, int>> french = {
        {"\xC3\xA7", 3}, {"\xC5\x93", 3}, {"\xC3\xA8", 1}, {"\xC3\xA9", 1},
        {"\xC3\xAA", 1}, {"\xC3\xA0", 1}, {"\xC3\xA2", 1}};
    const std::vector<std::string> spanish_words = {"el",  "los", "las", "una",
                                                    "por", "para", "con", "del"};
    const std::vector<std::string> german_words = {"der", "die",  "das", "und", "ist",
                                                   "nicht", "ein", "eine", "mit", "auf"};
    const std::vector<std::string> french_words = {"le",  "les", "des",  "est", "une",
                                                   "pour", "avec", "pas", "dans", "sur"};
    const int es = latin_score(text, spanish, spanish_words);
    const int de = latin_score(text, german, german_words);
    const int fr = latin_score(text, french, french_words);
    const int best = (std::max)(es, (std::max)(de, fr));
    if (best <= 0) return "en";
    if (es == best) return "es";
    if (de == best) return "de";
    return "fr";
}

// The language `text` is written in, out of glossary_languages() (zh en ja ko fr de es ru). The
// scripts decide on their own; the Latin ones fall back to detect_latin_language.
std::string detect_source_language(const std::string& text) {
    bool han = false;
    bool latin = false;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) {
            if (std::isalpha(lead)) latin = true;
            ++i;
            continue;
        }
        uint32_t code_point = 0;
        size_t extra = 0;
        if ((lead & 0xE0) == 0xC0) {
            code_point = lead & 0x1Fu;
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            code_point = lead & 0x0Fu;
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            code_point = lead & 0x07u;
            extra = 3;
        } else {
            ++i;
            continue;
        }
        if (i + extra >= text.size()) break;
        for (size_t k = 0; k < extra; ++k) {
            code_point = (code_point << 6) | (static_cast<unsigned char>(text[i + 1 + k]) & 0x3Fu);
        }
        i += extra + 1;
        if ((code_point >= 0x3040 && code_point <= 0x30FF) ||
            (code_point >= 0xFF66 && code_point <= 0xFF9F)) {
            return "ja";  // kana: Japanese, whatever Han it also uses
        }
        if ((code_point >= 0xAC00 && code_point <= 0xD7AF) ||
            (code_point >= 0x1100 && code_point <= 0x11FF)) {
            return "ko";
        }
        if (code_point >= 0x0400 && code_point <= 0x04FF) return "ru";
        if (code_point >= 0x3000 && code_point <= 0x9FFF) han = true;  // CJK punctuation and Han
        if (code_point >= 0x3400 && code_point <= 0x4DBF) han = true;
        if (code_point >= 0xF900 && code_point <= 0xFAFF) han = true;
    }
    if (han) return "zh";  // Japanese and Korean returned above, so Han alone is Chinese
    return latin ? detect_latin_language(text) : "en";
}

// What a text in `source` is translated into: the learning page's row of that language, and —
// where that row is off, which is also the case with no configuration at all — what this
// function did before the rows existed: Chinese into English, everything else into Chinese.
std::string translation_target(const cxxime::Config& config, const std::string& source) {
    std::string target = config.gloss_target(source);
    if (target.empty() || target == source) target = source == "zh" ? "en" : "zh";
    return target;
}

constexpr int kTranslateTimeoutMs = 30000;

}  // namespace

TranslateService::TranslateService() = default;

TranslateService::~TranslateService() {
    stop();
}

bool TranslateService::start(const std::shared_ptr<const cxxime::Config>& config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
    }
    worker_ = std::thread([this] { run(); });
    return true;
}

void TranslateService::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (server_) {
        server_->stop();
        server_.reset();
    }
}

void TranslateService::update_config(const std::shared_ptr<const cxxime::Config>& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

void TranslateService::request(uint32_t id, const std::string& text,
                              const std::string& chinese_target) {
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chinese_target_ = chinese_target;
        if (text.empty()) {
            phase_ = Phase::kIdle;
            owner_ = id;
            ++generation_;
            text_.clear();
            return;
        }
        phase_ = Phase::kTranslating;
        owner_ = id;
        generation = ++generation_;
        text_.clear();
    }
    post([this, id, generation, text] { do_translate(id, generation, text); });
}

void TranslateService::session_ended(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ == id) {
        phase_ = Phase::kIdle;
        owner_ = 0;
        text_.clear();
    }
}

uint32_t TranslateService::take_status(uint32_t id, std::string* text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ != id) return cxxime::kTranslateIdle;
    if (phase_ == Phase::kDone) {
        if (text) *text = text_;
        phase_ = Phase::kIdle;
        owner_ = 0;
        text_.clear();
        return cxxime::kTranslateDone;
    }
    return phase_ == Phase::kTranslating ? cxxime::kTranslateTranslating : cxxime::kTranslateIdle;
}

// The long-text path (TEXT_RESULT_POLL / TEXT_RESULT_READ): like take_status, but the text stays
// where it is, so the client can read it in pieces. It is dropped when the session ends or the
// next request comes in, which is what request() and session_ended() already do.
uint32_t TranslateService::peek_status(uint32_t id, std::string* text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ != id) return cxxime::kTranslateIdle;
    if (phase_ == Phase::kDone) {
        if (text) *text = text_;
        return cxxime::kTranslateDone;
    }
    return phase_ == Phase::kTranslating ? cxxime::kTranslateTranslating : cxxime::kTranslateIdle;
}

void TranslateService::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
}

void TranslateService::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(5),
                           [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) break;
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
        }
        if (task) {
            task();
            continue;
        }
        // Idle: unload the model after translate.idle_seconds without a request.
        std::shared_ptr<const cxxime::Config> config;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            config = config_;
        }
        if (server_ && config && config->translate_idle_seconds > 0 &&
            std::chrono::steady_clock::now() - last_used_ >
                std::chrono::seconds(config->translate_idle_seconds)) {
            server_->stop();
            server_.reset();
            server_device_.clear();
        }
    }
}

bool TranslateService::ensure_server(std::string* error) {
    if (server_ && server_->running()) return true;

    cxxime::mt::InstalledModel installed;
    if (!cxxime::mt::find_installed(&installed)) {
        if (error) *error = "the translation model is not installed";
        return false;
    }
    std::string device;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_) device = config_->translate_device;
    }
    if (device.empty()) device = cxxime::mt::kCpuDevice;  // "" means the CPU, as elsewhere
    auto server = std::make_unique<cxxime::mt::LlamaServer>();
    if (!server->start(installed, device, error)) return false;
    server_ = std::move(server);
    server_device_ = device;
    return true;
}

void TranslateService::do_translate(uint32_t id, uint64_t generation, std::string text) {
    std::string result;
    std::string error;

    std::shared_ptr<const cxxime::Config> config;
    std::string chinese_target;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = config_;
        chinese_target = chinese_target_;
    }
    const std::string source = detect_source_language(text);
    // The offline model always goes by the learning page's rows — the translate key and the
    // translate action alike (the rows are where the languages and their packs are).
    const std::string offline_target =
        config ? translation_target(*config, source) : (source == "zh" ? "en" : "zh");
    // The API goes by the AI page's translate language instead (Config::ai.translate_language):
    // Chinese into it, anything else into Chinese. The translate action sends the language it was
    // started with; the translate key (Ctrl+C+C) sends none, and gets the same setting.
    std::string online_language = chinese_target;
    if (online_language.empty() && config) online_language = config->ai.translate_language;
    std::string online_target = offline_target;
    if (!online_language.empty()) {
        online_target = source == "zh" ? online_language : std::string("zh");
    }

    // The online AI first, when the user turned it on for the selection translation
    // (Config::ai.use_for.translate). A failure falls back to the offline model.
    if (config && cxxime::ai::usable_for(config->ai, &cxxime::AiUseFor::translate)) {
        std::string translation;
        std::string ai_error;
        if (cxxime::ai::translate(config->ai, text, online_target, &translation, &ai_error,
                                  nullptr) &&
            !translation.empty()) {
            result = std::move(translation);
        }
    }

    if (result.empty() && ensure_server(&error)) {
        last_used_ = std::chrono::steady_clock::now();
        cxxime::mt::Item item;
        item.source = source;
        item.target = offline_target;
        item.text = text;
        const int max_tokens = std::clamp(static_cast<int>(text.size()) * 2, 64, 1024);
        const std::vector<std::string> out =
            server_->translate_document({item}, {}, max_tokens, kTranslateTimeoutMs);
        if (!out.empty()) result = out[0];
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ != id || generation_ != generation || phase_ != Phase::kTranslating) return;
    if (result.empty()) {
        phase_ = Phase::kIdle;  // nothing to show: the client stops polling
    } else {
        text_ = std::move(result);
        phase_ = Phase::kDone;
    }
}
