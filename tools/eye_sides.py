"""Where does ONE eye sit inside an eyes mesh? The window centres on it."""
import sys, os
sys.path.insert(0, r'C:\Studios\Mod Studio\Fitting Room\tools')
import nif_bounds as nb
import struct, io


def clusters(path):
    d = open(path, 'rb').read()
    h = nb.header(d)
    off = h['start']
    pts = []
    pending = []
    for i in range(h['numBlocks']):
        t = h['types'][h['idx'][i]]
        got = None
        if t in nb.SHAPES:
            name, trans, scale, sphere, p = nb.shape_block(d, off, h['strings'],
                                                           off + h['sizes'][i])
            got = p
            if p is None:
                pending.append(name)
        elif t == 'NiSkinPartition':
            got = nb.partition_block(d, off)
        if got:
            pts.extend(got)
        off += h['sizes'][i]
    return pts


for path in sys.argv[1:]:
    try:
        pts = clusters(path)
    except Exception as e:
        print(path, 'failed:', e)
        continue
    if not pts:
        print(path, 'no geometry')
        continue
    xs = [p[0] for p in pts]
    zs = [p[2] for p in pts]
    ys = [p[1] for p in pts]
    half = max(abs(min(xs)), abs(max(xs)))
    right = [p for p in pts if p[0] > 0.05 * half]
    if not right:
        print(path, 'no right-side cluster')
        continue
    rx = [p[0] for p in right]
    rz = [p[2] for p in right]
    centre = (min(rx) + max(rx)) * 0.5
    print("%s\n   pair x +-%.3f  z %.3f..%.3f  y %.3f..%.3f" %
          (os.path.basename(path), half, min(zs), max(zs), min(ys), max(ys)))
    print("   one eye: x %.3f..%.3f centre %.3f  =  f %.4f of the pair's half width;"
          "  its own z %.3f..%.3f" %
          (min(rx), max(rx), centre, centre / half, min(rz), max(rz)))
