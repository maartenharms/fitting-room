#include "BodyPresetStore.h"

#include "BodyPresetJson.h"
#include "BuildChannel.h"

#include <json/json.h>
#include <tinyxml2.h>
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <set>

namespace OS {

    namespace {
        constexpr std::uintmax_t kMaxPresetBytes = 1024u * 1024u;

        // The codec moved to BodyPresetJson so the profile codec can embed a
        // preset payload without a second serializer; the store keeps its
        // filesystem duties and these local names.
        using BodyPresetJson::FromJson;
        using BodyPresetJson::Lower;
        using BodyPresetJson::ToJson;
        using BodyPresetJson::Trimmed;

        [[nodiscard]] bool ReplaceAtomically(const std::filesystem::path& a_temp,
                                             const std::filesystem::path& a_target,
                                             std::string& a_error) {
            if (MoveFileExW(a_temp.c_str(), a_target.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return true;
            }
            a_error = "atomic replace failed (Windows error " +
                      std::to_string(GetLastError()) + ")";
            std::error_code ignored;
            std::filesystem::remove(a_temp, ignored);
            return false;
        }
    }  // namespace

    BodyPresetStore& BodyPresetStore::GetSingleton() {
        static BodyPresetStore singleton(BuildChannel::CustomBodyPresetRoot());
        return singleton;
    }

    BodyPresetStore::BodyPresetStore(std::filesystem::path a_root) : root_(std::move(a_root)) {}

    bool BodyPresetStore::Validate(const BodyPreset& a_preset, std::string& a_error) {
        return BodyPresetJson::Validate(a_preset, a_error);
    }

