"""Each shape's AUTHORED bounding sphere, beside its own transform.

⚠⚠ THE AUTHORED SPHERE IS NOT THE VERTEX BOX AND THE DIFFERENCE MATTERS.
nif_bounds.py walks vertex buffers; this reads the modelBound the engine itself
consults, which is what MeshExtractor's head-space test measures. A skinned NIF
routinely leaves that sphere at zero, and an empty bound is not a measurement:
zero cannot tell "unmeasured" apart from "at the origin". Read this before
assuming a bound-based rule will fire on a given file.

Used 2026-08-20 to confirm the three vanilla beast heads carry real spheres
(radius 11.7 to 15.7), so the head-space test would measure them rather than
skip them, BEFORE the fix was built rather than after it failed in the field.

Usage: python nif_shape_bounds.py <nif> [<nif> ...]
"""
import struct, sys, os
sys.path.insert(0, r"C:\Studios\Mod Studio\Fitting Room\tools")
from nif_bounds import header
SHAPES = ("BSTriShape","BSSubIndexTriShape","BSDynamicTriShape","BSMeshLODTriShape")
for path in sys.argv[1:]:
    d = open(path,'rb').read(); h = header(d)
    starts, p = [], h['start']
    for sz in h['sizes']:
        starts.append(p); p += sz
    print('--- %s' % os.path.basename(path))
    for i in range(h['numBlocks']):
        t = h['types'][h['idx'][i]]
        if t not in SHAPES: continue
        b = starts[i]
        nameIdx = struct.unpack_from('<i', d, b)[0]
        nm = h['strings'][nameIdx] if 0 <= nameIdx < len(h['strings']) else ''
        nEx = struct.unpack_from('<I', d, b+4)[0]
        q = b + 8 + 4*nEx + 4      # controller
        q += 4                      # flags
        tr = struct.unpack_from('<3f', d, q); q += 12
        q += 36                     # rotation
        sc = struct.unpack_from('<f', d, q)[0]; q += 4
        q += 4                      # collision ref
        cx,cy,cz,r = struct.unpack_from('<4f', d, q)
        print('  %-22s trans=(%.3f,%.3f,%.3f) scale=%.2f  modelBound center=(%.3f,%.3f,%.3f) radius=%.4f'
              % (nm[:22], tr[0],tr[1],tr[2], sc, cx,cy,cz, r))
