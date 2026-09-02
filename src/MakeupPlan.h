#pragma once

#include "OverlayPlan.h"       // Rgb, PackTint, AlphaByte, ClampAlpha, DisplayName
#include "OverlayTransform.h"  // OS-209: a layer's art can be offset, and it bakes

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Makeup is the head's TINT MASK layers, and it is a different system from the
// [Ovl] overlay nodes OverlayPlan describes.
//
// ⚠⚠ AND THAT WAS MEASURED RATHER THAN ASSUMED. A lips overlay would not go
// black. A probe read the MATERIAL rather than the override store and printed
// `wrote tint 0xFF000000 | material tint = 0x00000000 alpha = 1.000 multiple =
// 0.000 | tint TOOK` while the shape still rendered white. There is no darkness
// override to reach on an overlay node for that art, because that art is not an
// overlay. See docs/handoffs/2026-08-16-makeup-tint-masks-measured.md.
//
// This header is the PURE half: the vocabulary, the packing and the rules about
// what a layer means. Everything that touches the engine lives in MakeupApi, so
// that this file compiles into a test that links no game code.
namespace OS::MakeupPlan {

    // ---- the layer types ----------------------------------------------------
    //
    // The engine stores a type on every tint mask and groups them by it: its own
    // GetNumTints(type) and GetTintMask(type, index) both walk the whole list
    // comparing this field, which is at TintMask+0x10.
    //
    // ⚠ THE LIST IS LONGER THAN THIS ENUM AND IT IS RACE DEPENDENT. Measured
    // across 331 installed presets: all 329 carrying tintInfo use indices 0
    // through 17 and some go past 24, so a page that only ever showed fifteen
    // slots would hide layers a character really has. The type is a LABEL for
    // grouping, never the index, and nothing here may treat the two as the same
    // number.
    enum class Type : std::uint32_t {
        kFreckles = 0,
        kLips,
        kCheeks,
        kEyeliner,
        kUpperEyeSocket,
        kLowerEyeSocket,
        kSkinTone,
        kWarPaint,
        kFrownLines,
        kLowerCheeks,
        kNose,
        kChin,
        kNeck,
        kForehead,
        kDirt,

        kTotal
    };

    inline constexpr std::size_t kTypeCount = static_cast<std::size_t>(Type::kTotal);

    struct TypeInfo {
        Type        type;
        const char* id;        // stable, for logs and saved state
        const char* labelKey;  // translation key
    };

    inline constexpr std::array<TypeInfo, kTypeCount> kTypes{ {
        { Type::kFreckles, "freckles", "$FR_Mk_Freckles" },
        { Type::kLips, "lips", "$FR_Mk_Lips" },
        { Type::kCheeks, "cheeks", "$FR_Mk_Cheeks" },
        { Type::kEyeliner, "eyeliner", "$FR_Mk_Eyeliner" },
        { Type::kUpperEyeSocket, "uppereye", "$FR_Mk_UpperEye" },
        { Type::kLowerEyeSocket, "lowereye", "$FR_Mk_LowerEye" },
        { Type::kSkinTone, "skintone", "$FR_Mk_SkinTone" },
        { Type::kWarPaint, "warpaint", "$FR_Mk_WarPaint" },
        { Type::kFrownLines, "frownlines", "$FR_Mk_FrownLines" },
        { Type::kLowerCheeks, "lowercheeks", "$FR_Mk_LowerCheeks" },
        { Type::kNose, "nose", "$FR_Mk_Nose" },
        { Type::kChin, "chin", "$FR_Mk_Chin" },
        { Type::kNeck, "neck", "$FR_Mk_Neck" },
        { Type::kForehead, "forehead", "$FR_Mk_Forehead" },
        { Type::kDirt, "dirt", "$FR_Mk_Dirt" },
    } };

    // ⚠ AN UNKNOWN TYPE IS SHOWN, NEVER DROPPED, which is the same call
    // OverlayLocations makes about a texture no pack registered. A race whose
    // list carries a type past kDirt is a race whose layers would otherwise
    // vanish from the page with nothing said, and a layer the player can see on
    // their own face but not on this page is the worst outcome available.
    [[nodiscard]] inline constexpr bool KnownType(std::uint32_t a_type) {
        return a_type < kTypeCount;
    }

    [[nodiscard]] inline const char* LabelKeyFor(std::uint32_t a_type) {
        return KnownType(a_type) ? kTypes[a_type].labelKey : "$FR_Mk_Other";
    }

    [[nodiscard]] inline const char* IdFor(std::uint32_t a_type) {
        return KnownType(a_type) ? kTypes[a_type].id : "other";
    }

    // ---- what KIND of slot a type is, which is not a naming exercise --------
    //
    // ⚠⚠ A TINT SLOT IS TYPED AND THE TYPE DECIDES WHAT BELONGS IN IT. MEASURED
    // 2026-08-16 by parsing the RACE records out of Skyrim.esm: 31 races carry
    // tint slots, 1404 in total, and each one is a TINI index with a TINP type
    // and a TINT texture. Per race and sex the counts are
    //
    //     SkinTone 1, Lips 1, Cheeks 1, LowerCheeks 1, Nose 1, Chin 1, Neck 1,
    //     Forehead 1, FrownLines 1, Eyeliner 1, UpperEyeSocket 1,
    //     LowerEyeSocket 1, WarPaint about 15, Dirt about 3.
    //
    // ⚠⚠ SO EVERY STRUCTURAL TYPE IS ONE SLOT WITH A TEXTURE FIXED BY THE RACE,
    // and that texture is the SHAPE of the feature on that face:
    // FemaleHeadNord_Lips.dds is the outline of Nord lips, not a lipstick.
    // Putting a makeup pack texture in it gives a lip shaped mask that is the
    // wrong shape for the face. RaceMenu offers these a colour and nothing else,
    // which is what the field noticed this page doing differently.
    //
    // Only WarPaint and Dirt are multi-slot, and WarPaint is where makeup
    // actually lives: all 1062 AddWarpaint registrations belong there.
    enum class Category : std::uint8_t {
        kFace,      // one slot, race texture, colour only
        kWarPaint,  // the makeup packs' own library
        kDirt,      // its own small library
        kOther      // a type this build has never heard of
    };

    // ⚠ TYPE 0 IS A SLOT WITH NO TINP AT ALL, AND IT IS DECORATIVE. MEASURED:
    // of 1445 vanilla slots, 41 carry no type subrecord, and their textures name
    // what they are: MaleHead_Frekles_01, FemaleHeadBothiahTattoo_01, the four
    // Forswarn tattoos, spread over Breton, Nord, Dark Elf and Redguard. That is
    // art a character wears rather than a feature of their face, so it belongs
    // with the unknowns, which keep every control. One outlier, a single
    // MaleHeadHuman_ForeHead slot, is structural and gets a texture picker it
    // does not need; the alternative is locking freckles and tattoos, which is
    // worse. Unknown types past the end land here for the same reason: the
    // widest answer, the same call OverlayLocations makes about art no pack
    // registered.
    [[nodiscard]] inline constexpr Category CategoryOf(std::uint32_t a_type) {
        switch (static_cast<Type>(a_type)) {
        case Type::kWarPaint:
            return Category::kWarPaint;
        case Type::kDirt:
            return Category::kDirt;
        case Type::kLips:
        case Type::kCheeks:
        case Type::kEyeliner:
        case Type::kUpperEyeSocket:
        case Type::kLowerEyeSocket:
        case Type::kSkinTone:
        case Type::kFrownLines:
        case Type::kLowerCheeks:
        case Type::kNose:
        case Type::kChin:
        case Type::kNeck:
        case Type::kForehead:
            return Category::kFace;
        default:
            return Category::kOther;
        }
    }

