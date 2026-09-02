#pragma once

// The face-settle wait, as arithmetic. One call per watcher slice; the
// watcher thread in ProfileCapture.cpp sleeps between calls, which is the
// entire fix for the 2026-08-22 01:25 page-save freeze.
//
// ⚠⚠ WHY A THREAD AND NOT A TASK REQUEUE. The old wait re-added itself to
// the SKSE task queue, whose drain runs until the queue is EMPTY (skse64
// Hooks_Threads.cpp: `while (!IsTaskQueueEmpty()) { pop; Run(); }`, read
// 2026-08-22), so a task queued from inside a task runs microseconds later
// in the SAME drain - measured on this rig 2026-07-31, 6 to 237 us, the
// WorldWatch heartbeat freeze (WorldWatch.cpp's heartbeat comment). The
// requeue was therefore always a same-frame spin: the old build's Save
// press burned all 600 iterations in one drain and hung the game for the
// ~8 s its 600 USVFS exists() calls cost ("produced nothing for eight
// seconds"), and the moment the wait stopped counting under pause
// (`800402b`), the same spin had no exit at all and froze the game until
// killed. No design that re-queues from inside the drain can wait; the
// waiting lives on a thread that sleeps, and the queue only ever gets
// single, non-requeueing tasks.
namespace OS::FaceWait {

    enum class Step {
        kWait,    // nothing decided; sleep another slice
        kPatch,   // answer arrived and the jslot exists: patch the face in
        kGiveUp,  // the unpaused budget is spent: log and stop, faceless
    };

    // Seconds of UNPAUSED waiting before the face half gives up. Wall-clock,
    // not iterations: the number means the same thing under every queue and
    // frame-rate. The field-measured save is 1.8 s; this is a generous
    // multiple, and running out is still just a faceless profile with one
    // warn line, the posture every missing block has.
    inline constexpr double kBudgetSeconds = 30.0;

    // Seconds of UNPAUSED waiting for the MESH half, counted only once the
    // jslot has landed.
    //
    // ⚠⚠ THE TWO HALVES DO NOT LAND TOGETHER, so one read the moment the
    // jslot appears is an early reading and not a verdict (the scar
    // a-not-drawn-reading-during-an-attach-is-not-a-verdict, in its export
    // clothes). SaveExternalCharacter writes the jslot inside the native
    // call and then QUEUES the mesh and the tint on an SKSE task, so the
    // file that proves the save is on disk while the file that proves the
    // head is still in a queue. The old line read once and blamed
    // RaceMenu's bEnableHeadExport, which was measured ON at the time.
    inline constexpr double kMeshBudgetSeconds = 6.0;

    struct Budget {
        double unpausedSeconds = 0.0;
    };

    // One slice of the wait, evaluated after sleeping it.
    //
    // ⚠ A PAUSED SLICE COSTS NOTHING, the `800402b` rule kept: the Papyrus
    // VM does not run while a menu pauses the game, so time spent inside
    // the editor is time SaveCharacter could never use, and a stay of any
    // length must not spend the budget. Under this shape that rule is
    // finally safe: an unspent budget here means the thread sleeps another
    // 250 ms, not that the game loop is held hostage.
    [[nodiscard]] inline Step Advance(Budget& a_budget, bool a_answered,
                                      bool a_fileExists, bool a_paused,
                                      double a_sliceSeconds,
                                      double a_capSeconds = kBudgetSeconds) {
        if (a_answered && a_fileExists) {
            return Step::kPatch;
        }
        if (a_paused) {
            return Step::kWait;
        }
        a_budget.unpausedSeconds += a_sliceSeconds;
        return a_budget.unpausedSeconds >= a_capSeconds ? Step::kGiveUp
                                                        : Step::kWait;
    }

}  // namespace OS::FaceWait
