#pragma once

#include "Outfit.h"  // StyleRefKey

#include <cctype>
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

        // A key for something that is NOT a form: OBody presets are bare names
        // with no plugin and no form ID, so there is nothing to build a
        // StyleRefKey out of.
        //
        // ⚠ THE "@" PREFIX IS WHAT KEEPS THE TWO KINDS APART. A form key's
        // first field is a plugin filename, which always carries an extension
        // and can never begin with "@", so a namespaced line cannot collide
        // with one however a mod is named. The separator stays "|" so the file
        // is still one flat list and LoadLines needs no cases.
        static std::string NamedKeyLine(std::string_view a_kind, std::string_view a_name) {
            return "@" + std::string{ a_kind } + "|" + std::string{ a_name };
        }

        [[nodiscard]] bool Contains(const StyleRefKey& a_key) const {
            return !a_key.Empty() && ContainsLine(KeyLine(a_key));
        }
        [[nodiscard]] bool ContainsLine(std::string_view a_line) const {
            return !a_line.empty() && keys_.contains(std::string{ a_line });
        }

        // Flip the star; returns the NEW state (true = now favorited).
        bool Toggle(const StyleRefKey& a_key) {
            return a_key.Empty() ? false : ToggleLine(KeyLine(a_key));
        }
        bool ToggleLine(std::string a_line) {
            if (a_line.empty()) {
                return false;
            }
            if (const auto it = keys_.find(a_line); it != keys_.end()) {
                keys_.erase(it);
                return false;
            }
            keys_.insert(std::move(a_line));
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

        // The same two, for things with no form behind them. Build the line
        // with FavoriteSet::NamedKeyLine so the namespace prefix is applied in
        // exactly one place.
        [[nodiscard]] bool IsFavoriteLine(std::string_view a_line);
        bool               ToggleLine(std::string a_line);

        // The one body-preset key builder. ⚠ Keep every caller going through
        // this rather than writing the kind string inline: the kind is part of
        // the on-disk key, so a second spelling of it would silently orphan
        // every star already saved.
        [[nodiscard]] inline std::string BodyKey(std::string_view a_preset) {
            return FavoriteSet::NamedKeyLine("body", a_preset);
        }

        // The one dye key builder, on exactly the terms BodyKey states: the
        // kind is part of the on-disk line, so a second spelling of "dye"
        // silently orphans every star already saved.
        //
        // ⚠ A DYE HAS NO FORM, which is why it takes this route rather than
        // StyleRefKey. Its id is `pack:name` and is already the palette's
        // stable identity across load orders, so it needs nothing built for it:
        // the namespaced line and the file that holds the others were both
        // already here.
        [[nodiscard]] inline std::string DyeKey(std::string_view a_id) {
            return FavoriteSet::NamedKeyLine("dye", a_id);
        }

        // The one skin-pack key builder (OS-214, user 2026-08-18: "we also
        // want to be able to favorite cards"). A pack's id is its folder name
        // under textures\FittingRoom\skins, which is what the SKIN record
        // carries too, so a star follows the pack across saves and rigs that
        // have it. Same terms as BodyKey: the kind is part of the on-disk line.
        [[nodiscard]] inline std::string SkinKey(std::string_view a_packId) {
            return FavoriteSet::NamedKeyLine("skin", a_packId);
        }

        // The one overlay-art key builder, for the overlay and makeup pickers'
        // cards. The identity is the texture's override path (the same string
        // the layer stores and the thumbnail cache keys on), lower-cased with
        // backslashes so the two spellings authors ship are one star; the
        // same texture offered by both pickers carries one star, which is the
        // right answer for one file.
        [[nodiscard]] inline std::string OverlayKey(std::string_view a_path) {
            std::string folded;
            folded.reserve(a_path.size());
            for (const char c : a_path) {
                folded.push_back(c == '/' ? '\\'
                                          : static_cast<char>(std::tolower(
                                                static_cast<unsigned char>(c))));
            }
            return FavoriteSet::NamedKeyLine("overlay", folded);
        }

        [[nodiscard]] std::size_t Count();

    }  // namespace Favorites

}  // namespace OS
