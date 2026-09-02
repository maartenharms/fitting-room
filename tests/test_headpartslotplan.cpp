// Pure-logic tests for discovered head-part SLOTS (HeadPartSlotPlan.h). No
// engine, no RE:: types - just the decisions the discovery loop reduces to.
//
// The cases that carry this file are the label ladder's refusals. A slot has no
// name anywhere in its record, so the label is scavenged from a convention, and
// every arm of that ladder is a guess that has to fail into the next one rather
// than into an empty string. A row with no name reads as a rendering fault.
#include "HeadPartSlotPlan.h"

#include <cstdio>
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

namespace {

    using namespace OS::HeadPartSlotPlan;

    void TestCustomTypeBoundary() {
        // The seven the engine names are never slots of their own. kEyebrows is
        // 6 and is the last of them, so 6 is vanilla and 7 is the first custom
        // number even though no mod on this rig uses 7.
        CHECK(!IsCustomType(0));  // kMisc
        CHECK(!IsCustomType(2));  // kEyes
        CHECK(!IsCustomType(3));  // kHair
        CHECK(!IsCustomType(4));  // kFacialHair
        CHECK(!IsCustomType(6));  // kEyebrows, the last named one
        CHECK(IsCustomType(7));   // kTotal, the first number nobody named
        // The three MEASURED on this rig 2026-08-13.
        CHECK(IsCustomType(32));   // NK_HornSlider.esp
        CHECK(IsCustomType(106));  // ED Horns RMIntegration.esp
        CHECK(IsCustomType(110));  // ChooeyDintEarsEdit.esp
    }

    void TestLabelPrefersTheTranslation() {
        // The two MEASURED files, both holding exactly one key.
        CHECK(LabelFor(106, "ED Horns", "ED Horns RMIntegration.esp") == "ED Horns");
        CHECK(LabelFor(110, "Chooey's Dint Ears Edit", "ChooeyDintEarsEdit.esp") ==
              "Chooey's Dint Ears Edit");
    }

    void TestLabelFallsBackToThePluginStem() {
        // No translation file, or one this ladder refused. The plugin name is
        // what the player sees in their mod manager, so it is a real answer
        // rather than a placeholder.
        CHECK(LabelFor(32, "", "NK_HornSlider.esp") == "NK_HornSlider");
        // Only the LAST dot is the extension. A plugin with a dotted name keeps
        // the rest of it.
        CHECK(LabelFor(32, "", "My.Horns.esp") == "My.Horns");
        // An extensionless name is not truncated to nothing.
        CHECK(LabelFor(32, "", "Horns") == "Horns");
    }

    void TestLabelIsNeverEmpty() {
        // Both sources gone. The number is useless to a player and still beats
        // a blank row, which reads as a bug in the browser.
        CHECK(LabelFor(32, "", "") == "Head part type 32");
        // A plugin name that is nothing but an extension leaves an empty stem,
        // which must not become the label.
        CHECK(LabelFor(110, "", ".esp") == "Head part type 110");
    }

    void TestSoleTranslationValue() {
        // The MEASURED shape: one key, a tab, the value.
        CHECK(SoleTranslationValue("$PRMI_EDHorns\tED Horns") == "ED Horns");
        // A trailing newline is ordinary and is not a second line.
        CHECK(SoleTranslationValue("$PRMI_EDHorns\tED Horns\n") == "ED Horns");
        // CRLF, which is what these files actually carry.
        CHECK(SoleTranslationValue("$Chooey_DintEarsEdit\tChooey's Dint Ears Edit\r\n") ==
              "Chooey's Dint Ears Edit");
        // A BOM survives the UTF-16 decode and lands on the key, which is
        // discarded, so it must not disturb the value.
        CHECK(SoleTranslationValue("\xEF\xBB\xBF$PRMI_EDHorns\tED Horns") == "ED Horns");
        // Blank lines are not keys.
        CHECK(SoleTranslationValue("\n\n$PRMI_EDHorns\tED Horns\n\n") == "ED Horns");
        // A value can contain spaces and apostrophes and keeps them.
        CHECK(SoleTranslationValue("$k\tTwo Words") == "Two Words");
    }

    void TestSoleTranslationValueRefusesAmbiguity() {
        // ⚠ THE CASE THE WHOLE LADDER TURNS ON. Two keys cannot say which slot
        // either one names, so the file is refused rather than guessed at, and
        // the plugin stem answers instead.
        CHECK(SoleTranslationValue("$a\tHorns\n$b\tEars").empty());
        // Three is the same refusal.
        CHECK(SoleTranslationValue("$a\tOne\n$b\tTwo\n$c\tThree").empty());
        // A file with no tab has no pairs at all.
        CHECK(SoleTranslationValue("just some text").empty());
        // A key with an empty value is not a usable pair, so a file holding one
        // real pair and one empty one still answers.
        CHECK(SoleTranslationValue("$a\t\n$b\tEars") == "Ears");
        // Nothing at all.
        CHECK(SoleTranslationValue("").empty());
    }

    void TestSortIsByTypeNumber() {
        // ⚠ BY NUMBER, NOT BY LABEL. The type is the only thing about a slot
        // that is stable across sessions; sorting by label moves the row under
        // the player's cursor when a mod is renamed or a translation is added.
        std::vector<Slot> slots{
            { 110, 11, "ChooeyDintEarsEdit.esp", "Chooey's Dint Ears Edit" },
            { 32, 357, "NK_HornSlider.esp", "NK_HornSlider" },
            { 106, 84, "ED Horns RMIntegration.esp", "ED Horns" },
        };
        SortForDisplay(slots);
        CHECK(slots.size() == 3);
        CHECK(slots[0].type == 32);
        CHECK(slots[1].type == 106);
        CHECK(slots[2].type == 110);
        // The rest of the record travels with its type.
        CHECK(slots[0].parts == 357);
        CHECK(slots[1].label == "ED Horns");
        CHECK(slots[2].plugin == "ChooeyDintEarsEdit.esp");
    }

    void TestSortHandlesTheDegenerateSizes() {
        std::vector<Slot> none;
        SortForDisplay(none);
        CHECK(none.empty());

        std::vector<Slot> one{ { 106, 84, "p.esp", "ED Horns" } };
        SortForDisplay(one);
        CHECK(one.size() == 1);
        CHECK(one[0].type == 106);

        // Already ordered stays ordered.
        std::vector<Slot> sorted{ { 7, 1, "a.esp", "A" }, { 8, 1, "b.esp", "B" } };
        SortForDisplay(sorted);
        CHECK(sorted[0].type == 7);
        CHECK(sorted[1].type == 8);
    }

}  // namespace

int main() {
    TestCustomTypeBoundary();
    TestLabelPrefersTheTranslation();
    TestLabelFallsBackToThePluginStem();
    TestLabelIsNeverEmpty();
    TestSoleTranslationValue();
    TestSoleTranslationValueRefusesAmbiguity();
    TestSortIsByTypeNumber();
    TestSortHandlesTheDegenerateSizes();

    if (g_failures != 0) {
        std::printf("HeadPartSlotPlanTests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all HeadPartSlotPlan tests passed\n");
    return 0;
}