    // ⚠⚠ A FACE SLOT HAS NO TEXTURE PICKER, and this is the rule the whole
    // split exists to carry. Its art is the race's own mask and swapping it is
    // never what the player meant.
    [[nodiscard]] inline constexpr bool AllowsTexture(Category a_category) {
        return a_category != Category::kFace;
    }

    // ⚠⚠ AND NO CLEAR EITHER. Clearing SkinTone does not remove makeup, it
    // removes the character's complexion; emptying Neck removes the neck blend.
    // These are not accessories. User's call 2026-08-16: no clear on a face
    // slot, and a Reset that puts the race's own default back instead.
    [[nodiscard]] inline constexpr bool AllowsClear(Category a_category) {
        return a_category != Category::kFace;
    }

    inline constexpr std::size_t kCategoryCount = 4;

    [[nodiscard]] inline constexpr std::size_t CategoryIndex(Category a_category) {
        return static_cast<std::size_t>(a_category);
    }

    struct CategoryInfo {
        Category    category;
        const char* id;
        const char* labelKey;
    };

    inline constexpr std::array<CategoryInfo, kCategoryCount> kCategories{ {
        { Category::kFace, "face", "$FR_Mk_Face" },
        { Category::kWarPaint, "warpaint", "$FR_Mk_WarPaintSection" },
        { Category::kDirt, "dirt", "$FR_Mk_DirtSection" },
        { Category::kOther, "other", "$FR_Mk_OtherSection" },
    } };

    [[nodiscard]] inline const char* CategoryLabelKey(Category a_category) {
        return kCategories[CategoryIndex(a_category)].labelKey;
    }

    [[nodiscard]] inline const char* CategoryId(Category a_category) {
        return kCategories[CategoryIndex(a_category)].id;
    }

    // ---- what one layer holds -----------------------------------------------
    //
    // ⚠⚠ THE ENGINE KEEPS THE STRENGTH TWICE AND BOTH COPIES MUST BE WRITTEN.
    // MEASURED in Ghidra on both builds. A TintMask is
    //
    //     +0x00  TESTexture*  texture
    //     +0x08  std::uint32_t colour, packed 0xAABBGGRR  (NOT the disk form)
    //     +0x0C  float         alpha
    //     +0x10  std::uint32_t type
    //
    // and the engine's own colour setter (AE FUN_140955a30) writes +0x08 and
    // +0x0C in the same breath.
    //
    // ⚠⚠ THE TOP BYTE IS THE STRENGTH IN BOTH FORMS AND THE REST OF THE WORD
    // IS NOT. An unused layer reads 0x00FFFFFF and a skin tone in use reads
    // 0xFFB79C91: those are PRESET words, off disk. The same skin tone in the
    // live field is 0xFF919CB7, because the live word is ABGR. The alpha claim
    // holds for both only because alpha is byte 3 either way. Read the engine
    // section below before touching either word.
    //
    // So this struct holds ONE strength and MakeupApi writes it into both
    // fields. Two fields, one author, which is the rule
    // [[two-readers-of-one-answer-drift-in-the-gap]] exists for.
    struct LayerState {
        bool             hasTexture{ false };
        std::string      texture;
        OverlayPlan::Rgb tint{};
        // 0 to 1. The engine's float alpha, and the top byte of its colour over
        // 255. Zero is a layer that is off rather than a layer that is black.
        float            strength{ 0.0f };
        // ---- OS-209 on makeup: where the art sits -----------------------------
        //
        // ⚠⚠ `texture` IS ALWAYS THE SOURCE, exactly as it is on an overlay
        // layer. What goes on the mask is a DIFFERENT string when this is not
        // identity: the baked file WirePath derives from the pair. MakeupApi's
        // Read decodes a baked path back through the sidecar beside it, so the
        // page, its labels and its undo history all see the art the player
        // picked and never a hash.
        //
        // ⚠ THE SAME BAKE, NOT A SECOND ONE. A face tint and an overlay that
        // name one source at one transform share one file, because the cache key
        // is the inputs and neither system is in it.
        OverlayTransform::Transform transform;

        friend bool operator==(const LayerState&, const LayerState&) = default;
    };

    // What a layer's mask should hold for a state: the source, or the bake of
    // it. Empty when the layer has no texture of ours, which is the case that
    // means "keep the art the race gave it" and never "blank the slot".
    //
    // ⚠ THE ONE PLACE THAT ANSWERS THIS, so the writer and any test ask the
    // same question. OverlayPlan::WirePath is its sibling and differs in one
    // thing: an overlay has a default texture standing for emptiness, and a
    // makeup layer has no such value because every slot carries the race's art
    // at all times.
    [[nodiscard]] inline std::string WirePath(const LayerState& a_state) {
        if (!a_state.hasTexture || a_state.texture.empty()) {
            return {};
        }
        if (OverlayTransform::IsIdentity(a_state.transform)) {
            return a_state.texture;
        }
        return OverlayTransform::BakedPath(a_state.texture, a_state.transform);
    }

    // One slot on the page, in the order the engine's list holds them.
    struct Layer {
        std::uint32_t index{ 0 };  // position in the player's tint list
        std::uint32_t type{ 0 };   // the mask's own type field

        friend bool operator==(const Layer&, const Layer&) = default;
    };

    // ⚠⚠ A LAYER IS OFF WHEN ITS STRENGTH IS ZERO, NOT WHEN IT HAS NO TEXTURE.
    // This is where makeup differs from an overlay and the presets say so. Every
    // slot in the list carries a texture at all times, because the list is built
    // from the RACE and a race gives every layer its default art; what an unused
    // layer carries is 0x00FFFFFF, a white colour at zero strength. Reading
    // "has a texture" as "in use" would report every character as wearing all
    // fifteen layers of makeup, and the page's own count would never fall.
    [[nodiscard]] inline bool Occupied(const LayerState& a_state) {
        return a_state.strength > 0.0f;
    }

    // ---- whose texture is on the head's tint slot ----------------------------
    //
    // ⚠⚠ THE ENGINE NAMES ITS OWN BAKE, and that name is the only handle we
    // get on "is the face still showing what we painted". Retint()
    // (RELOCATION_ID(51521, 52396)) assigns "Player face tint" to the texture it
    // builds; that string is how the function was found in the first place.
    // Anything ELSE sitting on that slot is somebody else's file.
    //
    // ⚠⚠ AND SOMEBODY ELSE IS skee, WITH A PRESET. FIELD 2026-08-26: the head's
    // tint texture flipped between 'Player face tint' and
    // 'Textures\CharGen\Exported\FR_Umbrael 17.dds' all session, four for four
    // against overlay edits, while a 44 second stretch with no overlay touch
    // never flipped once. r47 measured the same adversary on the load path. The
    // body was unaffected throughout because it reads bodyTintColor, a different
    // field: two painters of one appearance, and this time the HEAD is the stale
    // one.
    //
    // ⚠ EXACT, NOT CASE-FOLDED. The engine writes this literal; a match that
    // accepted near-misses would call somebody else's file ours and stand down
    // at exactly the moment it was needed.
    inline constexpr std::string_view kLiveFaceBakeName = "Player face tint";

