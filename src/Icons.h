#pragma once

#include <cmath>    // std::abs - the audit compares two measured widths
#include <cstddef>  // std::size_t
#include <cstdint>
#include <string>

// Font Awesome 5 Free Solid glyphs, merged into the editor body font by
// EditorStyle (icons.ttf, SIL OFL). Used for the slot list (symbols instead
// of numbers) and the Columns gear. Every codepoint used anywhere must appear
// in kAll so EditorStyle bakes it into the atlas.
//
// VERIFY new codepoints against the BUNDLED font (dist/icons.ttf =
// fontawesome5-solid-webfont-5.15.4), NOT a generic FA5 cheatsheet: the free
// solid set lacks Pro-only glyphs and places some icons at different
// codepoints (e.g. `vest` is e085 here, not the Pro f8dd - that mismatch
// rendered chest as "?", OS-35). All 28 below audited present (the newest,
// `coins` 0xf51e, verified in the bundled cmap 2026-07-15).
//
// Under FLICK the glyphs render from FUCK's own baked fa-solid atlas (not this
// ttf, which only feeds the dormant ImGuiOverlay fallback), but `coins` is a
// stable free-solid codepoint present in both, so it renders on either path.
namespace OS::Icons {

    inline constexpr std::uint16_t kGear     = 0xf013;  // cog - the Columns config button
    inline constexpr std::uint16_t kMask     = 0xf6fa;  // head
    inline constexpr std::uint16_t kHelmet   = 0xf807;  // hard-hat - hair / helmet
    inline constexpr std::uint16_t kBody     = 0xf553;  // tshirt - body
    inline constexpr std::uint16_t kMitten   = 0xf7b5;  // hands
    inline constexpr std::uint16_t kHand     = 0xf255;  // hand-rock - forearms / arms
    inline constexpr std::uint16_t kGem      = 0xf3a5;  // amulet
    inline constexpr std::uint16_t kRing     = 0xf70b;  // ring
    inline constexpr std::uint16_t kShoe     = 0xf54b;  // shoe-prints - feet
    inline constexpr std::uint16_t kSocks    = 0xf696;  // calves
    inline constexpr std::uint16_t kFeather  = 0xf56b;  // feather-alt - tail
    inline constexpr std::uint16_t kUser     = 0xf007;  // long hair
    inline constexpr std::uint16_t kCrown    = 0xf521;  // circlet
    inline constexpr std::uint16_t kEar      = 0xf2a2;  // assistive-listening - ears
    inline constexpr std::uint16_t kSmile    = 0xf118;  // face / mouth
    inline constexpr std::uint16_t kVest     = 0xe085;  // vest - chest (outer). NB free-solid
                                                        // ships vest at e085; the Pro codepoint
                                                        // f8dd is absent and rendered as "?".
    inline constexpr std::uint16_t kBack     = 0xf6ec;  // hiking - back
    inline constexpr std::uint16_t kSkull    = 0xf54c;  // decapitated head
    inline constexpr std::uint16_t kSkullX   = 0xf714;  // decapitate
    inline constexpr std::uint16_t kMagic    = 0xf0d0;  // FX
    inline constexpr std::uint16_t kCube     = 0xf1b2;  // generic / rare slots
    inline constexpr std::uint16_t kRealGear = 0xf2ea;  // undo-alt - revert a slot to worn gear
    inline constexpr std::uint16_t kDice     = 0xf522;  // Random button
    inline constexpr std::uint16_t kStar     = 0xf005;  // favorite toggle (gold = favorited)
    inline constexpr std::uint16_t kUndo     = 0xf0e2;  // undo edit
    inline constexpr std::uint16_t kRedo     = 0xf01e;  // redo edit
    inline constexpr std::uint16_t kSearch   = 0xf002;  // magnifier - the shared search bar
    inline constexpr std::uint16_t kTimes    = 0xf00d;  // X - clear a slot to equipped gear
    inline constexpr std::uint16_t kTrash    = 0xf1f8;  // trash - delete the whole outfit
    inline constexpr std::uint16_t kCoins    = 0xf51e;  // stack of coins - the Apply gold cost
    inline constexpr std::uint16_t kShield   = 0xf3ed;  // shield-alt - shield armor slot

