#include "DyeTexture.h"

#include "DyeKey.h"      // the RAMP half of the cache key, kept pure and tested
#include "DyeQuality.h"  // OS-140: which mip, how many bytes, has it settled
#include "EditorWindow.h"  // PumpPreviews, the preview grid's per-present tick
// Portrait.h is deliberately not included: the portrait pump is parked (tag portrait-capture-parked).
#include "Requip.h"        // Tick, the requip transition's per-present clock
#include "OutfitDye.h"   // QueueRepaint, armed when a build completes
#include "PreviewFrame.h"  // the preview grid's Present counter, bumped in the thunk
#include "Settings.h"    // the cache's VRAM budget and the two caps

#include <d3d11.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OS::DyeTexture {

    namespace {

        // ⚠ BSGraphics::Texture IS NiTexture::RendererData. CommonLib forward
        // declares the first (RE/N/NiSourceTexture.h:10) and fully defines the
        // second (RE/N/NiTexture.h:67), and they are the same 0x28 bytes. Three
        // shipped PDBs in the dev load order agree on the two fields used here,
        // +0x00 `ID3D11Texture2D*` and +0x10 `ID3D11ShaderResourceView*`.
        //
        // ⚠ THOSE TWO ARE THE ONLY FIELDS WITH EVIDENCE BEHIND THEM. CommonLib
        // reads 0x18/0x1A as width then height and Community Shaders' PDB as
        // height then width, and the 2026-08-05 field run read (0,0) out of them
        // on all eleven worn shapes of a dressed character. The engine does not
        // populate them. Do not build on the rest of this struct without
        // measuring it first.
        using RendererData = RE::NiTexture::RendererData;

        // ⚠ THE OVERLAY, NOT A MULTIPLY, AND THAT IS A CHOICE RATHER THAN AN
        // INHERITANCE. Writing new pixels escapes the vanilla shader's
        // D*D + 2*T*D*(1-D) entirely, which is the prize this rung was chasing.
        // Keeping the same curve anyway is what makes a dyed piece look dyed
        // rather than painted, and it means a colour picked on an ordinary
        // garment lands the same way on a reflective one. The freedom is still
        // there if the finish work ever wants it.
        //
        // ⚠ THE sRGB BRANCH IS MEASURED DEAD AND KEPT ANYWAY. Every worn diffuse
        // on the 2026-08-05 character read srgb=false: DXGI 71 (BC1_UNORM) on
        // the armour and 98 (BC7_UNORM) on the head. Skyrim types its diffuses
        // plainly. One outfit is not a load order, and the branch costs one bit
        // and one lerp.
        // ⚠ SHARED BY BOTH SHADERS BELOW, so the 2D and the cube path cannot
        // drift into two different ideas of what a recolour is.
        //
        // ⚠ LUMINANCE TIMES HUE, NOT A MULTIPLY, AND THE MASK IS WHY. Tinting
        // the envmap MASK was tried first and measured dead: the shader reads
        // one channel of it, so a red dye left it at times 1 and did nothing
        // while a blue dye took it to times 0 and switched the reflection off.
        // The mask is a strength control. The cubemap is the only thing in the
        // envmap path carrying colour, and multiplying THAT would repeat the
        // trap one level up, because a gold cubemap has almost no blue in it and
        // a blue dye would give near-black instead of blue metal. Taking the
        // luminance keeps every highlight and every shape in the reflection and
        // repaints only its colour.
        constexpr char kCommon[] = R"(
float3 Recolour(float3 src, float3 tint)
{
    float lum = dot(src, float3(0.2126, 0.7152, 0.0722));
    float m   = max(max(tint.r, tint.g), max(tint.b, 1e-4));
    return lum * (tint / m);
}

// Which curve a NON-RECOLOUR build runs, chosen by the blend's own flag bit.
//
// All four are per channel and take the source as the base and the dye as the
// top layer, so they are the photo-editor formulas rather than anything of
// ours. None clamps: every one maps [0,1]x[0,1] into [0,1], and the UNORM
// write is what would catch a tint outside that range.
//
// ⚠ THE ORDER OF THE TESTS IS THE PRIORITY, and the fill site guarantees at
// most one of these bits is ever set. The fallthrough is soft light, which is
// what every build had before this function existed, so an unknown or unset
// blend renders exactly as it always did.
//
// ⚠ SOFT LIGHT AND OVERLAY CANNOT MOVE PURE BLACK OR PURE WHITE; multiply and
// screen each move one end and not the other. That is a property of the
// formulas, documented on the enum, and it is the answer to a dye that will
// not reach a garment's darkest or brightest texels.
// ---- the NON-SEPARABLE pair's machinery ------------------------------------
//
// Straight out of the W3C compositing spec, which is Photoshop's own
// definition of Color and Luminosity. ⚠ THESE WEIGHTS ARE NOT Rec.709 AND THAT
// IS DELIBERATE: 0.30/0.59/0.11 is what the spec uses for these two modes, and
// the weights are part of the formula being named. Recolour and RampAt keep
// Rec.709 because they are not these modes.
float LumSpec(float3 c)
{
    return dot(c, float3(0.30, 0.59, 0.11));
}

// Pull a colour back inside the cube WITHOUT moving its luminance, by
// converging it on its own grey. A plain saturate would shift the luminance
// and defeat the whole point of setting it.
float3 ClipColour(float3 c)
{
    float l = LumSpec(c);
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    // ⚠ THE GUARDS ARE NOT DECORATION. At n == l (an achromatic colour) the
    // divisor is zero, and that is the ordinary case for a grey texel, not an
    // edge case.
    if (n < 0.0) {
        c = l + (c - l) * (l / max(l - n, 1e-5));
    }
    if (x > 1.0) {
        c = l + (c - l) * ((1.0 - l) / max(x - l, 1e-5));
    }
    return c;
}

float3 SetLum(float3 c, float l)
{
    return ClipColour(c + (l - LumSpec(c)));
}

float3 BlendCurve(float3 d, float3 t, uint flags)
{
    if ((flags & 2048u) != 0u) {
        return d * t;
    }
    if ((flags & 4096u) != 0u) {
        return 1.0 - (1.0 - d) * (1.0 - t);
    }
    if ((flags & 8192u) != 0u) {
        // Piecewise IN THE BASE, which is the whole difference from soft
        // light: step(0.5, d) is 1 where d >= 0.5, so the multiply arm takes
        // the dark half and the screen arm the light half.
        return lerp(2.0 * d * t, 1.0 - 2.0 * (1.0 - d) * (1.0 - t), step(0.5, d));
    }
    if ((flags & 16384u) != 0u) {
        // Colour: the dye's hue and saturation, the garment's luminance, kept
        // exactly. This is what Recolour's comment used to promise.
        return SetLum(t, LumSpec(d));
    }
    if ((flags & 32768u) != 0u) {
        // Luminosity: the garment's hue and saturation, the dye's luminance.
        // ⚠ lum(t) IS A CONSTANT HERE because the dye is one flat colour, so
        // every texel lands on one luminance and the garment's shading is
        // gone. Arithmetic, not a fault. The enum carries the reasoning.
        return SetLum(d, LumSpec(t));
    }
    return d * d + 2.0 * t * d * (1.0 - d);
}

// Two stops mapped over the source's own luminance.
//
// The hue travels across the mid tones rather than the extremes, which is what
// makes it read as pearl rather than as a gradient someone painted on: nacre is
// a pale body with the colour living where the light grazes. smoothstep is what
// keeps the ends clean, so a black crease stays black and a blown highlight
// stays white instead of both taking a tint no real pearl has.
//
// The source luminance still multiplies through at the end, exactly as the flat
// path does, so every crease, stitch and scratch the texture had survives.
//
// Two equal stops reduce to Recolour above, exactly: lerp returns stopA, m is
// its own max, and the result is lum * (stopA / m) term for term. That is what
// makes a dye authored with hex2 equal to hex indistinguishable from one
// authored without hex2 at all, in the shader as well as in the arithmetic.
//
// ⚠ THE WINDOW COMES IN RATHER THAN BEING BAKED, AND THAT IS THE FIX FOR THE
// FIRST FIELD RUN. Hardcoding 0.15 to 0.85 assumed a texture that uses most of
// the range. Measured on real content, the Abyss cubemap has mean luminance
// 0.306 and its diffuse PEAKS at 0.216, so t sat at zero and both targets
// painted stop A alone. The ramp was correct; the window was calibrated for a
// texture that does not exist. The two callers pass their own, from the INI.
// The ramp itself, once something has decided WHERE along it this texel sits.
//
// ⚠ THE POSITION IS A PARAMETER BECAUSE THE TWO MODES KEY ON DIFFERENT THINGS,
// and conflating them is what made the first field build read as a two-tone
// texture rather than a pearl. Nacre is a STATIC pattern, so it keys on the
// source's own luminance. Iridescence has to change with the VIEWING ANGLE, so
// it keys on the direction a cube texel represents. One ramp, two keys.
// ⚠ WHAT IT DIVIDES BY IS THE WHOLE DIFFERENCE BETWEEN A PEARL AND A TWO-TONE
// REPAINT, and the first three attempts all got it wrong in the same place.
//
// Recolour normalises the tint by its own max, and it has to: a gold cubemap has
// almost no blue in it, so multiplying by a blue dye gives near-black rather than
// blue metal. Dividing by max() throws the tint's BRIGHTNESS away and keeps only
// its hue, which is right when there is one tint.
//
// With two stops it is wrong. Dividing the blend by the BLEND's own max forces
// both stops to identical intensity, so all that ever varies is hue at constant
// brightness. Real nacre is a pale body with a BRIGHTER, more saturated sheen
// riding on it; normalising per-blend deletes precisely that and leaves a hue
// that slides around at flat intensity. Measured on the shipped pairs: Beetle
// Shell's stops differ 1.76x in max channel and every bit of that was discarded.
//
// So divide by a FIXED reference instead, stop A's max. Then stop B's brightness
// relative to stop A survives and the sheen end genuinely brightens.
//
// ⚠ AND t=0 IS STILL EXACTLY THE FLAT PATH. At t=0 the blend IS stop A, so this
// reduces to lum * stopA/max(stopA), term for term what Recolour produces. Two
// equal stops likewise stay flat at every t. Those two properties are what let
// this ship without changing any existing dye, and they are why the reference is
// stop A rather than something averaged.
//
// gSheen dials how much of that brightness difference survives: 0 reproduces the
// old fully-normalised behaviour, 1 lets it all through. A bright stop B can
// exceed 1 and clip, which is a real risk on a pair like Beetle Shell, so the
// knob exists to pull it back rather than to be admired.
// ---- travelling through HUE rather than through RGB ------------------------
//
// ⚠ THIS IS THE SHAPE ERROR UNDERNEATH ALL THE EARLIER ONES. A straight lerp
// between two stops produces exactly one blend: green to purple passes through
// grey-mauve and never through yellow or cyan. Nacre is thin-film interference,
// which sweeps THROUGH a sequence of hues, which is why a real pearl shows pink
// and green and gold at once. No choice of axis, window or normalisation can make
// a spectrum out of a straight line in RGB, so all four earlier attempts were
// tuning the position along a line that could not contain the answer.
//
// Going round the colour wheel instead puts the intermediate hues back. The LONG
// arc is the interesting one: green to purple the short way is a few degrees of
// mauve, the long way is green, yellow, orange, red, magenta, purple, which is
// the spectral cycle iridescence actually looks like.
float3 RgbToHsv(float3 c)
{
    float4 k = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = lerp(float4(c.bg, k.wz), float4(c.gb, k.xy), step(c.b, c.g));
    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}

float3 HsvToRgb(float3 c)
{
    float4 k = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(c.xxx + k.xyz) * 6.0 - k.www);
    return c.z * lerp(k.xxx, saturate(p - k.xxx), c.y);
}

// The blended colour at t, either straight through RGB or the long way round the
// hue wheel. Saturation and value still interpolate linearly in both cases: only
// the HUE takes the scenic route, because that is the part carrying the effect.
float3 BlendStops(float3 stopA, float3 stopB, float t, bool spectral)
{
    if (!spectral) {
        return lerp(stopA, stopB, t);
    }
    float3 a = RgbToHsv(stopA);
    float3 b = RgbToHsv(stopB);
    float dh = b.x - a.x;
    // The long arc, so the sweep passes through the hues BETWEEN the two stops
    // the other way round rather than the few degrees directly between them.
    dh = (dh > 0.0) ? dh - 1.0 : dh + 1.0;
    float h = frac(a.x + dh * t + 1.0);
    // ⚠ A GREY STOP HAS NO HUE TO TRAVEL FROM. RgbToHsv returns an arbitrary hue
    // at zero saturation, so a near-grey stop would spin the sweep off a
    // meaningless angle. Fall back to its partner's hue, which keeps a
    // pale-body-plus-coloured-sheen pair working rather than making it strobe.
    if (a.y < 1e-3) { h = frac(b.x + dh * (t - 1.0) + 1.0); }
    return HsvToRgb(float3(h, lerp(a.y, b.y, t), lerp(a.z, b.z, t)));
}

float3 RampAt(float3 src, float3 stopA, float3 stopB, float t, float sheen, bool spectral)
{
    float lum  = dot(src, float3(0.2126, 0.7152, 0.0722));
    float3 hue = BlendStops(stopA, stopB, t, spectral);
    float mA   = max(max(stopA.r, stopA.g), max(stopA.b, 1e-4));
    float mBlend = max(max(hue.r, hue.g), max(hue.b, 1e-4));
    // sheen 0 -> divide by the blend's own max (old, intensity-flat).
    // sheen 1 -> divide by stop A's max (the stops' brightness ratio survives).
    float mDiv = lerp(mBlend, mA, saturate(sheen));
    return saturate(lum * (hue / mDiv));
}

// NACRE's key: the source's own luminance, so the colour lives in the mid tones
// and every crease the texture had survives. A static look, which is what nacre
// on cloth is.
//
// max() so a window with hi <= lo cannot divide by zero inside smoothstep; it
// degenerates to a hard step at lo, which is a visible answer rather than NaN.
float RampTFromLuminance(float3 src, float lo, float hi)
{
    float lum = dot(src, float3(0.2126, 0.7152, 0.0722));
    return smoothstep(lo, max(hi, lo + 1e-4), lum);
}

// The window derived from the texture's OWN average, which is its smallest mip.
//
// ⚠ THIS IS WHAT MAKES NACRE VISIBLE ON EVERY ARMOUR INSTEAD OF ONE. A fixed
// absolute window is fitted to whichever albedo it was measured on: the Abyss
// metal peaks at luminance 0.216, so a window right for it sits entirely below
// a white linen and entirely above nothing - one texture gets the ramp, every
// differently-exposed one gets a flat colour. Scaling the window by the
// texture's own mean puts the mid-tones of ANY source inside it. The mean
// costs one Load of the 1x1 mip, cached after the first thread reads it.
float2 AutoWindow(float3 avgColour, float loMul, float hiMul)
{
    float m  = dot(avgColour, float3(0.2126, 0.7152, 0.0722));
    float lo = clamp(m * loMul, 0.0, 0.90);
    float hi = clamp(m * hiMul, lo + 0.02, 1.0);
    return float2(lo, hi);
}
)";

        constexpr char kShader[] = R"(
Texture2D<float4>   Src : register(t0);
// The iris mask, bound only when gFlags bit 7 says so. Its ALPHA says where
// the tint lands; see the masked branch in main for the arithmetic.
Texture2D<float4>   Msk : register(t1);
RWTexture2D<float4> Dst : register(u0);

cbuffer Params : register(b0)
{
    float3 gTint;
    uint   gFlags;   // bit 0: sRGB on write.  bit 1: recolour.  bit 2: run the ramp
    uint   gSrcMip;  // OS-140: which level of Src to read
    float3 gStopB;   // the second stop, in the bytes pad[3] was already rounding to
    float  gRampLo;  // the ramp's window, from the INI
    float  gRampHi;
    float  gRampAxis;  // 0 elevation, 1 azimuth, 2 luminance (cube only)
    float  gSheen;     // 0 = intensity-flat (old), 1 = the stops' brightness ratio survives
    float  gIrisRadius;  // the derived disc: fraction of the marked region's short side
    float  gIrisSoft;    // its feather, either side of that radius
    float  gMaskBroad;   // coverage above which a mask is a region, not an iris
};

