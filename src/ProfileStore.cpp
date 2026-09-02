#include "ProfileStore.h"

#include "BodyPresetJson.h"  // Lower, Trimmed
#include "BuildChannel.h"

#include <json/json.h>
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

namespace OS {

    namespace {
        constexpr std::uintmax_t kMaxProfileBytes = 1024u * 1024u;

        using BodyPresetJson::Lower;
        using BodyPresetJson::Trimmed;

        // The preset sanitizer, verbatim from the export path: alnum, space,
        // dash and underscore survive, everything else becomes an underscore.
        [[nodiscard]] std::string SanitizedBase(const std::string& a_name) {
            std::string base;
            for (const char c : a_name) {
                const auto uc = static_cast<unsigned char>(c);
                base += (std::isalnum(uc) || c == ' ' || c == '-' || c == '_')
                            ? c
                            : '_';
            }
            while (!base.empty() && base.back() == ' ') {
                base.pop_back();
            }
            if (base.empty()) {
                base = "profile";
            }
            if (base.size() > 80) {
                base.resize(80);
            }
            return base;
        }

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

    ProfileStore& ProfileStore::GetSingleton() {
        static ProfileStore singleton(BuildChannel::DataPath("Profiles"));
        return singleton;
    }

    ProfileStore::ProfileStore(std::filesystem::path a_root)
        : root_(std::move(a_root)) {}

    void ProfileStore::Load() {
        std::vector<Entry> loaded;
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
            std::set<std::string> names;
            for (const auto& path : files) {
                const auto size = std::filesystem::file_size(path, ec);
                if (ec || size > kMaxProfileBytes) {
                    ++rejected;
                    ec.clear();
                    continue;
                }
                std::ifstream in(path, std::ios::binary);
                Json::Value root;
                Json::CharReaderBuilder builder;
                std::string parseError;
                ProfileCodec::ProfileParse parse;
                std::string error;
                if (!in || !Json::parseFromStream(builder, in, &root, &parseError) ||
                    !ProfileCodec::ParseProfile(root, parse, error) ||
                    !names.insert(Lower(parse.profile.name)).second) {
                    ++rejected;
                    continue;
                }
                loaded.push_back(Entry{ std::move(parse.profile), path,
                                        parse.rewritable,
                                        std::move(parse.dropped) });
            }
        }
        std::ranges::sort(loaded, [](const Entry& a_a, const Entry& a_b) {
            return Lower(a_a.profile.name) < Lower(a_b.profile.name);
        });
        std::scoped_lock l(lock_);
        entries_ = std::move(loaded);
        rejected_ = rejected;
    }

    std::vector<ProfileStore::Entry> ProfileStore::Snapshot() const {
        std::scoped_lock l(lock_);
        return entries_;
    }

    std::optional<ProfileStore::Entry> ProfileStore::Find(
        std::string_view a_name) const {
        const auto wanted = Lower(Trimmed(a_name));
        std::scoped_lock l(lock_);
        const auto it = std::ranges::find_if(entries_, [&](const Entry& a_entry) {
            return Lower(a_entry.profile.name) == wanted;
        });
        if (it == entries_.end()) return std::nullopt;
        return *it;
    }

    std::size_t ProfileStore::RejectedCount() const {
        std::scoped_lock l(lock_);
        return rejected_;
    }

    bool ProfileStore::NameAvailable(std::string_view a_name,
                                     std::string_view a_exceptName) const {
        const auto wanted = Lower(Trimmed(a_name));
        if (wanted.empty()) return false;
        const auto except = Lower(Trimmed(a_exceptName));
        std::scoped_lock l(lock_);
        return std::ranges::none_of(entries_, [&](const Entry& a_entry) {
            const auto name = Lower(a_entry.profile.name);
            return name != except && name == wanted;
        });
    }

