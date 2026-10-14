// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The "AI" page (docs/ai.md): the optional online service. Off by default — nothing leaves the
// machine until it is turned on here — and each feature that may use it has its own switch, so
// "let the API translate the selection" does not imply "let it see the voice input".
//
// The address and the model may be left empty: the chosen provider's own are then used, and the
// hint under them says which those are. A key typed into the box is DPAPI-protected
// (ai_client.h) as soon as the page is read and the box is emptied, so the plain key is never
// written anywhere.

#include "editor_app.h"

#include <algorithm>
#include <string>

#include <cxxime/ai_client.h>

#include "editor_app_internal.h"
#include "i18n.h"

namespace cxxime {
namespace settings {

namespace {

// The two modes, in the order of the combo (AiConfig::mode).
const char* const kAiModeKeys[] = {"ai.mode_prefer_remote", "ai.mode_remote_only"};

std::wstring with_number(const wchar_t* text, std::size_t number) {
    return std::wstring(text) + L" " + std::to_wstring(number);
}

}  // namespace

void EditorApp::create_ai_panel(HWND panel) {
    const int x0 = kPanelPadLeft;
    const int labels = label_width({"ai.enabled", "ai.provider", "ai.base_url", "ai.model",
                                    "ai.key", "ai.mode", "ai.timeout", "ai.use_for"});
    int y = card_begin(panel, kPanelPadTop, tr("ai.card_service"));

    const int enabled_x = make_aligned_label(tr("ai.enabled"), x0, labels, y, panel);
    hAiEnabled_ = make_check(kAiEnabledId, tr("ai.enabled_check"), enabled_x, y, S(500), panel);
    y += kRowH;
    hAiAddressHint_ = make_hint(tr("ai.enabled_hint"), enabled_x, y - S(6), S(560), panel, 2);
    y += S(34);

    const int provider_x = make_aligned_label(tr("ai.provider"), x0, labels, y, panel);
    hAiProvider_ = make_combo(kAiProviderId, provider_x, y, S(300), panel);
    for (const ai::Provider& provider : ai::providers()) {
        combo_add(hAiProvider_, utf8_to_wstr(provider.name).c_str());
    }
    y += kRowH;

    const int url_x = make_aligned_label(tr("ai.base_url"), x0, labels, y, panel);
    hAiBaseUrl_ = make_edit(kAiBaseUrlId, url_x, y, S(440), panel);
    y += kRowH;
    make_hint(tr("ai.base_url_hint"), url_x, y - S(6), S(560), panel, 1);
    y += S(24);

    const int model_x = make_aligned_label(tr("ai.model"), x0, labels, y, panel);
    hAiModel_ = make_edit(kAiModelId, model_x, y, S(440), panel);
    y += kRowH;
    make_hint(tr("ai.model_hint"), model_x, y - S(6), S(560), panel, 1);
    y += S(24);

    const int key_x = make_aligned_label(tr("ai.key"), x0, labels, y, panel);
    hAiKey_ = make_edit(kAiKeyId, key_x, y, S(440), panel);
    SendMessageW(hAiKey_, EM_SETPASSWORDCHAR, static_cast<WPARAM>(L'*'), 0);
    y += kRowH;
    hAiKeyHint_ = make_hint(tr("ai.key_hint"), key_x, y - S(6), S(560), panel, 2);
    y += S(34);

    const int mode_x = make_aligned_label(tr("ai.mode"), x0, labels, y, panel);
    hAiMode_ = make_combo(kAiModeId, mode_x, y, S(260), panel);
    for (const char* key : kAiModeKeys) combo_add(hAiMode_, tr(key));
    y += kRowH;
    make_hint(tr("ai.mode_hint"), mode_x, y - S(6), S(560), panel, 2);
    y += S(34);

    const int timeout_x = make_aligned_label(tr("ai.timeout"), x0, labels, y, panel);
    hAiTimeout_ = make_edit(kAiTimeoutId, timeout_x, y, S(90), panel);
    y += kRowH;

    y = card_end(panel, y);
    y = card_begin(panel, y, tr("ai.card_features"));
    const int use_x = make_aligned_label(tr("ai.use_for"), x0, labels, y, panel);
    hAiUseTranslate_ =
        make_check(kAiUseTranslateId, tr("ai.use_translate"), use_x, y, S(560), panel);
    y += kRowH;
    hAiUseSelection_ =
        make_check(kAiUseSelectionId, tr("ai.use_selection"), use_x, y, S(560), panel);
    y += kRowH;
    hAiUseEnglish_ = make_check(kAiUseEnglishId, tr("ai.use_english"), use_x, y, S(560), panel);
    y += kRowH;
    hAiUseVoice_ = make_check(kAiUseVoiceId, tr("ai.use_voice"), use_x, y, S(560), panel);
    y += kRowH;
    hAiUseGlossary_ =
        make_check(kAiUseGlossaryId, tr("ai.use_glossary"), use_x, y, S(560), panel);
    y += kRowH;
    make_hint(tr("ai.use_for_hint"), use_x, y - S(6), S(560), panel, 2);
    y += S(34);

    hAiTest_ = make_button(kAiTestId, tr("ai.test"), use_x, y, S(120), panel);
    // A status line beside the button, like the translator's.
    hAiTestResult_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     use_x + S(132), y + S(5), S(420), S(kFontPt + 8), panel,
                                     nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessageW(hAiTestResult_, WM_SETFONT, reinterpret_cast<WPARAM>(get_font()), TRUE);
    y += kRowH;
    y = card_end(panel, y);
    (void)y;
}

void EditorApp::populate_ai() {
    if (!hAiEnabled_) return;
    ai_keys_ = config_.ai.keys;

    set_check(hAiEnabled_, config_.ai.enabled);

    const std::vector<ai::Provider>& providers = ai::providers();
    int index = 0;
    for (std::size_t i = 0; i < providers.size(); ++i) {
        if (config_.ai.provider == providers[i].id) index = static_cast<int>(i);
    }
    combo_set_index(hAiProvider_, index);

    SetWindowTextW(hAiBaseUrl_, utf8_to_wstr(config_.ai.base_url).c_str());
    SetWindowTextW(hAiModel_, utf8_to_wstr(config_.ai.model).c_str());
    SetWindowTextW(hAiKey_, L"");
    combo_set_index(hAiMode_, config_.ai.mode == "remote_only" ? 1 : 0);
    set_edit_int(hAiTimeout_, config_.ai.timeout_ms);

    set_check(hAiUseTranslate_, config_.ai.use_for.translate);
    set_check(hAiUseSelection_, config_.ai.use_for.selection);
    set_check(hAiUseEnglish_, config_.ai.use_for.english);
    set_check(hAiUseVoice_, config_.ai.use_for.voice);
    set_check(hAiUseGlossary_, config_.ai.use_for.glossary);

    // What the empty address and model mean: the provider's own.
    const std::wstring in_use =
        std::wstring(tr("ai.in_use")) + L" " + utf8_to_wstr(ai::effective_base_url(config_.ai)) +
        L" · " + utf8_to_wstr(ai::effective_model(config_.ai));
    SetWindowTextW(hAiAddressHint_, in_use.c_str());
    SetWindowTextW(hAiKeyHint_, with_number(tr("ai.keys_stored"), ai_keys_.size()).c_str());
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
    config.ai.model = edit_text_utf8(hAiModel_);
    config.ai.mode = combo_index(hAiMode_) == 1 ? "remote_only" : "prefer_remote";
    config.ai.timeout_ms = (std::max)(1000, (std::min)(get_edit_int(hAiTimeout_), 120000));
    config.ai.use_for.translate = get_check(hAiUseTranslate_);
    config.ai.use_for.selection = get_check(hAiUseSelection_);
    config.ai.use_for.english = get_check(hAiUseEnglish_);
    config.ai.use_for.voice = get_check(hAiUseVoice_);
    config.ai.use_for.glossary = get_check(hAiUseGlossary_);

    config.ai.keys = ai_keys_;
    // A key typed here is protected and added once: the box is emptied, so a later read does
    // not add it again, and the plain key is never kept anywhere.
    const std::string typed = edit_text_utf8(hAiKey_);
    if (!typed.empty()) {
        const std::string stored = ai::protect_secret(typed);
        if (!stored.empty()) {
            config.ai.keys.push_back(stored);
            ai_keys_ = config.ai.keys;
            SetWindowTextW(hAiKey_, L"");
        }
    }
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

    // One short request on this thread, bounded by ai.timeout_ms (the hourglass meanwhile).
    WaitCursor wait;
    std::string out;
    std::string error;
    const bool ok =
        ai::translate(candidate.ai, "今天天气很好，我们去公园散步吧。", "en", &out, &error, nullptr);
    if (ok) {
        SetWindowTextW(hAiTestResult_,
                       (std::wstring(tr("ai.test_ok")) + L" " + utf8_to_wstr(out)).c_str());
    } else {
        const std::string message = error.empty() ? ai::last_error() : error;
        SetWindowTextW(hAiTestResult_,
                       (std::wstring(tr("ai.test_failed")) + L" " + utf8_to_wstr(message)).c_str());
    }
    populate_ai();  // the stored key count may have changed
}

bool EditorApp::handle_ai_command(int control_id, int notification) {
    switch (control_id) {
    case kAiTestId:
        if (notification == BN_CLICKED) run_ai_test();
        return true;
    case kAiEnabledId:
    case kAiProviderId:
        // The service row and the key count are worth refreshing after a change.
        if (notification == BN_CLICKED || notification == CBN_SELCHANGE) populate_ai();
        return true;
    default:
        return false;
    }
}

}  // namespace settings
}  // namespace cxxime
