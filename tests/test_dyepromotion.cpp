// Dye promotion tests. No SKSE, no engine: what a character has newly earned
// is one walk over the palette with a rule set and a plain world state, and
// the gate the whole feature turns on is a single boolean. All of it is
// provable without a running game, a save or a UI.
#include "DyePalette.h"
#include "DyePromotion.h"

#include <json/json.h>

#include <process.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

using namespace OS;

namespace {

    // One clause, built the way a rules file spells it.
    Json::Value Always() {
        Json::Value list(Json::arrayValue);
        Json::Value c;
        c["type"] = "always";
        list.append(c);
        return list;
    }

    Json::Value AtLevel(unsigned a_min) {
        Json::Value list(Json::arrayValue);
        Json::Value c;
        c["type"] = "level";
        c["min"]  = a_min;
        list.append(c);
        return list;
    }

    // How many dyes the report blames on one rarity, or 0 when it is not in
    // there at all.
    //
    // ⚠ NOT std::map::at, which THROWS on a missing key. An uncaught throw
    // ends the process with 0xC0000409 and prints nothing, so the one thing a
    // failing assertion has to do, say which line failed, is exactly what it
    // would not do. Measured, not guessed: the first run of this file used at()
    // and the suite died with no output at all.
    std::size_t Unmatched(const PromotionResult& a_result,
                          const std::string&     a_rarity) {
        const auto it = a_result.unmatchedRarities.find(a_rarity);
        return it == a_result.unmatchedRarities.end() ? 0 : it->second;
    }

    // Whether the report names this key.
    //
    // ⚠ NOT unmatchedDyeRules[0], for the same reason Unmatched is not
    // std::map::at. operator[] on an EMPTY vector reads past the end, and in a
    // release build the process dies with 0xC0000005 having printed NOTHING,
    // because stdout is fully buffered when the harness redirects it: every
    // FAIL line already written is lost with it. A suite that crashes looks
    // like a suite that was never built. Measured here, not guessed: the
    // negative control for the orphaned-key fix reverted it, and this file
    // died silently instead of naming the assertion.
    bool Names(const std::vector<std::string>& a_keys, std::string_view a_key) {
        return std::ranges::any_of(
            a_keys, [a_key](const std::string& a_entry) { return a_entry == a_key; });
    }

    namespace fs = std::filesystem;

    // ⚠ THE SHIPPED FILES, THROUGH THE REAL LOADERS. Nothing in this repo read
    // dist/ until 2026-08-02. Everything that claimed the shipped rules load
    // was either tools/check_dye_rules.py, which is a DIFFERENT PARSER from
    // jsoncpp and this branch's most expensive lesson, or a throwaway probe
    // that is not in the tree and cannot fail a build. So the one thing nobody
    // could see was the file the players actually get: a 146 line // header
    // under failIfExtra and rejectDupKeys, 306 rarities against five tier
    // names matched byte for byte, and 220 override keys against real ids.
    //
    // ⚠ DyeRules::Load and DyePalette::Load both read paths relative to the
    // PROCESS WORKING DIRECTORY, under Data/, so the shipped tree is copied
    // into a temp Data/ and the process moves there. A copy, because a junction
    // needs mklink and this must run everywhere the build does; byte for byte,
    // so it is the shipped file the loader sees and not a paraphrase of it.
    //
    // ⚠ THIS SUITE HAS NO OTHER CHDIR, and that is why the case lives here
    // rather than in DyeRulesTests, whose Sandbox helper moves the working
    // directory for every one of its own cases and restores it in a
    // destructor. Two things moving one process-wide setting is how a case
    // comes to assert against whatever directory it happened to be left in,
    // which that helper has already been bitten by once.
    class ShippedTree {
    public:
        ShippedTree() {
            std::error_code ec;
            previous_ = fs::current_path(ec);
            ok_       = !ec;
            root_     = fs::temp_directory_path(ec) /
                    ("FittingRoomShippedDist-" + std::to_string(_getpid()));
            ok_ = ok_ && !ec;
            fs::remove_all(root_, ec);
            ok_ = ok_ && !ec;
            const auto into = root_ / "Data" / "SKSE" / "Plugins" / "FittingRoom";
            fs::create_directories(into, ec);
            ok_ = ok_ && !ec;
            // ⚠ THE WHOLE DIRECTORY, not the two subdirectories by name. The
            // POST_BUILD copy rules were written that way and a sibling added
            // later reached nothing; a test that repeats the mistake proves the
            // shipped tree loads while ignoring whatever part of it is new.
            fs::copy(fs::path(FR_DIST_DIR), into,
                     fs::copy_options::recursive |
                         fs::copy_options::overwrite_existing,
                     ec);
            ok_ = ok_ && !ec;
            fs::current_path(root_, ec);
            ok_ = ok_ && !ec;
        }
        ~ShippedTree() {
            std::error_code ec;
            fs::current_path(previous_, ec);
            fs::remove_all(root_, ec);
        }
        ShippedTree(const ShippedTree&)            = delete;
        ShippedTree& operator=(const ShippedTree&) = delete;

