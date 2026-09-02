#pragma once

// The preview grid's decisions, apart from its drawing.
//
// ⚠ PURE AND TESTED BECAUSE NO TEST COMPILES EditorUI.cpp. The file every
// presentation defect lands in is field-tested only, so every decision that
// can live as arithmetic over plain values lives here instead: layout, the
// reveal ramp, cache keys, and which entry a cache prunes, builds or evicts
// next. The DyeKey.h precedent, for the same reason.

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <memory>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "PreviewFilter.h"  // the slot-bit constants the pose rule reads

namespace OS::PreviewGrid {

    // The width a grid must lay itself out against, which is NOT the width it
    // was handed.
    //
    // ⚠⚠ A GRID WHOSE CARDS GROW WITH THE PANE CANNOT ALSO READ THE PANE'S
    // CURRENT WIDTH, because that is a loop with a frame in it. FittedSide
    // grows every card to eat the row's remainder, so the card size, and with
    // it the ROW HEIGHT, is a function of the available width. The available
    // width shrinks by exactly one scrollbar when a scrollbar appears. So:
    // content grows past the view, a scrollbar appears, the cards get narrower,
    // the rows get shorter, the content now fits, the scrollbar goes away, the
    // cards get wider again. One flip per frame, forever, and the user sees the
    // list strobing between two layouts (field 2026-08-21, 18 styles filtered
    // to skyrim.esm: 4 columns, 5 rows, a 12px bar costs 3px per card and 15px
    // of height, and the pane happened to sit inside that band).
    //
    // The fix is to lay out against the width the pane has WHEN A SCROLLBAR IS
    // PRESENT, in both states. That number does not move when the bar toggles,
    // so the content height stops moving, so the bar stops toggling. The cost
    // is one scrollbar of width given up in panes that never scroll, which is
    // invisible next to a strobing list.
    //
    // ⚠ MEASURED FROM THE WINDOW'S RIGHT EDGE, NOT FROM THE CONTENT REGION,
    // because the content region's right edge is the thing that moves. The
    // window's does not. a_toWindowEdge is (window right - cursor x) and
    // a_rightPad is the window padding that sits inside it.
    //
    // ⚠ DEGRADES TO THE WIDTH IT WAS HANDED rather than to something narrow. A
    // grid nested in a column or a table cell can produce a nonsense answer
    // here, and a wrong LAYOUT is worse than a flicker; the guard keeps this
    // from ever returning a bigger or negative width.
    [[nodiscard]] inline float StableAvail(float a_avail, float a_toWindowEdge,
                                           float a_rightPad, float a_scrollbar) {
        const float reserved = a_toWindowEdge - a_rightPad - a_scrollbar;
        if (reserved <= 0.0f || reserved > a_avail) {
            return a_avail;
        }
        return reserved;
    }

    // Columns from available width, as many as fit: the card-size slider is
    // the governor now, so the spec's cap of three would only make the
    // slider look broken (shrinking the cards bought no columns, field
    // 2026-08-09); the click-target concern it served lives in
    // Settings::kCardScaleMin. Never returns zero, so a degenerate width
    // still lays out.
    //
    // (The paging family that used to live beside this died with the pager:
    // the grid scrolls now, and the clipper in EditorUI does the windowing.)
    [[nodiscard]] inline int ColumnsFor(float a_availW, float a_side, float a_spacing) {
        if (a_availW <= a_side) {
            return 1;
        }
        const int cols = static_cast<int>((a_availW + a_spacing) / (a_side + a_spacing));
        return cols < 1 ? 1 : cols;
    }

    // The card size that actually fills a row, given the size the setting
    // asked for.
    //
    // ⚠⚠ THE SETTING IS A TARGET, NOT THE ANSWER. ColumnsFor floors, so
    // whatever does not divide evenly into the pane was simply left empty:
    // the Bodies library is a sidebar about 400 wide and a card about 215, so
    // it fit ONE column and wasted nearly half the width (field 2026-08-10).
    // Growing each card to eat the remainder fills the row exactly at any
    // setting, and the setting keeps its real meaning, which is DENSITY: a
    // smaller target fits more columns rather than leaving a bigger hole.
    //
    // ⚠ IT CAN COME BACK SMALLER THAN ASKED, on purpose. A pane narrower than
    // one card shrinks the card instead of overflowing it, because a card
    // wider than its column draws over the scrollbar and off the edge. The
    // floor stops that becoming unreadable.
    [[nodiscard]] inline float FittedSide(float a_availW, float a_side, float a_spacing,
                                          float a_minSide) {
        const int   cols = ColumnsFor(a_availW, a_side, a_spacing);
        const float used = a_availW - a_spacing * static_cast<float>(cols - 1);
        const float side = used / static_cast<float>(cols);
        return side < a_minSide ? a_minSide : side;
    }

