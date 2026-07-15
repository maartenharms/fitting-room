#include "CrashGuard.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>

namespace OS::CrashGuard {

    namespace {
        constexpr const char* kDir     = "Data/SKSE/Plugins/FittingRoom";
        constexpr const char* kPending = "Data/SKSE/Plugins/FittingRoom/pending_preview.txt";
        constexpr const char* kCrashers = "Data/SKSE/Plugins/FittingRoom/crashed_styles.txt";

        std::mutex             g_mutex;
        StyleRefKey            g_previewing;       // guarded by g_mutex
        std::set<std::string>  g_crashers;         // "modName|localID"; immutable after startup
        std::atomic<bool>      g_hasCrashers{ false };  // fast-path: skip lookups when empty
        bool                   g_markerLive{ false };

        std::string KeyLine(const StyleRefKey& a_key) {
            return a_key.modName + "|" + std::to_string(a_key.localFormID);
        }

        void WritePending(const StyleRefKey& a_key) {
            std::error_code ec;
            std::filesystem::create_directories(kDir, ec);
            if (std::ofstream f{ kPending, std::ios::trunc }) {
                f << KeyLine(a_key) << '\n';
            }
        }

        void DeletePending() {
            std::error_code ec;
            std::filesystem::remove(kPending, ec);
        }
    }

    void LoadAtStartup() {
        std::scoped_lock l(g_mutex);
        g_crashers.clear();
        if (std::ifstream f{ kCrashers }) {
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    g_crashers.insert(line);
                }
            }
        }

        // A surviving marker means last session's preview rebuild never
        // returned - that style crashed. Promote it and clear the marker.
        std::string pending;
        if (std::ifstream f{ kPending }) {
            std::getline(f, pending);
            if (!pending.empty() && pending.back() == '\r') {
                pending.pop_back();
            }
        }
        if (!pending.empty()) {
            const bool isNew = g_crashers.insert(pending).second;
            if (isNew) {
                std::error_code ec;
                std::filesystem::create_directories(kDir, ec);
                if (std::ofstream f{ kCrashers, std::ios::app }) {
                    f << pending << '\n';
                }
            }
            spdlog::warn("CrashGuard: '{}' crashed a preview last session - flagged as a "
                         "crasher (now {} total).",
                         pending, g_crashers.size());
        }
        DeletePending();
        g_hasCrashers.store(!g_crashers.empty(), std::memory_order_relaxed);
        spdlog::info("CrashGuard: {} known crasher style(s) loaded.", g_crashers.size());
    }

    void SetPreviewing(const StyleRefKey& a_key) {
        std::scoped_lock l(g_mutex);
        g_previewing = a_key;
    }

    void ClearPreviewing() {
        std::scoped_lock l(g_mutex);
        g_previewing = StyleRefKey{};
    }

    void BeginKick() {
        StyleRefKey key;
        {
            std::scoped_lock l(g_mutex);
            key = g_previewing;
        }
        if (key.Empty()) {
            return;  // not a style preview (editor open, quick-switch, load) - nothing to blame
        }
        WritePending(key);
        g_markerLive = true;
    }

    void EndKick() {
        if (g_markerLive) {
            DeletePending();
            g_markerLive = false;
        }
    }

    bool IsCrasher(const StyleRefKey& a_key) {
        if (!g_hasCrashers.load(std::memory_order_relaxed) || a_key.Empty()) {
            return false;  // fast path: no crashers recorded, no lock/alloc
        }
        std::scoped_lock l(g_mutex);
        return g_crashers.contains(KeyLine(a_key));
    }

    std::size_t Count() {
        std::scoped_lock l(g_mutex);
        return g_crashers.size();
    }

}  // namespace OS::CrashGuard
