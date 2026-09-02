// Profile codec tests. No SKSE, no engine - the codec operates on Json::Value.
//
// ⚠ FIXTURES ARE FORGED FROM REAL CAPTURES, per house rule. Provenance of
// every value, read off the live rig 2026-08-21:
//   * outfit slots: the user's own "Markynaz" outfit in the live outfits.json
//     (1Markynaz.esl 0x801/0x802/0x800, Complete Crafting Overhaul_Remastered
//     0x011825), and the "Redguard" export's requires + hide-31 pair.
//   * overlay textures: the OS-209 bake sidecar 0ba7a51f0218ef52.txt (source
//     "Actors\Character\Overlays\SFO\Face\Face Mole Forehead 1.dds", offsets
//     -0.025 / 0.163, scale 0.430) and the scraped overlay table's
//     "!ube\[spaz490]\nail overlays\ube_fingernails_basic.dds" (hands).
//   * colours: 706B99, the user's own saved "purp" dye scheme.
//   * body: OBody preset "Sindra" (default-bodies.json) and sliders from the
//     user's Sindra-Nerfed-72120d60.json (Skinny2.0 = 58.1 both ends,
//     FitnessDetails_2.0 = 10 small / 0 big, family ube, set
//     "UBE SE 2.0 Release Body").
//   * makeup: the vanilla warpaint tint-mask texture the race records name
//     (femaleheadwarpaint_01.dds), type "warpaint".
//   * glow strength 25.5: the measured cap, RaceMenu's alpha-byte/10.
#include "ProfileCodec.h"

#include "BodyPresetJson.h"
#include "JsonCodec.h"
#include "OverlayBaseline.h"  // ForgetIn and AnyIn, the record's pure bookkeeping
#include "PresetRequirements.h"
#include "SlotMask.h"

#include <json/json.h>

#include <cstdio>
#include <sstream>
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

static bool Dropped(const OS::ProfileCodec::ProfileParse& a_parse,
                    const std::string& a_needle) {
    for (const auto& line : a_parse.dropped) {
        if (line.find(a_needle) != std::string::npos) return true;
    }
    return false;
}

// The forged profile: every block filled with the real values above.
static OS::ProfileCodec::Profile ForgeProfile() {
    using namespace OS;
    ProfileCodec::Profile p;
    p.name        = "Aria";
    p.description = "forged from the live rig's own state";
    p.requires_   = { "ccbgssse021-lordsmail.esl" };

    p.face = ProfileCodec::FaceBlock{ "FR_Aria",
                                      ProfileCodec::FaceSource::kCaptured, 0 };

    Outfit outfit;
    outfit.name = "Markynaz";
    outfit.SetStyle(kBitBody, StyleRefKey{ "1Markynaz.esl", 0x801 });
    outfit.SetStyle(kBitHands, StyleRefKey{ "1Markynaz.esl", 0x802 });
    outfit.SetStyle(kBitFeet, StyleRefKey{ "1Markynaz.esl", 0x800 });
    outfit.SetHide(kBitHair);
    // A pearl beside a plain colour, both real: Abyssal Pearl out of the
    // shipped pearl.json (hex 8FA7C4, hex2 C9A0D8, mode iridescent, sheen
    // C9A0D8, gloss 200) and the user's own "purp" 706B99. The field caught
    // the old encoder dropping everything past the hex ("pearlescent dyes
    // always go to single color", 2026-08-22), so the pearl round-trip here
    // is the regression pin for that.
    {
        DyeChannel pearl{ true, 0x8F, 0xA7, 0xC4 };
        pearl.mode             = 2;  // DyeRamp::Mode::kIridescent
        pearl.secondSet        = true;
        pearl.r2               = 0xC9;
        pearl.g2               = 0xA0;
        pearl.b2               = 0xD8;
        pearl.palette.sheenSet = true;
        pearl.palette.sheenR   = 0xC9;
        pearl.palette.sheenG   = 0xA0;
        pearl.palette.sheenB   = 0xD8;
        pearl.palette.glossSet = true;
        pearl.palette.gloss    = 200;
        outfit.SetDye(kBitBody, DyeChannelId::kPrimary, pearl);
        outfit.SetDye(kBitBody, DyeChannelId::kSecondary,
                      DyeChannel{ true, 0x70, 0x6B, 0x99 });
    }
    p.outfit = outfit;

    p.character = ProfileCodec::CharacterBlock{
        StyleRefKey{ "Skyrim.esm", 0x013742 },  // DarkElfRace
        true
    };

    ProfileCodec::OverlaysBlock overlays;
    {
        ProfileCodec::OverlayEntry mole;
        mole.index                 = 0;
        mole.state.hasTexture      = true;
        mole.state.texture         = "Actors\\Character\\Overlays\\SFO\\Face\\Face Mole Forehead 1.dds";
        mole.state.hasTint         = true;
        mole.state.tint            = OverlayPlan::Rgb{ 0x70, 0x6B, 0x99 };
        mole.state.transform       = OverlayTransform::Transform{ -0.025f, 0.163f,
                                                                  0.430f, 0.0f };
        overlays.byLocation[OverlayPlan::Slot(OverlayPlan::Location::kFace)]
            .push_back(std::move(mole));

        ProfileCodec::OverlayEntry nails;
        nails.index            = 1;
        nails.state.hasTexture = true;
        nails.state.texture = "!ube\\[spaz490]\\nail overlays\\ube_fingernails_basic.dds";
        nails.state.hasAlpha     = true;
        nails.state.alpha        = 1.0f;
        nails.state.glow         = OverlayPlan::Rgb{ 0x70, 0x6B, 0x99 };
        nails.state.glowStrength = 25.5f;
        nails.state.hasFinish    = true;  // gloss/specular stay the shipped defaults
        overlays.byLocation[OverlayPlan::Slot(OverlayPlan::Location::kHands)]
            .push_back(std::move(nails));
    }
    p.overlays = std::move(overlays);

    ProfileCodec::MakeupEntry warpaint;
    warpaint.index             = 8;
    warpaint.type              = static_cast<std::uint32_t>(MakeupPlan::Type::kWarPaint);
    warpaint.state.tint        = OverlayPlan::Rgb{ 0x70, 0x6B, 0x99 };
    warpaint.state.strength    = 0.8f;
    warpaint.state.hasTexture  = true;
    warpaint.state.texture =
        "actors\\character\\character assets\\tintmasks\\femaleheadwarpaint_01.dds";
    p.makeup = std::vector{ warpaint };

    OS::BodyPreset custom;
    custom.id           = "72120d60a5e40b73e6ca5e8730d9eb5c";
    custom.name         = "Sindra Nerfed";
    custom.sex          = OS::BodySex::kFemale;
    custom.family       = OS::BodyFamily::kUBE;
    custom.sourceSet    = "UBE SE 2.0 Release Body";
    custom.sourcePreset = "Sindra";
    custom.sliders      = {
        OS::BodySliderValue{ "Skinny2.0", "Skinny",
                             "Presets and Important stuff", 58.1f, 58.1f },
        OS::BodySliderValue{ "FitnessDetails_2.0", "Fitness Details",
                             "Presets and Important stuff", 10.0f, 0.0f },
    };
    p.body = ProfileCodec::BodyBlock{ "Sindra", std::move(custom) };

    ProfileCodec::ShapeBlock shape;
    shape.morphs = { { "Skinny2.0", 0.581f } };
    shape.scales = { { "hands", 1.05f }, { "head", 1.0f } };
    p.shape = std::move(shape);

    p.skin   = ProfileCodec::SkinBlock{ "UBE" };
    p.weight = 58.1f;
    return p;
}

