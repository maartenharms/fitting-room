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
#include <string>
#include <utility>

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

        // WEAP and AMMO both inherit TESModelTextureSwap, but only through their
        // concrete type - hence the As<> dispatch. nullptr = not a weapon form.
        RE::TESModelTextureSwap* WeaponSwapOf(RE::TESBoundObject* a_form) {
            if (auto* weap = a_form ? a_form->As<RE::TESObjectWEAP>() : nullptr) {
                return weap;
            }
            if (auto* ammo = a_form ? a_form->As<RE::TESAmmo>() : nullptr) {
                return ammo;
            }
            return nullptr;
        }

        // The look identity of a weapon style: the NIF plus the alternate-texture
        // swap over it. A shared NIF under a different texture set is a different
        // look (the same blade retextured), so each MODS entry contributes both
        // what is applied (the set, by FormID) and where (name3D/index3D), in
        // stored order. The NIF path is case-folded - it is hand-authored across
        // plugins. \x1f is SetDetector's separator; \x1e nests above it here to
        // keep entries apart from the fields within one.
        std::string WeaponLookKey(RE::TESModelTextureSwap* a_swap) {
            if (!a_swap) {
                return {};
            }
            const char* path = a_swap->GetModel();
            std::string key  = path ? path : "";
            std::ranges::transform(key, key.begin(), [](char a_c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(a_c)));
            });
            if (!a_swap->alternateTextures) {
                return key;  // no swap: the bare NIF is the whole look
            }
            for (std::uint32_t i = 0; i < a_swap->numAlternateTextures; ++i) {
                const auto& alt = a_swap->alternateTextures[i];
                key += '\x1e';
                key += std::to_string(alt.textureSet ? alt.textureSet->GetFormID() : 0u);
                key += '\x1f';
                key += std::to_string(alt.index3D);
                key += '\x1f';
                key += alt.name3D.c_str() ? alt.name3D.c_str() : "";
            }
            return key;
        }
    }

    bool IsBoltAmmo(RE::TESAmmo* a_ammo) {
        // See the header for why this is not TESAmmo::IsBolt() - that helper is
        // an AE out-of-bounds read. Polarity is checked against the real enum
        // (AMMO_DATA::Flag::kNonBolt = 1 << 2, RE/A/AMMO_DATA.h): the flag marks
        // an ARROW, so a bolt is the flag's ABSENCE.
        return a_ammo &&
               a_ammo->GetRuntimeData().data.flags.none(RE::AMMO_DATA::Flag::kNonBolt);
    }

    std::optional<WeaponClass> ClassOfWeaponForm(RE::TESForm* a_form) {
        if (!a_form) {
            return std::nullopt;
        }
        // As<> rather than a formType switch: it is the same check the engine's
        // own RTTI does, and it yields the typed pointer the class needs anyway.
        if (auto* weap = a_form->As<RE::TESObjectWEAP>()) {
            // animType 0 (hand-to-hand) and anything unmapped come back nullopt
            // from ClassFromAnimType - a WEAP is not automatically styleable.
            return ClassFromAnimType(static_cast<std::uint8_t>(weap->GetWeaponType()));
        }
        if (auto* ammo = a_form->As<RE::TESAmmo>()) {
            return ClassForAmmo(IsBoltAmmo(ammo));
        }
        return std::nullopt;  // torch (LIGH), shield (ARMO), anything else
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
            if (mask == 0 || (mask & ~kNeverStyleMask) == 0) {
                if (dhit) diagFate(armo, name, "DROP: no styleable slot");
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
            item.form       = armo;
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

        // The weapon dimension (WEAP + AMMO). Same catalog, same StyleItem,
        // discriminated by weaponClass - see StyleCatalog.h. A weapon has no
        // armature, so the playable-race walk above does not apply to it.
        const auto diagFormFate = [&](RE::TESBoundObject* a_form, const char* a_name,
                                      const char* a_fate) {
            if (diag.empty() || !a_form) {
                return;
            }
            auto* f = a_form->GetFile(0);
            if (!(a_name && ContainsCI(a_name, diag)) && !(f && ContainsCI(f->GetFilename(), diag))) {
                return;
            }
            spdlog::info("[diag/catalog] {:08X} '{}' [{}] -> {}", a_form->GetFormID(),
                         a_name ? a_name : "", f ? f->GetFilename() : "?", a_fate);
        };

        // Shared tail for both weapon form types: everything past the
        // type-specific playable/class checks is identical.
        const auto addWeaponStyle = [&](RE::TESBoundObject* a_form, const char* a_name,
                                        WeaponClass a_class) {
            if (!a_name || !*a_name) {
                diagFormFate(a_form, a_name, "DROP: no display name");
                return;
            }
            auto*       swap  = WeaponSwapOf(a_form);
            const char* model = swap ? swap->GetModel() : nullptr;
            if (!model || !*model) {
                diagFormFate(a_form, a_name, "DROP: no model path");
                return;
            }
            StyleItem item;
            if (!StyleRef::Make(a_form, item.key)) {
                diagFormFate(a_form, a_name, "DROP: StyleRef::Make failed (no defining file)");
                return;
            }
            item.form        = a_form;
            item.name        = a_name;
            item.source      = item.key.modName;
            item.weaponClass = a_class;
            if (const char* ed = a_form->GetFormEditorID(); ed && *ed) {
                item.edid = ed;
            }
            diagFormFate(a_form, a_name, "KEPT (entered catalog, pre-collapse)");
            items_.push_back(std::move(item));
        };

        for (auto* weap : dh->GetFormArray<RE::TESObjectWEAP>()) {
            if (!weap) {
                continue;
            }
            const char* name = weap->GetName();
            // GetPlayable() is the engine's own virtual, so it reads kNonPlayable
            // correctly on both runtimes. Mirrors the ARMO pass above.
            if (!weap->GetPlayable()) {
                diagFormFate(weap, name, "DROP: not playable");
                continue;
            }
            const auto cls =
                ClassFromAnimType(static_cast<std::uint8_t>(weap->GetWeaponType()));
            if (!cls) {
                diagFormFate(weap, name, "DROP: no styleable weapon class (hand-to-hand?)");
                continue;
            }
            addWeaponStyle(weap, name, *cls);
        }

        for (auto* ammo : dh->GetFormArray<RE::TESAmmo>()) {
            if (!ammo) {
                continue;
            }
            const char* name = ammo->GetName();
            if (!ammo->GetPlayable()) {
                diagFormFate(ammo, name, "DROP: not playable");
                continue;
            }
            addWeaponStyle(ammo, name, ClassForAmmo(IsBoltAmmo(ammo)));
        }

        // Collapse variants that are the same LOOK. "Iron Armor of Health" and
        // plain "Iron Armor" share an addon set and look identical, so the
        // browser must show LOOKS, not records. Key on the sorted ARMA pointer
        // set; keep the first (usually the unenchanted base, which sorts
        // earlier by FormID).
        //
        // Weapons have no addon set, so they key on (class, look) instead - the
        // look being NIF + texture swap, see WeaponLookKey. NOT the name: that
        // would keep every enchanted variant as its own row, which is the
        // duplication this exists to remove, so two same-look weapons collapse
        // even when named differently (you are picking a look, not a weapon).
        // The class scopes the key because it is the dimension the user browses
        // in: one mesh shipped as both a Dagger and a Sword is two reachable
        // looks, and a flat key would strand the higher-FormID one in a browser
        // that filters by class.
        //
        // FormID-ascending sort makes "keep the first" mean "keep the lowest
        // FormID" for both dimensions in one pass.
        std::ranges::sort(items_, [](const StyleItem& a, const StyleItem& b) {
            return a.form->GetFormID() < b.form->GetFormID();
        });
        const auto addonKey = [](const StyleItem& it) {
            std::vector<RE::TESObjectARMA*> addons(it.Armo()->armorAddons.begin(),
                                                   it.Armo()->armorAddons.end());
            std::ranges::sort(addons);
            return addons;
        };
        std::vector<StyleItem>                        unique;
        std::set<std::vector<RE::TESObjectARMA*>>     seen;
        std::set<std::pair<WeaponClass, std::string>> seenWeapon;  // (class, look)
        std::size_t                                   armorCollapsed  = 0;
        std::size_t                                   weaponCollapsed = 0;
        for (auto& it : items_) {
            const bool fresh =
                it.IsWeapon()
                    ? seenWeapon.emplace(*it.weaponClass, WeaponLookKey(WeaponSwapOf(it.form)))
                          .second
                    : seen.insert(addonKey(it)).second;
            if (fresh) {
                unique.push_back(std::move(it));
                continue;
            }
            if (it.IsWeapon()) {
                ++weaponCollapsed;
            } else {
                ++armorCollapsed;
            }
            if (!diag.empty() && (ContainsCI(it.name, diag) || ContainsCI(it.source, diag))) {
                spdlog::info("[diag/catalog] {:08X} '{}' [{}] -> DROP: variant-collapsed "
                             "(same {} as a kept, lower-FormID style)",
                             it.form->GetFormID(), it.name, it.source,
                             it.IsWeapon() ? "class + look (model + texture swap)" : "addon set");
            }
        }
        items_ = std::move(unique);
        std::size_t armorKept  = 0;
        std::size_t weaponKept = 0;
        std::size_t ammoKept   = 0;
        for (const auto& it : items_) {
            if (!it.IsWeapon()) {
                ++armorKept;
            } else if (*it.weaponClass == WeaponClass::Arrows ||
                       *it.weaponClass == WeaponClass::Bolts) {
                ++ammoKept;
            } else {
                ++weaponKept;
            }
        }
        // Two collapse counts, not one: they answer different questions (a
        // shared addon set vs. a shared look).
        spdlog::info("StyleCatalog: indexed {} armor, {} weapon, {} ammo styles ({} armor "
                     "variants collapsed, {} weapon/ammo variants collapsed, {} fit no "
                     "playable race).",
                     armorKept, weaponKept, ammoKept, armorCollapsed, weaponCollapsed,
                     raceRejected);

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
            return StyleCatalog::SexIdxOf(player ? player->GetActorBase() : nullptr);
        }
        bool ArmaHasSexModel(RE::TESObjectARMA* a_arma, int a_sexIdx) {
            if (!a_arma) {
                return false;
            }
            const char* path = a_arma->bipedModels[a_sexIdx].GetModel();
            return path && *path;
        }
    }

    int StyleCatalog::SexIdxOf(RE::TESNPC* a_npc) {
        return (a_npc && a_npc->GetSex() == RE::SEXES::kFemale) ? RE::SEXES::kFemale
                                                                 : RE::SEXES::kMale;
    }

    FitReason StyleCatalog::EvaluateFitFor(RE::TESObjectARMO* a_armo, RE::TESRace* a_race,
                                            int a_sexIdx) {
        if (!a_armo || !a_race) {
            return FitReason::kFits;  // fail open - never a false red
        }
        bool anyRaceValid = false;
        for (auto* arma : a_armo->armorAddons) {
            if (!arma || !arma->IsValidRace(a_race)) {
                continue;
            }
            anyRaceValid = true;
            if (ArmaHasSexModel(arma, a_sexIdx)) {
                return FitReason::kFits;  // same armature fits race AND sex
            }
        }
        // A race-valid armature with no mesh for the target's sex is
        // gender-exclusive to the other sex - trying it on renders nothing
        // (and previewing the wrong-sex mesh has crashed the skinning pass).
        return anyRaceValid ? FitReason::kNoSex : FitReason::kNoRace;
    }

    FitReason StyleCatalog::EvaluateFit(RE::TESObjectARMO* a_armo) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        return EvaluateFitFor(a_armo, player ? player->GetRace() : nullptr, PlayerSexIdx());
    }

    std::string StyleCatalog::FitReasonText(FitReason a_reason) {
        switch (a_reason) {
            case FitReason::kNoSex:
                // Sourced from the fit cache's current SUBJECT (see
                // RefreshFitFor), not necessarily the player.
                return GetSingleton().fitSexIdx_ == RE::SEXES::kFemale ? "no female mesh"
                                                                        : "no male mesh";
            case FitReason::kCrashed:
                return "crashed the preview last time";
            default:  // kNoRace (and kFits, which never shows a tooltip)
                return "no " + std::string(GetSingleton().FitRaceName()) + " armature";
        }
    }

    void StyleCatalog::RefreshFitFor(RE::TESRace* a_race, int a_sexIdx) {
        fitRace_   = a_race;
        fitSexIdx_ = a_sexIdx;
        // The subject is the player iff these are literally the player's own
        // live race+sex right now. RefreshFit() always passes exactly that,
        // so it always lands here true. An NPC target's race/sex coinciding
        // with the player's is a harmless false positive: EvaluateFitFor's
        // result depends only on (race, sexIdx), so "clobbering" back to the
        // player later would recompute byte-identical values anyway.
        auto* player        = RE::PlayerCharacter::GetSingleton();
        fitSubjectIsPlayer_ = player && a_race == player->GetRace() && a_sexIdx == PlayerSexIdx();
        if (!a_race) {
            return;
        }
        std::size_t unfit     = 0;
        std::size_t noSex     = 0;
        std::size_t armorSeen = 0;
        for (auto& it : items_) {
            // A weapon has no armature to walk - it can only ever be kFits here.
            it.fitReason = it.IsWeapon() ? FitReason::kFits : EvaluateFitFor(it.Armo(), a_race, a_sexIdx);
            // A style that crashed the preview last session is flagged
            // regardless of race/sex fit - it must not be previewed again.
            // GLOBAL (CrashGuard is not per-target): a crashing mesh crashes
            // for any wearer, not just the subject this cache is for.
            if (CrashGuard::IsCrasher(it.key)) {
                it.fitReason = FitReason::kCrashed;
            }
            it.fitsBody = it.fitReason == FitReason::kFits;
            // Counted over ARMOR only, both sides of the ratio: this line reports
            // the race/sex fit check, which weapons do not take part in, and
            // including them would dilute it toward zero as the catalog grows.
            // A weapon CAN still be flagged (kCrashed) - CrashGuard's own load
            // line reports those.
            if (it.IsWeapon()) {
                continue;
            }
            ++armorSeen;
            if (!it.fitsBody) {
                ++unfit;
                if (it.fitReason == FitReason::kNoSex) {
                    ++noSex;
                }
            }
        }
        const char* edid = a_race->GetFormEditorID();
        spdlog::info("StyleCatalog: fit check vs race '{}' \"{}\" ({:08X}), sex {}: {} of {} "
                     "armor styles flagged may-not-fit ({} of them wrong-gender).",
                     edid && *edid ? edid : "?", a_race->GetName(), a_race->GetFormID(),
                     a_sexIdx == RE::SEXES::kFemale ? "F" : "M", unfit, armorSeen, noSex);
    }

    void StyleCatalog::RefreshFit() {
        auto* player = RE::PlayerCharacter::GetSingleton();
        RefreshFitFor(player ? player->GetRace() : nullptr, PlayerSexIdx());
        // The skin identifies the BODY SYSTEM independently of the race - a
        // body mod can ride on custom races (UBE_AllRace) or on a skin
        // override; the pair answers which one this character uses. Only
        // meaningful for the player (the only subject with a live Actor*
        // here - an NPC target is refreshed from bare race+sex via
        // RefreshFitFor, including "(away)" persisted assignees with no
        // loaded Actor*), so it stays a player-only second log line rather
        // than a parameter RefreshFitFor would have to accept and usually
        // not have.
        if (player) {
            auto* skin = REAug::GetActorSkin(player);
            spdlog::info("StyleCatalog: player skin {:08X} '{}'.", skin ? skin->GetFormID() : 0,
                         skin && skin->GetFormEditorID() ? skin->GetFormEditorID() : "?");
        }
    }

    void StyleCatalog::EnsureFitCurrent() {
        // Only self-heals the PLAYER's cache after a live race change
        // (RaceMenu). Gated on fitSubjectIsPlayer_ (see RefreshFitFor): once
        // the editor targets an NPC, this must NOT clobber that deliberate
        // cache back to the player just because it happened to run (e.g. at
        // editor open) while the NPC target's cache was live - restoring the
        // player's cache is the editor's job (RefreshFit() on target-switch-
        // back / close), not this self-heal's. When no NPC target is ever
        // selected, fitSubjectIsPlayer_ is always true and this behaves
        // exactly as before.
        if (!fitSubjectIsPlayer_) {
            return;
        }
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
            if (a_collectedOnly && !a_collection.Knows(a_it.form->GetFormID())) {
                return false;
            }
            if (a_favoritesOnly && !Favorites::IsFavorite(a_it.key)) {
                return false;
            }
            // Unfit styles are NEVER hidden (detection has false positives, and
            // silently missing items read as a broken catalog) - they render
            // red instead. See research/ube-fit-detection.md.
            //
            // A sword is neither light, heavy nor clothing - the armor-class
            // filter cannot judge one.
            if (!a_it.IsWeapon() && a_armorType >= 0 &&
                a_it.armorType != static_cast<std::uint8_t>(a_armorType)) {
                return false;
            }
            return ContainsCI(a_it.name, a_search) || ContainsCI(a_it.source, a_search);
        }
    }

    std::vector<const StyleItem*> StyleCatalog::Query(std::uint32_t a_bit,
                                                      std::string_view a_search,
                                                      bool a_collectedOnly, int a_armorType,
                                                      bool a_favoritesOnly,
                                                      std::optional<WeaponClass> a_weaponClass) const {
        std::vector<const StyleItem*> out;
        auto&                         collection = Collection::GetSingleton();
        for (const auto& it : items_) {
            // A weapon query wants exactly its class; an armor query wants the
            // armor items on that slot bit. Neither dimension leaks.
            const bool dimension = a_weaponClass ? it.weaponClass == a_weaponClass
                                                 : (!it.IsWeapon() && it.primaryBit == a_bit);
            if (dimension &&
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
            // A weapon has no slot bit (primaryBit stays 0) - without this every
            // weapon would light bit 0 in the slot list.
            if (!it.IsWeapon() && !((mask >> it.primaryBit) & 1u) &&
                Matches(it, a_search, a_collectedOnly, a_armorType, a_favoritesOnly, collection)) {
                mask |= 1u << it.primaryBit;
            }
        }
        return mask;
    }

}  // namespace OS
