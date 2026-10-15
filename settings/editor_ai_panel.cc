// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The "AI" page (docs/ai.md): the optional online service. Off by default — nothing leaves the
// machine until it is turned on here — and each feature that may use it has its own switch, so
// "let the API translate the selection" does not imply "let it see the voice input".
//
// Laid out like the AI settings of the SuMail project this borrows from (provider, keys,
// address, model, reasoning level, test):
//   * picking a service fills in its address and model, so the page always shows what will run;
//   * several keys are kept per setup: the list picks which one is tried first and the button
//     beside it drops one, while the eye reveals what is being typed;
//   * "Detect" reads the address back and selects the service it belongs to;
//   * "Get models" asks the service for its list (ai_client.cc list_models — /models, Ollama's
//     /api/tags, Anthropic's /v1/models) and fills the drop-down.
// Keys are stored DPAPI-protected (ai_client.h), never in clear.

#include "editor_app.h"

#include "key_capture.h"

#include <algorithm>
#include <cxxime/ai_actions.h>
#include <chrono>
#include <string>
#include <vector>

#include <cxxime/ai_client.h>

#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {

namespace {

// The page's own controls. The ids below 6000 are taken by the other pages (a collision shows up
// as one page's button running another page's command), so 6000+ is this page's range.
constexpr int kAiRevealId = 6014;
constexpr int kAiKeyAddId = 6015;
constexpr int kAiKeyListId = 6016;
constexpr int kAiKeyRemoveId = 6017;
constexpr int kAiDetectId = 6018;
constexpr int kAiEffortId = 6019;
constexpr int kAiThinkingId = 6020;
constexpr int kAiActionKeyId = 6021;       // AI actions on the selection: the menu key
constexpr int kAiActionListFirstId = 6022;  // .. 6029: one checkbox per action (in the menu)
constexpr int kAiActionKeyFirstId = 6040;   // .. 6047: one key box per action
constexpr int kAiActionLangFirstId = 6050;  // .. 6057: one language box (the translate action)
constexpr std::size_t kAiActionRowMax = 8;  // more actions than the page has room for

// The name of a language code, as the learning page names them (data/ui.*.json, "lang.<code>").
std::wstring language_name(const std::string& code) {
    return std::wstring(tr(("lang." + code).c_str()));
}

// A key that cannot work for an AI action: it is one press on its own, so the translate key's own
// combination would be taken by the translation first.
std::wstring action_key_notice(const KeyChoice& choice, const KeyChoice& translate_choice) {
    if (choice.kind == KeyChoice::Kind::kCombo &&
        same_key_choice(choice, translate_choice)) {
        return tr("ai.action_key_same");
    }
    return std::wstring();
}

// A tapped modifier is a tap to the box and {key, no modifiers} to the config; everything else is
// a combination. (KeyboardShortcut is what both the config and the input method pass around.)
bool is_tap_key(uint32_t virtual_key) {
    return virtual_key == VK_LSHIFT || virtual_key == VK_RSHIFT || virtual_key == VK_SHIFT ||
           virtual_key == VK_LCONTROL || virtual_key == VK_RCONTROL || virtual_key == VK_CONTROL;
}

KeyChoice action_key_choice(const cxxime::KeyboardShortcut& shortcut) {
    if (!shortcut.enabled()) return KeyChoice::none();
    if (shortcut.modifiers == 0 && is_tap_key(shortcut.virtual_key)) {
        return KeyChoice::tap(shortcut.virtual_key);
    }
    return KeyChoice::of(shortcut);
}

cxxime::KeyboardShortcut action_key_shortcut(const KeyChoice& choice) {
    if (choice.kind == KeyChoice::Kind::kTap) {
        return cxxime::KeyboardShortcut{0, choice.tap_key, 1};
    }
    if (choice.kind == KeyChoice::Kind::kCombo) {
        cxxime::KeyboardShortcut shortcut = choice.combo;
        shortcut.repeat = 1;  // one press: the box may have stored a double one
        return shortcut;
    }
    return {};
}

// The two modes, in the order of the combo (AiConfig::mode).
const char* const kAiModeKeys[] = {"ai.mode_prefer_remote", "ai.mode_remote_only"};
// The reasoning levels the combo offers; the first sends nothing (AiConfig::reasoning_effort).
const char* const kAiEffortValues[] = {"", "low", "medium", "high"};
const char* const kAiEffortKeys[] = {"ai.effort_default", "ai.effort_low", "ai.effort_medium",
                                     "ai.effort_high"};

std::wstring with_number(const wchar_t* text, std::size_t number) {
    return std::wstring(text) + L" " + std::to_wstring(number);
}

// A plain text box: make_edit (editor_controls.cc) is a number box (ES_NUMBER), which refuses an
// address or a key with "you can only type digits here".
HWND make_text_edit(int id, int x, int y, int width, HWND parent) {
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, x, y, width,
                                kCtrlH, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandle(nullptr), nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return edit;
}

// A drop-down made like make_combo but without its wheel-to-the-page redirect, so the open list
// scrolls with the wheel (thirty-odd services do not fit at once).
HWND make_list_combo(int id, int x, int y, int width, int drop_rows, HWND parent) {
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                     CBS_DROPDOWNLIST,
                                 x, y, width, kCtrlH * drop_rows, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandle(nullptr), nullptr);
    SendMessageW(combo, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return combo;
}

// The same, but the box takes a typed-in value too (the model).
HWND make_edit_combo(int id, int x, int y, int width, int drop_rows, HWND parent) {
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWN,
                                 x, y, width, kCtrlH * drop_rows, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandle(nullptr), nullptr);
    SendMessageW(combo, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    return combo;
}

void set_combo_text(HWND combo, const std::string& text) {
    SetWindowTextW(combo, utf8_to_wstr(text).c_str());
}

// The models of `models` replace the drop-down's items, with `current` left selected.
void fill_model_combo(HWND combo, const std::vector<std::string>& models,
                      const std::string& current) {
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int selected = -1;
    int index = 0;
    for (const std::string& id : models) {
        const std::wstring wide = utf8_to_wstr(id);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
        if (id == current) selected = index;
        ++index;
    }
    if (selected >= 0) {
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
    } else {
        set_combo_text(combo, current);
    }
}

// "oc_s……-2WK": enough to tell two stored keys apart without putting one on the screen.
std::wstring masked_key(const std::string& key) {
    const std::wstring wide = utf8_to_wstr(key);
    if (wide.size() <= 10) return L"********";
    return wide.substr(0, 4) + L"\u2026\u2026" + wide.substr(wide.size() - 4);
}

}  // namespace

