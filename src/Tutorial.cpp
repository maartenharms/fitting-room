#include "Tutorial.h"

#include "ChamferPanel.h"  // every button in here is one widget height
#include "EditorStyle.h"   // the cue when a step is satisfied
#include "FuckCompat.h"
#include "Settings.h"
#include "TutorialPlan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <vector>

namespace OS::Tutorial {

    namespace {

        using TutorialPlan::Id;

        // One card. Body only: no heading, per the register this editor already
        // settled on for first-run copy (the empty-pane hint's note, which
        // argues for muted, short, no heading, and never promising an optional
        // feature). The key is resolved at draw time so a language switch is
        // picked up without rebuilding the tables.
        //
        // A card that names an anchor rings it while it is up. kNone is an
        // ordinary answer: plenty of cards are about the page rather than about
        // one control on it.
        struct Step {
            const char* body;
            Anchor      at{ Anchor::kNone };
            // kNone is a card you read and dismiss. Anything else is a step you
            // have to DO, and it is drawn as a banner rather than a modal so
            // the editor stays usable while it waits.
            Action      need{ Action::kNone };
            // ⚠ THE VERY FIRST CARD OFFERS A WAY OUT, and only that one. A
            // tutorial that cannot be declined before it starts is a tutorial
            // people close and resent; asking once, up front, costs a button.
            bool        asks{ false };
            // What the session has to provide for this step to be askable at
            // all. kNone is the ordinary answer; see the note on Requires.
            Requires    wants{ Requires::kNone };
            // ⚠ A SECOND RING, AND IT EXISTS BECAUSE FOUR CARDS IS A HARD CAP
            // (TutorialPlan.h: past about six, people skip). A tester finished
            // the whole Outfits tutorial without noticing the "+" (user
            // 2026-08-16), and the honest fix, a card of its own, would have
            // been a fifth. One card can point at two things instead: the strip
            // says "these are your outfits" and the tighter box says "this one
            // makes another". LAST FIELD ON PURPOSE, so every existing
            // positional Step literal keeps compiling untouched.
            Anchor      also{ Anchor::kNone };
        };

        // The editor's attention colour, and the reason the ring is not the
        // theme's button colour any more.
        //
        // ⚠ THE MODAL'S BACKDROP IS A WHITE WASH, which is the user's own call
        // and is not going anywhere. A ring in ImGuiCol_ButtonActive sat in the
        // same tonal range as everything that wash had just flattened, so it
        // was there and nobody could see it (user 2026-08-08). Gold is the one
        // colour this editor already means something by: a rule owns this, a
        // search matched this, look here. Saturated rather than merely bright,
        // so it survives being washed.
        constexpr ImVec4 kHighlight{ 1.0f, 0.82f, 0.2f, 1.0f };

        // ⚠ THE COPY IS THE RECORDED CONFUSION, NOT A TOUR OF THE FEATURES.
        // These are the things playtesters actually did not work out, quoted in
        // the source beside the code that had to be changed for them. A card
        // spent on something nobody was ever confused by is a card spent.
        constexpr std::array kWelcomeSteps{
            Step{ "$FR_Tut_Welcome1", Anchor::kNone, Action::kNone, /*asks*/ true },
            Step{ "$FR_Tut_Welcome2Do", Anchor::kRail, Action::kOpenedPage },
        };

        // ⚠ READ, DO, DO, READ. The two middle steps are the ones the source
        // records people failing at, and reading about a control is not what
        // fixed either of them: a tester who is told where the roster is has
        // been told, and a tester who has clicked it has found it. The bookends
        // stay cards because "these tabs are your outfits" and "Apply may not
        // be needed" are facts rather than actions.
        constexpr std::array kOutfitsSteps{
            // ⚠ TWO RINGS ON ONE CARD, not a fifth card: the strip, plus the
            // "+" that a tester never noticed. See Step::also.
            Step{ "$FR_Tut_Outfits1", Anchor::kOutfitTabs, Action::kNone,
                  /*asks*/ false, Requires::kNone, Anchor::kAddOutfitTab },
            Step{ "$FR_Tut_Outfits2Do", Anchor::kSlotRow, Action::kSelectedSlot },
            // ⚠ ONLY WHEN THERE IS SOMEONE ELSE TO DRESS. The roster button is
            // not drawn at all with a one-entry list, so on a playthrough with
            // no follower this asked the player to click a control that was
            // never on screen and then waited for a click that could not happen.
            Step{ "$FR_Tut_Outfits3Do", Anchor::kTargetName, Action::kOpenedRoster,
                  /*asks*/ false, Requires::kOtherTargets },
            Step{ "$FR_Tut_Outfits4", Anchor::kApply },
        };

        // ⚠ THE TWO ACTIONS ARE THE WHOLE GESTURE, IN ORDER. Ring a piece, then
        // paint it. The source records that the pane "looked like it had done
        // nothing" because the ring moves on the tile three feet from the
        // control that acts on it, and doing the two halves in sequence is the
        // only explanation of that which cannot be misread.
        constexpr std::array kDyeSteps{
            Step{ "$FR_Tut_Dye1", Anchor::kDyeTiles },
            Step{ "$FR_Tut_Dye2Do", Anchor::kDyeTiles, Action::kSelectedDyeStripe },
            Step{ "$FR_Tut_Dye3Do", Anchor::kDyePalette, Action::kPickedDyeColour },
            Step{ "$FR_Tut_Dye4" },
        };

        constexpr std::array kPresetsSteps{
            Step{ "$FR_Tut_Presets1", Anchor::kPresetList },
            Step{ "$FR_Tut_Presets2", Anchor::kPresetTabs },
            Step{ "$FR_Tut_Presets3Do", Anchor::kPresetList, Action::kTriedPreset },
            Step{ "$FR_Tut_Presets4" },
        };

        // ⚠⚠ THIS PAGE HAS AN ACTION STEP NOW, AND THE ARGUMENT AGAINST ONE IS
        // RECORDED RATHER THAN DELETED (user 2026-08-16: "we don't really have
        // a rules page tutorial run through which we kind of need"). The old
        // note read: every honest ask here leaves something behind, since
        // making a rule leaves a live rule and flicking the master switch
        // changes a setting nobody came to change, so reading was the right
        // shape. That cost has not gone away. What changed is the page: an
        // empty Rules page now offers STARTERS, so the ask is one click that
        // produces a complete, named rule instead of a blank card with two
        // holes in it, and the card straight after it says the bin removes it.
        //
        // ⚠ READ, DO, READ, READ, and the order is the point. Card one says
        // what a rule is FOR in one sentence; the player then makes one; and
        // the two cards after it explain a card they are looking at rather than
        // one they are imagining. Reading about When and Then before a card
        // exists is exactly what the reporter struggled with.
        // ⚠ SIX CARDS AND THREE OF THEM ARE DOING, which is the shape asked for
        // by name (user 2026-08-16: "we need a bit more cards for the rules page
        // and it has to be more hands on, including how to add rules and
        // condition and then effects"). The middle three build one rule in the
        // order the card reads: make it, give it a When, give it a Then. The two
        // that close are about the card now sitting in front of the player.
        constexpr std::array kRulesSteps{
            Step{ "$FR_Tut_Rules1" },
            // ⚠ TWO RINGS, AND THE SECOND ONE IS OFTEN NOT THERE. "+ New Rule"
            // is on the toolbar whatever the list holds, so it is the anchor
            // that can always be pointed at; the starters are the friendlier
            // route and exist only on an empty page, where an anchor nobody
            // published simply draws no ring. That pairing is what let this
            // step stop being conditional (user 2026-08-16: a save with rules
            // in it was skipping straight past making one).
            Step{ "$FR_Tut_Rules2Do", Anchor::kNewRuleButton, Action::kMadeRule,
                  /*asks*/ false, Requires::kNone, Anchor::kRuleStarters },
            Step{ "$FR_Tut_Rules3Do", Anchor::kRuleAddCondition, Action::kAddedCondition },
            Step{ "$FR_Tut_Rules4Do", Anchor::kRuleAddEffect, Action::kAddedEffect },
            Step{ "$FR_Tut_Rules5", Anchor::kRuleList },
            Step{ "$FR_Tut_Rules6", Anchor::kRuleList },
        };

