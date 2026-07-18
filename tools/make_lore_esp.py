"""Generate FittingRoomLore.esp (ESL): the lore addon.

Contents:
  MISC 0x805 "The Seamstone" - a plain misc item (user direction: favoriting
              and "using" misc items is covered by other mods in the load
              order, which route through the equip pipeline - our
              TESEquipEvent sink opens the editor when that fires; the
              possession-gated hotkey is the always-available opener).
              Form-type history, each earlier shape proven broken in-game:
              ARMO slot-61 = hidden from EVERY item list (the engine hides
              armor whose slots are all unnamed, 44+); zero-effect ALCH =
              CTD in ItemCardPopulate (the potion card dereferences the
              first effect unconditionally). MISC has neither pathology.
              Keyword VendorItemSoulGem: in Farengar's VendorItemsSpells
              whitelist - the wizard sells it beside his soul gems.
  BOOK 0x801 "On the Outward Art" - delivered to the player's inventory once;
              explains the stone and points to Farengar.

FormIDs 0x800 (Dressing Stand ACTI), 0x802 (its REFR), 0x803 (ARMO
Seamstone) and 0x804 (ALCH Seamstone) are RETIRED - never reuse them; test
saves may reference them (dropping 0x803/0x804 from the plugin also prunes
the orphaned invisible/toxic copies from the chest and player on load).

Everything borrowed (mesh paths, bounds) is mined from the load order's own
Skyrim.esm, so every reference is valid by construction. READ-ONLY on the
esm; writes dist/FittingRoomLore.esp.
"""
import io
import struct
import sys
import zlib

import os

ESM = r"C:\Games\Nolvus\Instances\Nolvus Awakening\STOCK GAME\Data\Skyrim.esm"
# dist/ of THIS repo, wherever it lives (the repo folder was renamed once
# already - never hardcode it again).
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "dist", "optional", "FittingRoomLore.esp")

REC_HDR = 24
FLAG_COMPRESSED = 0x00040000

NOTE_TEXT = (
    "<p>On the Outward Art</p>"
    "<p>The College debates whether the reshaping of appearances belongs to "
    "Illusion, which deceives the eye, or to Alteration, which persuades the "
    "world itself. The tailors of the art hold with Alteration: the garment "
    "is not an illusion laid over the steel, but the steel convinced, for a "
    "while, to wear another face.</p>"
    "<p>Masters of the Outward Art once shaped their attire anywhere, by "
    "will alone. What they knew has since been bound into cut and polished "
    "foci - seamstones - so that any patient hand may hold the trick of it. "
    "Grasp the stone, and will your attire otherwise. The flesh beneath "
    "keeps its own protections; only the seeming changes.</p>"
    "<p>Alteration is a hungry art. A seamstone feeds on the most agreeable "
    "of metals: each seam re-persuaded dissolves a little gold into the "
    "working, much as the Transmute incantation coaxes silver toward it.</p>"
    "<p>Court wizards keep seamstones among their oddments. The one at "
    "Dragonsreach sells to any hand with coin enough.</p>"
)

def read_esm(path):
    with open(path, "rb") as f:
        return f.read()


def subrecords(data):
    """Yield (sig, payload) from decompressed record data. Handles XXXX."""
    off, size_override = 0, None
    while off + 6 <= len(data):
        sig = data[off:off + 4]
        sz = struct.unpack_from("<H", data, off + 4)[0]
        off += 6
        if sig == b"XXXX":
            size_override = struct.unpack_from("<I", data, off)[0]
            off += sz
            continue
        real = size_override if size_override is not None else sz
        size_override = None
        yield sig, data[off:off + real]
        off += real


def record_data(buf, rec_off, data_size, flags):
    raw = buf[rec_off + REC_HDR:rec_off + REC_HDR + data_size]
    if flags & FLAG_COMPRESSED:
        return zlib.decompress(raw[4:])
    return raw


