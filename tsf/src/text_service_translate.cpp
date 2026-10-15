// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Selection translation: the translate key (Ctrl+C+C) takes the text selected in the input box
// (read_selection_text), the server's offline model translates it (IPCCommand::TRANSLATE_SELECTION,
// polled with TRANSLATE_STATUS). The result is shown in a candidate window of its own — the
// translation as the candidate, the selected text as its note — or, with translate.direct_insert,
// typed over the selection.
#include "text_service.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <cxxime/ai_actions.h>
#include <cxxime/candidate_presentation.h>
#include <cxxime/candidate_window.h>
#include <cxxime/ipc_protocol.h>

#include "config_coordinator.h"
#include "edit_session.h"
#include "text_before_caret.h"

namespace {

constexpr UINT kTranslatePollMs = 100;
// One piece of a long text: what fits in the IPC field it travels in (ipc_protocol.h).
constexpr std::size_t kTextChunkBytes = cxxime::kTranslateTextCapacity - 1;

// How many bytes of `text` from `offset` fit in `limit` without ending inside a character. Zero
// only when there is nothing left to send.
std::size_t utf8_piece(const std::string& text, std::size_t offset, std::size_t limit) {
    std::size_t length = (std::min)(text.size() - offset, limit);
    while (length > 0 && offset + length < text.size() &&
           (static_cast<unsigned char>(text[offset + length]) & 0xC0) == 0x80) {
        --length;
    }
    return length;
}

// The note beside the answer when the selection was longer than translate.max_selection_chars and
// only its beginning was worked on. It lands in the window's note line (128 bytes), hence short.
std::wstring shortened_note(std::size_t chars) {
    return L"（只处理了前 " + std::to_wstring(chars) + L" 字）";
}

// Why a key could not do anything (the window says 无法完成：…, docs/ai.md). Nothing to work on is
// the one a user meets most: nothing selected, and the line the caret is on has no text.
constexpr const wchar_t kNothingToWorkOn[] = L"没有选中内容，光标这一行也没有文字";
constexpr const wchar_t kNoServerConnection[] = L"与后台服务的连接断开";

// A bare modifier press is not "another key": it must not close the menu (the user is still
// holding the chord the menu was opened with).
bool is_bare_modifier(WPARAM key) {
    return key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT || key == VK_CONTROL ||
           key == VK_LCONTROL || key == VK_RCONTROL || key == VK_MENU || key == VK_LMENU ||
           key == VK_RMENU || key == VK_LWIN || key == VK_RWIN;
}

// A tapped modifier, as an AI action key may be one (shortcuts.ai_action and the actions' own
// keys): Windows hands these to input methods, Alt it does not.
bool is_tap_key(uint32_t virtual_key) {
    return virtual_key == VK_LSHIFT || virtual_key == VK_RSHIFT || virtual_key == VK_SHIFT ||
           virtual_key == VK_LCONTROL || virtual_key == VK_RCONTROL || virtual_key == VK_CONTROL;
}

// The modifier bit a tap key stands for (0 for anything else).
uint32_t modifier_family_bit(uint32_t virtual_key) {
    if (virtual_key == VK_LSHIFT || virtual_key == VK_RSHIFT || virtual_key == VK_SHIFT) {
        return cxxime::kKeyModifierShift;
    }
    if (virtual_key == VK_LCONTROL || virtual_key == VK_RCONTROL || virtual_key == VK_CONTROL) {
        return cxxime::kKeyModifierControl;
    }
    return 0;
}

// Ctrl+C, pressed for the application (SendInput): what the selection translation leaves to the
// user's own first press is done here, since an AI action key is a single press. Only a program
// whose text cannot be read needs this (see _read_selected_text): the keys carry
// TextService::kInjectedCopyMark, so the input method's own key handling ignores them — without it
// the 'C' would count as another key and close the menu it was about to open.
void press_copy() {
    INPUT inputs[4] = {};
    const auto key = [](INPUT* input, WORD virtual_key, DWORD flags) {
        input->type = INPUT_KEYBOARD;
        input->ki.wVk = virtual_key;
        input->ki.dwFlags = flags;
        input->ki.dwExtraInfo = TextService::kInjectedCopyMark;
    };
    key(&inputs[0], VK_CONTROL, 0);
    key(&inputs[1], 'C', 0);
    key(&inputs[2], 'C', KEYEVENTF_KEYUP);
    key(&inputs[3], VK_CONTROL, KEYEVENTF_KEYUP);
    SendInput(4, inputs, sizeof(INPUT));
}

// What this key press is to work on, put on the clipboard ourselves (an AI action key copies for
// the user, docs/ai.md). The program is not asked to copy: sending it a Ctrl+C would put key
// events of ours in flight, and they would reach the input method's own handlers.
void put_clipboard_text(const std::wstring& text) {
    if (text.empty() || !OpenClipboard(nullptr)) return;
    if (EmptyClipboard()) {
        const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HANDLE memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* target = GlobalLock(memory)) {
                memcpy(target, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory)) memory = nullptr;
            }
            if (memory) GlobalFree(memory);
        }
    }
    CloseClipboard();
}

