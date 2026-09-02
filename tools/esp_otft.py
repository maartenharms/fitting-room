"""Dump the OUTFIT (OTFT) records that name a given armour, and what else they name.

The set-completion pass in SetDetector fills a detected set's empty slots from
the game's own outfit records, keyed on the set's BODY piece. When a card comes
back wearing something absurd (Scaled armour under an Execution Hood) the
questions are always the same: how many records name that body, do they agree
about the slot that went wrong, and does the piece they offer overlap something
the set is already wearing.

    python tools/esp_otft.py <plugin> [<plugin> ...] --body ArmorScaledCuirass

Leveled lists named by an outfit are expanded, which is what the runtime does
before the catalog ever sees the pieces. Cross-plugin form ids resolve through
each plugin's own master list, so a record in Update.esm that names a
Skyrim.esm armour resolves. A localized plugin's display names come out of its
.STRINGS tables, because the card shows those and the editor ids are not what
anybody reports a bug about.
"""
import argparse
import struct
import sys
import zlib

NUL = b"\x00"

SLOT_NAMES = {
    0: "30 head", 1: "31 hair", 2: "32 body", 3: "33 hands", 4: "34 forearms",
    5: "35 amulet", 6: "36 ring", 7: "37 feet", 8: "38 calves", 9: "39 shield",
    10: "40 tail", 11: "41 longhair", 12: "42 circlet", 13: "43 ears",
    14: "44 face", 15: "45", 16: "46", 17: "47", 18: "48", 19: "49", 20: "50",
    21: "51", 22: "52 genitals", 23: "53", 24: "54", 25: "55", 26: "56",
    27: "57", 28: "58", 29: "59", 30: "60", 31: "61",
}

# The head cluster. A piece landing on any of these draws on the same skull, so
# two of them is the clipping the field reported rather than a fuller set.
HEAD_BITS = (0, 1, 11, 12, 13, 14)


def subrecords(data):
    """Yield (sig, payload). XXXX carries the real size of the next subrecord."""
    p = 0
    override = None
    while p + 6 <= len(data):
        sig = data[p:p + 4].decode("latin-1")
        size, = struct.unpack_from("<H", data, p + 4)
        p += 6
        if sig == "XXXX":
            override, = struct.unpack_from("<I", data, p)
            p += size
            continue
        if override is not None:
            size = override
            override = None
        yield sig, data[p:p + size]
        p += size


def walk(data, p, end, out):
    while p + 24 <= end:
        sig = data[p:p + 4].decode("latin-1")
        size, = struct.unpack_from("<I", data, p + 4)
        if sig == "GRUP":
            walk(data, p + 24, p + size, out)
            p += size
            continue
        flags, formid = struct.unpack_from("<II", data, p + 8)
        body = data[p + 24:p + 24 + size]
        if flags & 0x00040000:
            body = zlib.decompress(body[4:])
        out.append((sig, formid, body))
        p += 24 + size


def load(path):
    """(records, masters, localized)."""
    data = open(path, "rb").read()
    hsize, = struct.unpack_from("<I", data, 4)
    flags, = struct.unpack_from("<I", data, 8)
    header = data[24:24 + hsize]
    masters = []
    for sig, v in subrecords(header):
        if sig == "MAST":
            masters.append(v.rstrip(NUL).decode("latin-1").lower())
    recs = []
    walk(data, 24 + hsize, len(data), recs)
    return recs, masters, bool(flags & 0x80)


def load_strings(path, language="English"):
    """id -> text out of a plugin's string tables."""
    base = path.replace("\\", "/")
    folder, name = base.rsplit("/", 1)
    stem = name.rsplit(".", 1)[0]
    out = {}
    for ext in ("STRINGS", "DLSTRINGS", "ILSTRINGS"):
        try:
            data = open("%s/Strings/%s_%s.%s" % (folder, stem, language, ext), "rb").read()
        except OSError:
            continue
        count = struct.unpack_from("<I", data, 0)[0]
        block = 8 + count * 8
        for i in range(count):
            sid, off = struct.unpack_from("<II", data, 8 + i * 8)
            p = block + off
            if ext == "STRINGS":
                out[sid] = data[p:data.find(NUL, p)].decode("cp1252", "replace")
            else:
                length = struct.unpack_from("<I", data, p)[0]
                out[sid] = data[p + 4:p + 4 + length].rstrip(NUL).decode("cp1252", "replace")
    return out


