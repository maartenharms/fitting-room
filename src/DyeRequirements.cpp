#include "DyeRequirements.h"

#include <algorithm>

namespace OS {

    namespace {
        // A value from a map, or zero. The SAME fail-closed reading Satisfied
        // uses: a skill nobody gathered and a deed nobody counts both read as
        // zero rather than as absent, so the tooltip shows the number the gate
        // actually compared against instead of a blank.
        template <class Map, class Key>
        std::uint32_t ValueOr0(const Map& a_map, const Key& a_key) {
            const auto it = a_map.find(a_key);
            return it == a_map.end() ? 0u : it->second;
        }
    }  // namespace

    DyeRequirements DescribeDyeRequirements(
        const std::vector<DyeCondition>& a_conds, const DyeWorldState& a_world) {
        DyeRequirements out;

        // ⚠ kNEVER FIRST, AND IT TAKES THE WHOLE RULE. Checked before anything
        // is described because an array is an AND: one clause nobody can ever
        // satisfy makes the readable half beside it beside the point, and
        // listing that half would name the only obstacle a player can see.
        // They would then go and reach it, nothing would change, and no line
        // anywhere would say why.
        //
        // The loader is kNever's only producer, so this state means exactly one
        // thing: a rule failed to parse and the fix is in the Unlocks folder.
        if (std::any_of(a_conds.begin(), a_conds.end(),
                        [](const DyeCondition& c) {
                            return c.kind == DyeCondKind::kNever;
                        })) {
            out.unreadable = true;
            out.allMet     = false;
            return out;
        }

        out.clauses.reserve(a_conds.size());
        for (const auto& cond : a_conds) {
            DyeClause clause;
            clause.kind = cond.kind;

            switch (cond.kind) {
            case DyeCondKind::kAlways:
                // ⚠ NO CLAUSE. It cannot appear on a locked dye at all, since
                // an always is satisfied from character creation, and drawing
                // "Always: met" as a requirement is noise on the one tooltip
                // whose whole job is naming what is missing. It still counts as
                // satisfied, which the loop below gets for free by skipping it.
                continue;

            case DyeCondKind::kNever:
                // Unreachable: the any_of above returned already. Present
                // because the plugin target compiles with /we4062, so a switch
                // missing an enumerator is a hard error there, and because
                // adding a kind later should break the build rather than
                // silently produce a clause with nothing filled in.
                continue;

            case DyeCondKind::kLevel:
                clause.required = cond.min;
                clause.hasValue = true;
                clause.value    = a_world.level;
                break;

            case DyeCondKind::kSkill:
                // ⚠ THE NAME COMES OFF THE PARSER'S OWN TABLE. Empty for an
                // ActorValue outside the eighteen, which the parser cannot
                // produce but a corrupt record can, and the caller falls back
                // rather than printing a blank where a skill should be.
                clause.subject.assign(SkillNameByAV(cond.skill));
                clause.required = cond.min;
                clause.hasValue = true;
                clause.value    = ValueOr0(a_world.skills, cond.skill);
                break;

            case DyeCondKind::kQuest:
                // ⚠ NO DISPLAY NAME, AND NONE INVENTED. Resolving one means
                // TESForm, which means the engine, which is the dependency this
                // module exists without. All that is known here is the plugin
                // and the local form id, so both are handed over and hasValue
                // stays false: there is no reading to put beside a quest, and a
                // zero would read as one.
                clause.subject = cond.quest.plugin;
                clause.formId  = cond.quest.formId;
                break;

            case DyeCondKind::kLocationCleared:
                // Same shape as a quest and for the same reason: a plugin and a
                // local form id, with no reading to put beside it.
                clause.subject = cond.location.plugin;
                clause.formId  = cond.location.formId;
                break;

            case DyeCondKind::kDeed:
                clause.subject  = cond.deed;
                clause.required = cond.min;
                clause.hasValue = true;
                clause.value    = ValueOr0(a_world.deeds, cond.deed);
                break;
            }

            // ⚠ Satisfied(), not a second comparison written out here. The
            // thresholds are inclusive, a quest is a set lookup, and a rule
            // whose verdict on screen can disagree with the rule that gated the
            // swatch is the worst of the failures available: it says earned
            // under a colour that is still grey.
            clause.met = Satisfied(cond, a_world);
            out.clauses.push_back(std::move(clause));
        }

        // Empty is met, the same answer AllSatisfied gives an empty list: no
        // rule is no obstacle. That covers a rule of nothing but always clauses
        // too, which lands here with every clause dropped.
        out.allMet = std::all_of(out.clauses.begin(), out.clauses.end(),
                                 [](const DyeClause& c) { return c.met; });
        return out;
    }

}  // namespace OS
