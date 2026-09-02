#pragma once

// Bridges for the handful of ImGui APIs that FUCK does not wrap 1:1, so the
// editor draw code can port ImGui:: -> FUCK:: mechanically and route the deltas
// through here. FUCK owns the ImGui context; we reach it only via FUCK::.
// (Drawlist text/rect helpers for the slot-row glyphs land with the icon port.)

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h
#include "FUCK_API.h"
#include <imgui.h>  // ImVec2/ImVec4, ImGuiCol_/ImGuiStyleVar_, ColorConvertFloat4ToU32

#include <algorithm>  // std::max/std::min - HatchRect solves segments against a rect
#include <cmath>      // std::sin/std::cos/std::floor - SpinnerArc's segment fan
#include <cstdarg>    // va_list - FUCK::SetTooltip is not variadic
#include <cstdio>     // vsnprintf
#include <string>     // ModalName builds the name both halves of a modal share

namespace OS::ui {

    // ImGui::GetFontSize() == the current font's pixel size == text line height.
    inline float FontSize() { return FUCK::GetTextLineHeight(); }

    // ImGui::GetStyle().<member> replacements (FUCK exposes style vars, not the struct).
    inline ImVec2 FramePadding() { return FUCK::GetStyleVarVec(ImGuiStyleVar_FramePadding); }
    inline ImVec2 ItemSpacing() { return FUCK::GetStyleVarVec(ImGuiStyleVar_ItemSpacing); }
    inline ImVec2 WindowPadding() { return FUCK::GetStyleVarVec(ImGuiStyleVar_WindowPadding); }
    inline float  FrameRounding() { return FUCK::GetStyleVar(ImGuiStyleVar_FrameRounding); }

    // How far a nested block steps in from whatever it sits under.
    //
    // ⚠⚠ THE THEME'S OWN IndentSpacing IS NOT USABLE RAW, AND THE NUMBER IS ON
    // RECORD. FLICK's default preset answers 106.7 at 1.33 scale against a 10.7
    // gutter, measured by a probe in the field on 2026-08-27. That is most of a
    // preview card, so every bare FUCK::Indent() pushed its block most of the way
    // across the pane on that theme while looking correct on Vel'dun, whose
    // number is smaller (user: "the cards are shoved to the right for some
    // reason on the vanilla FLICK theme", "the item in the skin list here going
    // off to the right"). ⛔ Do not go back to a bare Indent().
    //
    // ⚠ A CAP AND NOT A REPLACEMENT, which is FramePolicy::PlainRadius's rule
    // running the other way: a theme that indents modestly is obeyed and keeps
    // its own look, and a theme that asks for more than one gutter gets one
    // gutter. Nothing here can push a block further out than the spacing the
    // layout already uses, and nothing here can pull one in that was fine.
    //
    // ⚠ ONE GUTTER, because that is the gap the surrounding layout already puts
    // between things rather than a number picked for how it looked once.
    // RulesUI::DrawConditionFlow reached the same verdict from the other side and
    // aligns to a real edge instead; where there IS an edge to align to, that is
    // the better answer and this is for where there is not.
    inline float NestIndent() {
        const float theme  = FUCK::GetStyleVar(ImGuiStyleVar_IndentSpacing);
        const float gutter = ItemSpacing().x;
        if (!(theme > 0.0f)) {
            return 0.0f;  // a theme that indents nothing is left indenting nothing
        }
        return (theme > gutter) ? gutter : theme;
    }

    // The gap a control keeps from the RIGHT edge of whatever contains it.
    //
    // ⚠ A CONTROL ENDING EXACTLY AT A CHILD'S CONTENT MAXIMUM IS NOT FLUSH, IT
    // IS CLIPPED. Its frame stroke is centred on its own edge, so the outer half
    // lands past the content max and the child's clip rect eats it: the control
    // comes out with three sides drawn and one missing, which reads as cheap
    // long before anyone works out that a line is gone. -FLT_MIN and a bare
    // right-align both land exactly there.
    //
    // The presets row found this first and spelled the number out locally as
    // "trailingFrameClearance". It is the same requirement on the search field,
    // the trailing buttons on the filter rows, the outfit tab strip and Rescan
    // (user 2026-08-12), so it is one function now rather than a constant copied
    // to each and free to drift.
    inline float EdgeClearance() { return FramePadding().x; }

