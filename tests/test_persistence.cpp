// Codec tests. No SKSE, no engine - Encode/Decode operate on plain bytes.
#include "NpcAssignments.h"
#include "Outfit.h"
#include "PersistenceCodec.h"

#include <cstdio>
#include <unordered_map>

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

// Frozen length-prefixed string, the codec's v1 on-disk shape - local like
// PutLE32, and for the same reason (see its comment above).
static void PutStrV1(std::vector<std::byte>& a_out, const std::string& a_s) {
    PutLE32(a_out, static_cast<std::uint32_t>(a_s.size()));
    for (char c : a_s) {
        a_out.push_back(static_cast<std::byte>(c));
    }
}

// Frozen shape of Encode() from before the v2 weapon block existed
// (armor-only: count, active, then per outfit name/favorite/slot-entries).
// A real 0.1.1-era save's co-save bytes look exactly like this. Decoding
// them correctly under the current (v2) codec is the single most important
// guarantee in this file - see the CRITICAL note in PersistenceCodec.h.
// Built ONLY on the local PutLE32/PutStrV1 above, never on the production
// detail:: primitives - an oracle that shares the code under test would
// silently drift with it.
static std::vector<std::byte> EncodeV1(const OS::OutfitLibrary& a_lib) {
    using namespace OS;
    std::vector<std::byte> out;
    PutLE32(out, static_cast<std::uint32_t>(a_lib.Count()));
    PutLE32(out, static_cast<std::uint32_t>(a_lib.ActiveIndex() + 1));  // 0 == none

    for (const auto& o : a_lib.All()) {
        PutStrV1(out, o.name);
        out.push_back(static_cast<std::byte>(o.favorite ? 1 : 0));

        std::uint32_t count = 0;
        for (std::uint32_t b = 0; b < kBitCount; ++b) {
            if (o.EntryFor(b).kind != SlotEntry::Kind::kPassthrough) {
                ++count;
            }
        }
        PutLE32(out, count);
        for (std::uint32_t b = 0; b < kBitCount; ++b) {
            const auto& e = o.EntryFor(b);
            if (e.kind == SlotEntry::Kind::kPassthrough) {
                continue;
            }
            PutLE32(out, b);
            out.push_back(static_cast<std::byte>(e.kind));
            PutStrV1(out, e.style.modName);
            PutLE32(out, e.style.localFormID);
        }
    }
    return out;
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

    {  // the full ten-slot player library survives a co-save round-trip,
       // including its highest valid active index
        OutfitLibrary lib;
        for (std::size_t i = 0; i < kMaxOutfits; ++i) {
            CHECK(lib.Create("Saved outfit") == static_cast<int>(i));
        }
        CHECK(lib.Count() == 10);
        lib.Activate(kMaxOutfits - 1);
        CHECK(lib.ActiveIndex() == 9);

        OutfitLibrary out;
        CHECK(Decode(Encode(lib), kCodecVersion, out));
        CHECK(out.Count() == 10);
        CHECK(out.ActiveIndex() == 9);
        CHECK(out.Create("overflow") == -1);
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

    {  // v2 round-trip: weapon entries (style + hide + passthrough, mixed
       // across several classes) survive Encode -> Decode exactly, alongside
       // the existing armor slot dimension.
        OutfitLibrary lib;
        const int a = lib.Create("Duelist");
        lib.At(a)->SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        lib.At(a)->SetWeaponStyle(WeaponClass::Sword, StyleRefKey{ "Weapons.esp", 0x10A });
        lib.At(a)->SetWeaponStyle(WeaponClass::Bolts, StyleRefKey{ "Ammo.esp", 0x55 });
        lib.At(a)->SetWeaponHide(WeaponClass::Bow);
        lib.At(a)->SetWeaponHide(WeaponClass::Crossbow);
        // Dagger, WarAxe, Mace, Greatsword, BattleaxeWarhammer, Staff, Arrows
        // are left at passthrough (default) - the mix this test is for.
        lib.Activate(static_cast<std::size_t>(a));

        const auto bytes = Encode(lib);
        OutfitLibrary out;
        CHECK(Decode(bytes, kCodecVersion, out));
        CHECK(out.Count() == 1);
        auto* o0 = out.At(0);
        CHECK(o0 != nullptr);
        if (o0) {
            CHECK(o0->EntryFor(kBitBody).style.modName == "Armors.esp");
            CHECK(o0->WeaponEntryFor(WeaponClass::Sword).kind == SlotEntry::Kind::kStyle);
            CHECK(o0->WeaponEntryFor(WeaponClass::Sword).style.modName == "Weapons.esp");
            CHECK(o0->WeaponEntryFor(WeaponClass::Sword).style.localFormID == 0x10A);
            CHECK(o0->WeaponEntryFor(WeaponClass::Bolts).kind == SlotEntry::Kind::kStyle);
            CHECK(o0->WeaponEntryFor(WeaponClass::Bolts).style.modName == "Ammo.esp");
            CHECK(o0->WeaponEntryFor(WeaponClass::Bolts).style.localFormID == 0x55);
            CHECK(o0->WeaponEntryFor(WeaponClass::Bow).kind == SlotEntry::Kind::kHide);
            CHECK(o0->WeaponEntryFor(WeaponClass::Crossbow).kind == SlotEntry::Kind::kHide);
            CHECK(o0->WeaponEntryFor(WeaponClass::Dagger).kind == SlotEntry::Kind::kPassthrough);
            CHECK(o0->WeaponEntryFor(WeaponClass::Arrows).kind == SlotEntry::Kind::kPassthrough);
        }
    }

    {  // v1 bytes decode cleanly under the current (v2) codec: a save
       // written by 0.1.1 must still load. Armor entries restore exactly;
       // the weapon dimension - which v1 bytes never had - stays all
       // passthrough rather than being left uninitialized or rejected.
        OutfitLibrary lib;
        const int a = lib.Create("Court Dress");
        lib.At(a)->favorite = true;
        lib.At(a)->SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        lib.At(a)->SetHide(kBitHair);
        lib.Activate(static_cast<std::size_t>(a));

        const auto v1Bytes = EncodeV1(lib);

        OutfitLibrary out;
        CHECK(Decode(v1Bytes, 1, out));
        CHECK(out.Count() == 1);
        CHECK(out.ActiveIndex() == a);
        auto* o0 = out.At(0);
        CHECK(o0 != nullptr);
        if (o0) {
            CHECK(o0->name == "Court Dress");
            CHECK(o0->favorite == true);
            CHECK(o0->EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
            CHECK(o0->EntryFor(kBitBody).style.modName == "Armors.esp");
            CHECK(o0->EntryFor(kBitHair).kind == SlotEntry::Kind::kHide);
            for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
                CHECK(o0->WeaponEntryFor(static_cast<WeaponClass>(c)).kind ==
                      SlotEntry::Kind::kPassthrough);
            }
            // v1 never had a body block either - it must read back as the
            // "changes nothing" default, not as garbage.
            CHECK(o0->obodyPreset.empty());
            CHECK(o0->orefit == ORefitMode::kDefault);
        }
    }

    {  // v4 round-trip: same-class hand overrides preserve style, explicit
       // real-weapon passthrough, and absent/inherit as distinct states.
        OutfitLibrary lib;
        lib.Create("Twin Blades");
        auto* o = lib.At(0);
        o->SetWeaponStyle(WeaponClass::Sword, { "Both.esp", 1 });
        o->SetWeaponStyle(WeaponClass::Sword, { "Right.esp", 2 },
                          WeaponHand::Right);
        o->SetWeaponPassthrough(WeaponClass::Sword, WeaponHand::Left);

        OutfitLibrary out;
        CHECK(Decode(Encode(lib), kCodecVersion, out));
        const auto* restored = out.At(0);
        CHECK(restored != nullptr);
        if (restored) {
            CHECK(restored->ResolvedWeaponEntryFor(
                      WeaponClass::Sword, WeaponHand::Right).style.modName ==
                  "Right.esp");
            CHECK(restored->WeaponOverrideFor(
                      WeaponClass::Sword, WeaponHand::Left).has_value());
            CHECK(restored->ResolvedWeaponEntryFor(
                      WeaponClass::Sword, WeaponHand::Left).kind ==
                  SlotEntry::Kind::kPassthrough);
            CHECK(!restored->WeaponOverrideFor(
                WeaponClass::Dagger, WeaponHand::Right));
        }
    }

    {  // hidden style survives follower co-save round-trip and can be shown again
        OutfitLibrary lib;
        const int idx = lib.Create("Covered");
        lib.At(idx)->SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        ToggleHideSlot(*lib.At(idx), kBitBody);

        OutfitLibrary out;
        CHECK(Decode(Encode(lib), kCodecVersion, out));
        auto* restored = out.At(0);
        CHECK(restored != nullptr);
        if (restored) {
            CHECK(restored->EntryFor(kBitBody).kind == SlotEntry::Kind::kHide);
            ToggleHideSlot(*restored, kBitBody);
            CHECK(restored->EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
            CHECK(restored->EntryFor(kBitBody).style ==
                  (StyleRefKey{ "Armors.esp", 0x801 }));
        }
    }

    {  // corrupt weapon block: truncating mid-weapon-entry is rejected, and
       // the out-library is left completely unmutated (same discipline the
       // existing armor truncation test above exercises, now for the v2
       // weapon block).
        OutfitLibrary lib;
        lib.Create("Duelist");
        lib.At(0)->SetWeaponStyle(WeaponClass::Sword, StyleRefKey{ "Weapons.esp", 0x10A });
        auto bytes = Encode(lib);
        bytes.resize(bytes.size() - 3);  // cuts into the last weapon entry's formID field

        OutfitLibrary out;
        out.Create("stale");
        CHECK(!Decode(bytes, kCodecVersion, out));
        CHECK(out.Count() == 1);
        auto* o0 = out.At(0);
        CHECK(o0 != nullptr);
        if (o0) {
            CHECK(o0->name == "stale");
        }
    }

    {  // forward tolerance: a weapon entry whose class byte is unknown to
       // this build (>= kWeaponClassCount) is consumed and skipped, not
       // fatal - the rest of the outfit still loads. Hand-built bytes,
       // since Encode() would never emit an invalid class index itself.
        std::vector<std::byte> buf;
        PutLE32(buf, 1);                     // outfit count = 1
        PutLE32(buf, 1);                     // active = outfit 0
        PutLE32(buf, 4);                     // name length = 4
        buf.push_back(std::byte{ 'T' });
        buf.push_back(std::byte{ 'e' });
        buf.push_back(std::byte{ 's' });
        buf.push_back(std::byte{ 't' });
        buf.push_back(std::byte{ 0 });       // favorite = false
        PutLE32(buf, 0);                     // armor slot count = 0

        PutLE32(buf, 2);                     // weapon entry count = 2
        // entry 0: class byte == kWeaponClassCount - out of range, unknown
        buf.push_back(static_cast<std::byte>(kWeaponClassCount));
        buf.push_back(static_cast<std::byte>(SlotEntry::Kind::kStyle));
        PutLE32(buf, 1);                     // mod name length = 1
        buf.push_back(std::byte{ 'X' });
        PutLE32(buf, 5);                     // formID
        // entry 1: a valid Sword style entry
        buf.push_back(static_cast<std::byte>(WeaponClass::Sword));
        buf.push_back(static_cast<std::byte>(SlotEntry::Kind::kStyle));
        PutLE32(buf, 1);                     // mod name length = 1
        buf.push_back(std::byte{ 'Y' });
        PutLE32(buf, 7);                     // formID

        // v3 body block - this buffer is decoded AS the current version, so it
        // must carry every block that version defines.
        PutLE32(buf, 0);                     // empty preset name
        buf.push_back(std::byte{ 0 });       // ORefitMode::kDefault
        PutLE32(buf, 0);                     // no per-hand overrides (v4)

        OutfitLibrary out;
        CHECK(Decode(buf, kCodecVersion, out));
        CHECK(out.Count() == 1);
        auto* o0 = out.At(0);
        CHECK(o0 != nullptr);
        if (o0) {
            CHECK(o0->name == "Test");
            CHECK(o0->WeaponEntryFor(WeaponClass::Sword).kind == SlotEntry::Kind::kStyle);
            CHECK(o0->WeaponEntryFor(WeaponClass::Sword).style.modName == "Y");
            CHECK(o0->WeaponEntryFor(WeaponClass::Sword).style.localFormID == 7);
            CHECK(o0->WeaponEntryFor(WeaponClass::Bow).kind == SlotEntry::Kind::kPassthrough);
        }
    }

    {  // forward tolerance: a weapon entry with a valid class but an unknown
       // KIND byte (> kHide) is consumed and skipped too - kinds may grow
       // meanings in a future build just like classes may be appended.
        std::vector<std::byte> buf;
        PutLE32(buf, 1);                     // outfit count = 1
        PutLE32(buf, 0);                     // active = 0 (none)
        PutStrV1(buf, "Test");
        buf.push_back(std::byte{ 0 });       // favorite = 0
        PutLE32(buf, 0);                     // armor slot count = 0

        PutLE32(buf, 2);                     // weapon entry count = 2
        // entry 0: valid Sword class, unknown kind byte 3 (> kHide)
        buf.push_back(static_cast<std::byte>(WeaponClass::Sword));
        buf.push_back(std::byte{ 3 });
        PutStrV1(buf, "X");
        PutLE32(buf, 5);                     // formID
        // entry 1: a valid Bow hide (hide entries still carry an empty mod +
        // zero formID on the wire, same shape as the armor slot block)
        buf.push_back(static_cast<std::byte>(WeaponClass::Bow));
        buf.push_back(static_cast<std::byte>(SlotEntry::Kind::kHide));
        PutStrV1(buf, "");
        PutLE32(buf, 0);

        // v3 body block - see the note in the preceding case.
        PutLE32(buf, 0);                     // empty preset name
        buf.push_back(std::byte{ 0 });       // ORefitMode::kDefault
        PutLE32(buf, 0);                     // no per-hand overrides (v4)

        OutfitLibrary out;
        CHECK(Decode(buf, kCodecVersion, out));
        CHECK(out.Count() == 1);
        auto* o0 = out.At(0);
        CHECK(o0 != nullptr);
        if (o0) {
            CHECK(o0->WeaponEntryFor(WeaponClass::Sword).kind == SlotEntry::Kind::kPassthrough);
            CHECK(o0->WeaponEntryFor(WeaponClass::Bow).kind == SlotEntry::Kind::kHide);
        }
    }

    {  // v4 is a pure suffix extension of v1: for an armor-only library with
       // no body settings, the v4 bytes are exactly the v1 bytes with, after
       // EACH outfit's slot block, a zero weapon-count u32 (v2) followed by an
       // empty preset string (a zero u32 length) and a zero ORefit mode byte
       // (v3), followed by a zero hand-override count (v4). Pins the wire
       // layout itself, not just the round-trip behavior.
       //
       // This is what makes "old saves keep loading" a property of the FORMAT
       // rather than a hope: every version's bytes are a prefix of the next.
        OutfitLibrary lib;
        lib.Create("Alpha");
        lib.At(0)->SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        lib.Create("Beta");
        lib.At(1)->SetHide(kBitHair);
        lib.Activate(1);

        const auto v1 = EncodeV1(lib);

        // Alpha's body length, derived from a single-outfit library's v1
        // bytes (its 8-byte count/active header subtracted).
        OutfitLibrary onlyAlpha;
        onlyAlpha.Create("Alpha");
        onlyAlpha.At(0)->SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        const auto alphaEnd =
            static_cast<std::ptrdiff_t>(8 + (EncodeV1(onlyAlpha).size() - 8));

        const auto appendEmptyTail = [](std::vector<std::byte>& a_out) {
            PutLE32(a_out, 0);  // empty weapon block (v2)
            PutLE32(a_out, 0);  // empty preset string, length 0 (v3)
            a_out.push_back(std::byte{ 0 });  // ORefitMode::kDefault (v3)
            PutLE32(a_out, 0);  // no per-hand overrides (v4)
        };

        std::vector<std::byte> expected(v1.begin(), v1.begin() + alphaEnd);
        appendEmptyTail(expected);  // Alpha
        expected.insert(expected.end(), v1.begin() + alphaEnd, v1.end());
        appendEmptyTail(expected);  // Beta

        CHECK(Encode(lib) == expected);
    }

    {  // v3 round-trip: body settings survive Encode/Decode, and a v2-shaped
       // record (the same bytes with the 5-byte body tail removed) still
       // decodes - which is the actual guarantee a player upgrading from
       // 0.2.1 depends on.
        OutfitLibrary lib;
        const int a = lib.Create("Gown");
        lib.At(a)->obodyPreset = "Curvy Preset";
        lib.At(a)->orefit      = ORefitMode::kForceOff;
        lib.Activate(static_cast<std::size_t>(a));

        OutfitLibrary out;
        CHECK(Decode(Encode(lib), kCodecVersion, out));
        CHECK(out.At(0) != nullptr);
        if (out.At(0)) {
            CHECK(out.At(0)->obodyPreset == "Curvy Preset");
            CHECK(out.At(0)->orefit == ORefitMode::kForceOff);
        }

        // Same library with default body settings: remove the v4 count, then
        // the 5-byte body block to obtain exactly a v2 writer's bytes.
        OutfitLibrary plain;
        plain.Create("Gown");
        plain.Activate(0);
        auto v2 = Encode(plain);
        CHECK(v2.size() > 9);
        v2.resize(v2.size() - 4);  // drop v4 hand-override count
        v2.resize(v2.size() - 5);  // drop v3 body block

        OutfitLibrary fromV2;
        CHECK(Decode(v2, 2, fromV2));
        CHECK(fromV2.Count() == 1);
        CHECK(fromV2.At(0) != nullptr);
        if (fromV2.At(0)) {
            CHECK(fromV2.At(0)->name == "Gown");
            CHECK(fromV2.At(0)->obodyPreset.empty());
            CHECK(fromV2.At(0)->orefit == ORefitMode::kDefault);
        }
    }

    {  // NpcKey equality + NpcKeyHash usable in an unordered_map: same
       // {modName, localFormID} compares equal and hashes to the same slot;
       // either field differing makes distinct keys.
        NpcKey a{ "Skyrim.esm", 0x1A2B3 };
        NpcKey b{ "Skyrim.esm", 0x1A2B3 };
        NpcKey c{ "Skyrim.esm", 0x1A2B4 };
        NpcKey d{ "Dawnguard.esm", 0x1A2B3 };
        CHECK(a == b);
        CHECK(!(a == c));
        CHECK(!(a == d));

        std::unordered_map<NpcKey, int, NpcKeyHash> m;
        m[a] = 1;
        m[c] = 2;
        m[d] = 3;
        CHECK(m.size() == 3);
        CHECK(m.at(b) == 1);  // b == a: same key, not a fourth entry
        CHECK(m.at(c) == 2);
        CHECK(m.at(d) == 3);
    }

    {  // round-trip: NpcAssignments codec restores per-NPC inline libraries
       // exactly, keyed by {modName, localFormID}. Two NPCs, each with a
       // small library carrying a couple of slot entries.
        NpcAssignmentMap map;

        OutfitLibrary guardLib;
        const int gi = guardLib.Create("Guard Kit");
        guardLib.At(gi)->SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        guardLib.At(gi)->SetHide(kBitHair);
        guardLib.Activate(static_cast<std::size_t>(gi));
        map[NpcKey{ "Skyrim.esm", 0x13472 }] = NpcRecord{ std::move(guardLib) };
        map[NpcKey{ "Skyrim.esm", 0x13472 }].obodyBaseline = "CBBE Athletic";
        map[NpcKey{ "Skyrim.esm", 0x13472 }].obodyBaselineCaptured = true;

        OutfitLibrary lydiaLib;
        const int l1 = lydiaLib.Create("Travel Gear");
        lydiaLib.At(l1)->SetStyle(kBitFeet, StyleRefKey{ "Skyrim.esm", 0x1B3A3 });
        const int l2 = lydiaLib.Create("Formal");
        lydiaLib.At(l2)->SetHide(kBitBody);
        map[NpcKey{ "Skyrim.esm", 0xA2C94 }] = NpcRecord{ std::move(lydiaLib) };

        const auto bytes = EncodeNpcAssignments(map);
        CHECK(!bytes.empty());

        NpcAssignmentMap out;
        CHECK(DecodeNpcAssignments(bytes, kNpcRecordVersion, out));
        CHECK(out.size() == 2);

        const auto itGuard = out.find(NpcKey{ "Skyrim.esm", 0x13472 });
        CHECK(itGuard != out.end());
        if (itGuard != out.end()) {
            const auto& lib = itGuard->second.library;
            CHECK(lib.Count() == 1);
            CHECK(lib.ActiveIndex() == 0);
            CHECK(itGuard->second.obodyBaseline == "CBBE Athletic");
            CHECK(itGuard->second.obodyBaselineCaptured);
            auto* o0 = lib.At(0);
            CHECK(o0 != nullptr);
            if (o0) {
                CHECK(o0->name == "Guard Kit");
                CHECK(o0->EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
                CHECK(o0->EntryFor(kBitBody).style.modName == "Armors.esp");
                CHECK(o0->EntryFor(kBitBody).style.localFormID == 0x801);
                CHECK(o0->EntryFor(kBitHair).kind == SlotEntry::Kind::kHide);
            }
        }

        const auto itLydia = out.find(NpcKey{ "Skyrim.esm", 0xA2C94 });
        CHECK(itLydia != out.end());
        if (itLydia != out.end()) {
            const auto& lib = itLydia->second.library;
            CHECK(lib.Count() == 2);
            auto* o0 = lib.At(0);
            auto* o1 = lib.At(1);
            CHECK(o0 != nullptr);
            CHECK(o1 != nullptr);
            if (o0 && o1) {
                CHECK(o0->name == "Travel Gear");
                CHECK(o0->EntryFor(kBitFeet).style.localFormID == 0x1B3A3);
                CHECK(o1->name == "Formal");
                CHECK(o1->EntryFor(kBitBody).kind == SlotEntry::Kind::kHide);
            }
        }
    }

    {  // v1 NPC records remain readable; they predate follower OBody baselines.
        OutfitLibrary lib;
        lib.Create("Legacy");
        auto innerV2 = Encode(lib);
        innerV2.resize(innerV2.size() - 9);  // remove v4 hands + v3 body

        std::vector<std::byte> bytes;
        PutLE32(bytes, 1);
        PutStrV1(bytes, "Skyrim.esm");
        PutLE32(bytes, 0xA2C94);
        PutLE32(bytes, static_cast<std::uint32_t>(innerV2.size()));
        bytes.insert(bytes.end(), innerV2.begin(), innerV2.end());

        NpcAssignmentMap out;
        CHECK(DecodeNpcAssignments(bytes, 1, out));
        const auto it = out.find(NpcKey{ "Skyrim.esm", 0xA2C94 });
        CHECK(it != out.end());
        if (it != out.end()) {
            CHECK(!it->second.obodyBaselineCaptured);
            CHECK(it->second.obodyBaseline.empty());
        }
    }

    {  // v2 NPC records map to the v3 inner library and keep their appended
       // follower OBody baseline while v4 hand fields remain absent.
        OutfitLibrary lib;
        lib.Create("Legacy Body");
        lib.At(0)->obodyPreset = "Preset A";
        auto innerV3 = Encode(lib);
        innerV3.resize(innerV3.size() - 4);  // remove v4 hand count only

        std::vector<std::byte> bytes;
        PutLE32(bytes, 1);
        PutStrV1(bytes, "Skyrim.esm");
        PutLE32(bytes, 0xA2C94);
        PutLE32(bytes, static_cast<std::uint32_t>(innerV3.size()));
        bytes.insert(bytes.end(), innerV3.begin(), innerV3.end());
        PutLE32(bytes, 1);  // baseline captured
        PutStrV1(bytes, "Baseline");

        NpcAssignmentMap out;
        CHECK(DecodeNpcAssignments(bytes, 2, out));
        const auto it = out.find(NpcKey{ "Skyrim.esm", 0xA2C94 });
        CHECK(it != out.end());
        if (it != out.end()) {
            CHECK(it->second.library.At(0)->obodyPreset == "Preset A");
            CHECK(it->second.obodyBaselineCaptured);
            CHECK(it->second.obodyBaseline == "Baseline");
            CHECK(!it->second.library.At(0)->WeaponOverrideFor(
                WeaponClass::Sword, WeaponHand::Right));
        }
    }

    {  // a follower may persist with zero saved outfits: Equipped gear is the
       // immutable baseline, and the empty inline library survives the co-save
       // round-trip without silently recreating "Outfit 1".
        NpcAssignmentMap map;
        map[NpcKey{ "Skyrim.esm", 0xA2C94 }] = NpcRecord{};

        NpcAssignmentMap out;
        CHECK(DecodeNpcAssignments(EncodeNpcAssignments(map),
                                   kNpcRecordVersion, out));
        const auto it = out.find(NpcKey{ "Skyrim.esm", 0xA2C94 });
        CHECK(it != out.end());
        if (it != out.end()) {
            CHECK(it->second.library.Count() == 0);
            CHECK(it->second.library.ActiveIndex() == -1);
        }
    }

    {  // follower libraries use the same ten-slot cap and keep outfit ten
       // active across the outer NPC-assignment codec
        OutfitLibrary lib;
        for (std::size_t i = 0; i < kMaxOutfits; ++i) {
            CHECK(lib.Create("Follower outfit") == static_cast<int>(i));
        }
        lib.Activate(kMaxOutfits - 1);
        CHECK(lib.ActiveIndex() == 9);

        NpcAssignmentMap map;
        map[NpcKey{ "Skyrim.esm", 0xA2C94 }] =
            NpcRecord{ std::move(lib) };

        NpcAssignmentMap out;
        CHECK(DecodeNpcAssignments(EncodeNpcAssignments(map),
                                   kNpcRecordVersion, out));
        const auto it = out.find(NpcKey{ "Skyrim.esm", 0xA2C94 });
        CHECK(it != out.end());
        if (it != out.end()) {
            CHECK(it->second.library.Count() == 10);
            CHECK(it->second.library.ActiveIndex() == 9);
            CHECK(it->second.library.Create("overflow") == -1);
        }
    }

    {  // truncated outer entry: cutting bytes mid-entry is rejected, and the
       // out-map is left completely unmutated (same discipline the
       // whole-library codec's truncation test exercises).
        NpcAssignmentMap map;
        OutfitLibrary lib;
        lib.Create("Guard Kit");
        map[NpcKey{ "Skyrim.esm", 0x13472 }] = NpcRecord{ std::move(lib) };

        auto bytes = EncodeNpcAssignments(map);
        bytes.resize(bytes.size() - 2);  // cut into the entry's trailing bytes

        NpcAssignmentMap out;
        out[NpcKey{ "stale.esp", 1 }] = NpcRecord{};
        CHECK(!DecodeNpcAssignments(bytes, kNpcRecordVersion, out));
        CHECK(out.size() == 1);
        CHECK(out.find(NpcKey{ "stale.esp", 1 }) != out.end());
    }

    {  // per-entry tolerance: a 3-entry outer stream where the MIDDLE entry's
       // inner library is corrupt (hand-built to encode more than
       // kMaxOutfits outfits, so OS::Decode rejects it) - that ENTRY is
       // dropped, but the two good siblings still land and the call still
       // returns true. The outer stream stays aligned purely because the
       // corrupt entry's innerLen prefix told decode how far to skip.
        OutfitLibrary goodA;
        goodA.Create("Alpha");
        goodA.At(0)->SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
        const auto innerA = Encode(goodA);

        OutfitLibrary goodC;
        goodC.Create("Charlie");
        goodC.At(0)->SetHide(kBitFeet);
        const auto innerC = Encode(goodC);

        std::vector<std::byte> corruptInner;
        PutLE32(corruptInner, static_cast<std::uint32_t>(kMaxOutfits) + 1);  // count > kMaxOutfits
        PutLE32(corruptInner, 0);                                           // active

        std::vector<std::byte> buf;
        PutLE32(buf, 3);  // outer entry count

        PutStrV1(buf, "Skyrim.esm");
        PutLE32(buf, 0x111);
        PutLE32(buf, static_cast<std::uint32_t>(innerA.size()));
        buf.insert(buf.end(), innerA.begin(), innerA.end());
        PutLE32(buf, 0);  // v2 follower OBody baseline not captured
        PutStrV1(buf, "");

        PutStrV1(buf, "Broken.esp");
        PutLE32(buf, 0x222);
        PutLE32(buf, static_cast<std::uint32_t>(corruptInner.size()));
        buf.insert(buf.end(), corruptInner.begin(), corruptInner.end());
        PutLE32(buf, 0);
        PutStrV1(buf, "");

        PutStrV1(buf, "Skyrim.esm");
        PutLE32(buf, 0x333);
        PutLE32(buf, static_cast<std::uint32_t>(innerC.size()));
        buf.insert(buf.end(), innerC.begin(), innerC.end());
        PutLE32(buf, 0);
        PutStrV1(buf, "");

        NpcAssignmentMap out;
        CHECK(DecodeNpcAssignments(buf, kNpcRecordVersion, out));
        CHECK(out.size() == 2);
        CHECK(out.find(NpcKey{ "Broken.esp", 0x222 }) == out.end());

        const auto itA = out.find(NpcKey{ "Skyrim.esm", 0x111 });
        CHECK(itA != out.end());
        if (itA != out.end()) {
            CHECK(itA->second.library.Count() == 1);
            auto* o = itA->second.library.At(0);
            CHECK(o != nullptr);
            if (o) {
                CHECK(o->name == "Alpha");
            }
        }
        const auto itC = out.find(NpcKey{ "Skyrim.esm", 0x333 });
        CHECK(itC != out.end());
        if (itC != out.end()) {
            CHECK(itC->second.library.Count() == 1);
            auto* o = itC->second.library.At(0);
            CHECK(o != nullptr);
            if (o) {
                CHECK(o->name == "Charlie");
            }
        }
    }

    {  // verbatim-keep: an arbitrary/made-up modName round-trips unresolved -
       // resolving it to a real plugin in the current load order is not
       // decode's job, it's a later (engine-facing) stage's problem.
        NpcAssignmentMap map;
        OutfitLibrary lib;
        lib.Create("Whatever");
        map[NpcKey{ "NoSuchMod.esp", 0xDEAD }] = NpcRecord{ std::move(lib) };

        const auto bytes = EncodeNpcAssignments(map);
        NpcAssignmentMap out;
        CHECK(DecodeNpcAssignments(bytes, kNpcRecordVersion, out));
        CHECK(out.size() == 1);
        const auto it = out.find(NpcKey{ "NoSuchMod.esp", 0xDEAD });
        CHECK(it != out.end());
        if (it != out.end()) {
            CHECK(it->first.modName == "NoSuchMod.esp");
            CHECK(it->second.library.Count() == 1);
        }
    }

    if (g_failures == 0) {
        std::printf("PersistenceTests: all passed\n");
        return 0;
    }
    std::printf("PersistenceTests: %d failure(s)\n", g_failures);
    return 1;
}
