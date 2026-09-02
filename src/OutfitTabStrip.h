#pragma once

#include <algorithm>
#include <cstddef>

// Layout for the hand-rolled outfit tab strip.
//
// ⚠⚠ THE STRIP IS DRAWN BY US NOW, AND THE REASON IS THAT FLICK'S TABS CANNOT
// BE DECORATED AT ALL. FUCK::BeginTabItem submits the tab background AND its
// label in one call, and FLICK exposes no draw-list channels and no clip rect,
// so nothing of ours can ever land behind the text. That was recorded on
// 2026-08-12 as the reason the strip stayed square while the rest of the editor
// got its cut corner, and it is also why the "+" beside it could only ever
// approximate a tab: it was painting from the widget colours while the real
// tabs painted from rTabColor / rTabBorderColor, which the theme authors
// separately (user 2026-08-12, "it's not really well integrated with the other
// tabs").
//
// ⚠ THE SCROLL IS OURS TOO, AND NOT BY CHOICE. The FUCK ABI has SetScrollHereY
// and no SetScrollX of any kind, so a child cannot be scrolled horizontally
// through it. The strip therefore lives in a clipping BeginChild and the
// content is OFFSET by a scroll this header computes. ImGuiTabBarFlags_
// FittingPolicyScroll used to supply this; nothing in the ABI replaces it.
namespace OS::OutfitTabStrip {

    // One tab's box, in strip-local coordinates: x measured from the content
    // origin, BEFORE the scroll offset is applied.
    struct Slot {
        float x{ 0.0f };
        float w{ 0.0f };
    };

    // ⚠ THE SAME FORMULA FLICK'S OWN TAB USED, and it is worth keeping rather
    // than inventing a nicer one. ImGui::TabItemCalcSize was read out of FUCK's
    // PDB on 2026-08-12: label plus style.FramePadding.x twice. The live log
    // agreed to the pixel - FramePadding (8, 4), textW("Abyss") 49, tab width
    // 66 - so a strip drawn to this formula is the width the strip has always
    // been, and only the PAINT changes. Anyone widening this is changing how
    // many outfits fit on screen, which is a separate decision from the one
    // that moved the drawing.
    [[nodiscard]] inline constexpr float TabWidth(float a_textWidth, float a_padX) {
        return a_textWidth + a_padX * 2.0f;
    }

    // ⚠⚠ NEIGHBOURING TABS SIT TIGHT, AND ItemSpacing IS THE WRONG NUMBER FOR
    // THEM. It is the gap between two independent controls on a row, and a
    // strip of tabs is not that: they are one object, and FLICK's own tab bar
    // butted them together with no gap at all (user 2026-08-12, "there is a lot
    // of space between the outfit tabs"). Zero is not available to us either,
    // because these tabs carry an outline that FLICK's did not, and two
    // outlines sharing an edge draw a double-weight line between every pair.
    //
    // Two device pixels is the smallest gap that keeps the strokes apart at any
    // resolution scale, which is the only job this number has.
    [[nodiscard]] inline float TabGap(float a_resolutionScale) {
        return 2.0f * std::max(1.0f, a_resolutionScale);
    }

    // Lay the tabs out left to right with one spacing between neighbours, and
    // report the total content width. a_out must have room for a_count slots.
    //
    // The trailing "+" is just another width in the array: it is the same shape
    // as a tab now, and giving it its own case here is what let it drift.
    inline float Layout(const float* a_widths, std::size_t a_count, float a_spacing,
                        Slot* a_out) {
        float x = 0.0f;
        for (std::size_t i = 0; i < a_count; ++i) {
            a_out[i].x = x;
            a_out[i].w = a_widths[i];
            x += a_widths[i];
            if (i + 1 < a_count) {
                x += a_spacing;
            }
        }
        return x;
    }

    // How far the strip must be scrolled for one slot to be wholly visible,
    // moving as little as possible from where it already is.
    //
    // ⚠ CLAMPED AT BOTH ENDS AND THE FAR CLAMP COMES LAST. A slot near the end
    // asks for a scroll that would leave blank space past the final tab; the
    // content-width clamp pulls it back, and doing that before the reveal would
    // let the reveal push past it again.
    //
    // ⚠ A SLOT WIDER THAN THE VIEW REVEALS ITS LEFT EDGE. Both branches would
    // otherwise fight every frame, and the left edge is where the label starts.
    [[nodiscard]] inline float ScrollToReveal(float a_scroll, float a_viewWidth,
                                             float a_slotX, float a_slotWidth,
                                             float a_contentWidth) {
        const float maxScroll = std::max(0.0f, a_contentWidth - a_viewWidth);
        float       scroll    = a_scroll;
        if (a_slotWidth >= a_viewWidth || a_slotX < scroll) {
            scroll = a_slotX;
        } else if (a_slotX + a_slotWidth > scroll + a_viewWidth) {
            scroll = a_slotX + a_slotWidth - a_viewWidth;
        }
        return std::clamp(scroll, 0.0f, maxScroll);
    }

    // ---- browsing the row without wearing anything -----------------------
    //
    // Every way along this strip used to change the outfit, and changing outfit
    // restyles the character (user 2026-08-27, "i want to improve UX of
    // switching/scrolling between tabs, it's still awkward"). There was no
    // gesture for looking further along the row, so a long library could only
    // be read by wearing every entry in it, and each of those pinned a manual
    // pick and asked a modal if any work was in hand. The helpers below are the
    // maths for looking without wearing.

