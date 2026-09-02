#include "PreviewScopes.h"

#include "BuildChannel.h"

#include <json/json.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>

namespace OS::PreviewScopes {

    namespace {
        std::mutex g_lock;
        ScopeSet   g_set;

        // The same fold PreviewGrid::FoldPath applies, kept here rather than
        // reached for: this module is pure of the engine side on purpose, and
        // an author's file is folded ONCE at load so the match loop does no
        // per-geometry case work.
        [[nodiscard]] std::string Fold(std::string_view a_in) {
            std::string out;
            out.reserve(a_in.size());
            for (const char c : a_in) {
                out.push_back(c == '\\'
                                  ? '/'
                                  : static_cast<char>(std::tolower(
                                        static_cast<unsigned char>(c))));
            }
            return out;
        }

        void ReadStrings(const Json::Value& a_arr, std::vector<std::string>& a_out) {
            if (!a_arr.isArray()) {
                return;
            }
            for (const auto& v : a_arr) {
                if (v.isString()) {
                    auto folded = Fold(v.asString());
                    if (!folded.empty()) {
                        a_out.push_back(std::move(folded));
                    }
                }
            }
        }
    }  // namespace

    ScopeSet Snapshot() {
        const std::lock_guard lock{ g_lock };
        return g_set;
    }

    std::size_t Load() {
        const auto dir = BuildChannel::DataPath("PreviewFilters");

        ScopeSet        built;
        std::size_t     files = 0;
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || ec) {
            // No directory is the ordinary state of an install that ships no
            // fixups. Not a warning: an empty scope set is exactly the
            // behaviour of every build before this feature.
            const std::lock_guard lock{ g_lock };
            g_set = std::move(built);
            return 0;
        }

        // Sorted, so two files covering one path resolve the same way on every
        // machine. directory_iterator's order is filesystem-defined, and a
        // rule set that depends on it is a bug that only shows on somebody
        // else's disk.
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_regular_file(ec) || ec) {
                continue;
            }
            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (ext == ".json") {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());

        for (const auto& path : paths) {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                spdlog::warn("PreviewScopes: could not open '{}'; skipped.",
                             path.string());
                continue;
            }
            Json::Value             root;
            Json::CharReaderBuilder rb;
            std::string             errs;
            if (!Json::parseFromStream(rb, in, &root, &errs) || !root.isObject()) {
                // ⚠ ONE BAD FILE COSTS ITS OWN SCOPES AND NOTHING ELSE. These
                // are hand-authored fixups, the dye packs' shape rather than
                // the machine-written shared-unlock file's, so per-file
                // tolerance is right: a third party shipping a broken fixup
                // must not disarm the ones that work.
                spdlog::warn("PreviewScopes: '{}' did not parse and was skipped: {}",
                             path.string(), errs);
                continue;
            }
            ++files;
            const auto& arr = root["scopes"];
            if (!arr.isArray()) {
                continue;
            }
            for (const auto& s : arr) {
                if (!s.isObject()) {
                    continue;
                }
                Scope scope;
                if (s["path"].isString()) {
                    scope.path = Fold(s["path"].asString());
                }
                if (scope.path.empty()) {
                    // ⚠ REFUSED, NOT TREATED AS "EVERYTHING". A scope with no
                    // path is a typo, and the generous reading of it applies
                    // one mod's hide list to the entire catalog, which shows up
                    // as blank cards rather than as a bad rule.
                    spdlog::warn("PreviewScopes: a scope in '{}' has no \"path\" and "
                                 "was dropped. A scope with no path would cover every "
                                 "model in the game.",
                                 path.string());
                    continue;
                }
                ReadStrings(s["hide"], scope.hide);
                ReadStrings(s["show"], scope.show);
                if (scope.hide.empty() && scope.show.empty()) {
                    continue;
                }
                built.scopes.push_back(std::move(scope));
            }
        }

        spdlog::info("PreviewScopes: {} scope(s) from {} file(s) in {}.",
                     built.scopes.size(), files, dir.string());
        const auto count = built.scopes.size();
        {
            const std::lock_guard lock{ g_lock };
            g_set = std::move(built);
        }
        return count;
    }

}  // namespace OS::PreviewScopes
