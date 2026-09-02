// Pure-logic tests for the TRIP reader (BodyMorphTri.h). No engine, no
// filesystem: the input is a byte string and the output is a list of names.
//
// This file exists because of one field round. The Shape page decided which
// sliders to offer by resolving a body preset to a BodySlide slider SET, and on
// 2026-08-16 the preset 'Stoppu_2' named set "UBE 3.0 Release Preview", which
// does not exist in any of the 1611 .osp files on the reference load order: the
// preset was written against UBE 3.0 and UBE 2.0 TNG is what is installed. The
// resolve failed, the filter fell open, and 678 sliders were offered of which
// 49 belonged to a body that was never built. Those 49 stored a value under our
// key and moved nothing, which on screen is a control that does nothing at all.
//
// The tri is the answer to the question the set was being asked to approximate:
// a name in it can move the mesh and a name absent from it cannot.
//
// ⚠ THE FIXTURES ARE FORGED BYTE BY BYTE, NOT TRUNCATED FROM A REAL FILE. A
// truncated fixture can only test the cases that happen to be prefixes of the
// one file it came from, and the layout below was MEASURED against all 3238
// .tri files on the reference load order: every one begins "PIRT", 1711 carry
// more than one shape and the largest carries 36.
#include "BodyMorphTri.h"

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

    void PutU16(std::string& a_out, std::uint16_t a_v) {
        a_out.push_back(static_cast<char>(a_v & 0xFF));
        a_out.push_back(static_cast<char>((a_v >> 8) & 0xFF));
    }

    void PutStr(std::string& a_out, const std::string& a_s) {
        a_out.push_back(static_cast<char>(a_s.size()));
        a_out += a_s;
    }

    // One morph: name, a four-byte multiplier, a vertex count and that many
    // eight-byte diffs. The diffs are filler on purpose - the reader must skip
    // them by arithmetic and never look at them.
    void PutMorph(std::string& a_out, const std::string& a_name, std::uint16_t a_verts) {
        PutStr(a_out, a_name);
        a_out.append(4, '\xAB');  // multiplier
        PutU16(a_out, a_verts);
        a_out.append(static_cast<std::size_t>(a_verts) * 8u, '\xCD');
    }

    [[nodiscard]] bool Has(const std::vector<std::string>& a_names, const std::string& a_want) {
        for (const auto& n : a_names) {
            if (n == a_want) {
                return true;
            }
        }
        return false;
    }

}  // namespace

