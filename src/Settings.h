#pragma once
#include "PCH.h"

#include "CostMode.h"
#include "PagePolicy.h"  // Visibility: which pages the player asked for  // OS::CostMode / CostModeFrom, split out so a test can reach them
#include "UiScaleMigration.h"  // OS::UiScale, split out for the same reason
#include "LooksMigration.h"    // OS::LooksOn, the face keys' one-time flip, same split
#include "InstallerSeed.h"     // OS::InstallerSeed, the installer's answers once per install

#include <string>

namespace OS {

    // Runtime settings, persisted to the active build channel's INI.
    struct Settings {
        // ⚠ 0.5 AND 0.8 AS OF 2026-08-14 (user), narrowed from 0.4 and 1.2. The
        // old span had a lot of nothing at each end: below 0.5 the editor's own
        // text stops being readable at 1080p, and above 0.8 the panel is larger
        // than the screen it has to sit in, so both ends were settings that
        // could only make the editor worse.
        //
        // ⚠⚠ THE LOAD CLAMPS TO THIS RANGE, so narrowing it MOVES AN EXISTING
        // INSTALL that sat outside it. Anyone who had chosen 1.0 is now on 0.8
        // and their INI is rewritten to say so the next time the panel saves.
        // That is the intent rather than a side effect, and it is the reason
        // this is a range change and not just a default change.
        static constexpr float kUiScaleMin     = 0.5f;
        static constexpr float kUiScaleMax     = 0.8f;
        // Picture-card side in font-size units. 6 puts four cards on a row
        // of the browser pane (user 2026-08-09, down from the 9 the first
        // rounds ran at); the span is wide enough to double that or nearly
        // fill the pane with one column.
        // ⚠ THE FLOOR WAS ALSO THE DEFAULT, so the smallest card anyone could
        // ask for was the one they already had. The field asked for ESO-sized
        // thumbnails and the slider had nothing left to give (2026-08-10).
        // Small is now genuinely small, and it costs no empty space because
        // the grid fits its cards to the row (PreviewGrid::FittedSide): a
        // smaller target buys more columns rather than a bigger gap.
        // ⚠ 5 AS OF 2026-08-11 EVENING (user), having been 4 that morning and 5
        // before that. This is the FRESH-INSTALL value only: fPreviewCardScale
        // is written to the INI the first time the slider moves, and from then
        // on the INI wins, so an existing install sees nothing change here. See
        // live-ini-overrides-code-defaults.
        //
        // ⚠ IT IS ALSO THE INPUT TO PreviewGrid::NameScaleFor NOW, so moving it
        // moves how large a fresh install draws a card NAME as well: the name
        // is full size at kCardNameRefUnits (6) and scales below it, so 5 draws
        // names at 5/6 rather than the 4/6 this morning's value gave. The two
        // numbers are deliberately not tied together - the reference is about
        // legibility and this one is about density.
        static constexpr float kCardScaleMin     = 1.5f;
        static constexpr float kCardScaleMax     = 14.0f;
        static constexpr float kCardScaleDefault = 5.0f;
        // ⚠ 0.63 AS OF 2026-08-31 (user), having been 0.65 since 08-14, 0.7
        // since 08-08 and 0.8 before that. The editor is a lot of panel and each step down puts
        // less of it in the way. This is the value a FRESH INSTALL starts at;
        // the slider in the settings panel writes fUiScale and overrides it
        // from then on, so an existing install sees nothing change here. See
        // live-ini-overrides-code-defaults.
        //
        // ⚠⚠ AND A FRESH INSTALL IS NO LONGER THE ONLY ONE THAT GETS IT. Save
        // writes fUiScale, so every install has the default of its day pinned
        // in the file and nothing here could reach it. See OS::UiScale and the
        // migration in Settings::Load. ⚠ MOVING THIS NUMBER MEANS ADDING THE
        // OLD ONE TO kHandedOut AND BUMPING kSettingsVersion, or the change
        // reaches fresh installs only.
        static constexpr float kUiScaleDefault = 0.63f;

        static Settings& GetSingleton();

        void Load();  // kDataLoaded
        void Save();  // on change from the settings UI
        // Take the installer's answers once per install, whatever file Load
        // read. Returns true when it changed something, and Load saves then.
        // The reasoning is on OS::InstallerSeed.
        bool SeedFromInstaller();

        bool enabled{ true };    // [General] bEnabled
        bool sceneKick{ true };  // [Advanced] bSceneKick - paused-menu render kick
        // The cost and the Seamstone requirement, split out of the old single
        // bLoreMode toggle (which is migrated into both on load). Independent:
        // gold needs no lore ESP; the Seamstone requirement only bites when the
        // ESP actually provides the stone.
        //
        // ⚠ kCharge DOES NOT NEED THE ESP EITHER, which is worth saying because
        // it reads like it must. The charge is our own number in our own
        // co-save and the meter is drawn in our own window. The ESP supplies
        // the physical Seamstone, and carrying that is what requireSeamstone
        // below is about.
        // ⚠⚠ THIS MEMBER IS ONLY EVER READ WHEN THERE IS NO INI FILE AT ALL, so
        // it has to answer the FOMOD's own default rather than the other one.
        // Settings::Load reads iCostMode with the bLoreMode -> bUseGold chain as
        // its fallback, so a file that is present decides this even when the key
        // is missing; the member survives exactly one case, LoadFile failing.
        //
        // Freeform is the Recommended pick and the preselected answer on page
        // one of the installer, and it writes iCostMode=0. kCharge here meant
        // that a Freeform install whose INI could not be read came up on the
        // Seamstone economy, which is the opposite of what the player picked,
        // and Load then calls Save, so those defaults were written out and the
        // choice was gone for good. Under MO2 that new file lands in Overwrite
        // and shadows the mod's own INI, so reinstalling did not undo it.
        // kFree agrees with what the installer preselects, so the failure is now
        // "no economy" rather than "the economy you declined" (user 2026-08-27).
        //
        // ⚠ THIS CHANGES NOTHING FOR AN EXISTING SAVE. Every install that has an
        // INI, lore or free-form, old or new, takes the value from the file or
        // from the legacy chain and never reaches this line.
        CostMode costMode{ CostMode::kFree };  // [General] iCostMode (was bUseGold)
        bool requireSeamstone{ false }; // [General] bRequireSeamstone - default off: the hotkey opens the editor, the stone is an optional extra way in
        bool collectionOnly{ true };  // [General] bCollectionOnly - browser shows owned looks
        // [General] bCollectionShared - looks found by any character are
        // offered to all of them, through collection-shared.json beside the
        // INI. The gear half of dyeUnlocksShared below, same shape, same
        // add-only file, same eraser.
        //
        // ⚠ OFF BY DEFAULT, and unlike the dye one that is not merely caution.
        // The collection filter is the whole of the lore-friendly gear
        // progression, and sharing it hands a level-one character everything
        // every other character ever owned. That is a real playstyle and it is
        // exactly what was asked for, but it is not a default anyone should
        // arrive at without choosing it.
        //
        // ⚠ IT SHARES LOOKS, NEVER SEEN-MARKS. Inherited looks arrive unseen so
        // there is still something to discover; SharedCollection.h argues it.
        bool collectionShared{ false };
        // [General] bOutfitsShared - a NEW character starts with the outfits in
        // outfits.json instead of an empty list.
        //
        // ⚠⚠ OFF IS THE NEW DEFAULT AND IT IS A BEHAVIOUR CHANGE (user
        // 2026-08-24: "it also seems outfits are not exclusive to a character,
        // my outfits appear on every character, is this intended?"). It was
        // not: outfits.json has no character key, every new game was seeded
        // from it, and the first save baked the inheritance into that save's
        // own 'LIBR' record where it became invisible. The shipped tutorial
        // card has said "outfits belong to this save" the whole time, so the
        // code and the copy disagreed and the copy was the honest one.
        //
        // This is the argument Persistence.h already makes for RULES, applied
        // to the thing rules dress: generic outfit names recur across
        // characters, so importing another character's silently is wrong. Rules
        // refused the fallback from the start; outfits never did.
        //
        // ⚠ THE SEED ONLY. A save that already has a 'LIBR' record is
        // untouched, and a pre-collections save with NO such record still falls
        // back to outfits.json however this reads, because that fallback is the
        // upgrade path and taking it away would empty a library nobody chose to
        // empty. The one thing this gates is what a brand new character starts
        // with.
        bool outfitsShared{ false };
        // [General] bDyeUnlocks - a colour has to be earned before it can be
        // chosen. Read by CanUseDye at the dye pane's swatch click and by
        // nothing else.
        //
        // ⚠ IT GATES USING A COLOUR, NEVER EARNING ONE, and that asymmetry is
        // deliberate. Promotion runs whatever this says. Gating promotion on it
        // would mean a player with unlocks off who took Smithing to 100 and
        // made it Legendary has no record left that they were ever there, so
        // turning the setting on later would withhold, permanently, a colour
        // they earned. The reverse costs nothing: colours earned with the gate
        // off are waiting the moment it goes on.
        bool dyeUnlocks{ true };

        // [General] bDyeUnlocksShared - colours earned on ANY character are
        // offered to all of them, through one file beside this INI (OS-198).
        //
        // ⚠ DEFAULT OFF, unlike dyeUnlocks above, and that is not timidity.
        // Turning it on is a statement about what the player wants the economy
        // to be, and it cannot be fully undone: the file is add-only and
        // nothing clears it, so a player who tries it and dislikes it has to
        // delete a file rather than flip a switch back.
        //
        // ⚠⚠ THE FLAGS TRAVEL AND THE LEDGER NEVER DOES. The Seamstone charge
        // and the deed counters stay in the co-save that paid for them, per
        // OS-172's rule that a thing bought must live with the currency that
        // bought it. SharedDyeUnlocks.h carries the full reasoning and the
        // schema is built so this cannot be widened by accident.
        bool dyeUnlocksShared{ false };

        // [General] bRequireWornForStyles - a style only renders when real gear
        // is equipped under some part of it, instead of dressing the actor out
        // of nothing. Widens the rule the shield has always had (SlotMask.h's
        // StyleRequiresWornItem).
        //
        // ⚠ Not a promise that a bare slot stays bare. The test is against the
        // style's whole COVERAGE and any one covered slot satisfies it, so a
        // multi-slot piece still renders over the bare slots it spans. It has
        // to: coverage belongs to the mesh, and a robe cannot be staged without
        // its sleeves. See CanApplyStyleBit for the full reasoning.
        //
        // Default OFF: turning it on silently stops parts of existing saved
        // outfits from rendering, which is not a thing to do to someone on an
        // update.
        bool requireWornForStyles{ false };
        // ⚠ ON BY DEFAULT, because the behaviour it enables is the one the user
        // asked for and the one without it is the surprise (OS-142). A preset
        // stages wholesale, so slots it does not name fall through to real worn
        // gear and an imported set can arrive wearing a helmet it does not
        // contain. This hides exactly the slots that are both unnamed and
        // really occupied. It exists as a switch at all because "my import
        // stopped showing my circlet" is a fair complaint from someone who
        // liked the fall-through, and it costs one bool.
        bool hideEquippedOnImport{ true };  // [General] bHideEquippedOnImport
        // [General] bStayInEditorAfterApply - keep the editor open when a look
        // is applied, re-staging it in place instead of closing it.
        //
        // ⚠⚠ DEFAULT ON, USER'S CALL 2026-08-27: "make sure we do not get
        // booted from FR when we apply a looks preset". It had been off since
        // r40 reported the body's SMP and CBPC dead after an apply, because this
        // was the change that had arrived with it. r41 paired it properly and
        // cleared it: the physics dies on a look that SWITCHES RACE and is fine
        // on a look applied to the same race, with the editor behaving
        // identically in both. So the off default was only ever caution about a
        // bug this key does not cause, and the caution has now cost more than
        // the risk.
        //
        // ⚠ IT IS THE ONLY GATE ON EITHER PATH. ProfileApply's tail is where
        // the editor is closed or re-staged, and the Looks tab and the RaceMenu
        // preset browser both reach the apply through it, so this one key
        // answers for both. Setting it false restores the old exit.
        bool stayInEditorAfterApply{ true };

        // ⚠⚠ WHAT A LOOK DOES NOT CARRY: LEAVE IT, OR TAKE IT OFF. A look
        // that names no overlays says nothing about them, so its checkbox is
        // never offered (ProfilePlan: a true on a block the file does not carry
        // is a lie) and the previous character's overlays stay on. That is
        // right for someone layering a face onto a character they have built,
        // and wrong for someone importing a whole different person. The user
        // met both on 2026-08-27 and asked for the switch rather than a rule.
        //
        // ⚠ A PREFERENCE AND NOT A CAPABILITY. Which blocks a look carries is
        // the file's business and is decided per look; this is the player
        // saying what an import MEANS to them, so it lives here and persists
        // rather than riding on the apply.
        //
        // ⚠ ONLY WHERE A CLEAR HAS AN HONEST MEANING: overlays, the Shape
        // page's morphs and bone scales, and the skin pack. Makeup has no clear
        // (a tint list is the race's, not ours, and emptying it is not the same
        // as having none), and a body, a weight and an outfit have no empty
        // state to go to. Those are left alone whatever this says.
        bool replaceOnLookApply{ false };

        // ---- onboarding ------------------------------------------------------
        //
        // Which tutorials the player has finished. ⚠ PER INSTALL, WHICH IS WHY
        // THEY ARE HERE AND NOT IN THE CO-SAVE. The co-save is per SAVE and
        // RevertCallback wipes its state before every load, so a flag there
        // would come back every time a different save was loaded. The argument
        // that moved default looks INTO the co-save does not reach these: that
        // was about a purchase costing gold, and a tutorial costs nothing.
        //
        // ⚠ ONE KEY PER TUTORIAL, NOT ONE FOR ALL OF THEM. Three pages can be
        // absent (Presets with nothing installed, Body Studio outside its build,
        // Shape without RaceMenu), so a single flag would mean "finished the
        // tour on a machine that had no RaceMenu" is recorded as "has been
        // taught the Shape page", permanently. TutorialPlan.h carries the whole
        // argument and the scar it comes from.
        //
        // ⚠ AND FALSE IS THE RIGHT DEFAULT for exactly the reason bDyeUnlocks
        // documents above: an absent key keeps the C++ default, and no shipped
        // INI has any of these, so every existing install is offered the
        // tutorial once. Adding an eighth tutorial later needs no migration
        // either, because its key is absent everywhere and so it fires.
        // ⚠ A SETTING, NOT "MARK THEM ALL SEEN". Declining the tour on the very
        // first card could have been written as seven flags going true at once,
        // and that would be wrong in one place that matters: a tutorial added in
        // a later version has no key in anyone's INI, so it defaults to unseen
        // and would fire at somebody who had already said no. A person who
        // turns this off has answered the question for good, not for the
        // tutorials that happened to exist the day they answered.
        // ⚠⚠ A PREFERENCE, NOT A CAPABILITY, and PagePolicy.h's banner has the
        // full argument. Whether a rail tile draws also depends on OBody being
        // installed, on the build channel and on a preset source existing, and
        // those are asked at the rail every frame. This one has to survive a
        // load order the player has not fixed yet, which is why it is stored
        // and why it never learns about requirements.
        //
        // Everything on out of the box: a page that ships off is a page nobody
        // finds, which is the argument the hover-preview checkbox already lost
        // once.
        PagePolicy::Visibility pages{};  // [Pages] bStyles, bPresets, bDye, ...

