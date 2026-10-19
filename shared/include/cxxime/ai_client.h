// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The online AI client: a provider table (the address, default model and API shape of the
// mainstream services), the request/response of each API shape, and one blocking call that
// runs on a worker thread. Windows' WinHTTP does the network, so nothing new is linked in.
#ifndef CXXIME_AI_CLIENT_H_
#define CXXIME_AI_CLIENT_H_

#include <atomic>
#include <string>
#include <vector>

#include <cxxime/ai_config.h>

namespace cxxime {
namespace ai {

// How a service is spoken to (the built-in table below says which one a provider uses).
enum class Protocol {
    kOpenAiCompletions,    // POST {base}/chat/completions — deepseek, openai, kimi, ollama, ...
    kAnthropicMessages,    // POST {base}/v1/messages — claude, minimax, kimi-coding, ...
    kGoogleGenerativeAi,   // POST {base}/models/{model}:generateContent
    kMistralConversations  // POST {base}/chat/completions — Mistral's OpenAI-compatible route
};

struct Provider {
    const char* id;
    const char* name;      // shown in the settings list
    const char* base_url;
    const char* model;
    Protocol protocol;
    // Some services route by an extra header (OpenCode Go wants "x-opencode-session").
    // nullptr when the service needs none.
    const char* extra_header = nullptr;
};

const Provider* find_provider(const std::string& id);
const std::vector<Provider>& providers();
const char* protocol_name(Protocol protocol);
bool parse_protocol(const std::string& name, Protocol* protocol);

// The address and the model of `config`, with the chosen provider's own filled in.
std::string effective_base_url(const AiConfig& config);
std::string effective_model(const AiConfig& config);

// Config::ai.keys holds DPAPI-protected (current user) base64 strings, never the key itself.
std::string protect_secret(const std::string& plain);
bool unprotect_secret(const std::string& protected_base64, std::string* plain);

// True when the API may be used for a feature: turned on, that feature asks for it, and at
// least one key is configured. `use` is e.g. &AiUseFor::translate.
bool usable_for(const AiConfig& config, bool AiUseFor::*use);

// The provider a configuration asks for: its own when it names one, else the first entry.
const Provider& selected_provider(const AiConfig& config);

// The reasoning levels this service and model offer (SuMail's provider+model table, with
// its per-provider list as the fallback). Empty: the service offers none -- send no level.
std::vector<std::string> thinking_levels(const std::string& provider, const std::string& model);

// One chat request. Blocking: run it on a worker thread. `cancel` is polled while it waits.
// False with *error set when nothing usable came back.
bool complete(const AiConfig& config, const std::string& system_prompt,
              const std::string& user_prompt, std::string* text, std::string* error,
              const std::atomic<bool>* cancel);

// The model ids the service offers (GET {base}/models; Ollama /api/tags; Anthropic
// /v1/models; Google /models). Blocking, like complete(); one try per key.
bool list_models(const AiConfig& config, std::vector<std::string>* models, std::string* error,
                 const std::atomic<bool>* cancel);

// "translate `text` into `target`" ("zh" / "en" / ...): a prompt plus complete().
bool translate(const AiConfig& config, const std::string& text, const std::string& target,
               std::string* translation, std::string* error, const std::atomic<bool>* cancel);

// An AI action on selected text (ai_actions.h): the action's prompt as the system message and the
// text as the user message, through complete(). The answer is trimmed and stripped of the
// wrapping quotes, the code fences and the "以下是...：" line models like to add; an answer that
// holds nothing else comes back empty.
bool run_action(const AiConfig& config, const std::string& system_prompt, const std::string& text,
                std::string* answer, std::string* error, const std::atomic<bool>* cancel);

// Whether a search can run at all: the service of Config::ai.search_provider has a key — its own,
// or (DeepSeek) the setup's DeepSeek API key.
bool search_available(const AiConfig& config);

// 「提问」在发请求前先联网搜一下（docs/ai.md）——选中的文字与语音识别后处理两条路共用这一个：
// `action_id` 是 "ask"、搜索开关打开、且所选来源有密钥时，把 `question` 当查询搜一页结果，
// `prompt` 与 `message` 换成"带搜索结果"的那一份（message 变成"结果 + 问题"，prompt 追加一句
// 要求依据结果回答并列出来源）。返回是否真的搜到了：false 时两者原样不动（照旧直接回答），
// `context_chars` 是结果的字节数，`error` 是没搜成的原因。
bool with_web_search(const AiConfig& config, const std::string& action_id,
                     const std::string& question, std::string* prompt, std::string* message,
                     std::size_t* context_chars, std::string* error,
                     const std::atomic<bool>* cancel);

// A web search (docs/ai.md): `query` goes to the service Config::ai.search_provider names — Tavily
// (https://api.tavily.com/search) or DeepSeek's own search (https://api.deepseek.com/v1/tools/
// search, on the DeepSeek API key of this setup unless the search has one of its own) — and the
// results come back as the context handed to the model: numbered, each with its title, address and
// a snippet. False with *error set when no key is configured or the service refused.
bool web_search(const AiConfig& config, const std::string& query, std::string* context,
                std::string* error, const std::atomic<bool>* cancel);

// The last error of the most recent call on this thread (for the settings page's test button).
std::string last_error();

}  // namespace ai
}  // namespace cxxime

#endif  // CXXIME_AI_CLIENT_H_
