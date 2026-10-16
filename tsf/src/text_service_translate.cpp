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
constexpr const wchar_t kNothingToWorkOn[] =
    L"没有可处理的内容（没选中、光标这一行没字、剪贴板里也没有文本）";
// A program whose text cannot be read, and whose own copy did not come: rather than quietly
// working on whatever the clipboard happens to hold (docs/ai.md), say so and say what to do.
constexpr const wchar_t kNoSelectionRead[] = L"没取到选中的内容：请先自己按一次 Ctrl+C 再按这个键";
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

// The trailing line break taken off the end of `text` (a program that copies the whole line hands
// it over with its break; the answer belongs to the line itself).
std::wstring without_trailing_line_break(const std::wstring& text) {
    std::size_t end = text.size();
    while (end > 0 && (text[end - 1] == L'\n' || text[end - 1] == L'\r')) --end;
    return text.substr(0, end);
}

// The keys below are sent one step at a time, with the program's messages dispatched in between
// (pump_program_messages): the keys sit in this thread's queue until our handler returns (docs/ai.md),
// so a program can only take them one at a time if we look away between steps — which is what makes
// a selection land the way a hand pressing the keys would. How long each step settles for.
constexpr DWORD kKeyStepMs = 120;

// A key as the hardware would send it: with its scan code (MapVirtualKey) and, where the scan code
// asks for it, the extended flag. Measured: a selection made of keys sent as bare virtual keys
// arrives with the Shift ignored — End then only moves the caret, and the answer is inserted after
// it instead of replacing the line.
void fill_key(INPUT* input, WORD virtual_key, bool up) {
    const UINT scan = MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC_EX);
    input->type = INPUT_KEYBOARD;
    input->ki.wVk = 0;
    input->ki.wScan = static_cast<WORD>(scan & 0xFF);
    input->ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
    if ((scan >> 8) == 0xE0) {
        input->ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    input->ki.dwExtraInfo = TextService::kInjectedCopyMark;
}

// While we wait, the program's messages are dispatched — the same idea as wait_for_program_copy.
void pump_program_messages(DWORD milliseconds) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    for (;;) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (GetTickCount64() >= deadline) return;
        MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

// One step of a key sequence: press and release `virtual_key`, then let the program work through it.
void tap_key(WORD virtual_key) {
    INPUT inputs[2] = {};
    fill_key(&inputs[0], virtual_key, false);
    fill_key(&inputs[1], virtual_key, true);
    SendInput(2, inputs, sizeof(INPUT));
    pump_program_messages(kKeyStepMs);
}

void hold_key(WORD virtual_key, bool down) {
    INPUT input = {};
    fill_key(&input, virtual_key, !down);
    SendInput(1, &input, sizeof(INPUT));
    pump_program_messages(kKeyStepMs);
}

// Ctrl+C, the copy that puts a selection on the clipboard (its keys carry the mark ours do).
void ctrl_key_copy() {
    INPUT inputs[4] = {};
    fill_key(&inputs[0], VK_CONTROL, false);
    fill_key(&inputs[1], 'C', false);
    fill_key(&inputs[2], 'C', true);
    fill_key(&inputs[3], VK_CONTROL, true);
    SendInput(4, inputs, sizeof(INPUT));
    pump_program_messages(kKeyStepMs);
}

// Selects the line the caret is on, the way the keys are pressed by hand: Home to the line's start;
// Shift held down while End is pressed, so the selection reaches the line's end; Shift released;
// then Ctrl+C. A program that copies the whole line when nothing is selected hands the answer a
// *line*, and without this the selection would not be the line at all (docs/ai.md).
void select_line_with_keys() {
    tap_key(VK_HOME);
    hold_key(VK_SHIFT, true);
    tap_key(VK_END);
    hold_key(VK_SHIFT, false);
}

// The same, the old way: Ctrl+L — what an editor that copies the line when nothing is selected binds
// "select the line" to (Sublime Text's is exactly this). Only used when the keys above are shown not
// to have selected anything (see line_copy_is_selection).
void select_line_with_ctrl_l() {
    INPUT inputs[4] = {};
    fill_key(&inputs[0], VK_CONTROL, false);
    fill_key(&inputs[1], 'L', false);
    fill_key(&inputs[2], 'L', true);
    fill_key(&inputs[3], VK_CONTROL, true);
    SendInput(4, inputs, sizeof(INPUT));
    pump_program_messages(kKeyStepMs);
}