        bool tutorialsOn{ true };          // [Tutorial] bEnabled

        bool tutorialWelcome{ false };     // [Tutorial] bWelcome
        bool tutorialOutfits{ false };     // [Tutorial] bOutfits
        bool tutorialDye{ false };         // [Tutorial] bDye
        bool tutorialPresets{ false };     // [Tutorial] bPresets
        bool tutorialRules{ false };       // [Tutorial] bRules
        bool tutorialShape{ false };       // [Tutorial] bShape
        bool tutorialBodyStudio{ false };  // [Tutorial] bBodyStudio
        bool tutorialOverlays{ false };    // [Tutorial] bOverlays
        bool tutorialLooks{ false };       // [Tutorial] bLooks
        bool tutorialClosing{ false };     // [Tutorial] bClosing

        bool dumpBiped{ false };  // [Debug] bDumpBiped - biped dump logging
        // Development-channel-only kill switch for the disposable Body Studio
        // ownership proof. Release builds force this false even if an INI is
        // hand-edited, so spike controls can never appear in the normal mod.
        bool bodyStudioProof{ false };  // [Debug] bBodyStudioProof

        // Logs from inside NpcHair's PublishHead, on the game thread, which is
        // what makes it safe to leave in: it measures a pass that already runs
        // rather than adding a traversal of its own. Off unless you are chasing
        // something, because it prints every geometry on the head twice per
        // publish.
        bool hairFaceDump{ false };  // [Debug] bHairFaceDump - dump the face node each publish

        // Dye material census, TEMPORARY - delete with the Census block in
        // OutfitDye.cpp. Strictly read-only: it writes to no material, no
        // property and no geometry, it only reports what the dye walk is
        // already looking at. Task 1 of
        // docs/superpowers/specs/2026-08-02-feature-preserving-tint-spike.md,
        // which is scoped against these numbers rather than against a guess.
        bool dyeCensus{ false };  // [Debug] bDyeCensus - census the shapes the dye walk sees

        // [Debug] bAppearanceWatch - one watchdog over everything that paints
        // the player, sampling once a second and logging only transitions.
        //
        // ⚠ ON BY DEFAULT, unlike every other key in this section, and that is
        // deliberate. It replaces eight probes that were unconditional, it
        // prints nothing at all on a settled save, and the faults it exists for
        // are the ones a player only reports after the session that produced
        // them is over. AppearanceWatch.h argues the rest.
        bool appearanceWatch{ true };  // [Debug] bAppearanceWatch

        // Eye dye spike, TEMPORARY - delete with SpikeEyeDye in OutfitDye.cpp.
        // Step 3 of docs/superpowers/research/dyeing-eyes.md: put a colour on a
        // real iris and look at it, before any of it is persisted.
        //
        // ⚠ THE COLOUR IS A STRING SO IT CAN BE CHANGED WITHOUT A REBUILD.
        // What the spike is actually asking is artistic rather than mechanical:
        // the overlay blend keeps a saturated texture's hue, so a deep blue eye
        // asked to go amber may refuse to move at all. Answering that needs
        // several colours on several eyes in one sitting, which a hardcoded
        // constant makes into a rebuild each time.
        bool        eyeDyeSpike{ false };            // [Debug] bEyeDyeSpike
        // ⚠⚠ THE THIRD TEST COLOUR, AND THE FIRST TWO BOTH COLLIDED WITH A
        // FAILURE. Magenta is what a flat blue placeholder becomes under the
        // tint, so a success and a broken source looked the same. Amber is this
        // character's OWN eye colour, so "the swap did nothing" and "the swap
        // worked" looked the same, which is what a field shot of amber eyes on
        // an undyed head cost. Green collides with neither: it is not the
        // natural colour, not the placeholder, and not the blue-white blowout
        // the defect produces. a-test-colour-must-not-also-be-a-failure-mode.
        //
        // ⚠ Before changing it again, check the colour against all three: the
        // eye's own texture, the placeholder, and whatever the current defect
        // renders as.
        std::string eyeDyeSpikeHex{ "00B000" };      // [Debug] sEyeDyeSpikeHex

        // ⚠ sEyeDyeSplitHex LIVED HERE AND IS GONE, DELETED ON EVIDENCE. The
        // eye's second colour is Outfit::eyeTint2 now, so the spike key was a
        // second author of the same state, and an armed one silently forced a
        // split on every eye and dropped the sclera with it. The field report
        // was "sclera dye doesn't work"; the cause was one INI line.
        // live-ini-overrides-code-defaults, and the lesson is that a spike key
        // has to be deleted the day its feature lands rather than left behind.

        // The longest side a dyed EYE texture may have, in pixels. 0 lets the
        // install's own caps decide, which is what an eye had before this key.
        //
        // ⚠ NOT A NICETY, AND THE NUMBER IS MEASURED. On 2026-08-12 one dyed
        // demon eye was a 4096 twin at 87 MiB and cycling eye colours held 508
        // of a 512 MiB budget, evicting on nearly every build. An eye is a few
        // dozen pixels on screen at conversation distance, so a player changing
        // eye colour would thrash the whole cache on their own while looking at
        // the one surface least able to show the difference.
        //
        // ⚠ 512 IS FIELD-PROVEN ON THIS EXACT TEXTURE rather than chosen. The
        // same run built `preview 512x512 from source 4096x4096 mip 3` of the
        // demon iris and rendered it, which is also the control the capped-read
        // trap needs: a level the source's chain does not carry reads back as
        // zeros in silence, and mip 3 of a 13-level chain is nowhere near that.
        std::uint32_t eyeDyeCapPx{ 512 };            // [Debug] iDyeEyeCapPx

        // ⚠ bEyeDyeRecolour LIVED HERE AND IS GONE, RETIRED BY THE FEATURE THAT
        // REPLACED IT. It chose the eye's curve install-wide; the eye tile's
        // Blend mode row chooses it per outfit now, and Recolour is one of its
        // values. A [Debug] key that still decided what an eye renders would be
        // a second author of that state, which is precisely how sEyeDyeSplitHex
        // ate the sclera for a day.
        //
        // ⚠ THE MEASUREMENT IT CARRIED IS WORTH KEEPING AND IS NOT WHAT IT
        // LOOKS LIKE. "The overlay keeps the source's hue, wrong for an eye:
        // amber on this character's green demon iris comes out #9ABF13, still
        // green" was taken while kOverlay NAMED SOFT LIGHT. It is a reading
        // about kSoftLight and says nothing about the real Overlay, which is
        // the eye's default now. Recolour's own cost stands and was measured
        // the same day: it multiplies the source's LUMINANCE, so it can only
        // darken, and that amber gives #966100 on the green eye and #241700 on
        // a deep red one, which is nearly black.

        // ⚠⚠ THE NEGATIVE CONTROL, AND IT IS THE ONE READING THE PURPLE EYE
        // STILL NEEDS. Run the whole swap with the texture store SKIPPED: the
        // clone, the cache round trip, the SetMaterial intern and the render
        // pass invalidation all still happen, and no tinted texture is built or
        // written. Rung 1 shipped with exactly this control and it is why the
        // lost shine was correctly blamed on the machinery rather than the tint.
        //
        // If the iris still goes purple in this mode then NOTHING about the dye
        // colour is involved and the swap machinery is the cause, which points
        // at Community Shaders holding per-material state our brand new interned
        // material has no entry in. If the iris looks normal, the colour is ours
        // and the texture is where to look.
        //
        // MEASURED and this is why the control is worth a run: purple was
        // identical under magenta and under amber, two colours with nothing in
        // common, so the visible result is already behaving as though it does
        // not depend on the tint at all.
        bool eyeDyeNegativeControl{ false };         // [Debug] bEyeDyeNegativeControl

        // Swap the eye's material WITHOUT asking the engine to rebuild the
        // property's render pass list.
        //
        // ⚠⚠ THE NEGATIVE CONTROL ABOVE ALREADY ELIMINATED THE COLOUR. Thirteen
        // eye swaps ran on 2026-08-13 with no tinted texture built and none
        // written, and the iris still lit up and went purple. What is left is
        // the clone, the cache intern and DoClearRenderPasses, and only the last
        // of those can change how the engine decides to DRAW the eye.
        //
        // The pass list caches the technique the engine picked, not the textures
        // it binds, so a texture change may well not need the rebuild at all.
        // For ARMOUR it is measured as load bearing, which is why this is an
        // opt out on one caller rather than a removal.
        bool eyeDyeSkipRepick{ false };              // [Debug] bEyeDyeSkipRepick

        // Rung 1 of the same spike: tint through a SAME-FEATURE clone instead of
        // swapping the material to a FacegenTint.
        //
        //   0 = off, the shipped FacegenTint swap
        //   1 = rung 1, clone same-feature and write the specular fields
        //   2 = rung 1's NEGATIVE CONTROL: identical machinery, writes nothing
        //   3 = rung 2, tint the property's own emissiveColor, no material
        //   4 = rung 2's NEGATIVE CONTROL: records and invalidates, writes nothing
        //   5 = rung 3 (OS-139), clone same-feature and replace diffuseTexture
        //       with a GPU-tinted copy. THE ONE THAT KEEPS THE SHINE.
        //   6 = rung 3's NEGATIVE CONTROL: clones and invalidates, requests no
        //       texture and stores none
        //
        // ⚠ RUNG 3 IS THE ONLY MODE THAT HOLDS VRAM. One tinted texture per
        // distinct diffuse path per colour, roughly 5.6 MiB for a 1024 and
        // 22 MiB for a 2048, mip chain included, held for the session. There is
        // no eviction yet, which is the open half of the unit.
        //
        // ⚠ EVERY NON-ZERO MODE REPLACES THE SHIPPED DYE, it does not run beside
        // it. So NOTHING is dyed normally while any of these is set, including
        // the write modes if their rung turns out not to render. Rung 1 cost a
        // field session to that: it was read as "no dyes work at all" because
        // that is exactly what it looked like. Put this back to 0 when done.
        //
        // ⚠ MODE 2 IS NOT A COURTESY, IT IS HOW THIS RUNG GETS BELIEVED. The
        // spike's own risk note says rung 1 may look like it works while
        // measuring the wrong thing, because a same-feature clone still goes
        // through SetMaterial, which interns a copy rather than storing the
        // pointer. If mode 2 changes the picture at all then the CLONE is doing
        // something, and any colour seen in mode 1 cannot be attributed to the
        // write. Run 2 before believing 1.
        //
        // Task 2, rung 1 of
        // docs/superpowers/specs/2026-08-02-feature-preserving-tint-spike.md.
        // Temporary: delete with SwapToSameFeatureTint in OutfitDye.cpp when the
        // spike returns its verdict.
        std::uint32_t dyeSpikeRung{ 0 };  // [Debug] iDyeSpikeRung

        // Item 7 probe: apply the FIRST offered part of this discovered
        // head-part slot on the next editor open, and log what happened.
        //
        // The one question that decides whether custom head-part slots become a
        // feature: chargen never writes a type the engine did not name, and
        // RaceMenu reaches them through its own path, so whether
        // ChangeHeadPart plus a head rebuild actually puts horns on screen has
        // never been observed. This answers it for the price of one INI line
        // instead of a save-format bump and a browser pane.
        //
        // 0 means do nothing, which is the shipped value. On this rig the
        // interesting numbers are 32 (horns), 106 (horns) and 110 (Chooey's
        // ears); the editor's own log lists what this load order has.
        //
        // OS-139 finish probe: what a reflective piece's envMapScale does.
        //
        // ⚠ NEGATIVE MEANS LEAVE IT ALONE, and that is the default rather than
        // 1.0, because "the vanilla value" is not a constant. Every nif carries
        // its own, so a hardcoded default would silently restyle every
        // reflective piece in the load order the moment rung 3 was switched on.
        // The swap logs the ORIGINAL beside whatever is written, which is how
        // the field run learns what the real baselines are.
        //
        // ⚠ ONE VALUE FOR EVERY DYED REFLECTIVE SHAPE, which is what makes this
        // a probe rather than a feature. A shipped finish is per piece and
        // per-piece means a new field on DyeChannel, which means a codec bump,
        // and that is a decision with its own hazards
        // (cosave-version-equality-destroys-on-bump). Measure the range first,
        // then decide what the UI should even offer.
        //
        // Rough shape, to be confirmed in the field: 0 is matte, and larger is
        // more mirror-like. Delete with the rest of the spike.
        float dyeEnvScale{ -1.0f };  // [Debug] fDyeEnvScale

        // OS-139: give the REFLECTION the dye's colour, by tinting the envmap
        // mask rather than the cubemap.
        //
        // ⚠ THIS IS THE ANSWER fDyeEnvScale IS NOT. Scale only changes how much
        // of an unchanged gold reflection is added, so the field run found no
        // value that shows the dye AND keeps the metal looking like metal. The
        // mask is a per-texel "how mirror-like is this" map the shader
        // multiplies the cubemap by, so tinting it changes the reflection's hue
        // at full strength. The two are complementary: leave fDyeEnvScale
        // negative and turn this on.
        //
        // ⚠ OFF BY DEFAULT SO IT CAN BE A/B'd against the plain diffuse dye.
        // Whether a coloured reflection reads as "dyed metal" or as "plastic"
        // is a judgement the field makes, not one this comment can.
        bool dyeTintReflection{ false };  // [Debug] bDyeTintReflection

        // How much VRAM OS-139's tinted-texture cache may hold before it starts
        // freeing what nothing is rendering from.
        //
        // ⚠ THIS EXISTS BECAUSE THE FIRST VERSION HAD NO BOUND AND REACHED 6 GB.
        // Measured 2026-08-05: 92 textures resident after a few minutes of
        // picking colours, because a 4096² diffuse costs 87 MiB per colour and
        // every colour a drag passed through was kept for ever. A 24 GB card
        // absorbed it; nothing else would have.
        //
        // ⚠ Only entries the cache ALONE holds are freed, so a texture a garment
        // is currently rendering from is never taken out from under it. A budget
        // smaller than what one dressed character needs therefore does not
        // break anything, it just means the next colour rebuilds.
        std::uint32_t dyeTextureBudgetMiB{ 512 };  // [Debug] iDyeTextureBudgetMiB

