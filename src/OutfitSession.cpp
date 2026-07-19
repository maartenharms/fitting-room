#include "OutfitSession.h"

#include "REAugments.h"
#include "SceneGuard.h"
#include "Settings.h"
#include "StyleRef.h"

namespace OS {

    namespace {
        // A weapon style key -> live WEAP/AMMO. Ammo classes read back as AMMO,
        // everything else as WEAP; a key naming the wrong type comes back null
        // (LookupForm type-checks). Shared by WeaponDisplayFor (player) and the
        // snapshot build (NPC) so both resolve weapon styles identically.
        RE::TESBoundObject* ResolveWeaponStyleForm(WeaponClass a_class, const StyleRefKey& a_key) {
            const bool isAmmo = a_class == WeaponClass::Arrows || a_class == WeaponClass::Bolts;
            return isAmmo ? static_cast<RE::TESBoundObject*>(StyleRef::ResolveAmmo(a_key))
                          : static_cast<RE::TESBoundObject*>(StyleRef::ResolveWeapon(a_key));
        }

        // An NpcKey -> the runtime BASE formID in the CURRENT load order, or 0
        // when the plugin is absent. Mirrors StyleRef::Resolve's data-handler
        // lookup; type-checked as a TESNPC. The 0 return is the missing-plugin
        // signal: the assignment stays in the map (re-encoded on save) but is
        // not rendered until the plugin returns.
        std::uint32_t ResolveBaseFormID(const NpcKey& a_key) {
            if (a_key.modName.empty()) {
                return 0;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return 0;
            }
            auto* npc = dh->LookupForm<RE::TESNPC>(a_key.localFormID, a_key.modName);
            return npc ? npc->GetFormID() : 0;
        }

        // Classify a staging target handle: is it the player, and if not, what
        // is its base identity? An unresolvable handle classifies as a non-
        // player with baseFormID 0 - inert staging that drives neither the
        // player channel nor any snapshot entry, rather than a stale handle
        // wrongly hijacking the player.
        struct TargetClass {
            bool                  isPlayer{ true };
            std::uint32_t         baseFormID{ 0 };
            std::optional<NpcKey> key;
        };

        TargetClass ClassifyTarget(RE::ActorHandle a_handle) {
            TargetClass tc;
            auto        ptr   = a_handle.get();
            RE::Actor*  actor = ptr.get();
            auto*       player = RE::PlayerCharacter::GetSingleton();
            if (!actor) {
                tc.isPlayer = false;  // unresolvable -> inert
                return tc;
            }
            if (player && actor == player) {
                return tc;  // isPlayer already true
            }
            tc.isPlayer = false;
            if (auto* base = actor->GetActorBase()) {
                tc.baseFormID = base->GetFormID();
                NpcKey k;
                if (auto* file = base->GetFile(0)) {
                    k.modName = std::string{ file->GetFilename() };
                }
                k.localFormID = base->GetLocalFormID();
                tc.key        = std::move(k);
            }
            return tc;
        }
    }  // namespace

    OutfitSession& OutfitSession::GetSingleton() {
        static OutfitSession instance;
        return instance;
    }

    const Outfit* OutfitSession::EffectiveLocked() const {
        // A scene mod (OStim etc.) is running - stand down entirely so the
        // player renders their real/undressed gear, not the transmog. Both
        // biped hooks gate on IsActive(), which flows through here, so this one
        // early-out suspends the whole override. Lock-free atomic read.
        if (SceneGuard::Active()) {
            return nullptr;
        }
        if (suspended_) {
            return nullptr;
        }
        // An NPC-target staging never drives the PLAYER channel: while the
        // editor previews on a follower, the player renders their own active
        // outfit. Only a player-target staging overrides here.
        if (staged_ && stagedForPlayer_) {
            return &*staged_;
        }
        return library_.Active();
    }

    void OutfitSession::RecomputeWeaponStylingLocked() {
        // NOT EffectiveLocked(): that folds in SceneGuard, which flips
        // asynchronously and is never observed here, so a mutation landing
        // mid-scene would latch the flag false and leave it there once the scene
        // ended. suspended_ only moves through Suspend/Resume, which both
        // recompute, so it is safe to fold in. anyWeaponStyling_ is the PLAYER
        // weapon fast-path, so - like EffectiveLocked - a staged NPC target is
        // ignored here (the NPC weapon path reads the render snapshot instead).
        const Outfit* o =
            suspended_ ? nullptr : ((staged_ && stagedForPlayer_) ? &*staged_ : library_.Active());
        anyWeaponStyling_.store(o && AnyWeaponEntry(*o), std::memory_order_release);
    }

