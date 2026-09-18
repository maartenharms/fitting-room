#include "DyeGpu.h"
#include "GpuAccess.h"
#include "RendererData.h"

#include "Settings.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace OS::DyeGpu {

    namespace {

        // ---- the probe's dimensions -------------------------------------
        //
        // 64x64 is deliberately small. Task 1 asks whether a dispatch runs, not
        // what it costs, and a small target keeps the readback stall
        // irrelevant. Task 2 is where a 4096² goes under a timestamp query.
        constexpr UINT kProbeDim    = 64;
        constexpr UINT kThreadGroup = 8;  // must match [numthreads] below

        // ⚠ THE PATTERN IS COORDINATE DEPENDENT ON PURPOSE. A constant fill
        // cannot distinguish "the grid was covered" from "one group ran and the
        // rest of the texture happened to already hold that value". Writing x
        // into red and y into green means the readback proves BOTH that the
        // dispatch ran and that the thread-to-pixel mapping is what we think it
        // is: pixel (5,7) must read back r=5 g=7 or the probe has not earned
        // its result. Blue is a fixed 0.75 (191 after the UNORM round trip) as
        // a third independent byte that no coordinate can produce by accident.
        constexpr char kProbeShader[] = R"(
RWTexture2D<float4> Dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Dst[id.xy] = float4(id.x / 255.0, id.y / 255.0, 0.75, 1.0);
}
)";

        // The three pixels sampled on both sides of the dispatch. (0,0) is the
        // origin, (5,7) is asymmetric so a transposed mapping is visible as a
        // swap rather than hiding behind x==y, and (63,63) is the far corner
        // and proves the last thread group ran.
        struct Sample {
            UINT x;
            UINT y;
        };
        constexpr std::array<Sample, 3> kSamples{ { { 0, 0 }, { 5, 7 }, { 63, 63 } } };

        struct Pixel {
            std::uint8_t r{ 0 };
            std::uint8_t g{ 0 };
            std::uint8_t b{ 0 };
            std::uint8_t a{ 0 };
        };

        // Copies a_src into a_staging and reads the sample pixels out of it.
        // Returns false if the map failed, which is a different answer from
        // "the values were wrong" and is logged as such by the caller.
        bool ReadSamples(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_src,
                         ID3D11Texture2D* a_staging, std::array<Pixel, 3>& a_out) {
            a_ctx->CopyResource(a_staging, a_src);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            // No DO_NOT_WAIT: this blocks until the copy (and anything queued
            // before it, which is the whole point on the post-dispatch read)
            // has retired. A probe can afford the stall.
            if (FAILED(a_ctx->Map(a_staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                return false;
            }
            const auto* const base = static_cast<const std::uint8_t*>(mapped.pData);
            for (std::size_t i = 0; i < kSamples.size(); ++i) {
                // ⚠ RowPitch, NEVER width*4. A staging texture's rows are
                // padded to the driver's alignment and 64*4 happens to be a
                // plausible pitch, which is exactly the kind of coincidence
                // that makes a wrong read look right on one machine.
                const auto* const px =
                    base + (kSamples[i].y * mapped.RowPitch) + (kSamples[i].x * 4u);
                a_out[i] = Pixel{ px[0], px[1], px[2], px[3] };
            }
            a_ctx->Unmap(a_staging, 0);
            return true;
        }

        void LogSamples(const char* a_label, const std::array<Pixel, 3>& a_px, bool a_ok) {
            if (!a_ok) {
                spdlog::error("DyeGpu: {} readback FAILED to map.", a_label);
                return;
            }
            for (std::size_t i = 0; i < kSamples.size(); ++i) {
                spdlog::info("DyeGpu: {} ({:2},{:2}) = ({:3},{:3},{:3},{:3})", a_label,
                             kSamples[i].x, kSamples[i].y, a_px[i].r, a_px[i].g, a_px[i].b,
                             a_px[i].a);
            }
        }

        // ---- task 2's shader -----------------------------------------------
        //
        // Read a BC7 diffuse, tint it, write the result. ⚠ Load() RATHER THAN A
        // SAMPLER, because the hardware decodes the BC7 block either way and
        // Load gives an exact texel-for-texel mapping with no filtering to
        // account for. That the decode is free is the whole premise this task
        // exists to price.
        //
        // ⚠ THE TINT MATH IS THE GAME'S OWN OVERLAY, and it is here so the
        // number is honest rather than because rung 3 would keep it. Rung 3
        // would write a real recolour; the cost is identical either way,
        // because at 16.7 million texels this pass is bound by memory and not
        // by arithmetic.
        constexpr char kTintShader[] = R"(
Texture2D<float4>   Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float4 d = Src.Load(int3(id.xy, 0));
    float3 t = float3(0.35, 0.12, 0.45);
    float3 o = d.rgb * d.rgb + 2.0 * t * d.rgb * (1.0 - d.rgb);
    Dst[id.xy] = float4(o, d.a);
}
)";

        ID3D11ComputeShader* Compile(ID3D11Device* a_device, const char* a_src,
                                     std::size_t a_len, const char* a_name) {
            ID3DBlob* code{ nullptr };
            ID3DBlob* errors{ nullptr };
            const HRESULT hr = ::D3DCompile(a_src, a_len, a_name, nullptr, nullptr, "main",
                                            "cs_5_0", 0, 0, &code, &errors);
            if (FAILED(hr) || !code) {
                spdlog::error("DyeGpu: D3DCompile('{}') failed hr=0x{:08X} msg='{}'", a_name,
                              static_cast<unsigned>(hr),
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                if (errors) {
                    errors->Release();
                }
                return nullptr;
            }
            if (errors) {
                errors->Release();
            }
            ID3D11ComputeShader* cs{ nullptr };
            if (FAILED(a_device->CreateComputeShader(code->GetBufferPointer(),
                                                     code->GetBufferSize(), nullptr, &cs))) {
                spdlog::error("DyeGpu: CreateComputeShader('{}') failed.", a_name);
                cs = nullptr;
            }
            code->Release();
            return cs;
        }

        // Runs a dispatch under a timestamp pair and returns milliseconds, or a
        // negative number when the timing could not be trusted.
        //
        // ⚠ THE WARM-UP IS NOT OPTIONAL AND IT IS NOT TIMED. The first dispatch
        // of a shader carries driver-side setup that a shipped path would pay
        // once and never again, and reporting it as the per-texture cost would
        // overstate the very number this task exists to produce.
        double TimedDispatch(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx,
                             ID3D11ComputeShader* a_cs, ID3D11ShaderResourceView* a_srv,
                             ID3D11UnorderedAccessView* a_uav, UINT a_groupsX, UINT a_groupsY) {
            ID3D11Query*     disjointQ{ nullptr };
            ID3D11Query*     startQ{ nullptr };
            ID3D11Query*     endQ{ nullptr };
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            a_device->CreateQuery(&qd, &disjointQ);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            a_device->CreateQuery(&qd, &startQ);
            a_device->CreateQuery(&qd, &endQ);
            if (!disjointQ || !startQ || !endQ) {
                if (disjointQ) { disjointQ->Release(); }
                if (startQ) { startQ->Release(); }
                if (endQ) { endQ->Release(); }
                return -1.0;
            }

            a_ctx->CSSetShader(a_cs, nullptr, 0);
            a_ctx->CSSetShaderResources(0, 1, &a_srv);
            a_ctx->CSSetUnorderedAccessViews(0, 1, &a_uav, nullptr);

            a_ctx->Dispatch(a_groupsX, a_groupsY, 1);  // warm-up, deliberately untimed

            a_ctx->Begin(disjointQ);
            a_ctx->End(startQ);
            a_ctx->Dispatch(a_groupsX, a_groupsY, 1);
            a_ctx->End(endQ);
            a_ctx->End(disjointQ);

            ID3D11UnorderedAccessView* const nullUav[1]{ nullptr };
            ID3D11ShaderResourceView* const  nullSrv[1]{ nullptr };
            a_ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            a_ctx->CSSetShaderResources(0, 1, nullSrv);
            a_ctx->CSSetShader(nullptr, nullptr, 0);

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            while (a_ctx->GetData(disjointQ, &dj, sizeof(dj), 0) != S_OK) {
            }
            std::uint64_t t0{ 0 };
            std::uint64_t t1{ 0 };
            while (a_ctx->GetData(startQ, &t0, sizeof(t0), 0) != S_OK) {
            }
            while (a_ctx->GetData(endQ, &t1, sizeof(t1), 0) != S_OK) {
            }
            disjointQ->Release();
            startQ->Release();
            endQ->Release();
            if (dj.Disjoint || !dj.Frequency) {
                return -1.0;
            }
            return 1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(dj.Frequency);
        }

        // One measurement: a BC7 source of a_dim square, tinted into an RGBA8
        // destination of the same size.
        void MeasureTint(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx,
                         ID3D11ComputeShader* a_cs, UINT a_dim) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width            = a_dim;
            sd.Height           = a_dim;
            sd.MipLevels        = 1;
            sd.ArraySize        = 1;
            sd.Format           = DXGI_FORMAT_BC7_UNORM;
            sd.SampleDesc.Count = 1;
            sd.Usage            = D3D11_USAGE_DEFAULT;
            sd.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

            // ⚠ REAL BYTES, AND THE FIRST RUN'S NUMBERS DIED FOR WANT OF THEM.
            // Created with nullptr the source is never written, and a run
            // reported 4096 at 0.045 ms against 2048 at 0.042: four times the
            // texels for seven percent more time, which is 1.8 TB/s and above
            // any consumer card's bandwidth. Uninitialised memory does not have
            // to be fetched. Filling it forces the reads to be real.
            //
            // BC7 rows are BLOCK rows: 4x4 texels per block, 16 bytes per block,
            // so the pitch is (width/4)*16 and there are height/4 of them.
            const UINT blocksX = a_dim / 4;
            const UINT blocksY = a_dim / 4;
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(blocksX) * blocksY * 16u);
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xFFu);
            }
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem     = bytes.data();
            init.SysMemPitch = blocksX * 16u;

            ID3D11Texture2D* src{ nullptr };
            if (FAILED(a_device->CreateTexture2D(&sd, &init, &src)) || !src) {
                spdlog::error("DyeGpu: task 2 could not create a {}x{} BC7 source.", a_dim, a_dim);
                return;
            }

            D3D11_TEXTURE2D_DESC dd = sd;
            dd.Format               = DXGI_FORMAT_R8G8B8A8_UNORM;
            dd.BindFlags            = D3D11_BIND_UNORDERED_ACCESS;

            ID3D11Texture2D* dst{ nullptr };
            if (FAILED(a_device->CreateTexture2D(&dd, nullptr, &dst)) || !dst) {
                spdlog::error("DyeGpu: task 2 could not create a {}x{} RGBA8 target.", a_dim,
                              a_dim);
                src->Release();
                return;
            }

            // A 1x1 staging texture is enough for the control: it only has to
            // answer "did anything land here".
            D3D11_TEXTURE2D_DESC pd{};
            pd.Width            = 1;
            pd.Height           = 1;
            pd.MipLevels        = 1;
            pd.ArraySize        = 1;
            pd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
            pd.SampleDesc.Count = 1;
            pd.Usage            = D3D11_USAGE_STAGING;
            pd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
            ID3D11Texture2D* probe{ nullptr };
            a_device->CreateTexture2D(&pd, nullptr, &probe);

            ID3D11ShaderResourceView*  srv{ nullptr };
            ID3D11UnorderedAccessView* uav{ nullptr };
            a_device->CreateShaderResourceView(src, nullptr, &srv);
            a_device->CreateUnorderedAccessView(dst, nullptr, &uav);
            if (srv && uav) {
                const UINT groups = (a_dim + kThreadGroup - 1) / kThreadGroup;
                // ⚠ BEST OF SEVEN, NOT ONE SHOT. A single dispatch can land
                // inside a scheduling gap and read as far quicker than the work
                // can possibly be; the minimum over a handful is the honest
                // "what does this cost when nothing is in the way".
                constexpr int kRuns = 7;
                double best = -1.0;
                for (int i = 0; i < kRuns; ++i) {
                    const double ms =
                        TimedDispatch(a_device, a_ctx, a_cs, srv, uav, groups, groups);
                    if (ms >= 0.0 && (best < 0.0 || ms < best)) {
                        best = ms;
                    }
                }

                // ⚠ THE CONTROL, AND TASK 2 SHIPPED WITHOUT IT ONCE. Task 1 had
                // one and that is the only reason its result is believable. A
                // timing with nothing proving the dispatch wrote anything is a
                // number about an empty command buffer. Pixel (5,7) of the
                // destination must be something other than the zero an
                // untouched RGBA8 surface reads as.
                bool wrote = false;
                std::uint8_t px[4]{ 0, 0, 0, 0 };
                if (probe) {
                    D3D11_BOX box{ 5, 7, 0, 6, 8, 1 };
                    a_ctx->CopySubresourceRegion(probe, 0, 0, 0, 0, dst, 0, &box);
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (SUCCEEDED(a_ctx->Map(probe, 0, D3D11_MAP_READ, 0, &m))) {
                        const auto* p = static_cast<const std::uint8_t*>(m.pData);
                        px[0] = p[0]; px[1] = p[1]; px[2] = p[2]; px[3] = p[3];
                        a_ctx->Unmap(probe, 0);
                        wrote = (px[0] | px[1] | px[2] | px[3]) != 0;
                    }
                }

                // BC7 is one byte per texel, RGBA8 is four. Mip 0 only; a full
                // chain is 4/3 of this, which is where the spec's 21.3 MiB for a
                // 4096 BC7 came from against the 16 MiB here.
                const double srcMiB = static_cast<double>(a_dim) * a_dim / (1024.0 * 1024.0);
                if (best < 0.0) {
                    spdlog::warn("DyeGpu: task 2 {}x{} timing was DISJOINT, not trustworthy.",
                                 a_dim, a_dim);
                } else {
                    // Traffic is what makes a number checkable: read the source
                    // once, write the destination once. An implied bandwidth
                    // above the card's is the tell that the pass did not happen.
                    const double movedMiB = srcMiB * 5.0;  // 1 read + 4 written
                    const double gbs      = (movedMiB / 1024.0) / (best / 1000.0);
                    spdlog::info("DyeGpu: TASK 2  {:>4}x{:<4} BC7 -> RGBA8  best {:8.3f} ms of "
                                 "{}   {:.0f} GB/s implied   dst=({},{},{},{}) {}",
                                 a_dim, a_dim, best, kRuns, gbs, px[0], px[1], px[2], px[3],
                                 wrote ? "WROTE" : "*** WROTE NOTHING, IGNORE THE TIME ***");
                }
            } else {
                spdlog::error("DyeGpu: task 2 view creation failed at {}x{}.", a_dim, a_dim);
            }
            if (probe) { probe->Release(); }
            if (uav) { uav->Release(); }
            if (srv) { srv->Release(); }
            dst->Release();
            src->Release();
        }

        // ---- task 2: THE NUMBER --------------------------------------------
        //
        // ⚠ THIS IS THE STOPPING GATE FOR THE WHOLE SPIKE. If a tint of a real
        // sized diffuse does not fit a live preview, rung 3 closes a second
        // time and nothing below task 2 needs building.
        //
        // ⚠ IT MEASURES TEXTURES THIS MODULE OWNS, NOT A LIVE DIFFUSE, AND THAT
        // IS THE POINT OF DOING IT FIRST. Reaching a worn shape's diffuse means
        // going through BSGraphics::Texture, which CommonLib only forward
        // declares, so it is reverse engineering. The cost of the pass does not
        // depend on where the source came from - it is bound by the size and
        // format, both of which are reproduced exactly here - so the gate can be
        // answered before any of that work is done. A bad number here means the
        // RE was never worth starting.
        //
        // The ladder of sizes is the mip-capping lever the spec named: if 4096
        // is too slow and 2048 is not, tinting a capped mip is a real answer
        // rather than a compromise nobody measured.
        void RunTask2() {
            auto* const device = OS::Gpu::Device();
            auto* const ctx    = OS::Gpu::Context();
            if (!device || !ctx) {
                spdlog::error("DyeGpu: no device/context, task 2 cannot run.");
                return;
            }

            // BC7 needs 11_0; the same check task 1 made, kept because this can
            // be run on its own.
            if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
                spdlog::error("DyeGpu: feature level below 11_0, task 2 cannot run.");
                return;
            }

            auto* const cs = Compile(device, kTintShader, sizeof(kTintShader) - 1, "DyeGpuTint");
            if (!cs) {
                return;
            }
            spdlog::info("DyeGpu: ===== TASK 2, the tint pass. Times are GPU timestamps, "
                         "warm-up excluded. =====");
            for (const UINT dim : { 4096u, 2048u, 1024u }) {
                MeasureTint(device, ctx, cs, dim);
            }
            spdlog::info("DyeGpu: ===== TASK 2 done. The BC7 ENCODE half is NOT measured "
                         "here and is still open. =====");
            cs->Release();
        }

        // ---- task 1 ------------------------------------------------------
        void RunTask1() {
            // The same two objects ImGuiOverlay::EnsureInit reads, through the
            // one seam that turns the library's D3D types into the SDK's.
            auto* const device = OS::Gpu::Device();
            auto* const ctx    = OS::Gpu::Context();
            if (!device || !ctx) {
                spdlog::error("DyeGpu: device={} context={}, task 1 cannot run.",
                              static_cast<void*>(device), static_cast<void*>(ctx));
                return;
            }

            const auto level = device->GetFeatureLevel();
            spdlog::info("DyeGpu: device={} context={} featureLevel=0x{:04X}",
                         static_cast<void*>(device), static_cast<void*>(ctx),
                         static_cast<unsigned>(level));
            // Compute shader 5.0 needs 11_0. Skyrim runs 11_0, so this is a
            // recorded fact rather than an expected branch, but a spike that
            // assumes its platform is a spike that lies later.
            if (level < D3D_FEATURE_LEVEL_11_0) {
                spdlog::error("DyeGpu: feature level below 11_0, compute is unavailable.");
                return;
            }

            // ---- the target and its staging mirror ------------------------
            D3D11_TEXTURE2D_DESC td{};
            td.Width            = kProbeDim;
            td.Height           = kProbeDim;
            td.MipLevels        = 1;
            td.ArraySize        = 1;
            td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage            = D3D11_USAGE_DEFAULT;
            td.BindFlags        = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

            ID3D11Texture2D* target{ nullptr };
            if (FAILED(device->CreateTexture2D(&td, nullptr, &target)) || !target) {
                spdlog::error("DyeGpu: CreateTexture2D(target) failed.");
                return;
            }

            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage                = D3D11_USAGE_STAGING;
            sd.BindFlags            = 0;
            sd.CPUAccessFlags       = D3D11_CPU_ACCESS_READ;

            ID3D11Texture2D* staging{ nullptr };
            if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging)) || !staging) {
                spdlog::error("DyeGpu: CreateTexture2D(staging) failed.");
                target->Release();
                return;
            }

            // ---- ⚠ THE NEGATIVE CONTROL, AND IT RUNS FIRST ----------------
            //
            // Read the three pixels BEFORE anything is dispatched. If they
            // already carry the pattern then the readback is reading the wrong
            // resource or the pattern is being produced by something other than
            // our shader, and every number after this point would be worthless.
            // The previous spike ran a control on every rung for this reason
            // and it caught a real defect.
            //
            // A fresh DEFAULT texture with no initial data is formally
            // undefined, so "all zero" is the expected reading but is NOT the
            // pass condition. The pass condition is only that it does not match
            // the pattern.
            std::array<Pixel, 3> before{};
            const bool           beforeOk = ReadSamples(ctx, target, staging, before);
            LogSamples("PRE ", before, beforeOk);

            // ---- compile ---------------------------------------------------
            ID3DBlob* code{ nullptr };
            ID3DBlob* errors{ nullptr };
            const HRESULT hrc =
                ::D3DCompile(kProbeShader, sizeof(kProbeShader) - 1, "DyeGpuProbe", nullptr,
                             nullptr, "main", "cs_5_0", 0, 0, &code, &errors);
            if (FAILED(hrc) || !code) {
                spdlog::error("DyeGpu: D3DCompile failed hr=0x{:08X} msg='{}'",
                              static_cast<unsigned>(hrc),
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                if (errors) {
                    errors->Release();
                }
                staging->Release();
                target->Release();
                return;
            }
            if (errors) {
                errors->Release();
            }

            ID3D11ComputeShader* cs{ nullptr };
            const HRESULT hrs = device->CreateComputeShader(code->GetBufferPointer(),
                                                            code->GetBufferSize(), nullptr, &cs);
            code->Release();
            if (FAILED(hrs) || !cs) {
                spdlog::error("DyeGpu: CreateComputeShader failed hr=0x{:08X}",
                              static_cast<unsigned>(hrs));
                staging->Release();
                target->Release();
                return;
            }

            ID3D11UnorderedAccessView* uav{ nullptr };
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format        = td.Format;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(device->CreateUnorderedAccessView(target, &ud, &uav)) || !uav) {
                spdlog::error("DyeGpu: CreateUnorderedAccessView failed.");
                cs->Release();
                staging->Release();
                target->Release();
                return;
            }

            // ---- the timestamp instrument, proven here so task 2 can trust it
            //
            // ⚠ MEASURED HERE ON A CASE TOO SMALL TO CARE ABOUT, WHICH IS THE
            // POINT. Task 2's whole deliverable is a number in milliseconds, and
            // an instrument first used on the measurement that matters is an
            // instrument nobody has checked. A 64² dispatch should read as
            // very close to zero; anything else means the query pair is wrong
            // and task 2 would have reported nonsense.
            ID3D11Query*      disjointQ{ nullptr };
            ID3D11Query*      startQ{ nullptr };
            ID3D11Query*      endQ{ nullptr };
            D3D11_QUERY_DESC  qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            device->CreateQuery(&qd, &disjointQ);
            qd.Query = D3D11_QUERY_TIMESTAMP;
            device->CreateQuery(&qd, &startQ);
            device->CreateQuery(&qd, &endQ);
            const bool timed = disjointQ && startQ && endQ;

            // ---- dispatch --------------------------------------------------
            ctx->CSSetShader(cs, nullptr, 0);
            ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

            if (timed) {
                ctx->Begin(disjointQ);
                ctx->End(startQ);
            }
            ctx->Dispatch(kProbeDim / kThreadGroup, kProbeDim / kThreadGroup, 1);
            if (timed) {
                ctx->End(endQ);
                ctx->End(disjointQ);
            }

            // ⚠ UNBIND BEFORE ANYTHING ELSE DRAWS. Leaving our UAV bound to the
            // compute stage holds a reference to the texture and leaves the
            // pipeline in a state the game did not set. The game re-binds what
            // it uses, but it has no reason to touch a compute stage it never
            // uses, so nothing else would ever clear this.
            ID3D11UnorderedAccessView* const nullUav[1]{ nullptr };
            ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            ctx->CSSetShader(nullptr, nullptr, 0);

            // ---- the reading that decides task 1 ---------------------------
            std::array<Pixel, 3> after{};
            const bool           afterOk = ReadSamples(ctx, target, staging, after);
            LogSamples("POST", after, afterOk);

            // ---- the verdict, computed rather than eyeballed ----------------
            bool matched = afterOk;
            for (std::size_t i = 0; i < kSamples.size() && matched; ++i) {
                matched = after[i].r == static_cast<std::uint8_t>(kSamples[i].x) &&
                          after[i].g == static_cast<std::uint8_t>(kSamples[i].y) &&
                          after[i].b == 191u && after[i].a == 255u;
            }
            bool controlHeld = beforeOk;
            if (beforeOk) {
                // The control passes as long as the pre-dispatch read is not
                // already the pattern.
                bool preMatched = true;
                for (std::size_t i = 0; i < kSamples.size() && preMatched; ++i) {
                    preMatched = before[i].r == static_cast<std::uint8_t>(kSamples[i].x) &&
                                 before[i].g == static_cast<std::uint8_t>(kSamples[i].y) &&
                                 before[i].b == 191u && before[i].a == 255u;
                }
                controlHeld = !preMatched;
            }

            if (timed) {
                // Spin until the disjoint query retires. A probe may block; the
                // shipped path never would.
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
                while (ctx->GetData(disjointQ, &dj, sizeof(dj), 0) != S_OK) {
                }
                std::uint64_t t0{ 0 };
                std::uint64_t t1{ 0 };
                while (ctx->GetData(startQ, &t0, sizeof(t0), 0) != S_OK) {
                }
                while (ctx->GetData(endQ, &t1, sizeof(t1), 0) != S_OK) {
                }
                if (dj.Disjoint) {
                    spdlog::warn("DyeGpu: timestamp DISJOINT, the timing is not trustworthy.");
                } else if (dj.Frequency) {
                    const double ms = 1000.0 * static_cast<double>(t1 - t0) /
                                      static_cast<double>(dj.Frequency);
                    spdlog::info("DyeGpu: dispatch {}x{} took {:.4f} ms (freq {} Hz).", kProbeDim,
                                 kProbeDim, ms, dj.Frequency);
                }
            }

            spdlog::info("DyeGpu: ===== TASK 1 VERDICT: dispatch={} control={} =====",
                         matched ? "RAN" : "DID NOT RUN",
                         controlHeld ? "HELD" : "⚠ BROKEN, ignore the result above");

            if (disjointQ) {
                disjointQ->Release();
            }
            if (startQ) {
                startQ->Release();
            }
            if (endQ) {
                endQ->Release();
            }
            uav->Release();
            cs->Release();
            staging->Release();
            target->Release();
        }

        // ================================================================
        // TASK 3: reach a WORN shape's real diffuse, tint it, put the copy
        // back, and get a garment on screen recoloured with its envmap
        // intact.
        // ================================================================
        //
        // ⚠ RUNG 3 IS RUNG 1 WITH A DIFFERENT WRITE. `SwapToSameFeatureTint`
        // in OutfitDye.cpp already clones a material at its OWN feature through
        // virtual Create + CopyMembers, takes the reference the cache release
        // would otherwise destroy, hands it to SetMaterial, deletes its own copy
        // and calls DoClearRenderPasses. That machinery shipped and its negative
        // control PASSED in the field: it moves nothing on its own. Rung 1 wrote
        // specularColor and was refused because a highlight tint is too weak.
        // This writes `diffuseTexture` instead and nothing else about the swap
        // changes. Every ⚠ in that function applies here verbatim and each one
        // cost a field run.
        //
        // ---- THE LAYOUT OF BSGraphics::Texture, WHICH WAS THE ONLY OPEN
        // ---- RE QUESTION IN THE SPEC. Established 2026-08-05.
        //
        // `NiSourceTexture::rendererTexture` is a `BSGraphics::Texture*` at
        // +0x48 and CommonLib only FORWARD DECLARES that class
        // (RE/N/NiSourceTexture.h:10), so the spec called its layout the one
        // thing blocking both halves of this task. It is neither unknown nor
        // expensive: CommonLib carries the same 0x28 bytes under a SECOND NAME,
        // `NiTexture::RendererData` (RE/N/NiTexture.h:67), and three shipped
        // PDBs in the dev profile agree with it on the fields that matter.
        //
        //   offset  NiTexture::RendererData   CommunityShaders.pdb   Mu/HDLocalMap
        //   0x00    ID3D11Texture2D* texture  texture                texture
        //   0x08    unk08                     UAV                    unk08
        //   0x10    ID3D11SRV* resourceView   resourceView           resourceView
        //   0x18    width  (u16)              height (u16)           unk18
        //   0x1A    height (u16)              width  (u16)           -
        //   0x1C    unk1C{1}                  mips   (u8)            -
        //   0x1D    unk1D{0x1C}               format (u8)            -
        //   0x20    unk20{1}                  refCount               unk20
        //   0x24    unk24{0x130012}           pad24                  pad24
        //
        // Three independent projects, all sized 0x28, all agreeing on +0x00 and
        // +0x10, which are the only two this task reads. ⚠ THEY DISAGREE AT
        // 0x18/0x1A: CommonLib says width then height, Community Shaders says
        // height then width. That is logged below against the D3D desc rather
        // than assumed, and it is harmless either way here because our
        // destination is the same size as the source, so a square texture makes
        // the two answers identical.
        //
        // ⚠ AND THE LAYOUT IS PROVED AT RUNTIME BEFORE IT IS USED, which is the
        // negative control this task cannot do without. `SRV->GetResource()`
        // must hand back the very ID3D11Texture2D read from +0x00. Nothing but
        // a correct pair of offsets can produce that, and a mismatch aborts the
        // task rather than dyeing something on a wrong pointer.
        using RendererData = OS::RendererData;  // the SDK-typed mirror, RendererData.h

        // ---- the dye pass ------------------------------------------------
        //
        // ⚠ THE COLOUR SPACE LOOKED LIKE THE TRAP IN THIS SHADER AND IT IS
        // NOT ONE, WHICH IS A MEASUREMENT RATHER THAN A GUESS THAT SURVIVED.
        // This was written expecting BC7_UNORM_SRGB, on the reasoning that a
        // hardware decode also linearises and an RGBA8_UNORM destination fed
        // back to a sampler expecting sRGB renders washed out. The 2026-08-05
        // run measured every worn diffuse on a whole dressed character as
        // srgb=FALSE: DXGI format 71 (BC1_UNORM) on the armour and 98
        // (BC7_UNORM) on the head. Skyrim types its diffuses plainly and does
        // whatever gamma work it does in its own shaders.
        //
        // The sRGB branch stays because it costs one bit and one lerp, and
        // because "no _SRGB diffuse on THIS outfit" is not "no _SRGB diffuse in
        // any load order". What matters is that on the measured path it never
        // fires, and the proof is arithmetic: the run's COPY texel (82,56,41)
        // and TINT texel (40,25,65) satisfy D*D + 2*T*D*(1-D) on all three
        // channels to within a rounding step. A hidden decode or encode
        // anywhere in the chain could not leave that identity standing.
        //
        // The destination is R8G8B8A8_TYPELESS regardless, because a UAV cannot
        // be created on an _SRGB format at all, so the two-view arrangement is
        // what makes the sRGB case expressible even though it is not the case
        // we are in.
        //
        // ⚠ MODE BIT 1 IS THE CONTROL AND IT IS NOT DECORATION. It skips the
        // tint and copies the source through untouched. Three readings of one
        // texel then separate three different failures that all look the same
        // from a screenshot: zero before any dispatch, non-zero after the copy
        // (so the game's own diffuse really was read), and different again
        // after the tint (so the arithmetic really ran). Task 2's first numbers
        // died for want of exactly this.
        constexpr char kDyeShader[] = R"(