    // How big to draw a card's NAME, relative to body text.
    //
    // The card scaled and the font did not, so a small card showed three or
    // four letters of every name and the three-dot trim did the rest (field
    // 2026-08-11). The trim was never the bug - it was already there and
    // already measured against the card.
    //
    // ⚠ ASK THE CARD'S FITTED SIDE, NEVER FontSize() * previewCardScale. The
    // setting is a TARGET and FittedSide above is the answer: it grows each
    // card to eat the row's remainder and can also come back SMALLER than
    // asked in a narrow pane. A scale derived from the setting would therefore
    // be wrong in every pane whose width does not divide evenly, which is the
    // general case and the whole reason FittedSide exists.
    //
    // Dividing the fitted side by body text gives the card's real size in
    // body-text units, so the ratio holds at any UI scale and any resolution
    // scale without reading either: both inputs already move together.
    //
    // The reference is the size at which a name is drawn FULL size; smaller
    // cards scale down from there and larger ones are capped, because a name
    // bigger than body text is a different kind of wrong. The floor exists
    // because FittedSide can hand back a 2-unit card, and a name at a third
    // of body text is unreadable rather than merely tight.
    //
    // ⚠ THE TWO CONSTANTS ARE SEPARATE ON PURPOSE. If the field says small
    // cards are still too tight the knob is the floor alone; if it says names
    // shrink too eagerly the knob is the reference alone. Folding them into
    // one number would make every field round move both.
    // ⚠ BOTH MOVED ON 2026-08-26, 6 -> 7 AND 0.60 -> 0.55, AND THE COMMENT
    // ABOVE SAYS TO MOVE ONE. This is the exception it did not foresee, and the
    // test at test_previewgrid.cpp is what found it. The field asked for "card
    // text slightly smaller so more of it can fit", which is the reference
    // knob: the skin grid's 5.6-unit cards drew at 5.6/6 = 0.93 and now draw at
    // 5.6/7 = 0.80, so about a sixth more characters survive before the three
    // dots.
    //
    // ⚠⚠ BUT RAISING THE REFERENCE ALONE PUSHES THE DEFAULT CARD UNDER THE
    // FLOOR. A fresh install draws 4-unit cards, and 4/7 = 0.571 is below 0.60,
    // so every ordinary card would have pinned to the floor and the floor would
    // have stopped being a safety net for tiny cards and become the normal
    // value: the setting would visibly stop responding across most of its
    // range. Dropping the floor to 0.55 keeps 4 units proportional and keeps
    // the floor doing the job it was added for. The two are separate knobs and
    // this is still one change, because the second only moved to preserve the
    // first's meaning.
    inline constexpr float kCardNameRefUnits = 7.0f;
    inline constexpr float kCardNameMinScale = 0.55f;

    [[nodiscard]] inline float NameScaleFor(float a_side, float a_bodyPx) {
        if (a_bodyPx <= 0.0f || a_side <= 0.0f) {
            return 1.0f;  // no usable measurement: draw exactly as before
        }
        const float want = (a_side / a_bodyPx) / kCardNameRefUnits;
        if (want < kCardNameMinScale) {
            return kCardNameMinScale;
        }
        return want > 1.0f ? 1.0f : want;
    }

    // FNV-1a-64, the disk cache's filename hash. Pinned by test against the
    // reference vectors so a refactor cannot silently re-key every cached
    // thumbnail on every install.
    [[nodiscard]] inline std::uint64_t Fnv1a64(std::string_view a_s) {
        std::uint64_t h = 0xCBF29CE484222325ull;
        for (const unsigned char c : a_s) {
            h ^= c;
            h *= 0x100000001B3ull;
        }
        return h;
    }

    // "AB/AB12CD34EF56789A.png": sharded two-character subdirectory so the
    // cache never becomes one flat directory, extension .png because the disk
    // format IS the display format (Task 0: FUCK loads files, nothing else).
    [[nodiscard]] inline std::string ShardPathFor(std::uint64_t a_hash) {
        char buf[24]{};
        std::snprintf(buf, sizeof buf, "%016llX",
                      static_cast<unsigned long long>(a_hash));
        std::string out;
        out.reserve(23);
        out.append(buf, 2);
        out.push_back('/');
        out.append(buf);
        out.append(".png");
        return out;
    }

    // Lowercased with backslashes normalised: the one fold every on-disk
    // name comparison in the preview subsystem uses (keys here, the scene
    // filters in PreviewFilter.h through their callers).
    [[nodiscard]] inline std::string FoldPath(std::string_view a_in) {
        std::string out;
        out.reserve(a_in.size());
        for (const char c : a_in) {
            out.push_back(c == '\\' ? '/'
                                    : static_cast<char>(std::tolower(
                                          static_cast<unsigned char>(c))));
        }
        return out;
    }