        // OS-140: the longest side a PREVIEW build may have, in pixels.
        //
        // ⚠ EVERY REQUEST IS A PREVIEW FIRST, even when nothing is dragging.
        // 512 costs 1.3 MiB against a 4096's 85.3 and one sixty-fourth of the
        // GPU work, which is what makes a colour drag affordable: the 2026-08-05
        // session built 528 textures in an evening because a drag posts an edit
        // per frame and every intermediate colour got a full build.
        //
        // ⚠ CAPPING COSTS NO EXTRA GPU WORK, because a lower source mip is read
        // directly and the chain is already resident. 0 means no cap, which
        // turns the split off and is how the old behaviour is reproduced.
        std::uint32_t dyePreviewCapPx{ 512 };  // [Debug] iDyePreviewCapPx

        // OS-140: the longest side a COMMITTED build may have, in pixels.
        //
        // ⚠ THIS IS THE ONLY LEVER THAT MOVES THE FLOOR, and it defaults to 0,
        // no cap, on purpose. The 882.7 MiB that stayed resident after eviction
        // on 2026-08-05 was COMMITTED colours held by live materials, which the
        // preview split never touches. 2048 is 4x for nothing but sharpness up
        // close, and whether that sharpness is missed is a question for a
        // screenshot rather than for this comment.
        std::uint32_t dyeCommitCapPx{ 0 };  // [Debug] iDyeCommitCapPx

        // OS-140: how long a texture's colour must hold still before its full
        // build is queued, in milliseconds.
        //
        // ⚠ AN IDLE RATHER THAN THE PICKER'S MOUSE RELEASE. An idle here needs
        // no editor plumbing and covers every way a colour can arrive, including
        // a scheme apply, a paste and a save load. The release is more precise
        // and only covers the one control.
        std::uint32_t dyeSettleMs{ 250 };  // [Debug] iDyeSettleMs

        // OS-209: the longest side an overlay bake (the Position sliders on the
        // Overlays page) may have, in pixels. 0 is the source's own size.
        //
        // ⚠ THE BAKE IS UNCOMPRESSED R8G8B8A8 WITH A CHAIN, so 2048 is 21.3 MiB
        // on disk and in VRAM where a BC7 source of the same size is 5.3. The
        // plugin carries no block encoder, and 2048 is what a body overlay is
        // judged at on screen. See OverlayBake.h.
        std::uint32_t overlayBakeCapPx{ 2048 };  // [Debug] iOverlayBakeCapPx

        // OS-209: how much textures\FittingRoom\baked may hold before the
        // oldest bakes are deleted, in MiB. 0 never deletes.
        //
        // ⚠ A SAVE CAN NAME A DELETED FILE, and that layer then shows the
        // engine's placeholder until it is baked again from the page. The budget
        // makes that rare (about fifty 2048 bakes at the default) and the log
        // names every file it removes.
        std::uint32_t overlayBakeCacheMiB{ 1024 };  // [Debug] iOverlayBakeCacheMiB

        // OS-139, the GPU diffuse dye spike. Temporary: delete with
        // DyeGpu.{h,cpp} when the spike returns its verdict.
        //
        //   0 = off
        //   1 = task 1, does a compute dispatch run at all (PASSED 2026-08-05)
        //   2 = task 2, what does tinting one real-sized diffuse cost
        //       (PASSED 2026-08-05: 0.097 ms for a 4096² on a 4090)
        //   3 = task 3, THE MECHANISM. Reach a worn shape's real diffuse, tint
        //       it on the GPU, and put the copy back on a same-feature material
        //       clone. One garment recoloured on screen is the deliverable.
        //
        // ⚠ 1 AND 2 ALLOCATE AND THEN RUN ONCE PER SESSION, and 2 in particular
        // takes 80 MiB for its 4096² pair. Put this back to 0 when the numbers
        // are not wanted or every editor open pays for them again.
        //
        // ⚠ TASK 3 DOES NOT RESTORE, ON PURPOSE. It holds its swap for the rest
        // of the session so the picture stays on screen; survival across a cell
        // change is task 4 and is not tested here. Nothing it does is written
        // to disk, so reloading clears it.
        //
        // ⚠ THE PROBE RUNS ON THE FIRST PRESENT AFTER THE EDITOR IS OPENED
        // ONCE, because that is when the Present hook is installed and the
        // render thread is the only place the immediate context may be touched.
        std::uint32_t dyeGpuTask{ 0 };  // [Debug] iDyeGpuTask

        // Rung 2's blocking question, and ONLY the question.
        //
        // Rung 2 would tint through the property's own emissiveColor, which
        // touches no material at all and so cannot change a feature. The spike
        // refuses to let anything write there until one thing is settled:
        // emissiveColor is a `NiColor*`, not a value, and whose allocation it is
        // and whether two properties share one is unknown. That is the same
        // class of hazard as mutating a material the cache has deduplicated
        // across every actor wearing that armour, one layer down.
        //
        // ⚠ STRICTLY READ-ONLY. It records the POINTER, the multiplier and the
        // kOwnEmit flag per shape and counts how many properties name each
        // allocation. It writes to no property, no material and no colour. A
        // shared pointer here means rung 2 is dead as designed, and finding that
        // out by reading costs nothing while finding it out by writing would
        // recolour somebody else's armour.
        bool dyeEmissiveProbe{ false };  // [Debug] bDyeEmissiveProbe

        // [Debug] iProfileProbeKeyDIK. W1's field harness, successor to phase
        // 0's ChargenProbe on the same sacrificed key: one press captures the
        // player into the profile "F11 Look" (face included), the next press
        // applies it whole. A SPIKE KEY like its predecessor: it dies with
        // the Looks page (W2), the way ChargenProbe died with W1 and
        // iOverlayProbeKeyDIK died at 1.0.0. Default 0 = disarmed; the dev
        // rig binds F11 (87), never F9 (quickload underneath).
        std::uint32_t profileProbeKeyDIK{ 0 };

        // [Debug] bPaintScrape. The PaintScrape wrap of RaceMenu's five
        // RSM_Add*Paints functions, on by default since 2026-08-18. Off is a
        // diagnostic position for OS-231 only: the scrape is Fitting Room's
        // one piece of code that runs inside a menu's own AdvanceMovie tick,
        // and the preset load CTD is a corrupted menu snapshot one advance
        // downstream of whoever scribbled it. One launch with this false and
        // the crash still standing clears the scrape without unticking the
        // DLL.
        bool paintScrape{ true };

        // [Dye] bSeamstoneCharging. Charge the Seamstone from the inventory,
        // the way you recharge an enchanted weapon: a meter on its item card
        // and T Charge opening the vanilla soul gem list.
        //
        // ⚠⚠ IT WAS `[Debug] bItemCardCharge`, OFF, AND THAT COST A USER THEIR
        // SESSION. It started as an OS-140 spike and stayed a debug key after
        // it was finished, so a player who set iCostMode=2 opted into an
        // economy whose only currency they could not obtain: the stone starts
        // at zero, an empty stone blocks styling outright, and the sole way to
        // fill it was a Refill button inside the editor the empty stone was
        // blocking. Reported twice on 2026-08-14, and the first answer was "the
        // setting is off", which is true and is not an answer. A finished
        // feature behind a debug key is a feature nobody has
        // (a-spike-key-must-die-when-its-feature-lands).
        //
        // ⚠ A NEW KEY IN A NEW SECTION, NOT A FLIPPED DEFAULT, and that is the
        // migration bDyeKeepShine documents above. Every install that has ever
        // saved its settings carries an explicit `bItemCardCharge = false`, so
        // changing the old key's default would reach nobody who already plays
        // the mod. Nobody's INI has THIS key, so the default governs, and
        // Save() deletes the retired one so it cannot go on being a second
        // author of the same state.
        //
        // ⚠ IT IS NOT SEPARATELY SWITCHABLE FROM THE RECHARGE HOOKS, on
        // purpose. The card stamp is what makes T Charge appear; with the
        // recharge hooks absent, that prompt eats soul gems and credits
        // nothing, which is a bug this mod shipped once already. Both halves
        // read this one key and ItemCardCharge::Install refuses unless
        // SeamstoneRecharge installed first. See SeamstoneRecharge.h.
        //
        // ⚠ AE ONLY. The hooks are located against the 1.6.1170 call sites;
        // Install refuses on SE rather than guessing, and the editor's Refill
        // button remains the route there.
        bool seamstoneCharging{ true };  // [Dye] bSeamstoneCharging

        // What to claim the Seamstone's card "type" is, which is what decides
        // how the card lays itself out and therefore whether a charge meter is
        // drawn at all.
        //
        // ⚠ THE PER-TYPE CONSTANTS, READ OUT OF THE 1.6.1170 POPULATE FUNCTION
        // (AE 51897) RATHER THAN GUESSED. It switches on the form type and
        // writes the result as "type": Armor 1, Weapon 2, Ammo 2, Misc and
        // Apparatus 3, Book 4 or 6, Ingredient 8, Key 9, SoulGem 12, Scroll
        // from the spell itself. An earlier note here said MISC falls through
        // to 0; it does not, it gets 3.
        //
        // Only the Weapon and Ammo arms write "charge" and "usedCharge" at all,
        // and both write 2, so 2 is the only value that can produce a meter.
        // ⚠ AND IT DRAGS THE DAMAGE ROW WITH IT. The Weapon arm writes "damage"
        // unconditionally, so a stone laid out as a weapon shows a DAMAGE row
        // with whatever was left in the field. There is no type that draws a
        // charge without a damage.
        //
        // ⚠ -1 MEANS LEAVE IT ALONE, so the two can be compared without a
        // rebuild. ⚠ -2 SWEEPS: every time the stone's card is rebuilt it takes
        // the next value from 0 upward and logs it, which turns "try sixteen
        // numbers" from sixteen game restarts into one session of scrolling off
        // the stone and back onto it.
        //
        // The sweep exists because of what the skin's file actually is. Read out
        // of SkyUI_SE.bsa, itemcard.swf is a TIMELINE: WeaponDamageLabel and
        // WeaponDamageValue are objects placed inside the Weapons_Enchanted
        // frame, the same frame that holds WeaponChargeMeter, so no field can
        // separate them. But the file carries other frames that hold a meter and
        // no damage row - Craft_Enchanting_SoulGem is weight, value, a soul
        // level and a meter - and whether a plain type number reaches one of
        // them from the inventory cannot be read out of the file.
        std::int32_t itemCardType{ 2 };  // [Debug] iItemCardType

        // How much of a real enchanted weapon's card to imitate, so the skin's
        // charge meter can be bisected from the INI instead of from a rebuild.
        //
        // ⚠ THE UNKNOWN IS IN THE .swf AND CANNOT BE READ OUT OF THE EXE, which
        // is the whole reason this is a dial rather than a decision. Measured so
        // far: with type 2, charge and usedCharge set, the Seamstone's card
        // grows a DAMAGE row and no meter, while a real enchanted weapon beside
        // it shows a meter AND its effect text. The one field the engine writes
        // for that weapon and not for us is "effects", which is a plain string
        // (the card's own infoText), so it is the first suspect.
        //
        //   0  charge and usedCharge only, which is what shipped and drew
        //      nothing
        //   1  plus "effects", the suspect on its own
        //   2  plus damage, poisoned and damageChange: everything the Weapon arm
        //      of the populate function writes, so a card that still draws no
        //      meter at 2 is a card that never will
        std::int32_t itemCardShape{ 2 };  // [Debug] iItemCardShape

        // [Debug] sDiagnosePlugin - when non-empty, log the full pipeline fate
        // (catalog entry/drop, fit reason, name-stem, cluster membership) of
        // every ARMO whose display name OR source plugin contains this
        // substring. Answers "why is mod X producing no Discovered set?" from
        // one OutfitSlots.log. Blank (default) = off; e.g. sDiagnosePlugin = Abyss.
        std::string diagnosePlugin{};

        std::uint32_t editorKeyDIK{ 0x15 };      // [Input] iEditorKeyDIK - 0x15 = Y (default)
        std::uint32_t editorGamepadButton{ 0 };  // [Input] iEditorGamepadButton
        std::uint32_t nextOutfitKeyDIK{ 0 };     // [Input] iNextOutfitKeyDIK
        // OS-207. ⚠ UNASSIGNED BY DEFAULT, like the one above and unlike the
        // editor key: this one opens a menu the player did not ask for, so it
        // has to be a key they chose.
        std::uint32_t directEntryKeyDIK{ 0 };    // [Input] iDirectEntryKeyDIK

        std::uint32_t goldPerSlot{ 100 };    // [Lore] iGoldPerSlot
        std::uint32_t dyeCost{ 250 };        // [Lore] iDyeCost

        // What one changed dye channel costs on Apply.
        //
        // ⚠ NOT iDyeCost, WHICH IS A DIFFERENT TRANSACTION AND MUST STAY ONE.
        // iDyeCost is the price of UNLOCKING a colour permanently, read by
        // DyePalette as a swatch's default cost. Two older design docs say
        // iDyeCost would be repurposed into the per-channel apply price; the
        // shipped code never did it, and doing it now would silently reprice
        // every swatch in the palette. That is the bUseGold mistake with a
        // different key, so these are their own.
        //
        // ⚠ THE GOLD RATE DEFAULTS TO ZERO AND THE CHARGE RATE DOES NOT, which
        // looks inconsistent and is the whole point. Gold has shipped for
        // months billing slots only, so any non-zero default here would hand
        // every existing gold save a bill it never agreed to. Charge is new, so
        // nobody has an economy for it to disturb.
        std::uint32_t goldPerDye{ 0 };       // [Lore] iGoldPerDye
        std::uint32_t chargePerDye{ 50 };    // [Lore] iChargePerDye

        // One APPEARANCE dimension: a body preset, hair visibility, hair
        // colour, hair style, eyes, or brows. See OS::ChangedLookCount.
        //
        // ⚠ NON-ZERO IN BOTH MODES, WHICH IS THE OPPOSITE OF WHAT iGoldPerDye
        // DID, and the reasoning is the user's call rather than an oversight.
        // The dye rate defaulted to zero in gold so no existing save met a bill
        // it never agreed to. These six dimensions were free by ACCIDENT rather
        // than by design, EditorGate::StillDirty's own comment said so, and the
        // instruction was that essentially everything should cost (user
        // 2026-08-07). A gold player who wants the old behaviour sets this to 0,
        // which is the same escape iGoldPerDye offers in the other direction.
        //
        // Matched to the slot rate so the bill has one shape: restyling a slot,
        // recolouring a piece and changing a hairstyle are all one unit of work
        // to the Seamstone.
        std::uint32_t goldPerLook{ 100 };    // [Lore] iGoldPerLook
        std::uint32_t chargePerLook{ 100 };  // [Lore] iChargePerLook

