#pragma once

namespace OS::FsmpProbe {

    // A TEMPORARY field probe for the FSMP-inside-FR stint (2026-08-23). It
    // ships nothing, gates nothing, and the whole module is deleted with the
    // rest of the switch instruments once the hair gates pass.
    //
    // THE CONTRADICTION IT EXISTS TO SPLIT: the r23 hdtSMP64 log shows FSMP's
    // loop RUNNING during the in-editor browse window (adopts, constraints
    // built, 2 active skeletons, sub-millisecond process times) while the user
    // saw no hair motion. Three stories fit that log and nothing recorded so
    // far can tell them apart:
    //
    //   1. The world clock is frozen in the editor and FSMP idles under the
    //      pause (it never used to - the user's correction outranks the
    //      retracted "by design" reading).
    //   2. FSMP simulates, but its active systems belong to a STALE bone tree
    //      torn off by the browse churn, not to the drawn wig.
    //   3. FSMP created the head systems and never steps them: its
    //      disableSMPHairWhenWigEquipped=true skips hair physics "when an
    //      armor occupies the hair/longhair slot" (FSMP's own tooltip), and
    //      the ghost 'Iron Helmet' 451730E2 sits WORN on biped 31+42 all
    //      session - a worn slot is not a drawn slot, and this rig wears a
    //      ghost.
    //
    // One 6-sample run on each editor edge (open tagged "editor", close tagged
    // "world") logs, per second: the frame-anim clock and calendar deltas
    // (story 1), the count and motion of live hdtSSEPhysics_AutoRename_Head
    // bones (0 live bones = story 2; present-but-frozen = story 3), a body
    // physics bone beside them (hair-only freeze vs whole-actor freeze), and
    // the worn occupants of slots 31/41/42 (story 3's trigger, live).
    //
    // Called from EditorWindow::SetOpen on the main thread, both edges. The
    // watcher is a detached thread posting ONE task per sample - never a task
    // re-queueing a task (the FaceWait scar).

    void OnEditorToggle(bool a_open);

}  // namespace OS::FsmpProbe
