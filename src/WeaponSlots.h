#pragma once

// A per-weapon-class style slot (weapon + quiver transmog, stage 1).
// Parallel to, and independent from, the 32-bit armor SlotMask - weapons
// and ammo normally live in BipedAnim slots 32..41. Off-hand weapons are the
// exception: the engine stages them in the actor race's shield/editor slot.
// This is an unrelated index space from the armor EDITOR bits in SlotMask.h.
// Pure ints/strings, header-only,
// NO RE:: / CommonLib includes - the WEAP animation type arrives at the
// hook call site as a plain std::uint8_t, and this header must be
// includable standalone by the pure-logic unit test suite.
//
// Provenance: WEAPON_TYPE (animType) -> BipedAnim slot table, RE dump
// wpn_batch1 (SE 1.5.97): {0->32, 1->33, 2->34, 3->35, 4->36, 5->37, 6->37,
// 7->38, 8->39, 9->40}. animType 0 (hand-to-hand) has no styleable class -
// ClassFromAnimType never returns one for it. The quiver (AMMO, split
// arrow/bolt by the kNonBolt flag rather than an animType) is a fixed
// slot 41.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace OS {

    // Append-only contract: a new class goes BEFORE kTotal. kTotal is a
    // sentinel, never a real class - it is never passed to any function
    // below, only used to derive kWeaponClassCount, so the count can never
    // silently drift from the enumerator list (the CommonLib kTotal idiom).
    enum class WeaponClass : std::uint8_t {
        Sword,
        Dagger,
        WarAxe,
        Mace,
        Greatsword,
        BattleaxeWarhammer,
        Bow,
        Crossbow,
        Staff,
        Arrows,
        Bolts,
        kTotal,
    };

    inline constexpr std::size_t kWeaponClassCount = static_cast<std::size_t>(WeaponClass::kTotal);

    // kBoth is the legacy per-class value and remains the fallback for every
    // outfit written before per-hand overrides existed. Right/Left are
    // optional overrides for one-handed classes only.
    enum class WeaponHand : std::uint8_t {
        Both = 0,
        Right = 1,
        Left = 2,
        kTotal,
    };

    inline constexpr std::size_t kWeaponHandCount =
        static_cast<std::size_t>(WeaponHand::kTotal);

    // Keeps the per-hand editor affordance within the narrow slots panel:
    // the row's existing X owns both meanings instead of adding a side
    // button beside the hand stepper.
    enum class WeaponHandClearAction : std::uint8_t {
        UseRealWeapon,
        InheritBoth,
    };

    [[nodiscard]] constexpr WeaponHandClearAction ClearWeaponHandActionFor(
        WeaponHand a_hand, bool a_hasExplicitOverride) {
        return a_hand != WeaponHand::Both && a_hasExplicitOverride
                   ? WeaponHandClearAction::InheritBoth
                   : WeaponHandClearAction::UseRealWeapon;
    }

    [[nodiscard]] constexpr bool SupportsHandOverrides(WeaponClass a_class) {
        switch (a_class) {
            case WeaponClass::Sword:
            case WeaponClass::Dagger:
            case WeaponClass::WarAxe:
            case WeaponClass::Mace:
            case WeaponClass::Staff:
                return true;
            case WeaponClass::Greatsword:
            case WeaponClass::BattleaxeWarhammer:
            case WeaponClass::Bow:
            case WeaponClass::Crossbow:
            case WeaponClass::Arrows:
            case WeaponClass::Bolts:
            case WeaponClass::kTotal:
                return false;
        }
        return false;
    }

    [[nodiscard]] constexpr const char* HandJsonName(WeaponHand a_hand) {
        switch (a_hand) {
            case WeaponHand::Both: return "both";
            case WeaponHand::Right: return "right";
            case WeaponHand::Left: return "left";
            case WeaponHand::kTotal: break;
        }
        return "";
    }

    [[nodiscard]] constexpr std::optional<WeaponHand> HandFromJsonName(
        std::string_view a_name) {
        if (a_name.empty() || a_name == "both") {
            return WeaponHand::Both;
        }
        if (a_name == "right") {
            return WeaponHand::Right;
        }
        if (a_name == "left") {
            return WeaponHand::Left;
        }
        return std::nullopt;
    }

    // Engine WEAPON_TYPE (animType) -> styleable class. animType 0
    // (hand-to-hand) and anything >= 10 are not a class this mod styles.
    [[nodiscard]] constexpr std::optional<WeaponClass> ClassFromAnimType(std::uint8_t a_animType) {
        switch (a_animType) {
            case 1: return WeaponClass::Sword;
            case 2: return WeaponClass::Dagger;
            case 3: return WeaponClass::WarAxe;
            case 4: return WeaponClass::Mace;
            case 5: return WeaponClass::Greatsword;
            case 6: return WeaponClass::BattleaxeWarhammer;
            case 7: return WeaponClass::Bow;
            case 8: return WeaponClass::Staff;
            case 9: return WeaponClass::Crossbow;
            default: return std::nullopt;
        }
    }

    // AMMO has no animType; it splits on TESAmmo's kNonBolt flag instead.
    [[nodiscard]] constexpr WeaponClass ClassForAmmo(bool a_isBolt) {
        return a_isBolt ? WeaponClass::Bolts : WeaponClass::Arrows;
    }

    // The BipedAnim slot (32..41) that class's 3D materializes into.
    // Greatsword and BattleaxeWarhammer share slot 37 (animType 5 and 6 are
    // both two-handed weapons in the same biped slot); Arrows and Bolts
    // share slot 41 (one quiver slot for both ammo kinds).
    [[nodiscard]] constexpr std::uint32_t BipedSlotForClass(WeaponClass a_class) {
        // No default: an appended class that misses a mapping here is a
        // compiler warning (C4062/-Wswitch), not a silent 0.
        switch (a_class) {
            case WeaponClass::Sword: return 33;
            case WeaponClass::Dagger: return 34;
            case WeaponClass::WarAxe: return 35;
            case WeaponClass::Mace: return 36;
            case WeaponClass::Greatsword: return 37;
            case WeaponClass::BattleaxeWarhammer: return 37;
            case WeaponClass::Bow: return 38;
            case WeaponClass::Staff: return 39;
            case WeaponClass::Crossbow: return 40;
            case WeaponClass::Arrows: return 41;
            case WeaponClass::Bolts: return 41;
            case WeaponClass::kTotal: break;  // sentinel, never a real class
        }
        return 0;  // kTotal (or an out-of-range byte forced into the enum)
    }

    // Actor::AttachWeapon stages the main-hand weapon in its class slot
    // (33..40), but stages the off-hand weapon in the actor race's configured
    // shield/editor slot (normally 9). The part-loader hook receives that
    // resolved biped slot, so it is already a reliable hand discriminator even
    // when both hands equip the exact same WEAP form.
    //
    // These predicates intentionally classify slots, not forms. Callers must
    // still verify that objects[slot].item is a WEAP before treating an editor
    // slot as an off-hand weapon; shields and torches can occupy the same area.
    [[nodiscard]] constexpr bool IsOffHandWeaponBipedSlot(
        std::uint32_t a_bipedSlot) {
        return a_bipedSlot < 32;
    }

    [[nodiscard]] constexpr bool IsMainHandWeaponBipedSlot(
        std::uint32_t a_bipedSlot) {
        return a_bipedSlot >= 32 && a_bipedSlot <= 40;
    }

    // The loader's biped slot is a reliable hand signal, including when the
    // exact same WEAP form occupies both hands.
    [[nodiscard]] constexpr WeaponHand HandForBipedSlot(
        WeaponClass a_class, std::uint32_t a_bipedSlot) {
        if (!SupportsHandOverrides(a_class)) {
            return WeaponHand::Both;
        }
        return IsOffHandWeaponBipedSlot(a_bipedSlot)
                   ? WeaponHand::Left
                   : WeaponHand::Right;
    }

    // Actor::AttachWeapon has asymmetric hand behavior. A drawn right-hand
    // replacement can use virtual 0xB4 to move sheath -> hand. The left must
    // instead move its exact biped clone: field evidence shows a rebuilt
    // third-person offhand parked on WeaponSwordLeft, and virtual 0xB4 can
    // select the wrong children[0] when both hands share one WEAP form.
    enum class DrawnWeaponRepair : std::uint8_t {
        None,
        ReparentRight,
        ReparentLeftClone,
    };

    [[nodiscard]] constexpr DrawnWeaponRepair DrawnWeaponRepairFor(
        bool a_drawn, bool a_leftHand) {
        if (!a_drawn) {
            return DrawnWeaponRepair::None;
        }
        return a_leftHand ? DrawnWeaponRepair::ReparentLeftClone
                          : DrawnWeaponRepair::ReparentRight;
    }

    // SHIELD is Skyrim's left-hand attachment node. A clone already there
    // needs only to be revealed; an attached clone anywhere else (including
    // WeaponSwordLeft, the hip/sheath) must be moved when preserving a drawn
    // offhand across a forced equipment rebuild.
    [[nodiscard]] constexpr bool OffHandCloneNeedsHandReparent(
        bool a_preserveDrawn, std::string_view a_parentNodeName) {
        return a_preserveDrawn && a_parentNodeName != "SHIELD";
    }

    // The inventory/menu transition can transiently report a sheathed actor
    // state while the displayed 3D is still drawn. These are the engine's
    // observed drawn attachment parents: ordinary weapons use WEAPON; a drawn
    // bow is the special case on SHIELD. Preserve either source of truth.
    [[nodiscard]] constexpr bool PreserveDrawnWeaponPlacement(
        bool a_actorStateDrawn, std::string_view a_parentNodeName) {
        return a_actorStateDrawn || a_parentNodeName == "WEAPON" ||
               a_parentNodeName == "SHIELD";
    }

    [[nodiscard]] constexpr bool IsWeaponOrQuiverBipedSlot(
        std::uint32_t a_bipedSlot) {
        return a_bipedSlot <= 41;
    }

    // A visual-only engine biped object can be rebuilt without guessing a
    // hand only when its slot is intrinsically back/two-handed. This bounded
    // set covers template/default bows (Jenassa's edge case), crossbows and
    // two-handed weapons while refusing ambiguous one-hand/staff objects.
    [[nodiscard]] constexpr bool IsUnambiguousVisualWeaponSlot(
        std::uint32_t a_bipedSlot) {
        return a_bipedSlot == 37 || a_bipedSlot == 38 || a_bipedSlot == 40;
    }

    // Frozen public vocabulary for outfits.json / presets ("weapons" array,
    // see PRESETS.md) - rename never, append only.
    [[nodiscard]] constexpr const char* ClassJsonName(WeaponClass a_class) {
        // No default: an appended class that misses a name here is a compiler
        // warning (C4062/-Wswitch), not a silent "".
        switch (a_class) {
            case WeaponClass::Sword: return "sword";
            case WeaponClass::Dagger: return "dagger";
            case WeaponClass::WarAxe: return "waraxe";
            case WeaponClass::Mace: return "mace";
            case WeaponClass::Greatsword: return "greatsword";
            case WeaponClass::BattleaxeWarhammer: return "battleaxe";
            case WeaponClass::Bow: return "bow";
            case WeaponClass::Crossbow: return "crossbow";
            case WeaponClass::Staff: return "staff";
            case WeaponClass::Arrows: return "arrows";
            case WeaponClass::Bolts: return "bolts";
            case WeaponClass::kTotal: break;  // sentinel, never a real class
        }
        return "";  // kTotal (or an out-of-range byte forced into the enum)
    }

    // Case-sensitive reverse lookup of ClassJsonName; unknown strings (a
    // stale/foreign "weapons" entry) come back as nullopt so callers can
    // ignore the entry rather than guess.
    [[nodiscard]] constexpr std::optional<WeaponClass> ClassFromJsonName(std::string_view a_name) {
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto c = static_cast<WeaponClass>(i);
            if (a_name == ClassJsonName(c)) {
                return c;
            }
        }
        return std::nullopt;
    }

    // Editor slot-row translation key (FLICK), e.g. "$FR_WSlot_Sword".
    [[nodiscard]] constexpr const char* ClassLabelKey(WeaponClass a_class) {
        // No default: an appended class that misses a key here is a compiler
        // warning (C4062/-Wswitch), not a silent "".
        switch (a_class) {
            case WeaponClass::Sword: return "$FR_WSlot_Sword";
            case WeaponClass::Dagger: return "$FR_WSlot_Dagger";
            case WeaponClass::WarAxe: return "$FR_WSlot_WarAxe";
            case WeaponClass::Mace: return "$FR_WSlot_Mace";
            case WeaponClass::Greatsword: return "$FR_WSlot_Greatsword";
            case WeaponClass::BattleaxeWarhammer: return "$FR_WSlot_BattleaxeWarhammer";
            case WeaponClass::Bow: return "$FR_WSlot_Bow";
            case WeaponClass::Crossbow: return "$FR_WSlot_Crossbow";
            case WeaponClass::Staff: return "$FR_WSlot_Staff";
            case WeaponClass::Arrows: return "$FR_WSlot_Arrows";
            case WeaponClass::Bolts: return "$FR_WSlot_Bolts";
            case WeaponClass::kTotal: break;  // sentinel, never a real class
        }
        return "";  // kTotal (or an out-of-range byte forced into the enum)
    }

}  // namespace OS