class World:
    """Every loaded plugin's records under one id space of 'plugin|localid'.

    That is enough to compare two references and needs no load order, which a
    static dump has no way to know anyway.
    """

    def __init__(self):
        self.armo = {}   # gid -> (edid, full, mask)
        self.lvli = {}   # gid -> [gid]
        self.otft = {}   # gid -> (edid, [gid])

    @staticmethod
    def resolve(formid, own, masters):
        idx = formid >> 24
        if idx < len(masters):
            return "%s|%06X" % (masters[idx], formid & 0xFFFFFF)
        return "%s|%06X" % (own, formid & 0xFFFFFF)

    def add(self, path):
        own = path.replace("\\", "/").rsplit("/", 1)[-1].lower()
        recs, masters, localized = load(path)
        strings = load_strings(path) if localized else {}

        def text(v):
            if localized and len(v) == 4:
                return strings.get(struct.unpack_from("<I", v, 0)[0], "")
            return v.rstrip(NUL).decode("cp1252", "replace")

        for sig, formid, body in recs:
            gid = self.resolve(formid, own, masters)
            if sig == "ARMO":
                edid, full, mask = "", "", 0
                for s, v in subrecords(body):
                    if s == "EDID":
                        edid = v.rstrip(NUL).decode("latin-1")
                    elif s == "FULL":
                        full = text(v)
                    elif s in ("BOD2", "BODT") and len(v) >= 4:
                        mask = struct.unpack_from("<I", v, 0)[0]
                self.armo[gid] = (edid, full, mask)
            elif sig == "LVLI":
                items = []
                for s, v in subrecords(body):
                    if s == "LVLO" and len(v) >= 8:
                        items.append(self.resolve(struct.unpack_from("<I", v, 4)[0],
                                                  own, masters))
                self.lvli[gid] = items
            elif sig == "OTFT":
                edid, items = "", []
                for s, v in subrecords(body):
                    if s == "EDID":
                        edid = v.rstrip(NUL).decode("latin-1")
                    elif s == "INAM":
                        for off in range(0, len(v) - 3, 4):
                            items.append(self.resolve(
                                struct.unpack_from("<I", v, off)[0], own, masters))
                self.otft[gid] = (edid, items)

    def expand(self, gid, depth=0, seen=None):
        """An outfit entry flattened to armour ids, leveled lists expanded."""
        seen = seen if seen is not None else set()
        if gid in seen or depth > 8:
            return []
        seen.add(gid)
        if gid in self.armo:
            return [gid]
        out = []
        for child in self.lvli.get(gid, []):
            out.extend(self.expand(child, depth + 1, seen))
        return out

    def name(self, gid):
        edid, full, _ = self.armo.get(gid, ("", "", 0))
        return full or edid or gid

    def mask(self, gid):
        return self.armo.get(gid, ("", "", 0))[2]

    def slots(self, gid):
        m = self.mask(gid)
        return [SLOT_NAMES.get(i, str(i + 30)) for i in range(32) if (m >> i) & 1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plugins", nargs="+")
    ap.add_argument("--body", required=True,
                    help="substring of the body armour's editor id or name")
    ap.add_argument("--quiet", action="store_true",
                    help="per-slot verdicts only, no record listing")
    args = ap.parse_args()

    world = World()
    for p in args.plugins:
        world.add(p)

    bodies = [gid for gid, (edid, full, _) in world.armo.items()
              if args.body.lower() in (edid + " " + full).lower()]
    if not bodies:
        print("no ARMO matches %r" % args.body)
        return 1

    for body in sorted(bodies):
        edid, full, _ = world.armo[body]
        print("=" * 78)
        print("BODY %s  %s  (%s)" % (body, full or "<no name>", edid))
        print("     slots: %s" % ", ".join(world.slots(body)))
        naming = []
        for gid, (oedid, items) in world.otft.items():
            flat = []
            for entry in items:
                flat.extend(world.expand(entry))
            if body in flat:
                naming.append((gid, oedid, flat))
        print("     named by %d outfit record(s)" % len(naming))
        by_slot = {}
        for gid, oedid, flat in sorted(naming, key=lambda r: r[1]):
            others = [g for g in flat if g != body]
            if not args.quiet:
                print("  OTFT %s %s" % (gid, oedid))
            for g in others:
                if not args.quiet:
                    print("        %-34s %s" % (world.name(g), ", ".join(world.slots(g))))
                for s in world.slots(g):
                    by_slot.setdefault(s, {}).setdefault(world.name(g), 0)
                    by_slot[s][world.name(g)] += 1
        print("  --- per slot: what the completion pass sees ---")
        for s in sorted(by_slot):
            offers = by_slot[s]
            verdict = "AGREED" if len(offers) == 1 else "contested"
            detail = "; ".join("%s x%d" % (n, c) for n, c in sorted(offers.items()))
            print("     %-12s %-9s %s" % (s, verdict, detail))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
