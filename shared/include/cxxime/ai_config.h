// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Online AI (Config::ai): the input method can send the text it works on to an API
// (OpenAI-compatible, Anthropic Messages or Google Generative AI) instead of the offline
// models. Off by default — nothing leaves the machine until the user turns it on — and each
// feature that may use it has its own switch, so "use the API for the selection translation"
// does not imply "use it for the voice input".
#ifndef CXXIME_AI_CONFIG_H_
#define CXXIME_AI_CONFIG_H_

#include <map>
#include <string>
#include <vector>

#include <cxxime/keyboard_shortcut.h>

namespace cxxime {

// The features that may run on the API (Config::ai.use_for).
struct AiUseFor {
    bool translate = false;  // selection translation (Ctrl+C+C)
    bool selection = false;  // AI actions on the selected text
    bool voice = false;      // the recognized text is proofread before it is typed

    // Any of them on: the settings page shows the API as in use.
    bool any() const { return translate || selection || voice; }
};

struct AiConfig {
    // The one switch there is: on = the features ticked in use_for run on the API, and anything the
    // API fails at falls back to the offline model. Off = nothing leaves the machine.
    bool enabled = false;
    std::string provider = "deepseek";  // an id of the built-in provider table (ai_client.h)
    std::string base_url;               // empty: the provider's own
    std::string model;                  // empty: the provider's own
    std::string reasoning_effort;  // "" = do not send one
    int timeout_ms = 8000;
    std::vector<std::string> keys;  // DPAPI-protected, base64 (ai_secret in ai_client.h)
    // A key is only valid for the service it was made for, so each provider keeps its own list;
    // keys above mirrors the selected provider's list, which is what the client uses.
    std::map<std::string, std::vector<std::string>> keys_by_provider;
    AiUseFor use_for;
    // Off: the reasoning level is not sent at all (SuMail's thinking switch).
    bool thinking = true;
    // AI actions on the selected text (ai_actions.h, docs/ai.md). `action_key` opens the menu of
    // actions; it is the second key of a pair whose first key is the translate key (Ctrl+C: the
    // application copies the selection), so the program in front only has to leave this key alone.
    // `action_keys` gives an action a key of its own, run straight away instead of through the
    // menu, with the same reading. `action_disabled` hides an action from the menu (an id that is
    // not there is listed).
    KeyboardShortcut action_key;
    std::map<std::string, std::string> action_keys;
    std::map<std::string, bool> action_disabled;
    // The key that opens the translate language list — the same list \fy opens (docs/ai.md), so it
    // can be had without typing the command. Written like action_key (a tapped modifier, an F key,
    // or Ctrl/Alt and a key); empty = off.
    KeyboardShortcut language_key;
    // The key that opens the voice post-processing list (纠错 / 提问) — the same kind of list the
    // language key opens, for what the recognized text goes through (voice_polish_action) without
    // going into the settings. Written like action_key; empty = off.
    KeyboardShortcut voice_action_key;
    // The key that opens the online switch's list (线上（开）/ 线上（关）) — the same kind of list,
    // for turning the whole API on and off from the input box instead of the settings page. It
    // writes the same field the master switch does (enabled). Written like action_key; empty = off.
    KeyboardShortcut enable_key;
    // Web search, used by the "ask" action only (docs/ai.md): the question goes out first and what
    // comes back is handed to the model as context. Two services can do it — Tavily, and DeepSeek's
    // own search API (the same platform key as its chat API, so leaving the DeepSeek entry empty
    // means "use the DeepSeek API key of this setup"). `search_keys` holds one DPAPI-protected key
    // per service, like keys_by_provider above; `search_enabled` is the switch (the settings page's
    // tick and `search_key`'s list both write it) and `search_key` opens that list from the input
    // box. Written like action_key; empty = off.
    std::string search_provider = "tavily";  // "tavily" | "deepseek"
    std::map<std::string, std::string> search_keys;
    bool search_enabled = false;
    KeyboardShortcut search_key;
    // What Chinese is translated into on the API: a language code from the "translate" action's own
    // list (ai_actions.json) — the translate action and the selection translation key (Ctrl+C+C)
    // alike; anything that is not Chinese goes into Chinese. The offline model always goes by the
    // Learning page's rows instead. Changed on the AI page, or from the input box with \fy.
    std::string translate_language = "en";
    // ai.use_for.voice: which action the recognized text goes through before it is typed — an id
    // of data/ai_actions.json, "proofread" (纠错) by default, whose prompt is then the one used.
    // A name of your own, or empty for no post-processing at all (the text is typed as heard).
    std::string voice_polish_action = "proofread";

    bool uses(bool AiUseFor::*use) const { return enabled && use_for.*use; }
};

}  // namespace cxxime

#endif  // CXXIME_AI_CONFIG_H_
