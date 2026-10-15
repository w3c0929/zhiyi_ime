// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The AI actions on selected text (docs/ai.md): what the menu shows and what the provider is
// asked. The list is data/ai_actions.json, so an action is added as data, not as code — the menu
// order is the file's order, and the id is what the client sends over IPC (TEXT_UPLOAD_BEGIN,
// ipc_protocol.h). Read by the server (the prompts), the TSF (the menu) and the settings program
// (the list of rows), all of which read the same installed file.
#ifndef CXXIME_AI_ACTIONS_H_
#define CXXIME_AI_ACTIONS_H_

#include <cstddef>
#include <string>
#include <vector>

#include <cxxime/keyboard_shortcut.h>
namespace cxxime {

// What the client does with the answer (the "insert" field of data/ai_actions.json):
//   kReplace: the answer replaces the selection — one Ctrl+Z undoes it;
//   kBelow:   the answer is written under the selection, on lines of its own (the selection and
//             everything after it stay; a summary is read where the text it summarizes is);
//   kPreview: the answer is only shown, the selection stays.
enum class AiActionInsert { kReplace, kBelow, kPreview };

// The action that translates (docs/ai.md): the one with a language list. It is the action the
// \fy command and the settings page's language row belong to.
inline constexpr char kAiLanguageActionId[] = "translate";

// How many characters (UTF-8 code points) `text` holds — the unit of max_input_chars below.
// Every side that checks that limit counts the same way: a selection the client cut to 8000
// characters is up to 24000 bytes of Chinese, and the server refusing it for the byte count is
// what the two of them used to disagree about.
inline std::size_t ai_text_length(const std::string& text) {
    std::size_t characters = 0;
    for (const char byte : text) {
        if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80) ++characters;
    }
    return characters;
}

struct AiAction {
    std::string id;      // "proofread": the key of the action
    std::string label;   // the Chinese name shown in the menu
    std::string label_en;  // the English one
    std::string prompt;  // the system prompt; the user message is the selected text itself
    AiActionInsert insert = AiActionInsert::kReplace;
    int max_input_chars = 8000;  // a longer selection is refused with a note, never cut
    int timeout_ms = 20000;
    // An action that translates (the "translate" one, docs/ai.md): the languages it offers, the
    // first being the default. The choice (ai.translate_language) replaces {{language}} in the
    // prompt; the \fy command changes it from the input box. Empty for the other actions.
    std::vector<std::string> languages;
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

// The name of a language of the translate action ("ja" → 日语, or Japanese for an English
// interface), as the \fy candidate list shows it. An unknown code comes back as it is.
std::string ai_language_label(const std::string& code, const std::string& ui_language);

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
