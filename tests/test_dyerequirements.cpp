// Dye requirement tests. No SKSE, no engine, no ImGui: turning a rule and a
// world reading into "what it takes, what you have, whether that is enough" is
// arithmetic over two plain structs.
//
// The one thing this suite exists to pin down is that kNever does NOT come out
// the other side as a requirement. It is what the loader substitutes for a rule
// it could not parse, so drawing it as an ordinary locked swatch would send the
// player off to earn something that does not exist.
#include "DyeRequirements.h"

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

using namespace OS;

// ⚠ NEVER INDEX r.clauses DIRECTLY. A CHECK that reaches past the end of an
// empty vector takes the whole executable down with 0xC0000005, which prints
// NOTHING: the suite then looks like one that was never built rather than one
// that failed, and this branch has already lost time to exactly that twice.
// Proven again here on the very first run of this suite, against the
// unimplemented stub. Out of range hands back a default clause, so every
// assertion below reports a FAIL and the run finishes.
static const DyeClause& Clause(const DyeRequirements& a_r, std::size_t a_i) {
    static const DyeClause kNone{};
    return a_i < a_r.clauses.size() ? a_r.clauses[a_i] : kNone;
}

// A character at level 20 who has maxed Smithing, trained Alchemy a little,
// finished one quest and dyed nine channels.
static DyeWorldState Character() {
    DyeWorldState w;
    w.level                 = 20;
    w.skills[10]            = 100;  // Smithing
    w.skills[16]            = 41;   // Alchemy
    w.questsDone.insert(QuestRef{ "Skyrim.esm", 0x0002A12F });
    w.deeds["channelsDyed"] = 9;
    return w;
}

static DyeCondition Level(std::uint32_t a_min) {
    DyeCondition c;
    c.kind = DyeCondKind::kLevel;
    c.min  = a_min;
    return c;
}

static DyeCondition Skill(std::uint32_t a_av, std::uint32_t a_min) {
    DyeCondition c;
    c.kind  = DyeCondKind::kSkill;
    c.skill = a_av;
    c.min   = a_min;
    return c;
}

static DyeCondition Deed(const char* a_name, std::uint32_t a_min) {
    DyeCondition c;
    c.kind = DyeCondKind::kDeed;
    c.deed = a_name;
    c.min  = a_min;
    return c;
}

static DyeCondition Quest(const char* a_plugin, std::uint32_t a_formId) {
    DyeCondition c;
    c.kind  = DyeCondKind::kQuest;
    c.quest = QuestRef{ a_plugin, a_formId };
    return c;
}

