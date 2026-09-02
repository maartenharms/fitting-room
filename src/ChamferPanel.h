#pragma once

#include "ChamferPolicy.h"

#include <imgui.h>

// The drawing half of the chamfered panel: cut-corner fills and frames in the
// Vel'dun idiom, painted by us because FLICK's theme format cannot describe one.
// ChamferPolicy.h carries the why, both for the shape and for the way it is
// tiled.
//
// ⚠⚠ THE UNITS DIFFER FROM MENU STUDIO'S COPY OF THIS FILE, AND THAT IS THE ONE
// THING TO CHECK BEFORE COPYING A CALL SITE BETWEEN THE TWO MODS. Menu Studio's
// version takes UNSCALED pixels and applies FUCK::Scale itself, which suits a
// file whose sizes are hand-picked constants like `kTileSize = 46`. Nothing in
// this editor works that way: every dimension here is derived from
// OS::ui::FontSize(), which is already scaled by resolution, global scale and
// the user's UI-size slider. Scaling again inside would be the double-scale bug
// that pane has paid for repeatedly, and making each call site divide the scale
// back out would be worse. So THESE take FINAL DEVICE PIXELS. Hand one of them
// an unscaled constant and the shape will be right at 100% and shrink from
// there.
//
// ⚠ NO Draw / DrawWindow / DrawItem HERE, unlike Menu Studio's copy. Those
// decorate a whole window, and this editor's window cannot use them: it is a
// FUCK IWindow that deliberately does NOT set kNoBackground, because dropping
// that left everything transparent except the two child panels (see
// EditorWindow.cpp's GetFlags). FLICK owns this panel's background, so what we
// can chamfer is what we paint ourselves. Adding the window helpers before that
// changes would be shipping three functions nothing may call.
namespace OS::ChamferPanel {

    namespace Policy = OS::ChamferPolicy;

    using Policy::kAll;
    using Policy::kBottom;
    using Policy::kBottomLeft;
    using Policy::kBottomRight;
    using Policy::kLeft;
    using Policy::kNone;
    using Policy::kRight;
    using Policy::kTop;
    using Policy::kTopLeft;
    using Policy::kTopRight;

    // ⚠ ONE CUT FOR THE WHOLE EDITOR, AND IT IS AN ABSOLUTE SIZE RATHER THAN A
    // FRACTION OF EACH WIDGET. A fraction is right when one widget has to hold
    // its proportions across UI scales, which is what kRailTileRounding was
    // doing for a corner radius. It is wrong across DIFFERENT widgets: a rail
    // tile and a preview card differ by more than double in size, and cutting
    // each by 28% of itself would put a small notch on one and a huge bevel on
    // the other, which reads as two unrelated styles rather than one. Vel'dun's
    // own notches are a roughly constant size whatever they are cut into.
    //
    // Derived from the body font so it still follows resolution and the UI-size
    // slider, which is this editor's convention for every other dimension.
    [[nodiscard]] float StandardCut();

    // The cut a rect this size can actually wear: the standard one, clamped so a
    // small widget is not eaten by it.
    //
    // ⚠ THE CLAMP IS THE HALF OF THE RULE THE FIRST VERSION LEFT OUT. One
    // absolute cut is right for keeping a card and a tile in the same visual
    // family, and it stops being right the moment a widget is barely larger than
    // the cut: a palette swatch is 1.4 font heights, so the standard cut is over
    // 40% of it and the chip reads as a diamond rather than as a chip with its
    // corners taken off. A quarter of the shorter side is where a cut still
    // reads as a corner treatment instead of as the shape itself. ChamferPolicy
    // clamps at HALF, which is the point the geometry folds; this is the point
    // the look does, and it is the tighter of the two.
    [[nodiscard]] float CutFor(const ImVec2& a_min, const ImVec2& a_max);

