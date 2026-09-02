#pragma once

#include "PCH.h"

#include "DyeBlend.h"

#include <cstdint>
#include <string>
#include <string_view>

// OS-139 rung 3: tinted diffuse textures, built on the GPU and cached.
//
// Spec: docs/superpowers/specs/2026-08-05-gpu-diffuse-dye-spike.md.
// The mechanism is proven; DyeGpu.cpp's task 3 is the probe that proved it and
// this is the shipping-shaped version of the same three steps. ⚠ The probe
// duplicates some of the D3D below ON PURPOSE. It is the recorded evidence for
// the verdict and it is meant to be deleted whole; coupling it to a live module
// would make deleting it a refactor.
//
// ---- WHY THIS IS A CACHE AND NOT AN OPTIMISATION ------------------------
//
// ⚠ THE CACHE IS ON THE CRITICAL PATH OF SURVIVAL. The dye walk runs on the
// GAME thread and a D3D11 immediate context may only be touched on the RENDER
// thread, so a walk that had to tint a texture could not finish its own swap.
// With a cache the walk asks a question it can answer synchronously: is there
// already a texture for this path and this colour. A hit swaps immediately; a
// miss enqueues a build and reports the slot PENDING, which is a state
// OutfitDye already has and already re-arms on.
//
// That is what makes a dye survive a rebuild Fitting Room did not ask for. The
// existing deferred chain (`QueueRepaint`, armed from BipedHooks' worn-pass
// hook, which is the seam a save load, a re-equip, a cell change and a race
// menu exit all pass through) walks again, finds a HIT because the texture was
// built the first time the colour was chosen, and re-swaps with no GPU work and
// no thread hop at all.
//
// ⚠ A BUILD ASKS FOR ITS OWN REPAINT, AND POLLING WAS NOT ENOUGH. The first cut
// relied on the deferred chain re-arming on `pending` and coming back to find a
// hit. The field run refuted it: clicking a palette swatch reverted the piece to
// its undyed colours and left it there, because the restore that precedes every
// repaint had already run and the chain expired before the texture existed. The
// hex field only looked like it worked because a drag posts an edit per frame
// and the release lands one final edit seconds later, by which time the build
// had happened. So a completed build now queues QueueRepaint for the actor that
// asked, which is notify rather than poll and has no timing coupling at all.
//
// ⚠ AND `Pump` MUST NOT BE GATED ON THE EDITOR BEING OPEN. An earlier version of
// this comment argued that it could be, because choosing a colour needs the
// editor. That is wrong and the same run proved it: this cache lives in memory
// and the SAVE holds the colour, so loading a game makes every stored colour a
// first-time colour again with the editor shut, and the armour came back
// undyed. The pump is driven from RenderOverlay for that reason.
//
// ---- KEYED BY PATH AND COLOUR, NEVER PER SHAPE --------------------------
//
// Measured on one dressed character 2026-08-05: 6 distinct diffuse paths across
// 11 worn shapes, with the helmet, gloves and boots textures each serving
// three. A per-shape cache would hold nearly twice what it needs to.
//
// ---- AND BY QUALITY, WHICH IS OS-140 ------------------------------------
//
// Spec: docs/superpowers/specs/2026-08-06-dye-texture-cost-spec.md. The
// arithmetic lives in DyeQuality.h, off engine, where it can be tested.
//
// One colour has TWO entries. Every request builds a capped PREVIEW straight
// away, at whatever `iDyePreviewCapPx` allows, by reading the matching level of
// the source rather than by downsampling anything. When a texture's colour has
// stopped changing for `iDyeSettleMs` the entry is promoted to a COMMIT build,
// and the repaint that build queues is what re-points the material at it.
//
// ⚠ CAPPED FIRST, ALWAYS, EVEN WHEN NOTHING IS DRAGGING. It is tempting to build
// full quality directly when there is no drag. Do not: the same cheap-first path
// then covers a save load, a cell change, a scheme apply and a paste, and every
// one of them gets a dye on screen in a frame instead of after an 85 MiB build.
//
// ⚠ TWO SEPARATE COSTS AND ONE FIX DOES NOT CURE BOTH, and an earlier draft of
// the OS-139 spec said they collapse together. They do not. The CHURN is the 528
// builds an evening a drag produces, and preview and commit fixes exactly that.
// The FLOOR is the 882.7 MiB still resident afterwards, which is the COMMITTED
// colours held by live materials, and only `iDyeCommitCapPx` moves it.
//
// ⚠ THE UPGRADE MUST FREE THE PREVIEW. Once the material re-points, the
// preview's refcount falls to one and the retire sweep takes it. That sweep runs
// every pump regardless of the budget, because eviction only runs when the
// budget is exceeded and a stranded preview would sit there until something
// unrelated pushed the cache over. If it stops working this whole unit becomes a
// cost rather than a saving, and `retired=` against `commit=` in the stats line
// is how that shows up.