    [[nodiscard]] inline constexpr bool IsLiveFaceBake(std::string_view a_name) {
        return a_name == kLiveFaceBakeName;
    }

    // ---- when the watch looks, and how often ---------------------------------
    //
    // ⚠⚠ THE WINDOW IS THE FIGHT GUARD AND IT DOES NOT MOVE. Field 2026-08-26
    // measured the re-bind landing 1.1 s after a head build and 4.1 s after a
    // layer write, and three looks two seconds apart caught both. What it could
    // not do is catch them PROMPTLY: a look every two seconds leaves the face
    // wearing the wrong texture for up to two seconds after we could have seen
    // it, and that gap is what the field reports as a wrong colour that then
    // corrects itself.
    //
    // So the looks subdivide the SAME six seconds rather than extending them.
    // More looks inside the window shorten the wrongness; more looks past it
    // only make the fight with skee last longer, which is the failure the guard
    // exists to prevent.
    inline constexpr int kFaceLookSpacingMs    = 500;
    inline constexpr int kFaceWriteFirstLookMs = 500;
    inline constexpr int kFaceBuildFirstLookMs = 500;
    inline constexpr int kFaceLookCount        = 12;

    // How long a cycle armed at a_firstMs stays alive. The log line and the
    // suite read this rather than each recomputing it, so the window cannot
    // drift away from the number the comment above defends.
    [[nodiscard]] inline constexpr int FaceWatchWindowMs(int a_firstMs) {
        return a_firstMs + kFaceLookSpacingMs * (kFaceLookCount - 1);
    }

    // How long the watch stays quiet after WE apply a face on purpose.
    //
    // ⚠⚠ THE WATCH CANNOT TELL OUR OWN IMPORT FROM SKEE'S RE-BIND, AND IT ATE
    // A LOOK APPLY FOR IT. Both arrive as a named texture under
    // CharGen\Exported that is not the live bake, so the repair fired on the
    // face the user had just chosen. Field 2026-08-27, importing 'Almalexia'
    // onto Umbrael:
    //
    //   01:20:02.000  ProfileApply: 'Almalexia' complete
    //   01:20:02.803  the head's tint slot carries 'FR_Almalexia.dds' ... re-baked
    //   01:20:03.162  HEAD TEXTURE ... 'FR_Almalexia.dds' -> 'Player face tint'
    //
    // ⚠ LONGER THAN THE WATCH'S OWN WINDOW, ON PURPOSE. A look apply keeps
    // rebuilding the head for seconds after it reports complete, and a quiet
    // period that expired inside that would let the last rebuild reach the very
    // face the apply had just imported.
    inline constexpr int kFaceWatchQuietAfterApplyMs = 10000;

    // ---- what the head's tint slot is actually carrying -----------------------
    //
    // ⚠⚠ FOUR ANSWERS AND NOT TWO, AND EVERY PAIR ANYONE CONFLATED HAS COST
    // A ROUND. Field 2026-08-26: the delayed watch read an unnamed texture as
    // "no head to read" and stood down, and HeadBuildHook's late-build answer
    // reads the same emptiness as "another mod's business" and returns false.
    // Same mistake twice, and the state they both miss is the one a REBUILD
    // leaves behind: a fresh facegen material carries a tint texture the engine
    // has not named yet, and the composite that used to be on it is gone.
    //
    // The name is the only handle any of them have, so the verdict is derived
    // once, here, where a test can pin it, instead of by each caller's own idea
    // of what an empty string means.
    // ⚠⚠ RaceMenu's EXPORT FOLDER, WHICH IS NOT "SOMEBODY ELSE'S TEXTURE".
    // A face under CharGen\Exported was baked by somebody on purpose, by this
    // mod's own look export or by RaceMenu's, and a preset import binds one.
    // Reading it as a displacement is how the skin tone slider came to replace
    // an imported face with a freshly composited one (field 2026-08-27,
    // 05:41:19 on 'FR_Umbrael 19.dds'). ⚠ A COMPOSITE AND A PRE-BAKED IMAGE
    // CANNOT BOTH WIN: re-baking builds a new picture out of the tint list, so
    // keeping the export means the tone reaches the body and not the head.
    [[nodiscard]] inline constexpr bool IsChargenExport(std::string_view a_name) {
        constexpr std::string_view needle = "chargen\\exported\\";
        const auto                 fold   = [](char a_c) constexpr {
            if (a_c >= 'A' && a_c <= 'Z') {
                return static_cast<char>(a_c - 'A' + 'a');
            }
            return a_c == '/' ? '\\' : a_c;
        };
        if (a_name.size() < needle.size()) {
            return false;
        }
        for (std::size_t at = 0; at + needle.size() <= a_name.size(); ++at) {
            std::size_t i = 0;
            for (; i < needle.size(); ++i) {
                if (fold(a_name[at + i]) != needle[i]) {
                    break;
                }
            }
            if (i == needle.size()) {
                return true;
            }
        }
        return false;
    }

    // Whether a bound texture and a held export path name the SAME file,
    // ignoring case and slash direction the way IsChargenExport does.
    //
    // ⚠⚠ THE ONE QUESTION THE kPresetFace ARM COULD NOT ASK, and the whole of
    // the cell-change head-skin report turned on it. That arm re-bakes over any
    // chargen export, because an export is how a face baked for a PREVIOUS
    // character survives into the next save (field r88, "when i load my save
    // Umbrael's head can literally be another color"). Residue and the look the
    // player is wearing RIGHT NOW are both exports, so it could not tell them
    // apart and rebaked both.
    //
    // MEASURED on the author's own rig 2026-09-01, walking out of a building:
    //
    //   15:27:26.368  a head build binds 'FR_Almalexia.dds' and arms the watch
    //   15:27:26.967  the watch calls that export a preset face and re-bakes it
    //   15:27:27.903  the head wears 'Player face tint' instead
    //
    // The look had been applied 70 seconds earlier. Nothing was stale.
    //
    // ⚠ SO THE GATE IS IDENTITY, NOT A STAND-DOWN. LookFaceTint::Held names the
    // jslot this character's face was bound from, set by the one place that
    // binds it. An export that MATCHES it is this face, on purpose. Every other
    // export is still residue and is still re-baked, which is the r88 cure
    // untouched: a bled face carries a jslot this character does not hold.
    //
    // ⛔ NOT THE g_holdSkin GATE. That one is load bearing and left a face with
    // no composite for three and a half minutes on 2026-08-26.
    [[nodiscard]] inline constexpr bool NamesTheSameExport(std::string_view a_bound,
                                                          std::string_view a_held) {
        if (a_bound.empty() || a_held.empty() || a_bound.size() != a_held.size()) {
            return false;
        }
        const auto fold = [](char a_c) constexpr {
            if (a_c >= 'A' && a_c <= 'Z') {
                return static_cast<char>(a_c - 'A' + 'a');
            }
            return a_c == '/' ? '\\' : a_c;
        };
        for (std::size_t i = 0; i < a_bound.size(); ++i) {
            if (fold(a_bound[i]) != fold(a_held[i])) {
                return false;
            }
        }
        return true;
    }

