# -*- coding: utf-8 -*-
"""Count what Skyrim's own stat counters can actually reach.

A threshold like "Locations Discovered >= 50" is meaningless without the
denominator. This reads both out of the masters:

  * quests per type, which is what Game.QueryStat's per-line counters count
  * discoverable map markers, which is what Locations Discovered counts

Read only. Prints a table.
"""
import collections, io, os, struct, zlib

DATA = r"C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data"
PLUGINS = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"]
REC_HDR = 24
FLAG_COMPRESSED = 0x00040000

# QUST DNAM's questType byte. Names are the engine's own quest-type list, which
# is what the per-line "X Quests Completed" stats are keyed on.
QTYPE = {0: "None", 1: "Main Quest", 2: "College of Winterhold", 3: "Thieves Guild",
         4: "Dark Brotherhood", 5: "Companions", 6: "Miscellaneous", 7: "Daedric",
         8: "Side Quest", 9: "Civil War", 10: "Dawnguard", 11: "Dragonborn"}


def subrecords(data):
    off, override = 0, None
    n = len(data)
    while off + 6 <= n:
        sig = data[off:off + 4]
        sz = struct.unpack_from("<H", data, off + 4)[0]
        off += 6
        if sig == b"XXXX":
            override = struct.unpack_from("<I", data, off)[0]
            off += sz
            continue
        if override is not None:
            sz, override = override, None
        yield sig, data[off:off + sz]
        off += sz


def payload(buf, p, dsize, flags):
    raw = buf[p + REC_HDR:p + REC_HDR + dsize]
    if flags & FLAG_COMPRESSED:
        try:
            return zlib.decompress(raw[4:])
        except zlib.error:
            return b""
    return raw


def walk(buf, on_record):
    """Recurse every GRUP, including the nested cell/world children."""
    if buf[0:4] != b"TES4":
        return
    n = len(buf)
    stack = [(REC_HDR + struct.unpack_from("<I", buf, 4)[0], n)]
    while stack:
        p, end = stack.pop()
        while p + REC_HDR <= end:
            sig = buf[p:p + 4]
            if sig == b"GRUP":
                gsize = struct.unpack_from("<I", buf, p + 4)[0]
                if gsize < REC_HDR:
                    break
                stack.append((p + REC_HDR, p + gsize))   # descend
                p += gsize
                continue
            dsize, flags, formid = struct.unpack_from("<III", buf, p + 4)
            on_record(sig, p, dsize, flags, formid)
            p += REC_HDR + dsize


def main():
    qtypes = collections.Counter()
    markers = collections.Counter()
    seen_q, seen_m = set(), set()

    for name in PLUGINS:
        path = os.path.join(DATA, name)
        if not os.path.exists(path):
            continue
        buf = io.open(path, "rb").read()

        def on_record(sig, p, dsize, flags, formid, _buf=buf, _plug=name):
            if sig == b"QUST":
                if formid in seen_q:
                    return
                for ssig, sdata in subrecords(payload(_buf, p, dsize, flags)):
                    if ssig == b"DNAM" and len(sdata) >= 12:
                        t = struct.unpack_from("<I", sdata, 8)[0]
                        # a quest with no name is plumbing, not a player quest
                        qtypes[QTYPE.get(t, "type %d" % t)] += 1
                        seen_q.add(formid)
                        break
            elif sig == b"REFR":
                if formid in seen_m:
                    return
                for ssig, sdata in subrecords(payload(_buf, p, dsize, flags)):
                    if ssig == b"XMRK":
                        markers[_plug] += 1
                        seen_m.add(formid)
                        break

        walk(buf, on_record)
        print("  %-16s markers so far %4d" % (name, sum(markers.values())))

    print("\n=== quests by type (what the per-line stats count) ===")
    for k, v in sorted(qtypes.items(), key=lambda kv: -kv[1]):
        print("  %-24s %4d" % (k, v))

    print("\n=== discoverable map markers (what Locations Discovered counts) ===")
    for k, v in markers.items():
        print("  %-16s %4d" % (k, v))
    print("  %-16s %4d" % ("TOTAL", sum(markers.values())))


if __name__ == "__main__":
    main()
