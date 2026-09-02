"""What a skinned shape's partition bone MAP actually says, per shape.

Written 2026-08-21 to settle whether `NpcHairPlan::JudgePartition` is refusing
real defects or refusing legal meshes. That gate reads partition 0's bone map
and calls the mesh defective when the map is SHORTER than the skin's bone list,
on the theory that a vertex could then sample a matrix that was never uploaded.

The two index spaces that theory turns on:

  * a partition's `bones[]` is a PALETTE of GLOBAL bone indices into the skin
    instance's bone list, so a palette entry >= the skin's bone count really is
    unaddressable and really is a defect;
  * a vertex's bone indices are LOCAL indices into that palette, bounded by
    `numBones`, so a palette SHORTER than the skin's bone list is just a
    partition that does not use every bone.

This prints both numbers per shape so the question is answered by measurement.

⚠⚠ THE SSE VERTEX BLOCK IS SKIPPED BY ITS DECLARED SIZE, NEVER WALKED. On
BSVersion >= 100 `NiSkinPartition` carries the vertex buffer between its header
and its partitions, and byte-walking that buffer is the trap the body-card round
already paid for (see the memory note on BS83 partitions). `dataSize` says
exactly how many bytes to step over, so the partitions are reached without
decoding a single vertex.

Usage: python nif_skin_partition.py <nif> [<nif> ...]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nif_bounds import header

SKININST = ("NiSkinInstance", "BSDismemberSkinInstance", "BSSkinInstance")


def block_starts(h):
    starts, p = [], h["start"]
    for sz in h["sizes"]:
        starts.append(p)
        p += sz
    return starts


def skin_data_bones(d, start):
    """NiSkinData's own bone count. Layout: NiTransform (52) then uint bones."""
    return struct.unpack_from("<I", d, start + 52)[0]


def max_bone_index(d, vertBase, dataSize, vertexSize, desc, rows=None):
    """The largest LOCAL bone index any vertex uses, or None if unreadable.

    ⚠ THIS IS THE ONLY NUMBER THAT DECIDES THE FAULT. A palette shorter than the
    skin's bone list is legal; a vertex whose local index runs off the END of
    that palette is not, and that is the case the original defect was.

    The offset comes from the descriptor's own nibble for VA_SKINNING, never
    from a guessed struct layout: attribute i sits at ((desc >> (4i+4)) & 0xF)*4.
    Skinning packs four half weights ahead of four byte indices.
    """
    skinOff = ((desc >> (4 * 6 + 4)) & 0xF) * 4
    if skinOff == 0 or vertexSize == 0:
        return None
    idxOff = skinOff + 8                      # past the four half weights
    if idxOff + 4 > vertexSize:
        return None
    n = dataSize // vertexSize
    worst = -1
    for v in (range(n) if rows is None else rows):
        if v >= n:
            continue
        base = vertBase + v * vertexSize + idxOff
        for k in range(4):
            b = d[base + k]
            if b > worst:
                worst = b
    return worst


def partitions(d, start, bsver):
    """(numPartitions, [palette per partition]) without touching vertex data."""
    p = start
    numPartitions, = struct.unpack_from("<I", d, p)
    p += 4
    vertBase = dataSize = vertexSize = desc = 0
    if bsver >= 100:
        dataSize, = struct.unpack_from("<I", d, p); p += 4
        vertexSize, = struct.unpack_from("<I", d, p); p += 4
        desc, = struct.unpack_from("<Q", d, p); p += 8
        vertBase = p
        p += dataSize               # ⚠ stepped over, never decoded
    out = []
    for _ in range(numPartitions):
        numVertices, numTriangles, numBones, numStrips, numWeights = \
            struct.unpack_from("<5H", d, p)
        p += 10
        palette = struct.unpack_from("<%dH" % numBones, d, p)
        p += 2 * numBones
        # ⚠ THE SHARED BUFFER IS NOT THIS PARTITION'S VERTEX SET. On SSE the
        # vertex data spans every partition, so a max taken over the whole
        # buffer answers a question nobody asked. `vertexMap` names the rows
        # that belong to THIS partition, which is what the palette is judged
        # against.
        hasVertexMap = d[p]; p += 1
        vmap = None
        if hasVertexMap:
            vmap = struct.unpack_from("<%dH" % numVertices, d, p)
            p += 2 * numVertices
        out.append(dict(numVertices=numVertices, numBones=numBones,
                        numWeights=numWeights, palette=palette,
                        maxAll=max_bone_index(d, vertBase, dataSize,
                                              vertexSize, desc),
                        maxOwn=max_bone_index(d, vertBase, dataSize,
                                              vertexSize, desc, vmap)))
        # Only partition 0's palette is needed, and stepping the rest of a
        # partition means decoding the very buffers this refuses to decode.
        break
    return numPartitions, out


