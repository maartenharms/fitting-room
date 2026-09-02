#include "PresetBrowse.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace OS::PresetBrowse {

    namespace {

        [[nodiscard]] std::string LowerCopy(std::string_view a_text) {
            std::string out;
            out.reserve(a_text.size());
            for (const char c : a_text) {
                out += static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        [[nodiscard]] bool EndsWithCi(std::string_view a_text,
                                      std::string_view a_suffix) {
            if (a_text.size() < a_suffix.size()) {
                return false;
            }
            return LowerCopy(a_text.substr(a_text.size() - a_suffix.size())) ==
                   LowerCopy(a_suffix);
        }

    }  // namespace

    std::filesystem::path PresetsDir() {
        return std::filesystem::path("Data/SKSE/Plugins/CharGen/Presets");
    }

    std::filesystem::path ExportedDir() {
        return std::filesystem::path("Data/SKSE/Plugins/CharGen/Exported");
    }

    std::filesystem::path ExportedMeshesDir() {
        return std::filesystem::path("Data/Meshes/CharGen/Exported");
    }

    std::filesystem::path ExportedTintsDir() {
        return std::filesystem::path("Data/Textures/CharGen/Exported");
    }

    bool EnsureExportDirs() {
        std::error_code ec;
        // create_directories on a folder that already exists is false with no
        // error, so the answer comes from is_directory afterwards rather than
        // from the return: "already there" and "just made" are both fine and
        // only "still missing" is the failure this reports.
        for (const auto& dir :
             { ExportedDir(), ExportedMeshesDir(), ExportedTintsDir() }) {
            std::filesystem::create_directories(dir, ec);
        }
        return std::filesystem::is_directory(ExportedDir(), ec) &&
               std::filesystem::is_directory(ExportedMeshesDir(), ec) &&
               std::filesystem::is_directory(ExportedTintsDir(), ec);
    }

    std::vector<Entry> ListPresets(const std::filesystem::path& a_dir,
                                   bool a_skipFrPrefix) {
        std::vector<Entry> out;
        std::error_code    ec;
        // The error_code overloads throughout: a missing folder (RaceMenu
        // absent, or a rig that never exported) is an EMPTY list, never a
        // throw into the draw loop.
        if (!std::filesystem::is_directory(a_dir, ec)) {
            return out;
        }
        std::vector<std::string> seenLower;
        for (const auto& file :
             std::filesystem::directory_iterator(a_dir, ec)) {
            if (ec) {
                break;
            }
            if (!file.is_regular_file(ec)) {
                continue;
            }
            const auto ext = LowerCopy(file.path().extension().string());
            if (ext != ".jslot" && ext != ".slot") {
                continue;
            }
            const auto stem = file.path().stem().string();
            if (stem.empty()) {
                continue;
            }
            if (a_skipFrPrefix && LowerCopy(stem).starts_with("fr_")) {
                continue;
            }
            const auto lower = LowerCopy(stem);
            if (std::find(seenLower.begin(), seenLower.end(), lower) !=
                seenLower.end()) {
                continue;  // the json/binary pair is one preset
            }
            seenLower.push_back(lower);
            out.push_back(Entry{ stem });
        }
        std::sort(out.begin(), out.end(), [](const Entry& a_a, const Entry& a_b) {
            return LowerCopy(a_a.stem) < LowerCopy(a_b.stem);
        });
        return out;
    }

    std::string NormalizeExportName(std::string a_name) {
        const auto first = a_name.find_first_not_of(" \t");
        if (first == std::string::npos) {
            return {};
        }
        const auto last = a_name.find_last_not_of(" \t");
        a_name          = a_name.substr(first, last - first + 1);
        // Every trailing extension layer, not just one: the measured pileup
        // is six deep, and stripping a single layer would still let a paste
        // of that name grow a seventh.
        for (;;) {
            if (EndsWithCi(a_name, ".nif")) {
                a_name.erase(a_name.size() - 4);
            } else if (EndsWithCi(a_name, ".jslot")) {
                a_name.erase(a_name.size() - 6);
            } else if (EndsWithCi(a_name, ".slot")) {
                a_name.erase(a_name.size() - 5);
            } else {
                break;
            }
            while (!a_name.empty() &&
                   (a_name.back() == ' ' || a_name.back() == '\t')) {
                a_name.pop_back();
            }
        }
        // The capture's sanitizer rule: alnum, space, dash, underscore
        // survive, anything else becomes an underscore, trailing spaces go.
        std::string base;
        for (const char c : a_name) {
            const auto uc = static_cast<unsigned char>(c);
            base += (std::isalnum(uc) || c == ' ' || c == '-' || c == '_' ||
                     c == '!' || c == '(' || c == ')')
                        ? c
                        : '_';
        }
        while (!base.empty() && base.back() == ' ') {
            base.pop_back();
        }
        return base;
    }

}  // namespace OS::PresetBrowse
