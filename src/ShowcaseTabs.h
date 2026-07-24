#pragma once

namespace OS::ShowcaseTabs {

    inline constexpr int kCurated = 0;
    inline constexpr int kDiscovered = 1;
    inline constexpr int kExported = 2;
    inline constexpr int kSourceCount = 3;
    inline constexpr int kNoForcedSource = -1;

    struct State {
        int source{ kDiscovered };
        int forcedSource{ kNoForcedSource };
    };

    // ImGuiTabItemFlags_SetSelected is a trigger, not a description of the
    // current tab. Reapplying it every frame queues the remembered tab again
    // while a mouse-selected tab is becoming active, which makes the click
    // bounce back one frame later. Only explicit programmatic navigation gets
    // one forced frame.
    [[nodiscard]] inline bool ShouldForce(const State& a_state, int a_source) {
        return a_state.forcedSource == a_source;
    }

    // A mouse click becomes authoritative when ImGui reports its tab active.
    // While a programmatic switch is in flight, keep the requested source so
    // the previously active tab cannot undo it before ImGui consumes the flag.
    [[nodiscard]] inline bool ObserveActive(State& a_state, int a_source, bool a_active) {
        if (!a_active || a_state.forcedSource != kNoForcedSource ||
            a_state.source == a_source) {
            return false;
        }
        a_state.source = a_source;
        return true;
    }

    inline void Request(State& a_state, int a_source) {
        a_state.source       = a_source;
        a_state.forcedSource = a_source;
    }

    inline void FinishTabBar(State& a_state) {
        a_state.forcedSource = kNoForcedSource;
    }

    [[nodiscard]] inline constexpr int Cycle(int a_source, bool a_next) {
        return a_next ? (a_source + 1) % kSourceCount
                      : (a_source - 1 + kSourceCount) % kSourceCount;
    }

}  // namespace OS::ShowcaseTabs