        // Dragging a slider is safe to ask for because the page itself puts the
        // character back when you leave without saving, which is also the thing
        // the step is teaching.
        constexpr std::array kShapeSteps{
            Step{ "$FR_Tut_Shape1" },
            Step{ "$FR_Tut_Shape2", Anchor::kShapeSliders },
            Step{ "$FR_Tut_Shape3Do", Anchor::kShapeSliders, Action::kMovedShapeSlider },
        };

        // ⚠ READ, DO, DO, READ, the Outfits shape, because the recorded
        // confusions match: the field could not tell the two systems on this
        // page apart (overlays are skee node overrides, makeup is the face
        // bake), and clicking down the layer list then putting art in a layer
        // is the gesture the page exists for. The last card is the makeup
        // half's three facts nobody could have guessed: face bake, player
        // only, and a hard budget of fifteen worn layers.
        constexpr std::array kOverlaysSteps{
            Step{ "$FR_Tut_Ovl1", Anchor::kOverlayLayers },
            Step{ "$FR_Tut_Ovl2Do", Anchor::kOverlayLayers, Action::kSelectedOverlayLayer },
            Step{ "$FR_Tut_Ovl3Do", Anchor::kOverlayTextures, Action::kPickedOverlayTexture },
            Step{ "$FR_Tut_Ovl4", Anchor::kMakeupSections },
        };

        // The send-off. Two cards: what is left to discover, and a thank you.
        //
        // ⚠ NO ACTION STEP. Every other tutorial ends by having the player do
        // the thing, and the honest ask here would be "open the settings", which
        // drops them into a panel of switches with no tutorial to explain it.
        // Pointing at the gear and saying what is behind it is the whole job.
        constexpr std::array kClosingSteps{
            Step{ "$FR_Tut_Closing1", Anchor::kSettingsTile },
            Step{ "$FR_Tut_Closing2" },
        };

        // "Pick a preset on the left to begin" is what the empty page already
        // says, so the first step asks for it rather than repeating it.
        constexpr std::array kBodyStudioSteps{
            Step{ "$FR_Tut_Body1Do", Anchor::kBodyLibrary, Action::kPickedBodyPreset },
            Step{ "$FR_Tut_Body2" },
            Step{ "$FR_Tut_Body3" },
        };

        // ⚠ THE CAP IS ASSERTED PER TABLE, not trusted to whoever adds a card.
        // ⚠ THE PART COLUMN IS THE WHOLE TUTORIAL, and the other three cards
        // exist to get the player in front of it. Every other page in this
        // editor does one thing to one part of the character; a look is the
        // whole character in one file, so the only idea anybody has to be
        // taught here is that Apply is SELECTIVE. Bring back the face and
        // leave today's outfit alone, or take the outfit and dyes off a look
        // and keep your own face. Nothing on the page says that by itself:
        // ten ticked boxes read as "this is what is in the file", not as "this
        // is what is about to move".
        //
        // ⚠ AND THE PLAYER-ONLY RULE IS A GOTCHA, NOT A FEATURE, so it gets
        // the last card rather than the first. Looks capture and apply the
        // PLAYER whoever the editor is pointed at, which is a surprise
        // precisely for the player who has just spent the Outfits tutorial
        // learning to dress a follower.
        constexpr std::array kLooksSteps{
            Step{ "$FR_Tut_Looks1", Anchor::kLooksLibrary },
            Step{ "$FR_Tut_Looks2Do", Anchor::kLooksLibrary, Action::kSelectedLook,
                  /*asks*/ false, Requires::kSavedLooks },
            // ⚠⚠ THIS ONE NEEDS A SAVED LOOK TOO, and the anchor is the proof
            // rather than the argument: ProfilesUI publishes kLooksParts INSIDE the
            // branch that runs when a look is selected, so on an empty library it is
            // never published at all. The card would explain a column of checkboxes
            // that is provably not on screen, and ring nothing while doing it.
            Step{ "$FR_Tut_Looks3", Anchor::kLooksParts, Action::kNone,
                  /*asks*/ false, Requires::kSavedLooks },
            Step{ "$FR_Tut_Looks4", Anchor::kLooksSave },
        };

        static_assert(kWelcomeSteps.size() <= TutorialPlan::kMaxSteps);
        static_assert(kOutfitsSteps.size() <= TutorialPlan::kMaxSteps);
        static_assert(kDyeSteps.size() <= TutorialPlan::kMaxSteps);
        static_assert(kPresetsSteps.size() <= TutorialPlan::kMaxSteps);
        static_assert(kRulesSteps.size() <= TutorialPlan::kMaxSteps);
        static_assert(kShapeSteps.size() <= TutorialPlan::kMaxSteps);
        static_assert(kBodyStudioSteps.size() <= TutorialPlan::kMaxSteps);
        static_assert(kOverlaysSteps.size() <= TutorialPlan::kMaxSteps);
        static_assert(kLooksSteps.size() <= TutorialPlan::kMaxSteps);
        static_assert(kClosingSteps.size() <= TutorialPlan::kMaxSteps);

        // How many cards a table shows when a requirement is or is not met.
        //
        // ⚠ ASSERTED AGAINST THE REAL TABLES, because no test compiles this
        // file. The plan built at runtime is this same filter, so pinning the
        // counts here is the only place a later card that also needs a follower
        // can be caught shortening the no-follower tutorial by surprise.
        template <std::size_t N>
        [[nodiscard]] constexpr std::size_t CountEligible(const std::array<Step, N>& a_steps,
                                                          Requires a_absent) {
            std::size_t n = 0;
            for (const Step& s : a_steps) {
                if (s.wants == Requires::kNone || s.wants != a_absent) {
                    ++n;
                }
            }
            return n;
        }

