// Pure-logic tests for WHICH head part a character wears (HeadPartLadder.h):
// the outfit's, their character default, or their own.
//
// This precedence is what OutfitSession's push lambda calls "the whole
// feature", and it had no test until 2026-08-09 because it lived inline in an
// engine-only function. It got one when the ladder grew its second half: a
// rung is taken only if it is BOTH named AND usable for this character's
// current race and sex. Before that, a stored default naming a female eye was
// applied to a male character, because nothing on the apply path asks the
// question the browser asks.
#include "HeadPartLadder.h"

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
    using namespace OS::HeadPartLadder;

    {  // The whole 4-bit matrix, so no combination is decided by accident.
       // Written out rather than generated: the table IS the specification,
       // and a generator would only re-derive Choose from itself.
        struct Case {
            Rungs r;
            Wear  wear;
            bool  passedOver;
        };
        // outfitNames, outfitUsable, defaultNames, defaultUsable
        const Case cases[] = {
            { { false, false, false, false }, Wear::kOwn,     false },
            { { false, false, false, true  }, Wear::kOwn,     false },  // usable but unnamed = nothing
            { { false, false, true,  false }, Wear::kOwn,     true  },  // the reported bug's shape
            { { false, false, true,  true  }, Wear::kDefault, false },
            { { false, true,  false, false }, Wear::kOwn,     false },
            { { false, true,  false, true  }, Wear::kOwn,     false },
            { { false, true,  true,  false }, Wear::kOwn,     true  },
            { { false, true,  true,  true  }, Wear::kDefault, false },
            { { true,  false, false, false }, Wear::kOwn,     false },  // outfit named but unusable
            { { true,  false, false, true  }, Wear::kOwn,     false },
            { { true,  false, true,  false }, Wear::kOwn,     true  },
            { { true,  false, true,  true  }, Wear::kDefault, false },  // outfit loses, default catches
            { { true,  true,  false, false }, Wear::kOutfit,  false },
            { { true,  true,  false, true  }, Wear::kOutfit,  false },
            { { true,  true,  true,  false }, Wear::kOutfit,  false },
            { { true,  true,  true,  true  }, Wear::kOutfit,  false },  // outfit beats a usable default
        };
        for (const auto& c : cases) {
            const auto v = Choose(c.r);
            CHECK(v.wear == c.wear);
            CHECK(v.defaultPassedOver == c.passedOver);
        }
    }

    {  // ⚠ THE BUG THIS LADDER WAS BUILT FOR, named on its own so a refactor
       // cannot quietly restore it: a default that names a part the character
       // may not wear falls through to their OWN part. It does NOT get applied
       // just because it resolves, which is what happened until 2026-08-09.
        const auto v = Choose({ false, false, true, false });
        CHECK(v.wear == Wear::kOwn);
        CHECK(v.defaultPassedOver);
    }

    {  // ⚠ AND THE OTHER HALF: the very same default, once the character is
       // back to a race and sex it suits, is honoured again. Nothing was
       // cleared, so nothing has to be re-bought. This is the property that
       // made a filter the right answer instead of clearing on a race change.
        const auto v = Choose({ false, false, true, true });
        CHECK(v.wear == Wear::kDefault);
        CHECK(!v.defaultPassedOver);
    }

    {  // The outfit is the more specific statement and still wins outright,
       // including over a perfectly usable default. Unchanged by the fix.
        CHECK(Choose({ true, true, true, true }).wear == Wear::kOutfit);
    }

    {  // An UNUSABLE outfit part falls through exactly like an unusable
       // default: same rule, both rungs, so an outfit carrying a wrong-sex
       // eye cannot force it on either.
        CHECK(Choose({ true, false, true, true }).wear == Wear::kDefault);
        CHECK(Choose({ true, false, false, false }).wear == Wear::kOwn);
    }

    {  // ⚠ "Passed over" is reported ONLY when a default existed and lost.
       // A character who never set one is not being told anything was skipped,
       // or every row in the editor would carry a sentence about a default
       // that has never existed.
        CHECK(!Choose({ false, false, false, false }).defaultPassedOver);
        CHECK(!Choose({ true, true, false, false }).defaultPassedOver);
        CHECK(Choose({ false, false, true, false }).defaultPassedOver);
    }

    if (g_failures == 0) {
        std::printf("HeadPartLadderTests: all passed\n");
        return 0;
    }
    std::printf("HeadPartLadderTests: %d failure(s)\n", g_failures);
    return 1;
}
