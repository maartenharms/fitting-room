#pragma once

// One preview card and the grid that lays them out, shared between the panes
// that draw them.
//
// ⚠ THIS EXISTS BECAUSE A SECOND PANE ASKED FOR CARDS. The card was written
// for the styles grid and lived in EditorUI.cpp's anonymous namespace, which
// made it unreachable the moment the Bodies work needed the same picture. What
// moved here is the plain data and the layout loop; DrawPreviewCard itself
// STAYS in EditorUI.cpp and is only declared here, because it reads that
// file's own gold constant and icon helpers and moving it would have dragged
// half the presentation layer with it. A declaration costs nothing and the
// definition is one function in one place, which is the same arrangement the
// rest of this UI uses.

#include "FuckCompat.h"   // FUCK, and OS::ui's FontSize / ItemSpacing
#include "PadFocus.h"     // the controller's own focus, which the clipper must obey
#include "PreviewGrid.h"  // SceneIdentity, ColumnsFor (both pure)

#include <imgui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace OS::PreviewCardUI {

    // What a card is told. The dimension keeps everything it decides for
    // itself (tooltips, commit, hover accumulators, tint) and reads the
    // returned state instead.
    struct PreviewCardDesc {
        std::uint32_t                     pushId{ 0 };  // FormID scopes the id stack
        float                             side{ 0.0f };
        const std::string*                name{ nullptr };
        ImU32                             nameCol{ 0 };
        const PreviewGrid::SceneIdentity* scene{ nullptr };  // null draws the cross
        std::uint32_t                     orderHint{ 0 };
        bool                              selected{ false };
        // The favourite star (user 2026-08-09). Opt-in per caller and
        // KEY-AGNOSTIC, exactly like RowFavoriteStar: hair keys off a form,
        // body presets off a name, and this has no business knowing which.
        // The caller reads starHovered and does its own toggle.
        bool showStar{ false };
        bool favourite{ false };
        // The new-content mark (user 2026-08-11): a gold triangle in the card's
        // TOP RIGHT corner, opposite the star, for a style whose plugin was not
        // there last launch.
        //
        // ⚠ IT IS THE ROW BADGE'S FLAG AND NOTHING NEW. The table has said this
        // since OS-26 with a gold "NEW" beside the name, off StyleItem::isRecent
        // and RecentMods' known_plugins.txt baseline; the card view simply never
        // learned to say it, so switching to Cards lost the information. A
        // second notion of "new" living beside that one would be two meanings
        // wearing one colour.
        //
        // ⚠ NOT HIT-TESTED, unlike the star. It is a statement, not a control,
        // so it needs no rect of its own and cannot steal the card's click.
        bool isNew{ false };

        // The character-default pin (user 2026-08-17): a thumbtack in the
        // BOTTOM LEFT of the picture, on the card this character's stored
        // default names.
        //
        // ⚠ A SEPARATE THING FROM `selected`, AND KEEPING THEM SEPARATE IS THE
        // POINT. `selected` means "this is what is on the character right now",
        // which is what the gear grid's glow has always meant; the pin means
        // "this is the one they bought". A default that nothing overrides is
        // both, and lights both. A default the outfit outranks, or one this
        // character's race or sex is no longer offered, is only pinned, so the
        // grid never shows two glowing cards and the purchase is still visible.
        //
        // ⚠ THE SAME THUMBTACK THE "Set as default" BUTTON WEARS, deliberately.
        // That control is where a default is made, so the mark that says where
        // one landed has to be the same glyph or they read as two features.
        //
        // ⚠ NOT HIT-TESTED, for isNew's reason: it is a statement, not a
        // control. The corner is free - the star owns top left and the new-mark
        // owns top right - so it needs no rect and steals no click.
        bool isDefault{ false };

        // The restart mark, in the last corner: this card's content is only
        // partly in the virtual tree, so wearing it now gives half a result.
        //
        // ⚠⚠ THE SAME GLYPH THE EMPTY RIVAL CARD ALREADY WEARS, deliberately.
        // A player learns "this arrow means relaunch" from a card with no
        // picture, and the half visible case HAS a picture, so it needs the
        // meaning without the empty state. A second glyph for one meaning would
        // read as a second feature.
        //
        // ⚠ NOT HIT-TESTED, for isNew's and isDefault's reason: a statement,
        // not a control. Bottom right is the one corner still free.
        bool needsRestart{ false };

        // ---- a picture that is not a rendered scene ------------------------
        //
        // ⚠ AN ALTERNATIVE PICTURE SOURCE, NOT A SECOND CARD. The overlay
        // picker shows flat art off disk rather than a scene the preview cache
        // renders, and it was written as its own hand-rolled tile first. That
        // tile was the field complaint: it did not look or feel like a card
        // anywhere else in the mod, because it was not one. Everything that
        // makes a card a card lives in DrawPreviewCard, so the fix is to let
        // the picture come from somewhere else rather than to draw a second
        // kind of card beside it.
        //
        // Set flat and leave scene null. The three states are the same three a
        // scene has, and they draw exactly the same way: a picture, the cross
        // when it is known to have failed, the turning arc while it is coming.
        bool        flat{ false };
        ImTextureID flatImage{ 0 };
        bool        flatFailed{ false };

        // ⚠⚠ WHAT AN EMPTY TILE SAYS, AND A CROSS ONLY SAYS ONE THING.
        // The cross means "there is nothing to show and that is wrong",
        // which is right for a pack whose folder went missing and wrong
        // for a card that is merely waiting on something. A caller may
        // hand a UTF-8 glyph to centre in its place; the skin page draws
        // the redo arrow on a skin that needs a restart, because a grid of
        // crosses reads as a grid of errors (user 2026-08-26).
        //
        // ⚠ A GLYPH THAT IS NOT IN THE BUNDLED ATLAS RENDERS AS TOFU and
        // cannot be checked from outside the game, so pass one Icons.h has
        // already audited rather than a fresh codepoint.
        const char* emptyGlyph{ nullptr };
    };

    struct PreviewCardResult {
        bool clicked{ false };
        bool hovered{ false };
        bool nameTrimmed{ false };
        bool starHovered{ false };
    };

    // ⚠ The scene pointer is read synchronously (Request copies), so a caller
    // may point it at a stack-local identity.
    [[nodiscard]] PreviewCardResult DrawPreviewCard(const PreviewCardDesc& a_desc);

    // The grid loop: cards in the scrolling child, the dye grid's column
    // maths, NO PAGES (user 2026-08-09), and the hand-rolled clipper. FUCK
    // exports no IsRectVisible and no scroll query, but the child's screen
    // rect and the cursor's screen position are both scroll-adjusted, which is
    // the same answer. A skipped row leaves a Dummy of the row's exact height
    // so the scrollbar's extent never lies about the list.
    //
    // ⚠ THE CLIPPER IS ALSO THE LOAD CHUNKER. Request-on-draw is the grid's
    // visibility calculation (PreviewCache.h), so a card that draws is a card
    // that queues, and a card the clipper skips stays unqueued until scrolled
    // toward. One row of margin each side means a slow scroll meets cards that
    // started building a moment before they entered the view; the queue prune
    // throws away whatever a fast scroll left behind.
    // ⚠ a_drawOne TAKES THE FITTED SIDE, not the requested one, and it must
    // use it for the card it draws. The size setting is a target: the grid
    // grows each card to fill the row exactly, so a pane that fits two and a
    // half cards draws two full ones instead of two and a hole. Computing it
    // here and handing it out is what keeps the row width, the clipper's row
    // height and the card itself agreeing; a caller that recomputed its own
    // would drift the moment either changed.
    // a_pad registers the grid with the controller's focus model. Off by default
    // so a pane outside the rebuilt path (Body Studio) keeps its mouse-only
    // behaviour without opting into a focus it has no highlight for.
    template <class DrawOne>
    void DrawCardGrid(std::size_t a_count, float a_side, DrawOne&& a_drawOne,
                      bool a_pad = false) {
        // ⚠⚠ NOT GetContentRegionAvail() ON ITS OWN, and PreviewGrid::StableAvail
        // carries the whole argument. That number shrinks by one scrollbar the
        // moment a scrollbar appears, the cards shrink with it, the rows get
        // shorter, the content fits again and the scrollbar leaves: a strobe at
        // one flip per frame. Laying out against the with-a-scrollbar width in
        // both states breaks the loop.
        const float rawAvail = FUCK::GetContentRegionAvail().x;
        const float avail    = PreviewGrid::StableAvail(
            rawAvail,
            (FUCK::GetWindowPos().x + FUCK::GetWindowSize().x) - FUCK::GetCursorScreenPos().x,
            OS::ui::WindowPadding().x,
            FUCK::GetStyleVar(ImGuiStyleVar_ScrollbarSize));
        const float spacing = OS::ui::ItemSpacing().x;
        const int   cols    = PreviewGrid::ColumnsFor(avail, a_side, spacing);
        const float side =
            PreviewGrid::FittedSide(avail, a_side, spacing, OS::ui::FontSize() * 2.0f);
        const float nameH = OS::ui::FontSize() * 1.2f;
        const float rowH  = side + nameH;

        // ⚠⚠ THE FOCUSED ROW IS DRAWN WHETHER THE CLIPPER WANTS IT OR NOT, AND
        // THAT IS WHY THIS GRID NO LONGER SKIPS CARDS. A clipped row is not
        // submitted, and for three rounds focus lived in ImGui, which can only
        // move to an item submitted that frame - so a press at the edge of the
        // view did not step to the next row, it landed on whatever submitted
        // card scored best (user 2026-08-15, "d pad can skip cards sometimes,
        // too many in a direction"). The previous mitigation was three rows of
        // margin instead of one, which widened the window the fault needed
        // without closing it.
        //
        // PadFocus addresses the LIST by index, so focus can already be on a
        // card the clipper skipped; all this has to do is draw that one row and
        // scroll to it. The margin goes back to one row, which is what the load
        // chunker wants: a card that draws is a card that queues a preview
        // render, and the pad now costs exactly one extra row rather than four.
        const int  focusIdx = a_pad ? OS::PadFocus::FocusedIndex(OS::PadFocus::kRight) : -1;
        const bool padHere  = a_pad && OS::PadFocus::Active() &&
                             OS::PadFocus::Pane() == OS::PadFocus::kRight;
        const int focusRow = (padHere && focusIdx >= 0 && focusIdx < static_cast<int>(a_count))
                                 ? focusIdx / cols
                                 : -1;
        if (a_pad) {
            OS::PadFocus::Declare(OS::PadFocus::kRight, static_cast<int>(a_count), cols);
        }

        const ImVec2 winPos  = FUCK::GetWindowPos();
        const ImVec2 winSize = FUCK::GetWindowSize();
        // One row of margin each side, so a slow scroll meets cards that started
        // building a moment before they entered the view.
        const float  margin    = rowH + OS::ui::ItemSpacing().y;
        const float  visTop    = winPos.y - margin;
        const float  visBottom = winPos.y + winSize.y + margin;

        int row = 0;
        for (std::size_t first = 0; first < a_count;
             first += static_cast<std::size_t>(cols), ++row) {
            const float y      = FUCK::GetCursorScreenPos().y;
            const bool  forced = row == focusRow;
            if (!forced && (y + rowH < visTop || y > visBottom)) {
                FUCK::Dummy(ImVec2(side, rowH));
                continue;
            }
            const auto count =
                (std::min)(static_cast<std::size_t>(cols), a_count - first);
            for (std::size_t i = 0; i < count; ++i) {
                if (i != 0) {
                    FUCK::SameLine();
                }
                a_drawOne(first + i, side);
            }
            // ⚠ AFTER THE ROW, NOT BEFORE IT. SetScrollHereY anchors on the
            // previous line's cursor, which is this row only once it has been
            // submitted, and it is the only scroll setter FUCK exposes.
            if (forced && OS::PadFocus::JustMoved()) {
                FUCK::SetScrollHereY(0.5f);
            }
        }
    }

}  // namespace OS::PreviewCardUI