        // The door fee for the character editor itself (RaceMenu), taken once
        // as it opens.
        //
        // ⚠ A FLAT FEE RATHER THAN A PER-CHANGE ONE, and that is forced rather
        // than chosen: RaceMenu is somebody else's window. Nothing on this side
        // can see what was sculpted in there, so there is no count to multiply.
        // What CAN be priced is the trip, which is why this is a gate rather
        // than a bill.
        //
        // Dearer than one look because a trip in there is unbounded: a player
        // pays this once and may change everything RaceMenu exposes.
        std::uint32_t goldPerLooksMenu{ 250 };    // [Lore] iGoldPerLooksMenu
        std::uint32_t chargePerLooksMenu{ 250 };  // [Lore] iChargePerLooksMenu

        // The two Seamstone cues, as sound-descriptor editor IDs.
        //
        // ⚠ SETTINGS RATHER THAN LITERALS BECAUSE THESE STARTED AS GUESSES, and
        // one of them was wrong. A miss here is SILENT, so the INI keeps a fix
        // to one line rather than another build, and PlayUISound warns once per
        // unknown id so a miss is visible rather than mysterious.
        //
        // ⚠ BOTH ARE NOW VERIFIED AGAINST Skyrim.esm's OWN EDID TABLE, which is
        // what the guessing should have been in the first place. `UIEnchantRecharge`
        // is the sound the vanilla Charge prompt plays when a soul gem goes into
        // a weapon, which is exactly what the Refill row does to the stone.
        // The value shipped here before was `UIEnchantingCharge`, and no such
        // descriptor exists in the game - the row played only its generic
        // UIMenuOK and the fill itself was mute (user 2026-08-08, "refilling
        // soulgem has no sfx"). The scan found 101 UI* ids and nothing close to
        // the old spelling.
        //
        // Empty disables the cue outright.
        std::string chargeSpendSound{ "UIEnchantingItemCreate" };  // [Lore] sChargeSpendSound
        std::string chargeFillSound{ "UIEnchantRecharge" };        // [Lore] sChargeFillSound

        // [Lore] sGoldSpendSound. The coins leaving your purse when a style is
        // paid for in gold.
        //
        // ⚠ THE GOLD ARM WAS THE SILENT ONE. kCharge has announced itself since
        // it shipped and kGold never did, so paying with the Seamstone felt like
        // a transaction and paying with coin felt like nothing happened (user
        // 2026-08-14). Same setting shape as the two above so all three cost
        // cues are configured the same way.
        //
        // ITMGoldDown is a SOUN marker in Skyrim.esm, verified there rather than
        // recalled. ⚠ The SNDR beside it is ITMGoldDownSD; both resolve through
        // BuildSoundDataFromEditorID, and the unsuffixed marker is the spelling
        // UIMenuOK and UIMenuCancel already use in this project.
        std::string goldSpendSound{ "ITMGoldDown" };  // [Lore] sGoldSpendSound

        // Enchanting experience for putting a soul into the Seamstone, per
        // point of charge that actually LANDS. 0 turns it off.
        //
        // ⚠ THE OTHER FILL PATH ALREADY PAYS AND THAT IS THE WHOLE ARGUMENT.
        // SeamstoneRecharge points Skyrim's own Charge prompt at the stone, so
        // filling it from the item menu runs the engine's recharge and earns
        // whatever the engine grants. Our Refill row does the same thing to the
        // same stone with the same gem and earned nothing, so the two routes
        // disagreed about whether feeding the stone teaches you anything (user
        // 2026-08-08). This is not a new economy, it is the one that already
        // exists reaching the button that was skipping it.
        //
        // ⚠ OUR NUMBER, NOT THE ENGINE'S, and it is a setting for exactly that
        // ⚠⚠ WHAT WE PASS IS NOT EXPERIENCE. THE ENGINE MULTIPLIES IT BY 900.
        // AddSkillExperience hands its float to the skill advance worker, which
        // scales it by that skill's own Skill.UseMult out of its AVIF record,
        // and AVEnchanting (00045D:Skyrim.esm) carries UseMult 900. Then the
        // threshold to leave level L is pow(L, fSkillUseCurve) * ImproveMult +
        // ImproveOffset, and Enchanting's are 1 and 170.
        //
        // This shipped at 0.1, which is 100 handed over for a common soul and
        // therefore 90,000 experience once the 900 lands. Going from 20 to 99
        // costs about 265,000, so three common gems did it in three clicks, and
        // the worker drains its pool in a loop, so all of it arrives at once.
        // Reported 2026-08-29. The old comment here said "nothing readable from
        // here says what vanilla pays per point"; the AVIF says, and it is 900.
        //
        // 0.00006 pays about 54 experience for a common soul, which is a tenth
        // of a level at skill 20. A petty soul is 250 and pays 13, a grand is
        // 3000 and pays 162.
        //
        // ⚠ A MOD THAT EDITS AVEnchanting MOVES THIS. The 900 is vanilla's and
        // nothing here reads it back, so a load order that changes UseMult
        // scales every number above with it.
        //
        // ⚠ PER POINT LANDED, NOT PER POINT OF SOUL. A gem is eaten whole, and
        // the row already warns when the stone has no room for the rest of it;
        // paying full experience for the wasted part would make burning grand
        // souls into a nearly full stone the fastest way to train.
        static constexpr float kEnchantingXpDefault = 0.00006f;
        // The most a pre-version-3 file may carry before Load pulls it down to
        // the default. Everything in the old scale is broken by the same 900,
        // not just the value we shipped, so the gate is a ceiling rather than
        // an equality test against 0.1.
        static constexpr float kEnchantingXpLegacyCeiling = 0.001f;
        // ⚠ A GUARD, NOT A STYLE POINT. 0.1 sat here for a release and paid
        // 90,000 for a common soul, because the 900 is invisible at this line.
        // A common soul is 1000 charge, the engine multiplies by 900, and the
        // threshold to leave skill 20 is 514, so this is one level's worth.
        // Anything that trips this is back in the scale that caused the bug.
        static_assert(kEnchantingXpDefault * 1000.0f * 900.0f < 514.0f,
                      "one common soul would train more than a whole level of "
                      "Enchanting at skill 20: the engine multiplies this rate "
                      "by AVEnchanting's UseMult of 900");
        float enchantingXpPerCharge{ kEnchantingXpDefault };  // [Lore] fEnchantingXpPerCharge

        // The charge economy, read only while costMode is kCharge.
        //
        // Deliberately its own rate rather than reusing iGoldPerSlot: the two
        // currencies are earned in completely different ways (one is loot, one
        // is soul gems), so one number cannot be balanced for both, and a
        // player who tunes gold to taste should not have silently retuned the
        // stone.
        //
        // The default matches iGoldPerSlot so the bill has the same SHAPE in
        // both modes, per SeamstoneCharge.h: per changed slot, nothing else.
        std::uint32_t chargePerSlot{ 100 };  // [Lore] iChargePerSlot

        // What the stone holds when full. Also the meter's denominator, which
        // is why SeamstoneCharge::Fraction guards a zero here: this is a
        // hand-editable key and 0 would divide in the draw.
        //
        // 5000 holds a Grand soul (3000) with room to spare, so topping up a
        // part-full stone with the best gem in the game is not routinely a
        // waste. ⚠ LOWERING IT DOES NOT CUT AN EXISTING CHARGE DOWN, by
        // DyeUnlockSet::AddCharge's design, so charge > capacity is an ordinary
        // state and anything reading this has to tolerate it.
        std::uint32_t seamstoneCapacity{ 5000 };  // [Lore] iSeamstoneCapacity
        std::uint32_t slotBlocklist{ 0 };    // [Advanced] uSlotBlocklist
        float         menuFontSize{ 26.0f }; // [UI] fFontSize (px)
        float         uiScale{ kUiScaleDefault };  // [UI] fUiScale - complete editor scale
        // Which shape our own chrome draws: 1 carved, 2 plain. 0 was auto and
        // is read as carved; see FramePolicy.h.
        //
        // ⚠⚠ CARVED IS THE DEFAULT AND AUTO IS GONE, 2026-08-28, the field's
        // call: "now though the theme looks great on carved for vanilla and
        // vel'dun ... don't even have the follow theme option, just make it
        // carved and plain, carved by default". Auto had been the default since
        // 2026-08-27 for exactly one day, and it was there because the carve
        // looked wrong under FLICK's own theme. What looked wrong was the
        // editor's child windows stacking that theme's translucent ChildBg two
        // to four deep; with one ground under the editor the corner reads
        // correctly under both presets.
        //
        // ⚠⚠ THE ART MUST SHIP FOR CARVED TO BE HONEST, and it is the default
        // again, so this matters more than it did. IconImages::Frame and
        // FrameFill load frame.png and frame_fill.png; without them every carved
        // surface silently falls back to the straight bevel, which is a
        // different shape. make_fomod.sh copies dist/SKSE/Plugins/FittingRoom
        // wholesale, so the icons ride along, and that is load bearing.
        //
        // ⚠ CHANGING THIS NUMBER STILL REACHES NOBODY ON ITS OWN, because Save
        // writes iFrameStyle into every file. It needs no migration this time:
        // an install from the carved era holds 1 and stays carved, an install
        // from the one auto day holds 0 and reads as carved, and a 2 is a player
        // who went and found plain. Every road already leads where it should.
        static constexpr int kFrameStyleDefault = 1;
        int           frameStyle{ kFrameStyleDefault };  // [UI] iFrameStyle
        // The stamp Save writes into [General] iSettingsVersion.
        //
        // ⚠⚠ IT NEVER GOES DOWN, WHATEVER LEAVES. This was
        // OS::FrameStyle::kSettingsVersion until the frame-style migration was
        // deleted on 2026-08-28, and dropping back to the UI-scale migration's 1
        // would tell a file it had been through less than it has. Each migration
        // reads the file's number against its own gate; this is the highest of
        // them and Settings.cpp static_asserts that it stays that way.
        //
        // 3: the Seamstone refill experience rate. Save had already pinned the
        // broken 0.1 into every file that had ever loaded, so lowering the code
        // default on its own would have fixed nobody who had played.
        //
        // 4: the three face keys move ON once (1.1.7, user 2026-09-02). The
        // installer had shipped them off since 2026-08-29 and Save pinned that
        // into every file, so the new default alone would have reached only a
        // mod-manager install. OS::LooksOn carries the reasoning.
        static constexpr int kSettingsVersion = 4;
        // The file stamp this migration gates on.
        static constexpr int kEnchantingXpSettingsVersion = 3;
        // The installer answers this file has taken ([General] sInstallerStamp),
        // empty until an installer file has been seen. See InstallerSeed.h.
        std::string installerStamp;
        // ⚠⚠ THERE IS NO fPlainRounding KEY AND ONE SHOULD NOT BE ADDED BACK.
        // It existed from 2026-08-14 morning to that evening as the single
        // radius the plain path drew everything with, and every value it was
        // given was wrong in the field: the theme's number gave hard rectangles,
        // 8 gave bubbly panels, and 8-for-buttons-only was a guess nobody had
        // seen. The reference commit fde7e3f had no such knob. It picked a
        // radius PER SURFACE - the theme's for cards and slot tiles, 28% of the
        // edge for rail tiles, hard square for the frame - and that is what the
        // plain path does again. See ChamferPanel::PlainRadiusScope. A single
        // knob cannot express three rules, which is why it kept needing a new
        // value. Save() deletes the key, so an old one cannot sit in a player's
        // INI looking live.
        bool          hoverPreview{ true };   // [UI] bHoverPreview - preview styles on hover
        // [UI] bHoverPreviewCards - the same thing for the CARD views, and it
        // is a second key rather than a widening of the one above because the
        // two want opposite defaults (user 2026-08-11).
        //
        // ⚠ A ROW AND A CARD ARE NOT THE SAME GESTURE. A list is read top to
        // bottom and the pointer crosses a row on its way somewhere, which is
        // what the 0.18s rest is for. A grid is scanned, so the pointer lands
        // on cards it was never interested in, and each landing that survives
        // the rest costs a head rebuild or a follower's whole face node. The
        // card already carries a picture of the thing, which is the answer the
        // preview was standing in for on a row that carries only a name.
        //
        // Read through EditorUI's HoverPreviewOn, never on its own: which of
        // the two keys applies is decided by whether the current view is a
        // grid, not by which list is on screen.
        bool          hoverPreviewCards{ false };
        bool          advancedSlots{ false }; // [UI] bAdvancedSlots - editor shows ALL slots
        // ⚠ ON AS OF 2026-08-11, up from off (user). The pictures were always
        // there and the report was that weapons "preview nothing": a fresh
        // install browsing swords saw a two-column name table, because the
        // only way to the cards was a checkbox that hides while nothing is
        // selected. A feature nobody can find is not a feature.
        //
        // ⚠ THIS IS THE FRESH-INSTALL VALUE ONLY. bPreviewGrid is written to
        // the INI the first time the checkbox moves, and from then on the INI
        // wins, so an existing install sees nothing change here. See
        // live-ini-overrides-code-defaults.
        bool          previewGrid{ true };       // [UI] bPreviewGrid - weapon browser shows picture cards
        // The style browser's two optional columns and its unfit filter.
        //
        // ⚠ PERSISTED AS OF 2026-08-14 (user), and they were SESSION-LOCAL before,
        // reset to on at every editor open. That reset was a deliberate call in
        // August ("on by default and rarely changed"), and moving the controls
        // into the settings panel is what retires it: a switch that lives beside
        // the other persisted settings and silently forgets itself every open is
        // the worse of the two surprises. Turning a column off now means it
        // stays off.
        bool          browserShowClass{ true };   // [UI] bBrowserShowClass
        bool          browserShowPlugin{ true };  // [UI] bBrowserShowPlugin
        bool          browserHideUnfit{ true };   // [UI] bBrowserHideUnfit
        float         previewCardScale{ kCardScaleDefault };  // [UI] fPreviewCardScale - card side in font-size units
        std::uint32_t previewThumbPx{ 256 };     // [UI] iPreviewThumbPx - thumbnail texture size
        bool          previewDiskCache{ true };  // [UI] bPreviewDiskCache - keep thumbnails between sessions
        std::uint32_t previewCacheMiB{ 512 };    // [UI] iPreviewCacheMiB - on-disk thumbnail budget
        // [UI] bBodyFitFilter - the body pickers offer only presets that suit
        // the body this character actually wears.
        //
        // ⚠ ON BY DEFAULT AND OFFERED AS A WAY OUT, not off by default with a
        // way in. It reads the character's own body mesh and compares it to the
        // mesh each preset builds, and every unknown along that path already
        // fails open, so the list can only ever be narrowed by something it
        // proved. The switch is here for the install that proves something
        // wrong (user 2026-08-08).
        bool          bodyFitFilter{ true };
        // [UI] bSfwBodyCards - body preview cards are built from the body
        // mod's own COVERED slider set where one is installed.
        //
        // ⚠ ON BY DEFAULT (user 2026-08-11, after the field run). It costs a
        // one-off rebuild of the body pane on the first launch that sees it,
        // because the resolved set is part of the card's key, and that was
        // judged worth paying once so the covered picture is what a new
        // install gets without finding a switch.
        //
        // ⚠⚠ IT REACHES THE DISK KEY through BodyMorphData::CardIdentity. A
        // runtime toggle cannot ride the compile-time tag, and nothing else
        // about the card changes when it flips: same preset, same sliders, same
        // weight, same model paths. Without it in the key the pane serves the
        // other setting's photographs and the option looks broken.
        //
        // ⚠⚠ IT COVERS 3BA, CBBE AND HIMBO, AND IT CANNOT COVER UBE. Counted
        // 2026-08-11 over 3843 declared sets: 6 CBBE/3BA sets carry a covering
        // and 11 HIMBO do, while UBE has ZERO, confirmed by declared SHAPES as
        // well as by names. Borrowing another body's covering onto UBE was
        // built and reverted the same day: it fitted badly enough that nude
        // was the better picture, and UBE is a body whose whole purpose is to
        // be nude. A preset on a body that ships no covered build draws
        // exactly as before, which is the intended answer and not a failure.
        bool          sfwBodyCards{ true };
        // ⚠ UNLOCKED BY DEFAULT since the right edge became the only thing it
        // gives up. Locking existed to stop the window being dragged into a
        // mess, and it no longer can be: position and height are pinned in
        // both states, so the worst an unlocked player can do is choose a
        // width and undo it with Reset window size.
        //
        // ⚠ AN EXISTING INI STILL WINS. bLockLayout is written on every save,
        // so anyone who has run this mod before keeps whatever they had and
        // only fresh installs see the new default.
        bool          lockLayout{ false };    // [UI] bLockLayout - lock window size (gear unlocks the right edge)
        // The window's width in pixels once the player has dragged its right
        // edge, or 0 for the computed default.
        //
        // ⚠ WIDTH ONLY, AND THAT IS THE WHOLE FEATURE. Unlocking used to hand
        // over position and both axes, and a full-height panel has no useful
        // vertical size: dragging the bottom edge only lifted the footer off
        // the screen, which is the bug the field reported. The panel is
        // flush to the left edge and the full height of the display, so the
        // right edge is the only one that means anything.
        //
        // ⚠ NOT SAVED PER FRAME. It is written here as the drag happens and
        // committed to the INI when the editor closes.
        float         uiWidth{ 0.0f };        // [UI] fWidth - 0 = the computed default

