#include "StyleRef.h"

namespace OS::StyleRef {

    RE::TESObjectARMO* Resolve(const StyleRefKey& a_key) {
        if (a_key.modName.empty()) {
            return nullptr;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        return dh ? dh->LookupForm<RE::TESObjectARMO>(a_key.localFormID, a_key.modName) : nullptr;
    }

    bool Make(RE::TESObjectARMO* a_armo, StyleRefKey& a_out) {
        if (!a_armo) {
            return false;
        }
        auto* file = a_armo->GetFile(0);
        if (!file) {
            return false;
        }
        a_out.modName     = std::string{ file->GetFilename() };
        a_out.localFormID = a_armo->GetLocalFormID();
        return true;
    }

}  // namespace OS::StyleRef