    OutfitSession::WeaponDisplayEntry OutfitSession::WeaponDisplayFor(WeaponClass a_class) const {
        SlotEntry entry;
        {
            std::scoped_lock l(lock_);
            const auto*      o = EffectiveLocked();
            if (!o) {
                return {};  // suspended, mid-scene, or no outfit at all
            }
            entry = o->WeaponEntryFor(a_class);
        }
        // Resolved outside the lock, as VisitStyles does: StyleRef goes through
        // the data handler. The blocklist is an ARMOR mask, so it does not apply.
        if (entry.kind != SlotEntry::Kind::kStyle) {
            return { entry.kind, nullptr };  // kPassthrough / kHide need no form
        }
        RE::TESBoundObject* form = ResolveWeaponStyleForm(a_class, entry.style);
        if (!form) {
            spdlog::debug("weapon class {}: '{}'|{:06X} unresolved (plugin missing?)",
                          ClassJsonName(a_class), entry.style.modName, entry.style.localFormID);
            return {};  // inert style -> passthrough: show the real weapon
        }
        return { SlotEntry::Kind::kStyle, form };
    }

    bool OutfitSession::IsActive() const {
        std::scoped_lock l(lock_);
        return EffectiveLocked() != nullptr;
    }

    DisplaySet OutfitSession::Display() const {
        std::scoped_lock l(lock_);
        const auto* o = EffectiveLocked();
        return o ? ComputeDisplaySet(*o, blocklist_) : DisplaySet{};
    }

    void OutfitSession::VisitStyles(
        const std::function<void(std::uint32_t, RE::TESObjectARMO*)>& a_fn) const {
        std::vector<std::pair<std::uint32_t, StyleRefKey>> snapshot;
        {
            std::scoped_lock l(lock_);
            const auto* o = EffectiveLocked();
            if (!o) {
                return;
            }
            const auto allowed = ComputeDisplaySet(*o, blocklist_).styleMask;
            o->ForEachStyle([&](std::uint32_t bit, const StyleRefKey& key) {
                if ((allowed >> bit) & 1u) {
                    snapshot.emplace_back(bit, key);
                }
            });
        }
        for (const auto& [bit, key] : snapshot) {
            if (auto* armo = StyleRef::Resolve(key)) {
                a_fn(bit, armo);
            } else {
                spdlog::debug("style bit {}: '{}'|{:06X} unresolved (plugin missing?)",
                              bit, key.modName, key.localFormID);
            }
        }
    }

    void OutfitSession::BeginStaging(const Outfit& a_from) {
        // The player is the default staging target. forPlayer is forced true
        // here rather than derived from handle resolution, so the player path
        // can never fall inert even if the player handle fails to resolve -
        // preserving the original unconditional player-staging behavior. The
        // handle is recorded only so StagingTarget() reports uniformly.
        RE::ActorHandle playerHandle;
        if (auto* p = RE::PlayerCharacter::GetSingleton()) {
            playerHandle = p->GetHandle();
        }
        {
            std::scoped_lock l(lock_);
            const bool wasNpcStaging = staged_.has_value() && !stagedForPlayer_;

            stagedTarget_     = playerHandle;
            stagedForPlayer_  = true;
            stagedBaseFormID_ = 0;
            stagedNpcKey_.reset();
            staged_           = a_from;

            RecomputeWeaponStylingLocked();
            if (wasNpcStaging) {
                RebuildSnapshotLocked();  // drop a prior NPC override
            }
        }
        RequestRefresh();
    }

