#pragma once

#include <filesystem>
#include <string>
#include <vector>

// The RaceMenu preset browser's engine-free half: directory listings and the
// export-name normaliser, kept pure so the rig's own filename zoo (the
// 112-preset library, the .slot binaries, the .nif.nif.nif pileup) can pin
// them in a test executable.
//
// ⚠⚠ FILENAMES ONLY, the standing rule (racemenu-preset-api-is-declared-not-
// registered): a .jslot is NEVER parsed, so a listing is names and nothing
// else. No thumbnails, no metadata, no peeking.
namespace OS::PresetBrowse {

    // The three CharGen folders this feature touches, Data-relative the way
    // ProfileCapture's jslot path is. The game process resolves them through
    // MO2's VFS like every other Data path.
    [[nodiscard]] std::filesystem::path PresetsDir();
    [[nodiscard]] std::filesystem::path ExportedDir();
    [[nodiscard]] std::filesystem::path ExportedMeshesDir();
    [[nodiscard]] std::filesystem::path ExportedTintsDir();

    // Make the three folders SaveExternalCharacter writes into, and say
    // whether all three are there afterwards.
    //
    // ⚠⚠ MEASURED 2026-08-23: THE MESH HALF OF THE EXPORT HAD NEVER LANDED.
    // Nine FR_ jslots and nine FR_ tint textures were on the reference rig
    // and not one FR_ nif, because `Data\Meshes\CharGen\Exported` did not
    // exist at all. skee writes the mesh through NiStream::SavePath, which
    // opens a file and does not build a path to put it in, so the write went
    // nowhere and reported nothing. The root exports look healthy next to it
    // for the dull reason that mods ship their folder already.
    bool EnsureExportDirs();

    // One preset the browser shows: the CharGen name (stem, no extension)
    // exactly as the load native wants it.
    struct Entry {
        std::string stem;

        friend bool operator==(const Entry&, const Entry&) = default;
    };

    // List the preset stems in a_dir, non-recursive: every *.jslot plus
    // every *.slot (RaceMenu's binary fallback), deduplicated
    // case-insensitively so a json/binary pair is one row, sorted
    // case-insensitively. a_skipFrPrefix drops FR_* names: those are FR's
    // own captures, already reachable through their profiles, and listing
    // them twice would offer the same face under two doors.
    [[nodiscard]] std::vector<Entry> ListPresets(const std::filesystem::path& a_dir,
                                                 bool a_skipFrPrefix);

    // The export-name normaliser. The rig measured why this exists: RaceMenu
    // appends rather than replaces when a typed name already ends in .nif,
    // and the user's own export sits at every depth up to
    // `!UBE_Umbrael_August_2026.nif.nif.nif.nif.nif.nif`. Strips whitespace
    // at both ends, then every trailing .nif / .jslot / .slot layer
    // case-insensitively, then replaces filesystem-hostile characters the
    // way the capture's jslot sanitizer does. Empty in, empty out: the
    // caller's button stays disabled rather than inventing a name.
    [[nodiscard]] std::string NormalizeExportName(std::string a_name);

}  // namespace OS::PresetBrowse
