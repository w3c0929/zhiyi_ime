// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#ifndef CXXIME_RENDER_CONTEXT_H_
#define CXXIME_RENDER_CONTEXT_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

#include <cxxime/candidate.h>
#include <cxxime/candidate_presentation.h>

namespace cxxime {

// Candidate label: "1." .. "9.", and "0." for the tenth (selected with the 0 key).
inline std::wstring candidate_label(int index) {
    return std::to_wstring((index + 1) % 10) + L".";
}

struct Color { uint8_t r, g, b, a; };

struct Theme {
    Color background{255, 255, 255, 255};
    Color text{0, 0, 0, 255};               // normal candidate text
    Color comment_text{102, 102, 102, 255}; // normal candidate comment text
    Color label_text{128, 128, 128, 255};   // label "1. " text
    Color preedit_text{128, 128, 128, 255}; // preedit text
    Color preedit_separator{102, 102, 102, 255};
    Color preedit_active_back{232, 240, 248, 255};
    Color preedit_active_border{176, 200, 224, 255};
    Color preedit_cursor{0, 120, 215, 255}; // static cursor in popup preedit
    Color hilited_text{255, 255, 255, 255}; // highlighted candidate text
    Color hilited_back{0, 120, 215, 255};   // highlighted candidate background
    Color border{200, 200, 200, 255};       // window/separator border
    Color prev_page{128, 128, 128, 255};    // page nav arrow color
    Color next_page{128, 128, 128, 255};
    int font_size = 14;
    int preedit_font_size = 12;
    std::wstring font_name = L"Microsoft YaHei UI";
};

struct CandidateRect {
    int index;
    std::string text;
    std::string comment;
    RECT label_rect{};
    RECT text_rect{};
    RECT comment_rect{};
    RECT highlight_rect{};
    bool recommended = false;  // Laya's pick: sparkle at the top-right corner
    RECT mark_rect{};          // space reserved after the text for the sparkle
    std::string gloss;         // learning mode translation as shown ("n. now · adv. at present")
    RECT gloss_rect{};         // a column after the candidates (vertical layout only)
};

// Width reserved after a recommended candidate for its sparkle mark.
inline int recommendation_mark_width(int row_height) { return row_height * 9 / 20; }

// Learning mode translations are drawn at 80% of the candidate font.
inline int gloss_font_point(int font_size) { return (std::max)(8, font_size * 4 / 5); }

// "n. now · adv. at present": the first `count` senses of an encoded translation.
inline std::string gloss_display_text(const std::string& encoded, std::size_t count) {
    std::string text;
    std::size_t used = 0;
    for (const GlossPart& part : decode_candidate_gloss(encoded)) {
        if (used++ == count) break;
        if (!text.empty()) text += " \xC2\xB7 ";  // " · "
        if (!part.label.empty()) text += part.label + " ";
        text += part.text;
    }
    return text;
}

Theme make_light_theme();
Theme make_dark_theme();
Theme get_theme(const std::string& scheme_name);

struct Config;
Theme build_theme_from_config(const Config& cfg);

struct LayoutConfig;
enum class CandidateHoverTarget { None, Candidate, PreviousPage, NextPage, Settings };
enum class PreeditRunKind { Converted, Active, Separator };

struct PreeditTextRun {
    std::string text;
    RECT rect{};
    PreeditRunKind kind = PreeditRunKind::Active;
    bool focused = false;
};

// 竖排状态串里不占行的字符（空格、制表、换行）。
inline bool is_status_space(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

struct RenderContext {
    const std::vector<CandidateRect>* rects = nullptr;
    const Theme* theme = nullptr;
    const LayoutConfig* layout_cfg = nullptr;
    std::string preedit;
    // 右上角的状态串（中文·全拼·半角·中文标点），空 = 不显示
    std::string status_text;
    RECT status_rect{};  // 状态串的整体区域（横排：一行；竖排：一列）
    bool status_vertical = false;  // 竖排：一个字一行，右侧一列
    // 竖排时每个字一个字框（几何和渲染共用，避免两边算法不一致）。
    std::vector<RECT> status_glyph_rects;
    // 竖排状态列为了塞进窗口高度而缩小的字号比例（1.0 = 不缩）。
    float status_font_scale = 1.0f;
    int status_width = 0;  // 横排状态串的像素宽度（用来和 ☰ 按钮对齐）
    size_t preedit_cursor = 0;
    bool preedit_cursor_in_focus = false;
    bool preedit_cursor_emphasized = false;
    Color preedit_cursor_idle{};
    int page_current = 1, page_total = 1;
    int highlighted = -1;
    // Seconds since the Laya recommendation mark appeared while it twinkles; < 0 = steady.
    float sparkle_t = -1.0f;
    CandidateHoverTarget hovered_target = CandidateHoverTarget::None;
    int hovered_candidate_index = -1;
    RECT preedit_rect{};
    RECT preedit_active_rect{};
    RECT preedit_cursor_rect{};
    int preedit_corner_radius = 3;
    int preedit_border_width = 1;
    bool high_contrast = false;
    std::vector<PreeditTextRun> preedit_runs;
    RECT page_indicator_rect{};
    RECT prev_button_rect{};
    RECT next_button_rect{};
    // Settings button (the toolbar's ☰): opens the settings program.
    RECT settings_button_rect{};
    int preedit_text_height = 0;
};

} // namespace cxxime
#endif
