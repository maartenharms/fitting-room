#pragma once

#include <algorithm>

namespace OS::OutfitTabAdd {

    [[nodiscard]] inline constexpr float IdleGrey() { return 0.42f; }

    [[nodiscard]] inline float CornerRadius(float a_resolutionScale) {
        return std::max(2.0f, 3.0f * a_resolutionScale);
    }

    struct Layout {
        float width{ 0.0f };
        float height{ 0.0f };
        float textX{ 0.0f };
        float textY{ 0.0f };
    };

    // The add affordance is drawn beside, not inside, the tab bar. Its height
    // must therefore come from a submitted tab rectangle. Deriving it again
    // from font and frame padding would recreate the FLICK mismatch where a
    // normal Button is taller and expands the complete row.
    [[nodiscard]] inline Layout Measure(float a_tabHeight, float a_textWidth,
                                        float a_textHeight, float a_framePaddingX) {
        Layout out;
        out.width  = std::max(1.0f, a_textWidth + a_framePaddingX * 2.0f + 1.0f);
        out.height = std::max(1.0f, a_tabHeight);
        out.textX  = std::max(0.0f, (out.width - a_textWidth) * 0.5f);
        out.textY  = std::max(0.0f, (out.height - a_textHeight) * 0.5f);
        return out;
    }

}  // namespace OS::OutfitTabAdd
