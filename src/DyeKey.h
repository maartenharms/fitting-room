#pragma once

// The part of a tinted texture's cache key that describes the RAMP.
//
// ⚠ PURE AND TESTED BECAUSE THE FAULT IT PREVENTS IS INVISIBLE. Two dyes
// sharing a primary hex and differing only in their second stop would collide in
// DyeTexture's cache and the second would render as the first. That is silent,
// cached and persistent, and no screenshot distinguishes it from an authoring
// mistake. DyeTexture.cpp is not compiled by any test, so the arithmetic lives
// here instead.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

namespace OS::DyeKey {

    // Is this source one of the engine's own built-in textures rather than the
    // shape's?
    //
    // ⚠⚠ THE PURPLE EYE (2026-08-12). MEASURED from that run's own log:
    // `built 'BSShader_DefNormalMap|FFA500|ovl|c' COMMIT 16x16`. The engine's
    // default normal map is flat (128,128,255), and blue is a FIXED POINT of the
    // overlay `D*D + 2*T*D*(1-D)`, so D=1 returns 1 for every tint there is.
    // Amber came out #C093FF and magenta #C040FF. Both purple, which is why two
    // rounds with two deliberately different test colours produced the same
    // wrong answer and the second one looked like a confirmation of the first.
    //
    // ⚠ NAME, NOT SIZE. 16x16 is what that one happened to be; a size floor
    // would be a guess and would eventually refuse somebody's small mask.
    // `BSShader_` is the engine's own prefix for its built-in textures, and an
    // authored texture cannot collide with it because an authored one is named
    // by its path.
    //
    // ⚠ AND AN UNNAMED SOURCE, for a second reason that stands on its own:
    // DyeTexture keys both its cache and its refusals on this name, and a
    // nameless source collapses onto one shared key. Two of them would each be
    // served the other's tinted twin, silently and for as long as the entry
    // lived. cache-key-must-be-the-inputs-not-the-subject.
    //
    // ⚠ NOT A FAILURE, AND THE CALLER MUST NOT TREAT IT AS ONE. The right answer
    // to a placeholder is to leave the shape undyed and come back, because the
    // slot is TRANSIENT: the eye's real 4096 texture tinted correctly seconds
    // before the head rebuilt and put the default in its place. Undyed for a
    // frame beats lavender for ever.
    // ⚠⚠ THE EMPTY-NAME CLAUSE LEFT THIS PREDICATE ON 2026-08-13, AND REMOVING
    // IT FIXED A FIELD BUG RATHER THAN CAUSING ONE. It shipped here at 8638557
    // (00:11) and by 14:15 the field had measured that NO ARMOUR DYES AT ALL:
    // every armour diffuse reaches the funnel with an EMPTY name, so this
    // refused all of them. The probe proved those textures are real and
    // resident (rendererData non-null, one stable address per shape); they are
    // simply nameless.
    //
    // ⚠ THE PURPLE EYE IS UNAFFECTED, and that is measured, not assumed. Its
    // texture was NAMED: the log line preserved in DyeTexture.cpp and in
    // docs/STATUS.md is `built 'BSShader_DefNormalMap|FFA500|ovl|c'`, which the
    // strncmp below refuses on its own. The empty clause was never load
    // bearing for it; the header itself said so, calling it "a second reason
    // that stands on its own".
    //
    // That second reason was real and is now answered elsewhere: a nameless
    // source cannot be keyed, so two of them would each be served the other's
    // tinted twin. HasNoIdentity below is that check, and it runs on the
    // identity AFTER a caller has had the chance to supply the authored path.
    [[nodiscard]] inline bool IsEnginePlaceholder(const char* a_name) {
        return !a_name || std::strncmp(a_name, "BSShader_", 9) == 0;
    }

    // Nothing to key on. Refusing is not about the texture being unusable, it
    // is about the CACHE: an unkeyable source collapses onto one entry per
    // colour and blend, so two garments dyed one colour would be served each
    // other's bytes. cache-key-must-be-the-inputs-not-the-subject.
    //
    // ⚠ RUN THIS ON THE RESOLVED IDENTITY, NEVER ON THE RAW TEXTURE NAME. A
    // nameless texture whose material still carries its authored path HAS an
    // identity; refusing it on the raw name is exactly the bug above.
    [[nodiscard]] inline bool HasNoIdentity(const char* a_identity) {
        return !a_identity || !*a_identity;
    }

