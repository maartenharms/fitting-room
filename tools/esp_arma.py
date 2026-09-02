"""Dump ARMO and ARMA records from a plugin: slot mask and per-sex model paths.

Answers "which NIF does this armour card actually load, and which window does
its slot mask pick" without opening xEdit.
"""
import struct
import sys
import zlib

SLOT_NAMES = {
    0: "30 head", 1: "31 hair", 2: "32 body", 3: "33 hands", 4: "34 forearms",
    5: "35 amulet", 6: "36 ring", 7: "37 feet", 8: "38 calves", 9: "39 shield",
    10: "40 tail", 11: "41 long hair", 12: "42 circlet", 13: "43 ears",
    14: "44", 15: "45", 16: "46", 17: "47", 18: "48", 19: "49", 20: "50",
    21: "51", 22: "52", 23: "53", 24: "54", 25: "55", 26: "56", 27: "57",
    28: "58", 29: "59", 30: "60", 31: "61",
}


def subrecords(data):
    p = 0
    while p + 6 <= len(data):
        sig = data[p:p + 4].decode("latin-1")
        size, = struct.unpack_from("<H", data, p + 4)
        p += 6
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
        if flags & 0x00040000:                       # compressed
            body = zlib.decompress(body[4:])
        out.append((sig, formid, body))
        p += 24 + size


def main(path, want):
    data = open(path, "rb").read()
    recs = []
    # header record first, then top-level GRUPs
    hsize, = struct.unpack_from("<I", data, 4)
    walk(data, 24 + hsize, len(data), recs)

    arma = {}
    for sig, formid, body in recs:
        if sig != "ARMA":
            continue
        models = []
        edid = ""
        for s, v in subrecords(body):
            if s == "EDID":
                edid = v.rstrip(b"\x00").decode("latin-1")
            elif s in ("MOD2", "MOD3", "MOD4", "MOD5"):
                models.append((s, v.rstrip(b"\x00").decode("latin-1")))
        arma[formid] = (edid, models)

    for sig, formid, body in recs:
        if sig != "ARMO":
            continue
        edid, full, mask, addons, world = "", "", None, [], ""
        for s, v in subrecords(body):
            if s == "EDID":
                edid = v.rstrip(b"\x00").decode("latin-1")
            elif s == "FULL":
                full = v.rstrip(b"\x00").decode("latin-1")
            elif s == "BOD2":
                mask, = struct.unpack_from("<I", v, 0)
            elif s == "BODT":
                mask, = struct.unpack_from("<I", v, 0)
            elif s == "MODL" and len(v) == 4:
                addons.append(struct.unpack_from("<I", v, 0)[0])
            elif s == "MOD2":
                world = v.rstrip(b"\x00").decode("latin-1")
        if want and want.lower() not in (edid + " " + full).lower():
            continue
        slots = [SLOT_NAMES.get(i, str(i + 30))
                 for i in range(32) if mask and (mask >> i) & 1]
        print("ARMO %08X  %-28s  %s" % (formid, edid, full))
        print("     slots: %s   (mask 0x%08X)" % (", ".join(slots) or "none",
                                                  mask or 0))
        if world:
            print("     world model (gnd): %s" % world)
        for a in addons:
            e, models = arma.get(a, ("<not in this plugin>", []))
            print("     ARMA %08X %s" % (a, e))
            for s, m in models:
                tag = {"MOD2": "male", "MOD3": "female",
                       "MOD4": "male 1st", "MOD5": "female 1st"}.get(s, s)
                print("          %-10s %s" % (tag, m))
        print()


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "")
