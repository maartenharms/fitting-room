#pragma once

// The hover dye preview's decisions (spec 2026-08-09), kept pure for the same
// reason DyeFlash.h and DyeRamp.h are: the walk that consults this lives in
// OutfitDye.cpp and the hover that drives it lives in EditorUI.cpp, and no
// test compiles either.
//
// ⚠ THE WHOLE DESIGN CONSTRAINT, restated where the code is: this state is a
// transient overlay the paint walk consults ahead of the staged channel. It
// never touches g_staged, never reaches UpdateStaging, Persistence, the
// codec, ChangedDyeChannelCount or the history. A preview is painted, never
// stored, and a locked dye previewed grants nothing.

#include "DyeFlash.h"  // Key: a stripe named the way the editor names one
#include "Outfit.h"

#include <cstdint>

namespace OS::DyePreview {

    // The dwell before a hover arms, the same 0.18 s the style browser's
    // hover preview uses, so sweeping the grid costs nothing and resting on a
    // swatch answers quickly.
    inline constexpr double kArmDelaySeconds = 0.18;

    struct State {
        bool          armed{ false };
        std::uint32_t actorId{ 0 };  // FormID of the actor the editor is dressing
        DyeFlash::Key key{};         // the ringed stripe
        DyeChannel    channel{};     // exactly what a click would commit
    };

    // What one frame's hover does to the debounce. The caller owns the three
    // pieces of state (armed identity, pending identity, pending-since) and
    // this names the transition, so the sequence is testable.
    enum class Hover {
        kClear,   // no candidate: whatever is armed or pending comes down
        kKeep,    // armed candidate unchanged, or pending still inside the dwell
        kRepend,  // a new candidate starts its dwell
        kArm      // the pending candidate dwelt long enough
    };

    [[nodiscard]] constexpr Hover Decide(bool a_hasCandidate, bool a_sameAsArmed,
                                         bool a_sameAsPending, double a_now,
                                         double a_pendingSince, double a_delay) {
        if (!a_hasCandidate) {
            return Hover::kClear;
        }
        if (a_sameAsArmed) {
            return Hover::kKeep;
        }
        if (!a_sameAsPending) {
            return Hover::kRepend;
        }
        if (a_now - a_pendingSince >= a_delay) {
            return Hover::kArm;
        }
        return Hover::kKeep;
    }

    // Why a preview came down. Decided at the call sites rather than by a
    // classifier: the hover is re-derived every frame in the pane, so the
    // only job left is the log line saying the right reason.
    enum class Teardown {
        kUnhovered,     // the mouse left the swatch
        kCommitted,     // a click superseded it: the commit is on screen now
        kEditorClosed,  // the editor went away under it
        kTargetChanged  // the editor is dressing somebody else now
    };

    // Does the armed preview name THIS node walk? Armour matches on the clone
    // group's owner bit, because that is the bit the ringed tile names and the
    // bit the walk reads colour through; a garment on two slots previews on
    // both, which is one material and one answer, the walk's own rule.
    [[nodiscard]] constexpr bool Hits(const State& a_s, DyeTarget a_target,
                                      std::uint32_t a_viaSlot, WeaponClass a_cls,
                                      WeaponHand a_hand) {
        if (!a_s.armed || a_s.key.target != a_target) {
            return false;
        }
        if (a_s.key.channel >= kDyeChannelCount) {
            return false;
        }
        switch (a_target) {
            case DyeTarget::kArmour:
                return a_s.key.slotBit == a_viaSlot;
            case DyeTarget::kWeapon:
                return a_s.key.weaponClass == a_cls && a_s.key.weaponHand == a_hand;
            case DyeTarget::kEyes:
            case DyeTarget::kHair:
                // Hair is facegen-painted actor-base state the walk never
                // touches; a preview that wrote it would be the staged-write
                // bug in different clothes. The eye tile rides the head, not
                // the worn-slot walk, so the same answer holds.
                return false;
            // ⚠ NO HOVER PREVIEW ON A HEAD PART, AND THE COST IS WHY RATHER THAN
            // THE WALK. Unlike hair and eyes this one IS painted by a material
            // swap in a walk that could consult a preview, so the refusal is a
            // decision and not a consequence.
            //
            // What it would take is a head key on DyeFlash::Key, and that key is
            // a constexpr-comparable POD compared every frame. A head part is
            // identified by a slot plus a StyleRefKey, whose modName is a
            // std::string, so carrying it here puts a string allocation and a
            // string compare in the per-frame path and takes the key out of
            // constexpr. That is a real cost for a nicety on a tile that the eye
            // and hair tiles beside it do not have either.
            //
            // The consequence is honest and small: a horn takes its colour on
            // the click rather than on the hover. If this is ever wanted, interning
            // the part ref to a small id at the grid boundary is the way in, not
            // widening this key.
            case DyeTarget::kHeadPart:
                return false;
        }
        return false;
    }

    // The substitution itself: the staged slot dye with the previewed channel
    // in place of the staged one, when the key names this walk. The caller
    // computes paint-or-not off the RESULT, so a preview on an undyed slot
    // still paints.
    [[nodiscard]] constexpr SlotDye WithPreview(SlotDye a_dye, const State& a_s,
                                                DyeTarget a_target, std::uint32_t a_viaSlot,
                                                WeaponClass a_cls, WeaponHand a_hand) {
        if (Hits(a_s, a_target, a_viaSlot, a_cls, a_hand)) {
            a_dye.channels[a_s.key.channel] = a_s.channel;
        }
        return a_dye;
    }

}  // namespace OS::DyePreview
