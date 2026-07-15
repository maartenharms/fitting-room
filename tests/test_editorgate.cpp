// Pure-logic tests for the editor hotkey gate. No engine, no RE:: types.
#include "EditorGate.h"

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
    using namespace OS::EditorGate;

    {  // DecideGate: open editor closes unless a text field is focused
        CHECK(DecideGate(true, false, true, true) == GateAction::kClose);
        CHECK(DecideGate(true, true, true, true) == GateAction::kIgnore);
    }
    {  // DecideGate: closed editor needs a context, then the seamstone, then opens
        CHECK(DecideGate(false, false, false, true) == GateAction::kNeedContext);
        CHECK(DecideGate(false, false, true, false) == GateAction::kNeedSeamstone);
        CHECK(DecideGate(false, false, true, true) == GateAction::kOpen);
        // no context wins over a failed seamstone check
        CHECK(DecideGate(false, false, false, false) == GateAction::kNeedContext);
    }
    {  // DikName: named keys + unbound + hex fallback
        CHECK(DikName(0x18) == "O");
        CHECK(DikName(0x39) == "Space");
        CHECK(DikName(0x3B) == "F1");
        CHECK(DikName(0x00) == "(unbound)");
        CHECK(DikName(0xFF) == "Key 0xFF");
    }

    if (g_failures == 0) {
        std::printf("all EditorGate tests passed\n");
    }
    return g_failures;
}
