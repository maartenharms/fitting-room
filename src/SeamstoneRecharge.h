#pragma once
#include "PCH.h"

namespace OS::SeamstoneRecharge {

    // OS-140: let Skyrim's OWN soul-gem recharge fill the Seamstone, without
    // writing anything to anybody's save.
    //
    // ⚠ THIS REPLACES `SeamstoneEnchant`, WHICH WAS DELETED, AND THE REASON IT
    // WAS DELETED WAS PARTLY A MISREADING. That file lent the stone a real
    // `ExtraEnchantment` so the recharge would have a capacity to work against,
    // which meant hand-building an `ExtraDataList` the game then wrote into the
    // save permanently. Killing it was right. The stated reason - "the apply is
    // gated on the item really being enchanted" - was wrong: `FUN_1408ef5d0`
    // (AE 51018) reads "selectedIndex" off the Scaleform list and hands back the
    // selected `InventoryEntryData`, so the guard at the top of the apply means
    // "something is selected", nothing more. The field proved it before the
    // decompile did: with no enchantment anywhere near the stone, pressing T
    // Charge on it ate a Black Soul Gem and raised Enchanting to 16.
    //
    // ⚠ SO THE STONE WAS ALREADY REACHABLE, AND ALREADY DESTRUCTIVE. Read the
    // recharge apply ("ItemCardListCallback", AE 51859) as the disassembly has
    // it and the arithmetic is:
    //
    //     xmm6 = GetMaxCharge(entry)               AE 15992, float in XMM0
    //     xmm0 = GetChargePercent(entry, false)    AE 51895, float in XMM0
    //     xmm0 = max(xmm0 * xmm6 * 0.01, 0) + soulValue
    //     if (xmm0 <= xmm6) xmm6 = max(xmm0, 0)    else xmm6 stays at the max
    //     SetCharge(entry, xmm6, ...)              AE 16021
    //     ... then RemoveItem(gem), the sound, and the card refresh
    //
    // On a MISC `GetMaxCharge` is 0, so the clamp drops the new charge to 0 and
    // `SetCharge` writes nothing (it opens with an `__RTDynamicCast` a MISC
    // fails anyway). The gem is still eaten and the skill still rises. That is
    // the "I can eat up soul gems but the charge never goes up" report, and it
    // is a bug we introduced by putting a charge field on the card at all.
    //
    // ⚠ THE FIX IS THREE READ-ONLY CALL-SITE HOOKS AND NO SAVE WRITES. Each of
    // the three callees above is called exactly once inside AE 51859, so each
    // can be found by what it calls rather than by an offset, and each is
    // answered for the Seamstone only:
    //
    //     GetMaxCharge      -> our iSeamstoneCapacity, so the clamp passes
    //     GetChargePercent  -> what the stone really holds, so a top-up ADDS
    //     SetCharge         -> credit DyeUnlocks and DO NOT call the original
    //
    // Nothing is attached to the item, so nothing persists and turning the
    // setting off leaves no trace.
    //
    // ⚠ AND IT SHARES ONE KEY WITH THE CARD STAMP ON PURPOSE, `bItemCardCharge`.
    // The T Charge prompt appears because `ItemCardCharge` writes a charge field
    // onto the card. Card stamp on with this off is exactly the state that eats
    // gems for nothing, so the two must not be separately switchable, and
    // `ItemCardCharge::Install` refuses outright unless `Installed()` below is
    // true. All three hooks or none: a half-installed recharge is the same
    // destructive state by a different route.
    void Install();

    [[nodiscard]] bool Installed();

}  // namespace OS::SeamstoneRecharge
