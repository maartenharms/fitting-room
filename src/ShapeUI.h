#pragma once

namespace RE {
    class Actor;
}

namespace OS::ShapeUI {

    // Re-read the character's current shape out of RaceMenu. Called when the
    // page is entered and when the edit target changes, which are the only two
    // moments the values on screen can be stale.
    void OnOpen(RE::Actor* a_actor);

    // Draws the complete page below EditorUI's shared title/target row.
    void Draw(RE::Actor* a_actor);

    // Leaving the page. Puts the character back the way the page found them
    // unless the edit was committed.
    //
    // ⚠ WHAT COUNTS AS COMMITTED IS DELIBERATE AND IS THE WHOLE MODEL. Dragging
    // sliders is a PREVIEW, exactly as it is on the Body Studio page, and a
    // preview that outlives the page is how a character quietly ends up shaped
    // by an experiment nobody meant to keep (user 2026-08-07, "we should reset
    // the body to whatever we had if we exit"). Two things commit: saving the
    // sliders as a shape, and wearing a shape from the library. Both are a
    // deliberate press that says this is the shape I want.
    void OnClose(RE::Actor* a_actor);

    // Is there an edit on this page that leaving would strand?
    //
    // ⚠ THE SLIDERS ARE ALREADY ON THE CHARACTER, so this is not "your work is
    // about to be lost" in the usual sense. What leaving loses is the baseline
    // Discard puts back: the page re-reads the character on the way in, so once
    // you have left, "the way you found them" is whatever you left them as.
    [[nodiscard]] bool HasUnsavedChanges();

}  // namespace OS::ShapeUI