namespace OS::DyeTexture {

    // The best tinted twin of a_source for a_tint that exists right now, or
    // null.
    //
    // ⚠ THE COMMIT IS CHECKED BEFORE THE PREVIEW, and that ordering is what
    // lands the upgrade rather than an optimisation. The caller holds whatever
    // this returns until its next repaint, so a lookup that answered with the
    // preview after the commit existed would pin the preview for ever.
    //
    // Null means "not yet", never "no": a miss enqueues the build and the
    // caller should treat the slot as pending and come back. Null is also what
    // a REFUSED source returns, and a refusal is permanent and logged once; see
    // the layout control in the implementation.
    //
    // ⚠ GAME THREAD. Takes no lock of OutfitDye's, and OutfitDye must not call
    // this while holding g_lock, so that stays true.
    //
    // The returned pointer is owned by this module and lives for the process.
    // Assign it into an NiPointer as normal; the cache's own reference is what
    // keeps it off zero when the engine releases its copy.
    // How the tint is combined with the source, because the two textures this
    // module builds want different arithmetic.
    // ⚠⚠ kOverlay WAS RENAMED TO kSoftLight ON 2026-08-13 AND THE ARITHMETIC DID
    // NOT MOVE. The curve this shipped with is D*D + 2*T*D*(1-D), which expands
    // to (1-2T)*D^2 + 2*T*D: term for term the Pegtop SOFT LIGHT formula with
    // the dye as the top layer. It is not Overlay, which is piecewise in the
    // base and which now has its own enumerator below. The old name was wrong
    // for as long as it existed and nobody noticed until a true overlay was
    // asked for. The name moved; the value (0, still first), the tag ("ovl",
    // see BlendTag) and every pixel it produces did not.
    enum class Blend {
        // Soft light, D*D + 2*T*D*(1-D). THE DEFAULT, and for a DIFFUSE, where
        // keeping the vanilla curve is what makes a dyed piece look dyed rather
        // than painted.
        //
        // ⚠ IT CANNOT MOVE PURE BLACK OR PURE WHITE, at any tint. Solving
        // o == D gives (1-2T)*(D^2 - D) == 0, so D=0 and D=1 are fixed points
        // for every colour a player can pick. A field report that a dye does
        // not reach the darkest or brightest parts of a garment is this
        // identity, not a fault, and no amount of work on the tint reaches it.
        // Multiply and screen below are the two that do move those ends.
        kSoftLight,
        // Take the tint's hue and saturation, and carry the source's luminance
        // across as the output's VALUE. For a CUBEMAP, where a multiply would
        // be a trap: a gold cubemap has almost no blue in it, so multiplying by
        // a blue dye gives near-black rather than blue metal. Luminance times
        // the normalised tint keeps every highlight and every shape in the
        // reflection and only repaints its colour.
        //
        // ⚠⚠ IT DOES NOT PRESERVE LUMINANCE, though this comment claimed it did
        // until 2026-08-13. lum(out) = lum(src) * lum(tint)/max(tint), so a
        // saturated blue returns 7.22% of the source's brightness and even a
        // pure green returns 72%. What it preserves is the source's luma as the
        // output's MAX CHANNEL. That is a different and dimmer thing, and it is
        // why a dyed red eye measured #241700.
        //
        // ⚠⚠ THE ARITHMETIC WAS DELIBERATELY LEFT ALONE WHEN THAT WAS FOUND.
        // Every cubemap, every glow map and every dyed eye on this path renders
        // through it, including the iris mask the field confirmed, so
        // "correcting" it would restyle shipped, confirmed work to fix a
        // comment. kColour below is the luminance-preserving one, added beside
        // it rather than over it. render-change-needs-a-key-change: this one
        // has not changed and must not without a key change.
        kRecolour,
        // Metallic flake, for the envmap MASK. The mask is a per-texel strength
        // map (one channel, under vanilla and Community Shaders alike), so
        // breaking it into sparse bright speckles over a dimmed body turns the
        // reflection into discrete glinting flakes — the ATI car-paint flake
        // layer, achieved with the one texture that controls per-texel
        // reflectivity. The tint's RED channel carries the intensity; the other
        // two are unused, which also keys each intensity separately for free.
        kFlake,
        // Metallic flake, for the NORMAL map, and this is the half that
        // actually TWINKLES. Static dots in a strength map cannot: they are
        // fixed in texture space, they average away down the mip chain, and
        // nothing about them answers the camera. Perturbing the normal per
        // flake cell makes micro-facets whose specular highlights flare and die
        // as the view or the light moves - the ATI car-paint flake layer
        // proper, and the reason sparkle in other engines is a normal effect,
        // never an albedo one. Tint red carries the intensity, as kFlake.
        kFlakeNormal,