def walk_records(buf, want_types):
    """Yield (rectype, formid, flags, rec_off, data_size) for records whose
    type is in want_types, walking every GRUP."""
    total = len(buf)
    off = 0
    # skip TES4
    if buf[0:4] == b"TES4":
        tes4_size = struct.unpack_from("<I", buf, 4)[0]
        off = REC_HDR + tes4_size
    while off + REC_HDR <= total:
        sig = buf[off:off + 4]
        if sig == b"GRUP":
            gsize = struct.unpack_from("<I", buf, off + 4)[0]
            label = buf[off + 8:off + 12]
            gtype = struct.unpack_from("<i", buf, off + 12)[0]
            descend = gtype != 0 or label in want_types or label == b"CELL" or label == b"WRLD"
            if descend:
                yield from _walk_region(buf, off + 24, off + gsize, want_types)
            off += gsize
        else:
            dsize, flags, formid = struct.unpack_from("<III", buf, off + 4)
            if sig in want_types:
                yield sig, formid, flags, off, dsize
            off += REC_HDR + dsize
    return


def _walk_region(buf, off, end, want_types):
    while off + REC_HDR <= end:
        sig = buf[off:off + 4]
        if sig == b"GRUP":
            gsize = struct.unpack_from("<I", buf, off + 4)[0]
            yield from _walk_region(buf, off + 24, off + gsize, want_types)
            off += gsize
        else:
            dsize, flags, formid = struct.unpack_from("<III", buf, off + 4)
            if sig in want_types:
                yield sig, formid, flags, off, dsize
            off += REC_HDR + dsize


def find_donors(buf):
    """One pass: grand-soul-gem model (the Seamstone body), note-model book.

    The note donor is copied to FULL VANILLA PARITY (BUG-1, 2026-07-17): every
    one of the 259 note-model BOOKs in Skyrim.esm carries YNAM (pickup sound),
    KSIZ/KWDA (VendorItemBook), INAM (inventory art - the high-poly STAT the
    BookMenu displays while READING) and CNAM (item card description). Our
    0.1.1 book shipped without INAM/CNAM and reading it CTD'd in the field -
    the reading view resolves the book's inventory-art model. All formid
    payloads (YNAM/KWDA/INAM) point into Skyrim.esm, valid by construction.
    CNAM is NOT copyable (localized esm = lstring id); we write our own inline.
    """
    gem = note = None  # gem: (MODL, MODT, OBND, YNAM, ZNAM)
    # note: dict of donor subrecords, verbatim payloads

    want = {b"MISC", b"BOOK"}
    for sig, formid, flags, off, dsize in walk_records(buf, want):
        try:
            data = record_data(buf, off, dsize, flags)
        except Exception:
            continue
        if sig == b"MISC" and gem is None:
            fields = {}
            for s, p in subrecords(data):
                if s in (b"MODL", b"MODT", b"OBND", b"YNAM", b"ZNAM"):
                    fields[s] = p
            modl = fields.get(b"MODL", b"")
            if b"soulgemgrand" in modl.lower():
                gem = (fields.get(b"MODL"), fields.get(b"MODT"), fields.get(b"OBND"),
                       fields.get(b"YNAM"), fields.get(b"ZNAM"))
        elif sig == b"BOOK" and note is None:
            fields = {}
            for s, p in subrecords(data):
                if s in (b"MODL", b"MODT", b"OBND", b"DATA", b"YNAM", b"ZNAM",
                         b"KSIZ", b"KWDA", b"INAM"):
                    fields[s] = p
            modl = fields.get(b"MODL", b"")
            if b"note" in modl.lower():
                note = fields
        if gem and note:
            break
    return gem, note


def sub(sig, payload):
    return sig + struct.pack("<H", len(payload)) + payload


def zstring(s):
    return s.encode("cp1252") + b"\x00"


def record(sig, formid, flags, *subs):
    data = b"".join(subs)
    return (sig + struct.pack("<IIIHHHH", len(data), flags, formid, 0, 0, 44, 0) + data)


def grup(label, gtype, *payload):
    data = b"".join(payload)
    return (b"GRUP" + struct.pack("<I", 24 + len(data)) + label +
            struct.pack("<iHHHH", gtype, 0, 0, 0, 0) + data)


