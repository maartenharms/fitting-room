// Pure-logic tests for the dye strength blend. No engine, no Skyrim.
#include "DyeStrength.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS;

    {  // Full strength returns the colour untouched, at both extremes.
        CHECK(ApplyDyeStrength(0, 255) == 0);
        CHECK(ApplyDyeStrength(255, 255) == 255);
        CHECK(ApplyDyeStrength(200, 255) == 200);
        CHECK(ApplyDyeStrength(37, 255) == 37);
    }

    {  // Zero strength returns exactly the neutral, whatever went in.
        CHECK(ApplyDyeStrength(0, 0) == kDyeNeutral);
        CHECK(ApplyDyeStrength(255, 0) == kDyeNeutral);
        CHECK(ApplyDyeStrength(37, 0) == kDyeNeutral);
    }

    {  // The neutral is a fixed point at every strength, which is why a grey
       // swatch barely moves a mesh today.
        for (int s = 0; s <= 255; ++s) {
            CHECK(ApplyDyeStrength(kDyeNeutral, static_cast<std::uint8_t>(s)) ==
                  kDyeNeutral);
        }
    }

    {  // Half strength lands halfway, rounded to nearest rather than truncated.
       // 255 is 127 above the neutral; half of 127 is 63.5, which rounds to 64.
        CHECK(ApplyDyeStrength(255, 128) == 192);
        // 0 is 128 below; half of 128 is exactly 64.
        CHECK(ApplyDyeStrength(0, 128) == 64);
    }

    {  // Symmetric about the neutral: equal distance above and below weakens by
       // the same amount. This is what the rounding rule buys.
        for (int d = 1; d <= 127; ++d) {
            const auto up   = ApplyDyeStrength(static_cast<std::uint8_t>(128 + d), 128);
            const auto down = ApplyDyeStrength(static_cast<std::uint8_t>(128 - d), 128);
            CHECK((up - kDyeNeutral) == (kDyeNeutral - down));
        }
    }

    {  // Monotonic: more strength never moves a colour back toward the neutral.
        for (int s = 1; s <= 255; ++s) {
            const auto lo = ApplyDyeStrength(255, static_cast<std::uint8_t>(s - 1));
            const auto hi = ApplyDyeStrength(255, static_cast<std::uint8_t>(s));
            CHECK(hi >= lo);
        }
    }

    {  // Nothing ever leaves the byte range, at any pair of inputs.
        for (int c = 0; c <= 255; ++c) {
            for (int s = 0; s <= 255; s += 17) {
                const int v = ApplyDyeStrength(static_cast<std::uint8_t>(c),
                                               static_cast<std::uint8_t>(s));
                CHECK(v >= 0 && v <= 255);
            }
        }
    }

    if (g_failures == 0) {
        std::printf("all DyeStrength tests passed\n");
    }
    return g_failures;
}
