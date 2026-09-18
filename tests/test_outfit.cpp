// Pure-logic tests for Outfit / SlotMask / EditHistory / FavoriteSet.
// No engine, no RE:: types.
#include "ColorSnap.h"
#include "Favorites.h"
#include "FooterNotice.h"
#include "Outfit.h"
#include "OutfitTabs.h"
#include "PresetPreviewPolicy.h"
#include "SlotClaims.h"
#include "SlotMask.h"

#include <array>
#include <cstdio>
#include <span>
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

// ⚠ THIS SUITE IS ONE ~2000-LINE main() HOLDING DOZENS OF Outfit LOCALS, and
// sizeof(OS::Outfit) is 7536 bytes as of 2026-08-03. That frame is already
// close enough to the default 1 MB stack that it went over during this feature:
// a first cut of the weapon dye storage as three flat arrays took Outfit to
// 11760 bytes and the suite died with 0xC00000FD, STACK_OVERFLOW, before
// running a single CHECK. Sparse storage brought it back to 7536, and the test
// targets reserve 8 MB in CMakeLists.txt so the next struct that grows does not
// have to rediscover this.
//
// ⚠ It presented as a suite with NO output and NO failures, which is exactly
// the mode tools/build_tests.bat's header warns about. build.bat caught it only
// because it checks "%errorlevel% neq 0" rather than "errorlevel 1"; the latter
// is a signed >= test and every NTSTATUS crash code is negative, so it would
// have walked on and printed ALL_DONE. Do not relax either check.
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
        // The two bits engine 24220 reads: 30 (hides the whole head) and
        // 31 (hides the hair head-part). 42 was in this mask once and is not
        // read by that function at all.
        CHECK(IsHeadPartBit(BitForEditorSlot(30)));
        CHECK(IsHeadPartBit(BitForEditorSlot(31)));
        CHECK(!IsHeadPartBit(BitForEditorSlot(42)));
        CHECK(!IsHeadPartBit(BitForEditorSlot(32)));
    }

    {  // RenderedWornMask, reduced. Both inputs now come from the render pass,
       // so there is nothing left to guess and no `displaced` parameter.
        const auto head = MaskForEditorSlot(30);
        const auto hair = MaskForEditorSlot(31);
        const auto body = MaskForEditorSlot(32);

        // Nothing staged: the mask passes through.
        CHECK(RenderedWornMask(head | hair, 0, 0) == (head | hair));

        // THE KROSIS CASE, expressed with measured coverage: the mask spans
        // 30+31 and a Hair / Helmet style displaced it, so the pass reports the
        // mask's whole coverage hidden and only slot 31 styled.
        CHECK(RenderedWornMask(head | hair, /*hidden*/ head | hair,
                               /*styled*/ hair) == hair);

        // A style that covers the head keeps it culled - correct for a
        // full-face look.
        CHECK(RenderedWornMask(head | hair, head | hair, head | hair) == (head | hair));

        // OS-70: hiding a multi-slot helmet returns the head AND the hair.
        CHECK(RenderedWornMask(head | hair, head | hair, 0) == 0u);

        // A styled helmet still hides hair with no real helmet worn.
        CHECK(RenderedWornMask(body, 0, hair) == (body | hair));
    }

    {  // StagedCoverageOf: the OS-84 measurement. Coverage is what is ON the
       // biped, matched by armature identity - never what the ARMO declares and
       // never ApplyArmorAddon's return value, which is not a render signal.
       //
       // Pointers stand in for TESObjectARMA*; only identity matters.
        int skinHeadArma{}, spellKnightArma{}, spellKnightBeastArma{};
        int hoodArma{}, cuirassArma{}, netchArma{};

        std::array<int*, 32> staged{};

        {  // THE STEEL SPELL KNIGHT CASE, the one that settled OS-84. The ARMO
           // declares 30/31/42/43; only the race-valid armature stages, and only
           // onto slot 30. ApplyArmorAddon returned false and it RENDERED.
            staged.fill(nullptr);
            staged[BitForEditorSlot(30)] = &spellKnightArma;
            int* const own[]             = { &spellKnightArma, &spellKnightBeastArma };
            const auto measured =
                StagedCoverageOf<int*>(staged.data(), 32, own, 2);
            // Slot 30 claimed, so 24220 culls the head under the helmet (OS-86).
            CHECK(measured == MaskForEditorSlot(30));
            // And NOT the three slots the ARMO merely declares.
            CHECK((measured & MaskForEditorSlot(31)) == 0);
            CHECK((measured & MaskForEditorSlot(42)) == 0);
        }

        {  // THE RACE-SKIN TRAP. On a bare character the skin's own armature
           // occupies slot 30. A "this slot holds something" test would set the
           // head bit for every style and make every character HEADLESS - worse
           // than the bug being fixed. Identity matching cannot do that.
            staged.fill(nullptr);
            staged[BitForEditorSlot(30)] = &skinHeadArma;
            int* const own[]             = { &spellKnightArma };
            CHECK(StagedCoverageOf<int*>(staged.data(), 32, own, 1) == 0u);
        }

        {  // OS-70b, 'Boiled Netch Leather Helmet': genuinely staged nothing, so
           // it owns no slot. This is the case the old bool-gate was added for,
           // and the measurement still refuses it - one rule, both directions.
            staged.fill(nullptr);
            int* const own[] = { &netchArma };
            CHECK(StagedCoverageOf<int*>(staged.data(), 32, own, 1) == 0u);
        }

        {  // THE OPEN HOOD. Declares 30+31, armature covers only 31. Claiming
           // the declared mask would set slot 30 and cull the WHOLE HEAD; the
           // measurement claims hair and leaves the face alone.
            staged.fill(nullptr);
            staged[BitForEditorSlot(31)] = &hoodArma;
            int* const own[]             = { &hoodArma };
            const auto measured = StagedCoverageOf<int*>(staged.data(), 32, own, 1);
            CHECK(measured == MaskForEditorSlot(31));
            CHECK((measured & MaskForEditorSlot(30)) == 0);  // face survives
        }

        {  // Multi-slot is measured whole: a cuirass armature staging body and
           // forearms claims both, which is what the honesty restore needs.
            staged.fill(nullptr);
            staged[BitForEditorSlot(32)] = &cuirassArma;
            staged[BitForEditorSlot(34)] = &cuirassArma;
            int* const own[]             = { &cuirassArma };
            CHECK(StagedCoverageOf<int*>(staged.data(), 32, own, 1) ==
                  (MaskForEditorSlot(32) | MaskForEditorSlot(34)));
        }

        {  // Degenerate inputs never claim anything: null armature entries, an
           // empty armature list, and null arrays.
            staged.fill(nullptr);
            staged[BitForEditorSlot(30)] = &spellKnightArma;
            int* const withNull[]        = { nullptr, &spellKnightArma };
            CHECK(StagedCoverageOf<int*>(staged.data(), 32, withNull, 2) ==
                  MaskForEditorSlot(30));
            CHECK(StagedCoverageOf<int*>(staged.data(), 32, withNull, 0) == 0u);
            CHECK(StagedCoverageOf<int*>(nullptr, 32, withNull, 2) == 0u);
            CHECK(StagedCoverageOf<int*>(staged.data(), 32, nullptr, 2) == 0u);
            // A slot count past the mask width cannot shift out of range.
            CHECK(StagedCoverageOf<int*>(staged.data(), 999, withNull, 2) ==
                  MaskForEditorSlot(30));
        }
    }

    {  // GenitalDisplacementCull: transmog is the gap between what is worn and
       // what is seen, and TNG decides off the former. Field 2026-08-20: a
       // HIMBO male's genitals came through every styled armour set, but only
       // when no body gear was actually equipped.
        const std::uint32_t body    = MaskForEditorSlot(32);
        const std::uint32_t genital = MaskForEditorSlot(52);
        const std::uint32_t helmet  = MaskForEditorSlot(31);

        // The reported case: a body style over a bare body slot, TNG's piece on 52.
        CHECK(GenitalDisplacementCull(body, /*bodyWorn*/ false, /*genitalObject*/ true) ==
              genital);

        // ⚠ REAL BODY GEAR DECLINES. TNG answered off an actually equipped
        // piece, which is better information than this rule has.
        CHECK(GenitalDisplacementCull(body, /*bodyWorn*/ true, /*genitalObject*/ true) == 0u);

        // ⚠ THE SECOND INPUT IS THE BIPED'S OBJECT, NOT A WORN ARMO. TNG reaches
        // slot 52 through the SKIN, which owns no inventory entry, so reading
        // RealWorn here made the rule decline every time it mattered.
        // Nothing on 52 at all (no TNG/SOS installed): nothing to cull.
        CHECK(GenitalDisplacementCull(body, false, /*genitalObject*/ false) == 0u);

        // A style that is not on the body says nothing about the groin.
        CHECK(GenitalDisplacementCull(helmet, false, true) == 0u);
        CHECK(GenitalDisplacementCull(0u, false, true) == 0u);

        // A body style among others still fires: coverage is a mask, not a slot.
        CHECK(GenitalDisplacementCull(body | helmet, false, true) == genital);
    }
    {  // HeadDisplacementCull: the FIRST style-versus-REAL-WORN-GEAR rule in
       // this suite. Everything above asks what one piece covers; this asks
       // whether one piece evicts another, which is a different question and a
       // far more dangerous one.
       //
       // ⚠ EVERY CASE BELOW IS A GUARD SOMEONE TRIED TO REMOVE. The unguarded
       // version of this rule was refuted on three independent lenses, twice
       // with a path to a defect WORSE than the two-helmets report it fixes.
       // If one of these CHECKs starts failing, the fix is not to relax it.
        const auto head    = MaskForEditorSlot(30);
        const auto hair    = MaskForEditorSlot(31);
        const auto circlet = MaskForEditorSlot(42);
        const auto ears    = MaskForEditorSlot(43);
        const auto body    = MaskForEditorSlot(32);

        {  // THE REPORT ITSELF. A full-face style measured onto slot 30, a real
           // open hood staged on 31 that declares only headgear slots. The hood
           // is culled and nothing else is.
            CHECK(HeadDisplacementCull(/*styled*/ head, /*wornStaged*/ hair,
                                       /*wornDeclared*/ hair, /*shared*/ false) == hair);
        }

        {  // A real FULL HELM under a slot 30 style: it staged head and hair,
           // but the style owns slot 30 now, so only its hair half is culled.
           // Culling slot 30 as well would take the style's own clone with it.
            CHECK(HeadDisplacementCull(head, head | hair, head | hair | circlet | ears,
                                       false) == hair);
        }

        {  // GUARD 1, THE DIRECTION. A style on 31 over a real piece on 30 is
           // NOT the mirror case and must do nothing: a full-face piece
           // legitimately covers a hood, a hood does not cover a face.
            CHECK(HeadDisplacementCull(/*styled*/ hair, head, head, false) == 0u);
            // And the trigger is MEASURED, not declared. A helmet style that
            // declares 30 but only staged onto 31 has not landed on the head,
            // so it displaces nothing.
            CHECK(HeadDisplacementCull(/*styled*/ hair, hair, hair, false) == 0u);
        }

        {  // GUARD 2, THE ENCHANTED-VARIANT COLLISION - the one that ends in a
           // character with NO HEAD. StagedCoverageOf separates pieces only by
           // armature POINTER identity, and the style catalog dedupes enchanted
           // variants by exactly that identity, so 'Iron Helmet of Water-
           // breathing' worn under a styled base 'Iron Helmet' measures as ONE
           // piece. Culling then removes the STYLE's clone while styledCoverage
           // still ORs bit 30 into the published mask, and 24220 culls the head
           // node over an empty slot. Sharing an armature means stand down.
            CHECK(HeadDisplacementCull(head, head, head | hair, /*shared*/ true) == 0u);
            // Even when the measurement looks perfectly ordinary otherwise.
            CHECK(HeadDisplacementCull(head, hair, hair, /*shared*/ true) == 0u);
        }

        {  // GUARD 3, THE HOODED ROBE - the one that ends in an INVISIBLE
           // TORSO, which is worse still because a body-class bit can never be
           // un-culled by the show sweep. Slot 31 is not a headgear-only slot:
           // mage and monk robes claim it for the hood as well as 32 for the
           // body, so armo[kBitHair] is routinely a robe. A piece declaring
           // anything outside the headgear group is not headgear.
            CHECK(HeadDisplacementCull(head, /*wornStaged*/ hair | body,
                                       /*wornDeclared*/ hair | body, false) == 0u);
            // The guard reads the DECLARED mask, so it still stands down when
            // the robe happens to have staged only its hood this pass.
            CHECK(HeadDisplacementCull(head, /*wornStaged*/ hair,
                                       /*wornDeclared*/ hair | body, false) == 0u);
        }

        {  // GUARD 4: never cull what this pass measured as its own. A style
           // spanning head AND hair leaves nothing for the rule to take.
            CHECK(HeadDisplacementCull(head | hair, hair, hair, false) == 0u);
            CHECK(HeadDisplacementCull(head | hair | circlet, hair | circlet,
                                       hair | circlet, false) == 0u);
        }

        {  // GUARD 5: the result never names a slot outside the headgear group,
           // independently of guard 3. Belt and braces on purpose - the two are
           // refuted separately, so a future edit relaxing one must not
           // silently inherit the other's protection.
            const auto out = HeadDisplacementCull(head, /*wornStaged*/ hair | body,
                                                  /*wornDeclared*/ hair, false);
            CHECK((out & body) == 0u);
            CHECK(out == hair);
        }

        {  // Nothing worn on 31 at all: the caller passes zeroes and the rule
           // is a no-op rather than claiming the whole group.
            CHECK(HeadDisplacementCull(head, 0, 0, false) == 0u);
            // And a style that landed nowhere never triggers it.
            CHECK(HeadDisplacementCull(0, hair, hair, false) == 0u);
        }

        {  // The rule's output is always restorable: every bit it can return is
           // inside the headgear group, and none of those is a body-skin slot,
           // so the show sweep can always clear the flag again once the style
           // goes. This is the invariant that makes the cull safe to undo.
            const auto out = HeadDisplacementCull(head, hair | circlet | ears,
                                                  hair | circlet | ears, false);
            CHECK((out & ~kHeadgearSlotMask) == 0u);
            CHECK((out & (MaskForEditorSlot(32) | MaskForEditorSlot(33) |
                          MaskForEditorSlot(37))) == 0u);
        }
    }

    {  // VouchedHeadCoverage: WHICH ENTRY the hide is read from versus which
       // one carries the geometry. The 2026-09-14 field case, plus the guard
       // that stops the fix undoing r25.
        const auto head    = MaskForEditorSlot(30);
        const auto hair    = MaskForEditorSlot(31);
        const auto longer  = MaskForEditorSlot(41);
        const auto circlet = MaskForEditorSlot(42);
        const auto ears    = MaskForEditorSlot(43);
        const auto body    = MaskForEditorSlot(32);

        {  // THE REPORT ITSELF. A styled Ebony Helmet measured onto slot 30, so
           // its clone sits on entry 30 and entry 31 stages nothing at all. The
           // worn Iron Helmet on 31 is NOT a ghost, because the head is covered,
           // and publishing it as one is what left the hair showing through a
           // real helmet four minutes later.
            CHECK(VouchedHeadCoverage(head) == (head | hair));
        }

        {  // THE r25 GUARD, AND IT IS THE ONE THAT MATTERS. A circlet declares 42
           // alone and leaves the hair and the ears showing (field 2026-08-12).
           // A drawing circlet must never vouch for 31: that hands the hair hide
           // back to an invisible helmet and puts the bald character back.
            CHECK(VouchedHeadCoverage(circlet) == circlet);
            CHECK(VouchedHeadCoverage(ears) == ears);
            CHECK(VouchedHeadCoverage(circlet | ears) == (circlet | ears));
            CHECK((VouchedHeadCoverage(circlet | ears) & hair) == 0u);
        }

        {  // ONE DIRECTION, HeadDisplacementCull's asymmetry. A hood drawing on
           // 31 does not cover a face, so it vouches for nothing on 30.
            CHECK(VouchedHeadCoverage(hair) == hair);
            CHECK((VouchedHeadCoverage(hair) & head) == 0u);
        }

        {  // Nothing drawing on the head family vouches for nothing, which IS the
           // r25 case: a worn helmet staging no geometry anywhere stays a ghost
           // and the hair comes back rather than the character going bald.
            CHECK(VouchedHeadCoverage(0u) == 0u);
        }

        {  // The narrowing, guard 5's doctrine. The result feeds a drawn-mask
           // that also carries body bits, so nothing outside the headgear group
           // may survive - a vouched body slot would be an un-restorable hide.
            CHECK((VouchedHeadCoverage(head | body) & body) == 0u);
            CHECK(VouchedHeadCoverage(head | body) == (head | hair));
            CHECK((VouchedHeadCoverage(head) & ~kHeadgearSlotMask) == 0u);
            // 41 is long hair, not headgear, so it neither vouches nor rides along.
            CHECK(VouchedHeadCoverage(longer) == 0u);
            CHECK(VouchedHeadCoverage(head | longer) == (head | hair));
        }

        {  // Already drawing on its own entry: the vouch changes nothing, which
           // is the ordinary real-helmet case, and it is idempotent.
            CHECK(VouchedHeadCoverage(head | hair) == (head | hair));
            CHECK(VouchedHeadCoverage(hair | circlet) == (hair | circlet));
            CHECK(VouchedHeadCoverage(VouchedHeadCoverage(head)) ==
                  VouchedHeadCoverage(head));
        }

        {  // It only ever ADDS the hair bit, never removes a group bit the
           // caller measured, so a vouch can never take a hide away.
            const auto all = head | hair | circlet | ears;
            CHECK((VouchedHeadCoverage(all) & all) == all);
            CHECK(VouchedHeadCoverage(head | circlet) == (head | hair | circlet));
        }
    }
    {  // SharesAnyArmature: guard 2's input. Identity only, both directions,
       // and a null-tolerant walk - a null entry in either list is not a match.
        struct FakeArmo {
            std::vector<int*> armorAddons;
        };
        int ironArma{}, steelArma{}, hoodArma{};

        FakeArmo ironBase{ { &ironArma } };
        FakeArmo ironEnchanted{ { &ironArma } };  // the catalog's dedupe case
        FakeArmo steel{ { &steelArma } };
        FakeArmo hood{ { &hoodArma, nullptr } };
        FakeArmo empty{ {} };
        FakeArmo alsoEmpty{ {} };

        CHECK(SharesAnyArmature<int*>(&ironBase, &ironEnchanted));
        CHECK(SharesAnyArmature<int*>(&ironEnchanted, &ironBase));  // symmetric
        // The SAME record shares with itself whatever its armature list holds -
        // including an empty one. That is the answer guard 2 needs: if the two
        // pointers are equal the measurement cannot separate them at all, so
        // counting armatures would be asking the wrong question.
        CHECK(SharesAnyArmature<int*>(&ironBase, &ironBase));
        CHECK(SharesAnyArmature<int*>(&empty, &empty));
        CHECK(!SharesAnyArmature<int*>(&ironBase, &steel));
        CHECK(!SharesAnyArmature<int*>(&hood, &steel));
        CHECK(!SharesAnyArmature<int*>(&ironBase, &empty));
        // Two DIFFERENT records that each stage nothing share nothing.
        CHECK(!SharesAnyArmature<int*>(&empty, &alsoEmpty));
        CHECK(!SharesAnyArmature<int*>(static_cast<FakeArmo*>(nullptr), &ironBase));
        CHECK(!SharesAnyArmature<int*>(&ironBase, static_cast<FakeArmo*>(nullptr)));
    }

    {  // The hair override is applied on top of the rendered mask and is
       // INDEPENDENT of it: the setting describes the character's hair, not the
       // outfit's coverage, so a coverage miss must not silently drop it.
        const auto hair = MaskForEditorSlot(31);
        const auto body = MaskForEditorSlot(32);

        CHECK(ApplyHairMode(body, HairMode::kAuto) == body);
        CHECK(ApplyHairMode(body, HairMode::kHide) == (body | hair));
        CHECK(ApplyHairMode(body | hair, HairMode::kShow) == body);
        CHECK(ApplyHairMode(body | hair, HairMode::kAuto) == (body | hair));
        // Show wins over a helmet that would otherwise hide the hair.
        CHECK(ApplyHairMode(MaskForEditorSlot(30) | hair, HairMode::kShow) ==
              MaskForEditorSlot(30));
    }

    {  // ComputeSlotClaims: a style owns the row it sits on and every slot its
       // armatures cover.
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[0] = (1u << 0) | (1u << 1) | (1u << 12);  // a helmet spanning 30/31/42

        const auto claims = ComputeSlotClaims(1u << 0, cov);
        CHECK(claims.owner[0] == 0);   // the assigned row owns itself
        CHECK(claims.owner[1] == 0);   // covered by the row-0 style
        CHECK(claims.owner[12] == 0);
        CHECK(claims.owner[2] == SlotClaims::kNoOwner);
        CHECK(claims.IsCovered(1));
        CHECK(!claims.IsCovered(0));   // an owner is not "covered"
        CHECK(claims.IsFree(2));
    }

    {  // Disjoint styles do not interfere.
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[2] = (1u << 2);
        cov[7] = (1u << 7);
        const auto claims = ComputeSlotClaims((1u << 2) | (1u << 7), cov);
        CHECK(claims.owner[2] == 2);
        CHECK(claims.owner[7] == 7);
        CHECK(claims.owner[0] == SlotClaims::kNoOwner);
    }

    {  // LEGACY REPAIR: an outfit saved before coverage existed can hold an
       // overlap. There is no "newest" at load time, so the LOWEST owning row
       // wins and the later one is reported as losing its own row.
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[0] = (1u << 0) | (1u << 1);  // helmet spanning 30/31
        cov[1] = (1u << 1);              // a hood assigned to 31
        const auto claims = ComputeSlotClaims((1u << 0) | (1u << 1), cov);
        CHECK(claims.owner[0] == 0);
        CHECK(claims.owner[1] == 0);     // row 0 won; row 1 lost its own row
        CHECK(claims.Displaced() == (1u << 1));
    }

    {  // An empty outfit claims nothing.
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        const auto claims = ComputeSlotClaims(0, cov);
        for (std::uint32_t b = 0; b < Outfit::kBitCount; ++b) {
            CHECK(claims.IsFree(b));
        }
        CHECK(claims.Displaced() == 0);
    }

    {  // Assignment evicts a style whose editor row the new piece covers.
        Outfit o;
        o.SetStyle(1, StyleRefKey{ "hood.esp", 0x800 });   // a hood on row 31
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[1] = (1u << 1);

        // A helmet spanning 30/31 assigned to row 30 must clear the hood.
        const auto cleared = AssignStyleWithCoverage(
            o, 0, StyleRefKey{ "helm.esp", 0x900 }, (1u << 0) | (1u << 1), cov);

        CHECK(cleared == (1u << 1));
        CHECK(o.EntryFor(0).kind == SlotEntry::Kind::kStyle);
        CHECK(o.EntryFor(0).style.modName == "helm.esp");
        CHECK(o.EntryFor(1).kind == SlotEntry::Kind::kPassthrough);
    }

    {  // Disjoint styles survive an assignment untouched.
        Outfit o;
        o.SetStyle(2, StyleRefKey{ "body.esp", 0x800 });
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[2] = (1u << 2);

        const auto cleared =
            AssignStyleWithCoverage(o, 0, StyleRefKey{ "helm.esp", 0x900 }, (1u << 0), cov);

        CHECK(cleared == 0);
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kStyle);
    }

    {  // Body and Hands may share the auxiliary Forearms slot. That overlap
       // is normal layering, not ownership of each other's editor row, so
       // choosing gauntlets must not erase the cuirass.
        Outfit o;
        o.SetStyle(2, StyleRefKey{ "body.esp", 0x800 });
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[2] = (1u << 2) | (1u << 4);  // Body + Forearms

        const auto cleared = AssignStyleWithCoverage(
            o, 3, StyleRefKey{ "hands.esp", 0x900 },
            (1u << 3) | (1u << 4), cov);  // Hands + Forearms

        CHECK(cleared == 0);
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kStyle);
        CHECK(o.EntryFor(3).kind == SlotEntry::Kind::kStyle);
    }

    {  // H2: a style whose coverage lands on a HIDDEN row clears that hide. The
       // style you just picked is what you asked for, and half a rendered piece
       // is not representable.
        Outfit o;
        o.SetHide(1);
        std::array<std::uint32_t, Outfit::kBitCount> cov{};

        const auto cleared = AssignStyleWithCoverage(
            o, 0, StyleRefKey{ "helm.esp", 0x900 }, (1u << 0) | (1u << 1), cov);

        CHECK(cleared == (1u << 1));
        CHECK(o.EntryFor(1).kind == SlotEntry::Kind::kPassthrough);
    }

    {  // Re-assigning the same row is not self-eviction.
        Outfit o;
        o.SetStyle(0, StyleRefKey{ "a.esp", 0x1 });
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[0] = (1u << 0) | (1u << 1);

        const auto cleared = AssignStyleWithCoverage(
            o, 0, StyleRefKey{ "b.esp", 0x2 }, (1u << 0) | (1u << 1), cov);

        CHECK(cleared == 0);
        CHECK(o.EntryFor(0).style.modName == "b.esp");
    }

    {  // One wide piece can evict SEVERAL rows at once, and a mixture of kinds.
       // The loop handles it, but nothing above reached the case: every other
       // block collides with exactly one row, so a bug that stopped at the
       // first eviction would have passed them all.
       // (Comments here name biped SLOTS, code uses bits; bit = slot - 30.)
        Outfit o;
        o.SetStyle(1, StyleRefKey{ "hood.esp", 0x1 });   // slot 31
        o.SetStyle(12, StyleRefKey{ "circlet.esp", 0x2 });  // slot 42
        o.SetHide(11);                                    // slot 41, long hair
        o.SetStyle(2, StyleRefKey{ "body.esp", 0x3 });   // slot 32, uninvolved

        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[1]  = (1u << 1);
        cov[12] = (1u << 12);
        cov[2]  = (1u << 2);

        // A full helm spanning 30/31/41/42.
        const auto cleared = AssignStyleWithCoverage(
            o, 0, StyleRefKey{ "fullhelm.esp", 0x9 },
            (1u << 0) | (1u << 1) | (1u << 11) | (1u << 12), cov);

        CHECK(cleared == ((1u << 1) | (1u << 11) | (1u << 12)));
        CHECK(o.EntryFor(1).kind == SlotEntry::Kind::kPassthrough);
        CHECK(o.EntryFor(11).kind == SlotEntry::Kind::kPassthrough);
        CHECK(o.EntryFor(12).kind == SlotEntry::Kind::kPassthrough);
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kStyle);  // untouched
        CHECK(o.EntryFor(0).style.modName == "fullhelm.esp");
    }

    {  // H1/H3: hiding one row of a multi-slot piece hides the whole piece.
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[0] = (1u << 0) | (1u << 1);   // a mask spanning slots 30/31

        CHECK(ExpandHideOverCoverage(1u << 0, cov) == ((1u << 0) | (1u << 1)));
        CHECK(ExpandHideOverCoverage(1u << 1, cov) == ((1u << 0) | (1u << 1)));
    }

    {  // A hide on a row with no multi-slot piece expands to itself.
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[5] = (1u << 5);
        CHECK(ExpandHideOverCoverage(1u << 5, cov) == (1u << 5));
        CHECK(ExpandHideOverCoverage(1u << 9, cov) == (1u << 9));  // no coverage known
    }

    {  // Several hides union.
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[2] = (1u << 2) | (1u << 3) | (1u << 4);
        CHECK(ExpandHideOverCoverage((1u << 2) | (1u << 7), cov) ==
              ((1u << 2) | (1u << 3) | (1u << 4) | (1u << 7)));
    }

    {  // Nothing hidden expands to nothing.
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[0] = (1u << 0) | (1u << 1);
        CHECK(ExpandHideOverCoverage(0, cov) == 0);
    }

    {  // When TWO rows both claim the hidden slot, hide BOTH pieces. This is
       // the deliberate difference from the sibling functions, which each
       // resolve a contested slot to one owner (lowest row for repair, newest
       // for assignment). A hide has no assignment to corrupt and no way to
       // render half a piece, so over-hiding is the safe direction: whichever
       // of the two the engine actually staged, it is gone.
       //
       // Only reachable through the legacy-overlap path - assignment prevents
       // it going forward - but it was shipped untested, and a regression to
       // first-match would pass every other block in this file.
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[0] = (1u << 0) | (1u << 1);   // a helm spanning slots 30/31
        cov[1] = (1u << 1) | (1u << 11);  // a hood spanning 31/41, both claim 31

        CHECK(ExpandHideOverCoverage(1u << 1, cov) ==
              ((1u << 0) | (1u << 1) | (1u << 11)));
    }

    {  // Hair mode defaults to Auto, so every outfit written before it existed
       // behaves exactly as it did.
        Outfit o;
        CHECK(o.hair == HairMode::kAuto);
        CHECK(static_cast<std::uint8_t>(HairMode::kAuto) == 0);

        const auto d = ComputeDisplaySet(o, 0);
        CHECK(d.hair == HairMode::kAuto);
    }

    {  // The mode travels in DisplaySet so the follower path gets it for free.
        Outfit o;
        o.hair = HairMode::kHide;
        CHECK(ComputeDisplaySet(o, 0).hair == HairMode::kHide);
        o.hair = HairMode::kShow;
        CHECK(ComputeDisplaySet(o, 0).hair == HairMode::kShow);
    }

    {  // The body page's bare view. NULLOPT is the OFF state and has to leave
       // the DisplaySet byte for byte as it arrived, because it is what every
       // frame with the view off passes through.
        Outfit o;
        o.SetStyle(kBitBody, StyleRefKey{ "a.esp", 0x1 });
        const auto base = ComputeDisplaySet(o, 0);
        const auto off  = BareBodyDisplay(base, std::nullopt);
        CHECK(off.styleMask == base.styleMask);
        CHECK(off.hideMask == base.hideMask);
        CHECK(off.hiddenBodySkinMask == base.hiddenBodySkinMask);
        CHECK(off.hiddenAttachmentMask == base.hiddenAttachmentMask);
        CHECK(off.hair == base.hair);
    }

    {  // ON: the styles go with the gear. A style left standing on a slot the
       // actor wears nothing on would be the one garment the bare view missed,
       // which is why styleMask is CLEARED rather than masked by the hideable
       // set.
        Outfit o;
        o.SetStyle(kBitBody, StyleRefKey{ "a.esp", 0x1 });
        o.SetStyle(kBitCirclet, StyleRefKey{ "a.esp", 0x2 });  // nothing worn there
        const auto worn = MaskForEditorSlot(32) | MaskForEditorSlot(37);
        const auto d    = BareBodyDisplay(ComputeDisplaySet(o, 0), worn);
        CHECK(d.styleMask == 0);
        CHECK((d.hideMask & worn) == worn);
    }

    {  // ⚠⚠ THE REGRESSION. An armed view over ZERO worn coverage still takes
       // the styles off, because a character wearing no real armour is dressed
       // entirely in ours and is the case this feature exists for. Measured in
       // the field 2026-08-16: the old signature could not tell this from "the
       // view is off" and the transmog stayed on.
        Outfit o;
        o.SetStyle(kBitBody, StyleRefKey{ "a.esp", 0x1 });
        o.SetStyle(kBitFeet, StyleRefKey{ "a.esp", 0x2 });
        const auto d = BareBodyDisplay(ComputeDisplaySet(o, 0), 0u);
        CHECK(d.styleMask == 0);
        CHECK(d.hideMask == 0);  // nothing real to hide, and nothing invented
        CHECK(d.hiddenBodySkinMask == 0);
        CHECK(d.hiddenAttachmentMask == 0);
    }

    {  // NULLOPT over the same outfit leaves the styles alone. The pair above
       // and this one are the whole of the distinction.
        Outfit o;
        o.SetStyle(kBitBody, StyleRefKey{ "a.esp", 0x1 });
        const auto d = BareBodyDisplay(ComputeDisplaySet(o, 0), std::nullopt);
        CHECK(d.styleMask != 0);
    }

    {  // The two submasks are RE-DERIVED, not left holding what ComputeDisplaySet
       // put there. A hide bit that does not fall out into its submask reaches
       // neither the skin re-apply nor the node cull, so it hides nothing at all.
        Outfit     o;
        const auto worn = MaskForEditorSlot(32) |   // body, skin class
                          MaskForEditorSlot(37) |   // feet, skin class
                          MaskForEditorSlot(35);    // amulet, attachment class
        const auto d = BareBodyDisplay(ComputeDisplaySet(o, 0), worn);
        CHECK(d.hiddenBodySkinMask ==
              (MaskForEditorSlot(32) | MaskForEditorSlot(37)));
        CHECK(d.hiddenAttachmentMask == MaskForEditorSlot(35));
        CHECK((d.hiddenBodySkinMask | d.hiddenAttachmentMask) == d.hideMask);
    }

    {  // An existing hide SURVIVES the bare view rather than being replaced by
       // it. The outfit's own hides are the player's, and leaving the page must
       // put back exactly what they had, not a subset of it.
        Outfit o;
        o.SetHide(kBitHair);
        const auto d = BareBodyDisplay(ComputeDisplaySet(o, 0),
                                       MaskForEditorSlot(32));
        CHECK((d.hideMask & MaskForEditorSlot(31)) != 0);
        CHECK((d.hideMask & MaskForEditorSlot(32)) != 0);
    }

    {  // Hair is not worn gear. A bare body is a question about the body, and
       // answering it by making the character bald answers something else.
        Outfit o;
        o.hair = HairMode::kShow;
        CHECK(BareBodyDisplay(ComputeDisplaySet(o, 0), MaskForEditorSlot(32)).hair ==
              HairMode::kShow);
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

    {  // Auto and bRequireWornForStyles: a style with nothing under it shows
       // nothing, so it is not a visible torso
       //
       // ⚠⚠ FIELD 2026-08-29: "ORefit does not update when you unequip your
       // gear; it uses the ORefit status for your equipped outfit pieces even
       // though your character now has nothing equipped." Auto counted the
       // styled bits whether or not they would render, so unequipping left
       // ORefit forced ON against a bare body.
        const auto body  = MaskForEditorSlot(32);
        const auto chest = MaskForEditorSlot(46);

        // Setting off: unchanged, a styled torso reads as clothed on its own.
        CHECK(ResolveAutoORefit(ORefitMode::kDefault, body, 0, 0, true, false) ==
              ORefitMode::kForceOn);

        // Setting on, nothing equipped: the style cannot render, so ForceOff.
        CHECK(ResolveAutoORefit(ORefitMode::kDefault, body, 0, 0, true, true) ==
              ORefitMode::kForceOff);

        // Setting on, real gear under the styled slot: it renders, ForceOn.
        CHECK(ResolveAutoORefit(ORefitMode::kDefault, body, 0, body, true, true) ==
              ORefitMode::kForceOn);

        // Setting on, real gear on a DIFFERENT torso slot the outfit leaves
        // alone: the style still cannot show, but the passthrough chest is
        // genuinely worn and visible, so the torso is clothed either way.
        CHECK(ResolveAutoORefit(ORefitMode::kDefault, body, 0, chest, true, true) ==
              ORefitMode::kForceOn);

        // An explicit choice is still explicit, setting or no setting.
        CHECK(ResolveAutoORefit(ORefitMode::kForceOn, body, 0, 0, true, true) ==
              ORefitMode::kForceOn);
    }

    {  // Installed and Fitting Room custom bodies are mutually exclusive body
       // references, and changing either one is a body edit.
        Outfit base;
        Outfit custom = base;
        custom.customBodyPresetId = "0123456789abcdef";
        CHECK(BodyDiffers(base, custom));
        CHECK(AnyBodyEntry(custom));

        Outfit installed = custom;
        installed.customBodyPresetId.clear();
        installed.obodyPreset = "Installed Body";
        CHECK(BodyDiffers(custom, installed));
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

    {  // computeDisplaySet: the two hide submasks partition hideMask. The worn
       // mask shim takes hideMask whole (RenderedWornMask), so there is no
       // third head-part submask to derive.
        Outfit o;
        o.SetHide(1);                   // helmet (attachment)
        o.SetHide(2);                   // body (body-skin)
        const auto d = ComputeDisplaySet(o, /*blocklist*/ 0);
        CHECK(d.hideMask == ((1u << 1) | (1u << 2)));
        CHECK(d.hiddenBodySkinMask == (1u << 2));
        CHECK(d.hiddenAttachmentMask == (1u << 1));   // helmet culls its node too
        CHECK((d.hiddenBodySkinMask | d.hiddenAttachmentMask) == d.hideMask);
        CHECK((d.hiddenBodySkinMask & d.hiddenAttachmentMask) == 0u);
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
        CHECK(!CanApplyStyleBit(kBitShield, 0, MaskForEditorSlot(39)));
        CHECK(CanApplyStyleBit(kBitShield, MaskForEditorSlot(39), MaskForEditorSlot(39)));
        CHECK(!StyleRequiresWornItem(kBitBody));
        CHECK(CanApplyStyleBit(kBitBody, 0, MaskForEditorSlot(32)));

        // Regression: shield and off-hand weapon share biped object 9. When a
        // requested shield style is rejected because no real shield is worn,
        // slot 9 must not enter the armor-honesty restore mask. Otherwise the
        // naked-skin ARMO replaces the off-hand WEAP pointer and Skyrim crashes
        // on the next UpdateEquipment pass.
        std::uint32_t appliedCoverage = 0;
        if (CanApplyStyleBit(kBitShield, /*realWornMask*/ 0, MaskForEditorSlot(39))) {
            appliedCoverage |= MaskForEditorSlot(39);
        }
        CHECK((PostPassArmorRestoreMask(/*hideMask*/ 0, appliedCoverage) &
               MaskForEditorSlot(39)) == 0);
        CHECK(PostPassArmorRestoreMask(MaskForEditorSlot(31),
                                       MaskForEditorSlot(32)) ==
              (MaskForEditorSlot(31) | MaskForEditorSlot(32)));

        // ---- conjuring a shield to PREVIEW one (field 2026-08-11 evening) --
        //
        // ⚠⚠ THE FREE-OBJECT TERM IS THE CRASH GUARD FROM THE BLOCK ABOVE,
        // NOT A NEW CONVENIENCE. Everything the regression above describes is
        // still true: with object 9 occupied by an off-hand WEAP, a shield
        // that reaches the restore mask replaces that weapon's item with the
        // skin ARMO and the next UpdateEquipment crashes. What changed is that
        // "no real shield worn" and "object 9 is busy" are now separate
        // questions, and only the SECOND one is fatal.
        // ⚠⚠ AND THE SECOND TERM IS "SOMEBODY IS LOOKING". The first cut had
        // only the free-object test, so a conjured shield survived the editor
        // closing and the player walked out of Fitting Room carrying a shield
        // they did not own (field 2026-08-11 evening). Relaxing the rule for
        // BROWSING must not relax it for play - that persistence is the exact
        // thing "never a way to conjure one" was protecting against.
        // The second term is "the SHIELD ROW is the one on screen", not "the
        // editor is open". Gating on the editor left a conjured shield on the
        // arm after the player moved to another slot, for the rest of the
        // session - a preview belongs to the thing being previewed.
        CHECK(ShieldStyleMayConjure(/*free*/ true, /*rowActive*/ true));
        CHECK(!ShieldStyleMayConjure(/*free*/ true, /*rowActive*/ false));
        CHECK(!ShieldStyleMayConjure(/*free*/ false, /*previewing*/ true));
        CHECK(!ShieldStyleMayConjure(/*free*/ false, /*previewing*/ false));

        // Object 9 free, no real shield: it renders now. This is the feature.
        CHECK(CanApplyStyleBit(kBitShield, 0, MaskForEditorSlot(39), false,
                               ShieldStyleMayConjure(true, true)));
        // ...and stops the moment the editor does.
        CHECK(!CanApplyStyleBit(kBitShield, 0, MaskForEditorSlot(39), false,
                                ShieldStyleMayConjure(true, false)));
        // Object 9 BUSY and no real shield: refused, exactly as before. An
        // off-hand weapon or a torch lives there and must not be built over.
        CHECK(!CanApplyStyleBit(kBitShield, 0, MaskForEditorSlot(39), false,
                                ShieldStyleMayConjure(false, true)));

        // ⚠ THE DEFAULT MUST STAY REFUSING. Every existing caller omits the
        // argument, and a caller that cannot measure the biped must not be
        // able to conjure a shield by forgetting one.
        CHECK(!CanApplyStyleBit(kBitShield, 0, MaskForEditorSlot(39)));
        CHECK(!CanApplyStyleBit(kBitShield, 0, MaskForEditorSlot(39), false));

        // A real shield still renders whatever object 9 says, because then it
        // is a replacement rather than a conjuring - the original rule.
        CHECK(CanApplyStyleBit(kBitShield, MaskForEditorSlot(39),
                               MaskForEditorSlot(39), false, false));

        // Nothing here touches any other slot: the free-object term is
        // consulted only where StyleRequiresWornItem is true.
        CHECK(CanApplyStyleBit(kBitBody, 0, MaskForEditorSlot(32), false, false));
        CHECK(CanApplyStyleBit(kBitBody, 0, MaskForEditorSlot(32), false, true));

        // And the restore mask now legitimately carries bit 9 - because the
        // style APPLIED. That is safe only because it could not have applied
        // over an occupied object, which is the whole point of the guard.
        std::uint32_t conjured = 0;
        if (CanApplyStyleBit(kBitShield, 0, MaskForEditorSlot(39), false,
                             ShieldStyleMayConjure(true, true))) {
            conjured |= MaskForEditorSlot(39);
        }
        CHECK((PostPassArmorRestoreMask(0, conjured) & MaskForEditorSlot(39)) != 0);
    }

    {  // bRequireWornForStyles asks about the style's COVERAGE, not its anchor
        // A robe that dresses body, hands and feet from one ARMO.
        const std::uint32_t robe = MaskForEditorSlot(32) | MaskForEditorSlot(33) |
                                   MaskForEditorSlot(37);
        const std::uint32_t cuirass   = MaskForEditorSlot(32);
        const std::uint32_t gauntlets = MaskForEditorSlot(33);
        const std::uint32_t helmet    = MaskForEditorSlot(30);

        // Off, nothing changes: a style still renders over bare slots.
        CHECK(CanApplyStyleBit(kBitBody, 0, robe, false));

        // On, and wearing nothing at all: no gear under any of it, so no look.
        CHECK(!CanApplyStyleBit(kBitBody, 0, robe, true));
        // On, with real gear under the slot the style is anchored to.
        CHECK(CanApplyStyleBit(kBitBody, cuirass, robe, true));

        // ⚠ THE REGRESSION. The gate used to test the ANCHOR BIT while the
        // engine staged the ARMO across its whole coverage, so a multi-slot
        // style anchored on a bare slot was thrown away even though the actor
        // had real gear under another slot the same style covers. Gauntlets on,
        // cuirass off, robe anchored at the body: this must still dress.
        CHECK(CanApplyStyleBit(kBitBody, gauntlets, robe, true));

        // Gear that the style does not cover does not count as underneath it.
        CHECK(!CanApplyStyleBit(kBitBody, helmet, robe, true));

        // ⚠ The shield rule is structural and stays STRICT under the widened
        // one. A shield transmog replaces an equipped shield and must never
        // conjure one, so it asks about slot 39 itself - never about whatever
        // else its ARMO happens to cover. Without this, a shield ARMO that also
        // declared a body slot would ride in on a worn cuirass and hand the
        // player a shield they do not have.
        const std::uint32_t oddShield = MaskForEditorSlot(39) | MaskForEditorSlot(32);
        CHECK(!CanApplyStyleBit(kBitShield, cuirass, oddShield, true));
        CHECK(!CanApplyStyleBit(kBitShield, cuirass, oddShield, false));
        CHECK(CanApplyStyleBit(kBitShield, cuirass | MaskForEditorSlot(39), oddShield, true));
    }

    {  // the worn rule stands down for a PREVIEW, and for nothing else
        // In lore friendly (bRequireWornForStyles on) a preset click stages
        // the whole look and the render gate refused every piece with no real
        // gear under it, so a character wearing only skin showed nothing while
        // the Presets page said "Trying on" (field 2026-09-02). While the
        // editor is open the look is a picture the player asked to see; the
        // close edge rebuilds and the rule is back. The shape is
        // ShieldStyleMayConjure's second term, applied to the worn-everywhere
        // test alone.
        const std::uint32_t robe = MaskForEditorSlot(32) | MaskForEditorSlot(33) |
                                   MaskForEditorSlot(37);
        const std::uint32_t cuirass = MaskForEditorSlot(32);
        const std::uint32_t shield  = MaskForEditorSlot(39);

        // Wearing nothing, rule on: refused outside, drawn while previewing.
        CHECK(!CanApplyStyleBit(kBitBody, 0, robe, true, false, false));
        CHECK(CanApplyStyleBit(kBitBody, 0, robe, true, false, true));
        // ⚠ THE DEFAULT STAYS REFUSING. A caller that omits the argument is a
        // caller outside the editor, and it must not draw a look out of
        // nothing by forgetting one.
        CHECK(!CanApplyStyleBit(kBitBody, 0, robe, true));
        CHECK(!CanApplyStyleBit(kBitBody, 0, robe, true, false));
        // Rule off: previewing changes nothing, it was drawing already.
        CHECK(CanApplyStyleBit(kBitBody, 0, robe, false, false, true));
        CHECK(CanApplyStyleBit(kBitBody, 0, robe, false, false, false));
        // Real gear under it: drawn either way.
        CHECK(CanApplyStyleBit(kBitBody, cuirass, robe, true, false, true));
        CHECK(CanApplyStyleBit(kBitBody, cuirass, robe, true, false, false));

        // ⚠ ONLY the worn-everywhere test relaxes. The shield's own term is
        // the crash guard and still needs object 9 free: previewing with no
        // real shield and no conjure verdict is refused as before...
        CHECK(!CanApplyStyleBit(kBitShield, 0, shield, true, false, true));
        CHECK(!CanApplyStyleBit(kBitShield, 0, shield, false, false, true));
        // ...and with the object free and the row up, the shield the worn
        // rule used to refuse inside the editor draws while previewing.
        CHECK(CanApplyStyleBit(kBitShield, 0, shield, true,
                               ShieldStyleMayConjure(true, true), true));
        // A shield ARMO that also declares a body slot still asks about slot
        // 39 itself, previewing or not.
        const std::uint32_t oddShield = shield | cuirass;
        CHECK(!CanApplyStyleBit(kBitShield, cuirass, oddShield, true, false, true));
        CHECK(CanApplyStyleBit(kBitShield, cuirass | shield, oddShield, true, false, true));
    }

    {  // the Presets note: how many pieces draw in the fitting room only
        // The status line says "Nothing is worn under N of its M pieces" when
        // the worn rule is on and the preview alone is drawing some of the
        // look. Counted with the gate itself, previewing and not, so the note
        // and the render pass can only agree.
        const std::uint32_t robe = MaskForEditorSlot(32) | MaskForEditorSlot(33) |
                                   MaskForEditorSlot(37);
        const std::uint32_t gauntlets = MaskForEditorSlot(33);
        std::array<std::uint32_t, kBitCount> coverage{};
        coverage[kBitBody]    = robe;                    // sleeves reach the hands
        coverage[kBitHands]   = gauntlets;
        coverage[kBitHead]    = MaskForEditorSlot(30);
        coverage[kBitShield]  = MaskForEditorSlot(39);
        coverage[kBitCirclet] = 0;  // staged, but its plugin is missing
        const std::uint32_t styles = (1u << kBitBody) | (1u << kBitHands) |
                                     (1u << kBitHead) | (1u << kBitShield) |
                                     (1u << kBitCirclet);

        // Gauntlets on, rule on: the robe and the gauntlet style have real
        // gear under them, the helmet does not, the shield never draws at all
        // without a real one (so it is not THIS note's business), and the
        // unresolved circlet is not a piece.
        auto t = TallyPreviewOnly(styles, coverage, gauntlets, true, false);
        CHECK(t.pieces == 4);
        CHECK(t.bare == 1);
        // Wearing nothing: every armour piece is a preview.
        t = TallyPreviewOnly(styles, coverage, 0, true, false);
        CHECK(t.pieces == 4);
        CHECK(t.bare == 3);
        // Rule off: nothing is hidden outside, so nothing to say.
        t = TallyPreviewOnly(styles, coverage, 0, false, false);
        CHECK(t.pieces == 4);
        CHECK(t.bare == 0);
        // Object 9 free and the row up: the conjured shield draws here and
        // vanishes outside, which is exactly a preview-only piece.
        t = TallyPreviewOnly(styles, coverage, 0, true, ShieldStyleMayConjure(true, true));
        CHECK(t.bare == 4);
        // No styles at all.
        t = TallyPreviewOnly(0, coverage, 0, true, false);
        CHECK(t.pieces == 0);
        CHECK(t.bare == 0);
    }

    {  // library: twenty saved slots, one active, rename, activate/deactivate
        // ⚠ PINNED ON PURPOSE, and it caught this change. The cap is not a
        // free number: an older build refuses the WHOLE 'LIBR' record of a save
        // holding more than its own cap, so moving it strands every save that
        // uses the new room on every build older than the move. Outfit.h has
        // the account. Anyone editing this line should have read it.
        CHECK(kMaxOutfits == 20);
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

        follower.obodyPreset.clear();
        follower.customBodyPresetId = "0123456789abcdef";
        const auto customMannequin = MakeMannequinPreview(follower, 0);
        CHECK(customMannequin.obodyPreset.empty());
        CHECK(customMannequin.customBodyPresetId == "0123456789abcdef");
        CHECK(follower.customBodyPresetId == "0123456789abcdef");

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
        CHECK(PendingCost(/*hasPendingEdits*/ false,
                          /*charging*/ true,
                          /*changedSlots*/ 4,
                          /*costPerSlot*/ 100,
                          /*changedDyes*/ 3,
                          /*costPerDye*/ 50,
                          /*changedLooks*/ 2,
                          /*costPerLook*/ 100) == 0);
        CHECK(PendingCost(/*hasPendingEdits*/ true,
                          /*charging*/ true,
                          /*changedSlots*/ 4,
                          /*costPerSlot*/ 100,
                          /*changedDyes*/ 0,
                          /*costPerDye*/ 50,
                          /*changedLooks*/ 0,
                          /*costPerLook*/ 100) == 400);
        CHECK(PendingCost(/*hasPendingEdits*/ true,
                          /*charging*/ false,
                          /*changedSlots*/ 4,
                          /*costPerSlot*/ 100,
                          /*changedDyes*/ 3,
                          /*costPerDye*/ 50,
                          /*changedLooks*/ 2,
                          /*costPerLook*/ 100) == 0);
    }

    {  // dye channels are billed alongside slots, and either term can carry the
       // whole bill on its own. A dye-only edit changes no slot at all, so
       // pricing slots alone made recolouring a whole outfit free.
        CHECK(PendingCost(true, true, 0, 100, 3, 50, 0, 100) == 150);   // dye only
        CHECK(PendingCost(true, true, 4, 100, 3, 50, 0, 100) == 550);   // both terms
        // A zero rate on one term disables that term and leaves the other
        // alone, which is how gold ships: iGoldPerDye defaults to 0 so no
        // existing gold save is handed a bill it never used to pay.
        CHECK(PendingCost(true, true, 4, 100, 3, 0, 0, 100) == 400);
        CHECK(PendingCost(true, true, 4, 0, 3, 50, 0, 100) == 150);
        // The counts are bounded by the outfit, so the widest real bill against
        // a hand-edited INI still sits far inside u64.
        CHECK(PendingCost(true, true, 32, 4294967295u, 256, 4294967295u, 0, 0) ==
              std::uint64_t(32) * 4294967295u + std::uint64_t(256) * 4294967295u);
    }

    {  // ---- the third term: a LOOK ----------------------------------------
       // ⚠ SIX DIMENSIONS WERE FREE, AND THE HEADER SAID SO WITHOUT FIXING IT.
       // EditorGate::StillDirty names body, hair and dye as dimensions that
       // stage with zero changed slots, and pricing read the slot count alone,
       // so a body preset, hair visibility, hair colour, hair style, eyes and
       // brows all cost nothing. A character could be rebuilt head to toe for
       // free as long as no slot was touched (user 2026-08-07).
        CHECK(PendingCost(true, true, 0, 100, 0, 50, 1, 100) == 100);  // look only
        CHECK(PendingCost(true, true, 2, 100, 1, 50, 3, 100) == 200 + 50 + 300);
        // Each term still disables independently on a zero rate.
        CHECK(PendingCost(true, true, 0, 100, 0, 50, 3, 0) == 0);
        // And a look costs nothing when there is no pending edit or no charge,
        // exactly like the other two.
        CHECK(PendingCost(false, true, 0, 100, 0, 50, 3, 100) == 0);
        CHECK(PendingCost(true, false, 0, 100, 0, 50, 3, 100) == 0);
    }

    {  // ---- what counts as a look -----------------------------------------
       // The six dimensions that stage with zero changed slots, each worth one
       // unit. Counted rather than reduced to a bool so a player who changed
       // their body AND their eyes pays for two things, the same way two styled
       // slots cost twice one.
        OS::Outfit base, staged;
        CHECK(OS::ChangedLookCount(base, staged) == 0);

        staged.obodyPreset = "CBBE Curvy";
        CHECK(OS::ChangedLookCount(base, staged) == 1);
        staged.orefit = static_cast<OS::ORefitMode>(1);
        // ⚠ STILL ONE. The body preset and its ORefit mode are the same
        // dimension: BodyDiffers answers for both, and billing them apart would
        // charge twice for one trip to the body page.
        CHECK(OS::ChangedLookCount(base, staged) == 1);

        staged.hair = OS::HairMode::kHide;                      // visibility
        CHECK(OS::ChangedLookCount(base, staged) == 2);
        staged.hairStyle = OS::StyleRefKey{ "Hair.esp", 0x99u };
        CHECK(OS::ChangedLookCount(base, staged) == 3);
        staged.hairTint.set = true;
        staged.hairTint.r   = 40;
        CHECK(OS::ChangedLookCount(base, staged) == 4);
        staged.eyes = OS::StyleRefKey{ "Eyes.esp", 0x11u };
        CHECK(OS::ChangedLookCount(base, staged) == 5);
        staged.brows = OS::StyleRefKey{ "Brows.esp", 0x22u };
        // ⚠ EYES AND BROWS COUNT SEPARATELY even though HeadPartsDiffer answers
        // for the pair. They are two picks off two lists, so folding them into
        // one unit would make the second free.
        CHECK(OS::ChangedLookCount(base, staged) == 6);
        // The eye COLOUR is its own look, billed exactly as the hair colour
        // is; it shipped unbilled the first time, the same forgotten dimension
        // as every entry in this list before it.
        staged.eyeTint.set = true;
        staged.eyeTint.g   = 176;
        CHECK(OS::ChangedLookCount(base, staged) == 7);
        // And the sclera is the eighth, separate from the iris for the reason
        // eyes and brows are separate: two picks off two rows.
        staged.scleraTint.set = true;
        staged.scleraTint.r   = 200;
        CHECK(OS::ChangedLookCount(base, staged) == 8);
        // ⚠⚠ AND THE SECOND EYE COLOUR IS THE NINTH, which it was not when it
        // shipped. EyeTintDiffers learned it and this did not, so the edit
        // reached the screen and enabled Apply while costing nothing. That is
        // the exact asymmetry this function's own header warns about: a
        // dimension in one list and not the other is either uncommittable or
        // free, and both have now happened.
        staged.eyeTint2.set = true;
        staged.eyeTint2.b   = 90;
        CHECK(OS::ChangedLookCount(base, staged) == 9);

        // Symmetric: editing a value back to the committed one stops costing.
        OS::Outfit same = staged;
        CHECK(OS::ChangedLookCount(staged, same) == 0);

        // And putting it back is free, the rule every dimension here follows.
        OS::Outfit reverted   = staged;
        reverted.eyeTint2     = OS::HairTint{};
        CHECK(OS::ChangedLookCount(staged, reverted) == 0);
    }

    {  // ---- putting something BACK is free --------------------------------
       // ⚠ THE BILL IS FOR CHANGING A LOOK, NOT FOR ABANDONING ONE (user
       // 2026-08-07). Reverting a dimension to the character's own is the
       // player giving something up, and charging for it means an unaffordable
       // outfit can also be unaffordable to undo, which is a trap rather than
       // an economy. The diff is symmetric and cannot tell the two apart on its
       // own, so the direction is asked explicitly: a STAGED value that names
       // nothing is a revert.
        OS::Outfit dressed;
        dressed.obodyPreset = "CBBE Curvy";
        dressed.hairStyle   = OS::StyleRefKey{ "Hair.esp", 0x99u };
        dressed.eyes        = OS::StyleRefKey{ "Eyes.esp", 0x11u };
        dressed.brows       = OS::StyleRefKey{ "Brows.esp", 0x22u };
        dressed.hair        = OS::HairMode::kHide;
        dressed.hairTint.set = true;
        dressed.hairTint.r   = 40;
        // Six dimensions in, six units.
        CHECK(OS::ChangedLookCount(OS::Outfit{}, dressed) == 6);

        // All six back out again, and it costs nothing.
        OS::Outfit bare;
        CHECK(OS::ChangedLookCount(dressed, bare) == 0);

        // One at a time, so a partial revert is free per dimension rather than
        // only when everything goes at once.
        OS::Outfit dropHair = dressed;
        dropHair.hairStyle  = OS::StyleRefKey{};
        CHECK(OS::ChangedLookCount(dressed, dropHair) == 0);

        // ⚠ AND SWAPPING ONE NAMED LOOK FOR ANOTHER STILL COSTS. Free reverts
        // must not become a free route to a different look: clear then pick
        // would otherwise be two free steps where picking directly costs one.
        // Only the step that names NOTHING is free.
        OS::Outfit swapped = dressed;
        swapped.hairStyle  = OS::StyleRefKey{ "Hair.esp", 0xAAu };
        CHECK(OS::ChangedLookCount(dressed, swapped) == 1);
        // Picking one up from bare still costs, which is the same edit as the
        // second half of that clear-then-pick.
        CHECK(OS::ChangedLookCount(dropHair, dressed) == 1);
    }

    {  // ---- and the same rule for slots -----------------------------------
       // ⚠ TWO COUNTS, AND THEY MUST NOT BE MERGED. ChangedSlotCount answers
       // "is there an edit to commit" and feeds EditorGate::StillDirty;
       // BillableSlotCount answers "what does it cost". Making the first free
       // for a revert would grey Apply out and leave a removal uncommittable,
       // which is precisely the bug StillDirty's own comment says has shipped
       // four times. So the revert is free on the BILL only.
        OS::Outfit base, staged;
        base.SetStyle(2, OS::StyleRefKey{ "A.esp", 0x1u });
        base.SetHide(3);

        // Putting the real gear back on both: still a change, but free.
        staged = base;
        staged.SetPassthrough(2);
        staged.SetPassthrough(3);
        CHECK(OS::ChangedSlotCount(base, staged) == 2);   // Apply stays live
        CHECK(OS::BillableSlotCount(base, staged) == 0);  // and costs nothing

        // Styling a bare slot costs.
        OS::Outfit dressUp;
        dressUp.SetStyle(5, OS::StyleRefKey{ "B.esp", 0x2u });
        CHECK(OS::BillableSlotCount(OS::Outfit{}, dressUp) == 1);

        // ⚠ HIDING IS NOT REVEALING. Hiding a slot is a deliberate look, so it
        // is billed; it is only the return to real gear that is free.
        OS::Outfit hideOne;
        hideOne.SetHide(5);
        CHECK(OS::BillableSlotCount(OS::Outfit{}, hideOne) == 1);

        // Swapping one style for another still costs, same reasoning as looks:
        // clear-then-pick must not be a cheaper route than picking.
        OS::Outfit swapped = dressUp;
        swapped.SetStyle(5, OS::StyleRefKey{ "B.esp", 0x3u });
        CHECK(OS::BillableSlotCount(dressUp, swapped) == 1);
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

    {  // Hide All / Show All is a LOSSLESS round trip (field 2026-08-27: it
       // used to clear every slot instead, and no Show could bring one back).
       // Styles come back, and so do the dyes painting them.
        Outfit              o;
        const std::uint32_t bits = (1u << 2) | (1u << 3) | (1u << 5);
        o.SetStyle(2, StyleRefKey{ "a.esp", 0x1 });
        o.SetStyle(3, StyleRefKey{ "b.esp", 0x2 });
        o.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 10, 20, 30 });
        // Slot 5 is left passthrough on purpose: the round trip has to survive
        // a mask naming a slot that was never dressed.
        CHECK(!AllSlotsHidden(o, bits));
        HideSlots(o, bits);
        CHECK(AllSlotsHidden(o, bits));
        ShowSlots(o, bits);
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kStyle);
        CHECK(o.EntryFor(2).style == (StyleRefKey{ "a.esp", 0x1 }));
        CHECK(o.EntryFor(3).style == (StyleRefKey{ "b.esp", 0x2 }));
        CHECK(o.EntryFor(5).kind == SlotEntry::Kind::kPassthrough);
        CHECK(o.DyeFor(2).channels[0] == (DyeChannel{ true, 10, 20, 30 }));
    }

    {  // A second Show All press is a no-op, not a Hide All on what the first
       // one just revealed. ShowSlots moves only slots that ARE hidden.
        Outfit              o;
        const std::uint32_t bits = (1u << 2) | (1u << 3);
        o.SetStyle(2, StyleRefKey{ "a.esp", 0x1 });
        HideSlots(o, bits);
        ShowSlots(o, bits);
        ShowSlots(o, bits);
        CHECK(o.EntryFor(2).style == (StyleRefKey{ "a.esp", 0x1 }));
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kStyle);
        CHECK(o.EntryFor(3).kind == SlotEntry::Kind::kPassthrough);
    }

    {  // The mask is obeyed: a slot outside it is never touched by either arm.
        Outfit o;
        o.SetStyle(7, StyleRefKey{ "keep.esp", 0x9 });
        HideSlots(o, 1u << 2);
        CHECK(o.EntryFor(7).kind == SlotEntry::Kind::kStyle);
        ShowSlots(o, 1u << 2);
        CHECK(o.EntryFor(7).style == (StyleRefKey{ "keep.esp", 0x9 }));
        // An empty mask says every named slot is hidden, vacuously, which is
        // what the button wants: no editable rows means nothing to show.
        CHECK(AllSlotsHidden(o, 0));
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

    {  // FavoriteSet: keys for things with no form behind them (OBody presets)
        FavoriteSet fs;
        const auto  body = FavoriteSet::NamedKeyLine("body", "CBBE Curvy");

        CHECK(body == std::string("@body|CBBE Curvy"));
        CHECK(!fs.ContainsLine(body));
        CHECK(fs.ToggleLine(body));
        CHECK(fs.ContainsLine(body));
        CHECK(!fs.ToggleLine(body));
        CHECK(!fs.ContainsLine(body));

        // ⚠ THE NAMESPACE HAS TO ACTUALLY SEPARATE THEM. A form key's first
        // field is a plugin filename and always carries an extension, so it can
        // never begin with "@"; a preset named exactly like a plugin must still
        // not collide with that plugin's style.
        const StyleRefKey trap{ "body", 0 };  // KeyLine -> "body|0"
        fs.ToggleLine(FavoriteSet::NamedKeyLine("body", "0"));
        CHECK(!fs.Contains(trap));
        CHECK(fs.ContainsLine("@body|0"));

        // Both kinds live in one file and survive the round trip together.
        FavoriteSet mixed;
        mixed.Add(StyleRefKey{ "Armors.esp", 0x800 });
        mixed.ToggleLine(FavoriteSet::NamedKeyLine("body", "Vanilla"));
        FavoriteSet loaded;
        loaded.LoadLines(mixed.Serialize());
        CHECK(loaded.Size() == 2);
        CHECK(loaded.Contains(StyleRefKey{ "Armors.esp", 0x800 }));
        CHECK(loaded.ContainsLine("@body|Vanilla"));

        // An empty line is not a key, however it arrives.
        CHECK(!fs.ToggleLine(""));
        CHECK(!fs.ContainsLine(""));
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

    {  // HairDiffers, and EditHistory recording a hair-only edit. Hair is not a
       // slot, so ChangedSlotCount must stay blind to it (the lore-mode Apply is
       // billed per slot and a hair toggle is free), while everything that has
       // to REACT to it must not be.
        Outfit a;
        Outfit b = a;
        b.hair   = HairMode::kHide;

        CHECK(HairDiffers(a, b));
        CHECK(!HairDiffers(a, a));
        CHECK(!HairDiffers(b, b));
        // The billing predicate stays blind: a hair toggle costs nothing.
        CHECK(ChangedSlotCount(a, b) == 0);
        // And it is not a body edit either, so the body-only refresh path must
        // not be the one that claims it.
        CHECK(!BodyDiffers(a, b));

        // Undo must see it. Before HairDiffers was added to SlotsDiffer, this
        // Record was dropped as idempotent: change hair, press undo, nothing.
        EditHistory h;
        h.Reset(a);
        CHECK(!h.CanUndo());
        h.Record(b);
        CHECK(h.CanUndo());
        CHECK(h.Current().hair == HairMode::kHide);
        CHECK(h.Undo().hair == HairMode::kAuto);

        // Still idempotent when hair really did not move.
        EditHistory h2;
        h2.Reset(b);
        h2.Record(b);
        CHECK(!h2.CanUndo());
    }

    {  // HairTint: the per-outfit hair colour. Not a slot, so it must stay out of
       // the lore-mode Apply bill, but everything that has to REACT to it must
       // see it. Same split body and hair visibility already use.
        Outfit a;
        Outfit b   = a;
        b.hairTint = HairTint{ true, 200, 40, 90 };

        CHECK(HairTintDiffers(a, b));
        CHECK(!HairTintDiffers(a, a));
        CHECK(!HairTintDiffers(b, b));

        // Every channel is part of the identity, and so is the enable flag.
        // Each channel is varied ALONE against the same baseline so a predicate
        // that dropped or mis-wired any one of the three could not still pass.
        Outfit c   = b;
        c.hairTint = HairTint{ true, 200, 40, 91 };  // blue alone
        CHECK(HairTintDiffers(b, c));
        Outfit c2   = b;
        c2.hairTint = HairTint{ true, 201, 40, 90 };  // red alone
        CHECK(HairTintDiffers(b, c2));
        Outfit c3   = b;
        c3.hairTint = HairTint{ true, 200, 41, 90 };  // green alone
        CHECK(HairTintDiffers(b, c3));
        Outfit d   = b;
        d.hairTint = HairTint{ false, 200, 40, 90 };
        CHECK(HairTintDiffers(b, d));

        // Two disabled tints are the same regardless of leftover channels: a
        // cleared row means "leave my hair alone", and the stale RGB under it
        // must not register as an edit or the character would refresh forever.
        Outfit e   = a;
        e.hairTint = HairTint{ false, 1, 2, 3 };
        CHECK(!HairTintDiffers(a, e));

        // The billing predicate stays blind, and this is not a body edit.
        CHECK(ChangedSlotCount(a, b) == 0);
        CHECK(!BodyDiffers(a, b));

        // Undo must see it.
        EditHistory h;
        h.Reset(a);
        CHECK(!h.CanUndo());
        h.Record(b);
        CHECK(h.CanUndo());
        CHECK(h.Current().hairTint.set);
        CHECK(h.Current().hairTint.r == 200);
        CHECK(!h.Undo().hairTint.set);

        // Still idempotent when the tint really did not move.
        EditHistory h2;
        h2.Reset(b);
        h2.Record(b);
        CHECK(!h2.CanUndo());
    }

    {  // EyeTint: the per-outfit eye colour, the same split hairTint carries
       // and the fifth dimension to ship without it (field 2026-08-13: the
       // colour reached the screen only when the eye part changed, because
       // nothing diffed the tint).
        Outfit a;
        Outfit b  = a;
        b.eyeTint = HairTint{ true, 0, 176, 0 };

        CHECK(EyeTintDiffers(a, b));
        CHECK(!EyeTintDiffers(a, a));
        CHECK(!EyeTintDiffers(b, b));

        // The enable flag is part of the identity.
        Outfit d  = b;
        d.eyeTint = HairTint{ false, 0, 176, 0 };
        CHECK(EyeTintDiffers(b, d));

        // Two disabled tints are the same regardless of leftover channels, or
        // clearing the row and selecting another outfit would register a
        // phantom edit and refresh the character for nothing.
        Outfit e  = a;
        e.eyeTint = HairTint{ false, 1, 2, 3 };
        CHECK(!EyeTintDiffers(a, e));

        // ⚠ THE BLEND RIDES IN HERE RATHER THAN IN A NINTH DIMENSION. It
        // changes what the same RGB renders as, so a painted eye whose blend
        // moves is an eye change, and a pass that could not see it would leave
        // a picked blend off the screen until the colour itself moved.
        Outfit f   = b;
        f.eyeBlend = 3;
        CHECK(EyeTintDiffers(b, f));

        // And it is dead data when neither half is painted, the rule the tints
        // themselves follow: with no eye colour there is no texture being
        // mixed, so a leftover byte must not refresh the character for nothing.
        Outfit g   = a;
        g.eyeBlend = 3;
        CHECK(!EyeTintDiffers(a, g));

        // Not a slot and not a body edit; the refresh and the bill both have
        // their own terms for it.
        CHECK(ChangedSlotCount(a, b) == 0);
        CHECK(!BodyDiffers(a, b));

        // Undo must see it. Before EyeTintDiffers joined SlotsDiffer, this
        // Record was dropped as idempotent: change the eye colour, press
        // undo, nothing.
        EditHistory h;
        h.Reset(a);
        CHECK(!h.CanUndo());
        h.Record(b);
        CHECK(h.CanUndo());
        CHECK(h.Current().eyeTint.set);
        CHECK(h.Current().eyeTint.g == 176);
        CHECK(!h.Undo().eyeTint.set);

        // Still idempotent when the tint really did not move.
        EditHistory h2;
        h2.Reset(b);
        h2.Record(b);
        CHECK(!h2.CanUndo());
    }

    {  // NearestColorIndex: resolves a picked colour to the closest one available.
       // Candidates are colour-major rgb triples as a span, so its own length
       // carries the byte count - there is no separate count argument that
       // could disagree with the buffer's real size.
        const std::uint8_t pool[] = {
            0,   0,   0,    // 0 black
            255, 255, 255,  // 1 white
            200, 40,  90,   // 2 crimson
            10,  200, 10,   // 3 green
        };

        // Exact match wins outright.
        CHECK(NearestColorIndex(200, 40, 90, pool) == 2);
        CHECK(NearestColorIndex(0, 0, 0, pool) == 0);

        // A near miss lands on its neighbour, not on something else.
        CHECK(NearestColorIndex(198, 44, 88, pool) == 2);
        CHECK(NearestColorIndex(250, 250, 240, pool) == 1);

        // Empty pool has no answer, and the caller must handle that rather than
        // index a sentinel.
        CHECK(NearestColorIndex(1, 2, 3, std::span<const std::uint8_t>{}) ==
              kNoColorMatch);

        // A buffer whose length is not a multiple of 3 cannot be colour-major
        // triples - refused outright rather than silently truncated or read
        // one short.
        const std::uint8_t malformed[] = { 1, 2, 3, 4 };
        CHECK(NearestColorIndex(1, 2, 3, malformed) == kNoColorMatch);

        // A single-candidate pool always answers with it, however far away.
        const std::uint8_t one[] = { 7, 7, 7 };
        CHECK(NearestColorIndex(255, 0, 255, one) == 0);

        // Ties resolve to the LOWEST index, so the same picked colour snaps to
        // the same form every session on an unchanged load order.
        const std::uint8_t tie[] = { 100, 0, 0, 100, 0, 0 };
        CHECK(NearestColorIndex(100, 0, 0, tie) == 0);

        // The green channel must actually be WEIGHTED, not merely present in the
        // signature. Two candidates share the query's r and b exactly, so the
        // redmean r-term and b-term are bit-for-bit equal between them (dr=db=0
        // for both); only g differs. A distance function that dropped the green
        // term would see an exact tie here and resolve it to the LOWEST index -
        // the wrong colour - instead of the one that actually matches.
        const std::uint8_t greenOnly[] = {
            50, 0,  50,   // 0 - r/b match the query, g does not
            50, 10, 50,   // 1 - exact match on all three channels
        };
        CHECK(NearestColorIndex(50, 10, 50, greenOnly) == 1);

        // Redmean weighting must be genuinely ACTIVE, not merely non-zero on
        // green: green is weighted flat at 4x, above a mid-brightness red or
        // blue delta, so a numerically SMALLER green delta can still score
        // worse than a larger red one. Naive unweighted distance ranks these
        // the opposite way (100 vs 64 picks index 1; redmean gives 251 vs 256,
        // picking index 0).
        const std::uint8_t weighted[] = {
            138, 128, 128,  // 0: +10 red only
            128, 136, 128,  // 1: +8 green only
        };
        CHECK(NearestColorIndex(128, 128, 128, weighted) == 0);
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

    {  // dye storage round trips through the outfit
        Outfit o;
        CHECK(!o.AnyDye());
        CHECK(!o.DyeFor(2).channels[0].set);

        o.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 10, 20, 30 });
        CHECK(o.AnyDye());
        CHECK(o.DyeFor(2).channels[0].set);
        CHECK(o.DyeFor(2).channels[0].r == 10);
        CHECK(o.DyeFor(2).channels[0].g == 20);
        CHECK(o.DyeFor(2).channels[0].b == 30);
        CHECK(!o.DyeFor(2).channels[1].set);
        CHECK(!o.DyeFor(3).channels[0].set);

        o.SetDye(kBitCount, DyeChannelId::kPrimary, DyeChannel{ true, 1, 1, 1 });
        CHECK(o.DyeFor(kBitCount).channels[0].set == false);  // out of range is inert
    }

    {  // ClearDye drops the WHOLE slot, every channel, and touches no other
        Outfit o;
        for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
            o.SetDye(2, static_cast<DyeChannelId>(c),
                     DyeChannel{ true, static_cast<std::uint8_t>(c), 0, 0 });
        }
        o.SetDye(3, DyeChannelId::kPrimary, DyeChannel{ true, 7, 7, 7 });

        o.ClearDye(2);
        CHECK(!o.DyeFor(2).Any());
        CHECK(o.DyeFor(3).channels[0].set);   // the neighbour is not collateral
        o.ClearDye(kBitCount);                // out of range is inert, not a write
        CHECK(o.DyeFor(3).channels[0].set);
    }

    {  // ⚠ EMPTYING A SLOT TAKES ITS DYE WITH IT. The field report: clear a
       // transmog slot and the dye pane kept drawing a stripe for it, slashed
       // and unclickable, because the colour was still stored with nothing left
       // to paint. A dye belongs to the piece, so the piece leaving takes it.
        Outfit o;
        o.SetStyle(2, StyleRefKey{ "Armors.esp", 0x800 });
        o.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 10, 20, 30 });
        o.SetDye(2, static_cast<DyeChannelId>(4), DyeChannel{ true, 4, 4, 4 });
        o.SetStyle(3, StyleRefKey{ "Armors.esp", 0x801 });
        o.SetDye(3, DyeChannelId::kPrimary, DyeChannel{ true, 1, 2, 3 });

        ClearSlot(o, 2);
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kPassthrough);
        CHECK(!o.DyeFor(2).Any());
        CHECK(o.EntryFor(3).kind == SlotEntry::Kind::kStyle);  // one slot only
        CHECK(o.DyeFor(3).channels[0].set);

        ClearSlot(o, kBitCount);                               // out of range is inert
        CHECK(o.DyeFor(3).channels[0].set);
    }

    {  // ⚠ THE CLEAR AND THE DYE CLEAR ARE ONE UNDO STEP. History records whole
       // outfit snapshots at Push(), so two mutations either side of one Push
       // are one entry - but only if both land before it. Split them and one
       // undo hands the garment back naked, or the colour back with no garment.
        Outfit base;
        base.SetStyle(2, StyleRefKey{ "Armors.esp", 0x800 });
        base.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 10, 20, 30 });

        EditHistory h;
        h.Reset(base);

        Outfit staged = base;
        ClearSlot(staged, 2);
        h.Record(staged);
        CHECK(h.Size() == 2);  // ONE entry for the pair, not one each

        const Outfit& undone = h.Undo();
        CHECK(undone.EntryFor(2).kind == SlotEntry::Kind::kStyle);
        CHECK(undone.DyeFor(2).channels[0].set);
        CHECK(undone.DyeFor(2).channels[0].r == 10);

        const Outfit& redone = h.Redo();
        CHECK(redone.EntryFor(2).kind == SlotEntry::Kind::kPassthrough);
        CHECK(!redone.DyeFor(2).Any());
    }

    {  // ⚠⚠ A WITHIN-SLOT SWAP ARRIVES UNDYED, AND SWAPPING BACK RESTORES.
       // This test asserted the OPPOSITE until 2026-08-28: "a within-slot swap
       // keeps its dye ... losing the colour on every restyle would be a worse
       // bug than the orphan". That was sound while a dye could only belong to a
       // slot, because the only two options were inherit or lose. The field
       // proved inherit is the worse one: "set the chest piece to Armor1 and dye
       // it yellow, then swap to Armor2, it automatically dyes Armor2 yellow as
       // well." With the garment in the key there is no trade to make. Armor2
       // comes in its own colours and Armor1's yellow is waiting where it was.
        Outfit o;
        o.SetStyle(2, StyleRefKey{ "a.esp", 0x1 });
        o.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 10, 20, 30 });
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[2] = (1u << 2);

        const auto cleared =
            AssignStyleWithCoverage(o, 2, StyleRefKey{ "b.esp", 0x2 }, (1u << 2), cov);

        CHECK(cleared == 0);
        CHECK(o.EntryFor(2).style.modName == "b.esp");
        CHECK(!o.DyeFor(2).Any());  // b arrives in its own colours

        // ⚠ AND THE FIRST PIECE'S COLOUR IS PARKED, NOT DESTROYED. This is the
        // half that makes the reversal safe: the old comment's fear was losing
        // the colour on every restyle, and nothing is lost.
        AssignStyleWithCoverage(o, 2, StyleRefKey{ "a.esp", 0x1 }, (1u << 2), cov);
        CHECK(o.DyeFor(2).channels[0].set);
        CHECK(o.DyeFor(2).channels[0].r == 10);

        // Dyeing b now leaves a alone: two pieces, two colours, one slot.
        AssignStyleWithCoverage(o, 2, StyleRefKey{ "b.esp", 0x2 }, (1u << 2), cov);
        o.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 40, 50, 60 });
        CHECK(o.DyeFor(2).channels[0].r == 40);
        AssignStyleWithCoverage(o, 2, StyleRefKey{ "a.esp", 0x1 }, (1u << 2), cov);
        CHECK(o.DyeFor(2).channels[0].r == 10);
    }

    {  // An EVICTED row loses its dye with its style. Not merely tidy: the
       // winner's armature covers this bit too, so the biped's part clone here
       // is the new garment, and a surviving colour would paint the wrong piece.
        Outfit o;
        o.SetStyle(1, StyleRefKey{ "hood.esp", 0x800 });
        o.SetDye(1, DyeChannelId::kPrimary, DyeChannel{ true, 9, 9, 9 });
        o.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 5, 5, 5 });  // uninvolved
        std::array<std::uint32_t, Outfit::kBitCount> cov{};
        cov[1] = (1u << 1);

        const auto cleared = AssignStyleWithCoverage(
            o, 0, StyleRefKey{ "helm.esp", 0x900 }, (1u << 0) | (1u << 1), cov);

        CHECK(cleared == (1u << 1));
        CHECK(o.EntryFor(1).kind == SlotEntry::Kind::kPassthrough);
        CHECK(!o.DyeFor(1).Any());                   // evicted, so its colour went
        CHECK(o.DyeFor(2).channels[0].set);          // a disjoint slot is untouched
    }

    {  // ⚠ A HIDE/SHOW ROUND TRIP IS LOSSLESS, dye included. Hiding takes
       // nothing away - SetHide keeps the style it covers - so ToggleHideSlot
       // must never route through ClearSlot however much the kPassthrough
       // branch looks like an emptying.
        Outfit o;
        o.SetStyle(2, StyleRefKey{ "Armors.esp", 0x800 });
        o.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 10, 20, 30 });

        ToggleHideSlot(o, 2);                                   // Hide
        CHECK(o.DyeFor(2).channels[0].set);
        ToggleHideSlot(o, 2);                                   // Show
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kStyle);
        CHECK(o.DyeFor(2).channels[0].r == 10);

        // ...and on a slot with nothing styled, where Show lands on
        // kPassthrough. The dye there belongs to the player's REAL gear.
        Outfit bare;
        bare.SetDye(5, DyeChannelId::kPrimary, DyeChannel{ true, 1, 2, 3 });
        ToggleHideSlot(bare, 5);                                // Hide
        ToggleHideSlot(bare, 5);                                // Show
        CHECK(bare.EntryFor(5).kind == SlotEntry::Kind::kPassthrough);
        CHECK(bare.DyeFor(5).channels[0].set);
    }

    {  // two DISABLED channels compare equal whatever their leftover bytes hold
        Outfit a, b;
        CHECK(!DyeDiffers(a, b));

        a.SetDye(4, DyeChannelId::kAccent, DyeChannel{ false, 9, 9, 9 });
        b.SetDye(4, DyeChannelId::kAccent, DyeChannel{ false, 1, 2, 3 });
        CHECK(!DyeDiffers(a, b));  // both off: the channels underneath are dead data

        b.SetDye(4, DyeChannelId::kAccent, DyeChannel{ true, 1, 2, 3 });
        CHECK(DyeDiffers(a, b));

        a.SetDye(4, DyeChannelId::kAccent, DyeChannel{ true, 1, 2, 3 });
        CHECK(!DyeDiffers(a, b));

        a.SetDye(4, DyeChannelId::kAccent, DyeChannel{ true, 1, 2, 4 });
        CHECK(DyeDiffers(a, b));
    }

    {  // a dye edit is NOT a slot change (no gold) but IS undoable
        Outfit base;
        Outfit staged = base;
        staged.SetDye(5, DyeChannelId::kPrimary, DyeChannel{ true, 200, 100, 50 });
        CHECK(ChangedSlotCount(base, staged) == 0);  // never billed

        EditHistory h;
        h.Reset(base);
        h.Record(staged);
        CHECK(h.CanUndo());
        CHECK(!DyeDiffers(h.Current(), staged));
        CHECK(!DyeDiffers(h.Undo(), base));
    }

    {  // dyes on DIFFERENT slots differ, both directions. Every assertion
       // above uses one slot for both outfits, so an implementation that
       // iterated only the slots dyed in a_base would pass them all while
       // missing a dye added on a new slot.
        Outfit a, b;
        a.SetDye(4, DyeChannelId::kPrimary, DyeChannel{ true, 10, 20, 30 });
        b.SetDye(5, DyeChannelId::kPrimary, DyeChannel{ true, 10, 20, 30 });
        CHECK(DyeDiffers(a, b));
        CHECK(DyeDiffers(b, a));
    }

    {  // ⚠ THE "channelsDyed" DEED'S PRODUCER, which did not exist until
       // 2026-08-02: DyeWorld::Gather read the counter, the unlock tests wrote
       // it, and the shipped build never did. Four override rules and the whole
       // 32-colour Dye Stamp tier gate on it, so with the counter stuck at 0
       // those 36 colours could not be earned by any route at all.
       //
       // Unlocks are STICKY, so the direction that cannot be taken back is
       // counting a channel twice. Every assertion here is about that.
        Outfit base;
        Outfit staged = base;
        CHECK(ChangedDyeChannelCount(base, staged) == 0u);  // nothing staged

        staged.SetDye(5, DyeChannelId::kPrimary, DyeChannel{ true, 200, 100, 50 });
        staged.SetDye(5, DyeChannelId::kSecondary, DyeChannel{ true, 1, 2, 3 });
        CHECK(ChangedDyeChannelCount(base, staged) == 2u);  // per CHANNEL, not per slot
        CHECK(ChangedSlotCount(base, staged) == 0u);        // and still never billed

        // ⚠ RE-COMMITTING AN UNCHANGED OUTFIT COUNTS NOTHING. This is what
        // Apply does on the frame after Apply, and a count of "channels that
        // are set" rather than a diff would earn the deed again every time.
        base = staged;
        CHECK(ChangedDyeChannelCount(base, staged) == 0u);

        // Trying a second green on a channel already committed costs one, not
        // two: it is one channel painted, whatever it was before.
        staged.SetDye(5, DyeChannelId::kPrimary, DyeChannel{ true, 0, 255, 0 });
        CHECK(ChangedDyeChannelCount(base, staged) == 1u);

        // ⚠ UNDO BACK TO THE BASELINE COUNTS NOTHING. Undo and redo walk the
        // staged outfit and never commit; what they can do is present a staged
        // value equal to the committed one, and that must price at zero.
        EditHistory h;
        h.Reset(base);
        h.Record(staged);
        CHECK(ChangedDyeChannelCount(base, h.Current()) == 1u);
        CHECK(ChangedDyeChannelCount(base, h.Undo()) == 0u);
        CHECK(ChangedDyeChannelCount(base, h.Redo()) == 1u);

        // ⚠ CLEARING A CHANNEL PAINTS NOTHING, so it counts nothing, even
        // though DyeDiffers reports it and Apply lights up for it.
        Outfit cleared = base;
        cleared.SetDye(5, DyeChannelId::kSecondary, DyeChannel{});
        CHECK(DyeDiffers(base, cleared));
        CHECK(ChangedDyeChannelCount(base, cleared) == 0u);

        // A channel whose bytes changed while it stayed OFF is dead data, the
        // same rule DyeDiffers uses, and painting is not what happened.
        Outfit deadBytes = base;
        deadBytes.SetDye(6, DyeChannelId::kAccent, DyeChannel{ false, 9, 9, 9 });
        CHECK(ChangedDyeChannelCount(base, deadBytes) == 0u);

        // Every slot is walked, not only the ones the baseline already dyes.
        Outfit elsewhere = base;
        elsewhere.SetDye(7, DyeChannelId::kAccent, DyeChannel{ true, 4, 5, 6 });
        CHECK(ChangedDyeChannelCount(base, elsewhere) == 1u);
    }

    {  // a hair-STYLE edit is NOT a slot change but IS undoable. This is the
       // renders-but-not-a-slot list's regression test: hair style shipped
       // its refresh and dirty seams while SlotsDiffer went untouched, and a
       // style-only edit was invisible to undo until review caught it. The
       // dye-undo test above pins the same property for the newest field;
       // this one pins it for the one that was forgotten.
        Outfit base;
        Outfit staged = base;
        staged.hairStyle = StyleRefKey{ "ApachiiSkyHair.esm", 0x12C4 };
        CHECK(ChangedSlotCount(base, staged) == 0);  // never billed
        CHECK(HairStyleDiffers(base, staged));

        EditHistory h;
        h.Reset(base);
        h.Record(staged);
        CHECK(h.CanUndo());  // fails if HairStyleDiffers leaves SlotsDiffer
        CHECK(!HairStyleDiffers(h.Current(), staged));
        CHECK(!HairStyleDiffers(h.Undo(), base));
    }

    {  // ⚠ THE FOURTH TIME THIS LIST WAS FORGOTTEN, and the first one a field
       // report found rather than a review. Eyes and brows shipped as outfit
       // state in OS-161 with their own refresh and dirty seams, and
       // SlotsDiffer went untouched, so an eyes-only or brows-only edit was
       // invisible to undo exactly as hair, hair colour and hair style each
       // were before it. Both fields, because HeadPartsDiffer answers for the
       // pair and a one-sided predicate would pass this test half-broken.
        Outfit base;
        Outfit eyesOnly  = base;
        eyesOnly.eyes    = StyleRefKey{ "TheEyesOfBeauty.esp", 0x00081F };
        CHECK(ChangedSlotCount(base, eyesOnly) == 0);  // never billed as a slot
        CHECK(HeadPartsDiffer(base, eyesOnly));

        EditHistory he;
        he.Reset(base);
        he.Record(eyesOnly);
        CHECK(he.CanUndo());  // fails if HeadPartsDiffer leaves SlotsDiffer
        CHECK(!HeadPartsDiffer(he.Current(), eyesOnly));
        CHECK(!HeadPartsDiffer(he.Undo(), base));

        Outfit browsOnly  = base;
        browsOnly.brows   = StyleRefKey{ "Skyrim.esm", 0x0511BC };
        CHECK(ChangedSlotCount(base, browsOnly) == 0);
        CHECK(HeadPartsDiffer(base, browsOnly));

        EditHistory hb;
        hb.Reset(base);
        hb.Record(browsOnly);
        CHECK(hb.CanUndo());
        CHECK(!HeadPartsDiffer(hb.Undo(), base));
    }

    {  // shape index to channel, and how many channels a mesh can actually use
        //
        // Widened from three on 2026-07-31 (OBI's Abyss Boots carry SIX shapes,
        // and under three channels four of them shared one colour, which is what
        // the pane's joined last-row label exposed) and from eight to sixteen on
        // 2026-08-31, on a field report of a segmented armour capped at eight.
        CHECK(kDyeChannelCount == 16);

        // ⚠ THE REST OF THIS BLOCK IS WRITTEN AGAINST THE CONSTANT, not against
        // sixteen, so the next widening moves one line and not a page of them.
        constexpr auto kLast = kDyeChannelCount - 1;

        // One shape, one channel, all the way up to the last.
        CHECK(ChannelForShapeIndex(0) == DyeChannelId::kPrimary);
        CHECK(ChannelForShapeIndex(1) == DyeChannelId::kSecondary);
        CHECK(ChannelForShapeIndex(2) == DyeChannelId::kAccent);
        CHECK(static_cast<std::size_t>(ChannelForShapeIndex(5)) == 5);
        CHECK(static_cast<std::size_t>(ChannelForShapeIndex(6)) == 6);
        // Past where the old cap stood: shape 8 has its own channel now instead
        // of folding into the tail, which is the whole of this widening.
        CHECK(static_cast<std::size_t>(ChannelForShapeIndex(8)) == 8);
        CHECK(static_cast<std::size_t>(ChannelForShapeIndex(kLast - 1)) == kLast - 1);

        // The LAST channel still absorbs the tail rather than dropping it, so a
        // mesh busier than the channel count stays fully covered.
        CHECK(static_cast<std::size_t>(ChannelForShapeIndex(kLast)) == kLast);
        CHECK(static_cast<std::size_t>(ChannelForShapeIndex(kLast + 1)) == kLast);
        CHECK(static_cast<std::size_t>(ChannelForShapeIndex(97)) == kLast);

        CHECK(LiveChannelCount(0) == 0);
        CHECK(LiveChannelCount(1) == 1);   // 52.1% of worn armour meshes
        CHECK(LiveChannelCount(2) == 2);
        CHECK(LiveChannelCount(3) == 3);
        CHECK(LiveChannelCount(6) == 6);   // the Abyss boots, a colour per piece
        CHECK(LiveChannelCount(8) == 8);   // the old cap, no longer a cap
        CHECK(LiveChannelCount(kDyeChannelCount) == kDyeChannelCount);
        CHECK(LiveChannelCount(97) == kDyeChannelCount);  // capped at what exists
    }

    {  // every skip reason has its own translation key, and dyeable has none
        CHECK(DyeSkipKey(DyeSkipReason::kNone) == nullptr);
        CHECK(std::string(DyeSkipKey(DyeSkipReason::kHair)) == "$FR_DyeSkip_Hair");
        CHECK(std::string(DyeSkipKey(DyeSkipReason::kCharacterColour)) ==
              "$FR_DyeSkip_Character");
        CHECK(std::string(DyeSkipKey(DyeSkipReason::kEyes)) == "$FR_DyeSkip_Eyes");
        CHECK(std::string(DyeSkipKey(DyeSkipReason::kGlow)) == "$FR_DyeSkip_Glow");
        CHECK(std::string(DyeSkipKey(DyeSkipReason::kUnsupported)) ==
              "$FR_DyeSkip_Unsupported");
        // ⚠ NULL NOW. A reflective shape is always dyeable: bDyeReflective
        // stopped gating the painter and governs bulk instead, so nothing ever
        // carries this reason and its sentence became unreachable. The editor
        // says what dyeing metal costs on the stripe itself.
        CHECK(DyeSkipKey(DyeSkipReason::kReflectiveOff) == nullptr);
        // ⚠ NULL, AND THAT IS THE POINT. A blood overlay is never shown on a
        // tile at all, on either walk, so a sentence explaining why it cannot be
        // dyed would be a sentence nothing can reach. A key nothing reaches is
        // the orphan this project deletes rather than ships.
        CHECK(DyeSkipKey(DyeSkipReason::kDecal) == nullptr);
    }

    {  // ⚠ A FOREIGN OCCUPANT OF BIPED 9 ONLY EARNS A TILE IF IT IS STILL
       // GENUINELY UNDYEABLE. OS-111 gave every foreign occupant a synthetic
       // one-shape tile so a stored shield colour would not read as orphaned,
       // and that was right when nothing in that slot could take a dye. The
       // weapon walk claims biped 9 and paints it now, so a weapon there gets a
       // real tile of its own and the synthetic one is a second, dead copy
       // sitting beside it saying weapons cannot be dyed (OS-120).
       //
       // A torch keeps its tile, because a torch genuinely never dyes and its
       // slot would otherwise vanish from the grid with the colour still stored.
        CHECK(!ForeignOccupantEarnsATile(DyeSkipReason::kOffHandWeapon));
        CHECK(ForeignOccupantEarnsATile(DyeSkipReason::kTorch));
        CHECK(ForeignOccupantEarnsATile(DyeSkipReason::kUnsupported));
    }

    {  // With no tile left to carry it, the sentence about an off-hand weapon is
       // unreachable and its key is an orphan. Same treatment kDecal got:
       // share kNone's null branch rather than ship words nothing can read.
        CHECK(DyeSkipKey(DyeSkipReason::kOffHandWeapon) == nullptr);
        CHECK(std::string(DyeSkipKey(DyeSkipReason::kTorch)) == "$FR_DyeSkip_Torch");
    }

    {  // colours map onto a slot in piece order, and stop when they run out
        const std::array<DyeChannel, 3> src{ DyeChannel{ true, 10, 20, 30 },
                                             DyeChannel{ true, 40, 50, 60 },
                                             DyeChannel{ true, 70, 80, 90 } };

        const auto three = MapColoursToSlot(src, 3);
        CHECK(three.channels[0] == (DyeChannel{ true, 10, 20, 30 }));
        CHECK(three.channels[2] == (DyeChannel{ true, 70, 80, 90 }));

        // A one-piece garment takes the first colour and nothing else, rather
        // than dropping the paste entirely.
        const auto one = MapColoursToSlot(src, 1);
        CHECK(one.channels[0] == (DyeChannel{ true, 10, 20, 30 }));
        CHECK(!one.channels[1].set);
        CHECK(!one.channels[2].set);

        // More pieces than colours leaves the tail alone rather than repeating.
        const std::array<DyeChannel, 1> onlyOne{ DyeChannel{ true, 1, 2, 3 } };
        const auto wide = MapColoursToSlot(onlyOne, 3);
        CHECK(wide.channels[0] == (DyeChannel{ true, 1, 2, 3 }));
        CHECK(!wide.channels[1].set);

        CHECK(!MapColoursToSlot(src, 0).Any());  // nothing worn: nothing lands
    }

    {  // ⚠ THE RAIL BINDS BULK AND NOT AN EXPLICIT CLICK, which is the whole
       // shape of the per-piece reflective choice. Dyeing a reflective shape
       // costs its shine and the spike proved there is no third option, so the
       // player has to be able to make that trade PER PIECE. Clicking one
       // stripe is them making it; pasting a set onto everything is not, and
       // that one action would otherwise dull every piece of metal they own.
       //
       // So bDyeReflective stops gating the painter and governs bulk instead:
       // with the rail on, a reflective channel is left ALONE by a paste rather
       // than being given a colour it never asked for.
        const std::array<DyeChannel, 3> src{ DyeChannel{ true, 10, 20, 30 },
                                             DyeChannel{ true, 40, 50, 60 },
                                             DyeChannel{ true, 70, 80, 90 } };
        // Channel 1 is the metal.
        const std::array<bool, 3> reflective{ false, true, false };

        // Rail OFF: the paste covers everything, shine included.
        const auto all = MapColoursToSlotSkipping(src, 3, reflective, false);
        CHECK(all.channels[0].set);
        CHECK(all.channels[1].set);
        CHECK(all.channels[2].set);

        // Rail ON: the metal is skipped and its neighbours are untouched by the
        // skip. ⚠ SKIPPED, NEVER SHIFTED UP: colours land in PIECE order, so
        // closing the gap would silently move channel 2's colour onto the metal,
        // which is the exact thing being avoided.
        const auto railed = MapColoursToSlotSkipping(src, 3, reflective, true);
        CHECK(railed.channels[0].set);
        CHECK(!railed.channels[1].set);
        CHECK(railed.channels[2].set);
        CHECK(railed.channels[2].r == 70);

        // A garment with no metal is unaffected by the rail either way.
        const std::array<bool, 3> none{ false, false, false };
        CHECK(MapColoursToSlotSkipping(src, 3, none, true).channels ==
              MapColoursToSlot(src, 3).channels);
    }

    {  // EngineShapeFormIDs: the parsing half of the readable shape name
        std::uint32_t ids[2]{};

        // The real strings, copied from the 2026-08-02 field log. The slot 43
        // one is the helmet's ears and shares its parent with slot 31.
        CHECK(EngineShapeFormIDs(" (00012E4C)[1]/ (00000D64) [50%]", ids, 2) == 2);
        CHECK(ids[0] == 0x00012E4Cu);
        CHECK(ids[1] == 0x00000D64u);

        CHECK(EngineShapeFormIDs(" (00012E4C)[1]/ (00012E4D) [50%]", ids, 2) == 2);
        CHECK(ids[0] == 0x00012E4Cu);
        CHECK(ids[1] == 0x00012E4Du);

        // A mod-index form ID is nothing special.
        CHECK(EngineShapeFormIDs(" (0C000BF5)[1]/ (00012E4D) [50%]", ids, 2) == 2);
        CHECK(ids[0] == 0x0C000BF5u);

        // Lower case resolves the same.
        CHECK(EngineShapeFormIDs("(0c000bf5)", ids, 2) == 1);
        CHECK(ids[0] == 0x0C000BF5u);

        // a_max is a hard stop, not a hint.
        CHECK(EngineShapeFormIDs(" (00012E4C)[1]/ (00000D64) [50%]", ids, 1) == 1);
        CHECK(ids[0] == 0x00012E4Cu);

        // Nif-authored names carry no form ID and must be left alone. These are
        // the real ones from the same log.
        CHECK(EngineShapeFormIDs("CuirassIvory_1:0", ids, 2) == 0);
        CHECK(EngineShapeFormIDs("IronShield:0", ids, 2) == 0);
        CHECK(EngineShapeFormIDs("IronSatchel", ids, 2) == 0);
        CHECK(EngineShapeFormIDs("", ids, 2) == 0);

        // ⚠ "3BA" is a real shape name from that log and it is bare hex. It
        // must NOT parse: the parens are what make a form ID a form ID.
        CHECK(EngineShapeFormIDs("3BA", ids, 2) == 0);

        // ⚠ THE NEGATIVE CONTROL FOR THE EIGHT-DIGIT RULE. Every letter in
        // "abc" and "dead" is a hex digit, so a parser that accepted a short run
        // would turn an ordinary parenthesised word into a form ID and replace a
        // good label with a wrong one. Relax `digits == 8` in Outfit.h and these
        // three go red.
        CHECK(EngineShapeFormIDs("Belt (abc)", ids, 2) == 0);
        CHECK(EngineShapeFormIDs("Cloak (dead)", ids, 2) == 0);
        CHECK(EngineShapeFormIDs("Hood (000012E4C)", ids, 2) == 0);  // nine, not eight

        // A parenthesised word that is not hex at all is refused for the
        // simpler reason, and both refusals matter.
        CHECK(EngineShapeFormIDs("Robes (red)", ids, 2) == 0);
    }

    // ---- iridescence: the palette's values and the player's are separate ----
    // They are stored side by side rather than merged so that turning the
    // override off can IGNORE the player's value without destroying it. A
    // setting that silently erases dye work is what the economy spec's
    // grandfathering rule refuses.
    {
        using OS::DyeChannel;
        using OS::DyeMaterial;
        using OS::EffectiveMaterial;

        DyeChannel ch;
        ch.set = true; ch.r = 10; ch.g = 20; ch.b = 30;
        ch.palette = DyeMaterial{ true, 0xF5, 0xC5, 0x42, true, 210 };

        // Free form with no override set still uses the palette's.
        CHECK(EffectiveMaterial(ch, true) == ch.palette);
        CHECK(EffectiveMaterial(ch, false) == ch.palette);

        ch.player = DyeMaterial{ true, 0x00, 0xFF, 0x00, true, 140 };
        // Free form honours the player. Both presence bits are set here, so
        // the per-property merge reproduces the old whole-block override.
        CHECK(EffectiveMaterial(ch, true) == ch.player);
        // Lore mode ignores it and DOES NOT clear it. Both halves matter:
        // the palette value is what renders, and the player's value survives
        // for when the setting goes back.
        CHECK(EffectiveMaterial(ch, false) == ch.palette);
        CHECK(ch.player.sheenSet && ch.player.glossSet);

        // A hand typed colour has no palette entry, so in Lore mode it is
        // plain. Nothing enforces this; it falls out of the data, and it is
        // what makes the curated palette worth more in Lore mode.
        DyeChannel typed;
        typed.set = true; typed.r = 1; typed.g = 2; typed.b = 3;
        typed.player = DyeMaterial{ true, 0xFF, 0xFF, 0xFF, true, 200 };
        CHECK(!EffectiveMaterial(typed, false).Any());
        CHECK(EffectiveMaterial(typed, true).Any());

        // A default channel is plain in both modes.
        CHECK(!EffectiveMaterial(DyeChannel{}, true).Any());
        CHECK(!EffectiveMaterial(DyeChannel{}, false).Any());
    }

    // ---- a finish is TWO independent properties -----------------------------
    // One presence bit cannot serve two controls. Gloss and sheen are set
    // separately, so they are presence-tracked separately, and the effective
    // material is merged PER PROPERTY rather than chosen as a whole block.
    {
        using OS::DyeChannel;
        using OS::DyeMaterial;
        using OS::EffectiveMaterial;

        DyeChannel ch;
        ch.set = true; ch.r = 1; ch.g = 2; ch.b = 3;
        ch.palette = DyeMaterial{ true, 0xF5, 0xC5, 0x42, true, 210 };

        // Player sets ONLY gloss. The palette's sheen must survive.
        ch.player = DyeMaterial{};
        ch.player.glossSet = true;
        ch.player.gloss    = 140;
        const auto eff = EffectiveMaterial(ch, true);
        CHECK(eff.glossSet && eff.gloss == 140);
        CHECK(eff.sheenSet && eff.sheenR == 0xF5 && eff.sheenG == 0xC5 && eff.sheenB == 0x42);

        // AND A LATER SWATCH CLICK STILL CHANGES THE SHEEN. With whole-block
        // selection it could not: any override froze the sheen forever.
        ch.palette = DyeMaterial{ true, 0x11, 0x22, 0x33, true, 200 };
        const auto eff2 = EffectiveMaterial(ch, true);
        CHECK(eff2.sheenR == 0x11 && eff2.sheenG == 0x22 && eff2.sheenB == 0x33);
        CHECK(eff2.gloss == 140);  // the player's gloss still wins

        // GLOSS ON A PLAIN COLOUR MUST NOT FABRICATE A BLACK SHEEN. This is
        // the concrete bug: specularColor would be written to (0,0,0) and the
        // shape's highlight would go out.
        DyeChannel plain;
        plain.set = true; plain.r = 9; plain.g = 9; plain.b = 9;
        plain.player.glossSet = true;
        plain.player.gloss    = 200;
        const auto pe = EffectiveMaterial(plain, true);
        CHECK(pe.glossSet && pe.gloss == 200);
        CHECK(!pe.sheenSet);

        // Lore mode still ignores the player entirely and destroys nothing.
        const auto lore = EffectiveMaterial(ch, false);
        CHECK(lore.gloss == 200);
        CHECK(ch.player.glossSet);
    }

    // ---- a finish-only edit is NOT a painted channel ------------------------
    // ChangedDyeChannelCount feeds the channelsDyed deed, which unlocks 36
    // colours and is STICKY. Outfit.h names over-counting as the unrecoverable
    // direction, and the defaulted operator== moved it there by accident when
    // the struct grew.
    {
        OS::Outfit base, staged;
        OS::DyeChannel c{ true, 10, 20, 30 };
        base.SetDye(2, OS::DyeChannelId::kPrimary, c);
        auto tuned = c;
        tuned.player.glossSet = true;
        tuned.player.gloss    = 200;
        staged.SetDye(2, OS::DyeChannelId::kPrimary, tuned);

        // The edit is real and Apply must light up for it.
        CHECK(OS::DyeDiffers(base, staged));
        // But nothing was PAINTED, so the deed counts nothing.
        CHECK(OS::ChangedDyeChannelCount(base, staged) == 0);

        // A genuine colour change still counts.
        auto repainted = c;
        repainted.r = 99;
        OS::Outfit other;
        other.SetDye(2, OS::DyeChannelId::kPrimary, repainted);
        CHECK(OS::ChangedDyeChannelCount(base, other) == 1);
    }

    // ---- strength is stored, and the deed must not see it -------------------
    // ⚠ THE SAME FAILURE AS THE CASE ABOVE, WITH A NEW SLIDER ON IT. A strength
    // is dragged, so it is the exact shape of the farm SameDyeColour exists to
    // prevent: nudge it back and forth and every frame would be a "change" on a
    // sticky counter that cannot be walked back. A player weakening a colour has
    // not dyed a new channel.
    {
        OS::DyeChannel a{};
        a.set = true;
        a.r = 200; a.g = 40; a.b = 40;
        OS::DyeChannel b = a;
        b.strength = 32;

        CHECK(a.strength == 255);        // full by default, so old saves are unchanged
        CHECK(OS::SameDyeColour(a, b));  // the deed sees no change
        CHECK(!(a == b));                // but the channels are genuinely different

        // A real colour change is still a change, or the deed would never count.
        OS::DyeChannel c2 = a;
        c2.r = 201;
        CHECK(!OS::SameDyeColour(a, c2));

        // And through the deed's own entry point, which is what actually feeds
        // it. Apply must still light up, exactly as it does for a finish-only
        // edit above, because the armour genuinely repaints.
        OS::Outfit base2, staged2;
        base2.SetDye(2, OS::DyeChannelId::kPrimary, a);
        staged2.SetDye(2, OS::DyeChannelId::kPrimary, b);
        CHECK(OS::DyeDiffers(base2, staged2));
        CHECK(OS::ChangedDyeChannelCount(base2, staged2) == 0);
    }

    // ---- weapon dye: storage and the resolver -------------------------------
    // Mirrors the weapon STYLE dimension exactly, on purpose. A player who has
    // already learned that a style can differ between hands, and that clearing
    // a hand falls back to Both, gets the same two rules for colour with
    // nothing new to learn. Copying the shape matters more than the storage.
    {
        using OS::DyeChannel;
        using OS::DyeChannelId;
        using OS::WeaponClass;
        using OS::WeaponHand;

        OS::Outfit       o;
        const DyeChannel red{ true, 200, 20, 20 };
        const DyeChannel blue{ true, 20, 20, 200 };

        o.SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary, red);
        CHECK(o.ResolvedWeaponDyeFor(WeaponClass::Sword, WeaponHand::Both).channels[0] == red);
        CHECK(o.ResolvedWeaponDyeFor(WeaponClass::Sword, WeaponHand::Right).channels[0] == red);
        CHECK(o.ResolvedWeaponDyeFor(WeaponClass::Sword, WeaponHand::Left).channels[0] == red);
        // ⚠ NO OVERRIDE STORED IS NOT THE SAME AS AN OVERRIDE THAT IS EMPTY,
        // and only WeaponDyeOverrideFor can tell them apart. The editor needs
        // the difference to know whether its X clears a colour or drops back to
        // inheriting, which is what ClearWeaponHandActionFor already encodes
        // for styles.
        CHECK(o.WeaponDyeOverrideFor(WeaponClass::Sword, WeaponHand::Left) == nullptr);
        // Both never has an override, by definition.
        CHECK(o.WeaponDyeOverrideFor(WeaponClass::Sword, WeaponHand::Both) == nullptr);

        // A hand override wins for its own hand and leaves the other inheriting.
        o.SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary, blue, WeaponHand::Left);
        CHECK(o.ResolvedWeaponDyeFor(WeaponClass::Sword, WeaponHand::Left).channels[0] == blue);
        CHECK(o.ResolvedWeaponDyeFor(WeaponClass::Sword, WeaponHand::Right).channels[0] == red);
        CHECK(o.WeaponDyeOverrideFor(WeaponClass::Sword, WeaponHand::Left) != nullptr);

        // ⚠ AN EXPLICIT EMPTY OVERRIDE IS NOT THE SAME AS NO OVERRIDE, and for
        // dye the difference is visible on screen rather than bookkeeping. With
        // Both red and Left explicitly emptied, the off-hand renders UNDYED.
        o.ClearWeaponDye(WeaponClass::Sword, WeaponHand::Left);
        CHECK(o.WeaponDyeOverrideFor(WeaponClass::Sword, WeaponHand::Left) != nullptr);
        CHECK(!o.ResolvedWeaponDyeFor(WeaponClass::Sword, WeaponHand::Left).Any());
        CHECK(o.ResolvedWeaponDyeFor(WeaponClass::Sword, WeaponHand::Right).channels[0] == red);

        // Dropping the override itself falls back to Both rather than to
        // nothing. This is the assertion the sparse container could quietly
        // break by treating "empty" and "absent" as one state.
        o.ClearWeaponDyeHandOverride(WeaponClass::Sword, WeaponHand::Left);
        CHECK(o.WeaponDyeOverrideFor(WeaponClass::Sword, WeaponHand::Left) == nullptr);
        CHECK(o.ResolvedWeaponDyeFor(WeaponClass::Sword, WeaponHand::Left).channels[0] == red);

        // ⚠ A CLASS WITH NO HAND DIMENSION IGNORES AN OVERRIDE, the same rule
        // ResolvedWeaponEntryFor carries. A greatsword is one object in one
        // biped slot, so honouring a per-hand colour on it would paint whichever
        // hand the caller happened to name.
        o.SetWeaponDye(WeaponClass::Greatsword, DyeChannelId::kPrimary, red);
        o.SetWeaponDye(WeaponClass::Greatsword, DyeChannelId::kPrimary, blue, WeaponHand::Left);
        CHECK(o.ResolvedWeaponDyeFor(WeaponClass::Greatsword, WeaponHand::Left).channels[0] == red);

        // Classes are independent, and a weapon dye touches no armour bit.
        CHECK(!o.ResolvedWeaponDyeFor(WeaponClass::Bow, WeaponHand::Both).Any());
        CHECK(!o.DyeFor(2).Any());
        CHECK(o.AnyWeaponDye());

        // ⚠ THE CONTAINER STAYS CANONICAL, which is what lets the codec treat
        // membership as "persist this". Clearing a Both value REMOVES it,
        // because an empty Both and no Both say the same thing, so two outfits
        // holding the same colours cannot differ in what they store.
        OS::Outfit canon;
        std::size_t stored = 0;
        canon.ForEachWeaponDye([&](WeaponClass, WeaponHand, const OS::SlotDye&) { ++stored; });
        CHECK(stored == 0u);

        canon.SetWeaponDye(WeaponClass::Mace, DyeChannelId::kPrimary, red);
        stored = 0;
        canon.ForEachWeaponDye([&](WeaponClass, WeaponHand, const OS::SlotDye&) { ++stored; });
        CHECK(stored == 1u);

        canon.ClearWeaponDye(WeaponClass::Mace);
        stored = 0;
        canon.ForEachWeaponDye([&](WeaponClass, WeaponHand, const OS::SlotDye&) { ++stored; });
        CHECK(stored == 0u);
        CHECK(!canon.AnyWeaponDye());

        // Setting a channel to an UNSET value leaves nothing behind either, so
        // a set-then-unset round trip is indistinguishable from never touched.
        canon.SetWeaponDye(WeaponClass::Mace, DyeChannelId::kPrimary, red);
        canon.SetWeaponDye(WeaponClass::Mace, DyeChannelId::kPrimary, DyeChannel{});
        stored = 0;
        canon.ForEachWeaponDye([&](WeaponClass, WeaponHand, const OS::SlotDye&) { ++stored; });
        CHECK(stored == 0u);

        // ⚠ CANONICAL ORDER, NOT INSERTION ORDER. Written back to front on
        // purpose: the codec's bytes must not depend on which swatch the user
        // clicked first.
        OS::Outfit ordered;
        ordered.SetWeaponDye(WeaponClass::Staff, DyeChannelId::kPrimary, red);
        ordered.SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary, red,
                             WeaponHand::Left);
        ordered.SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary, red);
        std::vector<std::pair<int, int>> seen;
        ordered.ForEachWeaponDye([&](WeaponClass c, WeaponHand h, const OS::SlotDye&) {
            seen.emplace_back(static_cast<int>(c), static_cast<int>(h));
        });
        CHECK(seen.size() == 3u);
        CHECK(seen[0] == std::make_pair(static_cast<int>(WeaponClass::Sword),
                                        static_cast<int>(WeaponHand::Both)));
        CHECK(seen[1] == std::make_pair(static_cast<int>(WeaponClass::Sword),
                                        static_cast<int>(WeaponHand::Left)));
        CHECK(seen[2] == std::make_pair(static_cast<int>(WeaponClass::Staff),
                                        static_cast<int>(WeaponHand::Both)));
    }

    // ---- ⚠ weapon dye and the cost path, which is a FAIL OPEN DOOR ----------
    // DyeDiffers and ChangedDyeChannelCount both walked armour bits only. Left
    // that way, one omission fails in BOTH directions at once, and the two
    // symptoms are nothing alike:
    //
    //   DyeDiffers quiet  -> Apply never lights up for a weapon-only edit.
    //                        Loud, reported within a day.
    //   the count quiet   -> weapon dye is FREE and earns no channelsDyed.
    //                        Silent, and permanent, because unlocks are add
    //                        only and this header names over-counting as the
    //                        direction that cannot be undone.
    {
        using OS::DyeChannel;
        using OS::DyeChannelId;
        using OS::WeaponClass;
        using OS::WeaponHand;

        OS::Outfit       base, staged;
        const DyeChannel red{ true, 200, 20, 20 };
        const DyeChannel blue{ true, 20, 20, 200 };

        staged.SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary, red);
        CHECK(OS::DyeDiffers(base, staged));
        CHECK(OS::ChangedDyeChannelCount(base, staged) == 1u);

        // A per-hand override is its own painted channel, not a second reading
        // of the Both value.
        staged.SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary, blue, WeaponHand::Left);
        CHECK(OS::ChangedDyeChannelCount(base, staged) == 2u);

        // A DIFF, not a count of what is set: re-committing paints nothing.
        // Same defence against double counting the armour walk already has.
        CHECK(OS::ChangedDyeChannelCount(staged, staged) == 0u);
        CHECK(!OS::DyeDiffers(staged, staged));

        // Clearing counts zero and still reports as an edit, exactly as it does
        // for armour: nothing was painted, but it has to reach the screen and
        // the Apply gate.
        OS::Outfit cleared = staged;
        cleared.ClearWeaponDye(WeaponClass::Sword);
        cleared.ClearWeaponDyeHandOverride(WeaponClass::Sword, WeaponHand::Left);
        CHECK(OS::DyeDiffers(staged, cleared));
        CHECK(OS::ChangedDyeChannelCount(staged, cleared) == 0u);

        // ⚠ AN UNREACHABLE OVERRIDE EARNS NOTHING, and this is the assertion
        // that keeps the deed honest. A per-hand colour on a class with no hand
        // dimension can never render, the deed is sticky, and unlocks are add
        // only, so counting it would let a hand-edited outfits.json farm 36
        // colours with values nothing will ever paint. It is still an EDIT, or
        // the editor could not clear what it can see.
        OS::Outfit ghost;
        ghost.SetWeaponDye(WeaponClass::Greatsword, DyeChannelId::kPrimary, red,
                           WeaponHand::Left);
        CHECK(OS::ChangedDyeChannelCount(base, ghost) == 0u);
        CHECK(OS::DyeDiffers(base, ghost));

        // ⚠ AND A WEAPON DYE IS STILL NOT A SLOT CHANGE. OutfitDye.h keeps dyes
        // out of ChangedSlotCount because a dye is not a slot edit, and adding
        // a whole dimension to dye does not change that.
        CHECK(OS::ChangedSlotCount(base, staged) == 0u);
    }

    {  // ---- what we answer while Apparel Preview owns the look -------------
        using OS::PreviewStandDownMask;
        constexpr std::uint32_t kHairBit = 1u << 1;   // slot 31
        constexpr std::uint32_t kBodyBit = 1u << 2;   // slot 32
        constexpr std::uint32_t kCircBit = 1u << 12;  // slot 42

        // Nothing hidden: the raw mask, unchanged. This is the old behaviour
        // and it stays correct for everyone who is not hiding anything.
        CHECK(PreviewStandDownMask(kHairBit | kBodyBit, 0) == (kHairBit | kBodyBit));

        // ⚠ THE BUG. A helmet is equipped and Fitting Room is hiding it, so the
        // real mask still carries its bits: hiding removes the GEOMETRY and
        // leaves the item worn. Returning the raw mask therefore culled hair
        // for a helmet nobody could see (field 2026-08-06).
        CHECK(PreviewStandDownMask(kHairBit | kCircBit | kBodyBit,
                                   kHairBit | kCircBit) == kBodyBit);

        // ⚠ STYLED COVERAGE STAYS DROPPED, which is what separates this from
        // the ordinary shim. Apparel Preview may have replaced exactly the
        // geometry a style claimed, so that claim cannot survive; the hide is
        // the opposite kind of statement and does. There is no argument here to
        // pass styled coverage through, and that is the point.
        CHECK(PreviewStandDownMask(0, 0) == 0);
        CHECK(PreviewStandDownMask(kBodyBit, kBodyBit) == 0);

        // Hidden bits that are not worn change nothing: ExpandHideOverCoverage
        // can name a slot the engine never set.
        CHECK(PreviewStandDownMask(kBodyBit, kHairBit) == kBodyBit);

        // Same shape as the full shim with no styles, which is exactly what it
        // is. Pinned so the two cannot drift.
        CHECK(PreviewStandDownMask(kHairBit | kBodyBit, kHairBit) ==
              OS::RenderedWornMask(kHairBit | kBodyBit, kHairBit, 0u));
    }

    {  // ---- which worn slots an imported preset hides (OS-142 / OS-148) ----
        using OS::ImportHidesWornSlot;
        using OS::kHeadgearSlotMask;
        constexpr bool kWorn    = true;
        constexpr bool kBare    = false;
        constexpr bool kNamed   = true;   // the preset already decides this slot
        constexpr bool kUnnamed = false;

        const auto bit = [](std::uint32_t a_editorSlot) { return a_editorSlot - 30u; };

        // OS-142, still true: a slot the preset does not name, with your own
        // gear on it, is hidden so the set does not render your body armour
        // through it.
        CHECK(ImportHidesWornSlot(bit(32), kWorn, kUnnamed));   // body
        CHECK(ImportHidesWornSlot(bit(33), kWorn, kUnnamed));   // hands
        CHECK(ImportHidesWornSlot(bit(37), kWorn, kUnnamed));   // feet

        // Nothing worn there, or the preset dresses it: not ours to hide.
        CHECK(!ImportHidesWornSlot(bit(32), kBare, kUnnamed));
        CHECK(!ImportHidesWornSlot(bit(32), kWorn, kNamed));

        // ⚠⚠ OS-148's HEADGEAR EXEMPTION IS GONE (2026-08-12) and these four
        // now hide like every other slot. Both reasons it existed for went
        // first: the ear partition a hidden helmet used to leave suppressed is
        // restored by RestoreDismemberPartitions and its deferred half, field
        // confirmed; and the click-versus-hover disagreement was two call sites
        // staging different outfits, which the preview no longer does.
        //
        // A preset that dresses no head, imported onto a player wearing a
        // helmet, is the look of the preset plus a helmet, and saving it puts
        // that in the library.
        CHECK(ImportHidesWornSlot(bit(30), kWorn, kUnnamed));   // head
        CHECK(ImportHidesWornSlot(bit(31), kWorn, kUnnamed));   // hair / helmet
        CHECK(ImportHidesWornSlot(bit(42), kWorn, kUnnamed));   // circlet
        CHECK(ImportHidesWornSlot(bit(43), kWorn, kUnnamed));   // ears

        // The two gates still hold on headgear exactly as they do elsewhere:
        // nothing worn there, or the preset dresses it, is not ours.
        CHECK(!ImportHidesWornSlot(bit(31), kBare, kUnnamed));
        CHECK(!ImportHidesWornSlot(bit(31), kWorn, kNamed));

        // The mask itself, spelled out, so a slot cannot quietly leave it.
        CHECK(kHeadgearSlotMask == ((1u << bit(30)) | (1u << bit(31)) |
                                    (1u << bit(42)) | (1u << bit(43))));
        // ⚠ AND 42 IS IN IT EVEN THOUGH 24220 NEVER READS IT. It is here
        // because a helmet OCCUPIES it, and occupying is what drags the other
        // three into the hide. kHeadPartMask is the other question and is
        // correctly still just two bits.
        CHECK((kHeadgearSlotMask & (1u << bit(42))) != 0);
        CHECK(OS::kHeadPartMask == ((1u << bit(30)) | (1u << bit(31))));
    }

    // ---- what a PALETTE swatch click puts on a channel --------------------
    //
    // ⚠ THIS EXISTS BECAUSE THE FIELD RUN CAUGHT THE COMMIT PATH DROPPING
    // EVERYTHING BUT THE COLOUR. The click handler in EditorUI.cpp copied r, g
    // and b and nothing else, so a pearlescent dye reached the paint walk with
    // mode 0 and painted flat, and the declared finish never arrived at all,
    // which is why colour.palette was empty on every channel that ever shipped.
    // No test compiles EditorUI.cpp, so the decision lives here instead.
    {
        DyeChannel staged{};
        staged.set      = true;
        staged.r        = 0x11;
        staged.g        = 0x22;
        staged.b        = 0x33;
        staged.strength = 128;             // the PLAYER's slider
        staged.player.glossSet = true;     // the PLAYER's finish override
        staged.player.gloss    = 90;

        DyeChannel dye{};
        dye.set       = true;
        dye.r         = 0x8F;
        dye.g         = 0xA7;
        dye.b         = 0xC4;
        dye.mode      = 2;
        dye.secondSet = true;
        dye.r2        = 0xC9;
        dye.g2        = 0xA0;
        dye.b2        = 0xD8;
        dye.palette.glossSet = true;
        dye.palette.gloss    = 200;
        dye.palette.sheenSet = true;
        dye.palette.sheenR   = 0xFF;
        // ⚠ AND THE DYE'S OWN strength AND player BLOCK ARE NOISE. A Dye is
        // parsed into a DyeChannel, so it carries both fields whether or not
        // they mean anything, and neither belongs to the dye.
        dye.strength = 255;
        dye.player.glossSet = true;
        dye.player.gloss    = 7;

        dye.flake = 200;
        dye.blend = 4;  // the dye asks for overlay

        const auto out = ApplyPaletteDye(staged, dye);

        CHECK(out.set);
        // The colour, and the ramp that makes it a special dye rather than a hex.
        CHECK(out.r == 0x8F && out.g == 0xA7 && out.b == 0xC4);
        CHECK(out.mode == 2);
        CHECK(out.secondSet);
        CHECK(out.r2 == 0xC9 && out.g2 == 0xA0 && out.b2 == 0xD8);
        CHECK(out.flake == 200);
        // ⚠ THE BLEND IS THE DYE'S AND MUST BE NAMED HERE. This function copies
        // by NAME, so a field added to the struct and not to it reaches the
        // channel unexercised - which is exactly how `palette` was empty on
        // every channel that ever shipped until 2026-08-08.
        CHECK(out.blend == 4);
        // What the DYE said about the finish travels, per DyeChannel's own
        // comment on `palette`: "What the DYE said, copied at swatch click."
        CHECK(out.palette.glossSet && out.palette.gloss == 200);
        CHECK(out.palette.sheenSet && out.palette.sheenR == 0xFF);
        // ⚠ AND THE PLAYER'S OWN TWO FIELDS SURVIVE, which is the half that makes
        // this a merge rather than an assignment. strength is the slider and
        // `player` is the finish override the header says is "never cleared";
        // overwriting either would reset a player's work on every swatch click.
        CHECK(out.strength == 128);
        CHECK(out.player.glossSet && out.player.gloss == 90);
    }
    {   // A PLAIN dye clears a ramp the channel was carrying, so clicking an
        // ordinary colour after a pearl gives an ordinary colour rather than the
        // old second stop retinted.
        DyeChannel staged{};
        staged.set = true;
        staged.mode = 2;
        staged.secondSet = true;
        staged.r2 = 0xC9;
        staged.palette.glossSet = true;
        staged.palette.gloss    = 200;

        DyeChannel plain{};
        plain.set = true;
        plain.r = 0x8D; plain.g = 0x65; plain.b = 0x63;

        staged.flake = 200;
        staged.blend = 5;

        const auto out = ApplyPaletteDye(staged, plain);
        CHECK(out.mode == 0);
        CHECK(!out.secondSet);
        CHECK(out.r2 == 0);
        CHECK(out.flake == 0);  // a plain dye clears the sparkle too
        // And the blend with it: a dye that is no longer applied must not leave
        // its arithmetic behind for the next colour to be mixed through. Zero
        // is the deferring value, so the channel goes back to the install's.
        CHECK(out.blend == 0);
        // The declared finish clears with it: it belongs to the dye that is no
        // longer applied, not to the channel.
        CHECK(!out.palette.glossSet);
        CHECK(out.palette.gloss == kDyeNeutral);
    }
    {   // Idempotent, so a double click and a repaint cannot drift.
        DyeChannel dye{};
        dye.set = true; dye.r = 1; dye.g = 2; dye.b = 3;
        dye.mode = 1; dye.secondSet = true; dye.r2 = 9;
        const auto once  = ApplyPaletteDye(DyeChannel{}, dye);
        const auto twice = ApplyPaletteDye(once, dye);
        CHECK(once == twice);
    }
    {   // ⚠ THE CUT IS THE PIECE'S, NOT THE DYE'S (2026-09-04). Where metal
        // starts is a fact about a piece's own mask (the cloak wants a fifth
        // of the class gap, vanilla iron half), so a swatch click keeps the
        // staged byte exactly as it keeps strength: a player who tuned the
        // cape must not get the patches back on every colour they compare.
        CHECK(DyeChannel{}.cut == 128);
        CHECK((DyeChannel{ true, 1, 2, 3 }).cut == 128);  // four positional, as ever
        DyeChannel staged{};
        staged.set = true;
        staged.cut = 51;
        DyeChannel dye{};
        dye.set = true; dye.r = 1; dye.g = 2; dye.b = 3;
        dye.mode = 5;  // twotone, and nothing said about the cut
        CHECK(ApplyPaletteDye(staged, dye).cut == 51);
        // A dye that DECLARES a cut brings it, the way a dye that declares a
        // finish does; 128 is the struct's "nothing said", which is also the
        // default picture, so no shipped dye moves a piece.
        dye.cut = 200;
        CHECK(ApplyPaletteDye(staged, dye).cut == 200);
        // A plain dye leaves the piece's cut alone too: it is not the dye's
        // ramp to clear.
        DyeChannel plain{};
        plain.set = true; plain.r = 9;
        CHECK(ApplyPaletteDye(staged, plain).cut == 51);
        // And the deed cannot see it: SameDyeColour names its four fields.
        DyeChannel moved = staged;
        moved.cut = 30;
        CHECK(SameDyeColour(staged, moved));
    }

    {  // The Special section's writes: each helper changes ONE authored fact
       // and nothing else. Strength and player are the player's; set/r/g/b
       // are the deed's; none of them may move.
        DyeChannel ch{};
        ch.set      = true;
        ch.r        = 0x11; ch.g = 0x22; ch.b = 0x33;
        ch.strength = 7;
        ch.player.sheenSet = true;
        ch.player.sheenR   = 9;

        const auto second = WithSecondStop(ch, 0xAA, 0xBB, 0xCC);
        CHECK(second.secondSet && second.r2 == 0xAA && second.g2 == 0xBB && second.b2 == 0xCC);
        CHECK(second.strength == 7 && second.player.sheenSet && second.player.sheenR == 9);
        CHECK(second.set && second.r == 0x11 && second.g == 0x22 && second.b == 0x33);

        // OFF MEANS NO COLOUR IS REMEMBERED, the hair tint's own settled rule:
        // the bytes zero with the flag.
        const auto cleared = WithoutSecondStop(second);
        CHECK(!cleared.secondSet && cleared.r2 == 0 && cleared.g2 == 0 && cleared.b2 == 0);

        const auto glossy = WithDyeGloss(ch, 200);
        CHECK(glossy.palette.glossSet && glossy.palette.gloss == 200);
        const auto matte = WithoutDyeGloss(glossy);
        CHECK(!matte.palette.glossSet && matte.palette.gloss == 128);  // back to the neutral default

        const auto sheened = WithDyeSheen(ch, 1, 2, 3);
        CHECK(sheened.palette.sheenSet && sheened.palette.sheenR == 1 &&
              sheened.palette.sheenG == 2 && sheened.palette.sheenB == 3);
        const auto unsheened = WithoutDyeSheen(sheened);
        CHECK(!unsheened.palette.sheenSet && unsheened.palette.sheenR == 0 &&
              unsheened.palette.sheenG == 0 && unsheened.palette.sheenB == 0);

        const auto moded  = WithDyeMode(ch, 2);
        const auto flaked = WithDyeFlake(ch, 150);
        CHECK(moded.mode == 2 && flaked.flake == 150);
        // The cut, the same one-fact rule (2026-09-04).
        const auto cutAt = WithDyeCut(ch, 51);
        CHECK(cutAt.cut == 51);
        CHECK(cutAt.strength == 7 && cutAt.player.sheenSet && cutAt.set && cutAt.r == 0x11);

        // The blend is one more authored fact on the same terms, and its
        // ZERO is the cleared state rather than a seventh named curve, which
        // is why there is no WithoutDyeBlend beside it.
        const auto blended = WithDyeBlend(ch, 3);
        CHECK(blended.blend == 3);
        CHECK(blended.strength == 7 && blended.player.sheenSet);
        CHECK(WithDyeBlend(blended, 0).blend == 0);

        // ⚠ THE DEED CANNOT SEE ANY OF THESE. SameDyeColour names the four
        // fields it compares, so an authoring edit is never a "changed
        // channel" and the Special section can never farm unlocks.
        CHECK(SameDyeColour(ch, second));
        CHECK(SameDyeColour(ch, glossy));
        CHECK(SameDyeColour(ch, sheened));
        CHECK(SameDyeColour(ch, moded));
        CHECK(SameDyeColour(ch, flaked));
        CHECK(SameDyeColour(ch, blended));
    }

    {  // which slot row a multi-slot garment lists under (field 2026-08-10)
        using OS::PrimaryBitFor;

        // The report: a hooded robe takes 31 for the hood and 32 for the body,
        // and the lowest bit filed it under Helmet with the Torso row empty.
        CHECK(PrimaryBitFor(OS::MaskForEditorSlot(31) | OS::MaskForEditorSlot(32)) ==
              OS::kBitBody);
        // A full outfit claiming body, hands and feet was already right.
        CHECK(PrimaryBitFor(OS::MaskForEditorSlot(32) | OS::MaskForEditorSlot(33) |
                            OS::MaskForEditorSlot(37)) == OS::kBitBody);
        // Even under the head slot, which is lower still.
        CHECK(PrimaryBitFor(OS::MaskForEditorSlot(30) | OS::MaskForEditorSlot(32)) ==
              OS::kBitBody);

        // Everything with no body bit keeps the lowest bit it always had: that
        // answer was right for the items it was chosen for, and this is the
        // smallest change that fixes the reported one.
        CHECK(PrimaryBitFor(OS::MaskForEditorSlot(30) | OS::MaskForEditorSlot(31)) ==
              OS::kBitHead);
        CHECK(PrimaryBitFor(OS::MaskForEditorSlot(31) | OS::MaskForEditorSlot(42)) ==
              OS::kBitHair);
        CHECK(PrimaryBitFor(OS::MaskForEditorSlot(37)) == OS::kBitFeet);
        CHECK(PrimaryBitFor(OS::MaskForEditorSlot(35)) == OS::kBitAmulet);

        // A weapon carries no slot bits and must stay on bit 0 rather than
        // reading as a head item: MatchMask's own note depends on it.
        CHECK(PrimaryBitFor(0) == 0);
    }

    // ---- head-part dye: the key is a slot AND a part ------------------------
    //
    // Every assertion here is a measurement from the 2026-08-17 census turned
    // into a test. The shapes are real: a hair whose own part contributes no
    // geometry and whose nine extra parts contribute one each, and two horns on
    // two invented slots.
    {
        using OS::DyeChannel;
        using OS::DyeChannelId;
        using OS::StyleRefKey;

        OS::Outfit         o;
        const DyeChannel   gold{ true, 210, 170, 60 };
        const DyeChannel   teal{ true, 20, 110, 130 };
        const StyleRefKey  acc{ "Tullius Hair 3 SMP.esp", 0x000801 };
        const StyleRefKey  body{ "Tullius Hair 3 SMP.esp", 0x000802 };
        const StyleRefKey  hornA{ "ED Horns RMIntegration.esp", 0x00582D };

        // Nothing stored reads as no colour, and it does not create an entry.
        CHECK(!o.HeadPartDyeFor(3, acc).Any());
        CHECK(o.HeadPartDyeCount() == 0);
        CHECK(!o.AnyHeadPartDye());

        // ⚠⚠ THE WHOLE POINT OF THE KEY: two parts on ONE slot hold two
        // different colours. Keyed on the slot alone these would be one control,
        // which is the design this replaced.
        o.SetHeadPartDye(3, acc, DyeChannelId::kPrimary, gold);
        o.SetHeadPartDye(3, body, DyeChannelId::kPrimary, teal);
        CHECK(o.HeadPartDyeFor(3, acc).channels[0] == gold);
        CHECK(o.HeadPartDyeFor(3, body).channels[0] == teal);
        CHECK(o.HeadPartDyeCount() == 2);
        CHECK(o.AnyHeadPartDye());

        // The same part ref on a DIFFERENT slot is a different key. Nothing
        // offers one part in two slots today and nothing forbids it, and the
        // slot travelling with the ref is what keeps an ear entry meaning ears
        // when the horn mod leaves the load order.
        CHECK(!o.HeadPartDyeFor(106, acc).Any());
        o.SetHeadPartDye(106, hornA, DyeChannelId::kPrimary, gold);
        CHECK(o.HeadPartDyeFor(106, hornA).channels[0] == gold);
        CHECK(o.HeadPartDyeCount() == 3);

        // ⚠ AN EMPTY PART REF IS NOT A KEY. It is what CustomHeadPart returns
        // for "this outfit says nothing about that slot", so storing a colour
        // under one would give every unresolved lookup a colour to collide on.
        o.SetHeadPartDye(3, StyleRefKey{}, DyeChannelId::kPrimary, gold);
        CHECK(o.HeadPartDyeCount() == 3);

        // A write that leaves nothing set ERASES the entry, so two outfits
        // holding the same colours hold the same entries and encode to the same
        // bytes. Membership means "explicitly stored", the weapon rule.
        o.SetHeadPartDye(3, body, DyeChannelId::kPrimary, DyeChannel{});
        CHECK(o.HeadPartDyeCount() == 2);
        CHECK(!o.HeadPartDyeFor(3, body).Any());
        // ...and the neighbour on the same slot is untouched by it.
        CHECK(o.HeadPartDyeFor(3, acc).channels[0] == gold);

        o.ClearHeadPartDye(3, acc);
        CHECK(o.HeadPartDyeCount() == 1);
        CHECK(!o.HeadPartDyeFor(3, acc).Any());

        // ⚠ CANONICAL ORDER, slot then plugin then form id, whatever order the
        // colours were clicked in. The codec's golden bytes depend on it: two
        // outfits holding the same colours must encode identically.
        OS::Outfit clicked;
        clicked.SetHeadPartDye(106, hornA, DyeChannelId::kPrimary, gold);
        clicked.SetHeadPartDye(3, body, DyeChannelId::kPrimary, teal);
        clicked.SetHeadPartDye(3, acc, DyeChannelId::kPrimary, gold);
        std::vector<std::pair<std::uint32_t, std::uint32_t>> seen;
        clicked.ForEachHeadPartDye([&](std::uint32_t a_slot, const StyleRefKey& a_part,
                                       const OS::SlotDye&) {
            seen.emplace_back(a_slot, a_part.localFormID);
        });
        CHECK(seen.size() == 3);
        CHECK(seen[0].first == 3 && seen[0].second == 0x000801);
        CHECK(seen[1].first == 3 && seen[1].second == 0x000802);
        CHECK(seen[2].first == 106);
    }

    // ---- head-part dye: the diff and the counter, both directions -----------
    {
        using OS::DyeChannel;
        using OS::DyeChannelId;
        using OS::StyleRefKey;

        const DyeChannel  gold{ true, 210, 170, 60 };
        const StyleRefKey horn{ "NK_HornSlider.esp", 0x00081F };

        OS::Outfit base;
        OS::Outfit staged;

        // Identical outfits differ in nothing and paint nothing.
        CHECK(!OS::HeadPartDyeDiffers(base, staged));
        CHECK(!OS::DyeDiffers(base, staged));
        CHECK(OS::ChangedDyeChannelCount(base, staged) == 0);

        // ⚠ A NEW COLOUR ON THE STAGED SIDE. Without the head walk in
        // DyeDiffers, Apply stays greyed out with a dyed horn on screen.
        staged.SetHeadPartDye(32, horn, DyeChannelId::kPrimary, gold);
        CHECK(OS::HeadPartDyeDiffers(base, staged));
        CHECK(OS::DyeDiffers(base, staged));
        CHECK(OS::ChangedDyeChannelCount(base, staged) == 1);

        // ⚠⚠ THE OTHER DIRECTION, WHICH A ONE-WAY WALK MISSES. Committed colour,
        // cleared in the editor: the container is sparse, so walking the staged
        // side alone finds no entry and reports no difference, and the player is
        // left unable to commit a clear they can see.
        OS::Outfit committed;
        committed.SetHeadPartDye(32, horn, DyeChannelId::kPrimary, gold);
        OS::Outfit cleared = committed;
        cleared.ClearHeadPartDye(32, horn);
        CHECK(OS::HeadPartDyeDiffers(committed, cleared));
        CHECK(OS::DyeDiffers(committed, cleared));
        // ...and clearing PAINTS nothing, so it earns no deed. The armour walk's
        // own rule, and unlocks are sticky, so over-counting cannot be undone.
        CHECK(OS::ChangedDyeChannelCount(committed, cleared) == 0);

        // Same colour on both sides is not an edit and is not a paint.
        CHECK(!OS::HeadPartDyeDiffers(committed, committed));
        CHECK(OS::ChangedDyeChannelCount(committed, committed) == 0);

        // ⚠ THE PART REF IS PART OF THE COMPARISON. Same slot, same channel,
        // same colour, different part is a real difference: it is a colour that
        // moved from one shape of a hair to another.
        OS::Outfit onAcc;
        OS::Outfit onBody;
        onAcc.SetHeadPartDye(3, StyleRefKey{ "H.esp", 1 }, DyeChannelId::kPrimary, gold);
        onBody.SetHeadPartDye(3, StyleRefKey{ "H.esp", 2 }, DyeChannelId::kPrimary, gold);
        CHECK(OS::HeadPartDyeDiffers(onAcc, onBody));
    }

    {  // CopyDyeStateFrom: the whole dye surface moves (slot channels with
       // every pearl field, weapon colours, head-part colours, eye tints) and
       // the gear does not. Pearl values are Abyssal Pearl out of the shipped
       // pearl.json; the profile apply's outfit/dyes split leans on this.
        Outfit from;
        from.name = "Stalhrim (Light)";
        DyeChannel pearl{ true, 0x8F, 0xA7, 0xC4 };
        pearl.mode             = 2;
        pearl.secondSet        = true;
        pearl.r2               = 0xC9;
        pearl.g2               = 0xA0;
        pearl.b2               = 0xD8;
        pearl.palette.sheenSet = true;
        pearl.palette.sheenR   = 0xC9;
        pearl.palette.sheenG   = 0xA0;
        pearl.palette.sheenB   = 0xD8;
        pearl.palette.glossSet = true;
        pearl.palette.gloss    = 200;
        from.SetDye(kBitBody, DyeChannelId::kPrimary, pearl);
        from.SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary,
                          DyeChannel{ true, 0x70, 0x6B, 0x99 });
        from.SetHeadPartDye(3, StyleRefKey{ "H.esp", 1 },
                            DyeChannelId::kPrimary,
                            DyeChannel{ true, 0x70, 0x6B, 0x99 });
        from.eyeTint = HairTint{ true, 0x2E, 0x4A, 0x3C };

        Outfit to;
        to.name = "Base";
        to.SetStyle(kBitBody, StyleRefKey{ "1Markynaz.esl", 0x801 });
        to.SetDye(kBitBody, DyeChannelId::kPrimary,
                  DyeChannel{ true, 0x0F, 0x4C, 0x5C });
        to.CopyDyeStateFrom(from);

        CHECK(to.DyeFor(kBitBody).channels[0] == pearl);
        CHECK(to.WeaponDyeFor(WeaponClass::Sword).channels[0].set);
        CHECK(to.HeadPartDyeFor(3, StyleRefKey{ "H.esp", 1 }).channels[0].set);
        CHECK(to.eyeTint.set && to.eyeTint.r == 0x2E);
        CHECK(to.name == "Base");
        CHECK(to.EntryFor(kBitBody).style.localFormID == 0x801);
        CHECK(from.AnyDyeState());
        CHECK(!Outfit{}.AnyDyeState());
    }

    if (g_failures == 0) {
        std::printf("OutfitTests: all passed\n");
        return 0;
    }
    std::printf("OutfitTests: %d failure(s)\n", g_failures);
    return 1;
}
