#!/usr/bin/env python3
"""List the morph names inside a BodySlide .tri, per shape.

Written 2026-08-26 to answer "does this body carry a PushUp slider at runtime",
and kept because the answer differs per body and the question keeps coming back.

  python tools/tri_morphs.py <file.tri> [more.tri ...]
  python tools/tri_morphs.py --filter push,cleav <file.tri>

The format is the one BodyMorphTri.h measured over 3238 files on the reference
load order, and this walker is deliberately the same shape as that header:

  char  magic[4]   "PIRT"
  u16   shapeCount
  per shape:
    u8  nameLen, char name[nameLen]
    u16 morphCount
    per morph:
      u8  nameLen, char name[nameLen]
      f32 multiplier
      u16 vertexCount
      vertexCount * 8 bytes   (u16 index, then three i16 deltas)

The delta blocks are skipped by arithmetic and never read. A body tri is several
megabytes of vertex deltas and we only ever want the names.

What it has already settled, so nobody has to measure it twice:

  CBBE  femalebody.tri      97 morphs   PushUp, BreastCleavage        present
  3BA   femalebody.tri     154 morphs   PushUp, BreastCleavage        present
  UBE   femalebody_tangent.tri  238 morphs   NEITHER

A slider only becomes a runtime morph when BodySlide built the mesh with morphs
enabled, so a name missing here is missing from SetMorph too, whatever the
slider list in BodySlide shows.
"""
import os
import struct
import sys


def morphs(path):
    """[(shapeName, [morphName, ...]), ...], or None if it is not a PIRT file."""
    with open(path, "rb") as fh:
        b = fh.read()
    if b[:4] != b"PIRT":
        return None
    at = 4
    (shapes,) = struct.unpack_from("<H", b, at)
    at += 2
    out = []
    for _ in range(shapes):
        n = b[at]
        at += 1
        shape = b[at:at + n].decode("latin-1")
        at += n
        (count,) = struct.unpack_from("<H", b, at)
        at += 2
        names = []
        for _ in range(count):
            n = b[at]
            at += 1
            names.append(b[at:at + n].decode("latin-1"))
            at += n
            at += 4  # multiplier
            (verts,) = struct.unpack_from("<H", b, at)
            at += 2
            at += verts * 8  # skipped by arithmetic, never read
        out.append((shape, names))
    return out


def main(argv):
    terms = []
    files = []
    i = 0
    while i < len(argv):
        if argv[i] == "--filter" and i + 1 < len(argv):
            terms = [t.strip().lower() for t in argv[i + 1].split(",") if t.strip()]
            i += 2
            continue
        files.append(argv[i])
        i += 1

    if not files:
        print(__doc__)
        return 2

    for path in files:
        print("=== %s" % os.path.basename(path))
        try:
            parsed = morphs(path)
        except Exception as exc:  # a truncated or hand-edited file
            print("  PARSE FAILED: %s" % exc)
            continue
        if parsed is None:
            print("  not a PIRT file")
            continue
        for shape, names in parsed:
            if terms:
                hits = [n for n in names if any(t in n.lower() for t in terms)]
                print("  %-30s %4d morph(s)  matching: %s"
                      % (shape[:30], len(names), ", ".join(hits) if hits else "NONE"))
            else:
                print("  %-30s %4d morph(s)" % (shape[:30], len(names)))
                for n in names:
                    print("      %s" % n)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
