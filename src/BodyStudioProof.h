#pragma once

#include "BodyPreset.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace RE {
    class Actor;
}

namespace OS::BodyStudioProof {

    struct ORefitPolicy {
        int           mode{ 0 };
        std::uint32_t torsoStyleMask{ 0 };
        std::uint32_t torsoHideMask{ 0 };
    };

    struct Baseline {
        std::string preset;
        bool        captured{ false };
    };

    struct Status {
        std::string message;
        bool        activeCustom{ false };
    };

    [[nodiscard]] bool   Enabled();
    [[nodiscard]] Status Snapshot(RE::Actor* a_actor);

    // Clear the per-process ownership latch and every pending gesture at a
    // load boundary. Round eighteen: the latch survived a save load, so the
    // newly loaded character's first custom apply read alreadyOwned and
    // skipped the OBody handoff it never made. Called at kPreLoadGame and on
    // the kNewGame path (coc-from-main-menu-skips-newgame).
    void ForgetSession();

    void QueueInstalled(RE::Actor* a_actor, std::string_view a_preset,
                        ORefitPolicy a_policy);
    // Production authoring path: preview the in-memory edit buffer directly.
    // Unlike QueueCustom, this performs no XML resolve and is safe to call for
    // every committed slider gesture. The game-thread queue coalesces by actor.
    void QueueCustomPreset(RE::Actor* a_actor, BodyPreset a_preset,
                           ORefitPolicy a_policy);
    void QueueBaseline(RE::Actor* a_actor, Baseline a_baseline,
                       ORefitPolicy a_policy);
    void QueueClearOwned(RE::Actor* a_actor);
    void QueueRefreshAndReassert(RE::Actor* a_actor);

    // Called after OBody's normal refresh requests when a readiness cycle
    // returns. The extra task hop deliberately reasserts custom ownership last.
    void OnOBodyReady();

}  // namespace OS::BodyStudioProof
