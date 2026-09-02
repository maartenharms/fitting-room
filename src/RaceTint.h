#pragma once

#include "OverlayPlan.h"  // Rgb

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RE {
    class Actor;
}

// What the RACE says about a character's tint slots: how many there are, what
// each one is for, which mask it paints through and what colour it starts at.
//
// ⚠⚠ THE RACE IS THE AUTHORITY ON WHAT A TINT SLOT MEANS, and reading it is
// what turns the Makeup page from one list of fifteen identical looking rows
// into the three different control sets it should always have been. MEASURED
// 2026-08-16 out of Skyrim.esm: 31 races carry tint slots, 1445 in total, each
// a TINI index with a TINP type, a TINT texture and, on 757 of them, a TIND
// default colour. Every structural type is exactly ONE slot per character
// whose texture is the SHAPE of that feature on that face. Only WarPaint and
// Dirt are multi-slot.
//
// ⚠⚠ AND CommonLibSSE-NG'S DECLARATION IS RIGHT FOR BOTH RUNTIMES HERE, which
// is worth saying out loud because the neighbouring file exists to work around
// a case where it is not. MEASURED in Ghidra on SE 1.5.97 and AE 1.6.1170, in
// the TINI branch of TESRace::Load on each build:
//
//   faceRelatedData[sex]  race + sex*8 + 0x4A8   both builds
//   FaceRelatedData       allocated 0xC8         both builds
//   ->tintMasks           +0x90                  both builds
//   TintAsset             allocated 0x68         both builds
//   index / type / file / presetDefault  0x00 / 0x02 / 0x08 / 0x18   both
//   presets colors / values / indices    0x20 / 0x38 / 0x50          both
//
// So this file uses the published members directly. The player's TINT MASK
// LIST is the one whose offset AE moves, and that lives in MakeupApi.
namespace OS::RaceTint {

    struct Slot {
        // Whether the race had a slot at this position at all. A character
        // whose live list is longer than their race's is not impossible: other
        // mods add layers, and a slot with nothing behind it simply offers no
        // default rather than borrowing the neighbouring one's.
        bool             known{ false };
        std::uint32_t    type{ 0 };
        std::string      texture;  // TINT, the race's own mask for this slot
        // ⚠ TIND IS NOT ON EVERY SLOT AND THE PATTERN IS THE POINT. MEASURED:
        // 757 of 1445 carry one, and of the 688 without, 649 are WarPaint. So
        // the structural slots a Reset is FOR almost all have a default colour
        // and the war paint slots, which have a clear instead, mostly do not.
        bool             hasTint{ false };
        OverlayPlan::Rgb tint{};
        // The race's own TESTexture object for this slot, as an address to
        // compare against. See MakeupApi's census: if a live layer's texture
        // pointer IS this object, then writing a texture path onto that layer
        // writes it into the RACE RECORD and every character of that race
        // wearing that slot changes with it.
        const void*      textureObject{ nullptr };
    };

    // Every slot the character's race and sex define, in the race's own order,
    // which is the order the engine builds the live list in.
    //
    // ⚠ EMPTY IS A SUPPORTED ANSWER, not an error: a target with no actor base,
    // no race or no face data for its sex gets nothing and the page simply
    // offers no defaults. Nothing here writes.
    [[nodiscard]] std::vector<Slot> Slots(RE::Actor* a_actor);

}  // namespace OS::RaceTint
