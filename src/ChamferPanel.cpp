#include "ChamferPanel.h"

#include "FramePolicy.h"  // carved or plain, and what decides
#include "FuckCompat.h"  // FUCK, and OS::ui's FontSize
#include "IconImages.h"  // Frame / FrameFill, the two halves of the corner art
#include "PadFocus.h"    // the controller's own cursor, for buttons that name a pane
#include "Settings.h"    // the style override

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>

namespace OS::ChamferPanel {

    namespace {

        // ⚠ THE QUAD PRIMITIVES ARE API VERSION 3, AND BELOW THAT THEY NO-OP.
        // FUCK_API.h guards DrawQuad, DrawQuadFilled and the triangle family
        // with `i->version >= 3`, so against an older FLICK every call here
        // would return silently and a chamfered tile would render as nothing at
        // all. An invisible button is a much worse failure than a square one, so
        // ask first and fall back to the rectangle primitives, which have been
        // in the ABI since version 1.
        //
        // ⚠ THE EDITOR ALREADY REQUIRES VERSION 3 ELSEWHERE and this guard is
        // still not redundant. The gold fold on a card and on a swatch are both
        // bare DrawTriangleFilled calls with no check, so on an older host those
        // marks are already silently absent - a mark nobody sees. A tile that
        // does not draw is a control nobody can find, which is a different order
        // of failure, and it is the one worth spending a branch on.
        [[nodiscard]] bool HasQuads() {
            const auto* i = FUCK::GetInterface();
            return i && i->version >= 3 && i->DrawQuadFilled && i->DrawQuad &&
                   i->DrawTriangleFilled;
        }

        [[nodiscard]] ImVec2 Vec(const Policy::Point& a_p) { return ImVec2{ a_p.x, a_p.y }; }

        // Whole-pixel bounds. A chamfer is mostly straight edges, and a straight
        // edge on a fractional coordinate spreads across two rows at partial
        // coverage and reads as a soft, unevenly weighted frame. Rounded inward
        // so the panel can only ever shrink into its rect, never grow out of it
        // and meet a clip edge.
        [[nodiscard]] Policy::Rect Snap(const ImVec2& a_min, const ImVec2& a_max, float a_cut,
                                        unsigned a_corners) {
            return Policy::Rect{
                Policy::Point{ std::ceil(a_min.x), std::ceil(a_min.y) },
                Policy::Point{ std::floor(a_max.x), std::floor(a_max.y) },
                a_cut,
                a_corners,
            };
        }

        [[nodiscard]] bool Degenerate(const Policy::Rect& a_r) {
            return Policy::Width(a_r) <= 0.0f || Policy::Height(a_r) <= 0.0f;
        }

    }  // namespace

    float StandardCut() { return OS::ui::FontSize() * 0.6f; }

    float CutFor(const ImVec2& a_min, const ImVec2& a_max) {
        const float shorter = std::min(a_max.x - a_min.x, a_max.y - a_min.y);
        return std::min(StandardCut(), std::max(0.0f, shorter) * 0.25f);
    }

    // ---- carved or plain, asked once and answered here ---------------------
    //
    // ⚠⚠ THE DECISION LIVES IN THE FOUR DRAW CALLS AND NOT AT THEIR CALL SITES,
    // AND THAT IS THE WHOLE FIX. The first cut of this put the branch in the
    // editor window only, so the window plate went plain while every button,
    // tab and rail tile in it stayed carved: the setting appeared to do almost
    // nothing (field 2026-08-14, "we still see 9 slice corners when changing the
    // theme"). Everything this editor carves goes through Fill, Stroke,
    // FillImage or FrameImage, so asking here reaches all of it, including call
    // sites in EditorUI that this file has never heard of.
    // ⚠⚠ NOTHING READS THE LIVE PRESET ANY MORE. LivePresetIsCarved sat
    // here until 2026-08-28: an INI read of FUCKs/FUCK/defaultstyle.ini behind
    // a one-second cache, because auto had to know which theme was up. Auto is
    // gone (FramePolicy.h has the field call), so the file is no longer opened
    // at all and the cache, its mutex and its "preset '<name>' -> plain" line
    // went with it. That line was also a trap worth remembering: it logged the
    // preset every time, including on the frames where Resolve discarded the
    // answer, so it never meant "plain was drawn".
    [[nodiscard]] bool DrawPlainImpl() {
        return OS::FramePolicy::Resolve(OS::FramePolicy::StyleFromIni(
                   OS::Settings::GetSingleton().frameStyle)) ==
               OS::FramePolicy::Shape::kPlain;
    }

    // The header's public question. Forwards to the same answer the draw calls
    // use, so a caller cannot get a different one.
    bool PlainShape() { return DrawPlainImpl(); }

    namespace {
        // ⚠⚠ PLAIN HAS NO SINGLE RADIUS, AND EVERY ATTEMPT TO GIVE IT ONE HAS
        // FAILED IN THE FIELD. The theme's number made hard rectangles, 8px on
        // everything read bubbly on the tab strip and the name field, and 8px on
        // buttons alone was a third guess nobody had seen. The reference commit
        // fde7e3f, which is what the user means by "we had it working", never
        // had one number either: it asked the THEME for cards and slot tiles,
        // used a fraction of the widget for the rail tiles with an explicit note
        // not to use the theme's there, and drew the editor frame hard square.
        // Three surfaces, three answers, and the shape work replaced all three
        // with a knob.
        //
        // So the default is zero, which makes PlainRadiusFor return the theme's
        // own FrameRounding and reproduces the card rule exactly, and a surface
        // that had its own number in vanilla asks for it by name below.
        //
        // ⚠ A SCOPED OVERRIDE RATHER THAN A PARAMETER ON THE FOUR DRAW CALLS.
        // The four calls still own the CARVED-or-PLAIN decision, which is wrong
        // turn 3 and must not move back to a call site. This carries a RADIUS,
        // which is a different thing: it is a property of the surface being
        // painted and vanilla proved it belongs there. Present-thread only, like
        // every other static in this file.
        float g_plainRadiusOverride = -1.0f;
    }  // namespace