int main() {
    using OS::BodyMorphTri::ParseNames;
    using OS::BodyMorphTri::TriPathFor;

    // ---- a single-shape file, the shape a body tri has --------------------
    {
        std::string t = "PIRT";
        PutU16(t, 1);  // one shape
        PutStr(t, "BaseShape");
        PutU16(t, 3);  // three morphs
        PutMorph(t, "Amazon", 4);
        PutMorph(t, "anime breast", 0);  // a zero-vertex morph is still a name
        PutMorph(t, "BreastsBigger", 12);

        const auto names = ParseNames(t);
        CHECK(names.size() == 3);
        CHECK(Has(names, "Amazon"));
        CHECK(Has(names, "BreastsBigger"));
        // ⚠ A ZERO-VERTEX MORPH IS A REAL NAME. Real files carry them
        // (ube_penis.tri declares its shape and no morphs at all), and a reader
        // that dropped them would hide a working slider.
        CHECK(Has(names, "anime breast"));
    }

    // ---- more than one shape, which is the MAJORITY of real files ---------
    //
    // 1711 of 3238 measured files are multi-shape. A reader that stopped after
    // the first shape would silently lose the rest, and the loss would look
    // exactly like the bug this whole feature is fixing.
    {
        std::string t = "PIRT";
        PutU16(t, 3);
        PutStr(t, "Body");
        PutU16(t, 1);
        PutMorph(t, "OnlyOnBody", 2);
        PutStr(t, "Hands");
        PutU16(t, 0);  // a shape with no morphs at all
        PutStr(t, "Feet");
        PutU16(t, 2);
        PutMorph(t, "OnlyOnFeet", 1);
        // ⚠ THE SAME NAME ON TWO SHAPES IS NOT DEDUPED HERE. The caller builds
        // a set; deduping twice costs a second pass and hides nothing.
        PutMorph(t, "OnlyOnBody", 3);

        const auto names = ParseNames(t);
        CHECK(names.size() == 3);
        CHECK(Has(names, "OnlyOnBody"));
        CHECK(Has(names, "OnlyOnFeet"));
    }

    // ---- anything that is not a TRIP file reads as no names ---------------
    {
        CHECK(ParseNames("").empty());
        CHECK(ParseNames("PIRT").empty());          // magic and nothing else
        CHECK(ParseNames("FRTI\x01\x00").empty());  // wrong magic
        // A NIF handed in by mistake must not parse as a tri.
        CHECK(ParseNames("Gamebryo File Format").empty());
    }

    // ---- truncation gives NOTHING, never a partial list -------------------
    //
    // ⚠⚠ THIS IS THE ONE THAT MATTERS. A half-read file yields a short name
    // list, the caller filters with it, and every slider whose name fell off
    // the end goes grey for no reason the player can see. "Some of your
    // sliders vanished" is a worse failure than "the filter did not run", so a
    // short read returns empty and the caller falls back to filtering nothing.
    {
        std::string full = "PIRT";
        PutU16(full, 1);
        PutStr(full, "BaseShape");
        PutU16(full, 2);
        PutMorph(full, "First", 6);
        PutMorph(full, "Second", 6);
        CHECK(ParseNames(full).size() == 2);

        for (std::size_t cut = 1; cut < full.size(); ++cut) {
            const auto partial = ParseNames(full.substr(0, cut));
            // Either it is empty, or the cut happened to land on a valid whole
            // file - which it cannot here, because every prefix stops inside a
            // record. Empty is the only correct answer.
            CHECK(partial.empty());
        }
    }

    // ---- a corrupt count is refused rather than reserved against ----------
    {
        std::string t = "PIRT";
        PutU16(t, 0xFFFF);  // 65535 shapes in a six-byte file
        PutStr(t, "BaseShape");
        CHECK(ParseNames(t).empty());

        // A vertex count far past the end of the buffer stops the parse instead
        // of skipping off it.
        std::string v = "PIRT";
        PutU16(v, 1);
        PutStr(v, "BaseShape");
        PutU16(v, 1);
        PutStr(v, "Runaway");
        v.append(4, '\xAB');
        PutU16(v, 0xFFFF);  // claims 524280 bytes of diffs that are not there
        CHECK(ParseNames(v).empty());
    }

    // ---- trailing bytes are TOLERATED, unlike the co-save codec -----------
    //
    // This is somebody else's format. A future BodySlide appending to it must
    // not read as "every slider stopped working", which is what refusing the
    // whole file over unexpected bytes would do.
    {
        std::string t = "PIRT";
        PutU16(t, 1);
        PutStr(t, "BaseShape");
        PutU16(t, 1);
        PutMorph(t, "Kept", 1);
        t += "something a later version appended";
        const auto names = ParseNames(t);
        CHECK(names.size() == 1);
        CHECK(Has(names, "Kept"));
    }

    // ---- the tri that belongs to a built mesh -----------------------------
    {
        // The weight suffix comes off: BodySlide builds a _0 and a _1 and ONE
        // tri for the pair.
        CHECK(TriPathFor("!UBE\\Body\\femalebody_tangent_1.nif") ==
              "!UBE/Body/femalebody_tangent.tri");
        CHECK(TriPathFor("!UBE\\Body\\femalebody_tangent_0.nif") ==
              "!UBE/Body/femalebody_tangent.tri");
        // ⚠ VANILLA'S OWN RECORDS SPELL THE EXTENSION IN CAPITALS. The skin
        // ARMA for the male body is 'MaleBody_1.NIF' on this install, so a
        // case-sensitive extension test would drop the body it was written for.
        CHECK(TriPathFor("Actors\\Character\\Character Assets\\MaleBody_1.NIF") ==
              "Actors/Character/Character Assets/MaleBody.tri");
        // No weight suffix keeps the whole stem, which is what a head needs.
        CHECK(TriPathFor("!UBE\\Head\\femaleHead_tangent.nif") ==
              "!UBE/Head/femaleHead_tangent.tri");
        // A digit that is not a weight suffix is left alone.
        CHECK(TriPathFor("meshes\\thing2.nif") == "meshes/thing2.tri");
        // Not a mesh, so there is nothing to look for.
        CHECK(TriPathFor("").empty());
        CHECK(TriPathFor("femalebody_1.tri").empty());
        CHECK(TriPathFor("noextension").empty());
        CHECK(TriPathFor("_1.nif").empty());
    }

    if (g_failures == 0) {
        std::printf("BodyMorphTriTests: all passed\n");
        return 0;
    }
    std::printf("BodyMorphTriTests: %d failure(s)\n", g_failures);
    return 1;
}
