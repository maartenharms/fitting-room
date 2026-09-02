# -*- coding: utf-8 -*-
"""Build an EDID -> (plugin, local form id) index for QUST and LCTN.

    python tools\make_form_index.py

Writes tools/data/form_index.json. READ ONLY on the game's files.

WHY THIS EXISTS. The lore unlock rules name roughly ninety forms by hand, and
an unverified form id parses, never matches, and locks its colour for the life
of the character with no log line anywhere. Authoring against editor IDs and
resolving them here means a typo fails a build instead of a save. See
docs/superpowers/specs/2026-08-06-lore-dye-unlocks-design.md.

Two steps nobody else does for us:
  1. read strings/<plugin>_english.strings out of the game's own BSAs
  2. walk the masters keeping FULL's 4 byte lstring id, then join

The record walking is the same machinery tools/make_lore_esp.py already uses.
"""
import collections, io, json, os, struct, zlib

import lz4.frame

DATA = r"C:\Program Files (x86)\Steam\steamapps\common\Skyrim Special Edition\Data"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "data", "form_index.json")

PLUGINS = ["Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm"]

# One archive carries every plugin's table for every language. Patch.bsa is read
# LAST so its corrected strings win over Interface.bsa.
BSAS = ["Skyrim - Interface.bsa", "Skyrim - Patch.bsa"]

# ⚠ EACH PLUGIN HAS ITS OWN LSTRING ID SPACE. Merging them collides: doing that
# gave DB01 the name "What if she's right and I don't find anything?" from a
# different plugin's id 0x... Look every FULL up in its own plugin's table.
STEM = {"Skyrim.esm": "skyrim", "Update.esm": "update", "Dawnguard.esm": "dawnguard",
        "HearthFires.esm": "hearthfires", "Dragonborn.esm": "dragonborn"}

WANT = {b"QUST", b"LCTN"}
REC_HDR = 24
FLAG_COMPRESSED = 0x00040000


# ---------------------------------------------------------------- BSA

def bsa_files(path):
    """Yield (full_name_lower, data_bytes) for every file in a v104/v105 BSA."""
    with io.open(path, "rb") as f:
        buf = f.read()
    (magic, ver, folder_ofs, flags, folder_count, file_count,
     total_folder_name_len, total_file_name_len) = struct.unpack_from("<4sIIIIIII", buf, 0)
    if magic != b"BSA\x00":
        return
    default_compressed = bool(flags & 0x4)
    embedded_names = bool(flags & 0x100)
    frec = 24 if ver >= 105 else 16

    folders, off = [], folder_ofs
    for _ in range(folder_count):
        if ver >= 105:
            _h, cnt, _p1, ofs, _p2 = struct.unpack_from("<QIIII", buf, off)
        else:
            _h, cnt, ofs = struct.unpack_from("<QII", buf, off)
        folders.append((cnt, ofs))
        off += frec

    # A folder record's offset counts the FILE NAME block, so subtract it back off.
    files, name_block_at = [], 0
    for cnt, ofs in folders:
        p = ofs - total_file_name_len
        nlen = buf[p]                       # length includes the null terminator
        folder = buf[p + 1:p + nlen].decode("cp1252", "replace").rstrip("\x00")
        p += 1 + nlen
        for _ in range(cnt):
            _h, size, doff = struct.unpack_from("<QII", buf, p)
            p += 16
            files.append([folder, size, doff])
        name_block_at = max(name_block_at, p)

    blk = buf[name_block_at:name_block_at + total_file_name_len]
    names = [raw.decode("cp1252", "replace") for raw in blk.split(b"\x00")[:file_count]]

    for (folder, size, doff), name in zip(files, names):
        comp = default_compressed
        if size & 0x40000000:
            comp = not comp
        real = size & 0x3FFFFFFF
        d = doff
        if embedded_names:
            n = buf[d]
            d += 1 + n
            real -= 1 + n
        raw = buf[d:d + real]
        if comp:
            usize = struct.unpack_from("<I", raw, 0)[0]
            body = raw[4:]
            try:
                data = lz4.frame.decompress(body) if ver >= 105 else zlib.decompress(body)
            except Exception:
                try:
                    data = zlib.decompress(body)
                except Exception:
                    continue
            if len(data) != usize:
                pass
        else:
            data = raw
        yield ("%s\\%s" % (folder, name)).lower(), data


