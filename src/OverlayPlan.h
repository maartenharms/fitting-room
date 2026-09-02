#pragma once

#include "OverlayTransform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The Overlays page's engine-free half: which layers exist, what their nodes are
// called, and the numbers skee wants for a texture, a tint and an alpha.
//
// ⚠⚠ THE KEY CONSTANTS ARE NOT IN RaceMenu's MODDER HEADER, and they are the
// reason this file exists rather than the numbers sitting inline at the call
// site. They were recovered from skee's OverrideVariant.h and then corroborated
// against 331 installed RaceMenu .jslot presets, which store them literally:
// key 9 index 0 is the diffuse path (384 entries), key 8 is alpha (348), key 7
// is the tint (346). Full working in docs/handoffs/2026-08-16-overlays-
// interfaces-measured.md. Anything that changes a number here is changing what
// RaceMenu itself wrote into every preset on disk, so measure again first.
namespace OS::OverlayPlan {

    // ---- skee's override keys, measured ------------------------------------

    // ⚠ KEY 0 AND KEY 1 ARE THE GLOW, AND THEY ARE A PAIR. Measured from
    // RaceMenu's own OnOverlayGlowColorChange, extracted from RaceMenu.bsa: it
    // writes key 0 as a packed 0xAARRGGBB colour and key 1 as that same alpha
    // byte divided by TEN, which is where the 0 to 25.5 range in the installed
    // presets comes from. This page writes the colour and leaves the multiple
    // alone; see the note at the write in OverlayApi::Write for why.
    inline constexpr std::uint16_t kKeyEmissiveColour   = 0;  // packed 0xAARRGGBB
    inline constexpr std::uint16_t kKeyEmissiveMultiple = 1;  // float, 0 to 25.5

    // ⚠ KEYS 2 AND 3 ARE THE FINISH, AND THEY ARE NOT A PAIR THE WAY THE GLOW
    // IS. `ShaderGlossiness` and `ShaderSpecularStrength` are two independent
    // floats on `BSLightingShaderMaterialBase`, `specularPower` and
    // `specularColorScale`, and skee writes each straight through. They are
    // written and dropped together only because one control owns both.
    //
    // MEASURED twice. From `OverrideVariant.h` and the key table in
    // docs/handoffs/2026-08-16-overlays-interfaces-measured.md, and again from
    // what RaceMenu itself wrote: of the 331 installed presets, 15 carry key 2
    // and 15 carry key 3, all unindexed floats. The authored values are the
    // whole reason the ranges below are what they are.
    inline constexpr std::uint16_t kKeyGloss    = 2;  // float, specularPower
    inline constexpr std::uint16_t kKeySpecular = 3;  // float, specularColorScale

    inline constexpr std::uint16_t kKeyTint    = 7;  // packed 0xAARRGGBB, as an int
    inline constexpr std::uint16_t kKeyAlpha   = 8;  // float
    inline constexpr std::uint16_t kKeyTexture = 9;  // string, index is a texture slot

    // ⚠ ONLY kKeyTexture TAKES AN INDEX out of the three used here. skee's
    // SetValueVariant replaces the index with 0xFF for every key its
    // IsIndexValid says is unindexed, which lands in its internal SInt8 as -1
    // and is why every preset on disk shows index -1 beside keys 7 and 8. So
    // the index passed with a tint or an alpha is discarded and does not need
    // to be a particular value.
    [[nodiscard]] inline constexpr bool KeyTakesIndex(std::uint16_t a_key) {
        return a_key == kKeyTexture;
    }

    // The index an unindexed key is actually stored under.
    inline constexpr std::uint8_t kIndexNone = 0xFF;

    // ⚠⚠ PASS THIS AND NOT A BARE ZERO, ON EVERY CALL INCLUDING READS AND
    // REMOVALS. skee normalises the index on the way IN and not on the way out:
    // AddNodeOverride runs it through SetValueVariant, which replaces the index
    // of any unindexed key with 0xFF, but Impl_GetNodeOverride and
    // Impl_RemoveNodeOverride build their lookup key from the raw argument. The
    // stored set compares key AND index, so a tint written with index 0 lands
    // at index -1, and reading or removing it with index 0 finds nothing.
    //
    // The failure is silent in the worst way: the write works and is visible on
    // the character, the read comes back empty so the page shows no tint, and
    // the clear button does nothing at all. Passing 0xFF for the unindexed keys
    // matches what the write stored, and it is also what Add would have turned
    // it into, so one value is right for all three operations.
    [[nodiscard]] inline constexpr std::uint8_t IndexFor(std::uint16_t a_key,
                                                         std::uint8_t  a_slot) {
        return KeyTakesIndex(a_key) ? a_slot : kIndexNone;
    }

