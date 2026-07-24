#include "OutfitSession.h"

#include "BipedPost.h"
#include "ObodyApi.h"
#include "PresetPreviewPolicy.h"
#include "REAugments.h"
#include "RefreshGate.h"
#include "SceneGuard.h"
#include "Settings.h"
#include "StyleRef.h"

#include <chrono>

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

        std::atomic<bool>                 g_playerRefreshQueued{ false };
        std::atomic<bool>                 g_playerBodyApplyQueued{ false };
        RefreshGate::BlockingCooldown     g_blockingCooldown;

        void QueuePlayerPreviewEquipmentShow() {
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                BipedPost::QueueObjectNodeShow(
                    player->GetHandle(), PresetPreviewPolicy::kSuppressedBipedObjects);
            }
        }

        double SteadySeconds() {
            using Seconds = std::chrono::duration<double>;
            return Seconds(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool PlayerIsBlocking() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return false;
            }
            auto* state = player->AsActorState();
            return player->IsBlocking() || (state && state->actorState2.wantBlocking);
        }

        void ApplyPlayerBodyState() {
            try {
                const auto body = OutfitSession::GetSingleton().DisplayBody();
                if (body.drives) {
                    ObodyApi::ApplyOutfitBody(RE::PlayerCharacter::GetSingleton(), body.preset,
                                              static_cast<int>(body.orefit),
                                              body.torsoStyleMask, body.torsoHideMask);
                } else if (body.restoreBaseline) {
                    // Equipped gear is a real destination, not a suspended
                    // preview. Restore the player's captured OBody assignment
                    // after leaving an outfit or follower mannequin.
                    ObodyApi::ApplyOutfitBody(
                        RE::PlayerCharacter::GetSingleton(), {},
                        static_cast<int>(ORefitMode::kDefault), 0, 0);
                } else {
                    ObodyApi::ReleaseOutfitORefit(RE::PlayerCharacter::GetSingleton());
                }
            } catch (const std::exception& e) {
                spdlog::error("ApplyPlayerBodyState threw: {}", e.what());
            } catch (...) {
                spdlog::error("ApplyPlayerBodyState threw a non-standard exception.");
            }
        }

        // Update3DModel can finish rebuilding armor/body nodes after
        // REAug::RefreshPlayer returns. Applying OBody in that same task can
        // therefore morph the outgoing nodes, then lose the visible result
        // when the replacement 3D arrives. Queue exactly one follow-up pass:
        // it reads the latest staged body state after the rebuild has crossed
        // a task boundary, so rapid hover/click changes coalesce safely.
        void QueuePlayerBodyStateAfterRefresh() {
            if (g_playerBodyApplyQueued.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([] {
                    g_playerBodyApplyQueued.store(false, std::memory_order_release);
                    ApplyPlayerBodyState();
                });
            } else {
                g_playerBodyApplyQueued.store(false, std::memory_order_release);
                ApplyPlayerBodyState();
            }
        }

        void ApplyNpcBodyState(RE::ActorHandle a_actor) {
            try {
                auto       ptr   = a_actor.get();
                RE::Actor* actor = ptr.get();
                if (!actor) {
                    return;
                }
                auto& session = OutfitSession::GetSingleton();
                auto  body    = session.DisplayBodyForNpc(actor);
                if (body.drives) {
                    if (!body.preset.empty() && !body.baselineCaptured) {
                        // A follower may not have passed through OBody's normal
                        // distribution yet. Process first so the capture records
                        // the actor's real baseline rather than a premature
                        // empty assignment.
                        ObodyApi::EnsureProcessed(actor);
                        body.baseline = ObodyApi::AssignedPreset(actor);
                        body.baselineCaptured = true;
                        session.CaptureNpcBodyBaseline(body.key, body.baseline);
                        spdlog::info(
                            "OBody: captured follower baseline '{}' for {}|{:06X}.",
                            body.baseline.empty() ? "(none)" : body.baseline,
                            body.key.modName, body.key.localFormID);
                    }
                    ObodyApi::ApplyNpcOutfitBody(
                        actor, body.preset, static_cast<int>(body.orefit),
                        body.torsoStyleMask, body.torsoHideMask,
                        body.baseline, body.baselineCaptured);
                } else {
                    ObodyApi::ReleaseOutfitORefit(actor);
                }
            } catch (const std::exception& e) {
                spdlog::error("ApplyNpcBodyState threw: {}", e.what());
            } catch (...) {
                spdlog::error("ApplyNpcBodyState threw a non-standard exception.");
            }
        }

        void QueueNpcBodyStateAfterRefresh(RE::ActorHandle a_actor) {
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask([a_actor] { ApplyNpcBodyState(a_actor); });
            } else {
                ApplyNpcBodyState(a_actor);
            }
        }

        void ApplyPlayerRefresh() {
            // Defensive: a background refresh must never take the game down;
            // a C++ throw is caught and logged instead (engine AVs are still
            // handled by CrashGuard around the kick, not here).
            try {
                REAug::RefreshPlayer(Settings::GetSingleton().sceneKick);
                QueuePlayerBodyStateAfterRefresh();
            } catch (const std::exception& e) {
                spdlog::error("RefreshPlayer threw: {}", e.what());
            } catch (...) {
                spdlog::error("RefreshPlayer threw a non-standard exception.");
            }
        }

        void RunPlayerRefreshWhenSafe() {
            if (!g_blockingCooldown.Ready(PlayerIsBlocking(), SteadySeconds())) {
                // AddTask calls made while the queue drains land on a later
                // game-thread pass. Polling therefore stays non-blocking even
                // while an inventory menu has paused world time.
                if (auto* task = SKSE::GetTaskInterface()) {
                    task->AddTask(RunPlayerRefreshWhenSafe);
                } else {
                    g_playerRefreshQueued.store(false, std::memory_order_release);
                }
                return;
            }

            // Clear before applying. A concurrent hover arriving during the
            // rebuild may queue one follow-up, while all requests accumulated
            // during the block collapse into this latest-state refresh.
            g_playerRefreshQueued.store(false, std::memory_order_release);
            ApplyPlayerRefresh();
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
        // NPC editing gives the inventory's player-only viewport a transient
        // mannequin. This outranks the player's saved outfit but never mutates
        // it; clearing the optional exposes the exact prior library state.
        if (playerMannequin_) {
            return &*playerMannequin_;
        }
        if (staged_ && stagedForPlayer_) {
            return &*staged_;
        }
        return library_.Active();
    }

    OutfitSession::BodyDisplay OutfitSession::DisplayBody() const {
        std::scoped_lock l(lock_);
        BodyDisplay      d;
        // EffectiveLocked also returns null while a scene mod is running or the
        // override is suspended. Leaving the body alone there is deliberate:
        // those states undress the character temporarily, and yanking the body
        // preset back and forth around an OStim scene would be worse than
        // letting it ride.
        if (const auto* o = EffectiveLocked()) {
            d.preset = o->obodyPreset;
            d.orefit = o->orefit;
            const auto display = ComputeDisplaySet(*o, blocklist_);
            d.torsoStyleMask   = display.styleMask & kORefitTorsoMask;
            d.torsoHideMask    = display.hideMask & kORefitTorsoMask;
            d.drives = true;
        } else if (!SceneGuard::Active() && !suspended_) {
            // No active saved outfit means Equipped gear. Unlike a scene
            // suspension, that state must undo a transient follower mannequin
            // body preview (or the last player outfit's body setting).
            d.restoreBaseline = true;
        }
        return d;
    }

    OutfitSession::NpcBodyDisplay OutfitSession::DisplayBodyForNpc(
        RE::Actor* a_actor) const {
        NpcBodyDisplay d;
        const auto     tc = ClassifyTarget(a_actor ? a_actor->GetHandle()
                                                   : RE::ActorHandle{});
        if (tc.isPlayer || !tc.key || tc.baseFormID == 0) {
            return d;
        }
        d.key = *tc.key;
        std::scoped_lock l(lock_);
        if (SceneGuard::Active() || suspendedActors_.contains(tc.baseFormID)) {
            return d;
        }
        const auto it = npcAssignments_.find(*tc.key);
        if (it != npcAssignments_.end()) {
            d.baseline         = it->second.obodyBaseline;
            d.baselineCaptured = it->second.obodyBaselineCaptured;
        }
        const Outfit* outfit = nullptr;
        if (staged_ && !stagedForPlayer_ && stagedBaseFormID_ == tc.baseFormID) {
            outfit = &*staged_;
        } else if (it != npcAssignments_.end()) {
            outfit = it->second.library.Active();
        }
        if (outfit) {
            d.preset = outfit->obodyPreset;
            d.orefit = outfit->orefit;
            const auto display = ComputeDisplaySet(*outfit, blocklist_);
            d.torsoStyleMask   = display.styleMask & kORefitTorsoMask;
            d.torsoHideMask    = display.hideMask & kORefitTorsoMask;
            d.drives           = true;
        } else if (d.baselineCaptured) {
            // Equipped gear after a body-setting outfit must restore the
            // follower's captured OBody assignment exactly once per refresh.
            d.drives = true;
        }
        return d;
    }

    void OutfitSession::CaptureNpcBodyBaseline(
        const NpcKey& a_key, std::string a_preset) {
        std::scoped_lock l(lock_);
        auto&            rec = npcAssignments_[a_key];
        if (!rec.obodyBaselineCaptured) {
            rec.obodyBaseline         = std::move(a_preset);
            rec.obodyBaselineCaptured = true;
        }
    }

    void OutfitSession::RecomputeWeaponStylingLocked() {
        // NOT EffectiveLocked(): that folds in SceneGuard, which flips
        // asynchronously and is never observed here, so a mutation landing
        // mid-scene would latch the flag false and leave it there once the scene
        // ended. suspended_ only moves through Suspend/Resume, which both
        // recompute, so it is safe to fold in. anyWeaponStyling_ is the PLAYER
        // weapon fast-path. A mannequin can carry the follower's weapon styles
        // too, although a class still needs matching real player equipment.
        const Outfit* o = suspended_ ? nullptr
                                     : playerMannequin_ ? &*playerMannequin_
                                     : (staged_ && stagedForPlayer_) ? &*staged_
                                                                     : library_.Active();
        anyWeaponStyling_.store(o && AnyWeaponEntry(*o), std::memory_order_release);
    }

    OutfitSession::WeaponDisplayEntry OutfitSession::WeaponDisplayFor(
        WeaponClass a_class, WeaponHand a_hand) const {
        SlotEntry entry;
        {
            std::scoped_lock l(lock_);
            const auto*      o = EffectiveLocked();
            if (!o) {
                return {};  // suspended, mid-scene, or no outfit at all
            }
            entry = o->ResolvedWeaponEntryFor(a_class, a_hand);
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
        bool hadMannequin = false;
        {
            std::scoped_lock l(lock_);
            const bool wasNpcStaging = staged_.has_value() && !stagedForPlayer_;
            hadMannequin             = playerMannequin_.has_value();

            stagedTarget_     = playerHandle;
            stagedForPlayer_  = true;
            stagedBaseFormID_ = 0;
            stagedNpcKey_.reset();
            staged_           = a_from;
            playerMannequinBase_.reset();
            playerMannequin_.reset();

            RecomputeWeaponStylingLocked();
            if (wasNpcStaging) {
                RebuildSnapshotLocked();  // drop a prior NPC override
            }
        }
        if (hadMannequin) {
            QueuePlayerPreviewEquipmentShow();
        }
        RequestRefresh();
    }

    void OutfitSession::BeginStaging(RE::ActorHandle a_target, const Outfit& a_from,
                                     const Outfit& a_playerPreview) {
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
            playerMannequinBase_ = forPlayer
                                       ? std::nullopt
                                       : std::optional<Outfit>{ a_playerPreview };
            playerMannequin_ = forPlayer
                                   ? std::nullopt
                                   : std::optional<Outfit>{ MakeMannequinPreview(
                                         ComposeMannequinSource(
                                             false, *playerMannequinBase_, a_from),
                                         blocklist_) };

            RecomputeWeaponStylingLocked();
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
            RequestRefresh();  // refresh the player mannequin too
        }
    }

    void OutfitSession::UpdateStaging(const Outfit& a_next) {
        bool            forPlayer = true;
        RE::ActorHandle target;  // captured under the lock for the NPC refresh below
        RefreshGate::StagedUpdate refresh = RefreshGate::StagedUpdate::kNone;
        {
            std::scoped_lock l(lock_);
            if (!staged_) {
                return;
            }
            refresh = RefreshGate::ClassifyStagedUpdate(
                BodyDiffers(*staged_, a_next),
                ChangedSlotCount(*staged_, a_next));
            staged_   = a_next;
            forPlayer = stagedForPlayer_;
            target    = stagedTarget_;
            if (!forPlayer) {
                const Outfit& base =
                    playerMannequinBase_ ? *playerMannequinBase_ : Outfit{};
                playerMannequin_ = MakeMannequinPreview(
                    ComposeMannequinSource(false, base, a_next), blocklist_);
            }
            RecomputeWeaponStylingLocked();
            if (!forPlayer) {
                RebuildSnapshotLocked();  // refresh the NPC override with the new outfit
            }
        }
        if (refresh == RefreshGate::StagedUpdate::kNone) {
            return;
        }
        if (refresh == RefreshGate::StagedUpdate::kBodyOnly) {
            // Body controls are actor-scoped OBody operations. They neither
            // need nor benefit from rebuilding every worn armor addon first.
            // For a follower, update both the follower and the player
            // mannequin; the latter is only a preview and remains actor-local.
            if (!forPlayer) {
                QueueNpcBodyStateAfterRefresh(target);
            }
            QueuePlayerBodyStateAfterRefresh();
            return;
        }
        // Symmetric self-refresh (see BeginStaging): each hover/click/Random
        // preview kicks the player or the staged follower so the edit shows on
        // the actor being dressed. Same cost shape as the player (one kick per
        // ~0.18s hover) - intentional, not debounced.
        if (forPlayer) {
            RequestRefresh();
        } else {
            RequestRefreshActor(target);
            RequestRefresh();
        }
    }

    void OutfitSession::CommitStaging() {
        bool            forPlayer = true;
        bool            restorePreviewEquipment = false;
        bool            hadMannequin = false;
        RE::ActorHandle oldTarget;
        {
            std::scoped_lock l(lock_);
            if (!staged_) {
                return;
            }
            forPlayer               = stagedForPlayer_;
            restorePreviewEquipment = presetPreviewSuppression_;
            hadMannequin             = playerMannequin_.has_value();
            oldTarget               = stagedTarget_;
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
            presetPreviewSuppression_ = false;
            stagedNpcKey_.reset();
            stagedBaseFormID_ = 0;
            stagedForPlayer_  = true;
            stagedTarget_     = {};  // reset the staging fields as a unit
            playerMannequinBase_.reset();
            playerMannequin_.reset();
            RecomputeWeaponStylingLocked();
            if (!forPlayer) {
                RebuildSnapshotLocked();  // reflect the committed NPC outfit, drop the override
            }
        }
        if (restorePreviewEquipment) {
            BipedPost::QueueObjectNodeShow(
                oldTarget, PresetPreviewPolicy::kSuppressedBipedObjects);
        }
        if (hadMannequin) {
            QueuePlayerPreviewEquipmentShow();
        }
        if (forPlayer) {
            Persistence::QueueLibrarySave();  // commit mutates the GLOBAL player library
            RequestRefresh();
        } else {
            RequestRefresh();  // restore the player's saved outfit
        }
    }

    void OutfitSession::DiscardStaging() {
        bool            forPlayer = true;
        bool            hadNpcStage;
        bool            hadMannequin;
        bool            restorePreviewEquipment = false;
        RE::ActorHandle oldTarget;  // captured BEFORE the reset for the NPC revert
        {
            std::scoped_lock l(lock_);
            forPlayer   = stagedForPlayer_;
            hadNpcStage = staged_.has_value() && !stagedForPlayer_;
            hadMannequin = playerMannequin_.has_value();
            restorePreviewEquipment = presetPreviewSuppression_;
            oldTarget   = stagedTarget_;
            staged_.reset();
            presetPreviewSuppression_ = false;
            stagedNpcKey_.reset();
            stagedBaseFormID_ = 0;
            stagedForPlayer_  = true;
            stagedTarget_     = {};  // reset the staging fields as a unit
            playerMannequinBase_.reset();
            playerMannequin_.reset();
            RecomputeWeaponStylingLocked();
            if (hadNpcStage) {
                RebuildSnapshotLocked();  // drop the NPC preview override
            }
        }
        if (restorePreviewEquipment) {
            BipedPost::QueueObjectNodeShow(
                oldTarget, PresetPreviewPolicy::kSuppressedBipedObjects);
        }
        if (hadMannequin) {
            QueuePlayerPreviewEquipmentShow();
        }
        // Symmetric self-refresh (see BeginStaging): a player discard reverts the
        // player, an NPC discard kicks the follower so its preview reverts NOW -
        // when the editor switches away or closes without Apply - instead of
        // lingering until the actor's next natural rebuild.
        if (forPlayer || hadMannequin) {
            RequestRefresh();
        }
        if (hadNpcStage) {
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

    void OutfitSession::SetPresetPreviewSuppression(bool a_enabled) {
        bool            forPlayer = true;
        bool            restorePreviewEquipment = false;
        RE::ActorHandle target;
        {
            std::scoped_lock l(lock_);
            const bool next = a_enabled && staged_.has_value();
            if (presetPreviewSuppression_ == next) {
                return;
            }
            restorePreviewEquipment     = presetPreviewSuppression_ && !next;
            presetPreviewSuppression_ = next;
            forPlayer                 = stagedForPlayer_;
            target                    = stagedTarget_;
        }
        if (restorePreviewEquipment) {
            BipedPost::QueueObjectNodeShow(
                target, PresetPreviewPolicy::kSuppressedBipedObjects);
        }
        if (forPlayer) {
            RequestRefresh();
        } else {
            RequestRefreshActor(target);
            RequestRefresh();
        }
    }

    std::uint64_t OutfitSession::PreviewEquipmentSuppressionMask(
        RE::Actor* a_actor) const {
        if (!a_actor) {
            return 0;
        }
        auto* const player      = RE::PlayerCharacter::GetSingleton();
        const bool  actorPlayer = player && a_actor == player;
        std::uint32_t actorBase = 0;
        if (!actorPlayer) {
            if (auto* base = a_actor->GetActorBase()) {
                actorBase = base->GetFormID();
            }
        }
        std::scoped_lock l(lock_);
        if (presetPreviewSuppression_ && staged_) {
            const bool target =
                stagedForPlayer_ ? actorPlayer
                                 : (!actorPlayer && actorBase != 0 &&
                                    actorBase == stagedBaseFormID_);
            // Preset browsing keeps its established clean silhouette on both
            // the edited follower and the player mannequin.
            if (target || (actorPlayer && playerMannequin_)) {
                return PresetPreviewPolicy::kSuppressedBipedObjects;
            }
        }
        if (actorPlayer && playerMannequin_) {
            // Ordinary follower editing previews the follower's styled weapon
            // classes on the player while hiding unrelated player equipment.
            return PresetPreviewPolicy::MannequinSuppressedBipedObjects(
                *playerMannequin_);
        }
        return 0;
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
        bool hadMannequin = false;
        {
            std::scoped_lock l(lock_);
            hadMannequin     = playerMannequin_.has_value();
            library_          = std::move(a_lib);
            staged_.reset();
            presetPreviewSuppression_ = false;
            stagedNpcKey_.reset();
            stagedBaseFormID_ = 0;
            stagedForPlayer_  = true;
            stagedTarget_     = {};  // reset the staging fields as a unit
            playerMannequin_.reset();
            suspended_        = false;
            RecomputeWeaponStylingLocked();
            RebuildSnapshotLocked();  // drop any stale staged NPC override
        }
        if (hadMannequin) {
            QueuePlayerPreviewEquipmentShow();
        }
        RequestRefresh();
    }

    void OutfitSession::OnRevert() {
        // The player library is GLOBAL (outfits.json) - it survives save
        // boundaries. Only per-save PLAYER state resets: the active selection
        // and any staging. NPC assignments are per-save too, but reset through
        // their own OnNpcRevert (a separate co-save callback).
        bool hadMannequin = false;
        {
            std::scoped_lock l(lock_);
            hadMannequin = playerMannequin_.has_value();
            library_.Deactivate();
            staged_.reset();
            presetPreviewSuppression_ = false;
            stagedNpcKey_.reset();
            stagedBaseFormID_ = 0;
            stagedForPlayer_  = true;
            stagedTarget_     = {};  // reset the staging fields as a unit
            playerMannequin_.reset();
            suspended_        = false;
            RecomputeWeaponStylingLocked();
            RebuildSnapshotLocked();
        }
        if (hadMannequin) {
            QueuePlayerPreviewEquipmentShow();
        }
    }

    // ---- Actor dimension (NPC/follower assignments) ------------------------

    void OutfitSession::UpsertNpcLibrary(const NpcKey& a_key, OutfitLibrary a_library) {
        std::scoped_lock l(lock_);
        // Preserve the actor's captured OBody baseline across every structural
        // library edit (rename/add/delete/Apply).
        npcAssignments_[a_key].library = std::move(a_library);
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

        // Weapon classes: resolve the effective Both/Right/Left entries.
        // Optional hand overrides have already inherited Both at this point;
        // a kStyle whose plugin is gone collapses to passthrough.
        for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
            const auto wc = static_cast<WeaponClass>(c);
            for (std::size_t h = 0; h < kWeaponHandCount; ++h) {
                const auto hand = static_cast<WeaponHand>(h);
                const auto& e   = a_outfit.ResolvedWeaponEntryFor(wc, hand);
                if (e.kind == SlotEntry::Kind::kHide) {
                    rd.weapons[c][h] =
                        ResolvedNpcWeapon{ SlotEntry::Kind::kHide, nullptr };
                } else if (e.kind == SlotEntry::Kind::kStyle) {
                    if (auto* form = ResolveWeaponStyleForm(wc, e.style)) {
                        rd.weapons[c][h] =
                            ResolvedNpcWeapon{ SlotEntry::Kind::kStyle, form };
                    }
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
        if (g_playerRefreshQueued.exchange(true, std::memory_order_acq_rel)) {
            return;  // latest staged state will be read by the queued refresh
        }
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask(RunPlayerRefreshWhenSafe);
        } else {
            g_playerRefreshQueued.store(false, std::memory_order_release);
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
                // The body morph must target the rebuilt nodes, not the ones
                // Update3DModel is replacing. Re-resolve the latest staged
                // follower state on the next task pass.
                QueueNpcBodyStateAfterRefresh(a_actor);
            } catch (const std::exception& e) {
                spdlog::error("RefreshActor threw: {}", e.what());
            } catch (...) {
                spdlog::error("RefreshActor threw a non-standard exception.");
            }
        });
    }

    void OutfitSession::RequestRefreshLoadedNpcs() {
        const auto assignments = GetSingleton().SnapshotNpcAssignments();
        if (assignments.empty()) {
            return;
        }
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        task->AddTask([assignments] {
            auto* processLists = RE::ProcessLists::GetSingleton();
            if (!processLists) {
                return;
            }
            processLists->ForEachHighActor(
                [&](RE::Actor& a_actor) -> RE::BSContainer::ForEachResult {
                    auto* base = a_actor.GetActorBase();
                    if (!base || base->IsDynamicForm()) {
                        return RE::BSContainer::ForEachResult::kContinue;
                    }
                    NpcKey key;
                    if (auto* file = base->GetFile(0)) {
                        key.modName = std::string{ file->GetFilename() };
                    }
                    key.localFormID = base->GetLocalFormID();
                    if (assignments.contains(key)) {
                        RequestRefreshActor(a_actor.GetHandle());
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
        });
    }

}  // namespace OS
