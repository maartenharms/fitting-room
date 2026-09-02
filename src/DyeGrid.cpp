#include "DyeGrid.h"

#include "HeadPartSlotPlan.h"  // IsCustomType: which slot numbers a mod invented

#include <array>
#include <utility>

namespace OS::DyeGrid {

    std::vector<DyeTile> Build(const std::vector<ShapeInfo>& a_shapes,
                               const Outfit& a_staged, bool a_advancedEye) {
        std::vector<DyeTile> out;

        // ⚠ ONE GARMENT ON TWO BIPED SLOTS IS ONE TILE. The engine attaches a
        // multi-slot armature once per covered slot, so without this the grid
        // offers a tile per copy and the copies can be given two colours for one
        // helmet. See DyeCloneOwnersFrom for why source equality is the rule and
        // why a name match is not.
        //
        // ⚠ THE OTHER HALF IS IN OutfitDye::Repaint, and neither half is any use
        // alone. Collapsing the tile without resolving the paint the same way
        // leaves the hidden bit painting its own clone with no control on screen
        // to fix it, which is worse than the defect.
        const auto owners = DyeCloneOwnersFromShapes(a_shapes);

        // ⚠ FIRST, AND ALWAYS. Head first, matching how the slot list already
        // orders head parts, and in a fixed position so it does not move as the
        // character changes gear.
        //
        // There is no qualifying test, unlike every tile below. The control this
        // replaced had none either: it was a checkbox and a picker that were
        // always available, including for an "(away)" follower with no loaded
        // actor to read. A has-hair gate here would be a behaviour change
        // smuggled in under a presentation change.
        //
        // ONE stripe, because hairTint is a single RGB rather than up to eight
        // channels, and no material, because hair is painted by facegen rather
        // than by a material swap.
        {
            DyeTile hair;
            // ⚠ THE TILE IS A GROUP AND ITS TARGET IS THE COLOUR'S. Everything
            // about the hair lives here now: stripe 0 is the hair's own colour
            // and the stripes after it are the non-strand shapes the hair
            // contributes, appended below. The tile keeps kHair as its target so
            // every existing reader that asks "is this the hair tile" still gets
            // the right answer; what changed is that its stripes no longer all
            // paint the same thing, which is why each one carries its own.
            hair.target    = DyeTarget::kHair;
            hair.headGroup = HeadGroup::kHair;
            // ⚠ bit, weaponClass, weaponHand and handSplit are LEFT AT THEIR
            // DEFAULTS AND READ NOWHERE. kHair is keyed by nothing (see
            // DyeTarget), and bit's default 0 is a real addressable slot rather
            // than a spare value, so anything that read it would land on that
            // slot's garment. Keeping hair out of the bit-walking paths -
            // CollapseCloneDyes and the clone grouping - is the whole point.
            DyeStripe only;
            only.target  = DyeTarget::kHair;
            only.channel = static_cast<DyeChannelId>(0);
            only.dyeable = true;
            // ⚠ NO LABEL. This field carries the BSGeometry's own name, joined,
            // and is shown in the tooltip; hair has no worn shape to take one
            // from. The tile's NAME is a separate string the editor draws from
            // $FR_Dye_Hair, so putting that key here would only push an
            // untranslated key into a tooltip that has nothing to say.
            only.colour.set = a_staged.hairTint.set;
            only.colour.r   = a_staged.hairTint.r;
            only.colour.g   = a_staged.hairTint.g;
            only.colour.b   = a_staged.hairTint.b;
            // ⚠ colour.palette and colour.player STAY EMPTY. Both DyeMaterial
            // blocks describe a material swap on worn geometry, and hair is
            // painted by facegen, so there is no shader property at the end of
            // that path to receive a sheen or a gloss.
            hair.stripes.push_back(std::move(only));
            // shapeCount stays 0: hair contributes no WORN shape, so it adds
            // nothing to the counts the bulk paths size their loops from. The
            // head-part shapes appended below do not change that either, for the
            // same reason: they are not worn gear.
            out.push_back(std::move(hair));
        }
        // Where the hair group sits, so its shapes can be appended to it. Always
        // 0, because the hair tile is emitted first and unconditionally; held in a
        // name rather than written as a literal so the next tile inserted ahead of
        // it breaks nothing.
        const std::size_t hairTileAt = out.size() - 1;

        // ---- the EYE tile, second and always ----------------------------
        //
        // Everything the hair tile's note above says applies here word for
        // word: one stripe because Outfit::eyeTint is a single RGB, no
        // qualifying test because the control has to be reachable on an
        // "(away)" follower with no loaded actor, and shapeCount left at zero
        // because an eye contributes no worn shape to the bulk paths.
        //
        // ⚠ SECOND RATHER THAN FIRST, and only because hair was here already.
        // Nothing may assume tile 0 is armour, and now nothing may assume tile
        // 1 is either; a walk that wants worn gear has to read the target.
        {
            DyeTile eyes;
            eyes.target = DyeTarget::kEyes;
            // ⚠ TWO STRIPES BY DEFAULT: the iris on channel 0 and the sclera on
            // channel 1, because they are one eye and separate tiles read as
            // separate body parts (user 2026-08-13). The channel is the whole
            // address; every kEyes write site switches on it. A THIRD stripe,
            // the eye's second colour on channel 2, appears only under
            // a_advancedEye, which is off unless a player turns it on.
            //
            // ⚠ THE SECOND COLOUR IS APPENDED AT 2 RATHER THAN INSERTED AT 1,
            // so the sclera keeps the channel it has always had. The channel is
            // persisted in nothing today, but it IS the address the selection
            // ring holds across a frame, and renumbering it would move a live
            // selection onto a different colour. Appending is also what lets
            // the stripe come and go with the setting without disturbing the
            // two below it.
            //
            // ⚠ THE COUNT NEVER CHANGES WITH THE WORN PART, only with the
            // setting. Whether an eye mesh can show two colours side by side is
            // a property of that mesh, and a tile that grew and shrank as the
            // player browsed eye sets would reflow the grid under the pointer.
            // A setting is read once and does not move while browsing.
            DyeStripe iris;
            iris.target     = DyeTarget::kEyes;
            iris.channel    = static_cast<DyeChannelId>(0);
            iris.dyeable    = true;
            iris.colour.set = a_staged.eyeTint.set;
            iris.colour.r   = a_staged.eyeTint.r;
            iris.colour.g   = a_staged.eyeTint.g;
            iris.colour.b   = a_staged.eyeTint.b;
            eyes.stripes.push_back(std::move(iris));
            DyeStripe sclera;
            sclera.target     = DyeTarget::kEyes;
            sclera.channel    = static_cast<DyeChannelId>(1);
            // ⚠ THE TWO TAKE TURNS, AND THE SECOND COLOUR WINS. They share one
            // shader register, which is the precedence DyeTexture already
            // ships and PaintEyeTint already logs; without the cross the only
            // sign would be a log line nobody reads and a white that quietly
            // stopped changing. Clearing the second colour hands the register
            // back and the sclera paints again from the bytes it kept.
            //
            // ⚠⚠ AND THE CROSS IS GATED ON THE SETTING TOO, NOT ON THE STORED
            // COLOUR ALONE. An outfit that was given a second colour while the
            // experiment was on keeps those bytes forever; keying the cross on
            // them alone would leave the white refusing on an install where the
            // second colour cannot paint at all, which is a dead control
            // explaining itself with a reason that is no longer true.
            //
            // ⚠⚠ AND IT ASKS THE SAME QUESTION THE PAINTER ASKS, iris included.
            // PaintEyeTint drops a second colour that has no iris to sit beside
            // and then paints the sclera normally, so a cross keyed on the
            // second colour alone would refuse the one stripe that actually
            // renders, and explain it with a reason that is false at the moment
            // it is shown. The grid and the painter answer this together or
            // they lie to each other.
            const bool secondPaints =
                a_advancedEye && a_staged.eyeTint2.set && a_staged.eyeTint.set;
            sclera.dyeable          = !secondPaints;
            sclera.reason = secondPaints ? DyeSkipReason::kEyeSecondColour
                                         : DyeSkipReason::kNone;
            sclera.colour.set = a_staged.scleraTint.set;
            sclera.colour.r   = a_staged.scleraTint.r;
            sclera.colour.g   = a_staged.scleraTint.g;
            sclera.colour.b   = a_staged.scleraTint.b;
            eyes.stripes.push_back(std::move(sclera));
            if (a_advancedEye) {
                DyeStripe second;
                second.target  = DyeTarget::kEyes;
                second.channel = static_cast<DyeChannelId>(2);
                // ⚠ IT NEEDS AN IRIS COLOUR TO SIT BESIDE, and the cross is the
                // only way a player learns that. The paint splits the texture
                // in half and gives this the far side, so with the iris unset
                // the near side has no colour of its own and half an eye would
                // paint. PaintEyeTint refuses it for the same reason and says
                // so in the log; without this the player sets a colour and
                // nothing happens, which is the report that produced this line.
                second.dyeable    = a_staged.eyeTint.set;
                second.reason     = a_staged.eyeTint.set ? DyeSkipReason::kNone
                                                         : DyeSkipReason::kEyeNeedsIris;
                second.colour.set = a_staged.eyeTint2.set;
                second.colour.r   = a_staged.eyeTint2.r;
                second.colour.g   = a_staged.eyeTint2.g;
                second.colour.b   = a_staged.eyeTint2.b;
                eyes.stripes.push_back(std::move(second));
            }
            out.push_back(std::move(eyes));
        }

        // ---- head parts: the hair's own shapes, and everything invented -----
        //
        // ⚠⚠ TWO GROUPS, NOT A TILE PER PART, and the field is why. The key is
        // still per (slot, part), because that is what a colour belongs to and
        // what survives a load order change; what changed is the PRESENTATION.
        // One tile per part put nine tiles on screen for one hair and the pane
        // stopped being readable ("there are so many groups now it's very poor
        // UX", user 2026-08-17). So the hair's shapes join the hair's own colour
        // on one tile, and every invented slot's parts share a second one.
        //
        // ⚠ THE STRIPE CARRIES THE KEY. A group's stripes belong to different
        // parts, so the tile cannot hold one, and the editor builds its selection
        // from the stripe it clicked. Two stripes both on channel 0 are distinct
        // because their part refs are.
        //
        // ⚠ ORDER IS THE WALK'S ORDER, which is stable: the hair slot first, then
        // the discovered slots ascending, and within a slot the parent part before
        // its extras. Fixed by the load order and the records, so a stripe does
        // not move under the pointer between frames.
        //
        // ⚠ A REFUSED SHAPE KEEPS ITS STRIPE, which is the user's other call from
        // the same day: a shape that cannot take a dye is listed with its reason
        // rather than hidden, so a strand reads as a rule and not as a gap. What
        // gets no stripe is a part that contributed no geometry at all, because a
        // control over nothing on screen is a dead swatch.
        //
        // ⚠⚠ EXCEPT kHairOwnColour, AND THAT IS A REVERSAL OF THE CALL ABOVE BY
        // THE SAME USER, 2026-08-31: "why is it that we still have many extra
        // slots that we can't click for hairs, that inherit the first hair slot.
        // if we can't click on them then she should just be hidden". A modern
        // hair contributes a dozen strand shapes and every one of them landed
        // here, so the hair tile drew one clickable colour followed by ten that
        // could not be clicked at all.
        //
        // ⚠ WHAT CHANGED IS THE MARK, and that is why the earlier call stopped
        // holding. The 2026-08-17 argument was that a refused stripe teaches
        // something: it carried a CROSS, so it read as "refused, and here is
        // why". The cross came off every stripe earlier the same day, on this
        // user's call, and without it a strand row is a swatch that looks
        // exactly as clickable as the one beside it and is not. A rule nobody
        // can see is not teaching anything, it is just occupying the row.
        //
        // ⚠ ONLY THIS REASON, AND ONLY ON A HEAD PART. kHairOwnColour is set
        // nowhere else (OutfitDye guards it on target == kHeadPart), and it is
        // the one reason that means "already painted, by a control two rows up"
        // rather than "cannot be painted". Every other refusal still earns its
        // stripe: a glow map, a parallax shape and a hood's strands under kHair
        // all still say so, because none of them is answered by another swatch
        // the player can see.
        {
            DyeTile extras;
            extras.target    = DyeTarget::kHeadPart;
            extras.headGroup = HeadGroup::kExtras;

            for (const auto& sh : a_shapes) {
                if (sh.target != DyeTarget::kHeadPart) {
                    continue;
                }
                // ⚠⚠ THE SHARED PREDICATE, AND THIS WALK WAS THE ONE PLACE NOT
                // ASKING IT. DyeShapeEarnsAStripe says what a SHAPE IS, its own
                // note insists there is one copy for exactly that reason, and the
                // armour and weapon walks have both called it since it was
                // written. This walk never did, on the 2026-08-17 rule that a
                // refused head shape is listed rather than hidden, and the two
                // rules are about different questions: "can this take a dye" is
                // not "is this a thing the player is dyeing at all".
                //
                // A HAIRLINE is what found it (user 2026-08-31, "sometimes
                // hairline is in there which is something we can't click on or
                // change and should be hidden"). MEASURED off the field log:
                // every hairline in the session reads
                // `feature HairTint blood=true dyeable=false`, so the decal test
                // claims it before SkipReasonFor ever runs and the kHair to
                // kHairOwnColour conversion below never sees it. The decal test
                // is CORRECT and stays: kDecal plus kDynamicDecal is geometry the
                // engine paints onto something else, which is what a hairline is
                // as much as what weapon blood is. ⚠ Only the reason's NAME is
                // weapon flavoured; read its note in Outfit.h before trusting it
                // to mean blood.
                if (!DyeShapeEarnsAStripe(sh.reason)) {
                    continue;
                }
                // The reversal above. A strand the hair colour already paints
                // gets no swatch of its own.
                //
                // ⚠ SEPARATE FROM THE TEST ABOVE ON PURPOSE. That one hides a
                // shape the player is not dyeing at all; this one hides a shape
                // that IS painted, by a control two rows up. Folding
                // kHairOwnColour into DyeShapeEarnsAStripe would put a head-part
                // presentation call inside a rule the armour and weapon walks
                // read, where it means nothing.
                if (sh.reason == DyeSkipReason::kHairOwnColour) {
                    continue;
                }
                DyeStripe row;
                row.target   = DyeTarget::kHeadPart;
                row.headSlot = sh.headSlot;
                row.headPart = sh.headPart;
                row.channel  = ChannelForShapeIndex(sh.index);
                row.label    = sh.name;
                row.dyeable  = sh.dyeable;
                row.reason   = sh.reason;
                row.reflective = sh.reflective;
                row.colour   = a_staged.HeadPartDyeFor(sh.headSlot, sh.headPart)
                                 .channels[static_cast<std::size_t>(row.channel)];
                // The hair slot's shapes join the hair's own colour; everything
                // else is an extra. ⚠ THE HAIR SLOT NUMBER IS THE ENGINE'S 3 and
                // it is not spelled here: HeadPartSlotPlan::IsCustomType is the
                // one place that says which numbers a mod invented, so a slot the
                // engine names can only be the hair (the walk visits no other).
                auto& tile = HeadPartSlotPlan::IsCustomType(sh.headSlot)
                                 ? extras
                                 : out[hairTileAt];
                // ⚠⚠ shapeCount STAYS ZERO, AND IT IS LOAD BEARING RATHER THAN
                // LAZY. It counts WORN shapes, and the bulk paste and scheme
                // apply are disabled on a zero. That refusal is wanted here.
                //
                // It is a bug waiting to happen otherwise. SetStagedDyeChannel's
                // kHair arm has NO channel guard, and its own note says why that
                // is safe: the two loops that write all eight channels are paste
                // and scheme apply, and both are disabled on a zero shapeCount,
                // "which the hair tile's is". Counting shapes here would re-arm
                // exactly that, and the symptom would be a pasted scheme clearing
                // the player's hair colour with its last write.
                //
                // ⚠ THE FLASH DOES NOT READ IT ON A HEAD STRIPE. FlashChannel
                // walks the BIPED and head geometry hangs off the face node, so
                // the editor arms a head stripe on the stripe's own part instead
                // (OutfitDye::FlashHeadPart), and shapeCount can stay at its
                // paste-guarding zero without costing the horn its flash.
                tile.stripes.push_back(std::move(row));
            }

            if (!extras.stripes.empty()) {
                out.push_back(std::move(extras));
            }
        }

        for (std::uint32_t bit = 0; bit < kBitCount; ++bit) {
            if (owners[bit] != bit) {
                continue;  // a clone of a lower bit's garment; it has that tile
            }
            std::array<DyeStripe, kDyeChannelCount> rows;
            for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                rows[c].target  = DyeTarget::kArmour;
                rows[c].channel = static_cast<DyeChannelId>(c);
            }
            // Whether channel c got a shape that is actually part of the
            // GARMENT, as opposed to the player's own body riding on the same
            // slot.
            std::array<bool, kDyeChannelCount> anyGarment{};

            std::size_t shapeCount = 0;
            for (const auto& s : a_shapes) {
                // The target test comes FIRST. slotBit means nothing on a
                // weapon shape and an off-hand weapon's happens to be 9, so
                // without this a sword in the left hand adds its blade to the
                // shield's tile.
                if (s.target != DyeTarget::kArmour || s.slotBit != bit) {
                    continue;
                }
                // ⚠ COUNTED AND INDEXED BEFORE ANY FILTER. OutfitDye::Repaint
                // assigns these same indices while walking every shape,
                // undyeable ones included, so the editor's channels only line
                // up with the renderer's while this stays unconditional.
                ++shapeCount;
                const auto c = static_cast<std::size_t>(ChannelForShapeIndex(s.index));

                if (!DyeShapeEarnsAStripe(s.reason)) {
                    continue;  // counted and indexed, never labelled
                }
                anyGarment[c] = true;
                if (!s.name.empty()) {
                    if (!rows[c].label.empty()) {
                        rows[c].label += ", ";
                    }
                    rows[c].label += s.name;
                }
                rows[c].dyeable    = rows[c].dyeable || s.dyeable;
                rows[c].reflective = rows[c].reflective || s.reflective;
                if (!s.dyeable && rows[c].reason == DyeSkipReason::kNone) {
                    rows[c].reason = s.reason;
                }
            }

            DyeTile tile;
            tile.target     = DyeTarget::kArmour;
            tile.bit        = bit;
            tile.shapeCount = shapeCount;

            const auto live = LiveChannelCount(shapeCount);
            // Resolved, not DyeFor(bit): a colour sitting on a bit that just
            // lost its tile still belongs to this garment and has to be visible
            // on the tile that survived, or it paints the mesh with no swatch
            // anywhere showing it. CollapseCloneDyes normally makes this an
            // identity; it is here so the grid is right even on an outfit
            // nothing has collapsed yet.
            const auto slot = ResolvedSlotDye(a_staged, owners, bit);
            // ⚠ LIVE CHANNELS ONLY. A stored colour whose piece is not on the
            // garment currently worn gets no stripe, on the user's call after
            // seeing it: switching a slot's style left crossed-out squares
            // sitting among the real ones, and a tile should show the piece you
            // are looking at rather than a record of pieces you are not.
            //
            // ⚠ THIS HIDES, IT DOES NOT DESTROY, and the difference is the whole
            // reason it is safe. The colour stays in the outfit, so switching
            // back to the garment that had that piece brings it back. Reset
            // piece still loops every channel, so there is a way to purge one.
            //
            // Dropping the stored-only case also means a slot whose garment is
            // gone produces no tile at all, which is the same statement.
            for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                if (c >= live || !anyGarment[c]) {
                    continue;
                }
                rows[c].colour = slot.channels[c];
                tile.stripes.push_back(std::move(rows[c]));
            }