    // Is our chrome currently drawing the PLAIN shape rather than the carved
    // one? The four draw calls below ask this themselves, so no caller needs it
    // to pick a shape.
    //
    // ⚠⚠ IT IS EXPOSED FOR ONE CALLER AND ONE QUESTION: whether to paint a
    // plate at all. The editor window keeps FLICK's own background, so on the
    // plain path FLICK has ALREADY drawn the correct plate and ours goes on top
    // of it. Two translucent blacks over each other read darker than either, and
    // where our rounded corner cuts in, FLICK's own corner shows past it as a
    // step (field 2026-08-14, "transparent black bits that overlay the edges").
    // The answer is not a better plate, it is not painting a second one.
    [[nodiscard]] bool PlainShape();

    // The radius the PLAIN path draws with, for a surface that carried its own
    // number before the carve existed. In final device pixels, RAII, and it
    // nests: the previous value is restored on the way out.
    //
    // ⚠⚠ THIS IS A RADIUS AND NOT A SHAPE DECISION, and the difference is the
    // whole reason it is allowed to live at a call site. Whether the editor
    // draws carved or plain is answered inside the four draw calls below and
    // must stay there - putting it at a call site is wrong turn 3, which shipped
    // a plain window full of carved buttons. What the four calls cannot know is
    // how ROUND a plain version of this particular surface should be, and
    // vanilla answered that per surface: the preview card and the slot tile took
    // the theme's FrameRounding, the rail tiles took 28% of their own edge with
    // a note saying the theme's number was wrong for them, and the editor frame
    // was hard square (all measured off fde7e3f).
    //
    // ⚠ WITH NO SCOPE IN EFFECT A PLAIN SURFACE FOLLOWS THE THEME, so only a
    // surface that disagrees with the theme needs one. Carved draws ignore this
    // entirely; the cut is an absolute size and has nothing to do with a radius.
    struct PlainRadiusScope {
        float prev;
        explicit PlainRadiusScope(float a_radius);
        ~PlainRadiusScope();
        PlainRadiusScope(const PlainRadiusScope&)            = delete;
        PlainRadiusScope& operator=(const PlainRadiusScope&) = delete;
    };

    // Solid ground, no edge. a_cut and the rect are in final device pixels.
    void Fill(const ImVec2& a_min, const ImVec2& a_max, const ImVec4& a_col, float a_cut,
              unsigned a_corners = kAll);

    // The frame, drawn as a_thickness stacked single-pixel rings. a_thickness is
    // in final device pixels too, so pass std::max(1.0f, GetResolutionScale())
    // where the surrounding code already computes one.
    void Stroke(const ImVec2& a_min, const ImVec2& a_max, const ImVec4& a_col, float a_cut,
                float a_thickness, unsigned a_corners = kAll);

    // A picture with its corners cut, drawn as the same three quads the fill
    // uses with the texture coordinates carried along.
    //
    // ⚠ THIS REPLACES A KNOCKOUT AND THE FIELD IS WHY. The first version painted
    // the corner triangles back out in the colour behind the card, because
    // chamfering a card's fill alone is invisible: the thumbnail is an opaque
    // square laid over it and its square corners cover the cut. That version
    // shipped and the triangles were plainly visible (user 2026-08-12, "you can
    // see triangles on the edges where it's cut"), and the theme says why. Its
    // ChildBg is #1D1A1700, fully transparent, so the knockout fell back to the
    // window colour at #1D1A17F4, which is not opaque either: the painted
    // triangle came out as 96% window colour over the CARD's fill while the
    // background beside it was the same 96% over the game scene. Two different
    // composites of the same nominal colour, which is exactly a visible edge.
    //
    // ⚠ SO NOTHING IS PAINTED BACK. FUCK::DrawImageQuad takes four arbitrary
    // corners with four UVs and sits in the BASE interface, so the picture is
    // simply drawn as the cut shape. No backdrop is assumed, no colour is
    // guessed, and it is correct over a gradient, another picture or the moving
    // scene, none of which a knockout can be.
    void Image(ImTextureID a_tex, const ImVec2& a_min, const ImVec2& a_max,
               const ImVec4& a_tint, float a_cut, unsigned a_corners = kAll);

