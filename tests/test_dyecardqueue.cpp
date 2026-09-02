// The unlock card's timing and stacking, with no engine and no clock. Time is a
// parameter here, so a burst of forty colours across six minutes of dwell runs
// in a microsecond and the awkward cases (a batch bigger than the screen, a card
// expiring while another is still arriving, a load boundary landing mid-stack)
// are all reachable without a game.
#include "DyeCardQueue.h"

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

namespace {

    DyeCard Colour(const std::string& a_id) {
        DyeCard c;
        c.kind = DyeCardKind::kColour;
        c.id   = a_id;
        c.name = a_id;
        c.r = 200;
        c.g = 100;
        c.b = 50;
        return c;
    }

    std::vector<DyeCard> Batch(std::size_t a_count) {
        std::vector<DyeCard> v;
        for (std::size_t i = 0; i < a_count; ++i) {
            v.push_back(Colour("dye:" + std::to_string(i)));
        }
        return v;
    }

    // Total colours a queue still stands for: the kColour cards plus whatever
    // the tail is counting. The burst cap must never lose one.
    std::size_t Accounted(const DyeCardQueue& a_q) {
        std::size_t n = 0;
        for (const auto& c : a_q.Visible()) {
            n += (c.kind == DyeCardKind::kMore) ? c.more : 1;
        }
        return n;
    }

}  // namespace