    void OutfitSession::BeginStaging(RE::ActorHandle a_target, const Outfit& a_from) {
        // Classify the target with engine reads BEFORE taking the lock.
        auto       tc = ClassifyTarget(a_target);
        bool       forPlayer;
        {
            std::scoped_lock l(lock_);
            const bool wasNpcStaging = staged_.has_value() && !stagedForPlayer_;

            stagedTarget_     = a_target;
            stagedForPlayer_  = tc.isPlayer;
            stagedBaseFormID_ = tc.isPlayer ? 0u : tc.baseFormID;
            stagedNpcKey_     = tc.isPlayer ? std::nullopt : std::move(tc.key);
            staged_           = a_from;
            forPlayer         = stagedForPlayer_;

            RecomputeWeaponStylingLocked();  // player fast-path (uses library active when NPC-staging)
            // Rebuild the snapshot to publish a NEW NPC override, or to DROP a
            // prior one when switching back to the player. A pure player->player
            // stage with no prior NPC override leaves the snapshot untouched.
            if (!forPlayer || wasNpcStaging) {
                RebuildSnapshotLocked();
            }
        }
        // Staging SELF-REFRESHES its target, symmetric across the two channels:
        // a player target kicks the player (RequestRefresh), an NPC target kicks
        // that follower (RequestRefreshActor) so the live preview is visible
        // immediately on target-switch - "live-if-visible" per spec §5, not
        // deferred to Apply. RequestRefreshActor marshals to the main thread
        // (Task 5), so calling it from the present thread just queues.
        if (forPlayer) {
            RequestRefresh();
        } else {
            RequestRefreshActor(a_target);
        }
    }

    void OutfitSession::UpdateStaging(const Outfit& a_next) {
        bool            forPlayer = true;
        RE::ActorHandle target;  // captured under the lock for the NPC refresh below
        {
            std::scoped_lock l(lock_);
            if (!staged_) {
                return;
            }
            staged_   = a_next;
            forPlayer = stagedForPlayer_;
            target    = stagedTarget_;
            RecomputeWeaponStylingLocked();
            if (!forPlayer) {
                RebuildSnapshotLocked();  // refresh the NPC override with the new outfit
            }
        }
        // Symmetric self-refresh (see BeginStaging): each hover/click/Random
        // preview kicks the player or the staged follower so the edit shows on
        // the actor being dressed. Same cost shape as the player (one kick per
        // ~0.18s hover) - intentional, not debounced.
        if (forPlayer) {
            RequestRefresh();
        } else {
            RequestRefreshActor(target);
        }
    }

    void OutfitSession::CommitStaging() {
        bool forPlayer = true;
        {
            std::scoped_lock l(lock_);
            if (!staged_) {
                return;
            }
            forPlayer = stagedForPlayer_;
            if (forPlayer) {
                const int idx = library_.ActiveIndex();
                if (idx >= 0) {
                    if (auto* o = library_.At(static_cast<std::size_t>(idx))) {
                        *o = *staged_;
                    }
                }
            } else if (stagedNpcKey_) {
                // Minimal NPC commit. The editor's authoritative Apply path is
                // UpsertNpcLibrary(key, editedLibrary) (Task 8); this keeps a
                // bare CommitStaging coherent by writing the staged outfit into
                // the base's active outfit, creating the library/outfit if the
                // base had none. NEVER QueueLibrarySave - assignments are
                // per-save co-save state, never outfits.json.
                auto& rec = npcAssignments_[*stagedNpcKey_];
                int   idx = rec.library.ActiveIndex();
                if (idx < 0) {
                    idx = rec.library.Create("Outfit 1");
                    if (idx >= 0) {
                        rec.library.Activate(static_cast<std::size_t>(idx));
                    }
                }
                if (idx >= 0) {
                    if (auto* o = rec.library.At(static_cast<std::size_t>(idx))) {
                        *o = *staged_;
                    }
                }
            }
            staged_.reset();
            stagedNpcKey_.reset();
            stagedBaseFormID_ = 0;
            stagedForPlayer_  = true;
            stagedTarget_     = {};  // reset the staging fields as a unit
            RecomputeWeaponStylingLocked();
            if (!forPlayer) {
                RebuildSnapshotLocked();  // reflect the committed NPC outfit, drop the override
            }
        }
        if (forPlayer) {
            Persistence::QueueLibrarySave();  // commit mutates the GLOBAL player library
            RequestRefresh();
        }
    }

