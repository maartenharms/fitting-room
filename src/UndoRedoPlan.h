#pragma once

// Which history step a keyboard chord is asking for.
//
// ⚠ PURE, LIKE THE OTHER *Plan.h HEADERS BESIDE IT. No FUCK, no ImGui, no
// engine: the chord table is the part worth pinning and it cannot be exercised
// through an input context that only exists inside a running game.
//
// The bindings are the ones every editor on Windows ships and the ones the
// outfit page has had since OS-21:
//
//   Ctrl+Z          undo
//   Ctrl+Shift+Z    redo, and the chord the tooltip names (user 2026-09-02)
//   Ctrl+Y          redo as well, kept for the hands that reach for it
namespace OS::UndoRedoPlan {

    enum class Action {
        kNone,
        kUndo,
        kRedo
    };

    // ⚠⚠ SHIFT IS ASKED BEFORE THE Z IS HONOURED, and that order is the whole
    // subtlety. Z appears in both chords, so a test written as "if z then undo"
    // undoes on Ctrl+Shift+Z and the redo binding silently becomes a second
    // undo - a wrong step in the direction the player was trying to leave.
    //
    // ⚠ A CHORD WITHOUT CTRL IS NOTHING HERE. Plain Z and plain Y belong to
    // whatever else reads the keyboard; this answers only for the modifier.
    [[nodiscard]] inline constexpr Action Decide(bool a_ctrl, bool a_shift, bool a_z,
                                                 bool a_y) {
        if (!a_ctrl) {
            return Action::kNone;
        }
        if (a_z && !a_shift) {
            return Action::kUndo;
        }
        if (a_y || (a_z && a_shift)) {
            return Action::kRedo;
        }
        return Action::kNone;
    }

}  // namespace OS::UndoRedoPlan
