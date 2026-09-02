#pragma once

namespace OS::NpcHairSettle {

    // The drain for NpcHair's post-apply settle ladder (the cell-return
    // doubled hair, 2026-08-21; the finding lives in NpcHair.h above
    // TickSettleChecks).
    //
    // ⚠ A TICK NEEDS A HOST THAT RUNS WITH THE EDITOR SHUT, and this module
    // is that host and nothing else: an invisible FUCK IWindow whose IsOpen
    // is "ladders are pending" and whose Draw is one TickSettleChecks call.
    // The seam is the one the dye cards prove every session (FUCK draws
    // registered windows over live gameplay); the cost when idle is the
    // same one the card window already pays, a data-query IsOpen per frame.
    //
    // The two shapes this deliberately is not:
    //   * NOT a third detached timer thread beside WorldWatch's and
    //     DyeTick's. Those run for the process; this work exists for about
    //     eight seconds after a hair apply and then never again.
    //   * NOT a rider on either of those timers. Hanging one feature off
    //     another's clock ties them together at their timers, the coupling
    //     DyeTick's own header refuses.
    //
    // Register after FUCK::Connect, beside the other window registrations.
    // Safe to call when FUCK is absent: it says so once and the settle
    // ladders then simply never drain, which degrades to exactly the
    // pre-fix behaviour.
    void Register();

}  // namespace OS::NpcHairSettle
