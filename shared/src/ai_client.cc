// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/ai_client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <json.hpp>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace cxxime {
namespace ai {
namespace {

// The provider table: the address, the default model and the API shape of each service. Kept
// here rather than in a data file so an installation cannot be left without it.
const std::vector<Provider>& table() {
    static const std::vector<Provider> kProviders = {
        {"deepseek", "DeepSeek", "https://api.deepseek.com/v1", "deepseek-chat",
         Protocol::kOpenAiCompletions},
        {"openai", "OpenAI", "https://api.openai.com/v1", "gpt-4o-mini",
         Protocol::kOpenAiCompletions},
        {"anthropic", "Anthropic Claude", "https://api.anthropic.com", "claude-sonnet-4-5",
         Protocol::kAnthropicMessages},
        {"google", "Google Gemini", "https://generativelanguage.googleapis.com/v1beta",
         "gemini-2.5-pro", Protocol::kGoogleGenerativeAi},
        {"kimi", "Kimi (Moonshot)", "https://api.moonshot.cn/v1", "moonshot-v1-8k",
         Protocol::kOpenAiCompletions},
        {"moonshotai", "Moonshot AI", "https://api.moonshot.ai/v1", "kimi-k2",
         Protocol::kOpenAiCompletions},
        {"glm", "智谱 GLM", "https://open.bigmodel.cn/api/paas/v4", "glm-4-flash",
         Protocol::kOpenAiCompletions},
        {"zai", "Z.ai GLM", "https://api.z.ai/api/coding/paas/v4", "GLM-4.5",
         Protocol::kOpenAiCompletions},
        {"qwen", "通义千问", "https://dashscope.aliyuncs.com/compatible-mode/v1", "qwen-plus",
         Protocol::kOpenAiCompletions},
        {"qwen_token_plan", "通义千问（Token Plan）",
         "https://token-plan.cn-beijing.maas.aliyuncs.com/compatible-mode/v1", "qwen3-max",
         Protocol::kOpenAiCompletions},
        {"siliconflow", "硅基流动", "https://api.siliconflow.cn/v1", "Qwen/Qwen2.5-7B-Instruct",
         Protocol::kOpenAiCompletions},
        {"hunyuan", "腾讯混元", "https://api.hunyuan.cloud.tencent.com/v1", "hunyuan-turbo",
         Protocol::kOpenAiCompletions},
        {"minimax", "MiniMax", "https://api.minimax.io/anthropic", "MiniMax-M2",
         Protocol::kAnthropicMessages},
        {"minimax_cn", "MiniMax（国内）", "https://api.minimaxi.com/anthropic", "MiniMax-M2",
         Protocol::kAnthropicMessages},
        {"kimi_coding", "Kimi Coding", "https://api.kimi.com/coding", "kimi-coding",
         Protocol::kAnthropicMessages},
        {"xiaomi", "小米 MiMo", "https://api.xiaomimimo.com/v1", "MiMo-7B",
         Protocol::kOpenAiCompletions},
        {"xiaomi_token_cn", "小米 Token Plan（国内）", "https://token-plan-cn.xiaomimimo.com/v1",
         "mimo-v2.5", Protocol::kOpenAiCompletions},
        {"xiaomi_token_ams", "小米 Token Plan（AMS）", "https://token-plan-ams.xiaomimimo.com/v1",
         "mimo-v2.5", Protocol::kOpenAiCompletions},
        {"ant_ling", "Ant Ling", "https://api.ant-ling.com/v1", "deepseek-v3.1",
         Protocol::kOpenAiCompletions},
        {"groq", "Groq", "https://api.groq.com/openai/v1", "llama-3.3-70b-versatile",
         Protocol::kOpenAiCompletions},
        {"openrouter", "OpenRouter", "https://openrouter.ai/api/v1", "openai/gpt-4o",
         Protocol::kOpenAiCompletions},
        {"together", "Together AI", "https://api.together.ai/v1",
         "meta-llama/Llama-3.3-70B-Instruct-Turbo", Protocol::kOpenAiCompletions},
        {"cerebras", "Cerebras", "https://api.cerebras.ai/v1", "llama-3.3-70b",
         Protocol::kOpenAiCompletions},
        {"fireworks", "Fireworks AI", "https://api.fireworks.ai/inference",
         "accounts/fireworks/models/llama-v3p1-70b-instruct", Protocol::kOpenAiCompletions},
        {"nvidia", "NVIDIA NIM", "https://integrate.api.nvidia.com/v1",
         "meta/llama-3.3-70b-instruct", Protocol::kOpenAiCompletions},
        {"huggingface", "Hugging Face", "https://router.huggingface.co/v1",
         "meta-llama/Llama-3.3-70B-Instruct", Protocol::kOpenAiCompletions},
        {"xai", "xAI Grok", "https://api.x.ai/v1", "grok-4", Protocol::kOpenAiCompletions},
        {"mistral", "Mistral", "https://api.mistral.ai", "mistral-large-latest",
         Protocol::kMistralConversations},
        {"baseten", "Baseten", "https://inference.baseten.co/v1",
         "deepseek-ai/DeepSeek-V4-Flash-0731", Protocol::kOpenAiCompletions},
        {"vercel", "Vercel AI Gateway", "https://ai-gateway.vercel.sh",
         "anthropic/claude-sonnet-4.5", Protocol::kAnthropicMessages},
        {"opencode_go", "OpenCode Go", "https://opencode.ai/zen/go/v1", "deepseek-v4-flash",
         Protocol::kOpenAiCompletions, "x-opencode-session"},
        {"opencode", "OpenCode Zen", "https://opencode.ai/zen/v1", "deepseek-v4-flash",
         Protocol::kOpenAiCompletions},
        {"ollama", "Ollama（本机）", "http://localhost:11434/v1", "qwen2.5:7b",
         Protocol::kOpenAiCompletions},
        {"custom", "自定义（OpenAI 兼容）", "", "", Protocol::kOpenAiCompletions},
    };
    return kProviders;
}

thread_local std::string g_last_error;

std::string trim_slashes(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

// ---- base64 (Config::ai.keys holds DPAPI output in base64) ------------------------------------

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 2 < bytes.size()) {
        const std::uint32_t value = (static_cast<unsigned char>(bytes[i]) << 16) |
                                    (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                                    static_cast<unsigned char>(bytes[i + 2]);
        out.push_back(kBase64[(value >> 18) & 0x3F]);
        out.push_back(kBase64[(value >> 12) & 0x3F]);
        out.push_back(kBase64[(value >> 6) & 0x3F]);
        out.push_back(kBase64[value & 0x3F]);
        i += 3;
    }
    const std::size_t left = bytes.size() - i;
    if (left == 1) {
        const std::uint32_t value = static_cast<unsigned char>(bytes[i]) << 16;
        out.push_back(kBase64[(value >> 18) & 0x3F]);
        out.push_back(kBase64[(value >> 12) & 0x3F]);
        out.append("==");
    } else if (left == 2) {
        const std::uint32_t value = (static_cast<unsigned char>(bytes[i]) << 16) |
                                    (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out.push_back(kBase64[(value >> 18) & 0x3F]);
        out.push_back(kBase64[(value >> 12) & 0x3F]);
        out.push_back(kBase64[(value >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool base64_decode(const std::string& text, std::string* bytes) {
    int value = 0;
    int bits = 0;
    bytes->clear();
    for (const char ch : text) {
        if (ch == '=' || ch == '\r' || ch == '\n') continue;
        const char* found = std::strchr(kBase64, ch);
        if (!found) return false;
        value = (value << 6) | static_cast<int>(found - kBase64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes->push_back(static_cast<char>((value >> bits) & 0xFF));
        }
    }
    return true;
}

// A fixed entropy string: the ciphertext only decrypts on this machine, for this user, by us.
const BYTE kEntropy[] = {'z', 'h', 'i', 'y', 'u', 'i', 'm', 'e', 'a', 'i'};

// ---- HTTP ------------------------------------------------------------------------------------

bool http_request(const wchar_t* method, const std::wstring& url,
                  const std::vector<std::wstring>& headers, const std::string* body,
                  int timeout_ms, std::string* response, std::string* error) {
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts)) {
        *error = "invalid address";
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    HINTERNET session = WinHttpOpen(L"ZhiyuIME/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        *error = "cannot reach the network";
        return false;
    }
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    HINTERNET request = connect
                            ? WinHttpOpenRequest(connect, method, path.c_str(), nullptr,
                                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 parts.nScheme == INTERNET_SCHEME_HTTPS
                                                     ? WINHTTP_FLAG_SECURE
                                                     : 0)
                            : nullptr;
    if (request) {
        for (const std::wstring& header : headers) {
            WinHttpAddRequestHeaders(request, header.c_str(), static_cast<DWORD>(-1),
                                     WINHTTP_ADDREQ_FLAG_ADD);
        }
        if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               body ? const_cast<char*>(body->data()) : nullptr,
                               body ? static_cast<DWORD>(body->size()) : 0,
                               body ? static_cast<DWORD>(body->size()) : 0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD status = 0;
            DWORD size = sizeof(status);
            WinHttpQueryHeaders(request,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                WINHTTP_NO_HEADER_INDEX);
            response->clear();
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                std::string chunk(available, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, &chunk[0], available, &read) || read == 0) break;
                chunk.resize(read);
                response->append(chunk);
            }
            if (status >= 200 && status < 300) {
                ok = true;
            } else if (status == 401 || status == 403) {
                *error = "the API key was refused (HTTP " + std::to_string(status) + ")";
            } else if (status == 404) {
                *error = "the address or the model was not found (HTTP 404)";
            } else if (status == 429) {
                *error = "the service is rate limiting (HTTP 429)";
            } else {
                *error = "the service answered HTTP " + std::to_string(status);
            }
        } else {
            *error = "the request failed (network or timeout)";
        }
    } else {
        *error = "cannot reach the service";
    }
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// ---- request bodies and responses ------------------------------------------------------------

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                          nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &wide[0], count);
    return wide;
}

std::string read_text_part(const nlohmann::json& item) {
    if (item.is_object() && item.contains("text") && item["text"].is_string()) {
        return item["text"].get<std::string>();
    }
    return {};
}

// The user prompt an API gets, per shape, plus its answer.
struct Request {
    std::wstring url;
    std::vector<std::wstring> headers;
    std::string body;
};

// Some services route by an extra header whose value is ours to choose (OpenCode Go); one id
// per process is enough to look like a single session.
const std::string& session_id() {
    static const std::string kSession = [] {
        return "zhiyu-" + std::to_string(GetCurrentProcessId()) + "-" +
               std::to_string(GetTickCount64());
    }();
    return kSession;
}

// How a service wants to be told about thinking (SuMail's detectCompat + applyThinkingCompat).
// A provider whose name or address is not one of these uses the standard reasoning_effort.
std::string thinking_format_for(const Provider& provider, const std::string& base) {
    std::string url = base;
    for (char& c : url) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    const std::string id = provider.id;
    const auto has = [&](const char* text) { return url.find(text) != std::string::npos; };
    if (id == "deepseek" || has("deepseek.com")) return "deepseek";
    if (id == "zai" || has("api.z.ai") || has("open.bigmodel.cn")) return "zai";
    if (id == "qwen" || has("dashscope")) return "qwen";
    if (id == "openrouter" || has("openrouter.ai")) return "openrouter";
    if (id == "together" || has("api.together.")) return "together";
    if (id == "ant_ling" || has("api.ant-ling.com")) return "ant-ling";
    return {};
}

// Writes the thinking switch and the level the way this service expects. Thinking is on for most
// of them unless it is asked off, and each family asks differently:
//   deepseek, zai  thinking: {type: enabled|disabled} (+ reasoning_effort when on; zai keeps the
//                  reasoning of the previous turn)
//   qwen           enable_thinking: true|false
//   chat template  chat_template_kwargs: {enable_thinking, preserve_thinking}
//   otherwise      reasoning_effort: <level>, or "none" to turn it off
void apply_thinking(nlohmann::json* body, const std::string& format, bool enabled,
                    const std::string& effort) {
    const std::string level = effort.empty() ? std::string("high") : effort;
    if (format == "deepseek" || format == "zai") {
        nlohmann::json thinking = {{"type", enabled ? "enabled" : "disabled"}};
        if (enabled && format == "zai") thinking["clear_thinking"] = false;
        (*body)["thinking"] = thinking;
        if (enabled) (*body)["reasoning_effort"] = level;
        return;
    }
    if (format == "qwen") {
        (*body)["enable_thinking"] = enabled;
        return;
    }
    if (format == "qwen-chat-template") {
        (*body)["chat_template_kwargs"] = {{"enable_thinking", enabled},
                                           {"preserve_thinking", true}};
        return;
    }
    if (format == "chat-template") {
        (*body)["chat_template_kwargs"] = {{"enable_thinking", enabled}};
        return;
    }
    (*body)["reasoning_effort"] = enabled ? level : std::string("none");
}
bool build_request(const AiConfig& config, const Provider& provider,
                   const std::string& system_prompt, const std::string& user_prompt,
                   const std::string& key, bool send_thinking_off, Request* request) {
    const std::string base = trim_slashes(effective_base_url(config));
    const std::string model = effective_model(config);
    if (base.empty() || model.empty()) {
        g_last_error = "a service address and a model are needed";
        return false;
    }
    const std::wstring wide_key = utf8_to_wide(key);
    const std::wstring extra =
        (provider.extra_header && *provider.extra_header)
            ? utf8_to_wide(std::string(provider.extra_header) + ": " + session_id())
            : std::wstring();

    switch (provider.protocol) {
    case Protocol::kOpenAiCompletions:
    case Protocol::kMistralConversations: {
        const std::string path =
            provider.protocol == Protocol::kMistralConversations ? "/v1/chat/completions"
                                                                 : "/chat/completions";
        request->url = utf8_to_wide(base + path);
        request->headers.push_back(L"Content-Type: application/json");
        if (!extra.empty()) request->headers.push_back(extra);
        if (!wide_key.empty()) request->headers.push_back(L"Authorization: Bearer " + wide_key);
        nlohmann::json body = {{"model", model},
                               {"messages",
                                {{{"role", "system"}, {"content", system_prompt}},
                                 {{"role", "user"}, {"content", user_prompt}}}},

                               {"stream", false}};
        if (!send_thinking_off) apply_thinking(&body, thinking_format_for(provider, base), config.thinking, config.reasoning_effort);
        else if (config.thinking && !config.reasoning_effort.empty()) {
            body["reasoning_effort"] = config.reasoning_effort;
        }
        request->body = body.dump();
        return true;
    }
    case Protocol::kAnthropicMessages: {
        request->url = utf8_to_wide(base + "/v1/messages");
        request->headers.push_back(L"Content-Type: application/json");
        if (!extra.empty()) request->headers.push_back(extra);
        request->headers.push_back(L"anthropic-version: 2023-06-01");
        if (!wide_key.empty()) request->headers.push_back(L"x-api-key: " + wide_key);
        nlohmann::json body = {
            {"model", model},
            {"max_tokens", 2048},
            {"system", system_prompt},
            {"messages", {{{"role", "user"}, {"content", user_prompt}}}},
            };
        request->body = body.dump();
        return true;
    }
    case Protocol::kGoogleGenerativeAi: {
        request->url = utf8_to_wide(base + "/models/" + model + ":generateContent");
        if (!wide_key.empty()) {
            request->headers.push_back(L"x-goog-api-key: " + wide_key);
        }
        request->headers.push_back(L"Content-Type: application/json");
        if (!extra.empty()) request->headers.push_back(extra);
        nlohmann::json body = {
            {"systemInstruction", {{"parts", {{{"text", system_prompt}}}}}},
            {"contents", {{{"role", "user"}, {"parts", {{{"text", user_prompt}}}}}}},
            };
        request->body = body.dump();
        return true;
    }
    }
    g_last_error = "unsupported service type";
    return false;
}

bool parse_response(Protocol protocol, const std::string& body, std::string* text) {
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body);
    } catch (const std::exception&) {
        g_last_error = "the service answered something unexpected";
        return false;
    }
    try {
        switch (protocol) {
        case Protocol::kOpenAiCompletions:
        case Protocol::kMistralConversations: {
            if (!json.contains("choices") || !json["choices"].is_array() ||
                json["choices"].empty()) {
                break;
            }
            const auto& message = json["choices"][0]["message"];
            if (message.contains("content") && message["content"].is_string()) {
                *text = message["content"].get<std::string>();
                return !text->empty();
            }
            break;
        }
        case Protocol::kAnthropicMessages: {
            if (!json.contains("content") || !json["content"].is_array()) break;
            text->clear();
            for (const auto& item : json["content"]) text->append(read_text_part(item));
            return !text->empty();
        }
        case Protocol::kGoogleGenerativeAi: {
            if (!json.contains("candidates") || !json["candidates"].is_array() ||
                json["candidates"].empty()) {
                break;
            }
            const auto& parts = json["candidates"][0]["content"]["parts"];
            if (!parts.is_array()) break;
            text->clear();
            for (const auto& part : parts) text->append(read_text_part(part));
            return !text->empty();
        }
        }
    } catch (const std::exception&) {
        g_last_error = "the service answered something unexpected";
        return false;
    }
    // An error object is the usual reason: surface its message.
    if (json.contains("error")) {
        const auto& error = json["error"];
        if (error.is_object() && error.contains("message") && error["message"].is_string()) {
            g_last_error = error["message"].get<std::string>();
            return false;
        }
        if (error.is_string()) {
            g_last_error = error.get<std::string>();
            return false;
        }
    }
    g_last_error = "the service answered without a translation";
    return false;
}

}  // namespace