    PlainRadiusScope::PlainRadiusScope(float a_radius) : prev(g_plainRadiusOverride) {
        g_plainRadiusOverride = a_radius;
    }

    PlainRadiusScope::~PlainRadiusScope() { g_plainRadiusOverride = prev; }

    // The radius a plain shape draws this rect with, in final pixels.
    //
    // ⚠⚠ THE THEME'S, UNLESS THE SURFACE CARRIES ITS OWN AND ASKS FOR MORE.
    // That is vanilla's rule restored rather than a new one: the preview card
    // and the slot tile passed OS::ui::FrameRounding() straight through, and
    // with no override in scope this returns exactly that.
    [[nodiscard]] float PlainRadiusFor(const ImVec2& a_min, const ImVec2& a_max) {
        const auto* const i = FUCK::GetInterface();
        const float rounding =
            (i && i->GetStyleVar) ? FUCK::GetStyleVar(ImGuiStyleVar_FrameRounding) : 0.0f;
        // ⚠ THE THEME'S NUMBER UNLESS A SURFACE IS ASKING. g_plainRadiusOverride
        // is set only by a PlainRadiusScope; everything else gets zero, and zero
        // means "follow the theme exactly", which is what the card did.
        const float ours =
            (g_plainRadiusOverride >= 0.0f)
                ? g_plainRadiusOverride
                : 0.0f;
        // ⚠ ONE LINE, ONCE, BECAUSE TWO ROUNDS WENT ON GUESSING WHAT THE THEME
        // HELD. "We lost the rounded buttons" was a 1.0 rounding being honoured
        // faithfully, and nothing on disk said so until the style INI was read
        // by hand. This prints what the ABI actually answers, so the next report
        // about a shape is a comparison rather than another round of reasoning.
        static bool once = false;
        if (!once) {
            once = true;
            spdlog::info("ChamferPanel: FLICK iface={} GetStyleVar={} FrameRounding={:.2f} "
                         "WindowRounding={:.2f}; ours={:.2f} (surface override, -1 = none) "
                         "scale={:.2f}; style={} -> {}",
                         i != nullptr, (i && i->GetStyleVar) ? "yes" : "MISSING", rounding,
                         (i && i->GetStyleVar) ? FUCK::GetStyleVar(ImGuiStyleVar_WindowRounding)
                                               : 0.0f,
                         g_plainRadiusOverride, FUCK::GetResolutionScale(),
                         OS::Settings::GetSingleton().frameStyle,
                         DrawPlainImpl() ? "PLAIN" : "CARVED");
        }
        return OS::FramePolicy::ClampRadius(OS::FramePolicy::PlainRadius(rounding, ours),
                                            a_max.x - a_min.x, a_max.y - a_min.y);
    }

    void Fill(const ImVec2& a_min, const ImVec2& a_max, const ImVec4& a_col, float a_cut,
              unsigned a_corners) {
        if (a_col.w <= 0.0f) {
            return;
        }
        if (DrawPlainImpl()) {
            // ⚠ THE CORNER MASK IS DROPPED AND IT HAS TO BE: the ABI's rounded
            // rect takes no per-corner flags. A plain shape rounds all four or
            // none, which is what every other widget in a plain theme does, so
            // there is nothing to match by keeping the mask.
            FUCK::DrawRectFilled(a_min, a_max, a_col, PlainRadiusFor(a_min, a_max));
            return;
        }
        const Policy::Rect rect = Snap(a_min, a_max, a_cut, a_corners);
        if (Degenerate(rect)) {
            return;
        }
        // A square panel costs exactly what a plain rectangle costs, and the
        // fallback path is the same call, so an old FLICK degrades to the look
        // this editor had before the chamfer existed rather than to nothing.
        if (!HasQuads() || Policy::ClampCut(rect) <= 0.0f) {
            FUCK::DrawRectFilled(Vec(rect.min), Vec(rect.max), a_col, 0.0f);
            return;
        }
        const Policy::Fill fill = Policy::BuildFill(rect);
        for (int i = 0; i < fill.count; ++i) {
            const Policy::Quad& q = fill.quads[i];
            FUCK::DrawQuadFilled(Vec(q.v[0]), Vec(q.v[1]), Vec(q.v[2]), Vec(q.v[3]), a_col);
        }
    }

