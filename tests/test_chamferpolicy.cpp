#include "ChamferPolicy.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

namespace P = OS::ChamferPolicy;

// Areas here run to tens of thousands of square pixels in float, so a fixed
// epsilon would be either uselessly loose on the big cases or a false alarm on
// the small ones.
static bool Near(float a_lhs, float a_rhs, float a_rel = 1e-4f) {
    const float scale = std::fmax(1.0f, std::fmax(std::fabs(a_lhs), std::fabs(a_rhs)));
    return std::fabs(a_lhs - a_rhs) <= a_rel * scale;
}

static P::Rect MakeRect(float a_w, float a_h, float a_cut, unsigned a_corners = P::kAll) {
    return P::Rect{ P::Point{ 100.0f, 50.0f },
                    P::Point{ 100.0f + a_w, 50.0f + a_h },
                    a_cut,
                    a_corners };
}

// Every turn of a convex polygon bends the same way. This is the property that
// breaks first if a vertex is emitted out of order, and a self-intersecting
// outline would still have a plausible-looking area.
static bool IsConvex(const P::Outline& a_o) {
    if (a_o.count < 3) {
        return false;
    }
    int sign = 0;
    for (int i = 0; i < a_o.count; ++i) {
        const P::Point& a = a_o.v[i];
        const P::Point& b = a_o.v[(i + 1) % a_o.count];
        const P::Point& c = a_o.v[(i + 2) % a_o.count];
        const float cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        if (std::fabs(cross) < 1e-3f) {
            continue;  // collinear, or a zero-length straight in the diamond case
        }
        const int s = (cross > 0.0f) ? 1 : -1;
        if (sign == 0) {
            sign = s;
        } else if (s != sign) {
            return false;
        }
    }
    return true;
}