    // exclamation-triangle - the Rules tab's invalid-rule marker (Task 13).
    // Verified present in the bundled dist/SKSE/Plugins/FittingRoom/icons.ttf
    // cmap (Windows-Unicode-BMP subtable, glyph index 130, not .notdef)
    // before use, per the OS-35 lesson below.
    inline constexpr std::uint16_t kWarning  = 0xf071;

    // --- Weapons accordion (Task 8, weapon+quiver transmog) ----------------
    // FA5 Free Solid has no dedicated icon for most melee weapon classes
    // (sword/dagger/axe/mace/greatsword are Pro-only or simply absent from
    // the free set) - per the OS-35/OS-62 lesson, shipping a guessed
    // codepoint that isn't actually in the bundled font renders as tofu
    // ("?"), so those rows REUSE already-audited glyphs above instead of a
    // new one (see the kWeaponIcon table in EditorUI.cpp for which class
    // reuses which, and why). The five below are NEW codepoints, picked for
    // classes with a strong, well-known free-solid match - UNLIKE the 30
    // above they have NOT been screenshot-verified against the bundled
    // 5.15.4 atlas yet; that is the pending verification pass the design
    // spec calls out (Task 8 §7, [USER-CHECK]: "veto by screenshot"). If one
    // renders as tofu, swap it for a reuse like its melee siblings.
    inline constexpr std::uint16_t kBolt          = 0xf0e7;  // bolt - Bolts (literal pun; very common icon)
    inline constexpr std::uint16_t kBullseye      = 0xf140;  // bullseye - Bow (archery-target pun)
    inline constexpr std::uint16_t kCrosshairs    = 0xf05b;  // crosshairs - Crossbow (precision-aim pun)
    inline constexpr std::uint16_t kLocationArrow = 0xf124;  // location-arrow - Arrows (arrow-shape pun)
    inline constexpr std::uint16_t kHammer        = 0xf6e3;  // hammer - Battleaxe/Warhammer (name match)
    // link - a slot another row's multi-slot piece already covers. Chosen over
    // a lock (which reads "you may not change this", when the truth is "change
    // it on the owning row") and over reusing the slot's own dimmed icon (which
    // read as an empty slot). Verified present in the bundled cmap, not assumed.
    inline constexpr std::uint16_t kLink          = 0xf0c1;

    // --- Dye unlocks -------------------------------------------------------
    // A dye the character has not earned, and a dye whose RULE could not be
    // read. Two glyphs and not one, deliberately: an unparseable rule locks its
    // colour behind a condition no amount of playing can satisfy, so drawing it
    // with the same padlock as an honest gate sends the player off to earn
    // something that does not exist. The fix for that one is in their Unlocks
    // folder and the swatch has to look different enough to ask about.
    //
    // Both verified present in the BUNDLED font (dist/SKSE/Plugins/FittingRoom/
    // icons.ttf) by parsing its cmap, with controls: `coins` and `vest` came
    // back present and the Pro-only `vest-pro` came back absent, so the parser
    // was answering rather than agreeing. Not read off an FA5 cheatsheet, which
    // is what cost OS-35.
    inline constexpr std::uint16_t kLock          = 0xf023;  // lock - a colour not yet earned
    // ⚠ The unparseable-rule glyph is kWarning above, NOT a second constant.
    // This branch added a kWarnTriangle at the same 0xf071 without noticing the
    // one already there, and the merge with feat/auto-rules put both names in
    // kAll at once. One codepoint, one name.

    // ⚠ EVERY CODEPOINT USED ANYWHERE MUST BE IN HERE or EditorStyle never
    // bakes it into the atlas, and a correct codepoint missing from this list
    // renders as tofu exactly the way a wrong one does. That is the second
    // route to the OS-35 bug and it looks identical from in game.

    // grip-vertical - the Rules tab's drag handle for reordering rule priority.
    // Verified present in the bundled dist/SKSE/Plugins/FittingRoom/icons.ttf
    // cmap (Windows-Unicode-BMP subtable, glyph index 728, not .notdef) before
    // use, per the OS-35 lesson above. The same check confirmed kWarning at
    // glyph 130, which matches what that comment already recorded.
    inline constexpr std::uint16_t kGrip          = 0xf58e;

