#pragma once

#include "OutfitSession.h"
#include "SceneGuard.h"

// A gated per-actor NPC lookup shared by the render hooks (BipedHooks'
// HandleWornPass + GetWornMaskThunk, and WeaponHooks' LoadPartThunk NPC arm).
// The count/scene gate + snapshot load + actor/base resolve + find was
// copy-pasted across those thunks; this is the ONE place it lives now, so a
// future site (or a change to the gate order) only has to be right once.
namespace OS {

    // A gated per-actor NPC lookup for the render hooks. Lock-free: atomic count
    // and snapshot loads only, NEVER resolves a form. Returns a result whose
    // `entry` is null (=> the caller runs vanilla) when: no NPC assigned (count
    // 0), a scene is suppressing the override, the snapshot is null/empty, or
    // `owner` is not an assigned NPC. On a hit, `snapshot` keeps `entry` alive
    // for the caller's whole pass, and `actor`/`baseFormID` are filled. Call
    // ONLY on the game thread (the atomic shared_ptr load takes an internal
    // spinlock - see OutfitSession.h).
    //
    // LIFETIME: `entry` points INTO the map that `snapshot` co-owns, so bind the
    // result to a named local that outlives every use of `entry` - never read
    // `LookupAssignedNpc(...).entry` off a temporary, which would drop the
    // keep-alive at the semicolon and dangle. ([[nodiscard]] guards the discard.)
    struct NpcLookup {
        std::shared_ptr<const ActorRenderSnapshot> snapshot;
        const ResolvedNpcDisplay* entry{ nullptr };
        RE::Actor*    actor{ nullptr };
        std::uint32_t baseFormID{ 0 };
    };

    // Gate order, preserved exactly from the pre-extraction BipedHooks
    // preambles: count/scene FIRST (no lock, no snapshot load when nothing is
    // assigned anywhere or a scene stands the whole NPC path down - mirrors
    // OutfitSession::EffectiveLocked's scene gate on the player side), THEN
    // snapshot null/empty (pre-first-rebuild / emptied since the count was
    // read), THEN actor/base resolve (not an Actor, or no base), THEN the
    // snapshot find (this base is unassigned). Every early-out leaves `entry`
    // null, which is the caller's single "run vanilla" signal.
    //
    // noexcept: every step below is an atomic load, a register-free RTTI cast,
    // or an unordered_map::find on a trivial uint32_t key - none of it is
    // expected to throw, and every caller of this helper is itself inside a
    // hot-path noexcept thunk with its own try/catch, so an impossible escape
    // here should terminate rather than unwind into the engine.
    [[nodiscard]] inline NpcLookup LookupAssignedNpc(OutfitSession&      a_session,
                                                      RE::TESObjectREFR* a_owner) noexcept {
        NpcLookup lk;

        if (a_session.NpcRenderCount() == 0 || SceneGuard::Active()) {
            return lk;
        }

        lk.snapshot = a_session.RenderSnapshot();
        if (!lk.snapshot || lk.snapshot->empty()) {
            return lk;
        }

        lk.actor              = a_owner ? a_owner->As<RE::Actor>() : nullptr;
        RE::TESNPC* npcBase   = lk.actor ? lk.actor->GetActorBase() : nullptr;
        if (!npcBase) {
            return lk;
        }
        lk.baseFormID = npcBase->GetFormID();

        const auto it = lk.snapshot->find(lk.baseFormID);
        if (it == lk.snapshot->end()) {
            return lk;
        }
        lk.entry = &it->second;
        return lk;
    }

}  // namespace OS
