#include "PCH.h"

#include "DyeWorld.h"

#include "DyeStatDeed.h"
#include "DyeStats.h"

namespace OS::DyeWorld {

    namespace {

        using AV    = RE::ActorValue;
        using Skill = RE::PlayerCharacter::PlayerSkills::Data::Skills::Skill;

        // Whether ActorValue a_av, shifted down by kFirstSkillAV, is the index
        // a_skill occupies in PlayerSkills::Data. That shift is how this file
        // reaches legendaryLevels, and it is only correct while the two enums
        // list the same eighteen skills in the same order.
        [[nodiscard]] constexpr bool MapsTo(AV a_av, Skill a_skill) {
            return static_cast<std::uint32_t>(a_av) - kFirstSkillAV ==
                   static_cast<std::uint32_t>(a_skill);
        }

        // ⚠ The length assert is the one that stops a read past the array, so
        // it carries a message. legendaryLevels is declared
        // std::uint32_t[Skill::kTotal] at PlayerCharacter.h:391 and kTotal is 18
        // (PlayerCharacter.h:352-373), which is exactly the 6..23 range
        // DyeConditions.h states.
        static_assert(Skill::kTotal == kLastSkillAV - kFirstSkillAV + 1,
                      "legendaryLevels must be exactly as long as the skill "
                      "range this file loops over, or the shifted index reads "
                      "past the end of it");

        // ⚠ ONE ASSERT PER SKILL, deliberately. The length plus both ends does
        // not pin the middle, and a renumber in between would read a different
        // skill's counter with nothing to say so: the wrong colour granted, and
        // granted is the direction this system cannot take back.
        static_assert(MapsTo(AV::kOneHanded, Skill::kOneHanded));
        static_assert(MapsTo(AV::kTwoHanded, Skill::kTwoHanded));
        static_assert(MapsTo(AV::kArchery, Skill::kArchery));
        static_assert(MapsTo(AV::kBlock, Skill::kBlock));
        static_assert(MapsTo(AV::kSmithing, Skill::kSmithing));
        static_assert(MapsTo(AV::kHeavyArmor, Skill::kHeavyArmor));
        static_assert(MapsTo(AV::kLightArmor, Skill::kLightArmor));
        static_assert(MapsTo(AV::kPickpocket, Skill::kPickpocket));
        static_assert(MapsTo(AV::kLockpicking, Skill::kLockpicking));
        static_assert(MapsTo(AV::kSneak, Skill::kSneak));
        static_assert(MapsTo(AV::kAlchemy, Skill::kAlchemy));
        static_assert(MapsTo(AV::kSpeech, Skill::kSpeech));
        static_assert(MapsTo(AV::kAlteration, Skill::kAlteration));
        static_assert(MapsTo(AV::kConjuration, Skill::kConjuration));
        static_assert(MapsTo(AV::kDestruction, Skill::kDestruction));
        static_assert(MapsTo(AV::kIllusion, Skill::kIllusion));
        static_assert(MapsTo(AV::kRestoration, Skill::kRestoration));
        static_assert(MapsTo(AV::kEnchanting, Skill::kEnchanting));

    }  // namespace

    DyeWorldState Gather(const DyeRuleSet&   a_rules,
                         const DyeUnlockSet& a_unlocks) {
        DyeWorldState out;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            // ⚠ BELT AND BRACES, and it does NOT do what an earlier version of
            // this comment claimed. The singleton is non-null from game init
            // onward, long before the earliest SKSE message this plugin sees,
            // and during a load screen it holds the OUTGOING character's level
            // and skills rather than anything zeroed. So there is no load screen
            // this fires on and no zeroed world for it to catch. What keeps
            // promotion off the wrong character is the call site,
            // kPostLoadGame / kNewGame, after the co-save record is decoded.
            //
            // If it ever does fire, everything reads against a zeroed state.
            // That withholds rather than grants for most rules, which is the
            // recoverable direction, but it is NOT the absolute the old comment
            // asserted. kAlways passes, because "no rule" has always meant free.
            // kQuest genuinely fails closed, since an empty set contains
            // nothing. A level, skill or deed clause written "min": 0 passes
            // too: the parser accepts 0 as a uint and Satisfied compares >=.
            return out;
        }

        out.level = player->GetLevel();