int main() {
    using namespace OS;
    using namespace OS::ProfileCodec;

    {  // full profile -> JSON -> profile round-trip, block by block
        const auto   forged = ForgeProfile();
        const auto   json   = ProfileToJson(forged);
        ProfileParse back;
        std::string  error;
        CHECK(ParseProfile(json, back, error));
        CHECK(error.empty());
        CHECK(back.dropped.empty());
        CHECK(back.rewritable);

        const auto& p = back.profile;
        CHECK(p.name == "Aria");
        CHECK(p.description == forged.description);
        CHECK(p.requires_ == forged.requires_);
        CHECK(p.face == forged.face);
        CHECK(p.outfit && forged.outfit);
        CHECK(p.outfit->name == "Markynaz");
        CHECK(ChangedSlotCount(*p.outfit, *forged.outfit) == 0);
        CHECK(p.outfit->EntryFor(kBitBody).style.modName == "1Markynaz.esl");
        CHECK(p.outfit->EntryFor(kBitBody).style.localFormID == 0x801);
        CHECK(p.outfit->EntryFor(kBitHair).kind == SlotEntry::Kind::kHide);
        CHECK(p.makeup == forged.makeup);
        CHECK(p.body == forged.body);
        CHECK(p.shape == forged.shape);
        CHECK(p.skin == forged.skin);
        CHECK(p.weight && *p.weight == 58.1f);
        CHECK(p.character == forged.character);

        // The pearl survives whole - every field past the hex - and the plain
        // colour beside it still travels as the bare string it always was.
        CHECK(p.outfit->DyeFor(kBitBody).channels[0] ==
              forged.outfit->DyeFor(kBitBody).channels[0]);
        CHECK(p.outfit->DyeFor(kBitBody).channels[1] ==
              forged.outfit->DyeFor(kBitBody).channels[1]);
        CHECK(json["outfit"]["dyes"][0]["colours"][0].isObject());
        CHECK(json["outfit"]["dyes"][0]["colours"][0]["mode"].asUInt() == 2);
        CHECK(json["outfit"]["dyes"][0]["colours"][0]["second"].asString() ==
              "C9A0D8");
        CHECK(json["outfit"]["dyes"][0]["colours"][1].isString());
        CHECK(json["outfit"]["dyes"][0]["colours"][1].asString() == "706B99");

        // The transform quantises to three decimals, so the sidecar's own
        // values (already three-decimal) survive exactly.
        CHECK(p.overlays);
        const auto& face =
            p.overlays->byLocation[OverlayPlan::Slot(OverlayPlan::Location::kFace)];
        CHECK(face.size() == 1);
        CHECK(face[0].state.texture ==
              "Actors\\Character\\Overlays\\SFO\\Face\\Face Mole Forehead 1.dds");
        CHECK(face[0].state.hasTint &&
              face[0].state.tint == (OverlayPlan::Rgb{ 0x70, 0x6B, 0x99 }));
        CHECK(face[0].state.transform ==
              (OverlayTransform::Transform{ -0.025f, 0.163f, 0.430f, 0.0f }));
        const auto& hands =
            p.overlays->byLocation[OverlayPlan::Slot(OverlayPlan::Location::kHands)];
        CHECK(hands.size() == 1);
        CHECK(hands[0].index == 1);
        CHECK(hands[0].state.hasAlpha && hands[0].state.alpha == 1.0f);
        CHECK(hands[0].state.glowStrength == 25.5f);
        CHECK(hands[0].state.hasFinish &&
              hands[0].state.gloss == OverlayPlan::kDefaultGloss &&
              hands[0].state.specular == OverlayPlan::kDefaultSpecular);

        // The outfit block is the outfit codec's own schema, favorite removed.
        CHECK(!json["outfit"].isMember("favorite"));
        CHECK(json["outfit"]["name"].asString() == "Markynaz");

        // Makeup type is the stable id on disk, not a number.
        CHECK(json["makeup"][0]["type"].asString() == "warpaint");

        // Face always writes its whole triple; source never rides a default.
        CHECK(json["face"]["source"].asString() == "captured");
        CHECK(json["face"]["flags"].asInt() == 0);
    }

    {  // version gate: exactly integer 1
        ProfileParse parse;
        std::string  error;
        CHECK(!ParseProfile(Parse(R"({"version":"1","name":"Aria"})"), parse, error));
        CHECK(!error.empty());
        error.clear();
        CHECK(!ParseProfile(Parse(R"({"version":2,"name":"Aria"})"), parse, error));
        CHECK(error.find("version 2") != std::string::npos);
        error.clear();
        CHECK(!ParseProfile(Parse(R"({"name":"Aria"})"), parse, error));
        error.clear();
        CHECK(!ParseProfile(Parse(R"({"version":1})"), parse, error));
        CHECK(error.find("name") != std::string::npos);
        error.clear();
        CHECK(!ParseProfile(Parse(R"([1,2])"), parse, error));
    }

    {  // requires stays a hard gate, exactly like the presets
        ProfileParse parse;
        std::string  error;
        CHECK(!ParseProfile(
            Parse(R"({"version":1,"name":"Aria","requires":"not-an-array"})"),
            parse, error));
        CHECK(error.find("requires") != std::string::npos);
        error.clear();
        CHECK(!ParseProfile(
            Parse(R"({"version":1,"name":"Aria","requires":[""]})"), parse, error));
    }

    {  // partial file: a face-only profile decodes with every sibling absent
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "face":{"jslot":"FR_Aria","source":"captured","flags":0}})"),
            parse, error));
        CHECK(parse.dropped.empty());
        CHECK(parse.profile.face);
        CHECK(parse.profile.face->source == FaceSource::kCaptured);
        CHECK(!parse.profile.outfit && !parse.profile.overlays &&
              !parse.profile.makeup && !parse.profile.body &&
              !parse.profile.shape && !parse.profile.skin &&
              !parse.profile.weight);
    }

    {  // face defaults: source absent reads referenced (the side that never
       // deletes), flags absent reads 0, folder absent reads exported (what
       // every pre-folder profile meant)
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria","face":{"jslot":"Redguard"}})"),
            parse, error));
        CHECK(parse.profile.face);
        CHECK(parse.profile.face->source == FaceSource::kReferenced);
        CHECK(parse.profile.face->flags == 0);
        CHECK(parse.profile.face->folder == FaceFolder::kExported);
    }

    {  // face folder: the presets value round-trips; the exported value is
       // NEVER written (pre-folder files resave byte-stable); junk is
       // refused because the folder picks the dispatch native. The preset
       // name is the rig's own `(002026 July) Umbrael.jslot`.
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Umbrael preset",
                      "face":{"jslot":"(002026 July) Umbrael",
                              "folder":"presets"}})"),
            parse, error));
        CHECK(parse.profile.face);
        CHECK(parse.profile.face->folder == FaceFolder::kPresets);
        const auto json = ProfileToJson(parse.profile);
        CHECK(json["face"]["folder"].asString() == "presets");

        Profile exported;
        exported.name = "Aria";
        exported.face =
            FaceBlock{ "FR_Aria", FaceSource::kCaptured, 0,
                       FaceFolder::kExported };
        CHECK(!ProfileToJson(exported)["face"].isMember("folder"));

        parse = {};
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "face":{"jslot":"FR_Aria","folder":"chargen root"}})"),
            parse, error));
        CHECK(!parse.profile.face);
        CHECK(Dropped(parse, "folder"));
    }

    {  // face strictness: junk source, out-of-range flags, missing jslot all
       // drop the block by name and the siblings still apply
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "face":{"jslot":"FR_Aria","source":"maybe"},
                      "skin":{"pack":"UBE"}})"),
            parse, error));
        CHECK(!parse.profile.face);
        CHECK(Dropped(parse, "block 'face'"));
        CHECK(parse.profile.skin && parse.profile.skin->pack == "UBE");

        parse = {};
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "face":{"jslot":"FR_Aria","flags":16}})"),
            parse, error));
        CHECK(!parse.profile.face);
        CHECK(Dropped(parse, "0..15"));

        parse = {};
        CHECK(ParseProfile(Parse(R"({"version":1,"name":"Aria","face":{}})"),
                           parse, error));
        CHECK(Dropped(parse, "jslot"));
    }

    {  // per-block malformation: every sibling of one bad block still applies
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "outfit":"not-an-object",
                      "overlays":{"hands":[{"index":1,
                        "texture":"!ube\\[spaz490]\\nail overlays\\ube_fingernails_basic.dds"}]},
                      "weight":"heavy"})"),
            parse, error));
        CHECK(Dropped(parse, "block 'outfit'"));
        CHECK(Dropped(parse, "block 'weight'"));
        CHECK(!parse.profile.outfit && !parse.profile.weight);
        CHECK(parse.profile.overlays);
        CHECK(parse.rewritable);  // one block parsed, the file may be rewritten

        parse = {};
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "overlays":"junk","makeup":{"index":0}})"),
            parse, error));
        CHECK(Dropped(parse, "block 'overlays'"));
        CHECK(Dropped(parse, "block 'makeup'"));
        CHECK(!parse.profile.overlays && !parse.profile.makeup);
    }

    {  // a file whose present known blocks ALL fail is never rewritten
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "outfit":"junk","skin":[],"weight":null})"),
            parse, error));
        CHECK(parse.dropped.size() == 3);
        CHECK(!parse.rewritable);

        // Metadata-only is not that case: nothing failed, rewrite is fine.
        parse = {};
        CHECK(ParseProfile(Parse(R"({"version":1,"name":"Aria"})"), parse, error));
        CHECK(parse.rewritable);
    }

    {  // unknown top-level blocks are preserved on rewrite
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "skin":{"pack":"UBE"},
                      "sculpt":{"vertices":[1,2,3]}})"),
            parse, error));
        CHECK(parse.profile.extras.isMember("sculpt"));
        const auto rewritten = ProfileToJson(parse.profile);
        CHECK(rewritten["sculpt"]["vertices"][1].asInt() == 2);
        CHECK(rewritten["skin"]["pack"].asString() == "UBE");
    }

    {  // ⚠ the exclusivity rule: face flags claim a channel, the FR block for
       // that channel drops with the reason string, and flags=0 drops nothing
        const auto base = ForgeProfile();

        auto encodeWithFlags = [&](std::uint32_t a_flags) {
            auto profile = base;
            profile.face->flags = a_flags;
            return ProfileToJson(profile);
        };

        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(encodeWithFlags(1), parse, error));
        CHECK(!parse.profile.overlays);
        CHECK(Dropped(parse, "block 'overlays'"));
        CHECK(Dropped(parse, "overrides"));
        CHECK(parse.profile.makeup && parse.profile.skin && parse.profile.body);

        parse = {};
        CHECK(ParseProfile(encodeWithFlags(2), parse, error));
        CHECK(!parse.profile.body);
        CHECK(parse.profile.shape && parse.profile.shape->morphs.empty());
        CHECK(!parse.profile.shape->scales.empty());
        CHECK(Dropped(parse, "block 'body'"));
        CHECK(Dropped(parse, "shape morphs"));

        parse = {};
        CHECK(ParseProfile(encodeWithFlags(4), parse, error));
        CHECK(parse.profile.shape && parse.profile.shape->scales.empty());
        CHECK(!parse.profile.shape->morphs.empty());

        parse = {};
        CHECK(ParseProfile(encodeWithFlags(8), parse, error));
        CHECK(!parse.profile.skin);
        CHECK(Dropped(parse, "block 'skin'"));

        parse = {};
        CHECK(ParseProfile(encodeWithFlags(15), parse, error));
        CHECK(!parse.profile.overlays && !parse.profile.body &&
              !parse.profile.shape && !parse.profile.skin);
        // Makeup is deliberately NOT in the exclusivity set: a flags=0 face
        // apply rebuilds the tint list anyway and the pipeline re-reads it.
        CHECK(parse.profile.makeup);
        CHECK(parse.profile.outfit);
    }

    {  // colour strictness: a malformed overlay tint means "leave that
       // channel alone" and the entry survives; a malformed makeup tint drops
       // its entry whole (strength and colour are one write)
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "overlays":{"face":[{"index":0,"tint":"706B9",
                        "texture":"Actors\\Character\\Overlays\\SFO\\Face\\Face Mole Forehead 1.dds"}]},
                      "makeup":[{"index":8,"type":"warpaint","tint":"XYZ123",
                                 "strength":0.8}]})"),
            parse, error));
        CHECK(parse.profile.overlays);
        const auto& face = parse.profile.overlays
                               ->byLocation[OverlayPlan::Slot(OverlayPlan::Location::kFace)];
        CHECK(face.size() == 1 && !face[0].state.hasTint);
        CHECK(!parse.profile.makeup);
        CHECK(Dropped(parse, "block 'makeup'"));
    }

    {  // duplicate indexes: the first entry wins, later ones are skipped
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "overlays":{"body":[
                        {"index":0,"tint":"706B99"},
                        {"index":0,"tint":"FF0000"}]}})"),
            parse, error));
        const auto& body = parse.profile.overlays
                               ->byLocation[OverlayPlan::Slot(OverlayPlan::Location::kBody)];
        CHECK(body.size() == 1);
        CHECK(body[0].state.tint == (OverlayPlan::Rgb{ 0x70, 0x6B, 0x99 }));
    }

    {  // makeup type round-trips: known types by stable id, unknown by number
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "makeup":[{"index":0,"type":"warpaint","tint":"706B99","strength":1.0},
                                {"index":1,"type":17,"tint":"706B99","strength":0.5},
                                {"index":2,"type":"paint-by-numbers","tint":"706B99","strength":1.0}]})"),
            parse, error));
        CHECK(parse.profile.makeup && parse.profile.makeup->size() == 2);
        CHECK((*parse.profile.makeup)[0].type ==
              static_cast<std::uint32_t>(MakeupPlan::Type::kWarPaint));
        CHECK((*parse.profile.makeup)[1].type == 17);
        const auto json = ProfileToJson(parse.profile);
        CHECK(json["makeup"][0]["type"].asString() == "warpaint");
        CHECK(json["makeup"][1]["type"].asInt() == 17);
    }

    {  // body: an OBody name alone works, an invalid embedded preset drops the
       // block by name, and an empty body block is meaningless
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria","body":{"obodyPreset":"Sindra"}})"),
            parse, error));
        CHECK(parse.profile.body && parse.profile.body->obodyPreset == "Sindra");
        CHECK(!parse.profile.body->custom);

        parse = {};
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "body":{"custom":{"version":9,"id":"x"}}})"),
            parse, error));
        CHECK(!parse.profile.body);
        CHECK(Dropped(parse, "block 'body'"));
        CHECK(Dropped(parse, "embedded preset"));

        parse = {};
        CHECK(ParseProfile(Parse(R"({"version":1,"name":"Aria","body":{}})"),
                           parse, error));
        CHECK(!parse.profile.body);
    }

    {  // shape: junk values are skipped, and a shape with nothing usable drops
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "shape":{"morphs":{"Skinny2.0":0.581,"Bad":"x"},
                               "scales":{"hands":1.05}}})"),
            parse, error));
        CHECK(parse.profile.shape);
        CHECK(parse.profile.shape->morphs.size() == 1);
        CHECK(parse.profile.shape->morphs.at("Skinny2.0") == 0.581f);
        CHECK(parse.profile.shape->scales.at("hands") == 1.05f);

        parse = {};
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria","shape":{"morphs":{"Bad":"x"}}})"),
            parse, error));
        CHECK(!parse.profile.shape);
        CHECK(Dropped(parse, "block 'shape'"));
    }

    {  // weight clamps to the engine's 0..100
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(Parse(R"({"version":1,"name":"Aria","weight":150})"),
                           parse, error));
        CHECK(parse.profile.weight && *parse.profile.weight == 100.0f);
    }

    {  // skin: an EMPTY pack is a value ("wears none, take one off") and
       // round-trips; a missing pack member is still junk
        ProfileCodec::Profile bare;
        bare.name = "Aria";
        bare.skin = ProfileCodec::SkinBlock{ "" };
        const auto  json = ProfileToJson(bare);
        CHECK(json.isMember("skin"));
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(json, parse, error));
        CHECK(parse.profile.skin && parse.profile.skin->pack.empty());

        parse = {};
        CHECK(ParseProfile(Parse(R"({"version":1,"name":"Aria","skin":{}})"),
                           parse, error));
        CHECK(!parse.profile.skin);
        CHECK(Dropped(parse, "block 'skin'"));
    }

    {  // character: mod, id and female are all required; junk drops the block
       // and the siblings live on
        ProfileParse parse;
        std::string  error;
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "character":{"mod":"Skyrim.esm","id":"0x013742"}})"),
            parse, error));
        CHECK(!parse.profile.character);
        CHECK(Dropped(parse, "block 'character'"));

        parse = {};
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "character":{"mod":"Skyrim.esm","id":"0x013742",
                                   "female":false}})"),
            parse, error));
        CHECK(parse.profile.character);
        CHECK(parse.profile.character->race.modName == "Skyrim.esm");
        CHECK(parse.profile.character->race.localFormID == 0x013742);
        CHECK(!parse.profile.character->female);
    }

    {  // the look's own hair colour: the field that exists because skee's
       // jslot key goes missing whenever the base has no colour form at save
       // time, and an absent key reads back as 0 = black. FR_Nord 1.jslot on
       // the dev rig carried `weight` and nothing else, and applying it
       // painted (0,0,0) over a Nord whose own colour is (57,55,40).
        std::string error;
        ProfileCodec::ProfileParse parse;

        // Absent is the ordinary case and must STAY absent, so a profile
        // written before this field resaves byte-identical.
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria","face":{"jslot":"FR_Aria"}})"),
            parse, error));
        CHECK(parse.profile.face);
        CHECK(!parse.profile.face->hairColour);
        CHECK(!ProfileToJson(parse.profile)["face"].isMember("hairColor"));

        // Present unpacks 0xRRGGBB and survives a round trip unchanged. The
        // fixture is (57,55,40), the Nord's own colour on the dev rig, so a
        // wrong unpack reads as the exact bug this field was added for.
        parse = {};
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "face":{"jslot":"FR_Aria","hairColor":3749672}})"),
            parse, error));
        CHECK(parse.profile.face->hairColour);
        CHECK(parse.profile.face->hairColour->set);
        CHECK(parse.profile.face->hairColour->r == 57);
        CHECK(parse.profile.face->hairColour->g == 55);
        CHECK(parse.profile.face->hairColour->b == 40);
        const auto round = ProfileToJson(parse.profile);
        CHECK(round["face"]["hairColor"].asUInt() == 3749672u);
        ProfileCodec::ProfileParse again;
        CHECK(ParseProfile(round, again, error));
        CHECK(again.profile.face == parse.profile.face);

        // Out of range is REFUSED, not clamped: this value gets painted onto a
        // character, and half of a malformed colour is a colour nobody chose.
        // A refused face block drops the block rather than the whole file.
        parse = {};
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "face":{"jslot":"FR_Aria","hairColor":16777216}})"),
            parse, error));
        CHECK(!parse.profile.face);
        CHECK(Dropped(parse, "block 'face'"));

        parse = {};
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "face":{"jslot":"FR_Aria","hairColor":"black"}})"),
            parse, error));
        CHECK(!parse.profile.face);

        // Black is a colour somebody can legitimately pick, so 0 with the key
        // PRESENT is not the same state as the key being absent. This is the
        // distinction the whole field exists to preserve.
        parse = {};
        CHECK(ParseProfile(
            Parse(R"({"version":1,"name":"Aria",
                      "face":{"jslot":"FR_Aria","hairColor":0}})"),
            parse, error));
        CHECK(parse.profile.face->hairColour);
        CHECK(parse.profile.face->hairColour->set);
        CHECK(parse.profile.face->hairColour->r == 0);
    }

    {  // ---- what a preset needs installed --------------------------------
        using namespace OS::PresetRequirements;

        // ⚠ FIXTURE FORGED FROM A REAL JSLOT, per the house rule at the top of
        // this file. Read off the live rig 2026-08-26 from
        // MODS\mods\Adord - Nord Racemenu Preset\SKSE\Plugins\CharGen\Presets\
        // Male\Nord\Adord.jslot, which is formatVersion 3, as are all 400 of
        // the 537 jslots on this rig that were sampled. Every value below is
        // that file's, trimmed to the members this reads.
        const std::string kJslot = R"({
            "version": {"formatVersion": 3},
            "modNames": ["Brows.esp", "High Poly Head.esm", "KS Hairdo's.esp",
                         "Kala's Eyes - Improved.esp", "Skyrim.esm"],
            "actor": {"headTexture": "Skyrim.esm|03B521"},
            "headParts": [
                {"formIdentifier": "Skyrim.esm|051631", "type": 0},
                {"formIdentifier": "High Poly Head.esm|000806", "type": 1},
                {"formIdentifier": "KS Hairdo's.esp|01D8F9", "type": 3},
                {"formIdentifier": "Kala's Eyes - Improved.esp|13A834", "type": 7},
                {"formIdentifier": "Brows.esp|0408DA", "type": 8}
            ]
        })";
        {
            std::string why;
            const auto  got = PluginsFromJslot(Parse(kJslot), why);
            CHECK(why.empty());
            // Sorted case-insensitively and deduped, so the same plugin named
            // in modNames AND in a headPart is listed once.
            const std::vector<std::string> want{
                "Brows.esp", "High Poly Head.esm", "Kala's Eyes - Improved.esp",
                "KS Hairdo's.esp", "Skyrim.esm"
            };
            CHECK(got == want);
        }
        {  // ⚠⚠ A HEADPART NAMING A PLUGIN modNames FORGOT IS STILL A
            // REQUIREMENT. modNames is authoritative on every file measured,
            // but the union costs nothing and a preset that references a form
            // it does not declare would otherwise apply to a hole.
            std::string why;
            const auto  got = PluginsFromJslot(
                Parse(R"({"modNames":["Skyrim.esm"],
                          "headParts":[{"formIdentifier":"Undeclared.esp|000123"}]})"),
                why);
            CHECK(why.empty());
            CHECK(got == std::vector<std::string>({ "Skyrim.esm", "Undeclared.esp" }));
        }
        {  // ⚠⚠ FAIL SOFT: AN UNREADABLE SHAPE SAYS SO AND NAMES NOTHING. A
            // wrong requirements list is worse than none, so anything that is
            // not the shape this knows returns empty WITH a reason.
            std::string why;
            CHECK(PluginsFromJslot(Parse("[]"), why).empty());
            CHECK(!why.empty());

            why.clear();
            CHECK(PluginsFromJslot(Parse(R"({"version":{"formatVersion":3}})"), why).empty());
            CHECK(!why.empty());

            why.clear();
            CHECK(PluginsFromJslot(Parse(R"({"modNames":"Skyrim.esm"})"), why).empty());
            CHECK(!why.empty());

            // ⚠ GATED ON THE SHAPE, NOT THE VERSION NUMBER. A newer RaceMenu
            // that still writes modNames is still readable; one that renames it
            // falls into the case above and says so.
            why.clear();
            CHECK(PluginsFromJslot(
                      Parse(R"({"version":{"formatVersion":99},
                                "modNames":["Skyrim.esm"]})"),
                      why) == std::vector<std::string>({ "Skyrim.esm" }));
            CHECK(why.empty());
        }
        {  // ⚠⚠ A PLUGIN NAME IS NOT CASE SENSITIVE AND THE TWO SOURCES
            // DISAGREE. A jslot writes whatever the author's load order held
            // and a headPart writes whatever the record said, so a
            // case-sensitive dedup lists one plugin twice and reports a
            // requirement the player already satisfies.
            std::string why;
            const auto  got = PluginsFromJslot(
                Parse(R"({"modNames":["Skyrim.esm"],
                          "headParts":[{"formIdentifier":"SKYRIM.ESM|051631"}],
                          "actor":{"headTexture":"skyrim.esm|03B521"}})"),
                why);
            CHECK(why.empty());
            CHECK(got.size() == 1);
            CHECK(got == std::vector<std::string>({ "Skyrim.esm" }));
        }
        {  // Junk entries inside a good array are skipped, not fatal.
            std::string why;
            const auto  got = PluginsFromJslot(
                Parse(R"({"modNames":["Skyrim.esm", 7, "", "Dawnguard.esm"]})"), why);
            CHECK(why.empty());
            CHECK(got == std::vector<std::string>({ "Dawnguard.esm", "Skyrim.esm" }));
        }

        {  // The Looks side: every StyleRefKey the profile carries, plus its
            // own requires line and the race it was captured on.
            const auto forged = ForgeProfile();
            const auto got    = PluginsFromProfile(forged);
            CHECK(got == std::vector<std::string>({ "1Markynaz.esl",
                                                    "ccbgssse021-lordsmail.esl",
                                                    "Skyrim.esm" }));
        }
        {  // ⚠⚠ THE WEAPON AND HEAD-PART DIMENSIONS COUNT TOO. exportHealth
            // walks armour and weapons; a look also carries head-part dyes, and
            // a part from a mod that is gone is exactly the absence this list
            // exists to name.
            auto p = ForgeProfile();
            p.outfit->SetWeaponStyle(OS::WeaponClass::Sword,
                                     OS::StyleRefKey{ "AnotherWeapon.esp", 0x800 });
            // ⚠ A HEAD-PART DYE WITH NO CHANNEL SET IS ERASED ON THE WAY IN,
            // by SetHeadPartDye's own rule, so the entry has to carry a colour
            // for the walk to have anything to find.
            p.outfit->SetHeadPartDye(31, OS::StyleRefKey{ "KS Hairdo's.esp", 0x1D8F9 },
                                     OS::DyeChannelId::kPrimary,
                                     OS::DyeChannel{ true, 0x70, 0x6B, 0x99 });
            const auto got = PluginsFromProfile(p);
            CHECK(got == std::vector<std::string>({ "1Markynaz.esl", "AnotherWeapon.esp",
                                                    "ccbgssse021-lordsmail.esl",
                                                    "KS Hairdo's.esp", "Skyrim.esm" }));
        }
        {  // A profile that names nothing names nothing.
            OS::ProfileCodec::Profile bare;
            bare.name = "bare";
            CHECK(PluginsFromProfile(bare).empty());
        }
    }

    {  // ---- the overlay record's eraser -------------------------------------
        // ⚠⚠ THE FIELD ROUND THESE COME FROM, 2026-08-27. OverlayBaseline held
        // every overlay Fitting Room had ever written and had no way to be told
        // one had been taken off, so a look import onto another character put
        // the outgoing one's face art back 196 ms later: a male Nord in
        // Umbrael's '(SDZ21) Fabulous Makeup 2'. The record's own bookkeeping
        // is pure and is what these hold; the choke point that calls it is in
        // OverlayApi::Clear.
        using OS::OverlayBaseline::AnyIn;
        using OS::OverlayBaseline::ForgetIn;
        using OS::OverlayPlan::Location;

        const auto face2 = [] {
            OS::ProfileCodec::OverlayEntry e{};
            e.index            = 2;
            e.state.hasTexture = true;
            e.state.texture =
                "Actors\\Character\\Overlays\\(SDZ21) Fabulous Makeup 2\\"
                "SDZ21 Fabulous Makeup 2 UBE Full 8 Opt 5.dds";
            return e;
        }();

        {  // An empty record is empty, and forgetting from it is not an error.
            OS::ProfileCodec::OverlaysBlock block;
            CHECK(!AnyIn(block));
            CHECK(!ForgetIn(block, Location::kFace, 2));
            CHECK(!AnyIn(block));
        }
        {  // The measured layer goes in, comes out, and takes the record's
            // "anything at all" with it. That last part is the whole bug: a
            // flag left saying yes over an empty block still reasserts.
            OS::ProfileCodec::OverlaysBlock block;
            block.byLocation[OS::OverlayPlan::Slot(Location::kFace)].push_back(face2);
            CHECK(AnyIn(block));
            CHECK(ForgetIn(block, Location::kFace, 2));
            CHECK(!AnyIn(block));
            CHECK(!ForgetIn(block, Location::kFace, 2));  // and only once
        }
        {  // ⚠ THE INDEX IS PER LOCATION, so face 2 and body 2 are two layers
            // and forgetting one leaves the other. The record is addressed the
            // way a look's block is, and this is where an index-only match
            // would silently take the wrong slot off.
            OS::ProfileCodec::OverlaysBlock block;
            block.byLocation[OS::OverlayPlan::Slot(Location::kFace)].push_back(face2);
            block.byLocation[OS::OverlayPlan::Slot(Location::kBody)].push_back(face2);
            CHECK(ForgetIn(block, Location::kFace, 2));
            CHECK(AnyIn(block));
            CHECK(block.byLocation[OS::OverlayPlan::Slot(Location::kFace)].empty());
            CHECK(block.byLocation[OS::OverlayPlan::Slot(Location::kBody)].size() == 1);
        }
        {  // Neighbours in one location keep their own indices. Umbrael wore
            // Face [Ovl0] and Face [Ovl2] together in the round above, and the
            // report was that clearing one brought both back.
            OS::ProfileCodec::OverlaysBlock block;
            auto&                           face =
                block.byLocation[OS::OverlayPlan::Slot(Location::kFace)];
            OS::ProfileCodec::OverlayEntry face0{};
            face0.index            = 0;
            face0.state.hasTexture = true;
            face0.state.texture = "Actors\\Character\\Overlays\\SFO\\Face\\Face Mole Cheek 3.dds";
            face.push_back(face0);
            face.push_back(face2);
            CHECK(ForgetIn(block, Location::kFace, 2));
            CHECK(face.size() == 1);
            CHECK(face.front().index == 0);
            CHECK(AnyIn(block));
        }
    }

    {  // ---- the load-boundary judgement --------------------------------------
        // ⚠⚠ THE FIELD ROUND THIS COMES FROM, 2026-09-01 21:03. Apply Umbrael,
        // apply Almalexia over her, save, load: our cosave restored Almalexia's
        // layers, skee's came back holding Umbrael's set and landed late, and
        // the vacuum-only reassert stood down because "skee already holds 7
        // layer(s) with art". Art in the store is not proof anybody currently
        // means it, so the judgement compares CONTENT.
        using OS::OverlayBaseline::FindIn;
        using OS::OverlayBaseline::JudgeStore;
        using OS::OverlayBaseline::SameOverlayArt;
        using OS::OverlayBaseline::StoreVerdict;
        using OS::OverlayPlan::Layer;
        using OS::OverlayPlan::LayerState;
        using OS::OverlayPlan::Location;

        const auto makeLayer = [](Location a_loc, std::uint32_t a_index,
                                  const char* a_node) {
            Layer layer;
            layer.location = a_loc;
            layer.index    = a_index;
            layer.node     = a_node;
            return layer;
        };
        const auto art = [](const char* a_path) {
            LayerState state{};
            state.hasTexture = true;
            state.texture    = a_path;
            return state;
        };
        const char* kAlmaBody = "CO 3\\63 Body F.dds";
        const char* kUmbrFace = "!UBE\\Actors\\Character\\Overlays\\Community "
                                "Overlays\\CO 3\\64 Head Alt F.dds";

        OS::ProfileCodec::OverlaysBlock record;
        {
            OS::ProfileCodec::OverlayEntry body0{};
            body0.index = 0;
            body0.state = art(kAlmaBody);
            record.byLocation[OS::OverlayPlan::Slot(Location::kBody)].push_back(body0);
        }
        const auto body0 = makeLayer(Location::kBody, 0, "Body [Ovl0]");
        const auto body1 = makeLayer(Location::kBody, 1, "Body [Ovl1]");
        const auto face0 = makeLayer(Location::kFace, 0, "Face [Ovl0]");

        {  // The record's art on its layer, nothing else worn: agreement.
            std::vector<std::pair<Layer, LayerState>> live;
            live.emplace_back(body0, art(kAlmaBody));
            live.emplace_back(body1, LayerState{});
            live.emplace_back(face0, LayerState{});
            CHECK(JudgeStore(record, live) == StoreVerdict::kAgrees);
        }
        {  // No art anywhere: the original measured vacuum, and it stays its
            // own verdict because the write path differs (Install may be owed).
            std::vector<std::pair<Layer, LayerState>> live;
            live.emplace_back(body0, LayerState{});
            live.emplace_back(body1, LayerState{});
            CHECK(JudgeStore(record, live) == StoreVerdict::kVacuum);
        }
        {  // The 21:03 shape: the record's layer wears somebody else's art and
            // an unclaimed layer wears art at all. Both convict on their own.
            std::vector<std::pair<Layer, LayerState>> live;
            live.emplace_back(body0, art(kUmbrFace));
            CHECK(JudgeStore(record, live) == StoreVerdict::kDiverged);
            std::vector<std::pair<Layer, LayerState>> extra;
            extra.emplace_back(body0, art(kAlmaBody));
            extra.emplace_back(face0, art(kUmbrFace));
            CHECK(JudgeStore(record, extra) == StoreVerdict::kDiverged);
        }
        {  // A claimed layer worn BARE while the record says art: diverged too,
            // which is the half a count-based test can never see.
            std::vector<std::pair<Layer, LayerState>> live;
            live.emplace_back(body0, LayerState{});
            live.emplace_back(body1, art(kUmbrFace));
            CHECK(JudgeStore(record, live) == StoreVerdict::kDiverged);
        }
        {  // ⚠⚠ A LOST TINT CONVICTS. Field 2026-09-01 21:41: the skeleton face
            // art survived the load and its black tint did not, and the
            // art-only judge called the pale skull agreement. Both sides are
            // stored bytes, so a byte that moved is a divergence, not wobble.
            auto claimed  = art(kAlmaBody);
            claimed.hasTint = true;
            claimed.tint    = OS::OverlayPlan::Rgb{ 0, 0, 0 };
            OS::ProfileCodec::OverlaysBlock tintedRecord;
            {
                OS::ProfileCodec::OverlayEntry e{};
                e.index = 0;
                e.state = claimed;
                tintedRecord.byLocation[OS::OverlayPlan::Slot(Location::kBody)]
                    .push_back(e);
            }
            std::vector<std::pair<Layer, LayerState>> untinted;
            untinted.emplace_back(body0, art(kAlmaBody));  // art kept, tint gone
            CHECK(JudgeStore(tintedRecord, untinted) == StoreVerdict::kDiverged);
            std::vector<std::pair<Layer, LayerState>> intact;
            intact.emplace_back(body0, claimed);
            CHECK(JudgeStore(tintedRecord, intact) == StoreVerdict::kAgrees);
            // An alpha that moved convicts too; the codec-roundtrip epsilon
            // does not.
            auto faded     = claimed;
            faded.hasAlpha = true;
            faded.alpha    = 0.5f;
            auto fullRec       = claimed;
            fullRec.hasAlpha   = true;
            fullRec.alpha      = 1.0f;
            OS::ProfileCodec::OverlaysBlock alphaRecord;
            {
                OS::ProfileCodec::OverlayEntry e{};
                e.index = 0;
                e.state = fullRec;
                alphaRecord.byLocation[OS::OverlayPlan::Slot(Location::kBody)]
                    .push_back(e);
            }
            std::vector<std::pair<Layer, LayerState>> halved;
            halved.emplace_back(body0, faded);
            CHECK(JudgeStore(alphaRecord, halved) == StoreVerdict::kDiverged);
            auto nearAlpha  = fullRec;
            nearAlpha.alpha = 0.995f;
            std::vector<std::pair<Layer, LayerState>> near;
            near.emplace_back(body0, nearAlpha);
            CHECK(JudgeStore(alphaRecord, near) == StoreVerdict::kAgrees);
        }
        {  // The path fold: case, slashes, and the 'textures\' prefix skee's
            // store and a cosave roundtrip disagree on. A different file still
            // misses however it is spelled.
            CHECK(SameOverlayArt("CO 3\\63 Body F.dds", "co 3/63 body f.DDS"));
            CHECK(SameOverlayArt("textures\\CO 3\\63 Body F.dds", "CO 3\\63 Body F.dds"));
            CHECK(SameOverlayArt("CO 3\\63 Body F.dds", "Textures/CO 3/63 Body F.dds"));
            CHECK(!SameOverlayArt("CO 3\\63 Body F.dds", "CO 3\\64 Head Alt F.dds"));
            CHECK(!SameOverlayArt("textures\\a.dds", "textures\\b.dds"));
        }
        {  // FindIn answers by location AND index, and an entry is a claim
            // whatever its state holds.
            CHECK(FindIn(record, Location::kBody, 0) != nullptr);
            CHECK(FindIn(record, Location::kBody, 1) == nullptr);
            CHECK(FindIn(record, Location::kFace, 0) == nullptr);
        }
        {  // ⚠⚠ THE ETERNAL FIGHT OF 2026-09-01 22:00. Almalexia's look
            // captured the DEFAULT texture with a 'textures\' prefix; the
            // exact-size default test read that entry as ART on a layer whose
            // live reading is bare, and the judge convicted the same
            // divergence on every check, three re-asserts in one minute. A
            // claim of the default is a claim of bareness however spelled.
            using OS::OverlayBaseline::OccupiedLoose;
            auto spelledDefault = art("textures\\actors\\character\\overlays\\default.dds");
            CHECK(OS::OverlayPlan::Occupied(spelledDefault));  // the trap
            CHECK(!OccupiedLoose(spelledDefault));             // the cure
            CHECK(!OccupiedLoose(art("Actors\\Character\\Overlays\\Default.dds")));
            CHECK(OccupiedLoose(art(kAlmaBody)));

            OS::ProfileCodec::OverlaysBlock spelled;
            {
                OS::ProfileCodec::OverlayEntry e{};
                e.index = 0;
                e.state = art(kAlmaBody);
                spelled.byLocation[OS::OverlayPlan::Slot(Location::kBody)].push_back(e);
                OS::ProfileCodec::OverlayEntry d{};
                d.index = 0;
                d.state = spelledDefault;
                spelled.byLocation[OS::OverlayPlan::Slot(Location::kFace)].push_back(d);
            }
            std::vector<std::pair<Layer, LayerState>> live;
            live.emplace_back(body0, art(kAlmaBody));
            live.emplace_back(face0, LayerState{});  // bare, as the claim means
            CHECK(JudgeStore(spelled, live) == StoreVerdict::kAgrees);
            // And a WORN layer under a bare claim still convicts, so the
            // loosening cannot hide a real stranger.
            std::vector<std::pair<Layer, LayerState>> worn;
            worn.emplace_back(body0, art(kAlmaBody));
            worn.emplace_back(face0, art(kUmbrFace));
            CHECK(JudgeStore(spelled, worn) == StoreVerdict::kDiverged);
        }
    }

    {  // ---- a bare tint-list claim round-trips as a stamped wrapper --------
        // The 'MKUP' record's shape for a character wearing nothing: the codec
        // omits an empty makeup block, so the wrapper carries only its name and
        // the character stamp, and the load reads "no block" as the bare claim
        // (field 2026-09-02 02:40, the save that carried no record at all).
        ProfileCodec::Profile bare;
        bare.name   = "makeup-baseline";
        bare.makeup = std::vector<ProfileCodec::MakeupEntry>{};
        ProfileCodec::CharacterBlock who;
        who.race.modName    = "Skyrim.esm";
        who.race.localFormID = 0x013746;
        who.female          = false;
        bare.character      = who;
        const auto json = ProfileToJson(bare);
        CHECK(!json.isMember("makeup"));
        CHECK(json.isMember("character"));
        ProfileParse back;
        std::string  error;
        CHECK(ParseProfile(json, back, error));
        CHECK(!back.profile.makeup);
        CHECK(back.profile.character.has_value());
        CHECK(back.profile.character && back.profile.character->race.localFormID == 0x013746);
        CHECK(back.profile.character && !back.profile.character->female);
    }

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all profile codec tests passed\n");
    return 0;
}