    // ⚠ FUCK's ImGuiCol_ enum is TWO HIGHER than ours from ImGuiCol_WindowBg on.
    //
    // We compile against imgui 1.92.8 (vcpkg). FUCK.dll is built on a NEWER imgui
    // that inserts two entries straight after ImGuiCol_TextDisabled, so every
    // colour from ImGuiCol_WindowBg onward is off by two across the boundary.
    // Passing our raw value sets or reads the wrong slot, which is silent: the
    // widget simply keeps the theme's colour and nothing is logged.
    //
    // Measured from FUCK.pdb + FUCK.dll (1.6), four ways that agree:
    //   * ImGuiStyle::Colors spans 0x140..0x530, so 63 entries against our 61.
    //   * ImGui::StyleColorsDark writes stock WindowBg (0.06,0.06,0.06,0.94) at
    //     index 4, not 2, and Text/TextDisabled still at 0/1. That pins the
    //     insertion point to just after TextDisabled.
    //   * ImGui::OutlineButton fills with colour index 24; ours says Button = 22.
    //   * ImGui::Selectable takes its Header/Hovered/Active triple from 27/28/29;
    //     ours says Header = 25.
    // The count differs by exactly two and the shift is already two by index 2,
    // so no further entry can have been inserted later. +2 holds for the range.
    //
    // ImGuiStyleVar_ is NOT affected: FUCK's GStyleVarsInfo puts FramePadding at
    // 11 and ItemSpacing at 14, the same as ours. Only colours need translating.
    inline ImGuiCol FuckCol(ImGuiCol a_idx) {
        return a_idx >= ImGuiCol_WindowBg ? static_cast<ImGuiCol>(a_idx + 2) : a_idx;
    }

    // Push/read a style colour by OUR enum value, translated to FUCK's. Prefer
    // these over the raw FUCK:: calls anywhere the index is >= ImGuiCol_WindowBg.
    inline void PushStyleColor(ImGuiCol idx, const ImVec4& col) {
        FUCK::PushStyleColor(FuckCol(idx), col);
    }
    inline ImVec4 StyleColor(ImGuiCol idx) { return FUCK::GetStyleColorVec4(FuckCol(idx)); }

    // ImGui::GetColorU32(...) - FUCK returns ImVec4; pack to U32 for draw calls.
    inline ImU32 Col(ImGuiCol idx) { return ImGui::ColorConvertFloat4ToU32(StyleColor(idx)); }
    inline ImU32 Col(const ImVec4& v) { return ImGui::ColorConvertFloat4ToU32(v); }

    // ImGui::SetTooltip is printf-style; FUCK::SetTooltip takes only a plain
    // string, so pre-format here.
    //
    // ⚠⚠ 4K, AND IT WAS 1K UNTIL A TOOLTIP GREW PAST IT. vsnprintf truncates
    // in silence and returns the length it WOULD have written, which nobody
    // reads, so an over-long tooltip does not fail: it just stops mid-word and
    // looks like the text ends there. The Presets hover became the whole of
    // that page's detail column on 2026-08-12 (byline, description, per-piece
    // slot list, warnings, requires) and a full outfit clears 1K on its own.
    // Sized for the longest tooltip in the mod with room over it rather than
    // for that one caller, since every tooltip shares this buffer and the cost
    // is stack in a UI function.
    //
    // ⚠ THE "%s" AT EVERY CALL SITE IS NOT CEREMONY. FUCK::SetTooltip takes a
    // single const char* but names it `fmt`, so handing it a runtime string
    // whole would put a preset name or a mod-authored description where a
    // format is expected. Composed text goes through this as an ARGUMENT.
    inline void SetTooltipF(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        FUCK::SetTooltip(buf);
    }

    // Same pre-format step as SetTooltipF, but hands the string back instead of
    // showing it. For translated format strings that get composed into a larger
    // caption rather than printed straight out.
    inline std::string FormatF(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        return std::string(buf);
    }

