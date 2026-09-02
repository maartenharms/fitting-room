#include "OutfitSession.h"

#if defined(FR_BODY_STUDIO)
#include "BodyMeshPath.h"      // BuiltBodyMeshKey: which body is actually worn
#include "BodyMorphPlan.h"     // BuildPushUpMorphPlan, the per-family recipe
#include "BodyPresetStore.h"
#include "BodySlideCatalog.h"  // FamilyForMesh, the runtime half of the recipe
#include "RaceMenuMorphApi.h"  // ApplyPushUp, and its own morph key
#include "BodyStudioProof.h"
#include "DefaultBody.h"
#endif

#include "BipedPost.h"
#include "CbpcArmorClass.h"  // r55: the shown chest class rides the worn form
// ⚠ NOT under the FR_BODY_STUDIO guard the default BODY sits behind. A default
// body is Body Studio's, because that channel is where a body preset is
// authored; a default hairstyle is not, and hair, eyes and brows ship on every
// channel.
#include "DefaultLook.h"
#include "HairColor.h"
#include "HairStyle.h"
#include "HeadPart.h"
#include "HeadPartLadder.h"  // the precedence, pure and pinned
#include "NpcHair.h"
#include "NpcHairPlan.h"
#include "NpcIdentity.h"
#include "ObodyApi.h"
#include "OverlayReconcile.h"  // OS-233: CountPlayerBodyClones, the rebuild's own result line
#include "PresetPreviewPolicy.h"
#include "ProfileApply.h"  // InFlight: the close-path re-assert stands down mid-apply
#include "REAugments.h"
#include "RefreshGate.h"
#include "SceneGuard.h"
#include "Settings.h"
#include "StyleRef.h"

#include <atomic>
#include <chrono>

namespace OS {

