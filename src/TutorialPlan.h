#pragma once

// Which onboarding tutorial should run, and whether it has run already.
//
// Pure, and deliberately outside EditorUI.cpp for the same reason
// EditorGate::PlanPaneTransition is: no test compiles EditorUI.cpp, so any
// decision left in there reaches the field unexercised. This header names no
// engine types and compiles into the test executable.

#include <cstddef>
#include <cstdint>

namespace OS::TutorialPlan {

    // ⚠ ONE TUTORIAL PER PAGE, AND THAT IS NOT A PRESENTATION CHOICE. Three of
    // the six pages can be absent: Presets has no rail tile at all with nothing
    // installed, Body Studio is compiled out of a release build, and Shape can
    // be a page with no controls on it when RaceMenu is not loaded. A single
    // "the tutorial has been seen" flag would therefore key on the ENVIRONMENT:
    // someone who finished without RaceMenu would be marked done for good and
    // would never be shown the Shape page after installing it.
    //
    // That is not a hypothetical. NpcHair.cpp carries the same scar, where a
    // hairstyle declined for a missing FSMP was persisted as unsupported, so
    // one browse on a machine without it permanently greyed styles that
    // installing FSMP could not bring back.
    //
    // Per-tutorial flags also buy the better behaviour for nothing: a page that
    // appears later is taught the first time it is opened.
    enum class Id : std::uint8_t {
        kWelcome = 0,  // the rail, once, before any page
        kOutfits,
        kDye,
        kPresets,
        kRules,
        kShape,
        kBodyStudio,
        // ⚠ SAFE TO ADD HERE DESPITE THE MASK, and the reason is worth one
        // line: the mask below is BUILT from the named INI flags every time it
        // is read and never stored, so an id's bit position is a session-local
        // detail. bOverlays is simply a new key, absent from every existing
        // INI, which is exactly the "a page that appears later is taught the
        // first time it is opened" behaviour the per-tutorial flags bought.
        kOverlays,
        // The Looks page (kProfiles in PaneMode; "Looks" everywhere the player
        // can read). Added last of the page tutorials for the same reason
        // bOverlays was: a new INI key is absent from every existing file, so
        // it comes up unseen and the page is taught the first time it opens.
        kLooks,
        // ⚠ LAST, AND GATED ON THE REST. The send-off only makes sense once
        // there is nothing left to teach, so unlike every other id this one is
        // not owned by a page. ReadyForClosing below is what holds it back.
        kClosing,
        kCount
    };

    // ⚠ SIX CARDS, HARD (user 2026-08-08, raised from four by the same user on
    // 2026-08-16). The original rule was four, with the reasoning that past
    // about six people skip and a tutorial that is skipped teaches nothing at
    // all. That ceiling is now the cap itself, asked for by name: "we need a
    // bit more cards for the rules page and it has to be more hands on".
    //
    // ⚠ RAISING THE CAP IS NOT PERMISSION TO GROW THE OTHERS. Every table is
    // still asserted against it, and every table except the Rules one is still
    // four or fewer. The Rules page earned the extra pair by being the page
    // playtesters actually failed at; nothing else has that evidence.
    inline constexpr std::size_t kMaxSteps = 6;

    [[nodiscard]] constexpr std::uint32_t Bit(Id a_id) {
        return 1u << static_cast<std::uint32_t>(a_id);
    }

    [[nodiscard]] constexpr bool Seen(std::uint32_t a_seen, Id a_id) {
        return (a_seen & Bit(a_id)) != 0u;
    }

    [[nodiscard]] constexpr std::uint32_t MarkSeen(std::uint32_t a_seen, Id a_id) {
        return a_seen | Bit(a_id);
    }

    // Whether a_id should start right now.
    //
    // ⚠ AN EMPTY STEP TABLE NEVER FIRES, which is what makes it safe to declare
    // every page's id before every page's copy is written. A tutorial with no
    // cards would otherwise open an empty box and mark itself done, burning the
    // one chance it had.
    //
    // ⚠ AND IT REFUSES WHILE ANOTHER POPUP IS UP. The editor's leave-unsaved and
    // delete-outfit boxes are modal, and a tutorial card opening on top of a
    // question the player is being asked is two modals deep in a UI that has
    // never had to be.
    // ⚠ AND IT ASKS WHETHER THE PLAYER WANTS ANY OF THIS AT ALL. The first card
    // offers a way out, and the answer to that is a setting rather than seven
    // flags flipped at once: a tutorial added in a later version has no key in
    // anyone's INI, so it would default to unseen and fire at someone who had
    // already said no. Asked here, in the one function that decides, so no
    // caller can forget it and no new page can slip past.
    [[nodiscard]] constexpr bool ShouldFire(std::uint32_t a_seen, Id a_id,
                                            std::size_t a_stepCount, bool a_pageReady,
                                            bool a_otherPopupOpen, bool a_enabled) {
        return a_enabled && a_stepCount != 0 && a_pageReady && !a_otherPopupOpen &&
               !Seen(a_seen, a_id);
    }

