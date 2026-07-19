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

    {  // kWeaponClassCount matches the enum's 11 styleable classes
        CHECK(kWeaponClassCount == 11);
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
