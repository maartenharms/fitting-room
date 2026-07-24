#include "RefreshGate.h"

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
    using OS::RefreshGate::BlockingCooldown;
    using OS::RefreshGate::ClassifyStagedUpdate;
    using OS::RefreshGate::StagedUpdate;

    CHECK(ClassifyStagedUpdate(false, 0) == StagedUpdate::kNone);
    CHECK(ClassifyStagedUpdate(true, 0) == StagedUpdate::kBodyOnly);
    CHECK(ClassifyStagedUpdate(false, 1) == StagedUpdate::kEquipment);
    CHECK(ClassifyStagedUpdate(true, 2) == StagedUpdate::kEquipment);

    BlockingCooldown gate;
    CHECK(gate.Ready(false, 10.0));

    CHECK(!gate.Ready(true, 20.0));
    CHECK(!gate.Ready(true, 25.0));
    CHECK(!gate.Ready(false, 25.1));
    CHECK(!gate.Ready(false, 26.09));
    CHECK(gate.Ready(false, 26.10));

    // Once the cooldown is consumed, ordinary refreshes are immediate again.
    CHECK(gate.Ready(false, 26.11));

    // Re-entering block during the cooldown restarts the full post-block second.
    CHECK(!gate.Ready(true, 30.0));
    CHECK(!gate.Ready(false, 30.2));
    CHECK(!gate.Ready(true, 30.8));
    CHECK(!gate.Ready(false, 31.0));
    CHECK(!gate.Ready(false, 31.99));
    CHECK(gate.Ready(false, 32.0));

    if (g_failures == 0) {
        std::printf("all RefreshGate tests passed\n");
    }
    return g_failures;
}
