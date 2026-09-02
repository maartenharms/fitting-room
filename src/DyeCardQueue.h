#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace OS {

    // The unlock notification's timing and stacking, with no engine, no FUCK and
    // no clock of its own. Everything that decides WHEN a card appears, how long
    // it sits and what happens when a hundred arrive at once lives here so it can
    // be tested without a game.
    //
    // ⚠ TIME IS PASSED IN, NEVER READ. Every method that cares takes `a_now`, so
    // a test can run a burst of forty colours through six minutes of wall clock
    // in a microsecond, and so the window can drive this from FUCK::GetTime()
    // without this file knowing what FUCK is.
    //
    // ⚠ ABSOLUTE TIMESTAMPS, NOT ACCUMULATED DELTAS, and that is a behaviour
    // choice rather than a style one. The card window carries kCloseOnGameMenu,
    // which HIDES rather than closes: Draw simply stops being called while a
    // native menu is up (fuck-close-on-game-menu-hides). Under accumulated deltas
    // a card would freeze mid-dwell behind the inventory and still be sitting
    // there on the way out, minutes later. Under absolute time it expires on
    // schedule, unseen, which is what a loot notification does when you are not
    // looking at the screen.

    enum class DyeCardKind : std::uint8_t {
        kColour,  // one earned colour, drawn with its swatch
        kMore     // the overflow tail: "and N more"
    };

    // One entry on screen. Flat and copyable: it is snapshotted under a lock and
    // drawn on the render thread, so it must not reference palette storage that
    // the main thread can rebuild underneath it.
    struct DyeCard {
        DyeCardKind kind{ DyeCardKind::kColour };

        // Kept for the log line and for de-duplication. Never player-facing:
        // the id is `pack:name` and the display name comes off the palette.
        std::string id;
        std::string name;
        std::string rarity;

        std::uint8_t r{ 0 };
        std::uint8_t g{ 0 };
        std::uint8_t b{ 0 };

        // The second stop, for a pearlescent or metallic colour. The grid draws
        // both stops because a flat chip cannot tell those apart in a 306 colour
        // palette, and a card that showed only the first stop would name a
        // colour the swatch does not match.
        bool         secondSet{ false };
        std::uint8_t r2{ 0 };
        std::uint8_t g2{ 0 };
        std::uint8_t b2{ 0 };

        // How many colours this tail stands for. Zero on a kColour card.
        std::size_t more{ 0 };

        // When this card reached a visible slot. Zero while it is still waiting.
        double shownAt{ 0.0 };
    };

    struct DyeCardTiming {
        // How many sit on screen together. Past about five the stack reaches
        // the compass and stops reading as a notification.
        std::size_t maxVisible{ 5 };
        // How long a card holds at full opacity once it has finished arriving.
        double dwellSec{ 6.0 };
        // The fade out at the end.
        double fadeSec{ 0.6 };
        // The fade in at the start.
        double fadeInSec{ 0.10 };
        // How long the card takes to slide in from past the screen edge.
        //
        // ⚠⚠ TWO CLOCKS, AND THEY USED TO BE ONE. Both were fadeSec * 0.4, so a
        // card crossed a whole card width in 0.24 s while fading up from nothing
        // over that identical span: at the halfway point it was at half opacity
        // and already within a quarter of a width of home, and by the time it was
        // solid enough to see, the travel was over. The motion was real and the
        // arithmetic hid it, which is why the field asked for a card that "comes
        // out from the edge" over a build that already slid one. The fade is
        // short now and the slide is long, so the plate is at full strength while
        // it travels and the window's clip rect is what reveals it.
        //
        // ⚠ NOT PART OF LifeSec, and it does not need to be: the slide runs under
        // the alpha hold, and dwellSec is clamped to 2 seconds at the setting, so
        // a card is home long before it is retired.
        double entrySec{ 0.40 };
        // The gap between two cards entering, so a batch reads as a stream
        // rather than as a wall appearing in one frame.
        double staggerSec{ 0.35 };
        // Past this many waiting, the rest collapse into one "and N more" tail.
        //
        // ⚠ THE TAIL IS THE DISCLOSURE, not a silent truncation. A questline
        // completion can satisfy dozens of gates together and the 08-13 economy
        // stint moved 121 colours behind gates a single questline can clear; at
        // one card every 0.35s plus a 6s dwell, announcing all of them honestly
        // would hold the corner of the screen for minutes. The tail says the
        // number instead of pretending it did not happen.
        std::size_t burstCap{ 6 };
    };

    class DyeCardQueue {
    public:
        void SetTiming(const DyeCardTiming& a_timing) { timing_ = a_timing; }
        [[nodiscard]] const DyeCardTiming& Timing() const { return timing_; }

        // Queue a batch of freshly earned colours.
        //
        // ⚠ IT DOES NOT SHOW ANYTHING. Nothing becomes visible until Tick runs,
        // so the caller may push from the game thread while the render thread is
        // mid-draw without a card appearing half-built.
        void Push(std::vector<DyeCard> a_cards, double a_now);

        // Promote what is due and retire what has expired. Idempotent within a
        // frame: calling it twice with the same a_now changes nothing.
        void Tick(double a_now);

        // Did the last Tick take the visible stack from empty to occupied.
        //
        // ⚠ THIS IS "A HAUL STARTED ARRIVING", WHICH IS THE ONLY MOMENT WORTH A
        // SOUND. Cards enter staggerSec apart and a haul can be burstCap of
        // them, so a cue per card is a machine gun; a cue on this edge is one
        // chime per arrival however many colours it carries.
        //
        // ⚠ IT DESCRIBES THE LAST TICK, NOT THE QUEUE. Ticking twice at the same
        // a_now still leaves the CONTENTS unchanged, which is what the
        // idempotence above claims, but the second call reports false because
        // nothing entered on it. Read it once, right after the Tick that owns
        // the frame.
        [[nodiscard]] bool WokeFromEmpty() const { return wokeFromEmpty_; }

        // On screen right now, oldest FIRST, so the caller stacks downward in
        // arrival order and a card entering never shifts the ones above it.
        [[nodiscard]] const std::vector<DyeCard>& Visible() const { return visible_; }
        [[nodiscard]] std::size_t Waiting() const { return waiting_.size(); }
        [[nodiscard]] bool Empty() const { return visible_.empty() && waiting_.empty(); }

        // 0 while arriving, 1 through the dwell, back to 0 through the fade.
        [[nodiscard]] float AlphaOf(const DyeCard& a_card, double a_now) const;

        // How far off its resting place a card still is, 1 at the instant it
        // appears and 0 once it has arrived. The window multiplies this by a
        // slide distance; keeping it unitless is what lets the test assert the
        // motion without knowing the screen.
        //
        // ⚠ ITS CLOCK IS entrySec, NOT the fade in. See that field for what
        // sharing one cost.
        [[nodiscard]] float SlideOf(const DyeCard& a_card, double a_now) const;

        // Everything goes, immediately. The load boundary, and the setting being
        // switched off mid-session.
        void Clear();

    private:
        [[nodiscard]] double LifeSec() const {
            return timing_.fadeInSec + timing_.dwellSec + timing_.fadeSec;
        }

        DyeCardTiming        timing_{};
        std::vector<DyeCard> visible_{};
        std::vector<DyeCard> waiting_{};
        double               lastEnteredAt_{ 0.0 };
        bool                 everEntered_{ false };
        bool                 wokeFromEmpty_{ false };
    };

}  // namespace OS