    // What identifies a scene on disk: stable across sessions and load order
    // changes, which is why it is the plugin name plus LOCAL FormID and
    // never a runtime FormID. Phase 2: one to N model paths (an armour scene
    // is every addon that passed race and sex), plus the slot mask as RENDER
    // CONTEXT that stays OUT of the disk key.
    // What a scene IS, as render context (never identity): the extractor's
    // scaffolding rules key on head-part-ness, the pose picker adds the
    // eyes' straight-on iris chip, and the brows buy a lifted backdrop.
    // kSkin (OS-212): a skin pack's card. The character's own body, hands,
    // feet and head as subject roots, exactly a body card's composition,
    // drawn TEXTURED rather than grey and with the pack's files swapped in by
    // name (TextureSwapEntry::whenTex0Name), so the card shows what the pack
    // looks like on this character rather than a grey figure that looks the
    // same on every card.
    enum class SceneKind : std::uint8_t {
        kGear,
        kHair,
        kEyes,
        kBrows,
        kFacialHair,
        kBody,
        kSkin
    };

    // ⚠⚠ WRITTEN OUT RATHER THAN "NOT kGear", WHICH IS WHAT IT USED TO BE AND
    // WHAT A NEW KIND SILENTLY BREAKS. kBody is not a head part, and the old
    // spelling would have turned the head-part scaffolding filters on for a
    // whole body: the no-material rule and the non-standard-blend rule are
    // written about eye lenses and hair shells, and a body has neither.
    // Nothing in the compiler catches this, which is why it is a list.
    [[nodiscard]] inline bool IsHeadPartKind(SceneKind a_kind) {
        switch (a_kind) {
            case SceneKind::kHair:
            case SceneKind::kEyes:
            case SceneKind::kBrows:
            case SceneKind::kFacialHair:
                return true;
            case SceneKind::kGear:
            case SceneKind::kBody:
            case SceneKind::kSkin:
                return false;
        }
        return false;
    }

    // ⚠⚠ WHOSE SUBJECT IS A BODY, so the garment filters must not be pointed
    // at it. Those rules exist to strip a body OUT of a garment scene, and on
    // a Bodies card the body IS the scene: the face-flag rule (a UBE body is
    // kFaceGenRGBTint), the embedded-body leaf list (the 3BA reference NIF's
    // shapes are named exactly 3BA, 3BA_Vagina and 3BA_Anus) and the hands and
    // feet slot gates (a body's own hands leaf is "hands" and its slot mask is
    // zero) each delete the whole subject.
    //
    // This is the mannequin's own exemption asked for by a second caller, and
    // it is the same failure: every armour card came back as the item alone
    // with a perfectly correct key (field 2026-08-10, tag m1). A body card
    // would have come back failed(geometry) the same way.
    //
    // A LIST, not "kind == kBody", for IsHeadPartKind's reason one screen up.
    [[nodiscard]] inline bool IsBodySubjectKind(SceneKind a_kind) {
        switch (a_kind) {
            case SceneKind::kBody:
            case SceneKind::kSkin:
                return true;
            case SceneKind::kGear:
            case SceneKind::kHair:
            case SceneKind::kEyes:
            case SceneKind::kBrows:
            case SceneKind::kFacialHair:
                return false;
        }
        return false;
    }

    // ⚠ WHICH BODY SUBJECTS ARE DRAWN GREY. A body card is a grey figure on
    // purpose (user's call 2026-08-10: every other card in the app is one, and
    // the Bodies page is about SHAPE). A skin card is about the SKIN, so it
    // keeps its own diffuse and takes the pack's on top. Split out from
    // IsBodySubjectKind because the two questions ("exempt from the garment
    // filters, framed as a whole figure" and "drawn without its texture") were
    // one flag until a kind arrived that answers them differently.
    [[nodiscard]] inline bool DrawsGreyBody(SceneKind a_kind) {
        switch (a_kind) {
            case SceneKind::kBody:
                return true;
            case SceneKind::kSkin:
            case SceneKind::kGear:
            case SceneKind::kHair:
            case SceneKind::kEyes:
            case SceneKind::kBrows:
            case SceneKind::kFacialHair:
                return false;
        }
        return false;
    }

    // Which scenes get the skeleton alignment (OS-204). A hair NIF is authored
    // ONTO a head, so its authoring skeleton is a claim about where that head
    // is and can be compared with ours; the same is true of a beard.
    //
    // ⚠⚠ EYES JOINED THEM 2026-08-15, AND THE LINE THAT KEPT THEM OUT WAS
    // "their framing is settled and field-approved". That was true of the
    // framing and said nothing about the placement, and the two were not the
    // same question. Measured off preview.boxes: of forty eye cards, eight put
    // their mesh at z 2.57..4.04 while the other thirty-two put it at
    // 122.92..124.38, and the eye window covers 121.5..123.8. Those eight are
    // the separate faceparts/eyes*left.nif and eyes*right.nif files, they carry
    // no skeleton, and they are authored in HEAD space - so they rendered at
    // the mannequin's ankles and their cards came out blank ("not all eyes are
    // showing", three field rounds).
    //
    // ⚠ EXCLUDING THEM COST TWO ROUNDS RATHER THAN ONE, because the fix for
    // those eight went into MeshExtractor first and could not fire: the anchor
    // is passed only for kinds named here, so the new branch sat behind a null
    // pointer and its counter read zero. A gate and the code it gates are one
    // change.
    //
    // Brows stay out because they carry no mannequin to align against, and gear
    // because it is skinned to the body rather than parented to the skull.
    [[nodiscard]] inline bool IsAlignedKind(SceneKind a_kind) {
        return a_kind == SceneKind::kHair || a_kind == SceneKind::kFacialHair ||
               a_kind == SceneKind::kEyes;
    }

