#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ShapeOverlay.h"

namespace RE {
    class Actor;
}

// RaceMenu's NiTransform interface, for the Shape overlay.
//
// ⚠ THIS ONE IS PUBLISHED AND THE PRESET ONE WAS NOT. Two interfaces in
// RaceMenu's modder header are worth telling apart: skee's own main.cpp calls
// AddInterface for eleven names, and "NiTransform" is one of them while
// "Preset" is not. IPresetInterface is declared in the header and wired to
// nothing, which is why FacePresetApi could compile, run, and report "RaceMenu
// has no Preset interface" in the field on 2026-08-06. Check the registration
// and not the declaration before building on any of the others.
//
// ⚠ ONLY OUR OWN KEY IS EVER TOUCHED. Every call carries
// ShapeOverlay::kOwnedKey, and RemoveAllReferenceTransforms is deliberately not
// wrapped here at all: a preset in the reference load order already stores
// transforms under RMX_Head and RMXPlugin, so the broad clear would delete
// another mod's work. Same contract RaceMenuMorphApi holds for body morphs, and
// the reason it has no arbitrary-key overload either.
//
// ⚠ NOT GATED ON THE BODY STUDIO CHANNEL, unlike RaceMenuMorphApi. That gate is
// there because body morphs are that feature; shaping a character is not, and
// gating this would mean a normal build never ships it. RaceMenu absent is a
// supported state and simply leaves the page saying so.
namespace OS::NodeTransformApi {

    // Why the page cannot work, so it can say something true.
    //
    // ⚠⚠ THIS EXISTS BECAUSE THE PAGE WAS LYING. Available() is a bool and the
    // Shape page printed "RaceMenu is not loaded" whenever it came back false,
    // which is correct for exactly one of the four ways it can. The field found
    // it on Skyrim SE 1.5.97 (users on the Nexus page, 2026-08-29 onward: "I'm
    // using Skyrim SE 1.5.97 with Racemenu 0.4.16, and Fitting room says
    // Racemenu is not loaded"), where RaceMenu IS loaded and the refusal is the
    // version floor below. The other three skee consumers have carried a status
    // enum from the start and say "too old" properly; this one never did.
    //
    // ⚠ THE SHAPE OF IT MATCHES OverlayApi::Status AND SkinApi::Status on
    // purpose. Four consumers of one interface exchange should answer the same
    // question the same way, or a fifth will invent a fifth vocabulary.
    enum class Status : std::uint8_t {
        kNotRequested,     // Request has not run yet
        kNoMessaging,      // SKSE messaging unavailable
        kRaceMenuAbsent,   // no interface map came back
        kNoNiTransform,    // RaceMenu answered, the interface is not in the map
        kTooOld,           // the interface is there and older than the floor
        kReady,
    };

    struct ApplyResult {
        bool        available{ false };
        bool        actorLoaded{ false };
        bool        refreshed{ false };
        std::size_t bonesWritten{ 0 };
        std::size_t bonesCleared{ 0 };
    };

    // Acquire the interface. Call once at kPostPostLoad, for the reason
    // ObodyApi gives: skee does not answer earlier, so requesting from
    // kPostLoad silently gets nothing.
    void Request();

    [[nodiscard]] bool          Available();
    [[nodiscard]] std::uint32_t Version();
    [[nodiscard]] Status        GetStatus();

    // A translation key naming why the page cannot work, empty when it can.
    // Same contract SkinApi::UnavailableKey holds.
    [[nodiscard]] const char* UnavailableKey();

    // What the interface reported when it was refused for age, so the page can
    // print the two numbers rather than asking the player to find a log. Zero
    // when the refusal was not a version one.
    [[nodiscard]] std::uint32_t RefusedVersion();

    // Push a complete plan. Bones marked remove drop our override; the rest are
    // written. One scenegraph update runs after the whole batch, never per
    // bone.
    //
    // ⚠ SAFE TO CALL FROM THE RENDER THREAD. The editor draws through FUCK's
    // Present hook, so the engine work is marshaled onto the game thread by
    // handle, exactly as BodyWeight::Set does. Nothing here dereferences the
    // actor on the calling thread, which is also why the result cannot report
    // what the engine did - it reports what was queued.
    ApplyResult Apply(RE::Actor* a_actor, const std::vector<ShapeOverlay::Adjustment>& a_plan);

    // Drop every override this mod owns on this character, and nothing else.
    ApplyResult ClearOwned(RE::Actor* a_actor);

    // What is currently stored under our key, one entry per bone the overlay
    // knows about.
    //
    // ⚠ READS ON THE CALLING THREAD, unlike Apply. These are map lookups in
    // RaceMenu's own store rather than scenegraph work, and the page needs the
    // answer in the frame it opens; a marshaled read would arrive a frame after
    // the sliders were already drawn from defaults, which is the flicker the
    // hair colour picker used to have.
    [[nodiscard]] std::vector<ShapeOverlay::Reading> Read(RE::Actor* a_actor);

}  // namespace OS::NodeTransformApi