    // ImDrawList::AddRectFilled replacement. FUCK::DrawRectFilled uses the WINDOW
    // drawlist (same as ImGui's), so hover boxes sit behind following text just
    // as before. (DrawScreenRectFilled is a foreground/on-top overlay - not this.)
    inline void RectFilled(const ImVec2& a, const ImVec2& b, ImU32 col, float rounding = 0.0f) {
        FUCK::DrawRectFilled(a, b, ImGui::ColorConvertU32ToFloat4(col), rounding);
    }

    // ImDrawList::AddImage replacement, and the image sibling of RectFilled:
    // absolute screen rect, straight onto the window draw list.
    //
    // ⚠ AddImage, NOT FUCK::DrawImage. DrawImage is the ImGui::Image-shaped
    // call and SUBMITS AN ITEM, which is the same trap TextAt carries above: a
    // following IsItemHovered() would then be asking about the picture rather
    // than about the InvisibleButton underneath it, and the bookend icons are
    // drawn precisely on top of one. This one draws and registers nothing.
    //
    // col is a TINT multiplied into the texture, so a white silhouette takes
    // the caller's colour exactly the way a glyph takes its text colour.
    // mirror=true flips the picture horizontally by swapping the U coordinates,
    // which costs nothing: the same texture is sampled right to left. That is
    // how one arm drawing serves both arm rows, and why there is no second PNG
    // that is only the first one backwards.
    inline void ImageAt(ImTextureID tex, const ImVec2& a, const ImVec2& b, ImU32 col,
                        bool mirror = false) {
        FUCK::AddImage(tex, a, b, ImVec2(mirror ? 1.0f : 0.0f, 0.0f),
                       ImVec2(mirror ? 0.0f : 1.0f, 1.0f),
                       ImGui::ColorConvertU32ToFloat4(col));
    }

    // ImDrawList::AddText replacement. FUCK has no positioned-text primitive, so
    // move the layout cursor, emit colored text, and restore the cursor (Text*
    // registers a real item + advances the cursor; AddText did not). Respects
    // text_end so a search-match substring can be drawn in its own colour.
    // restore=false leaves the cursor at the drawn text (the last op is an item,
    // so no dangling SetCursorPos → no ImGui "extend boundaries" warning) - use it
    // where the next op resets the cursor anyway (e.g. a table column). restore=true
    // is for inline use where a following SameLine must align to the original line
    // (a subsequent real item then clears the SetCursorPos flag before window end).
    inline void TextAt(const ImVec2& pos, ImU32 col, const char* text, const char* text_end = nullptr,
                       bool restore = true) {
        [[maybe_unused]] const ImVec2 saved = FUCK::GetCursorScreenPos();
        FUCK::SetCursorScreenPos(pos);
        FUCK::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
        FUCK::TextUnformatted(text, text_end);
        FUCK::PopStyleColor();
        if (restore) {
            FUCK::SetCursorScreenPos(saved);
        }
    }

    // FUCK::CalcTextSize takes a label and returns a size; this is the half
    // every caller here wants, and it keeps the button-measuring maths short.
    [[nodiscard]] inline float CalcTextWidth(const char* a_text) {
        return FUCK::CalcTextSize(a_text).x;
    }