// How long the application is given to put the copied selection on the clipboard.
constexpr DWORD kClipboardWaitMs = 80;

std::string selection_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length,
                        nullptr, nullptr);
    return out;
}

std::wstring selection_from_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

// The clipboard's text: the input method's own copy of the selection, or — with nothing selected
// (docs/ai.md) — the text the user copied a moment ago. Only the **current** entry is taken: the
// clipboard's own history (Win+V) is never walked, and what is in `text` is replaced rather than
// added to, so this is always exactly the last thing copied, and only if it is text.
bool read_clipboard_text(std::wstring* text) {
    if (!text || !OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* locked = static_cast<const wchar_t*>(GlobalLock(data))) {
            *text = locked;
            GlobalUnlock(data);
            ok = !text->empty();
        }
    }
    CloseClipboard();
    return ok;
}

}  // namespace

bool TextService::_handle_translate_key(WPARAM key) {
    const cxxime::KeyboardShortcut& sc = _config.translate_selection_shortcut;
    if (!sc.enabled() || key != sc.virtual_key) return false;
    if ((_get_modifiers() & cxxime::kShortcutModifierMask) != sc.modifiers) return false;

    constexpr ULONGLONG kDoubleTapMs = 350;
    constexpr ULONGLONG kPairMs = 50;  // OnTestKeyDown and OnKeyDown report one press
    const ULONGLONG now = GetTickCount64();
    if (now - _lastTranslateKeySeen < kPairMs) return false;  // the other half of one press
    _lastTranslateKeySeen = now;

    const bool second =
        _lastTranslateCopyTick != 0 && now - _lastTranslateCopyTick <= kDoubleTapMs;
    if (!second) {
        _lastTranslateCopyTick = now;  // the first press: the application copies
        return false;
    }
    _lastTranslateCopyTick = 0;
    _translateKeyDown = static_cast<UINT>(key);
    // Read the selection and translate outside the key callback: reading it needs a synchronous
    // edit session, which cannot be nested inside one.
    PostMessageW(_configWindow, cxxime_tsf::WM_CXXIME_TRANSLATE_SELECTION, 0, 0);
    return true;
}

// What the key works on (docs/ai.md), up to translate.max_selection_chars: the selection, or with
// nothing selected what is written on the caret's line so far (from its start to the caret, which
// is selected for the user). It is put on the clipboard as it is taken — the key copies for the
// user — and what the clipboard happened to hold *before* is never used: that is what made the
// same key work on text the user could not see.
bool TextService::_read_selected_text(std::wstring* selection, bool* shortened) {
    if (shortened) *shortened = false;
    ITfContext* context = _current_edit_context_for_composition();
    if (!context) return false;
    const std::size_t limit =
        static_cast<std::size_t>((std::max)(100, _config.translate_max_selection_chars));
    // What the key is to work on, decided before anything is copied (docs/ai.md): the selection, or
    // — with nothing selected — what is written on the caret's line so far (from its start to the
    // caret), which select_line_at_caret() selects for the user so that what is about to be handled
    // is on screen.
    std::wstring wanted;
    bool cut = false;
    TextBeforeCaret read = read_selection_text(context, _clientId, limit, &wanted, &cut);
    bool make_the_program_copy = false;
    if (read == TextBeforeCaret::kRead && wanted.empty()) {
        read = select_line_at_caret(context, _clientId, limit, &wanted, &cut);
    } else if (read != TextBeforeCaret::kRead) {
        // A classic Win32 edit box, or a program that keeps its text to itself: it may well have a
        // selection we cannot see, and its own Ctrl+C is the only way to learn what it holds.
        read = TextBeforeCaret::kRead;
        cut = false;
        make_the_program_copy = true;
    }
    context->Release();
    if (read != TextBeforeCaret::kRead) return false;

    if (make_the_program_copy) {
        const DWORD sequence = GetClipboardSequenceNumber();
        press_copy();
        Sleep(kClipboardWaitMs);
        if (GetClipboardSequenceNumber() == sequence || !read_clipboard_text(selection) ||
            selection->empty()) {
            return false;  // it copied nothing: there was no selection to copy
        }
    } else {
        // What was read is what gets handled, and it goes on the clipboard as well — the copy is
        // the point of the key (the user should not have to press Ctrl+C themselves), but it is
        // done here rather than by asking the program for it, because key events of ours would
        // otherwise reach the input method's own handlers and close the menu this opens.
        *selection = wanted;
        put_clipboard_text(*selection);
    }
    if (selection->size() > limit) {
        selection->resize(limit);
        cut = true;
    }
    if (shortened) *shortened = cut;
    return !selection->empty();
}

