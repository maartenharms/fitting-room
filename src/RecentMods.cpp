#include "RecentMods.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>

namespace OS::RecentMods {

    namespace {
        constexpr const char* kDir  = "Data/SKSE/Plugins/FittingRoom";
        constexpr const char* kFile = "Data/SKSE/Plugins/FittingRoom/known_plugins.txt";

        std::mutex            g_mutex;
        std::set<std::string> g_known;             // plugins seen last launch (guarded)
        bool                  g_firstRun{ true };  // no baseline file -> flag nothing
        std::size_t           g_newCount{ 0 };
    }

    void LoadAtStartup() {
        std::scoped_lock l(g_mutex);
        g_known.clear();
        g_newCount = 0;
        std::ifstream f{ kFile };
        g_firstRun = !f.good();  // absent file = first run for this feature
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    g_known.insert(line);
                }
            }
        }
        spdlog::info("RecentMods: {} plugin(s) known from last launch{}.", g_known.size(),
                     g_firstRun ? " (first run - nothing flagged new)" : "");
    }

    bool IsNewPlugin(std::string_view a_plugin) {
        if (a_plugin.empty()) {
            return false;
        }
        std::scoped_lock l(g_mutex);
        if (g_firstRun) {
            return false;  // no baseline: never flag on the first run
        }
        return !g_known.contains(std::string(a_plugin));
    }

    void CommitSeen(const std::vector<std::string>& a_plugins) {
        std::scoped_lock l(g_mutex);
        std::set<std::string> current;
        for (const auto& p : a_plugins) {
            if (!p.empty()) {
                current.insert(p);
            }
        }
        g_newCount = 0;
        if (!g_firstRun) {
            for (const auto& p : current) {
                if (!g_known.contains(p)) {
                    ++g_newCount;
                }
            }
        }
        // Persist THIS launch's set as the new baseline - current plugins only,
        // so a removed-then-readded mod correctly flags new again later.
        std::error_code ec;
        std::filesystem::create_directories(kDir, ec);
        if (std::ofstream out{ kFile, std::ios::trunc }) {
            for (const auto& p : current) {
                out << p << '\n';
            }
        }
        const bool wasFirst = g_firstRun;
        g_known             = std::move(current);
        g_firstRun          = false;
        spdlog::info("RecentMods: {} newly-added plugin(s) this launch; baseline now {} plugin(s){}.",
                     g_newCount, g_known.size(), wasFirst ? " (seeded)" : "");
    }

    std::size_t NewCount() {
        std::scoped_lock l(g_mutex);
        return g_newCount;
    }

}  // namespace OS::RecentMods
