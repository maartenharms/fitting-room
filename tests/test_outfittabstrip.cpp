#include "OutfitTabStrip.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace S = OS::OutfitTabStrip;

static bool Near(float a_lhs, float a_rhs, float a_eps = 1e-4f) {
    return std::fabs(a_lhs - a_rhs) <= a_eps;
}

int main() {
    // ---- TabWidth is FLICK's own formula ---------------------------------
    // The numbers are the live ones logged on 2026-08-12: FramePadding.x 8,
    // textW("Abyss") 49, and FLICK's tab came out 66 wide (its extra +1 is the
    // tab bar's own separator and is not ours to reproduce). Pinning the
    // padding term is what stops a later "tidy" from silently changing how
    // many outfits fit on a screen.
    CHECK(Near(S::TabWidth(49.0f, 8.0f), 65.0f));
    CHECK(Near(S::TabWidth(0.0f, 8.0f), 16.0f));

    // ---- TabGap ----------------------------------------------------------
    // Tight, and never below two real pixels however small the scale claims to
    // be. A zero gap would merge the outlines of neighbouring tabs into one
    // double-weight line, which is the reason this is not simply 0.
    CHECK(Near(S::TabGap(1.0f), 2.0f));
    CHECK(Near(S::TabGap(1.333f), 2.666f, 1e-3f));
    CHECK(Near(S::TabGap(0.5f), 2.0f));

    // ---- Layout ----------------------------------------------------------
    {
        const float widths[] = { 100.0f, 50.0f, 30.0f };
        S::Slot     slots[3]{};
        const float total = S::Layout(widths, 3, 10.0f, slots);
        CHECK(Near(slots[0].x, 0.0f));
        CHECK(Near(slots[1].x, 110.0f));
        CHECK(Near(slots[2].x, 170.0f));
        CHECK(Near(slots[2].w, 30.0f));
        // ⚠ NO TRAILING SPACING. A gap after the last tab would scroll into
        // empty space at the right-hand end, which is the "there is more
        // deadspace below" complaint in the other axis.
        CHECK(Near(total, 200.0f));
    }
    {
        // One tab, and the degenerate zero case: neither may invent a spacing.
        const float widths[] = { 42.0f };
        S::Slot     slots[1]{};
        CHECK(Near(S::Layout(widths, 1, 10.0f, slots), 42.0f));
        CHECK(Near(S::Layout(widths, 0, 10.0f, slots), 0.0f));
    }

    // ---- ScrollToReveal --------------------------------------------------
    // Content 500 in a view of 200, so the far clamp sits at 300.
    {
        // Already visible: do not move. A strip that re-scrolls every frame
        // fights the user's own wheel.
        CHECK(Near(S::ScrollToReveal(0.0f, 200.0f, 10.0f, 50.0f, 500.0f), 0.0f));
        CHECK(Near(S::ScrollToReveal(100.0f, 200.0f, 120.0f, 50.0f, 500.0f), 100.0f));

        // Off the left: the slot's left edge becomes the scroll.
        CHECK(Near(S::ScrollToReveal(100.0f, 200.0f, 40.0f, 50.0f, 500.0f), 40.0f));

        // Off the right: move the least that reveals the right edge.
        CHECK(Near(S::ScrollToReveal(0.0f, 200.0f, 180.0f, 50.0f, 500.0f), 30.0f));

        // The last tab cannot scroll past the content's end.
        CHECK(Near(S::ScrollToReveal(0.0f, 200.0f, 470.0f, 30.0f, 500.0f), 300.0f));

        // Everything fits: the scroll is pinned at zero however it arrived.
        CHECK(Near(S::ScrollToReveal(80.0f, 500.0f, 10.0f, 50.0f, 300.0f), 0.0f));

        // ⚠ A SLOT WIDER THAN THE VIEW REVEALS ITS LEFT EDGE. Both branches
        // apply to it, and without the explicit first test they would disagree
        // frame to frame: one wants slotX, the other slotX + w - view.
        CHECK(Near(S::ScrollToReveal(0.0f, 100.0f, 150.0f, 260.0f, 500.0f), 150.0f));
        CHECK(Near(S::ScrollToReveal(400.0f, 100.0f, 150.0f, 260.0f, 500.0f), 150.0f));

        // Negative scroll never survives, whatever it was handed.
        CHECK(Near(S::ScrollToReveal(-50.0f, 200.0f, 10.0f, 50.0f, 500.0f), 0.0f));
    }

    // ---- NeedsArrows -----------------------------------------------------
    // ⚠ ASKED AGAINST THE FULL WIDTH, BEFORE THE ARROWS TAKE THEIR BANDS. The
    // exactly-fits case is the one that matters: it must answer no, because an
    // arrow pair shrinks the row and a yes there would keep them forever.
    CHECK(!S::NeedsArrows(200.0f, 200.0f));
    CHECK(!S::NeedsArrows(150.0f, 200.0f));
    CHECK(S::NeedsArrows(200.5f, 200.0f));

    // ---- ArrowedViewWidth ------------------------------------------------
    CHECK(Near(S::ArrowedViewWidth(200.0f, 24.0f, true), 152.0f));
    CHECK(Near(S::ArrowedViewWidth(200.0f, 24.0f, false), 200.0f));
    // ⚠⚠ NEVER ZERO, WHATEVER THE ROW DOES. BeginChild is given this number
    // and reads a zero as "use the whole remaining region", so a row too narrow
    // for its own controls would hand the tabs everything and draw them over
    // the arrows and the add button. A one-pixel strip is the honest answer.
    CHECK(Near(S::ArrowedViewWidth(30.0f, 24.0f, true), 1.0f));
    CHECK(Near(S::ArrowedViewWidth(48.0f, 24.0f, true), 1.0f));
    CHECK(Near(S::ArrowedViewWidth(0.0f, 24.0f, false), 1.0f));
    CHECK(S::ArrowedViewWidth(-50.0f, 24.0f, true) > 0.0f);

    // ---- BrowseStep ------------------------------------------------------
    CHECK(Near(S::BrowseStep(200.0f), 100.0f));
    // Never zero, or a held arrow would spin against a strip that never moves.
    CHECK(Near(S::BrowseStep(0.0f), 1.0f));

    // ---- ClampScroll and the ends ----------------------------------------
    {
        // Content 500 in a view of 200: the far end is 300.
        CHECK(Near(S::ClampScroll(150.0f, 200.0f, 500.0f), 150.0f));
        CHECK(Near(S::ClampScroll(-40.0f, 200.0f, 500.0f), 0.0f));
        CHECK(Near(S::ClampScroll(900.0f, 200.0f, 500.0f), 300.0f));
        // Everything fits: there is nowhere to browse to.
        CHECK(Near(S::ClampScroll(80.0f, 500.0f, 300.0f), 0.0f));

        CHECK(!S::CanBrowseLeft(0.0f));
        CHECK(S::CanBrowseLeft(1.0f));
        CHECK(S::CanBrowseRight(0.0f, 200.0f, 500.0f));
        CHECK(!S::CanBrowseRight(300.0f, 200.0f, 500.0f));
        // Both arrows are dead when the row fits, which is also when they are
        // not drawn at all.
        CHECK(!S::CanBrowseRight(0.0f, 500.0f, 300.0f));
    }

    // ---- ShouldChaseReveal -----------------------------------------------
    // ⚠⚠ THIS IS THE ONE THAT MADE BROWSING POSSIBLE. Run every frame, the
    // reveal is a clamp onto the selected tab and a hand-moved scroll is
    // dragged back before the next frame is drawn.
    {
        // Nothing moved: leave the scroll where the player put it.
        CHECK(!S::ShouldChaseReveal(3, 3, 200.0f, 200.0f, 500.0f, 500.0f));
        // The selection moved, so the lit tab is worth showing again.
        CHECK(S::ShouldChaseReveal(4, 3, 200.0f, 200.0f, 500.0f, 500.0f));
        // ⚠ AND SO DO THE WIDTHS. A rename or a delete moves the slot the
        // target names without the index changing, and a resize moves the view
        // under it; an index-only trigger parks the selected tab off the end
        // with nothing able to bring it back.
        CHECK(S::ShouldChaseReveal(3, 3, 176.0f, 200.0f, 500.0f, 500.0f));
        CHECK(S::ShouldChaseReveal(3, 3, 200.0f, 200.0f, 460.0f, 500.0f));
        // The first frame has no remembered target and must chase.
        CHECK(S::ShouldChaseReveal(0, -1, 200.0f, 0.0f, 500.0f, 0.0f));
    }

    // ---- browsing a row end to end ---------------------------------------
    // The whole point, played out: look along a library without the selection
    // moving, then have the reveal take it back when the selection does move.
    {
        const float view    = 200.0f;
        const float content = 500.0f;
        float       scroll  = 0.0f;
        const int   lit     = 1;

        // Two presses of the right arrow, each half a view.
        scroll = S::ClampScroll(scroll + S::BrowseStep(view), view, content);
        CHECK(Near(scroll, 100.0f));
        scroll = S::ClampScroll(scroll + S::BrowseStep(view), view, content);
        CHECK(Near(scroll, 200.0f));

        // The frames in between: same selection, same geometry, so nothing
        // reclaims the scroll. This is the assertion the old strip failed.
        CHECK(!S::ShouldChaseReveal(lit, lit, view, view, content, content));

        // Held to the end and it stops there rather than scrolling into blank.
        for (int i = 0; i < 20; ++i) {
            scroll = S::ClampScroll(scroll + S::BrowseStep(view), view, content);
        }
        CHECK(Near(scroll, 300.0f));
        CHECK(!S::CanBrowseRight(scroll, view, content));

        // Now click a tab back at the head of the row. The target changed, so
        // the reveal fires and drags the strip onto it.
        CHECK(S::ShouldChaseReveal(0, lit, view, view, content, content));
        scroll = S::ScrollToReveal(scroll, view, 0.0f, 60.0f, content);
        CHECK(Near(scroll, 0.0f));
    }

    // ---- SlotAt ----------------------------------------------------------
    {
        const float widths[] = { 100.0f, 50.0f };
        S::Slot     slots[2]{};
        S::Layout(widths, 2, 10.0f, slots);
        CHECK(S::SlotAt(slots, 2, 0.0f) == 0);
        CHECK(S::SlotAt(slots, 2, 99.9f) == 0);
        // The spacing between two tabs belongs to neither of them.
        CHECK(S::SlotAt(slots, 2, 105.0f) == 2);
        CHECK(S::SlotAt(slots, 2, 110.0f) == 1);
        CHECK(S::SlotAt(slots, 2, 1000.0f) == 2);
    }

    if (g_failures == 0) {
        std::printf("all OutfitTabStrip tests passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
