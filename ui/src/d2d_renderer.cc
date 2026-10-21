// Copyright (c) 2026 CxxIME Contributors. Apache License 2.0.

#include <cxxime/renderer.h>

#include <algorithm>
#include <string>

#include <cxxime/config.h>

#include "sparkle.h"

namespace cxxime {

static D2D1_COLOR_F c2d(const Color& c) { return D2D1::ColorF(c.r/255.0f, c.g/255.0f, c.b/255.0f, c.a/255.0f); }


static std::wstring dec(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 1) return {};
    std::wstring ws(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

static IDWriteTextFormat* mkfmt(IDWriteFactory* f, const wchar_t* name, float sz, DWRITE_TEXT_ALIGNMENT ha) {
    IDWriteTextFormat* fmt = nullptr;
    f->CreateTextFormat(name, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                        sz, L"zh-cn", &fmt);
    if (fmt) {
        fmt->SetTextAlignment(ha);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fmt->SetTrimming(&trimming, nullptr);
    }
    return fmt;
}

bool D2DRenderer::initialize(const Theme& theme, UINT dpi) {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d_factory_);
    if (FAILED(hr)) {
        finalize();
        return false;
    }
    // Draw into the paint DC rather than presenting to the HWND: an HWND target is tied to one
    // adapter, and on a monitor driven by another GPU its presents can show nothing at all.
    // CandidateWindow computes layout and HWND size in physical pixels. Keep D2D coordinates
    // in the same pixel space; otherwise high-DPI targets read our rectangles as DIP.
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        static_cast<float>(USER_DEFAULT_SCREEN_DPI), static_cast<float>(USER_DEFAULT_SCREEN_DPI));
    hr = d2d_factory_->CreateDCRenderTarget(&properties, &render_target_);
    if (FAILED(hr)) {
        finalize();
        return false;
    }
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &text_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray), &comment_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &bg_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DodgerBlue), &highlight_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &highlight_text_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(0.68f, 0.85f, 1.0f, 0.5f), &hover_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray), &preedit_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray),
                                          &preedit_separator_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                          &preedit_active_back_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray),
                                          &preedit_active_border_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::DodgerBlue),
                                          &preedit_cursor_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray), &label_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray), &nav_brush_);
    render_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray), &border_brush_);
    if (!text_brush_ || !comment_brush_ || !bg_brush_ || !highlight_brush_ ||
        !highlight_text_brush_ || !hover_brush_ || !preedit_brush_ ||
        !preedit_separator_brush_ || !preedit_active_back_brush_ ||
        !preedit_active_border_brush_ || !preedit_cursor_brush_ || !label_brush_ ||
        !nav_brush_ || !border_brush_) {
        finalize();
        return false;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&dwrite_factory_));
    if (FAILED(hr)) {
        finalize();
        return false;
    }
    float fsize = (float)theme.font_size * dpi / 72.0f;
    float psize = (float)theme.preedit_font_size * dpi / 72.0f;
    psize_ = psize;
    font_name_ = theme.font_name;
    fmt_left_ = mkfmt(dwrite_factory_, theme.font_name.c_str(), fsize,
                        DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt_right_ = mkfmt(dwrite_factory_, theme.font_name.c_str(), fsize,
                        DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt_preedit_ = mkfmt(dwrite_factory_, theme.font_name.c_str(), psize,
                         DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt_status_ = mkfmt(dwrite_factory_, theme.font_name.c_str(), psize,
                        DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt_status_center_ = mkfmt(dwrite_factory_, theme.font_name.c_str(), psize,
                               DWRITE_TEXT_ALIGNMENT_CENTER);
    // The page arrows are the candidate size (the same as Sogou's), not a smaller one.
    fmt_small_ = mkfmt(dwrite_factory_, theme.font_name.c_str(), fsize,
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt_gloss_ = mkfmt(dwrite_factory_, theme.font_name.c_str(),
                       (float)gloss_font_point(theme.font_size) * dpi / 72.0f,
                       DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!fmt_left_ || !fmt_right_ || !fmt_preedit_ || !fmt_status_ || !fmt_status_center_ ||
        !fmt_small_ || !fmt_gloss_) {
        finalize();
        return false;
    }
    if (SUCCEEDED(dwrite_factory_->CreateEllipsisTrimmingSign(fmt_gloss_, &gloss_ellipsis_))) {
        DWRITE_TRIMMING trimming = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fmt_gloss_->SetTrimming(&trimming, gloss_ellipsis_);
    }
    return true;
}

void D2DRenderer::finalize() {
    if (gloss_ellipsis_) { gloss_ellipsis_->Release(); gloss_ellipsis_ = nullptr; }
    for (auto* p : {&fmt_status_v_, &fmt_gloss_, &fmt_small_, &fmt_status_center_, &fmt_status_,
                    &fmt_preedit_, &fmt_right_, &fmt_left_}) {
        if (*p) { (*p)->Release(); *p = nullptr; }
    }
    fmt_status_v_scale_ = 0.0f;
    if (dwrite_factory_) { dwrite_factory_->Release(); dwrite_factory_ = nullptr; }
    for (auto* p : {&border_brush_, &nav_brush_, &label_brush_, &preedit_cursor_brush_,
                    &preedit_active_border_brush_, &preedit_active_back_brush_,
                    &preedit_separator_brush_, &preedit_brush_, &hover_brush_,
                    &highlight_text_brush_, &highlight_brush_, &bg_brush_, &comment_brush_,
                    &text_brush_})
    { if (*p) { (*p)->Release(); *p = nullptr; } }
    if (render_target_) { render_target_->Release(); render_target_ = nullptr; }
    if (d2d_factory_) { d2d_factory_->Release(); d2d_factory_ = nullptr; }
}

void D2DRenderer::draw_preedit(const RenderContext& ctx) {
    if (ctx.preedit.empty() || ctx.preedit_rect.right <= ctx.preedit_rect.left ||
        !preedit_brush_ || !ctx.theme) {
        return;
    }

    // The popup preedit is plain text: no fill and no box around the part being edited
    // (the runs already tell converted from active apart by colour).

    for (const auto& run : ctx.preedit_runs) {
        const std::wstring text = dec(run.text);
        ID2D1SolidColorBrush* brush = preedit_brush_;
        if (run.kind == PreeditRunKind::Converted) {
            brush = text_brush_;
        } else if (run.kind == PreeditRunKind::Separator) {
            brush = preedit_separator_brush_;
        }
        if (ctx.high_contrast && run.focused) {
            brush = highlight_text_brush_;
        }
        const D2D1_RECT_F rect = {
            static_cast<float>(run.rect.left), static_cast<float>(run.rect.top),
            static_cast<float>(run.rect.right), static_cast<float>(run.rect.bottom),
        };
        render_target_->DrawText(text.c_str(), static_cast<UINT32>(text.length()), fmt_preedit_,
                                 rect, brush);
    }

    if (preedit_cursor_brush_ &&
        ctx.preedit_cursor_rect.right > ctx.preedit_cursor_rect.left) {
        render_target_->FillRectangle(
            D2D1::RectF(static_cast<float>(ctx.preedit_cursor_rect.left),
                        static_cast<float>(ctx.preedit_cursor_rect.top),
                        static_cast<float>(ctx.preedit_cursor_rect.right),
                        static_cast<float>(ctx.preedit_cursor_rect.bottom)),
            preedit_cursor_brush_);
    }
}

HRESULT D2DRenderer::render(HDC hdc, const RECT& rect, const RenderContext& ctx) {
    if (!render_target_) return E_POINTER;
    HRESULT hr = render_target_->BindDC(hdc, &rect);
    if (FAILED(hr)) return hr;
    render_target_->BeginDraw();
    auto* cfg = ctx.layout_cfg;
    float corner = cfg ? (float)cfg->round_corner : 4.0f;
    float margin = cfg ? (float)cfg->margin_x : 12.0f;

    if (ctx.theme) {
        bg_brush_->SetColor(c2d(ctx.theme->background));
        text_brush_->SetColor(c2d(ctx.theme->text));
        comment_brush_->SetColor(c2d(ctx.theme->comment_text));
        highlight_brush_->SetColor(c2d(ctx.theme->hilited_back));
        highlight_text_brush_->SetColor(c2d(ctx.theme->hilited_text));
        preedit_brush_->SetColor(c2d(ctx.theme->preedit_text));
        preedit_separator_brush_->SetColor(c2d(ctx.theme->preedit_separator));
        preedit_active_back_brush_->SetColor(c2d(ctx.theme->preedit_active_back));
        preedit_active_border_brush_->SetColor(c2d(ctx.theme->preedit_active_border));
        Color cursor_color = ctx.theme->preedit_cursor;
        if (ctx.preedit_cursor_in_focus) {
            if (ctx.high_contrast) {
                cursor_color = ctx.theme->hilited_text;
            } else if (!ctx.preedit_cursor_emphasized) {
                cursor_color = ctx.preedit_cursor_idle;
            }
        }
        preedit_cursor_brush_->SetColor(c2d(cursor_color));
        label_brush_->SetColor(c2d(ctx.theme->label_text));
        nav_brush_->SetColor(c2d(ctx.theme->prev_page));
        border_brush_->SetColor(c2d(ctx.theme->border));
        D2D1::ColorF hover_col((ctx.theme->background.r + ctx.theme->hilited_back.r) / 2.0f / 255.0f,
                                (ctx.theme->background.g + ctx.theme->hilited_back.g) / 2.0f / 255.0f,
                                (ctx.theme->background.b + ctx.theme->hilited_back.b) / 2.0f / 255.0f, 1.0f);
        hover_brush_->SetColor(hover_col);
    }

    D2D1_SIZE_F sz = render_target_->GetSize();
    render_target_->FillRectangle(D2D1::RectF(0,0,sz.width,sz.height), bg_brush_);


    auto draw_border = [&]() {
        if (ctx.theme && cfg && cfg->border_width > 0 && border_brush_) {
            float bw = (float)cfg->border_width;
            float inset = bw / 2.0f;
            D2D1_ROUNDED_RECT rr = {
                D2D1::RectF(inset, inset, sz.width - inset, sz.height - inset),
                (float)cfg->round_corner_ex,
                (float)cfg->round_corner_ex,
            };
            render_target_->DrawRoundedRectangle(rr, border_brush_, bw);
        }
    };

    if (!ctx.rects || ctx.rects->empty()) {
        // No candidates — show preedit if available, otherwise placeholder
        if (!ctx.preedit.empty() && ctx.preedit_rect.right > ctx.preedit_rect.left) {
            draw_preedit(ctx);
        } else {
            render_target_->DrawText(L"知语输入法", 5, fmt_left_, D2D1::RectF(0,0,sz.width,sz.height), preedit_brush_);
        }
        draw_border();
        return render_target_->EndDraw();
    }

    // Preedit
    if (!ctx.preedit.empty() && ctx.preedit_rect.right > ctx.preedit_rect.left) {
        draw_preedit(ctx);
    }
    // 状态串：横排一行右对齐；竖排一列、每行一个字（字形不旋转）。
    if (!ctx.status_text.empty() && ctx.theme && fmt_status_ &&
        ctx.status_rect.right > ctx.status_rect.left) {
        const std::wstring status = dec(ctx.status_text);
        comment_brush_->SetColor(c2d(ctx.theme->comment_text));
        if (ctx.status_vertical && fmt_status_center_) {
            // 竖排：按「塞进窗口高度」的比例重建字号（行距收紧时字也跟着缩）。
            const float scale = ctx.status_font_scale > 0.05f ? ctx.status_font_scale : 1.0f;
            if (!fmt_status_v_ || fmt_status_v_scale_ != scale) {
                if (fmt_status_v_) { fmt_status_v_->Release(); fmt_status_v_ = nullptr; }
                fmt_status_v_ = mkfmt(dwrite_factory_, font_name_.c_str(), psize_ * scale,
                                      DWRITE_TEXT_ALIGNMENT_CENTER);
                fmt_status_v_scale_ = fmt_status_v_ ? scale : 0.0f;
            }
            std::size_t at = 0;
            for (const RECT& gr : ctx.status_glyph_rects) {
                while (at < status.size() && is_status_space(status[at])) {
                    ++at;
                }
                if (at >= status.size()) break;
                const std::wstring one(1, status[at++]);
                const D2D1_RECT_F sr = {(float)gr.left, (float)gr.top, (float)gr.right,
                                        (float)gr.bottom};
                render_target_->DrawText(one.c_str(), 1,
                                         fmt_status_v_ ? fmt_status_v_ : fmt_status_center_, sr,
                                         comment_brush_);
            }
        } else {
            const D2D1_RECT_F sr = {(float)ctx.status_rect.left, (float)ctx.status_rect.top,
                                    (float)ctx.status_rect.right, (float)ctx.status_rect.bottom};
            render_target_->DrawText(status.c_str(), (UINT32)status.length(), fmt_status_, sr,
                                     comment_brush_);
        }
    }

    // Candidates
    for (const auto& cr : *ctx.rects) {
        int i = cr.index;
        bool hl = (i == ctx.highlighted);
        bool hv = ctx.hovered_target == CandidateHoverTarget::Candidate &&
                  i == ctx.hovered_candidate_index;
        D2D1_RECT_F hr = {(float)cr.highlight_rect.left, (float)cr.highlight_rect.top,
                          (float)cr.highlight_rect.right, (float)cr.highlight_rect.bottom};
        const bool recommended = cr.recommended && cr.mark_rect.right > cr.mark_rect.left;
        const D2D1_ROUNDED_RECT rr = {hr, corner, corner};
        if ((hl && !recommended) || (hv && !hl)) {
            render_target_->FillRoundedRectangle(rr, hl ? highlight_brush_ : hover_brush_);
        }
        if (recommended) {
            draw_recommend_wash(rr, cr.mark_rect, ctx.sparkle_t, hl,
                                ctx.theme ? recommend_wash_opacity(ctx.theme->background)
                                          : 0.13f);
        }
        // Label
        std::wstring label = candidate_label(i);
        D2D1_RECT_F lr = {(float)cr.label_rect.left, (float)cr.label_rect.top,
                          (float)cr.label_rect.right, (float)cr.label_rect.bottom};
        render_target_->DrawText(label.c_str(), (UINT32)label.length(), fmt_right_, lr,
                                 hl ? highlight_text_brush_ : label_brush_);
        // Text
        auto wt = dec(cr.text);
        D2D1_RECT_F tr = {(float)cr.text_rect.left, (float)cr.text_rect.top,
                          (float)cr.text_rect.right, (float)cr.text_rect.bottom};
        render_target_->DrawText(wt.c_str(), (UINT32)wt.length(), fmt_left_, tr,
                                 hl ? highlight_text_brush_ : text_brush_);
        if (!cr.comment.empty()) {
            auto wc = dec(cr.comment);
            D2D1_RECT_F comment_rect = {
                (float)cr.comment_rect.left, (float)cr.comment_rect.top,
                (float)cr.comment_rect.right, (float)cr.comment_rect.bottom,
            };
            auto* brush = hl ? highlight_text_brush_ : (hv ? text_brush_ : comment_brush_);
            render_target_->DrawText(wc.c_str(), (UINT32)wc.length(), fmt_left_,
                                    comment_rect, brush);
        }
        if (!cr.gloss.empty() && fmt_gloss_) {
            auto wg = dec(cr.gloss);
            D2D1_RECT_F gloss_rect = {
                (float)cr.gloss_rect.left, (float)cr.gloss_rect.top,
                (float)cr.gloss_rect.right, (float)cr.gloss_rect.bottom,
            };
            render_target_->DrawText(wg.c_str(), (UINT32)wg.length(), fmt_gloss_, gloss_rect,
                                     hl ? highlight_text_brush_ : comment_brush_);
        }
        if (recommended) {
            draw_sparkle(cr.mark_rect, ctx.sparkle_t, hl);
        }
    }

    // Page nav (always visible, grayed when disabled)
    if (ctx.page_total > 1 && fmt_small_) {
        float nc = corner > 2 ? corner-1 : 1;
        bool pe = (ctx.page_current > 1), ne = (ctx.page_current < ctx.page_total);
        // 不可用时用"背景 3/4 + 文字 1/4"的浅色（与 GDI 同一公式）：一眼看出点不了。
        Color dim_color = ctx.theme
                              ? Color{static_cast<uint8_t>(
                                          (ctx.theme->background.r * 3 + ctx.theme->text.r) / 4),
                                      static_cast<uint8_t>(
                                          (ctx.theme->background.g * 3 + ctx.theme->text.g) / 4),
                                      static_cast<uint8_t>(
                                          (ctx.theme->background.b * 3 + ctx.theme->text.b) / 4),
                                      255}
                              : Color{160, 160, 160, 255};
        preedit_brush_->SetColor(c2d(dim_color));
        // <
        {
            D2D1_RECT_F pr = {(float)ctx.prev_button_rect.left, (float)ctx.prev_button_rect.top,
                              (float)ctx.prev_button_rect.right, (float)ctx.prev_button_rect.bottom};
            bool h = pe && ctx.hovered_target == CandidateHoverTarget::PreviousPage;
            if (h) { D2D1_ROUNDED_RECT rr = {pr, nc, nc}; render_target_->FillRoundedRectangle(rr, highlight_brush_); }
            auto* b = h ? highlight_text_brush_ : (pe ? nav_brush_ : preedit_brush_);
            // 画出来的折线（不用字形：‹ 在字体里本身只是小标点，放大字号也显小）。
            const float cx = (pr.left + pr.right) / 2.0f;
            const float cy = (pr.top + pr.bottom) / 2.0f;
            const float hw = (pr.right - pr.left) * 0.16f;
            const float hh = (pr.bottom - pr.top) * 0.20f;
            render_target_->DrawLine({cx + hw, cy - hh}, {cx - hw, cy}, b, 1.6f);
            render_target_->DrawLine({cx - hw, cy}, {cx + hw, cy + hh}, b, 1.6f);
        }
        // >
        {
            D2D1_RECT_F nr = {(float)ctx.next_button_rect.left, (float)ctx.next_button_rect.top,
                              (float)ctx.next_button_rect.right, (float)ctx.next_button_rect.bottom};
            bool h = ne && ctx.hovered_target == CandidateHoverTarget::NextPage;
            if (h) { D2D1_ROUNDED_RECT rr = {nr, nc, nc}; render_target_->FillRoundedRectangle(rr, highlight_brush_); }
            auto* b = h ? highlight_text_brush_ : (ne ? nav_brush_ : preedit_brush_);
            const float cx = (nr.left + nr.right) / 2.0f;
            const float cy = (nr.top + nr.bottom) / 2.0f;
            const float hw = (nr.right - nr.left) * 0.16f;
            const float hh = (nr.bottom - nr.top) * 0.20f;
            render_target_->DrawLine({cx - hw, cy - hh}, {cx + hw, cy}, b, 1.6f);
            render_target_->DrawLine({cx + hw, cy}, {cx - hw, cy + hh}, b, 1.6f);
        }
    }

    // Toolbar: the settings button (☰) — three bars, hovered like the page arrows.
    if (ctx.settings_button_rect.right > ctx.settings_button_rect.left) {
        const float l = (float)ctx.settings_button_rect.left;
        const float t = (float)ctx.settings_button_rect.top;
        const float r = (float)ctx.settings_button_rect.right;
        const float b = (float)ctx.settings_button_rect.bottom;
        const float rc2 = corner > 2 ? corner - 1 : 1;
        const bool h = ctx.hovered_target == CandidateHoverTarget::Settings;
        if (h) {
            D2D1_ROUNDED_RECT rr = {D2D1::RectF(l, t, r, b), rc2, rc2};
            render_target_->FillRoundedRectangle(rr, highlight_brush_);
        }
        auto* brush = h ? highlight_text_brush_ : nav_brush_;
        // 三条细横杠：条厚约 2px、缝约为条厚的 2 倍、整体宽约框宽的 58%。坐标取整 + 关闭抗锯齿，
        // 否则半像素落点会把细线糊成灰边（Sogou 那个就是像素级清晰的细线）。
        const int box_h = static_cast<int>(b - t);
        const int box_w = static_cast<int>(r - l);
        int bar = static_cast<int>(std::lround(box_h * 0.07f));
        if (bar < 2) bar = 2;
        const int gap = bar * 2;
        const int total = bar * 3 + gap * 2;
        const int left = static_cast<int>(std::lround((l + r) / 2.0f - box_w * 0.87f / 2.0f));
        const int right = left + static_cast<int>(std::lround(box_w * 0.87f));
        // 位置由几何决定（layout.nav_offset_y），这里只按按钮框垂直居中。
        const int top = static_cast<int>(std::lround((t + b) / 2.0f - total / 2.0f));
        const D2D1_ANTIALIAS_MODE previous = render_target_->GetAntialiasMode();
        render_target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < 3; ++i) {
            const int y = top + i * (bar + gap);
            render_target_->FillRectangle(
                D2D1::RectF((float)left, (float)y, (float)right, (float)(y + bar)), brush);
        }
        render_target_->SetAntialiasMode(previous);
    }

    draw_border();

    return render_target_->EndDraw();
}

ID2D1LinearGradientBrush* D2DRenderer::make_recommend_gradient(const D2D1_RECT_F& area,
                                                             const Color& from, const Color& to,
                                                             float opacity) {
    const D2D1_GRADIENT_STOP stops[] = {{0.0f, c2d(from)}, {1.0f, c2d(to)}};
    ID2D1GradientStopCollection* collection = nullptr;
    if (FAILED(render_target_->CreateGradientStopCollection(stops, 2, &collection))) {
        return nullptr;
    }
    ID2D1LinearGradientBrush* brush = nullptr;
    render_target_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(area.left, area.bottom),
                                            D2D1::Point2F(area.right, area.top)),
        D2D1::BrushProperties(opacity), collection, &brush);
    collection->Release();
    return brush;
}

void D2DRenderer::draw_recommend_wash(const D2D1_ROUNDED_RECT& box, const RECT& mark, float t,
                                      bool highlighted, float wash_opacity) {
    // Highlighted: the highlight itself turns blue-to-purple. Otherwise a light wash.
    if (ID2D1LinearGradientBrush* wash = make_recommend_gradient(
            box.rect, highlighted ? kRecommendHighlightBlue : kRecommendBlue,
            highlighted ? kRecommendHighlightPurple : kRecommendPurple,
            highlighted ? 1.0f : wash_opacity)) {
        render_target_->FillRoundedRectangle(box, wash);
        wash->Release();
    }
    // A soft glow behind the star, inside the candidate box.
    const SparkleStar star = sparkle_star(mark, t);
    const Color glow = highlighted ? Color{255, 255, 255, 255} : kRecommendPurple;
    const D2D1_GRADIENT_STOP stops[] = {
        {0.0f, D2D1::ColorF(glow.r / 255.0f, glow.g / 255.0f, glow.b / 255.0f,
                            highlighted ? 0.30f : 0.32f)},
        {1.0f, D2D1::ColorF(glow.r / 255.0f, glow.g / 255.0f, glow.b / 255.0f, 0.0f)},
    };
    ID2D1GradientStopCollection* collection = nullptr;
    if (FAILED(render_target_->CreateGradientStopCollection(stops, 2, &collection))) return;
    ID2D1RadialGradientBrush* brush = nullptr;
    const float radius = (box.rect.bottom - box.rect.top) * 0.85f;
    render_target_->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(D2D1::Point2F(star.cx, star.cy), D2D1::Point2F(),
                                            radius, radius),
        collection, &brush);
    collection->Release();
    if (brush) {
        render_target_->FillRoundedRectangle(box, brush);
        brush->Release();
    }
}