        static_assert(CountEligible(kOutfitsSteps, Requires::kNone) == 4,
                      "with a follower present the Outfits tutorial is four cards");
        static_assert(CountEligible(kOutfitsSteps, Requires::kOtherTargets) == 3,
                      "with nobody but the player it is three, and the roster step "
                      "is the one that goes: it waits on a control the editor does "
                      "not draw for a one-entry list");
        static_assert(CountEligible(kWelcomeSteps, Requires::kOtherTargets) ==
                          kWelcomeSteps.size(),
                      "no other tutorial depends on there being a follower");
        static_assert(CountEligible(kRulesSteps, Requires::kNone) == 6,
                      "the Rules tutorial is six cards, three of which build one "
                      "rule: make it, condition it, then give it something to do");
        static_assert(CountEligible(kRulesSteps, Requires::kOtherTargets) ==
                          kRulesSteps.size(),
                      "and none of them depends on the session, so a save with "
                      "rules already in it gets the same six");
        static_assert(CountEligible(kDyeSteps, Requires::kOtherTargets) == kDyeSteps.size());
        static_assert(CountEligible(kShapeSteps, Requires::kOtherTargets) ==
                      kShapeSteps.size());
        static_assert(CountEligible(kOverlaysSteps, Requires::kOtherTargets) ==
                      kOverlaysSteps.size());
        static_assert(CountEligible(kLooksSteps, Requires::kNone) == 4,
                      "with a look already saved the Looks tutorial is four cards");
        static_assert(CountEligible(kLooksSteps, Requires::kSavedLooks) == 2,
                      "on an empty library it is two: what a look is, and how to "
                      "save one. The pick step goes because there is nothing in the "
                      "list to click, and the parts step goes because its anchor is "
                      "only published beside a selected look");
        static_assert(CountEligible(kLooksSteps, Requires::kOtherTargets) ==
                          kLooksSteps.size(),
                      "and none of it depends on there being a follower, because "
                      "a look is the player's whoever is being edited");

        // ⚠ AN EMPTY TABLE IS HOW A TUTORIAL STAYS UNWRITTEN SAFELY, which is
        // what the kCount arm relies on. ShouldFire refuses a tutorial with no
        // cards, so a page whose copy has not been written yet costs nothing
        // and cannot open an empty box that marks itself done.
        [[nodiscard]] std::span<const Step> StepsFor(Id a_id) {
            switch (a_id) {
                case Id::kWelcome:
                    return kWelcomeSteps;
                case Id::kOutfits:
                    return kOutfitsSteps;
                case Id::kDye:
                    return kDyeSteps;
                case Id::kPresets:
                    return kPresetsSteps;
                case Id::kRules:
                    return kRulesSteps;
                case Id::kShape:
                    return kShapeSteps;
                case Id::kBodyStudio:
                    return kBodyStudioSteps;
                case Id::kOverlays:
                    return kOverlaysSteps;
                case Id::kLooks:
                    return kLooksSteps;
                case Id::kClosing:
                    return kClosingSteps;
                case Id::kCount:
                    break;
            }
            return {};
        }

        // Which tutorial a page owns. Switch and no default:, so a seventh page
        // is a compile error here rather than a page that silently teaches
        // nothing, which is the same discipline PlanPaneTransition uses.
        [[nodiscard]] Id ForPane(EditorGate::PaneMode a_mode) {
            switch (a_mode) {
                case EditorGate::PaneMode::kStyles:
                    return Id::kOutfits;
                case EditorGate::PaneMode::kDye:
                    return Id::kDye;
                case EditorGate::PaneMode::kPresets:
                    return Id::kPresets;
                case EditorGate::PaneMode::kRules:
                    return Id::kRules;
                case EditorGate::PaneMode::kShape:
                    return Id::kShape;
                case EditorGate::PaneMode::kBodyStudio:
                    return Id::kBodyStudio;
                case EditorGate::PaneMode::kOverlays:
                    return Id::kOverlays;
                case EditorGate::PaneMode::kProfiles:
                    return Id::kLooks;
            }
            return Id::kCount;
        }

        // The seven INI bools, reachable one at a time. Kept as individual
        // fields rather than a mask in Settings so the INI stays readable and
        // hand-editable, which is the documented escape hatch for every other
        // key in that file; the mask below is built from them for the pure
        // layer's benefit and never stored.
        //
        // Switch and no default:, so an eighth tutorial is a compile error here
        // rather than one that silently never records itself as finished.
        [[nodiscard]] bool* FlagPtr(Settings& a_cfg, Id a_id) {
            bool* p = nullptr;
            switch (a_id) {
                case Id::kWelcome:
                    p = &a_cfg.tutorialWelcome;
                    break;
                case Id::kOutfits:
                    p = &a_cfg.tutorialOutfits;
                    break;
                case Id::kDye:
                    p = &a_cfg.tutorialDye;
                    break;
                case Id::kPresets:
                    p = &a_cfg.tutorialPresets;
                    break;
                case Id::kRules:
                    p = &a_cfg.tutorialRules;
                    break;
                case Id::kShape:
                    p = &a_cfg.tutorialShape;
                    break;
                case Id::kBodyStudio:
                    p = &a_cfg.tutorialBodyStudio;
                    break;
                case Id::kOverlays:
                    p = &a_cfg.tutorialOverlays;
                    break;
                case Id::kLooks:
                    p = &a_cfg.tutorialLooks;
                    break;
                case Id::kClosing:
                    p = &a_cfg.tutorialClosing;
                    break;
                case Id::kCount:
                    break;
            }
            return p;
        }

        [[nodiscard]] std::uint32_t SeenMask() {
            auto&         cfg  = Settings::GetSingleton();
            std::uint32_t seen = TutorialPlan::kNothingSeen;
            for (auto raw = static_cast<std::uint8_t>(Id::kWelcome);
                 raw < static_cast<std::uint8_t>(Id::kCount); ++raw) {
                const auto id = static_cast<Id>(raw);
                if (const bool* p = FlagPtr(cfg, id); p && *p) {
                    seen = TutorialPlan::MarkSeen(seen, id);
                }
            }
            return seen;
        }

        // ⚠ WRITTEN THROUGH TO DISK IMMEDIATELY. A tutorial marked done only in
        // memory comes back after a crash or an alt-F4, which is exactly the
        // session where a player least wants to be taught again. Settings::Save
        // is the same call the settings panel already makes on every change.
        void MarkDone(Id a_id) {
            auto& cfg = Settings::GetSingleton();
            if (bool* p = FlagPtr(cfg, a_id)) {
                *p = true;
                cfg.Save();
            }
        }

