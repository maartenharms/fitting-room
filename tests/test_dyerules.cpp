// Dye rule tests. No SKSE, no engine: what a dye id requires, its own entry
// beating its rarity's beating free, is resolved from hand authored JSON, and
// all of it is provable without a save.
#include "DyeRules.h"

#include <json/json.h>

#include <process.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
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

namespace fs = std::filesystem;

// Names a list, rather than counting it. A size check passes for a list that
// holds the same key twice, which is how half a report can go missing.
static bool Holds(const std::vector<std::string>& a_list, std::string_view a_key) {
    return std::ranges::any_of(
        a_list, [a_key](const std::string& a_entry) { return a_entry == a_key; });
}

// ⚠ THIS SUITE TOUCHES DISK, alone among the pure ones, and deliberately.
// Three defects of the same shape have now landed in Load(), each one making a
// BROKEN install report identically to a healthy or an uninstalled one. None
// was reachable from a MergeFromJson test, because the step that turns a merge
// result into a file failure lives in Load and nothing exercised it. A test
// that stops at the flags leaves that step free to be reordered back into the
// bug, which is exactly what happened.
//
// kRulesDir is relative to the working directory, so each case gets its own.
namespace {
    // The parent of every sandbox, one per PROCESS.
    //
    // ⚠ The process id is in the name. This used to be a fixed path, so two
    // runs of this suite at once would remove_all each other's tree out from
    // under them. Concurrent builds are forbidden here and it was never
    // observed, but the id costs nothing to add.
    //
    // Removed once at the end of main. Each Sandbox already removes its own
    // directory; the parent was simply left in %TEMP% forever.
    fs::path SandboxParent(std::error_code& a_ec) {
        return fs::temp_directory_path(a_ec) /
               ("FittingRoomDyeRulesTests-" + std::to_string(_getpid()));
    }

    class Sandbox {
    public:
        // ⚠ EVERY STEP IS CHECKED, and none of them was until 2026-08-02. One
        // error_code was reused across four calls and read by nobody, so a
        // setup that failed left the case running against whatever the process
        // working directory happened to be. That is not a harmless flake: the
        // "no-unlocks-dir" case asserts scanned=0 failed=0 healthy=TRUE, which
        // is exactly what a failed setup produces, so it could pass VACUOUSLY
        // while proving nothing. Ok() is asserted by every case.
        //
        // ⚠ The previous_ capture is checked for its own reason. If it failed
        // the destructor's restore was a silent no-op, leaving the process
        // working directory inside a tree the very next line removes.
        explicit Sandbox(const char* a_name, bool a_makeUnlocks = true) {
            std::error_code ec;
            previous_ = fs::current_path(ec);
            ok_       = !ec;
            root_ = SandboxParent(ec) / a_name;
            ok_   = ok_ && !ec;
            fs::remove_all(root_, ec);
            ok_ = ok_ && !ec;
            fs::create_directories(a_makeUnlocks ? Unlocks() : Unlocks().parent_path(),
                                   ec);
            ok_ = ok_ && !ec;
            fs::current_path(root_, ec);
            ok_ = ok_ && !ec;
        }
        ~Sandbox() {
            std::error_code ec;
            fs::current_path(previous_, ec);
            fs::remove_all(root_, ec);
        }
        Sandbox(const Sandbox&)            = delete;
        Sandbox& operator=(const Sandbox&) = delete;

        // Whether the sandbox is actually standing. A case that asserts on a
        // report gathered from the wrong directory has proved nothing.
        [[nodiscard]] bool Ok() const { return ok_; }

        [[nodiscard]] fs::path Root() const { return root_; }
        [[nodiscard]] fs::path Unlocks() const {
            return root_ / "Data" / "SKSE" / "Plugins" / "FittingRoom" / "Unlocks";
        }
        void Write(const char* a_name, const std::string& a_body) const {
            std::ofstream out(Unlocks() / a_name);
            out << a_body;
        }

    private:
        fs::path root_;
        fs::path previous_;
        bool     ok_{ false };
    };

    // A junction, which needs no elevation and is what MO2's USVFS and Vortex
    // produce when they deploy. Returns whether the NAME now resolves as a
    // link, so a case that cannot set one up FAILS rather than passing quietly.
    bool MakeJunction(const fs::path& a_link, const fs::path& a_target) {
        const auto cmd = std::string("cmd /c mklink /J \"") + a_link.string() +
                         "\" \"" + a_target.string() + "\" >nul 2>&1";
        static_cast<void>(std::system(cmd.c_str()));
        std::error_code ec;
        return fs::exists(fs::symlink_status(a_link, ec)) && !ec;
    }

    // The shape that actually ships: BOTH sections, and a Common-rarity dye
    // whose own entry gates it at level 50. Every mis-edit below is applied to
    // this, because a mis-edit happens to a real file.
    const char* const kTiers =
        R"("tiers":{"Common":[{"type":"always"}],)"
        R"("Uncommon":[{"type":"level","min":20}]})";
    const char* const kDyes =
        R"("dyes":{"eso:master-gold":[{"type":"level","min":50}]})";

