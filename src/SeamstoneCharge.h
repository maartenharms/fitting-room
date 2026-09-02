#pragma once

#include <cstdint>

// The Seamstone's price list: what styling costs and what a soul gem is worth.
// OS-129.
//
// ⚠ THE CHARGE ITSELF IS NOT HERE, AND MUST NOT MOVE HERE. It lives on
// `DyeUnlockSet` (`DyeUnlocks.h`) as `Charge` / `SetCharge` / `AddCharge` /
// `SpendCharge`, on the record that already rides the co-save as a u32 in
// `'DYES'`. This file is the price list, not the purse:
//
//   holding and moving charge      -> DyeUnlocks, one owner, under its lock,
//                                     persisted with the unlocks and the deeds
//   what a thing costs, or is worth -> here, pure and testable
//
// ⚠ DO NOT ADD `Spend`, `CanAfford` OR `Recharge` BACK. All three were written
// here on 2026-08-04 and committed before anyone grepped `Charge` in
// `DyeUnlocks.h`, where all three already were. `AddCharge` in particular
// returns how much was actually applied for the exact reason a `Recharge` was
// wanted: the cost plan consumes a soul gem and then calls it, so a Grand gem
// into a full stone has to be reportable. A second copy of that arithmetic is
// a second thing that has to stay in step with the save, and the copy without
// the lock is the one that will be reached for.
//
// To ask whether the stone can pay, compare: `set.Charge() >= CostFor(...)`.
// To take payment, call `set.SpendCharge(CostFor(...))`, which refuses whole
// rather than part-paying.
//
// ⚠ NO ENGINE TYPES, ON PURPOSE, the same shape as `EditorGate.h` and
// `DyeGate.h`. A price table that needs a running game to check is a price
// table nobody checks, and this one is multiplied by user-supplied counts
// against user-supplied INI values.
//
// ⚠ THE STONE IS A PLAIN MISC ITEM AND THE CHARGE IS OURS. MISC items have no
// engine charge; enchantment charge lives on weapons and armour. Changing the
// form type would re-open the earlier forms the user rejected (see
// `LoreModule.h`) and collide with the rule that a plain MISC is the only safe
// custom form. So the meter is presentation over a number we keep, and nothing
// here asks the engine what the stone holds.
//
// ⚠ AND SKYRIM'S OWN RECHARGE MENU CANNOT FILL IT. This was asked for, spiked
// and abandoned on 2026-08-05, so here is the fact that settles it rather than
// an opinion. The recharge apply is the "ItemCardListCallback" Scaleform
// callback (AE 51859), registered by AE 51850 alongside "ShowSoulGemList"
// (AE 51858). Its FIRST statement is
//
//     if (!entry || !GetEnchantment(entry)) return;   // AE 51018
//
// so on an item with no enchantment the whole thing is a no-op, and everything
// after it - the soul value, the clamp against max charge, the write, and the
// RemoveItem that eats the gem - is inside that guard. There is no arrangement
// of card fields that gets past it: the button bar and the gem list are drawn
// by ActionScript off fields we can stamp, but the apply reads the ITEM.
//
// Passing the guard means giving the stone a real `ExtraEnchantment`, which
// means an `ExtraDataList` to hold it, which the game writes into the player's
// save and does not take back when the setting goes off. CommonLib cannot build
// one (`ExtraDataList`'s constructor is declared and never implemented), the
// hand-built version is unproven and was the prime suspect for a heap-corruption
// CTD, and `sizeof` reports the SE layout so even the allocation size has to be
// asserted by hand. That is a permanent write to somebody's save, in a
// container we cannot construct correctly, to duplicate a Refill that already
// works in the editor - and works BETTER, because our list states what a gem
// wastes before it is eaten and the vanilla one just eats it.
//
// What survives from the spike is the read-only half: `ItemCardCharge` stamps
// the charge onto the card's Scaleform object, which writes nothing anywhere.
namespace OS::SeamstoneCharge {

    // What a filled soul gem returns to the stone. Petty through Grand in the
    // engine's own order, with Black priced as Grand because that is what it
    // holds; the moral question is the player's, not the arithmetic's.
    //
    // ⚠ THESE ARE NOT THE ENGINE'S SOUL VALUES (250..3000) SCALED. They are
    // chosen against what styling COSTS below: a petty gem should be a slot or
    // two and a grand should be a wardrobe, so the numbers are picked from the
    // spend side rather than inherited from a table that was balanced for
    // enchanting.
    enum class SoulSize : std::uint8_t {
        kNone = 0,
        kPetty,
        kLesser,
        kCommon,
        kGreater,
        kGrand,
    };

    [[nodiscard]] constexpr std::uint32_t ValueOf(SoulSize a_size) {
        switch (a_size) {
            case SoulSize::kNone:
                return 0;
            case SoulSize::kPetty:
                return 250;
            case SoulSize::kLesser:
                return 500;
            case SoulSize::kCommon:
                return 1000;
            case SoulSize::kGreater:
                return 2000;
            case SoulSize::kGrand:
                return 3000;
        }
        return 0;
    }

    // What an edit costs. Mirrors the gold economy deliberately: per slot, plus
    // a flat charge for a dye, so a player switching cost modes meets the same
    // shape of bill and the two settings stay comparable.
    struct Price {
        std::uint32_t perSlot{ 100 };
        std::uint32_t perDye{ 250 };
        // ⚠ ONE APPEARANCE DIMENSION, ADDED 2026-08-07. Body preset, hair
        // visibility, hair colour, hair style, eyes and brows each stage with
        // zero changed slots, so with only the two terms above a character
        // could be rebuilt head to toe for nothing at all. See
        // OS::ChangedLookCount for what does and does not count as one.
        std::uint32_t perLook{ 100 };
    };

    // ⚠ SATURATING, NOT WRAPPING. These multiply a user-supplied count by a
    // user-supplied INI value, and an outfit with every slot styled against a
    // hand-edited price is exactly where a 32-bit product turns a huge bill
    // into a free one. A cost that saturates refuses; a cost that wraps grants.
    [[nodiscard]] constexpr std::uint32_t CostFor(const Price& a_price,
                                                  std::uint32_t a_slots,
                                                  std::uint32_t a_dyes,
                                                  std::uint32_t a_looks) {
        constexpr std::uint32_t kMax = 0xFFFFFFFFu;
        std::uint64_t total = static_cast<std::uint64_t>(a_price.perSlot) * a_slots +
                              static_cast<std::uint64_t>(a_price.perDye) * a_dyes +
                              static_cast<std::uint64_t>(a_price.perLook) * a_looks;
        return total > kMax ? kMax : static_cast<std::uint32_t>(total);
    }

    // For the meter. Returns 0..1 and is safe on a zero capacity, which a
    // hand-edited INI can produce and which would otherwise divide by zero in
    // the draw.
    //
    // Clamps above as well as below: `AddCharge` refuses to top up a stone that
    // is already over the cap rather than cutting it down, so a save made
    // before the player lowered `iSeamstoneCapacity` legitimately holds more
    // than full and the meter has to draw that as full, not as overflowing.
    [[nodiscard]] constexpr float Fraction(std::uint32_t a_held, std::uint32_t a_capacity) {
        if (a_capacity == 0) {
            return 0.0f;
        }
        const std::uint32_t held = a_held > a_capacity ? a_capacity : a_held;
        return static_cast<float>(held) / static_cast<float>(a_capacity);
    }

}  // namespace OS::SeamstoneCharge