int main() {
    const auto world = Character();

    {  // THE INVERSE OF SkillByName, and it has to round trip for all eighteen.
       // A hand-copied second table in the display layer is a table that can
       // silently disagree with the one the parser reads, and the failure would
       // be a swatch telling the player to train the wrong skill.
        for (std::uint32_t av = kFirstSkillAV; av <= kLastSkillAV; ++av) {
            const auto name = SkillNameByAV(av);
            CHECK(!name.empty());
            const auto back = SkillByName(name);
            CHECK(back.has_value());
            CHECK(back.value_or(0) == av);
        }
        // Nothing outside the block is a skill. The parser can never produce
        // one, but a corrupt co-save or a hand-built condition can.
        CHECK(SkillNameByAV(kFirstSkillAV - 1).empty());
        CHECK(SkillNameByAV(kLastSkillAV + 1).empty());
        CHECK(SkillNameByAV(0).empty());
    }

    {  // a level clause carries the requirement AND the reading beside it
        const auto r = DescribeDyeRequirements({ Level(15) }, world);
        CHECK(!r.unreadable);
        CHECK(r.allMet);
        CHECK(r.clauses.size() == 1);
        CHECK(Clause(r, 0).kind == DyeCondKind::kLevel);
        CHECK(Clause(r, 0).required == 15);
        CHECK(Clause(r, 0).hasValue);
        CHECK(Clause(r, 0).value == 20);
        CHECK(Clause(r, 0).met);
        CHECK(Clause(r, 0).subject.empty());  // a level has no subject to name
    }

    {  // and an unmet one still reports the reading, which is the whole point:
       // "Level 35" alone reads as broken next to a character sheet
        const auto r = DescribeDyeRequirements({ Level(35) }, world);
        CHECK(!r.allMet);
        CHECK(r.clauses.size() == 1);
        CHECK(Clause(r, 0).required == 35);
        CHECK(Clause(r, 0).value == 20);
        CHECK(!Clause(r, 0).met);
    }

    {  // a skill clause NAMES the skill. Skyrim's own Skills menu shows the
       // fortified number while the rule reads the base, so the value beside
       // the requirement is what answers "why is this still grey".
        const auto r = DescribeDyeRequirements({ Skill(16, 65) }, world);
        CHECK(r.clauses.size() == 1);
        CHECK(Clause(r, 0).kind == DyeCondKind::kSkill);
        CHECK(Clause(r, 0).subject == "Alchemy");
        CHECK(Clause(r, 0).required == 65);
        CHECK(Clause(r, 0).hasValue);
        CHECK(Clause(r, 0).value == 41);
        CHECK(!Clause(r, 0).met);
        CHECK(!r.allMet);
    }

    {  // a skill the world never gathered reads as zero rather than as absent,
       // the same fail-closed answer Satisfied gives
        const auto r = DescribeDyeRequirements({ Skill(18, 1) }, world);  // Alteration
        CHECK(r.clauses.size() == 1);
        CHECK(Clause(r, 0).subject == "Alteration");
        CHECK(Clause(r, 0).hasValue);
        CHECK(Clause(r, 0).value == 0);
        CHECK(!Clause(r, 0).met);
    }

    {  // an ActorValue that is not one of the eighteen leaves the subject
       // EMPTY rather than inventing a name. The UI needs to be able to tell.
        const auto r = DescribeDyeRequirements({ Skill(99, 10) }, world);
        CHECK(r.clauses.size() == 1);
        CHECK(Clause(r, 0).subject.empty());
        CHECK(Clause(r, 0).required == 10);
        CHECK(Clause(r, 0).value == 0);
        CHECK(!Clause(r, 0).met);
    }

    {  // a deed carries its own name through, so the UI can label the one it
       // knows and still show an unknown one from a third party pack
        const auto met = DescribeDyeRequirements({ Deed("channelsDyed", 5) }, world);
        CHECK(met.clauses.size() == 1);
        CHECK(Clause(met, 0).kind == DyeCondKind::kDeed);
        CHECK(Clause(met, 0).subject == "channelsDyed");
        CHECK(Clause(met, 0).required == 5);
        CHECK(Clause(met, 0).hasValue);
        CHECK(Clause(met, 0).value == 9);
        CHECK(Clause(met, 0).met);

        const auto unmet = DescribeDyeRequirements({ Deed("channelsDyed", 25) }, world);
        CHECK(Clause(unmet, 0).value == 9);
        CHECK(!Clause(unmet, 0).met);

        // A deed nobody counts reads zero, not absent.
        const auto unknown = DescribeDyeRequirements({ Deed("dragonsSlain", 1) }, world);
        CHECK(Clause(unknown, 0).subject == "dragonsSlain");
        CHECK(Clause(unknown, 0).hasValue);
        CHECK(Clause(unknown, 0).value == 0);
        CHECK(!Clause(unknown, 0).met);
    }

    {  // A QUEST HAS NO NUMBER. This module is pure, so it cannot reach
       // TESForm to resolve a display name, and it must not pretend to: it
       // hands back the plugin and the local form id and says there is no
       // reading, so the UI writes something honest and generic.
        const auto done = DescribeDyeRequirements(
            { Quest("Skyrim.esm", 0x0002A12F) }, world);
        CHECK(done.clauses.size() == 1);
        CHECK(Clause(done, 0).kind == DyeCondKind::kQuest);
        CHECK(Clause(done, 0).subject == "Skyrim.esm");
        CHECK(Clause(done, 0).formId == 0x0002A12Fu);
        CHECK(!Clause(done, 0).hasValue);
        CHECK(Clause(done, 0).value == 0);
        CHECK(Clause(done, 0).met);
        CHECK(done.allMet);

        // Same form id in a different plugin is a different quest. The pair is
        // the identity; neither half is.
        const auto other = DescribeDyeRequirements(
            { Quest("Dawnguard.esm", 0x0002A12F) }, world);
        CHECK(!Clause(other, 0).met);
        CHECK(!other.allMet);
    }

    {  // AN ARRAY IS AN AND, so every clause is shown and each carries its own
       // verdict. Reporting only the first unmet one would leave a player
       // fixing them one reload at a time.
        const auto r = DescribeDyeRequirements(
            { Level(15), Skill(10, 100), Deed("channelsDyed", 25) }, world);
        CHECK(r.clauses.size() == 3);
        CHECK(Clause(r, 0).met);   // level 15, they are 20
        CHECK(Clause(r, 1).met);   // Smithing 100, they have 100
        CHECK(!Clause(r, 2).met);  // 25 channels, they have dyed 9
        CHECK(!r.allMet);
        // Order is the rule's own order, not sorted by verdict: it is what the
        // author wrote and what their file will show them.
        CHECK(Clause(r, 0).kind == DyeCondKind::kLevel);
        CHECK(Clause(r, 1).kind == DyeCondKind::kSkill);
        CHECK(Clause(r, 2).kind == DyeCondKind::kDeed);
    }

    {  // kALWAYS EARNS NO CLAUSE. It cannot appear on a locked dye, and drawing
       // "Always: met" as a requirement is noise on the one tooltip whose whole
       // job is to say what is missing. It still counts as satisfied.
        const auto only = DescribeDyeRequirements(
            { DyeCondition{} }, world);  // the default kind IS kAlways
        CHECK(!only.unreadable);
        CHECK(only.clauses.empty());
        CHECK(only.allMet);

        const auto mixed = DescribeDyeRequirements(
            { DyeCondition{}, Level(35) }, world);
        CHECK(mixed.clauses.size() == 1);
        CHECK(Clause(mixed, 0).kind == DyeCondKind::kLevel);
        CHECK(!mixed.allMet);
    }

    {  // no rule at all is no obstacle, the same answer AllSatisfied gives an
       // empty list. A pack that never opted into an economy lands here.
        const auto r = DescribeDyeRequirements({}, world);
        CHECK(!r.unreadable);
        CHECK(r.clauses.empty());
        CHECK(r.allMet);
    }

    {  // ⚠ kNEVER IS NOT A CONDITION AND MUST NOT RENDER AS ONE. It is what the
       // loader substitutes for a rule it could not parse, so there is nothing
       // behind it a player could ever satisfy. It gets its own result, so the
       // UI can say the fix is in the Unlocks folder instead of naming a
       // requirement that does not exist.
        DyeCondition never;
        never.kind = DyeCondKind::kNever;

        const auto r = DescribeDyeRequirements({ never }, world);
        CHECK(r.unreadable);
        CHECK(r.clauses.empty());
        CHECK(!r.allMet);
    }

    {  // and it TAKES THE WHOLE RULE with it. An array is an AND, so one
       // unparseable clause makes the rest unsatisfiable too, and listing the
       // readable half beside it would read as the only thing standing in the
       // way. Reaching the level would then change nothing and there would be
       // no line anywhere saying why.
        DyeCondition never;
        never.kind = DyeCondKind::kNever;

        const auto r = DescribeDyeRequirements({ Level(15), never }, world);
        CHECK(r.unreadable);
        CHECK(r.clauses.empty());
        CHECK(!r.allMet);

        // Either side of a readable clause, same answer.
        const auto first = DescribeDyeRequirements({ never, Level(15) }, world);
        CHECK(first.unreadable);
        CHECK(first.clauses.empty());
        CHECK(!first.allMet);
    }

    {  // allMet agrees with AllSatisfied on every shape above, because two
       // functions that answer the same question and can disagree is how a
       // swatch ends up grey with a tooltip saying everything is met.
        const std::vector<std::vector<DyeCondition>> rules{
            {},
            { DyeCondition{} },
            { Level(15) },
            { Level(35) },
            { Skill(10, 100) },
            { Skill(16, 65) },
            { Deed("channelsDyed", 5) },
            { Deed("channelsDyed", 25) },
            { Quest("Skyrim.esm", 0x0002A12F) },
            { Quest("Dawnguard.esm", 0x0002A12F) },
            { Level(15), Skill(10, 100), Deed("channelsDyed", 25) },
        };
        for (const auto& conds : rules) {
            CHECK(DescribeDyeRequirements(conds, world).allMet ==
                  AllSatisfied(conds, world));
        }
    }

    if (g_failures == 0) {
        std::printf("DyeRequirementsTests: all passed\n");
        return 0;
    }
    std::printf("DyeRequirementsTests: %d failure(s)\n", g_failures);
    return 1;
}
