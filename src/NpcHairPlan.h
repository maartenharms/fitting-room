#pragma once

#include <cstdint>

// The pure decision behind a follower's hair STYLE push: given what her outfit
// names and what NpcHair currently has attached, what should change right now.
// No engine coupling, so the rule is unit-tested without RE:: types (see
// tests/test_npchairplan.cpp) the same way NpcResolve.h and RefreshGate.h are.
//
// WHY THIS EXISTS AT ALL. NpcHair's captures are per-session: they name
// scenegraph nodes, and RevertCallback clears them before every load because
// those nodes belong to a character being torn down. Outfit::hairStyle is the
// persistent half, in LIBR since v7 and riding inside every NPCO entry. So a
// follower reloads with her style recorded and nothing wearing it, and
// something has to close that gap. This is the rule that decides when.
//
// WHY IT IS A RECONCILE RATHER THAN AN APPLY. The push sits on the follower
// refresh seam, which is also every staging change, the OBody-readiness sweep
// and the post-load walk, so it runs far more often than a style actually
// changes. An Apply is a cull, an attach and a FixSkinInstances over her whole
// face node, and the engine worker behind that rewrites the skin binding of
// every geometry on her head. Answering kNone when she is already wearing what
// the outfit names is what makes the seam affordable.
namespace OS::NpcHairPlan {

    enum class Action : std::uint8_t {
        kNone,     // she is already right, or she is not ours to touch
        kApply,    // attach what the outfit names
        kRestore,  // put her own hair back
    };

    // What the outfit asks for, already looked up by the caller. Split into
    // "names nothing" and "names something that did not resolve" on purpose:
    // they are different situations and only the first one undresses her.
    struct Want {
        bool empty{ true };         // the outfit names no hair style at all
        bool resolved{ false };     // ...and the key found a live BGSHeadPart
        bool unsupported{ false };  // ...which NpcHair has already declined
        std::uint32_t formID{ 0 };  // that part's form id, 0 when there is none
    };

    // a_currentFormID is what NpcHair has attached to her now, 0 meaning her
    // own hair. Deliberately a form id rather than a pointer, so the whole
    // rule stays engine-free and the comparison is the one thing being tested.
    [[nodiscard]] inline Action Plan(bool a_manages, const Want& a_want,
                                     std::uint32_t a_currentFormID) {
        // Not ours. Note this is hands-off rather than clean-up: an actor
        // Fitting Room does not manage may still be wearing something we
        // attached before the user un-assigned her, and stripping it from a
        // path that has no opinion about her would be a side effect nobody
        // asked for. The same posture HairStateForNpc's manages flag takes.
        if (!a_manages) {
            return Action::kNone;
        }

        // Equipped gear, or an outfit that simply has no hair style. Anything
        // of ours has to come off; nothing of ours means nothing to do.
        if (a_want.empty) {
            return a_currentFormID != 0 ? Action::kRestore : Action::kNone;
        }

        // The plugin that defined the style has left the load order. LEAVE HER
        // ALONE rather than restoring: this is reversible - the key resolves
        // again the moment the plugin comes back - and the outfit still names
        // it, so undressing her would discard a choice that is merely
        // unreachable today. Same answer the player path gives, which warns and
        // keeps the current hair.
        if (!a_want.resolved) {
            return Action::kNone;
        }

        // Measured as over the engine's eight-bone head-part limit, which is
        // NOT reversible the way a missing plugin is: no future load order
        // makes physics hair bindable through this route. So if she is wearing
        // some OTHER style of ours, she is showing something the outfit does
        // not name and it comes off. With nothing attached there is nothing to
        // undo, and answering kNone there is what stops the push from
        // re-attempting an impossible style on every single refresh - one
        // declined attempt per session, then silence.
        if (a_want.unsupported) {
            return a_currentFormID != 0 ? Action::kRestore : Action::kNone;
        }

        // Already right. This is the answer almost every call gets.
        if (a_want.formID == a_currentFormID) {
            return Action::kNone;
        }

        // Either she is wearing nothing of ours (the cross-save case this
        // module exists for) or she is wearing the wrong one (how a discarded
        // hover preview reverts, since the preview goes straight to the hair
        // mechanism and never touches the staged outfit).
        return Action::kApply;
    }

    // ---- The admission test --------------------------------------------
    //
    // Whether the engine can actually BIND a head part's mesh to this actor,
    // asked after the attach because nothing on a BGSHeadPart predicts how its
    // mesh is rigged, and before the publish because FixSkinInstances is the
    // step that cannot survive a wrong answer.
    //
    // ⚠ TWO INDEPENDENT FAULTS LIVE HERE and conflating them is what shipped a
    // broken build. Counting bones catches one of them. It does not catch the
    // other, and for a while it looked like it did, because long chains trip
    // the count first.

    // ⚠ EIGHT, AND THE ENGINE'S OWN GUARD SAYS NINE. Do not "correct" it back.
    // The worker behind FixSkinInstances (AE 0x140432280) reads a per-geometry
    // bone-name override at `block + (bone + 4) * 8`, guarded by nothing but
    // `if (extraData == 0 || 8 < bone)`. Trusting that guard admits index 8,
    // one past the end of an eight-entry block, so a part with NINE bones is
    // the first that reads garbage. A gate written to the guard's number was
    // built, deployed, and crashed on the first nine-bone style.
    inline constexpr std::uint32_t kMaxHeadPartBones = 8;

    enum class Admission : std::uint8_t {
        kAdmit,
        kTooManyBones,  // reads past the end of the override block
        kUnboundBone,   // names a bone that is not on this actor's skeleton
        kBadPartition,  // bone map cannot address the bones the skin carries
        kAdmitSmp,      // FSMP-capable mesh, FSMP live: its path, not ours
    };

