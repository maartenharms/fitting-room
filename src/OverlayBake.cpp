#include "OverlayBake.h"

#include "WorkerGuard.h"  // no worker body may reach terminate
#include "Settings.h"     // iOverlayBakeCapPx, iOverlayBakeCacheMiB
#include "TextureLoad.h"  // the engine's own loader by path, shared with the cards

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OS::OverlayBake {

    namespace {

        using Microsoft::WRL::ComPtr;
        using RendererData = RE::NiTexture::RendererData;

        // ---- limits ---------------------------------------------------------

        // The transient's longest side. A drag is judged at screen scale on a
        // character a few hundred pixels tall; 1024 is sharper than that and
        // sixteen times cheaper than a 4096 dispatch on the integrated graphics
        // that can least afford a hitch per frame.
        constexpr std::uint32_t kTransientCapPx = 1024;

        // How many presents a source may take to come up with a live renderer
        // texture before its request is dropped with a log line. Loading is
        // synchronous in every measurement so far; this is the "not now" DyeTexture
        // gives an unloaded source, bounded.
        constexpr int kSourceRetries = 300;

        // The probe waits this many presents so skee's deferred SetNodeProperties
        // and the engine's texture load have both landed. About two seconds.
        constexpr int kProbePresents = 120;

        // ---- the shaders ------------------------------------------------------
        //
        // ⚠⚠ THE MAPPING IS OverlayTransform::ToSource WRITTEN IN HLSL, and the
        // tests on that header are what hold the two together. Change one and
        // change the other and bump kBakeVersion.
        constexpr char kBakeShader[] = R"(
Texture2D<float4> Src : register(t0);
SamplerState      Samp : register(s0);
RWTexture2D<float4> Dst : register(u0);

cbuffer P : register(b0)
{
    float2 gOffset;
    float  gScale;
    float  gRotRad;
    float2 gPivot;
    float2 gDstSize;
    float  gLod;
    float  gSrgb;    // 1 when the source view decodes sRGB, so the write re-encodes
    float2 gPad;
};

float3 LinearToSrgb(float3 c)
{
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055;
    return lerp(lo, hi, step(0.0031308, c));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)gDstSize.x || id.y >= (uint)gDstSize.y) {
        return;
    }
    float2 uv = (float2(id.xy) + 0.5) / gDstSize;
    float  c  = cos(-gRotRad);
    float  s  = sin(-gRotRad);
    float2 q  = uv - gPivot - gOffset;
    float2 r  = float2(c * q.x - s * q.y, s * q.x + c * q.y);
    float2 suv = r / max(gScale, 1e-4) + gPivot;
    float4 col = 0;
    if (all(suv >= 0.0) && all(suv <= 1.0)) {
        col = Src.SampleLevel(Samp, suv, gLod);
        if (gSrgb > 0.5) {
            col.rgb = LinearToSrgb(col.rgb);
        }
    }
    Dst[id.xy] = col;
}
)";

        // The alpha centroid of a source, on a 64x64 lattice at the level where
        // the source is about that size. One group, 64 threads, each summing an
        // 8x8 patch, reduced through groupshared into three floats.
        constexpr char kCentroidShader[] = R"(
Texture2D<float4> Src : register(t0);
SamplerState      Samp : register(s0);
RWStructuredBuffer<float> Out : register(u0);

cbuffer P : register(b0)
{
    float2 gOffset;
    float  gScale;
    float  gRotRad;
    float2 gPivot;
    float2 gDstSize;
    float  gLod;
    float  gSrgb;
    float2 gPad;
};