    std::string BodyPresetStore::NewId() {
        std::random_device rd;
        std::mt19937_64 rng((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
        const auto a = rng();
        const auto b = rng();
        char id[33]{};
        std::snprintf(id, sizeof(id), "%016llx%016llx",
                      static_cast<unsigned long long>(a),
                      static_cast<unsigned long long>(b));
        return id;
    }

    std::filesystem::path BodyPresetStore::PathFor(const BodyPreset& a_preset) const {
        std::string base;
        for (unsigned char c : a_preset.name) {
            base += std::isalnum(c) || c == '-' || c == '_' ? static_cast<char>(c) : '-';
        }
        while (!base.empty() && base.back() == '-') base.pop_back();
        if (base.empty()) base = "Body-Preset";
        if (base.size() > 80) base.resize(80);
        return root_ / (base + "-" + a_preset.id.substr(0, 8) + ".json");
    }

    void BodyPresetStore::Load() {
        std::vector<BodyPreset> loaded;
        std::unordered_map<std::string, std::filesystem::path> paths;
        std::size_t rejected = 0;
        std::error_code ec;
        if (std::filesystem::exists(root_, ec) && !ec) {
            std::vector<std::filesystem::path> files;
            for (std::filesystem::directory_iterator it(root_, ec), end;
                 !ec && it != end; it.increment(ec)) {
                if (it->is_regular_file(ec) && !ec &&
                    Lower(it->path().extension().string()) == ".json") {
                    files.push_back(it->path());
                }
                ec.clear();
            }
            std::ranges::sort(files);
            for (const auto& path : files) {
                const auto size = std::filesystem::file_size(path, ec);
                if (ec || size > kMaxPresetBytes) {
                    ++rejected;
                    ec.clear();
                    continue;
                }
                std::ifstream in(path, std::ios::binary);
                Json::Value root;
                Json::CharReaderBuilder builder;
                std::string parseError;
                BodyPreset preset;
                std::string error;
                if (!in || !Json::parseFromStream(builder, in, &root, &parseError) ||
                    !FromJson(root, preset, error) || paths.contains(preset.id)) {
                    ++rejected;
                    continue;
                }
                paths.emplace(preset.id, path);
                loaded.push_back(std::move(preset));
            }
        }
        std::ranges::sort(loaded, [](const BodyPreset& a_a, const BodyPreset& a_b) {
            return Lower(a_a.name) < Lower(a_b.name);
        });
        std::scoped_lock l(lock_);
        presets_ = std::move(loaded);
        paths_ = std::move(paths);
        rejected_ = rejected;
    }

    std::vector<BodyPreset> BodyPresetStore::Snapshot() const {
        std::scoped_lock l(lock_);
        return presets_;
    }

    std::optional<BodyPreset> BodyPresetStore::Find(std::string_view a_id) const {
        std::scoped_lock l(lock_);
        const auto it = std::ranges::find_if(presets_, [&](const BodyPreset& a_preset) {
            return a_preset.id == a_id;
        });
        if (it == presets_.end()) return std::nullopt;
        return *it;
    }

    std::size_t BodyPresetStore::RejectedCount() const {
        std::scoped_lock l(lock_);
        return rejected_;
    }

    bool BodyPresetStore::NameAvailable(std::string_view a_name,
                                        std::string_view a_exceptId) const {
        const auto wanted = Lower(Trimmed(a_name));
        if (wanted.empty()) return false;
        std::scoped_lock l(lock_);
        return std::ranges::none_of(presets_, [&](const BodyPreset& a_preset) {
            return a_preset.id != a_exceptId && Lower(a_preset.name) == wanted;
        });
    }

    bool BodyPresetStore::Save(BodyPreset& a_preset, std::string& a_error) {
        if (a_preset.id.empty()) a_preset.id = NewId();
        a_preset.name = Trimmed(a_preset.name);
        if (!Validate(a_preset, a_error)) return false;
        if (!NameAvailable(a_preset.name, a_preset.id)) {
            a_error = "a custom preset already uses that name";
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        if (ec) {
            a_error = "cannot create the custom preset directory: " + ec.message();
            return false;
        }
        const auto target = PathFor(a_preset);
        const auto temp = target.wstring() + L".tmp";
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        {
            std::ofstream out(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
            if (!out) {
                a_error = "cannot open the temporary preset file";
                return false;
            }
            out << Json::writeString(builder, ToJson(a_preset));
            out.flush();
            if (!out) {
                a_error = "cannot write the temporary preset file";
                out.close();
                std::filesystem::remove(std::filesystem::path(temp), ec);
                return false;
            }
        }
        std::filesystem::path oldPath;
        {
            std::scoped_lock l(lock_);
            if (const auto it = paths_.find(a_preset.id); it != paths_.end()) oldPath = it->second;
        }
        if (!ReplaceAtomically(std::filesystem::path(temp), target, a_error)) return false;
        if (!oldPath.empty() && oldPath != target) std::filesystem::remove(oldPath, ec);
        Load();
        return true;
    }

    bool BodyPresetStore::Delete(std::string_view a_id, std::string& a_error) {
        std::filesystem::path path;
        {
            std::scoped_lock l(lock_);
            const auto it = paths_.find(std::string(a_id));
            if (it == paths_.end()) {
                a_error = "custom preset was not found";
                return false;
            }
            path = it->second;
        }
        std::error_code ec;
        if (!std::filesystem::remove(path, ec) || ec) {
            a_error = "cannot remove custom preset: " + ec.message();
            return false;
        }
        Load();
        return true;
    }

    bool BodyPresetStore::ExportBodySlideXml(const std::filesystem::path& a_path,
                                             std::string& a_error) const {
        const auto presets = Snapshot();
        tinyxml2::XMLDocument doc;
        doc.InsertEndChild(doc.NewDeclaration());
        auto* root = doc.NewElement("SliderPresets");
        doc.InsertEndChild(root);
        for (const auto& preset : presets) {
            auto* node = doc.NewElement("Preset");
            node->SetAttribute("name", preset.name.c_str());
            node->SetAttribute("set", preset.sourceSet.c_str());
            root->InsertEndChild(node);
            for (const auto& group : preset.groups) {
                auto* child = doc.NewElement("Group");
                child->SetAttribute("name", group.c_str());
                node->InsertEndChild(child);
            }
            for (const auto& slider : preset.sliders) {
                for (const auto endpoint : { std::pair{ "small", slider.smallValue },
                                             std::pair{ "big", slider.bigValue } }) {
                    auto* child = doc.NewElement("SetSlider");
                    child->SetAttribute("name", slider.name.c_str());
                    child->SetAttribute("size", endpoint.first);
                    child->SetAttribute("value", endpoint.second);
                    node->InsertEndChild(child);
                }
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(a_path.parent_path(), ec);
        if (ec || doc.SaveFile(a_path.string().c_str()) != tinyxml2::XML_SUCCESS) {
            a_error = ec ? ec.message() : "tinyxml2 could not write the export";
            return false;
        }
        return true;
    }

}  // namespace OS
