"""Vertex AABB of every shape in a NIF, in the file's own space.

The framing views in `PreviewFraming.h` are fractions of the mannequin's box,
and that box is built at runtime from vertices (`MeshExtractor`), never from
the authored bound. A skinned NIF's `modelBound` reads zero, so the authored
sphere cannot answer where a body actually stands; this walks the vertex
buffers instead and reports the same numbers the renderer would measure.

Walks the header the way `nif_alpha.py` does (block type table, per-block
sizes), so block starts are exact rather than searched for.

⚠ The position field's width comes from the DESCRIPTOR'S OWN LAYOUT, never
from VF_FULLPREC: the next attribute's offset bounds the position bytes. That
flag lied in the field (MeshExtractor.cpp says so at length).

Usage: python nif_bounds.py <nif> [<nif> ...]
"""
import struct
import sys

SHAPES = ("BSTriShape", "BSSubIndexTriShape", "BSDynamicTriShape",
          "BSMeshLODTriShape")

# The vertex flags sit in the descriptor's top bits, and VF_VERTEX is the
# lowest of them: clear means the partition carries no positions at all, which
# is every head part on this rig (its positions are in the dynamic buffer).
def has_positions(desc):
    return bool((desc >> 44) & 1)


def read_sized(data, p):
    (n,) = struct.unpack_from("<I", data, p)
    return data[p + 4:p + 4 + n].decode("latin-1"), p + 4 + n


def read_short(data, p):
    n = data[p]
    return data[p + 1:p + 1 + n].decode("latin-1"), p + 1 + n


def half(u):
    """IEEE 754 binary16 to float, the way the vertex buffers store position."""
    return struct.unpack("<e", struct.pack("<H", u))[0]


def header(d):
    nl = d.index(b"\n")
    p = nl + 1
    (ver,) = struct.unpack_from("<I", d, p); p += 4
    p += 1  # endian
    (user,) = struct.unpack_from("<I", d, p); p += 4
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
    return dict(ver=ver, bsver=bsver, numBlocks=numBlocks, types=types,
                idx=idx, sizes=sizes, strings=strings, start=p)


def pos_format(desc, stride):
    """Half or full floats, decided by the descriptor's OWN layout.

    nibble[attr + 1] * 4 is the attribute's offset, so VA_TEXCOORD0's nibble
    bounds the position bytes: 8 means four halves, 12 or more means floats.
    VF_FULLPREC is never consulted; it lied in the field.
    """
    uvOff = (desc >> (4 * 1 + 2)) & 0x3C
    if uvOff >= 12:
        return "float"
    if uvOff:
        return "half"
    return "float" if stride >= 12 else "half"


def read_positions(d, base, count, stride, kind):
    out = []
    for i in range(count):
        q = base + i * stride
        if kind == "float":
            out.append(struct.unpack_from("<3f", d, q))
        else:
            out.append(tuple(half(v) for v in struct.unpack_from("<3H", d, q)))
    return out


def aabb(pts):
    mn = [min(p[i] for p in pts) for i in range(3)]
    mx = [max(p[i] for p in pts) for i in range(3)]
    return mn, mx


def apply(pts, trans, rot, scale):
    """The shape's own transform, because a head part carries one.

    A body mesh sits at the identity and a face part does not: EyesFemale is
    authored around the origin and placed by its own translation, so vertices
    read raw put the eye 120 units below where the renderer sees it.
    """
    out = []
    for x, y, z in pts:
        out.append((
            trans[0] + scale * (rot[0] * x + rot[1] * y + rot[2] * z),
            trans[1] + scale * (rot[3] * x + rot[4] * y + rot[5] * z),
            trans[2] + scale * (rot[6] * x + rot[7] * y + rot[8] * z)))
    return out