        // ---- live state -------------------------------------------------
        //
        // ⚠ A CARD OUTLIVES THE FRAME THAT ASKED FOR IT, so none of this can be
        // a frame local. The right-click menu on the outfit tabs carries the
        // same note for the same reason.
        Id          g_active{ Id::kCount };   // kCount means nothing is up
        Id          g_pending{ Id::kCount };  // asked for, not yet opened
        std::size_t g_step{ 0 };
        // ⚠ ONE TUTORIAL PER VISIT TO A PAGE, WHICH IS NOT ONE PER OPEN, and
        // the difference is a bug this already caused. The welcome is about the
        // rail and Outfits is the page you land on, so finishing the first put
        // the second up in the same breath: two cards, then immediately four
        // more (user 2026-08-08, "straight after we go past 2/2 we get hit with
        // another 1/4"). One per OPEN stopped that and went too far: after the
        // welcome, nothing else could appear for the rest of the session
        // whatever the player did, so the remaining six tutorials were
        // unreachable without closing the editor (same user, same day).
        //
        // Keyed on the page instead. Arriving somewhere is the event, so the
        // welcome finishes, and going to Dye brings the Dye one. Coming back to
        // Outfits brings that one. Nothing stacks, and nothing is stranded.
        bool                 g_firedThisVisit{ false };
        EditorGate::PaneMode g_lastPane{ EditorGate::PaneMode::kStyles };
        bool                 g_paneKnown{ false };
        // A read card is owed its modal, and only window scope may open one.
        bool g_wantCard{ false };
        // Whether the card has actually been seen on screen since it was asked
        // for. ⚠ WITHOUT THIS, A BEGIN THAT NEVER MATCHES IS INDISTINGUISHABLE
        // FROM A PLAYER PRESSING ESCAPE, and the difference is everything: one
        // is a bug that should heal, the other is a dismissal that should be
        // obeyed. Assuming the second silently threw away every tutorial for a
        // whole session when the open and the begin drifted apart in the id
        // stack (2026-08-08).
        bool g_cardSeen{ false };

        constexpr const char* kPopupId = "tutorial";

        // Where each control was, and on which frame it said so.
        //
        // ⚠ THE FRAME STAMP IS THE WHOLE SAFETY. A rect with no stamp for this
        // frame belongs to a control that is not on screen: a different page is
        // open, the list has scrolled past it, or nobody has wired that anchor
        // up yet. Ringing a remembered rectangle would draw a bright box around
        // whatever now occupies those pixels, and there is no clip rect
        // anywhere in this API to save us from that.
        struct AnchorRect {
            ImVec2        min{};
            ImVec2        max{};
            std::uint64_t frame{ 0 };
        };
        std::array<AnchorRect, static_cast<std::size_t>(Anchor::kCount)> g_anchors{};
        std::uint64_t                                                    g_frame{ 1 };

        // ⚠ DEFAULTS TRUE, AND THAT IS THE FAIL-SAFE DIRECTION. An unstated
        // requirement leaves the step in, so a fact nobody wired up yet costs a
        // step that cannot be completed rather than silently deleting a step
        // from everyone's tutorial. The first is visible the moment anyone
        // plays it; the second is invisible forever.
        std::array<bool, static_cast<std::size_t>(Requires::kCount)> g_facts{ true, true };

        // The steps this run will actually show, as indices into the step table.
        //
        // ⚠ AN INDIRECTION RATHER THAN A SKIP AT ADVANCE TIME, because the card
        // prints "step N of M". Stepping over an ineligible entry would number
        // the cards 1, 2, 4 of 4 and leave the player looking for the one that
        // went missing. Filtering first makes the count the truth.
        std::array<std::uint8_t, TutorialPlan::kMaxSteps> g_plan{};
        std::size_t                                      g_planCount{ 0 };

        // The ring, pulsing, over the card's own dimmed backdrop.
        //
        // ⚠ DrawScreenRect, NOT DrawRect. The window list draws in submission
        // order underneath the modal and its backdrop; the screen list is the
        // only one that reaches above them, and it is the same call the rules
        // page's drag indicator already uses for the same reason.
        //
        // ⚠ AND THE PULSE IS THE DYE SELECTION FLASH'S SHAPE, deliberately. The
        // editor already answers "the thing you just did happened over there"
        // by fading an outline in ButtonActive over about half a second, and it
        // was built for the neighbouring complaint. A second, different idea of
        // what a highlight looks like would be one too many.
        // What the ring must not draw across: the card itself.
        //
        // ⚠ THE SCREEN LIST IS ABOVE EVERYTHING, INCLUDING THE CARD. That is
        // exactly why it was chosen, and it is also why a ring round a large
        // region drew its edge straight through the middle of the tutorial card
        // sitting over it (user 2026-08-08, screenshot). There is no clip rect
        // anywhere in this API, so the only answer is to work out which parts of
        // the ring are not behind the card and submit only those. Same
        // arithmetic-instead-of-clipping idiom as HatchRect.
        struct Excluded {
            bool   on{ false };
            ImVec2 lo{};
            ImVec2 hi{};
        };

        [[nodiscard]] bool Overlaps(const ImVec2& a_lo, const ImVec2& a_hi,
                                    const Excluded& a_ex) {
            return a_ex.on && a_ex.hi.x > a_lo.x && a_ex.lo.x < a_hi.x &&
                   a_ex.hi.y > a_lo.y && a_ex.lo.y < a_hi.y;
        }

        // One filled band, minus the part the card covers. At most four pieces:
        // above, below, and the two side strips left over between them.
        //
        // ⚠ THE PIECES LOSE THEIR ROUNDING. A rounded corner on a piece that is
        // only a piece would put a curve in the middle of a straight edge, which
        // reads as a rendering fault rather than as a highlight.
        void FillMinus(const ImVec2& a_lo, const ImVec2& a_hi, const Excluded& a_ex,
                       ImU32 a_col, float a_rounding) {
            if (a_hi.x <= a_lo.x || a_hi.y <= a_lo.y) {
                return;
            }
            if (!Overlaps(a_lo, a_hi, a_ex)) {
                FUCK::DrawScreenRectFilled(a_lo, a_hi, a_col, a_rounding);
                return;
            }
            const float x0 = std::max(a_lo.x, a_ex.lo.x);
            const float x1 = std::min(a_hi.x, a_ex.hi.x);
            const float y0 = std::max(a_lo.y, a_ex.lo.y);
            const float y1 = std::min(a_hi.y, a_ex.hi.y);
            if (y0 > a_lo.y) {
                FUCK::DrawScreenRectFilled(a_lo, ImVec2(a_hi.x, y0), a_col, 0.0f);
            }
            if (y1 < a_hi.y) {
                FUCK::DrawScreenRectFilled(ImVec2(a_lo.x, y1), a_hi, a_col, 0.0f);
            }
            if (x0 > a_lo.x) {
                FUCK::DrawScreenRectFilled(ImVec2(a_lo.x, y0), ImVec2(x0, y1), a_col, 0.0f);
            }
            if (x1 < a_hi.x) {
                FUCK::DrawScreenRectFilled(ImVec2(x1, y0), ImVec2(a_hi.x, y1), a_col, 0.0f);
            }
        }

