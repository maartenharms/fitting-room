#pragma once

#include <cstdint>

// Whether a BGSHeadPart should be OFFERED to a given actor as a choice of the
// head-part type being edited: hair, eyes or brows.
//
// Engine-free on purpose, the same way NpcHairPlan.h is, because the loop this
// rule came out of needs TESDataHandler and can therefore never be unit tested
// (see tests/test_headpartplan.cpp). The lookup stays in HeadPart.cpp; the
// decision lives here, where it can be wrong in a way a test can catch.
//
// ⚠ TWO OF THESE RULES INVERT THE OBVIOUS DEFAULT, and both of them empty the
// list when they are read the natural way round. They are not defensive
// guesses; they are how the records are actually authored:
//
//   * A part with NEITHER sex flag set does not restrict by sex, so it is valid
//     for both. Reading an unset flag as "not for this sex" hides most modded
//     hair, which ships without the flags.
//   * A part with NO validRaces list is unrestricted, not invalid-for-everyone.
//     Modded parts frequently ship that way, so excluding them empties the list
//     for exactly the load orders that have the most to offer.
//
// ⚠ AND THE REJECTION IS NAMED RATHER THAN BOOLEAN. HairStyle.cpp records a
// bug where 26 vanilla parts were being discarded while the list still looked
// healthy, because a total cannot say what was dropped or why. The caller
// counts these by reason and logs the breakdown, which is what makes the next
// one visible on the first report instead of the third.
namespace OS::HeadPartPlan {

    enum class Reject : std::uint8_t {
        kNone,         // offer it
        kWrongType,    // a different head-part type entirely
        kNotPlayable,  // the record is not offered in character creation
        kExtraPart,    // rides inside a chosen part (hairlines), never picked directly
        kWrongSex,
        kWrongRace,
    };

    // Everything about one candidate record that bears on the decision, read
    // off the form by the caller. Deliberately plain bools rather than the
    // engine's flag set, so the rule cannot accidentally start depending on
    // anything else the form carries.
    struct Candidate {
        bool typeMatches{ false };  // part->type == the type being edited
        bool playable{ false };     // Flag::kPlayable
        bool extraPart{ false };    // IsExtraPart()
        bool flagMale{ false };     // Flag::kMale
        bool flagFemale{ false };   // Flag::kFemale
        bool hasRaceList{ false };  // validRaces != nullptr
        // ...and it contains this actor's race, OR the ARMOUR race that race
        // points at. See HeadPart.cpp: a custom race points RNAM at a vanilla
        // one precisely so ordinary content works on it, and no hair mod lists
        // a follower mod's invented race.
        bool raceListed{ false };
        // ⚠⚠ NOT A PROPERTY OF THIS RECORD. It is a property of the ACTOR and
        // the part TYPE: true when NOTHING of this type in the whole load order
        // names this actor's race or its armour race, so every race list there
        // is has nothing to say about this character and the filter can only
        // return an empty page. The caller works it out once per race and type
        // and copies it onto every candidate.
        //
        // ⚠ A FALLBACK, NOT A SECOND RULE. While one single part names the
        // race, this is false and the filter works exactly as it always has -
        // so a race a follower mod DOES list keeps getting the parts meant for
        // it. It only opens up when the alternative is offering nothing at all.
        bool raceListsSayNothing{ false };
    };

    [[nodiscard]] inline Reject Judge(const Candidate& a_c, bool a_actorIsFemale) {
        // Ordered cheapest and broadest first: type discards almost the whole
        // form array on every call, and there are thousands of head parts in a
        // load order this size.
        if (!a_c.typeMatches) {
            return Reject::kWrongType;
        }
        if (!a_c.playable) {
            return Reject::kNotPlayable;
        }
        if (a_c.extraPart) {
            return Reject::kExtraPart;
        }
        // Neither flag set = unrestricted. See the header note; reading this
        // the other way round is what hides a load order's modded parts.
        if (a_c.flagMale || a_c.flagFemale) {
            if (a_actorIsFemale ? !a_c.flagFemale : !a_c.flagMale) {
                return Reject::kWrongSex;
            }
        }
        // No list = unrestricted, for the same reason.
        //
        // ⚠⚠ AND A LIST THAT NAMES NOBODY LIKE THIS ACTOR IS ALSO NO LIST.
        // Measured on the user's load order 2026-08-19: a follower on
        // 'AK69KatanaRace', a race the follower mod invented, was offered 0 hair
        // parts out of 2485 and the log read "from 0 plugin(s)". Every hair in
        // a normal load order carries a validRaces list naming the vanilla
        // playable races, so a race invented last week is in none of them and
        // the rule rejected the entire library. Zero out of thousands is not a
        // filter working, it is a filter with nothing to filter ON.
        //
        // The two ways out are both here rather than in one place: raceListed
        // now also accepts the armour race, which is what settles it for a
        // follower whose race points RNAM at NordRace, and raceListsSayNothing
        // catches the rest, where not even the armour race is named anywhere.
        if (a_c.hasRaceList && !a_c.raceListed && !a_c.raceListsSayNothing) {
            return Reject::kWrongRace;
        }
        return Reject::kNone;
    }

    [[nodiscard]] inline const char* ReasonName(Reject a_r) {
        switch (a_r) {
            case Reject::kNone:        return "offered";
            case Reject::kWrongType:   return "wrong type";
            case Reject::kNotPlayable: return "not playable";
            case Reject::kExtraPart:   return "extra part";
            case Reject::kWrongSex:    return "wrong sex";
            case Reject::kWrongRace:   return "wrong race";
        }
        return "unknown";
    }

}  // namespace OS::HeadPartPlan