    // What dist/.../Unlocks/eso.json actually opens with: a run of 44
    // consecutive // lines, 6 of them bare.
    //
    // ⚠ ONE STATEMENT OF EACH NUMBER, used by the case below AND by the
    // assertions that check the case is the shape it claims. Two statements is
    // how the case came to be four lines while the commit that added it said
    // thirty six, with no way to tell which of the two was the claim.
    //
    // ⚠ AND THE SHIPPED FILE IS NOT PINNED TO THESE. This is a SHAPE test: a
    // long run of consecutive comment lines with bare ones in it must survive
    // the real reader flags. DyePromotionTests loads the real eso.json bytes
    // and would fail on its own if the header ever stopped parsing, so growing
    // that header does not have to be matched here in the same commit. Match it
    // when convenient and keep this generous.
    constexpr std::size_t kHeaderLines = 44;
    constexpr std::size_t kHeaderBare  = 6;

    // A run of a_lines consecutive "//" lines, a_bare of them carrying nothing
    // after the slashes, spread through the run rather than bunched at one end.
    // Built to a count so the case cannot drift away from what the shipped file
    // has, which is exactly what happened to the four-line version of it.
    std::string Header(std::size_t a_lines, std::size_t a_bare) {
        std::string out;
        const auto  every = a_bare > 0 ? a_lines / a_bare : a_lines + 1;
        std::size_t bare  = 0;
        for (std::size_t i = 0; i < a_lines; ++i) {
            const bool blank = bare < a_bare && i > 0 && i % every == 0;
            out += blank ? "//\n" : "// header line\n";
            bare += blank ? 1 : 0;
        }
        return out;
    }

    // Whether the published rule set would hand master-gold to a level 1
    // character. This is the consequence every failure below exists to
    // prevent, and asserting the report alone would not see it.
    bool GoldIsFree() {
        const auto checked = DyeRules::SnapshotChecked();
        return AllSatisfied(checked.rules.For("eso:master-gold", "Common"),
                            DyeWorldState{});
    }
}  // namespace