groupshared float3 gsSum[64];

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    float3 acc = 0;
    for (uint y = 0; y < 8; ++y) {
        for (uint x = 0; x < 8; ++x) {
            float2 uv = (float2(gid.x * 8 + x, gid.y * 8 + y) + 0.5) / 64.0;
            float  a  = Src.SampleLevel(Samp, uv, gLod).a;
            acc += float3(uv * a, a);
        }
    }
    gsSum[gi] = acc;
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        float3 t = 0;
        for (uint i = 0; i < 64; ++i) {
            t += gsSum[i];
        }
        Out[0] = t.x;
        Out[1] = t.y;
        Out[2] = t.z;
    }
}
)";

        struct Params {
            float offset[2];
            float scale;
            float rotRad;
            float pivot[2];
            float dstSize[2];
            float lod;
            float srgb;
            float pad[2];
        };
        static_assert(sizeof(Params) == 48, "the constant buffer is three registers");

        struct Gpu {
            bool                        tried{ false };
            bool                        ok{ false };
            ComPtr<ID3D11ComputeShader> bake;
            ComPtr<ID3D11ComputeShader> centroid;
            ComPtr<ID3D11Buffer>        cb;
            ComPtr<ID3D11SamplerState>  sampler;
            ComPtr<ID3D11Buffer>        centroidOut;
            ComPtr<ID3D11UnorderedAccessView> centroidUav;
            ComPtr<ID3D11Buffer>        centroidStaging;
        };
        Gpu g_gpu;  // render thread only

        ComPtr<ID3D11ComputeShader> CompileOne(ID3D11Device* a_device, const char* a_src,
                                               const char* a_name) {
            ComPtr<ID3D11ComputeShader> out;
            ID3DBlob*                   code{ nullptr };
            ID3DBlob*                   errors{ nullptr };
            const HRESULT hr = ::D3DCompile(a_src, std::strlen(a_src), a_name, nullptr, nullptr,
                                            "main", "cs_5_0", 0, 0, &code, &errors);
            if (FAILED(hr) || !code) {
                spdlog::error("OverlayBake: D3DCompile('{}') failed hr=0x{:08X} msg='{}'", a_name,
                              static_cast<unsigned>(hr),
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                if (errors) { errors->Release(); }
                if (code) { code->Release(); }
                return out;
            }
            if (errors) { errors->Release(); }
            a_device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                          nullptr, &out);
            code->Release();
            return out;
        }

        bool EnsureGpu(ID3D11Device* a_device) {
            if (g_gpu.tried) {
                return g_gpu.ok;
            }
            g_gpu.tried = true;
            g_gpu.bake     = CompileOne(a_device, kBakeShader, "OverlayBake");
            g_gpu.centroid = CompileOne(a_device, kCentroidShader, "OverlayCentroid");
            if (!g_gpu.bake || !g_gpu.centroid) {
                return false;
            }
            D3D11_BUFFER_DESC cbd{};
            cbd.ByteWidth      = sizeof(Params);
            cbd.Usage          = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(a_device->CreateBuffer(&cbd, nullptr, &g_gpu.cb))) {
                return false;
            }
            D3D11_SAMPLER_DESC sd{};
            sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD         = D3D11_FLOAT32_MAX;
            sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
            if (FAILED(a_device->CreateSamplerState(&sd, &g_gpu.sampler))) {
                return false;
            }
            D3D11_BUFFER_DESC ob{};
            ob.ByteWidth           = 3 * sizeof(float);
            ob.Usage               = D3D11_USAGE_DEFAULT;
            ob.BindFlags           = D3D11_BIND_UNORDERED_ACCESS;
            ob.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            ob.StructureByteStride = sizeof(float);
            if (FAILED(a_device->CreateBuffer(&ob, nullptr, &g_gpu.centroidOut))) {
                return false;
            }
            D3D11_UNORDERED_ACCESS_VIEW_DESC uvd{};
            uvd.Format              = DXGI_FORMAT_UNKNOWN;
            uvd.ViewDimension       = D3D11_UAV_DIMENSION_BUFFER;
            uvd.Buffer.FirstElement = 0;
            uvd.Buffer.NumElements  = 3;
            if (FAILED(a_device->CreateUnorderedAccessView(g_gpu.centroidOut.Get(), &uvd,
                                                           &g_gpu.centroidUav))) {
                return false;
            }
            D3D11_BUFFER_DESC sb{};
            sb.ByteWidth      = 3 * sizeof(float);
            sb.Usage          = D3D11_USAGE_STAGING;
            sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(a_device->CreateBuffer(&sb, nullptr, &g_gpu.centroidStaging))) {
                return false;
            }
            g_gpu.ok = true;
            spdlog::info("OverlayBake: shaders compiled.");
            return true;
        }

        bool IsSrgb(DXGI_FORMAT a_fmt) {
            switch (a_fmt) {
                case DXGI_FORMAT_BC1_UNORM_SRGB:
                case DXGI_FORMAT_BC2_UNORM_SRGB:
                case DXGI_FORMAT_BC3_UNORM_SRGB:
                case DXGI_FORMAT_BC7_UNORM_SRGB:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                    return true;
                default:
                    return false;
            }
        }

        // ---- sources ----------------------------------------------------------

        struct Source {
            RE::NiPointer<RE::NiSourceTexture> tex;
            ComPtr<ID3D11ShaderResourceView>   srv;  // our own view, whole chain
            std::uint32_t                      width{ 0 };
            std::uint32_t                      height{ 0 };
            std::uint32_t                      mips{ 1 };
            bool                               srgb{ false };
            bool                               ready{ false };
            bool                               pivotDone{ false };
            float                              pivotU{ 0.5f };
            float                              pivotV{ 0.5f };
            int                                retries{ 0 };
            bool                               refused{ false };
        };
        std::unordered_map<std::string, Source> g_sources;  // render thread only

        // Loads or re-checks a source; true when it can be sampled this present.
        bool ReadySource(ID3D11Device* a_device, const std::string& a_path, Source*& a_out) {
            auto& s = g_sources[a_path];
            a_out   = &s;
            if (s.ready) {
                return true;
            }
            if (s.refused) {
                return false;
            }
            if (!s.tex) {
                s.tex = TextureLoad::LoadRelative(a_path);
                if (!s.tex) {
                    s.refused = true;
                    spdlog::warn("OverlayBake: '{}' could not be loaded, so it cannot be moved.",
                                 a_path);
                    return false;
                }
            }
            auto* const rd = reinterpret_cast<RendererData*>(s.tex->rendererTexture);
            if (!rd || !rd->texture || !rd->resourceView) {
                if (++s.retries > kSourceRetries) {
                    s.refused = true;
                    spdlog::warn("OverlayBake: '{}' never came up with a renderer texture; "
                                 "refused.", a_path);
                }
                return false;
            }
            // The layout control DyeTexture keeps: the view must name the
            // texture, or two wrong offsets are about to be dispatched at.
            ID3D11Resource* res{ nullptr };
            rd->resourceView->GetResource(&res);
            const bool identity = res == static_cast<ID3D11Resource*>(rd->texture);
            if (res) { res->Release(); }
            if (!identity) {
                s.refused = true;
                spdlog::error("OverlayBake: LAYOUT CONTROL FAILED on '{}'; refused.", a_path);
                return false;
            }
            D3D11_TEXTURE2D_DESC sd{};
            rd->texture->GetDesc(&sd);
            if (sd.ArraySize != 1 || (sd.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0) {
                s.refused = true;
                spdlog::warn("OverlayBake: '{}' is not a plain 2D texture; refused.", a_path);
                return false;
            }
            // ⚠ A PLACEHOLDER-SIZED SOURCE IS REFUSED, NOT BAKED. A missing file
            // resolves to the engine's stand-in with a clean log; baking it would
            // write a file of nothing and hand it to skee as art. Real overlay art
            // is never this small.
            if (sd.Width < 16 || sd.Height < 16) {
                s.refused = true;
                spdlog::warn("OverlayBake: '{}' resolved to a {}x{} texture, which is the "
                             "engine's placeholder for a missing file; refused.",
                             a_path, sd.Width, sd.Height);
                return false;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format                    = sd.Format;
            vd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MostDetailedMip = 0;
            vd.Texture2D.MipLevels       = static_cast<UINT>(-1);
            if (FAILED(a_device->CreateShaderResourceView(rd->texture, &vd, &s.srv)) || !s.srv) {
                s.refused = true;
                spdlog::error("OverlayBake: could not view '{}' with its whole chain; refused.",
                              a_path);
                return false;
            }
            s.width  = sd.Width;
            s.height = sd.Height;
            s.mips   = sd.MipLevels;
            s.srgb   = IsSrgb(sd.Format);
            s.ready  = true;
            spdlog::info("OverlayBake: source '{}' {}x{} mips={} srgb={} fmt={}.", a_path,
                         s.width, s.height, s.mips, s.srgb, static_cast<int>(sd.Format));
            return true;
        }

        void UploadParams(ID3D11DeviceContext* a_ctx, const Params& a_p) {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(a_ctx->Map(g_gpu.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) &&
                m.pData) {
                std::memcpy(m.pData, &a_p, sizeof a_p);
                a_ctx->Unmap(g_gpu.cb.Get(), 0);
            }
        }

        // Measures the alpha centroid once per source. Maps a 12-byte staging
        // buffer, which stalls on the GPU for the one dispatch just issued: once
        // per source, never per frame.
        void MeasurePivot(ID3D11DeviceContext* a_ctx, Source& a_s, const std::string& a_path) {
            if (a_s.pivotDone) {
                return;
            }
            a_s.pivotDone = true;
            const auto  longest = std::max(a_s.width, a_s.height);
            float       lod     = 0.0f;
            for (auto d = longest; d > 64; d >>= 1) {
                lod += 1.0f;
            }
            Params p{};
            p.lod = lod;
            UploadParams(a_ctx, p);
            ID3D11ShaderResourceView*  srv = a_s.srv.Get();
            ID3D11UnorderedAccessView* uav = g_gpu.centroidUav.Get();
            ID3D11Buffer*              cb  = g_gpu.cb.Get();
            ID3D11SamplerState*        smp = g_gpu.sampler.Get();
            a_ctx->CSSetShader(g_gpu.centroid.Get(), nullptr, 0);
            a_ctx->CSSetShaderResources(0, 1, &srv);
            a_ctx->CSSetSamplers(0, 1, &smp);
            a_ctx->CSSetConstantBuffers(0, 1, &cb);
            a_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            a_ctx->Dispatch(1, 1, 1);
            ID3D11UnorderedAccessView* nullUav[1]{ nullptr };
            ID3D11ShaderResourceView*  nullSrv[1]{ nullptr };
            a_ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            a_ctx->CSSetShaderResources(0, 1, nullSrv);
            a_ctx->CSSetShader(nullptr, nullptr, 0);
            a_ctx->CopyResource(g_gpu.centroidStaging.Get(), g_gpu.centroidOut.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(a_ctx->Map(g_gpu.centroidStaging.Get(), 0, D3D11_MAP_READ, 0, &m)) &&
                m.pData) {
                const auto* f = static_cast<const float*>(m.pData);
                if (f[2] > 1e-3f) {
                    a_s.pivotU = std::clamp(f[0] / f[2], 0.0f, 1.0f);
                    a_s.pivotV = std::clamp(f[1] / f[2], 0.0f, 1.0f);
                }
                a_ctx->Unmap(g_gpu.centroidStaging.Get(), 0);
            }
            spdlog::info("OverlayBake: '{}' pivot ({:.3f}, {:.3f}) at lod {:.0f}.", a_path,
                         a_s.pivotU, a_s.pivotV, lod);
        }

        // ---- one dispatch into a destination ---------------------------------

        struct Target {
            ComPtr<ID3D11Texture2D>           texture;
            ComPtr<ID3D11ShaderResourceView>  srv;  // UNORM or _SRGB, whole chain
            ComPtr<ID3D11UnorderedAccessView> uav;  // UNORM, mip 0
            std::uint32_t                     width{ 0 };
            std::uint32_t                     height{ 0 };
            std::uint32_t                     mips{ 0 };
            bool                              srgb{ false };
        };

        bool MakeTarget(ID3D11Device* a_device, std::uint32_t a_w, std::uint32_t a_h,
                        bool a_srgb, Target& a_out) {
            D3D11_TEXTURE2D_DESC dd{};
            dd.Width            = a_w;
            dd.Height           = a_h;
            dd.MipLevels        = 0;  // the full chain; the shader writes 0, GenerateMips the rest
            dd.ArraySize        = 1;
            dd.Format           = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            dd.SampleDesc.Count = 1;
            dd.Usage            = D3D11_USAGE_DEFAULT;
            dd.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS |
                           D3D11_BIND_RENDER_TARGET;
            dd.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            if (FAILED(a_device->CreateTexture2D(&dd, nullptr, &a_out.texture)) || !a_out.texture) {
                spdlog::error("OverlayBake: could not allocate {}x{}.", a_w, a_h);
                return false;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
            svd.Format = a_srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            svd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            svd.Texture2D.MostDetailedMip = 0;
            svd.Texture2D.MipLevels       = static_cast<UINT>(-1);
            D3D11_UNORDERED_ACCESS_VIEW_DESC uvd{};
            uvd.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
            uvd.ViewDimension      = D3D11_UAV_DIMENSION_TEXTURE2D;
            uvd.Texture2D.MipSlice = 0;
            if (FAILED(a_device->CreateShaderResourceView(a_out.texture.Get(), &svd, &a_out.srv)) ||
                FAILED(a_device->CreateUnorderedAccessView(a_out.texture.Get(), &uvd, &a_out.uav))) {
                spdlog::error("OverlayBake: view creation failed for {}x{}.", a_w, a_h);
                a_out = Target{};
                return false;
            }
            D3D11_TEXTURE2D_DESC made{};
            a_out.texture->GetDesc(&made);
            a_out.width  = made.Width;
            a_out.height = made.Height;
            a_out.mips   = made.MipLevels;
            a_out.srgb   = a_srgb;
            return true;
        }

        void Dispatch(ID3D11DeviceContext* a_ctx, Source& a_src, const Transform& a_t,
                      std::uint32_t a_halvings, Target& a_dst) {
            const auto q = OverlayTransform::Quantise(a_t);
            Params     p{};
            p.offset[0]  = q.offsetX;
            p.offset[1]  = q.offsetY;
            p.scale      = q.scale;
            p.rotRad     = q.rotationDeg * OverlayTransform::kPi / 180.0f;
            p.pivot[0]   = a_src.pivotU;
            p.pivot[1]   = a_src.pivotV;
            p.dstSize[0] = static_cast<float>(a_dst.width);
            p.dstSize[1] = static_cast<float>(a_dst.height);
            p.lod        = OverlayTransform::SampleLod(a_halvings, q.scale);
            p.srgb       = a_src.srgb ? 1.0f : 0.0f;
            UploadParams(a_ctx, p);

            ID3D11ShaderResourceView*  srv = a_src.srv.Get();
            ID3D11UnorderedAccessView* uav = a_dst.uav.Get();
            ID3D11Buffer*              cb  = g_gpu.cb.Get();
            ID3D11SamplerState*        smp = g_gpu.sampler.Get();
            a_ctx->CSSetShader(g_gpu.bake.Get(), nullptr, 0);
            a_ctx->CSSetShaderResources(0, 1, &srv);
            a_ctx->CSSetSamplers(0, 1, &smp);
            a_ctx->CSSetConstantBuffers(0, 1, &cb);
            a_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            a_ctx->Dispatch((a_dst.width + 7) / 8, (a_dst.height + 7) / 8, 1);
            ID3D11UnorderedAccessView* nullUav[1]{ nullptr };
            ID3D11ShaderResourceView*  nullSrv[1]{ nullptr };
            ID3D11Buffer*              nullCb[1]{ nullptr };
            a_ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            a_ctx->CSSetShaderResources(0, 1, nullSrv);
            a_ctx->CSSetConstantBuffers(0, 1, nullCb);
            a_ctx->CSSetShader(nullptr, nullptr, 0);
            a_ctx->GenerateMips(a_dst.srv.Get());
        }

        // ---- the transient ---------------------------------------------------

        // Forges an NiSourceTexture over our RendererData, DyeTexture's way: a
        // byte copy of a live one of the same class, with only the fields whose
        // meaning is established overridden. See DyeTexture::Forge for why each
        // line is what it is; this is that function for a texture this module
        // keeps for the process.
        RE::NiSourceTexture* Forge(const RE::NiSourceTexture* a_model, RendererData* a_rd,
                                   const std::string& a_name) {
            void* const mem = RE::malloc(sizeof(RE::NiSourceTexture));
            if (!mem) {
                return nullptr;
            }
            std::memcpy(mem, static_cast<const void*>(a_model), sizeof(RE::NiSourceTexture));
            auto* const t     = static_cast<RE::NiSourceTexture*>(mem);
            t->_refCount      = 0;
            t->prev           = nullptr;
            t->next           = nullptr;
            t->unk40          = nullptr;
            t->rendererTexture = reinterpret_cast<RE::BSGraphics::Texture*>(a_rd);
            std::memset(&t->name, 0, sizeof(t->name));
            t->name = a_name.c_str();
            return t;
        }

        struct Transient {
            Target                             target;
            RE::NiPointer<RE::NiSourceTexture> tex;  // held for the process
            std::string                        source;
        };
        // Keyed by "<formID>|<node>". Render thread only. Never erased.
        std::unordered_map<std::string, Transient> g_transients;

        // The raw handle, never a dereference: this runs on the render thread
        // and needs an identity, not the actor.
        std::string LayerKey(RE::ActorHandle a_actor, const std::string& a_node) {
            return std::to_string(a_actor.native_handle()) + "|" + a_node;
        }

        // Puts a_tex on the [Ovl] node's material, on the game thread. The
        // exact write skee's NIOVTaskUpdateTexture makes for key 9, on a
        // material that is this clone's alone (see the header).
        void AssignOnGameThread(RE::ActorHandle a_actor, const std::string& a_node,
                                RE::NiPointer<RE::NiSourceTexture> a_tex) {
            auto* task = SKSE::GetTaskInterface();
            if (!task) {
                return;
            }
            task->AddTask([a_actor, a_node, a_tex] {
                const auto ptr   = a_actor.get();
                auto*      actor = ptr ? ptr.get() : nullptr;
                if (!actor) {
                    return;
                }
                auto* const root = actor->Get3D(false);
                if (!root) {
                    return;
                }
                auto* const obj  = root->GetObjectByName(RE::BSFixedString{ a_node });
                auto* const geom = obj ? obj->AsGeometry() : nullptr;
                if (!geom) {
                    return;
                }
                auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                    geom->GetGeometryRuntimeData()
                        .properties[RE::BSGeometry::States::kEffect]
                        .get());
                if (!prop || !prop->material) {
                    return;
                }
                auto* const mat = static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);
                mat->diffuseTexture = a_tex;
                prop->DoClearRenderPasses();
            });
        }

        // ---- requests ---------------------------------------------------------

        std::mutex g_lock;

        struct PreviewReq {
            RE::ActorHandle actor;
            std::string     node;
            std::string     source;
            Transform       t;
        };
        std::unordered_map<std::string, PreviewReq> g_previewWanted;  // latest per layer

        struct CommitReq {
            std::string                            source;
            Transform                              t;
            std::string                            stem;
            std::vector<std::function<void(bool)>> then;
        };
        std::vector<CommitReq>          g_commitQueue;
        std::unordered_set<std::string> g_commitInFlight;  // stems on the worker

        struct Probe {
            RE::ActorHandle actor;
            std::string     node;
            std::string     expected;
            int             presentsLeft{ kProbePresents };
        };
        std::vector<Probe> g_probes;

        Stats g_stats;

        // ---- disk -----------------------------------------------------------

        std::filesystem::path BakedRoot() {
            return std::filesystem::path{ "Data" } / "textures" / "FittingRoom" / "baked";
        }

        std::filesystem::path StemPath(const std::string& a_stem, const char* a_ext) {
            return BakedRoot() / (a_stem + a_ext);
        }

        void RunOnGameThread(std::function<void()> a_fn) {
            auto* task = SKSE::GetTaskInterface();
            if (!task) {
                return;
            }
            task->AddTask([fn = std::move(a_fn)] { fn(); });
        }

        void Finish(std::vector<std::function<void(bool)>> a_then, bool a_ok) {
            for (auto& fn : a_then) {
                RunOnGameThread([fn = std::move(fn), a_ok] { fn(a_ok); });
            }
        }

        // The whole chain of a finished target, tightly packed, on the CPU.
        bool ReadBack(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx, const Target& a_t,
                      std::vector<std::uint8_t>& a_out) {
            D3D11_TEXTURE2D_DESC sd{};
            a_t.texture->GetDesc(&sd);
            sd.Usage          = D3D11_USAGE_STAGING;
            sd.BindFlags      = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags      = 0;
            ComPtr<ID3D11Texture2D> staging;
            if (FAILED(a_device->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
                spdlog::error("OverlayBake: could not allocate the readback for {}x{}.",
                              a_t.width, a_t.height);
                return false;
            }
            a_ctx->CopyResource(staging.Get(), a_t.texture.Get());
            const auto total = OverlayTransform::ChainBytes(a_t.width, a_t.height, a_t.mips);
            a_out.resize(static_cast<std::size_t>(total));
            std::size_t at = 0;
            for (std::uint32_t level = 0; level < a_t.mips; ++level) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(a_ctx->Map(staging.Get(), level, D3D11_MAP_READ, 0, &m)) || !m.pData) {
                    spdlog::error("OverlayBake: readback map failed at level {}.", level);
                    return false;
                }
                const auto w    = OverlayTransform::LevelDim(a_t.width, level);
                const auto h    = OverlayTransform::LevelDim(a_t.height, level);
                const auto row  = static_cast<std::size_t>(w) * 4u;
                const auto* src = static_cast<const std::uint8_t*>(m.pData);
                for (std::uint32_t y = 0; y < h; ++y) {
                    std::memcpy(a_out.data() + at, src + static_cast<std::size_t>(y) * m.RowPitch,
                                row);
                    at += row;
                }
                a_ctx->Unmap(staging.Get(), level);
            }
            return at == a_out.size();
        }

        // Keeps the folder within iOverlayBakeCacheMiB, oldest first, never
        // the file just written. Worker thread.
        void Evict(const std::string& a_keepStem) {
            const auto budgetMiB = Settings::GetSingleton().overlayBakeCacheMiB;
            if (budgetMiB == 0) {
                return;
            }
            std::error_code                          ec;
            std::vector<OverlayTransform::CacheFile> files;
            for (const auto& e : std::filesystem::directory_iterator(BakedRoot(), ec)) {
                std::error_code fe;
                if (!e.is_regular_file(fe)) {
                    continue;
                }
                const auto p = e.path();
                if (p.extension() != ".dds") {
                    continue;
                }
                OverlayTransform::CacheFile f;
                f.stem  = p.stem().string();
                f.bytes = static_cast<std::uint64_t>(e.file_size(fe));
                const auto t = e.last_write_time(fe);
                f.mtime = static_cast<std::int64_t>(t.time_since_epoch().count());
                files.push_back(std::move(f));
            }
            const auto plan = OverlayTransform::EvictionPlan(
                files, static_cast<std::uint64_t>(budgetMiB) * 1024ull * 1024ull, a_keepStem);
            for (const auto& stem : plan) {
                std::error_code re;
                std::filesystem::remove(StemPath(stem, ".dds"), re);
                std::filesystem::remove(StemPath(stem, ".txt"), re);
                spdlog::info("OverlayBake: evicted baked overlay '{}' (folder over {} MiB).",
                             stem, budgetMiB);
            }
            if (!plan.empty()) {
                std::scoped_lock lock{ g_lock };
                g_stats.evicted += plan.size();
            }
        }

        // Writes the DDS and its sidecar straight to the destination, then
        // finishes the waiters. Worker thread.
        void WriteOnWorker(CommitReq a_req, std::vector<std::uint8_t> a_bytes, std::uint32_t a_w,
                           std::uint32_t a_h, std::uint32_t a_mips, bool a_srgb) {
            const auto t0 = std::chrono::steady_clock::now();
            std::error_code ec;
            std::filesystem::create_directories(BakedRoot(), ec);
            const auto ddsPath = StemPath(a_req.stem, ".dds");
            const auto txtPath = StemPath(a_req.stem, ".txt");
            bool       ok      = false;
            {
                std::ofstream out(ddsPath, std::ios::binary | std::ios::trunc);
                if (out) {
                    const auto header = OverlayTransform::DdsHeader(a_w, a_h, a_mips, a_srgb);
                    out.write(reinterpret_cast<const char*>(header.data()),
                              static_cast<std::streamsize>(header.size()));
                    out.write(reinterpret_cast<const char*>(a_bytes.data()),
                              static_cast<std::streamsize>(a_bytes.size()));
                    ok = static_cast<bool>(out);
                }
            }
            if (ok) {
                std::ofstream side(txtPath, std::ios::binary | std::ios::trunc);
                if (side) {
                    const auto text = OverlayTransform::SidecarText(a_req.source, a_req.t);
                    side.write(text.data(), static_cast<std::streamsize>(text.size()));
                    ok = static_cast<bool>(side);
                } else {
                    ok = false;
                }
            }
            const auto ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            if (ok) {
                spdlog::info("OverlayBake: wrote '{}' {}x{} mips={} srgb={} {:.1f} MiB in {:.0f} ms "
                             "from '{}' offset=({:.3f},{:.3f}) scale={:.3f} rot={:.1f}.",
                             ddsPath.string(), a_w, a_h, a_mips, a_srgb,
                             static_cast<double>(a_bytes.size()) / (1024.0 * 1024.0), ms,
                             a_req.source, a_req.t.offsetX, a_req.t.offsetY, a_req.t.scale,
                             a_req.t.rotationDeg);
                Evict(a_req.stem);
            } else {
                spdlog::error("OverlayBake: could not write '{}' ({}); the layer keeps its "
                              "source.", ddsPath.string(), ec.message());
                std::error_code re;
                std::filesystem::remove(ddsPath, re);
                std::filesystem::remove(txtPath, re);
            }
            {
                std::scoped_lock lock{ g_lock };
                g_commitInFlight.erase(a_req.stem);
                if (ok) { ++g_stats.commits; } else { ++g_stats.failed; }
            }
            Finish(std::move(a_req.then), ok);
        }

        // ---- the probe --------------------------------------------------------

        void RunProbe(const Probe& a_p) {
            RunOnGameThread([p = a_p] {
                const auto ptr   = p.actor.get();
                auto*      actor = ptr ? ptr.get() : nullptr;
                auto* const root = actor ? actor->Get3D(false) : nullptr;
                auto* const obj  = root ? root->GetObjectByName(RE::BSFixedString{ p.node }) : nullptr;
                auto* const geom = obj ? obj->AsGeometry() : nullptr;
                auto* const prop = geom ? netimmerse_cast<RE::BSLightingShaderProperty*>(
                                              geom->GetGeometryRuntimeData()
                                                  .properties[RE::BSGeometry::States::kEffect]
                                                  .get())
                                        : nullptr;
                if (!prop || !prop->material) {
                    spdlog::warn("OverlayBake: PROBE '{}' found no lit geometry by that name.",
                                 p.node);
                    return;
                }
                auto* const mat = static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);
                auto* const tex = mat->diffuseTexture.get();
                if (!tex) {
                    spdlog::warn("OverlayBake: PROBE '{}' material has NO diffuse texture.", p.node);
                    return;
                }
                auto* const rd = reinterpret_cast<RendererData*>(tex->rendererTexture);
                D3D11_TEXTURE2D_DESC sd{};
                if (rd && rd->texture) {
                    rd->texture->GetDesc(&sd);
                }
                const std::string name = tex->name.c_str() ? tex->name.c_str() : "";
                // The engine names a texture by the path it was asked for. skee
                // asks with the override path; a match on the tail is a load of
                // OUR file, and the size says whether it was the file or the
                // placeholder that answered. The transient's own name means a
                // drag was live when this ran (OverlayTransform::ProbeVerdict).
                const auto verdict = OverlayTransform::ProbeVerdict(p.expected, name, sd.Width,
                                                                    sd.Height);
                const char* text = "";
                switch (verdict) {
                    case OverlayTransform::Probe::kMatch:
                        text = "MATCH: the runtime-written file loaded through the engine's loader";
                        break;
                    case OverlayTransform::Probe::kPlaceholder:
                        text = "NAME MATCHES BUT THE SIZE IS A PLACEHOLDER: the file was not "
                               "found by the engine";
                        break;
                    case OverlayTransform::Probe::kInProgress:
                        text = "IN PROGRESS: the drag texture is on the material, so a new drag "
                               "was live when the probe ran; the next release probes again";
                        break;
                    case OverlayTransform::Probe::kMismatch:
                        text = "MISMATCH: skee has not applied the path (or applied another)";
                        break;
                }
                spdlog::info("OverlayBake: PROBE '{}' diffuse='{}' {}x{} fmt={} expected='{}' -> {}",
                             p.node, name, sd.Width, sd.Height, static_cast<int>(sd.Format),
                             p.expected, text);
            });
        }

    }  // namespace

    // ---- public -----------------------------------------------------------

    void Preview(RE::ActorHandle a_actor, const std::string& a_node, const std::string& a_source,
                 const Transform& a_t) {
        if (a_node.empty() || a_source.empty()) {
            return;
        }
        std::scoped_lock lock{ g_lock };
        const auto key = LayerKey(a_actor, a_node);
        if (g_previewWanted.count(key) != 0) {
            ++g_stats.superseded;
        }
        g_previewWanted[key] = PreviewReq{ a_actor, a_node, a_source, a_t };
    }

    std::string DiskPath(const std::string& a_overridePath) {
        return (std::filesystem::path{ "Data" } / "textures" /
                OverlayTransform::NormalisePath(a_overridePath))
            .string();
    }

    OverlayTransform::Decoded ReadSidecar(const std::string& a_bakedPath) {
        OverlayTransform::Decoded out;
        const auto stem = OverlayTransform::StemOf(a_bakedPath);
        if (stem.empty()) {
            return out;
        }
        std::ifstream in(StemPath(stem, ".txt"), std::ios::binary);
        if (!in) {
            return out;
        }
        std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        return OverlayTransform::ParseSidecar(text);
    }

    void Ensure(const std::string& a_source, const Transform& a_t,
                std::function<void(bool)> a_then) {
        if (OverlayTransform::IsIdentity(a_t) || a_source.empty()) {
            RunOnGameThread([fn = std::move(a_then)] { fn(true); });
            return;
        }
        const auto stem = OverlayTransform::FileStem(a_source, a_t);
        std::error_code ec;
        const auto      dds = StemPath(stem, ".dds");
        // ⚠ A FILE THE WORKER IS STILL WRITING EXISTS AND IS NOT READY. The
        // bytes go straight to the destination (no temp and rename under MO2),
        // so exists() turns true at the first byte; a caller told "ready" then
        // would hand skee a half file, and the engine caches the placeholder
        // it substitutes under that path. An in-flight stem queues instead and
        // Pump finds the finished file once the worker is done.
        bool inFlight = false;
        {
            std::scoped_lock lock{ g_lock };
            inFlight = g_commitInFlight.count(stem) != 0;
        }
        if (!inFlight && std::filesystem::exists(dds, ec)) {
            // Most recently used, for the eviction order. Best effort.
            std::error_code te;
            std::filesystem::last_write_time(dds, std::filesystem::file_time_type::clock::now(), te);
            {
                std::scoped_lock lock{ g_lock };
                ++g_stats.hits;
            }
            RunOnGameThread([fn = std::move(a_then)] { fn(true); });
            return;
        }
        std::scoped_lock lock{ g_lock };
        for (auto& r : g_commitQueue) {
            if (r.stem == stem) {
                r.then.push_back(std::move(a_then));
                return;
            }
        }
        CommitReq req;
        req.source = a_source;
        req.t      = OverlayTransform::Quantise(a_t);
        req.stem   = stem;
        req.then.push_back(std::move(a_then));
        g_commitQueue.push_back(std::move(req));
    }

    void ProbeLater(RE::ActorHandle a_actor, const std::string& a_node,
                    const std::string& a_expectedPath) {
        std::scoped_lock lock{ g_lock };
        g_probes.push_back(Probe{ a_actor, a_node, a_expectedPath, kProbePresents });
    }

    Stats GetStats() {
        std::scoped_lock lock{ g_lock };
        return g_stats;
    }

    void Pump() {
        // ---- probes count presents whether or not anything else is queued --
        std::vector<Probe> due;
        {
            std::scoped_lock lock{ g_lock };
            for (auto it = g_probes.begin(); it != g_probes.end();) {
                if (--it->presentsLeft <= 0) {
                    due.push_back(*it);
                    it = g_probes.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& p : due) {
            RunProbe(p);
        }

        // ---- take one preview and one commit ----------------------------------
        PreviewReq preview;
        bool       havePreview = false;
        CommitReq  commit;
        bool       haveCommit = false;
        {
            std::scoped_lock lock{ g_lock };
            if (!g_previewWanted.empty()) {
                auto it     = g_previewWanted.begin();
                preview     = it->second;
                havePreview = true;
                g_previewWanted.erase(it);
            }
            for (auto it = g_commitQueue.begin(); it != g_commitQueue.end(); ++it) {
                if (g_commitInFlight.count(it->stem) == 0) {
                    commit     = std::move(*it);
                    haveCommit = true;
                    g_commitQueue.erase(it);
                    break;
                }
            }
        }
        if (!havePreview && !haveCommit) {
            return;
        }

        auto* const rm = RE::BSRenderManager::GetSingleton();
        if (!rm) {
            return;
        }
        auto&       rt     = rm->GetRuntimeData();
        auto* const device = rt.forwarder;
        auto* const ctx    = rt.context;
        const auto  putBack = [&] {
            std::scoped_lock lock{ g_lock };
            if (havePreview) {
                const auto key = LayerKey(preview.actor, preview.node);
                if (g_previewWanted.count(key) == 0) {
                    g_previewWanted.emplace(key, preview);
                }
            }
            if (haveCommit) {
                g_commitQueue.insert(g_commitQueue.begin(), std::move(commit));
            }
        };
        if (!device || !ctx || !EnsureGpu(device)) {
            if (!g_gpu.ok && g_gpu.tried) {
                // The shaders will never compile on this machine; say so once and
                // fail the waiters rather than holding them for ever.
                static bool s_said = false;
                if (!s_said) {
                    s_said = true;
                    spdlog::error("OverlayBake: no GPU path this session; overlay offsets are "
                                  "unavailable.");
                }
                if (haveCommit) {
                    std::scoped_lock lock{ g_lock };
                    ++g_stats.failed;
                    Finish(std::move(commit.then), false);
                }
                return;
            }
            putBack();
            return;
        }

        // ---- the preview -----------------------------------------------------
        if (havePreview) {
            Source* src = nullptr;
            if (!ReadySource(device, preview.source, src)) {
                if (!src->refused) {
                    std::scoped_lock lock{ g_lock };
                    const auto       key = LayerKey(preview.actor, preview.node);
                    if (g_previewWanted.count(key) == 0) {
                        g_previewWanted.emplace(key, preview);
                    }
                }
            } else {
                MeasurePivot(ctx, *src, preview.source);
                const auto fit = OverlayTransform::FitToCap(src->width, src->height,
                                                            kTransientCapPx);
                const auto key = LayerKey(preview.actor, preview.node);
                auto&      tr  = g_transients[key];
                const bool fresh = !tr.target.texture || tr.target.width != fit.width ||
                                   tr.target.height != fit.height || tr.target.srgb != src->srgb;
                bool ok = true;
                if (fresh) {
                    // A new object; the previous one, if any, is kept for the
                    // process (see the header) and simply stops being written.
                    Transient made;
                    ok = MakeTarget(device, fit.width, fit.height, src->srgb, made.target);
                    if (ok) {
                        auto* const rd = new RendererData(static_cast<std::uint16_t>(fit.width),
                                                          static_cast<std::uint16_t>(fit.height));
                        rd->texture = made.target.texture.Get();
                        rd->texture->AddRef();  // the RendererData's own hold
                        rd->resourceView = made.target.srv.Get();
                        rd->resourceView->AddRef();
                        rd->unk1C = static_cast<std::uint8_t>(made.target.mips);
                        rd->unk1D = static_cast<std::uint8_t>(src->srgb ? 29 : 28);
                        auto* const forged =
                            Forge(src->tex.get(), rd,
                                  std::string{ OverlayTransform::kTransientPrefix } + key);
                        if (!forged) {
                            ok = false;
                            delete rd;
                        } else {
                            made.tex = RE::NiPointer<RE::NiSourceTexture>(forged);
                            made.source = preview.source;
                            // ⚠ THE OLD TRANSIENT IS LEAKED ON PURPOSE. Its texture
                            // may still be on the material until skee repaints, and
                            // a forged NiSourceTexture must never reach refcount 0.
                            if (tr.tex) {
                                (void)new RE::NiPointer<RE::NiSourceTexture>(tr.tex);
                            }
                            tr = std::move(made);
                        }
                    }
                }
                if (ok) {
                    Dispatch(ctx, *src, preview.t, fit.halvings, tr.target);
                    tr.source = preview.source;
                    AssignOnGameThread(preview.actor, preview.node, tr.tex);
                    std::scoped_lock lock{ g_lock };
                    ++g_stats.previews;
                }
            }
        }

        // ---- the commit -----------------------------------------------------
        if (haveCommit) {
            // A request queued while the same stem was on the worker finds its
            // file here once the worker is done; a second bake of it would be a
            // second copy of the same bytes.
            {
                std::error_code ec;
                if (std::filesystem::exists(StemPath(commit.stem, ".dds"), ec)) {
                    std::scoped_lock lock{ g_lock };
                    ++g_stats.hits;
                    Finish(std::move(commit.then), true);
                    return;
                }
            }
            Source* src = nullptr;
            if (!ReadySource(device, commit.source, src)) {
                if (src->refused) {
                    std::scoped_lock lock{ g_lock };
                    ++g_stats.failed;
                    Finish(std::move(commit.then), false);
                } else {
                    std::scoped_lock lock{ g_lock };
                    g_commitQueue.insert(g_commitQueue.begin(), std::move(commit));
                }
                return;
            }
            MeasurePivot(ctx, *src, commit.source);
            const auto cap = Settings::GetSingleton().overlayBakeCapPx;
            const auto fit = OverlayTransform::FitToCap(src->width, src->height, cap);
            Target     target;
            if (!MakeTarget(device, fit.width, fit.height, src->srgb, target)) {
                std::scoped_lock lock{ g_lock };
                ++g_stats.failed;
                Finish(std::move(commit.then), false);
                return;
            }
            const auto t0 = std::chrono::steady_clock::now();
            Dispatch(ctx, *src, commit.t, fit.halvings, target);
            std::vector<std::uint8_t> bytes;
            if (!ReadBack(device, ctx, target, bytes)) {
                std::scoped_lock lock{ g_lock };
                ++g_stats.failed;
                Finish(std::move(commit.then), false);
                return;
            }
            const auto ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
            spdlog::info("OverlayBake: baked '{}' {}x{} mips={} (source {}x{}, cap {}) in {:.1f} ms; "
                         "writing {}.",
                         commit.source, target.width, target.height, target.mips, src->width,
                         src->height, cap, ms, commit.stem);
            {
                std::scoped_lock lock{ g_lock };
                g_commitInFlight.insert(commit.stem);
            }
            // ⚠⚠ GUARDED, AND THE IN-FLIGHT MARK IS A DESTRUCTOR'S JOB. An
            // exception escaping a std::thread's function is std::terminate,
            // which is a fail-fast with no crash log (WorkerGuard.h has the
            // whole story). WriteOnWorker erases its own stem from
            // g_commitInFlight near its end, so a throw before that line would
            // leave the stem marked in flight for the rest of the session and
            // every later bake of that overlay would be refused as already
            // running. Clearing it from a destructor covers the throwing path
            // and costs one redundant erase on the normal one.
            std::thread(
                [](CommitReq a_req, std::vector<std::uint8_t> a_bytes, std::uint32_t a_w,
                   std::uint32_t a_h, std::uint32_t a_mips, bool a_srgb) {
                    struct InFlightGuard {
                        std::string stem;
                        ~InFlightGuard() {
                            std::scoped_lock lock{ g_lock };
                            g_commitInFlight.erase(stem);
                        }
                    } inFlightGuard{ a_req.stem };
                    WorkerGuard::Run("OverlayBake", [&] {
                        WriteOnWorker(std::move(a_req), std::move(a_bytes), a_w, a_h, a_mips,
                                      a_srgb);
                    });
                },
                std::move(commit), std::move(bytes), target.width, target.height, target.mips,
                target.srgb)
                .detach();
        }
    }

}  // namespace OS::OverlayBake
