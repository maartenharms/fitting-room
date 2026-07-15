#pragma once

#include "Outfit.h"

#include <json/json.h>

#include <string>
#include <vector>

namespace OS::JsonCodec {

    // The one JSON shape an outfit has, everywhere it appears: the objects in
    // outfits.json's "outfits" array, showcase preset files, and per-outfit
    // exports all read and write THIS codec. Pure logic - no engine types -
    // so it stays unit-testable.
    //
    //   { "name": "...", "favorite": false, "slots": [
    //       { "slot": 32, "kind": "style", "mod": "Some.esp", "id": "0x000D62" },
    //       { "slot": 31, "kind": "hide" } ] }

    [[nodiscard]] Json::Value OutfitToJson(const Outfit& a_outfit);

    // Fills a_out from an outfit object. Slots outside 30-61, unknown kinds,
    // and empty style keys are skipped (same tolerance outfits.json has
    // always had). Returns false only when a_json is not an object.
    bool JsonToOutfit(const Json::Value& a_json, Outfit& a_out);

    // A showcase preset: one curated outfit + author metadata, shipped by an
    // armor mod as Data/SKSE/Plugins/FittingRoom/Presets/<unique-name>.json.
    // The file is FLAT: the outfit keys (name/slots) and the metadata keys
    // live on the same object, so the outfit codec above reads it verbatim.
    struct Preset {
        std::string              name;         // required; the imported outfit's name
        std::string              author;       // optional
        std::string              description;  // optional
        std::vector<std::string> requires_;    // optional; absent plugin = preset skipped
        Outfit                   outfit;
        std::string              file;  // set by the store, not the codec
    };

    inline constexpr int kPresetVersion = 1;

    // Parses a preset file's root object. On failure returns false and puts a
    // one-line author-readable reason into a_error (these lines end up in
    // OutfitSlots.log - they are the author's debugging surface).
    bool ParsePreset(const Json::Value& a_root, Preset& a_out, std::string& a_error);

    // The exact file ParsePreset accepts, from a built outfit. a_requires
    // should hold the non-vanilla plugins the outfit's styles reference.
    [[nodiscard]] Json::Value PresetToJson(const Outfit& a_outfit,
                                           const std::string& a_author,
                                           const std::string& a_description,
                                           const std::vector<std::string>& a_requires);

}  // namespace OS::JsonCodec
