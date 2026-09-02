#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace RE {
    class Actor;
}

// What the dismember sweep MEASURED about head-part occupants, published for
// the worn-mask shim to consume.
//
// ⚠⚠ ONE MEASURER, ONE CONSUMER, AND THE SPLIT IS THE WHOLE POINT. The shim
// runs inside the engine's mask read and cannot afford to walk the biped
// deciding what draws; the sweep in BipedPost already walks it and gets the
// answer right (field r27/r28: `worn 0x108E drawn 0x8C` every single sweep -
// slot 31's helmet is worn and does not draw). Before this, the shim asked its
// own cheaper question - is the occupant's `.addon` or `.part` missing - and
// the r28 round proved that question is too weak: the occupant on 31 answered
// `addon=set part=set partClone=null` and the engine hid the hair behind it on
// every head build, with the ladder re-enabling `0x2` a second later. That war
// is the bald flash.
//
// ⚠ A MISS IS SILENCE, NOT A GHOST. Nothing has been measured for an actor
// until its first sweep, and the shim keeps its strict synchronous test for
// that window. So a real helmet still hides hair from the frame it is worn:
// the measurement can only ever ADD ghosts the sweep has actually seen fail to
// draw, never invent one for a piece mid-attach.
namespace OS::MeasuredGhosts {

    // The two bits engine 24220 actually reads, and the ONLY ones the shim may
    // drop. SlotMask.h's kHeadPartMask is the authority and carries the
    // disassembly; this file cannot include it without dragging the whole slot
    // vocabulary into a header the UI reads, so BipedPost.cpp static_asserts
    // the two against each other instead.
    inline constexpr std::uint32_t kEngineHideMask = (1u << 0) | (1u << 1);

    // ⚠ THE WHOLE HEAD FAMILY IS MEASURED, THE ENGINE'S TWO BITS ARE ACTED ON.
    // A ghost on 42 costs no hair - 24220 never reads it - but it is still a
    // row in Fitting Room's Head page saying "Base gear" over an occupant that
    // draws nothing, and the 2026-08-25 field round was the user seeing TWO
    // occupied head slots and having no way to tell which was which. The
    // measurement is the same walk either way, so it is taken for all five and
    // split at publish.
    inline constexpr std::uint32_t kHeadFamilyMask = kEngineHideMask |
                                                     (1u << 11) |  // 41 long hair
                                                     (1u << 12) |  // 42 circlet
                                                     (1u << 13);   // 43 ears
    inline constexpr std::uint32_t kTrackedBits = 14;

    namespace detail {
        struct Entry {
            std::atomic<std::uint32_t> actorID{ 0 };  // 0 = empty
            std::atomic<std::uint32_t> ghosts{ 0 };   // worn & ~drawn, engine bits
            std::atomic<std::uint32_t> family{ 0 };   // ditto, whole head family
        };
        // Same shape and size as BipedHooks' RenderedCoverage table, for the
        // same reason: a crowded cell has a handful of styled actors in it and
        // a same-hash neighbour must read as a MISS rather than as this
        // actor's numbers.
        inline constexpr std::size_t     kSlots = 8;
        inline std::array<Entry, kSlots> g_table{};
    }  // namespace detail

    // ⚠⚠ AN ATTACH IN FLIGHT LOOKS EXACTLY LIKE A GHOST, AND r29 SHIPPED THAT
    // MISTAKE. A real Iron Helmet on the vanilla Nord reads not-drawn at the
    // ladder's +1s and +2.5s rungs and only gains its clone by +5s (field
    // 04:17:17-04:17:21, `drawn 0xC` then `drawn 0x8E`). Published straight
    // through, that early reading told the shim to drop the hair-hide, and
    // since the mask is only re-read on a head build the hair then clipped
    // through the helmet for as long as nothing rebuilt the head - which is
    // exactly what the field saw on opening the editor.
    //
    // So a slot has to hold still before its verdict counts. Seeing it DRAWN
    // clears the clock at once, so a late-attaching helmet never becomes a
    // ghost; seeing it not-drawn starts one, and only an unbroken stretch
    // longer than the dwell is published. The cost is stated plainly: for the
    // first few seconds after a rebuild a genuine ghost is not filtered and
    // the ladder fights the old war on its own. That is the half the field
    // complained about least, and the recurring re-enables that made the hair
    // vanish over and over (settle+4s, settle+10s) all sit past the dwell.
    inline constexpr double kGhostDwellSeconds = 6.0;

