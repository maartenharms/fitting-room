#pragma once

#include "PreviewGrid.h"
#include "TextureLoad.h"

// The capture side of OS-192: forms to plain TextureSwapEntry data, on the
// main thread, where the entry lists are built (a preview build must never
// walk the form graph, the grid's standing rule). The gate lives here so a
// runtime without the BSShaderTextureSet ids captures NOTHING: no swap in
// any key, none in any render, the two halves consistent in failure too.
namespace OS::PreviewSwapCapture {

    // The gate is TextureLoad's, which owns the one REL surface of loading by
    // path (BSShaderTextureSet creation, SE 99886 / AE 107172, IdOk first).
    // Kept as a name here so the capture side keeps reading as it did.
    [[nodiscard]] inline bool LoaderOk() {
        static const bool ok = [] {
            const bool present = OS::TextureLoad::LoaderOk();
            if (!present) {
                spdlog::warn(
                    "PreviewSwapCapture: no loader by path on this runtime, so "
                    "colour-variant cards render the base look this session.");
            }
            return present;
        }();
        return ok;
    }

    // The record paths of a texture set, unrooted, TX00..TX07. Plain data
    // members on the form; no vfunc.
    inline void FillPaths(PreviewGrid::TextureSwapEntry& a_entry,
                          const RE::BGSTextureSet* a_set) {
        for (std::size_t t = 0; t < a_entry.texPaths.size(); ++t) {
            const char* p = a_set->textures[t].textureName.c_str();
            if (p && *p) {
                a_entry.texPaths[t] = p;
            }
        }
    }

    // A model's MODS list as plain data. Entries with index -1 or no set
    // are dropped, which is the engine's own table-build rule
    // (docs/superpowers/research/re_verify/ae_swap_applier.c).
    [[nodiscard]] inline std::vector<PreviewGrid::TextureSwapEntry> FromModel(
        const RE::TESModelTextureSwap& a_model) {
        std::vector<PreviewGrid::TextureSwapEntry> out;
        if (!a_model.alternateTextures || a_model.numAlternateTextures == 0 ||
            !LoaderOk()) {
            return out;
        }
        for (std::uint32_t i = 0; i < a_model.numAlternateTextures; ++i) {
            const auto& alt = a_model.alternateTextures[i];
            if (!alt.textureSet ||
                alt.index3D == static_cast<std::uint32_t>(-1)) {
                continue;
            }
            PreviewGrid::TextureSwapEntry e;
            e.geomIndex = alt.index3D;
            FillPaths(e, alt.textureSet);
            out.push_back(std::move(e));
        }
        return out;
    }

    // A head part's TNAM as the apply-to-every-mesh entry. Main model only;
    // extra parts keep their own look, which is what the engine does.
    [[nodiscard]] inline std::vector<PreviewGrid::TextureSwapEntry> FromTextureSet(
        const RE::BGSTextureSet* a_set) {
        std::vector<PreviewGrid::TextureSwapEntry> out;
        if (!a_set || !LoaderOk()) {
            return out;
        }
        PreviewGrid::TextureSwapEntry e;
        e.geomIndex = PreviewGrid::TextureSwapEntry::kAllGeometry;
        FillPaths(e, a_set);
        out.push_back(std::move(e));
        return out;
    }

}  // namespace OS::PreviewSwapCapture