    // ---- small marks drawn from primitives, not set as glyphs -------------
    //
    // Diagonal hatching inside a rect, at 45 degrees, clipped BY ARITHMETIC.
    //
    // ⚠ THE ARITHMETIC IS THE WHOLE POINT. FUCK exposes no clip rect, no
    // window draw list and no channels, so a line submitted past the rect is
    // simply drawn across whatever sits beside it: the neighbouring swatch,
    // the tile behind it, the pane. Every segment is solved against the four
    // edges here, before it goes in.
    //
    // The family of lines is x + y = c, which is the "/" diagonal once y grows
    // downward. Two lines whose c differs by d sit d/sqrt(2) apart measured
    // perpendicular to themselves, so the step for a wanted spacing is that
    // spacing times sqrt(2). Solving one line against the rect is then just
    // clamping x to the range where the matching y is inside it.
    //
    // ⚠ AND IT INSETS BY HALF A STROKE. ImGui builds a thick line as an offset
    // quad plus an anti-aliased fringe, both of which bleed perpendicular to
    // the segment, so a segment that ends exactly on the edge still puts ink
    // outside it and the corner strokes poke through the border.
    inline void HatchRect(const ImVec2& a_min, const ImVec2& a_max, ImU32 a_col,
                          float a_spacing, float a_thickness) {
        const float in = a_thickness * 0.5f;
        const float x0 = a_min.x + in;
        const float x1 = a_max.x - in;
        const float y0 = a_min.y + in;
        const float y1 = a_max.y - in;
        if (x1 <= x0 || y1 <= y0 || a_spacing <= 0.0f) {
            return;
        }
        const ImVec4 col  = ImGui::ColorConvertU32ToFloat4(a_col);
        const float  step = a_spacing * 1.41421356f;
        for (float c = x0 + y0 + step; c < x1 + y1; c += step) {
            const float xa = std::max(x0, c - y1);
            const float xb = std::min(x1, c - y0);
            if (xb <= xa) {
                continue;
            }
            FUCK::DrawLine(ImVec2(xa, c - xa), ImVec2(xb, c - xb), col, a_thickness);
        }
    }

    // A filled rect whose LEFT edge fades out, drawn as vertical bands.
    //
    // ⚠⚠ FUCK CARRIES NO GRADIENT. Every draw call in the ABI takes ONE colour:
    // there is no multi-colour rect and no per-vertex colour anywhere in it. So a
    // ramp is bands, which is what the colour picker in EditorUI already does,
    // and the three rules it paid for in the field come with it.
    //
    // ⚠ ONE BAND PER DEVICE PIXEL, never a constant count. A ramp quantised below
    // the pixel width has visible steps and the step is what the eye finds; the
    // picker's banding survived being made finer and only went away when the
    // count stopped being a constant.
    //
    // ⚠⚠ AND THE BANDS MUST NOT OVERLAP. Overlapping is the right instinct for
    // OPAQUE fills, because it hides the hairline a float edge can leave, and it
    // is exactly wrong for translucent ones: the overlap gets blended twice and
    // every seam comes out a band darker, which read as a grid of tiles (user
    // 2026-08-04, "are these all tiny buttons?!"). Both edges come from ONE
    // expression evaluated at the same i, so band i's far edge IS band i+1's near
    // edge to the bit, and DrawRectFilled at zero rounding is not antialiased so
    // nothing softens across the join either.
    //
    // a_knee is the fraction of the WIDTH the fade takes. The rest is one rect at
    // full strength, which is most of the cost saved and also the point: the fade
    // belongs on the leading edge, not under the text. a_col's own alpha is the
    // peak, so a caller can multiply a card's fade into it and hand it straight
    // over.
    inline void FadeRectLeft(const ImVec2& a_min, const ImVec2& a_max,
                             const ImVec4& a_col, float a_knee) {
        const float w = a_max.x - a_min.x;
        if (w <= 0.0f || a_max.y <= a_min.y) {
            return;
        }
        const float knee = std::clamp(a_knee, 0.0f, 1.0f) * w;
        if (knee > 0.0f) {
            const int bands = std::clamp(static_cast<int>(knee), 1, 2048);
            for (int i = 0; i < bands; ++i) {
                const float t0 = static_cast<float>(i) / bands;
                const float t1 = static_cast<float>(i + 1) / bands;
                const float t  = (t0 + t1) * 0.5f;
                ImVec4      c  = a_col;
                // smoothstep, so the fade starts and ends without a corner in it.
                c.w *= t * t * (3.0f - 2.0f * t);
                RectFilled(ImVec2(a_min.x + knee * t0, a_min.y),
                           ImVec2(a_min.x + knee * t1, a_max.y), Col(c));
            }
        }
        if (knee < w) {
            RectFilled(ImVec2(a_min.x + knee, a_min.y), a_max, Col(a_col));
        }
    }

