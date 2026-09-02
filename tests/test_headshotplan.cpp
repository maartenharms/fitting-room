// Pure-logic tests for what the camera frames while a head part is browsed
// (HeadShotPlan.h). No engine, no RE:: types.
//
// The case that carries this file is the DISCOVERED SLOT. Horns and cat ears
// are not HeadPart::Kind values and never can be, so a reader that answers
// "what is the shot on" by asking for a Kind says "nothing" for every one of
// them, and the caller walks on into a branch that frames the body.
#include "HeadShotPlan.h"

#include <cstdio>
#include <iterator>
#include <string>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace {

    using namespace OS::HeadShotPlan;
    using Kind = OS::HeadPart::Kind;
    using Slot = OS::HeadPart::Slot;

    Selection OfKind(Kind a_kind) {
        Selection s;
        s.kind = a_kind;
        return s;
    }

    Selection OfSlot(Slot a_slot) {
        Selection s;
        s.slot = a_slot;
        return s;
    }

    const Shot kHairShot{ "NPC Head [Head]", 0.85f };
    const Shot kFaceShot{ "NPC Head [Head]", 0.95f };

    // The three MEASURED on the reference rig 2026-08-13: NK_HornSlider on 32,
    // ED Horns on 106, Chooey's ears on 110.
    const Slot kMeasuredSlots[] = { 32u, 106u, 110u };

    void TestTheFourNamedKindsKeepTheirShots() {
        CHECK(ShotFor(OfKind(Kind::kHair)) == kHairShot);
        CHECK(ShotFor(OfKind(Kind::kEyes)) == kFaceShot);
        CHECK(ShotFor(OfKind(Kind::kBrows)) == kFaceShot);
        CHECK(ShotFor(OfKind(Kind::kFacialHair)) == kFaceShot);
    }

    void TestADiscoveredSlotFramesTheHead() {
        // ⚠⚠ THE REGRESSION THIS FILE EXISTS FOR (user 2026-08-19: "when we are
        // selecting the head extras we are focused on the torso instead of
        // being properly focused on the head"). None of these is a Kind, so a
        // Kind-only reader returned an invalid shot and EditorUI fell through to
        // its armour branch, which defaults to the body bit and turns the camera
        // on NPC Spine2 [Spn2].
        for (const Slot slot : kMeasuredSlots) {
            CHECK(ShotFor(OfSlot(slot)).Valid());
            CHECK(ShotFor(OfSlot(slot)) == kHairShot);
        }
    }

    void TestADiscoveredSlotTakesHairDistanceNotFaceDistance() {
        // Not a taste call. A discovered slot borrows SceneKind::kHair
        // everywhere else in the editor, documented there as "a head and
        // shoulders under the horn". A horn reaches above the skull and face
        // distance would crop it.
        CHECK(ShotFor(OfSlot(32)).closeness == ShotFor(OfKind(Kind::kHair)).closeness);
        CHECK(ShotFor(OfSlot(32)).closeness != ShotFor(OfKind(Kind::kEyes)).closeness);
    }

    void TestNothingSelectedAsksNothingOfTheCamera() {
        CHECK(!Selection{}.Any());
        CHECK(!ShotFor(Selection{}).Valid());
        CHECK(KeyFor(Selection{}).empty());
    }

    void TestEverySelectionGetsItsOwnKey() {
        // The driver fires on a CHANGE of this string, so two head dimensions
        // sharing one key means picking the second never re-frames.
        const std::string keys[] = {
            KeyFor(OfKind(Kind::kHair)),  KeyFor(OfKind(Kind::kEyes)),
            KeyFor(OfKind(Kind::kBrows)), KeyFor(OfKind(Kind::kFacialHair)),
            KeyFor(OfSlot(32)),           KeyFor(OfSlot(106)),
            KeyFor(OfSlot(110)),
        };
        for (const auto& key : keys) {
            CHECK(!key.empty());
        }
        for (std::size_t i = 0; i < std::size(keys); ++i) {
            for (std::size_t j = i + 1; j < std::size(keys); ++j) {
                CHECK(keys[i] != keys[j]);
            }
        }
    }

    void TestASlotKeyCannotCollideWithAnArmourKey() {
        // EditorUI's armour branch returns "s" plus the biped bit. Slot 32 is
        // also a biped bit, so a key shaped the same way would make the horn row
        // and that armour slot one selection.
        for (const Slot slot : kMeasuredSlots) {
            CHECK(KeyFor(OfSlot(slot)) != "s" + std::to_string(slot));
        }
    }

    void TestAKindOutranksAStraySlot() {
        // The editor clears one when it sets the other, so this pairing should
        // never reach here. If it ever does, the named kind is the answer.
        Selection both;
        both.kind = Kind::kEyes;
        both.slot = 32;
        CHECK(ShotFor(both) == kFaceShot);
        CHECK(KeyFor(both) == KeyFor(OfKind(Kind::kEyes)));
    }

}  // namespace

int main() {
    TestTheFourNamedKindsKeepTheirShots();
    TestADiscoveredSlotFramesTheHead();
    TestADiscoveredSlotTakesHairDistanceNotFaceDistance();
    TestNothingSelectedAsksNothingOfTheCamera();
    TestEverySelectionGetsItsOwnKey();
    TestASlotKeyCannotCollideWithAnArmourKey();
    TestAKindOutranksAStraySlot();

    if (g_failures != 0) {
        std::printf("HeadShotPlanTests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all HeadShotPlan tests passed\n");
    return 0;
}
