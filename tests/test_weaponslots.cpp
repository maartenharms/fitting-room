// Pure-logic tests for the weapon-class mappings (weapon + quiver transmog,
// stage 1). No engine, no RE:: types.
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
    CHECK(IsUnambiguousVisualWeaponSlot(37));
    CHECK(IsUnambiguousVisualWeaponSlot(38));
    CHECK(IsUnambiguousVisualWeaponSlot(40));
    CHECK(!IsUnambiguousVisualWeaponSlot(33));
    CHECK(!IsUnambiguousVisualWeaponSlot(39));

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

    if (g_failures == 0) {
        std::printf("all WeaponSlots tests passed\n");
    }
    return g_failures;
}
