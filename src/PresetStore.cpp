#include "PresetStore.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

namespace OS {

    namespace {
        constexpr const char* kPresetsDir = "Data/SKSE/Plugins/FittingRoom/Presets";
        constexpr const char* kExportsDir = "Data/SKSE/Plugins/FittingRoom/Exports";
        // Sanity cap: a hand-written preset is a few KB; anything bigger is
        // not one of ours and must not stall the load.
        constexpr std::uintmax_t kMaxPresetBytes = 256 * 1024;

        std::string Lower(std::string a_s) {
            std::ranges::transform(a_s, a_s.begin(), [](unsigned char a_c) {
                return static_cast<char>(std::tolower(a_c));
            });
            return a_s;
        }

        bool CILess(const std::string& a_a, const std::string& a_b) {
            return Lower(a_a) < Lower(a_b);
        }
    }

    PresetStore& PresetStore::GetSingleton() {
        static PresetStore instance;
        return instance;
    }

    void PresetStore::Load() {
        std::vector<JsonCodec::Preset> loaded;
        std::size_t                    skipped = 0;

        auto* dh = RE::TESDataHandler::GetSingleton();
        std::error_code ec;
        if (std::filesystem::exists(kPresetsDir, ec)) {
            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::directory_iterator(kPresetsDir, ec)) {
                if (entry.is_regular_file(ec) &&
                    Lower(entry.path().extension().string()) == ".json") {
                    files.push_back(entry.path());
                }
            }
            std::ranges::sort(files);  // deterministic scan order

            for (const auto& path : files) {
                const auto file = path.filename().string();

                if (const auto size = std::filesystem::file_size(path, ec);
                    !ec && size > kMaxPresetBytes) {
                    spdlog::warn("PresetStore: SKIP '{}': {} bytes (cap {}).", file, size,
                                 kMaxPresetBytes);
                    ++skipped;
                    continue;
                }

                std::ifstream in(path);
                Json::Value   root;
                Json::CharReaderBuilder rb;
                std::string             errs;
                if (!in || !Json::parseFromStream(rb, in, &root, &errs)) {
                    spdlog::warn("PresetStore: SKIP '{}': not valid JSON ({}).", file,
                                 errs.empty() ? "open failed" : errs);
                    ++skipped;
                    continue;
                }

                JsonCodec::Preset preset;
                std::string       why;
                if (!JsonCodec::ParsePreset(root, preset, why)) {
                    spdlog::warn("PresetStore: SKIP '{}': {}.", file, why);
                    ++skipped;
                    continue;
                }

                // `requires` is the author's hard gate. Slot styles pointing
                // at absent plugins do NOT skip - those entries are simply
                // inert, so optional-integration pieces degrade gracefully.
                const std::string* missing = nullptr;
                for (const auto& req : preset.requires_) {
                    if (!dh || !dh->LookupModByName(req)) {
                        missing = &req;
                        break;
                    }
                }
                if (missing) {
                    spdlog::info("PresetStore: SKIP '{}': requires '{}' (not in the "
                                 "load order).", file, *missing);
                    ++skipped;
                    continue;
                }

                preset.file = file;
                spdlog::info("PresetStore: '{}' by {} ({}).", preset.name,
                             preset.author.empty() ? "(unknown)" : preset.author, file);
                loaded.push_back(std::move(preset));
            }
        }

        std::ranges::sort(loaded, [](const auto& a, const auto& b) {
            if (a.author != b.author) {
                return CILess(a.author, b.author);
            }
            return CILess(a.name, b.name);
        });

        {
            std::scoped_lock l(lock_);
            presets_ = std::move(loaded);
        }
        spdlog::info("PresetStore: {} showcase preset(s) loaded, {} skipped.",
                     Count(), skipped);
    }

    std::vector<JsonCodec::Preset> PresetStore::Snapshot() const {
        std::scoped_lock l(lock_);
        return presets_;
    }

    std::size_t PresetStore::Count() const {
        std::scoped_lock l(lock_);
        return presets_.size();
    }

    void PresetStore::RequestRescan() {
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] { GetSingleton().Load(); });
        }
    }

    std::string PresetStore::ExportOutfit(const Outfit& a_outfit) {
        // Prefill `requires` with every non-vanilla plugin the styles touch;
        // the author trims optional ones by hand (PRESETS.md).
        static const std::set<std::string> kVanilla = {
            "skyrim.esm", "update.esm", "dawnguard.esm", "hearthfires.esm",
            "dragonborn.esm",
        };
        std::vector<std::string> requires_;
        a_outfit.ForEachStyle([&](std::uint32_t, const StyleRefKey& a_key) {
            if (a_key.modName.empty() || kVanilla.contains(Lower(a_key.modName))) {
                return;
            }
            if (std::ranges::find(requires_, a_key.modName) == requires_.end()) {
                requires_.push_back(a_key.modName);
            }
        });

        const auto root = JsonCodec::PresetToJson(a_outfit, "", "", requires_);

        std::string base;
        for (const char c : a_outfit.name) {
            const auto uc = static_cast<unsigned char>(c);
            base += (std::isalnum(uc) || c == ' ' || c == '-' || c == '_') ? c : '_';
        }
        while (!base.empty() && base.back() == ' ') {
            base.pop_back();
        }
        if (base.empty()) {
            base = "outfit";
        }

        std::error_code ec;
        std::filesystem::create_directories(kExportsDir, ec);
        const auto path = std::string(kExportsDir) + "/" + base + ".json";
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            spdlog::error("PresetStore: cannot write '{}'.", path);
            return {};
        }
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "  ";
        out << Json::writeString(wb, root);
        spdlog::info("PresetStore: exported '{}' -> {}.", a_outfit.name, path);
        return path;
    }

}  // namespace OS