    // ⚠ STACKED ONE-PIXEL STROKES, NOT ONE THICK ONE. A thick polyline is mitred
    // and anti-aliased along its joints, so its apparent width differs between
    // the straights and the 45 degree cuts by construction, and a chamfer is
    // nothing but joints. Each single-pixel pass, centred half a pixel inside an
    // integer edge, lands on exactly one row and needs no coverage blending, so
    // N of them stack into a band that is N pixels everywhere. Same reasoning and
    // the same fix as the dye swatch and the card border already use.
    //
    // Policy::Inset is what makes the stack correct rather than merely repeated:
    // it walks the cut in as well as the sides, so pass N stays parallel to pass
    // 0 instead of splaying at the corners. See kInsetCutScale for the geometry.
    void Stroke(const ImVec2& a_min, const ImVec2& a_max, const ImVec4& a_col, float a_cut,
                float a_thickness, unsigned a_corners) {
        if (a_col.w <= 0.0f || a_thickness <= 0.0f) {
            return;
        }
        if (DrawPlainImpl()) {
            // ⚠ THE SAME RADIUS Fill USED, on the same rect. An outline drawn at
            // a different radius than its fill retreats out of its own corners
            // and the window shows through beside the line, which is the exact
            // fault ArtCorner exists to prevent on the carved path.
            FUCK::DrawRect(a_min, a_max, a_col, PlainRadiusFor(a_min, a_max), a_thickness);
            return;
        }
        const Policy::Rect base = Snap(a_min, a_max, a_cut, a_corners);
        if (Degenerate(base)) {
            return;
        }
        const int passes = std::max(1, static_cast<int>(std::floor(a_thickness)));

        // The fallback has to match Fill's, or an old FLICK draws a CUT outline
        // around a fill that has just fallen back to a SQUARE rectangle, leaving
        // the fill's corners sticking out past their own border. Worse than
        // either shape on its own.
        if (!HasQuads() || Policy::ClampCut(base) <= 0.0f) {
            for (int pass = 0; pass < passes; ++pass) {
                const float step = static_cast<float>(pass);
                FUCK::DrawRect(ImVec2{ base.min.x + step + 0.5f, base.min.y + step + 0.5f },
                               ImVec2{ base.max.x - step - 0.5f, base.max.y - step - 0.5f },
                               a_col, 0.0f, 1.0f);
            }
            return;
        }

        for (int pass = 0; pass < passes; ++pass) {
            const Policy::Rect ring = Policy::Inset(base, static_cast<float>(pass) + 0.5f);
            if (Degenerate(ring)) {
                break;
            }
            const Policy::Outline outline = Policy::BuildOutline(ring);
            for (int i = 0; i < outline.count; ++i) {
                const Policy::Point& p = outline.v[i];
                const Policy::Point& q = outline.v[(i + 1) % outline.count];
                FUCK::DrawLine(Vec(p), Vec(q), a_col, 1.0f);
            }
        }
    }

    void Image(ImTextureID a_tex, const ImVec2& a_min, const ImVec2& a_max,
               const ImVec4& a_tint, float a_cut, unsigned a_corners) {
        if (!a_tex || a_tint.w <= 0.0f) {
            return;
        }
        const Policy::Rect rect = Snap(a_min, a_max, a_cut, a_corners);
        if (Degenerate(rect)) {
            return;
        }
        // ⚠ AddImage, NOT DrawImage, on the fallback. DrawImage is the
        // ImGui::Image-shaped call and SUBMITS AN ITEM, which would make the
        // card's following hit tests ask about the picture instead of the
        // InvisibleButton underneath it. Same trap FuckCompat's ImageAt records.
        if (!HasQuads() || Policy::ClampCut(rect) <= 0.0f) {
            FUCK::AddImage(a_tex, Vec(rect.min), Vec(rect.max), ImVec2{ 0.0f, 0.0f },
                           ImVec2{ 1.0f, 1.0f }, a_tint);
            return;
        }
        // The UVs come from the SNAPPED rect, not the one that was asked for, so
        // the texture stays registered to the pixels actually being painted. Off
        // by the snap it would be a sub-pixel stretch, which on a thumbnail of a
        // face is the kind of thing that reads as a blurry card and gets blamed
        // on the preview cache.
        const float w = Policy::Width(rect);
        const float h = Policy::Height(rect);
        const auto  uv = [&](const Policy::Point& a_p) {
            return ImVec2{ (a_p.x - rect.min.x) / w, (a_p.y - rect.min.y) / h };
        };
        const Policy::Fill fill = Policy::BuildFill(rect);
        for (int i = 0; i < fill.count; ++i) {
            const Policy::Quad& q = fill.quads[i];
            FUCK::DrawImageQuad(a_tex, Vec(q.v[0]), Vec(q.v[1]), Vec(q.v[2]), Vec(q.v[3]),
                                uv(q.v[0]), uv(q.v[1]), uv(q.v[2]), uv(q.v[3]), a_tint);
        }
    }

    // ⚠ 20 IS THE TEXTURE'S OWN SLICE and it is spelled out twice in this file,
    // here and in kSliceUV below. Both come from tools/make_frame_texture.py's
    // SLICE, and if that changes both change: the UV decides what is sampled and
    // this decides how big it is drawn, so a mismatch stretches the corner art
    // rather than failing.
    float FrameCorner() { return 20.0f * std::max(1.0f, FUCK::GetResolutionScale()); }

    float ArtCorner(const ImVec2& a_min, const ImVec2& a_max) {
        const float shorter = std::min(a_max.x - a_min.x, a_max.y - a_min.y);
        return std::max(1.0f, std::min(FrameCorner(), std::max(0.0f, shorter) * 0.25f));
    }

    // SCOOP_R over SLICE, plus a line width so a fill lands beside the frame's
    // stroke rather than under its inner edge.
    float FrameClearance() {
        const float res = std::max(1.0f, FUCK::GetResolutionScale());
        return FrameCorner() * (12.0f / 20.0f) + res;
    }

