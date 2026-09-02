#include "PreviewRenderer.h"

#include "PreviewFraming.h"

#include <d3dcompiler.h>

#include <DirectXMath.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

#include <wrl/client.h>

namespace OS::PreviewRenderer {

    namespace {

        using Microsoft::WRL::ComPtr;

        // ⚠ FIELD ORDER IS THE HLSL CBUFFER'S, member for member. A reorder
        // here without the shader string below is a silently garbled draw.
        struct PreviewParams {
            DirectX::XMFLOAT4X4 ViewProj;
            DirectX::XMFLOAT3   LightDir;
            float               Ambient{ 0.36f };
            DirectX::XMFLOAT3   EyePos;
            float               SpecularStrength{ 0.10f };
            std::uint32_t       UseTexture{ 0 };
            std::uint32_t       UseNormal{ 0 };
            float               RimStrength{ 0.12f };
            float               FillStrength{ 0.30f };
            DirectX::XMFLOAT3   EmissiveColor{ 0.0f, 0.0f, 0.0f };
            float               MaterialAlpha{ 1.0f };
            float               AlphaCutoff{ 0.0f };
            float               EmissiveStrength{ 0.0f };
            float               SpecularPower{ 42.0f };
            std::uint32_t       UseAlpha{ 0 };
            float               BgLift{ 0.0f };  // backdrop lift, brow scenes
            float               PadBg[3]{};
        };
        static_assert(sizeof(PreviewParams) == 64 + 16 * 6, "cbuffer layout drifted");

        // The spec's shader, verbatim (spec lines 699-782): a lit pass for
        // geometry and a fullscreen-triangle gradient for the backdrop. The
        // two backdrop colours were retuned to sit near the editor's own
        // panel greys; that is a field-tuning knob, not structure.
        constexpr char kShader[] = R"(
cbuffer PreviewParams : register(b0)
{
    float4x4 ViewProj;
    float3   LightDir;      float Ambient;
    float3   EyePos;        float SpecularStrength;
    uint     UseTexture;    uint  UseNormal;
    float    RimStrength;   float FillStrength;
    float3   EmissiveColor; float MaterialAlpha;
    float    AlphaCutoff;   float EmissiveStrength;
    float    SpecularPower; uint  UseAlpha;
    float    BgLift;        float3 BgPad;
};

Texture2D    DiffuseTex     : register(t0);
SamplerState DiffuseSampler : register(s0);

struct VS_IN  { float3 Position : POSITION; float2 UV : TEXCOORD0; float3 Normal : NORMAL; };
struct VS_OUT { float4 Position : SV_Position; float2 UV : TEXCOORD0;
                float3 Normal : TEXCOORD1; float3 WorldPos : TEXCOORD2; };

VS_OUT PreviewVS(VS_IN input)
{
    VS_OUT o;
    o.Position = mul(float4(input.Position, 1.0), ViewProj);
    o.UV       = input.UV;
    o.Normal   = (UseNormal != 0) ? normalize(input.Normal) : float3(0.0, 0.0, 1.0);
    o.WorldPos = input.Position;
    return o;
}

float4 PreviewPS(VS_OUT input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    float4 sampled = (UseTexture != 0) ? DiffuseTex.Sample(DiffuseSampler, input.UV)
                                       : float4(0.62, 0.60, 0.56, 1.0);
    float alpha = saturate(MaterialAlpha * ((UseAlpha != 0) ? sampled.a : 1.0));
    if (alpha <= AlphaCutoff) { discard; }

    float3 baseColor = sampled.rgb;
    // TWO-SIDED LIGHTING, AND HAIR IS WHY IT IS NOT OPTIONAL.
    //
    // The rasteriser state is CULL_NONE, so back faces are drawn. Their
    // interpolated normal points AWAY from the key light, so dot(N,L) goes
    // negative and wrappedKey, fillLight and topLight all saturate to zero:
    // the fragment falls to Ambient alone and comes out nearly black.
    //
    // Closed geometry hides that. Armour and weapons are solids drawn in the
    // OPAQUE pass, where the front face is nearer and the depth write lets it
    // overwrite whatever the back face left. Hair is thin single-sided strand
    // cards drawn in the ALPHA pass with depth writes OFF, so the dark back
    // face is never rejected and blends straight into the front face beside
    // it. That is the hard-edged dark speckling reported over KS Hairdos
    // (field 2026-08-09), which reads as bad triangles and is really a normal
    // pointing the only way it can.
    float3 normal    = normalize(input.Normal) * (isFrontFace ? 1.0 : -1.0);
    float3 viewDir   = normalize(EyePos - input.WorldPos);
    float3 keyDir    = normalize(LightDir);
    float3 fillDir   = normalize(float3(-keyDir.x * 0.75, keyDir.y * 0.25, keyDir.z * 0.55 + 0.25));
    float3 rimDir    = normalize(float3(0.46, -0.36, 0.72));

    float  keyLight   = saturate(dot(normal, keyDir));
    float  wrappedKey = saturate(dot(normal, keyDir) * 0.65 + 0.35);
    float  fillLight  = saturate(dot(normal, fillDir));
    float  topLight   = saturate(normal.z * 0.5 + 0.5);
    float3 halfDir    = normalize(keyDir + viewDir);
    float  spec       = pow(saturate(dot(normal, halfDir)), max(SpecularPower, 8.0))
                        * SpecularStrength * (0.35 + keyLight * 0.65);
    float  rimMask    = saturate(dot(normal, rimDir) * 0.55 + 0.45);
    float  rim        = pow(1.0 - saturate(dot(normal, viewDir)), 2.2) * RimStrength * rimMask;

    float  lighting = Ambient + wrappedKey * 0.56 + fillLight * FillStrength + topLight * 0.08 + rim;
    float3 lit      = baseColor * lighting + spec;
    lit += saturate(EmissiveColor) * EmissiveStrength;
    lit  = saturate(lit * 0.96 + baseColor * 0.04);
    return float4(lit, alpha);
}

struct BG_OUT { float4 Position : SV_Position; float2 UV : TEXCOORD0; };

BG_OUT BackgroundVS(uint vertexID : SV_VertexID)
{
    float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    BG_OUT o;
    o.Position = float4(positions[vertexID], 0.0, 1.0);
    o.UV       = positions[vertexID] * float2(0.5, -0.5) + 0.5;
    return o;
}

float4 BackgroundPS(BG_OUT input) : SV_Target
{
    float2 centered  = input.UV * 2.0 - 1.0;
    float  radial    = length(float2(centered.x * 0.78, centered.y * 1.08));
    float  vignette  = smoothstep(1.28, 0.20, radial);
    float  floorLift = smoothstep(0.52, 1.0, input.UV.y) * 0.025;
    float3 color     = lerp(float3(0.066, 0.072, 0.086), float3(0.030, 0.033, 0.040), input.UV.y);
    color += vignette * 0.035 + floorLift;
    color *= 0.90 + vignette * 0.10;
    color += BgLift;
    return float4(saturate(color), 1.0);
}
)";