    // An X centred in a rect, drawn as two strokes rather than set as a glyph.
    //
    // ⚠ A GLYPH CANNOT BE CENTRED IN A SMALL BOX AND STAY CRISP. CalcTextSize
    // returns the advance width and the LINE height, so where the ink actually
    // sits inside that box is a property of the baked atlas and differs per
    // glyph; the palette's padlock carries that same residue and says so. Two
    // strokes are centred by construction, take their size from the box rather
    // than from the font, and stay sharp at any scale.
    inline void CrossMark(const ImVec2& a_min, const ImVec2& a_max, ImU32 a_col, float a_side,
                          float a_thickness) {
        const float  cx  = (a_min.x + a_max.x) * 0.5f;
        const float  cy  = (a_min.y + a_max.y) * 0.5f;
        const float  h   = a_side * 0.5f;
        const ImVec4 col = ImGui::ColorConvertU32ToFloat4(a_col);
        FUCK::DrawLine(ImVec2(cx - h, cy - h), ImVec2(cx + h, cy + h), col, a_thickness);
        FUCK::DrawLine(ImVec2(cx - h, cy + h), ImVec2(cx + h, cy - h), col, a_thickness);
    }

    // A chevron centred in a rect, pointing left or right, drawn as two strokes.
    //
    // ⚠ NOT A GLYPH, for CrossMark's reason and one that is sharper here: the
    // icon set carries chevron-up and chevron-down and NO left or right pair,
    // so drawing one as text means adding a codepoint and trusting a cmap that
    // is not the atlas FLICK actually bakes from. A wrong guess comes back as
    // tofu and only a screenshot can say so. Two strokes cannot.
    //
    // a_side is the mark's full height; the arms reach a quarter of it either
    // side of centre, which is the proportion a chevron glyph has.
    inline void Chevron(const ImVec2& a_min, const ImVec2& a_max, ImU32 a_col,
                        float a_side, float a_thickness, bool a_right) {
        const float  cx   = (a_min.x + a_max.x) * 0.5f;
        const float  cy   = (a_min.y + a_max.y) * 0.5f;
        const float  h    = a_side * 0.5f;
        const float  arm  = h * 0.5f;
        const float  tip  = a_right ? cx + arm : cx - arm;
        const float  back = a_right ? cx - arm : cx + arm;
        const ImVec4 col  = ImGui::ColorConvertU32ToFloat4(a_col);
        FUCK::DrawLine(ImVec2(back, cy - h), ImVec2(tip, cy), col, a_thickness);
        FUCK::DrawLine(ImVec2(back, cy + h), ImVec2(tip, cy), col, a_thickness);
    }

    // A solid triangle centred in a rect, pointing left or right.
    //
    // ⚠⚠ THE TRIANGLE PRIMITIVE IS API VERSION 3 AND NO-OPS BELOW IT, which is
    // the trap ChamferPanel::HasQuads guards for the chamfer. It is worth
    // guarding here too and the reason is specific to this mark: it is the only
    // thing telling a player the row runs on past the edge, so an older host
    // would get a strip that silently looks finished. Chevron above is the
    // fallback because DrawLine has been in the ABI since version 1.
    //
    // ⚠ CLOCKWISE IN SCREEN SPACE, which y-down inverts from the usual test.
    // ImGui fills either winding, but only one of them gets the antialiased
    // edge on the outside of the shape.
    inline void TriangleMark(const ImVec2& a_min, const ImVec2& a_max, ImU32 a_col,
                             float a_height, bool a_right) {
        const auto* iface = FUCK::GetInterface();
        if (!iface || iface->version < 3 || !iface->DrawTriangleFilled) {
            Chevron(a_min, a_max, a_col, a_height,
                    std::max(1.0f, FUCK::GetResolutionScale()), a_right);
            return;
        }
        // Whole pixels, for the reason ChamferPanel::Snap carries: a straight
        // edge on a fractional coordinate spreads over two rows at partial
        // coverage, and at this size that is most of the mark.
        const float  cx   = std::round((a_min.x + a_max.x) * 0.5f);
        const float  cy   = std::round((a_min.y + a_max.y) * 0.5f);
        const float  h    = a_height * 0.5f;
        const float  w    = a_height * 0.42f;
        const float  tip  = a_right ? cx + w : cx - w;
        const float  back = a_right ? cx - w : cx + w;
        const ImVec4 col  = ImGui::ColorConvertU32ToFloat4(a_col);
        if (a_right) {
            FUCK::DrawTriangleFilled(ImVec2(back, cy - h), ImVec2(tip, cy),
                                     ImVec2(back, cy + h), col);
        } else {
            FUCK::DrawTriangleFilled(ImVec2(back, cy + h), ImVec2(tip, cy),
                                     ImVec2(back, cy - h), col);
        }
    }

