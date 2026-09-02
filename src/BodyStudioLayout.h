#pragma once

namespace OS::BodyStudioLayout {

    // Pure seam for the adaptive workbench. Width and font size are in the
    // same ImGui coordinate space; expressing the gate in em keeps it stable
    // across UI scale and resolution.
    [[nodiscard]] inline bool UseCompact([[maybe_unused]] bool a_gamepad, float a_width,
                                         float a_fontSize) {
        // Outfit and Dye both retain their split while a controller is active;
        // Body Studio should not jump to another information architecture just
        // because the last input event came from a pad. Thirty em is enough for
        // a 13-em library, spacing, and a useful slider workspace.
        return a_width < a_fontSize * 30.0f;
    }

    [[nodiscard]] inline float LibraryWidth(float a_width, float a_fontSize) {
        const float proportional = a_width * 0.32f;
        const float minimum = a_fontSize * 13.0f;
        const float maximum = a_fontSize * 17.0f;
        return proportional < minimum ? minimum
             : proportional > maximum ? maximum
                                      : proportional;
    }

}  // namespace OS::BodyStudioLayout
