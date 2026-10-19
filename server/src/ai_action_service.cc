// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "ai_action_service.h"

#include <cxxime/ai_actions.h>
#include <cxxime/ai_client.h>
#include <cxxime/ipc_protocol.h>
#include <cxxime/query_trace.h>

namespace {

// 写进 trace 的失败原因：去掉引号、反斜杠和控制字符，免得把那一行 JSON 弄坏。
std::string json_safe(const std::string& text) {
    std::string out;
    for (const char ch : text) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\') continue;
        out.push_back(byte < 0x20 ? ' ' : ch);
        if (out.size() >= 80) break;
    }
    return out;
}

// What the input box shows when the action could not run. Short and readable: it lands in the
// popup the answer would have used.
std::string action_unavailable_reason(const cxxime::Config& config) {
    if (!config.ai.enabled) return "在线 AI 没有打开（设置 → AI）";
    if (config.ai.keys.empty()) return "没有配置 API 密钥（设置 → AI）";
    if (!cxxime::ai::usable_for(config.ai, &cxxime::AiUseFor::selection)) {
        return "“AI 动作”没有打开（设置 → AI）";
    }
    return {};
}

}  // namespace

AiActionService::AiActionService() = default;

AiActionService::~AiActionService() {
    stop();
}

bool AiActionService::start(const std::shared_ptr<const cxxime::Config>& config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        stopping_ = false;
    }
    worker_ = std::thread([this] { run(); });
    return true;
}

void AiActionService::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cancel_.store(true);
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void AiActionService::update_config(const std::shared_ptr<const cxxime::Config>& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
}

void AiActionService::request(uint32_t id, const std::string& action_id, std::string text) {
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel_.store(true);  // abandon whatever is in flight
        phase_ = Phase::kRunning;
        owner_ = id;
        generation = ++generation_;
        answer_.clear();
        cancel_.store(false);
    }
    post([this, id, generation, action_id, text = std::move(text)] {
        do_action(id, generation, action_id, text);
    });
}

void AiActionService::session_ended(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ != id) return;
    cancel_.store(true);
    phase_ = Phase::kIdle;
    owner_ = 0;
    answer_.clear();
}

uint32_t AiActionService::peek_status(uint32_t id, std::string* answer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ != id) return cxxime::kTranslateIdle;
    if (phase_ == Phase::kDone || phase_ == Phase::kFailed) {
        if (answer) *answer = answer_;
        return phase_ == Phase::kDone ? cxxime::kTranslateDone : cxxime::kTranslateFailed;
    }
    return phase_ == Phase::kRunning ? cxxime::kTranslateTranslating : cxxime::kTranslateIdle;
}

void AiActionService::post(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    wake_.notify_one();
}

void AiActionService::run() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) break;
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
        }
        if (task) task();
    }
}

void AiActionService::do_action(uint32_t id, uint64_t generation, std::string action_id,
                                std::string text) {
    std::string answer;
    std::string reason;
    std::shared_ptr<const cxxime::Config> config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = config_;
    }
    const cxxime::AiAction* action = cxxime::find_ai_action(action_id);
    if (!action) {
        reason = "未知的 AI 动作：" + action_id;
    } else if (!config) {
        reason = "配置还没有读进来";
    } else {
        reason = action_unavailable_reason(*config);
        if (reason.empty() &&
            action->max_input_chars > 0 &&
            cxxime::ai_text_length(text) > static_cast<std::size_t>(action->max_input_chars)) {
            // The client cuts the selection to the limit and says so; counting the same way here
            // (characters, not bytes) means reaching this is a client that did not cut.
            reason = "选中的文字超过这个动作的上限";
        }
        if (reason.empty()) {
            // 「提问」先联网搜一下（docs/ai.md）：开关打开、且所选来源有密钥时，把那句话当查询搜一页
            // 结果，连同结果一起交给模型；搜不到就照旧只回答问题，不因此失败（每次尝试都留一行
            // ai_search 记录）。语音那条路走的是同一个 with_web_search()。
            std::string prompt = action->prompt;
            std::string question = text;
            std::string search_error;
            std::size_t context_chars = 0;
            const bool searched = cxxime::ai::with_web_search(config->ai, action->id, text, &prompt,
                                                              &question, &context_chars,
                                                              &search_error, &cancel_);
            const bool search_wanted = action->id == "ask" && config->ai.search_enabled &&
                                       cxxime::ai::search_available(config->ai);
            if (search_wanted) {
                // 一行记录，用来确认"这次提问到底搜没搜、搜回来多少"（`%USERPROFILE%\zhiyu\logs\
                // server-trace.jsonl`，需要 diagnostics.trace_mode 不是 off）。
                char line[224] = {};
                const int length = snprintf(
                    line, sizeof(line),
                    "{\"event\":\"ai_search\",\"from\":\"ask\",\"provider\":\"%s\",\"ok\":%d,"
                    "\"chars\":%zu,\"error\":\"%s\"}",
                    config->ai.search_provider.c_str(), searched ? 1 : 0, context_chars,
                    json_safe(search_error).c_str());
                if (length > 0) cxxime::enqueue_server_trace_json(line, length);
            }
            std::string error;
            if (!cxxime::ai::run_action(config->ai, prompt, question, &answer, &error, &cancel_)) {
                reason = error.empty() ? "请求失败" : error;
            }
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (owner_ != id || generation_ != generation || phase_ != Phase::kRunning) return;
    if (reason.empty()) {
        answer_ = std::move(answer);
        phase_ = Phase::kDone;
    } else {
        answer_ = std::move(reason);
        phase_ = Phase::kFailed;
    }
}
