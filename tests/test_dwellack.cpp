#include "DwellAck.h"

#include <cstdio>
#include <string>

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

    const std::string a = "eso:black_opal";
    const std::string b = "eso:sunset_amber";
    const std::string none{};

    // ⚠ THE SHIPPED DELAY IS ZERO (user 2026-08-12: "make it go away on hover,
    // no timer"). These are the cases that decide what the field sees.
    {
        CHECK(kAckDwellSeconds == 0.0);

        // Contact, then the next frame: that is the whole gesture.
        DwellAck<std::string> d;
        CHECK(!d.Rested(true, a, 0.000));
        CHECK(d.Rested(true, a, 0.017));
        // And once only, however long the pointer stays.
        CHECK(!d.Rested(true, a, 0.034));
        CHECK(!d.Rested(true, a, 5.000));
    }

    {  // ⚠ EVEN AT ZERO, A SWEEP BANKS NOTHING, because a new candidate every
       // frame never gets its second frame. This is the whole reason the edge
       // survived the timer being taken out.
        DwellAck<std::string> d;
        double                t = 0.0;
        for (int i = 0; i < 400; ++i) {
            const std::string id = "eso:swatch_" + std::to_string(i);
            CHECK(!d.Rested(true, id, t));
            t += 0.016;
        }
    }

    {  // Leaving and coming back is a new hover, and fires again. The write is
       // idempotent, so this is cheaper than remembering one id forever.
        DwellAck<std::string> d;
        CHECK(!d.Rested(true, a, 0.000));
        CHECK(d.Rested(true, a, 0.017));
        CHECK(!d.Rested(false, none, 0.034));  // left the grid
        CHECK(!d.Rested(true, a, 0.050));      // back: a new hover
        CHECK(d.Rested(true, a, 0.067));
    }

    {  // The neighbour is its own hover; nothing carries from the last one.
        DwellAck<std::string> d;
        CHECK(!d.Rested(true, a, 0.000));
        CHECK(d.Rested(true, a, 0.017));
        CHECK(!d.Rested(true, b, 0.034));
        CHECK(d.Rested(true, b, 0.050));
    }

    // The delay is still a parameter and still works. Restoring the dwell that
    // shipped first is one constant, so these keep its cases alive.
    constexpr double kDwell = 0.45;
    {
        DwellAck<std::string> d;
        CHECK(!d.Rested(true, a, 0.00, kDwell));  // the dwell starts
        CHECK(!d.Rested(true, a, 0.20, kDwell));  // still inside it
        CHECK(d.Rested(true, a, 1.00, kDwell));   // rested: this is the write
        CHECK(!d.Rested(true, a, 1.10, kDwell));  // holding still is not a second read
    }

    {  // Leaving mid-dwell abandons it: coming back starts over rather than
       // finishing on time borrowed from the first pass.
        DwellAck<std::string> d;
        CHECK(!d.Rested(true, a, 0.0, kDwell));
        CHECK(!d.Rested(false, none, 0.2, kDwell));
        CHECK(!d.Rested(true, a, 0.3, kDwell));
        CHECK(!d.Rested(true, a, 0.5, kDwell));  // 0.2s in, not 0.5s in
        CHECK(d.Rested(true, a, 0.8, kDwell));
    }

    {  // A slow sweep is the case a delay buys and a zero does not: 0.1s per
       // swatch banks nothing here, and would bank every one of them at zero.
        DwellAck<std::string> d;
        double                t = 0.0;
        for (int i = 0; i < 40; ++i) {
            const std::string id = "eso:swatch_" + std::to_string(i);
            CHECK(!d.Rested(true, id, t, kDwell));
            t += 0.05;
            CHECK(!d.Rested(true, id, t, kDwell));
            t += 0.05;
        }
    }

    {  // Forget is the editor closing under it: nothing is owed afterwards.
        DwellAck<std::string> d;
        CHECK(!d.Rested(true, a, 0.0, kDwell));
        d.Forget();
        CHECK(!d.Rested(true, a, 1.0, kDwell));  // the dwell begins again here
        CHECK(d.Rested(true, a, 2.0, kDwell));
    }

    if (g_failures == 0) {
        std::printf("DwellAckTests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