const std::vector<Provider>& providers() { return table(); }

const Provider* find_provider(const std::string& id) {
    for (const Provider& provider : table()) {
        if (id == provider.id) return &provider;
    }
    return nullptr;
}

const Provider& selected_provider(const AiConfig& config) {
    // An id that is not in the table: the last entry ("custom"), whose address and model are
    // the user's — an empty one is then reported as such instead of silently using another
    // service.
    const Provider* provider = find_provider(config.provider);
    return provider ? *provider : table().back();
}

const char* protocol_name(Protocol protocol) {
    switch (protocol) {
    case Protocol::kOpenAiCompletions: return "openai-completions";
    case Protocol::kAnthropicMessages: return "anthropic-messages";
    case Protocol::kGoogleGenerativeAi: return "google-generative-ai";
    case Protocol::kMistralConversations: return "mistral-conversations";
    }
    return "openai-completions";
}

bool parse_protocol(const std::string& name, Protocol* protocol) {
    for (const Protocol candidate :
         {Protocol::kOpenAiCompletions, Protocol::kAnthropicMessages,
          Protocol::kGoogleGenerativeAi, Protocol::kMistralConversations}) {
        if (name == protocol_name(candidate)) {
            *protocol = candidate;
            return true;
        }
    }
    return false;
}