    // A nine-slice frame: the four corners drawn at a fixed size and the four
    // edges stretched between them, centre skipped because it is transparent.
    //
    // ⚠ THIS IS THE ROUTE ChamferPolicy.h POINTS AT, and it is not a fancier
    // chamfer. Quads can only ever produce an octagon; the look being chased is
    // ART. Vel'dun's frames are Flash vector drawings, so a drawing is what
    // matches them, and once one is being sampled the corner can carry a rivet,
    // a bevel or an inlay that no amount of geometry gets to.
    //
    // ⚠ THE CORNER IS NEVER STRETCHED, WHICH IS THE WHOLE POINT OF NINE-SLICING.
    // a_corner is the size the corner is drawn at in final device pixels, so it
    // should be the texture's own slice times the resolution scale. Hand it a
    // fraction of the panel instead and a wide panel gets fat corner art and a
    // narrow one gets thin, which is the failure a plain stretched image has.
    //
    // ⚠ THE TINT IS THE COLOUR. The texture is white with its shape in the
    // alpha, so pass the theme's border colour; passing white gets a white
    // frame, which is nobody's theme.
    void FrameImage(ImTextureID a_tex, const ImVec2& a_min, const ImVec2& a_max,
                    const ImVec4& a_tint, float a_corner);

    // The size FrameImage's corner is drawn at, in final device pixels.
    //
    // ⚠ CALLERS NEED THIS, NOT JUST THE FRAME. A corner drawn from art is a
    // region of the panel that is no longer straight edge, so anything drawn
    // against the panel's edge has to account for it. One function, so the
    // number is not spelled out at each site and cannot drift from the
    // texture's own slice.
    //
    // ⚠⚠ FOR THINGS THIS DRAWS, NOT FOR LAYING CONTROLS OUT, and the difference
    // cost a stint. FillImage and FrameImage take it because they are cutting
    // the shape. A widget standing OFF the corner by it is a different move and
    // it was rejected three times over: the "Editing:" roster was inset by this,
    // then by this plus a spacing, then put back where it started, and the
    // answer in the end was to move the control out of the corner altogether
    // (user 2026-08-12). A shaped corner is a bad place to put a control, and no
    // inset makes it a good one.
    // A FILLED panel whose corners are cut to the same curve the frame is,
    // through a texture whose alpha is the fill rather than a line.
    //
    // ⚠ THIS IS WHAT KEEPS AN ART CORNER HONEST. Quads draw an octagon and
    // never an arc, so a filled thing meeting the frame's corner used to have
    // only two options: keep clear of it, which leaves a gap, or bleed out of
    // it, which was the report before that. This is the third.
    //
    // ⚠ THE MASK IS PER CORNER because most panels only meet the frame at some
    // of theirs. The rail is cut on its left pair and square on the right,
    // where it meets the body rather than the frame. A corner outside the mask
    // costs a plain rect and no sampling at all.
    void FillImage(ImTextureID a_tex, const ImVec2& a_min, const ImVec2& a_max,
                   const ImVec4& a_tint, float a_corner, unsigned a_corners = kAll);

