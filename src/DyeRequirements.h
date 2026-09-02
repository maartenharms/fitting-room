#pragma once

#include "DyeConditions.h"

#include <cstdint>
#include <string>
#include <vector>

namespace OS {

    // One clause of an unlock rule, turned into the three things a locked
    // swatch has to be able to say: what it asks for, what the character
    // actually reads, and whether that is enough.
    //
    // ⚠ DATA, NOT A SENTENCE. The obvious shape here is one pre-baked string
    // per clause, and it would have been shorter. It also decides the colour,
    // the ordering and the layout on behalf of a caller that has a font, a
    // theme and a tooltip width and this module has none of the three. A
    // met clause and an unmet one want to look different, and a struct is what
    // lets the pane do that without this module knowing what a colour is.
    //
    // ⚠ AND IT IS WHAT KEEPS THE MODULE PURE. Formatting means translating,
    // translating means FUCK::Translate, and that is an engine dependency in a
    // file whose entire value is that a test executable can run it.
    struct DyeClause {
        // ⚠ NEVER kNever and NEVER kAlways. Both are answered above this level:
        // kNever takes the whole rule (see DyeRequirements::unreadable) and
        // kAlways contributes nothing worth drawing. A switch over this field
        // still has to carry all six arms, because the plugin target compiles
        // with /we4062 and a missing enumerator is a hard error there.
        DyeCondKind kind{ DyeCondKind::kAlways };

        // kSkill: the skill's rules-file name, "Smithing". Empty when the
        // ActorValue is not one of Skyrim's eighteen, which the parser cannot
        // produce but a corrupt record can, so the caller can fall back rather
        // than print nothing.
        // kDeed: the deed's own name, "channelsDyed".
        // kQuest: the plugin the quest is defined in.
        // kLevel: empty. A level has no subject to name.
        std::string subject;

        std::uint32_t required{ 0 };  // kLevel, kSkill, kDeed
        std::uint32_t formId{ 0 };    // kQuest, the LOCAL id inside the plugin

        // ⚠ Whether `value` is a reading of anything at all, which is NOT the
        // same as it being zero. A skill nobody trained reads zero and that is
        // a real number worth showing; a quest has no number behind it in
        // either direction. Without the flag the caller cannot tell "yours is
        // 0" from "there is nothing to show".
        bool          hasValue{ false };
        std::uint32_t value{ 0 };

        bool met{ false };
    };

    // A whole rule, turned into what a locked swatch draws.
    struct DyeRequirements {
        // ⚠ THE RULE COULD NOT BE READ, which is a different answer from "you
        // have not earned it yet" and has to stay distinguishable.
        //
        // kNever is not authorable. It is what the loader substitutes for a
        // rule it refused, so the dye is locked behind a condition no player
        // can ever satisfy. Drawing that as an ordinary locked swatch tells
        // them to go and earn something that does not exist, and no amount of
        // playing will change it: the fix is in their Unlocks folder and
        // nowhere else.
        //
        // When this is true, `clauses` is EMPTY and `allMet` is false. There is
        // nothing honest to list, because an array is an AND and one clause
        // nobody can satisfy makes the readable half beside it beside the
        // point.
        bool unreadable{ false };

        // In the rule's own order, not sorted by verdict. It is the order the
        // author wrote and the order their file will show them.
        std::vector<DyeClause> clauses;

        // ⚠ Agrees with AllSatisfied on the same inputs, and the test pins that
        // across every shape. Two functions answering the same question that
        // can disagree is how a swatch ends up grey under a tooltip saying
        // everything is met.
        bool allMet{ false };
    };

    // Describe one dye's rule against one reading of the world.
    //
    // Pure: no engine, no ImGui, no translation. The world state is a snapshot
    // the caller took on the main thread, so this is safe to run from the draw.
    [[nodiscard]] DyeRequirements DescribeDyeRequirements(
        const std::vector<DyeCondition>& a_conds, const DyeWorldState& a_world);

}  // namespace OS
