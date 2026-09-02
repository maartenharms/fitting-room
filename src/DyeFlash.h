#pragma once

#include "Outfit.h"  // DyeTarget, WeaponClass, WeaponHand, ChannelForShapeIndex

#include <cmath>  // the burst curve
#include <cstddef>
#include <cstdint>

// The pure half of "flash the shapes one dye stripe owns" (OS-144), split out
// of the engine walk for the same reason DyeGate is: the lifetime rules are
// testable and the scenegraph write around them is not.
//
// ---- Why the restore is the whole feature -------------------------------
//
// Lighting a shape up is one field. Putting it back is four different
// situations, and a shape left glowing is worse than no flash at all: it is
// permanent from the player's side, it looks like the dye did it, and nothing
// on screen connects it to the click that caused it.
//
// ⚠ THIS RUNS THROUGH THE EMISSIVE PROPERTY, NEVER THE DYE PATH. Since OS-140
// a dye builds a texture per colour, so pulsing a colour would build textures
// at flash rate. The emissive route was already measured by the rung-2 spike:
// every property owns its own emissiveColor allocation, none is shared, none is
// null, and the tint goes through with the material untouched. OutfitDye's
// Swapped::Kind::kEmissive record and RestoreOne's emissive branch are the
// mechanism; this header is only the decision about when they run.
//
// ---- The four ways a flash comes down ------------------------------------
//
// Three of them are decisions and live in Classify below. The fourth is not a
// decision at all and deliberately is not here:
//
// ⚠ A 3D REBUILD IS NOT A BOOLEAN AND MUST NOT BE ONE. The engine builds a
// fresh partClone and unhooks the old one without telling anybody, so there is
// no flag to read at gate time and any flag invented for it would be believed
// after it went stale. It is caught at the write instead, by the same two
// questions OutfitDye::RestoreOne already asks: re-read the property off the
// geometry and write only if it is STILL the one that was lit, and only if the
// geometry is STILL under one of the actor's roots. A rebuilt shape fails both,
// and abandoning it is correct rather than merely safe: the geometry that was
// lit is gone from the scene, and the geometry that replaced it was never lit.
namespace OS::DyeFlash {

    // How long the whole burst lasts. Long enough to find the piece on a busy
    // character, short enough that it is over before you reach for the mouse
    // again.
    inline constexpr double kHoldSeconds = 1.05;

    // ⚠ A BURST, NOT A HOLD, AND THAT IS THE POINT (user 2026-08-06). A single
    // steady glow is easy to miss on a bright piece, in daylight, or on a set
    // where several shapes are already pale: the eye has nothing to catch,
    // because a still image gives it no change to lock onto. Three pulses do,
    // and they cost nothing extra - the shapes are recorded once and only the
    // multiplier moves after that.
    inline constexpr int kBurstPulses = 3;

    // ⚠⚠ THE OVERLAYS PAGE TAKES ONE PULSE, NOT THREE (user 2026-08-16:
    // "overlays don't just flash once they flash 3 times, only make it flash
    // once please"). The argument above still holds for a dye stripe, which is
    // a band of colour on a piece of armour that a still glow does not separate
    // from a highlight. An overlay layer is a texture the player has just put
    // on a face they are looking at, and they already know where it went: the
    // flash is a confirmation rather than a search aid, so three of them is
    // three times as long as the answer takes.
    inline constexpr int kSinglePulse = 1;

    // One pulse of the burst, so a single-pulse flash runs at the same SPEED as
    // one of the three rather than being stretched over the whole hold. A
    // raised cosine over a full second reads as a slow swell, which is a
    // different cue from the blink the burst gives.
    inline constexpr double kOnePulseSeconds = kHoldSeconds / kBurstPulses;

    // The peak. The rung-2 probe measured kOwnEmit already true and
    // emissiveMult already 1.0 on worn armour, so this drives something that is
    // already contributing rather than switching an unused channel on.
    inline constexpr float kLitEmissiveMult = 6.0f;

