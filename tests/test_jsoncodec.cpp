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

    if (g_failures == 0) {
        std::printf("JsonCodecTests: all passed\n");
        return 0;
    }
    std::printf("JsonCodecTests: %d failure(s)\n", g_failures);
    return 1;
}
