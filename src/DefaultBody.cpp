#include "DefaultBody.h"

#include "BuildChannel.h"
#include "NpcIdentity.h"

#include <json/json.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace OS::DefaultBody {

    namespace {
        const auto kFile = BuildChannel::DataPath("default-bodies.json");

        // A corrupt or hand-mangled count should not be able to make us
        // allocate without bound, the same guard kMaxNpcAssignments applies to
        // the co-save side.
        constexpr std::size_t kMaxEntries = 4096;

        std::mutex                                  g_mutex;
        std::unordered_map<std::string, Entry>      g_map;  // guarded by g_mutex

        // "Plugin.esp|0008F0". One string so the map key is trivially
        // hashable and the file stays readable by hand.
        [[nodiscard]] std::string KeyText(const NpcKey& a_key) {
            char buf[16]{};
            std::snprintf(buf, sizeof(buf), "%06X", a_key.localFormID);
            return a_key.modName + "|" + buf;
        }

        [[nodiscard]] std::optional<std::string> KeyOf(RE::Actor* a_actor) {
            if (!a_actor) {
                return std::nullopt;
            }
            const auto key = NpcKeyFor(a_actor->GetActorBase());
            if (!key) {
                return std::nullopt;  // runtime base: no identity that survives a reload
            }
            return KeyText(*key);
        }

    }  // namespace

    std::size_t ImportLegacyFile() {
        std::scoped_lock l(g_mutex);
        // ⚠ DOES NOT CLEAR; the caller already reset the map for this load.
        std::ifstream f{ kFile };
        if (!f) {
            return 0;  // the ordinary case now: no legacy file to take over
        }
        Json::Value  root;
        Json::String errs;
        Json::CharReaderBuilder b;
        if (!Json::parseFromStream(b, f, &root, &errs) || !root.isObject()) {
            // ⚠ REFUSED WHOLE, NOT PARTIALLY. A half-read map would be written
            // straight back out by the next Set and would take the unread
            // half with it.
            spdlog::error("DefaultBody: '{}' is not readable JSON ({}); leaving it alone.",
                          kFile.string(), errs);
            return 0;
        }
        for (const auto& key : root.getMemberNames()) {
            if (g_map.size() >= kMaxEntries) {
                spdlog::warn("DefaultBody: more than {} entries; the rest are ignored.",
                             kMaxEntries);
                break;
            }
            const auto& node = root[key];
            if (!node.isObject()) {
                continue;
            }
            Entry e;
            e.installed = node.get("installed", "").asString();
            e.customId  = node.get("custom", "").asString();
            if (!e.Empty()) {
                g_map.emplace(key, std::move(e));
            }
        }
        spdlog::info("DefaultBody: imported {} character(s) from the retired '{}'. They "
                     "belong to THIS save from now on.",
                     g_map.size(), kFile.string());
        return g_map.size();
    }

    std::vector<DefaultBodyRecord> Snapshot() {
        std::scoped_lock l(g_mutex);
        std::vector<DefaultBodyRecord> rows;
        rows.reserve(g_map.size());
        for (const auto& [key, e] : g_map) {
            rows.push_back(DefaultBodyRecord{ key, e.installed, e.customId });
        }
        // Sorted for the reason DefaultLook::Snapshot gives: g_map is unordered
        // and unstable bytes would make every save differ for no reason.
        std::sort(rows.begin(), rows.end(),
                  [](const DefaultBodyRecord& a, const DefaultBodyRecord& b) {
                      return a.modAndId < b.modAndId;
                  });
        return rows;
    }

    void Restore(const std::vector<DefaultBodyRecord>& a_rows) {
        std::scoped_lock l(g_mutex);
        g_map.clear();
        for (const auto& row : a_rows) {
            Entry e;
            e.installed = row.installed;
            e.customId  = row.customId;
            if (!e.Empty() && !row.modAndId.empty()) {
                g_map.emplace(row.modAndId, std::move(e));
            }
        }
        spdlog::info("DefaultBody: this save holds {} character default(s).", g_map.size());
    }

    void ClearAll() {
        std::scoped_lock l(g_mutex);
        g_map.clear();
    }

    Entry For(RE::Actor* a_actor) {
        const auto key = KeyOf(a_actor);
        if (!key) {
            return {};
        }
        std::scoped_lock l(g_mutex);
        const auto it = g_map.find(*key);
        return it == g_map.end() ? Entry{} : it->second;
    }

    bool Has(RE::Actor* a_actor) { return !For(a_actor).Empty(); }

    void Set(RE::Actor* a_actor, std::string_view a_installed, std::string_view a_customId) {
        const auto key = KeyOf(a_actor);
        if (!key) {
            return;
        }
        Entry e;
        // Custom wins, matching RestoreStagedBody's precedence.
        if (!a_customId.empty()) {
            e.customId = std::string{ a_customId };
        } else {
            e.installed = std::string{ a_installed };
        }
        if (e.Empty()) {
            Clear(a_actor);
            return;
        }
        {
            std::scoped_lock l(g_mutex);
            g_map[*key] = e;
            // ⚠ NO WRITE HERE ANY MORE. The co-save owns this map through
            // 'DFBD'; see DefaultLook.h for why it stopped being a file.
        }
        spdlog::info("DefaultBody: '{}' default body set to '{}'.", *key,
                     e.customId.empty() ? e.installed : e.customId);
    }

    void Clear(RE::Actor* a_actor) {
        const auto key = KeyOf(a_actor);
        if (!key) {
            return;
        }
        std::scoped_lock l(g_mutex);
        if (g_map.erase(*key)) {
            // ⚠ NO WRITE HERE ANY MORE. The co-save owns this map through
            // 'DFBD'; see DefaultLook.h for why it stopped being a file.
            spdlog::info("DefaultBody: '{}' default body cleared.", *key);
        }
    }

}  // namespace OS::DefaultBody
