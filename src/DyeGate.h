#pragma once

#include <cstddef>

namespace OS::DyeGate {

    // The pure half of "when has a dye actually been painted". Split out of
    // OutfitDye.cpp for the same reason RefreshGate and EditorGate are split
    // out of their callers: the decision is testable and the engine walk around
    // it is not.
    //
    // ---- Why a null partClone is TWO different answers ----------------------
    //
    // BipedPost.h records the field discipline this rests on (2026-07-11
    // invisible-torso bug): 15500 writes objects[bit].item, .addon and .part
    // SYNCHRONOUSLY on the worn-pass call stack, and the 3D attach that fills
    // .partClone runs DEFERRED off BSTaskPool. Every dump taken at restore time
    // showed partClone == 0x0. BipedPost::QueueNodeCull exists solely because of
    // it.
    //
    // So a dyed slot with no partClone means one of two completely different
    // things, and the painter has to tell them apart or it reports a silent
    // failure as a normal state:
    //   * .addon and .part both empty  -> nothing is staged in this slot. The
    //     user dyed a slot they are not wearing. Nothing to paint, ever, and no
    //     amount of waiting changes it.
    //   * .addon or .part present      -> geometry IS staged and its clone has
    //     not landed yet. Painting now silently misses the slot; painting again
    //     in a frame or two catches it.
    //
    // The second case is B2 from the 2026-07-31 review: Repaint read partClone
    // synchronously right after UpdateEquipment, so every slot whose model came
    // off disk rather than the cache was skipped with no diagnostic at all.
    // ---- and why there are TWO more answers (OS-111, OS-116) ----------------
    //
    // Two sessions found two separate causes of one symptom, independently, and
    // that is the part worth keeping: "staged" was one boolean covering at least
    // three situations. Both causes leave partClone permanently null, both used
    // to read as kClonePending, and kClonePending promises "painting again in a
    // frame or two catches it". So both burned the whole retry budget every
    // rebuild and ended on the warning about a mesh that never finishes
    // attaching, which describes a real and serious failure, for two entirely
    // benign causes.
    //
    // OS-111, measured 2026-08-02. Biped object 9 is the shield's slot AND the
    // slot the engine stages the off-hand weapon and the torch into. With a
    // colour stored on the shield and a sword in the left hand, the walk saw
    // objects[9] with .item and .addon written and .partClone permanently null,
    // because a weapon's 3D arrives through the part-3D loader rather than the
    // armour attach. Something can be staged that this walk will never own, and
    // the caller has to say which it is.
    //
    //   * staged, no clone, the occupant is not armour -> not ours to paint.
    //     Nothing to wait for and nothing to retry.
    //
    // OS-116, measured 2026-08-03 in the log that confirmed OS-112. A garment
    // covering several biped slots stages its model on every slot it covers, and
    // the engine does NOT always clone it onto each one: a cuirass covering 32
    // and 34 was measured attaching a single clone on the chest bit and leaving
    // the forearm bit staged with no clone at all, permanently. That slot is not
    // waiting for anything. Its armature is on screen, rendered through the
    // lower slot's clone, and no number of deferred attempts will change what it
    // looks like.
    //
    //   * staged, no clone, a LOWER slot owns this armature -> already rendered
    //     by that slot. Nothing to wait for and nothing to retry.
    //
    // ⚠ The two are NOT redundant and must not collapse into one flag. One asks
    // WHAT is in the slot and the other asks WHERE its geometry went, they are
    // tested in different places (below), and the log wants different words for
    // each. If a third cause turns up, it is this shape again.
    enum class SlotPaintState {
        kNoDye,            // no channel set on this slot
        kNothingWorn,      // dyed, but nothing is staged here
        kClonePending,     // dyed, geometry staged, 3D attach has not run yet
        kClonedElsewhere,  // dyed and staged, but a lower slot carries the clone
        kForeignOccupant,  // dyed, but what is in the slot is not ours to paint
        kReady,            // dyed and the clone is attached
    };