void EditorApp::create_ai_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    const int labels = label_width({"ai.enabled", "ai.provider", "ai.base_url", "ai.model",
                                    "ai.key", "ai.mode", "ai.timeout", "ai.use_for", "ai.effort"});
    const int control_w = S(300);
    const int hint_w = S(420);
    int y = card_begin(panel, kPanelPadTop, tr("ai.card_service"));

    const int label_x = make_aligned_label(tr("ai.enabled"), x0, labels, y, panel);
    hAiEnabled_ = make_check(kAiEnabledId, tr("ai.enabled_check"), label_x, y, hint_w, panel);
    y += kRowH;
    make_hint(tr("ai.enabled_hint"), label_x, y - S(8), hint_w, panel, 1);
    y += S(24);

    // 服务商
    const int provider_x = make_aligned_label(tr("ai.provider"), x0, labels, y, panel);
    hAiProvider_ = make_list_combo(kAiProviderId, provider_x, y, control_w, 20, panel);
    for (const ai::Provider& provider : ai::providers()) {
        combo_add(hAiProvider_, utf8_to_wstr(provider.name).c_str());
    }
    y += kRowH;

    // API 密钥（两行，与 SuMail 一致）：上一行选"用哪把"（右端 ✕ 删除）；下一行是这把密钥的值
    // （默认圆点，右端按钮显示/隐藏，点开能看全）。改它就是改这一把，输入另一把再保存就是新增。
    const int key_x = make_aligned_label(tr("ai.key"), x0, labels, y, panel);
    const int key_w = S(200);
    hAiKeyList_ = make_list_combo(kAiKeyListId, key_x, y, key_w, 6, panel);
    hAiKeyRemove_ = make_button(kAiKeyRemoveId, L"\u2715", key_x + key_w + S(8), y, S(56), panel);
    y += kRowH;
    hAiKey_ = make_text_edit(kAiKeyId, key_x, y, key_w, panel);
    SendMessageW(hAiKey_, EM_SETPASSWORDCHAR, static_cast<WPARAM>(L'\u25CF'), 0);
    SendMessageW(hAiKey_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(tr("ai.key_placeholder")));
    hAiKeyReveal_ = make_button(kAiRevealId, tr("ai.key_show"), key_x + key_w + S(8), y, S(56),
                                panel);
    y += kRowH;
    hAiKeyHint_ = make_hint(tr("ai.key_hint"), key_x, y - S(8), hint_w, panel, 1);
    y += S(24);

    // 服务地址（旁边「识别」：按地址认出服务商）
    const int url_x = make_aligned_label(tr("ai.base_url"), x0, labels, y, panel);
    hAiBaseUrl_ = make_text_edit(kAiBaseUrlId, url_x, y, control_w - S(70), panel);
    hAiDetect_ = make_button(kAiDetectId, tr("ai.detect"), url_x + control_w - S(64), y, S(64),
                             panel);
    y += kRowH;
    hAiModelsHint_ = make_hint(tr("ai.base_url_hint"), url_x, y - S(8), hint_w, panel, 1);
    y += S(24);

    // 模型（可输入，「获取模型」拉列表）
    const int model_x = make_aligned_label(tr("ai.model"), x0, labels, y, panel);
    hAiModel_ = make_edit_combo(kAiModelId, model_x, y, control_w - S(116), 12, panel);
    hAiModels_ = make_button(kAiModelsId, tr("ai.get_models"), model_x + control_w - S(110), y,
                             S(110), panel);
    y += kRowH;

    // 推理等级
    const int effort_x = make_aligned_label(tr("ai.effort"), x0, labels, y, panel);
    hAiEffort_ = make_list_combo(kAiEffortId, effort_x, y, S(160), 5, panel);
    for (const char* key : kAiEffortKeys) combo_add(hAiEffort_, tr(key));
    // 思考模式：关掉就完全不发推理等级（SuMail 的 thinking 开关）。
    hAiThinking_ = make_check(kAiThinkingId, tr("ai.thinking"), effort_x + S(170), y, S(240), panel);
    y += kRowH;

    // 超时与模式
    const int timeout_x = make_aligned_label(tr("ai.timeout"), x0, labels, y, panel);
    hAiTimeout_ = make_edit(kAiTimeoutId, timeout_x, y, S(90), panel);
    y += kRowH;
    const int mode_x = make_aligned_label(tr("ai.mode"), x0, labels, y, panel);
    hAiMode_ = make_list_combo(kAiModeId, mode_x, y, S(260), 4, panel);
    for (const char* key : kAiModeKeys) combo_add(hAiMode_, tr(key));
    y += kRowH;

    hAiTest_ = make_button(kAiTestId, tr("ai.test"), timeout_x, y, S(110), panel);
    hAiTestResult_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     timeout_x + S(122), y + S(5), S(320), S(kFontPt + 8), panel,
                                     nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(hAiTestResult_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    y += kRowH;
    y = card_end(panel, y);

    y = card_begin(panel, y, tr("ai.card_features"));
    const int use_x = make_aligned_label(tr("ai.use_for"), x0, labels, y, panel);
    hAiUseTranslate_ =
        make_check(kAiUseTranslateId, tr("ai.use_translate"), use_x, y, hint_w, panel);
    y += kRowH;
    hAiUseSelection_ =
        make_check(kAiUseSelectionId, tr("ai.use_selection"), use_x, y, hint_w, panel);
    y += kRowH;
    hAiUseEnglish_ = make_check(kAiUseEnglishId, tr("ai.use_english"), use_x, y, hint_w, panel);
    y += kRowH;
    hAiUseVoice_ = make_check(kAiUseVoiceId, tr("ai.use_voice"), use_x, y, hint_w, panel);
    y += kRowH;
    hAiUseGlossary_ = make_check(kAiUseGlossaryId, tr("ai.use_glossary"), use_x, y, hint_w, panel);
    y += kRowH;
    make_hint(tr("ai.use_for_hint"), use_x, y - S(8), hint_w, panel, 1);
    y += S(24);
    y = card_end(panel, y);

    // AI actions on the selection (docs/ai.md): the key that opens the menu of actions beside the
    // selected text, and a row per action — whether the menu lists it, and a key of its own.
    y = card_begin(panel, y, tr("ai.card_actions"));
    const int action_x = make_aligned_label(tr("ai.action_key"), x0, labels, y, panel);
    // One press on its own (the selection is read for the user), so a tapped modifier (right Shift
    // and the like) is allowed and the box takes it as a tap; no doubling.
    hAiActionKey_ = create_key_capture(kAiActionKeyId, action_x, y, S(160), kCtrlH, panel, true);
    key_capture_set_allow_common(hAiActionKey_, false);
    key_capture_set_check(hAiActionKey_, [this](const KeyChoice& choice) -> std::wstring {
        return action_key_notice(choice, hTranslateKey_ ? key_capture_get(hTranslateKey_)
                                                        : KeyChoice::none());
    });
    y += kRowH;

    hAiActionRows_.clear();
    hAiActionKeys_.clear();
    hAiActionLangs_.clear();
    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    if (!actions.empty()) {
        const int row_x = make_aligned_label(tr("ai.action_list"), x0, labels, y, panel);
        for (std::size_t i = 0; i < actions.size() && i < kAiActionRowMax; ++i) {
            const int row_y = y + static_cast<int>(i) * kRowH;
            hAiActionRows_.push_back(make_check(
                kAiActionListFirstId + static_cast<int>(i),
                utf8_to_wstr(cxxime::ai_action_label(actions[i], ui_language_)).c_str(), row_x,
                row_y, S(200), panel));
            HWND key = create_key_capture(kAiActionKeyFirstId + static_cast<int>(i),
                                          row_x + S(210), row_y, S(150), kCtrlH, panel, true);
            key_capture_set_allow_common(key, false);
            key_capture_set_check(key, [this](const KeyChoice& choice) -> std::wstring {
                return action_key_notice(choice, hTranslateKey_ ? key_capture_get(hTranslateKey_)
                                                                : KeyChoice::none());
            });
            hAiActionKeys_.push_back(key);
            // An action that picks a language (the translate one): which language it goes into.
            HWND language = nullptr;
            if (!actions[i].languages.empty()) {
                language = make_list_combo(kAiActionLangFirstId + static_cast<int>(i),
                                           row_x + S(370), row_y, S(150),
                                           static_cast<int>(actions[i].languages.size()), panel);
                for (const std::string& code : actions[i].languages) {
                    combo_add(language, language_name(code).c_str());
                }
            }
            hAiActionLangs_.push_back(language);
        }
        y += kRowH * static_cast<int>((std::min)(actions.size(), kAiActionRowMax));
    }
    make_hint(tr("ai.action_key_hint"), action_x, y - S(6), hint_w, panel, 2);
    y += S(44);
    card_end(panel, y);
}


