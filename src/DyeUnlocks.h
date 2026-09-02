#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OS {

    // One character's dye progression: the colours they have earned, the charge
    // in their Seamstone, and the deeds we count ourselves.
    //
    // All three live together because they ride one co-save record and are
    // cleared together. Splitting them would mean three records that must not
    // disagree about which character they belong to.
    //
    // Pure, so the wire format is provable without a save.
    class DyeUnlockSet {
    public:
        // ⚠ ADD ONLY. Promotion never removes. A skill drops on a Legendary
        // reset and a quest can be failed; a colour already earned must not
        // evaporate underneath the player.
        //
        // Returns whether the id is now held. False means it was refused for
        // being empty, over kMaxStrLen, or past kMaxUnlocks. ⚠ The caller has
        // to respect that: Promote announcing a colour it failed to store
        // would re-announce it on every load forever, for a colour the player
        // can never use.
        //
        // [[nodiscard]] so that stays a compile error rather than something a
        // reviewer has to notice. It is the exact bug this return value was
        // added to prevent, and the compiler can hold the line for free.
        [[nodiscard]] bool        Add(std::string_view a_id);
        [[nodiscard]] bool        Has(std::string_view a_id) const;
        [[nodiscard]] std::size_t Size() const;

        // Every id held.
        //
        // ⚠ A COPY, not a reference, and it is the same call Encode two
        // methods down already makes rather than a new kind of one. Its caller
        // runs inside DyeUnlocks::With and then writes a FILE; handing back a
        // reference would invite doing that write with g_lock held, which is
        // exactly what SaveCallback walked the lock back out of for the
        // co-save. The set is a few hundred short strings, the same order as
        // the Encode buffer that is already copied out beside it.
        [[nodiscard]] std::set<std::string> Ids() const;

        [[nodiscard]] std::uint32_t Charge() const;
        void                        SetCharge(std::uint32_t a_charge);
        // Saturates at a_cap rather than wrapping, and NEVER decreases.
        //
        // Returns how much was actually applied, which can be less than
        // a_amount (saturated at the cap) or zero (already at or over it).
        // ⚠ The cost plan consumes a soul gem and then calls this; with no
        // return it could not tell whether the gem was accepted, and a Grand
        // gem into a full stone would be eaten for nothing with no way to say
        // so. [[nodiscard]] for the same reason Add has one.
        [[nodiscard]] std::uint32_t AddCharge(std::uint32_t a_amount,
                                              std::uint32_t a_cap);
        // What that same AddCharge WOULD apply, without applying it. The
        // remainder (a_amount minus this) is what a gem would waste.
        //
        // ⚠ AddCharge IS IMPLEMENTED IN TERMS OF THIS, rather than the two
        // being written separately and kept in step. A recharge has to state
        // the loss BEFORE it eats the soul gem, so a preview is required; and a
        // preview that disagreed with the real thing by one edit would tell the
        // player a Grand soul fits and then swallow it. One body, so they
        // cannot drift. test_dyeunlocks.cpp asserts the agreement anyway,
        // because "they share a body" is a property a future edit can quietly
        // remove.
        [[nodiscard]] std::uint32_t ChargeRoomFor(std::uint32_t a_amount,
                                                  std::uint32_t a_cap) const;
        // False and no change when there is not enough. Never underflows.
        [[nodiscard]] bool          SpendCharge(std::uint32_t a_amount);

        [[nodiscard]] std::uint32_t Deed(std::string_view a_name) const;
        void                        BumpDeed(std::string_view a_name,
                                             std::uint32_t    a_by);

        void Clear();

        // Wire: u32 unlock count, each id as u32 length + bytes; u32 charge;
        // u32 deed count, each as u32 name length + bytes + u32 value.
        [[nodiscard]] std::vector<std::byte> Encode() const;
        // ⚠ ALL OR NOTHING. On false this object is untouched, so a refused
        // record cannot leave a character holding half of someone else's.
        [[nodiscard]] bool Decode(std::span<const std::byte> a_bytes,
                                  std::uint32_t              a_version);

    private:
        std::set<std::string>                 ids_;
        std::map<std::string, std::uint32_t>  deeds_;
        std::uint32_t                         charge_{ 0 };
    };

    inline constexpr std::uint32_t kDyeUnlockVersion = 1;

    // Whether a record of version a_record may be read by a build whose
    // current version is a_current.
    //
    // ⚠ SPLIT OUT OF Decode ON PURPOSE, so the rule can be checked against
    // version pairs the current constant cannot produce. With
    // kDyeUnlockVersion at 1 there is no way to construct "a v1 record under a
    // v2 build" through Decode, and that is exactly the case a bump has to
    // survive. A runtime test of Decode therefore cannot tell a range from an
    // equality; the static_asserts below can, and they fail at COMPILE time
    // against an equality implementation.
    [[nodiscard]] constexpr bool DyeVersionLoadable(std::uint32_t a_record,
                                                    std::uint32_t a_current) {
        return a_record != 0 && a_record <= a_current;
    }

    static_assert(DyeVersionLoadable(1, 2),
                  "a v1 record must still load under a v2 build, or bumping "
                  "the version silently destroys every player's charge and "
                  "deeds");
    static_assert(DyeVersionLoadable(1, 1));
    static_assert(!DyeVersionLoadable(2, 1),
                  "a record from a newer build is refused");
    static_assert(!DyeVersionLoadable(0, 1), "0 was never written");

    // The one deed counted in the first pass. A named constant because the
    // rules files spell it and a typo there is silent.
    inline constexpr const char* kDeedChannelsDyed = "channelsDyed";

    // The live set for the current character, behind a lock.
    //
    // ⚠ NO RAW REFERENCE ESCAPES, deliberately. The obvious shape here is a
    // CurrentDyeUnlocks() returning DyeUnlockSet&, and the first draft of this
    // header had one, citing Collection as precedent. That took the singleton
    // half of the precedent and dropped the safety half: Collection carries a
    // mutable mutex and takes it in every accessor, and so do DyePalette,
    // DyeSchemes, PresetStore, AutoPresets, Favorites and HairColor. The
    // uniform contract in this codebase is a lock plus a Snapshot() copy for
    // the render thread, and this is not the module to break it in.
    //
    // The hazard is live as soon as the pane is wired: the editor-open
    // promotion pass runs Add on the main thread while the draw loop walks the
    // same std::set through Has. Rebalancing a red-black tree under an
    // iterator is the same class of bug as the FUCK WindowState map race that
    // already cost this project a CTD.
    namespace DyeUnlocks {

        // MAIN THREAD ONLY: the co-save callbacks and the promotion pass. Runs
        // a_fn against the live set with the lock held.
        //
        // ⚠ NOT REENTRANT. g_lock is a plain mutex, so reaching With or
        // Snapshot from inside a_fn deadlocks the calling thread with no crash
        // and no log line. That includes reaching them INDIRECTLY, through
        // anything that takes a snapshot of its own on the inside.
        //
        // ⚠ DyeWorld::Gather WAS that indirect route, and it is why its
        // signature takes a DyeUnlockSet instead of fetching one. It called
        // Snapshot for the deed counters, so gathering the world inside a With
        // hung the game, and the only defence was a comment asking callers not
        // to. Handing the set in makes the previously fatal call the correct
        // one: a caller already inside a With passes a_set. Anything else that
        // grows a Snapshot on the inside should be moved the same way rather
        // than documented, because "make the whole promotion pass atomic" is a
        // future edit that reads like an improvement.
        void With(const std::function<void(DyeUnlockSet&)>& a_fn);

        // Any thread. A consistent copy, the same contract
        // DyePalette::Snapshot has. Take it once per editor open and read from
        // the copy, NOT once per dye per frame.
        [[nodiscard]] DyeUnlockSet Snapshot();

        // Any thread. Just the charge, without copying the unlock set.
        //
        // ⚠ EXISTS SO A HOT PATH DOES NOT REACH FOR Snapshot. The item-card
        // hook runs every time the player highlights an inventory row, and
        // Snapshot copies a std::set of several hundred strings; the charge is
        // one u32 sitting under the same lock. Anything that needs ONLY the
        // charge belongs here.
        [[nodiscard]] std::uint32_t CurrentCharge();

        // ---- which unlocked colours the player has actually LOOKED AT ------
        //
        // Record 'DACK'. A colour is NEW when it is unlocked and not
        // acknowledged, exactly as a look is new when it is collected and not
        // acknowledged (Collection.h), and the palette draws it with the same
        // gold corner the cards use.
        //
        // ⚠⚠ ITS OWN RECORD, AND 'DYES' IS NOT WIDENED. That record carries
        // the player's CHARGE, which they bought with soul gems, and the deeds
        // they were billed for. A malformed ornament must not be able to cost
        // them either, which is the argument 'DYHI' already makes for not being
        // part of 'DYES', and a version bump on the record holding paid
        // currency is the one this project has been bitten by twice. Adding a
        // field there would have been the smaller diff and the larger risk.
        //
        // Any thread: same lock as the set above.
        [[nodiscard]] bool Acknowledged(std::string_view a_id);
        void               Acknowledge(std::string_view a_id);

        // ⚠⚠ THE BASELINE, and without it a save with a hundred earned colours
        // opens with a hundred gold corners. Owed by default, cleared only by a
        // decoded record, taken after the shared merge has run so an inherited
        // colour is not adopted as already seen. Collection::TakeAckBaselineIfOwed
        // carries the full argument; this is the same shape.
        //
        // ⚠ IT RETURNS THE COUNT RATHER THAN LOGGING IT, and that is a
        // constraint of this file rather than a style choice: DyeUnlocks.cpp
        // is compiled into two test targets with no engine and no PCH, so it
        // has no spdlog. The caller says the sentence.
        [[nodiscard]] std::size_t TakeAckBaselineIfOwed();

        [[nodiscard]] std::vector<std::byte> EncodeAcked();
        [[nodiscard]] bool DecodeAcked(std::span<const std::byte> a_bytes,
                                       std::uint32_t              a_version);
        // Per-save, so the revert callback clears it with everything else.
        void ClearAcked();

    }  // namespace DyeUnlocks

}  // namespace OS