    namespace {
        // The "no active outfit" base for Rules::Compose - a real Outfit
        // rather than a temporary constructed at every overlay-only compose,
        // which Compose would then copy again into its own local.
        const Outfit kEmptyOutfit{};

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
                // A base with no defining file yields no key, so the target
                // classifies exactly like an unresolvable handle: inert. Every
                // caller already gates on tc.key (DisplayBodyForNpc returns an
                // empty display, BeginStaging stages no NPC key), so this is
                // the same "no persistent identity" rule the teammate picker
                // and RequestRefreshLoadedNpcs apply - it was only THIS site
                // that reached GetLocalFormID unguarded. See NpcIdentity.h.
                tc.key = NpcKeyFor(base);
            }
            return tc;
        }

        // One rung of the head-part ladder: does this key still resolve to a
        // part, and is that part one this character may actually wear?
        //
        // ⚠⚠ ONE READER FOR THREE CALLERS, AND THE THIRD IS WHY IT MOVED HERE.
        // The hair push and the eye/brow/beard push each carried their own copy
        // of these ten lines, which was survivable while both only ever DECIDED
        // what to wear. DefaultAuthorship asks the same question to decide
        // whether a paid-for default gets DELETED, and a fourth copy answering
        // it half a degree differently would destroy a purchase - so there is
        // one of them and all three inherit it.
        //
        // a_quiet suppresses the warning for the reader that is only observing:
        // the pushes report an unresolvable key once each, and the authorship
        // read runs on the same stack moments earlier.
        std::pair<bool, bool> ResolveRung(RE::Actor* a_actor, HeadPart::Kind a_kind,
                                          const StyleRefKey& a_key,
                                          RE::BGSHeadPart*& a_out, bool a_quiet) {
            a_out = nullptr;
            if (a_key.Empty()) {
                return { false, false };  // names nothing
            }
            auto* const dh = RE::TESDataHandler::GetSingleton();
            auto* const part =
                dh ? dh->LookupForm<RE::BGSHeadPart>(a_key.localFormID, a_key.modName)
                   : nullptr;
            if (!part) {
                if (!a_quiet) {
                    spdlog::warn("HeadPart: {} '{}'|{:06X} no longer resolves; leaving "
                                 "the current one alone.",
                                 HeadPart::KindName(a_kind), a_key.modName,
                                 a_key.localFormID);
                }
                return { false, false };
            }
            a_out = part;
            return { true, HeadPart::IsValidFor(a_actor, a_kind, part) };
        }

        // Push the hair colour a_outfit implies onto the PLAYER. a_outfit null
        // means "no outfit at all" (Base gear), which restores their own
        // colour; a DISABLED tint needs no branch here because Apply restores
        // internally.
        //
        // Hair colour is PUSHED into the actor base. Hair VISIBILITY, the
        // feature it shares a UI section with, is PULLED - the worn-mask shim
        // reads it out of the display set on every render pass, so it needs no
        // trigger at all. A colour has no such reader, which is why EVERY path
        // that changes which outfit an actor is showing has to state it, and why
        // stating it is not optional on a path that only requests a refresh.
        //
        // ⚠ PLAYER ONLY. Followers are pushed from PushNpcHairColor, inside
        // RequestRefreshActor's task, and deliberately not from here as well -
        // see that function for why two writers on one actor breaks the
        // recapture test. The asymmetry is not an oversight: the player channel
        // pushes at the CHOICE points because a load-time push would trip the
        // 'ACTV'-before-'HCOL' ordering trap, while a follower has no such
        // constraint precisely because its seed and its apply are colocated.
        //
        // ⚠ Order: colour FIRST, refresh SECOND. That is what the 2026-07-30
        // spike proved renders live inside the paused editor; reversed, the
        // rebuild carries the OLD colour and the edit shows up one refresh late.
        //
        // ⚠ Call from OUTSIDE lock_, alongside the refresh requests. SetHairColor
        // touches engine state and lock_ only guards this session's own fields.
        void PushPlayerHairColor(const Outfit* a_outfit) {
            auto* const player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;
            }
            // ⚠ THROUGH THE LADDER, INCLUDING THE NO-OUTFIT CASE. Restore here
            // would take a character's default colour off the moment they wore
            // nothing this mod manages, which is the same "zero read as nothing
            // to do" shape the bare-body mask was fixed for: no outfit is a
            // state the default is FOR, not a state to skip.
            HairColor::Push(player, a_outfit);

            // Hair STYLE rides the same push, and PLAYER ONLY for the reason
            // HeadPart.h gives: this route rebuilds the head to put the change
            // on screen, and that rebuild hands a named NPC a complexion no
            // repaint can undo. Followers go through NpcHair's attach instead.
            //
            // ⚠ NOT because a follower's record cannot be reached. That was the
            // reasoning here until 2026-07-31 and it was measured wrong; only
            // her FACE is baked, and ChangeHeadPart does reach her hair.
            //
            // ⚠ Resolved here rather than stored as a pointer. The outfit holds
            // a mod+id pair precisely so a plugin leaving the load order makes
            // this fail to resolve and do nothing, instead of dereferencing a
            // form that is gone.
            // ⚠ THE CHARACTER'S DEFAULT SITS BETWEEN THE OUTFIT AND THE
            // RESTORE. Same three-step rule the eye and brow push uses: the
            // outfit wins because it is the more specific statement, an outfit
            // naming no hair falls to what the player said this character
            // normally wears, and only a character with no default goes back to
            // their own record.
            // ⚠⚠ THROUGH THE LADDER, AND HAIR WAS THE KIND THAT NEVER WAS. Eyes
            // and brows were moved onto HeadPartLadder::Choose on 2026-08-09 for
            // exactly the fault the field then reported on hair: a rung was
            // taken on RESOLVING alone, so a default naming a part this
            // character's race and sex would not be offered was applied anyway.
            // The engine does not police it, ChangeHeadPart matches on head-part
            // type and reads neither the sex flags nor the race list, so a
            // woman's saved default hair landed on the man RaceMenu had just
            // made of her and stayed there (user 2026-08-11).
            //
            // ⚠ THE KEY IS DELIBERATELY NOT THE FIX. DefaultLook keys by actor
            // base and a base carries no sex, which reads like the third
            // outing of "a cache key must be the inputs, not the subject" - but
            // this is not a cache. It is a stored PURCHASE, and putting sex in
            // its key would detach every default already paid for in every
            // existing save. The ladder answers the same question at the point
            // of use, which is why the eye and brow half was built this way,
            // and it costs nothing: the default is KEPT and comes back the
            // moment the character suits it again.
            const auto hairDefault = DefaultLook::For(player, DefaultLook::Part::kHair);
            // The shared rung above, which the eye/brow push and the authorship
            // read use too. It used to be a lambda of its own here, on the
            // reasoning that "this half speaks HairStyle" - and HairStyle is
            // HeadPart::Kind::kHair, so the two copies were answering one
            // question in two places.
            RE::BGSHeadPart* outfitHair  = nullptr;
            RE::BGSHeadPart* defaultHair = nullptr;
            const auto [oNames, oUsable] =
                ResolveRung(player, HeadPart::Kind::kHair,
                            a_outfit ? a_outfit->hairStyle : StyleRefKey{}, outfitHair,
                            false);
            const auto [dNames, dUsable] =
                ResolveRung(player, HeadPart::Kind::kHair, hairDefault, defaultHair, false);

            const auto verdict =
                HeadPartLadder::Choose({ oNames, oUsable, dNames, dUsable });

            if (verdict.defaultPassedOver) {
                const auto why =
                    HeadPart::WhyNotFor(player, HeadPart::Kind::kHair, defaultHair);
                spdlog::info(
                    "HeadPart: the character's default hair is not offered to them any "
                    "more ({}), so they wear their own. It is KEPT and comes back if "
                    "they change back.",
                    why ? HeadPartPlan::ReasonName(*why) : "unreadable character");
            }

            switch (verdict.wear) {
                case HeadPartLadder::Wear::kOutfit:
                    HairStyle::Apply(player, outfitHair);
                    break;
                case HeadPartLadder::Wear::kDefault:
                    HairStyle::Apply(player, defaultHair);
                    break;
                case HeadPartLadder::Wear::kOwn:
                    HairStyle::Restore(player);
                    break;
            }
        }

        // Eye and brow type (OS-161). Its own function rather than a tail on the
        // hair push, because the head editor handoff needs to call it on its
        // own: closing RaceMenu re-asserts the parts without touching the hair
        // colour, and the colour path has its own re-assert for the opposite
        // case.
        //
        // Resolved here rather than stored as a pointer, for the reason the hair
        // style gives: the outfit holds a mod+id pair precisely so a plugin
        // leaving the load order makes this fail to resolve and do nothing,
        // instead of dereferencing a form that is gone.
        //
        // ⚠ ONE SWAP PER PART, because each Apply queues its own (since OS-230
        // the engine's per-part swap on the face node; before that a full
        // DoReset3D each). Two of them on an outfit that changes both is
        // wasteful but correct, and correct is the right side to land on first:
        // HeadPart::Apply returns early when the part is unchanged, so the
        // common switch between two outfits naming the same eyes costs nothing
        // at all. Coalescing was a real optimisation while each one was a full
        // reset; a per-part swap is priced per part already.
        void PushPlayerHeadParts(const Outfit* a_outfit) {
            auto* const player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                return;
            }
            const auto push = [&](HeadPart::Kind a_kind, const StyleRefKey& a_outfitKey) {
                // ⚠ THE CHARACTER'S DEFAULT SITS BETWEEN THE OUTFIT AND THE
                // RESTORE, and the order is the whole feature. An outfit that
                // names eyes wins, because it is the more specific statement;
                // an outfit that names none falls to what the player said this
                // character normally looks like; and only a character with no
                // default at all goes back to their own record.
                //
                // ⚠⚠ AND A RUNG IS TAKEN ONLY IF IT IS ALSO WEARABLE. The
                // precedence lives in HeadPartLadder::Choose, which is pure and
                // pinned; this half just answers its four questions. Until
                // 2026-08-09 a rung was taken on RESOLVING alone, so a stored
                // default naming a female eye was written onto a male
                // character and stayed there (the engine's own ChangeHeadPart
                // matches on type and checks nothing else). Nothing is cleared
                // when a rung is skipped: the default is still stored, still
                // paid for, and comes back the moment the character suits it
                // again.
                const auto defaultKey =
                    DefaultLook::For(player, DefaultLook::PartFor(a_kind));
                RE::BGSHeadPart* outfitPart  = nullptr;
                RE::BGSHeadPart* defaultPart = nullptr;
                const auto [oNames, oUsable] =
                    ResolveRung(player, a_kind, a_outfitKey, outfitPart, false);
                const auto [dNames, dUsable] =
                    ResolveRung(player, a_kind, defaultKey, defaultPart, false);

                const auto verdict = HeadPartLadder::Choose(
                    { oNames, oUsable, dNames, dUsable });

                if (verdict.defaultPassedOver) {
                    const auto why = HeadPart::WhyNotFor(player, a_kind, defaultPart);
                    spdlog::info(
                        "HeadPart: the character's default {} is not offered to them "
                        "any more ({}), so they wear their own. It is KEPT and comes "
                        "back if they change back.",
                        HeadPart::KindName(a_kind),
                        why ? HeadPartPlan::ReasonName(*why) : "unreadable character");
                }

                // ⚠ THE headprobe LINES ARE GONE, ANSWERED. They asked which
                // rung a push took and whether the write landed, because the eyes
                // clear was reported as doing nothing and the three candidates -
                // the push never running, the ladder choosing a rung that writes
                // nothing, and Restore finding no capture - look identical from
                // outside. MEASURED 2026-08-15 18:01:46: the push ran, the ladder
                // fell to their own, the capture existed and ChangeHeadPart
                // wrote. The clear works; what is still open is whether the
                // capture holds the right "before", which is a different question
                // and needs a different probe.
                switch (verdict.wear) {
                    case HeadPartLadder::Wear::kOutfit:
                        HeadPart::Apply(player, a_kind, outfitPart);
                        return;
                    case HeadPartLadder::Wear::kDefault:
                        HeadPart::Apply(player, a_kind, defaultPart);
                        return;
                    case HeadPartLadder::Wear::kOwn:
                        HeadPart::Restore(player, a_kind);
                        return;
                }
            };
            push(HeadPart::Kind::kEyes, a_outfit ? a_outfit->eyes : StyleRefKey{});
            push(HeadPart::Kind::kBrows, a_outfit ? a_outfit->brows : StyleRefKey{});
            push(HeadPart::Kind::kFacialHair,
                 a_outfit ? a_outfit->facialHair : StyleRefKey{});

            // Slots this load order invented, one push each.
            //
            // ⚠⚠ OVER EVERY DISCOVERED SLOT, NOT OVER WHAT THE OUTFIT NAMES,
            // and the difference is the whole of taking a look OFF. Iterating
            // the outfit's own list would push the ears an outfit names and
            // leave the previous outfit's horns sitting on the character,
            // because nothing would ever reach the rung that puts their own
            // part back. A slot the outfit says nothing about is a slot the
            // outfit is asking to be left alone, and only asking about it can
            // deliver that.
            //
            // ⚠⚠ THREE RUNGS NOW, AND THE MIDDLE ONE IS NEW. This used to say
            // there could be no character default for an invented slot, because
            // DefaultLook::Part is a closed enum over four kinds and a fifth
            // would be a third vocabulary to widen. That was true of Part and
            // false of the feature: DefaultLook carries a counted list keyed by
            // the raw slot number now, exactly as the outfit already did, so
            // nothing widened (user 2026-08-26, Umbrael canonically has horns
            // and every outfit that ignores horns was taking them off).
            //
            // ⚠⚠ NOT HeadPartLadder::Choose, AND THE DIFFERENCE IS ONE ARM.
            // The four kinds RESTORE when the outfit names something unusable;
            // this loop has always SKIPPED and KEPT it, which is the line four
            // lines below and predates the ladder. Routing this through Choose
            // would quietly change that behaviour for every existing outfit, so
            // the pin is added around the old shape rather than through it.
            // ⚠ The two precedences ought to be one, and making them one is a
            // decision about the unusable-outfit arm rather than about pinning.
            for (const auto& slot : HeadPart::DiscoveredSlots()) {
                auto* const dh = RE::TESDataHandler::GetSingleton();
                const auto  resolve = [&](const StyleRefKey& a_key) {
                    return !a_key.Empty() && dh
                               ? dh->LookupForm<RE::BGSHeadPart>(a_key.localFormID,
                                                                 a_key.modName)
                               : nullptr;
                };
                const auto key =
                    a_outfit ? a_outfit->CustomHeadPart(slot.type) : StyleRefKey{};
                if (!key.Empty()) {
                    auto* const part = resolve(key);
                    if (!part) {
                        spdlog::warn("HeadPart: {} '{}'|{:06X} no longer resolves; leaving "
                                     "the current one alone.",
                                     slot.label, key.modName, key.localFormID);
                        continue;
                    }
                    // ⚠ WEARABLE AS WELL AS RESOLVABLE, the lesson the eyes paid
                    // for: the engine's ChangeHeadPart matches on type and checks
                    // nothing else, so a stored reference to a part this character
                    // is no longer offered would land and stay.
                    if (const auto why = HeadPart::WhyNotForSlot(player, slot.type, part);
                        !why || *why != HeadPartPlan::Reject::kNone) {
                        spdlog::info("HeadPart: this outfit's {} is not offered to this "
                                     "character ({}), so it is skipped and KEPT; it comes "
                                     "back if they change back.",
                                     slot.label,
                                     why ? HeadPartPlan::ReasonName(*why)
                                         : "unreadable character");
                        continue;
                    }
                    HeadPart::ApplySlot(player, slot.type, part);
                    continue;
                }

                // The outfit says nothing about this slot, which is where a pin
                // answers. Nothing pinned means what it always meant: put back
                // whatever they had before Fitting Room first touched the slot,
                // a no-op on every character until they use one.
                const auto pinned = DefaultLook::ForSlot(player, slot.type);
                auto* const pinnedPart = resolve(pinned);
                if (!pinnedPart) {
                    if (!pinned.Empty()) {
                        spdlog::warn("HeadPart: this character's pinned {} '{}'|{:06X} no "
                                     "longer resolves; they wear their own.",
                                     slot.label, pinned.modName, pinned.localFormID);
                    }
                    HeadPart::RestoreSlot(player, slot.type);
                    continue;
                }
                // ⚠ A PIN IS TAKEN ONLY IF IT IS ALSO WEARABLE, the same rule the
                // four kinds pay for. Nothing is cleared when it is passed over:
                // the pin is still stored and comes back the moment the
                // character suits it again.
                if (const auto why = HeadPart::WhyNotForSlot(player, slot.type, pinnedPart);
                    !why || *why != HeadPartPlan::Reject::kNone) {
                    spdlog::info("HeadPart: this character's pinned {} is not offered to "
                                 "them any more ({}), so they wear their own. It is KEPT "
                                 "and comes back if they change back.",
                                 slot.label,
                                 why ? HeadPartPlan::ReasonName(*why)
                                     : "unreadable character");
                    HeadPart::RestoreSlot(player, slot.type);
                    continue;
                }
                HeadPart::ApplySlot(player, slot.type, pinnedPart);
            }
        }

        // ⚠⚠ THE WHOLE OF THE PLAYER'S LOOK, IN ONE CALL, AND EVERY PATH THAT
        // CHANGES WHICH OUTFIT THE PLAYER IS SHOWING GOES THROUGH IT (OS-229).
        // Hair colour and hair style ride PushPlayerHairColor; eyes, brows,
        // facial hair and the discovered slots ride PushPlayerHeadParts. Four
        // staging paths called the pair by hand and the quick-switch hotkey
        // called HALF of it: it stated the colour, because that was the only
        // look dimension when it was written, and every dimension added since
        // (style, eyes, brows, facial hair, horns) simply never learned about
        // the hotkey. Field 2026-08-18 22:51: seven quick-switches, seven
        // colour pushes, zero `HeadPart: hair ... rebuild issued`, and the
        // style only caught up when the editor opened and pushed the pair.
        // Two painters of one appearance always drift; this is the one painter.
        //
        // Colour first, head parts second, the order the staging paths kept:
        // the colour push establishes the exact tint the head-build repaint
        // reads, so every rebuild the head-part half issues carries it.
        void PushPlayerLook(const Outfit* a_outfit) {
            PushPlayerHairColor(a_outfit);
            PushPlayerHeadParts(a_outfit);
        }

        // The FOLLOWER half of the colour push, and the only place a non-player
        // actor's hair colour is written. Runs from RequestRefreshActor's task on
        // the GAME thread, immediately before the rebuild that carries it.
        //
        // One seam rather than one call per staging path, for two reasons. It is
        // the only way to reach a follower who was never edited this session -
        // the post-load sweep and the OBody-readiness sweep both come through
        // here - and it keeps the write on a single thread. An extra synchronous
        // push from the present-thread staging paths would have two threads
        // applying to the SAME actor, and the recapture test is exactly what
        // cannot survive that: the second one can read a colour the first has
        // decided on but not yet written, conclude "someone else changed it", and
        // store Fitting Room's own colour as the user's original.
        void PushNpcHairColor(RE::Actor* a_actor) {
            if (!a_actor) {
                return;
            }
            auto&      session = OutfitSession::GetSingleton();
            const auto st      = session.HairStateForNpc(a_actor);
            if (!st.manages) {
                return;  // not Fitting Room's actor, or the player: touch nothing
            }

            // ⚠ SEED BEFORE APPLY, and note HOW that ordering is guaranteed: not
            // by which co-save record is read first, but by these being two
            // consecutive statements in the one function that applies a
            // follower's colour at all. That distinction is the whole reason the
            // load path is wired here instead of in OnNpcLoad. Seeding from a
            // load callback would inherit the trap that stops ActivateByName from
            // being wired: 'ACTV' is written before 'HCOL', so an apply driven
            // from the load would run before the baseline landed, capture Fitting
            // Room's own colour as the user's original, and SeedCaptured would
            // then correctly refuse to overwrite the fresher-looking lie.
            //
            // SeedCaptured is itself write-once and defers to a live capture, so
            // re-seeding on every later refresh costs a lookup and changes
            // nothing.
            if (st.baselineCaptured) {
                HairColor::CapturedColor seed;
                seed.captured    = true;
                seed.modName     = st.baselineMod;
                seed.localFormID = st.baselineLocalID;
                HairColor::SeedCaptured(a_actor, seed);
            }

            // The follower half of the same ladder, and the same call, so the
            // two paths cannot answer this differently.
            HairColor::Push(a_actor, st.outfit ? &*st.outfit : nullptr);

            // Mirror whatever HairColor now holds back into the follower's record,
            // which is what puts it in the next save. Apply captures on first
            // touch and Restore forgets, so reading it back rather than predicting
            // it keeps one source of truth: HairColor decides, the record follows.
            const auto captured = HairColor::CapturedFor(a_actor);
            if (captured.captured) {
                session.CaptureNpcHairBaseline(st.key, captured.modName,
                                               captured.localFormID);
            } else {
                session.ForgetNpcHairBaseline(st.key);
            }
        }

        // The follower half of the hair STYLE push, and the reason a style now
        // survives a save. NpcHair's captures are per-session by necessity -
        // they name scenegraph nodes, and RevertCallback clears them before
        // every load because those nodes belong to a character being torn down.
        // Outfit::hairStyle is the persistent half. So a follower arrives with
        // her style recorded and nothing wearing it, and this is what closes
        // that gap.
        //
        // ⚠ RECONCILE, NOT APPLY. This seam is every staging change, the
        // OBody-readiness sweep and the post-load walk, so it runs far more
        // often than a style actually changes. NpcHair::Apply is a cull, an
        // attach and a FixSkinInstances over her whole face node. The plan
        // answering kNone when she already wears what the outfit names is the
        // only reason a push can sit here at all - see NpcHairPlan.h.
        //
        // Deliberately NOT symmetric with PushNpcHairColor's baseline seeding.
        // Colour needs a seed-before-apply order because it CAPTURES the user's
        // original into persistent state and a wrong capture is unrecoverable.
        // Style captures nothing persistent: NpcHair reads her own hair off the
        // live head at attach time, so there is no baseline to get wrong and no
        // ordering trap to respect.
        // One kind of a follower's head-part push through NpcHair. Hair and
        // facial hair share every line of this (OS-225): the outfit's key,
        // then the character's default, resolved and planned against what she
        // wears through us IN THAT KIND, and NpcHair culls and attaches. What
        // differs is which field of the outfit is read and which default part
        // is asked for, and both come from the kind.
        void PushNpcHeadPart(RE::Actor* a_actor, HeadPart::Kind a_kind) {
            if (!a_actor || !NpcHair::Handles(a_kind)) {
                return;
            }
            const auto st = OutfitSession::GetSingleton().HairStateForNpc(a_actor);

            // ⚠ Resolved here rather than stored as a pointer, the same rule the
            // player push states: the outfit holds a mod+id pair precisely so a
            // plugin leaving the load order fails to resolve and does nothing,
            // instead of dereferencing a form that is gone.
            NpcHairPlan::Want want;
            RE::BGSHeadPart*  part = nullptr;
            // The follower's half of the default-look rule; see the player push
            // for why the character's default sits between the outfit and the
            // restore. A follower is exactly the character this is worth having
            // for: her own record's hair is whatever the mod that made her
            // shipped, and "she always wears this" is a thing the player wants
            // to say once.
            StyleRefKey key;
            if (st.outfit) {
                key = a_kind == HeadPart::Kind::kFacialHair ? st.outfit->facialHair
                                                             : st.outfit->hairStyle;
            }
            if (key.Empty()) {
                key = DefaultLook::For(a_actor, DefaultLook::PartFor(a_kind));
            }
            if (!key.Empty()) {
                want.empty = false;
                if (auto* const dh = RE::TESDataHandler::GetSingleton()) {
                    part = dh->LookupForm<RE::BGSHeadPart>(key.localFormID, key.modName);
                }
                want.resolved = part != nullptr;
                if (part) {
                    want.formID      = part->GetFormID();
                    want.unsupported = NpcHair::IsUnsupported(part);
                } else {
                    // debug, not warn: this fires on every refresh for as long as
                    // the plugin is absent, and it is the same "unresolved
                    // (plugin missing?)" note VisitStyles and WeaponDisplayFor
                    // make about a style they skip.
                    spdlog::debug("NpcHair: {} '{}'|{:06X} no longer resolves; leaving hers "
                                  "alone.",
                                  HeadPart::KindName(a_kind), key.modName, key.localFormID);
                }
            }

            // AppliedTo answers with the DECISION when one is queued but not yet
            // drained, so two refreshes in quick succession cannot both decide to
            // apply the same style.
            auto* const current = NpcHair::AppliedTo(a_actor, a_kind);
            switch (NpcHairPlan::Plan(st.manages, want,
                                      current ? current->GetFormID() : 0u)) {
                case NpcHairPlan::Action::kApply:
                    NpcHair::Apply(a_actor, part);
                    break;
                case NpcHairPlan::Action::kRestore:
                    NpcHair::Restore(a_actor, a_kind);
                    break;
                case NpcHairPlan::Action::kNone:
                    break;
            }
        }

        void PushNpcHairStyle(RE::Actor* a_actor) {
            PushNpcHeadPart(a_actor, HeadPart::Kind::kHair);
        }

        // OS-225. Beside the style push and after it, in the same task, so a
        // follower who wears both arrives with both. Same reconcile shape, so
        // sitting on every refresh costs a plan and a return when nothing
        // changed.
        void PushNpcFacialHair(RE::Actor* a_actor) {
            PushNpcHeadPart(a_actor, HeadPart::Kind::kFacialHair);
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

        // ⚠ THE CHARACTER'S DEFAULT BODY IS SUBSTITUTED HERE, AT THE ONE POINT
        // THE ANSWER IS RESOLVED, and the first cut got that wrong. It was
        // applied inside EditorUI::RestoreStagedBody, which is only one of
        // several routes to a body change, so the default held when the editor
        // cleared a slot and vanished the moment an outfit switch came through
        // this function instead (user 2026-08-06: "if i switch outfit ... it can
        // be unset and go away? but if i change weight it came back?" - the
        // weight path went through RestoreStagedBody, the outfit switch did
        // not). One resolution point, every caller.
        //
        // ⚠ IT FILLS THE OUTFIT'S OWN FIELDS RATHER THAN ADDING A BRANCH. An
        // empty preset with drives set means "revert to baseline" downstream,
        // and there are four such branches across two functions; substituting
        // the value makes every one of them behave as though the outfit had
        // named it, instead of needing the same test repeated in each.
        //
        // ⚠ AND IT COVERS restoreBaseline TOO. Base gear with no outfit is
        // also a case of "nothing said otherwise", which is precisely what a
        // default is for; leaving it out would put the character back on their
        // captured baseline the moment they took an outfit off, which reads as
        // the setting having been forgotten again.
        void SubstituteDefaultBody(OutfitSession::BodyDisplay& a_body, RE::Actor* a_actor) {
#if defined(FR_BODY_STUDIO)
            if (!a_actor || !a_body.preset.empty() || !a_body.customPresetId.empty()) {
                return;
            }
            if (!a_body.drives && !a_body.restoreBaseline) {
                return;  // a scene or a suspension: leave the body alone entirely
            }
            const auto def = OS::DefaultBody::For(a_actor);
            if (def.Empty()) {
                return;
            }
            a_body.preset         = def.installed;
            a_body.customPresetId = def.customId;
            a_body.drives         = true;
            a_body.restoreBaseline = false;
#else
            (void)a_body;
            (void)a_actor;
#endif
        }

        // ⚠⚠ THE RECIPE IS THE BODY'S, NOT THE OUTFIT'S. The outfit stores a
        // level and nothing else; which named morphs that level means depends on
        // the body the actor is actually wearing, which is why this resolves the
        // family here rather than at authoring time. A save carried to another
        // body picks up that body's recipe.
        //
        // ⚠ AN EMPTY PLAN IS A CLEAR AND THAT IS THE POINT. Off, a body with no
        // recipe, and a catalog that has not finished scanning all land here, and
        // all three mean the same thing: this character wears none of our lift.
        void ApplyPushUpFor(RE::Actor* a_actor, PushUpMode a_mode) {
            if (!a_actor || !RaceMenuMorphApi::Available()) {
                return;
            }
            std::vector<RaceMenuMorphApi::MorphValue> plan;
            auto                                     family = BodyFamily::kUnknown;
            if (a_mode != PushUpMode::kNone) {
                if (auto snap = BodySlideCatalog::GetSingleton().Snapshot()) {
                    family = snap->FamilyForMesh(BuiltBodyMeshKey(a_actor));
                }
                for (const auto& m : BuildPushUpMorphPlan(family, a_mode)) {
                    plan.push_back({ m.name, m.value });
                }
            }
            // ⚠ SILENT WHEN THERE IS NOTHING TO SAY, because this runs on every
            // body apply. A clear on a character who never had lift costs one
            // key lookup and writes no line.
            if (plan.empty() && !RaceMenuMorphApi::HasPushUp(a_actor)) {
                // ⚠⚠ EXCEPT WHEN THE SILENCE MEANS SOMETHING. Off with nothing
                // to take off is the common case and stays quiet. A mode that was
                // ASKED for and produced no morphs is the family or the recipe
                // failing, and until 2026-08-27 that looked identical to off.
                if (a_mode != PushUpMode::kNone) {
                    spdlog::warn("PushUp: mode {} was asked for on family {} and the "
                                 "recipe came back EMPTY, so nothing is written.",
                                 static_cast<int>(a_mode), static_cast<int>(family));
                }
                return;
            }
            const auto result = RaceMenuMorphApi::ApplyPushUp(a_actor, plan);
            spdlog::debug("PushUp: {} morph(s) written for mode {} on family {} "
                          "(available={} loaded={} refreshed={}).",
                          plan.size(), static_cast<int>(a_mode),
                          static_cast<int>(family), result.available,
                          result.actorLoaded, result.refreshed);
            // ⚠⚠ THE WRITE LANDING IS NOT THE MORPH SURVIVING. OBody rebuilds
            // the body about 0.2 s after this line, every time, so the line above
            // proves only that RaceMenu accepted the values. The probe reads the
            // key back once that rebuild has settled and says which of the two
            // faults this is.
            RaceMenuMorphApi::ArmPushUpProbe(a_actor, plan.size());
        }

        void ApplyPlayerBodyState() {
            try {
                auto body = OutfitSession::GetSingleton().DisplayBody();
                SubstituteDefaultBody(body, RE::PlayerCharacter::GetSingleton());
                if (body.drives) {
#if defined(FR_BODY_STUDIO)
                    BodyStudioProof::ORefitPolicy policy{
                        static_cast<int>(body.orefit), body.torsoStyleMask,
                        body.torsoHideMask
                    };
                    if (!body.customPresetId.empty()) {
                        if (auto preset = BodyPresetStore::GetSingleton().Find(
                                body.customPresetId)) {
                            BodyStudioProof::QueueCustomPreset(
                                RE::PlayerCharacter::GetSingleton(), std::move(*preset), policy);
                        } else {
                            spdlog::warn("Body Studio: outfit references missing custom preset '{}'.",
                                         body.customPresetId);
                            auto* player = RE::PlayerCharacter::GetSingleton();
                            if (BodyStudioProof::Snapshot(player).activeCustom) {
                                BodyStudioProof::QueueBaseline(player, {}, policy);
                            } else {
                                ObodyApi::ApplyOutfitBody(
                                    player, {}, static_cast<int>(body.orefit),
                                    body.torsoStyleMask, body.torsoHideMask);
                            }
                        }
                    } else if (!body.preset.empty()) {
                        BodyStudioProof::QueueInstalled(
                            RE::PlayerCharacter::GetSingleton(), body.preset, policy);
                    } else if (BodyStudioProof::Snapshot(
                                   RE::PlayerCharacter::GetSingleton()).activeCustom) {
                        BodyStudioProof::QueueBaseline(
                            RE::PlayerCharacter::GetSingleton(), {}, policy);
                    } else {
                        ObodyApi::ApplyOutfitBody(
                            RE::PlayerCharacter::GetSingleton(), {},
                            static_cast<int>(body.orefit), body.torsoStyleMask,
                            body.torsoHideMask);
                    }
#else
                    ObodyApi::ApplyOutfitBody(RE::PlayerCharacter::GetSingleton(), body.preset,
                                              static_cast<int>(body.orefit),
                                              body.torsoStyleMask, body.torsoHideMask);
#endif
                } else if (body.restoreBaseline) {
                    // Base gear is a real destination, not a suspended
                    // preview. Restore the player's captured OBody assignment
                    // after leaving an outfit or follower mannequin.
                    ObodyApi::ApplyOutfitBody(
                        RE::PlayerCharacter::GetSingleton(), {},
                        static_cast<int>(ORefitMode::kDefault), 0, 0);
                } else {
                    ObodyApi::ReleaseOutfitORefit(RE::PlayerCharacter::GetSingleton());
                }
                // Follows body.drives, so leaving an outfit takes the lift off
                // with the rest of the body it was part of.
                ApplyPushUpFor(RE::PlayerCharacter::GetSingleton(),
                               body.drives ? body.pushUp : PushUpMode::kNone);
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
                // The follower's own default, for the reason the player's copy
                // above carries. Substituted before the baseline capture below,
                // so a default counts as "the outfit named a preset" there too.
                SubstituteDefaultBody(body, actor);
                if (body.drives) {
                    if ((!body.preset.empty() || !body.customPresetId.empty()) &&
                        !body.baselineCaptured) {
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
#if defined(FR_BODY_STUDIO)
                    BodyStudioProof::ORefitPolicy policy{
                        static_cast<int>(body.orefit), body.torsoStyleMask,
                        body.torsoHideMask
                    };
                    if (!body.customPresetId.empty()) {
                        if (auto preset = BodyPresetStore::GetSingleton().Find(
                                body.customPresetId)) {
                            BodyStudioProof::QueueCustomPreset(actor, std::move(*preset), policy);
                        } else {
                            spdlog::warn("Body Studio: follower outfit references missing custom preset '{}'.",
                                         body.customPresetId);
                            if (BodyStudioProof::Snapshot(actor).activeCustom) {
                                BodyStudioProof::QueueBaseline(
                                    actor, { body.baseline, body.baselineCaptured }, policy);
                            } else {
                                ObodyApi::ApplyNpcOutfitBody(
                                    actor, {}, static_cast<int>(body.orefit),
                                    body.torsoStyleMask, body.torsoHideMask,
                                    body.baseline, body.baselineCaptured);
                            }
                        }
                    } else if (!body.preset.empty()) {
                        BodyStudioProof::QueueInstalled(actor, body.preset, policy);
                    } else if (BodyStudioProof::Snapshot(actor).activeCustom) {
                        BodyStudioProof::QueueBaseline(
                            actor, { body.baseline, body.baselineCaptured }, policy);
                    } else {
                        ObodyApi::ApplyNpcOutfitBody(
                            actor, {}, static_cast<int>(body.orefit),
                            body.torsoStyleMask, body.torsoHideMask,
                            body.baseline, body.baselineCaptured);
                    }
#else
                    ObodyApi::ApplyNpcOutfitBody(
                        actor, body.preset, static_cast<int>(body.orefit),
                        body.torsoStyleMask, body.torsoHideMask,
                        body.baseline, body.baselineCaptured);
#endif
                } else {
                    ObodyApi::ReleaseOutfitORefit(actor);
                }
                ApplyPushUpFor(actor, body.drives ? body.pushUp : PushUpMode::kNone);
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
                // The chest the override shows may have changed class against
                // the worn one; keep CBPC's armor-state read on the SHOWN
                // piece (r55). Idempotent, nudges CBPC only on a change.
                CbpcArmorClass::AssertPlayer();
            } catch (const std::exception& e) {
                spdlog::error("RefreshPlayer threw: {}", e.what());
            } catch (...) {
                spdlog::error("RefreshPlayer threw a non-standard exception.");
            }
        }

        void RunPlayerRefreshWhenSafe() {
            if (!g_blockingCooldown.Ready(PlayerIsBlocking(), SteadySeconds())) {
                // ⚠ The comment that used to sit here said AddTask calls made
                // while the queue drains land on a LATER game-thread pass, so
                // this poll stayed non-blocking. That is false, and it was
                // measured false on 2026-07-31: a task added during a drain runs
                // in the SAME pass, microseconds later. Copying this pattern into
                // WorldWatch's heartbeat, which re-queued unconditionally rather
                // than only while waiting, froze the game on every launch.
                //
                // It survives here only because the wait is bounded: BlockingCooldown
                // releases one second after the block ends, so the worst case is a
                // one-second spin, which reads as a hitch rather than a hang. It is
                // still a busy-wait on the main thread and still worth replacing.
                // Do NOT treat it as a pattern to copy.
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
        //
        // The rule overlay is PLAYER-scoped and must NOT land on this branch.
        // playerMannequin_ renders a FOLLOWER's outfit on the player's body
        // while the user edits that follower (see the member's own comment);
        // composing a "hide the helmet" player rule onto it would make the
        // FOLLOWER's helmet vanish from the viewport for a reason the user
        // never configured on that follower, and Task 11's editor gate does
        // not save this - it only gates new decisions, so an overlay already
        // in force would stay composed for the entire editing session.
        // Return it untouched; the overlay resumes once the mannequin clears.
        if (playerMannequin_) {
            return &*playerMannequin_;
        }

        const Outfit* base =
            (staged_ && stagedForPlayer_) ? &*staged_ : library_.Active();

        // The rule overlay composes HERE, on the const Outfit* Display(),
        // VisitStyles() and DisplayBody() all read for the staged/library
        // case above. Composing at each of those instead would be three
        // chances to forget one, and the one most easily forgotten is
        // DisplayBody, whose ORefit torso masks would then describe gear the
        // player is not actually showing.
        //
        // staged_ && stagedForPlayer_ DELIBERATELY still composes, unlike the
        // mannequin above: that IS the player's own outfit, being edited live
        // in the player's own editor, so what the overlay would show on their
        // saved outfit is also what it shows on their in-progress edit - the
        // same "what the user is looking at is what they get" rule that lets
        // the mannequin case win over composing at all.
        //
        // Returning storage rather than a temporary keeps every existing
        // caller's `const Outfit*` valid; callers already hold lock_ for the
        // lifetime of the pointer, which is what makes this safe. See this
        // method's declaration for the resulting pointer-lifetime contract.
        if (!ruleOverlay_.empty()) {
            composed_ = Rules::Compose(base ? *base : kEmptyOutfit, ruleOverlay_);
            return &composed_;
        }
        return base;
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
            d.customPresetId = o->customBodyPresetId;
            d.orefit = o->orefit;
            d.pushUp = o->pushUp;
            const auto display = ComputeDisplaySet(*o, blocklist_);
            d.torsoStyleMask   = display.styleMask & kORefitTorsoMask;
            d.torsoHideMask    = display.hideMask & kORefitTorsoMask;
            d.drives = true;
        } else if (!SceneGuard::Active() && !suspended_) {
            // No active saved outfit means Base gear. Unlike a scene
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
        if (NpcResolve::StagedTargetMatches(staged_.has_value(), stagedForPlayer_,
                                            stagedBaseFormID_, tc.baseFormID)) {
            outfit = &*staged_;
        } else if (it != npcAssignments_.end()) {
            outfit = it->second.library.Active();
        }
        if (outfit) {
            d.preset = outfit->obodyPreset;
            d.customPresetId = outfit->customBodyPresetId;
            d.orefit = outfit->orefit;
            d.pushUp = outfit->pushUp;
            const auto display = ComputeDisplaySet(*outfit, blocklist_);
            d.torsoStyleMask   = display.styleMask & kORefitTorsoMask;
            d.torsoHideMask    = display.hideMask & kORefitTorsoMask;
            d.drives           = true;
        } else if (d.baselineCaptured) {
            // Base gear after a body-setting outfit must restore the
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

    OutfitSession::NpcHairState OutfitSession::HairStateForNpc(RE::Actor* a_actor) const {
        NpcHairState s;
        const auto   tc = ClassifyTarget(a_actor ? a_actor->GetHandle()
                                                 : RE::ActorHandle{});
        if (tc.isPlayer || !tc.key || tc.baseFormID == 0) {
            // ⚠ The PLAYER leaves here with manages == false, and that is
            // load-bearing rather than tidy. The player channel pushes its own
            // colour from the staging paths against the outfit it is showing;
            // falling through to the "not assigned" answer below would report
            // Base gear and RESTORE, stripping the colour off the outfit
            // they are actually wearing.
            //
            // It is also what keeps the player mannequin from creeping back in.
            // The mannequin lives in playerMannequin_, which only the player
            // channel reads, and this function neither runs for the player nor
            // reads that field - so a follower preview still cannot repaint the
            // player's own hair, which stays consistent with
            // ComposeMannequinSource copying no hair state at all.
            return s;
        }
        s.key = *tc.key;
        std::scoped_lock l(lock_);
        const auto       it = npcAssignments_.find(*tc.key);
        if (it != npcAssignments_.end()) {
            s.baselineMod      = it->second.hairBaselineMod;
            s.baselineLocalID  = it->second.hairBaselineLocalID;
            s.baselineCaptured = it->second.hairBaselineCaptured;
        }

        // The same staged-override-else-assigned-active rule DisplayBodyForNpc
        // uses, so a live follower preview and a committed assignment resolve
        // through one description of "what this actor is showing".
        //
        // Deliberately NOT that function's suspension/scene gate. Hair colour
        // follows the user's outfit CHOICE, not whether the override is standing
        // down for a beast form or an OStim scene - the same rule DiscardStaging
        // states. Folding those in would let an async scene flip repaint a
        // character's persistent base colour, and it would make a resume have to
        // undo a suspend that never restored anything.
        const bool stagedHere = NpcResolve::StagedTargetMatches(
            staged_.has_value(), stagedForPlayer_, stagedBaseFormID_, tc.baseFormID);
        const bool assigned   = it != npcAssignments_.end();

        // An actor Fitting Room has no opinion about keeps manages == false, so
        // the caller touches nothing. An ASSIGNED actor whose library is inactive
        // is a different thing entirely: that is a real Equipped-gear choice and
        // it must restore, which is why manages is not simply "has an outfit".
        s.manages = assigned || stagedHere;
        if (stagedHere) {
            s.outfit = *staged_;
        } else if (assigned) {
            if (const auto* o = it->second.library.Active()) {
                s.outfit = *o;
            }
        }
        return s;
    }

    void OutfitSession::CaptureNpcHairBaseline(
        const NpcKey& a_key, std::string a_mod, std::uint32_t a_localFormID) {
        std::scoped_lock l(lock_);
        auto&            rec = npcAssignments_[a_key];
        if (!rec.hairBaselineCaptured) {
            rec.hairBaselineMod      = std::move(a_mod);
            rec.hairBaselineLocalID  = a_localFormID;
            rec.hairBaselineCaptured = true;
        }
    }

    void OutfitSession::ForgetNpcHairBaseline(const NpcKey& a_key) {
        std::scoped_lock l(lock_);
        // find, not operator[]: forgetting a baseline must never CREATE an
        // assignment for an actor that has none, which would put an empty
        // library into the co-save and start rendering nothing for them.
        const auto it = npcAssignments_.find(a_key);
        if (it == npcAssignments_.end()) {
            return;
        }
        it->second.hairBaselineMod.clear();
        it->second.hairBaselineLocalID  = 0;
        it->second.hairBaselineCaptured = false;
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

    std::optional<Outfit> OutfitSession::ActiveOutfitFor(RE::Actor* a_actor) const {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!a_actor || !player) {
            return std::nullopt;
        }
        if (a_actor != player) {
            // OS-128. Followers dye from their own assignment, and the branch
            // below answers with the same precedence the snapshot build uses
            // for their styles, so the two can never disagree about which
            // outfit she is wearing.
            return ActiveOutfitForNpc(a_actor);
        }
        std::scoped_lock l(lock_);
        // EffectiveLocked, so this answers with the ONE description of what the
        // player is rendering: the mannequin while a follower is being edited,
        // else the staged outfit, else the library's active one, and nothing at
        // all while a scene mod runs or the override is suspended. A dye is a
        // property of the geometry currently on screen, so it has to follow the
        // same rule the styles on that geometry follow rather than a second one.
        //
        // The mannequin carries no dye of its own for the same structural reason
        // it carries no hair state: ComposeMannequinSource starts from the
        // player's captured equipped gear and overlays only slot and weapon
        // entries. So editing a follower shows the follower's styles on the
        // player's body without the follower's dye, which is the same trade
        // already accepted for hair.
        const auto* o = EffectiveLocked();
        return o ? std::optional<Outfit>{ *o } : std::nullopt;
    }

    std::optional<Outfit> OutfitSession::ActiveOutfitForNpc(RE::Actor* a_actor) const {
        // Engine reads BEFORE the lock, matching DisplayBodyForNpc. A base with
        // no defining file yields no key, which is the same "no persistent
        // identity" rule every other NPC caller applies.
        //
        // a_actor is dereferenced unguarded, unlike the null-tolerant siblings:
        // the sole caller, ActiveOutfitFor, has already rejected null.
        const auto tc = ClassifyTarget(a_actor->GetHandle());
        if (tc.isPlayer || !tc.key || tc.baseFormID == 0) {
            return std::nullopt;
        }
        std::scoped_lock l(lock_);
        // ⚠ SceneGuard is checked HERE and never inside SelectNpcSource. It
        // flips asynchronously and is read lock-free on the hooks, so folding
        // it into the pure selector would drag an async global into the built
        // snapshot. EffectiveLocked gates the player on it the same way.
        if (SceneGuard::Active()) {
            return std::nullopt;
        }
        const auto          it = npcAssignments_.find(*tc.key);
        const Outfit* const active =
            it != npcAssignments_.end() ? it->second.library.Active() : nullptr;
        // ⚠ Suspension is passed to the selector rather than early-returned, so
        // the selector stays the single authority on precedence: suspension
        // first, then the staged override, then the assigned active outfit.
        const bool suspended = suspendedActors_.contains(tc.baseFormID);
        const bool stagedHere = NpcResolve::StagedTargetMatches(
            staged_.has_value(), stagedForPlayer_, stagedBaseFormID_, tc.baseFormID);
        switch (NpcResolve::SelectNpcSource(suspended, stagedHere, active != nullptr)) {
            case NpcResolve::NpcSource::kStagedOverride:
                // A staged target wins even with no assignment yet, which is
                // what makes a follower being dressed for the FIRST time
                // preview in the right colour instead of snapping to it on
                // Apply. No special case needed: hasActiveOutfit is false here
                // and kStagedOverride still wins.
                return std::optional<Outfit>{ *staged_ };
            case NpcResolve::NpcSource::kAssignedActive:
                return std::optional<Outfit>{ *active };
            case NpcResolve::NpcSource::kNone:
                break;
        }
        return std::nullopt;
    }

    bool OutfitSession::DrivesPlayerHairTint() const {
        // ⚠⚠ THE CHARACTER DEFAULT COUNTS, NOT ONLY THE OUTFIT, and this is the
        // same widening DrivesPlayerHeadParts already carries one function down
        // - for the same reason, one feature later. This answers "is Fitting
        // Room what is painted on their hair", which gates the head-editor
        // re-assert AND the head-build repaint. A character whose colour comes
        // from a DEFAULT was answering no, so neither ran: the engine's own
        // build repainted the hair from the actor base's colour FORM and the
        // exact shade never came back.
        //
        // ⚠ MEASURED 2026-08-16, and this is the field report it closes: set a
        // colour as the default, save, load, and the hair comes back on the
        // SNAPPED palette form rather than the shade that was chosen -
        // `repaint ... rgb=(255,153,97) [form]` where the default says
        // (255,100,100). Opening the editor fixed it, which is the tell: a push
        // was all it ever needed and nothing was pushing.
        std::scoped_lock l(lock_);
        auto* const      player = RE::PlayerCharacter::GetSingleton();
        if (player && DefaultLook::HasHairColour(player)) {
            return true;
        }
        const auto* o = EffectiveLocked();
        return o && o->hairTint.set;
    }

    void OutfitSession::PushPlayerLookFor(const Outfit* a_outfit) {
        // The file-local pair, and nothing else: this member exists so a caller
        // outside this file has the same one door the staging paths use.
        PushPlayerLook(a_outfit);
    }

    void OutfitSession::ReassertPlayerHairColor(bool a_atLoadBoundary) {
        // A hair colour is a colour: same flag as the skin tone, not the one
        // that swaps eyes and brows.
        if (!Settings::GetSingleton().keepColoursAfterRebuild) {
            static std::atomic<bool> said{ false };
            if (!said.exchange(true)) {
                spdlog::info("HairColor reassert: stood down for the rest of this "
                             "session because [Compat] bKeepColoursAfterRebuild is "
                             "off. The character keeps whatever RaceMenu and the "
                             "engine's head build painted.");
            }
            return;
        }
        std::optional<Outfit> effective;
        // ⚠ BOTH HALVES OF THE REFUSAL, TAKEN UNDER THE ONE ACQUISITION THAT
        // DECIDES IT. "No outfit is effective right now" and "an outfit is
        // effective and carries no hair colour" are different faults with
        // different fixes, and until this line existed the function returned
        // identically silent for both - so a field log could not tell them
        // apart, and neither could be told from the call never happening. Read
        // here rather than re-derived after the lock is dropped, because the
        // outfit can come off in between and the log would then describe a
        // different moment from the one that made the decision.
        bool        haveOutfit = false;
        std::string outfitName;
        {
            std::scoped_lock l(lock_);
            const auto*      o = EffectiveLocked();
            if (o) {
                haveOutfit = true;
                outfitName = o->name;
            }
            // ⚠ Re-checked under the SAME acquisition that copies it, rather
            // than trusted from the caller's earlier DrivesPlayerHairTint().
            // Between those two points the outfit can come off, and pushing a
            // null outfit here would run Restore and overwrite the very colour
            // the head editor just set. For a RE-assert, doing nothing is
            // always the safe answer, so the null case returns instead of
            // falling through to the Restore branch the staging paths want.
            if (o) {
                effective = *o;
            }
        }
        // ⚠⚠ THE DEFAULT IS WHY "THE OUTFIT NAMES NO COLOUR" IS NOT "NOTHING TO
        // DO". This function used to require o->hairTint.set before it would
        // push anything, and an outfit naming no colour is EXACTLY the case a
        // character default exists for - so the one configuration the feature
        // was built to serve was the one that returned early. Same shape as the
        // bare-body mask reading zero as nothing to do, and the hide mask's
        // nullopt: the empty case is the feature's case.
        //
        // ⚠ THE LADDER DECIDES, NOT THIS FUNCTION. PushPlayerHairColor resolves
        // outfit-then-default-then-own through HairColor::Push, so all this has
        // to establish is whether ANYTHING names a colour. Re-deriving the
        // precedence here would be the second reader that drifts.
        auto* const player       = RE::PlayerCharacter::GetSingleton();
        const bool  outfitTints  = effective && effective->hairTint.set;
        const HairTint defaulted = player ? DefaultLook::HairColour(player) : HairTint{};
        if (!outfitTints && !defaulted.set) {
            // ⚠ STILL DOES NOTHING WHEN NOTHING NAMES A COLOUR - on the head
            // editor's path. For a RE-assert, doing nothing is the safe
            // answer there: pushing would run Restore and overwrite the very
            // colour the editor may have just set.
            //
            // The LOAD boundary is the one place that answer was wrong: the
            // session's hair state outlives the load, so an earlier save that
            // names no look comes back with the abandoned look's colour still
            // on the actor base and nobody to put it back (field 2026-08-24:
            // a look applied, an earlier save loaded, the look's colour kept).
            // RestoreIfStillOurs carries its own guard - it only writes while
            // the base still wears the exact form this session last applied -
            // so a colour the load's changeform or the user already moved is
            // left alone.
            if (a_atLoadBoundary && player &&
                HairColor::RestoreIfStillOurs(player)) {
                RequestRefresh();  // repaint the restored colour onto the hair
                return;
            }
            if (haveOutfit) {
                spdlog::info("HairColor reassert: outfit '{}' is effective, neither it nor "
                             "this character's default names a hair colour, so there is "
                             "nothing to put back.",
                             outfitName);
            } else {
                spdlog::info("HairColor reassert: no outfit is effective and this character "
                             "has no default hair colour, so there is nothing to put back. "
                             "The character keeps whatever the engine's own head build "
                             "painted from the actor base.");
            }
            return;
        }
        if (outfitTints) {
            spdlog::info("HairColor reassert: outfit '{}' carries ({},{},{}); pushing it.",
                         outfitName, effective->hairTint.r, effective->hairTint.g,
                         effective->hairTint.b);
        } else {
            spdlog::info("HairColor reassert: outfit '{}' names no hair colour, so this "
                         "character's DEFAULT ({},{},{}) is what gets pushed.",
                         haveOutfit ? outfitName : "(none)", defaulted.r, defaulted.g,
                         defaulted.b);
        }
        // ⚠ Colour FIRST, refresh SECOND, for the same reason as the staging
        // paths: reversed, the rebuild carries the OLD colour and the re-assert
        // lands one refresh late. Called from OUTSIDE lock_ because
        // PushPlayerHairColor reaches SetHairColor, which touches engine state.
        // ⚠ NULL WHEN NO OUTFIT IS EFFECTIVE, WHICH IS NOW REACHABLE. The
        // default-only case can arrive here in Base gear, and the ladder answers
        // it: a null outfit names nothing, so it falls straight to the default.
        // Guarded above, so this can never reach the Restore rung with nothing
        // to assert.
        PushPlayerHairColor(effective ? &*effective : nullptr);
        RequestRefresh();
    }

    bool OutfitSession::DrivesPlayerHeadParts() const {
        // ⚠⚠ THE CHARACTER DEFAULT COUNTS, NOT ONLY THE OUTFIT, and this is
        // load bearing rather than tidy. This answers "is Fitting Room what is
        // on their face", which decides whether opening RaceMenu stands us
        // down. A character whose eyes come from a DEFAULT was answering no,
        // so the stand-down never ran: they sculpted a new race with our part
        // on the record looking like their own, and the capture we would later
        // restore from was the pre-change one. Widened alongside the ladder's
        // usability rung, because that rung's fall-through goes to Restore and
        // a stale capture is what Restore would then write over their new
        // face with.
        std::scoped_lock l(lock_);
        auto* const      player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            for (const auto part : { DefaultLook::Part::kEyes, DefaultLook::Part::kBrows,
                                     DefaultLook::Part::kFacialHair }) {
                if (DefaultLook::Has(player, part)) {
                    return true;
                }
            }
        }
        const auto* o = EffectiveLocked();
        return o && (!o->eyes.Empty() || !o->brows.Empty() || !o->facialHair.Empty());
    }

    void OutfitSession::StandDownPlayerHeadParts() {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        // ⚠ RESTORE, WHICH ALSO DROPS THE CAPTURE, and the drop is the point.
        // The capture holds what the character had before Fitting Room first
        // touched them. Keeping it across the editor would leave it describing
        // the PRE-EDITOR eyes, so a later clear of the outfit would silently
        // undo whatever the user did in RaceMenu, permanently. Letting it go
        // here is what makes the next capture describe their post-editor face.
        //
        // Both kinds unconditionally rather than only the ones the outfit
        // names: an outfit that names eyes and not brows can still have a brow
        // capture left by a hover preview, and half a stand-down is the same
        // stale-capture bug in a narrower window.
        HeadPart::Restore(player, HeadPart::Kind::kEyes);
        HeadPart::Restore(player, HeadPart::Kind::kBrows);
        HeadPart::Restore(player, HeadPart::Kind::kFacialHair);
        spdlog::info("HeadPart: stood down for the character editor; the player's own "
                     "eyes and brows are back and the capture is dropped.");
    }

    void OutfitSession::ReassertPlayerHeadParts() {
        if (!Settings::GetSingleton().reassertAppearance) {
            static std::atomic<bool> said{ false };
            if (!said.exchange(true)) {
                spdlog::info("HeadParts reassert: stood down for the rest of this "
                             "session because [Compat] bReassertAppearance is off. The "
                             "character keeps the eyes and brows their own record "
                             "names.");
            }
            return;
        }
        // ⚠ THE CHARACTER'S DEFAULT COUNTS AS SOMETHING TO ASSERT, and leaving
        // it out was a real fault rather than a missing nicety. The guard below
        // used to ask only whether the OUTFIT named eyes or brows, so a player
        // who set a default and wore an outfit naming neither got no push at
        // all: every reassert returned here and the character kept their record
        // eyes until something else pushed. That is exactly the shape of the
        // load-time report, since PushPlayerHeadParts already falls from the
        // outfit to the default to the record and the fall was never reached.
        //
        // Read outside the lock on purpose: DefaultLook is its own store and
        // knows nothing about this one, so taking ours around it would be
        // ordering two locks for no reason.
        auto* const player = RE::PlayerCharacter::GetSingleton();
        // ⚠ ALL THREE PUSHED KINDS, and facial hair was missing until
        // 2026-08-09: the push handles it (see PushPlayerHeadParts) but this
        // gate did not name it, so a character whose ONLY default was a beard
        // returned early and never got it reasserted after a load.
        const bool hasDefault =
            player && (!DefaultLook::For(player, DefaultLook::Part::kEyes).Empty() ||
                       !DefaultLook::For(player, DefaultLook::Part::kBrows).Empty() ||
                       !DefaultLook::For(player, DefaultLook::Part::kFacialHair).Empty());

        std::optional<Outfit> effective;
        {
            std::scoped_lock l(lock_);
            const auto*      o = EffectiveLocked();
            // ⚠ Re-checked under the SAME acquisition that copies it, exactly
            // as ReassertPlayerHairColor explains: between the caller's gate
            // and here the outfit can come off, and re-asserting a null one
            // would restore a baseline over eyes the editor just set. For a
            // RE-assert, doing nothing is always the safe answer.
            //
            // ⚠ THE ORIGINAL SAFETY IS UNCHANGED BY THE WIDENING. The case that
            // comment protects is "names nothing AND no default", which still
            // returns below. A default is not a baseline: it is a statement the
            // player made about this character, so asserting it is the feature.
            if (o && (!o->eyes.Empty() || !o->brows.Empty() ||
                      !o->facialHair.Empty() || hasDefault)) {
                effective = *o;
            }
        }
        if (!effective && !hasDefault) {
            return;
        }
        // Straight through the same push the outfit switch uses, so the recapture
        // runs and whatever the editor left becomes the new "their own". A null
        // outfit is a legitimate argument here rather than a miss: both keys
        // come out empty and the push falls straight to the default, which is
        // the whole point when a character has one and is wearing nothing.
        PushPlayerHeadParts(effective ? &*effective : nullptr);
    }

    OutfitSession::LookAuthorship OutfitSession::DefaultAuthorship() const {
        LookAuthorship out;
        auto* const   player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return out;  // every bit false, which is "no default is authoring"
        }
        // Copied under the lock and used outside it, the pattern every function
        // here that touches the engine follows: the rung below resolves forms
        // and asks HeadPart whether the character may wear them.
        std::optional<Outfit> effective;
        {
            std::scoped_lock l(lock_);
            if (const auto* o = EffectiveLocked()) {
                effective = *o;
            }
        }
        const auto authored = [&](HeadPart::Kind a_kind, const StyleRefKey& a_outfitKey) {
            RE::BGSHeadPart* outfitPart  = nullptr;
            RE::BGSHeadPart* defaultPart = nullptr;
            const auto [oNames, oUsable] =
                ResolveRung(player, a_kind, a_outfitKey, outfitPart, true);
            const auto [dNames, dUsable] =
                ResolveRung(player, a_kind,
                            DefaultLook::For(player, DefaultLook::PartFor(a_kind)),
                            defaultPart, true);
            return HeadPartLadder::Choose({ oNames, oUsable, dNames, dUsable }).wear ==
                   HeadPartLadder::Wear::kDefault;
        };
        out.hair = authored(HeadPart::Kind::kHair,
                            effective ? effective->hairStyle : StyleRefKey{});
        out.eyes = authored(HeadPart::Kind::kEyes,
                            effective ? effective->eyes : StyleRefKey{});
        out.brows = authored(HeadPart::Kind::kBrows,
                             effective ? effective->brows : StyleRefKey{});
        out.facialHair = authored(HeadPart::Kind::kFacialHair,
                                  effective ? effective->facialHair : StyleRefKey{});
        // ⚠ THE COLOUR LADDER IS NOT THE PART LADDER, and it is written out
        // rather than borrowed: a colour has no usability rung at all, because
        // there is no such thing as a shade this character is not offered.
        // Mirrors HairColor::Push exactly - the outfit wins where it names one,
        // and only then does the default author anything.
        out.hairColour = !(effective && effective->hairTint.set) &&
                         DefaultLook::HairColour(player).set;
        return out;
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
        // Beginning to stage an outfit is the OTHER way one becomes what the
        // player is showing, and it does not pass through UpdateStaging: every
        // editor tab switch, "+", Equipped-gear tab and post-Apply re-stage
        // arrives here instead. Without this, picking an outfit whose tint
        // differs from the last one left the previous colour on the character
        // until some unrelated slot was edited - exactly the symptom hair
        // visibility had before it was added to the classifier.
        PushPlayerLook(&a_from);
        RequestRefresh();
    }

    void OutfitSession::BeginStaging(RE::ActorHandle a_target, const Outfit& a_from,
                                     const Outfit* a_playerPreview) {
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
            // No preview source means the caller can show the follower herself,
            // so there is nothing for a stand-in to do. Both stay empty, and
            // every mannequin read below is already written to treat empty as
            // "this session has none" rather than as an error.
            const bool wantMannequin = !forPlayer && a_playerPreview != nullptr;
            playerMannequinBase_ = wantMannequin
                                       ? std::optional<Outfit>{ *a_playerPreview }
                                       : std::nullopt;
            playerMannequin_ = wantMannequin
                                   ? std::optional<Outfit>{ MakeMannequinPreview(
                                         ComposeMannequinSource(
                                             false, *playerMannequinBase_, a_from),
                                         blocklist_) }
                                   : std::nullopt;

            RecomputeWeaponStylingLocked();
            // Rebuild the snapshot to publish a NEW NPC override, or to DROP a
            // prior one when switching back to the player. A pure player->player
            // stage with no prior NPC override leaves the snapshot untouched.
            if (!forPlayer || wasNpcStaging) {
                RebuildSnapshotLocked();
            }
        }
        // Same reason as the player overload above, for a target that IS the
        // player. A follower target is pushed by the RequestRefreshActor below
        // instead, which resolves the staged override itself.
        //
        // Either way the player mannequin is never tinted, and that holds without
        // a special case: MakeMannequinPreview and ComposeMannequinSource overlay
        // slot and weapon entries onto the player's captured equipped gear and
        // copy no hair state at all, and HairStateForNpc never runs for the
        // player. Painting the player's own base colour for a follower preview
        // would be persistent state written for a transient viewport.
        if (forPlayer) {
            PushPlayerLook(&a_from);
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
            // The player still gets a refresh with no mannequin in play, and it
            // is not wasted: switching from a target that HAD one to a target
            // that does not is exactly when the old stand-in has to come off.
            RequestRefresh();
        }
    }

    void OutfitSession::UpdateStaging(const Outfit& a_next) {
        bool            forPlayer = true;
        RE::ActorHandle target;  // captured under the lock for the NPC refresh below
        RefreshGate::StagedUpdate refresh = RefreshGate::StagedUpdate::kNone;
        // The eight things an edit can have changed, which together decide
        // whether this push needs an equipment pass, a body-only pass or
        // nothing at all. Computed under the lock below and handed to
        // ClassifyStagedUpdate.
        int  diffSlots = 0;
        bool diffBody = false, diffHair = false, diffHairTint = false;
        bool diffHairStyle = false, diffDye = false, diffHeadParts = false;
        bool diffEyeTint = false;
        {
            std::scoped_lock l(lock_);
            if (!staged_) {
                return;
            }
            diffBody      = BodyDiffers(*staged_, a_next);
            diffSlots     = ChangedSlotCount(*staged_, a_next);
            diffHair      = HairDiffers(*staged_, a_next);
            diffHairTint  = HairTintDiffers(*staged_, a_next);
            diffHairStyle = HairStyleDiffers(*staged_, a_next);
            diffDye       = DyeDiffers(*staged_, a_next);
            diffHeadParts = HeadPartsDiffer(*staged_, a_next);
            diffEyeTint   = EyeTintDiffers(*staged_, a_next);
            refresh = RefreshGate::ClassifyStagedUpdate(
                diffBody, diffSlots, diffHair, diffHairTint, diffHairStyle,
                diffDye, diffHeadParts, diffEyeTint);
            staged_   = a_next;
            forPlayer = stagedForPlayer_;
            target    = stagedTarget_;
            // Only when BeginStaging left a base behind. Without that guard an
            // edit would resurrect a mannequin this session deliberately did
            // not build, over an empty Outfit, and the follower's own preview
            // would end up competing with a stand-in wearing nothing.
            if (!forPlayer && playerMannequinBase_) {
                playerMannequin_ = MakeMannequinPreview(
                    ComposeMannequinSource(false, *playerMannequinBase_, a_next),
                    blocklist_);
            }
            RecomputeWeaponStylingLocked();
            if (!forPlayer) {
                RebuildSnapshotLocked();  // refresh the NPC override with the new outfit
            }
        }
        if (refresh == RefreshGate::StagedUpdate::kNone) {
            return;
        }
        // The staged outfit is what the subject is showing, so its colour lands
        // here - BEFORE either refresh branch below requests the rebuild that
        // carries it. See PushHairColor for why the order is not negotiable.
        //
        // ⚠ Nothing automated protects this ordering. It is a sequence of two
        // engine calls with no observable return, so moving this line below the
        // refresh requests compiles clean and leaves every test suite green
        // (verified 2026-07-30). The comment and a field check are the whole
        // defence. Do not move it.
        //
        // Ahead of the kBodyOnly branch on purpose: a tint change always
        // classifies kEquipment, so on the body-only path this only re-asserts a
        // colour that is already set, which Apply makes a no-op.
        //
        // Player only; a staged follower is pushed by RequestRefreshActor below,
        // which reads this same staged outfit back out of the session.
        if (forPlayer) {
            PushPlayerLook(&a_next);
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
            bareBodyPreview_          = false;
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
        // What the subject's hair goes BACK to once the preview is dropped.
        // Empty == Base gear, i.e. restore the character's own colour.
        std::optional<Outfit> reverted;
        {
            std::scoped_lock l(lock_);
            forPlayer   = stagedForPlayer_;
            hadNpcStage = staged_.has_value() && !stagedForPlayer_;
            hadMannequin = playerMannequin_.has_value();
            restorePreviewEquipment = presetPreviewSuppression_;
            oldTarget   = stagedTarget_;
            // Read the outfit the PLAYER actually has active, before the reset
            // below drops the staged one. Deliberately NOT EffectiveLocked():
            // that folds in the scene guard and beast-form suspension, which flip
            // asynchronously and describe whether the override is standing down,
            // not which outfit the user chose. Hair colour follows the CHOICE, so
            // a scene starting mid-discard cannot make this repaint a character.
            //
            // No follower branch here on purpose: an NPC discard kicks
            // RequestRefreshActor at the bottom of this function, and that task
            // resolves the follower's reverted outfit itself - from live state
            // AFTER this reset, which is the same answer without a second copy of
            // the rule.
            if (forPlayer) {
                if (const auto* o = library_.Active()) {
                    reverted = *o;
                }
            }
            staged_.reset();
            presetPreviewSuppression_ = false;
            bareBodyPreview_          = false;
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
        // Un-paint the previewed colour, before the refresh that carries it.
        // This is the one path where skipping the push would not merely be late,
        // it would be PERSISTENT: SetHairColor writes actor-base state that lands
        // in the save, so closing the editor or switching target without Apply
        // has to put the active outfit's colour back - or the character's own,
        // when what they go back to is Base gear.
        //
        // ⚠⚠ UNLESS A PROFILE APPLY IS IN FLIGHT. Apply closes the editor by
        // design, so this discard runs INSIDE the apply, and on 2026-08-22
        // 03:56 it pushed the OLD outfit's look one millisecond after the
        // character step flipped the sex flag: it recaptured pre-switch head
        // parts the step had just cleared, and computed fit and skin against
        // a half-switched character (Nord race, female sex). The apply's own
        // outfit step and settle refresh write the FINAL look moments later,
        // so the close pass here is not merely early, its content is wrong;
        // it stands down and says so.
        const bool applyInFlight = ProfileApply::InFlight();
        if (forPlayer) {
            if (applyInFlight) {
                spdlog::info(
                    "OutfitSession: staging discarded mid-apply; the apply's "
                    "own push and settle refresh carry the look, so the "
                    "close-path re-assert stands down.");
            } else {
                PushPlayerLook(reverted ? &*reverted : nullptr);
            }
        }
        // Symmetric self-refresh (see BeginStaging): a player discard reverts the
        // player, an NPC discard kicks the follower so its preview reverts NOW -
        // when the editor switches away or closes without Apply - instead of
        // lingering until the actor's next natural rebuild.
        if ((forPlayer && !applyInFlight) || hadMannequin) {
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

    void OutfitSession::SetBareBodyPreview(bool a_enabled, bool a_playerSubject) {
        bool            forPlayer = true;
        bool            staged    = false;
        bool            now       = false;
        RE::ActorHandle target;
        {
            std::scoped_lock l(lock_);
            staged = staged_.has_value();
            // ⚠⚠ NOT ANDed WITH staged. See the declaration: the Rules page has
            // no staged outfit by design and the AND is what made the button
            // dead there. What staging still decides is WHO, and that is in
            // BareBodyHideMask.
            const bool next = a_enabled;
            if (bareBodyPreview_ == next && bareBodySubjectIsPlayer_ == a_playerSubject) {
                return;  // the editor re-asserts this every frame; most are this one
            }
            bareBodyPreview_         = next;
            bareBodySubjectIsPlayer_ = a_playerSubject;
            now                      = next;
            // ⚠ WITH NOTHING STAGED THERE IS NO FOLLOWER TO REFRESH. The mask
            // below answers for the player alone in that case, so the refresh
            // that carries it is the player's.
            forPlayer                = staged ? stagedForPlayer_ : true;
            target                   = stagedTarget_;
        }
        // ⚠ THE FIRST HALF OF "did the checkbox reach the render". The first
        // cut reported nothing at all, so a tick that did nothing and a tick
        // that never arrived read identically from the field. It logs the ASK
        // beside the result, because those differing is the whole diagnosis:
        // the ask arriving as false means the UI, and the ask arriving true
        // while staged is false means the session. Two calls a session, so it
        // can be info.
        spdlog::info("bare view: {} (asked {}, staged {}, player {})",
                     now ? "ON" : "OFF", a_enabled, staged, forPlayer);
        // ⚠ NOTHING TO PUT BACK BY HAND ON THE WAY OFF, unlike the equipment
        // suppression above. That one culls object nodes itself and so owns a
        // restore; this changes only what the worn pass is TOLD, and the pass
        // runs its own show-before-cull sweep every time it fires. The refresh
        // below is what fires it.
        if (forPlayer) {
            RequestRefresh();
        } else {
            RequestRefreshActor(target);
            RequestRefresh();
        }
    }

    std::optional<std::uint32_t> OutfitSession::BareBodyHideMask(
        RE::Actor* a_actor, std::uint32_t a_wornCoverage) const {
        // ⚠ NO COVERAGE TEST HERE ANY MORE. An actor wearing nothing real is
        // the case this feature is FOR, not a case to skip; the styles come off
        // whatever the coverage says. See BareBodyDisplay.
        if (!a_actor) {
            return std::nullopt;
        }
        auto* const   player      = RE::PlayerCharacter::GetSingleton();
        const bool    actorPlayer = player && a_actor == player;
        std::uint32_t actorBase   = 0;
        if (!actorPlayer) {
            if (auto* base = a_actor->GetActorBase()) {
                actorBase = base->GetFormID();
            }
        }
        std::scoped_lock l(lock_);
        // OS-233: the first of the reconcile's two passes answers for the
        // player exactly as the bare view would, with no editor session behind
        // it. Same subtraction as below, so the two cannot drift.
        if (overlayReconcilePass_ && actorPlayer) {
            const std::uint32_t out = a_wornCoverage & ~(blocklist_ | kNeverHideMask);
            spdlog::debug("overlay reconcile: bare pass, hide 0x{:08X} of worn 0x{:08X} on the "
                          "player (styles off).",
                          out, a_wornCoverage);
            return out;
        }
        if (!bareBodyPreview_) {
            return std::nullopt;
        }
        // ⚠⚠ NO STAGED OUTFIT IS NOT NO ANSWER, and that was the bug the button
        // move exposed. The Rules page discards staging on purpose - it hands
        // the character back to the rules engine - so every page but that one
        // could undress and Rules could not (field 2026-08-19). The subtraction
        // is the same one the staged path and the OS-233 reconcile pass both
        // use; what is missing without staging is only the answer to WHO, and
        // the editor supplies that through SetBareBodyPreview.
        //
        // ⚠ THE PLAYER AND NOBODY ELSE. A follower is only ever the subject
        // through a staged session, so an unstaged bare view that answered for
        // an NPC would be undressing an actor the editor is not pointed at.
        if (!staged_) {
            if (!bareBodySubjectIsPlayer_ || !actorPlayer) {
                return std::nullopt;
            }
            const std::uint32_t out = a_wornCoverage & ~(blocklist_ | kNeverHideMask);
            spdlog::debug("bare: hide 0x{:08X} of worn 0x{:08X} on the player with nothing "
                          "staged (styles off).",
                          out, a_wornCoverage);
            return out;
        }
        // The same gate PreviewEquipmentSuppressionMask uses, and for the same
        // reason: the edited follower is the subject, and while she is off
        // screen the player mannequin is standing in for her, so the two have
        // to undress together or the page shows a dressed stand-in.
        const bool target =
            stagedForPlayer_ ? actorPlayer
                             : (!actorPlayer && actorBase != 0 &&
                                actorBase == stagedBaseFormID_);
        if (!target && !(actorPlayer && playerMannequin_)) {
            spdlog::debug("bare: armed but {:08X} is not the subject.",
                          a_actor->GetFormID());
            return std::nullopt;
        }
        const std::uint32_t out = a_wornCoverage & ~(blocklist_ | kNeverHideMask);
        // ⚠⚠ LOGGED ON EVERY ARMED PASS INCLUDING THE ZERO ONE, and the zero
        // one is the whole reason this line was moved below the coverage test.
        // The first cut returned above it, so the case that was silently doing
        // nothing was also the case that said nothing, and a feature that fails
        // exactly where it is quiet costs a field round every time.
        spdlog::debug("bare: hide 0x{:08X} of worn 0x{:08X} on {:08X} (styles off).",
                      out, a_wornCoverage, a_actor->GetFormID());
        return out;
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
            bareBodyPreview_          = false;
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
        //
        // ruleOverlay_ resets here too: a fresh save carries no rule state,
        // and without this a helmet-hide left composed from the PREVIOUS
        // save would still be riding EffectiveLocked the moment the new one
        // finishes loading, before any rule has evaluated against it.
        bool hadMannequin = false;
        {
            std::scoped_lock l(lock_);
            hadMannequin = playerMannequin_.has_value();
            library_.Deactivate();
            staged_.reset();
            presetPreviewSuppression_ = false;
            bareBodyPreview_          = false;
            stagedNpcKey_.reset();
            stagedBaseFormID_ = 0;
            stagedForPlayer_  = true;
            stagedTarget_     = {};  // reset the staging fields as a unit
            playerMannequin_.reset();
            suspended_        = false;
            ruleOverlay_.clear();
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
            const bool    stagedHere  = NpcResolve::StagedTargetMatches(
                staged_.has_value(), stagedForPlayer_, stagedBaseFormID_, base);
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

    int OutfitSession::FindOutfitIndexLocked(std::string_view a_name) const {
        if (a_name.empty()) {
            return -1;
        }
        for (std::size_t i = 0; i < library_.Count(); ++i) {
            if (const auto* o = library_.At(i); o && o->name == a_name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void OutfitSession::ActivateByNameLocked(std::string_view a_name) {
        library_.Deactivate();
        if (const int idx = FindOutfitIndexLocked(a_name); idx >= 0) {
            library_.Activate(static_cast<std::size_t>(idx));
        }
        RecomputeWeaponStylingLocked();
    }

    void OutfitSession::ActivateByName(std::string_view a_name) {
        bool found = false;
        {
            std::scoped_lock l(lock_);
            ActivateByNameLocked(a_name);
            found = library_.ActiveIndex() >= 0;
        }
        if (!a_name.empty() && !found) {
            spdlog::warn("Active outfit '{}' not found in the global library (renamed or "
                         "deleted from another save?) - deactivated.",
                         a_name);
        }
        RequestRefresh();
    }

    bool OutfitSession::ApplyRuleDecision(bool a_realGear, std::string_view a_outfitName,
                                          Rules::Overlay a_overlay) {
        // Membership check, overlay write, and base activation all happen
        // under this ONE acquisition. Splitting them across separate
        // scoped_locks (as an earlier version of this method did) would let
        // a reader observe the NEW overlay composed over the OLD base for
        // however long the gap lasted - EffectiveLocked has no way to know
        // the two writes belong together.
        {
            std::scoped_lock l(lock_);
            if (!a_realGear && !a_outfitName.empty() &&
                FindOutfitIndexLocked(a_outfitName) < 0) {
                // A rule naming an outfit this save does not have must be
                // inert. Falling through to ActivateByNameLocked would hit
                // its unknown-name branch, which DEACTIVATES - stripping the
                // player to real gear on a rule that was supposed to do
                // nothing. Return now, having touched neither ruleOverlay_
                // nor the active selection.
                return false;
            }
            ruleOverlay_ = std::move(a_overlay);
            if (a_realGear) {
                ActivateByNameLocked("");  // deactivate: the explicit real-gear base
            } else if (!a_outfitName.empty()) {
                ActivateByNameLocked(a_outfitName);
            }
            // else kKeep: overlay changed, base untouched. Overlays are
            // armor-slot-only (Rules::Compose never touches the weapon entry
            // arrays), so anyWeaponStyling_ cannot change here - no
            // RecomputeWeaponStylingLocked needed, unlike the two branches
            // above that go through ActivateByNameLocked.
        }
        RequestRefresh();
        return true;
    }

    void OutfitSession::ClearRuleOverlay() {
        std::scoped_lock l(lock_);
        ruleOverlay_.clear();
    }

    void OutfitSession::SetBlocklist(std::uint32_t a_mask) {
        std::scoped_lock l(lock_);
        blocklist_ = a_mask;
        // No weapon recompute: the blocklist is an ARMOR slot mask, so it cannot
        // change AnyWeaponStyling(). Give weapons a blocklist and it can.
    }

    namespace {

        // ⚠ THREE QUARTERS OF A SECOND, PUSHED OUT ON EVERY REBUILD. The
        // measured clear lands somewhere between 60 ms and 560 ms after the
        // Obody_ApplyMorph edge, and OBody sends the event more than once per
        // apply, so the re-assert waits out the whole burst rather than racing
        // the middle of it. Re-arming resets the clock.
        constexpr auto kPushUpReassertDelay = std::chrono::milliseconds(750);

        std::chrono::steady_clock::time_point g_pushUpReassertDueAt{};
        bool                                  g_pushUpReassertArmed{ false };

    }

    void OutfitSession::NoteBodyRebuilt() {
        g_pushUpReassertDueAt = std::chrono::steady_clock::now() + kPushUpReassertDelay;
        g_pushUpReassertArmed = true;
    }

    void OutfitSession::RunPushUpReassert() {
        if (!g_pushUpReassertArmed ||
            std::chrono::steady_clock::now() < g_pushUpReassertDueAt) {
            return;
        }
        g_pushUpReassertArmed = false;
        auto* const player    = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        const auto body = OutfitSession::GetSingleton().DisplayBody();
        const auto mode = body.drives ? body.pushUp : PushUpMode::kNone;
        if (mode == PushUpMode::kNone) {
            return;  // nothing to put back, and a clear is what the rebuild did anyway
        }
        // ⚠⚠ THE SURVIVAL CHECK IS THE LOOP GUARD AS WELL AS THE ECONOMY. A
        // rebuild that left our key alone must cost nothing, and a re-assert that
        // wrote unconditionally would refresh the body on every OBody apply for
        // the rest of the session.
        if (RaceMenuMorphApi::HasPushUp(player)) {
            return;
        }
        spdlog::info("PushUp: the rebuild cleared our morph key, so mode {} is "
                     "written again after it.",
                     static_cast<int>(mode));
        ApplyPushUpFor(player, mode);
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

    void OutfitSession::ReconcilePlayerOverlays() {
        auto* task = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        task->AddTask([] {
            auto* const player = RE::PlayerCharacter::GetSingleton();
            if (!player || !player->Get3D(false)) {
                return;
            }
            auto& self = GetSingleton();
            // ⚠⚠ BOTH PASSES IN THIS ONE TASK, ON PURPOSE. A frame is presented
            // between two drains and never inside one, and the whole reason the
            // repair is invisible is that the skin body attached by the first
            // pass is replaced by the second before anything draws it. Two
            // RequestRefresh calls would coalesce into one pass anyway (the
            // queued flag), which is exactly the same-model refresh that does
            // nothing.
            //
            // ⚠ THE PLAIN REFRESH, NOT RunPlayerRefreshWhenSafe. That gate waits
            // out a block by re-queueing itself, and a re-queue from inside a
            // drain runs in the same drain; a bounded spin at load is not worth
            // buying for a repair that skee's next attach can also make.
            {
                std::scoped_lock l(self.lock_);
                self.overlayReconcilePass_ = true;
            }
            try {
                REAug::RefreshPlayer(Settings::GetSingleton().sceneKick);
            } catch (...) {
                spdlog::error("OverlayReconcile: the bare pass threw.");
            }
            // ⚠ THE COUNT AFTER THE BARE PASS AND BEFORE THE STYLED ONE, which
            // the result line below cannot give. Field 2026-09-02 03:11: the
            // rebuild ran twice after loading an Umbrael save and reported 0
            // clones both times, and nothing said whether skee attached none
            // on the bare pass (the skin body never attached, or OverlayFix
            // skipped it) or attached them and the styled pass took them.
            spdlog::info("OverlayReconcile: after the BARE pass (styles off) the player "
                         "carries {} body overlay clone(s) on the 3p root, {} on the 1p "
                         "root; the styled pass runs next in this drain.",
                         OverlayReconcile::CountPlayerBodyClones(),
                         OverlayReconcile::CountPlayerFirstPersonClones());
            OverlayReconcile::LogOverlayCensus(player);
            {
                std::scoped_lock l(self.lock_);
                self.overlayReconcilePass_ = false;
            }
            try {
                REAug::RefreshPlayer(Settings::GetSingleton().sceneKick);
                QueuePlayerBodyStateAfterRefresh();
            } catch (...) {
                spdlog::error("OverlayReconcile: the styled pass threw.");
            }
            // The result, read HERE and not on a timer: skee's attach-time build
            // is synchronous inside the pass, so the count is final by this line,
            // and the first field round's timer fired before this task had even
            // drained (the queue sat behind an 850 ms load frame) and reported 0
            // for a rebuild that then landed 7.
            spdlog::info("OverlayReconcile: two-pass rebuild ran (bare, then styled, one drain); "
                         "the player carries {} body overlay clone(s) now, {} on the 1p root.",
                         OverlayReconcile::CountPlayerBodyClones(),
                         OverlayReconcile::CountPlayerFirstPersonClones());
        });
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
                // Colour first, rebuild second - the order the spike proved, and
                // here both halves sit in ONE task, so nothing can reorder them.
                // This is also the only push a follower gets: see PushNpcHairColor.
                PushNpcHairColor(actor);
                REAug::RefreshActor(actor, Settings::GetSingleton().sceneKick);
                // ⚠ AFTER the refresh, and that ordering is the whole reason a
                // reloaded style stays on. NpcHair::Apply QUEUES its attach, so
                // what matters is which task lands first: kicked from above this
                // line, the attach would drain ahead of any rebuild RefreshActor
                // queued and be wiped by it. Below, it lands after. Colour is
                // the opposite way round (push first, rebuild second) because a
                // colour is carried BY the rebuild rather than destroyed by it.
                PushNpcHairStyle(actor);
                PushNpcFacialHair(actor);  // OS-225, same ordering argument
                // Attached hair does not survive a HEAD rebuild. This refresh
                // passes kModel alone so it should not cause one, which is why
                // the call is a verify rather than a re-apply: it returns
                // immediately when our geometry is still there, and only rebuilds
                // when something actually threw it away. Cheap insurance against
                // the paths that DO rebuild heads and do not come through here.
                NpcHair::Reassert(actor);
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
                    const auto key = NpcKeyFor(base);
                    if (key && assignments.contains(*key)) {
                        RequestRefreshActor(a_actor.GetHandle());
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
        });
    }

}  // namespace OS