    enum class FaceTintVerdict {
        kNoHead,         // no facegen head in the scene: nothing to judge
        kOurs,           // the live bake is bound; the face shows what we painted
        kCompositeGone,  // a head with an unnamed tint: a rebuild threw it away
        kPresetFace,     // an exported face, bound because somebody chose it
        kDisplaced,      // somebody else's named texture is bound over ours
    };

    [[nodiscard]] inline constexpr FaceTintVerdict JudgeFaceTint(
        bool a_haveHead, std::string_view a_name) {
        if (!a_haveHead) {
            return FaceTintVerdict::kNoHead;
        }
        if (a_name.empty()) {
            return FaceTintVerdict::kCompositeGone;
        }
        if (IsLiveFaceBake(a_name)) {
            return FaceTintVerdict::kOurs;
        }
        return IsChargenExport(a_name) ? FaceTintVerdict::kPresetFace
                                       : FaceTintVerdict::kDisplaced;
    }

    // ---- the PRESET's packed colour ------------------------------------------
    //
    // The same 0xAARRGGBB the presets store and OverlayPlan::PackTint already
    // builds. The reuse is deliberate and it is SCOPED: this word is the disk
    // form and skee's override keys, and it is never the live tint mask.
    //
    // ⚠⚠ REUSING IT FOR THE LIVE FIELD IS THE BUG OF 2026-08-16. One packing
    // serving two destinations is exactly what broke, so the engine's word has
    // its own pair below and the two meet only in PresetToEngine and
    // EngineToPreset.

    [[nodiscard]] inline constexpr std::uint32_t PackColour(const OverlayPlan::Rgb& a_tint,
                                                            float a_strength) {
        return OverlayPlan::PackTint(a_tint.r, a_tint.g, a_tint.b,
                                     OverlayPlan::AlphaByte(a_strength));
    }

    // ⚠ THE STRENGTH COMES OUT OF THE TOP BYTE HERE AND NOT OUT OF THE FLOAT,
    // and only because this is the PRESET path, where there is no float to read:
    // a .jslot carries one packed integer per layer. A live layer is read from
    // the engine's float, which is the authority, and MakeupApi does that.
    [[nodiscard]] inline constexpr float StrengthFromPacked(std::uint32_t a_packed) {
        return static_cast<float>((a_packed >> 24) & 0xFFu) / 255.0f;
    }

    [[nodiscard]] inline constexpr OverlayPlan::Rgb TintFromPacked(std::uint32_t a_packed) {
        return OverlayPlan::UnpackTint(a_packed);
    }

    [[nodiscard]] inline constexpr float ClampStrength(float a_strength) {
        return OverlayPlan::ClampAlpha(a_strength);
    }

    // ---- the ENGINE's form, which is not the preset's ------------------------
    //
    // ⚠⚠ THE FIELD AT TintMask+0x08 IS ABGR AND THE PRESET ON DISK IS ARGB.
    // Field report 2026-08-16: "when i set the color of makeup to pure red, it
    // appears blue, when i set it to pure blue it appears red", with green
    // unaffected. That is the signature of exactly this swap and nothing else.
    //
    // MEASURED, two ways that do not depend on each other:
    //
    //  * `RE::Color` (RE/C/Color.h) is four BYTES in the order red, green, blue,
    //    alpha, at offsets 0, 1, 2, 3. On little-endian x64 those bytes read as
    //    one `std::uint32_t` are `red | green<<8 | blue<<16 | alpha<<24`, which
    //    is **0xAABBGGRR**.
    //  * The 331 installed presets say the DISK form is the other one. Across
    //    329 SkinTone layers, reading the low 24 bits as RRGGBB gives a warm
    //    colour 67.8% of the time and reading them as BBGGRR gives one 23.4% of
    //    the time, and the individual values settle it: rgb(183,156,145) and
    //    rgb(167,134,122) are skin, while their mirrors rgb(145,156,183) and
    //    rgb(122,134,167) are a cool blue-grey nobody's face is.
    //
    // So RaceMenu swizzles when it writes a .jslot, and the two forms are both
    // real. Neither may be used for the other's job.
    //
    // ⚠ THE ALPHA DOES NOT MOVE, and that is why the field saw green survive as
    // well. Alpha is byte 3 in the struct, which is the TOP byte on
    // little-endian, and it is the top byte of the preset word too. Only red and
    // blue trade places; green sits in the middle of both.

    [[nodiscard]] inline constexpr std::uint32_t PackEngineColour(const OverlayPlan::Rgb& a_tint,
                                                                  float a_strength) {
        return (static_cast<std::uint32_t>(OverlayPlan::AlphaByte(a_strength)) << 24) |
               (static_cast<std::uint32_t>(a_tint.b) << 16) |
               (static_cast<std::uint32_t>(a_tint.g) << 8) |
               static_cast<std::uint32_t>(a_tint.r);
    }

    [[nodiscard]] inline constexpr OverlayPlan::Rgb EngineTintFrom(std::uint32_t a_packed) {
        return OverlayPlan::Rgb{ static_cast<std::uint8_t>(a_packed & 0xFFu),
                                 static_cast<std::uint8_t>((a_packed >> 8) & 0xFFu),
                                 static_cast<std::uint8_t>((a_packed >> 16) & 0xFFu) };
    }

    // Same top byte as the preset form, for the reason written out above. Kept
    // as its own name anyway, so a caller states which word it is holding.
    [[nodiscard]] inline constexpr float EngineStrengthFrom(std::uint32_t a_packed) {
        return static_cast<float>((a_packed >> 24) & 0xFFu) / 255.0f;
    }

    // The two forms, converted. Anything that reads a preset into a live layer
    // goes through here rather than assuming the words are interchangeable.
    //
    // ⚠⚠ KEEP THESE EVEN THOUGH NOTHING OUTSIDE THE TESTS CALLS THEM YET. The
    // whole preset quartet (PackColour, TintFromPacked, StrengthFromPacked and
    // this pair) has no production caller, because .jslot makeup import is not
    // built. A dead-code sweep would take them, and the day import lands
    // somebody reaches for the engine packer to read a disk word and the 2026-08-16
    // red-for-blue bug comes straight back. They are the bridge, kept on purpose.
    [[nodiscard]] inline constexpr std::uint32_t PresetToEngine(std::uint32_t a_preset) {
        return PackEngineColour(TintFromPacked(a_preset), StrengthFromPacked(a_preset));
    }

    [[nodiscard]] inline constexpr std::uint32_t EngineToPreset(std::uint32_t a_engine) {
        return PackColour(EngineTintFrom(a_engine), EngineStrengthFrom(a_engine));
    }

    // ---- names ---------------------------------------------------------------

