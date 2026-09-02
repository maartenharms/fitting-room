"""Per-shape: shader type, alpha flags decoded, and the diffuse it wears.

Written 2026-08-21 after a preview filter was authored off a MESH-WIDE alpha
listing and ate the wrong shapes. `nif_alpha.py` prints every NiAlphaProperty in
block order and `nif_shadertype.py` prints every shape, and pairing the two by
eye is a guess: a shape may have no alpha property at all, so the Nth property
is not the Nth shape's. This walks each geometry block's own refs instead, so
the shape and its alpha are read together or not at all.

⚠ THE FLAGS ARE THE POINT. blend enable is bit 0, src is bits 1..4, dst is bits
5..8, TEST enable is bit 9, test func is bits 10..12, no-sorter is bit 13. A
cutout (test on) and an overlay (test off) can carry the same blend modes and
are not the same kind of thing.

Usage: python nif_shape_alpha.py <file.nif> [...]
"""
import struct
import sys

SHADER_TYPES = {
    0: "Default", 1: "EnvMap", 2: "Glow", 3: "Parallax", 4: "FaceGen",
    5: "Skin_Tint", 6: "Hair_Tint", 7: "ParallaxOcc", 8: "MultitexLandscape",
    9: "LODLandscape", 10: "Snow", 11: "MultiLayerParallax", 12: "TreeAnim",
    13: "LODObjects", 14: "SparkleSnow", 15: "LODObjectsHD", 16: "EyeEnvmap",
    17: "Cloud", 18: "LODLandscapeNoise", 19: "MultitexLandLODBlend",
}

BLEND = {0: "ONE", 1: "ZERO", 2: "SRC_COLOR", 3: "INV_SRC_COLOR",
         4: "DEST_COLOR", 5: "INV_DEST_COLOR", 6: "SRC_ALPHA",
         7: "INV_SRC_ALPHA", 8: "DEST_ALPHA", 9: "INV_DEST_ALPHA",
         10: "SRC_ALPHA_SAT"}

TESTF = {0: "ALWAYS", 1: "LESS", 2: "EQUAL", 3: "LESSEQUAL", 4: "GREATER",
         5: "NOTEQUAL", 6: "GREATEREQUAL", 7: "NEVER"}

GEOM_TYPES = {"BSTriShape", "BSDynamicTriShape", "BSSubIndexTriShape",
              "NiTriShape", "NiTriStrips"}


def read_sized(d, p):
    (n,) = struct.unpack_from("<I", d, p)
    return d[p + 4:p + 4 + n].decode("latin-1"), p + 4 + n


def read_short(d, p):
    n = d[p]
    return d[p + 1:p + 1 + n].decode("latin-1"), p + 1 + n


def decode(flags):
    blend = bool(flags & 1)
    src = (flags >> 1) & 0xF
    dst = (flags >> 5) & 0xF
    test = bool((flags >> 9) & 1)
    func = (flags >> 10) & 0x7
    return blend, src, dst, test, func


