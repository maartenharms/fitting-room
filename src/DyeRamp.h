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
        kIridescent = 2,  // the ramp on the cubemap where there is one, nacre on a pearl carrier, flat elsewhere
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

    // ---- where a second colour can live on a shape -------------------------
    //
    // A two-stop dye needs a carrier for its second stop, and the shape decides
    // which one it has. The cubemap of a reflective shape carries it as an
    // angle sweep. The fuzz and coat constants of a Community Shaders True PBR
    // material carry it as a grazing-angle sheen (PbrPearl.h). A shape with
    // neither has only its diffuse, and on a diffuse a second colour can only
    // be a luminance-keyed ramp across the texture itself.
    enum class Carrier : std::uint8_t {
        kNone     = 0,  // diffuse only: cloth, leather, a PBR piece with no fuzz or coat
        kCubemap  = 1,  // an environment-mapped shape, the sweep on metal
        kPbrPearl = 2,  // a True PBR material whose flags declare fuzz or a coloured coat
    };

    // ⚠ THE CUBEMAP OUTRANKS THE PEARL. A reflective shape keeps the sweep it
    // has always had whatever its material's flags say; the pearl question is
    // only asked on a shape with no cubemap, which is where every True PBR
    // piece lands (Community Shaders answers kDefault for its own material and
    // PG Patcher stripped the cubemap). Keeping that order here, pure, is what
    // stops OutfitDye.cpp deciding it inside a condition no test compiles.
    [[nodiscard]] constexpr Carrier CarrierFor(bool a_shapeIsReflective,
                                               bool a_pbrPearlCarries) {
        if (a_shapeIsReflective) {
            return Carrier::kCubemap;
        }
        return a_pbrPearlCarries ? Carrier::kPbrPearl : Carrier::kNone;
    }

    // What a shape actually gets, once its carrier is known.
    //
    // ⚠ IRIDESCENT DEGRADES TO FLAT WHERE THERE IS NO CARRIER, AND TO NACRE ON
    // A PEARL CARRIER (2026-09-02). Through 1.1.7 it degraded to nacre on every
    // shape without a cubemap, on the argument that a plain skirt beside a
    // sweeping cuirass reads as a bug. Under softlight that nacre had been a
    // flat tint anyway, because the shader runs the diffuse ramp only under
    // recolour; d9c49f10 (1.1.6) forced recolour on that arm to stop a bright
    // pearl going white, and the luminance-keyed ramp ran on cloth for the
    // first time. The field called it ugly marbling, fine on metal and not on
    // cloth, and the user chose flat over a tamer ramp ("do 1"). So a shape
    // with nothing but its diffuse takes the primary colour flat, like a flat
    // dye, and the sweep on the metal half of a garment is the look.
    //
    // A True PBR pearl carrier keeps nacre: OutfitDye.cpp hands that ramp to
    // the fuzz (pearlRidesFuzz), or leaves it on the diffuse beside the coat
    // write for a coat-only piece, which is the 1.1.6 pearl mechanism exactly
    // as it was. Nacre dyes are the diffuse ramp by design and are degraded
    // nowhere.
    [[nodiscard]] constexpr Mode EffectiveMode(Mode a_declared, Carrier a_carrier) {
        if (a_declared != Mode::kIridescent) {
            return a_declared;
        }
        switch (a_carrier) {
            case Carrier::kCubemap:
                return Mode::kIridescent;
            case Carrier::kPbrPearl:
                return Mode::kNacre;
            default:
                return Mode::kFlat;
        }
    }

    // ---- which ONE target the ramp lands on --------------------------------
    //
    // ⚠ EXACTLY ONE, NEVER BOTH, AND THAT IS THE WHOLE TRICK. On a reflective
    // shape running iridescent the reflection is what changes with viewing
    // angle, so the ramp goes to the cubemap and the diffuse keeps the flat
    // tint. Ramping the diffuse as well would double the colour and read as a
    // stain rather than as a sheen. On a pearl carrier, and under a nacre dye
    // anywhere, the ramp goes to the diffuse instead. A shape with no carrier
    // under an iridescent dye ramps neither.
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
