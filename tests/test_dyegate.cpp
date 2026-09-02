#include "DyeGate.h"

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
    using OS::DyeGate::ClassifySlot;
    using OS::DyeGate::ClassifyTeardown;
    using OS::DyeGate::DrainedWithoutWaiting;
    using OS::DyeGate::kRepaintAttempts;
    using OS::DyeGate::kRepaintGapMs;
    using OS::DyeGate::kRepaintPosts;
    using OS::DyeGate::ShouldPostAgain;
    using OS::DyeGate::ShouldWalkNow;
    using OS::DyeGate::ShouldWalkSecondBiped;
    using OS::DyeGate::SlotPaintState;
    using OS::DyeGate::TeardownAction;

    // ⚠ TWO INDEPENDENT SESSIONS ADDED ONE ARGUMENT EACH, a day apart, for two
    // different causes of one symptom, and the merge kept both. The names below
    // exist because the raw booleans stopped being readable at four: a call
    // reading (true, false, true, true, false) says nothing about which "true"
    // is which, and the two flags have OPPOSITE polarity for the ordinary slot.
    //
    // Fourth argument, "foreign": something is in the slot and it is not ours to
    // paint (OS-111). False for every armour bit except 9.
    constexpr bool kMine    = false;
    constexpr bool kForeign = true;
    // Fifth argument, "ownsClone": no LOWER slot shares this slot's source
    // model (OS-116). True for an ordinary slot, false only on the extra bits a
    // multi-slot garment covers.
    constexpr bool kOwns        = true;
    constexpr bool kSharedBelow = false;

    // ---- the distinction the whole file exists for ------------------------
    // A dyed slot with no partClone is TWO different states, and reading both
    // as "nothing to dye" is exactly the bug this replaces (B2, 2026-07-31):
    // BipedPost.h measured that .addon and .part are written synchronously by
    // 15500 while the 3D attach filling .partClone runs deferred off BSTaskPool,
    // so a slot with staged geometry and no clone is simply EARLY.
    CHECK(ClassifySlot(true, false, true, kMine, kOwns) == SlotPaintState::kClonePending);
    // Nothing staged and nothing cloned: the user dyed a slot they are not
    // wearing. No amount of waiting changes it, so it must NOT read as pending
    // or the retry chain would run its full budget on every rebuild.
    CHECK(ClassifySlot(true, false, false, kMine, kOwns) == SlotPaintState::kNothingWorn);
    // Both fields count. An armature-backed piece fills .addon and a plain
    // TESModel piece fills .part, so the caller ORs them and either alone proves
    // the slot is occupied.
    CHECK(ClassifySlot(true, false, true, kMine, kOwns) == SlotPaintState::kClonePending);

    // ⚠ MEASURED IN THE FIELD 2026-08-03, and it fired 779 times in one session
    // before this dimension existed. A garment covering several biped slots
    // stages its model on every one of them, and the engine does not necessarily
    // clone it onto each: a cuirass on 32 and 34 attached a single clone on the
    // chest bit and left the forearm bit staged with no clone for the whole
    // session. Since OS-112 that forearm bit resolves its colour through the
    // chest bit, so it reads as dyed.
    //
    // It is NOT waiting for anything. Its armature is on screen already, drawn
    // through the lower slot's clone. Reading it as pending held the retry chain
    // open for its full budget on every rebuild and then printed a warning that
    // says a garment is rendering undyed, which is the one line that exists to
    // catch a mesh that genuinely never attaches. 60 of those were false.
    CHECK(ClassifySlot(true, false, true, kMine, kSharedBelow) ==
          SlotPaintState::kClonedElsewhere);
    // ...and the ownership flag only matters while the clone is missing. Nothing
    // staged is still nothing worn, whoever owns the armature.
    CHECK(ClassifySlot(true, false, false, kMine, kSharedBelow) ==
          SlotPaintState::kNothingWorn);

    // The ordinary case, and it must win over the staged flag: once the clone is
    // attached there is nothing left to wait for.
    CHECK(ClassifySlot(true, true, true, kMine, kOwns) == SlotPaintState::kReady);
    CHECK(ClassifySlot(true, true, false, kMine, kOwns) == SlotPaintState::kReady);
    // A slot that DOES have its own clone is ready even when a lower slot shares
    // its source. That is the two-clone helmet, where both copies are attached
    // and both get painted; only the colour is shared, never the readiness.
    CHECK(ClassifySlot(true, true, true, kMine, kSharedBelow) == SlotPaintState::kReady);

    // No dye on the slot is never pending, whatever the geometry is doing. A
    // slot with staged-but-uncloned 3D and no dye must not hold the retry chain
    // open - that is the difference between one deferred pass and 12 of them on
    // a character wearing nothing dyed at all.
    CHECK(ClassifySlot(false, false, true, kMine, kOwns) == SlotPaintState::kNoDye);
    CHECK(ClassifySlot(false, false, false, kMine, kOwns) == SlotPaintState::kNoDye);
    CHECK(ClassifySlot(false, true, true, kMine, kOwns) == SlotPaintState::kNoDye);
    CHECK(ClassifySlot(false, true, false, kMine, kOwns) == SlotPaintState::kNoDye);
    CHECK(ClassifySlot(false, false, true, kMine, kSharedBelow) == SlotPaintState::kNoDye);

    // ---- teardown: the save-load path has to ask the same question ---------
    // 2026-08-01, field-confirmed. Clear() used to release its reference and
    // drop the record WITHOUT putting the material back, on the theory that a
    // save load is always tearing the 3D down. It is not. Reloading into the
    // same character leaves the property alive still carrying the FacegenTint,
    // and with the record gone nothing knows to restore it. The feature guard
    // then reads that shape as kFaceGenRGBTint and skips it for good, and the
    // engine's own attach fills the material's empty facegen texture slots from
    // the actor root, which is why armour came back wearing skin.
    //
    // The log said it in counts: one record cleared, then 'skipped on feature'
    // 1 -> 2 and 'wrote' 1 -> 0, with the stranded shape named.
    CHECK(ClassifyTeardown(true, true, true) == TeardownAction::kRestore);
    // Detached geometry: release only. Writing a material into 3D that is no
    // longer in the scene is work at best and a fault at worst, and this is the
    // case the original Clear() was right about. It must stay right.
    CHECK(ClassifyTeardown(true, false, true) == TeardownAction::kReleaseOnly);
    // The property was reassigned out from under us, which is the rarer of the
    // two staleness modes. Comparing the stale pointer is defined; writing
    // through it is not.
    CHECK(ClassifyTeardown(false, true, true) == TeardownAction::kReleaseOnly);
    // Nothing recorded to put back.
    CHECK(ClassifyTeardown(true, true, false) == TeardownAction::kReleaseOnly);
    // Every remaining combination releases rather than restores. Restoring is
    // the narrow case and it has to stay narrow: this is the direction where a
    // wrong answer writes through freed memory.
    CHECK(ClassifyTeardown(false, false, false) == TeardownAction::kReleaseOnly);
    CHECK(ClassifyTeardown(false, false, true) == TeardownAction::kReleaseOnly);
    CHECK(ClassifyTeardown(false, true, false) == TeardownAction::kReleaseOnly);
    CHECK(ClassifyTeardown(true, false, false) == TeardownAction::kReleaseOnly);

    // ---- foreign occupants, OS-111 -----------------------------------------
    // Biped 9 is the shield's slot and also where the engine stages the off-hand
    // weapon and the torch. Measured 2026-08-02: those stage .item and .addon
    // and NEVER fill .partClone, so without this the slot reads kClonePending,
    // which promises a clone that is never coming, and the chain spends its
    // whole budget waiting for it.
    CHECK(ClassifySlot(true, false, true, kForeign, kOwns) ==
          SlotPaintState::kForeignOccupant);
    // ⚠ Foreign outranks ready. If an off-hand weapon ever did present a clone
    // here, painting it with the shield's colour would still be wrong.
    CHECK(ClassifySlot(true, true, true, kForeign, kOwns) ==
          SlotPaintState::kForeignOccupant);
    // No dye still wins over everything: an undyed slot asks no questions.
    CHECK(ClassifySlot(false, false, true, kForeign, kOwns) == SlotPaintState::kNoDye);
    // ⚠ EMPTY IS NOT FOREIGN, and this is the assertion that stops the fix
    // changing every character who simply is not carrying a shield. The caller's
    // predicate returns false for a null item, so an empty slot 9 keeps reading
    // kNothingWorn exactly as it did before.
    CHECK(ClassifySlot(true, false, false, kMine, kOwns) == SlotPaintState::kNothingWorn);

    // ---- where OS-111 and OS-116 meet, which is what the merge added --------
    // Neither branch could write these: each had only its own flag. They pin the
    // ORDER, which is the one thing a merge can quietly get wrong, because both
    // fixes look like "a staged slot with a permanently null partClone" and
    // swapping the two tests still passes every assertion above.
    //
    // Foreign outranks shared-below as well as ready. A slot holding something
    // that is not ours is answered by WHAT is in it; where the armature of the
    // garment we are not painting ended up is not a question worth asking.
    CHECK(ClassifySlot(true, false, true, kForeign, kSharedBelow) ==
          SlotPaintState::kForeignOccupant);
    CHECK(ClassifySlot(true, true, true, kForeign, kSharedBelow) ==
          SlotPaintState::kForeignOccupant);
    // ...and with foreign false the ownership answer still comes back, which is
    // the half that would go silent if the merged chain returned early.
    CHECK(ClassifySlot(true, false, true, kMine, kSharedBelow) ==
          SlotPaintState::kClonedElsewhere);

    // ---- the retry decision, OS-110 ----------------------------------------
    // A walk costs a real pass over the biped, so it is spent only when real
    // time moved. This is the whole fix: counting posts was never counting
    // frames, and twelve posts inside two milliseconds is not a wait.
    CHECK(ShouldWalkNow(true, kRepaintAttempts));
    CHECK(ShouldWalkNow(true, 1));
    // No time passed: do nothing at all. Under a queue that drains its own
    // additions in one pass, this is every link after the first.
    CHECK(!ShouldWalkNow(false, kRepaintAttempts));
    // Walks spent: no more passes even if a frame did go by.
    CHECK(!ShouldWalkNow(true, 0));
    CHECK(!ShouldWalkNow(false, 0));

    // Posting again needs something waiting AND room in both budgets.
    CHECK(ShouldPostAgain(1, kRepaintAttempts, kRepaintPosts));
    CHECK(ShouldPostAgain(3, 1, 1));
    // Nothing waiting: stop immediately, however much budget is left. A chain
    // that keeps running after everything painted is pure swap churn - each pass
    // restores every material and swaps it back.
    CHECK(!ShouldPostAgain(0, kRepaintAttempts, kRepaintPosts));
    // Walks spent: stop even though something is still waiting. Bounded on
    // purpose - a mesh that never finishes attaching leaves the slot pending
    // forever, and an unbounded chain would walk the whole biped every frame for
    // the rest of the session.
    CHECK(!ShouldPostAgain(2, 0, kRepaintPosts));
    // ⚠ Posts spent with walks to spare is the OS-110 signature, and it must end
    // the chain too or the spin bound buys nothing.
    CHECK(!ShouldPostAgain(2, kRepaintAttempts, 0));

    // That signature, named, so the caller can report it rather than going quiet.
    CHECK(DrainedWithoutWaiting(kRepaintAttempts, 0));
    CHECK(DrainedWithoutWaiting(1, 0));
    // Walks all spent is the ordinary exhaustion, not the drain signature.
    CHECK(!DrainedWithoutWaiting(0, 0));
    // Posts left means the chain ended for some other reason.
    CHECK(!DrainedWithoutWaiting(kRepaintAttempts, 5));

    // The budget has to be worth more than one frame or it is not a retry at
    // all: the first attempt is the synchronous-equivalent one that B2 showed
    // finds a null clone.
    CHECK(kRepaintAttempts > 1);
    // The spin bound has to exceed the walk budget or it would end chains that
    // were waiting legitimately, one frame at a time.
    CHECK(kRepaintPosts > kRepaintAttempts);
    // A gap of zero would not defeat a same-pass drain, which runs links
    // microseconds apart. Anything above zero does.
    CHECK(kRepaintGapMs > 0);

    // ---- both drain semantics, walked end to end ---------------------------
    // A queue that yields a frame per post: every link walks, and the chain is
    // worth exactly the walk budget, which is what the old comment claimed and
    // never delivered.
    unsigned walks = kRepaintAttempts, posts = kRepaintPosts, ran = 0;
    do {
        if (ShouldWalkNow(true, walks)) {
            --walks;
            ++ran;
        }
        if (posts > 0) {
            --posts;
        }
    } while (ShouldPostAgain(1, walks, posts));
    CHECK(ran == kRepaintAttempts);
    CHECK(!DrainedWithoutWaiting(walks, posts));

    // A queue that drains its own additions in one pass: the gap never opens
    // after the first link, so exactly ONE walk happens instead of twelve, the
    // spin bound ends it, and the signature is reportable.
    walks = kRepaintAttempts;
    posts = kRepaintPosts;
    ran   = 0;
    bool firstLink = true;
    do {
        if (ShouldWalkNow(firstLink, walks)) {  // only the first link sees a gap
            --walks;
            ++ran;
        }
        firstLink = false;
        if (posts > 0) {
            --posts;
        }
    } while (ShouldPostAgain(1, walks, posts));
    CHECK(ran == 1);
    CHECK(DrainedWithoutWaiting(walks, posts));

    // And a chain that finds nothing pending on its first pass costs exactly one
    // pass, which is what makes it safe to arm on every rebuild.
    walks = kRepaintAttempts;
    posts = kRepaintPosts;
    ran   = 0;
    do {
        if (ShouldWalkNow(true, walks)) {
            --walks;
            ++ran;
        }
        if (posts > 0) {
            --posts;
        }
    } while (ShouldPostAgain(0, walks, posts));
    CHECK(ran == 1);

    {  // ⚠ THE SECOND BIPED IS ONLY WALKED WHEN IT IS A DIFFERENT OBJECT, and
       // this is a measured hazard rather than a defensive nicety.
       // TESObjectREFR::GetBiped1 ignores its flag in the base implementation
       // and returns GetBiped2(), so "give me the first-person one" can hand
       // back the third-person one. Walking that twice would paint every shape,
       // then walk the same geometry again and count it a second time, and any
       // actor without a real first-person biped hits exactly that path.
       //
       // FPPROBE measured distinct=true on the player, which is what makes
       // first-person dye reachable at all. The guard is here because the
       // player is not the only actor Repaint runs on (OS-125).
        int  tp = 0;
        int  fp = 0;
        CHECK(ShouldWalkSecondBiped(&tp, &fp));
        CHECK(!ShouldWalkSecondBiped(&tp, &tp));  // same object, would double-walk
        CHECK(!ShouldWalkSecondBiped(&tp, nullptr));
        // A null FIRST argument is not a "walk it anyway" case. Repaint returns
        // early on a null third-person biped, so reaching here with one means
        // the caller changed shape and the answer should be no rather than a
        // walk of whatever the second pointer happens to be.
        CHECK(!ShouldWalkSecondBiped(nullptr, &fp));
        CHECK(!ShouldWalkSecondBiped(nullptr, nullptr));
    }

    if (g_failures == 0) {
        std::printf("all DyeGate tests passed\n");
    }
    return g_failures;
}