    // Texture slots inside a BSTextureSet. Slot 0 is the overlay's own art and
    // slot 1 is its normal.
    //
    // ⚠⚠ THE NORMAL IS THE ANSWER TO "RaceMenu cannot do this". Its own UI
    // exposes slot 0 alone, but skee bounds-checks the index against
    // BSTextureSet::kNumTextures and passes it straight to the texture set, so
    // slot 1 is reachable through exactly the same call. Six presets in the
    // reference load order already ship a slot 1 override.
    inline constexpr std::uint8_t kSlotDiffuse = 0;
    inline constexpr std::uint8_t kSlotNormal  = 1;

    // What skee resets a layer's diffuse to. Matches sDefaultTexture in the
    // shipped skee64.ini and the value in the presets.
    inline constexpr std::string_view kDefaultTexture =
        "Actors\\Character\\Overlays\\Default.dds";

    // ---- locations ---------------------------------------------------------

    // ⚠ THE ORDER IS THE PAGE'S, NOT skee's. IOverlayInterface::OverlayLocation
    // runs Body, Hand, Feet, Face; the page leads with Face because that is
    // what a player opening an overlays page is looking for. OverlayApi maps
    // between the two, and nothing outside it may assume the values line up.
    enum class Location : std::uint8_t { kFace = 0, kBody = 1, kHands = 2, kFeet = 3 };

    inline constexpr std::size_t kLocationCount = 4;

    struct LocationInfo {
        Location    location;
        const char* id;        // stable, for logs and saved state
        const char* labelKey;  // translation key for the group header
    };

    inline constexpr std::array<LocationInfo, kLocationCount> kLocations{ {
        { Location::kFace, "face", "$FR_Ovl_Face" },
        { Location::kBody, "body", "$FR_Ovl_Body" },
        { Location::kHands, "hands", "$FR_Ovl_Hands" },
        { Location::kFeet, "feet", "$FR_Ovl_Feet" },
    } };

    [[nodiscard]] inline constexpr std::size_t Slot(Location a_location) {
        return static_cast<std::size_t>(a_location);
    }

    // ---- node names --------------------------------------------------------

    // Substitute a layer index into the format skee's GetOverlayFormat returns.
    //
    // ⚠⚠ THE PLACEHOLDER IS "{}" AND NOT "%d". skee builds these with
    // std::vformat, so the format strings in its headers read "Body [Ovl{}]".
    // Handing one to a printf style call prints the braces, every node lookup
    // then misses, and nothing logs an error: the page would simply do nothing
    // for every layer on every character. Returns empty for a format with no
    // placeholder rather than silently giving every layer the same name.
    [[nodiscard]] inline std::string NodeName(std::string_view a_format,
                                              std::uint32_t    a_index) {
        const auto at = a_format.find("{}");
        if (at == std::string_view::npos) {
            return {};
        }
        std::string out{ a_format.substr(0, at) };
        out += std::to_string(a_index);
        out += a_format.substr(at + 2);
        return out;
    }

    struct Layer {
        Location      location{ Location::kFace };
        std::uint32_t index{ 0 };
        std::string   node;

        friend bool operator==(const Layer&, const Layer&) = default;
    };

    using Counts  = std::array<std::uint32_t, kLocationCount>;
    using Formats = std::array<std::string, kLocationCount>;

    // Every layer the installed skee says exists, in page order.
    //
    // ⚠ THE COUNTS COME FROM THE USER'S skee64.ini AND ARE NOT THREE. The
    // reference rig ships six body overlays and three of each of the rest, and
    // another rig can ship any of them. A layer list built from a constant is
    // wrong on somebody's install, and wrong quietly: the extra nodes exist and
    // simply never appear on the page.
    [[nodiscard]] inline std::vector<Layer> BuildLayers(const Formats& a_formats,
                                                        const Counts&  a_counts) {
        std::vector<Layer> layers;
        std::size_t        total = 0;
        for (const auto count : a_counts) {
            total += count;
        }
        layers.reserve(total);
        for (const auto& info : kLocations) {
            const auto slot = Slot(info.location);
            for (std::uint32_t i = 0; i < a_counts[slot]; ++i) {
                auto node = NodeName(a_formats[slot], i);
                if (node.empty()) {
                    continue;  // an unusable format drops its whole location
                }
                layers.push_back(Layer{ info.location, i, std::move(node) });
            }
        }
        return layers;
    }

