#pragma once

#include "ProfileCodec.h"

#include <string>

namespace RE {
    class Actor;
}

// The capture half of W1: read every block off the live actor into a
// ProfileCodec::Profile and save it through ProfileStore.
//
// The face block is the one asynchronous piece. CharGen.SaveCharacter is a
// Papyrus dispatch whose file lands whenever the VM gets to it (1.8 s
// field-measured, and never while a menu pause holds the VM), so the profile
// saves at once, faceless, and the face block patches itself into the stored
// file when the dispatch settles. The waiting happens on a sleeping watcher
// thread with a wall-clock budget (FaceWait.h, which also carries the freeze
// scar that forbids a task-requeue wait); running out degrades to a faceless
// profile with one warn line, the posture every missing block has.
namespace OS::ProfileCapture {

    // Capture a_player into a profile named a_name and save it. With
    // a_captureFace the jslot is captured under "FR_<sanitized name>" and the
    // face block reads "captured"; without it no face block is written.
    // Safe from any thread: actor reads happen on the calling thread the way
    // the pages already read, and everything else - the store write, the
    // stale-jslot remove, the dispatch - runs on one game-thread task.
    void Capture(RE::Actor* a_player, const std::string& a_name,
                 bool a_captureFace);

    // The synchronous block reads alone - everything Capture reads off the
    // live actor except the face - handed back for callers that pair them
    // with a face of their own. The preset browser's Save as look is the
    // customer: the current gear, body, colours and character wrapped
    // around a REFERENCED preset face, because a face-only row surprised
    // the field ("just saves the face?", 2026-08-22 23:5x).
    [[nodiscard]] ProfileCodec::Profile SnapshotBlocks(RE::Actor* a_player,
                                                       const std::string& a_name);

    // Whether a_name's face half is still settling (dispatched, jslot not
    // landed). The window is however long the editor stays open, since its
    // pause holds the VM; the page reads this to say "still saving" instead
    // of showing a faceless look with no explanation.
    [[nodiscard]] bool FacePending(const std::string& a_name);

    // A save load tears down the character every pending dispatch was aimed
    // at, so every face wait abandons: a SaveCharacter that settled on the
    // OTHER side of a load would capture whoever the player is THEN, and
    // patching that in would put a stranger's head on the look (field
    // 2026-08-22 03:2x: a look applied faceless because its face never got
    // to settle before the save changed). Abandoned names are remembered so
    // a jslot that lands late anyway is not healed in this session. Wired
    // into Persistence::RevertCallback.
    void OnRevert();

    // Export the player's head as RaceMenu's external-character trio:
    // CharGen\Exported\<name>.jslot always, plus <name>.nif and .dds through
    // skee's own export task when RaceMenu's bEnableHeadExport is on. The
    // name must already be normalised (PresetBrowse::NormalizeExportName);
    // the dispatch settles whenever the VM runs - never while the editor's
    // pause holds it - so a sleeping watcher reports which halves landed,
    // BOTH of them by name, and a missing nif is called what it is
    // (RaceMenu's head export switched off), not a failure.
    void ExportHeadTrio(const std::string& a_name);

    // Whether an export dispatch is still settling; the page disables the
    // button and says so instead of letting a second press race the first.
    [[nodiscard]] bool ExportPending();

    // Reconcile faceless profiles against CharGen\Exported: one whose own
    // FR_ jslot exists gains its face block, since that jslot was captured
    // under this profile's name and is its face by construction. Heals the
    // orphaned pairs the pre-fix builds left behind and any capture whose
    // patch was lost to a quit. Skips names still pending and names whose
    // wait was abandoned this session. Marshals itself onto a game task;
    // a_onHealed (optional) runs there after at least one profile healed.
    void HealFacelessFromDisk(void (*a_onHealed)() = nullptr);

}  // namespace OS::ProfileCapture
