#pragma once

#include "PCH.h"
#include "Outfit.h"

namespace OS::StyleRef {

    // {modName, localFormID} -> live ARMO in the CURRENT load order.
    // Returns nullptr when the plugin is absent or the form is not an armor.
    RE::TESObjectARMO* Resolve(const StyleRefKey& a_key);

    // ARMO -> persistable key. Uses GetFile(0) (the DEFINING master, not the last
    // override) so modName matches the plugin localFormID belongs to.
    // GetLocalFormID() is ESL-correct (low 12 bits for FE plugins).
    bool Make(RE::TESObjectARMO* a_armo, StyleRefKey& a_out);

}  // namespace OS::StyleRef
