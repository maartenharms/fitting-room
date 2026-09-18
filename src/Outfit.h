#pragma once

#include "BodyPreset.h"  // PushUpMode, which is a body shape and not an outfit one
#include "OutfitTabs.h"
#include "SlotMask.h"
#include "WeaponSlots.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OS {

    // Saved outfits per player/follower library. 10 until 2026-08-29.
    //
    // ⚠⚠ RAISING THIS IS A ONE-WAY DOOR FOR A SAVE, and the door is in builds
    // that are already out. PersistenceCodec's 'LIBR' decode refuses when
    // `count > kMaxOutfitCount`, and refusing a 'LIBR' record throws away the
    // WHOLE library rather than the outfits past the cap. So the moment a
    // player creates an 11th outfit, every build shipped before this one reads
    // their save and finds no outfits at all. Rolling back is something players
    // actually do: one did it the night before this changed.
    //
    // Nothing here can fix that, because the refusal lives in binaries already
    // downloaded. It is only safe to raise in the direction of travel, and each
    // raise re-arms the same trap against every build older than it.
    //
    // ⚠ THE STORAGE ITSELF DOES NOT CARE. outfits_ is a vector and the tab
    // strip already scrolls, so this is a policy number rather than a
    // structural one.
    inline constexpr std::size_t kMaxOutfits = 20;
    inline constexpr std::uint32_t kBitCount = 32;
    static_assert(kBitCount <= 32, "masks are uint32_t");

    // A style piece, stored load-order-independently. Resolved to a
    // TESObjectARMO* only inside StyleRef.cpp (engine code).
    struct StyleRefKey {
        std::string   modName;
        std::uint32_t localFormID{ 0 };

        [[nodiscard]] bool Empty() const { return modName.empty() && localFormID == 0; }
        friend bool operator==(const StyleRefKey&, const StyleRefKey&) = default;
    };

    struct SlotEntry {
        enum class Kind : std::uint8_t { kPassthrough = 0, kStyle = 1, kHide = 2 };
        Kind        kind{ Kind::kPassthrough };
        StyleRefKey style;
    };

    // One outfit's choice for one head-part slot the load order invented.
    //
    // The slot number travels WITH the reference rather than being implied by
    // position, because a load order that drops the horn mod must leave the ear
    // entry still meaning ears, and an entry naming a slot nothing provides any
    // more has to stay recognisable as that rather than silently becoming its
    // neighbour's.
    struct CustomHeadPartRef {
        std::uint32_t slot{ 0 };
        StyleRefKey   key;

        friend bool operator==(const CustomHeadPartRef&,
                               const CustomHeadPartRef&) = default;
    };

    // A corrupt or truncated record can claim any count, so the decoder bounds
    // it before trusting it, exactly as kMaxNpcAssignments does. Generous
    // against any real load order: the dev rig's five is the most anyone has
    // been seen to have, and a hundred slots would mean a hundred mods each
    // claiming their own head-part type.
    inline constexpr std::uint32_t kMaxCustomHeadParts = 128;

    // Per-outfit ORefit override (OBody NG). kDefault leaves OBody's own global
    // setting alone - the only value that behaves identically to a build with
    // no OBody integration at all, which is why it is the zero.
    enum class ORefitMode : std::uint8_t { kDefault = 0, kForceOn = 1, kForceOff = 2 };

    // Per-outfit PUSH-UP. PushUpMode itself lives in BodyPreset.h beside
    // BodyFamily, because which morphs it means depends on the body and not on
    // the outfit: BodyMorphPlan turns the pair into named morphs and this field
    // only says how much the garment asks for.
    //
    // ⚠ kNone IS THE ZERO for the same reason kDefault is ORefit's: it is the
    // only value that behaves exactly like a build without the feature.

    // Per-outfit hair COLOUR. Separate from HairMode, which is visibility: that
    // one rides the worn mask engine 24220 reads, this one goes through the
    // actor base and facegen. They share a UI section and nothing else.
    //
    // The RGB the USER PICKED is what is stored, never the colour form it
    // resolves to. A form ID would be a load-order dependent reference frozen
    // into an outfit file, so removing the mod that provided it would leave the
    // outfit carrying a dangling form. RGB re-resolved against the live pool on
    // every load is always valid. Same rule coverage follows: derive it, do not
    // persist it.
    //
    // ⚠ Plain POD, not RE::Color. This header names no engine types because it
    // compiles into the pure-logic test executables.
    struct HairTint {
        bool         set{ false };  // false = leave the character's own colour alone
        std::uint8_t r{ 0 };
        std::uint8_t g{ 0 };
        std::uint8_t b{ 0 };

        friend bool operator==(const HairTint&, const HairTint&) = default;
    };

    // ESO's iridescent dyes are the only ones that touch the material: they add
    // a metallic sheen AND shine with a second colour distinct from the base.
    // A single gloss byte cannot express that, so a finish is a colour plus a
    // sharpness.
    //
    // gloss follows kDyeNeutral: 128 is UNCHANGED, above tightens the
    // highlight toward metal, below broadens and dulls it. It is a nudge on
    // what the mesh already carries, never a replacement. The census measured
    // real meshes from 30 to 267, so an absolute write would collapse a
    // deliberately glossy ivory cuirass and a deliberately dull draugr piece
    // onto one number and destroy the mesh author's intent.
    struct DyeMaterial {
        // ⚠ TWO PRESENCE BITS, NOT ONE, and the reason is concrete. The feature
        // has two controls, so a player can set a gloss without ever choosing a
        // sheen. Under one bit that write produced {set, 0,0,0, gloss}, the
        // engine wrote specularColor black, and the shape's highlight went out.
        // The same bit also let any override freeze the sheen forever, so a
        // later swatch click could not change it.
        bool         sheenSet{ false };
        std::uint8_t sheenR{ 0 };
        std::uint8_t sheenG{ 0 };
        std::uint8_t sheenB{ 0 };
        bool         glossSet{ false };
        std::uint8_t gloss{ 128 };  // kDyeNeutral: 128 is unchanged

        [[nodiscard]] constexpr bool Any() const { return sheenSet || glossSet; }
        friend bool operator==(const DyeMaterial&, const DyeMaterial&) = default;
    };

    // One ESO-style dye channel. Plain POD for the same reason HairTint is:
    // this header compiles into the pure-logic test executables and must name
    // no engine types.
    struct DyeChannel {
        bool         set{ false };  // false = leave this channel alone entirely
        std::uint8_t r{ 0 };
        std::uint8_t g{ 0 };
        std::uint8_t b{ 0 };
        // How much of this colour lands: 255 as chosen, 0 the neutral, and
        // DyeStrength.h owns the blend. Default 255 so a channel decoded from a
        // record written before v13 looks exactly as it always did.
        //
        // ⚠ IT SITS BEFORE THE MATERIAL BLOCKS RATHER THAN AFTER THEM, and that
        // was checked rather than assumed: all 125 aggregate initialisations of
        // this struct pass exactly four elements, so every one of them still
        // means what it did and picks the default here.
        std::uint8_t strength{ 255 };
        // The second stop, and which ramp to run it through.
        //
        // ⚠ AFTER strength AND BEFORE THE MATERIAL BLOCKS, for the reason
        // strength itself records. 124 positional initialisations of this
        // struct pass exactly four elements, so every member from the fifth on
        // is reached only by a designated initialiser or a later assignment.
        // Putting these fifth would make `DyeChannel{ set, r, g, b, strength }`
        // write a mode, and that compiles. Re-audited 2026-08-08 before these
        // were added: 139 sites, 15 of them empty, 124 positional, none passing
        // five or more.
        //
        // ⚠ EVERY DEFAULT IS TODAY'S BEHAVIOUR. mode 0 is flat and secondSet is
        // false, so a channel decoded from a v13 record paints exactly as it
        // always did rather than merely being readable.
        //
        // ⚠ AND SameDyeColour BELOW DELIBERATELY DOES NOT SEE THEM. It names the
        // four fields it compares, so these are excluded by construction. That
        // is the safe direction: the channelsDyed deed it feeds is sticky and
        // over-counting cannot be undone, so re-dyeing a channel from flat red
        // to pearl red not counting as a new channel is the error this header
        // already says to prefer.
        std::uint8_t mode{ 0 };  // OS::DyeRamp::Mode, stored as a byte
        bool         secondSet{ false };
        std::uint8_t r2{ 0 };
        std::uint8_t g2{ 0 };
        std::uint8_t b2{ 0 };
        // Metallic flake: how strongly the reflection breaks into glinting
        // speckles. 0 is none, which is every dye that existed before it.
        // Carried on the channel for the same reason the stops are: there is no
        // dye id downstream of a swatch. Same placement rule as the block
        // above: after the four the positional initialisers reach, before the
        // material blocks.
        std::uint8_t flake{ 0 };
        // How this colour is mixed with the cloth under it, as an
        // OS::DyeBlend::Choice stored as a byte. 0 defers to the install's
        // [Dye] sDyeBlend, which is what every channel that ever existed
        // already does, so this default changes nothing anywhere.
        //
        // Carried on the channel for the reason the stops and the flake are:
        // there is no dye id downstream of a swatch. Same placement rule as the
        // blocks above, after the four the positional initialisers reach and
        // before the material blocks.
        std::uint8_t blend{ 0 };
        // What the DYE said, copied at swatch click. There is no dye id
        // downstream of a swatch, so this cannot be looked up at paint time and
        // has to travel with the colour.
        DyeMaterial  palette{};
        // What the PLAYER set. Honoured only while bDyeFinishOverride is on,
        // and never cleared when it is off.
        DyeMaterial  player{};
        // Where metal starts on THIS piece, for the envmask modes (2026-09-04):
        // 0 puts the cut at the mask's dark class mean (everything above the
        // cloth is metal), 255 at the bright class mean, 128 halfway, which is
        // where Otsu landed on every mask measured, so the default is the
        // picture every build before this byte drew. DyeRamp::CutFor turns it
        // into a threshold in the map's own units.
        //
        // ⚠ THE PIECE'S, NOT THE DYE'S. The Imperial Dragon cloak wants a fifth
        // of the class gap and vanilla iron half (STATUS 2026-09-04 07:50), so
        // no dye can know it: ApplyPaletteDye keeps the staged byte unless the
        // dye declares one, and SameDyeColour cannot see it, like strength.
        //
        // ⚠ LAST, AFTER BOTH MATERIAL BLOCKS, for the reason strength and mode
        // record above: 124 positional initialisations pass four elements and
        // must keep meaning what they mean. Codec v25 appends it after the
        // blend, so a v24 channel decodes to 128 and paints as it did.
        std::uint8_t cut{ 128 };
        friend bool operator==(const DyeChannel&, const DyeChannel&) = default;
    };

    // Whether two channels differ in the thing a PLAYER would call the colour.
    //
    // ⚠ ChangedDyeChannelCount must use this rather than operator==. That
    // function feeds the channelsDyed deed, which unlocks 36 colours and is
    // sticky, and this header already names over-counting as the direction that
    // cannot be undone. When the struct grew two material blocks the defaulted
    // comparison silently moved the count into that direction, so a player
    // could nudge a slider back and forth and farm the deed for free.
    //
    // ⚠ AND STRENGTH IS EXCLUDED FOR THE SAME REASON, DELIBERATELY. It is a
    // slider, so it is the exact shape of the farm this function already exists
    // to prevent: nudge it back and forth and every frame is a "change". A
    // player weakening a colour has not dyed a new channel. The body below needs
    // no edit for that, because it names the four fields it compares rather than
    // excluding the ones it does not, which is the property that makes this
    // correct as the struct grows.
    [[nodiscard]] inline constexpr bool SameDyeColour(const DyeChannel& a, const DyeChannel& b) {
        return a.set == b.set && a.r == b.r && a.g == b.g && a.b == b.b;
    }

    // What a channel becomes when a PALETTE dye is clicked onto it.
    //
    // ⚠ THIS IS A MERGE, NOT AN ASSIGNMENT, AND IT IS HERE BECAUSE THE OTHER TWO
    // SHAPES BOTH SHIPPED BROKEN. Copying r, g and b alone is what the swatch
    // click did until 2026-08-08: a pearlescent dye reached the paint walk with
    // mode 0 and painted flat, and the finish the dye declared never arrived at
    // all, which is the whole reason `palette` was empty on every channel that
    // ever shipped. Assigning the dye's channel wholesale is the opposite fault:
    // it would reset the player's strength slider and wipe the `player` finish
    // override on every click.
    //
    // ⚠ WHICH SIDE EACH FIELD COMES FROM IS THE STRUCT'S OWN ANSWER, not a
    // judgement made here. `palette` says "What the DYE said, copied at swatch
    // click" and `player` says "What the PLAYER set... never cleared". strength
    // is a slider, so it is the player's. Everything describing the colour,
    // including the ramp that makes it special, is the dye's.
    //
    // ⚠ AND THE DYE'S FIELDS OVERWRITE RATHER THAN MERGE, so clicking a plain
    // colour after a pearl CLEARS the ramp and the declared finish. A dye that is
    // no longer applied must not leave its second stop behind for the next
    // colour to be ramped against.
    //
    // ⚠ PURE, AND IN THIS HEADER, FOR THE REASON TutorialPlan.h AND EditorGate.h
    // ARE: no test compiles EditorUI.cpp, so a commit path left in the click
    // handler reaches the field unexercised, which is exactly how it shipped
    // dropping four of its five fields.
    [[nodiscard]] inline constexpr DyeChannel ApplyPaletteDye(const DyeChannel& a_staged,
                                                             const DyeChannel& a_dye) {
        DyeChannel out{};
        // The player's, preserved.
        out.strength = a_staged.strength;
        out.player   = a_staged.player;
        // The cut is the piece's too, unless the dye says where its metal
        // starts: 128 is "nothing said" (and every shipped dye), so the byte a
        // player tuned on a piece survives every swatch they compare.
        out.cut = a_dye.cut != 128 ? a_dye.cut : a_staged.cut;
        // The dye's, in full.
        out.set       = true;
        out.r         = a_dye.r;
        out.g         = a_dye.g;
        out.b         = a_dye.b;
        out.mode      = a_dye.mode;
        out.secondSet = a_dye.secondSet;
        out.r2        = a_dye.r2;
        out.g2        = a_dye.g2;
        out.b2        = a_dye.b2;
        out.flake     = a_dye.flake;
        out.blend     = a_dye.blend;
        out.palette   = a_dye.palette;
        return out;
    }

    // Is this channel still showing exactly what a_dye would put on it?
    //
    // ⚠⚠ THE ANSWER IS ApplyPaletteDye ITSELF, RUN FORWARDS, AND THAT IS THE
    // WHOLE POINT. Naming the fields again here would be a second reader of one
    // answer, and the two would drift the first time a field is added to the
    // merge: the click would carry it and this would not, so a channel painted
    // by a swatch would stop recognising its own dye for a reason nothing logs.
    // Asking "would applying this dye change anything?" cannot drift, because
    // it is the same code the click runs. A field added above is compared here
    // the moment it is copied there, with no edit to this function.
    //
    // ⚠ STRENGTH AND `player` ARE EXCLUDED FOR FREE, and correctly: ApplyPaletteDye
    // carries both over from the staged channel, so they are equal on both sides
    // by construction. A player who weakened a colour or overrode its finish is
    // still wearing that dye and the name must not vanish when they touch a
    // slider.
    //
    // ⚠ THIS IS A COLOUR MATCH, NOT A STORED IDENTITY. There is no dye id
    // downstream of a swatch, which the DyeChannel members above say three
    // times, so this is the only question that can be asked of a channel that
    // was painted before anything recorded an id, which is every channel in
    // every existing save. The cost is that two dyes declaring identical
    // colour AND identical finish are indistinguishable here; the caller
    // decides what to do about a tie, and DyePalette::FindAppliedIn refuses
    // one rather than picking a winner by file order.
    [[nodiscard]] inline constexpr bool ChannelCarriesPaletteDye(const DyeChannel& a_channel,
                                                                 const DyeChannel& a_dye) {
        return a_channel.set && ApplyPaletteDye(a_channel, a_dye) == a_channel;
    }

    // ---- the Special section's writes (spec 2026-08-09) --------------------
    //
    // Beside ApplyPaletteDye for the reason it is here: no test compiles
    // EditorUI.cpp, and the swatch click already shipped once dropping four of
    // its five fields. Each helper takes the staged channel by value and
    // returns it with ONE authored fact changed; strength and player are the
    // player's and none of these touches either, which the tests hold.

    [[nodiscard]] inline constexpr DyeChannel WithSecondStop(DyeChannel a_ch, std::uint8_t a_r,
                                                             std::uint8_t a_g, std::uint8_t a_b) {
        a_ch.secondSet = true;
        a_ch.r2        = a_r;
        a_ch.g2        = a_g;
        a_ch.b2        = a_b;
        return a_ch;
    }

    // OFF MEANS NO COLOUR IS REMEMBERED: the bytes zero with the flag, the
    // same one-meaning-for-one-state rule the hair tint's clear settled.
    [[nodiscard]] inline constexpr DyeChannel WithoutSecondStop(DyeChannel a_ch) {
        a_ch.secondSet = false;
        a_ch.r2        = 0;
        a_ch.g2        = 0;
        a_ch.b2        = 0;
        return a_ch;
    }

    [[nodiscard]] inline constexpr DyeChannel WithDyeGloss(DyeChannel a_ch, std::uint8_t a_gloss) {
        a_ch.palette.glossSet = true;
        a_ch.palette.gloss    = a_gloss;
        return a_ch;
    }

    // Unset is "the dye says nothing about gloss", which is not the same
    // statement as neutral; the byte returns to the struct's own default so an
    // unset block is bit-identical to a never-set one.
    [[nodiscard]] inline constexpr DyeChannel WithoutDyeGloss(DyeChannel a_ch) {
        a_ch.palette.glossSet = false;
        a_ch.palette.gloss    = 128;
        return a_ch;
    }

    [[nodiscard]] inline constexpr DyeChannel WithDyeSheen(DyeChannel a_ch, std::uint8_t a_r,
                                                           std::uint8_t a_g, std::uint8_t a_b) {
        a_ch.palette.sheenSet = true;
        a_ch.palette.sheenR   = a_r;
        a_ch.palette.sheenG   = a_g;
        a_ch.palette.sheenB   = a_b;
        return a_ch;
    }

    [[nodiscard]] inline constexpr DyeChannel WithoutDyeSheen(DyeChannel a_ch) {
        a_ch.palette.sheenSet = false;
        a_ch.palette.sheenR   = 0;
        a_ch.palette.sheenG   = 0;
        a_ch.palette.sheenB   = 0;
        return a_ch;
    }

    [[nodiscard]] inline constexpr DyeChannel WithDyeMode(DyeChannel a_ch, std::uint8_t a_mode) {
        a_ch.mode = a_mode;
        return a_ch;
    }

    [[nodiscard]] inline constexpr DyeChannel WithDyeFlake(DyeChannel a_ch, std::uint8_t a_flake) {
        a_ch.flake = a_flake;
        return a_ch;
    }

    // Where metal starts on the piece, the envmask modes' one slider.
    [[nodiscard]] inline constexpr DyeChannel WithDyeCut(DyeChannel a_ch, std::uint8_t a_cut) {
        a_ch.cut = a_cut;
        return a_ch;
    }

    // ⚠ THE BYTE IS AN OS::DyeBlend::Choice AND ITS ZERO DEFERS, so this is also
    // how a player puts a dye back on the install's setting: there is no
    // WithoutDyeBlend, because kDefault IS the cleared value and a second
    // function saying so would be two ways to spell one state.
    [[nodiscard]] inline constexpr DyeChannel WithDyeBlend(DyeChannel a_ch, std::uint8_t a_blend) {
        a_ch.blend = a_blend;
        return a_ch;
    }

    // A channel is an INDEX. The first three keep names only because the JSON
    // codec's original field names map to them and files written before the
    // widening still use those keys; a garment with eight named pieces has no
    // natural name for its sixth. Casting an arbitrary in-range index to this
    // type is intended and well defined, the underlying type being fixed.
    enum class DyeChannelId : std::uint8_t {
        kPrimary   = 0,
        kSecondary = 1,
        kAccent    = 2,
    };

    // Widened from 3 to 8 on 2026-07-31 and from 8 to 16 on 2026-08-31, both
    // measured rather than guessed. The first: OBI's Abyss Boots carry six
    // shapes, and at three channels four of them shared one colour. The second:
    // a heavily segmented armour showed eight swatches and dyed everything from
    // the eighth piece on as one, which the field named directly ("extremely
    // segmented armors appear to have a limit of 8", user 2026-08-31). Sixteen
    // covers those with room and costs four bytes per channel on a DYED slot
    // only.
    //
    // ⚠ Moving this number again does NOT need a codec bump. The wire carries
    // the channel count per dyed slot from LIBR v9 on, and the decoder stores
    // min(wire, kDyeChannelCount) and skips the rest, so this constant is free
    // to move in either direction. That is the whole reason the count is on the
    // wire; do not "simplify" it back to an implied width.
    //
    // ⚠ WIDENING IT ALSO LENGTHENS THE DYE PANE'S STRIPS, so the strip drawer
    // has to be able to wrap before this number grows again. It can: the tile
    // measures its row against the panel's own width (EditorUI, DrawDyeTile's
    // caller) and folds onto further rows rather than running off the edge.
    inline constexpr std::size_t kDyeChannelCount = 16;

    // Three channels per armour slot, stored whether or not tier 1 can express
    // all three. The count matches ESO so tier 2 can rebind these same channels
    // to tint-mask regions without a schema change or a migration; tier 1 binds
    // them to mesh shapes by index instead (see ChannelForShapeIndex).
    struct SlotDye {
        std::array<DyeChannel, kDyeChannelCount> channels{};

        [[nodiscard]] bool Any() const {
            for (const auto& c : channels) {
                if (c.set) {
                    return true;
                }
            }
            return false;
        }
    };

    // One stored ARMOUR colour, keyed by the slot AND by the garment whose
    // shapes it paints.
    //
    // ⚠⚠ THE GARMENT IS IN THE KEY, AND THAT IS THE WHOLE POINT. This used to
    // be std::array<SlotDye, kBitCount>, one dye per bit with the garment
    // nowhere in it, so a colour belonged to the SLOT. SetStyle writes
    // entries_[bit] and never touched dyes_[bit], which meant restyling a slot
    // handed the old piece's colour to the new one. The field report, 2026-08-28:
    // "if I set the chest piece to Armor1 and dye it yellow, then swap to
    // Armor2, it automatically dyes Armor2 yellow as well. Then I swap to
    // Armor3, and the dyes seem to reset." Both halves are this one fault. The
    // inherit is the missing key; the apparent reset is the same stored channels
    // landing on a mesh whose shapes do not line up with them, because
    // ChannelForShapeIndex binds channels to shape ORDER within whatever is worn.
    //
    // ⚠ THE SLOT STAYS IN THE KEY BESIDE THE GARMENT, exactly as
    // HeadPartDyeEntry keeps its slot beside its part, and for a reason this
    // module already had: a garment may be worn at a bit that is not its own,
    // and two bits showing one mesh are two tiles the player can address. It
    // also means a dye set on a slot holding NOTHING still has somewhere to
    // live, which is what the paint walk's clone-group fill relies on.
    //
    // ⚠ AND IT IS WHAT DyeGrid ALREADY PROMISED. Its note on the retired
    // `orphaned` stripe says "the colour is still in the outfit and comes back
    // with the garment", which was the intent for a long time before the
    // storage could express it. Swap away and back and the colour returns now.
    //
    // ⛔ THIS REVERSES A DELIBERATE, TESTED DECISION. test_outfit.cpp carried
    // "A WITHIN-SLOT SWAP KEEPS ITS DYE ... losing the colour on every restyle
    // would be a worse bug than the orphan". That was the right call while a
    // dye could only belong to a slot: the choice then was inherit or lose.
    // With the garment in the key there is no such trade, so the argument
    // retires with the array it was defending. User's call, 2026-08-28.
    struct ArmourDyeEntry {
        std::uint32_t bit{ 0 };
        StyleRefKey   garment;
        SlotDye       dye{};
    };

    // A player trying colours on many pieces in one outfit accumulates one entry
    // per garment, which is the feature. Bounded anyway on kMaxCustomHeadParts'
    // terms: a corrupt or truncated record can claim any count, and the decoder
    // has to stop somewhere it can defend.
    inline constexpr std::size_t kMaxArmourDyes = 256;

    // One stored weapon colour, keyed by class and hand.
    //
    // ⚠ SPARSE ON PURPOSE, and this is not premature. Mirroring the weapon
    // STYLE dimension literally, three arrays of kWeaponClassCount, cost 4.4 KB
    // on EVERY Outfit and took sizeof(Outfit) to 11760 bytes, which is copied
    // into the undo history, the library vector and the co-save whether or not
    // a single weapon is ever dyed. Weapon dye is addressed by a class-and-hand
    // pair, most outfits will hold none at all, and the accessors below present
    // exactly the same two rules either way.
    //
    // ⚠ THE ARMOUR SIDE USED TO BE THE COUNTEREXAMPLE HERE, on the grounds that
    // it is one slot per bit and every bit is addressable. It is sparse too now
    // (see ArmourDyeEntry), and for a correctness reason rather than a size one,
    // though it does take 4 KB off every Outfit on the way past.
    //
    // SlotDye is 128 bytes because the dye finish work put two DyeMaterial
    // blocks in every DyeChannel, so the array form scales badly with a struct
    // that is still growing.
    struct WeaponDyeEntry {
        WeaponClass cls{ WeaponClass::Sword };
        WeaponHand  hand{ WeaponHand::Both };
        SlotDye     dye{};
    };

    // One stored HEAD PART colour, keyed by the slot and by the part whose
    // geometry it paints. Sparse for WeaponDyeEntry's reason and more so: the
    // slot numbers are invented by whichever mod claimed them, so there is no
    // array to index in the first place.
    //
    // ⚠ THE SLOT TRAVELS WITH THE PART REF, exactly as CustomHeadPartRef's own
    // note argues: a load order that drops the horn mod must leave an ear entry
    // still meaning ears, and an entry naming a slot nothing provides any more
    // has to stay recognisable as that rather than silently becoming its
    // neighbour's. It is also what lets one part ref be told apart from the same
    // part offered in two slots, which nothing does today and nothing forbids.
    //
    // ⚠ A SlotDye RATHER THAN ONE DyeChannel, though every part measured gives
    // exactly one shape. The channel array is what covers the multi-shape part
    // if one ever turns up, through the same ChannelForShapeIndex the armour
    // walk uses, and it costs an outfit that dyes no head part nothing at all
    // because the container is sparse.
    struct HeadPartDyeEntry {
        std::uint32_t slot{ 0 };
        StyleRefKey   part;
        SlotDye       dye{};
    };

    // Tier 1's binding: shapes are enumerated in scenegraph traversal order,
    // which is fixed by the NIF block order and therefore stable for a given
    // mesh. Shape 0 takes primary, shape 1 secondary, everything from shape 2
    // onward folds into accent so a busy mesh stays fully covered rather than
    // having its tail silently undyeable.
    //
    // Tier 2 replaces this function and nothing else: the channels become mask
    // indices and the stored data is unchanged.
    [[nodiscard]] inline constexpr DyeChannelId ChannelForShapeIndex(std::size_t a_index) {
        // One shape one channel, except that the LAST channel absorbs every
        // shape from its own index on, so a mesh busier than the channel count
        // stays fully covered instead of having its tail silently undyeable.
        return static_cast<DyeChannelId>(a_index < kDyeChannelCount - 1
                                             ? a_index
                                             : kDyeChannelCount - 1);
    }

    // How many channels have a shape behind them on a mesh of a_shapeCount
    // shapes. The editor greys out the rest: 52.1% of worn armour meshes in a
    // typical load order hold exactly one shape, so leaving two dead colour
    // pickers on screen reads as a bug rather than a limit.
    [[nodiscard]] inline constexpr std::size_t LiveChannelCount(std::size_t a_shapeCount) {
        return a_shapeCount < kDyeChannelCount ? a_shapeCount : kDyeChannelCount;
    }

    // Lay a list of colours onto a slot in piece order. Shared by "paste onto
    // this slot" and "apply this scheme", because they are the same operation:
    // neither knows or cares which garment the colours came from, so a set made
    // on a three-piece cuirass still does something sensible on one-piece boots.
    // Extra colours are dropped and missing ones leave the channel alone, both
    // in preference to repeating, which would put a colour somewhere the user
    // never chose.
    [[nodiscard]] inline SlotDye MapColoursToSlot(
        std::span<const DyeChannel> a_colours, std::size_t a_shapeCount) {
        SlotDye    out;
        const auto live = LiveChannelCount(a_shapeCount);
        for (std::size_t i = 0; i < live && i < a_colours.size(); ++i) {
            out.channels[i] = a_colours[i];
        }
        return out;
    }

    // The same, with the reflective rail applied.
    //
    // ⚠ THE RAIL BINDS BULK AND NOT AN EXPLICIT CLICK, and that split is the
    // whole per-piece reflective choice. Dyeing a reflective shape costs its
    // shine, permanently: the feature-preserving tint spike tested three ways
    // round it and returned a no on all of them, so the trade is real and the
    // player has to be able to make it PER PIECE. Clicking one stripe is them
    // making it, and the painter honours that regardless of the setting.
    // Pasting a set onto everything is NOT them making it, and that single
    // action would otherwise dull every piece of metal they own at once.
    //
    // So `bDyeReflective` no longer gates the painter. It governs this, and
    // this is reached from paste, paste-all and apply-a-scheme.
    //
    // ⚠ A SKIPPED CHANNEL IS LEFT ALONE, NEVER CLOSED UP. Colours land in PIECE
    // order, so compacting the gap would slide the next colour onto the metal,
    // which is precisely the write the rail exists to prevent.
    [[nodiscard]] inline SlotDye MapColoursToSlotSkipping(
        std::span<const DyeChannel> a_colours, std::size_t a_shapeCount,
        std::span<const bool> a_reflective, bool a_railOn) {
        SlotDye    out;
        const auto live = LiveChannelCount(a_shapeCount);
        for (std::size_t i = 0; i < live && i < a_colours.size(); ++i) {
            if (a_railOn && i < a_reflective.size() && a_reflective[i]) {
                continue;
            }
            out.channels[i] = a_colours[i];
        }
        return out;
    }

    // The byte that changes nothing. The engine's tint is an overlay,
    // D*D + 2*T*D*(1-D), so T = 0.5 is its algebraic identity and byte 128 is
    // what lands there. Named here rather than typed as a literal in the editor
    // because the obvious guess for "no change" is 0, and 0 collapses the
    // overlay to D*D, which squares every diffuse value and darkens the whole
    // garment. Someone hunting for neutral by dragging the picker to black gets
    // the most visible change in the range and concludes the feature is broken.
    // The editor seeds a freshly enabled channel here and offers a button that
    // returns to it. See OutfitDye.h for the shader arithmetic.
    inline constexpr std::uint8_t kDyeNeutral = 128;

    // The material that actually renders on this channel.
    //
    // ⚠ MERGED PER PROPERTY, never chosen as a whole block. A player who tuned
    // the gloss has not chosen a sheen, so the dye's own sheen must still show
    // through, and a later swatch click must still be able to change it.
    [[nodiscard]] inline constexpr DyeMaterial EffectiveMaterial(
        const DyeChannel& a_channel, bool a_overrideAllowed) {
        DyeMaterial out = a_channel.palette;
        if (a_overrideAllowed) {
            if (a_channel.player.sheenSet) {
                out.sheenSet = true;
                out.sheenR   = a_channel.player.sheenR;
                out.sheenG   = a_channel.player.sheenG;
                out.sheenB   = a_channel.player.sheenB;
            }
            if (a_channel.player.glossSet) {
                out.glossSet = true;
                out.gloss    = a_channel.player.gloss;
            }
        }
        return out;
    }

    // Why a mesh piece cannot take a dye. Pure, and separate from the material
    // feature it is derived from, because the FEATURE is an engine type this
    // header must not name and because two features can share one reason: a
    // face and a facegen-tinted body are both "your character's own colouring"
    // as far as the user is concerned.
    enum class DyeSkipReason : std::uint8_t {
        kNone = 0,          // dyeable, no reason to give
        kHair,              // kHairTint: the tint is a vertex-colour mask
        kCharacterColour,   // kFaceGen / kFaceGenRGBTint: carries the skin tone
        kEyes,              // kEye
        kGlow,              // kGlowMap: dyeing spreads the glow past its mask
        kUnsupported,       // the parallax family
        kReflectiveOff,     // kEnvironmentMap while bDyeReflective is off
        // ⚠ BIPED 9 IS NOT THE SHIELD'S ALONE. The engine stages the off-hand
        // weapon, and a torch, in the actor race's shield slot. Neither is
        // reachable by the armour walk (OS-111: they stage .item and .addon but
        // their 3D arrives through the part-3D loader, so objects[9].partClone
        // stays null), and neither should take a shield's colour even if it
        // were. Two reasons rather than one because the fixes differ: an
        // off-hand weapon becomes dyeable when weapon dye ships, a torch never
        // does.
        kOffHandWeapon,
        kTorch,
        // ⚠ NOT A MATERIAL VERDICT LIKE THE ONES ABOVE. It comes off the shader
        // FLAGS rather than off the feature: kDecal and kDynamicDecal together,
        // which is geometry the engine paints onto at runtime. It has to come
        // off the flags, because the same overlay reads EnvironmentMap on one
        // weapon and Default on another, so no feature identifies it. The
        // measurement, and why it is BOTH BITS and not either, sits beside the
        // test in OutfitDye's shape walk.
        //
        // ⚠ SLSF2_Weapon_Blood IS NOT THE TEST AND MUST NOT BE RE-PROPOSED. It
        // is the engine's own name for this geometry, which is exactly why it
        // looked certain, and it reads false on every shape it should have
        // caught. TRACKER OS-123 carries the measurement.
        //
        // ⚠ NAMED FOR THE TEST, NOT FOR THE FIRST THING IT CAUGHT. It shipped
        // as kWeaponBlood for the overlay that produced it: that shape
        // traverses FIRST on every weapon measured, so left dyeable it claims
        // Primary and the first colour the player picks lands on geometry that
        // stays invisible until the blade draws blood (OS-123). A hairline sets
        // the same two bits and arrives here too, measured off the field log
        // 2026-08-31, and the blood name had every later reader looking for a
        // weapon.
        //
        // ⚠ THE DYEPASS LOG STILL SPELLS THIS FIELD `blood=`, on purpose: it is
        // the one-line field check OS-123 was closed on and TRACKER names it by
        // that spelling. Renaming the field would strand that row's evidence.
        kDecal,
        // ⚠ NOT A PROPERTY OF THE MESH, unlike every reason above it: this one
        // says another CONTROL has the resource. The eye's second colour and
        // the sclera colour share one shader register (gStopB), the split wins
        // it, and the painter already says so in the log. The stripe crosses so
        // the trade is visible rather than only logged.
        //
        // ⚠ A CROSS MEANS "CANNOT ACT RIGHT NOW", NEVER "YOUR STORED COLOUR IS
        // LOST". Clearing the second colour hands the register back and the
        // sclera paints again from the bytes it kept the whole time.
        kEyeSecondColour,
        // ⚠ AN ORDERING CONSTRAINT, SAID ON THE STRIPE RATHER THAN LEARNED BY
        // TRYING. The eye's second colour takes the far half of the texture, so
        // with no iris colour on the near half there is nothing for it to sit
        // beside and half an eye would paint. PaintEyeTint refuses it and logs;
        // the stripe carries this so a player is told BEFORE the click, which
        // is the report that produced it: "if we dye the second colour only,
        // nothing happens".
        kEyeNeedsIris,
        // ⚠ A POINTER AT ANOTHER CONTROL, NOT A REFUSAL, and it exists because
        // kHair's sentence is true and useless on a head part. A strand shape
        // inside a hair IS painted, by the hair colour on the tile two rows up,
        // so telling the player "dyeing it would streak along the strands" leaves
        // them believing the hair cannot be coloured at all. The user asked for
        // the refusal to be spoken rather than the shape hidden (2026-08-17), and
        // a spoken refusal that points nowhere is the half of that which fails.
        //
        // ⚠ NOT A REPLACEMENT FOR kHair. That one still fires on the ARMOUR
        // walk, where a hood's strands are worn geometry that the hair colour
        // does NOT reach, so pointing at the hair tile there would be a lie.
        kHairOwnColour,
        // ⚠ `kNoMaterial` LIVED HERE FOR ONE BUILD AND IS GONE. It named a shape
        // with no lighting property at all, so the grid could list FSMP's
        // collision proxies (`VirtualGround`, `VirtualHead`,
        // `VirtualHairCollision_N`, which ship as head-part EXTRAS on any SMP
        // hair) with a reason instead of hiding them. The field's call was the
        // opposite for this one class: a crossed strand row teaches that the
        // hair colour paints it, a crossed collision proxy teaches nothing, and
        // the player cannot act on a shape they never chose. Such a shape gets
        // no stripe now, on every walk, and the head census still prints it as
        // "(no lighting shader)" so the log can answer for it.
    };

    // ⚠ `NameIsBloodDecal` LIVED HERE AND IS GONE, DELETED ON EVIDENCE.
    // Blood overlays were matched by name (`edgeblood`, `bloodlight`) while the
    // real predicate was being measured. It is now `kDecal | kDynamicDecal` in
    // OutfitDye.cpp, read off the shader property, and a field run carrying BOTH
    // answers settled it: across a whole session there were ZERO shapes where
    // the names decided something the flags missed, and zero the other way. They
    // agreed on every shape, so the list was redundant rather than merely ugly.
    //
    // ⚠ IF A NAME RULE IS EVER NEEDED AGAIN, NEVER MATCH THE BARE WORD "BLOOD".
    // Skyrim ships the Bloodskal Blade, and Bloodthorn, so that test silently
    // refuses to dye a real weapon and leaves no swatch and no reason on screen.
    // Match measured tokens, case-insensitively: one nif wrote `Edgeblood01` and
    // the next `EdgeBlood01`, which presents as "works on my sword, not my axe".

    // Biped 9 is the shield's bit, and the engine also stages an off-hand weapon
    // and a torch there. When one of those holds the slot, does it still earn a
    // synthetic one-shape tile of its own?
    //
    // ⚠ ONLY IF IT IS STILL GENUINELY UNDYEABLE, which a weapon no longer is.
    // OS-111 gave every foreign occupant a tile so a stored shield colour would
    // not read as orphaned, and that was correct while nothing in that slot
    // could take a dye. The weapon walk claims biped 9 and paints it now, so a
    // weapon gets a real tile of its own and the synthetic one is a second, dead
    // copy sitting beside it insisting weapons cannot be dyed (OS-120).
    //
    // ⚠ A TORCH KEEPS ITS TILE and that is the whole reason this is a question
    // rather than a deletion. A torch never dyes, so without the tile its slot
    // vanishes from the grid with the shield's colour still stored behind it,
    // which is the orphan OS-111 was fixing.
    //
    // ⚠ NOT EMITTING A TILE IS NOT FORGETTING A COLOUR. The shield's own dye
    // stays in the outfit and comes back with the shield; only the control goes.
    [[nodiscard]] inline constexpr bool ForeignOccupantEarnsATile(DyeSkipReason a_reason) {
        return a_reason != DyeSkipReason::kOffHandWeapon;
    }

    // Whether a shape earns a LABELLED, dyeable channel on its tile.
    //
    // ⚠ FALSE DOES NOT MEAN "SKIP IT". The shape is still counted and still
    // consumes its traversal index, because OutfitDye::Repaint walks every shape
    // and the editor's channel numbering only lines up with the renderer's while
    // both count the same things. False means the channel it lands on carries no
    // name, no swatch and no colour of its own.
    //
    // ⚠ ONE PREDICATE FOR BOTH WALKS, and it is one because it was two. The
    // armour walk and the weapon walk grew their own copy of this test a day
    // apart, one for the player's body riding on a slot and one for a weapon's
    // blood overlay. A rule about what a SHAPE IS has no business differing by
    // which walk happened to find it, and two copies is exactly how a garment
    // carrying a blood flag becomes the one place a blood overlay still gets
    // drawn.
    [[nodiscard]] inline constexpr bool DyeShapeEarnsAStripe(DyeSkipReason a_reason) {
        return a_reason != DyeSkipReason::kCharacterColour &&
               a_reason != DyeSkipReason::kDecal;
    }

    // What a dyeable thing IS, which decides how it is keyed.
    //
    // ⚠ A DISCRIMINATED KEY RATHER THAN ONE WIDENED INDEX, deliberately.
    // Widening slotBit to cover 0 through 41 looks simpler and breaks a
    // separation this codebase maintains on purpose: WeaponSlots.h opens by
    // calling weapon slots "an unrelated index space from the armor EDITOR bits
    // in SlotMask.h", OutfitDye.cpp static_asserts dye bits to the 32 armour
    // biped objects, and the OFF-HAND WEAPON LIVES AT BIPED 9, which is the
    // shield's bit. One index would collide there head on, silently, on any
    // character carrying a sword in the left hand.
    enum class DyeTarget : std::uint8_t {
        kArmour,  // keyed by slotBit
        kWeapon,  // keyed by weaponClass + weaponHand
        // ⚠ KEYED BY NOTHING. Hair is not a slot and not a weapon class, so it
        // carries no key field at all: there is exactly one hair tile and its
        // colour lives in Outfit::hairTint rather than in a DyeChannel. This is
        // deliberate and is what keeps hair out of CollapseCloneDyes and the
        // clone grouping, both of which walk slot bits and so never see it.
        kHair,
        // ⚠ KEYED BY NOTHING EITHER, on exactly the terms kHair sits on: one
        // tile, one RGB in Outfit::eyeTint, no slot bit and no weapon pair.
        //
        // ⚠ AND IT IS PAINTED TWO DIFFERENT WAYS depending on the eye, which is
        // measured rather than a design choice. An ordinary eye keeps its
        // colour in its diffuse TEXTURE and takes a tinted twin of it. A GLOWING
        // eye keeps its colour in the property's EMISSIVE, and swapping its
        // material stops the glow mask gating that emissive, which lights the
        // whole eyeball instead of the iris ring. So a glowing eye is tinted
        // through emissiveColor alone and its material is never touched. Both
        // paths are in SpikeEyeDye's successor in OutfitDye.cpp and the choice
        // is made per eye from the property, never from the feature.
        //
        // ⚠ TWO STRIPES ON THE ONE TILE, AND THE CHANNEL IS WHICH. Stripe 0 is
        // the iris (Outfit::eyeTint), stripe 1 the sclera (Outfit::scleraTint),
        // on one tile because they are one eye (user 2026-08-13: "make them in
        // the same dye group, it's sleeker"). The sclera briefly shipped as its
        // own target; every kEyes site that reads a colour now switches on the
        // CHANNEL, and channels 2 and up mean nothing here and must write
        // nothing, or a bulk loop over eight channels would land its last
        // write wherever the default arm pointed.
        kEyes,
        // A HEAD PART's geometry: a horn, an ear, or one of the shapes a modded
        // hair carries that is not a strand. Keyed by the head-part SLOT plus
        // the StyleRefKey of the part that geometry came from.
        //
        // ⚠⚠ THE SLOT ALONE IS NOT A KEY, AND THAT IS MEASURED RATHER THAN
        // CAUTIOUS. Field 2026-08-17: the hair part itself carries `model=''`
        // and contributes NO geometry, while its nine extra parts contribute one
        // each, so slot 3 names nine shapes of which one was dyeable. The design
        // this replaced keyed on the slot alone and would have had one control
        // for a gold ornament, a cloth piece, three FSMP collision proxies and
        // four strand meshes.
        //
        // ⚠ THE PART REF RATHER THAN THE TRAVERSAL INDEX, on the user's call of
        // the same day. Every shape measured took its name from its OWN
        // BGSHeadPart record, so a plugin-and-form pair addresses it exactly and
        // survives a load order change, where an index moves the moment a hair's
        // shape order does. The same log had a part whose mesh is missing from
        // disk contributing nothing at all, so that order is not stable in
        // practice, which is the argument in one reading.
        //
        // The index is not gone, it moved DOWN a level: a part that gives more
        // than one geometry uses ChannelForShapeIndex within itself, exactly as
        // an armour slot does across its shapes. Nothing has been seen to do
        // that yet (every part measured gave exactly one geometry), so the
        // common case is channel 0 alone.
        kHeadPart,
    };

    // One shape of a worn mesh, as the editor needs to label it.
    //
    // Plain data, and deliberately here rather than in OutfitDye.h: that header
    // includes PCH.h and so pulls in all of CommonLibSSE, which would keep
    // DyeGrid out of a pure test executable. Nothing in this struct is an
    // engine type.
    struct ShapeInfo {
        // ⚠ FIRST, NOT APPENDED, AND THAT BREAKS EVERY POSITIONAL CALLER ON
        // PURPOSE. Appending it with a kArmour default would have kept both
        // producers in OutfitDye.cpp compiling and correct today, and would have
        // made the NEXT one silently wrong: a weapon shape built without setting
        // target reads as armour on slot bit 0 and lands on somebody's head. The
        // rule written on DyeGate::ClassifySlot applies to a struct just as
        // well. A new dimension breaks its callers or it gets forgotten.
        DyeTarget     target{ DyeTarget::kArmour };
        WeaponClass   weaponClass{ WeaponClass::Sword };  // kWeapon only
        WeaponHand    weaponHand{ WeaponHand::Both };     // kWeapon only
        std::uint32_t slotBit{ 0 };                       // kArmour only
        // kHeadPart only: the head-part slot, and the part the geometry was
        // built from. ⚠ THE PAIR, NEVER THE SLOT ALONE, for the reason on
        // DyeTarget::kHeadPart: one slot's parts contribute one geometry each and
        // a hair's nine shapes all sit on slot 3.
        std::uint32_t headSlot{ 0 };
        StyleRefKey   headPart;
        // Traversal order. ⚠ ITS SCOPE IS THE KEY, NOT THE ACTOR. For armour it
        // counts shapes within the slot and for a head part it counts them
        // within the PART, so channel 0 is that part's only shape in every case
        // measured so far.
        std::size_t   index{ 0 };  // feeds ChannelForShapeIndex
        std::string   name;        // the BSGeometry's own name, e.g. "Cloak"
        bool          dyeable{ false };  // false = its material feature is skipped
        DyeSkipReason reason{ DyeSkipReason::kNone };  // why not, when !dyeable
        // kEnvironmentMap, whether or not bDyeReflective currently lets it dye.
        // Separate from `reason` because it is true in BOTH states and the
        // editor has something to say either way: dyed, this piece goes matte,
        // and undyed it is the one piece of the garment that will not change.
        bool          reflective{ false };
        // The engine model this slot's geometry was cloned FROM, as an opaque
        // number. Every shape on one slot carries the same value.
        //
        // ⚠ AN IDENTITY, NEVER A POINTER TO FOLLOW. It is compared against
        // other slots' values and nothing else; nothing may read through it and
        // it must not outlive the pass that produced it. Opaque because this
        // header names no engine types, which is also what keeps the grouping
        // testable without an engine.
        //
        // Zero means the producer could not identify the slot, and zero claims
        // nothing: an unidentified slot groups with no other, including with
        // another unidentified one.
        std::uintptr_t sourceId{ 0 };
    };

    // Which slot bit owns each slot's dye, once clones are folded together.
    // Indexed by bit; a bit that owns itself is its own entry.
    using DyeCloneOwners = std::array<std::uint32_t, kBitCount>;

    // A garment whose armature covers more than one biped slot is attached ONCE
    // PER COVERED SLOT, and the engine hands each slot its own clone of the one
    // mesh. Measured 2026-08-02 on a helmet covering 31 and 43: same source
    // model, two `partClone`s, two shader properties, and therefore two dye
    // tiles that could be given two different colours for one helmet.
    //
    // ⚠ THE ENGINE'S BEHAVIOUR, NOT A TRANSMOG DEFECT. It happens in vanilla,
    // where two identical copies of one mesh are indistinguishable; dye is
    // simply the first thing that ever coloured them apart.
    //
    // The rule is source equality, and it is true BY CONSTRUCTION rather than
    // by fitting the data: an armature owns its model, so two different
    // armatures cannot present the same source. Name comparison was considered
    // and rejected - the clones do share a name, so it would work on this data,
    // but two unrelated pieces are free to share one and the failure mode is
    // silently merging two real controls into one.
    //
    // Groups take the LOWEST covered bit, and the mapping is transitive, so
    // three slots on one armature collapse onto one owner rather than two.
    [[nodiscard]] inline DyeCloneOwners DyeCloneOwnersFrom(
        std::span<const std::uintptr_t> a_sourceIds) {
        DyeCloneOwners out{};
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            out[bit] = bit;
        }
        const auto n = a_sourceIds.size() < kBitCount ? a_sourceIds.size() : kBitCount;
        for (std::uint32_t bit = 0; bit < n; ++bit) {
            if (a_sourceIds[bit] == 0) {
                continue;  // unidentified: claims nothing and joins nothing
            }
            for (std::uint32_t lower = 0; lower < bit; ++lower) {
                if (a_sourceIds[lower] == a_sourceIds[bit]) {
                    // out[lower], not `lower`: a third slot on the same armature
                    // has to land on the group's owner rather than on the middle
                    // member that happened to match it first.
                    out[bit] = out[lower];
                    break;
                }
            }
        }
        return out;
    }

    // The same map, taken from a shape snapshot. Every shape on a slot carries
    // that slot's source, so the first one seen answers for the slot.
    //
    // A slot with no shapes in the snapshot contributes a zero and therefore
    // groups with nothing, which is right: it has no tile and paints nothing.
    // ⚠ ARMOUR SHAPES ONLY, and the filter is load bearing rather than tidy.
    // slotBit is shared storage that means nothing on a weapon, and the engine
    // stages the OFF-HAND WEAPON AT BIPED 9, the shield's own bit. Without this
    // filter an off-hand sword claims bit 9 in the table below, and a real
    // shield built from the same source model is folded into its group and
    // loses its tile entirely.
    [[nodiscard]] inline DyeCloneOwners DyeCloneOwnersFromShapes(
        std::span<const ShapeInfo> a_shapes) {
        std::array<std::uintptr_t, kBitCount> sources{};
        for (const auto& s : a_shapes) {
            if (s.target != DyeTarget::kArmour) {
                continue;
            }
            if (s.slotBit < kBitCount && sources[s.slotBit] == 0) {
                sources[s.slotBit] = s.sourceId;
            }
        }
        return DyeCloneOwnersFrom(sources);
    }

    // Geometry the engine builds for a worn armour is named
    // "<name> (<formID>)[<i>]/<name> (<formID>) [<weight>%]". When the forms
    // carry no display name that collapses to " (00012E4C)[1]/ (00000D64) [50%]",
    // which is what a player sees in the dye tile's tooltip today and tells them
    // nothing at all. Measured 2026-08-02: four of eight shapes on a dressed
    // character read that way, including the one on slot 43 that turned out to
    // be a helmet's ears.
    //
    // Pull the form IDs out here so a caller WITH engine access can resolve
    // them. Pure, so the parsing half is testable without the engine, which is
    // the half that can be wrong in a way nobody notices.
    //
    // Returns how many were found, writing at most a_max. Zero means the name
    // carries no form ID and should be left exactly as it is: a nif-authored
    // name like "CuirassIvory_1:0" is already the best label available.
    [[nodiscard]] inline std::size_t EngineShapeFormIDs(std::string_view a_name,
                                                        std::uint32_t*   a_out,
                                                        std::size_t      a_max) {
        std::size_t found = 0;
        for (std::size_t i = 0; i < a_name.size() && found < a_max; ++i) {
            if (a_name[i] != '(') {
                continue;
            }
            std::size_t   j      = i + 1;
            std::uint32_t v      = 0;
            std::size_t   digits = 0;
            while (j < a_name.size() && digits < 8) {
                const char c = a_name[j];
                std::uint32_t d = 0;
                if (c >= '0' && c <= '9') {
                    d = static_cast<std::uint32_t>(c - '0');
                } else if (c >= 'A' && c <= 'F') {
                    d = static_cast<std::uint32_t>(c - 'A' + 10);
                } else if (c >= 'a' && c <= 'f') {
                    d = static_cast<std::uint32_t>(c - 'a' + 10);
                } else {
                    break;
                }
                v = (v << 4) | d;
                ++digits;
                ++j;
            }
            // ⚠ EXACTLY EIGHT DIGITS AND A CLOSING PAREN, and the strictness is
            // the point. Every letter in "abc" and "dead" is a hex digit, so a
            // looser rule turns an ordinary parenthesised word in a
            // nif-authored name into a form ID and replaces a good label with a
            // wrong one. A form ID printed by the engine is always eight wide.
            if (digits == 8 && j < a_name.size() && a_name[j] == ')') {
                a_out[found++] = v;
                i              = j;
            }
        }
        return found;
    }

    // The translation key for a reason, or null when there is nothing to say.
    // Null rather than an empty string so a caller that forgets to check gets a
    // crash in testing instead of a blank row in the field.
    [[nodiscard]] inline constexpr const char* DyeSkipKey(DyeSkipReason a_reason) {
        switch (a_reason) {
            case DyeSkipReason::kHair:            return "$FR_DyeSkip_Hair";
            case DyeSkipReason::kCharacterColour: return "$FR_DyeSkip_Character";
            case DyeSkipReason::kEyes:            return "$FR_DyeSkip_Eyes";
            case DyeSkipReason::kGlow:            return "$FR_DyeSkip_Glow";
            case DyeSkipReason::kUnsupported:     return "$FR_DyeSkip_Unsupported";
            case DyeSkipReason::kTorch:           return "$FR_DyeSkip_Torch";
            case DyeSkipReason::kEyeSecondColour: return "$FR_DyeSkip_EyeScleraBusy";
            case DyeSkipReason::kEyeNeedsIris:    return "$FR_DyeSkip_EyeNeedsIris";
            case DyeSkipReason::kHairOwnColour:   return "$FR_DyeSkip_HairOwnColour";
            // ⚠ ORPHANED BY OS-120 AND SHARING kNone's BRANCH, not deleted from
            // the enum. ForeignReasonFor still returns it, because it is what
            // ForeignOccupantEarnsATile reads to tell a weapon from a torch. It
            // simply never reaches a stripe any more, so its sentence became a
            // key nothing could read and $FR_DyeSkip_OffHand went with it.
            case DyeSkipReason::kOffHandWeapon:
            // ⚠ NOTHING TO SAY, ON PURPOSE, and it shares kNone's branch rather
            // than carrying words of its own. A blood overlay is filtered out of
            // BOTH walks, so no stripe ever holds this reason and any sentence
            // written here would be unreachable. It sits in the switch so the
            // enum stays exhaustive; give it a key the day something actually
            // shows one.
            case DyeSkipReason::kDecal:
            // ⚠ ORPHANED when bDyeReflective stopped gating the painter. A
            // reflective shape is always dyeable now, so no stripe ever carries
            // this reason and its sentence became a key nothing could read. The
            // enumerator stays because a stored outfit may still name it.
            //
            // ⚠ IT BELONGS IN THIS GROUP AND NOT ONE LINE HIGHER. Placed above
            // kTorch it falls through and answers with the TORCH sentence, which
            // is what the first cut of this change did and what the test caught.
            case DyeSkipReason::kReflectiveOff:
            case DyeSkipReason::kNone:            break;
        }
        return nullptr;
    }

    class Outfit {
    public:
        // The 32-slot shape constant, reachable as Outfit::kBitCount for code
        // (SlotClaims.h and the coverage helpers) that wants to name it off the
        // type rather than the bare namespace constant above. It describes the
        // type's SHAPE, not its state, which is why it sits ahead of the
        // instance data.
        //
        // Aliasing rather than replacing keeps OS::kBitCount available to the
        // several files that already use it unqualified. Inside this class the
        // unqualified name now resolves to this member instead of the namespace
        // constant, which changes nothing: it IS that constant, by reference,
        // so the two can never drift apart.
        static constexpr std::uint32_t kBitCount = OS::kBitCount;

        std::string name;
        bool        favorite{ false };

        // Body dimension (OBody NG integration, 2026-07-22). A third dimension
        // alongside armor bits and weapon classes, but NOT slot-shaped: it is
        // one setting for the whole outfit, not per-slot.
        //
        // Both default to "leave the body alone", so an outfit written before
        // this existed - or one made on a setup without OBody - behaves exactly
        // as it always did. That is what lets the codec read v1 records by
        // simply not filling these in.
        std::string obodyPreset;                    // installed OBody name
        // Stable Fitting Room custom-preset identity. Mutually exclusive with
        // obodyPreset; the editor clears one whenever it assigns the other.
        // The ID survives custom-preset renames because outfits must not key a
        // reference on user-facing text.
        std::string customBodyPresetId;
        ORefitMode  orefit{ ORefitMode::kDefault };
        PushUpMode  pushUp{ PushUpMode::kNone };
        HairMode    hair{ HairMode::kAuto };
        HairTint    hairTint{};
        // Per-outfit EYE colour, same shape as hairTint and for the same reason:
        // one RGB with a presence bit, cleared meaning "leave her own eyes
        // alone". Reuses HairTint's type because the two are the same four
        // bytes and a second identical struct would only invite them to drift.
        //
        // ⚠ NOT A DyeChannel and NOT a slot. Eyes have no coverage, no hide mode
        // and no passthrough, so this stays out of the slot array exactly as
        // hairTint does, which is what keeps it out of CollapseCloneDyes and the
        // clone grouping.
        HairTint    eyeTint{};
        // Per-outfit SCLERA colour, the other half of the eye pair: the iris
        // mask splits an eye texture into the disc and the white around it,
        // eyeTint colours the disc, this colours the white. Same shape, same
        // terms, cleared meaning "leave the white alone".
        //
        // ⚠ ON AN EYE WITH NO USABLE MASK THIS PAINTS NOTHING, by design.
        // Without the mask there is no way to say where the sclera is, and a
        // whole-eye fallback here would paint the iris with the sclera colour,
        // which is the exact defect the mask fixed, inverted.
        HairTint    scleraTint{};
        // The eye's SECOND colour, and where it lands is a separate question
        // from what it is. Cleared means the eye takes one colour, which is
        // every outfit made before this existed.
        //
        // ⚠⚠ WHAT IT MEANS DEPENDS ON THE MESH, AND THE CENSUS MEASURED WHICH.
        // The paint splits the TEXTURE down the middle, and eye models come in
        // two kinds: a SHARED disc, where both eyeballs read the same texels
        // (1123 of 1549 parts on the dev rig), gives a two-tone iris on both
        // eyes; a UV-SPLIT set, where the eyes read disjoint halves (426
        // parts), gives one colour per eye. One field, one paint, two looks,
        // and the part decides. docs/research/2026-08-13-eye-structure-census.md
        // carries the measurement.
        //
        // ⚠ IT TAKES gStopB FROM THE SCLERA WHEN BOTH ARE SET, which is the
        // precedence DyeTexture already ships and says out loud in the log. The
        // register holds one colour; the editor crosses the sclera stripe so
        // the refusal is visible rather than only logged.
        //
        // ⚠ SAME SHAPE AS THE OTHER THREE TINTS, deliberately: HairTint is four
        // bytes with a presence bit, and a fourth identical struct would only
        // invite them to drift.
        HairTint    eyeTint2{};
        // How the eye colour is mixed with the eye texture under it, as an
        // OS::DyeBlend::Choice stored as a byte. 0 defers to the eye's own
        // default, which is Overlay: an eye texture carries white reflective
        // highlights and a base-dependent curve leaves them standing where a
        // repaint takes them with everything else (user 2026-08-13).
        //
        // ⚠⚠ A PLAIN BYTE ON THE OUTFIT, NOT A FIELD ON HairTint, and the
        // reason is that hairTint is the same type: a blend on the struct would
        // hand hair a field hair cannot use, and the hair colour goes through
        // facegen where there is no texture to mix with at all.
        //
        // ⚠⚠ ONE BYTE FOR THE PAIR, iris and sclera together, on the terms
        // EyeTintDiffers already answers for them: on a masked eye the sclera
        // runs the SAME curve as the iris by construction, so two bytes would
        // be a second control that cannot express anything the first does not.
        //
        // ⚠⚠ NOT A NINTH REFRESH DIMENSION. EyeTintDiffers is dimension eight
        // and is already named by all six lists, so this rides inside it and
        // nothing in RefreshGate.h moves. Adding a dimension for it would mean
        // six lists to edit, five of which have each shipped broken once.
        std::uint8_t eyeBlend{ 0 };
        // Per-outfit hair STYLE: the BGSHeadPart to wear, empty for "leave
        // their own alone". A StyleRefKey because that is the shape every
        // form reference already has here, mod name plus local form ID, so
        // the codec and the JSON both already know how to carry one. It is
        // NOT a slot entry: a hair style has no coverage, no hide mode and
        // no passthrough, so it never enters the slot array.
        StyleRefKey hairStyle{};
        // Per-outfit EYE and BROW type (OS-161), on exactly the terms hairStyle
        // sits on: a BGSHeadPart reference, empty meaning leave their own alone,
        // and not a slot entry.
        //
        // ⚠ THESE WERE A CHARACTER EDIT FOR ONE BUILD and are outfit state now
        // (user 2026-08-06). The difference is not cosmetic: as a character edit
        // the write went straight to the actor base and the game saved it, so
        // there was nothing to store and no version to bump. As outfit state the
        // outfit owns the answer, which is what lets one outfit carry a look and
        // another leave it alone.
        //
        // ⚠ PLAYER ONLY, and the outfit still carries the field for a follower's
        // outfit. That is deliberate rather than sloppy: the restriction is
        // about which MECHANISM can apply the part, not about what an outfit is
        // allowed to record, and a follower route arriving later should find the
        // data already there rather than needing another version bump. The
        // editor hides the rows for her; the push simply never applies them.
        StyleRefKey eyes{};
        StyleRefKey brows{};
        // Per-outfit FACIAL HAIR (OS-196), on exactly the same terms as the
        // two above: a beard is the engine's kFacialHair head part, and one
        // outfit carrying a full beard while another is clean-shaven is the
        // same statement as one outfit carrying green eyes.
        //
        // ⚠ ITS SEX FILTER IS CHARACTER CREATION'S, NOT A RULE OF OURS. Facial
        // hair records are male-flagged, so a female character is offered a
        // short list or none at all. That is HeadPartPlan::Judge answering the
        // same way the vanilla chargen menu does, and the row is left in place
        // rather than hidden on sex: a load order is free to ship a beard for
        // anyone, and hiding the row would silently disagree with the game.
        StyleRefKey facialHair{};
        // Head-part slots this load order invented: horns, cat ears, and
        // anything else authored at a head-part type the engine never named.
        //
        // ⚠ A LIST RATHER THAN FIELDS, AND THAT IS FORCED. The four references
        // above are fields because the engine names exactly those four types.
        // A discovered slot's number is chosen by whichever mod claimed it, so
        // the set exists only at run time; MEASURED on the dev rig 2026-08-13,
        // five of them, at 32, 69, 71, 106 and 110. Naming fields for those
        // would encode one rig's load order into every save.
        //
        // Empty means this outfit names none, which is the same statement an
        // empty StyleRefKey makes above: leave whatever they have alone.
        //
        // ⚠ ONE ENTRY PER SLOT AT MOST. SetCustomHeadPart enforces it, because
        // two entries for one slot would make which one wins depend on write
        // order rather than on anything the player did.
        std::vector<CustomHeadPartRef> customHeadParts;

        // What this outfit names for a slot, or an empty key for "leave it
        // alone". Linear over a list that holds one entry per slot the player
        // actually set, so it is a handful at most.
        [[nodiscard]] StyleRefKey CustomHeadPart(std::uint32_t a_slot) const {
            for (const auto& part : customHeadParts) {
                if (part.slot == a_slot) {
                    return part.key;
                }
            }
            return {};
        }

        // Name a part for a slot, or clear it with an empty key. Clearing
        // ERASES the entry rather than storing an empty one, so an outfit that
        // has never touched a slot and one that was set and then cleared are
        // the same outfit and encode to the same bytes.
        void SetCustomHeadPart(std::uint32_t a_slot, StyleRefKey a_key) {
            for (auto it = customHeadParts.begin(); it != customHeadParts.end(); ++it) {
                if (it->slot == a_slot) {
                    if (a_key.Empty()) {
                        customHeadParts.erase(it);
                    } else {
                        it->key = std::move(a_key);
                    }
                    return;
                }
            }
            if (!a_key.Empty()) {
                customHeadParts.push_back(CustomHeadPartRef{ a_slot, std::move(a_key) });
            }
        }

        void SetStyle(std::uint32_t a_bit, StyleRefKey a_key) {
            if (a_bit >= kBitCount) return;
            entries_[a_bit] = { SlotEntry::Kind::kStyle, std::move(a_key) };
        }
        void SetHide(std::uint32_t a_bit) {
            if (a_bit >= kBitCount) return;
            // A hidden entry may retain the style it covers. The renderer
            // considers only kind==kHide, while keeping the key here makes
            // Show reversible after copying, saving, and reopening an outfit.
            // Calling SetHide on an already-hidden entry preserves that key.
            StyleRefKey covered;
            if (entries_[a_bit].kind == SlotEntry::Kind::kStyle ||
                entries_[a_bit].kind == SlotEntry::Kind::kHide) {
                covered = entries_[a_bit].style;
            }
            entries_[a_bit] = { SlotEntry::Kind::kHide, std::move(covered) };
        }
        // Persistence decoder seam: the armor wire format has always carried
        // mod/form fields for Hide entries. Old records contain an empty key;
        // new records use those existing fields for the covered style.
        void SetHiddenWithRestore(std::uint32_t a_bit, StyleRefKey a_key) {
            if (a_bit >= kBitCount) return;
            entries_[a_bit] = { SlotEntry::Kind::kHide, std::move(a_key) };
        }
        void SetPassthrough(std::uint32_t a_bit) {
            if (a_bit >= kBitCount) return;
            entries_[a_bit] = {};
        }

        void SetDye(std::uint32_t a_bit, DyeChannelId a_channel, DyeChannel a_value) {
            if (a_bit >= kBitCount) return;
            if (static_cast<std::size_t>(a_channel) >= kDyeChannelCount) return;
            SlotDye staged = DyeFor(a_bit);
            staged.channels[static_cast<std::size_t>(a_channel)] = a_value;
            SetArmourDye(a_bit, entries_[a_bit].style, staged);
        }

        // Drop every colour stored against the garment currently in one slot.
        //
        // ⚠ THE GARMENT CURRENTLY IN IT, which is what "Reset piece" means and
        // is why this is not a purge of the bit. A colour stored against a
        // garment that is not being worn is not an orphan any more, it is the
        // colour waiting for that garment to come back, and taking it here would
        // undo the whole point of putting the garment in the key.
        //
        // Its own method rather than a loop of SetDye at each site because that
        // loop is bounded by kDyeChannelCount, and kDyeChannelCount is
        // deliberately free to move in either direction (see its note). A count
        // written out at four call sites is four places to miss when it does.
        void ClearDye(std::uint32_t a_bit) {
            if (a_bit >= kBitCount) return;
            const auto& key = entries_[a_bit].style;
            for (auto it = armourDyes_.begin(); it != armourDyes_.end(); ++it) {
                if (it->bit == a_bit && it->garment == key) {
                    armourDyes_.erase(it);
                    return;
                }
            }
        }

        // Decoder and paint seam: write a whole SlotDye against an explicit
        // (slot, garment) key. Storing an all-unset dye ERASES the entry rather
        // than keeping an empty one, so an outfit that never dyed a piece and
        // one that dyed it and cleared it are the same outfit and encode to the
        // same bytes. The undo history compares whole outfits, so an empty entry
        // left lying about would read as a change nobody made.
        void SetArmourDye(std::uint32_t a_bit, const StyleRefKey& a_garment,
                          const SlotDye& a_dye) {
            if (a_bit >= kBitCount) return;
            for (auto it = armourDyes_.begin(); it != armourDyes_.end(); ++it) {
                if (it->bit == a_bit && it->garment == a_garment) {
                    if (a_dye.Any()) {
                        it->dye = a_dye;
                    } else {
                        armourDyes_.erase(it);
                    }
                    return;
                }
            }
            if (!a_dye.Any() || armourDyes_.size() >= kMaxArmourDyes) {
                return;
            }
            // Sorted on insert by (bit, mod, form) so two outfits holding the
            // same colours encode to the same bytes whatever order the player
            // clicked the swatches in. The weapon block pins the same rule for
            // the same reason; see ForEachWeaponDye's canonical-order note.
            const auto at = std::lower_bound(
                armourDyes_.begin(), armourDyes_.end(), a_bit,
                [](const ArmourDyeEntry& a_e, std::uint32_t a_b) { return a_e.bit < a_b; });
            auto pos = at;
            while (pos != armourDyes_.end() && pos->bit == a_bit &&
                   (pos->garment.modName < a_garment.modName ||
                    (pos->garment.modName == a_garment.modName &&
                     pos->garment.localFormID < a_garment.localFormID))) {
                ++pos;
            }
            armourDyes_.insert(pos, ArmourDyeEntry{ a_bit, a_garment, a_dye });
        }

        [[nodiscard]] const SlotDye& DyeFor(std::uint32_t a_bit) const {
            static const SlotDye kNone{};
            if (a_bit >= kBitCount) {
                return kNone;
            }
            const auto& key = entries_[a_bit].style;
            for (const auto& e : armourDyes_) {
                if (e.bit == a_bit && e.garment == key) {
                    return e.dye;
                }
            }
            return kNone;
        }

        // Every stored armour colour, key included, in canonical order. The
        // codec's seam: DyeFor answers "what paints the slot right now" and
        // cannot see a colour parked against a garment that is not being worn,
        // which is exactly the set a save has to carry.
        template <class F>
        void ForEachArmourDye(F&& a_fn) const {
            for (const auto& e : armourDyes_) {
                a_fn(e.bit, e.garment, e.dye);
            }
        }

        [[nodiscard]] std::size_t ArmourDyeCount() const { return armourDyes_.size(); }

        [[nodiscard]] bool AnyDye() const {
            for (const auto& e : armourDyes_) {
                if (e.dye.Any()) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] const SlotEntry& EntryFor(std::uint32_t a_bit) const {
            static const SlotEntry kNone{};
            return a_bit < kBitCount ? entries_[a_bit] : kNone;
        }

        [[nodiscard]] std::uint32_t StyleMask() const { return MaskOf(SlotEntry::Kind::kStyle); }
        [[nodiscard]] std::uint32_t HideMask() const { return MaskOf(SlotEntry::Kind::kHide); }

        template <class F>
        void ForEachStyle(F&& a_fn) const {
            for (std::uint32_t b = 0; b < kBitCount; ++b) {
                if (entries_[b].kind == SlotEntry::Kind::kStyle) {
                    a_fn(b, entries_[b].style);
                }
            }
        }

        // Weapon dimension (weapon + quiver transmog, stage 1). Parallel to
        // the armor entries_ above - same SlotEntry kind, a separate array
        // indexed by WeaponClass instead of an editor-slot bit (see
        // WeaponSlots.h for the class<->BipedAnim-slot mapping). Additive:
        // an outfit that never touches weapons leaves weaponEntries_
        // all-passthrough, so ChangedSlotCount/EditHistory below are unaffected.
        void SetWeaponStyle(WeaponClass a_class, StyleRefKey a_key,
                            WeaponHand a_hand = WeaponHand::Both) {
            WeaponEntryStorage(a_class, a_hand) =
                SlotEntry{ SlotEntry::Kind::kStyle, std::move(a_key) };
        }
        void SetWeaponHide(WeaponClass a_class,
                           WeaponHand a_hand = WeaponHand::Both) {
            WeaponEntryStorage(a_class, a_hand) =
                SlotEntry{ SlotEntry::Kind::kHide, {} };
        }
        // For Both, passthrough clears the legacy class value. For Right/Left,
        // it is an EXPLICIT override meaning "show the real weapon in this
        // hand", distinct from no override (inherit Both).
        void SetWeaponPassthrough(WeaponClass a_class,
                                  WeaponHand a_hand = WeaponHand::Both) {
            WeaponEntryStorage(a_class, a_hand) = SlotEntry{};
        }

        // Editor replacement semantics. Choosing a new Both value means "use
        // this on both hands", so stale explicit Right/Left overrides must not
        // continue winning resolution. Raw setters above deliberately retain
        // additive behavior for persistence/JSON decoding.
        void SetWeaponStyleForSelection(
            WeaponClass a_class, StyleRefKey a_key, WeaponHand a_hand) {
            ClearOverridesForBothSelection(a_class, a_hand);
            SetWeaponStyle(a_class, std::move(a_key), a_hand);
        }
        void SetWeaponPassthroughForSelection(
            WeaponClass a_class, WeaponHand a_hand) {
            ClearOverridesForBothSelection(a_class, a_hand);
            SetWeaponPassthrough(a_class, a_hand);
        }

        void ClearWeaponHandOverride(WeaponClass a_class, WeaponHand a_hand) {
            if (a_hand == WeaponHand::Right) {
                rightWeaponEntries_[Idx(a_class)].reset();
            } else if (a_hand == WeaponHand::Left) {
                leftWeaponEntries_[Idx(a_class)].reset();
            }
        }

        // Stored value. For Right/Left with no override this returns
        // passthrough; use WeaponOverrideFor to distinguish inheritance.
        [[nodiscard]] const SlotEntry& WeaponEntryFor(
            WeaponClass a_class, WeaponHand a_hand = WeaponHand::Both) const {
            if (a_hand == WeaponHand::Right) {
                const auto& value = rightWeaponEntries_[Idx(a_class)];
                return value ? *value : kPassthroughWeapon_;
            }
            if (a_hand == WeaponHand::Left) {
                const auto& value = leftWeaponEntries_[Idx(a_class)];
                return value ? *value : kPassthroughWeapon_;
            }
            return weaponEntries_[Idx(a_class)];
        }

        [[nodiscard]] const std::optional<SlotEntry>& WeaponOverrideFor(
            WeaponClass a_class, WeaponHand a_hand) const {
            static const std::optional<SlotEntry> kNoOverride;
            if (a_hand == WeaponHand::Right) {
                return rightWeaponEntries_[Idx(a_class)];
            }
            if (a_hand == WeaponHand::Left) {
                return leftWeaponEntries_[Idx(a_class)];
            }
            return kNoOverride;
        }

        [[nodiscard]] const SlotEntry& ResolvedWeaponEntryFor(
            WeaponClass a_class, WeaponHand a_hand) const {
            if (SupportsHandOverrides(a_class) && a_hand != WeaponHand::Both) {
                const auto& over = WeaponOverrideFor(a_class, a_hand);
                if (over) {
                    return *over;
                }
            }
            return WeaponEntryFor(a_class);
        }

        // ---- weapon dye -----------------------------------------------------
        //
        // Three storages per class, mirroring the weapon STYLE dimension above
        // rather than inventing a second way to say the same thing. A player
        // who has already learned that a style can differ between hands, and
        // that clearing a hand falls back to Both, gets the same two rules for
        // colour with nothing new to learn.
        //
        // ⚠ SEPARATE FROM dyes_, NOT AN EXTENSION OF IT. dyes_ is indexed by
        // armour bit and OutfitDye.cpp holds a static_assert pinning that index
        // to the 32 armour biped objects. Weapons live at biped 33 to 41, and
        // the off-hand weapon lives at biped 9, which would collide head on
        // with the shield. WeaponSlots.h opens by calling weapon slots "an
        // unrelated index space" and this keeps them one.
        // ⚠ EVERY REFERENCE THESE RETURN IS INVALIDATED BY THE NEXT MUTATION,
        // which is the one thing the array form did not require callers to know.
        // They point into a vector. Read the value, or copy it; do not hold a
        // reference across a SetWeaponDye, a Clear, or a decode.
        void SetWeaponDye(WeaponClass a_class, DyeChannelId a_channel,
                          DyeChannel a_value, WeaponHand a_hand = WeaponHand::Both) {
            WeaponDyeStorage(a_class, a_hand)
                .channels[static_cast<std::size_t>(a_channel)] = a_value;
            PruneEmptyBothDye(a_class, a_hand);
        }

        // Drop every colour stored against one class-and-hand, for the same
        // reason ClearDye exists rather than a loop of SetDye: the loop is
        // bounded by kDyeChannelCount, which is deliberately free to move.
        //
        // On Both this REMOVES the entry, because an empty Both value and no
        // Both value say the same thing. On a hand it keeps an empty entry,
        // because those two do NOT say the same thing (see below).
        void ClearWeaponDye(WeaponClass a_class, WeaponHand a_hand = WeaponHand::Both) {
            if (a_hand == WeaponHand::Both) {
                EraseWeaponDye(a_class, a_hand);
                return;
            }
            WeaponDyeStorage(a_class, a_hand) = {};
        }

        // Drop the OVERRIDE itself, so the hand goes back to inheriting Both.
        //
        // ⚠ DISTINCT FROM ClearWeaponDye ON A HAND, and for dye the difference
        // is visible on screen rather than bookkeeping. With Both red and Left
        // an explicit empty override, the off-hand weapon renders UNDYED; with
        // Both red and no Left override at all, it renders red. The container
        // has to carry that difference, which is why membership means
        // "explicitly stored" and not "non-empty".
        void ClearWeaponDyeHandOverride(WeaponClass a_class, WeaponHand a_hand) {
            if (a_hand != WeaponHand::Both) {
                EraseWeaponDye(a_class, a_hand);
            }
        }

        // Stored value. For Right/Left with no override this returns an empty
        // SlotDye; use WeaponDyeOverrideFor to distinguish inheritance.
        [[nodiscard]] const SlotDye& WeaponDyeFor(
            WeaponClass a_class, WeaponHand a_hand = WeaponHand::Both) const {
            const auto* found = FindWeaponDye(a_class, a_hand);
            return found ? *found : kNoWeaponDye_;
        }

        // nullptr means "inherits Both". A non-null pointer to an all-empty
        // SlotDye means "explicitly no colour on this hand", which is a
        // different picture. Both never has an override, by definition.
        //
        // A pointer rather than the optional the style dimension returns: with
        // sparse storage there is no stored optional to hand back a reference
        // to, and returning one by value would copy 128 bytes per call on a
        // path the editor walks every frame.
        [[nodiscard]] const SlotDye* WeaponDyeOverrideFor(
            WeaponClass a_class, WeaponHand a_hand) const {
            return a_hand == WeaponHand::Both ? nullptr
                                              : FindWeaponDye(a_class, a_hand);
        }

        // What actually paints on that hand. Identical in shape to
        // ResolvedWeaponEntryFor, including the SupportsHandOverrides gate: a
        // greatsword is one object in one biped slot, so honouring a per-hand
        // colour on it would paint whichever hand the caller happened to name.
        [[nodiscard]] const SlotDye& ResolvedWeaponDyeFor(
            WeaponClass a_class, WeaponHand a_hand) const {
            if (SupportsHandOverrides(a_class) && a_hand != WeaponHand::Both) {
                if (const auto* over = WeaponDyeOverrideFor(a_class, a_hand)) {
                    return *over;
                }
            }
            return WeaponDyeFor(a_class);
        }

        [[nodiscard]] bool AnyWeaponDye() const {
            for (const auto& e : weaponDyes_) {
                if (e.dye.Any()) {
                    return true;
                }
            }
            return false;
        }

        // ---- head-part dye, keyed by slot and part ---------------------------
        //
        // The same three rules the weapon accessors present, so a caller that
        // knows one knows both: a read of something unstored answers with a
        // shared empty, a write that leaves nothing set ERASES the entry, and
        // membership means "explicitly stored" so two outfits holding the same
        // colours encode to the same bytes.

        [[nodiscard]] const SlotDye& HeadPartDyeFor(std::uint32_t a_slot,
                                                    const StyleRefKey& a_part) const {
            if (const auto* found = FindHeadPartDye(a_slot, a_part)) {
                return *found;
            }
            return kNoHeadPartDye_;
        }

        void SetHeadPartDye(std::uint32_t a_slot, const StyleRefKey& a_part,
                            DyeChannelId a_channel, DyeChannel a_value) {
            // ⚠ AN EMPTY PART REF NAMES NOTHING AND MUST NOT BE STORED. It is
            // what CustomHeadPart returns for "this outfit says nothing about
            // that slot", so an entry keyed on one would be a colour every
            // unresolved lookup collided on.
            if (a_part.Empty()) {
                return;
            }
            auto& dye = HeadPartDyeStorage(a_slot, a_part);
            dye.channels[static_cast<std::size_t>(a_channel)] = a_value;
            if (!dye.Any()) {
                EraseHeadPartDye(a_slot, a_part);
            }
        }

        // Drop every colour stored against one part. ClearDye's argument applies
        // here word for word: a loop of SetHeadPartDye at the call sites would be
        // bounded by kDyeChannelCount, which is deliberately free to move.
        void ClearHeadPartDye(std::uint32_t a_slot, const StyleRefKey& a_part) {
            EraseHeadPartDye(a_slot, a_part);
        }

        // Everything a player would call "the dyes", taken whole from another
        // outfit: slot channels, weapon colours, head-part colours and the eye
        // tints. Exists for the profile apply's split (outfit and dyes are
        // separate checkboxes there), and it is a MEMBER because three of the
        // containers are private; a free function would need a setter per
        // container and each one is a chance to forget the next field this
        // struct grows.
        //
        // ⚠ hairTint IS DELIBERATELY NOT HERE. Hair colour is the hair's, not
        // a dye: it rides the outfit's gear half and has its own painter
        // (HairColor), so moving it with the dyes would split one value
        // between two checkboxes.
        void CopyDyeStateFrom(const Outfit& a_from) {
            // ⚠⚠ THE ARMOUR COLOURS ARE RE-KEYED ONTO THIS OUTFIT'S OWN
            // GARMENTS, not copied across. An armour dye is keyed by (slot,
            // garment) since the restyle fix, and the whole point of this
            // function is the profile apply's dyes-WITHOUT-outfit checkbox: the
            // two outfits are then wearing different pieces, and a straight copy
            // would carry keys naming the SOURCE's garments, which resolve
            // against nothing here. The player asked for these colours on what
            // they are wearing, so that is what they get.
            //
            // ⚠ PER BIT, THROUGH DyeFor, WHICH TAKES ONLY WHAT THE SOURCE COULD
            // SHOW. Colours parked in the source against garments it is not
            // wearing have no slot to land on and are not part of what the
            // player is looking at when they tick the box. Ticking BOTH boxes
            // copies the outfit too, so the garments match and this is exactly a
            // straight copy anyway.
            armourDyes_.clear();
            for (std::uint32_t b = 0; b < kBitCount; ++b) {
                const auto& dye = a_from.DyeFor(b);
                if (dye.Any()) {
                    SetArmourDye(b, entries_[b].style, dye);
                }
            }
            weaponDyes_   = a_from.weaponDyes_;
            headPartDyes_ = a_from.headPartDyes_;
            eyeTint       = a_from.eyeTint;
            scleraTint    = a_from.scleraTint;
            eyeTint2      = a_from.eyeTint2;
            eyeBlend      = a_from.eyeBlend;
        }

        [[nodiscard]] bool AnyDyeState() const {
            if (AnyDye() || AnyHeadPartDye() || !weaponDyes_.empty()) {
                return true;
            }
            return eyeTint.set || scleraTint.set || eyeTint2.set;
        }

        [[nodiscard]] bool AnyHeadPartDye() const {
            for (const auto& e : headPartDyes_) {
                if (e.dye.Any()) {
                    return true;
                }
            }
            return false;
        }

        // Every stored head-part dye in CANONICAL order: slot ascending, then
        // plugin name, then local form id.
        //
        // ⚠ THE ORDER IS LOAD BEARING FOR THE CODEC, on ForEachWeaponDye's
        // terms: two outfits holding the same colours must encode to the same
        // bytes. It cannot be a walk of a fixed index space the way the weapon
        // one is, because the slots are invented at run time, so the sort is
        // explicit and this is the only place it is written.
        template <class F>
        void ForEachHeadPartDye(F&& a_fn) const {
            std::vector<const HeadPartDyeEntry*> sorted;
            sorted.reserve(headPartDyes_.size());
            for (const auto& e : headPartDyes_) {
                sorted.push_back(&e);
            }
            std::sort(sorted.begin(), sorted.end(),
                      [](const HeadPartDyeEntry* a_l, const HeadPartDyeEntry* a_r) {
                          if (a_l->slot != a_r->slot) {
                              return a_l->slot < a_r->slot;
                          }
                          if (a_l->part.modName != a_r->part.modName) {
                              return a_l->part.modName < a_r->part.modName;
                          }
                          return a_l->part.localFormID < a_r->part.localFormID;
                      });
            for (const auto* e : sorted) {
                a_fn(e->slot, e->part, e->dye);
            }
        }

        [[nodiscard]] std::size_t HeadPartDyeCount() const { return headPartDyes_.size(); }

        // Every stored weapon dye, in CANONICAL order: class ascending, then
        // hand ascending. Never insertion order.
        //
        // ⚠ THE ORDER IS LOAD BEARING FOR THE CODEC. Two outfits that hold the
        // same colours must encode to the same bytes, or a golden byte test
        // pins whatever order the fixture happened to write in and a real save
        // written in another order reads as a different record.
        template <class F>
        void ForEachWeaponDye(F&& a_fn) const {
            for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
                const auto cls = static_cast<WeaponClass>(i);
                for (std::size_t h = 0; h < kWeaponHandCount; ++h) {
                    const auto hand = static_cast<WeaponHand>(h);
                    if (const auto* found = FindWeaponDye(cls, hand)) {
                        a_fn(cls, hand, *found);
                    }
                }
            }
        }

        template <class F>
        void ForEachWeaponStyle(F&& a_fn) const {
            for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
                if (weaponEntries_[i].kind == SlotEntry::Kind::kStyle) {
                    a_fn(static_cast<WeaponClass>(i), weaponEntries_[i].style);
                }
                if (rightWeaponEntries_[i] &&
                    rightWeaponEntries_[i]->kind == SlotEntry::Kind::kStyle) {
                    a_fn(static_cast<WeaponClass>(i), rightWeaponEntries_[i]->style);
                }
                if (leftWeaponEntries_[i] &&
                    leftWeaponEntries_[i]->kind == SlotEntry::Kind::kStyle) {
                    a_fn(static_cast<WeaponClass>(i), leftWeaponEntries_[i]->style);
                }
            }
        }

    private:
        [[nodiscard]] std::uint32_t MaskOf(SlotEntry::Kind a_kind) const {
            std::uint32_t m = 0;
            for (std::uint32_t b = 0; b < kBitCount; ++b) {
                if (entries_[b].kind == a_kind) {
                    m |= (1u << b);
                }
            }
            return m;
        }

        static constexpr std::size_t Idx(WeaponClass a_class) {
            return static_cast<std::size_t>(a_class);
        }

        void ClearOverridesForBothSelection(
            WeaponClass a_class, WeaponHand a_hand) {
            if (a_hand == WeaponHand::Both) {
                rightWeaponEntries_[Idx(a_class)].reset();
                leftWeaponEntries_[Idx(a_class)].reset();
            }
        }

        SlotEntry& WeaponEntryStorage(WeaponClass a_class, WeaponHand a_hand) {
            if (a_hand == WeaponHand::Right) {
                auto& value = rightWeaponEntries_[Idx(a_class)];
                if (!value) value.emplace();
                return *value;
            }
            if (a_hand == WeaponHand::Left) {
                auto& value = leftWeaponEntries_[Idx(a_class)];
                if (!value) value.emplace();
                return *value;
            }
            return weaponEntries_[Idx(a_class)];
        }

        // Linear, and that is the right shape here. The container is bounded at
        // kWeaponClassCount * kWeaponHandCount, 33 entries, and in practice
        // holds none or a handful. A map would cost a node allocation each and
        // a fatter empty Outfit for a search that never gets long enough to
        // matter.
        [[nodiscard]] const SlotDye* FindWeaponDye(WeaponClass a_class,
                                                   WeaponHand a_hand) const {
            for (const auto& e : weaponDyes_) {
                if (e.cls == a_class && e.hand == a_hand) {
                    return &e.dye;
                }
            }
            return nullptr;
        }

        SlotDye& WeaponDyeStorage(WeaponClass a_class, WeaponHand a_hand) {
            for (auto& e : weaponDyes_) {
                if (e.cls == a_class && e.hand == a_hand) {
                    return e.dye;
                }
            }
            weaponDyes_.push_back(WeaponDyeEntry{ a_class, a_hand, {} });
            return weaponDyes_.back().dye;
        }

        void EraseWeaponDye(WeaponClass a_class, WeaponHand a_hand) {
            for (std::size_t i = 0; i < weaponDyes_.size(); ++i) {
                if (weaponDyes_[i].cls == a_class && weaponDyes_[i].hand == a_hand) {
                    weaponDyes_.erase(weaponDyes_.begin() +
                                      static_cast<std::ptrdiff_t>(i));
                    return;
                }
            }
        }

        // Linear, on FindWeaponDye's terms and for the same reason: a head with
        // five invented slots and a nine-shape hair holds a handful of entries at
        // the very most, and most outfits hold none.
        [[nodiscard]] const SlotDye* FindHeadPartDye(std::uint32_t a_slot,
                                                     const StyleRefKey& a_part) const {
            for (const auto& e : headPartDyes_) {
                if (e.slot == a_slot && e.part == a_part) {
                    return &e.dye;
                }
            }
            return nullptr;
        }

        SlotDye& HeadPartDyeStorage(std::uint32_t a_slot, const StyleRefKey& a_part) {
            for (auto& e : headPartDyes_) {
                if (e.slot == a_slot && e.part == a_part) {
                    return e.dye;
                }
            }
            headPartDyes_.push_back(HeadPartDyeEntry{ a_slot, a_part, {} });
            return headPartDyes_.back().dye;
        }

        void EraseHeadPartDye(std::uint32_t a_slot, const StyleRefKey& a_part) {
            for (std::size_t i = 0; i < headPartDyes_.size(); ++i) {
                if (headPartDyes_[i].slot == a_slot && headPartDyes_[i].part == a_part) {
                    headPartDyes_.erase(headPartDyes_.begin() +
                                        static_cast<std::ptrdiff_t>(i));
                    return;
                }
            }
        }

        // Keeps membership meaning "explicitly stored". A Both entry that ends
        // up with nothing set says exactly what no entry says, so leaving it
        // behind would let two identical outfits differ in the container and
        // therefore on the wire.
        void PruneEmptyBothDye(WeaponClass a_class, WeaponHand a_hand) {
            if (a_hand == WeaponHand::Both &&
                !WeaponDyeFor(a_class, WeaponHand::Both).Any()) {
                EraseWeaponDye(a_class, a_hand);
            }
        }

        std::array<SlotEntry, kBitCount>         entries_{};
        // ⚠ SPARSE AND KEYED, not one per bit. See ArmourDyeEntry: the garment
        // is half the key, and that is what stops a restyle handing the old
        // piece's colour to the new one.
        std::vector<ArmourDyeEntry>              armourDyes_{};
        std::array<SlotEntry, kWeaponClassCount> weaponEntries_{};
        std::array<std::optional<SlotEntry>, kWeaponClassCount> rightWeaponEntries_{};
        std::array<std::optional<SlotEntry>, kWeaponClassCount> leftWeaponEntries_{};
        // Sparse: see WeaponDyeEntry. An outfit that dyes no weapon carries an
        // empty vector rather than 4.4 KB of zeroed SlotDye.
        std::vector<WeaponDyeEntry>                             weaponDyes_{};
        // Sparse, and unlike the weapon one it could not be anything else: the
        // slot numbers are chosen by whichever mod claimed them, so there is no
        // index space to lay an array over.
        std::vector<HeadPartDyeEntry>                           headPartDyes_{};
        inline static const SlotEntry kPassthroughWeapon_{};
        inline static const SlotDye   kNoWeaponDye_{};
        inline static const SlotDye   kNoHeadPartDye_{};
    };

    // What the render override must do this pass. Pure function of the outfit
    // and the user's blocklist - the unit-test surface.
    // ⚠ ADDING A FIELD HERE IS A TWO-SITE CHANGE. This struct is built by
    // ComputeDisplaySet (from an Outfit) and REBUILT field by field by
    // NpcResolve::WornRequiredDisplay, which starts from a fresh DisplaySet
    // rather than copying and amending. A field added to only one of them is
    // silently dropped on the follower path and nowhere else, which is the
    // hardest shape of bug to notice: the player works, one target does not.
    // Add the copy to BOTH, and add a test that reads it back through
    // WornRequiredDisplay.
    struct DisplaySet {
        std::uint32_t styleMask{ 0 };
        std::uint32_t hideMask{ 0 };
        std::uint32_t hiddenBodySkinMask{ 0 };    // re-apply race skin ARMA here
        std::uint32_t hiddenAttachmentMask{ 0 };  // cull objects[slot].partClone here
        // There is deliberately no hiddenHeadPartMask any more. The worn-mask
        // shim used to subtract exactly `hideMask & kHeadPartMask` from the
        // engine's mask, which could never clear slot 30 and so could never
        // give a hidden or displaced head back (see RenderedWornMask). The
        // shim now recomputes the mask from what is actually rendered, which
        // subsumes this field.
        HairMode      hair{ HairMode::kAuto };  // applied as one override on the worn mask
    };

    [[nodiscard]] inline DisplaySet ComputeDisplaySet(const Outfit& a_outfit,
                                                      std::uint32_t a_blocklist) {
        DisplaySet d;
        d.styleMask = a_outfit.StyleMask() & ~(a_blocklist | kNeverStyleMask);
        d.hideMask  = a_outfit.HideMask() & ~(a_blocklist | kNeverHideMask);

        d.hiddenBodySkinMask   = d.hideMask & kBodySkinMask;
        d.hiddenAttachmentMask = d.hideMask & ~kBodySkinMask;
        d.hair = a_outfit.hair;
        return d;
    }

    // Fold the body page's transient "show me this body bare" over a computed
    // DisplaySet. NULLOPT means the view is off, or this is not the actor being
    // edited, and the DisplaySet is returned exactly as it arrived. A VALUE is
    // the set of worn slots that may be stripped, AND IT IS ALLOWED TO BE ZERO.
    //
    // ⚠⚠ ZERO IS A REAL ANSWER, NOT AN ABSENCE, AND CONFLATING THE TWO IS THE
    // BUG THIS SIGNATURE EXISTS TO PREVENT. The first cut took a plain mask and
    // returned early on 0, which reads as "nothing to do" and is wrong in the
    // one case that matters most: a character wearing NO real armour, whose
    // whole look is therefore our own styles. Measured in the field log
    // 2026-08-16 - the flag armed, the worn pass ran 4 ms later, and the mask
    // function returned at its coverage test without a word, so a fully dressed
    // transmog stayed on a character with nothing underneath it. The two
    // questions are separate: whether OUR geometry draws needs no measurement
    // at all, while what of the REAL gear may be hidden needs all of it.
    //
    // ⚠⚠ a_hideable MUST BE MEASURED WORN COVERAGE, NEVER A CONSTANT. A hide bit
    // on a slot with no worn gear still reaches CullNodes, which sets kHidden on
    // objects[slot].partClone - and on a bare character slot 30 holds the RACE
    // SKIN's own armature (the warning on StagedCoverageOf). "Hide everything"
    // is therefore a headless character rather than a bare one. The caller
    // passes RealWorn::coverage, which is exactly the gear there is to take off.
    //
    // ⚠ THE STYLES GO TOO, and that is the difference between this and Hide All.
    // Hide All answers "wear none of your real gear" and leaves the transmog on;
    // this answers "show me the body", so our own geometry cannot survive it
    // either. styleMask is cleared rather than masked by a_hideable, because a
    // style may sit on a slot the actor wears nothing on and would then be the
    // one garment left standing.
    //
    // ⚠ hair IS UNTOUCHED. It is not worn gear and a bald preview answers a
    // question nobody asked; the mode the player chose stays theirs.
    //
    // ⚠ NOT AN EDIT, and nothing here writes an Outfit. Staging a hide would
    // light the Apply button, land on the undo timeline and persist the moment
    // an auto-apply fired, so a look at the body would cost the player their
    // outfit. The flag lives on the session for exactly as long as the page is
    // on screen (OutfitSession::SetBareBodyPreview).
    [[nodiscard]] inline DisplaySet BareBodyDisplay(
        DisplaySet a_display, std::optional<std::uint32_t> a_hideable) {
        if (!a_hideable) {
            return a_display;
        }
        // ⚠ THE STYLES GO FIRST AND UNCONDITIONALLY, before the mask is even
        // looked at. They are ours, they need no measurement, and on a bare
        // character they are the entire outfit.
        a_display.styleMask = 0;
        a_display.hideMask |= *a_hideable;
        // Re-derived through the same two lines ComputeDisplaySet uses: a hide
        // bit that does not fall out into its submask hides nothing at all.
        a_display.hiddenBodySkinMask   = a_display.hideMask & kBodySkinMask;
        a_display.hiddenAttachmentMask = a_display.hideMask & ~kBodySkinMask;
        return a_display;
    }

    // Does the outfit's BODY dimension differ? Separate from ChangedSlotCount
    // on purpose - see the note on EditHistory::SlotsDiffer.
    [[nodiscard]] inline bool BodyDiffers(const Outfit& a_base, const Outfit& a_staged) {
        return a_base.obodyPreset != a_staged.obodyPreset ||
               a_base.customBodyPresetId != a_staged.customBodyPresetId ||
               a_base.orefit != a_staged.orefit ||
               a_base.pushUp != a_staged.pushUp;
    }

    // Does the outfit's HAIR override differ? Its own predicate for the same
    // reason BodyDiffers is: hair is not a slot, so it must stay out of
    // ChangedSlotCount and off the lore-mode Apply bill, but a change to it
    // still has to reach the screen and the undo timeline.
    //
    // Hair needs a full equipment rebuild, not the body-only path. The lever is
    // engine 24220, which runs inside the rebuild orchestrator 24221 (+0x1DF),
    // so nothing short of that re-reads the worn mask the override rides on.
    [[nodiscard]] inline bool HairDiffers(const Outfit& a_base, const Outfit& a_staged) {
        return a_base.hair != a_staged.hair;
    }

    // Does the outfit's hair COLOUR differ? Its own predicate for the same
    // reason BodyDiffers and HairDiffers are: a colour is not a slot, so it must
    // stay out of ChangedSlotCount and off the lore-mode Apply bill, while still
    // reaching the screen and the undo timeline.
    //
    // Two DISABLED tints compare equal whatever their leftover channels hold. A
    // cleared row means "leave my hair alone", and if the stale RGB underneath
    // counted as a difference, clearing one outfit and selecting another would
    // register a phantom edit and re-refresh the character for nothing.
    // Does the outfit's hair STYLE differ? Its own predicate for the same
    // reason the tint has one: a head part is not a slot, so it must stay out
    // of ChangedSlotCount and off the lore-mode Apply bill while still
    // reaching the screen, the Apply gate and the undo timeline.
    [[nodiscard]] inline bool HairStyleDiffers(const Outfit& a_base, const Outfit& a_staged) {
        return a_base.hairStyle != a_staged.hairStyle;
    }

    // Do the outfit's EYE, BROW or FACIAL HAIR types differ (OS-161, OS-196)?
    // One predicate for the set, because they are applied together, gated
    // together and hidden together; nothing in the editor or the push ever
    // asks about one alone.
    //
    // ⚠ THIS IS A NEW DIMENSION FOR EditorGate::StillDirty, whose own comment
    // records that a forgotten dimension has shipped four times. Adding the
    // fields without adding this is exactly that bug, and it presents as an
    // Apply button that will not light up.
    //
    // ⚠⚠ AND FACIAL HAIR IS THE FIFTH TIME IT NEARLY DID. This term is a
    // comparison, not a switch, so widening HeadPart::Kind said NOTHING about
    // it: a beard-only edit would have been classified as no change at all,
    // and the pick would never have committed.
    [[nodiscard]] inline bool HeadPartsDiffer(const Outfit& a_base, const Outfit& a_staged) {
        // ⚠⚠ THE INVENTED SLOTS RIDE THIS PREDICATE RATHER THAN GETTING A NEW
        // ONE, AND THAT IS DELIBERATE. ClassifyStagedUpdate's own header records
        // that it has been "the forgotten half of a feature" five times, and
        // every one of those was a new dimension needing a new term in six
        // lists. A horn is a head part; folding it into the head-part term means
        // there is no sixth list to forget, no new parameter, and no caller that
        // keeps compiling while silently reading false.
        //
        // The comparison is order-insensitive by construction: SetCustomHeadPart
        // keeps one entry per slot and erases a cleared one, so two outfits
        // naming the same parts hold the same entries. It is NOT sorted, so two
        // outfits that reached the same set by different routes could compare
        // unequal; that costs one redundant push and never a missed one, which
        // is the safe side of this particular wrong answer.
        return a_base.eyes != a_staged.eyes || a_base.brows != a_staged.brows ||
               a_base.facialHair != a_staged.facialHair ||
               a_base.customHeadParts != a_staged.customHeadParts;
    }

    [[nodiscard]] inline bool HairTintDiffers(const Outfit& a_base, const Outfit& a_staged) {
        const auto& x = a_base.hairTint;
        const auto& y = a_staged.hairTint;
        if (x.set != y.set) {
            return true;
        }
        if (!x.set) {
            return false;  // both disabled: the channels underneath are dead data
        }
        return x.r != y.r || x.g != y.g || x.b != y.b;
    }

    // Does the outfit's EYE colour differ? The same set-then-bytes rule as the
    // hair tint above, for the same reason: two disabled tints compare equal
    // whatever dead bytes sit underneath.
    //
    // ⚠ THIS PREDICATE EXISTS BECAUSE ITS ABSENCE WAS THE FIFTH FORGOTTEN
    // DIMENSION TO SHIP. The Eyes row landed with a codec field, a dye tile and
    // a paint pass, and no diff term anywhere: an eye-colour-only edit
    // classified kNone in ClassifyStagedUpdate and reached the screen only when
    // the eye PART changed, because HeadPartsDiffer fired for it (field
    // 2026-08-13: "it only changes when we switch eye"). Every list that names
    // HairTintDiffers has to name this too.
    //
    // ⚠ ONE PREDICATE FOR THE EYE PAIR, iris and sclera together, on the terms
    // HeadPartsDiffer covers its trio: they are painted by one pass, staged on
    // adjacent rows, and nothing that reacts to a change ever asks about one
    // alone. The BILL counts them separately (two picks, two looks); this
    // answers "did the eye change at all".
    [[nodiscard]] inline bool EyeTintDiffers(const Outfit& a_base, const Outfit& a_staged) {
        const auto same = [](const HairTint& x, const HairTint& y) {
            if (x.set != y.set) {
                return false;
            }
            if (!x.set) {
                return true;
            }
            return x.r == y.r && x.g == y.g && x.b == y.b;
        };
        // ⚠ THE SECOND COLOUR IS THE THIRD MEMBER OF THE SAME PAIR-WISE
        // ANSWER, not a dimension of its own. It is painted by the same pass,
        // staged on an adjacent stripe of the same tile, and nothing that
        // reacts to an eye change ever asks about it alone. Folding it in here
        // is what keeps RefreshGate.h still and all six lists untouched.
        if (!same(a_base.eyeTint, a_staged.eyeTint) ||
            !same(a_base.scleraTint, a_staged.scleraTint) ||
            !same(a_base.eyeTint2, a_staged.eyeTint2)) {
            return true;
        }
        // ⚠ THE BLEND RIDES IN HERE RATHER THAN IN A NINTH DIMENSION, which is
        // what keeps all six lists still. eyeBlend changes what the same RGB
        // renders as, so it is an eye change by every definition this predicate
        // serves, and a pass that did not see it would leave a picked blend off
        // the screen until the colour itself moved.
        //
        // ⚠ DEAD DATA WHEN NEITHER HALF IS PAINTED, the rule the tints
        // themselves follow two lines up: with both tints cleared there is no
        // texture being mixed, so a leftover byte must not register an edit and
        // re-refresh the character for nothing.
        const bool paints = a_base.eyeTint.set || a_staged.eyeTint.set ||
                            a_base.scleraTint.set || a_staged.scleraTint.set ||
                            a_base.eyeTint2.set || a_staged.eyeTint2.set;
        return paints && a_base.eyeBlend != a_staged.eyeBlend;
    }

    // Does either outfit's DYE differ? Its own predicate for exactly the reason
    // BodyDiffers, HairDiffers and HairTintDiffers each have one: a dye is not a
    // slot, so it must stay out of ChangedSlotCount and off the lore-mode Apply
    // bill, while still reaching the screen and the undo timeline.
    //
    // Two DISABLED channels compare equal whatever their leftover bytes hold,
    // the same rule HairTintDiffers uses. A cleared channel means "leave this
    // alone", and if the stale RGB underneath counted as a difference, clearing
    // a dye and selecting another outfit would register a phantom edit and
    // re-refresh the character for nothing.
    // Do two channel arrays differ? ⚠ ONE FUNCTION FOR EVERY DYE DIMENSION, and
    // it is one because it was three: the armour walk, the weapon walk and now
    // the head-part walk all need the disabled-channel rule above, and three
    // copies of a four-line comparison is how one of them ends up comparing
    // leftover bytes under a cleared flag.
    // two-readers-of-one-answer-drift-in-the-gap.
    [[nodiscard]] inline bool DyeChannelsDiffer(
        const std::array<DyeChannel, kDyeChannelCount>& a_x,
        const std::array<DyeChannel, kDyeChannelCount>& a_y) {
        for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
            if (a_x[c].set != a_y[c].set) {
                return true;
            }
            if (a_x[c].set && !(a_x[c] == a_y[c])) {
                return true;
            }
        }
        return false;
    }

    // Does any head-part colour differ between two outfits?
    //
    // ⚠⚠ BOTH DIRECTIONS, AND THAT IS NOT BELT AND BRACES. The container is
    // sparse and keyed on a slot-and-part pair neither outfit has to hold, so a
    // one-way walk answers "no difference" for a colour the OTHER side alone
    // carries. Walking base only misses a newly dyed horn, which is the loud
    // failure; walking staged only misses a horn dye the player just cleared,
    // which leaves Apply greyed out with a change on screen.
    [[nodiscard]] inline bool HeadPartDyeDiffers(const Outfit& a_base,
                                                 const Outfit& a_staged) {
        bool differs = false;
        const auto compare = [&differs](const Outfit& a_from, const Outfit& a_against) {
            a_from.ForEachHeadPartDye([&](std::uint32_t a_slot, const StyleRefKey& a_part,
                                         const SlotDye& a_dye) {
                if (DyeChannelsDiffer(a_dye.channels,
                                      a_against.HeadPartDyeFor(a_slot, a_part).channels)) {
                    differs = true;
                }
            });
        };
        compare(a_base, a_staged);
        compare(a_staged, a_base);
        return differs;
    }

    [[nodiscard]] inline bool DyeDiffers(const Outfit& a_base, const Outfit& a_staged) {
        for (std::uint32_t bit = 0; bit < Outfit::kBitCount; ++bit) {
            if (DyeChannelsDiffer(a_base.DyeFor(bit).channels,
                                  a_staged.DyeFor(bit).channels)) {
                return true;
            }
        }
        // ⚠ THE HEAD-PART WALK IS PART OF THIS FUNCTION, on exactly the terms
        // the weapon walk below states: without it a horn-only edit leaves Apply
        // greyed out with no way to commit it. The counter half is in
        // ChangedDyeChannelCount and the two are extended together or the pair
        // fails in opposite directions at once.
        if (HeadPartDyeDiffers(a_base, a_staged)) {
            return true;
        }
        // ⚠ THE WEAPON WALK IS PART OF THIS FUNCTION, NOT A FOLLOW-UP. Without
        // it a weapon-only edit leaves Apply greyed out with no way to commit
        // it, which is the loud half of the fail-open door the weapon dye spec
        // names. The silent half is in ChangedDyeChannelCount below, and the
        // two have to be extended together or the pair fails in opposite
        // directions at once.
        //
        // ⚠ NO SupportsHandOverrides GATE HERE, deliberately, and the asymmetry
        // with the counter below is the point. This asks "is there anything to
        // commit", and an override on a class with no hand dimension is still
        // stored state the editor has to be able to clear. The counter asks
        // "what did this PAINT", where an unreachable value must earn nothing.
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto cls = static_cast<WeaponClass>(i);
            for (std::size_t h = 0; h < kWeaponHandCount; ++h) {
                const auto hand = static_cast<WeaponHand>(h);
                if (DyeChannelsDiffer(a_base.WeaponDyeFor(cls, hand).channels,
                                      a_staged.WeaponDyeFor(cls, hand).channels)) {
                    return true;
                }
            }
        }
        return false;
    }

    // How many dye channels committing a_staged over a_base actually PAINTS.
    //
    // The producer behind the "channelsDyed" deed, which had none at all until
    // 2026-08-02: DyeWorld::Gather read the counter, eleven tests wrote it, and
    // nothing in the shipped build ever did. Measured on the shipped rules with
    // the counter stuck at 0, a level 100 character with every skill at 100
    // reaches 282 of 318 colours; the other 36 are gated on this number and
    // were unreachable by any means.
    //
    // ⚠ A DIFF, NOT A COUNT OF WHAT IS SET, and that is the whole defence
    // against double counting. The dye pane is a staging area with undo and
    // redo running through it, so cost is charged when the outfit is
    // COMMITTED, exactly as ChangedSlotCount and PendingGoldCost already do it
    // for slots. Re-committing an unchanged outfit diffs to zero. Undo and redo
    // never commit anything at all; they walk the staged value and the caller
    // prices it against the same committed baseline it always did.
    //
    // ⚠ CLEARING A CHANNEL COUNTS ZERO, deliberately. DyeDiffers reports it,
    // because it is an edit that has to reach the screen and the Apply gate,
    // but nothing was painted, and this is the number a deed is earned from.
    // Unlocks are sticky, so a colour granted off a miscount is granted
    // forever, and undercounting only ever means the player dyes one more
    // garment.
    [[nodiscard]] inline std::uint32_t ChangedDyeChannelCount(
        const Outfit& a_base, const Outfit& a_staged) {
        std::uint32_t n = 0;
        for (std::uint32_t bit = 0; bit < Outfit::kBitCount; ++bit) {
            const auto& x = a_base.DyeFor(bit).channels;
            const auto& y = a_staged.DyeFor(bit).channels;
            for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                if (y[c].set && !SameDyeColour(x[c], y[c])) {
                    ++n;
                }
            }
        }
        // ⚠ THE SILENT HALF OF THE FAIL-OPEN DOOR. Left out, weapon dye is free
        // and earns no channelsDyed, nothing anywhere says so, and the damage
        // is permanent because unlocks are add only. That shape has been
        // produced on this branch more than once: something loses a rule, no
        // rule means free, and there is no log line for it.
        //
        // ⚠ THE STORAGES ARE WALKED, NOT THE RESOLVED VALUES. Resolving would
        // read the Both colour once per hand and bill a single swatch click
        // three times. The per-hand override is its own painted channel and the
        // Both value is one, which is exactly how ForEachWeaponStyle already
        // enumerates the style dimension.
        //
        // ⚠ AN UNREACHABLE OVERRIDE EARNS NOTHING. A per-hand colour on a class
        // SupportsHandOverrides refuses can never render, and the deed unlocks
        // 36 colours and is sticky, so counting it would let a hand-edited
        // outfits.json farm the palette with values nothing will ever paint.
        // Under-counting only ever means the player dyes one more weapon;
        // over-counting cannot be undone. DyeDiffers deliberately does NOT
        // share this gate, so such a value still reports as an edit and can be
        // cleared.
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto cls = static_cast<WeaponClass>(i);
            for (std::size_t h = 0; h < kWeaponHandCount; ++h) {
                const auto hand = static_cast<WeaponHand>(h);
                if (hand != WeaponHand::Both && !SupportsHandOverrides(cls)) {
                    continue;
                }
                const auto& x = a_base.WeaponDyeFor(cls, hand).channels;
                const auto& y = a_staged.WeaponDyeFor(cls, hand).channels;
                for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                    if (y[c].set && !SameDyeColour(x[c], y[c])) {
                        ++n;
                    }
                }
            }
        }
        // ⚠ THE HEAD-PART HALF, and it is here because the weapon note above
        // says what happens when it is not: no rule means free, and unlocks are
        // add only, so a horn dyed for nothing is a permanent hole in the deed.
        //
        // ⚠ ONLY THE STAGED SIDE IS WALKED, unlike HeadPartDyeDiffers, and the
        // asymmetry is the same one the weapon walk has: this counts what the
        // commit PAINTS, and a colour only the base holds is one the player is
        // clearing. Clearing counts zero, exactly as the armour walk's own note
        // says.
        a_staged.ForEachHeadPartDye([&](std::uint32_t a_slot, const StyleRefKey& a_part,
                                        const SlotDye& a_dye) {
            const auto& was = a_base.HeadPartDyeFor(a_slot, a_part).channels;
            for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                if (a_dye.channels[c].set && !SameDyeColour(was[c], a_dye.channels[c])) {
                    ++n;
                }
            }
        });
        return n;
    }

    // True when the outfit says anything at all about the body.
    [[nodiscard]] inline bool AnyBodyEntry(const Outfit& a_outfit) {
        return !a_outfit.obodyPreset.empty() ||
               !a_outfit.customBodyPresetId.empty() ||
               a_outfit.orefit != ORefitMode::kDefault ||
               a_outfit.pushUp != PushUpMode::kNone;
    }

    // Resolve the stored ORefit setting against the torso the player actually
    // SEES. Explicit On and Off win. Default is the transmog-aware Auto mode:
    // styled torso slots count as clothed, hidden ones count as empty, and
    // passthrough slots inherit their real worn state. If the outfit does not
    // touch any OBody-relevant torso slot, leave OBody in charge of real gear.
    // ⚠⚠ A STYLE THAT WILL NOT RENDER IS NOT A VISIBLE TORSO. With
    // [General] bRequireWornForStyles on, CanApplyStyleBit (SlotMask.h) refuses
    // a style whose coverage has no real gear under it, so a player who
    // unequips everything is standing there bare. Auto mode counted the styled
    // bits as visible anyway and left ORefit forced ON, which is the state it
    // had computed while the gear was still equipped: reported 2026-08-29 as
    // "ORefit does not update when you unequip your gear; it uses the ORefit
    // status for your equipped outfit pieces even though your character now has
    // nothing equipped".
    //
    // ⚠ THE TEST IS AT MASK GRANULARITY AND CanApplyStyleBit's IS AT COVERAGE
    // GRANULARITY, so the two can still disagree for a style whose ARMA covers
    // slots the outfit does not name: a robe staged on the chest reaches the
    // sleeves, and real gear on only the sleeves satisfies the renderer while
    // this sees nothing. Auto's job is to answer "is a torso visible", not to
    // re-derive the render decision, and erring towards ForceOff here matches
    // what the player is looking at. Passing the styles' real coverage in is
    // the exact fix and needs a caller that has it.
    [[nodiscard]] inline ORefitMode ResolveAutoORefit(
        ORefitMode a_configured, std::uint32_t a_styleMask,
        std::uint32_t a_hideMask, std::uint32_t a_realWornMask,
        bool a_globalORefitEnabled = true, bool a_requireWornForStyles = false) {
        if (a_configured != ORefitMode::kDefault) {
            return a_configured;
        }
        const auto changed = (a_styleMask | a_hideMask) & kORefitTorsoMask;
        if (changed == 0 || !a_globalORefitEnabled) {
            return ORefitMode::kDefault;
        }
        // The styled bits only count while something real is under them.
        const auto stylesShow =
            (!a_requireWornForStyles || (a_realWornMask & a_styleMask) != 0)
                ? a_styleMask
                : 0u;
        const auto visible = (stylesShow |
                              (a_realWornMask & ~(a_styleMask | a_hideMask))) &
                             kORefitTorsoMask;
        return visible != 0 ? ORefitMode::kForceOn : ORefitMode::kForceOff;
    }

    // True when the outfit styles or hides ANY weapon class - hiding counts.
    // The predicate behind OutfitSession's weapon fast-path flag; it lives here,
    // pure, so the part deciding whether the feature runs at all is unit-tested.
    [[nodiscard]] inline bool AnyWeaponEntry(const Outfit& a_outfit) {
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto wc = static_cast<WeaponClass>(i);
            if (a_outfit.WeaponEntryFor(wc).kind != SlotEntry::Kind::kPassthrough ||
                a_outfit.WeaponOverrideFor(wc, WeaponHand::Right).has_value() ||
                a_outfit.WeaponOverrideFor(wc, WeaponHand::Left).has_value()) {
                return true;
            }
        }
        return false;
    }

    // Build the player's transient mannequin look while editing a follower.
    // Styled slots are copied verbatim. Every other hideable, unblocked armor
    // slot is hidden so the player's real outfit cannot show through the
    // follower preview. Body settings are retained deliberately so the player
    // mannequin can preview the follower's staged OBody preset and ORefit
    // shape. This is transient only: OutfitSession clears playerMannequin_ on
    // target switch, Apply, discard, and close, then refreshes the player's
    // real saved outfit/body.
    //
    // The returned Outfit is render-only. OutfitSession never inserts it into
    // either library, so it cannot affect persistence or saved outfit data.
    [[nodiscard]] inline Outfit MakeMannequinPreview(
        const Outfit& a_follower, std::uint32_t a_blocklist) {
        Outfit out = a_follower;
        const std::uint32_t cannotHide = kNeverHideMask | a_blocklist;
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            if (((cannotHide >> bit) & 1u) == 0 &&
                out.EntryFor(bit).kind == SlotEntry::Kind::kPassthrough) {
                out.SetHide(bit);
            }
        }
        return out;
    }

    // Build the render-only source for the player mannequin while editing a
    // follower. Captured equipped gear is the visual base even for a mutable
    // saved outfit: passthrough means "show the follower's real gear", not
    // "show the player's real gear". Explicit staged styles/hides overlay that
    // base. The mutable saved outfit itself remains untouched, so a freshly
    // created all-passthrough outfit is still empty in persistence.
    [[nodiscard]] inline Outfit ComposeMannequinSource(
        bool a_actualGear, const Outfit& a_equipped, const Outfit& a_staged) {
        if (a_actualGear) {
            return a_equipped;
        }
        Outfit out = a_equipped;
        out.name   = a_staged.name;
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            const auto& entry = a_staged.EntryFor(bit);
            if (entry.kind == SlotEntry::Kind::kStyle) {
                out.SetStyle(bit, entry.style);
            } else if (entry.kind == SlotEntry::Kind::kHide) {
                out.SetHide(bit);
            }
        }
        for (std::size_t i = 0; i < kWeaponClassCount; ++i) {
            const auto cls   = static_cast<WeaponClass>(i);
            const auto& entry = a_staged.WeaponEntryFor(cls);
            if (entry.kind != SlotEntry::Kind::kPassthrough) {
                // A new Both choice replaces the equipped per-hand baseline;
                // absent staged hand overrides now inherit that new choice.
                out.ClearWeaponHandOverride(cls, WeaponHand::Right);
                out.ClearWeaponHandOverride(cls, WeaponHand::Left);
            }
            if (entry.kind == SlotEntry::Kind::kStyle) {
                out.SetWeaponStyle(cls, entry.style);
            } else if (entry.kind == SlotEntry::Kind::kHide) {
                out.SetWeaponHide(cls);
            }
            for (const auto hand : { WeaponHand::Right, WeaponHand::Left }) {
                if (const auto& over = a_staged.WeaponOverrideFor(cls, hand);
                    over) {
                    if (over->kind == SlotEntry::Kind::kStyle) {
                        out.SetWeaponStyle(cls, over->style, hand);
                    } else if (over->kind == SlotEntry::Kind::kHide) {
                        out.SetWeaponHide(cls, hand);
                    } else {
                        out.SetWeaponPassthrough(cls, hand);
                    }
                }
            }
        }
        // Body settings belong to the staged follower outfit. Preserve them so
        // the render-only player mannequin previews the requested body along
        // with the follower's composed equipment.
        out.obodyPreset       = a_staged.obodyPreset;
        out.customBodyPresetId = a_staged.customBodyPresetId;
        out.orefit             = a_staged.orefit;
        return out;
    }

    // True when the two slot entries render differently (drives the Apply
    // cost: a slot that returns to its baseline value costs nothing).
    [[nodiscard]] inline bool SlotDiffers(const SlotEntry& a, const SlotEntry& b) {
        return a.kind != b.kind ||
               (a.kind == SlotEntry::Kind::kStyle && !(a.style == b.style));
    }

    // How many slots the staged outfit changes vs. the committed baseline -
    // armor bits then weapon classes, same SlotDiffers predicate for both.
    // The lore-mode Apply charges per changed slot, so this must return to 0
    // whenever the staged outfit is edited back to its baseline.
    [[nodiscard]] inline std::uint32_t ChangedSlotCount(const Outfit& a_base,
                                                        const Outfit& a_staged) {
        std::uint32_t n = 0;
        for (std::uint32_t b = 0; b < kBitCount; ++b) {
            if (SlotDiffers(a_base.EntryFor(b), a_staged.EntryFor(b))) {
                ++n;
            }
        }
        for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
            const auto wc = static_cast<WeaponClass>(c);
            if (SlotDiffers(a_base.WeaponEntryFor(wc), a_staged.WeaponEntryFor(wc))) {
                ++n;
            }
            for (const auto hand : { WeaponHand::Right, WeaponHand::Left }) {
                const auto& before = a_base.WeaponOverrideFor(wc, hand);
                const auto& after  = a_staged.WeaponOverrideFor(wc, hand);
                if (before.has_value() != after.has_value() ||
                    (before && after && SlotDiffers(*before, *after))) {
                    ++n;
                }
            }
        }
        return n;
    }

    // How many changed slots are on the BILL, which is not the same question as
    // how many changed.
    //
    // ⚠ TWO COUNTS, DELIBERATELY, AND MERGING THEM BREAKS ONE OF THEM.
    // ChangedSlotCount above answers "is there an edit to commit" and feeds
    // EditorGate::StillDirty; this answers "what does it cost". Taking a style
    // off or revealing hidden gear must stay a CHANGE, or Apply greys out and
    // the removal can never be committed, which is exactly the failure
    // StillDirty's own comment says has shipped four times. It must not be a
    // CHARGE, because an outfit a player cannot afford would otherwise be one
    // they cannot afford to undo (user 2026-08-07).
    //
    // Free exactly when the staged slot returns to real gear. Hiding is not
    // revealing: a hidden slot is a deliberate look and is billed like a style.
    [[nodiscard]] inline std::uint32_t BillableSlotCount(const Outfit& a_base,
                                                         const Outfit& a_staged) {
        const auto billable = [](const SlotEntry& a_before, const SlotEntry& a_after) {
            return SlotDiffers(a_before, a_after) &&
                   a_after.kind != SlotEntry::Kind::kPassthrough;
        };
        std::uint32_t n = 0;
        for (std::uint32_t b = 0; b < kBitCount; ++b) {
            n += billable(a_base.EntryFor(b), a_staged.EntryFor(b)) ? 1u : 0u;
        }
        for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
            const auto wc = static_cast<WeaponClass>(c);
            n += billable(a_base.WeaponEntryFor(wc), a_staged.WeaponEntryFor(wc)) ? 1u : 0u;
            for (const auto hand : { WeaponHand::Right, WeaponHand::Left }) {
                const auto& before = a_base.WeaponOverrideFor(wc, hand);
                const auto& after  = a_staged.WeaponOverrideFor(wc, hand);
                // Dropping a hand override entirely is a revert too: the hand
                // goes back to whatever the class says, which is the same
                // "stop overriding" the passthrough test catches above.
                if (!after.has_value()) {
                    continue;
                }
                if (!before.has_value() || SlotDiffers(*before, *after)) {
                    n += after->kind != SlotEntry::Kind::kPassthrough ? 1u : 0u;
                }
            }
        }
        return n;
    }

    // How many APPEARANCE dimensions differ: the ones that stage with zero
    // changed slots and were therefore free until 2026-08-07.
    //
    // ⚠ THE LIST IS THE SAME ONE EditorGate::StillDirty NAMES, and it has to
    // stay that way. That predicate exists because a dimension with no changed
    // slot gets its dirty flag cleared the same frame Push sets it; this exists
    // because the same dimension gets billed nothing. A dimension added to one
    // and not the other is either uncommittable or free, and both have shipped.
    //
    // ⚠ COUNTED, NOT A BOOL. A player who changed their body and their eyes has
    // done two things, and folding them into one unit would make the second
    // free, which is the shape of the bug this whole function answers.
    //
    // Body and its ORefit mode are ONE dimension on purpose: BodyDiffers answers
    // for both, they are set on the same page, and splitting them would charge
    // twice for one visit. Eyes and brows are TWO, because they are two picks
    // off two lists even though one predicate answers for the pair.
    // ⚠ AND PUTTING ONE BACK IS FREE. The bill is for CHANGING a look, not for
    // abandoning one: reverting a dimension to the character's own is the
    // player giving something up, and charging for it means an outfit they
    // cannot afford is also one they cannot afford to undo. That is a trap
    // rather than an economy (user 2026-08-07).
    //
    // ⚠ ASKED OF THE STAGED SIDE, NOT OF THE DIFFERENCE, and that asymmetry is
    // the whole mechanism. A diff is symmetric and cannot tell "picked one up"
    // from "put one down"; the direction lives in whether the value being moved
    // TO names anything. Free exactly when it names nothing.
    //
    // ⚠ SWAPPING ONE NAMED LOOK FOR ANOTHER STILL COSTS, which is what stops
    // this becoming a free route to a different look. Were the test "did it get
    // simpler", clear-then-pick would be two free steps where picking directly
    // costs one, and every player would learn the detour.
    [[nodiscard]] inline std::uint32_t ChangedLookCount(const Outfit& a_base,
                                                        const Outfit& a_staged) {
        // "Leave it alone" for each dimension: the value that means the
        // character wears their own. Body includes ORefit, since one predicate
        // and one page own both, so changing only the refit mode away from
        // default is still a change.
        const bool bodyBare = a_staged.obodyPreset.empty() &&
                              a_staged.customBodyPresetId.empty() &&
                              a_staged.orefit == ORefitMode::kDefault;
        std::uint32_t n = 0;
        n += (BodyDiffers(a_base, a_staged) && !bodyBare) ? 1u : 0u;
        n += (HairDiffers(a_base, a_staged) && a_staged.hair != HairMode::kAuto) ? 1u : 0u;
        n += (HairStyleDiffers(a_base, a_staged) && !a_staged.hairStyle.Empty()) ? 1u : 0u;
        n += (HairTintDiffers(a_base, a_staged) && a_staged.hairTint.set) ? 1u : 0u;
        // The eye colours are billed exactly as the hair colour above: a look
        // when the staged side names one, free when the edit is putting her
        // own colour back. Iris and sclera count SEPARATELY, like eyes and
        // brows below: two picks off two rows, so folding them into
        // EyeTintDiffers (which answers for the pair) would make the second
        // free, or bill a sclera edit as an iris one.
        const auto tintDiffers = [](const HairTint& x, const HairTint& y) {
            if (x.set != y.set) {
                return true;
            }
            return x.set && (x.r != y.r || x.g != y.g || x.b != y.b);
        };
        n += (tintDiffers(a_base.eyeTint, a_staged.eyeTint) && a_staged.eyeTint.set) ? 1u
                                                                                     : 0u;
        n += (tintDiffers(a_base.scleraTint, a_staged.scleraTint) &&
              a_staged.scleraTint.set)
                 ? 1u
                 : 0u;
        // ⚠⚠ AND THE SECOND EYE COLOUR, ON THE SAME TERMS AS THE OTHER TWO.
        // This function's own header says a dimension added to one list and not
        // the other is either uncommittable or free, and BOTH have shipped.
        // EyeTintDiffers learned eyeTint2 and this did not, so a second-colour
        // edit reached the screen, enabled Apply, and cost nothing.
        n += (tintDiffers(a_base.eyeTint2, a_staged.eyeTint2) && a_staged.eyeTint2.set)
                 ? 1u
                 : 0u;
        n += (a_base.eyes != a_staged.eyes && !a_staged.eyes.Empty()) ? 1u : 0u;
        n += (a_base.brows != a_staged.brows && !a_staged.brows.Empty()) ? 1u : 0u;
        n += (a_base.facialHair != a_staged.facialHair && !a_staged.facialHair.Empty())
                 ? 1u
                 : 0u;
        return n;
    }

    // The price of PENDING edits, not merely a structural difference between
    // two outfit values. The explicit pending bit is important in the editor's
    // tab-switch frame, where the frame-start library snapshot still names the
    // old outfit while staging already names the newly selected one.
    //
    // ⚠ WAS PendingGoldCost, AND THE RENAME IS THE POINT. It is not gold's any
    // more: gold and the Seamstone's charge both come through here, so the bill
    // has one shape and switching cost mode changes what you pay WITH rather
    // than what you are paying FOR. Two functions would be two multiplies that
    // have to agree.
    //
    // ⚠ Slots, dye channels AND looks, all required rather than defaulted. A
    // default of zero on any term would let a new call site silently price that
    // dimension at nothing, which is the fail-open direction and is exactly how
    // six dimensions came to be free in the first place.
    //
    // u64 cannot overflow here even against a hand-edited INI: the RATES come
    // from the INI but the COUNTS do not. Slots are bounded by kBitCount,
    // channels by the slots times kDyeChannelCount and looks by six, so the
    // largest product any term can reach is around 1.1e12.
    [[nodiscard]] inline std::uint64_t PendingCost(bool          a_hasPendingEdits,
                                                   bool          a_charging,
                                                   std::uint32_t a_changedSlots,
                                                   std::uint32_t a_costPerSlot,
                                                   std::uint32_t a_changedDyes,
                                                   std::uint32_t a_costPerDye,
                                                   std::uint32_t a_changedLooks,
                                                   std::uint32_t a_costPerLook) {
        if (!a_hasPendingEdits || !a_charging) {
            return 0;
        }
        return std::uint64_t(a_changedSlots) * a_costPerSlot +
               std::uint64_t(a_changedDyes) * a_costPerDye +
               std::uint64_t(a_changedLooks) * a_costPerLook;
    }

    // Empty one slot: its entry AND the colours stored against it, as ONE
    // mutation.
    //
    // A dye belongs to the PIECE, not to the slot, the way it does in ESO. The
    // moment a slot stops holding a piece its colours have nothing left to
    // paint, and leaving them behind stranded them: the dye pane drew the slot
    // as an orphan stripe, a slashed swatch that could not be picked, sitting on
    // a slot the user had just cleared. It read as the clear having failed.
    //
    // ⚠ ONE FUNCTION, NOT TWO CALLS AT EACH SITE, because undo keys on
    // whole-outfit snapshots taken at Push(). Two mutations either side of a
    // Push() would be two history entries, and one undo would then hand the
    // garment back without its colour, or the colour back without its garment.
    //
    // ⚠ NOT FOR A HIDE, and not for a within-slot SWAP. Hiding is reversible -
    // SetHide keeps the style it covers and ToggleHideSlot hands it back - so a
    // hide/show round trip has to be lossless, and the geometry is still there
    // regardless (a hide sets NiAVObject kHidden on the part clone rather than
    // removing it, so the shape snapshot still sees it). A swap never empties
    // the slot at all, and losing the colour on every restyle would be a worse
    // bug than the orphan this fixes.
    inline void ClearSlot(Outfit& a_outfit, std::uint32_t a_bit) {
        a_outfit.SetPassthrough(a_bit);
        a_outfit.ClearDye(a_bit);
    }

    // What a slot's clone group actually paints: the owner's own colours, with
    // any channel the owner never set filled from the other bits in the group,
    // lowest first.
    //
    // ⚠ THE OWNER'S OWN CHOICE ALWAYS WINS. There is one mesh, so there can be
    // one answer, and the answer has to be the one with a tile on screen behind
    // it. Filling only the owner's UNSET channels is what keeps a colour the
    // user put on a slot that has since lost its tile - the current save has one
    // - painting the garment instead of vanishing.
    //
    // Pure, and used on a COPY at paint time, so a garment renders correctly
    // whether or not the collapse below has ever been committed.
    [[nodiscard]] inline SlotDye ResolvedSlotDye(const Outfit&         a_outfit,
                                                 const DyeCloneOwners& a_owners,
                                                 std::uint32_t         a_bit) {
        if (a_bit >= kBitCount) {
            return {};
        }
        const auto owner = a_owners[a_bit];
        SlotDye    out   = a_outfit.DyeFor(owner);
        for (std::uint32_t b = owner + 1; b < kBitCount; ++b) {
            if (a_owners[b] != owner) {
                continue;
            }
            const auto& donor = a_outfit.DyeFor(b);
            for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                if (!out.channels[c].set && donor.channels[c].set) {
                    out.channels[c] = donor.channels[c];
                }
            }
        }
        return out;
    }

    // Fold every clone group's colours onto its owner and empty the rest.
    // Returns whether anything moved.
    //
    // ⚠ THIS MOVES DATA THAT IS ALREADY IN THE PLAYER'S SAVE, and moving rather
    // than dropping is the user's call, made on the direction of the mistake: a
    // colour moved is recoverable because they can still clear it, and a colour
    // cleared is gone. The alternative silently undoes a choice they made and
    // are looking at on their character.
    //
    // ⚠ THE LOSING BIT IS EMPTIED, including any channel that could not move
    // because the owner had already been given one. That bit has no tile any
    // more, so anything left on it is unreachable, unclearable and paints
    // nothing - a ghost, and the reason to run this at all rather than to lean
    // on ResolvedSlotDye alone. Leave it behind and clearing the surviving tile
    // just lets the ghost resolve back into view.
    //
    // Idempotent: a second run finds the losing bits already empty.
    inline bool CollapseCloneDyes(Outfit& a_outfit, const DyeCloneOwners& a_owners) {
        bool changed = false;
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            const auto owner = a_owners[bit];
            if (owner == bit || !a_outfit.DyeFor(bit).Any()) {
                continue;
            }
            // Re-read per bit rather than once up front: a third slot in the
            // same group must merge against the owner as it now stands.
            const auto merged = ResolvedSlotDye(a_outfit, a_owners, bit);
            for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                a_outfit.SetDye(owner, static_cast<DyeChannelId>(c), merged.channels[c]);
            }
            a_outfit.ClearDye(bit);
            changed = true;
        }
        return changed;
    }

    // Reversible hide toggle for the editor's per-slot Hide/Show button.
    // A hidden SlotEntry carries the style it covers, so the round trip stays
    // reversible through editor close/open, history snapshots, JSON, and
    // follower co-saves. Legacy hides have an empty key and reveal real gear.
    inline void ToggleHideSlot(Outfit& a_staged, std::uint32_t a_bit) {
        if (a_bit >= kBitCount) {
            return;
        }
        if (a_staged.EntryFor(a_bit).kind == SlotEntry::Kind::kHide) {
            const StyleRefKey covered = a_staged.EntryFor(a_bit).style;
            if (!covered.Empty()) {
                a_staged.SetStyle(a_bit, covered);
            } else {
                // ⚠ SetPassthrough, deliberately NOT ClearSlot. Showing a legacy
                // hide takes nothing away - the slot goes back to the real gear
                // it was already showing before the hide - so the dye stored
                // against that gear has to survive the round trip.
                a_staged.SetPassthrough(a_bit);
            }
        } else {
            a_staged.SetHide(a_bit);
        }
    }

    // The editor's bulk Hide All / Show All, over an explicit mask of the slots
    // the row list is willing to touch (its own table, less the never-hide
    // slots and the INI blocklist). Pure, so the round trip is pinned by test
    // rather than by looking at the button.
    //
    // ⚠⚠ SHOW ALL IS THE HIDE'S INVERSE AND NOTHING ELSE. It called ClearSlot
    // until 2026-08-27, which empties the entry and takes the dye with it, so
    // a Hide All followed by a Show All wiped every slot the outfit had dressed
    // and no Show could bring one back ("it obliterates the slots so they can't
    // come back"). The per-slot button had always been reversible; only the
    // bulk pair was not. Both arms go through the same primitives that button
    // uses, so there is one answer to "what does hiding do" rather than two.
    inline bool AllSlotsHidden(const Outfit& a_staged, std::uint32_t a_bits) {
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            if (!((a_bits >> bit) & 1u)) {
                continue;
            }
            if (a_staged.EntryFor(bit).kind != SlotEntry::Kind::kHide) {
                return false;
            }
        }
        return true;
    }

    inline void HideSlots(Outfit& a_staged, std::uint32_t a_bits) {
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            if ((a_bits >> bit) & 1u) {
                a_staged.SetHide(bit);  // keeps the style it covers
            }
        }
    }

    // Only hidden slots move. A slot the mask names but that is not hidden is
    // left where it is: ToggleHideSlot would HIDE it, which would turn a second
    // Show All press into a Hide All on whatever the first one revealed.
    inline void ShowSlots(Outfit& a_staged, std::uint32_t a_bits) {
        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            if (((a_bits >> bit) & 1u) &&
                a_staged.EntryFor(bit).kind == SlotEntry::Kind::kHide) {
                ToggleHideSlot(a_staged, bit);  // hands the covered style back
            }
        }
    }

    // Bounded undo/redo timeline for the editor's staged outfit (OS-21). A
    // linear list of snapshots with a cursor: Reset seeds the state the editor
    // opened on, Record appends the state reached by each REAL edit (style
    // pick, Remove/Show, real-gear, Random), and Undo/Redo walk the cursor.
    // Transient hover-preview is deliberately never recorded - only committed
    // edits reach Record. Slot-only comparison (names are edited on a separate
    // path and must not create history), so re-picking the selected style is a
    // no-op. Pure logic - unit-tested like ToggleHideSlot/ChangedSlotCount.
    class EditHistory {
    public:
        static constexpr std::size_t kCap = 20;  // bounded so a long session can't grow unbounded

        void Reset(const Outfit& a_initial) {
            states_.assign(1, a_initial);
            cursor_ = 0;
        }

        void Record(const Outfit& a_next) {
            if (states_.empty()) {  // never Reset: treat this as the seed
                states_.push_back(a_next);
                cursor_ = 0;
                return;
            }
            if (!SlotsDiffer(states_[cursor_], a_next)) {
                return;  // idempotent edit (e.g. re-picking the selected style)
            }
            states_.resize(cursor_ + 1);  // a new edit drops any redo tail
            states_.push_back(a_next);
            cursor_ = states_.size() - 1;
            if (states_.size() > kCap) {
                const std::size_t drop = states_.size() - kCap;
                states_.erase(states_.begin(),
                              states_.begin() + static_cast<std::ptrdiff_t>(drop));
                cursor_ -= drop;
            }
        }

        [[nodiscard]] bool CanUndo() const { return cursor_ > 0; }
        [[nodiscard]] bool CanRedo() const { return cursor_ + 1 < states_.size(); }

        // Walk the cursor and return the now-current snapshot. A no-op (returns
        // the current snapshot unchanged) when there is nothing to undo/redo.
        const Outfit& Undo() {
            if (CanUndo()) {
                --cursor_;
            }
            return Current();
        }
        const Outfit& Redo() {
            if (CanRedo()) {
                ++cursor_;
            }
            return Current();
        }

        [[nodiscard]] const Outfit& Current() const {
            static const Outfit kEmpty;
            return states_.empty() ? kEmpty : states_[cursor_];
        }
        [[nodiscard]] std::size_t Size() const { return states_.size(); }

    private:
        // Two outfits differ as edits iff any slot (armor bit or weapon
        // class) renders differently - structurally the same predicate
        // ChangedSlotCount uses, so history and Apply-cost agree.
        //
        // ⚠ BODY, HAIR, HAIR COLOUR, HAIR STYLE, AND DYE ARE DELIBERATELY
        // *NOT* IN ChangedSlotCount, BUT *ARE* HERE. ChangedSlotCount drives
        // the lore-mode Apply gold cost, priced per changed SLOT. A body
        // preset, a hair override, a hair colour, a hair style, and an armour
        // dye are not slots, so none of them may be billed as one. But
        // undo/redo keys on this predicate, so leaving them out entirely makes
        // such an edit invisible to history: change it, press undo, nothing
        // happens. The two callers want different questions answered, so they
        // get different predicates.
        //
        // Hair was added to Outfit without being added here, and it reproduced
        // that exact failure one field over. Hair colour repeated the pattern,
        // then hair STYLE repeated it a third time - it shipped its refresh
        // and dirty seams while this list went untouched, and a style-only
        // edit was invisible to undo until review caught it during the dye
        // work. Anything new that renders but is not a slot belongs in this
        // list, and the test beside the dye-undo one exists so the omission
        // fails loudly next time.
        //
        // ⚠ EYES AND BROWS MADE IT FOUR, and the test did not catch this one:
        // a field report did (2026-08-08). OS-161 shipped them with a codec
        // field, a push and a dirty term, and this list went untouched again,
        // so an eyes-only edit could not be undone. The lesson the note above
        // draws is right and was simply not applied; the regression test for
        // this field now sits beside the hair-style one.
        static bool SlotsDiffer(const Outfit& a, const Outfit& b) {
            return ChangedSlotCount(a, b) != 0 || BodyDiffers(a, b) ||
                   HairDiffers(a, b) || HairTintDiffers(a, b) ||
                   HairStyleDiffers(a, b) || DyeDiffers(a, b) ||
                   HeadPartsDiffer(a, b) || EyeTintDiffers(a, b);
        }

        std::vector<Outfit> states_;
        std::size_t         cursor_{ 0 };
    };

    class OutfitLibrary {
    public:
        [[nodiscard]] std::size_t Count() const { return outfits_.size(); }
        [[nodiscard]] int         ActiveIndex() const { return active_; }

        // Invalidated by Create()/Remove(); do not hold across calls.
        [[nodiscard]] Outfit*       At(std::size_t i) { return i < outfits_.size() ? &outfits_[i] : nullptr; }
        [[nodiscard]] const Outfit* At(std::size_t i) const { return i < outfits_.size() ? &outfits_[i] : nullptr; }

        [[nodiscard]] const Outfit* Active() const {
            return active_ >= 0 ? At(static_cast<std::size_t>(active_)) : nullptr;
        }

        // Returns the new index, or -1 if the saved-outfit cap is reached.
        int Create(std::string a_name) {
            if (outfits_.size() >= kMaxOutfits) {
                return -1;
            }
            Outfit o;
            o.name = std::move(a_name);
            outfits_.push_back(std::move(o));
            return static_cast<int>(outfits_.size()) - 1;
        }

        void Rename(std::size_t i, std::string a_name) {
            if (auto* o = At(i)) {
                o->name = std::move(a_name);
            }
        }

        void Remove(std::size_t i) {
            if (i >= outfits_.size()) {
                return;
            }
            outfits_.erase(outfits_.begin() + static_cast<std::ptrdiff_t>(i));
            if (active_ == static_cast<int>(i)) {
                active_ = -1;
            } else if (active_ > static_cast<int>(i)) {
                --active_;
            }
        }

        // Remove one saved outfit and keep the library on the nearest remaining
        // saved outfit when the removed entry was active. If it was the final
        // entry, the library becomes inactive: Equipped gear is the permanent
        // baseline, so an empty saved-outfit library is valid.
        //
        // Returns the resulting active index (-1 == Equipped gear). Invalid
        // indices are harmless and leave the current selection untouched.
        int RemoveAndSelectNeighbor(std::size_t i) {
            if (i >= outfits_.size()) {
                return active_;
            }
            const bool removedActive = active_ == static_cast<int>(i);
            Remove(i);
            if (removedActive && !outfits_.empty()) {
                Activate(std::min(i, outfits_.size() - 1));
            }
            return active_;
        }

        void Activate(std::size_t i) {
            if (i < outfits_.size()) {
                active_ = static_cast<int>(i);
            }
        }
        void Deactivate() { active_ = -1; }

        // Quick-switch treats immutable Equipped gear as logical tab zero,
        // then every saved outfit in library order. This is the same model as
        // controller tab cycling inside the editor, including a valid
        // Equipped-only state when the saved library is empty.
        int CycleIncludingEquipped(bool a_next) {
            const int next =
                OutfitTabs::Cycle(active_, static_cast<int>(outfits_.size()), a_next);
            if (next < 0) {
                Deactivate();
            } else {
                Activate(static_cast<std::size_t>(next));
            }
            return active_;
        }

        // Move the outfit at a_from so it sits at index a_to; the active
        // outfit follows its entry.
        void Move(std::size_t a_from, std::size_t a_to) {
            if (a_from >= outfits_.size() || a_to >= outfits_.size() || a_from == a_to) {
                return;
            }
            auto moved = std::move(outfits_[a_from]);
            outfits_.erase(outfits_.begin() + static_cast<std::ptrdiff_t>(a_from));
            outfits_.insert(outfits_.begin() + static_cast<std::ptrdiff_t>(a_to),
                            std::move(moved));
            const int from = static_cast<int>(a_from), to = static_cast<int>(a_to);
            if (active_ == from) {
                active_ = to;
            } else if (from < to && active_ > from && active_ <= to) {
                --active_;
            } else if (to < from && active_ >= to && active_ < from) {
                ++active_;
            }
        }
        void Clear() {
            outfits_.clear();
            active_ = -1;
        }

        [[nodiscard]] std::vector<Outfit>&       All() { return outfits_; }
        [[nodiscard]] const std::vector<Outfit>& All() const { return outfits_; }

    private:
        std::vector<Outfit> outfits_;
        int                 active_{ -1 };
    };

}  // namespace OS