    // The live framed-widget height, measured off a real InputText. Zero until
    // the first field of the session draws; FrameWidgetHeight falls back to
    // the formula below until then. Written every frame a field draws, so a
    // UI-scale change corrects it one frame later without anyone resetting it.
    static float s_measuredFrameWidgetH = 0.0f;

    float FrameWidgetHeight() {
        if (s_measuredFrameWidgetH > 0.0f) {
            return s_measuredFrameWidgetH;
        }
        // The OutlineButton formula out of FLICK's PDB: label plus 2*(8*scale)
        // on both axes. Scale choice discussed at the declaration.
        return FUCK::GetTextLineHeight() + 2.0f * FUCK::Scale(8.0f);
    }

    void NoteFrameWidgetHeight() {
        const float h = FUCK::GetItemRectSize().y;
        if (h <= 0.0f) {
            return;
        }
        // One line, once, so the fallback formula is checked against a real
        // widget on every rig that ever draws a field. A disagreement here
        // means the 8*scale guess is wrong on this host and the measured value
        // is quietly doing all the work.
        static bool logged = false;
        if (!logged) {
            logged              = true;
            const float formula = FUCK::GetTextLineHeight() + 2.0f * FUCK::Scale(8.0f);
            spdlog::info("[chamfer] frame widget height measured {:.1f} (formula {:.1f}, "
                         "GetFrameHeight {:.1f})",
                         h, formula, FUCK::GetFrameHeight());
        }
        s_measuredFrameWidgetH = h;
    }

    // ⚠⚠ A GLYPH CENTRES BY PLAIN CalcTextSize ARITHMETIC AND NEEDS NO NUDGE,
    // AND A DERIVED NUDGE SHIPPED AND WAS FIELD-REJECTED THE SAME DAY. This
    // comment is what remains of it, so nobody re-derives it.
    //
    // The derivation looked sound: fontTools on the two shipped files says FA5
    // Free Solid centres its ink 0.375em above its own baseline (checked
    // across eleven codepoints, every midpoint on 0.375 to within 0.008), and
    // Jost-Regular's baseline sits at 1.070/1.445 of the line height, so ink
    // centred against JOST's baseline would ride 0.1345 of a line high, about
    // 3px. The wrong step was assuming FA glyphs sit on Jost's baseline at
    // all. FA5's own vertical metrics are ascent 0.875 / descent -0.125, which
    // puts its ink centre at exactly 0.5 of its pixel height: the font centres
    // itself, by design.
    //
    // The measured evidence was already in the tree, twice, before the
    // derivation was written: DrawRailTile and Menu Studio's action bar both
    // centre glyphs by plain (box - CalcTextSize) / 2 and both are
    // field-confirmed. The +3.77px it added is the "icon is not centred in the
    // button now" of 2026-08-12. See
    // a-glyph-atlas-you-do-not-ship-cannot-be-verified-offline: FLICK's bake
    // lives inside FUCK.dll, so the only measurements that bind are the ones
    // made on screen.

