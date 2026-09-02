#pragma once

// The arithmetic behind a two-stop dye, kept pure so it is exercised.
//
// ⚠ NO ENGINE TYPES IN THIS HEADER, which is the same rule TutorialPlan.h and
// EditorGate.h follow and for the same reason: no test compiles OutfitDye.cpp or
// EditorUI.cpp, so a decision left in either reaches the field unexercised.

#include <cstdint>

namespace OS::DyeRamp {

    enum class Mode : std::uint8_t {
        kFlat       = 0,  // one hue, exactly what shipped before
        kNacre      = 1,  // two stops mapped over luminance, static
        kIridescent = 2,  // nacre pointed at the cubemap where there is one
    };

    // ⚠ AN UNKNOWN BYTE IS FLAT, NOT THE LAST ENUMERATOR. A record written by a
    // later build carries a mode this one has never heard of, and the safe
    // reading of "I do not know what this is" is the behaviour that shipped
    // before modes existed.
    [[nodiscard]] constexpr Mode ModeFromByte(std::uint8_t a_raw) {
        switch (a_raw) {
            case 1:
                return Mode::kNacre;
            case 2:
                return Mode::kIridescent;
            default:
                return Mode::kFlat;
        }
    }

    // How far the second stop travels from the first, by strength.
    //
    // ⚠ STRENGTH NARROWS THE GAP RATHER THAN FADING TOWARD GREY. A special dye
    // at 0 is its own first colour, not a washed-out one, so the slider tones a
    // loud pearl down without leaving the colour. Two equal stops are flat at
    // every strength, which is what makes a dye authored without hex2 identical
    // to one authored with hex2 equal to hex.
    [[nodiscard]] constexpr float NarrowStop(float a_first, float a_second,
                                             std::uint8_t a_strength) {
        const float t = static_cast<float>(a_strength) / 255.0f;
        return a_first + (a_second - a_first) * t;
    }

    // The same walk in the units the paint path actually holds.
    //
    // ⚠ BYTES IN, BYTE OUT, AND THAT IS NOT A CONVENIENCE. The stops are
    // authored as hex bytes and the caller hands the result straight to
    // ApplyDyeStrength, which takes and returns a byte. Narrowing in floats and
    // converting once at the end would round differently from the primary colour
    // sitting beside it, so at one strength the two stops would land a level
    // apart from where the same arithmetic put the first one.
    //
    // ⚠ ROUNDS HALF AWAY FROM ZERO, DELIBERATELY THE SAME RULE AS
    // ApplyDyeStrength. A second stop darker than the first is ordinary, so the
    // walk runs in both directions and both have to round symmetrically or the
    // ramp is not monotone under a dragged slider.
    [[nodiscard]] constexpr std::uint8_t NarrowStopByte(std::uint8_t a_first,
                                                        std::uint8_t a_second,
                                                        std::uint8_t a_strength) {
        const int delta  = static_cast<int>(a_second) - static_cast<int>(a_first);
        const int scaled = delta * static_cast<int>(a_strength);
        const int moved  = scaled >= 0 ? (scaled + 127) / 255 : -((-scaled + 127) / 255);
        return static_cast<std::uint8_t>(static_cast<int>(a_first) + moved);
    }

    // What a shape actually gets, once the shape is known.
    //
    // ⚠ IRIDESCENT DEGRADES TO NACRE, NEVER TO FLAT. The angle shift lives in
    // the cubemap, so a shape without one cannot have it. Falling back to flat
    // would leave the cloth half of a garment a plain colour beside a
    // shimmering metal half, which the fragmentation note already records as
    // reading like a bug. Nacre keeps both halves on the same two stops.
    [[nodiscard]] constexpr Mode EffectiveMode(Mode a_declared, bool a_shapeIsReflective) {
        if (a_declared == Mode::kIridescent && !a_shapeIsReflective) {
            return Mode::kNacre;
        }
        return a_declared;
    }

    // ---- which ONE target the ramp lands on --------------------------------
    //
    // ⚠ EXACTLY ONE, NEVER BOTH, AND THAT IS THE WHOLE TRICK. On a reflective
    // shape running iridescent the reflection is what changes with viewing
    // angle, so the ramp goes to the cubemap and the diffuse keeps the flat
    // tint. Ramping the diffuse as well would double the colour and read as a
    // stain rather than as a sheen. On every other shape there is no reflection
    // to carry it, so the ramp goes to the diffuse instead.
    //
    // ⚠ BOTH PREDICATES TAKE AN *EFFECTIVE* MODE. Pass a declared one and a
    // cloth shape asks for a cubemap it does not have; EffectiveMode above is
    // what resolves that, and it has to run first.
    [[nodiscard]] constexpr bool RampsDiffuse(Mode a_effective) {
        return a_effective == Mode::kNacre;
    }

    [[nodiscard]] constexpr bool RampsReflection(Mode a_effective) {
        return a_effective == Mode::kIridescent;
    }

}  // namespace OS::DyeRamp
