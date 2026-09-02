#include "BodyMorphCatalog.h"

#include "BodyMorphTri.h"  // the built meshes' own morph names, the real filter

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

namespace OS::BodyMorphCatalog {

    namespace {

        namespace parse = BodyMorphCatalogParse;

        std::atomic<std::shared_ptr<const Snapshot>> g_snapshot{
            std::make_shared<const Snapshot>()
        };
        std::atomic<std::uint32_t> g_active{ 0 };
        std::atomic<std::uint64_t> g_generation{ 0 };

        // A category file is a few KB and a slider set can be a megabyte of
        // slider data. Anything past this is not a BodySlide file.
        constexpr std::uintmax_t kMaxFileBytes = 32ull * 1024ull * 1024ull;

        // ⚠ A CAP ON THE SLIDER-SET WALK, AND IT IS LOGGED WHEN IT BITES. The
        // reference load order carries several thousand .osp files, almost all
        // of them outfit conversions, and we are looking for exactly one. The
        // direct filename guess below usually finds it without walking at all;
        // this is the fallback's budget, and going over it means the page shows
        // an unfiltered list rather than freezing a background thread on a
        // pathological install.
        constexpr std::size_t kMaxProjectFilesRead = 4000;

        [[nodiscard]] bool ReadFile(const std::filesystem::path& a_path,
                                    std::string&                 a_out) {
            std::error_code ec;
            const auto      size = std::filesystem::file_size(a_path, ec);
            if (ec || size == 0 || size > kMaxFileBytes) {
                return false;
            }
            std::ifstream in(a_path, std::ios::binary);
            if (!in) {
                return false;
            }
            a_out.assign(std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>());
            return !a_out.empty();
        }