        // The engine's own record that a skill reached 100, read once and
        // guarded once. See the loop for why it is needed at all. A null
        // anywhere on this path leaves every skill on its base value, which
        // withholds rather than grants.
        const auto* pskills = player->GetInfoRuntimeData().skills;
        const auto* sdata   = pskills ? pskills->data : nullptr;

        if (auto* avo = player->AsActorValueOwner()) {
            // ⚠ This null check is DEAD and is kept for shape, not for safety.
            // Actor.h:678 returns
            // &REL::RelocateMemberIfNewer<ActorValueOwner>(...), and
            // Relocation.h:2153-2156 computes that as this plus an offset, so it
            // cannot be null for a non-null player. Its unreachable branch would
            // leave skills empty, which ValueOr0 turns into 0, so it fails
            // closed if it ever stopped being unreachable. Written down so the
            // next reviewer does not spend the measurement again.
            //
            // ⚠ The range comes from DyeConditions.h, NOT from two constants
            // hand-copied into this file. The parser's skill table and this
            // loop have to agree, and a second statement of 6 and 23 is a
            // second thing to forget when the table changes.
            for (std::uint32_t av = kFirstSkillAV; av <= kLastSkillAV; ++av) {
                // ⚠ BASE, not GetActorValue, and the difference is permanent.
                // GetActorValue folds in temporary modifiers, so a Fortify
                // Smithing potion or an enchanted ring would report skill the
                // character never trained. Unlocks are STICKY, so a colour
                // granted off a buff is granted forever and wrongly granted is
                // the one direction this system cannot take back. The base
                // value can only ever withhold a colour until the skill is
                // genuinely earned, and that is one levelup away from fixed.
                const auto value =
                    avo->GetBaseActorValue(static_cast<RE::ActorValue>(av));
                // ⚠ THE UPPER BOUND IS NOT PARANOIA. NaN fails both comparisons
                // and lands on 0, but +inf passes "> 0.0f", and converting a
                // float the destination cannot represent is undefined behaviour
                // rather than a defined wrap. A wrapped large value satisfies
                // every min threshold, so the missing bound failed OPEN.
                // 4294967296.0f is exactly 2^32 and exactly representable, so
                // the comparison is not itself approximate.
                out.skills[av] = (value > 0.0f && value < 4294967296.0f)
                                     ? static_cast<std::uint32_t>(value)
                                     : 0u;

                // ⚠ A Legendary reset writes the base back to 15 and destroys
                // the only evidence the skill was ever trained. Add-only does
                // not cover that: it protects a colour already promoted, and
                // promotion runs on load, so a character who reaches Smithing
                // 100 and legendaries it in the same session never had a
                // promotion pass see the 100. Sixteen shipped rules gate on a
                // skill at 100, and legendarying the moment you get there is
                // the point of the mechanic, so this is the mainline path.
                //
                // legendaryLevels is the engine's own counter and no buff, no
                // ability and no console AV write can raise it, which is what
                // makes it safe to trust in the granting direction where
                // GetActorValue was not.
                //
                // ⚠ READ AS PROOF OF 100, NEVER MULTIPLIED BY THE RESET COUNT.
                // Three resets prove 100 three times over, not 300. So this
                // raises the skill to 100 and stops; a base above 100, from a
                // regrind past the cap, keeps its own higher value.
                //
                // ⚠ A character who legendaried before this rule existed is
                // still withheld. The counter records that a reset happened,
                // not what the base was before it, so installing the mod or
                // adding a skill rule later cannot recover it. Known issue,
                // not a bug report.
                if (sdata && out.skills[av] < 100u &&
                    sdata->legendaryLevels[av - kFirstSkillAV] > 0) {
                    out.skills[av] = 100u;
                }
            }
        }

        if (auto* handler = RE::TESDataHandler::GetSingleton()) {
            for (const auto& ref : a_rules.QuestRefs()) {
                // ⚠ LookupForm, not LookupByID. A rule names a LOCAL form id
                // and the plugin that defines it, because a full form id
                // depends on load order and would break the moment a user
                // reorders their mods.
                //
                // A ref that will not resolve, because the plugin is absent,
                // the id is wrong, or the form is not a quest, simply stays out
                // of the set. Its condition reads unsatisfied rather than
                // satisfied: unknown must never pass for done.
                auto* quest =
                    handler->LookupForm<RE::TESQuest>(ref.formId, ref.plugin);
                if (quest && quest->IsCompleted()) {
                    out.questsDone.insert(ref);
                }
            }
            // ⚠ CLEARED, WHICH IS A FLAG ON THE LOCATION AND NOT A QUEST. The
            // same resolve-by-plugin-and-local-id rule applies, and the same
            // fail-closed rule: a location that will not resolve stays out of
            // the set and its condition reads unsatisfied, because unknown must
            // never pass for done.
            for (const auto& ref : a_rules.LocationRefs()) {
                auto* location =
                    handler->LookupForm<RE::BGSLocation>(ref.formId, ref.plugin);
                if (location && location->IsCleared()) {
                    out.locationsCleared.insert(ref);
                }
            }
        }