    // A button with the editor's corner and the theme's padding.
    //
    // ⚠ IT IS HAND-ROLLED BECAUSE FUCK::Button CANNOT BE EITHER. It is
    // ImGui::OutlineButton, which sizes itself as label plus 2 * (8 * scale)
    // and never reads style.FramePadding at all, measured out of FLICK's PDB
    // and recorded in FuckCompat.h. So its shape is square and its padding is
    // not ours to set, which is both halves of the field report (user
    // 2026-08-12, "we don't have proper padding on buttons, and are we able to
    // get buttons to have the corner cut too").
    //
    // The shape is the same InvisibleButton-then-paint idiom DrawRailTile and
    // the "+" tab already use, so this is a third instance of a proven pattern
    // rather than a new one. Colours come from the theme's Button family, so a
    // preset still drives them.
    //
    // ⚠ IT RETURNS ITS HOVER AND ITS RECT RATHER THAN LEAVING THEM TO THE
    // CALLER, which is DrawRailTile's rule and it is not optional. The label
    // goes through OS::ui::TextAt, and that submits a real TextUnformatted, so
    // an IsItemHovered() or GetItemRectMin() after this call would be asking
    // about the TEXT and not about the button underneath it. Every caller here
    // needs at least one of the two, for a tooltip or a tutorial anchor.
    struct ButtonResult {
        bool   clicked{ false };
        bool   hovered{ false };
        // Controller: this button holds nav focus. Captured inside Button for
        // the reason hover is - the label goes through TextAt, which submits an
        // item, so an IsItemFocused() after the call asks about the text.
        bool   focused{ false };
        ImVec2 min{};
        ImVec2 max{};
    };
    // ⚠⚠ a_disabled IS NOT OPTIONAL POLISH, AND IT IS WHY THIS CANNOT BE A
    // FIND-AND-REPLACE ACROSS THE 88 FUCK::Button CALL SITES. Many of them sit
    // inside FUCK::BeginDisabled. ImGui's disabling reaches the InvisibleButton
    // underneath, so the click is correctly refused, but it does NOT reach the
    // draw calls above: they are ours and nothing multiplies their alpha. A
    // converted button inside BeginDisabled would therefore go dead while still
    // LOOKING enabled, which is worse than a square corner and silent. Every
    // site that moves across has to pass the same condition its BeginDisabled
    // already tests.
    // ⚠ a_padPane IS OPT-IN, AND HAS TO BE. This helper draws 91 buttons across
    // six panels, most of them in surfaces the controller rebuild does not reach
    // (Settings, Rules, Shape, Body Studio, the tutorial). Registering all of
    // them with one pane would put the editor's action bar and a settings dialog
    // on the same cursor. Pass an OS::PadFocus pane at the sites that belong to
    // one; -1 leaves the button mouse-only, exactly as it is today.
    [[nodiscard]] ButtonResult Button(const char* a_label, float a_minWidth = 0.0f,
                                      bool a_disabled = false, int a_padPane = -1);

    // The same button, sized as an exact SQUARE of one widget height, for a
    // control whose whole label is one glyph.
    //
    // ⚠⚠ Button() ABOVE CANNOT BE SQUARE AND THAT IS NOT A DEFAULT WORTH
    // TWEAKING. Its width is text plus FramePadding.x twice while its height is
    // GetFrameHeight(), which is text plus FramePadding.y twice, so a glyph
    // button comes out with the theme's HORIZONTAL padding at its sides and its
    // VERTICAL padding above and below. Vel'dun's FramePadding is (8, 4) on
    // this rig, logged by the tab-metrics line in EditorUI, so that is 8 either
    // side of the glyph and 4 top and bottom: exactly twice as much across as
    // down (user 2026-08-12, "the rescan button is there but it's not got even
    // padding all around"). Passing GetFrameHeight() as a_minWidth does not fix
    // it, because it is a MINIMUM and the text term is the larger of the two.
    //
    // A square of GetFrameHeight() is even by construction for the glyphs this
    // editor draws, and it keeps the height every other framed widget on the
    // row has. MEASURED out of the bundled icons.ttf with fontTools on
    // 2026-08-12: every audited FA5 glyph advances 1.125em or less, and a
    // 1.0em glyph in a box of fontSize + 2 * FramePadding.y leaves exactly
    // FramePadding.y on all four sides.
    //
    // ⚠ THE WIDEST AUDITED GLYPH IS kFillDrip AT 1.125em AND IT STILL FITS. It
    // needs the box to be at least 1.125 * fontSize, and the box is fontSize +
    // 2 * FramePadding.y, so it only overflows once fontSize exceeds sixteen
    // times FramePadding.y - 64px against this rig's 4. Not a case to code for,
    // but the arithmetic is here so nobody has to redo it.
    [[nodiscard]] ButtonResult IconButton(const char* a_glyph, bool a_disabled = false,
                                          int a_padPane = -1);

