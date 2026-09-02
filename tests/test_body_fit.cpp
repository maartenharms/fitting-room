// Does a body preset suit the body the character is already on?
//
// Every case below is written from the same angle: the filter may only hide a
// preset when it can PROVE the preset is for another body. Anything it cannot
// classify stays on screen. The pickers that call this compile into no test, so
// this file is the only place the rule is checked at all.
#include "BodyPreset.h"

#include <cstdlib>
#include <iostream>

namespace {
    int failures = 0;
#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " << __LINE__ << ": " #x "\n"; ++failures; } } while (false)
}

int main() {
    using namespace OS;

    // ---- an unknown body hides nothing ------------------------------------
    {
        const BuiltBody nothing;
        CHECK(!nothing.Known());
        CHECK(BodyFitCompatible("CBBE 3BBB Body Amazing", BodyFamily::k3BA, nothing));
        CHECK(BodyFitCompatible("UBE", BodyFamily::kUBE, nothing));
        CHECK(BodyFitCompatible("HIMBO", BodyFamily::kHIMBO, nothing));
        CHECK(BodyFitCompatible("", BodyFamily::kUnknown, nothing));
    }

    // Half an answer still counts as an answer. A set with no family behind it
    // is the shape BodyMorphCatalog publishes when it resolved the set but the
    // catalog has not classified it, and a family with no set is what an
    // installed preset gives when its project file could not be read.
    {
        BuiltBody setOnly;
        setOnly.sourceSet = "UBE";
        CHECK(setOnly.Known());

        BuiltBody familyOnly;
        familyOnly.family = BodyFamily::kUBE;
        CHECK(familyOnly.Known());
    }

    // ---- the same slider set fits, whatever else disagrees -----------------
    {
        BuiltBody built;
        built.sourceSet = "CBBE 3BBB Body Amazing";
        built.family    = BodyFamily::k3BA;
        CHECK(BodyFitCompatible("CBBE 3BBB Body Amazing", BodyFamily::k3BA, built));
        // Case is BodySlide's business, not ours. The same set is spelt
        // differently across the files it appears in.
        CHECK(BodyFitCompatible("cbbe 3bbb body amazing", BodyFamily::k3BA, built));
        CHECK(BodyFitCompatible("CBBE 3BBB BODY AMAZING", BodyFamily::k3BA, built));
        // A prefix is NOT the same set.
        CHECK(!BodyFitCompatible("CBBE 3BBB", BodyFamily::kUBE, built));
        CHECK(!BodyFitCompatible("CBBE 3BBB Body Amazing Special", BodyFamily::kUBE, built));
    }

    // ---- a proven family mismatch is the only thing that hides -------------
    {
        BuiltBody built;
        built.sourceSet = "CBBE 3BBB Body Amazing";
        built.family    = BodyFamily::k3BA;
        CHECK(!BodyFitCompatible("UBE", BodyFamily::kUBE, built));
        CHECK(!BodyFitCompatible("HIMBO", BodyFamily::kHIMBO, built));
        // ⚠⚠ THIS ONE FLIPPED ON 2026-08-29 AND IT USED TO ASSERT THE
        // OPPOSITE. A catch-all against a named family was read as a proven
        // mismatch, and it is not one: kGenericV1 is what
        // FamilyFromSetSignature returns for every set whose name carries none
        // of himbo / ube / 3ba / 3bbb / cbbe, so it covers BHUNP and UNP and
        // Fusion Girl, but equally "OutfitStudioFrame", "TNG Default" and every
        // preset whose set name simply does not advertise a body. One enum
        // value was doing duty as both "a different body" and "we could not
        // tell", and only the first of those may hide anything. The mesh arm
        // still proves the real misfits: two sets that build different meshes
        // are caught above this line whatever their families say.
        CHECK(BodyFitCompatible("UNP Female Body", BodyFamily::kGenericV1, built));
        // A different set in the same family is still a fit: one body ships
        // several sets and presets move between them.
        CHECK(BodyFitCompatible("CBBE 3BBB Body Amazing SMP", BodyFamily::k3BA, built));
        CHECK(BodyFitCompatible("", BodyFamily::k3BA, built));
    }

    // ---- either side unclassified is not a mismatch ------------------------
    {
        BuiltBody built;
        built.sourceSet = "UBE";
        built.family    = BodyFamily::kUBE;
        // The preset could not be classified: the catalog has no entry for it,
        // its project file was unreadable, or the scan is still in flight.
        CHECK(BodyFitCompatible("Some Body", BodyFamily::kUnknown, built));
        CHECK(BodyFitCompatible("", BodyFamily::kUnknown, built));

        // The BODY could not be classified, but its set is known. A preset for
        // that same set fits; anything else is unproven and stays.
        BuiltBody setOnly;
        setOnly.sourceSet = "UBE";
        CHECK(BodyFitCompatible("UBE", BodyFamily::kUBE, setOnly));
        CHECK(BodyFitCompatible("CBBE 3BBB Body Amazing", BodyFamily::k3BA, setOnly));
    }

    // ⚠ A CATCH-ALL MATCHES ANYTHING, ON EITHER SIDE. kGenericV1 is not a
    // family, it is the absence of one, so it is never evidence: not against
    // another catch-all and not against a named family either. This is
    // deliberate, not an oversight.
    //
    // ⚠⚠ THE LAST LINE FLIPPED ON 2026-08-29, and it is the same reversal the
    // block above records. A player on a body this mod cannot name had every
    // named preset in their library hidden, and on the reference load order
    // nine of 231 survivors went that way; on a rig whose body IS a generic
    // one the ratio inverts and it is the CBBE, 3BA, UBE and HIMBO presets
    // that disappear instead.
    {
        BuiltBody built;
        built.sourceSet = "UNP Female Body";
        built.family    = BodyFamily::kGenericV1;
        CHECK(BodyFitCompatible("BHUNP 3BBB Advanced", BodyFamily::kGenericV1, built));
        CHECK(BodyFitCompatible("CBBE Body", BodyFamily::kGenericV1, built));
        CHECK(BodyFitCompatible("UBE", BodyFamily::kUBE, built));
    }

    // ---- the mesh arm still carries the negative ---------------------------
    //
    // ⚠⚠ THE POINT OF THE TWO REVERSALS ABOVE. Loosening the family arms costs
    // nothing that matters because a preset which builds a DIFFERENT mesh from
    // the character's is refused before any family is consulted, catch-all or
    // not. What the family arms were doing on their own was hiding presets on
    // the strength of a name we had failed to parse.
    {
        BuiltBody built;
        built.meshKey = "actors\\character\\character assets\\femalebody";
        built.family  = BodyFamily::k3BA;
        CHECK(!BodyFitCompatible("HIMBO Body - SOS", BodyFamily::kGenericV1,
                                 "actors\\character\\character assets\\malebody",
                                 built));
        CHECK(BodyFitCompatible("UNP Female Body", BodyFamily::kGenericV1,
                                "actors\\character\\character assets\\femalebody",
                                built));
    }

    // ---- the whole-preset overload reads the same two fields ---------------
    {
        BuiltBody built;
        built.sourceSet = "CBBE 3BBB Body Amazing";
        built.family    = BodyFamily::k3BA;

        BodyPreset preset;
        preset.name      = "Curvy Custom";
        preset.sex       = BodySex::kFemale;
        preset.family    = BodyFamily::k3BA;
        preset.sourceSet = "CBBE 3BBB Body Amazing";
        CHECK(BodyFitCompatible(preset, built));

        preset.family    = BodyFamily::kUBE;
        preset.sourceSet = "UBE";
        CHECK(!BodyFitCompatible(preset, built));

        // ⚠ FIT IS NOT SEX, AND NEITHER SUBSUMES THE OTHER. The pickers ask
        // both questions; a male preset in a matching family is still wrong for
        // a female character.
        preset.family    = BodyFamily::k3BA;
        preset.sourceSet = "CBBE 3BBB Body Amazing";
        preset.sex       = BodySex::kMale;
        CHECK(BodyFitCompatible(preset, built));
        CHECK(!BodySexCompatible(preset.sex, true));
    }

    // ---- a HIMBO character is the mirror of the above ----------------------
    {
        BuiltBody built;
        built.sourceSet = "HIMBO";
        built.family    = BodyFamily::kHIMBO;
        CHECK(BodyFitCompatible("HIMBO", BodyFamily::kHIMBO, built));
        CHECK(!BodyFitCompatible("CBBE 3BBB Body Amazing", BodyFamily::k3BA, built));
        CHECK(BodyFitCompatible("Vanilla Male", BodyFamily::kUnknown, built));
    }

    if (failures == 0) {
        std::cout << "body fit tests passed\n";
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