    // a_staged is "objects[bit].addon != nullptr || objects[bit].part !=
    // nullptr". Both are read because the two are written by different arms of
    // 15500 (an armature-backed piece fills .addon, a plain TESModel piece fills
    // .part) and either one on its own proves the slot is occupied.
    // a_foreign is "something is in this slot and it is not ours to paint". It
    // is FALSE for an empty slot, deliberately: an empty shield slot is
    // kNothingWorn and always was, and folding emptiness into foreignness would
    // change the answer for every character not carrying a shield.
    // a_ownsClone is "no LOWER slot shares this slot's source model", which is
    // what OS-112's clone grouping already answers. True for an ordinary slot,
    // so the only slots it changes are the extra ones a multi-slot garment
    // covers.
    //
    // ⚠ THE TWO ARE TESTED IN DIFFERENT PLACES AND THE PLACES ARE THE POINT.
    // Foreign is tested BEFORE the clone: if an off-hand weapon ever did present
    // a partClone here, painting it would still be wrong, so the ownership
    // question outranks the readiness one. ownsClone is tested only AFTER
    // hasClone came back false, because a slot carrying its own clone is kReady
    // whatever the grouping says about it.
    //
    // ⚠ NO DEFAULT ARGUMENTS, the same rule StillDirty and ClassifyStagedUpdate
    // carry. Both dimensions arrived because the pending count was wrong without
    // them, and a defaulted parameter is exactly how the NEXT one gets
    // forgotten: every existing call keeps compiling while the new input
    // silently reads false. Two sessions adding one each, a day apart and
    // neither knowing about the other, is the argument for that rule rather than
    // against it.
    [[nodiscard]] constexpr SlotPaintState ClassifySlot(bool a_dyeSet, bool a_hasClone,
                                                        bool a_staged, bool a_foreign,
                                                        bool a_ownsClone) {
        if (!a_dyeSet) {
            return SlotPaintState::kNoDye;
        }
        if (a_foreign) {
            return SlotPaintState::kForeignOccupant;
        }
        if (a_hasClone) {
            return SlotPaintState::kReady;
        }
        if (!a_staged) {
            return SlotPaintState::kNothingWorn;
        }
        return a_ownsClone ? SlotPaintState::kClonePending
                           : SlotPaintState::kClonedElsewhere;
    }

    // ---- the retry chain, and why counting posts is not waiting (OS-110) ----
    //
    // ⚠ THIS COMMENT USED TO SAY THE COUNT BELOW WAS A FRAME BUDGET. IT IS NOT,
    // AND THE OLD TEXT IS WHY NOBODY NOTICED. It read: "a frame budget, not a
    // timeout: SKSE's task interface drains once per frame on the main thread,
    // so this is roughly 12 frames, 200 ms at 60 fps... the worst case is a
    // cold-cache nif read, which is what the 12 covers."
    //
    // Measured 2026-08-02: twelve complete Repaint passes, every one logging the
    // same pending slot, all stamped inside TWO MILLISECONDS, then the
    // budget-exhausted warning. A cold-cache nif read is not covered by two
    // milliseconds. PostRepaintAttempt posts its next link from inside the
    // previous link's task body, and the queue drains links added during a drain
    // within that same drain, so the chain never yields a frame and B2 waits for
    // nothing at all. Dye works in the field only because the clone is normally
    // attached already.
    //
    // The fix does not assume which way the queue behaves, because assuming is
    // what produced the wrong comment above. A link spends a WALK only when real
    // time has actually moved, and the number of POSTS is bounded separately so
    // that a queue draining in one pass cannot spin. Both semantics come out
    // right:
    //
    //   * yielding a frame per post -> the gap is a frame, every post walks,
    //     twelve walks span twelve frames, exactly what the old text claimed.
    //   * draining in one pass       -> the gap stays at zero, the posts bound
    //     runs out, ONE walk happens instead of twelve, and the chain says so in
    //     the log rather than looking like it waited.

    // Real painting attempts one biped rebuild is worth. Bounded rather than
    // "retry until it lands" on purpose: a slot can stay kClonePending forever
    // when a mesh fails to load at all, and an unbounded chain would walk the
    // whole biped for the rest of the session.
    inline constexpr unsigned kRepaintAttempts = 12;

    // Hard bound on links posted, whether or not they walk. Only reachable when
    // the queue drains in one pass, where it is what stops the spin. Sized so
    // that exhausting it is unmistakably cheap: these are no-op lambdas.
    inline constexpr unsigned kRepaintPosts = 256;

