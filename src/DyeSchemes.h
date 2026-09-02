#pragma once

#include "JsonCodec.h"  // Outfit.h (DyeChannel) + the shared RRGGBB colour rule

#include <json/json.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace OS {

    // A named set of colours, reusable across outfits and characters.
    //
    // A scheme stores COLOURS ONLY. No slot, no piece name, no garment: it is
    // applied through the same piece-order mapping a paste uses
    // (MapColoursToSlot), so a set made on a three-piece cuirass still does
    // something sensible on one-piece boots. That is also what makes it survive
    // tier 2 unchanged, when shapes become authored mask regions.
    struct DyeScheme {
        std::string             name;
        std::vector<DyeChannel> colours;
    };

    // ⚠ GLOBAL FILES, DELIBERATELY NOT THE CO-SAVE.
    // Data/SKSE/Plugins/FittingRoom/DyeSchemes/*.json, one file per scheme,
    // beside the existing favorites.txt, known_plugins.txt, Presets/ and
    // Exports/. A scheme belongs to the INSTALLATION rather than to a save: it
    // is worth being hand-editable and shareable, and the co-save codec is at
    // LIBR v8 / NPCO v8 with no downgrade path already, so another version bump
    // for something that is not save state would be a cost with no benefit.
    //
    // Pure: no engine types, so this compiles into the test executable.
    namespace DyeSchemes {

        // Codec. Colours serialise as the same bare RRGGBB hex the hairColor
        // field and the outfit "dyes" array already use.
        [[nodiscard]] Json::Value ToJson(const DyeScheme& a_scheme);

        // Fills a_out. Returns false only when the object cannot be a scheme at
        // all, which in practice means it has no name; a malformed COLOUR is
        // dropped rather than guessed at, so a partly-corrupt file still loads
        // the colours it got right.
        bool FromJson(const Json::Value& a_json, DyeScheme& a_out);

        // The file a scheme's name maps to. The name is free text typed by the
        // user and is about to be pasted into a path, so separators, traversal
        // and the Windows-reserved characters are folded out. Exposed for the
        // tests: this is the security-relevant half of the store.
        [[nodiscard]] std::string FileNameFor(std::string_view a_name);

        // Scan the directory. Safe to call again to pick up hand-edited files.
        void Load();

        // Consistent copy for the render thread (the editor draw loop).
        [[nodiscard]] std::vector<DyeScheme> Snapshot();
        [[nodiscard]] std::size_t            Count();

        // Write one scheme, replacing any file of the same name, and refresh
        // the cache. False on an I/O failure or an empty name.
        [[nodiscard]] bool Save(const DyeScheme& a_scheme);

        // Delete one scheme by name. False when it was not there.
        [[nodiscard]] bool Remove(std::string_view a_name);

    }  // namespace DyeSchemes

}  // namespace OS
