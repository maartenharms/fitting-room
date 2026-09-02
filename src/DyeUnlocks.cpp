#include "DyeUnlocks.h"

// For kMaxRecordBytes only, to bind the caps below to the record size limit at
// compile time. Persistence.h is include-free and declaration-only, so this
// module stays as pure as its header claims.
#include "Persistence.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

namespace OS {

    namespace {
        // Bounds on a corrupt count, so a garbage u32 becomes an early refusal
        // rather than a long loop of small allocations. Nothing here reserves,
        // so a bogus count also runs out of buffer on the first read; these
        // make that cheap and explicit instead of incidental.
        //
        // ⚠ kMaxStrLen is enforced on the WRITE side too, in Add and BumpDeed.
        // Encode writes the true length of what it holds, so a cap checked only
        // here would let the object emit a record it cannot read back.
        //
        // ⚠ ALL THREE ARE PART OF THE WIRE FORMAT. RAISING ANY OF THEM IS A
        // FORMAT CHANGE AND REQUIRES A kDyeUnlockVersion BUMP. They read like
        // private implementation details and they are not: Decode refuses any
        // record whose counts or string lengths exceed them, so these numbers
        // decide which byte streams are legal. Raise kMaxDeeds to 512 and ship
        // it, and a player who rolls back to a build carrying the old value
        // hands their save to a Decode that refuses the WHOLE record. Not a
        // partial load: the set stays as RevertCallback left it, which is
        // cleared, and SaveCallback writes that emptiness straight back over
        // the good record on their next autosave.
        //
        // What that costs is not symmetrical. Most colours come back on the
        // next promotion pass. The Seamstone charge and the deed counters have
        // NO second source anywhere, so gems already spent are simply gone.
        //
        // Bumping the version does not make the rollback lossless: a v2 record
        // is refused by a v1 build either way. It makes the loss the ACCOUNTED
        // one. Version rollback being destructive is written down in the spec's
        // co-save section, and a maintainer who has to bump the version is
        // looking straight at it. A cap raised on its own is a door nobody
        // wrote down.
        //
        // Lowering a cap is not a format change, only a narrowing of what is
        // legal to write, but it strands records already in the wild. Free
        // exactly while nothing has shipped, which is why these moved now.
        constexpr std::uint32_t kMaxUnlocks = 2048;   // 306 shipped today
        constexpr std::uint32_t kMaxDeeds   = 64;     // one today
        constexpr std::uint32_t kMaxStrLen  = 256;    // ids run about 28

        // ⚠ THE CAPS AND THE RECORD SIZE LIMIT ARE ONE DECISION, so the
        // compiler holds them together rather than a reader. The expression is
        // Encode's layout at its maximum, term for term: u32 unlock count, each
        // id as u32 length + bytes; u32 charge; u32 deed count, each as u32
        // name length + bytes + u32 value. Not a loose over-estimate: the worst
        // case the caps accept encodes to exactly this many bytes.
        //
        // At the original 8192 / 256 / 256 this evaluated to 2,197,516 against
        // a 1,048,576 byte record cap. WriteRecord would have refused the
        // record, and because SKSE rebuilds the co-save the record would then
        // be absent from the save rather than left at its previous value.
        // Unreachable with real ids at 28 characters, which is why nothing ever
        // hit it, and no reason at all to leave the door standing.
        //
        // ⚠ IF THIS EVER FAILS, LOWER A CAP. Do NOT raise kMaxRecordBytes:
        // that value guards every co-save record, so widening it to fit one
        // record's theoretical worst case weakens bounds that have nothing to
        // do with dyes.
        //
        // 64-bit arithmetic deliberately. At u32 a future cap raise could wrap
        // the product and make this assert pass by overflowing, which is the
        // one failure mode a size check must not have.
        static_assert(4ull + std::uint64_t{ kMaxUnlocks } * (4ull + kMaxStrLen) + 4ull +
                              4ull + std::uint64_t{ kMaxDeeds } * (8ull + kMaxStrLen) <=
                          Persistence::kMaxRecordBytes,
                      "DyeUnlockSet's caps let it encode more bytes than one co-save "
                      "record may carry, so WriteRecord would refuse the record and SKSE "
                      "would rebuild the co-save without it: the charge and the deeds "
                      "have no second source and would be gone. Lower a cap here. Do NOT "
                      "raise kMaxRecordBytes, which guards every other record too.");

        void AppendU32(std::vector<std::byte>& a_out, std::uint32_t a_v) {
            for (int i = 0; i < 4; ++i) {
                a_out.push_back(static_cast<std::byte>((a_v >> (i * 8)) & 0xFF));
            }
        }

