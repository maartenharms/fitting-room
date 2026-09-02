#pragma once

#include <cstdint>
#include <vector>

#include <d3d11.h>

#include "MeshExtractor.h"
#include "PreviewFraming.h"

// One offscreen draw of an extracted mesh set into a square target, then a
// readback of the pixels. The full pipeline state is saved before the first
// binding and restored after the readback: OM with its UAVs, RS, IA, all five
// shader stages, blend, depth, the slot-0 constant buffers, sampler and SRV.
// Spec Appendix B with its four corrections; PreviewRenderer.cpp carries them
// inline where each applies.
namespace OS::PreviewRenderer {

    struct Timings {
        double renderMs{ 0.0 };
        double readbackMs{ 0.0 };
    };

    // Draw a_meshes into an a_size square and read it back as tightly packed
    // RGBA8. a_pose picks the camera: kDiagonal for weapons, kUpright for
    // worn gear, kEyes the straight-on single-eye chip (PreviewFraming.h
    // carries the reasoning). a_bgLift raises the backdrop toward grey, the
    // brow scenes' contrast fix; 0 keeps the studio dark. Render thread
    // only. False on any failure, with the pipeline state restored
    // regardless.
    [[nodiscard]] bool RenderThumbnail(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx,
                                       const std::vector<MeshExtractor::RenderMesh>& a_meshes,
                                       std::uint32_t a_size, PreviewFraming::Pose a_pose,
                                       float a_bgLift, PreviewFraming::Crop a_crop,
                                       std::vector<std::uint8_t>& a_outRgba,
                                       Timings& a_outTimings);

}  // namespace OS::PreviewRenderer