    // A loading arc centred in a rect, drawn as strokes for CrossMark's
    // reason: FUCK exposes no arc or circle primitive, so the ring is a fan
    // of short line segments. 270 degrees of them, rotated by a_turn (pass
    // GetTime(); one revolution per second), which is what makes a waiting
    // tile read as work in progress rather than as an empty pane.
    inline void SpinnerArc(const ImVec2& a_min, const ImVec2& a_max, ImU32 a_col,
                           float a_radius, float a_thickness, double a_turn) {
        constexpr int   kSegments = 12;
        constexpr float kSweep    = 4.71238898f;  // 270 degrees
        constexpr float kTau      = 6.28318531f;
        const float  cx    = (a_min.x + a_max.x) * 0.5f;
        const float  cy    = (a_min.y + a_max.y) * 0.5f;
        const ImVec4 col   = ImGui::ColorConvertU32ToFloat4(a_col);
        const float  phase =
            static_cast<float>(a_turn - std::floor(a_turn)) * kTau;
        float px = 0.0f;
        float py = 0.0f;
        for (int i = 0; i <= kSegments; ++i) {
            const float a = phase + kSweep * (static_cast<float>(i) / kSegments);
            const float x = cx + std::cos(a) * a_radius;
            const float y = cy + std::sin(a) * a_radius;
            if (i > 0) {
                FUCK::DrawLine(ImVec2(px, py), ImVec2(x, y), col, a_thickness);
            }
            px = x;
            py = y;
        }
    }

    // Disabled-colour prose that WRAPS. FUCK::TextDisabled does not wrap, so a
    // sentence in a panel narrower than itself is simply cut off at the right
    // edge with no mark that anything was dropped (user 2026-08-07, on the
    // Shape page's intro line). TextWrapped is the call that wraps and it draws
    // in ImGuiCol_Text, so the dimming has to be pushed around it.
    //
    // ⚠ HEADINGS KEEP PLAIN TextDisabled. Wrapping a single word buys nothing
    // and a heading that folds onto two lines is worse than one that does not.
    // This is for the lines that are sentences.
    inline void TextDisabledWrapped(const char* a_text) {
        PushStyleColor(ImGuiCol_Text, StyleColor(ImGuiCol_TextDisabled));
        FUCK::TextWrapped("%s", a_text);
        FUCK::PopStyleColor();
    }

    // ---- one shape for every modal in the editor -------------------------
    //
    // The one name both halves of a modal must agree on.
    //
    // ⚠ THE "##" IS THE CAPTION FIX. A popup is titled with the string it is
    // keyed by, and these keys are ids rather than prose, so a confirmation
    // came up captioned "leave_unsaved" (user 2026-08-07). ImGui measures a
    // window title with hide_text_after_double_hash set, so a name that opens
    // with ## draws an empty title bar and keeps the id doing its own job.
    //
    // ⚠ ## AND NOT ###, WHICH IS THE ONE THAT LOOKS RIGHT AND IS NOT. ImHashStr
    // restarts its CRC when it meets ###, so "delete_shape" and
    // "Caption###delete_shape" hash differently and the box would never open;
    // ## carries no such rule, so both halves hash the same whole string. That
    // is only safe while ONE helper builds the name for both, which is why
    // OpenModal exists rather than leaving callers on FUCK::OpenPopup.
    //
    // ⚠ kNoDecoration DOES NOT SUPPRESS THE CAPTION. That was the first fix,
    // it shipped, and the field brought the caption back unchanged: the flag is
    // FUCK's own and its host does not put it on the popup path.
    //
    // ⚠ AND NEITHER DOES BeginPopup, WHICH WAS THE SECOND FIX. It has no title
    // bar at all, being the entry point that hard-codes NoTitleBar, but it is
    // not modal: it lost the centring and the dimmed backdrop, and the user
    // wants those (2026-08-07, "i prefer having it centered and white
    // transparent background"). The caption was never worth that trade.
    inline std::string ModalName(const char* a_id) { return std::string("##") + a_id; }