        // ---- the photo-editor curves, 2026-08-13 ------------------------
        //
        // Asked for by name, and all three are the standard formulas rather
        // than anything of ours. They are DIFFUSE blends: the cube shader has
        // no branch for them (it runs recolour or the ramp and nothing else),
        // so a request for one against a cubemap source silently gets the
        // cube's own arithmetic. That matches where kSoftLight already stood
        // and is written down rather than fixed, because a cubemap wants its
        // colour repainted and not its exposure changed.
        //
        // ⚠ NONE OF THEM ARE THE DEFAULT and none can become it by accident:
        // Settings resolves an unknown INI spelling to kSoftLight, which is
        // the behaviour every install had before this existed.

        // o = D * T. Darkens, and unlike soft light it CAN reach pure white:
        // white times the tint is the tint. It cannot lift black, and a
        // saturated dye on a dark garment goes nearly black, which is the
        // honest behaviour of a multiply and the reason it is not the default.
        kMultiply,
        // o = 1 - (1-D)*(1-T). Lightens, the mirror of multiply: it can lift
        // pure black to the tint and cannot darken white. The pair is what a
        // player reaches for when soft light's fixed ends are the problem.
        kScreen,
        // The real Overlay: 2*D*T below mid-grey, 1-2*(1-D)*(1-T) at or above
        // it. Piecewise IN THE BASE, which is what separates it from soft
        // light and what makes it boost contrast rather than tint gently.
        // Shares soft light's fixed points at 0 and 1.
        kOverlay,

        // ---- the two NON-SEPARABLE modes ---------------------------------
        //
        // ⚠ THESE TWO DO NOT WORK A CHANNEL AT A TIME. Every blend above maps
        // each of R, G and B independently; these read all three to compute a
        // luminance and then put a whole colour back, which is what "non
        // separable" means in the Photoshop and W3C compositing specs. They use
        // THOSE specs' luminance weights (0.30, 0.59, 0.11), not the Rec.709
        // weights Recolour and RampAt use, because the weights are part of the
        // formula being named. Do not unify them.

        // Photoshop's Color: hue and saturation from the dye, luminance from
        // the garment, and unlike kRecolour it PRESERVES that luminance
        // exactly. This is the mode kRecolour's comment used to describe.
        kColour,
        // Photoshop's Luminosity: hue and saturation from the garment,
        // luminance from the dye.
        //
        // ⚠⚠ WITH A FLAT DYE THIS IS DEGENERATE AND THAT IS ARITHMETIC, NOT AN
        // OPINION. The top layer here is one colour, so lum(tint) is a
        // CONSTANT, so every texel comes out at the same luminance and all of
        // the garment's shading and depth is gone; what survives is its hue and
        // saturation pattern. Photoshop's Luminosity is useful because its top
        // layer is an IMAGE with luminance that varies. Shipped because it was
        // asked for by name and because seeing it is cheaper than arguing about
        // it, documented so nobody debugs the flatness as a fault.
        kLuminosity,
    };