        // [Scene] scene-framework coexistence (OStim, SexLab and the like):
        // while a scene runs, suspend the transmog override (player shows
        // real/undressed gear) and block the editor. Scenes are recognized by
        // Papyrus SendModEvent NAMES (comma-separated, case-insensitive), plus a
        // stable OutfitSlots-specific pair any mod can fire to integrate.
        //
        // ⚠⚠ SEXLAB'S NAMES ARE MEASURED, NOT READ OFF ITS SCRIPTS. A run of
        // the mod-event logger on 2026-08-26 named every event a session saw.
        // SexLab sends AnimationStarting / AnimationEnding / AnimationEnd bare,
        // with Player-prefixed and _CreatureSummoner-suffixed copies alongside
        // them, and there is NO Hook prefix on any of them: the
        // HookAnimationStarting and HookAnimationEnding taken out of
        // sslThreadModel.pex are names SexLab never sends, and a field session
        // configured with them saw nothing at all.
        //
        // ⛔ StageStart AND PlayerStageStart BELONG IN NEITHER LIST. They fire
        // at every stage boundary and would suspend or resume in the middle of a
        // scene rather than at its edges.
        //
        // ⚠ THE BARE NAMES AND NOT THE PLAYER ONES, WHICH IS A DECISION.
        // AnimationStarting already covers a scene the player is in as well as
        // one they are not, because SexLab sends both; adding
        // PlayerAnimationStarting beside it would suspend twice for one player
        // scene. Reach for the Player variants only if a scene with no player in
        // it turns out to suspend the player's transmog.
        bool        sceneCompat{ true };  // [Scene] bSceneCompat
        // [Scene] sSuspendEvents
        std::string sceneSuspendEvents{
            "ostim_start,AnimationStarting,OutfitSlots_SuspendTransmog"
        };
        // [Scene] sResumeEvents
        std::string sceneResumeEvents{
            "ostim_end,AnimationEnding,AnimationEnd,OutfitSlots_ResumeTransmog"
        };

        // [Physics] How strongly a SHOWN outfit's chest piece takes the breast
        // bounce read off the worn armour. This is the `percentage` argument of
        // CBPC's ApplyBounceInterpolation, which BLENDS the amplitudes in
        // CBPCBounceinterpolationconfig_FittingRoom.txt onto the actor: 100 is
        // that profile winning outright, 0 hands the read back to whatever class
        // the real worn armour has, and the values between are a blend.
        //
        // ⚠⚠ IT IS NOT A BOUNCE AMOUNT AND MUST NOT BE NAMED ONE. The profile
        // mirrors the rig's baseline amplitudes, so turning this UP cannot go
        // past what the naked body already does; it only decides which of the two
        // reads wins. Raising the ceiling means editing the amplitudes in that
        // shipped text file, which is a different control and a different day.
        int cbpcBouncePercent{ 100 };  // [Physics] iBouncePercent

        // [Compat] Screen Archer Menu integration: the registered menu name we
        // treat as "SAM is open" (so the editor hotkey works while posing).
        // Configurable in case a SAM build registers a different name.
        std::string samMenuName{ "ScreenArcherMenu" };  // [Compat] sSamMenuName

        // [Compat] While the editor is open, block ALL input from the game and
        // other mods (Screen Archer Menu's scroll->FOV, Wheeler's wheel) via the
        // input-dispatch hook. Off = the older behavior where input can leak to
        // other mods - a safety valve if the hook ever misbehaves.
        bool blockInputWhileOpen{ true };  // [Compat] bBlockInputWhileOpen

        // [Compat] While the editor is open, dragging with the left button held
        // over the WORLD (not over the editor panels) rotates the camera: the
        // third-person orbit in the inventory context, the free camera in the
        // Screen Archer Menu context. Needs bBlockInputWhileOpen (the modal
        // input path is what sees the drag).
        bool  cameraDragWhileOpen{ true };      // [Compat] bCameraDragWhileOpen
        float cameraDragSensitivity{ 0.005f };  // [Compat] fCameraDragSensitivity (radians per count)

        // [Dye] Dye reflective (kEnvironmentMap) shapes as well as plain
        // (kDefault) ones. On by default because envmap and default shapes
        // coexist INSIDE ONE GARMENT: the player's equipped Iron Armor measured
        // as three shapes, 'IronArmor' envmap plus 'IronSkirt' and
        // 'IronSatchel' default, so leaving envmap out dyes the leather and
        // leaves the metal.
        //
        // The swap itself is proven safe on envmap, 88 of them in one field
        // session with no fault, but whether the REFLECTION survives it has
        // never been observed. This exists so that answer can be acted on
        // without a rebuild.
        // [Dye] bDyeReflective. ⚠ NO LONGER GATES THE PAINTER, it governs BULK.
        //
        // Dyeing a reflective shape costs its shine and that is permanent: the
        // feature-preserving tint spike tested three ways round it and returned
        // a no on all of them. A permanent trade belongs to the PIECE, so a
        // channel the player explicitly clicked is always painted, whatever this
        // says, and the stripe's tooltip states the cost.
        //
        // What this decides is whether paste and apply-a-scheme reach reflective
        // channels (MapColoursToSlotSkipping). ON, the historical value, means
        // they do, which is what shipped before. OFF makes them skip metal, so
        // one bulk action cannot dull everything worn.
        bool dyeReflective{ true };

        // ---- the requip transition (OS-206) ------------------------------

        // [Requip] bRequipFlourish. The whole transition, on or off.
        //
        // ⚠ OFF DOES NOT MEAN "SWAP WITHOUT A FLASH AND KEEP THE ART". It
        // means the swap goes through untouched, exactly as it did before the
        // feature existed. There is no half-on state, because the half that
        // could be left running is the half that writes alpha, and a garment
        // left at alpha 0 reads to the player as FR deleting their gear.
        bool requipFlourish{ true };

        // [Requip] bRequipAura. The borrowed magic effect that washes over the
        // body during the transition. VISUAL ONLY.
        //
        // ⚠⚠ IT HAS NOTHING TO DO WITH SOUND, AND TWO EVENINGS WERE SPENT
        // BELIEVING IT DID. The record was dumped out of Skyrim.esm on
        // 2026-08-15 and neither half carries an audio field: EFSH
        // SoulTrapFXShader is EDID/ICON/ICO2/NAM7/NAM8/NAM9/DATA, ARTO
        // SoulTrapTargetEffects is EDID/OBND/MODL/MODT/DNAM. The noise on an
        // outfit swap is Skyrim's own equip audio and predates this feature
        // entirely.
        //
        // ⚠⚠ THE TRAIL THAT LED THE WRONG WAY, so nobody follows it again.
        // `ShaderReferenceEffect` owns a `soundHandle`, which reads as proof the
        // shader makes a sound. It is not: it says such an effect CAN carry one,
        // and these records do not. Acting on that inference cost two crashes,
        // one writing to the handle at creation and one guarding that write.
        // ⚠ `InstantiateHitShader` also does not return a usable pointer on this
        // runtime; the header says ShaderReferenceEffect*, the call returned 1.
        bool requipAura{ true };

        // [Requip] bRequipSoundOn and sRequipSoundId. A magic sound of our own,
        // over the top of the swap.
        //
        // ⚠ ADDING RATHER THAN REPLACING, because the sound underneath belongs
        // to the engine's own equip handling and is not ours to take away. A
        // conjuration cue over armour clatter reads as magic; the clatter alone
        // reads as rummaging in a chest.
        //
        // ⚠⚠ TWO THINGS DECIDE WHETHER AN ID IS USABLE, AND NEITHER IS ITS NAME.
        // Both were learned by shipping a bad pick, twice.
        //
        // LENGTH. The flourish is 0.48 s. MAGConjureBoundWeaponAppear was picked
        // because its editor id described gear appearing; its wav is 155 KB,
        // about 1.8 s, and it was still playing after the garments had resolved.
        // MAGConjurePortalOpen2DSD, offered alongside it, is 377 KB and nearly
        // four and a half seconds. Sizes come from the BSA index joined to each
        // descriptor's ANAM, and the audio is uncompressed wav, so bytes track
        // seconds directly.
        //
        // ⚠⚠ LOOPING, WHICH IS WORSE, BECAUSE IT DOES NOT END ON ITS OWN. This
        // is played and forgotten: nothing holds the handle and nothing stops
        // it, so a looping descriptor plays until something else interrupts it.
        // MAGIllusionChargeSD did exactly that in the field, and it kept going
        // when a menu opened over it.
        //
        // ⚠ THE FLAG IS LNAM BYTE 1, BIT 0x08, verified against five descriptors
        // whose names already carry Bethesda's own answer: every ...LP and
        // ...LPM has it (MAGIllusionReadyLPSD 01 08 00 20, MAGConjureReadyLP
        // 00 08 00 20, MAGAlterationReadyLP 04 08 00 01, MAGCloakShockLPSD,
        // MAGShockWallNodeLP) and no one-shot does. ⚠ AND AN ABSENT LNAM IS NOT
        // A SAFE ZERO: MAGIllusionChargeSD has no LNAM subrecord at all, and it
        // is the one that looped. Require the field, then require the bit clear.
        //
        // Verified one-shots, LNAM present and 0x08 clear, shortest first:
        //
        //   UIMagicUnselect                    38 KB  ~0.4 s  a soft tick
        //   UIMagicSelect                      66 KB  ~0.7 s  brighter
        //   MAGMysticismSoulTrapCaptureShader  96 KB  ~1.1 s  the default
        //   MAGConjurationCharge050           114 KB  ~1.3 s
        //   MAGEnchantedUnsheatheOther        163 KB  ~1.9 s  enchanted draw
        //
        // The default pairs with what is on screen: the aura is borrowed from
        // the soul trap shader, and this is the soul trap's own capture cue.
        //
        // ⚠ AN UNKNOWN ID IS SILENT AND SAYS SO IN THE LOG. EditorStyle's
        // PlayUISound checks the handle before playing and logs an id it could
        // not build, which is the discipline the crashes above lacked.
        bool        requipSoundOn{ true };
        std::string requipSoundId{ "MAGMysticismSoulTrapCaptureShader" };

        // [Requip] bRequipFlashAllGear. Whether the flourish lights everything
        // worn or only the pieces that actually changed.
        //
        // ⚠ ON, BY FIELD REQUEST 2026-08-15 ("i also just want all gear to flash
        // when we switch outfit"), and it is a deliberate move away from what
        // the design argued for. Painting only the changed slots is defensible
        // on paper and reads as patchy in play: a swap that keeps the boots
        // leaves two dark boots standing in the middle of a violet character.
        //
        // ⚠ IT DOES NOT CHANGE WHEN THE FLOURISH FIRES, only what it paints.
        // The changed-slot diff still decides whether anything happens at all,
        // so an outfit rule that moves nothing is still silent and the mask that
        // is painted is simply wider than the mask that was tested.
        bool requipFlashAll{ true };

        // [Requip] fRequipSpeed. A multiplier over both halves of the curve, so
        // the field can tune half a second without a rebuild.
        //
        // ⚠ CLAMPED ON READ, and the clamp is load-bearing rather than tidy. A
        // zero or a negative divides the curve's span to nothing, which returns
        // 1.0 from RequipFlourish::detail::Unit on the very first frame and
        // drops the garment to alpha 0 before anything has posted a refresh.
        float requipSpeed{ 1.0f };

        // [Requip] sRequipColour. The light the garments burn into, as "R,G,B"
        // in 0-255. Defaults to the Seamstone violet.
        std::string requipColour{ "150,90,220" };

