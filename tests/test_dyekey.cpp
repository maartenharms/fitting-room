#include "../src/DyeKey.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(x)                                                              \
    do {                                                                      \
        if (!(x)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #x);          \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

int main() {
    using namespace OS::DyeKey;

    {  // A flat dye's suffix is empty, so every texture cached before modes
        // existed keeps its exact key and the cache is not invalidated wholesale
        // by shipping this. Whatever gloss a flat dye carries is irrelevant to
        // its texture (no marker mip is written), so it must not fragment the
        // key either.
        CHECK(RampSuffix(0, false, 0, 0, 0, 128).empty());
        CHECK(RampSuffix(0, false, 0, 0, 0, 200).empty());
    }
    {  // ⚠ THE COLLISION THIS EXISTS TO PREVENT. Same primary hex, different
        // second stop, must not share a cache entry. Without this the second dye
        // silently renders as the first, forever, because the entry is cached.
        CHECK(RampSuffix(1, true, 0xC9, 0xA0, 0xD8, 128) !=
              RampSuffix(1, true, 0x20, 0x90, 0x40, 128));
    }
    {  // Same stops, different mode, also distinct: nacre and iridescent write
        // different bytes to different targets.
        CHECK(RampSuffix(1, true, 0xC9, 0xA0, 0xD8, 128) !=
              RampSuffix(2, true, 0xC9, 0xA0, 0xD8, 128));
    }
    {  // ⚠ GLOSS IS IN THE KEY FOR A RAMPED DYE, because it is baked into the
        // texture: the marker mip's alpha is the roughness Community Shaders
        // reads back. Two pearls sharing stops and differing in gloss are two
        // different textures, and a key that cannot tell them apart hands the
        // matte one's reflection to the glossy one.
        CHECK(RampSuffix(2, true, 0xC9, 0xA0, 0xD8, 200) !=
              RampSuffix(2, true, 0xC9, 0xA0, 0xD8, 128));
    }
    {  // secondSet false ignores the stop bytes entirely, so a channel carrying
        // stale r2/g2/b2 under secondSet=false cannot fragment the cache.
        CHECK(RampSuffix(0, false, 0xFF, 0x00, 0x00, 128) ==
              RampSuffix(0, false, 0x00, 0xFF, 0x00, 128));
    }
    {  // Identical inputs are identical keys, or nothing is ever a cache hit.
        CHECK(RampSuffix(2, true, 1, 2, 3, 128) == RampSuffix(2, true, 1, 2, 3, 128));
    }
    {  // ⚠ A MODE WITH NO SECOND STOP IS STILL NOT FLAT, and it has to key
        // apart from flat. mode 1 with secondSet false is what a dye authored
        // with "mode": "nacre" and no hex2 produces: the ramp runs between the
        // colour and itself, which paints flat, but the SHADER takes a different
        // branch to get there. Sharing a cache entry with a genuinely flat dye
        // would be right today and wrong the moment the two branches diverge.
        CHECK(!RampSuffix(1, false, 0, 0, 0, 128).empty());
        CHECK(RampSuffix(1, false, 0, 0, 0, 128) != RampSuffix(2, false, 0, 0, 0, 128));
    }
    {  // ⚠ THE SUFFIX NEVER CONTAINS THE SEPARATOR THAT SPLITS THE KEY FROM ITS
        // QUALITY TAG at the end. DyeTexture retires a preview by rewriting the
        // LAST character of the commit's key, so anything here that looked like
        // a trailing "|p" or "|c" would derive a key matching nothing and leave
        // the preview resident for ever.
        const auto s = RampSuffix(2, true, 0xC9, 0xA0, 0xD8, 128);
        CHECK(!s.empty());
        CHECK(s.back() != 'p' && s.back() != 'c');
    }
    {  // Every stop byte reaches the key. A formatter that dropped one would
        // collide two dyes differing only in that channel, which is the same
        // silent fault one level down.
        CHECK(RampSuffix(1, true, 0x11, 0x00, 0x00, 128) != RampSuffix(1, true, 0x00, 0x00, 0x00, 128));
        CHECK(RampSuffix(1, true, 0x00, 0x11, 0x00, 128) != RampSuffix(1, true, 0x00, 0x00, 0x00, 128));
        CHECK(RampSuffix(1, true, 0x00, 0x00, 0x11, 128) != RampSuffix(1, true, 0x00, 0x00, 0x00, 128));
    }
    {  // ⚠ FIXED WIDTH PER CHANNEL, so no two distinct stop triples can spell
        // the same text. Without the %02X a stop of (0x1, 0x11, 0x1) and one of
        // (0x11, 0x1, 0x11) both render as "1111 1", and the cache hands one
        // dye's texture to the other.
        CHECK(RampSuffix(1, true, 0x01, 0x11, 0x01, 128) != RampSuffix(1, true, 0x11, 0x01, 0x11, 128));
    }
    {  // The whole byte range survives the format, including the top bit, which
        // a signed char passed to a %X would print as eight F's.
        CHECK(RampSuffix(1, true, 0xFF, 0xFF, 0xFF, 128) != RampSuffix(1, true, 0x7F, 0x7F, 0x7F, 128));
        CHECK(RampSuffix(255, true, 0xFF, 0xFF, 0xFF, 128).size() < 24);
    }

    {  // ⚠⚠ THE PURPLE EYE. The engine's own default normal map reached the
        // tint shader as a DIFFUSE and came back purple for every colour tried,
        // because it is flat blue and blue is a fixed point of the overlay.
        // This is the exact name the 2026-08-12 log recorded.
        CHECK(IsEnginePlaceholder("BSShader_DefNormalMap"));
        // The whole family, not the one instance that was caught. The prefix is
        // the engine's and a size floor was refused as a guess.
        CHECK(IsEnginePlaceholder("BSShader_DefDiffuseMap"));
        CHECK(IsEnginePlaceholder("BSShader_"));
    }
    {  // ⚠⚠ A NAMELESS SOURCE IS NOT A PLACEHOLDER, AND THIS BLOCK USED TO SAY
        // IT WAS. That cost the field every armour dye for fourteen hours on
        // 2026-08-13: armour diffuses arrive RESIDENT and nameless, and folding
        // "has no name" into "is one of the engine's defaults" refused all of
        // them. A null name is still refused, because there is no string to
        // reason about at all; an EMPTY one is now a question for HasNoIdentity
        // below, asked only after the caller has offered the material's own
        // authored path.
        CHECK(IsEnginePlaceholder(nullptr));
        CHECK(!IsEnginePlaceholder(""));
    }
    {  // The cache half of the old clause, kept whole and moved. An unkeyable
        // source still has to be refused: every one of them collapses onto a
        // single cache key, so two would each be served the other's tinted
        // twin. What changed is WHEN the question is asked, not the answer.
        CHECK(HasNoIdentity(nullptr));
        CHECK(HasNoIdentity(""));
        // A resolved identity passes, whether it came from the texture or from
        // the material standing in for it.
        CHECK(!HasNoIdentity("textures\\armor\\iron\\f\\CuirassPlate.dds"));
        // ⚠ AND IT ASKS NOTHING ABOUT PLACEHOLDERS. The two predicates are
        // deliberately independent: a placeholder HAS an identity and is still
        // refused, by the other test. Folding them back together is the bug.
        CHECK(!HasNoIdentity("BSShader_DefNormalMap"));
    }
    {  // ⚠ AND AN AUTHORED TEXTURE MUST STILL PASS, which is the half that
        // makes the rule a rule rather than a switch. These are the real paths
        // from the run that found the fault; refusing any of them would trade a
        // purple eye for an undyeable one.
        CHECK(!IsEnginePlaceholder(
            "textures\\actors\\Character\\eyes\\wDemonEyes\\UBE\\wbUbeDemonEye04.dds"));
        CHECK(!IsEnginePlaceholder("textures\\Actors\\Character\\Eyes\\EyeDarkElf.dds"));
        CHECK(!IsEnginePlaceholder("textures\\dlc01\\cubemaps\\dg_cubemap.dds"));
        // Nothing about the prefix test is case- or substring-loose: a path that
        // merely CONTAINS the word must not be refused, and only a leading match
        // counts.
        CHECK(!IsEnginePlaceholder("textures\\mymod\\BSShader_lookalike.dds"));
        CHECK(!IsEnginePlaceholder("bsshader_deftexture"));
    }

    {  // No mask, no suffix, so every texture cached before iris masks existed
        // keeps the exact key it already had. An empty name means "no mask"
        // here because Acquire refuses nameless masks before keys are built.
        CHECK(MaskSuffix(nullptr).empty());
        CHECK(MaskSuffix("").empty());
    }
    {  // ⚠ THE COLLISION THE MASK SUFFIX EXISTS TO PREVENT. Two eyes sharing a
        // diffuse and differing in their normal are two different masked
        // builds; a key that cannot tell them apart serves each the other's
        // bytes, silently, for as long as the entry lived.
        CHECK(MaskSuffix("textures\\!COR\\Eyes_ILV\\eye01_n.dds") !=
              MaskSuffix("textures\\actors\\character\\eyes\\wb_ube_n.dds"));
        CHECK(!MaskSuffix("textures\\!COR\\Eyes_ILV\\eye01_n.dds").empty());
    }
    {  // Identical masks are identical suffixes, or a masked build is never a
        // cache hit.
        CHECK(MaskSuffix("eye01_n.dds") == MaskSuffix("eye01_n.dds"));
    }

    {  // Iris-only is the empty suffix: every masked key cached before the
        // sclera existed survives byte for byte.
        CHECK(MaskDyeSuffix(true, false, 0, 0, 0).empty());
        CHECK(MaskDyeSuffix(true, false, 200, 40, 90).empty());
    }
    {  // A sclera colour fragments the key, and two different sclera colours
        // are two different suffixes: the silent-collision rule again.
        CHECK(!MaskDyeSuffix(true, true, 200, 40, 90).empty());
        CHECK(MaskDyeSuffix(true, true, 200, 40, 90) !=
              MaskDyeSuffix(true, true, 200, 40, 91));
    }
    {  // ⚠ "DYE MY IRIS BLACK" AND "LEAVE MY IRIS ALONE" ARE DIFFERENT BYTES.
        // An iris-off build carries a canonical black tint in the key ahead of
        // this suffix, and black is a colour a player can pick, so the iris-off
        // marker has to live here or the two collide.
        CHECK(MaskDyeSuffix(true, true, 200, 40, 90) !=
              MaskDyeSuffix(false, true, 200, 40, 90));
    }
    {  // Fixed width per channel, the same trap RampSuffix pins.
        CHECK(MaskDyeSuffix(true, true, 0x01, 0x11, 0x01) !=
              MaskDyeSuffix(true, true, 0x11, 0x01, 0x11));
    }
    {  // ⚠ AN ENVMASK BUILD IS NEVER AN EYE BUILD (2026-09-04). The metal
        // modes read a shape's environment mask through its RED channel with
        // an absolute window, where the eye path reads a normal map's alpha
        // against its own mean. Same mask name, same tint, different picture,
        // so the suffix is never empty, and the window is inside it because an
        // INI edit to the window changes the picture too.
        CHECK(!EnvMaskSuffix(0.15f, 0.45f).empty());
        CHECK(EnvMaskSuffix(0.15f, 0.45f) != EnvMaskSuffix(0.16f, 0.45f));
        CHECK(EnvMaskSuffix(0.15f, 0.45f) != EnvMaskSuffix(0.15f, 0.46f));
        // Fixed width, the RampSuffix collision rule.
        CHECK(EnvMaskSuffix(0.01f, 0.11f) != EnvMaskSuffix(0.11f, 0.01f));
        // ⚠ THE CHANNEL IS IN IT. A shape with no environment mask falls back
        // to its normal map's alpha, the channel the engine reads for its
        // reflection strength when no mask is bound. The same texture read
        // through red and through alpha is two pictures, so two keys.
        CHECK(EnvMaskSuffix(0.5f, 0.12f, true) != EnvMaskSuffix(0.5f, 0.12f, false));
        CHECK(EnvMaskSuffix(0.5f, 0.12f) == EnvMaskSuffix(0.5f, 0.12f, false));
        // ⚠ AND THE CUT (2026-09-04), the per-piece "Metal starts" byte. Two
        // cuts on one map are two pictures, and the default spelled out is
        // the same key as the default left unsaid, so no entry built before
        // the byte existed moves.
        CHECK(EnvMaskSuffix(0.5f, 0.12f, true, 51) != EnvMaskSuffix(0.5f, 0.12f, true, 128));
        CHECK(EnvMaskSuffix(0.5f, 0.12f, true) == EnvMaskSuffix(0.5f, 0.12f, true, 128));
        CHECK(EnvMaskSuffix(0.5f, 0.12f, false, 51) != EnvMaskSuffix(0.5f, 0.12f, true, 51));
        CHECK(EnvMaskSuffix(0.5f, 0.12f, false, 0) != EnvMaskSuffix(0.5f, 0.12f, false, 255));
    }

    std::printf("DyeKeyTests: all passed\n");
    return 0;
}
