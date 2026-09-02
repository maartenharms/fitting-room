"""Read every NiAlphaProperty in a NIF: blend enable, test enable, threshold.

Walks the header properly (block type table, per-block sizes) rather than
guessing, so the block offsets are exact.
"""
import struct
import sys


def read_sized(data, p):
    """SizedString: u32 length. Used for block type names and the string table."""
    (n,) = struct.unpack_from("<I", data, p)
    return data[p + 4:p + 4 + n].decode("latin-1"), p + 4 + n


def read_short(data, p):
    """ShortString: u8 length, NUL terminated. The BS stream header uses these,
    which is what a u32 read gets catastrophically wrong."""
    n = data[p]
    return data[p + 1:p + 1 + n].decode("latin-1"), p + 1 + n


def parse(path):
    d = open(path, "rb").read()
    # header string, newline terminated
    nl = d.index(b"\n")
    p = nl + 1
    (ver,) = struct.unpack_from("<I", d, p); p += 4
    endian = d[p]; p += 1
    (user,) = struct.unpack_from("<I", d, p); p += 4
    (numBlocks,) = struct.unpack_from("<I", d, p); p += 4
    (bsver,) = struct.unpack_from("<I", d, p); p += 4
    author, p = read_short(d, p)
    if bsver > 130:
        (_,) = struct.unpack_from("<I", d, p); p += 4
    proc, p = read_short(d, p)
    export, p = read_short(d, p)
    (numTypes,) = struct.unpack_from("<H", d, p); p += 2
    types = []
    for _ in range(numTypes):
        t, p = read_sized(d, p)
        types.append(t)
    idx = struct.unpack_from("<%dH" % numBlocks, d, p); p += 2 * numBlocks
    sizes = struct.unpack_from("<%dI" % numBlocks, d, p); p += 4 * numBlocks
    (numStrings,) = struct.unpack_from("<I", d, p); p += 4
    (maxStr,) = struct.unpack_from("<I", d, p); p += 4
    for _ in range(numStrings):
        _s, p = read_sized(d, p)
    (numGroups,) = struct.unpack_from("<I", d, p); p += 4
    p += 4 * numGroups

    print("%s  ver=%08X bsver=%d blocks=%d" % (path.rsplit("\\", 1)[-1], ver, bsver, numBlocks))
    off = p
    found = 0
    for i in range(numBlocks):
        tname = types[idx[i]]
        if tname == "NiAlphaProperty":
            # NiObjectNET: name (int string ref), then extra data / controller
            # refs. For NiAlphaProperty the payload after those is flags(u16)
            # + threshold(u8). Layout: name(4) numExtra(4) [refs] controller(4)
            q = off + 4
            (numExtra,) = struct.unpack_from("<I", d, q); q += 4
            q += 4 * numExtra
            q += 4  # controller ref
            (flags,) = struct.unpack_from("<H", d, q); q += 2
            threshold = d[q]
            blend = bool(flags & 1)
            test = bool((flags >> 9) & 1)
            print("  NiAlphaProperty flags=%d (0x%04X) blend=%s test=%s threshold=%d (%.3f)"
                  % (flags, flags, blend, test, threshold, threshold / 255.0))
            found += 1
        off += sizes[i]
    if not found:
        print("  (no NiAlphaProperty)")


if __name__ == "__main__":
    for a in sys.argv[1:]:
        try:
            parse(a)
        except Exception as e:
            print(a, "parse failed:", e)
