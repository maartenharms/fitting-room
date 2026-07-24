#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace RE {
    class Actor;
}

namespace OS::ObodyApi {

    // Thin client for OBody NG's native plugin API (4.4.0+, added 2024-10-19).
    //
    // The vendored contract is extern/OBody_API.h, copied verbatim from
    // OBody-NG include/API/API.h - its own comment instructs consumers to copy
    // it wholesale, and both projects are GPL-3.0. Nothing of OBody is linked;
    // the interface arrives at runtime through an SKSE message, so a load order
    // without OBody simply never answers and every call below no-ops.
    //
    // ⚠ NOTHING IN THIS HEADER EXPOSES OBody TYPES. The vendored header needs
    // `Actor`/`TESForm` to exist as unqualified names before it is included, so
    // it is included in exactly one translation unit (ObodyApi.cpp) behind those
    // aliases. Leaking that requirement into our headers would force the alias
    // on every file that transitively includes this one.

    // Send the RequestPluginInterface message. Call from kPostPostLoad and no
    // earlier: OBody does not answer before that, and the header's own worked
    // example uses exactly that message.
    void Request();

    // Is OBody installed at all (did it answer our request)? True does NOT mean
    // it is safe to call - see Available(). Use this only to tell "OBody is
    // absent" from "OBody is present but momentarily unready" in the UI.
    [[nodiscard]] bool Present();

    // Safe to call right now: OBody answered AND is currently ready.
    //
    // ⚠ READINESS IS A CYCLE, NOT A LATCH. OBody goes UNREADY around saves and
    // comes back afterwards, and the API is explicitly unsafe to touch in that
    // window. Every accessor below re-checks this; do not cache the answer
    // across a frame boundary.
    [[nodiscard]] bool Available();

    // Preset names for the given sex, as OWNED copies.
    //
    // ⚠ THE COPY IS THE WHOLE POINT. GetPresetNames hands back string_views
    // that OBody only guarantees until the next OBodyIsNoLongerReady - i.e.
    // until the player saves. Holding those views in the editor's list would
    // read freed memory the first time someone saves with the panel open, on
    // the render thread, which is about the worst place to find out.
    [[nodiscard]] std::vector<std::string> PresetNames(bool a_female);

    // The preset OBody currently has assigned to the actor ("" if none/unready).
    [[nodiscard]] std::string AssignedPreset(RE::Actor* a_actor);

    // Ensure OBody has distributed/recorded a baseline assignment for this
    // actor before it is queried or overridden. Safe and idempotent.
    void EnsureProcessed(RE::Actor* a_actor);

    // Assign a preset by name. Empty name clears the assignment. Returns false
    // if unready or OBody rejected the name.
    bool AssignPreset(RE::Actor* a_actor, std::string_view a_presetName, bool a_applyMorphsNow);

    // Force ORefit on/off for one actor, independent of the global setting.
    void ForceORefit(RE::Actor* a_actor, bool a_applied);

    // The GLOBAL ORefit setting (the MCM toggle), for reporting only.
    [[nodiscard]] bool GlobalORefitEnabled();

    // ---- outfit glue -----------------------------------------------------

    // Apply an outfit's body settings to an actor.
    //
    // a_preset empty means "this outfit does not set a body", which REVERTS to
    // the captured baseline rather than doing nothing - see the note on
    // Baseline() for why that is the honest reading of "your usual body".
    //
    // a_orefit is Outfit's ORefitMode as an int (0 auto / 1 on / 2 off).
    // Auto resolves the two torso masks against the actor's real worn slots so
    // passthrough remains honest while styled and hidden slots follow the
    // visible transmog. The int keeps this header free of Outfit.h.
    void ApplyOutfitBody(RE::Actor* a_actor, std::string_view a_preset, int a_orefit,
                         std::uint32_t a_torsoStyleMask, std::uint32_t a_torsoHideMask);

    // Follower counterpart. Its baseline is owned and persisted by that
    // follower's NpcRecord, never by the player-global baseline below.
    void ApplyNpcOutfitBody(RE::Actor* a_actor, std::string_view a_preset, int a_orefit,
                            std::uint32_t a_torsoStyleMask,
                            std::uint32_t a_torsoHideMask,
                            std::string_view a_baseline,
                            bool a_baselineCaptured);

    // Stop enforcing an outfit's ORefit mode without changing the assigned
    // body preset. Used when no outfit currently drives the display, including
    // temporary scene suspension where reverting the whole body would flicker.
    void ReleaseOutfitORefit(RE::Actor* a_actor);

    // The actor's ORIGINAL OBody preset, captured the first time this mod ever
    // assigned one, so "Your usual body" has something to go back to.
    //
    // ⚠ CAPTURE-ONCE, AND IT MUST PERSIST. Re-capturing on a later assign
    // would record the PREVIOUS OUTFIT's preset as the baseline and lose the
    // real one forever - and the symptom (body never returns to normal) only
    // shows after the SECOND outfit switch, so a single-step test cannot see
    // it. These two exist so Persistence can round-trip it through the
    // co-save; without that, a reload re-captures from whatever the outfit
    // last set.
    [[nodiscard]] std::string Baseline();
    void                      SetBaseline(std::string_view a_preset, bool a_captured);
    [[nodiscard]] bool        BaselineCaptured();

}  // namespace OS::ObodyApi