    // Whether the row is longer than the space it was given, asked BEFORE the
    // arrows take an end each.
    //
    // ⚠ AGAINST THE FULL WIDTH ON PURPOSE, and the answer settles in one pass
    // because of it: arrows only ever make the visible width SMALLER, so a row
    // that overflows without them still overflows with them. Asking after they
    // were reserved lets a row that just fits grow arrows, which shrink the
    // row, which keeps the arrows - a strip that flickers at exactly one
    // library size and is stable at every other, which is the worst kind.
    [[nodiscard]] inline constexpr bool NeedsArrows(float a_contentWidth,
                                                    float a_fullWidth) {
        return a_contentWidth > a_fullWidth;
    }

    // What is left for the tabs once the arrows have their bands.
    //
    // ⚠ A BAND EACH RATHER THAN A GLYPH LAID OVER THE TABS. An arrow drawn on
    // top of the strip covers a tab that is still hit-testable underneath it,
    // so the click nearest the edge - the one a player reaching for "more"
    // actually makes - would land on whichever of the two won the id stack.
    //
    // ⚠ WHERE the two bands sit is the caller's business and this does not
    // care: they were one at each end until 2026-08-27 and are a pair on the
    // right now, beside the add button. Only the total matters here.
    // ⚠⚠ NEVER ZERO, AND THAT IS NOT TIDINESS. The only consumer of this
    // number is the strip's BeginChild, and ImGui reads a zero size component
    // as "use the whole remaining content region" rather than as "no width". A
    // row too narrow to hold its own controls would therefore hand the tabs the
    // ENTIRE row and draw them straight over the arrows and the add button,
    // which is a worse answer than a one-pixel strip and does not look like a
    // layout bug when it happens.
    [[nodiscard]] inline float ArrowedViewWidth(float a_fullWidth,
                                                float a_arrowWidth,
                                                bool  a_arrows) {
        const float view =
            a_arrows ? a_fullWidth - a_arrowWidth * 2.0f : a_fullWidth;
        return std::max(1.0f, view);
    }

    // How far one arrow press, or one notch of a browse wheel, moves the strip.
    //
    // ⚠ A FRACTION OF THE VIEW, NOT A TAB. Widths here are label widths, so
    // "one tab" travels a different distance depending on where along the row
    // it starts: a library of short names would crawl while one long name
    // jumped. Half a view also leaves half of what was on screen on screen,
    // which is what makes a second press read as a continuation rather than as
    // a new place.
    [[nodiscard]] inline float BrowseStep(float a_viewWidth) {
        return std::max(1.0f, a_viewWidth * 0.5f);
    }

    // The scroll a hand-moved strip is allowed to hold. ScrollToReveal clamps
    // its own answer; nothing that moves the scroll directly gets that for
    // free, and an unclamped browse scrolls into blank space past the last tab.
    [[nodiscard]] inline float ClampScroll(float a_scroll, float a_viewWidth,
                                           float a_contentWidth) {
        return std::clamp(a_scroll, 0.0f,
                          std::max(0.0f, a_contentWidth - a_viewWidth));
    }

    [[nodiscard]] inline constexpr bool CanBrowseLeft(float a_scroll) {
        return a_scroll > 0.0f;
    }

    [[nodiscard]] inline bool CanBrowseRight(float a_scroll, float a_viewWidth,
                                             float a_contentWidth) {
        return a_scroll < std::max(0.0f, a_contentWidth - a_viewWidth);
    }

    // Whether the strip should chase its reveal target this frame.
    //
    // ⚠⚠ THE REVEAL IS EDGE TRIGGERED, AND EVERY WAY OF BROWSING DEPENDS ON
    // THAT ONE FACT. ScrollToReveal moves as little as it can, which reads like
    // "leave the scroll where it is", but it is a clamp evaluated EVERY FRAME:
    // move the strip past the selected tab by hand and the next frame drags it
    // straight back onto it. Nothing could have scrolled this strip while that
    // ran unconditionally, which is the real reason the only way along the row
    // was to change what the character was wearing.
    //
    // ⚠ THE GEOMETRY TERMS ARE NOT DECORATION. A rename, a delete or a pane
    // resize moves the slot the target names without the target itself
    // changing, and an index-only trigger would leave the selected tab parked
    // off the end with nothing left in the frame able to bring it back.
    [[nodiscard]] inline bool ShouldChaseReveal(int a_slot, int a_lastSlot,
                                                float a_viewWidth,
                                                float a_lastViewWidth,
                                                float a_contentWidth,
                                                float a_lastContentWidth) {
        return a_slot != a_lastSlot || a_viewWidth != a_lastViewWidth ||
               a_contentWidth != a_lastContentWidth;
    }

    // Which slot a strip-local x lands in, or a_count if it lands in a gap or
    // past the end. Used only by the tests and by anything that wants to reason
    // about the strip without submitting items; the live strip hit-tests with
    // an InvisibleButton per tab, which is what gives it hover and click for
    // free and keeps the id stack honest.
    [[nodiscard]] inline std::size_t SlotAt(const Slot* a_slots, std::size_t a_count,
                                            float a_x) {
        for (std::size_t i = 0; i < a_count; ++i) {
            if (a_x >= a_slots[i].x && a_x < a_slots[i].x + a_slots[i].w) {
                return i;
            }
        }
        return a_count;
    }

}  // namespace OS::OutfitTabStrip
