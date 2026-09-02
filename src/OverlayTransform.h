#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// The overlay offset sliders' engine-free half (OS-209): what a transform is,
// how it is quantised, what file a bake lands in, what the sidecar beside it
// says, the DDS header the bake is written under, and the one piece of
// arithmetic the compute shader and every test share.
//
// ⚠⚠ THE PICTURE IS BAKED, NOT SHIFTED. MEASURED 2026-08-18 (docs/handoffs/
// 2026-08-18-skin-changer-built-offsets-priced.md): no skee override key writes
// BSShaderMaterial::texCoordOffset, so a transform cannot ride an override the
// way a tint or an alpha does. What CAN ride key 9 is a texture path. So the
// overlay's own RGBA is resampled at the transform into a new DDS under
// textures\FittingRoom\baked\ and THAT path goes into key 9, exactly as any
// texture does. skee persists it in its co-save and repaints it at every
// install, and this mod never touches the [Ovl] material after the bake lands.
// One painter, which is the repo's rule.
//
// ⚠⚠ THE CACHE KEY IS THE INPUTS. cache-key-must-be-the-inputs-not-the-subject:
// the file name is a hash of (source path, offset, scale, rotation, bake
// version) and nothing else, so two layers on two characters asking for one
// picture share one file, and moving a slider back to a value it held before
// finds the file it made the first time. The transform is QUANTISED before it
// is hashed, so a slider's float noise cannot spawn a file per frame.
//
// ⚠ THE SIDECAR IS THE DECODER, NOT A SECOND STORE. skee holds the baked path
// under key 9; the page reads that path back and needs to know which art it
// came from and where the sliders sit. A hash is not invertible, so a small text
// file beside the DDS carries the source and the four numbers. It is written
// with the DDS and evicted with it. Nothing else remembers a transform: an FR
// co-save record here would be a second author of one appearance, and two
// painters of one appearance always drift.
namespace OS::OverlayTransform {

    // ---- the transform -------------------------------------------------

    struct Transform {
        float offsetX{ 0.0f };      // UV units, positive moves the art +u
        float offsetY{ 0.0f };      // UV units, positive moves the art +v
        float scale{ 1.0f };        // about the art's own pivot, 1 = as authored
        float rotationDeg{ 0.0f };  // degrees about the pivot

        friend bool operator==(const Transform&, const Transform&) = default;
    };

    // The slider ranges. Offsets span a whole texture in either direction, which
    // is further than anyone wants and exactly far enough that no reachable
    // position is refused. Scale bottoms out above zero so the inverse the
    // shader takes never divides by it.
    inline constexpr float kOffsetMin = -1.0f;
    inline constexpr float kOffsetMax = 1.0f;
    inline constexpr float kScaleMin  = 0.1f;
    inline constexpr float kScaleMax  = 4.0f;
    inline constexpr float kRotMin    = -180.0f;
    inline constexpr float kRotMax    = 180.0f;

    [[nodiscard]] inline constexpr float ClampF(float a_v, float a_lo, float a_hi) {
        return a_v < a_lo ? a_lo : (a_v > a_hi ? a_hi : a_v);
    }

    [[nodiscard]] inline constexpr Transform Clamp(const Transform& a_t) {
        return Transform{ ClampF(a_t.offsetX, kOffsetMin, kOffsetMax),
                          ClampF(a_t.offsetY, kOffsetMin, kOffsetMax),
                          ClampF(a_t.scale, kScaleMin, kScaleMax),
                          ClampF(a_t.rotationDeg, kRotMin, kRotMax) };
    }

    // ⚠ QUANTISED TO WHAT A SLIDER CAN SHOW, AND THAT IS THE CACHE KEY'S
    // GRANULARITY. Offsets and scale to a thousandth, rotation to a tenth of a
    // degree. Coarser than a float and finer than an eye at any zoom the page
    // offers, and every value that reaches a key or a sidecar has been through
    // here first, so a value read back and written again hashes to the same
    // file rather than to a neighbour of it.
    [[nodiscard]] inline float Round3(float a_v) {
        return std::round(a_v * 1000.0f) / 1000.0f;
    }
    [[nodiscard]] inline float Round1(float a_v) {
        return std::round(a_v * 10.0f) / 10.0f;
    }

