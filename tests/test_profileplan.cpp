// Apply-order tests. The sequence IS the two-painter resolution (spec
// section 6), so the full order is pinned as one literal list of the log
// labels: a reorder is a regression even when every step passes alone, and
// this is the test the spec's section 9 asks for.
#include "ProfilePlan.h"

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

static std::vector<std::string> Labels(const std::vector<OS::ProfilePlan::Step>& a_steps) {
    std::vector<std::string> out;
    for (const auto step : a_steps) out.emplace_back(OS::ProfilePlan::LogLabel(step));
    return out;
}

int main() {
    using namespace OS::ProfilePlan;

    {  // ⚠⚠ THE order, as log labels. Character first (it decides WHO the
       // rest applies to, the 2026-08-22 male-body field report made
       // executable), face second, weight before body and shape, overlays
       // last. Changing this list is changing the two-painter resolution;
       // do not "fix" this test to match a reorder.
        const auto full = Labels(Build(AllOn()));
        const std::vector<std::string> pinned{ "character", "face", "outfit",
                                               "makeup", "weight", "body",
                                               "shape", "skin", "overlays" };
        CHECK(full == pinned);
    }

    {  // participation filters, never reorders: dropping blocks keeps the
       // relative order of everything that remains
        Participation take = AllOn();
        take.character = false;
        take.face   = false;
        take.weight = false;
        take.skin   = false;
        const auto some = Labels(Build(take));
        const std::vector<std::string> pinned{ "outfit", "makeup", "body",
                                               "shape", "overlays" };
        CHECK(some == pinned);

        CHECK(Build(Participation{}).empty());
    }

    {  // The character block participates like any other: presence drives
       // the default, the checkbox masks it, and its step leads. Forged from
       // the field pair the switch exists for: a look captured on
       // UBE_AllRace.esp|05A198 female, applied to somebody else.
        OS::ProfileCodec::Profile p;
        p.name = "Umbrael 5";
        OS::ProfileCodec::CharacterBlock character;
        character.race   = OS::StyleRefKey{ "UBE_AllRace.esp", 0x05A198 };
        character.female = true;
        p.character      = character;
        p.skin           = OS::ProfileCodec::SkinBlock{ "Sayble 4K" };
        const auto take = ParticipationFor(p);
        CHECK(take.character);
        CHECK(Labels(Build(And(take, AllOn()))) ==
              (std::vector<std::string>{ "character", "skin" }));

        Participation noSwitch = AllOn();
        noSwitch.character     = false;  // the user unticked Character
        CHECK(Labels(Build(And(take, noSwitch))) ==
              std::vector<std::string>{ "skin" });
    }

    {  // ParticipationFor mirrors block presence, and an empty makeup list is
       // absence (nothing to write is not a step)
        OS::ProfileCodec::Profile p;
        p.name = "Aria";
        p.skin = OS::ProfileCodec::SkinBlock{ "UBE" };
        p.makeup.emplace();  // present but empty
        const auto take = ParticipationFor(p);
        CHECK(take.skin);
        CHECK(!take.makeup);
        CHECK(!take.face && !take.outfit && !take.dyes && !take.weight &&
              !take.body && !take.shape && !take.overlays);
        const auto steps = Labels(Build(take));
        CHECK(steps == std::vector<std::string>{ "skin" });
    }

    {  // The dye half rides the outfit step: either checkbox alone still
       // takes the ONE step, and neither adds a new label to the pinned
       // order (a ninth label would be a reorder by another name).
        OS::ProfileCodec::Profile p;
        p.name   = "Aria";
        p.outfit = OS::Outfit{};
        const auto take = ParticipationFor(p);
        CHECK(take.outfit && take.dyes);

        Participation dyesOnly = AllOn();
        dyesOnly.outfit        = false;
        CHECK(Labels(Build(And(take, dyesOnly))) ==
              std::vector<std::string>{ "outfit" });

        Participation gearOnly = AllOn();
        gearOnly.dyes          = false;
        CHECK(Labels(Build(And(take, gearOnly))) ==
              std::vector<std::string>{ "outfit" });

        Participation neither = AllOn();
        neither.outfit        = false;
        neither.dyes          = false;
        CHECK(Build(And(take, neither)).empty());
    }

    {  // And() is the page's checkbox mask over the profile's presence
        OS::ProfileCodec::Profile p;
        p.name     = "Aria";
        p.face     = OS::ProfileCodec::FaceBlock{ "FR_Aria" };
        p.skin     = OS::ProfileCodec::SkinBlock{ "UBE" };
        p.weight   = 58.1f;
        Participation boxes = AllOn();
        boxes.face          = false;  // the user unticked the face
        const auto steps = Labels(Build(And(ParticipationFor(p), boxes)));
        const std::vector<std::string> pinned{ "weight", "skin" };
        CHECK(steps == pinned);
    }

    {  // Count() is what the Looks page's "3 of 8 parts" line reads
        CHECK(Count(Participation{}) == 0);
        CHECK(Count(AllOn()) == 10);
        Participation some;
        some.face   = true;
        some.outfit = true;
        some.dyes   = true;
        CHECK(Count(some) == 3);
    }

    {  // EveryUsable() is the All button: present blocks, minus what the
       // rig cannot honour. Never AllOn() - a tick on an absent block is a
       // promise the apply cannot keep.
        OS::ProfileCodec::Profile p;
        p.name   = "Aria";
        p.face   = OS::ProfileCodec::FaceBlock{ "FR_Aria" };
        p.weight = 58.1f;
        const auto present = ParticipationFor(p);
        const auto withFace = EveryUsable(present, true);
        CHECK(withFace.face);
        CHECK(withFace.weight);
        CHECK(!withFace.skin);  // the profile has no skin block to take
        // RaceMenu absent: the face cannot apply, so All must not claim it.
        const auto noFace = EveryUsable(present, false);
        CHECK(!noFace.face);
        CHECK(noFace.weight);
        CHECK(Count(noFace) == Count(withFace) - 1);
        // And the All button's own end state: pressing it can never widen
        // what Apply will actually do.
        CHECK(Labels(Build(And(noFace, present))) ==
              Labels(Build(noFace)));
    }

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("all profile plan tests passed\n");
    return 0;
}
