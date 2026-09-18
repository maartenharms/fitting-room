// Dye palette tests. No SKSE, no engine - a dye is an id, a name, a colour
// and a price, and the store is JSON on disk.
#include "DyePalette.h"

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

int main() {
    using namespace OS;

    {  // one dye parses, with every field
        Json::Value j;
        j["id"]   = "vanilla:ebony";
        j["name"] = "Ebony Black";
        j["hex"]  = "1C1A1F";
        j["cost"] = 250;

        Dye d;
        CHECK(DyePalette::DyeFromJson(j, 100, d));
        CHECK(d.id == "vanilla:ebony");
        CHECK(d.name == "Ebony Black");
        CHECK(d.colour == (DyeChannel{ true, 0x1C, 0x1A, 0x1F }));
        CHECK(d.cost == 250u);
    }

    {  // cost is optional and falls back to the caller's default, so a
       // hand-authored pack does not have to price every entry
        Json::Value j;
        j["id"]   = "vanilla:ash";
        j["name"] = "Ash Grey";
        j["hex"]  = "8A8A8A";

        Dye d;
        CHECK(DyePalette::DyeFromJson(j, 100, d));
        CHECK(d.cost == 100u);
    }

    {  // the root itself has to be an object: DyesFromJson (Task 2) will hand
       // this one entry at a time from a hand-edited pack, and an array entry
       // that is a bare string or number must be dropped rather than crash
       // reaching for ["id"]
        Dye out;
        CHECK(!DyePalette::DyeFromJson(Json::Value(), 100, out));
        CHECK(!DyePalette::DyeFromJson(Json::Value("nope"), 100, out));
    }

    {  // untrusted input is DROPPED, never guessed
        // Checked after EVERY call below, not once at the end. id and name
        // are immune to a later case masking an earlier bad write, because a
        // non-empty string stays non-empty through a later rejection.
        // colour is NOT immune the same way: noName below hands DyeFromJson a
        // VALID hex ("FFFFFF"), so a regression that wrote a_out.colour before
        // finishing validation could leave out.colour.set true right there,
        // and badHex/shortHex below would then overwrite it back to unset
        // before a single end-of-block check ever saw it.
        Json::Value noId;
        noId["name"] = "Nameless";
        noId["hex"]  = "FFFFFF";
        Dye out;
        CHECK(!DyePalette::DyeFromJson(noId, 100, out));
        CHECK(out.id.empty());
        CHECK(out.name.empty());
        CHECK(!out.colour.set);

        Json::Value noName;
        noName["id"]  = "vanilla:x";
        noName["hex"] = "FFFFFF";
        CHECK(!DyePalette::DyeFromJson(noName, 100, out));
        CHECK(out.id.empty());
        CHECK(out.name.empty());
        CHECK(!out.colour.set);

        Json::Value badHex;
        badHex["id"]   = "vanilla:y";
        badHex["name"] = "Bad";
        badHex["hex"]  = "nonsense";
        CHECK(!DyePalette::DyeFromJson(badHex, 100, out));
        CHECK(out.id.empty());
        CHECK(out.name.empty());
        CHECK(!out.colour.set);

        Json::Value shortHex;
        shortHex["id"]   = "vanilla:z";
        shortHex["name"] = "Short";
        shortHex["hex"]  = "FFF";
        CHECK(!DyePalette::DyeFromJson(shortHex, 100, out));
        CHECK(out.id.empty());
        CHECK(out.name.empty());
        CHECK(!out.colour.set);
    }

    {  // rarity parses and is optional. Absent means empty, which the rules
       // resolve as free, so a pack that predates the economy keeps working.
        Json::Value j;
        j["id"]     = "eso:void-pitch";
        j["name"]   = "Void Pitch";
        j["hex"]    = "000000";
        j["rarity"] = "Rare";

        Dye d;
        CHECK(DyePalette::DyeFromJson(j, 100, d));
        CHECK(d.rarity == "Rare");

        // ⚠ d is REUSED, carrying "Rare" in. An absent rarity has to write the
        // empty one over it rather than leave the previous entry's tier
        // behind, because a_out belongs to the CALLER and this function cannot
        // know what is in it: a conditional assignment would give an untiered
        // dye whatever rarity the buffer arrived carrying, and inheriting
        // Common is inheriting free.
        //
        // ⚠ The reuse is this test's doing, not the shipped caller's.
        // DyesFromJson declares a fresh Dye inside its loop, so the buffer it
        // hands over is always clean. The comment here and in DyePalette.cpp
        // both claimed otherwise until 2026-08-02. The contract is what makes
        // the unconditional write necessary, and reusing a buffer on purpose
        // is how that contract gets held.
        Json::Value bare;
        bare["id"]   = "vanilla:ebony";
        bare["name"] = "Ebony Black";
        bare["hex"]  = "1C1A1F";
        CHECK(DyePalette::DyeFromJson(bare, 100, d));
        CHECK(d.rarity.empty());

        // a rarity that is not a string is IGNORED rather than fatal: it is
        // metadata, and losing a colour over it would be a worse trade than
        // losing its tier
        Json::Value oddRarity;
        oddRarity["id"]     = "eso:x";
        oddRarity["name"]   = "X";
        oddRarity["hex"]    = "010203";
        oddRarity["rarity"] = 7;
        CHECK(DyePalette::DyeFromJson(oddRarity, 100, d));
        CHECK(d.rarity.empty());

        // ⚠ AND A RARITY THAT IS PRESENT AND BLANK STORES THE SAME THING. It
        // is byte for byte what an absent key stores, which is what makes it
        // the one malformed shape the drop count could not see.
        Json::Value blankRarity;
        blankRarity["id"]     = "eso:y";
        blankRarity["name"]   = "Y";
        blankRarity["hex"]    = "010203";
        blankRarity["rarity"] = "";
        CHECK(DyePalette::DyeFromJson(blankRarity, 100, d));
        CHECK(d.rarity.empty());
    }

    {  // ⚠ A DROPPED RARITY IS COUNTED, because dropping it is the same thing
       // as making the dye FREE. The rules resolve an empty rarity to no tier,
       // and no tier means no requirement, so a pack that wrote "rarity": 7
       // hands its colours out at level 1. Keeping the colour is still the
       // right trade; being quiet about it is not.
       //
       // ⚠ ABSENT IS NOT MALFORMED, and only the malformed one is counted.
       // vanilla.json's twelve dyes carry no "rarity" at all and are
       // deliberately free, so counting those would put a warning in every
       // player's log about the shipped file working as designed.
        Json::Value root;
        Json::Value typed;
        typed["id"]     = "eso:a";
        typed["name"]   = "A";
        typed["hex"]    = "010203";
        typed["rarity"] = "Rare";
        Json::Value dropped;
        dropped["id"]     = "eso:b";
        dropped["name"]   = "B";
        dropped["hex"]    = "040506";
        dropped["rarity"] = 7;  // present and the wrong type: counted
        Json::Value absent;
        absent["id"]   = "vanilla:c";
        absent["name"] = "C";
        absent["hex"]  = "070809";  // no rarity key at all: NOT counted
        // ⚠ PRESENT AND BLANK IS COUNTED TOO, and it was the one shape that
        // slipped through. The check asked !isString(), and "" IS a string, so
        // a key that was typed and left empty parsed, stored the same empty
        // rarity an absent key stores, and was reported by nobody. Wrote the
        // key and left it blank is the same author error as wrote the key and
        // got the type wrong, and both hand the colour out at level 1.
        Json::Value blank;
        blank["id"]     = "eso:e";
        blank["name"]   = "E";
        blank["hex"]    = "0A0B0C";
        blank["rarity"] = "";
        root["dyes"].append(typed);
        root["dyes"].append(dropped);
        root["dyes"].append(absent);
        root["dyes"].append(blank);

        std::vector<Dye> out;
        const auto       tally = DyePalette::DyesFromJson(root, 100, out);
        CHECK(tally.rejected == 0);
        CHECK(tally.raritiesDropped == 2);
        CHECK(out.size() == 4);  // the colour survives losing its tier
        CHECK(out[1].rarity.empty());
        CHECK(out[2].rarity.empty());  // absent: empty, and NOT counted
        CHECK(out[3].rarity.empty());
    }

    {  // counted only where the ENTRY survived. A malformed rarity on a dye
       // that was rejected outright costs nothing, because a dye that never
       // loaded cannot be free, and counting it would warn about a colour that
       // is not there.
        Json::Value root;
        Json::Value doomed;
        doomed["id"]     = "eso:d";
        doomed["name"]   = "D";
        doomed["hex"]    = "zzzzzz";  // rejected: no colour, so no tier to lose
        doomed["rarity"] = Json::Value(Json::arrayValue);
        root["dyes"].append(doomed);

        std::vector<Dye> out;
        const auto       tally = DyePalette::DyesFromJson(root, 100, out);
        CHECK(tally.rejected == 1);
        CHECK(tally.raritiesDropped == 0);
        CHECK(out.empty());
    }

    {  // FindIn: the pure matching rule, split out of Find so it is testable
       // without reaching the locked store (the same idiom DyeGate.h uses:
       // split the decision from the engine walk around it, and the same
       // shape as MergeDye, a pure function over a caller-supplied vector).
        std::vector<Dye> dyes = {
            Dye{ "pack:a", "A", DyeChannel{ true, 1, 2, 3 }, 10 },
            Dye{ "pack:b", "B", DyeChannel{ true, 4, 5, 6 }, 20 },
        };

        const auto hit = DyePalette::FindIn(dyes, "pack:b");
        CHECK(hit.has_value());
        CHECK(hit->id == "pack:b");
        CHECK(hit->name == "B");

        CHECK(!DyePalette::FindIn(dyes, "pack:missing").has_value());

        // Ids are opaque text compared exactly ("pack:name" convention): case
        // is part of the identity, not decoration, in both directions.
        CHECK(!DyePalette::FindIn(dyes, "pack:B").has_value());
        CHECK(!DyePalette::FindIn(dyes, "PACK:a").has_value());

        // A duplicate id should never reach the store (MergeDye is what
        // refuses it on the way in), but if a caller-built list holds one
        // anyway, the first entry is authoritative, the same rule MergeDye
        // enforces at the merge point.
        std::vector<Dye> dup = {
            Dye{ "pack:x", "First", DyeChannel{ true, 1, 1, 1 }, 10 },
            Dye{ "pack:x", "Second", DyeChannel{ true, 2, 2, 2 }, 20 },
        };
        const auto dupHit = DyePalette::FindIn(dup, "pack:x");
        CHECK(dupHit.has_value());
        CHECK(dupHit->name == "First");
    }

    {  // Find: the lock-and-copy wrapper around FindIn. This test binary
       // never calls Load(), so g_dyes stays empty for the whole run and
       // every id misses here; FindIn above is what tests the matching rule
       // itself, against a store this file can actually seed.
        CHECK(!DyePalette::Find("vanilla:ebony").has_value());
        CHECK(!DyePalette::Find("").has_value());
    }

    {  // a file's entries load in order, and a bad entry does not take the file
       // down with it
        Json::Value root;
        Json::Value good;
        good["id"]   = "vanilla:a";
        good["name"] = "A";
        good["hex"]  = "010203";
        Json::Value bad;
        bad["id"]   = "vanilla:b";
        bad["name"] = "B";
        bad["hex"]  = "zzzzzz";        // malformed: this ENTRY is dropped
        Json::Value alsoGood;
        alsoGood["id"]   = "vanilla:c";
        alsoGood["name"] = "C";
        alsoGood["hex"]  = "040506";
        root["dyes"].append(good);
        root["dyes"].append(bad);
        root["dyes"].append(alsoGood);

        std::vector<Dye> out;
        CHECK(DyePalette::DyesFromJson(root, 100, out).rejected == 1);  // one rejected
        CHECK(out.size() == 2);        // the bad one dropped, the others kept
        CHECK(out[0].id == "vanilla:a");
        CHECK(out[1].id == "vanilla:c");
    }

    {  // a root that is not a JSON object must be dropped whole, not indexed.
       // jsoncpp's operator[] only tolerates object or null on the left; an
       // array or a string root is neither, and indexing it throws
       // Json::LogicError instead of politely returning a null member. A
       // pack author shipping a bare array, [ { "id": ... } ], is a natural
       // and common JSON shape, and a parseable file with the wrong root
       // shape must be skipped like any other malformed pack, never crash
       // the game at kDataLoaded.
        Json::Value arrayRoot(Json::arrayValue);
        Json::Value entry;
        entry["id"]   = "vanilla:f";
        entry["name"] = "F";
        entry["hex"]  = "0D0E0F";
        arrayRoot.append(entry);
        std::vector<Dye> out;
        // 0 rejected, not 1: there was no "dyes" array to have counted
        // entries from, the whole root was refused before that point.
        CHECK(DyePalette::DyesFromJson(arrayRoot, 100, out).rejected == 0);
        CHECK(out.empty());

        Json::Value stringRoot("just a scalar, not an object");
        CHECK(DyePalette::DyesFromJson(stringRoot, 100, out).rejected == 0);
        CHECK(out.empty());
    }

    {  // an object root with no "dyes" key, or "dyes" present but not an
       // array, is empty rather than a crash or a guess
        Json::Value noKey;
        noKey["unrelated"] = "field";
        std::vector<Dye> out;
        CHECK(DyePalette::DyesFromJson(noKey, 100, out).rejected == 0);
        CHECK(out.empty());

        Json::Value wrongShape;
        wrongShape["dyes"] = "not an array";
        CHECK(DyePalette::DyesFromJson(wrongShape, 100, out).rejected == 0);
        CHECK(out.empty());
    }

    {  // "dyes" holding bare strings and numbers: each array ENTRY reaches
       // DyeFromJson one at a time, which drops anything that is not an
       // object rather than crash reaching for ["id"]. This is the case
       // the comment on the DyeFromJson root-object test above promised
       // would be covered once DyesFromJson existed to hand entries over.
        Json::Value root;
        root["dyes"].append(Json::Value("just a string"));
        root["dyes"].append(Json::Value(42));
        Json::Value good;
        good["id"]   = "vanilla:e";
        good["name"] = "E";
        good["hex"]  = "0A0B0C";
        root["dyes"].append(good);

        std::vector<Dye> out;
        // Both the bare string and the bare number are entries that reached
        // DyeFromJson and failed its isObject() check, so both count.
        CHECK(DyePalette::DyesFromJson(root, 100, out).rejected == 2);
        CHECK(out.size() == 1);
        CHECK(out[0].id == "vanilla:e");
    }

    {  // DyesFromJson APPENDS rather than clears: Load relies on this to
       // fold one file's entries onto what earlier files already
       // contributed, so a pre-seeded vector must come back with its
       // existing entry untouched and the new one added after it.
        Json::Value root;
        Json::Value only;
        only["id"]   = "vanilla:d";
        only["name"] = "D";
        only["hex"]  = "070809";
        root["dyes"].append(only);

        std::vector<Dye> out;
        out.push_back(Dye{ "seed:pre", "Pre-existing", DyeChannel{ true, 9, 9, 9 }, 5 });
        CHECK(DyePalette::DyesFromJson(root, 100, out).rejected == 0);  // nothing rejected
        CHECK(out.size() == 2);
        CHECK(out[0].id == "seed:pre");   // untouched
        CHECK(out[1].id == "vanilla:d");  // appended after it
    }

    {  // an id claimed twice: the FIRST wins, deterministically, and the loser is
       // reported so a pack conflict is not silent
        std::vector<Dye> merged;
        Dye first{ "pack:x", "First", DyeChannel{ true, 1, 1, 1 }, 10 };
        Dye second{ "pack:x", "Second", DyeChannel{ true, 2, 2, 2 }, 20 };
        CHECK(DyePalette::MergeDye(merged, first));
        CHECK(!DyePalette::MergeDye(merged, second));  // refused: id already taken
        CHECK(merged.size() == 1);
        CHECK(merged[0].name == "First");
    }

    {  // two DIFFERENT ids both succeed and both land, in order: only the
       // collision path was covered before this
        std::vector<Dye> merged;
        Dye a{ "pack:a", "A", DyeChannel{ true, 1, 1, 1 }, 10 };
        Dye b{ "pack:b", "B", DyeChannel{ true, 2, 2, 2 }, 20 };
        CHECK(DyePalette::MergeDye(merged, a));
        CHECK(DyePalette::MergeDye(merged, b));
        CHECK(merged.size() == 2);
        CHECK(merged[0].id == "pack:a");
        CHECK(merged[1].id == "pack:b");
    }

    // ---- the special-dye keys: hex2, mode, gloss and sheen ----------------

    {  // ⚠ EVERY EXISTING ENTRY PARSES UNCHANGED. All 318 shipped dyes across
       // eso.json and vanilla.json carry none of the new keys, and a parser that
       // quietly changed their meaning would repaint the whole palette.
        Json::Value j;
        j["id"]     = "eso:aspect-red";
        j["name"]   = "Aspect Red";
        j["hex"]    = "8D6563";
        j["rarity"] = "Common";
        Dye d{};
        CHECK(DyePalette::DyeFromJson(j, 0, d));
        CHECK(d.colour.mode == 0);
        CHECK(!d.colour.secondSet);
        CHECK(d.colour.r2 == 0 && d.colour.g2 == 0 && d.colour.b2 == 0);
        CHECK(!d.colour.palette.sheenSet);
        CHECK(!d.colour.palette.glossSet);
        // And the finish block is left entirely alone rather than zeroed, which
        // is the difference between "no gloss" and "gloss 0". kDyeNeutral is what
        // means no change; 0 would flatten every highlight it touched.
        CHECK(d.colour.palette.gloss == kDyeNeutral);
    }

    {  // hex2 and mode together, which is what a pearl actually is
        Json::Value j;
        j["id"]   = "fr:abyssal-pearl";
        j["name"] = "Abyssal Pearl";
        j["hex"]  = "8FA7C4";
        j["hex2"] = "C9A0D8";
        j["mode"] = "iridescent";
        Dye d{};
        CHECK(DyePalette::DyeFromJson(j, 0, d));
        CHECK(d.colour.r == 0x8F && d.colour.g == 0xA7 && d.colour.b == 0xC4);
        CHECK(d.colour.secondSet);
        CHECK(d.colour.r2 == 0xC9 && d.colour.g2 == 0xA0 && d.colour.b2 == 0xD8);
        CHECK(d.colour.mode == 2);
    }

    {  // mode names, and an unknown one is flat rather than a parse failure: a
       // dye pack from a later version should lose its ramp, not its colour.
        auto modeOf = [](const char* m) {
            Json::Value j;
            j["id"]   = "x";
            j["name"] = "x";
            j["hex"]  = "808080";
            j["mode"] = m;
            Dye d{};
            DyePalette::DyeFromJson(j, 0, d);
            return d.colour.mode;
        };
        CHECK(modeOf("flat") == 0);
        CHECK(modeOf("nacre") == 1);
        CHECK(modeOf("iridescent") == 2);
        // The envmask modes (2026-09-04): the shape's own environment mask
        // draws the metal-versus-cloth line these three read.
        CHECK(modeOf("metal") == 3);
        CHECK(modeOf("cloth") == 4);
        CHECK(modeOf("twotone") == 5);
        CHECK(modeOf("holographic") == 0);
        CHECK(modeOf("") == 0);
    }

    {  // The blend key, on exactly the mode's terms. Absent means the dye
       // defers to the install's setting, which is what all 318 shipped dyes
       // do by naming no key at all.
        auto blendOf = [](const char* b) {
            Json::Value j;
            j["id"]    = "x";
            j["name"]  = "x";
            j["hex"]   = "808080";
            j["blend"] = b;
            Dye d{};
            DyePalette::DyeFromJson(j, 0, d);
            return d.colour.blend;
        };
        CHECK(blendOf("softlight") == 1);
        CHECK(blendOf("multiply") == 2);
        CHECK(blendOf("screen") == 3);
        CHECK(blendOf("overlay") == 4);
        CHECK(blendOf("colour") == 5);
        CHECK(blendOf("luminosity") == 6);
        // ⚠ AN UNKNOWN SPELLING DEFERS RATHER THAN REFUSING, so a pack written
        // against a later build loses its blend and keeps its colour.
        CHECK(blendOf("hardlight") == 0);
        CHECK(blendOf("") == 0);
        // ⚠ "color" IS THE SAME CURVE AS "colour" AND THIS ONCE ASSERTED THE
        // OPPOSITE. DyeTexture::BlendFromName has always taken both spellings,
        // so a pack author writing either gets what the settings picker calls
        // Colour. The writer still emits "colour" alone.
        CHECK(blendOf("color") == 5);
        CHECK(blendOf("colour") == 5);

        Json::Value none;
        none["id"]   = "x";
        none["name"] = "x";
        none["hex"]  = "808080";
        Dye plain{};
        CHECK(DyePalette::DyeFromJson(none, 0, plain));
        CHECK(plain.colour.blend == 0);  // absent is the deferring value

        // ⚠ AND A WRONG TYPE COSTS THE BLEND, NOT THE ENTRY, matching the mode
        // below and the rarity: a blend is metadata, and the colour is worth
        // more than it.
        Json::Value wrong;
        wrong["id"]    = "x";
        wrong["name"]  = "x";
        wrong["hex"]   = "808080";
        wrong["blend"] = 3;  // a number, not a spelling
        Dye w{};
        CHECK(DyePalette::DyeFromJson(wrong, 0, w));
        CHECK(w.colour.blend == 0);
    }

    {  // ⚠ A MODE OF THE WRONG TYPE IS FLAT, NOT A REFUSAL, matching how rarity
       // already treats a wrong type: the colour is worth more than the ramp.
        Json::Value j;
        j["id"]   = "x";
        j["name"] = "x";
        j["hex"]  = "808080";
        j["mode"] = 7;  // a number, not one of the three names
        Dye d{};
        CHECK(DyePalette::DyeFromJson(j, 0, d));
        CHECK(d.colour.mode == 0);
    }

    {  // ⚠ hex2 WITHOUT A MODE IS STILL FLAT. Two stops mean nothing without a
       // ramp to run them through, and silently promoting to nacre would make a
       // typo change how a dye looks. The stops are still STORED, so adding the
       // mode later is a one-word edit rather than a re-author.
        Json::Value j;
        j["id"]   = "x";
        j["name"] = "x";
        j["hex"]  = "808080";
        j["hex2"] = "FF0000";
        Dye d{};
        CHECK(DyePalette::DyeFromJson(j, 0, d));
        CHECK(d.colour.mode == 0);
        CHECK(d.colour.secondSet);
        CHECK(d.colour.r2 == 0xFF);
    }

    {  // A mode with no hex2 keeps the mode and sets no second stop, which paints
       // the dye's own colour: the ramp runs between it and itself.
        Json::Value j;
        j["id"]   = "x";
        j["name"] = "x";
        j["hex"]  = "808080";
        j["mode"] = "nacre";
        Dye d{};
        CHECK(DyePalette::DyeFromJson(j, 0, d));
        CHECK(d.colour.mode == 1);
        CHECK(!d.colour.secondSet);
    }

    {  // The finish fields, which the record has carried unauthored since the
       // Body Studio work: gloss and sheen finally reachable from data. This is
       // not new storage, it is the parser a dormant struct never had.
        Json::Value j;
        j["id"]    = "fr:burnished-gold";
        j["name"]  = "Burnished Gold";
        j["hex"]   = "C9A227";
        j["gloss"] = 200;
        j["sheen"] = "FFFFFF";
        Dye d{};
        CHECK(DyePalette::DyeFromJson(j, 0, d));
        CHECK(d.colour.palette.glossSet && d.colour.palette.gloss == 200);
        CHECK(d.colour.palette.sheenSet);
        CHECK(d.colour.palette.sheenR == 0xFF);
        CHECK(d.colour.palette.sheenG == 0xFF);
        CHECK(d.colour.palette.sheenB == 0xFF);
    }

    {  // ⚠ THE TWO PRESENCE BITS STAY INDEPENDENT, which is the fault DyeMaterial's
       // own header records: under one bit a gloss with no sheen wrote
       // specularColor black and put the shape's highlight out.
        Json::Value j;
        j["id"]    = "x";
        j["name"]  = "x";
        j["hex"]   = "808080";
        j["gloss"] = 200;
        Dye d{};
        CHECK(DyePalette::DyeFromJson(j, 0, d));
        CHECK(d.colour.palette.glossSet);
        CHECK(!d.colour.palette.sheenSet);

        Json::Value k;
        k["id"]    = "y";
        k["name"]  = "y";
        k["hex"]   = "808080";
        k["sheen"] = "FFF4D0";
        Dye e{};
        CHECK(DyePalette::DyeFromJson(k, 0, e));
        CHECK(!e.colour.palette.glossSet);
        CHECK(e.colour.palette.sheenSet);
    }

    {  // flake: optional, clamped like gloss, absent means none — which is every
       // dye that existed before sparkle did.
        auto flakeOf = [](int f) {
            Json::Value j;
            j["id"]    = "x";
            j["name"]  = "x";
            j["hex"]   = "808080";
            j["flake"] = f;
            Dye d{};
            DyePalette::DyeFromJson(j, 0, d);
            return static_cast<int>(d.colour.flake);
        };
        CHECK(flakeOf(200) == 200);
        CHECK(flakeOf(400) == 255);
        CHECK(flakeOf(-5) == 0);

        Json::Value j;
        j["id"]   = "x";
        j["name"] = "x";
        j["hex"]  = "808080";
        Dye d{};
        CHECK(DyePalette::DyeFromJson(j, 0, d));
        CHECK(d.colour.flake == 0);

        // Wrong type is no flake, not a refusal: same trade as mode and rarity,
        // the colour is worth more than the sparkle.
        Json::Value k;
        k["id"]    = "x";
        k["name"]  = "x";
        k["hex"]   = "808080";
        k["flake"] = "lots";
        Dye e{};
        CHECK(DyePalette::DyeFromJson(k, 0, e));
        CHECK(e.colour.flake == 0);
    }

    {  // "cut", the per-piece "Metal starts" byte (2026-09-04): optional,
       // clamped like flake, absent means 128, which is "the dye says
       // nothing" and the halfway cut every mask measured wanted.
        auto cutOf = [](int c) {
            Json::Value j;
            j["id"]   = "x";
            j["name"] = "x";
            j["hex"]  = "808080";
            j["cut"]  = c;
            Dye d{};
            DyePalette::DyeFromJson(j, 0, d);
            return static_cast<int>(d.colour.cut);
        };
        CHECK(cutOf(51) == 51);
        CHECK(cutOf(400) == 255);
        CHECK(cutOf(-5) == 0);

        Json::Value j;
        j["id"]   = "x";
        j["name"] = "x";
        j["hex"]  = "808080";
        Dye d{};
        CHECK(DyePalette::DyeFromJson(j, 0, d));
        CHECK(d.colour.cut == 128);

        // Wrong type says nothing, not a refusal: the colour is worth more
        // than where its metal starts.
        Json::Value k;
        k["id"]   = "x";
        k["name"] = "x";
        k["hex"]  = "808080";
        k["cut"]  = "low";
        Dye e{};
        CHECK(DyePalette::DyeFromJson(k, 0, e));
        CHECK(e.colour.cut == 128);
    }

    {  // gloss is clamped rather than wrapped, in both directions. A pack author
       // typing 400 gets the ceiling, not 144.
        auto glossOf = [](int g) {
            Json::Value j;
            j["id"]    = "x";
            j["name"]  = "x";
            j["hex"]   = "808080";
            j["gloss"] = g;
            Dye d{};
            DyePalette::DyeFromJson(j, 0, d);
            return static_cast<int>(d.colour.palette.gloss);
        };
        CHECK(glossOf(400) == 255);
        CHECK(glossOf(-5) == 0);
        CHECK(glossOf(0) == 0);
        CHECK(glossOf(255) == 255);
    }

    {  // A malformed hex2 is refused the way a malformed hex already is, rather
       // than parsed to black. An imported pack is untrusted and a half-read
       // colour would be applied to the player's gear.
        Json::Value j;
        j["id"]   = "x";
        j["name"] = "x";
        j["hex"]  = "808080";
        j["hex2"] = "nope";
        j["mode"] = "nacre";
        Dye d{};
        CHECK(!DyePalette::DyeFromJson(j, 0, d));
    }

    {  // And so is a malformed sheen, for the same reason: it is a colour.
        Json::Value j;
        j["id"]    = "x";
        j["name"]  = "x";
        j["hex"]   = "808080";
        j["sheen"] = "12345";  // five digits, not six
        Dye d{};
        CHECK(!DyePalette::DyeFromJson(j, 0, d));
    }

    {  // The strict six-digit rule is the SHARED one, so hex2 refuses everything
       // hex refuses: a leading hash, a three-digit shorthand, trailing text.
        for (const char* bad : { "#FF0000", "F00", "FF00000", "GG0000", "" }) {
            Json::Value j;
            j["id"]   = "x";
            j["name"] = "x";
            j["hex"]  = "808080";
            j["hex2"] = bad;
            Dye d{};
            CHECK(!DyePalette::DyeFromJson(j, 0, d));
        }
    }

    // ---- which dye is this channel wearing (OS-201) ----------------------
    //
    // The match is ApplyPaletteDye run forwards, so these tests are really
    // asking whether the merge and the recognition still agree. A field added
    // to the merge and not to the match would fail the round trip below
    // without anyone editing this file, which is the property being bought.
    {
        auto make = [](const char* id, std::uint8_t r, std::uint8_t g,
                       std::uint8_t b) {
            Dye d{};
            d.id       = id;
            d.name     = id;
            d.colour.set = true;
            d.colour.r = r;
            d.colour.g = g;
            d.colour.b = b;
            return d;
        };
        std::vector<Dye> pal{ make("p:red", 0xFF, 0, 0), make("p:green", 0, 0xFF, 0) };

        {  // an unset channel is nobody's dye, and says so as 0 matches
            DyeChannel ch{};
            const auto m = DyePalette::FindAppliedIn(pal, ch);
            CHECK(m.matches == 0);
            CHECK(!m.dye.has_value());
        }

        {  // the round trip: paint it, then recognise it
            const auto ch = ApplyPaletteDye(DyeChannel{}, pal[0].colour);
            const auto m  = DyePalette::FindAppliedIn(pal, ch);
            CHECK(m.matches == 1);
            CHECK(m.dye.has_value() && m.dye->id == "p:red");
        }

        {  // ⚠ THE PLAYER'S OWN FIELDS MUST NOT LOSE THE NAME. Weakening a
           // colour or overriding its finish is still wearing that dye, and
           // this is the case a hand-written field list would have got wrong.
            auto ch     = ApplyPaletteDye(DyeChannel{}, pal[0].colour);
            ch.strength = 40;
            ch.player.glossSet = true;
            ch.player.gloss    = 200;
            const auto m = DyePalette::FindAppliedIn(pal, ch);
            CHECK(m.matches == 1);
            CHECK(m.dye.has_value() && m.dye->id == "p:red");
        }

        {  // a colour the player mixed themselves belongs to no dye
            DyeChannel ch{};
            ch.set = true;
            ch.r = 0x12; ch.g = 0x34; ch.b = 0x56;
            const auto m = DyePalette::FindAppliedIn(pal, ch);
            CHECK(m.matches == 0);
            CHECK(!m.dye.has_value());
        }

        {  // ⚠ A FINISH IS PART OF THE IDENTITY. Same rgb, different ramp, so
           // the flat red dye must NOT claim a channel carrying a pearl.
            auto ch  = ApplyPaletteDye(DyeChannel{}, pal[0].colour);
            ch.mode  = 2;
            const auto m = DyePalette::FindAppliedIn(pal, ch);
            CHECK(m.matches == 0);
        }

        {  // ⚠ A TIE IS REFUSED RATHER THAN BROKEN BY FILE ORDER, and the count
           // is what separates it from "no palette dye made this".
            auto tie = pal;
            tie.push_back(make("other:red", 0xFF, 0, 0));
            const auto ch = ApplyPaletteDye(DyeChannel{}, pal[0].colour);
            const auto m  = DyePalette::FindAppliedIn(tie, ch);
            CHECK(m.matches == 2);
            CHECK(!m.dye.has_value());
        }
    }

    // ---- ordering the grid by hue (OS-202) --------------------------------
    {
        auto key = [](std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            DyeChannel c{};
            c.set = true;
            c.r = r; c.g = g; c.b = b;
            return DyePalette::HueKeyOf(c);
        };

        {  // ⚠ EVERY GREY IS NEUTRAL, INCLUDING THE TWO ENDS. Black and white
           // are the ones a tolerance-based check gets right by accident and a
           // hue calculation gets wrong on purpose.
            for (int v : { 0x00, 0x0F, 0x80, 0xC0, 0xFF }) {
                const auto k = key(static_cast<std::uint8_t>(v),
                                   static_cast<std::uint8_t>(v),
                                   static_cast<std::uint8_t>(v));
                CHECK(k.neutral);
                CHECK(k.hue == 0);
                CHECK(k.saturation == 0);
                CHECK(k.value == v);
            }
        }

        {  // ⚠ A DESATURATED COLOUR IS STILL A COLOUR. One byte apart is not
           // grey, and dragging it into the grey run is the failure the zero
           // test in the .cpp is written against.
            const auto k = key(0x81, 0x80, 0x80);
            CHECK(!k.neutral);
        }

        {  // the primaries land in wheel order and the secondaries between them
            CHECK(key(0xFF, 0, 0).hue == 0);
            CHECK(key(0, 0xFF, 0).hue == 85);
            CHECK(key(0, 0, 0xFF).hue == 171);
            const auto yellow  = key(0xFF, 0xFF, 0);
            const auto cyan    = key(0, 0xFF, 0xFF);
            const auto magenta = key(0xFF, 0, 0xFF);
            CHECK(yellow.hue > 0 && yellow.hue < 85);
            CHECK(cyan.hue > 85 && cyan.hue < 171);
            CHECK(magenta.hue > 171);
        }

        {  // ⚠ THE WEDGE BELOW RED WRAPS TO THE TOP RATHER THAN GOING NEGATIVE.
           // A hue just under red is the one case the sixth arithmetic can
           // produce a negative, and a byte cast would have folded it onto a
           // green.
            const auto k = key(0xFF, 0x00, 0x20);
            CHECK(!k.neutral);
            CHECK(k.hue > 200);
        }

        {  // full saturation reads as full, and half as roughly half
            CHECK(key(0xFF, 0, 0).saturation == 255);
            const auto half = key(0xFF, 0x80, 0x80);
            CHECK(half.saturation > 100 && half.saturation < 155);
        }

        {  // the ordering itself: colours ahead of greys, wheel order within
           // the colours, and light before dark inside the grey run
            const auto red   = key(0xFF, 0, 0);
            const auto green = key(0, 0xFF, 0);
            const auto white = key(0xFF, 0xFF, 0xFF);
            const auto black = key(0, 0, 0);
            CHECK(DyePalette::HueKeyLess(red, green));
            CHECK(!DyePalette::HueKeyLess(green, red));
            CHECK(DyePalette::HueKeyLess(red, white));
            CHECK(DyePalette::HueKeyLess(green, black));
            CHECK(DyePalette::HueKeyLess(white, black));
            CHECK(!DyePalette::HueKeyLess(black, white));
            // irreflexive, which is what makes it a strict weak ordering and
            // keeps std::stable_sort out of undefined behaviour
            CHECK(!DyePalette::HueKeyLess(red, red));
            CHECK(!DyePalette::HueKeyLess(white, white));
        }

        {  // within one hue, the strong one leads
            const auto strong = key(0xFF, 0x00, 0x00);
            const auto washed = key(0xFF, 0x80, 0x80);
            CHECK(strong.hue == washed.hue);
            CHECK(DyePalette::HueKeyLess(strong, washed));
        }
    }

    if (g_failures == 0) {
        std::printf("DyePaletteTests: all passed\n");
        return 0;
    }
    std::printf("DyePaletteTests: %d failure(s)\n", g_failures);
    return 1;
}
