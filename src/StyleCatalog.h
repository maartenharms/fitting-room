#pragma once

#include "Outfit.h"

#include <string>
#include <vector>

namespace OS {

    // Why a style may not render on the player. Drives the red-flag reason.
    enum class FitReason : std::uint8_t {
        kFits = 0,   // a race-valid armature with a mesh for the player's sex exists
        kNoRace,     // no armature is valid for the player's race (UBE etc.)
        kNoSex,      // a race-valid armature exists but none has a mesh for the
                     // player's sex (gender-exclusive armor on the wrong gender)
        kCrashed,    // previewing this style crashed the game on a prior launch
                     // (CrashGuard); flagged so the same crash can't recur
    };

    struct StyleItem {
        RE::TESObjectARMO* armo{ nullptr };
        StyleRefKey        key;
        std::string        name;
        std::string        source;      // plugin filename
        std::uint32_t      slotMask{ 0 };
        std::uint32_t      primaryBit{ 0 };  // lowest covered bit: the ONE slot it lists under
        std::uint8_t       armorType{ 0 };   // 0 light, 1 heavy, 2 clothing
        bool               fitsBody{ true };  // false = won't render (see fitReason)
        FitReason          fitReason{ FitReason::kFits };
        bool               isRecent{ false };  // source plugin newly added this launch (OS-26)
        std::string        edid;  // best-effort; empty on runtimes without EDID retention
    };

    class StyleCatalog {
    public:
        static StyleCatalog& GetSingleton();

        // Scan every loaded ARMO. Call at kDataLoaded.
        void Build();

        // Re-evaluate fitsBody against the player's CURRENT race. The race is
        // only meaningful once the save is in (body mods like UBE are custom
        // races; at kDataLoaded the player still has the main-menu default,
        // which is why a Build()-time race filter provably misses them - see
        // research/ube-fit-detection.md). Call at kPostLoadGame/kNewGame.
        void RefreshFit();

        // RefreshFit() only if the race changed since the last evaluation
        // (RaceMenu race swaps mid-save). Cheap when unchanged. Must run on
        // the main thread while the editor is CLOSED - Draw() reads fitsBody
        // unsynchronized.
        void EnsureFitCurrent();

        // The shared render predicate: an armature that fits the player's race
        // AND provides a mesh for the player's sex. Used per-catalog-item and
        // for ARMOs outside the catalog (showcase preset pieces). Returns kFits
        // when the player is unknowable (fail open, never a false red).
        [[nodiscard]] static FitReason EvaluateFit(RE::TESObjectARMO* a_armo);
        [[nodiscard]] static bool      FitsPlayer(RE::TESObjectARMO* a_armo) {
            return EvaluateFit(a_armo) == FitReason::kFits;
        }

        // Human-readable reason for the flag tooltip, e.g. "no Nord UBE
        // armature" or "no female mesh". Never null; empty-ish for kFits.
        [[nodiscard]] const char*        FitRaceName() const;
        [[nodiscard]] static std::string FitReasonText(FitReason a_reason);

        // Items whose PRIMARY slot is the given bit (multi-slot armor lists
        // under its lowest slot only - no duplicates across slots), filtered
        // by a case-insensitive substring of the name OR source plugin (empty
        // = all). a_collectedOnly additionally limits to looks in the player's
        // Collection (owned-at-some-point); a_armorType (0 light, 1 heavy,
        // 2 clothing; -1 = any) narrows to one armor class; a_favoritesOnly
        // limits to starred looks (OS-22). Styles from newly-added plugins are
        // never filtered - they just sort to the top and carry a NEW badge
        // (OS-26). Unfit styles are never hidden either - they render red (see
        // fitReason) so nothing silently vanishes from the catalog.
        [[nodiscard]] std::vector<const StyleItem*> Query(std::uint32_t a_bit,
                                                          std::string_view a_search,
                                                          bool a_collectedOnly,
                                                          int  a_armorType,
                                                          bool a_favoritesOnly) const;

        // Bitmask of primary slots that have at least one item matching the
        // search - drives the slot-list highlight while searching.
        [[nodiscard]] std::uint32_t MatchMask(std::string_view a_search, bool a_collectedOnly,
                                              int a_armorType, bool a_favoritesOnly) const;

        [[nodiscard]] std::size_t Size() const { return items_.size(); }

        // All indexed items - read on the main thread (auto-preset generation
        // runs there, same discipline as Query). Do not hold across a Build().
        [[nodiscard]] const std::vector<StyleItem>& Items() const { return items_; }

    private:
        StyleCatalog() = default;
        std::vector<StyleItem> items_;
        RE::TESRace*           fitRace_{ nullptr };  // race fitsBody was last evaluated against
    };

}  // namespace OS
