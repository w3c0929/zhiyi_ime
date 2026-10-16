// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The focused element's text, through UI Automation (docs/ai.md): for a program whose text TSF cannot
// read and which is not a classic edit control, a text pattern is the only way to learn what is
// selected without asking the program to copy it — and to select it without sending a single key.

#ifndef ZHIYU_TSF_UIA_TEXT_H_
#define ZHIYU_TSF_UIA_TEXT_H_

#include "pch.h"

#include <string>

// The action's target: the selection, or — with the caret alone — the caret's line from its start to
// the caret, which is then selected so that what is about to be handled is on screen (docs/ai.md).
// False when the focused element offers no text pattern (most of the time), or has no text there.
bool uia_read_target(std::wstring* text);

// Selects that same target again, and says whether its text is still `expected` — the check that keeps
// the answer from being pasted anywhere but over the text it was made from.
bool uia_select_if_unchanged(const std::wstring& expected);

#endif  // ZHIYU_TSF_UIA_TEXT_H_