    void PaintButtonBox(const ImVec2& a_min, const ImVec2& a_max, bool a_hovered,
                        bool a_held, bool a_disabled) {
        // ⚠ NO PLAIN RADIUS HERE, AND THE REFERENCE COMMIT IS WHY. A button on
        // the plain path draws square, because vanilla's buttons were
        // FUCK::Button - ImGui::OutlineButton - which has no radius to give.
        // Rounding them was this editor's invention, not something restored,
        // and 8px on everything then 8px on buttons alone were both attempts to
        // find a number for a surface that never had one (user 2026-08-14,
        // "vanilla exactly").
        // ⚠⚠ TWO OF THESE THREE ARE THE THEME'S AND THE THIRD IS NOT, AND THAT
        // IS THE WHOLE OF WHY THIS IS NOT ONE StyleColor CALL. Measured on this
        // rig, 2026-08-12, by the [chamfer] style line:
        //
        //     Button  = (0.11, 0.10, 0.09, 0.92)   Vel'dun's #22201CEB
        //     Hovered = (0.23, 0.20, 0.17, 0.94)   Vel'dun's #3A342CF0
        //     Active  = (1.00, 1.00, 1.00, 1.00)   nobody's
        //     Text    = (1.00, 1.00, 1.00, 1.00)
        //
        // FLICK maps the theme's [Widget] rBackgroundColor onto Button and its
        // rBackgroundActiveColor onto ButtonHovered, so those two ARE authored
        // and are worth keeping: they are the pair the preset designed its text
        // to be read against. ImGuiCol_ButtonActive is not mapped from anything
        // and sits at FLICK's own white, which is the same colour as Text, so
        // reading it filled a held button solid white and then drew its label
        // invisibly on top (user 2026-08-12, "the click state highlight not
        // being consistent").
        //
        // ⚠ THE FIRST FIX FOR THAT REPLACED ALL THREE WITH LOW-ALPHA WHITE, the
        // way DrawRailTile builds its tiles, and that was the wrong trade: it
        // discarded two colours the preset really had authored in order to route
        // around the one it had not. The rail has its OWN reason for refusing the
        // family, recorded there, and it is about the theme's gold meaning
        // something specific on those tiles. It is not a reason that applies out
        // here.
        //
        // So the press is EXTRAPOLATED from the two authored points rather than
        // invented: the theme drew a ramp from resting to hovered, and a press is
        // one more step along that same ramp. It follows any preset, it goes
        // darker on a light theme exactly as it goes lighter on a dark one, and
        // it can only collide with the text colour if the theme's own hover
        // already does.
        const ImVec4 base  = OS::ui::StyleColor(ImGuiCol_Button);
        const ImVec4 hover = OS::ui::StyleColor(ImGuiCol_ButtonHovered);

        // ⚠ A THEME IS ALLOWED TO AUTHOR NO RAMP AT ALL, and then extrapolating
        // returns the hover colour and a press answers with nothing. A press is
        // a deliberate act and wants an answer, so that case lifts toward white
        // instead. Menu Studio's action bar reached the same conclusion in its
        // own words and for its own surface.
        const float spread = std::abs(hover.x - base.x) + std::abs(hover.y - base.y) +
                             std::abs(hover.z - base.z);
        const auto  step   = [](float a_from, float a_to) {
            return std::clamp(a_to + (a_to - a_from), 0.0f, 1.0f);
        };
        const ImVec4 pressedFill =
            spread > 0.02f
                ? ImVec4{ step(base.x, hover.x), step(base.y, hover.y),
                          step(base.z, hover.z), step(base.w, hover.w) }
                : ImVec4{ std::clamp(hover.x + 0.13f, 0.0f, 1.0f),
                          std::clamp(hover.y + 0.13f, 0.0f, 1.0f),
                          std::clamp(hover.z + 0.13f, 0.0f, 1.0f), hover.w };

        ImVec4 fill = a_held                       ? pressedFill
                      : (a_hovered && !a_disabled) ? hover
                                                   : base;
        ImVec4 line = OS::ui::StyleColor(ImGuiCol_Border);
        if (a_disabled) {
            // The same dimming ImGui's own disabling applies, done by hand
            // because none of the drawing below goes through it.
            fill.w *= 0.5f;
            line.w *= 0.5f;
        }
        // ⚠ ArtCorner, NOT FrameCorner. This used to hand the frame's own 20 to
        // both calls below, and on a rect one widget high that meant a fill cut
        // at half its height sitting inside an outline cut at a quarter: the
        // fill pulled back out of its corners and the window showed through
        // beside the line. See ArtCorner for why the two clamp differently.
        const float corner = ArtCorner(a_min, a_max);
        if (const auto tex = IconImages::FrameFill()) {
            FillImage(tex, a_min, a_max, fill, corner);
        } else {
            Fill(a_min, a_max, fill, CutFor(a_min, a_max));
        }
        if (const auto tex = IconImages::Frame()) {
            FrameImage(tex, a_min, a_max, line, corner);
        } else {
            Stroke(a_min, a_max, line, CutFor(a_min, a_max),
                   std::max(1.0f, FUCK::GetResolutionScale()));
        }
    }

    void PaintTabBox(const ImVec2& a_min, const ImVec2& a_max, bool a_hovered,
                     bool a_selected) {
        // The measurements and why the border is derived are at the
        // declaration. ⚠ TabHovered beats Tab but never beats TabSelected: the
        // lit tab is the one piece of state this strip has to keep saying, and
        // a hover that overwrote it would blank the answer to "which outfit am
        // I editing" for as long as the pointer was resting.
        const ImVec4 fill = a_selected ? OS::ui::StyleColor(ImGuiCol_TabSelected)
                            : a_hovered ? OS::ui::StyleColor(ImGuiCol_TabHovered)
                                        : OS::ui::StyleColor(ImGuiCol_Tab);
        const ImVec4 border = OS::ui::StyleColor(ImGuiCol_Border);
        constexpr float kIdleBorder = 0.60f;
        const ImVec4    line =
            a_selected ? border
                          : ImVec4{ border.x * kIdleBorder, border.y * kIdleBorder,
                                    border.z * kIdleBorder, border.w };
        // ⚠⚠ SQUARE, AND THAT IS THE USER'S CALL AFTER SEEING BOTH (2026-08-12,
        // "in all honesty i prefer the square tabs from before"). The cut
        // corner was the whole stated reason for hand-rolling the strip, so it
        // is worth being clear that the hand-roll still paid: what it actually
        // bought was the theme's real tab colours, ownership of the selection,
        // the gap between tabs and a scroll the ABI cannot provide. The shape
        // was only ever the most visible of those, and it is now a choice
        // rather than something FLICK decided for us.
        //
        // ⚠ IT ALSO SEPARATES THE "+" FROM THE TABS FOR FREE. That control
        // keeps the cut corner every other button in this editor has, so square
        // tabs and one cut affordance beside them say "these are pages, that is
        // an action" without a second colour doing the work alone.
        //
        // ⚠⚠ A CLOSED BOX, AND THE OPEN-BOTTOM VARIANT WAS TRIED AND REVERTED
        // THE SAME DAY. Dropping the bottom edge is the classical tab shape and
        // it looked wrong here: with each tab painting its own three bands, the
        // SIDES stopped appearing on some tabs (user 2026-08-12, "some of the
        // lines don't appear on the sides of tabs"). A full outline on every
        // tab is the shape, and the presets source tabs are hand-rolled through
        // this same function now so both strips wear it.
        //
        // A zero cut takes Fill and Stroke down their rectangle paths, which is
        // the same pair of calls the pre-chamfer editor drew and needs no art.
        Fill(a_min, a_max, fill, 0.0f);
        Stroke(a_min, a_max, line, 0.0f, std::max(1.0f, FUCK::GetResolutionScale()));
        // ⚠⚠ THE OVERLINE IS WHAT ACTUALLY ANSWERS "WHICH TAB AM I ON". The
        // three fills are the same gold at three alphas, so hover and selected
        // differ by 0.17 of it and a pointer resting on a neighbour washes the
        // answer out (user 2026-08-27, "quite hard to tell which tab is
        // selected"). A solid bar is a different KIND of cue rather than more of
        // the same one, which is the whole point: no amount of tuning those
        // alphas separates two states painted the same way.
        //
        // ⛔⛔ NOT ImGuiCol_TabSelectedOverline, WHICH SHIPPED ONCE AND CAME OUT
        // BLUE. That is ImGui's own colour for exactly this bar and it looked
        // like the right answer, but OS::ui::StyleColor reads FLICK's style, not
        // the one EditorStyle::Apply writes: Apply feeds ImGui::GetStyle() in
        // OUR context, which only the retired overlay path ever used. So setting
        // that entry in the theme changed nothing the editor reads, FLICK's own
        // preset never restyled it, and the bar drew in stock ImGui blue (user
        // 2026-08-27). ⚠ ANY COLOUR THIS EDITOR PAINTS WITH HAS TO BE ONE FLICK
        // ACTUALLY THEMES.
        //
        // ⛔ NOR THE TAB'S OWN FILL AT FULL ALPHA, WHICH SHIPPED SECOND AND WAS
        // INVISIBLE (user 2026-08-27, "there is no bar color"). That looked
        // safer than a stated colour because the bar would be the hue already
        // under it. The premise was wrong: the three tab fills are FLICK's, not
        // the gold in EditorStyle, and FLICK's are three near identical darks.
        // Opening the alpha on a dark gives a dark bar on a dark tab.
        //
        // So the bar is kJournalGold, the yellow this editor already means for a
        // highlight, stated once in the header with the theme note that permits
        // it. ⚠ THAT IS TWO REFUTED ATTEMPTS AT READING THIS COLOUR FROM THE
        // THEME. There is nothing in FLICK's tab family to read.
        //
        // ⚠ THE TOP EDGE, inside the box rather than above it, so a tab sitting
        // hard against the pane below keeps its own footprint and two
        // neighbouring bars cannot merge into one line across the strip.
        if (a_selected) {
            const float t = std::max(2.0f, 3.0f * FUCK::GetResolutionScale());
            Fill(a_min, ImVec2(a_max.x, a_min.y + t), kJournalGold, 0.0f);
        }
    }

