#include "StyleCatalog.h"

#include "Collection.h"
#include "CrashGuard.h"
#include "Favorites.h"
#include "REAugments.h"
#include "RecentMods.h"
#include "Settings.h"
#include "SlotMask.h"
#include "StyleRef.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <set>

namespace OS {

    namespace {
        bool ContainsCI(std::string_view a_hay, std::string_view a_needle) {
            if (a_needle.empty()) {
                return true;
            }
            const auto lower = [](char a_c) {
                return std::tolower(static_cast<unsigned char>(a_c));
            };
            const auto it = std::search(a_hay.begin(), a_hay.end(), a_needle.begin(),
                                        a_needle.end(),
                                        [&](char a, char b) { return lower(a) == lower(b); });
            return it != a_hay.end();
        }
    }

    StyleCatalog& StyleCatalog::GetSingleton() {
        static StyleCatalog instance;
        return instance;
    }

    void StyleCatalog::Build() {
        items_.clear();
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            spdlog::error("StyleCatalog: no TESDataHandler.");
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* skin   = player ? REAug::GetActorSkin(player) : nullptr;

        // Hygiene filter: a style must fit at least one PLAYABLE race. At
        // kDataLoaded the player still wears the main-menu default race, so
        // filtering against player->GetRace() here silently indexes gear a
        // UBE-race (or any custom-race) save can never render - and drops
        // gear only such races CAN wear. Per-character fit is a FLAG
        // (RefreshFit), never an index filter.
        std::vector<RE::TESRace*> playableRaces;
        for (auto* race : dh->GetFormArray<RE::TESRace>()) {
            if (race && race->data.flags.any(RE::RACE_DATA::Flag::kPlayable)) {
                playableRaces.push_back(race);
            }
        }

        // [Debug] sDiagnosePlugin: trace, per matching ARMO, exactly where it
        // enters or falls out of the catalog (the OS-45 "why is mod X missing?"
        // tool). Off (empty) = zero overhead beyond one emptiness check per form.
        const std::string& diag      = Settings::GetSingleton().diagnosePlugin;
        const auto          diagMatch = [&](RE::TESObjectARMO* a_armo, const char* a_name) {
            if (diag.empty() || !a_armo) {
                return false;
            }
            if (a_name && ContainsCI(a_name, diag)) {
                return true;
            }
            auto* f = a_armo->GetFile(0);
            return f && ContainsCI(f->GetFilename(), diag);
        };
        const auto diagFate = [](RE::TESObjectARMO* a_armo, const char* a_name,
                                 const char* a_fate) {
            auto* f = a_armo ? a_armo->GetFile(0) : nullptr;
            spdlog::info("[diag/catalog] {:08X} '{}' [{}] slot=0x{:X} type={} -> {}",
                         a_armo ? a_armo->GetFormID() : 0u, a_name ? a_name : "",
                         f ? f->GetFilename() : "?",
                         a_armo ? static_cast<std::uint32_t>(a_armo->GetSlotMask()) : 0u,
                         a_armo ? static_cast<int>(a_armo->GetArmorType()) : -1, a_fate);
        };

        std::size_t raceRejected = 0;
        for (auto* armo : dh->GetFormArray<RE::TESObjectARMO>()) {
            if (!armo) {
                continue;
            }
            const char* name = armo->GetName();
            const bool  dhit = diagMatch(armo, name);
            if (!armo->GetPlayable()) {
                if (dhit) diagFate(armo, name, "DROP: not playable");
                continue;
            }
            if (!name || !*name) {
                if (dhit) diagFate(armo, name, "DROP: no display name");
                continue;
            }
            if (armo->armorAddons.empty()) {
                if (dhit) diagFate(armo, name, "DROP: no armor addons");
                continue;
            }
            if (armo == skin) {
                if (dhit) diagFate(armo, name, "DROP: is the player skin");
                continue;  // the naked body is not a style
            }
            const auto mask = static_cast<std::uint32_t>(armo->GetSlotMask());
            if (mask == 0 || (mask & ~kNeverTouchMask) == 0) {
                if (dhit) diagFate(armo, name, "DROP: no usable slot (none/shield-only)");
                continue;
            }
            // A style must be able to RENDER on some possible player: at
            // least one addon must fit a playable race (the engine's own
            // check, armor-parent chain included). Filters child gear,
            // creature armor, NPC-locked refits - which no player character
            // could ever wear.
            if (!playableRaces.empty()) {
                bool fits = false;
                for (auto* arma : armo->armorAddons) {
                    if (!arma) {
                        continue;
                    }
                    for (auto* race : playableRaces) {
                        if (arma->IsValidRace(race)) {
                            fits = true;
                            break;
                        }
                    }
                    if (fits) {
                        break;
                    }
                }
                if (!fits) {
                    ++raceRejected;
                    if (dhit) diagFate(armo, name, "DROP: no playable-race armature (IsValidRace)");
                    continue;
                }
            }
            StyleItem item;
            if (!StyleRef::Make(armo, item.key)) {
                if (dhit) diagFate(armo, name, "DROP: StyleRef::Make failed (no defining file)");
                continue;
            }
            item.armo       = armo;
            item.name       = name;
            item.source     = item.key.modName;
            item.slotMask   = mask;
            item.primaryBit = static_cast<std::uint32_t>(std::countr_zero(mask));
            item.armorType  = static_cast<std::uint8_t>(armo->GetArmorType());
            if (const char* ed = armo->GetFormEditorID(); ed && *ed) {
                item.edid = ed;  // best-effort; a corroborating stem for set detection
            }
            if (dhit) diagFate(armo, name, "KEPT (entered catalog, pre-collapse)");
            items_.push_back(std::move(item));
        }

        // Collapse enchanted variants. "Iron Armor of Health" and plain "Iron
        // Armor" share an addon set and look identical, so the browser must show
        // LOOKS, not records. Key on the sorted ARMA pointer set; keep the first
        // (usually the unenchanted base, which sorts earlier by FormID).
        std::ranges::sort(items_, [](const StyleItem& a, const StyleItem& b) {
            return a.armo->GetFormID() < b.armo->GetFormID();
        });
        const auto addonKey = [](const StyleItem& it) {
            std::vector<RE::TESObjectARMA*> addons(it.armo->armorAddons.begin(),
                                                   it.armo->armorAddons.end());
            std::ranges::sort(addons);
            return addons;
        };
        std::vector<StyleItem>                    unique;
        std::set<std::vector<RE::TESObjectARMA*>> seen;
        for (auto& it : items_) {
            if (seen.insert(addonKey(it)).second) {
                unique.push_back(std::move(it));
            } else if (!diag.empty() &&
                       (ContainsCI(it.name, diag) || ContainsCI(it.source, diag))) {
                spdlog::info("[diag/catalog] {:08X} '{}' [{}] -> DROP: variant-collapsed "
                             "(shares an addon set with a kept, lower-FormID style)",
                             it.armo->GetFormID(), it.name, it.source);
            }
        }
        const auto before = items_.size();
        items_ = std::move(unique);
        spdlog::info("StyleCatalog: indexed {} armor styles ({} variants collapsed, {} "
                     "fit no playable race).",
                     items_.size(), before - items_.size(), raceRejected);

        // OS-26: flag styles whose plugin is newly added this launch, then
        // persist the current plugin set as next launch's baseline. First run
        // seeds silently (nothing flagged).
        std::vector<std::string> sources;
        sources.reserve(items_.size());
        for (auto& it : items_) {
            it.isRecent = RecentMods::IsNewPlugin(it.source);
            sources.push_back(it.source);
        }
        RecentMods::CommitSeen(sources);
    }

