// Pure-logic tests for the NPC render-decision core (NpcResolve.h). No engine,
// no RE:: types - the worn-required mask math and the snapshot-source
// precedence the biped hooks and OutfitSession's snapshot build reduce to.
#include "EditTargetLabel.h"
#include "NpcResolve.h"
#include "Outfit.h"
#include "SlotMask.h"

#include <cstdint>
#include <cstdio>
#include <string>

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
    using namespace OS::NpcResolve;

    {  // NPC styles are appearance choices and may fill an unworn visual slot,
       // matching the player path. Hides remain limited to real worn gear.
        DisplaySet in;
        in.styleMask = 0b111;
        in.hideMask  = 0b111;
        const auto out = WornRequiredDisplay(in, 0b101);
        CHECK(out.styleMask == 0b111);
        CHECK(out.hideMask == 0b101);
    }

    {  // In particular, a follower can preview/wear a helmet style without
       // already having a gameplay helmet equipped underneath it.
        DisplaySet in;
        in.styleMask = (1u << 2);  // one styled slot
        const auto out = WornRequiredDisplay(in, 0u);  // actor wears nothing
        CHECK(out.styleMask == (1u << 2));
        CHECK(out.hideMask == 0u);
    }

    {  // Full worn coverage is a no-op on the style/hide masks.
        DisplaySet in;
        in.styleMask = 0xABCD;
        in.hideMask  = 0x1234;
        const auto out = WornRequiredDisplay(in, 0xFFFFFFFFu);
        CHECK(out.styleMask == 0xABCD);
        CHECK(out.hideMask == 0x1234);
    }

    {  // Submask re-derivation from the MASKED hideMask. Hide a body-skin slot
       // (32 -> bit 2), a head-part slot (31 -> bit 1) and an attachment slot
       // (35 -> bit 5); worn coverage omits the head-part slot. The masked set
       // must drop the head-part bit from EVERY derived submask.
        const std::uint32_t bitBody = MaskForEditorSlot(32);  // kBodySkinMask member
        const std::uint32_t bitHead = MaskForEditorSlot(31);  // kHeadPartMask member
        const std::uint32_t bitAmul = MaskForEditorSlot(35);  // attachment (neither)

        DisplaySet in;
        in.hideMask = bitBody | bitHead | bitAmul;
        // Sanity: the raw derivation carries the head-part bit before masking.
        in.hiddenBodySkinMask   = in.hideMask & kBodySkinMask;
        in.hiddenAttachmentMask = in.hideMask & ~kBodySkinMask;
        in.hiddenHeadPartMask   = in.hideMask & kHeadPartMask;
        CHECK(in.hiddenHeadPartMask == bitHead);

        const auto out = WornRequiredDisplay(in, bitBody | bitAmul);  // no head-part worn
        CHECK(out.hideMask == (bitBody | bitAmul));
        CHECK(out.hiddenBodySkinMask == bitBody);           // body-skin survives
        CHECK(out.hiddenAttachmentMask == bitAmul);         // attachment survives
        CHECK(out.hiddenHeadPartMask == 0u);                // head-part dropped
    }

    {  // Worn-required derivation matches ComputeDisplaySet's own submask rule
       // when worn coverage is total (they must agree slot-for-slot).
        Outfit o;
        o.SetHide(kBitBody);   // 32 -> body-skin
        o.SetHide(kBitHair);   // 31 -> head-part
        o.SetHide(kBitAmulet); // 35 -> attachment
        const auto full   = ComputeDisplaySet(o, 0u);
        const auto masked = WornRequiredDisplay(full, 0xFFFFFFFFu);
        CHECK(masked.hideMask == full.hideMask);
        CHECK(masked.hiddenBodySkinMask == full.hiddenBodySkinMask);
        CHECK(masked.hiddenAttachmentMask == full.hiddenAttachmentMask);
        CHECK(masked.hiddenHeadPartMask == full.hiddenHeadPartMask);
    }

    {  // SelectNpcSource precedence: suspension stands the actor down FIRST,
       // even over a staged preview on that base (mirrors EffectiveLocked).
        CHECK(SelectNpcSource(/*suspended*/ true, /*staged*/ true, /*active*/ true) ==
              NpcSource::kNone);
        CHECK(SelectNpcSource(true, false, true) == NpcSource::kNone);
        CHECK(SelectNpcSource(true, false, false) == NpcSource::kNone);
    }

    {  // A staged target overrides an assigned base's active outfit, and stages
       // even when the base has no active outfit of its own (new-NPC preview).
        CHECK(SelectNpcSource(false, true, true) == NpcSource::kStagedOverride);
        CHECK(SelectNpcSource(false, true, false) == NpcSource::kStagedOverride);
    }

    {  // No staging: the assigned active outfit renders iff one exists.
        CHECK(SelectNpcSource(false, false, true) == NpcSource::kAssignedActive);
        CHECK(SelectNpcSource(false, false, false) == NpcSource::kNone);
    }

    {  // Race-switch suspension rule (spec §6): the trigger is beast-ness, not
       // "did the race change". A beast/creature current race (werewolf,
       // vampire lord) has no styleable humanoid biped -> suspend; any
       // humanoid current race (including an ORDINARY vampire race, which the
       // switch event also fires for) stays styleable -> resume. The RE
       // keyword lookup lives in RaceSwitchSink; this seam takes the bool.
        CHECK(ShouldSuspendForRace(/*currentRaceIsBeast*/ true) == true);
        CHECK(ShouldSuspendForRace(/*currentRaceIsBeast*/ false) == false);
    }

    {  // Editor "Editing:" label disambiguation (Task 8): a unique name stays
       // bare; a colliding name gets its plugin appended so the two are
       // distinguishable. Index alignment with the input is preserved.
        const std::vector<TargetLabelInput> in{
            { "Lydia", "Skyrim.esm" },   // collides with the modded replacer below
            { "Serana", "Dawnguard.esm" },
            { "Lydia", "BijinNPCs.esp" },
            { "", "Orphan.esp" },  // nameless -> plugin fallback, never empty
        };
        const auto out = BuildDisambiguatedLabels(in);
        CHECK(out.size() == 4);
        CHECK(out[0] == "Lydia (Skyrim.esm)");   // collision -> plugin appended
        CHECK(out[1] == "Serana");               // unique -> bare
        CHECK(out[2] == "Lydia (BijinNPCs.esp)");// the other Lydia, distinguished
        CHECK(out[3] == "Orphan.esp");           // empty name -> plugin fallback
    }

    {  // A unique name with an empty plugin stays bare (no trailing " ()").
        const std::vector<TargetLabelInput> in{ { "Inigo", "" } };
        const auto out = BuildDisambiguatedLabels(in);
        CHECK(out.size() == 1);
        CHECK(out[0] == "Inigo");
    }

    if (g_failures == 0) {
        std::printf("all NpcSession tests passed\n");
    }
    return g_failures;
}
