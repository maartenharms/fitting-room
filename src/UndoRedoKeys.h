#pragma once

#include "FuckCompat.h"
#include "UndoRedoPlan.h"

// Ctrl+Z, Ctrl+Shift+Z and Ctrl+Y for every page that keeps a history, read in
// ONE place.
//
// ⚠⚠ FOUR PAGES DRAW UNDO AND REDO BUTTONS AND ONLY ONE OF THEM ANSWERED THE
// KEYBOARD (user 2026-08-18: "a lot of undo redo buttons on all pages don't
// have ctrl z ctrl y"). The outfit page had the chord since OS-21, written
// inline at the foot of its own draw; the Shape, Body Studio and Overlays pages
// each grew the same pair of buttons and none of them read a key. Three more
// copies of that inline block is the shape this file exists to refuse: the
// bindings, the text-field gate and the repeat rule are one answer, and a page
// asks for it rather than restating it.
//
// ⚠ EACH PAGE STILL OWNS ITS OWN STACK. This says WHICH step was asked for and
// nothing about whose history it belongs to, because the pages are mutually
// exclusive - Shape, Body Studio and Overlays each return out of the editor
// draw before the outfit page's own handler is reached - and a central switch
// that knew every page's stack would be a second place to keep that list.
namespace OS::ui::UndoRedo {

    using OS::UndoRedoPlan::Action;

    // ⚠ NO REPEAT, WHICH IS THE `false` ON EVERY IsKeyPressed HERE. A held
    // Ctrl+Z that auto-repeated would walk the whole history in a few frames,
    // and a history the player cannot stop halfway through is worse than one
    // step per press on every page that has one.
    //
    // ⚠ AN ACTIVE ITEM DECLINES, so a search box or a rename field keeps
    // ImGui's own text-edit undo. This is the gate the outfit page already
    // applied and the reason it is inside this function rather than at the four
    // call sites: a page that forgot it would eat the player's typing.
    [[nodiscard]] inline Action Poll() {
        if (FUCK::IsAnyItemActive()) {
            return Action::kNone;
        }
        return OS::UndoRedoPlan::Decide(
            FUCK::IsModifierPressed(FUCK::Modifier::kCtrl),
            FUCK::IsModifierPressed(FUCK::Modifier::kShift),
            FUCK::IsKeyPressed(ImGuiKey_Z, false),
            FUCK::IsKeyPressed(ImGuiKey_Y, false));
    }

}  // namespace OS::ui::UndoRedo
