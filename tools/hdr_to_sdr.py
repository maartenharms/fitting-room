"""HDR10 PNG -> SDR sRGB PNG, tone-mapped rather than reinterpreted.

The shots are 16-bit, BT.2020, PQ, full range, peaking at 466 nits. The earlier
pass resized them with a plain SDR bitmap copy, which read the PQ code values as
if they were sRGB: that is the washed-out look, and it is a decode error rather
than anything wrong with the capture.

Measured on the sources before choosing an anchor: in a UI-heavy frame the
densest luminance band is 136-185 nits at 21% of pixels, which is the panel
drawn at paper white. So paper white is ~160 nits here, not the 203 the standard
assumes, and 96% of every frame sits below it. That makes these images
SDR-with-highlights, so the curve leaves the bulk alone and only rolls off what
is above the knee.

usage: hdr_to_sdr.py <in.png> <out.png> [--paper 160] [--knee 0.75]
                     [--width 1280] [--height 720] [--sat 1.0]
"""
import argparse, subprocess, sys
import numpy as np
from PIL import Image

# --- PQ (SMPTE ST 2084) ---
M1, M2 = 2610 / 16384, 2523 / 4096 * 128
C1, C2, C3 = 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32

# BT.2020 -> BT.709, linear light
BT2020_TO_709 = np.array([
    [ 1.66049100, -0.58764114, -0.07284986],
    [-0.12455047,  1.13289990, -0.00834942],
    [-0.01815076, -0.10057889,  1.11872966],
], dtype=np.float32)

LUMA_709 = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def decode_pq(path, w, h):
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
         "-pix_fmt", "rgb48le", "-"],
        capture_output=True, check=True).stdout
    sig = np.frombuffer(raw, dtype="<u2").reshape(h, w, 3).astype(np.float32) / 65535.0
    e = np.power(np.clip(sig, 0, 1), 1.0 / M2)
    return 10000.0 * np.power(np.clip(e - C1, 0, None) / (C2 - C3 * e), 1.0 / M1)


def shoulder(L, knee, white):
    """Identity below the knee, smooth compression from knee..white into knee..1.

    A plain Reinhard would pull paper white down to about 0.56 and hand back the
    same flat picture the naive resize did. Everything below the knee is already
    where it belongs, so it is left exactly alone.
    """
    out = L.copy()
    hi = L > knee
    span = white - knee
    if span <= 0:
        return np.clip(out, 0, 1)
    x = (L[hi] - knee) / span
    out[hi] = knee + (1.0 - knee) * (1.0 - np.exp(-3.0 * x)) / (1.0 - np.exp(-3.0))
    return np.clip(out, 0, 1)


def srgb_oetf(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def convert(src, dst, paper, knee, width, height, sat, in_w=2560, in_h=1440):
    nits = decode_pq(src, in_w, in_h)

    lin709 = nits @ BT2020_TO_709.T          # still absolute nits, 709 primaries
    lin = np.clip(lin709 / paper, 0, None)   # paper white == 1.0

    white = float(nits.max()) / paper
    L = np.clip(lin @ LUMA_709, 1e-6, None)
    scale = shoulder(L, knee, white) / L     # hue-preserving: scale, don't curve per channel
    lin *= scale[..., None]

    if sat != 1.0:
        g = (lin * LUMA_709).sum(-1, keepdims=True)
        lin = np.clip(g + (lin - g) * sat, 0, None)

    rgb = (srgb_oetf(lin) * 255.0 + 0.5).astype(np.uint8)
    img = Image.fromarray(rgb, "RGB")
    if width and height:
        img = img.resize((width, height), Image.LANCZOS)
    img.save(dst, "PNG", optimize=True)
    return white


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("src"); ap.add_argument("dst")
    ap.add_argument("--paper", type=float, default=160.0)
    ap.add_argument("--knee", type=float, default=0.75)
    ap.add_argument("--sat", type=float, default=1.0)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    a = ap.parse_args()
    w = convert(a.src, a.dst, a.paper, a.knee, a.width, a.height, a.sat)
    print(f"{a.dst}  paper={a.paper:.0f} knee={a.knee} white={w:.2f}x sat={a.sat}")