        // [Requip] sRequipArtForm and sRequipShaderForm. Where the aura comes
        // from, as "Plugin.esm|0xFORMID". Empty means FittingRoomLore.esp's own
        // FR_RequipArt and FR_RequipShader.
        //
        // ⚠ THESE EXIST SO THE EFFECT CAN BE JUDGED BEFORE IT IS AUTHORED. The
        // Creation Kit records are the destination and these are not a
        // substitute for them: a vanilla shader carries vanilla colours and
        // vanilla timings, so it will not match the Seamstone and cannot be
        // tuned. What it can do is answer "is a gas pass the right idea" for the
        // cost of an INI edit, and let the answer come from the game rather than
        // from an argument about it.
        //
        // The defaults are the soul trap pair, scanned out of Skyrim.esm on
        // 2026-08-15 rather than remembered: EFSH 0x000506D7 SoulTrapFXShader
        // and ARTO 0x000506D6 SoulTrapTargetEffects, chosen because they are
        // violet, body attached and about the right length. Others worth trying,
        // all measured from the same scan:
        //
        //   Skyrim.esm|0x00064D67  GhostEtherealFXShader   translucent, drifting
        //   Skyrim.esm|0x0010FDF9  SteamFXShader           denser, white
        //   Skyrim.esm|0x0002DF92  InvisFXShader           thin shimmer
        //   Skyrim.esm|0x0003F811  SummonTargetFX          ARTO, conjuration
        //   Skyrim.esm|0x000B967F  FXCameraAttachFineMistObject  ARTO, fine mist
        // ⚠ THE ART KEY IS A LIST AND THE SHADER KEY IS NOT. Art objects are
        // separate NIFs hung on the scene and several play together happily,
        // which is how the effect gets depth: a body plume, a ground burst and
        // a drift can be three records rather than one impossible one. Two
        // membrane shaders over one body just fight each other, so that stays a
        // single form.
        //
        // The body FX family is the one worth reading: `*RitualCastBodyFX` are
        // body attached particle systems under `Magic\`, which is what an
        // effect shader on its own cannot do.
        //
        //   Skyrim.esm|0x00044F57  SummonMassCastBodyFX      conjuration, violet
        //   Skyrim.esm|0x0010F7A2  ReanimateRitualCastBodyFX necromancy, violet
        //   Skyrim.esm|0x000E7559  BaneUndeadCastBodyFX      brighter, harder
        //   Skyrim.esm|0x0007534B  ShoutSelfAreaEffect01     burst from the feet
        //   Skyrim.esm|0x000154BD  FXAlduinSoulEscapeObject  rising soul streams
        // ⚠ THE DEFAULT IS THE ONE COMBINATION THE FIELD HAS APPROVED, and it is
        // deliberately the quiet one. The soul trap pair alone was called
        // fantastic on 2026-08-15. Everything richer that was tried on top of it
        // was rejected the same evening for being distracting, so a longer list
        // ships as a comment rather than as a default.
        // ⚠⚠ A SHADER CAN CARRY A LOOPING SOUND, AND IT IS NOT A SUBRECORD.
        // EFSH keeps almost everything in one 400 byte DATA blob, and the sound
        // is a form id at DATA+0x134. Listing the record's subrecord signatures
        // shows EDID/ICON/ICO2/NAM7/NAM8/NAM9/DATA and no audio field, which is
        // what led to two evenings of "the shader cannot be making that noise".
        // SoulTrapFXShader's DATA+0x134 resolves to SOUN
        // MAGMystisicmSoulTrapActiveLP: Bethesda's own typo, and an LP, so it
        // loops for as long as the effect is up.
        //
        // ⚠ 109 OF THE 169 VANILLA SHADERS READ ZERO THERE and play nothing of
        // their own. Check that field before borrowing one. Silent and worth
        // trying, all measured:
        //
        //   Skyrim.esm|0x00103129  GhostVioletFXShader     violet, the default
        //   Skyrim.esm|0x000D2057  GhostFXShaderNew        paler
        //   Skyrim.esm|0x000C5EF7  GhostFXShaderNightingale darker purple
        //   Skyrim.esm|0x00094162  MagicArmorEbonyFleshFXS a flesh-spell sheen
        //   Skyrim.esm|0x000ABF08  AbsorbBlueFXS           cooler
        std::string requipArtForm{ "Skyrim.esm|0x000506D6" };
        std::string requipShaderForm{ "Skyrim.esm|0x00103129" };

        // [Requip] sRequipSlotArtForm. Art played once PER CHANGED GARMENT,
        // hung on that garment's own node, rather than once on the actor.
        //
        // ⚠ THIS IS THE HALF THAT MAKES IT THE CLOTHES RATHER THAN THE PERSON.
        // `InstantiateHitArt` takes an attach node, so the emitter can ride the
        // partClone the swap is about to destroy. Deduped per node, because one
        // garment can answer to several slots.
        //
        // ⚠⚠ OFF BY DEFAULT, AND THE FIELD TURNED IT OFF RATHER THAN THE
        // AUTHOR. It shipped on for one evening with AbsorbSpellHitEffect01 and
        // came straight back as "too distracting, and bends light". Two reasons,
        // and both outlive that particular record:
        //
        // IT MULTIPLIES. A full-body swap is five or six nodes, so whatever is
        // named here plays five or six times at once. A record that is pleasant
        // once is a firework six times.
        //
        // REFRACTION MULTIPLIES WORSE. The absorb family draws through a
        // refractive pass, so six copies do not just brighten, they bend the
        // scene behind the character six times over. Anything with a distortion
        // or membrane pass is the wrong shape for this key, however good it
        // looks alone. Prefer a flat additive puff.
        //
        //   Skyrim.esm|0x000EA518  AbsorbSpellHitEffect01  ⚠ REJECTED, refractive
        //   Skyrim.esm|0x000A6EF6  AbsorbSpellEffect       ⚠ same family, longer
        //   Skyrim.esm|0x0010950B  DA16SkullDreamsteelHitFX  untried
        std::string requipSlotArtForm{ "" };

        // [Dye] bDyeSortByHue. How the swatches are ordered INSIDE a group.
        //
        // ⚠ IT ORDERS WITHIN A GROUP AND NEVER REGROUPS. My Dyes stay in front,
        // the rarity ladder keeps its order, and locked colours stay behind the
        // ones you can use, because all three are answers to questions a hue has
        // nothing to say about. What changes is only the last term: the pack's
        // own file order becomes a walk around the colour wheel.
        //
        // ⚠ ON AS OF 2026-08-14 (user), having shipped off that morning. With
        // 458 colours the pack's file order is not an order a player can use to
        // find anything, and the only reader who benefits from it is the pack
        // author. Off is still there for them.
        //
        // ⚠⚠ AND THE DEFAULT ONLY REACHES A FRESH INSTALL. An INI written while
        // this was false carries `bDyeSortByHue = false` and keeps winning, so
        // the change is invisible on exactly the machines that saw the first
        // version. See live-ini-overrides-code-defaults.
        bool dyeSortByHue{ true };

        // [Dye] bDyeKeepShine. Dye by rewriting the diffuse on the GPU and
        // leaving the material at its own feature, so a reflective piece keeps
        // its cubemap instead of going matte.
        //
        // ⚠ A NEW KEY RATHER THAN A NEW DEFAULT FOR iDyeSpikeRung, AND THE
        // MIGRATION IS THE WHOLE REASON. Save() writes iDyeSpikeRung on every
        // settings write, so every install that has ever saved carries an
        // explicit `iDyeSpikeRung = 0`. Flipping that default would therefore
        // reach nobody who already plays the mod: an explicit key beats a
        // changed default, and the change would look like it had shipped while
        // every existing character kept dyeing metal matte. Nobody's INI has
        // THIS key, so the default governs and an upgrade gets the shine.
        //
        // ⚠ AND IT BELONGS IN [Dye], NOT [Debug]. iDyeSpikeRung is a spike
        // control whose own comment documents rungs 1 to 4 and calls itself a
        // spike; it is the wrong thing to hand a player as the switch for how
        // their armour looks. It stays exactly what it was, an override for the
        // spike rungs, and EffectiveDyeRung below is the one place the two are
        // reconciled.
        bool dyeKeepShine{ true };

        // ---- dyeing a True PBR shape without taking it off the PBR path -----
        //
        // [Dye] bDyePbrOnPbrPath. A shape Community Shaders draws as True PBR
        // (property bit 42, kVertexLighting) used to be DOWNGRADED when it was
        // dyed: swapped to a FacegenTint material with the PBR marker and the
        // envmap, soft, rim and back lighting bits taken down with it. The
        // colour landed and the piece went FLAT, because CS stopped drawing it
        // as PBR, its `_rmaos` went unread, and a PG-patched mesh has no cubemap
        // left to fall back on (PG Patcher strips slot 4 on conversion). That
        // is the 2026-08-30 Callisto circlet report, which reached us as "it
        // loses its cubemap when we dye it" - the piece never had one.
        //
        // ⚠⚠ THE SHAPE HAS TO STAY ON THE PBR PATH TO KEEP ITS SHADING, and
        // the machinery for that already existed. Rung 3 clones through the
        // material's own VIRTUAL Create() and CopyMembers(), so a PBR material
        // clones as a PBR material with its rmaos, emissive and feature
        // textures intact, and the only write is `diffuseTexture`, which lives
        // on the base class. No flag moves, so the technique and the material
        // still agree - which makes this strictly SAFER than the downgrade it
        // replaces, not riskier: the 2026-08-21 exit CTD was a FacegenTint
        // material sitting under a PBR technique, and that pairing cannot
        // happen on this route.
        //
        // ⚠ THE ENVMAP FINISH BLOCK CANNOT REACH A PBR MATERIAL, which is what
        // makes the shared path type-safe. It is gated on GetFeature() ==
        // kEnvironmentMap and Community Shaders returns kDefault from its PBR
        // material on purpose, so the envMapScale store that would run off the
        // end of a shorter allocation is never reached here.
        //
        // ⚠ OFF PUTS THE DOWNGRADE BACK, unchanged, so a field round that does
        // not like the result costs an INI edit rather than a build. An
        // explicit iDyeSpikeRung still refuses a PBR shape outright either way:
        // a measurement that swaps paths on some shapes is not a measurement.
        bool dyePbrOnPbrPath{ true };

        // ---- a pearl's second colour on a True PBR piece --------------------
        //
        // [Dye] bDyePbrPearl. A pearlescent dye on a True PBR piece used to
        // fall to a nacre ramp keyed on the diffuse's luminance, and a PBR
        // base colour map is flat albedo: the ramp collapsed onto one point,
        // one colour, no sweep. This routes the dye's second stop (or its
        // sheen) onto the material's own fuzz and coat constants instead,
        // which is the layer the shader already renders at grazing angles -
        // the sheen a pearl physically is.
        //
        // ⚠ THE KILL SWITCH FOR A COMMUNITY SHADERS UPDATE. The writes are
        // raw offsets measured out of the shipped 26.8.16 DLL, gated on the
        // allocation's RTTI name (PbrPearl.h carries the measurements). A
        // newer CS that relays the material out from under those offsets
        // costs a field round an INI edit here rather than a rebuild.
        bool dyePbrPearl{ true };

        // [Dye] fDyePbrPearlSheenMax. The most a pearl may lift a piece's
        // fuzz weight to, at full strength. ⚠ FIELD-SET, 2026-08-30 19:58:
        // the first run floored every weight to 1.0 and the whole Callisto
        // set read as an oil slick over authored weights of 0.2 to 0.4, so
        // the ceiling exists exactly so the next round is an INI edit. 0
        // turns the lift off and leaves every artist value alone; the write
        // itself still recolours the fuzz.
        float dyePbrPearlSheenMax{ 0.5f };

        // ---- telling the player a colour just unlocked ----------------------
        //
        // [Dye] bDyeUnlockCards. A card at the top right when the economy hands
        // over a colour, the way a loot notification announces an item.
        //
        // ⚠ IT IS NOT THE GOLD FOLD'S SWITCH. ShowsNewDyeMark keeps marking
        // earned, unused colours in the grid whatever this is set to. Turning
        // this off silences the announcement, not the record of it.
        //
        // ⚠ NEW KEY, SO THE DEFAULT ACTUALLY REACHES EXISTING INSTALLS, which
        // is the trap bDyeKeepShine above documents at length. Nobody's INI
        // carries these.
        bool dyeUnlockCards{ true };

        // [Dye] fDyeCardDwell. How long a card holds before it fades, in
        // seconds. Clamped to 2..20 where it is read.
        float dyeCardDwell{ 6.0f };

        // [Dye] iDyeCardMax. How many cards stack on screen together. Clamped
        // to 1..8: past about five the stack reaches the compass and stops
        // reading as a notification.
        std::int32_t dyeCardMax{ 5 };

        // [Dye] sDyeCardSound. Played once when a batch of cards starts
        // arriving. Blank for none.
        //
        // ⚠ ONCE PER BATCH, NOT ONCE PER CARD, and the stagger is why. Cards
        // enter 0.35s apart and a burst can be six of them, so a cue per card
        // is a machine gun. DyeUnlockCard fires this on the empty-to-occupied
        // edge of the visible stack.
        //
        // ⚠ VERIFIED AGAINST Skyrim.esm RATHER THAN RECALLED, which is the rule
        // sChargeSpendSound's own comment exists because of: its first default
        // named a descriptor that does not exist and was silent for a whole
        // release. UIAlchemyLearnEffect is an SNDR in Skyrim.esm and it is the
        // game's own "you learned a new property of something" chime, which is
        // this event exactly.
        std::string dyeCardSound{ "UIAlchemyLearnEffect" };

        // [Dye] iDyeCardBurst. Past this many queued, the rest collapse into a
        // single "and N more" tail. Clamped to 2..12.
        //
        // ⚠ THE TAIL IS A DISCLOSURE, NOT A CAP THAT HIDES. A questline
        // completion can satisfy dozens of gates at once and the 08-13 economy
        // stint moved 121 colours behind gates one questline can clear;
        // announcing all of them one at a time would hold the corner of the
        // screen for minutes. The tail says the number instead.
        std::int32_t dyeCardBurst{ 6 };

        // [Dye] iDyeTickSeconds. How often, during gameplay, the unlock rules
        // are re-checked so a colour earned by what you just did is granted
        // while you are still standing there. Clamped to 5..600.
        //
        // ⚠⚠ WITHOUT THIS THERE IS NO SUCH MOMENT. Before 2026-08-14 promotion
        // ran at load and at editor open and nowhere else, so a colour was never
        // earned "while playing": it was earned on the next loading screen. A
        // card wired to those two call sites would fire over a load screen or
        // inside the editor it is suppressed in.
        //
        // ⚠ THE COST IS A WORLD GATHER, which is eighteen virtual actor value
        // reads plus a form lookup per quest or location the rules name, so this
        // is deliberately slow rather than per-frame. 0 disables the tick
        // entirely and puts the mod back on load-time promotion only.
        std::int32_t dyeTickSeconds{ 30 };