    // ---- colour ------------------------------------------------------------

    // ⚠⚠ THE TINT IS 0xAARRGGBB AND THE TOP BYTE IS THE LAYER'S ALPHA. It was
    // 0x00RRGGBB here, which is a tint whose alpha is zero, and the field
    // verdict on that build was that the colouring did not behave like
    // RaceMenu's and that a pure black overlay could not be reached (user
    // 2026-08-16). Black through this call was the integer 0.
    //
    // MEASURED against the installed presets, which are what RaceMenu's own
    // controls wrote: of the 346 [Ovl] nodes carrying both key 7 and key 8,
    // 328 have a key 7 top byte exactly equal to round(key8 * 255), and every
    // one of the 18 that do not has a degenerate alpha - a negative denormal
    // sitting at zero - rather than a different rule. RaceMenu writes the same
    // alpha twice, once packed here and once as the float under key 8, so a
    // layer whose two disagree is a layer this page wrote.
    //
    // ⚠ THE PAIR MUST BE WRITTEN TOGETHER. Nothing may set the alpha slider
    // without re-sending the tint, or the two halves of one value drift.
    // OverlayApi::Write sends both keys on every call for that reason.
    //
    // Black is reachable without the emissive keys and the presets say so:
    // 0PreFHighElfUBE-Delia's face layer is 0xFF000000 with alpha 1 and carries
    // neither key 0 nor key 1, as 64 of the 98 live preset layers do. The
    // emissive pair is RaceMenu's separate glow control, not part of this.
    [[nodiscard]] inline constexpr std::uint32_t PackTint(std::uint8_t a_r,
                                                          std::uint8_t a_g,
                                                          std::uint8_t a_b,
                                                          std::uint8_t a_a = 0xFF) {
        return (static_cast<std::uint32_t>(a_a) << 24) |
               (static_cast<std::uint32_t>(a_r) << 16) |
               (static_cast<std::uint32_t>(a_g) << 8) | static_cast<std::uint32_t>(a_b);
    }

    // The alpha byte that goes beside a colour, from the float the page holds.
    // Rounded rather than truncated, because the presets round: an alpha of
    // 0.294 is stored as 75 and 75 / 255 is 0.2941.
    [[nodiscard]] inline constexpr std::uint8_t AlphaByte(float a_alpha) {
        const float clamped = a_alpha < 0.0f ? 0.0f : (a_alpha > 1.0f ? 1.0f : a_alpha);
        return static_cast<std::uint8_t>(clamped * 255.0f + 0.5f);
    }

    struct Rgb {
        std::uint8_t r{ 255 };
        std::uint8_t g{ 255 };
        std::uint8_t b{ 255 };

        friend bool operator==(const Rgb&, const Rgb&) = default;
    };

    // ⚠ THE TOP BYTE IS DROPPED HERE ON PURPOSE, AND IT IS THE ALPHA. Key 8
    // carries the same number as a float and that is the one the page reads
    // into its slider, so recovering it twice would give two authors of one
    // value. A naive shift that kept it would hand back a red channel that is
    // really an alpha.
    [[nodiscard]] inline constexpr Rgb UnpackTint(std::uint32_t a_packed) {
        return Rgb{ static_cast<std::uint8_t>((a_packed >> 16) & 0xFFu),
                    static_cast<std::uint8_t>((a_packed >> 8) & 0xFFu),
                    static_cast<std::uint8_t>(a_packed & 0xFFu) };
    }

    [[nodiscard]] inline constexpr float ClampAlpha(float a_alpha) {
        return a_alpha < 0.0f ? 0.0f : (a_alpha > 1.0f ? 1.0f : a_alpha);
    }