void TextService::_process_selection_translate() {
    std::wstring selection;
    bool shortened = false;
    if (!_read_selected_text(&selection, &shortened)) {
        _present_failure(kNothingToWorkOn, std::wstring());
        return;
    }
    const std::string text = selection_to_utf8(selection);
    if (text.empty()) {
        _present_failure(kNothingToWorkOn, std::wstring());
        return;
    }
    // Only part of a shortened selection was worked on: typing the answer back would throw the
    // rest of the selection away, so it is shown instead.
    const cxxime::AiActionInsert insert =
        _config.translate_direct_insert && !shortened ? cxxime::AiActionInsert::kReplace
                                                     : cxxime::AiActionInsert::kPreview;
    if (!_send_selected_text(cxxime::kTextUploadTranslate, std::string(), text, selection, insert,
                             shortened ? shortened_note(selection.size()) : std::wstring())) {
        _present_failure(kNoServerConnection, selection);
    }
}

// The window a key that could not do anything shows (docs/ai.md): the reason, and — when there is
// one — the text it was about. Doing nothing in silence is what makes a key look broken.
void TextService::_present_failure(const std::wstring& reason, const std::wstring& source) {
    _present_translate_window(L"无法完成：" + reason, source, std::wstring(), _query_caret_rect());
}

// Sends the selected text for `purpose`: the short command when it fits the IPC field (the path
// the selection translation has always used), the chunked one otherwise. The result is polled the
// same way either way (see _poll_translation).
bool TextService::_send_selected_text(std::uint32_t purpose, const std::string& action_id,
                                      const std::string& text, const std::wstring& source,
                                      cxxime::AiActionInsert insert, const std::wstring& note) {
    if (!_ensure_ipc_session()) return false;
    const bool long_text = text.size() > kTextChunkBytes;
    _textPurpose = purpose;
    _textActionId = action_id;
    // The answer comes back in pieces whenever the chunked channel carried the text — and an AI
    // action always uses it (only the translate command has an answer of its own in the poll).
    _textPieces = long_text || purpose != cxxime::kTextUploadTranslate;
    _textInsert = insert;
    _textNote = note;
    _translateSource = source;

    cxxime::IPCResponse response = {};
    if (long_text) {
        if (!_client.text_upload_begin(_sessionId, purpose, action_id, text.size(), response)) {
            return false;
        }
        for (std::size_t offset = 0; offset < text.size();) {
            const std::size_t length = utf8_piece(text, offset, kTextChunkBytes);
            if (length == 0) return false;
            if (!_client.text_upload_chunk(_sessionId, purpose, offset, text.substr(offset, length),
                                           response)) {
                return false;
            }
            offset += length;
        }
    } else if (purpose == cxxime::kTextUploadTranslate) {
        if (!_client.translate_selection(_sessionId, text, response)) return false;
    } else {
        // An AI action always goes through the chunked channel: one piece, and the server starts
        // as soon as it has arrived.
        if (!_client.text_upload_begin(_sessionId, purpose, action_id, text.size(), response) ||
            !_client.text_upload_chunk(_sessionId, purpose, 0, text, response)) {
            return false;
        }
    }
    _start_translation_poll();
    return true;
}

