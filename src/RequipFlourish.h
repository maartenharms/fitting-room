#pragma once

#include <cstdint>

// The pure half of the requip transition (OS-206), split out for the reason
// DyeFlash.h and DyeGate are: the timing rules are testable and the scenegraph
// write around them is not.
//
// ⚠ THIS FILE LEARNS NOTHING ABOUT WHAT A GARMENT LOOKED LIKE. Every curve
// returns a mix in 0 to 1 and the engine side lerps between the value it
// recorded and the peak. A curve that returned an absolute emissive multiplier
// would be wrong on any material whose author did not start at 1.0, and the
// caller already holds the original in its restore record.
namespace OS::RequipFlourish {

    // The outgoing garment burns away. Short, because the swap waits on it and
    // every millisecond here is a millisecond of the old outfit still on screen
    // after the player asked for the new one.
    inline constexpr double kBurnSeconds = 0.18;

    // The incoming garment resolves out of the light. Longer than the burn: the
    // player is looking at the thing they chose, so this half is the one worth
    // spending time on.
    inline constexpr double kCondenseSeconds = 0.30;

    // ⚠ WELL ABOVE DyeFlash::kLitEmissiveMult (6.0), ON PURPOSE. That one marks
    // a shape so the eye can find it and the garment must stay readable
    // underneath. This one has to wash the garment out completely, because the
    // detail going missing is what lets the swap happen unseen.
    inline constexpr float kPeakEmissiveMult = 14.0f;

    // How many frames Wait may sit there before the whole thing is abandoned.
    //
    // ⚠ A BACKSTOP, NOT A TUNING KNOB. Half a second at 60 fps and several
    // times the worst rebuild the stutter work measured. It exists because the
    // alternative to giving up is a garment parked at alpha 0 forever, which
    // the player reads as FR deleting their gear. If this ever fires in the
    // field the rebuild is the bug, not this number.
    inline constexpr std::uint32_t kWaitFrameBudget = 30;

    namespace detail {
        [[nodiscard]] inline constexpr double Unit(double a_elapsed, double a_span) {
            if (a_span <= 0.0 || a_elapsed >= a_span) {
                return 1.0;
            }
            return a_elapsed <= 0.0 ? 0.0 : a_elapsed / a_span;
        }
    }

    // How far the outgoing garment has travelled towards the peak.
    //
    // Ease out, so the light arrives early. The alpha below leaves late, and
    // the gap between the two is the whole effect: a garment that is already
    // glowing when it starts to go.
    [[nodiscard]] inline constexpr float BurnMix(double a_elapsed) {
        const double u = detail::Unit(a_elapsed, kBurnSeconds);
        const double f = 1.0 - (1.0 - u) * (1.0 - u);
        return static_cast<float>(f);
    }

    // How solid the outgoing garment still is.
    //
    // Ease in and cubed, so it holds near opaque and then drops. ⚠ THE RETURN
    // AT AND PAST kBurnSeconds IS LITERAL ZERO rather than the cubic's value,
    // because "almost gone" is a visible ghost and because Wait may hold this
    // frame's value for as long as the frame budget allows.
    [[nodiscard]] inline constexpr float BurnAlpha(double a_elapsed) {
        const double u = detail::Unit(a_elapsed, kBurnSeconds);
        if (u >= 1.0) {
            return 0.0f;
        }
        return static_cast<float>(1.0 - u * u * u);
    }

    // How much of the peak is still on the incoming garment. Ease out again, so
    // the real material arrives quickly and the tail is a fading rim rather
    // than a slow reveal.
    //
    // ⚠ LITERAL ZERO AT THE END, on BurnAlpha's terms: this is the value that
    // hands over to the restore, and a restore that starts from 0.02 of a peak
    // of 14 pops.
    [[nodiscard]] inline constexpr float CondenseMix(double a_elapsed) {
        const double u = detail::Unit(a_elapsed, kCondenseSeconds);
        if (u >= 1.0) {
            return 0.0f;
        }
        const double f = (1.0 - u) * (1.0 - u);
        return static_cast<float>(f);
    }

