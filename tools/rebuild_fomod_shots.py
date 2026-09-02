"""Rebuild every FOMOD installer picture from the HDR sources, tone-mapped.

Replaces the first pass, which resized the 16-bit PQ PNGs with a plain SDR
bitmap copy and produced the washed-out set. Same destination names, so nothing
in either ModuleConfig.xml has to change.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # hdr_to_sdr.py sits beside this file
from hdr_to_sdr import convert

SRC = r"C:/Games/Nolvus/Instances/Nolvus Awakening/SHOTS"
FR = r"C:/Studios/Mod Studio/Fitting Room/fomod/images"
MS = r"C:/Studios/Mod Studio/Menu Studio/fomod/images"

PAPER, KNEE = 203.0, 0.75

JOBS = [
    # Fitting Room
    ("freeform.png",                  FR, "free-form.png"),
    ("lore friendly.png",             FR, "lore-friendly.png"),
    ("earn dyes.png",                 FR, "earn-dyes.png"),
    ("all dyes unlocked.png",         FR, "all-dyes.png"),
    ("PAGE - Worth Knowing.png",      FR, "worth-knowing.png"),
    # Menu Studio, scene step
    ("a clean void.png",              MS, "scene-void.png"),
    ("the dressing room.png",         MS, "scene-dressing-room.png"),
    ("just you and the room.png",     MS, "scene-just-you.png"),
    ("leave the world where it is.png", MS, "scene-world.png"),
    # Menu Studio, background step
    ("no dome, just colour.png",      MS, "dome-blank.png"),
    ("the star dome.png",             MS, "dome-stars.png"),
    ("the vampire dome.png",          MS, "dome-vampire.png"),
    ("the aurora dome.png",           MS, "dome-aurora.png"),
    ("my own picture.png",            MS, "dome-custom.png"),
    # Menu Studio, closing step
    ("PAGE - what menu studio needs and what goes well with it.png", MS, "before-you-play.png"),
]

fail = 0
total_before = total_after = 0
for src_name, dst_dir, dst_name in JOBS:
    src = os.path.join(SRC, src_name)
    dst = os.path.join(dst_dir, dst_name)
    if not os.path.isfile(src):
        print(f"MISSING SOURCE: {src}")
        fail = 1
        continue
    before = os.path.getsize(dst) if os.path.isfile(dst) else 0
    convert(src, dst, PAPER, KNEE, 1280, 720, 1.0)
    after = os.path.getsize(dst)
    total_before += before
    total_after += after
    print(f"  {dst_name:26} {before//1024:5} KB -> {after//1024:5} KB")

print(f"\ntotal {total_before//1024} KB -> {total_after//1024} KB "
      f"({(total_after-total_before)//1024:+d} KB)")
sys.exit(fail)
