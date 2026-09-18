"""Hand copy of the Fitting Room DLL and PDB from build\\release into the MO2
slot the Lumos 20 sync profile runs, with the nacre pair backed up first and
both ends hashed. Python, not PowerShell: the slot name carries a wildcard."""
import hashlib
import os
import shutil
import sys

SRC = r"C:\Studios\Mod Studio\Fitting Room\.claude\worktrees\commonlib-ng\build\release"
DST = r"C:\Games\Nolvus\Instances\Nolvus Awakening\MODS\mods\Fitting Room [outfit-dye]\SKSE\Plugins"
STAMP = "20260916-pre-teardown"


def md5(p):
    return hashlib.md5(open(p, "rb").read()).hexdigest().upper()


ok = True
for name in ("FittingRoom.dll", "FittingRoom.pdb"):
    dst = os.path.join(DST, name)
    bak = dst + ".bak-" + STAMP
    if os.path.exists(dst) and not os.path.exists(bak):
        shutil.copy2(dst, bak)
        print(f"backed up {name} {md5(bak)[:8]} -> {os.path.basename(bak)}")
    src = os.path.join(SRC, name)
    shutil.copy2(src, dst)
    a, b = md5(src), md5(dst)
    print(f"{name}: build {a} ({os.path.getsize(src)} B)  slot {b} ({os.path.getsize(dst)} B)  {'MATCH' if a == b else 'MISMATCH'}")
    ok = ok and a == b
print("slot listing:")
for f in sorted(os.listdir(DST)):
    if f.startswith("FittingRoom."):
        p = os.path.join(DST, f)
        print(f"  {md5(p)[:8]} {os.path.getsize(p):>10} {f}")
sys.exit(0 if ok else 1)
