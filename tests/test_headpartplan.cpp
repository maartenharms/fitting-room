// Pure-logic tests for which head parts get OFFERED (HeadPartPlan.h). No
// engine, no RE:: types - just the decision HeadPart::AvailableFor's loop
// reduces to for one candidate record.
//
// The cases that carry this file are the two inverted defaults. Both of them
// are silent when read the natural way round: the list simply comes back short,
// and a short list of hairstyles looks like a load order with few hairstyles.
// HairStyle.cpp already records one of these shipping (26 vanilla parts
// discarded while the browser looked healthy because modded parts filled it),
// which is the whole reason the rule was pulled out here to be tested at all.
#include "HeadPartPlan.h"

#include <cstdio>
#include <string_view>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace {

    constexpr bool kFemale = true;
    constexpr bool kMale   = false;

    // A record that passes everything, so each test can spoil exactly one
    // thing and the failure names that one thing.
    OS::HeadPartPlan::Candidate Good() {
        OS::HeadPartPlan::Candidate c;
        c.typeMatches = true;
        c.playable    = true;
        c.extraPart   = false;
        c.flagMale    = false;
        c.flagFemale  = false;
        c.hasRaceList = false;
        c.raceListed  = false;
        return c;
    }

}  // namespace

int main() {
    using namespace OS::HeadPartPlan;

    {  // The baseline: an unrestricted playable part of the right type is
       // offered to anyone. This is the shape most modded records actually
       // have, which is why it is the baseline rather than an edge case.
        CHECK(Judge(Good(), kFemale) == Reject::kNone);
        CHECK(Judge(Good(), kMale) == Reject::kNone);
    }

    {  // ⚠ INVERTED DEFAULT 1: neither sex flag means BOTH sexes, not neither.
       // Reading an unset kFemale as "not for her" is what hides most of a
       // modded load order, and it hides it silently.
        auto unflagged = Good();
        unflagged.flagMale   = false;
        unflagged.flagFemale = false;
        CHECK(Judge(unflagged, kFemale) == Reject::kNone);
        CHECK(Judge(unflagged, kMale) == Reject::kNone);
    }

    {  // Once EITHER flag is set the record is restricting, and it is honoured
       // in both directions.
        auto femaleOnly = Good();
        femaleOnly.flagFemale = true;
        CHECK(Judge(femaleOnly, kFemale) == Reject::kNone);
        CHECK(Judge(femaleOnly, kMale) == Reject::kWrongSex);

        auto maleOnly = Good();
        maleOnly.flagMale = true;
        CHECK(Judge(maleOnly, kMale) == Reject::kNone);
        CHECK(Judge(maleOnly, kFemale) == Reject::kWrongSex);

        // Both flags set is a record that genuinely serves both, and must not
        // be mistaken for the restricting case.
        auto both = Good();
        both.flagMale   = true;
        both.flagFemale = true;
        CHECK(Judge(both, kFemale) == Reject::kNone);
        CHECK(Judge(both, kMale) == Reject::kNone);
    }

    {  // ⚠ INVERTED DEFAULT 2: no validRaces list means unrestricted, not
       // invalid for everyone. The failure mode is identical to the sex one -
       // a shorter list, no error - and it bites hardest on the load orders
       // carrying the most modded parts.
        auto noList = Good();
        noList.hasRaceList = false;
        noList.raceListed  = false;  // meaningless without a list, and must stay so
        CHECK(Judge(noList, kFemale) == Reject::kNone);
    }

    {  // With a list present it is honoured.
        auto listed = Good();
        listed.hasRaceList = true;
        listed.raceListed  = true;
        CHECK(Judge(listed, kFemale) == Reject::kNone);

        auto notListed = Good();
        notListed.hasRaceList = true;
        notListed.raceListed  = false;
        CHECK(Judge(notListed, kFemale) == Reject::kWrongRace);
    }

    {  // The three flat exclusions.
        auto wrongType = Good();
        wrongType.typeMatches = false;
        CHECK(Judge(wrongType, kFemale) == Reject::kWrongType);

        auto notPlayable = Good();
        notPlayable.playable = false;
        CHECK(Judge(notPlayable, kFemale) == Reject::kNotPlayable);

        // Hairlines and their equivalents ride inside a chosen part's
        // extraParts. Offering one directly lets the user pick half a look.
        auto extra = Good();
        extra.extraPart = true;
        CHECK(Judge(extra, kFemale) == Reject::kExtraPart);
    }

    {  // Type is judged before everything else, because it discards nearly the
       // whole form array on every call and the array is thousands long. A
       // wrong-type record that is ALSO unplayable must still say wrong type,
       // or the logged breakdown misattributes the bulk of the discards.
        auto c = Good();
        c.typeMatches = false;
        c.playable    = false;
        c.extraPart   = true;
        CHECK(Judge(c, kFemale) == Reject::kWrongType);
    }

    {  // ⚠ A LIST THAT NAMES THIS ACTOR NOWHERE IS NOT A FILTER. The field
       // case is a follower on a race her own mod invented: every hair in the
       // load order lists the vanilla races, none of them lists hers, and the
       // browser came back with 0 of 2485 (user 2026-08-19). The caller works
       // that out once per race and type; the rule's job is to stop rejecting
       // once it is told.
        auto blind = Good();
        blind.hasRaceList         = true;
        blind.raceListed          = false;
        blind.raceListsSayNothing = true;
        CHECK(Judge(blind, kFemale) == Reject::kNone);
        CHECK(Judge(blind, kMale) == Reject::kNone);
    }

    {  // ⚠⚠ AND IT MUST NOT OPEN THE GATE WHILE THE LISTS DO SAY SOMETHING,
       // which is the whole of holding a vanilla character at 2485. A Nord is
       // named by thousands of parts, so raceListsSayNothing is false for her
       // and a part that lists only Orcs is still refused - exactly as it was
       // before the fallback existed.
        auto other = Good();
        other.hasRaceList         = true;
        other.raceListed          = false;
        other.raceListsSayNothing = false;
        CHECK(Judge(other, kFemale) == Reject::kWrongRace);
    }

    {  // The fallback changes nothing for a part that DOES name the actor,
       // whether it named her race or the armour race behind it: the caller
       // folds both into raceListed, so this rule sees one answer.
        auto listed = Good();
        listed.hasRaceList         = true;
        listed.raceListed          = true;
        listed.raceListsSayNothing = false;
        CHECK(Judge(listed, kFemale) == Reject::kNone);
        listed.raceListsSayNothing = true;
        CHECK(Judge(listed, kFemale) == Reject::kNone);
    }

    {  // ⚠ AND IT IS THE LAST RULE, NOT THE FIRST. A part of the wrong sex is
       // still the wrong sex on a race nothing names, or a follower on an
       // invented race would be offered every man's beard in the load order.
        auto wrongSex = Good();
        wrongSex.flagMale           = true;
        wrongSex.flagFemale         = false;
        wrongSex.hasRaceList        = true;
        wrongSex.raceListed         = false;
        wrongSex.raceListsSayNothing = true;
        CHECK(Judge(wrongSex, kFemale) == Reject::kWrongSex);
    }

    {  // Every reason has a name. The breakdown is logged, and an unnamed
       // reason there is the same silent-total problem in a different place.
        CHECK(std::string_view{ ReasonName(Reject::kNone) } != "unknown");
        CHECK(std::string_view{ ReasonName(Reject::kWrongType) } != "unknown");
        CHECK(std::string_view{ ReasonName(Reject::kNotPlayable) } != "unknown");
        CHECK(std::string_view{ ReasonName(Reject::kExtraPart) } != "unknown");
        CHECK(std::string_view{ ReasonName(Reject::kWrongSex) } != "unknown");
        CHECK(std::string_view{ ReasonName(Reject::kWrongRace) } != "unknown");
    }

    if (g_failures == 0) {
        std::printf("test_headpartplan: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
