#pragma once

#include <json/json.h>

#include <compare>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OS {

    // kNever is what the loader substitutes for a rule it could not parse, so a
    // broken rule LOCKS its dye instead of freeing it. See the spec: unlocks
    // are sticky, so wrongly freed is permanent and wrongly locked is one
    // reload away from fixed.
    //
    // The loader is its only producer. A rules file naming "never" is refused,
    // which does not withhold the lock (any unparseable clause reaches kNever)
    // but does keep every lock counted and logged.
    enum class DyeCondKind {
        kAlways,
        kNever,
        kLevel,
        kSkill,
        kQuest,
        kDeed,
        // ⚠ CLEARED, NOT DISCOVERED, AND THEY ARE DIFFERENT ACHIEVEMENTS.
        // Discovering a place is walking near it. Clearing it is emptying
        // it, which is the deed a colour is worth. Skyrim tracks the second
        // on the location itself.
        kLocationCleared,
    };

    // A quest, by the only identity that survives a load order change: the
    // plugin that defines it and the LOCAL form id inside that plugin.
    //
    // Two fields rather than one joined string. A joined key would only have to
    // be taken apart again by the bridge that resolves it, and the splitting
    // costs a find, two substrs and a throwing integer parse to recover what
    // was thrown away.
    struct QuestRef {
        std::string   plugin;
        std::uint32_t formId{ 0 };
        friend auto operator<=>(const QuestRef&, const QuestRef&) = default;
    };

    // One clause of an unlock rule. A flat struct rather than a variant: the
    // announced growth path (HasPerk, SpellLearned, LocationDiscovery) reuses
    // QuestRef exactly, so the struct does not actually widen by four kinds.
    //
    // ⚠ The zero value of kind is kAlways, so a default-constructed condition
    // is SATISFIED. That is the module's one fail-open default and it is
    // deliberate, because "no rule" has to mean free. Anything that means "this
    // went wrong" must say kNever explicitly.
    struct DyeCondition {
        DyeCondKind   kind{ DyeCondKind::kAlways };
        std::uint32_t min{ 0 };       // kLevel, kSkill, kDeed
        std::uint32_t skill{ 0 };     // kSkill: an RE::ActorValue as a number
        QuestRef      quest;          // kQuest
        // ⚠ ITS OWN FIELD RATHER THAN SHARING quest, THOUGH THE TYPE IS THE
        // SAME. The header above anticipated reusing QuestRef and that is
        // exactly what this does; reusing the FIELD as well would make a
        // clause that reads "quest" mean a location half the time, and every
        // site that logs or displays one would have to know the kind before
        // it could name what it is looking at.
        QuestRef      location;       // kLocationCleared
        std::string   deed;           // kDeed
    };

    // Everything a condition can ask about, gathered ONCE on the main thread.
    //
    // Deliberately holds no engine types, which is what lets every rule in this
    // module be proved without a running game. The bridge that fills it is the
    // only piece that needs Skyrim.
    struct DyeWorldState {
        std::uint32_t                                    level{ 0 };
        std::unordered_map<std::uint32_t, std::uint32_t> skills;  // ActorValue -> level
        std::set<QuestRef>                               questsDone;
        std::set<QuestRef>                               locationsCleared;
        std::unordered_map<std::string, std::uint32_t>   deeds;
    };

    // Skyrim's eighteen skills occupy a contiguous ActorValue block. Exposed so
    // the engine bridge iterates THIS module's idea of the range rather than
    // hand-copying 6 and 23 into a second file that can silently disagree.
    inline constexpr std::uint32_t kFirstSkillAV = 6;
    inline constexpr std::uint32_t kLastSkillAV  = 23;

    // The ActorValue number for a skill name, or nullopt for anything that is
    // not one of Skyrim's eighteen skills. Used by the parser so a rules file
    // says "Smithing" rather than 10.
    [[nodiscard]] std::optional<std::uint32_t> SkillByName(std::string_view a_name);

    // The other way round: the rules-file name for an ActorValue number, or an
    // EMPTY view for anything that is not one of the eighteen.
    //
    // ⚠ IT LIVES HERE BECAUSE THE TABLE DOES. A locked swatch has to name the
    // skill it is waiting on, and the display layer holds an ActorValue number
    // by then. The obvious fix is a second name table over there, which is a
    // table that can silently disagree with the one the parser reads: the two
    // are only ever compared by a human, and the failure is a swatch telling
    // the player to train the wrong skill. Both directions read the one array
    // in the .cpp, and the test round trips all eighteen.
    [[nodiscard]] std::string_view SkillNameByAV(std::uint32_t a_av);

    [[nodiscard]] bool Satisfied(const DyeCondition& a_cond,
                                 const DyeWorldState& a_world);

    // Every clause must hold. An EMPTY list is satisfied: no rule is no
    // obstacle, which is what makes a pack that never opted into an economy
    // keep working.
    [[nodiscard]] bool AllSatisfied(const std::vector<DyeCondition>& a_conds,
                                    const DyeWorldState&             a_world);

    // Parse one clause. Returns false and leaves a_out alone when the entry
    // cannot be trusted.
    [[nodiscard]] bool ConditionFromJson(const Json::Value& a_json,
                                         DyeCondition&      a_out);

    // Parse a whole rule. ALL OR NOTHING, unlike the dye pack loader: half of
    // an AND is a weaker rule than the author wrote, so one bad clause would
    // hand out a colour rather than merely lose one. On false, a_out is
    // cleared.
    [[nodiscard]] bool ConditionsFromJson(const Json::Value&         a_json,
                                          std::vector<DyeCondition>& a_out);

}  // namespace OS
