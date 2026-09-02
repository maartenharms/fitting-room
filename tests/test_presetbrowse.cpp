// Preset browser tests. No SKSE, no engine.
//
// ⚠ FIXTURES ARE FORGED FROM REAL FILES, per house rule. Provenance, read
// off the live rig 2026-08-22 01:1x (the W3 handoff's measured inventory):
//   * preset names: `(002026 July) Umbrael.jslot` and `!UBE_RavenMay2026.jslot`
//     from the user's own CharGen\Presets (112 files).
//   * the FR_ capture family: `FR_Umbrael.jslot` .. `FR_Umbrael 3.jslot`,
//     tonight's page saves in CharGen\Exported.
//   * the extension pileup: `!UBE_Umbrael_August_2026.nif.nif.nif.nif.nif.nif`
//     in the CharGen root - RaceMenu appends rather than replaces when the
//     typed name already ends in .nif.
#include "PresetBrowse.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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

    void Touch(const std::filesystem::path& a_path) {
        std::ofstream out(a_path);
        out << "{}";
    }

}  // namespace

int main() {
    using namespace OS::PresetBrowse;

    // ---- NormalizeExportName -------------------------------------------

    // The measured pileup: six .nif layers collapse to the bare stem.
    CHECK(NormalizeExportName(
              "!UBE_Umbrael_August_2026.nif.nif.nif.nif.nif.nif") ==
          "!UBE_Umbrael_August_2026");
    // One layer, mixed case, surrounding whitespace.
    CHECK(NormalizeExportName("  MyHead.NIF ") == "MyHead");
    // jslot and binary-slot layers strip too, including stacked kinds.
    CHECK(NormalizeExportName("Umbrael.jslot") == "Umbrael");
    CHECK(NormalizeExportName("Umbrael.slot") == "Umbrael");
    CHECK(NormalizeExportName("Umbrael.jslot.nif") == "Umbrael");
    // Real preset punctuation survives; hostile characters become
    // underscores.
    CHECK(NormalizeExportName("(002026 July) Umbrael") ==
          "(002026 July) Umbrael");
    CHECK(NormalizeExportName("!UBE_RavenMay2026") == "!UBE_RavenMay2026");
    CHECK(NormalizeExportName("a/b\\c:d") == "a_b_c_d");
    // Empty and whitespace-only stay empty: the button stays disabled.
    CHECK(NormalizeExportName("").empty());
    CHECK(NormalizeExportName("   ").empty());
    // A name that is ONLY extension layers normalises to empty too.
    CHECK(NormalizeExportName(".nif.nif").empty());

    // ---- ListPresets ---------------------------------------------------

    const auto root = std::filesystem::temp_directory_path() /
                      "fr_presetbrowse_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    // A missing directory is an empty list, never a throw.
    CHECK(ListPresets(root / "no_such_dir", false).empty());

    // The rig's own names, plus a json/binary pair and an FR_ family.
    Touch(root / "(002026 July) Umbrael.jslot");
    Touch(root / "!UBE_RavenMay2026.jslot");
    Touch(root / "!UBE_RavenMay2026.slot");  // binary twin: one row
    Touch(root / "FR_Umbrael.jslot");
    Touch(root / "FR_Umbrael 2.jslot");
    Touch(root / "readme.txt");  // not a preset, ignored

    const auto all = ListPresets(root, false);
    CHECK(all.size() == 4);
    // Case-insensitive sort puts the paren name after the bang names.
    CHECK(all.size() == 4 && all[0].stem == "!UBE_RavenMay2026");
    CHECK(all.size() == 4 && all[1].stem == "(002026 July) Umbrael");
    CHECK(all.size() == 4 && all[2].stem == "FR_Umbrael");
    CHECK(all.size() == 4 && all[3].stem == "FR_Umbrael 2");

    const auto noFr = ListPresets(root, true);
    CHECK(noFr.size() == 2);
    for (const auto& entry : noFr) {
        CHECK(!entry.stem.starts_with("FR_"));
    }

    std::filesystem::remove_all(root);

    if (g_failures == 0) {
        std::printf("PresetBrowseTests: all passed\n");
        return 0;
    }
    std::printf("PresetBrowseTests: %d failure(s)\n", g_failures);
    return 1;
}
