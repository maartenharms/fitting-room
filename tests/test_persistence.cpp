// Codec tests. No SKSE, no engine - Encode/Decode operate on plain bytes.
#include "Outfit.h"
#include "PersistenceCodec.h"

#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

// Little-endian u32, matching the codec's on-disk layout. Lets a test
// hand-build a corrupt buffer that Encode() would never emit.
static void PutLE32(std::vector<std::byte>& a_out, std::uint32_t a_v) {
    for (int i = 0; i < 4; ++i) {
        a_out.push_back(static_cast<std::byte>((a_v >> (i * 8)) & 0xFF));
    }
}

int main() {
    using namespace OS;

    {  // round-trip: names, favorite flag, active index, styles and hides
        OutfitLibrary lib;
        const int a = lib.Create("Court Dress");
        const int b = lib.Create("Dungeon");
        lib.At(a)->favorite = true;
        lib.At(a)->SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        lib.At(a)->SetHide(kBitHair);
        lib.At(b)->SetStyle(kBitFeet, StyleRefKey{ "Skyrim.esm", 0x1B3A3 });
        lib.Activate(static_cast<std::size_t>(b));

        const auto bytes = Encode(lib);
        CHECK(!bytes.empty());

        OutfitLibrary out;
        CHECK(Decode(bytes, kCodecVersion, out));
        CHECK(out.Count() == 2);
        CHECK(out.ActiveIndex() == b);
        // Dereferencing At(i) after a failed Decode would crash the harness;
        // fail loudly with a line number instead.
        auto* o0 = out.At(0);
        auto* o1 = out.At(1);
        CHECK(o0 != nullptr);
        CHECK(o1 != nullptr);
        if (o0 && o1) {
            CHECK(o0->name == "Court Dress");
            CHECK(o0->favorite == true);
            CHECK(o0->EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
            CHECK(o0->EntryFor(kBitBody).style.modName == "Armors.esp");
            CHECK(o0->EntryFor(kBitBody).style.localFormID == 0x801);
            CHECK(o0->EntryFor(kBitHair).kind == SlotEntry::Kind::kHide);
            CHECK(o1->EntryFor(kBitFeet).style.localFormID == 0x1B3A3);
            CHECK(o1->EntryFor(kBitBody).kind == SlotEntry::Kind::kPassthrough);
        }
    }

    {  // empty library round-trips
        OutfitLibrary lib;
        OutfitLibrary out;
        out.Create("stale");           // must be cleared by Decode
        CHECK(Decode(Encode(lib), kCodecVersion, out));
        CHECK(out.Count() == 0);
        CHECK(out.ActiveIndex() == -1);
    }

    {  // a future version is refused, and the target is left untouched
        OutfitLibrary lib;
        lib.Create("keep me");
        auto bytes = Encode(lib);

        OutfitLibrary out;
        out.Create("existing");
        CHECK(!Decode(bytes, kCodecVersion + 1, out));
        CHECK(out.Count() == 1);
        auto* o0 = out.At(0);
        CHECK(o0 != nullptr);
        if (o0) {
            CHECK(o0->name == "existing");
        }
    }

    {  // truncated data is rejected, not read out of bounds
        OutfitLibrary lib;
        lib.Create("Court Dress");
        lib.At(0)->SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        auto bytes = Encode(lib);
        bytes.resize(bytes.size() / 2);

        OutfitLibrary out;
        CHECK(!Decode(bytes, kCodecVersion, out));
    }

    {  // a wildly long string length must not allocate or overrun
        std::vector<std::byte> evil(16, std::byte{ 0xFF });
        OutfitLibrary out;
        CHECK(!Decode(evil, kCodecVersion, out));
    }

    {  // an over-long name whose bytes ARE all present: only the kMaxStringLen
       // cap can reject it - Need(len) alone cannot, since the 600 bytes exist.
       // (The all-0xFF case above dies earlier on count>kMaxOutfitCount, so it
       // never reaches Reader::Str; this record is otherwise fully valid.)
        std::vector<std::byte> buf;
        PutLE32(buf, 1);                       // count = 1
        PutLE32(buf, 0);                       // active = 0 (none)
        PutLE32(buf, 600);                     // name length = 600 (> kMaxStringLen)
        for (int i = 0; i < 600; ++i) {
            buf.push_back(static_cast<std::byte>('A'));  // 600 real bytes present
        }
        buf.push_back(std::byte{ 0 });         // favorite = 0
        PutLE32(buf, 0);                       // slot count = 0 (buffer exactly consumed)

        OutfitLibrary out;
        CHECK(!Decode(buf, kCodecVersion, out));
    }

    {  // a valid payload with one extra trailing byte is rejected as garbage,
       // not silently accepted (truncation is caught elsewhere, by !r.ok).
        OutfitLibrary lib;
        lib.Create("Court Dress");
        lib.At(0)->SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        auto bytes = Encode(lib);
        bytes.push_back(std::byte{ 0 });       // trailing garbage

        OutfitLibrary out;
        CHECK(!Decode(bytes, kCodecVersion, out));
    }

    {  // a non-empty library with NO active outfit round-trips: exercises the
       // active==0 -> Deactivate() branch with count>0, and clears any stale
       // active index already sitting in the target.
        OutfitLibrary lib;
        lib.Create("Alpha");
        lib.Create("Beta");
        lib.Deactivate();

        OutfitLibrary out;
        out.Create("stale");
        out.Activate(0);                       // target starts WITH an active outfit
        CHECK(out.ActiveIndex() == 0);
        CHECK(Decode(Encode(lib), kCodecVersion, out));
        CHECK(out.Count() == 2);
        CHECK(out.ActiveIndex() == -1);
    }

    {  // a style with an empty modName survives the round-trip: the len==0
       // path in PutStr/Reader::Str (zero-length string, no bytes written/read).
        OutfitLibrary lib;
        lib.Create("Naked Body");
        lib.At(0)->SetStyle(kBitBody, StyleRefKey{ "", 0x123 });

        OutfitLibrary out;
        CHECK(Decode(Encode(lib), kCodecVersion, out));
        CHECK(out.Count() == 1);
        auto* o0 = out.At(0);
        CHECK(o0 != nullptr);
        if (o0) {
            CHECK(o0->EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
            CHECK(o0->EntryFor(kBitBody).style.modName == "");
            CHECK(o0->EntryFor(kBitBody).style.localFormID == 0x123);
        }
    }

    if (g_failures == 0) {
        std::printf("PersistenceTests: all passed\n");
        return 0;
    }
    std::printf("PersistenceTests: %d failure(s)\n", g_failures);
    return 1;
}
