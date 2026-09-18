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
        o.obodyPreset = "Installed value must lose to the custom owner";
        o.customBodyPresetId = "0123456789abcdef";
        o.orefit      = ORefitMode::kForceOff;

        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(!json.isMember("obodyPreset"));
        CHECK(json["customBodyPresetId"].asString() == "0123456789abcdef");
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
        CHECK(back.obodyPreset.empty());
        CHECK(back.customBodyPresetId == "0123456789abcdef");
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

    {  // Hair survives a JSON round trip by NAME, not by number - the file is a
       // hand-editable authoring surface (see PRESETS.md).
        Outfit o;
        o.name = "Hooded";
        o.hair = HairMode::kHide;
        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(json["hair"].asString() == "hide");

        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.hair == HairMode::kHide);
    }

    {  // Auto is omitted, so a file written before this is byte-identical.
        Outfit o;
        o.name = "Plain";
        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(!json.isMember("hair"));

        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.hair == HairMode::kAuto);
    }

    {  // An unknown value degrades to auto rather than refusing the outfit.
        Json::Value json(Json::objectValue);
        json["name"] = "Weird";
        json["hair"] = "banana";
        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.hair == HairMode::kAuto);
    }

    {  // Hair colour survives an export/import round trip, and is OMITTED when
       // disabled so a file written before this feature stays byte-identical.
        Outfit o;
        o.hairTint = HairTint{ true, 200, 40, 90 };
        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(json.isMember("hairColor"));
        CHECK(json["hairColor"].asString() == "C8285A");  // 200,40,90 = C8,28,5A

        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.hairTint.set);
        CHECK(back.hairTint.r == 200);
        CHECK(back.hairTint.g == 40);
        CHECK(back.hairTint.b == 90);

        Outfit plain;
        CHECK(!JsonCodec::OutfitToJson(plain).isMember("hairColor"));

        // A malformed value decodes to disabled rather than to a half-parsed
        // colour: an imported file is untrusted input.
        Json::Value bad;
        bad["hairColor"] = "nonsense";
        Outfit fromBad;
        CHECK(JsonCodec::JsonToOutfit(bad, fromBad));
        CHECK(!fromBad.hairTint.set);

        // A human hand-editing an exported file types what their colour
        // picker gave them - typically lowercase ("c8285a"), not the
        // uppercase this codec itself emits. The round trip must accept it.
        Json::Value lower;
        lower["hairColor"] = "c8285a";
        Outfit fromLower;
        CHECK(JsonCodec::JsonToOutfit(lower, fromLower));
        CHECK(fromLower.hairTint.set);
        CHECK(fromLower.hairTint.r == 200);
        CHECK(fromLower.hairTint.g == 40);
        CHECK(fromLower.hairTint.b == 90);

        // Strict means exactly six, not "at least six": eight valid hex digits
        // must be rejected outright rather than silently read as the last six
        // (which would quietly apply a colour close to, but not, the one in
        // the file - worse than leaving hair alone, because it looks correct).
        Json::Value overlong;
        overlong["hairColor"] = "C8285A00";
        Outfit fromOverlong;
        CHECK(JsonCodec::JsonToOutfit(overlong, fromOverlong));
        CHECK(!fromOverlong.hairTint.set);
    }

    {  // Eye colour survives an export/import round trip on the same rules as
       // hair: omitted when disabled, one RRGGBB hex, malformed decodes to
       // disabled. An exported outfit was losing its eye colour because this
       // codec never carried the field.
        Outfit o;
        o.eyeTint = HairTint{ true, 0, 176, 0 };
        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(json.isMember("eyeColor"));
        CHECK(json["eyeColor"].asString() == "00B000");

        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.eyeTint.set);
        CHECK(back.eyeTint.r == 0);
        CHECK(back.eyeTint.g == 176);
        CHECK(back.eyeTint.b == 0);

        Outfit plain;
        CHECK(!JsonCodec::OutfitToJson(plain).isMember("eyeColor"));

        Json::Value bad;
        bad["eyeColor"] = "nonsense";
        Outfit fromBad;
        CHECK(JsonCodec::JsonToOutfit(bad, fromBad));
        CHECK(!fromBad.eyeTint.set);

        // Lowercase hex from a human's colour picker is accepted, exactly as
        // hairColor's is: one parser serves both.
        Json::Value lower;
        lower["eyeColor"] = "00b000";
        Outfit fromLower;
        CHECK(JsonCodec::JsonToOutfit(lower, fromLower));
        CHECK(fromLower.eyeTint.set);
        CHECK(fromLower.eyeTint.g == 176);
    }

    {  // The sclera colour rides beside the eye colour on identical rules.
        Outfit o;
        o.scleraTint = HairTint{ true, 200, 40, 90 };
        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(json.isMember("scleraColor"));
        CHECK(json["scleraColor"].asString() == "C8285A");
        CHECK(!json.isMember("eyeColor"));  // the halves omit independently

        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.scleraTint.set);
        CHECK(back.scleraTint.r == 200);
        CHECK(!back.eyeTint.set);

        Json::Value bad;
        bad["scleraColor"] = "nonsense";
        Outfit fromBad;
        CHECK(JsonCodec::JsonToOutfit(bad, fromBad));
        CHECK(!fromBad.scleraTint.set);
    }

    {  // ⚠⚠ THE SECOND EYE COLOUR AND THE EYE BLEND RIDE HERE TOO, and their
       // absence was a real defect: the co-save carried both and the EXPORT
       // dropped them, so one outfit rendered two ways depending on which door
       // it came back through. Each eye field has been added to this file the
       // day it landed, for exactly this reason; these two were missed.
        Outfit o;
        o.eyeTint  = HairTint{ true, 0, 176, 0 };
        o.eyeTint2 = HairTint{ true, 200, 40, 90 };
        o.eyeBlend = 3;  // screen

        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(json.isMember("eyeColor2"));
        CHECK(json["eyeColor2"].asString() == "C8285A");
        CHECK(json.isMember("eyeBlend"));
        CHECK(json["eyeBlend"].asUInt() == 3u);

        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.eyeTint2.set);
        CHECK(back.eyeTint2.r == 200 && back.eyeTint2.g == 40 && back.eyeTint2.b == 90);
        CHECK(back.eyeBlend == 3);

        // Omitted when they carry nothing, so a file written before either
        // field existed stays byte-identical.
        Outfit plain;
        plain.eyeTint     = HairTint{ true, 1, 2, 3 };
        const auto minimal = JsonCodec::OutfitToJson(plain);
        CHECK(!minimal.isMember("eyeColor2"));
        CHECK(!minimal.isMember("eyeBlend"));

        // ⚠ AN UNREADABLE BLEND DEFERS RATHER THAN REFUSING, DyeBlend.h's rule:
        // a hand-edited file costs the eye its curve, never its colour.
        Json::Value wild;
        wild["eyeColor"] = "00B000";
        wild["eyeBlend"] = 99;
        Outfit fromWild;
        CHECK(JsonCodec::JsonToOutfit(wild, fromWild));
        CHECK(fromWild.eyeTint.set);   // the colour survives
        CHECK(fromWild.eyeBlend == 0);  // the curve defers
    }

    {  // Dye survives an export/import round trip, and is OMITTED when no
       // slot is dyed so a pre-dye file stays byte-identical. Slots speak the
       // same editor numbers the slots array does; channels are the same bare
       // RRGGBB hex hairColor uses.
        Outfit o;
        o.SetDye(BitForEditorSlot(32), DyeChannelId::kPrimary,
                 DyeChannel{ true, 200, 40, 90 });
        o.SetDye(BitForEditorSlot(32), DyeChannelId::kAccent,
                 DyeChannel{ true, 1, 2, 3 });
        o.SetDye(BitForEditorSlot(46), DyeChannelId::kSecondary,
                 DyeChannel{ true, 9, 8, 7 });
        const auto json = JsonCodec::OutfitToJson(o);
        CHECK(json.isMember("dyes"));
        CHECK(json["dyes"].size() == 2);
        CHECK(json["dyes"][0]["slot"].asUInt() == 32);
        // A "colours" ARRAY now, not three named keys: past three pieces there
        // is no name for the sixth. Position is the channel, so an unset one in
        // the middle is a null placeholder rather than a closed gap, which would
        // slide every later colour onto the wrong piece.
        CHECK(json["dyes"][0]["colours"].size() == 3);
        CHECK(json["dyes"][0]["colours"][0].asString() == "C8285A");
        CHECK(json["dyes"][0]["colours"][1].isNull());
        CHECK(json["dyes"][0]["colours"][2].asString() == "010203");
        CHECK(json["dyes"][1]["slot"].asUInt() == 46);
        // Trailing unset channels are trimmed, so one colour on the second
        // channel writes two entries and not eight.
        CHECK(json["dyes"][1]["colours"].size() == 2);
        CHECK(json["dyes"][1]["colours"][0].isNull());
        CHECK(json["dyes"][1]["colours"][1].asString() == "090807");

        Outfit back;
        CHECK(JsonCodec::JsonToOutfit(json, back));
        CHECK(back.DyeFor(BitForEditorSlot(32)).channels[0] ==
              (DyeChannel{ true, 200, 40, 90 }));
        CHECK(!back.DyeFor(BitForEditorSlot(32)).channels[1].set);
        CHECK(back.DyeFor(BitForEditorSlot(32)).channels[2] ==
              (DyeChannel{ true, 1, 2, 3 }));
        CHECK(back.DyeFor(BitForEditorSlot(46)).channels[1] ==
              (DyeChannel{ true, 9, 8, 7 }));

        Outfit plain;
        CHECK(!JsonCodec::OutfitToJson(plain).isMember("dyes"));

        // A channel past the old three round trips, which is the whole point of
        // the widening: the Abyss boots carry six pieces.
        Outfit wide;
        wide.SetDye(BitForEditorSlot(37), static_cast<DyeChannelId>(5),
                    DyeChannel{ true, 11, 22, 33 });
        Outfit wideBack;
        CHECK(JsonCodec::JsonToOutfit(JsonCodec::OutfitToJson(wide), wideBack));
        CHECK(wideBack.DyeFor(BitForEditorSlot(37)).channels[5] ==
              (DyeChannel{ true, 11, 22, 33 }));

        // The per-piece cut (2026-09-04) rides the object form the way the
        // flake does: a channel at the default 128 stays the bare string every
        // pre-cut file wrote, and any other byte round trips.
        Outfit     tuned;
        DyeChannel low{ true, 5, 6, 7 };
        low.mode = 5;
        low.cut  = 51;
        tuned.SetDye(BitForEditorSlot(32), DyeChannelId::kPrimary, low);
        const auto tunedJson = JsonCodec::OutfitToJson(tuned);
        CHECK(tunedJson["dyes"][0]["colours"][0].isObject());
        CHECK(tunedJson["dyes"][0]["colours"][0]["cut"].asUInt() == 51u);
        Outfit tunedBack;
        CHECK(JsonCodec::JsonToOutfit(tunedJson, tunedBack));
        CHECK(tunedBack.DyeFor(BitForEditorSlot(32)).channels[0].cut == 51);
        CHECK(tunedBack.DyeFor(BitForEditorSlot(32)).channels[0] == low);
        Outfit atDefault;
        atDefault.SetDye(BitForEditorSlot(32), DyeChannelId::kPrimary,
                         DyeChannel{ true, 5, 6, 7 });
        CHECK(JsonCodec::OutfitToJson(atDefault)["dyes"][0]["colours"][0].isString());
    }

    {  // A preset written before the widening used primary/secondary/accent as
       // named keys. Those files still parse: there is no reason to strand one
       // that reads perfectly well, and presets ship inside other people's mods.
        Json::Value legacy;
        Json::Value d;
        d["slot"]      = 32;
        d["primary"]   = "C8285A";
        d["accent"]    = "010203";
        legacy["dyes"].append(d);

        Outfit out;
        CHECK(JsonCodec::JsonToOutfit(legacy, out));
        CHECK(out.DyeFor(BitForEditorSlot(32)).channels[0] ==
              (DyeChannel{ true, 200, 40, 90 }));
        CHECK(!out.DyeFor(BitForEditorSlot(32)).channels[1].set);
        CHECK(out.DyeFor(BitForEditorSlot(32)).channels[2] ==
              (DyeChannel{ true, 1, 2, 3 }));
    }

    {  // Dye import tolerance mirrors hairColor's: malformed channels decode
       // to "leave this alone", out-of-range slots are skipped whole, and
       // lowercase hex from a human's colour picker is accepted.
        Json::Value root;
        Json::Value d0;
        d0["slot"]    = 32;
        d0["primary"] = "nonsense";          // malformed: channel stays off
        d0["accent"]  = "c8285a";            // lowercase: accepted
        Json::Value d1;
        d1["slot"]      = 99;                // out of range: skipped whole
        d1["primary"]   = "FF0000";
        Json::Value dyes(Json::arrayValue);
        dyes.append(d0);
        dyes.append(d1);
        dyes.append("not an object");        // non-object: skipped
        root["dyes"] = std::move(dyes);

        Outfit out;
        CHECK(JsonCodec::JsonToOutfit(root, out));
        CHECK(!out.DyeFor(BitForEditorSlot(32)).channels[0].set);
        CHECK(out.DyeFor(BitForEditorSlot(32)).channels[2] ==
              (DyeChannel{ true, 200, 40, 90 }));
        // Nothing else anywhere: the bad slot and bad channel landed nowhere.
        bool anyOther = false;
        for (std::uint32_t bit = 0; bit < Outfit::kBitCount; ++bit) {
            for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                if (bit == BitForEditorSlot(32) && c == 2) {
                    continue;
                }
                if (out.DyeFor(bit).channels[c].set) {
                    anyOther = true;
                }
            }
        }
        CHECK(!anyOther);
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

        // A hand-edited "version": "1" (string, not a number) is a wrong
        // type, not a coercible match - it must be rejected like any other
        // junk version, not crash reading it (jsoncpp's asInt() throws on a
        // non-numeric Value with exceptions compiled in).
        root = Parse(R"({ "version": "1", "name": "X",
                          "slots": [ { "slot": 31, "kind": "hide" } ] })");
        CHECK(!JsonCodec::ParsePreset(root, p, err));

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

    {  // The unsupported-hair sidecar: a style the follower attach has
       // measured and declined stays declined across sessions, so the one
       // visible dud attach happens once EVER rather than once per session.
       // Bone counts and partition maps are mesh properties, so the answer is
       // load-order-scoped like outfits.json, not save-scoped.
        std::vector<StyleRefKey> keys{
            { "KSHairdosSMP.esp", 0x0123ABu },
            { "KS Hairdo's.esp", 0x000D62u },
        };
        const auto j = JsonCodec::UnsupportedHairToJson(keys);
        std::vector<StyleRefKey> back;
        CHECK(JsonCodec::JsonToUnsupportedHair(j, back));
        CHECK(back == keys);
    }

    {  // Tolerance matches every other reader in this file: junk entries are
       // skipped, not fatal, and an empty or wrong-typed root reads as an
       // empty list rather than an error a load would abort on.
        const auto j = Parse(R"({ "version": 3, "entries": [
            { "mod": "A.esp", "id": "0x10" },
            { "mod": "", "id": "0x11" },
            "not-an-object",
            { "mod": "B.esp", "id": "junk" },
            { "mod": "C.esp", "id": "0x30" } ] })");
        std::vector<StyleRefKey> back;
        CHECK(JsonCodec::JsonToUnsupportedHair(j, back));
        CHECK(back.size() == 2);
        CHECK(back[0].modName == "A.esp");
        CHECK(back[0].localFormID == 0x10u);
        CHECK(back[1].modName == "C.esp");
        CHECK(back[1].localFormID == 0x30u);

        std::vector<StyleRefKey> none;
        CHECK(!JsonCodec::JsonToUnsupportedHair(Parse("[]"), none));
        CHECK(none.empty());
    }

    {  // OS-248: schema v3. Any other version is OUTDATED, not corrupt:
       // reject it so the loader starts empty and styles re-measure once.
       // ⚠⚠ v2 MUST NOW BE REFUSED, and that is the point of the bump rather
       // than bookkeeping: v2 files were written by a partition gate that
       // refused any single-partition mesh whose bone map was shorter than its
       // bone list, and that rule was measured wrong. Every decline it
       // persisted is a mesh that may well be fine, and a persisted verdict
       // outliving the rule that made it greys a style out for good.
        std::vector<StyleRefKey> keys;
        keys.push_back({ "KSHairdosSMP.esp", 0x000801u });
        const auto j = JsonCodec::UnsupportedHairToJson(keys);
        CHECK(j["version"].asInt() == 3);

        std::vector<StyleRefKey> back;
        CHECK(JsonCodec::JsonToUnsupportedHair(j, back));
        CHECK(back.size() == 1);

        Json::Value v1(Json::objectValue);
        v1["version"] = 1;
        v1["entries"] = j["entries"];
        std::vector<StyleRefKey> none;
        CHECK(!JsonCodec::JsonToUnsupportedHair(v1, none));
        CHECK(none.empty());

        Json::Value v2(Json::objectValue);
        v2["version"] = 2;
        v2["entries"] = j["entries"];
        CHECK(!JsonCodec::JsonToUnsupportedHair(v2, none));
        CHECK(none.empty());

        Json::Value noVersion(Json::objectValue);
        noVersion["entries"] = j["entries"];
        CHECK(!JsonCodec::JsonToUnsupportedHair(noVersion, none));
        CHECK(none.empty());
    }

    {  // A hand-edited "version": "2" (string, not a number) must not crash
       // the loader. jsoncpp's asInt() throws Json::LogicError on a
       // non-numeric Value when exceptions are compiled in (they are, in
       // this build) - the version read has to treat a wrong JSON type as
       // junk, the same tolerance every other field in this file gets.
        Json::Value strVersion(Json::objectValue);
        strVersion["version"] = "3";
        std::vector<StyleRefKey> out;
        CHECK(!JsonCodec::JsonToUnsupportedHair(strVersion, out));
        CHECK(out.empty());
    }

    if (g_failures == 0) {
        std::printf("JsonCodecTests: all passed\n");
        return 0;
    }
    std::printf("JsonCodecTests: %d failure(s)\n", g_failures);
    return 1;
}