void D2DRenderer::draw_sparkle(const RECT& mark, float t, bool highlighted) {
    const SparkleStar star = sparkle_star(mark, t);
    if (star.r < 0.5f) return;
    ID2D1PathGeometry* path = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (SUCCEEDED(d2d_factory_->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
        const auto c = sparkle_curve(star);
        sink->BeginFigure(D2D1::Point2F(c[0].x, c[0].y), D2D1_FIGURE_BEGIN_FILLED);
        for (size_t i = 1; i + 2 < c.size(); i += 3) {
            sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(c[i].x, c[i].y),
                                                D2D1::Point2F(c[i + 1].x, c[i + 1].y),
                                                D2D1::Point2F(c[i + 2].x, c[i + 2].y)));
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        // White on the gradient highlight; blue-to-purple elsewhere.
        const D2D1_RECT_F area = {star.cx - star.r, star.cy - star.r, star.cx + star.r,
                                  star.cy + star.r};
        ID2D1Brush* brush = nullptr;
        if (highlighted) {
            ID2D1SolidColorBrush* white = nullptr;
            render_target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), &white);
            brush = white;
        } else {
            brush = make_recommend_gradient(area, kRecommendBlue, kRecommendPurple, 1.0f);
        }
        if (brush) {
            render_target_->FillGeometry(path, brush);
            brush->Release();
        }
    }
    if (sink) sink->Release();
    if (path) path->Release();
}

} // namespace cxxime