    // Empty for a flat dye, which is what keeps every texture cached before
    // modes existed on exactly the key it already had.
    //
    // ⚠ IT APPENDS TO THE COLOUR AND THE QUALITY TAG COMES AFTER, so nothing
    // here may end in a bare "p" or "c". DyeTexture retires a preview by
    // rewriting the LAST character of the commit's key, and a suffix that looked
    // like a quality tag would derive a key matching nothing and leave the
    // preview resident for the process. The hex digits keep it safe by
    // construction, and a test pins it anyway.
    [[nodiscard]] inline std::string RampSuffix(std::uint8_t a_mode, bool a_secondSet,
                                                std::uint8_t a_r2, std::uint8_t a_g2,
                                                std::uint8_t a_b2, std::uint8_t a_gloss) {
        if (a_mode == 0 && !a_secondSet) {
            return {};
        }
        char buf[28]{};
        // ⚠ THE STOP BYTES ARE OMITTED WHEN secondSet IS FALSE. A channel can
        // carry stale r2/g2/b2 underneath a cleared flag, and letting those
        // reach the key would split one cache entry into several that all
        // render identically.
        //
        // ⚠ AND %02X RATHER THAN %X, WHICH IS NOT COSMETIC. Variable width lets
        // two distinct stop triples spell one string: (0x01, 0x11, 0x01) and
        // (0x11, 0x01, 0x11) both render as "1111 1" without the padding, and
        // the cache then hands one dye's texture to the other. That is the exact
        // collision this whole file exists to close, reintroduced one level down.
        //
        // ⚠ GLOSS JOINS THE KEY THE MOMENT A RAMP DOES, because for a ramped dye
        // it is baked into the TEXTURE: the marker mip's alpha is the roughness
        // Community Shaders decodes back out. It stays OUT of the flat key
        // above, where no marker mip is ever written and a gloss difference
        // changes nothing about the bytes.
        if (!a_secondSet) {
            std::snprintf(buf, sizeof buf, "|m%u:g%02X", static_cast<unsigned>(a_mode),
                          static_cast<unsigned>(a_gloss));
        } else {
            std::snprintf(buf, sizeof buf, "|m%u:%02X%02X%02X:g%02X",
                          static_cast<unsigned>(a_mode), static_cast<unsigned>(a_r2),
                          static_cast<unsigned>(a_g2), static_cast<unsigned>(a_b2),
                          static_cast<unsigned>(a_gloss));
        }
        return std::string(buf);
    }

    // The part of the key that names the MASK, when a build reads one. An eye's
    // iris-masked diffuse is built from TWO textures: the diffuse it recolours
    // and the normal map whose alpha says where the iris is.
    //
    // ⚠ THE KEY MUST CARRY BOTH INPUTS OR TWO EYES SHARING A DIFFUSE AND
    // DIFFERING IN THEIR NORMAL WOULD COLLIDE, and each would be served the
    // other's bytes, silently, for as long as the entry lived. That is the same
    // fault class RampSuffix above exists to prevent: the key is the INPUTS of
    // the build, never just its subject.
    //
    // Empty with no mask, so every texture cached before masks existed keeps
    // the exact key it already had. Same rule as RampSuffix and the cap suffix,
    // for the same reason.
    //
    // ⚠ AN EMPTY NAME IS "NO MASK" HERE, so the caller must refuse a nameless
    // or placeholder mask BEFORE building keys; letting one through would key a
    // masked build as unmasked. Acquire does that with IsEnginePlaceholder,
    // which already treats an empty name as a placeholder.
    [[nodiscard]] inline std::string MaskSuffix(const char* a_maskName) {
        if (!a_maskName || !*a_maskName) {
            return {};
        }
        return std::string("|k") + a_maskName;
    }

