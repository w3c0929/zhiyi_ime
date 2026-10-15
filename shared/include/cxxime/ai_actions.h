// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The AI actions on selected text (docs/ai.md): what the menu shows and what the provider is
// asked. The list is data/ai_actions.json, so an action is added as data, not as code — the menu
// order is the file's order, and the id is what the client sends over IPC (TEXT_UPLOAD_BEGIN,
// ipc_protocol.h). Read by the server (the prompts), the TSF (the menu) and the settings program
// (the list of rows), all of which read the same installed file.
#ifndef CXXIME_AI_ACTIONS_H_
#define CXXIME_AI_ACTIONS_H_

#include <string>
#include <vector>

#include <cxxime/keyboard_shortcut.h>
namespace cxxime {

struct AiAction {
    std::string id;      // "proofread": the key of the action
    std::string label;   // the Chinese name shown in the menu
    std::string label_en;  // the English one
    std::string prompt;  // the system prompt; the user message is the selected text itself
    // replace: the answer replaces the selection (one Ctrl+Z undoes it). preview: the answer is
    // only shown, the selection stays.
    bool replace = true;
    int max_input_chars = 8000;  // a longer selection is refused with a note, never cut
    int timeout_ms = 20000;
};

// The actions of data/ai_actions.json, in the file's order; empty when the file is missing or
// cannot be read (the feature is then simply not offered). Loaded once.
const std::vector<AiAction>& ai_actions();
// The action with this id, or nullptr.
const AiAction* find_ai_action(const std::string& id);

// The name of `action` to show, for the interface language `ui_language` ("auto", "zh-CN",
// "en-US", ...). Anything that is not English — "auto" included — gets the Chinese name, which is
// what the input method's own messages use.
const std::string& ai_action_label(const AiAction& action, const std::string& ui_language);

// An AI action key (ai.action_key, ai.action_keys): one press on its own — the text the user
// selected is read (copied) for them, so no second key and no key of their own is needed. Any of
//   a tapped Shift or Ctrl ("RShift", "Shift" for either side),
//   an F1–F11 key on its own,
//   Ctrl or Alt (with Shift) and a letter, digit, punctuation key or Space.
// Never a plain letter or digit: it would be taken from typing.
bool is_valid_ai_action_key(const KeyboardShortcut& shortcut);

// Its configuration string ("RShift", "Shift", "F6", "Ctrl+K"; "disabled" for none). A tapped
// modifier has no place in a plain KeyboardShortcut string (Shift there is the modifier), hence
// these two.
std::string ai_action_key_string(const KeyboardShortcut& shortcut);
bool parse_ai_action_key(const std::string& value, KeyboardShortcut* shortcut);

} // namespace cxxime

#endif // CXXIME_AI_ACTIONS_H_
