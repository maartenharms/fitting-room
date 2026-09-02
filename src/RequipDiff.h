#pragma once

#include "Outfit.h"  // SlotEntry, StyleRefKey, kBitCount

#include <array>
#include <cstdint>
#include <optional>

// Which armour slots the swap about to happen will actually change (OS-206).
//
// ⚠ ARRAYS RATHER THAN TWO Outfit REFERENCES, deliberately. Taking the
// extracted entries keeps one comparison with two callers feeding it, and it
// keeps this header out of Outfit's mutation API so a test can build an input
// with an aggregate initialiser.
namespace OS::RequipDiff {

    using SlotEntries = std::array<SlotEntry, kBitCount>;

    // ⚠ THE STYLE IS COMPARED ONLY WHERE THE KIND USES IT. A passthrough or
    // hidden slot keeps whatever StyleRefKey was last written to it, so a
    // wholesale struct comparison reports a change on a slot that looks
    // identical on screen. That is a violet flash on a garment nobody touched,
    // and on a full outfit swap it would arm most of the character.
    [[nodiscard]] inline bool SlotMoved(const SlotEntry& a_before, const SlotEntry& a_after) {
        if (a_before.kind != a_after.kind) {
            return true;
        }
        return a_before.kind == SlotEntry::Kind::kStyle && !(a_before.style == a_after.style);
    }

    // A mask over the 32 armour biped bits. Zero means nothing to do, and the
    // caller must treat zero as "never arm" rather than "arm with no shapes":
    // an armed flourish with an empty record set has nothing to put back and
    // waits for something else to take it down.
    [[nodiscard]] inline std::uint32_t ChangedMask(const SlotEntries& a_before,
                                                   const SlotEntries& a_after) {
        std::uint32_t mask = 0;
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            if (SlotMoved(a_before[bit], a_after[bit])) {
                mask |= (1u << bit);
            }
        }
        return mask;
    }

    // Pull the 32 armour entries out of an outfit so they can be compared later.
    //
    // ⚠ THE CALLER MUST TAKE THE "BEFORE" SNAPSHOT BEFORE IT MUTATES ANYTHING.
    // Both trigger sites change OutfitSession and then refresh, so a snapshot
    // taken next to the refresh call is a snapshot of the new state twice over,
    // and ChangedMask returns zero on every swap. That failure is silent: the
    // swap still works and the flourish simply never plays.
    [[nodiscard]] inline SlotEntries Snapshot(const Outfit& a_outfit) {
        SlotEntries out{};
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            out[bit] = a_outfit.EntryFor(bit);
        }
        return out;
    }

    // Whether the flourish may light a shape that the dye grid classified with
    // this reason.
    //
    // ⚠⚠ SKIN IS THE ONE THIS EXISTS FOR, AND IT WAS FIELD SEEN ON 2026-08-15.
    // The first build lit every geometry hanging off a changed biped slot. Skin
    // is worn as a TESObjectARMO in those same slots, so switching to Naked
    // left nothing in them but skin and the character's hands went flat violet.
    // A saturated emissive on skin does not read as magic, it reads as a
    // missing texture, which is what the design said would happen and what it
    // said to prevent. The implementation simply never asked.
    //
    // ⚠ IT REUSES DyeSkipReason RATHER THAN RE-READING THE MATERIAL, so the
    // classifier that already decides "this shape carries the skin tone" for
    // the dye grid decides it here too. A second reader of that answer would
    // drift from the first the next time a feature is added.
    //
    // ⚠ NOT DYEABLE IS A DIFFERENT QUESTION FROM NOT LIGHTABLE, and conflating
    // them is the easy mistake here. A glowing or parallax garment is refused a
    // dye stripe because dyeing it looks wrong; it is still part of the outfit
    // being swapped and it should still burn away with the rest of the set.
    //
    // ⚠ NO DEFAULT LABEL, so /we4062 makes a new DyeSkipReason fail the build
    // here rather than defaulting into "light it". A new character-side feature
    // silently painted would be this same bug returning.
    [[nodiscard]] inline constexpr bool RequipMayPaint(DyeSkipReason a_reason) {
        switch (a_reason) {
            case DyeSkipReason::kCharacterColour:  // kFaceGen: the skin tone
            case DyeSkipReason::kHair:
            case DyeSkipReason::kEyes:
            // ⚠ THE BUILD FOUND THESE TWO, NOT THE AUTHOR. Both are eye
            // reasons, and both were missed when this switch was first written;
            // /we4062 refused it rather than letting them default into "light
            // it". That is the whole argument for the missing default label.
            case DyeSkipReason::kEyeSecondColour:
            case DyeSkipReason::kEyeNeedsIris:
            // Arrived with head-part dye and is head geometry, which the requip
            // flourish never reaches: it arms shapes on the ARMOUR slots named
            // in a mask and the face node hangs off neither. False is therefore
            // the honest answer rather than a cautious one, and it is the same
            // answer the three character-side reasons above give.
            case DyeSkipReason::kHairOwnColour:
                return false;
            // Invisible until the blade draws blood, so lighting it spends the
            // flourish on geometry nobody can see (OS-123 found the same shape
            // claiming a dye stripe).
            case DyeSkipReason::kDecal:
                return false;
            case DyeSkipReason::kNone:
            case DyeSkipReason::kGlow:
            case DyeSkipReason::kUnsupported:
            case DyeSkipReason::kReflectiveOff:
            // Reachable only through the weapon walk, which the armour mask
            // never visits. Lightable by default rather than by intent; a
            // future weapon flourish has to make this a decision.
            case DyeSkipReason::kOffHandWeapon:
            case DyeSkipReason::kTorch:
                return true;
        }
        return false;  // unreachable; refuse, because refusing is the safe answer
    }

    // The entries an actor is wearing right now, or all-passthrough when the
    // actor has no active outfit. An absent outfit is a real state (the player
    // cycled back to their base gear) and it diffs correctly against a present
    // one, so it must not be an early return at the call site.
    [[nodiscard]] inline SlotEntries SnapshotActive(const std::optional<Outfit>& a_outfit) {
        return a_outfit ? Snapshot(*a_outfit) : SlotEntries{};
    }

}  // namespace OS::RequipDiff