    // ⚠⚠ THE MANNEQUIN'S OWN HEAD IS A SUBJECT OF THE ANCHOR, NOT ONLY ITS
    // SOURCE, AND ON EVERY KIND. IsAlignedKind above answers "does the ITEM
    // move onto the head"; these two answer "which roots touch the anchor at
    // all", which is a different question and was conflated with it until
    // 2026-08-20.
    //
    // The fault that separated them: vanilla's MaleHeadArgonian.nif skins its
    // shape to NPC Spine2 and gives it NO transform, while its three siblings
    // (female Argonian, both Khajiit) carry trans z=120.344 on the shape.
    // Measured offline against the file and confirmed to two decimals by the
    // field log: the male Argonian head rendered at z -11.01..11.26 while the
    // others sat at ~109..134. On a live actor the engine skins it to the real
    // skeleton and nothing is wrong; only a standalone loader like a card sees
    // the origin. So the head lay on the floor by the mannequin's feet, and was
    // absent from every window framed on the head.
    //
    // ⚠ AND IT IS NOT A HAIR-ONLY FAULT, WHICH IS WHY THIS IS NOT IsAlignedKind.
    // The figure carries its head on an armour card too, so gating this on the
    // aligned kinds would have fixed the hair cards and left the head lying at
    // the feet of every helmet, cuirass and boot card. Same trap the comment
    // above IsAlignedKind already records: a gate and the code it gates are one
    // change.
    //
    // ⚠ THE HEAD ROOT PUBLISHES TO ITSELF and that is deliberate. Both files
    // carry an 'NPC Head [Head]' NiNode at 120.344 even when the shape does
    // not, so FindHeadAnchor reads a valid anchor off the head root itself
    // before the align step in the same Extract call consumes it. A head whose
    // geometry already sits at the anchor measures nearer to it than to the
    // origin and is left alone, so the three correct siblings never move.
    [[nodiscard]] inline bool RootConsultsHeadAnchor(bool a_mannequin,
                                                     bool a_mannequinHead,
                                                     SceneKind a_kind) {
        if (a_mannequinHead) {
            return true;  // the figure's own head, on every card
        }
        if (a_mannequin) {
            return false;  // body, hands, feet and tail are skinned; never moved
        }
        return IsAlignedKind(a_kind);  // the item, unchanged
    }

    [[nodiscard]] inline bool RootPublishesHeadAnchor(bool a_mannequin,
                                                      bool a_mannequinHead,
                                                      SceneKind a_kind) {
        if (!a_mannequin) {
            return false;  // only the figure reports where its head bone is
        }
        return a_mannequinHead || IsAlignedKind(a_kind);
    }

    // One texture swap, as plain data (a queued request must survive a save
    // load, so no RE:: pointer ever rides a scene). geomIndex is the
    // ENGINE's running geometry counter: an unfiltered depth-first walk in
    // child order, every AsGeometry() hit counting, cull flags ignored
    // (docs/superpowers/research/re_verify/ae_swap_walker.c). The records'
    // name3D is authoring decoration the engine never reads. kAllGeometry
    // is the head-part TNAM case: the set applies to every mesh of the
    // model it rides.
    struct TextureSwapEntry {
        static constexpr std::uint32_t kAllGeometry = 0xFFFFFFFFu;
        std::uint32_t              geomIndex{ 0 };
        std::array<std::string, 8> texPaths{};  // record paths, unrooted, TX00..TX07
        // ⚠ THE SKIN PACK RULE (OS-212), and it is a third way to pick a
        // geometry rather than a refinement of the index. When non-empty the
        // entry applies to every geometry whose OWN TX00 has this file name
        // (last segment, case-folded, SkinPlan::FileName), and geomIndex is
        // not consulted at all. It is the live skin changer's rule verbatim: a
        // pack is a folder of files matched by name against what a shape
        // reads, so a card that matched any other way could disagree with the
        // character it is a picture of. Empty everywhere else, and an empty
        // name adds nothing to the key.
        std::string                whenTex0Name;
    };