    // Whether a finished run has earned the flag.
    //
    // ⚠⚠ A RUN CUT SHORT BY A MISSING REQUIREMENT IS NOT A RUN THAT TAUGHT THE
    // PAGE, and this is the header's own opening lesson one level down. Per-page
    // flags exist because a single flag would key on the ENVIRONMENT; WITHIN one
    // page exactly the same thing happens per CARD. A first ever visit to Looks
    // has an empty library, so two of its four cards are dropped, and marking it
    // seen there means the player never learns the per part ticks or that
    // clicking a look shows what is in it.
    //
    // ⚠⚠ a_total IS THE RETRYABLE TOTAL AND NOT THE TABLE'S SIZE, which is the
    // whole of why this takes two counts instead of an Id. Retrying only pays
    // when the requirement is one the player is about to satisfy: a saved look is
    // made ON THAT PAGE within a minute, so the retry converges on the next
    // visit. A FOLLOWER is a fact about the world that may never become true, and
    // the card that needs one lives on Outfits, which is the page the editor
    // opens on. Counting that step here would show a solo player the same three
    // cards every single time they opened the editor, forever. The caller decides
    // which requirements are worth waiting for and hands the count down.
    //
    // ⚠ SKIP STILL ENDS IT FOR GOOD, whatever the plan looked like. "I do not
    // want this" is an answer about the tutorial and not about the library.
    [[nodiscard]] constexpr bool ShouldMarkSeen(std::size_t a_shown, std::size_t a_total,
                                                bool a_skipped) {
        return a_skipped || a_shown >= a_total;
    }

    // ⚠ SKIP COUNTS AS DONE (user 2026-08-08). Both endings write the same flag,
    // which is why this is one function rather than a Finish and a Dismiss that
    // could come to differ. A tutorial that returns after being dismissed is
    // worse than one that never ran.
    [[nodiscard]] constexpr std::uint32_t Finish(std::uint32_t a_seen, Id a_id) {
        return MarkSeen(a_seen, a_id);
    }

    // Nothing has been seen. What the replay button writes.
    inline constexpr std::uint32_t kNothingSeen = 0u;

    // Whether every tutorial except the send-off has been finished.
    //
    // ⚠ COUNTED, NOT COMPARED AGAINST A CONSTANT. A mask literal would be a
    // second place that has to know how many tutorials exist, and it would go
    // quietly wrong the moment an eighth is added: the send-off would either
    // never fire or fire early, and neither shows up until somebody plays all
    // the way through. Walking the enum cannot drift from the enum.
    //
    // ⚠ AND A PAGE THAT IS NOT INSTALLED NEVER ARRIVES HERE, which is the known
    // cost. Someone without RaceMenu has no Shape page to be taught, so its
    // flag stays false and the send-off waits. That is the right way round: the
    // alternative is telling a player they have seen everything when a page is
    // sitting there untaught, and installing RaceMenu later still earns them
    // both the Shape tutorial and the send-off after it.
    [[nodiscard]] constexpr bool ReadyForClosing(std::uint32_t a_seen) {
        for (std::uint8_t raw = 0; raw < static_cast<std::uint8_t>(Id::kClosing); ++raw) {
            if (!Seen(a_seen, static_cast<Id>(raw))) {
                return false;
            }
        }
        return true;
    }

    static_assert(static_cast<std::uint32_t>(Id::kCount) <= 32,
                  "the seen set is a uint32 mask, so there is room for 32 tutorials");
    static_assert(!Seen(kNothingSeen, Id::kOutfits), "a fresh install has seen nothing");
    static_assert(Seen(Finish(kNothingSeen, Id::kOutfits), Id::kOutfits),
                  "finishing marks it seen");
    static_assert(!Seen(Finish(kNothingSeen, Id::kOutfits), Id::kDye),
                  "finishing one must not mark another, or a page that appears "
                  "later is silently counted as taught");
    static_assert(ShouldFire(kNothingSeen, Id::kOutfits, 3, true, false, true),
                  "an unseen tutorial with cards on a ready page fires");
    static_assert(!ShouldFire(kNothingSeen, Id::kOutfits, 0, true, false, true),
                  "an empty step table never fires");
    static_assert(!ShouldFire(kNothingSeen, Id::kOutfits, 3, false, false, true),
                  "a page with nothing on it teaches nothing");
    static_assert(!ShouldFire(kNothingSeen, Id::kOutfits, 3, true, true, true),
                  "never on top of another modal");
    static_assert(!ShouldFire(Finish(kNothingSeen, Id::kOutfits), Id::kOutfits, 3, true, false,
                              true),
                  "once is once");
    static_assert(!ShouldFire(kNothingSeen, Id::kWelcome, 2, true, false, false),
                  "a player who said no is not asked again, not even by the "
                  "welcome that asked them");
    static_assert(ShouldMarkSeen(4, 4, false),
                  "a run that showed every card it had is taught");
    static_assert(!ShouldMarkSeen(2, 4, false),
                  "a run shortened by a missing requirement is not");
    static_assert(ShouldMarkSeen(2, 4, true),
                  "but skip ends it however short the plan was");
    static_assert(ShouldMarkSeen(3, 3, false),
                  "and a step dropped for a requirement nobody waits on is not "
                  "counted in the total, so Outfits without a follower is done");
    static_assert(!ReadyForClosing(kNothingSeen),
                  "a fresh install has everything still to learn");
    static_assert(!ReadyForClosing(Finish(kNothingSeen, Id::kOutfits)),
                  "one page taught is not all of them");
    static_assert(ReadyForClosing(0x1FFu),
                  "the nine page tutorials done is what the send-off waits for");
    static_assert(!ReadyForClosing(0xFFu),
                  "eight of nine is not all of them; Looks counts");
    static_assert(!ReadyForClosing(0x17Fu),
                  "and Overlays still counts; neither new page may be dropped "
                  "by widening the mask for the other");
    static_assert(!Seen(0x1FFu, Id::kClosing),
                  "and the send-off itself is not among them");

}  // namespace OS::TutorialPlan
