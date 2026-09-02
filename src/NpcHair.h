#pragma once

#include <cstdint>

namespace RE {
    class Actor;
    class BGSHeadPart;
}

namespace OS::HeadPart {
    enum class Kind : std::uint8_t;
}

namespace OS::NpcHair {

    // Hair STYLE for a follower, which is a different mechanism from the
    // player's and not a scoping variant of it. HairStyle.h owns the player:
    // it writes the actor base with ChangeHeadPart and forces a head rebuild.
    // That route was tried on a follower on 2026-07-31 and works, in that her
    // hair changes, and it is unusable: the rebuild gives a dark elf a pale
    // human complexion, and it is the WRONG SKIN rather than the right skin
    // mis-shaded, so there is no correct value a repaint could write back.
    //
    // So this reaches the geometry instead and never rebuilds anything. Her
    // complexion, warpaint and makeup are untouched by construction, which is
    // the whole reason the module exists.
    //
    // ⚠ THE SEQUENCE IS NOT NEGOTIABLE and docs/re/head-part-attach.md carries
    // the derivation. Seven steps, five field runs to find them all, and every
    // missing one produced hair that was silently ABSENT rather than visibly
    // wrong. In particular the two finalisers run BETWEEN the attach loop and
    // the facegen loop, because that is the order the engine's own head build
    // uses, and the skin rebind at the end is what took four runs to reach.
    //
    // ⚠ CALL FROM ANY THREAD. Everything here queues its engine work onto the
    // game thread itself, and that is a correction rather than a convenience.
    // This header used to say the opposite, that callers must marshal and
    // nothing queued on their behalf, and the very first caller forgot: the
    // editor draws on FUCK's PRESENT thread and called straight through. It
    // crashed inside hdt::BSFaceGenNiNodeEx::SkinAllGeometry, because HDT-SMP
    // hooks FixSkinInstances, so the call re-entered SMP's skinning while the
    // game thread was free to be walking the same BSDismemberSkinInstance. The
    // visual artifacting alongside it was the same race, seen not caught.
    //
    // One place that cannot be forgotten beats a rule in a comment.

    // ⚠ TWO KINDS THROUGH ONE MODULE, HAIR AND FACIAL HAIR (OS-225). Every
    // capture below is keyed by (actor, kind), so a follower can wear a style
    // of ours AND a beard of ours, each restored and re-asserted on its own.
    // The kind of an Apply is read off the PART, because a part's type is what
    // says which of her own pieces to cull; asking the caller to repeat it is
    // how the two drift apart. Restore and AppliedTo take it because they have
    // no part to read it from.
    //
    // ⚠ EYES AND BROWS ARE NOT THIS AND ARE REFUSED. Brows are a facegen tinted
    // part baked into the head's own texture and eyes carry a per record TNAM;
    // culling one by geometry name and attaching another does not produce a
    // face, it produces a hole. Handles says which kinds this route takes.
    [[nodiscard]] bool Handles(HeadPart::Kind a_kind);

    // Cull her own part of a_part's kind and wear a_part instead. Captures
    // what it hid and what it added so Restore can be exact. Queued: nothing is
    // applied by the time this returns, but AppliedTo answers with the new part
    // immediately, so the editor never reads a stale row or re-fires a preview
    // against itself. A part whose kind this module does not handle is
    // ignored, and says so once.
    void Apply(RE::Actor* a_actor, RE::BGSHeadPart* a_part);

    // Put her own part of that kind back and forget the capture. Queued, same
    // as Apply. A no-op when this actor was never styled in that kind.
    void Restore(RE::Actor* a_actor, HeadPart::Kind a_kind);

    // What this actor is currently wearing through us in that kind, or null
    // when nothing. The editor reads it to label a reset control and to seed
    // the picker.
    [[nodiscard]] RE::BGSHeadPart* AppliedTo(RE::Actor* a_actor, HeadPart::Kind a_kind);

