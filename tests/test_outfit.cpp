// Pure-logic tests for Outfit / SlotMask / EditHistory / FavoriteSet.
// No engine, no RE:: types.
#include "Favorites.h"
#include "FooterNotice.h"
#include "Outfit.h"
#include "OutfitTabs.h"
#include "PresetPreviewPolicy.h"
#include "SlotMask.h"

#include <cstdio>
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

int main() {
    using namespace OS;

    {  // Export feedback is bounded text, never an unbounded filesystem path.
        CHECK(FooterNotice::ExportResult(
                  "Data/SKSE/Plugins/FittingRoom/Exports/"
                  "Magedali White - Purple Royal.json",
                  "Exported",
                  "Export failed")
                  == "Exported");
        CHECK(FooterNotice::ExportResult("", "Exported", "Export failed") ==
              "Export failed");
    }

    {  // slot <-> bit mapping
        CHECK(BitForEditorSlot(30) == 0);
        CHECK(BitForEditorSlot(32) == 2);
        CHECK(BitForEditorSlot(46) == 16);
        CHECK(MaskForEditorSlot(32) == (1u << 2));
        CHECK(IsBodySkinBit(BitForEditorSlot(32)));
        CHECK(IsBodySkinBit(BitForEditorSlot(33)));
        CHECK(IsBodySkinBit(BitForEditorSlot(37)));
        CHECK(!IsBodySkinBit(BitForEditorSlot(31)));
        CHECK(IsHeadPartBit(BitForEditorSlot(31)));
        CHECK(IsHeadPartBit(BitForEditorSlot(42)));
        CHECK(!IsHeadPartBit(BitForEditorSlot(32)));
    }

    {  // First-person armor uses its own biped. Body and hand hides must
       // restage naked 1P skin there; feet/head hides remain third-person-only.
        const auto head     = MaskForEditorSlot(31);
        const auto body     = MaskForEditorSlot(32);
        const auto hands    = MaskForEditorSlot(33);
        const auto forearms = MaskForEditorSlot(34);
        const auto feet     = MaskForEditorSlot(37);
        CHECK(FirstPersonBodySkinHideMask(
                  head | body | hands | feet) ==
              (body | hands));

        // Honesty restoration is scoped to the 1P-visible slots touched by
        // either hiding or style injection, including ARMA forearm coverage.
        CHECK(FirstPersonArmorRestoreMask(
                  body | feet, hands | forearms) ==
              (body | hands | forearms));
    }

    {  // an empty outfit displays nothing of its own
        Outfit o;
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kPassthrough);
        CHECK(o.StyleMask() == 0);
        CHECK(o.HideMask() == 0);
    }

    {  // ORefit Auto follows the visible transmog torso, not replaced gear
        const auto body  = MaskForEditorSlot(32);
        const auto chest = MaskForEditorSlot(46);

        CHECK(ResolveAutoORefit(ORefitMode::kDefault, 0, 0, body) ==
              ORefitMode::kDefault);  // outfit makes no torso decision
        CHECK(ResolveAutoORefit(ORefitMode::kDefault, body, 0, 0) ==
              ORefitMode::kForceOn);  // styled torso looks clothed
        CHECK(ResolveAutoORefit(ORefitMode::kDefault, 0, body, body) ==
              ORefitMode::kForceOff);  // hidden torso ignores real body armor
        CHECK(ResolveAutoORefit(ORefitMode::kDefault, 0, body, body | chest) ==
              ORefitMode::kForceOn);  // passthrough chest remains visibly worn
        CHECK(ResolveAutoORefit(ORefitMode::kDefault, 0, body | chest, body | chest) ==
              ORefitMode::kForceOff);  // every occupied torso slot is hidden
        CHECK(ResolveAutoORefit(ORefitMode::kDefault, body, 0, 0, false) ==
              ORefitMode::kDefault);  // Auto never overrides OBody's global Off

        CHECK(ResolveAutoORefit(ORefitMode::kForceOn, 0, body, 0) ==
              ORefitMode::kForceOn);
        CHECK(ResolveAutoORefit(ORefitMode::kForceOn, 0, body, 0, false) ==
              ORefitMode::kForceOn);  // explicit override still means explicit
        CHECK(ResolveAutoORefit(ORefitMode::kForceOff, body, 0, body) ==
              ORefitMode::kForceOff);
    }

    {  // style + hide + passthrough masks
        Outfit o;
        o.SetStyle(2, StyleRefKey{ "Armors.esp", 0x800 });   // body
        o.SetHide(1);                                        // helmet
        CHECK(o.StyleMask() == (1u << 2));
        CHECK(o.HideMask() == (1u << 1));
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kStyle);
        CHECK(o.EntryFor(2).style.localFormID == 0x800);
        CHECK(o.EntryFor(0).kind == SlotEntry::Kind::kPassthrough);

        o.SetPassthrough(2);
        CHECK(o.StyleMask() == 0);
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kPassthrough);
    }

    {  // out-of-range bits are safe no-ops (mutators called from the biped rebuild)
        Outfit o;
        o.SetStyle(32, StyleRefKey{ "x.esp", 1 });   // == kBitCount, just past the end
        o.SetHide(99);
        CHECK(o.StyleMask() == 0);
        CHECK(o.HideMask() == 0);
        CHECK(o.EntryFor(32).kind == SlotEntry::Kind::kPassthrough);
        CHECK(o.EntryFor(99).kind == SlotEntry::Kind::kPassthrough);
        o.SetPassthrough(32);                        // must not corrupt anything
        CHECK(o.StyleMask() == 0);
    }

    {  // ForEachStyle visits exactly the styled bits, ascending, with the right keys
        Outfit o;
        o.SetStyle(5, StyleRefKey{ "b.esp", 0x22 });
        o.SetStyle(1, StyleRefKey{ "a.esp", 0x11 });
        o.SetHide(3);                                // must NOT be visited
        std::vector<std::uint32_t> bits;
        std::vector<StyleRefKey>   keys;
        o.ForEachStyle([&](std::uint32_t b, const StyleRefKey& k) {
            bits.push_back(b);
            keys.push_back(k);
        });
        CHECK(bits.size() == 2);
        CHECK(bits == (std::vector<std::uint32_t>{ 1, 5 }));   // ascending bit order
        CHECK(keys[0] == (StyleRefKey{ "a.esp", 0x11 }));
        CHECK(keys[1] == (StyleRefKey{ "b.esp", 0x22 }));

        int count = 0;
        Outfit empty;
        empty.ForEachStyle([&](std::uint32_t, const StyleRefKey&) { ++count; });
        CHECK(count == 0);
    }

    {  // computeDisplaySet: blocklist forces passthrough
        Outfit o;
        o.SetHide(2);
        const auto d = ComputeDisplaySet(o, /*blocklist*/ (1u << 2));
        CHECK(d.hideMask == 0);         // slot 2 was blocklisted
        CHECK(d.styleMask == 0);
    }

    {  // computeDisplaySet: hidden head-part bits are reported for the mask shim
        Outfit o;
        o.SetHide(1);                   // helmet (head part)
        o.SetHide(2);                   // body (not a head part)
        const auto d = ComputeDisplaySet(o, /*blocklist*/ 0);
        CHECK(d.hideMask == ((1u << 1) | (1u << 2)));
        CHECK(d.hiddenHeadPartMask == (1u << 1));
        CHECK(d.hiddenBodySkinMask == (1u << 2));
        CHECK(d.hiddenAttachmentMask == (1u << 1));   // helmet culls its node too
    }

    {  // shield styling is render-only: style is allowed, hide remains forbidden
        Outfit o;
        o.SetHide(kBitShield);
        const auto d1 = ComputeDisplaySet(o, /*blocklist*/ 0);
        CHECK((d1.hideMask & MaskForEditorSlot(39)) == 0);
        CHECK(d1.hiddenAttachmentMask == 0);

        Outfit o2;
        o2.SetStyle(kBitShield, StyleRefKey{ "x.esp", 1 });
        const auto d2 = ComputeDisplaySet(o2, 0);
        CHECK((d2.styleMask & MaskForEditorSlot(39)) != 0);

        // Unlike normal armor, selecting a shield style must not conjure a
        // shield when the actor has none equipped.
        CHECK(StyleRequiresWornItem(kBitShield));
        CHECK(!CanApplyStyleBit(kBitShield, 0));
        CHECK(CanApplyStyleBit(kBitShield, MaskForEditorSlot(39)));
        CHECK(!StyleRequiresWornItem(kBitBody));
        CHECK(CanApplyStyleBit(kBitBody, 0));

        // Regression: shield and off-hand weapon share biped object 9. When a
        // requested shield style is rejected because no real shield is worn,
        // slot 9 must not enter the armor-honesty restore mask. Otherwise the
        // naked-skin ARMO replaces the off-hand WEAP pointer and Skyrim crashes
        // on the next UpdateEquipment pass.
        std::uint32_t appliedCoverage = 0;
        if (CanApplyStyleBit(kBitShield, /*realWornMask*/ 0)) {
            appliedCoverage |= MaskForEditorSlot(39);
        }
        CHECK((PostPassArmorRestoreMask(/*hideMask*/ 0, appliedCoverage) &
               MaskForEditorSlot(39)) == 0);
        CHECK(PostPassArmorRestoreMask(MaskForEditorSlot(31),
                                       MaskForEditorSlot(32)) ==
              (MaskForEditorSlot(31) | MaskForEditorSlot(32)));
    }

    {  // library: ten saved slots, one active, rename, activate/deactivate
        CHECK(kMaxOutfits == 10);
        OutfitLibrary lib;
        CHECK(lib.Count() == 0);
        CHECK(lib.ActiveIndex() == -1);
        const int i = lib.Create("Tavern");
        CHECK(i == 0);
        CHECK(lib.Count() == 1);
        lib.Activate(0);
        CHECK(lib.ActiveIndex() == 0);
        CHECK(lib.Active() != nullptr);
        lib.Rename(0, "Court");
        CHECK(lib.At(0)->name == "Court");
        lib.Deactivate();
        CHECK(lib.ActiveIndex() == -1);
        CHECK(lib.Active() == nullptr);

        for (std::size_t k = 1; k < kMaxOutfits; ++k) {
            CHECK(lib.Create("x") == static_cast<int>(k));
        }
        CHECK(lib.Create("overflow") == -1);   // hard cap of kMaxOutfits
        CHECK(lib.Count() == kMaxOutfits);

        // The quick-switch/controller model includes Equipped gear outside
        // all ten saved slots and wraps cleanly at the new highest index.
        lib.Activate(kMaxOutfits - 1);
        CHECK(lib.CycleIncludingEquipped(true) == -1);
        CHECK(lib.CycleIncludingEquipped(true) == 0);
        CHECK(lib.CycleIncludingEquipped(false) == -1);
        CHECK(lib.CycleIncludingEquipped(false) ==
              static_cast<int>(kMaxOutfits - 1));
    }

    {  // immutable equipped-gear tab is logical tab 0, outside the outfit cap
        CHECK(OutfitTabs::LogicalFromActive(-1) == 0);
        CHECK(OutfitTabs::LogicalFromActive(0) == 1);
        CHECK(OutfitTabs::ActiveFromLogical(0) == -1);
        CHECK(OutfitTabs::ActiveFromLogical(3) == 2);
        CHECK(OutfitTabs::ForcedSelectionForActive(-1) ==
              OutfitTabs::kForceEquippedGear);
        CHECK(OutfitTabs::ForcedSelectionForActive(2) == 2);
        CHECK(!OutfitTabs::ShouldAcceptActivation(
            /*reported*/ -1, /*forced*/ 0));
        CHECK(OutfitTabs::ShouldAcceptActivation(
            /*reported*/ 0, /*forced*/ 0));
        CHECK(OutfitTabs::ShouldAcceptActivation(
            /*reported*/ -1, OutfitTabs::kNoForcedSelection));
        CHECK(!OutfitTabs::CanDeleteSaved(-1, 1));
        CHECK(OutfitTabs::CanDeleteSaved(0, 1));  // the final saved outfit is deletable
        CHECK(OutfitTabs::Cycle(/*active*/ -1, /*outfitCount*/ 2, true) == 0);
        CHECK(OutfitTabs::Cycle(/*active*/ 0, /*outfitCount*/ 2, true) == 1);
        CHECK(OutfitTabs::Cycle(/*active*/ 1, /*outfitCount*/ 2, true) == -1);
        CHECK(OutfitTabs::Cycle(/*active*/ -1, /*outfitCount*/ 2, false) == 1);
        CHECK(OutfitTabs::Cycle(/*active*/ -1, /*outfitCount*/ 0, true) == -1);
        CHECK(OutfitTabs::Cycle(/*active*/ -1, /*outfitCount*/ 0, false) == -1);
    }

    {  // The outfit strip maps vertical wheel motion to the same one-step
       // selection path as LB/RB, whether or not every tab currently fits.
       // Wheel down moves right/next; wheel up moves left/previous. Moving the
       // pointer off the strip leaves selection alone.
        const auto next = OutfitTabs::WheelCycleRequest(
            /*active*/ 8, /*outfitCount*/ 10, /*wheel*/ -1.0f,
            /*stripHovered*/ true);
        CHECK(next.has_value());
        CHECK(*next == 9);

        const auto wrapToEquipped = OutfitTabs::WheelCycleRequest(
            /*active*/ 9, /*outfitCount*/ 10, /*wheel*/ -1.0f,
            /*stripHovered*/ true);
        CHECK(wrapToEquipped.has_value());
        CHECK(*wrapToEquipped == -1);

        const auto previous = OutfitTabs::WheelCycleRequest(
            /*active*/ -1, /*outfitCount*/ 10, /*wheel*/ 1.0f,
            /*stripHovered*/ true);
        CHECK(previous.has_value());
        CHECK(*previous == 9);

        CHECK(!OutfitTabs::WheelCycleRequest(
            8, 10, -1.0f, /*stripHovered*/ false));
        const auto fitsButStillCycles = OutfitTabs::WheelCycleRequest(
            1, 3, -1.0f, /*stripHovered*/ true);
        CHECK(fitsButStillCycles.has_value());
        if (fitsButStillCycles) {
            CHECK(*fitsButStillCycles == 2);
        }
        CHECK(!OutfitTabs::WheelCycleRequest(
            8, 10, 0.0f, /*stripHovered*/ true));
        CHECK(!OutfitTabs::WheelCycleRequest(
            -1, 0, -1.0f, /*stripHovered*/ true));
    }

    {  // follower mannequin: styles survive, every other hideable slot goes bare
        Outfit follower;
        follower.name = "Follower";
        follower.SetStyle(kBitBody, StyleRefKey{ "FollowerArmor.esp", 0x123 });
        follower.SetStyle(kBitShield, StyleRefKey{ "FollowerArmor.esp", 0x456 });
        follower.obodyPreset = "Follower body preview";
        follower.orefit      = ORefitMode::kForceOn;

        const auto mannequin = MakeMannequinPreview(
            follower, MaskForEditorSlot(44));  // configured blocklist remains untouched
        CHECK(mannequin.EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
        CHECK(mannequin.EntryFor(kBitShield).kind == SlotEntry::Kind::kStyle);
        CHECK(mannequin.EntryFor(BitForEditorSlot(33)).kind == SlotEntry::Kind::kHide);
        CHECK(mannequin.EntryFor(BitForEditorSlot(44)).kind ==
              SlotEntry::Kind::kPassthrough);
        CHECK(mannequin.obodyPreset == "Follower body preview");
        CHECK(mannequin.orefit == ORefitMode::kForceOn);
        CHECK(follower.obodyPreset == "Follower body preview");

        const auto suppressed =
            PresetPreviewPolicy::MannequinSuppressedBipedObjects(follower);
        CHECK((suppressed & (1ull << BipedSlotForClass(WeaponClass::Sword))) != 0);
        follower.SetWeaponStyle(WeaponClass::Sword,
                                StyleRefKey{ "FollowerWeapons.esp", 0x789 });
        const auto withSword =
            PresetPreviewPolicy::MannequinSuppressedBipedObjects(follower);
        CHECK((withSword & (1ull << BipedSlotForClass(WeaponClass::Sword))) == 0);
        CHECK((withSword & (1ull << BipedSlotForClass(WeaponClass::Bow))) != 0);
        CHECK(!PresetPreviewPolicy::HighlightPresetRow(
            /*exported*/ true, 0, 0, false));
        CHECK(PresetPreviewPolicy::HighlightPresetRow(
            /*exported*/ true, 1, 0, false));
        CHECK(PresetPreviewPolicy::HighlightPresetRow(
            /*exported*/ true, 0, 2, false));
        CHECK(PresetPreviewPolicy::HighlightPresetRow(
            /*exported*/ false, 0, 0, true));
        CHECK(!PresetPreviewPolicy::HighlightPresetRow(
            /*exported*/ false, 1, 2, false));  // curated filtering is store-owned
    }

    {  // Equipped gear previews the follower's captured gear, not the empty stage
        Outfit equipped;
        equipped.SetStyle(kBitBody, StyleRefKey{ "FollowerArmor.esp", 0x123 });
        Outfit inactiveStage;
        const auto source =
            ComposeMannequinSource(/*actualGear*/ true, equipped, inactiveStage);
        CHECK(source.EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
    }

    {  // A fresh mutable follower outfit visually starts from captured gear.
       // Explicit edits overlay it, while the empty saved record stays empty.
        Outfit equipped;
        equipped.SetStyle(kBitBody, StyleRefKey{ "FollowerArmor.esp", 0x123 });
        equipped.SetStyle(kBitHair, StyleRefKey{ "FollowerHair.esp", 0x456 });
        Outfit fresh;
        fresh.name = "Outfit 1";
        fresh.SetHide(kBitHair);
        fresh.SetWeaponStyle(WeaponClass::Bow,
                             StyleRefKey{ "FollowerWeapons.esp", 0x789 });

        const auto source =
            ComposeMannequinSource(/*actualGear*/ false, equipped, fresh);
        CHECK(source.EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
        CHECK(source.EntryFor(kBitBody).style.modName == "FollowerArmor.esp");
        CHECK(source.EntryFor(kBitHair).kind == SlotEntry::Kind::kHide);
        CHECK(source.WeaponEntryFor(WeaponClass::Bow).kind ==
              SlotEntry::Kind::kStyle);
        CHECK(fresh.EntryFor(kBitBody).kind == SlotEntry::Kind::kPassthrough);
        CHECK(fresh.EntryFor(kBitHair).kind == SlotEntry::Kind::kHide);
    }

    {  // External quick-switch includes immutable Equipped gear.
        OutfitLibrary lib;
        CHECK(lib.CycleIncludingEquipped(true) == -1);  // zero-count is stable
        lib.Create("a");
        lib.Create("b");
        CHECK(lib.CycleIncludingEquipped(true) == 0);   // Equipped -> a
        CHECK(lib.CycleIncludingEquipped(true) == 1);   // a -> b
        CHECK(lib.CycleIncludingEquipped(true) == -1);  // b -> Equipped
        CHECK(lib.Active() == nullptr);
        CHECK(lib.CycleIncludingEquipped(false) == 1);  // Equipped -> b backwards
    }

    {  // deleting an outfit clears the active index if it pointed at it
        OutfitLibrary lib;
        lib.Create("a");
        lib.Create("b");
        lib.Activate(1);
        lib.Remove(1);
        CHECK(lib.ActiveIndex() == -1);
        CHECK(lib.Count() == 1);
    }

    {  // delete-final lands deterministically on immutable Equipped gear
        OutfitLibrary lib;
        lib.Create("only");
        lib.Activate(0);
        CHECK(lib.RemoveAndSelectNeighbor(0) == -1);
        CHECK(lib.Count() == 0);
        CHECK(lib.ActiveIndex() == -1);
        CHECK(lib.Active() == nullptr);
        CHECK(OutfitTabs::ForcedSelectionForActive(lib.ActiveIndex()) ==
              OutfitTabs::kForceEquippedGear);
    }

    {  // deleting the active outfit selects the nearest saved neighbor
        OutfitLibrary lib;
        lib.Create("a");
        lib.Create("b");
        lib.Create("c");
        lib.Activate(1);
        CHECK(lib.RemoveAndSelectNeighbor(1) == 1);
        CHECK(lib.Count() == 2);
        CHECK(lib.At(1) != nullptr);
        CHECK(lib.At(1)->name == "c");
        CHECK(lib.Active() == lib.At(1));
    }

    {  // removing below the active index decrements it; it still names the same outfit
        OutfitLibrary lib;
        lib.Create("a");
        lib.Create("b");
        lib.Create("c");
        lib.Activate(2);
        lib.Remove(0);
        CHECK(lib.ActiveIndex() == 1);
        CHECK(lib.At(1)->name == "c");
        CHECK(lib.Count() == 2);
    }

    {  // removing above the active index leaves it unchanged
        OutfitLibrary lib;
        lib.Create("a");
        lib.Create("b");
        lib.Create("c");
        lib.Activate(0);
        lib.Remove(2);
        CHECK(lib.ActiveIndex() == 0);
        CHECK(lib.Count() == 2);
    }

    {  // Move: the active outfit follows its entry, neighbors shift correctly
        OutfitLibrary lib;
        lib.Create("a");
        lib.Create("b");
        lib.Create("c");
        lib.Activate(2);          // active = "c"
        lib.Move(2, 0);           // c a b
        CHECK(lib.At(0)->name == "c");
        CHECK(lib.At(1)->name == "a");
        CHECK(lib.At(2)->name == "b");
        CHECK(lib.ActiveIndex() == 0);
        lib.Activate(1);          // active = "a"
        lib.Move(0, 2);           // a b c -> moving "c" right past "a": a b c? -> order: a b c
        CHECK(lib.At(2)->name == "c");
        CHECK(lib.ActiveIndex() == 0);  // "a" shifted left with the move
        lib.Move(5, 0);                 // out of range: no-op
        lib.Move(1, 1);                 // same index: no-op
        CHECK(lib.Count() == 3);
        CHECK(lib.At(0)->name == "a");
    }

    {  // removing an out-of-range index is a harmless no-op
        OutfitLibrary lib;
        lib.Create("a");
        lib.Create("b");
        lib.Activate(1);
        lib.Remove(5);
        CHECK(lib.Count() == 2);
        CHECK(lib.ActiveIndex() == 1);
    }

    {  // ChangedSlotCount: only value-differences count, styles compare by key
        Outfit base;
        base.SetStyle(2, StyleRefKey{ "a.esp", 0x10 });
        base.SetHide(1);
        Outfit staged = base;
        CHECK(ChangedSlotCount(base, staged) == 0);      // identical
        staged.SetStyle(2, StyleRefKey{ "a.esp", 0x11 }); // different key
        CHECK(ChangedSlotCount(base, staged) == 1);
        staged.SetStyle(2, StyleRefKey{ "a.esp", 0x10 }); // back to baseline key
        CHECK(ChangedSlotCount(base, staged) == 0);
        staged.SetPassthrough(1);                         // un-hide slot 1
        CHECK(ChangedSlotCount(base, staged) == 1);
    }

    {  // ToggleHideSlot round trip returns a STYLED slot to its style -
        // the Apply-cost regression: Hide then Show must net zero cost.
        Outfit base;
        base.SetStyle(2, StyleRefKey{ "Armors.esp", 0x800 });   // committed styled slot
        Outfit staged = base;
        CHECK(ChangedSlotCount(base, staged) == 0);

        ToggleHideSlot(staged, 2);                              // Hide
        CHECK(staged.EntryFor(2).kind == SlotEntry::Kind::kHide);
        CHECK(staged.EntryFor(2).style ==
              (StyleRefKey{ "Armors.esp", 0x800 }));            // covered style travels with outfit
        CHECK(ChangedSlotCount(base, staged) == 1);             // hiding is a real change

        ToggleHideSlot(staged, 2);                              // Show
        CHECK(staged.EntryFor(2).kind == SlotEntry::Kind::kStyle);
        CHECK(staged.EntryFor(2).style == (StyleRefKey{ "Armors.esp", 0x800 }));
        CHECK(ChangedSlotCount(base, staged) == 0);             // cost reset to zero
    }

    {  // switching tabs is not a pending edit: the frame-start snapshot may
       // still hold the old active outfit, but its structural difference from
       // the newly staged saved outfit must never flash a lore-friendly price.
        CHECK(PendingGoldCost(/*hasPendingEdits*/ false,
                              /*charging*/ true,
                              /*changedSlots*/ 4,
                              /*costPerSlot*/ 100) == 0);
        CHECK(PendingGoldCost(/*hasPendingEdits*/ true,
                              /*charging*/ true,
                              /*changedSlots*/ 4,
                              /*costPerSlot*/ 100) == 400);
        CHECK(PendingGoldCost(/*hasPendingEdits*/ true,
                              /*charging*/ false,
                              /*changedSlots*/ 4,
                              /*costPerSlot*/ 100) == 0);
    }

    {  // ToggleHideSlot on a passthrough slot round-trips to passthrough
        Outfit o;
        ToggleHideSlot(o, 5);                                   // Hide
        CHECK(o.EntryFor(5).kind == SlotEntry::Kind::kHide);
        CHECK(o.EntryFor(5).style.Empty());
        ToggleHideSlot(o, 5);                                   // Show
        CHECK(o.EntryFor(5).kind == SlotEntry::Kind::kPassthrough);
    }

    {  // re-stashing on each Hide: pick a new style while hidden, toggle again
        Outfit o;
        o.SetStyle(3, StyleRefKey{ "a.esp", 0x1 });
        ToggleHideSlot(o, 3);                                   // Hide (retain A)
        o.SetStyle(3, StyleRefKey{ "b.esp", 0x2 });            // pick B from browser
        ToggleHideSlot(o, 3);                                   // Hide (retain B)
        ToggleHideSlot(o, 3);                                   // Show -> B, not A
        CHECK(o.EntryFor(3).style == (StyleRefKey{ "b.esp", 0x2 }));
    }

    {  // EditHistory: linear undo/redo over staged-outfit snapshots (OS-21)
        EditHistory h;
        Outfit      a;                                          // baseline (empty)
        h.Reset(a);
        CHECK(h.Size() == 1);
        CHECK(!h.CanUndo());
        CHECK(!h.CanRedo());

        Outfit b = a;
        b.SetStyle(2, StyleRefKey{ "m.esp", 0x1 });
        h.Record(b);
        CHECK(h.CanUndo());
        CHECK(!h.CanRedo());

        Outfit c = b;
        c.SetStyle(7, StyleRefKey{ "m.esp", 0x2 });
        h.Record(c);
        CHECK(h.Size() == 3);

        CHECK(ChangedSlotCount(h.Undo(), b) == 0);              // walk back to b
        CHECK(h.CanRedo());
        CHECK(ChangedSlotCount(h.Undo(), a) == 0);              // ...then to a
        CHECK(!h.CanUndo());
        CHECK(ChangedSlotCount(h.Undo(), a) == 0);              // undo past start is a no-op
        CHECK(ChangedSlotCount(h.Redo(), b) == 0);              // forward to b
        CHECK(ChangedSlotCount(h.Redo(), c) == 0);              // ...then c
        CHECK(!h.CanRedo());
        CHECK(ChangedSlotCount(h.Redo(), c) == 0);              // redo past end is a no-op
    }

    {  // EditHistory: idempotent Record dropped; a new edit truncates the redo tail
        EditHistory h;
        Outfit      a;
        h.Reset(a);
        Outfit b = a;
        b.SetStyle(2, StyleRefKey{ "m.esp", 0x1 });
        h.Record(b);
        h.Record(b);                                            // same slots - no new entry
        CHECK(h.Size() == 2);

        h.Undo();                                               // back to a; b is now a redo
        CHECK(h.CanRedo());
        Outfit d = a;
        d.SetStyle(5, StyleRefKey{ "m.esp", 0x9 });
        h.Record(d);                                            // a fresh branch drops the b redo
        CHECK(!h.CanRedo());
        CHECK(ChangedSlotCount(h.Current(), d) == 0);
    }

    {  // EditHistory: bounded to kCap - the oldest snapshots drop, cursor stays valid
        EditHistory h;
        Outfit      o;
        h.Reset(o);
        for (std::uint32_t i = 1; i <= EditHistory::kCap + 5; ++i) {
            o.SetStyle(2, StyleRefKey{ "m.esp", i });          // each edit distinct
            h.Record(o);
        }
        CHECK(h.Size() == EditHistory::kCap);
        CHECK(h.CanUndo());
        CHECK(!h.CanRedo());
        CHECK(ChangedSlotCount(h.Current(), o) == 0);           // newest state intact at the cursor
    }

    {  // FavoriteSet: toggle / contains / add / remove (OS-22)
        FavoriteSet       fs;
        const StyleRefKey k1{ "Armors.esp", 0x800 };
        const StyleRefKey k2{ "Other.esp", 0x14 };
        CHECK(!fs.Contains(k1));
        CHECK(fs.Toggle(k1) == true);                           // starred
        CHECK(fs.Contains(k1));
        CHECK(fs.Size() == 1);
        CHECK(fs.Toggle(k1) == false);                          // un-starred
        CHECK(!fs.Contains(k1));
        CHECK(fs.Size() == 0);

        fs.Add(k1);
        fs.Add(k2);
        fs.Add(k1);                                             // idempotent add
        CHECK(fs.Size() == 2);
        fs.Remove(k2);
        CHECK(fs.Size() == 1);
        CHECK(fs.Contains(k1));
        CHECK(!fs.Contains(k2));

        CHECK(fs.Toggle(StyleRefKey{}) == false);               // empty key never stored
        CHECK(!fs.Contains(StyleRefKey{}));
        CHECK(fs.Size() == 1);

        CHECK(FavoriteSet::KeyLine(k1) == std::string("Armors.esp|2048"));  // 0x800 == 2048
    }

    {  // weapon entries: set/get style, hide, passthrough per class (weapon
       // transmog stage 1) - same SlotEntry kind as armor, separate array
        Outfit o;
        o.SetWeaponStyle(WeaponClass::Sword, StyleRefKey{ "Weapons.esp", 0x900 });
        CHECK(o.WeaponEntryFor(WeaponClass::Sword).kind == SlotEntry::Kind::kStyle);
        CHECK(o.WeaponEntryFor(WeaponClass::Sword).style.localFormID == 0x900);
        CHECK(o.WeaponEntryFor(WeaponClass::Bow).kind == SlotEntry::Kind::kPassthrough);

        o.SetWeaponHide(WeaponClass::Bow);
        CHECK(o.WeaponEntryFor(WeaponClass::Bow).kind == SlotEntry::Kind::kHide);
        CHECK(o.WeaponEntryFor(WeaponClass::Sword).kind == SlotEntry::Kind::kStyle);  // unaffected

        o.SetWeaponPassthrough(WeaponClass::Sword);
        CHECK(o.WeaponEntryFor(WeaponClass::Sword).kind == SlotEntry::Kind::kPassthrough);
    }

    {  // Same-class dual wield: old Both remains the fallback; each hand can
       // override independently, including explicit real-weapon passthrough.
        Outfit o;
        o.SetWeaponStyle(WeaponClass::Sword, { "Both.esp", 1 });
        CHECK(o.ResolvedWeaponEntryFor(WeaponClass::Sword, WeaponHand::Right).style.modName ==
              "Both.esp");
        CHECK(o.ResolvedWeaponEntryFor(WeaponClass::Sword, WeaponHand::Left).style.modName ==
              "Both.esp");
        CHECK(!o.WeaponOverrideFor(WeaponClass::Sword, WeaponHand::Right));

        o.SetWeaponStyle(WeaponClass::Sword, { "Right.esp", 2 }, WeaponHand::Right);
        o.SetWeaponPassthrough(WeaponClass::Sword, WeaponHand::Left);
        CHECK(o.ResolvedWeaponEntryFor(WeaponClass::Sword, WeaponHand::Right).style.modName ==
              "Right.esp");
        CHECK(o.WeaponOverrideFor(WeaponClass::Sword, WeaponHand::Left).has_value());
        CHECK(o.ResolvedWeaponEntryFor(WeaponClass::Sword, WeaponHand::Left).kind ==
              SlotEntry::Kind::kPassthrough);
        CHECK(AnyWeaponEntry(o));

        o.ClearWeaponHandOverride(WeaponClass::Sword, WeaponHand::Left);
        CHECK(!o.WeaponOverrideFor(WeaponClass::Sword, WeaponHand::Left));
        CHECK(o.ResolvedWeaponEntryFor(WeaponClass::Sword, WeaponHand::Left).style.modName ==
              "Both.esp");

        // Selecting a NEW value while editing Both is a replacement action,
        // not merely a fallback edit: it deliberately removes both explicit
        // hand overrides so the choice is visible on both weapons.
        o.SetWeaponStyleForSelection(
            WeaponClass::Sword, { "NewBoth.esp", 3 }, WeaponHand::Both);
        CHECK(!o.WeaponOverrideFor(WeaponClass::Sword, WeaponHand::Right));
        CHECK(!o.WeaponOverrideFor(WeaponClass::Sword, WeaponHand::Left));
        CHECK(o.ResolvedWeaponEntryFor(
                  WeaponClass::Sword, WeaponHand::Right).style.modName ==
              "NewBoth.esp");
        CHECK(o.ResolvedWeaponEntryFor(
                  WeaponClass::Sword, WeaponHand::Left).style.modName ==
              "NewBoth.esp");

        o.SetWeaponStyle(
            WeaponClass::Sword, { "RightAgain.esp", 4 }, WeaponHand::Right);
        o.SetWeaponPassthroughForSelection(
            WeaponClass::Sword, WeaponHand::Both);
        CHECK(!o.WeaponOverrideFor(WeaponClass::Sword, WeaponHand::Right));
        CHECK(o.ResolvedWeaponEntryFor(
                  WeaponClass::Sword, WeaponHand::Right).kind ==
              SlotEntry::Kind::kPassthrough);
    }

    {  // Lore cost counts Both once and each optional hand override once.
        Outfit base;
        Outfit edited = base;
        edited.SetWeaponStyle(WeaponClass::Sword, { "Both.esp", 1 });
        CHECK(ChangedSlotCount(base, edited) == 1);
        edited.SetWeaponStyle(WeaponClass::Sword, { "Right.esp", 2 }, WeaponHand::Right);
        CHECK(ChangedSlotCount(base, edited) == 2);
        edited.SetWeaponPassthrough(WeaponClass::Sword, WeaponHand::Left);
        CHECK(ChangedSlotCount(base, edited) == 3);
        edited.ClearWeaponHandOverride(WeaponClass::Sword, WeaponHand::Right);
        CHECK(ChangedSlotCount(base, edited) == 2);
    }

    {  // ForEachWeaponStyle visits exactly the styled classes, ascending, with the right keys
        Outfit o;
        o.SetWeaponStyle(WeaponClass::Bow, StyleRefKey{ "b.esp", 0x22 });
        o.SetWeaponStyle(WeaponClass::Dagger, StyleRefKey{ "a.esp", 0x11 });
        o.SetWeaponHide(WeaponClass::Mace);   // must NOT be visited
        std::vector<WeaponClass> classes;
        std::vector<StyleRefKey> keys;
        o.ForEachWeaponStyle([&](WeaponClass c, const StyleRefKey& k) {
            classes.push_back(c);
            keys.push_back(k);
        });
        CHECK(classes.size() == 2);
        CHECK(classes == (std::vector<WeaponClass>{ WeaponClass::Dagger, WeaponClass::Bow }));
        CHECK(keys[0] == (StyleRefKey{ "a.esp", 0x11 }));
        CHECK(keys[1] == (StyleRefKey{ "b.esp", 0x22 }));

        int count = 0;
        Outfit empty;
        empty.ForEachWeaponStyle([&](WeaponClass, const StyleRefKey&) { ++count; });
        CHECK(count == 0);
    }

    {  // ChangedSlotCount: weapon diffs count exactly like armor diffs
        Outfit base;
        base.SetWeaponStyle(WeaponClass::Sword, StyleRefKey{ "a.esp", 0x10 });
        Outfit staged = base;
        CHECK(ChangedSlotCount(base, staged) == 0);                          // identical
        staged.SetWeaponStyle(WeaponClass::Sword, StyleRefKey{ "a.esp", 0x11 });  // different key
        CHECK(ChangedSlotCount(base, staged) == 1);
        staged.SetWeaponStyle(WeaponClass::Sword, StyleRefKey{ "a.esp", 0x10 });  // back to baseline
        CHECK(ChangedSlotCount(base, staged) == 0);
        staged.SetWeaponHide(WeaponClass::Bow);                               // new weapon-only change
        CHECK(ChangedSlotCount(base, staged) == 1);
    }

    {  // ChangedSlotCount: armor and weapon diffs accumulate independently
        Outfit base;
        base.SetStyle(2, StyleRefKey{ "arm.esp", 0x1 });
        base.SetWeaponStyle(WeaponClass::Bow, StyleRefKey{ "wpn.esp", 0x2 });
        Outfit staged = base;
        CHECK(ChangedSlotCount(base, staged) == 0);
        staged.SetHide(2);                        // armor change
        staged.SetWeaponHide(WeaponClass::Bow);    // weapon change
        CHECK(ChangedSlotCount(base, staged) == 2);
    }

    {  // armor-only outfits are unaffected: default (all-passthrough) weapon
       // arrays compare equal and contribute nothing to ChangedSlotCount
        Outfit base;
        base.SetStyle(1, StyleRefKey{ "arm.esp", 0x1 });
        base.SetHide(4);
        Outfit staged = base;
        CHECK(ChangedSlotCount(base, staged) == 0);            // identical armor, untouched weapons
        staged.SetStyle(1, StyleRefKey{ "arm.esp", 0x2 });      // armor-only edit
        CHECK(ChangedSlotCount(base, staged) == 1);             // weapons contribute nothing extra
    }

    {  // EditHistory: weapon-only edits are recorded, and Undo restores the
       // previous weapon entry (OS-21 extended to the weapon dimension)
        EditHistory h;
        Outfit      a;                                          // baseline (empty)
        h.Reset(a);
        CHECK(!h.CanUndo());

        Outfit b = a;
        b.SetWeaponStyle(WeaponClass::Crossbow, StyleRefKey{ "w.esp", 0x1 });
        h.Record(b);
        CHECK(h.CanUndo());
        CHECK(h.Current().WeaponEntryFor(WeaponClass::Crossbow).style ==
              (StyleRefKey{ "w.esp", 0x1 }));

        Outfit c = b;
        c.SetWeaponHide(WeaponClass::Crossbow);
        h.Record(c);
        CHECK(h.Current().WeaponEntryFor(WeaponClass::Crossbow).kind == SlotEntry::Kind::kHide);

        const Outfit& undone = h.Undo();
        CHECK(undone.WeaponEntryFor(WeaponClass::Crossbow).kind == SlotEntry::Kind::kStyle);
        CHECK(undone.WeaponEntryFor(WeaponClass::Crossbow).style == (StyleRefKey{ "w.esp", 0x1 }));
    }

    {  // AnyWeaponEntry: the predicate behind the session's weapon fast path.
       // A false here is a silently dead feature, so pin every kind and the
       // full class range - and pin that armor styling alone never trips it.
        Outfit o;
        CHECK(!AnyWeaponEntry(o));  // fresh outfit: all-passthrough

        o.SetStyle(0, StyleRefKey{ "a.esp", 0x1 });  // armor is a separate dimension
        o.SetHide(5);
        CHECK(!AnyWeaponEntry(o));

        o.SetWeaponStyle(WeaponClass::Sword, StyleRefKey{ "w.esp", 0x1 });
        CHECK(AnyWeaponEntry(o));
        o.SetWeaponPassthrough(WeaponClass::Sword);
        CHECK(!AnyWeaponEntry(o));

        o.SetWeaponHide(WeaponClass::Bow);  // hiding is styling too
        CHECK(AnyWeaponEntry(o));
        o.SetWeaponPassthrough(WeaponClass::Bow);
        CHECK(!AnyWeaponEntry(o));

        // Every class must be seen - a loop that stops short (or a class
        // appended past the count) would strand the last ones as dead.
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto c = static_cast<WeaponClass>(i);
            Outfit     one;
            one.SetWeaponStyle(c, StyleRefKey{ "w.esp", 0x1 });
            CHECK(AnyWeaponEntry(one));
        }
    }

    {  // FavoriteSet: serialize/load round-trips, tolerant of CRLF and blank lines
        FavoriteSet fs;
        fs.Add(StyleRefKey{ "a.esp", 0x1 });
        fs.Add(StyleRefKey{ "b.esp", 0x2 });

        FavoriteSet fs2;
        fs2.LoadLines(fs.Serialize());
        CHECK(fs2.Size() == 2);
        CHECK(fs2.Contains(StyleRefKey{ "a.esp", 0x1 }));
        CHECK(fs2.Contains(StyleRefKey{ "b.esp", 0x2 }));

        FavoriteSet fs3;
        fs3.LoadLines("a.esp|1\r\n\r\nb.esp|2\n");             // CRLF, blank line, trailing NL
        CHECK(fs3.Size() == 2);
        CHECK(fs3.Contains(StyleRefKey{ "a.esp", 0x1 }));
        CHECK(fs3.Contains(StyleRefKey{ "b.esp", 0x2 }));

        FavoriteSet fs4;
        fs4.LoadLines("");                                      // empty blob clears
        CHECK(fs4.Size() == 0);
    }

    {  // Preset browsing hides only render objects that can spoil an outfit
       // preview: shield plus every weapon/quiver biped object. The mask is
       // transient and never becomes an Outfit entry.
        const auto mask = PresetPreviewPolicy::kSuppressedBipedObjects;
        CHECK((mask & (1ull << kBitShield)) != 0);
        for (std::uint32_t slot = 32; slot <= 41; ++slot) {
            CHECK((mask & (1ull << slot)) != 0);
        }
        CHECK((mask & (1ull << kBitBody)) == 0);
        CHECK((mask & (1ull << 31)) == 0);
    }

    if (g_failures == 0) {
        std::printf("OutfitTests: all passed\n");
        return 0;
    }
    std::printf("OutfitTests: %d failure(s)\n", g_failures);
    return 1;
}
