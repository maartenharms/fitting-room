#include "StyleCatalog.h"

#include "Collection.h"
#include "CrashGuard.h"
#include "Favorites.h"
#include "Mannequin.h"
#include "PreviewSwapCapture.h"
#include "REAugments.h"
#include "RecentMods.h"
#include "Settings.h"
#include "SlotMask.h"
#include "StyleRef.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <map>
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
                         a_armo ? a_armo->GetSlotMask().underlying() : 0u,
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
            const auto mask = armo->GetSlotMask().underlying();
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
            item.primaryBit = PrimaryBitFor(mask);
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
            // The preview grid renders from this without touching the form
            // again; `model` was already validated non-empty above. The
            // model's own alternate textures ride along (OS-192): the same
            // subobject WeaponLookKey already treats as look identity, so
            // "the same blade retextured" entries stop sharing one picture.
            item.modelPaths.push_back(model);
            if (auto captured = PreviewSwapCapture::FromModel(*swap);
                !captured.empty()) {
                item.swaps.push_back(std::move(captured));
            }
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
        // The maps remember WHICH kept row each key landed on, not merely that
        // it was seen: a collapsed record has to be recorded on its
        // representative's variantIds, or the Collection filter can only ever
        // see the one record of the group that happened to sort first.
        std::vector<StyleItem>                                     unique;
        std::map<std::vector<RE::TESObjectARMA*>, std::size_t>     seen;
        std::map<std::pair<WeaponClass, std::string>, std::size_t> seenWeapon;  // (class, look)
        std::size_t                                                armorCollapsed  = 0;
        std::size_t                                                weaponCollapsed = 0;
        for (auto& it : items_) {
            // Two maps with unrelated iterator types, so the branch resolves
            // to (kept index, fresh?) rather than to an iterator.
            std::size_t keptAt = 0;
            bool        fresh  = false;
            if (it.IsWeapon()) {
                const auto [pos, ins] = seenWeapon.emplace(
                    std::pair{ *it.weaponClass, WeaponLookKey(WeaponSwapOf(it.form)) },
                    unique.size());
                keptAt = pos->second;
                fresh  = ins;
            } else {
                const auto [pos, ins] = seen.emplace(addonKey(it), unique.size());
                keptAt = pos->second;
                fresh  = ins;
            }
            if (fresh) {
                unique.push_back(std::move(it));
                continue;
            }
            unique[keptAt].variantIds.push_back(it.form->GetFormID());
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
        std::size_t weaponPaths = 0;
        for (const auto& it : items_) {
            if (it.IsWeapon() && it.HasPreviewScene()) {
                ++weaponPaths;
            }
        }
        spdlog::info("StyleCatalog: {} weapon/ammo style(s) carry a model path for the "
                     "preview grid.",
                     weaponPaths);

        // Every form -> its look's row, so a Collection question about ANY
        // record of a group (a preset naming the enchanted variant, say)
        // reaches the row that group collapsed onto. Built here, after items_
        // has settled, so no index can outlive the vector it points into.
        lookIndex_.clear();
        lookIndex_.reserve(items_.size() + armorCollapsed + weaponCollapsed);
        for (std::size_t i = 0; i < items_.size(); ++i) {
            lookIndex_.emplace(items_[i].form->GetFormID(), i);
            for (const auto id : items_[i].variantIds) {
                lookIndex_.emplace(id, i);
            }
        }

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
        // Owned-at-some-point, asked of the whole collapsed look rather than
        // of the one record that represents it. See StyleItem::variantIds.
        bool LookCollected(const StyleItem& a_it, const Collection& a_collection) {
            if (a_collection.Knows(a_it.form->GetFormID())) {
                return true;  // the common case: the representative itself
            }
            return std::ranges::any_of(a_it.variantIds, [&](RE::FormID a_id) {
                return a_collection.Knows(a_id);
            });
        }
    }

    bool LookIsNew(const StyleItem& a_item) {
        auto& collection = Collection::GetSingleton();
        if (!LookCollected(a_item, collection)) {
            return false;  // never owned: absent, not new
        }
        // ⚠ ACKNOWLEDGED IF **ANY** OWNED RECORD IS, which is deliberately the
        // opposite quantifier to "new if any is unacknowledged". One row is one
        // picture with one name, so one click settles it. The other rule would
        // re-light a look the player has seen for years every time a WACCF
        // re-issue or an enchanted variant of it first entered their pack, and
        // the row would look identical each time.
        //
        // known && !IsNew IS "known and acknowledged", since IsNew is known &&
        // not acknowledged. Spelled out rather than given a helper because the
        // pair reads as a contradiction at a glance and the helper would hide
        // that rather than answer it.
        const auto acknowledged = [&](RE::FormID a_id) {
            return collection.Knows(a_id) && !collection.IsNew(a_id);
        };
        if (acknowledged(a_item.form->GetFormID())) {
            return false;
        }
        return !std::ranges::any_of(a_item.variantIds, acknowledged);
    }

    void AcknowledgeLook(const StyleItem& a_item) {
        auto& collection = Collection::GetSingleton();
        // Acknowledge ignores an id the collection does not know, so this is a
        // walk rather than a filtered walk.
        collection.Acknowledge(a_item.form->GetFormID());
        for (const auto id : a_item.variantIds) {
            collection.Acknowledge(id);
        }
    }

    bool StyleCatalog::IsLookCollected(RE::FormID a_formID) const {
        auto& collection = Collection::GetSingleton();
        if (const auto it = lookIndex_.find(a_formID); it != lookIndex_.end()) {
            return LookCollected(items_[it->second], collection);
        }
        // Not indexed (dropped by a Build() filter, or not a style form at
        // all): the record is all we can speak for.
        return collection.Knows(a_formID);
    }

    namespace {
        int PlayerSexIdx() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            return StyleCatalog::SexIdxOf(player ? player->GetActorBase() : nullptr);
        }
        const char* RaceEdid(RE::TESRace* a_race) {
            if (!a_race) {
                return "(none)";
            }
            const char* edid = a_race->GetFormEditorID();
            return edid && *edid ? edid : "(unnamed)";
        }
        // ⚠ THE ENGINE WEARS THE MALE MODEL WHEN THE FEMALE PATH IS EMPTY,
        // and half of vanilla is authored on exactly that convention. Measured
        // against the load order 2026-08-20: Skyrim.esm's StormCloakBootsAA
        // ("Fur Boots") ships male='Armor\StormCloaks\BootsM_1.nif',
        // female=NONE, and every female hold guard on screen is wearing it.
        // Reading the empty path as "no female mesh" called 656 of 8631 armor
        // styles kNoSex on a Bosmer female, which excluded them from the
        // browser AND from set completion's candidate pool - the field report
        // was "guard presets are missing shoes" and "we don't have fur boots
        // in this load order", one cause. The fallback is ASYMMETRIC, female
        // to male only: a female-only piece on a male renders nothing (the
        // invisible-armour report every female-only mod page carries), so a
        // male subject still requires the male path itself.
        //
        // One answerer for "which model slot will the engine use": the fit
        // verdict and the card's path collector both ask HERE, because two
        // readers of one rule drift in the gap - a style that fits but
        // collects no mesh is a set completed with an invisible boot.
        const RE::TESModelTextureSwap* EffectiveBipedModel(RE::TESObjectARMA* a_arma,
                                                           int a_sexIdx) {
            if (!a_arma) {
                return nullptr;
            }
            const auto& own     = a_arma->bipedModels[a_sexIdx];
            const char* ownPath = own.GetModel();
            if (ownPath && *ownPath) {
                return &own;
            }
            if (a_sexIdx == RE::SEXES::kFemale) {
                const auto& male     = a_arma->bipedModels[RE::SEXES::kMale];
                const char* malePath = male.GetModel();
                if (malePath && *malePath) {
                    return &male;
                }
            }
            return nullptr;
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
        // ⚠ THE ARMOUR RACE COUNTS TOO, AND ASKING ONLY ABOUT THE LITERAL RACE
        // WAS WRONG BY FOUR STYLES IN TEN. TESRace::armorParentRace is the CK's
        // Armor Race (RNAM), and it is what the ENGINE resolves an armature
        // through when it dresses somebody. A custom body race points RNAM at
        // DefaultRace precisely so ordinary armour works on it, and almost no
        // armour mod lists the custom race in its own ARMA. So a character on
        // one of those races got told that four in ten installed styles had "no
        // armature", and the hide-unfit filter then took them off the page -
        // which from the player's side is whole plugins going missing.
        //
        // Measured on the reference load order 2026-08-07: race
        // '00UBE_DarkElfRace', 343 of 865 armor styles flagged, and NONE of them
        // for the wrong gender. That count is the report "sometimes other esps
        // are not appearing for the player or follower", and it explains the
        // sometimes: it depends entirely on whether the subject's race is a
        // custom one, so a follower on a vanilla race looked fine beside a
        // player who did not.
        //
        // ⚠ ONE LEVEL, NOT A WALK. RNAM is where the engine looks and vanilla
        // races point theirs at DefaultRace, whose own RNAM is itself. The
        // identity guard is what stops that self-reference costing a second
        // IsValidRace call on every armature of every style.
        RE::TESRace* const armorRace = a_race->armorParentRace;
        const auto         validFor  = [&](RE::TESObjectARMA* a_arma) {
            return a_arma->IsValidRace(a_race) ||
                   (armorRace && armorRace != a_race && a_arma->IsValidRace(armorRace));
        };
        bool anyRaceValid = false;
        for (auto* arma : a_armo->armorAddons) {
            if (!arma || !validFor(arma)) {
                continue;
            }
            anyRaceValid = true;
            if (EffectiveBipedModel(arma, a_sexIdx)) {
                return FitReason::kFits;  // the engine has a model to wear
            }
        }
        // A race-valid armature with no model the engine would fall back to is
        // exclusive to the other sex - trying it on renders nothing. After the
        // female-to-male fallback above this is in practice a male subject and
        // a female-only piece; the reverse direction wears the male mesh and
        // counts as a fit, because that is what the engine puts on screen.
        return anyRaceValid ? FitReason::kNoSex : FitReason::kNoRace;
    }

    FitReason StyleCatalog::EvaluateFit(RE::TESObjectARMO* a_armo) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        return EvaluateFitFor(a_armo, player ? player->GetRace() : nullptr, PlayerSexIdx());
    }

    void StyleCatalog::CollectArmourPaths(
        RE::TESObjectARMO* a_armo, RE::TESRace* a_race, int a_sexIdx,
        std::vector<std::string>& a_out,
        std::vector<std::vector<PreviewGrid::TextureSwapEntry>>& a_swaps) {
        a_out.clear();
        a_swaps.clear();
        if (!a_armo || !a_race) {
            return;
        }
        // The same race question EvaluateFitFor asks, RNAM fallback included:
        // the fallback carries 343 of 865 styles on the reference load order,
        // so a collector without it blanks about 40% of the grid on a
        // custom-race character.
        RE::TESRace* const armorRace = a_race->armorParentRace;
        bool               anySwap   = false;
        for (auto* arma : a_armo->armorAddons) {
            if (!arma) {
                continue;
            }
            const bool raceValid =
                arma->IsValidRace(a_race) ||
                (armorRace && armorRace != a_race && arma->IsValidRace(armorRace));
            if (!raceValid) {
                continue;
            }
            // The colour of a variant lives on THIS subobject: the record
            // research found every field-named family (monk robes, Dunmer
            // Outfit, LDD) as per-variant ARMAs sharing one NIF, each
            // carrying its own alternate textures on the per-sex model
            // (docs/superpowers/specs/2026-08-09-preview-grid-texture-swaps-design.md).
            //
            // The slot the ENGINE would dress, not the subject's literal one:
            // EffectiveBipedModel is the same female-to-male fallback the fit
            // verdict uses, and the swaps ride the model that is actually
            // worn, so a unisex boot's card carries the male mesh and the
            // male slot's alternate textures together.
            if (const auto* model = EffectiveBipedModel(arma, a_sexIdx)) {
                a_out.emplace_back(model->GetModel());
                a_swaps.push_back(PreviewSwapCapture::FromModel(*model));
                anySwap = anySwap || !a_swaps.back().empty();
            }
        }
        // Absent-when-empty: a scene with no swap anywhere must not carry a
        // sized vector, so the disk key cannot grow a stray suffix and the
        // byte-identity pin holds structurally.
        if (!anySwap) {
            a_swaps.clear();
        }
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
            Mannequin::Clear();
            return;
        }
        // The mannequin's parts, resolved once for this target rather than
        // per card (OS-204). ⚠ THE PLAYER'S OWN SKIN WHEN THE TARGET IS THE
        // PLAYER, the race's otherwise: the skin identifies the BODY SYSTEM
        // independently of the race, which is the point RefreshFit's own log
        // line already makes, and an NPC target here has no live Actor* to
        // ask. race->skin is the engine's own base-body answer, so the
        // fallback is the engine's rather than a guess.
        Mannequin::Refresh(fitSubjectIsPlayer_ && player ? REAug::GetActorSkin(player)
                                                         : a_race->skin,
                           a_race, a_sexIdx);
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
            // The preview grid's scene, resolved here because this walk
            // already knows the target's race and sex and runs on the main
            // thread. kFits is the gate: an unfit style keeps no scene and
            // its card draws the cross rather than a wrong-sex mesh
            // (previewing those has crashed the skinning pass).
            if (!it.IsWeapon()) {
                if (it.fitReason == FitReason::kFits) {
                    CollectArmourPaths(it.Armo(), a_race, a_sexIdx, it.modelPaths,
                                       it.swaps);
                } else {
                    it.modelPaths.clear();
                    it.swaps.clear();
                }
            }
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
        // ⚠ THE ARMOUR RACE IS NAMED IN THE LINE, and it is the only thing that
        // tells a healthy custom-race character from the bug this used to have.
        // A run reporting a high unfit count against a race whose RNAM is
        // DefaultRace is the symptom; the same count against a race that IS its
        // own armour race is a load order that genuinely has no armatures.
        auto* const armorRace = a_race->armorParentRace;
        const char* armorEdid = armorRace ? armorRace->GetFormEditorID() : nullptr;
        spdlog::info("StyleCatalog: fit check vs race '{}' \"{}\" ({:08X}), armour race "
                     "'{}', sex {}: {} of {} armor styles flagged may-not-fit ({} of them "
                     "wrong-gender).",
                     edid && *edid ? edid : "?", a_race->GetName(), a_race->GetFormID(),
                     armorEdid && *armorEdid ? armorEdid
                     : armorRace             ? "(unnamed)"
                                             : "(none)",
                     a_sexIdx == RE::SEXES::kFemale ? "F" : "M", unfit, armorSeen, noSex);
        std::size_t armourPaths = 0;
        for (const auto& it : items_) {
            if (!it.IsWeapon() && it.HasPreviewScene()) {
                ++armourPaths;
            }
        }
        spdlog::info("StyleCatalog: {} armour style(s) carry a preview scene for this "
                     "target.",
                     armourPaths);
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
            spdlog::info("StyleCatalog: fit cache check declined - subject is not the "
                         "player (cached '{}'). A race change will not be picked up "
                         "until the editor puts the player back.",
                         RaceEdid(fitRace_));
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* race   = player ? player->GetRace() : nullptr;
        // ⚠ SEX COUNTS TOO, AND IT DID NOT BEFORE. showracemenu changes sex as
        // readily as race, EvaluateFitFor reads both, and the tooltip says "no
        // female mesh" off fitSexIdx_ - so a sex swap left every one of those
        // three answering for the character who used to be there.
        const int sex = PlayerSexIdx();
        // ⚠ TWO SOURCES FOR THE RACE, AND THEY ARE PRINTED SEPARATELY ON
        // PURPOSE. Actor::GetRace() and the actor base's own race are the two
        // answers to "what race is the player", and this cache self-heals off
        // the first one only. Field evidence 2026-08-07: RaceMenu erased its
        // head-morph registry at 07:40 and started attaching vanilla-path
        // armour, and the fit check sixteen minutes later still named the UBE
        // race - so either nothing changed or one of these two lags the other.
        // Printing both costs a pointer read and tells those apart in one run.
        auto* const baseRace = player && player->GetActorBase()
                                   ? player->GetActorBase()->GetRace()
                                   : nullptr;
        const bool  stale    = race && (race != fitRace_ || sex != fitSexIdx_);
        spdlog::info("StyleCatalog: fit cache check - actor race '{}' ({:08X}), base race "
                     "'{}' ({:08X}), sex {}; cached race '{}', sex {} -> {}.",
                     RaceEdid(race), race ? race->GetFormID() : 0, RaceEdid(baseRace),
                     baseRace ? baseRace->GetFormID() : 0,
                     sex == RE::SEXES::kFemale ? "F" : "M", RaceEdid(fitRace_),
                     fitSexIdx_ == RE::SEXES::kFemale ? "F" : "M",
                     stale ? "REBUILDING" : "unchanged");
        if (stale) {
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
            if (a_collectedOnly && !LookCollected(a_it, a_collection)) {
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
                                          int a_armorType, bool a_favoritesOnly,
                                          bool a_hideUnfit) const {
        std::uint32_t mask       = 0;
        auto&         collection = Collection::GetSingleton();
        for (const auto& it : items_) {
            // A weapon has no slot bit (primaryBit stays 0) - without this every
            // weapon would light bit 0 in the slot list.
            if (it.IsWeapon() || ((mask >> it.primaryBit) & 1u)) {
                continue;
            }
            // ⚠⚠ THE BROWSER'S OWN DROP, COPIED EXACTLY. Matches() deliberately
            // never judges fit, because the browser wants unfit rows drawn red
            // rather than missing. The browser then erases them itself when
            // bBrowserHideUnfit is on, and this mask has to make the same cut
            // or it lights a slot on an item the pane will not show. Crashers
            // are left in, because the browser leaves them in too: it draws
            // them and blocks the click.
            if (a_hideUnfit && !it.fitsBody) {
                continue;
            }
            if (Matches(it, a_search, a_collectedOnly, a_armorType, a_favoritesOnly, collection)) {
                mask |= 1u << it.primaryBit;
            }
        }
        return mask;
    }

}  // namespace OS