    // Does this actor wear ANY head part through us, in any kind?
    //
    // ⚠⚠ "IS SHE ONE OF OURS" HAS TWO ANSWERS AND NpcLoadSink ONLY KNEW ONE.
    // It gated her reload re-apply on the OUTFIT assignment map, so a follower
    // whose hair was changed and who was never given an outfit read as "not one
    // of ours" and her reload was skipped. Her head then rebuilt with her own
    // hair back under our style and nothing looked again. Reported 2026-08-29:
    // "when I change a follower hair her default hair comes back on top of the
    // hair i chose when i switch cell". The hair store and the outfit store are
    // different sets and every "is she ours" test has to ask both.
    [[nodiscard]] bool HoldsHeadPartFor(RE::Actor* a_actor);

    // Re-run the last Apply for this actor, in EVERY kind she wears through us,
    // because the attached geometry does NOT survive anything that rebuilds her
    // head. A cell change, a race switch, an equip that kicks a 3D refresh: all
    // of them drop it, and the capture it left behind describes nodes that no
    // longer exist. This clears
    // the stale capture first, so it re-culls and re-attaches against whatever
    // is there now rather than trying to undo something that is gone.
    void Reassert(RE::Actor* a_actor);

    // The engine just built a head part for somebody who is not the player.
    //
    // ⚠⚠ THE REBUILD NOBODY WAS CATCHING. Everything that re-asserts a
    // follower's style hangs off a seam Fitting Room drives - a refresh, a
    // load, a race switch - and a head the ENGINE rebuilds on its own schedule
    // goes through none of them. That is how "her old hair and her new hair at
    // the same time" survives: her own pieces come back, ours are untouched,
    // and nothing asks. HeadBuildHook already sees every head part of every
    // actor built, and it hands that fact here.
    //
    // ⚠ CALLED FROM INSIDE THE DETOUR, so it must stay atomics only: no lock,
    // no scenegraph, no session. It latches and queues; the reassert itself
    // runs on the game thread after the build, where it is safe to walk.
    void NoteEngineHeadBuild();

    // True once this style has been measured as one the engine cannot bind to
    // this actor. The editor greys those rows and stops previewing them,
    // because the answer never changes for a given part and re-attempting it
    // costs a visible glitch each time.
    //
    // ⚠ THESE ARE HARD ENGINE LIMITS, NOT BUGS WE ARE ROUTING AROUND, and
    // there are TWO of them. NpcHairPlan::JudgeBinding holds the rule.
    //
    //   1. TOO MANY BONES. The worker behind FixSkinInstances reads a
    //      per-geometry bone-name override block at (bone + 4) * 8 and guards
    //      it with nothing but `bone > 8`. The block holds eight entries, so
    //      the guard is off by one against its own array and EIGHT is the real
    //      ceiling. Field-observed failing at 9, 10, 11, 14, 17 and 18.
    //
    //   2. A BONE THAT IS NOT ON HER SKELETON, at ANY count. The engine
    //      resolves each bone by name against the actor's skeleton root and,
    //      when that returns null, says nothing and leaves the entry pointing
    //      at the nif's own node. That node has no world transform under her
    //      skeleton, so the mesh stretches away to infinity.
    //
    // ⚠ A CLAIM THAT USED TO SIT HERE WAS WRONG, and it shipped a broken
    // build: that physics hair always has nine or more bones "by
    // construction", so counting was sufficient. KSSMP_Ominous_Elf binds
    // seven, passed a count-only gate, and rendered stretched on a follower.
    // Its skin names six strand-chain bones, 'Ominous L1' through 'R3', which
    // exist only inside the hair nif and on neither skeleton_female.nif this
    // load order ships. Long chains do usually blow the count as well, which
    // is exactly why counting looked sufficient for as long as it did.
    [[nodiscard]] bool IsUnsupported(RE::BGSHeadPart* a_part);

    // The session-only subset of IsUnsupported (OS-99 Task 6.5): styles FSMP
    // failed to build physics for THIS session. Declined until restart, never
    // persisted, re-measured next launch. The editor asks so its tooltip can
    // say "failed this session, retries next" instead of "can never bind".
    [[nodiscard]] bool IsSessionUnsupported(RE::BGSHeadPart* a_part);