        // Our own counters, HANDED IN rather than fetched. Reading them here
        // through DyeUnlocks::Snapshot is what made calling this function from
        // inside DyeUnlocks::With a silent hang. See the header.
        out.deeds[kDeedChannelsDyed] = a_unlocks.Deed(kDeedChannelsDyed);

        // ⚠ SKYRIM'S OWN COUNTERS, WHICH ARE NOT FETCHED HERE EITHER, and for a
        // harder reason than the one above: they cannot be. Game.QueryStat
        // answers about 180 ms after it is asked, on the VM's thread, so there
        // is no synchronous read to put on this line. DyeStats dispatched them
        // and this takes whatever has landed.
        //
        // ⚠ WHAT HAS NOT LANDED READS 0, DELIBERATELY. On the first pass of a
        // load that is every one of them, so a colour gating on a stat stays
        // locked until the second pass runs from DyeStats' settle callback.
        // Withholding is the recoverable direction and promotion is add-only,
        // so the second pass can only ever hand something over.
        //
        // ⚠ THE KEY IS THE DEED NAME THE RULE WROTE, prefix and all, because
        // that is what Satisfied looks up. StatNames() has already taken the
        // prefix off for the dispatch, so it goes back on here; putting the
        // bare stat name in this map would leave every stat clause reading a
        // counter nothing filled.
        //
        // Written even when absent, rather than left out, so the locked swatch
        // can show a real "3 of 50" instead of falling back to nothing.
        const auto stats = DyeStats::Values();
        for (const auto& name : a_rules.StatNames()) {
            const auto it = stats.find(name);
            out.deeds[std::string(kStatDeedPrefix) + name] =
                it == stats.end() ? 0u : it->second;
        }

        return out;
    }

    std::map<QuestRef, std::string> NameRefs(const DyeRuleSet& a_rules) {
        std::map<QuestRef, std::string> out;
        auto* const                     handler = RE::TESDataHandler::GetSingleton();
        if (!handler) {
            return out;
        }

        // ⚠ ONE HELPER FOR BOTH, AND THE FULL NAME IS THE ONLY THING TAKEN.
        // An editor id would resolve more often and is not a name: "DLC1VQ08"
        // helps nobody, and printing it would be the plugin line's mistake in
        // a longer costume. A form with no full name simply stays out.
        const auto take = [&out](const QuestRef& a_ref, const RE::TESFullName* a_named) {
            if (!a_named) {
                return;
            }
            const std::string_view name = a_named->GetFullName();
            if (name.empty()) {
                return;
            }
            out[a_ref] = std::string(name);
        };

        for (const auto& ref : a_rules.QuestRefs()) {
            take(ref, handler->LookupForm<RE::TESQuest>(ref.formId, ref.plugin));
        }
        for (const auto& ref : a_rules.LocationRefs()) {
            take(ref, handler->LookupForm<RE::BGSLocation>(ref.formId, ref.plugin));
        }

        // ⚠ THE COUNT, NOT THE NAMES. A pack that names forty quests would put
        // forty lines in a log nobody is reading for that, and the shortfall is
        // the only interesting number: refs minus names is exactly the set of
        // swatches that will fall back to naming their plugin.
        const auto refs = a_rules.QuestRefs().size() + a_rules.LocationRefs().size();
        if (out.size() < refs) {
            spdlog::info("Dye unlocks: named {} of {} quest/location requirement(s); "
                         "the rest name their plugin instead, because the form did "
                         "not resolve or carries no name.",
                         out.size(), refs);
        } else {
            spdlog::info("Dye unlocks: named all {} quest/location requirement(s).",
                         refs);
        }
        return out;
    }

}  // namespace OS::DyeWorld