    // The name a card and a row show for a layer's art. Shared with the overlays
    // page so one texture never reads as two different things depending on which
    // list it turned up in.
    [[nodiscard]] inline std::string DisplayName(std::string_view a_path) {
        return OverlayPlan::DisplayName(a_path);
    }

    // ---- the snapshot --------------------------------------------------------
    //
    // ⚠⚠ MAKEUP REVERTS RATHER THAN UNDOING, AND THAT IS AN ADVANTAGE THE
    // OVERLAYS PAGE DOES NOT HAVE. Its history exists because skee cannot be
    // asked what an override held before it was written, so the only record of a
    // previous appearance is the one the page kept. Tint masks CAN be read back
    // off the live character, so the state the player walked in with is a real
    // measurement rather than a remembered claim, and one button restores it.
    using Snapshot = std::vector<LayerState>;

    // Which slots differ, so a revert costs the engine the layers that actually
    // moved rather than the whole list. Same shape as OverlayPlan's diff and for
    // the same reason: the retint after a batch is a texture rebuild.
    [[nodiscard]] inline std::vector<std::size_t> Differences(const Snapshot& a_from,
                                                              const Snapshot& a_to) {
        std::vector<std::size_t> out;
        const std::size_t        count = a_from.size() < a_to.size() ? a_to.size() : a_from.size();
        for (std::size_t i = 0; i < count; ++i) {
            const bool haveFrom = i < a_from.size();
            const bool haveTo   = i < a_to.size();
            if (!haveFrom || !haveTo || !(a_from[i] == a_to[i])) {
                out.push_back(i);
            }
        }
        return out;
    }

    [[nodiscard]] inline std::size_t UsedIn(const Snapshot& a_state) {
        std::size_t used = 0;
        for (const auto& layer : a_state) {
            if (Occupied(layer)) {
                ++used;
            }
        }
        return used;
    }

    // Worn layers of one kind. Takes the layer list beside the snapshot because
    // the type lives on the layer and the strength lives on the state, and the
    // two are parallel by index.
    //
    // ⚠ A SHORT LAYER LIST DOES NOT DROP THE TAIL, it counts it as unknown. The
    // two come from one read and cannot normally disagree, but a caller that
    // hands in a stale pair would otherwise silently lose slots from the total,
    // and a budget line that understates what is worn is the one thing this
    // count exists to prevent.
    [[nodiscard]] inline std::size_t UsedIn(const Snapshot&           a_state,
                                            const std::vector<Layer>& a_layers,
                                            Category                  a_category) {
        std::size_t used = 0;
        for (std::size_t i = 0; i < a_state.size(); ++i) {
            if (!Occupied(a_state[i])) {
                continue;
            }
            const auto type = i < a_layers.size() ? a_layers[i].type : 0u;
            if (CategoryOf(type) == a_category) {
                ++used;
            }
        }
        return used;
    }

    // ---- the apply-side slot match -------------------------------------------
    //
    // ⚠⚠ AN INDEX IS NOT A LAYER'S IDENTITY ACROSS SESSIONS. Field 2026-08-22
    // (round fourteen): the live tint list is authored per session, RaceMenu's
    // packs insert and move layers, and the same look applied against a
    // reshaped list wrote its war paint into a stranger's slot at the captured
    // index, while every earlier round had silently skipped that entry on the
    // type guard. Same index, same type, different layer. The layer's own
    // texture path is the identity a capture can trust; the captured index is
    // only a hint, and only for colour-only captures of the structural band.
    //
    // The rules, in the order they are tried:
    //   1. Same type, same texture path: the slot IS the captured layer,
    //      wherever the list moved it. Reapplies land here and stay put.
    //   2. The type occurs exactly once in the live list: structural slots
    //      like the skin tone match by type alone, whatever art a pack gave
    //      them this session.
    //   3. The capture carries a texture and a same-type slot is FREE
    //      (strength zero): the write installs the texture there. This is how
    //      a payload like a war paint lands on a character that never wore
    //      it, without ever stomping a paint somebody else put on.
    //   4. A colour-only capture falls back to its captured index when the
    //      type there still matches. That is the old rule, and for a capture
    //      that owns no texture it is still the best claim there is.
    //   5. Nothing. Skipping is honest; writing into a slot that belongs to
    //      something else is the round-fourteen skull.
    inline constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

    // Case-insensitive, slash-insensitive. Both sides come from a TESTexture
    // or a capture of one, but packs and captures disagree on case and the
    // occasional forward slash, and a path miss here silently demotes rule 1
    // to rule 3.
    [[nodiscard]] inline bool SameTexturePath(std::string_view a_lhs, std::string_view a_rhs) {
        if (a_lhs.size() != a_rhs.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a_lhs.size(); ++i) {
            char x = a_lhs[i];
            char y = a_rhs[i];
            if (x == '/') x = '\\';
            if (y == '/') y = '\\';
            if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
            if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
            if (x != y) {
                return false;
            }
        }
        return true;
    }

    // Whether a mask's texture names the skin tone mask, the one file every
    // race and every preset calls SkinTone.dds.
    //
    // ⚠⚠ THE TYPE FIELD IS NOT ENOUGH ON A LIST THAT IS NOT THIS RACE'S. A
    // reloaded list reads every type as zero, and when its length does not
    // match the race's slots there is no race record to take the type from.
    // Field 2026-09-02 02:58: Nord 3 applied over a 108 slot list on a 34 slot
    // race, every slot read as type 0, the clear-only pass took slot 0 for a
    // clearable stranger and wrote the SKIN TONE to alpha 0, and the tone
    // re-assert found no tone slot at all and repainted every heartbeat. The
    // file name is the identity the type cannot give.
    [[nodiscard]] inline bool IsSkinToneTexture(std::string_view a_path) {
        constexpr std::string_view name = "skintone.dds";
        if (a_path.size() < name.size()) {
            return false;
        }
        const auto tail = a_path.substr(a_path.size() - name.size());
        for (std::size_t i = 0; i < name.size(); ++i) {
            char c = tail[i];
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if (c != name[i]) {
                return false;
            }
        }
        // A bare "skintone.dds" or one behind a separator; "myskintone.dds"
        // is somebody else's art.
        const std::size_t at = a_path.size() - name.size();
        return at == 0 || a_path[at - 1] == '\\' || a_path[at - 1] == '/';
    }

