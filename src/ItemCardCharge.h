#pragma once
#include "PCH.h"

namespace OS::ItemCardCharge {

    // OS-140 SPIKE: put a charge meter on the Seamstone's vanilla item card.
    //
    // ⚠ WHY A HOOK IS THE ONLY ROUTE, measured against the 1.6.1170 binary
    // rather than guessed. The item card population function (AE id 51897)
    // opens with
    //
    //     switch (*(uint8_t*)(form + 0x1A))     // TESForm::formType
    //       case 0x1F: case 0x20:  ->  generic path      (Light, MISC)
    //       case 0x29: case 0x2A:  ->  writes "charge"   (Weapon, Ammo)
    //
    // and the switch runs BEFORE any extra data is read. So attaching
    // ExtraCharge + ExtraEnchantment to the stone cannot work, even though
    // InventoryEntryData::GetEnchantmentCharge has a branch that reads exactly
    // those two extras with no form-type test. That branch is a trap: it makes
    // the extra-data route look viable right up until you read the switch.
    //
    // ⚠ SO WE WRITE THE FIELD OURSELVES, AFTER THE ORIGINAL RUNS. The caller
    // populates the card and THEN pushes it to ActionScript, so a post-call
    // stamp lands before the movie ever sees the object.
    //
    // "charge" is a PERCENTAGE: the weapon path computes current * 100 / max
    // (the 100 is a float constant at 0x141ad2954, read out of the binary).
    //
    // ⚠ AND THE WEAPON ARM WRITES "damage" UNCONDITIONALLY, which is why a
    // stamped stone shows an empty DAMAGE row. The type constants are
    // Armor 1, Weapon 2, Ammo 2, Misc and Apparatus 3, Book 4/6, Ingredient 8,
    // Key 9, SoulGem 12, and only the two arms that write 2 write a charge at
    // all. There is no layout that draws a charge without a damage, so the row
    // is the price of the meter rather than a bug to chase.
    //
    // ⚠ WHAT THIS DOES NOT ANSWER, and the reason it is still a spike. Whether
    // the skin's ActionScript draws a meter for a field on a MISC card is up to
    // that .swf and cannot be read out of the exe. ⚠ AND THE ONE TEST RUN SO
    // FAR PROVED NOTHING EITHER WAY, because the stone held 0 at the time: a
    // meter at 0% and a meter that was never drawn look identical. Fill the
    // stone from the editor's Refill first, then look.
    // Off by default; [Debug] bItemCardCharge.
    void Install();

    [[nodiscard]] bool Installed();

}  // namespace OS::ItemCardCharge