    // chevron-up / chevron-down. Both verified in the bundled cmap by the same
    // pass that checked kGrip: glyph 136 and 137, neither .notdef. Currently
    // UNUSED: they were the Rules tab's move-up/move-down buttons, retired once
    // dragging grew an insertion line and a bottom drop strip. Kept declared and
    // in kAll because the verification is the expensive part and the atlas cost
    // is two glyphs - reach for these rather than re-deriving a new pair.
    inline constexpr std::uint16_t kChevronUp     = 0xf077;
    inline constexpr std::uint16_t kChevronDown   = 0xf078;

    // --- breaking up the generic cube --------------------------------------
    // Eight slot rows shared kCube, so the neck, both miscs, both pelvis rows,
    // both legs and the shoulder were one undifferentiated column of cubes
    // (user 2026-08-05). Three of them have a real free-solid answer and take
    // it here; the other five do not, and kSlots records which and why rather
    // than shipping a worse guess.
    //
    // All three verified in the BUNDLED dist/SKSE/Plugins/FittingRoom/icons.ttf
    // cmap on 2026-08-05 with fontTools, WITH CONTROLS rather than on trust:
    // `cube` (0xf1b2, the glyph these are replacing and therefore known to
    // render today) came back present at glyph 316, and the Pro-only `vest`
    // (0xf8dd) came back ABSENT, so the parser was answering rather than
    // agreeing. Glyph indices 581, 533 and 317, none of them .notdef.
    //
    // ⚠ AND ALL THREE ARE LONG-STABLE CODEPOINTS, which is the part that
    // matters under FLICK. Cmap presence here is necessary and not sufficient,
    // because the atlas that actually renders is FUCK's own baked fa-solid; the
    // hedge is that `cubes` is a FontAwesome 4 era codepoint and `box` and
    // `ribbon` have been at these values since FA5.0 and are unchanged in FA6,
    // so they land whichever family FUCK baked. Same reasoning kCoins shipped
    // on.
    inline constexpr std::uint16_t kRibbon        = 0xf4d6;  // ribbon - neck
    inline constexpr std::uint16_t kBox           = 0xf466;  // box - misc
    inline constexpr std::uint16_t kCubes         = 0xf1b3;  // cubes - misc 2

    // --- the hand-drawn sort header ----------------------------------------
    // caret-up / caret-down, the marker on the sorted column. Solid triangles
    // rather than sort-up/sort-down (0xf0de/0xf0dd), which are a PAIR of
    // triangles with one dimmed and read as a smudge beside a word at row size.
    // Both verified in the bundled cmap on 2026-08-05 by the same pass that
    // checked kRibbon, with the Pro-only vest as the absent control.
    inline constexpr std::uint16_t kCaretUp       = 0xf0d8;
    inline constexpr std::uint16_t kCaretDown     = 0xf0d7;

    // --- Mode rail (OS-130) ------------------------------------------------
    // Three of the four rail entries. Styles reuses kBody above, so it costs no
    // atlas work at all.
    //
    // All three verified present in the BUNDLED dist/SKSE/Plugins/FittingRoom/
    // icons.ttf cmap on 2026-08-03 by parsing it with fontTools, with controls
    // rather than on trust: free-solid `vest` (0xe085) came back present and
    // the Pro-only `vest` (0xf8dd) came back absent, so the parser was
    // answering rather than agreeing. Glyph indices below, none of them
    // .notdef.
    //
    // ⚠ CMAP PRESENCE IS NECESSARY AND NOT SUFFICIENT. Under FLICK these render
    // from FUCK's own baked fa-solid atlas, and this ttf only feeds the dormant
    // ImGuiOverlay fallback. All three are core free-solid icons from the same
    // family as everything already rendering here, but the gate is a screenshot
    // in game. If one comes back as tofu, reuse an already audited glyph the
    // way the melee weapon classes do rather than guessing a second codepoint.
    // --- body preset row ---------------------------------------------------
    // A plain feminine figure, and NOT kBody. kBody is a t-shirt, which is
    // right for the body ARMOUR slot and for chest-under and for the rail's
    // Outfits tile, and wrong for the row that picks an OBody preset: that row
    // sets a body SHAPE, and a garment is the one thing it does not change
    // (user 2026-08-04, "we want a simple feminine body silhouette").
    //
    // Verified in the BUNDLED dist icons.ttf and in two on-disk fa-solid-900
    // copies, with controls rather than on trust: free-solid `vest` (0xe085)
    // came back present and the Pro-only 0xf8dd came back absent, so the parser
    // was answering rather than agreeing. FA5 calls this `female`; FA6 renamed
    // the same codepoint `person-dress`, so it survives either atlas.
    inline constexpr std::uint16_t kFemale        = 0xf182;

