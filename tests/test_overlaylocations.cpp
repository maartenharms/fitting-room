// Which overlay art belongs on which location. No SKSE and no engine: the rule
// is three tiers of pure lookup, and the third tier is the one worth pinning.
//
// ⚠⚠ THE FALLBACK IS THE FEATURE. RaceMenu's own lists are authored per pack in
// Papyrus, so any art whose pack ships no registration script is unplaceable by
// anything but its name, and art whose name says nothing has to be SHOWN. A
// regression here does not look like a crash, it looks like a player's library
// quietly missing two thirds of itself, which is the exact failure the page was
// warned against before it was written.
#include "OverlayLocations.h"

#include <process.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (false)

using namespace OS::OverlayLocations;
using OS::OverlayPlan::Location;

int main() {
    {  // The key the table is written under, which the generator has to match
        // exactly or every lookup misses and the whole filter reads as "no pack
        // ever said anything".
        CHECK(Key("Actors\\Character\\Overlays\\CO 3\\52 Body F.dds") ==
              "actors\\character\\overlays\\co 3\\52 body f.dds");
        CHECK(Key("Actors/Character/Overlays/A.dds") == "actors\\character\\overlays\\a.dds");
        CHECK(Key("\\Actors\\A.dds") == "actors\\a.dds");
        // ⚠ THE textures\ LEVEL COMES OFF. A path that keeps it resolves to
        // Data\textures\textures\... in game and the table would be keyed on a
        // path nothing can ever look up.
        CHECK(Key("textures\\Actors\\A.dds") == "actors\\a.dds");
        CHECK(Key("Textures\\Actors\\A.dds") == "actors\\a.dds");
    }

    {  // The generator's vocabulary.
        CHECK(FromLists("body") == Bit(Location::kBody));
        CHECK(FromLists("body,feet") == (Bit(Location::kBody) | Bit(Location::kFeet)));
        CHECK(FromLists("body, feet") == (Bit(Location::kBody) | Bit(Location::kFeet)));
        CHECK(FromLists("face,feet,hands,body") == kAll);
        CHECK(FromLists("") == kNone);

        // ⚠⚠ A WARPAINT IS NOT A LOCATION, and this is the check that keeps it
        // that way. Shep's two collections register all 264 of their body
        // tattoos as warpaints, so reading "warp" as Face would hide every one
        // of them from the location they are painted for. It places the texture
        // on NO location, which drops it to the name rule and then to being
        // shown everywhere.
        //
        // ⚠ IT IS RECORDED NOW RATHER THAN DROPPED, and these checks are
        // written as the invariant rather than as a literal so the two cannot be
        // confused again: the bit exists so the Makeup section can find its
        // 1062 textures, and it must never widen what the overlays picker
        // offers. Location bits first, then the bit itself.
        CHECK(!HasLocation(FromLists("warp")));
        CHECK(IsWarpaint(FromLists("warp")));
        CHECK(IsWarpaint(FromLists("warpaint")));
        CHECK(LocationBits(FromLists("warp,body")) == Bit(Location::kBody));
        CHECK(IsWarpaint(FromLists("warp,body")));
        CHECK(!IsWarpaint(FromLists("body")));
        CHECK(FromLists("something-a-newer-generator-writes") == kNone);

        // ⚠ THE WARPAINT BIT IS OUTSIDE kAll, so nothing that reasons about
        // locations can ever pick it up by accident.
        CHECK((kWarpaint & kAll) == 0);
    }

    {  // The name rule, second tier. Measured names from the installed packs.
        CHECK(FromPathTokens("actors\\character\\overlays\\co 3\\52 Body F.dds") ==
              Bit(Location::kBody));
        CHECK(FromPathTokens("actors\\character\\overlays\\co 3\\63 Head F.dds") ==
              Bit(Location::kFace));
        CHECK(FromPathTokens("actors\\character\\overlays\\co 3\\52 Hands O.dds") ==
              Bit(Location::kHands));
        CHECK(FromPathTokens("!ube\\[spaz490]\\nail overlays\\UBE_ToeNails_Basic.dds") ==
              (Bit(Location::kHands) | Bit(Location::kFeet)));

        // ⚠ THE FILE NAME IS ASKED FIRST AND ALONE. This is the case that makes
        // it matter: the folder says Body and the file says Head, and a single
        // search over the whole path would offer a face texture on the torso.
        CHECK(FromPathTokens("actors\\character\\overlays\\bodypaints\\63 Head F.dds") ==
              Bit(Location::kFace));

        // The folder answers when the name says nothing, which is how a pack
        // that sorts its art into directories gets placed at all.
        CHECK(FromPathTokens("actors\\character\\overlays\\sfo\\face\\01.dds") ==
              Bit(Location::kFace));
        CHECK(FromPathTokens("actors\\character\\overlays\\wnb\\hand\\01.dds") ==
              Bit(Location::kHands));

        // ⚠⚠ AND NOTHING IS THE HONEST ANSWER FOR MOST OF THE LIBRARY. Two
        // thirds of the installed textures look like this.
        CHECK(FromPathTokens("actors\\character\\overlays\\shepstattoocollection\\shep1.dds") ==
              kNone);
        CHECK(FromPathTokens("actors\\character\\overlays\\obistuff\\ow_01.dds") == kNone);
    }

    {  // The whole rule, in order.
        const auto path = "actors\\character\\overlays\\co 3\\63 Head F.dds";
        // The pack's own word wins, even against a name that disagrees.
        CHECK(Resolve(Bit(Location::kBody), path) == Bit(Location::kBody));
        // With no registration the name answers.
        CHECK(Resolve(kNone, path) == Bit(Location::kFace));

        // ⚠⚠ A WARPAINT-ONLY REGISTRATION STILL FALLS THROUGH TO THE NAME, and
        // this is the regression the warpaint bit could most easily have caused.
        // Resolve used to stop at any non-empty mask, so the moment "warp"
        // started setting one, every warpaint-only texture would have resolved
        // to a mask with no location: invisible on all four pages. Shep's 264
        // body tattoos are exactly this case.
        CHECK(Resolve(kWarpaint, path) == Bit(Location::kFace));
        CHECK(Resolve(kWarpaint, "actors\\character\\overlays\\shepstattoocollection\\"
                                 "shep1.dds") == kAll);
        // And the bit never leaks out of Resolve, whose answer is a location set.
        CHECK(!IsWarpaint(Resolve(kWarpaint, path)));
        CHECK(!IsWarpaint(Resolve(static_cast<Mask>(Bit(Location::kBody) | kWarpaint), path)));
        // ⚠⚠ AND WITH NEITHER, EVERY LOCATION. Never kNone: a mask of nothing
        // is a texture that appears on no page at all, which is the one
        // outcome this feature must not produce.
        CHECK(Resolve(kNone, "actors\\character\\overlays\\obistuff\\ow_01.dds") == kAll);
        CHECK(Resolve(kNone, "") == kAll);
    }

    {  // Allows, over every location, so a mask cannot silently mean the wrong bit.
        const auto body = Bit(Location::kBody);
        CHECK(Allows(body, Location::kBody));
        CHECK(!Allows(body, Location::kFace));
        CHECK(!Allows(body, Location::kHands));
        CHECK(!Allows(body, Location::kFeet));
        for (const auto location : { Location::kFace, Location::kBody, Location::kHands,
                                     Location::kFeet }) {
            CHECK(Allows(kAll, location));
            CHECK(!Allows(kNone, location));
        }
    }

    {  // The mask suffix, before any table is loaded: pure name work.
        CHECK(MaskTwinOf("actors\\character\\overlays\\sfo\\face\\face freckles 4 m.dds") ==
              "actors\\character\\overlays\\sfo\\face\\face freckles 4.dds");
        CHECK(MaskTwinOf("actors\\character\\overlays\\p\\a_m.dds") ==
              "actors\\character\\overlays\\p\\a.dds");
        CHECK(MaskTwinOf("actors\\character\\overlays\\p\\a mask.dds") ==
              "actors\\character\\overlays\\p\\a.dds");
        // Case and slashes are normalised on the way in, as everywhere else.
        CHECK(MaskTwinOf("Actors/Character/Overlays/P/A M.DDS") ==
              "actors\\character\\overlays\\p\\a.dds");
        // Nothing to strip.
        CHECK(MaskTwinOf("actors\\character\\overlays\\p\\a.dds").empty());
        CHECK(MaskTwinOf("actors\\character\\overlays\\p\\album.dds").empty());
        // ⚠ A NAME THAT IS NOTHING BUT THE SUFFIX HAS NO TWIN, and must not
        // resolve to the directory itself.
        CHECK(MaskTwinOf("actors\\character\\overlays\\p\\m.dds").empty());
        CHECK(MaskTwinOf("m.dds").empty());
        CHECK(MaskTwinOf("actors\\character\\overlays\\p\\a.png").empty());
    }

    {  // A missing table is a supported state and leaves the page showing more
        // rather than less.
        const auto report = LoadFrom("no-such-file-anywhere.json");
        CHECK(!report.loaded);
        CHECK(report.placed == 0);
        CHECK(!report.diagnostic.empty());
        CHECK(!Loaded());
        CHECK(For("actors\\character\\overlays\\obistuff\\ow_01.dds") == kAll);
    }

    {  // ---- the corrections file, against a table written here -------------
        //
        // ⚠⚠ A FIXUP WIDENS AND NEVER NARROWS, and every branch of that is
        // pinned on a three row table: one warpaint-only row the fixup names
        // (corrected), one the same script ALSO placed on face (untouched: a
        // pack's own placement outranks a correction), and one from a script
        // whose fixup names no location (skipped, still warpaint-only).
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path  box = fs::temp_directory_path(ec) /
                             ("FittingRoomOverlayLocationsTests-" + std::to_string(_getpid()));
        fs::remove_all(box, ec);
        fs::create_directories(box, ec);
        CHECK(!ec);

        const std::string a = "actors\\character\\overlays\\shep\\a.dds";
        const std::string b = "actors\\character\\overlays\\shep\\b.dds";
        const std::string c = "actors\\character\\overlays\\other\\c.dds";
        // A path as JSON spells it: every backslash doubled.
        const auto j = [](const std::string& a_path) {
            std::string out;
            for (const char ch : a_path) {
                if (ch == '\\') {
                    out += '\\';
                }
                out += ch;
            }
            return out;
        };

        const fs::path table  = box / "overlay-locations.json";
        const fs::path fixups = box / "overlay-locations-fixups.json";
        const fs::path bare   = box / "overlay-locations-no-by-source.json";
        {
            // The generator's shape: by_source keys carry the rig's mod folder
            // (or an archive and a '!') in front of the script's file name.
            std::ofstream(table)
                << "{ \"by_source\": {"
                   "  \"Some Mod On This Rig\\\\Scripts\\\\Sheps_Tattoo_Collection.pex\": ["
                   "    \"" << j(a) << "\", \"" << j(b) << "\" ],"
                   "  \"Other Mod\\\\Other.bsa!other.pex\": [ \"" << j(c) << "\" ] },"
                   "  \"paints\": {"
                   "    \"" << j(a) << "\": \"warp\","
                   "    \"" << j(b) << "\": \"face,warp\","
                   "    \"" << j(c) << "\": \"warp\" } }";
            // Typed by hand: the case differs from the table's, one fixup names a
            // script no table has, one says "warp" and so places nothing.
            std::ofstream(fixups)
                << "{ \"fixups\": ["
                   "  { \"script\": \"sheps_tattoo_collection.PEX\", \"warpaint_is\": \"body\" },"
                   "  { \"script\": \"nobody.pex\", \"warpaint_is\": \"face\" },"
                   "  { \"script\": \"other.pex\", \"warpaint_is\": \"warp\" } ] }";
            std::ofstream(bare)
                << "{ \"paints\": { \"" << j(a) << "\": \"warp\" } }";
        }
        {
            const auto report = LoadFrom(table, fixups);
            CHECK(report.loaded);
            CHECK(report.entries == 3);
            CHECK(report.corrected == 1);
            CHECK(report.fixups == 1);
            CHECK(report.placed == 2);
            CHECK(report.diagnostic.find("1 corrected by 1 fixup(s)") != std::string::npos);
            // a: body added, warpaint kept, so it is on the body page AND in the
            // Makeup library, which is where RaceMenu has it.
            CHECK(HasLocation(Registered(a)));
            CHECK(Allows(Registered(a), Location::kBody));
            CHECK(!Allows(Registered(a), Location::kFace));
            CHECK(IsWarpaint(Registered(a)));
            CHECK(For(a) == Bit(Location::kBody));
            // b: the pack placed it on face itself, and the fixup left it alone.
            CHECK(Registered(b) == static_cast<Mask>(Bit(Location::kFace) | kWarpaint));
            CHECK(For(b) == Bit(Location::kFace));
            // c: a fixup that names no location is skipped, not applied as
            // nothing.
            CHECK(!HasLocation(Registered(c)));
            CHECK(IsWarpaint(Registered(c)));
            // And the Makeup library still has all three.
            CHECK(WarpaintPaths().size() == 3);
        }
        {  // The table alone: no correction, a is warpaint-only as written.
            const auto report = LoadFrom(table);
            CHECK(report.loaded);
            CHECK(report.corrected == 0);
            CHECK(report.fixups == 0);
            CHECK(report.placed == 1);
            CHECK(!HasLocation(Registered(a)));
            CHECK(IsWarpaint(Registered(a)));
        }
        {  // No fixups file: the table loads, nothing is corrected, and the line
            // says why rather than staying quiet about a file that failed to
            // deploy.
            const auto report = LoadFrom(table, box / "missing-fixups.json");
            CHECK(report.loaded);
            CHECK(report.corrected == 0);
            CHECK(report.diagnostic.find("no fixups file") != std::string::npos);
            CHECK(!HasLocation(Registered(a)));
        }
        {  // A table from before by_source existed cannot take a fixup, and the
            // line says to re-run the generator.
            const auto report = LoadFrom(bare, fixups);
            CHECK(report.loaded);
            CHECK(report.corrected == 0);
            CHECK(report.diagnostic.find("no by_source") != std::string::npos);
            CHECK(!HasLocation(Registered(a)));
        }
        {  // A fixups file that will not parse corrects nothing and hides
            // nothing.
            std::ofstream(fixups) << "{ this is not json";
            const auto report = LoadFrom(table, fixups);
            CHECK(report.loaded);
            CHECK(report.entries == 3);
            CHECK(report.corrected == 0);
            CHECK(report.diagnostic.find("did not parse") != std::string::npos);
        }
        fs::remove_all(box, ec);
    }

    {  // ---- the scrape's own vocabulary ---------------------------------
        //
        // ⚠ RaceMenu'S WIRE FORMAT, NOT OURS. Every row a pack pushes into the
        // RaceSex Menu is "name;;path", or "name;;t0|t1|..." from the Ex forms.
        {
            const auto one = PathsInEntry("Lovely Blush;;Actors\\Character\\Overlays\\a.dds");
            CHECK(one.size() == 1);
            CHECK(one.front() == "Actors\\Character\\Overlays\\a.dds");
        }
        {
            const auto many = PathsInEntry("Set;;a.dds|b.dds|c.dds");
            CHECK(many.size() == 3);
            CHECK(many.front() == "a.dds");
            CHECK(many.back() == "c.dds");
        }
        // The Ex forms pad to eight slots and the empty ones are not textures.
        CHECK(PathsInEntry("Set;;a.dds|||b.dds||||").size() == 2);
        // A name with no separator at all, and a name with nothing after it.
        //
        // ⚠⚠ NEITHER IS READ AS A BARE PATH, and this is the check that keeps
        // the narrowing safe. A scraped row OUTRANKS the shipped table, so a
        // line this reader does not understand has to add NOTHING: read as a
        // path it would key a row on a display name and place a real texture
        // nowhere.
        CHECK(PathsInEntry("just a name").empty());
        CHECK(PathsInEntry("Name;;").empty());
        CHECK(PathsInEntry("").empty());

        // ⚠ MaskToLists AND FromLists ARE ONE PAIR. The scrape writes with the
        // first and this loader reads with the second, so a word spelt
        // differently on the two sides is a row that reads as kNone and hides a
        // texture. Round tripped rather than pinned to a literal.
        for (const Mask mask : { kNone, kAll, kWarpaint,
                                 static_cast<Mask>(Bit(Location::kBody) | kWarpaint),
                                 static_cast<Mask>(Bit(Location::kFeet) | Bit(Location::kFace)),
                                 static_cast<Mask>(kAll | kWarpaint) }) {
            CHECK(FromLists(MaskToLists(mask)) == mask);
        }
        CHECK(MaskToLists(kNone).empty());
        CHECK(MaskToLists(Bit(Location::kBody)) == "body");
        CHECK(MaskToLists(kWarpaint) == "warp");
    }

    {  // ---- the scrape layered over the shipped table ---------------------
        //
        // ⚠⚠ THE ONE LAYER IN THIS FILE THAT MAY NARROW. Everything else widens
        // by design. The scrape is what RaceMenu itself listed on THIS rig, so
        // it outranks the shipped snapshot of somebody else's, including when it
        // says less. These four rows are the whole rule: one added, one
        // overridden wider, one overridden NARROWER, one the scrape never
        // mentioned and must therefore leave alone.
        namespace fs = std::filesystem;
        std::error_code ec;
        const auto      box = fs::temp_directory_path() /
                         ("fr-ovl-scrape-" + std::to_string(_getpid()));
        fs::remove_all(box, ec);
        fs::create_directories(box, ec);
        CHECK(!ec);

        const std::string added   = "actors\\character\\overlays\\newpack\\n.dds";
        const std::string wider   = "actors\\character\\overlays\\shep\\a.dds";
        const std::string narrow  = "actors\\character\\overlays\\lovely\\l.dds";
        const std::string untouch = "actors\\character\\overlays\\co3\\52 body f.dds";
        const auto        j       = [](const std::string& a_path) {
            std::string out;
            for (const char ch : a_path) {
                if (ch == '\\') {
                    out += '\\';
                }
                out += ch;
            }
            return out;
        };

        const fs::path table   = box / "overlay-locations.json";
        const fs::path scraped = box / "overlay-locations-scraped.json";
        const fs::path fixups  = box / "overlay-locations-fixups.json";
        std::ofstream(table)
            << "{ \"by_source\": {"
               "  \"Rig\\\\Scripts\\\\Sheps_Tattoo_Collection.pex\": [ \"" << j(wider) << "\" ] },"
               "  \"paints\": {"
               "    \"" << j(wider) << "\": \"warp\","
               "    \"" << j(narrow) << "\": \"face,warp\","
               "    \"" << j(untouch) << "\": \"body\" } }";
        std::ofstream(fixups)
            << "{ \"fixups\": [ { \"script\": \"sheps_tattoo_collection.pex\", "
               "\"warpaint_is\": \"body\" } ] }";
        std::ofstream(scraped)
            << "{ \"scraped\": { \"rows\": 3 }, \"paints\": {"
               "    \"" << j(added) << "\": \"warp\","
               "    \"" << j(wider) << "\": \"body,warp\","
               "    \"" << j(narrow) << "\": \"warp\" } }";

        {
            const auto report = LoadFrom(table, scraped, fixups);
            CHECK(report.loaded);
            CHECK(report.entries == 4);  // three shipped plus the one the scrape added
            CHECK(report.scraped == 3);
            CHECK(report.scrapedAdded == 1);
            CHECK(report.scrapedOverrode == 2);
            CHECK(report.diagnostic.find("1 the table had never heard of") != std::string::npos);
            CHECK(report.diagnostic.find("2 it disagreed with") != std::string::npos);

            // Added: a pack the shipped table has never heard of stops being
            // shown everywhere, which is the field symptom this exists for.
            CHECK(Known(added));
            CHECK(IsWarpaint(Registered(added)));
            CHECK(!HasLocation(Registered(added)));

            // ⚠⚠ NARROWED ON PURPOSE. The shipped table had this on face; this
            // rig's RaceMenu lists it only under war paint, so it is makeup here
            // and the overlays picker keeps it out. Lovely Makeup is the pack
            // the field named.
            CHECK(!HasLocation(Registered(narrow)));
            CHECK(IsWarpaint(Registered(narrow)));

            // ⚠ AND THE FIXUP STILL RUNS AFTER THE SCRAPE. Shep's row is
            // warpaint-only in RaceMenu too, so the scrape agreeing does not
            // take the body page away; here the scrape placed it itself, which
            // the fixup then leaves alone because it never touches a row that
            // already has a location.
            CHECK(Allows(Registered(wider), Location::kBody));
            CHECK(For(wider) == Bit(Location::kBody));

            // ⚠⚠ A KEY THE SCRAPE NEVER MENTIONED IS UNTOUCHED, which is what
            // makes a partial capture safe: it can only under-add and can never
            // hide art the shipped table placed.
            CHECK(Allows(Registered(untouch), Location::kBody));
            CHECK(!Allows(Registered(untouch), Location::kFace));
        }
        {  // ⚠⚠ A SCRAPED ROW THAT IS NOT TEXT MUST NOT END THE PROCESS.
           // jsoncpp THROWS on asString() over a non-string and on
           // getMemberNames() over a non-object, and this file is written per
           // rig, so its contents vary with the player's load order in a way
           // the shipped table's never do. An uncaught throw here is a CRT
           // fail-fast, and a fail-fast is a crash Crash Logger and Trainwreck
           // cannot see, because it skips the filter they hook. That is the
           // shape of "clicking the overlays section crashes my game, with no
           // crash log", reported twice in August 2026. The unreadable rows are
           // skipped and counted; the readable ones still land.
            const fs::path junk = box / "overlay-locations-scraped-junk.json";
            std::ofstream(junk) << "{ \"paints\": {"
                                   "    \"" << j(added) << "\": \"warp\","
                                   "    \"" << j(narrow) << "\": 42,"
                                   "    \"" << j(untouch) << "\": [ \"body\" ] } }";
            const auto report = LoadFrom(table, junk, fixups);
            CHECK(report.loaded);
            CHECK(report.scraped == 1);         // the one string row
            CHECK(report.scrapedSkipped == 2);  // the number and the array
            CHECK(report.diagnostic.find("were not text and were skipped") !=
                  std::string::npos);
            // The readable row still did its job.
            CHECK(Known(added));
            // And the rows we could not read left the shipped answers standing
            // rather than taking the page down with them.
            CHECK(Allows(Registered(narrow), Location::kFace));
            CHECK(Allows(Registered(untouch), Location::kBody));
        }
        {  // ⚠ AND A paints THAT IS NOT AN OBJECT AT ALL, which getMemberNames()
           // would also throw on. This one was ALREADY safe and the test says so
           // on purpose: ReadPaints refuses any file whose paints is not an
           // object, so the scrape stands down before the loop is reached. Pinned
           // because that guarantee is what makes a guard at the loop dead code,
           // and if ReadPaints ever loosens, this fails rather than the field.
            const fs::path notobj = box / "overlay-locations-scraped-notobj.json";
            std::ofstream(notobj) << "{ \"paints\": \"this is not an object\" }";
            const auto report = LoadFrom(table, notobj, fixups);
            CHECK(report.loaded);
            CHECK(report.scraped == 0);
            CHECK(report.entries == 3);
            CHECK(report.diagnostic.find("has no paints object") != std::string::npos);
            CHECK(Allows(Registered(narrow), Location::kFace));
        }
        {  // No scrape yet is the normal state and says so rather than warning.
            const auto report = LoadFrom(table, box / "no-scrape.json", fixups);
            CHECK(report.loaded);
            CHECK(report.scraped == 0);
            CHECK(report.entries == 3);
            CHECK(report.diagnostic.find("No scrape yet") != std::string::npos);
            // Without the scrape the shipped table's face answer stands.
            CHECK(Allows(Registered(narrow), Location::kFace));
            // And the fixup does its old job on Shep's warpaint-only row.
            CHECK(Allows(Registered(wider), Location::kBody));
        }
        {  // ⚠ A BROKEN SCRAPE FALLS BACK TO THE TABLE RATHER THAN EMPTYING
            // THE PAGE, the same answer a broken table and a broken fixups file
            // give. The file is written by this build, so it is worth a word.
            std::ofstream(scraped) << "{ this is not json";
            const auto report = LoadFrom(table, scraped, fixups);
            CHECK(report.loaded);
            CHECK(report.entries == 3);
            CHECK(report.scraped == 0);
            CHECK(report.diagnostic.find("did not parse") != std::string::npos);
            CHECK(Allows(Registered(narrow), Location::kFace));
        }
        fs::remove_all(box, ec);
    }

#ifdef FR_DIST_DIR
    {  // ⚠⚠ THE SHIPPED TABLE, RUN THROUGH THE REAL LOADER. The file is
        // generated by a PowerShell script that nothing else checks, so this is
        // the only place its format and this reader are held together. A
        // generator change that breaks the key shape shows up here rather than
        // as a picker that silently stopped filtering.
        const std::filesystem::path file =
            std::filesystem::path{ FR_DIST_DIR } / "Overlays" / "overlay-locations.json";
        const auto report = LoadFrom(file);
        CHECK(report.loaded);
        CHECK(Loaded());
        // Pinned low enough to survive a load order change and high enough that
        // an empty or half written file cannot pass.
        CHECK(report.entries > 1000);
        CHECK(report.placed > 400);

        // Community Overlays 3 registers per location, and it is the pack the
        // field complaint was made against: its body art must not be offered
        // for a face slot.
        const auto body =
            For("Actors\\Character\\Overlays\\Community Overlays\\CO 3\\52 Body F.dds");
        CHECK(Allows(body, Location::kBody));
        CHECK(!Allows(body, Location::kFace));

        const auto head =
            For("Actors\\Character\\Overlays\\Community Overlays\\CO 3\\63 Head F.dds");
        CHECK(Allows(head, Location::kFace));
        CHECK(!Allows(head, Location::kBody));

        // ⚠ CASE AND SLASHES DO NOT MATTER TO A LOOKUP, because the picker
        // hands over whatever the disk scan found and the generator wrote
        // whatever the script held.
        CHECK(For("actors/character/overlays/community overlays/co 3/52 body f.dds") == body);

        // Shep's body tattoos are registered as warpaints, so the table places
        // them nowhere, their names say nothing, and they stay visible.
        CHECK(For("Actors\\Character\\Overlays\\ShepsTattooCollection\\shep1.dds") == kAll);

        // ---- the mask twins, against the real registrations --------------
        //
        // ⚠⚠ THE TWO HALVES OF THIS BLOCK ARE THE WHOLE RULE. The same " m"
        // means mask in one pack and MALE in another, and only the packs' own
        // registrations tell them apart. Getting this backwards deletes half
        // of Community Overlays 3 from the picker.

        // SFO ships the art and its tint mask side by side. The mask is
        // registered as a warpaint, the art as a face overlay, so the mask is
        // the redundant one.
        {
            const std::string mask = "actors\\character\\overlays\\sfo\\face\\"
                                     "face freckles 4 m.dds";
            const auto        twin = MaskTwinOf(mask);
            CHECK(twin == "actors\\character\\overlays\\sfo\\face\\face freckles 4.dds");
            CHECK(Known(mask));
            CHECK(Known(twin));
            CHECK(!HasLocation(Registered(mask)));  // registered, placed nowhere
            CHECK(IsWarpaint(Registered(mask)));    // and the table says why
            CHECK(HasLocation(Registered(twin)));   // real overlay art
            CHECK(MaskTwinIsRedundant(mask, twin));
        }

        // ⚠⚠ AND COMMUNITY OVERLAYS 3's " m" IS MALE. Same suffix, same shape,
        // and it must survive: it is registered for a location of its own.
        {
            const std::string male = "actors\\character\\overlays\\community overlays\\"
                                     "co 3\\52 body m.dds";
            CHECK(HasLocation(Registered(male)));
            CHECK(Allows(For(male), Location::kBody));
            CHECK(!MaskTwinIsRedundant(male, MaskTwinOf(male)));
        }

        // ---- the Makeup section's library --------------------------------
        //
        // ⚠⚠ THE 1062 WARPAINTS ARE THE WHOLE REASON THE BIT EXISTS. They are
        // the makeup packs by name (Koralina, Female Makeup Suite, LDD, Lovely
        // and Pretty, Lamenthia, Obi, SkFO) plus Shep's tattoos, and until now
        // nothing had a home for them: the overlays picker offered them only
        // because it shows everything it cannot place.
        {
            const auto warpaints = WarpaintPaths();
            CHECK(warpaints.size() > 1000);
            // Sorted, so the grid does not reshuffle between sessions.
            CHECK(std::is_sorted(warpaints.begin(), warpaints.end()));
            // Every one of them is what it says it is, and none of them is a
            // texture the overlays table placed on a location of its own.
            for (const auto& path : warpaints) {
                CHECK(IsWarpaint(Registered(path)));
            }
            // The SFO mask twin is in here, because a mask twin IS a warpaint
            // registration. The picker's own duplicate rule is what hides it,
            // and that rule is tested above rather than being folded in here.
            CHECK(std::binary_search(warpaints.begin(), warpaints.end(),
                                     std::string{ "actors\\character\\overlays\\sfo\\face\\"
                                                  "face freckles 4 m.dds" }));
        }

        // Neither half known: a pack with no script at all keeps both files.
        CHECK(!MaskTwinIsRedundant("actors\\character\\overlays\\nopack\\thing m.dds",
                                   "actors\\character\\overlays\\nopack\\thing.dds"));
        // No twin at all is not a mask, whatever the name says.
        CHECK(!MaskTwinIsRedundant("actors\\character\\overlays\\sfo\\face\\"
                                   "face freckles 4 m.dds",
                                   ""));

        // ---- the shipped fixups, through the same loader ---------------------
        //
        // ⚠⚠ THIS IS THE ROUND TRIP THE PICKER RELIES ON (OS-219). The overlays
        // picker keeps a warpaint-only registration out as makeup, mirroring
        // RaceMenu, and Shep's 264 body tattoos are warpaint-only by the pack's
        // own mistake. The fixups file is what gives them the body page back,
        // and it is keyed by script name against by_source, so a generator
        // that stops writing by_source, a fixup that misspells the script, or a
        // deploy that forgets the second file all show up HERE as Shep's
        // tattoos vanishing from every overlay page.
        {
            const std::filesystem::path fixups =
                std::filesystem::path{ FR_DIST_DIR } / "Overlays" /
                "overlay-locations-fixups.json";
            const auto fixed = LoadFrom(file, fixups);
            CHECK(fixed.loaded);
            CHECK(fixed.fixups == 2);
            // Pinned low enough to survive a load order change, high enough
            // that a fixup that matched nothing cannot pass: both collections
            // together are 264 on the reference rig.
            CHECK(fixed.corrected >= 200);
            CHECK(fixed.diagnostic.find("fixup(s)") != std::string::npos);

            const std::string shepF = "actors\\character\\overlays\\shepstattoocollection\\shep1.dds";
            const std::string shepM = "actors\\character\\overlays\\shepsmaletattoocollection\\shep1.dds";
            for (const auto& shep : { shepF, shepM }) {
                CHECK(Known(shep));
                CHECK(HasLocation(Registered(shep)));
                CHECK(Allows(Registered(shep), Location::kBody));
                CHECK(!Allows(Registered(shep), Location::kFace));
                CHECK(IsWarpaint(Registered(shep)));  // still in the Makeup library
                CHECK(For(shep) == Bit(Location::kBody));
            }

            // And the makeup packs are exactly what the picker's rule keeps out:
            // registered as a warpaint and placed nowhere. Lovely is the one the
            // field named; LDD and Koralina ride the same rule.
            for (const auto path : {
                     "actors\\character\\overlays\\(sdz21) lovely makeup\\"
                     "sdz21 lovely makeup blush cheeks full.dds",
                     "actors\\character\\overlays\\shepstattoocollection\\shep1.dds" }) {
                CHECK(Known(path));
                CHECK(IsWarpaint(Registered(path)));
            }
            CHECK(!HasLocation(Registered("actors\\character\\overlays\\(sdz21) lovely makeup\\"
                                          "sdz21 lovely makeup blush cheeks full.dds")));
        }
    }
#endif

    if (g_failures == 0) {
        std::printf("OverlayLocationsTests OK\n");
        return 0;
    }
    std::printf("OverlayLocationsTests: %d failure(s)\n", g_failures);
    return 1;
}
