// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "switch_keys.h"

#include <string>

#include <windows.h>

namespace cxxime {
namespace settings {

namespace {

// The switch_key action of a tapped modifier for each switch, in box order; "noop" is none.
constexpr const char* kTapActions[kSwitchKeyCount] = {"code", "toggle_style", "toggle_punct",
                                                      "toggle_shape", "toggle_voice"};

// A tapped modifier: its left/right switch_key names, the generic VK (either side) and the two
// side VKs.
struct TapKeys {
    const char* left;
    const char* right;
    uint32_t any_vk;
    uint32_t left_vk;
    uint32_t right_vk;
};
constexpr TapKeys kTapKeys[] = {
    {"Shift_L", "Shift_R", VK_SHIFT, VK_LSHIFT, VK_RSHIFT},
    {"Control_L", "Control_R", VK_CONTROL, VK_LCONTROL, VK_RCONTROL},
};

const TapKeys* tap_keys_of(uint32_t vk) {
    for (const TapKeys& keys : kTapKeys) {
        if (vk == keys.any_vk || vk == keys.left_vk || vk == keys.right_vk) return &keys;
    }
    return nullptr;
}

std::string tap_key_action(const Config& config, const char* key) {
    const auto found = config.ascii_switch_key.find(key);
    return found != config.ascii_switch_key.end() ? found->second : "noop";
}

bool is_toggle_action(const std::string& action) {
    for (int slot = 1; slot < kSwitchKeyCount; ++slot) {
        if (action == kTapActions[slot]) return true;
    }
    return false;
}

// Whether a switch_key action belongs to a switch: any action but noop and the four others taps
// the Chinese/English switch (code, set_ascii_mode, clear ...).
bool action_of_slot(const std::string& action, int slot) {
    if (slot == 0) return action != "noop" && !is_toggle_action(action);
    return action == kTapActions[slot];
}

// What one side of the Chinese/English switch's tapped modifier does. Everything that is not one of
// the six named actions is the plain switch (a hand-written "clear" or "candidate" included, which
// switch the mode the same way); "append" does nothing on a tap (ascii_composer).
TapAction tap_action_of(const std::string& action) {
    if (action == "noop" || action == "append") return TapAction::kNone;
    if (action == "set_ascii_mode") return TapAction::kEnglish;
    if (action == "unset_ascii_mode") return TapAction::kChinese;
    if (is_toggle_action(action)) return TapAction::kNone;  // one of the other four switches
    return TapAction::kToggle;
}

const char* tap_action_value(TapAction action) {
    switch (action) {
    case TapAction::kToggle:
        return "code";
    case TapAction::kEnglish:
        return "set_ascii_mode";
    case TapAction::kChinese:
        return "unset_ascii_mode";
    case TapAction::kNone:
    default:
        return "noop";
    }
}

// What each side of a tap choice takes: the Chinese/English switch by its two actions, any other
// switch by the side its key stands for.
void sides_of(const KeyChoice& choice, bool* left, bool* right) {
    if (choice.tap_left != TapAction::kNone || choice.tap_right != TapAction::kNone) {
        *left = choice.tap_left != TapAction::kNone;
        *right = choice.tap_right != TapAction::kNone;
        return;
    }
    const TapSide side = tap_side(choice.tap_key);
    *left = side != TapSide::kRight;
    *right = side != TapSide::kLeft;
}

}  // namespace

uint32_t tap_family(uint32_t vk) {
    const TapKeys* keys = tap_keys_of(vk);
    return keys ? keys->any_vk : 0;
}

TapSide tap_side(uint32_t vk) {
    const TapKeys* keys = tap_keys_of(vk);
    if (keys && vk == keys->left_vk) return TapSide::kLeft;
    if (keys && vk == keys->right_vk) return TapSide::kRight;
    return TapSide::kEither;
}

uint32_t tap_key_on_side(uint32_t family, TapSide side) {
    const TapKeys* keys = tap_keys_of(family);
    if (!keys) return 0;
    switch (side) {
    case TapSide::kLeft:
        return keys->left_vk;
    case TapSide::kRight:
        return keys->right_vk;
    case TapSide::kEither:
    default:
        return keys->any_vk;
    }
}

bool tap_choices_overlap(const KeyChoice& left, const KeyChoice& right) {
    if (left.kind != KeyChoice::Kind::kTap || right.kind != KeyChoice::Kind::kTap) return false;
    const uint32_t family = tap_family(left.tap_key);
    if (family == 0 || family != tap_family(right.tap_key)) return false;
    bool left_left = false, left_right = false, right_left = false, right_right = false;
    sides_of(left, &left_left, &left_right);
    sides_of(right, &right_left, &right_right);
    return (left_left && right_left) || (left_right && right_right);
}

const TapChoice* switch_tap_choices() {
    // The menu of the Chinese/English box, in the order it is shown (ui: keys.tap_*).
    static const TapChoice choices[kSwitchTapChoiceCount] = {
        {TapAction::kToggle, TapAction::kToggle, "keys.tap_both_toggle"},
        {TapAction::kToggle, TapAction::kNone, "keys.tap_left_toggle"},
        {TapAction::kNone, TapAction::kToggle, "keys.tap_right_toggle"},
        {TapAction::kEnglish, TapAction::kChinese, "keys.tap_left_en_right_zh"},
        {TapAction::kChinese, TapAction::kEnglish, "keys.tap_left_zh_right_en"},
        {TapAction::kEnglish, TapAction::kNone, "keys.tap_left_en"},
        {TapAction::kChinese, TapAction::kNone, "keys.tap_left_zh"},
        {TapAction::kNone, TapAction::kEnglish, "keys.tap_right_en"},
        {TapAction::kNone, TapAction::kChinese, "keys.tap_right_zh"},
    };
    return choices;
}

int switch_tap_choice_index(TapAction left, TapAction right) {
    const TapChoice* choices = switch_tap_choices();
    for (int i = 0; i < kSwitchTapChoiceCount; ++i) {
        if (choices[i].left == left && choices[i].right == right) return i;
    }
    return -1;
}

KeyChoice switch_key_choice(const Config& config, int slot) {
    const KeyboardShortcut* combos[] = {&config.ascii_toggle_shortcut,
                                        &config.english_style_shortcut,
                                        &config.punct_toggle_shortcut,
                                        &config.shape_toggle_shortcut,
                                        &config.voice_input_shortcut};
    if (combos[slot]->enabled()) return KeyChoice::of(*combos[slot]);
    // Each side on its own (a hand-written config may give the two sides to two switches).
    for (const TapKeys& keys : kTapKeys) {
        const std::string left_action = tap_key_action(config, keys.left);
        const std::string right_action = tap_key_action(config, keys.right);
        const bool left = action_of_slot(left_action, slot);
        const bool right = action_of_slot(right_action, slot);
        if (!left && !right) continue;
        KeyChoice choice = KeyChoice::tap(keys.any_vk);
        if (slot == 0) {
            // What each side does, so the box can show it and the arrow menu can offer it.
            choice.tap_left = tap_action_of(left_action);
            choice.tap_right = tap_action_of(right_action);
        } else {
            choice.tap_key = left && right ? keys.any_vk
                                           : left ? keys.left_vk
                                                  : keys.right_vk;
        }
        return choice;
    }
    return KeyChoice::none();
}

void apply_switch_key_choices(Config& config, const KeyChoice (&choices)[kSwitchKeyCount]) {
    KeyboardShortcut* combos[] = {&config.ascii_toggle_shortcut, &config.english_style_shortcut,
                                  &config.punct_toggle_shortcut, &config.shape_toggle_shortcut,
                                  &config.voice_input_shortcut};
    for (const TapKeys& keys : kTapKeys) {
        config.ascii_switch_key[keys.left] = "noop";
        config.ascii_switch_key[keys.right] = "noop";
    }
    // A tapped Alt never reaches the input method: none is kept (1.5.3 builds before the release
    // could write one).
    config.ascii_switch_key["Alt_L"] = "noop";
    config.ascii_switch_key["Alt_R"] = "noop";
    for (int slot = 0; slot < kSwitchKeyCount; ++slot) {
        const KeyChoice& choice = choices[slot];
        *combos[slot] = choice.kind == KeyChoice::Kind::kCombo ? choice.combo : KeyboardShortcut{};
        if (choice.kind != KeyChoice::Kind::kTap) continue;
        const TapKeys* keys = tap_keys_of(choice.tap_key);
        if (!keys) continue;
        if (slot == 0) {
            // The Chinese/English switch: what each side of the tapped modifier does (tap_left /
            // tap_right), which is also which sides it takes at all.
            config.ascii_switch_key[keys->left] = tap_action_value(choice.tap_left);
            config.ascii_switch_key[keys->right] = tap_action_value(choice.tap_right);
            continue;
        }
        const char* action = kTapActions[slot];
        const TapSide side = tap_side(choice.tap_key);
        if (side != TapSide::kRight) config.ascii_switch_key[keys->left] = action;
        if (side != TapSide::kLeft) config.ascii_switch_key[keys->right] = action;
    }
}

}  // namespace settings
}  // namespace cxxime