def shape_block(d, off, strings, end=0):
    """A shape's own header: its name, and its inline vertex data if it has any.

    ⚠ A SKINNED BODY HAS NONE. Its geometry lives in the NiSkinPartition that
    follows, which is why a walker that only reads shapes reports an empty
    file for every body mesh on the instance.
    """
    q = off
    (nameRef,) = struct.unpack_from("<i", d, q); q += 4
    name = strings[nameRef] if 0 <= nameRef < len(strings) else "?"
    (numExtra,) = struct.unpack_from("<I", d, q); q += 4
    q += 4 * numExtra
    q += 4                                    # controller ref
    q += 4                                    # flags, u32 on this stream
    trans = struct.unpack_from("<3f", d, q); q += 12
    rot = struct.unpack_from("<9f", d, q); q += 36
    (scale,) = struct.unpack_from("<f", d, q); q += 4
    q += 4                                    # collision object ref
    sphere = struct.unpack_from("<4f", d, q); q += 16
    q += 12                                   # skin, shader, alpha refs
    (desc,) = struct.unpack_from("<Q", d, q); q += 8
    (numTri,) = struct.unpack_from("<H", d, q); q += 2
    (numVert,) = struct.unpack_from("<H", d, q); q += 2
    (dataSize,) = struct.unpack_from("<I", d, q); q += 4
    pts = None
    if numVert and dataSize:
        stride = dataSize // numVert
        pts = read_positions(d, q, numVert, stride, pos_format(desc, stride))
    elif numVert and end:
        # ⚠ A DYNAMIC SHAPE KEEPS ITS POSITIONS IN THE MORPH BUFFER, and its
        # partition has VF_VERTEX clear, so offset 0 there is somebody else's
        # attribute. Every head part on this rig is one. The buffer is a vec4
        # per vertex behind a size word; find that word rather than trusting a
        # field order that differs by exporter.
        want = numVert * 16
        r = q
        while r + 4 + want <= end:
            (n,) = struct.unpack_from("<I", d, r)
            if n == want:
                pts = [struct.unpack_from("<4f", d, r + 4 + 16 * i)[:3]
                       for i in range(numVert)]
                break
            r += 4
    if pts is not None:
        pts = apply(pts, trans, rot, scale)
    return name, trans, scale, sphere, pts


def partition_block(d, off):
    """NiSkinPartition on this stream: the shared vertex buffer up front."""
    q = off
    q += 4                                    # numPartitions
    (dataSize,) = struct.unpack_from("<I", d, q); q += 4
    (vertexSize,) = struct.unpack_from("<I", d, q); q += 4
    (desc,) = struct.unpack_from("<Q", d, q); q += 8
    if not dataSize or not vertexSize or not has_positions(desc):
        return None
    count = dataSize // vertexSize
    return read_positions(d, q, count, vertexSize, pos_format(desc, vertexSize))


def parse(path, quiet=False):
    d = open(path, "rb").read()
    h = header(d)
    if not quiet:
        print(path)
    off = h["start"]
    fmn = [None] * 3
    fmx = [None] * 3
    pending = []
    for i in range(h["numBlocks"]):
        tname = h["types"][h["idx"][i]]
        pts = None
        label = tname
        if tname in SHAPES:
            name, trans, scale, sphere, pts = shape_block(
                d, off, h["strings"], off + h["sizes"][i])
            label = "%s %s" % (tname, name[:20])
            if pts is None:
                pending.append(label)
            if abs(scale - 1.0) > 1e-4 or any(abs(v) > 1e-4 for v in trans):
                print("  (!) %s transform: trans=(%.3f,%.3f,%.3f) scale=%.4f"
                      % (label, trans[0], trans[1], trans[2], scale))
        elif tname == "NiSkinPartition":
            pts = partition_block(d, off)
            label = "  (skin) %s" % (pending.pop(0) if pending else "?")
        if pts:
            mn, mx = aabb(pts)
            for k in range(3):
                fmn[k] = mn[k] if fmn[k] is None else min(fmn[k], mn[k])
                fmx[k] = mx[k] if fmx[k] is None else max(fmx[k], mx[k])
            if not quiet:
                print("  %-34s verts=%6d  z %9.4f..%9.4f  x %9.4f..%9.4f"
                      "  y %9.4f..%9.4f"
                      % (label[:34], len(pts), mn[2], mx[2], mn[0], mx[0],
                         mn[1], mx[1]))
        off += h["sizes"][i]
    if fmn[0] is not None:
        print("  FILE  z %9.4f..%9.4f  height %8.4f   x %9.4f..%9.4f  %s"
              % (fmn[2], fmx[2], fmx[2] - fmn[2], fmn[0], fmx[0],
                 path.rsplit("/", 1)[-1]))
    return fmn, fmx


if __name__ == "__main__":
    for a in sys.argv[1:]:
        try:
            parse(a)
        except Exception as e:
            print(a, "parse failed:", e)