// An answer delivered the way the user would put it there: on the clipboard, then Ctrl+V, which
// the program applies to its own selection. Only a program whose text the input method cannot read
// needs this — TSF sees no selection in a transitory document, so an ordinary insert would land at
// the caret instead of over what is selected. The keys carry the mark ours do, so the input
// method's own key handling skips them.
void paste_text(const std::wstring& text) {
    put_clipboard_text(text);
    INPUT inputs[4] = {};
    const auto key = [](INPUT* input, WORD virtual_key, DWORD flags) {
        input->type = INPUT_KEYBOARD;
        input->ki.wVk = virtual_key;
        input->ki.dwFlags = flags;
        input->ki.dwExtraInfo = TextService::kInjectedCopyMark;
    };
    key(&inputs[0], VK_CONTROL, 0);
    key(&inputs[1], 'V', 0);
    key(&inputs[2], 'V', KEYEVENTF_KEYUP);
    key(&inputs[3], VK_CONTROL, KEYEVENTF_KEYUP);
    SendInput(4, inputs, sizeof(INPUT));
}

// How long a program's own copy is waited for, in steps, and the wait itself: it *dispatches the
// program's messages* rather than sleeping through them. The keys that ask for the copy are sent
// from this thread — the program's own — so they only sit in its queue until this handler returns:
// without pumping, the program cannot act on them while we are looking, which is exactly how the
// clipboard ended up one press behind (docs/ai.md).
constexpr DWORD kProgramCopyWaitMs = 300;
constexpr DWORD kProgramCopyStepMs = 20;

