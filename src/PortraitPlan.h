#pragma once

#include <algorithm>
#include <cstring>
#include <cstdint>

// The pure half of the portrait capture: crop-rect arithmetic and pixel-row
// conversion, engine-free so PortraitPlanTests can pin them. The D3D half
// (backbuffer copy, mapping, the PNG hand-off) lives in Portrait.cpp and
// stays out of here.
namespace OS::PortraitPlan {

    struct CropRect {
        std::uint32_t x{ 0 };
        std::uint32_t y{ 0 };
        std::uint32_t size{ 0 };  // square

        [[nodiscard]] bool Valid() const { return size > 0; }

        friend bool operator==(const CropRect&, const CropRect&) = default;
    };

    // A square crop centred on the projected head, sized from the projected
    // head radius. a_headRadiusPx is the head bound's radius in pixels; the
    // factor widens it to a head-and-shoulders portrait rather than a
    // skull-tight one. Everything clamps: the size to [a_minSize, the
    // shorter screen edge], the origin so the square stays on screen even
    // for a head framed at the edge. A zero radius (projection failed, head
    // behind the camera) is an invalid rect the caller skips.
    [[nodiscard]] inline CropRect HeadCrop(float a_centerXPx, float a_centerYPx,
                                           float a_headRadiusPx,
                                           std::uint32_t a_screenW,
                                           std::uint32_t a_screenH,
                                           std::uint32_t a_minSize = 96) {
        if (a_headRadiusPx <= 0.0f || a_screenW == 0 || a_screenH == 0) {
            return {};
        }
        const auto shortEdge = std::min(a_screenW, a_screenH);
        auto       size      = static_cast<std::uint32_t>(a_headRadiusPx * 4.4f);
        size                 = std::clamp(size, std::min(a_minSize, shortEdge),
                                          shortEdge);
        const auto half = static_cast<float>(size) / 2.0f;
        auto x = static_cast<std::int64_t>(a_centerXPx - half);
        auto y = static_cast<std::int64_t>(a_centerYPx - half);
        x      = std::clamp<std::int64_t>(x, 0, static_cast<std::int64_t>(a_screenW - size));
        y      = std::clamp<std::int64_t>(y, 0, static_cast<std::int64_t>(a_screenH - size));
        return CropRect{ static_cast<std::uint32_t>(x),
                         static_cast<std::uint32_t>(y), size };
    }

    // The backbuffer formats the converter understands, named here so the
    // capture can log the DXGI value it refused rather than guessing.
    enum class PixelLayout : std::uint8_t {
        kRgba8,     // DXGI R8G8B8A8_UNORM (+_SRGB)
        kBgra8,     // DXGI B8G8R8A8_UNORM (+_SRGB)
        kRgb10A2,   // DXGI R10G10B10A2_UNORM
        kRgba16F,   // DXGI R16G16B16A16_FLOAT
    };

    // IEEE half to float, the scalar textbook decode. Enough for a portrait:
    // NaN and infinity clamp to white, denormals to their tiny values.
    [[nodiscard]] inline float HalfToFloat(std::uint16_t a_h) {
        const std::uint32_t sign = (a_h & 0x8000u) << 16;
        const std::uint32_t exp  = (a_h & 0x7C00u) >> 10;
        const std::uint32_t man  = a_h & 0x03FFu;
        std::uint32_t       bits;
        if (exp == 0) {
            if (man == 0) {
                bits = sign;
            } else {
                // Denormal: promote to normalised float.
                float f = static_cast<float>(man) / 16777216.0f;  // man / 2^24
                return (sign ? -f : f);
            }
        } else if (exp == 0x1F) {
            bits = sign | 0x7F800000u | (man << 13);
        } else {
            bits = sign | ((exp + 112u) << 23) | (man << 13);
        }
        float out;
        static_assert(sizeof(out) == sizeof(bits));
        std::memcpy(&out, &bits, sizeof(out));
        return out;
    }

    // Convert one source pixel to RGBA8 with alpha forced opaque (a portrait
    // has no business being translucent; HDR backbuffers carry junk alpha).
    // a_src points at the pixel; stride is the caller's business.
    inline void ConvertPixel(PixelLayout a_layout, const std::uint8_t* a_src,
                             std::uint8_t* a_dst) {
        const auto clamp8 = [](float a_v) {
            return static_cast<std::uint8_t>(
                std::clamp(a_v * 255.0f + 0.5f, 0.0f, 255.0f));
        };
        switch (a_layout) {
            case PixelLayout::kRgba8:
                a_dst[0] = a_src[0];
                a_dst[1] = a_src[1];
                a_dst[2] = a_src[2];
                break;
            case PixelLayout::kBgra8:
                a_dst[0] = a_src[2];
                a_dst[1] = a_src[1];
                a_dst[2] = a_src[0];
                break;
            case PixelLayout::kRgb10A2: {
                std::uint32_t v;
                std::memcpy(&v, a_src, sizeof(v));
                a_dst[0] = static_cast<std::uint8_t>(((v >> 0) & 0x3FFu) >> 2);
                a_dst[1] = static_cast<std::uint8_t>(((v >> 10) & 0x3FFu) >> 2);
                a_dst[2] = static_cast<std::uint8_t>(((v >> 20) & 0x3FFu) >> 2);
                break;
            }
            case PixelLayout::kRgba16F: {
                std::uint16_t h[3];
                std::memcpy(h, a_src, sizeof(h));
                a_dst[0] = clamp8(HalfToFloat(h[0]));
                a_dst[1] = clamp8(HalfToFloat(h[1]));
                a_dst[2] = clamp8(HalfToFloat(h[2]));
                break;
            }
        }
        a_dst[3] = 255;
    }

    [[nodiscard]] inline std::uint32_t BytesPerPixel(PixelLayout a_layout) {
        switch (a_layout) {
            case PixelLayout::kRgba8:
            case PixelLayout::kBgra8:
            case PixelLayout::kRgb10A2:
                return 4;
            case PixelLayout::kRgba16F:
                return 8;
        }
        return 0;
    }

}  // namespace OS::PortraitPlan
