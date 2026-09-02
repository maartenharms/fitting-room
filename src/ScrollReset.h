#pragma once

// Scrolling lists keep their position per ImGui window id, so a list left
// halfway down is still halfway down the next time its mode is opened. Switching
// mode from the rail should feel like arriving somewhere, not like resuming
// someone else's scroll, so the switch asks every list to go back to the top.
//
// ⚠ A COUNTER AND NOT A BOOL. The reset has to survive more than the frame the
// switch happens on: on that first frame a list is being laid out for the first
// time, its content height is not known yet, and a scroll set against an unknown
// extent gets clamped away. Two frames is enough for the extent to settle.
//
// ⚠ SetScrollHereY IS THE ONLY SCROLL SETTER FUCK EXPOSES, and it is version
// gated: on an older FUCK the inline wrapper silently does nothing. That is an
// acceptable failure here (lists keep their old position, exactly as before this
// existed) but it does mean this cannot be verified by reading the code alone.
//
// Lives in its own header because both EditorUI.cpp and RulesUI.cpp have lists
// to reset and neither includes the other.
namespace OS::ScrollReset {

    inline int g_frames = 0;

    // Called by whatever changes the editor's mode.
    inline void Request() { g_frames = 2; }

    // Call immediately after BeginChild, before any item in that child. ImGui
    // anchors SetScrollHereY on the previous line's cursor, which at that point
    // is the content start, so the target resolves to the top of the list.
    [[nodiscard]] inline bool Pending() { return g_frames > 0; }

    // Once per frame, after everything has drawn.
    inline void Tick() {
        if (g_frames > 0) {
            --g_frames;
        }
    }

}  // namespace OS::ScrollReset