    // ⚠⚠ AND THE VERDICT BELONGS TO A RACE, NOT TO AN ACTOR - r30's fault, and
    // the dwell alone could never have survived it. Applying a look rides a
    // ChangeRace out to the vanilla race and back (ProfileApply's 'character'
    // step, twice per apply). The Iron Helmet on 31 has a real Nord addon, so
    // on that leg it genuinely DRAWS and the sweep honestly says so - and keyed
    // on the actor alone that honest reading zeroed the clock, so the custom
    // race came back owing a fresh six seconds and the player was bald for all
    // of them, every apply. The banked log walks it: published 0x2 at 06:26:33,
    // 0x1 at 06:26:43 (the ChangeRace to 00013746 one line above), 0x3 at
    // 06:27:00, 0x1 again at 06:27:05 with the next switch.
    //
    // So each (actor, race) keeps its OWN clock and its own verdict, and a
    // reading taken on one race neither clears nor advances the other's. A
    // verdict is sticky across an excursion, which is what removes the second
    // bald window: the first sweep after the race lands republishes what was
    // already measured instead of re-serving the dwell.
    //
    // ⚠ AND IT SELF-HEALS. Seeing the slot DRAW on the race it was condemned on
    // drops the verdict there and then, so a memo can never outlive the fact it
    // records (a-not-drawn-reading-during-an-attach-is-not-a-verdict). Only an
    // unbroken not-drawn stretch on THAT race can write one, which is why a
    // real helmet mid-attach still cannot earn it.
    namespace detail {
        // Publisher-side only, and the sweep is game-thread, so this needs no
        // atomics: when each head-part bit was FIRST seen not drawn on this
        // race, or 0 for "it was drawn last time we looked".
        struct Dwell {
            std::uint32_t actorID{ 0 };
            std::uint32_t raceID{ 0 };
            std::uint64_t seen{ 0 };  // eviction order; 0 is an unused entry
            double        since[kTrackedBits]{};
            std::uint32_t occupant[kTrackedBits]{};  // whose verdict this is
            std::uint32_t settled{ 0 };              // what this race has confirmed
        };
        // One per (actor, race). The player plus a handful of followers, at two
        // races each while a look applies, sits well inside this.
        inline constexpr std::size_t      kDwells = 16;
        inline std::array<Dwell, kDwells> g_dwell{};
        inline std::uint64_t              g_seen = 0;

        // ⚠ EVICTION RE-SERVES THE DWELL, it does not invent a verdict. Losing
        // an entry costs one more bald window and never hair through a helmet,
        // which is this file's usual fail-safe direction.
        inline Dwell& FindDwell(std::uint32_t a_actorID,
                                std::uint32_t a_raceID) noexcept {
            std::size_t oldest = 0;
            for (std::size_t i = 0; i < kDwells; ++i) {
                if (g_dwell[i].actorID == a_actorID &&
                    g_dwell[i].raceID == a_raceID) {
                    return g_dwell[i];
                }
                if (g_dwell[i].seen < g_dwell[oldest].seen) {
                    oldest = i;
                }
            }
            g_dwell[oldest]         = Dwell{};
            g_dwell[oldest].actorID = a_actorID;
            g_dwell[oldest].raceID  = a_raceID;
            return g_dwell[oldest];
        }
    }  // namespace detail

