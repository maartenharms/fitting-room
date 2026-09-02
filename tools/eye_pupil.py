"""Where is the pupil? The front pole of an eyeball, measured, not boxed."""
import sys, os
sys.path.insert(0, r'C:\Studios\Mod Studio\Fitting Room\tools')
import nif_bounds as nb


def points(path):
    d = open(path, 'rb').read()
    h = nb.header(d)
    off, pts = h['start'], []
    for i in range(h['numBlocks']):
        t = h['types'][h['idx'][i]]
        got = None
        if t in nb.SHAPES:
            got = nb.shape_block(d, off, h['strings'], off + h['sizes'][i])[4]
        elif t == 'NiSkinPartition':
            got = nb.partition_block(d, off)
        if got:
            pts.extend(got)
        off += h['sizes'][i]
    return pts


for path in sys.argv[1:]:
    pts = points(path)
    half = max(abs(min(p[0] for p in pts)), abs(max(p[0] for p in pts)))
    one = [p for p in pts if p[0] > 0.05 * half]          # one eye
    ymax = max(p[1] for p in one)
    ymin = min(p[1] for p in one)
    depth = ymax - ymin
    zlo, zhi = min(p[2] for p in one), max(p[2] for p in one)
    print("%s" % os.path.basename(path))
    print("   one eye box: z %.3f..%.3f (h %.3f), y %.3f..%.3f, x %.3f..%.3f"
          % (zlo, zhi, zhi - zlo, ymin, ymax, min(p[0] for p in one), max(p[0] for p in one)))
    for cut in (0.02, 0.05, 0.10):
        front = [p for p in one if p[1] >= ymax - depth * cut]
        fz = [p[2] for p in front]
        fx = [p[0] for p in front]
        print("     front %2.0f%% of depth (%4d verts): pupil z %.3f  (z %.3f..%.3f), x %.3f"
              % (cut * 100, len(front), (min(fz) + max(fz)) / 2, min(fz), max(fz),
                 (min(fx) + max(fx)) / 2))
    print("     for reference: box top %.3f, box centre %.3f, box bottom %.3f"
          % (zhi, (zlo + zhi) / 2, zlo))