        void DrawRing(Anchor a_anchor, const Excluded& a_ex) {
            if (a_anchor == Anchor::kNone || a_anchor == Anchor::kCount) {
                return;
            }
            const auto& rect = g_anchors[static_cast<std::size_t>(a_anchor)];
            if (rect.frame != g_frame) {
                return;  // not drawn this frame, so there is nothing to point at
            }
            const float scale = std::max(1.0f, FUCK::GetResolutionScale());
            // ⚠ CLEAR OF THE THING RATHER THAN ON IT. Three pixels put the
            // stroke down on the tab borders and the "+" beside them, so it
            // read as a line cutting through the controls instead of a box
            // around them (user 2026-08-08, screenshot). It has to miss what it
            // is pointing at by more than the widget's own border.
            const float pad = 6.0f * scale;
            // 0.55 to 1.0 and back, a little under two seconds a cycle. Never
            // to zero: a ring that vanishes reads as a glitch rather than as a
            // pulse, and the player may be looking away on the down beat.
            const float wave =
                0.5f + 0.5f * std::sin(static_cast<float>(FUCK::GetTime()) * 3.4f);
            const ImVec2 lo(rect.min.x - pad, rect.min.y - pad);
            const ImVec2 hi(rect.max.x + pad, rect.max.y + pad);
            const float  rounding = OS::ui::FrameRounding();

            // ⚠ A WASH AS WELL AS A STROKE. An outline is a few hundred pixels
            // of colour on a screen the modal has just washed white, and it was
            // reported as barely noticeable. Tinting the whole region is what
            // makes the eye land there, and the stroke then says where the
            // region ends. Kept low: this sits ON TOP of the control, so
            // anything heavier would be hiding what it is pointing at.
            ImVec4 fill = kHighlight;
            fill.w      = 0.10f + 0.10f * wave;
            FillMinus(lo, hi, a_ex, OS::ui::Col(fill), rounding);

            // ⚠ THE OUTLINE IS FOUR BANDS, NOT DrawScreenRect. A stroked rect
            // is one call that cannot be cut, and the whole point here is that
            // parts of it must not be drawn. Four thin filled bands go through
            // the same subtraction the wash does, so the ring and its fill can
            // never disagree about where the card is.
            const float  t    = 3.0f * scale;
            const ImU32  line = OS::ui::Col(ImVec4{ kHighlight.x, kHighlight.y, kHighlight.z,
                                                   0.65f + 0.35f * wave });
            FillMinus(lo, ImVec2(hi.x, lo.y + t), a_ex, line, 0.0f);
            FillMinus(ImVec2(lo.x, hi.y - t), hi, a_ex, line, 0.0f);
            FillMinus(ImVec2(lo.x, lo.y + t), ImVec2(lo.x + t, hi.y - t), a_ex, line, 0.0f);
            FillMinus(ImVec2(hi.x - t, lo.y + t), ImVec2(hi.x, hi.y - t), a_ex, line, 0.0f);
        }

        // Break a sentence into lines that fit a_maxW, measured rather than
        // estimated. Words only: a single word wider than the limit takes its
        // own line and overhangs, which is the honest failure for a limit that
        // cannot hold it, and is what any wrap does with an unbreakable run.
        std::vector<std::string> WrapToWidth(const char* a_text, float a_maxW) {
            std::vector<std::string> lines;
            if (!a_text || !*a_text) {
                return lines;
            }
            const std::string text{ a_text };
            std::string       line;
            std::size_t       i = 0;
            while (i < text.size()) {
                const std::size_t space = text.find(' ', i);
                const std::string word =
                    text.substr(i, space == std::string::npos ? std::string::npos : space - i);
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (!line.empty() && OS::ui::CalcTextWidth(candidate.c_str()) > a_maxW) {
                    lines.push_back(line);
                    line = word;
                } else {
                    line = candidate;
                }
                if (space == std::string::npos) {
                    break;
                }
                i = space + 1;
            }
            if (!line.empty()) {
                lines.push_back(line);
            }
            return lines;
        }

        // Every ring this step asks for, in one call so the three draw sites
        // cannot disagree about how many there are. The second one is optional
        // and costs nothing when it is kNone.
        void DrawRings(const Step& a_step, const Excluded& a_ex) {
            DrawRing(a_step.at, a_ex);
            DrawRing(a_step.also, a_ex);
        }

        // ⚠⚠ WHICH UNMET REQUIREMENTS ARE WORTH COMING BACK FOR. A saved look is
        // made on the Looks page itself, usually within a minute of reading the
        // card that says how, so a tutorial shortened by its absence is worth
        // re-offering on the next visit and converges immediately. A follower is
        // not: it is a fact about the playthrough that a solo player may never
        // make true, and the step that wants one is on Outfits, the page the
        // editor opens on. Waiting for that would put the same three cards up
        // every time the editor opened, which is worse than the card they missed.
        [[nodiscard]] constexpr bool Retryable(Requires a_wants) {
            return a_wants == Requires::kSavedLooks;
        }

        [[nodiscard]] bool Holds(const Step& a_step) {
            return a_step.wants == Requires::kNone ||
                   g_facts[static_cast<std::size_t>(a_step.wants)];
        }

        // How many cards a_id would actually show right now. ⚠ THIS IS WHAT
        // ShouldFire HAS TO BE ASKED, not the table's raw size: a tutorial whose
        // every step is ineligible would otherwise fire, find nothing to draw,
        // and mark itself seen.
        [[nodiscard]] std::size_t EligibleCount(Id a_id) {
            std::size_t n = 0;
            for (const Step& s : StepsFor(a_id)) {
                if (Holds(s)) {
                    ++n;
                }
            }
            return n;
        }

        // The card count a finished run is measured against: what it showed, plus
        // what it dropped for a requirement worth waiting on. See ShouldMarkSeen.
        [[nodiscard]] std::size_t RetryableTotal(Id a_id) {
            std::size_t n = 0;
            for (const Step& s : StepsFor(a_id)) {
                if (Holds(s) || Retryable(s.wants)) {
                    ++n;
                }
            }
            return n;
        }

        void BuildPlan(Id a_id) {
            g_planCount      = 0;
            const auto steps = StepsFor(a_id);
            for (std::size_t i = 0; i < steps.size() && g_planCount < g_plan.size(); ++i) {
                if (Holds(steps[i])) {
                    g_plan[g_planCount++] = static_cast<std::uint8_t>(i);
                }
            }
        }

        // The step the player is on, resolved through the plan.
        [[nodiscard]] const Step& CurStep(std::span<const Step> a_steps) {
            return a_steps[g_plan[g_step]];
        }

        // a_skipped is the player saying no, which always ends it. Reaching the
        // end of a plan that was cut short does not; see TutorialPlan::ShouldMarkSeen.
        void Finish(bool a_skipped) {
            if (g_active != Id::kCount &&
                TutorialPlan::ShouldMarkSeen(g_planCount, RetryableTotal(g_active),
                                             a_skipped)) {
                MarkDone(g_active);
            }
            g_active    = Id::kCount;
            g_step      = 0;
            g_planCount = 0;
        }

        // ⚠ NOT MARKED SEEN ON THE WAY OUT. Someone who says no has not been
        // shown the welcome, so if they turn tutorials back on later they
        // should get it, not start at whatever came after it.
        void Decline() {
            auto& cfg      = Settings::GetSingleton();
            cfg.tutorialsOn = false;
            cfg.Save();
            g_active = Id::kCount;
            g_step   = 0;
        }

        [[nodiscard]] bool OnActionStep() {
            if (g_active == Id::kCount) {
                return false;
            }
            return g_step < g_planCount && CurStep(StepsFor(g_active)).need != Action::kNone;
        }

