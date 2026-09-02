// Dye history tests. No SKSE, no engine, no ImGui: a capped most-recent-first
// list of colours and its wire format are pure.
#include "DyeHistory.h"

#include <algorithm>
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
    {  // most recent first
        DyeHistoryList h;
        h.Add(1, 2, 3);
        h.Add(4, 5, 6);
        const auto e = h.Entries();
        CHECK(e.size() == 2);
        CHECK(e[0].r == 4 && e[0].g == 5 && e[0].b == 6);
        CHECK(e[1].r == 1 && e[1].g == 2 && e[1].b == 3);
    }

    {  // a repeat promotes rather than appends
        DyeHistoryList h;
        h.Add(1, 2, 3);
        h.Add(4, 5, 6);
        h.Add(1, 2, 3);
        const auto e = h.Entries();
        CHECK(e.size() == 2);
        CHECK(e[0].r == 1 && e[0].g == 2 && e[0].b == 3);
        CHECK(e[1].r == 4);
    }

    {  // the cap evicts the oldest
        DyeHistoryList h;
        for (std::uint32_t i = 0; i < kMaxHistoryColours + 5; ++i) {
            h.Add(static_cast<std::uint8_t>(i), 0, 0);
        }
        const auto e = h.Entries();
        CHECK(e.size() == kMaxHistoryColours);
        CHECK(e[0].r == static_cast<std::uint8_t>(kMaxHistoryColours + 4));
        CHECK(e.back().r == 5);
    }

    {  // Clear empties it
        DyeHistoryList h;
        h.Add(9, 9, 9);
        h.Clear();
        CHECK(h.Size() == 0);
        CHECK(h.Entries().empty());
    }

    {  // a colour matching an installed dye is refused; one matching nothing is kept
        std::vector<Dye> palette;
        Dye              d;
        d.id     = "eso:test-red";
        d.name   = "Test Red";
        d.colour = DyeChannel{ true, 0xB5, 0x44, 0x3A };
        palette.push_back(d);

        Outfit base;
        Outfit staged;
        staged.SetDye(2, DyeChannelId::kPrimary,
                      DyeChannel{ true, 0xB5, 0x44, 0x3A });  // equals the palette
        staged.SetDye(3, DyeChannelId::kPrimary,
                      DyeChannel{ true, 0x12, 0x34, 0x56 });  // equals nothing

        const auto got = ChangedHandPickedColours(base, staged, palette);
        CHECK(got.size() == 1);
        CHECK(got[0].r == 0x12 && got[0].g == 0x34 && got[0].b == 0x56);
    }

    {  // the shared predicate both callers use
        std::vector<Dye> palette;
        Dye              d;
        d.id     = "eso:test-red";
        d.colour = DyeChannel{ true, 0xB5, 0x44, 0x3A };
        palette.push_back(d);
        CHECK(!IsHandPicked(0xB5, 0x44, 0x3A, palette));  // the palette reaches it
        CHECK(IsHandPicked(0xB5, 0x44, 0x3B, palette));   // one byte off, it does not
        CHECK(IsHandPicked(0x12, 0x34, 0x56, palette));
        CHECK(IsHandPicked(0x12, 0x34, 0x56, {}));        // empty palette reaches nothing
    }

    {  // an unchanged channel is not re-captured, and a cleared one is not captured
        std::vector<Dye> palette;
        Outfit           base;
        base.SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 0x12, 0x34, 0x56 });
        Outfit staged = base;  // identical: nothing changed
        CHECK(ChangedHandPickedColours(base, staged, palette).empty());

        staged.SetDye(2, DyeChannelId::kPrimary, DyeChannel{});  // cleared
        CHECK(ChangedHandPickedColours(base, staged, palette).empty());
    }

    {  // round trip
        DyeHistoryList a;
        a.Add(1, 2, 3);
        a.Add(4, 5, 6);
        DyeHistoryList b;
        CHECK(b.Decode(a.Encode(), kDyeHistoryVersion));
        CHECK(b.Entries() == a.Entries());
    }

    {  // ⚠ THE GOLDEN v1 LAYOUT. NEVER regenerate this from Encode: its whole
       // job is to fail when the layout drifts, and every other case in this
       // file derives its bytes from Encode, so a reorder would move encoder
       // and decoder together and keep them all green while making every
       // shipped save unreadable.
        DyeHistoryList a;
        a.Add(0x11, 0x22, 0x33);
        a.Add(0x44, 0x55, 0x66);
        const std::byte want[] = {
            std::byte{ 0x02 }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x00 },
            std::byte{ 0x44 }, std::byte{ 0x55 }, std::byte{ 0x66 },
            std::byte{ 0x11 }, std::byte{ 0x22 }, std::byte{ 0x33 },
        };
        const auto got = a.Encode();
        CHECK(got.size() == sizeof(want));
        CHECK(std::equal(got.begin(), got.end(), std::begin(want)));
    }

    {  // a truncated record is refused and leaves the object untouched
        DyeHistoryList a;
        a.Add(7, 7, 7);
        auto bytes = a.Encode();
        bytes.resize(bytes.size() - 1);  // one byte short of the last entry
        DyeHistoryList b;
        b.Add(9, 9, 9);
        CHECK(!b.Decode(bytes, kDyeHistoryVersion));
        CHECK(b.Size() == 1);
        CHECK(b.Entries()[0].r == 9);  // its own content survived
    }

    {  // a record from a newer build is refused
        DyeHistoryList a;
        a.Add(1, 1, 1);
        DyeHistoryList b;
        CHECK(!b.Decode(a.Encode(), kDyeHistoryVersion + 1));
        CHECK(b.Size() == 0);
    }

    {  // a count past the cap is refused rather than allocated
        const std::vector<std::byte> bytes{ std::byte{ 0xFF }, std::byte{ 0xFF },
                                            std::byte{ 0xFF }, std::byte{ 0xFF } };
        DyeHistoryList b;
        CHECK(!b.Decode(bytes, kDyeHistoryVersion));
        CHECK(b.Size() == 0);
    }

    if (g_failures == 0) {
        std::printf("DyeHistoryTests: all passed\n");
        return 0;
    }
    std::printf("DyeHistoryTests: %d failure(s)\n", g_failures);
    return 1;
}
