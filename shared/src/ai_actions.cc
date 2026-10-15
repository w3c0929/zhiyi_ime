// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include <cxxime/ai_actions.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <windows.h>

#include <json.hpp>

#include <cxxime/data_path.h>

namespace cxxime {

namespace {

AiAction action_from_json(const nlohmann::json& row) {
    AiAction action;
    if (row.contains("id") && row["id"].is_string()) action.id = row["id"].get<std::string>();
    if (row.contains("label") && row["label"].is_string()) {
        action.label = row["label"].get<std::string>();
    }
    if (row.contains("label_en") && row["label_en"].is_string()) {
        action.label_en = row["label_en"].get<std::string>();
    }
    if (row.contains("prompt") && row["prompt"].is_string()) {
        action.prompt = row["prompt"].get<std::string>();
    }
    if (row.contains("insert") && row["insert"].is_string()) {
        const std::string mode = row["insert"].get<std::string>();
        if (mode == "preview") {
            action.insert = AiActionInsert::kPreview;
        } else if (mode == "below") {
            action.insert = AiActionInsert::kBelow;
        } else {
            action.insert = AiActionInsert::kReplace;
        }
    }
    if (row.contains("max_input_chars") && row["max_input_chars"].is_number_integer()) {
        action.max_input_chars = (std::max)(0, row["max_input_chars"].get<int>());
    }
    if (row.contains("timeout_ms") && row["timeout_ms"].is_number_integer()) {
        action.timeout_ms = (std::max)(1000, row["timeout_ms"].get<int>());
    }
    if (row.contains("languages") && row["languages"].is_array()) {
        for (const auto& language : row["languages"]) {
            if (language.is_string()) action.languages.push_back(language.get<std::string>());
        }
    }
    return action;
}

std::vector<AiAction> load() {
    std::vector<AiAction> actions;
    std::ifstream file(std::filesystem::path(data_path("ai_actions.json")));
    if (!file) return actions;
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        return actions;  // a broken file leaves the feature off rather than half on
    }
    if (!parsed.is_object() || !parsed.contains("actions") || !parsed["actions"].is_array()) {
        return actions;
    }
    for (const auto& row : parsed["actions"]) {
        if (!row.is_object()) continue;
        AiAction action = action_from_json(row);
        // An action without an id or a name has nothing to show. A prompt it must have too, unless
        // it is one that picks a language (the translate action): that work is the translation's,
        // which has a prompt of its own (docs/ai.md).
        if (action.id.empty() || (action.label.empty() && action.label_en.empty()) ||
            (action.prompt.empty() && action.languages.empty())) {
            continue;
        }
        actions.push_back(std::move(action));
    }
    return actions;
}

} // namespace

const std::vector<AiAction>& ai_actions() {
    static const std::vector<AiAction> actions = load();
    return actions;
}

const AiAction* find_ai_action(const std::string& id) {
    const std::vector<AiAction>& actions = ai_actions();
    const auto found = std::find_if(actions.begin(), actions.end(),
                                    [&](const AiAction& action) { return action.id == id; });
    return found == actions.end() ? nullptr : &*found;
}

const std::string& ai_action_label(const AiAction& action, const std::string& ui_language) {
    const bool english = ui_language.compare(0, 2, "en") == 0;
    if (english && !action.label_en.empty()) return action.label_en;
    return action.label;
}

namespace {

// A tapped modifier, as a KeyboardShortcut holds one: the key, no modifiers. Windows hands these
// to input methods; Alt it does not, so an Alt tap would never arrive.
struct TappedModifier {
    const char* name;   // what the configuration string says
    uint32_t virtual_key;
};
constexpr TappedModifier kTappedModifiers[] = {
    {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT},       {"SHIFT", VK_SHIFT},
    {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},     {"CTRL", VK_CONTROL},
};

const TappedModifier* tapped_modifier(uint32_t virtual_key) {
    for (const TappedModifier& modifier : kTappedModifiers) {
        if (modifier.virtual_key == virtual_key) return &modifier;
    }
    return nullptr;
}

} // namespace

bool is_valid_ai_action_key(const KeyboardShortcut& shortcut) {
    if (!shortcut.enabled()) return true;  // off
    if (shortcut.repeat != 1) return false;  // one press
    if (shortcut.modifiers == 0 && tapped_modifier(shortcut.virtual_key)) return true;
    return is_valid_input_mode_shortcut(shortcut);
}

std::string ai_action_key_string(const KeyboardShortcut& shortcut) {
    if (!shortcut.enabled()) return "disabled";
    if (shortcut.modifiers == 0) {
        if (const TappedModifier* modifier = tapped_modifier(shortcut.virtual_key)) {
            return modifier->name;
        }
    }
    return keyboard_shortcut_string(shortcut);
}

bool parse_ai_action_key(const std::string& value, KeyboardShortcut* shortcut) {
    if (!shortcut) return false;
    // The tapped modifiers first: "SHIFT" is a tap here, while in a plain shortcut string it is the
    // Shift modifier ("Shift+Space").
    std::string upper;
    upper.reserve(value.size());
    for (const char ch : value) {
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    for (const TappedModifier& modifier : kTappedModifiers) {
        if (upper == modifier.name) {
            *shortcut = KeyboardShortcut{0, modifier.virtual_key, 1};
            return true;
        }
    }
    if (!parse_keyboard_shortcut(value, shortcut)) return false;
    return shortcut->enabled() ? is_valid_ai_action_key(*shortcut) : true;
}

} // namespace cxxime