void TextService::_start_translation_poll() {
    if (!_configWindow) return;
    _translationTargetGeneration = _uiTargetGeneration;
    if (!_translationPolling) {
        _translationPolling = SetTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_TRANSLATE_POLL,
                                       kTranslatePollMs, nullptr) != 0;
    }
}

void TextService::_stop_translation_poll() {
    if (!_translationPolling) return;
    _translationPolling = false;
    if (_configWindow) KillTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_TRANSLATE_POLL);
}

void TextService::_poll_translation() {
    if (!_translationPolling) return;
    // Another input box (or none): the request is dropped.
    if (!_activated || !_sessionId || !_inputFocused ||
        _uiTargetGeneration != _translationTargetGeneration) {
        _stop_translation_poll();
        return;
    }
    cxxime::IPCResponse response = {};
    const bool polled = _textPieces ? _client.text_result_poll(_sessionId, _textPurpose, response)
                                    : _client.translate_status(_sessionId, response);
    if (!polled) {
        _stop_translation_poll();
        return;
    }
    if (response.translate_state == cxxime::kTranslateTranslating) return;

    _stop_translation_poll();
    const bool failed = response.translate_state == cxxime::kTranslateFailed;
    // kTranslateIdle: the service has nothing for us at all — it never got the request, or gave it
    // up without an answer (translate_service.cc). That is as much a dead end as a failure, and it
    // must not leave the window saying 正在… for ever.
    const bool idle = response.translate_state == cxxime::kTranslateIdle;
    if (response.translate_state != cxxime::kTranslateDone && !failed && !idle) return;
    const bool answered = !failed && !idle;

    // The answer: one field (the translate command), or as many pieces as the server says are
    // ready.
    std::string answer;
    if (answered && _textPieces) {
        const std::size_t total = response.text_result_bytes;
        for (std::size_t offset = 0; offset < total;) {
            cxxime::IPCResponse piece = {};
            if (!_client.text_result_read(_sessionId, _textPurpose, offset, piece)) break;
            const std::size_t length =
                strnlen_s(piece.translate_text, sizeof(piece.translate_text));
            if (length == 0) break;
            answer.append(piece.translate_text, length);
            offset += length;
        }
    } else if (answered) {
        const std::size_t length =
            strnlen_s(response.translate_text, sizeof(response.translate_text));
        answer.assign(response.translate_text, length);
    }
    std::wstring translation = selection_from_utf8(answer);
    std::wstring note = _textNote;
    // No answer at all, or a reason in its place: the text is not an answer, so it is shown and
    // never typed over the selection.
    const bool no_answer = translation.empty();
    if (!answered || no_answer) {
        translation = no_answer ? std::wstring(L"无法完成：服务没有返回结果")
                                : L"无法完成：" + translation;
        note.clear();
    }
    const bool deliver = answered && !no_answer;

    if (deliver && _textInsert == cxxime::AiActionInsert::kReplace) {
        insert_text(translation, true);  // replaces the selection
    } else if (deliver && _textInsert == cxxime::AiActionInsert::kBelow) {
        // Under the selection, on a line of its own (a summary is read under the text it
        // summarizes). The note — only the beginning of a long selection was worked on — rides
        // along as the last line: there is no window here to say it in.
        std::wstring inserted = L"\r\n" + translation;
        if (!note.empty()) inserted += L"\r\n" + note;
        insert_text(inserted, true, nullptr, true);
    } else {
        // The cached caret may be stale (nothing typed): ask for it now, like the candidate
        // window does, so the window opens at the selection.
        RECT caret = _caretRect;
        if (ITfContext* context = _current_edit_context_for_composition()) {
            EditSession* session = new (std::nothrow) EditSession(this, context);
            if (session) {
                session->set_action(EditSession::Action::QUERY_CARET);
                HRESULT edit_result = E_FAIL;
                if (SUCCEEDED(context->RequestEditSession(
                        _clientId, session, TF_ES_READ | TF_ES_SYNC, &edit_result)) &&
                    SUCCEEDED(edit_result)) {
                    RECT queried = {};
                    if (session->get_caret_rect(queried)) caret = queried;
                }
                session->Release();
            }
            context->Release();
        }
        _present_translate_window(translation, _translateSource, note, caret);
    }
}

