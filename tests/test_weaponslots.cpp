// Pure-logic tests for the weapon-class mappings (weapon + quiver transmog,
// stage 1). No engine, no RE:: types.
#include "WeaponPreview.h"
#include "WeaponSlots.h"

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string_view>

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
    {  // ---- rebuilding a visual-only weapon ------------------------------
       // ⚠ EVERY MAIN-HAND SLOT, NOT THE THREE BACK ONES. This started as
       // {37, 38, 40} on the reasoning that only an intrinsically two-handed or
       // back slot names its hand without guessing, so a follower carrying an
       // engine-owned weapon that is absent from inventory could have a default
       // BOW restyled and nothing else. A sword, dagger, war axe, mace or staff
       // in the same situation was refused and simply never transmogged (user
       // 2026-08-07, "this can happen not just for a bow").
       //
       // The guess was never needed. This header's own IsMainHandWeaponBipedSlot
       // states the rule: AttachWeapon stages the main hand in the class's slot
       // (33..40) and the OFF hand in the race's shield slot, so a weapon FOUND
       // in 33..40 is main-hand by construction. There is nothing left to be
       // ambiguous about, and the narrow set was caution rather than a limit.
        CHECK(IsUnambiguousVisualWeaponSlot(33));  // sword
        CHECK(IsUnambiguousVisualWeaponSlot(34));  // dagger
        CHECK(IsUnambiguousVisualWeaponSlot(35));  // war axe
        CHECK(IsUnambiguousVisualWeaponSlot(36));  // mace
        CHECK(IsUnambiguousVisualWeaponSlot(37));  // greatsword / battleaxe
        CHECK(IsUnambiguousVisualWeaponSlot(38));  // bow
        CHECK(IsUnambiguousVisualWeaponSlot(39));  // staff
        CHECK(IsUnambiguousVisualWeaponSlot(40));  // crossbow

        // ⚠ 41 IS AMMO AND STAYS OUT. A quiver has no hand at all, AttachWeapon
        // rejects it, and REAugments reaches it through its own ammo entry
        // point. Letting it in here would hand ammo to the weapon attach.
        CHECK(!IsUnambiguousVisualWeaponSlot(41));
        // ⚠ AND SO DOES 32, WHICH IsMainHandWeaponBipedSlot DOES ACCEPT. That
        // predicate answers "is this slot a hand signal" and 32 is the body,
        // reached only because the range starts there. Nothing stages a weapon
        // in it, so rebuilding one out of it would be acting on a garment.
        CHECK(!IsUnambiguousVisualWeaponSlot(32));
        // Below the weapon range: the off-hand/editor slots, which the caller
        // reaches by a form-checked scan rather than by this predicate.
        CHECK(!IsUnambiguousVisualWeaponSlot(9));
        CHECK(!IsUnambiguousVisualWeaponSlot(0));
    }

    {  // The part-loader's biped slot is a hand discriminator.
        CHECK(IsOffHandWeaponBipedSlot(9));   // vanilla humanoid shield/off-hand slot
        CHECK(IsOffHandWeaponBipedSlot(31));  // any race-configured editor slot
        CHECK(!IsOffHandWeaponBipedSlot(32));
        CHECK(IsMainHandWeaponBipedSlot(32));
        CHECK(IsMainHandWeaponBipedSlot(40));
        CHECK(!IsMainHandWeaponBipedSlot(9));
        CHECK(!IsMainHandWeaponBipedSlot(41));  // quiver has no hand
        CHECK(IsWeaponOrQuiverBipedSlot(0));
        CHECK(IsWeaponOrQuiverBipedSlot(41));
        CHECK(!IsWeaponOrQuiverBipedSlot(42));
    }

    {  // kWeaponClassCount matches the enum's 11 styleable classes
        CHECK(kWeaponClassCount == 11);
        CHECK(kWeaponHandCount == 3);
    }

    {  // Only one-handed classes expose overrides; biped slot selects hand.
        CHECK(SupportsHandOverrides(WeaponClass::Sword));
        CHECK(SupportsHandOverrides(WeaponClass::Dagger));
        CHECK(SupportsHandOverrides(WeaponClass::WarAxe));
        CHECK(SupportsHandOverrides(WeaponClass::Mace));
        CHECK(SupportsHandOverrides(WeaponClass::Staff));
        CHECK(!SupportsHandOverrides(WeaponClass::Bow));
        CHECK(!SupportsHandOverrides(WeaponClass::Greatsword));
        CHECK(HandForBipedSlot(WeaponClass::Sword, 33) == WeaponHand::Right);
        CHECK(HandForBipedSlot(WeaponClass::Sword, 9) == WeaponHand::Left);
        CHECK(HandForBipedSlot(WeaponClass::Bow, 38) == WeaponHand::Both);
        CHECK(std::string_view(HandJsonName(WeaponHand::Right)) == "right");
        CHECK(HandFromJsonName("") == WeaponHand::Both);
        CHECK(HandFromJsonName("left") == WeaponHand::Left);
        CHECK(!HandFromJsonName("wrong"));
    }

    {  // A forced model reattach must restore either equipped hand to its
       // drawn presentation. Only the right uses virtual 0xB4. The left uses
       // its exact biped clone so same-form dual wield cannot steal child[0]
       // from a shared sheath node.
        CHECK(DrawnWeaponRepairFor(false, false) ==
              DrawnWeaponRepair::None);
        CHECK(DrawnWeaponRepairFor(false, true) ==
              DrawnWeaponRepair::None);
        CHECK(DrawnWeaponRepairFor(true, false) ==
              DrawnWeaponRepair::ReparentRight);
        CHECK(DrawnWeaponRepairFor(true, true) ==
              DrawnWeaponRepair::ReparentLeftClone);

        // Field evidence from the failed build: the rebuilt third-person
        // offhand clone was on WeaponSwordLeft (the hip), while first person
        // was already on SHIELD (the left-hand attachment node).
        CHECK(OffHandCloneNeedsHandReparent(true, "WeaponSwordLeft"));
        CHECK(OffHandCloneNeedsHandReparent(true, ""));
        CHECK(!OffHandCloneNeedsHandReparent(true, "SHIELD"));
        CHECK(!OffHandCloneNeedsHandReparent(false, "WeaponSwordLeft"));

        // Inventory/menu entry may transiently report a sheathed actor state
        // even while the existing 3D is still visibly hand-parented. Preserve
        // that visual truth across the equipment rebuild.
        CHECK(PreserveDrawnWeaponPlacement(false, "WEAPON"));
        CHECK(PreserveDrawnWeaponPlacement(false, "SHIELD"));  // drawn bow
        CHECK(!PreserveDrawnWeaponPlacement(false, "WeaponSword"));
        CHECK(!PreserveDrawnWeaponPlacement(false, "WeaponSwordLeft"));
        CHECK(PreserveDrawnWeaponPlacement(true, "WeaponSword"));
    }

    {  // The compact row X replaces the old side button: an explicit
       // per-hand value first falls back to Both; inherited/legacy rows clear
       // to the real weapon for that scope.
        CHECK(ClearWeaponHandActionFor(WeaponHand::Both, false) ==
              WeaponHandClearAction::UseRealWeapon);
        CHECK(ClearWeaponHandActionFor(WeaponHand::Right, false) ==
              WeaponHandClearAction::UseRealWeapon);
        CHECK(ClearWeaponHandActionFor(WeaponHand::Left, true) ==
              WeaponHandClearAction::InheritBoth);
    }

    {  // ClassFromAnimType: engine WEAPON_TYPE table, animType 1..9
        CHECK(ClassFromAnimType(1) == WeaponClass::Sword);
        CHECK(ClassFromAnimType(2) == WeaponClass::Dagger);
        CHECK(ClassFromAnimType(3) == WeaponClass::WarAxe);
        CHECK(ClassFromAnimType(4) == WeaponClass::Mace);
        CHECK(ClassFromAnimType(5) == WeaponClass::Greatsword);
        CHECK(ClassFromAnimType(6) == WeaponClass::BattleaxeWarhammer);
        CHECK(ClassFromAnimType(7) == WeaponClass::Bow);
        CHECK(ClassFromAnimType(8) == WeaponClass::Staff);
        CHECK(ClassFromAnimType(9) == WeaponClass::Crossbow);
    }

    {  // ClassFromAnimType: hand-to-hand (0) and out-of-range values are not styleable
        CHECK(ClassFromAnimType(0) == std::nullopt);
        CHECK(ClassFromAnimType(10) == std::nullopt);
        CHECK(ClassFromAnimType(255) == std::nullopt);
    }

    {  // ClassForAmmo splits arrows/bolts on the AMMO kNonBolt flag
        CHECK(ClassForAmmo(true) == WeaponClass::Bolts);
        CHECK(ClassForAmmo(false) == WeaponClass::Arrows);
    }

    {  // ClassJsonName / ClassFromJsonName round-trip all 11 classes;
       // unknown strings return nullopt
        constexpr WeaponClass kAll[kWeaponClassCount] = {
            WeaponClass::Sword,   WeaponClass::Dagger,     WeaponClass::WarAxe,
            WeaponClass::Mace,    WeaponClass::Greatsword, WeaponClass::BattleaxeWarhammer,
            WeaponClass::Bow,     WeaponClass::Crossbow,   WeaponClass::Staff,
            WeaponClass::Arrows,  WeaponClass::Bolts,
        };
        constexpr const char* kNames[kWeaponClassCount] = {
            "sword", "dagger", "waraxe", "mace", "greatsword", "battleaxe",
            "bow",   "crossbow", "staff", "arrows", "bolts",
        };
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            CHECK(std::string_view(ClassJsonName(kAll[i])) == kNames[i]);
            const auto roundTrip = ClassFromJsonName(kNames[i]);
            CHECK(roundTrip.has_value());
            CHECK(roundTrip.has_value() && *roundTrip == kAll[i]);
        }
        CHECK(ClassFromJsonName("shield") == std::nullopt);
        CHECK(ClassFromJsonName("") == std::nullopt);
        CHECK(ClassFromJsonName("Sword") == std::nullopt);  // case-sensitive, no fuzzy match
    }

    {  // BipedSlotForClass: sword..crossbow at 33..40, greatsword/battleaxe
       // share slot 37, arrows/bolts share the quiver slot 41
        CHECK(BipedSlotForClass(WeaponClass::Sword) == 33);
        CHECK(BipedSlotForClass(WeaponClass::Dagger) == 34);
        CHECK(BipedSlotForClass(WeaponClass::WarAxe) == 35);
        CHECK(BipedSlotForClass(WeaponClass::Mace) == 36);
        CHECK(BipedSlotForClass(WeaponClass::Greatsword) == 37);
        CHECK(BipedSlotForClass(WeaponClass::BattleaxeWarhammer) == 37);
        CHECK(BipedSlotForClass(WeaponClass::Bow) == 38);
        CHECK(BipedSlotForClass(WeaponClass::Crossbow) == 40);
        CHECK(BipedSlotForClass(WeaponClass::Staff) == 39);
        CHECK(BipedSlotForClass(WeaponClass::Arrows) == 41);
        CHECK(BipedSlotForClass(WeaponClass::Bolts) == 41);
    }

    {  // BipedSlotForClass: every class lands in the weapon/quiver range 33..41
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto slot = BipedSlotForClass(static_cast<WeaponClass>(i));
            CHECK(slot >= 33 && slot <= 41);
        }
    }

    {  // ClassLabelKey: one non-empty translation key per class, no collisions
        std::string_view seen[kWeaponClassCount];
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto c = static_cast<WeaponClass>(i);
            seen[i]      = ClassLabelKey(c);
            CHECK(!seen[i].empty());
        }
        CHECK(std::string_view(ClassLabelKey(WeaponClass::Sword)) == "$FR_WSlot_Sword");
        CHECK(std::string_view(ClassLabelKey(WeaponClass::Bolts)) == "$FR_WSlot_Bolts");
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            for (std::size_t j = i + 1; j < kWeaponClassCount; ++j) {
                CHECK(seen[i] != seen[j]);
            }
        }
    }

    {  // Which biped slots a hand owns, for finding that hand's parent node.
        // Main-hand weapons occupy the class slots 32 to 40; the off hand
        // occupies the shield slot below 32. Quiver belongs to neither hand:
        // it hangs on the back and is reached by naming it directly.
        CHECK(WeaponBipedSlotOwnedBy(32, WeaponHand::Right));
        CHECK(WeaponBipedSlotOwnedBy(40, WeaponHand::Right));
        CHECK(!WeaponBipedSlotOwnedBy(31, WeaponHand::Right));

        CHECK(WeaponBipedSlotOwnedBy(31, WeaponHand::Left));
        CHECK(!WeaponBipedSlotOwnedBy(32, WeaponHand::Left));
        CHECK(!WeaponBipedSlotOwnedBy(40, WeaponHand::Left));

        // Both accepts either, which is what an outfit written before per-hand
        // overrides existed means by it.
        CHECK(WeaponBipedSlotOwnedBy(31, WeaponHand::Both));
        CHECK(WeaponBipedSlotOwnedBy(32, WeaponHand::Both));

        // The quiver is nobody's hand, on any of the three.
        CHECK(!WeaponBipedSlotOwnedBy(41, WeaponHand::Right));
        CHECK(!WeaponBipedSlotOwnedBy(41, WeaponHand::Left));
        CHECK(!WeaponBipedSlotOwnedBy(41, WeaponHand::Both));

        // And it agrees with the two predicates it is built from, across the
        // whole weapon/quiver range, so the boundary lives in one place.
        for (std::uint32_t s = 0; s <= 41; ++s) {
            CHECK(WeaponBipedSlotOwnedBy(s, WeaponHand::Left) ==
                  IsOffHandWeaponBipedSlot(s));
            CHECK(WeaponBipedSlotOwnedBy(s, WeaponHand::Right) ==
                  IsMainHandWeaponBipedSlot(s));
        }
    }

    {  // Where to look for the node a weapon hangs on, and in what order.
        // Main hand first for Both: a class held in both hands frames the
        // primary. Field 2026-08-07 proved why the order and the domain both
        // matter: an off-hand-first scan over "any slot below 32" matched
        // ARMOUR slot 0, whose clone parents to the actor root, and every
        // weapon row framed the whole character.
        CHECK(WeaponNodeSearchFor(WeaponHand::Both).tryClassSlot);
        CHECK(WeaponNodeSearchFor(WeaponHand::Both).tryOffHand);

        // A named hand looks in exactly one place, or a dual-wield pair could
        // not be told apart at all.
        CHECK(WeaponNodeSearchFor(WeaponHand::Right).tryClassSlot);
        CHECK(!WeaponNodeSearchFor(WeaponHand::Right).tryOffHand);
        CHECK(!WeaponNodeSearchFor(WeaponHand::Left).tryClassSlot);
        CHECK(WeaponNodeSearchFor(WeaponHand::Left).tryOffHand);

        // The class slot is the main-hand placement, so it must never land in
        // the off-hand domain the scan below 32 owns. Ammo is the exception
        // the spec names: the quiver is neither hand.
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto c    = static_cast<WeaponClass>(i);
            const auto slot = BipedSlotForClass(c);
            CHECK(!IsOffHandWeaponBipedSlot(slot));
            const bool ammo = c == WeaponClass::Arrows || c == WeaponClass::Bolts;
            CHECK(ammo ? (slot == 41) : IsMainHandWeaponBipedSlot(slot));
        }
    }

    {  // WeaponPreview::ShouldShow - whether to hang a weapon the character
       // does NOT carry on their sheath node, so an empty class previews.
       //
       // ⚠ THE OCCUPANCY GUARD IS THE ONE THAT MATTERS AND IT IS WHY THIS IS
       // SAFE AT ALL. Refusing an occupied slot means we never fight real
       // gear, never trip AttachWeaponPart's change-detect on the way in, and
       // never have to remember a state to restore - the slot was empty, so
       // putting it back means making it empty.
        namespace WP = OS::WeaponPreview;
        using D      = WP::Decline;
        const auto sword = BipedSlotForClass(WeaponClass::Sword);   // 33
        const auto quiver = BipedSlotForClass(WeaponClass::Arrows);  // 41

        // The case the feature exists for: a sword class with an empty hand.
        CHECK(WP::ShouldShow(true, true, sword, false, false) == D::kNone);

        // ⚠ AMMO IS ALLOWED AND REACHES A DIFFERENT ENGINE FUNCTION. It
        // returned kIsAmmo until the field asked for quivers and bolts; the
        // decline was never a limit of the engine, only of what had been
        // wired. Actor::AttachWeapon still rejects an AMMO form on its first
        // instruction (kWeapon 0x29 vs kAmmo 0x2A), so the CALLER must route
        // slot 41 to AttachAmmoPart - handing it to the weapon door is a
        // silent no-op, which is exactly how "quivers do not appear" looked.
        CHECK(WP::ShouldShow(true, true, quiver, false, false) == D::kNone);

        // ⚠ THE ACTOR REALLY CARRIES ONE: hands off, every time. This is the
        // guard that keeps the "[not equipped]" tag honest - it is only ever
        // shown when there is genuinely nothing there.
        CHECK(WP::ShouldShow(true, true, sword, true, false) == D::kSlotOccupied);

        // No actor, or no biped built yet, is not a preview.
        CHECK(WP::ShouldShow(false, true, sword, false, false) == D::kNoActor);
        // Nothing picked is not a preview either.
        CHECK(WP::ShouldShow(true, false, sword, false, false) == D::kNoStyle);

        // ⚠ SLOT 0 IS A HEAD ARMOUR SLOT. BipedSlotForClass answers 0 for
        // kTotal or a byte forced into the enum, and attaching there would put
        // a weapon on the character's face. The range check is not paranoia.
        CHECK(WP::ShouldShow(true, true, 0u, false, false) == D::kBadClass);
        CHECK(WP::ShouldShow(true, true, BipedSlotForClass(WeaponClass::kTotal),
                             false, false) == D::kBadClass);
        // Nor anywhere else outside the weapon range.
        CHECK(WP::ShouldShow(true, true, 12u, false, false) == D::kBadClass);
        CHECK(WP::ShouldShow(true, true, 31u, false, false) == D::kBadClass);
        CHECK(WP::ShouldShow(true, true, 42u, false, false) == D::kBadClass);
        // ⚠ AND 32, WHICH IsMainHandWeaponBipedSlot DOES ACCEPT. It is the
        // BODY, in range only because the range starts there, and nothing
        // stages a weapon in it. Using the looser predicate here would hang a
        // sword off the torso.
        CHECK(WP::ShouldShow(true, true, 32u, false, false) == D::kBadClass);

        // Idempotence: the editor calls this every frame with the same pick,
        // so a weapon that is STILL ON THE BIPED is a no-op rather than a
        // detach-and-re-attach loop that rebuilds a node every present.
        CHECK(WP::ShouldShow(true, true, sword, false, true) == D::kAlreadyShowing);

        // ⚠⚠ AND THE FIELD BUG, PINNED. `a_stillUp` false with everything else
        // unchanged MUST come back kNone so the caller re-attaches. This is
        // the case a biped rebuild creates: the engine restages objects[] and
        // takes our node, while the caller's own record still says "I attached
        // that". Answering kAlreadyShowing here is what made a clicked weapon
        // invisible until the player clicked away and back (2026-08-11).
        CHECK(WP::ShouldShow(true, true, sword, false, false) == D::kNone);

        // ⚠ ORDER OF THE GUARDS IS ITSELF A DECISION. Occupancy is asked
        // BEFORE "already showing", so a real weapon appearing under a live
        // preview reports the occupancy and the caller stands down, rather
        // than reporting kAlreadyShowing and leaving ours parented over theirs.
        CHECK(WP::ShouldShow(true, true, sword, true, true) == D::kSlotOccupied);

        // The player equipping their own weapon under a live preview is the
        // reported "when i equip my weapon again it's fine" case: occupancy
        // wins, the caller tears its own preview down, and the real weapon is
        // never touched.
        CHECK(WP::ShouldShow(true, true, sword, true, false) == D::kSlotOccupied);

        {  // MayDisplaceWorn: the ONLY door through which this module touches
           // gear the player actually owns (field 2026-08-11 evening, "make
           // the preview replace an equipped weapon while browsing").
           //
           // ⚠ THE FORM COMPARISON AGAINST GetEquippedObject IS THE GUARD.
           // "The slot holds a weapon" is not enough: a follower's spare is
           // drawn from inventory by Immersive Equipment Displays onto the
           // skeleton, and engine visual-only weapons exist the actor does not
           // carry. Displacing either leaves us holding something we cannot
           // put back, because the restore works by re-attaching a form the
           // actor still has EQUIPPED.
            CHECK(WP::MayDisplaceWorn(true, true, sword));

            CHECK(!WP::MayDisplaceWorn(false, true, sword));   // not theirs
            CHECK(!WP::MayDisplaceWorn(true, false, sword));   // not a weapon
            CHECK(!WP::MayDisplaceWorn(false, false, sword));

            // The quiver IS displaceable - it is a thing the player carries.
            CHECK(WP::MayDisplaceWorn(true, true, quiver));
            // Never the body, the head, or the off-hand domain.
            CHECK(!WP::MayDisplaceWorn(true, true, 32u));
            CHECK(!WP::MayDisplaceWorn(true, true, 0u));
            CHECK(!WP::MayDisplaceWorn(true, true, 9u));

            // Every real class is displaceable, quiver included: the piece to
            // hide is whatever the player is carrying, and ammo counts.
            for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
                const auto c = static_cast<WeaponClass>(i);
                CHECK(WP::MayDisplaceWorn(true, true, BipedSlotForClass(c)));
            }
        }

        {  // The drawn reading that decides where the player's own weapon goes
           // back. ⚠ THE ACTOR STATE ALONE IS NOT ENOUGH: the editor is a
           // paused menu and the transition transiently reports SHEATHED while
           // the 3D is still hand-parented, so the node is a second source of
           // truth. Getting this backwards hands the weapon back on the hip
           // while the game thinks it is drawn.
            CHECK(PreserveDrawnWeaponPlacement(true, "WeaponSword"));   // state wins
            CHECK(PreserveDrawnWeaponPlacement(false, "WEAPON"));       // node wins
            CHECK(PreserveDrawnWeaponPlacement(false, "SHIELD"));       // drawn bow
            CHECK(!PreserveDrawnWeaponPlacement(false, "WeaponSword")); // truly sheathed
            CHECK(!PreserveDrawnWeaponPlacement(false, ""));            // nothing attached
        }

        // Every reason has a sentence; a decline that logs "?" is a decline
        // nobody can act on.
        for (const auto d : { D::kNone, D::kNoActor, D::kNoStyle, D::kBadClass,
                              D::kIsAmmo, D::kSlotOccupied, D::kAlreadyShowing }) {
            CHECK(std::string_view(WP::Why(d)) != "?");
            CHECK(!std::string_view(WP::Why(d)).empty());
        }

        // Every real WEAPON class is previewable on an empty slot, and the two
        // ammo classes are the only ones that are not. If a class were ever
        // remapped outside 33..40 this catches it here rather than as a sword
        // hanging off somebody's head.
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto c    = static_cast<WeaponClass>(i);
            CHECK(WP::ShouldShow(true, true, BipedSlotForClass(c), false,
                                 false) == D::kNone);
        }
    }

    if (g_failures == 0) {
        std::printf("all WeaponSlots tests passed\n");
    }
    return g_failures;
}