    // The minimum real gap between two walks, in milliseconds.
    //
    // Four rather than a frame time. A same-pass drain runs links microseconds
    // apart, so anything above zero defeats it, and staying well under a frame
    // means the gate opens on every frame at any sane rate rather than skipping
    // every other one at high refresh.
    inline constexpr unsigned kRepaintGapMs = 4;

    // Spend a walk only when real time moved and walks remain.
    [[nodiscard]] constexpr bool ShouldWalkNow(bool a_gapElapsed, unsigned a_walksLeft) {
        return a_gapElapsed && a_walksLeft > 0;
    }

    // Post another link only while something is still waiting AND both budgets
    // have room. Counts are the values AFTER this link has taken its share, so
    // zero means spent.
    [[nodiscard]] constexpr bool ShouldPostAgain(std::size_t a_pending, unsigned a_walksLeft,
                                                 unsigned a_postsLeft) {
        return a_pending > 0 && a_walksLeft > 0 && a_postsLeft > 0;
    }

    // The chain ran out of posts while it still had walks it never got to spend.
    // That is the OS-110 signature and nothing else produces it: the gate never
    // opened, so no real time passed across the whole chain.
    [[nodiscard]] constexpr bool DrainedWithoutWaiting(unsigned a_walksLeft,
                                                       unsigned a_postsLeft) {
        return a_postsLeft == 0 && a_walksLeft > 0;
    }

    // ---- what one swap record's teardown may do -----------------------------
    //
    // Restore, which runs before every repaint, and Clear, which runs on the
    // save load, both route through this. That is the entire point of it: they
    // used to decide separately and they disagreed, and the disagreement was
    // invisible until a garment came back from a reload wearing skin.
    enum class TeardownAction {
        kRestore,      // put the original material and flags back, then release
        kReleaseOnly,  // release our reference and touch nothing in the scene
    };

    // a_propStillOurs is "the geometry's effect property is STILL the one that
    // was swapped". a_attached is "the geometry is still under the actor's 3D
    // root". a_hasOriginal is "a displaced material was actually recorded".
    //
    // ⚠ A SAVE LOAD DOES NOT IMPLY THE 3D IS GONE, and assuming it did is what
    // stranded a tint material on 2026-08-01. Clear released its reference and
    // dropped the record without restoring, because the load "is tearing this
    // 3D down anyway". Reloading into the same character tears nothing down:
    // the property came back still carrying the FacegenTint, the only record
    // that knew to put the real material back was gone, and the feature guard
    // then read that shape as kFaceGenRGBTint and skipped it permanently. So
    // the save-load path has to ASK about attachment exactly like the refresh
    // path does rather than assume the answer.
    //
    // Restoring is the narrow case and must stay narrow. A wrong answer in the
    // release direction leaves a stranded material, which is ugly; a wrong
    // answer in the restore direction writes through freed memory.
    [[nodiscard]] constexpr TeardownAction ClassifyTeardown(bool a_propStillOurs,
                                                            bool a_attached,
                                                            bool a_hasOriginal) {
        return a_propStillOurs && a_attached && a_hasOriginal
                   ? TeardownAction::kRestore
                   : TeardownAction::kReleaseOnly;
    }

    // Is the actor's second biped holder worth a walk of its own?
    //
    // ⚠ THE IDENTITY TEST IS THE WHOLE POINT, and it guards a measured hazard
    // rather than a hypothetical one. `TESObjectREFR::GetBiped1` ignores its
    // flag in the base implementation and returns `GetBiped2()`, so asking for
    // the first-person biped can hand back the third-person one. Walking that a
    // second time would paint every shape, then revisit the same geometry and
    // count it again, on any actor that has no separate first-person biped.
    //
    // FPPROBE measured `distinct=true` on the player, which is what makes
    // first-person dye reachable at all. This exists because the player is not
    // the only actor Repaint runs on.
    //
    // Deliberately takes plain pointers: the decision is about object identity
    // and nothing else, so it stays out of the engine types and stays testable.
    [[nodiscard]] constexpr bool ShouldWalkSecondBiped(const void* a_first,
                                                       const void* a_second) {
        return a_first != nullptr && a_second != nullptr && a_first != a_second;
    }

}  // namespace OS::DyeGate