        void AppendString(std::vector<std::byte>& a_out, const std::string& a_s) {
            AppendU32(a_out, static_cast<std::uint32_t>(a_s.size()));
            for (const char c : a_s) {
                a_out.push_back(static_cast<std::byte>(c));
            }
        }

        bool ReadU32(std::span<const std::byte>& a_in, std::uint32_t& a_out) {
            if (a_in.size() < 4) {
                return false;
            }
            a_out = 0;
            for (int i = 0; i < 4; ++i) {
                a_out |= static_cast<std::uint32_t>(a_in[i]) << (i * 8);
            }
            a_in = a_in.subspan(4);
            return true;
        }

        bool ReadString(std::span<const std::byte>& a_in, std::string& a_out) {
            std::uint32_t len = 0;
            // ⚠ Zero length is refused, not just over-length. Add and BumpDeed
            // both reject an empty name, so accepting one here would let Decode
            // build a state the public API forbids: it would be a second
            // constructor that skips the invariants.
            if (!ReadU32(a_in, len) || len == 0 || len > kMaxStrLen ||
                a_in.size() < len) {
                return false;
            }
            a_out.assign(reinterpret_cast<const char*>(a_in.data()), len);
            a_in = a_in.subspan(len);
            return true;
        }
    }  // namespace

    bool DyeUnlockSet::Add(std::string_view a_id) {
        // ⚠ The length cap is enforced HERE, not only in Decode. Encode writes
        // the true length of whatever it holds, so an id longer than
        // kMaxStrLen would encode fine and then fail to decode, and because
        // Decode is all or nothing that kills the WHOLE record: every other
        // colour, the charge and the deeds with it. DyePalette::DyeFromJson
        // puts no length bound on an id, so a generated third party pack can
        // reach here with one.
        //
        // Refusing costs one colour that never unlocks. Accepting costs the
        // save.
        if (a_id.empty() || a_id.size() > kMaxStrLen) {
            return false;
        }
        // The COUNT cap is enforced here too, for the same reason as the
        // length: Encode writes the true count, so a set past kMaxUnlocks
        // would emit a record Decode refuses whole. Already-held ids are not
        // affected, only new ones.
        if (!ids_.contains(std::string(a_id)) && ids_.size() >= kMaxUnlocks) {
            return false;
        }
        ids_.emplace(a_id);
        return true;
    }

    bool DyeUnlockSet::Has(std::string_view a_id) const {
        return ids_.find(std::string(a_id)) != ids_.end();
    }

    std::size_t DyeUnlockSet::Size() const { return ids_.size(); }

    std::set<std::string> DyeUnlockSet::Ids() const { return ids_; }

    std::uint32_t DyeUnlockSet::Charge() const { return charge_; }

    void DyeUnlockSet::SetCharge(std::uint32_t a_charge) { charge_ = a_charge; }

