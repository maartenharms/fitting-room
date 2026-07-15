#pragma once

#include "Outfit.h"  // StyleRefKey

#include <set>
#include <string>
#include <string_view>

namespace OS {

    // The set of styles the player has starred (OS-22). Keyed by the same
    // load-order-independent "modName|localID" line CrashGuard uses, so a
    // favorite survives load-order shuffles. Pure logic - no engine, no
    // filesystem - so it is unit-tested like ToggleHideSlot. The runtime
    // Favorites module (below) wraps one of these behind a mutex + flat file.
    class FavoriteSet {
    public:
        static std::string KeyLine(const StyleRefKey& a_key) {
            return a_key.modName + "|" + std::to_string(a_key.localFormID);
        }

        [[nodiscard]] bool Contains(const StyleRefKey& a_key) const {
            return !a_key.Empty() && keys_.contains(KeyLine(a_key));
        }

        // Flip the star; returns the NEW state (true = now favorited).
        bool Toggle(const StyleRefKey& a_key) {
            if (a_key.Empty()) {
                return false;
            }
            auto line = KeyLine(a_key);
            if (const auto it = keys_.find(line); it != keys_.end()) {
                keys_.erase(it);
                return false;
            }
            keys_.insert(std::move(line));
            return true;
        }

        void Add(const StyleRefKey& a_key) {
            if (!a_key.Empty()) {
                keys_.insert(KeyLine(a_key));
            }
        }
        void Remove(const StyleRefKey& a_key) {
            if (!a_key.Empty()) {
                keys_.erase(KeyLine(a_key));
            }
        }

        [[nodiscard]] std::size_t Size() const { return keys_.size(); }

        // Newline-joined key lines (sorted, since keys_ is a std::set) - the
        // on-disk form. Round-trips through LoadLines.
        [[nodiscard]] std::string Serialize() const {
            std::string s;
            for (const auto& k : keys_) {
                s += k;
                s += '\n';
            }
            return s;
        }

        // Replace the set from a newline-delimited blob (tolerant of CRLF and
        // blank lines), e.g. the contents of favorites.txt.
        void LoadLines(std::string_view a_text) {
            keys_.clear();
            std::size_t i = 0;
            while (i <= a_text.size()) {
                const std::size_t nl  = a_text.find('\n', i);
                const std::size_t end = nl == std::string_view::npos ? a_text.size() : nl;
                std::string_view  line = a_text.substr(i, end - i);
                if (!line.empty() && line.back() == '\r') {
                    line.remove_suffix(1);
                }
                if (!line.empty()) {
                    keys_.emplace(line);
                }
                if (nl == std::string_view::npos) {
                    break;
                }
                i = nl + 1;
            }
        }

    private:
        std::set<std::string> keys_;
    };

    // Runtime favorites: a FavoriteSet mirrored to a GLOBAL flat file
    // (Data/SKSE/Plugins/FittingRoom/favorites.txt), same doctrine as
    // CrashGuard's crashed_styles.txt and the global outfits.json - a starred
    // look is a preference, not per-character state. Toggled from the editor
    // (Present thread), read by the catalog filter; guarded by a mutex.
    namespace Favorites {

        void LoadAtStartup();  // kDataLoaded - before the editor can open

        [[nodiscard]] bool IsFavorite(const StyleRefKey& a_key);

        // Flip + persist immediately; returns the new state (true = favorited).
        bool Toggle(const StyleRefKey& a_key);

        [[nodiscard]] std::size_t Count();

    }  // namespace Favorites

}  // namespace OS