    // ---- glow --------------------------------------------------------------
    //
    // ⚠⚠ THE GLOW IS A SECOND COLOUR AND IT IS NOT THE TINT. RaceMenu has had
    // two controls per overlay all along, and the difference is what a black
    // overlay that still renders white is made of: the tint colours the
    // diffuse, and the emissive keeps emitting whatever the material inherited
    // whether or not the diffuse went black. A page with one colour control can
    // reach the first half and never the second.
    //
    // ⚠ THE STRENGTH IS THE GLOW COLOUR'S OWN ALPHA BYTE OVER TEN. Measured
    // from RaceMenu's OnOverlayGlowColorChange, extracted from RaceMenu.bsa:
    //
    //   int alpha = Math.RightShift(color, 24)
    //   AddNodeOverrideInt(..., 0, -1, color, true)          ; emissive colour
    //   AddNodeOverrideFloat(..., 1, -1, alpha / 10.0, true) ; emissive multiple
    //
    // which is exactly why the installed presets hold multiples on a 0 to 25.5
    // scale in steps of a tenth: 255 over 10. The two keys are one control and
    // are always written together.
    // ⚠⚠ RaceMenu'S CEILING IS 25.5 AND OURS IS 1.0, AND THAT IS A UI DECISION
    // RATHER THAN A DIFFERENT NUMBER ON THE WIRE. Field, 2026-08-16: "glow
    // strength is very strong. if even .1 is strong then 25.5 is overkill". On
    // a slider that reaches 25.5, every useful value is inside the first half
    // percent of the travel and the rest is degrees of white.
    //
    // 1.0 is not an arbitrary trim. DyeFlash's rung-2 probe measured the
    // emissive multiple already sitting at 1.0 on worn material, so full travel
    // here means "as bright as the material's own default emissive" and the
    // slider spends its whole length on the range a player can actually judge.
    // RaceMenu's own ceiling is twenty-five times that.
    //
    // ⚠⚠ THE CEILING IS BACK AT RaceMenu'S 25.5 AND THE SLIDER IS CURVED
    // INSTEAD. It was trimmed to 1.0 because 0.1 read as very strong, and that
    // reading was taken while the glow COLOUR defaulted to white: the emissive
    // was fighting at full brightness before the strength did anything. With
    // the colour defaulting to black the same strength is a different control,
    // and the field asked for much brighter than 1.0 (user 2026-08-16).
    //
    // ⚠ SO THE RANGE IS FULL AND THE TRAVEL IS SQUARED, which keeps both ends
    // usable. A linear slider to 25.5 puts every subtle value inside the first
    // half percent, which is the complaint that caused the trim; a squared one
    // gives 10% = 0.26, 20% = 1.0, 50% = 6.4 and 100% = 25.5, so the bottom of
    // the travel is where the fine control is and the top still reaches what
    // RaceMenu can reach.
    inline constexpr float kGlowStrengthMax = 25.5f;

    // Kept as its own name because the packing below is about what the WIRE can
    // carry, which is a separate question from what the slider offers even
    // while the two happen to agree.
    inline constexpr float kGlowStrengthWireMax = 25.5f;

    [[nodiscard]] inline constexpr float ClampGlow(float a_strength) {
        return a_strength < 0.0f ? 0.0f
                                 : (a_strength > kGlowStrengthMax ? kGlowStrengthMax
                                                                  : a_strength);
    }

    // The slider's own units. The stored value is RaceMenu's multiple, because
    // that is what goes on the wire, and a player should not have to think in
    // multiples of a material property to make something glow a bit.
    //
    // Not constexpr: std::sqrt is not, and the curve is worth more than the
    // compile-time evaluation nothing here needs.
    [[nodiscard]] inline float GlowPercent(float a_strength) {
        const float clamped = ClampGlow(a_strength);
        if (clamped <= 0.0f) {
            return 0.0f;
        }
        return std::sqrt(clamped / kGlowStrengthMax) * 100.0f;
    }

    [[nodiscard]] inline float GlowFromPercent(float a_percent) {
        const float clamped = a_percent < 0.0f ? 0.0f : (a_percent > 100.0f ? 100.0f : a_percent);
        const float unit    = clamped / 100.0f;
        return unit * unit * kGlowStrengthMax;
    }

    // The alpha byte that carries a glow strength, the inverse of RaceMenu's
    // divide by ten. Rounded, so 2.9 stores 29 and reads back as 2.9.
    //
    // ⚠ CLAMPED TO THE WIRE'S CEILING, NOT TO THE SLIDER'S. This has to be able
    // to represent a value RaceMenu wrote, or a preset read and rewritten here
    // would come back changed by the packing rather than by the player.
    [[nodiscard]] inline constexpr std::uint8_t GlowAlphaByte(float a_strength) {
        const float clamped =
            a_strength < 0.0f
                ? 0.0f
                : (a_strength > kGlowStrengthWireMax ? kGlowStrengthWireMax : a_strength);
        return static_cast<std::uint8_t>(clamped * 10.0f + 0.5f);
    }

