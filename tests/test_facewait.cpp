#include "FaceWait.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::FaceWait;
    constexpr double kSlice = 0.25;

    // The budget is wall-clock and it is the shipped number. An iteration
    // count died here: 600 task-requeue waits turned out to be 600 SAME-DRAIN
    // iterations, not 600 frames, and the game hung for exactly as long as
    // they took.
    CHECK(kBudgetSeconds == 30.0);

    {  // ⚠ THE 2026-08-22 01:25 FREEZE STATE, exactly as the log left it:
       // editor open (paused), answer pending, jslot missing. However long
       // the stay, the verdict is kWait and the budget stays untouched - a
       // slice under pause is time the VM could never have used. Under the
       // old requeue this exact state was an infinite same-drain spin; here
       // it is a thread sleeping 250 ms at a time.
        Budget b;
        for (int i = 0; i < 100000; ++i) {
            CHECK(Advance(b, false, false, true, kSlice) == Step::kWait);
        }
        CHECK(b.unpausedSeconds == 0.0);
    }

    {  // The 00:41 shape: unpaused, the answer never comes (the old build
       // burned its whole wait inside the pause and then saved faceless).
       // Now it is a clean give-up after exactly the budget's worth of
       // unpaused slices, and not one slice sooner.
        Budget    b;
        const int cap = static_cast<int>(kBudgetSeconds / kSlice);  // 120
        for (int i = 0; i < cap - 1; ++i) {
            CHECK(Advance(b, false, false, false, kSlice) == Step::kWait);
        }
        CHECK(Advance(b, false, false, false, kSlice) == Step::kGiveUp);
    }

    {  // The healthy save: ~1.8 s of unpaused VM time (field-measured), then
       // answer and file together. Lands as kPatch with most of the budget
       // unspent.
        Budget b;
        for (int i = 0; i < 8; ++i) {  // 2 s of slices
            CHECK(Advance(b, false, false, false, kSlice) == Step::kWait);
        }
        CHECK(Advance(b, true, true, false, kSlice) == Step::kPatch);
        CHECK(b.unpausedSeconds == 2.0);
    }

    {  // The answer can arrive before the file is visible (the jslot rides
       // RaceMenu's own write and MO2's VFS): answer alone keeps waiting,
       // and the pair lands it. The reverse order holds too - a stale file
       // alone (the stale-YES the probe closed) is not a landing.
        Budget b;
        CHECK(Advance(b, true, false, false, kSlice) == Step::kWait);
        CHECK(Advance(b, false, true, false, kSlice) == Step::kWait);
        CHECK(Advance(b, true, true, false, kSlice) == Step::kPatch);
    }

    {  // A landed pair wins even on the slice the budget would have died on:
       // the checks come before the spend.
        Budget b;
        b.unpausedSeconds = kBudgetSeconds - kSlice;
        CHECK(Advance(b, true, true, false, kSlice) == Step::kPatch);
    }

    {  // Pause flapping: the editor opening and closing around the save only
       // charges the unpaused slices. Ten paused, four unpaused, ten paused:
       // one second spent.
        Budget b;
        for (int i = 0; i < 10; ++i) {
            CHECK(Advance(b, false, false, true, kSlice) == Step::kWait);
        }
        for (int i = 0; i < 4; ++i) {
            CHECK(Advance(b, false, false, false, kSlice) == Step::kWait);
        }
        for (int i = 0; i < 10; ++i) {
            CHECK(Advance(b, false, false, true, kSlice) == Step::kWait);
        }
        CHECK(b.unpausedSeconds == 1.0);
    }

    // The MESH half, which is the same arithmetic under a shorter cap. It
    // exists because the two halves of an export do not land together: the
    // jslot is written inside the native call and the nif is queued behind
    // it, so the old single read the instant the jslot appeared called a
    // healthy export broken and blamed a setting that was measured ON.
    CHECK(kMeshBudgetSeconds == 6.0);
    CHECK(kMeshBudgetSeconds < kBudgetSeconds);

    {  // The task drains a second or so after the jslot: four waiting slices,
       // then the file, and the dwell ends on a landing rather than a verdict
       // taken too early.
        Budget b;
        for (int i = 0; i < 4; ++i) {
            CHECK(Advance(b, true, false, false, kSlice, kMeshBudgetSeconds) ==
                  Step::kWait);
        }
        CHECK(Advance(b, true, true, false, kSlice, kMeshBudgetSeconds) ==
              Step::kPatch);
        CHECK(b.unpausedSeconds == 1.0);
    }

    {  // The mesh never arrives: give up after exactly the mesh budget, not
       // the face budget, so a rig with head export off costs six seconds and
       // not thirty.
        Budget    b;
        const int cap = static_cast<int>(kMeshBudgetSeconds / kSlice);  // 24
        for (int i = 0; i < cap - 1; ++i) {
            CHECK(Advance(b, true, false, false, kSlice, kMeshBudgetSeconds) ==
                  Step::kWait);
        }
        CHECK(Advance(b, true, false, false, kSlice, kMeshBudgetSeconds) ==
              Step::kGiveUp);
    }

    {  // Reading the folder while a menu holds the game still costs nothing
       // here either: the export task cannot drain under pause, so a stay in
       // the editor must not spend the mesh dwell.
        Budget b;
        for (int i = 0; i < 1000; ++i) {
            CHECK(Advance(b, true, false, true, kSlice, kMeshBudgetSeconds) ==
                  Step::kWait);
        }
        CHECK(b.unpausedSeconds == 0.0);
    }

    if (g_failures == 0) {
        std::printf("test_facewait: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
