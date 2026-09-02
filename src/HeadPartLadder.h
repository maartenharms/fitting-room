#pragma once

#include <cstdint>

// WHICH head part a character ends up wearing: the outfit's, their character
// default, or their own.
//
// Engine-free and pure for HeadPartPlan.h's reason: the precedence below is
// what OutfitSession's push lambda calls "the whole feature", and until now it
// lived inline in an engine-only function that no test can compile.
//
// ⚠⚠ A RUNG IS TAKEN ONLY IF IT IS BOTH NAMED AND USABLE, and the second half
// is the fix this file was added for (2026-08-09). It used to be "named", full
// stop: a stored default naming a part the character's race and sex would NOT
// be offered was resolved and applied anyway. The engine does not police it -
// TESNPC::ChangeHeadPart matches on head-part TYPE alone and never reads the
// part's sex flags or race list (decompiled on both runtimes) - so a female
// eye landed on a male character's record and stayed there.
//
// ⚠ THE HONOUR RULE IS THE PICK RULE, deliberately and structurally: usability
// is HeadPartPlan::Judge, the same function on the same Candidate the browser
// builds its list from. The editor can never wear a part it would not let you
// pick, and the two cannot drift because there is only one of them.
//
// ⚠ NOTHING IS LATCHED OR STORED. The verdict is recomputed on every push, so
// a character who changes back wears their default again with nothing spent
// and nothing restored. That is the whole reason this is a filter and not a
// "clear the defaults on a race change" event: setting a default COSTS the
// player gold or Seamstone charge (DefaultLook.h), so deleting one on an act
// the player can undo would destroy something they bought.
// See [[persisted-verdicts-must-not-key-on-environment]] for why the verdict
// is not remembered either.
namespace OS::HeadPartLadder {

    enum class Wear : std::uint8_t {
        kOutfit,   // the outfit named one and it is usable
        kDefault,  // the outfit named none; the character default is usable
        kOwn,      // neither, so restore whatever they had before we touched it
    };

    // What each rung offers. "Names" is "a reference is stored and it still
    // resolves to a part"; "usable" is "that part passes Judge for this
    // character right now". Usable is meaningless when nothing is named and
    // callers pass false for it.
    struct Rungs {
        bool outfitNames{ false };
        bool outfitUsable{ false };
        bool defaultNames{ false };
        bool defaultUsable{ false };
    };

    struct Verdict {
        Wear wear{ Wear::kOwn };
        // A default was stored and lost anyway. The editor says so on the row,
        // because "your paid default is dormant, here is why" is the whole
        // difference between this and the part silently going missing.
        bool defaultPassedOver{ false };
    };

    [[nodiscard]] inline constexpr Verdict Choose(const Rungs& a_r) {
        // ⚠ THE OUTFIT WINS EVEN OVER A USABLE DEFAULT, unchanged: it is the
        // more specific statement. It only loses when it is unusable, which
        // is the same fall-through a default gets.
        if (a_r.outfitNames && a_r.outfitUsable) {
            return { Wear::kOutfit, false };
        }
        if (a_r.defaultNames && a_r.defaultUsable) {
            return { Wear::kDefault, false };
        }
        // ⚠ REPORTED ONLY WHEN A DEFAULT ACTUALLY LOST. A character with no
        // default at all is not "passed over", and saying so would put a
        // sentence on every row that has never had one.
        return { Wear::kOwn, a_r.defaultNames && !a_r.defaultUsable };
    }

}  // namespace OS::HeadPartLadder
