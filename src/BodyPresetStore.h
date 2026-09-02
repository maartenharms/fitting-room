#pragma once

#include "BodyPreset.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace OS {

    class BodyPresetStore {
    public:
        static BodyPresetStore& GetSingleton();

        explicit BodyPresetStore(std::filesystem::path a_root);

        void Load();
        [[nodiscard]] std::vector<BodyPreset> Snapshot() const;
        [[nodiscard]] std::optional<BodyPreset> Find(std::string_view a_id) const;
        [[nodiscard]] std::size_t RejectedCount() const;

        // Save assigns a stable id when this is a new preset. Existing ids are
        // replaced atomically and a rename also removes the old filename.
        [[nodiscard]] bool Save(BodyPreset& a_preset, std::string& a_error);
        [[nodiscard]] bool Delete(std::string_view a_id, std::string& a_error);
        [[nodiscard]] bool ExportBodySlideXml(const std::filesystem::path& a_path,
                                              std::string& a_error) const;
        [[nodiscard]] bool NameAvailable(std::string_view a_name,
                                         std::string_view a_exceptId = {}) const;
        [[nodiscard]] static bool Validate(const BodyPreset& a_preset,
                                           std::string& a_error);

    private:
        [[nodiscard]] static std::string NewId();
        [[nodiscard]] std::filesystem::path PathFor(const BodyPreset& a_preset) const;

        std::filesystem::path root_;
        mutable std::mutex lock_;
        std::vector<BodyPreset> presets_;
        std::unordered_map<std::string, std::filesystem::path> paths_;
        std::size_t rejected_{ 0 };
    };

}  // namespace OS