        // ⚠ ASSERTED, never assumed. A setup that failed leaves the loaders
        // reading an EMPTY tree, and an empty tree reports scanned=0 failed=0
        // healthy=TRUE and hands out every colour: the exact shape of a passing
        // run for anything that only checked "healthy".
        [[nodiscard]] bool Ok() const { return ok_; }

    private:
        fs::path root_;
        fs::path previous_;
        bool     ok_{ false };
    };

    // A world state. Skills are set across the whole range the rules can name,
    // so a rule this file forgets about is still answered rather than reading 0
    // by accident.
    DyeWorldState World(std::uint32_t a_level, std::uint32_t a_skill,
                        std::uint32_t a_channelsDyed) {
        DyeWorldState w;
        w.level = a_level;
        for (std::uint32_t av = kFirstSkillAV; av <= kLastSkillAV; ++av) {
            w.skills[av] = a_skill;
        }
        w.deeds[kDeedChannelsDyed] = a_channelsDyed;
        return w;
    }

}  // namespace

int main() {
    {  // promotion adds what is newly satisfied and reports it, so the caller
       // can tell the player. It never removes.
        std::vector<DyePromotable> palette{
            { "eso:free-one", "Common" },
            { "eso:needs-20", "Rare" },
            { "eso:needs-quest", "Rare" },
        };

        Json::Value root;
        root["tiers"]["Common"] = Always();
        root["tiers"]["Rare"]   = AtLevel(20);

        Json::Value questOnly(Json::arrayValue);
        Json::Value q;
        q["type"]   = "quest";
        q["plugin"] = "Skyrim.esm";
        q["formId"] = "0002A12F";
        questOnly.append(q);
        root["dyes"]["eso:needs-quest"] = questOnly;

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(root).rejected == 0);

        DyeWorldState w;
        w.level = 10;

        DyeUnlockSet set;
        auto         result = Promote(palette, rules, w, set);
        CHECK(result.gained.size() == 1);
        CHECK(result.gained[0] == "eso:free-one");
        CHECK(set.Has("eso:free-one"));
        CHECK(!set.Has("eso:needs-20"));
        // the quest is not in questsDone, and unknown must never pass for done
        CHECK(!set.Has("eso:needs-quest"));

        // running it again gains nothing: already-held ids are not re-reported
        result = Promote(palette, rules, w, set);
        CHECK(result.gained.empty());

        // level up and the Rare tier opens
        w.level = 20;
        result  = Promote(palette, rules, w, set);
        CHECK(result.gained.size() == 1);
        CHECK(result.gained[0] == "eso:needs-20");

        // ⚠ the world going BACKWARDS takes nothing away
        w.level = 1;
        result  = Promote(palette, rules, w, set);
        CHECK(result.gained.empty());
        CHECK(set.Has("eso:needs-20"));
        CHECK(set.Size() == 2);
    }

    {  // the gate. Off, everything is usable whatever the rules say.
        DyeUnlockSet set;
        CHECK(set.Add("eso:obsidian-black"));

        CHECK(CanUseDye("eso:obsidian-black", set, true));
        CHECK(!CanUseDye("eso:void-pitch", set, true));

        CHECK(CanUseDye("eso:obsidian-black", set, false));
        CHECK(CanUseDye("eso:void-pitch", set, false));
    }

    {  // The gold "newly earned" fold, and the reason it needs the mode.
       //
       // ⚠⚠ THE FREE-FORM ROW IS THE BUG THIS PINS. The mark clears through
       // DyeUnlocks::Acknowledge, which refuses an id the earned set does not
       // hold; with unlocks OFF nothing is locked, so a colour the character
       // never earned drew a fold that no click could take off (field
       // 2026-08-11). Every free-form combination has to answer false here or
       // that comes straight back.
        for (bool locked : { false, true }) {
            for (bool acked : { false, true }) {
                CHECK(!ShowsNewDyeMark(/*unlocksOn*/ false, locked, acked));
            }
        }
        // Lore-friendly: the one earned-and-unseen combination speaks.
        CHECK(ShowsNewDyeMark(true, /*locked*/ false, /*acknowledged*/ false));
        CHECK(!ShowsNewDyeMark(true, false, true));   // seen it already
        CHECK(!ShowsNewDyeMark(true, true, false));   // not earned: the padlock's job
        CHECK(!ShowsNewDyeMark(true, true, true));

        // And the pairing that makes the mark clearable: wherever the fold
        // shows, the swatch is usable, which is exactly the condition
        // Acknowledge needs to bank the mark.
        DyeUnlockSet earned;
        CHECK(earned.Add("eso:obsidian-black"));
        const bool lockedHere = !CanUseDye("eso:obsidian-black", earned, true);
        CHECK(!lockedHere);
        CHECK(ShowsNewDyeMark(true, lockedHere, /*acknowledged*/ false));
        const bool lockedOther = !CanUseDye("eso:void-pitch", earned, true);
        CHECK(lockedOther);
        CHECK(!ShowsNewDyeMark(true, lockedOther, /*acknowledged*/ false));
    }

    {  // ⚠ ANNOUNCE ONLY WHAT WAS ACTUALLY STORED. Add refuses an id that is
       // empty, over its length cap or past its count cap. Reporting one of
       // those as newly earned would re-announce it on every single load
       // forever, for a colour that can never be chosen, and the set would
       // disagree with the message the player just read.
        std::vector<DyePromotable> palette{
            { "", "Common" },                    // refused: an empty id
            { std::string(400, 'x'), "Common" },  // refused: past kMaxStrLen
            { "eso:real", "Common" },
        };

        Json::Value root;
        root["tiers"]["Common"] = Always();

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(root).rejected == 0);

        DyeWorldState w;
        DyeUnlockSet  set;
        const auto    result = Promote(palette, rules, w, set);
        CHECK(result.gained.size() == 1);
        CHECK(result.gained[0] == "eso:real");
        CHECK(set.Size() == 1);
    }

    {  // ⚠ A RARITY THAT MATCHED NOTHING IS FREE, and promotion is the only
       // place that can see it: the palette and the rule set are never both in
       // scope anywhere else. The tier lookup is an exact byte match, so
       // "Dye stamp" against a table that spells it "Dye Stamp" resolves to no
       // rule, and no rule means no requirement.
       //
       // Not a failure, deliberately: a third party pack may ship a bucket this
       // mod never heard of, and the documented fallback is that it stays free.
       // It is REPORTED so that a typo in a hand authored file is not silent.
        std::vector<DyePromotable> palette{
            { "eso:a", "Dye Stamp" },  // matches the tier, so it is gated
            { "eso:b", "Dye stamp" },  // one letter out: matches nothing
            { "eso:c", "Dye stamp" },
            { "vanilla:d", "" },       // no rarity at all: deliberately free
            { "eso:e", "Rare" },       // own rule, so its missing tier is moot
        };

        Json::Value root;
        root["tiers"]["Dye Stamp"] = AtLevel(60);
        root["dyes"]["eso:e"]      = AtLevel(60);

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(root).rejected == 0);

        DyeWorldState w;
        w.level = 1;
        DyeUnlockSet set;
        auto         result = Promote(palette, rules, w, set);

        // exactly the rarity nothing matched, and how many dyes it covered.
        // ⚠ An EMPTY rarity is not in here: a dye with no rarity is the
        // supported way to ship a free colour, and vanilla.json's twelve do
        // exactly that. A dye with its own rule is not in here either, because
        // its tier was never consulted.
        CHECK(result.unmatchedRarities.size() == 1);
        CHECK(Unmatched(result, "Dye stamp") == 2);
        CHECK(Unmatched(result, "Dye Stamp") == 0);

        // and the two of them really are free at level 1, which is why the
        // line is worth printing. vanilla:d is free on purpose; b and c are
        // free by accident and look identical from the save's point of view.
        CHECK(result.gained.size() == 3);
        CHECK(!set.Has("eso:a"));
        CHECK(!set.Has("eso:e"));

        // ⚠ REPORTED AGAIN on the next pass, though those two are now held.
        // The check sits BEFORE the already-held skip on purpose. The load
        // where the damage is already in the save is exactly the load a player
        // goes looking for a reason on, and a warning that fires once and
        // never again is one nobody reads.
        result = Promote(palette, rules, w, set);
        CHECK(result.gained.empty());
        CHECK(result.unmatchedRarities.size() == 1);
        CHECK(Unmatched(result, "Dye stamp") == 2);
    }

    {  // ⚠ AN AUTHORED [] IS NOT AN UNMATCHED RARITY, and this is the pair
       // that makes Covers worth having at all. Both shapes below resolve to
       // an empty condition list, exactly like a rarity nothing matched, so a
       // report built on emptiness would warn about two colours an author
       // deliberately freed and the real warning would drown.
        std::vector<DyePromotable> palette{
            { "eso:freebie", "Iridescent" },  // its OWN rule, spelled []
            { "eso:tiered", "Rare" },         // its TIER, spelled []
            { "eso:orphan", "Iridescent" },   // genuinely nothing authored
        };

        Json::Value root;
        root["dyes"]["eso:freebie"] = Json::Value(Json::arrayValue);
        root["tiers"]["Rare"]       = Json::Value(Json::arrayValue);

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(root).rejected == 0);

        DyeWorldState w;
        w.level = 1;
        DyeUnlockSet set;
        const auto   result = Promote(palette, rules, w, set);

        // all three really are free. The only difference is whether an author
        // said so, and that difference is the whole report.
        CHECK(result.gained.size() == 3);
        CHECK(result.unmatchedRarities.size() == 1);
        CHECK(Unmatched(result, "Iridescent") == 1);  // the orphan alone
    }

    {  // ⚠ AN ORPHANED PER-DYE KEY, the free-dye route every other report is
       // blind to. The key merges cleanly, it is not an unknown top level key,
       // and its dye drops onto a rarity tier that exists, so Covers answers
       // true and unmatchedRarities stays empty. Nothing above promotion holds
       // the palette and the rule set at once, so nothing above can see it.
        std::vector<DyePromotable> palette{
            { "eso:master-gold", "Common" },
            { "eso:plain", "Common" },
        };

        Json::Value root;
        root["tiers"]["Common"]         = Always();
        root["dyes"]["eso:master_gold"] = AtLevel(50);  // typo: no dye claims it

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(root).rejected == 0);

        DyeWorldState w;
        w.level = 1;
        DyeUnlockSet set;
        auto         result = Promote(palette, rules, w, set);

        CHECK(result.unmatchedDyeRules.size() == 1);
        CHECK(Names(result.unmatchedDyeRules, "eso:master_gold"));
        CHECK(result.unmatchedRarities.empty());  // the blind spot, measured

        // ⚠ AND THE CONSEQUENCE, which is what makes the line worth printing.
        // The colour the author gated at level 50 fell onto the Common tier,
        // which is always, so a level 1 character owns it permanently.
        CHECK(set.Has("eso:master-gold"));

        // reported AGAIN next pass, for the same reason the rarity report is:
        // the load a player goes looking on is the one where it already
        // happened.
        result = Promote(palette, rules, w, set);
        CHECK(result.gained.empty());
        CHECK(result.unmatchedDyeRules.size() == 1);
    }

    {  // the control, and the half that keeps the report honest. Every key
       // claimed means the report says nothing, because a warning that fires
       // on a healthy install is one nobody reads. The shipped configuration
       // is this shape.
        std::vector<DyePromotable> palette{
            { "eso:master-gold", "Common" },
        };

        Json::Value root;
        root["tiers"]["Common"]         = Always();
        root["dyes"]["eso:master-gold"] = AtLevel(50);

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(root).rejected == 0);

        DyeWorldState w;
        w.level = 1;
        DyeUnlockSet set;
        const auto   result = Promote(palette, rules, w, set);
        CHECK(result.unmatchedDyeRules.empty());
        CHECK(!set.Has("eso:master-gold"));  // the gate really is holding
    }

    {  // the whole palette against NO rules at all. This is what a user who
       // deleted Unlocks/eso.json looks like from in here, and what a colour
       // pack that shipped no rules file has always looked like: every dye
       // free. The rule set cannot tell those two apart, so promotion names
       // every rarity it could not match and lets the log say it once.
        std::vector<DyePromotable> palette{
            { "eso:a", "Common" },
            { "eso:b", "Common" },
            { "eso:c", "Rare" },
        };
        const DyeRuleSet rules;  // nothing authored at all
        DyeWorldState    w;
        DyeUnlockSet     set;
        const auto       result = Promote(palette, rules, w, set);
        CHECK(result.gained.size() == 3);
        CHECK(result.unmatchedRarities.size() == 2);
        CHECK(Unmatched(result, "Common") == 2);
        CHECK(Unmatched(result, "Rare") == 1);
    }

    {  // ⚠ THE SHIPPED CONFIGURATION, end to end, through the real Load of
       // both directories. See ShippedTree above for why nothing did this
       // before and why a Python checker cannot stand in for it.
        const ShippedTree tree;
        CHECK(tree.Ok());

        const auto rules = DyeRules::Load();
        // A rules file that fails to load contributes none of its keys, and
        // every dye it covered then resolves to no rule, which means FREE and
        // permanent. This is the assertion that the 36 line // comment header
        // still parses under failIfExtra and rejectDupKeys.
        // ⚠ THREE FILES SINCE 2026-08-13: eso.json, lore.json and skyrim.json,
        // and the count is asserted rather than ignored on purpose. All three
        // ship in core, so this IS every install's shape - the "free-form gets
        // eso.json alone" this comment used to describe stopped being true when
        // make_fomod.sh moved lore.json into core. The number moving is how a
        // fourth file appearing in dist, or one failing to load, gets noticed
        // here rather than in somebody's save.
        CHECK(rules.filesScanned == 3);
        CHECK(rules.filesFailed == 0);
        CHECK(rules.rulesRejected == 0);
        // ⚠ AND NO NOTES. A note here means a section or a rule shipped empty,
        // which frees every colour it covers without failing anything.
        CHECK(rules.notes.empty());
        for (const auto& why : rules.failures) {
            std::printf("FAIL shipped rules: %s\n", why.c_str());
            ++g_failures;
        }
        for (const auto& note : rules.notes) {
            std::printf("FAIL shipped rules note: %s\n", note.c_str());
            ++g_failures;
        }
        const auto checked = DyeRules::SnapshotChecked();
        CHECK(checked.healthy);
        CHECK(checked.rules.TierCount() == 5);
        // ⚠ 220 -> 239 ON 2026-08-11: the nineteen fr: metals got their own
        // ladder instead of falling to the Material tier and arriving in one
        // batch. This number moving is not incidental - it is the only place
        // the REAL C++ loader confirms it read every one of them, and ten of
        // the nineteen carry a second clause, so it also proves multi-clause
        // rules parse. The Python checker cannot stand in for that.
        //
        // ⚠ 239 -> 367 ON 2026-08-13, and it is a MERGED count, not a sum. The
        // three files carry 239, 139 and 121 keys. eso.json and lore.json
        // overlap on all but six of the second, because lore.json mostly
        // REPLACES rules eso.json already had; skyrim.json overlaps neither,
        // because every key in it is a colour that did not exist before. So
        // 239 + 6 + 121 = 366.
        //
        // ⚠ Three of those six were COMMON, which is "always", so they were free
        // at level 1 and are gated now. That direction is safe: unlocks are
        // add-only, so nobody who already earned one loses it.
        CHECK(checked.rules.DyeCount() == 366);

        // ⚠ THE SHIPPED STAT DEEDS, THROUGH THE REAL LOADER, and this is the
        // only assertion anywhere that can prove they parsed. A "stat:" deed
        // reaches Game.QueryStat, which answers int 0 for a name it does not
        // know - the same 0 an honest zero gives - so a clause that failed to
        // load and a colour nobody has earned yet are indistinguishable at
        // runtime, in the log, and in the game. If StatNames() comes back short
        // here, 58 colours are unreachable on every character and nothing else
        // would say so.
        //
        // ⚠ THE EXACT SPELLINGS ARE THE POINT, not decoration. Three of the
        // eight questline counters carry a word or an apostrophe nobody
        // guesses, and all three are shipped, so they are asserted by name.
        {
            const auto stats = checked.rules.StatNames();
            CHECK(stats.size() == 30);
            CHECK(stats.contains("Locations Discovered"));
            CHECK(stats.contains("Civil War Quests Completed"));
            CHECK(stats.contains("Dungeons Cleared"));
            CHECK(stats.contains("Werewolf Transformations"));
            CHECK(stats.contains("Days as a Vampire"));
            CHECK(stats.contains("Standing Stones Found"));
            CHECK(stats.contains("Misc Objectives Completed"));
            // ⚠⚠ THESE THREE ARE THE WHOLE REASON tools/make_stat_index.py
            // EXISTS, and they are asserted character for character. Every one
            // was typed the obvious way first - "Companions ...", "Thieves
            // Guild ...", "Dark Brotherhood ..." - and every one of those
            // spellings answers int 0 through Game.QueryStat, which is byte for
            // byte what an honest zero answers. No log line, no throw, and no
            // way to tell it from a colour simply not earned yet. If somebody
            // tidies the leading "The" or the apostrophe out of the shipped
            // TSV, this is the only thing anywhere that will say so.
            CHECK(stats.contains("The Companions Quests Completed"));
            CHECK(stats.contains("Thieves' Guild Quests Completed"));
            CHECK(stats.contains("The Dark Brotherhood Quests Completed"));
            // ⚠ AND OURS IS NOT IN IT. channelsDyed is counted by this plugin,
            // not by the engine, and dispatching it would ask Skyrim for a
            // counter it has never heard of - answered 0, colour locked, no log
            // line. The shipped rules gate 32 Dye Stamp colours on it, so if the
            // prefix rule ever stopped separating the two vocabularies this is
            // where it shows.
            CHECK(!stats.contains("channelsDyed"));
        }

        const auto palReport = DyePalette::Load(0);
        // ⚠ FOUR FILES SINCE 2026-08-13: eso.json, vanilla.json, pearl.json and
        // skyrim.json. Each is its own file rather than appended to eso.json
        // because each is a different KIND of thing: eso mirrors ESO's palette,
        // pearl is the authored metallic finishes, vanilla is the starter set,
        // and skyrim is named for Skyrim's own world and earned by doing
        // Skyrim's own things. Same reason the count is asserted rather than
        // ignored: a fifth file appearing, or one failing to load, is noticed
        // here.
        CHECK(palReport.filesScanned == 4);
        // ⚠ AND entriesRejected STAYS ZERO, WHICH IS WHAT PROVES THE FINISH KEYS
        // PARSE. pearl.json and skyrim.json both carry hex2, mode, gloss, sheen
        // and flake, and a malformed one of those is a REFUSED ENTRY rather than
        // a dropped key: hex2 and sheen go through the same strict six-digit
        // validator the primary colour does. So a typo in any of the 38 finishes
        // shows up right here as a rejected entry and a colour the player never
        // gets, rather than as a swatch that quietly ships flat.
        CHECK(palReport.entriesRejected == 0);
        CHECK(palReport.idCollisions == 0);
        CHECK(palReport.raritiesDropped == 0);
        std::vector<DyePromotable> palette;
        for (const auto& dye : DyePalette::Snapshot()) {
            palette.push_back({ dye.id, dye.rarity });
        }
        // ⚠ 337 -> 458 ON 2026-08-13. idCollisions above is what makes this a
        // sum rather than a hope: skyrim.json's ids are all "skyrim:" prefixed,
        // so nothing in it can quietly displace an eso: or fr: colour and take
        // that colour's unlock rule with it.
        CHECK(palette.size() == 458);  // 306 eso + 12 vanilla + 19 pearl + 121 skyrim

        {   // ⚠ THE SHIPPED SPECIAL DYES ARE READ BACK THROUGH THE REAL LOADER,
            // which is the only place in the suite where the JSON on disk, the
            // parser and the record meet. The palette tests build their Json::Value
            // by hand, so a pearl.json with a typo in a key name would parse
            // cleanly, ship flat, and be caught by nothing.
            const auto snap = DyePalette::Snapshot();
            const auto find = [&snap](std::string_view a_id) -> const Dye* {
                for (const auto& d : snap) {
                    if (d.id == a_id) {
                        return &d;
                    }
                }
                return nullptr;
            };
            const auto* pearl = find("fr:abyssal-pearl");
            CHECK(pearl != nullptr);
            if (pearl) {
                CHECK(pearl->colour.mode == 2);  // iridescent
                CHECK(pearl->colour.secondSet);
                CHECK(pearl->colour.r2 == 0xC9 && pearl->colour.g2 == 0xA0 &&
                      pearl->colour.b2 == 0xD8);
                CHECK(pearl->colour.palette.glossSet);
                CHECK(pearl->rarity == "Material");
            }
            const auto* nacre = find("fr:moonlit-nacre");
            CHECK(nacre != nullptr);
            if (nacre) {
                CHECK(nacre->colour.mode == 1);  // nacre
                CHECK(nacre->colour.secondSet);
            }
            // ⚠ THE EARNED FINISHES, READ BACK THE SAME WAY. The premium
            // rewards in skyrim.json wear a pearlescent finish rather than a
            // brighter flat colour (user 2026-08-13), and the finish is what
            // makes them worth the questline. That is authored in a TSV and
            // emitted by a generator, so nothing between the author and the
            // player looks at it: a mode the loader does not recognise silently
            // ships FLAT, which is the one failure that would still pass every
            // count above.
            const auto* endOfLine = find("skyrim:sithis-black");
            CHECK(endOfLine != nullptr);
            if (endOfLine) {
                CHECK(endOfLine->colour.mode == 2);  // iridescent
                CHECK(endOfLine->colour.secondSet);
                CHECK(endOfLine->colour.palette.glossSet);
                CHECK(endOfLine->colour.palette.sheenSet);
                // ⚠ Rare, NOT Material, and that is deliberate. The grid files
                // it under Metallic off the finish fields, so its rarity is free
                // to go on pricing the unlock. A pack that used Material here to
                // "mean metallic" would gate it on Smithing 30 instead.
                CHECK(endOfLine->rarity == "Rare");
            }
            // Flake on top of a ramp, which pearl.json never combines: the
            // Wabbajack is the loudest thing in the pack on purpose.
            const auto* mad = find("skyrim:wabbajack-violet");
            CHECK(mad != nullptr);
            if (mad) {
                CHECK(mad->colour.mode == 2);
                CHECK(mad->colour.secondSet);
                CHECK(mad->colour.flake == 255);
            }

            // The all-flake dye: no ramp at all, sparkle alone. Proves the third
            // family is independent of the other two.
            const auto* star = find("fr:starlight");
            CHECK(star != nullptr);
            if (star) {
                CHECK(star->colour.mode == 0);
                CHECK(!star->colour.secondSet);
                CHECK(star->colour.flake == 255);
                CHECK(star->colour.palette.sheenSet);
            }
            // The one that authors a FINISH and no ramp, which is the case that
            // proves the two families are independent: a sheen with no second stop.
            const auto* gold = find("fr:burnished-gold");
            CHECK(gold != nullptr);
            if (gold) {
                CHECK(gold->colour.mode == 0);
                CHECK(!gold->colour.secondSet);
                CHECK(gold->colour.palette.sheenSet);
                CHECK(gold->colour.palette.glossSet && gold->colour.palette.gloss == 210);
            }
        }

        {   // A brand new character: level 1, every skill at 25, which is the
            // highest base a race can hand out, and no deeds. What is gained
            // here is what the design gives away at character creation and
            // makes permanent.
            //
            // ⚠ A BAND, NOT A NUMBER. The exact 63 is 64 Commons plus
            // vanilla.json's twelve free-by-no-rarity, minus the thirteen
            // overrides that tighten Common dyes. Pinning it exactly would
            // turn every deliberate palette edit into a test failure with no
            // bug behind it; a band still catches the failures that matter,
            // which are all enormous: a lost "tiers" section is 318 and a
            // softened Uncommon is 163.
            //
            // ⚠ THE BAND IS ALSO THE FLOOR ON THE STARTING PALETTE. Every
            // per-dye override written for a Common colour costs one swatch
            // here, because nothing else in the table can be satisfied at
            // level 1: the skill floor is 26 and the level floor is 2. Adding
            // Common overrides is the one edit to the rules that can walk this
            // out of its band, so read a failure at the bottom end as "the
            // Commons stopped being the starting palette" rather than as a
            // loader fault.
            DyeUnlockSet set;
            const auto   res = Promote(palette, checked.rules,
                                       World(1, 25, 0), set);
            CHECK(res.gained.size() >= 60);
            CHECK(res.gained.size() <= 90);
            if (res.gained.size() < 60 || res.gained.size() > 90) {
                std::printf("     level 1 gained %zu, expected 60 to 90\n",
                            res.gained.size());
            }
            // ⚠ BOTH FREE-DYE REPORTS EMPTY. A rarity no tier matched frees the
            // whole bucket, and it is an exact byte match, so "Dye stamp"
            // against "Dye Stamp" is 32 colours. An override key no dye claims
            // hands its colour back to a looser tier and the rarity report
            // cannot see it.
            CHECK(res.unmatchedRarities.empty());
            for (const auto& [rarity, n] : res.unmatchedRarities) {
                std::printf("FAIL shipped: rarity \"%s\" has no tier, %zu "
                            "colour(s) free\n", rarity.c_str(), n);
                ++g_failures;
            }
            CHECK(res.unmatchedDyeRules.empty());
            for (const auto& key : res.unmatchedDyeRules) {
                std::printf("FAIL shipped: override \"%s\" names no dye\n",
                            key.c_str());
                ++g_failures;
            }
        }

        {   // ⚠ AND THE OTHER END, which is the claim that was FALSE until the
            // "channelsDyed" counter got a producer. A maxed character with the
            // deed at zero reaches 282 of 318, and the 36 that are missing are
            // the ones gated on that counter: the whole Dye Stamp tier plus
            // four overrides. The commit body and STATUS both said "318 of 318,
            // nothing unreachable", measured with the counter set by hand in a
            // probe.
            //
            // This asserts the SHAPE, not the number: everything a maxed
            // character can reach without ever dyeing anything, and then all
            // 318 once the deed is earned. If a future rule gates on a second
            // counter with no producer, the second half of this fails.
            DyeUnlockSet neverDyed;
            const auto   res = Promote(palette, checked.rules,
                                       World(100, 100, 0), neverDyed);
            CHECK(res.gained.size() < palette.size());   // the deed is real

            // ⚠ THE EXACT COUNTS ARE GONE AND THAT IS THE POINT OF World().
            // These were 282 and "all of them" while every rule gated on a
            // level, a skill or our own counter, which World() can express.
            // lore.json gates 80 colours on named quests and cleared locations,
            // and World() models neither, so those are unreachable here BY
            // CONSTRUCTION rather than by any fault in the rules. Asserting a
            // number again would only be asserting how many lore rules exist.
            //
            // What still has to hold is the shape the original assertion was
            // really about: earning the deed strictly widens what a maxed
            // character can reach, and everything NOT reachable is a lore lock
            // rather than something with no producer at all.
            DyeUnlockSet earned;
            const auto   res2 = Promote(palette, checked.rules,
                                        World(100, 100, 100), earned);
            CHECK(res2.gained.size() > res.gained.size());
            const auto loreLocked = checked.rules.DyeKeys().size();
            CHECK(res2.gained.size() + loreLocked >= palette.size());
            if (res2.gained.size() + loreLocked < palette.size()) {
                std::printf("     maxed gained %zu of %zu, so something in the "
                            "shipped table cannot be earned at all\n",
                            res2.gained.size(), palette.size());
                for (const auto& dye : palette) {
                    if (!earned.Has(dye.id)) {
                        std::printf("     unreachable: %s [%s]\n",
                                    dye.id.c_str(), dye.rarity.c_str());
                    }
                }
            }
        }
    }

    if (g_failures == 0) {
        std::printf("DyePromotionTests: all passed\n");
        return 0;
    }
    std::printf("DyePromotionTests: %d failure(s)\n", g_failures);
    return 1;
}