    // The height of a FLICK framed widget - an InputText, a Combo - which is
    // NOT ImGui's GetFrameHeight and is the height every button here uses.
    //
    // ⚠⚠ GetFrameHeight IS THE WRONG NUMBER AND IT COST A FIELD ROUND. It is
    // fontSize + 2 * FramePadding.y, which on this rig is 28 + 8 = 36. FLICK's
    // own widgets pad by 8 * scale a side, the same arithmetic its OutlineButton
    // was measured to use, which is 28 + 21.3 = 49.3. A chamfered button sized
    // by GetFrameHeight therefore ended 13px above the bottom of the Name field
    // on its own row - tops level, because a row lays out from the top, bottoms
    // not (user 2026-08-12, "the top is fine, but the bottom of the button
    // isn't aligned with the row's bottom"). The probe run the same day pinned
    // it: the button sat at offset 0.0 from its own line with even gaps above
    // and below, so the primitive was placed right and simply too short.
    //
    // MEASURED, NOT DERIVED, as soon as the session allows: every InputText
    // that shares a row with one of these buttons reports its real height
    // through NoteFrameWidgetHeight below, and that number wins. Until the
    // first one draws, the fallback is the OutlineButton formula out of FLICK's
    // PDB - fontSize + 2 * (8 * scale). The one uncertainty in the fallback is
    // WHICH scale the 8 multiplies; resolution and global are both 1.333 on
    // this rig so they cannot be told apart here, and the measured value
    // retires the question one frame after any pane with a field draws.
    [[nodiscard]] float FrameWidgetHeight();

    // Report the LAST SUBMITTED ITEM's height as the live framed-widget
    // height. Call immediately after a FUCK::InputText, before anything else
    // is submitted. Wrong item, wrong height, silently - which is why every
    // call site sits on the very next line.
    void NoteFrameWidgetHeight();

    // The fill and outline of a button, without the button: the theme's
    // Button family with the extrapolated press, the frame art, ArtCorner.
    // This is exactly what Button() paints, extracted so a control that
    // cannot BE a Button - the outfit strip's "+", which has to match the TAB
    // height rather than the widget height - still looks like one. Painting
    // it any other way is how the "+" spent a month as a hardcoded grey slab
    // beside themed buttons (user 2026-08-12, "the + button needs some love").
    void PaintButtonBox(const ImVec2& a_min, const ImVec2& a_max, bool a_hovered,
                        bool a_held, bool a_disabled = false);

    // The same, in the theme's TAB colours rather than its widget colours, for
    // the hand-rolled outfit strip.
    //
    // ⚠⚠ A THEME AUTHORS TABS SEPARATELY FROM WIDGETS AND THE TWO ARE NOT
    // CLOSE. Read out of Vel'dun.ini on 2026-08-12, which is the preset this
    // rig runs:
    //
    //     [Widget] rBackgroundColor       #22201CEB   rBorderColor       #D1C7AEFF
    //     [Widget] rTabColor              #1D1A17C8   rTabBorderColor    #7D7767FF
    //     [Widget] rTabActiveColor        #2E2A24F0   rTabBorderActive   #D1C7AEFF
    //
    // Painting a tab from the widget family gives it the ACTIVE tab's border
    // brightness while it sits beside idle ones, which is exactly what the "+"
    // looked wrong for (user 2026-08-12, "it's not really well integrated with
    // the other tabs").
    //
    // ⚠ THE FILL IS READ AND THE BORDER IS DERIVED, because only half of it is
    // reachable. ImGuiCol_Tab / TabHovered / TabSelected are real ImGui colours
    // and FLICK maps the three rTab* fills onto them. A tab's BORDER is not an
    // ImGui colour at all - FLICK draws it itself - so no style read can return
    // it. The derivation is measured rather than guessed: rTabBorderActiveColor
    // is byte-identical to rBorderColor, and rTabBorderColor is that same
    // colour at 0.60 on every channel (125/209, 119/199, 103/174). So selected
    // takes ImGuiCol_Border unchanged and idle takes it at 0.60, which is one
    // ratio rather than two constants and follows any preset that keeps the
    // relationship this one has.
    void PaintTabBox(const ImVec2& a_min, const ImVec2& a_max, bool a_hovered,
                     bool a_selected);