        // ⚠ THE OPEN IS ONLY EVER ASKED FOR HERE, NEVER PERFORMED. OpenPopup
        // hashes against the current id stack, and the thing that most often
        // wants to move the tutorial on is a click handler buried inside a
        // PushID. Setting a flag and letting window scope do the opening is the
        // same deferral the outfit tab menu uses, and for the same reason.
        void WantCardIfReading() {
            if (!OnActionStep() && g_active != Id::kCount) {
                g_wantCard = true;
            }
        }

        void Advance() {
            if (g_step + 1 >= g_planCount) {
                Finish(false);
                return;
            }
            ++g_step;
            WantCardIfReading();
        }

    }  // namespace

    void PublishAnchor(Anchor a_anchor, const ImVec2& a_min, const ImVec2& a_max) {
        if (a_anchor == Anchor::kNone || a_anchor == Anchor::kCount) {
            return;
        }
        auto& slot = g_anchors[static_cast<std::size_t>(a_anchor)];
        slot.min   = a_min;
        slot.max   = a_max;
        slot.frame = g_frame;
    }

    void SetRequirement(Requires a_requirement, bool a_met) {
        if (a_requirement == Requires::kNone || a_requirement == Requires::kCount) {
            return;
        }
        g_facts[static_cast<std::size_t>(a_requirement)] = a_met;
    }

    void NotifyAction(Action a_action) {
        if (a_action == Action::kNone || !OnActionStep()) {
            return;
        }
        if (CurStep(StepsFor(g_active)).need != a_action) {
            return;  // the player did something else, which is allowed
        }
        const Id was = g_active;
        EditorStyle::PlayUISound("UIMenuOK");
        Advance();
        // ⚠ THE WELCOME HANDS OVER, AND ONLY WHEN ITS LAST STEP WAS SATISFIED.
        // Its final ask is "choose a page", so the player has just said where
        // they want to be, and making them arrive to nothing would waste the
        // one moment that page's tutorial is obviously wanted. This is not the
        // back-to-back wall that one-per-visit exists to stop: that was a card
        // the player dismissed followed instantly by another. Skip and No
        // thanks never reach here, because neither goes through an action.
        if (was == Id::kWelcome && g_active == Id::kCount) {
            g_firedThisVisit = false;
        }
    }

    void DrawBanner() {
        if (!OnActionStep()) {
            return;
        }
        const auto steps = StepsFor(g_active);
        // ⚠ IN THE LAYOUT, ON PURPOSE. A floating instruction would need a
        // second window, and the two calls that would take have no call sites
        // anywhere in this project. A banner costs nothing, cannot be covered
        // by a child, and the rules page already has one for the same job.
        // ⚠ THE TEXT GOES FIRST AND THE BUTTON IS PLACED FROM WHERE IT ENDED,
        // which is the third arrangement this one row has had and the reason the
        // other two were wrong. A SameLine after TextWrapped clipped Skip down
        // to "Sk", because a wrapped run takes the whole content width and
        // leaves a following item nowhere to go. Its own line fixed that and
        // cost a line the banner cannot spend. Pinning it to the right EDGE
        // fixed that and stranded it far from the sentence it belongs to, with
        // the separator cutting through it (user 2026-08-08, three rounds).
        //
        // ⚠ SO THE BUTTON'S X IS MEASURED, NEVER PREDICTED. The text is drawn
        // with a wrap limit, and GetItemRectMax then reports where the longest
        // line actually ended, which for a wrapped run is the ink rather than
        // the limit. That is the same read-it-back rule the rest of this editor
        // follows for widths, and it is what puts Skip beside the sentence at
        // any font size, any UI scale and any translation.
        const char*  skipLabel = "$FR_Tut_Skip"_T;
        const ImVec2 rowP0     = FUCK::GetCursorScreenPos();
        const float  avail     = FUCK::GetContentRegionAvail().x;
        const float  gap       = OS::ui::ItemSpacing().x;
        // ⚠ THE RESERVATION IS EXACT NOW, and the two-estimate hedge that used
        // to be here is gone with the FUCK::Button it was hedging against.
        // ChamferPanel::Button measures itself as label plus FramePadding.x
        // twice, which is the line below, so the space set aside and the
        // control that goes in it agree by construction rather than by
        // over-reserving.
        const float skipW =
            OS::ui::CalcTextWidth(skipLabel) + OS::ui::FramePadding().x * 2.0f;
        const float textW  = std::max(1.0f, avail - skipW - gap);
        // ⚠ THE BUTTON'S REAL HEIGHT, which is FLICK's widget height and not
        // ImGui's GetFrameHeight; the one-line nudge below centres the sentence
        // against the button, so the two must use the same number.
        const float frameH = OS::ChamferPanel::FrameWidgetHeight();

        // Nudged down by half the difference so a one-line instruction sits
        // level with the button beside it rather than riding its top edge.
        const float lineTop =
            rowP0.y + std::max(0.0f, (frameH - OS::ui::FontSize()) * 0.5f);
        FUCK::SetCursorScreenPos(ImVec2(rowP0.x, lineTop));
        OS::ui::PushStyleColor(ImGuiCol_Text, kHighlight);
        // ⚠⚠ HAND WRAPPED, AND PushTextWrapPos IS GONE FROM THIS ROW. The
        // reservation above was correct and the text ran straight through it
        // anyway: a field shot on 2026-08-16 shows an instruction ending
        // "...to hide that slot instea" with Skip drawn on top of the rest.
        //
        // Two ways that happens and this fixes both without needing to know
        // which. PushTextWrapPos is an OPTIONAL entry in the ABI, guarded on
        // version and on a non-null pointer, so a host that does not carry it
        // silently wraps at the WINDOW width instead of at our reservation. And
        // its argument is window-local while the cursor here was just set in
        // SCREEN space, so the sum being handed to it is only right if
        // GetCursorPos answers in the space this line assumes.
        //
        // Measuring the lines ourselves depends on neither. CalcTextWidth is
        // the same measurement every other width in this editor is read back
        // from, and the wrap is then a fact rather than a request.
        const std::vector<std::string> lines =
            WrapToWidth(FUCK::Translate(CurStep(steps).body), textW);
        float lastLineW = 0.0f;
        float y         = lineTop;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            FUCK::SetCursorScreenPos(ImVec2(rowP0.x, y));
            FUCK::TextUnformatted(lines[i].c_str());
            if (i + 1 == lines.size()) {
                lastLineW = OS::ui::CalcTextWidth(lines[i].c_str());
            }
            y += OS::ui::FontSize();
        }
        FUCK::PopStyleColor();
        // The ink of the LAST line, which is what Skip has to sit beside, and
        // the bottom of the block, which is what the separator has to clear.
        const ImVec2 textMax(rowP0.x + lastLineW, y);