    // The three letters a blend contributes to a cache key.
    //
    // ⚠⚠ THIS FUNCTION EXISTS BECAUSE THE THING IT REPLACES SHIPPED BROKEN.
    // ColourOf and SlotOf each spelled the blend with a ternary chain ending
    // `: "ovl"`, so kFlakeNormal keyed BYTE-IDENTICALLY to the default and had
    // done since the day it was added. The build got the right arithmetic (the
    // enum travels whole in Request::blend) while the KEY lied, which is the
    // silent, cached and persistent fault ColourOf's own header warns about,
    // one level down from where it was watching. The chain had already been
    // widened by hand once, for kFlake, and was missed the second time.
    //
    // ⚠ A switch WITH NO default IS THE WHOLE POINT. A ternary chain has a
    // fallthrough arm, so widening the enum compiles and silently aliases;
    // this makes the compiler list the sites instead.
    // widen-an-enum-by-making-the-compiler-list-the-sites.
    //
    // ⚠ kSoftLight KEEPS "ovl" THOUGH IT WAS RENAMED. The tag names the
    // ARITHMETIC, and that did not change, so every armour texture already
    // cached keeps the key it had. Spelling it "sft" would invalidate the lot
    // to no purpose. The true overlay takes "ovr", which is unused.
    [[nodiscard]] constexpr const char* BlendTag(Blend a_blend) {
        switch (a_blend) {
            case Blend::kSoftLight:
                return "ovl";
            case Blend::kRecolour:
                return "rec";
            case Blend::kFlake:
                return "flk";
            case Blend::kFlakeNormal:
                return "fln";
            case Blend::kMultiply:
                return "mul";
            case Blend::kScreen:
                return "scr";
            case Blend::kOverlay:
                return "ovr";
            case Blend::kColour:
                return "col";
            case Blend::kLuminosity:
                return "lms";
        }
        return "ovl";  // unreachable; the switch is exhaustive and has no default
    }

    // The name a LOG prints, never a cache key. BlendTag above cannot say
    // "softlight" because its "ovl" is baked into every cached texture key on
    // every install (the enumerator was renamed 2026-08-13, the tag
    // deliberately was not), and on 2026-08-30 that exact string sent a field
    // diagnosis hunting a phantom overlay for a session. Logs get the INI
    // spelling; keys keep their history.
    [[nodiscard]] constexpr const char* BlendName(Blend a_blend) {
        switch (a_blend) {
            case Blend::kSoftLight:
                return "softlight";
            case Blend::kRecolour:
                return "recolour";
            case Blend::kFlake:
                return "flake";
            case Blend::kFlakeNormal:
                return "flakenormal";
            case Blend::kMultiply:
                return "multiply";
            case Blend::kScreen:
                return "screen";
            case Blend::kOverlay:
                return "overlay";
            case Blend::kColour:
                return "colour";
            case Blend::kLuminosity:
                return "luminosity";
        }
        return "softlight";  // unreachable; the switch is exhaustive
    }

    // ⚠ THE TAGS ARE ASSERTED DISTINCT AT COMPILE TIME, NOT TESTED. No test
    // compiles this header (it needs the engine's texture types), which is the
    // same gap that let the old ternary alias two blends onto one string for
    // months. A static_assert cannot be skipped, needs no harness, and fails
    // the build rather than the picture.
    //
    // The switch above is the primary guard: it makes a new enumerator a
    // compile error until it is given a tag. This is the second half, catching
    // a tag that is given but is already taken.
    namespace detail {
        inline constexpr Blend kAllBlends[]{
            Blend::kSoftLight, Blend::kRecolour, Blend::kFlake,      Blend::kFlakeNormal,
            Blend::kMultiply,  Blend::kScreen,   Blend::kOverlay,    Blend::kColour,
            Blend::kLuminosity,
        };

