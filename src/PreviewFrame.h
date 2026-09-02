#pragma once

// The preview cache's frame clock: the PRESENT counter, bumped in the one
// Present hook Fitting Room owns (DyeTexture's), never in Draw.
//
// ⚠ NOT A DRAW COUNTER, AND THAT IS THE WHOLE FILE. Whether FUCK calls an
// IWindow's Draw exactly once per Present is not established; if it ran twice,
// an epoch bumped at Draw entry would prune requests made by the first pass of
// the same frame. Stamping and pruning against Present is correct whatever
// FUCK does, and costs one relaxed increment per frame.

#include <atomic>
#include <cstdint>

namespace OS::PreviewFrame {

    inline std::atomic<std::uint64_t> g_present{ 0 };

    inline void BeginPresentFrame() {
        g_present.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] inline std::uint64_t Current() {
        return g_present.load(std::memory_order_relaxed);
    }

}  // namespace OS::PreviewFrame