    // And back again, for a strength read off a node.
    [[nodiscard]] inline constexpr float GlowStrengthFromByte(std::uint8_t a_alpha) {
        return static_cast<float>(a_alpha) / 10.0f;
    }

    // ---- the finish, keys 2 and 3 (OS-226 S1) -------------------------------
    //
    // Glossiness is the material's `specularPower`, which tightens the
    // highlight, and specular strength is its `specularColorScale`, which
    // brightens it. Together they are the difference between a matte tattoo and
    // one that looks wet or lacquered, and neither of them touches a material:
    // skee owns both keys, persists them in its own co-save and repaints them
    // at every install, so a finish set here survives a save, a reload and a
    // RaceMenu trip with nothing from this mod involved.
    //
    // ⚠⚠ THE DEFAULTS ARE MEASURED OFF THE LIVE MATERIAL, NOT CHOSEN. The
    // 2026-08-18 probe read `specPower=30.0 specScale=3.000` on all nine
    // installed layers, and the RaceMenu template parses to the same two
    // numbers. So an untouched layer already sits here and the sliders open
    // where the layer really is.
    //
    // ⚠⚠ AN UNSET FINISH IS DROPPED, NOT WRITTEN AT THE DEFAULT, and this is
    // the one place the glow's rule does NOT carry over. The glow is written on
    // every layer because an inherited emissive is a fault: it is what made a
    // black overlay render white. An inherited FINISH is not a fault, it is the
    // skin's own, and skee copies the source skin's material values onto the
    // clone at install (measured 2026-08-18, and the same copy is why
    // kModelSpaceNormals is not on a live layer). Writing 30 and 3 onto every
    // layer would flatten a shinier body's overlays to a constant nobody asked
    // for. Dropping instead is what the normal map already does one field up in
    // OverlayApi::WriteNow, for the same reason.
    inline constexpr float kDefaultGloss    = 30.0f;
    inline constexpr float kDefaultSpecular = 3.0f;

    // ⚠ THE CEILINGS ARE WHAT AUTHORS ACTUALLY WROTE. Scanning all 331
    // installed presets on 2026-08-18: key 2 holds 0 nine times, 5 twice and
    // 500 four times; key 3 holds 0 nine times, 5 twice and 10 four times. So
    // 500 and 10 are not headroom picked off a struct, they are the values a
    // preset author reached for when they wanted a layer to look wet, and a
    // slider that stopped short of them could not reproduce a preset already on
    // disk.
    inline constexpr float kGlossMax    = 500.0f;
    inline constexpr float kSpecularMax = 10.0f;

    [[nodiscard]] inline constexpr float ClampGloss(float a_value) {
        return a_value < 0.0f ? 0.0f : (a_value > kGlossMax ? kGlossMax : a_value);
    }

    [[nodiscard]] inline constexpr float ClampSpecular(float a_value) {
        return a_value < 0.0f ? 0.0f : (a_value > kSpecularMax ? kSpecularMax : a_value);
    }

    // ---- how long a look apply owns the layers -------------------------------
    //
    // ⚠⚠ THE APPLY'S OWN SKIN STEP USED TO UNDO ITS OVERLAY STEP, and the field
    // log caught it to the millisecond on 2026-09-01. Loading a save and then
    // applying a look over it:
    //
    //   14:51:34.012  step 'skin'      (arms the node push through MakeupApi)
    //   14:51:34.013  step 'overlays'  (5 written, 4 cleared)
    //   14:51:35.349  the push fires
    //   14:51:35.504  the PREVIOUS character's 7 body layers are back
    //
    // `SetNodeProperties` re-asserts what SKEE still holds, which is not what
    // the overlays step just decided, so the last writer won and it was the
    // wrong one. Two painters of one appearance, in the wrong order.
    //
    // ⚠ A WINDOW RATHER THAN A FLAG CLEARED AT THE END OF THE STEP, for the
    // same reason the face watch uses one: an apply keeps repainting after its
    // steps report done, and each of those repaints re-arms the push. The push
    // that bit us was armed 1.3 seconds AFTER the overlays step had finished, so
    // anything lifted at the end of the step would have missed it.
    //
    // ⚠ TEN SECONDS, MATCHING `MakeupPlan::kFaceWatchQuietAfterApplyMs`. It is
    // the same settle being covered and the same measurement behind it, so the
    // two numbers move together or the face and the layers disagree about when
    // an apply is over.
    //
    // ⚠ THE COST IS NAMED: a skin tone dragged within this window of an apply
    // will not get its layers pushed back until the next repaint after it. The
    // drag case is why the push exists, so this silences it for the shortest
    // span that still covers a settle rather than turning it off.
    inline constexpr int kNodePushQuietAfterApplyMs = 10000;