// What the analysis pass worked out about the mask: xy the marked region's
// centre in uv, z the derived iris radius in uv, w non-zero when the mask was
// too broad to be an iris and this disc is what the tint should use instead.
// Zeroed (and so ignored) on every build that runs no analysis.
Buffer<float4> Disc : register(t2);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    Dst.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) { return; }

    float4 d = Src.Load(int3(id.xy, gSrcMip & 0xFFFFu));
    float3 o;
    if ((gFlags & 32u) != 0u) {
        // FLAKE ON THE NORMAL MAP, the half of sparkle that twinkles. Each
        // flake cell tilts the tangent-space normal a random amount, making a
        // micro-facet whose specular highlight catches the light at its own
        // angle. The engine then does the twinkling per frame for free: move
        // the camera and different facets align with the light. Non-flake
        // texels pass through byte-exact, so the garment's own surface detail
        // is untouched between the glints.
        uint2 cell = id.xy >> 1;
        uint  h    = cell.x * 374761393u + cell.y * 668265263u;
        h          = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        float intensity = gTint.x;
        float isFlake   = step(1.0 - (0.02 + 0.06 * intensity), float(h & 0xFFFFu) / 65535.0);
        o = d.rgb;
        if (isFlake > 0.5) {
            float2 tilt = float2(float((h >> 4) & 0xFFu), float((h >> 12) & 0xFFu)) / 255.0;
            float3 n    = d.rgb * 2.0 - 1.0;
            n.xy += (tilt - 0.5) * (1.2 * intensity);
            n = normalize(n);
            o = n * 0.5 + 0.5;
        }
        Dst[id.xy] = float4(o, d.a);  // alpha is the spec mask; never touched
        return;
    }
    if ((gFlags & 16u) != 0u) {
        // FLAKE, on a STRENGTH map. The mask says per texel how reflective the
        // surface is, so sparse full-strength speckles over a dimmed body break
        // the reflection into discrete glints: the car-paint flake layer, done
        // with the one texture that controls per-texel reflectivity. gTint.x is
        // the intensity; hue never enters, because the mask has none to carry.
        //
        // 2x2-texel cells rather than single texels, so a flake survives being
        // seen at less than 1:1 and the pattern reads as glitter rather than as
        // noise. Only mip 0 runs this pass; GenerateMips averages it down the
        // chain, which softens distant flakes exactly the way distance should.
        uint2 cell = id.xy >> 1;
        uint  h    = cell.x * 374761393u + cell.y * 668265263u;
        h          = (h ^ (h >> 13)) * 1274126177u;
        h ^= h >> 16;
        float r         = float(h & 0xFFFFu) / 65535.0;
        float intensity = gTint.x;
        float isFlake   = step(1.0 - (0.02 + 0.06 * intensity), r);
        float body      = 1.0 - 0.45 * intensity;
        o = min(d.rgb * lerp(body, 1.6, isFlake), 1.0);
    } else {
        // ---- the PER-EYE SPLIT, bit 10 -----------------------------------
        //
        // On a split eye set the two eyes read disjoint halves of this texture
        // (left eye low u, right eye high u; the census doc carries the
        // measurements), so a per-eye colour is a per-HALF colour. gStopB is
        // the right eye's tint here, free for the reason the sclera already
        // borrows it on masked builds: a split set's mask is flat, the sclera
        // cannot land, and the fill site never sets both.
        float3 tint = gTint;
        if ((gFlags & 1024u) != 0u && float(id.x) + 0.5 >= 0.5 * float(w)) {
            tint = gStopB;
        }
        if ((gFlags & 2u) != 0u) {
            // Bit 6: the window scales off this texture's own average.
            // gRampLo/Hi arrive as MULTIPLIERS in that mode; the fill site
            // chose which pair to send, so the shader never has to know which
            // INI key it is reading.
            float lo = gRampLo;
            float hi = gRampHi;
            if ((gFlags & 64u) != 0u) {
                float2 w2 = AutoWindow(Src.Load(int3(0, 0, gSrcMip >> 16)).rgb, gRampLo,
                                       gRampHi);
                lo = w2.x;
                hi = w2.y;
            }
            o = ((gFlags & 4u) != 0u)
                    ? RampAt(d.rgb, tint, gStopB,
                             RampTFromLuminance(d.rgb, lo, hi), gSheen,
                             (gFlags & 8u) != 0u)
                    : Recolour(d.rgb, tint);
        } else {
            o = BlendCurve(d.rgb, tint, gFlags);
        }
    }
    // ---- the IRIS MASK, bit 7 ------------------------------------------
    //
    // Lerp each texel between the untouched source and the tinted value by
    // the mask's alpha, so an eye's sclera keeps its own pink and veins while
    // the iris takes the dye, with the author's soft edge feathering for free.
    //
    // ⚠ NORMALISED AGAINST THE MASK'S OWN MEAN, NOT READ RAW, and that is
    // measured rather than cautious. The ILV eye's mask is a disc at alpha
    // 0.30-0.34 over a floor of 0.05-0.09: these are SPECULAR INTENSITIES an
    // author picked for the highlight, not a 0-to-1 stencil, and raw alpha
    // would dye the iris at a third strength and the sclera at 7%. The window
    // scales off the mask's own mean (its smallest mip), exactly the
    // AutoWindow trick above: mean 0.073 with the shipped multipliers puts
    // the window at 0.11..0.18, under the disc and over the floor.
    //
    // ⚠ EVERY UNUSABLE MASK FALLS BACK TO t=1, THE UNMASKED BUILD, NEVER TO
    // t=0. A mask that silently ate the dye would read as a dead picker.
    // Unusable means: too few mips to carry a real mean (the AutoWindow rule),
    // or a mean above half, which is a flat "everything is shiny" alpha and
    // not an iris disc (the iris is a small part of any eye texture).
    //
    // gRampLo/gRampHi arrive as the MASK WINDOW'S multipliers on a masked
    // build; a masked request is always flat, so the ramp cannot want them
    // (same fill-site discipline as the auto window's bit 6).
    //
    // ⚠ TWO SIDES, TWO OPTIONAL TINTS. Bit 9 says "no iris tint": inside the
    // disc keeps the source (the request's tint is a canonical black the key
    // discipline requires; it is never read here). Bit 8 says the SCLERA
    // takes gStopB, through the same arithmetic the request's own blend uses,
    // so a recolour eye gets a recolour sclera. gStopB is free on a masked
    // build for the reason the window scalars are: the ramp is forbidden.
    //
    // ⚠ THE FALLBACK DIRECTION SPLITS WITH THE SIDES, and that is deliberate.
    // t=1 on an unusable mask keeps the iris tint covering the whole eye (the
    // behaviour that shipped) and makes the sclera tint paint NOTHING: with
    // no disc there is no way to say where the white is, and painting the
    // whole eye sclera-colour would be the original defect inverted.
    if ((gFlags & 128u) != 0u) {
        uint mw, mh, mlv;
        Msk.GetDimensions(0u, mw, mh, mlv);
        // The mask and the destination may differ in size (the ILV mask is
        // 2048 over a 1024 diffuse capped to 512), so read the mask mip
        // nearest the destination's texel density at the same uv.
        float lodf = max(0.0, log2(max(float(mw) / float(w), 1.0)));
        uint  mmip = min(uint(lodf + 0.5), mlv - 1u);
        uint2 msz  = uint2(max(mw >> mmip, 1u), max(mh >> mmip, 1u));
        float2 uv  = (float2(id.xy) + 0.5) / float2(w, h);
        int2   mxy = int2(min(uint2(uv * float2(msz)), msz - 1u));
        float maskA = Msk.Load(int3(mxy, mmip)).a;
        float meanA = Msk.Load(int3(0, 0, mlv - 1u)).a;
        float t = (mlv < 4u || meanA > 0.5)
                      ? 1.0
                      : smoothstep(meanA * gRampLo,
                                   max(meanA * gRampHi, meanA * gRampLo + 1e-4),
                                   maskA);
        // ⚠⚠ THE MASK MAY MARK THE EYE RATHER THAN THE IRIS, and on the eye
        // sets that do, everything above is correct arithmetic on the wrong
        // question: it feathers a disc that covers the entire eyeball, so a
        // green iris colour paints a green eye. The analysis pass measures the
        // marked area and hands back a disc at its centre; when it says the
        // mask was that broad, the disc replaces the alpha entirely rather than
        // multiplying it, because the alpha inside the region carries no
        // information about where the iris is.
        float4 disc = Disc.Load(0);
        if (disc.w > 0.5) {
            float d2 = length(uv - disc.xy);
            float r  = max(disc.z, 1e-4);
            t = 1.0 - smoothstep(r * (1.0 - gIrisSoft),
                                 max(r * (1.0 + gIrisSoft), r * (1.0 - gIrisSoft) + 1e-4),
                                 d2);
        }
        float3 inside  = ((gFlags & 512u) != 0u) ? d.rgb : o;
        float3 outside = d.rgb;
        if ((gFlags & 256u) != 0u) {
            // ⚠ THE SAME CURVE THE IRIS TOOK, so a screen-blended eye gets a
            // screen-blended sclera and the two halves of one eye cannot be
            // built by two different arithmetics.
            outside = ((gFlags & 2u) != 0u) ? Recolour(d.rgb, gStopB)
                                            : BlendCurve(d.rgb, gStopB, gFlags);
        }
        o = lerp(outside, inside, t);
    }
    if ((gFlags & 1u) != 0u) {
        float3 lo = o * 12.92;
        float3 hi = 1.055 * pow(max(o, 1e-5), 1.0 / 2.4) - 0.055;
        o = lerp(hi, lo, step(o, 0.0031308));
    }
    Dst[id.xy] = float4(o, d.a);
}
)";

        // ---- the mask analysis pass -----------------------------------------
        //
        // ⚠⚠ IT EXISTS BECAUSE "THE NORMAL MAP'S ALPHA IS THE IRIS" IS TRUE OF
        // SOME EYE SETS AND FALSE OF THE REST, and the false half is what the
        // field saw as "the iris colour dyes the whole eye" (2026-08-16, 3BA and
        // HIMBO characters). Measured: UBE's ILV mask is a genuine disc (mean
        // alpha 0.073, peak 0.34, 7% coverage) while vanilla's eyebrown_n.dds is
        // a hard 0/255 mask covering the whole bottom half of the texture, which
        // is the eye AREA of a vanilla eye layout. One assumption, two authoring
        // conventions.
        //
        // ⚠ ONE THREAD, ON PURPOSE. The pass runs once per texture BUILD, not
        // per frame, and it reads a coarse mip of at most 64 on the long side,
        // so the whole reduction is a few thousand loads. A group-shared
        // reduction would be faster and would need a barrier discipline that
        // buys nothing at this size.
        //
        // ⚠ THE THRESHOLD IS THE MASK'S OWN MEAN, matching the tint pass's
        // window rather than inventing a second idea of "covered". On the ILV
        // disc that selects the disc; on the vanilla mask it selects the marked
        // half; on a flat mask it selects nothing and the coverage reads as
        // zero, which is the "leave it alone" answer.
        constexpr char kMaskShader[] = R"(
Texture2D<float4> Msk : register(t0);
RWBuffer<float4>  Out : register(u0);

cbuffer Params : register(b0)
{
    float3 gTint;
    uint   gFlags;
    uint   gSrcMip;
    float3 gStopB;
    float  gRampLo;
    float  gRampHi;
    float  gRampAxis;
    float  gSheen;
    float  gIrisRadius;
    float  gIrisSoft;
    float  gMaskBroad;
};

[numthreads(1, 1, 1)]
void main()
{
    uint w, h, lv;
    Msk.GetDimensions(0u, w, h, lv);
    Out[0] = float4(0.0, 0.0, 0.0, 0.0);
    if (lv < 4u) { return; }               // no mip chain, no mean to read

    uint mip = 0u;
    while (mip + 1u < lv && max(w >> mip, h >> mip) > 64u) { mip += 1u; }
    uint2 sz = uint2(max(w >> mip, 1u), max(h >> mip, 1u));
    float meanA = Msk.Load(int3(0, 0, lv - 1u)).a;

    uint  covered = 0u;
    uint4 box = uint4(sz.x, sz.y, 0u, 0u);   // minX minY maxX maxY
    for (uint y = 0u; y < sz.y; y += 1u) {
        for (uint x = 0u; x < sz.x; x += 1u) {
            if (Msk.Load(int3(int(x), int(y), int(mip))).a > meanA) {
                covered += 1u;
                box.x = min(box.x, x); box.y = min(box.y, y);
                box.z = max(box.z, x); box.w = max(box.w, y);
            }
        }
    }
    if (covered == 0u) { return; }
    float coverage = float(covered) / float(sz.x * sz.y);
    if (coverage <= gMaskBroad) { return; }  // already an iris; the alpha is used as-is

    float2 lo = (float2(box.xy)) / float2(sz);
    float2 hi = (float2(box.zw) + 1.0) / float2(sz);
    float2 centre = (lo + hi) * 0.5;
    float  side   = min(hi.x - lo.x, hi.y - lo.y);
    Out[0] = float4(centre, side * gIrisRadius, 1.0);
}
)";

        // ⚠ A SECOND SHADER RATHER THAN A FLAG, BECAUSE THE BINDING TYPES
        // DIFFER. A cubemap is six array slices, and a Texture2D cannot be bound
        // where a Texture2DArray is declared. Everything else about the pass is
        // the same, which is why the arithmetic lives in kCommon and only the
        // declarations and the face index change here.
        constexpr char kCubeShader[] = R"(
Texture2DArray<float4>   Src : register(t0);
RWTexture2DArray<float4> Dst : register(u0);

cbuffer Params : register(b0)
{
    float3 gTint;
    uint   gFlags;
    uint   gSrcMip;
    float3 gStopB;
    float  gRampLo;
    float  gRampHi;
    float  gRampAxis;
    float  gSheen;  // 0 = intensity-flat (old), 1 = the stops' brightness ratio survives
    // ⚠ DECLARED HERE TOO AND UNREAD, because this cbuffer must MATCH the C++
    // Params byte for byte. A cube build never masks (the funnel refuses it),
    // so nothing here has a use for them; leaving them out would make every
    // field after gSheen read the next one's bytes.
    float  gIrisRadius;
    float  gIrisSoft;
    float  gMaskBroad;
};

// Which DIRECTION a cube texel stands for. Standard D3D cube face layout:
// +X, -X, +Y, -Y, +Z, -Z in slice order.
float3 CubeTexelDir(uint3 id, uint w, uint h)
{
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h) * 2.0 - 1.0;
    float u = uv.x;
    float v = uv.y;
    if (id.z == 0) { return normalize(float3( 1.0,   -v,   -u)); }
    if (id.z == 1) { return normalize(float3(-1.0,   -v,    u)); }
    if (id.z == 2) { return normalize(float3(   u,  1.0,    v)); }
    if (id.z == 3) { return normalize(float3(   u, -1.0,   -v)); }
    if (id.z == 4) { return normalize(float3(   u,   -v,  1.0)); }
    return normalize(float3(-u, -v, -1.0));
}