    // Which live slot a captured layer belongs to, or kNoSlot. a_claimed marks
    // slots earlier entries of the same apply already took, so two captured
    // war paints land on two canvases rather than fighting over one.
    [[nodiscard]] inline std::size_t MatchSlot(std::uint32_t             a_capturedIndex,
                                               std::uint32_t             a_capturedType,
                                               const LayerState&         a_captured,
                                               const std::vector<Layer>& a_layers,
                                               const Snapshot&           a_live,
                                               const std::vector<bool>&  a_claimed) {
        const std::size_t n = a_layers.size() < a_live.size() ? a_layers.size()
                                                              : a_live.size();
        const auto        open = [&](std::size_t i) {
            return i < a_claimed.size() ? !a_claimed[i] : true;
        };
        const bool hasTexture = a_captured.hasTexture && !a_captured.texture.empty();
        if (hasTexture) {
            for (std::size_t i = 0; i < n; ++i) {
                if (open(i) && a_layers[i].type == a_capturedType &&
                    SameTexturePath(a_live[i].texture, a_captured.texture)) {
                    return i;
                }
            }
        }
        // ⚠⚠ THE TEXTURE ALONE, BECAUSE THE TYPE LABEL MOVES AND THE PATH DOES
        // NOT. Field 2026-08-23, one session, one character: a layer captured
        // at index 11 as a war paint was read back at index 11 as FRECKLES an
        // eighty seconds later, because the face step reshapes the tint list
        // on its way past. The pass above wants type AND texture, so it fell
        // through, and the payload landed in the first free war paint canvas
        // instead of on the slot already wearing its own texture. The result
        // is the same art composited twice, once by the preset and once by
        // us, which is what the field saw as a colour that had changed.
        //
        // A texture path is a stronger identity than a type label: the label
        // is the engine's grouping and the path is what the layer IS. This
        // pass never overrides the one above, it only catches what that one
        // dropped, so nothing that matched before matches differently now.
        if (hasTexture) {
            for (std::size_t i = 0; i < n; ++i) {
                if (open(i) && SameTexturePath(a_live[i].texture, a_captured.texture)) {
                    return i;
                }
            }
        }
        {
            std::size_t hit  = kNoSlot;
            std::size_t hits = 0;
            for (std::size_t i = 0; i < n; ++i) {
                if (a_layers[i].type == a_capturedType) {
                    hit = i;
                    ++hits;
                }
            }
            if (hits == 1 && open(hit)) {
                return hit;
            }
        }
        if (hasTexture) {
            for (std::size_t i = 0; i < n; ++i) {
                if (open(i) && a_layers[i].type == a_capturedType && !Occupied(a_live[i])) {
                    return i;
                }
            }
            // ⚠ NO INDEX FALLBACK FOR A PAYLOAD. Every same-type slot is worn
            // by something this capture does not own.
            return kNoSlot;
        }
        if (a_capturedIndex < n && a_layers[a_capturedIndex].type == a_capturedType &&
            open(a_capturedIndex)) {
            return a_capturedIndex;
        }
        return kNoSlot;
    }

    // ---- the face-carried write ----------------------------------------------
    //
    // ⚠⚠ A FACE BLOCK OWNS THE FACE TEXTURE AND THE SKIN TONE, AND THE LIST
    // OWNS EVERY RETINT AFTER IT. r40 field: writing the WHOLE capture beside a
    // face block ended in a retint that rebuilt the face out of the tint list
    // and threw away what the preset painted, so for a long time the step stood
    // aside entirely when a face was carried. The user's A/B of 2026-09-01 is
    // the other half: a look applied whole left its makeup empty, and applying
    // the makeup alone afterwards loaded it perfectly, through the very write
    // the stand-aside withheld.
    //
    // ⚠⚠ AND THE FIRST CUT OF THIS PLAN DREW THE LINE ONE SLOT TOO WIDE. It
    // kept every kFace-category entry out of the write, complexion being the
    // preset's, and the field answered the same evening: the LIPS loaded with
    // the look and went bare the moment a face overlay was adjusted, because
    // the edit's retint composites from the list and the list never got the
    // lips colour. skee's preset apply paints from the preset's own baked
    // texture and does not populate the live list (the r37 measurement), so
    // anything the capture holds back is simply absent from every list-built
    // face. What stays out is the SKIN TONE alone: writing it would re-arm
    // HoldSkinTone after the apply deliberately released the hold (a held tone
    // fights the export on every later head build), and the settle refresh
    // already paints the body from the captured tone.
    //
    // The plan REPLACES rather than adds, the same call StepOverlays makes: a
    // slot the capture does not claim is cleared, so the previous look's
    // markings never survive by omission. ⚠⚠ THE ERASER ASKS THE WRITE'S OWN
    // QUESTION, everything but the tone, and the second field round of
    // 2026-09-01 is why. The first eraser asked AllowsClear, which spares the
    // kFace band, and Almalexia's LIPS then outlived her: Umbrael captures no
    // lips, the slot went unclaimed, unclearable, and her next retint wore a
    // stranger's lipstick. A mark and its eraser must ask one question. The
    // 2026-08-16 rule is untouched where it was made: AllowsClear still guards
    // the USER's clear controls, where emptying a complexion slot destroys the
    // character. A look REPLACE is a different statement: the look's bake is
    // the whole face now, and what its capture does not hold it did not have.
    struct FaceCarriedPlan {
        Snapshot                 target;        // what to write, parallel to the live list
        std::vector<std::size_t> indices;       // the slots the write names
        std::size_t              written{ 0 };  // captured layers that landed
        std::size_t              cleared{ 0 };  // stale paint taken out
        std::size_t              skipped{ 0 };  // captured layers with no slot to land in
        std::size_t              kept{ 0 };     // skin tone entries left to the preset
    };

    // a_captured is ProfileCodec's makeup block (any entry type carrying
    // .index, .type and .state), kept a template so this header stays below
    // the codec in the include order.
    template <class Entry>
    [[nodiscard]] inline FaceCarriedPlan PlanFaceCarried(
        const std::vector<Entry>& a_captured, const std::vector<Layer>& a_layers,
        const Snapshot& a_live) {
        FaceCarriedPlan plan;
        plan.target = a_live;
        std::vector<bool> claimed(plan.target.size(), false);
        for (const auto& entry : a_captured) {
            if (entry.type == static_cast<std::uint32_t>(Type::kSkinTone)) {
                ++plan.kept;
                continue;
            }
            if (!Occupied(entry.state)) {
                continue;  // a layer the capture holds at zero adds nothing
            }
            const std::size_t i = MatchSlot(entry.index, entry.type, entry.state,
                                            a_layers, plan.target, claimed);
            if (i == kNoSlot) {
                ++plan.skipped;
                continue;
            }
            auto next = entry.state;
            if (!next.hasTexture) {
                // The capture did not own the texture, so the slot keeps the
                // one the race (or the jslot) put there.
                next.hasTexture = plan.target[i].hasTexture;
                next.texture    = plan.target[i].texture;
            }
            claimed[i]     = true;
            plan.target[i] = std::move(next);
            plan.indices.push_back(i);
            ++plan.written;
        }
        const std::size_t n = a_layers.size() < plan.target.size()
                                  ? a_layers.size()
                                  : plan.target.size();
        for (std::size_t i = 0; i < n; ++i) {
            if (a_layers[i].type ==
                    static_cast<std::uint32_t>(Type::kSkinTone) ||
                claimed[i]) {
                continue;
            }
            // ⚠ AND THE TONE BY ITS FILE NAME, because a list that is not this
            // race's reads every type as zero (field 2026-09-02 02:58), and an
            // eraser that took the tone for a stranger would remove the face.
            if (IsSkinToneTexture(plan.target[i].texture)) {
                continue;
            }
            if (!plan.target[i].hasTexture && plan.target[i].strength <= 0.0f) {
                continue;  // already empty, so nothing to say or write
            }
            plan.target[i] = LayerState{};
            plan.indices.push_back(i);
            ++plan.cleared;
        }
        return plan;
    }