    // Ask for a modal. Pairs with BeginModal below and MUST be used instead of
    // FUCK::OpenPopup for anything that one begins.
    inline void OpenModal(const char* a_id) { FUCK::OpenPopup(ModalName(a_id).c_str()); }

    // Every modal in this editor goes through here so they cannot drift apart.
    //
    // ⚠ THE kAutoResize BELOW HAS NEVER DONE ANYTHING, and the note above says
    // why without having drawn the conclusion: FUCK's window flags are its
    // OWN, and its host does not put them on the popup path. That was proven
    // in the field with kNoDecoration, which shipped and came back with the
    // caption unchanged. kAutoResize is the same enum on the same call, so
    // every modal here has been sizing itself by ImGui's default all along.
    // Short ones fit it and nobody noticed; a tutorial card whose text ran past
    // it grew a SCROLLBAR and clipped its own buttons (user 2026-08-08, with a
    // screenshot). It is left in place because removing it would look like a
    // behaviour change and is not one.
    [[nodiscard]] inline bool BeginModal(const char* a_id) {
        return FUCK::BeginPopupModal(ModalName(a_id).c_str(), nullptr,
                                     FUCK::WindowFlags::kAutoResize);
    }

    // Fix the NEXT modal's width, and let its height follow its content.
    //
    // ⚠ SetNextWindowSize IS ImGui'S OWN CALL and does reach the popup, which
    // is the whole reason this exists rather than a better flag. A zero on an
    // axis is ImGui's "fit this one to the contents", so a width goes in and
    // the height still follows the text, which is what kAutoResize was being
    // asked for and never delivered.
    //
    // ⚠ OPT IN, NOT FOLDED INTO BeginModal. The eight modals already in this
    // editor size themselves by the default and none has been reported wrong,
    // so giving all of them a width in one edit would be changing eight things
    // to fix one. Call this only where the content is long enough to need it.
    inline void NextModalWidth(float a_width) {
        FUCK::SetNextWindowSize(ImVec2(a_width, 0.0f));
    }

    // ---- one shape for every right-click menu in the editor ---------------
    //
    // ⚠ OPEN AND BEGIN MUST SIT AT THE SAME ID-STACK DEPTH, and everything
    // about this shape follows from that. Both hash their name against the
    // CURRENT id stack, so a menu opened from inside a PushID and begun outside
    // it is two different popups: the open is real, the begin never matches,
    // and the control silently does nothing. That is OS-29, which this file has
    // already cost the project three times.
    //
    // So a caller records WHICH item was right-clicked and does nothing else.
    // Both halves then run at window scope, where the id stack is empty and
    // cannot differ. The tab strip is the case that forces it: its rows live
    // inside BeginTabBar, which pushes an id of its own, AND inside a PushID
    // per outfit.
    //
    // ⚠ BeginPopupContextItem WOULD SIDESTEP THAT by doing both at one depth,
    // and is deliberately not used. It has zero call sites in this project, it
    // would be running under those same two pushes, and nothing here compiles
    // into a test, so it would be an unexercised third-party call on a path
    // that reaches the field directly. The deferred shape has shipped
    // precedents in this editor. If it is ever adopted, note that its second
    // parameter is a bare int mouse button rather than a PopupFlags.
    //
    // ⚠ AND THE WHOLE FAMILY IS version >= 3 GATED inside FUCK, silently doing
    // nothing on an older host. That is not a live risk, because every modal in
    // this editor already goes through the same gate and they demonstrably open
    // in the field, but it is why a menu that never appears is worth checking
    // the host version for before it is worth debugging the ids.
    inline std::string ContextMenuName(const char* a_id) {
        return std::string("##ctx_") + a_id;
    }

