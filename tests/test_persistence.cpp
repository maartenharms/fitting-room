// Codec tests. No SKSE, no engine - Encode/Decode operate on plain bytes.
#include "DyeBlend.h"
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

// One v9 dye entry: the slot bit, a channel count, then that many channels of
// FOUR bytes each. Channels not named are written off, which is what SetDye
// leaves them as.
static void PutV9DyeEntry(
    std::vector<std::byte>& a_out, std::uint32_t a_bit,
    std::initializer_list<std::pair<std::size_t, std::array<std::uint8_t, 3>>> a_set) {
    PutLE32(a_out, a_bit);
    PutLE32(a_out, static_cast<std::uint32_t>(OS::kDyeChannelCount));
    for (std::size_t c = 0; c < OS::kDyeChannelCount; ++c) {
        const std::pair<std::size_t, std::array<std::uint8_t, 3>>* found = nullptr;
        for (const auto& s : a_set) {
            if (s.first == c) {
                found = &s;
            }
        }
        a_out.push_back(std::byte{ found ? std::uint8_t{ 1 } : std::uint8_t{ 0 } });
        for (std::size_t i = 0; i < 3; ++i) {
            a_out.push_back(std::byte{ found ? found->second[i] : std::uint8_t{ 0 } });
        }
    }
}

