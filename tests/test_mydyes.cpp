#include "MyDyes.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS;

    {  // Id folding: lowercased, namespaced, path-hostile characters folded.
        CHECK(MyDyes::IdForName("Rose Gold") == "custom:rose gold");
        CHECK(MyDyes::IdForName("a/b\\c:d") == "custom:a_b_c_d");
        CHECK(MyDyes::IdForName("") == "custom:");
    }

    {  // ⚠ THE ROUND TRIP COVERS EVERY SPECIAL FIELD, through the real
       // parser. Schemes silently flatten these; this suite is what makes
       // that gap impossible to repeat here.
        Dye d;
        d.id     = MyDyes::IdForName("Test Pearl");
        d.name   = "Test Pearl";
        d.custom = true;
        d.colour = DyeChannel{ true, 0x10, 0x20, 0x30 };
        d.colour.mode      = 2;
        d.colour.secondSet = true;
        d.colour.r2        = 0x40;
        d.colour.g2        = 0x50;
        d.colour.b2        = 0x60;
        d.colour.flake     = 77;
        d.colour.blend     = 3;  // screen
        d.colour.palette.glossSet = true;
        d.colour.palette.gloss    = 200;
        d.colour.palette.sheenSet = true;
        d.colour.palette.sheenR   = 1;
        d.colour.palette.sheenG   = 2;
        d.colour.palette.sheenB   = 3;
        // The player's fields, which must NOT survive into the pack.
        d.colour.strength        = 7;
        d.colour.player.glossSet = true;
        d.colour.player.gloss    = 9;

        std::vector<Dye> parsed;
        const auto tally = DyePalette::DyesFromJson(MyDyes::PackToJson({ d }), 0, parsed);
        CHECK(tally.rejected == 0);
        CHECK(parsed.size() == 1);
        if (parsed.size() == 1) {
            const auto& p = parsed[0];
            CHECK(p.id == d.id && p.name == d.name);
            CHECK(p.colour.r == 0x10 && p.colour.g == 0x20 && p.colour.b == 0x30);
            CHECK(p.colour.mode == 2);
            CHECK(p.colour.secondSet && p.colour.r2 == 0x40 && p.colour.g2 == 0x50 &&
                  p.colour.b2 == 0x60);
            CHECK(p.colour.flake == 77);
            CHECK(p.colour.blend == 3);
            CHECK(p.colour.palette.glossSet && p.colour.palette.gloss == 200);
            CHECK(p.colour.palette.sheenSet && p.colour.palette.sheenR == 1 &&
                  p.colour.palette.sheenG == 2 && p.colour.palette.sheenB == 3);
            // The player's fields land as their defaults: never serialised.
            CHECK(p.colour.strength == 255);
            CHECK(!p.colour.player.glossSet && !p.colour.player.sheenSet);
            // No rarity written, so the dye is free under lore promotion.
            CHECK(p.rarity.empty());
        }
    }

    {  // A flat custom colour writes no special keys at all and still round
       // trips.
        Dye d;
        d.id     = MyDyes::IdForName("plain");
        d.name   = "plain";
        d.colour = DyeChannel{ true, 0xAB, 0xCD, 0xEF };
        const auto o = MyDyes::DyeToJson(d);
        CHECK(!o.isMember("hex2") && !o.isMember("mode") && !o.isMember("flake") &&
              !o.isMember("gloss") && !o.isMember("sheen"));
        // ⚠ AND NO BLEND KEY EITHER. A dye that defers must look on disk
        // exactly like every dye authored before blends existed, so writing
        // "default" would put a word in a hand-editable file that means
        // nothing to a reader and nothing to the parser.
        CHECK(!o.isMember("blend"));
    }

    {  // ⚠⚠ THE SAVE RACE, AND THIS IS THE TEST THAT NAMES IT. The list a save
       // merges into has to come from the PACK FILE, never from the palette:
       // the palette only gains a newly saved dye after a marshalled
       // DyePalette::Load lands, so two saves inside one reload window used to
       // write a pack holding only the second, and the first was gone from
       // disk.
        Dye first;
        first.id     = MyDyes::IdForName("First");
        first.name   = "First";
        first.colour = DyeChannel{ true, 0x11, 0x22, 0x33 };

        const Json::StreamWriterBuilder wb;
        const auto packText = Json::writeString(wb, MyDyes::PackToJson({ first }));

        auto onDisk = MyDyes::CustomsFromPackText(packText);
        CHECK(onDisk.has_value());
        if (onDisk) {
            CHECK(onDisk->size() == 1);
            // The origin stamp is a fact about the FILE, and this text is the
            // custom pack, so the parser owes it the mark the loader gives it.
            CHECK(onDisk->size() == 1 && (*onDisk)[0].custom);

            Dye second;
            second.id     = MyDyes::IdForName("Second");
            second.name   = "Second";
            second.colour = DyeChannel{ true, 0x44, 0x55, 0x66 };
            MyDyes::UpsertById(*onDisk, second);
            CHECK(onDisk->size() == 2);
            CHECK(onDisk->size() == 2 && (*onDisk)[0].id == MyDyes::IdForName("First"));
        }
    }

    {  // ⚠ AN UNREADABLE PACK IS NOT AN EMPTY ONE. Handing back an empty list
       // for a file that exists and will not parse is exactly what lets a save
       // overwrite dyes it never saw, so these refuse instead.
        CHECK(!MyDyes::CustomsFromPackText("").has_value());
        CHECK(!MyDyes::CustomsFromPackText("{").has_value());
        CHECK(!MyDyes::CustomsFromPackText("not json at all").has_value());
        // A root that is not an object. jsoncpp's operator[] THROWS on one, so
        // this is the crash guard as much as the refusal, the same rule
        // DyesFromJson already keeps for the loader.
        CHECK(!MyDyes::CustomsFromPackText("[]").has_value());
        // "dyes" present and the wrong type: the file says something this code
        // does not understand, so it is refused rather than read as zero dyes.
        CHECK(!MyDyes::CustomsFromPackText(R"({"dyes": "nope"})").has_value());

        // ⚠ AND AN OBJECT WITH NO dyes KEY IS A READABLE EMPTY PACK, not a
        // refusal. It is what the loader reads as zero dyes, and refusing it
        // would dead-end a player whose pack was hand-emptied: every save
        // after would be silently declined with the file sitting right there.
        const auto bare = MyDyes::CustomsFromPackText("{}");
        CHECK(bare.has_value() && bare->empty());
        const auto empty = MyDyes::CustomsFromPackText(R"({"dyes": []})");
        CHECK(empty.has_value() && empty->empty());
    }

    {  // A pack whose entries are individually bad loses those entries and
       // keeps the file: that is DyesFromJson's own split between a rejected
       // ENTRY and an unreadable FILE, and it must survive here or one typo in
       // a hand-edited pack would refuse every future save.
        const auto mixed = MyDyes::CustomsFromPackText(
            R"({"dyes": [{"id":"custom:ok","name":"ok","hex":"112233"},{"name":"no id"}]})");
        CHECK(mixed.has_value());
        CHECK(mixed.has_value() && mixed->size() == 1);
    }

    {  // Upsert: same id replaces, new id appends.
        std::vector<Dye> list;
        Dye a;
        a.id   = "custom:a";
        a.name = "a";
        MyDyes::UpsertById(list, a);
        CHECK(list.size() == 1);
        a.name = "a2";
        MyDyes::UpsertById(list, a);
        CHECK(list.size() == 1 && list[0].name == "a2");
        Dye b;
        b.id = "custom:b";
        MyDyes::UpsertById(list, b);
        CHECK(list.size() == 2);
    }

    if (g_failures == 0) {
        std::printf("MyDyesTests: all passed\n");
    }
    return g_failures;
}
