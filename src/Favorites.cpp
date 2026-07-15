#include "Favorites.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace OS::Favorites {

    namespace {
        constexpr const char* kDir  = "Data/SKSE/Plugins/FittingRoom";
        constexpr const char* kFile = "Data/SKSE/Plugins/FittingRoom/favorites.txt";

        std::mutex        g_mutex;
        FavoriteSet       g_set;                    // guarded by g_mutex
        std::atomic<bool> g_any{ false };           // fast path: skip the lock when nothing is starred

        // Rewrite the whole file from the current set (called under g_mutex).
        // The set is tiny (looks the player deliberately starred), so a full
        // rewrite on each toggle is cheaper than an append-with-tombstones log.
        void WriteLocked() {
            std::error_code ec;
            std::filesystem::create_directories(kDir, ec);
            if (std::ofstream f{ kFile, std::ios::trunc }) {
                f << g_set.Serialize();
            }
        }
    }

    void LoadAtStartup() {
        std::scoped_lock l(g_mutex);
        std::string blob;
        if (std::ifstream f{ kFile }) {
            std::ostringstream ss;
            ss << f.rdbuf();
            blob = ss.str();
        }
        g_set.LoadLines(blob);
        g_any.store(g_set.Size() > 0, std::memory_order_relaxed);
        spdlog::info("Favorites: {} favorited style(s) loaded.", g_set.Size());
    }

    bool IsFavorite(const StyleRefKey& a_key) {
        if (!g_any.load(std::memory_order_relaxed) || a_key.Empty()) {
            return false;  // fast path: nothing starred, no lock
        }
        std::scoped_lock l(g_mutex);
        return g_set.Contains(a_key);
    }

    bool Toggle(const StyleRefKey& a_key) {
        std::scoped_lock l(g_mutex);
        const bool now = g_set.Toggle(a_key);
        WriteLocked();
        g_any.store(g_set.Size() > 0, std::memory_order_relaxed);
        return now;
    }

    std::size_t Count() {
        std::scoped_lock l(g_mutex);
        return g_set.Size();
    }

}  // namespace OS::Favorites