        // ---- session-lifetime pipeline objects ---------------------------

        ComPtr<ID3D11VertexShader>      g_vs;
        ComPtr<ID3D11PixelShader>       g_ps;
        ComPtr<ID3D11VertexShader>      g_bgVs;
        ComPtr<ID3D11PixelShader>       g_bgPs;
        ComPtr<ID3D11InputLayout>       g_inputLayout;
        ComPtr<ID3D11Buffer>            g_cbuffer;
        ComPtr<ID3D11RasterizerState>   g_rasterizer;
        ComPtr<ID3D11BlendState>        g_opaqueBlend;
        ComPtr<ID3D11BlendState>        g_alphaBlend;
        ComPtr<ID3D11DepthStencilState> g_depthWrite;
        ComPtr<ID3D11DepthStencilState> g_depthTestOnly;
        ComPtr<ID3D11DepthStencilState> g_depthOff;
        ComPtr<ID3D11SamplerState>      g_sampler;
        bool                            g_compileFailed = false;

        // The offscreen targets, rebuilt only when the size changes.
        ComPtr<ID3D11Texture2D>        g_colour;
        ComPtr<ID3D11RenderTargetView> g_rtv;
        ComPtr<ID3D11Texture2D>        g_depth;
        ComPtr<ID3D11DepthStencilView> g_dsv;
        ComPtr<ID3D11Texture2D>        g_staging;
        std::uint32_t                  g_targetSize = 0;

        // DyeTexture's CompileOne, generalised over entry point and profile
        // rather than reused: that helper hardcodes cs_5_0 and returns a
        // compute shader. Same no-include rule, one self-contained string.
        ID3DBlob* CompileBlob(const char* a_entry, const char* a_profile) {
            ID3DBlob*     code{ nullptr };
            ID3DBlob*     errors{ nullptr };
            const HRESULT hr =
                ::D3DCompile(kShader, sizeof(kShader) - 1, "PreviewShader", nullptr,
                             nullptr, a_entry, a_profile, 0, 0, &code, &errors);
            if (FAILED(hr) || !code) {
                spdlog::error("PreviewRenderer: D3DCompile({}) failed hr=0x{:08X} msg='{}'",
                              a_entry, static_cast<unsigned>(hr),
                              errors ? static_cast<const char*>(errors->GetBufferPointer())
                                     : "");
                if (errors) {
                    errors->Release();
                }
                return nullptr;
            }
            if (errors) {
                errors->Release();
            }
            return code;
        }

