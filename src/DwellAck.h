#pragma once

// "The pointer has been on this since it arrived", held as a piece of state a
// call site can own, so a mark can be acknowledged ONCE per hover.
//
// ⚠⚠ THE DELAY SHIPS AT ZERO, AND THAT IS THE USER'S CALL (2026-08-12). This
// went out with a 0.45 s dwell first, on the reasoning below, and the field
// answer was "make it go away on hover, no timer". So the edge is what earns
// its keep now: without it the acknowledge would be re-issued every frame the
// pointer sat still, sixty co-save writes a second for one glance.
//
// ⚠ WHAT THE DWELL WAS PROTECTING, because it is still true and the constant
// is still here. Dragging the pointer across a palette of several hundred
// swatches on the way somewhere else passes over dozens of them, and with no
// delay every one of them that holds the pointer for two frames is
// acknowledged. Restoring it is one number, and the sweep case is still tested
// below the zero-delay one.
//
// ⚠ FLICK CANNOT ANSWER "HAS THE TOOLTIP BEEN SHOWN", which is what the ask
// literally asked for. Measured 2026-08-12 against the whole of FUCK_API.h: it
// exposes SetTooltip, BeginTooltip and EndTooltip and no query of any kind for
// a tip's visibility or an item's hover age. It would not help if it did. This
// editor emits its tooltips with no delay at all - the swatch's inside
// `if (swHot)`, the card's on `r.hovered`, the row's on `hovered && !overStar`
// - so the tooltip and the hover begin on the same frame, and clearing on
// hover IS clearing when the tooltip appears.
//
// ⚠ THE TRANSITION IS DyePreview::Decide's, NOT A SECOND ONE. That function is
// pure, tested and names precisely this sequence. What is added here is the
// state each call site would otherwise juggle by hand, and the one-shot EDGE,
// which is the part an acknowledgement needs and a preview does not: a preview
// is a level that is either armed or not, and an acknowledgement is a write
// that must land once per hover.
//
// ⚠⚠ IT MUST NOT BORROW THE PREVIEW'S OWN STATE, and that is the mistake this
// header exists to make impossible. The dye preview's candidate is gated on
// `selPaintable && g_hoverPreview && !selKeyOnHair`, and the style preview's
// whole resolve sits behind HoverPreviewOn(), !AnyHeadPartSelected() and
// !alreadyChosen. An acknowledgement riding either would leave every gold mark
// permanently unclearable for a player who turned hover preview off. The
// invariant is the one the swatch's own unlocks-on term already carries: the
// mark may only be drawn where the thing that clears it can succeed.

#include "DyePreview.h"  // Decide: the dwell transition, pure and already tested

namespace OS {

    // How long a mark-bearing thing sits under the pointer before its mark
    // counts as read. ZERO: the mark goes on contact, which is what the field
    // asked for after trying 0.45 s.
    //
    // ⚠ ZERO IS NOT INSTANT, IT IS THE NEXT FRAME. The first frame on a new
    // thing starts the dwell and the second one completes it, because the
    // transition has to see the same candidate twice to know the pointer did
    // not merely pass through. At 60 fps that is 17 ms, which is nobody's
    // "timer", and it is what keeps a fast sweep from banking every colour it
    // crosses.
    inline constexpr double kAckDwellSeconds = 0.0;

    // The dwell as state. Id is whatever names the thing under the pointer: a
    // dye's id, a look's key. It needs equality, default construction and copy,
    // and nothing else.
    template <class Id>
    class DwellAck {
    public:
        // True on exactly ONE frame per hover: the frame the dwell completes.
        // Holding still afterwards returns false, so a caller may do the
        // irreversible thing here without a gate of its own.
        [[nodiscard]] bool Rested(bool a_hasCandidate, const Id& a_candidate, double a_now,
                                  double a_delay = kAckDwellSeconds) {
            const bool sameArmed   = m_armed && a_candidate == m_id;
            const bool samePending = m_pendingSet && a_candidate == m_pending;
            switch (DyePreview::Decide(a_hasCandidate, sameArmed, samePending, a_now, m_since,
                                       a_delay)) {
                case DyePreview::Hover::kClear:
                    // ⚠ LEAVING FORGETS, so coming back fires again. That is
                    // deliberate rather than tidy: the write is
                    // idempotent, and the alternative is remembering one id
                    // forever to save a call that costs nothing.
                    Forget();
                    return false;
                case DyePreview::Hover::kKeep:
                    return false;
                case DyePreview::Hover::kRepend:
                    m_pending    = a_candidate;
                    m_pendingSet = true;
                    m_since      = a_now;
                    return false;
                case DyePreview::Hover::kArm:
                    m_armed = true;
                    m_id    = a_candidate;
                    return true;
            }
            return false;
        }

        // Drop the dwell: the pane went away, or whoever is being dressed did.
        void Forget() {
            m_armed      = false;
            m_pendingSet = false;
            m_id         = Id{};
            m_pending    = Id{};
            m_since      = 0.0;
        }

    private:
        Id     m_id{};       // banked for this rest: never fires twice
        Id     m_pending{};  // dwelling, not yet long enough
        double m_since{ 0.0 };
        bool   m_armed{ false };
        bool   m_pendingSet{ false };
    };

}  // namespace OS