    struct SceneIdentity {
        std::string              plugin;
        std::uint32_t            localFormId{ 0 };
        std::string              editorId;
        std::vector<std::string> modelPaths;
        // Parallel to modelPaths when present; EMPTY when the scene carries
        // no swap anywhere, which is what keeps every swap-less key
        // byte-identical (pinned). Capture only assigns it when at least
        // one path captured entries.
        std::vector<std::vector<TextureSwapEntry>> swaps;
        // Parallel to modelPaths when present; EMPTY when no preview scope
        // covers any path in the scene, which is what keeps every scope-less
        // key byte-identical (pinned, OS-191). An individual entry is empty
        // for a path no scope covers even when a sibling path has one.
        std::vector<std::string> scopeTags;
        std::uint32_t            slotMask{ 0 };  // context, never identity
        SceneKind                kind{ SceneKind::kGear };  // context, never identity
        // How many LEADING entries of modelPaths are mannequin rather than
        // item (OS-204). Context, never identity, and the distinction matters:
        // those paths are already in the key, one each, exactly like an
        // item's, so keying this count as well would say the same thing twice.
        // It exists so the build side can tell which roots resolve no diffuse
        // and which framing a scene has earned.
        std::size_t              mannequinPathCount{ 0 };
        // ⚠⚠ THE MANNEQUIN'S OWN VERSION, AND IT IS IDENTITY RATHER THAN
        // CONTEXT, unlike the count beside it. Empty means no mannequin, and
        // an empty tag adds NOTHING to the key, which is what keeps every
        // weapon, ammo, eye and brow card byte-identical.
        //
        // It exists because the mannequin's pixels can change without any
        // path changing: the extractor decides what to draw out of those NIFs,
        // and that decision has already been wrong once (the garment filters
        // deleted the whole body, field 2026-08-10, with the key perfectly
        // correct). Bumping this rebuilds exactly the cards that carry a
        // mannequin and leaves every other thumbnail alone, which is the
        // cheap half of the renderer-version bump this project keeps
        // preferring to avoid.
        std::string              mannequinTag;
        // Which mannequin path is the HEAD, or npos. Context like the count
        // beside it: the head is drawn but never measured, because it is the
        // one part a full-face helm can remove and a reference box that
        // changes size slides every standard view down the figure.
        std::size_t              mannequinHeadIndex{ static_cast<std::size_t>(-1) };
        // Which mannequin path is the FEET, or npos. Context like the head
        // index, and it exists for the same reason pointing the other way.
        //
        // ⚠⚠ THE FEET ARE THE OTHER PART THAT CAN GO, and forgetting that
        // reached the field: any footwear drops them, so on a boots card the
        // box floor would jump from the ankle to the hips and every fraction
        // of it would slide up the figure. Three knee-high boots came back cut
        // off at the bottom (field 2026-08-10) and they were exactly the three
        // records whose slot mask made the old rule fire.
        //
        // So the feet are DRAWN but never MEASURED, the mirror of the head,
        // and the reference box is the body's alone. That way it is the same
        // box whether the feet are composed or not, which is the invariant the
        // whole framing rests on.
        std::size_t              mannequinFeetIndex{ static_cast<std::size_t>(-1) };
        // What a body card needs to rebuild its SHAPE, since the card is built
        // long after it is requested and the slider values are not otherwise
        // reachable from an identity.
        //
        // ⚠ CONTEXT, NEVER IDENTITY, like the mannequin's path count. The
        // shape already reaches the key through editorId, which carries a hash
        // of these very values (BodyMorphData::CardIdentity); keying them here
        // as well would say the same thing twice and make the key enormous.
        //
        // ⚠ SHARED AND CONST because a SceneIdentity is copied into the cache
        // and queued, and a preset's slider list is not small.
        struct BodyShape {
            std::string                                projectFile;
            std::string                                setName;
            // ⚠⚠ THE CARD IS BUILT LONG AFTER IT IS ASKED FOR, so the shape
            // has to say WHICH SOURCE answered, not just what to ask. A
            // preset with no BodySlide project takes its morphs from the built
            // body, and an empty projectFile is the whole of that signal:
            // without this the build re-asked for a project it never had,
            // BodyMorphSource logged "'' would not open as XML", the field
            // came back empty, and every fallback card drew its base body
            // with no morphs at all (field 2026-08-10, "they appear to be
            // empty/zeroed sliders").
            std::string                                runtimeMesh;
            // ⚠ NO morphShape HERE. It was written by BodyCardScene and read
            // by nobody, because the build re-asks BodyMorphSource for the
            // source anyway and takes the shapes from it. Which geometry a
            // field belongs to is now a property of the SOURCE, one per shape,
            // and duplicating a single name here could only go stale.
            std::vector<std::pair<std::string, float>> sliders;
            // The character's weight as 0..1. The sliders above are ALREADY
            // walked to it; this is here for the ones the preset never names,
            // whose small->big default only the .osp knows.
            float                                      weight01{ 1.0f };
        };
        std::shared_ptr<const BodyShape> bodyShape;
        // How many LEADING modelPaths the body morph applies to. One on a body
        // card (its reference mesh), zero everywhere else.
        //
        // ⚠⚠ THE HEAD, HANDS AND FEET RIDE THE SAME SCENE AND MUST NOT TAKE
        // THE FIELD. A .osd is indexed by the body shape's vertex array; a
        // hand NIF has its own, so the same displacement lands on unrelated
        // vertices and shreds it. The partition map already taught this once.
        //
        // ⚠ CONTEXT, NEVER IDENTITY, like mannequinPathCount. The paths it
        // counts are in the key already; counting them again would say the
        // same thing twice.
        std::size_t morphPathCount{ 0 };
        // How many LEADING modelPaths are rooted at Data rather than meshes\.
        //
        // ⚠⚠ THE TWO BODY SOURCES DISAGREE ABOUT THIS AND NOTHING ELSE TELLS
        // THEM APART. A BodySlide reference mesh lives under
        // CalienteTools\...\ShapeData and needs LoadDataRelative; the built
        // body behind a runtime .tri is an ordinary form model path under
        // meshes\ and needs the normal loader. Sending either through the
        // other's entry point returns null and drops the subject in silence,
        // which is the failure the head, hands and feet already hit once.
        //
        // ⚠ CONTEXT, NEVER IDENTITY. The paths themselves are in the key.
        std::size_t dataRootedPathCount{ 0 };
    };