    // sliders-h - the Shape rail tile (OS-161).
    //
    // ⚠ THIS WAS `child` (0xf1ae) FOR ONE BUILD AND THE FIELD REJECTED IT. The
    // reasoning was that the rail names SUBJECTS and not tools, so a bare
    // standing figure beat a row of sliders. That reasoning was fine and the
    // result was not: beside kFemale, which is a standing figure in a dress,
    // two of the six tiles were a person-shaped outline and the user could not
    // tell the Shape tile from the Body Studio one (2026-08-07, "it doesn't
    // have a unique icon, it's the same icon as the body studio one"). A rail
    // entry's first job is to be findable, and a subject-matter icon that
    // collides with its neighbour is not doing that job.
    //
    // So the tool icon wins here, and it happens to describe the page exactly:
    // Shape is the one page that is nothing but sliders.
    //
    // Verified in the BUNDLED dist/SKSE/Plugins/FittingRoom/icons.ttf cmap on
    // 2026-08-07 with fontTools at glyph 338, not .notdef, and WITH THE
    // CONTROLS the paragraph above demands: free-solid `vest` (0xe085) came
    // back present at glyph 35 and Pro-only `vest` (0xf8dd) came back absent,
    // so the parser was answering rather than agreeing.
    inline constexpr std::uint16_t kFigure        = 0xf1de;

    inline constexpr std::uint16_t kTint          = 0xf043;  // tint - Dye (cmap glyph 92)
    inline constexpr std::uint16_t kBook          = 0xf02d;  // book - Presets (cmap glyph 72)
    inline constexpr std::uint16_t kFlow          = 0xf542;  // project-diagram - Rules (cmap glyph 652)