    // Called by the sweep with the race it measured on, which head-family bits
    // DRAW, the form ID occupying each tracked bit (0 where nothing is worn),
    // and the current steady-clock reading in seconds. Publishing zero is
    // meaningful: it says "measured, nothing is a ghost".
    //
    // ⚠⚠ "NOT WORN" IS NOT "IT DRAWS", AND THE FIRST CUT OF THIS CONFLATED
    // THEM. It took a single `notDrawn` mask, so a slot holding nothing arrived
    // looking exactly like a slot holding something that renders. Applying a
    // look re-equips gear, the helmet is off the biped for a sweep or two, and
    // the field round read it plainly:
    //
    //   07:14:38.498  ghost slot(s) 00000003   <- settled, correct
    //   07:14:42.505  worn 0x8C drawn 0x8C     <- helmet not worn for this sweep
    //   07:14:44.115  ghost slot(s) 00000001   <- verdict thrown away, bald again
    //
    // So an unworn slot now freezes: it judges nothing, clears nothing, and is
    // published as no ghost because an unworn slot has no hide to drop anyway.
    //
    // ⚠ AND THE VERDICT FOLLOWS THE PIECE, not the slot. Freezing across an
    // unequip would otherwise let a ghost helmet's verdict land on a REAL one
    // swapped into the same slot, which is hair through a helmet. A bit whose
    // occupant changes starts over.
    inline void Publish(std::uint32_t a_actorID, std::uint32_t a_raceID,
                        std::uint32_t a_drawn, const std::uint32_t* a_occupants,
                        double a_now) noexcept {
        if (a_actorID == 0) {
            return;
        }
        auto& dwell = detail::FindDwell(a_actorID, a_raceID);
        dwell.seen  = ++detail::g_seen;
        std::uint32_t worn = 0;
        for (std::uint32_t bit = 0; bit < kTrackedBits; ++bit) {
            const auto mask = 1u << bit;
            if ((kHeadFamilyMask & mask) == 0) {
                continue;  // not a head-family slot; nothing here reads it
            }
            const std::uint32_t occupant = a_occupants ? a_occupants[bit] : 0u;
            if (occupant == 0) {
                continue;  // nothing worn: freeze the clock and keep the verdict
            }
            worn |= mask;
            if (dwell.occupant[bit] != occupant) {
                // A different piece. Whatever its predecessor earned says
                // nothing about this one.
                dwell.occupant[bit] = occupant;
                dwell.since[bit]    = 0.0;
                dwell.settled &= ~mask;
            }
            if ((a_drawn & mask) != 0) {
                // It draws on this race: whatever it was, it is not a ghost
                // here, and any verdict this race held goes with the reading.
                dwell.since[bit] = 0.0;
                dwell.settled &= ~mask;
                continue;
            }
            if (dwell.since[bit] == 0.0) {
                dwell.since[bit] = a_now;
            }
            if (a_now - dwell.since[bit] >= kGhostDwellSeconds) {
                dwell.settled |= mask;
            }
        }
        // ⚠ ONLY WHAT IS ACTUALLY WORN. A frozen verdict on an empty slot is
        // kept for the piece's return and never handed to the shim, because
        // 24220 hides for a worn slot and there is nothing there to hide for.
        const auto published = dwell.settled & worn;
        auto&      slot      = detail::g_table[a_actorID % detail::kSlots];
        slot.actorID.store(0, std::memory_order_relaxed);  // invalid while writing
        slot.ghosts.store(published & kEngineHideMask, std::memory_order_relaxed);
        slot.family.store(published & kHeadFamilyMask, std::memory_order_relaxed);
        slot.actorID.store(a_actorID, std::memory_order_release);
    }

    // The measured ghost bits for this actor, or nothing measured yet.
    // ⚠ The owner is re-checked AFTER the payload load: a neighbour that
    // published in between leaves the id mismatched (writes pass through 0),
    // and that reads as a miss rather than as somebody else's mask.
    [[nodiscard]] inline bool Read(std::uint32_t a_actorID,
                                   std::uint32_t& a_out) noexcept {
        if (a_actorID == 0) {
            return false;
        }
        const auto& slot = detail::g_table[a_actorID % detail::kSlots];
        if (slot.actorID.load(std::memory_order_acquire) != a_actorID) {
            return false;
        }
        const auto ghosts = slot.ghosts.load(std::memory_order_relaxed);
        if (slot.actorID.load(std::memory_order_acquire) != a_actorID) {
            return false;
        }
        a_out = ghosts;
        return true;
    }

    // The same verdicts across the whole head family, for the Head page rather
    // than for the engine. ⚠ NOT FOR THE SHIM: dropping a bit outside
    // kEngineHideMask would be inert at best (24220 never reads 41, 42 or 43)
    // and this file's fail-safe reasoning only covers the two it does read.
    [[nodiscard]] inline bool ReadHeadFamily(std::uint32_t  a_actorID,
                                             std::uint32_t& a_out) noexcept {
        if (a_actorID == 0) {
            return false;
        }
        const auto& slot = detail::g_table[a_actorID % detail::kSlots];
        if (slot.actorID.load(std::memory_order_acquire) != a_actorID) {
            return false;
        }
        const auto family = slot.family.load(std::memory_order_relaxed);
        if (slot.actorID.load(std::memory_order_acquire) != a_actorID) {
            return false;
        }
        a_out = family;
        return true;
    }

    // A load boundary tears down every actor these ids name.
    inline void Clear() noexcept {
        for (auto& slot : detail::g_table) {
            slot.actorID.store(0, std::memory_order_relaxed);
            slot.ghosts.store(0, std::memory_order_relaxed);
            slot.family.store(0, std::memory_order_relaxed);
        }
        for (auto& dwell : detail::g_dwell) {
            dwell = detail::Dwell{};
        }
        detail::g_seen = 0;
    }

}  // namespace OS::MeasuredGhosts