    std::uint32_t DyeUnlockSet::ChargeRoomFor(std::uint32_t a_amount,
                                              std::uint32_t a_cap) const {
        // ⚠ NEVER DECREASES. min(charge_ + amount, cap) looks right and is not:
        // with charge_ already above the cap it returns the cap, so feeding a
        // petty gem to a stone holding 4000 against a cap of 1000 destroys 3000
        // charge. That state is ordinary, not exotic: iSeamstoneCapacity is a
        // user editable INI key and lowering it is a normal way to make the
        // economy harsher.
        //
        // Over the cap, the stone keeps what it has and the gem is refused;
        // spending drains it back under on its own.
        if (charge_ >= a_cap) {
            return 0;
        }
        // Saturating, and the clamp is on the SUM computed in 64 bits so a
        // charge already near the top of a u32 cannot wrap before it is capped.
        const auto sum = static_cast<std::uint64_t>(charge_) + a_amount;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(sum, a_cap)) - charge_;
    }

    std::uint32_t DyeUnlockSet::AddCharge(std::uint32_t a_amount,
                                          std::uint32_t a_cap) {
        // The whole rule lives in ChargeRoomFor so the preview a recharge shows
        // and the write it then performs cannot disagree.
        const auto applied = ChargeRoomFor(a_amount, a_cap);
        charge_ += applied;
        return applied;
    }

    bool DyeUnlockSet::SpendCharge(std::uint32_t a_amount) {
        if (charge_ < a_amount) {
            return false;
        }
        charge_ -= a_amount;
        return true;
    }

    std::uint32_t DyeUnlockSet::Deed(std::string_view a_name) const {
        const auto it = deeds_.find(std::string(a_name));
        return it == deeds_.end() ? 0u : it->second;
    }

    void DyeUnlockSet::BumpDeed(std::string_view a_name, std::uint32_t a_by) {
        // Same caps as Add, and for the same reason: a name or a count this
        // cannot encode and read back would take the whole record down. A
        // deed already present still bumps; only a NEW key is refused at the
        // count cap.
        if (a_name.empty() || a_name.size() > kMaxStrLen) {
            return;
        }
        if (!deeds_.contains(std::string(a_name)) && deeds_.size() >= kMaxDeeds) {
            return;
        }
        auto&      slot = deeds_[std::string(a_name)];
        const auto sum  = static_cast<std::uint64_t>(slot) + a_by;
        slot            = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(sum, std::numeric_limits<std::uint32_t>::max()));
    }

    void DyeUnlockSet::Clear() {
        ids_.clear();
        deeds_.clear();
        charge_ = 0;
    }

    std::vector<std::byte> DyeUnlockSet::Encode() const {
        std::vector<std::byte> out;
        AppendU32(out, static_cast<std::uint32_t>(ids_.size()));
        for (const auto& id : ids_) {
            AppendString(out, id);
        }
        AppendU32(out, charge_);
        AppendU32(out, static_cast<std::uint32_t>(deeds_.size()));
        for (const auto& [name, value] : deeds_) {
            AppendString(out, name);
            AppendU32(out, value);
        }
        return out;
    }

    bool DyeUnlockSet::Decode(std::span<const std::byte> a_bytes,
                              std::uint32_t              a_version) {
        // ⚠ A RANGE, not equality. Equality reads as harmless and is not: the
        // day kDyeUnlockVersion becomes 2, every existing player's record is
        // refused, and because Persistence writes the record unconditionally
        // the emptied set is saved back over the good one on their next save.
        //
        // What that destroys is worse than it looks, because it half hides.
        // Promotion re-adds whatever is currently satisfied, so most of the
        // palette reappears and nobody reports it. What does not come back is
        // the Seamstone charge (gems already spent), the deed counters, and
        // every id whose pack is uninstalled, which is the exact case the
        // keep-unresolvable-ids rule below exists to protect.
        //
        // 0 was never written. Above ours is a downgrade: a record from a newer
        // build we cannot understand, which is the one case refusing is right.
        // The format only ever appends, so a v2 field reads under
        // `if (a_version >= 2)` and v1 records keep loading.
        if (!DyeVersionLoadable(a_version, kDyeUnlockVersion)) {
            return false;
        }
        std::uint32_t count = 0;
        if (!ReadU32(a_bytes, count) || count > kMaxUnlocks) {
            return false;
        }
        std::set<std::string> loadedIds;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::string id;
            if (!ReadString(a_bytes, id)) {
                return false;
            }
            // Kept whether or not any installed dye claims this id.
            loadedIds.insert(std::move(id));
        }

        std::uint32_t loadedCharge = 0;
        if (!ReadU32(a_bytes, loadedCharge)) {
            return false;
        }

        std::uint32_t deedCount = 0;
        if (!ReadU32(a_bytes, deedCount) || deedCount > kMaxDeeds) {
            return false;
        }
        std::map<std::string, std::uint32_t> loadedDeeds;
        for (std::uint32_t i = 0; i < deedCount; ++i) {
            std::string   name;
            std::uint32_t value = 0;
            if (!ReadString(a_bytes, name) || !ReadU32(a_bytes, value)) {
                return false;
            }
            // A name twice is a corrupt record, not a merge. emplace would keep
            // the first silently and leave deedCount disagreeing with the map.
            if (!loadedDeeds.emplace(std::move(name), value).second) {
                return false;
            }
        }

        // ⚠ Nothing may be left over. This is the only place the module would
        // otherwise break its own refuse-everything-malformed rule, and it is
        // the net that catches a botched version bump: someone appends a v2
        // field and forgets the read branch, and without this a v1 decoder
        // accepts the record, drops the field, and writes it back truncated.
        if (!a_bytes.empty()) {
            return false;
        }

        // Nothing above this line touched a member, so every early return left
        // this object exactly as it was.
        //
        // ⚠ REPLACE, NOT MERGE, and the direction matters. Merging reads as the
        // generous choice and is a fail-open door: it would mean a load can only
        // ever ADD to what the object already holds, so the day the revert clear
        // in Persistence::RevertCallback is missed, removed or reordered, the
        // previous character's colours survive into this character's set and
        // SaveCallback stamps them into their save as earned. Unlocks are add
        // only, so nothing ever takes them back. Replacing turns that same
        // missed clear into nothing at all: the record simply wins.
        //
        // ⚠ CONSEQUENCE FOR CALL ORDER: ANYTHING THAT ADDS IDS MUST RUN AFTER
        // THIS, never before. The promotion pass is the code that will make
        // that matter. Promote first and this assignment discards every colour
        // it just granted, silently, because promotion re-grants most of them
        // on the following load and the loss reads as a delay.
        ids_    = std::move(loadedIds);
        deeds_  = std::move(loadedDeeds);
        charge_ = loadedCharge;
        return true;
    }

    namespace DyeUnlocks {

        namespace {
            std::mutex   g_lock;
            DyeUnlockSet g_set;
            // ⚠ UNDER THE SAME LOCK AS g_set, and that is not laziness. The
            // baseline reads the set and writes this in one breath, and the
            // palette's draw asks both about the same colour on the render
            // thread; two locks would be two answers a frame apart.
            std::set<std::string> g_acked;
            bool                  g_ackBaselineOwed = true;
        }  // namespace

        void With(const std::function<void(DyeUnlockSet&)>& a_fn) {
            if (!a_fn) {
                return;
            }
            std::scoped_lock l(g_lock);
            a_fn(g_set);
        }

        DyeUnlockSet Snapshot() {
            std::scoped_lock l(g_lock);
            return g_set;
        }

        std::uint32_t CurrentCharge() {
            std::scoped_lock l(g_lock);
            return g_set.Charge();
        }

        bool Acknowledged(std::string_view a_id) {
            std::scoped_lock l(g_lock);
            return g_acked.contains(std::string(a_id));
        }

        void Acknowledge(std::string_view a_id) {
            if (a_id.empty() || a_id.size() > kMaxStrLen) {
                return;  // Add's own invariants; an id it would refuse cannot be seen
            }
            std::scoped_lock l(g_lock);
            // ⚠ ONLY WHAT IS ACTUALLY UNLOCKED. Acknowledging a locked colour
            // would bank a mark for something the player has not earned, and
            // the moment they earn it the corner they were owed is gone.
            if (g_set.Has(a_id)) {
                g_acked.insert(std::string(a_id));
            }
        }

        std::size_t TakeAckBaselineIfOwed() {
            std::size_t      adopted = 0;
            std::scoped_lock l(g_lock);
            if (!g_ackBaselineOwed) {
                return 0;
            }
            g_ackBaselineOwed = false;
            for (const auto& id : g_set.Ids()) {
                if (g_acked.insert(id).second) {
                    ++adopted;
                }
            }
            return adopted;
        }

        std::vector<std::byte> EncodeAcked() {
            std::scoped_lock       l(g_lock);
            std::vector<std::byte> out;
            AppendU32(out, static_cast<std::uint32_t>(g_acked.size()));
            for (const auto& id : g_acked) {
                AppendString(out, id);
            }
            return out;
        }

        bool DecodeAcked(std::span<const std::byte> a_bytes, std::uint32_t a_version) {
            // The same range rule the unlock record uses, and for the same
            // reason: an equality test here would make any future bump throw
            // away every mark rather than read the older shape.
            if (!DyeVersionLoadable(a_version, kDyeUnlockVersion)) {
                return false;
            }
            std::uint32_t count = 0;
            if (!ReadU32(a_bytes, count) || count > kMaxUnlocks) {
                return false;
            }
            std::set<std::string> loaded;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::string id;
                if (!ReadString(a_bytes, id)) {
                    return false;  // truncated: refuse the whole record
                }
                loaded.insert(std::move(id));
            }
            if (!a_bytes.empty()) {
                return false;  // trailing garbage
            }
            std::scoped_lock l(g_lock);
            g_acked = std::move(loaded);
            // The record's PRESENCE is the signal, not its contents: a save
            // that has been through this and acknowledged nothing carries an
            // empty record, and re-taking the baseline over it would clear
            // every mark the player had not got to yet.
            g_ackBaselineOwed = false;
            return true;
        }

        void ClearAcked() {
            std::scoped_lock l(g_lock);
            g_acked.clear();
            // Owed again, which is what makes a new game behave: revert runs
            // before every load AND before a new game, and only a decoded
            // record says otherwise.
            g_ackBaselineOwed = true;
        }

    }  // namespace DyeUnlocks

}  // namespace OS