def dump(path):
    d = open(path, "rb").read()
    h = header(d)
    starts = block_starts(h)
    types = [h["types"][h["idx"][i]] for i in range(h["numBlocks"])]
    shapeOf = {}
    for i, t in enumerate(types):
        if t in ("BSTriShape", "BSSubIndexTriShape", "BSDynamicTriShape",
                 "BSMeshLODTriShape", "NiTriShape"):
            nameIdx, = struct.unpack_from("<i", d, starts[i])
            shapeOf[i] = h["strings"][nameIdx] if 0 <= nameIdx < len(h["strings"]) else "?"

    print("=== %s  (bsver=%d)" % (os.path.basename(path), h["bsver"]))
    found = False
    for i, t in enumerate(types):
        if t not in SKININST:
            continue
        found = True
        b = starts[i]
        dataRef, partRef, rootRef, nbones = struct.unpack_from("<iiiI", d, b)
        owner = "?"
        for si, sn in shapeOf.items():
            if si < i:
                owner = sn
        skinBones = None
        if 0 <= dataRef < h["numBlocks"] and types[dataRef] == "NiSkinData":
            skinBones = skin_data_bones(d, starts[dataRef])
        if not (0 <= partRef < h["numBlocks"]) or types[partRef] != "NiSkinPartition":
            print("  [%2d] %-26s instBones=%-3d skinDataBones=%-4s  NO PARTITION"
                  % (i, t, nbones, skinBones))
            continue
        try:
            nparts, parts = partitions(d, starts[partRef], h["bsver"])
        except Exception as e:
            print("  [%2d] %-26s partition parse failed: %s" % (i, t, e))
            continue
        p0 = parts[0] if parts else None
        maxEntry = max(p0["palette"]) if p0 and p0["palette"] else 0
        ref = skinBones if skinBones is not None else nbones
        shortMap = p0 and p0["numBones"] < ref
        outOfRange = p0 and p0["palette"] and maxEntry >= ref
        maxLocal = p0["maxOwn"] if p0 else None
        overflow = (p0 and maxLocal is not None and p0["numBones"] > 0 and
                    maxLocal >= p0["numBones"])
        verdict = []
        if nparts != 1:
            verdict.append("EXEMPT(multi-partition)")
        else:
            if shortMap:
                verdict.append("FR-flags:mapLen<skinBones")
            if outOfRange:
                verdict.append("DEFECT:paletteEntry>=skinBones")
            if overflow:
                verdict.append("DEFECT:vertexLocalIdx>=mapLen")
            if not verdict:
                verdict.append("clean")
        print("  [%2d] %-16s skinBones=%-4s parts=%d p0.numBones=%-3s "
              "maxPalette=%-4s p0vertMax=%-4s allVertMax=%-4s w/v=%-2s  %s"
              % (i, t.replace("BSDismemberSkinInstance", "BSDismember"),
                 skinBones, nparts,
                 p0["numBones"] if p0 else "-", maxEntry if p0 else "-",
                 "?" if maxLocal is None else maxLocal,
                 "?" if not p0 or p0["maxAll"] is None else p0["maxAll"],
                 p0["numWeights"] if p0 else "-", "; ".join(verdict)))
    if not found:
        print("  (no skin instance)")


if __name__ == "__main__":
    for a in sys.argv[1:]:
        try:
            dump(a)
        except Exception as e:
            print(a, "FAILED:", e)