    enum class Phase : std::uint8_t {
        kIdle,      // nothing armed
        kBurn,      // the outgoing garments are going
        kWait,      // the refresh has been posted and the rebuild has not landed
        kCondense,  // the incoming garments are resolving
    };

    // ⚠ THE HANDOFF OUT OF kWait IS AN OBSERVATION, NOT A DEADLINE, and that is
    // the single most important line in this file. OutfitSession::RequestRefresh
    // POSTS a task; the rebuild lands whenever the queue drains and
    // Update3DModel finishes, and it coalesces against a refresh already in
    // flight, so it can land earlier than planned as easily as later. No
    // elapsed time proves anything about it. Only geometry under the slot does.
    //
    // Passing a_elapsed here at all is for kBurn, which owns a real clock.
    [[nodiscard]] inline constexpr Phase NextPhase(Phase a_phase, bool a_geometryReady,
                                                   double a_elapsed) {
        switch (a_phase) {
            case Phase::kIdle:
                return Phase::kIdle;
            case Phase::kBurn:
                return a_elapsed >= kBurnSeconds ? Phase::kWait : Phase::kBurn;
            case Phase::kWait:
                return a_geometryReady ? Phase::kCondense : Phase::kWait;
            case Phase::kCondense:
                return Phase::kCondense;
        }
        return Phase::kIdle;  // unreachable; idle, because that is the safe answer
    }

    enum class Teardown : std::uint8_t {
        kNotArmed,      // nothing is up, nothing to do
        kHold,          // running, and it keeps running
        kSessionEnded,  // kPreLoadGame, or the setting went off under it
        kActorGone,     // unloaded, cell change, death
        kSuperseded,    // a second swap arrived mid-flourish
        kWaitExpired,   // the rebuild never landed; take the pop
        kFinished,      // Condense ran out, the ordinary ending
    };

    // ⚠ ORDER IS DELIBERATE AND IT IS NOT "SOONEST FIRST", the same rule
    // DyeFlash::Classify carries and for the same reason. Every branch below
    // kHold ends it either way, so this only decides what the log says, and a
    // wrong reason in the log is how the next session hunts the wrong thing.
    //
    // ⚠ a_elapsed IS CONDENSE'S CLOCK ONLY. Burn's end is a phase change rather
    // than a teardown, so asking the clock here about kBurn would end the whole
    // flourish at the exact moment it was supposed to post the refresh.
    [[nodiscard]] inline constexpr Teardown Classify(Phase a_phase, bool a_actorAlive,
                                                     bool a_sessionLive, bool a_superseded,
                                                     double a_elapsed,
                                                     std::uint32_t a_waitFrames) {
        if (a_phase == Phase::kIdle) {
            return Teardown::kNotArmed;
        }
        if (!a_sessionLive) {
            return Teardown::kSessionEnded;
        }
        if (!a_actorAlive) {
            return Teardown::kActorGone;
        }
        if (a_superseded) {
            return Teardown::kSuperseded;
        }
        if (a_phase == Phase::kWait && a_waitFrames >= kWaitFrameBudget) {
            return Teardown::kWaitExpired;
        }
        if (a_phase == Phase::kCondense && a_elapsed >= kCondenseSeconds) {
            return Teardown::kFinished;
        }
        return Teardown::kHold;
    }

    // ⚠ A SWITCH WITH NO DEFAULT, ON PURPOSE. /we4062 is on, so a new Teardown
    // enumerator fails the build here rather than falling through to "keep
    // going". A reason to end the flourish that nobody wired up is exactly the
    // shape of a garment left at alpha 0, and that is the one failure this file
    // exists to make impossible.
    [[nodiscard]] inline constexpr bool EndsIt(Teardown a_t) {
        switch (a_t) {
            case Teardown::kNotArmed:
            case Teardown::kHold:
                return false;
            case Teardown::kSessionEnded:
            case Teardown::kActorGone:
            case Teardown::kSuperseded:
            case Teardown::kWaitExpired:
            case Teardown::kFinished:
                return true;
        }
        return true;  // unreachable; ends it, because that is the safe answer
    }

}  // namespace OS::RequipFlourish
