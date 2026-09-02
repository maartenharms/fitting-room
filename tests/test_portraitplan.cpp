// Portrait plan tests: crop arithmetic and pixel conversion, engine-free.
//
// ⚠ FIXTURES FROM REAL VALUES per house rule: the screen is the rig's own
// 2560x1440 (the r20 field screenshots' dimensions), and the format list is
// exactly the four DXGI layouts LayoutFor accepts in Portrait.cpp.
#include "PortraitPlan.h"

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::PortraitPlan;

    // ---- HeadCrop ------------------------------------------------------

    // A head framed mid-screen at a plausible editor distance: the crop is
    // square, centred, and 4.4 times the projected radius.
    {
        const auto c = HeadCrop(1280.0f, 500.0f, 90.0f, 2560, 1440);
        CHECK(c.Valid());
        CHECK(c.size == static_cast<std::uint32_t>(90.0f * 4.4f));
        CHECK(c.x == 1280 - c.size / 2);
        CHECK(c.y == 500 - c.size / 2);
    }
    // Clamped to the screen when the head sits near an edge.
    {
        const auto c = HeadCrop(40.0f, 30.0f, 90.0f, 2560, 1440);
        CHECK(c.Valid() && c.x == 0 && c.y == 0);
    }
    {
        const auto c = HeadCrop(2550.0f, 1430.0f, 90.0f, 2560, 1440);
        CHECK(c.Valid());
        CHECK(c.x + c.size <= 2560 && c.y + c.size <= 1440);
    }
    // A tiny projected head still yields the minimum usable card.
    {
        const auto c = HeadCrop(1280.0f, 700.0f, 4.0f, 2560, 1440);
        CHECK(c.Valid() && c.size == 96);
    }
    // A huge radius clamps to the short edge and stays on screen.
    {
        const auto c = HeadCrop(1280.0f, 700.0f, 2000.0f, 2560, 1440);
        CHECK(c.Valid() && c.size == 1440 && c.y == 0);
    }
    // Failed projection is an invalid rect.
    CHECK(!HeadCrop(100.0f, 100.0f, 0.0f, 2560, 1440).Valid());
    CHECK(!HeadCrop(100.0f, 100.0f, 50.0f, 0, 0).Valid());

    // ---- HalfToFloat ---------------------------------------------------

    CHECK(HalfToFloat(0x0000) == 0.0f);
    CHECK(HalfToFloat(0x3C00) == 1.0f);
    CHECK(HalfToFloat(0xBC00) == -1.0f);
    CHECK(HalfToFloat(0x3800) == 0.5f);
    CHECK(HalfToFloat(0x4248) == 3.140625f);

    // ---- ConvertPixel --------------------------------------------------

    std::uint8_t dst[4];

    // RGBA8 passes through with alpha forced opaque.
    {
        const std::uint8_t src[4] = { 10, 20, 30, 7 };
        ConvertPixel(PixelLayout::kRgba8, src, dst);
        CHECK(dst[0] == 10 && dst[1] == 20 && dst[2] == 30 && dst[3] == 255);
    }
    // BGRA8 swaps.
    {
        const std::uint8_t src[4] = { 30, 20, 10, 7 };
        ConvertPixel(PixelLayout::kBgra8, src, dst);
        CHECK(dst[0] == 10 && dst[1] == 20 && dst[2] == 30 && dst[3] == 255);
    }
    // R10G10B10A2: full-scale channels read 255 each (1023 >> 2).
    {
        const std::uint32_t v = (1023u) | (1023u << 10) | (1023u << 20);
        std::uint8_t        src[4];
        std::memcpy(src, &v, 4);
        ConvertPixel(PixelLayout::kRgb10A2, src, dst);
        CHECK(dst[0] == 255 && dst[1] == 255 && dst[2] == 255 && dst[3] == 255);
    }
    // R10G10B10A2 mid-grey: 512 >> 2 = 128.
    {
        const std::uint32_t v = (512u) | (512u << 10) | (512u << 20);
        std::uint8_t        src[4];
        std::memcpy(src, &v, 4);
        ConvertPixel(PixelLayout::kRgb10A2, src, dst);
        CHECK(dst[0] == 128 && dst[1] == 128 && dst[2] == 128);
    }
    // RGBA16F: one clamps to 255, half to 128, and over-range HDR clamps.
    {
        const std::uint16_t h[4] = { 0x3C00, 0x3800, 0x4400, 0x0000 };
        std::uint8_t        src[8];
        std::memcpy(src, h, 8);
        ConvertPixel(PixelLayout::kRgba16F, src, dst);
        CHECK(dst[0] == 255);
        CHECK(dst[1] == 128);
        CHECK(dst[2] == 255);  // 4.0 clamps
        CHECK(dst[3] == 255);
    }

    CHECK(BytesPerPixel(PixelLayout::kRgba8) == 4);
    CHECK(BytesPerPixel(PixelLayout::kBgra8) == 4);
    CHECK(BytesPerPixel(PixelLayout::kRgb10A2) == 4);
    CHECK(BytesPerPixel(PixelLayout::kRgba16F) == 8);

    if (g_failures == 0) {
        std::printf("PortraitPlanTests: all passed\n");
        return 0;
    }
    std::printf("PortraitPlanTests: %d failure(s)\n", g_failures);
    return 1;
}