        [[nodiscard]] std::string Lower(std::string_view a_in) {
            std::string s(a_in);
            std::ranges::transform(s, s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        }

        // Every directory under the root whose name matches, case-insensitively.
        // ⚠ RECURSIVE AND NOT A FIXED DEPTH. The three layouts measured on the
        // reference load order put SliderCategories at two different depths and
        // spell CalienteTools and BodySlide with different casing; MO2 merges
        // them all into one tree and Windows does not care about the casing, but
        // the DEPTH is real and a fixed path misses whichever layout it was not
        // written for.
        [[nodiscard]] std::vector<std::filesystem::path> FilesUnder(
            const std::filesystem::path& a_root, std::string_view a_dirName,
            std::string_view a_extension) {
            std::vector<std::filesystem::path> out;
            std::error_code                    ec;
            if (!std::filesystem::exists(a_root, ec) || ec) {
                return out;
            }
            const auto wanted = Lower(a_dirName);
            for (auto it = std::filesystem::recursive_directory_iterator(
                     a_root, std::filesystem::directory_options::skip_permission_denied,
                     ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) {
                    break;
                }
                if (!it->is_regular_file(ec) || ec) {
                    continue;
                }
                const auto& path = it->path();
                if (Lower(path.extension().string()) != a_extension) {
                    continue;
                }
                const auto parent = path.parent_path().filename().string();
                if (Lower(parent) == wanted) {
                    out.push_back(path);
                }
            }
            // ⚠ SORTED BY FILE NAME, WHICH IS THE BODY AUTHOR'S OWN ORDERING.
            // UBE ships 0NecocoSliders.xml and 1ube.xml with numeric prefixes
            // for exactly this reason: BodySlide lists them in that order, and
            // Merge gives the first file the claim on a shared slider name. So
            // sorting here is what puts the body's own headings above a
            // neighbour's.
            std::ranges::sort(out, [](const auto& a, const auto& b) {
                return Lower(a.filename().string()) < Lower(b.filename().string());
            });
            return out;
        }

        // Which slider set the named BodySlide preset was built against.
        [[nodiscard]] std::string ResolveSet(const std::filesystem::path& a_root,
                                             const std::string&           a_presetName,
                                             std::size_t&                 a_filesRead) {
            if (a_presetName.empty()) {
                return {};
            }
            for (const auto& path : FilesUnder(a_root, "SliderPresets", ".xml")) {
                std::string text;
                ++a_filesRead;
                if (!ReadFile(path, text)) {
                    continue;
                }
                if (auto set = parse::ParsePresetSet(text, a_presetName); !set.empty()) {
                    return set;
                }
            }
            return {};
        }

        // The morph names that slider set carries.
        [[nodiscard]] std::set<std::string> ResolveNames(
            const std::filesystem::path& a_root, const std::string& a_setName,
            std::size_t& a_filesRead, bool& a_hitCap) {
            std::set<std::string> out;
            if (a_setName.empty()) {
                return out;
            }
            const auto projects = FilesUnder(a_root, "SliderSets", ".osp");

            // ⚠ THE FILENAME GUESS FIRST, AND IT IS NOT A SHORTCUT WORTH
            // SKIPPING. BodySlide names a project file after the set inside it
            // almost every time, so this turns a walk over thousands of outfit
            // conversions into one file read on a normal install.
            const auto wanted = Lower(a_setName) + ".osp";
            for (const auto& path : projects) {
                if (Lower(path.filename().string()) != wanted) {
                    continue;
                }
                std::string text;
                ++a_filesRead;
                if (ReadFile(path, text)) {
                    out = parse::ParseSliderSetNames(text, a_setName);
                    if (!out.empty()) {
                        return out;
                    }
                }
            }

            std::size_t read = 0;
            for (const auto& path : projects) {
                if (read >= kMaxProjectFilesRead) {
                    a_hitCap = true;
                    break;
                }
                std::string text;
                ++read;
                ++a_filesRead;
                if (!ReadFile(path, text)) {
                    continue;
                }
                out = parse::ParseSliderSetNames(text, a_setName);
                if (!out.empty()) {
                    return out;
                }
            }
            return out;
        }

        // The morph names the character's own built meshes carry.
        //
        // ⚠⚠ THIS IS THE FILTER NOW, AND THE SLIDER SET IS THE FALLBACK. A set
        // says what it COULD build; a tri says what can move THIS character,
        // which is the question the page is actually asking. BodyMorphTri.h
        // carries the field round that separated them.
        //
        // ⚠ A MESH WITH NO TRI IS NOT AN ERROR. Heads have none, and a body
        // built without the Build Morphs checkbox has none either. It
        // contributes nothing and the others still filter; only ALL of them
        // coming back empty falls through to the set.
        [[nodiscard]] std::set<std::string> BuiltNames(
            const std::filesystem::path& a_meshRoot,
            const std::vector<std::string>& a_wornMeshes, std::size_t& a_filesRead) {
            std::set<std::string> out;
            for (const auto& mesh : a_wornMeshes) {
                const auto rel = BodyMorphTri::TriPathFor(mesh);
                if (rel.empty()) {
                    continue;
                }
                std::string text;
                std::error_code ec;
                const auto      path = a_meshRoot / rel;
                if (!std::filesystem::exists(path, ec) || ec) {
                    continue;
                }
                ++a_filesRead;
                if (!ReadFile(path, text)) {
                    continue;
                }
                for (auto& name : BodyMorphTri::ParseNames(text)) {
                    out.insert(std::move(name));
                }
            }
            return out;
        }

    }  // namespace

