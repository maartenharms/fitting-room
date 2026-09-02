// Which deed names name a SKYRIM stat and which name one of Fitting Room's own
// counters. No SKSE, no engine: it is a prefix rule and the consequences of
// getting it wrong are the whole reason it is a named function with tests.
#include "DyeStatDeed.h"

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

using namespace OS;

int main() {
    // ---- the prefix separates the two producers ----------------------------
    // Fitting Room counts its own deeds; Skyrim counts its misc stats. One
    // "deed" clause kind reaches both, so the NAME has to say which, or a typo
    // in either vocabulary silently borrows the other's producer and reads 0.
    {
        CHECK(StatNameFromDeed("stat:Dungeons Cleared") == "Dungeons Cleared");
        CHECK(StatNameFromDeed("stat:Locations Discovered") ==
              "Locations Discovered");

        // Fitting Room's own counter is NOT a stat and must not be dispatched
        // to the engine, which would answer 0 and lock every colour that gates
        // on it.
        CHECK(!StatNameFromDeed("channelsDyed").has_value());
    }

    // ---- the name is taken VERBATIM ----------------------------------------
    // ⚠⚠ THIS IS THE TEST THAT MATTERS. Game.QueryStat answers int 0 for a name
    // the engine does not know, which is byte for byte the answer an honest
    // zero gives, so a name this function "helpfully" tidied would lock its
    // colour for the life of every character with nothing logged anywhere.
    // Three of the eight questline counters carry a mark or a word nobody
    // guesses, and all three are exercised here:
    {
        CHECK(StatNameFromDeed("stat:Thieves' Guild Quests Completed") ==
              "Thieves' Guild Quests Completed");
        CHECK(StatNameFromDeed("stat:The Companions Quests Completed") ==
              "The Companions Quests Completed");
        CHECK(StatNameFromDeed("stat:The Dark Brotherhood Quests Completed") ==
              "The Dark Brotherhood Quests Completed");

        // Whitespace is part of the name, not noise to be trimmed. A trim here
        // would make this function DISAGREE with tools/check_dye_rules.py,
        // which compares the authored text against the index byte for byte: the
        // script would pass a name the engine then fails to recognise. One
        // spelling, checked in one place.
        CHECK(StatNameFromDeed("stat: Dungeons Cleared") == " Dungeons Cleared");
        CHECK(StatNameFromDeed("stat:Dungeons Cleared ") == "Dungeons Cleared ");

        // A colon inside the name survives; only the FIRST prefix is consumed.
        CHECK(StatNameFromDeed("stat:stat:odd") == "stat:odd");
    }

    // ---- everything that is not the prefix -------------------------------
    // ⚠ NOT A STAT reads as one of Fitting Room's own deeds, which is the
    // fail-safe direction: an unknown FR counter reads 0 and locks, and locked
    // is one fixed file away from earned. A wrongly DISPATCHED name would also
    // read 0, so both directions lock; what the distinction buys is that the
    // log can say which vocabulary the author meant.
    {
        CHECK(!StatNameFromDeed("").has_value());
        CHECK(!StatNameFromDeed("stat").has_value());
        CHECK(!StatNameFromDeed("stat:").has_value());   // prefix, no name
        CHECK(!StatNameFromDeed("Stat:Dungeons Cleared").has_value());  // case
        CHECK(!StatNameFromDeed("STAT:Dungeons Cleared").has_value());
        CHECK(!StatNameFromDeed(" stat:Dungeons Cleared").has_value());
        CHECK(!StatNameFromDeed("xstat:Dungeons Cleared").has_value());
        CHECK(!StatNameFromDeed("Dungeons Cleared").has_value());
    }

    // ---- the view points into the caller's storage -------------------------
    // It is a view, so it is only valid while the argument is. Pinned because
    // the bridge holds these to build its dispatch list and a copy is cheap.
    {
        const std::string owned = "stat:Dragon Souls Collected";
        const auto        name  = StatNameFromDeed(owned);
        CHECK(name.has_value());
        CHECK(name->data() == owned.data() + kStatDeedPrefix.size());
    }

    if (g_failures == 0) {
        std::printf("DyeStatDeedTests: all passed\n");
        return 0;
    }
    std::printf("DyeStatDeedTests: %d failure(s)\n", g_failures);
    return 1;
}
