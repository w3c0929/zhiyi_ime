// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_SYMBOL_PROCESSOR_H_
#define CXXIME_SYMBOL_PROCESSOR_H_

#include <cxxime/context.h>
#include <cxxime/key_event.h>
#include <cxxime/processor.h>

namespace cxxime {

class SymbolProcessor {
public:
    static bool is_active(const Context& context);
    static bool is_trigger(const KeyEvent& event);

    // `preserve_case`: English (ASCII) mode, where a backslash that begins no command is text and
    // text keeps the capitals it was typed with (docs/ai.md).
    ProcessResult process_key(const KeyEvent& event, Context& context, bool allow_trigger,
                              bool preserve_case = false) const;

private:
    static bool edit_preedit(const KeyEvent& event, Context& context);
    static ProcessResult select_candidate(Context& context, int index);
};

} // namespace cxxime

#endif // CXXIME_SYMBOL_PROCESSOR_H_