std::string effective_base_url(const AiConfig& config) {
    if (!config.base_url.empty()) return config.base_url;
    const Provider* provider = find_provider(config.provider);
    return provider ? provider->base_url : std::string();
}

std::string effective_model(const AiConfig& config) {
    if (!config.model.empty()) return config.model;
    const Provider* provider = find_provider(config.provider);
    return provider ? provider->model : std::string();
}

std::string protect_secret(const std::string& plain) {
    if (plain.empty()) return {};
    DATA_BLOB input = {};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    input.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB entropy = {};
    entropy.pbData = const_cast<BYTE*>(kEntropy);
    entropy.cbData = sizeof(kEntropy);
    DATA_BLOB output = {};
    if (!CryptProtectData(&input, L"ZhiyuIME", &entropy, nullptr, nullptr, 0, &output)) {
        return {};
    }
    const std::string encoded =
        base64_encode(std::string(reinterpret_cast<char*>(output.pbData), output.cbData));
    LocalFree(output.pbData);
    return encoded;
}

bool unprotect_secret(const std::string& protected_base64, std::string* plain) {
    std::string bytes;
    if (!plain || !base64_decode(protected_base64, &bytes) || bytes.empty()) return false;
    DATA_BLOB input = {};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data()));
    input.cbData = static_cast<DWORD>(bytes.size());
    DATA_BLOB entropy = {};
    entropy.pbData = const_cast<BYTE*>(kEntropy);
    entropy.cbData = sizeof(kEntropy);
    DATA_BLOB output = {};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, 0, &output)) {
        return false;
    }
    plain->assign(reinterpret_cast<char*>(output.pbData), output.cbData);
    LocalFree(output.pbData);
    return true;
}