    std::filesystem::path ProfileStore::NewPathFor(const std::string& a_name) const {
        const auto base = SanitizedBase(a_name);
        auto taken = [&](const std::filesystem::path& a_path) {
            std::error_code ec;
            if (std::filesystem::exists(a_path, ec) && !ec) return true;
            std::scoped_lock l(lock_);
            return std::ranges::any_of(entries_, [&](const Entry& a_entry) {
                return a_entry.file == a_path;
            });
        };
        auto candidate = root_ / (base + ".json");
        // The uniquifier loop the spec asks for: the export path's silent
        // trunc overwrite is a recorded gap, not a precedent.
        for (int suffix = 2; taken(candidate); ++suffix) {
            candidate = root_ / (base + "-" + std::to_string(suffix) + ".json");
        }
        return candidate;
    }

    bool ProfileStore::Save(const ProfileCodec::Profile& a_profile,
                            std::string& a_error) {
        auto profile = a_profile;
        profile.name = Trimmed(profile.name);
        if (profile.name.empty()) {
            a_error = "a profile needs a name";
            return false;
        }

        std::filesystem::path target;
        if (const auto existing = Find(profile.name)) {
            if (!existing->rewritable) {
                // Every known block in that file failed to parse, so what
                // this build holds is not the file's content. Writing would
                // flatten a broken-but-newer file into an empty one.
                a_error = "refusing to rewrite '" +
                          existing->file.filename().string() +
                          "': its blocks failed to parse";
                return false;
            }
            target = existing->file;
        } else {
            target = NewPathFor(profile.name);
        }

        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        if (ec) {
            a_error = "cannot create the profile directory: " + ec.message();
            return false;
        }
        const auto temp = target.wstring() + L".tmp";
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        {
            std::ofstream out(std::filesystem::path(temp),
                              std::ios::binary | std::ios::trunc);
            if (!out) {
                a_error = "cannot open the temporary profile file";
                return false;
            }
            out << Json::writeString(builder, ProfileCodec::ProfileToJson(profile));
            out.flush();
            if (!out) {
                a_error = "cannot write the temporary profile file";
                out.close();
                std::filesystem::remove(std::filesystem::path(temp), ec);
                return false;
            }
        }
        if (!ReplaceAtomically(std::filesystem::path(temp), target, a_error)) {
            return false;
        }
        Load();
        return true;
    }

    bool ProfileStore::Rename(std::string_view a_name, std::string_view a_newName,
                              std::string& a_error) {
        const auto newName = Trimmed(a_newName);
        if (newName.empty()) {
            a_error = "a profile needs a name";
            return false;
        }
        const auto existing = Find(a_name);
        if (!existing) {
            a_error = "no profile named '" + std::string(a_name) + "'";
            return false;
        }
        if (!existing->rewritable) {
            a_error = "refusing to rewrite '" +
                      existing->file.filename().string() +
                      "': its blocks failed to parse";
            return false;
        }
        if (!NameAvailable(newName, existing->profile.name)) {
            a_error = "a profile already uses that name";
            return false;
        }
        if (Lower(newName) == Lower(existing->profile.name)) {
            // A case-only rename keeps the file; Save replaces it in place.
            auto renamed = existing->profile;
            renamed.name = newName;
            return Save(renamed, a_error);
        }
        auto renamed = existing->profile;
        renamed.name = newName;
        if (!Save(renamed, a_error)) return false;
        std::error_code ec;
        std::filesystem::remove(existing->file, ec);
        Load();
        return true;
    }

    bool ProfileStore::Delete(std::string_view a_name, std::string& a_error,
                              std::string* a_capturedJslot) {
        if (a_capturedJslot) a_capturedJslot->clear();
        const auto existing = Find(a_name);
        if (!existing) {
            a_error = "no profile named '" + std::string(a_name) + "'";
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::remove(existing->file, ec) || ec) {
            a_error = "cannot delete '" + existing->file.filename().string() +
                      "'" + (ec ? " (" + ec.message() + ")" : "");
            return false;
        }
        if (a_capturedJslot && existing->profile.face &&
            existing->profile.face->source == ProfileCodec::FaceSource::kCaptured) {
            *a_capturedJslot = existing->profile.face->jslot;
        }
        Load();
        return true;
    }

}  // namespace OS