    // The shared body of Button and IconButton. Everything about a chamfered
    // button except how wide it is, because that is the only thing the two
    // disagree about: Button measures its label, IconButton takes a square.
    static ButtonResult ButtonSized(const char* a_label, float a_width, bool a_disabled,
                                    int a_padPane) {
        const ImVec2 textSize = FUCK::CalcTextSize(a_label);
        const float  w        = a_width;
        // ⚠ EXACTLY FrameWidgetHeight, NOT GetFrameHeight AND NOT max() WITH
        // THE TEXT PLUS PADDING. One widget height for the whole pane, and the
        // widget is FLICK's: GetFrameHeight is ImGui's fontSize plus
        // FramePadding.y twice, which came out 13px SHORT of the Name field on
        // the same row (user 2026-08-12, and the measured numbers are at
        // FrameWidgetHeight's declaration). The max() variant failed earlier
        // the other way: a label measuring a fraction taller than the line
        // height made one button a pixel taller than its row.
        const float  h        = FrameWidgetHeight();

        // Where the line says this button should start, read BEFORE anything is
        // submitted. Compared against the rect the InvisibleButton actually
        // gets, below, so "the button is offset upwards" is a subtraction
        // rather than a fourth guess.
        const float askedY = FUCK::GetCursorScreenPos().y;

        ButtonResult r;
        // The click is refused here as well as by any BeginDisabled around it.
        // Belt and braces on purpose: a caller that passes the flag but forgets
        // the wrapper still gets a button that cannot be pressed, which is the
        // safe direction to be wrong in.
        // ⚠ NO EnableNav. The editor owns its controller cursor now (PadFocus.h),
        // and ImGui's NoNav default is what keeps it the only one; a button that
        // belongs to a pane says so with a_padPane instead.
        //
        // ⚠ A DISABLED BUTTON IS NOT A STOP. The action bar is full of them -
        // Undo and Redo are disabled far more often than not - and a cursor that
        // parks on one has nowhere to go and nothing to do.
        const auto hit = a_padPane >= 0 ? OS::PadFocus::Item(a_padPane, !a_disabled)
                                        : OS::PadFocus::Hit{};
        const bool pressed = FUCK::InvisibleButton(a_label, ImVec2{ w, h });
        r.focused          = hit.focused;
        r.clicked          = (pressed || hit.activate) && !a_disabled;
        // ⚠⚠ AllowWhenDisabled, AND WITHOUT IT EVERY CONVERTED SITE INSIDE A
        // BeginDisabled LOSES ITS TOOLTIP. A disabled item does not register
        // hover at all under the default flags, and the sites being converted
        // are full of tooltips that exist precisely to explain why a control is
        // greyed - the ones that say what you would have to do to enable it.
        // They all read IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) today
        // and would have gone quiet in exactly the state they were written for.
        //
        // Safe to report it unconditionally, because nothing downstream confuses
        // hover with availability: the click is already refused by a_disabled
        // twice over, and the hover FILL below is gated on !a_disabled so a dead
        // button still cannot light up under the pointer.
        r.hovered = FUCK::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        const bool held    = FUCK::IsItemActive() && !a_disabled;
        const ImVec2 mn   = FUCK::GetItemRectMin();
        const ImVec2 mx   = FUCK::GetItemRectMax();
        r.min             = mn;
        r.max             = mx;

        // Once a session, so the reasoning below is a measurement in this tree
        // and not a note inherited from an earlier one. It is four numbers on
        // the first chamfered button of the session and nothing after that.
        static bool logged = false;
        if (!logged) {
            logged             = true;
            const auto  b      = OS::ui::StyleColor(ImGuiCol_Button);
            const auto  bh     = OS::ui::StyleColor(ImGuiCol_ButtonHovered);
            const auto  ba     = OS::ui::StyleColor(ImGuiCol_ButtonActive);
            const auto  tx     = OS::ui::StyleColor(ImGuiCol_Text);
            spdlog::info("[chamfer] style Button=({:.2f},{:.2f},{:.2f},{:.2f}) "
                         "Hovered=({:.2f},{:.2f},{:.2f},{:.2f}) "
                         "Active=({:.2f},{:.2f},{:.2f},{:.2f}) "
                         "Text=({:.2f},{:.2f},{:.2f},{:.2f})",
                         b.x, b.y, b.z, b.w, bh.x, bh.y, bh.z, bh.w, ba.x, ba.y, ba.z,
                         ba.w, tx.x, tx.y, tx.z, tx.w);
            // The sizes the two reports of 2026-08-12 both turn on, and the
            // ones nothing in this editor has ever printed. lineH is what
            // CalcTextSize returns for a glyph's height and what the centring
            // divides; glyphW is the icon's real advance. If glyphW is well
            // under lineH then FLICK bakes its icon font smaller than its body
            // font, and both the square and the nudge are keyed to the wrong
            // number.
            spdlog::info(
                "[chamfer] metrics askedY={:.1f} rectY={:.1f}..{:.1f} (offset {:.1f}) "
                "w={:.1f} h={:.1f} frameH={:.1f} lineH={:.1f} framePad=({:.1f},{:.1f}) "
                "glyphW={:.1f}",
                askedY, mn.y, mx.y, mn.y - askedY, w, h, FUCK::GetFrameHeight(),
                FUCK::GetTextLineHeight(), OS::ui::FramePadding().x,
                OS::ui::FramePadding().y, textSize.x);
        }

        // ⚠ NAV FOCUS READS AS HOVER for the paint, the slot row band's rule.
        // r.hovered itself stays the pointer's, since callers hang tooltips on
        // it and a tooltip sits at the mouse. Gamepad gated for the band's
        // reason: a mouse click leaves nav focus on the button it hit, and that
        // must not paint as a second hover once the pointer moves off.
        const bool navHot = r.focused &&
                            FUCK::GetInputDevice() == FUCK::InputDevice::kGamepad;
        PaintButtonBox(mn, mx, r.hovered || navHot, held, a_disabled);
        const ImVec4 text = a_disabled
                                ? OS::ui::StyleColor(ImGuiCol_TextDisabled)
                                : OS::ui::StyleColor(ImGuiCol_Text);
        OS::ui::TextAt(ImVec2{ mn.x + (w - textSize.x) * 0.5f,
                               mn.y + (h - textSize.y) * 0.5f },
                       OS::ui::Col(text), a_label);
        // ⚠ THE FOOTPRINT IS SEALED BACK, the same Dummy the swatch and the
        // dye tile close with. TextAt restores the cursor but not the previous
        // line's extent, and the next widget is placed off exactly that, so
        // without this a SameLine after a button creeps left and up.
        FUCK::SetCursorScreenPos(mn);
        FUCK::Dummy(ImVec2{ w, h });
        return r;
    }