        // Beside the sentence when there IS one line, and in the reserved
        // column when there is more than one. The way out has to exist: an
        // action step has no card, so without this a player who does not want
        // to do the thing has nothing to press, and a tutorial you cannot leave
        // is worse than one you never saw.
        //
        // ⚠⚠ HUGGING THE TEXT IS ONLY SAFE ON A SINGLE LINE, and doing it
        // anyway put the button through the middle of a sentence (field
        // 2026-08-16, screenshot: Skip drawn over "A card reads as one
        // sentence"). The button sits on the FIRST line's y, and the width it
        // was hugging is the LAST line's ink, so on a wrapped instruction those
        // two describe different rows. The reservation above already keeps
        // every line clear of the column, so parking it there cannot overlap
        // anything.
        const float skipX = lines.size() > 1
                                ? rowP0.x + avail - skipW
                                : std::min(textMax.x + gap, rowP0.x + avail - skipW);
        FUCK::SetCursorScreenPos(ImVec2(skipX, rowP0.y));
        // ⚠ skip.max, NOT GetItemRectMax(). ChamferPanel::Button draws its
        // label through TextAt, which submits a real item, so the last item out
        // here is the TEXT and not the button under it.
        const auto   skip    = ChamferPanel::Button(skipLabel);
        const bool   skipped = skip.clicked;
        const ImVec2 skipMax = skip.max;

