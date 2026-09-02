// Dye grid tests. No SKSE, no engine, no ImGui: which slots earn a tile and
// which channels earn a stripe is pure arithmetic over a shape snapshot.
#include "DyeGrid.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

using namespace OS;

// A garment shape on a slot, named, dyeable.
static ShapeInfo Garment(std::uint32_t a_bit, std::size_t a_index,
                         const char* a_name) {
    ShapeInfo s;
    s.slotBit = a_bit;
    s.index   = a_index;
    s.name    = a_name;
    s.dyeable = true;
    return s;
}

// The same, carrying the engine model the slot's geometry was cloned from.
// Zero, which is what Garment leaves, means "could not identify".
static ShapeInfo Clone(std::uint32_t a_bit, std::size_t a_index, const char* a_name,
                       std::uintptr_t a_source) {
    ShapeInfo s = Garment(a_bit, a_index, a_name);
    s.sourceId  = a_source;
    return s;
}

// A head-part shape: the slot, the part it was built from, and the shape's own
// name off the geometry. dyeable by default, like Garment above.
static ShapeInfo Head(std::uint32_t a_slot, const char* a_mod, std::uint32_t a_form,
                      std::size_t a_index, const char* a_name) {
    ShapeInfo s;
    s.target   = DyeTarget::kHeadPart;
    s.headSlot = a_slot;
    s.headPart = StyleRefKey{ a_mod, a_form };
    s.index    = a_index;
    s.name     = a_name;
    s.dyeable  = true;
    return s;
}

// A weapon shape. slotBit is deliberately left alone: for a weapon it means
// nothing, and the class-and-hand pair is the whole key.
static ShapeInfo Weapon(WeaponClass a_class, WeaponHand a_hand, std::size_t a_index,
                        const char* a_name) {
    ShapeInfo s;
    s.target      = DyeTarget::kWeapon;
    s.weaponClass = a_class;
    s.weaponHand  = a_hand;
    s.index       = a_index;
    s.name        = a_name;
    s.dyeable     = true;
    return s;
}

// Two bits on one armature, as the engine leaves them.
static DyeCloneOwners TwoClones(std::uint32_t a_lower, std::uint32_t a_upper) {
    std::array<std::uintptr_t, kBitCount> src{};
    src[a_lower] = src[a_upper] = 0xA1;
    return DyeCloneOwnersFrom(src);
}

// The grid MINUS the hair tile, which is unconditional and always first.
//
// ⚠ IT ASSERTS THE INVARIANT ON EVERY INPUT IN THIS FILE, and that is the point
// of routing the gear tests through it rather than shifting forty indices by
// one. "First and always" is a claim about every grid the function can build,
// not about the two the hair block below happens to construct, so every case
// here - dressed, bare, cloned, dual-wielding, ten shapes - pays it a check on
// the way past for free.
//
// Only ONE leading hair tile is dropped, so emitting two, or emitting one in
// the wrong place, still turns the gear assertions red rather than being
// absorbed here.
static std::vector<DyeTile> GearTiles(const std::vector<ShapeInfo>& a_shapes,
                                      const Outfit&                 a_staged) {
    auto tiles = DyeGrid::Build(a_shapes, a_staged);
    // ⚠ BOTH APPEARANCE TILES, IN ORDER, AND EACH ASSERTED BEFORE IT IS
    // DROPPED. Hair leads and eyes follow it, both unconditionally, and the
    // eye tile carries TWO stripes (iris then sclera) rather than being two
    // tiles. Asserting the target before erasing is what stops this helper
    // absorbing a tile emitted in the wrong place, which is the whole reason
    // it does not simply erase two entries.
    //
    // ⚠ TWO BECAUSE THIS HELPER TAKES THE DEFAULT, which is the shipping
    // answer: the eye's second colour is experimental and behind
    // bEyeAdvancedColour. The block further down exercises it on.
    CHECK(!tiles.empty() && tiles.front().target == DyeTarget::kHair);
    if (!tiles.empty() && tiles.front().target == DyeTarget::kHair) {
        tiles.erase(tiles.begin());
    }
    CHECK(!tiles.empty() && tiles.front().target == DyeTarget::kEyes);
    if (!tiles.empty() && tiles.front().target == DyeTarget::kEyes) {
        CHECK(tiles.front().stripes.size() == 2);
        tiles.erase(tiles.begin());
    }
    return tiles;
}

