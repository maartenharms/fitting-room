#pragma once
#include "PCH.h"

// TESObjectREFR::InstantiateHitShader and InstantiateHitArt are DECLARED by
// alandtse's CommonLibSSE-NG and no longer DEFINED: CharmedBaryon's 3.7.0
// dropped the two bodies and kept the declarations, and the fork inherits
// that, so a call compiles and then links against nothing (LNK2019 on the
// 2026-09-14 migration; BardHero met the same two on 09-12). These are the
// 3.6.0 bodies this mod shipped on, the same ids and the same argument order,
// as free functions with the reference first. The ids sit in the self-check
// table too, so a log from a runtime we do not have says whether they resolve.
//
// ⚠ NEVER DEREFERENCE WHAT THEY RETURN. Requip.cpp measured the shader call
// coming back with 1 in rbx on 1.6.1170, truthy and not a pointer; the effect
// still plays. Both call sites ignore the value.
namespace OS::HitFx {
    inline constexpr REL::RelocationID kInstantiateHitShader{ 19446, 19872 };
    inline constexpr REL::RelocationID kInstantiateHitArt{ 22289, 22769 };

    inline RE::ShaderReferenceEffect* InstantiateHitShader(
        RE::TESObjectREFR* a_ref, RE::TESEffectShader* a_shader, float a_dur,
        RE::TESObjectREFR* a_facingRef = nullptr, bool a_faceTarget = false,
        bool a_attachToCamera = false, RE::NiAVObject* a_attachNode = nullptr,
        bool a_interfaceEffect = false) {
        using func_t = RE::ShaderReferenceEffect* (*)(RE::TESObjectREFR*, RE::TESEffectShader*,
                                                      float, RE::TESObjectREFR*, bool, bool,
                                                      RE::NiAVObject*, bool);
        static REL::Relocation<func_t> func{ kInstantiateHitShader };
        return func(a_ref, a_shader, a_dur, a_facingRef, a_faceTarget, a_attachToCamera,
                    a_attachNode, a_interfaceEffect);
    }

    inline RE::ModelReferenceEffect* InstantiateHitArt(
        RE::TESObjectREFR* a_ref, RE::BGSArtObject* a_art, float a_dur,
        RE::TESObjectREFR* a_facingRef, bool a_faceTarget, bool a_attachToCamera,
        RE::NiAVObject* a_attachNode = nullptr, bool a_interfaceEffect = false) {
        using func_t = RE::ModelReferenceEffect* (*)(RE::TESObjectREFR*, RE::BGSArtObject*, float,
                                                     RE::TESObjectREFR*, bool, bool,
                                                     RE::NiAVObject*, bool);
        static REL::Relocation<func_t> func{ kInstantiateHitArt };
        return func(a_ref, a_art, a_dur, a_facingRef, a_faceTarget, a_attachToCamera,
                    a_attachNode, a_interfaceEffect);
    }
}
