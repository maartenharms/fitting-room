"""Every block in a NIF: type, name, and its own local translation.

The companion to nif_bounds.py. That one measures vertices; this one shows the
node graph those vertices hang under, which is what decides where a standalone
loader draws them.

⚠⚠ THIS IS THE TOOL THAT SETTLED THE ARGONIAN HEAD (2026-08-20). Vanilla's
MaleHeadArgonian.nif gives its shape NO transform and skins it to NPC Spine2,
while its three beast siblings put trans z=120.344 on the shape and skin to NPC
Head. All four carry an 'NPC Head [Head]' node at that same height, so the node
graph alone does not give it away: the SHAPE's own row is the one to read.

A live actor never shows this, because the engine skins the mesh to the real
skeleton and that supplies the placement. It only bites something that loads the
file on its own, which is what a preview card does.

Usage: python nif_nodes.py <nif> [<nif> ...]
"""
import struct, sys, os
sys.path.insert(0, r"C:\Studios\Mod Studio\Fitting Room\tools")
from nif_bounds import header

def dump(path):
    d = open(path, 'rb').read()
    h = header(d)
    starts, p = [], h['start']
    for sz in h['sizes']:
        starts.append(p); p += sz
    print('--- %s  (%d blocks) ---' % (os.path.basename(path), h['numBlocks']))
    for i in range(h['numBlocks']):
        t = h['types'][h['idx'][i]]
        b = starts[i]
        name = ''
        tr = None
        try:
            nameIdx = struct.unpack_from('<i', d, b)[0]
            if 0 <= nameIdx < len(h['strings']):
                name = h['strings'][nameIdx]
            nEx = struct.unpack_from('<I', d, b + 4)[0]
            q = b + 8 + 4 * nEx + 4          # past extraData refs + controller
            q += 4                            # flags
            tr = struct.unpack_from('<3f', d, q)
        except Exception:
            pass
        if t.startswith('Ni') or t.startswith('BS'):
            s = '  [%2d] %-24s %-28s' % (i, t, name[:28])
            if tr and any(abs(v) > 0.0005 for v in tr):
                s += ' trans=(%.3f, %.3f, %.3f)' % tr
            print(s)

for a in sys.argv[1:]:
    dump(a)
