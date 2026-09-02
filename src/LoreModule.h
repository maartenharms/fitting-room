#pragma once
#include "PCH.h"

namespace OS::LoreModule {

    // The optional lore addon (FittingRoomLore.esp): the Seamstone, a
    // carryable Alteration focus, sold by Farengar. The DLL NEVER requires the
    // ESP - absent, the mod runs in pure-UI mode and the editor opens from the
    // inventory as before.
    //
    // ⚠ THE ESP ALSO CARRIES A BOOK, 0x801, AND NOTHING SHIPS IT TO ANYONE.
    // It was retired in 2026-07-18 because reading it does nothing yet, and
    // OnPostLoadGame reclaims the copies older builds pushed. The FORM stays
    // because saves made against those builds reference it and the id has to
    // keep resolving; it is out of the installer text and out of the user's
    // way, and it comes back if the Seamstone ever gets the writing it needs.
    // ⛔ Do not deliver it, stock it, or describe it to a player.
    //
    // The Seamstone is a plain MISC item (user direction; the earlier forms
    // were proven broken in-game: slot-61 ARMO is hidden from every item
    // list, a zero-effect ALCH CTDs ItemCardPopulate). The editor opens
    // ONLY from the inventory (user rule, every mode - the hotkey and the
    // stone-use path share the gate); misc-interaction mods that route
    // "use" through the equip pipeline open it via our equip sink. Present
    // + [General] bLoreMode: opening additionally requires the stone in
    // the player's inventory, and Apply charges gold per changed slot
    // (iGoldPerSlot).

    void Init();  // kDataLoaded: resolve the ESP + forms, register the equip sink
    void OnPostLoadGame();  // reclaim the retired note; ask for a stock check

    // ⚠⚠ STOCKING FARENGAR IS NOT A LOAD-TIME JOB AND HAS NOT BEEN SINCE
    // 2026-08-28. There are three moments now and the last one is the one that
    // works: the game finishing a load, Farengar's cell attaching, and a
    // conversation or a barter opening while he is standing there. The first
    // is never delivered to a character started with `coc` from the main menu,
    // and the second fires while the engine is still bringing his merchant
    // chest up. Only the third happens reliably, late, and every time. All
    // three go through the task queue, so none of them runs inside a cell
    // attach, and every one of them reads the chest back and says what it
    // found. LoreModule.cpp carries the field evidence.

    [[nodiscard]] bool Available();     // ESP + forms resolved
    [[nodiscard]] bool HasSeamstone();  // the player carries a Seamstone

    // ---- the gate, in one place ------------------------------------------
    //
    // ⚠ THESE TWO EXIST BECAUSE THE EXPRESSION WAS BEING SPELLED OUT AT ITS
    // CALL SITE, and only one call site had it. The hotkey composed
    // `!(requireSeamstone && Available()) || HasSeamstone()` inline while the
    // action-bar button and the Papyrus open path checked nothing at all, so
    // the requirement was decorative: refused at the key, granted by the
    // button. Anything that can open the editor asks GateSatisfied now.

    // Whether the Seamstone requirement is in force at all: the setting is on
    // AND the ESP that supplies the stone is actually loaded. False in pure-UI
    // mode, where the stone does not exist and cannot be asked for.
    [[nodiscard]] bool RequirementActive();

    // Whether the player may open the editor under that requirement. Always
    // true when the requirement is not active.
    [[nodiscard]] bool GateSatisfied();

    // ⚠ ContextNotification IS GONE (user 2026-08-11). It answered "what to
    // say when the editor is asked for from somewhere it cannot open", and the
    // answer is now nothing: the three refusal sites log and draw no message,
    // the same call the seamstone refusal took on 2026-08-07.
    //
    // The notifications that remain all report something the player just DID
    // (an outfit swap, a refusal during a scene). This one reported a thing
    // they had not done, on a key they may have pressed by accident, for the
    // whole life of the save. If a hint about the inventory and Screen Archer
    // Menu ever comes back it belongs where they went looking for it, in the
    // settings panel beside the hotkey.

    // The Seamstone base form, or null in pure-UI mode. For code that has to
    // recognise the stone among other items rather than merely ask whether the
    // player has one (the item-card charge hook compares against this).
    [[nodiscard]] RE::TESObjectMISC* Stone();

}  // namespace OS::LoreModule