    ButtonResult Button(const char* a_label, float a_minWidth, bool a_disabled,
                        int a_padPane) {
        const ImVec2 pad = OS::ui::FramePadding();
        return ButtonSized(
            a_label, std::max(a_minWidth, FUCK::CalcTextSize(a_label).x + pad.x * 2.0f),
            a_disabled, a_padPane);
    }

    // ⚠ THE SAME HEIGHT FOR THE WIDTH, WHICH IS THE WHOLE OF THIS FUNCTION:
    // an exact square of one FLICK widget height. The reasoning and the
    // measurements are at the declaration; the short version is that a glyph
    // in Button() gets FramePadding.x at its sides and something else above
    // and below, so its padding could never be even.
    ButtonResult IconButton(const char* a_glyph, bool a_disabled, int a_padPane) {
        return ButtonSized(a_glyph, FrameWidgetHeight(), a_disabled, a_padPane);
    }

    void FillImage(ImTextureID a_tex, const ImVec2& a_min, const ImVec2& a_max,
                   const ImVec4& a_tint, float a_corner, unsigned a_corners) {
        if (!a_tex || a_tint.w <= 0.0f) {
            return;
        }
        if (DrawPlainImpl()) {
            // The art IS the carve, so a plain shape simply does not sample it.
            FUCK::DrawRectFilled(a_min, a_max, a_tint, PlainRadiusFor(a_min, a_max));
            return;
        }
        const float x0 = std::ceil(a_min.x), y0 = std::ceil(a_min.y);
        const float x1 = std::floor(a_max.x), y1 = std::floor(a_max.y);
        const float c  = std::max(
            1.0f, std::min({ a_corner, (x1 - x0) * 0.5f, (y1 - y0) * 0.5f }));
        if (x1 - x0 <= 0.0f || y1 - y0 <= 0.0f) {
            return;
        }
        constexpr float kS = 20.0f / 64.0f;
        // A corner that is not in the mask is drawn as a plain filled rect, so
        // one call can cut the two corners that meet a frame and leave the two
        // that meet a straight edge alone. That is the rail's case exactly.
        const auto corner = [&](float a_dx, float a_dy, unsigned a_bit, float a_u,
                                float a_v) {
            const ImVec2 p0{ a_dx, a_dy }, p1{ a_dx + c, a_dy + c };
            if (a_corners & a_bit) {
                FUCK::AddImage(a_tex, p0, p1, ImVec2{ a_u, a_v },
                               ImVec2{ a_u + kS, a_v + kS }, a_tint);
            } else {
                FUCK::DrawRectFilled(p0, p1, a_tint, 0.0f);
            }
        };
        corner(x0, y0, Policy::kTopLeft, 0.0f, 0.0f);
        corner(x1 - c, y0, Policy::kTopRight, 1.0f - kS, 0.0f);
        corner(x0, y1 - c, Policy::kBottomLeft, 0.0f, 1.0f - kS);
        corner(x1 - c, y1 - c, Policy::kBottomRight, 1.0f - kS, 1.0f - kS);
        // The cross between the corners is solid whatever the corners do, so it
        // is three rects rather than five stretched image samples: same pixels,
        // no filtering, and it cannot pick up a seam from the texture's edge.
        FUCK::DrawRectFilled(ImVec2{ x0 + c, y0 }, ImVec2{ x1 - c, y0 + c }, a_tint, 0.0f);
        FUCK::DrawRectFilled(ImVec2{ x0, y0 + c }, ImVec2{ x1, y1 - c }, a_tint, 0.0f);
        FUCK::DrawRectFilled(ImVec2{ x0 + c, y1 - c }, ImVec2{ x1 - c, y1 }, a_tint, 0.0f);
    }

