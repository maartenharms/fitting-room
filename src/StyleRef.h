#pragma once

#include "PCH.h"
#include "Outfit.h"

namespace OS::StyleRef {

    // {modName, localFormID} -> live ARMO in the CURRENT load order.
    // Returns nullptr when the plugin is absent or the form is not an armor.
    RE::TESObjectARMO* Resolve(const StyleRefKey& a_key);

    // The weapon-dimension counterparts. Same key shape, same lookup - only the
    // expected form type differs, so a key naming an ARMO comes back nullptr
    // here (LookupForm type-checks). Callers treat nullptr as passthrough.
    RE::TESObjectWEAP* ResolveWeapon(const StyleRefKey& a_key);
    RE::TESAmmo*       ResolveAmmo(const StyleRefKey& a_key);

    // The live style form of ANY dimension (ARMO, WEAP or AMMO), for callers
    // that hold a key without knowing which kind it names - the Collection
    // keeps all three in one map. Returns nullptr when the plugin is absent or
    // the form is not a style form at all.
    RE::TESBoundObject* ResolveAny(const StyleRefKey& a_key);

    // Style form (ARMO, WEAP or AMMO) -> persistable key. Uses GetFile(0) (the
    // DEFINING master, not the last override) so modName matches the plugin
    // localFormID belongs to. GetLocalFormID() is ESL-correct (low 12 bits for
    // FE plugins). Both are plain TESForm members, so one function serves every
    // dimension; only the Resolve* that reads the key back differs.
    bool Make(RE::TESForm* a_form, StyleRefKey& a_out);

}  // namespace OS::StyleRef
