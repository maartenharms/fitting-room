#pragma once

#include "PCH.h"

#include <string>
#include <string_view>

// Loading a texture by PATH through the engine's own loader, from anywhere in
// the plugin that needs a texture it can name and does not already hold.
//
// ⚠ THIS IS THE ONE LOADER, SHARED. MeshExtractor grew it for the card swap
// (OS-192) and OverlayBake needs the same call for the overlay it resamples;
// two copies of a vtable-slot call with an SEH frame around it would be two
// readers of one answer, and the second would drift the first time the slot
// or the prefix rule was corrected. two-readers-of-one-answer-drift-in-the-gap.
//
// How it works: an engine BSShaderTextureSet is created (SE 99886 / AE
// 107172, IdOk first), the path is set into slot 0, and the set's loader vfunc
// (slot 38, the exact call BSLightingShaderMaterialBase::OnLoadTextureSet
// makes) fills an NiPointer. The engine's texture cache dedups by path under
// it, so a path already resident comes back as the live object.
//
// ⚠⚠ THE PATH IS ROOTED "Data\Textures\" BEFORE THE SET SEES IT. The engine
// itself hands this loader a stack buffer with that exact prefix
// (docs/superpowers/research/re_verify/ae_swap_helpers.c). An override path
// as skee stores it ("Actors\Character\Overlays\X.dds") is relative to
// textures\ and needs the root put on; a path that already carries
// "textures\" must NOT get it twice, which is the 2026-08-09 purple-eye defect.
//
// ⚠⚠ A MISSING FILE IS NOT A NULL. The engine substitutes its placeholder and
// hands back a live NiSourceTexture with a clean log, so nothing downstream
// can tell the two apart by pointer (missing-texture-resolves-to-a-
// placeholder). Callers that need to know check a pixel, a size, or the file.
namespace OS::TextureLoad {

    // Whether the loader can run on this runtime at all (the create id is
    // present in the Address Library). Says so in the log once when it is not.
    [[nodiscard]] bool LoaderOk();

    // a_rooted is the full "Data\Textures\..." spelling.
    [[nodiscard]] RE::NiPointer<RE::NiSourceTexture> LoadRooted(const std::string& a_rooted);

    // a_relative is an override path, relative to textures\, either slash,
    // with or without a leading "textures\" segment.
    [[nodiscard]] RE::NiPointer<RE::NiSourceTexture> LoadRelative(std::string_view a_relative);

    // The rooted spelling LoadRelative would use, for a log line or an
    // existence check on disk.
    [[nodiscard]] std::string Rooted(std::string_view a_relative);

}  // namespace OS::TextureLoad