    // Which scenes stand upright rather than on the weapons' diagonal: worn
    // gear (any slot bit, phase 2) and head parts (no bits, phase 3). Head
    // parts sit in head bind space, +Z up and +Y facing, which is the
    // orientation the upright framing was built for; the AABB centring
    // absorbs their height offset. A shield ALONE stays diagonal: it lies
    // flat along the forearm in bind pose, so upright framed it edge-on
    // (field 2026-08-09); a shield sharing a scene with body slots follows
    // the body.
    [[nodiscard]] inline bool StandsUpright(std::uint32_t a_slotMask, bool a_headPart) {
        return a_headPart || (a_slotMask & ~PreviewFilter::kShieldSlotBit) != 0;
    }

    // ---- the mannequin (OS-204) ------------------------------------------
    //
    // What a scene wants under its item. kFigure is the body the skin ARMO
    // resolves to plus the race's head; kHead is the head alone.
    enum class MannequinKind : std::uint8_t { kNone, kFigure };

    // ⚠ WEAPONS AND AMMO MUST ANSWER kNone AND THAT IS LOAD BEARING, not
    // taste. The mannequin reaches the disk key through the model paths it
    // adds, so a scene that adds none keys byte-identically to today and keeps
    // the thumbnail phase 1 built for it. A sword on a figure was also
    // rejected on the picture (spec), but the key is why this is pinned.
    //
    // A LONE SHIELD answers kNone for the same two reasons: it keeps the
    // weapons' diagonal (kShield), and a disc photographed edge-on against a
    // standing body is neither of the two pictures. A shield sharing a scene
    // with body slots follows the body, which is what StandsUpright already
    // decides for the pose.
    //
    // Eyes and brows answer kNone because their cards are close-ups that were
    // field-approved as they are: kEyes frames ONE eyeball and a head behind
    // it would fill the card with cheek. Hair and facial hair take a head,
    // which is the whole point of the feature for them.
    [[nodiscard]] inline MannequinKind MannequinFor(SceneKind    a_kind,
                                                    std::uint32_t a_slotMask) {
        switch (a_kind) {
            case SceneKind::kHair:
            case SceneKind::kFacialHair:
                // ⚠ THE SAME FIGURE GEAR GETS, NOT A HEAD ON ITS OWN. A head
                // alone framed itself, so a hair card and a helmet card came
                // back at two different scales and read as two features
                // (field 2026-08-10). Both take the whole figure and both
                // crop to head and shoulders, so they are the same picture
                // with a different thing on it. Long hair also needs the
                // shoulders it falls onto to have anywhere to land.
                return MannequinKind::kFigure;
            case SceneKind::kGear:
                return StandsUpright(a_slotMask, false) ? MannequinKind::kFigure
                                                        : MannequinKind::kNone;
            case SceneKind::kEyes:
                // ⚠ AN EYE IN A FACE, asked for 2026-08-10. The straight-on
                // iris chip was field-approved when there was no head to put
                // it in; a disembodied eyeball on a card was always the least
                // bad of two, not the picture anyone wanted.
                return MannequinKind::kFigure;
            case SceneKind::kBody:
            case SceneKind::kSkin:
                // ⚠⚠ THE BODY IS THE SUBJECT, so composing the mannequin onto
                // it would put this load order's body inside the body being
                // photographed, two near-identical meshes z-fighting the whole
                // card. The Bodies page shows what a slider set BUILDS, and
                // one of the things it can build is the very mesh the
                // mannequin is made of. A skin card IS the mannequin's parts,
                // as subject roots, so the same answer for the same reason.
                return MannequinKind::kNone;
            default:  // kBrows, which keep their field-approved close-up
                return MannequinKind::kNone;
        }
    }

