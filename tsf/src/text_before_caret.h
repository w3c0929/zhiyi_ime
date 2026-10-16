// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// Reads the text before the caret of an input box, for the context the recommendation model
// uses (sent to the server with SET_CONTEXT when a composition starts).

#ifndef ZHIYU_TSF_TEXT_BEFORE_CARET_H_
#define ZHIYU_TSF_TEXT_BEFORE_CARET_H_

#include "pch.h"

#include <cstddef>
#include <string>

enum class TextBeforeCaret {
    kRead,         // `text` holds up to max_chars characters before the caret (or the selection)
    kPrivate,      // a password / private field: never read
    kUnavailable,  // the app did not let us read it, or its document is transitory (classic
                   // Win32 edit boxes; not Chromium's): the remembered context is used instead
};

// A classic Win32 program reaching TSF through the IMM compatibility layer: its transitory
// document holds only the composition (Chromium's transitory documents are real ones).
bool context_is_imm_document(ITfContext* context);

// A synchronous read-only edit session; call it while handling a key.
TextBeforeCaret read_text_before_caret(ITfContext* context, TfClientId client_id, size_t max_chars,
                                       std::wstring* text);

// As read_text_before_caret, but reads the selected text itself (empty when nothing is selected):
// what the selection translation translates and the AI actions work on. `truncated` (optional)
// tells whether the selection was longer than max_chars: it holds the first max_chars characters
// then, and the caller is expected to say so rather than pass it off as the whole selection.
TextBeforeCaret read_selection_text(ITfContext* context, TfClientId client_id, size_t max_chars,
                                    std::wstring* text, bool* truncated = nullptr);

// From the start of the caret's line to the caret, which it *selects* for the caller (docs/ai.md:
// an action key with nothing selected works on the part of the line written so far, and having it
// selected is how the user sees what is about to be handled). Bounded to max_chars before the
// caret: `truncated` says the line began further back, so the caller treats it like a cut
// selection.
TextBeforeCaret select_line_at_caret(ITfContext* context, TfClientId client_id, size_t max_chars,
                                     std::wstring* text, bool* truncated = nullptr);

// The same read, *without* selecting what it read: for asking what the caret's line holds (after the
// answer was pasted, docs/ai.md) without leaving a selection behind that the user did not ask for.
TextBeforeCaret read_line_at_caret(ITfContext* context, TfClientId client_id, size_t max_chars,
                                   std::wstring* text, bool* truncated = nullptr);

// Selects `length` characters *from the caret onwards*, without reading them, when the caller
// already knows both (a program whose text cannot be read still hands its own copy of the line over,
// docs/ai.md). With the caret moved to the line's start first — a plain Home, which these programs
// answer to — this selects exactly the line, and a selection TSF sets is a real document selection:
// the same thing that makes a readable program exact. Unlike the reads above this is tried on
// transitory documents too, because whether their text store accepts a selection is exactly what is
// being asked. True when the edit session set it.
bool select_length_at_caret(ITfContext* context, TfClientId client_id, size_t length);

#endif  // ZHIYU_TSF_TEXT_BEFORE_CARET_H_
