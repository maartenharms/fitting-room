#pragma once

// The thumbnail camera's decisions as arithmetic over an AABB, apart from any
// DirectX: which side of a weapon to look at, how to roll the frame, and how
// far back to stand. PreviewGrid.h's reason applies here too - no test
// compiles the renderer, so everything decidable over plain values lives in
// this header and is pinned by test.
//
// The framing itself, decided 2026-08-09 after field round 2 showed every
// blade as a thin upright stick: look at the FLAT of the item (the plane of
// its two longest axes, whose normal is the shortest one), run the long axis
// along the card's diagonal (a square's diagonal is the longest line it has),
// and stand exactly far enough back that the AABB's corners reach kFill of
// the frame. The eye is tilted a little off the flat so the picture keeps
// some depth, and the studio light rig rides the camera basis so no
// orientation is ever lit from behind.

#include "PreviewFilter.h"  // the slot bits, so the crops read as slots

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace OS::PreviewFraming {

    inline constexpr float kFovDegrees = 34.0f;
    // How much of the frame's half-angle a corner may reach; the rest is the
    // margin that keeps a blade tip off the card's edge.
    inline constexpr float kFill = 0.86f;
    // The off-flat tilt: toward the width axis and along the length, so the
    // flat reads as a surface rather than as an elevation drawing.
    inline constexpr float kTiltWidth  = 0.35f;
    inline constexpr float kTiltLength = 0.22f;

    struct Vec3 {
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
    };

    [[nodiscard]] inline Vec3  Add(const Vec3& a, const Vec3& b) {
        return { a.x + b.x, a.y + b.y, a.z + b.z };
    }
    [[nodiscard]] inline Vec3  Scale(const Vec3& a, float s) {
        return { a.x * s, a.y * s, a.z * s };
    }
    [[nodiscard]] inline float Dot(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    [[nodiscard]] inline Vec3  Cross(const Vec3& a, const Vec3& b) {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
                 a.x * b.y - a.y * b.x };
    }
    [[nodiscard]] inline float Length(const Vec3& a) {
        return std::sqrt(Dot(a, a));
    }
    // A degenerate input answers +X rather than NaN, so a broken AABB still
    // frames SOMETHING and the failure is a wrong picture, never a crash.
    [[nodiscard]] inline Vec3 Normalize(const Vec3& a) {
        const float len = Length(a);
        if (len <= 1e-6f) {
            return { 1.0f, 0.0f, 0.0f };
        }
        return Scale(a, 1.0f / len);
    }

    [[nodiscard]] inline Vec3 Axis(int a_i) {
        switch (a_i) {
            case 0:  return { 1.0f, 0.0f, 0.0f };
            case 1:  return { 0.0f, 1.0f, 0.0f };
            default: return { 0.0f, 0.0f, 1.0f };
        }
    }

    // Extent ranking, stable on ties so equal boxes frame identically on
    // every machine: longest, second, shortest as world-axis indices.
    struct AxisOrder {
        int longest{ 0 };
        int second{ 1 };
        int shortest{ 2 };
    };

    [[nodiscard]] inline AxisOrder RankAxes(const std::array<float, 3>& a_extent) {
        std::array<int, 3> idx{ 0, 1, 2 };
        std::stable_sort(idx.begin(), idx.end(),
                         [&](int a, int b) { return a_extent[a] > a_extent[b]; });
        return { idx[0], idx[1], idx[2] };
    }

    struct Frame {
        Vec3  eyeDir;    // unit, from the scene centre TOWARD the eye
        Vec3  up;        // unit, the camera's up
        Vec3  lightDir;  // unit, the studio key rebuilt on this basis
        float distance{ 1.0f };  // eye = centre + eyeDir * distance
        float nearZ{ 0.1f };
        float farZ{ 100.0f };
    };

    // The poses, decided by what the scene IS rather than by its box.
    // kDiagonal is the weapon presentation: flat to the eye, long axis on
    // the card's diagonal (field-approved 2026-08-09). kUpright is worn
    // gear: a garment lying on the weapon diagonal reads as floating debris
    // (armour round 2), and its bind pose already carries the one right
    // orientation, standing up +Z and facing +Y, so the camera stands in
    // front, a little to the side and a little above, roll-free. kShield
    // is the diagonal MIRRORED to the other side of the flat: the diagonal
    // always looks down the POSITIVE thickness axis, and a shield's bind
    // pose points its face the other way, so the card showed the strap
    // side (field 2026-08-09). A blade has no wrong side; a shield does.
    enum class Pose : std::uint8_t { kDiagonal, kUpright, kEyes, kShield };

    // The eyes mesh spans both eyeballs across X, and a card framing the
    // staring pair read as horror rather than as a colour choice (field
    // 2026-08-09 round 3). Frame ONE eye: the -X half, the halves being
    // symmetric; Y and Z stay whole.
    [[nodiscard]] inline std::pair<std::array<float, 3>, std::array<float, 3>> EyeHalf(
        const std::array<float, 3>& a_min, const std::array<float, 3>& a_max) {
        auto mx = a_max;
        mx[0]   = a_min[0] + (a_max[0] - a_min[0]) * 0.5f;
        return { a_min, mx };
    }

    // ---- the standard views (OS-204) --------------------------------------
    //
    // ⚠⚠ THE FRAME COMES FROM THE MANNEQUIN, NEVER FROM THE ITEM, AND THAT IS
    // THE WHOLE RULE. The cut before this one took the item's own box, which
    // gave every card a frame of its own: two gauntlets photographed at two
    // different distances, hair sitting at a different height on every card,
    // rings sometimes a hand and sometimes a figure. The ask that replaced it
    // is one line and it is worth quoting, because it is easy to design away
    // from: "we want the different views for specific slots to be
    // standardized so each card has the mannequin in the same position."
    //
    // So a view is a fixed window on the BODY, in fractions of the body's own
    // box, and every card wearing that slot gets exactly that window. Two
    // bodies of different heights still line up, because the fractions are of
    // each body's own extent. An item that sticks out past its window gets
    // cut, and that is the price of the ask rather than an oversight.
    //
    // ⚠ The ITEM still chooses WHICH window, and only that. A slot nobody
    // wrote down falls back to where the item actually sits on the body, so a
    // modded head slot lands in the head view instead of at full length; every
    // card that lands in a view still shares the view exactly.
    enum class Crop : std::uint8_t {
        kWhole, kHead, kEye, kChest, kHand, kFeet, kFacialHair, kRing
    };

    // Which head part a scene IS, when it is one. A bool per kind was two
    // bools and about to be four, and the fourth would have been the one that
    // silently took a beard down the hair path.
    enum class HeadScene : std::uint8_t { kNone, kHair, kFacialHair, kEyes };

    // Each window as { zLo, zHi, xCentreOffset, xHalfWidth }, in fractions of
    // the body box: z from its bottom, x from its centre.
    //
    // ⚠ THESE ARE FIELD KNOBS AND THE CURRENT VALUES ARE A FIRST CUT. The
    // right numbers are whatever puts the item in the middle of the card on
    // this load order's body, which is judged on a card and nowhere else.
    struct View {
        float zLo, zHi, xOff, xHalf;
        // ⚠⚠ HOW FAR FORWARD THE WINDOW REACHES, as a fraction of the body's
        // own depth, and 1.0 means the whole of it. This is the ONLY knob that
        // moves a tight close-up nearer, and it took a field instrument to see
        // why.
        //
        // The camera stands off the window's FRONT FACE: distance works out to
        // the box's maximum y plus the lateral fit, so clipping the BACK of
        // the window changes the centre and the distance by the same amount
        // and moves the camera not at all. Only the front matters.
        //
        // On this rig the body's front face is the BUST at y 10.88 while the
        // eye's front is at 6.97, so a window keeping the whole depth parks
        // the camera four units further out than the subject needs and no
        // amount of tightening the band recovers it. Measured from the
        // preview.boxes line 2026-08-10:
        // mann=(-31.67,-12.43,11.19)..(31.67,10.88,114.48).
        float yHi{ 1.0f };
    };

    // ⚠ THE TOP SITS WELL ABOVE THE CROWN, and that is the fix rather than a
    // margin: a helmet is TALLER than the head it covers, so a window ending
    // at the skull cuts the crest off every one of them (field 2026-08-10).
    // The bottom came up at the same time, because a card that reached the
    // bust spent most of itself on chest.
    //
    // ⚠ THE CENTRE WAS THE DEFECT, NOT THE SPAN, and it was measured rather
    // than nudged (2026-08-10). On this rig's body the head runs f 0.84075 to
    // 0.99928, centre 0.92001, while the shipped window centred on 0.965: a
    // twentieth of the figure too high, which put the head a eighth of a card
    // below centre and cut the jaw. Recentred on the measured 0.919, and
    // WIDENED rather than kept at the old span, because the alternative that
    // held the span gave the crest only 3.9 units of room and the line above
    // is a field report about crests being cut. This keeps 5.25.
    inline constexpr View kHeadView{ 0.799f, 1.039f, 0.00f, 0.135f };
    // The eye sits just under the crown. This is the tightest window there is
    // and it is offset off centre, because a card of BOTH eyes read as horror
    // (field 2026-08-09) and that verdict outlived the head arriving.
    // The eye fills about 40% of the card, which is the field's own number.
    // ⚠ It is the SPAN that decides that, not the centre: the centre was right
    // the first time and the window around it was five times the eye.
    // ⚠ AND THEN THE CENTRE WAS WRONG TOO. Measured 2026-08-10, one eye sits
    // at x f 0.03712 and the window was offset 0.052, which is what "more
    // centred" was reporting. Tightening the span with it takes the eye from
    // 35.8% of the card to 41.0%, which is the field's own number again.
    // ⚠⚠ THE BAND IS ALREADY THE EYE, SO THE ZOOM COMES FROM yHi. One eye is
    // 2.39 units tall and this window is 2.41: tightening it crops the iris
    // and buys almost nothing, because the camera distance is set by the
    // window's front face rather than by its height.
    //
    // The field asked twice for more, and the field log settled it. The body's
    // front face is the bust at y 10.88 and the eye's front is at 6.97, so the
    // whole-depth window stood the camera 9.45 units off a subject that only
    // needed six. Pulling the front face in to 0.85 of the body's depth puts
    // it at y 7.38, four tenths of a unit clear of the eye, and takes the eye
    // from 41% of the card to 66%.
    //
    // ⚠ 0.85 IS NEAR THE FLOOR, not a round number: 0.83 puts the front face
    // at 6.92, which is BEHIND the front of the eye, and the camera would be
    // inside the thing it is photographing.
    inline constexpr View kEyeView{ 0.9280f, 0.9463f, 0.0371f, 0.0165f, 0.85f };
    // ⚠⚠ AND THE WINDOW ABOVE IS ANCHORED TO THE WRONG THING, WHICH IS
    // OS-205. Every fraction in it is a share of the FIGURE's height, and the
    // figure's height is extrapolated from the BODY MESH's own box, so a body
    // replacer moves a window that is trying to frame an eye. The eye does not
    // move with it: a head part is placed by its own authored transform (every
    // one of them on this rig sits at z 120.344) and by the skeleton, and
    // neither knows which body is installed.
    //
    // MEASURED 2026-08-18 off the installed meshes, window against eye:
    //
    //   body mesh                eye window        the eye is at     verdict
    //   UBE   11.1875..114.4760  122.017..124.422  122.915..124.385  fits by .04
    //   3BA   11.4338..113.9850  121.472..123.860  122.915..124.385  TOP CUT .53
    //   HIMBO 11.3191..115.1421  122.722..125.139  123.509..124.820  fits by .32
    //
    // The three windows span 1.28 units on a window 2.40 units tall, so no
    // value of kEyeView is right for all three and a tune against one card
    // moves the other two. That is the report ("correct on UBE, pushed up
    // under the top edge on HIMBO and 3BA") reproduced by arithmetic.
    //
    // So the eye's window is a share of the HEAD's box instead, the one part
    // of the scene that moves with the eye. Measured on High Poly Head, which
    // is the head this load order renders:
    //
    //   female head 109.8637..131.6192, its eye 122.9153..124.3847
    //   male   head 109.3307..131.7054, its eye 123.5090..124.8204
    //
    // The eye's centre is f 0.6337 of the female head's height and f 0.6630 of
    // the male's, so the centre here is the mean of the two and the span is
    // today's 2.4 units carried over. Both eyes are then held with margin, and
    // the two sexes miss centre by the same 27% in opposite directions, which
    // is inside what the field already calls right: the UBE card it approved
    // sits 36% high.
    //
    // ⚠ A FRACTION RATHER THAN AN OFFSET BELOW THE CROWN, though the crown
    // is the tighter fit of the two (0.43 units of spread against 0.65). An
    // offset in units cannot survive a race whose height scale is not 1.0, and
    // the head node carries that scale.
    //
    // ⚠ THE DEPTH RIDES THE HEAD TOO, for the same reason and with a real
    // consequence: 0.85 of the BODY's depth puts the front face at y 5.18 on
    // the installed body, which is BEHIND the eye's own front at 7.63, so the
    // camera was standing off a plane inside the thing it photographs. 0.90 of
    // the head's depth puts it at 7.99 on the female head and 8.20 on the
    // male, a third of a unit clear of the eye in both.
    inline constexpr View kEyeHeadView{ 0.5905f, 0.7055f, 0.1710f, 0.0766f, 0.90f };

    // ⚠⚠ AND THE HEAD BOX IS NOT ENOUGH EITHER, WHICH THE FIELD SAID
    // WITHIN THE HOUR: "the eye card for UBE is now lower and not perfectly
    // centered like it was before, the eyes for 3BA are better but now quite
    // low". Both low, and the log says why. Two mannequins rendered eye cards in
    // the same session (`preview.boxes crop=2`, 2026-08-18 03:20), and they
    // agree about nothing:
    //
    //   head 109.86..131.69 (h 21.83), its eye 122.92..124.38 (1.46 tall)
    //   head 110.63..131.48 (h 20.85), its eye 122.11..124.50 (2.39 tall)
    //
    // The eye's centre is f 0.6317 of the first head's box and f 0.6079 of the
    // second, half a unit apart on a window 2.4 units tall, and the two EYE
    // MESHES are not even the same size: one is 64% taller than the other. A
    // race picks its own head part and its own eye part, so a constant fraction
    // of the head is a third anchor that is close but never right, and the
    // second one is the one that reads worst because a 2.39-unit eye in a
    // 2.40-unit window has no margin to be wrong in.
    //
    // So the eye's window is built FROM THE EYE. It is the subject of its own
    // card, the renderer already measures it, and centring on it is exact for
    // every race, sex, body and eye mesh at once.
    //
    // ⚠ THIS IS NOT THE UNION WITH THE ITEM THAT PreviewRenderer FORBIDS.
    // That rule is about WIDENING a garment's window until it holds whatever
    // the garment sticks out, which makes the frame the item's rather than the
    // slot's. Here the item IS the slot's whole subject, every eye record on
    // one character shares one mesh (28 cards, one box, in that log), and the
    // card is a close-up of an eyeball with no mannequin visible to align.
    //
    // ⚠⚠ THE EYE'S BOX IS NOT THE EYE, AND THE FIELD CAUGHT BOTH HALVES
    // OF THAT IN ONE LOOK: "the eye appears too far right in the cards, it needs
    // to be more centered, and UBE eye looks much smaller". Two eye meshes off
    // this instance, vertices clustered by side rather than boxed:
    //
    //   mesh                 pair reaches x   ONE eye's centre   its own z
    //   LDD EyesFemale       4.080            2.609 (f 0.6395)   122.915..124.385
    //   UBE eyesfemale       3.545            2.351 (f 0.6632)   122.113..124.501
    //   UBE eyesfemale_outer 3.559            2.349 (f 0.6600)   122.096..124.518
    //
    // **x was half a unit out.** One eye's centre is f 0.64 of the pair's half
    // width, not the 0.52 this shipped with, so the window sat 0.49 units inboard
    // of the eye on one mesh and 0.51 on the other. The camera looks down -Y with
    // +Z up, so `XMMatrixLookAtLH` puts SCREEN-RIGHT ON +X: a window centred
    // inboard of the eye pushes the eye right, which is exactly what came back.
    //
    // **And the two meshes are not the same shape.** The UBE eye's box is 62%
    // taller than the LDD one, and ALL of that is at the BOTTOM: their tops agree
    // within 0.133 units while their bottoms are 0.819 apart. A window sized to
    // fill with the box therefore shrinks whichever mesh carries more globe
    // behind the lid, which is the "much smaller" card. So the SIZE comes from
    // the head, the one measurement per character that no eye record can move.
    //
    // ⚠⚠ THE SIZE WAS THE ONLY THING THE BOX GOT WRONG, and hanging the
    // window off the box's TOP to fix it was an over-correction that the field
    // caught next look: "UBE eyes are slightly too low, 3BA eyes are fine". The
    // two meshes are different SHAPES, not the same shape at different heights.
    // Measured by taking each eye's front-most vertices, which is the pupil,
    // because the front pole of an eyeball is where it looks:
    //
    //   UBE eyesfemale   is a globe: 2.388 tall, 2.387 wide, pupil at z 123.307
    //                    and its box centre is 123.307, the same number
    //   LDD EyesFemale   is an aperture patch: 1.469 tall, 2.941 wide, and its
    //                    box centre 123.650 is the middle of what shows
    //
    // So the BOX CENTRE is the thing to look at on both: the pupil axis on a
    // globe, the middle of the aperture on a patch. The box TOP is not, because
    // a globe's top is the crown of the eyeball, hidden behind the lid, and that
    // is exactly the 0.49 units the UBE card was riding high on.

    // The eye's own height, as a share of the head box. Measured on the mesh
    // whose box IS the visible eye: 1.470 of a 21.83-unit head.
    inline constexpr float kEyeHeightOfHead = 0.0673f;
    // The eye's share of the frame's height. The field's number for the card it
    // approved was 66%; this is the one knob to turn for nearer or further.
    inline constexpr float kEyeFill = 0.75f;
    // One eye's centre, as a share of the PAIR's half width. The eyes mesh
    // carries both, so its own centre is the midline. Between the two measured
    // meshes (f 0.6395 and f 0.6632), which lands each within 0.05 units.
    inline constexpr float kEyeSideCentre = 0.65f;
    // ⚠ x IS A SHARE OF THE z HALF AND MUST STAY UNDER 1.0. The framing
    // fits whichever half-extent is larger, so an x window wider than the z one
    // silently decides the distance and every change to the band does nothing
    // (kChestView learned this the expensive way). Under 1.0, x only chooses
    // which eye is centred.
    inline constexpr float kEyeSideHalf = 0.60f;
    // How far past the eye's own front face the camera's plane sits, in frame
    // heights. The plane behind the subject was a real defect on the body
    // anchor; this one cannot go behind the eye because it is measured off it.
    inline constexpr float kEyeClearance = 0.25f;
    // ⚠ AN "EYE" TALLER THAN THIS SHARE OF THE HEAD IS NOT AN EYE, and the
    // window falls back to the head fraction rather than framing whatever
    // arrived. The two measured eyes are f 0.067 and f 0.115 of their heads, so
    // this leaves a factor of three of room for a mesh that carries lashes or
    // wetness with it.
    inline constexpr float kEyeMaxShare = 0.35f;
    // A beard, 70% closer than the head window it used to share (field
    // 2026-08-10). ⚠ THE CENTRE IS A GUESS AND THE SPAN IS NOT: the span is
    // the head window's divided by 1.7, which is what was asked for, but no
    // beard has been measured on this body the way the head and hands were, so
    // where the window SITS is a field knob. It is centred low, on the chin
    // rather than on the head, because a beard hangs BELOW the jaw and the
    // head window's own centre would have cut it off at the bottom edge.
    inline constexpr View kFacialHairView{ 0.799f, 0.941f, 0.00f, 0.079f };
    // Amulets. Tightened three times on the field's word, and the third time
    // the number came off the meshes instead: the amulets on this rig centre
    // at z 107.0 to 108.6 (gold, silver, Mara, Dibella, from their authored
    // bounding spheres), which is f 0.812 to 0.824 of the reference box, while
    // the window it had ran 0.700 to 0.870 and centred on 0.785. Nearly half
    // the card was empty chest BELOW the pendant, which is what "zoom in way
    // more" was looking at.
    //
    // Recentred on the measured pendant and cut from 22.4 units to 13.4. ⚠ The
    // chain's top is now cropped and that is deliberate: the sphere reaches
    // z 116 because a chain loops up around the neck, and holding all of it
    // would give back most of what the tightening bought.
    // ⚠ xHalf came down WITH the span, and must stay under it. The framing
    // fits whichever half-extent is larger, so an x window wider than the z
    // one silently decides the distance and the tightening does nothing.
    inline constexpr View kChestView{ 0.760f, 0.862f, 0.00f, 0.105f };
    // One hand where it hangs, which carries gauntlets, bracers and rings.
    // ⚠ "LOWER THAN IT LOOKS" WAS BACKWARDS AND THIS WINDOW MOVED THE WRONG
    // WAY. Measured 2026-08-10, the hands hang at f 0.45761 to 0.59469, centre
    // 0.52615, and the shipped window's TOP edge was 0.490: below the centre
    // of the thing it was framing, so three quarters of the hand sat above the
    // frame and the rest jammed into a corner. The x offset is solved through
    // the projection rather than read off the mesh, because the camera is
    // yawed and the window keeps the body's whole depth, so centring in world
    // X is not centring on the card.
    //
    // ⚠ THE TOP THEN WENT UP AGAIN FOR THE CUFFS, and deliberately not as far
    // as it could. Gauntlets span f 0.448 to 0.702 where a bare hand stops at
    // 0.595, so a window that contained every cuff would run to 0.700 and
    // shrink the hand itself by a quarter; the field asked for "a little
    // higher" on a view it had just called much better. This keeps the hand
    // large and buys most of the cuff.
    inline constexpr View kHandView{ 0.430f, 0.660f, 0.400f, 0.150f };
    // A ring wants the FINGERS, not the hand (field 2026-08-10): on the hand
    // window a band is a few pixels of metal. The hand hangs fingers-DOWN, so
    // the fingers are the bottom of it, and this window sits on the lower half
    // and is roughly a third of the hand window's span.
    // ⚠ SAME CAVEAT AS THE BEARD: the span is the ask and the centre is a
    // guess. The hands were measured, a ring on a finger was not, so which
    // finger the band lands on is a field knob. x rides the hand's own solved
    // offset, since the ring is on the hand the hand window already frames.
    inline constexpr View kRingView{ 0.450f, 0.530f, 0.400f, 0.052f };
    // Feet and calves, closer than the whole leg.
    // ⚠ THE FLOOR DROPPED FOR HEELS (field 2026-08-10). A high heel is
    // authored with the foot lifted and its sole reaching below the ground the
    // flat-footed body stands on, so the window's bottom edge was clipping the
    // heel off the very boots the feet rules exist for. Lowered rather than
    // rescaled: the field called this view much better and only wanted the
    // bottom to show.
    inline constexpr View kFeetView{ -0.050f, 0.215f, 0.00f, 0.155f };

    [[nodiscard]] inline View ViewFor(Crop a_crop) {
        switch (a_crop) {
            case Crop::kHead:       return kHeadView;
            case Crop::kEye:        return kEyeView;
            case Crop::kChest:      return kChestView;
            case Crop::kHand:       return kHandView;
            case Crop::kFeet:       return kFeetView;
            case Crop::kFacialHair: return kFacialHairView;
            case Crop::kRing:       return kRingView;
            default:                return View{ 0.0f, 1.0f, 0.0f, 0.5f };
        }
    }

    inline constexpr std::uint32_t kBodySlotBit = 1u << 2;  // biped slot 32
    inline constexpr std::uint32_t kRingSlotBit = 1u << 6;  // biped slot 36
    inline constexpr std::uint32_t kHandGroup =
        PreviewFilter::kHandsSlotBit | (1u << 4) | kRingSlotBit;  // 33, 34, 36
    inline constexpr std::uint32_t kFeetGroup =
        PreviewFilter::kFeetSlotBit | (1u << 8);  // 37, 38
    inline constexpr std::uint32_t kChestGroup = 1u << 5;  // 35, the amulet
    inline constexpr std::uint32_t kHeadGroup =
        PreviewFilter::kHeadSlotBit | (1u << 1) | (1u << 12) | (1u << 13);  // 30,31,42,43

    // A head part carries no slot mask of its own, so it names itself.
    [[nodiscard]] inline Crop CropFor(bool a_figure, std::uint32_t a_slotMask,
                                      HeadScene a_head = HeadScene::kNone) {
        if (!a_figure) {
            return Crop::kWhole;
        }
        if (a_head == HeadScene::kEyes) {
            return Crop::kEye;
        }
        // ⚠ A BEARD IS NOT HAIR FOR FRAMING, whatever it is for everything
        // else. They shared the head window until the field asked for a beard
        // 70% closer (2026-08-10), and a hair needs the whole skull while a
        // beard needs the jaw.
        if (a_head == HeadScene::kFacialHair) {
            return Crop::kFacialHair;
        }
        if (a_head == HeadScene::kHair) {
            return Crop::kHead;
        }
        // ⚠ The body slot means the whole figure: a cuirass windowed to its own
        // height loses the head and legs that make it read as worn.
        if (a_slotMask == 0 || (a_slotMask & kBodySlotBit) != 0) {
            return Crop::kWhole;
        }
        // ⚠ THE RING COMES OUT OF THE HAND GROUP FIRST, and only when it is
        // alone. A band framed with the whole hand is a few pixels of metal
        // (field 2026-08-10), so it earns the fingers instead. Anything
        // claiming the ring slot AND a gauntlet slot is a gauntlet.
        if (a_slotMask == kRingSlotBit) {
            return Crop::kRing;
        }
        if ((a_slotMask & ~kHandGroup) == 0) {
            return Crop::kHand;
        }
        if ((a_slotMask & ~kFeetGroup) == 0) {
            return Crop::kFeet;
        }
        if ((a_slotMask & ~kChestGroup) == 0) {
            return Crop::kChest;
        }
        if ((a_slotMask & ~kHeadGroup) == 0) {
            return Crop::kHead;
        }
        return Crop::kWhole;
    }

    // The fallback for a slot no group above claims: where the item actually
    // sits on the body picks the window, and then the window is the standard
    // one. ⚠ This is the ONLY place the item's own box is consulted, and it
    // decides a CHOICE rather than a framing, so every card that lands in a
    // view still shares that view exactly.
    [[nodiscard]] inline Crop CropByHeight(float a_bodyMin, float a_bodyMax,
                                           float a_itemMin, float a_itemMax) {
        const float height = a_bodyMax - a_bodyMin;
        if (height <= 1e-4f) {
            return Crop::kWhole;
        }
        const float centre = ((a_itemMin + a_itemMax) * 0.5f - a_bodyMin) / height;
        const float span   = (a_itemMax - a_itemMin) / height;
        if (span > 0.45f) {
            return Crop::kWhole;  // it covers the figure, so it IS the figure
        }
        if (centre > 0.80f) {
            return Crop::kHead;
        }
        if (centre > 0.62f) {
            return Crop::kChest;
        }
        if (centre < 0.22f) {
            return Crop::kFeet;
        }
        return Crop::kWhole;
    }

    // The window on the body, in world units. Depth stays whole: an item
    // stands proud of the body and nothing is gained by clipping it in Y.
    // ⚠⚠ THE REFERENCE BODY MUST NOT CHANGE WHEN A PART IS DROPPED, and that
    // is what broke slot 30 (field 2026-08-10). A full-face helm takes the
    // head off the mannequin, which takes the top off the mannequin's box,
    // which slides every fraction down the figure: the same window that framed
    // a hood's head framed a mask's chest. So the reference is built from the
    // BODY, whose parts are unconditional, and its top is extrapolated to
    // where the crown would be. Same box with a head or without one.
    //
    // ⚠⚠ AND THE FLOOR GOES THE SAME WAY, which the first cut of this missed
    // and the field caught. The head is not the only part that can be dropped:
    // footwear takes the FEET, and the feet are what the box stands on, so a
    // boots card measured its floor at the hips and slid every fraction up the
    // figure. Three knee-high boots came back cut off at the bottom (field
    // 2026-08-10), and they were precisely the three records whose slot mask
    // made the old drop rule fire. Widening that rule to all footwear, which
    // is the other half of the same report, would have done it to every boot
    // in the catalog.
    //
    // So the reference is the BODY's box alone, and both ends are
    // extrapolated. The body is the one part nothing removes: hands and feet
    // are dropped by slot, the head by a full-face helm. Hands never mattered
    // anyway, sitting inside the body's own z range.
    //
    // kBodyHeight is how much of a full standing figure the body mesh spans,
    // and kBodyFloor is how far below the body's own bottom the ground is,
    // both as fractions of the full figure. Measured on this rig 2026-08-10:
    // feet 0.0424..11.4121, body 11.1875..114.4760, head 110.6280..131.4800,
    // so the figure is 131.4376 tall, the body spans 103.2885 of it and
    // stands 11.1451 up from the floor.
    inline constexpr float kBodyHeight = 0.785836f;
    inline constexpr float kBodyFloor  = 0.084794f;

    // ⚠ kNeckFraction is a field knob: how far up a full standing figure the
    // reference parts themselves reach, none of them having a head on it.
    //
    // ⚠⚠ IT IS MEASURED AND IT IS RIGHT, SO DO NOT RE-OPEN IT. The handoff
    // named this constant as the likely cause of "head view too low" and "hand
    // view worse" and it was neither. Measured on this rig's own meshes
    // 2026-08-10 it is 0.870631 against the 0.87 shipped, which moves a window
    // by a tenth of a unit on a 131-unit figure. Both reports were the view
    // literals above. The spread across bodies bounds it at 0.8656 (female
    // weight 0) to 0.8706 (female weight 1) to 0.8691 (male), so no single
    // value is exact for every character and this one is inside the spread.
    //
    // ⚠ THE REFERENCE IS BODY UNION HANDS UNION FEET, not the body alone:
    // PreviewRenderer takes every mannequin part except the head. Its FLOOR
    // comes from the feet at z 0.0424 rather than from the body at z 11.1875,
    // which is 11 units of arithmetic anyone measuring the body by itself
    // would get wrong.
    inline constexpr float kNeckFraction = 0.87f;

    // a_bodyMin/Max is the BODY mesh's box: no head, no feet, and hands make
    // no difference. Both ends are extrapolated to the full standing figure,
    // so the answer is the same box however many parts the scene dropped.
    [[nodiscard]] inline std::pair<std::array<float, 3>, std::array<float, 3>> ReferenceBody(
        const std::array<float, 3>& a_bodyMin, const std::array<float, 3>& a_bodyMax) {
        auto        mn     = a_bodyMin;
        auto        mx     = a_bodyMax;
        const float height = a_bodyMax[2] - a_bodyMin[2];
        if (height > 1e-4f) {
            const float full = height / kBodyHeight;
            mn[2] = a_bodyMin[2] - full * kBodyFloor;
            mx[2] = mn[2] + full;
        }
        return { mn, mx };
    }

    // One measured box out of the scene, and whether the scene had one. ⚠ THE
    // HEAD IS NOT ALWAYS THERE: a full-face helm takes it off the mannequin
    // (biped slot 30, the engine's own rule), so a window that wants the head
    // needs an answer for its absence, and that answer is the figure fraction
    // this always used. The item is missing on a mannequin-only scene.
    struct Box {
        std::array<float, 3> min{};
        std::array<float, 3> max{};
        bool                 valid{ false };
    };

    // One window's arithmetic against one box, so a head-anchored view and a
    // figure-anchored one cannot drift apart in how they read a View.
    [[nodiscard]] inline std::pair<std::array<float, 3>, std::array<float, 3>> Fractional(
        const std::array<float, 3>& a_min, const std::array<float, 3>& a_max,
        const View& a_view) {
        const float height = a_max[2] - a_min[2];
        const float width  = a_max[0] - a_min[0];
        if (height <= 1e-4f || width <= 1e-4f) {
            return { a_min, a_max };  // a degenerate body frames itself
        }
        const View& v       = a_view;
        const float xCentre = (a_min[0] + a_max[0]) * 0.5f + width * v.xOff;
        auto        mn      = a_min;
        auto        mx      = a_max;
        mn[2] = a_min[2] + height * v.zLo;
        mx[2] = a_min[2] + height * v.zHi;
        mn[0] = xCentre - width * v.xHalf;
        mx[0] = xCentre + width * v.xHalf;
        // ⚠ THE FRONT FACE ONLY, and the back is deliberately left alone. The
        // camera stands off the window's maximum y, so pulling the back in
        // moves the centre and the distance by the same amount and achieves
        // nothing; pulling the FRONT in is the only thing that gets a tight
        // close-up nearer its subject. A view that keeps the whole depth (the
        // default) leaves this axis exactly as it was.
        if (v.yHi < 1.0f) {
            const float depth = a_max[1] - a_min[1];
            if (depth > 1e-4f) {
                mx[1] = a_min[1] + depth * v.yHi;
            }
        }
        return { mn, mx };
    }

    // Is this item plausibly the eye its card says it is?
    [[nodiscard]] inline bool EyeIsFramable(const Box& a_head, const Box& a_item) {
        if (!a_item.valid) {
            return false;
        }
        const float eye = a_item.max[2] - a_item.min[2];
        if (eye <= 1e-4f || (a_item.max[0] - a_item.min[0]) <= 1e-4f) {
            return false;
        }
        if (!a_head.valid) {
            return true;  // no head to sanity-check against, and the eye is real
        }
        const float head = a_head.max[2] - a_head.min[2];
        return head <= 1e-4f || eye <= head * kEyeMaxShare;
    }

    // The eye's window: hung from the item's top, sized off the head, offset
    // sideways onto one eye. Three measurements and no tuned constant, so the
    // same card comes out of any race, any body and any eye mesh.
    [[nodiscard]] inline std::pair<std::array<float, 3>, std::array<float, 3>> EyeWindow(
        const Box& a_head, const Box& a_item) {
        // How tall the eye itself is. The head answers when it can, because an
        // eye mesh's own box carries however much globe the author modelled
        // behind the lid. Without a head, the box is all there is.
        const float boxed = a_item.max[2] - a_item.min[2];
        const float eye   = a_head.valid
                                ? (a_head.max[2] - a_head.min[2]) * kEyeHeightOfHead
                                : boxed;
        const float height  = eye / kEyeFill;
        const float zHalf   = height * 0.5f;
        // ⚠ THE BOX'S CENTRE IS THE ANCHOR AND ITS TOP IS NOT. On a globe
        // the centre is the pupil axis (measured: both 123.307 on the UBE mesh)
        // and the top is the crown behind the lid; on an aperture patch the
        // centre is the middle of what shows. Only the box's HEIGHT lies.
        const float zCentre = (a_item.min[2] + a_item.max[2]) * 0.5f;
        const float midline = (a_item.min[0] + a_item.max[0]) * 0.5f;
        const float pair    = (a_item.max[0] - a_item.min[0]) * 0.5f;
        const float xCentre = midline + pair * kEyeSideCentre;
        const float xHalf   = zHalf * kEyeSideHalf;
        auto        mn      = a_item.min;
        auto        mx      = a_item.max;
        mn[2] = zCentre - zHalf;
        mx[2] = zCentre + zHalf;
        mn[0] = xCentre - xHalf;
        mx[0] = xCentre + xHalf;
        // The back of the box is the head's when there is one: it costs nothing
        // (the camera stands off the FRONT face) and it keeps the box honest
        // about the scene's depth.
        mn[1] = a_head.valid ? (std::min)(a_head.min[1], a_item.min[1]) : a_item.min[1];
        mx[1] = a_item.max[1] + height * kEyeClearance;
        return { mn, mx };
    }

    // ⚠ THE BOXES ARE REQUIRED ARGUMENTS RATHER THAN DEFAULTED ONES. A
    // trailing default on this path has cost a whole feature once already
    // (PreviewCache's morph span, which compiled green and rendered the raw
    // reference mesh for every body card), so a caller with no head or no item
    // has to say so with an empty Box.
    [[nodiscard]] inline std::pair<std::array<float, 3>, std::array<float, 3>> WindowFor(
        const std::array<float, 3>& a_mannMin, const std::array<float, 3>& a_mannMax,
        Crop a_crop, const Box& a_head, const Box& a_item) {
        if (a_crop == Crop::kWhole) {
            return { a_mannMin, a_mannMax };
        }
        // ⚠ THE EYE ALONE, and the other windows stay on the figure on
        // purpose. The same anchor error is in all of them, but the eye's
        // window is 2.4 units tall where the head's is 31 and the beard's is
        // 18, so a 1.3-unit spread is half the eye's frame and four percent of
        // theirs. They are worth measuring; they are not worth a change nobody
        // can see.
        //
        // Three anchors, in the order of how much they know: the eye itself,
        // then the head that carries it, then the figure.
        if (a_crop == Crop::kEye) {
            if (EyeIsFramable(a_head, a_item)) {
                return EyeWindow(a_head, a_item);
            }
            if (a_head.valid) {
                return Fractional(a_head.min, a_head.max, kEyeHeadView);
            }
        }
        return Fractional(a_mannMin, a_mannMax, ViewFor(a_crop));
    }

    [[nodiscard]] inline Frame FrameFor(const std::array<float, 3>& a_min,
                                        const std::array<float, 3>& a_max,
                                        Pose a_pose = Pose::kDiagonal) {
        const std::array<float, 3> extent{ a_max[0] - a_min[0], a_max[1] - a_min[1],
                                           a_max[2] - a_min[2] };
        Frame out;
        if (a_pose == Pose::kEyes) {
            // Straight at the face, +Z up, no yaw and no tilt: an iris chip.
            // The colour is the whole point of an eye card, and any angle
            // costs iris for sclera.
            out.eyeDir = { 0.0f, 1.0f, 0.0f };
            out.up     = { 0.0f, 0.0f, 1.0f };
        } else if (a_pose == Pose::kUpright) {
            // The skeleton's own frame: +Z is up, +Y is the character's
            // facing (phase 1's fixed studio camera sat at +Y for exactly
            // this reason). The yaw toward +X and the small lift keep depth
            // without costing the upright read; up is +Z orthogonalised
            // against the eye so the corner fit measures true.
            out.eyeDir = Normalize({ 0.38f, 0.92f, 0.20f });
            const Vec3 zUp{ 0.0f, 0.0f, 1.0f };
            out.up = Normalize(Add(zUp, Scale(out.eyeDir, -Dot(zUp, out.eyeDir))));
        } else {
            const auto order      = RankAxes(extent);
            const Vec3 lengthAxis = Axis(order.longest);
            const Vec3 widthAxis  = Axis(order.second);
            const Vec3 thickAxis  = Axis(order.shortest);

            // The eye sits along the thickness (the flat's normal), tilted
            // so the picture keeps depth. A shield looks down the NEGATIVE
            // thickness instead: its face is on that side (the enum's note).
            const float side = a_pose == Pose::kShield ? -1.0f : 1.0f;
            out.eyeDir       = Normalize(Add(Scale(thickAxis, side),
                                             Add(Scale(widthAxis, kTiltWidth),
                                                 Scale(lengthAxis, kTiltLength))));

            // Roll the camera so the long axis runs the diagonal: project it
            // onto the image plane, then set up 45 degrees off it. The minus
            // sign picks lower-left to upper-right; with a plus the blade
            // runs the other way.
            const Vec3 proj =
                Add(lengthAxis, Scale(out.eyeDir, -Dot(lengthAxis, out.eyeDir)));
            const Vec3  d      = Normalize(proj);
            const float kCos45 = 0.70710678f;
            out.up = Normalize(Add(Scale(d, kCos45),
                                   Scale(Cross(out.eyeDir, d), -kCos45)));
        }

        // The camera's actual right, for the corner fit and the light rig.
        // LookAtLH convention: forward = -eyeDir, right = up x forward.
        const Vec3 forward = Scale(out.eyeDir, -1.0f);
        const Vec3 right   = Normalize(Cross(out.up, forward));

        // Stand exactly far enough back that every AABB corner fits inside
        // kFill of the half-angle, on both frame axes. Sphere fits waste the
        // frame on long thin objects, which is every weapon.
        const float fovRad  = kFovDegrees * 3.14159265f / 180.0f;
        const float tanHalf = std::tan(fovRad * 0.5f) * kFill;
        const Vec3  half{ extent[0] * 0.5f, extent[1] * 0.5f, extent[2] * 0.5f };
        float       need     = 1.0f;
        float       maxDepth = 0.0f;
        for (int sx = -1; sx <= 1; sx += 2) {
            for (int sy = -1; sy <= 1; sy += 2) {
                for (int sz = -1; sz <= 1; sz += 2) {
                    const Vec3 corner{ half.x * static_cast<float>(sx),
                                       half.y * static_cast<float>(sy),
                                       half.z * static_cast<float>(sz) };
                    const float depth = Dot(corner, out.eyeDir);
                    maxDepth = (std::max)(maxDepth, std::fabs(depth));
                    const float r = std::fabs(Dot(corner, right));
                    const float u = std::fabs(Dot(corner, out.up));
                    need = (std::max)(need, depth + r / tanHalf);
                    need = (std::max)(need, depth + u / tanHalf);
                }
            }
        }
        out.distance = need;
        out.nearZ    = (std::max)(0.1f, out.distance - maxDepth * 2.0f);
        out.farZ     = out.distance + maxDepth * 2.0f + 80.0f;

        // The old fixed rig, expressed on the camera basis it was designed
        // against: mostly behind the eye, a little above, a little to its
        // left. Riding the basis is what keeps every orientation front-lit.
        out.lightDir = Normalize(Add(Scale(out.eyeDir, 0.88f),
                                     Add(Scale(out.up, 0.30f),
                                         Scale(right, -0.36f))));
        return out;
    }

}  // namespace OS::PreviewFraming
