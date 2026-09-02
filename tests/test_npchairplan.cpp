// Pure-logic tests for the follower hair-style reconcile (NpcHairPlan.h). No
// engine, no RE:: types - just the decision OutfitSession's follower refresh
// reduces to when it asks "what, if anything, should change about this
// actor's hair right now".
//
// The case that carries the feature is kApply with no current style: that is a
// follower loading from a save with a style NpcHair has no in-memory record
// of, because its captures are per-session and RevertCallback clears them.
//
// The case that keeps it affordable is want-is-current -> kNone. Every
// follower refresh runs through this, and an Apply is a cull, an attach and a
// FixSkinInstances over the whole face node.
#include "NpcHairPlan.h"

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
    using namespace OS::NpcHairPlan;

    constexpr std::uint32_t kStyleA = 0x0012C4u;
    constexpr std::uint32_t kStyleB = 0x0034D8u;

    // A resolved, supported style the outfit names.
    const auto styled = [](std::uint32_t a_id) {
        Want w;
        w.empty    = false;
        w.resolved = true;
        w.formID   = a_id;
        return w;
    };

    {  // An actor Fitting Room has no opinion about is never touched, and that
       // holds even when it is wearing a style through us - the not-managed
       // answer is "hands off", not "clean up".
        CHECK(Plan(false, Want{}, 0) == Action::kNone);
        CHECK(Plan(false, Want{}, kStyleA) == Action::kNone);
        CHECK(Plan(false, styled(kStyleA), 0) == Action::kNone);
    }

    {  // Managed, and the outfit names no style: nothing attached means there
       // is nothing to do.
        CHECK(Plan(true, Want{}, 0) == Action::kNone);
    }

    {  // Same, but something IS attached. This is Equipped gear after a styled
       // outfit, and it has to come off.
        CHECK(Plan(true, Want{}, kStyleA) == Action::kRestore);
    }

    {  // THE CROSS-SAVE CASE. The outfit names a style, NpcHair has no record
       // of this actor because the load cleared every capture, so the style
       // has to go back on.
        CHECK(Plan(true, styled(kStyleA), 0) == Action::kApply);
    }

    {  // IDEMPOTENCE, and the reason a push can sit on every follower refresh.
       // Already wearing what the outfit names costs one comparison.
        CHECK(Plan(true, styled(kStyleA), kStyleA) == Action::kNone);
    }

    {  // Wearing a DIFFERENT style than the outfit names. Re-applying is
       // correct: it is how a discarded hover preview reverts, since the
       // preview goes straight to the hair mechanism and never touches the
       // staged outfit.
        CHECK(Plan(true, styled(kStyleB), kStyleA) == Action::kApply);
    }

    {  // The plugin that defined the style has left the load order. Leave the
       // hair alone rather than stripping it - the key still resolves if the
       // plugin comes back, so this is environmental and reversible. Matches
       // what the player path does with an unresolvable hairStyle.
        Want gone;
        gone.empty    = false;
        gone.resolved = false;
        gone.formID   = kStyleA;
        CHECK(Plan(true, gone, 0) == Action::kNone);
        CHECK(Plan(true, gone, kStyleB) == Action::kNone);
    }

    {  // Measured over the engine's eight-bone head-part limit. Unlike a
       // missing plugin this can never succeed, so an actor left wearing some
       // OTHER style is showing something the outfit does not name and is put
       // back to her own.
        Want physics = styled(kStyleA);
        physics.unsupported = true;
        CHECK(Plan(true, physics, kStyleB) == Action::kRestore);
    }

    {  // ...and with nothing attached there is nothing to undo, so the state
       // is stable: one declined attempt per session, then silence. Without
       // this the push would re-attempt a style that cannot work on every
       // single follower refresh.
        Want physics = styled(kStyleA);
        physics.unsupported = true;
        CHECK(Plan(true, physics, 0) == Action::kNone);
    }

    {  // Unsupported outranks want-is-current. A style measured as over the
       // limit never became the current one (Apply declines before it lands),
       // so this combination means a stale record rather than a working style.
        Want physics = styled(kStyleA);
        physics.unsupported = true;
        CHECK(Plan(true, physics, kStyleA) == Action::kRestore);
    }

    {  // An empty want is not a resolution failure. The two are distinct
       // inputs and only the first one restores.
        Want none;
        none.empty      = true;
        none.resolved   = false;
        CHECK(Plan(true, none, kStyleA) == Action::kRestore);
    }

    // ---- The admission test: can the engine actually bind this mesh? -------
    //
    // ⚠ REGRESSION, and it is a SECOND fault rather than a miss in the first.
    // KSSMP_Ominous_Elf reached a follower's head on 2026-08-01 and rendered
    // stretched, having passed a gate that only counted bones. Measured off the
    // nif afterwards: its skin binds NPC Head, NPC Spine2 and six chain bones
    // named 'Ominous L1'..'R3', which is seven or eight in total and therefore
    // under the ceiling. None of the six exist on skeleton_female.nif (checked
    // against both XPMSSE at 647 nodes and UBE at 761, zero matches), so
    // FixSkinInstances resolves them to nothing, leaves the mesh bound to the
    // nif's own bones, and those have no world transform.
    //
    // So bone COUNT and bone RESOLVABILITY are different questions and the
    // count alone was never sufficient.

    {  // Ordinary vanilla-style hair: few bones, all of them real.
        CHECK(JudgeBinding(2, 0) == Admission::kAdmit);
        CHECK(JudgeBinding(4, 0) == Admission::kAdmit);
    }

    {  // Exactly at the ceiling still binds. The engine's block holds eight.
        CHECK(JudgeBinding(8, 0) == Admission::kAdmit);
    }

    {  // THE CASE THAT SHIPPED BROKEN. Under the ceiling, but carrying bones
       // her skeleton does not have.
        CHECK(JudgeBinding(7, 6) == Admission::kUnboundBone);
        CHECK(JudgeBinding(8, 1) == Admission::kUnboundBone);
    }

    {  // One unresolvable bone is enough. A partially bound mesh still
       // stretches, so there is no "mostly fine" here.
        CHECK(JudgeBinding(2, 1) == Admission::kUnboundBone);
    }

    {  // Nine is the first count that reads past the end of the eight-entry
       // override block, which is the ORIGINAL fault and still a real one.
        CHECK(JudgeBinding(9, 0) == Admission::kTooManyBones);
        CHECK(JudgeBinding(17, 0) == Admission::kTooManyBones);
    }

    {  // Both faults at once, which is every long-chain physics hair. The
       // count is reported, because an out-of-bounds read is the more severe
       // of the two and is worth naming when it is present.
        CHECK(JudgeBinding(11, 4) == Admission::kTooManyBones);
    }

    // ---- The partition rule: does the bone map name a bone that exists? ----
    //
    // ⚠⚠ THIS RULE WAS MEASURED WRONG AND NARROWED ON 2026-08-21. It used to
    // refuse any single partition whose bone map was SHORTER than the skin's
    // bone list, on the theory that a vertex would then index past the end of
    // the map. The index spaces were backwards: a vertex's bone index bytes are
    // GLOBAL indices into the skin's bone list, not local indices into the map,
    // so a short map cannot be overflowed by anything.
    //
    // The measurement that settled it (tools/nif_skin_partition.py,
    // docs/re/skin-partition-bone-map.md): vanilla femalebody_1.nif carries a
    // SIX entry map on partition 0, that partition's OWN vertices reference
    // bone index 23, and the skin lists 30 bones. Under the old reading that
    // body could not draw. It draws on every character in the game.
    //
    // The cost of the old rule was 40 of the 43 shapes in Modular HDT-SMP
    // Hairstyles refused against ZERO with an out of range entry, which is why
    // SMP hair could not be worn by a follower at all.

    {  // A full map over the skin's bones is fine, and always was.
        CHECK(!JudgePartition(1, 2, 1, 2));   // identity [0,1]
        CHECK(!JudgePartition(1, 4, 3, 4));   // identity [0..3] (Galactic)
    }

    {  // No partition data at all judges fine - there is nothing to check,
       // and the unbound/count rules still stand on their own.
        CHECK(!JudgePartition(0, 0, 0, 2));
    }

    {  // Multiple partitions are the legitimate pre-SSE layout, where subset
       // maps are the whole point. Not the observed defect; leave them alone.
        CHECK(!JudgePartition(2, 1, 0, 2));
        CHECK(!JudgePartition(3, 2, 1, 4));
    }

    {  // ⚠⚠ A SHORT MAP IS LEGAL, ASSERTED SO THE OLD RULE CANNOT COME BACK
       // QUIETLY. Each of these was refused before 2026-08-21 and each is a
       // shape measured on a mesh that renders correctly.
        CHECK(!JudgePartition(1, 1, 1, 2));    // map [1] of 2 (Butterfly078)
        CHECK(!JudgePartition(1, 1, 0, 2));    // map [0] of 2 (CroftBand)
        CHECK(!JudgePartition(1, 3, 2, 4));    // any shortfall
        CHECK(!JudgePartition(1, 16, 18, 19)); // AStraightLong_FClip, EVG SMP
        CHECK(!JudgePartition(1, 7, 12, 13));  // AMiddlePartLong_FClip
        CHECK(!JudgePartition(1, 1, 6, 7));    // an EVG shape rigid on one bone
        CHECK(!JudgePartition(1, 6, 23, 30));  // vanilla femalebody_1 partition 0
    }

    {  // A map entry naming a bone slot the skin does not have is malformed on
       // its face, whatever the map length. This is the whole rule now.
        CHECK(JudgePartition(1, 2, 2, 2));    // entry 2 in a two-bone skin
        CHECK(JudgePartition(1, 1, 5, 3));    // entry 5 in a three-bone skin
        CHECK(JudgePartition(1, 16, 19, 19)); // one past the end of the list
    }

    {  // Severity when faults stack: the count overrun can crash and is
       // reported first; the partition defect outranks a mere unbound bone.
        CHECK(JudgeBinding(11, 0, true) == Admission::kTooManyBones);
        CHECK(JudgeBinding(2, 1, true) == Admission::kBadPartition);
        CHECK(JudgeBinding(2, 0, true) == Admission::kBadPartition);
        CHECK(JudgeBinding(2, 1, false) == Admission::kUnboundBone);
        CHECK(JudgeBinding(2, 0, false) == Admission::kAdmit);
    }

    // OS-99: the fourth verdict. An SMP-capable mesh with FSMP live is
    // admitted for FSMP's own skinning path, and the two BINDING gates are
    // deliberately not consulted: unbound strand bones are the bones FSMP
    // exists to merge, and long chains blow the count by construction. The
    // PARTITION gate stays consulted (Task 6.5): FSMP repairs bone bindings,
    // never partition maps, so a defective map corrupts the render on FSMP's
    // path exactly as on the engine's. Either liveness input false falls
    // through to JudgeBinding unchanged.
    {
        // Both true: admits readings that fail either binding gate.
        CHECK(JudgeStyle(17, 10, false, true, true) == Admission::kAdmitSmp);
        CHECK(JudgeStyle(8, 6, false, true, true) == Admission::kAdmitSmp);
        CHECK(JudgeStyle(2, 0, false, true, true) == Admission::kAdmitSmp);

        // ⚠ NOT the partition gate (Task 6.5). A defective map means the
        // verdict is JudgeBinding's, in JudgeBinding's severity order, even
        // with SMP capable and FSMP live.
        CHECK(JudgeStyle(2, 0, true, true, true) == Admission::kBadPartition);
        CHECK(JudgeStyle(17, 0, true, true, true) == Admission::kTooManyBones);
        CHECK(JudgeStyle(17, 10, true, true, true) == Admission::kTooManyBones);

        // FSMP absent: identical to JudgeBinding, SMP-capable or not.
        CHECK(JudgeStyle(17, 10, true, true, false) == Admission::kTooManyBones);
        CHECK(JudgeStyle(8, 6, false, true, false) == Admission::kUnboundBone);
        CHECK(JudgeStyle(2, 0, false, true, false) == Admission::kAdmit);

        // Not SMP-capable: identical to JudgeBinding even with FSMP live.
        CHECK(JudgeStyle(9, 0, false, false, true) == Admission::kTooManyBones);
        CHECK(JudgeStyle(2, 1, false, false, true) == Admission::kUnboundBone);
        CHECK(JudgeStyle(2, 0, true, false, true) == Admission::kBadPartition);
        CHECK(JudgeStyle(2, 0, false, false, true) == Admission::kAdmit);

        // Neither: the existing table, spot-checked.
        CHECK(JudgeStyle(11, 4, false, false, false) == Admission::kTooManyBones);
        CHECK(JudgeStyle(2, 0, false, false, false) == Admission::kAdmit);
    }

    if (g_failures == 0) {
        std::printf("all NpcHairPlan tests passed\n");
    }
    return g_failures;
}
