// Pure-logic tests for which pages the player wants in the rail, and for the
// correction that moves the editor off a page that stopped being drawn. No
// engine, no RE:: types.
//
// ⚠ WHAT THIS FILE DELIBERATELY DOES NOT COVER: whether a tile is actually
// DRAWN. That also depends on OBody being installed, on the build channel, and
// on a preset source existing, none of which are offline-testable and none of
// which live in PagePolicy.h. Correction takes the drawn list as an argument
// for exactly that reason, so the tests supply their own.
#include "PagePolicy.h"

#include <cstdio>
#include <vector>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS::PagePolicy;
    using PaneMode = OS::EditorGate::PaneMode;

    constexpr PaneMode kAll[] = {
        PaneMode::kStyles, PaneMode::kPresets,  PaneMode::kDye,      PaneMode::kRules,
        PaneMode::kBodyStudio, PaneMode::kShape, PaneMode::kOverlays, PaneMode::kProfiles,
    };

    {  // Everything on is the default, and it is every page rather than most
        constexpr Visibility v{};
        CHECK(WantedCount(v) == 8);
        for (const auto m : kAll) {
            CHECK(Wanted(v, m));
        }
    }

    {  // Each flag governs its own page and nothing else
        Visibility v{};
        v.looks = false;
        CHECK(!Wanted(v, PaneMode::kProfiles));
        CHECK(Wanted(v, PaneMode::kStyles));
        CHECK(Wanted(v, PaneMode::kShape));
        CHECK(WantedCount(v) == 7);

        Visibility b{};
        b.bodies = false;
        CHECK(!Wanted(b, PaneMode::kBodyStudio));
        CHECK(Wanted(b, PaneMode::kProfiles));
        CHECK(WantedCount(b) == 7);
    }

    {  // WantedCount reaches zero rather than bottoming out at one. The panel
       // reports this state and the INI can be hand-edited into it.
        Visibility none{};
        none.styles = none.presets = none.dye = none.rules = false;
        none.bodies = none.shape = none.overlays = none.looks = false;
        CHECK(WantedCount(none) == 0);
        for (const auto m : kAll) {
            CHECK(!Wanted(none, m));
        }
    }

    {  // The clamp is what makes an empty rail unreachable, and it reports
       // whether it did anything so a caller can save the corrected INI rather
       // than fighting the file every load.
        Visibility none{};
        none.styles = none.presets = none.dye = none.rules = false;
        none.bodies = none.shape = none.overlays = none.looks = false;
        CHECK(EnsureAtLeastOne(none));
        CHECK(WantedCount(none) == 1);
        CHECK(Wanted(none, PaneMode::kStyles));

        // ⚠ Styles specifically, not whichever flag sorts first. It is the page
        // the mod exists for and the only one with no requirement that can go
        // missing, so the clamp can never land somewhere unreachable.
        CHECK(!Wanted(none, PaneMode::kDye));
    }

    {  // A visibility that already has a page is left ALONE. The clamp must not
       // quietly switch Styles back on for someone who turned it off and kept
       // Dye, which would read as the panel refusing an edit.
        Visibility onlyDye{};
        onlyDye.styles = onlyDye.presets = onlyDye.rules = false;
        onlyDye.bodies = onlyDye.shape = onlyDye.overlays = onlyDye.looks = false;
        const Visibility before = onlyDye;
        CHECK(!EnsureAtLeastOne(onlyDye));
        CHECK(onlyDye == before);
        CHECK(!Wanted(onlyDye, PaneMode::kStyles));
        CHECK(WantedCount(onlyDye) == 1);

        Visibility full{};
        CHECK(!EnsureAtLeastOne(full));
        CHECK(full == Visibility{});
    }

    {  // Standing on a page that drew: stay put
        const std::vector<PaneMode> drawn{ PaneMode::kStyles, PaneMode::kDye,
                                           PaneMode::kProfiles };
        CHECK(!Correction(drawn, PaneMode::kStyles).has_value());
        CHECK(!Correction(drawn, PaneMode::kDye).has_value());
        CHECK(!Correction(drawn, PaneMode::kProfiles).has_value());
    }

    {  // Standing on a page that did not draw: move to the FIRST drawn one,
       // which is the order the tiles were submitted in and not an enum order.
       // Presets is enum 1 and would win a numeric race; Shape is enum 5 and
       // drew first here, so Shape is the answer.
        const std::vector<PaneMode> drawn{ PaneMode::kShape, PaneMode::kPresets };
        CHECK(Correction(drawn, PaneMode::kProfiles) == PaneMode::kShape);
        CHECK(Correction(drawn, PaneMode::kStyles) == PaneMode::kShape);
    }

    {  // A requirement disappearing is corrected exactly like a preference
       // change, because only the drawn list is consulted. Uninstalling OBody
       // drops Bodies out of the list and this moves off it.
        const std::vector<PaneMode> withBodies{ PaneMode::kStyles, PaneMode::kBodyStudio };
        CHECK(!Correction(withBodies, PaneMode::kBodyStudio).has_value());
        const std::vector<PaneMode> withoutBodies{ PaneMode::kStyles };
        CHECK(Correction(withoutBodies, PaneMode::kBodyStudio) == PaneMode::kStyles);
    }

    {  // Nothing drew at all: stay put and let the caller show the empty state.
       // ⚠ This returns the same nullopt as "you are fine" on purpose. The
       // caller tells them apart by looking at the drawn list, which is the one
       // test, rather than by unpacking a tri-state at every call site.
        const std::vector<PaneMode> nothing{};
        CHECK(!Correction(nothing, PaneMode::kStyles).has_value());
        CHECK(!Correction(nothing, PaneMode::kProfiles).has_value());
        CHECK(nothing.empty());
    }

    {  // One page drew and it is not the current one
        const std::vector<PaneMode> one{ PaneMode::kRules };
        CHECK(Correction(one, PaneMode::kStyles) == PaneMode::kRules);
        CHECK(!Correction(one, PaneMode::kRules).has_value());
    }

    {  // Equality is by value, so the panel can tell a real change from a redraw
        Visibility a{};
        Visibility b{};
        CHECK(a == b);
        b.dye = false;
        CHECK(!(a == b));
    }

    if (g_failures == 0) {
        std::printf("PagePolicyTests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