        bool EnsurePipeline(ID3D11Device* a_device) {
            if (g_vs && g_ps && g_bgVs && g_bgPs && g_cbuffer) {
                return true;
            }
            if (g_compileFailed) {
                return false;
            }
            const auto fail = [&] {
                g_compileFailed = true;
                spdlog::error("PreviewRenderer: pipeline setup failed once, so the "
                              "renderer stays down for the session.");
                return false;
            };

            ID3DBlob* vsBlob = CompileBlob("PreviewVS", "vs_5_0");
            if (!vsBlob) {
                return fail();
            }
            if (FAILED(a_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                                    vsBlob->GetBufferSize(), nullptr,
                                                    g_vs.GetAddressOf()))) {
                vsBlob->Release();
                return fail();
            }
            const D3D11_INPUT_ELEMENT_DESC layout[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                  D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
                  D3D11_INPUT_PER_VERTEX_DATA, 0 },
                { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20,
                  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            };
            const HRESULT ilHr = a_device->CreateInputLayout(
                layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                g_inputLayout.GetAddressOf());
            vsBlob->Release();
            if (FAILED(ilHr)) {
                return fail();
            }

            const auto makePs = [&](const char* a_entry, ComPtr<ID3D11PixelShader>& a_out) {
                ID3DBlob* blob = CompileBlob(a_entry, "ps_5_0");
                if (!blob) {
                    return false;
                }
                const HRESULT hr = a_device->CreatePixelShader(
                    blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                    a_out.GetAddressOf());
                blob->Release();
                return SUCCEEDED(hr);
            };
            const auto makeVs = [&](const char* a_entry, ComPtr<ID3D11VertexShader>& a_out) {
                ID3DBlob* blob = CompileBlob(a_entry, "vs_5_0");
                if (!blob) {
                    return false;
                }
                const HRESULT hr = a_device->CreateVertexShader(
                    blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                    a_out.GetAddressOf());
                blob->Release();
                return SUCCEEDED(hr);
            };
            if (!makePs("PreviewPS", g_ps) || !makeVs("BackgroundVS", g_bgVs) ||
                !makePs("BackgroundPS", g_bgPs)) {
                return fail();
            }