        // ---- the two-stop ramp's luminance window ---------------------------
        //
        // Where the ramp travels from stop A to stop B, in SOURCE luminance. A
        // texel darker than the low end is pure stop A, brighter than the high
        // end is pure stop B, and the smoothstep between them is what puts the
        // colour in the mid tones where a pearl carries it.
        //
        // ⚠ THESE ARE NOT COSMETIC AND THEIR FIRST VALUES WERE WRONG BY AN ORDER
        // OF MAGNITUDE. The ramp shipped with a hardcoded 0.15 to 0.85, which
        // assumes a texture using most of the range. Real Skyrim content does
        // not. Measured 2026-08-08 on Obi's Abyss set: the cubemap
        // ore_steel_e.dds has mean luminance 0.306 with 75% of its texels under
        // 0.387, so 0.15-0.85 left t pinned near zero and the reflection was
        // essentially stop A alone. Its diffuse Metals1_D.dds PEAKS at 0.216, so
        // t was 0.000 across the whole texture and nacre was arithmetically
        // identical to a flat dye. The ramp was running correctly and painting
        // one colour.
        //
        // ⚠ TWO WINDOWS BECAUSE THE TWO MODES KEY ON DIFFERENT QUANTITIES, which
        // is a stronger reason than the original one. The DIFFUSE pair is a window
        // on the texture's own luminance, so its useful range is whatever the
        // albedo happens to occupy. The REFLECTION pair is a window on an ANGLE:
        // the ramp there keys on the direction each cube texel represents, so its
        // input is a well-spread 0 to 1 and a broad window is what gives a smooth
        // sweep. They are not the same units and must never be collapsed.
        //
        // ⚠ AND THEY ARE INI KEYS BECAUSE THE RIGHT VALUES ARE A FIELD QUESTION
        // the spec deliberately left open. The defaults below are measured off
        // ONE armour set, which is a starting point rather than an answer, and
        // tuning them must not cost a rebuild.
        // ⚠ A FIXED WINDOW IS A STOPGAP, NOT THE ANSWER, and saying so is the
        // point of this note. These defaults are fitted to ONE armour set. A dark
        // metal albedo wants roughly 0.00 to 0.12; a white linen one would sit
        // entirely above that and come out as pure stop B, which is the opposite
        // failure. No single absolute pair can serve both, so the real fix is to
        // take the window from each texture's OWN luminance range, which needs a
        // reduction pass over the source and is the next piece of work here.
        // Until then these are the INI knobs that make the difference tunable.
        float dyeRampDiffuseLo{ 0.00f };     // [Dye] fDyeRampDiffuseLo
        float dyeRampDiffuseHi{ 0.12f };     // [Dye] fDyeRampDiffuseHi
        // Broad, because the input here is an angular sweep already spread across
        // 0 to 1 rather than a texture's luminance bunched at one end. Narrow this
        // and the colour snaps between the two stops instead of travelling.
        float dyeRampReflectionLo{ 0.15f };  // [Dye] fDyeRampReflectionLo
        float dyeRampReflectionHi{ 0.85f };  // [Dye] fDyeRampReflectionHi

        // Which quantity the REFLECTION's hue keys on. 0 elevation, 1 azimuth,
        // 2 the spec's original cubemap luminance.
        //
        // ⚠ A KNOB BECAUSE THREE ANSWERS HAVE NOW BEEN TRIED AND EACH COST A
        // REBUILD AND A RELAUNCH. Luminance read as marbling because a cubemap's
        // brightness describes the environment rather than the viewing angle.
        // Azimuth was the first correction and still did not read as pearl.
        // Elevation is the default. Which one actually looks like nacre is an art
        // question the spec deliberately left open, and it should not cost a
        // compile to ask it again.
        std::uint32_t dyeRampReflectionAxis{ 0 };  // [Dye] iDyeRampReflectionAxis

        // How much of the two stops' own BRIGHTNESS difference reaches the paint.
        // 0 reproduces the original fully-normalised behaviour, where both stops
        // came out at identical intensity and only the hue moved. 1 lets the ratio
        // through, so a brighter second stop reads as a sheen riding on a paler
        // body, which is what nacre actually is.
        //
        // ⚠ THIS IS THE ONE THE FIRST THREE ATTEMPTS ALL MISSED. Recolour divides
        // the tint by its own max because a dark dye on a gold cubemap would
        // otherwise go near-black, and the ramp inherited that divide without
        // noticing it also flattens the two stops onto one intensity. Beetle
        // Shell's stops differ 1.76x in max channel and every bit was discarded.
        //
        // Pull it back toward 0 if a bright second stop clips to white; that is
        // the failure this knob exists to let a player fix.
        float dyeRampSheen{ 1.0f };  // [Dye] fDyeRampSheen

        // Whether a special dye travels round the colour wheel between its two
        // stops instead of straight through RGB.
        //
        // ⚠ THE SHAPE ERROR UNDERNEATH THE OTHER THREE. A straight lerp between
        // two stops yields exactly one blend: green to purple goes through
        // grey-mauve and never through yellow or cyan. Nacre is thin-film
        // interference, a sweep THROUGH a sequence of hues, which is why a pearl
        // shows pink and green and gold at once. No axis, window or normalisation
        // can make a spectrum out of a straight line, so the first four attempts
        // were all adjusting position along a line that could not hold the answer.
        //
        // Off restores the straight RGB blend, which is what shipped through those
        // four attempts and is still the right look for a plain two-tone dye.
        bool dyeRampSpectral{ true };  // [Dye] bDyeRampSpectral

        // The luminance window scales off each texture's OWN average instead of
        // being one absolute pair, which is what the fixed-window stopgap note
        // above always said the real fix was. Off falls back to the absolute
        // keys. The two multipliers bracket the mean: lo x mean to hi x mean is
        // where the ramp travels, so mid-tones of ANY albedo land inside it.
        bool  dyeRampAutoWindow{ true };  // [Dye] bDyeRampAutoWindow
        float dyeRampAutoLo{ 0.35f };     // [Dye] fDyeRampAutoLo (x mean)
        float dyeRampAutoHi{ 2.20f };     // [Dye] fDyeRampAutoHi (x mean)

        // Which curve an ARMOUR DIFFUSE is dyed with. "softlight" is what every
        // install had before this key existed and is the default; "multiply",
        // "screen" and "overlay" are the photo-editor curves of those names.
        //
        // ⚠ THE SPELLING LIVES HERE AND THE MEANING DOES NOT. Resolving it to a
        // DyeTexture::Blend would need this header to include DyeTexture.h,
        // which would drag the engine's texture types into a header half the
        // project includes. DyeTexture::BlendFromName owns the mapping, next to
        // the enum it names, and the paint walk resolves once per pass.
        //
        // ⚠ GLOBAL ON PURPOSE, FOR NOW. A per-dye or per-slot blend has to ride
        // DyeChannel (there is no dye id downstream of a swatch), which means
        // the co-save codec, ApplyPaletteDye and the My Dyes writer. This key
        // exists to answer whether any of these curves is worth that, and only
        // a field run answers it. Eyes are deliberately NOT on this key: their
        // default is Overlay, a constant in PaintEyeTint, and a player who
        // wants something else picks it on the eye tile's own Blend mode row.
        std::string dyeBlendName{ "softlight" };  // [Dye] sDyeBlend

        // ---- the same dye, on a True PBR shape ------------------------------
        //
        // [Dye] sDyePbrBlend. Field 2026-08-30, first round with
        // bDyePbrOnPbrPath on: the Callisto circlet dyed, kept its shading, and
        // came back LESS SATURATED and off-hue against the rest of the outfit.
        // Both halves of that are measured rather than reasoned.
        //
        // ⚠⚠ THE SOURCE IS A DIFFERENT IMAGE, AND THAT ALONE WOULD DO IT. The
        // outfit ships the same artwork twice: `Callisto_BootsGlovesMask_Dif`
        // under `textures\` and again under `textures\pbr\`. Same dimensions,
        // same byte size, DIFFERENT hashes. A PBR base colour map is albedo with
        // the baked lighting taken out, so it is flatter and brighter than the
        // vanilla diffuse beside it. Overlay's output is a mix of tint and
        // source whose ratio is set by the SOURCE's value, so a brighter source
        // pulls the result toward white: less saturated, and off-hue against the
        // pieces that still carry the vanilla map. The gloves and boots read
        // that same artwork from the vanilla path and look right, which is the
        // control this conclusion rests on.
        //
        // ⚠ AND THE SHADER TAKES MORE OUT. `baseColor *= 1 - Metallic`
        // (Lighting.hlsl): on metallic texels the diffuse contribution is
        // multiplied by ZERO and the colour survives only as F0, the reflection
        // tint, which the environment then desaturates further. Nothing on our
        // side reaches that, so the blend is the lever we have.
        //
        // Default `multiply`, FIELD-SET 2026-08-31 round three: softlight left
        // a full-saturation dye "not very saturated" on the whole PBR set, and
        // multiply brought it back ("looks very good saturation wise", the
        // user, same session). Multiply's output is bounded by the source, so
        // the flat bright PBR albedo becomes a strength rather than the
        // washout it was under softlight and overlay. ⛔ NOT `recolour`,
        // though it is the obvious guess: it takes the tint's hue outright and
        // would erase the piece's own colour variation, so a gold trim and a
        // dark strap would come back as one hue at two brightnesses.
        //
        // ⚠ ONE VARIABLE. The two-stop ramp was the second suspect for the
        // hue; it is out of this key's blast radius now, since a pearl whose
        // fuzz carries the sweep takes a FLAT diffuse through this blend (see
        // pearlRidesFuzz in OutfitDye.cpp).
        std::string dyePbrBlendName{ "multiply" };  // [Dye] sDyePbrBlend

        // Offer the eye's SECOND colour, which is experimental. Off by default:
        // an ordinary install dyes the iris and the white, and that is the
        // whole eye feature.
        //
        // ⚠⚠ NOT A KEY THAT DECIDES WHAT A COLOUR LOOKS LIKE, which is the
        // distinction that makes this legitimate where bEyeDyeRecolour and
        // sEyeDyeSplitHex were not. Those two chose the ARITHMETIC behind a
        // player's back and became second authors of shipped state. This offers
        // or withholds a CONTROL, and the control's own value is stored on the
        // outfit where the player put it.
        //
        // ⚠ TURNING IT OFF LOSES NOTHING. Outfit::eyeTint2 keeps its bytes; the
        // stripe stops being offered and the painter stops reading it, so
        // turning the setting back on restores the colour that was there.
        //
        // ⚠⚠ THE PAINTER AND THE GRID MUST READ THE SAME ANSWER. DyeGrid::Build
        // takes it as a parameter (that file compiles into a pure test and may
        // name no singleton) and PaintEyeTint reads it here. A control offered
        // by one and ignored by the other is the exact bug that ate the sclera
        // for a day.
        bool eyeAdvancedColour{ false };          // [Dye] bEyeAdvancedColour

        // The iris mask's window, as multiples of the mask's own mean alpha,
        // exactly the auto-window trick above applied to the eye normal map's
        // alpha channel.
        //
        // ⚠ MULTIPLIERS OF THE MEAN, NOT ABSOLUTE ALPHAS, because the mask is
        // authored specular intensity and every eye set picks its own levels.
        // MEASURED on the ILV set: the disc sits at 0.30-0.34 over a floor of
        // 0.05-0.09 with a mean of 0.073, so the shipped pair puts the window
        // at 0.11..0.18, over the floor and under the disc, and the soft edge
        // in between feathers. INI keys rather than constants for the reason
        // the ramp axis is one: recalibrating this must be an INI edit and a
        // restart, never a rebuild.
        float dyeEyeMaskLo{ 1.5f };  // [Dye] fDyeEyeMaskLo (x mean)
        float dyeEyeMaskHi{ 2.5f };  // [Dye] fDyeEyeMaskHi (x mean)

        // ---- the derived iris disc, for eye sets whose mask is not one ------
        //
        // ⚠⚠ HALF THE EYE SETS IN A LOAD ORDER DO NOT DRAW AN IRIS IN THEIR
        // NORMAL ALPHA, AND THAT IS THE "iris dye colours the whole eye" REPORT
        // (field 2026-08-16, 3BA and HIMBO characters). MEASURED on the two
        // sets involved: the ILV mask UBE ships is a real disc, mean alpha
        // 0.073 with a peak of 0.34 over 7% of the texture. Vanilla's
        // eyebrown_n.dds is a hard 0/255 mask with mean 0.497 covering the
        // whole BOTTOM HALF, which is the entire eye area of a vanilla eye
        // texture rather than its iris. Tinting "inside the disc" therefore
        // tinted the eyeball.
        //
        // ⚠ THE REGION IS STILL WORTH READING, so the disc is DERIVED from it
        // rather than invented: centre on the covered region's own centre and
        // take a fraction of its smaller side. MEASURED against the same
        // vanilla pair: the region is the bottom half (centre 0.5, 0.75, sides
        // 1.0 x 0.5) and the iris in the diffuse sits at 0.5, 0.75 with a
        // radius near 0.12, which is 0.24 of that smaller side.
        float dyeEyeIrisRadius{ 0.24f };  // [Dye] fDyeEyeIrisRadius (x region)
        // The feather, as a fraction either side of the radius. An eye is a few
        // dozen pixels on screen and a hard edge reads as a ring.
        float dyeEyeIrisSoft{ 0.25f };  // [Dye] fDyeEyeIrisSoft
        // How much of the texture a mask has to cover before it is treated as
        // an eye REGION rather than an iris. The two measured sets sit at 0.07
        // and 0.497, so anything between them separates them; the shipped value
        // is nearer the low one because a mask that covers a quarter of an eye
        // texture is already too big to be an iris.
        float dyeEyeMaskBroad{ 0.25f };  // [Dye] fDyeEyeMaskBroad (coverage)

        // Which dye path actually runs, once the player setting and the spike
        // override are both taken into account.
        //
        // ⚠ ONE FUNCTION BECAUSE TWO READERS MUST NOT DRIFT. The dye walk picks
        // the path from this and the editor decides from it whether to warn that
        // dyeing a piece costs its shine. Those answering differently is a
        // tooltip that lies in one direction or the other, which is exactly the
        // class of bug the "RaceMenu is not loaded" message was.
        //
        // A spike rung set explicitly still wins, so the negative controls stay
        // runnable without touching the player setting.
        [[nodiscard]] std::uint32_t EffectiveDyeRung() const {
            return dyeSpikeRung != 0 ? dyeSpikeRung : (dyeKeepShine ? 5u : 0u);
        }

