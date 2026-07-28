// Pure-logic tests for the look-aware catalog: which slot a multi-slot armor
// lists under, and what a collapsed variant group remembers about the records
// it absorbed. No engine, no RE:: types.
//
// The robes fixtures below are real Skyrim.esm ground truth, read off the ARMO
// records and their addon sets. They are the exact records that make "Novice
// Robes" unfindable in the browser today.
#include "StyleGroup.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace {
    // Editor slots -> the catalog's bit space, so the fixtures below read like
    // the evidence table does.
    constexpr std::uint32_t Slots(std::initializer_list<std::uint32_t> a_editorSlots) {
        std::uint32_t mask = 0;
        for (const auto slot : a_editorSlots) {
            mask |= OS::MaskForEditorSlot(slot);
        }
        return mask;
    }
}

int main() {
    using namespace OS;

    {  // ---- ContainsCI: an empty needle matches everything ----------------
        CHECK(ContainsCI("Mantled College Robes", "robes"));
        CHECK(ContainsCI("Mantled College Robes", "COLLEGE"));
        CHECK(ContainsCI("Mantled College Robes", ""));
        CHECK(ContainsCI("", ""));
        CHECK(!ContainsCI("", "robes"));
        CHECK(!ContainsCI("Mage Hood", "robes"));
    }

    {  // ---- which slot row a garment lists under ------------------------
        // Anything covering Body lists under Body; nothing else moves.
        CHECK(PrimaryBitForSlotMask(Slots({ 32 })) == kBitBody);           // unhooded robes
        CHECK(PrimaryBitForSlotMask(Slots({ 31, 32, 42 })) == kBitBody);   // hooded warlock robes
        CHECK(PrimaryBitForSlotMask(Slots({ 31, 32 })) == kBitBody);       // Archmage's Robes
        CHECK(PrimaryBitForSlotMask(Slots({ 32, 33, 37 })) == kBitBody);   // full-body armor set

        // Head and accessory gear is untouched: still the lowest covered bit.
        CHECK(PrimaryBitForSlotMask(Slots({ 31, 42 })) == kBitHair);       // Novice Hood, Iron Helmet
        CHECK(PrimaryBitForSlotMask(Slots({ 30, 31, 42, 43 })) == kBitHead);  // Daedric Helmet
        CHECK(PrimaryBitForSlotMask(Slots({ 42 })) == kBitCirclet);
        CHECK(PrimaryBitForSlotMask(Slots({ 35 })) == kBitAmulet);
        CHECK(PrimaryBitForSlotMask(Slots({ 33 })) == kBitHands);
        CHECK(PrimaryBitForSlotMask(Slots({ 46 })) == kBitCloak);
        CHECK(PrimaryBitForSlotMask(Slots({ 39 })) == kBitShield);

        // A mask the catalog drops before it ever gets here; answer 0 rather
        // than countr_zero's 32.
        CHECK(PrimaryBitForSlotMask(0) == 0);
    }

    {  // ---- The Novice Robes group (evidence section 3) ------------------
        // Six named "Novice Robes*" records plus a template collapse into
        // 000D3DE9 "Mantled College Robes". Owning 0010D66A therefore does not
        // make the look findable under any name, in any slot.
        StyleGroup g;
        g.Seed(0x000D3DE9);
        const std::string survivor = "Mantled College Robes";
        g.Fold(0x0010D2B4, "Novice Robes", survivor);
        g.Fold(0x0010D667, "Novice Robes of Destruction", survivor);
        g.Fold(0x0010D668, "Novice Robes of Alteration", survivor);
        g.Fold(0x0010D669, "Novice Robes of Illusion", survivor);
        g.Fold(0x0010D66A, "Novice Robes of Conjuration", survivor);
        g.Fold(0x0010D66B, "Novice Robes of Restoration", survivor);
        g.Fold(0x0010D671, "Novice Robes", survivor);  // a second record, same name

        CHECK(g.members.size() == 8);       // survivor + 7 absorbed
        CHECK(g.members.front() == 0x000D3DE9);
        CHECK(g.aliases.size() == 6);       // the duplicate "Novice Robes" folded once

        // Owning ANY member makes the look browsable.
        const std::set<std::uint32_t> owned{ 0x0010D66A };
        CHECK(g.AnyMemberKnown([&](std::uint32_t a_id) { return owned.contains(a_id); }));
        CHECK(!owned.contains(0x000D3DE9));  // the survivor itself is NOT owned
        const std::set<std::uint32_t> ownsNothing{ 0x0001396D };
        CHECK(!g.AnyMemberKnown([&](std::uint32_t a_id) { return ownsNothing.contains(a_id); }));

        // The name printed in the player's inventory finds the row, and the
        // tooltip can say which name matched.
        CHECK(g.MatchingAlias("Novice Robes") == "Novice Robes");
        CHECK(g.MatchingAlias("conjuration") == "Novice Robes of Conjuration");
        CHECK(g.MatchingAlias("NOVICE robes of Illusion") == "Novice Robes of Illusion");
        CHECK(g.MatchingAlias("Necromancer").empty());
        // An empty search is not an alias hit - otherwise every row would claim
        // "also known as" the moment the search box was cleared.
        CHECK(g.MatchingAlias("").empty());
    }

    {  // ---- Fold hygiene -------------------------------------------------
        StyleGroup g;
        g.Seed(0x00000001);
        g.Fold(0x00000002, "Iron Armor", "Iron Armor");           // survivor's own name
        g.Fold(0x00000003, "Iron Armor of Health", "Iron Armor");
        g.Fold(0x00000004, "Iron Armor of Health", "Iron Armor"); // duplicate name
        g.Fold(0x00000005, "", "Iron Armor");                     // unnamed
        CHECK(g.members.size() == 5);            // every member counts for ownership
        CHECK(g.aliases.size() == 1);            // only the one distinct new name
        CHECK(g.aliases.front() == "Iron Armor of Health");

        // Aliases are bounded; members are not. Dropping a member would
        // silently un-collect a look the player owns.
        StyleGroup big;
        big.Seed(0x00001000);
        for (std::uint32_t i = 0; i < 100; ++i) {
            big.Fold(0x00001001 + i, "Robes of Destruction " + std::to_string(i), "Black Mage Robes");
        }
        CHECK(big.aliases.size() == StyleGroup::kMaxAliases);
        CHECK(big.members.size() == 101);
        CHECK(big.AnyMemberKnown([](std::uint32_t a_id) { return a_id == 0x00001064; }));

        // A row that collapsed nothing (or a weapon row, which never seeds)
        // answers no to everything - so the caller's survivor-exact test stays
        // the whole test for those.
        StyleGroup lone;
        CHECK(!lone.AnyMemberKnown([](std::uint32_t) { return true; }));
        CHECK(lone.MatchingAlias("anything").empty());
        lone.Seed(0x00000042);
        CHECK(lone.AnyMemberKnown([](std::uint32_t a_id) { return a_id == 0x00000042; }));
    }

    {  // ---- Seed resets, so a rebuilt catalog never inherits a stale group
        StyleGroup g;
        g.Seed(0x00000001);
        g.Fold(0x00000002, "Old Name", "Survivor");
        g.Seed(0x00000001);
        CHECK(g.members.size() == 1);
        CHECK(g.aliases.empty());
    }

    if (g_failures == 0) {
        std::printf("StyleGroupTests: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
