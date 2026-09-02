#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace OS {

    // No candidate. Returned for an empty pool, or one whose length is not a
    // multiple of 3 and therefore cannot be colour-major rgb triples; callers
    // must branch on it rather than indexing.
    inline constexpr std::size_t kNoColorMatch = static_cast<std::size_t>(-1);

    // Which colour in a_candidates is closest to (a_r, a_g, a_b).
    //
    // a_candidates is COLOUR-MAJOR rgb triples: a_candidates[3 * i] is colour
    // i's red. The span carries its own length, so unlike a raw pointer plus a
    // separate colour count, there is no way for a caller holding a flat buffer
    // to pass a count that disagrees with it - the colour count is derived here
    // as a_candidates.size() / 3, and a length that is not itself a multiple of
    // 3 can only mean a malformed call.
    //
    // Distance is the "redmean" weighting rather than naive RGB Euclidean: a
    // widely used low-cost stand-in for CIE94-style perceptual weighting,
    // markedly closer to perceived difference than flat Euclidean distance,
    // which is what matters here - the user judges the result on a character,
    // not in a number. Per channel, with rmean = (a_r + candidate_r) / 2:
    //   weight_r = 2 + rmean / 256
    //   weight_g = 4                        (fixed - no dependence on rmean)
    //   weight_b = 2 + (255 - rmean) / 256
    // Each weight is scaled by 256 to keep the arithmetic in integers, so
    // 512 = 2*256 and 767 = 2*256 + 255 (the "+255" is (255 - rmean)'s own
    // constant, folded into weight_b's scaled-up form). Each >> 8 below divides
    // that same scaling back out of its own term. weight_g needs no shift: it
    // was never scaled up, because it does not depend on rmean.
    //
    // Ties resolve to the LOWEST index, so one picked colour maps to one form
    // deterministically within a session and across sessions on an unchanged
    // load order.
    [[nodiscard]] inline std::size_t NearestColorIndex(
        std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b,
        std::span<const std::uint8_t> a_candidates) {
        if (a_candidates.empty() || a_candidates.size() % 3 != 0) {
            return kNoColorMatch;
        }
        const std::size_t count = a_candidates.size() / 3;

        std::size_t   best     = kNoColorMatch;
        std::uint64_t bestDist = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const std::int64_t cr = a_candidates[3 * i + 0];
            const std::int64_t cg = a_candidates[3 * i + 1];
            const std::int64_t cb = a_candidates[3 * i + 2];

            const std::int64_t rmean = (static_cast<std::int64_t>(a_r) + cr) / 2;
            const std::int64_t dr    = static_cast<std::int64_t>(a_r) - cr;
            const std::int64_t dg    = static_cast<std::int64_t>(a_g) - cg;
            const std::int64_t db    = static_cast<std::int64_t>(a_b) - cb;

            // Integer arithmetic throughout: no float, no rounding surprises, and
            // the tie test stays exact.
            const std::uint64_t dist = static_cast<std::uint64_t>(
                (((512 + rmean) * dr * dr) >> 8) + 4 * dg * dg +
                (((767 - rmean) * db * db) >> 8));

            if (best == kNoColorMatch || dist < bestDist) {
                best     = i;
                bestDist = dist;  // strict <, so the lowest index keeps a tie
            }
        }
        return best;
    }

}  // namespace OS