    namespace {
        int PlayerSexIdx() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* npc    = player ? player->GetActorBase() : nullptr;
            return (npc && npc->GetSex() == RE::SEXES::kFemale) ? RE::SEXES::kFemale
                                                                : RE::SEXES::kMale;
        }
        bool ArmaHasSexModel(RE::TESObjectARMA* a_arma, int a_sexIdx) {
            if (!a_arma) {
                return false;
            }
            const char* path = a_arma->bipedModels[a_sexIdx].GetModel();
            return path && *path;
        }
    }

    FitReason StyleCatalog::EvaluateFit(RE::TESObjectARMO* a_armo) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* race   = player ? player->GetRace() : nullptr;
        if (!a_armo || !race) {
            return FitReason::kFits;  // fail open - never a false red
        }
        const int sexIdx       = PlayerSexIdx();
        bool      anyRaceValid = false;
        for (auto* arma : a_armo->armorAddons) {
            if (!arma || !arma->IsValidRace(race)) {
                continue;
            }
            anyRaceValid = true;
            if (ArmaHasSexModel(arma, sexIdx)) {
                return FitReason::kFits;  // same armature fits race AND sex
            }
        }
        // A race-valid armature with no mesh for the player's sex is
        // gender-exclusive to the other sex - trying it on renders nothing
        // (and previewing the wrong-sex mesh has crashed the skinning pass).
        return anyRaceValid ? FitReason::kNoSex : FitReason::kNoRace;
    }

    std::string StyleCatalog::FitReasonText(FitReason a_reason) {
        switch (a_reason) {
            case FitReason::kNoSex:
                return PlayerSexIdx() == RE::SEXES::kFemale ? "no female mesh" : "no male mesh";
            case FitReason::kCrashed:
                return "crashed the preview last time";
            default:  // kNoRace (and kFits, which never shows a tooltip)
                return "no " + std::string(GetSingleton().FitRaceName()) + " armature";
        }
    }

    void StyleCatalog::RefreshFit() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* race   = player ? player->GetRace() : nullptr;
        fitRace_     = race;
        if (!race) {
            return;
        }
        std::size_t unfit = 0;
        std::size_t noSex = 0;
        for (auto& it : items_) {
            it.fitReason = EvaluateFit(it.armo);
            // A style that crashed the preview last session is flagged
            // regardless of race/sex fit - it must not be previewed again.
            if (CrashGuard::IsCrasher(it.key)) {
                it.fitReason = FitReason::kCrashed;
            }
            it.fitsBody = it.fitReason == FitReason::kFits;
            if (!it.fitsBody) {
                ++unfit;
                if (it.fitReason == FitReason::kNoSex) {
                    ++noSex;
                }
            }
        }
        // The skin identifies the BODY SYSTEM independently of the race -
        // a body mod can ride on custom races (UBE_AllRace) or on a skin
        // override; the pair answers which one this character uses.
        auto*       skin = REAug::GetActorSkin(RE::PlayerCharacter::GetSingleton());
        const char* edid = race->GetFormEditorID();
        spdlog::info("StyleCatalog: fit check vs race '{}' \"{}\" ({:08X}), skin {:08X} "
                     "'{}', sex {}: {} of {} styles flagged may-not-fit ({} of them "
                     "wrong-gender).",
                     edid && *edid ? edid : "?", race->GetName(), race->GetFormID(),
                     skin ? skin->GetFormID() : 0,
                     skin && skin->GetFormEditorID() ? skin->GetFormEditorID() : "?",
                     PlayerSexIdx() == RE::SEXES::kFemale ? "F" : "M", unfit, items_.size(),
                     noSex);
    }

    void StyleCatalog::EnsureFitCurrent() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* race   = player ? player->GetRace() : nullptr;
        if (race && race != fitRace_) {
            RefreshFit();
        }
    }

    const char* StyleCatalog::FitRaceName() const {
        if (!fitRace_) {
            return "your race";
        }
        if (const char* full = fitRace_->GetName(); full && *full) {
            return full;
        }
        const char* edid = fitRace_->GetFormEditorID();
        return edid && *edid ? edid : "your race";
    }

    namespace {
        bool Matches(const StyleItem& a_it, std::string_view a_search, bool a_collectedOnly,
                     int a_armorType, bool a_favoritesOnly, const Collection& a_collection) {
            if (a_collectedOnly && !a_collection.Knows(a_it.armo->GetFormID())) {
                return false;
            }
            if (a_favoritesOnly && !Favorites::IsFavorite(a_it.key)) {
                return false;
            }
            // Unfit styles are NEVER hidden (detection has false positives, and
            // silently missing items read as a broken catalog) - they render
            // red instead. See research/ube-fit-detection.md.
            if (a_armorType >= 0 && a_it.armorType != static_cast<std::uint8_t>(a_armorType)) {
                return false;
            }
            return ContainsCI(a_it.name, a_search) || ContainsCI(a_it.source, a_search);
        }
    }

    std::vector<const StyleItem*> StyleCatalog::Query(std::uint32_t a_bit,
                                                      std::string_view a_search,
                                                      bool a_collectedOnly, int a_armorType,
                                                      bool a_favoritesOnly) const {
        std::vector<const StyleItem*> out;
        auto&                         collection = Collection::GetSingleton();
        for (const auto& it : items_) {
            if (it.primaryBit == a_bit &&
                Matches(it, a_search, a_collectedOnly, a_armorType, a_favoritesOnly, collection)) {
                out.push_back(&it);
            }
        }
        // Newly-added-plugin styles float to the top (OS-26), then name-ascending.
        std::ranges::sort(out, [](const StyleItem* a, const StyleItem* b) {
            if (a->isRecent != b->isRecent) {
                return a->isRecent;
            }
            return a->name < b->name;
        });
        return out;
    }

    std::uint32_t StyleCatalog::MatchMask(std::string_view a_search, bool a_collectedOnly,
                                          int a_armorType, bool a_favoritesOnly) const {
        std::uint32_t mask       = 0;
        auto&         collection = Collection::GetSingleton();
        for (const auto& it : items_) {
            if (!((mask >> it.primaryBit) & 1u) &&
                Matches(it, a_search, a_collectedOnly, a_armorType, a_favoritesOnly, collection)) {
                mask |= 1u << it.primaryBit;
            }
        }
        return mask;
    }

}  // namespace OS
