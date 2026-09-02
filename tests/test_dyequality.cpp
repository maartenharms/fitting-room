#include "DyeQuality.h"

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
    using OS::DyeQuality::ChainBytes;
    using OS::DyeQuality::HasSettled;
    using OS::DyeQuality::Level;
    using OS::DyeQuality::PickLevel;
    using OS::DyeQuality::ShouldUpgrade;

    // ---- PickLevel ------------------------------------------------------
    // A cap of 0 means no cap, which is what a commit build asks for until
    // the field decides otherwise. Mip 0 at the source's own size.
    {
        const Level l = PickLevel(4096, 4096, 13, 0);
        CHECK(l.mip == 0);
        CHECK(l.width == 4096 && l.height == 4096);
    }
    // 4096 capped at 2048 is exactly one level down. This is the 4x lever.
    {
        const Level l = PickLevel(4096, 4096, 13, 2048);
        CHECK(l.mip == 1);
        CHECK(l.width == 2048 && l.height == 2048);
    }
    // 4096 capped at 512 is three levels down, which is the preview.
    {
        const Level l = PickLevel(4096, 4096, 13, 512);
        CHECK(l.mip == 3);
        CHECK(l.width == 512 && l.height == 512);
    }
    // ⚠ A SOURCE WITH NO CHAIN CANNOT BE CAPPED, and reading mip 1 of it
    // would return zeros rather than failing. Full size at mip 0 is the only
    // honest answer.
    {
        const Level l = PickLevel(4096, 4096, 1, 512);
        CHECK(l.mip == 0);
        CHECK(l.width == 4096 && l.height == 4096);
    }
    // A short chain caps as far as the chain goes and no further.
    {
        const Level l = PickLevel(4096, 4096, 2, 512);
        CHECK(l.mip == 1);
        CHECK(l.width == 2048 && l.height == 2048);
    }
    // Already under the cap: nothing to do.
    {
        const Level l = PickLevel(1024, 1024, 11, 2048);
        CHECK(l.mip == 0);
        CHECK(l.width == 1024 && l.height == 1024);
    }
    // ⚠ THE CAP IS ON THE LONGER SIDE, so aspect survives. A 4096x2048
    // capped at 1024 is 1024x512 and not 1024x1024.
    {
        const Level l = PickLevel(4096, 2048, 13, 1024);
        CHECK(l.mip == 2);
        CHECK(l.width == 1024 && l.height == 512);
    }
    // Neither dimension may reach zero, whatever the shift does.
    {
        const Level l = PickLevel(1, 1, 1, 512);
        CHECK(l.mip == 0);
        CHECK(l.width == 1 && l.height == 1);
    }
    // A zero mip count is a source that reported nothing. Treat it as one
    // level rather than dividing by it.
    {
        const Level l = PickLevel(2048, 2048, 0, 512);
        CHECK(l.mip == 0);
        CHECK(l.width == 2048 && l.height == 2048);
    }

    // ---- ChainBytes -----------------------------------------------------
    // The three figures the spec's floor table is built on, to the MiB.
    CHECK(ChainBytes(4096, 4096, 1) / (1024 * 1024) == 85);
    CHECK(ChainBytes(2048, 2048, 1) / (1024 * 1024) == 21);
    CHECK(ChainBytes(512, 512, 1) / (1024 * 1024) == 1);
    // A 512 cubemap: one face's mip 0 is 512*512*4 = 1 MiB, six faces are 6
    // MiB, and the chain takes that to 8. That is the `ore_steel_e.dds` row of
    // the spec's floor table, arrived at from the other direction.
    //
    // ⚠ NOT `ChainBytes(512, 512, 1) * 6`. The 4/3 is ONE integer division over
    // the whole total, so that identity is off by two bytes and asserting it
    // would be asserting the rounding rather than the arithmetic.
    //
    // ⚠ AND 4/3 IS THE STANDARD APPROXIMATION RATHER THAN THE EXACT CHAIN. The
    // real sum of a ten-level chain here is 8388600 bytes, so this overshoots by
    // eight. Overshooting is the safe direction for a budget and it is what the
    // shipped cache has always counted with.
    CHECK(ChainBytes(512, 512, 6) == 8388608ull);

    // ---- HasSettled -----------------------------------------------------
    CHECK(!HasSettled(1000, 1000, 250));   // just changed
    CHECK(!HasSettled(1249, 1000, 250));   // one millisecond short
    CHECK(HasSettled(1250, 1000, 250));    // exactly the idle
    CHECK(HasSettled(9999, 1000, 250));
    // ⚠ A CLOCK THAT WENT BACKWARDS IS NOT A SETTLE. Nothing should be able
    // to spend 85 MiB because a timestamp read oddly.
    CHECK(!HasSettled(999, 1000, 250));
    // A settle of zero still requires the clock to have reached the change.
    CHECK(HasSettled(1000, 1000, 0));

    // ---- ShouldUpgrade --------------------------------------------------
    // The one case that should upgrade.
    CHECK(ShouldUpgrade(true, false, false, true, false));
    // Not settled yet: the player is still dragging.
    CHECK(!ShouldUpgrade(true, false, false, false, false));
    // ⚠ NO PREVIEW MEANS NO COMMIT. A source the preview refused is one the
    // commit would refuse too, at 85 MiB to find out.
    CHECK(!ShouldUpgrade(false, false, false, true, false));
    // Already committed, so there is nothing to upgrade to.
    CHECK(!ShouldUpgrade(true, true, false, true, false));
    // Already queued, so queueing again would build it twice.
    CHECK(!ShouldUpgrade(true, false, true, true, false));
    // ⚠ THE HOVER PREVIEW'S FLAG. A colour only a hover asked for never
    // upgrades however long it settles; the moment a real request touches the
    // colour it upgrades exactly as before.
    CHECK(!ShouldUpgrade(true, false, false, true, true));
    CHECK(ShouldUpgrade(true, false, false, true, false));

    if (g_failures == 0) {
        std::printf("DyeQuality: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