    // ---- the character's SAVED tint layers, the third copy -------------------
    //
    // ⚠⚠ THE PLAYER CARRIES A THIRD COPY OF THE LIST AND THE ENGINE RESTORES
    // FROM IT. MEASURED in Ghidra on AE 1.6.1170, 2026-09-02, in the character
    // editor's init (RaceSexMenu, AddrLib 52409): it first rebuilds the live
    // list from the race's tint assets (40696: texture, type and default per
    // asset) and then, for every asset, looks the actor BASE's saved tint layer
    // up by the asset's TINI index and copies that layer's colour and its
    // interpolation onto the live mask (40697, through TESNPC's layer lookup
    // 24782). That is `TESNPC::tintLayers` on the player's own base: written by
    // chargen, carried by the vanilla save, and untouched by every write this
    // mod ever made into the live list.
    //
    // FIELD 2026-09-02 01:47, the Nord 3 leftovers' second life: Nord 3's face
    // was clean after the load, the user opened RaceMenu, and Umbrael's six
    // makeup layers came back at the exact indices and alphas of her preset
    // (11 at 0.388, 13 at 0.333, 14 to 17 at 1.0) the moment the editor
    // rebuilt the list, because nothing since her own chargen had rewritten
    // those layers. Every slider drag then baked them, skee's export re-bind
    // hid them, and the close edge captured them as the character's own.
    //
    // So a write into the live list is mirrored into the saved layers, the
    // same law the overlay and tint-list records follow: a hold must be
    // refreshed by every writer of what it holds. This is the pure half: which
    // layer each live slot needs, keyed by the race asset's TINI index at the
    // same position. A slot with no asset behind it has no key and is skipped,
    // and the engine could never restore it either.
    struct SavedLayer {
        std::uint16_t    tintIndex{ 0 };
        OverlayPlan::Rgb tint{};
        float            strength{ 0.0f };
    };

    // What the engine stores for a layer's strength and reads back as a signed
    // byte times 0.01: an integer percentage. Clamped to the byte's honest
    // range, because 0.388 has to survive the trip as 39 and not as garbage.
    [[nodiscard]] inline constexpr std::uint16_t InterpolationOf(float a_strength) {
        const float clamped = ClampStrength(a_strength);
        const int   percent = static_cast<int>(clamped * 100.0f + 0.5f);
        return static_cast<std::uint16_t>(percent < 0 ? 0 : (percent > 100 ? 100 : percent));
    }

    inline constexpr int kNoTintIndex = -1;