    // ⚠ THE GLOSS SLIDER IS SQUARED FOR THE REASON THE GLOW SLIDER IS. Linear
    // to 500 puts the default at 6% of the travel and every ordinary value in
    // the first tenth, which is the exact complaint that got the glow slider
    // trimmed and then curved. Squared: 10% = 5, 24% = 30 (the default sits
    // near a quarter of the way along), 50% = 125, 100% = 500.
    //
    // Specular is left LINEAR. Its whole range is 0 to 10, the default is 3, and
    // a curve on ten units would be ceremony rather than control.
    [[nodiscard]] inline float GlossPercent(float a_value) {
        const float clamped = ClampGloss(a_value);
        if (clamped <= 0.0f) {
            return 0.0f;
        }
        return std::sqrt(clamped / kGlossMax) * 100.0f;
    }

    [[nodiscard]] inline float GlossFromPercent(float a_percent) {
        const float clamped = a_percent < 0.0f ? 0.0f : (a_percent > 100.0f ? 100.0f : a_percent);
        const float unit    = clamped / 100.0f;
        return unit * unit * kGlossMax;
    }

    // ---- what a layer currently holds --------------------------------------

    struct LayerState {
        bool        hasTexture{ false };
        std::string texture;
        bool        hasNormal{ false };
        std::string normal;
        bool        hasTint{ false };
        Rgb         tint;
        bool        hasAlpha{ false };
        float       alpha{ 1.0f };
        // ⚠ THE GLOW DEFAULTS TO OFF AND IS WRITTEN ANYWAY (user 2026-08-16).
        // Leaving the keys unwritten would be RaceMenu parity and would also
        // leave a layer glowing with whatever its material inherited, which is
        // the whole reason a black overlay came out white. Writing a strength
        // of zero means the colour control means what it says on every layer,
        // and the cost is stated plainly: a pack whose art is meant to glow
        // stops until the player turns this up.
        // ⚠⚠ BLACK, NOT WHITE, AND THE PROBE IS WHY. Measured 2026-08-16 on a
        // lips overlay that would not go black: we wrote tint 0xFF000000, the
        // material read back tint 0x00000000 with alpha 1.0 and a glow multiple
        // of 0.000, and the shape still rendered white. The one white value
        // left on it was the emissive colour at 0x00FFFFFF.
        //
        // On the skin family of shaders the emissive colour is not gated by the
        // multiple the way a glow map is, so a strength of zero does not put it
        // out. Only the colour does. A glow that defaults to white therefore
        // ships every layer with a white emissive nobody asked for, which is
        // the whole of "some overlays can be dyed and some can't": it depends
        // on the shader the shape happens to carry.
        Rgb         glow{ 0, 0, 0 };
        float       glowStrength{ 0.0f };
        // ---- OS-226 S1: the finish -----------------------------------------
        //
        // ⚠ hasFinish IS THE WHOLE CONTROL. False means the layer wears the
        // skin's own finish and both keys are REMOVED from the node; true means
        // these two numbers are written. The sliders set it and the reset
        // clears it, so one predicate answers "is this layer's finish ours" for
        // the header, the write and the eraser alike.
        bool        hasFinish{ false };
        float       gloss{ kDefaultGloss };
        float       specular{ kDefaultSpecular };
        // ---- OS-209: where the art sits ------------------------------------
        //
        // ⚠⚠ `texture` IS ALWAYS THE SOURCE, THE ART THE PLAYER PICKED, and this
        // is what moved it. What skee holds under key 9 is a DIFFERENT string
        // when this is not identity: the baked file OverlayApi::Write derives
        // from the two of them (OverlayTransform::BakedPath). Read decodes a
        // baked path back into this pair through the sidecar beside the file,
        // so the page, the row labels, the thumbnails and the undo history all
        // see the art and never a hash. Identity means the source path itself
        // is on the layer and no file exists for it.
        OverlayTransform::Transform transform;

        friend bool operator==(const LayerState&, const LayerState&) = default;
    };

    // What key 9 should hold for a state: the source, or the bake of it.
    [[nodiscard]] inline std::string WirePath(const LayerState& a_state) {
        if (!a_state.hasTexture || a_state.texture.empty()) {
            return std::string{ kDefaultTexture };
        }
        if (OverlayTransform::IsIdentity(a_state.transform)) {
            return a_state.texture;
        }
        return OverlayTransform::BakedPath(a_state.texture, a_state.transform);
    }

