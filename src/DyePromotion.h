#pragma once

#include "DyeConditions.h"
#include "DyeRules.h"
#include "DyeUnlocks.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace OS {

    // The two fields promotion needs off a dye. A separate struct rather than
    // OS::Dye so this module stays independent of DyePalette, and so a test can
    // build a palette without a colour in sight.
    struct DyePromotable {
        std::string id;
        std::string rarity;
    };

    // What one promotion pass did.
    struct PromotionResult {
        // The ids GAINED by this call, in palette order, so a caller can tell
        // the player what just opened up.
        //
        // ⚠ Only ids that were actually STORED. Add refuses an id that is
        // empty, over its length cap or past its count cap, and announcing one
        // of those would re-announce it on every load forever, for a colour
        // that can never be chosen.
        std::vector<std::string> gained;

        // Rarities that matched NOTHING, and how many dyes each covered.
        //
        // ⚠ THIS IS A FREE-DYE REPORT, not a curiosity. A rarity with no tier
        // entry resolves to no conditions, and no conditions means no
        // requirement, so every dye it covers is handed out at level 1 and
        // written permanently into the save. The tier lookup is an exact byte
        // match, so "Dye stamp" against a table spelling it "Dye Stamp" frees
        // all 32 of them and looks identical to an author who meant it.
        //
        // ⚠ It does not FREEZE anything, deliberately. A third party pack may
        // ship a bucket this mod never heard of, and the documented fallback is
        // that an unknown rarity stays free. Freezing a player's whole palette
        // over someone else's new bucket would be the wrong trade. Being quiet
        // about it is the other wrong trade, and this is the only place in the
        // codebase that can see it at all: nowhere else holds the palette and
        // the rule set at the same time.
        //
        // Dyes with an EMPTY rarity are not counted. Shipping a colour with no
        // rarity is the supported way to say "free", which is what
        // vanilla.json's twelve entries do. A dye carrying its own per-dye
        // rule is not counted either, because its tier was never consulted.
        std::map<std::string, std::size_t> unmatchedRarities;

        // Per-dye rule keys that no dye in the palette claims, sorted.
        //
        // ⚠ THE OTHER FREE-DYE REPORT, and the one the rarity report cannot
        // reach. An override key orphaned by a typo or a renamed id merges
        // cleanly, is not an unknown top level key, and leaves its dye on the
        // RARITY TIER instead. Covers() then answers true off that tier, so
        // unmatchedRarities stays empty and nothing above notices. Measured on
        // the shipped palette and the shipped rules: changing one key from
        // eso:master-gold to eso:master_gold moves gained from 63 to 64 with
        // every other number identical, because the Common tier is
        // [{"type":"always"}] and master-gold's real gate is level 50.
        //
        // ⚠ An orphan is a DOWNGRADE rather than a lock, and that claim now
        // has something behind it. tools/check_dye_rules.py refuses an
        // override weaker than its own tier on any dimension the two share:
        // rewriting eso:void-pitch, a Rare whose tier is level 35, from deed
        // 100 to level 2 used to pass a green check and be honoured by the
        // loader. Where the two gate on DIFFERENT things, "Smithing 100"
        // against "level 35", nothing orders them without a model of how fast
        // a character levels, so the script lists those and declines to
        // decide. Sixty three of the 220 are that shape, so read this as proven
        // where it can be and taken on trust where it cannot.
        //
        // Thirteen of the 220 sit on Common, which is free at level 1 and
        // permanent once written, and for those the claim is unconditional:
        // the tier is "always", so any override at all tightens it.
        //
        // ⚠ It FREEZES NOTHING, the same trade unmatchedRarities makes. A
        // rules pack covering a colour pack the user has not installed is
        // legitimate and common, so this only has to stop being silent.
        std::vector<std::string> unmatchedDyeRules;
    };

    // Add every dye whose rule is now satisfied and is not already held.
    //
    // ⚠ Never removes. Running this against a world that has gone backwards,
    // a Legendary skill reset or a failed quest, gains nothing and loses
    // nothing.
    //
    // ⚠ The caller decides whether to run this at all. A rules file that
    // failed to load contributes none of its keys, so every dye it covered
    // reads as having no rule, which means free. Promotion has no way to see
    // that from in here; DyeRules::SnapshotChecked is what carries the answer.
    [[nodiscard]] PromotionResult Promote(
        const std::vector<DyePromotable>& a_palette, const DyeRuleSet& a_rules,
        const DyeWorldState& a_world, DyeUnlockSet& a_set);

    // May this colour be chosen right now? The whole feature turns on this, so
    // it is a free function that a test can reach without an engine, a save or
    // a UI.
    [[nodiscard]] inline bool CanUseDye(std::string_view    a_id,
                                        const DyeUnlockSet& a_unlocked,
                                        bool                a_unlocksOn) {
        return !a_unlocksOn || a_unlocked.Has(a_id);
    }

    // Does this swatch wear the gold "you just earned this" fold?
    //
    // ⚠⚠ ONLY WHILE UNLOCKS ARE ON, AND THAT IS THE FIX RATHER THAN A TASTE
    // CALL. The mark clears on the click that uses the colour, and clearing it
    // is DyeUnlocks::Acknowledge, which deliberately refuses an id the earned
    // set does not hold - a locked colour must not be able to spend its corner
    // before it is won. With unlocks OFF, CanUseDye says yes to every colour in
    // the palette while the earned set still holds only what promotion granted,
    // so every other swatch drew a fold that no click could ever take off
    // (user 2026-08-11, "clicking on dyes doesn't remove the gold foil"). Under
    // this gate the fold is drawn exactly where a click can clear it.
    //
    // It is also the truthful reading. "Newly earned" is a statement about an
    // economy, and free-form has none: nothing is earned there because
    // everything is already yours, so a discovery mark has nothing to say.
    //
    // ⚠ NOT DRAWN ON A LOCKED SWATCH either, which is the rule the padlock
    // already owns: a colour you have not won has nothing to celebrate, and its
    // corner is where the padlock's neighbourhood already is.
    [[nodiscard]] inline constexpr bool ShowsNewDyeMark(bool a_unlocksOn, bool a_locked,
                                                        bool a_acknowledged) {
        return a_unlocksOn && !a_locked && !a_acknowledged;
    }

}  // namespace OS
