#pragma once

#include <string>

namespace RE {
    class Actor;
}

namespace OS::OverlaysUI {

    // ---- what the camera should be looking at -----------------------------
    //
    // The editor already knows how to swing Menu Studio's camera onto the part
    // of the character being worked on, and it is the same want here: picking a
    // face layer should put the face in shot. So this page answers the shot
    // driver's two questions rather than driving a camera of its own, which
    // also means one painter of that framing instead of two.
    //
    // ⚠ THE KEY IS THE SELECTION AND NOT THE NODE IT PRODUCES. Every layer in a
    // location gives the same node, so a node comparison cannot tell "the user
    // picked a different layer" from "nothing happened", and re-clicking the
    // layer you have orbited away from would never bring the shot back.

    // The overlay node the selected layer lives on, for the flash. Empty when
    // nothing is selected.
    //
    // ⚠ THIS IS THE OVERLAY NODE AND NOT THE SKELETON NODE ShotNode RETURNS.
    // The camera turns around a bone; the flash lights the layer's own sheet.
    // Aiming either at the other gives a shot pivoting on a texture or a
    // whole thigh lit up for a freckle.
    [[nodiscard]] const char* SelectedOverlayNode();

    // The node of a layer whose CONTENT just changed, taken once and cleared.
    // Empty when nothing changed this frame.
    //
    // ⚠⚠ THE SHOT DRIVER CANNOT ANSWER THIS AND THAT IS WHY IT EXISTS. The
    // flash rides UpdateShotFocus, which fires on a change of SELECTION, so
    // choosing a different texture for the layer already selected changed the
    // character and lit nothing: the one moment a player most wants to be shown
    // where their pick landed (user 2026-08-16). Applying, emptying and
    // reordering all change what is worn without touching the selection, so the
    // page reports them here and the editor arms the same flash it already
    // arms for a selection change.
    //
    // ⚠ REPORTED, NOT FLASHED. OutfitDye::FlashNode is game thread only and the
    // page draws inside FLICK's Present hook, so the marshalling stays in the
    // one place that already does it rather than being copied here.
    [[nodiscard]] std::string TakeChangedNode();

    // ⚠⚠ A MAKEUP LAYER NEVER REPORTS, AND THAT IS MEASURED RATHER THAN
    // FORGOTTEN. A flash marks a SHAPE and makeup has none: it is painted into
    // the head's tint texture, and the head is one geometry. Narrowed as far as
    // it goes, the walk lit 2 of 21 shapes and washed the whole head anyway,
    // because those two ARE the head. See the note above NoteMakeupChanged in
    // the .cpp for the log line. Only overlay layers report here.

    // A stable string for the current selection, for the driver's change test.
    [[nodiscard]] std::string ShotSelectionKey();

    // The skeleton node this page wants framed, empty when it wants nothing.
    [[nodiscard]] const char* ShotNode();

    // How tight, on the driver's 0 to 1 scale.
    [[nodiscard]] float ShotCloseness();

    // Re-read the character's layers out of RaceMenu, and install the slots if
    // this character has none. Called when the page is entered and when the edit
    // target changes, which are the only two moments the page can be stale.
    void OnOpen(RE::Actor* a_actor);

    // Draws the complete page below EditorUI's shared title and target row.
    void Draw(RE::Actor* a_actor);

    // ⚠ THERE IS NO OnClose AND NO HasUnsavedChanges, DELIBERATELY, AND IT IS
    // THE SAME CALL THE SHAPE PAGE MAKES. Every control here writes straight
    // through to a node override, and the override is the storage: it rides the
    // save, it survives a page change, and it is what RaceMenu itself would
    // show. There is no staged edit for leaving to strand, so a leave warning
    // would fire on a page that has nothing to lose.
    //
    // What that costs is an undo, which is why Clear is per layer rather than
    // only a wipe of everything.
    //
    // ⚠ NO kRendererVersion BUMP, AND THE QUESTION WAS ASKED RATHER THAN
    // SKIPPED. The overlays handoff expected this page to be a render change
    // for the preview cards. It is not, and the reason is structural: the
    // cards are rendered from meshes loaded out of NIF files, and nothing in
    // the preview path reads the live actor's 3D at all. Overlay nodes are
    // created at runtime on the actor and exist in no NIF, so no card can ever
    // contain one and no cached card can be stale because of this page. If a
    // preview is ever taught to shoot the live character, that changes and this
    // note is where to start.

}  // namespace OS::OverlaysUI