        [[nodiscard]] constexpr bool BlendTagsDistinct() {
            for (std::size_t i = 0; i < std::size(kAllBlends); ++i) {
                for (std::size_t j = i + 1; j < std::size(kAllBlends); ++j) {
                    if (std::string_view{ BlendTag(kAllBlends[i]) } ==
                        std::string_view{ BlendTag(kAllBlends[j]) }) {
                        return false;
                    }
                }
            }
            return true;
        }
    }  // namespace detail

    static_assert(detail::BlendTagsDistinct(),
                  "two Blend values share a cache-key tag, so one blend's cached texture "
                  "would be served for the other's request");

    // The INI spelling of a diffuse blend, resolved once per paint pass.
    //
    // ⚠ AN UNKNOWN SPELLING IS SOFT LIGHT, NEVER A REFUSAL AND NEVER THE LAST
    // ENUMERATOR. Same rule as DyeRamp::ModeFromByte and DyePalette's mode key:
    // a setting written against a later build must lose the setting, not the
    // dye. A typo therefore renders exactly as an install with no key at all.
    //
    // ⚠ ONLY THE FOUR DIFFUSE CURVES ARE SPELLABLE. The flake blends and
    // recolour are chosen by which TEXTURE is being built, never by a player,
    // so naming them here would let an INI point the armour walk at arithmetic
    // meant for a normal map.
    [[nodiscard]] inline Blend BlendFromName(std::string_view a_name) {
        if (a_name == "multiply") {
            return Blend::kMultiply;
        }
        if (a_name == "screen") {
            return Blend::kScreen;
        }
        if (a_name == "overlay") {
            return Blend::kOverlay;
        }
        // ⚠ BOTH SPELLINGS OF COLOUR. The mod's own prose is British and
        // Photoshop's menu is American, so a player copying either is right.
        if (a_name == "colour" || a_name == "color") {
            return Blend::kColour;
        }
        if (a_name == "luminosity") {
            return Blend::kLuminosity;
        }
        return Blend::kSoftLight;
    }

    // The blends a PLAYER may choose, in the order the settings panel lists
    // them, spelled the way the INI spells them.
    //
    // ⚠ THE INDEX IS NOT STORED ANYWHERE, THE NAME IS, so this array can be
    // reordered freely and a player's setting survives. That is the opposite of
    // the cost-mode combo next to it in SettingsUI, whose index IS the stored
    // value and whose order is therefore pinned by static_asserts. The two
    // controls look alike and are not, which is why this says so.
    //
    // ⚠ THE FLAKE BLENDS AND kRecolour ARE ABSENT ON PURPOSE. They are chosen
    // by which TEXTURE is being built, never by a player, and offering them
    // here would point the armour walk at arithmetic written for a normal map.
    inline constexpr std::string_view kSelectableBlendNames[]{
        "softlight", "multiply", "screen", "overlay", "colour", "luminosity",
    };

    // ---- the same six, as a DYE's own choice -------------------------------
    //
    // ⚠ THE TWO TABLES ARE HELD TOGETHER BY THE ASSERT BELOW, NOT BY CARE. A
    // dye's blend and the install's blend are spelled in the same files by the
    // same people, so "multiply" on a dye and "multiply" in the INI must resolve
    // to one curve. DyeBlend::kChoiceNames is this array with a leading empty
    // entry for the deferring zero; anything else is a silent divergence where a
    // pack author's word means something the settings panel does not offer.
    // ⚠ TWO MORE THAN THE SIX, NOT ONE: kDefault's empty entry sits at index 0
    // and kRecolour is appended past the six. The six in the middle are the
    // ones that must match, which is what the element-wise check below holds.
    static_assert(std::size(DyeBlend::kChoiceNames) == std::size(kSelectableBlendNames) + 2,
                  "a player-selectable blend exists with no dye-side choice, or the other "
                  "way about, so a dye and the INI would offer different curves");

    [[nodiscard]] constexpr bool BlendChoiceNamesMatch() {
        for (std::size_t i = 0; i < std::size(kSelectableBlendNames); ++i) {
            if (DyeBlend::kChoiceNames[i + 1] != kSelectableBlendNames[i]) {
                return false;
            }
        }
        return DyeBlend::kChoiceNames[0].empty();
    }

