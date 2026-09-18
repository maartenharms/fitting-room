// Makeup's vocabulary and its packing. No SKSE and no engine: everything here
// is the half of the feature that can be held still.
//
// ⚠⚠ THE ONE THING WORTH PINNING IS WHAT "IN USE" MEANS. Makeup is not the
// overlays page with different art and this is where the difference bites: every
// tint layer carries a texture at all times, because the list is built from the
// race and a race gives every layer its default art. An unused layer is one at
// zero STRENGTH, not one with no texture. Reading it the overlays way would
// report every character as wearing all fifteen layers.
#include "MakeupPlan.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (false)

using namespace OS::MakeupPlan;
using OS::OverlayPlan::Rgb;

namespace {

    [[nodiscard]] bool NearlyEqual(float a_lhs, float a_rhs) {
        const float diff = a_lhs - a_rhs;
        return (diff < 0.0f ? -diff : diff) < 0.002f;
    }

    [[nodiscard]] LayerState MakeLayer(const char* a_texture, Rgb a_tint, float a_strength) {
        LayerState state{};
        state.texture    = a_texture;
        state.hasTexture = !state.texture.empty();
        state.tint       = a_tint;
        state.strength   = a_strength;
        return state;
    }

}  // namespace

int main() {
    {  // The type table lines up with the enum, which is the only thing keeping
        // a label from naming the wrong layer.
        CHECK(kTypes.size() == kTypeCount);
        CHECK(kTypeCount == 15);
        for (std::size_t i = 0; i < kTypes.size(); ++i) {
            CHECK(static_cast<std::uint32_t>(kTypes[i].type) == static_cast<std::uint32_t>(i));
            CHECK(kTypes[i].id != nullptr && kTypes[i].id[0] != '\0');
            CHECK(kTypes[i].labelKey != nullptr && kTypes[i].labelKey[0] == '$');
        }
        CHECK(std::string{ IdFor(static_cast<std::uint32_t>(Type::kLips)) } == "lips");
        CHECK(std::string{ IdFor(static_cast<std::uint32_t>(Type::kSkinTone)) } == "skintone");
    }

    {  // ⚠⚠ A TYPE PAST THE END IS SHOWN, NOT DROPPED. The measured presets run
        // to index 24 and beyond and the list is race dependent, so a race whose
        // list carries a type this build has never heard of must still get a
        // row. A layer a player can see on their own face and not on this page
        // is the worst outcome available.
        CHECK(KnownType(0));
        CHECK(KnownType(14));
        CHECK(!KnownType(15));
        CHECK(!KnownType(99));
        CHECK(std::string{ LabelKeyFor(99) } == "$FR_Mk_Other");
        CHECK(std::string{ IdFor(99) } == "other");
    }

    {  // The packed colour, against values measured out of the installed
        // presets. These are not invented numbers: index 0 is a skin tone at
        // 0xFFB79C91, an unused layer reads 0x00FFFFFF and a lip colour in use
        // reads 0x30A95452.
        CHECK(PackColour(Rgb{ 0xB7, 0x9C, 0x91 }, 1.0f) == 0xFFB79C91u);
        CHECK(PackColour(Rgb{ 0xFF, 0xFF, 0xFF }, 0.0f) == 0x00FFFFFFu);

        CHECK(TintFromPacked(0xFFB79C91u) == (Rgb{ 0xB7, 0x9C, 0x91 }));
        CHECK(TintFromPacked(0x30A95452u) == (Rgb{ 0xA9, 0x54, 0x52 }));
        CHECK(NearlyEqual(StrengthFromPacked(0xFFB79C91u), 1.0f));
        CHECK(NearlyEqual(StrengthFromPacked(0x00FFFFFFu), 0.0f));
        CHECK(NearlyEqual(StrengthFromPacked(0x30A95452u), 48.0f / 255.0f));

        // ⚠ THE TOP BYTE IS THE STRENGTH AND NEVER A RED CHANNEL. A shift that
        // kept it would hand back a colour whose red is really an alpha, which
        // is the mistake OverlayPlan::UnpackTint is commented against.
        CHECK(TintFromPacked(0x30A95452u).r == 0xA9);
    }

    {  // The round trip, which is what a slider does every frame it moves.
        for (const std::uint32_t packed : { 0xFFB79C91u, 0x00FFFFFFu, 0x30A95452u, 0x7F102030u }) {
            const auto tint     = TintFromPacked(packed);
            const auto strength = StrengthFromPacked(packed);
            CHECK(PackColour(tint, strength) == packed);
        }
    }

    {  // ---- the engine's word is ABGR and the preset's is ARGB -------------
        //
        // ⚠⚠ THIS IS THE 2026-08-16 FIELD BUG, PINNED. "Pure red appears blue,
        // pure blue appears red", green unaffected. RE::Color is bytes red,
        // green, blue, alpha at 0, 1, 2, 3, so the live field read as a
        // little-endian uint32 is 0xAABBGGRR, while a .jslot stores 0xAARRGGBB.
        // Writing one into the other swaps exactly red and blue.
        const Rgb red{ 255, 0, 0 };
        const Rgb blue{ 0, 0, 255 };
        const Rgb green{ 0, 255, 0 };

        // Pure red: the preset word carries it in the RED byte, the engine word
        // in the BLUE byte. Getting these two the same way round is the bug.
        CHECK(PackColour(red, 1.0f) == 0xFFFF0000u);
        CHECK(PackEngineColour(red, 1.0f) == 0xFF0000FFu);
        CHECK(PackColour(blue, 1.0f) == 0xFF0000FFu);
        CHECK(PackEngineColour(blue, 1.0f) == 0xFFFF0000u);

        // ⚠ GREEN IS THE CONTROL. It sits in the middle of both words, which is
        // why the field saw red and blue trade and green survive. A "fix" that
        // moved green would be a different bug.
        CHECK(PackColour(green, 1.0f) == PackEngineColour(green, 1.0f));
        CHECK(PackColour(green, 1.0f) == 0xFF00FF00u);

        // ⚠ THE ALPHA DOES NOT MOVE EITHER. Alpha is byte 3, which is the top
        // byte on little-endian, and the top byte of the preset word too.
        for (const float s : { 0.0f, 0.25f, 1.0f }) {
            CHECK((PackColour(red, s) >> 24) == (PackEngineColour(red, s) >> 24));
            CHECK(NearlyEqual(EngineStrengthFrom(PackEngineColour(red, s)), s));
        }

        // Both directions round trip on their own form.
        for (const Rgb c : { red, blue, green, Rgb{ 0xB7, 0x9C, 0x91 }, Rgb{ 1, 2, 3 } }) {
            CHECK(EngineTintFrom(PackEngineColour(c, 1.0f)) == c);
            CHECK(TintFromPacked(PackColour(c, 1.0f)) == c);
        }

        // And the two converters are each other's inverse.
        for (const std::uint32_t preset : { 0xFFB79C91u, 0x30A95452u, 0x00FFFFFFu, 0x7F102030u }) {
            CHECK(EngineToPreset(PresetToEngine(preset)) == preset);
        }
        // A skin tone measured off disk: warm under the preset word, and the
        // engine word for the same colour has the channels the other way about.
        CHECK(PresetToEngine(0xFFB79C91u) == 0xFF919CB7u);
    }

    {  // ⚠⚠ OCCUPIED IS THE STRENGTH AND NOTHING ELSE. Every one of these
        // carries real art and only the last two are worn.
        CHECK(!Occupied(MakeLayer("Actors\\Character\\Character Assets\\TintMasks\\SkinTone.dds",
                              Rgb{ 255, 255, 255 }, 0.0f)));
        CHECK(!Occupied(LayerState{}));
        CHECK(Occupied(MakeLayer("Actors\\Character\\Character Assets\\TintMasks\\SkinTone.dds",
                             Rgb{ 0xB7, 0x9C, 0x91 }, 1.0f)));
        CHECK(Occupied(MakeLayer("x.dds", Rgb{ 0, 0, 0 }, 0.05f)));

        // ⚠ A BLACK LAYER AT FULL STRENGTH IS WORN. Black is a colour here, not
        // an absence, and treating a zeroed tint as "off" is exactly the
        // conflation the overlays page spent three fixes on.
        CHECK(Occupied(MakeLayer("x.dds", Rgb{ 0, 0, 0 }, 1.0f)));
    }

    {  // The strength clamp, which the engine's float needs as much as the byte.
        CHECK(NearlyEqual(ClampStrength(-1.0f), 0.0f));
        CHECK(NearlyEqual(ClampStrength(0.5f), 0.5f));
        CHECK(NearlyEqual(ClampStrength(3.0f), 1.0f));
    }

    {  // The count a section header shows.
        Snapshot state;
        state.push_back(MakeLayer("a.dds", Rgb{ 1, 2, 3 }, 0.0f));
        state.push_back(MakeLayer("b.dds", Rgb{ 1, 2, 3 }, 0.5f));
        state.push_back(MakeLayer("c.dds", Rgb{ 1, 2, 3 }, 1.0f));
        CHECK(UsedIn(state) == 2);
        CHECK(UsedIn(Snapshot{}) == 0);
    }

    {  // ---- the revert -----------------------------------------------------
        //
        // ⚠⚠ THE DIFF IS WHAT KEEPS A REVERT FROM COSTING A REBAKE PER LAYER.
        // The retint after a batch builds a texture, so a revert that wrote all
        // fifteen slots would be as expensive as it is unnecessary.
        Snapshot before;
        before.push_back(MakeLayer("a.dds", Rgb{ 1, 2, 3 }, 0.5f));
        before.push_back(MakeLayer("b.dds", Rgb{ 4, 5, 6 }, 0.0f));
        before.push_back(MakeLayer("c.dds", Rgb{ 7, 8, 9 }, 1.0f));

        Snapshot after = before;
        CHECK(Differences(before, after).empty());

        after[1].strength = 0.75f;
        const auto one    = Differences(before, after);
        CHECK(one.size() == 1);
        CHECK(one[0] == 1);

        // A colour change with the strength unmoved still counts.
        after       = before;
        after[2].tint = Rgb{ 9, 9, 9 };
        CHECK(Differences(before, after).size() == 1);

        // So does a texture change.
        after         = before;
        after[0].texture = "z.dds";
        CHECK(Differences(before, after).size() == 1);

        // ⚠ A SHORTER OR LONGER LIST IS A DIFFERENCE ON EVERY SLOT PAST THE
        // OVERLAP, not a crash and not a silent truncation. A target switch or a
        // race change can land one here, and reading past the end of either side
        // is the failure this shape exists to make impossible.
        Snapshot shorter{ before.begin(), before.begin() + 1 };
        const auto grown = Differences(shorter, before);
        CHECK(grown.size() == 2);
        CHECK(grown[0] == 1);
        CHECK(grown[1] == 2);
        CHECK(Differences(before, shorter).size() == 2);
        CHECK(Differences(Snapshot{}, Snapshot{}).empty());
    }

    {  // ---- the ceiling ----------------------------------------------------
        //
        // ⚠⚠ THIS ONE GUARDS AGAINST A CTD, not against a wrong colour. The
        // game crashed on 2026-08-16 going past fifteen worn layers: an access
        // violation on a refcount inside the engine's own face retint. The face
        // tint composites through a sixteen slot shader (TintMask0 to
        // TintMask15, measured in AE FUN_141488380), so there is no slot for the
        // next one to bind.
        CHECK(kMaxWornLayers == 15);

        Snapshot state;
        for (std::size_t i = 0; i < 20; ++i) {
            state.push_back(MakeLayer("a.dds", Rgb{ 1, 2, 3 }, 0.0f));
        }
        CHECK(UsedIn(state) == 0);
        CHECK(RemainingLayers(state) == 15);
        CHECK(WithinCeiling(state));

        for (std::size_t i = 0; i < 14; ++i) {
            state[i].strength = 1.0f;
        }
        CHECK(UsedIn(state) == 14);
        CHECK(RemainingLayers(state) == 1);
        CHECK(WithinCeiling(state));
        CHECK(MayWear(state, 19));

        // Exactly at the ceiling: legal to SEND, but nothing new may go on.
        state[14].strength = 1.0f;
        CHECK(UsedIn(state) == 15);
        CHECK(RemainingLayers(state) == 0);
        CHECK(WithinCeiling(state));
        CHECK(!MayWear(state, 19));

        // ⚠⚠ A LAYER ALREADY WORN MAY ALWAYS BE EDITED. Reading the budget as
        // "no changes at all" would stop a player recolouring what they already
        // have, which is not what runs the engine out of slots.
        CHECK(MayWear(state, 0));
        CHECK(MayWear(state, 14));

        // Over the ceiling, which is reachable from a preset or another mod
        // rather than from this page.
        state[15].strength = 1.0f;
        CHECK(UsedIn(state) == 16);
        CHECK(!WithinCeiling(state));
        CHECK(RemainingLayers(state) == 0);
        // ⚠ TURNING ONE OFF MUST STILL BE ALLOWED, or a character over the
        // ceiling could never be brought back under it.
        CHECK(MayWear(state, 15));
        state[15].strength = 0.0f;
        CHECK(WithinCeiling(state));

        // A black layer at full strength counts, because black is a colour.
        Snapshot black;
        black.push_back(MakeLayer("x.dds", Rgb{ 0, 0, 0 }, 1.0f));
        CHECK(UsedIn(black) == 1);
        CHECK(RemainingLayers(black) == 14);

        // An empty list is trivially within, and a bad index does not read out
        // of range.
        CHECK(WithinCeiling(Snapshot{}));
        CHECK(MayWear(Snapshot{}, 0));
        CHECK(MayWear(Snapshot{}, 999));
    }

    {  // ---- what backs the head's tint texture ----------------------------
        //
        // ⚠⚠ THE ALIAS IS THE BLACK FACE (field 2026-09-09, three logs). The
        // chargen's retint backs the face with a view onto the shared tint
        // render target and no texture of its own; a head build's texture owns
        // a copy. The texture pointer decides, and nothing else may.
        CHECK(JudgeTintBacking(false, false) == TintBacking::kNone);
        CHECK(JudgeTintBacking(false, true) == TintBacking::kNone);
        CHECK(JudgeTintBacking(true, false) == TintBacking::kAlias);
        CHECK(JudgeTintBacking(true, true) == TintBacking::kPrivate);
        CHECK(std::string{ TintBackingLabel(TintBacking::kAlias) } ==
              "ALIAS of the tint render target");
        CHECK(std::string{ TintBackingLabel(TintBacking::kPrivate) } == "private");
        CHECK(std::string{ TintBackingLabel(TintBacking::kNone) } == "no backing");
    }

    {  // ⚠⚠ THE TAXONOMY, WHICH IS THE THING THE RACE RECORD MEASURED. Every
        // structural type is one slot per character with a texture the race
        // fixes, so it takes a colour and nothing else. Only war paint and dirt
        // are multi-slot, and only they take a library.
        for (const auto type : { Type::kLips, Type::kCheeks, Type::kLowerCheeks,
                                 Type::kEyeliner, Type::kUpperEyeSocket,
                                 Type::kLowerEyeSocket, Type::kSkinTone, Type::kFrownLines,
                                 Type::kNose, Type::kChin, Type::kNeck, Type::kForehead }) {
            const auto category = CategoryOf(static_cast<std::uint32_t>(type));
            CHECK(category == Category::kFace);
            CHECK(!AllowsTexture(category));
            CHECK(!AllowsClear(category));
        }
        CHECK(CategoryOf(static_cast<std::uint32_t>(Type::kWarPaint)) == Category::kWarPaint);
        CHECK(CategoryOf(static_cast<std::uint32_t>(Type::kDirt)) == Category::kDirt);
        CHECK(AllowsTexture(Category::kWarPaint));
        CHECK(AllowsClear(Category::kWarPaint));
        CHECK(AllowsTexture(Category::kDirt));
        CHECK(AllowsClear(Category::kDirt));

        // ⚠ TYPE 0 IS "None" TO THE RACE RECORD AND NO VANILLA SLOT CARRIES IT,
        // so it sorts with the unknowns and keeps every control rather than
        // being locked as a face slot on the strength of a guessed name.
        CHECK(CategoryOf(0) == Category::kOther);
        CHECK(CategoryOf(99) == Category::kOther);
        CHECK(AllowsTexture(Category::kOther));
        CHECK(AllowsClear(Category::kOther));

        // The section table lines up with the enum, same rule the type table
        // above is held to.
        CHECK(kCategories.size() == kCategoryCount);
        for (std::size_t i = 0; i < kCategories.size(); ++i) {
            CHECK(CategoryIndex(kCategories[i].category) == i);
            CHECK(kCategories[i].labelKey[0] == '$');
        }
        CHECK(std::string{ CategoryId(Category::kDirt) } == "dirt");
    }

    {  // ⚠⚠ THE BUDGET HAS TO BE LEGIBLE PER SECTION, because a normal
        // character spends most of the fifteen on structural slots before any
        // war paint goes on.
        std::vector<Layer> layers;
        Snapshot           state;
        const auto         add = [&](Type a_type, float a_strength) {
            layers.push_back(Layer{ static_cast<std::uint32_t>(layers.size()),
                                    static_cast<std::uint32_t>(a_type) });
            state.push_back(MakeLayer("x.dds", Rgb{ 10, 20, 30 }, a_strength));
        };
        add(Type::kSkinTone, 1.0f);
        add(Type::kLips, 0.5f);
        add(Type::kNeck, 0.0f);  // present but off
        add(Type::kWarPaint, 1.0f);
        add(Type::kWarPaint, 0.0f);
        add(Type::kDirt, 0.25f);

        CHECK(UsedIn(state) == 4);
        CHECK(UsedIn(state, layers, Category::kFace) == 2);
        CHECK(UsedIn(state, layers, Category::kWarPaint) == 1);
        CHECK(UsedIn(state, layers, Category::kDirt) == 1);
        CHECK(UsedIn(state, layers, Category::kOther) == 0);

        // ⚠ A LAYER LIST SHORTER THAN THE SNAPSHOT COUNTS THE TAIL AS UNKNOWN
        // rather than dropping it, so no worn layer ever falls out of the total.
        std::vector<Layer> shortList{ layers.front() };
        CHECK(UsedIn(state, shortList, Category::kFace) == 1);
        CHECK(UsedIn(state, shortList, Category::kOther) == 3);
    }

    {  // ⚠⚠ THE ROUND-FOURTEEN SKULL, replayed. The look was captured against a
        // list carrying a war paint at index 11; the session's live list holds
        // freckles there and a stranger's paint further down. Under index+type
        // the skull either skipped (the good rounds) or stomped a stranger
        // (round fourteen). Under MatchSlot it lands on a FREE war paint canvas
        // and never on a worn one.
        const auto warpaint = static_cast<std::uint32_t>(Type::kWarPaint);
        const auto freckles = static_cast<std::uint32_t>(Type::kFreckles);
        const auto skintone = static_cast<std::uint32_t>(Type::kSkinTone);
        const auto eyeliner = static_cast<std::uint32_t>(Type::kEyeliner);

        // The live list, round-fourteen 34-layer shape in miniature:
        // 0 skintone, 1 freckles (the captured index!), 2 a WORN foreign
        // paint, 3 a free paint canvas, 4 another free canvas, 5 eyeliner.
        std::vector<Layer> layers;
        auto               addLayer = [&](std::uint32_t a_type) {
            layers.push_back(Layer{ static_cast<std::uint32_t>(layers.size()), a_type });
        };
        addLayer(skintone);
        addLayer(freckles);
        addLayer(warpaint);
        addLayer(warpaint);
        addLayer(warpaint);
        addLayer(eyeliner);

        Snapshot live;
        live.push_back(MakeLayer("Tintmasks\\SkinTone.dds", Rgb{ 200, 180, 170 }, 1.0f));
        live.push_back(MakeLayer("Tintmasks\\Freckles.dds", Rgb{ 90, 60, 50 }, 0.0f));
        live.push_back(MakeLayer("CO 3\\StrangersPaint.dds", Rgb{ 10, 10, 10 }, 0.8f));
        live.push_back(MakeLayer("Tintmasks\\Blank.dds", Rgb{ 255, 255, 255 }, 0.0f));
        live.push_back(MakeLayer("Tintmasks\\Blank.dds", Rgb{ 255, 255, 255 }, 0.0f));
        live.push_back(MakeLayer("Tintmasks\\Eyeliner01.dds", Rgb{ 0, 0, 0 }, 0.0f));

        std::vector<bool> claimed(live.size(), false);

        const auto skull =
            MakeLayer("CO 3\\56 Head 2 F M.dds", Rgb{ 255, 255, 255 }, 0.388f);

        // Captured at index 1 (the reshaped spot): the type there is freckles,
        // the worn paint at 2 is a stranger's, so the skull takes the first
        // FREE canvas, slot 3.
        const auto slot = MatchSlot(1, warpaint, skull, layers, live, claimed);
        CHECK(slot == 3);
        CHECK(!Occupied(live[slot]));
        claimed[slot] = true;

        // A second captured paint claims the NEXT canvas, not the same one.
        const auto second =
            MakeLayer("CO 3\\OtherPaint.dds", Rgb{ 0, 0, 0 }, 1.0f);
        const auto slot2 = MatchSlot(1, warpaint, second, layers, live, claimed);
        CHECK(slot2 == 4);
        claimed[slot2] = true;

        // A third paint finds every canvas claimed or worn: honest kNoSlot,
        // never the stranger's slot 2.
        const auto third = MakeLayer("CO 3\\Third.dds", Rgb{ 0, 0, 0 }, 1.0f);
        CHECK(MatchSlot(1, warpaint, third, layers, live, claimed) == kNoSlot);

        // Reapply: the skull texture now LIVES in slot 3, so the same entry
        // matches it by identity even with nothing free and the claim map
        // reset. Case and slash may differ between capture and engine.
        Snapshot after = live;
        after[3]       = skull;
        std::vector<bool> fresh(live.size(), false);
        const auto        reapply =
            MakeLayer("co 3/56 head 2 f m.dds", Rgb{ 255, 255, 255 }, 0.388f);
        CHECK(MatchSlot(1, warpaint, reapply, layers, after, fresh) == 3);

        // Structural: eyeliner captured at a stale index still finds the one
        // eyeliner slot by type. Colour-only skintone does too.
        const auto liner = MakeLayer("Tintmasks\\Eyeliner01.dds", Rgb{ 20, 20, 20 }, 0.7f);
        CHECK(MatchSlot(30, eyeliner, liner, layers, live, fresh) == 5);
        LayerState colourOnly{};
        colourOnly.tint     = Rgb{ 1, 2, 3 };
        colourOnly.strength = 1.0f;
        CHECK(MatchSlot(0, skintone, colourOnly, layers, live, fresh) == 0);

        // Colour-only capture of a MULTI-slot type keeps the old index rule:
        // index in range and type matching writes there, anything else skips.
        CHECK(MatchSlot(2, warpaint, colourOnly, layers, live, fresh) == 2);
        CHECK(MatchSlot(1, warpaint, colourOnly, layers, live, fresh) == kNoSlot);

        // Path compare: separators and case fold, lengths do not.
        CHECK(SameTexturePath("A\\b/C.dds", "a/B\\c.dds"));
        CHECK(!SameTexturePath("a.dds", "ab.dds"));
    }

    {  // ⚠⚠ THE 2026-08-23 FIELD CASE: THE TYPE MOVED AND THE TEXTURE DID NOT.
        // One session, one character, eighty seconds apart. A paint captured at
        // index 11 as a war paint was read back at index 11 as FRECKLES,
        // because the face step reshapes the tint list on its way past. Type
        // AND texture therefore missed, and the fallback dropped the payload
        // into the first free canvas, so the preset's copy at 1 and ours at 2
        // composited together and the field saw a colour that had changed.
        // The path is the identity, and it lands on the slot already wearing
        // it.
        const auto warpaint = static_cast<std::uint32_t>(Type::kWarPaint);
        const auto freckles = static_cast<std::uint32_t>(Type::kFreckles);
        const auto skintone = static_cast<std::uint32_t>(Type::kSkinTone);

        std::vector<Layer> layers;
        auto               addLayer = [&](std::uint32_t a_type) {
            layers.push_back(Layer{ static_cast<std::uint32_t>(layers.size()), a_type });
        };
        addLayer(skintone);
        addLayer(freckles);  // the reshaped slot, and the paint is ON it
        addLayer(warpaint);
        addLayer(warpaint);

        Snapshot live;
        live.push_back(MakeLayer("Tintmasks\\SkinTone.dds", Rgb{ 200, 180, 170 }, 1.0f));
        live.push_back(
            MakeLayer("CO 3\\56 Head 2 F M.dds", Rgb{ 255, 255, 255 }, 0.388f));
        live.push_back(MakeLayer("Makeup\\Makeup_009.dds", Rgb{ 102, 75, 75 }, 0.0f));
        live.push_back(MakeLayer("Tintmasks\\Blank.dds", Rgb{ 255, 255, 255 }, 0.0f));

        const auto        paint =
            MakeLayer("CO 3\\56 Head 2 F M.dds", Rgb{ 255, 255, 255 }, 0.388f);
        std::vector<bool> claimed(live.size(), false);
        CHECK(MatchSlot(11, warpaint, paint, layers, live, claimed) == 1);

        // ⚠ AND IT NEVER STEALS A CLAIMED SLOT. With 1 already spoken for the
        // entry falls through to the old behaviour and takes a free canvas,
        // which is the posture that kept a stranger's paint safe.
        std::vector<bool> taken(live.size(), false);
        taken[1] = true;
        CHECK(MatchSlot(11, warpaint, paint, layers, live, taken) == 2);

        // ⚠ THE WIDENING IS ONLY A FALLBACK. A capture whose type and texture
        // both agree with a later slot still prefers that slot, so nothing
        // that matched before matches differently now.
        Snapshot both = live;
        both[3]       = paint;
        std::vector<bool> fresh(live.size(), false);
        CHECK(MatchSlot(11, warpaint, paint, layers, both, fresh) == 3);
    }

    {  // Whose texture is on the head's tint slot
        // ⚠ THE ENGINE'S OWN LITERAL. Retint assigns this name to the texture it
        // builds, so it is the only handle on "the face still shows what we
        // painted". Spelling it twice is what this case exists to stop.
        CHECK(IsLiveFaceBake("Player face tint"));
        CHECK(IsLiveFaceBake(kLiveFaceBakeName));
        // ⚠⚠ THE ONE THE FIELD ACTUALLY MET. skee re-binds a preset's exported
        // tint file over the slot; that is the whole bug and it must read as not
        // ours or the rebake stands down exactly when it is needed.
        CHECK(!IsLiveFaceBake("Textures\\CharGen\\Exported\\FR_Umbrael 17.dds"));
        CHECK(!IsLiveFaceBake(""));
        // ⚠ EXACT, NOT CASE-FOLDED, AND NOT A PREFIX. A near miss that counted
        // as ours would be a silent stand-down, which is the failure mode this
        // whole check exists to avoid.
        CHECK(!IsLiveFaceBake("player face tint"));
        CHECK(!IsLiveFaceBake("Player face tint 2"));
        // ⚠⚠ AND AN EXPORT IS ITS OWN ANSWER, not "somebody else's texture".
        // The two shared one verdict and wanted opposite treatment: a face
        // under CharGen\Exported was baked on purpose, so the skin tone slider
        // must not rebuild over it (user's call 2026-08-27).
        CHECK(IsChargenExport("Textures\\CharGen\\Exported\\FR_Umbrael 19.dds"));
        CHECK(IsChargenExport("textures\\chargen\\exported\\anything.dds"));
        CHECK(IsChargenExport("Textures/CharGen/Exported/FR_Nord 3.dds"));
        CHECK(!IsChargenExport("Textures\\Actors\\Character\\Female\\femalehead_d.dds"));
        CHECK(!IsChargenExport("Player face tint"));
        CHECK(!IsChargenExport("chargen\\exported"));  // the folder, no file under it
        CHECK(!IsChargenExport(""));
        CHECK(!IsLiveFaceBake("Player face"));
    }

    {  // Which export is THIS character's face, and which is somebody's residue
        // ⚠⚠ THE QUESTION THE CELL-CHANGE HEAD-SKIN REPORT TURNED ON. Both a
        // bled face and the look being worn right now are chargen exports, so
        // the arm that re-bakes residue re-baked the live one too. Measured
        // walking out of a building 2026-09-01: a head build bound
        // 'FR_Almalexia.dds', the watch met it 0.6 s later and re-baked it away.
        CHECK(NamesTheSameExport("Textures\\CharGen\\Exported\\FR_Almalexia.dds",
                                 "Textures\\CharGen\\Exported\\FR_Almalexia.dds"));
        // ⚠ CASE AND SLASH FOLD, because the bound name comes off the engine
        // and the held one is built by PathFor, and they need not agree.
        CHECK(NamesTheSameExport("textures/chargen/exported/fr_almalexia.dds",
                                 "Textures\\CharGen\\Exported\\FR_Almalexia.dds"));
        // ⚠⚠ AND THE r88 CURE IS UNTOUCHED: a face baked for somebody else
        // carries a jslot this character does not hold, so it still re-bakes.
        CHECK(!NamesTheSameExport("Textures\\CharGen\\Exported\\FR_Umbrael 19.dds",
                                  "Textures\\CharGen\\Exported\\FR_Almalexia.dds"));
        // ⚠ A PREFIX IS NOT A MATCH. 'FR_Alma' must not spare 'FR_Almalexia'.
        CHECK(!NamesTheSameExport("Textures\\CharGen\\Exported\\FR_Alma.dds",
                                  "Textures\\CharGen\\Exported\\FR_Almalexia.dds"));
        // Nothing held, and nothing bound, are both "not this character's".
        CHECK(!NamesTheSameExport("Textures\\CharGen\\Exported\\FR_Almalexia.dds", ""));
        CHECK(!NamesTheSameExport("", "Textures\\CharGen\\Exported\\FR_Almalexia.dds"));
        CHECK(!NamesTheSameExport("", ""));
    }

    {  // The four answers the head's tint slot can give
        // ⚠⚠ THE ONE THE FIELD MET SECOND, AND THE REASON THIS TABLE EXISTS.
        // A rebuilt material carries a tint texture the engine has not named,
        // and both readers of that state used to call it "nothing to do". It is
        // the opposite: the composite is gone and only a re-bake puts it back.
        CHECK(JudgeFaceTint(true, "") == FaceTintVerdict::kCompositeGone);
        // ⚠ AND NOT WHEN THERE IS NO HEAD. Same empty string, different
        // answer, which is exactly the pair that was conflated.
        CHECK(JudgeFaceTint(false, "") == FaceTintVerdict::kNoHead);
        CHECK(JudgeFaceTint(false, "Player face tint") == FaceTintVerdict::kNoHead);
        CHECK(JudgeFaceTint(true, "Player face tint") == FaceTintVerdict::kOurs);
        CHECK(JudgeFaceTint(true, kLiveFaceBakeName) == FaceTintVerdict::kOurs);
        // ⚠⚠ THE 20:52 ADVERSARY IS ITS OWN ANSWER NOW. skee's exported preset
        // file bound over ours is still the thing a write has to fight, but it
        // is a face somebody made rather than a stranger's texture, and only a
        // write defending its own retint may rebuild over it. MakeupApi holds
        // that half; this table only has to keep the two apart.
        CHECK(JudgeFaceTint(true, "Textures\\CharGen\\Exported\\FR_Umbrael 17.dds") ==
              FaceTintVerdict::kPresetFace);
        // ⚠ A NEAR MISS IS DISPLACED, NEVER OURS. A stand-down that reads a
        // near miss as ours is silent, which is the worst shape this can take.
        CHECK(JudgeFaceTint(true, "player face tint") == FaceTintVerdict::kDisplaced);
        CHECK(JudgeFaceTint(true, "Player face tint 2") == FaceTintVerdict::kDisplaced);
        // ⚠ AND A TEXTURE THAT IS NEITHER IS STILL A DISPLACEMENT, which is
        // the arm that puts our bake back when a third party takes the slot.
        CHECK(JudgeFaceTint(true, "Textures\\Actors\\Character\\femalehead_d.dds") ==
              FaceTintVerdict::kDisplaced);
    }

    {  // The face watch's schedule, which is a window and not a look count
        // ⚠⚠ SIX SECONDS ON BOTH EDGES, BEFORE AND AFTER. Field 2026-08-26
        // measured the re-bind at 1.1 s after a build and 4.1 s after a write,
        // and the guard's whole point is that shortening the wrongness the field
        // sees must not widen the fight with skee. Both cycles still end at the
        // same six seconds they always did.
        CHECK(FaceWatchWindowMs(kFaceWriteFirstLookMs) == 6000);
        CHECK(FaceWatchWindowMs(kFaceBuildFirstLookMs) == 6000);
        // ⚠ AND THE FIRST LOOK IS INSIDE A SECOND, which is the half of the
        // schedule the field actually experiences.
        CHECK(kFaceWriteFirstLookMs <= 1000);
        CHECK(kFaceBuildFirstLookMs <= 1000);
        CHECK(kFaceLookSpacingMs <= 1000);
        CHECK(kFaceLookCount > 3);
        // ⚠⚠ THE QUIET PERIOD MUST OUTLAST THE WATCH'S OWN WINDOW. A look apply
        // keeps rebuilding the head after it reports complete, and a stand-down
        // that expired inside the watch's window would let the last of those
        // rebuilds repair away the face the apply had just imported.
        CHECK(kFaceWatchQuietAfterApplyMs > FaceWatchWindowMs(kFaceWriteFirstLookMs));
        CHECK(kFaceWatchQuietAfterApplyMs > FaceWatchWindowMs(kFaceBuildFirstLookMs));
    }

    // ---- OS-209 on makeup: what path a layer's mask should hold -------------
    {
        // ⚠⚠ EMPTY IS "KEEP WHAT THE RACE GAVE IT", NOT "BLANK THE SLOT", and
        // that is the one place this differs from OverlayPlan::WirePath. An
        // overlay has a default texture standing for emptiness; a makeup slot
        // always carries the race's art, so the writer reads an empty answer as
        // a layer it must not touch the texture of.
        LayerState none;
        CHECK(WirePath(none).empty());
        none.hasTexture = true;
        CHECK(WirePath(none).empty());  // flag set, path still empty

        // An unmoved layer is its own source, byte for byte. A bake would be a
        // copy of the art at the art's own transform, which is a file nobody
        // needs.
        LayerState plain;
        plain.hasTexture = true;
        plain.texture    = "textures\\actors\\character\\makeup\\rose.dds";
        CHECK(WirePath(plain) == plain.texture);

        // Moved, and the answer is the baked file rather than the art.
        LayerState moved = plain;
        moved.transform.offsetX = 0.25f;
        const auto wire         = WirePath(moved);
        CHECK(wire != moved.texture);
        CHECK(OS::OverlayTransform::IsBakedPath(wire));
        CHECK(wire == OS::OverlayTransform::BakedPath(moved.texture, moved.transform));

        // ⚠ ONE CACHE KEY, TWO SYSTEMS. The key is the inputs and neither the
        // page nor the family is in it, so a face tint and an overlay naming one
        // source at one transform ask for one file. Anything that made these
        // differ would double the bake cache for no gain.
        CHECK(wire == OS::OverlayPlan::WirePath([&] {
                  OS::OverlayPlan::LayerState ovl;
                  ovl.hasTexture = true;
                  ovl.texture    = moved.texture;
                  ovl.transform  = moved.transform;
                  return ovl;
              }()));

        // Every scale, rotation and offset field reaches the key, so two layers
        // that differ in any one of them cannot share a file.
        LayerState scaled = plain;
        scaled.transform.scale = 2.0f;
        LayerState turned      = plain;
        turned.transform.rotationDeg = 90.0f;
        LayerState up                = plain;
        up.transform.offsetY         = -0.5f;
        CHECK(WirePath(scaled) != wire);
        CHECK(WirePath(turned) != wire);
        CHECK(WirePath(up) != wire);
        CHECK(WirePath(scaled) != WirePath(turned));

        // ⚠ A TRANSFORM IS PART OF THE LAYER'S IDENTITY. Differences() drives
        // which layers a write sends, so a transform the comparison could not
        // see would be edited on the page and never reach the face.
        Snapshot before{ plain };
        Snapshot after{ moved };
        CHECK(Differences(before, after).size() == 1);
        CHECK(Differences(before, before).empty());

        // ⛔ AND A FACE SLOT NEVER GETS THIS. Its art is the race's own feature
        // mask and it carries no picker, so the same predicate that withholds
        // the picker withholds the sliders.
        CHECK(!AllowsTexture(Category::kFace));
        CHECK(AllowsTexture(Category::kWarPaint));
        CHECK(AllowsTexture(Category::kDirt));
        CHECK(AllowsTexture(Category::kOther));
    }

    {  // ⚠⚠ THE FACE-CARRIED PLAN: EVERYTHING THE LOOK WEARS GOES IN EXCEPT
        // THE TONE. The user's A/B of 2026-09-01: a look applied whole left its
        // makeup empty while applying the makeup alone loaded it perfectly,
        // because the step stood aside whenever a face was carried. The first
        // cut wrote the paint library only, and the field answered the same
        // evening: the LIPS went bare on the first face-overlay edit, because
        // the edit's retint composites from the list and skee never populates
        // it. So the write takes every worn capture except the skin tone,
        // whose hold the apply deliberately releases, while the clear still
        // asks AllowsClear and so never empties a complexion slot.
        struct Captured {
            std::uint32_t index;
            std::uint32_t type;
            LayerState    state;
        };
        const auto warpaint = static_cast<std::uint32_t>(Type::kWarPaint);
        const auto dirt     = static_cast<std::uint32_t>(Type::kDirt);
        const auto skintone = static_cast<std::uint32_t>(Type::kSkinTone);
        const auto lips     = static_cast<std::uint32_t>(Type::kLips);

        std::vector<Layer> layers;
        auto               addLayer = [&](std::uint32_t a_type) {
            layers.push_back(Layer{ static_cast<std::uint32_t>(layers.size()), a_type });
        };
        addLayer(skintone);
        addLayer(warpaint);  // the previous look's marking, worn
        addLayer(warpaint);  // a free canvas
        addLayer(dirt);      // race art, unworn
        addLayer(lips);      // the race's lips mask, worn by the previous look

        Snapshot live;
        live.push_back(MakeLayer("Tintmasks\\SkinTone.dds", Rgb{ 200, 180, 170 }, 1.0f));
        live.push_back(MakeLayer("CO 3\\Stranger.dds", Rgb{ 10, 10, 10 }, 0.9f));
        live.push_back(MakeLayer("Makeup\\Default.dds", Rgb{ 255, 255, 255 }, 0.0f));
        live.push_back(MakeLayer("Tintmasks\\Dirt01.dds", Rgb{ 90, 70, 50 }, 0.0f));
        live.push_back(MakeLayer("Tintmasks\\FemaleHeadNord_Lips.dds", Rgb{ 60, 30, 40 }, 0.8f));

        {  // The look brings a war paint, a dirt tone and its LIPS beside its
            // skin tone. The lips are the 20:12 field shape: a different mask
            // texture than the slot wears, landing by the one-lips-slot rule.
            std::vector<Captured> captured;
            captured.push_back({ 0, skintone,
                                 MakeLayer("", Rgb{ 1, 2, 3 }, 1.0f) });
            captured.push_back({ 1, warpaint,
                                 MakeLayer("CO 3\\A.dds", Rgb{ 255, 0, 0 }, 0.5f) });
            LayerState dirtTone{};
            dirtTone.tint     = Rgb{ 80, 60, 40 };
            dirtTone.strength = 0.4f;
            captured.push_back({ 3, dirt, dirtTone });
            captured.push_back({ 6, lips,
                                 MakeLayer("!COR\\TintMasks\\femaleheadnord_lips.dds",
                                           Rgb{ 0x78, 0x48, 0x61 }, 0.73f) });

            const auto plan = PlanFaceCarried(captured, layers, live);
            CHECK(plan.written == 3);   // the war paint, the dirt and the lips
            CHECK(plan.kept == 1);      // the skin tone stays the preset's
            CHECK(plan.cleared == 1);   // the stranger's marking goes
            CHECK(plan.skipped == 0);
            // The skin tone slot is byte-for-byte the live one.
            CHECK(plan.target[0].texture == live[0].texture);
            CHECK(NearlyEqual(plan.target[0].strength, live[0].strength));
            CHECK(plan.target[0].tint == live[0].tint);
            // The payload landed on the free canvas, not the worn one.
            CHECK(plan.target[2].texture == "CO 3\\A.dds");
            CHECK(NearlyEqual(plan.target[2].strength, 0.5f));
            // The stranger's slot is emptied, texture released to the race.
            CHECK(!plan.target[1].hasTexture);
            CHECK(NearlyEqual(plan.target[1].strength, 0.0f));
            // A colour-only capture keeps the slot's own art.
            CHECK(plan.target[3].hasTexture);
            CHECK(plan.target[3].texture == "Tintmasks\\Dirt01.dds");
            CHECK(NearlyEqual(plan.target[3].strength, 0.4f));
            // The lips land on the one lips slot, stale index and all, and the
            // captured mask rides along.
            CHECK(plan.target[4].texture == "!COR\\TintMasks\\femaleheadnord_lips.dds");
            CHECK(plan.target[4].tint == (Rgb{ 0x78, 0x48, 0x61 }));
            CHECK(NearlyEqual(plan.target[4].strength, 0.73f));
            // Every touched slot is named for the write, and only those.
            CHECK(plan.indices.size() == 4);
        }

        {  // A capture held at zero strength writes nothing and resurrects
            // nothing: the slot it would have claimed is cleared instead, and
            // ⚠⚠ SO ARE THE UNCLAIMED LIPS. Second field round of 2026-09-01:
            // the eraser that spared the kFace band left Almalexia's lips on
            // Umbrael, whose capture holds none. The eraser now asks the
            // write's own question, everything but the tone.
            std::vector<Captured> captured;
            captured.push_back({ 1, warpaint,
                                 MakeLayer("CO 3\\Stranger.dds", Rgb{ 10, 10, 10 }, 0.0f) });
            const auto plan = PlanFaceCarried(captured, layers, live);
            CHECK(plan.written == 0);
            CHECK(plan.cleared == 4);  // stranger, canvas art, dirt art, lips
            CHECK(!plan.target[1].hasTexture);
            CHECK(!plan.target[2].hasTexture);
            CHECK(!plan.target[3].hasTexture);
            CHECK(!plan.target[4].hasTexture);
            CHECK(NearlyEqual(plan.target[4].strength, 0.0f));
            // The tone still stands: it is the one slot the plan never manages.
            CHECK(plan.target[0].hasTexture);
            CHECK(NearlyEqual(plan.target[0].strength, 1.0f));
        }

        {  // A reapply of the paint already worn claims its own slot: updated in
            // place, never cleared-and-elsewhere. The unclaimed canvas, dirt
            // art and lips are released.
            std::vector<Captured> captured;
            captured.push_back({ 1, warpaint,
                                 MakeLayer("co 3/stranger.dds", Rgb{ 0, 255, 0 }, 0.7f) });
            const auto plan = PlanFaceCarried(captured, layers, live);
            CHECK(plan.written == 1);
            CHECK(plan.cleared == 3);
            CHECK(plan.target[1].tint == (Rgb{ 0, 255, 0 }));
            CHECK(NearlyEqual(plan.target[1].strength, 0.7f));
            CHECK(plan.indices.size() == 4);
        }

        {  // More payloads than canvases: the honest skip, and the count says so.
            std::vector<Captured> captured;
            captured.push_back({ 1, warpaint,
                                 MakeLayer("CO 3\\A.dds", Rgb{ 255, 0, 0 }, 0.5f) });
            captured.push_back({ 2, warpaint,
                                 MakeLayer("CO 3\\B.dds", Rgb{ 0, 0, 255 }, 0.5f) });
            captured.push_back({ 3, warpaint,
                                 MakeLayer("CO 3\\C.dds", Rgb{ 0, 255, 255 }, 0.5f) });
            const auto plan = PlanFaceCarried(captured, layers, live);
            // A lands on the free canvas; B and C find every same-type canvas
            // worn or claimed and take the honest kNoSlot rather than the
            // stranger's slot. The stranger's marking, the dirt art and the
            // lips are cleared.
            CHECK(plan.written == 1);
            CHECK(plan.skipped == 2);
            CHECK(plan.cleared == 3);
        }

        {  // ⚠⚠ THE 'NORD 3' SHAPE: a look that wears nothing beyond its tone
            // leaves nothing. Field 2026-09-01, second round: its apply stood
            // aside, the kFace band survived, the panel showed makeup the face
            // did not wear, and the next edit's retint painted it on. The plan
            // now runs for an empty capture too, and its eraser takes every
            // occupied slot but the tone.
            std::vector<Captured> captured;
            captured.push_back({ 0, skintone,
                                 MakeLayer("", Rgb{ 200, 180, 170 }, 1.0f) });
            const auto plan = PlanFaceCarried(captured, layers, live);
            CHECK(plan.written == 0);
            CHECK(plan.kept == 1);
            CHECK(plan.cleared == 4);
            CHECK(plan.target[0].hasTexture);         // the tone stands
            CHECK(!plan.target[4].hasTexture);        // the lips do not
            CHECK(plan.indices.size() == 4);
        }
    }

    {  // ---- the saved tint layers, the third copy (field 2026-09-02) --------
        // The mirror keys each live slot by the race asset's TINI index at the
        // same position, carries a cleared slot as a zero layer, and skips a
        // slot the race has no asset for. 0.388 rides as 39, the byte the
        // engine reads back times 0.01, which is how Umbrael's skull came back
        // at exactly 0.388 on a Nord.
        Snapshot live;
        live.push_back(MakeLayer("skintone.dds", Rgb{ 167, 134, 122 }, 1.0f));
        live.push_back(MakeLayer("56 Head 2 F M.dds", Rgb{ 255, 255, 255 }, 0.388f));
        live.push_back(MakeLayer("Makeup_010.dds", Rgb{ 10, 20, 30 }, 0.0f));
        live.push_back(MakeLayer("stranger.dds", Rgb{ 1, 2, 3 }, 0.5f));
        live.push_back(MakeLayer("Makeup_015.dds", Rgb{ 0, 0, 0 }, 1.0f));
        const std::vector<int> indices{ 0, 11, 13, kNoTintIndex, 14 };
        const auto             plan = PlanSavedLayers(live, indices);
        CHECK(plan.size() == 4);
        CHECK(plan[0].tintIndex == 0 && NearlyEqual(plan[0].strength, 1.0f));
        CHECK(plan[1].tintIndex == 11 && plan[1].tint.r == 255 &&
              NearlyEqual(plan[1].strength, 0.388f));
        CHECK(InterpolationOf(plan[1].strength) == 39);
        CHECK(plan[2].tintIndex == 13 && NearlyEqual(plan[2].strength, 0.0f));
        CHECK(InterpolationOf(plan[2].strength) == 0);
        CHECK(plan[3].tintIndex == 14 && InterpolationOf(plan[3].strength) == 100);
        CHECK(InterpolationOf(0.333f) == 33);
        CHECK(InterpolationOf(1.7f) == 100);
        CHECK(InterpolationOf(-0.2f) == 0);
        // A live list longer than the race's index table stops at the table.
        const std::vector<int> shorter{ 0, 11 };
        CHECK(PlanSavedLayers(live, shorter).size() == 2);
    }

    {  // ---- the skin tone slot by its file name (field 2026-09-02 02:58) ----
        // The identity the type field cannot give on a list that is not this
        // race's: every race and every preset calls the mask SkinTone.dds.
        CHECK(IsSkinToneTexture("Actors\\Character\\Character Assets\\TintMasks\\SkinTone.dds"));
        CHECK(IsSkinToneTexture("actors/character/character assets/tintmasks/skintone.dds"));
        CHECK(IsSkinToneTexture("SkinTone.dds"));
        CHECK(!IsSkinToneTexture("!COR\\TintMasks\\femaleheadnord_lips.dds"));
        CHECK(!IsSkinToneTexture("Actors\\Character\\MySkinTone.dds"));
        CHECK(!IsSkinToneTexture("tone.dds"));
        CHECK(!IsSkinToneTexture(""));
    }

    {  // ---- the worn-set watch reads a change by CONTENT (field 2026-09-02 03:44) ----
        // RaceMenu's replay rewrites many slots in one tick; a slider moves
        // one. And the replay's content is the list at the last RaceSexMenu
        // close, so a wholesale change EQUAL to that copy is the replay and a
        // wholesale change that is not (a preset loaded inside RaceMenu) is
        // the user's.
        const auto slot = [](std::uint32_t a_index, std::uint8_t a_alpha, const char* a_texture,
                             bool a_tone = false) {
            WornSlot s;
            s.index   = a_index;
            s.alpha   = a_alpha;
            s.texture = a_texture;
            s.tone    = a_tone;
            return s;
        };
        WornSet before;
        before.size  = 108;
        before.slots = { slot(0, 167, "SkinTone.dds", true), slot(11, 99, "56 Head 2 F M.dds"),
                         slot(13, 85, "Makeup_010.dds") };
        CHECK(ClassifyWornChange(before, before) == WornChange::kNone);
        CHECK(WornSlotsChanged(before, before) == 0);
        // one slider tick: one slot's strength
        WornSet oneAlpha = before;
        oneAlpha.slots[1].alpha = 100;
        CHECK(ClassifyWornChange(before, oneAlpha) == WornChange::kSingle);
        // one art pick at the same strength is still one slot
        WornSet oneArt = before;
        oneArt.slots[2].texture = "Makeup_011.dds";
        CHECK(ClassifyWornChange(before, oneArt) == WornChange::kSingle);
        // the same art spelled the other way is no change at all
        WornSet spelled = before;
        spelled.slots[2].texture = "MAKEUP_010.DDS";
        CHECK(ClassifyWornChange(before, spelled) == WornChange::kNone);
        // a slot switched off is one slot
        WornSet oneOff = before;
        oneOff.slots.pop_back();
        CHECK(ClassifyWornChange(before, oneOff) == WornChange::kSingle);
        // the tone dragged in RaceMenu is one slot too
        WornSet toneDrag = before;
        toneDrag.slots[0].alpha = 255;
        CHECK(ClassifyWornChange(before, toneDrag) == WornChange::kSingle);
        // the 03:44:49 replay: the Nord's four over Umbrael's, in one tick
        WornSet replay;
        replay.size  = 108;
        replay.slots = { slot(0, 255, "SkinTone.dds", true), slot(1, 107, "MaleUpperEyeSocket.dds"),
                         slot(2, 96, "MaleLowerEyeSocket.dds"), slot(6, 48, "MaleHeadNord_Lips.dds") };
        CHECK(ClassifyWornChange(before, replay) == WornChange::kWholesale);
        CHECK(WornSlotsChanged(before, replay) == 6);
        // a list whose LENGTH moved was rebuilt, whatever it wears
        WornSet rebuilt = before;
        rebuilt.size    = 34;
        CHECK(ClassifyWornChange(before, rebuilt) == WornChange::kWholesale);

        // The copy match: index and strength of every worn slot but the tone,
        // whatever the length. The 03:43:58 close was 34 slots; the replay
        // landed on 108 with the same four.
        WornSet closeEdge;
        closeEdge.size  = 34;
        closeEdge.slots = { slot(0, 255, "SkinTone.dds", true), slot(1, 107, "a.dds"),
                            slot(2, 96, "b.dds"), slot(6, 48, "c.dds") };
        CHECK(SameWornSet(closeEdge, replay));
        WornSet toneMoved = closeEdge;
        toneMoved.slots[0].alpha = 167;  // the held tone re-asserted at the close edge
        CHECK(SameWornSet(toneMoved, replay));
        WornSet strengthOff = replay;
        strengthOff.slots[3].alpha = 49;
        CHECK(!SameWornSet(closeEdge, strengthOff));
        WornSet extra = replay;
        extra.slots.push_back(slot(7, 10, "d.dds"));
        CHECK(!SameWornSet(closeEdge, extra));
        WornSet fewer = replay;
        fewer.slots.pop_back();
        CHECK(!SameWornSet(closeEdge, fewer));
        WornSet shuffled;
        shuffled.size  = 108;
        shuffled.slots = { slot(6, 48, "c.dds"), slot(2, 96, "b.dds"), slot(1, 107, "a.dds"),
                           slot(0, 255, "SkinTone.dds", true) };
        CHECK(SameWornSet(closeEdge, shuffled));
        WornSet bareA;
        bareA.size  = 108;
        bareA.slots = { slot(0, 255, "SkinTone.dds", true) };
        WornSet bareB;
        bareB.size = 34;
        CHECK(SameWornSet(bareA, bareB));

        // The verdict inside the editor.
        CHECK(JudgeEditorChange(true, WornChange::kWholesale, false, false) == EditorVerdict::kInit);
        CHECK(JudgeEditorChange(true, WornChange::kSingle, true, true) == EditorVerdict::kInit);
        CHECK(JudgeEditorChange(false, WornChange::kNone, true, true) == EditorVerdict::kNothing);
        CHECK(JudgeEditorChange(true, WornChange::kNone, true, true) == EditorVerdict::kNothing);
        CHECK(JudgeEditorChange(false, WornChange::kSingle, true, true) == EditorVerdict::kUsersSlot);
        CHECK(JudgeEditorChange(false, WornChange::kWholesale, true, true) == EditorVerdict::kReplay);
        CHECK(JudgeEditorChange(false, WornChange::kWholesale, true, false) ==
              EditorVerdict::kUsersWholesale);
        CHECK(JudgeEditorChange(false, WornChange::kWholesale, false, false) ==
              EditorVerdict::kUnknownWholesale);
        // a match claimed with no copy on record is no match
        CHECK(JudgeEditorChange(false, WornChange::kWholesale, false, true) ==
              EditorVerdict::kUnknownWholesale);
    }

    {  // ---- the list is rebuilt to the race's slots after a switch (r35) ----
        // The engine's editor init rebuilds the live list from the race; the
        // apply path never did, so a race-switching apply kept the old race's
        // list all session. The trigger: this apply switched race or sex, or
        // the list is another race's length. No race data, no rebuild.
        CHECK(NeedsRaceRebuild(true, 108, 108));
        CHECK(NeedsRaceRebuild(true, 34, 34));
        CHECK(NeedsRaceRebuild(false, 108, 34));
        CHECK(NeedsRaceRebuild(false, 34, 0));
        CHECK(!NeedsRaceRebuild(false, 108, 108));
        CHECK(!NeedsRaceRebuild(false, 0, 34));
        CHECK(!NeedsRaceRebuild(true, 0, 34));
    }

    {  // ---- which foreign change names RaceMenu's copy (field 2026-09-02 05:19 to 05:26) ----
        // The replay after a load landed 11 to 32 seconds after the post-load
        // edge over four loads, so a fixed short window is the wrong instrument:
        // the FIRST wholesale change that was not ours after a load edge is the
        // replay, however late it comes, and one per load. After an apply the
        // replay came 0.6 s later (03:13); that window stays. The revert's
        // empty list is never a replay.
        CHECK(RemembersAsCopy(WornChange::kWholesale, 108, true, false));
        CHECK(RemembersAsCopy(WornChange::kWholesale, 108, false, true));
        CHECK(!RemembersAsCopy(WornChange::kWholesale, 108, false, false));
        CHECK(!RemembersAsCopy(WornChange::kSingle, 108, true, true));
        CHECK(!RemembersAsCopy(WornChange::kNone, 108, true, true));
        CHECK(!RemembersAsCopy(WornChange::kWholesale, 0, true, true));
    }

    if (g_failures == 0) {
        std::printf("MakeupPlanTests OK\n");
        return 0;
    }
    std::printf("MakeupPlanTests: %d failure(s)\n", g_failures);
    return 1;
}