bool usable_for(const AiConfig& config, bool AiUseFor::*use) {
    return config.uses(use) && !config.keys.empty();
}

std::string last_error() { return g_last_error; }

bool complete(const AiConfig& config, const std::string& system_prompt,
              const std::string& user_prompt, std::string* text, std::string* error,
              const std::atomic<bool>* cancel) {
    g_last_error.clear();
    if (!text) return false;
    text->clear();
    if (config.keys.empty()) {
        g_last_error = "no API key is configured";
        if (error) *error = g_last_error;
        return false;
    }
    const Provider& provider = selected_provider(config);

    // One try per key: a refused key (401/429) is not worth retrying with itself.
    for (const std::string& stored : config.keys) {
        if (cancel && cancel->load()) {
            g_last_error = "cancelled";
            if (error) *error = g_last_error;
            return false;
        }
        std::string key;
        if (!unprotect_secret(stored, &key) || key.empty()) continue;
        Request request;
        if (!build_request(config, provider, system_prompt, user_prompt, key, false, &request)) {
            if (error) *error = g_last_error;
            return false;
        }
        std::string body;
        std::string failure;
        if (!http_request(L"POST", request.url, request.headers, &request.body,
                          (std::max)(1000, config.timeout_ms), &body, &failure)) {
            g_last_error = failure;
            continue;  // the next key may still work
        }
        if (parse_response(provider.protocol, body, text)) {
            if (error) error->clear();
            return true;
        }
        // Turning thinking off means sending a value some services do not know: ask again without
        // it rather than fail the translation.
        if (!config.thinking &&
            build_request(config, provider, system_prompt, user_prompt, key, true, &request) &&
            http_request(L"POST", request.url, request.headers, &request.body,
                         (std::max)(1000, config.timeout_ms), &body, &failure) &&
            parse_response(provider.protocol, body, text)) {
            if (error) error->clear();
            return true;
        }
        continue;
    }
    if (error) *error = g_last_error.empty() ? "the request failed" : g_last_error;
    return false;
}