def parse_strings(data):
    """count, dirsize, [(id, offset)], then a data block of C strings."""
    count, dsize = struct.unpack_from("<II", data, 0)
    base = 8 + count * 8
    out = {}
    for i in range(count):
        sid, ofs = struct.unpack_from("<II", data, 8 + i * 8)
        p = base + ofs
        end = data.find(b"\x00", p)
        out[sid] = data[p:end].decode("cp1252", "replace")
    return out


def load_all_strings():
    table = {}
    for bsa in BSAS:
        path = os.path.join(DATA, bsa)
        if not os.path.exists(path):
            continue
        got = 0
        for name, data in bsa_files(path):
            if name.endswith(".strings") and "_english" in name:
                try:
                    m = parse_strings(data)
                except Exception:
                    continue
                stem = os.path.basename(name).split("_english")[0]
                table.setdefault(stem, {}).update(m)
                got += len(m)
        print("%-26s %6d strings" % (bsa, got))
    return table


# ---------------------------------------------------------------- ESM

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


def walk(buf, plugin, out):
    if buf[0:4] != b"TES4":
        return
    off = REC_HDR + struct.unpack_from("<I", buf, 4)[0]
    n = len(buf)
    while off + 24 <= n:
        if buf[off:off + 4] != b"GRUP":
            break
        gsize = struct.unpack_from("<I", buf, off + 4)[0]
        if buf[off + 8:off + 12] not in WANT:
            off += gsize
            continue
        end, p = off + gsize, off + 24
        while p + REC_HDR <= end:
            sig = buf[p:p + 4]
            if sig == b"GRUP":
                p += struct.unpack_from("<I", buf, p + 4)[0]
                continue
            dsize, flags, formid = struct.unpack_from("<III", buf, p + 4)
            if sig in WANT:
                raw = buf[p + REC_HDR:p + REC_HDR + dsize]
                if flags & FLAG_COMPRESSED:
                    try:
                        raw = zlib.decompress(raw[4:])
                    except zlib.error:
                        raw = b""
                edid = lsid = None
                for ssig, sdata in subrecords(raw):
                    if ssig == b"EDID":
                        edid = sdata.rstrip(b"\x00").decode("cp1252", "replace")
                    elif ssig == b"FULL" and len(sdata) == 4:
                        lsid = struct.unpack_from("<I", sdata, 0)[0]
                if edid:
                    key = (sig.decode(), edid)
                    if key not in out:
                        out[key] = {"kind": sig.decode(), "edid": edid, "plugin": plugin,
                                    "formId": "%06X" % (formid & 0x00FFFFFF), "lsid": lsid}
                    elif out[key]["lsid"] is None and lsid is not None:
                        out[key]["lsid"] = lsid       # an override supplied the name
            p += REC_HDR + dsize
        off = end


def main():
    strings = load_all_strings()
    print("tables: %s" % {k: len(v) for k, v in sorted(strings.items())})

    out = {}
    for name in PLUGINS:
        path = os.path.join(DATA, name)
        if os.path.exists(path):
            walk(io.open(path, "rb").read(), name, out)

    rows = []
    for r in sorted(out.values(), key=lambda x: (x["kind"], x["edid"])):
        table = strings.get(STEM.get(r["plugin"], ""), {})
        r["name"] = table.get(r["lsid"]) if r["lsid"] else None
        r.pop("lsid", None)
        rows.append(r)

    named = sum(1 for r in rows if r["name"])
    c = collections.Counter(r["kind"] for r in rows)
    io.open(OUT, "w", encoding="utf-8").write(json.dumps(rows, ensure_ascii=False, indent=1))
    print("rows %s, named %d of %d -> %s" % (dict(c), named, len(rows), OUT))


if __name__ == "__main__":
    main()
