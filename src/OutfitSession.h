#pragma once

#include "PCH.h"

#include "NpcAssignments.h"
#include "NpcResolve.h"
#include "Outfit.h"
#include "Persistence.h"
#include "RuleModel.h"

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
    // The bit is retained so the hook can filter it against the display mask
    // without re-resolving the form.
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
        // limits HIDE to the actor's real worn coverage per rebuild via
        // NpcResolve::WornRequiredDisplay. Styles remain visual choices and
        // may fill an otherwise unworn slot, as on the player.
        DisplaySet                                       display;
        std::vector<ResolvedNpcStyle>                    styles;   // style bits only, bit order
        std::array<std::array<ResolvedNpcWeapon, kWeaponHandCount>,
                   kWeaponClassCount> weapons{};
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

        // The body settings the current display implies (OBody).
        //
        // ⚠ SAME STAGED-ELSE-ACTIVE RULE AS Display() ABOVE, and for the same
        // reason: "a live preview is not a second mechanism". Reading the
        // library's ACTIVE outfit instead is what made a body preset apply
        // only on Apply while every style previewed instantly - the staged
        // outfit is the one being shown, so it is the one the body must follow.
        struct BodyDisplay {
            std::string preset;                            // empty == revert to baseline
            std::string customPresetId;                    // stable Body Studio reference
            ORefitMode  orefit{ ORefitMode::kDefault };
            PushUpMode  pushUp{ PushUpMode::kNone };
            std::uint32_t torsoStyleMask{ 0 };             // for transmog-aware Auto
            std::uint32_t torsoHideMask{ 0 };
            bool        drives{ false };                   // false == do not touch the body
            bool        restoreBaseline{ false };          // Equipped: undo a prior preview
        };
        [[nodiscard]] BodyDisplay DisplayBody() const;

        struct NpcBodyDisplay : BodyDisplay {
            NpcKey      key;
            std::string baseline;
            bool        baselineCaptured{ false };
        };

        // Actor-scoped body state for the NPC refresh task. Equipped gear with
        // a captured baseline still drives one restoration; otherwise an
        // inactive library merely releases ORefit enforcement.
        [[nodiscard]] NpcBodyDisplay DisplayBodyForNpc(RE::Actor* a_actor) const;
        void CaptureNpcBodyBaseline(const NpcKey& a_key, std::string a_preset);

        // Actor-scoped HAIR COLOUR state for the NPC refresh task, the follower
        // counterpart to DisplayBodyForNpc above and resolved by the same
        // staged-override-else-assigned-active rule.
        //
        // Three states, not two, because hair colour is written into persistent
        // actor-base state and the wrong default is destructive:
        //   manages == false            -> NOT Fitting Room's actor. Touch nothing.
        //   manages, outfit present     -> apply that outfit's tint.
        //   manages, outfit == nullopt  -> Equipped gear: restore their own colour.
        // The player always leaves as manages == false; see the implementation
        // for why that is load-bearing rather than merely tidy.
        struct NpcHairState {
            bool                  manages{ false };
            NpcKey                key;
            std::optional<Outfit> outfit;
            // The follower's pre-Fitting-Room colour as persisted in their NPCO
            // record, to seed before the first apply of the session.
            std::string           baselineMod;
            std::uint32_t         baselineLocalID{ 0 };
            bool                  baselineCaptured{ false };
        };
        [[nodiscard]] NpcHairState HairStateForNpc(RE::Actor* a_actor) const;

        // The outfit a_actor is currently SHOWING, or nullopt when Fitting Room
        // is showing them nothing. The seam for anything that has to re-apply a
        // purely visual property after a rebuild and has no actor base to read
        // it back from; OutfitDye is the first such consumer.
        //
        // ⚠ PLAYER ONLY, and that is a scope decision rather than a limitation
        // of this function. Dye is player-only in tier 1, and answering for a
        // follower here would hand REAug::RefreshActor an outfit to paint on
        // every assigned NPC in the cell. Followers resolve through
        // npcAssignments_ exactly as HairStateForNpc does, so extending this is
        // a matter of copying that function's staged-else-assigned-active rule,
        // not of rewriting the seam.
        //
        // ⚠ Returns a COPY, not the const Outfit* EffectiveLocked hands out
        // internally. The caller runs on the GAME thread while the editor
        // mutates library_ and staged_ from the FUCK present thread, so a
        // pointer into either one outlives the lock that made it valid. One
        // Outfit copy per refresh is the price of that being safe by
        // construction instead of by timing.
        [[nodiscard]] std::optional<Outfit> ActiveOutfitFor(RE::Actor* a_actor) const;

        // True while the PLAYER's effective outfit carries a hair colour, i.e.
        // while Fitting Room owns what is painted on their hair. The gate for
        // HeadEditorSink; see RefreshGate::ShouldReassertAfterHeadEditor for
        // why answering "no" has to mean "leave the hair completely alone".
        [[nodiscard]] bool DrivesPlayerHairTint() const;

        // Re-push the player's outfit hair colour and queue the rebuild that
        // carries it, for when something outside Fitting Room has repainted the
        // hair from the actor base - the head editor closing, and the load
        // boundary.
        //
        // Deliberately routed through the SAME push the staging paths use
        // rather than calling HairColor::Repaint directly. The editor rebuilds
        // the head from the base, so re-tinting only the geometry would be
        // undone by the actor's next natural rebuild; the base write is what
        // makes it stick. It also means Apply's recapture test runs, so a
        // colour the user genuinely changed in the editor becomes the new
        // baseline they revert to when the outfit comes off, instead of being
        // silently reverted to a pre-editor one.
        //
        // a_atLoadBoundary changes ONE branch: when nothing names a colour,
        // the head-editor caller must do nothing (pushing would Restore over
        // the colour the editor just set), but the load boundary is exactly
        // where "nothing names a colour while the base still wears OUR
        // colour" means an earlier save got contaminated by the abandoned
        // session - so that caller restores the captured original instead,
        // guarded by HairColor::RestoreIfStillOurs's lastApplied test.
        void ReassertPlayerHairColor(bool a_atLoadBoundary = false);

        // ⚠⚠ THE ONE PAINTER OF THE PLAYER'S LOOK FOR AN OUTFIT CHANGE MADE
        // OUTSIDE STAGING (OS-229). Hair colour, hair style, eyes, brows,
        // facial hair and every discovered slot, through the same two pushes
        // the staging paths run, in the same order, from the same function. The
        // quick-switch hotkey stated the hair COLOUR by hand and nothing else,
        // because colour was the only look dimension when it was written, so
        // for every dimension added since it changed the outfit and left the
        // head as it was until the editor opened. Call from OUTSIDE lock_ and
        // BEFORE the refresh that carries the change; null is "no outfit", and
        // the ladders inside answer that with the character's defaults.
        void PushPlayerLookFor(const Outfit* a_outfit);

        // True while the PLAYER's effective outfit names an eye or brow type,
        // i.e. while Fitting Room owns part of their face. The gate for the head
        // editor handoff; see RefreshGate::ClassifyHeadPartHandoff for why
        // answering "no" has to mean "leave the face completely alone".
        [[nodiscard]] bool DrivesPlayerHeadParts() const;

        // Hand the character back to the editor: put their own eyes and brows on
        // and DROP the capture, so what they leave RaceMenu wearing becomes the
        // new baseline rather than being reverted to a pre-editor one later.
        void StandDownPlayerHeadParts();

        // Take over again on the way out, through the same push an outfit switch
        // uses so the recapture runs.
        void ReassertPlayerHeadParts();

        // Which of the five look dimensions the PLAYER wears because of a
        // character DEFAULT right now.
        //
        // ⚠⚠ THE LADDER ANSWERS THIS, NOT DefaultLook::Has, and the distinction
        // is the difference between "bought" and "worn". A stored default can be
        // dormant: the outfit outranks it, or this character is not offered that
        // part any more. HeadEditorSink reads this at the OPEN edge of a
        // character-editor visit, and the bit decides whether a change made in
        // there is evidence about THIS default or about something the default
        // was not authoring at all - RefreshGate::DefaultVerdict::kDormant is
        // the arm it feeds, and getting it wrong deletes something the player
        // paid for.
        //
        // Routed through HeadPartLadder::Choose and the same rung the push
        // uses, so the ladder stays the only reader of its own precedence.
        struct LookAuthorship {
            bool hair{ false };
            bool eyes{ false };
            bool brows{ false };
            bool facialHair{ false };
            bool hairColour{ false };
        };
        [[nodiscard]] LookAuthorship DefaultAuthorship() const;

        // Record a follower's pre-Fitting-Room hair colour so it survives the
        // save. Write-once per actor, exactly like CaptureNpcBodyBaseline: the
        // first capture is the truth about what they looked like before we
        // touched them, and a later one would be describing our own work.
        void CaptureNpcHairBaseline(const NpcKey& a_key, std::string a_mod,
                                    std::uint32_t a_localFormID);

        // Drop it again, after a restore has put the follower's own colour back.
        //
        // ⚠ Not optional bookkeeping. NPCO encodes the baseline triple for every
        // entry unconditionally, so unlike the player's 'HCOL' record - which is
        // simply not written when there is nothing captured - a follower's stale
        // baseline WOULD persist. Leave one behind, let the user then restyle
        // that follower's hair in RaceMenu, and the next restore paints the
        // pre-RaceMenu colour back over their edit.
        void ForgetNpcHairBaseline(const NpcKey& a_key);

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
        [[nodiscard]] WeaponDisplayEntry WeaponDisplayFor(
            WeaponClass a_class, WeaponHand a_hand = WeaponHand::Both) const;

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
        // a_playerPreview is the follower's live worn gear, used as the base
        // the transient player mannequin overlays onto. NULL means "she is on
        // screen, do not build one", and that is the whole switch: no preview
        // source, no mannequin, and UpdateStaging stays consistent because it
        // rebuilds only when this call left a base behind.
        void BeginStaging(RE::ActorHandle a_target, const Outfit& a_from,
                          const Outfit* a_playerPreview);
        void UpdateStaging(const Outfit& a_next);
        void CommitStaging();   // player: staged -> active outfit; NPC: upsert into the base's library
        void DiscardStaging();
        [[nodiscard]] bool IsStaging() const;

        // Who staging currently targets (nullopt when not staging). The player
        // handle for the player path; an NPC's handle otherwise. Editor reads
        // this to route Apply / footer text.
        [[nodiscard]] std::optional<RE::ActorHandle> StagingTarget() const;

        // Preset browsing and follower mannequins transiently hide weapon,
        // shield and quiver render objects that would otherwise show unrelated
        // real equipment through the preview. Never persisted.
        void SetPresetPreviewSuppression(bool a_enabled);
        [[nodiscard]] std::uint64_t PreviewEquipmentSuppressionMask(
            RE::Actor* a_actor) const;

        // The body page's bare view: while it is on, the edit target renders
        // with neither their real gear nor this mod's styles, so a body preset
        // can be judged on the body rather than through an outfit. Transient
        // and never persisted, exactly like the suppression above.
        //
        // ⚠ THE EDITOR RE-ASSERTS THIS EVERY FRAME IT DRAWS THE PAGE, which is
        // what makes the state safe to hold here at all. There is no single
        // "left the body page" event to hook - the list appears whenever the
        // body slot row is selected and that selection is cleared from five
        // places - so the page's presence IS the flag, and the one case a draw
        // loop cannot cover (the editor no longer drawing) is closed by
        // EditorUI::OnClose.
        //
        // ⚠⚠ A STAGED OUTFIT IS NOT REQUIRED AND USED TO BE. The flag was
        // `a_enabled && staged_.has_value()`, which was invisible until the
        // button moved onto every page: the Rules page hands the character back
        // to the rules engine by DISCARDING staging, so the press flipped a
        // bool that was ANDed away one line later and nothing happened, with no
        // log line either because the setter early-returns on an unchanged
        // value (field 2026-08-19, "button is there but does nothing"). The
        // mask itself never needed staging; OS-233's reconcile pass has been
        // answering the same question without one since the day it landed.
        //
        // a_playerSubject is who the editor is looking at, and it is only read
        // when nothing is staged. With staging the staged target decides, as
        // before. Without it, the ONLY actor a bare view may undress is the
        // player, and only while the player is the one being edited: answering
        // for anyone else would strip a follower the editor is not pointed at.
        void SetBareBodyPreview(bool a_enabled, bool a_playerSubject);
        // Which of a_wornCoverage to strip off this actor this pass. NULLOPT
        // means the view is off or this is not the actor being edited; a value
        // means it IS, and that value may legitimately be zero.
        //
        // ⚠⚠ NULLOPT AND ZERO ARE DIFFERENT ANSWERS. Returning a plain 0 for
        // both shipped a feature that did nothing on exactly the character it
        // matters most on: one wearing no real armour, whose look is entirely
        // our styles. See BareBodyDisplay for the measured field trace.
        //
        // ⚠ TAKES MEASURED COVERAGE AND RETURNS A SUBSET OF IT. A hide bit on
        // a slot holding no worn gear culls the race skin's own node. The
        // blocklist and the shield are removed here rather than at the call
        // site so every consumer inherits both.
        [[nodiscard]] std::optional<std::uint32_t> BareBodyHideMask(
            RE::Actor* a_actor, std::uint32_t a_wornCoverage) const;

        // OS-233: put RaceMenu's body overlays back on the player by running the
        // two passes the Hide outfit toggle runs, back to back inside ONE task,
        // so no frame is presented between them. The first pass answers the worn
        // pass exactly as the bare view does (styles off, real gear hidden, so
        // the skin body attaches and skee's attach-time build hangs the clones
        // on it); the second is a normal pass, which re-attaches the styled top
        // and skee reverts and rebuilds on that. Why this and not one of skee's
        // public calls is on OverlayReconcile.h. Any thread; the work is queued.
        static void ReconcilePlayerOverlays();

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

        // Apply a rules-engine decision: base activation plus the overlay.
        // a_outfitName empty with a_realGear false means "leave the active
        // outfit alone" (the kKeep base); a_realGear true deactivates.
        // Returns false when a_outfitName names an outfit this save's library
        // does not have, having changed NOTHING: a rule referencing a deleted
        // outfit must be inert, never a strip to real gear.
        bool ApplyRuleDecision(bool a_realGear, std::string_view a_outfitName,
                               Rules::Overlay a_overlay);

        // Drop the rule overlay without touching the active outfit
        // selection. Does not refresh - same contract as SetBlocklist, and
        // for the same kind of reason: OnRevert calls this as one reset
        // among several that intentionally do not queue a mid-load refresh.
        // A rule's overlay must not outlive the state that put it there:
        // OnRevert calls this because a fresh save carries no rule state;
        // WorldWatch::NotifyManualPick and SetEngineEnabled's disable edge
        // call it because kPaused/disabled never run ApplyRuleDecision, so
        // without an explicit clear the last rule's overlay would stay
        // composed over whatever the user chose instead; and
        // WorldWatch::NotifyOutfitDeleted calls it because a rule's
        // orphaned overlay must not keep composing once the outfit that
        // produced it is gone.
        void ClearRuleOverlay();

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

        // Reapply loaded follower appearance/body state after an external API
        // readiness cycle (notably OBody going unready around saves).
        static void RequestRefreshLoadedNpcs();

        // ---- push-up, after the rebuild that clears it ------------------------
        //
        // ⚠⚠ OBODY CLEARS OUR MORPH KEY AND THE FIELD MEASURED IT UNANIMOUSLY.
        // 2026-08-27, twenty-nine probe reads and twenty-nine of them GONE:
        //
        //   01:01:19.070  PushUp: 4 morph(s) written for mode 2, refreshed=true
        //   01:01:19.111  mod event 'Obody_ApplyMorph'
        //   01:01:19.130  Body Studio proof: ... ORefitApplied=true
        //   01:01:19.626  PushUp probe: our morph key is GONE
        //
        // Forty one milliseconds after our write the body is rebuilt, and the
        // key is gone half a second later. ApplyPushUpFor already sits at the
        // END of ApplyPlayerBodyState, but the paths above it QUEUE their work,
        // so being written last in source order is not being written last. The
        // user's own workaround is the same shape from the other side: equipping
        // and unequipping torso gear makes the lift appear, because that puts a
        // write after the rebuild rather than before it.
        //
        // So the rebuild announces itself and the lift goes back on afterwards.
        static void NoteBodyRebuilt();

        // Run the armed re-assert if it is due. Called from the world tick, and
        // silent unless a rebuild armed it. ⚠ A NO-OP WHEN THE KEY SURVIVED,
        // which is what keeps a rebuild that did not clear anything from costing
        // a write, and what makes this safe against ever chasing its own tail.
        static void RunPushUpReassert();

    private:
        OutfitSession() = default;

        // Call under lock_. The returned pointer is valid only until lock_
        // is released OR EffectiveLocked() is called again - a second call
        // within the same locked scope can overwrite composed_, the shared
        // scratch storage a prior return may point into. Copy out what you
        // need (or finish using the pointer) before either happens.
        [[nodiscard]] const Outfit* EffectiveLocked() const;

        // Index of the outfit named a_name in library_, or -1 when a_name
        // is empty or matches nothing. Call under lock_.
        [[nodiscard]] int FindOutfitIndexLocked(std::string_view a_name) const;

        // Deactivate, then activate a_name if FindOutfitIndexLocked finds
        // it. Call under lock_; recomputes the weapon-styling flag but does
        // NOT refresh, so a caller doing more than one thing under the same
        // acquisition (ApplyRuleDecision writing the overlay AND activating
        // a base in one go) can queue exactly one RequestRefresh() after
        // the whole mutation lands, instead of splitting it across two
        // acquisitions and letting a reader observe the new overlay
        // composed over the OLD base mid-way.
        void ActivateByNameLocked(std::string_view a_name);

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
        bool                  presetPreviewSuppression_{ false };
        bool                  bareBodyPreview_{ false };
        // Who the editor is on, read ONLY when nothing is staged. See
        // SetBareBodyPreview for why that case exists and why it is player-only.
        bool                  bareBodySubjectIsPlayer_{ true };
        // OS-233: true for exactly the first of ReconcilePlayerOverlays' two
        // passes. BareBodyHideMask answers for the player as if the bare view
        // were on, no staged session needed. Under lock_.
        bool                  overlayReconcilePass_{ false };
        std::atomic<bool>     anyWeaponStyling_{ false };  // see AnyWeaponStyling()

        // The rule overlay currently in force on the PLAYER. Derived state: a
        // rule owns it, it is never persisted, and it composes over the active
        // outfit (Rules::Compose). Empty when no rule with an overlay applies.
        Rules::Overlay ruleOverlay_;
        // Scratch storage for the composed result. EffectiveLocked()
        // recomposes into this on EVERY read that has a non-empty overlay -
        // there is no cache, and none is needed (single-digit microseconds,
        // on a path that fires on equipment changes, not per frame). Held
        // as storage only so EffectiveLocked() can keep returning a
        // `const Outfit*` and every existing caller is unchanged; see its
        // declaration for the pointer-lifetime contract that follows from
        // reusing one buffer.
        mutable Outfit composed_;

        // Staging target. When stagedForPlayer_ is true, staged_ drives the
        // player channel exactly as before. When false, staged_ overrides the
        // NPC snapshot while playerMannequin_ transiently drives the player
        // viewport with the same styles over a naked visual baseline. The
        // mannequin is never inserted into either library.
        bool                  stagedForPlayer_{ true };
        RE::ActorHandle       stagedTarget_;          // player handle for the player path
        std::optional<NpcKey> stagedNpcKey_;          // set iff staging an NPC (for CommitStaging upsert)
        std::uint32_t         stagedBaseFormID_{ 0 };  // resolved base formID of an NPC target (0 = player/none)
        // Both empty whenever the follower is visible in her own right, which
        // is the normal case with Menu Studio installed. playerMannequinBase_
        // doubles as the "a mannequin is wanted" flag: BeginStaging sets it
        // only when the caller passed a preview source, and UpdateStaging
        // refuses to rebuild without it, so the two paths cannot disagree about
        // whether this session has a mannequin.
        std::optional<Outfit> playerMannequinBase_;    // captured follower gear under mutable edits
        std::optional<Outfit> playerMannequin_;       // render-only; never persisted

        // The follower half of ActiveOutfitFor. Split out so the public method
        // stays a two-line dispatch and the NPC path takes lock_ exactly once,
        // after the pre-lock engine reads that ClassifyTarget performs.
        [[nodiscard]] std::optional<Outfit> ActiveOutfitForNpc(RE::Actor* a_actor) const;

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
