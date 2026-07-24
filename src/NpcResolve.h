#pragma once

#include "Outfit.h"    // DisplaySet, ComputeDisplaySet
#include "SlotMask.h"  // kBodySkinMask / kHeadPartMask

#include <cstdint>

// The pure render-decision core for the NPC/follower dimension. Everything the
// per-actor render decision reduces to that carries NO engine coupling lives
// here, header-only, so the same math the biped hooks run on the hot path is
// unit-tested without RE:: types (see tests/test_npcsession.cpp).
//
// Three decisions live here:
//   1. WornRequiredDisplay - NPC styles are visual and may fill an otherwise
//      unworn slot, matching the player. Hides remain limited to real gear.
//   2. SelectNpcSource - which outfit source a given assigned base resolves
//      to at snapshot BUILD time (suspension / staged-target override /
//      assigned-active), mirroring the player's EffectiveLocked precedence.
//   3. ShouldSuspendForRace - the spec §6 race-switch suspension rule:
//      whether the actor's CURRENT race is a beast/creature form with no
//      styleable humanoid biped (werewolf, vampire lord).
namespace OS::NpcResolve {

    // Styles are appearance choices, so they remain intact even if the actor
    // has no gameplay item in that slot (notably, a follower may wear a styled
    // helmet without first equipping a real helmet). Hide is different: it can
    // only remove real worn gear, so it is intersected with worn coverage. The
    // derived hide submasks are rebuilt from that intersection.
    // Armor path only: weapons/quiver are inherently worn-keyed by the funnel
    // and are never routed through here.
    //
    // a_wornCoverageMask is in the same editor-bit space as DisplaySet's masks
    // (bit = biped slot - 30): bit set == the actor wears an ARMO covering it.
    [[nodiscard]] inline DisplaySet WornRequiredDisplay(DisplaySet     a_in,
                                                        std::uint32_t  a_wornCoverageMask) {
        DisplaySet out;
        out.styleMask = a_in.styleMask;
        out.hideMask  = a_in.hideMask & a_wornCoverageMask;

        // Re-derive from the MASKED hideMask, identical to ComputeDisplaySet -
        // a hide bit dropped by the worn mask must fall out of every submask
        // too, or the hook would skin/cull a slot the actor does not wear.
        out.hiddenBodySkinMask   = out.hideMask & kBodySkinMask;
        out.hiddenAttachmentMask = out.hideMask & ~kBodySkinMask;
        out.hiddenHeadPartMask   = out.hideMask & kHeadPartMask;
        return out;
    }

    // Which outfit an assigned base resolves to when the snapshot is built.
    enum class NpcSource : std::uint8_t {
        kNone,            // suspended, or the base has no active outfit -> vanilla
        kAssignedActive,  // the base's own library's active outfit
        kStagedOverride,  // the editor is staging on THIS base - preview wins
    };

    // Precedence mirrors the player's EffectiveLocked: suspension (beast form /
    // race switch) stands the actor down FIRST, ahead of the editor's live
    // preview, so a race-switched follower never renders armor even mid-edit;
    // then a staged target overrides that one base; otherwise the base shows
    // its assigned active outfit (nothing if it has none). Scene suppression is
    // deliberately NOT folded in here: SceneGuard flips asynchronously and is
    // read lock-free on the hook (as in EffectiveLocked), so it gates at hook
    // time, keeping the built snapshot free of that async global.
    [[nodiscard]] inline NpcSource SelectNpcSource(bool a_suspended,
                                                   bool a_stagedTargetMatches,
                                                   bool a_hasActiveOutfit) {
        if (a_suspended) {
            return NpcSource::kNone;
        }
        if (a_stagedTargetMatches) {
            return NpcSource::kStagedOverride;
        }
        if (!a_hasActiveOutfit) {
            return NpcSource::kNone;
        }
        return NpcSource::kAssignedActive;
    }

    // §6 race-switch suspension rule. Takes a plain bool rather than an
    // RE::TESRace* so it stays unit-testable without RE:: types (see
    // tests/test_npcsession.cpp) - the caller (RaceSwitchSink) does the RE
    // keyword lookup (TESRace::HasKeyword(ActorTypeCreature)) and hands the
    // pure decision the answer.
    //
    // The axis is beast-ness, NOT "did the race change". A TESSwitchRace-
    // CompleteEvent fires for ordinary runtime vampirism too (Nord ->
    // NordRaceVampire), and its current-vs-base race DOES diverge - but a
    // regular vampire is a fully armor-wearing HUMANOID whose transmog must
    // stay visible, so a race-diff signal would wrongly suspend it for the
    // entire duration of vampirism. Only a beast/creature form (werewolf,
    // vampire lord - carrying ActorTypeCreature, unlike every humanoid race
    // including vampire races and Khajiit/Argonian) has no styleable
    // humanoid biped, so beast-ness is the correct, sufficient trigger. DO
    // NOT "simplify" this back to a current-vs-base race comparison.
    [[nodiscard]] inline bool ShouldSuspendForRace(bool a_currentRaceIsBeast) {
        return a_currentRaceIsBeast;
    }

}  // namespace OS::NpcResolve
