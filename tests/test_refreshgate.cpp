#include "RefreshGate.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using OS::RefreshGate::BlockingCooldown;
    using OS::RefreshGate::ClassifyStagedUpdate;
    using OS::RefreshGate::StagedUpdate;

    // ⚠ EVERY CALL BELOW PASSES ALL EIGHT INPUTS. ClassifyStagedUpdate has no
    // default arguments on purpose (see the header), so adding a dimension
    // breaks this file at every line instead of silently reading false, and the
    // shorter forms these used to be written in no longer compile.
    CHECK(ClassifyStagedUpdate(false, 0, false, false, false, false, false, false) == StagedUpdate::kNone);
    CHECK(ClassifyStagedUpdate(true, 0, false, false, false, false, false, false) == StagedUpdate::kBodyOnly);
    CHECK(ClassifyStagedUpdate(false, 1, false, false, false, false, false, false) == StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(true, 2, false, false, false, false, false, false) == StagedUpdate::kEquipment);

    // A hair-only edit has to reach the screen. It rides on the worn mask read
    // by 24220, which runs only inside the rebuild orchestrator, so the
    // body-only path is not enough: this must be the equipment path.
    CHECK(ClassifyStagedUpdate(false, 0, true, false, false, false, false, false) == StagedUpdate::kEquipment);
    // And it must not be downgraded when a body edit rides along with it.
    CHECK(ClassifyStagedUpdate(true, 0, true, false, false, false, false, false) == StagedUpdate::kEquipment);
    // Slots still win on their own, hair or not.
    CHECK(ClassifyStagedUpdate(false, 3, true, false, false, false, false, false) == StagedUpdate::kEquipment);

    // A hair-COLOUR-only edit has to reach the screen too. Same path as hair
    // visibility: the refresh is what carries it.
    CHECK(ClassifyStagedUpdate(false, 0, false, true, false, false, false, false) == StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(true, 0, false, true, false, false, false, false) == StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(false, 0, true, true, false, false, false, false) == StagedUpdate::kEquipment);
    // A hair STYLE-only edit must not classify kNone. UpdateStaging returns
    // early on kNone and that return is AHEAD of the hair push, so kNone here
    // means the style is applied to nothing at all.
    CHECK(ClassifyStagedUpdate(false, 0, false, false, true, false, false, false) == StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(true, 0, false, false, true, false, false, false) == StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(false, 2, false, false, true, false, false, false) == StagedUpdate::kEquipment);

    // A DYE-only edit must not classify kNone either. OutfitDye::Repaint is
    // driven off a rebuild - the tail of RefreshActor and the worn-pass hook -
    // and only the equipment branch issues one, so kNone or kBodyOnly here means
    // the dye is painted onto nothing.
    CHECK(ClassifyStagedUpdate(false, 0, false, false, false, true, false, false) ==
          StagedUpdate::kEquipment);
    // And a body edit riding along must not downgrade it to kBodyOnly, which
    // runs OBody calls and touches no material.
    CHECK(ClassifyStagedUpdate(true, 0, false, false, false, true, false, false) ==
          StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(false, 4, false, false, false, true, false, false) ==
          StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(false, 0, false, false, true, true, false, false) ==
          StagedUpdate::kEquipment);

    // An EYE or BROW-only edit must not classify kNone, and this is the term
    // that was missing for the whole of OS-161's life. UpdateStaging returns
    // early on kNone and that return is AHEAD of PushPlayerHeadParts, so kNone
    // here means the part is applied to nobody: the row renames itself, the
    // Apply button lights, and the character does not change.
    //
    // ⚠ IT LOOKED LIKE IT WORKED BECAUSE OF THE HOVER PREVIEW. Clicking a row
    // means hovering it first, and the preview calls HeadPart::Apply directly.
    // The Random button has no hover, which is what exposed it (field
    // 2026-08-08).
    CHECK(ClassifyStagedUpdate(false, 0, false, false, false, false, true, false) ==
          StagedUpdate::kEquipment);
    // And a body edit riding along must not downgrade it to kBodyOnly, which
    // issues no rebuild for the part to ride on.
    CHECK(ClassifyStagedUpdate(true, 0, false, false, false, false, true, false) ==
          StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(false, 5, false, false, false, false, true, false) ==
          StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(false, 0, false, false, true, false, true, false) ==
          StagedUpdate::kEquipment);

    // An EYE-COLOUR-only edit must not classify kNone, and it did for the
    // whole first field run of the Eyes row: the colour reached the screen
    // only when the eye PART changed alongside it, because HeadPartsDiffer
    // fired for that and nothing fired for the tint. The painter is
    // PaintEyeTint at the tail of the repaint, and only the equipment branch
    // issues one (field 2026-08-13).
    CHECK(ClassifyStagedUpdate(false, 0, false, false, false, false, false, true) ==
          StagedUpdate::kEquipment);
    // And a body edit riding along must not downgrade it to kBodyOnly, which
    // issues no rebuild for the repaint to ride on.
    CHECK(ClassifyStagedUpdate(true, 0, false, false, false, false, false, true) ==
          StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(false, 6, false, false, false, false, false, true) ==
          StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(false, 0, false, false, false, false, true, true) ==
          StagedUpdate::kEquipment);

    // And nothing changing is still nothing.
    CHECK(ClassifyStagedUpdate(false, 0, false, false, false, false, false, false) ==
          StagedUpdate::kNone);
    // A dye that did not change leaves a body edit on the body-only path.
    CHECK(ClassifyStagedUpdate(true, 0, false, false, false, false, false, false) ==
          StagedUpdate::kBodyOnly);

    using OS::RefreshGate::ShouldReassertAfterHeadEditor;

    // The one case that acts: the head editor CLOSED and an outfit is driving
    // the tint, so the geometry the editor just repainted has to be re-tinted.
    CHECK(ShouldReassertAfterHeadEditor(true, false, true));
    // Opening is not the moment - the editor is about to repaint anyway, and
    // re-asserting into a menu that is still running would fight its preview.
    CHECK(!ShouldReassertAfterHeadEditor(true, true, true));
    // Any other menu closing is none of this sink's business.
    CHECK(!ShouldReassertAfterHeadEditor(false, false, true));
    // ⚠ The gate that keeps Fitting Room out of the user's way: with no outfit
    // tint there is nothing to re-assert, and a hair colour just picked in the
    // editor must NOT be overwritten by a Restore of a stale baseline.
    CHECK(!ShouldReassertAfterHeadEditor(true, false, false));
    CHECK(!ShouldReassertAfterHeadEditor(false, true, false));

    // ---- eye and brow handoff (OS-161) -----------------------------------
    //
    // ⚠ THIS ONE ACTS IN BOTH DIRECTIONS, and that is the whole difference from
    // the tint above. A tint only needs rewriting after the editor repaints it.
    // A head PART is a record on the actor base, which is exactly what RaceMenu
    // edits, so ours has to come off on the way IN or the editor opens showing
    // Fitting Room's eyes as though they were the character's own.
    using OS::RefreshGate::ClassifyHeadPartHandoff;
    using OS::RefreshGate::HeadEditorHandoff;

    // Opening hands the character back.
    CHECK(ClassifyHeadPartHandoff(true, true, true) == HeadEditorHandoff::kStandDown);
    // Closing takes over again, which also RE-CAPTURES: whatever they left the
    // editor wearing becomes the thing a later clear restores.
    CHECK(ClassifyHeadPartHandoff(true, false, true) == HeadEditorHandoff::kReassert);

    // ⚠ THE GATE, and it matters more here than for the tint. With the outfit
    // naming no eyes, nothing of ours is on the character: standing down would
    // restore a capture that describes nothing, and re-asserting on the way out
    // would overwrite eyes the user just picked in the editor.
    CHECK(ClassifyHeadPartHandoff(true, true, false) == HeadEditorHandoff::kNone);
    CHECK(ClassifyHeadPartHandoff(true, false, false) == HeadEditorHandoff::kNone);

    // Every other menu in the game is none of this sink's business, in either
    // direction and whatever the outfit says.
    CHECK(ClassifyHeadPartHandoff(false, true, true) == HeadEditorHandoff::kNone);
    CHECK(ClassifyHeadPartHandoff(false, false, true) == HeadEditorHandoff::kNone);
    CHECK(ClassifyHeadPartHandoff(false, false, false) == HeadEditorHandoff::kNone);

    BlockingCooldown gate;
    CHECK(gate.Ready(false, 10.0));

    CHECK(!gate.Ready(true, 20.0));
    CHECK(!gate.Ready(true, 25.0));
    CHECK(!gate.Ready(false, 25.1));
    CHECK(!gate.Ready(false, 26.09));
    CHECK(gate.Ready(false, 26.10));

    // Once the cooldown is consumed, ordinary refreshes are immediate again.
    CHECK(gate.Ready(false, 26.11));

    // Re-entering block during the cooldown restarts the full post-block second.
    CHECK(!gate.Ready(true, 30.0));
    CHECK(!gate.Ready(false, 30.2));
    CHECK(!gate.Ready(true, 30.8));
    CHECK(!gate.Ready(false, 31.0));
    CHECK(!gate.Ready(false, 31.99));
    CHECK(gate.Ready(false, 32.0));

    // ---- the character-editor visit ------------------------------------
    //
    // Deleting a default costs the player a look to re-set, so every arm that
    // is NOT kClear is protecting a purchase. One case per arm, in rule order.
    {
        using OS::RefreshGate::BaselineRepair;
        using OS::RefreshGate::ClassifyBaselineRepair;
        using OS::RefreshGate::ClassifyEditorVisit;
        using OS::RefreshGate::ClassifyVisitedDefault;
        using OS::RefreshGate::DefaultVerdict;
        using OS::RefreshGate::EditorVisit;
        using OS::RefreshGate::LookDimension;

        constexpr std::uint32_t A = 0x000D3ADEu;
        constexpr std::uint32_t B = 0x0010F812u;

        auto d = [](bool hasDefault, bool authored, bool readable, bool weWrote,
                    std::uint32_t before, std::uint32_t after) {
            LookDimension x;
            x.hasDefault      = hasDefault;
            x.defaultAuthored = authored;
            x.readable        = readable;
            x.weWrote         = weWrote;
            x.before          = before;
            x.after           = after;
            return x;
        };

        // 1. Nothing bought here outranks every other reason, so the log never
        // claims a clear for a default that never existed.
        CHECK(ClassifyVisitedDefault(true, true, d(false, false, true, true, A, B)) ==
              DefaultVerdict::kNoDefault);
        // 2. A close with no matching open reading.
        CHECK(ClassifyVisitedDefault(false, false, d(true, true, true, false, A, B)) ==
              DefaultVerdict::kUnarmed);
        // 3. A failed read must not authorise a deletion.
        CHECK(ClassifyVisitedDefault(true, false, d(true, true, false, false, A, B)) ==
              DefaultVerdict::kUnreadable);
        // 4. Our own hover preview must not bill the player.
        CHECK(ClassifyVisitedDefault(true, false, d(true, true, true, true, A, B)) ==
              DefaultVerdict::kOurOwnWrite);
        // 5. THE GUARD. Race or sex not steady keeps every default, because a
        // race change is reversible inside the menu; see HeadPartLadder.h.
        CHECK(ClassifyVisitedDefault(true, true, d(true, true, true, false, A, B)) ==
              DefaultVerdict::kIdentityMoved);
        // 6. Look-and-cancel. The most important line in this file.
        CHECK(ClassifyVisitedDefault(true, false, d(true, true, true, false, A, A)) ==
              DefaultVerdict::kUnchanged);
        // 7. Stored but dormant because the outfit names this dimension: the
        // outfit outranks a usable default, so this default was never the
        // override and must survive.
        CHECK(ClassifyVisitedDefault(true, false, d(true, false, true, false, A, B)) ==
              DefaultVerdict::kDormant);
        // 8. The real thing.
        CHECK(ClassifyVisitedDefault(true, false, d(true, true, true, false, A, B)) ==
              DefaultVerdict::kClear);
        // 9-11. 0 is a slot STATE, not a missing read.
        CHECK(ClassifyVisitedDefault(true, false, d(true, true, true, false, 0, 0)) ==
              DefaultVerdict::kUnchanged);
        CHECK(ClassifyVisitedDefault(true, false, d(true, true, true, false, 0, A)) ==
              DefaultVerdict::kClear);
        CHECK(ClassifyVisitedDefault(true, false, d(true, true, true, false, A, 0)) ==
              DefaultVerdict::kClear);

        // The repair must NOT share the deletion's gate.
        // A hover-preview capture with no default behind it, which nothing
        // stands down and nothing repaired before this.
        CHECK(ClassifyBaselineRepair(true, d(false, false, true, false, A, B)) ==
              BaselineRepair::kReseed);
        CHECK(ClassifyBaselineRepair(true, d(true, true, true, false, A, A)) ==
              BaselineRepair::kNone);
        CHECK(ClassifyBaselineRepair(true, d(true, true, true, true, A, B)) ==
              BaselineRepair::kNone);
        CHECK(ClassifyBaselineRepair(false, d(true, true, true, false, A, B)) ==
              BaselineRepair::kNone);
        CHECK(ClassifyBaselineRepair(true, d(true, true, false, false, A, B)) ==
              BaselineRepair::kNone);

        auto steady = [](bool armed) {
            EditorVisit v;
            v.armed            = armed;
            v.identityReadable = true;
            return v;
        };

        // Independence: changing hair must not cost the eye default.
        {
            auto v  = steady(true);
            v.hair  = d(true, true, true, false, A, B);
            v.eyes  = d(true, true, true, false, A, A);
            v.brows = d(true, true, true, false, A, A);
            v.facialHair = d(true, true, true, false, A, A);
            v.hairColour = d(true, true, true, false, A, A);
            const auto o = ClassifyEditorVisit(v);
            CHECK(o.hair == DefaultVerdict::kClear);
            CHECK(o.eyes == DefaultVerdict::kUnchanged);
            CHECK(o.ClearedCount() == 1);
            CHECK(o.ReseededCount() == 1);
        }
        // Facial hair alone: it arrived late and two gates forgot it once.
        {
            auto v       = steady(true);
            v.facialHair = d(true, true, true, false, A, B);
            const auto o = ClassifyEditorVisit(v);
            CHECK(o.facialHair == DefaultVerdict::kClear);
            CHECK(o.ClearedCount() == 1);
        }
        // Hair colour alone: its own surface, its own entry points.
        {
            auto v       = steady(true);
            v.hairColour = d(true, true, true, false, A, B);
            const auto o = ClassifyEditorVisit(v);
            CHECK(o.hairColour == DefaultVerdict::kClear);
            CHECK(o.ClearedCount() == 1);
        }
        // Look-and-cancel at the aggregate: five defaults, nothing lost.
        {
            auto v = steady(true);
            v.hair = v.eyes = v.brows = v.facialHair = v.hairColour =
                d(true, true, true, false, A, A);
            const auto o = ClassifyEditorVisit(v);
            CHECK(o.ClearedCount() == 0);
            CHECK(o.ReseededCount() == 0);
        }
        // All five moved on a steady identity.
        {
            auto v = steady(true);
            v.hair = v.eyes = v.brows = v.facialHair = v.hairColour =
                d(true, true, true, false, A, B);
            const auto o = ClassifyEditorVisit(v);
            CHECK(o.ClearedCount() == 5);
            CHECK(o.ReseededCount() == 5);
        }
        // THE GUARD at the aggregate, three ways in. Deletions all die,
        // repairs all still happen. This is the test that pins the split.
        for (int which = 0; which < 3; ++which) {
            auto v = steady(true);
            if (which == 0) {
                v.identityChanged = true;
            } else if (which == 1) {
                v.identitySwitched = true;
            } else {
                v.identityReadable = false;  // could not be shown to be steady
            }
            v.hair = v.eyes = v.brows = v.facialHair = v.hairColour =
                d(true, true, true, false, A, B);
            const auto o = ClassifyEditorVisit(v);
            CHECK(o.ClearedCount() == 0);
            CHECK(o.hair == DefaultVerdict::kIdentityMoved);
            CHECK(o.ReseededCount() == 5);
        }
        // An unarmed visit cannot know identity moved either, so no verdict may
        // claim it. Guards the rule order.
        {
            EditorVisit v;  // armed false, identityReadable false
            v.identityChanged = true;
            v.hair = v.eyes = v.brows = v.facialHair = v.hairColour =
                d(true, true, true, false, A, B);
            const auto o = ClassifyEditorVisit(v);
            CHECK(o.ClearedCount() == 0);
            CHECK(o.ReseededCount() == 0);
            CHECK(o.hair == DefaultVerdict::kUnarmed);
        }
        // Only two dimensions were ever bought; all five baselines still get
        // repaired. A player cannot lose what they never paid for.
        {
            auto v  = steady(true);
            v.hair  = d(false, false, true, false, A, B);
            v.eyes  = d(true, true, true, false, A, B);
            v.brows = d(true, true, true, false, A, B);
            v.facialHair = d(false, false, true, false, A, B);
            v.hairColour = d(false, false, true, false, A, B);
            const auto o = ClassifyEditorVisit(v);
            CHECK(o.ClearedCount() == 2);
            CHECK(o.hair == DefaultVerdict::kNoDefault);
            CHECK(o.ReseededCount() == 5);
        }
        // One visit exercising five arms, the shape a real close edge has.
        {
            auto v       = steady(true);
            v.hair       = d(true, true, true, false, A, B);   // kClear
            v.eyes       = d(true, true, true, true, A, B);    // kOurOwnWrite
            v.brows      = d(true, true, false, false, A, B);  // kUnreadable
            v.facialHair = d(false, false, true, false, A, B); // kNoDefault
            v.hairColour = d(true, false, true, false, A, B);  // kDormant
            const auto o = ClassifyEditorVisit(v);
            CHECK(o.hair == DefaultVerdict::kClear);
            CHECK(o.eyes == DefaultVerdict::kOurOwnWrite);
            CHECK(o.brows == DefaultVerdict::kUnreadable);
            CHECK(o.facialHair == DefaultVerdict::kNoDefault);
            CHECK(o.hairColour == DefaultVerdict::kDormant);
            CHECK(o.ClearedCount() == 1);
        }
        // The fail-safe default, asserted rather than assumed.
        {
            const auto o = ClassifyEditorVisit(EditorVisit{});
            CHECK(o.ClearedCount() == 0);
            CHECK(o.ReseededCount() == 0);
            CHECK(o.hair == DefaultVerdict::kNoDefault);
        }
    }

    if (g_failures == 0) {
        std::printf("all RefreshGate tests passed\n");
    }
    return g_failures;
}