int main() {
    {  // a worn three-shape slot becomes one tile with three stripes
        std::vector<ShapeInfo> shapes{ Garment(2, 0, "Fabrics_Top"),
                                       Garment(2, 1, "Leather_Side"),
                                       Garment(2, 2, "Metal_Breastplate") };
        Outfit staged;
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        CHECK(tiles[0].bit == 2u);
        CHECK(tiles[0].shapeCount == 3);
        CHECK(tiles[0].stripes.size() == 3);
        CHECK(tiles[0].stripes[0].label == "Fabrics_Top");
        CHECK(tiles[0].stripes[2].label == "Metal_Breastplate");
        CHECK(tiles[0].stripes[0].dyeable);
    }

    {  // nothing worn and nothing stored: no tiles at all
        std::vector<ShapeInfo> shapes;
        Outfit                 staged;
        CHECK(GearTiles(shapes, staged).empty());
    }

    {  // the player's own body is not a garment piece, so it earns no stripe,
       // but it MUST still be counted: the count feeds LiveChannelCount and the
       // index feeds ChannelForShapeIndex, and OutfitDye::Repaint walks every
       // shape including this one. Drop it from either and dyeing one piece
       // recolours another.
        ShapeInfo body;
        body.slotBit = 2;
        body.index   = 0;
        body.name    = "3BA";
        body.dyeable = false;
        body.reason  = DyeSkipReason::kCharacterColour;

        std::vector<ShapeInfo> shapes{ body, Garment(2, 1, "Cloak") };
        Outfit                 staged;
        const auto             tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        CHECK(tiles[0].shapeCount == 2);        // body counted
        CHECK(tiles[0].stripes.size() == 1);    // body not shown
        CHECK(tiles[0].stripes[0].label == "Cloak");
        CHECK(tiles[0].stripes[0].channel == DyeChannelId::kSecondary);
    }

    {  // a slot with nothing worn gets NO tile, even with a colour still stored
       // on it. That colour is not lost: it is in the outfit and comes back with
       // the garment, and Reset piece purges it.
        std::vector<ShapeInfo> shapes;
        Outfit                 staged;
        staged.SetDye(7, DyeChannelId::kPrimary, DyeChannel{ true, 0x11, 0x22, 0x33 });
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.empty());
    }

    {  // dye channels 4 and 5 of a six-shape cuirass, then assign a two-shape
       // style. live drops to 2, so those colours are past the end and get NO
       // stripe: a tile shows the pieces of the garment being worn and nothing
       // else. Switching the six-shape style back brings them back with it.
        std::vector<ShapeInfo> shapes{ Garment(2, 0, "Body"), Garment(2, 1, "Trim") };
        Outfit                 staged;
        staged.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 1, 1, 1 });
        staged.SetDye(2, static_cast<DyeChannelId>(4), DyeChannel{ true, 4, 4, 4 });
        staged.SetDye(2, static_cast<DyeChannelId>(5), DyeChannel{ true, 5, 5, 5 });

        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        // Two stripes, the new style's live channels. A live channel with no
        // colour on it still gets one, because that is how you paint it in the
        // first place.
        CHECK(tiles[0].stripes.size() == 2);
        CHECK(tiles[0].stripes[0].channel == DyeChannelId::kPrimary);
        CHECK(tiles[0].stripes[0].colour.r == 1);
        CHECK(tiles[0].stripes[1].channel == DyeChannelId::kSecondary);
        CHECK(!tiles[0].stripes[1].colour.set);   // live, just never dyed
        // ⚠ The stranded colours are still IN THE OUTFIT, only undrawn. This is
        // the assertion that separates hiding from destroying.
        CHECK(staged.DyeFor(2).channels[4].set);
        CHECK(staged.DyeFor(2).channels[4].r == 4);
        CHECK(staged.DyeFor(2).channels[5].set);
    }

    {  // a body-only channel holding a stored colour gets no stripe: there is no
       // garment behind it, so there is no piece to show. The value is real and
       // stays in the outfit, it just has nowhere on screen to sit.
        ShapeInfo body;
        body.slotBit = 3;
        body.index   = 0;
        body.name    = "3BA";
        body.reason  = DyeSkipReason::kCharacterColour;

        std::vector<ShapeInfo> shapes{ body };
        Outfit                 staged;
        staged.SetDye(3, DyeChannelId::kPrimary, DyeChannel{ true, 9, 9, 9 });

        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.empty());
        CHECK(staged.DyeFor(3).channels[0].set);   // kept, not destroyed
    }

    {  // ⚠ THE FIELD REPORT. Empty a transmog slot and its stripe must GO, not
       // linger as a slashed swatch on a slot the user just cleared. The clear
       // takes the dye with it, so the tile has nothing left to be built from.
       //
       // Both halves are asserted, because "no tile" only means the fix worked
       // if the same state WITH the colour still stored does produce the orphan.
        std::vector<ShapeInfo> worn{ Garment(2, 0, "Robe"), Garment(2, 1, "Trim") };
        Outfit                 staged;
        staged.SetStyle(2, StyleRefKey{ "Armors.esp", 0x800 });
        staged.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 10, 20, 30 });
        CHECK(GearTiles(worn, staged).size() == 1);       // dressed and dyed

        // The slot is emptied, so the garment leaves the shape snapshot with it.
        const std::vector<ShapeInfo> bare;
        Outfit                       stale = staged;
        stale.SetPassthrough(2);                               // the OLD behaviour
        // The reported swatch is gone twice over now. It used to survive this as
        // an orphaned stripe, which is what the field report was about; stripes
        // are emitted for live pieces only, so an empty slot has none.
        CHECK(GearTiles(bare, stale).empty());

        ClearSlot(staged, 2);                                  // what the editor does now
        CHECK(GearTiles(bare, staged).empty());           // no tile, no stripe
    }

    {  // ...and it stays gone across a commit and a reopen. CommitStaging is a
       // whole-Outfit assignment into the library's active outfit and the editor
       // reloads by copying it straight back, so the cleared dye travels with
       // the cleared entry through both. Modelled here as the two copies they
       // are: a dye that survived either one would come back as the orphan.
        Outfit committed;
        committed.SetStyle(2, StyleRefKey{ "Armors.esp", 0x800 });
        committed.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 10, 20, 30 });

        Outfit staged = committed;      // editor opens on the committed outfit
        ClearSlot(staged, 2);
        committed = staged;             // Apply -> CommitStaging: *o = *staged_
        Outfit reopened = committed;    // reopen -> g_staged = *active

        CHECK(!reopened.DyeFor(2).Any());
        CHECK(GearTiles({}, reopened).empty());
    }

    {  // ⚠ THE SAFETY NET IS STILL LIVE. Nothing above may reach the orphan
       // through the common path any more, but a stored colour can still arrive
       // with no shape behind it - a co-save or an imported preset writes dye
       // per channel with no live-geometry check at all. It must still draw,
       // or the colour is unreachable and unclearable.
        Outfit loaded;                                    // as the codec leaves it
        loaded.SetStyle(2, StyleRefKey{ "Armors.esp", 0x800 });
        loaded.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 1, 1, 1 });
        loaded.SetDye(2, static_cast<DyeChannelId>(5), DyeChannel{ true, 5, 5, 5 });

        // The garment that came back is a two-shape one: channel 5 has nothing,
        // so it gets no stripe and the tile is exactly the two live pieces.
        std::vector<ShapeInfo> shapes{ Garment(2, 0, "Robe"), Garment(2, 1, "Trim") };
        const auto             tiles = GearTiles(shapes, loaded);
        CHECK(tiles.size() == 1);
        CHECK(tiles[0].stripes.size() == 2);
        CHECK(tiles[0].stripes[0].channel == DyeChannelId::kPrimary);
        CHECK(tiles[0].stripes[1].channel == DyeChannelId::kSecondary);
        // Survived the round trip even though nothing draws it.
        CHECK(loaded.DyeFor(2).channels[5].set);
    }

    {  // an undyeable garment piece still gets its stripe and keeps its reason.
       // Naming why is the point of the pane: dragonbone read as broken for
       // months because nothing was said at all.
        ShapeInfo glow;
        glow.slotBit = 4;
        glow.index   = 0;
        glow.name    = "GlowPart";
        glow.dyeable = false;
        glow.reason  = DyeSkipReason::kGlow;

        std::vector<ShapeInfo> shapes{ glow };
        Outfit                 staged;
        const auto             tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        CHECK(tiles[0].stripes.size() == 1);
        CHECK(!tiles[0].stripes[0].dyeable);
        CHECK(tiles[0].stripes[0].reason == DyeSkipReason::kGlow);
    }

    {  // more than one slot worn at once. The grid's whole job, and untested
       // until now. Tiles come back in slot bit order.
        std::vector<ShapeInfo> shapes{ Garment(7, 0, "Boot"), Garment(2, 0, "Robe"),
                                       Garment(2, 1, "Trim") };
        Outfit                 staged;
        const auto             tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 2);
        CHECK(tiles[0].bit == 2u);            // bit order, not shape order
        CHECK(tiles[0].stripes.size() == 2);
        CHECK(tiles[1].bit == 7u);
        CHECK(tiles[1].stripes.size() == 1);
    }

    {  // a mesh busier than the channel count. ChannelForShapeIndex folds every
       // shape from the last channel on into it, so the strip stops at the
       // channel count and the tail shares the last stripe rather than going
       // silently undyeable.
       //
       // ⚠ THE MESH IS SIZED FROM THE CONSTANT, not from a literal. This was
       // ten shapes against a cap of eight, and widening the cap to sixteen
       // turned it into an ordinary ten-shape mesh testing nothing.
        constexpr std::size_t kBusy = kDyeChannelCount + 2;
        std::vector<ShapeInfo> shapes;
        for (std::size_t i = 0; i < kBusy; ++i) {
            shapes.push_back(Garment(2, i, "Piece"));
        }
        Outfit     staged;
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        CHECK(tiles[0].shapeCount == kBusy);
        CHECK(tiles[0].stripes.size() == kDyeChannelCount);   // the cap, not kBusy
        // the last channel absorbed the tail, so its label carries several names
        CHECK(tiles[0].stripes.back().label == "Piece, Piece, Piece");
    }

    // ---- OS-112: one garment covering two biped slots is attached twice, so
    // the grid used to offer a tile per copy and the copies could be given two
    // different colours for one helmet.

    {  // the measured case. One helmet on biped 31 and 43, one armature, so the
       // engine clones the same model onto both bits. ONE tile, under the lower.
        std::vector<ShapeInfo> shapes{ Clone(1, 0, "Helmet", 0xA1),
                                       Clone(13, 0, "Helmet", 0xA1) };
        Outfit                 staged;
        const auto             tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        CHECK(tiles[0].bit == 1u);
    }

    {  // ⚠ THE CASE A SLOPPY FIX SILENTLY DELETES. The earlier helmet in the same
       // session carried two different addons on those same two bits, a helmet
       // piece and an ears piece. Different sources, two tiles, and dyeing them
       // apart is a feature rather than the defect above.
        std::vector<ShapeInfo> shapes{ Clone(1, 0, "HelmetPiece", 0xA1),
                                       Clone(13, 0, "EarsPiece", 0xB2) };
        Outfit                 staged;
        const auto             tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 2);
        CHECK(tiles[0].bit == 1u);
        CHECK(tiles[1].bit == 13u);
    }

    {  // ⚠ NEGATIVE CONTROL FOR THE RULE, NOT THE OUTCOME. Both clones of one
       // mesh do carry identical names, so comparing names would pass every test
       // above - it fits the measured data exactly. It is still wrong, because
       // two unrelated pieces are free to share a name and the failure mode is
       // silently merging two real controls into one.
       //
       // Same name, different armatures. Compare sources and this is two tiles;
       // flip the grouping to compare names and THIS is the assertion that goes
       // red while every other one stays green.
        std::vector<ShapeInfo> shapes{ Clone(1, 0, "ArmorHelmet", 0xA1),
                                       Clone(13, 0, "ArmorHelmet", 0xB2) };
        Outfit                 staged;
        CHECK(GearTiles(shapes, staged).size() == 2);
    }

    {  // a zero source is "could not identify" and it claims nothing, including
       // against another zero. Two unidentified slots keep their own tiles, so a
       // producer that cannot answer leaves the grid exactly as it was before the
       // collapse existed.
        std::vector<ShapeInfo> shapes{ Garment(1, 0, "A"), Garment(13, 0, "B") };
        Outfit                 staged;
        CHECK(GearTiles(shapes, staged).size() == 2);
    }

    {  // three slots on one armature collapse to ONE tile, not two. The mapping
       // has to be transitive: bit 20 matches bit 13, which already belongs to
       // bit 1, so it must land on 1 rather than on the middle member.
        std::vector<ShapeInfo> shapes{ Clone(1, 0, "Robe", 0xC3),
                                       Clone(13, 0, "Robe", 0xC3),
                                       Clone(20, 0, "Robe", 0xC3) };
        Outfit                 staged;
        const auto             tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        CHECK(tiles[0].bit == 1u);
    }

    {  // the collapse changes WHICH TILES EXIST and nothing else. The survivor
       // keeps its own stripes, its own shape count and its own channel indices.
        std::vector<ShapeInfo> shapes{
            Clone(1, 0, "Body", 0xD4), Clone(1, 1, "Trim", 0xD4),
            Clone(13, 0, "Body", 0xD4), Clone(13, 1, "Trim", 0xD4)
        };
        Outfit     staged;
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        CHECK(tiles[0].bit == 1u);
        CHECK(tiles[0].shapeCount == 2);          // the survivor's own, not both copies'
        CHECK(tiles[0].stripes.size() == 2);
        CHECK(tiles[0].stripes[0].channel == DyeChannelId::kPrimary);
        CHECK(tiles[0].stripes[0].label == "Body");
        CHECK(tiles[0].stripes[1].channel == DyeChannelId::kSecondary);
        CHECK(tiles[0].stripes[1].label == "Trim");
    }

    {  // ⚠ THE LIVE SAVE, which has red on the bit that loses its tile. The
       // colour has to appear ON THE SURVIVING TILE, or it paints the helmet with
       // no swatch anywhere on screen showing it - which is the state that is
       // worse than the defect.
        std::vector<ShapeInfo> shapes{ Clone(1, 0, "Helmet", 0xA1),
                                       Clone(13, 0, "Helmet", 0xA1) };
        Outfit                 staged;
        staged.SetDye(13, DyeChannelId::kPrimary, DyeChannel{ true, 0xFF, 0, 0 });
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        CHECK(tiles[0].bit == 1u);
        CHECK(tiles[0].stripes[0].colour.set);
        CHECK(tiles[0].stripes[0].colour.r == 0xFF);
    }

    {  // ⚠ THE REPAINT HALF, STATED PURELY. Every bit in a group resolves to the
       // SAME value, which is what makes the two shader properties take one
       // write. Read DyeFor(bit) in the paint pass instead - that is exactly
       // "ship the tile collapse without this half" - and this goes red. The
       // state it catches is one helmet rendering in two colours with only one
       // control on screen.
        Outfit o;
        o.SetDye(13, DyeChannelId::kPrimary, DyeChannel{ true, 0xFF, 0, 0 });
        const auto owners = TwoClones(1, 13);
        CHECK(ResolvedSlotDye(o, owners, 1).channels ==
              ResolvedSlotDye(o, owners, 13).channels);
        CHECK(ResolvedSlotDye(o, owners, 13).channels[0].r == 0xFF);
    }

    {  // the owner's own choice wins. One mesh, one answer, and the answer has to
       // be the one with a tile behind it.
        Outfit o;
        o.SetDye(1, DyeChannelId::kPrimary, DyeChannel{ true, 0, 0, 0xFF });
        o.SetDye(13, DyeChannelId::kPrimary, DyeChannel{ true, 0xFF, 0, 0 });
        const auto owners = TwoClones(1, 13);
        CHECK(ResolvedSlotDye(o, owners, 1).channels[0].b == 0xFF);
        CHECK(ResolvedSlotDye(o, owners, 1).channels[0].r == 0);
    }

    {  // the collapse MOVES the colour down and EMPTIES the bit that lost its
       // tile. Both halves asserted, because moving without emptying leaves a
       // ghost: unreachable, unclearable, painting nothing of its own, and it
       // resolves straight back into view the moment the surviving tile is
       // cleared. The last two lines are that clear actually working.
        Outfit o;
        o.SetDye(13, DyeChannelId::kPrimary, DyeChannel{ true, 0xFF, 0, 0 });
        const auto owners = TwoClones(1, 13);
        CHECK(CollapseCloneDyes(o, owners));
        CHECK(o.DyeFor(1).channels[0].set);
        CHECK(o.DyeFor(1).channels[0].r == 0xFF);
        CHECK(!o.DyeFor(13).Any());

        o.SetDye(1, DyeChannelId::kPrimary, DyeChannel{});
        CHECK(!ResolvedSlotDye(o, owners, 1).channels[0].set);
    }

    {  // a channel that could NOT move is emptied all the same. The owner had
       // already been given that channel, so the loser's value lost the merge -
       // and leaving it behind would make it exactly the ghost above.
        Outfit o;
        o.SetDye(1, DyeChannelId::kPrimary, DyeChannel{ true, 0, 0, 0xFF });
        o.SetDye(13, DyeChannelId::kPrimary, DyeChannel{ true, 0xFF, 0, 0 });
        o.SetDye(13, DyeChannelId::kSecondary, DyeChannel{ true, 0, 0xFF, 0 });
        const auto owners = TwoClones(1, 13);
        CHECK(CollapseCloneDyes(o, owners));
        CHECK(o.DyeFor(1).channels[0].b == 0xFF);   // the owner's own, untouched
        CHECK(o.DyeFor(1).channels[1].g == 0xFF);   // the free channel moved down
        CHECK(!o.DyeFor(13).Any());                 // nothing left behind either way
    }

    {  // three slots on one armature all fold onto the owner, and the lowest
       // donor wins a channel two of them carry.
        Outfit o;
        o.SetDye(13, DyeChannelId::kPrimary, DyeChannel{ true, 0xFF, 0, 0 });
        o.SetDye(20, DyeChannelId::kPrimary, DyeChannel{ true, 0, 0xFF, 0 });
        o.SetDye(20, DyeChannelId::kSecondary, DyeChannel{ true, 0, 0, 0xFF });
        std::array<std::uintptr_t, kBitCount> src{};
        src[1] = src[13] = src[20] = 0xC3;
        const auto owners = DyeCloneOwnersFrom(src);
        CHECK(CollapseCloneDyes(o, owners));
        CHECK(o.DyeFor(1).channels[0].r == 0xFF);   // bit 13, the lower donor
        CHECK(o.DyeFor(1).channels[1].b == 0xFF);   // bit 20's uncontested channel
        CHECK(!o.DyeFor(13).Any());
        CHECK(!o.DyeFor(20).Any());
    }

    {  // idempotent, because the editor runs it every frame the pane is open. The
       // second run must find nothing to do and change nothing.
        Outfit o;
        o.SetDye(13, DyeChannelId::kPrimary, DyeChannel{ true, 0xFF, 0, 0 });
        const auto owners = TwoClones(1, 13);
        CHECK(CollapseCloneDyes(o, owners));
        CHECK(!CollapseCloneDyes(o, owners));
        CHECK(o.DyeFor(1).channels[0].r == 0xFF);
        CHECK(!o.DyeFor(13).Any());
    }

    {  // nothing grouped, nothing touched. Two separate garments keep their own
       // colours and the function reports no change, so the ordinary outfit never
       // gets rewritten on its way through the pane.
        Outfit o;
        o.SetDye(1, DyeChannelId::kPrimary, DyeChannel{ true, 0, 0, 0xFF });
        o.SetDye(13, DyeChannelId::kPrimary, DyeChannel{ true, 0xFF, 0, 0 });
        std::array<std::uintptr_t, kBitCount> src{};
        src[1]            = 0xA1;
        src[13]           = 0xB2;
        const auto owners = DyeCloneOwnersFrom(src);
        CHECK(!CollapseCloneDyes(o, owners));
        CHECK(o.DyeFor(1).channels[0].b == 0xFF);
        CHECK(o.DyeFor(13).channels[0].r == 0xFF);
    }

    // ---- weapon tiles -------------------------------------------------------
    // A weapon is keyed by class and hand, never by slot bit. WeaponSlots.h
    // opens by calling weapon slots "an unrelated index space from the armor
    // EDITOR bits", and the off-hand weapon shares biped 9 with the shield, so
    // widening slotBit to cover both would collide there head on.
    {  // ⚠ ONE-HANDED CLASS, BOTH HANDS: TWO TILES. The engine stages the main
       // hand in the class slot and the off hand in biped 9, so a dual-wielding
       // character has two swords on screen and must be able to colour them
       // separately. That is the whole reason hands exist in this dimension.
        std::vector<ShapeInfo> shapes{
            Weapon(WeaponClass::Sword, WeaponHand::Right, 0, "Blade"),
            Weapon(WeaponClass::Sword, WeaponHand::Left, 0, "Blade"),
        };
        Outfit staged;
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 2);
        if (tiles.size() == 2) {
            CHECK(tiles[0].target == DyeTarget::kWeapon);
            CHECK(tiles[0].weaponClass == WeaponClass::Sword);
            CHECK(tiles[0].weaponHand == WeaponHand::Right);
            CHECK(tiles[1].weaponHand == WeaponHand::Left);
            CHECK(tiles[0].stripes.size() == 1);
            // ⚠ TWO SWORDS ON SCREEN, so an edit belongs to the hand and not to
            // the class. Without this the editor writes the Both value and one
            // click silently recolours the other hand too.
            CHECK(tiles[0].handSplit);
            CHECK(tiles[1].handSplit);
        }
    }

    {  // ⚠ ONE SWORD IS NOT SPLIT, and that is the half that decides the common
       // case. Colouring the only sword you carry should colour "your sword",
       // the Both value, so a second one picked up later matches it. It is a
       // Right-hand tile either way, because the engine stages the main hand in
       // the class slot, so the tile's own hand cannot answer this.
        std::vector<ShapeInfo> shapes{
            Weapon(WeaponClass::Sword, WeaponHand::Right, 0, "Blade"),
        };
        Outfit staged;
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        if (tiles.size() == 1) {
            CHECK(tiles[0].weaponHand == WeaponHand::Right);
            CHECK(!tiles[0].handSplit);
        }
    }

    {  // Two DIFFERENT classes are not a split either: a sword and a dagger are
       // one of each, so each is still "your sword" and "your dagger".
        std::vector<ShapeInfo> shapes{
            Weapon(WeaponClass::Sword, WeaponHand::Right, 0, "Blade"),
            Weapon(WeaponClass::Dagger, WeaponHand::Left, 0, "Blade"),
        };
        Outfit staged;
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 2);
        if (tiles.size() == 2) {
            CHECK(!tiles[0].handSplit);
            CHECK(!tiles[1].handSplit);
        }
    }

    {  // A two-handed class is ONE tile and its hand is Both. Nothing about a
       // greatsword has a hand to choose, and SupportsHandOverrides refuses one,
       // so a grid that offered two would offer a control that cannot render.
        std::vector<ShapeInfo> shapes{
            Weapon(WeaponClass::Greatsword, WeaponHand::Both, 0, "Blade"),
            Weapon(WeaponClass::Greatsword, WeaponHand::Both, 1, "Hilt"),
        };
        Outfit staged;
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        if (tiles.size() == 1) {
            CHECK(tiles[0].weaponClass == WeaponClass::Greatsword);
            CHECK(tiles[0].weaponHand == WeaponHand::Both);
            CHECK(tiles[0].stripes.size() == 2);
            CHECK(tiles[0].stripes[1].label == "Hilt");
        }
    }

    {  // ⚠ A WEAPON'S BLOOD OVERLAY IS COUNTED AND INDEXED, NEVER LABELLED, the
       // same treatment the player's body gets on an armour slot. Skyrim ships
       // this shape on weapon meshes and it traverses FIRST, so without the
       // filter it claims Primary: the user reaches for the first swatch, the
       // write lands on geometry that is invisible until the blade is bloodied,
       // and the report is "I dye it and nothing changes" (OS-123).
       //
       // The count and the index MUST survive, because OutfitDye::Repaint walks
       // every shape including this one. Drop it from either and the editor
       // labels one piece while the renderer paints another.
        ShapeInfo blood = Weapon(WeaponClass::Sword, WeaponHand::Right, 0, "Edgeblood01");
        blood.dyeable   = false;
        blood.reason    = DyeSkipReason::kDecal;

        std::vector<ShapeInfo> shapes{
            blood,
            Weapon(WeaponClass::Sword, WeaponHand::Right, 1, "IronLongSword:0"),
        };
        Outfit     staged;
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        if (tiles.size() == 1) {
            CHECK(tiles[0].shapeCount == 2);      // blood counted
            CHECK(tiles[0].stripes.size() == 1);  // blood not shown
            CHECK(tiles[0].stripes[0].label == "IronLongSword:0");
            // ⚠ THE VACATED CHANNEL DOES NOT COLLAPSE. The blade keeps the
            // channel its traversal index earned, which is what keeps the grid
            // and Repaint agreeing on a stored colour. The hole is invisible in
            // the pane: DrawDyeTile lays swatches out positionally, so the blade
            // is simply the first square on the tile.
            CHECK(tiles[0].stripes[0].channel == DyeChannelId::kSecondary);
        }
    }

    {  // ⚠ THE SAME ON AN ARMOUR SLOT, because the rule is about what the shape
       // IS and not about which walk found it. The flag is read on every shape,
       // so a garment carrying one must not be the one place a blood overlay
       // gets a crossed-out square and a tooltip. Hidden means hidden.
        ShapeInfo blood = Garment(3, 0, "BloodLight");
        blood.dyeable   = false;
        blood.reason    = DyeSkipReason::kDecal;

        std::vector<ShapeInfo> shapes{ blood, Garment(3, 1, "Gauntlet") };
        Outfit                 staged;
        const auto             tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        if (tiles.size() == 1) {
            CHECK(tiles[0].shapeCount == 2);      // counted
            CHECK(tiles[0].stripes.size() == 1);  // not shown
            CHECK(tiles[0].stripes[0].label == "Gauntlet");
            CHECK(tiles[0].stripes[0].channel == DyeChannelId::kSecondary);
        }
    }

    {  // A sheathed weapon whose only shape is the blood overlay earns NO tile.
       // Emitting one would put a tile on screen whose every swatch is dead,
       // which is the phantom-tile shape of OS-120 arriving by another route.
        ShapeInfo blood = Weapon(WeaponClass::Dagger, WeaponHand::Left, 0, "BloodLighting");
        blood.dyeable   = false;
        blood.reason    = DyeSkipReason::kDecal;

        Outfit staged;
        CHECK(GearTiles(std::vector<ShapeInfo>{ blood }, staged).empty());
    }

    {  // ⚠ THE SHIELD'S OWN COLOUR SURVIVES A WEAPON BEING HELD, which is the
       // half OS-120 has to not break. Dropping the synthetic tile is what used
       // to make a stored shield colour read as orphaned, so the rule has to be
       // "no tile" and not "no colour": biped 9 emits nothing while a sword is
       // in that hand, and the shield's colour is still there to come back when
       // the shield does.
        Outfit staged;
        staged.SetDye(9, DyeChannelId::kPrimary, DyeChannel{ true, 9, 9, 9 });

        // A sword in the left hand: the weapon walk gives it its own tile, and
        // the armour side contributes nothing at all for bit 9.
        std::vector<ShapeInfo> shapes{
            Weapon(WeaponClass::Sword, WeaponHand::Left, 0, "Blade"),
        };
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 1);
        if (tiles.size() == 1) {
            CHECK(tiles[0].target == DyeTarget::kWeapon);
        }
        // Hidden, never destroyed.
        CHECK(staged.DyeFor(9).channels[0].set);
        CHECK(staged.DyeFor(9).channels[0].r == 9);
    }

    {  // ⚠ THE MASK IS BY CHANNEL ID, NEVER BY STRIPE POSITION, and that is the
       // whole reason it is a function rather than a loop at the call site.
       // Stripes are only the LIVE channels: a garment whose first shape is the
       // player's body, or a weapon's blood overlay, emits no stripe for channel
       // 0, so stripe[0] is channel 1. Indexing the mask positionally would then
       // put the metal's flag on the wrong channel and the rail would skip a
       // piece the player can see while dulling the one they cannot.
        DyeTile tile;
        tile.target     = DyeTarget::kArmour;
        tile.shapeCount = 3;
        DyeStripe a;
        a.channel    = DyeChannelId::kSecondary;  // channel 1, first stripe drawn
        a.reflective = false;
        DyeStripe b;
        b.channel    = DyeChannelId::kAccent;  // channel 2
        b.reflective = true;                   // the metal
        tile.stripes = { a, b };

        const auto mask = DyeGrid::ReflectiveMask(tile);
        CHECK(!mask[0]);  // no stripe: not reflective, and not skipped
        CHECK(!mask[1]);
        CHECK(mask[2]);   // the metal, on its own channel id rather than at [1]
        for (std::size_t c = 3; c < kDyeChannelCount; ++c) {
            CHECK(!mask[c]);
        }
    }

    {  // Arrows and bolts SHARE biped 41 and are still two classes. Keying on
       // the slot would give them one tile between them.
        std::vector<ShapeInfo> shapes{
            Weapon(WeaponClass::Arrows, WeaponHand::Both, 0, "Shafts"),
            Weapon(WeaponClass::Bolts, WeaponHand::Both, 0, "Shafts"),
        };
        Outfit staged;
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 2);
        if (tiles.size() == 2) {
            CHECK(tiles[0].weaponClass == WeaponClass::Arrows);
            CHECK(tiles[1].weaponClass == WeaponClass::Bolts);
        }
    }

    {  // Armour first in bit order, weapons after in class-then-hand order,
       // matching the slots panel where Weapons sits below Regular. Fed in
       // reverse so the order proves the walk and not the input.
        std::vector<ShapeInfo> shapes{
            Weapon(WeaponClass::Staff, WeaponHand::Both, 0, "Shaft"),
            Weapon(WeaponClass::Sword, WeaponHand::Left, 0, "Blade"),
            Garment(7, 0, "Boot"),
            Garment(2, 0, "Cuirass"),
        };
        Outfit staged;
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 4);
        if (tiles.size() == 4) {
            CHECK(tiles[0].target == DyeTarget::kArmour);
            CHECK(tiles[0].bit == 2u);
            CHECK(tiles[1].target == DyeTarget::kArmour);
            CHECK(tiles[1].bit == 7u);
            CHECK(tiles[2].target == DyeTarget::kWeapon);
            CHECK(tiles[2].weaponClass == WeaponClass::Sword);
            CHECK(tiles[3].weaponClass == WeaponClass::Staff);
        }
    }

    {  // The tile's colour resolves through ResolvedWeaponDyeFor, so a hand
       // override shows on that hand's tile and the other keeps inheriting.
        std::vector<ShapeInfo> shapes{
            Weapon(WeaponClass::Sword, WeaponHand::Right, 0, "Blade"),
            Weapon(WeaponClass::Sword, WeaponHand::Left, 0, "Blade"),
        };
        Outfit staged;
        staged.SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary,
                            DyeChannel{ true, 200, 0, 0 });
        staged.SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary,
                            DyeChannel{ true, 0, 0, 200 }, WeaponHand::Left);
        const auto tiles = GearTiles(shapes, staged);
        CHECK(tiles.size() == 2);
        if (tiles.size() == 2) {
            CHECK(tiles[0].stripes[0].colour.r == 200);  // Right inherits Both
            CHECK(tiles[1].stripes[0].colour.b == 200);  // Left overrides
        }
    }

    {  // ⚠ A WEAPON SHAPE MUST NOT REACH THE ARMOUR CLONE GROUPING, and this is
       // the assertion that proves the filter rather than merely surviving it.
       //
       // slotBit is shared storage that means NOTHING on a weapon, so a weapon
       // shape carries whatever it was left at. DyeCloneOwnersFromShapes takes
       // the FIRST source it sees per bit, so an unfiltered weapon listed ahead
       // of the armour steals that bit's entry. Here it steals bit 0's, the two
       // bits of one genuine two-slot garment then fail to match, and OS-112
       // comes straight back: one helmet, two tiles, two colours.
       //
       // ⚠ THE WEAPON IS LISTED FIRST ON PURPOSE. Listed second it changes
       // nothing, because the bit is already claimed, and the test would pass
       // with the filter removed. An earlier draft of this test did exactly
       // that and proved nothing.
        auto weapon     = Weapon(WeaponClass::Sword, WeaponHand::Left, 0, "Blade");
        weapon.sourceId = 0xBEEF;  // slotBit left at its default 0

        std::vector<ShapeInfo> shapes{ weapon, Clone(0, 0, "Helm", 0xA1),
                                       Clone(1, 0, "Ears", 0xA1) };
        Outfit                 staged;
        const auto             tiles = GearTiles(shapes, staged);
        // One armour tile for the two-slot garment, plus the weapon's own.
        CHECK(tiles.size() == 2);
        if (tiles.size() == 2) {
            CHECK(tiles[0].target == DyeTarget::kArmour);
            CHECK(tiles[0].bit == 0u);
            CHECK(tiles[0].stripes.size() == 1);  // both bits, one tile
            CHECK(tiles[1].target == DyeTarget::kWeapon);
            CHECK(tiles[1].weaponHand == WeaponHand::Left);
        }
    }

    // ---- the hair tile ------------------------------------------------------
    // Hair is a tile like the others to look at and nothing like them
    // underneath: no slot bit, no weapon key, exactly one stripe, and it is
    // always present because the control it replaced was always present.
    //
    // ⚠ Built through DyeGrid::Build directly, never through GearTiles. GearTiles
    // exists to drop this tile, so asserting on its output here would be asking
    // the helper to prove the thing it removes.
    {
        Outfit o;

        // Always emitted, even with no shapes at all, and always first. Nothing
        // else is on this grid, so the two appearance tiles are the whole of
        // it, and the eye tile carries the eye as two stripes by default.
        const auto empty = DyeGrid::Build({}, o);
        CHECK(empty.size() == 2);  // hair, then eyes: both unconditional
        if (empty.size() == 2) {
            CHECK(empty.front().target == DyeTarget::kHair);
            CHECK(empty[1].target == DyeTarget::kEyes);
            // ⚠ THE SHIPPING SHAPE: iris and white, and no second colour. That
            // is the whole eye feature for an ordinary install, and the second
            // colour is experimental and behind a setting.
            CHECK(empty[1].stripes.size() == 2);
            if (empty[1].stripes.size() == 2) {
                CHECK(static_cast<int>(empty[1].stripes[0].channel) == 0);
                CHECK(static_cast<int>(empty[1].stripes[1].channel) == 1);
                // ⚠ AND THE WHITE IS DYEABLE WITH THE SETTING OFF EVEN WHEN A
                // SECOND COLOUR IS STORED. That is exercised properly below;
                // here it pins the plain case.
                CHECK(empty[1].stripes[1].dyeable);
            }
            CHECK(empty.front().stripes.size() == 1);
            if (empty.front().stripes.size() == 1) {
                CHECK(empty.front().stripes[0].dyeable);
            }
        }

        {  // ⚠⚠ THE SETTING GATES THE STRIPE, AND A STORED COLOUR MUST NOT
           // OUTLIVE IT. An outfit given a second eye colour while the
           // experiment was on keeps those bytes forever. With the setting off
           // the stripe is gone AND the white is dyeable again: keying the
           // white's cross on the stored colour alone would leave it refusing
           // on an install where the second colour cannot paint at all, which
           // is a dead control explaining itself with a reason that is no
           // longer true.
            Outfit e;
            e.eyeTint  = HairTint{ true, 10, 20, 30 };
            e.eyeTint2 = HairTint{ true, 200, 40, 90 };

            const auto off = DyeGrid::Build({}, e, false);
            CHECK(off.size() == 2);
            if (off.size() == 2) {
                CHECK(off[1].stripes.size() == 2);
                if (off[1].stripes.size() == 2) {
                    CHECK(off[1].stripes[1].dyeable);  // the white is free
                    CHECK(off[1].stripes[1].reason == DyeSkipReason::kNone);
                }
            }

            const auto on = DyeGrid::Build({}, e, true);
            CHECK(on.size() == 2);
            if (on.size() == 2) {
                CHECK(on[1].stripes.size() == 3);
                if (on[1].stripes.size() == 3) {
                    CHECK(static_cast<int>(on[1].stripes[2].channel) == 2);
                    CHECK(on[1].stripes[2].colour.set);
                    CHECK(on[1].stripes[2].colour.r == 200);
                    // The two share one shader register and the second wins, so
                    // the white crosses with a reason rather than going quiet.
                    CHECK(!on[1].stripes[1].dyeable);
                    CHECK(on[1].stripes[1].reason == DyeSkipReason::kEyeSecondColour);
                }
            }
        }

        {  // ⚠ THE SECOND COLOUR NEEDS AN IRIS TO SIT BESIDE, and the stripe
           // says so BEFORE the click. It paints the far half of the texture,
           // so with the iris unset the near half has no colour of its own and
           // half an eye would paint. The field report was "dye the second
           // colour only and nothing happens": the refusal existed in the
           // painter and was logged, and nothing said it on screen.
            Outfit bare;
            bare.eyeTint2 = HairTint{ true, 200, 40, 90 };

            const auto tiles = DyeGrid::Build({}, bare, true);
            CHECK(tiles.size() == 2);
            if (tiles.size() == 2 && tiles[1].stripes.size() == 3) {
                CHECK(!tiles[1].stripes[2].dyeable);
                CHECK(tiles[1].stripes[2].reason == DyeSkipReason::kEyeNeedsIris);
            }

            // Give it an iris and the refusal lifts.
            bare.eyeTint     = HairTint{ true, 10, 20, 30 };
            const auto fixed = DyeGrid::Build({}, bare, true);
            if (fixed.size() == 2 && fixed[1].stripes.size() == 3) {
                CHECK(fixed[1].stripes[2].dyeable);
                CHECK(fixed[1].stripes[2].reason == DyeSkipReason::kNone);
            }
        }

        // Still first once a garment is on, so it never moves as gear changes.
        // ⚠ Built through the same helper the gear tests use rather than a
        // positional aggregate: ShapeInfo's own header warns that `target` went
        // in FIRST rather than being appended, so an initialiser list written
        // from memory puts the slot bit in the weapon class.
        const std::vector<ShapeInfo> shapes{ Garment(2, 0, "Cuirass") };
        const auto                   dressed = DyeGrid::Build(shapes, o);
        CHECK(dressed.size() == 3);  // hair, eyes, and the garment's own tile
        if (!dressed.empty()) {
            CHECK(dressed.front().target == DyeTarget::kHair);
        }

        // ⚠ The property that matters: no armour path can ever reach it.
        // CollapseCloneDyes and the clone grouping walk slot bits, so a hair
        // tile carrying a bit is what would drag hair into them. Bit 0 is a real
        // addressable slot rather than a spare value, so a hair tile holding it
        // would collide with that slot's own garment rather than sit harmlessly
        // out of range.
        for (const auto& t : dressed) {
            if (t.target == DyeTarget::kHair) {
                CHECK(t.bit == 0);
                // No worn shape behind it, so it contributes none to the count
                // the bulk paths size their channel loops from.
                CHECK(t.shapeCount == 0);
            }
        }

        // ⚠ NO QUALIFYING TEST, and this is the assertion that pins it. Every
        // other tile appears only when a shape earns it; hair has no shape to
        // earn it with and must appear anyway, because the checkbox and picker
        // it replaces were always available - including for an "(away)" follower
        // with no loaded actor to read a head off. A has-hair gate here would be
        // a behaviour change smuggled in under a presentation change.
        //
        // hairTint round-trips through the tile in both states. Untinted first,
        // because a stripe that reported `set` regardless would pass the tinted
        // half on its own and mean "this outfit overrides the hair colour" for
        // every outfit ever made.
        if (empty.size() == 2 && empty.front().stripes.size() == 1) {
            CHECK(!empty.front().stripes[0].colour.set);
        }

        o.hairTint        = HairTint{ true, 10, 20, 30 };
        const auto tinted = DyeGrid::Build({}, o);
        CHECK(tinted.size() == 2 && tinted.front().stripes.size() == 1);
        if (tinted.size() == 2 && tinted.front().stripes.size() == 1) {
            const auto& c = tinted.front().stripes[0].colour;
            CHECK(c.set);
            CHECK(c.r == 10 && c.g == 20 && c.b == 30);

            // ⚠ THE TWO MATERIAL BLOCKS STAY EMPTY. They describe a material
            // swap on worn geometry, and hair is painted by facegen, so there is
            // no material to nudge. Filling them would send a sheen and a gloss
            // down a path with no shader property at the end of it.
            CHECK(!c.palette.Any());
            CHECK(!c.player.Any());
        }

        // The eye pair round-trips through the ONE eye tile's two stripes,
        // and each stripe reads its OWN field: the sclera stripe showing the
        // iris colour would be the one-flag-two-rows bug in miniature. Iris
        // first, sclera second, addressed by channel.
        o.eyeTint         = HairTint{ true, 1, 2, 3 };
        o.scleraTint      = HairTint{ true, 4, 5, 6 };
        const auto eyed   = DyeGrid::Build({}, o);
        CHECK(eyed.size() == 2);
        if (eyed.size() == 2 && eyed[1].stripes.size() == 2) {
            CHECK(static_cast<int>(eyed[1].stripes[0].channel) == 0);
            CHECK(static_cast<int>(eyed[1].stripes[1].channel) == 1);
            const auto& ec = eyed[1].stripes[0].colour;
            const auto& sc = eyed[1].stripes[1].colour;
            CHECK(ec.set && ec.r == 1 && ec.g == 2 && ec.b == 3);
            CHECK(sc.set && sc.r == 4 && sc.g == 5 && sc.b == 6);
        }
    }


    // ---- head parts: TWO GROUPS, and the key stays per shape ---------------
    //
    // The shapes are the 2026-08-17 census. A hair contributes nine parts on slot
    // 3 with two separately dyeable, and horns sit on invented slots 32 and 106.
    // One tile per part put eleven tiles on screen and the pane stopped being
    // readable, so the hair's shapes join the hair's own colour and the invented
    // slots share a second tile. What did NOT change is the key: a colour still
    // belongs to one (slot, part), and each stripe carries that.
    {
        std::vector<ShapeInfo> shapes;
        shapes.push_back(Head(3, "Hair.esp", 0x801, 0, "ACC"));
        shapes.push_back(Head(3, "Hair.esp", 0x802, 0, "GuanYinping2"));
        shapes.push_back(Head(106, "ED.esp", 0x582D, 0, "D_EDHornB"));
        shapes.push_back(Head(32, "NK.esp", 0x81F, 0, "_NK_DragonboneHelmHorns"));

        Outfit staged;
        staged.hairTint = HairTint{ true, 30, 20, 10 };
        staged.SetHeadPartDye(3, StyleRefKey{ "Hair.esp", 0x801 }, DyeChannelId::kPrimary,
                              DyeChannel{ true, 210, 170, 60 });
        staged.SetHeadPartDye(106, StyleRefKey{ "ED.esp", 0x582D }, DyeChannelId::kPrimary,
                              DyeChannel{ true, 15, 76, 92 });

        const auto tiles = DyeGrid::Build(shapes, staged);

        // Exactly TWO tiles carry head groups: the hair and the extras.
        std::size_t groups = 0;
        for (const auto& t : tiles) {
            if (t.headGroup != HeadGroup::kNone) {
                ++groups;
            }
        }
        CHECK(groups == 2);

        const DyeTile* hair   = nullptr;
        const DyeTile* extras = nullptr;
        for (const auto& t : tiles) {
            if (t.headGroup == HeadGroup::kHair) {
                hair = &t;
            }
            if (t.headGroup == HeadGroup::kExtras) {
                extras = &t;
            }
        }
        CHECK(hair != nullptr);
        CHECK(extras != nullptr);

        if (hair) {
            // The hair's own colour first, then its two shapes.
            CHECK(hair->stripes.size() == 3);
            // ⚠ THE FIRST STRIPE IS THE COLOUR AND IT IS NOT A HEAD PART. A tile
            // whose stripes all shared its target is exactly what this replaced.
            CHECK(hair->stripes[0].target == DyeTarget::kHair);
            CHECK(hair->stripes[0].colour.set);
            CHECK(hair->stripes[0].colour.r == 30);
            CHECK(hair->stripes[1].target == DyeTarget::kHeadPart);
            CHECK(hair->stripes[2].target == DyeTarget::kHeadPart);
            // Each shape stripe carries its own key, so the two cannot share a
            // colour: the ornament is dyed and the cloth beside it is not.
            CHECK(hair->stripes[1].label == "ACC");
            CHECK(hair->stripes[1].headPart.localFormID == 0x801);
            CHECK(hair->stripes[1].colour.set && hair->stripes[1].colour.r == 210);
            CHECK(hair->stripes[2].label == "GuanYinping2");
            CHECK(hair->stripes[2].headPart.localFormID == 0x802);
            CHECK(!hair->stripes[2].colour.set);
        }

        if (extras) {
            // Both invented slots share the one tile, in walk order.
            CHECK(extras->stripes.size() == 2);
            CHECK(extras->stripes[0].target == DyeTarget::kHeadPart);
            CHECK(extras->stripes[0].headSlot == 106);
            CHECK(extras->stripes[0].colour.set && extras->stripes[0].colour.r == 15);
            CHECK(extras->stripes[1].headSlot == 32);
            CHECK(!extras->stripes[1].colour.set);
        }

        // ⚠⚠ NEITHER GROUP COUNTS WORN SHAPES, and the hair colour is why it
        // matters. SetStagedDyeChannel writes hairTint on ANY channel, and the
        // only thing stopping a pasted scheme from landing its last, unset write
        // there is that bulk paste is disabled on a zero shapeCount. So a group
        // that counted its shapes would clear the player hair colour on a paste.
        CHECK(hair->shapeCount == 0);
        CHECK(extras->shapeCount == 0);

        // ⚠ NEITHER GROUP IS A GARMENT TILE, so a character wearing nothing but
        // horns must not read as having dyeable gear on.
        std::size_t worn = 0;
        for (const auto& t : tiles) {
            if (t.target == DyeTarget::kArmour || t.target == DyeTarget::kWeapon) {
                ++worn;
            }
        }
        CHECK(worn == 0);
    }

    // ---- every stripe of every tile names what it paints --------------------
    //
    // The editor builds its selection from the stripe, so a stripe whose target
    // was left at the default would write through the wrong dimension. Armour and
    // weapon stripes are the ones with no reason to differ, which is exactly why
    // they are the ones that would go unnoticed.
    {
        std::vector<ShapeInfo> shapes;
        shapes.push_back(Garment(2, 0, "Cuirass"));
        shapes.push_back(Weapon(WeaponClass::Sword, WeaponHand::Both, 0, "Blade"));
        shapes.push_back(Head(106, "ED.esp", 0x582D, 0, "D_EDHornB"));

        const auto tiles = DyeGrid::Build(shapes, Outfit{});
        for (const auto& t : tiles) {
            for (const auto& st : t.stripes) {
                switch (t.target) {
                    case DyeTarget::kArmour:
                        CHECK(st.target == DyeTarget::kArmour);
                        break;
                    case DyeTarget::kWeapon:
                        CHECK(st.target == DyeTarget::kWeapon);
                        break;
                    case DyeTarget::kEyes:
                        CHECK(st.target == DyeTarget::kEyes);
                        break;
                    case DyeTarget::kHair:
                        // The hair GROUP: its own colour plus any head-part
                        // shapes, so the tile's target answers for stripe 0 only.
                        CHECK(st.target == DyeTarget::kHair ||
                              st.target == DyeTarget::kHeadPart);
                        break;
                    case DyeTarget::kHeadPart:
                        CHECK(st.target == DyeTarget::kHeadPart);
                        break;
                }
            }
        }
    }

    // ---- a strand the hair colour paints gets NO stripe ---------------------
    //
    // ⚠⚠ THIS TEST USED TO ASSERT THE OPPOSITE and the reversal is the user's,
    // 2026-08-31: "we still have many extra slots that we can't click for hairs,
    // that inherit the first hair slot. if we can't click on them then she
    // should just be hidden". The 2026-08-17 call it replaces was that a refused
    // shape is LISTED with its reason rather than hidden, and it stopped holding
    // when the CROSS came off the stripes earlier the same day: a refusal nobody
    // can see is not a rule being taught, it is a row being occupied.
    //
    // ⚠ ONLY kHairOwnColour. It is the one reason that means "already painted,
    // by a control two rows up"; every other refusal still earns its stripe.
    {
        std::vector<ShapeInfo> shapes;
        ShapeInfo strand = Head(3, "Hair.esp", 0x803, 0, "GuanYinping");
        strand.dyeable   = false;
        strand.reason    = DyeSkipReason::kHairOwnColour;
        shapes.push_back(strand);

        const auto tiles = DyeGrid::Build(shapes, Outfit{});
        const DyeTile* hair = nullptr;
        for (const auto& t : tiles) {
            if (t.headGroup == HeadGroup::kHair) {
                hair = &t;
            }
        }
        // The tile is still emitted unconditionally: it is the hair COLOUR's
        // control and has to be reachable even on a character with no hair.
        CHECK(hair != nullptr);
        if (hair) {
            // The colour, and nothing after it.
            CHECK(hair->stripes.size() == 1);
            CHECK(hair->stripes[0].target == DyeTarget::kHair);
            CHECK(hair->stripes[0].dyeable);
        }
    }

    // ---- a hair's DYEABLE shapes still get theirs ---------------------------
    //
    // The guard above must not take the whole group with it. A hair that
    // contributes an ornament the dye can reach keeps that stripe beside the
    // colour, and a strand in the same hair still goes.
    {
        std::vector<ShapeInfo> shapes;
        ShapeInfo ornament = Head(3, "Hair.esp", 0x803, 0, "Pearl");
        ornament.dyeable   = true;
        shapes.push_back(ornament);
        ShapeInfo strand = Head(3, "Hair.esp", 0x803, 1, "Strands");
        strand.dyeable   = false;
        strand.reason    = DyeSkipReason::kHairOwnColour;
        shapes.push_back(strand);
        // ⚠ AND A REFUSAL THAT IS NOT THE HAIR'S KEEPS ITS ROW, so the guard
        // cannot quietly widen into "hide anything undyeable".
        ShapeInfo glow = Head(3, "Hair.esp", 0x803, 2, "Glow");
        glow.dyeable    = false;
        glow.reason     = DyeSkipReason::kGlow;
        shapes.push_back(glow);
        // ⚠⚠ A HAIRLINE. MEASURED off the field log 2026-08-31: every hairline
        // reads `feature HairTint blood=true`, so the decal test claims it and
        // it arrives here as kDecal rather than as a hair reason. It is a
        // decal the player cannot dye and cannot click, and this walk was the
        // one place not asking DyeShapeEarnsAStripe, which has refused that
        // reason since it was written.
        ShapeInfo hairline = Head(3, "Hair.esp", 0x803, 3, "0_HAIRLINE_Female_Elf_Straight");
        hairline.dyeable   = false;
        hairline.reason    = DyeSkipReason::kDecal;
        shapes.push_back(hairline);

        const auto tiles = DyeGrid::Build(shapes, Outfit{});
        const DyeTile* hair = nullptr;
        for (const auto& t : tiles) {
            if (t.headGroup == HeadGroup::kHair) {
                hair = &t;
            }
        }
        CHECK(hair != nullptr);
        if (hair) {
            // The colour, the ornament, the glow. The strand and the hairline
            // are both gone, for two different reasons.
            CHECK(hair->stripes.size() == 3);
            CHECK(hair->stripes[1].label == "Pearl");
            CHECK(hair->stripes[1].dyeable);
            CHECK(hair->stripes[2].label == "Glow");
            CHECK(hair->stripes[2].reason == DyeSkipReason::kGlow);
            for (const auto& st : hair->stripes) {
                CHECK(st.reason != DyeSkipReason::kHairOwnColour);
                CHECK(st.reason != DyeSkipReason::kDecal);
                CHECK(st.label.find("HAIRLINE") == std::string::npos);
            }
        }
    }

    // ---- the shared predicate reaches head parts now ------------------------
    //
    // ⚠ THE POINT IS THAT THIS WALK ASKS IT AT ALL. DyeShapeEarnsAStripe is one
    // rule about what a shape IS and the armour and weapon walks have both read
    // it from the start; the head-part walk did not, which is how a hairline
    // got a swatch. Pinned per reason so a later edit cannot quietly drop the
    // call and pass on the hairline case alone.
    {
        for (const auto reason : { DyeSkipReason::kDecal,
                                   DyeSkipReason::kCharacterColour }) {
            std::vector<ShapeInfo> shapes;
            ShapeInfo refused = Head(3, "Hair.esp", 0x803, 0, "Refused");
            refused.dyeable   = false;
            refused.reason    = reason;
            shapes.push_back(refused);

            const auto tiles = DyeGrid::Build(shapes, Outfit{});
            for (const auto& t : tiles) {
                for (const auto& st : t.stripes) {
                    CHECK(st.label != "Refused");
                }
            }
        }
    }

    // ---- a part that gives TWO shapes indexes within itself -----------------
    //
    // Nothing measured does this: every part in the census gave exactly one
    // geometry. The channel fallback covers it if one ever turns up, and it is the
    // index WITHIN the part, which is why the painter is called once per part.
    {
        std::vector<ShapeInfo> shapes;
        shapes.push_back(Head(106, "ED.esp", 0x1, 0, "HornBase"));
        shapes.push_back(Head(106, "ED.esp", 0x1, 1, "HornTip"));

        Outfit staged;
        staged.SetHeadPartDye(106, StyleRefKey{ "ED.esp", 0x1 }, DyeChannelId::kSecondary,
                              DyeChannel{ true, 7, 8, 9 });

        const auto tiles = DyeGrid::Build(shapes, staged);
        const DyeTile* extras = nullptr;
        for (const auto& t : tiles) {
            if (t.headGroup == HeadGroup::kExtras) {
                extras = &t;
            }
        }
        CHECK(extras != nullptr);
        if (extras) {
            CHECK(extras->stripes.size() == 2);
            // Two stripes, ONE part, so they differ by channel and share a key.
            CHECK(extras->stripes[0].headPart == extras->stripes[1].headPart);
            CHECK(extras->stripes[0].channel == DyeChannelId::kPrimary);
            CHECK(extras->stripes[1].channel == DyeChannelId::kSecondary);
            CHECK(!extras->stripes[0].colour.set);
            CHECK(extras->stripes[1].colour.set && extras->stripes[1].colour.r == 7);
        }
    }

    if (g_failures == 0) {
        std::printf("DyeGridTests: all passed\n");
        return 0;
    }
    std::printf("DyeGridTests: %d failure(s)\n", g_failures);
    return 1;
}
