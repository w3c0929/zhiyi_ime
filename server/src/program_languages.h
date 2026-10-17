// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// The language each program was left in (settings > 按键 > 默认输入法 = 自动). A session starts in the
// language its program was left in last time; a program never seen starts Chinese. The list is one
// small JSON file in the user data directory (user_data_path("program-languages.json")), kept on
// this computer only and never uploaded (docs/privacy.md); it is written only when a program's
// language actually changes, so switching back and forth costs one write per switch.

#ifndef CXXIME_SERVER_PROGRAM_LANGUAGES_H_
#define CXXIME_SERVER_PROGRAM_LANGUAGES_H_

#include <cstddef>
#include <map>
#include <mutex>
#include <string>

namespace cxxime {
namespace server {

class ProgramLanguages {
public:
    // Reads the file. Missing, unreadable or damaged is simply an empty list: this is a memory, not
    // something anything depends on.
    void load();
    // "chinese" or "english"; empty when this program has not been seen before.
    std::string lookup(const std::string& program) const;
    // Records the language of `program` and writes the file when that changes anything.
    void remember(const std::string& program, const std::string& language);

    // Far more programs than a person uses; the least recently heard from one is dropped when the
    // list is full, so the file cannot grow without end.
    static constexpr std::size_t kMaxPrograms = 256;

private:
    struct Entry {
        std::string language;
        long long last = 0;  // when this program was last heard from, seconds since the epoch
    };
    void write_locked() const;

    mutable std::mutex mutex_;
    std::map<std::string, Entry> programs_;
};

}  // namespace server
}  // namespace cxxime

#endif  // CXXIME_SERVER_PROGRAM_LANGUAGES_H_