        // Whether the REFLECTION takes the dye's colour as well as the diffuse.
        //
        // ⚠ KEEPING THE SHINE AND COLOURING IT ARE ONE FEATURE, NOT TWO, and
        // shipping only the first is a half-measure that looks like a bug.
        // Recolouring the diffuse alone leaves the original steel cubemap on
        // top of it, so a red dye on a piece whose look is mostly reflection
        // reads as "barely dyed" (field screenshot 2026-08-08: #FF0000 on the
        // Abyss cuirass came out grey-green with a red hint). The gold shot
        // everyone remembers had the REFLECTION gold, and that was
        // bDyeTintReflection, not the diffuse rewrite.
        //
        // ⚠ AND THE CUBEMAP IS THE ONLY THING THAT CAN CARRY IT. The envmap
        // MASK was tried and measured dead on 1380 shapes: the shader reads one
        // channel, so a red dye left it at times 1 and did nothing while a blue
        // dye took it to times 0 and switched the reflection off. It is a
        // strength map, never a colour.
        //
        // Same shape as EffectiveDyeRung above: spike work driven by an explicit
        // iDyeSpikeRung keeps using the [Debug] key, ordinary play follows the
        // player's setting. So bDyeTintReflection is not a way to get shine
        // without colour during normal play, because that state is the defect
        // rather than a preference.
        [[nodiscard]] bool TintsReflection() const {
            return dyeSpikeRung != 0 ? dyeTintReflection : dyeKeepShine;
        }

        // OS-97. Point the world camera at the follower being edited instead of
        // at the player. Measured 2026-07-31: with the declutter cull fixed the
        // follower renders, but she sits 110 degrees off the view axis - behind
        // the camera - so "visible" and "in shot" turned out to be different
        // problems and only the first one was solved.
        //
        // ⚠ A KILL SWITCH, NOT A PREFERENCE. This writes PlayerCamera::
        // cameraTarget while Show Player In Inventory, SmoothCam and Menu
        // Studio's CameraGate all have a hand on the same camera. If the shot
        // fights, jitters, or the gameplay camera is left on the follower after
        // closing the editor, turn this off and the editor behaves exactly as it
        // did before.
        bool cameraFrameTarget{ true };  // [Compat] bCameraFrameTarget

        // Ask Menu Studio to swing its camera onto the part of the character
        // the editor is working on: the head while you pick a helmet, the feet
        // while you pick boots.
        //
        // ⚠ ON BY DEFAULT AS OF 2026-08-08, REVERSING THE 2026-08-04 CALL, both
        // on the user's own instruction. It was off because a camera that moves
        // when you click a list is a strong opinion and it seemed like the
        // player's to hold; the reversal is that a first-time player who never
        // finds the setting never sees the feature at all, and seeing the part
        // you are editing is most of the point of the editor.
        //
        // ⚠ IT STILL NEEDS MENU STUDIO'S OWN CAMERA SWITCHED ON, which is a
        // second consent Menu Studio checks for itself and refuses without. So
        // this default cannot move a camera on its own; it only stops being the
        // thing standing in the way.
        bool cameraFocusSlot{ true };  // [Compat] bCameraFocusSlot

        // [Compat] bReassertAppearance - Fitting Room puts a look's hair colour,
        // skin tone and head parts back after something else overwrites them.
        // Off, it stops, and RaceMenu and the engine's own head build get the
        // last word on how the character looks.
        //
        // ON IS THE SHIPPED BEHAVIOUR and the default. The switch exists
        // because the re-assertion is the most load-bearing thing the mod does
        // to a character and there was no way to take it out of the picture
        // while a fault was being measured. Turning it off leaves outfits,
        // dyes, overlays and the editor working; only the unrequested writes
        // stop.
        //
        // ⚠ IT IS THE PLAYER ONLY. NpcHair::Reassert keeps painting followers,
        // because a follower's colour is one the player asked for and nothing
        // else on the rig is fighting over it.
        //
        // ⚠ THE OWED FACE REBAKE STILL RUNS. It clears a face tint Fitting
        // Room itself baked for the wrong race, so skipping it would leave the
        // mod's own residue on a character it had just promised to stop
        // touching.
        // ⚠⚠ THE DEFAULT TURNED OVER ON 2026-08-29, FROM ON TO OFF. It shipped
        // on for the whole 1.1.x line, and two players reported a broken face
        // on the same night: eyes replaced with ones they had not chosen, and
        // a second face over the first. Both rolled back. A mod that quietly
        // rewrites a face somebody built in RaceMenu has to be asked, not
        // assumed, and everything the mod is actually for keeps working
        // without it.
        //
        // ⚠ ON AGAIN AS OF 1.1.7 (user 2026-09-02), after the face arc fixed
        // what had turned it off, and EXISTING INSTALLS ARE MIGRATED ONCE this
        // time: Save() had pinned the installer's 0 into every file, and the
        // FOMOD's rewrite reaches only a mod-manager install, so a hand-copied
        // ini or one written into Overwrite would have kept the old answer for
        // good. OS::LooksOn flips a 0 under an old stamp and leaves a stamped
        // one alone, which is the UI-scale migration's shape.
        //
        // ⚠⚠ NARROWED ON 2026-08-29 TO HEAD PARTS ONLY, and that is the whole
        // fix for the regression below. This flag used to gate FOUR things and
        // only ONE of them was ever implicated in the reports that turned it
        // off. The account above names two symptoms: "eyes replaced with ones
        // they had not chosen", which is ReassertPlayerHeadParts and nothing
        // else, and "a second face over the first", which is bLooksRaceMenu and
        // nothing else. Neither of them is a COLOUR. Turning the colour repairs
        // off alongside them was collateral, and it cost the regression that
        // bKeepColoursAfterRebuild now exists to undo. This flag keeps the one
        // job the evidence actually convicts.
        bool reassertAppearance{ true };  // [Compat] bReassertAppearance

        // [Compat] bKeepColoursAfterRebuild - put the COLOURS Fitting Room
        // painted back after the engine rebuilds a head over them. The skin
        // tone the mod is holding, and the hair colour an outfit is driving.
        // Head parts are NOT included and never will be: those are
        // bReassertAppearance's job, above, for the reason written there.
        //
        // ⚠⚠ ON BY DEFAULT, BECAUSE OFF WAS A SHIPPED REGRESSION. An interior
        // switch makes the engine rebuild the head. The rebuild repaints the
        // head and does not touch the body, so with no repair the head keeps
        // the engine's paint while the body keeps ours and the two no longer
        // match. Reported 2026-08-29 against 1.1.4 as "after interior switch it
        // always changes my head skin to a different one than the body", head
        // going white, confirmed by a second player the same day, and their own
        // three clues all name this: uninstalling the mod cures it, because
        // then nothing moved the body either; opening the inventory and closing
        // it cures it, because that path re-applies; and it survives a new game
        // and every installer answer, because it is not about a look at all.
        //
        // ⚠ A NEW KEY RATHER THAN A MIGRATION OF THE OLD ONE, deliberately.
        // Every ini written before today lacks it, so every install reads this
        // default and gets the repair back, mod-manager and hand-copied alike.
        // A migration would have had to guess whether a 0 in the old key meant
        // "I do not want my eyes replaced" or "I do not want my colours kept",
        // and those are now different questions.
        bool keepColoursAfterRebuild{ true };  // [Compat] bKeepColoursAfterRebuild

        // [Compat] bLooksRaceMenu - whether applying a look is allowed to load
        // its RaceMenu preset and switch race and sex, which is the character
        // and face steps in ProfilePlan. Off, a look still carries its outfit,
        // dyes, body, shape, skin and overlays; only the RaceMenu half stands
        // down.
        //
        // ⚠ ON BY DEFAULT AS OF 1.1.7 (user 2026-09-02). It shipped off from
        // 2026-08-29 while it was the newest and least proven thing in the mod;
        // the face arc that closed on 2026-09-01 fixed what the first night in
        // the field found, and the Looks page stops calling itself experimental
        // on the mod page. ProfileApply::Apply drops the two boxes centrally
        // rather than each caller checking. A 0 under an old stamp is moved
        // once by OS::LooksOn, the same as bReassertAppearance above.
        bool looksRaceMenu{ true };  // [Compat] bLooksRaceMenu

        // [Compat] bGridInventoryHide - an EXPERIMENT, and the account of it is
        // at SuppressGridInventory in EditorWindow.cpp. 'GISU' stops Grid
        // Inventory drawing but not taking the mouse, so its old area still
        // swallows a click-drag while the editor is up. kHide is meant to stop
        // both, and it closed the grid on 1.5.1 in five attempts out of five.
        //
        // ⚠ OFF, AND IT STAYS OFF UNTIL THE FIELD SAYS OTHERWISE. On a build
        // where kHide still closes the grid, turning this on costs the player
        // their editor the moment it opens.
        bool gridInventoryHide{ false };  // [Compat] bGridInventoryHide

        // [Compat] bPreLoadPresetErase - drop skee's mapped preset for the
        // player at kPreLoadGame, before the outgoing save is torn down.
        //
        // ⚠⚠ THIS IS A MEASURED EXPERIMENT, NOT SHIPPED BEHAVIOUR, and it must
        // be resolved before 1.0.0 rather than shipped as a switch. r63-r70
        // proved every erase INSIDE the load window manufactures the
        // load-double, and the erase was removed on that evidence. r90 then
        // measured the thing those rounds could not see: skee repaints the
        // hair from that same map during the LOAD SCREEN, 42 s before
        // kPostLoadGame, so every timing those rounds tried was downstream of
        // the paint. This message is the only window upstream of it, and it is
        // the only one never tried.
        //
        // ⚠ THE MAP CARRIES SCULPT AND HAIR COLOUR TOGETHER, which is why this
        // is a real bind rather than an oversight: keeping it bleeds the
        // previous character's hair colour onto this one, and dropping it has
        // so far doubled the face. A pass here that fixes the hair AND loads
        // clean is the result this switch exists to find.
        //
        // ⚠ THE VM MAY NOT ANSWER IN TIME. Papyrus is dispatched, not called,
        // and a load resets the VM. The dispatch and the answer both log with
        // their own timestamps precisely so a round can say whether the erase
        // landed before the head build rather than assuming it did.
        bool preLoadPresetErase{ true };  // [Compat] bPreLoadPresetErase

        // [Overlays] The Overlays picker's filter, off switch for.
        //
        // ⚠ IT LIVES HERE RATHER THAN ON THE PAGE, AND THAT IS A REVERSAL. It
        // was a checkbox beside the search box, on the argument that a player
        // who cannot find their texture has to see why from where they are
        // looking. The field verdict was that the page had become a wall of
        // controls and explanations for a thing that is right nearly always
        // (user 2026-08-16), so the switch is a setting and the page is a
        // picker again.
        //
        // Off, the picker offers the art each pack registered for the location
        // being edited, and leaves out the mask copies that carry no
        // transparency. On, it offers every texture installed.
        bool overlayShowAllArt{ false };  // [Overlays] bOverlayShowAllArt

        // [Overlays] OS-209: carry the Position transform across a change of
        // art instead of resetting it (user 2026-08-31, "can we have a toggle
        // for keep offset").
        //
        // Off, which is the default and the behaviour every build before this
        // one had, new art starts where its author drew it: a position belongs
        // to the picture it was set on, and carrying one across shows the new
        // tattoo shifted by the old one's slider. On, the numbers stay put and
        // the incoming art is baked at them straight away, which is what a
        // player dialling in one spot across several designs actually wants.
        //
        // ⚠ IT INTERCEPTS THE RESET, NOTHING ELSE. The carried transform goes
        // through the same Push the click already made, so key 9 and the
        // sidecar are written exactly as a slider release writes them. There is
        // no second store and no deferred value: the layer is the one author of
        // its own position either way.
        //
        // ⚠ AND IT IS A PANEL CONTROL, unlike bOverlayShowAllArt above. That
        // one is a picker filter a player sets once; this is a mode they flip
        // while working, so it sits in the Position header beside the sliders
        // it changes the meaning of, and persists here so it survives a
        // restart.
        bool overlayKeepOffset{ false };  // [Overlays] bOverlayKeepOffset

        // [Targets] Everyone loaded around the player in the "Editing:" roster,
        // not only followers (user 2026-08-18: "add a toggle ... to be able to
        // change other npcs in fitting room too, just add them to the dropdown
        // for 'Editing:' and have this option disabled by default"). Off, the
        // roster is the player, live teammates and away assignees, as it has
        // been. On, every loaded actor with an ActorTypeNPC race and a base an
        // assignment can be keyed on joins it, after the followers. Off by
        // default: a town puts dozens of names in the list, and a stranger's
        // outfit is theirs until the player says otherwise.
        bool editOtherNpcs{ false };  // [Targets] bEditOtherNpcs

        // [Rules] Rules::RuleEngine tunables (auto-switching). Heartbeat is
        // how often WorldWatch re-checks the world when nothing else (menu
        // close, combat, equip, death) already triggered an evaluation.
        // Min dwell is how long an auto-switched outfit must stay on before
        // a new match may replace it, skipped on the combat rising edge (see
        // RuleEngine::Evaluate's bypass).
        float heartbeatSeconds{ 2.0f };  // [Rules] fHeartbeatSeconds

        // [Rules] fCastHoldSeconds. How long the "when casting" condition stays
        // true after you STOP casting.
        //
        // ⚠⚠ IT IS A TAIL NOW, NOT THE WHOLE WINDOW, and the default came down
        // from 6 to 2 on 2026-08-14 because of it. It used to be the entire
        // definition of the condition: TESSpellCastEvent fires once on release
        // and fires the same way for a channelled spell and a thrown one, so
        // "casting" could only mean "within N seconds of that event". No N fits
        // both - 6 was long enough to cover Flames and far too long for a
        // fireball, which is what the field reported ("it takes so long to
        // switch back when we stop casting"). WorldWatch::CastingHeld now asks
        // the engine whether the player is still casting, so the channel holds
        // itself open for as long as it runs and this number only has to cover
        // the GAP BETWEEN casts.
        //
        // ⚠ SO DO NOT RAISE IT TO "CATCH" A SPELL. Nothing is being caught any
        // more; the live read does that. Raising it only makes the outfit
        // linger after the last cast. Lower it if the revert feels slow, raise
        // it only if a rotation of separate casts flickers between them.
        //
        // 0 or less switches the condition off: a rule using it never matches,
        // mid-cast included.
        float castHoldSeconds{ 2.0f };
        float minDwellSeconds{ 5.0f };   // [Rules] fMinDwellSeconds
    };

}  // namespace OS
