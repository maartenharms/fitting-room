#pragma once
#include "PCH.h"

#include <cstdint>
#include <vector>

#include "SeamstoneCharge.h"

namespace OS::SoulGems {

    // The player's filled soul gems, and the one operation the Seamstone needs
    // on them: eat exactly one.
    //
    // ⚠ THIS IS THE ONLY FILE THAT KNOWS A SOUL GEM IS AN ENGINE OBJECT.
    // SeamstoneCharge.h prices souls and holds no engine types, DyeUnlocks
    // holds the charge, and this converts between the two. Keeping the walk
    // here rather than in EditorUI is what makes "which stack am I actually
    // destroying" a question with one answer.

    // One size of gem the player is carrying, and how many.
    struct Held {
        SeamstoneCharge::SoulSize size{ SeamstoneCharge::SoulSize::kNone };
        std::uint32_t             count{ 0 };
    };

    // MAIN THREAD. Every usable filled gem in the player's inventory, collapsed
    // to one row per soul size, biggest soul first.
    //
    // ⚠ WALKS STACKS, NOT FORMS, and that is not fussiness. A base form can
    // hold filled and empty instances at once: Skyrim swaps an empty gem for
    // its filled variant only on a FULL capacity fill, so a Grand gem holding a
    // Common soul stays the SAME form carrying an ExtraSoul, sitting in the
    // inventory beside genuinely empty ones. InventoryEntryData::GetSoulLevel
    // reports the first filled list it finds for the whole entry, so counting
    // by form would report five gems when one is filled, and then destroy an
    // empty one while paying out for a soul.
    //
    // ⚠ Called at editor open and NOT per frame. GetInventory builds a map of
    // the entire inventory, which is the cost this codebase already refuses to
    // pay in a draw loop.
    [[nodiscard]] std::vector<Held> Available();

    // MAIN THREAD. Destroy exactly one filled gem of a_size. Returns false if
    // none was found, in which case nothing was removed and the caller MUST NOT
    // credit any charge.
    //
    // ⚠ NO EMPTY GEM COMES BACK, which matches vanilla enchanting and is not an
    // oversight: TESSoulGem::linkedSoulGem points from empty to filled, so
    // there is no reverse lookup to find the empty variant with.
    [[nodiscard]] bool ConsumeOne(SeamstoneCharge::SoulSize a_size);

    // The display name for a size, for the refill list. Ours rather than the
    // form's: one row can stand for several different gem forms holding the
    // same soul, so no single item name is honest for it.
    [[nodiscard]] const char* Name(SeamstoneCharge::SoulSize a_size);

}  // namespace OS::SoulGems