int main() {
    {  // resolution order: the dye's own entry beats its tier, and a dye with
       // neither is free. That last fallback is what keeps vanilla.json, which
       // carries no rarity at all, working exactly as it did.
        Json::Value root;
        Json::Value common(Json::arrayValue);
        Json::Value always;
        always["type"] = "always";
        common.append(always);
        root["tiers"]["Common"] = common;

        Json::Value rare(Json::arrayValue);
        Json::Value lvl35;
        lvl35["type"] = "level";
        lvl35["min"]  = 35;
        rare.append(lvl35);
        root["tiers"]["Rare"] = rare;

        Json::Value override_(Json::arrayValue);
        Json::Value wolfQuest;
        wolfQuest["type"]   = "quest";
        wolfQuest["plugin"] = "Skyrim.esm";
        wolfQuest["formId"] = "0002A12F";
        override_.append(wolfQuest);
        root["dyes"]["eso:wolfs-fur-brown"] = override_;

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(root).rejected == 0);   // no rejects

        // an override wins over the tier
        const auto wolf = rules.For("eso:wolfs-fur-brown", "Common");
        CHECK(wolf.size() == 1);
        CHECK(wolf[0].kind == DyeCondKind::kQuest);

        // no override: the tier applies
        const auto voidPitch = rules.For("eso:void-pitch", "Rare");
        CHECK(voidPitch.size() == 1);
        CHECK(voidPitch[0].kind == DyeCondKind::kLevel);
        CHECK(voidPitch[0].min == 35u);

        // a rarity with no tier entry, and no rarity at all, are both free
        CHECK(rules.For("eso:x", "Iridescent").empty());
        CHECK(rules.For("vanilla:ebony", "").empty());
    }

    {  // every quest a rule mentions, so the bridge queries those and only
       // those rather than walking every quest in the load order
        Json::Value root;
        Json::Value arr(Json::arrayValue);
        Json::Value q1;
        q1["type"]   = "quest";
        q1["plugin"] = "Skyrim.esm";
        q1["formId"] = "0002A12F";
        arr.append(q1);
        root["dyes"]["eso:a"] = arr;

        Json::Value arr2(Json::arrayValue);
        Json::Value q2;
        q2["type"]   = "quest";
        q2["plugin"] = "Dawnguard.esm";
        q2["formId"] = "00004EE4";
        arr2.append(q2);
        arr2.append(q1);            // the same quest twice, deduped
        root["tiers"]["Rare"] = arr2;

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(root).rejected == 0);
        const auto refs = rules.QuestRefs();
        CHECK(refs.size() == 2);
        CHECK(refs.contains(QuestRef{ "Skyrim.esm", 0x0002A12F }));
        CHECK(refs.contains(QuestRef{ "Dawnguard.esm", 0x00004EE4 }));
    }

    {  // a bad RULE is counted AND LOCKS its dye; the rest of the file still
       // loads, because one typo should not cost the other colours their rules
        Json::Value root;
        Json::Value ok(Json::arrayValue);
        Json::Value lvl;
        lvl["type"] = "level";
        lvl["min"]  = 10;
        ok.append(lvl);

        Json::Value broken(Json::arrayValue);
        Json::Value nope;
        nope["type"] = "horoscope";
        broken.append(nope);

        root["dyes"]["eso:good"] = ok;
        root["dyes"]["eso:bad"]  = broken;

        DyeRuleSet rules;
        const auto merged = rules.MergeFromJson(root);
        CHECK(merged.rejected == 1);
        CHECK(merged.SawSection());  // it DID contribute, one rule was just bad
        CHECK(rules.For("eso:good", "").size() == 1);

        // ⚠ A rejected rule becomes kNever, so its dye LOCKS. It must NOT come
        // back empty, because empty means free and a freed colour is promoted
        // into the save and survives the typo being fixed. Locking is the
        // reversible direction. This assertion is the whole reason kNever
        // exists; if it ever flips to .empty(), read the spec before "fixing"
        // it.
        const auto bad = rules.For("eso:bad", "");
        CHECK(bad.size() == 1);
        CHECK(bad[0].kind == DyeCondKind::kNever);
        CHECK(!AllSatisfied(bad, DyeWorldState{}));

        // A rejected rule beats its tier too. Falling back to the tier would
        // quietly hand the dye whatever the tier allows.
        Json::Value tierRoot;
        Json::Value freeTier(Json::arrayValue);
        Json::Value always;
        always["type"] = "always";
        freeTier.append(always);
        tierRoot["tiers"]["Common"] = freeTier;
        CHECK(rules.MergeFromJson(tierRoot).rejected == 0);
        CHECK(rules.For("eso:bad", "Common")[0].kind == DyeCondKind::kNever);
    }

    {  // ⚠ A LATER FILE WINS A KEY, INCLUDING WITH A BROKEN RULE. The header
       // spends five lines on this and nothing exercised it, which makes it
       // exactly the behaviour a later "improvement" would reverse. It is also
       // the whole reason Load bothers to sort filenames.
       //
       // One broken third party rules file can therefore lock a tier the
       // shipped file defined correctly. That is deliberate: the later author
       // meant to override, and a lock is the recoverable direction.
        Json::Value first;
        Json::Value good(Json::arrayValue);
        Json::Value lvl;
        lvl["type"] = "level";
        lvl["min"]  = 10;
        good.append(lvl);
        first["tiers"]["Rare"] = good;

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(first).rejected == 0);
        CHECK(rules.For("eso:x", "Rare").size() == 1);
        CHECK(rules.For("eso:x", "Rare")[0].kind == DyeCondKind::kLevel);

        Json::Value second;
        Json::Value broken(Json::arrayValue);
        Json::Value nope;
        nope["type"] = "horoscope";
        broken.append(nope);
        second["tiers"]["Rare"] = broken;

        const auto merged = rules.MergeFromJson(second);
        CHECK(merged.rejected == 1);
        CHECK(merged.rejectedKeys.size() == 1);
        CHECK(merged.rejectedKeys[0] == "Rare");   // names it, does not just count it
        CHECK(rules.For("eso:x", "Rare").size() == 1);
        CHECK(rules.For("eso:x", "Rare")[0].kind == DyeCondKind::kNever);
    }

    {  // an explicitly EMPTY override beats a restrictive tier. Task 1 pins
       // that ConditionsFromJson accepts [], but nothing pinned that For
       // returns it rather than falling through. A For that treated an empty
       // entry as absent would flip a deliberately free dye to locked and every
       // other test here would stay green.
        Json::Value root;
        Json::Value strict(Json::arrayValue);
        Json::Value lvl;
        lvl["type"] = "level";
        lvl["min"]  = 50;
        strict.append(lvl);
        root["tiers"]["Rare"]        = strict;
        root["dyes"]["eso:freebie"]  = Json::Value(Json::arrayValue);

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(root).rejected == 0);
        CHECK(rules.For("eso:freebie", "Rare").empty());          // free
        CHECK(rules.For("eso:other", "Rare").size() == 1);        // still gated

        // ⚠ THE PAIR, UNDER ONE WORLD STATE, is what proves anything here.
        // AllSatisfied on an empty vector is true BY DEFINITION, so asserting
        // it on the freebie alone only restates the .empty() check above and
        // cannot fail independently of it. The half that can fail is the other
        // dye: the same level 1 character must clear the freebie AND be
        // refused the tier, which is the difference the override is for.
        DyeWorldState lowLevel;
        lowLevel.level = 1;
        CHECK(AllSatisfied(rules.For("eso:freebie", "Rare"), lowLevel));
        CHECK(!AllSatisfied(rules.For("eso:other", "Rare"), lowLevel));
    }

    {  // ⚠ COVERS IS NOT !For().empty(), AND NOTHING IN THIS FILE PINNED THAT.
       // Every case here would still pass against a Covers rewritten as
       // "return !For(a_id, a_rarity).empty();", because the only two shapes
       // where the pair disagrees were never authored: a rule spelled [] is
       // AUTHORED and resolves to an empty vector, which is the same value an
       // unauthored one returns with the opposite meaning.
       //
       // Promotion reads Covers to decide whether to WARN. A Covers answering
       // off emptiness would warn about every colour an author deliberately
       // freed, which is noise, and noise is how the real warning stops being
       // read.
       //
       // Both now go through one private Find, so the pair cannot drift. These
       // assertions are what would catch it going back.
        Json::Value root;
        root["dyes"]["eso:freebie"] = Json::Value(Json::arrayValue);
        root["tiers"]["Rare"]       = Json::Value(Json::arrayValue);

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(root).rejected == 0);

        // an explicit [] on a DYE: authored, and free
        CHECK(rules.Covers("eso:freebie", "Iridescent"));
        CHECK(rules.For("eso:freebie", "Iridescent").empty());

        // an explicit [] on a TIER: the same thing one grain wider, and this
        // is the one with a blast radius, since it covers a whole rarity
        CHECK(rules.Covers("eso:anything", "Rare"));
        CHECK(rules.For("eso:anything", "Rare").empty());

        // the negative half, which is the only one an emptiness check got right
        CHECK(!rules.Covers("eso:anything", "Iridescent"));
        CHECK(!rules.Covers("eso:anything", ""));

        // ⚠ AN EMPTY RARITY ANSWERS ON THE PER-DYE ENTRY ALONE, so a colour
        // shipped with no rarity can never pick up whatever "" would hit in the
        // tier table.
        CHECK(rules.Covers("eso:freebie", ""));
    }

    {  // DyeKeys names every per-dye key, so promotion can check them against
       // the palette. An orphaned key is silent at every other layer: it merges
       // cleanly, it is not an unknown top level key, and its dye falls back to
       // a rarity tier that exists, which makes Covers answer true.
        Json::Value root;
        root["dyes"]["eso:b"] = Json::Value(Json::arrayValue);
        root["dyes"]["eso:a"] = Json::Value(Json::arrayValue);
        root["tiers"]["Rare"] = Json::Value(Json::arrayValue);  // NOT a dye key

        DyeRuleSet rules;
        CHECK(rules.MergeFromJson(root).rejected == 0);
        const auto keys = rules.DyeKeys();
        CHECK(keys.size() == 2);
        CHECK(Holds(keys, "eso:a"));
        CHECK(Holds(keys, "eso:b"));
        // ⚠ Short circuited on purpose. CHECK does not abort, so an indexing
        // assertion against an empty vector would read past the end and kill
        // the process with 0xC0000005 and no output at all.
        CHECK(!keys.empty() && keys[0] == "eso:a");  // sorted: a stable report

        // ⚠ A REJECTED KEY IS STILL A KEY. It became kNever rather than
        // disappearing, so it is still something an author wrote and still
        // has to be matched against the palette. An orphan that LOCKS is worth
        // naming too, and skipping rejects here would hide it.
        Json::Value broken;
        Json::Value nope;
        nope["type"] = "horoscope";
        Json::Value arr(Json::arrayValue);
        arr.append(nope);
        broken["dyes"]["eso:bad"] = arr;
        CHECK(rules.MergeFromJson(broken).rejected == 1);
        CHECK(rules.DyeKeys().size() == 3);
        CHECK(Holds(rules.DyeKeys(), "eso:bad"));
    }

    {  // rejects accumulate across BOTH sections in one merge
        Json::Value root;
        Json::Value broken(Json::arrayValue);
        Json::Value nope;
        nope["type"] = "horoscope";
        broken.append(nope);
        root["tiers"]["Rare"]     = broken;
        root["dyes"]["eso:bad"]   = broken;

        DyeRuleSet rules;
        const auto merged = rules.MergeFromJson(root);
        CHECK(merged.rejected == 2);
        CHECK(merged.rejectedKeys.size() == 2);
        CHECK(merged.SawSection());

        // ⚠ WHICH keys, not just how many. A size of 2 is also what
        // {"Rare","Rare"} gives, so the count alone passes even if the dyes
        // section never reported and half the locks are unnamed in the log.
        // Naming them is the entire reason rejectedKeys exists.
        CHECK(Holds(merged.rejectedKeys, "Rare"));
        CHECK(Holds(merged.rejectedKeys, "eso:bad"));
    }

    {  // ⚠ A file that parses and contributes NOTHING is the dangerous shape,
       // because it looks exactly like success and leaves every dye free.
       // Load turns each of these into a FILE failure so promotion freezes.
        using Section = DyeRuleSet::MergeResult::Section;
        DyeRuleSet rules;

        // root is a JSON array: someone wrapped the object
        const auto notObject = rules.MergeFromJson(Json::Value(Json::arrayValue));
        CHECK(notObject.rejected == 0);
        CHECK(!notObject.SawSection());
        CHECK(notObject.tiersState == Section::kAbsent);
        CHECK(notObject.dyesState == Section::kAbsent);

        // root is a string
        CHECK(!rules.MergeFromJson(Json::Value("nope")).SawSection());

        // an object with neither section
        CHECK(!rules.MergeFromJson(Json::Value(Json::objectValue)).SawSection());

        // an object whose section key is MISSPELLED, with nothing else in it
        Json::Value typo;
        typo["dye"]["eso:x"] = Json::Value(Json::arrayValue);
        const auto typoRes   = rules.MergeFromJson(typo);
        CHECK(!typoRes.SawSection());
        CHECK(typoRes.unknownKeys.size() == 1);
        CHECK(Holds(typoRes.unknownKeys, "dye"));

        // ⚠ AN ABSENT SECTION IS NOT A MIS-EDIT. A file that only defines
        // tiers, or only defines dyes, is legitimate and must stay clean. This
        // is the case every check below has to avoid catching, and it is why
        // the misspelled key needs its own evidence rather than being inferred
        // from a missing section.
        Json::Value tiersOnly;
        tiersOnly["tiers"]["Common"] = Json::Value(Json::arrayValue);
        const auto tiersOnlyRes      = rules.MergeFromJson(tiersOnly);
        CHECK(tiersOnlyRes.SawSection());
        CHECK(tiersOnlyRes.tiersState == Section::kObject);
        CHECK(tiersOnlyRes.dyesState == Section::kAbsent);
        CHECK(tiersOnlyRes.unknownKeys.empty());

        // ⚠ A WRONG-TYPED SECTION, WITH ITS SIBLING VALID. This shape is the
        // whole point and both of these cases used to be written with the
        // other section ABSENT, so they proved nothing: sawSection was false
        // either way and they merely re-ran the "an object with neither
        // section" case above. A real mis-edit happens to a real file, and
        // every real file has both sections.
        Json::Value wrongType;
        wrongType["tiers"]["Common"] = Json::Value(Json::arrayValue);
        wrongType["dyes"]            = "oops";
        const auto wrongTypeRes      = rules.MergeFromJson(wrongType);
        CHECK(wrongTypeRes.SawSection());   // the healthy sibling: NOT a pass
        CHECK(wrongTypeRes.dyesState == Section::kWrongType);
        CHECK(wrongTypeRes.tiersState == Section::kObject);

        // the same on the TIERS side, which is where the blast radius is: one
        // bad tier key covers a whole rarity
        Json::Value wrongTierType;
        wrongTierType["tiers"]         = 7;
        wrongTierType["dyes"]["eso:x"] = Json::Value(Json::arrayValue);
        const auto wrongTierRes        = rules.MergeFromJson(wrongTierType);
        CHECK(wrongTierRes.SawSection());
        CHECK(wrongTierRes.tiersState == Section::kWrongType);
        CHECK(wrongTierRes.dyesState == Section::kObject);

        // ⚠ A LITERAL null SECTION IS A MIS-EDIT, NOT AN ABSENCE. jsoncpp's
        // operator[] answers with the same null value for both, so this is
        // decided by isMember and would silently become kAbsent if anyone
        // swapped that for an isNull check.
        Json::Value nullSection;
        nullSection["tiers"]["Common"] = Json::Value(Json::arrayValue);
        nullSection["dyes"]           = Json::Value(Json::nullValue);
        const auto nullRes            = rules.MergeFromJson(nullSection);
        CHECK(nullRes.SawSection());
        CHECK(nullRes.dyesState == Section::kWrongType);

        // both sections wrong: reported once, by both states
        Json::Value bothWrong;
        bothWrong["tiers"]    = 7;
        bothWrong["dyes"]     = "oops";
        const auto bothRes    = rules.MergeFromJson(bothWrong);
        CHECK(!bothRes.SawSection());
        CHECK(bothRes.tiersState == Section::kWrongType);
        CHECK(bothRes.dyesState == Section::kWrongType);

        // an EMPTY but present section DOES count: the author opted in and
        // deliberately listed nothing, which is not the same as a mis-edit
        Json::Value emptySection;
        emptySection["dyes"]   = Json::Value(Json::objectValue);
        const auto emptyRes    = rules.MergeFromJson(emptySection);
        CHECK(emptyRes.SawSection());
        CHECK(emptyRes.dyesState == Section::kObject);

        CHECK(rules.For("eso:anything", "Rare").empty());
    }

    {  // The positive control, and every assertion below is only worth
       // anything against it: the shipped two-section shape loads clean and
       // master-gold stays locked at level 1.
        const Sandbox box("valid");
        CHECK(box.Ok());   // a sandbox that did not stand proves nothing
        box.Write("eso.json", std::string("{") + kTiers + "," + kDyes + "}");
        const auto report = DyeRules::Load();
        CHECK(report.filesScanned == 1);
        CHECK(report.filesFailed == 0);
        CHECK(DyeRules::Healthy());
        CHECK(!GoldIsFree());

        // ⚠ The pair, under one lock. Two acquisitions can straddle a Load.
        const auto checked = DyeRules::SnapshotChecked();
        CHECK(checked.healthy);
        CHECK(checked.rules.DyeCount() == 1);
        CHECK(checked.rules.TierCount() == 2);
    }

    {  // ⚠ PLAN TASK 8, MANUAL STEP 14, PINNED. Rename "dyes" to "dye". Valid
       // JSON, plausible, and it must FREEZE exactly like a stray comma.
       //
       // This is the case the whole round was about. It is invisible to the
       // section states, because the rename leaves dyes ABSENT and tiers a
       // healthy object, which is character for character a legitimate
       // tiers-only file. Before the fix this reported scanned=1 failed=0
       // healthy=TRUE, dropped every per-dye override, and dropped master-gold
       // onto the Common tier, which is [{"type":"always"}]: free at level 1
       // and written permanently into the co-save.
        const Sandbox box("misspelled-section");
        CHECK(box.Ok());
        box.Write("eso.json",
                  std::string("{") + kTiers +
                      R"(,"dye":{"eso:master-gold":[{"type":"level","min":50}]}})");
        const auto report = DyeRules::Load();
        CHECK(report.filesScanned == 1);
        CHECK(report.filesFailed == 1);
        CHECK(!DyeRules::Healthy());
        // ⚠ The freeze is what protects the player, NOT the resolution. The
        // rules really are gone, so master-gold really does resolve free here;
        // promotion refuses to run at all because filesFailed is non-zero.
        CHECK(GoldIsFree());
    }

    {  // a wrong TYPE on one section, with the other healthy. The healthy
       // sibling used to answer for both and the file loaded clean.
        const Sandbox box("wrong-typed-section");
        CHECK(box.Ok());
        box.Write("eso.json", std::string("{") + kTiers + R"(,"dyes":"oops"})");
        const auto report = DyeRules::Load();
        CHECK(report.filesScanned == 1);
        CHECK(report.filesFailed == 1);
        CHECK(!DyeRules::Healthy());
    }

    {  // ⚠ AN ENTRY NAMED LIKE A RULES FILE THAT IS NOT ONE. An archive that
       // extracted eso.json as a FOLDER used to be skipped in silence:
       // scanned=0 failed=0 healthy=TRUE, and the whole palette free.
        const Sandbox   box("json-is-a-folder");
        CHECK(box.Ok());
        std::error_code ec;
        fs::create_directories(box.Unlocks() / "eso.json", ec);
        const auto report = DyeRules::Load();
        CHECK(report.filesFailed == 1);
        CHECK(!DyeRules::Healthy());
    }

    {  // ⚠ A DANGLING LINK NAMED LIKE A RULES FILE. is_regular_file cannot
       // follow it and sets its error_code, which was collected and thrown
       // away. Reachable without elevation, and mod managers deploy by
       // linking, so this is an ordinary broken install rather than an exotic
       // one.
        const Sandbox   box("dangling-json");
        CHECK(box.Ok());
        std::error_code ec;
        fs::create_directories(box.Root() / "gone", ec);
        const bool made =
            MakeJunction(box.Unlocks() / "eso.json", box.Root() / "gone");
        CHECK(made);   // a case that could not be set up has proved nothing
        fs::remove_all(box.Root() / "gone", ec);
        if (made) {
            const auto report = DyeRules::Load();
            CHECK(report.filesFailed == 1);
            CHECK(!DyeRules::Healthy());
            fs::remove(box.Unlocks() / "eso.json", ec);
        }
    }

    {  // ⚠ AND THE POLICY THAT GOES WITH IT, stated so it is a decision rather
       // than an accident: an unreadable entry that is NOT named .json is
       // skipped in silence. Unlocks sits in Data, where MO2 leaves meta.ini
       // and users leave notes and backup folders, and none of those can free
       // a dye. Freezing over an unreadable readme would spend the one gate
       // that has to keep meaning something on a false alarm.
        const Sandbox   box("stray-entries");
        CHECK(box.Ok());
        std::error_code ec;
        box.Write("eso.json", std::string("{") + kTiers + "," + kDyes + "}");
        box.Write("meta.ini", "[General]\n");
        fs::create_directories(box.Unlocks() / "backup", ec);
        fs::create_directories(box.Root() / "gone", ec);
        const bool made =
            MakeJunction(box.Unlocks() / "readme.txt", box.Root() / "gone");
        CHECK(made);
        fs::remove_all(box.Root() / "gone", ec);
        if (made) {
            const auto report = DyeRules::Load();
            CHECK(report.filesScanned == 1);
            CHECK(report.filesFailed == 0);
            CHECK(DyeRules::Healthy());
            CHECK(!GoldIsFree());
            fs::remove(box.Unlocks() / "readme.txt", ec);
        }
    }

    {  // ⚠ NO RULES AT ALL IS NOT AN ERROR, and must not become one. An absent
       // Unlocks means everything free and nothing warns, deliberately, and it
       // is what the check below has to keep telling apart from a broken link.
        const Sandbox box("no-unlocks-dir", false);
        // ⚠ THE ONE THAT COULD PASS VACUOUSLY, and the reason Ok() exists at
        // all. This case expects scanned=0 failed=0 healthy=TRUE, which is
        // character for character what a sandbox that never got built produces:
        // Load runs against whatever directory the process was already in, and
        // that has no Unlocks either.
        CHECK(box.Ok());
        const auto    report = DyeRules::Load();
        CHECK(report.filesScanned == 0);
        CHECK(report.filesFailed == 0);
        CHECK(DyeRules::Healthy());
    }

    {  // ⚠ A DANGLING LINK ON UNLOCKS ITSELF IS A BROKEN INSTALL, NOT AN
       // UNINSTALLED ONE. exists() follows the link through to its missing
       // target and answers false with its error code CLEAR, which is byte for
       // byte the answer the case above gets. symlink_status does not follow
       // it, and that is the only thing keeping the two apart.
        const Sandbox   box("dangling-unlocks", false);
        CHECK(box.Ok());
        std::error_code ec;
        fs::create_directories(box.Root() / "real_unlocks", ec);
        {
            std::ofstream out(box.Root() / "real_unlocks" / "eso.json");
            out << "{" << kTiers << "," << kDyes << "}";
        }
        const bool made = MakeJunction(box.Unlocks(), box.Root() / "real_unlocks");
        CHECK(made);
        if (made) {
            // live first, so the failure below is the target going away and
            // not the link never having worked
            const auto live = DyeRules::Load();
            CHECK(live.filesScanned == 1);
            CHECK(live.filesFailed == 0);
            CHECK(!GoldIsFree());

            fs::remove_all(box.Root() / "real_unlocks", ec);
            const auto dangling = DyeRules::Load();
            CHECK(dangling.filesScanned == 0);
            CHECK(dangling.filesFailed == 1);
            CHECK(!DyeRules::Healthy());
            fs::remove(box.Unlocks(), ec);
        }
    }

    // ⚠ THE FOUR READER SHAPES, and they are the only thing standing between a
    // future "simplify the reader setup" edit and a repeat of the sixth defect.
    //
    // All four parse CLEAN under jsoncpp's defaults, and all four lose a whole
    // section on the way. None of them is visible to the closed schema: in the
    // first two the lost content never becomes JSON at all, so it can never
    // turn up in getMemberNames() and unknownKeys stays empty; in the last two
    // the parser collapses the duplicate keys before MergeFromJson is called,
    // so MergeResult sees one well formed section that happens to be the wrong
    // one. Load's reader flags are the only layer that can see any of them.
    //
    // Measured before the fix, on the shipped two-section shape: every one
    // reported scanned=1 failed=0 healthy=TRUE.
    {  // an extra "}" closes the root early: the rest of the file is trailing
       // text the parser silently discards, so "dyes" is simply gone
        const Sandbox box("extra-brace");
        CHECK(box.Ok());
        box.Write("eso.json", std::string("{") + kTiers + "}," + kDyes + "}");
        const auto report = DyeRules::Load();
        CHECK(report.filesScanned == 1);
        CHECK(report.filesFailed == 1);
        CHECK(!DyeRules::Healthy());
    }

    {  // two concatenated objects: only the first ever becomes JSON
        const Sandbox box("concatenated-objects");
        CHECK(box.Ok());
        box.Write("eso.json", std::string("{") + kTiers + "}{" + kDyes + "}");
        const auto report = DyeRules::Load();
        CHECK(report.filesScanned == 1);
        CHECK(report.filesFailed == 1);
        CHECK(!DyeRules::Healthy());
    }

    {  // a duplicated "dyes": the LAST one wins and the first block's entries
       // are dropped, which is how master-gold loses its override
        const Sandbox box("duplicate-dyes");
        CHECK(box.Ok());
        box.Write("eso.json",
                  std::string("{") + kTiers + "," + kDyes +
                      R"(,"dyes":{"eso:other":[{"type":"always"}]}})");
        const auto report = DyeRules::Load();
        CHECK(report.filesScanned == 1);
        CHECK(report.filesFailed == 1);
        CHECK(!DyeRules::Healthy());
    }

    {  // ⚠ A DUPLICATED "tiers", which is the one that actually ships. The tier
       // block is the most copy pasted thing in a rules file, and the LAST one
       // replacing the whole table takes every rarity gated colour with it,
       // 277 of the 306.
        const Sandbox box("duplicate-tiers");
        CHECK(box.Ok());
        box.Write("eso.json",
                  std::string("{") + kTiers + R"(,"tiers":{},)" + kDyes + "}");
        const auto report = DyeRules::Load();
        CHECK(report.filesScanned == 1);
        CHECK(report.filesFailed == 1);
        CHECK(!DyeRules::Healthy());
    }

    {  // ⚠ AND THE CONTROLS, which are the half that keeps the fix honest. A
       // stricter reader that froze any of these would be a worse bug than the
       // one it fixed, because a frozen palette is what the whole gate spends
       // its credibility on.
       //
       // The trailing comma is here deliberately and is NOT rejected: it parses
       // to exactly the object the author meant, so it cannot free a single
       // dye, and freezing over it would be strictness with no safety value.
        struct Clean {
            const char* name;
            std::string body;
        };
        const std::string t = kTiers;
        const std::string d = kDyes;
        const Clean       cases[] = {
            { "tiers-only", "{" + t + "}" },
            { "dyes-only", "{" + d + "}" },
            { "trailing-newline", "{" + t + "," + d + "}\n" },
            { "trailing-crlf-spaces", "{" + t + "," + d + "}\r\n   \r\n" },
            { "utf8-bom", "\xEF\xBB\xBF{" + t + "," + d + "}" },
            { "trailing-comma",
              R"({"tiers":{"Common":[{"type":"always"}],)"
              R"("Uncommon":[{"type":"level","min":20}],},)" + d + "}" },
            { "leading-line-comment", "// notes\n{" + t + "," + d + "}" },
            // ⚠ THE SHAPE THE SHIPPED FILE ACTUALLY HAS, which the single line
            // case above only nearly covers. dist/.../Unlocks/eso.json opens
            // with a long header of consecutive // lines, several of them
            // bare, because the closed schema refuses a "_comment" key
            // and the reasons that file has to carry are worth more than the
            // brevity. If a stricter reader ever stopped skipping past a RUN
            // of them, the rules would fail to load and every player's
            // progression would freeze on the shipped configuration.
            //
            // ⚠ THE COUNTS ARE THE POINT, so they are built rather than typed.
            // This case was four lines with two bare ones while the commit that
            // added it said thirty six with five, which is a test that agrees
            // with a claim nobody can check against it. Header() below is
            // exactly that shape, and DyePromotionTests loads the real file
            // itself, so the synthetic case and the shipped bytes are now
            // covered separately rather than one standing in for the other.
            { "leading-line-comment-block",
              Header(kHeaderLines, kHeaderBare) + "{" + t + "," + d + "}" },
            { "inner-line-comment", "{" + t + ",\n// overrides\n" + d + "}" },
            { "inner-block-comment", "{" + t + ",\n/* overrides */\n" + d + "}" },
            { "trailing-line-comment", "{" + t + "," + d + "}\n// end\n" },
            { "trailing-block-comment", "{" + t + "," + d + "}\n/* end */\n" },
        };
        // ⚠ The header case is only worth what its counts are worth, so they
        // are read back OFF THE CASE ITSELF rather than off a second call to
        // Header. A first version of this asserted Header(36, 5) beside a case
        // built from Header(4, 2) and passed, which is the very mistake the
        // case exists to stop being made in prose.
        {
            const auto& body = cases[7].body;   // "leading-line-comment-block"
            CHECK(std::string_view(cases[7].name) == "leading-line-comment-block");
            const auto  run  = body.substr(0, body.find('{'));
            CHECK(static_cast<std::size_t>(std::ranges::count(run, '\n')) ==
                  kHeaderLines);
            std::size_t bare = 0, at = 0;
            while ((at = run.find("//\n", at)) != std::string::npos) {
                ++bare;
                at += 3;
            }
            CHECK(bare == kHeaderBare);
            // A run long enough and bare enough to be the shape this exists to
            // pin, whatever the shipped header grows to.
            CHECK(kHeaderLines >= 30 && kHeaderBare >= 4);
        }
        for (const auto& c : cases) {
            const Sandbox box(c.name);
            CHECK(box.Ok());
            box.Write("eso.json", c.body);
            const auto report = DyeRules::Load();
            if (report.filesFailed != 0) {
                std::printf("FAIL clean shape rejected: %s\n", c.name);
                ++g_failures;
                for (const auto& why : report.failures) {
                    std::printf("     %s\n", why.c_str());
                }
            }
            CHECK(report.filesScanned == 1);
            CHECK(DyeRules::Healthy());
        }
    }

    {  // ⚠ A PRESENT BUT EMPTY SECTION IS NOT A FAILURE, and must not become
       // one. Deliberately listing nothing is a legitimate way to opt into the
       // format, and freezing a player's whole palette over it would be the
       // same over-reaction as freezing over an unreadable readme.
       //
       // It DOES leave every dye that section covered free, and it looks
       // identical to a section a bad edit emptied, so it is said out loud. The
       // recurring problem in this module is silence, not noise.
        const Sandbox box("empty-sections");
        CHECK(box.Ok());
        box.Write("eso.json", R"({"tiers":{},"dyes":{}})");
        const auto report = DyeRules::Load();
        CHECK(report.filesScanned == 1);
        CHECK(report.filesFailed == 0);
        CHECK(DyeRules::Healthy());
        CHECK(report.notes.size() == 2);
        CHECK(Holds(report.notes,
                    "eso.json: \"tiers\" is present and empty, no tier rules "
                    "from this file"));
        CHECK(Holds(report.notes,
                    "eso.json: \"dyes\" is present and empty, no dye rules "
                    "from this file"));
        // and everything really is free, which is what the note is warning
        // about rather than hiding
        CHECK(GoldIsFree());
    }

    {  // ⚠ AN EMPTY RULE IS FREE AND WAS REPORTED NOWHERE. The empty SECTION
       // above gets a note; one empty key inside a healthy section frees every
       // dye it covers just as completely and said nothing at all. On a tier
       // that is a whole rarity in one line.
       //
       // A NOTE and never a failure: [] is the documented spelling for
       // "deliberately free", so warning would be wrong and freezing would be
       // the same over-reaction as freezing over an unreadable readme.
        const Sandbox box("empty-rules");
        CHECK(box.Ok());
        box.Write(
            "eso.json",
            R"({"tiers":{"Common":[],"Uncommon":[{"type":"level","min":20}]},)"
            R"("dyes":{"eso:master-gold":[]}})");
        const auto report = DyeRules::Load();
        CHECK(report.filesScanned == 1);
        CHECK(report.filesFailed == 0);
        CHECK(DyeRules::Healthy());
        CHECK(report.notes.size() == 2);
        CHECK(Holds(report.notes,
                    "eso.json: tier \"Common\" is present and empty, so "
                    "nothing gates it"));
        CHECK(Holds(report.notes,
                    "eso.json: dye \"eso:master-gold\" is present and empty, "
                    "so nothing gates it"));
        // and it really is free, which is the consequence the note is about
        CHECK(GoldIsFree());
    }

    {  // a healthy file says NOTHING. A note on every ordinary load would be
       // noise, and noise is how a real warning stops being read.
        const Sandbox box("no-notes-when-healthy");
        CHECK(box.Ok());
        box.Write("eso.json", std::string("{") + kTiers + "," + kDyes + "}");
        const auto report = DyeRules::Load();
        CHECK(report.filesFailed == 0);
        CHECK(report.notes.empty());
    }

    {  // a FAILED file gets its failure and no note. The empty-section note is
       // for a file that loaded, and stacking both on one file would leave the
       // user reading past the only line that matters.
        const Sandbox box("no-notes-when-failed");
        CHECK(box.Ok());
        box.Write("eso.json", std::string("{") + kTiers + R"(,"dyes":"oops"})");
        const auto report = DyeRules::Load();
        CHECK(report.filesFailed == 1);
        CHECK(report.notes.empty());
    }

    {   // ⚠ The parent of every sandbox, which each case's own destructor never
        // touched. It was left in %TEMP% forever, one directory per run.
        std::error_code ec;
        const auto      parent = SandboxParent(ec);
        if (!ec) {
            fs::remove_all(parent, ec);
            CHECK(!fs::exists(parent, ec));
        }
    }

    if (g_failures == 0) {
        std::printf("DyeRulesTests: all passed\n");
        return 0;
    }
    std::printf("DyeRulesTests: %d failure(s)\n", g_failures);
    return 1;
}