    // --- buttons that lost their words -------------------------------------
    // The Rules tab's per-rule Import, and the dye pane's Copy / Paste / Paste
    // to everything. All four verified in the BUNDLED
    // dist/SKSE/Plugins/FittingRoom/icons.ttf cmap on 2026-08-06 with fontTools,
    // WITH CONTROLS rather than on trust: free-solid `vest` (0xe085) came back
    // present at glyph 35 and the Pro-only `vest` (0xf8dd) came back absent, so
    // the parser was answering rather than agreeing. Glyph indices 697, 179,
    // 206 and 704, none of them .notdef.
    //
    // ⚠ CMAP PRESENCE IS NECESSARY AND NOT SUFFICIENT - under FLICK these render
    // from FUCK's own baked fa-solid atlas. `copy` and `paste` are FontAwesome 4
    // era codepoints and are as safe as anything here. `file-import` (FA5.1) and
    // `fill-drip` (FA5.2) are newer, but they are the same vintage as kFlow
    // (project-diagram, FA5.1) and kRibbon (FA5.0), which both render today on
    // this exact path, so the vintage is evidenced rather than hoped for.
    // If one does come back as tofu, take the reuse route the melee weapon
    // classes took: `clone` (0xf24d, FA4.5 era) is verified present at glyph 401
    // and is the fallback for either.
    inline constexpr std::uint16_t kFileImport    = 0xf56f;  // file-import - copy a pack rule to your own
    inline constexpr std::uint16_t kCopy          = 0xf0c5;  // copy - lift this slot's colours
    inline constexpr std::uint16_t kPaste         = 0xf0ea;  // paste - drop them on one slot
    // ⚠ THE NARROWEST OF THE THREE PAINT TOOLS: one drip, one channel. It has
    // been round this row twice before landing here. It was Paste-to-everything
    // on 2026-08-06 and withdrawn because a bucket beside a clipboard copy and
    // paste read as an unrelated third tool; it was the reset-colour button
    // until 2026-08-28, when a second button wearing it made two visible items
    // share an ImGui id. With the clipboard gone from the row it is a paint
    // glyph among paint glyphs, which is the one job it was always right for.
    inline constexpr std::uint16_t kFillDrip      = 0xf576;  // fill-drip
    // ⚠⚠ THE DYE ROW IS A LIFT AND TWO FAMILIES, and these four are what make
    // it read that way. It used to be copy, paste, reset colour, reset piece,
    // clear all, fill piece, all pieces: three ways to put colour on and three
    // ways to take it off, interleaved, with a clipboard pair at the front and
    // TWO buttons wearing kFillDrip. Two visible items with the same glyph are
    // two visible items with the same ImGui id, and Dear ImGui said so on
    // screen (field 2026-08-28).
    //
    // Ordered now as lift, then everything that puts colour on, then everything
    // that takes it off (user 2026-08-28). The lift is an eye-dropper because
    // that is the tool that means "take this colour"; the three that apply are
    // paint tools scaled by reach, one drip to one channel, a poured bucket to
    // one piece, a spray can to everything worn.
    //
    // ⚠ ALL FOUR VERIFIED IN dist/SKSE/Plugins/FittingRoom/icons.ttf BEFORE
    // USE, per the OS-35 rule above: 1001 glyphs, all of 0xf1fb, 0xf575, 0xf5bd
    // and 0xf12d among them, read out of the cmap rather than assumed from the
    // FontAwesome vintage.
    //
    // ⚠ fill AND fill-drip ARE A BUCKET APART, which is the one risk here. They
    // sit side by side meaning "this channel" and "this whole piece", and the
    // tooltips carry the difference. If the field cannot tell them apart, the
    // drip is the one to move: it has the smaller job and the weaker claim on
    // the bucket shape.
    inline constexpr std::uint16_t kEyeDropper    = 0xf1fb;  // eye-dropper - lift the ringed colour
    inline constexpr std::uint16_t kFill          = 0xf575;  // fill - pour it over one whole piece
    inline constexpr std::uint16_t kSprayCan      = 0xf5bd;  // spray-can - over every piece worn
    inline constexpr std::uint16_t kEraser        = 0xf12d;  // eraser - take one channel back off
    // layer-group. Verified present in the bundled
    // dist/SKSE/Plugins/FittingRoom/icons.ttf cmap before use (1001 glyphs,
    // 0xf5fd among them), per the OS-35 rule.
    //
    // ⚠ NO LONGER THE DYE PANE'S "All pieces" BUTTON, which wears kSprayCan
    // from 2026-08-28. It held that job while the row led with a clipboard copy
    // and paste: every other button there acted on ONE piece, so this one had
    // to say "many pieces" rather than "colour", and stacked sheets did that
    // where a paint symbol would have read as a third clipboard tool. The row
    // is a paint family now and the spray can says both halves at once.
    // Kept declared and in kAll: the cmap verification is the expensive part
    // and the atlas cost is one glyph.
    inline constexpr std::uint16_t kLayerGroup    = 0xf5fd;  // layer-group
    // eye - the Appearance eye-type row (OS-161). Verified present in the
    // bundled dist/SKSE/Plugins/FittingRoom/icons.ttf cmap before use, per the
    // OS-35 rule above.
    //
    // ⚠ THERE IS NO BROW GLYPH IN EITHER FA SET, and eye-slash (f070, also
    // present) means "hidden" rather than "brow", so the brow row borrows
    // kSmile. Do not go looking for a better codepoint: the wall is the same one
    // the leg, hip, waist and shoulder rows hit, and the answer there was a
    // picture (see IconImages.h), which is the upgrade path here too.
    inline constexpr std::uint16_t kEye           = 0xf06e;
    // eye-slash - Hide All, beside kEye's Show All (user 2026-08-11: the outfit
    // toolbar goes to symbols). The note above already recorded this codepoint
    // as present in the bundled cmap; verified again against
    // dist/SKSE/Plugins/FittingRoom/icons.ttf before declaring it, per OS-35.
    //
    // ⚠ IT MEANS "HIDDEN", WHICH IS WHY THE BROW ROW COULD NOT HAVE IT and this
    // button can: that row wanted a picture of a brow and got a statement about
    // visibility. Here visibility IS the subject.
    inline constexpr std::uint16_t kEyeSlash      = 0xf070;

