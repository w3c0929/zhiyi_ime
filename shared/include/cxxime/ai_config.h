// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Online AI (Config::ai): the input method can send the text it works on to an API
// (OpenAI-compatible, Anthropic Messages or Google Generative AI) instead of the offline
// models. Off by default — nothing leaves the machine until the user turns it on — and each
// feature that may use it has its own switch, so "use the API for the selection translation"
// does not imply "use it for the voice input".
#ifndef CXXIME_AI_CONFIG_H_
#define CXXIME_AI_CONFIG_H_

#include <string>
#include <vector>

namespace cxxime {

// The features that may run on the API (Config::ai.use_for).
struct AiUseFor {
    bool translate = false;  // selection translation (Ctrl+C+C)
    bool selection = false;  // AI actions on the selected text
    bool english = false;    // English completion / rewriting
    bool voice = false;      // voice input post-processing
    bool glossary = false;   // language packs the offline model cannot cover

    // Any of them on: the settings page shows the API as in use.
    bool any() const {
        return translate || selection || english || voice || glossary;
    }
};

struct AiConfig {
    bool enabled = false;
    // "prefer_remote": run on the API, fall back to the offline model when it fails.
    // "remote_only": never fall back (a failure then shows nothing).
    std::string mode = "prefer_remote";
    std::string provider = "deepseek";  // an id of the built-in provider table (ai_client.h)
    std::string base_url;               // empty: the provider's own
    std::string model;                  // empty: the provider's own
    double temperature = 0.7;
    std::string reasoning_effort;  // "" = do not send one
    int timeout_ms = 8000;
    std::vector<std::string> keys;  // DPAPI-protected, base64 (ai_secret in ai_client.h)
    AiUseFor use_for;

    bool uses(bool AiUseFor::*use) const { return enabled && use_for.*use; }
    bool remote_only() const { return mode == "remote_only"; }
};

}  // namespace cxxime

#endif  // CXXIME_AI_CONFIG_H_