    // ⚠ THE DEFAULT TEXTURE IS EMPTINESS, NOT CONTENT. skee writes
    // sDefaultTexture into slot 0 when a layer is reset, so a layer holding it
    // has an override on it and still shows nothing. Treating that as an
    // occupied layer would make every reset slot read as used, and the page's
    // "in use" count would never fall.
    [[nodiscard]] inline bool IsDefaultTexture(std::string_view a_path) {
        if (a_path.size() != kDefaultTexture.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a_path.size(); ++i) {
            const auto lhs = static_cast<unsigned char>(a_path[i]);
            const auto rhs = static_cast<unsigned char>(kDefaultTexture[i]);
            const auto l   = lhs == '/' ? '\\' : static_cast<char>(std::tolower(lhs));
            const auto r   = rhs == '/' ? '\\' : static_cast<char>(std::tolower(rhs));
            if (l != r) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline bool Occupied(const LayerState& a_state) {
        return a_state.hasTexture && !a_state.texture.empty() &&
               !IsDefaultTexture(a_state.texture);
    }

    // ---- texture paths -----------------------------------------------------

    // Turn a path found on disk into the form the override wants.
    //
    // ⚠ RELATIVE TO textures\, WITH BACKSLASHES, AND NO LEADING SEPARATOR. The
    // presets are unanimous on this: "Actors\Character\Overlays\Default.dds",
    // never "textures\Actors\..." and never a forward slash. A path with the
    // textures level still on it resolves to Data\textures\textures\... and the
    // engine falls back to its placeholder without saying so, which is the
    // failure [[missing-texture-resolves-to-a-placeholder]] describes.
    [[nodiscard]] inline std::string ToOverridePath(std::string_view a_path) {
        std::string out;
        out.reserve(a_path.size());
        for (const auto ch : a_path) {
            out.push_back(ch == '/' ? '\\' : ch);
        }
        while (!out.empty() && out.front() == '\\') {
            out.erase(out.begin());
        }
        constexpr std::string_view kPrefix = "textures\\";
        if (out.size() > kPrefix.size()) {
            bool matches = true;
            for (std::size_t i = 0; i < kPrefix.size(); ++i) {
                const auto l = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(out[i])));
                if (l != kPrefix[i]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                out.erase(0, kPrefix.size());
            }
        }
        return out;
    }

    // Turn a path the disk scan produced into the form the override wants, by
    // finding the "textures" segment and keeping everything after it.
    //
    // ⚠⚠ std::filesystem::relative IS THE WRONG TOOL HERE AND IT FAILED IN THE
    // FIELD. Under a mod manager the enumerated file does not live under the
    // directory that was walked: on 2026-08-16 the picker scanned
    // Data/textures/actors/character/overlays and got entries whose relative
    // path was "..\..\mods\Pretty Makeup - UBE - Racemenu Overlays\Textures\..."
    // because the real file sits outside the virtual root. Every one of those
    // went into the layer's texture override, so nothing a player picked could
    // ever have appeared on the character, and the thumbnail for it could not
    // be read either. The segment search does not care where the file really
    // lives, which is the property that was needed all along.
    //
    // Empty when there is no textures segment at all, so an unusable entry is
    // dropped rather than written somewhere as a path that cannot resolve.
    [[nodiscard]] inline std::string FromScanPath(std::string_view a_path) {
        std::string norm;
        norm.reserve(a_path.size());
        for (const auto ch : a_path) {
            norm.push_back(ch == '/' ? '\\' : ch);
        }
        std::string lowered = norm;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // The LAST one, because a mod folder may itself be called "textures"
        // and the game path is always the deepest.
        constexpr std::string_view kSeg = "\\textures\\";
        auto                       at   = lowered.rfind(kSeg);
        std::size_t                cut  = std::string::npos;
        if (at != std::string::npos) {
            cut = at + kSeg.size();
        } else if (lowered.rfind("textures\\", 0) == 0) {
            cut = 9;  // the whole path is already relative and starts there
        }
        if (cut == std::string::npos || cut >= norm.size()) {
            return {};
        }
        return norm.substr(cut);
    }

    // Where the game keeps its own tint masks, which is a second library and
    // not a second overlay folder.
    //
    // ⚠⚠ THE PICKER COULD NOT FIND THE DIRT ART AND THIS IS WHY. Field
    // 2026-08-16. The scan walked textures\actors\character\overlays and
    // nothing else, because it was written for the Overlays page, while the
    // whole vanilla tint mask library sits here: FemaleHeadDirt_01.dds through
    // _03.dds, every structural mask a race record names, and the 7323
    // references the installed presets make to this folder.
    inline constexpr std::string_view kTintMaskDir = "actors\\character\\character assets\\tintmasks\\";

    [[nodiscard]] inline bool IsTintMaskPath(std::string_view a_overridePath) {
        std::string lowered{ a_overridePath };
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowered.find(kTintMaskDir) != std::string::npos;
    }

    // The pack folder a texture belongs to: the first level under the overlays
    // directory, which is how packs separate themselves on disk and therefore
    // how the picker groups them. Empty for art sitting loose at the top.
    //
    // ⚠ THE TINT MASK LIBRARY GROUPS THE SAME WAY, one level under its own
    // root. The vanilla masks sit loose at the top of it and group under the
    // picker's "loose" heading, which is where they belong: they are the game's
    // own art rather than a pack's.
    [[nodiscard]] inline std::string PackFolder(std::string_view a_overridePath) {
        std::string lowered{ a_overridePath };
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        constexpr std::string_view kDir = "actors\\character\\overlays\\";
        auto                       at   = lowered.find(kDir);
        auto                       len  = kDir.size();
        if (at == std::string::npos) {
            at  = lowered.find(kTintMaskDir);
            len = kTintMaskDir.size();
        }
        if (at == std::string::npos) {
            return {};
        }
        const auto rest = a_overridePath.substr(at + len);
        const auto sep  = rest.find('\\');
        return sep == std::string_view::npos ? std::string{} : std::string{ rest.substr(0, sep) };
    }

    // The bit of a texture path worth putting on a button.
    [[nodiscard]] inline std::string DisplayName(std::string_view a_path) {
        auto cut = a_path.find_last_of("\\/");
        auto out = std::string{ cut == std::string_view::npos ? a_path
                                                              : a_path.substr(cut + 1) };
        if (out.size() > 4) {
            const auto tail = out.substr(out.size() - 4);
            if (tail == ".dds" || tail == ".DDS") {
                out.erase(out.size() - 4);
            }
        }
        return out;
    }

    // Whether a file is one of the maps that accompany an overlay rather than
    // the overlay art itself.
    //
    // ⚠ THE PICKER IS UNUSABLE WITHOUT THIS. Overlay packs ship the normal
    // beside the diffuse under the same stem, so an unfiltered scan of the
    // reference rig offers the player two entries per overlay, one of which
    // paints a blue-violet normal map onto their skin. The suffixes are the
    // game's own: _n normal, _msn model space normal, _s and _sk specular and
    // subsurface, _em emissive.
    [[nodiscard]] inline bool IsMapTexture(std::string_view a_path) {
        auto cut = a_path.find_last_of("\\/");
        auto name = cut == std::string_view::npos ? a_path : a_path.substr(cut + 1);
        if (name.size() <= 4) {
            return false;
        }
        const auto ext = name.substr(name.size() - 4);
        if (ext != ".dds" && ext != ".DDS") {
            return false;
        }
        std::string stem{ name.substr(0, name.size() - 4) };
        std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        for (const std::string_view suffix : { "_n", "_msn", "_s", "_sk", "_em" }) {
            if (stem.size() > suffix.size() &&
                stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) == 0) {
                return true;
            }
        }
        return false;
    }

    // The normal map that goes with a diffuse, by the game's own convention.
    // Returns empty when the diffuse does not look like one, so a caller can
    // offer nothing rather than a path that will not resolve.
    //
    // ⚠ A GUESS, AND OFFERED AS ONE. Overlay authors are not obliged to ship a
    // matching _n beside the diffuse, and most do not. The page uses this to
    // pre-fill a field the user can overwrite, never to write a normal nobody
    // asked for.
    [[nodiscard]] inline std::string GuessNormal(std::string_view a_diffuse) {
        if (a_diffuse.size() <= 4) {
            return {};
        }
        const auto stem = a_diffuse.substr(0, a_diffuse.size() - 4);
        const auto ext  = a_diffuse.substr(a_diffuse.size() - 4);
        if (ext != ".dds" && ext != ".DDS") {
            return {};
        }
        std::string out{ stem };
        out += "_n";
        out += ext;
        return out;
    }

}  // namespace OS::OverlayPlan
