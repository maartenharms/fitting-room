#!/usr/bin/env python3
"""Render the nine-slice frame the editor draws its chrome with.

WHY A TEXTURE AT ALL. ChamferPolicy.h names this as the escape hatch and it is
the honest one: FLICK's theme format cannot describe a cut corner, and the
quad-drawn chamfer beside it can only ever produce an octagon. Vel'dun's own
frames are Flash vector art, so the way to look like them is to draw ART rather
than to draw geometry. Vel'dun UI FLICK ships no images to borrow (two INIs, two
JSONs and a backup), and lifting another author's art out of their SWFs is a
licensing question rather than a technical one, so this generates our own.

WHY IT IS WHITE. The alpha carries the SHAPE and nothing else; the colour comes
from the tint at draw time, which is read from the live FLICK theme. A frame
with its own baked colour would be the one thing in the editor that ignores the
user's preset, which is the complaint that starts every theming thread.

WHY IT IS REGENERABLE RATHER THAN HAND-PAINTED. Every number below is a knob,
and the ones worth turning are at the top. A frame that came out of a paint
program would be a binary nobody could adjust without owning the same tool and
remembering what they did.

Pure standard library on purpose: this box has no imaging package and a build
tool that needs one installed is a build tool that stops working on the next
machine.

    python tools/make_frame_texture.py
"""

import binascii
import struct
import zlib
from pathlib import Path

SIZE = 64          # texture is square; the nine-slice corner is SLICE of it
SLICE = 20         # corner region, in texels. Must exceed CUT or the corner
                   # motif would run into the stretched edge and smear.
CUT = 14.0         # how far the corner treatment reaches along each side
SS = 4             # supersampling per axis. The corners live or die on this.

# The line profile, as (start, end, alpha) bands measured in texels INWARD from
# the shape's boundary. Two lines rather than one: a solid outer edge and a
# lighter inner rule, which is what stops a frame reading as a plain box.
BANDS = [(0.0, 2.0, 1.00),
         (3.0, 4.0, 0.55)]

# WHY THE CORNER IS A PARAMETER AND THE REST IS NOT. Once the frame is art, the
# corner is the only part of it the nine-slice never stretches, so it is the one
# place a shape can be spent with no cost and no distortion at any panel size.
# A curve here is exact; the same curve drawn procedurally would need arc
# primitives, which are version 3 in the FLICK ABI and awkward to stroke.
#
#   chamfer  the 45 degree cut the quad-drawn version makes, for parity
#   round    a convex quarter circle, the ordinary softened corner
#   scoop    a quarter circle BITTEN OUT of the corner, concave, which is the
#            Vel'dun move: the frame turns inward instead of cutting across
CORNERS = ("chamfer", "round", "scoop")

# The scoop's radius, which is NOT CUT and should not be folded into it. CUT is
# how far a straight cut travels along each side; this is the radius of a disc
# removed at the corner, and the two are only loosely comparable. Picked from a
# four-depth sheet in the field (user 2026-08-12, radius 4, 6, 9 and 12 shown,
# 12 chosen). Keep it under SLICE or the arc runs into the stretched edge.
SCOOP_R = 12.0


