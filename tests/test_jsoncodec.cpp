// JSON codec tests. No SKSE, no engine - the codec operates on Json::Value.
#include "JsonCodec.h"
#include "SlotMask.h"

#include <json/json.h>

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static Json::Value Parse(const std::string& a_text) {
    Json::Value             root;
    Json::CharReaderBuilder rb;
    std::string             errs;
    std::istringstream      in(a_text);
    Json::parseFromStream(rb, in, &root, &errs);
    return root;
}

int main() {
    using namespace OS;

    {  // outfit -> JSON -> outfit round-trip
        Outfit o;
        o.name     = "Court Dress";
        o.favorite = true;
        o.SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        o.SetStyle(kBitFeet, StyleRefKey{ "Skyrim.esm", 0x1B3A3 });
        o.SetHide(kBitHair);
        o.obodyPreset = "Umbral's Umbrage Nerfed";
        o.orefit      = ORefitMode::kForceOff;

        const auto json = JsonCodec::OutfitToJson(o);
        Outfit     back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.name == "Court Dress");
        CHECK(back.favorite == true);
        CHECK(back.EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
        CHECK(back.EntryFor(kBitBody).style.modName == "Armors.esp");
        CHECK(back.EntryFor(kBitBody).style.localFormID == 0x801);
        CHECK(back.EntryFor(kBitFeet).style.localFormID == 0x1B3A3);
        CHECK(back.EntryFor(kBitHair).kind == SlotEntry::Kind::kHide);
        CHECK(back.EntryFor(kBitHands).kind == SlotEntry::Kind::kPassthrough);
        CHECK(back.obodyPreset == "Umbral's Umbrage Nerfed");
        CHECK(back.orefit == ORefitMode::kForceOff);
    }

    {  // hidden style survives player-library/export JSON save and reload
        Outfit o;
        o.SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        ToggleHideSlot(o, kBitBody);

        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(json["slots"][0]["kind"].asString() == "hide");
        CHECK(json["slots"][0]["mod"].asString() == "Armors.esp");
        CHECK(json["slots"][0]["id"].asString() == "0x000801");

        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.EntryFor(kBitBody).kind == SlotEntry::Kind::kHide);
        ToggleHideSlot(back, kBitBody);
        CHECK(back.EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
        CHECK(back.EntryFor(kBitBody).style ==
              (StyleRefKey{ "Armors.esp", 0x801 }));
    }

    {  // tolerance: bad slots, bad kinds, bad hex are skipped, not fatal
        const auto root = Parse(R"({
            "name": "Tolerant",
            "slots": [
                { "slot": 29, "kind": "hide" },
                { "slot": 62, "kind": "hide" },
                { "slot": 31, "kind": "invisible" },
                { "slot": 32, "kind": "style", "mod": "", "id": "0x0" },
                { "slot": 37, "kind": "style", "mod": "Boots.esp", "id": "0x00000A" },
                "not-an-object",
                { "slot": 35, "kind": "hide" }
            ]
        })");
        Outfit o;
        CHECK(JsonCodec::JsonToOutfit(root, o));
        CHECK(o.name == "Tolerant");
        CHECK(o.EntryFor(kBitHair).kind == SlotEntry::Kind::kPassthrough);   // bad kind
        CHECK(o.EntryFor(kBitBody).kind == SlotEntry::Kind::kPassthrough);   // empty key
        CHECK(o.EntryFor(kBitFeet).style.modName == "Boots.esp");
        CHECK(o.EntryFor(kBitFeet).style.localFormID == 0xA);
        CHECK(o.EntryFor(kBitAmulet).kind == SlotEntry::Kind::kHide);
        CHECK(o.StyleMask() == MaskForEditorSlot(37));
        CHECK(o.HideMask() == MaskForEditorSlot(35));
    }

    {  // JsonToOutfit rejects non-objects
        Outfit o;
        CHECK(!JsonCodec::JsonToOutfit(Json::Value("just a string"), o));
        CHECK(!JsonCodec::JsonToOutfit(Json::Value(Json::arrayValue), o));
    }

    {  // absent or invalid optional body keys default safely
        Outfit legacy;
        CHECK(JsonCodec::JsonToOutfit(Parse(R"({ "name": "Legacy", "slots": [] })"),
                                      legacy));
        CHECK(legacy.obodyPreset.empty());
        CHECK(legacy.orefit == ORefitMode::kDefault);

        Outfit invalid;
        CHECK(JsonCodec::JsonToOutfit(
            Parse(R"({ "name": "Invalid", "orefit": 99, "slots": [] })"), invalid));
        CHECK(invalid.orefit == ORefitMode::kDefault);

        Outfit malformed;
        CHECK(JsonCodec::JsonToOutfit(
            Parse(R"({ "name": "Malformed", "obodyPreset": {}, "orefit": "off" })"),
            malformed));
        CHECK(malformed.obodyPreset.empty());
        CHECK(malformed.orefit == ORefitMode::kDefault);
    }

    {  // preset happy path (flat schema: metadata + outfit on one object)
        const auto root = Parse(R"({
            "version": 1,
            "name": "Ebony Vanguard - Intended Look",
            "author": "SomeAuthor",
            "description": "The full set.",
            "requires": ["EbonyVanguard.esp"],
            "slots": [
                { "slot": 32, "kind": "style", "mod": "EbonyVanguard.esp", "id": "0x000D62" },
                { "slot": 31, "kind": "hide" }
            ]
        })");
        JsonCodec::Preset p;
        std::string       err;
        CHECK(JsonCodec::ParsePreset(root, p, err));
        CHECK(err.empty());
        CHECK(p.name == "Ebony Vanguard - Intended Look");
        CHECK(p.author == "SomeAuthor");
        CHECK(p.description == "The full set.");
        CHECK(p.requires_.size() == 1 && p.requires_[0] == "EbonyVanguard.esp");
        CHECK(p.outfit.name == p.name);
        CHECK(p.outfit.EntryFor(kBitBody).style.localFormID == 0xD62);
        CHECK(p.outfit.EntryFor(kBitHair).kind == SlotEntry::Kind::kHide);
    }

    {  // preset failure modes carry author-readable reasons
        JsonCodec::Preset p;
        std::string       err;

        CHECK(!JsonCodec::ParsePreset(Json::Value(Json::arrayValue), p, err));

        auto root = Parse(R"({ "version": 2, "name": "X",
                               "slots": [ { "slot": 31, "kind": "hide" } ] })");
        CHECK(!JsonCodec::ParsePreset(root, p, err));
        CHECK(err.find("version 2") != std::string::npos);

        root = Parse(R"({ "version": 1,
                          "slots": [ { "slot": 31, "kind": "hide" } ] })");
        CHECK(!JsonCodec::ParsePreset(root, p, err));
        CHECK(err.find("name") != std::string::npos);

        root = Parse(R"({ "version": 1, "name": "X", "requires": "NotAnArray.esp",
                          "slots": [ { "slot": 31, "kind": "hide" } ] })");
        CHECK(!JsonCodec::ParsePreset(root, p, err));
        CHECK(err.find("requires") != std::string::npos);

        root = Parse(R"({ "version": 1, "name": "X", "slots": [] })");
        CHECK(!JsonCodec::ParsePreset(root, p, err));
        CHECK(err.find("slots") != std::string::npos);

        root = Parse(R"({ "version": 1, "name": "X",
                          "slots": [ { "slot": 12, "kind": "hide" } ] })");
        CHECK(!JsonCodec::ParsePreset(root, p, err));  // out-of-range only
    }

    {  // export round-trip: PresetToJson output is a valid preset file
        Outfit o;
        o.name     = "My Fit";
        o.favorite = true;  // must NOT leak into the preset
        o.SetStyle(kBitBody, StyleRefKey{ "Cool.esp", 0xABC });
        o.SetHide(kBitCirclet);

        const auto json =
            JsonCodec::PresetToJson(o, "Me", "Shared from my library.", { "Cool.esp" });
        CHECK(!json.isMember("favorite"));

        JsonCodec::Preset p;
        std::string       err;
        CHECK(JsonCodec::ParsePreset(json, p, err));
        CHECK(p.name == "My Fit");
        CHECK(p.author == "Me");
        CHECK(p.requires_.size() == 1 && p.requires_[0] == "Cool.esp");
        CHECK(p.outfit.EntryFor(kBitBody).style.localFormID == 0xABC);
        CHECK(p.outfit.EntryFor(kBitCirclet).kind == SlotEntry::Kind::kHide);
        CHECK(p.outfit.favorite == false);
    }

    {  // outfit with weapons -> JSON -> outfit round-trip: a mix of weapon
       // style/hide/passthrough entries across several classes survives
       // alongside the existing armor slots, and the JSON shape mirrors the
       // slots array exactly (same kind strings, same hex id format).
        Outfit o;
        o.name = "Duelist";
        o.SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        o.SetWeaponStyle(WeaponClass::Sword, StyleRefKey{ "Weapons.esp", 0x10A });
        o.SetWeaponStyle(WeaponClass::Bolts, StyleRefKey{ "Ammo.esp", 0x55 });
        o.SetWeaponHide(WeaponClass::Bow);
        // Dagger, WarAxe, Mace, ... stay passthrough - the mix this test is for.

        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(json.isMember("weapons"));
        CHECK(json["weapons"].isArray());
        CHECK(json["weapons"].size() == 3);

        bool sawSwordStyle = false, sawBowHide = false;
        for (const auto& w : json["weapons"]) {
            if (w["class"].asString() == "sword") {
                sawSwordStyle = w["kind"].asString() == "style" &&
                                w["mod"].asString() == "Weapons.esp" &&
                                w["id"].asString() == "0x00010A";
            }
            if (w["class"].asString() == "bow") {
                sawBowHide =
                    w["kind"].asString() == "hide" && !w.isMember("mod") && !w.isMember("id");
            }
        }
        CHECK(sawSwordStyle);
        CHECK(sawBowHide);

        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.EntryFor(kBitBody).style.modName == "Armors.esp");
        CHECK(back.WeaponEntryFor(WeaponClass::Sword).kind == SlotEntry::Kind::kStyle);
        CHECK(back.WeaponEntryFor(WeaponClass::Sword).style.modName == "Weapons.esp");
        CHECK(back.WeaponEntryFor(WeaponClass::Sword).style.localFormID == 0x10A);
        CHECK(back.WeaponEntryFor(WeaponClass::Bolts).kind == SlotEntry::Kind::kStyle);
        CHECK(back.WeaponEntryFor(WeaponClass::Bolts).style.modName == "Ammo.esp");
        CHECK(back.WeaponEntryFor(WeaponClass::Bolts).style.localFormID == 0x55);
        CHECK(back.WeaponEntryFor(WeaponClass::Bow).kind == SlotEntry::Kind::kHide);
        CHECK(back.WeaponEntryFor(WeaponClass::Dagger).kind == SlotEntry::Kind::kPassthrough);
    }

    {  // JSON without a "weapons" key at all: every weapon entry stays
       // passthrough - this is exactly what every 0.1.1-era outfits.json
       // outfit object looks like.
        const auto root = Parse(R"({
            "name": "No Weapons Key",
            "slots": [ { "slot": 32, "kind": "style", "mod": "Armors.esp", "id": "0x801" } ]
        })");
        Outfit o;
        CHECK(JsonCodec::JsonToOutfit(root, o));
        CHECK(o.name == "No Weapons Key");
        CHECK(o.EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
        for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
            CHECK(o.WeaponEntryFor(static_cast<WeaponClass>(c)).kind ==
                  SlotEntry::Kind::kPassthrough);
        }
    }

    {  // Per-hand JSON is additive: missing hand means legacy Both; explicit
       // passthrough differs from an absent override (inherit Both).
        Outfit o;
        o.name = "Twin Blades";
        o.SetWeaponStyle(WeaponClass::Sword, { "Both.esp", 1 });
        o.SetWeaponStyle(WeaponClass::Sword, { "Right.esp", 2 },
                         WeaponHand::Right);
        o.SetWeaponPassthrough(WeaponClass::Sword, WeaponHand::Left);

        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(json["weapons"].size() == 3);
        bool sawBoth = false, sawRight = false, sawLeftReal = false;
        for (const auto& w : json["weapons"]) {
            const auto hand = w.get("hand", "").asString();
            sawBoth |= hand.empty() && w["mod"].asString() == "Both.esp";
            sawRight |= hand == "right" && w["mod"].asString() == "Right.esp";
            sawLeftReal |= hand == "left" &&
                           w["kind"].asString() == "passthrough";
        }
        CHECK(sawBoth);
        CHECK(sawRight);
        CHECK(sawLeftReal);

        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.ResolvedWeaponEntryFor(
                  WeaponClass::Sword, WeaponHand::Right).style.modName == "Right.esp");
        CHECK(back.WeaponOverrideFor(
                  WeaponClass::Sword, WeaponHand::Left).has_value());
        CHECK(back.ResolvedWeaponEntryFor(
                  WeaponClass::Sword, WeaponHand::Left).kind ==
              SlotEntry::Kind::kPassthrough);
    }

    {  // tolerance: bad class, bad kind, empty style key and non-objects are
       // skipped, not fatal - the rest of the outfit (armor slots and other
       // weapon entries) still parses fine. Mirrors the armor slots
       // tolerance block above.
        const auto root = Parse(R"({
            "name": "Tolerant Weapons",
            "slots": [
                { "slot": 32, "kind": "hide" },
                { "slot": 39, "kind": "style", "mod": "Shields.esp", "id": "0x44" }
            ],
            "weapons": [
                { "class": "shield", "kind": "style", "mod": "X.esp", "id": "0x1" },
                { "class": "mace", "kind": "invisible" },
                { "class": "bow", "kind": "style", "mod": "", "id": "0x0" },
                { "class": "sword", "kind": "style", "mod": "Weapons.esp", "id": "0x10A" },
                "not-an-object"
            ]
        })");
        Outfit o;
        CHECK(JsonCodec::JsonToOutfit(root, o));
        CHECK(o.name == "Tolerant Weapons");
        CHECK(o.EntryFor(kBitBody).kind == SlotEntry::Kind::kHide);
        CHECK(o.EntryFor(kBitShield).kind == SlotEntry::Kind::kStyle);
        CHECK(o.EntryFor(kBitShield).style.modName == "Shields.esp");
        CHECK(o.EntryFor(kBitShield).style.localFormID == 0x44);
        CHECK(o.WeaponEntryFor(WeaponClass::Sword).kind == SlotEntry::Kind::kStyle);
        CHECK(o.WeaponEntryFor(WeaponClass::Sword).style.modName == "Weapons.esp");
        CHECK(o.WeaponEntryFor(WeaponClass::Sword).style.localFormID == 0x10A);
        // Shield remains armor slot 39, not a WeaponClass. The weapons-array
        // "shield" entry is therefore ignored; the slots-array entry above is kept.
        CHECK(o.WeaponEntryFor(WeaponClass::Mace).kind == SlotEntry::Kind::kPassthrough);
        CHECK(o.WeaponEntryFor(WeaponClass::Bow).kind == SlotEntry::Kind::kPassthrough);
        for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
            const auto wc = static_cast<WeaponClass>(c);
            if (wc != WeaponClass::Sword) {
                CHECK(o.WeaponEntryFor(wc).kind == SlotEntry::Kind::kPassthrough);
            }
        }
    }

    {  // an armor-only outfit (all weapon entries passthrough) serializes
       // WITHOUT a "weapons" key at all - keeps 0.1.1-era files byte-stable.
        Outfit o;
        o.name = "Armor Only";
        o.SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        o.SetHide(kBitHair);

        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(!json.isMember("weapons"));
    }

    {  // a weapons-only preset is a valid preset: the usable-content gate
       // accepts weapon entries, not just armor slots (the editor's Export
       // can produce exactly such files).
        const auto root = Parse(R"({
            "version": 1,
            "name": "Blades Loadout",
            "weapons": [
                { "class": "sword", "kind": "style", "mod": "Blades.esp", "id": "0x10A" },
                { "class": "arrows", "kind": "hide" }
            ]
        })");
        JsonCodec::Preset p;
        std::string       err;
        CHECK(JsonCodec::ParsePreset(root, p, err));
        CHECK(err.empty());
        CHECK(p.outfit.WeaponEntryFor(WeaponClass::Sword).kind == SlotEntry::Kind::kStyle);
        CHECK(p.outfit.WeaponEntryFor(WeaponClass::Sword).style.modName == "Blades.esp");
        CHECK(p.outfit.WeaponEntryFor(WeaponClass::Arrows).kind == SlotEntry::Kind::kHide);

        // ...but a preset with NO usable content of either dimension still
        // fails, and the reason now names both arrays.
        const auto empty = Parse(R"({ "version": 1, "name": "X",
                                      "slots": [], "weapons": [] })");
        CHECK(!JsonCodec::ParsePreset(empty, p, err));
        CHECK(err.find("slots") != std::string::npos);
        CHECK(err.find("weapons") != std::string::npos);
    }

    if (g_failures == 0) {
        std::printf("JsonCodecTests: all passed\n");
        return 0;
    }
    std::printf("JsonCodecTests: %d failure(s)\n", g_failures);
    return 1;
}