// IRIDESCENCE's key: where this texel sits around the horizontal sweep.
//
// ⚠ THIS IS THE MECHANISM THE SPEC GOT WRONG, AND THE REASON THE FIRST BUILD
// LOOKED LIKE MARBLING RATHER THAN PEARL. The spec keyed hue on cube texel
// LUMINANCE, arguing that orbiting sweeps the reflection vector across texels of
// differing brightness. It does sweep, but a cubemap's brightness describes the
// ENVIRONMENT (bright sky, dark ground), not the angle between eye and surface,
// so the hue pattern was fixed in world space and merely slid across the piece
// as the camera moved. Measured: mean luminance 0.306 with std 0.157, which is
// neither angular nor evenly spread.
//
// The direction a texel represents IS angular. The engine samples this cube by
// reflect(-V, N), so as V rotates the sampled direction rotates with it, and a
// hue that varies smoothly with direction becomes a hue that varies with viewing
// angle. That is goniochromism, which is what pearlescence actually is.
//
// ⚠ WHICH ANGLE, CHOSEN AT RUNTIME, BECAUSE THIS IS AN ART QUESTION AND EACH
// ANSWER COST A REBUILD. Luminance was the spec's key and read as marbling.
// Azimuth was the first correction and still did not read as pearl. Elevation is
// the default now. Selectable so the next attempt is an INI edit and a restart
// rather than another round trip.
//
//   0 = ELEVATION. dir.y straight, already -1 to 1 on a unit vector and with no
//       wrap anywhere: the poles are the endpoints. On a curved piece this puts a
//       smooth band across it, the top reflecting one stop and the underside the
//       other, and the band sweeps as you move. This is the shot-silk read.
//   1 = AZIMUTH, as cos rather than atan2. atan2 wraps at +-pi and the
//       discontinuity draws a hard line down the reflection; dir.x over the
//       length of dir.xz is exactly cos of that angle, smooth and periodic, so a
//       full orbit travels A to B and back with no edge.
float RampTFromDirection(float3 dir, float lo, float hi, float axis)
{
    float k = (axis < 0.5) ? dir.y
                           : dir.x * rsqrt(max(dot(dir.xz, dir.xz), 1e-6));
    return smoothstep(lo, max(hi, lo + 1e-4), saturate(k * 0.5 + 0.5));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h, faces;
    Dst.GetDimensions(w, h, faces);
    if (id.x >= w || id.y >= h || id.z >= faces) { return; }

    float4 d = Src.Load(int4(id.xy, id.z, gSrcMip & 0xFFFFu));
    // ⚠ THE HUE KEYS ON DIRECTION AND THE BRIGHTNESS STILL COMES FROM THE SOURCE.
    // RampAt multiplies the source luminance back through, so every highlight and
    // every shape in the reflection survives exactly as the flat recolour keeps
    // them; only which HUE each direction resolves to is new. That split is what
    // makes this read as a material rather than as a painted-on gradient.
    //
    // The cubemap is the only thing in the envmap path carrying colour, so this
    // one branch is the whole of the effect: no new shader family, no material
    // surgery, no texture beyond the cubemap this path already recolours.
    //
    // No overlay branch here because there never was one: a cubemap is always
    // kRecolour, for the reason the Blend enum records.
    // axis 2 falls back to the spec's original luminance key, kept so the two can
    // be compared side by side without a rebuild rather than argued about. The
    // auto window (bit 6) only ever accompanies the LUMINANCE key: the direction
    // keys' window is an angle, and scaling an angle by a brightness is nonsense
    // the fill site refuses to emit.
    float lo = gRampLo;
    float hi = gRampHi;
    if ((gFlags & 64u) != 0u) {
        float2 aw = AutoWindow(Src.Load(int4(0, 0, 0, gSrcMip >> 16)).rgb, gRampLo, gRampHi);
        lo = aw.x;
        hi = aw.y;
    }
    float t = (gRampAxis >= 1.5)
                  ? RampTFromLuminance(d.rgb, lo, hi)
                  : RampTFromDirection(CubeTexelDir(id, w, h), lo, hi, gRampAxis);
    float3 o = ((gFlags & 4u) != 0u)
                   ? RampAt(d.rgb, gTint, gStopB, t, gSheen, (gFlags & 8u) != 0u)
                   : Recolour(d.rgb, gTint);
    if ((gFlags & 1u) != 0u) {
        float3 lo = o * 12.92;
        float3 hi = 1.055 * pow(max(o, 1e-5), 1.0 / 2.4) - 0.055;
        o = lerp(hi, lo, step(o, 0.0031308));
    }
    Dst[id] = float4(o, d.a);
}
)";

        struct Params {
            float         tint[3]{ 0.5f, 0.5f, 0.5f };
            std::uint32_t flags{ 0 };  // bit 0 sRGB, bit 1 hue blend
            // ⚠ OS-140. Which level of the SOURCE to read. A capped build is a
            // read of a smaller mip and nothing else, which is why it costs less
            // GPU work as well as less memory and needs no downsample pass of
            // its own.
            std::uint32_t srcMip{ 0 };
            // ⚠ THE OLD pad[3] SPENT, NOT WIDENED. A float3 is exactly the 12
            // bytes the buffer was already rounding up to, so the second stop
            // arrives for free: no new constant register, no change to
            // sizeof(Params), and the static_assert below is unchanged evidence
            // of that rather than a number somebody had to update. If it fires,
            // this member is the wrong type; do not move the number.
            //
            // 0.5 rather than 0 so an uninitialised ramp is mid grey, matching
            // tint above. A stop of black would darken every texel of a build
            // that reached the ramp branch without a stop, which is a picture
            // somebody would chase; grey is the shader's own no-change value.
            float stopB[3]{ 0.5f, 0.5f, 0.5f };
            // ⚠ THE RAMP'S LUMINANCE WINDOW, AND THIS IS WHAT MOVED THE ASSERT
            // BELOW FROM 32 TO 48. Unlike stopB above, which spent padding that
            // was already being rounded away, these are genuinely new data and a
            // third constant register is what they cost. The number moving is
            // correct here; what must never happen is the number moving because
            // somebody mistyped a member, which is why the note on stopB says so.
            //
            // Defaults match the shipped INI defaults so a build that somehow
            // reached the GPU without Settings still ramps rather than painting
            // one colour.
            float rampLo{ 0.15f };
            float rampHi{ 0.85f };
            // Which angle the REFLECTION's hue keys on: 0 elevation, 1 azimuth,
            // 2 the spec's original luminance. A float rather than a uint purely
            // so it lands in the same register without a packing rule to remember.
            // This is the slack the note below said the next scalar would take.
            float rampAxis{ 0.0f };
            // How much of the two stops' own brightness difference survives the
            // normalise. This is the last float the 48-byte buffer had spare, so
            // the assert below still does not move; anything further needs a
            // fourth register and should say so out loud.
            float sheen{ 1.0f };
            // ⚠ THE FOURTH REGISTER, AND THE NOTE ABOVE ASKED WHOEVER SPENT IT
            // TO SAY SO. These three are the derived iris disc: how big it is
            // as a fraction of the marked region, how far it feathers, and how
            // much coverage makes a mask a region rather than an iris. They are
            // read by the analysis pass and by the 2D tint pass; the cube pass
            // declares them and reads none, because a cube build never masks.
            //
            // Defaults match the shipped INI defaults, on the same reasoning as
            // the ramp window above.
            float irisRadius{ 0.24f };
            float irisSoft{ 0.25f };
            float maskBroad{ 0.25f };
            // ⚠ A CONSTANT BUFFER IS ALLOCATED IN WHOLE 16-BYTE REGISTERS, and
            // CreateBuffer refuses a width that is not a multiple of one. The
            // three floats above spend three quarters of the fourth register;
            // this is the quarter nobody has a use for yet, named so the next
            // scalar takes it rather than adding a fifth register by accident.
            float pad4{ 0.0f };
        };
        static_assert(sizeof(Params) == 64);  // four constant registers

        struct Request {
            // ⚠ THE SLOT IS THE KEY WITHOUT THE COLOUR, AND IT IS WHAT MAKES A
            // DRAG SURVIVABLE. Dragging the colour picker posts an edit per
            // frame, so every intermediate colour used to enqueue its own build
            // of every texture on the character. The queue then drained four per
            // frame while the drag added far more, which is why the reflection
            // only caught up once the cursor stopped, and why 92 textures and
            // 6 GB were resident after a few minutes of picking. At most ONE
            // pending build per texture per blend now, always the newest colour:
            // a drag costs what its final colour costs.
            //
            // ⚠ OS-140: THE QUALITY IS IN THE SLOT TOO. A queued commit and the
            // next frame's preview of the same texture are two different pending
            // builds, so the coalescing above must not collapse them into one.
            std::string                        slot;   // path|blend|quality
            std::string                        owner;  // path|blend, the SlotState
            std::string                        key;
            DyeQuality::Quality                quality{ DyeQuality::Quality::kPreview };
            RE::NiPointer<RE::NiSourceTexture> source;  // held across the hop
            // The iris mask, held across the hop exactly as the source is.
            // Null for an unmasked build, which is every build before eyes.
            RE::NiPointer<RE::NiSourceTexture> mask;
            // Which side of the disc takes colour; only read when mask is set.
            MaskDye                            maskDye;
            RE::NiColor                        tint;
            // Beside the tint because it is part of the same answer: what colour
            // this texture is being made. The key already carries it, so the
            // coalescing above compares it for free; this is what the BUILD reads.
            Ramp                               ramp;
            Blend                              blend{ Blend::kSoftLight };
            // The caller's own ceiling, read by Build. It is in this request's
            // own key as well, so a capped and an uncapped build of one texture
            // are two entries and neither can be served for the other.
            std::uint32_t                      capPx{ 0 };
            // ⚠ EVERY actor that asked, not just the first. Two followers and
            // the player can all be waiting on one shared atlas, and repainting
            // only whoever happened to miss first would leave the others showing
            // undyed armour with nothing left to bring them back.
            std::vector<RE::ActorHandle>       waiters;
        };

        // ⚠ NEVER DESTROYED, the same way OutfitDye leaks g_swapped and for the
        // same reason. These hold NiPointers into engine objects and D3D
        // resources; running their destructors at DLL_PROCESS_DETACH would
        // DecRef into a renderer and a heap that are already gone.
        struct Entry {
            RE::NiPointer<RE::NiSourceTexture> tex;
            std::uint64_t                      bytes{ 0 };
        };
        auto& g_cache = *new std::unordered_map<std::string, Entry>();
        // Insertion order, for evicting the oldest first.
        auto& g_order = *new std::vector<std::string>();
        // Builds currently inside Pump, so a walk on the game thread cannot
        // enqueue a second copy of one already being made.
        auto& g_inFlight = *new std::unordered_set<std::string>();
        auto& g_pending = *new std::vector<Request>();
        // Sources the layout control rejected and builds that could not
        // allocate. Both are permanent: retrying either every repaint would
        // spin a failure into a log flood.
        auto& g_refused = *new std::unordered_set<std::string>();

        // ---- what one TEXTURE's colour is doing right now -------------------
        //
        // ⚠ THIS IS THE WHOLE OF "WHAT COUNTS AS SETTLED". Acquire stamps the
        // clock when a texture's colour CHANGES rather than when it is asked
        // for, and the difference matters because Acquire runs on every repaint
        // while a repaint does not mean the player moved anything. The sweep in
        // Pump reads the clock and promotes. Nothing about the editor is
        // involved, so a scheme apply and a paste settle exactly like a drag.
        //
        // Bounded by distinct texture paths times blends, which was 6 on one
        // dressed character. Never pruned and it does not need to be.
        struct SlotState {
            std::string                        colour;  // path|RRGGBB|blend|ramp, no quality
            std::uint64_t                      changedAtMs{ 0 };
            RE::NiPointer<RE::NiSourceTexture> source;
            // The iris mask lives here for the same reason the ramp and the
            // cap below do: PromoteLocked builds the COMMIT request out of
            // this struct and nothing else, so a mask kept only on the
            // preview's request would dye the iris correctly at 512 and then
            // paint the whole eyeball the moment the settle sweep promoted.
            RE::NiPointer<RE::NiSourceTexture> mask;
            MaskDye                            maskDye;
            RE::NiColor                        tint;
            // ⚠ THE RAMP HAS TO LIVE HERE TOO, AND FORGETTING IT IS A DELAYED
            // FAULT RATHER THAN A VISIBLE ONE. PromoteLocked builds the COMMIT
            // request out of this struct and nothing else, so a ramp kept only on
            // the preview's request would paint a special dye correctly at 512
            // and then silently lose its second stop the instant the sharper
            // build landed and the material re-pointed at it. Right for a second,
            // wrong afterwards, and cached that way.
            Ramp                               ramp;
            Blend                              blend{ Blend::kSoftLight };
            // The caller's own ceiling, carried for the same reason the ramp
            // above is: PromoteLocked builds the COMMIT request out of this
            // struct and nothing else, so a cap kept only on the preview's
            // request would hold an eye at 512 and then quietly hand it an 87
            // MiB twin the moment the settle sweep promoted it.
            std::uint32_t                      capPx{ 0 };
            std::vector<RE::ActorHandle>       waiters;
            bool                               commitQueued{ false };
            // Every request for this colour so far was a hover preview's, so
            // the settle sweep must not promote it (spec 2026-08-09). Sticky
            // in the safe direction: one real request makes the colour
            // upgradeable for ever; a later hover cannot take that back.
            bool                               previewOnly{ false };
        };
        auto& g_slots = *new std::unordered_map<std::string, SlotState>();

        // Previews whose commit has been built. Swept every pump and freed as
        // soon as nothing is rendering from them any more.
        auto& g_retired = *new std::vector<std::string>();

        std::mutex  g_lock;
        Stats       g_stats;
        std::uint64_t g_bytes{ 0 };

        ID3D11ComputeShader* g_cs{ nullptr };
        ID3D11ComputeShader* g_csCube{ nullptr };
        ID3D11Buffer*        g_cb{ nullptr };
        bool                 g_compileFailed{ false };

        // The mask analysis pass and the four floats it hands to the tint pass.
        // One buffer for the whole process: the passes run back to back on the
        // render thread and nothing outlives a build, so there is nothing here
        // to key per texture.
        ID3D11ComputeShader*       g_csMask{ nullptr };
        ID3D11Buffer*              g_disc{ nullptr };
        ID3D11UnorderedAccessView* g_discUav{ nullptr };
        ID3D11ShaderResourceView*  g_discSrv{ nullptr };
        // ⚠ READ BACK RATHER THAN INFERRED. The verdict is computed on the GPU
        // and the log is the only place a field report can be answered from, so
        // 16 bytes come home per build and the line says what was decided.
        ID3D11Buffer*              g_discStaging{ nullptr };

        // ---- WHICH THREAD IS THE RENDER THREAD, AND IS IT THE GAME'S? -------
        //
        // ⚠ THIS IS A MEASUREMENT THE UNIT HAS BEEN ASSUMING SINCE TASK 1 AND
        // NOBODY HAS TAKEN. DyeGpu.h states that the immediate context may only
        // be touched from the render thread, and every design since has been
        // shaped by treating that as a different thread from the game's. It is
        // an inference from "D3D11 immediate contexts are not thread safe", not
        // an observation: nothing has ever compared the two ids.
        //
        // It matters now because the pump cannot reach a closed editor. FUCK
        // gates Draw AND RenderOverlay on the window being open, measured
        // 2026-08-05: a save loaded with the editor shut queues every request
        // correctly and no build ever runs. The fix is either a Present hook of
        // our own or an always-open invisible window, and the second is already
        // a documented hazard on this project - an alpha-0 window still hit
        // tests.
        //
        // ⚠ IF THESE TWO IDS MATCH, BOTH OF THOSE ARE UNNECESSARY. The pump
        // could be queued through the task interface like any other game-thread
        // work and the whole problem disappears. If they differ, the Present
        // hook is the answer and this line says so plainly rather than leaving
        // it to be guessed at again.
        std::atomic<unsigned long> g_renderTid{ 0 };
        std::atomic<unsigned long> g_gameTid{ 0 };

        void ReportThreads(const char* a_who, std::atomic<unsigned long>& a_slot) {
            const auto tid = ::GetCurrentThreadId();
            unsigned long expected = 0;
            if (!a_slot.compare_exchange_strong(expected, tid)) {
                return;  // already reported for this side
            }
            const auto other = (&a_slot == &g_renderTid ? g_gameTid : g_renderTid)
                                   .load(std::memory_order_relaxed);
            spdlog::info("DyeTexture: {} thread is {}.", a_who, tid);
            if (other != 0) {
                spdlog::info("DyeTexture: ===== render and game threads are {} (render={} "
                             "game={}). The pump {} be queued as an ordinary task. =====",
                             g_renderTid.load() == g_gameTid.load() ? "THE SAME" : "DIFFERENT",
                             g_renderTid.load(), g_gameTid.load(),
                             g_renderTid.load() == g_gameTid.load() ? "CAN" : "CANNOT");
            }
        }

        // ---- the render-thread tick, and why it is a hook ------------------
        //
        // ⚠ THE MEASUREMENT ABOVE IS WHY THIS EXISTS RATHER THAN AN AddTask.
        // Render 609800 against game 611052 on 2026-08-05: they really are two
        // threads, so the immediate context genuinely cannot be reached from the
        // task interface and the constraint this unit has carried since task 1
        // is real rather than assumed.
        //
        // ⚠ AND FUCK'S DRAW IS NOT ENOUGH, WHICH IS THE OTHER HALF. Draw and
        // RenderOverlay are both gated on the window being open, so a save
        // loaded with the editor shut queues every request and builds none. The
        // only tick that runs regardless of the UI is Present itself.
        //
        // ⚠ A VTABLE PATCH, NOT A CODE HOOK, AND THAT IS WHAT MAKES CHAINING
        // SAFE HERE. The hazards in `chained-hook-pitfalls` are about consuming
        // registers captured by somebody else's stub. This swaps one function
        // pointer in a COM vtable and calls whatever was there before, so FUCK's
        // own Present patch and this one compose whichever order they land in.
        // The write is a single aligned pointer store, which is atomic on x64,
        // so it cannot tear under a render thread calling through it.
        //
        // ⚠ NEVER UNPATCHED. Restoring the original at process exit would race
        // a render thread that may still be inside the thunk, and the DLL is
        // not unloaded before the process dies anyway.
        using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
        PresentFn        g_originalPresent{ nullptr };
        std::atomic<bool> g_hookInstalled{ false };

        HRESULT __stdcall PresentThunk(IDXGISwapChain* a_chain, UINT a_sync, UINT a_flags) {
            // The preview grid's frame clock. One relaxed increment; see
            // PreviewFrame.h for why it lives here and not in Draw.
            OS::PreviewFrame::BeginPresentFrame();
            // ⚠ THE PUMP RUNS BEFORE THE FRAME IS PRESENTED AND IT MUST NOT
            // THROW. Everything it does is bounded: it returns immediately on an
            // empty queue, which is every frame outside a dye apply, and builds
            // at most a few textures otherwise.
            Pump();
            // The preview grid's pump rides the same thunk for the same
            // reason (the immediate context), plus one of its own: run from
            // Draw, its offscreen render and FUCK image traffic sat INSIDE
            // FUCK's UI pass and the whole editor vanished on exactly the
            // loading frames (field 2026-08-09). Out here it sits on one
            // side of FUCK's entire present, whichever side this hook
            // chained onto, and PruneSelection's one-present tolerance is
            // what makes either ordering correct.
            OS::EditorWindow::PumpPreviews();
            // ⛔ THE PORTRAIT PUMP IS PARKED (user's call, 2026-08-23). It
            // rode this thunk for the immediate context and to keep FUCK
            // image traffic outside the UI pass, and it will need both again
            // if it ever comes back - but every source it can reach at this
            // point in the chain reads black on a Community Shaders plus
            // upscaler rig, and a per-present mutex probe for a feature that
            // cannot produce a picture is not worth a frame. Restore point:
            // git tag `portrait-capture-parked`.
            // OS-206: the requip transition's clock. It rides this thunk rather
            // than ImGuiOverlay's because that one is installed on the first
            // editor OPEN and the flourish has to run with no menu up at all.
            //
            // ⚠ IT MUST NOT THROW AND IT MUST BE CHEAP WHEN IDLE, on Pump's
            // terms: one atomic load and one compare on every frame the player
            // is not swapping an outfit, which is nearly all of them.
            OS::Requip::Tick();
            return g_originalPresent(a_chain, a_sync, a_flags);
        }

        // Installed lazily, from the first request. Costs nothing while rung 3
        // is off, and by the time an actor is being dyed the renderer is up.
        void EnsurePresentHook() {
            if (g_hookInstalled.load(std::memory_order_acquire)) {
                return;
            }
            auto* const rm = RE::BSRenderManager::GetSingleton();
            if (!rm) {
                return;
            }
            auto* const chain = rm->GetRuntimeData().swapChain;
            if (!chain) {
                return;
            }
            if (g_hookInstalled.exchange(true, std::memory_order_acq_rel)) {
                return;  // another thread won the race
            }

            // Slot 8 of IDXGISwapChain: IUnknown 0-2, IDXGIObject 3-6,
            // IDXGIDeviceSubObject 7, then Present.
            auto** const vtbl = *reinterpret_cast<void***>(chain);
            constexpr std::size_t kPresentSlot = 8;

            DWORD old{ 0 };
            if (!::VirtualProtect(&vtbl[kPresentSlot], sizeof(void*), PAGE_READWRITE, &old)) {
                spdlog::error("DyeTexture: could not unprotect the swap chain vtable; the "
                              "dye pump stays gated on the editor being open AND the "
                              "preview grid cannot build (its drain lives in this thunk).");
                g_hookInstalled.store(false, std::memory_order_release);
                return;
            }
            g_originalPresent = reinterpret_cast<PresentFn>(vtbl[kPresentSlot]);
            vtbl[kPresentSlot] = reinterpret_cast<void*>(&PresentThunk);
            ::VirtualProtect(&vtbl[kPresentSlot], sizeof(void*), old, &old);

            spdlog::info("DyeTexture: Present hooked (chain={} original={}), so the pump now "
                         "runs with the editor closed.",
                         static_cast<void*>(chain),
                         reinterpret_cast<void*>(g_originalPresent));
        }

        // ⚠ ONE CHARACTER, AND KEEP IT THAT WAY. Retiring a preview derives its
        // key from the commit's by rewriting the last character, so a longer tag
        // would silently produce a key that matches nothing and leave the
        // preview resident for ever.
        const char* QualityTag(DyeQuality::Quality a_q) {
            return a_q == DyeQuality::Quality::kCommit ? "c" : "p";
        }

        // The caller's own ceiling, as a key fragment. ⚠ EMPTY AT 0, which is
        // what keeps every texture cached before per-caller caps existed on the
        // exact key it already had. DyeKey::RampSuffix carries the same rule for
        // the same reason.
        std::string CapSuffix(std::uint32_t a_capPx) {
            return a_capPx == 0 ? std::string{} : fmt::format("|x{}", a_capPx);
        }

        // The identity of a COLOUR on a texture, with no quality in it.
        // ⚠ TAKES THE RESOLVED IDENTITY, NOT THE TEXTURE. The caller decides
        // what a source is called, because a nameless texture's material can
        // still know its authored path and Acquire is the only place that
        // pairing is visible. Passing the texture here is what made this
        // function read `a_src->name` and key every nameless armour diffuse as
        // the empty string.
        std::string ColourOf(const char* a_identity, const RE::NiColor& a_tint,
                             Blend a_blend, const Ramp& a_ramp, std::uint32_t a_capPx,
                             const char* a_maskName, const MaskDye& a_maskDye) {
            // Quantised to bytes deliberately: the picker is 8 bit per channel,
            // so two colours that differ below a byte are the same dye and
            // should share one texture rather than allocate a second.
            const auto r = static_cast<int>(a_tint.red * 255.0f + 0.5f);
            const auto g = static_cast<int>(a_tint.green * 255.0f + 0.5f);
            const auto b = static_cast<int>(a_tint.blue * 255.0f + 0.5f);
            const char* const name = a_identity;
            // ⚠ THE BLEND IS PART OF THE KEY. A diffuse and a cubemap can never
            // be the same file in practice, but the arithmetic is what the
            // cached bytes ARE, so keying without it would let one blend serve a
            // lookup that asked for the other.
            //
            // ⚠ AND THE RAMP IS IN IT FOR EXACTLY THE SAME REASON, one step
            // further. Two dyes with one hex between them and different second
            // stops are different textures, and a key that cannot tell them
            // apart hands back the wrong one. That fault is silent, cached and
            // persistent, so of everything in this feature it is the one most
            // likely to ship unnoticed. RampSuffix is empty for a flat dye, which
            // is what keeps every texture already cached on the key it had.
            //
            // ⚠ AND THE MASK, because a masked build depends on TWO textures.
            // DyeKey::MaskSuffix carries the reason; empty with no mask, as
            // every suffix here is empty at its default.
            //
            // ⚠ AND THE MASK DYE, because a masked build now has up to two
            // colours in it. Only when a mask is present at all: an unmasked
            // build has no disc for the sclera to be outside of.
            //
            // ⚠⚠ AND THE TAG COMES FROM BlendTag, NOT FROM A TERNARY HERE. The
            // chain this replaced spelled four enum values with three strings
            // and had aliased kFlakeNormal onto the default since the day it
            // was added. The header carries the measurement.
            // ⚠ AND THE IRIS DISC'S SETTINGS, on a masked build only. They move
            // the picture with every other input identical, which is the whole
            // reason each suffix here exists; empty at the shipped defaults, so
            // no existing entry changes key.
            const auto& cfgK = Settings::GetSingleton();
            return fmt::format("{}|{:02X}{:02X}{:02X}|{}{}{}{}{}{}", name ? name : "<unnamed>",
                               r, g, b, BlendTag(a_blend),
                               OS::DyeKey::RampSuffix(a_ramp.mode, a_ramp.secondSet, a_ramp.r2,
                                                      a_ramp.g2, a_ramp.b2, a_ramp.gloss),
                               CapSuffix(a_capPx), OS::DyeKey::MaskSuffix(a_maskName),
                               (a_maskName && *a_maskName)
                                   ? OS::DyeKey::MaskDyeSuffix(a_maskDye.irisSet,
                                                               a_maskDye.scleraSet,
                                                               a_maskDye.r, a_maskDye.g,
                                                               a_maskDye.b,
                                                               a_maskDye.splitSet,
                                                               a_maskDye.r2, a_maskDye.g2,
                                                               a_maskDye.b2)
                                   : std::string{},
                               (a_maskName && *a_maskName)
                                   ? OS::DyeKey::IrisDiscSuffix(cfgK.dyeEyeIrisRadius,
                                                                cfgK.dyeEyeIrisSoft,
                                                                cfgK.dyeEyeMaskBroad)
                                   : std::string{});
        }

        // ⚠ THE QUALITY IS PART OF THE CACHE KEY AND NOT PART OF THE COLOUR. One
        // colour has two entries at two sizes, and a lookup that ignored the
        // quality would hand a 512 preview back to a caller whose commit had
        // already been built.
        std::string KeyOf(const std::string& a_colour, DyeQuality::Quality a_q) {
            return a_colour + "|" + QualityTag(a_q);
        }

        // The identity of a TEXTURE, with neither colour nor quality. This is
        // what the settle clock is kept per, and what a refusal is keyed on.
        // ⚠ THE CAP IS IN THE SLOT TOO, not only in the colour. The slot owns the
        // settle clock, the queue's coalescing identity and the refusal set, so
        // one texture asked for at two ceilings has to be two slots: sharing one
        // would let an eye's capped commit satisfy a full-size request's settle
        // and cancel it, which is the same collision the ramp had one level up.
        // ⚠ AND SO IS THE MASK, for the same reason again: a masked and an
        // unmasked build of one diffuse are two different subjects, and two
        // eyes sharing a diffuse under different normals are two settle clocks.
        // ⚠ THE RESOLVED IDENTITY, for the reason on ColourOf. These two must
        // agree on what a source is called or the settle clock, the queue and
        // the refusal set would key one texture two ways.
        std::string SlotOf(const char* a_identity, Blend a_blend,
                           std::uint32_t a_capPx, const char* a_maskName) {
            const char* const name = a_identity;
            // ⚠ THE SAME BlendTag ColourOf USES, and sharing it is the fix
            // rather than a tidy-up: these were two independently written
            // ternary chains, so the aliasing had to be found and repaired
            // twice, and the next widening would have had to be too.
            return fmt::format("{}|{}{}{}", name ? name : "<unnamed>", BlendTag(a_blend),
                               CapSuffix(a_capPx), OS::DyeKey::MaskSuffix(a_maskName));
        }

        // The build queue's coalescing identity. ⚠ THE QUALITY IS IN IT, so a
        // queued commit cannot be superseded by the next frame's preview.
        std::string QueueSlotOf(const std::string& a_slot, DyeQuality::Quality a_q) {
            return a_slot + "|" + QualityTag(a_q);
        }

        std::uint64_t NowMs() {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
        }

        void LogStatsLocked(const char* a_where);

        // ⚠ ONLY ENTRIES THE CACHE ALONE HOLDS, AND THAT IS WHAT MAKES THIS
        // SAFE RATHER THAN A USE AFTER FREE. A tinted texture handed to a
        // material is referenced by that material's own NiPointer, so its count
        // is at least two; one means nothing is rendering from it and dropping
        // it frees it now. An entry still in use is skipped and considered again
        // on the next sweep, which is why this walks past rather than stopping.
        //
        // ⚠ OLDEST FIRST rather than least-recently-used. A dye session works
        // forwards through colours, so insertion order and recency are the same
        // order here, and keeping a second timestamp per entry would buy nothing
        // this cache can measure.
        // a_protect is the key that was inserted this instant. ⚠ IT MUST BE
        // SKIPPED. A texture the moment it is built is held by nothing but the
        // cache, so its refcount is one and it is the single most evictable
        // thing here, and the caller is about to hand it to a material. Without
        // this a full cache would free every new build immediately and rebuild
        // it forever.
        void EvictLocked(std::uint64_t a_budgetBytes, const std::string& a_protect) {
            if (g_bytes <= a_budgetBytes) {
                return;
            }
            std::size_t   freed{ 0 };
            std::uint64_t freedBytes{ 0 };
            for (auto it = g_order.begin();
                 it != g_order.end() && g_bytes > a_budgetBytes;) {
                if (*it == a_protect) {
                    ++it;
                    continue;
                }
                const auto found = g_cache.find(*it);
                if (found == g_cache.end()) {
                    it = g_order.erase(it);
                    continue;
                }
                if (found->second.tex && found->second.tex->GetRefCount() <= 1) {
                    g_bytes -= found->second.bytes;
                    freedBytes += found->second.bytes;
                    ++freed;
                    g_cache.erase(found);
                    it = g_order.erase(it);
                } else {
                    ++it;
                }
            }
            if (freed != 0) {
                g_stats.evicted += freed;
                spdlog::info("DyeTexture: evicted {} unused texture(s), {:.1f} MiB, now "
                             "holding {:.1f} MiB of a {:.0f} MiB budget.",
                             freed, static_cast<double>(freedBytes) / (1024.0 * 1024.0),
                             static_cast<double>(g_bytes) / (1024.0 * 1024.0),
                             static_cast<double>(a_budgetBytes) / (1024.0 * 1024.0));
            }
        }

        bool IsSrgb(DXGI_FORMAT a_fmt) {
            switch (a_fmt) {
                case DXGI_FORMAT_BC1_UNORM_SRGB:
                case DXGI_FORMAT_BC2_UNORM_SRGB:
                case DXGI_FORMAT_BC3_UNORM_SRGB:
                case DXGI_FORMAT_BC7_UNORM_SRGB:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
                    return true;
                default:
                    return false;
            }
        }

        // Builds an NiSourceTexture that owns nothing the engine loaded, by
        // copying a live one of the same class and overriding the fields whose
        // meaning is established.
        //
        // ⚠ A NEW OBJECT RATHER THAN A WRITE INTO THE EXISTING ONE, and that is
        // correctness rather than tidiness. One NiSourceTexture serves EVERY
        // shape using that texture path, so substituting inside it would dye
        // every actor wearing anything that shares the file. Per-actor dye needs
        // a per-recipe object, and the material clone is where it goes.
        //
        // ⚠ THE BYTE COPY IS THE EVIDENCE, NOT A SHORTCUT. Every field this does
        // not understand keeps a value the engine itself produced for a texture
        // of exactly this class, and the vtable pointer comes from a live
        // instance rather than a relocation we would have to trust. Only the
        // five fields whose meaning IS established get overridden.
        RE::NiSourceTexture* Forge(const RE::NiSourceTexture* a_model, RendererData* a_rd,
                                   const std::string& a_key) {
            void* const mem = RE::malloc(sizeof(RE::NiSourceTexture));
            if (!mem) {
                return nullptr;
            }
            std::memcpy(mem, static_cast<const void*>(a_model), sizeof(RE::NiSourceTexture));
            auto* const t = static_cast<RE::NiSourceTexture*>(mem);

            t->_refCount = 0;  // the cache's NiPointer brings this to 1
            // ⚠ OUT OF NiTexture's GLOBAL LIST ON PURPOSE. The engine walks that
            // list to release textures on a device event, and an object never
            // linked into it is one the walk cannot reach. The copied prev/next
            // would otherwise point this into the middle of a list that does not
            // know about it.
            t->prev            = nullptr;
            t->next            = nullptr;
            t->unk40           = nullptr;  // nothing streams this off disk
            t->rendererTexture = reinterpret_cast<RE::BSGraphics::Texture*>(a_rd);
            // ⚠ ZERO BEFORE ASSIGNING. `name` is a BSFixedString and the memcpy
            // copied a string-pool pointer this object never acquired. Assigning
            // over it would release a reference we do not own; zeroing first
            // makes try_release a no-op and the acquire real.
            std::memset(&t->name, 0, sizeof(t->name));
            t->name = a_key.c_str();
            return t;
        }

        ID3D11ComputeShader* CompileOne(ID3D11Device* a_device, const char* a_body,
                                        const char* a_name) {
            // Both shaders are prefixed with the shared arithmetic rather than
            // #include, because D3DCompile has no include handler here and the
            // two must not be allowed to disagree.
            const std::string src = std::string{ kCommon } + a_body;
            ID3DBlob*         code{ nullptr };
            ID3DBlob*         errors{ nullptr };
            const HRESULT     hr = ::D3DCompile(src.data(), src.size(), a_name, nullptr, nullptr,
                                            "main", "cs_5_0", 0, 0, &code, &errors);
            if (FAILED(hr) || !code) {
                spdlog::error("DyeTexture: D3DCompile('{}') failed hr=0x{:08X} msg='{}'", a_name,
                              static_cast<unsigned>(hr),
                              errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
                if (errors) {
                    errors->Release();
                }
                return nullptr;
            }
            if (errors) {
                errors->Release();
            }
            ID3D11ComputeShader* cs{ nullptr };
            const HRESULT hrs = a_device->CreateComputeShader(code->GetBufferPointer(),
                                                              code->GetBufferSize(), nullptr,
                                                              &cs);
            code->Release();
            if (FAILED(hrs) || !cs) {
                spdlog::error("DyeTexture: CreateComputeShader('{}') failed hr=0x{:08X}", a_name,
                              static_cast<unsigned>(hrs));
                return nullptr;
            }
            return cs;
        }

        // The analysis pass's buffer, its two views and the 16-byte staging copy
        // the log reads. Failure is not fatal to a build: without it the tint
        // pass sees a zeroed Disc and behaves exactly as it did before the disc
        // existed, which is the fail-safe direction.
        bool EnsureDiscBuffer(ID3D11Device* a_device) {
            if (g_disc && g_discUav && g_discSrv && g_discStaging) {
                return true;
            }
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 16;  // one float4
            bd.Usage     = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(a_device->CreateBuffer(&bd, nullptr, &g_disc)) || !g_disc) {
                spdlog::warn("DyeTexture: the iris analysis buffer could not be created, "
                             "so an eye whose mask marks the whole eye keeps taking the "
                             "dye whole.");
                return false;
            }
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format             = DXGI_FORMAT_R32G32B32A32_FLOAT;
            ud.ViewDimension      = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = 1;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd2{};
            sd2.Format             = DXGI_FORMAT_R32G32B32A32_FLOAT;
            sd2.ViewDimension      = D3D11_SRV_DIMENSION_BUFFER;
            sd2.Buffer.NumElements = 1;
            D3D11_BUFFER_DESC st{};
            st.ByteWidth      = 16;
            st.Usage          = D3D11_USAGE_STAGING;
            st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(a_device->CreateUnorderedAccessView(g_disc, &ud, &g_discUav)) ||
                FAILED(a_device->CreateShaderResourceView(g_disc, &sd2, &g_discSrv)) ||
                FAILED(a_device->CreateBuffer(&st, nullptr, &g_discStaging))) {
                spdlog::warn("DyeTexture: the iris analysis views could not be created; "
                             "the mask is used exactly as authored.");
                if (g_discUav) { g_discUav->Release(); g_discUav = nullptr; }
                if (g_discSrv) { g_discSrv->Release(); g_discSrv = nullptr; }
                if (g_discStaging) { g_discStaging->Release(); g_discStaging = nullptr; }
                g_disc->Release();
                g_disc = nullptr;
                return false;
            }
            return true;
        }

        bool EnsureShader(ID3D11Device* a_device) {
            if (g_cs && g_csCube && g_cb) {
                EnsureDiscBuffer(a_device);  // idempotent, and never fatal
                return true;
            }
            if (g_compileFailed) {
                return false;
            }
            g_cs     = CompileOne(a_device, kShader, "DyeTexture2D");
            g_csCube = CompileOne(a_device, kCubeShader, "DyeTextureCube");
            // ⚠ NOT PART OF THE FAILURE PAIR BELOW. A missing analysis pass
            // costs the derived iris disc and nothing else, so it must not stop
            // every dye in the mod from building.
            g_csMask = CompileOne(a_device, kMaskShader, "DyeTextureMaskScan");
            if (!g_cs || !g_csCube) {
                g_compileFailed = true;
                if (g_cs) { g_cs->Release(); g_cs = nullptr; }
                if (g_csCube) { g_csCube->Release(); g_csCube = nullptr; }
                return false;
            }

            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(Params);
            bd.Usage     = D3D11_USAGE_DEFAULT;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(a_device->CreateBuffer(&bd, nullptr, &g_cb)) || !g_cb) {
                g_compileFailed = true;
                spdlog::error("DyeTexture: constant buffer creation failed.");
                g_cs->Release();
                g_cs = nullptr;
                g_csCube->Release();
                g_csCube = nullptr;
                return false;
            }
            EnsureDiscBuffer(a_device);
            return true;
        }

        // Read three texels out of a finished destination and log them.
        //
        // ⚠ THIS IS THE NEGATIVE CONTROL FOR THE ONE THING CAPPING INTRODUCES.
        // Every other failure in this file is loud. A mip read that lands
        // outside the bound view returns ZEROS rather than failing, so a broken
        // cap looks like a working build that produced a black texture, and the
        // log would say nothing at all.
        //
        // ⚠ THE DESTINATION RATHER THAN THE SOURCE, and that is the only way it
        // can be done cheaply. The source is BC1 or BC7, and a block compressed
        // copy has to be block aligned, so a one-texel read of it is illegal.
        // The destination is RGBA8. If the source read returned zeros then the
        // overlay sends zero to zero and the alpha copies straight through, so
        // three texels of (0,0,0,0) is the signature of a level that was not
        // there. A real texture having three black texels at these exact
        // positions is possible and vanishingly unlikely, which is why this
        // warns rather than refusing.
        void ControlCappedRead(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx,
                               ID3D11Texture2D* a_dst, const DyeQuality::Level& a_level,
                               const std::string& a_key) {
            D3D11_TEXTURE2D_DESC stg{};
            stg.Width            = 1;
            stg.Height           = 1;
            stg.MipLevels        = 1;
            stg.ArraySize        = 1;
            stg.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
            stg.SampleDesc.Count = 1;
            stg.Usage            = D3D11_USAGE_STAGING;
            stg.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

            ID3D11Texture2D* readback{ nullptr };
            if (FAILED(a_device->CreateTexture2D(&stg, nullptr, &readback)) || !readback) {
                spdlog::warn("DyeTexture: the capped-read control could not allocate, so the "
                             "mip read is UNVERIFIED for this session.");
                return;
            }

            const UINT points[3][2]{ { 0u, 0u },
                                     { a_level.width / 2u, a_level.height / 2u },
                                     { a_level.width - 1u, a_level.height - 1u } };
            int nonZero = 0;
            for (const auto& pt : points) {
                D3D11_BOX box{};
                box.left   = pt[0];
                box.top    = pt[1];
                box.front  = 0;
                box.right  = pt[0] + 1;
                box.bottom = pt[1] + 1;
                box.back   = 1;
                a_ctx->CopySubresourceRegion(readback, 0, 0, 0, 0, a_dst, 0, &box);

                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(a_ctx->Map(readback, 0, D3D11_MAP_READ, 0, &m)) || !m.pData) {
                    spdlog::warn("DyeTexture: the capped-read control could not map, so the "
                                 "mip read is UNVERIFIED for this session.");
                    readback->Release();
                    return;
                }
                const auto* const px = static_cast<const std::uint8_t*>(m.pData);
                spdlog::info("DyeTexture: capped-read control ({:5},{:5}) = ({:3},{:3},{:3},"
                             "{:3})",
                             pt[0], pt[1], px[0], px[1], px[2], px[3]);
                if (px[0] != 0 || px[1] != 0 || px[2] != 0 || px[3] != 0) {
                    ++nonZero;
                }
                a_ctx->Unmap(readback, 0);
            }
            readback->Release();

            spdlog::info("DyeTexture: ===== CAPPED READ CONTROL on '{}': source mip {} at "
                         "{}x{}, {} of 3 texels non-zero, so the level {} =====",
                         a_key, a_level.mip, a_level.width, a_level.height, nonZero,
                         nonZero != 0 ? "IS THERE" : "WAS NOT READ");
        }

        // One build, start to finish, on the render thread. Returns the forged
        // texture or null, and reports why in the log either way.
        RE::NiSourceTexture* Build(ID3D11Device* a_device, ID3D11DeviceContext* a_ctx,
                                   const Request& a_req, bool& a_refused,
                                   std::uint64_t& a_bytes) {
            a_refused = false;
            auto* const src = a_req.source.get();
            if (!src) {
                return nullptr;
            }
            auto* const rd = reinterpret_cast<RendererData*>(src->rendererTexture);
            if (!rd || !rd->texture || !rd->resourceView) {
                spdlog::warn("DyeTexture: '{}' has no live renderer texture; refused.",
                             a_req.key);
                a_refused = true;
                return nullptr;
            }

            D3D11_TEXTURE2D_DESC sd{};
            rd->texture->GetDesc(&sd);

            // ⚠ THE LAYOUT CONTROL, AND IT STAYS IN THE SHIPPING PATH RATHER
            // THAN RETIRING WITH THE PROBE. If +0x00 and +0x10 are the texture
            // and its view, the view must name that exact texture as its
            // resource. Two wrong offsets cannot produce that. It costs one
            // AddRef per build and it is the difference between refusing a
            // strange texture and dispatching compute at a garbage pointer.
            ID3D11Resource* res{ nullptr };
            rd->resourceView->GetResource(&res);
            const bool identity = res == static_cast<ID3D11Resource*>(rd->texture);
            if (res) {
                res->Release();
            }
            if (!identity) {
                spdlog::error("DyeTexture: LAYOUT CONTROL FAILED on '{}'. The view at +0x10 "
                              "does not name the texture at +0x00, so this source is refused "
                              "rather than tinted.", a_req.key);
                a_refused = true;
                return nullptr;
            }

            const bool srgb = IsSrgb(sd.Format);
            // ⚠ THE SOURCE'S OWN DESC DECIDES THIS, not the caller. A cubemap is
            // six array slices carrying the TEXTURECUBE misc flag, and detecting
            // it here means an envmap and a diffuse go down the same call with
            // no second API for the caller to get wrong.
            const bool cube = sd.ArraySize == 6 &&
                              (sd.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;

            // ⚠ OS-140. THE CAP IS APPLIED HERE AND NOWHERE ELSE, so a preview
            // and a commit are the same code path with a different number. The
            // source's own mip count bounds it: a texture that shipped without a
            // chain cannot be capped, and PickLevel says so by returning mip 0
            // at full size rather than reading a level that does not exist.
            const auto& cfg      = Settings::GetSingleton();
            const auto  globalCap = a_req.quality == DyeQuality::Quality::kCommit
                                        ? cfg.dyeCommitCapPx
                                        : cfg.dyePreviewCapPx;
            // ⚠ THE SMALLER OF THE TWO WINS, AND 0 MEANS NO CAP ON EITHER SIDE,
            // so the fold cannot be a plain std::min. A caller's ceiling only
            // ever tightens the install's, never loosens it: an eye asking for
            // 512 on an install whose commits are capped at 256 gets 256.
            const auto capPx = (globalCap == 0)         ? a_req.capPx
                               : (a_req.capPx == 0)     ? globalCap
                                                        : std::min(globalCap, a_req.capPx);
            const auto  level = DyeQuality::PickLevel(sd.Width, sd.Height, sd.MipLevels, capPx);

            // ⚠ TYPELESS WITH TWO VIEWS. A UAV cannot be created on an _SRGB
            // format, and the engine has to read one back if the source was
            // sRGB, so the resource is typeless and carries a UNORM view to
            // write through and a matching view to be read through. MipLevels 0
            // asks D3D for the full chain; the shader writes mip 0 and
            // GenerateMips fills the rest, which is cheaper than tinting each
            // level and is what the spec predicted.
            D3D11_TEXTURE2D_DESC dd{};
            dd.Width            = level.width;
            dd.Height           = level.height;
            dd.MipLevels        = 0;
            dd.ArraySize        = cube ? 6 : 1;
            dd.Format           = DXGI_FORMAT_R8G8B8A8_TYPELESS;
            dd.SampleDesc.Count = 1;
            dd.Usage            = D3D11_USAGE_DEFAULT;
            dd.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS |
                           D3D11_BIND_RENDER_TARGET;
            dd.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS |
                           (cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0u);

            ID3D11Texture2D* dst{ nullptr };
            if (FAILED(a_device->CreateTexture2D(&dd, nullptr, &dst)) || !dst) {
                spdlog::error("DyeTexture: could not allocate {}x{}{} for '{}'.", dd.Width,
                              dd.Height, cube ? " cube" : "", a_req.key);
                return nullptr;
            }

            const auto unormFmt =
                srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;

            // The view the ENGINE reads through has to be the same shape the
            // material expects, so a cubemap goes back as a cube.
            D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
            svd.Format = unormFmt;
            if (cube) {
                svd.ViewDimension                 = D3D11_SRV_DIMENSION_TEXTURECUBE;
                svd.TextureCube.MostDetailedMip   = 0;
                svd.TextureCube.MipLevels         = static_cast<UINT>(-1);
            } else {
                svd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
                svd.Texture2D.MostDetailedMip = 0;
                svd.Texture2D.MipLevels       = static_cast<UINT>(-1);
            }

            D3D11_UNORDERED_ACCESS_VIEW_DESC uvd{};
            uvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (cube) {
                uvd.ViewDimension                  = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                uvd.Texture2DArray.MipSlice        = 0;
                uvd.Texture2DArray.FirstArraySlice = 0;
                uvd.Texture2DArray.ArraySize       = 6;
            } else {
                uvd.ViewDimension      = D3D11_UAV_DIMENSION_TEXTURE2D;
                uvd.Texture2D.MipSlice = 0;
            }

            ID3D11ShaderResourceView*  ourSrv{ nullptr };
            ID3D11UnorderedAccessView* uav{ nullptr };
            a_device->CreateShaderResourceView(dst, &svd, &ourSrv);
            a_device->CreateUnorderedAccessView(dst, &uvd, &uav);

            // ⚠ A VIEW OF OUR OWN ON BOTH PATHS, AND ON THE 2D PATH THAT IS NEW
            // FOR OS-140. Capping means reading a level above 0, and `Src.Load`
            // at a level the bound view does not expose returns ZEROS rather
            // than failing. That failure is a black garment with nothing in the
            // log. The engine's view probably does expose the whole chain;
            // building our own off the same texture with MipLevels = -1 means we
            // do not have to be right about that. It costs one view creation per
            // build and leaves the engine's own view untouched.
            //
            // The cube path needed one already, for a different reason: the
            // engine's cubemap view is a TEXTURECUBE, and a TextureCube cannot be
            // bound where the shader declares Texture2DArray.
            D3D11_SHADER_RESOURCE_VIEW_DESC ssd{};
            ssd.Format = sd.Format;
            if (cube) {
                ssd.ViewDimension                  = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
                ssd.Texture2DArray.MostDetailedMip = 0;
                ssd.Texture2DArray.MipLevels       = static_cast<UINT>(-1);
                ssd.Texture2DArray.FirstArraySlice = 0;
                ssd.Texture2DArray.ArraySize       = 6;
            } else {
                ssd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
                ssd.Texture2D.MostDetailedMip = 0;
                ssd.Texture2D.MipLevels       = static_cast<UINT>(-1);
            }
            ID3D11ShaderResourceView* srcSrvOwned{ nullptr };
            if (FAILED(a_device->CreateShaderResourceView(rd->texture, &ssd, &srcSrvOwned)) ||
                !srcSrvOwned) {
                spdlog::error("DyeTexture: could not view the source of '{}' with its whole "
                              "mip chain, so it cannot be tinted at a capped size.",
                              a_req.key);
                if (ourSrv) { ourSrv->Release(); }
                if (uav) { uav->Release(); }
                dst->Release();
                return nullptr;
            }
            ID3D11ShaderResourceView* srcSrv = srcSrvOwned;

            if (!ourSrv || !uav) {
                spdlog::error("DyeTexture: view creation failed for '{}'.", a_req.key);
                if (srcSrvOwned) { srcSrvOwned->Release(); }
                if (ourSrv) { ourSrv->Release(); }
                if (uav) { uav->Release(); }
                dst->Release();
                return nullptr;
            }

            // ---- the iris mask's own view, when the request carries one -----
            //
            // ⚠ A MASK THAT IS NOT READY IS A "NOT NOW", NEVER A DOWNGRADE.
            // Building unmasked would cache "dye the whole eyeball" under a key
            // that PROMISES the mask, and the cache would serve that lie for as
            // long as the entry lived. Returning null un-refused lands in the
            // allocation-failure path, which retries on a later pass.
            //
            // ⚠ THE KEY ALREADY NAMES THE MASK, so this failure cannot recur
            // for ever on a mask that never loads: the caller re-asks on each
            // repaint and stops asking when the shape stops carrying the mask.
            //
            // ⚠ A MASKED CUBE OR A MASKED RAMP IS A CALLER ERROR, said out loud
            // and built without the mask. The mask rides the 2D shader only,
            // and its window rides the two scalars the ramp would need. No call
            // site can produce either today; the log line is for the one that
            // some day does.
            ID3D11ShaderResourceView* maskSrvOwned{ nullptr };
            bool                      masked = a_req.mask != nullptr;
            if (masked && (cube || a_req.ramp.mode != 0)) {
                spdlog::error("DyeTexture: '{}' asked for an iris mask on a {} build, "
                              "which the mask cannot ride; built unmasked.",
                              a_req.key, cube ? "cube" : "ramped");
                masked = false;
            }
            if (masked) {
                auto* const mrd =
                    reinterpret_cast<RendererData*>(a_req.mask->rendererTexture);
                if (!mrd || !mrd->texture || !mrd->resourceView) {
                    spdlog::warn("DyeTexture: the mask of '{}' has no live renderer "
                                 "texture yet; the build waits for it.",
                                 a_req.key);
                    ourSrv->Release();
                    uav->Release();
                    srcSrvOwned->Release();
                    dst->Release();
                    return nullptr;
                }
                // The layout control, exactly the source's: two wrong offsets
                // cannot name each other, and dispatching compute at a garbage
                // pointer is the alternative.
                ID3D11Resource* mres{ nullptr };
                mrd->resourceView->GetResource(&mres);
                const bool mIdentity = mres == static_cast<ID3D11Resource*>(mrd->texture);
                if (mres) {
                    mres->Release();
                }
                if (!mIdentity) {
                    spdlog::error("DyeTexture: LAYOUT CONTROL FAILED on the mask of '{}', "
                                  "so this build is refused rather than tinted.",
                                  a_req.key);
                    a_refused = true;
                    ourSrv->Release();
                    uav->Release();
                    srcSrvOwned->Release();
                    dst->Release();
                    return nullptr;
                }
                D3D11_TEXTURE2D_DESC md{};
                mrd->texture->GetDesc(&md);
                // A view of our own with the whole chain, for the source's
                // reason: the shader reads the mask's LAST mip as its mean, and
                // a view that stopped short would hand back zeros in silence.
                D3D11_SHADER_RESOURCE_VIEW_DESC msd{};
                msd.Format                    = md.Format;
                msd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
                msd.Texture2D.MostDetailedMip = 0;
                msd.Texture2D.MipLevels       = static_cast<UINT>(-1);
                if (FAILED(a_device->CreateShaderResourceView(mrd->texture, &msd,
                                                              &maskSrvOwned)) ||
                    !maskSrvOwned) {
                    spdlog::error("DyeTexture: could not view the mask of '{}'; the "
                                  "build waits rather than dyeing the whole shape.",
                                  a_req.key);
                    ourSrv->Release();
                    uav->Release();
                    srcSrvOwned->Release();
                    dst->Release();
                    return nullptr;
                }
            }

            Params p;
            p.tint[0] = a_req.tint.red;
            p.tint[1] = a_req.tint.green;
            p.tint[2] = a_req.tint.blue;
            // ⚠ BIT 2 IS "RUN THE RAMP", AND ONE BIT IS ENOUGH. DyeRamp::
            // EffectiveMode has already resolved iridescent down to nacre by the
            // time a request gets here, and which TARGET the ramp lands on was
            // decided by the caller picking the 2D or the cube shader. So the
            // only thing left for the shader to know is whether to run it.
            //
            // ⚠ AND IT SITS INSIDE THE RECOLOUR BRANCH, NEVER ON THE OVERLAY ONE.
            // The overlay is the game's own D*D + 2*T*D*(1-D) curve, which is
            // what makes a dyed diffuse look dyed rather than painted; a ramp
            // there would be a second idea of what dyeing is.
            // ⚠ BIT 3 IS A FLAG RATHER THAN A FLOAT BECAUSE THE BUFFER IS FULL.
            // Params is 48 bytes with every one spent, and "which arc" is boolean
            // anyway. A fourth register would be the honest cost of the next
            // SCALAR the ramp wants; this is not one.
            // ⚠ THE AUTO WINDOW GOES WITH THE LUMINANCE KEY AND NOWHERE ELSE.
            // The cube's direction keys window an ANGLE, and scaling an angle by
            // a brightness is nonsense, so the bit is only emitted for the
            // diffuse ramp and for a cube explicitly on the luminance axis. It
            // also needs a real mip chain to read an average from: a source
            // with fewer than four mips gets the manual window rather than a
            // corner texel pretending to be a mean.
            const auto& cfgR = Settings::GetSingleton();
            const bool lumKey  = !cube || cfgR.dyeRampReflectionAxis >= 2;
            const bool autoWin = cfgR.dyeRampAutoWindow && a_req.ramp.mode != 0 && lumKey &&
                                 sd.MipLevels >= 4;
            p.flags   = (srgb ? 1u : 0u) | (a_req.blend == Blend::kRecolour ? 2u : 0u) |
                      (a_req.ramp.mode != 0 ? 4u : 0u) |
                      (cfgR.dyeRampSpectral ? 8u : 0u) |
                      (a_req.blend == Blend::kFlake ? 16u : 0u) |
                      (a_req.blend == Blend::kFlakeNormal ? 32u : 0u) |
                      (autoWin ? 64u : 0u) | (masked ? 128u : 0u) |
                      (masked && a_req.maskDye.scleraSet ? 256u : 0u) |
                      (masked && !a_req.maskDye.irisSet ? 512u : 0u) |
                      (masked && a_req.maskDye.splitSet ? 1024u : 0u) |
                      // ⚠ ONE BIT EACH AND MUTUALLY EXCLUSIVE BY CONSTRUCTION,
                      // because the blend is one enum value. BlendCurve reads
                      // them in a fixed order and falls through to soft light,
                      // so even a hand-corrupted flags word renders something
                      // rather than nothing.
                      (a_req.blend == Blend::kMultiply ? 2048u : 0u) |
                      (a_req.blend == Blend::kScreen ? 4096u : 0u) |
                      (a_req.blend == Blend::kOverlay ? 8192u : 0u) |
                      (a_req.blend == Blend::kColour ? 16384u : 0u) |
                      (a_req.blend == Blend::kLuminosity ? 32768u : 0u);
            // The source's top mip rides the upper half of srcMip, which the
            // shader unpacks; the auto window reads its average there.
            p.srcMip  = level.mip | ((sd.MipLevels - 1u) << 16);
            p.stopB[0] = static_cast<float>(a_req.ramp.r2) / 255.0f;
            p.stopB[1] = static_cast<float>(a_req.ramp.g2) / 255.0f;
            p.stopB[2] = static_cast<float>(a_req.ramp.b2) / 255.0f;
            // ⚠ WHICH WINDOW, DECIDED BY WHICH TARGET THIS BUILD IS. `cube` is
            // already the answer to that question a few lines up, and it is the
            // right discriminator rather than the blend: the blend says HOW the
            // tint combines, this says WHAT KIND OF TEXTURE the luminance came
            // off. A cubemap at mean luminance 0.31 and an armour albedo peaking
            // at 0.22 need windows two orders apart, so one pair cannot serve
            // both and sharing them is what made the first field run paint a
            // single colour.
            // In auto mode the two floats are MULTIPLIERS of the texture's own
            // mean; in manual mode they are the absolute window, as before.
            p.rampLo = autoWin ? cfg.dyeRampAutoLo
                     : cube    ? cfg.dyeRampReflectionLo
                               : cfg.dyeRampDiffuseLo;
            p.rampHi = autoWin ? cfg.dyeRampAutoHi
                     : cube    ? cfg.dyeRampReflectionHi
                               : cfg.dyeRampDiffuseHi;
            // Only the cube shader reads this; the 2D one has no direction to key
            // on, so nacre is always the luminance ramp.
            p.rampAxis = static_cast<float>(cfg.dyeRampReflectionAxis);
            // ⚠ BOTH TARGETS, unlike the axis. The intensity-flattening was in the
            // shared RampAt, so nacre on cloth lost its sheen exactly as the
            // reflection did, and one knob fixes both.
            p.sheen = cfg.dyeRampSheen;
            // ⚠ ON A MASKED BUILD THE RAMP'S TWO SCALARS CARRY THE MASK WINDOW
            // instead, as multipliers of the mask's own mean alpha. A masked
            // request is always flat (enforced above), so the ramp cannot want
            // them, which is the same fill-site discipline the auto window's
            // bit 6 already uses: the shader never knows which INI key it reads.
            //
            // ⚠ AND gStopB CARRIES THE SCLERA'S COLOUR, the ramp's second stop
            // being the other thing a flat request never sends.
            if (masked) {
                p.rampLo = cfg.dyeEyeMaskLo;
                p.rampHi = cfg.dyeEyeMaskHi;
                if (a_req.maskDye.scleraSet) {
                    p.stopB[0] = static_cast<float>(a_req.maskDye.r) / 255.0f;
                    p.stopB[1] = static_cast<float>(a_req.maskDye.g) / 255.0f;
                    p.stopB[2] = static_cast<float>(a_req.maskDye.b) / 255.0f;
                }
                // ⚠ THE SPLIT WINS gStopB WHEN BOTH ARE ASKED FOR, matching the
                // funnel that already drops the sclera on a split request: a
                // split set's flat mask gives the sclera nothing to land on, so
                // the slot carries the right eye's colour instead.
                if (a_req.maskDye.splitSet) {
                    p.stopB[0] = static_cast<float>(a_req.maskDye.r2) / 255.0f;
                    p.stopB[1] = static_cast<float>(a_req.maskDye.g2) / 255.0f;
                    p.stopB[2] = static_cast<float>(a_req.maskDye.b2) / 255.0f;
                }
            }
            p.irisRadius = cfg.dyeEyeIrisRadius;
            p.irisSoft   = cfg.dyeEyeIrisSoft;
            p.maskBroad  = cfg.dyeEyeMaskBroad;
            a_ctx->UpdateSubresource(g_cb, 0, nullptr, &p, 0, 0);

            // ---- the mask analysis pass, before the tint reads its answer ----
            //
            // ⚠ MASKED BUILDS ONLY, and it writes the buffer every time rather
            // than trusting what is in it. The buffer is process-wide (one
            // build runs at a time on this thread), so leaving a previous eye's
            // verdict in place would give this one somebody else's iris.
            if (masked && g_csMask && g_discUav && EnsureDiscBuffer(a_device)) {
                a_ctx->CSSetShader(g_csMask, nullptr, 0);
                a_ctx->CSSetConstantBuffers(0, 1, &g_cb);
                ID3D11ShaderResourceView* const maskOnly[1]{ maskSrvOwned };
                a_ctx->CSSetShaderResources(0, 1, maskOnly);
                a_ctx->CSSetUnorderedAccessViews(0, 1, &g_discUav, nullptr);
                a_ctx->Dispatch(1, 1, 1);
                ID3D11UnorderedAccessView* const noUav[1]{ nullptr };
                ID3D11ShaderResourceView* const  noSrv[1]{ nullptr };
                a_ctx->CSSetUnorderedAccessViews(0, 1, noUav, nullptr);
                a_ctx->CSSetShaderResources(0, 1, noSrv);
                // ⚠ THE VERDICT COMES HOME SO THE LOG CAN CARRY IT. 16 bytes,
                // once per BUILD rather than per frame, and it is the only way
                // a field report about a wrongly placed iris can be answered
                // without a debugger. Same discipline as the capped-read
                // control below.
                if (g_discStaging) {
                    a_ctx->CopyResource(g_discStaging, g_disc);
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (SUCCEEDED(a_ctx->Map(g_discStaging, 0, D3D11_MAP_READ, 0, &m)) &&
                        m.pData) {
                        const float* const v = static_cast<const float*>(m.pData);
                        if (v[3] > 0.5f) {
                            spdlog::info(
                                "EyeIris: '{}' has a mask that marks the whole eye rather "
                                "than the iris, so the tint takes a disc worked out from "
                                "it: centre ({:.3f}, {:.3f}), radius {:.3f} of the "
                                "texture, feather {:.2f}.",
                                a_req.key, v[0], v[1], v[2], cfg.dyeEyeIrisSoft);
                        } else {
                            spdlog::info(
                                "EyeIris: '{}' keeps its authored mask; it is a disc "
                                "already, or it covers nothing above its own mean.",
                                a_req.key);
                        }
                        a_ctx->Unmap(g_discStaging, 0);
                    }
                }
            }

            a_ctx->CSSetShader(cube ? g_csCube : g_cs, nullptr, 0);
            a_ctx->CSSetConstantBuffers(0, 1, &g_cb);
            // t1 rides along null on an unmasked build, which is every build
            // before eyes; the shader only reads it behind bit 7. t2 is the
            // analysis verdict, read only inside that same branch.
            ID3D11ShaderResourceView* const srvs[3]{ srcSrv, maskSrvOwned,
                                                     masked ? g_discSrv : nullptr };
            a_ctx->CSSetShaderResources(0, 3, srvs);
            a_ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            // ⚠ OVER THE DESTINATION, NOT THE SOURCE. A capped build writes a
            // smaller grid, and dispatching the source's grid would launch 64x
            // the threads for the same picture with every one past the
            // destination bounds returning early.
            a_ctx->Dispatch((level.width + 7u) / 8u, (level.height + 7u) / 8u, cube ? 6u : 1u);

            // ⚠ UNBOUND BEFORE ANYTHING ELSE DRAWS. Leaving the game's own
            // diffuse view and our UAV bound to the compute stage holds
            // references and leaves the pipeline in a state the game did not
            // set. The game re-binds what it uses, but it has no reason to touch
            // a compute stage it never uses, so nothing else would clear this.
            ID3D11UnorderedAccessView* const nullUav[1]{ nullptr };
            ID3D11ShaderResourceView* const  nullSrv[3]{ nullptr, nullptr, nullptr };
            ID3D11Buffer* const              nullCb[1]{ nullptr };
            a_ctx->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
            a_ctx->CSSetShaderResources(0, 3, nullSrv);
            a_ctx->CSSetConstantBuffers(0, 1, nullCb);
            a_ctx->CSSetShader(nullptr, nullptr, 0);
            uav->Release();
            if (srcSrvOwned) {
                srcSrvOwned->Release();
            }
            if (maskSrvOwned) {
                maskSrvOwned->Release();
            }

            a_ctx->GenerateMips(ourSrv);

            // ⚠ ONCE PER SESSION AND ONLY FOR A CAPPED BUILD. It maps, which
            // blocks on the GPU, so it must never become a per-build cost. Mip 0
            // needs no control: reading the level the engine itself renders from
            // cannot be out of range.
            static bool s_controlDone = false;
            if (!s_controlDone && level.mip != 0) {
                s_controlDone = true;
                ControlCappedRead(a_device, a_ctx, dst, level, a_req.key);
            }

            D3D11_TEXTURE2D_DESC made{};
            dst->GetDesc(&made);

            // ---- the Community Shaders marker mip, iridescent cubes only ----
            //
            // ⚠ THIS IS THE LINE OF COMMUNICATION WITH COMMUNITY SHADERS, found
            // 2026-08-08 after five attempts tuned a texture CS never renders.
            // With dynamic cubemaps on (this load order forces them for every
            // envmap material via Vanilla Fresnel's conversion), CS ignores the
            // cubemap's directional content entirely and samples ONE texel: the
            // top mip at LOD 15, which clamps to the last mip. Lighting.hlsl
            // 2526-2531 on the shipped Jiaye build: if that texel's ALPHA < 1,
            // its rgb IS the reflection's F0 and its alpha IS the roughness.
            // That is the Dynamic Cubemap Creator authoring convention, and
            // writing it here means the reflection carries the dye's SECOND
            // stop, cleanly, instead of the mud that mip-averaging a spectral
            // ramp produces (which is exactly what "always just 1 colour" was).
            //
            // ⚠ AFTER GenerateMips, which would otherwise overwrite it with the
            // chain average. Same command stream, so ordering is guaranteed.
            //
            // ⚠ VANILLA AND NON-CONVERTING INSTALLS ARE UNHARMED BY DESIGN. The
            // vanilla envmap path samples directionally and only ever reaches
            // the last mip under extreme minification, so the directional ramp
            // the compute pass just built keeps serving those installs
            // untouched. This texel only speaks when something asks the exact
            // question CS asks.
            //
            // Bytes go in as authored sRGB: the engine-facing view is UNORM
            // (srgb has been false for every cubemap this path has logged) and
            // CS gamma-decodes the sample itself.
            if (cube && a_req.ramp.mode == 2 && made.MipLevels > 0) {
                const std::uint8_t rough = static_cast<std::uint8_t>(std::clamp(
                    255 - static_cast<int>(a_req.ramp.gloss), 10, 240));
                const std::uint8_t texel[4]{ a_req.ramp.r2, a_req.ramp.g2, a_req.ramp.b2,
                                             rough };
                const UINT lastMip = made.MipLevels - 1;
                for (UINT face = 0; face < 6; ++face) {
                    a_ctx->UpdateSubresource(
                        dst, D3D11CalcSubresource(lastMip, face, made.MipLevels), nullptr,
                        texel, sizeof texel, sizeof texel);
                }
                spdlog::info("DyeTexture: marker mip on '{}': F0={:02X}{:02X}{:02X} "
                             "roughness={:.2f}, so a dynamic-cubemap install reflects the "
                             "second stop.",
                             a_req.key, texel[0], texel[1], texel[2], rough / 255.0f);
            }

            auto* const ourRd = new RendererData(static_cast<std::uint16_t>(made.Width),
                                                 static_cast<std::uint16_t>(made.Height));
            ourRd->texture      = dst;
            ourRd->resourceView = ourSrv;
            ourRd->unk1C        = static_cast<std::uint8_t>(made.MipLevels);
            ourRd->unk1D        = static_cast<std::uint8_t>(srgb ? 29 : 28);

            auto* const forged = Forge(src, ourRd, a_req.key);
            if (!forged) {
                spdlog::error("DyeTexture: could not allocate an NiSourceTexture for '{}'.",
                              a_req.key);
                delete ourRd;
                ourSrv->Release();
                dst->Release();
                return nullptr;
            }

            a_bytes = DyeQuality::ChainBytes(made.Width, made.Height, made.ArraySize);
            // ⚠ THE SOURCE SIZE AND THE LEVEL ARE BOTH PRINTED, because that is
            // what makes a cap readable from a log alone. "4096x4096 mip 3 to
            // 512x512" is a working cap; "4096x4096 mip 0 to 4096x4096" on a
            // preview is a cap that did nothing, and the two look identical from
            // the destination size on its own.
            spdlog::info("DyeTexture: built '{}' {} {}x{}{} from source {}x{} mip {} mips={} "
                         "srgb={} ~{} KiB{}",
                         a_req.key,
                         a_req.quality == DyeQuality::Quality::kCommit ? "COMMIT" : "preview",
                         made.Width, made.Height, cube ? " x6 cube" : "", sd.Width, sd.Height,
                         level.mip, made.MipLevels, srgb, a_bytes / 1024ull,
                         // ⚠ THE WINDOW IS PRINTED ON THE BUILD THAT USED IT. The
                         // first field run could not tell "the ramp never ran"
                         // from "the ramp ran and the window flattened it",
                         // because nothing said what curve the build was given.
                         a_req.ramp.mode != 0
                             ? fmt::format(" ramp mode={} stop2={:02X}{:02X}{:02X} window={:.3f}"
                                           "..{:.3f} key={}",
                                           a_req.ramp.mode, a_req.ramp.r2, a_req.ramp.g2,
                                           a_req.ramp.b2, p.rampLo, p.rampHi,
                                           !cube               ? "luminance(diffuse)"
                                           : p.rampAxis < 0.5f ? "elevation"
                                           : p.rampAxis < 1.5f ? "azimuth"
                                                               : "luminance")
                             : std::string{});
            return forged;
        }

        // ⚠ FREE THE PREVIEW ONCE ITS COMMIT HAS TAKEN OVER, AND DO IT
        // REGARDLESS OF THE BUDGET. Eviction only runs when the cache is over
        // its budget, so a retired preview would otherwise sit resident until
        // something unrelated pushed it over. If this does not work then this
        // whole unit has ADDED a cost rather than removed one, and the way to
        // know is the held figure in the log rather than reasoning about it.
        //
        // ⚠ THE REFCOUNT TEST IS EVICTION'S, FOR EVICTION'S REASON. A preview is
        // still on a material until the repaint the commit queued has actually
        // walked, so taking it then would be a use after free. Walking past and
        // trying again next frame is what makes this safe.
        void RetireLocked() {
            for (auto it = g_retired.begin(); it != g_retired.end();) {
                const auto found = g_cache.find(*it);
                if (found == g_cache.end()) {
                    // Already gone, most likely taken by an eviction sweep.
                    it = g_retired.erase(it);
                    continue;
                }
                if (found->second.tex && found->second.tex->GetRefCount() <= 1) {
                    g_bytes -= found->second.bytes;
                    ++g_stats.retired;
                    spdlog::info("DyeTexture: retired the preview of '{}', {:.1f} MiB, now "
                                 "holding {:.1f} MiB.",
                                 *it,
                                 static_cast<double>(found->second.bytes) / (1024.0 * 1024.0),
                                 static_cast<double>(g_bytes) / (1024.0 * 1024.0));
                    g_cache.erase(found);
                    g_order.erase(std::remove(g_order.begin(), g_order.end(), *it),
                                  g_order.end());
                    it = g_retired.erase(it);
                } else {
                    ++it;  // still on a material; the repaint has not landed yet
                }
            }
        }

        // Promote every texture whose colour has stopped changing.
        void PromoteLocked(std::uint32_t a_settleMs) {
            const auto now = NowMs();
            for (auto& [slot, st] : g_slots) {
                if (st.colour.empty() || !st.source) {
                    continue;
                }
                const auto previewKey = KeyOf(st.colour, DyeQuality::Quality::kPreview);
                const auto commitKey  = KeyOf(st.colour, DyeQuality::Quality::kCommit);
                if (!DyeQuality::ShouldUpgrade(
                        g_cache.count(previewKey) != 0, g_cache.count(commitKey) != 0,
                        st.commitQueued,
                        DyeQuality::HasSettled(now, st.changedAtMs, a_settleMs),
                        st.previewOnly)) {
                    continue;
                }
                g_pending.push_back(Request{ QueueSlotOf(slot, DyeQuality::Quality::kCommit),
                                             slot, commitKey, DyeQuality::Quality::kCommit,
                                             st.source, st.mask, st.maskDye, st.tint,
                                             st.ramp, st.blend, st.capPx, st.waiters });
                st.commitQueued = true;
            }
        }

    }  // namespace

    RE::NiSourceTexture* Acquire(RE::NiSourceTexture* a_source, const RE::NiColor& a_tint,
                                 RE::ActorHandle a_waiter, Blend a_blend, const Ramp& a_ramp,
                                 bool a_previewOnly, std::uint32_t a_capPx,
                                 RE::NiSourceTexture* a_mask, const MaskDye& a_maskDye,
                                 const char* a_sourceHint, const char* a_maskHint) {
        if (!a_source) {
            return nullptr;
        }
        // Acquire is called from the dye walk, which OutfitDye.h documents as
        // game thread only, so this is that side of the comparison.
        ReportThreads("game (Acquire)", g_gameTid);
        // ⚠ HERE RATHER THAN AT PLUGIN INIT, so a load order that never turns
        // rung 3 on never patches anything. By the time an actor is being dyed
        // the renderer is up, which is the only precondition.
        EnsurePresentHook();

        // ---- the IDENTITY, which is not always the texture's own name -------
        //
        // ⚠⚠ MEASURED 2026-08-13: an armour diffuse arrives here RESIDENT and
        // NAMELESS. Its material still knows the authored path, so the caller
        // passes that as a hint and the key is built from it. Without this the
        // whole armour walk was refused for fourteen hours.
        //
        // ⚠ THE TEXTURE'S OWN NAME WINS WHEN IT HAS ONE, so every key already
        // in the field is byte-identical and nothing cached is invalidated. The
        // hint only fills a hole.
        //
        // ⚠ AND THE HINT IS NOT CONSULTED FOR THE PLACEHOLDER TEST, which is
        // the whole reason the two questions are now separate predicates. A
        // shape can carry a perfectly good authored path in its material while
        // the engine's own default sits in the live slot: that IS the purple
        // eye. The placeholder test reads the RAW name and refuses on it; the
        // identity test reads the resolved one and refuses only if there is
        // nothing to key on at all.
        const auto identityOf = [](const RE::NiSourceTexture* a_tex, const char* a_hint) {
            const char* const own = a_tex ? a_tex->name.c_str() : nullptr;
            return (own && *own) ? own : a_hint;
        };
        const char* const srcName  = a_source->name.c_str();
        const char* const srcId    = identityOf(a_source, a_sourceHint);
        const char* const maskName = a_mask ? identityOf(a_mask, a_maskHint) : nullptr;
        const auto colour =
            ColourOf(srcId, a_tint, a_blend, a_ramp, a_capPx, maskName, a_maskDye);
        // ⚠ SlotOf STAYS RAMPLESS, DELIBERATELY. It identifies a source texture
        // and a blend, not a colour, and the settle clock is per source: changing
        // a dye's second stop is a colour change on the SAME slot, which is
        // exactly what the clock below has to see so it can restart. Putting the
        // ramp in here would give one texture two independent settle clocks and
        // let a flat and a ramped build of it queue against each other.
        const auto slot       = SlotOf(srcId, a_blend, a_capPx, maskName);
        const auto previewKey = KeyOf(colour, DyeQuality::Quality::kPreview);
        const auto commitKey  = KeyOf(colour, DyeQuality::Quality::kCommit);

        std::scoped_lock lock{ g_lock };

        // ---- the settle clock -------------------------------------------
        //
        // Stamped on a CHANGE and not on a request, because this runs on every
        // repaint and a repaint does not mean anybody moved the picker.
        auto& st = g_slots[slot];
        if (st.colour != colour) {
            // ⚠ A NEW COLOUR CANCELS THE OLD ONE'S QUEUED COMMIT. Settling on
            // blue and dragging away a frame later would otherwise spend a full
            // build on a colour nobody is looking at, which is the exact cost
            // this whole unit exists to remove. A commit already inside Pump is
            // left to finish: it is cheap to keep once built and cancelling it
            // there would mean reaching into a build in flight.
            if (st.commitQueued) {
                const auto queued = QueueSlotOf(slot, DyeQuality::Quality::kCommit);
                g_pending.erase(
                    std::remove_if(g_pending.begin(), g_pending.end(),
                                   [&](const Request& r) { return r.slot == queued; }),
                    g_pending.end());
            }
            st.colour       = colour;
            st.changedAtMs  = NowMs();
            st.commitQueued = false;
            st.waiters.clear();
            // A fresh colour takes the request's own word for it; the old
            // colour's history says nothing about this one.
            st.previewOnly = a_previewOnly;
        } else {
            // Same colour: sticky in the safe direction. One real request
            // makes the colour upgradeable for ever; a later hover cannot
            // take that back (spec 2026-08-09).
            st.previewOnly = st.previewOnly && a_previewOnly;
        }
        st.source  = RE::NiPointer<RE::NiSourceTexture>(a_source);
        st.mask    = RE::NiPointer<RE::NiSourceTexture>(a_mask);
        st.maskDye = a_maskDye;
        st.tint    = a_tint;
        // Refreshed on every request, not only on a change, for the same reason
        // the tint above is: this is what PromoteLocked will build the commit
        // from, and it has to describe the colour that is actually being asked
        // for right now.
        st.ramp   = a_ramp;
        st.blend  = a_blend;
        // In the slot key already, so this is only what PromoteLocked reads back
        // when it builds the commit request.
        st.capPx  = a_capPx;
        if (a_waiter &&
            std::find(st.waiters.begin(), st.waiters.end(), a_waiter) == st.waiters.end()) {
            st.waiters.push_back(a_waiter);
        }

        // ---- the best thing already built wins ---------------------------
        //
        // ⚠ COMMIT BEFORE PREVIEW, AND THAT ORDERING IS WHAT LANDS THE UPGRADE.
        // A completed build queues a repaint, the repaint walks again, and this
        // is the line that hands the sharper texture over. Without it the
        // material would keep the preview for ever, its refcount would never
        // fall to one, and the retire sweep would have nothing to free.
        if (const auto it = g_cache.find(commitKey); it != g_cache.end()) {
            ++g_stats.hits;
            return it->second.tex.get();
        }
        if (const auto it = g_cache.find(previewKey); it != g_cache.end()) {
            ++g_stats.hits;
            return it->second.tex.get();
        }
        // ---- a placeholder is not a source ------------------------------
        //
        // ⚠⚠ THIS IS THE PURPLE EYE, AND IT IS NOT AN EYE BUG. MEASURED off the
        // 2026-08-12 amber run: the log carries
        // `built 'BSShader_DefNormalMap|FFA500|ovl|c' COMMIT 16x16`, so a shape
        // reached here with the ENGINE'S OWN DEFAULT NORMAL MAP in its diffuse
        // slot. That texture is flat (128,128,255), and blue is a FIXED POINT of
        // the overlay `D*D + 2*T*D*(1-D)`, so D=1 comes back as 1 whatever the
        // tint is. Amber gives #C093FF and magenta gives #C040FF: both purple,
        // which is why two rounds with two different test colours produced the
        // same wrong answer and neither one looked like a clue.
        //
        // ⚠ THE SLOT IS TRANSIENT, WHICH IS WHY THE OLD BEHAVIOUR WAS SO HARD TO
        // READ. The eye's real 4096 texture was tinted correctly seconds
        // earlier in the same run; the head was then rebuilt and a later repaint
        // pass found the default sitting in the slot and swapped THAT in, where
        // it stayed. So the failure needs no bad targeting, no failed build and
        // no broken clone, and every instrument that asked "did a texture come
        // back" answered yes. missing-texture-resolves-to-a-placeholder.
        //
        // ⚠ REFUSING IS THE WHOLE FIX AND IT NEEDS NO TIMING. Null here means
        // "not yet" to the caller, which reports the slot pending and lets the
        // deferred repaint chain come back once the real texture has resolved.
        // An undyed shape for a frame beats a lavender one for ever, and that is
        // the fail-safe direction.
        //
        // The test itself is in DyeKey.h with the rest of this module's pure
        // logic, and tested there, for the reason that file records: nothing in
        // DyeTexture.cpp is compiled by any suite.
        //
        // ⚠ IN Acquire RATHER THAN IN THE DIFFUSE SWAP, because this is the one
        // funnel every tinted texture passes through. A placeholder is no more
        // dyeable as a cubemap, an envmap mask or a flake normal than it is as a
        // diffuse, and the armour walk can meet the same slot mid-rebuild that
        // the eye did.
        if (DyeKey::IsEnginePlaceholder(srcName)) {
            if (g_refused.insert(slot).second) {
                ++g_stats.refused;
                spdlog::warn("DyeTexture: '{}' is one of the engine's own placeholder "
                             "textures, not this shape's own, so there is nothing here "
                             "worth tinting; refused. The shape stays undyed until its "
                             "real texture resolves and the next repaint finds it.",
                             slot);
            }
            return nullptr;
        }
        // ⚠⚠ AND SEPARATELY, NOTHING TO KEY ON. This used to be folded into the
        // test above and that cost the field every armour dye for fourteen
        // hours: a resident, perfectly dyeable texture with an empty name was
        // read as one of the engine's defaults. The two questions are genuinely
        // different. "Is the engine's fallback sitting in this slot" is about
        // the RAW name and is a refusal on the merits. "Can this be keyed" is
        // about the RESOLVED identity and is a refusal about the CACHE, because
        // an unkeyable source collapses onto one entry per colour and blend and
        // two garments would be served each other's bytes.
        //
        // ⚠ REACHED ONLY WHEN THE CALLER HAD NO PATH EITHER. The material's own
        // texture set is consulted first, so this now means "neither the
        // texture nor its material knows what this is", which nothing in the
        // field has yet produced.
        if (DyeKey::HasNoIdentity(srcId)) {
            if (g_refused.insert(slot).second) {
                ++g_stats.refused;
                spdlog::warn("DyeTexture: a source texture arrived with no name and no "
                             "authored path from its material, so there is nothing to "
                             "key it by and two of them would share one cache entry; "
                             "refused. ptr={} rendererData={}",
                             static_cast<const void*>(a_source),
                             static_cast<const void*>(a_source->rendererTexture));
            }
            return nullptr;
        }
        // ⚠ AND THE MASK GETS THE SAME TEST, because the eye's NORMAL slot can
        // hold `BSShader_DefNormalMap` around a head rebuild exactly as the
        // diffuse slot held it on 2026-08-12. A placeholder's alpha is flat, so
        // a build keyed on it would cache "dye the whole eyeball" against a name
        // that vanishes when the real normal resolves; refusing here means the
        // next repaint asks with the real mask and gets the real answer. The
        // refusal is permanent per slot and harmless for the same reason the
        // source's is: the slot names the placeholder, and that pairing never
        // recurs once the slot resolves.
        // ⚠ THE MASK KEEPS BOTH TESTS, and that is not symmetry for its own
        // sake. An empty mask name MEANS "no mask" to DyeKey::MaskSuffix and to
        // ColourOf's mask-dye ternary, while the build itself keys off the mask
        // POINTER, so an unkeyable mask let through would write masked bytes
        // under an unmasked key with the sclera and split colours missing from
        // it. Two silent collisions, not one.
        if (a_mask && (DyeKey::IsEnginePlaceholder(a_mask->name.c_str()) ||
                       DyeKey::HasNoIdentity(maskName))) {
            if (g_refused.insert(slot).second) {
                ++g_stats.refused;
                spdlog::warn("DyeTexture: the mask of '{}' is one of the engine's own "
                             "placeholder textures, so the iris disc is not in it; "
                             "refused. The shape stays undyed until the real normal map "
                             "resolves and the next repaint finds it.",
                             slot);
            }
            return nullptr;
        }
        // ⚠ REFUSAL IS KEYED ON THE SOURCE, NOT ON THE COLOUR AND NOT ON THE
        // QUALITY. What the layout control rejects is a TEXTURE, and that
        // verdict does not become untrue because somebody picked a different
        // colour or asked for a smaller copy.
        if (g_refused.count(slot) != 0) {
            // Permanent. Reported once when it happened; silent here so a
            // refused texture cannot flood the log on every repaint.
            return nullptr;
        }
        ++g_stats.misses;

        // ---- queue the PREVIEW, always -----------------------------------
        //
        // ⚠ ONE PENDING BUILD PER SLOT PER QUALITY, NEWEST COLOUR WINS. See
        // Request::slot: this is what stops a drag from queueing every colour it
        // passes through. The quality is in the coalescing identity, so a queued
        // commit is a different slot and cannot be overwritten from here.
        // ⚠ FLAKE BUILDS SKIP THE PREVIEW AND BUILD AT COMMIT QUALITY DIRECTLY.
        // A capped preview computes its cells on the smaller grid, so its flakes
        // are effectively four texels wide when the commit's are one - which the
        // field run reported as "the sparkle is lower rez sometimes": it was the
        // 512 preview being seen before the commit landed. Flake textures are
        // one-off swaps, never dragged through a colour picker, so the
        // cheap-first rule that justifies previews does not apply to them.
        const auto buildQ =
            (a_blend == Blend::kFlake || a_blend == Blend::kFlakeNormal)
                ? DyeQuality::Quality::kCommit
                : DyeQuality::Quality::kPreview;
        const auto buildKey  = KeyOf(colour, buildQ);
        const auto queueSlot = QueueSlotOf(slot, buildQ);
        const auto at = std::find_if(g_pending.begin(), g_pending.end(),
                                     [&](const Request& r) { return r.slot == queueSlot; });
        Request*   req{ nullptr };
        if (at != g_pending.end()) {
            if (at->key != buildKey) {
                ++g_stats.superseded;
                at->key    = buildKey;
                at->tint   = a_tint;
                // ⚠ THE RAMP MOVES WITH THE TINT. The key carries it, so a ramp
                // change is what put us in this branch; leaving the old one on
                // the request would build the new key's entry out of the
                // previous dye's second stop, and the cache would then serve
                // that as the new dye for as long as the entry lived.
                at->ramp   = a_ramp;
                at->source = RE::NiPointer<RE::NiSourceTexture>(a_source);
                // The mask and its dye move with the source for the reason the
                // ramp moves with the tint: this request is being rewritten to
                // a new key, and the build must read the inputs that key names.
                at->mask    = RE::NiPointer<RE::NiSourceTexture>(a_mask);
                at->maskDye = a_maskDye;
            }
            req = &*at;
        } else if (g_inFlight.count(buildKey) == 0) {
            g_pending.push_back(Request{ queueSlot, slot, buildKey, buildQ,
                                         RE::NiPointer<RE::NiSourceTexture>(a_source),
                                         RE::NiPointer<RE::NiSourceTexture>(a_mask),
                                         a_maskDye, a_tint, a_ramp, a_blend, a_capPx, {} });
            req = &g_pending.back();
            // A direct commit IS the commit; the settle sweep must not queue a
            // second one for this colour.
            if (buildQ == DyeQuality::Quality::kCommit) {
                st.commitQueued = true;
            }
        }
        // Join the waiting list. A second actor arriving after the first is
        // exactly the case that needs it, and a superseded request keeps the
        // waiters it already had.
        if (req && a_waiter &&
            std::find(req->waiters.begin(), req->waiters.end(), a_waiter) ==
                req->waiters.end()) {
            req->waiters.push_back(a_waiter);
        }
        return nullptr;
    }

    void Pump() {
        ReportThreads("render (Pump)", g_renderTid);
        // ⚠ READ ONCE PER PUMP, NOT PER BUILD. They are singleton fields behind
        // a lock-free read, and neither can meaningfully change inside one
        // frame's worth of builds.
        const auto& cfg = Settings::GetSingleton();
        const std::uint64_t budgetBytes =
            static_cast<std::uint64_t>(cfg.dyeTextureBudgetMiB) * 1024ull * 1024ull;
        const auto settleMs = cfg.dyeSettleMs;

        std::vector<Request> batch;
        {
            std::scoped_lock lock{ g_lock };
            // ⚠ BOTH SWEEPS RUN BEFORE THE EMPTY-QUEUE RETURN. A retired preview
            // is normally waiting on a repaint that has not walked yet, which
            // means the queue IS empty on the frame it becomes freeable, and a
            // colour settles precisely because nothing new is being queued.
            // Putting either after the return would strand it.
            RetireLocked();
            PromoteLocked(settleMs);
            if (g_pending.empty()) {
                return;
            }
            // See the header: bounded rather than drained, so a whole outfit's
            // worth of first-time colours cannot land in one frame.
            constexpr std::size_t kPerFrame = 4;
            const auto            take = std::min(kPerFrame, g_pending.size());
            batch.assign(g_pending.begin(), g_pending.begin() + take);
            g_pending.erase(g_pending.begin(), g_pending.begin() + take);
            for (const auto& r : batch) {
                g_inFlight.insert(r.key);
            }
        }

        auto* const rm = RE::BSRenderManager::GetSingleton();
        if (!rm) {
            return;
        }
        auto& rt = rm->GetRuntimeData();
        auto* const device = rt.forwarder;
        auto* const ctx    = rt.context;
        if (!device || !ctx || !EnsureShader(device)) {
            // Put them back rather than dropping them: a device that is not up
            // yet is a "not now", and g_queued still names them so nothing
            // re-enqueues a duplicate meanwhile.
            std::scoped_lock lock{ g_lock };
            for (const auto& r : batch) {
                g_inFlight.erase(r.key);
            }
            g_pending.insert(g_pending.begin(), batch.begin(), batch.end());
            return;
        }

        for (const auto& req : batch) {
            bool          refused{ false };
            std::uint64_t bytes{ 0 };
            // ⚠ THE BUILD RUNS OUTSIDE THE LOCK. It dispatches compute and can
            // block on the driver, and Acquire is called from the game thread on
            // every dyed shape of every repaint.
            auto* const built = Build(device, ctx, req, refused, bytes);

            std::scoped_lock lock{ g_lock };
            g_inFlight.erase(req.key);
            if (built) {
                g_cache.emplace(req.key, Entry{ RE::NiPointer<RE::NiSourceTexture>(built),
                                                bytes });
                g_order.push_back(req.key);
                g_bytes += bytes;
                g_stats.bytes = g_bytes;
                g_stats.held  = g_cache.size();
                if (req.quality == DyeQuality::Quality::kCommit) {
                    ++g_stats.commitBuilds;
                    // ⚠ THE PREVIEW IS RETIRED HERE AND FREED LATER, AND THE GAP
                    // BETWEEN THE TWO IS A REPAINT. Its refcount is still two
                    // until the material re-points, so this only records the
                    // intent and RetireLocked does the freeing once the count
                    // says it is safe.
                    const auto previewKey =
                        req.key.substr(0, req.key.size() - 1) +
                        QualityTag(DyeQuality::Quality::kPreview);
                    if (g_cache.count(previewKey) != 0 &&
                        std::find(g_retired.begin(), g_retired.end(), previewKey) ==
                            g_retired.end()) {
                        g_retired.push_back(previewKey);
                    }
                } else {
                    ++g_stats.previewBuilds;
                }
                // ⚠ AFTER THE INSERT, NOT BEFORE. The entry that just arrived is
                // about to be handed to a material, so it has a refcount of one
                // and would be the first thing an eviction sweep took. Inserting
                // first and sweeping second lets the older, genuinely unused
                // entries go instead.
                EvictLocked(budgetBytes, req.key);
                // ⚠ NOTIFY, NOT POLL, AND THIS LINE IS THE WHOLE FIX. Without
                // it the swap depends on the deferred chain happening to walk
                // again after the build, and the field run showed it expiring
                // first: a swatch click reverted the piece to its own colours
                // and left it there. The restore that precedes every repaint
                // had already run, so "not yet" looks exactly like "undyed".
                //
                // ⚠ QUEUED TO THE GAME THREAD. QueueRepaint resolves an actor
                // handle and its task body walks live 3D, and this is the render
                // thread.
                if (auto* const task = SKSE::GetTaskInterface()) {
                    for (const auto h : req.waiters) {
                        task->AddTask([h] { OutfitDye::QueueRepaint(h); });
                    }
                }
                spdlog::info("DyeTexture: '{}' is ready; {} actor(s) asked to repaint.",
                             req.key, req.waiters.size());
                // ⚠ THE CHURN CONTROL, AND IT IS A COUNT RATHER THAN A PICTURE.
                // Every commit gets a line because a working split makes them
                // rare, and previews get one every 32 because a working split
                // makes them common. A session whose commit count approaches its
                // preview count is a split that is not working, whatever the
                // screen shows.
                if (req.quality == DyeQuality::Quality::kCommit ||
                    g_stats.previewBuilds % 32 == 0) {
                    LogStatsLocked(req.quality == DyeQuality::Quality::kCommit ? "commit"
                                                                              : "churn");
                }
            } else if (refused) {
                // ⚠ KEYED ON THE SOURCE. The layout control rejects a TEXTURE,
                // and picking a different colour does not make it acceptable.
                g_refused.insert(req.owner);
                ++g_stats.refused;
            } else {
                // Not refused, not built: an allocation failure. Left out of
                // g_refused deliberately, so a transient out-of-memory can
                // succeed on a later pass rather than being written off. The
                // slot's commit flag is cleared so the settle sweep tries again
                // rather than believing one is still queued for ever.
                if (req.quality == DyeQuality::Quality::kCommit) {
                    if (const auto s = g_slots.find(req.owner); s != g_slots.end()) {
                        s->second.commitQueued = false;
                    }
                }
                ++g_stats.failed;
            }
        }
    }

    void EnsurePresent() {
        EnsurePresentHook();
    }

    Stats GetStats() {
        std::scoped_lock lock{ g_lock };
        Stats s   = g_stats;
        s.held    = g_cache.size();
        s.queued  = g_pending.size();
        s.bytes   = g_bytes;
        return s;
    }

    namespace {
        // ⚠ THE CALLER ALREADY HOLDS g_lock. GetStats takes it, so calling the
        // public LogStats from inside Pump's per-request loop would deadlock on
        // a non-recursive mutex.
        void LogStatsLocked(const char* a_where) {
            spdlog::info("DyeTexture[{}]: held={} ({:.1f} MiB) queued={} hits={} misses={} "
                         "preview={} commit={} retired={} superseded={} evicted={} refused={} "
                         "failed={}",
                         a_where, g_cache.size(),
                         static_cast<double>(g_bytes) / (1024.0 * 1024.0), g_pending.size(),
                         g_stats.hits, g_stats.misses, g_stats.previewBuilds,
                         g_stats.commitBuilds, g_stats.retired, g_stats.superseded,
                         g_stats.evicted, g_stats.refused, g_stats.failed);
        }
    }  // namespace

    void LogStats(const char* a_where) {
        std::scoped_lock lock{ g_lock };
        LogStatsLocked(a_where);
    }

}  // namespace OS::DyeTexture