        // ⚠ THE RULE CLEARS BOTH, AND OFF THEIR MEASURED RECTS. This survives
        // the conversion even though its original reason did not: the button IS
        // exactly GetFrameHeight now, but the TEXT beside it can wrap to two
        // lines and be the taller of the two, so the max is still doing work.
        FUCK::SetCursorScreenPos(ImVec2(rowP0.x, std::max(textMax.y, skipMax.y)));
        FUCK::Spacing();
        FUCK::Separator();
        if (skipped) {
            Finish(true);
        }
    }

    bool ShowRailOnly() { return g_active == Id::kWelcome; }

    void OnEditorOpened() {
        // Nothing is carried across an open. A card left up when the editor was
        // closed has no position on screen to return to, and re-asking is the
        // honest behaviour: the flag was only written when it finished.
        g_active        = Id::kCount;
        g_pending       = Id::kCount;
        g_step          = 0;
        g_planCount     = 0;
        g_wantCard       = false;
        g_cardSeen       = false;
        g_firedThisVisit = false;
        // ⚠ FORGOTTEN, NOT GUESSED. The pane on the next open is decided by
        // the editor, not by this, so claiming to know it here would cost the
        // first arrival its tutorial.
        g_paneKnown      = false;
    }

    void ResetAll() {
        auto& cfg = Settings::GetSingleton();
        // ⚠ THE MASTER SWITCH GOES BACK ON TOO. Someone pressing Replay after
        // declining has plainly changed their mind, and a Replay that clears
        // seven flags while leaving the thing that suppresses all seven off is
        // a button that does nothing at all.
        cfg.tutorialsOn = true;
        for (auto raw = static_cast<std::uint8_t>(Id::kWelcome);
             raw < static_cast<std::uint8_t>(Id::kCount); ++raw) {
            if (bool* p = FlagPtr(cfg, static_cast<Id>(raw))) {
                *p = false;
            }
        }
        cfg.Save();
        OnEditorOpened();
    }

    void Draw(EditorGate::PaneMode a_mode, bool a_pageReady) {
        // ⚠ ASKED BEFORE ANYTHING IS OPENED. IsPopupOpen with kAnyPopup is what
        // keeps a card off the top of the leave-unsaved question or the delete
        // confirmation, which are both modal. Two stacked modals is a state
        // this editor has never had to be in and this is not the feature to put
        // it there.
        const bool otherPopup =
            g_active == Id::kCount && FUCK::IsPopupOpen(nullptr, FUCK::PopupFlags::kAnyPopup);

        // Arriving on a page is what opens the door again. ⚠ Also true of the
        // FIRST frame after an open, which is what g_paneKnown is for: without
        // it the editor would land on Outfits, see no change, and never offer
        // anything at all.
        if (!g_paneKnown || a_mode != g_lastPane) {
            g_paneKnown      = true;
            g_lastPane       = a_mode;
            g_firedThisVisit = false;
        }

        if (g_active == Id::kCount && g_pending == Id::kCount && !g_firedThisVisit) {
            const auto seen = SeenMask();
            // The rail first, and only once ever. It is how you reach five of
            // the six pages, so a player who never finds it never finds them,
            // and no page tutorial can teach it because every page is behind it.
            // ⚠ THE SEND-OFF OUTRANKS THE PAGE, and only ever once, because by
            // the time it is ready every page tutorial is already seen and
            // ForPane would name one of them and fire nothing.
            const Id want =
                !TutorialPlan::Seen(seen, Id::kWelcome) ? Id::kWelcome
                : (TutorialPlan::ReadyForClosing(seen) &&
                   !TutorialPlan::Seen(seen, Id::kClosing))
                    ? Id::kClosing
                    : ForPane(a_mode);
            const bool ready =
                (want == Id::kWelcome || want == Id::kClosing) ? true : a_pageReady;
            if (want != Id::kCount &&
                TutorialPlan::ShouldFire(seen, want, EligibleCount(want), ready,
                                         otherPopup,
                                         Settings::GetSingleton().tutorialsOn)) {
                g_pending = want;
            }
        }

        if (g_pending != Id::kCount) {
            g_active        = g_pending;
            g_pending       = Id::kCount;
            g_step          = 0;
            g_firedThisVisit = true;
            g_cardSeen      = false;
            // ⚠ BUILT ONCE, HERE, AND NOT REBUILT WHILE THE TUTORIAL RUNS. The
            // plan is what g_step indexes, so rebuilding it mid-run would move
            // the player to a different card. A follower who joins after the
            // Outfits tutorial started keeps the plan it started with, which is
            // the behaviour that cannot surprise anyone.
            BuildPlan(g_active);
            WantCardIfReading();
        }

        // The only place a card is opened, for the id-stack reason
        // WantCardIfReading records.
        if (g_wantCard) {
            g_wantCard = false;
            OS::ui::OpenModal(kPopupId);
        }

        if (g_active == Id::kCount) {
            ++g_frame;
            return;
        }

        const auto steps = StepsFor(g_active);
        // Belt and braces; ShouldFire already refuses both of these. The plan
        // being empty is the same condition as the table being empty once
        // requirements are taken into account.
        if (steps.empty() || g_planCount == 0) {
            g_active = Id::kCount;
            ++g_frame;
            return;
        }
        if (g_step >= g_planCount) {
            g_step = g_planCount - 1;
        }

        // An action step has no card at all. It is waiting for the player to do
        // the thing, and a modal would be blocking the one control it is
        // pointing at. Its instruction went out with the layout, in DrawBanner,
        // and with nothing over the page the ring needs nothing cut out of it.
        if (CurStep(steps).need != Action::kNone) {
            DrawRings(CurStep(steps), Excluded{});
            ++g_frame;
            return;
        }

        // ⚠ A WIDTH, BECAUSE kAutoResize DOES NOTHING ON A POPUP. Without it
        // the card takes ImGui's default size, and text past that scrolls
        // inside the box and clips the buttons off the bottom (user 2026-08-08,
        // with a screenshot). NextModalWidth carries the measurement.
        //
        // Both terms are measured rather than picked: a share of the display so
        // it is sane on any resolution, and a multiple of the font so it stays
        // a readable line length rather than one enormous line at a large UI
        // scale. The smaller wins.
        const float display = FUCK::GetDisplaySize().x;
        OS::ui::NextModalWidth(
            std::min(display > 0.0f ? display * 0.40f : OS::ui::FontSize() * 22.0f,
                     OS::ui::FontSize() * 22.0f));

        // ⚠ AN OPAQUE PANEL, PUSHED ROUND THE WHOLE BEGIN/END PAIR. The theme's
        // popup background is part transparent, so the card was prose floating
        // over the editor with the page showing through it (user 2026-08-08).
        // A tutorial card has to read as a thing sitting in front of the
        // editor; text on a see-through rectangle reads as a rendering fault.
        //
        // ⚠ WindowBg RATHER THAN A COLOUR OF ITS OWN, at full alpha, so the
        // card is the editor's own panel colour and stays right if the theme
        // changes. Pushed around the whole pair rather than only around Begin:
        // which of the two actually reads the colour is an ImGui internal, and
        // balancing the push across both branches costs nothing.
        ImVec4 cardBg = OS::ui::StyleColor(ImGuiCol_WindowBg);
        cardBg.w      = 1.0f;
        OS::ui::PushStyleColor(ImGuiCol_PopupBg, cardBg);

        Excluded cardRect{};
        if (OS::ui::BeginModal(kPopupId)) {
            // ⚠ TAKEN INSIDE THE MODAL, WHICH IS THE ONLY PLACE IT EXISTS.
            // GetWindowPos and GetWindowSize describe the CURRENT window,
            // so this is the card here and the editor anywhere else. The
            // ring below is drawn after the card for exactly this reason:
            // it cannot cut itself around something it has not measured.
            const ImVec2 wp = FUCK::GetWindowPos();
            const ImVec2 ws = FUCK::GetWindowSize();
            cardRect.on     = true;
            cardRect.lo     = wp;
            cardRect.hi     = ImVec2(wp.x + ws.x, wp.y + ws.y);
            g_cardSeen      = true;
            FUCK::TextWrapped("%s", FUCK::Translate(CurStep(steps).body));
            FUCK::Spacing();
            FUCK::TextDisabled("%s", OS::ui::FormatF("$FR_Tut_Step"_T,
                                                     static_cast<int>(g_step) + 1,
                                                     static_cast<int>(g_planCount))
                                         .c_str());
            FUCK::Spacing();

            const bool  last      = g_step + 1 >= g_planCount;
            const char* backLabel = "$FR_Tut_Back"_T;
            const char* nextLabel = last ? "$FR_Tut_Done"_T : "$FR_Tut_Next"_T;
            const char* skipLabel = "$FR_Tut_Skip"_T;

            // ⚠ THE ASKING CARD GETS TWO BUTTONS, NOT THE USUAL THREE. Back is
            // meaningless on the first card and Skip would be a third answer to
            // a yes-or-no question, sitting between them and meaning neither.
            // A question deserves its own answers.
            if (CurStep(steps).asks) {
                const char* yesLabel = "$FR_Tut_Yes"_T;
                const char* noLabel  = "$FR_Tut_No"_T;
                // ⚠ CentreTwoButtons IS EXACTLY RIGHT NOW RATHER THAN NEARLY.
                // Its own note says it estimates on FramePadding.x and is only
                // correct while that happens to equal 8 * scale, which is what
                // FUCK::Button used. ChamferPanel::Button measures by the same
                // formula the centring does, so the estimate and the buttons
                // are one arithmetic.
                OS::ui::CentreTwoButtons(yesLabel, noLabel);
                if (ChamferPanel::Button(yesLabel).clicked) {
                    Advance();
                    if (g_active == Id::kCount || OnActionStep()) {
                        FUCK::CloseCurrentPopup();
                    }
                }
                FUCK::SameLine();
                if (ChamferPanel::Button(noLabel).clicked) {
                    Decline();
                    FUCK::CloseCurrentPopup();
                }
                FUCK::EndPopup();
                FUCK::PopStyleColor();
                DrawRings(CurStep(steps), cardRect);
                ++g_frame;
                return;
            }

            OS::ui::CentreThreeButtons(backLabel, nextLabel, skipLabel);
            // Drawn rather than hidden on the first card, so the row does not
            // change width between steps and the buttons stay where the hand
            // left them.
            // ⚠ THE CONDITION IS PASSED AS WELL AS WRAPPED. ImGui's disabling
            // refuses the click but does not reach ChamferPanel's own draw
            // calls, so without the flag Back would go dead while still looking
            // live on the first card.
            const bool backDisabled = g_step == 0;
            FUCK::BeginDisabled(backDisabled);
            const auto back = ChamferPanel::Button(backLabel, 0.0f, backDisabled);
            FUCK::EndDisabled();
            if (back.clicked) {
                --g_step;
            }
            FUCK::SameLine();
            if (ChamferPanel::Button(nextLabel).clicked) {
                Advance();
                // ⚠ CLOSED WHENEVER THE NEXT STEP IS NOT A CARD, which is the
                // finish AND the handover to an action step. Advance decides
                // which; this only has to get out of the way, because a modal
                // left standing would be blocking the very control the next
                // step is about to ask the player to click.
                if (g_active == Id::kCount || OnActionStep()) {
                    FUCK::CloseCurrentPopup();
                }
            }
            FUCK::SameLine();
            // ⚠ SKIP COUNTS AS DONE (user 2026-08-08), through the same Finish
            // the last card uses. A tutorial that returns after being dismissed
            // is worse than one that never ran.
            if (ChamferPanel::Button(skipLabel).clicked) {
                Finish(true);
                FUCK::CloseCurrentPopup();
            }
            FUCK::EndPopup();
        } else if (g_cardSeen) {
            // ⚠ CLOSED FROM UNDER US, WHICH IS NOT THE SAME AS FINISHED. A modal
            // can go away without either button being pressed, and marking it
            // done here would burn the one showing on a stray Escape. The card
            // is dropped and the tutorial is still owed.
            //
            // ⚠ AND ONLY ONCE IT HAS BEEN SEEN. A begin that has never once
            // matched is not a dismissal, it is a fault, and treating it as a
            // dismissal is how a single mismatched id stack turned into every
            // tutorial in the mod silently declining to appear. Asking again
            // next frame costs nothing and heals.
            g_active  = Id::kCount;
            g_step    = 0;
            g_cardSeen = false;
        }
        // Popped through FUCK:: rather than OS::ui::, which wraps only the
        // PUSH. The shift the wrapper exists to correct is on the index, and a
        // pop takes none.
        FUCK::PopStyleColor();

        // ⚠ AFTER THE CARD, AND THAT IS THE FIX. The screen list draws above
        // every window, so a ring submitted before the card still landed on top
        // of it, and a ring round a large region put its edge through the middle
        // of the card. Drawing it here means the card's own rectangle is known
        // and can be subtracted, in the same frame, with no lag.
        DrawRings(CurStep(steps), cardRect);

        // ⚠ TICKED AT THE END, ON EVERY PATH. The publishes happen earlier in
        // the frame, while the pages draw, so they stamp the value this
        // function is still comparing against. Advancing it here is what makes
        // last frame's rect stale rather than merely old.
        ++g_frame;
    }

}  // namespace OS::Tutorial
