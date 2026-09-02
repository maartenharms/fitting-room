#include "DyeCardQueue.h"

#include <algorithm>

namespace OS {

    namespace {

        // What one waiting entry stands for: itself, or everything a tail has
        // already swallowed.
        [[nodiscard]] std::size_t Weight(const DyeCard& a_card) {
            return a_card.kind == DyeCardKind::kMore ? a_card.more : 1;
        }

    }  // namespace

    void DyeCardQueue::Push(std::vector<DyeCard> a_cards, double) {
        if (a_cards.empty()) {
            return;
        }
        waiting_.insert(waiting_.end(),
                        std::make_move_iterator(a_cards.begin()),
                        std::make_move_iterator(a_cards.end()));

        // ⚠ COLLAPSE THE OVERFLOW HERE, NOT AT DRAW TIME, so the count is
        // decided once by the thread that owns the queue rather than recomputed
        // every frame by the one that draws it.
        //
        // ⚠ burstCap 0 means no cap at all. It is not reachable from the INI
        // clamp, but the arithmetic below would underflow on it and a
        // notification is not worth a crash.
        if (timing_.burstCap == 0 || waiting_.size() <= timing_.burstCap) {
            return;
        }
        const std::size_t keep = timing_.burstCap - 1;
        std::size_t       more = 0;
        for (std::size_t i = keep; i < waiting_.size(); ++i) {
            more += Weight(waiting_[i]);
        }
        waiting_.resize(keep);

        // ⚠ ONE TAIL, EVER. A second burst arriving behind a tail is folded
        // into it by the loop above, because the tail is one of the entries
        // past `keep` and its weight is its whole count. Two stacked tails
        // reading "and 30 more" over "and 12 more" would make the player do
        // arithmetic to learn a number this could simply say.
        DyeCard tail;
        tail.kind = DyeCardKind::kMore;
        tail.more = more;
        waiting_.push_back(std::move(tail));
    }

    void DyeCardQueue::Tick(double a_now) {
        const double life = LifeSec();
        wokeFromEmpty_    = false;

        // Retire FIRST, so a slot freed this frame is filled this frame rather
        // than leaving a visible gap in the stack for one present.
        //
        // ⚠ `a_now < shownAt` IS NOT PARANOIA. FUCK::GetTime is not promised to
        // be monotonic across a load screen, and a card stamped in the future
        // would sit at alpha 0 forever while still holding a slot, which reads
        // as the notifications having silently died.
        std::erase_if(visible_, [&](const DyeCard& c) {
            return a_now < c.shownAt || (a_now - c.shownAt) >= life;
        });

        // Read AFTER the retire pass above, so a haul arriving into a stack that
        // just finished emptying counts as a fresh arrival rather than as a
        // continuation of the one that ended.
        const bool wasEmpty = visible_.empty();

        while (!waiting_.empty() && visible_.size() < timing_.maxVisible) {
            // The stagger, and the reason it is skipped for the very first card
            // of a session: there is nothing for it to be staggered against, and
            // waiting one interval before the first card of a batch would delay
            // every announcement by it for no gain.
            if (everEntered_ && (a_now - lastEnteredAt_) < timing_.staggerSec) {
                break;
            }
            DyeCard card = std::move(waiting_.front());
            waiting_.erase(waiting_.begin());
            card.shownAt = a_now;
            visible_.push_back(std::move(card));
            lastEnteredAt_ = a_now;
            everEntered_   = true;
            // With a zero stagger the guard above is false forever, so the loop
            // fills the screen in one call. That is the configuration the tests
            // use to inspect a whole batch, and it is a legitimate INI setting
            // for somebody who wants the stack to appear at once.
        }
        wokeFromEmpty_ = wasEmpty && !visible_.empty();
    }

    float DyeCardQueue::AlphaOf(const DyeCard& a_card, double a_now) const {
        const double age = a_now - a_card.shownAt;
        if (age < 0.0) {
            return 0.0f;
        }
        const double in = timing_.fadeInSec;
        if (in > 0.0 && age < in) {
            return static_cast<float>(age / in);
        }
        const double holdEnd = in + timing_.dwellSec;
        if (age <= holdEnd) {
            return 1.0f;
        }
        if (timing_.fadeSec <= 0.0) {
            return 0.0f;
        }
        const double out = age - holdEnd;
        if (out >= timing_.fadeSec) {
            return 0.0f;
        }
        return static_cast<float>(1.0 - out / timing_.fadeSec);
    }

    float DyeCardQueue::SlideOf(const DyeCard& a_card, double a_now) const {
        const double age = a_now - a_card.shownAt;
        if (age < 0.0) {
            return 1.0f;
        }
        const double in = timing_.entrySec;
        if (in <= 0.0 || age >= in) {
            return 0.0f;
        }
        // Eased so the card decelerates into place instead of stopping dead, and
        // so most of the distance is covered early: the card is opaque by then,
        // and travel nobody can see is not motion.
        const double t = 1.0 - age / in;
        return static_cast<float>(t * t);
    }

    void DyeCardQueue::Clear() {
        visible_.clear();
        waiting_.clear();
        // ⚠ THE STAGGER CLOCK GOES TOO. Left standing, the first card of the
        // NEXT character would be held back by a timestamp from the previous
        // one, which on a long session is an arbitrary delay nobody could
        // explain from the code.
        lastEnteredAt_ = 0.0;
        everEntered_   = false;
        // The arrival edge goes too. Left standing, the first Tick after a load
        // would report a haul that belongs to the previous session and chime for
        // cards nobody is going to see.
        wokeFromEmpty_ = false;
    }

}  // namespace OS
