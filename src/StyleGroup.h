#pragma once

#include "SlotMask.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The pure "a catalog row is a LOOK, not a record" core. Everything the
// look-aware catalog decides that carries no engine coupling lives here,
// header-only, so it is unit-tested without RE:: types (see
// tests/test_stylegroup.cpp) - the same shape as SlotMask.h / NpcResolve.h.
namespace OS {

    // Case-insensitive substring test, with the "an empty needle matches
    // everything" convention every caller wants (the browser's empty search
    // box shows all rows; an unset sDiagnosePlugin traces nothing because its
    // callers check emptiness first).
    [[nodiscard]] inline bool ContainsCI(std::string_view a_hay, std::string_view a_needle) {
        if (a_needle.empty()) {
            return true;
        }
        const auto lower = [](char a_c) {
            return std::tolower(static_cast<unsigned char>(a_c));
        };
        const auto it = std::search(a_hay.begin(), a_hay.end(), a_needle.begin(), a_needle.end(),
                                    [&](char a, char b) { return lower(a) == lower(b); });
        return it != a_hay.end();
    }

    // ---- Variant collapse: what it used to throw away ----------------------
    //
    // StyleCatalog::Build collapses armor records sharing an addon set into one
    // ROW, because they are one LOOK - "Iron Armor of Health" is "Iron Armor"
    // with an enchantment, and a browser of looks must not list it twice. The
    // survivor is the lowest-FormID member; every other member's identity was
    // simply discarded. That discard caused both halves of the robes bug:
    //
    //   * COLLECTED-ONLY tested the SURVIVOR's FormID. Owning "Novice Robes of
    //     Conjuration" (0010D66A) did not make its row browsable, because that
    //     record collapses into 000D3DE9 "Mantled College Robes" - a sibling
    //     the player has never owned. The look was invisible under any name,
    //     in any slot.
    //   * SEARCH tested the SURVIVOR's name. No surviving vanilla row is named
    //     "Novice Robes*" at all, so searching the name printed in your own
    //     inventory found nothing.
    //
    // A StyleGroup keeps exactly what was discarded: every member record's
    // FormID, and every distinct member display name ("aliases"). The row is
    // still ONE row - this is identity, not duplication. (Listing a row under
    // every covered slot instead was rejected: it duplicates rows across slots,
    // breaks the documented no-duplicates invariant, and makes the storage bit
    // ambiguous.)
    struct StyleGroup {
        std::vector<std::uint32_t> members;  // every member's FormID, survivor first
        std::vector<std::string>   aliases;  // distinct member names, minus the survivor's

        // Aliases feed a tooltip and a substring search, so they are bounded:
        // the biggest real group (36 warlock robe records) carries ~35 distinct
        // names, and nothing is served by letting a pathological plugin grow one
        // row without limit. Members are NOT bounded - dropping one would
        // silently un-collect a look the player owns, which is the bug this
        // exists to fix.
        static constexpr std::size_t kMaxAliases = 24;

        // The survivor opens its own group, so ownership and search have one
        // uniform thing to walk instead of "the survivor, plus the others".
        void Seed(std::uint32_t a_survivorFormID) {
            members.assign(1, a_survivorFormID);
            aliases.clear();
        }

        // Fold one collapsed variant in. The survivor's own name is never an
        // alias (the row already shows it) and duplicates are dropped, so the
        // eight-record "Mantled College Robes" group contributes the handful of
        // distinct "Novice Robes*" names rather than eight entries.
        void Fold(std::uint32_t a_formID, std::string_view a_name,
                  std::string_view a_survivorName) {
            members.push_back(a_formID);
            if (a_name.empty() || a_name == a_survivorName || aliases.size() >= kMaxAliases) {
                return;
            }
            if (std::ranges::find(aliases, a_name) != aliases.end()) {
                return;
            }
            aliases.emplace_back(a_name);
        }

        // "Does the Collection know ANY member of this look?" The
        // ownership test arrives as a callback so this header stays engine-free.
        // An empty group answers false, which is why the caller tests the
        // survivor first - that keeps weapon rows (never grouped) unchanged.
        template <class Fn>
        [[nodiscard]] bool AnyMemberKnown(Fn&& a_knows) const {
            for (const auto id : members) {
                if (a_knows(id)) {
                    return true;
                }
            }
            return false;
        }

        // The first alias containing a_search, or empty. Non-empty is exactly
        // the condition "this row matched through a name it no longer shows",
        // which is what the row tooltip reports.
        [[nodiscard]] std::string_view MatchingAlias(std::string_view a_search) const {
            if (a_search.empty()) {
                return {};
            }
            for (const auto& alias : aliases) {
                if (ContainsCI(alias, a_search)) {
                    return alias;
                }
            }
            return {};
        }
    };

    // Which ONE slot row a multi-slot armor lists under.
    //
    // Lowest covered bit, EXCEPT that anything covering Body lists under Body.
    // Hooded robes are slots 31+32+42, and "lowest bit" filed them under
    // Hair/Helmet - so browsing Body, the slot a robe obviously belongs to,
    // never showed one. Hoods (31+42), helmets and circlets do not cover Body
    // and stay exactly where they were, so this moves garments and only
    // garments.
    //
    // A zero mask cannot reach the catalog (Build drops it), but answer 0
    // rather than invoking countr_zero's 32 on it.
    [[nodiscard]] constexpr std::uint32_t PrimaryBitForSlotMask(std::uint32_t a_slotMask) {
        if (a_slotMask == 0) {
            return 0;
        }
        if ((a_slotMask >> kBitBody) & 1u) {
            return kBitBody;
        }
        return static_cast<std::uint32_t>(std::countr_zero(a_slotMask));
    }

}  // namespace OS