void wait_for_program_copy(DWORD sequence = 0) {
    const ULONGLONG deadline = GetTickCount64() + kProgramCopyWaitMs;
    for (;;) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (GetTickCount64() >= deadline) return;
        // With a sequence: the copy has come (the caller checks it); without one, wait it out.
        if (sequence != 0 && GetClipboardSequenceNumber() != sequence) return;
        MsgWaitForMultipleObjectsEx(0, nullptr, kProgramCopyStepMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

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

// Selects the caret's line and copies it, and says whether the copy came back as that line — the
// copy is what tells "the line is selected" from "nothing is selected and the program copies the
// whole line anyway": the answer may only be pasted over the line it was made from (docs/ai.md).
// Ctrl+C now, and what it put on the clipboard.
bool copy_now(std::wstring* copied) {
    const DWORD sequence = GetClipboardSequenceNumber();
    ctrl_key_copy();
    wait_for_program_copy(sequence);
    if (GetClipboardSequenceNumber() == sequence) return false;
    return read_clipboard_text(copied);
}

// Whether a selection of `line` is really there, asked by *shrinking* it by one character: told to
// shift one to the left, a selection copies one character less, while a program that copies the whole
// line merely because nothing is selected answers the same copy either way. The shift is undone
// afterwards, so the line ends up selected exactly as it was. This is what the keys are judged by,
// and not the copy alone: in these editors the copy of a line and the copy of a selection of that
// line look the same — both carry the line break (measured in Sublime Text, docs/ai.md).
bool selection_is_the_line(const std::wstring& line) {
    std::wstring before;
    if (!copy_now(&before) || without_trailing_line_break(before) != line) return false;
    hold_key(VK_SHIFT, true);
    tap_key(VK_LEFT);
    hold_key(VK_SHIFT, false);
    std::wstring shrunk;
    const bool copied = copy_now(&shrunk);
    hold_key(VK_SHIFT, true);
    tap_key(VK_RIGHT);
    hold_key(VK_SHIFT, false);
    if (!copied || before.empty()) return false;
    return shrunk.size() + 1 == before.size() &&
           shrunk.compare(0, shrunk.size(), before, 0, shrunk.size()) == 0;
}

bool select_and_copy_line(ITfContext* context, TfClientId client_id, const std::wstring& line,
                          const char** how_out) {
    static const char* const kHow[] = {"none", "home_tsf", "keys", "ctrl_l"};
    if (how_out) *how_out = kHow[0];
    // Home first — a plain key, which a program that ignores a synthesized Shift still answers to —
    // and then TSF selecting the line from there: a real document selection, the same thing a
    // readable document gets. Nothing is taken on trust: select_length_at_caret reads the selection
    // back and compares positions, so a program that ignored the write is caught before anything is
    // pasted (docs/ai.md).
    if (context) {
        tap_key(VK_HOME);
        if (select_length_at_caret(context, client_id, line.size())) {
            if (how_out) *how_out = kHow[1];
            return true;
        }
    }
    // The keys, judged by shrinking what they selected (selection_is_the_line) — which is also what
    // tells a selection from a program's own line copy.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 0) {
            select_line_with_keys();  // Home, Shift+End
        } else {
            select_line_with_ctrl_l();  // what these editors bind "select the line" to
        }
        if (selection_is_the_line(line)) {
            if (how_out) *how_out = kHow[attempt + 2];
            return true;
        }
    }
    return false;
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

// What the key works on (docs/ai.md), up to translate.max_selection_chars, in that order:
//   1. the selection;
//   2. with nothing selected, what is written on the caret's line so far (its start to the caret),
//      which select_line_at_caret() selects for the user so that what is about to be handled is on
//      screen — and which is put on the clipboard, since the key copies for the user;
//   3. only when neither has anything: **the clipboard**, so what the user copied can be handled in
//      an empty box (or in a program that will not give its text up). Being last is what keeps the
//      same key from handling text the user cannot see — a selection, or the caret's own line,
//      always wins over whatever the clipboard happens to hold.
bool TextService::_read_selected_text(std::wstring* selection, bool* shortened,
                                      const wchar_t** reason, bool the_user_copied) {
    if (shortened) *shortened = false;
    if (reason) *reason = kNothingToWorkOn;
    ITfContext* context = _current_edit_context_for_composition();
    if (!context) return false;
    const std::size_t limit =
        static_cast<std::size_t>((std::max)(100, _config.translate_max_selection_chars));
    std::wstring wanted;
    bool cut = false;
    _textProgramCopiedLine = false;  // decided again below, by what this read finds
    _textLineText.clear();
    const char* tier = "selection";
    TextBeforeCaret read = read_selection_text(context, _clientId, limit, &wanted, &cut);
    if (read == TextBeforeCaret::kRead && wanted.empty()) {
        // Tier 2: the caret's line, from its start to the caret.
        tier = "line";
        read = select_line_at_caret(context, _clientId, limit, &wanted, &cut);
    }
    if (read == TextBeforeCaret::kPrivate) {
        // A password field: the input method is not to look at it, and it will not use the
        // clipboard in its place either (docs/ai.md).
        context->Release();
        return false;
    }
    // A classic Win32 edit box, or a program that keeps its text to itself: it may well have a
    // selection we cannot see, and its own Ctrl+C is the only way to learn what it holds.
    const bool program_copy_first = read != TextBeforeCaret::kRead;
    _readNeededProgramCopy = program_copy_first;
    context->Release();

    if (!wanted.empty()) {
        // Tiers 1-2 gave something. It goes on the clipboard as well — the key copies for the user,
        // but the copy is made here rather than by asking the program for it, because key events of
        // ours would otherwise reach the input method's own handlers and close the menu this opens.
        *selection = wanted;
        put_clipboard_text(*selection);
        _trace_ai_source(tier, *selection);
    } else if (program_copy_first) {
        // The program has the text and its own copy is the only way to it: asked by message first (a
        // classic edit box honours WM_COPY and keeps the selection it has), and by keys only when
        // that changes nothing — keys of ours go through the program's input context, which some
        // editors answer by re-activating it.
        const DWORD sequence = GetClipboardSequenceNumber();
        HWND target = reinterpret_cast<HWND>(_effectiveEditTarget.view_window);
        if (!target || !IsWindow(target)) target = GetFocus();
        if (target && IsWindow(target)) {
            DWORD_PTR result = 0;
            SendMessageTimeoutW(target, WM_COPY, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 200,
                                &result);
        }
        const DWORD after_message = GetClipboardSequenceNumber();
        if (after_message == sequence) {
            _readingSource = true;  // keys arriving while we pump are the program's, not the key's
            press_copy();
            for (DWORD waited = 0; waited < kProgramCopyWaitMs; waited += kProgramCopyStepMs) {
                if (GetClipboardSequenceNumber() != sequence) break;
                wait_for_program_copy();
            }
            _readingSource = false;
        }
        const DWORD after_keys = GetClipboardSequenceNumber();
        if (after_keys != sequence) {
            read_clipboard_text(selection);
            // A program that copies the whole line when nothing is selected (Sublime Text does):
            // that copy *is* the caret's line, break and all, and the answer belongs over that line
            // — so the break is taken off what is worked on, and the program is asked to select the
            // line before the answer is pasted (docs/ai.md). Only a copy of exactly one line is
            // treated this way: with several, the line we would select is not the text we worked on.
            const std::wstring line = without_trailing_line_break(*selection);
            _textProgramCopiedLine =
                line.size() != selection->size() &&
                line.find_first_of(L"\r\n") == std::wstring::npos;
            if (_textProgramCopiedLine) {
                *selection = line;
                _textLineText = line;
            }
        }
        char detail[160] = {};
        snprintf(detail, sizeof(detail), "program_copy seq=%u message=%u keys=%u chars=%zu",
                 static_cast<unsigned>(sequence), static_cast<unsigned>(after_message),
                 static_cast<unsigned>(after_keys), selection->size());
        _enqueue_event_trace("ai_source", detail);
        if (selection->empty()) {
            if (!the_user_copied) {
                // Its copy did not come. What the clipboard holds is not necessarily what the user
                // means to work on (it may be what they copied minutes ago), so it is refused and
                // said so: quietly handling text the user cannot see is what made this feel chaotic
                // (docs/ai.md).
                if (reason) *reason = kNoSelectionRead;
                return false;
            }
            // The translate key's own first press was the user's copy: that clipboard is theirs.
            if (!read_clipboard_text(selection) || selection->empty()) return false;
            _trace_ai_source("user_copy", *selection);
        }
    } else {
        // Nothing at the caret, in a program whose text could be read: the clipboard, which is what
        // the user copied themselves. Being last is what keeps this key from handling text the user
        // cannot see.
        if (!read_clipboard_text(selection) || selection->empty()) return false;
        _trace_ai_source("clipboard", *selection);
    }
    if (selection->empty()) return false;
    if (selection->size() > limit) {
        selection->resize(limit);
        cut = true;
    }
    if (shortened) *shortened = cut;
    return true;
}

void TextService::_process_selection_translate() {
    std::wstring selection;
    bool shortened = false;
    const wchar_t* reason = nullptr;
    // The translate key's contract is two presses of Ctrl+C: the first is the user's own copy, so
    // the clipboard is theirs to work from even in a program the input method cannot read.
    if (!_read_selected_text(&selection, &shortened, &reason, /*the_user_copied=*/true)) {
        _present_failure(reason, std::wstring());
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
    _textProgressShown = false;  // this is the reason, not a job still running
    _present_translate_window(L"无法完成：" + reason, source, std::wstring(), _query_caret_rect());
}

// Which of the three places the text came from, and how much of it: the one thing the log needs to
// tell "it worked on my selection" from "it worked on something I cannot see" (docs/ai.md).
void TextService::_trace_ai_source(const char* tier, const std::wstring& text) {
    char detail[96] = {};
    snprintf(detail, sizeof(detail), "source=%s chars=%zu", tier, text.size());
    _enqueue_event_trace("ai_source", detail);
}

void TextService::_arm_ai_ui_grace() {
    // Long enough to cover the read (a copy, and an editor reacting to it) and short enough that a
    // window never outlives its business by much: the answer's own delivery closes it explicitly.
    _aiUiGraceUntil = GetTickCount64() + 1200;
}

bool TextService::_ai_ui_grace_active() const {
    return GetTickCount64() < _aiUiGraceUntil;
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
    _textProgramOwnsItsText = _readNeededProgramCopy;
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
    if (response.translate_state == cxxime::kTranslateTranslating) {
        // Still working. The window that says so is put back if it was taken down behind our back:
        // some editors do that when their input context moves over the copy that read the text
        // (docs/ai.md), and a window nobody can see is no better than no window at all. Put back
        // *where it is* — nothing of ours moves while the user waits, which reads as a window that
        // cannot sit still.
        if (_textProgressShown && _translateWindow && !_translateWindow->is_visible()) {
            _translateWindow->show();
        }
        return;
    }
    _textProgressShown = false;

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

    // A program that keeps its text to itself (a classic edit box, Sublime Text and the like) has
    // no selection TSF can see, let alone replace: its own copy is the only way to the text, and the
    // answer goes back the same way. Writing *under* a selection cannot be done that way (a paste
    // would throw the selection away), so an action that wants that is only shown there.
    ITfContext* write_context = _current_edit_context_for_composition();
    // Either the read showed the document could not be read at all (the strongest signal: the text
    // came from the program's own copy), or it looks like one of the programs that keeps its text
    // to itself (a transitory document, Sublime Text and the like).
    const bool program_owns_its_text =
        _textProgramOwnsItsText || (write_context && context_is_imm_document(write_context));
    if (write_context) write_context->Release();
    {
        // Which way the answer goes, for whenever it lands somewhere unexpected (docs/ai.md).
        char detail[128] = {};
        snprintf(detail, sizeof(detail), "deliver insert=%d program_owns_its_text=%d context=%d",
                 static_cast<int>(_textInsert), program_owns_its_text ? 1 : 0,
                 write_context ? 1 : 0);
        _enqueue_event_trace("ai_ui", detail);
    }

    // The answer goes in the way the user would put it there — pasted — and the program replaces
    // whatever it has selected: the selection the read left behind, which is the one that was there
    // or the caret's line (docs/ai.md). That is the delivery a program's own undo (Ctrl+Z) takes
    // back like any other edit. A write through TSF is *not* in every program's undo: Win11 Notepad
    // empties the whole document on the Ctrl+Z after one (measured), and undoes a replacement the
    // input method composed over a selection the same way — the input method cannot ask for better,
    // and has no undo of its own.
    // The line the answer was made from has to be the thing the paste replaces: Home selects it,
    // Shift+End extends it to the line's end, and the Ctrl+C that follows says whether the program
    // did it. When the copy is not that line — a program that ignores a synthesized Shift+End copies
    // the whole line simply because nothing is selected — the answer is *shown*, not written over
    // whatever the caret is on (docs/ai.md).
    const bool line_should_be_replaced =
        deliver && _textInsert == cxxime::AiActionInsert::kReplace && _textProgramCopiedLine;
    const char* line_how = nullptr;
    bool line_selected = true;
    if (line_should_be_replaced) {
        ITfContext* line_context = _current_edit_context_for_composition();
        line_selected = select_and_copy_line(line_context, _clientId, _textLineText, &line_how);
        if (line_context) line_context->Release();
        // Which of the three ways selected the line — the one thing the log needs to tell a program
        // that honours TSF's own selection from one that only answers to keys (docs/ai.md).
        char detail[128] = {};
        snprintf(detail, sizeof(detail), "line_select how=%s selected=%d",
                 line_how ? line_how : "none", line_selected ? 1 : 0);
        _enqueue_event_trace("ai_ui", detail);
        if (!line_selected) {
            // Nothing to replace: the program would not select that line, so the answer is shown
            // with the reason instead of being typed over whatever the caret is on (docs/ai.md).
            note = L"没能选中光标这一行，未替换";
        }
    }

    if (deliver && _textInsert == cxxime::AiActionInsert::kReplace && line_selected) {
        paste_text(translation);
        _hide_translate_window();  // the answer is in: the window has done its job (OnEndEdit
                                   // cannot be relied on for it — see the grace window)
    } else if (deliver && _textInsert == cxxime::AiActionInsert::kBelow && !program_owns_its_text) {
        // Under the selection, on a line of its own (a summary is read under the text it
        // summarizes). The note — only the beginning of a long selection was worked on — rides
        // along as the last line: there is no window here to say it in.
        std::wstring inserted = L"\r\n" + translation;
        if (!note.empty()) inserted += L"\r\n" + note;
        insert_text(inserted, true, nullptr, true, false);
        _hide_translate_window();
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

    // Placed once, where it first appears: a window that moves while the answer is being fetched —
    // or when the answer replaces what it was saying — reads as one that cannot sit still
    // (docs/ai.md).
    if (!_translateWindow->is_visible()) _translateWindow->move_to_caret(caret);
    _translateWindow->update(page);
    _translateWindow->show();
    _arm_ai_ui_grace();  // the copy that got the text may bounce the edit target just now
    // Where it ended up, for whenever it cannot be seen (docs/ai.md): the owner, the caret it was
    // placed at, whether it came up visible, and the rect it occupies.
    RECT window_rect = {};
    GetWindowRect(_translateWindow->hwnd_for_test(), &window_rect);
    const SIZE layout = _translateWindow->layout_size();
    char detail[256] = {};
    snprintf(detail, sizeof(detail),
             "answer_window owner=%p caret=%ld,%ld,%ld,%ld visible=%d rect=%ld,%ld,%ld,%ld "
             "layout=%ldx%ld chars=%zu",
             static_cast<void*>(owner), caret.left, caret.top, caret.right, caret.bottom,
             _translateWindow->is_visible() ? 1 : 0, window_rect.left, window_rect.top,
             window_rect.right, window_rect.bottom, layout.cx, layout.cy, translation.size());
    _enqueue_event_trace("ai_ui", detail);
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

// How often a held tapped modifier's state is looked at, and how long it may be held and still
// count as a tap (docs/ai.md).
constexpr UINT kAiTapWatchMs = 60;
constexpr ULONGLONG kAiTapMaxHoldMs = 700;

void TextService::_watch_ai_action_tap() {
    const UINT pressed = _aiActionKeyDown;
    if (pressed == 0) {
        if (_configWindow) KillTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_AI_TAP_WATCH);
        return;
    }
    const bool still_held = (GetAsyncKeyState(static_cast<int>(pressed)) & 0x8000) != 0;
    // Held: either a chord is coming (another key cancels it) or it is being held down, which is
    // not a tap. Released: it was one — unless another key came first, which the key callbacks say.
    if (still_held && GetTickCount64() - _aiActionKeyDownTick < kAiTapMaxHoldMs) return;
    const std::string action_id = _aiActionKeyId;
    const bool language = _aiActionKeyLanguage;
    const bool moved = _aiActionKeyMoved;
    _aiActionKeyDown = 0;
    _aiActionKeyId.clear();
    _aiActionKeyLanguage = false;
    _aiActionKeyMoved = false;
    if (_configWindow) KillTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_AI_TAP_WATCH);
    if (moved || still_held) return;
    _fire_ai_action_key(language, action_id);
}

// What a key that has decided to fire does: the language list, or the action (the menu, when the
// id is empty).
void TextService::_fire_ai_action_key(bool language, const std::string& action_id) {
    if (language) {
        _pendingLanguageMenu = true;
    } else {
        _pendingAiActionId = action_id;
    }
    PostMessageW(_configWindow, cxxime_tsf::WM_CXXIME_AI_ACTION_SELECTION, 0, 0);
}

bool TextService::_handle_ai_action_key(WPARAM key) {
    // One press on its own: the text the user selected is read (copied) for them, so there is no
    // pair and no Ctrl+C of their own to press. The key is the menu key, or one action's own key,
    // which runs it without the menu.
    std::string action_id;
    const bool language_key = _is_language_key(key);
    if (!language_key && !_ai_action_key_for(key, &action_id)) return false;
    if (is_tap_key(key)) {
        // A tapped modifier: a Shift that begins a chord (Shift+A) is not a tap, so the decision
        // waits for the key-up, and the application keeps the key meanwhile. Not every program
        // hands the key-up over, so the key's own state is watched as well.
        _aiActionKeyDown = static_cast<UINT>(key);
        _aiActionKeyId = action_id;
        _aiActionKeyLanguage = language_key;
        _aiActionKeyMoved = false;
        _aiActionKeyDownTick = GetTickCount64();
        if (_configWindow) {
            SetTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_AI_TAP_WATCH, kAiTapWatchMs, nullptr);
        }
        return false;
    }
    _aiActionKeyDown = static_cast<UINT>(key);
    _aiActionKeyLanguage = language_key;
    if (language_key) {
        _pendingLanguageMenu = true;
    } else {
        _pendingAiActionId = action_id;
    }
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
    const bool language = _aiActionKeyLanguage;
    const bool moved = _aiActionKeyMoved;
    _aiActionKeyDown = 0;
    _aiActionKeyId.clear();
    _aiActionKeyLanguage = false;
    _aiActionKeyMoved = false;
    if (_configWindow) KillTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_AI_TAP_WATCH);
    if (is_tap_key(pressed)) {
        if (!moved) _fire_ai_action_key(language, action_id);
        return false;  // a modifier is the application's: eating its key-up would leave it stuck
    }
    if (pfEaten) *pfEaten = TRUE;  // a combination we took: its key-up goes with it
    return true;
}