Texture2D<float4>   Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

cbuffer Params : register(b0)
{
    float3 gTint;
    uint   gMode;   // bit 0: encode sRGB on write.  bit 1: control, no tint
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Dst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) { return; }

    float4 d = Src.Load(int3(id.xy, 0));
    float3 o = d.rgb;
    if ((gMode & 2u) == 0u) {
        o = d.rgb * d.rgb + 2.0 * gTint * d.rgb * (1.0 - d.rgb);
    }
    if ((gMode & 1u) != 0u) {
        float3 lo = o * 12.92;
        float3 hi = 1.055 * pow(max(o, 1e-5), 1.0 / 2.4) - 0.055;
        o = lerp(hi, lo, step(o, 0.0031308));
    }
    Dst[id.xy] = float4(o, d.a);
}
)";

        struct DyeParams {
            float         tint[3]{ 0.0f, 0.0f, 0.0f };
            std::uint32_t mode{ 0 };
        };
        static_assert(sizeof(DyeParams) == 16);  // one constant register

        // ⚠ A SATURATED COLOUR ON PURPOSE. The deliverable is a picture, and a
        // subtle shift is the one result a screenshot cannot settle. The math
        // is the game's own overlay, D*D + 2*T*D*(1-D), so 0.5 is the identity;
        // this pushes red and green down and blue up hard.
        constexpr DyeParams kTint{ { 0.12f, 0.14f, 0.85f }, 0 };

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

        // OutfitDye.cpp has one of these in its own anonymous namespace. Not
        // shared, because this whole module is meant to be deleted with the
        // spike and a header shared with it would outlive the probe.
        const char* FeatureNameOf(RE::BSShaderMaterial::Feature a_feature) {
            using F = RE::BSShaderMaterial::Feature;
            switch (a_feature) {
                case F::kDefault:        return "Default";
                case F::kEnvironmentMap: return "EnvironmentMap";
                case F::kGlowMap:        return "GlowMap";
                case F::kParallax:       return "Parallax";
                default:                 return "other";
            }
        }

        // What the walk found and what makes it a candidate.
        struct Cand {
            RE::BSGeometry*                   geom{ nullptr };
            RE::BSLightingShaderProperty*     prop{ nullptr };
            RE::BSLightingShaderMaterialBase* mat{ nullptr };
            RE::NiSourceTexture*              diffuse{ nullptr };
            RendererData*                     rd{ nullptr };
            ID3D11Texture2D*                  tex{ nullptr };
            ID3D11ShaderResourceView*         srv{ nullptr };
            D3D11_TEXTURE2D_DESC              desc{};
            RE::BSShaderMaterial::Feature     feature{};
            bool                              srvMatchesTexture{ false };
        };

        // ⚠ EVERYTHING HERE IS HELD FOR THE PROCESS AND NOTHING IS EVER
        // RELEASED, AND THAT IS THE DESIGN RATHER THAN A LEAK LEFT IN.
        //
        // `original` is the material the swap displaced, IncRef'd for the same
        // reason SwapToSameFeatureTint does it: SetMaterial runs the displaced
        // material through the cache's release, which destroys it at zero, and
        // an armour material interned for one shape normally has a refcount of
        // exactly 1. Task 3 never restores - the picture is the deliverable and
        // survival is task 4 - so the reference is simply never given back.
        //
        // `tinted` is the forged NiSourceTexture, and holding one reference to
        // it FOREVER is what makes the forgery safe. The interned material
        // holds its own reference and will DecRef when it is eventually
        // released; ours keeps the count off zero, so the engine's
        // ~NiSourceTexture never runs on an object that was never linked into
        // NiTexture's global list and never had a BSResource::Stream.
        struct Task3Held {
            RE::NiPointer<RE::NiSourceTexture> tinted;
            RE::BSShaderMaterial*              original{ nullptr };
            ID3D11Texture2D*                   tex{ nullptr };
            ID3D11ShaderResourceView*          srv{ nullptr };
        };
        Task3Held g_held;

        // Copies one texel of a_src's mip 0 into a_staging and reads it back.
        // a_staging must be 1x1 and share a_src's typeless family.
        bool ReadTexel(ID3D11DeviceContext* a_ctx, ID3D11Texture2D* a_staging,
                       ID3D11Texture2D* a_src, UINT a_x, UINT a_y, Pixel& a_out) {
            const D3D11_BOX box{ a_x, a_y, 0, a_x + 1, a_y + 1, 1 };
            a_ctx->CopySubresourceRegion(a_staging, 0, 0, 0, 0, a_src, 0, &box);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(a_ctx->Map(a_staging, 0, D3D11_MAP_READ, 0, &m))) {
                return false;
            }
            const auto* const p = static_cast<const std::uint8_t*>(m.pData);
            a_out = Pixel{ p[0], p[1], p[2], p[3] };
            a_ctx->Unmap(a_staging, 0);
            return true;
        }

        void DispatchDye(ID3D11DeviceContext* a_ctx, ID3D11ComputeShader* a_cs,
                         ID3D11Buffer* a_cb, ID3D11ShaderResourceView* a_srv,
                         ID3D11UnorderedAccessView* a_uav, const DyeParams& a_params, UINT a_w,
                         UINT a_h) {
            a_ctx->UpdateSubresource(a_cb, 0, nullptr, &a_params, 0, 0);
            a_ctx->CSSetShader(a_cs, nullptr, 0);
            a_ctx->CSSetConstantBuffers(0, 1, &a_cb);
            a_ctx->CSSetShaderResources(0, 1, &a_srv);
            a_ctx->CSSetUnorderedAccessViews(0, 1, &a_uav, nullptr);
            a_ctx->Dispatch((a_w + kThreadGroup - 1) / kThreadGroup,
                            (a_h + kThreadGroup - 1) / kThreadGroup, 1);

            // ⚠ UNBOUND EVERY TIME, not just at the end. Leaving the game's own
            // diffuse SRV and our UAV bound to the compute stage holds
            // references and leaves the pipeline in a state the game did not
            // set. The game re-binds what it uses, but it has no reason to
            // touch a compute stage it never uses, so nothing else would ever
            // clear this.
            ID3D11UnorderedAccessView* const nullUav[1]{ nullptr };
            ID3D11ShaderResourceView* const  nullSrv[1]{ nullptr };
            ID3D11Buffer* const              nullCb[1]{ nullptr };
            a_ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            a_ctx->CSSetShaderResources(0, 1, nullSrv);
            a_ctx->CSSetConstantBuffers(0, 1, nullCb);
            a_ctx->CSSetShader(nullptr, nullptr, 0);
        }

        // Builds an NiSourceTexture that owns nothing the engine loaded, by
        // copying a live one of the same class and overriding the fields whose
        // meaning is known.
        //
        // ⚠ WHY A FORGERY RATHER THAN AN ENGINE CALL. The alternative the spec
        // named is substituting inside the EXISTING NiSourceTexture, and that
        // is wrong for the shipped shape of this feature rather than merely
        // untidy: one NiSourceTexture is shared by every shape using that
        // texture path - the census found the Abyss set serving many shapes off
        // 4 atlases - so writing into it dyes every actor wearing anything that
        // shares the file. Per-material isolation needs a per-recipe texture
        // object, and the material clone is where it goes.
        //
        // ⚠ THE BYTE COPY IS THE EVIDENCE, NOT A SHORTCUT. Every field this
        // function does not understand - flags at +0x50, formatPrefs, the two
        // words at +0x28 - keeps a value the engine itself produced for a
        // texture of exactly this class, and the vtable pointer comes from a
        // live instance rather than from a relocation we would have to trust.
        // Only the five fields whose meaning IS established get overridden.
        RE::NiSourceTexture* ForgeSourceTexture(const RE::NiSourceTexture* a_model,
                                                RendererData* a_rd, const char* a_name) {
            void* const mem = RE::malloc(sizeof(RE::NiSourceTexture));
            if (!mem) {
                return nullptr;
            }
            std::memcpy(mem, static_cast<const void*>(a_model), sizeof(RE::NiSourceTexture));
            auto* const t = static_cast<RE::NiSourceTexture*>(mem);

            // The model's count came with the bytes and means nothing here. The
            // NiPointer the caller takes brings this to 1.
            t->_refCount = 0;
            // ⚠ OUT OF NiTexture's GLOBAL LIST ON PURPOSE. The engine walks
            // that list to release textures on a device event; an object that
            // was never linked into it is one the walk cannot reach, which is
            // safer than being half-linked, and the copied prev/next would
            // otherwise point our forgery into the middle of a list that does
            // not know about it.
            t->prev = nullptr;
            t->next = nullptr;
            // Nothing streams this texture off disk. The model's stream belongs
            // to the model.
            t->resourceStream = nullptr;
            t->rendererTexture = reinterpret_cast<RE::BSGraphics::Texture*>(a_rd);
            // ⚠ ZERO BEFORE ASSIGNING. `name` is a BSFixedString and the memcpy
            // copied a string-pool pointer this object never acquired a
            // reference to. Assigning over it would release a reference we do
            // not own; zeroing first makes the operator's try_release a no-op
            // and the acquire real.
            std::memset(&t->name, 0, sizeof(t->name));
            t->name = a_name;
            return t;
        }

        // Everything the render thread prepared, handed to the game thread.
        struct Task3Swap {
            RE::NiPointer<RE::BSGeometry>               geom;
            RE::NiPointer<RE::BSLightingShaderProperty> prop;
            RE::NiPointer<RE::NiSourceTexture>          tinted;
        };

        // ⚠ THE MATERIAL SWAP GOES ON THE GAME THREAD AND THE DISPATCH DOES
        // NOT, AND NEITHER HALF MAY MOVE. The immediate context is not thread
        // safe and tasks 1 and 2 established the render thread as the only
        // legal place to touch it; the shipped dye does every material swap
        // from the task interface and has never faulted doing so. So the
        // pixels are made where the context lives and the pointer work is
        // queued to where the engine expects it.
        void SwapDiffuseOnGameThread(Task3Swap a_swap) {
            auto* const prop = a_swap.prop.get();
            auto* const geom = a_swap.geom.get();
            if (!prop || !geom || !a_swap.tinted) {
                spdlog::error("DyeGpu: task 3 lost its target across the thread hop.");
                return;
            }
            // ⚠ STALENESS TEST, NOT DEFENSIVENESS. A 3D rebuild between the
            // dispatch and this task builds a new partClone and leaves the old
            // property orphaned; swapping through it would paint geometry
            // nothing is drawing and read as a silent no.
            auto* const live = netimmerse_cast<RE::BSLightingShaderProperty*>(
                geom->GetGeometryRuntimeData()
                    .shaderProperty
                    .get());
            if (live != prop) {
                spdlog::warn("DyeGpu: task 3 target went stale before the swap "
                             "(prop {} is now {}). Nothing was written.",
                             static_cast<void*>(prop), static_cast<void*>(live));
                return;
            }
            if (!prop->material ||
                prop->material->GetType() != RE::BSShaderMaterial::Type::kLighting) {
                spdlog::warn("DyeGpu: task 3 target is no longer a lighting material.");
                return;
            }

            auto* const src   = static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);
            // ⚠ virtual Create + CopyMembers, NEVER CopyBaseMembers. Create is
            // vtable slot 01 and CopyMembers slot 02, every material overrides
            // both, and this is the pair the engine's own material cache uses on
            // a miss. CopyBaseMembers copies the base subobject only, which on
            // an Envmap material leaves envTexture at 0xA0 unset - the very
            // reflection this rung exists to preserve.
            auto* const fresh = static_cast<RE::BSLightingShaderMaterialBase*>(src->Create());
            if (!fresh) {
                spdlog::error("DyeGpu: task 3 could not clone the material.");
                return;
            }
            fresh->CopyMembers(src);

            const auto* const wasDiffuse = fresh->diffuseTexture.get();
            // THE WRITE. Rung 1 put a colour in specularColor here; this puts a
            // whole texture in diffuseTexture and changes nothing else.
            fresh->diffuseTexture = a_swap.tinted;

            // ⚠ MANDATORY AND IT MUST PRECEDE SetMaterial. SetMaterial runs the
            // displaced material through the material cache's release, which
            // DecRefs and destroys at zero, and an armour material interned for
            // one shape normally has a refcount of exactly 1.
            g_held.original = prop->material;
            g_held.original->IncRef();

            // ⚠ true, THE UNIQUE PATH, DELIBERATELY. Acquire computes a CRC over
            // the material and probes a hash table; a clone differing only in
            // one texture pointer could plausibly collide with the material it
            // was cloned from, which would hand back the untinted original and
            // make this read as a silent no.
            prop->SetMaterial(fresh, true);
            // ⚠ NOT A DOUBLE FREE. SetMaterial is not a store: acquire clones
            // and interns the clone, so `fresh` is always copied from and then
            // dropped, and nothing in the engine releases it. The destructor is
            // virtual, so this runs the engine's deleting destructor and drops
            // the texture references CopyMembers took - including the one it
            // took on our forgery, which g_held.tinted keeps off zero.
            delete fresh;

            // ⚠ NO FLAG IS TOUCHED AND NO SETUP CALL IS MADE. A same-feature
            // clone has no offset collision to clear, and SetupGeometry /
            // FinishSetupGeometry WERE THE SHINE - they mutate the property one
            // way only and nothing puts back what they clear. DoClearRenderPasses
            // is the only engine call needed: the pass list is cached on the
            // property and GetRenderPasses re-picks the technique off the
            // invalidated lastRenderPassState.
            prop->DoClearRenderPasses();

            spdlog::info("DyeGpu: ===== TASK 3 SWAPPED '{}' feature={} : diffuse '{}' -> "
                         "'{}'. Look at this shape. =====",
                         geom->name.c_str(),
                         static_cast<int>(src->GetFeature()),
                         wasDiffuse ? wasDiffuse->name.c_str() : "<none>",
                         a_swap.tinted->name.c_str());
        }

        // Returns false while the task has not been able to run yet, so the
        // caller can try again on a later frame. Returns true once it has
        // reached a verdict, pass or fail.
        bool RunTask3() {
            auto* const device = OS::Gpu::Device();
            auto* const ctx    = OS::Gpu::Context();
            if (!device || !ctx) {
                return false;
            }

            auto* const pc = RE::PlayerCharacter::GetSingleton();
            // ⚠ Get3D(false), NEVER Get3D(). For the PLAYER, Get3D() answers
            // with whichever body the camera is using, and in the menus that is
            // the FIRST-PERSON skeleton - culled, and not the garment anyone can
            // see. Measured on this project 2026-08-02.
            auto* const root = pc ? pc->Get3D(false) : nullptr;
            if (!root) {
                return false;  // no third-person 3D yet; try again next frame
            }

            // ---- the walk ------------------------------------------------
            std::vector<Cand> cands;
            RE::BSVisit::TraverseScenegraphGeometries(
                root, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                        a_geom->GetGeometryRuntimeData()
                            .shaderProperty
                            .get());
                    if (!prop || !prop->material ||
                        prop->material->GetType() != RE::BSShaderMaterial::Type::kLighting) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const auto feature = prop->material->GetFeature();
                    using F            = RE::BSShaderMaterial::Feature;
                    // ⚠ THE SAME TWO FEATURES THE SHIPPED DYE ACCEPTS, and the
                    // narrowing matters: kFaceGen and kFaceGenRGBTint are the
                    // player's own skin and kHairTint is her hair. Dyeing a face
                    // blue would be a picture of the wrong thing.
                    if (feature != F::kDefault && feature != F::kEnvironmentMap) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }

                    auto* const mat =
                        static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);
                    auto* const diffuse = mat->diffuseTexture.get();
                    if (!diffuse) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    auto* const rdata =
                        reinterpret_cast<RendererData*>(diffuse->rendererTexture);
                    if (!rdata || !rdata->texture || !rdata->resourceView) {
                        spdlog::info("DyeGpu: task 3 skips '{}' - diffuse '{}' has no live "
                                     "renderer texture (rd={} tex={} srv={}).",
                                     a_geom->name.c_str(), diffuse->name.c_str(),
                                     static_cast<void*>(rdata),
                                     rdata ? static_cast<void*>(rdata->texture) : nullptr,
                                     rdata ? static_cast<void*>(rdata->resourceView) : nullptr);
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }

                    Cand c;
                    c.geom    = a_geom;
                    c.prop    = prop;
                    c.mat     = mat;
                    c.diffuse = diffuse;
                    c.rd      = rdata;
                    c.tex     = rdata->texture;
                    c.srv     = rdata->resourceView;
                    c.feature = feature;
                    c.tex->GetDesc(&c.desc);

                    // ⚠ THE CONTROL ON THE WHOLE RE CLAIM, PER SHAPE. If +0x00
                    // and +0x10 are the texture and its shader-resource view,
                    // then the view must name that exact texture as its
                    // resource. Nothing but a correct pair of offsets produces
                    // that, and two garbage pointers cannot produce it by luck.
                    ID3D11Resource* res{ nullptr };
                    c.srv->GetResource(&res);  // AddRefs
                    c.srvMatchesTexture = res == static_cast<ID3D11Resource*>(c.tex);
                    if (res) {
                        res->Release();
                    }
                    cands.push_back(c);
                    return RE::BSVisit::BSVisitControl::kContinue;
                });

            if (cands.empty()) {
                return false;  // undressed, or the biped has not attached yet
            }

            spdlog::info("DyeGpu: ===== TASK 3, {} dyeable shape(s) on the third-person "
                         "biped =====", cands.size());
            std::size_t matched = 0;
            for (const auto& c : cands) {
                // ⚠ THE 0x18/0x1A DISAGREEMENT, SETTLED OR DECLARED
                // INCONCLUSIVE RATHER THAN ASSUMED. CommonLib reads those two
                // u16s as width-then-height and Community Shaders' PDB as
                // height-then-width. A square texture cannot tell them apart,
                // and 4096² diffuses are the common case, so most lines here
                // will say so honestly.
                const char* order = "SQUARE, inconclusive";
                if (c.desc.Width != c.desc.Height) {
                    order = (c.rd->width == static_cast<std::uint16_t>(c.desc.Width))
                                ? "width@0x18 (CommonLib)"
                                : "height@0x18 (CommunityShaders)";
                }
                matched += c.srvMatchesTexture ? 1 : 0;
                spdlog::info("DyeGpu:   '{}' feature={} {}x{} mips={} fmt={} srgb={} "
                             "rd(0x18,0x1A)=({},{}) {} srv->GetResource {} diffuse='{}'",
                             c.geom->name.c_str(), FeatureNameOf(c.feature), c.desc.Width,
                             c.desc.Height, c.desc.MipLevels, static_cast<int>(c.desc.Format),
                             IsSrgb(c.desc.Format), c.rd->width, c.rd->height, order,
                             c.srvMatchesTexture ? "MATCHES  ✅" : "MISMATCH ❌",
                             c.diffuse->name.c_str());
            }

            if (matched != cands.size()) {
                spdlog::error("DyeGpu: ===== TASK 3 ABORTS. The BSGraphics::Texture layout "
                              "control FAILED on {} of {} shape(s): a shader-resource view "
                              "read at +0x10 did not name the texture read at +0x00. The "
                              "offsets are wrong and nothing below would mean anything. "
                              "=====", cands.size() - matched, cands.size());
                return true;
            }

            // ---- pick one ------------------------------------------------
            //
            // ⚠ ENVMAP FIRST, AND THAT IS THE POINT OF THE WHOLE RUNG. The
            // shipped swap changes GetFeature() and loses the reflection;
            // "envmap intact" is the claim under test, so a shape that HAS one
            // is the only shape that can test it. Largest texture breaks the
            // tie because the biggest diffuse is almost always the biggest
            // garment, and a buckle recoloured in a screenshot proves nothing.
            const Cand* pick = nullptr;
            for (const auto& c : cands) {
                const bool better =
                    !pick ||
                    (c.feature == RE::BSShaderMaterial::Feature::kEnvironmentMap &&
                     pick->feature != RE::BSShaderMaterial::Feature::kEnvironmentMap) ||
                    (c.feature == pick->feature &&
                     static_cast<std::uint64_t>(c.desc.Width) * c.desc.Height >
                         static_cast<std::uint64_t>(pick->desc.Width) * pick->desc.Height);
                if (better) {
                    pick = &c;
                }
            }
            const Cand tgt = *pick;
            spdlog::info("DyeGpu: task 3 picked '{}' feature={} {}x{} diffuse='{}'",
                         tgt.geom->name.c_str(), FeatureNameOf(tgt.feature), tgt.desc.Width,
                         tgt.desc.Height, tgt.diffuse->name.c_str());

            auto* const cs = Compile(device, kDyeShader, sizeof(kDyeShader) - 1, "DyeGpuDye");
            if (!cs) {
                return true;
            }

            // ---- the destination -----------------------------------------
            //
            // ⚠ TYPELESS WITH TWO VIEWS. A UAV cannot be created on an _SRGB
            // format, and the engine has to read one back if the source was
            // sRGB, so the resource is typeless and carries a UNORM UAV to
            // write through and an SRGB shader-resource view to be read
            // through. MipLevels 0 asks D3D for the full chain; the shader
            // writes mip 0 and GenerateMips fills the rest, which the spec
            // predicted is cheaper than tinting the chain.
            const bool           srgb = IsSrgb(tgt.desc.Format);
            D3D11_TEXTURE2D_DESC dd{};
            dd.Width            = tgt.desc.Width;
            dd.Height           = tgt.desc.Height;
            dd.MipLevels        = 0;
            dd.ArraySize        = 1;
            dd.Format           = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            dd.SampleDesc.Count = 1;
            dd.Usage            = D3D11_USAGE_DEFAULT;
            dd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS |
                           D3D11_BIND_RENDER_TARGET;
            dd.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;

            ID3D11Texture2D* dst{ nullptr };
            if (FAILED(device->CreateTexture2D(&dd, nullptr, &dst)) || !dst) {
                spdlog::error("DyeGpu: task 3 could not create a {}x{} destination.",
                              dd.Width, dd.Height);
                cs->Release();
                return true;
            }

            D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
            svd.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            svd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            svd.Texture2D.MostDetailedMip = 0;
            svd.Texture2D.MipLevels       = static_cast<UINT>(-1);

            D3D11_UNORDERED_ACCESS_VIEW_DESC uvd{};
            uvd.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
            uvd.ViewDimension       = D3D11_UAV_DIMENSION_TEXTURE2D;
            uvd.Texture2D.MipSlice  = 0;

            ID3D11ShaderResourceView*  ourSrv{ nullptr };
            ID3D11UnorderedAccessView* uav{ nullptr };
            ID3D11Buffer*              cb{ nullptr };
            ID3D11Texture2D*           staging{ nullptr };
            device->CreateShaderResourceView(dst, &svd, &ourSrv);
            device->CreateUnorderedAccessView(dst, &uvd, &uav);

            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(DyeParams);
            bd.Usage     = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            device->CreateBuffer(&bd, nullptr, &cb);

            D3D11_TEXTURE2D_DESC pd{};
            pd.Width            = 1;
            pd.Height           = 1;
            pd.MipLevels        = 1;
            pd.ArraySize        = 1;
            pd.Format           = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            pd.SampleDesc.Count = 1;
            pd.Usage            = D3D11_USAGE_STAGING;
            pd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
            device->CreateTexture2D(&pd, nullptr, &staging);

            const auto release = [&] {
                if (staging) { staging->Release(); }
                if (cb) { cb->Release(); }
                if (uav) { uav->Release(); }
                cs->Release();
            };

            if (!ourSrv || !uav || !cb || !staging) {
                spdlog::error("DyeGpu: task 3 view/buffer creation failed "
                              "(srv={} uav={} cb={} staging={}).",
                              static_cast<void*>(ourSrv), static_cast<void*>(uav),
                              static_cast<void*>(cb), static_cast<void*>(staging));
                if (ourSrv) { ourSrv->Release(); }
                release();
                dst->Release();
                return true;
            }

            // ---- the three readings that decide this ---------------------
            const UINT sx = tgt.desc.Width / 2;
            const UINT sy = tgt.desc.Height / 2;

            Pixel      pre{};
            const bool preOk = ReadTexel(ctx, staging, dst, sx, sy, pre);

            DyeParams copyParams = kTint;
            copyParams.mode      = (srgb ? 1u : 0u) | 2u;  // control: no tint
            DispatchDye(ctx, cs, cb, tgt.srv, uav, copyParams, tgt.desc.Width, tgt.desc.Height);
            Pixel      copied{};
            const bool copyOk = ReadTexel(ctx, staging, dst, sx, sy, copied);

            DyeParams tintParams = kTint;
            tintParams.mode      = srgb ? 1u : 0u;
            DispatchDye(ctx, cs, cb, tgt.srv, uav, tintParams, tgt.desc.Width, tgt.desc.Height);
            Pixel      tinted{};
            const bool tintOk = ReadTexel(ctx, staging, dst, sx, sy, tinted);

            // Mip 0 now holds the tinted result; fill the rest of the chain
            // from it rather than tinting each level.
            ctx->GenerateMips(ourSrv);

            const bool readSource = copyOk && (copied.r | copied.g | copied.b | copied.a) != 0;
            const bool tintRan    = copyOk && tintOk &&
                                 (copied.r != tinted.r || copied.g != tinted.g ||
                                  copied.b != tinted.b);
            spdlog::info("DyeGpu: task 3 texel ({},{})  PRE=({},{},{},{}) "
                         "COPY=({},{},{},{}) TINT=({},{},{},{})",
                         sx, sy, pre.r, pre.g, pre.b, pre.a, copied.r, copied.g, copied.b,
                         copied.a, tinted.r, tinted.g, tinted.b, tinted.a);
            spdlog::info("DyeGpu: task 3 controls: preRead={} readTheGamesDiffuse={} "
                         "tintChangedIt={}",
                         preOk, readSource, tintRan);
            if (!readSource || !tintRan) {
                spdlog::error("DyeGpu: ===== TASK 3 STOPS BEFORE THE SWAP. {} No material "
                              "was touched. =====",
                              !readSource ? "The copy control read nothing, so the game's "
                                            "own diffuse was never sampled."
                                          : "The tint control matched the copy, so the "
                                            "arithmetic never ran.");
                ourSrv->Release();
                release();
                dst->Release();
                return true;
            }

            // ---- the object that carries it back into the engine ---------
            auto* const ourRd = new RendererData(static_cast<std::uint16_t>(dd.Width),
                                                 static_cast<std::uint16_t>(dd.Height));
            ourRd->texture      = dst;
            ourRd->resourceView = ourSrv;
            // ⚠ 0x18 AND 0x1A ARE NOT WIDTH AND HEIGHT, AND NEITHER HEADER IS
            // RIGHT ABOUT THEM. CommonLib reads those two u16s as width then
            // height and Community Shaders' PDB as height then width; the
            // 2026-08-05 run read (0,0) out of them on all ELEVEN worn shapes,
            // across two texture formats and four sizes. The engine does not
            // populate them, so a "copy whichever order the source proved"
            // branch stood here reasoning from a field that is always zero -
            // it is gone, and this writes the real dimensions instead. Nothing
            // rendering reads them either way: the engine's own textures render
            // with zeroes there and ours rendered with values there.
            //
            // ⚠ The ONLY two fields with evidence behind them are +0x00 and
            // +0x10, and those have it from three PDBs plus the per-shape
            // GetResource identity check above. Do not build anything on the
            // rest of this struct without measuring it first.
            // CS's PDB names 0x1C mips and 0x1D format, and CommonLib's own
            // default for 0x1D is 0x1C, which is 28, which is
            // DXGI_FORMAT_R8G8B8A8_UNORM. Two independent readings agreeing on
            // a value that decodes to the right constant is why these are set
            // rather than left at the constructor's guess.
            D3D11_TEXTURE2D_DESC madeDesc{};
            dst->GetDesc(&madeDesc);
            ourRd->unk1C = static_cast<std::uint8_t>(madeDesc.MipLevels);
            ourRd->unk1D = static_cast<std::uint8_t>(srgb ? 29 : 28);

            auto* const forged = ForgeSourceTexture(
                tgt.diffuse, ourRd, srgb ? "FittingRoom\\DyeGpuTask3_srgb" : "FittingRoom\\DyeGpuTask3");
            if (!forged) {
                spdlog::error("DyeGpu: task 3 could not allocate an NiSourceTexture.");
                delete ourRd;
                ourSrv->Release();
                release();
                dst->Release();
                return true;
            }

            g_held.tinted = RE::NiPointer<RE::NiSourceTexture>(forged);
            g_held.tex    = dst;      // never released; the forgery renders from it
            g_held.srv    = ourSrv;   // ditto
            spdlog::info("DyeGpu: task 3 forged NiSourceTexture {} rendererTexture={} "
                         "tex={} srv={} mips={} refs={}",
                         static_cast<void*>(forged), static_cast<void*>(ourRd),
                         static_cast<void*>(dst), static_cast<void*>(ourSrv),
                         madeDesc.MipLevels, forged->GetRefCount());

            release();  // the shader, the constant buffer, the UAV and the staging texel

            Task3Swap swap;
            swap.geom   = RE::NiPointer<RE::BSGeometry>(tgt.geom);
            swap.prop   = RE::NiPointer<RE::BSLightingShaderProperty>(tgt.prop);
            swap.tinted = g_held.tinted;
            if (auto* const task = SKSE::GetTaskInterface()) {
                task->AddTask([swap]() mutable { SwapDiffuseOnGameThread(std::move(swap)); });
            } else {
                spdlog::error("DyeGpu: task 3 has no task interface; the swap cannot run.");
            }
            return true;
        }

    }  // namespace

    void RunProbeOnce() {
        static bool s_done{ false };
        static bool s_announced{ false };
        static int  s_waits{ 0 };
        if (s_done) {
            return;
        }
        const auto task = Settings::GetSingleton().dyeGpuTask;
        if (task == 0) {
            return;
        }
        if (!s_announced) {
            s_announced = true;
            spdlog::info("DyeGpu: ===== iDyeGpuTask={} starting on the render thread =====",
                         task);
        }
        switch (task) {
            case 1:
                s_done = true;
                RunTask1();
                break;
            case 2:
                s_done = true;
                RunTask2();
                break;
            case 3: {
                // ⚠ TASK 3 RETRIES AND 1 AND 2 DO NOT, because it needs
                // something the others do not: a player with third-person 3D
                // wearing something. The first frame the editor draws is not
                // guaranteed to have one, and a probe that self-disabled on
                // that frame would report nothing at all and look like an
                // unreachable call site, which is the exact failure that cost
                // task 1 its first run.
                constexpr int kMaxWaits = 600;  // ~10 s at 60 fps
                if (RunTask3()) {
                    s_done = true;
                } else if (++s_waits >= kMaxWaits) {
                    s_done = true;
                    spdlog::warn("DyeGpu: ===== TASK 3 GAVE UP after {} frames. No "
                                 "third-person 3D with a dyeable shape carrying a live "
                                 "diffuse was ever found. =====", kMaxWaits);
                }
                break;
            }
            default:
                s_done = true;
                spdlog::warn("DyeGpu: iDyeGpuTask={} is not implemented yet.", task);
                break;
        }
    }

}  // namespace OS::DyeGpu