    static_assert(BlendChoiceNamesMatch(),
                  "a dye's blend spelling and the INI's have diverged, so the same word "
                  "would mean two different curves depending on where it was written");

    // What a stored choice actually resolves to on this surface.
    //
    // ⚠⚠ THE FALLBACK IS THE CALLER'S AND THERE IS NO DEFAULT FOR IT. Armour
    // passes what sDyeBlend resolved to; the eye passes kOverlay. A default
    // here would be one of those two winning on the other's surface, and the
    // two have never wanted the same answer: a garment wants the vanilla curve
    // kept so it reads as dyed rather than painted, and an eye wants its white
    // highlights left standing.
    [[nodiscard]] constexpr Blend ResolveDyeBlend(std::uint8_t a_choice, Blend a_fallback) {
        switch (DyeBlend::ChoiceFromByte(a_choice)) {
            case DyeBlend::Choice::kSoftLight:
                return Blend::kSoftLight;
            case DyeBlend::Choice::kMultiply:
                return Blend::kMultiply;
            case DyeBlend::Choice::kScreen:
                return Blend::kScreen;
            case DyeBlend::Choice::kOverlay:
                return Blend::kOverlay;
            case DyeBlend::Choice::kColour:
                return Blend::kColour;
            case DyeBlend::Choice::kLuminosity:
                return Blend::kLuminosity;
            case DyeBlend::Choice::kRecolour:
                return Blend::kRecolour;
            case DyeBlend::Choice::kDefault:
                break;
        }
        // ⚠ THE DEFAULT ARM IS THE ENUMERATOR, NOT A `default:` LABEL, for the
        // reason BlendTag's own switch has none: widening Choice must be a
        // compile error listing the sites, not a silent fall into deferring.
        return a_fallback;
    }

    // The ramp a dye asks for, if it asks for one.
    //
    // ⚠ EVERY DEFAULT IS FLAT, so every existing call site keeps its exact
    // meaning without being touched, and a texture cached before ramps existed
    // keeps the key it already had.
    //
    // ⚠ THE MODE HERE IS ALREADY EFFECTIVE, never declared. DyeRamp::EffectiveMode
    // resolves iridescent down to nacre on a shape with no cubemap, and that
    // decision belongs to the caller because only the caller knows the shape.
    // Which TARGET the ramp lands on is likewise the caller's: it picks the
    // diffuse or the cubemap by which texture it hands in. By the time a request
    // reaches this module the only remaining question is whether to run the ramp
    // at all.
    struct Ramp {
        std::uint8_t mode{ 0 };
        bool         secondSet{ false };
        std::uint8_t r2{ 0 };
        std::uint8_t g2{ 0 };
        std::uint8_t b2{ 0 };
        // The dye's gloss, carried because an IRIDESCENT cubemap build bakes it
        // into the marker mip's alpha as roughness (see the marker note in the
        // .cpp). 128 is the struct's own neutral, exactly DyeMaterial's.
        std::uint8_t gloss{ 128 };
    };