// The key that opens the translate language list, if this is it — the same shape as an action key.
bool TextService::_is_language_key(WPARAM key) const {
    const cxxime::KeyboardShortcut& shortcut = _config.ai.language_key;
    if (!shortcut.enabled() || key != shortcut.virtual_key) return false;
    std::uint32_t modifiers = _get_modifiers() & cxxime::kShortcutModifierMask;
    // A tapped modifier arrives with its own bit already set: that bit does not count.
    if (shortcut.modifiers == 0 && is_tap_key(shortcut.virtual_key)) {
        modifiers &= ~modifier_family_bit(shortcut.virtual_key);
    }
    return modifiers == shortcut.modifiers;
}

// The language list, for the key that asks for it: the very thing \fy does, typed for the user —
// the engine recognizes the command, and the list appears in the candidate window as usual, so the
// keys and the choice are the ones the command already has (docs/ai.md). No modifiers are reported
// for those three keys, whatever the shortcut itself was pressed with.
void TextService::_open_language_list() {
    // \fy is a command typed on its own: mid-word it would be punctuation, not the command.
    if (_composing) return;
    ITfContext* context = _current_edit_context_for_composition();
    if (!context) return;
    _modifierOverride = 0;
    for (const WPARAM key : {static_cast<WPARAM>(VK_OEM_5), static_cast<WPARAM>('F'),
                             static_cast<WPARAM>('Y')}) {
        BOOL eaten = FALSE;
        _ProcessKeyEvent(context, key, 0, &eaten);
    }
    _modifierOverride.reset();
    context->Release();
}

