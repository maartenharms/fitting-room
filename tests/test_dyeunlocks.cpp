// Dye unlock tests. No SKSE, no engine: the earned set, the Seamstone charge
// and the deed counters are one record with a wire format, and all of it is
// provable without a save.
#include "DyeUnlocks.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

using namespace OS;

int main() {
    {  // the record round trips all three pieces, and an id the palette no
       // longer knows is KEPT rather than pruned: uninstalling a dye pack must
       // hide its colours, not spend the progression that earned them
        DyeUnlockSet set;
        CHECK(!set.Has("eso:obsidian-black"));
        CHECK(set.Charge() == 0u);

        CHECK(set.Add("eso:obsidian-black"));
        CHECK(set.Add("gone:removed-pack"));
        CHECK(set.Add("eso:obsidian-black"));   // idempotent
        CHECK(set.Has("eso:obsidian-black"));
        CHECK(set.Size() == 2);

        set.SetCharge(640);
        set.BumpDeed("channelsDyed", 9);
        set.BumpDeed("channelsDyed", 3);
        CHECK(set.Charge() == 640u);
        CHECK(set.Deed("channelsDyed") == 12u);
        CHECK(set.Deed("neverTouched") == 0u);

        const auto bytes = set.Encode();
        DyeUnlockSet back;
        CHECK(back.Decode(bytes, kDyeUnlockVersion));
        CHECK(back.Size() == 2);
        CHECK(back.Has("eso:obsidian-black"));
        CHECK(back.Has("gone:removed-pack"));   // unresolvable, still kept
        CHECK(back.Charge() == 640u);
        CHECK(back.Deed("channelsDyed") == 12u);

        DyeUnlockSet wrongVersion;
        CHECK(!wrongVersion.Decode(bytes, kDyeUnlockVersion + 1));
    }

    {  // a truncated record is refused WHOLE rather than half applied, so a
       // corrupt co-save cannot leave a character with someone else's colours
        DyeUnlockSet set;
        CHECK(set.Add("eso:void-pitch"));
        set.SetCharge(100);
        auto bytes = set.Encode();
        bytes.resize(bytes.size() - 3);

        DyeUnlockSet back;
        CHECK(back.Add("eso:untouched"));
        CHECK(!back.Decode(bytes, kDyeUnlockVersion));
        CHECK(back.Has("eso:untouched"));   // the failed decode changed nothing
        CHECK(back.Size() == 1);
    }

    {  // Clear resets all three. Loading a second save in one session would
       // otherwise hand the new character the first one's colours and charge.
        DyeUnlockSet set;
        CHECK(set.Add("eso:void-pitch"));
        set.SetCharge(500);
        set.BumpDeed("channelsDyed", 4);
        set.Clear();
        CHECK(set.Size() == 0);
        CHECK(set.Charge() == 0u);
        CHECK(set.Deed("channelsDyed") == 0u);
    }

    {  // charge saturates at the cap rather than wrapping. A grand gem into an
       // almost full stone must not roll a u32 over to nearly nothing.
        DyeUnlockSet set;
        set.SetCharge(900);
        CHECK(set.AddCharge(400, 1000) == 100u);   // 900 -> 1000, only 100 fit
        CHECK(set.Charge() == 1000u);

        // ⚠ ADDING CHARGE MUST NEVER REDUCE IT. A stone already over the cap is
        // ordinary: iSeamstoneCapacity is a user editable INI key and lowering
        // it is a normal way to make the economy harsher. min(charge + amount,
        // cap) would turn this petty gem into a 3000 charge loss.
        set.SetCharge(4000);
        CHECK(set.AddCharge(25, 1000) == 0u);
        CHECK(set.Charge() == 4000u);   // refused, NOT clamped down

        // exactly at the cap is the same case
        set.SetCharge(1000);
        CHECK(set.AddCharge(25, 1000) == 0u);
        CHECK(set.Charge() == 1000u);

        // and a u32 near the ceiling still cannot wrap
        set.SetCharge(4294967290u);
        CHECK(set.AddCharge(400, 4294967295u) == 5u);   // only 5 fit before the ceiling
        CHECK(set.Charge() == 4294967295u);
    }

    {  // ⚠ ANYTHING REACHABLE THROUGH THE PUBLIC API MUST ROUND TRIP. Encode
       // writes the true length of what it holds, so a cap enforced only on
       // the read side lets the object emit bytes it cannot read back, and
       // because Decode is all or nothing that kills the WHOLE record: every
       // other colour, the charge and the deeds with it.
        DyeUnlockSet set;
        // 300 is chosen to sit above kMaxStrLen, which is 256. If that cap ever
        // moves these two lines are the tripwire, and a cap raise is a format
        // change requiring a kDyeUnlockVersion bump: see the comment beside the
        // caps in DyeUnlocks.cpp before touching either.
        CHECK(!set.Add(std::string(300, 'x')));   // over kMaxStrLen: refused at Add
        CHECK(set.Add("eso:normal"));
        set.BumpDeed(std::string(300, 'y'), 5);   // same
        set.BumpDeed("channelsDyed", 7);
        CHECK(set.Size() == 1);              // the long one never got in
        CHECK(set.Deed("channelsDyed") == 7u);

        DyeUnlockSet back;
        CHECK(back.Decode(set.Encode(), kDyeUnlockVersion));
        CHECK(back.Size() == 1);
        CHECK(back.Has("eso:normal"));
        CHECK(back.Deed("channelsDyed") == 7u);
    }

    {  // ⚠ THE COUNT CAPS ARE PART OF THE WIRE FORMAT, and they are pinned here
       // as literals ON PURPOSE. Raising kMaxUnlocks or kMaxDeeds in
       // DyeUnlocks.cpp fails this block, which is the point: a cap raise is a
       // format change needing a kDyeUnlockVersion bump, and it must not be
       // reachable by editing one number in one file. The size static_assert
       // beside those caps holds the other half of the same decision.
        DyeUnlockSet set;
        bool         lastAccepted = true;
        for (int i = 0; i < 2049; ++i) {
            lastAccepted = set.Add("id" + std::to_string(i));
        }
        CHECK(!lastAccepted);        // the one past the cap was refused
        CHECK(set.Size() == 2048);
        // an id already held still reports true AT the cap. Only new ids are
        // refused, or a full palette would start failing to re-confirm colours
        // the player has already earned.
        CHECK(set.Add("id0"));
        CHECK(set.Size() == 2048);

        DyeUnlockSet deeds;
        for (int i = 0; i < 65; ++i) {
            deeds.BumpDeed("deed" + std::to_string(i), 1);
        }
        CHECK(deeds.Deed("deed63") == 1u);   // the last one under the cap got in
        CHECK(deeds.Deed("deed64") == 0u);   // the one past it did not
        deeds.BumpDeed("deed0", 4);          // an EXISTING deed still bumps at the cap
        CHECK(deeds.Deed("deed0") == 5u);
    }

    {  // a record whose count is over the cap is refused WHOLE, not trimmed.
       // This is the rollback door the cap comment in DyeUnlocks.cpp describes:
       // a future build that raises kMaxDeeds writes a legitimate v1 record an
       // older build cannot read, and the older build does not lose the extra
       // deeds, it loses the charge and every colour in the same record.
        std::vector<std::byte> bytes;
        const auto             u32 = [&bytes](std::uint32_t a_v) {
            for (int i = 0; i < 4; ++i) {
                bytes.push_back(static_cast<std::byte>((a_v >> (i * 8)) & 0xFF));
            }
        };
        const auto str = [&bytes, &u32](const std::string& a_s) {
            u32(static_cast<std::uint32_t>(a_s.size()));
            for (const char c : a_s) {
                bytes.push_back(static_cast<std::byte>(c));
            }
        };
        u32(1);
        str("eso:void-pitch");
        u32(1234);   // a charge with no second source anywhere
        u32(65);     // one deed past kMaxDeeds
        for (int i = 0; i < 65; ++i) {
            str("deed" + std::to_string(i));
            u32(1);
        }

        DyeUnlockSet back;
        CHECK(!back.Decode(bytes, kDyeUnlockVersion));
        CHECK(back.Size() == 0);
        CHECK(back.Charge() == 0u);   // the colours and the charge went with it
    }

    {  // trailing bytes are refused. This is the net that catches a botched
       // version bump: someone appends a v2 field and forgets the read branch,
       // and without this a v1 decoder accepts the record, drops the field,
       // and writes it back truncated.
        DyeUnlockSet set;
        CHECK(set.Add("eso:void-pitch"));
        auto bytes = set.Encode();
        bytes.push_back(std::byte{ 0 });

        DyeUnlockSet back;
        CHECK(!back.Decode(bytes, kDyeUnlockVersion));
    }

    {  // the version is a RANGE, not equality. A v1 record must still load
       // when kDyeUnlockVersion moves on, or bumping it silently destroys
       // every existing player's charge and deeds. Only a record from a
       // NEWER build than ours is refused.
        DyeUnlockSet set;
        CHECK(set.Add("eso:void-pitch"));
        set.SetCharge(250);
        const auto bytes = set.Encode();

        DyeUnlockSet v1;
        CHECK(v1.Decode(bytes, 1));
        CHECK(v1.Charge() == 250u);

        DyeUnlockSet zero;
        CHECK(!zero.Decode(bytes, 0));                        // never written
        DyeUnlockSet future;
        CHECK(!future.Decode(bytes, kDyeUnlockVersion + 1));  // a downgrade

        // ⚠ The runtime checks above cannot actually tell a range from an
        // equality while kDyeUnlockVersion is 1, because "a v1 record under a
        // v2 build" is unconstructible. The static_asserts on
        // DyeVersionLoadable in the header are what pin that, and they fail at
        // COMPILE time against an equality. These stay because they document
        // the rule where it is used.
    }

    {  // ⚠ A REAL v1 RECORD, BYTE FOR BYTE. NEVER regenerate this from
       // Encode(): its entire job is to fail when Encode's layout drifts, and
       // every other test in this file would stay green through exactly that
       // change, because they all derive their bytes from Encode.
       //
       // The version range protects the version NUMBER, not the layout. Swap
       // charge and deedCount, both u32, and a record with charge 0 and no
       // deeds still decodes with the meanings exchanged. Once a release
       // ships, this layout is frozen whether or not anything asserts it.
        static constexpr unsigned char kV1[] = {
            0x01, 0, 0, 0,                                     // one unlock
            0x0E, 0, 0, 0,                                     // id length 14
            'e',  's', 'o', ':', 'v', 'o', 'i', 'd', '-', 'p', 'i', 't', 'c', 'h',
            0xFA, 0, 0, 0,                                     // charge 250
            0x00, 0, 0, 0,                                     // no deeds
        };
        DyeUnlockSet back;
        CHECK(back.Decode(std::as_bytes(std::span{ kV1 }), 1));
        CHECK(back.Size() == 1);
        CHECK(back.Has("eso:void-pitch"));
        CHECK(back.Charge() == 250u);
        CHECK(back.Deed("channelsDyed") == 0u);
    }

    {  // a deed name twice is a corrupt record, not a merge. Keeping the first
       // silently would leave deedCount disagreeing with the map.
        std::vector<std::byte> bytes;
        const auto             u32 = [&bytes](std::uint32_t a_v) {
            for (int i = 0; i < 4; ++i) {
                bytes.push_back(static_cast<std::byte>((a_v >> (i * 8)) & 0xFF));
            }
        };
        const auto str = [&bytes, &u32](std::string_view a_s) {
            u32(static_cast<std::uint32_t>(a_s.size()));
            for (const char c : a_s) {
                bytes.push_back(static_cast<std::byte>(c));
            }
        };
        u32(0);                    // no unlocks
        u32(0);                    // no charge
        u32(2);                    // two deeds...
        str("channelsDyed");
        u32(10);
        str("channelsDyed");       // ...with the same name
        u32(99);

        DyeUnlockSet back;
        CHECK(!back.Decode(bytes, kDyeUnlockVersion));
    }

    {  // Add reports whether the id got in, so Promote can avoid announcing a
       // colour it failed to store. Without this it would re-announce that
       // colour on every load forever.
        DyeUnlockSet set;
        CHECK(set.Add("eso:fine"));
        CHECK(!set.Add(""));
        CHECK(!set.Add(std::string(300, 'x')));   // over the 256-byte kMaxStrLen
        CHECK(set.Add("eso:fine"));   // already held still reports true
        CHECK(set.Size() == 1);
    }

    {  // AddCharge reports what it applied, so the cost plan can tell the
       // player a gem was wasted rather than silently eating it
        DyeUnlockSet set;
        CHECK(set.AddCharge(400, 1000) == 400u);
        CHECK(set.AddCharge(400, 1000) == 400u);
        CHECK(set.AddCharge(400, 1000) == 200u);   // saturated at the cap
        CHECK(set.Charge() == 1000u);
        CHECK(set.AddCharge(400, 1000) == 0u);     // already full
        set.SetCharge(4000);
        CHECK(set.AddCharge(25, 1000) == 0u);      // over a lowered cap
        CHECK(set.Charge() == 4000u);
    }

    {  // ChargeRoomFor is what the recharge shows the player BEFORE it eats the
       // soul gem, so it has to answer exactly what AddCharge will then do.
       // They share a body today; this holds the line if that ever stops being
       // true, which is the edit that would tell someone a Grand soul fits and
       // then swallow it.
        const std::uint32_t charges[] = { 0u, 1u, 999u, 1000u, 1001u, 4000u,
                                          4294967290u, 4294967295u };
        const std::uint32_t amounts[] = { 0u, 1u, 25u, 400u, 3000u, 4294967295u };
        const std::uint32_t caps[]    = { 0u, 1u, 1000u, 5000u, 4294967295u };
        for (const auto c : charges) {
            for (const auto a : amounts) {
                for (const auto cap : caps) {
                    DyeUnlockSet probe;
                    probe.SetCharge(c);
                    const auto predicted = probe.ChargeRoomFor(a, cap);
                    // ⚠ Read the charge BEFORE the call: AddCharge moves it.
                    const auto before  = probe.Charge();
                    const auto applied = probe.AddCharge(a, cap);
                    CHECK(predicted == applied);
                    // And the prediction is a real quantity, not just a number
                    // that matches: it is exactly how far the charge moved.
                    CHECK(probe.Charge() - before == predicted);
                    // What a gem of this size would waste. Never negative,
                    // which is the arithmetic a "you will lose N" prompt does.
                    CHECK(predicted <= a);
                }
            }
        }
        // The preview does not mutate, which is the entire point of having it.
        DyeUnlockSet quiet;
        quiet.SetCharge(250);
        CHECK(quiet.ChargeRoomFor(4000, 5000) == 4000u);
        CHECK(quiet.ChargeRoomFor(4000, 5000) == 4000u);
        CHECK(quiet.Charge() == 250u);
    }

    {  // a zero length id or deed name is refused on the way IN, so Decode
       // must refuse one too rather than being a second constructor that
       // skips the invariants
        DyeUnlockSet set;
        CHECK(!set.Add(""));
        set.BumpDeed("", 5);
        CHECK(set.Size() == 0);
        CHECK(set.Deed("") == 0u);

        // hand-built record carrying one zero-length id
        std::vector<std::byte> bytes;
        const auto             u32 = [&bytes](std::uint32_t a_v) {
            for (int i = 0; i < 4; ++i) {
                bytes.push_back(static_cast<std::byte>((a_v >> (i * 8)) & 0xFF));
            }
        };
        u32(1);  // one id
        u32(0);  // of length zero
        u32(0);  // charge
        u32(0);  // no deeds

        DyeUnlockSet back;
        CHECK(!back.Decode(bytes, kDyeUnlockVersion));
    }

    {  // an empty span is refused and leaves the target alone
        DyeUnlockSet back;
        CHECK(back.Add("eso:survivor"));
        CHECK(!back.Decode(std::span<const std::byte>{}, kDyeUnlockVersion));
        CHECK(back.Has("eso:survivor"));
    }

    {  // the singleton hands out the same instance, and Snapshot is a COPY
        DyeUnlocks::With([](DyeUnlockSet& a_set) {
            a_set.Clear();
            CHECK(a_set.Add("eso:live"));
        });
        auto copy = DyeUnlocks::Snapshot();
        CHECK(copy.Has("eso:live"));

        CHECK(copy.Add("eso:only-in-the-copy"));
        CHECK(!DyeUnlocks::Snapshot().Has("eso:only-in-the-copy"));

        DyeUnlocks::With([](DyeUnlockSet& a_set) { a_set.Clear(); });
        CHECK(DyeUnlocks::Snapshot().Size() == 0);
    }

    {  // spending refuses rather than underflowing
        DyeUnlockSet set;
        set.SetCharge(100);
        CHECK(set.SpendCharge(60));
        CHECK(set.Charge() == 40u);
        CHECK(!set.SpendCharge(41));
        CHECK(set.Charge() == 40u);   // a refused spend takes nothing
        CHECK(set.SpendCharge(40));
        CHECK(set.Charge() == 0u);
    }
    if (g_failures == 0) {
        std::printf("DyeUnlocksTests: all passed\n");
        return 0;
    }
    std::printf("DyeUnlocksTests: %d failure(s)\n", g_failures);
    return 1;
}
