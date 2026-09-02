#pragma once

#include "FuckCompat.h"  // the editor's one context-menu shape, and ItemRightClicked

#include <algorithm>

// Expand or collapse EVERY accordion on the page, from a right-click on any one
// of them (user 2026-08-16: "we need a simple way to collapse and expand all
// accordions, i'm thinking of adding this as a drop down when you right click
// any accordion", then "make sure this applies to all accordions in all pages").
//
// ⚠⚠ THE STATE IS A ONE-SHOT AND IT IS NOT NEGOTIABLE. SetNextItemOpen's
// default condition is Always, so a stored "everything is collapsed" flag would
// re-assert itself every frame and NO header on the page could be opened by
// clicking it. That is not a theory: it shipped on the Shape page on
// 2026-08-07 and no accordion there could be opened at all until it was found.
// So a request is applied on exactly ONE frame and then expires, which hands
// the open state straight back to each header's own.
//
// ⚠⚠ AND THE EXPIRY IS A FRAME BOUNDARY, NOT A Draw() CALL. Menu() is called
// from several pages, and clearing the request inside it would mean the first
// page to call it wiped the request before the second page's headers ever saw
// it. NewFrame() promotes a pending request into the live one exactly once per
// frame, from the top of the editor's own Draw, so any number of Menu() sites
// is safe and their order does not matter.
//
// ⚠ ONE REQUEST FOR THE WHOLE EDITOR RATHER THAN ONE PER PAGE. Only the page
// being drawn submits headers, so a global request reaches precisely the
// accordions on screen and nothing else. A per-page map would be state that
// exists only to describe which page is drawing, which the draw already knows.
//
// ⚠ NOT ImGui:: ANYTHING IN HERE, including a frame counter. ImGui calls made
// from Fitting Room bind to FITTING ROOM's own ImGui context rather than the
// host's, which is a measured CTD; everything goes through FUCK. That is why
// the frame boundary is a call the editor makes rather than a frame number this
// file reads.
namespace OS::ui::FoldAll {

    namespace detail {
        // -1 nothing asked, 0 collapse every accordion, 1 expand every one.
        inline int  g_live{ -1 };     // applies to headers drawn THIS frame
        inline int  g_pending{ -1 };  // what the menu asked for, for the next one
        inline bool g_wantMenu{ false };
    }  // namespace detail

    // ⚠ AT THE TOP OF THE EDITOR'S PER-FRAME DRAW, BEFORE ANY HEADER SUBMITS.
    // Promotes the menu's click into this frame's request and lets the previous
    // frame's expire.
    inline void NewFrame() {
        detail::g_live    = detail::g_pending;
        detail::g_pending = -1;
    }

    // Immediately BEFORE submitting a collapsing header.
    //
    // Returns true when it asserted an open state, so a caller that has a
    // SetNextItemOpen of its own (the pad toggle in FramedCollapsingHeader)
    // knows not to call a second one over the top of it.
    inline bool Before() {
        if (detail::g_live < 0) {
            return false;
        }
        FUCK::SetNextItemOpen(detail::g_live == 1);
        return true;
    }

    // Immediately AFTER submitting one.
    //
    // ⚠ CALL THIS EVEN WHEN THE HEADER CAME BACK SHUT, before any early return
    // on it. Right-clicking a collapsed accordion is the likeliest way anyone
    // reaches "expand all", and a menu that only answered on open headers would
    // be missing exactly when it is wanted.
    inline void After() {
        if (OS::ui::ItemRightClicked()) {
            detail::g_wantMenu = true;
        }
    }

    // The menu itself, at WINDOW SCOPE and after that window's headers.
    //
    // ⚠ BOTH HALVES HERE, IN ONE CALL, which is what makes the id stack safe.
    // FuckCompat.h carries the rule and the three times this project has paid
    // for breaking it: OpenPopup and BeginPopup hash their name against the
    // current id stack, so a menu opened at one depth and begun at another is
    // two different popups and the control silently does nothing.
    inline void Menu() {
        if (detail::g_wantMenu) {
            detail::g_wantMenu = false;
            OS::ui::OpenContextMenu("fold_all");
        }
        if (OS::ui::BeginContextMenu("fold_all")) {
            if (OS::ui::ContextMenuItem(FUCK::Translate("$FR_Shape_ExpandAll"))) {
                detail::g_pending = 1;
            }
            if (OS::ui::ContextMenuItem(FUCK::Translate("$FR_Shape_CollapseAll"))) {
                detail::g_pending = 0;
            }
            OS::ui::EndContextMenu();
        }
    }

}  // namespace OS::ui::FoldAll

namespace OS::ui {

    // A collapsing header that keeps its box when it is SHUT, and that carries
    // the fold-all menu whether it is open or not.
    //
    // ⚠ FLICK'S THEME FRAMES AN EXPANDED HEADER AND DROPS THE FRAME WHEN IT
    // CLOSES, so a column of accordions loses its structure at exactly the
    // moment the player collapses it. The four fills below put the edge back.
    //
    // ⚠ HERE RATHER THAN IN EACH PAGE, and the reason is that Body Studio had
    // written it and the Looks page's block groups were about to write it again
    // (2026-08-25). EditorUI keeps its own copy on purpose: that one also owns
    // the pad cursor and a one-frame-late toggle, which is a second job this
    // has no business growing.
    [[nodiscard]] inline bool FramedHeader(const char* a_label, int a_flags = 0) {
        // Expand-all and collapse-all reach these too, from a right-click on any
        // one of them. In the wrapper rather than at the call sites, so nothing
        // using it can be left out. See above.
        (void)FoldAll::Before();
        const bool open = FUCK::CollapsingHeader(a_label, a_flags);
        // ⚠ BEFORE THE EARLY RETURN, so a SHUT header carries the menu.
        FoldAll::After();
        if (open) {
            return true;
        }
        const ImVec2 min       = FUCK::GetItemRectMin();
        const ImVec2 max       = FUCK::GetItemRectMax();
        const float  thickness = std::max(1.0f, FUCK::GetResolutionScale());
        const ImU32  color     = OS::ui::Col(ImGuiCol_Border);
        OS::ui::RectFilled(min, ImVec2(max.x, min.y + thickness), color);
        OS::ui::RectFilled(ImVec2(min.x, max.y - thickness), max, color);
        OS::ui::RectFilled(min, ImVec2(min.x + thickness, max.y), color);
        OS::ui::RectFilled(ImVec2(max.x - thickness, min.y), max, color);
        return false;
    }

}  // namespace OS::ui
