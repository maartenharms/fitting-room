// The body mesh key, which is what lets a race's skin and a BodySlide slider
// set be compared at all. The two sources spell the same mesh differently, so
// every case below is really the same question: do these two spellings reduce
// to the same body?
#include "BodyMeshPath.h"

#include <cstdlib>
#include <iostream>

namespace {
    int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; ++failures; } } while (false)
}

int main() {
    using namespace OS;

    // ---- the case the whole feature rests on ------------------------------
    // What the engine hands back for a vanilla-pathed body, against what
    // BodySlide says it builds. These must meet.
    {
        const auto fromActor =
            BodyMeshKey("actors\\character\\character assets\\femalebody_1.nif");
        const auto fromSet = BodyMeshKey(
            "meshes\\actors\\character\\character assets", "femalebody");
        CHECK(fromActor == fromSet);
        CHECK(fromActor == "actors\\character\\character assets\\femalebody");
    }

    // ---- and the case it exists to separate -------------------------------
    {
        const auto ube = BodyMeshKey("actors\\character\\UBE\\femalebody_1.nif");
        const auto cbbe =
            BodyMeshKey("actors\\character\\character assets\\femalebody_1.nif");
        CHECK(ube != cbbe);
        CHECK(BodyMeshKeysComparable(ube, cbbe));
    }

    // ---- the weight suffix ------------------------------------------------
    // ⚠ _0 and _1 are the same body. The engine names one weight, BodySlide
    // names the pair, and keeping the suffix would make every honest match fail
    // and hide the entire list.
    {
        CHECK(BodyMeshKey("a\\femalebody_0.nif") == BodyMeshKey("a\\femalebody_1.nif"));
        CHECK(BodyMeshKey("a\\femalebody_1.nif") == BodyMeshKey("a\\femalebody"));
        // A trailing digit that is not a weight suffix stays.
        CHECK(BodyMeshKey("a\\body2.nif") == "a\\body2");
        CHECK(BodyMeshKey("a\\body_2.nif") == "a\\body_2");
    }

    // ---- spelling differences between the two sources ---------------------
    {
        // Case never matters on this platform and the two sources disagree.
        CHECK(BodyMeshKey("Actors\\Character\\Character Assets\\FemaleBody_1.nif") ==
              BodyMeshKey("actors\\character\\character assets\\femalebody_1.nif"));
        // Forward slashes appear in hand-edited files.
        CHECK(BodyMeshKey("actors/character/femalebody_1.nif") ==
              BodyMeshKey("actors\\character\\femalebody_1.nif"));
        // Joining a path that already ends in a separator doubles it.
        CHECK(BodyMeshKey("meshes\\actors\\", "femalebody") ==
              BodyMeshKey("meshes\\actors", "femalebody"));
        // A Data\ prefix appears on the BodySlide side of some installs.
        CHECK(BodyMeshKey("Data\\meshes\\actors\\femalebody_1.nif") ==
              BodyMeshKey("actors\\femalebody_1.nif"));
        // Trailing separators and spaces.
        CHECK(BodyMeshKey("actors\\femalebody_1.nif  ") ==
              BodyMeshKey("actors\\femalebody_1.nif"));
    }

    // ---- unknown is unknown, never "matches nothing" ----------------------
    // ⚠ Every caller fails open on an empty key. A set with no OutputFile tells
    // us nothing about which body it builds, and a race whose skin could not be
    // read tells us nothing about the character.
    {
        CHECK(BodyMeshKey("").empty());
        CHECK(BodyMeshKey("meshes\\actors", "").empty());
        CHECK(BodyMeshKey("", "").empty());
        CHECK(!BodyMeshKeysComparable("", "actors\\femalebody"));
        CHECK(!BodyMeshKeysComparable("actors\\femalebody", ""));
        CHECK(!BodyMeshKeysComparable("", ""));
        CHECK(BodyMeshKeysComparable("a", "b"));
    }

    // ---- a set with no path still keys on its file ------------------------
    {
        CHECK(BodyMeshKey("", "femalebody") == "femalebody");
    }

    // ---- male and female bodies are different meshes ----------------------
    {
        CHECK(BodyMeshKey("actors\\character\\character assets\\femalebody_1.nif") !=
              BodyMeshKey("actors\\character\\character assets\\malebody_1.nif"));
    }

    if (failures == 0) {
        std::cout << "body mesh path tests passed\n";
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
