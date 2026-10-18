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
#include "uia_text.h"

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
constexpr DWORD kKeyStepMs = 10;

// A key as real keyboard input: the virtual key, its scan code, and — for the keys that have one —
// the extended flag. Home, End and the arrows need that flag spelled out: their scan codes are the
// same as the numeric keypad's 7, 1, 4 and so on, and MapVirtualKey does not tell the two apart. Sent
// as a bare scan code, Home arrived as the keypad's 7 and typed a "7" into the document (measured).
void fill_key(INPUT* input, WORD virtual_key, bool up) {
    input->type = INPUT_KEYBOARD;
    input->ki.wVk = virtual_key;
    input->ki.wScan = static_cast<WORD>(MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC));
    DWORD flags = up ? KEYEVENTF_KEYUP : 0;
    switch (virtual_key) {
    case VK_HOME:
    case VK_END:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_INSERT:
    case VK_DELETE:
        flags |= KEYEVENTF_EXTENDEDKEY;
        break;
    default:
        break;
    }
    input->ki.dwFlags = flags;
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
// `copy_line` is the table's fifth step (Ctrl+C). It is left out where the answer is pasted right
// after: reading the clipboard in between takes the selection away from the program (measured in VS
// Code: the paste then landed at the caret), and the delivery reads the line back afterwards anyway.
void select_line_with_keys(bool whole_line, bool copy_line) {
    // The line the answer replaces, as the program's own keys. `whole_line`: the caret's whole line —
    // a program that copied it for us says nothing about where the caret sits in it, so the answer
    // covers all of it (the table: Home, Shift held, End, Shift released). Otherwise just the part
    // written before the caret, which Shift+Home takes from the caret (docs/ai.md).
    if (whole_line) {
        tap_key(VK_HOME);
        hold_key(VK_SHIFT, true);
        tap_key(VK_END);
        hold_key(VK_SHIFT, false);
    } else {
        hold_key(VK_SHIFT, true);
        tap_key(VK_HOME);
        hold_key(VK_SHIFT, false);
    }
    if (copy_line) {
        ctrl_key_copy();  // the line on the clipboard, then the answer replaces it
    }
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

// A classic Windows edit control (Edit, RichEdit) answers its own messages: its text can be read and
// a range replaced without a single key of ours, which is the same thing that makes Notepad exact
// (docs/ai.md). Only for controls inside this process — the messages carry buffers and pointers, and
// those cannot cross a process boundary.
bool is_classic_edit_control(HWND control) {
    if (!control || !IsWindow(control)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(control, &pid);
    if (pid != GetCurrentProcessId()) return false;
    wchar_t class_name[64] = {};
    if (!GetClassNameW(control, class_name, ARRAYSIZE(class_name))) return false;
    return _wcsicmp(class_name, L"Edit") == 0 || _wcsicmp(class_name, L"RICHEDIT50W") == 0 ||
           _wcsnicmp(class_name, L"RichEdit", 8) == 0;
}

bool edit_control_text(HWND control, std::wstring* text) {
    const LRESULT length = SendMessageW(control, WM_GETTEXTLENGTH, 0, 0);
    if (length < 0) return false;
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    const LRESULT copied = SendMessageW(control, WM_GETTEXT,
                                        static_cast<WPARAM>(buffer.size()),
                                        reinterpret_cast<LPARAM>(buffer.data()));
    if (copied < 0) return false;
    buffer.resize(static_cast<std::size_t>(copied));
    *text = std::move(buffer);
    return true;
}

void edit_control_selection(HWND control, LONG* start, LONG* end) {
    DWORD from = 0;
    DWORD to = 0;
    SendMessageW(control, EM_GETSEL, reinterpret_cast<WPARAM>(&from),
                 reinterpret_cast<LPARAM>(&to));
    *start = static_cast<LONG>(from);
    *end = static_cast<LONG>(to);
}

// What an action works on, by the same rule as the TSF read: the selection when there is one,
// otherwise the part of the caret's line written before the caret (docs/ai.md).
bool edit_control_range(HWND control, std::wstring* text, LONG* start, LONG* end) {
    std::wstring all;
    if (!edit_control_text(control, &all)) return false;
    LONG from = 0;
    LONG to = 0;
    edit_control_selection(control, &from, &to);
    if (from == to) {
        const LRESULT line = SendMessageW(control, EM_LINEFROMCHAR, static_cast<WPARAM>(to), 0);
        from = static_cast<LONG>(SendMessageW(control, EM_LINEINDEX, static_cast<WPARAM>(line), 0));
    }
    if (from < 0 || to < from || to > static_cast<LONG>(all.size())) return false;
    *text = all.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from));
    *start = from;
    *end = to;
    return !text->empty();
}