    // ⚠ THE TROUGH IS ZERO, NOT THE ORIGINAL VALUE, and that is what makes the
    // blink read as a blink. The flash also forces emissiveColor to white, so
    // holding the original multiplier between pulses would leave a shape whose
    // author picked black glowing steadily at the bottom of every cycle, and
    // three pulses would read as one long shimmer. Zero times white is nothing,
    // which is exactly the off state wanted. The ORIGINAL multiplier comes back
    // at teardown, from the record, and never from this curve.
    inline constexpr float kDarkEmissiveMult = 0.0f;

    // Where in the burst a given moment sits, as a multiplier to write.
    //
    // Raised cosine rather than a square wave: a hard on/off at this rate reads
    // as a rendering fault rather than as a deliberate cue, and it strobes.
    // Zero at both ends of every pulse and at both ends of the burst, so the
    // last pulse lands on dark and the restore below it cannot pop.
    //
    // Not constexpr, because std::cos is not. Pure and testable all the same,
    // which is the property that matters here.
    [[nodiscard]] inline float BurstMultAt(double a_elapsed, double a_hold,
                                           int a_pulses = kBurstPulses) {
        if (a_hold <= 0.0 || a_elapsed <= 0.0 || a_elapsed >= a_hold || a_pulses < 1) {
            return kDarkEmissiveMult;
        }
        const double phase = (a_elapsed / a_hold) * static_cast<double>(a_pulses);
        const double f     = phase - std::floor(phase);
        // 0 at f=0, 1 at f=0.5, 0 at f=1.
        const double shape = 0.5 - 0.5 * std::cos(f * 2.0 * 3.14159265358979323846);
        return static_cast<float>(shape * static_cast<double>(kLitEmissiveMult));
    }

    // Which stripe is lit. COMPARED, NEVER FOLLOWED: it names a stripe the same
    // way the editor names one, and nothing here reads through any of it.
    struct Key {
        DyeTarget     target{ DyeTarget::kArmour };
        std::uint32_t slotBit{ 0 };                       // kArmour only
        WeaponClass   weaponClass{ WeaponClass::Sword };  // kWeapon only
        WeaponHand    weaponHand{ WeaponHand::Both };     // kWeapon only
        std::size_t   channel{ 0 };
    };

    [[nodiscard]] inline constexpr bool operator==(const Key& a_l, const Key& a_r) {
        return a_l.target == a_r.target && a_l.slotBit == a_r.slotBit &&
               a_l.weaponClass == a_r.weaponClass && a_l.weaponHand == a_r.weaponHand &&
               a_l.channel == a_r.channel;
    }

    enum class Teardown {
        kNotArmed,       // nothing is lit, nothing to do
        kHold,           // lit and it stays lit
        kEditorClosed,   // the editor went away under it
        kTargetChanged,  // the editor is dressing somebody else now
        kSuperseded,     // a different stripe was clicked
        kExpired         // the hold elapsed
    };

    // ⚠ ORDER IS DELIBERATE AND IT IS NOT "SOONEST FIRST". The clock is asked
    // LAST, so a flash that both expired and lost its editor in the same frame
    // reports the editor. Every branch ends it either way, so this only decides
    // what the log line says, and a wrong reason in the log is how the next
    // session ends up hunting a timer that was never involved.
    //
    // a_targetSame is the caller's comparison rather than a target passed in:
    // the editor already owns that identity and this header names no actors.
    [[nodiscard]] inline constexpr Teardown Classify(bool a_armed, bool a_editorOpen,
                                                     bool a_targetSame, bool a_superseded,
                                                     double a_now, double a_until) {
        if (!a_armed) {
            return Teardown::kNotArmed;
        }
        if (!a_editorOpen) {
            return Teardown::kEditorClosed;
        }
        if (!a_targetSame) {
            return Teardown::kTargetChanged;
        }
        if (a_superseded) {
            return Teardown::kSuperseded;
        }
        if (a_now >= a_until) {
            return Teardown::kExpired;
        }
        return Teardown::kHold;
    }

