#include "UiScaleMigration.h"
#include "LooksMigration.h"
#include "InstallerSeed.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace U = OS::UiScale;

// The value the mod ships today. Named here rather than included, because
// Settings.h pulls the engine in and that is the whole reason the header under
// test was split out of it.
static constexpr float kToday = 0.63f;

int main() {
    // ---- what was handed out ---------------------------------------------
    // 0.8 was kUiScaleDefault from release 0.3.0 and 0.7 from 2026-08-08.
    // Nobody picked either one; they were the value of the day.
    CHECK(U::WasHandedOut(0.8f));
    CHECK(U::WasHandedOut(0.7f));
    // The six decimals the INI actually stores, round-tripped through double,
    // which is the form Settings::Load hands over.
    CHECK(U::WasHandedOut(static_cast<float>(0.800000)));
    CHECK(U::WasHandedOut(static_cast<float>(0.700000)));
    // ⚠ AND NOTHING ELSE. A value between them was moved to by hand, and this
    // restraint is what makes the migration safe to run without asking.
    CHECK(!U::WasHandedOut(0.75f));
    CHECK(!U::WasHandedOut(kToday));
    CHECK(!U::WasHandedOut(0.5f));
    // Close is not equal. The slider steps far coarser than this tolerance.
    CHECK(!U::WasHandedOut(0.79f));
    CHECK(!U::WasHandedOut(0.71f));

    // ---- Adopt -----------------------------------------------------------
    {
        // An unstamped INI carrying a handed-out default starts at today's.
        CHECK(U::Adopt(0.8f, 0, kToday) == kToday);
        CHECK(U::Adopt(0.7f, 0, kToday) == kToday);

        // ⚠⚠ 0.65 IS A HANDED-OUT DEFAULT TOO, since 2026-08-31, and an INI
        // stamped at version 1 is exactly the population that has it pinned:
        // that build migrated 0.8 and 0.7 and then wrote 0.65 into every file
        // it saved. A stamp BELOW the current one has to re-migrate or the
        // whole existing player base stays on the old number, which is the
        // fault this file exists for.
        CHECK(U::Adopt(0.65f, 0, kToday) == kToday);
        CHECK(U::Adopt(0.65f, 1, kToday) == kToday);
        // ...and once THIS build has stamped it, 0.65 is a choice like any
        // other and is left alone for good.
        CHECK(U::Adopt(0.65f, U::kSettingsVersion, kToday) == 0.65f);

        // ⚠⚠ A STAMPED 0.8 IS A CHOICE AND SURVIVES. 0.8 is also kUiScaleMax,
        // so the top of the slider and the July default are the same number:
        // the stamp is the only thing that can tell them apart, and this is the
        // assertion that says a player who wants the biggest editor keeps it.
        CHECK(U::Adopt(0.8f, U::kSettingsVersion, kToday) == 0.8f);
        CHECK(U::Adopt(0.7f, U::kSettingsVersion, kToday) == 0.7f);

        // A hand-picked value is never touched, stamp or no stamp.
        CHECK(U::Adopt(0.75f, 0, kToday) == 0.75f);
        CHECK(U::Adopt(0.55f, 0, kToday) == 0.55f);
        CHECK(U::Adopt(0.75f, U::kSettingsVersion, kToday) == 0.75f);

        // Already on today's value: nothing to do, either way.
        CHECK(U::Adopt(kToday, 0, kToday) == kToday);
        CHECK(U::Adopt(kToday, U::kSettingsVersion, kToday) == kToday);

        // A stamp from a LATER build is still a stamp. Rolling the mod back
        // must not re-run a migration the file has already been through.
        CHECK(U::Adopt(0.8f, U::kSettingsVersion + 1, kToday) == 0.8f);
    }

    // ---- the two roads to 0.8 --------------------------------------------
    // ⚠ THE CLAMP IS THE SECOND ONE. The slider once ran to 1.6 (OS-17), so an
    // INI from that era arrives as exactly kUiScaleMax whatever it said. Adopt
    // is given the CLAMPED value, so one rule covers both roads. This mirrors
    // the order in Settings::Load rather than repeating the clamp here.
    {
        const float clampedFrom16 = 0.8f;  // std::clamp(1.6f, 0.5f, 0.8f)
        CHECK(U::Adopt(clampedFrom16, 0, kToday) == kToday);
    }

    // ---- the face keys' one-time flip (OS::LooksOn) ------------------------
    // In this binary on purpose: one migration test executable, so the lists
    // in build.bat, build_tests.bat and CMakeLists gain nothing to drift on.
    {
        namespace L = OS::LooksOn;
        // A 0 under an old stamp is the installer's answer of its day.
        CHECK(L::Adopt(false, 0) == true);
        CHECK(L::Adopt(false, L::kSettingsVersion - 1) == true);
        // A 1 is a 1 whatever the stamp.
        CHECK(L::Adopt(true, 0) == true);
        CHECK(L::Adopt(true, L::kSettingsVersion) == true);
        // A stamped 0 is a choice and stays, and a later build's stamp is
        // still a stamp.
        CHECK(L::Adopt(false, L::kSettingsVersion) == false);
        CHECK(L::Adopt(false, L::kSettingsVersion + 1) == false);
    }

    // ---- the installer's answers, once per install (OS::InstallerSeed) ----
    {
        namespace I = OS::InstallerSeed;
        // A fresh install over a file that never took an installer's answers.
        CHECK(I::ShouldSeed("", "1.1.7-lore-earn-full-20260902T0800Z"));
        // A different zip, or the same zip with different answers.
        CHECK(I::ShouldSeed("1.1.6-lore-earn-safe-x", "1.1.7-lore-earn-full-y"));
        CHECK(I::ShouldSeed("1.1.7-lore-earn-full-y", "1.1.7-lore-free-full-y"));
        // The same install again is a no-op, which is what keeps the panel's
        // later edits.
        CHECK(!I::ShouldSeed("1.1.7-lore-earn-full-y", "1.1.7-lore-earn-full-y"));
        // A hand-copied install has no installer file and nothing to seed.
        CHECK(!I::ShouldSeed("", ""));
        CHECK(!I::ShouldSeed("1.1.7-lore-earn-full-y", ""));
    }

    if (g_failures == 0) {
        std::printf("all UiScaleMigration tests passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