void TextService::_process_ai_action_selection() {
    if (_pendingLanguageMenu) {
        // The language list needs no text to work on, so it opens wherever the key is pressed.
        _pendingLanguageMenu = false;
        _open_language_list();
        return;
    }
    const std::string action_id = _pendingAiActionId;
    _pendingAiActionId.clear();
    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    if (actions.empty()) return;  // no data file, or nothing usable in it
    std::wstring selection;
    bool shortened = false;
    const wchar_t* reason = nullptr;
    if (!_read_selected_text(&selection, &shortened, &reason)) {
        _present_failure(reason, std::wstring());
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
    // A row can be clicked, like a candidate (the window is MA_NOACTIVATE, so the click does not
    // take the focus away from the input box the menu belongs to).
    _aiMenuWindow->set_candidate_selection_callback([this](std::size_t row) {
        if (_aiMenuOpen) _run_ai_action(row);
    });
    _aiMenuWindow->set_layout(_config.layout);
    _aiMenuWindow->set_page_info(1, 1);  // one page: no turning arrows
    _aiMenuWindow->set_preedit(std::string(), size_t{0}, size_t{0}, size_t{0}, size_t{0}, false);
    _aiMenuHighlight = 0;
    _aiMenuOpen = true;
    // The order the translation window uses: place it, fill it, show it.
    const RECT caret = _query_caret_rect();
    _aiMenuWindow->move_to_caret(caret);
    _render_ai_menu();
    _aiMenuWindow->show();
    _arm_ai_ui_grace();  // the copy that got the text may bounce the edit target just now
    RECT window_rect = {};
    GetWindowRect(_aiMenuWindow->hwnd_for_test(), &window_rect);
    char detail[192] = {};
    snprintf(detail, sizeof(detail),
             "menu_window owner=%p caret=%ld,%ld,%ld,%ld visible=%d rect=%ld,%ld,%ld,%ld",
             static_cast<void*>(owner), caret.left, caret.top, caret.right, caret.bottom,
             _aiMenuWindow->is_visible() ? 1 : 0, window_rect.left, window_rect.top,
             window_rect.right, window_rect.bottom);
    _enqueue_event_trace("ai_ui", detail);
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
    _textProgressText = L"正在" + label + L"…";
    _textProgressShown = true;
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