    void OutfitSession::DiscardStaging() {
        bool            forPlayer = true;
        bool            hadNpcStage;
        RE::ActorHandle oldTarget;  // captured BEFORE the reset for the NPC revert
        {
            std::scoped_lock l(lock_);
            forPlayer   = stagedForPlayer_;
            hadNpcStage = staged_.has_value() && !stagedForPlayer_;
            oldTarget   = stagedTarget_;
            staged_.reset();
            stagedNpcKey_.reset();
            stagedBaseFormID_ = 0;
            stagedForPlayer_  = true;
            stagedTarget_     = {};  // reset the staging fields as a unit
            RecomputeWeaponStylingLocked();
            if (hadNpcStage) {
                RebuildSnapshotLocked();  // drop the NPC preview override
            }
        }
        // Symmetric self-refresh (see BeginStaging): a player discard reverts the
        // player, an NPC discard kicks the follower so its preview reverts NOW -
        // when the editor switches away or closes without Apply - instead of
        // lingering until the actor's next natural rebuild.
        if (forPlayer) {
            RequestRefresh();
        } else if (hadNpcStage) {
            RequestRefreshActor(oldTarget);
        }
    }

    bool OutfitSession::IsStaging() const {
        std::scoped_lock l(lock_);
        return staged_.has_value();
    }

    std::optional<RE::ActorHandle> OutfitSession::StagingTarget() const {
        std::scoped_lock l(lock_);
        if (!staged_) {
            return std::nullopt;
        }
        return stagedTarget_;
    }

    void OutfitSession::Suspend() {
        {
            std::scoped_lock l(lock_);
            if (suspended_) {
                return;
            }
            suspended_ = true;
            RecomputeWeaponStylingLocked();
        }
        spdlog::info("override suspended (beast form / race switch).");
        RequestRefresh();
    }

    void OutfitSession::Resume() {
        {
            std::scoped_lock l(lock_);
            if (!suspended_) {
                return;
            }
            suspended_ = false;
            RecomputeWeaponStylingLocked();
        }
        spdlog::info("override resumed.");
        RequestRefresh();
    }

    void OutfitSession::OnLoad(OutfitLibrary a_lib) {
        {
            std::scoped_lock l(lock_);
            library_          = std::move(a_lib);
            staged_.reset();
            stagedNpcKey_.reset();
            stagedBaseFormID_ = 0;
            stagedForPlayer_  = true;
            stagedTarget_     = {};  // reset the staging fields as a unit
            suspended_        = false;
            RecomputeWeaponStylingLocked();
            RebuildSnapshotLocked();  // drop any stale staged NPC override
        }
        RequestRefresh();
    }

    void OutfitSession::OnRevert() {
        // The player library is GLOBAL (outfits.json) - it survives save
        // boundaries. Only per-save PLAYER state resets: the active selection
        // and any staging. NPC assignments are per-save too, but reset through
        // their own OnNpcRevert (a separate co-save callback).
        std::scoped_lock l(lock_);
        library_.Deactivate();
        staged_.reset();
        stagedNpcKey_.reset();
        stagedBaseFormID_ = 0;
        stagedForPlayer_  = true;
        stagedTarget_     = {};  // reset the staging fields as a unit
        suspended_        = false;
        RecomputeWeaponStylingLocked();
        RebuildSnapshotLocked();
    }

    // ---- Actor dimension (NPC/follower assignments) ------------------------

    void OutfitSession::UpsertNpcLibrary(const NpcKey& a_key, OutfitLibrary a_library) {
        std::scoped_lock l(lock_);
        npcAssignments_[a_key] = NpcRecord{ std::move(a_library) };
        RebuildSnapshotLocked();
        // NO QueueLibrarySave / no outfits.json write: assignments are per-save
        // co-save state, persisted from SnapshotNpcAssignments in SaveCallback.
    }

    void OutfitSession::RemoveNpcAssignment(const NpcKey& a_key) {
        std::scoped_lock l(lock_);
        npcAssignments_.erase(a_key);
        RebuildSnapshotLocked();
    }

    NpcAssignmentMap OutfitSession::SnapshotNpcAssignments() const {
        std::scoped_lock l(lock_);
        return npcAssignments_;
    }

    void OutfitSession::OnNpcLoad(NpcAssignmentMap a_map) {
        std::scoped_lock l(lock_);
        npcAssignments_ = std::move(a_map);
        suspendedActors_.clear();  // per-save runtime suspension starts clean
        RebuildSnapshotLocked();
    }

    void OutfitSession::OnNpcRevert() {
        // Assignments are PER-SAVE (unlike the global player library): a revert
        // clears them entirely; the next co-save load reinstalls.
        std::scoped_lock l(lock_);
        npcAssignments_.clear();
        suspendedActors_.clear();
        RebuildSnapshotLocked();
    }