void TextService::_present_translate_window(const std::wstring& translation,
                                            const std::wstring& source, const std::wstring& note,
                                            const RECT& caret) {
    HWND owner = reinterpret_cast<HWND>(_effectiveEditTarget.view_window);
    if (!owner || !IsWindow(owner)) owner = GetFocus();
    if (!owner || !IsWindow(owner)) return;

    if (!_translateWindow) {
        _translateWindow = std::make_unique<cxxime::CandidateWindow>();
        if (!_translateWindow->create(owner, _config) ||
            !_translateWindow->owner_matches(owner)) {
            _translateWindow.reset();
            return;
        }
    } else if (!_translateWindow->ensure_created(owner)) {
        return;
    }
    _translateWindow->set_layout(_config.layout);
    _translateWindow->set_page_info(1, 1);
    _translateWindow->set_preedit(std::string(), size_t{0}, size_t{0}, size_t{0}, size_t{0}, false);

    cxxime::CandidatePresentationPage page;
    page.page_size = 1;
    page.highlighted = 0;
    cxxime::CandidatePresentationItem item;
    item.text = selection_to_utf8(translation);
    // The note (a shortened selection…) matters more than the source, which is still in front of
    // the user; the note line is a candidate gloss, so it is only one of the two.
    item.gloss = selection_to_utf8(note.empty() ? source : note);
    page.items.push_back(std::move(item));

    _translateWindow->move_to_caret(caret);
    _translateWindow->update(page);
    _translateWindow->show();
}

void TextService::_hide_translate_window() {
    if (_translateWindow) _translateWindow->hide();
}

// ---- AI actions (docs/ai.md, ai_actions.h) ------------------------------------------------------

RECT TextService::_query_caret_rect() {
    RECT caret = _caretRect;
    if (ITfContext* context = _current_edit_context_for_composition()) {
        EditSession* session = new (std::nothrow) EditSession(this, context);
        if (session) {
            session->set_action(EditSession::Action::QUERY_CARET);
            HRESULT edit_result = E_FAIL;
            if (SUCCEEDED(context->RequestEditSession(_clientId, session,
                                                      TF_ES_READ | TF_ES_SYNC, &edit_result)) &&
                SUCCEEDED(edit_result)) {
                RECT queried = {};
                if (session->get_caret_rect(queried)) caret = queried;
            }
            session->Release();
        }
        context->Release();
    }
    return caret;
}

bool TextService::_handle_ai_action_key(WPARAM key) {
    // One press on its own: the text the user selected is read (copied) for them, so there is no
    // pair and no Ctrl+C of their own to press. The key is the menu key, or one action's own key,
    // which runs it without the menu.
    std::string action_id;
    if (!_ai_action_key_for(key, &action_id)) return false;
    if (is_tap_key(key)) {
        // A tapped modifier: a Shift that begins a chord (Shift+A) is not a tap, so the decision
        // waits for the key-up, and the application keeps the key meanwhile.
        _aiActionKeyDown = static_cast<UINT>(key);
        _aiActionKeyId = action_id;
        _aiActionKeyMoved = false;
        return false;
    }
    _aiActionKeyDown = static_cast<UINT>(key);
    _pendingAiActionId = action_id;
    // Reading the selection and placing the menu need an edit session and the caret, which cannot
    // be done inside a key callback: the message goes to the config window, as the translation's
    // does.
    PostMessageW(_configWindow, cxxime_tsf::WM_CXXIME_AI_ACTION_SELECTION, 0, 0);
    return true;
}

// Which AI action `key` is, if it is one: the menu key (an empty id) or an action's own key.
bool TextService::_ai_action_key_for(WPARAM key, std::string* action_id) const {
    const std::uint32_t held = _get_modifiers() & cxxime::kShortcutModifierMask;
    const auto matches = [&](const cxxime::KeyboardShortcut& shortcut) {
        if (!shortcut.enabled() || key != shortcut.virtual_key) return false;
        // A tapped modifier arrives with its own bit already set: that bit does not count.
        std::uint32_t modifiers = held;
        if (shortcut.modifiers == 0 && is_tap_key(shortcut.virtual_key)) {
            modifiers &= ~modifier_family_bit(shortcut.virtual_key);
        }
        return modifiers == shortcut.modifiers;
    };
    if (matches(_config.ai.action_key)) {
        action_id->clear();
        return true;
    }
    for (const auto& entry : _config.ai.action_keys) {
        cxxime::KeyboardShortcut shortcut;
        if (!cxxime::parse_ai_action_key(entry.second, &shortcut)) continue;
        if (!matches(shortcut)) continue;
        *action_id = entry.first;
        return true;
    }
    return false;
}

