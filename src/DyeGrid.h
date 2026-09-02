#pragma once

#include "Outfit.h"  // ShapeInfo, Outfit, DyeChannel, DyeSkipReason

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace OS {

    // Which group of head things a tile gathers. kNone for every tile that is
    // not a group: armour, weapons, and the eye tile.
    //
    // ⚠⚠ A TILE STOPPED BEING ONE KEY WHEN THIS ARRIVED, and the field is what
    // asked for it. Item 8 first shipped one tile per (slot, part), which is what
    // the data says a colour belongs to, and on a real head that was the hair
    // colour, the eyes, and then up to nine more tiles for one hair: "there are
    // so many groups now it's very poor UX" (user 2026-08-17). So the KEY stayed
    // per shape and the PRESENTATION became two groups.
    enum class HeadGroup : std::uint8_t {
        kNone = 0,
        // The hair: its own colour first, then every non-strand shape the hair
        // contributes. One tile for everything about the hair.
        kHair,
        // Horns, ears, and anything else authored at a head-part type the engine
        // never named. One tile however many slots the load order invented.
        kExtras,
    };

    // One swatch on a tile.
    //
    // ⚠⚠ THE STRIPE IS WHAT IS ADDRESSED, NOT THE TILE, since the groups above.
    // A group tile holds stripes belonging to DIFFERENT things: the hair tile's
    // first stripe is the hair colour, painted through facegen with no key at
    // all, and the ones after it are head-part shapes each keyed by its own slot
    // and part. So every stripe carries the identity a click has to write
    // through, and the editor builds its selection from the STRIPE.
    //
    // ⚠ target IS SET ON EVERY STRIPE OF EVERY TILE, armour and weapons
    // included, rather than only where it differs. One rule, read
    // unconditionally, is what stops a weapon stripe inheriting kArmour from a
    // default nobody remembered to set.
    struct DyeStripe {
        // What this stripe paints, which is NOT always its tile's target.
        DyeTarget     target{ DyeTarget::kArmour };
        // kHeadPart stripes only: the slot and the part whose shape this is.
        std::uint32_t headSlot{ 0 };
        StyleRefKey   headPart;
        // ⚠ THE CHANNEL IS SCOPED TO THE STRIPE'S OWN KEY, not to the tile. Two
        // stripes in one group tile can both be channel 0, because they are
        // channel 0 of two different parts, and they are still distinct
        // selections because their part refs differ. On an armour or weapon tile
        // nothing changed: the tile is the key and the channel is unique within
        // it.
        DyeChannelId  channel{ DyeChannelId::kPrimary };
        // Joined piece names. Shown in the TOOLTIP only. They come out of the
        // nif as Fabrics_Top and MetalOrnament_Coif, which means nothing to a
        // player, so they stopped occupying a row each.
        std::string   label;
        DyeChannel    colour;                          // .set false = no dye
        bool          dyeable{ false };
        DyeSkipReason reason{ DyeSkipReason::kNone };  // why not, when !dyeable
        bool          reflective{ false };
        // ⚠ THERE IS NO `orphaned` HERE ANY MORE. A stored colour whose piece is
        // gone used to get a stripe marked orphaned, drawn with a cross so it
        // could be cleared. It is simply not emitted now: a tile shows the
        // pieces of the garment being worn and nothing else. The colour is still
        // in the outfit and comes back with the garment; Reset piece purges it.
    };

    // One tile: a slot's, a weapon class-and-hand's, or hair's.
    //
    // ⚠ THE KEY IS DISCRIMINATED, NOT WIDENED. See DyeTarget in Outfit.h for
    // why: the off-hand weapon lives at biped 9, which is the shield's bit, so
    // one index space for both would collide on any character carrying a sword
    // in the left hand. Read `bit` only when target is kArmour and the weapon
    // pair only when it is kWeapon.
    //
    // ⚠ A kHair TILE READS NO KEY FIELD AT ALL, so `bit` and the weapon pair are
    // both meaningless on it and stay at their defaults. bit's default 0 is a
    // real addressable slot rather than a spare value, which is why reading it
    // is worse than useless: it would put hair on that slot's garment. There is
    // exactly ONE hair tile, so matching the target already identifies it.
    //
    // A kHair tile also carries EXACTLY ONE stripe, because Outfit::hairTint is
    // a single RGB rather than up to kDyeChannelCount channels, and it is ALWAYS
    // EMITTED, FIRST, whatever is worn - the control it replaced was always
    // available too, including for an "(away)" follower with no loaded actor.
    // Anything walking this vector must therefore not assume the first tile is
    // armour, and must not treat an all-hair grid as "the character has
    // something dyeable on".
    //
    // ⚠ ReflectiveMask is MEANINGLESS on one. Hair has no worn shape and so no
    // reflective shape, the single stripe leaves `reflective` false, and the
    // mask comes back all false. That is the correct answer rather than a
    // missing one: bDyeReflective governs a material swap hair never takes.
    struct DyeTile {
        DyeTarget              target{ DyeTarget::kArmour };
        std::uint32_t          bit{ 0 };                       // kArmour only
        WeaponClass            weaponClass{ WeaponClass::Sword };  // kWeapon only
        WeaponHand             weaponHand{ WeaponHand::Both };     // kWeapon only
        // Which group of head things this tile gathers, kNone on every tile that
        // is not one. ⚠ THE TILE'S ONLY KEY WHEN IT IS A GROUP: the colours
        // belong to the stripes, and the group is what the ring, the widget id
        // and the title are keyed on.
        HeadGroup              headGroup{ HeadGroup::kNone };
        // kWeapon only: this class has MORE THAN ONE tile on screen right now,
        // which happens exactly when the character is holding one in each hand.
        //
        // ⚠ IT DECIDES WHERE AN EDIT IS WRITTEN, so it lives here rather than
        // being counted in the editor. Colouring the only sword you are carrying
        // should colour "your sword", which is the Both value, so picking up a
        // second one later matches. Colouring one of two swords must write only
        // that hand, or the click would silently recolour the other one too. One
        // rule, and the grid is the only place that can see the whole set.
        bool                   handSplit{ false };
        std::size_t            shapeCount{ 0 };  // EVERY shape, body included
        std::vector<DyeStripe> stripes;
    };

    // Pure: no engine types and no ImGui, so this compiles into a test
    // executable and the include rules are provable without a rendering context.
    namespace DyeGrid {

        // Every slot worth a tile, in slot bit order. A slot qualifies when it
        // has a garment shape on a live channel, and on nothing else: a stored
        // colour with no piece to sit on is not drawn and does not make a tile.
        // ⚠ a_advancedEye IS A PARAMETER AND NOT A Settings READ, because this
        // file compiles into a pure test executable and must name no singleton.
        // It gates the eye tile's SECOND COLOUR stripe, which is experimental
        // and off by default: with it false the eye is the iris and the white,
        // which is the whole feature for an ordinary install.
        //
        // ⚠⚠ IT MUST BE THE SAME ANSWER THE PAINTER USES. PaintEyeTint drops a
        // second colour under the same setting, so a grid built with one value
        // and a paint run under the other would offer a control that does
        // nothing, which is the exact shape of the bug that ate the sclera.
        //
        // ⚠ DEFAULTED SO EVERY EXISTING CALLER AND TEST KEEPS ITS MEANING, and
        // false is the shipping answer rather than a convenience: a default of
        // true here would turn the experiment on for anyone who forgot to pass
        // it.
        [[nodiscard]] std::vector<DyeTile> Build(const std::vector<ShapeInfo>& a_shapes,
                                                 const Outfit&                 a_staged,
                                                 bool a_advancedEye = false);

        // Which of a tile's channels carry a reflective shape.
        //
        // Feeds MapColoursToSlotSkipping, which is how `bDyeReflective` governs
        // bulk operations now that it no longer gates the painter: a paste or a
        // scheme leaves the metal alone while the rail is on, and an explicit
        // click on the stripe still dyes it.
        //
        // ⚠ KEYED BY CHANNEL ID, NEVER BY STRIPE POSITION. Stripes are only the
        // LIVE channels, so a garment whose first shape is the player's body, or
        // a weapon whose first shape is its blood overlay, emits no stripe for
        // channel 0 and its first stripe is channel 1. Walking stripes
        // positionally would put the metal's flag on the wrong channel, and the
        // rail would then skip a piece the player can see while dulling the one
        // they cannot.
        [[nodiscard]] inline std::array<bool, kDyeChannelCount> ReflectiveMask(
            const DyeTile& a_tile) {
            std::array<bool, kDyeChannelCount> mask{};
            for (const auto& st : a_tile.stripes) {
                const auto c = static_cast<std::size_t>(st.channel);
                if (c < mask.size()) {
                    mask[c] = st.reflective;
                }
            }
            return mask;
        }

    }  // namespace DyeGrid

}  // namespace OS
