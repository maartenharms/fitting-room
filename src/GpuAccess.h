#pragma once
#include "PCH.h"

#include <d3d11.h>
#include <dxgi.h>

// The engine's device, immediate context, swap chain and framebuffer view, as
// the SDK types this mod's D3D11 code is written against.
//
// alandtse's library hands them out from RE::BSGraphics::Renderer as its own
// REX::W32 declarations of the same COM interfaces. Same objects, same
// vtables, so a reinterpret_cast at this one seam is the whole translation and
// no caller carries a REX::W32 name. The old BSRenderManager's flat swapChain
// and resourceView are renderWindows[0]'s; both headers put them at the same
// distance from the device pointer (+0x28 and +0x48, read on 2026-09-14).
namespace OS::Gpu {
    [[nodiscard]] inline RE::BSGraphics::RendererData* Data() {
        auto* const renderer = RE::BSGraphics::Renderer::GetSingleton();
        return renderer ? &renderer->GetRuntimeData() : nullptr;
    }

    [[nodiscard]] inline ID3D11Device* Device() {
        auto* const d = Data();
        return d ? reinterpret_cast<ID3D11Device*>(d->forwarder) : nullptr;
    }

    [[nodiscard]] inline ID3D11DeviceContext* Context() {
        auto* const d = Data();
        return d ? reinterpret_cast<ID3D11DeviceContext*>(d->context) : nullptr;
    }

    [[nodiscard]] inline IDXGISwapChain* SwapChain() {
        auto* const d = Data();
        return d ? reinterpret_cast<IDXGISwapChain*>(d->renderWindows[0].swapChain) : nullptr;
    }

    [[nodiscard]] inline ID3D11ShaderResourceView* FramebufferView() {
        auto* const d = Data();
        return d ? reinterpret_cast<ID3D11ShaderResourceView*>(d->renderWindows[0].resourceView)
                 : nullptr;
    }
}
