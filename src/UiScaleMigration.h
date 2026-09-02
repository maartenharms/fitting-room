#pragma once

#include <cmath>

// Whether a stored fUiScale is a preference or a leftover, split out of
// Settings.h so a test can reach it.
//
// ⚠ SETTINGS.H INCLUDES PCH.H AND THEREFORE THE WHOLE ENGINE, which is why this
// is its own file rather than a section of that one, the same way CostMode.h
// and PagePolicy.h are. A decision that can be wrong belongs where a test
// executable can run it.
//
// ⚠⚠ THE PROBLEM THIS EXISTS FOR: Settings::Save WRITES fUiScale, so every
// install that has ever run the mod has the DEFAULT OF ITS DAY pinned in the
// file, and live-ini-overrides-code-defaults means no later default can ever
// reach it. kUiScaleDefault was 0.8 from release 0.3.0 (39bb5f23, 2026-07-29)
// and 0.7 from 2026-08-08, 0.65 from 2026-08-14 and 0.63 from 2026-08-31, so
// "the editor came up at 0.8" is an install DATE rather than a preference (user
// 2026-08-27, "editor UI scale when i installed the mod could sometimes be 0.8,
// we need to make sure it's .65 always by default").
//
// ⚠ AND THE CLAMP LEADS TO THE SAME PLACE. The slider's range was once 0.8 to
// 1.6 (OS-17) and is 0.5 to 0.8 now, so an INI from that era clamps to exactly
// 0.8 on load whatever it actually said. Both roads end on the same number,
// which is why the check below runs AFTER the clamp rather than on the raw
// value: one test covers both.
namespace OS::UiScale {

    // Bumped when a load-time migration is added. An INI with no stamp was
    // written before any of them existed.
    //
    // ⚠ THE STAMP IS THE ONLY THING SEPARATING A LEFTOVER FROM A CHOICE. 0.8 is
    // also kUiScaleMax, so someone who deliberately dragged the slider to the
    // top looks exactly like someone handed it in July, and no amount of
    // reading the value can tell them apart. Once a build that writes this
    // stamp has saved, every later 0.8 is known to be deliberate and is left
    // alone for good.
    // ⚠⚠ VERSION 2, 2026-08-31, BECAUSE THE DEFAULT MOVED AGAIN, 0.65 to 0.63.
    // A stamp of 1 means "this install was seen by the build that migrated 0.8
    // and 0.7", and every one of those has 0.65 pinned in its file by
    // Settings::Save. Leaving the stamp at 1 would hand 0.63 to fresh installs
    // only and leave every existing player on the old number, which is the same
    // shape as the fault this file was written for.
    //
    // ⚠ THE COST IS NAMED RATHER THAN HIDDEN: 0.65 sits mid-range, so unlike
    // 0.8 it CAN have been chosen deliberately, and this migration moves those
    // players too. It is two hundredths on a slider they can drag back, against
    // every install that never chose anything sitting on a stale default for
    // good. The user's own call on the 0.8 round is the precedent: "we need to
    // make sure it's .65 always by default".
    inline constexpr int kSettingsVersion = 2;

    // The scales that were once handed out as kUiScaleDefault.
    //
    // ⚠ ONLY THE HANDED-OUT ONES, and that restraint is the whole care taken
    // here. A player who moved the slider to 0.75 chose it, and a migration
    // that reset every legacy value would throw that away to fix a number they
    // never picked.
    inline constexpr float kHandedOut[] = { 0.8f, 0.7f, 0.65f };

    [[nodiscard]] inline bool WasHandedOut(float a_scale) {
        for (const float handed : kHandedOut) {
            // The INI stores six decimals and parses back through double, so
            // this compares within a tolerance far tighter than the slider's
            // own step and far looser than the round trip's error.
            if (std::fabs(a_scale - handed) < 0.0005f) {
                return true;
            }
        }
        return false;
    }

    // What a stored fUiScale should become on load. a_stored has already been
    // clamped into the current range; see the note above for why that order
    // matters.
    [[nodiscard]] inline float Adopt(float a_stored, int a_iniVersion,
                                     float a_default) {
        if (a_iniVersion >= kSettingsVersion) {
            return a_stored;  // stamped, so this is a choice
        }
        return WasHandedOut(a_stored) ? a_default : a_stored;
    }

}  // namespace OS::UiScale