def parse(path):
    d = open(path, "rb").read()
    p = d.index(b"\n") + 1
    p += 4 + 1 + 4
    (numBlocks,) = struct.unpack_from("<I", d, p); p += 4
    (bsver,) = struct.unpack_from("<I", d, p); p += 4
    _a, p = read_short(d, p)
    if bsver > 130:
        p += 4
    _b, p = read_short(d, p)
    _c, p = read_short(d, p)
    (numTypes,) = struct.unpack_from("<H", d, p); p += 2
    types = []
    for _ in range(numTypes):
        t, p = read_sized(d, p)
        types.append(t)
    idx = struct.unpack_from("<%dH" % numBlocks, d, p); p += 2 * numBlocks
    sizes = struct.unpack_from("<%dI" % numBlocks, d, p); p += 4 * numBlocks
    (numStrings,) = struct.unpack_from("<I", d, p); p += 4
    p += 4
    strings = []
    for _ in range(numStrings):
        s, p = read_sized(d, p)
        strings.append(s)
    (numGroups,) = struct.unpack_from("<I", d, p); p += 4
    p += 4 * numGroups

    offs, off = [], p
    for i in range(numBlocks):
        offs.append(off)
        off += sizes[i]

    def tname(i):
        return types[idx[i]]

    # shader block -> (type, textureset ref); alpha block -> flags/threshold
    shaders, alphas, texsets = {}, {}, {}
    for i in range(numBlocks):
        t = tname(i)
        if t == "BSLightingShaderProperty":
            q = offs[i]
            (stype,) = struct.unpack_from("<I", d, q); q += 4
            q += 4                                    # name ref
            (nx,) = struct.unpack_from("<I", d, q); q += 4 + 4 * nx
            (ctrl,) = struct.unpack_from("<i", d, q); q += 4
            (f1,) = struct.unpack_from("<I", d, q); q += 4
            (f2,) = struct.unpack_from("<I", d, q); q += 4
            q += 8 + 8                                # uv offset, scale
            (ts,) = struct.unpack_from("<i", d, q); q += 4
            emis = struct.unpack_from("<3f", d, q); q += 12
            (emult,) = struct.unpack_from("<f", d, q); q += 4
            q += 4                                    # texture clamp mode
            (alpha,) = struct.unpack_from("<f", d, q)
            shaders[i] = (stype, ts, alpha, emult, emis, ctrl)
        elif t == "BSEffectShaderProperty":
            shaders[i] = (-1, -1, 1.0, 1.0, (0, 0, 0), -1)
        elif t == "NiAlphaProperty":
            q = offs[i]
            q += 4                                    # name ref
            (nx,) = struct.unpack_from("<I", d, q); q += 4 + 4 * nx
            q += 4                                    # controller
            (flags,) = struct.unpack_from("<H", d, q); q += 2
            thr = d[q]
            alphas[i] = (flags, thr)
        elif t == "BSShaderTextureSet":
            q = offs[i]
            (n,) = struct.unpack_from("<I", d, q); q += 4
            paths = []
            for _ in range(n):
                s, q = read_sized(d, q)
                paths.append(s)
            texsets[i] = paths

    print("%s  bsver=%d blocks=%d" % (path.replace("\\", "/").rsplit("/", 1)[-1],
                                      bsver, numBlocks))
    for i in range(numBlocks):
        if tname(i) not in GEOM_TYPES:
            continue
        (nameRef,) = struct.unpack_from("<i", d, offs[i])
        name = strings[nameRef] if 0 <= nameRef < len(strings) else "?"
        # scan the block for refs into the shader / alpha tables
        sh = al = None
        q, end = offs[i], offs[i] + sizes[i]
        while q + 4 <= end:
            (ref,) = struct.unpack_from("<i", d, q)
            if sh is None and ref in shaders:
                sh = ref
            elif al is None and ref in alphas:
                al = ref
            q += 4
        if sh is None:
            stxt = "(no shader)"
            diff = ""
        else:
            stype, ts, salpha, emult, emis, ctrl = shaders[sh]
            stxt = "EFFECT" if stype < 0 else SHADER_TYPES.get(stype, str(stype))
            diff = texsets.get(ts, [""])[0] if ts in texsets else ""
            extra = "  shaderAlpha=%.3f emissiveMult=%.3f ctrl=%s" % (
                salpha, emult, "yes" if ctrl >= 0 else "no")
        if al is None:
            atxt = "(no alpha property)"
        else:
            flags, thr = alphas[al]
            blend, src, dst, test, func = decode(flags)
            atxt = "0x%04X blend=%s" % (flags, "on " if blend else "off")
            if blend:
                atxt += " %s->%s" % (BLEND.get(src, src), BLEND.get(dst, dst))
            atxt += "  test=%s" % ("on" if test else "off")
            if test:
                atxt += " %s>%d" % (TESTF.get(func, func), thr)
        print("  %-22s %-10s %s" % (name[:22], stxt, atxt))
        if sh is not None:
            print("  %-22s  %s" % ("", extra))
        if diff:
            print("  %-22s   diffuse=%s" % ("", diff))


if __name__ == "__main__":
    for a in sys.argv[1:]:
        try:
            parse(a)
        except Exception as e:
            print(a, "parse failed:", e)
