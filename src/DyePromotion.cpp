#include "DyePromotion.h"

#include <set>
#include <utility>

namespace OS {

    PromotionResult Promote(const std::vector<DyePromotable>& a_palette,
                            const DyeRuleSet&                 a_rules,
                            const DyeWorldState&              a_world,
                            DyeUnlockSet&                     a_set) {
        PromotionResult result;
        // ⚠ EVERY LOAD, NOT ONLY THE FIRST, for the same reason the unmatched
        // rarity check sits above the already-held skip: an orphaned key has
        // already freed its dye by the time anyone goes looking, and a warning
        // that fires once is one nobody reads.
        //
        // One pass over the palette to collect ids, then one lookup per
        // authored key. Twenty nine keys against 318 ids on the shipped
        // configuration, once per load, on the main thread. In that
        // configuration it produces nothing at all.
        //
        // Views into a_palette, which outlives this call. DyeKeys() hands back
        // owned strings, so what goes into the result owns itself.
        {
            std::set<std::string_view> present;
            for (const auto& dye : a_palette) {
                present.insert(dye.id);
            }
            for (auto& key : a_rules.DyeKeys()) {
                if (!present.contains(key)) {
                    result.unmatchedDyeRules.push_back(std::move(key));
                }
            }
        }
        for (const auto& dye : a_palette) {
            // ⚠ ABOVE THE ALREADY-HELD SKIP, deliberately. A rarity that
            // matched nothing has already freed its dyes on the FIRST pass, so
            // a check sitting below the skip would report it once and then go
            // quiet from the second load onward, which is every load a player
            // might go looking for the reason on. It costs one map lookup per
            // dye per load, once, on the main thread.
            //
            // An empty rarity is not an unmatched one. Shipping a colour with
            // no rarity is the supported way to say "free", and vanilla.json's
            // twelve entries do exactly that.
            if (!dye.rarity.empty() && !a_rules.Covers(dye.id, dye.rarity)) {
                ++result.unmatchedRarities[dye.rarity];
            }
            if (a_set.Has(dye.id)) {
                continue;  // already held: not re-reported, and never rechecked
            }
            // ⚠ Announce it only if it actually got STORED. Add refuses an id
            // that is empty, over the length cap or past the count cap, and
            // reporting one of those as newly earned would re-announce it on
            // every single load forever, for a colour that can never be used.
            //
            // ⚠ AND THE ORDER IS LOAD BEARING. && short circuits, so a dye
            // whose rule is not satisfied never reaches Add at all. Swapping
            // the operands would store the whole palette and then ask whether
            // it should have, and unlocks are add only, so there is no asking
            // afterwards.
            if (AllSatisfied(a_rules.For(dye.id, dye.rarity), a_world) &&
                a_set.Add(dye.id)) {
                result.gained.push_back(dye.id);
            }
        }
        return result;
    }

}  // namespace OS
