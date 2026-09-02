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
       // (32 -> bit 2), a hair slot (31 -> bit 1) and an attachment slot
       // (35 -> bit 5); worn coverage omits the hair slot. The masked set must
       // drop that bit from EVERY derived submask.
        const std::uint32_t bitBody = MaskForEditorSlot(32);  // kBodySkinMask member
        const std::uint32_t bitHair = MaskForEditorSlot(31);
        const std::uint32_t bitAmul = MaskForEditorSlot(35);  // attachment

        DisplaySet in;
        in.hideMask             = bitBody | bitHair | bitAmul;
        in.hiddenBodySkinMask   = in.hideMask & kBodySkinMask;
        in.hiddenAttachmentMask = in.hideMask & ~kBodySkinMask;
        CHECK(in.hiddenAttachmentMask == (bitHair | bitAmul));

        const auto out = WornRequiredDisplay(in, bitBody | bitAmul);  // no hair gear worn
        CHECK(out.hideMask == (bitBody | bitAmul));
        CHECK(out.hiddenBodySkinMask == bitBody);           // body-skin survives
        CHECK(out.hiddenAttachmentMask == bitAmul);         // attachment survives, hair dropped
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
    }

    {  // Hair is an appearance choice, not a worn-gear question, so the
       // worn-required masking must pass it through untouched.
        DisplaySet in;
        in.hair = HairMode::kShow;
        CHECK(WornRequiredDisplay(in, 0u).hair == HairMode::kShow);
        CHECK(WornRequiredDisplay(in, 0xFFFFFFFFu).hair == HairMode::kShow);
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

    // ---- StagedTargetMatches -------------------------------------------
    // The editor stages on exactly one base at a time. Everything else,
    // including a player-staging session and an unresolvable base, must not
    // match, or a follower inherits the player's staged outfit.
    {
        constexpr std::uint32_t kHer = 0x0001A696;
        constexpr std::uint32_t kHim = 0x000A2C8E;

        // Staging on her, asked about her.
        CHECK(StagedTargetMatches(true, false, kHer, kHer));

        // Staging on her, asked about someone else.
        CHECK(!StagedTargetMatches(true, false, kHer, kHim));

        // Staging on the PLAYER. No base may match, whatever the stale
        // stagedBaseFormID_ still holds.
        CHECK(!StagedTargetMatches(true, true, kHer, kHer));

        // Nothing staged at all.
        CHECK(!StagedTargetMatches(false, false, kHer, kHer));

        // The zero guard. An unresolvable base is 0, and a session that is
        // not staging an NPC leaves stagedBaseFormID_ at 0. Without the guard
        // these compare equal and every keyless actor silently picks up the
        // staged outfit.
        CHECK(!StagedTargetMatches(true, false, 0, 0));
        CHECK(!StagedTargetMatches(true, false, 0, kHer));
        CHECK(!StagedTargetMatches(true, false, kHer, 0));
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

    {  // NearestFirstOrder: the roster walk's order is process-list order;
        // the picker wants the person you walked up to on top. A city shape:
        // a far guard first in the walk, the adjacent smith third.
        const std::vector<float> dist2{ 90000.0f, 2500.0f, 40.0f, 640000.0f };
        const auto               order = NearestFirstOrder(dist2);
        CHECK(order.size() == 4);
        CHECK(order[0] == 2);
        CHECK(order[1] == 1);
        CHECK(order[2] == 0);
        CHECK(order[3] == 3);
    }

    {  // Ties keep the walk's order (stable), so two people on one spot do
        // not swap places between opens. Empty and single are the identity.
        const std::vector<float> tied{ 100.0f, 100.0f, 1.0f };
        const auto               order = NearestFirstOrder(tied);
        CHECK(order.size() == 3);
        CHECK(order[0] == 2);
        CHECK(order[1] == 0);
        CHECK(order[2] == 1);
        CHECK(NearestFirstOrder({}).empty());
        const auto one = NearestFirstOrder({ 7.0f });
        CHECK(one.size() == 1 && one[0] == 0);
    }

    if (g_failures == 0) {
        std::printf("all NpcSession tests passed\n");
    }
    return g_failures;
}