    // ---- the 2026-08-12 batch: text buttons becoming symbols ---------------
    //
    // Added on the user's call after the chamfer pass ("generally try to replace
    // text buttons with symbols in places which make sense"). None of the three
    // had an equivalent already in this file, which is why they are new rather
    // than reused: the nearest candidates were kRedo for a rescan and
    // kFileImport for an apply, and both already carry a different meaning in
    // this same editor. One glyph saying two things is worse than one word.
    //
    // ⚠ ALL THREE ARE FA5 FREE SOLID, which is the bake FLICK ships, so they are
    // as likely to be present as everything above them. That is not a promise
    // and the audit below is why it does not have to be: a codepoint the host
    // atlas lacks is measured as tofu once per session and never printed, and
    // the caller falls back to its wording. The risk of a new glyph here is a
    // rig that reads the word instead of the symbol, not a rig that reads "?".
    inline constexpr std::uint16_t kRefresh       = 0xf021;  // sync - Rescan
    inline constexpr std::uint16_t kSave          = 0xf0c7;  // floppy - save a scheme
    inline constexpr std::uint16_t kCheck         = 0xf00c;  // tick - apply a scheme

    // thumbtack - "Set as default", all three of them (user 2026-08-12, "this
    // button should become a symbol actually").
    //
    // ⚠ NOT kCheck, AND THE RESERVATION IS THE REASON. A tick is the obvious
    // first reach and kCheck is sitting right there unused, but it was added
    // three lines up for the saved-schemes accordion's Apply, which is the very
    // next thing on the list. Taking it here would leave that with no glyph and
    // would break this file's own rule: one glyph saying two things is worse
    // than one word.
    //
    // A pin says "this is my usual" without borrowing from anything: kStar is
    // Favorites and sits on the SAME row as two of these three buttons, so it
    // was never available either.
    //
    // MEASURED in the bundled dist/SKSE/Plugins/FittingRoom/icons.ttf cmap
    // 2026-08-12 with fontTools, per the OS-35 rule above: present, advance
    // 0.750em, ink 0.752 x 1.002em.
    inline constexpr std::uint16_t kThumbtack     = 0xf08d;

    // portrait - the Looks rail tile (W2): a person in a frame is a saved
    // look. New rather than reused because every head/person glyph already
    // here carries a different meaning in this same editor (kMask is the head
    // SLOT, kUser is hair, kFemale and kFigure are two rail neighbours), and
    // one glyph saying two things is worse than one word. MEASURED in the
    // bundled dist/SKSE/Plugins/FittingRoom/icons.ttf cmap 2026-08-22 with
    // fontTools, WITH THE CONTROLS the OS-35 rule demands: present (glyph
    // 'portrait'), while Pro-only vest 0xf8dd came back absent and free vest
    // 0xe085 present, so the parser was answering rather than agreeing.
    inline constexpr std::uint16_t kPortrait      = 0xf3e0;

    inline constexpr std::uint16_t kAll[] = {
        kGear, kMask, kHelmet, kBody, kMitten, kHand, kGem, kRing, kShoe, kSocks, kFeather, kUser,
        kCrown, kEar, kSmile, kVest, kBack, kSkull, kSkullX, kMagic, kCube, kRealGear, kDice,
        kStar, kUndo, kRedo, kSearch, kTimes, kTrash, kCoins, kShield,
        kBolt, kBullseye, kCrosshairs, kLocationArrow, kHammer, kLink, kWarning, kGrip,
        kChevronUp, kChevronDown, kLock, kTint, kBook, kFlow, kFemale, kFigure,
        kRibbon, kBox, kCubes, kCaretUp, kCaretDown,
        kFileImport, kCopy, kPaste, kFillDrip, kLayerGroup, kEye, kEyeSlash,
        kEyeDropper, kFill, kSprayCan, kEraser,
        kRefresh, kSave, kCheck, kThumbtack, kPortrait,
    };

