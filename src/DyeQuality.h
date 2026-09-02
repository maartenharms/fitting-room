#pragma once

#include <cstdint>

// OS-140: the arithmetic behind the preview and commit split, off engine.
//
// Spec: docs/superpowers/specs/2026-08-06-dye-texture-cost-spec.md.
// Plan: docs/superpowers/plans/2026-08-06-dye-texture-cost.md.
//
// ⚠ SPLIT OUT FOR THE SAME REASON DyeGate AND RefreshGate ARE. The decisions
// here are functions of numbers and nothing else, and DyeTexture.cpp around them
// touches a D3D11 device that no test can hold. Four wrong answers here would
// each cost a field run to find, and none of them needs one.
//
// ---- WHY THERE ARE TWO COSTS AND ONE FIX DOES NOT CURE BOTH ---------------
//
// Measured 2026-08-05 on one dressed character in the Abyss set.
//
// The CHURN is 528 builds in one evening, 526 of them distinct. Dragging the
// colour picker posts an edit per frame and the pump drains four per frame, so a
// drag at 60 fps builds roughly one 85.3 MiB texture per frame. ⚠ Queue
// coalescing already shipped and it fixed the LAG rather than the cost: it only
// collapses builds queued at the same instant. The preview cap is what fixes
// this one.
//
// The FLOOR is 882.7 MiB still resident against a 512 MiB budget after eviction.
// Those are the COMMITTED colours, referenced by live materials, so eviction
// correctly refuses to take them and the preview split never touches them. Only
// making the committed texture itself smaller moves this one.
namespace OS::DyeQuality {

    // Which of the two textures a colour gets.
    //
    // ⚠ THE PREVIEW IS BUILT FIRST EVEN WHEN NOTHING IS DRAGGING, and that is
    // deliberate rather than a shortcut. The same cheap-first path then covers a
    // save load, a cell change, a scheme apply and a paste, so every one of them
    // puts a dye on screen in a frame instead of after an 85 MiB build. The
    // responsiveness is a side benefit worth having on purpose.
    enum class Quality {
        kPreview,
        kCommit,
    };

    // Which source mip a build reads, and what size that makes its destination.
    struct Level {
        std::uint32_t mip{ 0 };
        std::uint32_t width{ 0 };
        std::uint32_t height{ 0 };
    };

    // ⚠ READING A LOWER MIP IS FREE AND EXACT, which is the single most useful
    // fact in the spec. `Src.Load(int3(xy, m))` reads level m, the source chain
    // is already resident, and a capped build therefore costs less GPU work as
    // well as less memory and needs no downsample pass of its own.
    //
    // a_capPx of 0 means no cap. a_mipLevels is the source's real chain length
    // and it is a BOUND rather than a hint: reading a level the source does not
    // have returns zeros instead of failing, so a texture that shipped without a
    // chain gets mip 0 at full size and says so by returning it.
    //
    // ⚠ THE CAP APPLIES TO BOTH SIDES, so a non-square texture keeps its aspect.
    // The loop stops as soon as neither dimension exceeds the cap.
    [[nodiscard]] constexpr Level PickLevel(std::uint32_t a_srcW, std::uint32_t a_srcH,
                                            std::uint32_t a_mipLevels,
                                            std::uint32_t a_capPx) {
        const std::uint32_t levels = a_mipLevels == 0 ? 1u : a_mipLevels;
        std::uint32_t       mip    = 0;
        if (a_capPx != 0) {
            while (mip + 1 < levels &&
                   ((a_srcW >> mip) > a_capPx || (a_srcH >> mip) > a_capPx)) {
                ++mip;
            }
        }
        Level out;
        out.mip    = mip;
        out.width  = a_srcW >> mip;
        out.height = a_srcH >> mip;
        // A mip chain bottoms out at one texel and never at none.
        if (out.width == 0) {
            out.width = 1;
        }
        if (out.height == 0) {
            out.height = 1;
        }
        return out;
    }

    // 4 bytes per texel at mip 0, and a full chain is 4/3 of that. Six slices on
    // the cube path.
    //
    // ⚠ THIS IS THE WHOLE FLOOR ARITHMETIC. The cache holds RGBA8 and the
    // sources are BC1 or BC7, so it is 4 bytes per texel against 0.5 or 1: a
    // 4096 square costs 85.3 MiB here and the BC1 original it came from is under
    // 11 MiB.
    [[nodiscard]] constexpr std::uint64_t ChainBytes(std::uint32_t a_w, std::uint32_t a_h,
                                                     std::uint32_t a_slices) {
        return (static_cast<std::uint64_t>(a_w) * a_h * 4ull * 4ull * a_slices) / 3ull;
    }

    // Has this texture's colour stopped changing for long enough to be worth a
    // full build.
    //
    // ⚠ AN IDLE RATHER THAN A MOUSE RELEASE, and the spec says why: the picker
    // is not the only way a colour arrives. An idle needs no editor plumbing and
    // covers a scheme apply, a paste and a save load as well. Prefer it unless
    // it measurably misbehaves.
    //
    // The backwards test is not decoration. A clock that reads backwards must
    // not be able to spend an 85 MiB build.
    [[nodiscard]] constexpr bool HasSettled(std::uint64_t a_nowMs, std::uint64_t a_changedAtMs,
                                            std::uint32_t a_settleMs) {
        return a_nowMs >= a_changedAtMs && (a_nowMs - a_changedAtMs) >= a_settleMs;
    }

    // ⚠ THE PREVIEW MUST EXIST FIRST. A source the preview build refused is one
    // the commit build would refuse too, and a commit queued ahead of its own
    // preview would spend the full allocation to discover that.
    //
    // ⚠ a_previewOnly IS THE HOVER PREVIEW'S FLAG (spec 2026-08-09). A colour
    // that only a hover ever asked for settles like any other, and without
    // this a hover held past iDyeSettleMs spends a full commit build on a
    // colour that was never committed. Any non-preview request for the same
    // colour clears the flag at the slot, so a real commit is never starved.
    [[nodiscard]] constexpr bool ShouldUpgrade(bool a_previewBuilt, bool a_commitBuilt,
                                               bool a_commitQueued, bool a_settled,
                                               bool a_previewOnly) {
        return a_settled && !a_previewOnly && a_previewBuilt && !a_commitBuilt &&
               !a_commitQueued;
    }

}  // namespace OS::DyeQuality