// A v9 LIBR payload holding ONE outfit with no styles, hand built.
//
// ⚠ v9 CANNOT BE FORGED BY TRUNCATING A CURRENT ENCODE ANY MORE, and that is
// the whole reason this exists. Every version through v12 APPENDED, so an older
// shape was always a PREFIX of a newer one and `Encode(lib)` minus a suffix was
// a genuine older payload. v13 writes a strength byte INSIDE each dye channel,
// so a DYED library's v9 bytes are not a prefix of its v13 bytes and no
// resize() reaches them. An undyed library is still forgeable that way, because
// PutChannel is never called for it; a dyed one has to be built by hand.
//
// Built only on the local primitives above, never on production detail::, for
// the same reason EncodeV1 is: an oracle that shares the code under test
// silently drifts with it.
static std::vector<std::byte> EncodeV9OneOutfit(const std::string& a_name,
                                                OS::HairMode           a_hair,
                                                std::uint32_t          a_dyeEntryCount,
                                                const std::vector<std::byte>& a_dyeEntries) {
    std::vector<std::byte> out;
    PutLE32(out, 1);  // outfit count
    PutLE32(out, 1);  // active = outfit 0
    PutStrV1(out, a_name);
    out.push_back(std::byte{ 0 });                          // favorite
    PutLE32(out, 0);                                        // armour slot entries
    PutLE32(out, 0);                                        // weapon entries (v2)
    PutLE32(out, 0);                                        // OBody preset name (v3)
    out.push_back(std::byte{ 0 });                          // ORefitMode::kDefault (v3)
    PutLE32(out, 0);                                        // per-hand overrides (v4)
    out.push_back(static_cast<std::byte>(a_hair));          // HairMode (v5)
    out.push_back(std::byte{ 0 });                          // hairTint disabled (v6)
    out.push_back(std::byte{ 0 });                          // hairTint.r (v6)
    out.push_back(std::byte{ 0 });                          // hairTint.g (v6)
    out.push_back(std::byte{ 0 });                          // hairTint.b (v6)
    PutLE32(out, 0);                                        // hairStyle modName (v7)
    PutLE32(out, 0);                                        // hairStyle localFormID (v7)
    PutLE32(out, a_dyeEntryCount);                          // dye entries (v8, v9)
    out.insert(out.end(), a_dyeEntries.begin(), a_dyeEntries.end());
    return out;  // and STOPS: no weapon dye (v10), no custom body (v11), no
                 // eyes or brows (v12), no strength (v13)
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
        // ⚠ DERIVED, NOT TYPED. These were 10 and 9 and went stale the moment
        // the cap moved, which is the whole point of filling the library with
        // a loop over kMaxOutfits: the test is about a FULL library round
        // tripping, not about the number ten.
        CHECK(lib.Count() == kMaxOutfits);
        lib.Activate(kMaxOutfits - 1);
        CHECK(lib.ActiveIndex() == static_cast<int>(kMaxOutfits) - 1);

        OutfitLibrary out;
        CHECK(Decode(Encode(lib), kCodecVersion, out));
        CHECK(out.Count() == kMaxOutfits);
        CHECK(out.ActiveIndex() == static_cast<int>(kMaxOutfits) - 1);
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

    {  // v12: eyes and brows round-trip, independently of each other and of the
       // hair style they sit beside on the wire.
       //
       // Independence is the case worth pinning rather than the round trip
       // itself. All three are a mod name plus a form id written back to back,
       // so a decoder that read them in the wrong order, or reused one field for
       // two, still decodes without error and still passes any test that sets
       // only one of them.
        OutfitLibrary lib;
        const int     idx = lib.Create("Face");
        lib.At(idx)->hairStyle = StyleRefKey{ "Hair.esp", 0x111 };
        lib.At(idx)->eyes      = StyleRefKey{ "Eyes.esp", 0x222 };
        lib.At(idx)->brows     = StyleRefKey{ "Brows.esp", 0x333 };

        OutfitLibrary out;
        CHECK(Decode(Encode(lib), kCodecVersion, out));
        const auto* restored = out.At(0);
        CHECK(restored != nullptr);
        if (restored) {
            CHECK(restored->hairStyle == (StyleRefKey{ "Hair.esp", 0x111 }));
            CHECK(restored->eyes == (StyleRefKey{ "Eyes.esp", 0x222 }));
            CHECK(restored->brows == (StyleRefKey{ "Brows.esp", 0x333 }));
        }
    }

    {  // v12: an outfit may name eyes and NOT brows. Empty is the
       // leave-their-own-alone value, so it has to survive as empty rather than
       // being filled in from its neighbour.
        OutfitLibrary lib;
        const int     idx = lib.Create("EyesOnly");
        lib.At(idx)->eyes = StyleRefKey{ "Eyes.esp", 0x444 };

        OutfitLibrary out;
        CHECK(Decode(Encode(lib), kCodecVersion, out));
        const auto* restored = out.At(0);
        CHECK(restored != nullptr);
        if (restored) {
            CHECK(restored->eyes == (StyleRefKey{ "Eyes.esp", 0x444 }));
            CHECK(restored->brows.Empty());
        }
    }

    {  // ⚠ THE ONE THAT PROTECTS EXISTING SAVES. A v11 record has no eye or brow
       // bytes at all, and must still decode, keeping everything it does carry
       // and leaving both new fields empty.
       //
       // This is the case Decode's own header warns about: the LIBR record is
       // every save's own outfit library, so a decoder that stopped accepting an
       // older version would silently empty the library of every player who
       // upgrades. Built by encoding at the current version and cutting the v12
       // suffix back off, which also pins that the suffix is exactly 16 bytes
       // and sits last.
        OutfitLibrary lib;
        const int     idx = lib.Create("Old");
        lib.At(idx)->hairStyle = StyleRefKey{ "Hair.esp", 0x555 };
        lib.Activate(idx);
        // ⚠ EYES AND BROWS LEFT EMPTY DELIBERATELY. The suffix is a length-
        // prefixed STRING plus a u32 per part, so a named reference does not
        // occupy a fixed 16 bytes and truncating by 16 would cut into the middle
        // of the custom-body ID instead. Empty is what makes the suffix exactly
        // 4 + 4 + 4 + 4, which is the shape being pinned.
        auto v11 = Encode(lib);
        CHECK(v11.size() > 24);
        v11.resize(v11.size() - 1);   // drop the v23 push-up byte
        v11.resize(v11.size() - 4);   // drop the v22 head-part dye count (zero)
        v11.resize(v11.size() - 4);   // drop the v21 invented-slot count (zero)
        v11.resize(v11.size() - 5);   // drop v20 second eye + v19 eye blend
        v11.resize(v11.size() - 8);   // drop v16 facial hair
        v11.resize(v11.size() - 8);   // drop v18 sclera + v17 eye tint
        v11.resize(v11.size() - 16);  // drop v12 eyes + brows

        OutfitLibrary out;
        CHECK(Decode(v11, 11, out));
        CHECK(out.Count() == 1);
        const auto* restored = out.At(0);
        CHECK(restored != nullptr);
        if (restored) {
            CHECK(restored->name == "Old");
            CHECK(restored->hairStyle == (StyleRefKey{ "Hair.esp", 0x555 }));
            CHECK(restored->eyes.Empty());
            CHECK(restored->brows.Empty());
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
        // 26 bytes of tail (body + hand-overrides + hair + tint + style + dye)
        // now follow the weapon block, so -3 lands inside the trailing v8 dye
        // count, cutting it short. Either way the point stands: a
        // truncated tail must be refused rather than half-read.
        bytes.resize(bytes.size() - 3);

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

        // v3 body block, v4 hand-override count, v5 hair byte, v6 tint block -
        // this buffer is decoded AS the current version, so it must carry every
        // block that version defines.
        PutLE32(buf, 0);                     // empty preset name
        buf.push_back(std::byte{ 0 });       // ORefitMode::kDefault
        PutLE32(buf, 0);                     // no per-hand overrides (v4)
        buf.push_back(std::byte{ 0 });       // HairMode::kAuto (v5)
        buf.push_back(std::byte{ 0 });       // hairTint disabled (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.r (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.g (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.b (v6)
        PutLE32(buf, 0);            // hairStyle: empty modName (v7)
        PutLE32(buf, 0);            // hairStyle: localFormID (v7)
        PutLE32(buf, 0);            // dye: no dyed slots (v8)
        PutLE32(buf, 0);            // weapon dye: none (v10)
        PutLE32(buf, 0);            // custom body ID: empty (v11)
        PutLE32(buf, 0);            // eyes: empty modName (v12)
        PutLE32(buf, 0);            // eyes: localFormID (v12)
        PutLE32(buf, 0);            // brows: empty modName (v12)
        PutLE32(buf, 0);            // brows: localFormID (v12)
        PutLE32(buf, 0);            // facial hair: empty modName (v16)
        PutLE32(buf, 0);            // facial hair: localFormID (v16)
        buf.push_back(std::byte{ 0 });  // eyeTint disabled (v17)
        buf.push_back(std::byte{ 0 });  // eyeTint.r (v17)
        buf.push_back(std::byte{ 0 });  // eyeTint.g (v17)
        buf.push_back(std::byte{ 0 });  // eyeTint.b (v17)
        buf.push_back(std::byte{ 0 });  // scleraTint disabled (v18)
        buf.push_back(std::byte{ 0 });  // scleraTint.r (v18)
        buf.push_back(std::byte{ 0 });  // scleraTint.g (v18)
        buf.push_back(std::byte{ 0 });  // scleraTint.b (v18)
        buf.push_back(std::byte{ 0 });  // eyeBlend: defer (v19)
        buf.push_back(std::byte{ 0 });  // eyeTint2 disabled (v20)
        buf.push_back(std::byte{ 0 });  // eyeTint2.r (v20)
        buf.push_back(std::byte{ 0 });  // eyeTint2.g (v20)
        buf.push_back(std::byte{ 0 });  // eyeTint2.b (v20)
        buf.push_back(std::byte{ 0 });  // invented head-part slots: none (v21)
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });  // head-part dyes: none (v22)
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });  // push-up: none (v23)

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

        // v3 body block, v4 hand-override count, v5 hair byte, v6 tint block -
        // see the note in the preceding case.
        PutLE32(buf, 0);                     // empty preset name
        buf.push_back(std::byte{ 0 });       // ORefitMode::kDefault
        PutLE32(buf, 0);                     // no per-hand overrides (v4)
        buf.push_back(std::byte{ 0 });       // HairMode::kAuto (v5)
        buf.push_back(std::byte{ 0 });       // hairTint disabled (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.r (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.g (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.b (v6)
        PutLE32(buf, 0);            // hairStyle: empty modName (v7)
        PutLE32(buf, 0);            // hairStyle: localFormID (v7)
        PutLE32(buf, 0);            // dye: no dyed slots (v8)
        PutLE32(buf, 0);            // weapon dye: none (v10)
        PutLE32(buf, 0);            // custom body ID: empty (v11)
        PutLE32(buf, 0);            // eyes: empty modName (v12)
        PutLE32(buf, 0);            // eyes: localFormID (v12)
        PutLE32(buf, 0);            // brows: empty modName (v12)
        PutLE32(buf, 0);            // brows: localFormID (v12)
        PutLE32(buf, 0);            // facial hair: empty modName (v16)
        PutLE32(buf, 0);            // facial hair: localFormID (v16)
        buf.push_back(std::byte{ 0 });  // eyeTint disabled (v17)
        buf.push_back(std::byte{ 0 });  // eyeTint.r (v17)
        buf.push_back(std::byte{ 0 });  // eyeTint.g (v17)
        buf.push_back(std::byte{ 0 });  // eyeTint.b (v17)
        buf.push_back(std::byte{ 0 });  // scleraTint disabled (v18)
        buf.push_back(std::byte{ 0 });  // scleraTint.r (v18)
        buf.push_back(std::byte{ 0 });  // scleraTint.g (v18)
        buf.push_back(std::byte{ 0 });  // scleraTint.b (v18)
        buf.push_back(std::byte{ 0 });  // eyeBlend: defer (v19)
        buf.push_back(std::byte{ 0 });  // eyeTint2 disabled (v20)
        buf.push_back(std::byte{ 0 });  // eyeTint2.r (v20)
        buf.push_back(std::byte{ 0 });  // eyeTint2.g (v20)
        buf.push_back(std::byte{ 0 });  // eyeTint2.b (v20)
        buf.push_back(std::byte{ 0 });  // invented head-part slots: none (v21)
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });  // head-part dyes: none (v22)
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });  // push-up: none (v23)

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

    {  // v6 is a pure suffix extension of v1: for an armor-only library with
       // no body settings, the v6 bytes are exactly the v1 bytes with, after
       // EACH outfit's slot block, a zero weapon-count u32 (v2) followed by an
       // empty preset string (a zero u32 length) and a zero ORefit mode byte
       // (v3), a zero hand-override count (v4), a zero hair byte (v5), and a
       // disabled 4-byte hair-tint block (v6).
       // Pins the wire layout itself, not just the round-trip behavior.
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
            a_out.push_back(std::byte{ 0 });  // HairMode::kAuto (v5)
            a_out.push_back(std::byte{ 0 });  // hairTint disabled (v6)
            a_out.push_back(std::byte{ 0 });  // hairTint.r (v6)
            a_out.push_back(std::byte{ 0 });  // hairTint.g (v6)
            a_out.push_back(std::byte{ 0 });  // hairTint.b (v6)
            PutLE32(a_out, 0);  // hairStyle: empty modName (v7)
            PutLE32(a_out, 0);  // hairStyle: localFormID (v7)
            PutLE32(a_out, 0);  // dye: no dyed slots (v8)
            PutLE32(a_out, 0);  // weapon dye: none (v10)
            PutLE32(a_out, 0);  // custom body ID: empty (v11)
            PutLE32(a_out, 0);  // eyes: empty modName (v12)
            PutLE32(a_out, 0);  // eyes: localFormID (v12)
            PutLE32(a_out, 0);  // brows: empty modName (v12)
            PutLE32(a_out, 0);  // brows: localFormID (v12)
            PutLE32(a_out, 0);  // facial hair: empty modName (v16)
            PutLE32(a_out, 0);  // facial hair: localFormID (v16)
            a_out.push_back(std::byte{ 0 });  // eyeTint disabled (v17)
            a_out.push_back(std::byte{ 0 });  // eyeTint.r (v17)
            a_out.push_back(std::byte{ 0 });  // eyeTint.g (v17)
            a_out.push_back(std::byte{ 0 });  // eyeTint.b (v17)
            a_out.push_back(std::byte{ 0 });  // scleraTint disabled (v18)
            a_out.push_back(std::byte{ 0 });  // scleraTint.r (v18)
            a_out.push_back(std::byte{ 0 });  // scleraTint.g (v18)
            a_out.push_back(std::byte{ 0 });  // scleraTint.b (v18)
            a_out.push_back(std::byte{ 0 });  // eyeBlend: defer (v19)
            a_out.push_back(std::byte{ 0 });  // eyeTint2 disabled (v20)
            a_out.push_back(std::byte{ 0 });  // eyeTint2.r (v20)
            a_out.push_back(std::byte{ 0 });  // eyeTint2.g (v20)
            a_out.push_back(std::byte{ 0 });  // eyeTint2.b (v20)
            a_out.push_back(std::byte{ 0 });  // invented head-part slots: none (v21)
            a_out.push_back(std::byte{ 0 });
            a_out.push_back(std::byte{ 0 });
            a_out.push_back(std::byte{ 0 });
            a_out.push_back(std::byte{ 0 });  // head-part dyes: none (v22)
            a_out.push_back(std::byte{ 0 });
            a_out.push_back(std::byte{ 0 });
            a_out.push_back(std::byte{ 0 });
            a_out.push_back(std::byte{ 0 });  // push-up: none (v23)
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
        lib.At(a)->customBodyPresetId = "0123456789abcdef";
        lib.At(a)->orefit      = ORefitMode::kForceOff;
        lib.Activate(static_cast<std::size_t>(a));

        OutfitLibrary out;
        CHECK(Decode(Encode(lib), kCodecVersion, out));
        CHECK(out.At(0) != nullptr);
        if (out.At(0)) {
            CHECK(out.At(0)->obodyPreset.empty());
            CHECK(out.At(0)->customBodyPresetId == "0123456789abcdef");
            CHECK(out.At(0)->orefit == ORefitMode::kForceOff);
        }

        // Same library with default body settings: remove the v6 hair-tint
        // block, the v5 hair byte, the v4 count, then the 5-byte body block to
        // obtain exactly a v2 writer's bytes.
        OutfitLibrary plain;
        plain.Create("Gown");
        plain.Activate(0);
        auto v2 = Encode(plain);
        CHECK(v2.size() > 10);
        v2.resize(v2.size() - 1);   // drop the v23 push-up byte
        v2.resize(v2.size() - 4);   // drop the v22 head-part dye count (zero)
        v2.resize(v2.size() - 4);   // drop the v21 invented-slot count (zero)
        v2.resize(v2.size() - 5);   // drop v20 second eye + v19 eye blend
        v2.resize(v2.size() - 8);   // drop v16 facial hair
        v2.resize(v2.size() - 8);   // drop v18 sclera + v17 eye tint
        v2.resize(v2.size() - 16);  // drop v12 eyes + brows (two refs)
        v2.resize(v2.size() - 4);  // drop v11 custom body ID (empty string)
        v2.resize(v2.size() - 4);  // drop v10 weapon dye block (empty: a zero count)
        v2.resize(v2.size() - 4);  // drop v8 dye block (empty: a zero count)
        v2.resize(v2.size() - 8);  // drop v7 hair-style block
        v2.resize(v2.size() - 4);  // drop v6 hair-tint block
        v2.resize(v2.size() - 1);  // drop v5 hair byte
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

    {  // v5 round trip carries the hair mode.
        //
        // NOTE: the plan called for a bare Outfit + lib.Add(o), but
        // OutfitLibrary has no Add() - every other test in this file builds
        // outfits via Create()+At(), so this follows that convention instead.
        OutfitLibrary lib;
        const int idx = lib.Create("Hooded");
        lib.At(idx)->hair = HairMode::kHide;

        const auto bytes = Encode(lib);
        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        CHECK(back.Count() == 1);
        // Guarded like every other At(0) use in this file: a null deref here
        // would crash the whole suite instead of printing one FAIL line.
        auto* o0 = back.At(0);
        CHECK(o0 != nullptr);
        if (o0) {
            CHECK(o0->hair == HairMode::kHide);
        }
    }

    {  // A v4 record still decodes, and yields kAuto rather than garbage.
        //
        // ⚠ ASSUMPTION, VERIFY IT BEFORE TRUSTING THIS TEST: for a
        // single-outfit library, Encode's tail is now [...hand-override
        // count][hair byte (v5)][hair-tint block, 4 bytes (v6)][hair-style
        // block, 8 bytes (v7)][empty dye block, a 4-byte zero count (v8)].
        // Dropping the LAST 17 bytes removes the v8+v7+v6+v5 tail together,
        // yielding a genuine v4 payload. READ Encode's tail before trusting this
        // arithmetic again next time a version is appended - this test's own
        // truncation count went stale exactly this way when v6 was added
        // (it used to be a single bytes.pop_back(), correct only while the hair
        // byte was the last thing Encode wrote). If anything is written after
        // the per-outfit loop (an active-outfit index, a trailing count,
        // anything), this truncation is WRONG and you must hand-build the v4
        // bytes instead. Decode is expected to reject trailing garbage, which is
        // exactly why a v6 payload cannot simply be decoded as v4 without
        // dropping the newer tail first.
        OutfitLibrary lib;
        const int idx = lib.Create("Legacy");
        lib.At(idx)->hair = HairMode::kShow;  // deliberately set; v4 bytes cannot carry it

        auto bytes = Encode(lib);
        OutfitLibrary back;
        // v11 custom ID(4) + v10 weapon dye(4) + v8 dye(4) + v7 style(8)
        // + v6 tint(4) + v5 hair(1)
        // to reach a genuine v4 shape.
        bytes.resize(bytes.size() - 71);  // +8: v17 eye tint, v18 sclera; +1: v19 eye blend; +4: v22 head dye; +1: v23 push-up
        CHECK(Decode(bytes, 4, back));
        CHECK(back.Count() == 1);
        auto* o0 = back.At(0);
        CHECK(o0 != nullptr);
        if (o0) {
            CHECK(o0->hair == HairMode::kAuto);
        }
    }

    {  // An out-of-range mode byte falls back to kAuto instead of being cast
       // into a value the render switch does not handle.
        OutfitLibrary lib;
        lib.Create("Corrupt");
        // The v5 hair byte, 70 from the end: the v6 tint block (4 bytes), the
        // v7 hair-style block (8), the v8 empty dye count (4), the v10 empty
        // weapon dye count (4), the v11 custom ID (4), the v12 eye and brow
        // refs (16), the v16 facial-hair ref (8), the v17 eye tint (4), the
        // v18 sclera (4), the v19 eye blend (1), the v20 second eye (4), the
        // v21 invented-slot count (4), the v22 head-part dye count (4) and the
        // v23 push-up byte (1) now
        // sit after it. Encode with kShow first so a pass cannot be the default
        // value sneaking through.
        //
        // ⚠ This offset has to move with every appended field. At -5 it landed
        // in the style block's length prefix instead, which made the whole
        // decode fail rather than exercising the mode-byte fallback at all.
        lib.At(0)->hair = HairMode::kShow;
        auto bytes = Encode(lib);
        bytes[bytes.size() - 71] = std::byte{ 0x7F };
        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        auto* o0 = back.At(0);
        CHECK(o0 != nullptr);
        if (o0) {
            CHECK(o0->hair == HairMode::kAuto);
        }
    }

    {  // v6 round trip: the hair tint survives encode/decode intact.
        OutfitLibrary lib;
        const int     idx = lib.Create("Tinted");
        CHECK(idx >= 0);
        lib.At(static_cast<std::size_t>(idx))->hairTint = HairTint{ true, 200, 40, 90 };

        const auto    bytes = Encode(lib);
        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            CHECK(o->hairTint.set);
            CHECK(o->hairTint.r == 200);
            CHECK(o->hairTint.g == 40);
            CHECK(o->hairTint.b == 90);
        }
    }

    {  // A disabled tint round trips as disabled.
        OutfitLibrary lib;
        const int     idx = lib.Create("Plain");
        CHECK(idx >= 0);
        const auto    bytes = Encode(lib);
        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        auto* o0 = back.At(0);
        CHECK(o0 != nullptr);
        if (o0) {
            CHECK(!o0->hairTint.set);
        }
    }

    {  // v20 round trip: the eye's SECOND colour survives, and the v19
       // boundary holds.
       //
       // ⚠ THE BOUNDARY IS THE POINT. A v19 payload is the current encode
       // minus its last four bytes, and it must decode with the second colour
       // CLEARED rather than black: cleared means the eye takes one colour,
       // which is what every outfit written before this meant, and a black
       // second colour would paint half of every dyed iris black on load.
        OutfitLibrary lib;
        const int     idx = lib.Create("TwoTone");
        CHECK(idx >= 0);
        lib.At(static_cast<std::size_t>(idx))->eyeTint  = HairTint{ true, 0, 176, 0 };
        lib.At(static_cast<std::size_t>(idx))->eyeTint2 = HairTint{ true, 200, 40, 90 };

        auto          bytes = Encode(lib);
        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            CHECK(o->eyeTint2.set);
            CHECK(o->eyeTint2.r == 200 && o->eyeTint2.g == 40 && o->eyeTint2.b == 90);
        }

        bytes.resize(bytes.size() - 13);  // drop v23 push-up + v22 head dye + v21 slot count + v20 second eye: exactly v19 bytes
        OutfitLibrary v19;
        CHECK(Decode(bytes, 19u, v19));
        const auto* p = v19.At(0);
        CHECK(p != nullptr);
        if (p) {
            CHECK(p->eyeTint.set);      // the v17 field still arrives
            CHECK(p->eyeTint.g == 176);
            CHECK(!p->eyeTint2.set);    // and the v20 field defaults to cleared
            CHECK(p->eyeTint2.r == 0 && p->eyeTint2.g == 0 && p->eyeTint2.b == 0);
        }
    }

    {  // The second colour is part of the EYE's own diff term rather than a
       // ninth refresh dimension, which is what keeps all six lists still.
        Outfit a;
        Outfit b   = a;
        b.eyeTint  = HairTint{ true, 10, 20, 30 };
        Outfit c   = b;
        c.eyeTint2 = HairTint{ true, 200, 40, 90 };
        CHECK(EyeTintDiffers(b, c));
        CHECK(!EyeTintDiffers(c, c));
        // And it is not a slot edit, the same as the tints it sits beside.
        CHECK(ChangedSlotCount(b, c) == 0);
    }

    {  // v19 round trip: the eye BLEND survives, and the v18 boundary holds.
       //
       // ⚠ THE BOUNDARY IS THE POINT, NOT THE ROUND TRIP. A v18 payload is the
       // current encode minus its last byte, and it must decode with the blend
       // at zero - which DEFERS, so the eye renders through the recolour every
       // confirmed iris already renders through. A decode that landed a real
       // blend on it would restyle every eye in every save on load.
        OutfitLibrary lib;
        const int     idx = lib.Create("EyeBlend");
        CHECK(idx >= 0);
        lib.At(static_cast<std::size_t>(idx))->eyeTint  = HairTint{ true, 12, 200, 90 };
        lib.At(static_cast<std::size_t>(idx))->eyeBlend =
            static_cast<std::uint8_t>(OS::DyeBlend::Choice::kScreen);

        auto          bytes = Encode(lib);
        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            CHECK(OS::DyeBlend::ChoiceFromByte(o->eyeBlend) == OS::DyeBlend::Choice::kScreen);
        }

        bytes.resize(bytes.size() - 14);  // drop v23 push-up + v22 head dye + v21 count + v20 second eye + v19 blend: exactly v18 bytes
        OutfitLibrary v18;
        CHECK(Decode(bytes, 18u, v18));
        const auto* p = v18.At(0);
        CHECK(p != nullptr);
        if (p) {
            CHECK(p->eyeTint.set);   // the v17 field still arrives
            CHECK(p->eyeTint.g == 200);
            CHECK(OS::DyeBlend::ChoiceFromByte(p->eyeBlend) == OS::DyeBlend::Choice::kDefault);
        }
    }

    {  // v19 round trip, the CHANNEL half: a per-dye blend survives, and the
       // v18 boundary holds inside the channel rather than at the record's end.
       //
       // ⚠ FORGED THROUGH GetChannel DIRECTLY, the way v15's own boundary is.
       // The byte is written INSIDE every channel, so a dyed library's v18
       // bytes are not a prefix of its v19 bytes and this cannot be forged by
       // truncating a whole record.
        DyeChannel wide{ true, 10, 20, 30 };
        wide.blend = static_cast<std::uint8_t>(OS::DyeBlend::Choice::kMultiply);

        std::vector<std::byte> buf;
        detail::PutChannel(buf, wide);

        detail::Reader r19{ buf };
        DyeChannel     got19;
        CHECK(detail::GetChannel(r19, kCodecVersion, got19));
        CHECK(OS::DyeBlend::ChoiceFromByte(got19.blend) == OS::DyeBlend::Choice::kMultiply);

        // The same bytes minus the trailing blend ARE a v18 channel.
        buf.resize(buf.size() - 1);
        detail::Reader r18{ buf };
        DyeChannel     got18;
        CHECK(detail::GetChannel(r18, 18u, got18));
        CHECK(got18.r == 10);
        CHECK(OS::DyeBlend::ChoiceFromByte(got18.blend) == OS::DyeBlend::Choice::kDefault);
    }

    {  // v25 round trip, the CHANNEL half: the per-piece "Metal starts" cut
       // survives, and the v24 boundary holds inside the channel.
       //
       // ⚠ FORGED THROUGH GetChannel DIRECTLY, the way v19's is: the byte is
       // written INSIDE every channel, so a dyed library's v24 bytes are not a
       // prefix of its v25 bytes and no resize() of a whole record reaches it.
        DyeChannel tuned{ true, 10, 20, 30 };
        tuned.mode = 5;
        tuned.cut  = 51;

        std::vector<std::byte> buf;
        detail::PutChannel(buf, tuned);

        detail::Reader r25{ buf };
        DyeChannel     got25;
        CHECK(detail::GetChannel(r25, kCodecVersion, got25));
        CHECK(got25.cut == 51);
        CHECK(got25 == tuned);

        // The same bytes minus the trailing cut ARE a v24 channel, and it
        // decodes to 128: halfway, the one picture every v24 build drew.
        buf.resize(buf.size() - 1);
        detail::Reader r24{ buf };
        DyeChannel     got24;
        CHECK(detail::GetChannel(r24, 24u, got24));
        CHECK(got24.r == 10 && got24.mode == 5);
        CHECK(got24.cut == 128);
    }

    {  // v18 round trip: both eye colours survive encode/decode, and the v17
       // boundary holds: a v17 payload is the current encode minus its last
       // five bytes, and it decodes with the sclera left alone.
        OutfitLibrary lib;
        const int     idx = lib.Create("EyePair");
        CHECK(idx >= 0);
        lib.At(static_cast<std::size_t>(idx))->eyeTint    = HairTint{ true, 0, 176, 0 };
        lib.At(static_cast<std::size_t>(idx))->scleraTint = HairTint{ true, 200, 40, 90 };

        auto          bytes = Encode(lib);
        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            CHECK(o->eyeTint.set);
            CHECK(o->eyeTint.g == 176);
            CHECK(o->scleraTint.set);
            CHECK(o->scleraTint.r == 200);
            CHECK(o->scleraTint.b == 90);
        }

        bytes.resize(bytes.size() - 18);  // drop v23 push-up + v22 head dye + v21 count + v20 + v19 + v18 sclera: exactly v17 bytes
        OutfitLibrary v17;
        CHECK(Decode(bytes, 17u, v17));
        const auto* p = v17.At(0);
        CHECK(p != nullptr);
        if (p) {
            CHECK(p->eyeTint.set);       // the v17 field still arrives
            CHECK(!p->scleraTint.set);   // and the v18 field defaults off
        }
    }

    {  // ⚠ THE LOAD-BEARING TEST. v5 bytes must still decode under v6 and land
       // on a disabled tint. If this ever fails, every save written by 0.4.0
       // loses its outfit library.
        OutfitLibrary lib;
        const int     idx = lib.Create("FromV5");
        CHECK(idx >= 0);
        lib.At(static_cast<std::size_t>(idx))->hair = HairMode::kHide;

        // Chop the trailing weapon dye(4) + dye(4) + style(8) + tint(4) to
        // forge a v5 payload.
        auto bytes = Encode(lib);
        CHECK(bytes.size() > 20);
        bytes.resize(bytes.size() - 70);  // +8: v17 eye tint, v18 sclera; +1: v19 eye blend; +4: v22 head dye

        OutfitLibrary back;
        CHECK(Decode(bytes, 5, back));
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            CHECK(o->hair == HairMode::kHide);   // the v5 field still arrives
            CHECK(!o->hairTint.set);             // and the v6 field defaults off
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
        // remove v10 weapon dye + v8 dye + v7 style + v6 tint + v5 hair
        // + v4 hands + v3 body
        innerV2.resize(innerV2.size() - 80);  // +8: v17 eye tint, v18 sclera; +1: v19 eye blend; +4: v22 head dye

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
        // remove v10 weapon dye + v8 dye + v7 style + v6 tint + v5 hair
        // + v4 hand count
        innerV3.resize(innerV3.size() - 75);  // +8: v17 eye tint, v18 sclera; +1: v19 eye blend; +4: v22 head dye

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
        CHECK(lib.ActiveIndex() == static_cast<int>(kMaxOutfits) - 1);

        NpcAssignmentMap map;
        map[NpcKey{ "Skyrim.esm", 0xA2C94 }] =
            NpcRecord{ std::move(lib) };

        NpcAssignmentMap out;
        CHECK(DecodeNpcAssignments(EncodeNpcAssignments(map),
                                   kNpcRecordVersion, out));
        const auto it = out.find(NpcKey{ "Skyrim.esm", 0xA2C94 });
        CHECK(it != out.end());
        if (it != out.end()) {
            CHECK(it->second.library.Count() == kMaxOutfits);
            CHECK(it->second.library.ActiveIndex() ==
                  static_cast<int>(kMaxOutfits) - 1);
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

        // Every trailing per-entry field the CURRENT outer version defines: the
        // v2 OBody pair and the v6 hair triple. 20 bytes with empty strings
        // (4 + 4 + 4 + 4 + 4), and the drop path must consume all of them.
        const auto appendEntryTail = [](std::vector<std::byte>& a_out) {
            PutLE32(a_out, 0);   // v2 obodyBaselineCaptured = false
            PutStrV1(a_out, "");  // v2 obodyBaseline
            PutLE32(a_out, 0);   // v6 hairBaselineCaptured = false
            PutStrV1(a_out, "");  // v6 hairBaselineMod
            PutLE32(a_out, 0);   // v6 hairBaselineLocalID
        };

        std::vector<std::byte> buf;
        PutLE32(buf, 3);  // outer entry count

        PutStrV1(buf, "Skyrim.esm");
        PutLE32(buf, 0x111);
        PutLE32(buf, static_cast<std::uint32_t>(innerA.size()));
        buf.insert(buf.end(), innerA.begin(), innerA.end());
        appendEntryTail(buf);

        PutStrV1(buf, "Broken.esp");
        PutLE32(buf, 0x222);
        PutLE32(buf, static_cast<std::uint32_t>(corruptInner.size()));
        buf.insert(buf.end(), corruptInner.begin(), corruptInner.end());
        appendEntryTail(buf);

        PutStrV1(buf, "Skyrim.esm");
        PutLE32(buf, 0x333);
        PutLE32(buf, static_cast<std::uint32_t>(innerC.size()));
        buf.insert(buf.end(), innerC.begin(), innerC.end());
        appendEntryTail(buf);

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

    {  // NPCO v4 round trip preserves a follower's hair mode.
        NpcAssignmentMap map;
        NpcRecord        rec;
        CHECK(rec.library.Create("Follower look") == 0);
        if (auto* o = rec.library.At(0)) {
            o->hair = HairMode::kShow;
        }
        map[NpcKey{ "Follower.esp", 0x800 }] = rec;

        const auto       bytes = EncodeNpcAssignments(map);
        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(bytes, kNpcRecordVersion, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Follower.esp", 0x800 });
        CHECK(it != back.end());
        if (it != back.end()) {
            CHECK(it->second.library.Count() == 1);
            const auto* got = it->second.library.At(0);
            CHECK(got != nullptr);
            if (got) {
                CHECK(got->hair == HairMode::kShow);
            }
        }
    }

    {  // NPCO v6 round trip: a follower's captured pre-Fitting-Room hair colour
       // survives the outer codec as a {captured, plugin, local form ID} triple.
       // Deliberately paired with a captured OBody baseline, since the two sit
       // adjacent on the wire and a field-order slip would swap them.
        NpcAssignmentMap map;
        NpcRecord        rec;
        CHECK(rec.library.Create("Follower look") == 0);
        rec.obodyBaseline         = "CBBE Athletic";
        rec.obodyBaselineCaptured = true;
        rec.hairBaselineMod       = "Skyrim.esm";
        rec.hairBaselineLocalID   = 0x0EB60Fu;
        rec.hairBaselineCaptured  = true;
        map[NpcKey{ "Follower.esp", 0x800 }] = rec;

        const auto       bytes = EncodeNpcAssignments(map);
        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(bytes, kNpcRecordVersion, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Follower.esp", 0x800 });
        CHECK(it != back.end());
        if (it != back.end()) {
            CHECK(it->second.hairBaselineCaptured);
            CHECK(it->second.hairBaselineMod == "Skyrim.esm");
            CHECK(it->second.hairBaselineLocalID == 0x0EB60Fu);
            // The neighbouring pair must not have been consumed as ours.
            CHECK(it->second.obodyBaselineCaptured);
            CHECK(it->second.obodyBaseline == "CBBE Athletic");
        }
    }

    {  // An UNcaptured hair baseline round trips as uncaptured. The flag is the
       // only thing that distinguishes it from a capture of nothing, which is
       // why it is on the wire at all rather than inferred from an empty name.
        NpcAssignmentMap map;
        NpcRecord        rec;
        CHECK(rec.library.Create("Untouched") == 0);
        map[NpcKey{ "Follower.esp", 0x801 }] = rec;

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(EncodeNpcAssignments(map), kNpcRecordVersion, back));
        const auto it = back.find(NpcKey{ "Follower.esp", 0x801 });
        CHECK(it != back.end());
        if (it != back.end()) {
            CHECK(!it->second.hairBaselineCaptured);
            CHECK(it->second.hairBaselineMod.empty());
            CHECK(it->second.hairBaselineLocalID == 0u);
        }
    }

    {  // ⚠ THE REGRESSION THAT NEARLY SHIPPED. Every historical innerVersion
       // branch must be a FROZEN literal, so that bumping LIBR does not
       // re-point an already-saved NPCO record at a wire shape it was never
       // written in. A v3 record holds LIBR v4 bytes forever.
       //
       // (The current-version pin that used to sit here moved into the mapping
       // block below, which asserts the WHOLE mapping rather than one constant.
       // It was guarding the wrong constant anyway: this block's truncation
       // arithmetic tracks kCodecVersion, not kNpcRecordVersion.)
        NpcAssignmentMap map;
        NpcRecord        rec;
        CHECK(rec.library.Create("Saved under v3") == 0);
        map[NpcKey{ "Old.esp", 0x123 }] = rec;

        // Current encode writes LIBR v11 inner bytes. Drop the trailing custom
        // body ID (4), weapon
        // dye count (4), dye count (4), style block (8), tint block (4) and
        // hair byte (1) to make genuine v4-shaped inner bytes, then wrap them
        // the way a v3 record on disk would look.
        auto inner = Encode(rec.library);
        inner.resize(inner.size() - 71);  // +8: v17 eye tint, v18 sclera; +1: v19 eye blend; +4: v22 head dye

        std::vector<std::byte> v3;
        detail::PutU32(v3, 1u);
        detail::PutStr(v3, "Old.esp");
        detail::PutU32(v3, 0x123u);
        detail::PutU32(v3, static_cast<std::uint32_t>(inner.size()));
        v3.insert(v3.end(), inner.begin(), inner.end());
        detail::PutU32(v3, 0u);   // obodyBaselineCaptured
        detail::PutStr(v3, "");   // obodyBaseline

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(v3, 3, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Old.esp", 0x123 });
        CHECK(it != back.end());
        if (it != back.end()) {
            // The library SURVIVED. Before the fix it was silently dropped and
            // the load still reported success with a smaller count.
            CHECK(it->second.library.Count() == 1);
        }
    }

    {  // ⚠ THE 1e49d57 REGRESSION TEST, extended to the LIBR v6 bump. An NPCO
       // record written at v4 embeds LIBR v5. Decoding it after LIBR moved to v6
       // must still read the inner payload as v5, or the follower's library is
       // dropped and the load reports success with a smaller count.
       //
       // Pinned by CONSTRUCTION, not by literal: assert the whole historical
       // mapping so a future bump cannot quietly re-point one of these.
        CHECK(NpcoInnerLibrVersion(1) == 2u);
        CHECK(NpcoInnerLibrVersion(2) == 3u);
        CHECK(NpcoInnerLibrVersion(3) == 4u);
        CHECK(NpcoInnerLibrVersion(4) == 5u);   // frozen the moment v5 shipped
        CHECK(NpcoInnerLibrVersion(5) == 6u);   // frozen the moment v6 shipped
        CHECK(NpcoInnerLibrVersion(6) == 6u);   // frozen the moment v7 shipped
        CHECK(NpcoInnerLibrVersion(7) == 7u);   // frozen the moment v8 shipped
        // The branch THIS bump freezes. Without it the default arm answers 9
        // for NPCO v8 records that actually embed LIBR v8 bytes, and every
        // follower library written by a v8 build decodes at the wrong version,
        // fails on the missing per-slot channel count, and is dropped.
        CHECK(NpcoInnerLibrVersion(8) == 8u);   // frozen the moment v9 shipped
        // The branch THIS bump freezes. Without it the default arm answers 10
        // for NPCO v9 records that actually embed LIBR v9 bytes, so every
        // follower library written by a v9 build decodes at the wrong version,
        // runs off the end looking for a weapon dye block that was never
        // written, and is dropped.
        CHECK(NpcoInnerLibrVersion(9) == 9u);   // frozen the moment v10 shipped
        CHECK(NpcoInnerLibrVersion(10) == 10u); // frozen the moment v11 shipped
        // The branch THIS bump freezes. Without it the default arm answers 12
        // for NPCO v11 records that actually embed LIBR v11 bytes, so every
        // follower library written by a v11 build decodes at the wrong version,
        // runs off the end looking for eye and brow references that were never
        // written, and is dropped.
        CHECK(NpcoInnerLibrVersion(11) == 11u); // frozen the moment v12 shipped
        // The branch THIS bump freezes. Without it the default arm answers 13
        // for NPCO v12 records that actually embed LIBR v12 bytes, so every
        // follower library written by a v12 build decodes at the wrong version,
        // runs off the end looking for a per-channel strength byte that was
        // never written, and is dropped.
        CHECK(NpcoInnerLibrVersion(12) == 12u); // frozen the moment v13 shipped
        // The branch THIS bump freezes. Without it the default arm answers 14
        // for NPCO v13 records that actually embed LIBR v13 bytes, so every
        // follower library written by a v13 build decodes at the wrong version,
        // runs off the end of each dye channel looking for a mode and a second
        // stop that were never written, and is dropped.
        CHECK(NpcoInnerLibrVersion(13) == 13u); // frozen the moment v14 shipped
        // The branch THIS bump freezes. v14 held the top spot for one day and
        // never left the dev machine, but that machine's own saves carry it,
        // and the default arm would answer 15 for records that stop before the
        // flake and finish bytes.
        CHECK(NpcoInnerLibrVersion(14) == 14u); // frozen the moment v15 shipped
        // The branch THIS bump freezes (OS-196, facial hair). v15 is what the
        // 2026-08-09 texture-swap builds wrote and the dev machine's saves
        // carry it; the default arm would answer 16 for records that stop
        // before the facial-hair pair, so every follower library written by a
        // v15 build would run off the end of its last outfit and be dropped.
        CHECK(NpcoInnerLibrVersion(15) == 15u); // frozen the moment v16 shipped
        // ⚠ FROZEN TO 17, NOT 16, AND IT IS A TRIAGE. The LIBR v17 bump forgot
        // the outer constant entirely, so NPCO v16 records embed LIBR v16 from
        // the facial-hair build and LIBR v17 from the eye-colour build, and no
        // single freeze can serve both. 17 is the meaning of the saves being
        // played; the older build's inner libraries read as truncated and are
        // skipped, which is what has silently happened since the constant
        // moved. NpcAssignments.h carries the whole story; NPCO v17 does not
        // exist for the same reason.
        CHECK(NpcoInnerLibrVersion(16) == 17u);
        // The branch THIS bump freezes. NPCO v18 held the top spot for one day
        // and the dev machine's 2026-08-13 saves carry it; without the freeze
        // the default arm answers 19 for records that stop before the blend
        // byte, so every channel of every follower library written by a v18
        // build runs off its own end and the whole record is dropped.
        CHECK(NpcoInnerLibrVersion(18) == 18u);
        // The branch THIS bump freezes. NPCO v19 held the top spot for part of
        // one afternoon; without the freeze the default arm answers 20 for
        // records that stop before the second eye colour, and every follower
        // library written by a v19 build runs off the end of its last outfit
        // and is dropped.
        CHECK(NpcoInnerLibrVersion(19) == 19u);
        CHECK(NpcoInnerLibrVersion(20) == 20u);
        // The branch THIS bump freezes. NPCO v21 is what the 2026-08-16 and
        // 2026-08-17 appearance-stint builds write, and those saves are on the
        // dev rig. Without the freeze the default arm answers 22 for records
        // that stop before the head-part dyes, so every follower library in them
        // runs off the end of its last outfit and is dropped.
        CHECK(NpcoInnerLibrVersion(21) == 21u);
        // The branch THIS bump freezes. NPCO v22 is what the 2026-08-26 face and
        // dye stint builds write, and those saves are on the dev rig. Without the
        // freeze the default arm answers 23 for records that stop before the
        // push-up byte, so every follower library written by a v22 build runs off
        // the end of its last outfit and is dropped.
        CHECK(NpcoInnerLibrVersion(22) == 22u);
        // The branch THIS bump freezes. NPCO v23 is what the 2026-08-27 and
        // 2026-08-28 builds write, including the 1.1.1 zips the playtesters
        // already have. Without the freeze the default arm answers 24 for
        // records that stop before the armour dye block grew its garment key,
        // so every follower library written by a v23 build runs off the end of
        // its last outfit and is dropped.
        CHECK(NpcoInnerLibrVersion(23) == 23u);
        // The branch THIS bump freezes. NPCO v24 is what 1.1.8 writes, the
        // version LIVE ON NEXUS, so every player's save carries it. Without
        // the freeze the default arm answers 25 for records whose dye channels
        // stop before the cut byte, so every dyed follower library in them
        // runs off the end of a channel and is dropped.
        CHECK(NpcoInnerLibrVersion(24) == 24u);
        // ⚠ This trailing pair proves NOTHING about the freeze: it passes for
        // any two constants, by construction of the default arm. It is here to
        // catch a bump that forgets to move BOTH, not to stand in for the
        // frozen literals above it.
        CHECK(NpcoInnerLibrVersion(kNpcRecordVersion) == kCodecVersion);

        // And the current version is genuinely new, so the line above is not
        // accidentally comparing a version against itself.
        CHECK(kNpcRecordVersion == 25u);
        CHECK(kCodecVersion == 25u);
    }

    {  // ⚠ THE OUTGOING NPCO v22, END TO END, step 3 of the four the bump note
       // lists. A v22 outer embeds LIBR v22 bytes, so the forge is the current
       // encode with only the v23 push-up byte dropped.
        OutfitLibrary lib;
        CHECK(lib.Create("StillReadableAtNpco22") >= 0);
        auto bytes = Encode(lib);
        bytes.resize(bytes.size() - 1);  // drop the v23 push-up byte: LIBR v22

        OutfitLibrary back;
        CHECK(Decode(bytes, 22u, back));  // LIBR v22, the outgoing inner version
        CHECK(back.Count() == 1);

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(bytes.size()));
        outer.insert(outer.end(), bytes.begin(), bytes.end());
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);

        NpcAssignmentMap map;
        CHECK(DecodeNpcAssignments(outer, 22u, map));  // NPCO v22, outgoing outer
        CHECK(map.size() == 1);
    }

    {  // ⚠ THE OUTGOING NPCO v23, END TO END, step 3 of the four the bump note
       // lists.
       //
       // ⚠ ZERO BYTES ARE DROPPED, AND THAT IS NOT AN OVERSIGHT. Every other
       // forge here trims an appended field, because every other bump appended
       // one. v24 did not: it widened an ENTRY INSIDE the armour dye block. A
       // library that dyes nothing writes a dye count of zero and no entries at
       // all, so its v24 bytes ARE its v23 bytes, exactly. The migration itself
       // is the test below this one, which is where a dye actually appears.
        OutfitLibrary lib;
        CHECK(lib.Create("StillReadableAtNpco23") >= 0);
        auto bytes = Encode(lib);
        CHECK(!lib.At(0)->AnyDye());  // the premise the byte identity rests on

        OutfitLibrary back;
        CHECK(Decode(bytes, 23u, back));  // LIBR v23, the outgoing inner version
        CHECK(back.Count() == 1);

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(bytes.size()));
        outer.insert(outer.end(), bytes.begin(), bytes.end());
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);

        NpcAssignmentMap map;
        CHECK(DecodeNpcAssignments(outer, 23u, map));  // NPCO v23, outgoing outer
        CHECK(map.size() == 1);
    }

    {  // ⚠⚠ THE OUTGOING NPCO v24, END TO END, step 3 of the four the bump note
       // lists, and THE LAYOUT 1.1.8 WRITES: v24 is the version live on Nexus,
       // so every player's save carries it. v25 appends one byte INSIDE every
       // dye channel (the per-piece "Metal starts" cut), so a dyed v24 record
       // is not a prefix of its v25 encode and Encode() cannot forge it. Hand
       // built on the v23 forge above plus the garment key v24 put in the
       // entry, with the twenty-four byte channel v24 wrote.
        const auto putChannel24 = [](std::vector<std::byte>& a_out, bool a_set,
                                     std::uint8_t a_r, std::uint8_t a_g,
                                     std::uint8_t a_b, std::uint8_t a_mode) {
            a_out.push_back(std::byte{ a_set ? std::uint8_t{ 1 } : std::uint8_t{ 0 } });
            a_out.push_back(static_cast<std::byte>(a_r));
            a_out.push_back(static_cast<std::byte>(a_g));
            a_out.push_back(static_cast<std::byte>(a_b));
            a_out.push_back(std::byte{ 255 });                // strength (v13)
            a_out.push_back(static_cast<std::byte>(a_mode));  // mode (v14)
            for (int i = 0; i < 5; ++i) {
                a_out.push_back(std::byte{ 0 });  // second*, flake (v14/v15)
            }
            for (int fb = 0; fb < 2; ++fb) {      // palette then player (v15)
                for (int i = 0; i < 5; ++i) {
                    a_out.push_back(std::byte{ 0 });
                }
                a_out.push_back(std::byte{ 128 });  // gloss
            }
            a_out.push_back(std::byte{ 0 });      // blend: defer (v19)
            // and STOPS: no cut (v25)
        };

        std::vector<std::byte> buf;
        PutLE32(buf, 1);                     // outfit count
        PutLE32(buf, 1);                     // active
        PutStrV1(buf, "Worn24");
        buf.push_back(std::byte{ 0 });       // favorite
        PutLE32(buf, 1);                     // armor slot count = 1
        PutLE32(buf, 2);                     // bit 2
        buf.push_back(static_cast<std::byte>(SlotEntry::Kind::kStyle));
        PutStrV1(buf, "Armors.esp");
        PutLE32(buf, 0x800);
        PutLE32(buf, 0);                     // weapon entry count
        PutLE32(buf, 0);                     // empty preset name (v3)
        buf.push_back(std::byte{ 0 });       // ORefitMode::kDefault (v3)
        PutLE32(buf, 0);                     // no per-hand overrides (v4)
        buf.push_back(std::byte{ 0 });       // HairMode::kAuto (v5)
        for (int i = 0; i < 4; ++i) {
            buf.push_back(std::byte{ 0 });   // hairTint (v6)
        }
        PutLE32(buf, 0);                     // hairStyle modName (v7)
        PutLE32(buf, 0);                     // hairStyle localFormID (v7)

        PutLE32(buf, 1);                     // ONE dye entry
        PutLE32(buf, 2);                     // on bit 2
        PutStrV1(buf, "Armors.esp");         // and its garment: this is v24
        PutLE32(buf, 0x800);
        PutLE32(buf, static_cast<std::uint32_t>(kDyeChannelCount));
        putChannel24(buf, true, 10, 20, 30, 5);  // a twotone dye, no cut byte
        for (std::size_t c = 1; c < kDyeChannelCount; ++c) {
            putChannel24(buf, false, 0, 0, 0, 0);
        }

        PutLE32(buf, 0);                     // weapon dyes (v10)
        PutStrV1(buf, "");                   // body preset id (v11)
        PutStrV1(buf, "");                   // eyes: modName (v12)
        PutLE32(buf, 0);                     // eyes: localFormID (v12)
        PutStrV1(buf, "");                   // brows: modName (v16)
        PutLE32(buf, 0);                     // brows: localFormID (v16)
        PutStrV1(buf, "");                   // facial hair: modName (v16)
        PutLE32(buf, 0);                     // facial hair: localFormID (v16)
        for (int i = 0; i < 4; ++i) {
            buf.push_back(std::byte{ 0 });   // eye tint (v17)
        }
        for (int i = 0; i < 4; ++i) {
            buf.push_back(std::byte{ 0 });   // sclera tint (v18)
        }
        buf.push_back(std::byte{ 0 });       // eye blend (v19)
        for (int i = 0; i < 4; ++i) {
            buf.push_back(std::byte{ 0 });   // eye tint 2 (v20)
        }
        PutLE32(buf, 0);                     // invented head-part slots (v21)
        PutLE32(buf, 0);                     // head-part dyes (v22)
        buf.push_back(std::byte{ 0 });       // push-up (v23)

        OutfitLibrary back;
        CHECK(Decode(buf, 24u, back));       // LIBR v24, the outgoing inner version
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            // The dye lands on its garment with the cut at 128: halfway, the
            // one picture every v24 build drew.
            CHECK(o->DyeFor(2).channels[0].set);
            CHECK(o->DyeFor(2).channels[0].r == 10);
            CHECK(o->DyeFor(2).channels[0].mode == 5);
            CHECK(o->DyeFor(2).channels[0].cut == 128);
        }

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(buf.size()));
        outer.insert(outer.end(), buf.begin(), buf.end());
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);

        NpcAssignmentMap map;
        CHECK(DecodeNpcAssignments(outer, 24u, map));  // NPCO v24, outgoing outer
        CHECK(map.size() == 1);
        if (map.size() == 1) {
            const auto* f = map.begin()->second.library.At(0);
            CHECK(f != nullptr);
            if (f) {
                CHECK(f->DyeFor(2).channels[0].r == 10);
                CHECK(f->DyeFor(2).channels[0].cut == 128);
            }
        }
    }

    {  // ⚠⚠ THE v23 MIGRATION: A SLOT COLOUR BECOMES THAT PIECE'S COLOUR.
       // A v23 dye entry names only a bit. The armour SLOT block is decoded
       // before the dye block, so by the time the entry arrives the outfit
       // already knows what is in that bit, and SetDye keys the colour to it.
       // This is the whole reason the format change needed no migration pass:
       // the ordering does it. Hand-forged, because Encode() cannot emit v23.
        const auto putChannel = [](std::vector<std::byte>& a_out, bool a_set,
                                   std::uint8_t a_r, std::uint8_t a_g,
                                   std::uint8_t a_b) {
            a_out.push_back(std::byte{ a_set ? std::uint8_t{ 1 } : std::uint8_t{ 0 } });
            a_out.push_back(static_cast<std::byte>(a_r));
            a_out.push_back(static_cast<std::byte>(a_g));
            a_out.push_back(static_cast<std::byte>(a_b));
            a_out.push_back(std::byte{ 255 });  // strength (v13)
            for (int i = 0; i < 6; ++i) {
                a_out.push_back(std::byte{ 0 });  // mode, second*, flake (v14/v15)
            }
            for (int fb = 0; fb < 2; ++fb) {      // palette then player (v15)
                for (int i = 0; i < 5; ++i) {
                    a_out.push_back(std::byte{ 0 });
                }
                a_out.push_back(std::byte{ 128 });  // gloss
            }
            a_out.push_back(std::byte{ 0 });      // blend: defer (v19)
        };

        std::vector<std::byte> buf;
        PutLE32(buf, 1);                     // outfit count
        PutLE32(buf, 1);                     // active
        PutStrV1(buf, "Worn");
        buf.push_back(std::byte{ 0 });       // favorite
        PutLE32(buf, 1);                     // armor slot count = 1
        PutLE32(buf, 2);                     // bit 2
        buf.push_back(static_cast<std::byte>(SlotEntry::Kind::kStyle));
        PutStrV1(buf, "Armors.esp");
        PutLE32(buf, 0x800);
        PutLE32(buf, 0);                     // weapon entry count
        PutLE32(buf, 0);                     // empty preset name (v3)
        buf.push_back(std::byte{ 0 });       // ORefitMode::kDefault (v3)
        PutLE32(buf, 0);                     // no per-hand overrides (v4)
        buf.push_back(std::byte{ 0 });       // HairMode::kAuto (v5)
        for (int i = 0; i < 4; ++i) {
            buf.push_back(std::byte{ 0 });   // hairTint (v6)
        }
        PutLE32(buf, 0);                     // hairStyle modName (v7)
        PutLE32(buf, 0);                     // hairStyle localFormID (v7)

        PutLE32(buf, 1);                     // ONE dye entry
        PutLE32(buf, 2);                     // on bit 2, and NO garment: this is v23
        PutLE32(buf, static_cast<std::uint32_t>(kDyeChannelCount));
        putChannel(buf, true, 10, 20, 30);
        for (std::size_t c = 1; c < kDyeChannelCount; ++c) {
            putChannel(buf, false, 0, 0, 0);
        }

        PutLE32(buf, 0);                     // weapon dyes (v10)
        PutStrV1(buf, "");                   // body preset id (v11)
        PutStrV1(buf, "");                   // eyes: modName (v12)
        PutLE32(buf, 0);                     // eyes: localFormID (v12)
        PutStrV1(buf, "");                   // brows: modName (v16)
        PutLE32(buf, 0);                     // brows: localFormID (v16)
        PutStrV1(buf, "");                   // facial hair: modName (v16)
        PutLE32(buf, 0);                     // facial hair: localFormID (v16)
        for (int i = 0; i < 4; ++i) {
            buf.push_back(std::byte{ 0 });   // eye tint (v17)
        }
        for (int i = 0; i < 4; ++i) {
            buf.push_back(std::byte{ 0 });   // sclera tint (v18)
        }
        buf.push_back(std::byte{ 0 });       // eye blend (v19)
        for (int i = 0; i < 4; ++i) {
            buf.push_back(std::byte{ 0 });   // eye tint 2 (v20)
        }
        PutLE32(buf, 0);                     // invented head-part slots (v21)
        PutLE32(buf, 0);                     // head-part dyes (v22)
        buf.push_back(std::byte{ 0 });       // push-up (v23)

        OutfitLibrary back;
        CHECK(Decode(buf, 23u, back));
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            // The colour landed, and it landed ON THE GARMENT rather than on the
            // bit: swap the piece and it is gone, put the piece back and it
            // returns. A v23 build could not have told those two apart.
            CHECK(o->DyeFor(2).channels[0].set);
            CHECK(o->DyeFor(2).channels[0].r == 10);

            Outfit swapped = *o;
            swapped.SetStyle(2, StyleRefKey{ "Armors.esp", 0x801 });
            CHECK(!swapped.DyeFor(2).Any());
            swapped.SetStyle(2, StyleRefKey{ "Armors.esp", 0x800 });
            CHECK(swapped.DyeFor(2).channels[0].r == 10);
        }
    }

    {  // ⚠ THE OUTGOING NPCO v19, END TO END, step 3 of the four the bump note
       // lists. A v19 outer embeds LIBR v19 bytes, so the forge is the current
       // encode with only the v20 second eye colour dropped.
        OutfitLibrary lib;
        CHECK(lib.Create("StillReadableAtNpco19") >= 0);
        auto bytes = Encode(lib);
        bytes.resize(bytes.size() - 13);  // drop v22 head dye + v21 slot count + v20 second eye: LIBR v19 bytes

        OutfitLibrary back;
        CHECK(Decode(bytes, 19u, back));  // LIBR v19, the outgoing inner version
        CHECK(back.Count() == 1);

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(bytes.size()));
        outer.insert(outer.end(), bytes.begin(), bytes.end());
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);

        NpcAssignmentMap map;
        CHECK(DecodeNpcAssignments(outer, 19u, map));  // NPCO v19, outgoing outer
        CHECK(map.size() == 1);
    }

    {  // ⚠ THE OUTGOING NPCO v18, END TO END, which is step 3 of the four the
       // bump note lists. The mapping assertions above check a pure function,
       // and that pure function read perfectly fine on both occasions this bug
       // reached a build. A v18 outer embeds LIBR v18 bytes, so the forge is
       // the current encode with only the v19 eye blend dropped.
        OutfitLibrary lib;
        CHECK(lib.Create("StillReadableAtNpco18") >= 0);
        auto bytes = Encode(lib);
        bytes.resize(bytes.size() - 14);  // drop v22 head dye + v21 count + v20 second eye + v19 blend: LIBR v18 bytes

        OutfitLibrary back;
        CHECK(Decode(bytes, 18u, back));  // LIBR v18, the outgoing inner version
        CHECK(back.Count() == 1);

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(bytes.size()));
        outer.insert(outer.end(), bytes.begin(), bytes.end());
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);

        NpcAssignmentMap map;
        CHECK(DecodeNpcAssignments(outer, 18u, map));  // NPCO v18, outgoing outer
        CHECK(map.size() == 1);
    }

    {  // ⚠ THE OUTGOING NPCO v16, END TO END. A v16 outer embeds LIBR v17
       // bytes (the eye-colour build wrote exactly this shape), so the forge is
       // the current encode with only the v18 sclera dropped. If the freeze
       // above ever regresses to the default arm, the inner decode runs off
       // the end looking for a sclera nobody wrote and this drops the record.
        OutfitLibrary lib;
        CHECK(lib.Create("StillReadableAtNpco16") >= 0);
        auto bytes = Encode(lib);
        bytes.resize(bytes.size() - 18);  // drop v22 head dye + v21 count + v20 + v19 + v18 sclera: LIBR v17 bytes

        OutfitLibrary back;
        CHECK(Decode(bytes, 17u, back));  // LIBR v17, the outgoing inner version
        CHECK(back.Count() == 1);

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(bytes.size()));
        outer.insert(outer.end(), bytes.begin(), bytes.end());
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);

        NpcAssignmentMap map;
        CHECK(DecodeNpcAssignments(outer, 16u, map));  // NPCO v16, outgoing outer
        CHECK(map.size() == 1);
    }

    {  // ⚠ THE OUTGOING VERSION MUST STILL BE ACCEPTED, AND THIS IS A SECOND
       // WAY TO LOSE IT THAT THE FREEZE ABOVE DOES NOT COVER. Both decoders
       // gate on an explicit list that ends with the CURRENT-version constant
       // rather than a literal, so the moment that constant moves, the version
       // it used to name falls out of the list entirely. The freeze decides
       // which inner version a v12 outer embeds; this decides whether a v12
       // record is read at all. Getting the freeze right and this wrong loses
       // strictly more data, because it refuses the whole record rather than
       // one entry.
       //
       // Not a hypothetical: the accepted lists carry literals 1 through 11 and
       // nothing else, so every earlier bump had to add exactly this line.
        OutfitLibrary lib;
        CHECK(lib.Create("StillReadable") >= 0);
        auto bytes = Encode(lib);
        bytes.resize(bytes.size() - 22);   // drop v22 head dye + v21 count + v20 + v19 blend + v18 sclera + v17 eye tint
        bytes.resize(bytes.size() - 8);  // v16 appends facial hair; this predates it

        OutfitLibrary back;
        CHECK(Decode(bytes, 12u, back));  // LIBR v12, the outgoing inner version

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(bytes.size()));
        outer.insert(outer.end(), bytes.begin(), bytes.end());
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);

        NpcAssignmentMap map;
        CHECK(DecodeNpcAssignments(outer, 12u, map));  // NPCO v12, outgoing outer
    }

    {  // ⚠ AND THE SAME THING FOR THE v14 BUMP, WHICH IS THE ONE THAT WOULD
       // HAVE HURT MOST. v13 is the version the shipped 0.4.0 playtest build
       // writes, so a v14 decoder that dropped 13 out of its accepted list
       // would refuse the whole 'LIBR' record of every save made on that build:
       // the entire outfit library and every NPC assignment, not one entry.
       // That is the exact loss the handoff for this feature opens with.
        OutfitLibrary lib;
        CHECK(lib.Create("StillReadableAtV13") >= 0);
        auto bytes = Encode(lib);
        bytes.resize(bytes.size() - 22);   // drop v22 head dye + v21 count + v20 + v19 blend + v18 sclera + v17 eye tint
        bytes.resize(bytes.size() - 8);  // v16 appends facial hair; this predates it

        OutfitLibrary back;
        CHECK(Decode(bytes, 13u, back));  // LIBR v13, the outgoing inner version
        CHECK(back.Count() == 1);

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(bytes.size()));
        outer.insert(outer.end(), bytes.begin(), bytes.end());
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);

        NpcAssignmentMap map;
        CHECK(DecodeNpcAssignments(outer, 13u, map));  // NPCO v13, outgoing outer
        CHECK(map.size() == 1);
    }

    {  // ⚠ AND v14, THE ONE-DAY VERSION. It never shipped anywhere, but the dev
       // machine's own field-session saves from 2026-08-08 carry it, and both
       // accepted lists end with the current-version constant, so 14 fell out
       // of them the moment that constant moved to 15.
        OutfitLibrary lib;
        CHECK(lib.Create("StillReadableAtV14") >= 0);
        auto bytes = Encode(lib);
        bytes.resize(bytes.size() - 22);   // drop v22 head dye + v21 count + v20 + v19 blend + v18 sclera + v17 eye tint
        bytes.resize(bytes.size() - 8);  // v16 appends facial hair; this predates it

        OutfitLibrary back;
        CHECK(Decode(bytes, 14u, back));  // LIBR v14, the outgoing inner version
        CHECK(back.Count() == 1);

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(bytes.size()));
        outer.insert(outer.end(), bytes.begin(), bytes.end());
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);

        NpcAssignmentMap map;
        CHECK(DecodeNpcAssignments(outer, 14u, map));  // NPCO v14, outgoing outer
        CHECK(map.size() == 1);
    }

    {  // Facial hair survives a round trip at v16, beside the eye and brow
       // refs it was appended behind, and an outfit that names none decodes to
       // an empty reference rather than to whatever the neighbouring field
       // held. Both halves matter: the pair is written unconditionally, so a
       // misordered read would swap a beard for a brow rather than lose it.
        OutfitLibrary lib;
        const int     idx = lib.Create("Bearded");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto* o      = lib.At(static_cast<std::size_t>(idx));
            o->eyes       = StyleRefKey{ "Eyes.esp", 0x111u };
            o->brows      = StyleRefKey{ "Brows.esp", 0x222u };
            o->facialHair = StyleRefKey{ "Beards.esp", 0x333u };
        }
        const int bare = lib.Create("CleanShaven");
        CHECK(bare >= 0);

        OutfitLibrary back;
        CHECK(Decode(Encode(lib), kCodecVersion, back));
        CHECK(back.Count() == 2);
        if (const auto* o = back.At(0)) {
            CHECK(o->eyes == (StyleRefKey{ "Eyes.esp", 0x111u }));
            CHECK(o->brows == (StyleRefKey{ "Brows.esp", 0x222u }));
            CHECK(o->facialHair == (StyleRefKey{ "Beards.esp", 0x333u }));
        }
        if (const auto* o = back.At(1)) {
            CHECK(o->facialHair.Empty());
        }
    }

    {  // ⚠ AND v15, THE VERSION THIS BUMP MAKES OUTGOING (OS-196, facial hair).
       // v15 is what the 2026-08-09 texture-swap builds wrote and what the dev
       // machine's saves carry, so it has to stay in both accepted lists and
       // its inner version has to stay frozen at LIBR 15. The pure mapping
       // assertions above cannot catch a miss here: this bug reached a build
       // TWICE while NpcoInnerLibrVersion read perfectly fine.
        OutfitLibrary lib;
        CHECK(lib.Create("StillReadableAtV15") >= 0);
        auto bytes = Encode(lib);
        bytes.resize(bytes.size() - 22);   // drop v22 head dye + v21 count + v20 + v19 blend + v18 sclera + v17 eye tint
        bytes.resize(bytes.size() - 8);  // v16 appends facial hair; v15 stops before it

        OutfitLibrary back;
        CHECK(Decode(bytes, 15u, back));  // LIBR v15, the outgoing inner version
        CHECK(back.Count() == 1);
        if (const auto* o = back.At(0)) {
            CHECK(o->name == "StillReadableAtV15");
            CHECK(o->facialHair.Empty());  // a v15 outfit names none, and that is right
        }

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(bytes.size()));
        outer.insert(outer.end(), bytes.begin(), bytes.end());
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);

        NpcAssignmentMap map;
        CHECK(DecodeNpcAssignments(outer, 15u, map));  // NPCO v15, outgoing outer
        CHECK(map.size() == 1);
        const auto it = map.find(NpcKey{ "Skyrim.esm", 0x1A6C3u });
        CHECK(it != map.end());
        if (it != map.end()) {
            const auto* o = it->second.library.At(0);
            CHECK(o != nullptr);
            if (o) {
                CHECK(o->name == "StillReadableAtV15");
            }
        }
    }

    {  // END TO END, THE v11 BOUNDARY. NPCO v10 embeds LIBR v10 and must
       // remain readable after LIBR v11 appends the stable custom-body ID.
        OutfitLibrary inner;
        const int idx = inner.Create("FromNpcoV10");
        CHECK(idx >= 0);
        if (idx >= 0) {
            inner.At(static_cast<std::size_t>(idx))->obodyPreset = "Installed V10 Body";
        }
        auto innerBytes = Encode(inner);
        CHECK(innerBytes.size() > 20);
        // Encode writes the CURRENT version, so getting back to v10 bytes means
        // stripping every suffix appended since, newest first.
        innerBytes.resize(innerBytes.size() - 8);   // strip v16 facial hair
        innerBytes.resize(innerBytes.size() - 22);   // drop v22 head dye + v21 count + v20 + v19 blend + v18 sclera + v17 eye tint
        innerBytes.resize(innerBytes.size() - 16);  // strip v12 eyes + brows
        innerBytes.resize(innerBytes.size() - 4);   // strip v11 empty custom ID

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(innerBytes.size()));
        outer.insert(outer.end(), innerBytes.begin(), innerBytes.end());
        PutLE32(outer, 1);
        PutStrV1(outer, "Baseline V10");
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0xD66u);

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(outer, 10, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Skyrim.esm", 0x1A6C3u });
        CHECK(it != back.end());
        if (it != back.end()) {
            const auto* outfit = it->second.library.At(0);
            CHECK(outfit != nullptr);
            if (outfit) {
                CHECK(outfit->obodyPreset == "Installed V10 Body");
                CHECK(outfit->customBodyPresetId.empty());
            }
            CHECK(it->second.obodyBaseline == "Baseline V10");
            CHECK(it->second.hairBaselineLocalID == 0xD66u);
        }
    }

    {  // END TO END, THE v12 BOUNDARY. NPCO v11 embeds LIBR v11 and must stay
       // readable now that LIBR v12 appends eye and brow references.
       //
       // ⚠ THE THIRD OF THE THREE THINGS NpcAssignments.h DEMANDS ON A BUMP, and
       // its note says why a mapping assertion is not a substitute: that checks
       // a pure function, and the bug it is guarding against reached a build
       // twice while the pure function read perfectly fine. This forges the
       // actual bytes.
        OutfitLibrary inner;
        const int     idx = inner.Create("FromNpcoV11");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto* o               = inner.At(static_cast<std::size_t>(idx));
            o->customBodyPresetId = "custom-v11";
            o->hairStyle          = StyleRefKey{ "Hair.esp", 0x99u };
        }
        auto innerBytes = Encode(inner);
        CHECK(innerBytes.size() > 16);
        innerBytes.resize(innerBytes.size() - 8);   // strip v16 facial hair
        innerBytes.resize(innerBytes.size() - 22);   // drop v22 head dye + v21 count + v20 + v19 blend + v18 sclera + v17 eye tint
        innerBytes.resize(innerBytes.size() - 16);  // back to exactly v11 bytes

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(innerBytes.size()));
        outer.insert(outer.end(), innerBytes.begin(), innerBytes.end());
        PutLE32(outer, 1);
        PutStrV1(outer, "Baseline V11");
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0xD66u);

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(outer, 11, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Skyrim.esm", 0x1A6C3u });
        CHECK(it != back.end());
        if (it != back.end()) {
            const auto* outfit = it->second.library.At(0);
            CHECK(outfit != nullptr);
            if (outfit) {
                // Everything v11 carried survives...
                CHECK(outfit->name == "FromNpcoV11");
                CHECK(outfit->customBodyPresetId == "custom-v11");
                CHECK(outfit->hairStyle == (StyleRefKey{ "Hair.esp", 0x99u }));
                // ...and the fields it never had decode to leave-them-alone.
                CHECK(outfit->eyes.Empty());
                CHECK(outfit->brows.Empty());
            }
            CHECK(it->second.obodyBaseline == "Baseline V11");
        }
    }

    {  // END TO END, THE v13 BOUNDARY. NPCO v12 embeds LIBR v12 and must stay
       // readable now that LIBR v13 writes a strength byte per dye channel.
       //
       // ⚠ THE THIRD OF THE THREE THINGS NpcAssignments.h DEMANDS ON A BUMP.
       // The mapping assertion above checks a pure function, and this bug
       // reached a build twice while that pure function read perfectly fine.
       //
       // ⚠ THE FIXTURE IS DELIBERATELY UNDYED, AND THAT IS WHAT MAKES THE FORGE
       // EXACT. Every boundary test above this one truncates trailing bytes to
       // walk an encoding back a version. That cannot work here, because the
       // strength byte is written INSIDE each channel rather than appended at
       // the end, so a dyed library's v13 bytes are interleaved with its v12
       // ones and no suffix can be cut off. With no dye stored, PutChannel is
       // never reached at all (the armour walk skips slots failing dye.Any()
       // and ForEachWeaponDye visits only stored weapon dyes), so this
       // library's v13 encoding IS its v12 encoding, byte for byte, and the
       // forge needs no truncation.
        OutfitLibrary inner;
        const int     idx = inner.Create("FromNpcoV12");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto* o               = inner.At(static_cast<std::size_t>(idx));
            o->customBodyPresetId = "custom-v12";
            o->hairStyle          = StyleRefKey{ "Hair.esp", 0x99u };
        }
        auto innerBytes = Encode(inner);
        innerBytes.resize(innerBytes.size() - 22);   // drop v22 head dye + v21 count + v20 + v19 blend + v18 sclera + v17 eye tint
        innerBytes.resize(innerBytes.size() - 8);  // v16 appends facial hair; this inner predates it
        CHECK(innerBytes.size() > 16);

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(innerBytes.size()));
        outer.insert(outer.end(), innerBytes.begin(), innerBytes.end());
        PutLE32(outer, 1);
        PutStrV1(outer, "Baseline V12");
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0xD66u);

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(outer, 12, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Skyrim.esm", 0x1A6C3u });
        CHECK(it != back.end());
        if (it != back.end()) {
            const auto* outfit = it->second.library.At(0);
            CHECK(outfit != nullptr);
            if (outfit) {
                CHECK(outfit->name == "FromNpcoV12");
                CHECK(outfit->customBodyPresetId == "custom-v12");
                CHECK(outfit->hairStyle == (StyleRefKey{ "Hair.esp", 0x99u }));
            }
            CHECK(it->second.obodyBaseline == "Baseline V12");
        }
    }

    {  // END TO END, THE v14 BOUNDARY. NPCO v13 embeds LIBR v13 and must stay
       // readable now that LIBR v14 writes a mode and a second stop per dye
       // channel.
       //
       // ⚠ THE THIRD OF THE THINGS NpcAssignments.h DEMANDS ON A BUMP, and the
       // one the two previous regressions needed: the mapping assertion above
       // checks a pure function, and that function read perfectly fine both
       // times this bug reached a build.
       //
       // ⚠ THE FIXTURE IS UNDYED FOR THE SAME REASON THE v13 ONE IS, and the
       // reason got stronger rather than weaker. v14 also writes INSIDE each
       // channel rather than appending at the end, so a dyed library's v13 bytes
       // are interleaved with its v14 ones and no suffix can be cut off. With no
       // dye stored, PutChannel is never reached, so this library's v14 encoding
       // IS its v13 encoding byte for byte and the forge needs no truncation.
       // What the interleaved half costs in coverage is paid back by the direct
       // GetChannel boundary test above, which is the exact unit the gate is in.
        OutfitLibrary inner;
        const int     idx = inner.Create("FromNpcoV13");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto* o               = inner.At(static_cast<std::size_t>(idx));
            o->customBodyPresetId = "custom-v13";
            o->hairStyle          = StyleRefKey{ "Hair.esp", 0x99u };
            o->eyes               = StyleRefKey{ "Eyes.esp", 0x55u };
        }
        auto innerBytes = Encode(inner);
        innerBytes.resize(innerBytes.size() - 22);   // drop v22 head dye + v21 count + v20 + v19 blend + v18 sclera + v17 eye tint
        innerBytes.resize(innerBytes.size() - 8);  // v16 appends facial hair; this inner predates it
        CHECK(innerBytes.size() > 16);

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(innerBytes.size()));
        outer.insert(outer.end(), innerBytes.begin(), innerBytes.end());
        PutLE32(outer, 1);
        PutStrV1(outer, "Baseline V13");
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0xD66u);

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(outer, 13, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Skyrim.esm", 0x1A6C3u });
        CHECK(it != back.end());
        if (it != back.end()) {
            const auto* outfit = it->second.library.At(0);
            CHECK(outfit != nullptr);
            if (outfit) {
                CHECK(outfit->name == "FromNpcoV13");
                CHECK(outfit->customBodyPresetId == "custom-v13");
                CHECK(outfit->hairStyle == (StyleRefKey{ "Hair.esp", 0x99u }));
                CHECK(outfit->eyes == (StyleRefKey{ "Eyes.esp", 0x55u }));
            }
            CHECK(it->second.obodyBaseline == "Baseline V13");
        }
    }

    {  // END TO END, THE v15 BOUNDARY. NPCO v14 embeds LIBR v14 and must stay
       // readable now that LIBR v15 writes flake and finish bytes per channel.
       // Undyed forge for the same interleaving reason as the v13 and v14 ones.
        OutfitLibrary inner;
        const int     idx = inner.Create("FromNpcoV14");
        CHECK(idx >= 0);
        if (idx >= 0) {
            inner.At(static_cast<std::size_t>(idx))->customBodyPresetId = "custom-v14";
        }
        auto innerBytes = Encode(inner);
        innerBytes.resize(innerBytes.size() - 22);   // drop v22 head dye + v21 count + v20 + v19 blend + v18 sclera + v17 eye tint
        innerBytes.resize(innerBytes.size() - 8);  // v16 appends facial hair; this inner predates it

        std::vector<std::byte> outer;
        PutLE32(outer, 1);
        PutStrV1(outer, "Skyrim.esm");
        PutLE32(outer, 0x1A6C3u);
        PutLE32(outer, static_cast<std::uint32_t>(innerBytes.size()));
        outer.insert(outer.end(), innerBytes.begin(), innerBytes.end());
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);
        PutStrV1(outer, "");
        PutLE32(outer, 0);

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(outer, 14, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Skyrim.esm", 0x1A6C3u });
        CHECK(it != back.end());
        if (it != back.end()) {
            const auto* outfit = it->second.library.At(0);
            CHECK(outfit != nullptr);
            if (outfit) {
                CHECK(outfit->name == "FromNpcoV14");
                CHECK(outfit->customBodyPresetId == "custom-v14");
            }
        }
    }

    {  // A two-stop channel survives a full library round trip at v14, on
       // armour and on a weapon, which is the pair PutChannel's two callers are.
       // The direct round trip above pins the bytes; this pins that BOTH blocks
       // reach the widened function, which is the whole reason the per-channel
       // bytes were moved into it.
        OutfitLibrary lib;
        const int     idx = lib.Create("TwoStop");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto*      o = lib.At(static_cast<std::size_t>(idx));
            DyeChannel ch{};
            ch.set = true;
            ch.r = 0x8F; ch.g = 0xA7; ch.b = 0xC4;
            ch.strength  = 200;
            ch.mode      = 2;
            ch.secondSet = true;
            ch.r2 = 0xC9; ch.g2 = 0xA0; ch.b2 = 0xD8;
            o->SetDye(3u, DyeChannelId::kPrimary, ch);
            o->SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary, ch,
                            WeaponHand::Both);
        }
        OutfitLibrary back;
        CHECK(Decode(Encode(lib), kCodecVersion, back));
        CHECK(back.Count() == 1);
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            const auto& arm = o->DyeFor(3u).channels[0];
            CHECK(arm.mode == 2);
            CHECK(arm.secondSet);
            CHECK(arm.r2 == 0xC9 && arm.g2 == 0xA0 && arm.b2 == 0xD8);
            CHECK(arm.strength == 200);

            bool sawWeapon = false;
            o->ForEachWeaponDye([&](WeaponClass, WeaponHand, const SlotDye& d) {
                sawWeapon = true;
                CHECK(d.channels[0].mode == 2);
                CHECK(d.channels[0].secondSet);
                CHECK(d.channels[0].b2 == 0xD8);
            });
            CHECK(sawWeapon);
        }
    }

    {  // Strength survives a v13 round trip, on armour and on a weapon.
        OutfitLibrary lib;
        const int     idx = lib.Create("Strength");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto*      o = lib.At(static_cast<std::size_t>(idx));
            DyeChannel ch{};
            ch.set = true;
            ch.r = 200; ch.g = 40; ch.b = 40;
            ch.strength = 77;
            o->SetDye(3u, DyeChannelId::kPrimary, ch);
            o->SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary, ch,
                            WeaponHand::Both);
        }
        OutfitLibrary back;
        CHECK(Decode(Encode(lib), kCodecVersion, back));
        CHECK(back.Count() == 1);
        if (back.Count() == 1) {
            const auto* o = back.At(0);
            CHECK(o->DyeFor(3u).channels[0].strength == 77);
            bool sawWeapon = false;
            o->ForEachWeaponDye([&](WeaponClass, WeaponHand, const SlotDye& d) {
                sawWeapon = true;
                CHECK(d.channels[0].strength == 77);
            });
            CHECK(sawWeapon);
        }
    }

    {  // ⚠ AND A v12 CHANNEL COMES BACK AT FULL STRENGTH, which is the half of
       // the default that actually matters. 255 on the struct makes an older
       // record READABLE; this pins that it is also CORRECT, because a channel
       // written before the byte existed was painted at full force and full
       // force is what it has to decode to. A default of 0 would read without
       // any error at all and repaint every saved dye in every save to grey.
       //
       // ⚠ TESTED ON GetChannel DIRECTLY RATHER THAN THROUGH A LIBRARY, and
       // that is forced rather than preferred. Every other old-version case in
       // this file forges its fixture by encoding at the current version and
       // truncating, which works only while a version APPENDS. This one writes
       // INSIDE each channel, so a dyed library's v12 bytes are not a prefix of
       // its v13 bytes and no amount of truncation produces them. GetChannel is
       // the exact unit the version gate lives in, so it is what gets pinned.
        const std::byte v12[]{ std::byte{ 1 }, std::byte{ 200 }, std::byte{ 40 },
                               std::byte{ 40 } };
        detail::Reader r12{ std::span<const std::byte>{ v12 } };
        DyeChannel     out12{};
        CHECK(detail::GetChannel(r12, 12u, out12));
        CHECK(out12.set && out12.r == 200 && out12.g == 40 && out12.b == 40);
        CHECK(out12.strength == 255);  // full force, not grey
        CHECK(r12.pos == 4);           // and it read no byte that was never written

        const std::byte v13[]{ std::byte{ 1 }, std::byte{ 200 }, std::byte{ 40 },
                               std::byte{ 40 }, std::byte{ 77 } };
        detail::Reader r13{ std::span<const std::byte>{ v13 } };
        DyeChannel     out13{};
        CHECK(detail::GetChannel(r13, 13u, out13));
        CHECK(out13.strength == 77);
        CHECK(r13.pos == 5);

        // A v13 reader handed a truncated channel fails rather than inventing
        // a strength, so a corrupt record is refused instead of half-read.
        detail::Reader rShort{ std::span<const std::byte>{ v12 } };
        DyeChannel     outShort{};
        CHECK(!detail::GetChannel(rShort, 13u, outShort));
    }

    {  // v15 ROUND TRIP: a channel carrying everything a channel can carry
       // survives encode and decode.
       //
       // ⚠ COMPARED WITH operator==, WHICH IS DEFAULTED AND THEREFORE SEES EVERY
       // MEMBER. That is what makes this catch a widening that writes twelve of
       // the thirteen new bytes: a hand-written field-by-field comparison would
       // only check the fields somebody remembered to list.
        DyeChannel in{};
        in.set      = true;
        in.r        = 0x8F;
        in.g        = 0xA7;
        in.b        = 0xC4;
        in.strength = 200;
        in.mode     = 2;
        in.secondSet = true;
        in.r2       = 0xC9;
        in.g2       = 0xA0;
        in.b2       = 0xD8;
        in.flake    = 160;
        in.palette.sheenSet = true;
        in.palette.sheenR   = 0xFF;
        in.palette.sheenG   = 0xF4;
        in.palette.sheenB   = 0xD0;
        in.palette.glossSet = true;
        in.palette.gloss    = 210;
        in.player.glossSet  = true;
        in.player.gloss     = 90;
        in.cut              = 51;

        std::vector<std::byte> bytes;
        detail::PutChannel(bytes, in);
        // set..b2 (10) + flake (1) + palette block (6) + player block (6)
        // + the v19 blend (1) + the v25 cut (1)
        CHECK(bytes.size() == 25);

        detail::Reader rd{ std::span<const std::byte>{ bytes } };
        DyeChannel     out{};
        CHECK(detail::GetChannel(rd, kCodecVersion, out));
        CHECK(out == in);
        CHECK(rd.pos == bytes.size());
    }

    {  // ⚠ THE LITERAL 14, NEVER kCodecVersion - 1, same rule as the 13 below.
       // A v14 channel's bytes end at b2. It must decode with flake 0 and both
       // finish blocks at their defaults, and it must consume exactly its own
       // ten bytes.
        const std::byte v14[]{ std::byte{ 1 },    std::byte{ 0x8F }, std::byte{ 0xA7 },
                               std::byte{ 0xC4 }, std::byte{ 200 },  std::byte{ 2 },
                               std::byte{ 1 },    std::byte{ 0xC9 }, std::byte{ 0xA0 },
                               std::byte{ 0xD8 } };
        detail::Reader r14{ std::span<const std::byte>{ v14 } };
        DyeChannel     out14{};
        CHECK(detail::GetChannel(r14, 14u, out14));
        CHECK(out14.set && out14.mode == 2 && out14.secondSet && out14.b2 == 0xD8);
        CHECK(out14.flake == 0);
        CHECK(!out14.palette.sheenSet && !out14.palette.glossSet);
        CHECK(out14.palette.gloss == kDyeNeutral);  // default, not zero
        CHECK(!out14.player.sheenSet && !out14.player.glossSet);
        CHECK(r14.pos == 10);

        // A v15 reader handed a v14 channel fails rather than inventing a
        // finish, so a truncated record is refused instead of half-read.
        detail::Reader rShort15{ std::span<const std::byte>{ v14 } };
        DyeChannel     outShort15{};
        CHECK(!detail::GetChannel(rShort15, 15u, outShort15));
    }

    {  // ⚠ THE LITERAL 13, NEVER kCodecVersion - 1. PersistenceCodec.h keeps
       // historical versions as literals and only the current one as the
       // constant, so a test written against the constant tracks it forward and
       // silently stops testing the boundary it was written for.
       //
       // A v13 channel's bytes are set, r, g, b, strength and nothing else. It
       // must decode to a FLAT channel: mode 0, no second stop. Anything else
       // repaints every existing save, and the reading that matters is not
       // "readable" but "correct": a channel written before these bytes existed
       // WAS a single flat colour, so flat is what it has to come back as.
       // Garbage in mode would turn every dye in every save into a ramp between
       // its colour and black.
        const std::byte v13[]{ std::byte{ 1 },    std::byte{ 0x8F }, std::byte{ 0xA7 },
                               std::byte{ 0xC4 }, std::byte{ 200 } };
        detail::Reader r13{ std::span<const std::byte>{ v13 } };
        DyeChannel     out13{};
        CHECK(detail::GetChannel(r13, 13u, out13));
        CHECK(out13.set && out13.r == 0x8F && out13.g == 0xA7 && out13.b == 0xC4);
        CHECK(out13.strength == 200);
        CHECK(out13.mode == 0);
        CHECK(!out13.secondSet);
        CHECK(out13.r2 == 0 && out13.g2 == 0 && out13.b2 == 0);
        CHECK(r13.pos == 5);  // it consumed the v13 record and no more

        // And every version below it lands in the same place, so the whole
        // history decodes flat rather than only the version just retired.
        for (std::uint32_t v = 8; v <= 13; ++v) {
            detail::Reader rv{ std::span<const std::byte>{ v13 } };
            DyeChannel     outv{};
            CHECK(detail::GetChannel(rv, v, outv));
            CHECK(outv.mode == 0);
            CHECK(!outv.secondSet);
        }

        // A v14 reader handed a v13 channel fails rather than inventing a mode,
        // so a truncated record is refused instead of half-read. Same guard the
        // v13 case above has, one version on.
        detail::Reader rShort14{ std::span<const std::byte>{ v13 } };
        DyeChannel     outShort14{};
        CHECK(!detail::GetChannel(rShort14, 14u, outShort14));
    }

    {  // ⚠ END TO END: a real 0.4.0 save. NPCO v4 outer wrapping a genuine LIBR
       // v5 inner must recover the follower's library intact. The mapping
       // assertions above check a pure function; this checks the bytes.
       //
       // Before the frozen v4 branch existed, this decoded as TRUE with ZERO
       // entries: silent data loss reported as success.
        OutfitLibrary inner;
        const int     idx = inner.Create("Follower Look");
        CHECK(idx >= 0);
        inner.At(static_cast<std::size_t>(idx))->hair = HairMode::kHide;

        // Forge a genuine v5-shaped inner: encode at v8 and drop the 4-byte
        // empty dye count, the 8-byte style block and the 4-byte tint block
        // that v8, v7 and v6 appended.
        auto innerBytes = Encode(inner);
        CHECK(innerBytes.size() > 20);
        innerBytes.resize(innerBytes.size() - 70);  // + v10 weapon + v11 custom + v12 refs + v16 facial + both eye tints + v19 blend + v22 head dye

        // Hand-build the NPCO v4 outer wire shape around it.
        std::vector<std::byte> outer;
        detail::PutU32(outer, 1u);                  // entry count
        detail::PutStr(outer, "Skyrim.esm");        // key: mod name
        detail::PutU32(outer, 0x1A6C0u);            // key: local form ID
        detail::PutU32(outer, static_cast<std::uint32_t>(innerBytes.size()));
        outer.insert(outer.end(), innerBytes.begin(), innerBytes.end());
        detail::PutU32(outer, 1u);                  // v2+: obodyBaselineCaptured
        detail::PutStr(outer, "Preset A");          // v2+: obodyBaseline

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(outer, 4, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Skyrim.esm", 0x1A6C0u });
        CHECK(it != back.end());
        if (it != back.end()) {
            CHECK(it->second.library.Count() == 1);
            CHECK(it->second.obodyBaselineCaptured);
            CHECK(it->second.obodyBaseline == "Preset A");
            // v4's outer shape has no hair triple either (v6 appended it), so
            // the follower reads back as never having had a colour captured.
            CHECK(!it->second.hairBaselineCaptured);
            CHECK(it->second.hairBaselineMod.empty());
            CHECK(it->second.hairBaselineLocalID == 0u);
            // back.size() == 1 above is what proves the inner bytes were read
            // AT v5: for this single-outfit payload neither wrong version can
            // even reach these fields (v6 dies needing the absent tint bytes,
            // v4 dies on the hair byte as trailing garbage), so the entry only
            // survives at v5. What the two assertions below buy is content
            // FIDELITY - they catch an unrelated field-misalignment regression
            // in the codec that still happened to land the right byte count.
            const auto* o = it->second.library.At(0);
            CHECK(o != nullptr);
            if (o) {
                CHECK(o->hair == HairMode::kHide);
                CHECK(!o->hairTint.set);  // v5 bytes carry no tint
            }
        }
    }

    {  // ⚠ THE v8 DYE BOUNDARY. A LIBR v8 payload writes THREE channels per
       // dyed slot with the number implied by its version; v9 writes a count
       // first and eight channels. A v8 save must still decode, land its three
       // colours, and leave the new channels off.
       //
       // Forged rather than round-tripped on purpose: Encode() only ever emits
       // the current version, so the only way to pin the OLD shape is to write
       // the old bytes by hand. Built on the local PutLE32 so a change to the
       // production primitives cannot move this frozen shape with it.
        OutfitLibrary lib;
        const int     idx = lib.Create("V8Dye");
        CHECK(idx >= 0);

        // A v10 encode of an outfit with NO dye ends in a 4-byte zero dye count
        // followed by a 4-byte zero weapon dye count. Drop both and append a
        // hand-built v8 dye block in their place.
        auto bytes = Encode(lib);
        CHECK(bytes.size() > 8);
        bytes.resize(bytes.size() - 58);  // +8: v17 eye tint, v18 sclera; +1: v19 eye blend; +4: v22 head dye

        PutLE32(bytes, 1);   // one dyed slot
        PutLE32(bytes, 2);   // slot bit 2 (Body)
        // EXACTLY three channels, no count ahead of them: that is v8.
        const std::uint8_t v8[3][4] = {
            { 1, 10, 20, 30 }, { 1, 40, 50, 60 }, { 0, 0, 0, 0 }
        };
        for (const auto& ch : v8) {
            for (const auto b : ch) {
                bytes.push_back(static_cast<std::byte>(b));
            }
        }

        OutfitLibrary out;
        CHECK(Decode(bytes, 8, out));   // 8, not kCodecVersion - the whole point
        CHECK(out.Count() == 1);
        const auto* o = out.At(0);
        CHECK(o != nullptr);
        if (o) {
            const auto& dye = o->DyeFor(2);
            CHECK(dye.channels[0] == (DyeChannel{ true, 10, 20, 30 }));
            CHECK(dye.channels[1] == (DyeChannel{ true, 40, 50, 60 }));
            CHECK(!dye.channels[2].set);
            // The channels v8 never knew about stay off rather than picking up
            // whatever followed on the wire.
            CHECK(!dye.channels[3].set);
            CHECK(!dye.channels[kDyeChannelCount - 1].set);
        }

        // And the same bytes at the CURRENT version must be rejected, because
        // v9 expects a channel count where v8 put colour bytes. Silent
        // misparsing here is what a missing version guard looks like.
        OutfitLibrary wrong;
        CHECK(!Decode(bytes, kCodecVersion, wrong));
    }

    {  // ⚠ END TO END, THE v8 BOUNDARY. NPCO v8 outer wrapping a genuine LIBR v8
       // inner. Required by checklist item 3 in NpcAssignments.h: the mapping
       // assertions only exercise a pure function, and this bug reached a build
       // twice while that function read perfectly fine.
       //
       // ⚠ The inner NEEDS SURGERY NOW, and this comment used to say it did
       // not. That was true while v9 was current: a v9 encode with no dye was
       // byte-identical to a v8 one, both ending in a 4-byte zero dye count.
       // v10 appends a zero weapon dye count that no v8 writer ever emitted, so
       // it has to come off. What is being pinned is that NpcoInnerLibrVersion(8)
       // is a frozen 8u and not the default arm's kCodecVersion.
        OutfitLibrary inner;
        const int     idx = inner.Create("FromNpcoV8");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto* o = inner.At(static_cast<std::size_t>(idx));
            CHECK(o != nullptr);
            if (o) {
                o->hair     = HairMode::kHide;
                o->hairTint = HairTint{ true, 12, 34, 56 };
            }
        }
        auto innerBytes = Encode(inner);
        CHECK(innerBytes.size() > 4);
        innerBytes.resize(innerBytes.size() - 54);  // strip v10 weapon + v11 custom + v12 refs + v16 facial + both eye tints + v19 blend + v22 head dye

        std::vector<std::byte> outer;
        PutLE32(outer, 1);                       // entry count
        PutStrV1(outer, "Skyrim.esm");           // key: mod name
        PutLE32(outer, 0x1A6C1u);                // key: local form ID
        PutLE32(outer, static_cast<std::uint32_t>(innerBytes.size()));
        outer.insert(outer.end(), innerBytes.begin(), innerBytes.end());
        PutLE32(outer, 1);                       // v2+: obodyBaselineCaptured
        PutStrV1(outer, "Preset H");             // v2+: obodyBaseline
        PutLE32(outer, 1);                       // v6+: hairBaselineCaptured
        PutStrV1(outer, "Skyrim.esm");           // v6+: hairBaselineMod
        PutLE32(outer, 0xD64u);                  // v6+: hairBaselineLocalID

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(outer, 8, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Skyrim.esm", 0x1A6C1u });
        CHECK(it != back.end());
        if (it != back.end()) {
            // With the freeze missing this reads the inner at v9, trips on the
            // absent per-slot channel count, and drops the entry: the whole
            // follower library gone, with the load still reporting success.
            CHECK(it->second.library.Count() == 1);
            const auto* o = it->second.library.At(0);
            CHECK(o != nullptr);
            if (o) {
                CHECK(o->name == "FromNpcoV8");
                CHECK(o->hair == HairMode::kHide);
                CHECK(o->hairTint.set);
                CHECK(o->hairTint.r == 12);
            }
        }
    }

    {  // v10 WEAPON DYE ROUND TRIP. Three storages have to survive, and the
       // third is the one a codec gets wrong: an explicitly emptied hand
       // override is a stored value that renders UNDYED, which is a different
       // picture from having no override and inheriting Both.
        OutfitLibrary lib;
        const int     idx = lib.Create("WeaponDye");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto* o = lib.At(static_cast<std::size_t>(idx));
            CHECK(o != nullptr);
            if (o) {
                o->SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary,
                                DyeChannel{ true, 10, 20, 30 });
                o->SetWeaponDye(WeaponClass::Sword, DyeChannelId::kSecondary,
                                DyeChannel{ true, 40, 50, 60 });
                o->SetWeaponDye(WeaponClass::Staff, DyeChannelId::kPrimary,
                                DyeChannel{ true, 70, 80, 90 }, WeaponHand::Right);
                // Explicitly emptied, not absent.
                o->ClearWeaponDye(WeaponClass::Sword, WeaponHand::Left);
                // And an armour dye alongside, so the two blocks are proven not
                // to read each other's bytes.
                o->SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 1, 2, 3 });
            }
        }

        OutfitLibrary back;
        CHECK(Decode(Encode(lib), kCodecVersion, back));
        CHECK(back.Count() == 1);
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            CHECK(o->WeaponDyeFor(WeaponClass::Sword).channels[0] ==
                  (DyeChannel{ true, 10, 20, 30 }));
            CHECK(o->WeaponDyeFor(WeaponClass::Sword).channels[1] ==
                  (DyeChannel{ true, 40, 50, 60 }));
            CHECK(o->ResolvedWeaponDyeFor(WeaponClass::Staff, WeaponHand::Right)
                      .channels[0] == (DyeChannel{ true, 70, 80, 90 }));
            // ⚠ THE ASSERTION A CODEC LOSES. The override must come back
            // PRESENT and EMPTY. Come back absent and the off-hand sword
            // silently inherits the Both colour on the next load.
            CHECK(o->WeaponDyeOverrideFor(WeaponClass::Sword, WeaponHand::Left) != nullptr);
            CHECK(!o->ResolvedWeaponDyeFor(WeaponClass::Sword, WeaponHand::Left).Any());
            // The right hand of the same class never had an override and still
            // inherits, so "present and empty" was not just written everywhere.
            CHECK(o->WeaponDyeOverrideFor(WeaponClass::Sword, WeaponHand::Right) == nullptr);
            CHECK(o->ResolvedWeaponDyeFor(WeaponClass::Sword, WeaponHand::Right)
                      .channels[0] == (DyeChannel{ true, 10, 20, 30 }));
            CHECK(o->DyeFor(2).channels[0] == (DyeChannel{ true, 1, 2, 3 }));
        }
    }

    {  // ⚠ THE v10 GOLDEN TAIL. Pins the weapon dye block's byte layout, which
       // is otherwise only ever compared against itself by a round trip.
       //
       // The order is CANONICAL, class then hand, and that is what this really
       // guards: the entries are written back to front below, so a codec that
       // emitted insertion order would fail here rather than in the field on
       // somebody else's save.
        OutfitLibrary lib;
        const int     idx = lib.Create("Golden");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto* o = lib.At(static_cast<std::size_t>(idx));
            CHECK(o != nullptr);
            if (o) {
                o->SetWeaponDye(WeaponClass::Dagger, DyeChannelId::kPrimary,
                                DyeChannel{ true, 9, 8, 7 }, WeaponHand::Right);
                o->SetWeaponDye(WeaponClass::Sword, DyeChannelId::kPrimary,
                                DyeChannel{ true, 1, 2, 3 });
            }
        }
        const auto bytes = Encode(lib);

        std::vector<std::byte> tail;
        PutLE32(tail, 2);  // two stored weapon dyes
        // Sword (0) / Both (0) first, because canonical order sorts by class.
        tail.push_back(static_cast<std::byte>(WeaponClass::Sword));
        tail.push_back(static_cast<std::byte>(WeaponHand::Both));
        PutLE32(tail, static_cast<std::uint32_t>(kDyeChannelCount));
        for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
            // TWENTY-FIVE bytes since v25 (twenty-three at v15, the blend at
            // v19, the cut at v25), and the fifth is 255 on EVERY
            // channel including the off ones: strength defaults to full on the
            // struct, and PutChannel writes the member rather than branching on
            // `set`.
            //
            // ⚠ THE TWO 128s ARE THE SAME RULE ONE LAYER DOWN: gloss defaults to
            // kDyeNeutral on the struct and PutChannel writes the member, so an
            // unset finish still carries 128 in its gloss byte. Pinning the
            // zeros AND the 128s is the assertion that shipping flake and the
            // finish blocks did not change one byte of what an ORDINARY dye
            // encodes to beyond appending its defaults.
            const std::uint8_t ch[25] = { c == 0 ? std::uint8_t{ 1 } : std::uint8_t{ 0 },
                                          c == 0 ? std::uint8_t{ 1 } : std::uint8_t{ 0 },
                                          c == 0 ? std::uint8_t{ 2 } : std::uint8_t{ 0 },
                                          c == 0 ? std::uint8_t{ 3 } : std::uint8_t{ 0 },
                                          std::uint8_t{ 255 },
                                          std::uint8_t{ 0 },    // v14 mode
                                          std::uint8_t{ 0 },    // v14 secondSet
                                          std::uint8_t{ 0 },    // v14 r2
                                          std::uint8_t{ 0 },    // v14 g2
                                          std::uint8_t{ 0 },    // v14 b2
                                          std::uint8_t{ 0 },    // v15 flake
                                          std::uint8_t{ 0 },    // v15 pal.sheenSet
                                          std::uint8_t{ 0 },    // v15 pal.sheenR
                                          std::uint8_t{ 0 },    // v15 pal.sheenG
                                          std::uint8_t{ 0 },    // v15 pal.sheenB
                                          std::uint8_t{ 0 },    // v15 pal.glossSet
                                          std::uint8_t{ 128 },  // v15 pal.gloss (kDyeNeutral)
                                          std::uint8_t{ 0 },    // v15 plr.sheenSet
                                          std::uint8_t{ 0 },    // v15 plr.sheenR
                                          std::uint8_t{ 0 },    // v15 plr.sheenG
                                          std::uint8_t{ 0 },    // v15 plr.sheenB
                                          std::uint8_t{ 0 },    // v15 plr.glossSet
                                          std::uint8_t{ 128 },  // v15 plr.gloss
                                          std::uint8_t{ 0 },    // v19 blend: defer
                                          std::uint8_t{ 128 } };  // v25 cut: halfway
            for (const auto b : ch) {
                tail.push_back(static_cast<std::byte>(b));
            }
        }
        // Dagger (1) / Right (1) second.
        tail.push_back(static_cast<std::byte>(WeaponClass::Dagger));
        tail.push_back(static_cast<std::byte>(WeaponHand::Right));
        PutLE32(tail, static_cast<std::uint32_t>(kDyeChannelCount));
        for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
            const std::uint8_t ch[25] = { c == 0 ? std::uint8_t{ 1 } : std::uint8_t{ 0 },
                                          c == 0 ? std::uint8_t{ 9 } : std::uint8_t{ 0 },
                                          c == 0 ? std::uint8_t{ 8 } : std::uint8_t{ 0 },
                                          c == 0 ? std::uint8_t{ 7 } : std::uint8_t{ 0 },
                                          std::uint8_t{ 255 },
                                          std::uint8_t{ 0 },    // v14 mode
                                          std::uint8_t{ 0 },    // v14 secondSet
                                          std::uint8_t{ 0 },    // v14 r2
                                          std::uint8_t{ 0 },    // v14 g2
                                          std::uint8_t{ 0 },    // v14 b2
                                          std::uint8_t{ 0 },    // v15 flake
                                          std::uint8_t{ 0 },    // v15 pal.sheenSet
                                          std::uint8_t{ 0 },    // v15 pal.sheenR
                                          std::uint8_t{ 0 },    // v15 pal.sheenG
                                          std::uint8_t{ 0 },    // v15 pal.sheenB
                                          std::uint8_t{ 0 },    // v15 pal.glossSet
                                          std::uint8_t{ 128 },  // v15 pal.gloss
                                          std::uint8_t{ 0 },    // v15 plr.sheenSet
                                          std::uint8_t{ 0 },    // v15 plr.sheenR
                                          std::uint8_t{ 0 },    // v15 plr.sheenG
                                          std::uint8_t{ 0 },    // v15 plr.sheenB
                                          std::uint8_t{ 0 },    // v15 plr.glossSet
                                          std::uint8_t{ 128 },  // v15 plr.gloss
                                          std::uint8_t{ 0 },    // v19 blend: defer
                                          std::uint8_t{ 128 } };  // v25 cut: halfway
            for (const auto b : ch) {
                tail.push_back(static_cast<std::byte>(b));
            }
        }
        PutLE32(tail, 0);  // v11 custom body ID is empty
        // v12 appends the eye and brow references last, both empty here. This
        // golden tail is compared against the END of Encode's output, so every
        // field appended after the weapon dye block has to be mirrored or the
        // comparison slides off the layout it exists to pin.
        PutLE32(tail, 0);  // v12 eyes: empty modName
        PutLE32(tail, 0);  // v12 eyes: localFormID
        PutLE32(tail, 0);  // v12 brows: empty modName
        PutLE32(tail, 0);  // v12 brows: localFormID
        PutLE32(tail, 0);  // v16 facial hair: empty modName
        PutLE32(tail, 0);  // v16 facial hair: localFormID
        tail.push_back(std::byte{ 0 });  // v17 eyeTint disabled
        tail.push_back(std::byte{ 0 });  // v17 eyeTint.r
        tail.push_back(std::byte{ 0 });  // v17 eyeTint.g
        tail.push_back(std::byte{ 0 });  // v17 eyeTint.b
        tail.push_back(std::byte{ 0 });  // v18 scleraTint disabled
        tail.push_back(std::byte{ 0 });  // v18 scleraTint.r
        tail.push_back(std::byte{ 0 });  // v18 scleraTint.g
        tail.push_back(std::byte{ 0 });  // v18 scleraTint.b
        tail.push_back(std::byte{ 0 });  // v19 eyeBlend: defer
        tail.push_back(std::byte{ 0 });  // v20 eyeTint2 disabled
        tail.push_back(std::byte{ 0 });  // v20 eyeTint2.r
        tail.push_back(std::byte{ 0 });  // v20 eyeTint2.g
        tail.push_back(std::byte{ 0 });  // v20 eyeTint2.b
        PutLE32(tail, 0);  // v21 invented head-part slots: a count of none
        PutLE32(tail, 0);  // v22 head-part dyes: a count of none
        tail.push_back(std::byte{ 0 });  // v23 push-up: none

        CHECK(bytes.size() >= tail.size());
        if (bytes.size() >= tail.size()) {
            const auto off = bytes.size() - tail.size();
            bool       same = true;
            for (std::size_t i = 0; i < tail.size(); ++i) {
                if (bytes[off + i] != tail[i]) {
                    same = false;
                }
            }
            CHECK(same);
        }
    }

    {  // ⚠ THE v9 BOUNDARY, AND THE NEGATIVE CONTROL FOR WHERE THE BLOCK WENT.
       // A v9 payload has no weapon dye block at all. Decoding one must land
       // every armour colour EXACTLY as before and leave weapon dye empty.
       //
       // This is the assertion that catches a weapon block appended in the
       // wrong place: put it ahead of the armour dye block instead of after it
       // and the round trip above still passes, because it only ever compares
       // the codec against itself. This one does not.
        // ⚠ HAND BUILT SINCE v13, NOT TRUNCATED FROM A CURRENT ENCODE. This
        // used to be `Encode(lib)` with 24 trailing bytes dropped, which worked
        // while every version appended. The strength byte lands INSIDE each
        // channel, so a dyed library's v9 bytes are no longer a suffix-trim of
        // its current ones. EncodeV9OneOutfit's own note carries the argument.
        std::vector<std::byte> dyes;
        PutV9DyeEntry(dyes, 2, { { 0, { 11, 22, 33 } }, { 1, { 44, 55, 66 } } });
        PutV9DyeEntry(dyes, 7, { { 0, { 77, 88, 99 } } });
        const auto bytes = EncodeV9OneOutfit("V9Boundary", HairMode::kAuto, 2, dyes);

        OutfitLibrary out;
        CHECK(Decode(bytes, 9, out));  // 9, not kCodecVersion - the whole point
        CHECK(out.Count() == 1);
        const auto* o = out.At(0);
        CHECK(o != nullptr);
        if (o) {
            CHECK(o->DyeFor(2).channels[0] == (DyeChannel{ true, 11, 22, 33 }));
            CHECK(o->DyeFor(2).channels[1] == (DyeChannel{ true, 44, 55, 66 }));
            CHECK(o->DyeFor(7).channels[0] == (DyeChannel{ true, 77, 88, 99 }));
            CHECK(!o->AnyWeaponDye());
            CHECK(o->WeaponDyeOverrideFor(WeaponClass::Sword, WeaponHand::Left) == nullptr);
        }

        // ⚠ THIS CONTROL WEAKENED AT v13, AND IT IS A REAL PROPERTY OF THE
        // FORMAT RATHER THAN A TEST THAT NEEDS LOOSENING. It used to read "the
        // same bytes at the CURRENT version must be REJECTED, because v10
        // expects a weapon dye count where v9's record simply ends". That held
        // while every version APPENDED: a later reader consumed the same bytes
        // per channel, reached the true end of the v9 record, and ran out.
        //
        // v13 writes a byte INSIDE each channel, so a v13 reader takes FIVE
        // bytes where v9 wrote four and walks INTO the following bytes instead
        // of off the end. This fixture's unset channels are all zero, so the
        // misaligned reader finds a zero entry count, then zero-length strings
        // for every field after it, and lands exactly on the end of the buffer.
        // Decode SUCCEEDS on bytes it has completely misread.
        //
        // So what is pinned is no longer "refused". It is the thing that
        // actually matters: a version confusion must not silently hand back the
        // v9 DATA. The tell is the strength byte, which picks up the next
        // channel's `set` flag rather than the 255 a v9 channel means.
        OutfitLibrary wrong;
        bool          sameData = false;
        if (Decode(bytes, kCodecVersion, wrong) && wrong.At(0)) {
            sameData = wrong.At(0)->DyeFor(2).channels[0] ==
                       (DyeChannel{ true, 11, 22, 33 });
        }
        CHECK(!sameData);
    }

    {  // ⚠ END TO END, THE v9 BOUNDARY. NPCO v9 outer wrapping a genuine LIBR v9
       // inner. Required by checklist item 3 in NpcAssignments.h, and it is the
       // item that matters: the mapping assertions only exercise a pure
       // function, and this bug reached a build twice while that function read
       // perfectly fine.
       //
       // Unlike the v8 case, the inner is not a current encode at all: it is
       // built at v9 by hand. It used to be a current encode with 24 trailing
       // bytes stripped, which stopped working at v13 because the strength byte
       // lands INSIDE each dye channel rather than after the record, so a dyed
       // library's v9 bytes are not a prefix of its current ones.
        std::vector<std::byte> dyes;
        PutV9DyeEntry(dyes, 2, { { 0, { 5, 6, 7 } } });
        const auto innerBytes =
            EncodeV9OneOutfit("FromNpcoV9", HairMode::kHide, 1, dyes);

        std::vector<std::byte> outer;
        PutLE32(outer, 1);                       // entry count
        PutStrV1(outer, "Skyrim.esm");           // key: mod name
        PutLE32(outer, 0x1A6C2u);                // key: local form ID
        PutLE32(outer, static_cast<std::uint32_t>(innerBytes.size()));
        outer.insert(outer.end(), innerBytes.begin(), innerBytes.end());
        PutLE32(outer, 1);                       // v2+: obodyBaselineCaptured
        PutStrV1(outer, "Preset K");             // v2+: obodyBaseline
        PutLE32(outer, 1);                       // v6+: hairBaselineCaptured
        PutStrV1(outer, "Skyrim.esm");           // v6+: hairBaselineMod
        PutLE32(outer, 0xD65u);                  // v6+: hairBaselineLocalID

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(outer, 9, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Skyrim.esm", 0x1A6C2u });
        CHECK(it != back.end());
        if (it != back.end()) {
            // With the freeze missing this reads the inner at v10, runs off the
            // end looking for a weapon dye block that was never written, and
            // drops the entry: the whole follower library gone, with the load
            // still reporting success.
            CHECK(it->second.library.Count() == 1);
            const auto* o = it->second.library.At(0);
            CHECK(o != nullptr);
            if (o) {
                CHECK(o->name == "FromNpcoV9");
                CHECK(o->hair == HairMode::kHide);
                CHECK(o->DyeFor(2).channels[0] == (DyeChannel{ true, 5, 6, 7 }));
            }
        }
    }

    {  // ⚠ END TO END, THE v6 BOUNDARY. NPCO v5 outer wrapping a genuine LIBR v6
       // inner, which is what a dev build of 0.4.1 wrote. v5's outer bytes stop
       // after the OBody pair - it has NO hair triple - and its inner payload is
       // LIBR v6, so NpcoInnerLibrVersion(5) must be a frozen 6u rather than
       // aliasing kCodecVersion. Required by checklist item 3 in
       // NpcAssignments.h: the mapping assertions above only exercise a pure
       // function, and this exact bug reached a build twice while that function
       // read perfectly fine.
       //
       // Wire shape verified field by field against EncodeNpcAssignments:
       //   count u32 | modName str | localFormID u32 | innerLen u32 | inner
       //   | obodyBaselineCaptured u32 | obodyBaseline str        <- v5 ends here
       // Built on the local PutLE32/PutStrV1 rather than detail::, so a change
       // to the production primitives cannot silently move this frozen shape
       // with it (see the note above EncodeV1).
        OutfitLibrary inner;
        const int     idx = inner.Create("FromNpcoV5");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto* o = inner.At(static_cast<std::size_t>(idx));
            CHECK(o != nullptr);
            if (o) {
                o->hair     = HairMode::kHide;
                o->hairTint = HairTint{ true, 200, 40, 90 };
            }
        }

        // Forge a genuine LIBR v6 inner: encode at v8 and drop the 4-byte empty
        // dye count (v8) plus the 8-byte hair STYLE block (v7) - an empty
        // StyleRefKey is a 4-byte zero length plus a 4-byte form ID.
        //
        // This used to read `Encode(inner)` whole, because v6 WAS current. That
        // line silently became wrong the moment kCodecVersion moved to 7, which
        // is exactly the failure mode the checklist warns about: the version
        // that just lost current status is the one that breaks.
        auto innerBytes = Encode(inner);
        CHECK(innerBytes.size() > 16);
        innerBytes.resize(innerBytes.size() - 66);  // + v10 weapon + v11 custom + v12 refs + v16 facial + both eye tints + v19 blend + v22 head dye

        std::vector<std::byte> outer;
        PutLE32(outer, 1);                       // entry count
        PutStrV1(outer, "Skyrim.esm");           // key: mod name
        PutLE32(outer, 0x1A6C0u);                // key: local form ID
        PutLE32(outer, static_cast<std::uint32_t>(innerBytes.size()));
        outer.insert(outer.end(), innerBytes.begin(), innerBytes.end());
        PutLE32(outer, 1);                       // v2+: obodyBaselineCaptured
        PutStrV1(outer, "Preset B");             // v2+: obodyBaseline

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(outer, 5, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Skyrim.esm", 0x1A6C0u });
        CHECK(it != back.end());
        if (it != back.end()) {
            // The library SURVIVED. With the freeze missing, the inner bytes get
            // read at whatever LIBR version is current and this is 0 entries -
            // silent data loss reported as a successful load.
            CHECK(it->second.library.Count() == 1);
            CHECK(it->second.obodyBaselineCaptured);
            CHECK(it->second.obodyBaseline == "Preset B");
            // v5 carries no hair triple, so the follower reads back uncaptured
            // rather than picking up whatever followed on the wire.
            CHECK(!it->second.hairBaselineCaptured);
            CHECK(it->second.hairBaselineMod.empty());
            CHECK(it->second.hairBaselineLocalID == 0u);
            const auto* o = it->second.library.At(0);
            CHECK(o != nullptr);
            if (o) {
                CHECK(o->name == "FromNpcoV5");
                CHECK(o->hair == HairMode::kHide);
                CHECK(o->hairTint.set);      // LIBR v6 inner: the tint IS there
                CHECK(o->hairTint.r == 200);
                CHECK(o->hairTint.g == 40);
                CHECK(o->hairTint.b == 90);
            }
        }
    }

    {  // ⚠ END TO END, THE v7 BOUNDARY. NPCO v6 outer wrapping a genuine LIBR v6
       // inner, which is what every save written before hair STYLE existed
       // contains. NPCO v6 outer bytes DO carry the hair triple (that is what
       // v6 added), and its inner is LIBR v6, so NpcoInnerLibrVersion(6) must be
       // a frozen 6u rather than aliasing kCodecVersion.
       //
       // Required by checklist item 3 in NpcAssignments.h. The mapping
       // assertions above only exercise a pure function, and this bug reached a
       // build twice while that function read perfectly fine.
       //
       // Wire shape, field by field against EncodeNpcAssignments:
       //   count u32 | modName str | localFormID u32 | innerLen u32 | inner
       //   | obodyCaptured u32 | obodyBaseline str
       //   | hairCaptured u32 | hairMod str | hairLocalID u32   <- v6 ends here
        OutfitLibrary inner;
        const int     idx = inner.Create("FromNpcoV6");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto* o = inner.At(static_cast<std::size_t>(idx));
            CHECK(o != nullptr);
            if (o) {
                o->hair     = HairMode::kShow;
                o->hairTint = HairTint{ true, 11, 22, 33 };
            }
        }

        // Same forge as above: v8 bytes minus the 4-byte dye count and the
        // 8-byte hair style block.
        auto innerBytes = Encode(inner);
        CHECK(innerBytes.size() > 16);
        innerBytes.resize(innerBytes.size() - 66);  // + v10 weapon + v11 custom + v12 refs + v16 facial + both eye tints + v19 blend + v22 head dye

        std::vector<std::byte> outer;
        PutLE32(outer, 1);                       // entry count
        PutStrV1(outer, "Skyrim.esm");           // key: mod name
        PutLE32(outer, 0x1A6C0u);                // key: local form ID
        PutLE32(outer, static_cast<std::uint32_t>(innerBytes.size()));
        outer.insert(outer.end(), innerBytes.begin(), innerBytes.end());
        PutLE32(outer, 1);                       // v2+: obodyBaselineCaptured
        PutStrV1(outer, "Preset C");             // v2+: obodyBaseline
        PutLE32(outer, 1);                       // v6+: hairBaselineCaptured
        PutStrV1(outer, "Skyrim.esm");           // v6+: hairBaselineMod
        PutLE32(outer, 0x0A042Fu);               // v6+: hairBaselineLocalID

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(outer, 6, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Skyrim.esm", 0x1A6C0u });
        CHECK(it != back.end());
        if (it != back.end()) {
            // The library SURVIVED. With the v6 freeze missing, the inner bytes
            // get read at LIBR v7, run out needing the absent style block, and
            // the entry is DROPPED - 0 entries reported as a successful load.
            CHECK(it->second.library.Count() == 1);
            CHECK(it->second.obodyBaselineCaptured);
            CHECK(it->second.obodyBaseline == "Preset C");
            // v6's outer DOES carry the hair triple, unlike v5's.
            CHECK(it->second.hairBaselineCaptured);
            CHECK(it->second.hairBaselineMod == "Skyrim.esm");
            CHECK(it->second.hairBaselineLocalID == 0x0A042Fu);
            const auto* o = it->second.library.At(0);
            CHECK(o != nullptr);
            if (o) {
                CHECK(o->name == "FromNpcoV6");
                CHECK(o->hair == HairMode::kShow);
                CHECK(o->hairTint.set);
                CHECK(o->hairTint.r == 11);
                // A v6 inner carries no style, so it must read back EMPTY
                // rather than picking up whatever followed on the wire.
                CHECK(o->hairStyle.Empty());
            }
        }
    }

    {  // ⚠ END TO END, THE v8 BOUNDARY. NPCO v7 outer wrapping a genuine LIBR v7
       // inner, which is what every save written before outfit DYE existed
       // contains. NPCO v7 kept v6's outer shape (the OBody pair plus the hair
       // triple) and its inner is LIBR v7 - hair style, no dye block - so
       // NpcoInnerLibrVersion(7) must be a frozen 7u rather than aliasing
       // kCodecVersion.
       //
       // Required by checklist item 3 in NpcAssignments.h. The mapping
       // assertions above only exercise a pure function, and this bug reached a
       // build twice while that function read perfectly fine.
       //
       // Wire shape, field by field against EncodeNpcAssignments:
       //   count u32 | modName str | localFormID u32 | innerLen u32 | inner
       //   | obodyCaptured u32 | obodyBaseline str
       //   | hairCaptured u32 | hairMod str | hairLocalID u32   <- v7 ends here
        OutfitLibrary inner;
        const int     idx = inner.Create("FromNpcoV7");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto* o = inner.At(static_cast<std::size_t>(idx));
            CHECK(o != nullptr);
            if (o) {
                o->SetStyle(kBitBody, StyleRefKey{ "Armors.esp", 0x801 });
                o->hairStyle = StyleRefKey{ "ApachiiSkyHair.esm", 0x0012C4u };
            }
        }

        // Forge a genuine LIBR v7 inner: drop the 4-byte empty dye count (v8)
        // and the 4-byte empty weapon dye count (v10), which is the whole of
        // what those versions append for an outfit that dyes nothing.
        auto innerBytes = Encode(inner);
        CHECK(innerBytes.size() > 8);
        innerBytes.resize(innerBytes.size() - 58);  // +8: v17 eye tint, v18 sclera; +1: v19 eye blend; +4: v22 head dye

        std::vector<std::byte> outer;
        PutLE32(outer, 1);                       // entry count
        PutStrV1(outer, "Skyrim.esm");           // key: mod name
        PutLE32(outer, 0x1A6C0u);                // key: local form ID
        PutLE32(outer, static_cast<std::uint32_t>(innerBytes.size()));
        outer.insert(outer.end(), innerBytes.begin(), innerBytes.end());
        PutLE32(outer, 1);                       // v2+: obodyBaselineCaptured
        PutStrV1(outer, "Preset D");             // v2+: obodyBaseline
        PutLE32(outer, 1);                       // v6+: hairBaselineCaptured
        PutStrV1(outer, "Skyrim.esm");           // v6+: hairBaselineMod
        PutLE32(outer, 0x0A042Fu);               // v6+: hairBaselineLocalID

        NpcAssignmentMap back;
        CHECK(DecodeNpcAssignments(outer, 7, back));
        CHECK(back.size() == 1);
        const auto it = back.find(NpcKey{ "Skyrim.esm", 0x1A6C0u });
        CHECK(it != back.end());
        if (it != back.end()) {
            // The library SURVIVED. With the v7 freeze missing, the inner bytes
            // get read at LIBR v8, run out needing the absent dye count, and
            // the entry is DROPPED - 0 entries reported as a successful load.
            CHECK(it->second.library.Count() == 1);
            CHECK(it->second.obodyBaselineCaptured);
            CHECK(it->second.obodyBaseline == "Preset D");
            CHECK(it->second.hairBaselineCaptured);
            CHECK(it->second.hairBaselineMod == "Skyrim.esm");
            CHECK(it->second.hairBaselineLocalID == 0x0A042Fu);
            const auto* o = it->second.library.At(0);
            CHECK(o != nullptr);
            if (o) {
                CHECK(o->name == "FromNpcoV7");
                CHECK(o->EntryFor(kBitBody).kind == SlotEntry::Kind::kStyle);
                CHECK(o->EntryFor(kBitBody).style.modName == "Armors.esp");
                CHECK(o->hairStyle.modName == "ApachiiSkyHair.esm");
                // A v7 inner carries no dye, so every channel must read back
                // OFF rather than picking up whatever followed on the wire.
                CHECK(!o->AnyDye());
            }
        }
    }

    {  // Hair STYLE round-trips at the current version.
        OutfitLibrary lib;
        const int     idx = lib.Create("Styled");
        CHECK(idx >= 0);
        if (idx >= 0) {
            auto* o = lib.At(static_cast<std::size_t>(idx));
            CHECK(o != nullptr);
            if (o) {
                o->hairStyle = StyleRefKey{ "ApachiiSkyHair.esm", 0x0012C4u };
            }
        }
        OutfitLibrary back;
        CHECK(Decode(Encode(lib), kCodecVersion, back));
        CHECK(back.Count() == 1);
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            CHECK(o->hairStyle.modName == "ApachiiSkyHair.esm");
            CHECK(o->hairStyle.localFormID == 0x0012C4u);
        }
    }

    {  // v8 dye round trip
        OutfitLibrary lib;
        const int idx = lib.Create("Dyed");
        CHECK(idx == 0);
        auto* o = lib.At(0);
        o->SetStyle(2, StyleRefKey{ "Skyrim.esm", 0x12345 });
        o->SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 200, 100, 50 });
        o->SetDye(2, DyeChannelId::kAccent, DyeChannel{ true, 1, 2, 3 });
        o->SetDye(7, DyeChannelId::kSecondary, DyeChannel{ true, 9, 8, 7 });
        lib.Activate(0);

        const auto bytes = Encode(lib);
        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        CHECK(back.Count() == 1);
        const auto* r = back.At(0);
        CHECK(r != nullptr);
        if (r) {
            CHECK(r->DyeFor(2).channels[0] == (DyeChannel{ true, 200, 100, 50 }));
            CHECK(r->DyeFor(2).channels[1].set == false);
            CHECK(r->DyeFor(2).channels[2] == (DyeChannel{ true, 1, 2, 3 }));
            CHECK(r->DyeFor(7).channels[1] == (DyeChannel{ true, 9, 8, 7 }));
            CHECK(!r->DyeFor(3).Any());
        }
    }

    {  // v21: head-part slots the load order invented survive a round trip, and
       // the slot NUMBER travels with each reference rather than being implied
       // by position. MEASURED numbers from the dev rig: 32 horns, 110 ears.
        OutfitLibrary lib;
        lib.Create("Horned");
        lib.At(0)->SetCustomHeadPart(110, StyleRefKey{ "ChooeyDintEarsEdit.esp", 0x838 });
        lib.At(0)->SetCustomHeadPart(32, StyleRefKey{ "NK_HornSlider.esp", 0x811 });

        const auto bytes = Encode(lib);
        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        CHECK(back.Count() == 1);
        const auto* r = back.At(0);
        CHECK(r != nullptr);
        if (r) {
            CHECK(r->customHeadParts.size() == 2);
            CHECK(r->CustomHeadPart(110) ==
                  (StyleRefKey{ "ChooeyDintEarsEdit.esp", 0x838 }));
            CHECK(r->CustomHeadPart(32) == (StyleRefKey{ "NK_HornSlider.esp", 0x811 }));
            // A slot this outfit says nothing about reads as empty, which is
            // the leave-it-alone answer rather than a missing entry.
            CHECK(r->CustomHeadPart(106).Empty());
        }
    }

    {  // ONE ENTRY PER SLOT. Setting the same slot twice replaces rather than
       // appends, because two entries for one slot would make the winner depend
       // on write order instead of on anything the player did.
        OutfitLibrary lib;
        lib.Create("Rehorned");
        lib.At(0)->SetCustomHeadPart(32, StyleRefKey{ "NK_HornSlider.esp", 0x811 });
        lib.At(0)->SetCustomHeadPart(32, StyleRefKey{ "NK_HornSlider.esp", 0x999 });
        CHECK(lib.At(0)->customHeadParts.size() == 1);
        CHECK(lib.At(0)->CustomHeadPart(32) ==
              (StyleRefKey{ "NK_HornSlider.esp", 0x999 }));

        // Clearing ERASES, so an outfit that never touched a slot and one that
        // was set and then cleared encode to the same bytes.
        OutfitLibrary untouched;
        untouched.Create("Rehorned");
        lib.At(0)->SetCustomHeadPart(32, StyleRefKey{});
        CHECK(lib.At(0)->customHeadParts.empty());
        CHECK(Encode(lib) == Encode(untouched));
    }

    {  // A v20 record predates the field entirely and decodes to an outfit
       // naming no invented slot, which is what every outfit written before
       // this feature meant.
        OutfitLibrary lib;
        CHECK(lib.Create("PreSlots") >= 0);
        auto bytes = Encode(lib);
        // ⚠ EIGHT, NOT FOUR, SINCE v22. Two counted blocks now sit past the v20
        // boundary and an empty outfit contributes a zero count to each: the v21
        // invented-slot list and the v22 head-part dyes. Dropping only the last
        // one leaves a v21 record and this test would then be pinning the wrong
        // boundary while still passing.
        bytes.resize(bytes.size() - 9);  // exactly v20 bytes

        OutfitLibrary back;
        CHECK(Decode(bytes, 20u, back));
        CHECK(back.Count() == 1);
        const auto* r = back.At(0);
        CHECK(r != nullptr);
        if (r) {
            CHECK(r->customHeadParts.empty());
            CHECK(!r->AnyHeadPartDye());
        }
    }

    {  // v22: a head-part dye survives a round trip, keyed by slot AND part.
       // The shapes are the 2026-08-17 census: a hair contributing nine parts on
       // slot 3 with two of them dyeable, and a horn on invented slot 106.
        OutfitLibrary lib;
        lib.Create("Dyed horns");
        const StyleRefKey acc{ "Tullius Hair 3 SMP.esp", 0x801 };
        const StyleRefKey body{ "Tullius Hair 3 SMP.esp", 0x802 };
        const StyleRefKey horn{ "ED Horns RMIntegration.esp", 0x582D };
        lib.At(0)->SetHeadPartDye(3, acc, DyeChannelId::kPrimary,
                                 DyeChannel{ true, 210, 170, 60 });
        lib.At(0)->SetHeadPartDye(3, body, DyeChannelId::kSecondary,
                                 DyeChannel{ true, 20, 110, 130 });
        lib.At(0)->SetHeadPartDye(106, horn, DyeChannelId::kPrimary,
                                 DyeChannel{ true, 15, 76, 92 });

        const auto bytes = Encode(lib);
        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        CHECK(back.Count() == 1);
        const auto* r = back.At(0);
        CHECK(r != nullptr);
        if (r) {
            CHECK(r->HeadPartDyeCount() == 3);
            // ⚠ TWO PARTS ON ONE SLOT COME BACK AS TWO, which is the whole
            // reason the key is a pair. A slot-keyed record would have merged
            // these into one colour on the way out.
            CHECK(r->HeadPartDyeFor(3, acc).channels[0] ==
                  (DyeChannel{ true, 210, 170, 60 }));
            CHECK(r->HeadPartDyeFor(3, body).channels[1] ==
                  (DyeChannel{ true, 20, 110, 130 }));
            CHECK(r->HeadPartDyeFor(106, horn).channels[0] ==
                  (DyeChannel{ true, 15, 76, 92 }));
            // A part this outfit says nothing about reads as no colour.
            CHECK(!r->HeadPartDyeFor(3, horn).Any());
            CHECK(!r->HeadPartDyeFor(32, horn).Any());
        }
    }

    {  // A v21 record stops before the head-part dyes and decodes to an outfit
       // that colours no head part, which is what every outfit written by the
       // 2026-08-16 and 2026-08-17 builds meant. Those saves exist on the dev
       // rig, so this boundary is a live compatibility case rather than a
       // hypothetical one.
        OutfitLibrary lib;
        CHECK(lib.Create("PreHeadDye") >= 0);
        lib.At(0)->SetCustomHeadPart(106, StyleRefKey{ "ED Horns RMIntegration.esp", 0x582D });
        auto bytes = Encode(lib);
        bytes.resize(bytes.size() - 5);  // drop the v22 count: exactly v21 bytes

        OutfitLibrary back;
        CHECK(Decode(bytes, 21u, back));
        CHECK(back.Count() == 1);
        const auto* r = back.At(0);
        CHECK(r != nullptr);
        if (r) {
            CHECK(!r->AnyHeadPartDye());
            // ...and the invented slot it DOES name still arrives, so the
            // boundary drops one field rather than the record.
            CHECK(r->CustomHeadPart(106) ==
                  (StyleRefKey{ "ED Horns RMIntegration.esp", 0x582D }));
        }
    }

    {  // Clearing a head-part dye ERASES its entry, so an outfit that never
       // dyed a part and one that was dyed and cleared encode to the same bytes.
       // The same rule the invented-slot list above is pinned on.
        OutfitLibrary lib;
        lib.Create("Cleared");
        const StyleRefKey horn{ "NK_HornSlider.esp", 0x81F };
        lib.At(0)->SetHeadPartDye(32, horn, DyeChannelId::kPrimary,
                                 DyeChannel{ true, 1, 2, 3 });
        lib.At(0)->ClearHeadPartDye(32, horn);

        OutfitLibrary untouched;
        untouched.Create("Cleared");
        CHECK(Encode(lib) == Encode(untouched));
    }

    {  // ⚠ CANONICAL ORDER ON THE WIRE. Two outfits given the same colours in a
       // different order must encode to the SAME bytes, or the golden layout is
       // whichever swatch a fixture happened to click first.
        const StyleRefKey acc{ "H.esp", 0x801 };
        const StyleRefKey body{ "H.esp", 0x802 };
        const StyleRefKey horn{ "ED.esp", 0x582D };

        OutfitLibrary one;
        one.Create("Order");
        one.At(0)->SetHeadPartDye(3, acc, DyeChannelId::kPrimary, DyeChannel{ true, 9, 9, 9 });
        one.At(0)->SetHeadPartDye(3, body, DyeChannelId::kPrimary, DyeChannel{ true, 8, 8, 8 });
        one.At(0)->SetHeadPartDye(106, horn, DyeChannelId::kPrimary,
                                 DyeChannel{ true, 7, 7, 7 });

        OutfitLibrary other;
        other.Create("Order");
        other.At(0)->SetHeadPartDye(106, horn, DyeChannelId::kPrimary,
                                   DyeChannel{ true, 7, 7, 7 });
        other.At(0)->SetHeadPartDye(3, body, DyeChannelId::kPrimary,
                                   DyeChannel{ true, 8, 8, 8 });
        other.At(0)->SetHeadPartDye(3, acc, DyeChannelId::kPrimary,
                                   DyeChannel{ true, 9, 9, 9 });

        CHECK(Encode(one) == Encode(other));
    }

    {  // ⚠ A COUNT BEYOND THE BOUND IS REFUSED BEFORE ANYTHING IS RESERVED.
       // A corrupt record can claim any count, and reserving on it commits the
       // allocation the record CLAIMS rather than the one it carries.
       //
       // ⚠ THE COUNT HERE IS ENORMOUS ON PURPOSE, AND A SMALL ONE MADE THIS
       // TEST VACUOUS. At kMaxCustomHeadParts + 1 the decode returns false
       // whether the bound exists or not, because the entry read runs off the
       // end of the buffer a line later and fails there instead. Only a count
       // large enough to make the reserve itself ruinous separates the two.
        OutfitLibrary lib;
        CHECK(lib.Create("Corrupt") >= 0);
        auto bytes = Encode(lib);
        // Overwrite the trailing count with one no allocator should be asked for.
        const std::uint32_t tooMany = 0xFFFFFFF0u;
        bytes[bytes.size() - 4] = static_cast<std::byte>(tooMany & 0xFF);
        bytes[bytes.size() - 3] = static_cast<std::byte>((tooMany >> 8) & 0xFF);
        bytes[bytes.size() - 2] = static_cast<std::byte>((tooMany >> 16) & 0xFF);
        bytes[bytes.size() - 1] = static_cast<std::byte>((tooMany >> 24) & 0xFF);

        OutfitLibrary back;
        CHECK(!Decode(bytes, kCodecVersion, back));
    }

    {  // an outfit with no dye still round trips
        OutfitLibrary lib;
        lib.Create("Plain");
        lib.At(0)->SetStyle(1, StyleRefKey{ "A.esp", 0x800 });
        const auto bytes = Encode(lib);
        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        const auto* r = back.At(0);
        CHECK(r != nullptr);
        if (r) {
            CHECK(!r->AnyDye());
        }
    }

    {  // forward tolerance: a dye entry whose slot bit is out of range is
       // consumed and lands nowhere - the stream stays aligned and the other
       // dyed slots survive. Hand-built bytes, since Encode() would never
       // emit an invalid bit itself. This pins the decode-site tolerance
       // (SetDye's bounds guard doing the ignoring), which the armor block
       // deliberately does NOT share: harmonizing the two loops breaks here.
        std::vector<std::byte> buf;
        PutLE32(buf, 1);                     // outfit count = 1
        PutLE32(buf, 1);                     // active = outfit 0
        PutLE32(buf, 4);                     // name length = 4
        buf.push_back(std::byte{ 'D' });
        buf.push_back(std::byte{ 'y' });
        buf.push_back(std::byte{ 'e' });
        buf.push_back(std::byte{ 'd' });
        buf.push_back(std::byte{ 0 });       // favorite = false
        PutLE32(buf, 0);                     // armor slot count = 0
        PutLE32(buf, 0);                     // weapon entry count = 0
        PutLE32(buf, 0);                     // empty preset name (v3)
        buf.push_back(std::byte{ 0 });       // ORefitMode::kDefault (v3)
        PutLE32(buf, 0);                     // no per-hand overrides (v4)
        buf.push_back(std::byte{ 0 });       // HairMode::kAuto (v5)
        buf.push_back(std::byte{ 0 });       // hairTint disabled (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.r (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.g (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.b (v6)
        PutLE32(buf, 0);                     // hairStyle: empty modName (v7)
        PutLE32(buf, 0);                     // hairStyle: localFormID (v7)

        PutLE32(buf, 2);                     // dye entry count = 2 (v8, v9)
        // Written from kDyeChannelCount rather than a literal, so widening the
        // channels again keeps this test honest instead of quietly measuring
        // the wrong number of bytes.
        // entry 0: bit 0xFFFFFFFF - out of range, must be consumed and ignored
        PutLE32(buf, 0xFFFFFFFFu);
        PutLE32(buf, 0);                     // garment: empty modName (v24)
        PutLE32(buf, 0);                     // garment: localFormID (v24)
        PutLE32(buf, static_cast<std::uint32_t>(kDyeChannelCount));
        for (std::size_t c = 0; c < kDyeChannelCount; ++c) {   // all channels set
            buf.push_back(std::byte{ 1 });
            buf.push_back(std::byte{ 9 });
            buf.push_back(std::byte{ 9 });
            buf.push_back(std::byte{ 9 });
            buf.push_back(std::byte{ 255 });  // strength (v13)
            buf.push_back(std::byte{ 0 });    // mode (v14)
            buf.push_back(std::byte{ 0 });    // secondSet (v14)
            buf.push_back(std::byte{ 0 });    // r2 (v14)
            buf.push_back(std::byte{ 0 });    // g2 (v14)
            buf.push_back(std::byte{ 0 });    // b2 (v14)
            buf.push_back(std::byte{ 0 });    // flake (v15)
            for (int fb = 0; fb < 2; ++fb) {  // palette then player (v15)
                buf.push_back(std::byte{ 0 });    // sheenSet
                buf.push_back(std::byte{ 0 });    // sheenR
                buf.push_back(std::byte{ 0 });    // sheenG
                buf.push_back(std::byte{ 0 });    // sheenB
                buf.push_back(std::byte{ 0 });    // glossSet
                buf.push_back(std::byte{ 128 });  // gloss
            }
            buf.push_back(std::byte{ 0 });    // blend: defer (v19)
            buf.push_back(std::byte{ 128 });  // cut: halfway (v25)
        }
        // entry 1: a valid primary dye on bit 2. The garment key is EMPTY, and
        // it has to be: this forge writes an armor slot count of 0, so bit 2
        // holds no style and an empty key is what DyeFor(2) resolves against.
        PutLE32(buf, 2);
        PutLE32(buf, 0);                     // garment: empty modName (v24)
        PutLE32(buf, 0);                     // garment: localFormID (v24)
        PutLE32(buf, static_cast<std::uint32_t>(kDyeChannelCount));
        buf.push_back(std::byte{ 1 });       // primary set
        buf.push_back(std::byte{ 200 });
        buf.push_back(std::byte{ 100 });
        buf.push_back(std::byte{ 50 });
        buf.push_back(std::byte{ 255 });     // strength (v13)
        buf.push_back(std::byte{ 0 });       // mode (v14)
        buf.push_back(std::byte{ 0 });       // secondSet (v14)
        buf.push_back(std::byte{ 0 });       // r2 (v14)
        buf.push_back(std::byte{ 0 });       // g2 (v14)
        buf.push_back(std::byte{ 0 });       // b2 (v14)
        buf.push_back(std::byte{ 0 });       // flake (v15)
        for (int fb = 0; fb < 2; ++fb) {     // palette then player (v15)
            buf.push_back(std::byte{ 0 });
            buf.push_back(std::byte{ 0 });
            buf.push_back(std::byte{ 0 });
            buf.push_back(std::byte{ 0 });
            buf.push_back(std::byte{ 0 });
            buf.push_back(std::byte{ 128 });
        }
        buf.push_back(std::byte{ 0 });       // blend: defer (v19)
        buf.push_back(std::byte{ 128 });     // cut: halfway (v25)
        for (std::size_t c = 1; c < kDyeChannelCount; ++c) {   // the rest off
            buf.push_back(std::byte{ 0 });
            buf.push_back(std::byte{ 0 });
            buf.push_back(std::byte{ 0 });
            buf.push_back(std::byte{ 0 });
            buf.push_back(std::byte{ 255 });  // strength (v13)
            buf.push_back(std::byte{ 0 });    // mode (v14)
            buf.push_back(std::byte{ 0 });    // secondSet (v14)
            buf.push_back(std::byte{ 0 });    // r2 (v14)
            buf.push_back(std::byte{ 0 });    // g2 (v14)
            buf.push_back(std::byte{ 0 });    // b2 (v14)
            buf.push_back(std::byte{ 0 });    // flake (v15)
            for (int fb = 0; fb < 2; ++fb) {  // palette then player (v15)
                buf.push_back(std::byte{ 0 });
                buf.push_back(std::byte{ 0 });
                buf.push_back(std::byte{ 0 });
                buf.push_back(std::byte{ 0 });
                buf.push_back(std::byte{ 0 });
                buf.push_back(std::byte{ 128 });
            }
            buf.push_back(std::byte{ 0 });    // blend: defer (v19)
            buf.push_back(std::byte{ 128 });  // cut: halfway (v25)
        }
        PutLE32(buf, 0);                     // weapon dye: none (v10)
        PutLE32(buf, 0);                     // custom body ID: empty (v11)
        PutLE32(buf, 0);            // eyes: empty modName (v12)
        PutLE32(buf, 0);            // eyes: localFormID (v12)
        PutLE32(buf, 0);            // brows: empty modName (v12)
        PutLE32(buf, 0);            // brows: localFormID (v12)
        PutLE32(buf, 0);            // facial hair: empty modName (v16)
        PutLE32(buf, 0);            // facial hair: localFormID (v16)
        buf.push_back(std::byte{ 0 });  // eyeTint disabled (v17)
        buf.push_back(std::byte{ 0 });  // eyeTint.r (v17)
        buf.push_back(std::byte{ 0 });  // eyeTint.g (v17)
        buf.push_back(std::byte{ 0 });  // eyeTint.b (v17)
        buf.push_back(std::byte{ 0 });  // scleraTint disabled (v18)
        buf.push_back(std::byte{ 0 });  // scleraTint.r (v18)
        buf.push_back(std::byte{ 0 });  // scleraTint.g (v18)
        buf.push_back(std::byte{ 0 });  // scleraTint.b (v18)
        buf.push_back(std::byte{ 0 });  // eyeBlend: defer (v19)
        buf.push_back(std::byte{ 0 });  // eyeTint2 disabled (v20)
        buf.push_back(std::byte{ 0 });  // eyeTint2.r (v20)
        buf.push_back(std::byte{ 0 });  // eyeTint2.g (v20)
        buf.push_back(std::byte{ 0 });  // eyeTint2.b (v20)
        buf.push_back(std::byte{ 0 });  // invented head-part slots: none (v21)
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });  // head-part dyes: none (v22)
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });
        buf.push_back(std::byte{ 0 });  // push-up: none (v23)

        OutfitLibrary back;
        CHECK(Decode(buf, kCodecVersion, back));  // aligned: the trailing-garbage
                                                  // check passes only if the bad
                                                  // entry consumed its count AND
                                                  // all of its channel bytes
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            CHECK(o->DyeFor(2).channels[0] == (DyeChannel{ true, 200, 100, 50 }));
            CHECK(o->AnyDye());              // the valid entry landed
        }
    }

    {  // a dye count larger than the slot space is refused outright - a
       // same-version writer can never emit more than kBitCount entries, so
       // a bigger count is corruption, not data.
        std::vector<std::byte> buf;
        PutLE32(buf, 1);                     // outfit count = 1
        PutLE32(buf, 0);                     // no active outfit
        PutLE32(buf, 1);                     // name length = 1
        buf.push_back(std::byte{ 'X' });
        buf.push_back(std::byte{ 0 });       // favorite = false
        PutLE32(buf, 0);                     // armor slot count = 0
        PutLE32(buf, 0);                     // weapon entry count = 0
        PutLE32(buf, 0);                     // empty preset name (v3)
        buf.push_back(std::byte{ 0 });       // ORefitMode::kDefault (v3)
        PutLE32(buf, 0);                     // no per-hand overrides (v4)
        buf.push_back(std::byte{ 0 });       // HairMode::kAuto (v5)
        buf.push_back(std::byte{ 0 });       // hairTint disabled (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.r (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.g (v6)
        buf.push_back(std::byte{ 0 });       // hairTint.b (v6)
        PutLE32(buf, 0);                     // hairStyle: empty modName (v7)
        PutLE32(buf, 0);                     // hairStyle: localFormID (v7)
        PutLE32(buf, kBitCount + 1);         // dye count = 33: refuse

        OutfitLibrary back;
        CHECK(!Decode(buf, kCodecVersion, back));
    }

    {  // truncation INSIDE a dye entry, mid-channel, fails cleanly. The
       // per-read r.ok bailouts inside the channel loop are what catch this;
       // hoisting them out of the loop keeps every other test green and
       // breaks exactly here.
        OutfitLibrary lib;
        lib.Create("Dyed");
        auto* o = lib.At(0);
        CHECK(o != nullptr);
        if (o) {
            o->SetDye(2, DyeChannelId::kPrimary, DyeChannel{ true, 200, 100, 50 });
        }
        auto bytes = Encode(lib);
        // ⚠ 7, NOT 3: the empty v10 weapon dye count is 4 bytes and now sits
        // AFTER the dye channels, so a 3-byte cut lands in that count and this
        // test would still pass while no longer testing anything about a
        // mid-channel truncation. The number has to move with every field
        // appended after the dye block.
        // ⚠ THIS WENT STALE ONCE ALREADY AND THE TEST STAYED GREEN THROUGH IT.
        // At 43 the tail was 45 bytes (v10 weapon 4, v11 ID 4, v12 refs 16, v16
        // facial 8, v17 eye 4, v18 sclera 4, v19 blend 1, v20 second eye 4), so
        // the cut stopped two bytes INTO the weapon dye count and Decode failed
        // on that U32 rather than inside GetChannel. It still returned false,
        // so the assertion passed while testing nothing it was written for.
        // 45 + 3 lands three bytes into the last channel again.
        bytes.resize(bytes.size() - 53);     // the 49-byte tail (v22 head dye included), plus 3 into the last channel's RGB

        OutfitLibrary back;
        CHECK(!Decode(bytes, kCodecVersion, back));
    }

    {  // ⚠ DROP-PATH ALIGNMENT at v6. A two-entry v6 stream whose FIRST entry's
       // inner library is undecodable: that entry is dropped, and the drop path
       // must consume every trailing per-entry field the success path does, or
       // the reader is left mid-record and every later follower is lost.
       //
       // Entry 1's tail is 45 bytes: obodyCaptured u32 (4) + "Dropped Preset"
       // str (4 + 14) + hairCaptured u32 (4) + "Dropped.esp" str (4 + 11) +
       // hairLocalID u32 (4). The strings are deliberately NON-empty so a
       // short-consuming drop path lands mid-string and fails hard, instead of
       // a run of zeroes happening to re-parse as a plausible next entry.
       //
       // Entry 2 carries its OWN key and its own hairBaselineLocalID: asserting
       // both is what proves the reader resumed at exactly the right byte, not
       // merely that it resumed somewhere that still parsed.
        OutfitLibrary goodB;
        CHECK(goodB.Create("Bravo") == 0);
        if (auto* o = goodB.At(0)) {
            o->SetHide(kBitFeet);
        }
        const auto innerB = Encode(goodB);

        // Undecodable inner: a count above kMaxOutfits, which OS::Decode
        // rejects outright. Only 8 bytes long, and innerLen says so, so the
        // OUTER stream stays aligned across the skip on its own.
        std::vector<std::byte> corruptInner;
        PutLE32(corruptInner, static_cast<std::uint32_t>(kMaxOutfits) + 1);
        PutLE32(corruptInner, 0);

        std::vector<std::byte> buf;
        PutLE32(buf, 2);  // outer entry count

        PutStrV1(buf, "Broken.esp");
        PutLE32(buf, 0x222);
        PutLE32(buf, static_cast<std::uint32_t>(corruptInner.size()));
        buf.insert(buf.end(), corruptInner.begin(), corruptInner.end());
        PutLE32(buf, 1);                        // obodyBaselineCaptured
        PutStrV1(buf, "Dropped Preset");        // obodyBaseline
        PutLE32(buf, 1);                        // hairBaselineCaptured
        PutStrV1(buf, "Dropped.esp");           // hairBaselineMod
        PutLE32(buf, 0x0BADu);                  // hairBaselineLocalID

        PutStrV1(buf, "Skyrim.esm");
        PutLE32(buf, 0x333);
        PutLE32(buf, static_cast<std::uint32_t>(innerB.size()));
        buf.insert(buf.end(), innerB.begin(), innerB.end());
        PutLE32(buf, 1);                        // obodyBaselineCaptured
        PutStrV1(buf, "Preset C");              // obodyBaseline
        PutLE32(buf, 1);                        // hairBaselineCaptured
        PutStrV1(buf, "Hair.esp");              // hairBaselineMod
        PutLE32(buf, 0xBEEFu);                  // hairBaselineLocalID

        NpcAssignmentMap out;
        CHECK(DecodeNpcAssignments(buf, kNpcRecordVersion, out));
        CHECK(out.size() == 1);
        CHECK(out.find(NpcKey{ "Broken.esp", 0x222 }) == out.end());

        const auto it = out.find(NpcKey{ "Skyrim.esm", 0x333 });
        CHECK(it != out.end());
        if (it != out.end()) {
            CHECK(it->second.library.Count() == 1);
            const auto* o = it->second.library.At(0);
            CHECK(o != nullptr);
            if (o) {
                CHECK(o->name == "Bravo");
            }
            CHECK(it->second.obodyBaseline == "Preset C");
            CHECK(it->second.hairBaselineCaptured);
            CHECK(it->second.hairBaselineMod == "Hair.esp");
            CHECK(it->second.hairBaselineLocalID == 0xBEEFu);
        }
    }

    {  // 'HCOL' round trip: the player's captured hair colour baseline
       // survives encode/decode intact. This is the pure logic behind
       // Persistence.cpp's SaveCallback/LoadCallback HCOL block, which cannot
       // itself compile into this pure test target (it talks to the live
       // SKSE serialization interface).
        const auto bytes = EncodeHairBaseline("Skyrim.esm", 0x0EB60Fu);

        std::string   modName;
        std::uint32_t localFormID = 0;
        CHECK(DecodeHairBaseline(bytes, modName, localFormID));
        CHECK(modName == "Skyrim.esm");
        CHECK(localFormID == 0x0EB60Fu);
    }

    {  // Truncated bytes must be refused, not half-applied. Cutting the
       // trailing form ID off leaves a coherent string but no ID; cutting
       // into the string body itself must fail the Reader's own Need() guard.
        const auto full = EncodeHairBaseline("Skyrim.esm", 0x0EB60Fu);

        auto missingID = full;
        missingID.resize(missingID.size() - 4);  // drop the trailing U32 whole
        std::string   modName;
        std::uint32_t localFormID = 0;
        CHECK(!DecodeHairBaseline(missingID, modName, localFormID));

        auto missingIDByte = full;
        missingIDByte.resize(missingIDByte.size() - 1);  // one byte short of the ID
        CHECK(!DecodeHairBaseline(missingIDByte, modName, localFormID));

        auto truncatedString = full;
        truncatedString.resize(6);  // length prefix claims 10 bytes, only 2 follow
        CHECK(!DecodeHairBaseline(truncatedString, modName, localFormID));

        CHECK(!DecodeHairBaseline({}, modName, localFormID));  // empty input
    }

    {  // Trailing garbage after an otherwise well-formed pair is refused too,
       // matching Decode/DecodeNpcAssignments's own end-of-stream check: this
       // record's bytes are not a longer stream that a partial parse could
       // legitimately stop short of, so any leftover byte is corruption.
        auto bytes = EncodeHairBaseline("Skyrim.esm", 0x0EB60Fu);
        bytes.push_back(std::byte{ 0xFF });
        std::string   modName;
        std::uint32_t localFormID = 0;
        CHECK(!DecodeHairBaseline(bytes, modName, localFormID));
    }

    {  // An empty mod name decodes to "not captured" even though the bytes
       // parse cleanly (r.ok stays true and every byte is consumed) - it is
       // the same rule HairColor::Resolve applies to a captured value it is
       // asked to resolve, so a record that somehow claimed an empty plugin
       // name can never be treated as a real captured original.
        const auto bytes = EncodeHairBaseline("", 0x1234u);
        std::string   modName;
        std::uint32_t localFormID = 0;
        CHECK(!DecodeHairBaseline(bytes, modName, localFormID));
    }

    {  // 'HPBS' round trip: the player's captured pre-Fitting-Room head parts,
       // one row per slot. The style counterpart of the 'HCOL' block above,
       // and the reason it exists is in PersistenceCodec.h: the capture used
       // to be session state keyed on a form id that is 0x14 in every save.
        const std::vector<HeadPartBaselineRow> rows{
            { 0u, "Skyrim.esm", 0x0EB60Fu },      // hair
            { 2u, "Skyrim.esm", 0x0D2004u },      // eyes
            { 110u, "Chooey.esp", 0x000801u },    // a slot this load order invented
        };
        const auto bytes = EncodeHeadPartBaseline(rows);

        std::vector<HeadPartBaselineRow> out;
        CHECK(DecodeHeadPartBaseline(bytes, out));
        CHECK(out == rows);
    }

    {  // No captures at all is a legal record rather than a malformed one: a
       // character who has had nothing changed still round trips, and decodes
       // to an empty list rather than to a failure.
        const auto bytes = EncodeHeadPartBaseline({});
        std::vector<HeadPartBaselineRow> out{ { 7u, "Stale.esp", 1u } };
        CHECK(DecodeHeadPartBaseline(bytes, out));
        CHECK(out.empty());
    }

    {  // Truncation, trailing garbage and an empty plugin name are all refused
       // WHOLE. Half-adopting a baseline is how a character ends up with
       // someone else's face in one slot and their own in the next.
        const std::vector<HeadPartBaselineRow> rows{ { 0u, "Skyrim.esm", 0x0EB60Fu },
                                                     { 2u, "Skyrim.esm", 0x0D2004u } };
        const auto full = EncodeHeadPartBaseline(rows);

        std::vector<HeadPartBaselineRow> out;
        auto missingID = full;
        missingID.resize(missingID.size() - 4);  // drop the last row's form id
        CHECK(!DecodeHeadPartBaseline(missingID, out));

        auto cutMidString = full;
        cutMidString.resize(10);  // into the first row's plugin name
        CHECK(!DecodeHeadPartBaseline(cutMidString, out));

        auto trailing = full;
        trailing.push_back(std::byte{ 0xFF });
        CHECK(!DecodeHeadPartBaseline(trailing, out));

        CHECK(!DecodeHeadPartBaseline({}, out));  // not even a count

        // An empty name that still claims a form id can never resolve, so it
        // is a fault; the (empty, 0) shape below is the one exception.
        const auto emptyName = EncodeHeadPartBaseline({ { 0u, "", 0x1234u } });
        CHECK(!DecodeHeadPartBaseline(emptyName, out));
    }

    {  // The "nothing" row: an empty plugin name with a zero form id means the
       // player had NO part in that slot before Fitting Room touched it. It
       // round trips beside real rows, because RestoreSlot wears the slot's
       // placeholder for it and a reload would otherwise forget that there was
       // ever nothing there.
        const std::vector<HeadPartBaselineRow> rows{
            { 106u, "", 0u },                     // horns: they had none
            { 110u, "Chooey.esp", 0x000801u },    // ears: they had this one
        };
        const auto bytes = EncodeHeadPartBaseline(rows);
        std::vector<HeadPartBaselineRow> out;
        CHECK(DecodeHeadPartBaseline(bytes, out));
        CHECK(out == rows);
    }

    {  // A count past the cap is refused before a single row is read, so a
       // corrupt record cannot make the decoder reserve or walk an absurd
       // list. Same guard kMaxCustomHeadParts gives the outfit codec.
        std::vector<std::byte> bytes;
        const std::uint32_t    absurd = kMaxHeadPartBaselineRows + 1u;
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<std::byte>((absurd >> (i * 8)) & 0xFFu));
        }
        std::vector<HeadPartBaselineRow> out;
        CHECK(!DecodeHeadPartBaseline(bytes, out));
    }

    {  // 'RULE' record (Task 10): a full round trip of every field, including
       // a rulesJson blob well past the 512-byte kMaxStringLen cap that the
       // shared Str()/PutStr helpers enforce elsewhere in this file. This
       // pins an actual bug the first draft of EncodeRuleState/DecodeRuleState
       // had: routing rulesJson through PutStr/Str (sized for short fields
       // like mod names) would silently reject any save with more than a
       // handful of rules. rulesJson gets its own length prefix specifically
       // so a real rule set is never bound by that cap.
        // ---- 'SEXF', the sex a look states --------------------------------
        {  // Both values round-trip, and FALSE is a real answer rather than an
           // absent record: "this look says male" and "we were never told" are
           // different states and only the second may leave the base alone.
            bool female = true;
            CHECK(DecodeCharacterSex(EncodeCharacterSex(false), female));
            CHECK(!female);
            CHECK(DecodeCharacterSex(EncodeCharacterSex(true), female));
            CHECK(female);
        }
        {  // Empty and over-long are both refused, and a_female is untouched:
           // guessing here puts a character in the wrong body.
            bool female = true;
            CHECK(!DecodeCharacterSex(std::span<const std::byte>{}, female));
            CHECK(female);
            auto bytes = EncodeCharacterSex(false);
            bytes.push_back(std::byte{ 0x00 });
            CHECK(!DecodeCharacterSex(bytes, female));
            CHECK(female);
        }

        // ---- 'FTNT', the export a look's baked face lives in ---------------
        {  // Round trip, including the space and the FR_ prefix a real jslot
           // carries. The name is what the late-rebake gate compares against,
           // so a codec that mangled either end would make a legitimate look
           // read as residue and get its own face rebaked away.
            std::string jslot;
            CHECK(DecodeLookFaceTint(EncodeLookFaceTint("FR_Umbrael 17"), jslot));
            CHECK(jslot == "FR_Umbrael 17");
        }
        {  // Empty is REFUSED rather than round-tripped, and that is the whole
           // convention: absence is carried by the record not being written,
           // exactly as 'SKTN' and 'SEXF' carry theirs. An empty jslot names no
           // file, so a record holding one is malformed, and accepting it would
           // hand HeadBuildHook a path of "Textures\\CharGen\\Exported\\.dds" to
           // bind every build for the rest of the session.
            std::string jslot = "untouched";
            CHECK(!DecodeLookFaceTint(EncodeLookFaceTint(""), jslot));
        }
        {  // Truncated and over-long are both refused. a_jslot is not trusted
           // on false, so the test only asserts the verdict.
            std::string jslot;
            CHECK(!DecodeLookFaceTint(std::span<const std::byte>{}, jslot));
            auto bytes = EncodeLookFaceTint("FR_Nord 3");
            bytes.push_back(std::byte{ 0x00 });
            CHECK(!DecodeLookFaceTint(bytes, jslot));
        }

        // ---- 'SKTN', the held skin tone -----------------------------------
        {  // Round trip, including the strength quantised to a byte.
            std::uint8_t r = 0, g = 0, b = 0;
            float        s = 0.0f;
            CHECK(DecodeSkinToneHold(EncodeSkinToneHold(0, 63, 97, 0.655f), r, g, b, s));
            CHECK(r == 0 && g == 63 && b == 97);
            CHECK(s > 0.65f && s < 0.66f);  // 167/255, the byte it survives as
        }
        {  // The ends hold exactly, because 0 and 1 are the two a user actually
           // sets and a rounding slip at either would be visible on the body.
            std::uint8_t r = 0, g = 0, b = 0;
            float        s = -1.0f;
            CHECK(DecodeSkinToneHold(EncodeSkinToneHold(255, 255, 255, 1.0f), r, g, b, s));
            CHECK(s == 1.0f);
            CHECK(DecodeSkinToneHold(EncodeSkinToneHold(0, 0, 0, 0.0f), r, g, b, s));
            CHECK(s == 0.0f);
        }
        {  // Out-of-range strength is clamped at WRITE time, so a record can
           // never be written in a shape its own decoder would have to guess
           // about. Same discipline PutStr follows for over-long fields.
            std::uint8_t r = 0, g = 0, b = 0;
            float        s = 0.0f;
            CHECK(DecodeSkinToneHold(EncodeSkinToneHold(1, 2, 3, 9.0f), r, g, b, s));
            CHECK(s == 1.0f);
            CHECK(DecodeSkinToneHold(EncodeSkinToneHold(1, 2, 3, -9.0f), r, g, b, s));
            CHECK(s == 0.0f);
        }
        {  // Truncated and over-long records are both refused, and the outputs
           // are left alone: a half-read tone is a body wearing a colour nobody
           // chose.
            auto bytes = EncodeSkinToneHold(10, 20, 30, 0.5f);
            bytes.pop_back();
            std::uint8_t r = 111, g = 111, b = 111;
            float        s = 0.25f;
            CHECK(!DecodeSkinToneHold(bytes, r, g, b, s));
            CHECK(r == 111 && g == 111 && b == 111 && s == 0.25f);

            auto extra = EncodeSkinToneHold(10, 20, 30, 0.5f);
            extra.push_back(std::byte{ 0x01 });
            CHECK(!DecodeSkinToneHold(extra, r, g, b, s));
            CHECK(r == 111 && s == 0.25f);
        }

        // ---- 'OVLB', the player's overlay art -----------------------------
        {  // ⚠ THE SAME BLOB REASONING AS rulesJson BELOW, and the same trap.
           // A populated overlays block is a JSON document with a texture path
           // per layer, so it passes 512 bytes without trying and PutStr would
           // silently refuse the save. This pins the length prefix.
            const std::string json(1500, 'j');
            std::string       back;
            CHECK(DecodeOverlayBaseline(EncodeOverlayBaseline(json), back));
            CHECK(back == json);
            CHECK(back.size() == 1500);
        }
        {  // ⚠ EMPTY DECODES SUCCESSFULLY and is not the same as an absent
           // record: "recorded, and it was empty" versus "a save from before
           // this build". The decoder must not conflate them by failing.
            std::string back = "not touched yet";
            CHECK(DecodeOverlayBaseline(EncodeOverlayBaseline(std::string{}), back));
            CHECK(back.empty());
        }
        {  // Truncated bytes are refused, and a_out is left alone. Half a
           // record is worse than none: none is a state the load path already
           // handles, half is a character wearing part of somebody's art.
            auto bytes = EncodeOverlayBaseline("{\"overlays\":{}}");
            CHECK(bytes.size() > 4);
            bytes.resize(bytes.size() - 3);
            std::string back = "untouched";
            CHECK(!DecodeOverlayBaseline(bytes, back));
            CHECK(back == "untouched");
        }
        {  // Trailing garbage is refused for the same reason: a record that
           // decodes "successfully" while ignoring bytes it did not understand
           // is how a future version silently loses half its content.
            auto bytes = EncodeOverlayBaseline("{}");
            bytes.push_back(std::byte{ 0x7F });
            std::string back = "untouched";
            CHECK(!DecodeOverlayBaseline(bytes, back));
            CHECK(back == "untouched");
        }

        RuleRecordFields fields;
        fields.rulesJson         = std::string(800, 'x');  // > kMaxStringLen (512)
        fields.engineEnabled     = false;
        fields.pinned            = true;
        fields.pinnedName        = "Fancy Dress";
        fields.lastAppliedRuleId = "r-7f3a";
        fields.disabledPackIds   = { "pack-a", "pack-b", "pack-c" };

        const auto       bytes = EncodeRuleState(fields);
        RuleRecordFields back;
        CHECK(DecodeRuleState(bytes, back));
        CHECK(back == fields);
        CHECK(back.rulesJson.size() == 800);
    }

    {  // Defaults (no pin, engine on, empty rules) round-trip too - the shape
       // a save with an EXPLICIT but otherwise-empty 'RULE' record carries,
       // distinct from having no record at all.
        RuleRecordFields fields;  // all defaults
        RuleRecordFields back;
        CHECK(DecodeRuleState(EncodeRuleState(fields), back));
        CHECK(back == fields);
        CHECK(!back.pinned);
        CHECK(back.pinnedName.empty());
    }

    {  // ⚠ THE PROBE: a >512-char pinnedName used to encode cleanly and
       // then be REFUSED by Reader::Str's kMaxStringLen check -
       // DecodeRuleState returned false and the save lost its entire rule
       // set AND its pin, even though only the pin name was too long.
       // PutStr now clamps at WRITE time, so a record can no longer be
       // written in a shape its own decoder rejects: this decodes cleanly,
       // with the over-long field truncated rather than poisoning
       // everything else riding in the same record.
        RuleRecordFields fields;
        fields.rulesJson  = "{\"version\":1,\"rules\":[]}";
        fields.pinned     = true;
        fields.pinnedName = std::string(600, 'p');  // > kMaxStringLen (512)

        const auto       bytes = EncodeRuleState(fields);
        RuleRecordFields back;
        CHECK(DecodeRuleState(bytes, back));
        CHECK(back.pinned);
        CHECK(back.pinnedName.size() == 512);  // clamped, not lost
    }

    {  // The idCount > 4096 guard is the ONLY hard allocation limit
       // standing between a corrupted length prefix and
       // disabledPackIds.reserve() attempting to allocate space for
       // billions of strings. Hand-built bytes (a valid rulesJson blob,
       // engine/pin bytes and two empty strings, then an absurd idCount)
       // confirm DecodeRuleState refuses rather than reserving.
        std::vector<std::byte> buf;
        detail::PutBlob(buf, "{\"version\":1,\"rules\":[]}");  // rulesJson
        detail::PutU8(buf, 1);              // engineEnabled = true
        detail::PutU8(buf, 0);              // pinned = false
        detail::PutStr(buf, "");            // pinnedName
        detail::PutStr(buf, "");            // lastAppliedRuleId
        detail::PutU32(buf, 0xFFFFFFFFu);   // idCount: absurd

        RuleRecordFields out;
        CHECK(!DecodeRuleState(buf, out));
    }

    {  // A 'RULE' record that fails to decode (empty, garbage, or a length
       // prefix claiming more than is present) must never partially write
       // its output: a false return has to be trustworthy on its own,
       // without the caller also needing to know which fields got mutated
       // before the corruption was discovered. This is the codec's OWN
       // contract, pinned here independently of any caller - it is NOT
       // what makes Persistence::LoadCallback clear the pin on a missing or
       // rejected record; that guarantee comes from LoadCallback's
       // haveRuleRecord staying false, so it never even looks at a
       // partially-decoded RuleRecordFields in the first place (see the
       // LoadCallback comment in Persistence.cpp). Keeping the two
       // guarantees straight matters: conflating them is how a real
       // load-path regression later gets waved through on "that's tested".
        RuleRecordFields seed;
        seed.pinned     = true;
        seed.pinnedName = "should not survive a failed decode";

        RuleRecordFields empty = seed;
        CHECK(!DecodeRuleState(std::vector<std::byte>{}, empty));
        CHECK(empty.pinned);
        CHECK(empty.pinnedName == "should not survive a failed decode");

        RuleRecordFields garbage = seed;
        const std::vector<std::byte> junk{ std::byte{ 0xFF }, std::byte{ 0xFF },
                                           std::byte{ 0xFF } };
        CHECK(!DecodeRuleState(junk, garbage));
        CHECK(garbage.pinned);
        CHECK(garbage.pinnedName == "should not survive a failed decode");

        RuleRecordFields       truncated = seed;
        std::vector<std::byte> claimTooLong;
        detail::PutU32(claimTooLong, 10000u);   // rulesJson claims 10000 bytes
        claimTooLong.push_back(std::byte{ '{' });  // only one byte actually follows
        CHECK(!DecodeRuleState(claimTooLong, truncated));
        CHECK(truncated.pinned);
        CHECK(truncated.pinnedName == "should not survive a failed decode");
    }

    {  // Trailing garbage after an otherwise well-formed record is refused,
       // same discipline as Decode/DecodeNpcAssignments/DecodeHairBaseline.
        RuleRecordFields fields;
        fields.rulesJson = "{\"version\":1,\"rules\":[]}";
        auto bytes       = EncodeRuleState(fields);
        bytes.push_back(std::byte{ 0x00 });
        RuleRecordFields back;
        CHECK(!DecodeRuleState(bytes, back));
    }

    {  // ---- 'DFLK' / 'DFBD': this save's character defaults -----------------
        //
        // These moved out of default-looks.json and default-bodies.json because
        // a default costs a look to set and the charge lives in the co-save, so
        // a file on disk meant paying, reloading and keeping the purchase.
        using OS::DecodeDefaultBodies;
        using OS::DecodeDefaultLooks;
        using OS::DefaultBodyRecord;
        using OS::DefaultLookRecord;
        using OS::EncodeDefaultBodies;
        using OS::EncodeDefaultLooks;

        // Empty round trips, and it has to: a save whose character has no
        // default still writes the record, and that written emptiness is what
        // says "this save has been through this build" so the one-time import
        // from the old file never runs twice.
        constexpr std::uint32_t kDflkV2 = OS::kDefaultLookVersion;

        std::vector<DefaultLookRecord> emptyLooks;
        CHECK(DecodeDefaultLooks(EncodeDefaultLooks({}), kDflkV2, emptyLooks));
        CHECK(emptyLooks.empty());

        // ⚠ THE CURRENT VERSION IS 4, AND THE THREE OLDER ONES ARE STILL READ.
        // Named as a literal rather than taken from the constant, so a future
        // bump has to come here and decide what it does to the older halves
        // instead of sliding past a test that always agrees with itself.
        CHECK(OS::kDefaultLookVersion == 4u);

        DefaultLookRecord a;
        a.modAndId = "Skyrim.esm|000007";
        a.hairMod  = "ApachiiSkyHair.esm";
        a.hairId   = 0x0012C4;
        a.eyesMod  = "TheEyesOfBeauty.esp";
        a.eyesId   = 0x00081F;
        // Brows deliberately left unset: each part is independent, and an entry
        // with only some of them filled is the normal state, not half written.
        DefaultLookRecord b;
        b.modAndId = "Skyrim.esm|013478";  // a follower, who is per-save too now
        b.browsMod = "Skyrim.esm";
        b.browsId  = 0x0511BC;

        // Facial hair on one of them, unset on the other, for the same reason
        // brows are unset above: the parts are independent.
        a.facialMod = "Beards.esp";
        a.facialId  = 0x000A21;

        // And a default hair COLOUR on one of them (v3). Independent of the
        // style exactly as the four references are independent of each other:
        // "she is always this shade of red" is a separate statement from "she
        // always wears this hairstyle", and a character may make either one
        // without the other.
        a.hairTintSet = true;
        a.hairTintR   = 0x8A;
        a.hairTintG   = 0x2B;
        a.hairTintB   = 0x0E;

        // And pinned slots (v4), on the character who already has the most
        // filled in. Two of them, because the list is counted rather than one
        // field: a character with horns AND cat ears is the case a fixed-width
        // record could not have expressed.
        a.customHeadParts.push_back({ 106u, "ED Horns RMIntegration.esp", 0x000D41u });
        a.customHeadParts.push_back({ 110u, "ChooeyDintEarsEdit.esp", 0x0007A2u });

        std::vector<DefaultLookRecord> looksBack;
        CHECK(DecodeDefaultLooks(EncodeDefaultLooks({ a, b }), kDflkV2, looksBack));
        CHECK(looksBack.size() == 2);
        CHECK(looksBack[0] == a);
        CHECK(looksBack[1] == b);

        // ⚠⚠ A v1 RECORD STILL DECODES, AND THIS IS THE TEST THAT SAYS THE
        // BUMP DID NOT DELETE ANYBODY'S DEFAULTS. The read path used to
        // demand version equality, so 'DFLK' v2 without this would have
        // SKIPPED every record written before facial hair existed and quietly
        // dropped every default the player had set. The v1 bytes are forged
        // rather than truncated: a v1 row simply stops after the brow pair.
        {
            std::vector<std::byte> v1;
            OS::detail::PutU32(v1, 1u);  // one row
            OS::detail::PutStr(v1, "Skyrim.esm|000007");
            OS::detail::PutStr(v1, "ApachiiSkyHair.esm");
            OS::detail::PutU32(v1, 0x0012C4u);
            OS::detail::PutStr(v1, "TheEyesOfBeauty.esp");
            OS::detail::PutU32(v1, 0x00081Fu);
            OS::detail::PutStr(v1, "");
            OS::detail::PutU32(v1, 0u);

            std::vector<DefaultLookRecord> old;
            CHECK(DecodeDefaultLooks(v1, 1u, old));
            CHECK(old.size() == 1);
            CHECK(old[0].hairMod == "ApachiiSkyHair.esm");
            CHECK(old[0].eyesId == 0x00081Fu);
            CHECK(old[0].facialMod.empty());  // a v1 save names no facial hair
            CHECK(old[0].facialId == 0u);
            // And the same bytes read AS v2 must FAIL rather than half-read:
            // the row is short by a pair, which is what the version is for.
            std::vector<DefaultLookRecord> mis;
            CHECK(!DecodeDefaultLooks(v1, kDflkV2, mis));
        }

        // ⚠⚠ AND A v2 RECORD STILL DECODES, which is the same test one version
        // on and it is not redundant. v3 appends a hair COLOUR, so the moment
        // the constant moved to 3 every save written since facial hair landed
        // became the "older half", and without this they would be refused and
        // every default in them dropped. The bytes are forged rather than
        // truncated, for the reason the v1 block gives.
        {
            std::vector<std::byte> v2;
            OS::detail::PutU32(v2, 1u);  // one row
            OS::detail::PutStr(v2, "Skyrim.esm|000007");
            OS::detail::PutStr(v2, "ApachiiSkyHair.esm");
            OS::detail::PutU32(v2, 0x0012C4u);
            OS::detail::PutStr(v2, "TheEyesOfBeauty.esp");
            OS::detail::PutU32(v2, 0x00081Fu);
            OS::detail::PutStr(v2, "");
            OS::detail::PutU32(v2, 0u);
            OS::detail::PutStr(v2, "Beards.esp");
            OS::detail::PutU32(v2, 0x000A21u);

            std::vector<DefaultLookRecord> old;
            CHECK(DecodeDefaultLooks(v2, 2u, old));
            CHECK(old.size() == 1);
            CHECK(old[0].facialMod == "Beards.esp");
            // ⚠ A v2 SAVE NAMES NO DEFAULT COLOUR, and that is the "changes
            // nothing" default rather than a black one: the character keeps
            // whatever colour they already had, which is byte for byte what
            // that save already did.
            CHECK(!old[0].hairTintSet);
            CHECK(old[0].hairTintR == 0u);
            // The same bytes read AS v3 must fail rather than half-read.
            std::vector<DefaultLookRecord> mis2;
            CHECK(!DecodeDefaultLooks(v2, kDflkV2, mis2));
        }

        // ⚠⚠ AND A v3 RECORD STILL DECODES. Same test one version on, and it
        // is not redundant for the same reason the v2 block is not: v4 appends
        // the pinned slots, so the moment the constant moved to 4 every save
        // written since the hair colour landed became the older half.
        {
            std::vector<std::byte> v3;
            OS::detail::PutU32(v3, 1u);  // one row
            OS::detail::PutStr(v3, "Skyrim.esm|000007");
            OS::detail::PutStr(v3, "ApachiiSkyHair.esm");
            OS::detail::PutU32(v3, 0x0012C4u);
            OS::detail::PutStr(v3, "");
            OS::detail::PutU32(v3, 0u);
            OS::detail::PutStr(v3, "");
            OS::detail::PutU32(v3, 0u);
            OS::detail::PutStr(v3, "");
            OS::detail::PutU32(v3, 0u);
            OS::detail::PutU8(v3, 1u);     // a colour IS set
            OS::detail::PutU8(v3, 0x8Au);
            OS::detail::PutU8(v3, 0x2Bu);
            OS::detail::PutU8(v3, 0x0Eu);

            std::vector<DefaultLookRecord> old;
            CHECK(DecodeDefaultLooks(v3, 3u, old));
            CHECK(old.size() == 1);
            CHECK(old[0].hairTintSet);
            CHECK(old[0].hairTintR == 0x8Au);
            // ⚠ A v3 SAVE PINS NOTHING, and an empty list is exactly what that
            // save said rather than a loss: those characters wore whatever
            // their outfit named and their own part otherwise.
            CHECK(old[0].customHeadParts.empty());
            // The same bytes read AS v4 must fail rather than half-read.
            std::vector<DefaultLookRecord> mis3;
            CHECK(!DecodeDefaultLooks(v3, kDflkV2, mis3));
        }

        // ⚠⚠ A CORRUPT SLOT COUNT IS REFUSED BEFORE IT IS RESERVED. The count
        // is the one field in this record an attacker or a truncated file can
        // set freely, and reserve() on it is a large allocation.
        {
            std::vector<std::byte> huge;
            OS::detail::PutU32(huge, 1u);
            OS::detail::PutStr(huge, "Skyrim.esm|000007");
            OS::detail::PutStr(huge, "");
            OS::detail::PutU32(huge, 0u);
            OS::detail::PutStr(huge, "");
            OS::detail::PutU32(huge, 0u);
            OS::detail::PutStr(huge, "");
            OS::detail::PutU32(huge, 0u);
            OS::detail::PutStr(huge, "");
            OS::detail::PutU32(huge, 0u);
            OS::detail::PutU8(huge, 0u);
            OS::detail::PutU8(huge, 0u);
            OS::detail::PutU8(huge, 0u);
            OS::detail::PutU8(huge, 0u);
            OS::detail::PutU32(huge, 0xFFFFFFFFu);  // slots
            std::vector<DefaultLookRecord> never;
            CHECK(!DecodeDefaultLooks(huge, kDflkV2, never));
        }

        // ⚠ A PIN CLEARED IS A PIN GONE, not a pin stored empty. An entry that
        // encoded an empty reference would read back as "this character always
        // wears nothing there", which is what having no pin already means, and
        // it would also make a set-then-cleared character differ on the wire
        // from one that never pinned anything.
        {
            DefaultLookRecord none;
            none.modAndId = "Skyrim.esm|00001A";
            none.hairMod  = "ApachiiSkyHair.esm";
            none.hairId   = 0x0012C4u;
            std::vector<DefaultLookRecord> noneBack;
            CHECK(DecodeDefaultLooks(EncodeDefaultLooks({ none }), kDflkV2, noneBack));
            CHECK(noneBack.size() == 1);
            CHECK(noneBack[0].customHeadParts.empty());
            CHECK(noneBack[0] == none);
        }

        // ⚠ A DEFAULT OF BLACK IS A DEFAULT, and the flag is what says so. An
        // encoder that dropped a cleared RGB, or a decoder that read the flag
        // back off the colour, would turn "she is always jet black" into "she
        // has no default colour" across one save.
        {
            DefaultLookRecord black;
            black.modAndId    = "Skyrim.esm|000019";
            black.hairTintSet = true;
            black.hairTintR   = 0u;
            black.hairTintG   = 0u;
            black.hairTintB   = 0u;
            std::vector<DefaultLookRecord> blackBack;
            CHECK(DecodeDefaultLooks(EncodeDefaultLooks({ black }), kDflkV2, blackBack));
            CHECK(blackBack.size() == 1);
            CHECK(blackBack[0] == black);
            CHECK(blackBack[0].hairTintSet);
        }

        // An unknown version is refused outright rather than guessed at.
        {
            std::vector<DefaultLookRecord> future;
            CHECK(!DecodeDefaultLooks(EncodeDefaultLooks({ a }), 99u, future));
        }

        // Truncation and trailing garbage are both refused, so a corrupt record
        // leaves the map EMPTY rather than half read. The caller must not fall
        // back to the old file on a refusal: that would put the cross-save leak
        // straight back.
        auto looksBytes = EncodeDefaultLooks({ a });
        looksBytes.pop_back();
        std::vector<DefaultLookRecord> broken;
        CHECK(!DecodeDefaultLooks(looksBytes, kDflkV2, broken));
        auto looksTrail = EncodeDefaultLooks({ a });
        looksTrail.push_back(std::byte{ 0x00 });
        CHECK(!DecodeDefaultLooks(looksTrail, kDflkV2, broken));

        DefaultBodyRecord ib;
        ib.modAndId  = "Skyrim.esm|000007";
        ib.installed = "CBBE Curvy";
        DefaultBodyRecord cb;
        cb.modAndId = "Skyrim.esm|013478";
        cb.customId = "bs-0f3a91";

        std::vector<DefaultBodyRecord> bodiesBack;
        CHECK(DecodeDefaultBodies(EncodeDefaultBodies({ ib, cb }), bodiesBack));
        CHECK(bodiesBack.size() == 2);
        CHECK(bodiesBack[0] == ib);
        CHECK(bodiesBack[1] == cb);

        auto bodyBytes = EncodeDefaultBodies({ ib });
        bodyBytes.push_back(std::byte{ 0x7F });
        std::vector<DefaultBodyRecord> bodyBroken;
        CHECK(!DecodeDefaultBodies(bodyBytes, bodyBroken));

        // ⚠ AN ABSURD COUNT IS REFUSED BEFORE ANY RESERVE. Without this a
        // corrupt four bytes asks for a four-billion-entry vector.
        std::vector<std::byte> huge;
        for (int i = 0; i < 4; ++i) {
            huge.push_back(std::byte{ 0xFF });
        }
        std::vector<DefaultLookRecord> never;
        CHECK(!DecodeDefaultLooks(huge, kDflkV2, never));
        std::vector<DefaultBodyRecord> neverB;
        CHECK(!DecodeDefaultBodies(huge, neverB));
    }

    {  // v23 round-trip: an outfit's push-up survives, and a v22 record has none
        OutfitLibrary lib;
        lib.Create("Bodice");
        lib.Activate(0);
        if (auto* made = lib.At(0)) {
            made->pushUp = PushUpMode::kFull;
        }
        auto bytes = Encode(lib);

        OutfitLibrary back;
        CHECK(Decode(bytes, kCodecVersion, back));
        const auto* o = back.At(0);
        CHECK(o != nullptr);
        if (o) {
            CHECK(o->pushUp == PushUpMode::kFull);
        }

        // ⚠⚠ THE GUARANTEE THAT ACTUALLY MATTERS IS THE UPGRADE. A v22 record
        // is these bytes with the last one gone, and it has to DECODE rather
        // than be refused: a refusal here empties the player's whole library,
        // not one outfit of it. kNone is what every outfit written before this
        // meant.
        auto v22 = bytes;
        v22.resize(v22.size() - 1);
        OutfitLibrary older;
        CHECK(Decode(v22, 22u, older));
        const auto* p = older.At(0);
        CHECK(p != nullptr);
        if (p) {
            CHECK(p->name == "Bodice");
            CHECK(p->pushUp == PushUpMode::kNone);
        }

        // ⚠ AND AN OUT-OF-RANGE BYTE READS AS OFF rather than as a mode the
        // recipe table has no arm for. A later build with a third level writes
        // exactly this byte, and the record still has to survive it.
        auto bad  = bytes;
        bad.back() = std::byte{ 200 };
        OutfitLibrary tolerated;
        CHECK(Decode(bad, kCodecVersion, tolerated));
        const auto* q = tolerated.At(0);
        CHECK(q != nullptr);
        if (q) {
            CHECK(q->pushUp == PushUpMode::kNone);
        }
    }

    if (g_failures == 0) {
        std::printf("PersistenceTests: all passed\n");
        return 0;
    }
    std::printf("PersistenceTests: %d failure(s)\n", g_failures);
    return 1;
}
