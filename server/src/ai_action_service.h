// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The AI actions on selected text (docs/ai.md, ai_actions.h): zhiyu-server runs the action the
// client asked for on the text it uploaded (TEXT_UPLOAD_*, ipc_protocol.h) and keeps the answer
// for the client to read in pieces (TEXT_RESULT_*). One request at a time, for the session that
// asked; another request replaces it.
//
// The answer is asked of the online AI only (ai_client.h): with the AI off, or without a key, the
// action fails with a reason the input box can show. The call blocks for seconds, so it runs on a
// thread of its own, never on the IPC thread.
#ifndef CXXIME_SERVER_AI_ACTION_SERVICE_H_
#define CXXIME_SERVER_AI_ACTION_SERVICE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <cxxime/config.h>

class AiActionService {
public:
    AiActionService();
    ~AiActionService();

    bool start(const std::shared_ptr<const cxxime::Config>& config);
    void stop();
    void update_config(const std::shared_ptr<const cxxime::Config>& config);

    // Session `id` asked for `action_id` (ai_actions.h) on `text` (UTF-8).
    void request(uint32_t id, const std::string& action_id, std::string text);
    // The session ended or lost the focus: drop its request and stop the call in flight.
    void session_ended(uint32_t id);

    // kTranslate* (ipc_protocol.h) for session `id`, with the answer kept for the next call so the
    // client can read it in pieces; kTranslateFailed when it could not be produced, in which case
    // `answer` is the reason to show.
    uint32_t peek_status(uint32_t id, std::string* answer);

private:
    enum class Phase { kIdle, kRunning, kDone, kFailed };

    void post(std::function<void()> task);
    void run();
    void do_action(uint32_t id, uint64_t generation, std::string action_id, std::string text);

    std::mutex mutex_;
    Phase phase_ = Phase::kIdle;
    uint32_t owner_ = 0;
    uint64_t generation_ = 0;
    std::string answer_;  // the answer, or the reason it failed
    std::shared_ptr<const cxxime::Config> config_;
    std::atomic<bool> cancel_{false};

    // The worker's own.
    std::thread worker_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;
};

#endif  // CXXIME_SERVER_AI_ACTION_SERVICE_H_
