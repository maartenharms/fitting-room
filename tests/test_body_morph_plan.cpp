#include "BodyMorphPlan.h"
#include "BodyStudioDraft.h"
#include "BodyStudioLayout.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
    int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; ++failures; } } while (false)
    void Near(float a, float b) { CHECK(std::fabs(a - b) < 0.0001f); }
}

int main() {
    using namespace OS;
    BodyPreset preset;
    preset.family = BodyFamily::k3BA;
    preset.sourceSet = "CBBE 3BBB Body Amazing";
    preset.sliders = {
        { "Waist", "Waist", "Torso", 20.0f, 80.0f },
        { "Zero", "Zero", "Other", 0.0f, 0.0f },
    };
    auto plan = BuildBodyMorphPlan(preset, 50.0f);
    CHECK(plan.size() == 1);
    CHECK(plan[0].name == "Waist");
    Near(plan[0].value, 0.5f);

    // Native BodySlide range is deliberately not clamped.
    preset.sliders[0].smallValue = -20.0f;
    preset.sliders[0].bigValue = 160.0f;
    plan = BuildBodyMorphPlan(preset, 100.0f);
    Near(plan[0].value, 1.6f);

    preset.sourceSet = "BHUNP 3BBB Advanced";
    preset.sliders = {
        { "Breasts", "Breasts", "Chest", 25.0f, 100.0f },
        { "Waist", "Waist", "Torso", 25.0f, 100.0f },
    };
    plan = BuildBodyMorphPlan(preset, 0.0f);
    Near(plan[0].value, 0.75f);  // exact OBody inverted-slider policy
    Near(plan[1].value, 0.25f);

    // The library's Empty body row starts a new, source-compatible document:
    // identity and values are new, while the resolved project schema survives.
    BodyPreset source;
    source.id = "existing-custom";
    source.name = "Installed Curvy";
    source.sex = BodySex::kFemale;
    source.family = BodyFamily::k3BA;
    source.sourceSet = "CBBE 3BBB Body Amazing";
    source.sourcePreset = "Installed Curvy";
    source.groups = { "3BA" };
    source.sliders = {
        { "Waist", "Waist Width", "Torso", 20.0f, 80.0f },
        { "Legs", "Leg Size", "Legs", -10.0f, 140.0f },
    };
    CHECK(CanMakeEmptyBodyDraft(source));
    const auto empty = MakeEmptyBodyDraft(source);
    CHECK(empty.has_value());
    CHECK(empty->id.empty());
    CHECK(empty->name == "Untitled body");
    CHECK(empty->sex == source.sex);
    CHECK(empty->family == source.family);
    CHECK(empty->sourceSet == source.sourceSet);
    CHECK(empty->sourcePreset == source.sourcePreset);
    CHECK(empty->groups == source.groups);
    CHECK(empty->sliders.size() == source.sliders.size());
    CHECK(empty->sliders[0].name == "Waist");
    Near(empty->sliders[0].smallValue, 0.0f);
    Near(empty->sliders[0].bigValue, 0.0f);
    Near(empty->sliders[1].smallValue, 0.0f);
    Near(empty->sliders[1].bigValue, 0.0f);

    BodyPreset unresolved;
    CHECK(!CanMakeEmptyBodyDraft(unresolved));
    CHECK(!MakeEmptyBodyDraft(unresolved).has_value());

    CHECK(std::string_view(BodyStudioCommitLabel(false)) == "Create custom preset");
    CHECK(std::string_view(BodyStudioCommitLabel(true)) == "Save changes");

    // A normal Fitting Room editor body is roughly 900 units wide at a 24-unit
    // font. It must keep the Outfit/Dye-style two-pane workbench; compact is a
    // fallback for genuinely narrow surfaces, not the ordinary desktop page.
    CHECK(!BodyStudioLayout::UseCompact(false, 900.0f, 24.0f));
    CHECK(!BodyStudioLayout::UseCompact(true, 900.0f, 24.0f));
    CHECK(BodyStudioLayout::UseCompact(false, 600.0f, 24.0f));
    Near(BodyStudioLayout::LibraryWidth(900.0f, 24.0f), 312.0f);
    Near(BodyStudioLayout::LibraryWidth(1200.0f, 24.0f), 384.0f);


    {  // Which body a BodySlide set signature names
        // ⚠⚠ THE 3BA TEST MUST WIN OVER THE CBBE ONE AND THE ORDER IS THE
        // WHOLE POINT. Every 3BA set is called something like
        // "CBBE 3BBB Body Amazing", so a cbbe test placed first would claim
        // every 3BA body in the world. The case above this one already uses
        // that exact string as a 3BA source set.
        CHECK(FamilyFromSetSignature("CBBE 3BBB Body Amazing") == BodyFamily::k3BA);
        CHECK(FamilyFromSetSignature("CBBE 3BA") == BodyFamily::k3BA);
        CHECK(FamilyFromSetSignature("CBBE Body Special") == BodyFamily::kCBBE);
        CHECK(FamilyFromSetSignature("CBBE") == BodyFamily::kCBBE);
        CHECK(FamilyFromSetSignature("UBE") == BodyFamily::kUBE);
        CHECK(FamilyFromSetSignature("HIMBO") == BodyFamily::kHIMBO);
        // ⚠ ANYTHING UNRECOGNISED IS GENERIC AND NOT CBBE. Falling back to
        // CBBE would hand a push-up recipe to a body that cannot run it.
        CHECK(FamilyFromSetSignature("Some Other Body") == BodyFamily::kGenericV1);
        // The id round trip has to survive the new family or stored presets
        // come back as kUnknown.
        CHECK(BodyFamilyFromId(BodyFamilyId(BodyFamily::kCBBE)) == BodyFamily::kCBBE);
    }

    {  // Push-up, which is a per-family recipe and not one slider
        auto value = [](const std::vector<BodyMorphValue>& a_plan,
                        std::string_view                    a_name) {
            for (const auto& m : a_plan) {
                if (m.name == a_name) {
                    return m.value;
                }
            }
            return 0.0f;
        };
        auto has = [&](const std::vector<BodyMorphValue>& a_plan,
                       std::string_view                   a_name) {
            for (const auto& m : a_plan) {
                if (m.name == a_name) {
                    return true;
                }
            }
            return false;
        };

        // Off writes nothing at all, on every body. A recipe that wrote zeros
        // would still be a write, and ClearOwned is how this gets taken off.
        for (auto family : { BodyFamily::k3BA, BodyFamily::kCBBE, BodyFamily::kUBE,
                             BodyFamily::kHIMBO, BodyFamily::kGenericV1,
                             BodyFamily::kUnknown }) {
            CHECK(BuildPushUpMorphPlan(family, PushUpMode::kNone).empty());
        }

        // ⚠⚠ MEASURED, NEVER GUESSED. tools/tri_morphs.py over the reference
        // load order, 2026-08-26: 3BA's femalebody.tri carries 154 morphs on the
        // '3BA' shape including 'PushUp' and 'BreastCleavage', and CBBE's carries
        // 97 including both. A morph name that does not exist is a SILENT no-op,
        // so these strings are the feature and the suite holds them.
        const auto ba = BuildPushUpMorphPlan(BodyFamily::k3BA, PushUpMode::kFull);
        CHECK(has(ba, "PushUp"));
        CHECK(has(ba, "BreastCleavage"));
        CHECK(BuildPushUpMorphPlan(BodyFamily::kCBBE, PushUpMode::kFull) == ba);

        // Subtle is the same recipe turned down, so the names match and every
        // value is smaller. Different names per level would be a second recipe
        // to tune and nobody asked for one.
        const auto baSubtle = BuildPushUpMorphPlan(BodyFamily::k3BA, PushUpMode::kSubtle);
        CHECK(baSubtle.size() == ba.size());
        CHECK(value(baSubtle, "PushUp") > 0.0f);
        CHECK(value(baSubtle, "PushUp") < value(ba, "PushUp"));
        CHECK(value(baSubtle, "BreastCleavage") < value(ba, "BreastCleavage"));

        // ⚠⚠ UBE CARRIES NEITHER NAME ACROSS 238 MORPHS, WHICH IS WHY THIS IS A
        // RECIPE AND NOT A PASSTHROUGH. Composed instead from the sliders it does
        // have, and the ' n|p' IS PART OF THE NAME: UBE spells a bidirectional
        // slider 'BreastsCupSag n|p', not BreastsCupSagN. Measured out of GT
        // Softbody's femalebody_tangent.tri. Guessing that suffix would have made
        // the whole feature a silent no-op on the one body it was asked for.
        const auto ube = BuildPushUpMorphPlan(BodyFamily::kUBE, PushUpMode::kFull);
        CHECK(!has(ube, "PushUp"));
        CHECK(!has(ube, "BreastCleavage"));
        CHECK(has(ube, "Breasts_Perky"));
        CHECK(has(ube, "BreastsCupSag n|p"));
        CHECK(has(ube, "BreastUpperCurve n|p"));
        CHECK(has(ube, "BreastCenterGapWidth n|p"));
        // ⚠ SAG LIFTS BY GOING NEGATIVE. A positive value here would DROP the
        // breast, which is the exact opposite of the feature.
        CHECK(value(ube, "BreastsCupSag n|p") < 0.0f);
        // And the gap narrows to make cleavage, which is also negative.
        CHECK(value(ube, "BreastCenterGapWidth n|p") < 0.0f);

        // Subtle is nearer zero on every ingredient, whichever way it points.
        const auto ubeSubtle = BuildPushUpMorphPlan(BodyFamily::kUBE, PushUpMode::kSubtle);
        CHECK(ubeSubtle.size() == ube.size());
        for (const auto& m : ubeSubtle) {
            CHECK(std::fabs(m.value) < std::fabs(value(ube, m.name)));
        }

        // ⚠⚠ AND NOTHING AT ALL FOR A BODY WE CANNOT NAME. The control is
        // HIDDEN on these rather than shown doing nothing, so the predicate the
        // UI asks and the plan the writer builds must never disagree.
        for (auto family : { BodyFamily::kHIMBO, BodyFamily::kGenericV1,
                             BodyFamily::kUnknown }) {
            CHECK(BuildPushUpMorphPlan(family, PushUpMode::kFull).empty());
            CHECK(BuildPushUpMorphPlan(family, PushUpMode::kSubtle).empty());
        }
        for (auto family : { BodyFamily::k3BA, BodyFamily::kCBBE, BodyFamily::kUBE,
                             BodyFamily::kHIMBO, BodyFamily::kGenericV1,
                             BodyFamily::kUnknown }) {
            CHECK(HasPushUpRecipe(family) ==
                  !BuildPushUpMorphPlan(family, PushUpMode::kFull).empty());
        }
    }

    if (!failures) std::cout << "BodyMorphPlanTests: all passed\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