    // a_waiter is the actor whose walk asked. A build that completes queues a
    // repaint for it, which is how the swap actually lands; see the header note
    // on notify versus poll. A null handle is legal and simply means nobody is
    // waiting on this one.
    //
    // ⚠ THE RAMP IS PART OF THE CACHE KEY, and that is not an optimisation. Two
    // dyes sharing a primary hex and differing only in their second stop are
    // different textures, so a key that could not tell them apart would hand
    // back the wrong one, silently, for as long as the entry lived.
    // ⚠ a_previewOnly (spec 2026-08-09): a hover preview's request. While every
    // request for a colour says it, the settle sweep never promotes that colour
    // to a commit build; the first real request clears it. It does NOT change
    // which quality THIS call builds, so the flake blends keep their direct
    // commit builds (their capped previews are a field-confirmed defect), which
    // is a recorded narrowing of the spec's no-commit-builds sentence.
    // ⚠ a_capPx: A CEILING THIS CALLER NEEDS ON TOP OF THE GLOBAL ONES, in
    // pixels on the longest side, 0 for none. It only ever tightens: the
    // smaller of this and iDyeCommitCapPx / iDyePreviewCapPx wins, so a caller
    // cannot ask for more than the install allows.
    //
    // It exists because the RIGHT resolution is a property of the SURFACE and
    // not of the texture. MEASURED 2026-08-12: one dyed demon eye is a 4096
    // twin at 87 MiB and cycling eye colours held 508 of a 512 MiB budget,
    // evicting on almost every build, while the eye itself occupies a few dozen
    // pixels on screen. The same 4096 on a cuirass is worth keeping.
    //
    // ⚠ AND IT IS IN THE CACHE KEY, which is the whole reason it is a parameter
    // rather than a number read inside the build. Two callers asking for one
    // texture at two ceilings want two different sets of bytes, and a key that
    // could not tell them apart would serve a 512 eye twin to a caller that
    // asked for the full one, silently and for as long as the entry lived. The
    // suffix is empty at 0, so every texture cached before this existed keeps
    // the exact key it had. Same discipline as DyeKey::RampSuffix, same reason.
    //
    // ⚠ a_mask: A SECOND SOURCE whose ALPHA says where the tint lands, null for
    // everywhere, which is byte for byte what every call before masks existed
    // asked for. The eye path passes its normal map here: Skyrim reads a normal
    // map's alpha as the specular mask, the cornea is the shiny part of an eye,
    // so the disc an author drew to place the highlight is exactly the iris.
    // The build lerps each texel between the untouched source and the tinted
    // value by that alpha, normalised against the mask's own mean (see the
    // shader), so the sclera keeps its own pink and veins.
    //
    // ⚠ THE MASK IS IN THE CACHE KEY AND IN THE SLOT, both, because it is an
    // INPUT of the build: two eyes sharing a diffuse and differing in their
    // normal are two different textures AND two different settle clocks.
    // DyeKey::MaskSuffix carries the arithmetic and the reason.
    //
    // ⚠ A MASKED REQUEST IS ALWAYS FLAT. The mask's window rides the two
    // constant-buffer scalars the ramp would otherwise use (the buffer is full,
    // and its own note says the next scalar pays for a fourth register), so a
    // request carrying both a mask and a ramp is a caller error: the build logs
    // it and drops the mask. The eye path always passes Ramp{}.

    // Which side of the mask's disc takes colour, and what the far side takes.
    // The default is exactly the first masked build that shipped: the request's
    // own tint inside the disc, the source untouched outside it, so every
    // existing masked call keeps its meaning without being touched.
    //
    // ⚠ irisSet false MEANS THE REQUEST'S TINT IS CANONICAL BLACK, and the
    // caller owes that. The tint is in the cache key, so an "ignored" tint
    // that still varied would fragment one sclera dye into many entries.
    // DyeKey::MaskDyeSuffix carries the iris-off marker for the same reason:
    // black-ignored and black-applied must not collide.
    //
    // Only read when a_mask is non-null; an unmasked build has no disc for
    // either side to be a side of.
    struct MaskDye {
        bool         irisSet{ true };
        bool         scleraSet{ false };
        std::uint8_t r{ 0 };
        std::uint8_t g{ 0 };
        std::uint8_t b{ 0 };
        // ---- the PER-EYE split (heterochromia), 2026-08-13 ------------------
        //
        // On a split eye set the two eyes read DISJOINT halves of one texture
        // (measured: wammy and UBE 8_1, left eye u 0.010..0.498, right eye u
        // 0.511..0.998, the side fixed by the Lens meshes' vertex sign; the
        // census doc carries the numbers). So per-eye colour is per-HALF
        // colour: the request's own tint covers u < 0.5 (the LEFT eye) and
        // r2/g2/b2 covers the rest (the RIGHT eye).
        //
        // ⚠ A SPLIT BUILD NEVER CARRIES A SCLERA. The split families ship a
        // FLAT normal alpha (measured mean 0.97..1.00), so the mask windows to
        // t=1 and a sclera colour has nowhere to land; the fill site reuses
        // gStopB for r2/g2/b2 on the strength of exactly that. A set that one
        // day ships BOTH a split UV and a usable disc gets the split and loses
        // the sclera, which is the fallback direction the sclera already has.
        //
        // ⚠ ONLY MEANINGFUL ON A SPLIT SET. On a shared-disc set (ILV, Hit2,
        // Aretuza, LDD) both eyes read the SAME texels and the u=0.5 line cuts
        // through the one shared iris disc: both eyes would render half-and-
        // half. The caller owns not asking for that; the classifier that reads
        // a part's UV layout is what will own it when the UI ships.
        bool         splitSet{ false };
        std::uint8_t r2{ 0 };
        std::uint8_t g2{ 0 };
        std::uint8_t b2{ 0 };
    };

