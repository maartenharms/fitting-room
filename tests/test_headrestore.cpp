// Pure-logic tests for the head restore: "Hidden means render as if nothing was
// equipped there", not "literally don't render it". No engine, no RE:: types.
//
// The helmet slot masks below are Skyrim.esm ground truth, read off the BOD2
// records: they are what explains why hiding an Iron Helmet restored the head
// perfectly while hiding a Daedric one left a headless character.
#include "HeadRestore.h"
#include "Outfit.h"

#include <array>
#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace {
    constexpr std::uint32_t Slots(std::initializer_list<std::uint32_t> a_editorSlots) {
        std::uint32_t mask = 0;
        for (const auto slot : a_editorSlots) {
            mask |= OS::MaskForEditorSlot(slot);
        }
        return mask;
    }

    // The real records, by name.
    constexpr std::uint32_t kDaedricHelmet = Slots({ 30, 31, 42, 43 });  // 0001396D, closed
    constexpr std::uint32_t kIronHelmet    = Slots({ 31, 42 });          // 00012E4D, open
    constexpr std::uint32_t kHoodedRobes   = Slots({ 31, 32, 42 });      // warlock robes
    constexpr std::uint32_t kCirclet       = Slots({ 42 });
}

int main() {
    using namespace OS;

    {  // ---- the mask itself ----------------------------------------------
        // 30 Head, 31 Hair, 42 Circlet, 43 Ears. Strictly wider than
        // kHeadPartMask (31+42), which is the whole bug: bits 30 and 43 of a
        // worn closed helm went on suppressing face and ears.
        CHECK(kHeadContentMask == Slots({ 30, 31, 42, 43 }));
        CHECK((kHeadPartMask & kHeadContentMask) == kHeadPartMask);
        CHECK(kHeadContentMask != kHeadPartMask);
        CHECK(CoversHeadContent(kDaedricHelmet));
        CHECK(CoversHeadContent(kIronHelmet));
        CHECK(CoversHeadContent(kHoodedRobes));   // via its hood
        CHECK(!CoversHeadContent(Slots({ 32 })));  // plain robes
        CHECK(!CoversHeadContent(Slots({ 32, 33, 37 })));
    }

    {  // ---- which real items stop suppressing ----------------------------
        const std::array daedric{ kDaedricHelmet };
        // ONE click on Hair/Helmet lifts the whole closed helm - the user is
        // not asked to hide four rows to undo one helmet.
        CHECK(LiftedHeadBits(daedric, MaskForEditorSlot(31)) == kDaedricHelmet);
        CHECK(LiftedHeadBits(daedric, MaskForEditorSlot(30)) == kDaedricHelmet);
        CHECK(LiftedHeadBits(daedric, MaskForEditorSlot(43)) == kDaedricHelmet);
        // An untouched helmet suppresses exactly as vanilla does.
        CHECK(LiftedHeadBits(daedric, 0) == 0);
        CHECK(LiftedHeadBits(daedric, MaskForEditorSlot(32)) == 0);

        const std::array iron{ kIronHelmet };
        CHECK(LiftedHeadBits(iron, MaskForEditorSlot(31)) == kIronHelmet);
        CHECK(LiftedHeadBits(iron, MaskForEditorSlot(30)) == 0);  // covers no slot 30

        // Hiding the BODY row of hooded robes removes the hood with it, so the
        // head suppression lifts too - and only the head-content bits do, since
        // those are the only ones this mask governs.
        const std::array hooded{ kHoodedRobes };
        CHECK(LiftedHeadBits(hooded, MaskForEditorSlot(32)) == Slots({ 31, 42 }));

        // Several head items at once: only the touched ones lift.
        const std::array pair{ kIronHelmet, kCirclet };
        CHECK(LiftedHeadBits(pair, MaskForEditorSlot(42)) == (kIronHelmet | kCirclet));
        CHECK(LiftedHeadBits(std::span<const std::uint32_t>{}, MaskForEditorSlot(31)) == 0);
    }

    {  // ---- the composed mask, end to end --------------------------------
        // The reported case: a real Daedric Helmet, Hair/Helmet hidden.
        // 0.3.0 stripped only bit 31 and left 30/42/43 set, which is the
        // headless character. Everything on the head now comes back.
        const std::array daedric{ kDaedricHelmet };
        const auto hideHair     = MaskForEditorSlot(31);
        const auto hiddenHeadPt = hideHair & kHeadPartMask;
        CHECK(ComposeWornMask(kDaedricHelmet, 0, 0, hiddenHeadPt,
                              LiftedHeadBits(daedric, hideHair)) == 0);
        // The 0.3.0 answer, for contrast - face and ears still suppressed.
        CHECK(((kDaedricHelmet | 0u) & ~hiddenHeadPt) == Slots({ 30, 42, 43 }));

        // An open helm restored perfectly before and still does.
        const std::array iron{ kIronHelmet };
        CHECK(ComposeWornMask(kIronHelmet, 0, 0, hiddenHeadPt,
                              LiftedHeadBits(iron, hideHair)) == 0);

        // The reported hood-void case: a cosmetic hood (31+42) styled over the
        // real Daedric helm. The hood keeps culling hair; the face comes back
        // inside it because the helm no longer suppresses it.
        const auto styleHair = MaskForEditorSlot(31);
        CHECK(ComposeWornMask(kDaedricHelmet, styleHair, kIronHelmet, 0,
                              LiftedHeadBits(daedric, styleHair)) == Slots({ 31, 42 }));

        // ANTI-REGRESSION: a closed helm styled over a closed helm. The real
        // item's suppression lifts, so the styled item's own coverage is the
        // only thing left to shadow the face - without it this would render a
        // face through solid metal, which 0.3.0 did not do.
        CHECK(ComposeWornMask(kDaedricHelmet, styleHair, kDaedricHelmet, 0,
                              LiftedHeadBits(daedric, styleHair)) == kDaedricHelmet);

        // Hiding a head slot with nothing real in it: unchanged from 0.3.0.
        CHECK(ComposeWornMask(0, 0, 0, hiddenHeadPt, 0) == 0);

        // A styled helmet with no real headgear still hides hair, as before.
        CHECK(ComposeWornMask(Slots({ 32 }), styleHair, kIronHelmet, 0, 0) ==
              Slots({ 31, 32, 42 }));

        // No outfit at all is the identity: every pass on an unstyled actor
        // must hand the engine back exactly what it computed.
        CHECK(ComposeWornMask(kDaedricHelmet, 0, 0, 0, 0) == kDaedricHelmet);
        CHECK(ComposeWornMask(kIronHelmet | Slots({ 32, 33, 37 }), 0, 0, 0, 0) ==
              (kIronHelmet | Slots({ 32, 33, 37 })));

        // Styled coverage never reaches outside head content - the shim is the
        // only consumer and 24220 reads nothing else.
        CHECK(ComposeWornMask(0, 0, Slots({ 32, 33, 37 }), 0, 0) == 0);

        // "Hide All": everything hidden, nothing styled -> nothing on the head.
        const auto hideEverything = ~0u & ~kNeverHideMask;
        CHECK(ComposeWornMask(kDaedricHelmet, 0, 0, hideEverything & kHeadPartMask,
                              LiftedHeadBits(daedric, hideEverything)) == 0);
    }

    {  // ---- the DisplaySet the shim is fed is still 0.3.0's --------------
        // ComputeDisplaySet is untouched by this change; the restore happens at
        // the shim, from the same masks. Guard that hiddenHeadPartMask keeps
        // its narrow meaning so the two are not silently conflated.
        Outfit o;
        o.SetHide(BitForEditorSlot(31));
        const auto d = ComputeDisplaySet(o, 0);
        CHECK(d.hideMask == MaskForEditorSlot(31));
        CHECK(d.hiddenHeadPartMask == MaskForEditorSlot(31));
        CHECK((d.hiddenHeadPartMask & MaskForEditorSlot(30)) == 0);
        CHECK((d.hiddenHeadPartMask & MaskForEditorSlot(43)) == 0);
    }

    if (g_failures == 0) {
        std::printf("HeadRestoreTests: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
