// Pure-logic tests for Outfit / SlotMask / EditHistory / FavoriteSet.
// No engine, no RE:: types.
#include "Favorites.h"
#include "Outfit.h"
#include "SlotMask.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

int main() {
    using namespace OS;

    {  // slot <-> bit mapping
        CHECK(BitForEditorSlot(30) == 0);
        CHECK(BitForEditorSlot(32) == 2);
        CHECK(BitForEditorSlot(46) == 16);
        CHECK(MaskForEditorSlot(32) == (1u << 2));
        CHECK(IsBodySkinBit(BitForEditorSlot(32)));
        CHECK(IsBodySkinBit(BitForEditorSlot(33)));
        CHECK(IsBodySkinBit(BitForEditorSlot(37)));
        CHECK(!IsBodySkinBit(BitForEditorSlot(31)));
        CHECK(IsHeadPartBit(BitForEditorSlot(31)));
        CHECK(IsHeadPartBit(BitForEditorSlot(42)));
        CHECK(!IsHeadPartBit(BitForEditorSlot(32)));
    }

    {  // an empty outfit displays nothing of its own
        Outfit o;
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kPassthrough);
        CHECK(o.StyleMask() == 0);
        CHECK(o.HideMask() == 0);
    }

    {  // style + hide + passthrough masks
        Outfit o;
        o.SetStyle(2, StyleRefKey{ "Armors.esp", 0x800 });   // body
        o.SetHide(1);                                        // helmet
        CHECK(o.StyleMask() == (1u << 2));
        CHECK(o.HideMask() == (1u << 1));
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kStyle);
        CHECK(o.EntryFor(2).style.localFormID == 0x800);
        CHECK(o.EntryFor(0).kind == SlotEntry::Kind::kPassthrough);

        o.SetPassthrough(2);
        CHECK(o.StyleMask() == 0);
        CHECK(o.EntryFor(2).kind == SlotEntry::Kind::kPassthrough);
    }

    {  // out-of-range bits are safe no-ops (mutators called from the biped rebuild)
        Outfit o;
        o.SetStyle(32, StyleRefKey{ "x.esp", 1 });   // == kBitCount, just past the end
        o.SetHide(99);
        CHECK(o.StyleMask() == 0);
        CHECK(o.HideMask() == 0);
        CHECK(o.EntryFor(32).kind == SlotEntry::Kind::kPassthrough);
        CHECK(o.EntryFor(99).kind == SlotEntry::Kind::kPassthrough);
        o.SetPassthrough(32);                        // must not corrupt anything
        CHECK(o.StyleMask() == 0);
    }

    {  // ForEachStyle visits exactly the styled bits, ascending, with the right keys
        Outfit o;
        o.SetStyle(5, StyleRefKey{ "b.esp", 0x22 });
        o.SetStyle(1, StyleRefKey{ "a.esp", 0x11 });
        o.SetHide(3);                                // must NOT be visited
        std::vector<std::uint32_t> bits;
        std::vector<StyleRefKey>   keys;
        o.ForEachStyle([&](std::uint32_t b, const StyleRefKey& k) {
            bits.push_back(b);
            keys.push_back(k);
        });
        CHECK(bits.size() == 2);
        CHECK(bits == (std::vector<std::uint32_t>{ 1, 5 }));   // ascending bit order
        CHECK(keys[0] == (StyleRefKey{ "a.esp", 0x11 }));
        CHECK(keys[1] == (StyleRefKey{ "b.esp", 0x22 }));

        int count = 0;
        Outfit empty;
        empty.ForEachStyle([&](std::uint32_t, const StyleRefKey&) { ++count; });
        CHECK(count == 0);
    }

    {  // computeDisplaySet: blocklist forces passthrough
        Outfit o;
        o.SetHide(2);
        const auto d = ComputeDisplaySet(o, /*blocklist*/ (1u << 2));
        CHECK(d.hideMask == 0);         // slot 2 was blocklisted
        CHECK(d.styleMask == 0);
    }

    {  // computeDisplaySet: hidden head-part bits are reported for the mask shim
        Outfit o;
        o.SetHide(1);                   // helmet (head part)
        o.SetHide(2);                   // body (not a head part)
        const auto d = ComputeDisplaySet(o, /*blocklist*/ 0);
        CHECK(d.hideMask == ((1u << 1) | (1u << 2)));
        CHECK(d.hiddenHeadPartMask == (1u << 1));
        CHECK(d.hiddenBodySkinMask == (1u << 2));
        CHECK(d.hiddenAttachmentMask == (1u << 1));   // helmet culls its node too
    }

    {  // the shield (bit 9) is never styled or hidden, even if an outfit sets it
        Outfit o;
        o.SetHide(kBitShield);
        const auto d1 = ComputeDisplaySet(o, /*blocklist*/ 0);
        CHECK((d1.hideMask & MaskForEditorSlot(39)) == 0);
        CHECK(d1.hiddenAttachmentMask == 0);

        Outfit o2;
        o2.SetStyle(kBitShield, StyleRefKey{ "x.esp", 1 });
        const auto d2 = ComputeDisplaySet(o2, 0);
        CHECK((d2.styleMask & MaskForEditorSlot(39)) == 0);
    }

    {  // library: 10 slots, one active, rename, activate/deactivate
        OutfitLibrary lib;
        CHECK(lib.Count() == 0);
        CHECK(lib.ActiveIndex() == -1);
        const int i = lib.Create("Tavern");
        CHECK(i == 0);
        CHECK(lib.Count() == 1);
        lib.Activate(0);
        CHECK(lib.ActiveIndex() == 0);
        CHECK(lib.Active() != nullptr);
        lib.Rename(0, "Court");
        CHECK(lib.At(0)->name == "Court");
        lib.Deactivate();
        CHECK(lib.ActiveIndex() == -1);
        CHECK(lib.Active() == nullptr);

        for (std::size_t k = 1; k < kMaxOutfits; ++k) {
            CHECK(lib.Create("x") == static_cast<int>(k));
        }
        CHECK(lib.Create("overflow") == -1);   // hard cap of kMaxOutfits
        CHECK(lib.Count() == kMaxOutfits);
    }

    {  // deleting an outfit clears the active index if it pointed at it
        OutfitLibrary lib;
        lib.Create("a");
        lib.Create("b");
        lib.Activate(1);
        lib.Remove(1);
        CHECK(lib.ActiveIndex() == -1);
        CHECK(lib.Count() == 1);
    }

    {  // removing below the active index decrements it; it still names the same outfit
        OutfitLibrary lib;
        lib.Create("a");
        lib.Create("b");
        lib.Create("c");
        lib.Activate(2);
        lib.Remove(0);
        CHECK(lib.ActiveIndex() == 1);
        CHECK(lib.At(1)->name == "c");
        CHECK(lib.Count() == 2);
    }

    {  // removing above the active index leaves it unchanged
        OutfitLibrary lib;
        lib.Create("a");
        lib.Create("b");
        lib.Create("c");
        lib.Activate(0);
        lib.Remove(2);
        CHECK(lib.ActiveIndex() == 0);
        CHECK(lib.Count() == 2);
    }

    {  // Move: the active outfit follows its entry, neighbors shift correctly
        OutfitLibrary lib;
        lib.Create("a");
        lib.Create("b");
        lib.Create("c");
        lib.Activate(2);          // active = "c"
        lib.Move(2, 0);           // c a b
        CHECK(lib.At(0)->name == "c");
        CHECK(lib.At(1)->name == "a");
        CHECK(lib.At(2)->name == "b");
        CHECK(lib.ActiveIndex() == 0);
        lib.Activate(1);          // active = "a"
        lib.Move(0, 2);           // a b c -> moving "c" right past "a": a b c? -> order: a b c
        CHECK(lib.At(2)->name == "c");
        CHECK(lib.ActiveIndex() == 0);  // "a" shifted left with the move
        lib.Move(5, 0);                 // out of range: no-op
        lib.Move(1, 1);                 // same index: no-op
        CHECK(lib.Count() == 3);
        CHECK(lib.At(0)->name == "a");
    }

    {  // removing an out-of-range index is a harmless no-op
        OutfitLibrary lib;
        lib.Create("a");
        lib.Create("b");
        lib.Activate(1);
        lib.Remove(5);
        CHECK(lib.Count() == 2);
        CHECK(lib.ActiveIndex() == 1);
    }

    {  // ChangedSlotCount: only value-differences count, styles compare by key
        Outfit base;
        base.SetStyle(2, StyleRefKey{ "a.esp", 0x10 });
        base.SetHide(1);
        Outfit staged = base;
        CHECK(ChangedSlotCount(base, staged) == 0);      // identical
        staged.SetStyle(2, StyleRefKey{ "a.esp", 0x11 }); // different key
        CHECK(ChangedSlotCount(base, staged) == 1);
        staged.SetStyle(2, StyleRefKey{ "a.esp", 0x10 }); // back to baseline key
        CHECK(ChangedSlotCount(base, staged) == 0);
        staged.SetPassthrough(1);                         // un-hide slot 1
        CHECK(ChangedSlotCount(base, staged) == 1);
    }

    {  // ToggleHideSlot round trip returns a STYLED slot to its style -
        // the Apply-cost regression: Hide then Show must net zero cost.
        std::array<SlotEntry, kBitCount> stash{};
        Outfit base;
        base.SetStyle(2, StyleRefKey{ "Armors.esp", 0x800 });   // committed styled slot
        Outfit staged = base;
        CHECK(ChangedSlotCount(base, staged) == 0);

        ToggleHideSlot(staged, stash, 2);                       // Hide
        CHECK(staged.EntryFor(2).kind == SlotEntry::Kind::kHide);
        CHECK(ChangedSlotCount(base, staged) == 1);             // hiding is a real change

        ToggleHideSlot(staged, stash, 2);                       // Show
        CHECK(staged.EntryFor(2).kind == SlotEntry::Kind::kStyle);
        CHECK(staged.EntryFor(2).style == (StyleRefKey{ "Armors.esp", 0x800 }));
        CHECK(ChangedSlotCount(base, staged) == 0);             // cost reset to zero
    }

    {  // ToggleHideSlot on a passthrough slot round-trips to passthrough
        std::array<SlotEntry, kBitCount> stash{};
        Outfit o;
        ToggleHideSlot(o, stash, 5);                            // Hide
        CHECK(o.EntryFor(5).kind == SlotEntry::Kind::kHide);
        ToggleHideSlot(o, stash, 5);                            // Show
        CHECK(o.EntryFor(5).kind == SlotEntry::Kind::kPassthrough);
    }

    {  // re-stashing on each Hide: pick a new style while hidden, toggle again
        std::array<SlotEntry, kBitCount> stash{};
        Outfit o;
        o.SetStyle(3, StyleRefKey{ "a.esp", 0x1 });
        ToggleHideSlot(o, stash, 3);                            // Hide (stash A)
        o.SetStyle(3, StyleRefKey{ "b.esp", 0x2 });            // pick B from browser
        ToggleHideSlot(o, stash, 3);                            // Hide (re-stash B)
        ToggleHideSlot(o, stash, 3);                            // Show -> B, not A
        CHECK(o.EntryFor(3).style == (StyleRefKey{ "b.esp", 0x2 }));
    }

    {  // EditHistory: linear undo/redo over staged-outfit snapshots (OS-21)
        EditHistory h;
        Outfit      a;                                          // baseline (empty)
        h.Reset(a);
        CHECK(h.Size() == 1);
        CHECK(!h.CanUndo());
        CHECK(!h.CanRedo());

        Outfit b = a;
        b.SetStyle(2, StyleRefKey{ "m.esp", 0x1 });
        h.Record(b);
        CHECK(h.CanUndo());
        CHECK(!h.CanRedo());

        Outfit c = b;
        c.SetStyle(7, StyleRefKey{ "m.esp", 0x2 });
        h.Record(c);
        CHECK(h.Size() == 3);

        CHECK(ChangedSlotCount(h.Undo(), b) == 0);              // walk back to b
        CHECK(h.CanRedo());
        CHECK(ChangedSlotCount(h.Undo(), a) == 0);              // ...then to a
        CHECK(!h.CanUndo());
        CHECK(ChangedSlotCount(h.Undo(), a) == 0);              // undo past start is a no-op
        CHECK(ChangedSlotCount(h.Redo(), b) == 0);              // forward to b
        CHECK(ChangedSlotCount(h.Redo(), c) == 0);              // ...then c
        CHECK(!h.CanRedo());
        CHECK(ChangedSlotCount(h.Redo(), c) == 0);              // redo past end is a no-op
    }

    {  // EditHistory: idempotent Record dropped; a new edit truncates the redo tail
        EditHistory h;
        Outfit      a;
        h.Reset(a);
        Outfit b = a;
        b.SetStyle(2, StyleRefKey{ "m.esp", 0x1 });
        h.Record(b);
        h.Record(b);                                            // same slots - no new entry
        CHECK(h.Size() == 2);

        h.Undo();                                               // back to a; b is now a redo
        CHECK(h.CanRedo());
        Outfit d = a;
        d.SetStyle(5, StyleRefKey{ "m.esp", 0x9 });
        h.Record(d);                                            // a fresh branch drops the b redo
        CHECK(!h.CanRedo());
        CHECK(ChangedSlotCount(h.Current(), d) == 0);
    }

    {  // EditHistory: bounded to kCap - the oldest snapshots drop, cursor stays valid
        EditHistory h;
        Outfit      o;
        h.Reset(o);
        for (std::uint32_t i = 1; i <= EditHistory::kCap + 5; ++i) {
            o.SetStyle(2, StyleRefKey{ "m.esp", i });          // each edit distinct
            h.Record(o);
        }
        CHECK(h.Size() == EditHistory::kCap);
        CHECK(h.CanUndo());
        CHECK(!h.CanRedo());
        CHECK(ChangedSlotCount(h.Current(), o) == 0);           // newest state intact at the cursor
    }

    {  // FavoriteSet: toggle / contains / add / remove (OS-22)
        FavoriteSet       fs;
        const StyleRefKey k1{ "Armors.esp", 0x800 };
        const StyleRefKey k2{ "Other.esp", 0x14 };
        CHECK(!fs.Contains(k1));
        CHECK(fs.Toggle(k1) == true);                           // starred
        CHECK(fs.Contains(k1));
        CHECK(fs.Size() == 1);
        CHECK(fs.Toggle(k1) == false);                          // un-starred
        CHECK(!fs.Contains(k1));
        CHECK(fs.Size() == 0);

        fs.Add(k1);
        fs.Add(k2);
        fs.Add(k1);                                             // idempotent add
        CHECK(fs.Size() == 2);
        fs.Remove(k2);
        CHECK(fs.Size() == 1);
        CHECK(fs.Contains(k1));
        CHECK(!fs.Contains(k2));

        CHECK(fs.Toggle(StyleRefKey{}) == false);               // empty key never stored
        CHECK(!fs.Contains(StyleRefKey{}));
        CHECK(fs.Size() == 1);

        CHECK(FavoriteSet::KeyLine(k1) == std::string("Armors.esp|2048"));  // 0x800 == 2048
    }

    {  // FavoriteSet: serialize/load round-trips, tolerant of CRLF and blank lines
        FavoriteSet fs;
        fs.Add(StyleRefKey{ "a.esp", 0x1 });
        fs.Add(StyleRefKey{ "b.esp", 0x2 });

        FavoriteSet fs2;
        fs2.LoadLines(fs.Serialize());
        CHECK(fs2.Size() == 2);
        CHECK(fs2.Contains(StyleRefKey{ "a.esp", 0x1 }));
        CHECK(fs2.Contains(StyleRefKey{ "b.esp", 0x2 }));

        FavoriteSet fs3;
        fs3.LoadLines("a.esp|1\r\n\r\nb.esp|2\n");             // CRLF, blank line, trailing NL
        CHECK(fs3.Size() == 2);
        CHECK(fs3.Contains(StyleRefKey{ "a.esp", 0x1 }));
        CHECK(fs3.Contains(StyleRefKey{ "b.esp", 0x2 }));

        FavoriteSet fs4;
        fs4.LoadLines("");                                      // empty blob clears
        CHECK(fs4.Size() == 0);
    }

    if (g_failures == 0) {
        std::printf("OutfitTests: all passed\n");
        return 0;
    }
    std::printf("OutfitTests: %d failure(s)\n", g_failures);
    return 1;
}
