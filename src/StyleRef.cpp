#include "StyleRef.h"

namespace OS::StyleRef {

    RE::TESObjectARMO* Resolve(const StyleRefKey& a_key) {
        if (a_key.modName.empty()) {
            return nullptr;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        return dh ? dh->LookupForm<RE::TESObjectARMO>(a_key.localFormID, a_key.modName) : nullptr;
    }

    RE::TESObjectWEAP* ResolveWeapon(const StyleRefKey& a_key) {
        if (a_key.modName.empty()) {
            return nullptr;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        return dh ? dh->LookupForm<RE::TESObjectWEAP>(a_key.localFormID, a_key.modName) : nullptr;
    }

    RE::TESAmmo* ResolveAmmo(const StyleRefKey& a_key) {
        if (a_key.modName.empty()) {
            return nullptr;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        return dh ? dh->LookupForm<RE::TESAmmo>(a_key.localFormID, a_key.modName) : nullptr;
    }

    RE::TESBoundObject* ResolveAny(const StyleRefKey& a_key) {
        if (a_key.modName.empty()) {
            return nullptr;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return nullptr;
        }
        auto* form = dh->LookupForm(a_key.localFormID, a_key.modName);
        if (!form) {
            return nullptr;
        }
        switch (form->GetFormType()) {
            case RE::FormType::Armor:
            case RE::FormType::Weapon:
            case RE::FormType::Ammo:
                return form->As<RE::TESBoundObject>();
            default:
                return nullptr;  // a key that no longer names a style form
        }
    }

    bool Make(RE::TESForm* a_form, StyleRefKey& a_out) {
        if (!a_form) {
            return false;
        }
        auto* file = a_form->GetFile(0);
        if (!file) {
            return false;
        }
        a_out.modName     = std::string{ file->GetFilename() };
        a_out.localFormID = a_form->GetLocalFormID();
        return true;
    }

}  // namespace OS::StyleRef