// The key-up of an AI action key: a tapped modifier fires here, when no other key came in between.
bool TextService::_finish_ai_action_key(WPARAM key, BOOL* pfEaten) {
    if (_aiActionKeyDown == 0 || key != _aiActionKeyDown) return false;
    const UINT pressed = _aiActionKeyDown;
    const std::string action_id = _aiActionKeyId;
    const bool moved = _aiActionKeyMoved;
    _aiActionKeyDown = 0;
    _aiActionKeyId.clear();
    _aiActionKeyMoved = false;
    if (is_tap_key(pressed)) {
        if (!moved) {
            _pendingAiActionId = action_id;
            PostMessageW(_configWindow, cxxime_tsf::WM_CXXIME_AI_ACTION_SELECTION, 0, 0);
        }
        return false;  // a modifier is the application's: eating its key-up would leave it stuck
    }
    if (pfEaten) *pfEaten = TRUE;  // a combination we took: its key-up goes with it
    return true;
}

void TextService::_process_ai_action_selection() {
    const std::string action_id = _pendingAiActionId;
    _pendingAiActionId.clear();
    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    if (actions.empty()) return;  // no data file, or nothing usable in it
    std::wstring selection;
    bool shortened = false;
    if (!_read_selected_text(&selection, &shortened)) {
        _present_failure(kNothingToWorkOn, std::wstring());
        return;
    }
    if (action_id.empty()) {
        _open_ai_menu(selection, shortened);
        return;
    }
    // An action's own key: straight to it, no menu.
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].id != action_id) continue;
        _start_ai_action(i, selection, shortened);
        return;
    }
}

void TextService::_open_ai_menu(const std::wstring& source, bool shortened) {
    _close_ai_menu();
    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto hidden = _config.ai.action_disabled.find(actions[i].id);
        if (hidden != _config.ai.action_disabled.end() && hidden->second) continue;
        _aiMenuRows.push_back(i);
    }
    if (_aiMenuRows.empty()) return;
    _aiMenuSource = source;
    _aiMenuShortened = shortened;
    if (!_show_menu_window()) _close_ai_menu();
}

// The menu window: created (or reused), placed at the caret, filled with the current rows, shown.
bool TextService::_show_menu_window() {
    HWND owner = reinterpret_cast<HWND>(_effectiveEditTarget.view_window);
    if (!owner || !IsWindow(owner)) owner = GetFocus();
    if (!owner || !IsWindow(owner)) return false;
    if (!_aiMenuWindow) {
        _aiMenuWindow = std::make_unique<cxxime::CandidateWindow>();
        if (!_aiMenuWindow->create(owner, _config) || !_aiMenuWindow->owner_matches(owner)) {
            _aiMenuWindow.reset();
            return false;
        }
    } else if (!_aiMenuWindow->ensure_created(owner)) {
        return false;
    }
    _aiMenuWindow->set_layout(_config.layout);
    _aiMenuWindow->set_page_info(1, 1);  // one page: no turning arrows
    _aiMenuWindow->set_preedit(std::string(), size_t{0}, size_t{0}, size_t{0}, size_t{0}, false);
    _aiMenuHighlight = 0;
    _aiMenuOpen = true;
    // The order the translation window uses: place it, fill it, show it.
    _aiMenuWindow->move_to_caret(_query_caret_rect());
    _render_ai_menu();
    _aiMenuWindow->show();
    return true;
}