            if (!tile.stripes.empty()) {
                out.push_back(std::move(tile));
            }
        }

        // ---- weapons, after the armour and in class-then-hand order ---------
        //
        // Matches the slots panel, where Weapons sits below Regular, and it is
        // the same canonical order Outfit::ForEachWeaponDye uses so the grid and
        // the co-save agree on what "first" means.
        //
        // ⚠ NO CLONE GROUPING HERE, and it is absent because it cannot apply
        // rather than because it was skipped. Grouping exists for ONE armature
        // the engine cloned onto several covered slots; a weapon is one object
        // in one slot. The two classes that share a biped slot, greatsword with
        // battleaxe and arrows with bolts, are different classes and only one of
        // each pair can be equipped, so they can never both want the same tile.
        for (std::size_t ci = 0; ci < kWeaponClassCount; ++ci) {
            const auto cls = static_cast<WeaponClass>(ci);
            for (std::size_t hi = 0; hi < kWeaponHandCount; ++hi) {
                const auto hand = static_cast<WeaponHand>(hi);

                std::array<DyeStripe, kDyeChannelCount> rows;
                for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                    rows[c].target  = DyeTarget::kWeapon;
                    rows[c].channel = static_cast<DyeChannelId>(c);
                }
                std::array<bool, kDyeChannelCount> anyGarment{};

                std::size_t shapeCount = 0;
                for (const auto& s : a_shapes) {
                    if (s.target != DyeTarget::kWeapon || s.weaponClass != cls ||
                        s.weaponHand != hand) {
                        continue;
                    }
                    // Counted and indexed before any filter, for the same reason
                    // the armour walk does it: OutfitDye::Repaint assigns these
                    // same indices while walking every shape.
                    ++shapeCount;
                    const auto c =
                        static_cast<std::size_t>(ChannelForShapeIndex(s.index));

                    // Same rule as the armour walk above, and the same call, so
                    // the two cannot drift. A blood overlay traverses first on
                    // every weapon measured, so labelling it hands Primary to a
                    // mesh that stays invisible until the blade is bloodied and
                    // the dye appears to do nothing (OS-123).
                    if (!DyeShapeEarnsAStripe(s.reason)) {
                        continue;  // counted and indexed, never labelled
                    }
                    anyGarment[c] = true;
                    if (!s.name.empty()) {
                        if (!rows[c].label.empty()) {
                            rows[c].label += ", ";
                        }
                        rows[c].label += s.name;
                    }
                    rows[c].dyeable    = rows[c].dyeable || s.dyeable;
                    rows[c].reflective = rows[c].reflective || s.reflective;
                    if (!s.dyeable && rows[c].reason == DyeSkipReason::kNone) {
                        rows[c].reason = s.reason;
                    }
                }

                DyeTile tile;
                tile.target      = DyeTarget::kWeapon;
                tile.weaponClass = cls;
                tile.weaponHand  = hand;
                tile.shapeCount  = shapeCount;

                const auto live = LiveChannelCount(shapeCount);
                // Resolved, so a hand with no override of its own shows the Both
                // colour it will actually render in, rather than an empty swatch
                // beside a weapon that is visibly coloured.
                const auto dye = a_staged.ResolvedWeaponDyeFor(cls, hand);
                for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                    if (c >= live || !anyGarment[c]) {
                        continue;
                    }
                    rows[c].colour = dye.channels[c];
                    tile.stripes.push_back(std::move(rows[c]));
                }

                if (!tile.stripes.empty()) {
                    out.push_back(std::move(tile));
                }
            }
        }

        // Which weapon classes ended up with more than one tile. Only a
        // one-handed class carried in both hands can, and it is what tells the
        // editor whether an edit belongs to that hand or to the class.
        for (auto& t : out) {
            if (t.target != DyeTarget::kWeapon) {
                continue;
            }
            std::size_t sameClass = 0;
            for (const auto& other : out) {
                if (other.target == DyeTarget::kWeapon &&
                    other.weaponClass == t.weaponClass) {
                    ++sameClass;
                }
            }
            t.handSplit = sameClass > 1;
        }

        return out;
    }

}  // namespace OS::DyeGrid