int main() {
    // ---- a push shows nothing until the clock moves -------------------------
    // ⚠ THE POINT IS THREADS, not tidiness. Push runs on the game thread out of
    // the promotion pass while Draw is running on the render thread; if Push
    // seated a card directly, a half-built entry would be drawable the moment
    // its first field was written.
    {
        DyeCardQueue q;
        q.Push(Batch(3), 100.0);
        CHECK(q.Visible().empty());
        CHECK(q.Waiting() == 3);
        CHECK(!q.Empty());
    }

    // ---- cards enter one at a time, in arrival order ------------------------
    {
        DyeCardQueue q;
        DyeCardTiming t;
        t.staggerSec = 0.35;
        q.SetTiming(t);

        q.Push(Batch(3), 100.0);
        q.Tick(100.0);
        CHECK(q.Visible().size() == 1);
        CHECK(q.Visible()[0].id == "dye:0");

        // Too soon for the second: the stagger has not elapsed.
        q.Tick(100.2);
        CHECK(q.Visible().size() == 1);

        q.Tick(100.4);
        CHECK(q.Visible().size() == 2);
        // Oldest FIRST, so a new card lands underneath and never shifts the
        // ones already being read.
        CHECK(q.Visible()[0].id == "dye:0");
        CHECK(q.Visible()[1].id == "dye:1");

        q.Tick(100.8);
        CHECK(q.Visible().size() == 3);
        CHECK(q.Waiting() == 0);
    }

    // ---- Tick is idempotent within a frame ----------------------------------
    // Draw can run more than once per present on this project, and a second call
    // at the same timestamp must not admit a second card.
    {
        DyeCardQueue q;
        q.Push(Batch(4), 50.0);
        q.Tick(50.0);
        q.Tick(50.0);
        q.Tick(50.0);
        CHECK(q.Visible().size() == 1);
    }

    // ---- the screen cap holds, and the queue drains as cards retire ---------
    {
        DyeCardQueue q;
        DyeCardTiming t;
        t.maxVisible = 2;
        t.dwellSec   = 1.0;
        t.fadeSec    = 0.2;
        t.staggerSec = 0.1;
        t.burstCap   = 99;  // out of the way; the screen cap is what is under test
        q.SetTiming(t);

        q.Push(Batch(4), 0.0);
        q.Tick(0.0);
        q.Tick(0.1);
        CHECK(q.Visible().size() == 2);

        // Full. The third waits however long it has to.
        q.Tick(0.5);
        CHECK(q.Visible().size() == 2);
        CHECK(q.Waiting() == 2);

        // fade-in 0.10 + dwell 1.0 + fade 0.2 = 1.30 of life. At 1.35 the first
        // is past it and the second (seated at 0.1) is not, which is the frame
        // where exactly one slot frees.
        q.Tick(1.35);
        CHECK(q.Visible().size() == 2);  // first retired, third took its slot
        CHECK(q.Visible()[0].id == "dye:1");
        CHECK(q.Visible()[1].id == "dye:2");
        CHECK(q.Waiting() == 1);
    }

    // ---- a burst past the cap collapses into a tail, and loses nothing ------
    // ⚠⚠ THE ACCOUNTING IS THE TEST. A cap that silently dropped the remainder
    // would look identical on screen and would be a lie about an economy the
    // player is grinding.
    {
        DyeCardQueue q;
        DyeCardTiming t;
        t.burstCap   = 6;
        t.maxVisible = 99;   // out of the way
        t.staggerSec = 0.0;  // let them all in at once
        t.dwellSec   = 1000.0;
        q.SetTiming(t);

        q.Push(Batch(40), 0.0);
        // Five colours plus one tail standing for the other 35.
        CHECK(q.Waiting() == 6);

        for (int i = 0; i < 10; ++i) {
            q.Tick(static_cast<double>(i));
        }
        CHECK(q.Visible().size() == 6);
        CHECK(q.Visible()[5].kind == DyeCardKind::kMore);
        CHECK(q.Visible()[5].more == 35);
        CHECK(Accounted(q) == 40);
    }

    // ---- a second burst adds to the existing tail rather than making two ----
    // Two tails stacked as "and 30 more" / "and 12 more" is arithmetic homework,
    // not a notification.
    {
        DyeCardQueue q;
        DyeCardTiming t;
        t.burstCap   = 4;
        t.staggerSec = 0.0;
        q.SetTiming(t);

        q.Push(Batch(10), 0.0);   // 3 colours + tail(7)
        CHECK(q.Waiting() == 4);
        q.Push(Batch(5), 0.0);    // all 5 land behind a tail that already exists
        CHECK(q.Waiting() == 4);

        std::size_t tails = 0, counted = 0;
        q.Tick(0.0);
        // Drain everything into view to inspect it.
        for (int i = 0; i < 20; ++i) { q.Tick(0.0); }
        for (const auto& c : q.Visible()) {
            if (c.kind == DyeCardKind::kMore) { ++tails; counted += c.more; }
        }
        CHECK(tails == 1);
        CHECK(counted == 12);
        CHECK(Accounted(q) == 15);
    }

    // ---- a batch exactly at the cap gets no tail ----------------------------
    // An off-by-one here reads as "and 0 more", which is worse than nothing.
    {
        DyeCardQueue q;
        DyeCardTiming t;
        t.burstCap   = 6;
        t.maxVisible = 99;  // the screen cap is not what is under test here
        t.staggerSec = 0.0;
        q.SetTiming(t);

        q.Push(Batch(6), 0.0);
        CHECK(q.Waiting() == 6);
        q.Tick(0.0);
        for (int i = 0; i < 10; ++i) { q.Tick(0.0); }
        CHECK(q.Visible().size() == 6);
        std::size_t tails = 0;
        for (const auto& c : q.Visible()) {
            if (c.kind == DyeCardKind::kMore) { ++tails; }
        }
        CHECK(tails == 0);
        CHECK(Accounted(q) == 6);
    }

    // ---- alpha: in, hold, out ----------------------------------------------
    {
        DyeCardQueue q;
        DyeCardTiming t;
        t.dwellSec   = 4.0;
        t.fadeSec    = 1.0;
        t.fadeInSec  = 0.4;  // stretched, so the ramp has frames to assert on
        t.staggerSec = 0.0;
        q.SetTiming(t);

        q.Push(Batch(1), 10.0);
        q.Tick(10.0);
        const DyeCard& c = q.Visible()[0];

        CHECK(q.AlphaOf(c, 10.0) < 0.01f);          // the instant it appears
        CHECK(q.AlphaOf(c, 10.2) > 0.4f);           // halfway in
        CHECK(q.AlphaOf(c, 10.2) < 0.6f);
        CHECK(q.AlphaOf(c, 10.4) > 0.99f);          // arrived
        CHECK(q.AlphaOf(c, 12.0) > 0.99f);          // holding
        CHECK(q.AlphaOf(c, 14.4) > 0.99f);          // last instant of the dwell
        CHECK(q.AlphaOf(c, 14.9) < 0.6f);           // fading
        CHECK(q.AlphaOf(c, 15.4) < 0.01f);          // gone
        // ⚠ Never negative and never above one: it is multiplied into a colour.
        CHECK(q.AlphaOf(c, 99.0) >= 0.0f);
        CHECK(q.AlphaOf(c, 99.0) <= 1.0f);
    }

    // ---- slide: fully offset on arrival, home when the entry is done --------
    {
        DyeCardQueue q;
        DyeCardTiming t;
        t.entrySec   = 0.4;
        t.staggerSec = 0.0;
        q.SetTiming(t);

        q.Push(Batch(1), 0.0);
        q.Tick(0.0);
        const DyeCard& c = q.Visible()[0];

        CHECK(q.SlideOf(c, 0.0) > 0.99f);
        CHECK(q.SlideOf(c, 0.2) < 0.99f);
        CHECK(q.SlideOf(c, 0.2) > 0.0f);
        CHECK(q.SlideOf(c, 0.4) < 0.01f);
        CHECK(q.SlideOf(c, 9.0) < 0.01f);  // stays home, never drifts back out
    }

    // ---- the entry is VISIBLE, which is the whole of the last fix -----------
    // ⚠⚠ THE TWO CLOCKS MUST NOT MEET AGAIN. While the slide ran on the fade-in
    // window, a card was at half opacity with a quarter of a width left to
    // travel, so it faded in roughly where it stops rather than sliding out of
    // the screen edge. This is that regression, stated as the property the fix
    // has to keep: solid long before it stops moving.
    {
        DyeCardQueue q;
        DyeCardTiming t;
        t.fadeInSec  = 0.10;
        t.entrySec   = 0.40;
        t.staggerSec = 0.0;
        q.SetTiming(t);

        q.Push(Batch(1), 0.0);
        q.Tick(0.0);
        const DyeCard& c = q.Visible()[0];

        CHECK(q.AlphaOf(c, 0.10) > 0.99f);  // fully opaque a tenth of a second in
        CHECK(q.SlideOf(c, 0.10) > 0.5f);   // ...with over half the travel left
        CHECK(q.SlideOf(c, 0.30) > 0.02f);  // still moving three quarters through
        CHECK(q.SlideOf(c, 0.40) < 0.01f);  // and only then home
    }

    // ---- Clear drops everything, visible and waiting ------------------------
    // ⚠ THE LOAD BOUNDARY. DyeUnlocks::Forget runs at kPreLoadGame and at the
    // top of every Request, and a notifier holding cards across that boundary
    // would announce the OUTGOING character's colours over the incoming one's
    // first frames.
    {
        DyeCardQueue q;
        q.Push(Batch(10), 0.0);
        q.Tick(0.0);
        CHECK(!q.Empty());
        q.Clear();
        CHECK(q.Empty());
        CHECK(q.Visible().empty());
        CHECK(q.Waiting() == 0);

        // And it can be used again straight after, with the stagger clock reset
        // rather than remembering a card from the previous character.
        q.Push(Batch(1), 0.0);
        q.Tick(0.0);
        CHECK(q.Visible().size() == 1);
    }

    // ---- an empty push is a no-op ------------------------------------------
    // The promotion pass runs on a timer and earns nothing the overwhelming
    // majority of the time.
    {
        DyeCardQueue q;
        q.Push({}, 0.0);
        q.Tick(0.0);
        CHECK(q.Empty());
    }

    // ---- time going backwards does not strand a card -----------------------
    // ⚠ FUCK::GetTime is not guaranteed monotonic across a load screen, and a
    // card whose shownAt is in the future would sit at alpha 0 forever while
    // still holding a slot. It expires instead.
    {
        DyeCardQueue q;
        DyeCardTiming t;
        t.dwellSec   = 2.0;
        t.staggerSec = 0.0;
        q.SetTiming(t);

        q.Push(Batch(1), 500.0);
        q.Tick(500.0);
        CHECK(q.Visible().size() == 1);
        q.Tick(10.0);  // clock jumped backwards
        CHECK(q.Visible().empty());
    }

    // ---- the arrival edge, which is what the sound hangs off ---------------
    // ⚠ ONCE PER HAUL, NOT ONCE PER CARD. Cards enter staggerSec apart, so a
    // cue keyed on "a card entered" fires burstCap times in a row and reads as
    // a machine gun. This asserts the shape the sound depends on: true on the
    // tick that fills an empty stack, false on every tick that only adds to it.
    {
        DyeCardQueue  q;
        DyeCardTiming t;
        t.dwellSec   = 10.0;
        t.staggerSec = 0.5;
        t.maxVisible = 4;
        q.SetTiming(t);

        CHECK(!q.WokeFromEmpty());  // nothing has happened yet

        q.Push(Batch(3), 100.0);
        q.Tick(100.0);
        CHECK(q.Visible().size() == 1);
        CHECK(q.WokeFromEmpty());  // the haul started arriving

        q.Tick(100.6);  // the second card of the SAME haul enters
        CHECK(q.Visible().size() == 2);
        CHECK(!q.WokeFromEmpty());  // ...and must not chime again

        q.Tick(101.2);
        CHECK(q.Visible().size() == 3);
        CHECK(!q.WokeFromEmpty());
    }
    {  // a stack that empties and is then given more DOES chime again, because
       // that is a second arrival rather than a continuation of the first
        DyeCardQueue  q;
        DyeCardTiming t;
        t.dwellSec   = 1.0;
        t.fadeSec    = 0.0;
        t.staggerSec = 0.0;
        q.SetTiming(t);

        q.Push(Batch(1), 200.0);
        q.Tick(200.0);
        CHECK(q.WokeFromEmpty());

        q.Tick(210.0);  // long past the dwell, so it retires
        CHECK(q.Visible().empty());
        CHECK(!q.WokeFromEmpty());

        q.Push(Batch(1), 211.0);
        q.Tick(211.0);
        CHECK(q.Visible().size() == 1);
        CHECK(q.WokeFromEmpty());
    }
    {  // Clear takes the edge with it, so the first Tick of the next session
       // does not chime for a haul belonging to the previous one
        DyeCardQueue q;
        DyeCardTiming t;
        t.staggerSec = 0.0;
        q.SetTiming(t);
        q.Push(Batch(1), 300.0);
        q.Tick(300.0);
        CHECK(q.WokeFromEmpty());
        q.Clear();
        CHECK(!q.WokeFromEmpty());
    }

    if (g_failures == 0) {
        std::printf("test_dyecardqueue: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
