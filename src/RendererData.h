#pragma once
#include "PCH.h"

#include <d3d11.h>

#include <cstddef>

// RE::NiTexture::RendererData with the SDK's D3D11 types in its two pointer
// fields. alandtse's library spells them as REX::W32::ID3D11Texture2D and
// REX::W32::ID3D11ShaderResourceView, its own declarations of the same COM
// interfaces, and this mod's dye and bake code talks to the device with the
// SDK's. Same bytes, same vtables. The static_asserts hold the mirror to the
// library's layout, so a library change breaks the build here rather than a
// texture at run time. Allocation stays on the game's heap, as the engine
// frees these.
namespace OS {
    // TES_HEAP_REDEFINE_NEW spells the namespace bare, the way the library's
    // own RE and REL namespaces alias it.
    namespace stl = SKSE::stl;

    struct RendererData {
        RendererData(std::uint16_t a_width, std::uint16_t a_height) noexcept :
            width(a_width), height(a_height) {}

        ID3D11Texture2D*          texture{ nullptr };       // 00
        std::uint64_t             unk08{ 0 };               // 08
        ID3D11ShaderResourceView* resourceView{ nullptr };  // 10
        std::uint16_t             width;                    // 18
        std::uint16_t             height;                   // 1A
        std::uint8_t              unk1C{ 1 };               // 1C
        std::uint8_t              unk1D{ 0x1C };            // 1D
        std::uint16_t             unk1E{ 0 };               // 1E
        std::uint32_t             unk20{ 1 };               // 20
        std::uint32_t             unk24{ 0x130012 };        // 24

        TES_HEAP_REDEFINE_NEW();
    };
    static_assert(sizeof(RendererData) == sizeof(RE::NiTexture::RendererData));
    static_assert(offsetof(RendererData, texture) == offsetof(RE::NiTexture::RendererData, texture));
    static_assert(offsetof(RendererData, resourceView) ==
                  offsetof(RE::NiTexture::RendererData, resourceView));
    static_assert(offsetof(RendererData, width) == offsetof(RE::NiTexture::RendererData, width));
    static_assert(offsetof(RendererData, height) == offsetof(RE::NiTexture::RendererData, height));
    static_assert(offsetof(RendererData, unk1C) == offsetof(RE::NiTexture::RendererData, unk1C));
    static_assert(offsetof(RendererData, unk1D) == offsetof(RE::NiTexture::RendererData, unk1D));
    static_assert(offsetof(RendererData, unk24) == offsetof(RE::NiTexture::RendererData, unk24));
}
