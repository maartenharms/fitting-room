"""What `boneWorld * skinToBone` actually is, per bone, for every skinned shape.

The engine draws a skinned vertex at `sum_i w_i * boneWorld_i * skinToBone_i * v`.
A loader that draws the raw vertex buffer is asserting that product is the
identity. This prints the product so the assertion is a measurement rather than
a belief: an author who built in body space gets translation ~(0,0,0), and one
who built somewhere else gets the offset that puts the mesh back.

⚠⚠ BONE WORLD TRANSFORMS ARE ACCUMULATED THROUGH THE NODE TREE, NEVER READ AS
LOCALS. Armour files put most skeleton bones directly under the root, where the
two happen to agree, but an SMP chain (`s3 0` -> `s3 1` -> ...) is nested and
each link carries its own translation. Reading locals there reported a spurious
z spread of -76..0 on a shape whose vertices measure correct, which would have
looked like a second fault.

Usage: python nif_bindpose.py <nif> [<nif> ...]
"""
import struct
import sys
import os

sys.path.insert(0, r"C:\Studios\Mod Studio\Fitting Room\tools")
from nif_bounds import header

SKININST = ("NiSkinInstance", "BSDismemberSkinInstance", "BSSkinInstance")
NODES = ("NiNode", "BSFadeNode", "BSLeafAnimNode", "BSTreeNode", "BSOrderedNode",
         "BSValueNode", "BSMultiBoundNode", "NiBillboardNode", "NiSwitchNode")
IDENT = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)


def mat_mul(a, b):
    return tuple(sum(a[r * 3 + k] * b[k * 3 + c] for k in range(3))
                 for r in range(3) for c in range(3))


def mat_vec(m, v):
    return tuple(sum(m[r * 3 + k] * v[k] for k in range(3)) for r in range(3))


def compose(a, b):
    """NiTransform a * b, each (rot9, trans3, scale)."""
    ar, at, asc = a
    br, bt, bsc = b
    t = mat_vec(ar, tuple(asc * x for x in bt))
    return (mat_mul(ar, br),
            (t[0] + at[0], t[1] + at[1], t[2] + at[2]),
            asc * bsc)


def read_avobject(d, h, b):
    """(name, (rot,trans,scale), byte offset just past the collision ref)."""
    nameIdx, = struct.unpack_from("<i", d, b)
    nm = h["strings"][nameIdx] if 0 <= nameIdx < len(h["strings"]) else ""
    nEx, = struct.unpack_from("<I", d, b + 4)
    q = b + 8 + 4 * nEx + 4 + 4            # extra refs, controller, flags
    tr = struct.unpack_from("<3f", d, q); q += 12
    rot = struct.unpack_from("<9f", d, q); q += 36
    sc, = struct.unpack_from("<f", d, q); q += 4
    q += 4                                  # collision object ref
    return nm, (rot, tr, sc), q


def dump(path):
    d = open(path, "rb").read()
    h = header(d)
    starts, p = [], h["start"]
    for sz in h["sizes"]:
        starts.append(p)
        p += sz
    types = [h["types"][h["idx"][i]] for i in range(h["numBlocks"])]

    local = {}
    children = {}
    for i, t in enumerate(types):
        if t not in NODES:
            continue
        try:
            nm, xf, q = read_avobject(d, h, starts[i])
            local[i] = (nm, xf)
            n, = struct.unpack_from("<I", d, q)
            children[i] = [c for c in struct.unpack_from("<%di" % n, d, q + 4)
                           if 0 <= c < h["numBlocks"]]
        except Exception:
            pass

    world = {}

    def descend(idx, parent):
        if idx not in local or idx in world:
            return
        nm, xf = local[idx]
        w = compose(parent, xf)
        world[idx] = (nm, w)
        for c in children.get(idx, []):
            descend(c, w)

    referenced = set()
    for kids in children.values():
        referenced.update(kids)
    for i in local:
        if i not in referenced:
            descend(i, (IDENT, (0.0, 0.0, 0.0), 1.0))
    for i in local:                       # anything left (cycles, odd roots)
        descend(i, (IDENT, (0.0, 0.0, 0.0), 1.0))

    print("--- %s" % os.path.basename(path))
    for i, t in enumerate(types):
        if t not in SKININST:
            continue
        b = starts[i]
        dataRef, partRef, rootRef, nbones = struct.unpack_from("<iiiI", d, b)
        boneRefs = struct.unpack_from("<%di" % nbones, d, b + 16)
        if not (0 <= dataRef < h["numBlocks"]) or types[dataRef] != "NiSkinData":
            print("  [%d] %s -> no NiSkinData" % (i, t))
            continue
        db = starts[dataRef]
        dbones, = struct.unpack_from("<I", d, db + 52)
        hasw = d[db + 56]
        q = db + 57
        print("  [%2d] %s  bones=%d" % (i, t, nbones))
        offsets, missing = [], 0
        for bone in range(dbones):
            vals = struct.unpack_from("<13f", d, q)
            s2b = (vals[0:9], vals[9:12], vals[12])
            q += 52 + 16
            nverts, = struct.unpack_from("<H", d, q)
            q += 2
            if hasw:
                q += nverts * 6
            ref = boneRefs[bone] if bone < len(boneRefs) else -1
            if ref not in world:
                missing += 1
                continue
            nm, w = world[ref]
            _, trans, _ = compose(w, s2b)
            offsets.append((nm, trans))
            if len(offsets) <= 3:
                print("       %-28s boneWorld*skinToBone trans=(%8.3f,%8.3f,%8.3f)"
                      % (nm[:28], trans[0], trans[1], trans[2]))
        if offsets:
            xs = [o[1][0] for o in offsets]
            ys = [o[1][1] for o in offsets]
            zs = [o[1][2] for o in offsets]
            spread = max(max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs))
            print("       ---- %d bones (%d unresolved): x %.3f..%.3f  y %.3f..%.3f"
                  "  z %.3f..%.3f   spread %.4f %s"
                  % (len(offsets), missing, min(xs), max(xs), min(ys), max(ys),
                     min(zs), max(zs), spread,
                     "UNIFORM" if spread < 0.01 else "PER-BONE"))
            worst = max(offsets, key=lambda o: abs(o[1][2]))
            if spread >= 0.01:
                print("       worst bone: %-28s z=%.3f" % (worst[0][:28], worst[1][2]))
        print()


if __name__ == "__main__":
    for a in sys.argv[1:]:
        try:
            dump(a)
        except Exception as e:
            print(a, "failed:", e)