    // One layer per live slot that has a race asset behind it, cleared slots
    // included: a layer at zero is the honest mirror of a slot the character
    // does not wear, where an absent layer would let the engine restore the
    // asset's default instead. a_tintIndices is parallel to the live list and
    // carries kNoTintIndex where the race has no asset at that position.
    [[nodiscard]] inline std::vector<SavedLayer> PlanSavedLayers(
        const Snapshot& a_live, const std::vector<int>& a_tintIndices) {
        std::vector<SavedLayer> out;
        const std::size_t n = a_live.size() < a_tintIndices.size() ? a_live.size()
                                                                    : a_tintIndices.size();
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            if (a_tintIndices[i] < 0 || a_tintIndices[i] > 0xFFFF) {
                continue;
            }
            SavedLayer layer;
            layer.tintIndex = static_cast<std::uint16_t>(a_tintIndices[i]);
            layer.tint      = a_live[i].tint;
            layer.strength  = ClampStrength(a_live[i].strength);
            out.push_back(layer);
        }
        return out;
    }

    // ---- the worn-set watch's vocabulary (field 2026-09-02 03:44) ------------
    //
    // ⚠⚠ RACEMENU REPLAYS ITS OWN COPY OF THE TINT LIST, BY INDEX, AND THE ONLY
    // HANDLE ON IT IS WHAT THE CHANGE LOOKS LIKE. Measured 03:11 and 03:13:
    // about a second after every load and every LoadCharacterEx, texture,
    // colour and alpha per slot and the rest cleared, no write of ours beside
    // it. And at 03:44:49, inside the character editor, seven seconds after it
    // opened and right after a slider's head build: the four chargen tints from
    // the 03:43 close over the look's six. So the copy is the list as it stood
    // at the last RaceSexMenu close, and a time window from the editor's open
    // (three seconds) missed it.
    //
    // A slider moves ONE slot per tick. The replay moves many in one tick, and
    // its content is the copy. A wholesale change that is NOT the copy is the
    // user's too: a preset loaded inside RaceMenu's own Presets tab rewrites
    // the whole list on purpose, and putting our record over that would undo
    // the user's own action. Three answers, one table, pinned by the suite.
    struct WornSlot {
        std::uint32_t index{ 0 };
        std::uint8_t  alpha{ 0 };    // the strength byte; a worn slot is above zero
        std::string   texture;       // the mask's art, so a swap at one strength is a change
        bool          tone{ false }; // the skin tone slot, which the copy match ignores
    };

    struct WornSet {
        std::vector<WornSlot> slots;      // worn slots only, in list order
        std::uint32_t         size{ 0 };  // the list's length; when it moves, it was rebuilt
    };

    enum class WornChange { kNone, kSingle, kWholesale };

    // How many slots differ between two readings: came, went, or changed
    // strength or art. The tone counts here, because a tone drag is one slot.
    [[nodiscard]] inline std::size_t WornSlotsChanged(const WornSet& a_before,
                                                      const WornSet& a_after) {
        std::size_t changed = 0;
        for (const auto& b : a_before.slots) {
            const WornSlot* match = nullptr;
            for (const auto& a : a_after.slots) {
                if (a.index == b.index) {
                    match = &a;
                    break;
                }
            }
            if (!match || match->alpha != b.alpha ||
                !SameTexturePath(match->texture, b.texture)) {
                ++changed;
            }
        }
        for (const auto& a : a_after.slots) {
            bool found = false;
            for (const auto& b : a_before.slots) {
                if (b.index == a.index) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                ++changed;
            }
        }
        return changed;
    }

    // A change of LENGTH is a rebuild and wholesale on its own.
    [[nodiscard]] inline WornChange ClassifyWornChange(const WornSet& a_before,
                                                       const WornSet& a_after) {
        if (a_before.size != a_after.size) {
            return WornChange::kWholesale;
        }
        const auto changed = WornSlotsChanged(a_before, a_after);
        if (changed == 0) {
            return WornChange::kNone;
        }
        return changed == 1 ? WornChange::kSingle : WornChange::kWholesale;
    }

    // Whether two readings wear the same set: the same indices at the same
    // strengths. The tone is left out, because the close edge re-asserts a held
    // tone and the copy may be taken either side of that. The length is left
    // out, because the copy replays onto whatever list is live (34 at the
    // close, 108 at the replay). The art is left out, because the copy carries
    // the jslot's paths and the list ours.
    [[nodiscard]] inline bool SameWornSet(const WornSet& a_lhs, const WornSet& a_rhs) {
        const auto counted = [](const WornSet& a_set) {
            std::size_t n = 0;
            for (const auto& s : a_set.slots) {
                if (!s.tone && s.alpha != 0) {
                    ++n;
                }
            }
            return n;
        };
        if (counted(a_lhs) != counted(a_rhs)) {
            return false;
        }
        for (const auto& l : a_lhs.slots) {
            if (l.tone || l.alpha == 0) {
                continue;
            }
            bool hit = false;
            for (const auto& r : a_rhs.slots) {
                if (!r.tone && r.index == l.index && r.alpha == l.alpha) {
                    hit = true;
                    break;
                }
            }
            if (!hit) {
                return false;
            }
        }
        return true;
    }

    enum class EditorVerdict {
        kNothing,           // no change to judge
        kInit,              // the editor's own init, inside its first seconds: put the record back
        kUsersSlot,         // one slot moved: the user's slider, left alone
        kReplay,            // many slots moved and they are RaceMenu's copy: put the record back
        kUsersWholesale,    // many slots moved and they are not the copy: the user's preset, left alone
        kUnknownWholesale,  // many slots moved and no copy is on record: left alone, and said
    };

    [[nodiscard]] inline constexpr EditorVerdict JudgeEditorChange(bool       a_withinInit,
                                                                   WornChange a_change,
                                                                   bool       a_copyKnown,
                                                                   bool       a_matchesCopy) {
        if (a_change == WornChange::kNone) {
            return EditorVerdict::kNothing;
        }
        if (a_withinInit) {
            return EditorVerdict::kInit;
        }
        if (a_change == WornChange::kSingle) {
            return EditorVerdict::kUsersSlot;
        }
        if (!a_copyKnown) {
            return EditorVerdict::kUnknownWholesale;
        }
        return a_matchesCopy ? EditorVerdict::kReplay : EditorVerdict::kUsersWholesale;
    }

    // Whether a change that was not ours, seen outside the editor, is
    // RaceMenu's replay and so names its copy: wholesale, on a list that
    // exists, and either the first such change after a load edge or one in
    // the seconds after an apply.
    //
    // ⚠ THE FIRST AFTER A LOAD, NOT A CHANGE INSIDE A SHORT WINDOW. Measured
    // 2026-09-02 over four loads: the replay landed 11, 16, 25 and 32 seconds
    // after the post-load edge, and a ten second window remembered none of
    // them, so the first editor visit after a load had no copy to judge a
    // replay against. After an apply the replay came 0.6 s after
    // LoadCharacterEx (03:13), so that window stays short. The revert's empty
    // list is never a replay.
    [[nodiscard]] inline constexpr bool RemembersAsCopy(WornChange    a_change,
                                                        std::uint32_t a_size,
                                                        bool          a_firstAfterLoad,
                                                        bool          a_afterApply) {
        return a_change == WornChange::kWholesale && a_size != 0 &&
               (a_firstAfterLoad || a_afterApply);
    }

    // ---- the list after a race switch (r35, closed 2026-09-02) ---------------
    //
    // ⚠⚠ THE ENGINE NEVER REBUILDS THE PLAYER'S TINT LIST ON A RACE SWITCH,
    // ONLY THE CHARACTER EDITOR'S INIT DOES. Measured r35 (2026-08-23): a Nord
    // switched to a 108 slot race kept his 34 slot list at +0.25 s and still at
    // +8 s, and every writer of ours refused it, correctly. Measured 03:44:42:
    // the moment RaceMenu opened the list became 108 and the look's six were
    // written. So the apply runs the same rebuild itself. The trigger is the
    // apply's own knowledge, a race or sex switch (a sex flip on one race
    // switches the race's HALF, male masks on a female character, same
    // length, other art), plus the measurable r35 shape for a list an earlier
    // apply left behind. No race data means nothing to rebuild from.
    [[nodiscard]] inline constexpr bool NeedsRaceRebuild(bool        a_switched,
                                                         std::size_t a_raceSlots,
                                                         std::size_t a_liveSize) {
        if (a_raceSlots == 0) {
            return false;
        }
        return a_switched || a_liveSize != a_raceSlots;
    }

    // ---- the ceiling, and it is a HARD one -----------------------------------
    //
    // ⚠⚠ TOO MANY WORN LAYERS CRASHES THE GAME. Field 2026-08-16: the game CTD'd
    // on going past fifteen. The crash is an access violation on a refcount,
    // `lock xadd [rcx+8],eax`, reached from inside the engine's own face retint
    // (RELOCATION_ID(51521, 52396), AE 0x954AE0) at +0xFD, which is where it
    // builds the "Player face tint" texture. Our call to that retint is the last
    // frame before the engine, so nothing above it can prevent this: the guard
    // has to stop the WRITE.
    //
    // ⚠ WHY THERE IS A CEILING AT ALL, MEASURED IN GHIDRA: the face tint is
    // composited by the imagespace shader `ISAlphaBlend`, AE FUN_141488380,
    // which declares exactly SIXTEEN tint mask sampler slots, TintMask0 through
    // TintMask15, and sixteen matching colour constants, Color through
    // Color[15]. There is no seventeenth slot to bind, so a seventeenth worn
    // layer has nowhere to go.
    //
    // ⚠ FIFTEEN RATHER THAN SIXTEEN, AND THE LAST STEP IS INFERRED RATHER THAN
    // MEASURED. The shader has sixteen slots and the field crashed above
    // fifteen, which fits slot 0 being the skin tone every character carries,
    // leaving fifteen for everything else. That reading is consistent with both
    // facts but the reservation itself has not been read out of the binary. The
    // conservative number is the one that matches what actually crashed, so this
    // is fifteen. If someone later measures the base slot and finds sixteen is
    // safe, this is the line to change and the note to correct.
    //
    // ⚠⚠ NOT A SETTING, DELIBERATELY. Other mods can push past this and the
    // result is an unstable game rather than a working one, so an INI key here
    // would be an option whose only use is to break the save. User's call
    // 2026-08-16: do not exceed it.
    inline constexpr std::size_t kMaxWornLayers = 15;

    // Whether a proposed state is safe to send to the engine.
    //
    // ⚠ THE WHOLE LIST, NOT THE EDIT. A character can arrive over the ceiling
    // from RaceMenu, a preset or another mod, and the retint would crash on the
    // first write we made even if that write turned something OFF. So the
    // question is always "how many would be worn AFTER this", never "did we add
    // one".
    [[nodiscard]] inline bool WithinCeiling(const Snapshot& a_state) {
        return UsedIn(a_state) <= kMaxWornLayers;
    }

    // How many more may be switched on. Zero when full or over.
    [[nodiscard]] inline std::size_t RemainingLayers(const Snapshot& a_state) {
        const auto used = UsedIn(a_state);
        return used >= kMaxWornLayers ? 0 : kMaxWornLayers - used;
    }

    // Whether switching this one ON is allowed. Turning one OFF always is, and
    // that matters: a character already over the ceiling has to be able to get
    // back under it.
    [[nodiscard]] inline bool MayWear(const Snapshot& a_state, std::size_t a_index) {
        if (a_index < a_state.size() && Occupied(a_state[a_index])) {
            return true;  // already on, so this changes nothing about the count
        }
        return UsedIn(a_state) < kMaxWornLayers;
    }

}  // namespace OS::MakeupPlan