bool translate(const AiConfig& config, const std::string& text, const std::string& target,
               std::string* translation, std::string* error, const std::atomic<bool>* cancel) {
    const bool to_english = target == "en";
    const std::string system =
        to_english
            ? "You translate into English. Output only the translation, without quotes, notes "
              "or the original text."
            : "You translate into Chinese. Output only the translation, without quotes, notes "
              "or the original text.";
    return complete(config, system, text, translation, error, cancel);
}

bool list_models(const AiConfig& config, std::vector<std::string>* models, std::string* error,
                 const std::atomic<bool>* cancel) {
    g_last_error.clear();
    if (!models) return false;
    models->clear();
    if (config.keys.empty()) {
        g_last_error = "no API key is configured";
        if (error) *error = g_last_error;
        return false;
    }
    const Provider& provider = selected_provider(config);
    const std::string base = trim_slashes(effective_base_url(config));
    if (base.empty()) {
        g_last_error = "a service address is needed";
        if (error) *error = g_last_error;
        return false;
    }
    // Ollama has no /models: its own list lives at /api/tags (below the /v1 the chat call uses).
    const bool ollama =
        std::string(provider.id) == "ollama" || base.find(":11434") != std::string::npos;
    std::string url;
    if (ollama) {
        std::string root = base;
        if (root.size() > 3 && root.compare(root.size() - 3, 3, "/v1") == 0) {
            root.erase(root.size() - 3);
        }
        url = root + "/api/tags";
    } else if (provider.protocol == Protocol::kAnthropicMessages) {
        url = base + "/v1/models";
    } else {
        url = base + "/models";  // OpenAI-compatible, Mistral and Google alike
    }
    const std::wstring wide_url = utf8_to_wide(url);

    for (const std::string& stored : config.keys) {
        if (cancel && cancel->load()) {
            g_last_error = "cancelled";
            if (error) *error = g_last_error;
            return false;
        }
        std::string key;
        if (!unprotect_secret(stored, &key) || key.empty()) continue;
        const std::wstring wide_key = utf8_to_wide(key);
        std::vector<std::wstring> headers;
        headers.push_back(L"Accept: application/json");
        switch (provider.protocol) {
        case Protocol::kAnthropicMessages:
            headers.push_back(L"anthropic-version: 2023-06-01");
            headers.push_back(L"x-api-key: " + wide_key);
            break;
        case Protocol::kGoogleGenerativeAi:
            headers.push_back(L"x-goog-api-key: " + wide_key);
            break;
        default:
            if (!wide_key.empty()) headers.push_back(L"Authorization: Bearer " + wide_key);
            break;
        }

        std::string body;
        std::string failure;
        if (!http_request(L"GET", wide_url, headers, nullptr, (std::max)(1000, config.timeout_ms),
                          &body, &failure)) {
            g_last_error = failure;
            continue;
        }
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(body);
        } catch (const std::exception&) {
            g_last_error = "the service answered something unexpected";
            continue;
        }
        // OpenAI-compatible, Anthropic and Ollama answer "data" or "models"; Google's ids carry
        // a "models/" prefix.
        for (const char* key_name : {"data", "models"}) {
            if (!json.contains(key_name) || !json[key_name].is_array()) continue;
            for (const auto& item : json[key_name]) {
                std::string id;
                if (item.is_object()) {
                    if (item.contains("id") && item["id"].is_string()) {
                        id = item["id"].get<std::string>();
                    } else if (item.contains("name") && item["name"].is_string()) {
                        id = item["name"].get<std::string>();
                    }
                }
                constexpr const char* kPrefix = "models/";
                if (id.compare(0, 7, kPrefix) == 0) id.erase(0, 7);
                if (!id.empty()) models->push_back(id);
            }
        }
        if (!models->empty()) {
            std::sort(models->begin(), models->end());
            models->erase(std::unique(models->begin(), models->end()), models->end());
            if (error) error->clear();
            return true;
        }
        g_last_error = "the service answered without a model list";
    }
    if (error) *error = g_last_error.empty() ? "the request failed" : g_last_error;
    return false;
}

}  // namespace ai
}  // namespace cxxime