int main() {
    // Vertex counts: one vertex per square corner, two per cut one.
    {
        CHECK(P::BuildOutline(MakeRect(300.0f, 200.0f, 12.0f, P::kAll)).count == 8);
        CHECK(P::BuildOutline(MakeRect(300.0f, 200.0f, 12.0f, P::kNone)).count == 4);
        CHECK(P::BuildOutline(MakeRect(300.0f, 200.0f, 12.0f, P::kTop)).count == 6);
        CHECK(P::BuildOutline(MakeRect(300.0f, 200.0f, 12.0f, P::kLeft)).count == 6);
        CHECK(P::BuildOutline(MakeRect(300.0f, 200.0f, 12.0f, P::kTopLeft)).count == 5);
        // A zero cut is the same shape as no mask at all.
        CHECK(P::BuildOutline(MakeRect(300.0f, 200.0f, 0.0f, P::kAll)).count == 4);
    }

    // The outline is convex for every corner mask.
    {
        for (unsigned mask = 0; mask <= P::kAll; ++mask) {
            CHECK(IsConvex(P::BuildOutline(MakeRect(300.0f, 200.0f, 18.0f, mask))));
        }
    }

    // Area against the closed form. Each cut corner removes a right triangle
    // with legs of the cut length, so the octagon is w*h minus cut^2/2 per cut
    // corner. Checking the shoelace against arithmetic done a completely
    // different way is the point.
    {
        const float w = 300.0f, h = 200.0f, c = 16.0f;
        struct Case {
            unsigned mask;
            int      cuts;
        };
        const Case cases[] = { { P::kNone, 0 },  { P::kTopLeft, 1 },
                               { P::kTop, 2 },   { P::kLeft, 2 },
                               { P::kRight, 2 }, { P::kAll & ~P::kTopLeft, 3 },
                               { P::kAll, 4 } };
        for (const Case& k : cases) {
            const auto  outline  = P::BuildOutline(MakeRect(w, h, c, k.mask));
            const float expected = (w * h) - (0.5f * c * c * static_cast<float>(k.cuts));
            CHECK(Near(P::OutlineArea(outline), expected));
        }
    }

    // ⚠ THE FILL MUST TILE THE OUTLINE EXACTLY. This is the test that earns the
    // file. The three-quad split is hand-derived, and the two ways it can be
    // wrong (leaving a sliver of the shape unpainted, or overlapping two quads)
    // both look like a correct panel on an opaque fill and only show up as a
    // seam or a double-blended band on a translucent one. Comparing total fill
    // area against outline area catches both: a gap makes the sum too small, an
    // overlap makes it too large.
    {
        const float sizes[][2] = { { 300.0f, 200.0f }, { 64.0f, 64.0f }, { 500.0f, 31.0f } };
        const float cuts[]     = { 0.0f, 1.0f, 7.5f, 16.0f, 40.0f };
        for (const auto& size : sizes) {
            for (const float cut : cuts) {
                for (unsigned mask = 0; mask <= P::kAll; ++mask) {
                    const P::Rect r           = MakeRect(size[0], size[1], cut, mask);
                    const float   outlineArea = P::OutlineArea(P::BuildOutline(r));
                    const float   fillArea    = P::FillArea(P::BuildFill(r));
                    CHECK(Near(fillArea, outlineArea));
                }
            }
        }
    }

    // The fill's pieces are side by side, never stacked. Area agreement alone
    // cannot tell a gap plus an equal overlap from a clean tiling, so check the
    // x spans meet end to end as well.
    {
        const P::Rect r    = MakeRect(300.0f, 200.0f, 16.0f, P::kAll);
        const auto    fill = P::BuildFill(r);
        CHECK(fill.count == 3);
        for (int i = 0; i + 1 < fill.count; ++i) {
            float rightOfThis = fill.quads[i].v[0].x;
            for (const P::Point& p : fill.quads[i].v) {
                rightOfThis = std::fmax(rightOfThis, p.x);
            }
            float leftOfNext = fill.quads[i + 1].v[0].x;
            for (const P::Point& p : fill.quads[i + 1].v) {
                leftOfNext = std::fmin(leftOfNext, p.x);
            }
            CHECK(Near(rightOfThis, leftOfNext));
        }
        // A square rect collapses to the single middle quad and costs no more
        // than a plain rectangle would.
        CHECK(P::BuildFill(MakeRect(300.0f, 200.0f, 16.0f, P::kNone)).count == 1);
        CHECK(P::BuildFill(MakeRect(300.0f, 200.0f, 0.0f, P::kAll)).count == 1);
        // One cut side means two pieces, not three.
        CHECK(P::BuildFill(MakeRect(300.0f, 200.0f, 16.0f, P::kLeft)).count == 2);
    }

    // Clamping. Half the shorter side is the limit; past it the outline folds.
    {
        CHECK(Near(P::ClampCut(MakeRect(300.0f, 200.0f, 500.0f)), 100.0f));
        CHECK(Near(P::ClampCut(MakeRect(80.0f, 200.0f, 500.0f)), 40.0f));
        CHECK(P::ClampCut(MakeRect(300.0f, 200.0f, -4.0f)) == 0.0f);
        CHECK(P::ClampCut(MakeRect(0.0f, 0.0f, 8.0f)) == 0.0f);
        CHECK(P::ClampCut(MakeRect(300.0f, 200.0f, std::nanf(""))) == 0.0f);
        // A panel collapsing mid-animation must still produce something
        // drawable rather than a folded-over shape.
        CHECK(Near(P::OutlineArea(P::BuildOutline(MakeRect(0.0f, 120.0f, 20.0f))), 0.0f));
        CHECK(Near(P::FillArea(P::BuildFill(MakeRect(0.0f, 120.0f, 20.0f))), 0.0f));
    }

    // The clamped extreme is a diamond.
    {
        const float   s       = 120.0f;
        const P::Rect r       = MakeRect(s, s, s * 0.5f, P::kAll);
        const auto    outline = P::BuildOutline(r);
        CHECK(IsConvex(outline));
        // Four cuts of s/2 remove 4 * (s/2)^2 / 2 = s^2/2, half the square.
        CHECK(Near(P::OutlineArea(outline), s * s * 0.5f));
        CHECK(Near(P::FillArea(P::BuildFill(r)), s * s * 0.5f));
    }

    // ⚠ INSET MOVES EVERY EDGE THE SAME PERPENDICULAR DISTANCE. The straights
    // are easy and the diagonals are where a border pass goes wrong. The
    // top-left diagonal lies on x + y = min.x + min.y + cut; two such lines sit
    // |dk| / sqrt(2) apart. If the cut were carried through unchanged the
    // diagonals would drift relative to the straights and a stacked border would
    // read thicker at the corners than along the sides.
    {
        const P::Rect r        = MakeRect(300.0f, 200.0f, 24.0f, P::kAll);
        const float   depths[] = { 1.0f, 2.0f, 3.5f, 7.0f };
        for (const float d : depths) {
            const P::Rect in = P::Inset(r, d);
            CHECK(Near(in.min.y - r.min.y, d));  // top edge
            CHECK(Near(in.min.x - r.min.x, d));  // left edge
            CHECK(Near(r.max.x - in.max.x, d));  // right edge
            CHECK(Near(r.max.y - in.max.y, d));  // bottom edge
            const float k0 = r.min.x + r.min.y + P::ClampCut(r);
            const float k1 = in.min.x + in.min.y + P::ClampCut(in);
            CHECK(Near((k1 - k0) / std::sqrt(2.0f), d));
        }
    }

    // Inset past the cut squares the corner off, it does not invert it.
    {
        const P::Rect deep = P::Inset(MakeRect(300.0f, 200.0f, 3.0f, P::kAll), 40.0f);
        CHECK(deep.cut == 0.0f);
        CHECK(P::BuildOutline(deep).count == 4);
        CHECK(P::BuildFill(deep).count == 1);
    }

    // The mask survives an inset, so every border pass cuts the same corners.
    {
        const P::Rect r = MakeRect(300.0f, 200.0f, 20.0f, P::kTop);
        CHECK(P::Inset(r, 2.0f).corners == P::kTop);
        CHECK(P::BuildOutline(P::Inset(r, 2.0f)).count == 6);
    }

    if (g_failures == 0) {
        std::printf("ChamferPolicyTests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
