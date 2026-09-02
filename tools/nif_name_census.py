"""Census of NIF sized-strings across a mod tree.

NIF stores names as SizedString: uint32 little-endian length, then that many
bytes, no terminator. Scanning for that pattern is reliable enough for a census
and needs no version-specific header walk, which is the part that differs
between Bethesda stream versions.

Usage: python nif_name_census.py <root> [<root> ...]
"""
import os
import re
import struct
import sys
from collections import Counter, defaultdict

PRINTABLE = re.compile(rb"^[ -~]+$")


def sized_strings(data):
    out = []
    n = len(data)
    i = 0
    while i + 4 <= n:
        (ln,) = struct.unpack_from("<I", data, i)
        # Node names in practice are short. The bound also keeps a random u32
        # from swallowing the rest of the file.
        if 2 <= ln <= 128 and i + 4 + ln <= n:
            chunk = data[i + 4 : i + 4 + ln]
            if PRINTABLE.match(chunk):
                out.append(chunk.decode("ascii"))
                i += 4 + ln
                continue
        i += 1
    return out


def main(roots):
    names = Counter()
    where = defaultdict(set)
    files = 0
    for root in roots:
        for dirpath, _dirs, filenames in os.walk(root):
            for fn in filenames:
                if not fn.lower().endswith(".nif"):
                    continue
                path = os.path.join(dirpath, fn)
                try:
                    with open(path, "rb") as f:
                        data = f.read()
                except OSError:
                    continue
                files += 1
                for s in set(sized_strings(data)):
                    names[s] += 1
                    where[s].add(path)

    print("files scanned: %d" % files)
    print("distinct strings: %d" % len(names))

    # The question this census exists to answer: what does an SMP collision
    # proxy look like in THIS load order, and is "Col" a pattern or one name.
    print("\n--- strings containing 'col' (case-insensitive) ---")
    for s, c in sorted(names.items()):
        if "col" in s.lower():
            print("%5d  %s" % (c, s))

    print("\n--- strings starting with 'Col' ---")
    for s, c in sorted(names.items()):
        if s.lower().startswith("col") or "_col" in s.lower():
            ex = sorted(where[s])[0]
            print("%5d  %-45s %s" % (c, s, ex))


if __name__ == "__main__":
    main(sys.argv[1:])