    // Folded fields '|'-joined, the paths ';'-joined within their field, and
    // a fixed "none" context: no scene contains the body, so race and sex
    // must NOT enter the key or changing either would orphan every
    // thumbnail. They reach the key through the resolved paths instead (a
    // female mesh is a different path). ⚠ A ONE-path key is byte-identical
    // to phase 1's, which is what keeps every weapon thumbnail valid across
    // this change; the test suite pins it.
    // A path that carries texture swaps grows "@idx:tx00,idx:tx00" (OS-192):
    // '*' for kAllGeometry, entries in authored order, ',' between them
    // because ';' already separates paths in this field. Only TX00 enters
    // the key: the card samples only the diffuse, so a swap differing in
    // normals alone renders the same pixels and should share them. A path
    // with no swaps contributes exactly its old bytes; the swap-less pin
    // lives on that.
    // A TXST texture path as the ENGINE resolves it: relative to Data\Textures,
    // with a leading "textures/" removed.
    //
    // ⚠⚠ AUTHORS SHIP BOTH FORMS AND THE ENGINE TAKES BOTH. A TESTextureSet
    // stores paths rooted at Data\Textures, so "actors/character/eyes/x.dds"
    // is the usual form, and a record that spells "textures/actors/..." means
    // the same file. Fitting Room's swap loader prepends "Data\Textures\"
    // unconditionally, so the second form asked the engine for
    // Data\Textures\textures\actors\... , which does not exist.
    //
    // ⚠⚠ AND THE FAILURE IS SILENT, which is why it reached the field: a
    // texture the engine cannot find is not an error, it is substituted with
    // the famous purple placeholder and still hands back a live
    // NiSourceTexture. So the swap "applied", the diffuse "resolved",
    // preview.swap-no-diffuse stayed at zero, and 116 eye cards rendered the
    // same lavender ball with a completely clean log (field 2026-08-09).
    //
    // ⚠ THE KEY USES THIS TOO, and that is what makes the fix cheap. Two
    // records naming one file in the two spellings ARE one texture and should
    // share one card, so normalising here re-keys exactly the cards that were
    // wrong and leaves every other key byte-identical: no renderer bump, no
    // cache wipe. Pinned both ways by test.
    [[nodiscard]] inline std::string NormalizeTexturePath(std::string_view a_folded) {
        constexpr std::string_view kPrefix{ "textures/" };
        if (a_folded.size() > kPrefix.size() &&
            a_folded.compare(0, kPrefix.size(), kPrefix) == 0) {
            a_folded.remove_prefix(kPrefix.size());
        }
        return std::string{ a_folded };
    }

    [[nodiscard]] inline std::string DiskKeyFor(const SceneIdentity& a_id) {
        char formId[12]{};
        std::snprintf(formId, sizeof formId, "%08x", a_id.localFormId);
        std::string paths;
        for (std::size_t i = 0; i < a_id.modelPaths.size(); ++i) {
            if (!paths.empty()) {
                paths.push_back(';');
            }
            paths += FoldPath(a_id.modelPaths[i]);
            // ⚠ THE SCOPE TAG GOES ON AFTER THE SWAPS AND BEFORE THE NEXT
            // PATH, and the order of the two suffixes is fixed rather than
            // incidental: a path carrying both must fold the same way every
            // time or its own thumbnail orphans itself between runs. '#'
            // because '@' is the swaps' and ';' already separates paths.
            //
            // A path no scope covers contributes exactly its old bytes, which
            // is the pin: the alternative to this whole mechanism was a
            // renderer bump, and that rebuilds the entire cache rather than
            // the handful of cards a fixup actually changes.
            const auto tag = i < a_id.scopeTags.size() ? std::string_view{ a_id.scopeTags[i] }
                                                       : std::string_view{};
            const bool hasSwaps = i < a_id.swaps.size() && !a_id.swaps[i].empty();
            if (!hasSwaps) {
                if (!tag.empty()) {
                    paths.push_back('#');
                    paths += tag;
                }
                continue;
            }
            paths.push_back('@');
            for (std::size_t e = 0; e < a_id.swaps[i].size(); ++e) {
                if (e != 0) {
                    paths.push_back(',');
                }
                const auto& entry = a_id.swaps[i][e];
                if (!entry.whenTex0Name.empty()) {
                    // '=' and the name: a spelling no index or '*' can
                    // produce, so a by-name entry never keys like an indexed
                    // one and every existing key is untouched.
                    paths.push_back('=');
                    paths += FoldPath(entry.whenTex0Name);
                } else if (entry.geomIndex == TextureSwapEntry::kAllGeometry) {
                    paths.push_back('*');
                } else {
                    paths += std::to_string(entry.geomIndex);
                }
                paths.push_back(':');
                // Normalised, so the two spellings of one texture fold to one
                // key and share one card.
                paths += NormalizeTexturePath(FoldPath(entry.texPaths[0]));
            }
            if (!tag.empty()) {
                paths.push_back('#');
                paths += tag;
            }
        }
        auto key = FoldPath(a_id.plugin) + "|" + formId + "|" +
                   FoldPath(a_id.editorId) + "|" + paths + "|none";
        // Appended LAST and only when present, so a scene with no mannequin
        // produces exactly the bytes it produced before this feature existed.
        if (!a_id.mannequinTag.empty()) {
            key += "|";
            key += a_id.mannequinTag;
        }
        return key;
    }

