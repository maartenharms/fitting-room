// Pure-logic tests for the Shape overlay's decision layer (ShapeOverlay.h). No
// engine, no RE:: types - just what the page decides to push at RaceMenu and
// what it reads back out again.
//
// Two of these cases carry the file, and both fail silently in the field.
//
// The first is the identity write. A control dragged away and put back has to
// REMOVE its override, not store 1.0, because a stored identity leaves a
// Fitting Room key on a bone forever: nothing looks wrong on screen, and the
// evidence only surfaces in somebody else's VisitNodes or in RaceMenu's own
// co-save months later.
//
// The second is the pairing. Every paired control writes both sides, so a plan
// that quietly emits one bone gives a character with one big hand, which reads
// as a skeleton or mesh problem rather than as our bug.
#include "ShapeOverlay.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace {

    using namespace OS::ShapeOverlay;

    [[nodiscard]] std::size_t IndexOf(std::string_view a_id) {
        for (std::size_t i = 0; i < kSliderCount; ++i) {
            if (a_id == kSliders[i].id) {
                return i;
            }
        }
        return kSliderCount;  // no such control; every caller CHECKs this
    }

    [[nodiscard]] const Adjustment* Find(const std::vector<Adjustment>& a_plan,
                                         std::string_view                a_bone) {
        for (const auto& adj : a_plan) {
            if (adj.bone == a_bone) {
                return &adj;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t CountWrites(const std::vector<Adjustment>& a_plan) {
        std::size_t n = 0;
        for (const auto& adj : a_plan) {
            if (!adj.remove) {
                ++n;
            }
        }
        return n;
    }

}  // namespace

int main() {
    const std::size_t hands = IndexOf("hands");
    const std::size_t head  = IndexOf("head");
    const std::size_t feet  = IndexOf("feet");
    CHECK(hands < kSliderCount);
    CHECK(head < kSliderCount);
    CHECK(feet < kSliderCount);

    {  // The catalogue itself. Every control needs a first bone and a range
       // that can actually hold the default, or it is a slider that cannot be
       // returned to neutral.
        for (std::size_t i = 0; i < kSliderCount; ++i) {
            const auto& s = kSliders[i];
            CHECK(s.id && *s.id);
            CHECK(s.label && *s.label);
            // Every control's key has to be in the translations file, and
            // redeploy.ps1 -VerifyOnly refuses a build whose keys are missing
            // from the winning slot. The prefix is what makes them findable
            // there, and a key that is only the prefix is a copy-paste.
            CHECK(std::string_view{ s.key }.starts_with("$FR_Shape_"));
            CHECK(std::string_view{ s.key } != "$FR_Shape_");
            CHECK(s.bones[0] && *s.bones[0]);
            CHECK(s.minScale < s.maxScale);
            CHECK(s.minScale <= kDefaultScale && kDefaultScale <= s.maxScale);
            CHECK(std::string_view{ GroupLabel(s.group) } != "");
        }
    }

    {  // Ids and keys are unique. Ids key the logs, and a duplicate would make
       // two controls indistinguishable in the one place we get to see a field
       // report from; a duplicate key would draw one label on two rows.
        for (std::size_t i = 0; i < kSliderCount; ++i) {
            for (std::size_t j = i + 1; j < kSliderCount; ++j) {
                CHECK(std::string_view{ kSliders[i].id } !=
                      std::string_view{ kSliders[j].id });
                CHECK(std::string_view{ kSliders[i].key } !=
                      std::string_view{ kSliders[j].key });
            }
        }
    }

    {  // No bone is driven by two controls. Two sliders on one bone means the
       // second one written wins and the first appears not to work.
        for (std::size_t i = 0; i < kSliderCount; ++i) {
            for (const char* a : kSliders[i].bones) {
                if (!a) continue;
                for (std::size_t j = i + 1; j < kSliderCount; ++j) {
                    for (const char* b : kSliders[j].bones) {
                        if (!b) continue;
                        CHECK(std::string_view{ a } != std::string_view{ b });
                    }
                }
            }
        }
    }

    {  // An untouched character produces removals for every bone and not one
       // write. This is the case that keeps our key off a character nobody has
       // shaped.
        const auto plan = BuildPlan(DefaultValues());
        CHECK(CountWrites(plan) == 0);
        CHECK(!plan.empty());
        for (const auto& adj : plan) {
            CHECK(adj.remove);
        }
        CHECK(!AnyAdjusted(DefaultValues()));
    }

    {  // ⚠ THE IDENTITY WRITE. Dragging to a value and back has to leave
       // nothing behind, including from the float noise a drag lands on.
        auto values   = DefaultValues();
        values[head]  = 1.0f + (kDefaultEpsilon * 0.5f);
        const auto p1 = BuildPlan(values);
        CHECK(CountWrites(p1) == 0);
        CHECK(Find(p1, "NPC Head [Head]")->remove);

        values[head]  = 1.0f - (kDefaultEpsilon * 0.5f);
        const auto p2 = BuildPlan(values);
        CHECK(CountWrites(p2) == 0);

        // Just outside the epsilon is a real edit, or the control has a dead
        // band the user can feel.
        values[head]  = 1.0f + (kDefaultEpsilon * 4.0f);
        const auto p3 = BuildPlan(values);
        CHECK(CountWrites(p3) == 1);
    }

    {  // ⚠ THE PAIRING. One value, both sides, same number.
        auto values    = DefaultValues();
        values[hands]  = 1.10f;
        const auto plan = BuildPlan(values);
        const auto* l  = Find(plan, "NPC L Hand [LHnd]");
        const auto* r  = Find(plan, "NPC R Hand [RHnd]");
        CHECK(l && r);
        CHECK(!l->remove && !r->remove);
        CHECK(l->scale == r->scale);
        CHECK(l->scale == 1.10f);
        CHECK(CountWrites(plan) == 2);
        CHECK(AnyAdjusted(values));
    }

    {  // Clamping is per control and uses that control's own range, which is
       // the whole reason the ranges live on the slider rather than in one
       // constant. Hands allow 1.15; the head does not.
        auto values   = DefaultValues();
        values[hands] = 99.0f;
        values[head]  = 99.0f;
        const auto plan = BuildPlan(values);
        CHECK(Find(plan, "NPC L Hand [LHnd]")->scale == kSliders[hands].maxScale);
        CHECK(Find(plan, "NPC Head [Head]")->scale == kSliders[head].maxScale);
        CHECK(kSliders[hands].maxScale != kSliders[head].maxScale);

        values[feet]  = -5.0f;
        const auto p2 = BuildPlan(values);
        CHECK(Find(p2, "NPC L Foot [Lft ]")->scale == kSliders[feet].minScale);
    }

    {  // One control's plan covers that control's bones and nothing else, which
       // is what a slider drag pushes. An out-of-range index is a no-op rather
       // than a read past the catalogue.
        const auto one = PlanFor(hands, 1.10f);
        CHECK(one.size() == 2);
        CHECK(Find(one, "NPC L Hand [LHnd]") && Find(one, "NPC R Hand [RHnd]"));
        CHECK(!Find(one, "NPC Head [Head]"));

        const auto unpaired = PlanFor(head, 1.05f);
        CHECK(unpaired.size() == 1);
        CHECK(unpaired[0].bone == "NPC Head [Head]");

        CHECK(PlanFor(kSliderCount, 1.10f).empty());
        CHECK(PlanFor(kSliderCount + 99, 1.10f).empty());
    }

    {  // A short values vector cannot read past its end. It arrives that way
       // from a character shaped before an update added a control.
        const auto plan = BuildPlan(Values{ 1.05f });
        CHECK(!plan.empty());
        CHECK(CountWrites(plan) == 1);
    }

    {  // Read back what was written: the round trip is what makes reopening the
       // page show the character's actual shape rather than a row of defaults.
        auto values   = DefaultValues();
        values[hands] = 1.12f;
        values[feet]  = 0.90f;

        std::vector<Reading> readings;
        for (const auto& adj : BuildPlan(values)) {
            if (!adj.remove) {
                readings.push_back(Reading{ adj.bone, adj.scale, true });
            }
        }
        const auto back = ReadBack(readings);
        CHECK(back.size() == kSliderCount);
        CHECK(back[hands] == 1.12f);
        CHECK(back[feet] == 0.90f);
        CHECK(back[head] == kDefaultScale);
    }

    {  // A bone with no override under our key reads as untouched, and a
       // reading flagged absent is ignored even if it carries a number. That
       // second half matters: HasNodeTransformScale is the question, and
       // GetNodeTransformScale on a bone with no override returns whatever it
       // returns.
        std::vector<Reading> readings{
            Reading{ "NPC Head [Head]", 0.5f, false },
        };
        const auto back = ReadBack(readings);
        CHECK(back[head] == kDefaultScale);
    }

    {  // The left side wins a disagreement. Only something outside Fitting Room
       // writing our key can produce this, and a defined answer beats averaging
       // into a third value neither bone holds.
        std::vector<Reading> readings{
            Reading{ "NPC L Hand [LHnd]", 1.05f, true },
            Reading{ "NPC R Hand [RHnd]", 0.95f, true },
        };
        CHECK(ReadBack(readings)[hands] == 1.05f);
    }

    {  // Only the right side present still answers, so a half-written pair from
       // an interrupted apply comes back visible rather than reading as
       // untouched.
        std::vector<Reading> readings{
            Reading{ "NPC R Hand [RHnd]", 0.95f, true },
        };
        CHECK(ReadBack(readings)[hands] == 0.95f);
    }

    {  // A stored override wider than our range is clamped on the way in, not
       // dropped. Another mod's key cannot reach here, but an older build of
       // ours with a wider range can, and a control showing an unreachable
       // value is one the user cannot move.
        std::vector<Reading> readings{
            Reading{ "NPC Head [Head]", 3.0f, true },
        };
        CHECK(ReadBack(readings)[head] == kSliders[head].maxScale);
    }

    {  // Nothing read back at all is an unshaped character, which is what a
       // first visit to the page looks like.
        const auto back = ReadBack({});
        CHECK(back.size() == kSliderCount);
        CHECK(!AnyAdjusted(back));
    }

    if (g_failures == 0) {
        std::printf("test_shapeoverlay: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
