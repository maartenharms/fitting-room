#pragma once

#include <cstdint>
#include <vector>

namespace OS {

    // Resolves the doubled-first-keystroke seam (OS-46). While the editor is
    // open the keyboard reaches ImGui through TWO independent paths: WndProc
    // (WM_KEYDOWN/WM_CHAR - the primary source in menus, and it stamps a
    // "WM owns the keyboard" recency tick) and the mod's own engine input feed
    // (ImGuiOverlay::FeedEvent). A recency gate keeps the engine feed silent
    // while WM is active, but on the FIRST key after an idle pause the engine
    // event can be processed a beat BEFORE WndProc updates the tick - so both
    // paths deliver that key and the first character doubles.
    //
    // The fix defers the engine keyboard feed by one input frame. When the
    // engine believes WM is idle it HOLDS the key instead of feeding it; one
    // frame later, if WM has since claimed the keyboard (the key was really
    // WM's, just a beat behind) the held key is DROPPED - no double; if WM
    // stayed silent (it is genuinely dead - the menu-re-entry case the engine
    // feed exists to rescue, STATUS 15c) the held key is FED so typing still
    // works, one frame late. Time is the only signal that separates "WM idle
    // because the user paused" from "WM dead", so a one-frame wait is the
    // minimum correct fix. Pure logic - unit-tested; the overlay supplies
    // wmOwnsKeyboard (its recency gate) and replays the drained keys into ImGui.
    //
    // Touched only on the input thread (like the existing g_stickNavDir /
    // g_lastWmKeyTick state), so it needs no synchronization.
    class KeyboardArbiter {
    public:
        struct Key {
            std::uint32_t dik{ 0 };
            bool          down{ false };  // true = press, false = release
        };

        // An engine keyboard transition arrived. a_wmOwnsKeyboard = WndProc has
        // seen a WM key recently (the recency gate). If WM owns the keyboard the
        // event is WM's - drop it (and any earlier holds, since WM is proven
        // live); otherwise hold it one frame for Drain to feed or drop.
        void OnEngineKey(std::uint32_t a_dik, bool a_down, bool a_wmOwnsKeyboard) {
            if (a_wmOwnsKeyboard) {
                held_.clear();  // WM is live - it owns the keyboard
                return;
            }
            held_.push_back({ a_dik, a_down });
        }

        // Called once per input frame, BEFORE processing that frame's new
        // events. If WM claimed the keyboard since the keys were held, they were
        // WM's - return nothing (WM already fed them; feeding again would
        // double). Otherwise WM stayed silent across a frame - it is dead -
        // so return the held keys for the caller to feed to ImGui.
        [[nodiscard]] std::vector<Key> Drain(bool a_wmOwnsKeyboard) {
            if (held_.empty() || a_wmOwnsKeyboard) {
                held_.clear();
                return {};
            }
            std::vector<Key> out;
            out.swap(held_);
            return out;
        }

        void               Clear() { held_.clear(); }  // editor open/close
        [[nodiscard]] bool Empty() const { return held_.empty(); }

    private:
        std::vector<Key> held_;
    };

}  // namespace OS
