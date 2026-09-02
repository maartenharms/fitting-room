#pragma once

#include "FuckCompat.h"  // FUCK

// ⚠⚠ THIS IS AN INERT STUB. THE EDITOR HAS NO CONTROLLER FOCUS MODEL AND NO
// CONTROLLER CURSOR, BY THE USER'S DECISION ON 2026-08-15: "i don't care anymore
// about extensive controller support, just have basic".
//
// Three models were built and field tested in one day and all three were
// rejected by the person who asked for them:
//
//   1. ImGui nav, patched over three rounds. "holy crap the UX is so bad on
//      controller."
//   2. A focus registry the editor owned (panes and items, d-pad and stick).
//      "controls simply don't work, and i've given up."
//   3. A pointer driven by injected mouse input. Worked at the mechanism level
//      and still failed as an experience.
//
// What the pad does now is the set of bindings that never needed a navigation
// model at all, and every one of them is field confirmed: the right stick turns
// and zooms the character, the triggers change page, the shoulders change
// outfit, B and Start close, and Y clears the row under the mouse. Choosing
// things is the mouse's job.
//
// ⚠ THE API SURVIVES AS NO-OPS ON PURPOSE, AND THE CALL SITES ARE INERT RATHER
// THAN WRONG. Every one of them reads `hovered || hit.focused` or
// `clicked || hit.activate`, so with `focused` and `activate` permanently false
// they collapse to exactly the mouse behaviour that was there before any of this
// work started. That is a provable revert rather than sixty hand edits made at
// the end of a long day, and the call sites can be swept whenever it is
// convenient without any of them changing behaviour when they go.
//
// ⚠⚠ ONE THING FROM THAT WORK MUST NOT BE REVERTED: `##navsentinel` in
// EditorUI's root child. MEASURED from a crash log, an access violation in
// FUCK's own ImGui at `NavMoveRequestApplyResult`: with ZERO nav-eligible items
// a d-pad press submits a nav move that resolves to nothing and imgui does not
// guard it. The sentinel is the guard. It is not navigation and must not grow
// into any.
namespace OS::PadFocus {

    enum Pane : int { kRail = 0, kLeft, kRight, kTabs, kActions, kPaneCount };

    inline constexpr int kFlat = 0;

    struct Hit {
        bool focused{ false };
        bool activate{ false };
    };

    [[nodiscard]] inline bool Active() { return false; }
    [[nodiscard]] inline int  Pane() { return kLeft; }
    [[nodiscard]] inline int  FocusedIndex(int) { return -1; }
    [[nodiscard]] inline int  Count(int) { return 0; }
    [[nodiscard]] inline bool JustMoved() { return false; }
    [[nodiscard]] inline bool TakeClose() { return false; }

    [[nodiscard]] inline Hit Item(int, bool = true) { return {}; }
    [[nodiscard]] inline Hit At(int, int) { return {}; }

    inline void SetCols(int, int) {}
    inline void Declare(int, int, int) {}
    inline void KeepInView(const Hit&) {}
    inline void GoTo(int, bool = false) {}
    inline void Reset() {}

    // ⚠ THE ONE LINE THAT STILL DOES SOMETHING. Nothing in this editor carries
    // EnableNav except the sentinel, so ImGui must never paint a nav highlight
    // of its own over a UI that is driven entirely by the mouse.
    inline void BeginFrame() {
        if (FUCK::GetInputDevice() == FUCK::InputDevice::kGamepad) {
            FUCK::SetNavCursorVisible(false);
        }
    }

}  // namespace OS::PadFocus
