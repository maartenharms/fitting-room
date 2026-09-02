#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace OS::DyeUnlockCard {

    // "You just earned a colour", said on screen while the player is walking
    // around, in a stack of cards at the top right.
    //
    // ⚠⚠ THIS DRAWS DURING GAMEPLAY WITH NO MENU OPEN, and that was the one
    // thing nobody had established when the feature was handed over. It is
    // settled now: FUCK draws a registered IWindow over live gameplay, and
    // BardHero ships six of them on this rig. The requirements it costs are
    // exactly two, and both are load-bearing:
    //
    //   * `IsOpen()` MUST BE A DATA QUERY, not user state. FUCK gates both Draw
    //     and RenderOverlay on it (measured 2026-08-05, DyeTexture.cpp), so a
    //     card that reports itself closed is never drawn. `SetOpen` is therefore
    //     a no-op: nothing outside this file opens or closes it, the queue does.
    //   * `kPassInputToGame` IS MANDATORY. This is a decoration over live
    //     gameplay, and an input-capturing window there reads as a total input
    //     lock rather than as a bug in a notification.
    //
    // ⚠ NOT THE GOLD FOLD, AND IT DOES NOT REPLACE IT. ShowsNewDyeMark marks an
    // earned, unused colour in the grid until the click that spends it. A card
    // is seen once and goes. They answer different questions, and the fold is
    // field confirmed, so nothing here touches it.

    // Register the window. Once, at kDataLoaded, AFTER SettingsUI::Register
    // (which is what calls FUCK::Connect).
    void Register();

    // Announce colours that were just earned, by palette id.
    //
    // ⚠ MAIN THREAD ONLY. It resolves each id against DyePalette here, on the
    // caller's thread, and stores flat copies: the render thread must never
    // reach a palette the main thread can rebuild underneath it.
    //
    // ⚠ IT IS NOT THE PROMOTION PASS'S DECISION WHETHER TO SPEAK. Announcements
    // are off until Arm() is called, so the load-time passes can hand their
    // gains straight in and be silently absorbed as the baseline.
    void Announce(const std::vector<std::string>& a_ids);

    // Start announcing. Called once per load, by the gameplay tick, AFTER its
    // own first pass has been absorbed.
    //
    // ⚠⚠ THE BASELINE, and without it the first thing a returning player sees
    // is a stack of cards for colours they earned last session. Owed by default
    // and taken by the tick rather than by a load event, because a new game
    // started from the main menu fires no kPreLoadGame
    // (coc-from-main-menu-skips-newgame) and the tick runs down every one of
    // those paths. DyeUnlocks::TakeAckBaselineIfOwed makes the same argument for
    // the gold fold and is the shape this copies.
    void Arm();
    [[nodiscard]] bool Armed();

    // Drop every card and disarm. The load boundary, and the setting going off.
    //
    // ⚠ DyeUnlocks::Forget runs at kPreLoadGame and at the top of every Request,
    // and a notifier holding cards across that boundary would announce the
    // outgoing character's colours over the incoming one's first frames.
    void Forget();

    // Re-read dwell, stack height and the on/off switch from the INI. Called at
    // startup and whenever the settings panel commits.
    void ApplySettings();

    // Queue a sample stack so the player can SEE what the setting they just
    // ticked actually does.
    //
    // ⚠⚠ THIS EXISTS BECAUSE THE FEATURE IS OTHERWISE UNOBSERVABLE, and that is
    // a measurement rather than a worry. Field run 2026-08-14: registration,
    // the tick and the arming all logged correctly and every pass for three
    // minutes reported "nothing newly earned, 122 held". A character who has
    // earned most of the palette has nothing left for the tick to hand over, so
    // there is no way to check the card is alive short of playing until a gate
    // happens to open. Reported by the user as "i don't see any notifications",
    // which is exactly what a working feature with nothing to say looks like.
    //
    // ⚠ IT DRAWS REAL COLOURS OFF THE PALETTE, never invented ones, so the
    // preview answers the questions a preview is for: does the swatch read,
    // does a two-stop dye show both stops, is the dwell right, does the stack
    // clear the compass. A card full of placeholder grey would answer none of
    // them.
    //
    // ⚠ Bypasses the armed gate on purpose. It is not an announcement, it is a
    // demonstration, and the baseline exists to stop the LOAD talking rather
    // than to stop the player asking.
    //
    // ⚠ ANY THREAD, and the settings panel calls it from the RENDER one.
    // DyePalette::Snapshot takes its own lock and hands back a copy, which is
    // this codebase's uniform contract for exactly this, and the queue push is
    // under the same lock Draw uses. Unlike Announce, which resolves ids off a
    // promotion pass on the game thread, nothing here touches the engine.
    void Preview();

}  // namespace OS::DyeUnlockCard