def edge_distance(x, y, corner):
    """Distance from (x, y) to the nearest point on the frame's boundary.

    Every mode is a minimum over pieces, because the shape is an intersection:
    a point inside is as far from the boundary as its NEAREST piece. The four
    straight sides are shared by all three; only the corner term differs.
    """
    w = h = float(SIZE)
    d = min(x, y, w - x, h - y)

    if corner == "chamfer":
        # Four half planes, one per diagonal. Divided by root two because
        # x + y = c is not in unit-normal form.
        root2 = 2.0 ** 0.5
        return min(d,
                   (x + y - CUT) / root2,
                   ((w - x) + y - CUT) / root2,
                   (x + (h - y) - CUT) / root2,
                   ((w - x) + (h - y) - CUT) / root2)

    if corner == "round":
        # The rounded rect: inside a corner's quadrant the boundary is a circle
        # of radius CUT centred CUT in from both sides, so the distance is how
        # far the point is from that circle's rim. Outside the quadrants the
        # straights above already answer it.
        for cx, cy in ((CUT, CUT), (w - CUT, CUT), (CUT, h - CUT), (w - CUT, h - CUT)):
            if (x < CUT or x > w - CUT) and (y < CUT or y > h - CUT):
                if (x - cx) * (x - cx) + (y - cy) * (y - cy) >= 0.0:
                    r = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
                    if (x - cx) * (cx - w * 0.5) >= 0 and (y - cy) * (cy - h * 0.5) >= 0:
                        d = min(d, CUT - r)
        return d

    # scoop: the shape is the square with a disc of radius SCOOP_R removed at
    # each corner, so the boundary there is an arc centred ON the corner and the
    # distance to it is how far outside that disc the point sits. Concave, which
    # is the whole difference from "round": the frame turns INWARD at the corner
    # instead of cutting across it or bulging out.
    for cx, cy in ((0.0, 0.0), (w, 0.0), (0.0, h), (w, h)):
        r = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
        d = min(d, r - SCOOP_R)
    return d


def sample(x, y, corner, solid=False):
    """Alpha at one sample point.

    Two textures come out of the same shape. The FRAME samples the line bands,
    which is chrome. The FILL samples the whole interior, which is what lets a
    filled panel be cut to the same curve: quads can draw an octagon and never
    an arc, so anything that has to meet the frame's corner either keeps clear
    of it or is drawn through this. The rail's background wash was the first
    thing to need it and buttons are the next.
    """
    d = edge_distance(x, y, corner)
    if d < 0.0:
        return 0.0
    if solid:
        # Feather the outermost half texel so the fill's arc is anti-aliased
        # the same way the frame's is. Without it the two shapes agree in
        # geometry and disagree on screen, which is a visible fringe.
        return min(1.0, d / 0.5) if d < 0.5 else 1.0
    for lo, hi, value in BANDS:
        if lo <= d < hi:
            return value
    return 0.0


def render(corner="chamfer", solid=False):
    """One RGBA row per texel line, supersampled SS x SS for clean curves."""
    rows = []
    step = 1.0 / SS
    weight = 1.0 / (SS * SS)
    for py in range(SIZE):
        row = bytearray()
        for px in range(SIZE):
            total = 0.0
            for sy in range(SS):
                y = py + (sy + 0.5) * step
                for sx in range(SS):
                    total += sample(px + (sx + 0.5) * step, y, corner, solid)
            a = int(round(min(1.0, total * weight) * 255.0))
            # White, because the tint at draw time supplies the colour. The RGB
            # stays 255 even where alpha is 0 so a bilinear sample at the edge
            # of a band fades toward transparent white rather than toward black,
            # which is where a dark halo around a light frame comes from.
            row += bytes((255, 255, 255, a))
        rows.append(bytes(row))
    return rows


def chunk(tag, payload):
    body = tag + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(
        ">I", binascii.crc32(body) & 0xFFFFFFFF)


def write_png(path, rows):
    raw = b"".join(b"\x00" + r for r in rows)  # filter byte 0 per row
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)
    return len(png)


def main():
    import sys
    corner = sys.argv[1] if len(sys.argv) > 1 else "chamfer"
    if corner not in CORNERS:
        raise SystemExit(f"corner must be one of {CORNERS}, got {corner!r}")
    icons = (Path(__file__).resolve().parent.parent
             / "dist" / "SKSE" / "Plugins" / "FittingRoom" / "icons")
    for name, solid in (("frame.png", False), ("frame_fill.png", True)):
        size = write_png(icons / name, render(corner, solid))
        print(f"wrote {icons / name} ({size} bytes, {SIZE}x{SIZE}, "
              f"slice {SLICE}, corner {corner})")


if __name__ == "__main__":
    main()
