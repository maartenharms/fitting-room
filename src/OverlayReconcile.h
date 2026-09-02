#pragma once

// OS-233: put the player's body overlays back after RaceMenu's post-load pass
// takes them.
//
// ⚠⚠ THE MECHANISM, MEASURED 2026-08-19 ACROSS FIVE LOADS. About 1.5 s after a
// save loads, RaceMenu runs its own "reapply the player" pass. For the body it
// reverts the `Body [Ovl]` clones it installed at the initial attach and then
// looks the body armour up by the FORM WORN to build them again; the model on
// that slot is Fitting Room's styled one, so it finds nothing to clone from and
// installs nothing. The Face clones survive because the head is not looked up
// that way. Every reading agreed: 7 body clones at the initial attach, 7 either
// side of Fitting Room's own load-time refresh, 0 at RaceMenu's face reinstall
// one frame later, and OverlayFix (which wraps skee's install and logs every
// attempt) logging nothing at that moment.
//
// What puts them back is a real CHANGE of the body slot across a refresh:
// skee's attach-time build runs synchronously with the attached node. Measured
// on the editor's Hide outfit toggle: styles off (the skin body attaches) 0 to
// 7 inside the refresh, styles back on (the styled top attaches) revert and
// rebuild inside the refresh, 7. A same-model refresh does nothing (Skyrim
// reuses the slot's model, skee's hook never fires), and none of skee's public
// calls creates them for the player: AddOverlays, RevertOverlays and
// RemoveOverlays plus AddOverlays were each tried on 01:21 and left 0.
//
// So the repair is those two passes, run back to back inside ONE task drain so
// no frame is presented between them and nothing flashes. It runs only when
// asked and only when it has to: RaceMenu holds at least one body layer with
// art on it and none of the clones is on the player's third person 3D.
//
// Triggers: skee's install callback firing for the player's Face (that IS
// RaceMenu's post-load pass, one frame after the body clones went) arms a check
// 150 ms out; the load-time refresh arms a fallback 2 s out for a rig with no
// face layers to fire on. The check itself runs on the game thread off the
// rules heartbeat's 100 ms poll, which already exists. Twice per load at most.
//
// Skipped while the character editor is open (RaceMenu owns the overlays there
// and does a full rebuild on close), while the player has no 3D, and when the
// overlay interface is not there.

namespace RE {
    class Actor;
}

namespace OS::OverlayReconcile {

    // A new save started loading: forget this load's attempt count.
    void OnLoad();

    // Fitting Room's own load-time refresh finished; arm the fallback check.
    void ArmAfterLoadRefresh();

    // skee installed a Face overlay on the player. Cheap; called from inside
    // skee's install, per node. Arms the check.
    void NoteFaceInstall();

    // A ProfileApply finished on the player. From here until the next load the
    // orphan-blank pass stands down: whatever art the body wears now is the
    // look's own, whatever the store says. ⚠⚠ r42 MEASURED, 3-for-3: a
    // face-only preset apply (skee's LoadCharacterPresetEx replaces the whole
    // overlay store with the preset's, and a bare face preset carries none)
    // left the store empty with the look's art still painted, and the blank
    // pass erased that art 0.15-0.5 s later every time. That is the field's
    // "a look applies correctly once and then stops".
    void NoteProfileApply();

    // The player's overlays were edited on the Overlays page: arm the check,
    // and when this load's repair budget is spent, allow one more attempt, so
    // a user who touches the layers after a failed repair gets one more
    // rebuild instead of silence. Field 2026-09-02 03:11: both attempts made 0
    // clones and the user's later edits could not re-arm it. Game thread.
    void NoteOverlayEdit();

    // Game thread, every 100 ms. Runs the armed check when it is due.
    void Tick();

    // A number that changes whenever skee's override store may have been
    // rewritten under a reader holding a copy of it.
    //
    // ⚠⚠ THE OVERLAYS PAGE HOLDS EXACTLY SUCH A COPY (user 2026-08-24:
    // "overlay layers get cleared/hidden in the Overlays page after loading
    // racemenu presets, they can be on the body visually but not in the
    // overlays page, very odd"). Its rows are read once on page entry, and
    // `LoadCharacterPresetEx` REPLACES the whole store with the preset's own,
    // which a bare face preset does not carry. The clones on the body are
    // geometry with a material and nobody repaints them from the store, so the
    // art stays while the record goes, and a page that re-read only on entry
    // showed rows from before the wipe or empty rows after it with no way to
    // tell which.
    //
    // ⚠ MONOTONIC, AND THE VALUE MEANS NOTHING. Compare it to one you took
    // earlier; if it moved, read again. It is bumped by the face-install
    // callback, by a profile apply and by a load, never reset.
    [[nodiscard]] std::uint32_t StoreGeneration();

    // How many `Body [ ... Ovl ... ]` geometries hang under the player's third
    // person root; -1 with no 3D. Game thread. Shared with the probe so both
    // read one answer.
    [[nodiscard]] int CountPlayerBodyClones();

    // Body plus Hands clones under the player's FIRST person root; -1 with no
    // 1p 3D. The first person renders only those two locations, so feet and
    // face have no clone to count there.
    [[nodiscard]] int CountPlayerFirstPersonClones();

    // The overlay-clone census, both persons, player only, debug level: one
    // line per root with its Body/Hands/Feet/Face [Ovl] counts, then one line
    // per HANDS clone naming the diffuse it wears. Field 2026-08-21 ("hand
    // overlays don't appear in first person") is the reason it exists: skee's
    // source installs on both roots and the SetNodeProperties push applies to
    // both, so whichever link is broken on this rig, the census says which -
    // no clones on the 1p root is the install half, clones wearing the
    // default diffuse are the apply half. Game thread.
    void LogOverlayCensus(RE::Actor* a_actor);

}  // namespace OS::OverlayReconcile
