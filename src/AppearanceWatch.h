#pragma once
#include "PCH.h"

// One watchdog over everything that paints the player, sampling once a second
// and logging only what CHANGED.
//
// It replaces the eight single-question probes the cross-save hunt accumulated
// (a dump at the boundary and seven timed rungs behind it). Those answered
// "what does this field hold at +12 s", which is the wrong question when the
// fault is a write nobody has named: the rungs miss anything between two of
// them, they print a full page whether or not a single value moved, and r79
// caught them lying about their own timing, with post-load+45s and
// post-load+60s firing 0.8 s after post-load because a previous load's timers
// were still running against the new one.
//
// This asks one question instead: WHO CHANGED, WHEN, AND FROM WHAT. Each line
// names the field, the old value, the new value and the time since the last
// load boundary, so a field round produces one timeline rather than four
// hunts.
//
// ⚠ IT IS PERMANENT, unlike every probe that came before it, and it is behind
// [Debug] bAppearanceWatch. A transition-only instrument stays quiet on a
// settled save and is worth having the next time a painter starts drifting.
namespace OS::AppearanceWatch {

    // From kDataLoaded. Starts the sampler once; later calls do nothing.
    void Start();

    // From every load boundary: the revert edge and kPostLoadGame. Resets the
    // clock the transitions are timed against and makes the next sample print
    // a full baseline instead of a diff, because after a boundary every field
    // has legitimately moved and diffing against the outgoing character says
    // nothing.
    void NoteLoadBoundary(const char* a_why);

}  // namespace OS::AppearanceWatch
