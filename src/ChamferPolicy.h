#pragma once

#include <cmath>

// The geometry behind a chamfered ("cut corner") panel, kept free of both the
// engine and FLICK so the arithmetic can be tested on its own.
//
// ⚠⚠ THIS FILE IS A COPY. The original is Menu Studio's `src/ChamferPolicy.h`,
// ported here 2026-08-12 so the two mods can look like one design language. The
// two are byte-identical apart from this note and the namespace, and they should
// stay that way: a fix to the geometry in either tree belongs in both. The
// DRAWING layer beside it (ChamferPanel) is deliberately NOT identical, and its
// header says exactly where it diverges and why. There is no shared library
// between these repositories to put this in, and inventing one for three files
// would be a bigger commitment than copying two headers.
//
// WHY THIS EXISTS AT ALL. FLICK's theme file cannot express a cut corner. Its
// whole geometry vocabulary is scalar - fRounding, fFrameRounding, fTabRounding,
// fButtonRounding, fGrabRounding, fScrollbarRounding, fPopupRounding - and every
// one of those feeds an ImGui corner ARC. There is no chamfer mode, no border
// image, no nine-slice. A theme author gets colours and a radius, which is why
// Vel'dun's own FLICK preset is a colour overhaul: that is the ceiling of the
// format, not a shortfall of the author. The notched frames in Vel'dun UI proper
// are Flash vector art inside 332 SWFs and nothing in the INI reaches that layer.
//
// So a cut corner has to be drawn by the plugin. FLICK exposes enough to do it:
// DrawQuadFilled takes four arbitrary points and DrawLine takes two.
//
// ⚠ THERE IS NO CONVEX-POLYGON FILL IN THE FLICK ABI, and that constraint shapes
// everything below. An octagon cannot be one primitive, so it has to be tiled,
// and every interior edge where two filled primitives meet is a place the fill
// can show a seam. Two facts about how ImGui fills decide how bad that is:
//
//   * AddRectFilled with zero rounding emits a hard quad with NO anti-aliased
//     fringe.
//   * AddQuadFilled and AddTriangleFilled go through AddConvexPolyFilled, which
//     feathers roughly a pixel OUTWARD from every edge when anti-aliased fill is
//     on (it is, by default).
//
// Mixing the two is the trap. A feathered quad edge laid against a hard rect
// edge puts partial coverage on top of full coverage, and on a translucent fill
// the overlap blends twice and reads as a darker line. Two feathered edges laid
// against each other each contribute about half coverage and sum back to one,
// which is the case that disappears.
//
// Hence: tile the octagon into THREE QUADS split by two vertical lines, and draw
// all three the same way. Three primitives, two interior seams, both of them
// quad-to-quad. The obvious alternative - a rect plus four corner triangles -
// costs seven primitives, eight seams, and mixes both fill paths. Treat the seam
// reasoning as reasoning; what is certain is the primitive and seam counts.
//
// If a panel ever needs to be genuinely seam-free over a transparent background,
// the escape hatch is not more geometry: load a texture whose alpha carries the
// chamfer and draw it with a single AddImage. That is one quad, one blend, and
// it buys authored corner ornaments as well. This header is the code-only route.

namespace OS::ChamferPolicy {

    struct Point {
        float x = 0.0f;
        float y = 0.0f;
    };

    // Which corners get cut. A mask rather than a bool because the look this is
    // chasing rarely cuts all four: Vel'dun's list headers cut the leading pair
    // and leave the trailing pair square, and a panel that docks against a screen
    // edge wants the docked corners left alone.
    enum Corner : unsigned {
        kNone        = 0u,
        kTopLeft     = 1u << 0,
        kTopRight    = 1u << 1,
        kBottomRight = 1u << 2,
        kBottomLeft  = 1u << 3,
        kAll         = kTopLeft | kTopRight | kBottomRight | kBottomLeft,
        kTop         = kTopLeft | kTopRight,
        kBottom      = kBottomLeft | kBottomRight,
        kLeft        = kTopLeft | kBottomLeft,
        kRight       = kTopRight | kBottomRight,
    };