    [[nodiscard]] inline Transform Quantise(const Transform& a_t) {
        const auto c = Clamp(a_t);
        Transform  q{ Round3(c.offsetX), Round3(c.offsetY), Round3(c.scale),
                     Round1(c.rotationDeg) };
        // -0.000 and 0.000 are one value; a signed zero would print differently
        // and hash differently while meaning the same picture.
        if (q.offsetX == 0.0f) { q.offsetX = 0.0f; }
        if (q.offsetY == 0.0f) { q.offsetY = 0.0f; }
        if (q.rotationDeg == 0.0f) { q.rotationDeg = 0.0f; }
        // ⚠ 180 AND -180 ARE ONE ROTATION. The slider reaches both ends and the
        // two must not be two files of one picture.
        if (q.rotationDeg == -180.0f) { q.rotationDeg = 180.0f; }
        return q;
    }

    // Identity means "no bake at all": the layer holds the source path itself
    // and no file is written. Judged after quantising, so a slider parked at
    // 0.0004 is identity rather than a bake nobody can see.
    [[nodiscard]] inline bool IsIdentity(const Transform& a_t) {
        const auto q = Quantise(a_t);
        return q.offsetX == 0.0f && q.offsetY == 0.0f && q.scale == 1.0f &&
               q.rotationDeg == 0.0f;
    }

    // ---- where a bake lives --------------------------------------------

    // Relative to textures\, as every override path is (OverlayPlan::
    // ToOverridePath). Written with the backslash the presets use.
    inline constexpr std::string_view kBakedDir = "FittingRoom\\baked\\";

    // ⚠⚠ BUMP THIS WHEN THE PICTURE A GIVEN INPUT PRODUCES CHANGES.
    // render-change-needs-a-key-change: the pivot rule, the sampling, the
    // output format and the sidecar layout are all part of what a stem names.
    // A change to any of them under the same version would serve every existing
    // file as if it were the new arithmetic, silently and for ever.
    inline constexpr std::uint32_t kBakeVersion = 1;

    // The source path as it is hashed and compared: lower case, backslashes.
    // Two spellings of one file are two keys, which costs a second bake and
    // nothing else; the picker writes one spelling.
    [[nodiscard]] inline std::string NormalisePath(std::string_view a_path) {
        std::string out;
        out.reserve(a_path.size());
        for (const auto ch : a_path) {
            const auto c = static_cast<unsigned char>(ch);
            out.push_back(c == '/' ? '\\' : static_cast<char>(std::tolower(c)));
        }
        return out;
    }