// The stored keys, masked, into the list; the hint says how many there are.
void EditorApp::refresh_ai_key_list() {
    if (!hAiKeyList_) return;
    SendMessageW(hAiKeyList_, CB_RESETCONTENT, 0, 0);
    if (ai_keys_.empty()) {  // nothing saved yet: the list says so, as SuMail's does
        SendMessageW(hAiKeyList_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(tr("ai.key_none")));
        SetWindowTextW(hAiKeyHint_, with_number(tr("ai.keys_stored"), 0).c_str());
        SetWindowTextW(hAiKey_, L"");
        return;
    }
    int index = 0;
    for (const std::string& stored : ai_keys_) {
        std::string key;
        std::wstring text =
            cxxime::ai::unprotect_secret(stored, &key) ? masked_key(key) : L"********";
        if (index == 0) text += std::wstring(L" ") + tr("ai.key_in_use");
        SendMessageW(hAiKeyList_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        ++index;
    }
    if (!ai_keys_.empty()) SendMessageW(hAiKeyList_, CB_SETCURSEL, 0, 0);
    SetWindowTextW(hAiKeyHint_, with_number(tr("ai.keys_stored"), ai_keys_.size()).c_str());
    show_ai_key_value();
}

void EditorApp::populate_ai() {
    if (!hAiEnabled_) return;
    // Each service keeps its own keys: show the selected one's.
    const auto stored_for_provider = config_.ai.keys_by_provider.find(config_.ai.provider);
    ai_keys_ = stored_for_provider != config_.ai.keys_by_provider.end()
                   ? stored_for_provider->second
                   : config_.ai.keys;

    set_check(hAiEnabled_, config_.ai.enabled);

    const std::vector<ai::Provider>& providers = ai::providers();
    int index = 0;
    for (std::size_t i = 0; i < providers.size(); ++i) {
        if (config_.ai.provider == providers[i].id) index = static_cast<int>(i);
    }
    combo_set_index(hAiProvider_, index);

    // Always show what is in use: never a blank box.
    set_combo_text(hAiBaseUrl_, ai::effective_base_url(config_.ai));
    set_combo_text(hAiModel_, ai::effective_model(config_.ai));
    SetWindowTextW(hAiKey_, L"");
    combo_set_index(hAiMode_, config_.ai.mode == "remote_only" ? 1 : 0);
    set_edit_int(hAiTimeout_, config_.ai.timeout_ms);

    refresh_ai_effort_combo();  // the levels this service and model offer
    set_check(hAiThinking_, config_.ai.thinking);
    EnableWindow(hAiEffort_, config_.ai.thinking ? TRUE : FALSE);

    set_check(hAiUseTranslate_, config_.ai.use_for.translate);
    set_check(hAiUseSelection_, config_.ai.use_for.selection);
    set_check(hAiUseEnglish_, config_.ai.use_for.english);
    set_check(hAiUseVoice_, config_.ai.use_for.voice);
    set_check(hAiUseGlossary_, config_.ai.use_for.glossary);

    key_capture_set(hAiActionKey_, action_key_choice(config_.ai.action_key));
    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    for (std::size_t i = 0; i < hAiActionRows_.size() && i < actions.size(); ++i) {
        const std::string& id = actions[i].id;
        const auto hidden = config_.ai.action_disabled.find(id);
        set_check(hAiActionRows_[i], hidden == config_.ai.action_disabled.end() || !hidden->second);
        cxxime::KeyboardShortcut shortcut;
        const auto stored = config_.ai.action_keys.find(id);
        if (stored != config_.ai.action_keys.end()) {
            cxxime::parse_ai_action_key(stored->second, &shortcut);
        }
        key_capture_set(hAiActionKeys_[i], action_key_choice(shortcut));
        if (i < hAiActionLangs_.size() && hAiActionLangs_[i]) {
            const std::vector<std::string>& languages = actions[i].languages;
            const auto chosen = std::find(languages.begin(), languages.end(),
                                          config_.ai.translate_language);
            combo_set_index(hAiActionLangs_[i], chosen == languages.end()
                                                    ? 0
                                                    : static_cast<int>(chosen - languages.begin()));
        }
    }

    refresh_ai_key_list();
}

void EditorApp::read_ai(cxxime::Config& config) {
    if (!hAiEnabled_) return;
    config.ai.enabled = get_check(hAiEnabled_);

    const std::vector<ai::Provider>& providers = ai::providers();
    const int provider = combo_index(hAiProvider_);
    if (provider >= 0 && provider < static_cast<int>(providers.size())) {
        config.ai.provider = providers[static_cast<std::size_t>(provider)].id;
    }
    config.ai.base_url = edit_text_utf8(hAiBaseUrl_);
    wchar_t model[256] = {};
    GetWindowTextW(hAiModel_, model, static_cast<int>(std::size(model)));
    config.ai.model = wstr_to_utf8(model);
    const int effort = combo_index(hAiEffort_);
    config.ai.reasoning_effort = effort >= 1 && effort <= static_cast<int>(ai_effort_levels_.size())
                                     ? ai_effort_levels_[static_cast<std::size_t>(effort - 1)]
                                     : std::string();
    config.ai.thinking = get_check(hAiThinking_);
    config.ai.mode = combo_index(hAiMode_) == 1 ? "remote_only" : "prefer_remote";
    config.ai.timeout_ms = (std::max)(1000, (std::min)(get_edit_int(hAiTimeout_), 120000));
    config.ai.use_for.translate = get_check(hAiUseTranslate_);
    config.ai.use_for.selection = get_check(hAiUseSelection_);
    config.ai.use_for.english = get_check(hAiUseEnglish_);
    config.ai.use_for.voice = get_check(hAiUseVoice_);
    config.ai.use_for.glossary = get_check(hAiUseGlossary_);
    config.ai.action_key = action_key_shortcut(key_capture_get(hAiActionKey_));

    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    config.ai.action_keys.clear();
    config.ai.action_disabled.clear();
    for (std::size_t i = 0; i < hAiActionRows_.size() && i < actions.size(); ++i) {
        const std::string& id = actions[i].id;
        if (!get_check(hAiActionRows_[i])) config.ai.action_disabled[id] = true;
        const cxxime::KeyboardShortcut shortcut =
            action_key_shortcut(key_capture_get(hAiActionKeys_[i]));
        if (shortcut.enabled()) config.ai.action_keys[id] = cxxime::ai_action_key_string(shortcut);
        if (i < hAiActionLangs_.size() && hAiActionLangs_[i]) {
            const std::vector<std::string>& languages = actions[i].languages;
            const int index = combo_index(hAiActionLangs_[i]);
            if (index >= 0 && index < static_cast<int>(languages.size())) {
                config.ai.translate_language = languages[static_cast<std::size_t>(index)];
            }
        }
    }

    // Row two is the key in use and is editable: a value that is not among the stored ones is a
    // new key and is added (SuMail's box adds one the same way); an unchanged one is left alone,
    // so the same key is never stored twice.
    const std::string typed = edit_text_utf8(hAiKey_);
    if (!typed.empty()) {
        bool known = false;
        for (const std::string& stored : ai_keys_) {
            std::string plain;
            if (ai::unprotect_secret(stored, &plain) && plain == typed) {
                known = true;
                break;
            }
        }
        if (!known) {
            const std::string protected_key = ai::protect_secret(typed);
            if (!protected_key.empty()) ai_keys_.push_back(protected_key);
        }
    }
    // The list picks which key is tried first (the ones after it are the fallbacks), as SuMail's
    // key drop-down does.
    const int chosen = combo_index(hAiKeyList_);
    if (chosen > 0 && chosen < static_cast<int>(ai_keys_.size())) {
        std::rotate(ai_keys_.begin(), ai_keys_.begin() + chosen, ai_keys_.end());
    }
    config.ai.keys = ai_keys_;
    refresh_ai_key_list();
}

void EditorApp::apply_ai_provider_defaults() {
    if (!hAiProvider_) return;
    const std::vector<ai::Provider>& providers = ai::providers();
    const int index = combo_index(hAiProvider_);
    if (index < 0 || index >= static_cast<int>(providers.size())) return;
    const ai::Provider& provider = providers[static_cast<std::size_t>(index)];
    // What is shown is what runs: the service's own address and model (both empty for "custom").
    // Keys belong to the service they were made for: keep this one, bring up the other.
    config_.ai.keys_by_provider[config_.ai.provider] = ai_keys_;
    config_.ai.provider = provider.id;
    const auto for_new = config_.ai.keys_by_provider.find(config_.ai.provider);
    ai_keys_ = for_new != config_.ai.keys_by_provider.end() ? for_new->second
                                                          : std::vector<std::string>{};
    refresh_ai_key_list();
    set_combo_text(hAiBaseUrl_, provider.base_url);
    SendMessageW(hAiModel_, CB_RESETCONTENT, 0, 0);
    if (provider.model[0] != '\0') {
        const std::wstring wide = utf8_to_wstr(provider.model);
        SendMessageW(hAiModel_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
        SendMessageW(hAiModel_, CB_SETCURSEL, 0, 0);
    } else {
        set_combo_text(hAiModel_, std::string());
    }
    SetWindowTextW(hAiModelsHint_, tr("ai.base_url_hint"));
}

// The address names the service: select it (and with it its own address and model). This is what
// SuMail's "detect" does for an address pasted from a gateway.
void EditorApp::detect_ai_provider() {
    if (!hAiBaseUrl_) return;
    const std::string address = edit_text_utf8(hAiBaseUrl_);
    if (address.empty()) return;
    const std::vector<ai::Provider>& providers = ai::providers();
    for (std::size_t i = 0; i < providers.size(); ++i) {
        const std::string known = providers[i].base_url;
        if (known.empty()) continue;
        if (address.find(known) != std::string::npos || known.find(address) != std::string::npos) {
            combo_set_index(hAiProvider_, static_cast<int>(i));
            set_combo_text(hAiBaseUrl_, known);
            SendMessageW(hAiModel_, CB_RESETCONTENT, 0, 0);
            const std::wstring wide = utf8_to_wstr(providers[i].model);
            SendMessageW(hAiModel_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide.c_str()));
            SendMessageW(hAiModel_, CB_SETCURSEL, 0, 0);
            SetWindowTextW(hAiModelsHint_,
                           (std::wstring(tr("ai.detected")) + L" " +
                            utf8_to_wstr(providers[i].name))
                               .c_str());
            return;
        }
    }
    SetWindowTextW(hAiModelsHint_, tr("ai.detect_failed"));
}

void EditorApp::fetch_ai_models() {
    if (!hAiModel_) return;
    Config candidate = config_;
    read_ai(candidate);
    if (candidate.ai.keys.empty()) {
        SetWindowTextW(hAiModelsHint_, tr("ai.test_no_key"));
        return;
    }
    SetWindowTextW(hAiModelsHint_, tr("ai.getting_models"));
    UpdateWindow(hAiModelsHint_);

    // Bounded by ai.timeout_ms, on this thread (the hourglass meanwhile), as the test is.
    WaitCursor wait;
    std::vector<std::string> models;
    std::string error;
    if (!ai::list_models(candidate.ai, &models, &error, nullptr) || models.empty()) {
        const std::string message = error.empty() ? ai::last_error() : error;
        SetWindowTextW(
            hAiModelsHint_,
            (std::wstring(tr("ai.get_models_failed")) + L" " + utf8_to_wstr(message)).c_str());
        return;
    }
    wchar_t current[256] = {};
    GetWindowTextW(hAiModel_, current, static_cast<int>(std::size(current)));
    fill_model_combo(hAiModel_, models, wstr_to_utf8(current));
    SetWindowTextW(hAiModelsHint_, with_number(tr("ai.models_found"), models.size()).c_str());
    refresh_ai_effort_combo();  // the model changed: its levels may differ
}

void EditorApp::run_ai_test() {
    if (!hAiTestResult_) return;
    Config candidate = config_;
    read_ai(candidate);
    if (candidate.ai.keys.empty()) {
        SetWindowTextW(hAiTestResult_, tr("ai.test_no_key"));
        return;
    }
    SetWindowTextW(hAiTestResult_, tr("ai.testing"));
    UpdateWindow(hAiTestResult_);

    WaitCursor wait;
    std::string out;
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    const bool ok =
        ai::translate(candidate.ai, "今天天气很好，我们去公园散步吧。", "en", &out, &error, nullptr);
    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
    // "1234 ms · 成功：…": how long it took first, then what came back.
    const std::wstring took = std::to_wstring(elapsed) + L" ms \u00B7 ";
    if (ok) {
        SetWindowTextW(hAiTestResult_,
                       (took + tr("ai.test_ok") + L" " + utf8_to_wstr(out)).c_str());
    } else {
        const std::string message = error.empty() ? ai::last_error() : error;
        SetWindowTextW(hAiTestResult_,
                       (took + tr("ai.test_failed") + L" " + utf8_to_wstr(message)).c_str());
    }
    // Only the key list: populate_ai() would reset every control from the saved configuration and
    // throw away the ticks and text the user has not applied yet.
    refresh_ai_key_list();
}

// Row two shows the value of the key the list has selected (dots unless revealed).
void EditorApp::show_ai_key_value() {
    if (!hAiKey_) return;
    const int selected = hAiKeyList_ ? combo_index(hAiKeyList_) : 0;
    std::string plain;
    if (selected >= 0 && selected < static_cast<int>(ai_keys_.size())) {
        cxxime::ai::unprotect_secret(ai_keys_[static_cast<std::size_t>(selected)], &plain);
    }
    SetWindowTextW(hAiKey_, utf8_to_wstr(plain).c_str());
}

// The reasoning levels this service and model offer go into the combo (SuMail takes them from the
// engine the same way); a service that offers none leaves it greyed out.
void EditorApp::refresh_ai_effort_combo() {
    if (!hAiEffort_) return;
    const std::vector<ai::Provider>& providers = ai::providers();
    const int selected = combo_index(hAiProvider_);
    const std::string provider = selected >= 0 && selected < static_cast<int>(providers.size())
                                     ? providers[static_cast<std::size_t>(selected)].id
                                     : config_.ai.provider;
    wchar_t model[256] = {};
    GetWindowTextW(hAiModel_, model, static_cast<int>(std::size(model)));
    ai_effort_levels_ = ai::thinking_levels(provider, wstr_to_utf8(model));

    SendMessageW(hAiEffort_, CB_RESETCONTENT, 0, 0);
    combo_add(hAiEffort_, tr("ai.effort_default"));
    int index = 0;
    for (std::size_t i = 0; i < ai_effort_levels_.size(); ++i) {
        combo_add(hAiEffort_, utf8_to_wstr(ai_effort_levels_[i]).c_str());
        if (config_.ai.reasoning_effort == ai_effort_levels_[i]) index = static_cast<int>(i) + 1;
    }
    combo_set_index(hAiEffort_, index);
    EnableWindow(hAiEffort_, ai_effort_levels_.empty() ? FALSE : TRUE);
    EnableWindow(hAiThinking_, ai_effort_levels_.empty() ? FALSE : TRUE);
}
bool EditorApp::handle_ai_command(int control_id, int notification) {
    switch (control_id) {
    case kAiTestId:
        if (notification == BN_CLICKED) run_ai_test();
        return true;
    case kAiModelsId:
        if (notification == BN_CLICKED) fetch_ai_models();
        return true;
    case kAiDetectId:
        if (notification == BN_CLICKED) detect_ai_provider();
        return true;
    case kAiModelId:
        // Another model: its levels may differ.
        if (notification == CBN_SELCHANGE || notification == CBN_EDITCHANGE) {
            refresh_ai_effort_combo();
        }
        return false;
    case kAiProviderId:
        // Another service: fill the address and the model with its own. The drop-down's own
        // selection is left alone (populate_ai would set it straight back).
        if (notification == CBN_SELCHANGE) apply_ai_provider_defaults();
        return true;
    case kAiThinkingId:
        // The level is meaningless without it: grey it out.
        if (notification == BN_CLICKED) EnableWindow(hAiEffort_, get_check(hAiThinking_) ? TRUE : FALSE);
        return true;
    case kAiRevealId: {
        // The eye: show what is being typed, put it back to dots on the next press.
        if (notification != BN_CLICKED) return true;
        static bool revealed = false;
        revealed = !revealed;
        SendMessageW(hAiKey_, EM_SETPASSWORDCHAR,
                     revealed ? 0 : static_cast<WPARAM>(L'\u25CF'), 0);
        SetWindowTextW(hAiKeyReveal_, revealed ? tr("ai.key_hide") : tr("ai.key_show"));
        InvalidateRect(hAiKey_, nullptr, TRUE);
        return true;
    }
    case kAiKeyAddId: {
        if (notification != BN_CLICKED) return true;
        const std::string typed = edit_text_utf8(hAiKey_);
        if (typed.empty()) return true;
        const std::string stored = ai::protect_secret(typed);
        if (stored.empty()) return true;
        ai_keys_.push_back(stored);
        config_.ai.keys = ai_keys_;
        config_.ai.keys_by_provider[config_.ai.provider] = ai_keys_;
        SetWindowTextW(hAiKey_, L"");
        refresh_ai_key_list();
        return true;
    }
    case kAiKeyListId:
        // Another key picked: row two shows that one's value.
        if (notification == CBN_SELCHANGE) show_ai_key_value();
        return true;
    case kAiKeyRemoveId: {
        if (notification != BN_CLICKED) return true;
        const int selected = combo_index(hAiKeyList_);
        if (selected < 0 || selected >= static_cast<int>(ai_keys_.size())) return true;
        ai_keys_.erase(ai_keys_.begin() + selected);
        config_.ai.keys = ai_keys_;
        config_.ai.keys_by_provider[config_.ai.provider] = ai_keys_;
        config_.ai.keys_by_provider[config_.ai.provider] = ai_keys_;
        // The value box must not keep the removed key's text: reading the page would take it for a
        // new key and store it again.
        SetWindowTextW(hAiKey_, L"");
        refresh_ai_key_list();
        return true;
    }
    default:
        return false;
    }
}

}  // namespace settings
}  // namespace cxxime
