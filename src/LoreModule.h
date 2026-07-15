#pragma once
#include "PCH.h"

namespace OS::LoreModule {

    // The optional lore addon (FittingRoomLore.esp): the Seamstone, a
    // carryable Alteration focus, plus the note "On the Outward Art" that
    // explains it and points to Farengar (who keeps one in stock). The DLL
    // NEVER requires the ESP - absent, the mod runs in pure-UI mode and the
    // editor opens from the inventory as before.
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
    void OnPostLoadGame();  // hand the player the note once; stock Farengar

    [[nodiscard]] bool Available();     // ESP + forms resolved
    [[nodiscard]] bool HasSeamstone();  // the player carries a Seamstone

}  // namespace OS::LoreModule