    // ⚠ A SWITCH WITH NO DEFAULT, ON PURPOSE. /we4062 is on, so a new Teardown
    // enumerator fails the build here rather than falling through to "keep it
    // lit". A reason to take the flash down that nobody wired up is exactly the
    // shape of a shape left glowing forever, and it is the one failure this
    // whole file exists to make impossible.
    [[nodiscard]] inline constexpr bool EndsIt(Teardown a_t) {
        switch (a_t) {
            case Teardown::kNotArmed:
            case Teardown::kHold:
                return false;
            case Teardown::kEditorClosed:
            case Teardown::kTargetChanged:
            case Teardown::kSuperseded:
            case Teardown::kExpired:
                return true;
        }
        return true;  // unreachable; ends it, because that is the safe answer
    }

    // ---- what a stripe actually owns ---------------------------------------
    //
    // Tier 1 binds channels to shapes by traversal index, and the LAST channel
    // absorbs every shape from its own index on (see ChannelForShapeIndex). So
    // every stripe but the last owns one shape and the last one owns the tail,
    // which is why this cannot be "one stripe, one shape".
    [[nodiscard]] inline constexpr std::size_t ShapesOwnedByChannel(std::size_t a_channel,
                                                                    std::size_t a_shapeCount) {
        if (a_channel >= kDyeChannelCount) {
            return 0;
        }
        if (a_channel == kDyeChannelCount - 1) {
            return a_shapeCount > a_channel ? a_shapeCount - a_channel : 0;
        }
        return a_channel < a_shapeCount ? 1 : 0;
    }

    // ⚠ SINGLE-SHAPE GEAR IS NOT A CASE HERE ANY MORE, AND THE HISTORY IS WHY
    // IT LOOKS LIKE ONE SHOULD BE. On a garment the mesh author built as one
    // shape the only stripe owns the whole thing, so flashing it lights the
    // entire piece and picks nothing out. Until OS-164 that was a reason to
    // REFUSE, with a line in the pane saying so, and a `LightsTheWholeSlot`
    // predicate to detect it.
    //
    // OS-164 armed it anyway, for standardisation, which was right for a reason
    // the original decision underweighted: `LiveChannelCount`'s comment puts
    // 52.1% of worn armour meshes at exactly one shape, so "not worth arming"
    // was silently deciding most clicks in the game. OS-166 then dropped the
    // line too, by request, because the whole piece flashing IS the answer to
    // the click and narrating it was noise on the majority case.
    //
    // With neither a refusal nor a sentence left, the predicate had no caller
    // and was deleted rather than kept as documentation. Do not reintroduce one
    // without a consumer.

    // Nothing to light: a stripe past the end of what this garment has. Kept
    // apart from "owns everything" because only this one must never arm a flash
    // that then has nothing to restore.
    [[nodiscard]] inline constexpr bool OwnsNothing(std::size_t a_channel,
                                                    std::size_t a_shapeCount) {
        return ShapesOwnedByChannel(a_channel, a_shapeCount) == 0;
    }

    // Whether a click should arm anything at all. Kept beside the two
    // predicates above so the UI cannot consult one and forget the other.
    //
    // ⚠ OWNING NOTHING IS THE ONLY REFUSAL, and it is a mechanical one rather
    // than a judgement about what is worth seeing. A stripe past the end of this
    // garment has no shapes to light, so arming it would park a record with an
    // empty list, lit nothing, and wait for something else to take it down.
    // Every case that owns at least one shape arms, single-shape gear included.
    [[nodiscard]] inline constexpr bool WorthFlashing(std::size_t a_channel,
                                                      std::size_t a_shapeCount) {
        return !OwnsNothing(a_channel, a_shapeCount);
    }

}  // namespace OS::DyeFlash