    // Ask for a menu. ⚠ CALL AT WINDOW SCOPE, from a flag the item set.
    inline void OpenContextMenu(const char* a_id) {
        FUCK::OpenPopup(ContextMenuName(a_id).c_str());
    }

    // ⚠ AT THE SAME DEPTH AS OpenContextMenu. BeginPopup hard-codes NoTitleBar,
    // so unlike BeginModal this one needs no caption fix; the shared name is
    // still built in one place so the two halves cannot drift.
    [[nodiscard]] inline bool BeginContextMenu(const char* a_id) {
        return FUCK::BeginPopup(ContextMenuName(a_id).c_str());
    }

    // One row of a menu.
    //
    // ⚠ Selectable, BECAUSE MenuItem IS ABSENT from the FUCK API. Stock
    // Selectable closes the popup it was clicked in, and FUCK's is verbatim
    // stock, but the close is asked for outright rather than inherited: this is
    // the one behaviour a menu row cannot be wrong about, and a row that leaves
    // its menu standing after acting reads as a control that did not fire.
    [[nodiscard]] inline bool ContextMenuItem(const char* a_label) {
        if (FUCK::Selectable(a_label)) {
            FUCK::CloseCurrentPopup();
            return true;
        }
        return false;
    }

    inline void EndContextMenu() { FUCK::EndPopup(); }

    // True on the frame the item just submitted was right-clicked.
    //
    // ⚠ THE HOVER AND THE BUTTON ARE READ SEPARATELY, following the dye
    // stripe's right-click clear rather than IsItemClicked(1), so it does not
    // depend on which button flags a given wrapper forwards.
    [[nodiscard]] inline bool ItemRightClicked() {
        return FUCK::IsItemHovered() && FUCK::IsMouseClicked(1);
    }

    // Put the cursor where a row of two buttons will sit centred.
    //
    // Call immediately before submitting them, with the same labels. Measuring
    // them here rather than taking a width means a translated label cannot
    // push the row off centre.
    inline void CentreTwoButtons(const char* a_first, const char* a_second) {
        const float pad   = FramePadding().x * 2.0f;
        const float total = CalcTextWidth(a_first) + CalcTextWidth(a_second) + pad * 2.0f +
                            ItemSpacing().x;
        const float avail = FUCK::GetContentRegionAvail().x;
        if (avail > total) {
            FUCK::SetCursorPosX(FUCK::GetCursorPos().x + (avail - total) * 0.5f);
        }
    }

    // The three-button sibling, for a Back / Next / Skip row.
    //
    // ⚠ THE SAME ARITHMETIC AS CentreTwoButtons ON PURPOSE, INCLUDING ITS
    // UNCERTAINTY. FUCK::Button is ImGui::OutlineButton, which was measured out
    // of the PDB as sizing itself label.x + 2*(8*scale) and never reading
    // style.FramePadding, so a padding-based estimate is only right while the
    // style's FramePadding.x happens to equal 8*scale. Every modal in this
    // editor already centres on that estimate and none has been reported
    // crooked, so this matches them rather than being right alone: three
    // buttons centred by one rule and two by another would be a visible seam
    // whichever rule is correct. If a row does come out off centre, fix both
    // here together, and measure a real button with GetItemRectSize first
    // rather than picking a new formula.
    inline void CentreThreeButtons(const char* a_first, const char* a_second,
                                   const char* a_third) {
        const float pad   = FramePadding().x * 2.0f;
        const float total = CalcTextWidth(a_first) + CalcTextWidth(a_second) +
                            CalcTextWidth(a_third) + pad * 3.0f + ItemSpacing().x * 2.0f;
        const float avail = FUCK::GetContentRegionAvail().x;
        if (avail > total) {
            FUCK::SetCursorPosX(FUCK::GetCursorPos().x + (avail - total) * 0.5f);
        }
    }

}  // namespace OS::ui
