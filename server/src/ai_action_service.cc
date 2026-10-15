// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "ai_action_service.h"

#include <cxxime/ai_actions.h>
#include <cxxime/ai_client.h>
#include <cxxime/ipc_protocol.h>

namespace {

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
            std::string error;
            if (!cxxime::ai::run_action(config->ai, action->prompt, text, &answer, &error,
                                        &cancel_)) {
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