    // OS-247: drop the declines that were taken ONLY because no FSMP route was
    // armed yet, because one just armed.
    //
    // ⚠ CALL IT WHEN THE ROUTE GOES FROM DORMANT TO ARMED, never merely when
    // it is armed. FsmpBridge::ProveRoute() answers "is there a route", and it
    // latches, so a caller that cannot tell an arm from a repeat would run this
    // on every load boundary. It is an exact undo of what the OS-106 branch
    // recorded, so it clears nothing persisted and nothing declined for any
    // other cause; safe from the main thread, and free when there is nothing to
    // undo.
    void ClearFsmpAbsentDeclines();

    // OS-99 (Task 6.6): close the dwell hole. The deferred SMP health check
    // rides the next Apply/Restore for the actor, so a user who applies a
    // broken style and then just LOOKS at it would wear the stretch until
    // they touch the list again. The editor calls this every frame while
    // open; a pending arm older than the dwell window gets ONE game-thread
    // look queued for it. Since Task 6.7 a look may re-arm the entry with a
    // fresh stamp (the settle window) instead of concluding it; still
    // one-shot per arm, not a watcher, and free when nothing is pending.
    // Safe from any thread: it reads guarded state, mutates only the
    // queuedTick latch under the same guard, and queues through the
    // module's own game-thread marshal.
    void TickPendingSmpChecks();

    // The cell-return doubled hair (2026-08-21). A styled follower's own hair
    // comes back UNDER our attach after a cell change and return: the
    // load-side reassert re-applies and its cull succeeds, and something
    // later un-hides her pieces through a pass that builds no head part, so
    // neither HeadBuildHook nor any seam this module holds ever fires again.
    // TickPendingSmpChecks above cannot carry the answer either: it ticks
    // only while the editor draws, and this symptom's habitat is the editor
    // being shut and the player walking between cells.
    //
    // So every publish arms a three-step settle ladder (0.75 s, 2 s, 5 s) and
    // the ticker window in NpcHairSettle.{h,cpp} drains it from FUCK's
    // gameplay frame, the seam the dye cards already prove. Each step is one
    // ReassertNow, which is idempotent: a quiet head costs three name-lookup
    // walks and the ladder dies; a head still being un-hidden trips the
    // culled-again recovery, which re-arms. TickSettleChecks is safe from
    // any thread (guarded state, game-thread marshal, no engine access);
    // SettleChecksPending is the ticker's lock-free IsOpen answer.
    void               TickSettleChecks();
    [[nodiscard]] bool SettleChecksPending();

    // Drop every capture without touching a single actor. For a save revert:
    // the character these captures describe is being torn down and replaced,
    // so writing to them would reach the wrong scenegraph. Same posture as
    // HairStyle::Clear and HairColor::Clear.
    //
    // ⚠ Deliberately does NOT clear the unsupported set. That set describes
    // MESHES, not characters, and the answer for a mesh does not change when
    // the save does.
    void Clear();

    // OS-102. Tear every applied style down while the actors' 3D is STILL
    // VALID. Call from kPreLoadGame and nowhere else.
    //
    // ⚠ THIS IS AN ORDERING FIX, NOT A CLEANUP ONE, AND Clear() CANNOT STAND IN
    // FOR IT. Clear() runs from the revert callback, by which point the engine
    // has already destroyed the heads, and it deliberately touches no actor. So
    // FSMP is never told, and it keeps physics systems whose bone pointers name
    // a destroyed head; the next publish walks them through its SkinAllGeometry
    // hook and faults. Restoring here breaks the leaf links (which arms FSMP's
    // cleanHead sweep) and fires the reclaim nudge at the last instant either is
    // possible. Both still want doing: this one for FSMP, Clear() for our own
    // captures afterwards.
    //
    // Runs synchronously on the calling (main) thread - a queued task would
    // drain after the load, which is the ordering this exists to beat.
    void OnPreLoadGame();

    // Load Data/SKSE/Plugins/FittingRoom/unsupported-hair.json into the
    // unsupported set. Call once at kDataLoaded (needs the data handler to
    // resolve keys). Without this, every declined style costs its one visible
    // dud attach again each session; with it, once ever.
    void LoadUnsupportedAtStartup();

}  // namespace OS::NpcHair