    // FNV-1a, 64 bit. Not cryptographic and does not need to be: the stem
    // has to be stable across sessions and machines and unlikely to collide
    // among a few hundred bakes, and it is printable in sixteen hex digits.
    [[nodiscard]] inline constexpr std::uint64_t Fnv1a64(std::string_view a_text) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (const auto ch : a_text) {
            h ^= static_cast<unsigned char>(ch);
            h *= 0x100000001b3ull;
        }
        return h;
    }

    // The canonical text a bake is named after. Everything that changes the
    // picture is in it and nothing that does not.
    [[nodiscard]] inline std::string Key(std::string_view a_source, const Transform& a_t) {
        const auto q = Quantise(a_t);
        return std::format("v{}|{}|{:.3f}|{:.3f}|{:.3f}|{:.1f}", kBakeVersion,
                           NormalisePath(a_source), q.offsetX, q.offsetY, q.scale,
                           q.rotationDeg);
    }

    [[nodiscard]] inline std::string FileStem(std::string_view a_source, const Transform& a_t) {
        return std::format("{:016x}", Fnv1a64(Key(a_source, a_t)));
    }

    // The override path that goes into key 9: "FittingRoom\baked\<stem>.dds".
    [[nodiscard]] inline std::string BakedPath(std::string_view a_source, const Transform& a_t) {
        return std::string{ kBakedDir } + FileStem(a_source, a_t) + ".dds";
    }

    // Whether an override path read back off a layer is one of ours.
    [[nodiscard]] inline bool IsBakedPath(std::string_view a_overridePath) {
        const auto norm = NormalisePath(a_overridePath);
        const auto dir  = NormalisePath(kBakedDir);
        return norm.size() > dir.size() + 4 && norm.compare(0, dir.size(), dir) == 0 &&
               norm.compare(norm.size() - 4, 4, ".dds") == 0;
    }

    // ---- the probe's verdict --------------------------------------------
    //
    // What a drag's in-memory texture is named. The forged NiSourceTexture the
    // bake puts on the [Ovl] material during a drag carries this prefix and
    // never a file path, so a probe that reads it back knows a drag was live
    // when it fired, which is a timing fact and not a load failure.
    inline constexpr std::string_view kTransientPrefix = "FittingRoom\\baked\\transient\\";

    enum class Probe : std::uint8_t {
        kMatch,        // our file, at a real size: loaded through the engine's loader
        kPlaceholder,  // our path, but the engine substituted its stand-in
        kInProgress,   // the transient is on the material: a drag was live at probe time
        kMismatch,     // some other texture: skee has not applied the path, or applied another
    };

    // ⚠ MEASURED 2026-08-18: eleven probes in one session, seven MATCH and four
    // "MISMATCH" whose diffuse read 'FittingRoom\baked\transient\1048576|Face
    // [Ovl0]', each followed within a second by another release and a MATCH.
    // The user had started the next drag before the probe of the last release
    // ran. That verdict said skee had failed when nothing had, which is the
    // one thing an instrument must not do; the transient is now its own answer.
    [[nodiscard]] inline Probe ProbeVerdict(std::string_view a_expected, std::string_view a_got,
                                            std::uint32_t a_width, std::uint32_t a_height) {
        const auto expNorm = NormalisePath(a_expected);
        const auto gotNorm = NormalisePath(a_got);
        const auto trans   = NormalisePath(kTransientPrefix);
        if (gotNorm.size() >= trans.size() && gotNorm.compare(0, trans.size(), trans) == 0) {
            return Probe::kInProgress;
        }
        const bool nameHit = !expNorm.empty() && gotNorm.size() >= expNorm.size() &&
                             gotNorm.compare(gotNorm.size() - expNorm.size(), expNorm.size(),
                                             expNorm) == 0;
        if (!nameHit) {
            return Probe::kMismatch;
        }
        return a_width >= 16 && a_height >= 16 ? Probe::kMatch : Probe::kPlaceholder;
    }

    // The stem out of a baked path, empty when it is not one.
    [[nodiscard]] inline std::string StemOf(std::string_view a_overridePath) {
        if (!IsBakedPath(a_overridePath)) {
            return {};
        }
        const auto norm = NormalisePath(a_overridePath);
        const auto dir  = NormalisePath(kBakedDir);
        return norm.substr(dir.size(), norm.size() - dir.size() - 4);
    }

    // ---- the sidecar ---------------------------------------------------
    //
    // A handful of key=value lines beside the DDS, CRLF, ASCII apart from the
    // path. The first line names the format and its version so a file from a
    // later build is refused rather than misread.

    inline constexpr std::string_view kSidecarMagic = "FittingRoom overlay bake v";

    [[nodiscard]] inline std::string SidecarText(std::string_view a_source, const Transform& a_t) {
        const auto q = Quantise(a_t);
        return std::format("{}{}\r\nsource={}\r\noffsetX={:.3f}\r\noffsetY={:.3f}\r\n"
                           "scale={:.3f}\r\nrotation={:.1f}\r\n",
                           kSidecarMagic, kBakeVersion, a_source, q.offsetX, q.offsetY,
                           q.scale, q.rotationDeg);
    }

    struct Decoded {
        bool        ok{ false };
        std::string source;
        Transform   transform;
    };

    namespace detail {
        [[nodiscard]] inline bool ParseFloat(std::string_view a_text, float& a_out) {
            // from_chars is locale-blind, which is the point: the sidecar is
            // written with a full stop whatever the machine's own separator.
            const auto* first = a_text.data();
            const auto* last  = a_text.data() + a_text.size();
            const auto  r     = std::from_chars(first, last, a_out);
            return r.ec == std::errc{} && r.ptr == last;
        }
    }  // namespace detail

    [[nodiscard]] inline Decoded ParseSidecar(std::string_view a_text) {
        Decoded out;
        if (a_text.substr(0, kSidecarMagic.size()) != kSidecarMagic) {
            return out;
        }
        std::size_t pos = 0;
        bool        first = true;
        bool        gotSource = false;
        while (pos < a_text.size()) {
            auto end = a_text.find('\n', pos);
            if (end == std::string_view::npos) {
                end = a_text.size();
            }
            auto line = a_text.substr(pos, end - pos);
            pos       = end + 1;
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (first) {
                first = false;
                std::uint32_t version = 0;
                const auto    tail    = line.substr(kSidecarMagic.size());
                const auto    r = std::from_chars(tail.data(), tail.data() + tail.size(), version);
                if (r.ec != std::errc{} || version != kBakeVersion) {
                    return out;  // another build's file; refused whole
                }
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string_view::npos) {
                continue;
            }
            const auto key   = line.substr(0, eq);
            const auto value = line.substr(eq + 1);
            if (key == "source") {
                out.source = std::string{ value };
                gotSource  = !out.source.empty();
            } else if (key == "offsetX") {
                if (!detail::ParseFloat(value, out.transform.offsetX)) { return Decoded{}; }
            } else if (key == "offsetY") {
                if (!detail::ParseFloat(value, out.transform.offsetY)) { return Decoded{}; }
            } else if (key == "scale") {
                if (!detail::ParseFloat(value, out.transform.scale)) { return Decoded{}; }
            } else if (key == "rotation") {
                if (!detail::ParseFloat(value, out.transform.rotationDeg)) { return Decoded{}; }
            }
        }
        out.transform = Quantise(out.transform);
        out.ok        = gotSource;
        return out;
    }

    // ---- the DDS the bake is written as --------------------------------
    //
    // A DX10-header DDS of R8G8B8A8_UNORM (or _SRGB when the source was typed
    // sRGB), full mip chain, tightly packed levels. The DX10 header rather than
    // the legacy pixel-format block because that is the header BC7 already
    // needs and the engine already reads, and because R8G8B8A8 is the byte
    // order the readback hands over, so nothing is swizzled on the way to disk.
    //
    // ⚠ UNCOMPRESSED, AND THAT IS A KNOWN COST. A 2048 bake is 21.3 MiB on
    // disk and in VRAM against a BC7 source's 5.3. It is what a bake with no
    // block encoder in the plugin can write, and iOverlayBakeCapPx bounds it.

    inline constexpr std::size_t kDdsHeaderBytes = 4 + 124 + 20;

    inline constexpr std::uint32_t kDxgiR8G8B8A8Unorm     = 28;
    inline constexpr std::uint32_t kDxgiR8G8B8A8UnormSrgb = 29;

    [[nodiscard]] inline constexpr std::uint32_t LevelDim(std::uint32_t a_dim, std::uint32_t a_level) {
        const auto d = a_dim >> a_level;
        return d == 0 ? 1u : d;
    }

    [[nodiscard]] inline constexpr std::uint32_t MipCount(std::uint32_t a_w, std::uint32_t a_h) {
        std::uint32_t n = 1;
        auto          m = a_w > a_h ? a_w : a_h;
        while (m > 1) {
            m >>= 1;
            ++n;
        }
        return n;
    }

    [[nodiscard]] inline constexpr std::uint64_t LevelBytes(std::uint32_t a_w, std::uint32_t a_h,
                                                            std::uint32_t a_level) {
        return static_cast<std::uint64_t>(LevelDim(a_w, a_level)) * LevelDim(a_h, a_level) * 4ull;
    }

    [[nodiscard]] inline constexpr std::uint64_t ChainBytes(std::uint32_t a_w, std::uint32_t a_h,
                                                            std::uint32_t a_mips) {
        std::uint64_t total = 0;
        for (std::uint32_t i = 0; i < a_mips; ++i) {
            total += LevelBytes(a_w, a_h, i);
        }
        return total;
    }

    namespace detail {
        inline void Put32(std::uint8_t* a_dst, std::uint32_t a_v) {
            a_dst[0] = static_cast<std::uint8_t>(a_v & 0xFFu);
            a_dst[1] = static_cast<std::uint8_t>((a_v >> 8) & 0xFFu);
            a_dst[2] = static_cast<std::uint8_t>((a_v >> 16) & 0xFFu);
            a_dst[3] = static_cast<std::uint8_t>((a_v >> 24) & 0xFFu);
        }
        [[nodiscard]] inline std::uint32_t Get32(const std::uint8_t* a_src) {
            return static_cast<std::uint32_t>(a_src[0]) |
                   (static_cast<std::uint32_t>(a_src[1]) << 8) |
                   (static_cast<std::uint32_t>(a_src[2]) << 16) |
                   (static_cast<std::uint32_t>(a_src[3]) << 24);
        }
    }  // namespace detail

    [[nodiscard]] inline std::array<std::uint8_t, kDdsHeaderBytes> DdsHeader(std::uint32_t a_w,
                                                                             std::uint32_t a_h,
                                                                             std::uint32_t a_mips,
                                                                             bool          a_srgb) {
        std::array<std::uint8_t, kDdsHeaderBytes> h{};
        using detail::Put32;
        std::memcpy(h.data(), "DDS ", 4);
        auto* d = h.data() + 4;  // DDS_HEADER
        // DDSD_CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT | MIPMAPCOUNT
        constexpr std::uint32_t kFlags = 0x1u | 0x2u | 0x4u | 0x8u | 0x1000u | 0x20000u;
        Put32(d + 0, 124);
        Put32(d + 4, kFlags);
        Put32(d + 8, a_h);
        Put32(d + 12, a_w);
        Put32(d + 16, a_w * 4u);  // pitch of level 0
        Put32(d + 20, 0);         // depth
        Put32(d + 24, a_mips);
        // reserved1[11] at 28..71 stays zero
        auto* pf = d + 72;  // DDS_PIXELFORMAT
        Put32(pf + 0, 32);
        Put32(pf + 4, 0x4u);  // DDPF_FOURCC
        std::memcpy(pf + 8, "DX10", 4);
        // rgb bit count and masks stay zero under FOURCC
        // caps: DDSCAPS_TEXTURE, plus COMPLEX | MIPMAP when there is a chain
        const std::uint32_t caps = 0x1000u | (a_mips > 1 ? (0x8u | 0x400000u) : 0u);
        Put32(d + 104, caps);
        // caps2..4 and reserved2 stay zero
        auto* x = h.data() + 128;  // DDS_HEADER_DXT10
        Put32(x + 0, a_srgb ? kDxgiR8G8B8A8UnormSrgb : kDxgiR8G8B8A8Unorm);
        Put32(x + 4, 3);  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
        Put32(x + 8, 0);  // miscFlag
        Put32(x + 12, 1); // arraySize
        Put32(x + 16, 0); // miscFlags2, alpha mode unknown
        return h;
    }

    // What a header says, for the tests and for a sanity read of a file on
    // disk. ok is false for anything that is not a DX10 R8G8B8A8 header of the
    // kind DdsHeader writes.
    struct DdsInfo {
        bool          ok{ false };
        std::uint32_t width{ 0 };
        std::uint32_t height{ 0 };
        std::uint32_t mips{ 0 };
        bool          srgb{ false };
    };

    [[nodiscard]] inline DdsInfo ReadDdsHeader(const std::uint8_t* a_bytes, std::size_t a_size) {
        DdsInfo info;
        if (!a_bytes || a_size < kDdsHeaderBytes || std::memcmp(a_bytes, "DDS ", 4) != 0) {
            return info;
        }
        using detail::Get32;
        const auto* d = a_bytes + 4;
        if (Get32(d) != 124 || std::memcmp(d + 72 + 8, "DX10", 4) != 0) {
            return info;
        }
        const auto fmt = Get32(a_bytes + 128);
        if (fmt != kDxgiR8G8B8A8Unorm && fmt != kDxgiR8G8B8A8UnormSrgb) {
            return info;
        }
        info.height = Get32(d + 8);
        info.width  = Get32(d + 12);
        info.mips   = Get32(d + 24);
        info.srgb   = fmt == kDxgiR8G8B8A8UnormSrgb;
        info.ok     = info.width > 0 && info.height > 0 && info.mips > 0;
        return info;
    }

    // ---- what size a bake comes out at ---------------------------------

    struct OutputSize {
        std::uint32_t width{ 0 };
        std::uint32_t height{ 0 };
        std::uint32_t halvings{ 0 };  // how many times the source was halved
    };

    // Halve until the longest side is within the cap; 0 means no cap. The
    // halvings feed the sample LOD so the shader reads the matching level of
    // the source rather than aliasing mip 0 down.
    [[nodiscard]] inline constexpr OutputSize FitToCap(std::uint32_t a_w, std::uint32_t a_h,
                                                       std::uint32_t a_capPx) {
        OutputSize out{ a_w, a_h, 0 };
        if (a_capPx == 0) {
            return out;
        }
        while ((out.width > a_capPx || out.height > a_capPx) && (out.width > 1 || out.height > 1)) {
            out.width  = out.width > 1 ? out.width / 2 : 1;
            out.height = out.height > 1 ? out.height / 2 : 1;
            ++out.halvings;
        }
        return out;
    }

    // The mip the shader samples: the halvings above, plus one level per
    // halving the SCALE itself performs. Shrinking the art to half its size
    // reads level 1 of it, which is what keeps a shrunk tattoo from sparkling.
    [[nodiscard]] inline float SampleLod(std::uint32_t a_halvings, float a_scale) {
        const float s   = a_scale < 1e-4f ? 1e-4f : a_scale;
        const float own = s < 1.0f ? -std::log2(s) : 0.0f;
        return static_cast<float>(a_halvings) + own;
    }

    // ---- the one piece of arithmetic the shader and the tests share ------
    //
    // ⚠⚠ THIS IS THE MAPPING THE COMPUTE SHADER RUNS, WRITTEN OUT ONCE HERE
    // AND ONCE IN HLSL, AND THE TESTS BELOW ARE WHAT HOLD THE TWO TOGETHER.
    // The forward transform takes a source point p to
    //
    //     dst = R(rot) * scale * (p - pivot) + pivot + offset
    //
    // and the shader, per destination texel, needs the INVERSE: which source
    // texel lands here. Rotation is about the art's own PIVOT (the alpha
    // centroid the bake measures off the source, (0.5, 0.5) for a blank one),
    // so scaling a tattoo grows the tattoo where it sits rather than sliding
    // it towards a corner of the sheet. Rotation is in UV space, positive
    // clockwise as the texture is viewed with v downward.

    struct Uv {
        float u{ 0.0f };
        float v{ 0.0f };
    };

    inline constexpr float kPi = 3.14159265358979323846f;

    [[nodiscard]] inline Uv ToDest(Uv a_src, const Transform& a_t, Uv a_pivot) {
        const float rad = a_t.rotationDeg * kPi / 180.0f;
        const float c   = std::cos(rad);
        const float s   = std::sin(rad);
        const float qx  = (a_src.u - a_pivot.u) * a_t.scale;
        const float qy  = (a_src.v - a_pivot.v) * a_t.scale;
        return Uv{ c * qx - s * qy + a_pivot.u + a_t.offsetX,
                   s * qx + c * qy + a_pivot.v + a_t.offsetY };
    }

    [[nodiscard]] inline Uv ToSource(Uv a_dst, const Transform& a_t, Uv a_pivot) {
        const float rad = -a_t.rotationDeg * kPi / 180.0f;
        const float c   = std::cos(rad);
        const float s   = std::sin(rad);
        const float qx  = a_dst.u - a_pivot.u - a_t.offsetX;
        const float qy  = a_dst.v - a_pivot.v - a_t.offsetY;
        const float rx  = c * qx - s * qy;
        const float ry  = s * qx + c * qy;
        const float inv = 1.0f / (a_t.scale < 1e-4f ? 1e-4f : a_t.scale);
        return Uv{ rx * inv + a_pivot.u, ry * inv + a_pivot.v };
    }

    // Whether a source point exists: outside [0,1] the bake writes transparent
    // black, which is what a moved tattoo leaves behind it.
    [[nodiscard]] inline constexpr bool InSource(Uv a_uv) {
        return a_uv.u >= 0.0f && a_uv.u <= 1.0f && a_uv.v >= 0.0f && a_uv.v <= 1.0f;
    }

    // ---- keeping the folder bounded ------------------------------------
    //
    // ⚠ EVICTION IS BY AGE AND NEVER TOUCHES THE FILE JUST WRITTEN OR ONE
    // BEING KEPT. A save can name a file that is deleted here, and then that
    // layer loads as the engine's placeholder until it is baked again; the
    // budget is what makes that rare and the log line is what makes it
    // findable. 0 means no budget and nothing is ever evicted.

    struct CacheFile {
        std::string   stem;
        std::uint64_t bytes{ 0 };
        std::int64_t  mtime{ 0 };  // any monotone stamp; older is smaller
    };

    [[nodiscard]] inline std::vector<std::string> EvictionPlan(std::vector<CacheFile> a_files,
                                                               std::uint64_t          a_budgetBytes,
                                                               std::string_view       a_keepStem) {
        std::vector<std::string> evict;
        if (a_budgetBytes == 0) {
            return evict;
        }
        std::uint64_t total = 0;
        for (const auto& f : a_files) {
            total += f.bytes;
        }
        std::sort(a_files.begin(), a_files.end(), [](const CacheFile& a, const CacheFile& b) {
            return a.mtime != b.mtime ? a.mtime < b.mtime : a.stem < b.stem;
        });
        for (const auto& f : a_files) {
            if (total <= a_budgetBytes) {
                break;
            }
            if (f.stem == a_keepStem) {
                continue;
            }
            evict.push_back(f.stem);
            total -= f.bytes;
        }
        return evict;
    }

}  // namespace OS::OverlayTransform
