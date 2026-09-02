#include "BodyMorphPresets.h"

#include "BodyMorphCatalogParse.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace OS::BodyMorphPresets {

    namespace {

        namespace parse = BodyMorphCatalogParse;

        // Fitting Room's own shapes.
        const std::filesystem::path kHome{ "Data/SKSE/Plugins/FittingRoom/BodyShapes" };

        // ⚠ A SUBFOLDER RATHER THAN SliderPresets ITSELF. SAM's folder browser
        // walks into subfolders, BodySlide scans recursively, and one named
        // folder keeps an export identifiable and removable. Dropping files
        // loose beside a user's own presets is how an export becomes permanent
        // by accident.
        const std::filesystem::path kExport{
            "Data/CalienteTools/BodySlide/SliderPresets/Fitting Room"
        };

        std::atomic<std::shared_ptr<const Library>> g_library{
            std::make_shared<const Library>()
        };

        constexpr std::uintmax_t kMaxFileBytes = 4ull * 1024ull * 1024ull;

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

        [[nodiscard]] bool WriteFile(const std::filesystem::path& a_path,
                                     const std::string&           a_text) {
            std::error_code ec;
            std::filesystem::create_directories(a_path.parent_path(), ec);
            std::ofstream out(a_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                spdlog::error("BodyShapes: could not open '{}' for writing.",
                              a_path.string());
                return false;
            }
            out.write(a_text.data(), static_cast<std::streamsize>(a_text.size()));
            return out.good();
        }

        // The Preset name inside a file, which is what SAM and BodySlide show.
        // The stem is only how we find it again.
        [[nodiscard]] std::string FirstPresetName(std::string_view a_text) {
            parse::detail::Tag tag;
            std::size_t        at = 0;
            while (parse::detail::NextTag(a_text, at, tag)) {
                at = tag.end;
                if (tag.name == "Preset") {
                    return parse::detail::Attr(tag.attrs, "name");
                }
            }
            return {};
        }

        [[nodiscard]] std::string FirstPresetSet(std::string_view a_text) {
            parse::detail::Tag tag;
            std::size_t        at = 0;
            while (parse::detail::NextTag(a_text, at, tag)) {
                at = tag.end;
                if (tag.name == "Preset") {
                    return parse::detail::Attr(tag.attrs, "set");
                }
            }
            return {};
        }

    }  // namespace

    void Reload() {
        auto            library = std::make_shared<Library>();
        std::error_code ec;
        std::size_t     skipped = 0;

        if (std::filesystem::exists(kHome, ec) && !ec) {
            for (const auto& entry : std::filesystem::directory_iterator(
                     kHome, std::filesystem::directory_options::skip_permission_denied,
                     ec)) {
                if (ec) {
                    break;
                }
                if (!entry.is_regular_file(ec) || ec) {
                    continue;
                }
                auto ext = entry.path().extension().string();
                std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (ext != ".xml") {
                    continue;
                }
                std::string text;
                if (!ReadFile(entry.path(), text)) {
                    ++skipped;
                    continue;
                }
                Shape shape;
                shape.id   = entry.path().stem().string();
                shape.name = FirstPresetName(text);
                shape.set  = FirstPresetSet(text);
                if (shape.name.empty()) {
                    ++skipped;
                    continue;
                }
                shape.values = parse::ParsePresetSliders(text, shape.name);
                std::error_code exists;
                shape.exported = std::filesystem::exists(
                    kExport / (shape.id + ".xml"), exists);
                library->shapes.push_back(std::move(shape));
            }
        }

        std::ranges::sort(library->shapes, [](const Shape& a, const Shape& b) {
            return a.name < b.name;
        });

        std::ostringstream diag;
        diag << library->shapes.size() << " shape(s)";
        if (skipped) {
            diag << ", " << skipped << " unreadable";
        }
        library->diagnostic = diag.str();
        spdlog::info("BodyShapes: {}.", library->diagnostic);
        g_library.store(std::move(library), std::memory_order_release);
    }

    std::shared_ptr<const Library> Get() {
        return g_library.load(std::memory_order_acquire);
    }

    bool Save(const std::string& a_name, const std::string& a_set,
              const std::vector<std::pair<std::string, float>>& a_values) {
        const auto stem = parse::SafeFileStem(a_name);
        if (stem.empty()) {
            spdlog::warn("BodyShapes: refusing to save a shape with no usable name.");
            return false;
        }
        if (a_values.empty()) {
            spdlog::warn("BodyShapes: refusing to save '{}' with nothing in it.", a_name);
            return false;
        }
        const auto xml = parse::BuildPresetXml(a_name, a_set, a_values);
        if (!WriteFile(kHome / (stem + ".xml"), xml)) {
            return false;
        }
        // ⚠ AN EXISTING EXPORT IS REWRITTEN TOO, not left stale. Saving over a
        // shape the user had already sent to SAM and leaving SAM's copy on the
        // old values is the kind of divergence nobody would think to check for.
        std::error_code ec;
        if (std::filesystem::exists(kExport / (stem + ".xml"), ec) && !ec) {
            WriteFile(kExport / (stem + ".xml"), xml);
        }
        spdlog::info("BodyShapes: saved '{}' ({} slider(s), set '{}').", a_name,
                     a_values.size(), a_set.empty() ? "(unknown)" : a_set);
        Reload();
        return true;
    }

    bool Delete(const std::string& a_id) {
        std::error_code ec;
        const bool      gone = std::filesystem::remove(kHome / (a_id + ".xml"), ec);
        std::filesystem::remove(kExport / (a_id + ".xml"), ec);
        if (gone) {
            spdlog::info("BodyShapes: deleted '{}'.", a_id);
        }
        Reload();
        return gone;
    }

    bool SetExported(const std::string& a_id, bool a_exported) {
        std::error_code ec;
        if (!a_exported) {
            const bool gone = std::filesystem::remove(kExport / (a_id + ".xml"), ec);
            spdlog::info("BodyShapes: '{}' withdrawn from BodySlide's presets ({}).",
                         a_id, gone ? "removed" : "was not there");
            Reload();
            return true;
        }
        std::string text;
        if (!ReadFile(kHome / (a_id + ".xml"), text)) {
            spdlog::error("BodyShapes: cannot export '{}', its own file is unreadable.",
                          a_id);
            return false;
        }
        if (!WriteFile(kExport / (a_id + ".xml"), text)) {
            return false;
        }
        spdlog::info("BodyShapes: exported '{}' to '{}'; SAM, BodySlide and OBody can "
                     "read it there.",
                     a_id, kExport.string());
        Reload();
        return true;
    }

    const Shape* Find(const Library& a_library, const std::string& a_id) {
        const auto at = std::ranges::find_if(
            a_library.shapes, [&](const Shape& a_s) { return a_s.id == a_id; });
        return at == a_library.shapes.end() ? nullptr : &*at;
    }

}  // namespace OS::BodyMorphPresets
