// Dye condition tests. No SKSE, no engine: whether a character has earned a
// colour is arithmetic over a snapshot of facts about them.
#include "DyeConditions.h"

#include <json/json.h>

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

using namespace OS;

int main() {
    // ---- locationCleared ---------------------------------------------------
    // ⚠ CLEARED IS A SET MEMBERSHIP AND FAILS CLOSED. A location the bridge
    // could not resolve is simply absent, and absent must read as not cleared:
    // unknown never passes for done, which is the same rule the quest arm keeps.
    {
        using namespace OS;
        DyeWorldState world;
        world.locationsCleared.insert(QuestRef{ "Skyrim.esm", 0x018EF7u });

        DyeCondition cleared;
        cleared.kind     = DyeCondKind::kLocationCleared;
        cleared.location = QuestRef{ "Skyrim.esm", 0x018EF7u };
        CHECK(Satisfied(cleared, world));

        // A different location in the same plugin is a different lock.
        DyeCondition other;
        other.kind     = DyeCondKind::kLocationCleared;
        other.location = QuestRef{ "Skyrim.esm", 0x018EF8u };
        CHECK(!Satisfied(other, world));

        // The same local id in another plugin is also a different lock, which
        // is the whole reason the ref carries a plugin.
        DyeCondition elsewhere;
        elsewhere.kind     = DyeCondKind::kLocationCleared;
        elsewhere.location = QuestRef{ "Dawnguard.esm", 0x018EF7u };
        CHECK(!Satisfied(elsewhere, world));

        // ⚠ A CLEARED LOCATION IS NOT A COMPLETED QUEST. The two sets are
        // separate, so a rule naming one must never be satisfied by the other.
        DyeCondition asQuest;
        asQuest.kind  = DyeCondKind::kQuest;
        asQuest.quest = QuestRef{ "Skyrim.esm", 0x018EF7u };
        CHECK(!Satisfied(asQuest, world));
    }

    {  // a world with a level 20 character who has maxed Smithing, finished one
       // quest and dyed nine channels
        DyeWorldState w;
        w.level             = 20;
        w.skills[10]        = 100;  // kSmithing
        w.skills[23]        = 42;   // kEnchanting
        w.questsDone.insert(QuestRef{ "Skyrim.esm", 0x0002A12F });
        w.deeds["channelsDyed"] = 9;

        DyeCondition always;
        CHECK(Satisfied(always, w));  // kAlways is the default

        // kNever is what a rejected rule becomes. Nothing satisfies it, and
        // that is the whole point: a rule the loader could not parse must LOCK
        // its dye, because unlocks are sticky and wrongly freed is permanent.
        DyeCondition never;
        never.kind = DyeCondKind::kNever;
        CHECK(!Satisfied(never, w));

        DyeCondition lvl;
        lvl.kind = DyeCondKind::kLevel;
        lvl.min  = 20;
        CHECK(Satisfied(lvl, w));  // at the threshold counts
        lvl.min = 21;
        CHECK(!Satisfied(lvl, w));

        DyeCondition smith;
        smith.kind  = DyeCondKind::kSkill;
        smith.skill = 10;
        smith.min   = 100;
        CHECK(Satisfied(smith, w));
        smith.min = 101;
        CHECK(!Satisfied(smith, w));

        // A skill the world never recorded reads as zero rather than as absent,
        // so a rule naming a skill we forgot to gather fails closed.
        DyeCondition missing;
        missing.kind  = DyeCondKind::kSkill;
        missing.skill = 18;  // kAlteration, never set above
        missing.min   = 1;
        CHECK(!Satisfied(missing, w));

        DyeCondition quest;
        quest.kind  = DyeCondKind::kQuest;
        quest.quest = QuestRef{ "Skyrim.esm", 0x0002A12F };
        CHECK(Satisfied(quest, w));
        quest.quest.formId = 0x0002A130;
        CHECK(!Satisfied(quest, w));
        // Same form id, different plugin, is a different quest. The pair is the
        // identity; neither half is.
        quest.quest = QuestRef{ "Dawnguard.esm", 0x0002A12F };
        CHECK(!Satisfied(quest, w));

        DyeCondition deed;
        deed.kind = DyeCondKind::kDeed;
        deed.deed = "channelsDyed";
        deed.min  = 9;
        CHECK(Satisfied(deed, w));
        deed.min = 10;
        CHECK(!Satisfied(deed, w));

        // An unknown deed is zero, same reasoning as an ungathered skill.
        DyeCondition unknownDeed;
        unknownDeed.kind = DyeCondKind::kDeed;
        unknownDeed.deed = "dragonsFlossed";
        unknownDeed.min  = 1;
        CHECK(!Satisfied(unknownDeed, w));
    }

    {  // an array is an AND, and an empty array is satisfied: no rule means no
       // obstacle, which is what makes "always" the fallback for a pack that
       // never opted into an economy
        DyeWorldState w;
        w.level = 30;

        std::vector<DyeCondition> none;
        CHECK(AllSatisfied(none, w));

        DyeCondition lo;
        lo.kind = DyeCondKind::kLevel;
        lo.min  = 10;
        DyeCondition hi;
        hi.kind = DyeCondKind::kLevel;
        hi.min  = 50;

        CHECK(AllSatisfied({ lo }, w));
        CHECK(!AllSatisfied({ lo, hi }, w));
    }

    {  // SkillByName covers all eighteen and refuses anything else
        CHECK(SkillByName("OneHanded") == 6u);    // first of the block
        CHECK(SkillByName("Enchanting") == 23u);  // last of the block
        CHECK(SkillByName("Archery") == 8u);      // NOT Marksman
        CHECK(SkillByName("Speech") == 17u);      // NOT Speechcraft
        CHECK(!SkillByName("Marksman").has_value());
        CHECK(!SkillByName("smithing").has_value());  // case sensitive
        CHECK(!SkillByName("").has_value());
    }

    {  // parsing: every kind round trips from the JSON a rules file uses
        Json::Value j;
        j["type"] = "level";
        j["min"]  = 15;
        DyeCondition c;
        CHECK(ConditionFromJson(j, c));
        CHECK(c.kind == DyeCondKind::kLevel);
        CHECK(c.min == 15u);

        Json::Value s;
        s["type"]  = "skill";
        s["skill"] = "Smithing";
        s["min"]   = 100;
        CHECK(ConditionFromJson(s, c));
        CHECK(c.kind == DyeCondKind::kSkill);
        CHECK(c.skill == 10u);
        CHECK(c.min == 100u);

        Json::Value q;
        q["type"]   = "quest";
        q["plugin"] = "Skyrim.esm";
        q["formId"] = "0002A12F";     // a STRING: hex, and jsoncpp has no hex int
        CHECK(ConditionFromJson(q, c));
        CHECK(c.kind == DyeCondKind::kQuest);
        CHECK(c.quest.plugin == "Skyrim.esm");
        CHECK(c.quest.formId == 0x0002A12Fu);

        // lowercase, an 0x prefix, and both together. The header offers
        // 0x2a12f as an authoring example, so it has to actually work.
        q["formId"] = "0002a12f";
        CHECK(ConditionFromJson(q, c));
        CHECK(c.quest.formId == 0x0002A12Fu);
        q["formId"] = "0x2A12F";
        CHECK(ConditionFromJson(q, c));
        CHECK(c.quest.formId == 0x0002A12Fu);
        q["formId"] = "0x0002a12f";
        CHECK(ConditionFromJson(q, c));
        CHECK(c.quest.formId == 0x0002A12Fu);

        Json::Value d;
        d["type"] = "deed";
        d["deed"] = "channelsDyed";
        d["min"]  = 25;
        CHECK(ConditionFromJson(d, c));
        CHECK(c.kind == DyeCondKind::kDeed);
        CHECK(c.deed == "channelsDyed");

        Json::Value a;
        a["type"] = "always";
        CHECK(ConditionFromJson(a, c));
        CHECK(c.kind == DyeCondKind::kAlways);
    }

    {  // untrusted input is DROPPED, never guessed. A rule that half parses
       // would silently lock or unlock a colour, and both are worse than
       // refusing the entry.
        DyeCondition c;

        Json::Value noType;
        noType["min"] = 5;
        CHECK(!ConditionFromJson(noType, c));

        Json::Value badType;
        badType["type"] = "horoscope";
        CHECK(!ConditionFromJson(badType, c));

        Json::Value badSkill;
        badSkill["type"]  = "skill";
        badSkill["skill"] = "Bartering";   // not a Skyrim skill
        badSkill["min"]   = 50;
        CHECK(!ConditionFromJson(badSkill, c));

        Json::Value badHex;
        badHex["type"]   = "quest";
        badHex["plugin"] = "Skyrim.esm";
        badHex["formId"] = "zzzz";
        CHECK(!ConditionFromJson(badHex, c));

        Json::Value noPlugin;
        noPlugin["type"]   = "quest";
        noPlugin["formId"] = "0002A12F";
        CHECK(!ConditionFromJson(noPlugin, c));

        Json::Value noDeed;
        noDeed["type"] = "deed";
        noDeed["min"]  = 3;
        CHECK(!ConditionFromJson(noDeed, c));

        // Present but EMPTY is not the same as absent, and both are refused.
        Json::Value emptyPlugin;
        emptyPlugin["type"]   = "quest";
        emptyPlugin["plugin"] = "";
        emptyPlugin["formId"] = "0002A12F";
        CHECK(!ConditionFromJson(emptyPlugin, c));

        Json::Value emptyDeed;
        emptyDeed["type"] = "deed";
        emptyDeed["deed"] = "";
        emptyDeed["min"]  = 3;
        CHECK(!ConditionFromJson(emptyDeed, c));

        // A hex string too long even after the prefix is stripped.
        Json::Value longHex;
        longHex["type"]   = "quest";
        longHex["plugin"] = "Skyrim.esm";
        longHex["formId"] = "0x0002A12FF";
        CHECK(!ConditionFromJson(longHex, c));

        // ⚠ "never" is refused from a FILE, so the loader is its only
        // producer. That does not withhold the lock, since any unparseable
        // clause reaches kNever anyway; it makes every lock arrive counted and
        // logged rather than through one silently privileged spelling.
        Json::Value never;
        never["type"] = "never";
        CHECK(!ConditionFromJson(never, c));

        CHECK(!ConditionFromJson(Json::Value("level"), c));  // not an object
    }

    {  // a bad entry drops itself and takes the whole RULE with it. This is the
       // one place a partial parse is not survivable: half of an AND is a
       // weaker rule than was written, so a typo would hand out a rare colour.
        Json::Value arr(Json::arrayValue);
        Json::Value good;
        good["type"] = "level";
        good["min"]  = 10;
        Json::Value bad;
        bad["type"] = "nonsense";
        arr.append(good);
        arr.append(bad);

        std::vector<DyeCondition> out;
        CHECK(!ConditionsFromJson(arr, out));
        CHECK(out.empty());

        Json::Value allGood(Json::arrayValue);
        allGood.append(good);
        CHECK(ConditionsFromJson(allGood, out));
        CHECK(out.size() == 1);

        // ⚠ An EMPTY array parses and yields an empty rule, which AllSatisfied
        // reads as satisfied, which means FREE. That is a load bearing
        // semantic: "dyes": { "eso:foo": [] } is how a rules file says "this
        // one is deliberately free". Pinned here because nothing else proves
        // it and a later refactor could quietly make it a rejection.
        Json::Value emptyArr(Json::arrayValue);
        CHECK(ConditionsFromJson(emptyArr, out));
        CHECK(out.empty());
        CHECK(AllSatisfied(out, DyeWorldState{}));

        // Not an array at all is refused, and CLEARS whatever the caller had.
        // ⚠ That differs from ConditionFromJson, which leaves its out-param
        // alone on failure. Two functions one letter apart with opposite
        // contracts, both documented in the header; pinned here so neither
        // drifts onto the other's behaviour unnoticed.
        out.push_back(DyeCondition{});
        CHECK(out.size() == 1);
        CHECK(!ConditionsFromJson(Json::Value("nope"), out));
        CHECK(out.empty());
    }

    if (g_failures == 0) {
        std::printf("DyeConditionsTests: all passed\n");
        return 0;
    }
    std::printf("DyeConditionsTests: %d failure(s)\n", g_failures);
    return 1;
}
