#pragma once

// ⚠ FOR kDyeNeutral, WHICH IS NOT REDEFINED HERE ON PURPOSE. The plan had this
// header declare its own copy of the byte, and Outfit.h has owned one since the
// picker shipped. Two constants that must stay equal is exactly the drift this
// blend cannot survive: the editor seeds a freshly enabled channel at that byte
// and offers a button that returns to it, so a paint path measuring from a
// different neutral would weaken toward a colour the editor calls no-change.
// Outfit.h is pure POD by its own rule and names no engine types, so the
// pure-logic test executables still link with nothing added.
#include "Outfit.h"

#include <cstdint>

namespace OS {

    // How much of a dye lands. 255 is the colour as chosen, 0 is the neutral,
    // and everything between is the colour walked toward it.
    //
    // ⚠ THIS IS NOT THE SCALING OutfitDye.h FORBIDS, AND IT READS EXACTLY LIKE
    // IT. That warning is about compensating for the overlay's non-linearity so
    // black and saturated textures respond, which cannot work: D = 0 and D = 1
    // are fixed points of the shader itself. Blending toward the identity is a
    // different operation with an exact meaning, "apply less of this dye", and
    // it makes no claim about how any particular texture answers.
    //
    // ⚠ THE INTERMEDIATE IS SIGNED. Every component below the neutral makes
    // (a_component - kDyeNeutral) negative, and in unsigned arithmetic that
    // wraps to a huge value and the dye comes out bright.
    //
    // ⚠ AND IT ROUNDS TO NEAREST RATHER THAN TRUNCATING, so the curve is
    // symmetric about the neutral and a colour equally far above and below
    // weakens by the same amount. Both endpoints are exact under either rule;
    // only the midpoints differ, which is why the choice is pinned by a test.
    [[nodiscard]] inline constexpr std::uint8_t ApplyDyeStrength(
        std::uint8_t a_component, std::uint8_t a_strength) {
        const int delta  = static_cast<int>(a_component) - static_cast<int>(kDyeNeutral);
        const int scaled = delta * static_cast<int>(a_strength);
        // Round half away from zero, so the two sides of the neutral round the
        // same way. Plain (x + 127) / 255 would bias the negative side.
        const int moved = scaled >= 0 ? (scaled + 127) / 255 : -((-scaled + 127) / 255);
        return static_cast<std::uint8_t>(kDyeNeutral + moved);
    }

    // ---- the slider's units ------------------------------------------------
    //
    // Stored as a byte because that is what the blend above multiplies by and
    // what the codec carries; SHOWN as a percentage because "Strength 191" says
    // nothing and "Strength 75%" says all of it (user 2026-08-08).
    //
    // ⚠ THE CONVERSION IS NOT SYMMETRIC AND MUST NOT PRETEND TO BE. 256 bytes
    // do not fit 101 steps, so a byte the codec loaded is only approximated by
    // its percentage; what has to hold is the OTHER direction, that a
    // percentage survives being turned into a byte and read back. Without that
    // the handle fights the cursor: the slider writes p, the next frame reads
    // the byte back as p-1, and the control crawls backwards under a held
    // mouse. The static_assert below walks all 101 rather than spot-checking,
    // because the failure is one value wide.
    [[nodiscard]] inline constexpr int DyeStrengthToPercent(std::uint8_t a_strength) {
        return (static_cast<int>(a_strength) * 100 + 127) / 255;
    }

    // Clamps rather than asserting: the slider is bounded, but a translation
    // file or a future caller is not, and a percentage out of range should pin
    // to an endpoint instead of wrapping through the cast.
    [[nodiscard]] inline constexpr std::uint8_t DyeStrengthFromPercent(int a_percent) {
        const int p = a_percent < 0 ? 0 : (a_percent > 100 ? 100 : a_percent);
        return static_cast<std::uint8_t>((p * 255 + 50) / 100);
    }

    namespace detail {
        [[nodiscard]] inline constexpr bool DyePercentRoundTrips() {
            for (int p = 0; p <= 100; ++p) {
                if (DyeStrengthToPercent(DyeStrengthFromPercent(p)) != p) {
                    return false;
                }
            }
            return true;
        }
    }  // namespace detail

    static_assert(detail::DyePercentRoundTrips(),
                  "a percentage must survive the byte and read back as itself, or "
                  "the slider handle walks away from the cursor");
    // The endpoints are the two the player will look at hardest, so they are
    // named as well as covered by the loop.
    static_assert(DyeStrengthFromPercent(100) == 255, "full strength is the byte the picker stores");
    static_assert(DyeStrengthFromPercent(0) == 0);
    static_assert(DyeStrengthToPercent(255) == 100, "a channel at full reads as 100%, never 99");
    static_assert(DyeStrengthToPercent(0) == 0);

}  // namespace OS