    void FrameImage(ImTextureID a_tex, const ImVec2& a_min, const ImVec2& a_max,
                    const ImVec4& a_tint, float a_corner) {
        if (!a_tex || a_tint.w <= 0.0f) {
            return;
        }
        if (DrawPlainImpl()) {
            // ⚠ ONE SCALED PIXEL, because the art's own line weight is baked
            // into the texture and there is no number here to carry across.
            // Matches what Stroke's callers ask for on the fallback path.
            FUCK::DrawRect(a_min, a_max, a_tint, PlainRadiusFor(a_min, a_max),
                           std::max(1.0f, FUCK::GetResolutionScale()));
            return;
        }
        const float x0 = std::ceil(a_min.x);
        const float y0 = std::ceil(a_min.y);
        const float x1 = std::floor(a_max.x);
        const float y1 = std::floor(a_max.y);
        // ⚠ THE CORNER IS CLAMPED TO A QUARTER OF THE PANEL, not to a half. At a
        // half the two corners meet and the stretched edge between them is zero
        // wide, which is the case that draws a shape nobody asked for rather
        // than a frame; a quarter still leaves the edge visibly an edge. A panel
        // small enough to hit this is the one the caller should be drawing with
        // Stroke anyway.
        const float c = std::max(
            1.0f, std::min({ a_corner, (x1 - x0) * 0.25f, (y1 - y0) * 0.25f }));
        if (x1 - x0 <= 2.0f * c || y1 - y0 <= 2.0f * c) {
            return;
        }
        // The texture's own slice, as a fraction. Both halves of this number
        // live in tools/make_frame_texture.py (SLICE over SIZE) and the two have
        // to agree: a mismatch samples part of the corner art into the stretched
        // edge, which reads as a frame that smears near its corners.
        constexpr float kSliceUV = 20.0f / 64.0f;
        const float     u0 = 0.0f, u1 = kSliceUV, u2 = 1.0f - kSliceUV, u3 = 1.0f;

        // ⚠ AddImage, NOT DrawImage: no item is submitted, so a caller's
        // following IsItemHovered still asks about whatever it drew before this.
        const auto piece = [&](float a_dx0, float a_dy0, float a_dx1, float a_dy1,
                               float a_uu0, float a_vv0, float a_uu1, float a_vv1) {
            FUCK::AddImage(a_tex, ImVec2{ a_dx0, a_dy0 }, ImVec2{ a_dx1, a_dy1 },
                           ImVec2{ a_uu0, a_vv0 }, ImVec2{ a_uu1, a_vv1 }, a_tint);
        };
        // Corners, at their own size.
        piece(x0, y0, x0 + c, y0 + c, u0, u0, u1, u1);
        piece(x1 - c, y0, x1, y0 + c, u2, u0, u3, u1);
        piece(x0, y1 - c, x0 + c, y1, u0, u2, u1, u3);
        piece(x1 - c, y1 - c, x1, y1, u2, u2, u3, u3);
        // Edges, stretched along their length only.
        piece(x0 + c, y0, x1 - c, y0 + c, u1, u0, u2, u1);
        piece(x0 + c, y1 - c, x1 - c, y1, u1, u2, u2, u3);
        piece(x0, y0 + c, x0 + c, y1 - c, u0, u1, u1, u2);
        piece(x1 - c, y0 + c, x1, y1 - c, u2, u1, u3, u2);
        // No centre piece: it is transparent in the texture, so drawing it would
        // be a full-panel blend that costs a fill rate and changes nothing.
    }

}  // namespace OS::ChamferPanel