    // ⚠ A 45 DEGREE CHAMFER DOES NOT KEEP ITS CUT LENGTH WHEN YOU INSET IT, and
    // this constant is the whole reason Inset() exists rather than callers just
    // nudging min and max. Push the top edge and the diagonal edge inward by the
    // same perpendicular distance d and solve for where they now cross: the top
    // edge is y = min.y + d, the diagonal is x + y = min.x + min.y + c + d*sqrt2,
    // so the new corner vertex sits at x = min.x + c + d*sqrt2 - d. Measured from
    // the new left edge at min.x + d that is c - d*(2 - sqrt2).
    //
    // Get this wrong and a multi-pass border does not stay parallel to itself:
    // the diagonals splay or converge relative to the straights, a little more
    // with every pass, and a 3px edge ends up visibly thicker on the corners than
    // on the sides.
    inline constexpr float kInsetCutScale = 0.5857864376269049f;  // 2 - sqrt(2)

    struct Rect {
        Point min;
        Point max;
        float cut = 0.0f;
        unsigned corners = kAll;
    };

    [[nodiscard]] inline float Width(const Rect& a_r) { return a_r.max.x - a_r.min.x; }
    [[nodiscard]] inline float Height(const Rect& a_r) { return a_r.max.y - a_r.min.y; }

    // The cut this rect can actually carry. Half the shorter side is the point
    // where opposing cuts meet in the middle and the straight between them
    // vanishes; past that the outline would self-intersect and the fill would
    // fold back over itself. Clamped rather than rejected, because the caller
    // that hits this is usually a panel mid-animation collapsing to nothing, and
    // a degenerate frame should draw a diamond, not garbage.
    [[nodiscard]] inline float ClampCut(const Rect& a_r) {
        const float w = Width(a_r);
        const float h = Height(a_r);
        const float limit = 0.5f * ((w < h) ? w : h);
        float cut = a_r.cut;
        if (!(cut > 0.0f)) {  // also catches NaN
            return 0.0f;
        }
        if (limit <= 0.0f) {
            return 0.0f;
        }
        return (cut > limit) ? limit : cut;
    }

    [[nodiscard]] inline float CutAt(const Rect& a_r, Corner a_corner) {
        return (a_r.corners & a_corner) ? ClampCut(a_r) : 0.0f;
    }

    // The outline, clockwise from the top-left corner's first vertex. A corner
    // that is not cut contributes one vertex instead of two, so an all-square
    // rect yields four points and a fully cut one yields eight.
    struct Outline {
        Point v[8];
        int   count = 0;
    };

    [[nodiscard]] inline Outline BuildOutline(const Rect& a_r) {
        const float tl = CutAt(a_r, kTopLeft);
        const float tr = CutAt(a_r, kTopRight);
        const float br = CutAt(a_r, kBottomRight);
        const float bl = CutAt(a_r, kBottomLeft);

        Outline out;
        const auto push = [&out](float x, float y) {
            if (out.count < 8) {
                out.v[out.count++] = Point{ x, y };
            }
        };

        // Top edge, left to right.
        if (tl > 0.0f) {
            push(a_r.min.x + tl, a_r.min.y);
        } else {
            push(a_r.min.x, a_r.min.y);
        }
        if (tr > 0.0f) {
            push(a_r.max.x - tr, a_r.min.y);
            push(a_r.max.x, a_r.min.y + tr);
        } else {
            push(a_r.max.x, a_r.min.y);
        }
        // Right edge down, then the bottom right to left.
        if (br > 0.0f) {
            push(a_r.max.x, a_r.max.y - br);
            push(a_r.max.x - br, a_r.max.y);
        } else {
            push(a_r.max.x, a_r.max.y);
        }
        if (bl > 0.0f) {
            push(a_r.min.x + bl, a_r.max.y);
            push(a_r.min.x, a_r.max.y - bl);
        } else {
            push(a_r.min.x, a_r.max.y);
        }
        // Left edge back up. The top-left cut's second vertex closes the loop;
        // an uncut top-left already emitted its single corner above.
        if (tl > 0.0f) {
            push(a_r.min.x, a_r.min.y + tl);
        }
        return out;
    }

