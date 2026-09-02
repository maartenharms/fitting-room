#pragma once

namespace RE {
    class Actor;
}

// Body weight, per CHARACTER.
//
// ⚠ THERE IS NO STORAGE HERE AND THAT IS THE DESIGN. Weight lives on
// TESNPC::weight, which is the actor BASE, and the game already writes base
// form edits into the save. Adding a co-save field for it would be a second
// copy of a value the engine persists, and the two would disagree the moment
// anything else moved it (console, RaceMenu, another mod). So this reads and
// writes the engine's own value and stores nothing.
//
// ⚠ WRITING WEIGHT WITHOUT REBUILDING THE MORPH PLAN GIVES A BODY THAT
// DISAGREES WITH ITS OWN PRESET. Every slider in a preset is a function of
// weight (BodyMorphInterpolatePercent walks small -> big across it), so the
// gesture is not "set a number and refresh", it is "set a number, rebuild the
// plan, re-apply it". Set() does all three.
namespace OS::BodyWeight {

    // The character's current weight, 0..100. 0 for a null actor or a null base.
    [[nodiscard]] float Of(RE::Actor* a_actor);

    // ⚠ REFUSES A SHARED BASE, AND THIS IS THE FAIL-SAFE DIRECTION RATHER THAN
    // A LIMITATION WAITING TO BE LIFTED. TESNPC::weight is on the base, so a
    // generic actor's weight is shared by EVERY instance of that base in the
    // game, and the edit persists into the save. An outfit on a shared base
    // dresses several actors and is reversible; a weight on one reshapes them
    // permanently and nothing in the editor would say so. The player and any
    // unique follower are unique bases and are unaffected by this.
    [[nodiscard]] bool CanEdit(RE::Actor* a_actor);

    // Write the weight. Refuses when CanEdit is false. Safe to call from the
    // render thread: the engine write is marshaled to the game thread.
    //
    // ⚠ THE CALLER MUST RE-APPLY THE BODY AFTERWARDS, and this deliberately
    // does not. Every slider in a preset interpolates small -> big across
    // weight, so after this the applied morphs describe the OLD weight and the
    // shape does not move until something rebuilds the plan. Which body to put
    // back depends on the staged outfit (an installed OBody preset and a custom
    // Body Studio one take different paths), which is editor state this module
    // has no business reading - EditorUI::RestoreStagedBody already answers it
    // for both.
    //
    // Queue the re-apply immediately after this call. Both go through the SKSE
    // task queue on the main thread in FIFO order, so the rebuild reads the
    // weight this wrote.
    //
    // ⚠ NO Reset3D anywhere in that chain. The morph apply performs its own
    // refresh and that path is field tested; a second rebuild beside it is how
    // a working path acquires the hair-tint bug.
    void Set(RE::Actor* a_actor, float a_weight);

}  // namespace OS::BodyWeight
