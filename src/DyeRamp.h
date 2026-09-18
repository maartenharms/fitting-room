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
        kIridescent = 2,  // the ramp on the cubemap where there is one, the fuzz or coat on a pearl carrier, flat elsewhere
        // ---- the envmask modes (2026-09-04) --------------------------------
        //
        // A shape's own environment mask is the per-texel reflection strength
        // the engine reads (its .x), and that is the metal-versus-cloth line an
        // artist already drew. These three read it. They are not ramps: no stop
        // sweeps with light or luminance, so the carrier rule below never
        // touches them and both ramp predicates stay off. The per-shape answer
        // is DiffuseTakeFor's, asked of a shape's reflectivity and its mask.
        kMetal      = 3,  // hex on the reflective texels, the rest untouched
        kCloth      = 4,  // hex on the non-reflective texels, the rest untouched
        kTwoTone    = 5,  // hex on the cloth, hex2 on the metal
    };

    // ⚠ AN UNKNOWN BYTE IS FLAT, NOT THE LAST ENUMERATOR. A record written by a
    // later build carries a mode this one has never heard of, and the safe
    // reading of "I do not know what this is" is the behaviour that shipped
    // before modes existed. (Which is also what 1.1.8 makes of a save carrying
    // an envmask mode: flat, the colour kept, the split lost.)
    [[nodiscard]] constexpr Mode ModeFromByte(std::uint8_t a_raw) {
        switch (a_raw) {
            case 1:
                return Mode::kNacre;
            case 2:
                return Mode::kIridescent;
            case 3:
                return Mode::kMetal;
            case 4:
                return Mode::kCloth;
            case 5:
                return Mode::kTwoTone;
            default:
                return Mode::kFlat;
        }
    }

    [[nodiscard]] constexpr bool IsMaskMode(Mode a_mode) {
        return a_mode == Mode::kMetal || a_mode == Mode::kCloth || a_mode == Mode::kTwoTone;
    }

    // ---- what a shape gets under an envmask mode ----------------------------
    //
    // ⚠ THE ENGINE'S OWN REFLECTIVITY DECIDES, per shape and per texel, and
    // nothing here guesses at a garment. A shape the engine draws with an
    // environment map is metal wherever its mask says so, and metal everywhere
    // when it has no mask, because that is how the engine draws it. A shape the
    // engine does not reflect at all is cloth; a True PBR piece lands there too
    // for now, since its metallic map is a second reader and not this one.
    enum class Take : std::uint8_t {
        kUntouched = 0,  // no write at all: a metal dye on a cloth shape
        kPrimary   = 1,  // the dye's own colour, flat
        kSecond    = 2,  // hex2, flat: a twotone dye on an all-metal shape
        kSplit     = 3,  // a masked build; SplitFor says which side takes what
    };

    // Every ramp or flat mode answers kPrimary whatever the shape, which is the
    // request's own tint on the diffuse: exactly what those modes always did.
    [[nodiscard]] constexpr Take DiffuseTakeFor(Mode a_effective, bool a_reflective,
                                                bool a_hasMask) {
        if (!IsMaskMode(a_effective)) {
            return Take::kPrimary;
        }
        if (a_reflective && a_hasMask) {
            return Take::kSplit;
        }
        // One side of the line for the whole shape, so one flat answer.
        switch (a_effective) {
            case Mode::kMetal:
                return a_reflective ? Take::kPrimary : Take::kUntouched;
            case Mode::kCloth:
                return a_reflective ? Take::kUntouched : Take::kPrimary;
            default:  // kTwoTone
                return a_reflective ? Take::kSecond : Take::kPrimary;
        }
    }

    // Which side of a split build takes which colour. Inside is the reflective
    // side, the mask high; outside is the rest. This is what the paint path
    // builds its two-sided request from, so the decision lives here, tested.
    struct Split {
        Take inside;
        Take outside;
    };

    [[nodiscard]] constexpr Split SplitFor(Mode a_effective) {
        switch (a_effective) {
            case Mode::kMetal:
                return { Take::kPrimary, Take::kUntouched };
            case Mode::kCloth:
                return { Take::kUntouched, Take::kPrimary };
            case Mode::kTwoTone:
                return { Take::kSecond, Take::kPrimary };
            default:
                return { Take::kPrimary, Take::kPrimary };
        }
    }

    // ---- which map carries the split ----------------------------------------
    //
    // ⚠ "AUTHORED" IS THE TEST, NOT "BOUND" (field, 2026-09-04). The vanilla
    // iron war axe ships no _m.dds, so the engine binds a nameless default
    // into the mask slot and the texture set's mask path is empty; a split
    // keyed on that pointer was refused six times at the funnel (no identity
    // to key on) and the axe stayed undyed. The engine's own order is the
    // rule: the environment mask where the texture set names one, else the
    // normal map's alpha where it names that, else no map at all, which is
    // an all-metal shape to DiffuseTakeFor. The caller decides "usable" from
    // the texture's own name or its authored path, the funnel's own test.
    enum class MapSource : std::uint8_t {
        kNone        = 0,
        kEnvMask     = 1,  // read through red
        kNormalAlpha = 2,  // read through alpha
    };

    [[nodiscard]] constexpr MapSource MapSourceFor(bool a_envMaskUsable,
                                                   bool a_normalUsable) {
        if (a_envMaskUsable) {
            return MapSource::kEnvMask;
        }
        return a_normalUsable ? MapSource::kNormalAlpha : MapSource::kNone;
    }

    // The reflection (the cubemap, or the marker texel under Community
    // Shaders) IS the metal, so it takes the metal's colour: hex2 under
    // twotone, nothing under cloth, and the request's own tint everywhere
    // else, which is what every mode before these did.
    [[nodiscard]] constexpr Take ReflectionTakeFor(Mode a_effective) {
        switch (a_effective) {
            case Mode::kCloth:
                return Take::kUntouched;
            case Mode::kTwoTone:
                return Take::kSecond;
            default:
                return Take::kPrimary;
        }
    }

    // ---- where metal starts: the per-piece cut (2026-09-04) -----------------
    //
    // The analysis pass finds the two classes in a map's own histogram (Otsu
    // over a strided grid) and hands back their means; the cut is t of the way
    // up the gap between them, with t the channel's byte over 255. Byte 0 is
    // the dark mean (everything above the cloth is metal), 255 the bright one,
    // 128 halfway, which is where Otsu itself landed on every mask measured
    // (0.49 to 0.51 of the gap on the cloak and on vanilla iron).
    //
    // ⚠ PER PIECE, BECAUSE NO RULE FITS EVERY MAP (field, 2026-09-04). The
    // Imperial Dragon cloak's gold spreads flat from 0.10 to 0.75 and Otsu cut
    // it at 0.28, so the duller gold took the cloth colour; it wants a fifth
    // of the gap. Vanilla iron wants half: a fifth would put 69% of the
    // cuirass in the blend band. So the byte lives on the DyeChannel and the
    // slider is drawn per piece; the global stopgap was declined.
    [[nodiscard]] constexpr float CutFor(float a_mu0, float a_mu1, std::uint8_t a_byte) {
        const float t = static_cast<float>(a_byte) / 255.0f;
        return a_mu0 + t * (a_mu1 - a_mu0);
    }

    // The blend band either side of the cut, in the map's own units: inside
    // above hi, outside below lo, feathered between. a_feather is the band's
    // full width as a fraction of the class gap ([Dye] fDyeMetalMaskFeather,
    // 0.5 shipped).
    //
    // ⚠ THE LOW EDGE NEVER FALLS BELOW THE MIDPOINT OF THE DARK MEAN AND THE
    // CUT, and the band stays centred on the cut, so a low cut narrows the band
    // instead of sliding it over the cloth: what the slider calls metal is
    // fully metal and the cloth never catches the metal's colour. At 128 with
    // the shipped feather the clamp sits exactly on the band the modes shipped
    // with (a quarter of the gap each side), so the default picture is
    // unchanged by a byte. Byte 0 collapses the band onto the dark mean, a
    // hard edge there; the shader guards the degenerate window itself.
    struct Window {
        float lo;
        float hi;
    };

    [[nodiscard]] constexpr Window WindowFor(float a_mu0, float a_mu1, std::uint8_t a_byte,
                                             float a_feather) {
        const float cut   = CutFor(a_mu0, a_mu1, a_byte);
        float       half  = (a_mu1 - a_mu0) * a_feather * 0.5f;
        const float floor = (cut - a_mu0) * 0.5f;
        if (half > floor) {
            half = floor;
        }
        if (half < 0.0f) {
            half = 0.0f;
        }
        return { cut - half, cut + half };
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
    // A True PBR pearl carrier resolves to nacre here and never ramps its
    // diffuse: PearlRidesMaterial below hands the ramp to the fuzz or the coat
    // and the diffuse takes the first colour flat. Nacre dyes are the diffuse
    // ramp by design and are degraded nowhere, though the shipped palette no
    // longer declares one (2026-09-15).
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

    // ---- a pearl carrier's diffuse never ramps ------------------------------
    //
    // A True PBR pearl carrier's second stop lives in its fuzz or its coat,
    // one writer each (PbrPearl.h), so the diffuse under it takes the first
    // colour FLAT through the ordinary curve, like a flat dye. Through 1.1.9
    // only a fuzz piece was spared: a coat-only piece kept the luminance-keyed
    // ramp on its diffuse beside the coat write, which is the marbling the
    // user retired from the palette on 2026-09-15. A coat carries the second
    // colour as much as a fuzz does, so both spare the diffuse now.
    //
    // Takes an EFFECTIVE mode, like the two predicates above: on this carrier
    // an iridescent dye has already resolved to nacre, and a declared nacre
    // dye is the same case. A nacre dye on a shape with no carrier still
    // ramps its diffuse, exactly as 1.1.7 did.
    [[nodiscard]] constexpr bool PearlRidesMaterial(Carrier a_carrier, Mode a_effective) {
        return a_carrier == Carrier::kPbrPearl && RampsDiffuse(a_effective);
    }

}  // namespace OS::DyeRamp