    Snapshot ScanRoot(const std::filesystem::path&    a_calienteRoot,
                      const std::string&              a_presetName,
                      const std::filesystem::path&    a_meshRoot,
                      const std::vector<std::string>& a_wornMeshes) {
        Snapshot out;
        out.sourcePreset = a_presetName;

        std::vector<std::vector<parse::Category>> perFile;
        for (const auto& path : FilesUnder(a_calienteRoot, "SliderCategories", ".xml")) {
            std::string text;
            if (!ReadFile(path, text)) {
                continue;
            }
            ++out.categoryFilesRead;
            auto cats = parse::ParseCategories(text);
            if (!cats.empty()) {
                perFile.push_back(std::move(cats));
            }
        }
        auto merged            = parse::Merge(perFile);
        out.slidersBeforeFilter = parse::CountSliders(merged);
        // Taken BEFORE the filter, for the reason on Snapshot::allNames: the
        // page reads with this and draws with the filtered list.
        out.allNames.reserve(out.slidersBeforeFilter);
        for (const auto& cat : merged) {
            for (const auto& slider : cat.sliders) {
                out.allNames.push_back(slider.name);
            }
        }

        std::size_t filesRead = 0;
        bool        hitCap    = false;

        // ⚠ THE MESHES FIRST, AND THE SET IS NOT CONSULTED WHEN THEY ANSWER.
        // The tri union is the better answer and it is also far cheaper: the
        // set route read 694 support files on the reference load order to fail,
        // where this reads one file per worn mesh.
        auto available  = BuiltNames(a_meshRoot, a_wornMeshes, out.triFilesRead);
        out.builtNames  = available.size();
        out.fromMeshes  = !available.empty();

        if (!out.fromMeshes) {
            // The old route, kept whole: a character whose meshes carry no tri
            // at all can still be filtered by what their slider set declares,
            // and that is strictly better than showing every body's vocabulary.
            out.namedSet    = ResolveSet(a_calienteRoot, a_presetName, filesRead);
            out.resolvedSet = out.namedSet;
            available = ResolveNames(a_calienteRoot, out.resolvedSet, filesRead, hitCap);
            if (available.empty()) {
                // ⚠ THE SET NAME IS CLEARED WHEN ITS SLIDERS COULD NOT BE READ,
                // so Filtered() cannot answer true for a filter that did not
                // happen. namedSet keeps the name for the log; the two failures
                // read identically without it.
                out.resolvedSet.clear();
            }
        }
        out.availableNames = available.size();
        out.categories     = parse::KeepAvailable(merged, available);

        std::ostringstream diag;
        diag << out.categoryFilesRead << " category file(s), "
             << out.categories.size() << " group(s), "
             << parse::CountSliders(out.categories) << " of "
             << out.slidersBeforeFilter << " slider(s)";
        // ⚠ THE REASON, NOT ONE CATCH-ALL, which is the rule RaceMenuMorphApi's
        // status enum already follows in this same feature. "Could not resolve
        // a slider set" was printed for two different failures - no preset
        // found, and a preset naming a set nobody has installed - and a whole
        // session went into telling them apart by hand.
        if (out.fromMeshes) {
            diag << "; filtered against the character's BUILT MESHES ("
                 << out.builtNames << " morph names from " << out.triFilesRead
                 << " tri file(s) across " << a_wornMeshes.size() << " mesh path(s))";
        } else if (!out.resolvedSet.empty()) {
            diag << "; no morph data on the worn meshes, so filtered against slider set '"
                 << out.resolvedSet << "' (" << out.availableNames << " names) via preset '"
                 << a_presetName << "'";
        } else if (a_wornMeshes.empty()) {
            diag << "; UNFILTERED, the subject's worn meshes are not known here";
        } else if (a_presetName.empty()) {
            diag << "; UNFILTERED, no tri beside any of the " << a_wornMeshes.size()
                 << " worn mesh(es) and no body preset to fall back to";
        } else if (!out.namedSet.empty()) {
            // The 2026-08-16 case, named exactly: the preset resolved, the set
            // it named is not installed.
            diag << "; UNFILTERED, no tri beside any of the " << a_wornMeshes.size()
                 << " worn mesh(es), and preset '" << a_presetName << "' names slider set '"
                 << out.namedSet << "' which is not installed";
        } else {
            diag << "; UNFILTERED, no tri beside any of the " << a_wornMeshes.size()
                 << " worn mesh(es), and no slider preset named '" << a_presetName
                 << "' was found";
        }
        if (hitCap) {
            diag << "; project walk stopped at " << kMaxProjectFilesRead << " file(s)";
        }
        diag << "; " << filesRead << " support file(s) read";
        out.diagnostic = diag.str();
        return out;
    }

    void RequestScan(std::string a_presetName, std::vector<std::string> a_wornMeshes) {
        const auto requested = g_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        g_active.fetch_add(1, std::memory_order_acq_rel);
        std::thread([requested, preset = std::move(a_presetName),
                     worn = std::move(a_wornMeshes)]() mutable {
            auto next = ScanRoot("Data/CalienteTools", preset, "Data/meshes", worn);
            next.generation = requested;
            spdlog::info("BodyMorphCatalog: {}", next.diagnostic);
            // Switching subject mid-scan requests a different preset. Only the
            // newest request may publish, or a late scan for the character you
            // just left replaces the one you are looking at.
            if (g_generation.load(std::memory_order_acquire) == requested) {
                g_snapshot.store(std::make_shared<const Snapshot>(std::move(next)),
                                 std::memory_order_release);
            }
            g_active.fetch_sub(1, std::memory_order_acq_rel);
        }).detach();
    }

    std::shared_ptr<const Snapshot> Get() {
        return g_snapshot.load(std::memory_order_acquire);
    }

    bool Scanning() { return g_active.load(std::memory_order_acquire) != 0; }

}  // namespace OS::BodyMorphCatalog
