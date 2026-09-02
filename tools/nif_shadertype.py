"""Which shapes in a NIF wear which Skyrim shader type, and on what name.

Answers the overlay question offline: skee clones an overlay only off a
geometry whose BSLightingShaderProperty carries type 5 (Skin_Tint /
FaceGenRGBTint), so a mesh with none of those never receives an [Ovl] clone
at attach, and the uninstall branch runs instead.

Walks the header the way nif_alpha.py does (block type table, per-block
sizes), so the block offsets are exact. For each BSLightingShaderProperty the
type is the first u32 of the block on every stream this load order ships
(BSVER 83 through 155). Shape names come from the string table via each
geometry block's first field.
"""
import struct
import sys

SHADER_TYPES = {
    0: "Default", 1: "EnvMap", 2: "Glow", 3: "Parallax", 4: "FaceGen",
    5: "Skin_Tint(FaceGenRGBTint)", 6: "Hair_Tint", 7: "ParallaxOcc",
    8: "MultitexLandscape", 9: "LODLandscape", 10: "Snow", 11: "MultiLayerParallax",
    12: "TreeAnim", 13: "LODObjects", 14: "SparkleSnow", 15: "LODObjectsHD",
    16: "EyeEnvmap", 17: "Cloud", 18: "LODLandscapeNoise", 19: "MultitexLandLODBlend",
}

GEOM_TYPES = {"BSTriShape", "BSDynamicTriShape", "BSSubIndexTriShape", "NiTriShape",
              "NiTriStrips"}


def read_sized(data, p):
    (n,) = struct.unpack_from("<I", data, p)
    return data[p + 4:p + 4 + n].decode("latin-1"), p + 4 + n


def read_short(data, p):
    n = data[p]
    return data[p + 1:p + 1 + n].decode("latin-1"), p + 1 + n


def parse(path):
    d = open(path, "rb").read()
    nl = d.index(b"\n")
    p = nl + 1
    (ver,) = struct.unpack_from("<I", d, p); p += 4
    p += 1  # endian
    p += 4  # user version
    (numBlocks,) = struct.unpack_from("<I", d, p); p += 4
    (bsver,) = struct.unpack_from("<I", d, p); p += 4
    _author, p = read_short(d, p)
    if bsver > 130:
        p += 4
    _proc, p = read_short(d, p)
    _export, p = read_short(d, p)
    (numTypes,) = struct.unpack_from("<H", d, p); p += 2
    types = []
    for _ in range(numTypes):
        t, p = read_sized(d, p)
        types.append(t)
    idx = struct.unpack_from("<%dH" % numBlocks, d, p); p += 2 * numBlocks
    sizes = struct.unpack_from("<%dI" % numBlocks, d, p); p += 4 * numBlocks
    (numStrings,) = struct.unpack_from("<I", d, p); p += 4
    p += 4  # max string length
    strings = []
    for _ in range(numStrings):
        s, p = read_sized(d, p)
        strings.append(s)
    (numGroups,) = struct.unpack_from("<I", d, p); p += 4
    p += 4 * numGroups

    offs = []
    off = p
    for i in range(numBlocks):
        offs.append(off)
        off += sizes[i]

    def block_name(i):
        (ref,) = struct.unpack_from("<i", d, offs[i])
        return strings[ref] if 0 <= ref < len(strings) else "?"

    print("%s  bsver=%d blocks=%d" % (path.replace("\\", "/").rsplit("/", 1)[-1],
                                      bsver, numBlocks))
    shader_of = {}   # shader block index -> type
    for i in range(numBlocks):
        if types[idx[i]] == "BSLightingShaderProperty":
            # ⚠ BSVER >= 155 (SSE): the type is the FIRST u32 of the block.
            # BSVER 83 (LE-era files still shipped by mods): same position.
            (stype,) = struct.unpack_from("<I", d, offs[i])
            shader_of[i] = stype

    hits = 0
    for i in range(numBlocks):
        tname = types[idx[i]]
        if tname not in GEOM_TYPES:
            continue
        name = block_name(i)
        # The geometry's shader ref sits at a stream-dependent offset; rather
        # than parse each layout, find which shader block CLAIMS this shape by
        # scanning the geometry block for a plausible ref into shader_of.
        best = None
        q = offs[i]
        end = q + sizes[i]
        while q + 4 <= end:
            (ref,) = struct.unpack_from("<i", d, q)
            if ref in shader_of:
                best = ref
                break
            q += 4
        stype = shader_of.get(best)
        label = SHADER_TYPES.get(stype, str(stype)) if stype is not None else "(none found)"
        marker = "  <== OVERLAY SOURCE" if stype == 5 else ""
        print("  %-14s %-28s shader=%s%s" % (tname, name[:28], label, marker))
        hits += 1
    if not hits:
        print("  (no geometry blocks)")


if __name__ == "__main__":
    for a in sys.argv[1:]:
        try:
            parse(a)
        except Exception as e:
            print(a, "parse failed:", e)
