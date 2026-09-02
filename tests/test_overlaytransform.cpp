#include "../src/OverlayTransform.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x);          \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

namespace {
    bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
}

int main() {
    using namespace OS::OverlayTransform;

    {  // Quantise: three decimals, one for rotation, clamped, signed zero folded.
        const auto q = Quantise(Transform{ 0.12345f, -0.00049f, 1.23456f, 15.04f });
        CHECK(Near(q.offsetX, 0.123f, 1e-6f));
        CHECK(q.offsetY == 0.0f && !std::signbit(q.offsetY));
        CHECK(Near(q.scale, 1.235f, 1e-6f));
        CHECK(Near(q.rotationDeg, 15.0f, 1e-6f));
        const auto c = Quantise(Transform{ 3.0f, -3.0f, 0.0f, 400.0f });
        CHECK(c.offsetX == kOffsetMax && c.offsetY == kOffsetMin);
        CHECK(Near(c.scale, kScaleMin, 1e-6f) && c.rotationDeg == kRotMax);
        // 180 and -180 are one rotation and one file.
        CHECK(Quantise(Transform{ 0, 0, 1, -180.0f }).rotationDeg == 180.0f);
    }
    {  // Identity is judged after quantising.
        CHECK(IsIdentity(Transform{}));
        CHECK(IsIdentity(Transform{ 0.0004f, -0.0004f, 1.0004f, 0.04f }));
        CHECK(!IsIdentity(Transform{ 0.001f, 0, 1, 0 }));
        CHECK(!IsIdentity(Transform{ 0, 0, 1.001f, 0 }));
        CHECK(!IsIdentity(Transform{ 0, 0, 1, 0.1f }));
    }
    {  // The key is the inputs, normalised, and nothing else.
        const Transform t{ 0.1f, -0.05f, 1.25f, 15.0f };
        const auto      k1 = Key("Actors\\Character\\Overlays\\Pack\\Tattoo.dds", t);
        const auto      k2 = Key("actors/character/overlays/pack/tattoo.dds", t);
        CHECK(k1 == k2);
        CHECK(k1 == "v1|actors\\character\\overlays\\pack\\tattoo.dds|0.100|-0.050|1.250|15.0");
        // A different transform is a different key; float noise is not.
        CHECK(Key("a.dds", t) != Key("a.dds", Transform{ 0.1f, -0.05f, 1.25f, 15.1f }));
        CHECK(Key("a.dds", t) == Key("a.dds", Transform{ 0.1004f, -0.0504f, 1.2504f, 15.04f }));
        // The stem is sixteen hex digits and stable.
        const auto stem = FileStem("a.dds", t);
        CHECK(stem.size() == 16);
        CHECK(stem == FileStem("A.DDS", t));
        CHECK(stem != FileStem("b.dds", t));
        // The path is under the baked folder in override form.
        const auto path = BakedPath("a.dds", t);
        CHECK(path == "FittingRoom\\baked\\" + stem + ".dds");
        CHECK(IsBakedPath(path));
        CHECK(IsBakedPath("fittingroom/BAKED/0123456789abcdef.DDS"));
        CHECK(!IsBakedPath("Actors\\Character\\Overlays\\Default.dds"));
        CHECK(!IsBakedPath("FittingRoom\\baked\\"));
        CHECK(!IsBakedPath("FittingRoom\\baked\\x.txt"));
        CHECK(StemOf(path) == stem);
        CHECK(StemOf("Actors\\Character\\Overlays\\Default.dds").empty());
    }
    {  // The probe's verdict: the four readings a field log carried 2026-08-18.
        const std::string exp = "FittingRoom\\baked\\8e1499e005ff91fd.dds";
        // The engine names the load with the 'textures\' root and any case.
        CHECK(ProbeVerdict(exp, "textures\\FittingRoom\\BAKED\\8e1499e005ff91fd.dds", 2048,
                           2048) == Probe::kMatch);
        CHECK(ProbeVerdict(exp, "textures/fittingroom/baked/8e1499e005ff91fd.dds", 16, 16) ==
              Probe::kMatch);
        // Our path, the engine's stand-in.
        CHECK(ProbeVerdict(exp, "textures\\FittingRoom\\baked\\8e1499e005ff91fd.dds", 4, 4) ==
              Probe::kPlaceholder);
        // The drag texture, whatever size it took: a drag was live, no verdict
        // on the file at all.
        CHECK(ProbeVerdict(exp, "FittingRoom\\baked\\transient\\1048576|Face [Ovl0]", 1024,
                           1024) == Probe::kInProgress);
        CHECK(ProbeVerdict(exp, "fittingroom/baked/TRANSIENT/x", 0, 0) == Probe::kInProgress);
        // Some other texture, at any size.
        CHECK(ProbeVerdict(exp, "textures\\FittingRoom\\baked\\b3b04cb7b0e840cc.dds", 2048,
                           2048) == Probe::kMismatch);
        CHECK(ProbeVerdict(exp, "textures\\actors\\character\\overlays\\default.dds", 2048,
                           2048) == Probe::kMismatch);
        CHECK(ProbeVerdict(exp, "", 0, 0) == Probe::kMismatch);
        // No expectation matches nothing but the transient.
        CHECK(ProbeVerdict("", "textures\\a.dds", 64, 64) == Probe::kMismatch);
    }
    {  // FNV-1a 64 against a known vector.
        CHECK(Fnv1a64("") == 0xcbf29ce484222325ull);
        CHECK(Fnv1a64("a") == 0xaf63dc4c8601ec8cull);
    }
    {  // The sidecar round-trips, refuses another version, and quantises on read.
        const Transform t{ 0.1f, -0.05f, 1.25f, 15.0f };
        const auto      text = SidecarText("Actors\\Character\\Overlays\\Pack\\Tattoo.dds", t);
        CHECK(text.rfind("FittingRoom overlay bake v1\r\n", 0) == 0);
        CHECK(text.find("\r\nsource=Actors\\Character\\Overlays\\Pack\\Tattoo.dds\r\n") !=
              std::string::npos);
        const auto d = ParseSidecar(text);
        CHECK(d.ok);
        CHECK(d.source == "Actors\\Character\\Overlays\\Pack\\Tattoo.dds");
        CHECK(d.transform == Quantise(t));
        // Missing source: not usable.
        CHECK(!ParseSidecar("FittingRoom overlay bake v1\r\noffsetX=0.1\r\n").ok);
        // Another version: refused whole.
        CHECK(!ParseSidecar("FittingRoom overlay bake v2\r\nsource=a.dds\r\n").ok);
        // Not ours at all.
        CHECK(!ParseSidecar("hello").ok);
        CHECK(!ParseSidecar("").ok);
        // A bad number refuses rather than reading zero.
        CHECK(!ParseSidecar("FittingRoom overlay bake v1\r\nsource=a.dds\r\nscale=abc\r\n").ok);
        // LF-only line endings parse the same.
        const auto lf = ParseSidecar("FittingRoom overlay bake v1\nsource=a.dds\noffsetX=0.25\n");
        CHECK(lf.ok && lf.source == "a.dds" && Near(lf.transform.offsetX, 0.25f));
        // Missing numbers keep their identity defaults.
        CHECK(lf.transform.scale == 1.0f && lf.transform.rotationDeg == 0.0f);
    }
    {  // The DDS header: DX10, R8G8B8A8, the sizes and the mip count in place.
        const auto h = DdsHeader(2048, 1024, 12, false);
        CHECK(h.size() == 148);
        const auto info = ReadDdsHeader(h.data(), h.size());
        CHECK(info.ok);
        CHECK(info.width == 2048 && info.height == 1024 && info.mips == 12 && !info.srgb);
        const auto hs = DdsHeader(4, 4, 1, true);
        const auto is = ReadDdsHeader(hs.data(), hs.size());
        CHECK(is.ok && is.srgb && is.mips == 1);
        // The magic, the header size and the FourCC sit where DDS says.
        CHECK(h[0] == 'D' && h[1] == 'D' && h[2] == 'S' && h[3] == ' ');
        CHECK(h[4] == 124 && h[5] == 0);
        CHECK(h[4 + 72 + 8] == 'D' && h[4 + 72 + 9] == 'X' && h[4 + 72 + 10] == '1' &&
              h[4 + 72 + 11] == '0');
        // Pitch of level 0 is width * 4.
        CHECK(h[4 + 16] == 0 && h[4 + 17] == 0x20 && h[4 + 18] == 0 && h[4 + 19] == 0);
        // Not a header: refused.
        CHECK(!ReadDdsHeader(nullptr, 0).ok);
        std::uint8_t junk[148]{};
        CHECK(!ReadDdsHeader(junk, sizeof junk).ok);
    }
    {  // Mip arithmetic.
        CHECK(MipCount(1, 1) == 1);
        CHECK(MipCount(2048, 2048) == 12);
        CHECK(MipCount(2048, 512) == 12);
        CHECK(MipCount(4096, 4096) == 13);
        CHECK(LevelDim(2048, 11) == 1);
        CHECK(LevelDim(2048, 12) == 1);
        CHECK(LevelBytes(4, 2, 0) == 32);
        CHECK(LevelBytes(4, 2, 1) == 8);
        CHECK(LevelBytes(4, 2, 2) == 4);
        CHECK(ChainBytes(4, 2, 3) == 44);
        CHECK(ChainBytes(2048, 2048, 12) == 22369620ull);  // 4 * (4^12 - 1) / 3
    }
    {  // Fitting to a cap halves and counts, and 0 is no cap.
        const auto a = FitToCap(4096, 4096, 2048);
        CHECK(a.width == 2048 && a.height == 2048 && a.halvings == 1);
        const auto b = FitToCap(4096, 2048, 1024);
        CHECK(b.width == 1024 && b.height == 512 && b.halvings == 2);
        const auto c = FitToCap(512, 512, 2048);
        CHECK(c.width == 512 && c.halvings == 0);
        const auto d = FitToCap(4096, 4096, 0);
        CHECK(d.width == 4096 && d.halvings == 0);
        const auto e = FitToCap(3000, 100, 1024);
        CHECK(e.width == 750 && e.height == 25 && e.halvings == 2);
    }
    {  // The sample LOD: halvings plus what the scale shrinks.
        CHECK(Near(SampleLod(0, 1.0f), 0.0f));
        CHECK(Near(SampleLod(1, 1.0f), 1.0f));
        CHECK(Near(SampleLod(0, 0.5f), 1.0f));
        CHECK(Near(SampleLod(1, 0.25f), 3.0f));
        CHECK(Near(SampleLod(0, 2.0f), 0.0f));  // magnifying reads level 0
    }
    {  // ToSource inverts ToDest, for the shader's arithmetic.
        const Uv pivot{ 0.3f, 0.7f };
        const Transform ts[]{
            Transform{},
            Transform{ 0.1f, -0.2f, 1.0f, 0.0f },
            Transform{ 0.0f, 0.0f, 2.0f, 0.0f },
            Transform{ 0.0f, 0.0f, 1.0f, 90.0f },
            Transform{ 0.05f, 0.1f, 0.5f, -37.5f },
            Transform{ -0.4f, 0.25f, 3.0f, 180.0f },
        };
        const Uv points[]{ { 0.0f, 0.0f }, { 0.5f, 0.5f }, { 0.3f, 0.7f }, { 1.0f, 0.2f } };
        for (const auto& t : ts) {
            for (const auto& p : points) {
                const auto d = ToDest(p, t, pivot);
                const auto s = ToSource(d, t, pivot);
                CHECK(Near(s.u, p.u, 1e-4f) && Near(s.v, p.v, 1e-4f));
            }
        }
        // Identity maps a point to itself.
        const auto id = ToSource(Uv{ 0.25f, 0.75f }, Transform{}, pivot);
        CHECK(Near(id.u, 0.25f) && Near(id.v, 0.75f));
        // An offset of +0.1 in u: the destination texel at pivot+0.1 reads the
        // source at the pivot, so the art has MOVED +u.
        const auto o = ToSource(Uv{ 0.4f, 0.7f }, Transform{ 0.1f, 0, 1, 0 }, pivot);
        CHECK(Near(o.u, 0.3f) && Near(o.v, 0.7f));
        // Scale 2 about the pivot: a texel 0.2 from the pivot reads 0.1 from it.
        const auto sc = ToSource(Uv{ 0.5f, 0.7f }, Transform{ 0, 0, 2, 0 }, pivot);
        CHECK(Near(sc.u, 0.4f) && Near(sc.v, 0.7f));
        // The pivot itself is fixed under scale and rotation.
        const auto pv = ToSource(pivot, Transform{ 0, 0, 3, 123 }, pivot);
        CHECK(Near(pv.u, pivot.u) && Near(pv.v, pivot.v));
        // 90 degrees: +u from the pivot came from -v of the source (v down,
        // clockwise), so the source point is above the pivot.
        const auto r = ToSource(Uv{ 0.4f, 0.7f }, Transform{ 0, 0, 1, 90 }, pivot);
        CHECK(Near(r.u, 0.3f) && Near(r.v, 0.6f));
        CHECK(InSource(Uv{ 0, 0 }) && InSource(Uv{ 1, 1 }));
        CHECK(!InSource(Uv{ -0.001f, 0.5f }) && !InSource(Uv{ 0.5f, 1.001f }));
    }
    {  // Eviction: oldest first, never the kept stem, nothing under budget.
        std::vector<CacheFile> files{
            { "a", 100, 1 }, { "b", 100, 2 }, { "c", 100, 3 }, { "d", 100, 4 },
        };
        CHECK(EvictionPlan(files, 0, "d").empty());        // no budget, no eviction
        CHECK(EvictionPlan(files, 400, "d").empty());      // at budget
        const auto one = EvictionPlan(files, 300, "d");
        CHECK(one.size() == 1 && one[0] == "a");
        const auto two = EvictionPlan(files, 250, "d");
        CHECK(two.size() == 2 && two[0] == "a" && two[1] == "b");
        // The kept stem is skipped even when it is the oldest.
        const auto keep = EvictionPlan(files, 300, "a");
        CHECK(keep.size() == 1 && keep[0] == "b");
        // Ties break by stem so the plan is deterministic.
        std::vector<CacheFile> tie{ { "y", 50, 1 }, { "x", 50, 1 }, { "z", 50, 2 } };
        const auto tp = EvictionPlan(tie, 100, "");
        CHECK(tp.size() == 1 && tp[0] == "x");
    }

    std::printf("OverlayTransformTests: all checks passed\n");
    return 0;
}