            D3D11_BUFFER_DESC cbDesc{};
            cbDesc.ByteWidth      = sizeof(PreviewParams);
            cbDesc.Usage          = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(a_device->CreateBuffer(&cbDesc, nullptr, g_cbuffer.GetAddressOf()))) {
                return fail();
            }

            // CULL_NONE: armour and weapon meshes are frequently single-sided
            // and cull wrong (the spec's own note).
            D3D11_RASTERIZER_DESC rs{};
            rs.FillMode        = D3D11_FILL_SOLID;
            rs.CullMode        = D3D11_CULL_NONE;
            rs.DepthClipEnable = TRUE;
            if (FAILED(a_device->CreateRasterizerState(&rs, g_rasterizer.GetAddressOf()))) {
                return fail();
            }

            D3D11_BLEND_DESC opaque{};
            opaque.RenderTarget[0].BlendEnable           = FALSE;
            opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(a_device->CreateBlendState(&opaque, g_opaqueBlend.GetAddressOf()))) {
                return fail();
            }
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].BlendEnable           = TRUE;
            blend.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
            blend.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (FAILED(a_device->CreateBlendState(&blend, g_alphaBlend.GetAddressOf()))) {
                return fail();
            }

            D3D11_DEPTH_STENCIL_DESC depth{};
            depth.DepthEnable    = TRUE;
            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            depth.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
            if (FAILED(a_device->CreateDepthStencilState(&depth,
                                                         g_depthWrite.GetAddressOf()))) {
                return fail();
            }
            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            if (FAILED(a_device->CreateDepthStencilState(&depth,
                                                         g_depthTestOnly.GetAddressOf()))) {
                return fail();
            }
            D3D11_DEPTH_STENCIL_DESC noDepth{};
            noDepth.DepthEnable = FALSE;
            if (FAILED(a_device->CreateDepthStencilState(&noDepth,
                                                         g_depthOff.GetAddressOf()))) {
                return fail();
            }

            D3D11_SAMPLER_DESC samp{};
            samp.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            samp.AddressU       = D3D11_TEXTURE_ADDRESS_WRAP;
            samp.AddressV       = D3D11_TEXTURE_ADDRESS_WRAP;
            samp.AddressW       = D3D11_TEXTURE_ADDRESS_WRAP;
            samp.MaxLOD         = D3D11_FLOAT32_MAX;
            if (FAILED(a_device->CreateSamplerState(&samp, g_sampler.GetAddressOf()))) {
                return fail();
            }
            return true;
        }

        // Spec Appendix A: colour bound as target and resource, D24S8 depth,
        // plus the staging sibling the readback maps.
        bool EnsureTargets(ID3D11Device* a_device, std::uint32_t a_size) {
            if (g_colour && g_rtv && g_dsv && g_staging && g_targetSize == a_size) {
                return true;
            }
            g_rtv.Reset();
            g_dsv.Reset();
            g_colour.Reset();
            g_depth.Reset();
            g_staging.Reset();
            g_targetSize = 0;

            D3D11_TEXTURE2D_DESC colorDesc{};
            colorDesc.Width            = a_size;
            colorDesc.Height           = a_size;
            colorDesc.MipLevels        = 1;
            colorDesc.ArraySize        = 1;
            colorDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
            colorDesc.SampleDesc.Count = 1;
            colorDesc.Usage            = D3D11_USAGE_DEFAULT;
            colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(a_device->CreateTexture2D(&colorDesc, nullptr,
                                                 g_colour.GetAddressOf()))) {
                return false;
            }
            if (FAILED(a_device->CreateRenderTargetView(g_colour.Get(), nullptr,
                                                        g_rtv.GetAddressOf()))) {
                return false;
            }

            D3D11_TEXTURE2D_DESC depthDesc = colorDesc;
            depthDesc.Format    = DXGI_FORMAT_D24_UNORM_S8_UINT;
            depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            if (FAILED(a_device->CreateTexture2D(&depthDesc, nullptr,
                                                 g_depth.GetAddressOf()))) {
                return false;
            }
            if (FAILED(a_device->CreateDepthStencilView(g_depth.Get(), nullptr,
                                                        g_dsv.GetAddressOf()))) {
                return false;
            }

            D3D11_TEXTURE2D_DESC stagingDesc = colorDesc;
            stagingDesc.Usage          = D3D11_USAGE_STAGING;
            stagingDesc.BindFlags      = 0;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(a_device->CreateTexture2D(&stagingDesc, nullptr,
                                                 g_staging.GetAddressOf()))) {
                return false;
            }
            g_targetSize = a_size;
            return true;
        }

        // ---- the state guard, Appendix B with the four corrections -------

        struct PipelineStateGuard {
            std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT>
                                                                     rtvs{};
            std::array<ID3D11UnorderedAccessView*, D3D11_PS_CS_UAV_REGISTER_COUNT> uavs{};
            ID3D11DepthStencilView*  dsv{ nullptr };
            std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
                                     viewports{};
            UINT                     viewportCount{ 0 };
            ID3D11VertexShader*      vs{ nullptr };
            ID3D11PixelShader*       ps{ nullptr };
            ID3D11GeometryShader*    gs{ nullptr };
            ID3D11HullShader*        hs{ nullptr };
            ID3D11DomainShader*      ds{ nullptr };
            ID3D11InputLayout*       inputLayout{ nullptr };
            D3D11_PRIMITIVE_TOPOLOGY topology{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
            ID3D11RasterizerState*   rasterizer{ nullptr };
            ID3D11BlendState*        blend{ nullptr };
            float                    blendFactor[4]{};
            UINT                     sampleMask{ 0 };
            ID3D11DepthStencilState* depth{ nullptr };
            UINT                     stencilRef{ 0 };
            ID3D11Buffer*            vb[1]{ nullptr };
            UINT                     strides[1]{ 0 };
            UINT                     offsets[1]{ 0 };
            ID3D11Buffer*            ib{ nullptr };
            DXGI_FORMAT              ibFormat{ DXGI_FORMAT_UNKNOWN };
            UINT                     ibOffset{ 0 };
            ID3D11Buffer*            vsCb[1]{ nullptr };
            ID3D11Buffer*            psCb[1]{ nullptr };
            ID3D11SamplerState*      sampler[1]{ nullptr };
            ID3D11ShaderResourceView* srv[1]{ nullptr };

            void Save(ID3D11DeviceContext* a_ctx) {
                // Correction 1: the UAV-aware variant. Plain OMSetRenderTargets
                // on restore would silently drop a UAV Community Shaders or
                // ENB had bound at the OM stage, and the symptom is a broken
                // effect with a clean log.
                a_ctx->OMGetRenderTargetsAndUnorderedAccessViews(
                    static_cast<UINT>(rtvs.size()), rtvs.data(), &dsv, 0,
                    static_cast<UINT>(uavs.size()), uavs.data());
                viewportCount = static_cast<UINT>(viewports.size());
                a_ctx->RSGetViewports(&viewportCount, viewports.data());
                // Correction 2: ppClassInstances and its count must be both
                // null or both non-null; Wardrobe passes a count with no
                // array, which the debug layer flags.
                a_ctx->VSGetShader(&vs, nullptr, nullptr);
                a_ctx->PSGetShader(&ps, nullptr, nullptr);
                a_ctx->GSGetShader(&gs, nullptr, nullptr);
                a_ctx->HSGetShader(&hs, nullptr, nullptr);
                a_ctx->DSGetShader(&ds, nullptr, nullptr);
                a_ctx->IAGetInputLayout(&inputLayout);
                a_ctx->IAGetPrimitiveTopology(&topology);
                a_ctx->RSGetState(&rasterizer);
                a_ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
                a_ctx->OMGetDepthStencilState(&depth, &stencilRef);
                a_ctx->IAGetVertexBuffers(0, 1, vb, strides, offsets);
                a_ctx->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
                a_ctx->VSGetConstantBuffers(0, 1, vsCb);
                a_ctx->PSGetConstantBuffers(0, 1, psCb);
                a_ctx->PSGetSamplers(0, 1, sampler);
                a_ctx->PSGetShaderResources(0, 1, srv);
            }

            void Restore(ID3D11DeviceContext* a_ctx) {
                ID3D11ShaderResourceView* nullSrv = nullptr;
                a_ctx->PSSetShaderResources(0, 1, &nullSrv);  // never leave ours bound

                a_ctx->OMSetRenderTargetsAndUnorderedAccessViews(
                    static_cast<UINT>(rtvs.size()), rtvs.data(), dsv, 0,
                    static_cast<UINT>(uavs.size()), uavs.data(), nullptr);
                // Correction 3: unconditional. Zero viewports is still a
                // state to restore; a conditional restore beside an
                // unconditional set is the shape of a real bug.
                a_ctx->RSSetViewports(viewportCount,
                                      viewportCount > 0 ? viewports.data() : nullptr);
                a_ctx->VSSetShader(vs, nullptr, 0);
                a_ctx->PSSetShader(ps, nullptr, 0);
                a_ctx->GSSetShader(gs, nullptr, 0);
                a_ctx->HSSetShader(hs, nullptr, 0);
                a_ctx->DSSetShader(ds, nullptr, 0);
                a_ctx->IASetInputLayout(inputLayout);
                a_ctx->IASetPrimitiveTopology(topology);
                a_ctx->RSSetState(rasterizer);
                a_ctx->OMSetBlendState(blend, blendFactor, sampleMask);
                a_ctx->OMSetDepthStencilState(depth, stencilRef);
                a_ctx->IASetVertexBuffers(0, 1, vb, strides, offsets);
                a_ctx->IASetIndexBuffer(ib, ibFormat, ibOffset);
                a_ctx->VSSetConstantBuffers(0, 1, vsCb);
                a_ctx->PSSetConstantBuffers(0, 1, psCb);
                a_ctx->PSSetSamplers(0, 1, sampler);
                a_ctx->PSSetShaderResources(0, 1, srv);

                // Every interface a Get call returned was AddRef'ed and is
                // released here, the UAV array included, or the preview leaks
                // a reference per frame into the game's own pipeline objects.
                for (auto*& r : rtvs) {
                    if (r) { r->Release(); r = nullptr; }
                }
                for (auto*& u : uavs) {
                    if (u) { u->Release(); u = nullptr; }
                }
                IUnknown* singles[] = { dsv, vs, ps, gs, hs, ds, inputLayout, rasterizer,
                                        blend, depth, vb[0], ib, vsCb[0], psCb[0],
                                        sampler[0], srv[0] };
                for (auto* s : singles) {
                    if (s) { s->Release(); }
                }
                // Correction 4 is not code: predication and the
                // ...SetConstantBuffers1 offset family are NOT covered here.
                // An active predicate would silently skip the preview's own
                // draws (cosmetic); a D3D11.1 offset binding by another mod
                // would lose its offsets. Known gaps, stated rather than
                // pretended away.
            }
        };

        // The whole pose comes from PreviewFraming: the flat of the item to
        // the eye, the long axis on the card's diagonal, the corner-fit
        // distance and the light rig riding the camera basis. This function
        // only lifts that answer into DirectX types.
        void FillCamera(const std::array<float, 3>& a_min, const std::array<float, 3>& a_max,
                        PreviewFraming::Pose a_pose, PreviewParams& a_out) {
            const DirectX::XMFLOAT3 center{ (a_min[0] + a_max[0]) * 0.5f,
                                            (a_min[1] + a_max[1]) * 0.5f,
                                            (a_min[2] + a_max[2]) * 0.5f };
            const auto frame = PreviewFraming::FrameFor(a_min, a_max, a_pose);

            const DirectX::XMFLOAT3 eyeDir{ frame.eyeDir.x, frame.eyeDir.y,
                                            frame.eyeDir.z };
            const DirectX::XMFLOAT3 up{ frame.up.x, frame.up.y, frame.up.z };
            const auto centerVec = DirectX::XMLoadFloat3(&center);
            const auto eye       = DirectX::XMVectorMultiplyAdd(
                DirectX::XMLoadFloat3(&eyeDir),
                DirectX::XMVectorReplicate(frame.distance), centerVec);
            const auto view =
                DirectX::XMMatrixLookAtLH(eye, centerVec, DirectX::XMLoadFloat3(&up));
            const float fov = DirectX::XMConvertToRadians(PreviewFraming::kFovDegrees);
            const auto  proj =
                DirectX::XMMatrixPerspectiveFovLH(fov, 1.0f, frame.nearZ, frame.farZ);
            DirectX::XMStoreFloat4x4(&a_out.ViewProj,
                                     DirectX::XMMatrixTranspose(view * proj));
            DirectX::XMStoreFloat3(&a_out.EyePos, eye);
            a_out.LightDir = { frame.lightDir.x, frame.lightDir.y, frame.lightDir.z };
        }

        double MsSince(std::chrono::steady_clock::time_point a_t0) {
            return std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - a_t0)
                .count();
        }

    }  // namespace

    bool RenderThumbnail(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx,
                         const std::vector<MeshExtractor::RenderMesh>& a_meshes,
                         std::uint32_t a_size, PreviewFraming::Pose a_pose,
                         float a_bgLift, PreviewFraming::Crop a_crop,
                         std::vector<std::uint8_t>& a_outRgba,
                         Timings& a_outTimings) {
        if (!a_device || !a_ctx || a_meshes.empty() || a_size == 0) {
            return false;
        }
        if (!EnsurePipeline(a_device) || !EnsureTargets(a_device, a_size)) {
            return false;
        }

        // Merged scene bounds for one camera fit, from the meshes that have
        // real bounds; a mesh with none inherits the union of the others.
        std::array<float, 3> bmin{ -15.0f, -15.0f, -15.0f };
        std::array<float, 3> bmax{ 15.0f, 15.0f, 15.0f };
        bool                 have = false;
        // The mannequin's box and the item's box, kept apart for the crop.
        std::array<float, 3> mannMin{}, mannMax{}, itemMin{}, itemMax{};
        bool                 haveMann = false;
        bool                 haveItem = false;
        // The head is measured as well as drawn, and it is measured APART from
        // the reference body for the reason the reference exists: a full-face
        // helm takes it off the mannequin, so a box that mixed the two would
        // change size when the head went. It is the eye window's anchor
        // (OS-205, PreviewFraming's kEyeHeadView) and nothing else reads it.
        std::array<float, 3> headMin{}, headMax{};
        bool                 haveHead = false;
        for (const auto& m : a_meshes) {
            if (!m.HaveBounds) {
                continue;
            }
            if (!have) {
                bmin = m.BoundsMin;
                bmax = m.BoundsMax;
                have = true;
            } else {
                for (int i = 0; i < 3; ++i) {
                    bmin[i] = (std::min)(bmin[i], m.BoundsMin[i]);
                    bmax[i] = (std::max)(bmax[i], m.BoundsMax[i]);
                }
            }
            // ⚠⚠ THE REFERENCE IS THE BODY ALONE, and both the head and the
            // feet are excluded for the same reason: they are the parts that
            // can be dropped, and a reference that changes size when a part
            // goes slides every window along the figure. The head goes under a
            // full-face helm; the FEET go under any footwear, and being the
            // thing the box stands on they take the floor with them, which put
            // three knee-high boots' windows ten units up the leg and cut them
            // off at the bottom (field 2026-08-10). The hands are inside the
            // body's own z range and never mattered either way.
            //
            // ReferenceBody extrapolates both ends back to a full standing
            // figure, so the box is the same however many parts went.
            const bool refPart =
                m.Mannequin && !m.MannequinHead && !m.MannequinFeet;
            auto&      hit = refPart ? haveMann : haveItem;
            auto&      mn  = refPart ? mannMin : itemMin;
            auto&      mx  = refPart ? mannMax : itemMax;
            if (m.Mannequin && !refPart) {
                if (m.MannequinHead) {
                    if (!haveHead) {
                        headMin  = m.BoundsMin;
                        headMax  = m.BoundsMax;
                        haveHead = true;
                    } else {
                        for (int i = 0; i < 3; ++i) {
                            headMin[i] = (std::min)(headMin[i], m.BoundsMin[i]);
                            headMax[i] = (std::max)(headMax[i], m.BoundsMax[i]);
                        }
                    }
                }
                continue;  // drawn, and measured only into the head's own box
            }
            if (!hit) {
                mn  = m.BoundsMin;
                mx  = m.BoundsMax;
                hit = true;
                continue;
            }
            for (int i = 0; i < 3; ++i) {
                mn[i] = (std::min)(mn[i], m.BoundsMin[i]);
                mx[i] = (std::max)(mx[i], m.BoundsMax[i]);
            }
        }
        if (a_pose == PreviewFraming::Pose::kEyes && !haveMann) {
            // One eye, not the staring pair (PreviewFraming::EyeHalf). ⚠ THIS
            // BRANCH IS THE UN-COMPOSED ONE and it must stay exactly as it
            // was: such a scene keys byte-identically to the card already on
            // disk. With a face to sit in, the band below does the same job
            // and does it against the head rather than against the eyeball.
            const auto cropped = PreviewFraming::EyeHalf(bmin, bmax);
            bmin               = cropped.first;
            bmax               = cropped.second;
        } else if (haveMann && haveItem) {
            // ⚠ THE SLAB IS A FRACTION OF THE BODY, NOT OF THE SCENE, and the
            // difference is a card. A spiked helmet or a tall plume makes the
            // scene box taller than the figure, so a head slab measured off it
            // starts above the head and cuts the face off the bottom (field
            // 2026-08-10). Measured off the mannequin instead.
            //
            // ⚠⚠ AND THERE IS NO UNION WITH THE ITEM. This comment used to
            // promise one and the code never did it, which cost an
            // investigation. The code is right: a window widened to hold
            // whatever the item sticks out is a frame of the ITEM's, and that
            // is the one thing the standardized views exist to rule out. An
            // item that reaches past its window gets cut, and PreviewFraming.h
            // says so where the views are declared. Do not restore it.
            // ⚠ THE WINDOW IS THE MANNEQUIN'S, so every card wearing a slot
            // puts the body in the same place. The item is consulted for one
            // thing only: which window a slot nobody wrote down belongs in.
            const auto crop = a_crop == PreviewFraming::Crop::kWhole
                                  ? PreviewFraming::CropByHeight(
                                        mannMin[2],
                                        PreviewFraming::ReferenceBody(mannMin, mannMax)
                                            .second[2],
                                        itemMin[2], itemMax[2])
                                  : a_crop;
            const auto ref = PreviewFraming::ReferenceBody(mannMin, mannMax);
            const PreviewFraming::Box head{ headMin, headMax, haveHead };
            const PreviewFraming::Box item{ itemMin, itemMax, haveItem };
            const auto                window =
                PreviewFraming::WindowFor(ref.first, ref.second, crop, head, item);
            // ⚠ THE THREE BOXES, BECAUSE THE DEPTH AXIS IS UNMEASURED. Every
            // view number so far was tuned on z and x, which are the axes a
            // card shows; Y has never been read off this rig, and it is what
            // holds the camera back on the tightest windows (kEyeView is
            // already the size of the eye and still cannot zoom, because
            // maxDepth is half the body's front-to-back extent). One card of
            // any figure scene answers it, and then a depth clip is
            // arithmetic instead of a guess at where a face is.
            spdlog::debug(
                "PreviewRenderer: preview.boxes crop={} mann=({:.2f},{:.2f},{:.2f})"
                "..({:.2f},{:.2f},{:.2f}) ref z {:.2f}..{:.2f} item=({:.2f},{:.2f},"
                "{:.2f})..({:.2f},{:.2f},{:.2f}) head={} z {:.2f}..{:.2f} y {:.2f}"
                "..{:.2f} window z {:.2f}..{:.2f}",
                static_cast<int>(crop), mannMin[0], mannMin[1], mannMin[2], mannMax[0],
                mannMax[1], mannMax[2], ref.first[2], ref.second[2], itemMin[0],
                itemMin[1], itemMin[2], itemMax[0], itemMax[1], itemMax[2], haveHead,
                headMin[2], headMax[2], headMin[1], headMax[1], window.first[2],
                window.second[2]);
            bmin              = window.first;
            bmax              = window.second;
        }

        const auto renderStart = std::chrono::steady_clock::now();

        PipelineStateGuard guard;
        guard.Save(a_ctx);

        const float clearColor[4] = { 0.040f, 0.044f, 0.052f, 1.0f };
        a_ctx->ClearRenderTargetView(g_rtv.Get(), clearColor);
        a_ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                     1.0f, 0);

        ID3D11RenderTargetView* rtv = g_rtv.Get();
        a_ctx->OMSetRenderTargets(1, &rtv, g_dsv.Get());
        a_ctx->RSSetState(g_rasterizer.Get());

        D3D11_VIEWPORT viewport{};
        viewport.Width    = static_cast<float>(a_size);
        viewport.Height   = static_cast<float>(a_size);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        a_ctx->RSSetViewports(1, &viewport);

        // The backdrop: fullscreen triangle, no vertex input, depth off.
        a_ctx->OMSetBlendState(g_opaqueBlend.Get(), nullptr, 0xFFFFFFFF);
        a_ctx->OMSetDepthStencilState(g_depthOff.Get(), 0);
        a_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        a_ctx->IASetInputLayout(nullptr);
        ID3D11Buffer* nullBuffer = nullptr;
        UINT          zero       = 0;
        a_ctx->IASetVertexBuffers(0, 1, &nullBuffer, &zero, &zero);
        a_ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        a_ctx->VSSetShader(g_bgVs.Get(), nullptr, 0);
        a_ctx->PSSetShader(g_bgPs.Get(), nullptr, 0);
        a_ctx->GSSetShader(nullptr, nullptr, 0);
        a_ctx->HSSetShader(nullptr, nullptr, 0);
        a_ctx->DSSetShader(nullptr, nullptr, 0);
        {
            // The backdrop reads one cbuffer value: the lift a brow scene
            // buys so near-black strands sit on a readable card (field
            // 2026-08-09 round 3). Everything else in the struct is unused
            // by BackgroundPS and zeroed here; the per-mesh maps below
            // overwrite the whole buffer anyway.
            PreviewParams bgParams{};
            bgParams.BgLift = a_bgLift;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(a_ctx->Map(g_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                     &mapped))) {
                std::memcpy(mapped.pData, &bgParams, sizeof(bgParams));
                a_ctx->Unmap(g_cbuffer.Get(), 0);
            }
            ID3D11Buffer* bgCb = g_cbuffer.Get();
            a_ctx->PSSetConstantBuffers(0, 1, &bgCb);
        }
        a_ctx->Draw(3, 0);

        // The meshes.
        a_ctx->IASetInputLayout(g_inputLayout.Get());
        a_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
        a_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
        ID3D11Buffer* cb = g_cbuffer.Get();
        a_ctx->VSSetConstantBuffers(0, 1, &cb);
        a_ctx->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* sampler = g_sampler.Get();
        a_ctx->PSSetSamplers(0, 1, &sampler);

        UINT       stride   = sizeof(MeshExtractor::PreviewVertex);
        UINT       offset   = 0;
        const auto drawMesh = [&](const MeshExtractor::RenderMesh& a_mesh) {
            PreviewParams params{};
            FillCamera(bmin, bmax, a_pose, params);
            params.UseTexture       = a_mesh.Diffuse ? 1u : 0u;
            params.UseNormal        = 1u;
            params.UseAlpha         = a_mesh.UseAlpha ? 1u : 0u;
            params.MaterialAlpha    = a_mesh.MaterialAlpha;
            params.AlphaCutoff      = a_mesh.AlphaCutoff;
            params.SpecularStrength = a_mesh.SpecularStrength;
            params.SpecularPower    = a_mesh.SpecularPower;
            params.EmissiveColor    = { a_mesh.EmissiveColor[0], a_mesh.EmissiveColor[1],
                                        a_mesh.EmissiveColor[2] };
            params.EmissiveStrength = a_mesh.EmissiveStrength;

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(a_ctx->Map(g_cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                  &mapped))) {
                return;
            }
            std::memcpy(mapped.pData, &params, sizeof(params));
            a_ctx->Unmap(g_cbuffer.Get(), 0);

            ID3D11Buffer* vb = a_mesh.VertexBuffer.Get();
            a_ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            a_ctx->IASetIndexBuffer(a_mesh.IndexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* srv = a_mesh.Diffuse.Get();
            a_ctx->PSSetShaderResources(0, 1, &srv);
            a_ctx->DrawIndexed(a_mesh.IndexCount, 0, 0);
        };

        // Opaque first, then the alpha pass over a depth buffer it tests but
        // no longer writes.
        a_ctx->OMSetDepthStencilState(g_depthWrite.Get(), 0);
        a_ctx->OMSetBlendState(g_opaqueBlend.Get(), nullptr, 0xFFFFFFFF);
        for (const auto& mesh : a_meshes) {
            if (!mesh.BlendAlpha) {
                drawMesh(mesh);
            }
        }
        a_ctx->OMSetDepthStencilState(g_depthTestOnly.Get(), 0);
        a_ctx->OMSetBlendState(g_alphaBlend.Get(), nullptr, 0xFFFFFFFF);
        for (const auto& mesh : a_meshes) {
            if (mesh.BlendAlpha) {
                drawMesh(mesh);
            }
        }

        a_outTimings.renderMs = MsSince(renderStart);

        // The readback: one staging copy and one blocking map.
        const auto readbackStart = std::chrono::steady_clock::now();
        a_ctx->CopyResource(g_staging.Get(), g_colour.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        bool                     ok = false;
        if (SUCCEEDED(a_ctx->Map(g_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            a_outRgba.resize(static_cast<std::size_t>(a_size) * a_size * 4u);
            for (std::uint32_t row = 0; row < a_size; ++row) {
                std::memcpy(a_outRgba.data() + static_cast<std::size_t>(row) * a_size * 4u,
                            static_cast<const std::uint8_t*>(mapped.pData) +
                                static_cast<std::size_t>(row) * mapped.RowPitch,
                            static_cast<std::size_t>(a_size) * 4u);
            }
            a_ctx->Unmap(g_staging.Get(), 0);
            ok = true;
        }
        a_outTimings.readbackMs = MsSince(readbackStart);

        guard.Restore(a_ctx);
        return ok;
    }

}  // namespace OS::PreviewRenderer