    // Suspend/ResumeActor each trigger a full snapshot rebuild - intended for
    // RARE per-actor events (race switch, beast form). Do not wire them to a
    // frequent event without making the rebuild incremental first. NEITHER
    // kicks a visual rebuild on the actor itself: SuspendActor's caller
    // (RaceSwitchSink) needs none - the engine's own SwitchRace rebuild
    // already ran with nothing left to style over on an alt form - but
    // ResumeActor's caller does, since that same prior rebuild ran against
    // the still-suspended snapshot; see RaceSwitchSink.cpp's ResumeActor +
    // RequestRefreshActor pairing.
    void OutfitSession::SuspendActor(std::uint32_t a_baseFormID) {
        if (a_baseFormID == 0) {
            return;
        }
        std::scoped_lock l(lock_);
        if (!suspendedActors_.insert(a_baseFormID).second) {
            return;  // already suspended - no snapshot churn
        }
        RebuildSnapshotLocked();
    }

    void OutfitSession::ResumeActor(std::uint32_t a_baseFormID) {
        std::scoped_lock l(lock_);
        if (suspendedActors_.erase(a_baseFormID) == 0) {
            return;  // was not suspended
        }
        RebuildSnapshotLocked();
    }

    ResolvedNpcDisplay OutfitSession::ResolveDisplayLocked(const Outfit& a_outfit) const {
        ResolvedNpcDisplay rd;
        rd.display = ComputeDisplaySet(a_outfit, blocklist_);

        // Armor styles: resolve only the bits the DisplaySet allows (blocklist
        // / never-touch already removed), exactly as VisitStyles does. An
        // unresolved style (plugin gone) is dropped. The worn-required mask is
        // applied LATER, by the hook, against the actor's real worn coverage.
        a_outfit.ForEachStyle([&](std::uint32_t a_bit, const StyleRefKey& a_key) {
            if (!((rd.display.styleMask >> a_bit) & 1u)) {
                return;
            }
            if (auto* armo = StyleRef::Resolve(a_key)) {
                rd.styles.push_back(ResolvedNpcStyle{ a_bit, armo });
            }
        });

        // Weapon classes: resolve each non-passthrough entry, exactly as
        // WeaponDisplayFor does. A kStyle whose plugin is gone collapses to
        // passthrough (the inert-style policy), resolved once here.
        for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
            const auto  wc = static_cast<WeaponClass>(c);
            const auto& e  = a_outfit.WeaponEntryFor(wc);
            if (e.kind == SlotEntry::Kind::kHide) {
                rd.weapons[c] = ResolvedNpcWeapon{ SlotEntry::Kind::kHide, nullptr };
            } else if (e.kind == SlotEntry::Kind::kStyle) {
                if (auto* form = ResolveWeaponStyleForm(wc, e.style)) {
                    rd.weapons[c] = ResolvedNpcWeapon{ SlotEntry::Kind::kStyle, form };
                }
            }
        }
        return rd;
    }

    void OutfitSession::RebuildSnapshotLocked() {
        // Called under lock_ from every assignment / staging / suspension
        // mutation. Resolves every form ONCE at build time and publishes an
        // IMMUTABLE snapshot the hooks read lock-free. The hot path never
        // reaches here. Thread: the game thread for load/save/co-save paths, the
        // FUCK PRESENT thread for editor-driven NPC staging/assignment edits -
        // both safe (resolved TESForm* is thread-stable, the map is immutable,
        // and present-thread form resolution is already the norm in the editor's
        // Draw path). See the ActorRenderSnapshot header comment in OutfitSession.h.
        //
        // Cost note: this re-resolves EVERY assigned NPC's forms, not just the
        // one that changed, so the critical section scales with the assignment
        // count (capped at kMaxNpcAssignments). Fine because every caller is
        // editor/event-driven (assignment edits, staging changes, race-switch
        // suspension), never per-frame - an NPC hover-preview is edge-triggered.
        // If a future path calls a mutator frequently, resolve incrementally.
        auto snap = std::make_shared<ActorRenderSnapshot>();

        const bool npcStaging =
            staged_.has_value() && !stagedForPlayer_ && stagedBaseFormID_ != 0;

        for (const auto& [key, rec] : npcAssignments_) {
            const std::uint32_t base = ResolveBaseFormID(key);
            if (base == 0) {
                continue;  // plugin absent: kept in the map (re-saved), not rendered
            }
            const bool    suspended  = suspendedActors_.contains(base);
            const bool    stagedHere  = npcStaging && stagedBaseFormID_ == base;
            const Outfit* active      = rec.library.Active();
            const auto    src =
                NpcResolve::SelectNpcSource(suspended, stagedHere, active != nullptr);

            const Outfit* outfit = nullptr;
            switch (src) {
                case NpcResolve::NpcSource::kStagedOverride:
                    outfit = &*staged_;
                    break;
                case NpcResolve::NpcSource::kAssignedActive:
                    outfit = active;
                    break;
                case NpcResolve::NpcSource::kNone:
                    outfit = nullptr;
                    break;
            }
            if (outfit) {
                (*snap)[base] = ResolveDisplayLocked(*outfit);
            }
        }

        // A staged NPC target with no assignment yet (new-NPC live preview):
        // add it so the preview renders, unless the base is suspended.
        if (npcStaging && !snap->contains(stagedBaseFormID_) &&
            !suspendedActors_.contains(stagedBaseFormID_)) {
            (*snap)[stagedBaseFormID_] = ResolveDisplayLocked(*staged_);
        }

        // Publish snapshot FIRST, then the count, both with release. A hook that
        // acquire-reads count > 0 and then acquire-loads the snapshot is
        // guaranteed the non-null, fully-built pointer that made the count
        // positive (the store order + the paired acquires give the happens-before
        // edge; see NpcRenderCount()/RenderSnapshot()). Reading a slightly-stale
        // snapshot is memory-safe (shared_ptr keeps it alive); a torn/dangling
        // read is impossible (the map is immutable once published).
        const std::size_t n = snap->size();
        renderSnapshot_.store(std::move(snap), std::memory_order_release);
        npcRenderCount_.store(n, std::memory_order_release);
    }

    void OutfitSession::ActivateByName(std::string_view a_name) {
        bool found = false;
        {
            std::scoped_lock l(lock_);
            library_.Deactivate();
            if (!a_name.empty()) {
                for (std::size_t i = 0; i < library_.Count(); ++i) {
                    if (const auto* o = library_.At(i); o && o->name == a_name) {
                        library_.Activate(i);
                        found = true;
                        break;
                    }
                }
            }
            RecomputeWeaponStylingLocked();
        }
        if (!a_name.empty() && !found) {
            spdlog::warn("Active outfit '{}' not found in the global library (renamed or "
                         "deleted from another save?) - deactivated.",
                         a_name);
        }
        RequestRefresh();
    }

    void OutfitSession::SetBlocklist(std::uint32_t a_mask) {
        std::scoped_lock l(lock_);
        blocklist_ = a_mask;
        // No weapon recompute: the blocklist is an ARMOR slot mask, so it cannot
        // change AnyWeaponStyling(). Give weapons a blocklist and it can.
    }

    void OutfitSession::RequestRefresh() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] {
                // Defensive: a background refresh must never take the game down;
                // a C++ throw is caught and logged instead (engine AVs are still
                // handled by CrashGuard around the kick, not here).
                try {
                    REAug::RefreshPlayer(Settings::GetSingleton().sceneKick);
                } catch (const std::exception& e) {
                    spdlog::error("RefreshPlayer threw: {}", e.what());
                } catch (...) {
                    spdlog::error("RefreshPlayer threw a non-standard exception.");
                }
            });
        }
    }

    void OutfitSession::RequestRefreshActor(RE::ActorHandle a_actor) {
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        task->AddTask([a_actor] {
            // Same defensive contract as RequestRefresh's task, plus the
            // unload-safe re-resolve QueueNodeCull established: a handle
            // captured at request time may belong to an actor that streamed
            // out before this task drains.
            try {
                auto       ptr   = a_actor.get();
                RE::Actor* actor = ptr.get();
                if (!actor) {
                    return;  // unloaded since the request - its next natural rebuild catches up
                }
                REAug::RefreshActor(actor, Settings::GetSingleton().sceneKick);
            } catch (const std::exception& e) {
                spdlog::error("RefreshActor threw: {}", e.what());
            } catch (...) {
                spdlog::error("RefreshActor threw a non-standard exception.");
            }
        });
    }

}  // namespace OS