    // Journal-gold highlight: search hits, favorited stars, the NEW badge, the
    // Apply gold cost, and the selected tab's bar. A deliberate highlight
    // colour (Fuzzles' theme note: override the theme sparingly, for highlights
    // on text or icons only, and these qualify). One named constant so the
    // palette is explicit.
    //
    // ⚠⚠ IT LIVES HERE RATHER THAN IN EditorUI BECAUSE PaintTabBox NEEDS IT AND
    // A SECOND COPY WOULD DRIFT. EditorUI's kGold is an alias of this now, so
    // its thirty-odd call sites read unchanged and there is still one
    // definition.
    //
    // ⚠ A STATED COLOUR, WHICH IS NOT THE HABIT IN THIS FILE, and the reason is
    // that the theme cannot supply one. Everything else here reads FLICK's
    // style through OS::ui::StyleColor, but FLICK's tab colours are three near
    // identical darks: painting the bar from any of them makes it invisible,
    // which is what shipped once (user 2026-08-27, "there is no bar color").
    // The editor already means this yellow, so the bar borrows it rather than
    // inventing a third answer.
    inline constexpr ImVec4 kJournalGold{ 1.0f, 0.82f, 0.2f, 1.0f };

    [[nodiscard]] float FrameCorner();

    // The corner to hand BOTH FillImage and FrameImage for one rect, so the
    // fill and the outline drawn on it come out the same shape.
    //
    // ⚠⚠ HANDING THEM THE SAME NUMBER IS NOT ENOUGH, AND THAT IS THE WHOLE
    // REASON THIS EXISTS. They clamp differently on purpose: FillImage allows a
    // corner up to HALF the shorter side, FrameImage only a QUARTER, because a
    // frame whose corners meet has no edge left to be an edge. Pass
    // FrameCorner() to both on a widget-height rect and the fill is cut at half
    // its height while the outline is cut at a quarter, so the fill retreats
    // out of its own corners and the panel behind shows through beside the
    // line. On a big panel the two agree by luck, which is why the editor's own
    // frame never showed it and every small control does.
    //
    // Never above a quarter, so both clamps are satisfied by the same value and
    // the two calls cannot disagree whatever the rect is.
    [[nodiscard]] float ArtCorner(const ImVec2& a_min, const ImVec2& a_max);

    // How far a filled thing must keep from a corner of FrameImage's rect so it
    // does not show through the piece the frame cuts away.
    //
    // ⚠ THIS IS THE PRICE OF AN ART CORNER AND IT IS WORTH STATING ONCE. The
    // frame's corner is a CURVE and nothing we draw with quads is, so a filled
    // panel meeting that corner cannot be cut to match it: the two shapes would
    // disagree along the arc. Keeping clear is the only exact answer short of a
    // second texture whose alpha is the fill. The rail's own background wash was
    // the first thing to run into it (user 2026-08-12, "a tiny bit of the
    // sidebar bleeds out of the chamfer").
    //
    // The scoop is a disc centred ON the corner, so a rect whose corner sits at
    // least its radius away is entirely outside it. Radius over slice is the
    // texture's own ratio, and both halves live in make_frame_texture.py.
    [[nodiscard]] float FrameClearance();

}  // namespace OS::ChamferPanel