    // The fill, tiled into at most three quads by two vertical cuts. See the
    // seam discussion at the top for why it is quads all the way and why the
    // splits are vertical rather than radial.
    //
    // The left piece spans x from min.x to min.x + max(topLeftCut, bottomLeftCut)
    // and carries both left diagonals; the right piece mirrors it; the middle is
    // a plain full-height rectangle. When a side has no cut on either corner its
    // piece is zero-width and is dropped, so an all-square rect comes back as a
    // single quad and costs exactly what a rect would.
    struct Quad {
        Point v[4];
    };

    struct Fill {
        Quad quads[3];
        int  count = 0;
    };

    [[nodiscard]] inline Fill BuildFill(const Rect& a_r) {
        const float tl = CutAt(a_r, kTopLeft);
        const float tr = CutAt(a_r, kTopRight);
        const float br = CutAt(a_r, kBottomRight);
        const float bl = CutAt(a_r, kBottomLeft);

        const float leftBand  = (tl > bl) ? tl : bl;
        const float rightBand = (tr > br) ? tr : br;

        const float xL = a_r.min.x + leftBand;
        const float xR = a_r.max.x - rightBand;

        Fill fill;
        if (leftBand > 0.0f) {
            fill.quads[fill.count++] = Quad{ {
                Point{ a_r.min.x, a_r.min.y + tl },
                Point{ xL, a_r.min.y },
                Point{ xL, a_r.max.y },
                Point{ a_r.min.x, a_r.max.y - bl },
            } };
        }
        // The middle survives even when the two bands meet, because meeting is
        // exactly the clamped-diamond case and a zero-width middle draws nothing.
        if (xR >= xL) {
            fill.quads[fill.count++] = Quad{ {
                Point{ xL, a_r.min.y },
                Point{ xR, a_r.min.y },
                Point{ xR, a_r.max.y },
                Point{ xL, a_r.max.y },
            } };
        }
        if (rightBand > 0.0f) {
            fill.quads[fill.count++] = Quad{ {
                Point{ xR, a_r.min.y },
                Point{ a_r.max.x, a_r.min.y + tr },
                Point{ a_r.max.x, a_r.max.y - br },
                Point{ xR, a_r.max.y },
            } };
        }
        return fill;
    }

    // Shrink the whole shape by a_d measured perpendicular to every edge,
    // diagonals included. This is what a border pass walks, and it is the reason
    // a stroked chamfer stays parallel to itself instead of splaying.
    [[nodiscard]] inline Rect Inset(const Rect& a_r, float a_d) {
        Rect r = a_r;
        r.min.x += a_d;
        r.min.y += a_d;
        r.max.x -= a_d;
        r.max.y -= a_d;
        r.cut = ClampCut(a_r) - a_d * kInsetCutScale;
        if (!(r.cut > 0.0f)) {
            r.cut = 0.0f;
        }
        return r;
    }

    // Signed area, positive for the clockwise-in-screen-space winding above
    // (y grows downward, so the shoelace sign flips relative to maths class).
    // Only used by the tests, but it lives here so the definition of "the shape"
    // and the definition of "its area" cannot drift apart.
    [[nodiscard]] inline float OutlineArea(const Outline& a_o) {
        if (a_o.count < 3) {
            return 0.0f;
        }
        float twice = 0.0f;
        for (int i = 0; i < a_o.count; ++i) {
            const Point& p = a_o.v[i];
            const Point& q = a_o.v[(i + 1) % a_o.count];
            twice += (p.x * q.y) - (q.x * p.y);
        }
        return 0.5f * std::fabs(twice);
    }

    [[nodiscard]] inline float QuadArea(const Quad& a_q) {
        float twice = 0.0f;
        for (int i = 0; i < 4; ++i) {
            const Point& p = a_q.v[i];
            const Point& q = a_q.v[(i + 1) % 4];
            twice += (p.x * q.y) - (q.x * p.y);
        }
        return 0.5f * std::fabs(twice);
    }

    [[nodiscard]] inline float FillArea(const Fill& a_f) {
        float total = 0.0f;
        for (int i = 0; i < a_f.count; ++i) {
            total += QuadArea(a_f.quads[i]);
        }
        return total;
    }

}  // namespace OS::ChamferPolicy