    // UTF-8 encode a BMP codepoint (FA glyphs are all 3-byte 0x800-0xFFFF).
    //
    // ⚠ RAW: no fallback, no audit. This is what the audit MEASURES and what
    // Utf8 below returns when the host atlas has the glyph. Everything that
    // draws an icon wants Utf8, not this.
    [[nodiscard]] inline std::string Utf8Raw(std::uint16_t a_cp) {
        std::string s;
        if (a_cp < 0x80) {
            s += static_cast<char>(a_cp);
        } else if (a_cp < 0x800) {
            s += static_cast<char>(0xC0 | (a_cp >> 6));
            s += static_cast<char>(0x80 | (a_cp & 0x3F));
        } else {
            s += static_cast<char>(0xE0 | (a_cp >> 12));
            s += static_cast<char>(0x80 | ((a_cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (a_cp & 0x3F));
        }
        return s;
    }

    // ---- the atlas we do not own ------------------------------------------
    //
    // ⚠⚠ EVERY VERIFICATION ABOVE IS ABOUT THE BUNDLED icons.ttf, AND THAT IS
    // NOT THE FONT THAT RENDERS. Under FLICK these glyphs come out of the
    // fa-solid bake inside FUCK.dll; EditorStyle::InitFonts only ever fed the
    // dormant ImGuiOverlay path. That atlas belongs to another mod, ships with
    // its own version, and can therefore be missing codepoints this build has
    // checked into three cmaps - which is exactly what a second rig reported:
    // icons drawn as "?" where they render correctly here (user 2026-08-11).
    //
    // Cmap presence is necessary and not sufficient, the comments above have
    // said so for months, and the gate they name is "a screenshot in game". So
    // the editor takes that screenshot itself, once per session, and stops
    // printing a glyph it can see will come out as tofu.
    //
    // ⚠ THE CONTROL IS THE Pro-ONLY VEST, and it is the same control every
    // cmap pass in this file used. It is absent from FA5 Free Solid and from
    // FA6 Free, so whatever the host baked, this codepoint has no glyph and
    // MUST render as the font's fallback character - which is the "?" the
    // field reported. Measuring it gives the width of tofu on THIS rig,
    // without knowing which font, which version or which fallback char.
    inline constexpr std::uint16_t kAbsentControl = 0xf8dd;  // vest (Pro) - never present

    inline constexpr std::size_t kAllCount = sizeof(kAll) / sizeof(kAll[0]);

    // Filled by Audit(), read by Utf8. Untouched until then, so a build that
    // never audits behaves exactly as it did before this existed.
    //
    // ⚠ ONE STATE FOR THE WHOLE PLUGIN. inline variables, so the six
    // translation units that include this header share these rather than each
    // getting a private copy that only one of them ever fills.
    inline bool g_glyphAbsent[kAllCount] = {};
    inline bool g_glyphAudited           = false;

    [[nodiscard]] inline int IndexOf(std::uint16_t a_cp) {
        for (std::size_t i = 0; i < kAllCount; ++i) {
            if (kAll[i] == a_cp) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // What to print instead of a glyph the host cannot draw.
    //
    // ⚠ SHORT, AND ASCII. These land in button labels and in slot rows that
    // are already tight, so the job is "legible at a glance in three
    // characters", not "a word". A tooltip is one hover away everywhere one of
    // these appears; what "?" cost the player was any idea which button they
    // were looking at, and two letters buy that back.
    //
    // ⚠ A CODEPOINT WITH NO CASE HERE GETS "*", not tofu. The kAll discipline
    // above already says every codepoint used anywhere has to be listed there;
    // this deliberately does not add a second list that can fall out of step
    // with it, because a missing case here is cosmetic and a missing entry
    // there is invisible.
    [[nodiscard]] inline constexpr const char* Fallback(std::uint16_t a_cp) {
        switch (a_cp) {
            case kGear:          return "Cfg";
            case kMask:          return "Hd";
            case kHelmet:        return "Hlm";
            case kBody:          return "Bdy";
            case kMitten:        return "Hnd";
            case kHand:          return "Arm";
            case kGem:           return "Amu";
            case kRing:          return "Rng";
            case kShoe:          return "Ft";
            case kSocks:         return "Clv";
            case kFeather:       return "Tal";
            case kUser:          return "Hai";
            case kCrown:         return "Crc";
            case kEar:           return "Ear";
            case kSmile:         return "Fce";
            case kVest:          return "Cst";
            case kBack:          return "Bck";
            case kSkull:         return "Skl";
            case kSkullX:        return "Sk2";
            case kMagic:         return "FX";
            case kCube:          return "Slt";
            case kRealGear:      return "Rst";
            case kDice:          return "Rnd";
            case kStar:          return "*";
            case kUndo:          return "<-";
            case kRedo:          return "->";
            case kSearch:        return "Fnd";
            case kTimes:         return "x";
            case kTrash:         return "Del";
            case kCoins:         return "G";
            case kShield:        return "Shd";
            case kBolt:          return "Blt";
            case kBullseye:      return "Bow";
            case kCrosshairs:    return "Xbw";
            case kLocationArrow: return "Arw";
            case kHammer:        return "Axe";
            case kLink:          return "=";
            case kWarning:       return "!";
            case kGrip:          return ":::";
            case kChevronUp:     return "^";
            case kChevronDown:   return "v";
            case kLock:          return "[]";
            case kTint:          return "Dye";
            case kBook:          return "Set";
            case kFlow:          return "Rul";
            case kFemale:        return "Fig";
            case kFigure:        return "Shp";
            case kRibbon:        return "Nck";
            case kBox:           return "Msc";
            case kCubes:         return "Ms2";
            case kCaretUp:       return "^";
            case kCaretDown:     return "v";
            case kFileImport:    return "Imp";
            case kCopy:          return "Cpy";
            case kPaste:         return "Pst";
            case kEyeDropper:    return "Lift";
            case kFillDrip:      return "Col";
            case kFill:          return "Pce";
            case kSprayCan:      return "All";
            case kEraser:        return "Clr";
            case kLayerGroup:    return "Grp";
            case kEye:           return "See";
            case kEyeSlash:      return "Hid";
            // ⚠ THESE THREE REPLACED WHOLE WORDS ON THEIR BUTTONS, so their
            // fallbacks are the only thing a rig without the glyph has left to
            // read. Kept as close to the original wording as the width allows
            // rather than abbreviated to a code: "Rescan" losing its glyph
            // should still say what it does.
            case kRefresh:       return "Scan";
            case kSave:          return "Save";
            case kCheck:         return "Use";
            case kThumbtack:     return "Dflt";
            case kPortrait:      return "Lks";
            default:             return "*";
        }
    }

    // Measure every codepoint against the tofu control and remember which ones
    // the host cannot draw. a_widthOf measures one UTF-8 string in the font
    // that is actually current, which is why the caller supplies it: this
    // header names no UI type and the measurement has to happen inside a frame.
    //
    // Returns how many came back absent. Idempotent - the first call wins, so
    // it is safe to call every frame from a draw.
    //
    // ⚠ AN EXACT WIDTH MATCH IS THE TEST, and the reason it is trustworthy is
    // that a missing glyph is not merely similar to the fallback character, it
    // IS the fallback character: the same atlas entry, so the same advance to
    // the last bit. A present glyph that happens to share that advance is the
    // only false positive available, and it costs three letters rather than a
    // wrong picture.
    template <class WidthOf>
    inline std::size_t Audit(WidthOf&& a_widthOf) {
        if (g_glyphAudited) {
            return 0;
        }
        const float tofu = a_widthOf(Utf8Raw(kAbsentControl));
        // ⚠ A ZERO-WIDTH CONTROL MEANS THE MEASUREMENT ITSELF IS NOT ANSWERING
        // (no frame, no font, a host that returns nothing), and in that state
        // every codepoint would match it and the whole editor would fall to
        // text. Refuse instead, and leave the audit owed for a later frame.
        if (!(tofu > 0.0f)) {
            return 0;
        }
        std::size_t absent = 0;
        for (std::size_t i = 0; i < kAllCount; ++i) {
            const float w   = a_widthOf(Utf8Raw(kAll[i]));
            g_glyphAbsent[i] = w > 0.0f && std::abs(w - tofu) < 0.01f;
            absent += g_glyphAbsent[i] ? 1u : 0u;
        }
        g_glyphAudited = true;
        return absent;
    }

    // UTF-8 for a codepoint, or its stand-in when the host atlas has no glyph
    // for it. THE funnel: every icon in the editor goes through here, so one
    // audit fixes every call site at once and no site has to know that the
    // atlas is somebody else's.
    [[nodiscard]] inline std::string Utf8(std::uint16_t a_cp) {
        if (g_glyphAudited) {
            const int idx = IndexOf(a_cp);
            if (idx >= 0 && g_glyphAbsent[idx]) {
                return Fallback(a_cp);
            }
        }
        return Utf8Raw(a_cp);
    }

}  // namespace OS::Icons