    // The derived iris disc's own settings, for the same reason every other
    // suffix here exists: they change the PICTURE while the mask, the tint and
    // the blend all stay identical, so a key without them serves the old disc
    // forever after an INI edit.
    //
    // ⚠ EMPTY AT THE SHIPPED DEFAULTS, so every eye already cached keeps the
    // key it has. The default pair is the measured one (0.24 of the marked
    // region's short side, feathered a quarter either way, a mask over a
    // quarter of the texture counting as a region), and an install that never
    // touches the INI never pays a byte for this.
    //
    // ⚠ FIXED WIDTH, RampSuffix's collision rule: two distinct triples must not
    // be able to spell one string.
    [[nodiscard]] inline std::string IrisDiscSuffix(float a_radius, float a_soft,
                                                    float a_broad) {
        if (a_radius == 0.24f && a_soft == 0.25f && a_broad == 0.25f) {
            return {};
        }
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "|i%03d%03d%03d",
                      static_cast<int>(a_radius * 100.0f) % 1000,
                      static_cast<int>(a_soft * 100.0f) % 1000,
                      static_cast<int>(a_broad * 100.0f) % 1000);
        return buf;
    }

    // The part of a masked key that says WHICH SIDE of the iris disc takes
    // colour, and what the far side takes. A masked build now has up to two
    // tints: the request's own (the iris) and the sclera's, carried here.
    //
    // ⚠ EMPTY FOR IRIS-ONLY, which is the exact shape every masked build had
    // before the sclera existed, so every key already cached survives. Same
    // rule as every suffix in this file.
    //
    // ⚠ %02X FIXED WIDTH, the same collision RampSuffix closes: variable width
    // lets two distinct triples spell one string.
    //
    // ⚠ THE IRIS-OFF MARKER IS PART OF THE SUFFIX, not implied by the tint hex
    // in the key ahead of it. An iris-off build carries a canonical black tint
    // there, and black is also a colour a player can pick, so without ":ni"
    // the two would collide: "dye my iris black" and "leave my iris alone,
    // dye the sclera" are different bytes.
    // ⚠ THE PER-EYE SPLIT IS PART OF THE SUFFIX TOO (2026-08-13), for the rule
    // this whole file exists for: a split build paints the high-u half a second
    // colour, so a split and an unsplit build of one diffuse under one tint are
    // different pictures and must be different keys. "|e" plus the right eye's
    // hex; absent when unsplit, so every cached unsplit key survives.
    [[nodiscard]] inline std::string MaskDyeSuffix(bool a_irisSet, bool a_scleraSet,
                                                   std::uint8_t a_r, std::uint8_t a_g,
                                                   std::uint8_t a_b, bool a_splitSet = false,
                                                   std::uint8_t a_r2 = 0,
                                                   std::uint8_t a_g2 = 0,
                                                   std::uint8_t a_b2 = 0) {
        std::string out;
        if (!(a_irisSet && !a_scleraSet)) {
            char buf[20]{};
            if (a_scleraSet) {
                std::snprintf(buf, sizeof buf, "|s%02X%02X%02X%s",
                              static_cast<unsigned>(a_r), static_cast<unsigned>(a_g),
                              static_cast<unsigned>(a_b), a_irisSet ? "" : ":ni");
            } else {
                // Neither side coloured: nothing a caller should ever ask for,
                // but a distinct spelling beats colliding with iris-only.
                std::snprintf(buf, sizeof buf, "|s-:ni");
            }
            out = buf;
        }
        if (a_splitSet) {
            char buf[12]{};
            std::snprintf(buf, sizeof buf, "|e%02X%02X%02X", static_cast<unsigned>(a_r2),
                          static_cast<unsigned>(a_g2), static_cast<unsigned>(a_b2));
            out += buf;
        }
        return out;
    }

    // The envmask window, in the key for the reason IrisDiscSuffix is: it
    // changes the PICTURE while the mask, the tint and the blend all stay
    // identical, so a key without it would serve the old split forever after
    // an INI edit.
    //
    // ⚠ NEVER EMPTY, unlike every suffix above, and that is the point rather
    // than an oversight. An envmask build (2026-09-04) reads the mask's RED
    // channel against an absolute window; the eye path reads a normal map's
    // ALPHA against its own mean. Same mask name, same tint, different picture,
    // and the feature is new, so there is no cached key to preserve by staying
    // silent at the defaults.
    //
    // ⚠ FIXED WIDTH AND NO MODULO, RampSuffix's collision rule: a scalar of
    // exactly 1.0 must not spell the same as 0.0. Thousandths, clamped. The
    // two scalars are the feather and the one-material floor; the cut itself
    // is derived from the mask and needs no place here.
    //
    // ⚠ AND THE CHANNEL. A shape with no environment mask bound is read
    // through its normal map's alpha, the channel the engine falls back to for
    // its reflection strength. One texture read through red and through alpha
    // is two pictures, so ":r" marks the red read.
    //
    // ⚠ AND THE CUT (2026-09-04), where metal starts on the piece, spelled
    // ALWAYS and fixed width: two cuts on one map are two pictures. Spelled at
    // the default too, because this suffix is never empty by design and the
    // envmask cache is VRAM only, so every existing envmask key moving once
    // costs nothing; the default left unsaid is the same key as 128 said.
    [[nodiscard]] inline std::string EnvMaskSuffix(float a_feather, float a_gap,
                                                   bool a_red = false,
                                                   std::uint8_t a_cut = 128) {
        const auto mil = [](float a_v) {
            const int v = static_cast<int>(a_v * 1000.0f + 0.5f);
            return v < 0 ? 0 : (v > 9999 ? 9999 : v);
        };
        char buf[24]{};
        std::snprintf(buf, sizeof buf, "|m%04d%04d%s:c%02X", mil(a_feather), mil(a_gap),
                      a_red ? ":r" : "", static_cast<unsigned>(a_cut));
        return buf;
    }

}  // namespace OS::DyeKey