    // ⚠ THE THIRD FAULT: a bone map that names a bone the skin does not carry.
    // A map entry is an index into the skin instance's bone list, so an entry
    // at or past that list's length addresses nothing and the vertices weighted
    // against it sample whatever the previous draw left behind, which renders
    // as a giant fan off the head.
    //
    // ⚠⚠ A SHORT MAP IS NOT A FAULT, AND THIS GATE USED TO SAY IT WAS. Until
    // 2026-08-21 a single partition whose map was SHORTER than the skin's bone
    // list was refused outright, on the theory that a vertex could then index
    // past the end of the map. That theory had the index spaces backwards, and
    // three measurements killed it (tools/nif_skin_partition.py, written for
    // this, and docs/re/skin-partition-bone-map.md):
    //
    //   * A vertex's bone index bytes are GLOBAL indices into the skin's bone
    //     list, not local indices into the partition's map. Vanilla
    //     femalebody_1.nif proves it: partition 0 carries a SIX entry map, its
    //     own vertices reference bone index 23, and the skin lists 30. Under
    //     local indexing that mesh could not draw, and it draws on every body
    //     in the game.
    //   * The map is therefore a LIST OF THE BONES A PARTITION USES, so a map
    //     shorter than the bone list just means the partition does not use them
    //     all. Every legitimate mesh measured is shaped that way.
    //   * The cost of getting it backwards was 40 of the 43 shapes in Modular
    //     HDT-SMP Hairstyles refused, against ZERO with an out of range entry.
    //     That is the whole reason SMP hair could not be worn by a follower.
    //
    // ⚠ Multiple partitions are still left alone: subset maps are the point of
    // that layout and none has been observed broken.
    //
    // The player path never trips this in any case: the engine's full head
    // build regenerates partitions, while the follower attach renders the
    // nif's as shipped.
    [[nodiscard]] inline bool JudgePartition(std::uint32_t a_partitions,
                                             std::uint32_t a_mapLength,
                                             std::uint32_t a_maxMapEntry,
                                             std::uint32_t a_skinBones) {
        if (a_partitions != 1) {
            return false;  // nothing to judge, or legitimately partitioned
        }
        return a_mapLength > 0 && a_maxMapEntry >= a_skinBones;
    }

    // a_unboundBones counts skin bones whose name does not resolve against the
    // actor's skeleton root, which is the same lookup FixSkinInstances itself
    // performs.
    //
    // ⚠ WHY UNBOUND MATTERS EVEN AT TWO BONES. When that lookup returns null
    // the engine does NOT fall back to anything: it leaves the entry pointing
    // at the nif's own bone node, which has no world transform under her
    // skeleton, and the mesh stretches away to infinity. One unresolvable bone
    // is enough to do it, so there is no tolerance to spend here.
    //
    // Physics hair is the case in the field. Its strand chains are bones that
    // live only inside the hair nif (KSSMP_Ominous_Elf carries six, named
    // 'Ominous L1' through 'R3', against zero matches on either skeleton this
    // load order ships), and hdtSMP drives them at runtime rather than the
    // skeleton providing them. A long chain also blows the count, which is why
    // the count alone appeared sufficient until a six-chain style got through.
    [[nodiscard]] inline Admission JudgeBinding(std::uint32_t a_maxBones,
                                                std::uint32_t a_unboundBones,
                                                bool a_partitionDefect = false) {
        // Severity order when faults stack. The count overrun can take the
        // process down, so it is named first; a partition defect corrupts the
        // render outright and outranks a mere unbound bone.
        if (a_maxBones > kMaxHeadPartBones) {
            return Admission::kTooManyBones;
        }
        if (a_partitionDefect) {
            return Admission::kBadPartition;
        }
        if (a_unboundBones > 0) {
            return Admission::kUnboundBone;
        }
        return Admission::kAdmit;
    }

    // OS-99. A mesh that carries FSMP's own physics marker, on an install
    // where a known FSMP build is loaded, is admitted for FSMP's head-part
    // path: one engine call it detours (SkinSingleGeometry) makes it merge
    // the strand bones and build the system itself. The two BINDING gates
    // above judge the ENGINE's skinning path and are deliberately not
    // consulted: unbound strand bones are the bones FSMP exists to merge,
    // and long chains blow the count by construction, which is why this
    // feature was dead until 2026-08-01.
    //
    // ⚠ THE PARTITION GATE STAYS CONSULTED (Task 6.5). FSMP repairs bone
    // BINDINGS, never partition maps: its skinning rebuilds which bones
    // drive which vertices, but the renderer still uploads one matrix per
    // partition-map entry, so a map that cannot address the skin's bones
    // corrupts the render on FSMP's path exactly as on the engine's. A
    // defective map therefore falls through to JudgeBinding, whose severity
    // order names the fault.
    //
    // ⚠ BOTH liveness inputs, not either. smpCapable without FSMP means the
    // engine path would run and stretch the strand bones (that is the
    // measured KSSMP_Ominous_Elf failure). fsmpLive without the marker means
    // FSMP has no physics file to build from and the mesh is an ordinary
    // head part.
    [[nodiscard]] inline Admission JudgeStyle(std::uint32_t a_maxBones,
                                              std::uint32_t a_unboundBones,
                                              bool a_partitionDefect,
                                              bool a_smpCapable,
                                              bool a_fsmpLive) {
        if (a_smpCapable && a_fsmpLive && !a_partitionDefect) {
            return Admission::kAdmitSmp;
        }
        return JudgeBinding(a_maxBones, a_unboundBones, a_partitionDefect);
    }

}  // namespace OS::NpcHairPlan
