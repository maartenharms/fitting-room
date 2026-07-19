#pragma once

#include "PCH.h"

#include "NpcAssignments.h"
#include "NpcResolve.h"
#include "Outfit.h"
#include "Persistence.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace OS {

    // ---- Lock-free render snapshot (the biped/weapon hooks' NPC read side) --
    //
    // An IMMUTABLE, fully-resolved view of every assigned NPC's look, keyed by
    // BASE formID (owner->GetActorBase()->GetFormID()). Every pointer here was
    // resolved when the snapshot was BUILT (never in the hook): the biped/weapon
    // hook only atomic-loads the shared_ptr and reads these pointers; it NEVER
    // resolves a form, locks, or allocates on the hot path. Rebuilt wholesale on
    // any assignment / staging / suspension mutation and published atomically,
    // so an in-flight hook always reads a consistent, self-alive version.
    //
    // WHICH THREAD builds it: the load/save/co-save paths (OnLoad, OnNpcLoad,
    // OnNpcRevert, Persistence) build on the GAME THREAD; the editor-driven NPC
    // staging/assignment edits (BeginStaging(handle), UpdateStaging,
    // DiscardStaging, Upsert/RemoveNpcAssignment) build on the FUCK PRESENT
    // thread, because that is
    // where the editor draws. That is SAFE and consistent with the mod's
    // architecture: a resolved TESForm* is thread-stable once its record is
    // loaded, and the published map is immutable, so the biped hook (game
    // thread) reading it concurrently with a present-thread rebuild only ever
    // sees a complete old-or-new version. The one residual is StyleRef::Resolve's
    // LookupForm read racing a form-table rehash - which is PRE-EXISTING and
    // accepted: the editor's Draw path already resolves on the present thread
    // (row labels, EvaluateFitFor, WeaponDisplayFor), and with the inventory/SAM
    // menu up game-thread form churn is minimal. The hook's own contract is
    // unchanged - it still never resolves, only reads resolved pointers.

    // One resolved style piece: the editor bit it occupies and its live ARMO.
    // The bit is retained so the hook can apply the §3 worn-required rule
    // (NpcResolve::WornRequiredDisplay) per rebuild without re-deriving it.
    struct ResolvedNpcStyle {
        std::uint32_t      bit{ 0 };
        RE::TESObjectARMO* armo{ nullptr };
    };

    // One resolved weapon class entry. kPassthrough classes are stored inert
    // (form null) so the hook can index by class in O(1); a kStyle whose plugin
    // was gone at build time collapses to kPassthrough (the inert-style policy,
    // resolved once here rather than per part-load).
    struct ResolvedNpcWeapon {
        SlotEntry::Kind     kind{ SlotEntry::Kind::kPassthrough };
        RE::TESBoundObject* form{ nullptr };  // set iff kind == kStyle
    };

    // The complete resolved look for one base NPC.
    struct ResolvedNpcDisplay {
        // The outfit's full DisplaySet (blocklist already applied). The hook
        // narrows this to the actor's REAL worn coverage per rebuild via
        // NpcResolve::WornRequiredDisplay - the snapshot stores the unmasked
        // set because worn coverage is only known at rebuild time.
        DisplaySet                                       display;
        std::vector<ResolvedNpcStyle>                    styles;   // style bits only, bit order
        std::array<ResolvedNpcWeapon, kWeaponClassCount> weapons{};
    };

    // baseFormID -> resolved look. Immutable once published.
    using ActorRenderSnapshot = std::unordered_map<std::uint32_t, ResolvedNpcDisplay>;

    // The single source of truth for what the render override should display.
    // Thread-safe: hooks read it from the skinning pass.
    class OutfitSession {
    public:
        static OutfitSession& GetSingleton();

        // The outfit the override renders: the editor's staged outfit while the
        // editor is open, else the library's active outfit. One code path - a
        // live preview is not a second mechanism.
        [[nodiscard]] bool       IsActive() const;
        [[nodiscard]] DisplaySet Display() const;

        // Calls a_fn(bit, ARMO*) for each styled slot, resolved to live forms.
        void VisitStyles(const std::function<void(std::uint32_t, RE::TESObjectARMO*)>& a_fn) const;

        // What the weapon override must do for one class. The weapon dimension
        // is pull-shaped, not push-shaped like VisitStyles: the hook already
        // knows which class it is rendering, so it asks about that one rather
        // than being handed all eleven.
        struct WeaponDisplayEntry {
            SlotEntry::Kind     kind{ SlotEntry::Kind::kPassthrough };
            RE::TESBoundObject* style{ nullptr };  // set iff kind == kStyle
        };

        // The effective entry for a_class, resolved to a live WEAP/AMMO.
        // A kStyle whose plugin is gone resolves to nothing and is reported as
        // kPassthrough - the shipped inert-style policy: a missing style shows
        // the real weapon, it never blanks it.
        [[nodiscard]] WeaponDisplayEntry WeaponDisplayFor(WeaponClass a_class) const;

        // Hook fast path: false = no weapon class is styled or hidden, so the
        // caller can skip WeaponDisplayFor (and its lock) entirely. The contract
        // is one-directional - never false while styling is live, but MAY read
        // true while styling is suppressed (a scene mod is running; see
        // WeaponDisplayFor), which costs one locked call returning kPassthrough.
        // Acquire pairs with the release store under lock_, so a hook that sees
        // true also sees the outfit that made it true (free on x86; the ordering
        // is what makes the invariant hold rather than merely happen to).
        [[nodiscard]] bool AnyWeaponStyling() const {
            return anyWeaponStyling_.load(std::memory_order_acquire);
        }

        // Locked access to the outfit library. The callback runs while lock_ is
        // held, so it must NOT call another OutfitSession method (self-deadlock)
        // and must NOT call RequestRefresh(). Does not auto-refresh: if the
        // callback changes the displayed appearance, the caller calls
        // RequestRefresh() afterwards. Every call queues a global-library file
        // save (the library persists across saves) - mutators are the only
        // intended callers; readers use SnapshotLibrary.
        template <class Fn>
        void WithLibrary(Fn&& a_fn) {
            {
                std::scoped_lock l(lock_);
                std::forward<Fn>(a_fn)(library_);
                // The callback may have changed the active outfit's weapon
                // entries, or which outfit is active at all.
                RecomputeWeaponStylingLocked();
            }
            Persistence::QueueLibrarySave();
        }

        // A consistent copy for readers that need to hold library state across a
        // frame (e.g. the ImGui editor's draw loop) without holding the lock.
        [[nodiscard]] OutfitLibrary SnapshotLibrary() const {
            std::scoped_lock l(lock_);
            return library_;
        }

        // Editor staging. Staging now has a TARGET actor: the player (default)
        // stages into the player render channel exactly as before, while an NPC
        // target's staged outfit overrides ONLY that base's snapshot entry (a
        // live editor preview on a follower). UpdateStaging/CommitStaging/
        // DiscardStaging all honor whichever target BeginStaging set.
        void BeginStaging(const Outfit& a_from);                     // player (default target)
        void BeginStaging(RE::ActorHandle a_target, const Outfit& a_from);
        void UpdateStaging(const Outfit& a_next);
        void CommitStaging();   // player: staged -> active outfit; NPC: upsert into the base's library
        void DiscardStaging();
        [[nodiscard]] bool IsStaging() const;

        // Who staging currently targets (nullopt when not staging). The player
        // handle for the player path; an NPC's handle otherwise. Editor reads
        // this to route Apply / footer text.
        [[nodiscard]] std::optional<RE::ActorHandle> StagingTarget() const;

        // ---- Actor dimension (NPC/follower assignments) --------------------
        // Mutation side: an OWN mutex-protected map, keyed by base NPC identity.
        // These NEVER ride WithLibrary - assignments are per-save co-save state
        // and must not queue an outfits.json save. Every mutation rebuilds the
        // lock-free render snapshot below.
        // Neither call refreshes: an assignment edit's visual effect on a
        // LOADED actor is the caller's job via RequestRefreshActor (the
        // editor's Apply path does this - Task 8), exactly like WithLibrary's
        // "does not auto-refresh" contract above. Resolving the base's
        // current handle lives with the caller, not here - OutfitSession's
        // own mutation methods never walk ProcessLists.
        void UpsertNpcLibrary(const NpcKey& a_key, OutfitLibrary a_library);
        void RemoveNpcAssignment(const NpcKey& a_key);

        // A consistent copy for the editor's target picker / draw loop.
        [[nodiscard]] NpcAssignmentMap SnapshotNpcAssignments() const;

        // Co-save lifecycle. OnNpcLoad installs the loaded per-save assignments
        // (missing-plugin entries are kept verbatim, just not rendered until the
        // plugin returns). OnNpcRevert clears ALL of it: unlike the global
        // library, assignments are per-save and do not survive a save boundary.
        void OnNpcLoad(NpcAssignmentMap a_map);
        void OnNpcRevert();

        // Per-actor suspension (race switch / beast form), keyed by BASE
        // formID. Separate from the global player suspended_ below; a suspended
        // base is dropped from the snapshot until resumed.
        void SuspendActor(std::uint32_t a_baseFormID);
        void ResumeActor(std::uint32_t a_baseFormID);

        // The hook's lock-free NPC read side. NpcRenderCount() is the fast-path
        // gate - zero means "no assigned/staged NPC", so the non-player hook
        // path early-outs with no snapshot load at all, exactly like today.
        // RenderSnapshot() atomic-loads the immutable resolved map.
        //
        // Both loads are ACQUIRE, pairing with the release stores under lock_ in
        // RebuildSnapshotLocked (snapshot first, count second). So a consumer
        // that reads count > 0 and THEN loads the snapshot is guaranteed the
        // fully-built, non-null pointer that made the count positive - the
        // documented invariant holds on the abstract machine, not just on x86.
        // Same discipline as AnyWeaponStyling(); free on x86.
        //
        // RenderSnapshot() alone MAY be null (before the first rebuild) - always
        // gate on NpcRenderCount() > 0 first, or null-check the result.
        [[nodiscard]] std::shared_ptr<const ActorRenderSnapshot> RenderSnapshot() const {
            return renderSnapshot_.load(std::memory_order_acquire);
        }
        [[nodiscard]] std::size_t NpcRenderCount() const {
            return npcRenderCount_.load(std::memory_order_acquire);
        }

        // Beast form / race switch (global / player path). Unchanged.
        void Suspend();
        void Resume();

        // Persistence callbacks. OnLoad installs the GLOBAL library (file or
        // legacy migration); OnRevert resets only per-save state - the library
        // is global and survives save/load boundaries.
        void OnLoad(OutfitLibrary a_lib);
        void OnRevert();

        // Activate the outfit with this name (per-save active selection);
        // empty or unknown name deactivates. Refreshes the player.
        void ActivateByName(std::string_view a_name);

        // Does not refresh; a caller changing this at runtime must call RequestRefresh() afterwards.
        void SetBlocklist(std::uint32_t a_mask);

        // Queue a player visual rebuild on the main thread.
        static void RequestRefresh();

        // Queue an ACTOR visual rebuild on the main thread - the NPC/follower
        // counterpart to RequestRefresh, and the intended trigger for
        // SuspendActor/ResumeActor's visual effect (the mutators themselves
        // only touch the snapshot; see their comments). Handle-based and
        // unload-safe by construction (mirrors BipedPost::QueueNodeCull): the
        // handle is re-resolved when the queued task actually runs, so an
        // actor that streamed out between the request and the drain is
        // silently skipped rather than dereferencing a stale pointer - its
        // next natural rebuild on reload catches up regardless.
        static void RequestRefreshActor(RE::ActorHandle a_actor);

    private:
        OutfitSession() = default;
        [[nodiscard]] const Outfit* EffectiveLocked() const;

        // Refresh anyWeaponStyling_ from the outfit that is now effective.
        // Call under lock_ from EVERY path that can change it.
        void RecomputeWeaponStylingLocked();

        // Rebuild the immutable render snapshot from the current assignments +
        // staging target + suspension set, resolving every form on the game
        // thread, and publish it atomically. Call under lock_ from EVERY path
        // that changes an assignment, the staged NPC target, or suspension.
        void RebuildSnapshotLocked();

        // Resolve one outfit into a fully-resolved ResolvedNpcDisplay (forms +
        // masks). Reads blocklist_, so call under lock_. Shares StyleRef and
        // ComputeDisplaySet with the player Display()/VisitStyles path.
        [[nodiscard]] ResolvedNpcDisplay ResolveDisplayLocked(const Outfit& a_outfit) const;

        mutable std::mutex    lock_;
        OutfitLibrary         library_;
        std::optional<Outfit> staged_;
        std::uint32_t         blocklist_{ 0 };
        bool                  suspended_{ false };
        std::atomic<bool>     anyWeaponStyling_{ false };  // see AnyWeaponStyling()

        // Staging target. When stagedForPlayer_ is true, staged_ drives the
        // player channel (EffectiveLocked / weapon fast-path) exactly as
        // before; when false, staged_ overrides ONLY the NPC base identified by
        // stagedNpcKey_ / stagedBaseFormID_ in the render snapshot and never
        // touches the player. All meaningful only while staged_ has a value.
        bool                  stagedForPlayer_{ true };
        RE::ActorHandle       stagedTarget_;          // player handle for the player path
        std::optional<NpcKey> stagedNpcKey_;          // set iff staging an NPC (for CommitStaging upsert)
        std::uint32_t         stagedBaseFormID_{ 0 };  // resolved base formID of an NPC target (0 = player/none)

        // Actor dimension. npcAssignments_ is the mutation-side source of truth
        // (per-save co-save state); suspendedActors_ holds BASE formIDs stood
        // down by a race switch. renderSnapshot_ is the lock-free, resolved,
        // immutable read side the hooks consume; npcRenderCount_ is its relaxed
        // fast-path count (published alongside every rebuild).
        NpcAssignmentMap                  npcAssignments_;
        std::unordered_set<std::uint32_t> suspendedActors_;

        std::atomic<std::shared_ptr<const ActorRenderSnapshot>> renderSnapshot_;
        std::atomic<std::size_t>                                npcRenderCount_{ 0 };
    };

}  // namespace OS