// Replaces that range with `answer` through the control's own messages: one ordinary edit, which the
// control's undo (Ctrl+Z) takes back. Refused when the selection is no longer the range the answer
// was made from — the answer must not be written where the user has moved on (docs/ai.md).
bool edit_control_replace(HWND control, LONG start, LONG end, const std::wstring& answer) {
    if (!is_classic_edit_control(control)) return false;
    LONG from = 0;
    LONG to = 0;
    edit_control_selection(control, &from, &to);
    if (from != start || to != end) return false;
    SendMessageW(control, EM_SETSEL, static_cast<WPARAM>(start), static_cast<LPARAM>(end));
    SendMessageW(control, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(answer.c_str()));
    return true;
}

// The same, the answer written under the range on a line of its own (`insert: below`).
bool edit_control_insert_below(HWND control, LONG start, LONG end, const std::wstring& block) {
    if (!is_classic_edit_control(control)) return false;
    LONG from = 0;
    LONG to = 0;
    edit_control_selection(control, &from, &to);
    if (from != start || to != end) return false;
    SendMessageW(control, EM_SETSEL, static_cast<WPARAM>(end), static_cast<LPARAM>(end));
    SendMessageW(control, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(block.c_str()));
    return true;
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

// Selects the caret's line, the way the table says: ① let TSF select it (no keys of ours beyond the
// Home that puts the caret at the line's start, and that selection is read back and checked by
// position inside the same edit session), or ② the five steps — Home, Shift held down, End, Shift
// released, Ctrl+C. The answer is then pasted over what was selected.
//
// The keys are *not* checked with a copy of their own: the Shift+Left/Shift+Right probe that would
// tell "a selection is there" from "nothing is selected" costs two more keystrokes per action, and
// the call here is that the keys' own table is worth trusting (docs/ai.md).

// The way that selected the line in this program, remembered so that the next action can go straight
// to it. The code runs inside the program, so a static map is exactly a per-program cache; it is
// keyed by the class of the focused window, because one program can have more than one kind of
// editor and that class is what tells them apart.
enum { kLineRouteNone = 0, kLineRouteTsf = 1, kLineRouteKeys = 2 };

std::unordered_map<std::string, int>& line_route_cache() {
    static std::unordered_map<std::string, int> cache;
    return cache;
}

std::string focused_class_key() {
    HWND focus = GetFocus();
    wchar_t class_name[64] = {};
    if (!focus || !GetClassNameW(focus, class_name, ARRAYSIZE(class_name))) {
        return std::string("?");
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, class_name, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return std::string("?");
    }
    std::string key(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, class_name, -1, key.data(), required, nullptr, nullptr);
    key.pop_back();  // the terminating NUL
    return key;
}

// One of the two ways. True when it selected the line *without* any keys of ours (the TSF way, which
// is checked as it is set); the key way is the fallback and is not checked.
bool try_line_route(int route, ITfContext* context, TfClientId client_id, const std::wstring& line,
                    bool whole_line, bool readable) {
    if (route == kLineRouteTsf) {
        // Home — a plain key, which a program that ignores a synthesized Shift still answers to —
        // then TSF selecting the line from there: the same thing the read does for a readable
        // document (docs/ai.md).
        if (!context) return false;
        tap_key(VK_HOME);
        return select_length_at_caret(context, client_id, line.size());
    }
    // The keys, and no copy of their own: the answer is pasted straight after, and the delivery reads
    // the line back to see whether it landed (see the paste in _poll_translation, docs/ai.md).
    select_line_with_keys(whole_line, /*copy_line=*/!readable);
    return true;
}

// Selects the line for this program and says whether a route really left it selected. `readable` is
// false for a program whose text cannot be read: there the keys are the only way and nothing can prove
// they took, so their answer is trusted. True means the answer may be pasted; false means the caller
// must write it through TSF instead (docs/ai.md).
bool select_line_for_answer(ITfContext* context, TfClientId client_id, const std::wstring& line,
                            bool whole_line, bool readable, const char** how_out,
                            bool* remembered_out) {
    static const char* const kHow[] = {"none", "tsf", "keys"};
    if (how_out) *how_out = kHow[kLineRouteNone];
    if (remembered_out) *remembered_out = false;
    if (line_route_cache().size() > 64) {
        line_route_cache().clear();  // bounded: the classes this process ever saw
    }
    int& known = line_route_cache()[focused_class_key()];
    int order[2] = {kLineRouteTsf, kLineRouteKeys};
    if (known == kLineRouteTsf || known == kLineRouteKeys) {
        order[0] = known;  // what worked here before, first
        order[1] = known == kLineRouteTsf ? kLineRouteKeys : kLineRouteTsf;
        if (remembered_out) *remembered_out = true;
    }
    for (int route : order) {
        if (route == kLineRouteTsf && readable) {
            continue;  // a readable document: only its own keys make a selection it pastes over
        }
        if (try_line_route(route, context, client_id, line, whole_line, readable)) {
            known = route;
            if (how_out) *how_out = kHow[route];
            return true;
        }
    }
    if (how_out) *how_out = kHow[kLineRouteKeys];
    return false;  // the keys did not take: the caller writes the answer through TSF
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
    _textLineSelected = false;
    _textEditControl = nullptr;
    _textEditStart = 0;
    _textEditEnd = 0;
    _textUiaTarget = false;
    _textUiaText.clear();
    const char* tier = "selection";
    TextBeforeCaret read = read_selection_text(context, _clientId, limit, &wanted, &cut);
    bool line_tier = false;
    if (read == TextBeforeCaret::kRead && wanted.empty()) {
        // Tier 2: the caret's line, from its start to the caret.
        tier = "line";
        line_tier = true;
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

    // A classic edit control hands its text over through its own messages — nothing of ours in the
    // program's input stream, and no clipboard (the Notepad way, docs/ai.md). It comes first: it is
    // exact, it costs no keys, and for those controls the reads above find nothing to read anyway.
    if (program_copy_first) {
        HWND control = GetFocus();
        LONG edit_start = 0;
        LONG edit_end = 0;
        std::wstring edit_text;
        if (is_classic_edit_control(control) &&
            edit_control_range(control, &edit_text, &edit_start, &edit_end)) {
            _textEditControl = control;
            _textEditStart = edit_start;
            _textEditEnd = edit_end;
            wanted = std::move(edit_text);
            tier = "edit_control";
        } else if (uia_read_target(&edit_text)) {
            // A program with a text pattern but no readable document (WinUI, WPF, and the like):
            // UI Automation reads the text and selects it, so the answer is pasted over exactly that
            // (docs/ai.md). Nothing is assumed — the delivery asks again and compares the text.
            _textUiaTarget = true;
            _textUiaText = edit_text;
            wanted = std::move(edit_text);
            tier = "uia";
        }
    }

    if (!wanted.empty()) {
        // Tiers 1-2 gave something. It goes on the clipboard as well — the key copies for the user,
        // but the copy is made here rather than by asking the program for it, because key events of
        // ours would otherwise reach the input method's own handlers and close the menu this opens.
        *selection = wanted;
        put_clipboard_text(*selection);
        _trace_ai_source(tier, *selection);
        if (line_tier) {
            // The read selected the caret's line itself: the answer replaces it, and the delivery
            // selects it again with the program's own keys before pasting (docs/ai.md).
            _textLineSelected = true;
            _textLineText = *selection;
        }
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

// After the paste: does the line the answer was written into start with it? The caret sits right after
// what was pasted, so the same read the action used — the line up to the caret — holds the answer when
// the paste landed, and the *old* text when it did not. That is the one step a paste cannot be trusted
// for: which is why the answer is written through TSF when this says no (docs/ai.md).
int TextService::_line_starts_with(const std::wstring& expected) {
    if (expected.empty()) return -1;
    ITfContext* context = _current_edit_context_for_composition();
    if (!context) return -1;
    std::wstring line;
    bool cut = false;
    const TextBeforeCaret read =
        select_line_at_caret(context, _clientId, expected.size() + 32, &line, &cut);
    context->Release();
    if (read != TextBeforeCaret::kRead || line.empty()) return -1;  // nothing to ask: take it on trust
    const std::size_t head = (std::min)(expected.size(), line.size());
    return head > 0 && line.compare(0, head, expected, 0, head) == 0 ? 1 : 0;
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
        deliver && _textInsert == cxxime::AiActionInsert::kReplace &&
        (_textProgramCopiedLine || _textLineSelected);
    const char* line_how = nullptr;
    if (line_should_be_replaced) {
        bool line_remembered = false;
        ITfContext* line_context = _current_edit_context_for_composition();
        select_line_for_answer(line_context, _clientId, _textLineText, _textProgramCopiedLine,
                               !program_owns_its_text, &line_how, &line_remembered);
        if (line_context) line_context->Release();
        // Which way selected the line, and whether that answer was already known — the one thing the
        // log needs to tell a program that honours TSF's own selection from one that only answers to
        // keys (docs/ai.md). Whether it really is the line, the paste below reads back.
        char detail[160] = {};
        snprintf(detail, sizeof(detail), "line_select how=%s remembered=%d",
                 line_how ? line_how : "none", line_remembered ? 1 : 0);
        _enqueue_event_trace("ai_ui", detail);
    }

    // A classic edit control (Edit, RichEdit) put its text on the clipboard for us through its own
    // messages: the answer goes back the same way — the control's own edit, which its own undo takes
    // back — and nothing of ours reaches the program's input at all (docs/ai.md).
    bool edit_delivered = false;
    if (deliver && _textEditControl) {
        if (_textInsert == cxxime::AiActionInsert::kReplace) {
            edit_delivered =
                edit_control_replace(_textEditControl, _textEditStart, _textEditEnd, translation);
        } else if (_textInsert == cxxime::AiActionInsert::kBelow) {
            std::wstring block = L"\r\n" + translation;
            if (!note.empty()) block += L"\r\n" + note;
            edit_delivered = edit_control_insert_below(_textEditControl, _textEditStart,
                                                       _textEditEnd, block);
        }
        char detail[128] = {};
        snprintf(detail, sizeof(detail), "deliver edit_control=%d", edit_delivered ? 1 : 0);
        _enqueue_event_trace("ai_ui", detail);
    }

    // The same for a program read through UI Automation: the target is selected again and its text
    // compared, and only then is the answer pasted over it (docs/ai.md).
    bool uia_delivered = false;
    if (deliver && _textUiaTarget && _textInsert == cxxime::AiActionInsert::kReplace) {
        uia_delivered = uia_select_if_unchanged(_textUiaText);
        char detail[128] = {};
        snprintf(detail, sizeof(detail), "deliver uia=%d", uia_delivered ? 1 : 0);
        _enqueue_event_trace("ai_ui", detail);
    }

    if (edit_delivered) {
        _hide_translate_window();  // the answer is in: the window has done its job
    } else if (uia_delivered) {
        paste_text(translation);
        _hide_translate_window();
    } else if (deliver && _textInsert == cxxime::AiActionInsert::kReplace && !_textEditControl &&
               !_textUiaTarget) {
        paste_text(translation);
        bool placed = true;
        if (line_should_be_replaced && !program_owns_its_text) {
            // Did it land? A paste is the one step nothing here can be sure of, so the line is read
            // back (the caret sits right after what was pasted). When the answer is not there — the
            // selection can be lost between the keys and the paste — the line is selected again and
            // the answer pasted once more; a second miss leaves the answer *shown*, not written
            // somewhere else (docs/ai.md).
            // A Chromium editor's text store does not show the paste at once, so the read below waits
            // longer than a message pump would suggest (docs/ai.md).
            pump_program_messages(300);
            int verdict = _line_starts_with(translation);
            if (verdict == 0) {
                select_line_with_keys(/*whole_line=*/false, /*copy_line=*/false);
                paste_text(translation);
                pump_program_messages(300);
                verdict = _line_starts_with(translation);
            }
            placed = verdict != 0;  // 0: the old text is still there; -1: nothing to ask, trust it
            char detail[128] = {};
            snprintf(detail, sizeof(detail), "deliver paste=%d", placed ? 1 : 0);
            _enqueue_event_trace("ai_ui", detail);
        }
        if (placed) {
            _hide_translate_window();  // the answer is in: the window has done its job (OnEndEdit
                                       // cannot be relied on for it — see the grace window)
        }  // It did not land: the window stays, saying the answer. Shown, not written elsewhere.
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
    const AiKeyPurpose purpose = _aiActionKeyPurpose;
    const bool moved = _aiActionKeyMoved;
    _aiActionKeyDown = 0;
    _aiActionKeyId.clear();
    _aiActionKeyPurpose = AiKeyPurpose::kAction;
    _aiActionKeyMoved = false;
    if (_configWindow) KillTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_AI_TAP_WATCH);
    if (moved || still_held) return;
    _fire_ai_action_key(purpose, action_id);
}

// What a key that has decided to fire does: the language list, the voice post-processing list, or
// the action (the menu, when the id is empty).
void TextService::_fire_ai_action_key(AiKeyPurpose purpose, const std::string& action_id) {
    switch (purpose) {
    case AiKeyPurpose::kLanguage:
        _pendingLanguageMenu = true;
        break;
    case AiKeyPurpose::kVoiceMode:
        _pendingVoiceModeMenu = true;
        break;
    case AiKeyPurpose::kAction:
    default:
        _pendingAiActionId = action_id;
        break;
    }
    PostMessageW(_configWindow, cxxime_tsf::WM_CXXIME_AI_ACTION_SELECTION, 0, 0);
}

bool TextService::_handle_ai_action_key(WPARAM key) {
    // One press on its own: the text the user selected is read (copied) for them, so there is no
    // pair and no Ctrl+C of their own to press. The key is the menu key, or one action's own key,
    // which runs it without the menu.
    std::string action_id;
    AiKeyPurpose purpose = AiKeyPurpose::kAction;
    if (_is_language_key(key)) {
        purpose = AiKeyPurpose::kLanguage;
    } else if (_is_voice_action_key(key)) {
        purpose = AiKeyPurpose::kVoiceMode;
    } else if (!_ai_action_key_for(key, &action_id)) {
        return false;
    }
    if (is_tap_key(key)) {
        // A tapped modifier: a Shift that begins a chord (Shift+A) is not a tap, so the decision
        // waits for the key-up, and the application keeps the key meanwhile. Not every program
        // hands the key-up over, so the key's own state is watched as well.
        _aiActionKeyDown = static_cast<UINT>(key);
        _aiActionKeyId = action_id;
        _aiActionKeyPurpose = purpose;
        _aiActionKeyMoved = false;
        _aiActionKeyDownTick = GetTickCount64();
        if (_configWindow) {
            SetTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_AI_TAP_WATCH, kAiTapWatchMs, nullptr);
        }
        return false;
    }
    _aiActionKeyDown = static_cast<UINT>(key);
    _aiActionKeyPurpose = purpose;
    // Reading the selection and placing the menu need an edit session and the caret, which cannot
    // be done inside a key callback: the message goes to the config window, as the translation's
    // does.
    _fire_ai_action_key(purpose, action_id);
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
    const AiKeyPurpose purpose = _aiActionKeyPurpose;
    const bool moved = _aiActionKeyMoved;
    _aiActionKeyDown = 0;
    _aiActionKeyId.clear();
    _aiActionKeyPurpose = AiKeyPurpose::kAction;
    _aiActionKeyMoved = false;
    if (_configWindow) KillTimer(_configWindow, cxxime_tsf::TIMER_CXXIME_AI_TAP_WATCH);
    if (is_tap_key(pressed)) {
        if (!moved) _fire_ai_action_key(purpose, action_id);
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

// The key that opens the voice post-processing list (纠错 / 提问), if this is it — the same shape.
bool TextService::_is_voice_action_key(WPARAM key) const {
    const cxxime::KeyboardShortcut& shortcut = _config.ai.voice_action_key;
    if (!shortcut.enabled() || key != shortcut.virtual_key) return false;
    std::uint32_t modifiers = _get_modifiers() & cxxime::kShortcutModifierMask;
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
    if (_pendingVoiceModeMenu) {
        // 语音识别后处理的方式（纠错 / 提问）也一样：不用先选文字。
        _pendingVoiceModeMenu = false;
        _open_voice_mode_menu();
        return;
    }
    const std::string action_id = _pendingAiActionId;
    _pendingAiActionId.clear();
    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    if (actions.empty()) return;  // no data file, or nothing usable in it
    if (action_id.empty()) {
        // The menu key only *asks* which action to run: the text is read when a row is picked
        // (_run_ai_action), so pressing the key does not put the selection on the clipboard
        // (docs/ai.md).
        _open_ai_menu();
        return;
    }
    std::wstring selection;
    bool shortened = false;
    const wchar_t* reason = nullptr;
    if (!_read_selected_text(&selection, &shortened, &reason)) {
        _present_failure(reason, std::wstring());
        return;
    }
    // An action's own key: straight to it, no menu.
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].id != action_id) continue;
        _start_ai_action(i, selection, shortened);
        return;
    }
}

void TextService::_open_ai_menu() {
    _close_ai_menu();
    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    for (std::size_t i = 0; i < actions.size(); ++i) {
        const auto hidden = _config.ai.action_disabled.find(actions[i].id);
        if (hidden != _config.ai.action_disabled.end() && hidden->second) continue;
        _aiMenuRows.push_back(i);
    }
    if (_aiMenuRows.empty()) return;
    _aiMenuPurpose = AiMenuPurpose::kRunAction;
    if (!_show_menu_window()) _close_ai_menu();
}

// 语音识别后处理的方式列表（纠错 / 提问）：同一个窗口，两行，选中即写进配置——不执行动作
// （docs/ai.md）。哪一项 = data/ai_actions.json 里的 proofread / ask，名字也取自那张表。
void TextService::_open_voice_mode_menu() {
    _close_ai_menu();
    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    for (const char* id : {"proofread", "ask"}) {
        for (std::size_t i = 0; i < actions.size(); ++i) {
            if (actions[i].id == id) {
                _aiMenuRows.push_back(i);
                break;
            }
        }
    }
    if (_aiMenuRows.empty()) return;
    _aiMenuPurpose = AiMenuPurpose::kSetVoiceMode;
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
    {
        char detail[128] = {};
        snprintf(detail, sizeof(detail), "menu_draw rows=%zu highlighted=%d visible=%d",
                 _aiMenuRows.size(), page.highlighted,
                 _aiMenuWindow->is_visible() ? 1 : 0);
        _enqueue_event_trace("ai_ui", detail);
    }
}

void TextService::_close_ai_menu() {
    _aiMenuOpen = false;
    _aiMenuRows.clear();
    _aiMenuHighlight = 0;
    _aiMenuPurpose = AiMenuPurpose::kRunAction;
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
    const auto trace_key = [&](const char* what) {
        // Which key the menu got and where the highlight ended up: the log is what tells "the key
        // never arrived" from "the window did not draw it" (docs/ai.md).
        char detail[128] = {};
        snprintf(detail, sizeof(detail), "menu_key key=0x%04x what=%s rows=%zu highlight=%zu",
                 static_cast<unsigned>(key), what, rows, _aiMenuHighlight);
        _enqueue_event_trace("ai_ui", detail);
    };
    if (key >= '1' && key <= '9') {
        trace_key("digit");
        const std::size_t row = key - '1';
        if (row < rows) _run_ai_action(row);
        return taken();
    }
    // Down, Right and Tab move the highlight on; Up and Left back: either hand reaches for either,
    // so the menu answers to all four arrows (docs/ai.md).
    if (key == VK_DOWN || key == VK_RIGHT || key == VK_TAB) {
        if (rows > 0) _aiMenuHighlight = (_aiMenuHighlight + 1) % rows;
        _render_ai_menu();
        trace_key("next");
        return taken();
    }
    if (key == VK_UP || key == VK_LEFT) {
        if (rows > 0) _aiMenuHighlight = (_aiMenuHighlight + rows - 1) % rows;
        _render_ai_menu();
        trace_key("prev");
        return taken();
    }
    if (key == VK_RETURN || key == VK_SPACE) {
        // Return and Space both take the row: picking a candidate with either is the habit this
        // menu is shaped like, and nothing is being composed while it is open (docs/ai.md).
        trace_key(key == VK_SPACE ? "space" : "enter");
        _run_ai_action(_aiMenuHighlight);
        return taken();
    }
    if (key == VK_ESCAPE) {
        trace_key("escape");
        _close_ai_menu();
        return taken();
    }
    if (is_bare_modifier(key)) {
        trace_key("modifier");
        return false;  // the chord being held: the menu stays
    }
    trace_key("other");
    _close_ai_menu();
    return false;
}

void TextService::_run_ai_action(std::size_t row) {
    if (row >= _aiMenuRows.size()) return;
    const std::size_t index = _aiMenuRows[row];
    const AiMenuPurpose purpose = _aiMenuPurpose;
    _close_ai_menu();
    if (purpose == AiMenuPurpose::kSetVoiceMode) {
        // 选中的是语音识别后处理的方式（纠错 / 提问）：写进配置，顺手把这一用途打开（否则选了不
        // 生效会让人困惑），不执行任何动作（docs/ai.md）。
        const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
        if (index >= actions.size()) return;
        std::string id;
        for (const char c : actions[index].id) {  // the id goes into JSON: escape the two characters
            if (c == '"' || c == '\\') id.push_back('\\');
            id.push_back(c);
        }
        const std::string patch =
            "{\"ai\":{\"use_for\":{\"voice\":true},\"voice_polish_action\":\"" + id + "\"}}";
        cxxime_tsf::patch_user_config(patch);
        _enqueue_event_trace("ai_ui", ("voice_mode " + actions[index].id).c_str());
        return;
    }
    // The text is read *here*, now that the user has said which action to run: opening the menu alone
    // has no business putting the selection on the clipboard (docs/ai.md). It is read from where the
    // caret and selection stand at this moment.
    std::wstring selection;
    bool shortened = false;
    const wchar_t* reason = nullptr;
    if (!_read_selected_text(&selection, &shortened, &reason)) {
        _present_failure(reason, std::wstring());
        return;
    }
    _start_ai_action(index, selection, shortened);
}

void TextService::_start_ai_action(std::size_t index, const std::wstring& source,
                                   bool shortened) {
    const std::vector<cxxime::AiAction>& actions = cxxime::ai_actions();
    if (index >= actions.size()) return;
    const cxxime::AiAction action = actions[index];
    {
        // Which row of the menu ran, and on how much text: the log is what tells "the highlight
        // landed where I meant" from "the wrong action ran" (docs/ai.md).
        char detail[128] = {};
        snprintf(detail, sizeof(detail), "action id=%s index=%zu chars=%zu",
                 action.id.c_str(), index, source.size());
        _enqueue_event_trace("ai_action", detail);
    }
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