    RE::NiSourceTexture* Acquire(RE::NiSourceTexture* a_source, const RE::NiColor& a_tint,
                                 RE::ActorHandle a_waiter, Blend a_blend = Blend::kSoftLight,
                                 const Ramp& a_ramp = {}, bool a_previewOnly = false,
                                 std::uint32_t a_capPx = 0,
                                 RE::NiSourceTexture* a_mask = nullptr,
                                 const MaskDye& a_maskDye = {},
                                 // ⚠ WHAT THIS TEXTURE IS CALLED WHEN IT WILL NOT SAY.
                                 // MEASURED 2026-08-13: an armour diffuse arrives
                                 // resident and NAMELESS, so the key collapsed and the
                                 // guard refused it, and no armour dyed for fourteen
                                 // hours. The caller passes the authored path off the
                                 // material's own texture set, which is stable, real,
                                 // and identical to what a named texture would give.
                                 // Ignored whenever the texture has a name of its own,
                                 // so no key already in the field moves.
                                 const char* a_sourceHint = nullptr,
                                 const char* a_maskHint   = nullptr);

    // Build up to a few queued textures.
    //
    // ⚠ RENDER THREAD ONLY, for the same reason DyeGpu is: `.context` is the
    // IMMEDIATE context and D3D11 immediate contexts are not thread safe.
    // `EditorIWindow::Draw` is the live render thread on this project.
    //
    // ⚠ BOUNDED PER CALL rather than draining. One 4096 tint is 0.1 ms on the
    // dev card and about 1.2 ms on integrated graphics, so a whole outfit
    // drained in one frame is a visible hitch on the hardware that can least
    // afford it. The deferred repaint chain spans seconds and this runs every
    // frame, so spreading the builds costs nothing that anybody can see.
    void Pump();

    // Install the Present hook if it is not up yet. The hook installs itself
    // lazily from the first dye request; the preview cache calls this so its
    // frame clock (PreviewFrame.h, bumped in the thunk) runs even in a session
    // where no dye was ever touched.
    void EnsurePresent();

    struct Stats {
        std::size_t held{ 0 };      // textures in the cache
        std::size_t queued{ 0 };    // builds still owed
        std::size_t hits{ 0 };
        std::size_t misses{ 0 };
        std::size_t refused{ 0 };   // sources the layout control rejected
        std::size_t failed{ 0 };    // builds that could not allocate
        // Queued builds replaced by a newer colour before they ran. High during
        // a colour drag and that is the point: each one is a texture that was
        // never built.
        std::size_t superseded{ 0 };
        std::size_t evicted{ 0 };
        // ⚠ OS-140'S NEGATIVE CONTROL, AND IT IS A COUNT RATHER THAN A PICTURE.
        // The 2026-08-05 session built 528 textures, 526 of them distinct, in
        // one evening of colour picking. A drag of comparable length must now
        // produce a couple of commit builds and a modest number of previews. If
        // commits stay near 500 the split is not working, however good it looks
        // on screen.
        std::size_t previewBuilds{ 0 };
        std::size_t commitBuilds{ 0 };
        // Previews freed after their commit landed. Should track commitBuilds
        // closely. A gap means previews are staying resident after the material
        // re-pointed, which would make this unit a cost rather than a saving.
        std::size_t retired{ 0 };
        std::uint64_t bytes{ 0 };   // VRAM held, mip chain included
    };
    [[nodiscard]] Stats GetStats();

    // One line naming everything above, for the end of a dye pass.
    void LogStats(const char* a_where);

}  // namespace OS::DyeTexture
