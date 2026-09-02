#pragma once

// The mod's colour picker: a saturation and value square over a hue bar, drawn
// inline rather than hidden behind a swatch.
//
// ⚠ HAND BUILT, AND IT HAS TO BE. FUCK exposes ColorEdit3 and ColorEdit4 and
// nothing else in that family: no ColorPicker3, no ColorButton. ColorEdit3's own
// picker lives behind a click on its swatch, and on an unpainted subject that
// swatch is a small black square on a dark panel, so the one control that mixes
// a colour had no visible way in (user 2026-08-04).
//
// ⚠ SHARED SO THE TWO PAGES CANNOT DRIFT. The dye pane grew this first and the
// overlays page wants the same control; two pickers would be two answers to the
// same question, and the band and ramp rules below took a field round each to
// get right. Same reason PreviewCardUI exists.
//
// ⚠ DECLARED HERE AND DEFINED IN EditorUI.cpp, the arrangement PreviewCardUI and
// EditorNotice already use: the body reads that file's own colour helpers and
// draw primitives.
//
// ⚠ IT DOES NOT GATE ITSELF. Whether a player is allowed to mix freely is the
// CALLER's question and the two callers answer it differently: the dye pane
// hides this entirely under lore-friendly, where a colour has to be earned,
// while the overlays page shows it in both modes because body paint is not part
// of that economy (user 2026-08-16).
namespace OS::ColourPicker {

    // What eight-bit RGB cannot hold, kept per picker.
    //
    // ⚠ WITHOUT THIS THE CONTROL FIGHTS THE USER AT THE EDGES. A fully black or
    // fully desaturated colour reports no hue and no saturation, so a round trip
    // through RGB loses where the handle was and the next drag starts from red.
    // One carry per picker rather than one shared: two controls holding the same
    // remembered hue would drag each other about.
    struct Carry {
        float h{ 0.0f };
        float s{ 0.0f };
    };

    // Draws the square and the bar, and reports whether the colour moved this
    // frame. a_out is only written when it did.
    //
    // Sizes itself to the available width and to a share of the available
    // height, so it cannot push what sits under it below the fold.
    [[nodiscard]] bool Draw(Carry& a_carry, const float a_rgb[3], float a_out[3]);

}  // namespace OS::ColourPicker