def main():
    buf = read_esm(ESM)
    gem, note = find_donors(buf)

    if not gem or not gem[0]:
        sys.exit("no grand-soul-gem model donor found")
    if not note or not note.get(b"MODL"):
        sys.exit("no note-model donor found")
    for req in (b"YNAM", b"KSIZ", b"KWDA", b"INAM"):
        if not note.get(req):
            sys.exit(f"note donor lacks {req.decode()} - parity copy impossible, "
                     "pick a different donor")

    nul = b"\x00"
    print(f"seamstone model: {gem[0].rstrip(nul).decode('cp1252', 'replace')}")
    print(f"note model:      {note[b'MODL'].rstrip(nul).decode('cp1252', 'replace')}")

    BOOKID, STONEID = 0x01000801, 0x01000805

    # --- TES4 header (ESL) ---
    hedr = struct.pack("<fIi", 1.70, 2, 0x806)
    tes4_data = (sub(b"HEDR", hedr) + sub(b"CNAM", zstring("Fitting Room")) +
                 sub(b"SNAM", zstring("ESO-style transmog lore addon")) +
                 sub(b"MAST", zstring("Skyrim.esm")) + sub(b"DATA", struct.pack("<Q", 0)))
    tes4 = (b"TES4" + struct.pack("<IIIHHHH", len(tes4_data), 0x200, 0, 0, 0, 44, 0) +
            tes4_data)

    # --- MISC The Seamstone ---
    # A plain misc item: no slots (the ARMO lesson), no effects (the ALCH
    # lesson). Sounds are the donor gem's own pickup/putdown. Vendors trade
    # only items whose keywords hit their vendor formlist -
    # VendorItemSoulGem is in Farengar's VendorItemsSpells whitelist and
    # puts the stone on the right shelf.
    misc_subs = [sub(b"EDID", zstring("OS_Seamstone"))]
    if gem[2]:
        misc_subs.append(sub(b"OBND", gem[2]))
    misc_subs.append(sub(b"FULL", zstring("The Seamstone")))
    misc_subs.append(sub(b"MODL", gem[0]))          # grand soul gem
    if gem[1]:
        misc_subs.append(sub(b"MODT", gem[1]))
    if gem[3]:
        misc_subs.append(sub(b"YNAM", gem[3]))      # pickup sound
    if gem[4]:
        misc_subs.append(sub(b"ZNAM", gem[4]))      # putdown sound
    misc_subs.append(sub(b"KSIZ", struct.pack("<I", 1)))
    misc_subs.append(sub(b"KWDA", struct.pack("<I", 0x000937A3)))  # VendorItemSoulGem
    misc_subs.append(sub(b"DATA", struct.pack("<If", 500, 0.5)))   # value, weight
    misc = record(b"MISC", STONEID, 0, *misc_subs)

    # --- BOOK the note ---
    # Subrecord ORDER mirrors the donor exactly: EDID,OBND,FULL,MODL,MODT,
    # DESC,YNAM,(ZNAM,)KSIZ,KWDA,DATA,INAM,CNAM. INAM is the piece whose
    # absence CTD'd reading in 0.1.1 (BUG-1) - the BookMenu resolves the
    # inventory-art STAT (HighPolyNote02 for Note02-model notes) for the
    # reading close-up. CNAM is ours, inline (non-localized plugin).
    book_subs = [sub(b"EDID", zstring("OS_OutwardArtNote"))]
    if note.get(b"OBND"):
        book_subs.append(sub(b"OBND", note[b"OBND"]))
    book_subs.append(sub(b"FULL", zstring("On the Outward Art")))
    book_subs.append(sub(b"MODL", note[b"MODL"]))
    if note.get(b"MODT"):
        book_subs.append(sub(b"MODT", note[b"MODT"]))
    book_subs.append(sub(b"DESC", zstring(NOTE_TEXT)))
    book_subs.append(sub(b"YNAM", note[b"YNAM"]))          # ITMNoteUp
    if note.get(b"ZNAM"):
        book_subs.append(sub(b"ZNAM", note[b"ZNAM"]))
    book_subs.append(sub(b"KSIZ", note[b"KSIZ"]))
    book_subs.append(sub(b"KWDA", note[b"KWDA"]))          # VendorItemBook
    donor_data = note.get(b"DATA")
    book_data = donor_data if donor_data and len(donor_data) == 16 else struct.pack(
        "<BBHiIf", 0, 0, 0, -1, 5, 0.5)
    book_subs.append(sub(b"DATA", book_data))
    book_subs.append(sub(b"INAM", note[b"INAM"]))          # HighPolyNote02
    book_subs.append(sub(b"CNAM", zstring(
        "A College treatise on the outward art of the seamstones.")))
    book = record(b"BOOK", BOOKID, 0, *book_subs)

    out = io.BytesIO()
    out.write(tes4)
    out.write(grup(b"BOOK", 0, book))
    out.write(grup(b"MISC", 0, misc))

    import os
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(out.getvalue())
    print(f"wrote {OUT} ({out.tell()} bytes)")


if __name__ == "__main__":
    main()
