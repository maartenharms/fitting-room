#include "PCH.h"

#include "Portrait.h"

#include "DyeTexture.h"    // EnsurePresent: the thunk this pump rides
#include "EditorWindow.h"  // IsOpen: the capture's one gate
#include "FuckCompat.h"    // FUCK::Image
#include "PortraitPlan.h"
#include "PreviewPng.h"

#include <d3d11.h>
#include <d3d11_3.h>  // ID3D11ShaderResourceView1, BSRenderManager's view type
#include <dxgi.h>
#include <wrl/client.h>

#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace OS::Portrait {

    namespace {

        using Microsoft::WRL::ComPtr;

        [[nodiscard]] std::string Sanitize(const std::string& a_name) {
            std::string out;
            for (const char c : a_name) {
                const auto uc = static_cast<unsigned char>(c);
                out += (std::isalnum(uc) || c == ' ' || c == '-' || c == '_' ||
                        c == '!' || c == '(' || c == ')')
                           ? c
                           : '_';
            }
            while (!out.empty() && out.back() == ' ') out.pop_back();
            return out.empty() ? std::string{ "portrait" } : out;
        }

        [[nodiscard]] std::filesystem::path DirFor(Kind a_kind) {
            return std::filesystem::path(
                       "Data/SKSE/Plugins/FittingRoom/Portraits") /
                   (a_kind == Kind::kPreset ? "presets" : "looks");
        }

        [[nodiscard]] std::filesystem::path PathFor(Kind a_kind,
                                                    const std::string& a_name) {
            return DirFor(a_kind) / (Sanitize(a_name) + ".png");
        }

        [[nodiscard]] std::string MapKey(Kind a_kind, const std::string& a_name) {
            return (a_kind == Kind::kPreset ? "p:" : "l:") + Sanitize(a_name);
        }

        // ---- cross-thread state -------------------------------------------
        //
        // The request side is written from the FUCK draw thread and (for the
        // owed flow) the game thread; the pump consumes on the render
        // thread. One small mutex, held for pointer swaps only.
        struct Pending {
            Kind          kind{ Kind::kPreset };
            std::string   name;
            std::uint32_t delay{ 0 };
        };
        std::mutex             g_lock;
        std::optional<Pending> g_pending;
        std::optional<std::pair<Kind, std::string>> g_owed;
        // Encoding on its worker thread counts as pending too: a second
        // capture must not start while the first buffer is being written.
        std::atomic<bool> g_encoding{ false };
        // Stamped by the worker when a PNG lands, consumed by the pump to
        // reload the image; the string is the map key.
        std::mutex  g_doneLock;
        std::string g_doneKey;

        // ---- render-thread-only state -------------------------------------
        std::map<std::string, FUCK::Image> g_images;    // loaded portraits
        std::vector<std::string>           g_loadQueue; // keys awaiting load
        std::vector<FUCK::Image>           g_release;   // freed one present later

        std::atomic<bool> g_saidBadFormat{ false };

        [[nodiscard]] std::optional<PortraitPlan::PixelLayout> LayoutFor(
            DXGI_FORMAT a_format) {
            switch (a_format) {
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                    return PortraitPlan::PixelLayout::kRgba8;
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                    return PortraitPlan::PixelLayout::kBgra8;
                case DXGI_FORMAT_R10G10B10A2_UNORM:
                    return PortraitPlan::PixelLayout::kRgb10A2;
                case DXGI_FORMAT_R16G16B16A16_FLOAT:
                    return PortraitPlan::PixelLayout::kRgba16F;
                default:
                    return std::nullopt;
            }
        }

        [[nodiscard]] RE::NiCamera* SceneCamera() {
            // The camera the RENDERER used, not whatever hangs under
            // PlayerCamera's root: the r-one field round projected the head
            // to y=3949 on a 1440 screen off the first cameraRoot child.
            if (auto* const cam = RE::Main::WorldRootCamera()) {
                return cam;
            }
            auto* const pcam = RE::PlayerCamera::GetSingleton();
            auto* const root = pcam ? pcam->cameraRoot.get() : nullptr;
            if (!root) {
                return nullptr;
            }
            for (const auto& child : root->GetChildren()) {
                if (auto* const cam =
                        netimmerse_cast<RE::NiCamera*>(child.get())) {
                    return cam;
                }
            }
            return nullptr;
        }

        // Project the player's head into pixels. False when there is no
        // face node or the head sits outside the view. The world is paused
        // under the editor, which is what makes these reads safe here.
        [[nodiscard]] bool ProjectHead(std::uint32_t a_w, std::uint32_t a_h,
                                       float& a_cx, float& a_cy, float& a_rPx) {
            auto* const pc   = RE::PlayerCharacter::GetSingleton();
            auto* const face = pc ? pc->GetFaceNodeSkinned() : nullptr;
            auto* const cam  = SceneCamera();
            if (!face || !cam) {
                return false;
            }
            const auto& bound = face->worldBound;
            const auto& w2c   = cam->GetRuntimeData().worldToCam;
            const auto& port  = cam->GetRuntimeData2().port;
            float       x0, y0, z0, x1, y1, z1;
            if (!RE::NiCamera::WorldPtToScreenPt3(w2c, port, bound.center, x0,
                                                  y0, z0, 1e-5f) ||
                z0 <= 0.0f) {
                return false;
            }
            auto top = bound.center;
            top.z += bound.radius;
            if (!RE::NiCamera::WorldPtToScreenPt3(w2c, port, top, x1, y1, z1,
                                                  1e-5f)) {
                return false;
            }
            // x and y come back normalised with y measured from the BOTTOM;
            // pixels count from the top.
            a_cx  = x0 * static_cast<float>(a_w);
            a_cy  = (1.0f - y0) * static_cast<float>(a_h);
            a_rPx = std::abs(y1 - y0) * static_cast<float>(a_h);
            return true;
        }

        void QueueLoad(const std::string& a_key) {
            for (const auto& queued : g_loadQueue) {
                if (queued == a_key) {
                    return;
                }
            }
            g_loadQueue.push_back(a_key);
        }

        // Copy one source texture through a staging copy and convert the
        // crop to RGBA8. Returns the average luma of the result, or -1 on
        // any failure, so the caller can tell "worked" from "worked and is
        // a black frame".
        [[nodiscard]] float GrabCrop(ID3D11Texture2D* a_source,
                                     const PortraitPlan::CropRect& a_crop,
                                     std::vector<std::uint8_t>& a_rgba,
                                     const char* a_sourceName) {
            D3D11_TEXTURE2D_DESC desc{};
            a_source->GetDesc(&desc);
            const auto layout = LayoutFor(desc.Format);
            if (!layout || desc.SampleDesc.Count > 1) {
                if (!g_saidBadFormat.exchange(true)) {
                    spdlog::warn(
                        "Portrait: {} format {} (samples {}) is not one this "
                        "converter knows. Name the pair and the next build "
                        "learns it.",
                        a_sourceName, static_cast<int>(desc.Format),
                        desc.SampleDesc.Count);
                }
                return -1.0f;
            }
            if (a_crop.x + a_crop.size > desc.Width ||
                a_crop.y + a_crop.size > desc.Height) {
                return -1.0f;
            }
            ComPtr<ID3D11Device>        device;
            ComPtr<ID3D11DeviceContext> context;
            a_source->GetDevice(&device);
            if (!device) {
                return -1.0f;
            }
            device->GetImmediateContext(&context);
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.Usage                = D3D11_USAGE_STAGING;
            sd.BindFlags            = 0;
            sd.CPUAccessFlags       = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags            = 0;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging)) ||
                !staging) {
                return -1.0f;
            }
            context->CopyResource(staging.Get(), a_source);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0,
                                    &mapped))) {
                return -1.0f;
            }
            const auto bpp = PortraitPlan::BytesPerPixel(*layout);
            a_rgba.assign(
                static_cast<std::size_t>(a_crop.size) * a_crop.size * 4, 0);
            std::uint64_t     lumaSum = 0;
            const auto* const src = static_cast<const std::uint8_t*>(mapped.pData);
            for (std::uint32_t row = 0; row < a_crop.size; ++row) {
                const auto* srcRow = src +
                                     static_cast<std::size_t>(a_crop.y + row) *
                                         mapped.RowPitch +
                                     static_cast<std::size_t>(a_crop.x) * bpp;
                auto* dstRow = a_rgba.data() +
                               static_cast<std::size_t>(row) * a_crop.size * 4;
                for (std::uint32_t col = 0; col < a_crop.size; ++col) {
                    auto* const px = dstRow + col * 4;
                    PortraitPlan::ConvertPixel(*layout, srcRow + col * bpp, px);
                    lumaSum += px[0] + px[1] + px[2];
                }
            }
            context->Unmap(staging.Get(), 0);
            return static_cast<float>(lumaSum) /
                   (static_cast<float>(a_crop.size) * a_crop.size * 3.0f);
        }

        // The capture itself. Two candidate sources, tried in order:
        //
        // 1. The engine's own framebuffer (BSRenderManager's resourceView),
        //    the composed frame the renderer samples for its own effects.
        //    The r-one field round proved the swapchain backbuffer is BLACK
        //    at this thunk on the live rig: Community Shaders composes the
        //    final image deeper in the Present chain, so the buffer the
        //    engine's Present call carries has not been written yet. The
        //    framebuffer also predates FUCK's UI pass, so the FR window
        //    never photobombs the portrait.
        // 2. The backbuffer, for a rig whose framebuffer view is absent.
        //
        // A source that converts to near-black (luma < 2/255) is treated as
        // wrong and the other is tried; the log names who won and how
        // bright, so the next field round reads as a verdict either way.
        void RunCapture(IDXGISwapChain* a_chain, const Pending& a_p) {
            std::uint32_t screenW = 0, screenH = 0;
            ComPtr<ID3D11Texture2D> back;
            if (SUCCEEDED(a_chain->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
                D3D11_TEXTURE2D_DESC bd{};
                back->GetDesc(&bd);
                screenW = bd.Width;
                screenH = bd.Height;
            }
            ComPtr<ID3D11Texture2D> frame;
            if (auto* const rm = RE::BSRenderManager::GetSingleton()) {
                if (auto* const srv = rm->GetRuntimeData().resourceView) {
                    ComPtr<ID3D11Resource> res;
                    srv->GetResource(&res);
                    if (res) {
                        res.As(&frame);
                    }
                }
            }
            if (!screenW && frame) {
                D3D11_TEXTURE2D_DESC fd{};
                frame->GetDesc(&fd);
                screenW = fd.Width;
                screenH = fd.Height;
            }
            if (!screenW || !screenH) {
                spdlog::warn("Portrait: no capture source at all; dropped.");
                return;
            }

            float      cx = 0.0f, cy = 0.0f, rPx = 0.0f;
            const bool projected = ProjectHead(screenW, screenH, cx, cy, rPx);
            // ⚠ SANITY BEFORE TRUST: the first field round projected the
            // head to (1729,3949) r 3866px on a 2560x1440 frame. A reading
            // outside the frame's neighbourhood falls back to a centred
            // square - the editor frames the character mid-view anyway.
            const bool sane =
                projected && rPx > 0.0f &&
                rPx < static_cast<float>(screenH) &&
                cx > -0.25f * screenW && cx < 1.25f * screenW &&
                cy > -0.25f * screenH && cy < 1.25f * screenH;
            auto crop = sane ? PortraitPlan::HeadCrop(cx, cy, rPx, screenW,
                                                      screenH)
                             : PortraitPlan::CropRect{};
            if (!crop.Valid()) {
                const auto size = static_cast<std::uint32_t>(screenH * 0.62f);
                crop = PortraitPlan::CropRect{ (screenW - size) / 2,
                                               (screenH - size) / 2, size };
                spdlog::info("Portrait: projection {} (head at {:.0f},{:.0f} "
                             "r {:.0f}px); centre crop instead.",
                             projected ? "insane" : "failed", cx, cy, rPx);
            }

            std::vector<std::uint8_t> rgba;
            const char*               source = "framebuffer";
            float                     luma   = -1.0f;
            if (frame) {
                luma = GrabCrop(frame.Get(), crop, rgba, "framebuffer");
            }
            if (luma < 2.0f && back) {
                const auto backLuma =
                    GrabCrop(back.Get(), crop, rgba, "backbuffer");
                if (backLuma > luma) {
                    source = "backbuffer";
                    luma   = backLuma;
                }
            }
            if (luma < 0.0f) {
                spdlog::warn("Portrait: every source failed; dropped.");
                return;
            }

            const auto dest = PathFor(a_p.kind, a_p.name);
            const auto key  = MapKey(a_p.kind, a_p.name);
            spdlog::info("Portrait: captured {}x{} at ({},{}) for '{}' from "
                         "the {} (luma {:.1f}, head at {:.0f},{:.0f} r "
                         "{:.0f}px).",
                         crop.size, crop.size, crop.x, crop.y, a_p.name,
                         source, luma, cx, cy, rPx);
            g_encoding.store(true, std::memory_order_release);
            try {
                std::thread([rgba = std::move(rgba), size = crop.size, dest,
                             key]() {
                    std::error_code ec;
                    std::filesystem::create_directories(dest.parent_path(), ec);
                    if (PreviewPng::WriteRgba(dest, rgba, size, size)) {
                        std::scoped_lock l(g_doneLock);
                        g_doneKey = key;
                    } else {
                        spdlog::warn("Portrait: PNG write failed for '{}'.",
                                     dest.string());
                    }
                    g_encoding.store(false, std::memory_order_release);
                }).detach();
            } catch (const std::exception& e) {
                g_encoding.store(false, std::memory_order_release);
                spdlog::error("Portrait: encoder thread failed ({}).", e.what());
            }
        }

    }  // namespace

    void RequestCapture(Kind a_kind, const std::string& a_name,
                        std::uint32_t a_delayPresents) {
        if (a_name.empty()) {
            return;
        }
        if (!EditorWindow::IsOpen()) {
            spdlog::info("Portrait: capture for '{}' skipped, the editor is "
                         "closed (the world must be paused and framed).",
                         a_name);
            return;
        }
        {
            std::scoped_lock l(g_lock);
            if (g_pending || g_encoding.load(std::memory_order_acquire)) {
                return;
            }
            g_pending = Pending{ a_kind, a_name, a_delayPresents };
        }
        // The pump rides DyeTexture's Present thunk; a session where no dye
        // ever armed it still gets portraits.
        DyeTexture::EnsurePresent();
    }

    bool CapturePending() {
        std::scoped_lock l(g_lock);
        return g_pending.has_value() ||
               g_encoding.load(std::memory_order_acquire);
    }

    void NoteOwed(Kind a_kind, const std::string& a_name) {
        std::scoped_lock l(g_lock);
        g_owed = { a_kind, a_name };
    }

    std::optional<std::pair<Kind, std::string>> TakeOwed() {
        std::scoped_lock l(g_lock);
        auto out = std::move(g_owed);
        g_owed.reset();
        return out;
    }

    ImTextureID TextureFor(Kind a_kind, const std::string& a_name) {
        const auto key = MapKey(a_kind, a_name);
        if (const auto it = g_images.find(key); it != g_images.end()) {
            return it->second.IsLoaded() ? it->second.GetID()
                                         : static_cast<ImTextureID>(0);
        }
        // While a capture or encode is in flight, do not even probe the
        // disk: the first field round queued a load against the half-written
        // PNG the worker was still encoding, and the load failed loudly for
        // a file that was fine a present later (the done-key reload is the
        // one that owns that moment).
        if (CapturePending()) {
            return static_cast<ImTextureID>(0);
        }
        std::error_code ec;
        if (!std::filesystem::exists(PathFor(a_kind, a_name), ec)) {
            return static_cast<ImTextureID>(0);
        }
        // Queued rather than loaded here: FUCK image traffic inside the UI
        // pass is the 2026-08-09 vanish scar. The pump loads it a present
        // later. The placeholder entry stops this from re-queueing every
        // frame; IsLoaded stays false until the pump replaces it.
        g_images.emplace(key, FUCK::Image{});
        QueueLoad(key);
        DyeTexture::EnsurePresent();
        return static_cast<ImTextureID>(0);
    }

    void Pump(void* a_swapChain) {
        // Freed images from LAST present are safe to destroy now.
        g_release.clear();

        // A finished encode reloads its image.
        {
            std::string done;
            {
                std::scoped_lock l(g_doneLock);
                done.swap(g_doneKey);
            }
            if (!done.empty()) {
                if (const auto it = g_images.find(done); it != g_images.end()) {
                    g_release.push_back(std::move(it->second));
                    g_images.erase(it);
                }
                QueueLoad(done);
            }
        }

        // Queued loads: the key encodes kind and sanitized name, and the
        // file sits in the kind's folder.
        if (!g_loadQueue.empty()) {
            const auto key = g_loadQueue.back();
            g_loadQueue.pop_back();
            const auto kind = key.starts_with("p:") ? Kind::kPreset : Kind::kLook;
            const auto file = DirFor(kind) / (key.substr(2) + ".png");
            FUCK::Image image(file.string().c_str());
            if (image.IsLoaded()) {
                if (const auto it = g_images.find(key); it != g_images.end()) {
                    g_release.push_back(std::move(it->second));
                    g_images.erase(it);
                }
                g_images.emplace(key, std::move(image));
            } else {
                spdlog::warn("Portrait: '{}' did not load as a FUCK image.",
                             file.string());
            }
        }

        // The capture, delayed the asked number of presents.
        Pending run;
        {
            std::scoped_lock l(g_lock);
            if (!g_pending) {
                return;
            }
            if (g_pending->delay > 0) {
                --g_pending->delay;
                return;
            }
            run = *g_pending;
            g_pending.reset();
        }
        RunCapture(static_cast<IDXGISwapChain*>(a_swapChain), run);
    }

}  // namespace OS::Portrait
