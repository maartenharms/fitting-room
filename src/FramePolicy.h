#pragma once

#include <cstdint>

// Which SHAPE our own chrome draws: the carved corner this mod paints, or the
// plain one the surrounding theme uses.
//
// WHY THIS EXISTS. FLICK's theme format cannot describe a cut or carved corner
// (see ChamferPolicy.h), so this plugin paints its own out of a nine-slice
// texture.
//
// ⚠⚠ THERE IS NO AUTO ANY MORE, AND REMOVING IT IS THE FIELD'S CALL
// (2026-08-28: "now though the theme looks great on carved for vanilla and
// vel'dun ... don't even have the follow theme option, just make it carved and
// plain, carved by default"). Auto existed because the carve looked wrong under
// FLICK's own theme, and what actually looked wrong turned out to have nothing
// to do with the corner: the editor's child windows were stacking that theme's
// translucent ChildBg two to four deep, and the bands it made were read as a
// nine-slice defect. With one ground under the whole editor the same art reads
// correctly under both presets, so the question auto was asked to answer no
// longer has two answers.
//
// ⚠⚠ A PRESET NAME NO LONGER DECIDES ANYTHING, so PresetIsCarved and the
// [UI] sCarvedPresets list it read are gone rather than left unused. Nothing
// opens FUCKs/FUCK/defaultstyle.ini for this any more. If a future shape ever
// needs to know the live preset again, the key is still there to read; what must
// not come back is a third Style that resolves differently in the two plugins.
//
// ⚠ THE WIRE NUMBERS DO NOT MOVE. 1 is carved and 2 is plain exactly as they
// were, so a player who went and found plain keeps it. 0 was auto and now reads
// as carved, which is both where auto landed under Vel'dun and where the default
// now sits for everyone else.
//
// ⚠ AND THE OVERRIDE STAYS A RUNTIME SETTING, never an install choice: an
// installer may write the starting value and must not be the only place it can
// be set.
//
// ⚠⚠ MENU STUDIO CARRIES A COPY OF THIS FILE AND THE TWO ARE THE SAME
// ARITHMETIC ON PURPOSE. They are separate plugins with separate INIs, so there
// is nothing to share at build time, and the one thing that must not drift is
// what a given preset resolves to: a player running both sees one editor and one
// button strip in the same menu, and a disagreement between them would show up
// as two different corners on one screen. Change one, change the other, and the
// tests are the check.
namespace OS::FramePolicy {

    // What the INI holds. The numbers are the wire format, so append only.
    //
    // ⚠ 0 IS ABSENT ON PURPOSE AND MUST NOT BE REUSED. It was auto until
    // 2026-08-28 and is still sitting in every INI written before then, so it
    // has to keep meaning something: StyleFromIni reads it, and everything else
    // it has never seen, as carved.
    enum class Style : std::uint8_t {
        kCarved = 1,
        kPlain  = 2,
    };

    // What the drawing code actually does. Still a different type from Style
    // even now that the two map one to one: Style is what the INI holds and may
    // grow a value the draw sites have never heard of, and Resolve is the one
    // place that has to decide what such a value looks like.
    enum class Shape : std::uint8_t {
        kCarved,
        kPlain,
    };

    [[nodiscard]] inline constexpr Style StyleFromIni(int a_value) {
        switch (a_value) {
            case 2:  return Style::kPlain;
            default: return Style::kCarved;
        }
    }

    [[nodiscard]] inline constexpr int IniFromStyle(Style a_style) {
        return static_cast<int>(a_style);
    }

    // ⚠ NO SECOND ARGUMENT ANY MORE. It carried "is the live preset one of
    // ours", which only auto ever read; a shape that answers from the setting
    // alone cannot disagree between the two plugins over a file neither of them
    // opens now.
    [[nodiscard]] inline constexpr Shape Resolve(Style a_style) {
        return a_style == Style::kPlain ? Shape::kPlain : Shape::kCarved;
    }

    // The radius a plain shape draws with, in final pixels: the theme's number
    // unless the SURFACE carries its own and that one is larger.
    //
    // ⚠⚠ a_default IS A PROPERTY OF THE SURFACE, NOT A SETTING, and it stopped
    // being one on 2026-08-14. It was [UI] fPlainRounding for a day, one radius
    // for the whole plain path, and each value it held was wrong in the field:
    // the theme's own number gave hard rectangles, 8 gave bubbly panels and
    // tab strips, and 8-for-buttons-only was never seen. The reference commit
    // fde7e3f had no such number. It asked the theme for preview cards and slot
    // tiles, used 28% of the edge for rail tiles, and drew the editor frame hard
    // square, so ONE value could never have reproduced it. Callers pass zero to
    // mean "follow the theme", which is the common case and the card's rule.
    //
    // ⚠ THE THEME MAY ONLY ASK FOR MORE, which keeps a surface's number a FLOOR
    // rather than an override. A preset that rounds heavily still gets to look
    // like itself, and no surface can be forced squarer than the theme wants.
    //
    // ⚠ WRITTEN SO A NaN LANDS ON THE DEFAULT rather than propagating. A
    // GetStyleVar that answers garbage must not be able to pick the radius.
    [[nodiscard]] inline constexpr float PlainRadius(float a_themeRounding,
                                                     float a_default) {
        const float base = (a_default > 0.0f) ? a_default : 0.0f;
        return (a_themeRounding > base) ? a_themeRounding : base;
    }

    // ⚠ A RADIUS CANNOT EXCEED HALF THE SHORTER SIDE, the same clamp every
    // rounded-rect renderer applies internally. Spelled here so a caller can
    // predict what the draw call will do to it.
    [[nodiscard]] inline constexpr float ClampRadius(float a_radius, float a_width,
                                                     float a_height) {
        const float shorter = (a_width < a_height) ? a_width : a_height;
        const float limit   = (shorter > 0.0f) ? shorter * 0.5f : 0.0f;
        if (!(a_radius > 0.0f)) {
            return 0.0f;
        }
        return (a_radius > limit) ? limit : a_radius;
    }

}  // namespace OS::FramePolicy
