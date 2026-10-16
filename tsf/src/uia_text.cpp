// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.

#include "uia_text.h"

#include <uiautomationclient.h>

namespace {

// Made once per call: the objects are not cheap, and the focused element changes with every program
// and every move of the caret. `ok` says a text pattern is there to ask.
struct UiaCall {
    com_ptr<IUIAutomation> automation;
    com_ptr<IUIAutomationTextPattern> pattern;
    bool ok = false;

    UiaCall() {
        IUIAutomation* raw = nullptr;
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&raw))) ||
            !raw) {
            return;
        }
        automation.Attach(raw);
        IUIAutomationElement* focused = nullptr;
        if (FAILED(automation->GetFocusedElement(&focused)) || !focused) return;
        com_ptr<IUIAutomationElement> element;
        element.Attach(focused);
        IUIAutomationTextPattern* text = nullptr;
        if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text))) ||
            !text) {
            return;
        }
        pattern.Attach(text);
        ok = true;
    }
};

std::wstring bstr_text(BSTR value) {
    std::wstring text = value ? std::wstring(value) : std::wstring();
    if (value) SysFreeString(value);
    return text;
}

// The range an action works on (docs/ai.md): the selection, or — with the caret alone — the caret's
// line from its start to the caret. Null when the pattern has neither.
com_ptr<IUIAutomationTextRange> make_target_range(IUIAutomationTextPattern* pattern,
                                                  std::wstring* text, bool* was_selection) {
    IUIAutomationTextRangeArray* ranges = nullptr;
    if (FAILED(pattern->GetSelection(&ranges)) || !ranges) return nullptr;
    com_ptr<IUIAutomationTextRangeArray> array;
    array.Attach(ranges);
    int count = 0;
    if (FAILED(array->get_Length(&count)) || count <= 0) return nullptr;
    IUIAutomationTextRange* first = nullptr;
    if (FAILED(array->GetElement(0, &first)) || !first) return nullptr;
    com_ptr<IUIAutomationTextRange> caret;
    caret.Attach(first);

    BSTR value = nullptr;
    const std::wstring selected =
        SUCCEEDED(caret->GetText(-1, &value)) ? bstr_text(value) : std::wstring();
    if (!selected.empty()) {
        *text = selected;
        if (was_selection) *was_selection = true;
        return caret;
    }

    // Nothing selected: the caret's line, with its end moved back to the caret.
    IUIAutomationTextRange* clone = nullptr;
    if (FAILED(caret->Clone(&clone)) || !clone) return nullptr;
    com_ptr<IUIAutomationTextRange> line;
    line.Attach(clone);
    if (FAILED(line->ExpandToEnclosingUnit(TextUnit_Line))) return nullptr;
    if (FAILED(line->MoveEndpointByRange(TextPatternRangeEndpoint_End, caret.Get(),
                                         TextPatternRangeEndpoint_Start))) {
        return nullptr;
    }
    BSTR line_value = nullptr;
    const std::wstring line_text =
        SUCCEEDED(line->GetText(-1, &line_value)) ? bstr_text(line_value) : std::wstring();
    if (line_text.empty()) return nullptr;
    *text = line_text;
    if (was_selection) *was_selection = false;
    return line;
}

// The same target, selected, and whether its text is still `expected`.
bool select_target(const std::wstring* expected) {
    UiaCall call;
    if (!call.ok) return false;
    std::wstring text;
    bool was_selection = false;
    com_ptr<IUIAutomationTextRange> range = make_target_range(call.pattern.Get(), &text,
                                                             &was_selection);
    if (!range) return false;
    if (expected && text != *expected) return false;
    return SUCCEEDED(range->Select());
}

}  // namespace

bool uia_read_target(std::wstring* text) {
    if (!text) return false;
    UiaCall call;
    if (!call.ok) return false;
    bool was_selection = false;
    text->clear();
    com_ptr<IUIAutomationTextRange> range = make_target_range(call.pattern.Get(), text,
                                                             &was_selection);
    if (!range || text->empty()) return false;
    // Selecting it here is what the TSF read does for a readable document: the user sees what the
    // action is about to work on. The delivery selects it again, after checking the text is the same.
    range->Select();
    return true;
}

bool uia_select_if_unchanged(const std::wstring& expected) {
    return select_target(&expected);
}