    // Which swap a geometry gets, by its COLLECTED index (the engine's
    // counter, see TextureSwapEntry): the first exact index match, else the
    // first kAllGeometry entry, else none. First match wins among
    // duplicates, the hash-bucket behaviour of the engine's own table.
    //
    // a_tex0Name is the geometry's OWN TX00 file name (SkinPlan::FileName of
    // what its texture set reads), or empty when the caller has none. A
    // by-name entry (whenTex0Name set) is matched by that alone, after the
    // exact index and before kAllGeometry, and never by index; an indexed
    // entry never matches by name. Two rules, no overlap.
    [[nodiscard]] inline const TextureSwapEntry* SwapFor(
        const std::vector<TextureSwapEntry>& a_entries, std::uint32_t a_index,
        std::string_view a_tex0Name = {}) {
        for (const auto& e : a_entries) {
            if (e.whenTex0Name.empty() && e.geomIndex == a_index) {
                return &e;
            }
        }
        if (!a_tex0Name.empty()) {
            for (const auto& e : a_entries) {
                if (!e.whenTex0Name.empty() && FoldPath(e.whenTex0Name) == FoldPath(a_tex0Name)) {
                    return &e;
                }
            }
        }
        for (const auto& e : a_entries) {
            if (e.whenTex0Name.empty() && e.geomIndex == TextureSwapEntry::kAllGeometry) {
                return &e;
            }
        }
        return nullptr;
    }

    // The cache-decision half: enough of an entry to decide pruning, build
    // order and eviction without holding the entry itself.
    enum class State : std::uint8_t { kQueued, kReady, kFailed };

    struct EntryMeta {
        std::uint64_t requestFrame{ 0 };  // the PRESENT counter, never a Draw counter
        std::uint64_t lastUsed{ 0 };
        State         state{ State::kQueued };
        // The trailing two stay behind state on purpose: the tests build
        // metas positionally as { requestFrame, lastUsed, state, ... }.
        std::uint64_t readyFrame{ 0 };  // the present that made it kReady; 0 = never
        std::uint32_t orderHint{ 0 };   // the card's on-screen index this present
    };

    // How much of a just-readied thumbnail to show, 0..1 over kRevealFrames
    // presents: the card fades in instead of popping, and a cold page filling
    // one card per frame reads as arrival rather than flicker. A zero ready
    // frame answers 1 so an entry that never recorded one still displays.
    inline constexpr std::uint64_t kRevealFrames = 9;

    [[nodiscard]] inline float RevealAlpha(std::uint64_t a_readyFrame,
                                           std::uint64_t a_now) {
        if (a_readyFrame == 0) {
            return 1.0f;
        }
        if (a_now < a_readyFrame) {
            return 0.0f;  // a clock behind the stamp reveals nothing yet
        }
        const auto steps = a_now - a_readyFrame + 1;
        return steps >= kRevealFrames
                   ? 1.0f
                   : static_cast<float>(steps) / static_cast<float>(kRevealFrames);
    }

    // Queued entries whose stamp is TWO or more presents old were requested
    // by a view nobody is looking at any more; pruning them is what makes
    // scrolling free. ⚠ Two, not one: the drain runs in the Present thunk,
    // which can sit on either side of FUCK's own present work depending on
    // hook order, so a stamp from the previous present is merely one
    // ordering away from current, never stale. Ready and Failed survive on
    // their own terms (LRU below).
    [[nodiscard]] inline std::vector<std::size_t> PruneSelection(
        const std::vector<EntryMeta>& a_entries, std::uint64_t a_currentFrame) {
        std::vector<std::size_t> out;
        for (std::size_t i = 0; i < a_entries.size(); ++i) {
            if (a_entries[i].state == State::kQueued &&
                a_entries[i].requestFrame + 1 < a_currentFrame) {
                out.push_back(i);
            }
        }
        return out;
    }

    // FIFO by request stamp, then by the on-screen order hint: the card the
    // user has waited longest for, and among equals the one highest on the
    // screen. Without the hint, equal stamps resolve in unordered_map order
    // and a cold fill visibly scatters (field 2026-08-09).
    [[nodiscard]] inline std::optional<std::size_t> NextToBuild(
        const std::vector<EntryMeta>& a_entries) {
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < a_entries.size(); ++i) {
            if (a_entries[i].state != State::kQueued) {
                continue;
            }
            if (!best) {
                best = i;
                continue;
            }
            const auto& b = a_entries[*best];
            const auto& c = a_entries[i];
            if (c.requestFrame < b.requestFrame ||
                (c.requestFrame == b.requestFrame && c.orderHint < b.orderHint)) {
                best = i;
            }
        }
        return best;
    }

    // Only Ready entries hold a display handle, so only they count against the
    // cap and only they can be evicted. Returns the oldest-used one, or
    // nothing while the cap is not exceeded.
    [[nodiscard]] inline std::optional<std::size_t> LruVictim(
        const std::vector<EntryMeta>& a_entries, std::size_t a_cap) {
        std::size_t                ready = 0;
        std::optional<std::size_t> oldest;
        for (std::size_t i = 0; i < a_entries.size(); ++i) {
            if (a_entries[i].state != State::kReady) {
                continue;
            }
            ++ready;
            if (!oldest || a_entries[i].lastUsed < a_entries[*oldest].lastUsed) {
                oldest = i;
            }
        }
        if (ready <= a_cap) {
            return std::nullopt;
        }
        return oldest;
    }

}  // namespace OS::PreviewGrid