void TextService::_render_ai_menu() {
    if (!_aiMenuOpen || !_aiMenuWindow) return;
    cxxime::CandidatePresentationPage page;
    page.page_size = static_cast<uint32_t>(_aiMenuRows.size());
    page.highlighted = static_cast<int>(_aiMenuHighlight);
    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    for (std::size_t row : _aiMenuRows) {
        cxxime::CandidatePresentationItem item;
        if (row < actions.size()) {
            item.text = cxxime::ai_action_label(actions[row], _config.ui_language);
        }
        page.items.push_back(std::move(item));
    }
    _aiMenuWindow->update(page);
}

void TextService::_close_ai_menu() {
    _aiMenuOpen = false;
    _aiMenuRows.clear();
    _aiMenuHighlight = 0;
    _aiMenuSource.clear();
    _aiMenuShortened = false;
    if (_aiMenuWindow) _aiMenuWindow->hide();
}

// A key while the menu is open: the menu's own keys are taken, anything else closes it and stays
// the application's.
bool TextService::_handle_ai_menu_key(WPARAM key, BOOL* pfEaten) {
    if (!_aiMenuOpen) return false;
    const std::size_t rows = _aiMenuRows.size();
    const auto taken = [&] {
        if (pfEaten) *pfEaten = TRUE;
        return true;
    };
    if (key >= '1' && key <= '9') {
        const std::size_t row = key - '1';
        if (row < rows) _run_ai_action(row);
        return taken();
    }
    if (key == VK_DOWN || key == VK_TAB) {
        if (rows > 0) _aiMenuHighlight = (_aiMenuHighlight + 1) % rows;
        _render_ai_menu();
        return taken();
    }
    if (key == VK_UP) {
        if (rows > 0) _aiMenuHighlight = (_aiMenuHighlight + rows - 1) % rows;
        _render_ai_menu();
        return taken();
    }
    if (key == VK_RETURN) {
        _run_ai_action(_aiMenuHighlight);
        return taken();
    }
    if (key == VK_ESCAPE) {
        _close_ai_menu();
        return taken();
    }
    if (is_bare_modifier(key)) return false;  // the chord being held: the menu stays
    _close_ai_menu();
    return false;
}

void TextService::_run_ai_action(std::size_t row) {
    if (row >= _aiMenuRows.size()) return;
    const std::size_t index = _aiMenuRows[row];
    const std::wstring source = _aiMenuSource;
    const bool shortened = _aiMenuShortened;
    _close_ai_menu();
    _start_ai_action(index, source, shortened);
}

void TextService::_start_ai_action(std::size_t index, const std::wstring& source,
                                   bool shortened) {
    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    if (index >= actions.size()) return;
    const cxxime::AiAction action = actions[index];
    std::wstring text = source;
    bool cut = shortened;
    // Cut to what this action accepts and say so. A cut selection is never typed back: the answer
    // only covers what was sent, and replacing the whole selection would lose the rest.
    if (action.max_input_chars > 0 &&
        text.size() > static_cast<std::size_t>(action.max_input_chars)) {
        text.resize(static_cast<std::size_t>(action.max_input_chars));
        cut = true;
    }
    const std::wstring note = cut ? shortened_note(text.size()) : std::wstring();
    const RECT caret = _query_caret_rect();
    // Where the answer goes (the action's insert mode, ai_actions.h), except that replacing a
    // selection that was cut would throw the part that was not sent away — that one is shown
    // instead. Writing under the selection loses nothing, so it stays as the action asks.
    cxxime::AiActionInsert insert = action.insert;
    if (insert == cxxime::AiActionInsert::kReplace && cut) {
        insert = cxxime::AiActionInsert::kPreview;
    }

    // Working: the window says so until the poll brings the answer (or the reason it failed).
    const std::wstring label =
        selection_from_utf8(cxxime::ai_action_label(action, _config.ui_language));
    _present_translate_window(L"正在" + label + L"…", text, note, caret);
    // An action with a language list is a translation: it goes to the translation service (online
    // first, the offline model when that fails), and the tag is the language rather than the id.
    const bool translates = !action.languages.empty();
    const std::string tag = translates
                                ? (_config.ai.translate_language.empty() ? action.languages.front()
                                                                        : _config.ai.translate_language)
                                : action.id;
    const std::uint32_t purpose =
        translates ? cxxime::kTextUploadAiTranslate : cxxime::kTextUploadAiAction;
    if (!_send_selected_text(purpose, tag, selection_to_utf8(text), text, insert, note)) {
        _present_failure(kNoServerConnection, text);
    }
}
