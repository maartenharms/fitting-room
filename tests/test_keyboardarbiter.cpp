// Pure-logic tests for the keyboard feed arbiter (OS-46 doubled first key).
// No engine, no RE:: types.
#include "KeyboardArbiter.h"

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
    using OS::KeyboardArbiter;

    {  // WM idle at feed time, then WM wakes: the held first key is DROPPED
       // (WndProc already fed it) - this is the doubled-first-key fix.
        KeyboardArbiter a;
        a.OnEngineKey(0x1E, true, /*wmOwns*/ false);  // 'a' down, WM looks idle -> held
        CHECK(!a.Empty());
        const auto out = a.Drain(/*wmOwns*/ true);  // WM claimed the keyboard this frame
        CHECK(out.empty());                         // dropped -> no double
        CHECK(a.Empty());
    }
    {  // WM stays dead across the frame: the held key is FED (typing survives).
        KeyboardArbiter a;
        a.OnEngineKey(0x1E, true, false);
        const auto out = a.Drain(false);  // WM still silent -> genuinely dead
        CHECK(out.size() == 1);
        CHECK(out[0].dik == 0x1E && out[0].down);
        CHECK(a.Empty());  // drained
    }
    {  // WM owns at feed time: nothing is held, and prior holds are dropped.
        KeyboardArbiter a;
        a.OnEngineKey(0x1E, true, false);  // held
        a.OnEngineKey(0x30, true, true);   // WM now owns -> clears the hold, drops this
        CHECK(a.Empty());
        CHECK(a.Drain(false).empty());
    }
    {  // Several keys held across a dead-WM frame feed back in order.
        KeyboardArbiter a;
        a.OnEngineKey(0x1E, true, false);   // 'a' down
        a.OnEngineKey(0x1E, false, false);  // 'a' up
        a.OnEngineKey(0x30, true, false);   // 'b' down
        const auto out = a.Drain(false);
        CHECK(out.size() == 3);
        CHECK(out[0].dik == 0x1E && out[0].down);
        CHECK(out[1].dik == 0x1E && !out[1].down);
        CHECK(out[2].dik == 0x30 && out[2].down);
    }
    {  // Steady typing (WM owns every frame): never holds, never doubles.
        KeyboardArbiter a;
        a.OnEngineKey(0x1E, true, true);
        CHECK(a.Empty());
        CHECK(a.Drain(true).empty());
        a.OnEngineKey(0x1E, false, true);
        CHECK(a.Drain(true).empty());
    }
    {  // Empty drain and Clear are no-ops.
        KeyboardArbiter a;
        CHECK(a.Drain(false).empty());
        CHECK(a.Drain(true).empty());
        a.OnEngineKey(0x1E, true, false);
        a.Clear();
        CHECK(a.Empty());
    }

    if (g_failures == 0) {
        std::printf("all KeyboardArbiter tests passed\n");
    }
    return g_failures;
}
