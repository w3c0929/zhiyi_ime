// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_SYMBOL_TABLE_H_
#define CXXIME_SYMBOL_TABLE_H_

#include <string>
#include <unordered_map>
#include <vector>

#include <cxxime/candidate.h>
#include <cxxime/translation_result.h>

namespace cxxime {

inline constexpr char kSymbolPrefix = '\\';
// A command typed under the symbol prefix, instead of a symbol name (docs/ai.md): \fy asks for the
// translation language menu. The prefix is the same; the command is looked for first.
inline constexpr char kAiLanguageCommand[] = "fy";

class SymbolTable {
public:
    bool load(const std::string& path);
    bool empty() const { return categories_.empty(); }

    TranslationResult translate(const TranslationRequest& request) const;
    CandidatePage translate_page(const std::string& code, int page_index, int page_size,
                                 int candidate_offset = -1) const;

    // Whether any code in the table begins with `prefix`. English (ASCII) mode needs it to tell a
    // command being typed from a backslash that is just text (engine.cc, docs/ai.md).
    bool has_code_prefix(const std::string& prefix) const;

private:
    struct Category {
        std::string name;
        std::vector<std::string> candidates;
    };

    std::unordered_map<std::string, Category> categories_;
    std::vector<std::string> category_order_;
};

} // namespace cxxime

#endif // CXXIME_SYMBOL_TABLE_H_
