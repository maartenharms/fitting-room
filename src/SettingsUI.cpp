#include "PCH.h"

#include "SettingsUI.h"

#include "BuildChannel.h"
#include "CbpcArmorClass.h"  // SupportState: the bounce slider greys itself
#include "ChamferPanel.h"  // every button in here is one widget height
#include "DyeTexture.h"     // BlendFromName and the blends a player may pick
#include "DyeTick.h"        // ApplySettings, so the tick interval takes on commit
#include "DyeUnlockCard.h"  // ApplySettings, so the announce switch takes on commit
#include "EditorGate.h"
#include "EditorUI.h"  // ForgetMixedColours, on the playstyle switch
#include "Tutorial.h"  // ResetAll, behind the replay button
#include "InputListener.h"
#include "LoreModule.h"
#include "EditorStyle.h"    // PlayUISound, the rebuild button's click cue
#include "FramePolicy.h"   // the corner setting maps to the wire value here
#include "FuckCompat.h"    // ui::TextDisabledWrapped, the greyed-out reason line
#include "OutfitSession.h"  // to re-dress on the settings that change what renders
#include "Persistence.h"  // SyncSharedDyeUnlocks, the moment sharing is switched on
#include "PreviewCache.h"      // ForgetAll, the in-memory half of that button
#include "PreviewDiskCache.h"  // RebuildAll, behind the rebuild-previews button
#include "Settings.h"
#include "SharedCollection.h"  // Forget, the gear half of the same button
#include "SharedDyeUnlocks.h"  // Forget, behind the reset-shared-progress button

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h's INI callback typedefs

#include "FUCK_API.h"

#include <cstdint>
#include <iterator>  // std::size
#include <vector>

namespace {
    // Tooltip for the item just drawn (shown only on hover) - FLICK style.
    void Tip(const char* a_desc) {
        if (FUCK::IsItemHovered()) {
            FUCK::SetTooltip(a_desc);
        }
    }

    // ⚠ THE SAME TOOLTIP FOR A CHAMFERED BUTTON, WHICH HAS TO BE TOLD. Its
    // label goes through OS::ui::TextAt and that submits a real item, so the
    // version above would be asking about whatever the button drew last rather
    // than about the button. Every converted site in this editor carries the
    // same rule; this overload is so seven of them do not each spell it out.
    void Tip(bool a_hovered, const char* a_desc) {
        if (a_hovered) {
            FUCK::SetTooltip(a_desc);
        }
    }

    // One bindable-key dropdown. Returns whether the binding changed, so the
    // caller can fold it into its dirty flag and save.
    //
    // ⚠ The list is DELIBERATELY curated (EditorGate::kBindableKeys): no WASD,
    // no Z/X/M, no number row - those are movement, vanilla menus and the
    // quick-slot keys, and offering them invites a binding that fights the
    // game. "(unbound)" is first so a key can always be given back.
    //
    // a_apply pushes the new value into InputListener's cached copy, which is
    // what the input sink actually compares against; without it a rebind would
    // not take until the next load. Persisting to the INI is the caller's job.
    template <class Fn>
    bool KeyCombo(const char* a_label, std::uint32_t& a_dik, Fn&& a_apply) {
        // FUCK::Combo is the array form (no BeginCombo), so gather the names
        // and map the current value to its index.
        constexpr int kCount = static_cast<int>(std::size(OS::EditorGate::kBindableKeys));
        std::vector<const char*> names;
        names.reserve(kCount);
        int current = 0;
        for (int i = 0; i < kCount; ++i) {
            names.push_back(OS::EditorGate::kBindableKeys[i].name);
            if (OS::EditorGate::kBindableKeys[i].dik == a_dik) {
                current = i;
            }
        }
        if (!FUCK::Combo(a_label, &current, names.data(), kCount)) {
            return false;
        }
        const std::uint32_t dik = OS::EditorGate::kBindableKeys[current].dik;
        if (dik == a_dik) {
            return false;  // reselecting the same entry is not a change
        }
        a_dik = dik;
        a_apply(dik);
        return true;
    }

}  // namespace

namespace OS::SettingsUI {

    // The panel body. Edits the Settings singleton in place; a single Save() at
    // the end persists to OutfitSlots.ini when anything changed. Outfit Slots
    // reads settings at editor-open / apply-time, so edits take effect the next
    // time the editor opens.
    void DrawPanel() {
        auto& cfg   = OS::Settings::GetSingleton();
        bool  dirty = false;
        const ImVec4 kWarn{ 0.95f, 0.75f, 0.25f, 1.0f };

        // ⚠ TABS AS OF 2026-08-14 (user), matching Menu Studio's panel. This
        // had grown to six stacked sections in one scroll, where reaching the
        // dye half meant scrolling past the whole lore economy. WHICH settings
        // exist did not change, only how many are on screen at once.
        //
        // ⚠ THE SECTION HEADERS BECAME THE TAB LABELS, so the strings are the
        // ones already in the table and no translation lost its text. Setup is
        // the exception: Compatibility is one checkbox and Controls is two key
        // combos, so they share a tab and keep their own separators inside it.
        //
        // ⚠ A HIDDEN TAB DOES NOT DRAW, which is the point and also the one
        // thing to watch: every side effect in this panel already sits inside a
        // button or a checkbox handler, so nothing runs per frame that a hidden
        // tab would stop running. Audited before the split rather than after.
        if (FUCK::BeginTabBar("##FittingRoom")) {
            if (FUCK::BeginTabItem("$FR_Set_Playstyle"_T)) {
                // ⚠ EVERY PLAYSTYLE FIELD MUST BE SET BY BOTH BUTTONS. A field one
                // button sets and the other leaves alone is not "unchanged", it is
                // whatever the previous playstyle left behind - so the preset silently
                // means different things depending on what you clicked before.
                // bCollectionOnly was exactly that: neither button touched it, it
                // defaults true, and Free-form therefore still hid every look the
                // player had not owned - the opposite of free-form.
                //
                // ⚠⚠ AND ONE OF THESE FIELDS CHANGES WHAT IS ON SCREEN, WHICH NONE OF
                // THE OTHERS DO. bRequireWornForStyles decides whether a style renders
                // at all, and the checkbox further down has always paired it with a
                // refresh for a field-reported reason: a helmet transmog with no real
                // helmet under it SURVIVED the toggle and only vanished once that slot
                // was touched (2026-08-06), because the biped was stale. A preset that
                // set the flag and skipped the rebuild would reproduce that exactly,
                // with the player blaming the preset rather than a stale biped. So the
                // old value is taken here and the refresh is issued below if it moved.
                const bool wornBefore = cfg.requireWornForStyles;
                const auto loreBtn = ChamferPanel::Button("$FR_Set_PlaystyleLore"_T);
                if (loreBtn.clicked) {
                    // ⚠ CHARGE, NOT GOLD, and ONLY from this button. Lore-friendly's
                    // default cost is the stone (OS-129), but that is a default for
                    // someone ASKING for the preset, never a migration: Settings::Load
                    // keeps every existing save on whatever bUseGold said, so nobody is
                    // moved onto an empty stone by updating.
                    //
                    // ⚠ AND ONLY WHILE THE STONE EXISTS. See
                    // EditorGate::LorePresetCostMode: without the plugin the charge
                    // meter describes an item the player cannot own or buy, so the
                    // preset bills the same slots in gold instead and the line under
                    // the buttons says so. The ESP ships on both installer paths now,
                    // so this is the answer for an older install or a deleted plugin
                    // rather than for a choice anyone makes today.
                    cfg.costMode = OS::EditorGate::LorePresetCostMode(
                        OS::LoreModule::Available());
                    cfg.requireSeamstone = true;
                    cfg.collectionOnly   = true;  // earn the look before you can wear it
                    cfg.dyeUnlocks       = true;  // and earn the colour before you can pick it
                    // ⚠ THE SAME SENTENCE AS THE OTHER TWO, ONE STEP EARLIER (user
                    // 2026-08-11). Lore-friendly is "earn it, then wear it": the
                    // collection filter says you have to have owned the look, the
                    // colour gate says you have to have earned the dye, and this says
                    // you have to actually be carrying the gear the look is painted
                    // over. Dressing out of nothing is the free-form half of it.
                    cfg.requireWornForStyles = true;
                    // ⚠ THE COLOUR GATE HAS A SIDE DOOR AND THIS IS IT. dyeUnlocks locks
                    // the swatch grid, never the Recent strip, so every colour mixed
                    // free-hand in free-form was still one click from the brush the
                    // moment lore-friendly came back on. See EditorUI::ForgetMixedColours.
                    OS::EditorUI::ForgetMixedColours();
                    dirty                = true;
                }
                Tip(loreBtn.hovered, "$FR_Set_PlaystyleLoreTip"_T);
                FUCK::SameLine(0.0f, 8.0f);
                const auto freeBtn = ChamferPanel::Button("$FR_Set_PlaystyleFree"_T);
                if (freeBtn.clicked) {
                    cfg.costMode         = OS::CostMode::kFree;
                    cfg.requireSeamstone = false;
                    cfg.collectionOnly   = false;  // everything installed is available
                    // The colour axis of the same sentence. A Free-form preset that
                    // left two thirds of the palette locked would not be free-form, and
                    // the rule above is that a field ONE button sets is a field the
                    // other has to set too. Nothing is lost by flipping it either way:
                    // promotion runs whatever this says, so a character keeps earning
                    // colours with the gate off and they are waiting when it goes back on.
                    cfg.dyeUnlocks       = false;
                    // The wardrobe half: wear what you like over whatever you have on,
                    // including nothing. Set by both buttons on the rule above, and
                    // this direction is the safe one - it can only make MORE of an
                    // existing outfit render, never less.
                    cfg.requireWornForStyles = false;
                    // Both buttons, for the reason the ⚠ above this block gives: a
                    // playstyle field one button touches and the other does not stops
                    // meaning one thing. Nothing is lost going this way either - a
                    // free-form player can mix any colour back in one click.
                    OS::EditorUI::ForgetMixedColours();
                    dirty                = true;
                }
                Tip(freeBtn.hovered, "$FR_Set_PlaystyleFreeTip"_T);
                // ⚠ AFTER BOTH BUTTONS AND ON THE VALUE, NOT ON WHICH ONE WAS PRESSED.
                // Pressing the preset you are already on changes nothing and must not
                // cost a character rebuild, and either button can be the one that moves
                // this flag. See the note above the buttons for why the rebuild is not
                // optional when it does move.
                if (cfg.requireWornForStyles != wornBefore) {
                    OutfitSession::RequestRefresh();
                    OutfitSession::RequestRefreshLoadedNpcs();
                }
                FUCK::TextDisabled("%s", "$FR_Set_PlaystyleNote"_T);
                // ⚠ AT THE DECISION, NOT AFTER IT. This is the one thing the two
                // buttons cannot deliver, so it belongs where the player is choosing
                // between them rather than in a message that appears once they have
                // already pressed one. Shown only when the plugin is genuinely absent,
                // which on a current install is never.
                if (!OS::LoreModule::Available()) {
                    FUCK::TextColored(kWarn, "%s", "$FR_Set_PlaystyleNoStone"_T);
                }

                // ⚠ NOT A CHECKBOX. There is no state here to leave switched on: the
                // tutorials are seven separate flags, and a tick box would have to
                // answer "are they all done" and then mean something different the
                // moment one page had been visited and another had not. A button does
                // one thing and says so.
                //
                // Tutorial::ResetAll writes the INI itself, so this deliberately does
                // NOT set dirty: the panel's own save would be a second write of the
                // same file for no reason.
                const auto replayBtn = ChamferPanel::Button("$FR_Set_TutorialReplay"_T);
                if (replayBtn.clicked) {
                    OS::Tutorial::ResetAll();
                }
                Tip(replayBtn.hovered, "$FR_Set_TutorialReplayTip"_T);
                // ⚠ THE SWITCH IS SHOWN AS WELL AS THE BUTTON, because the first card
                // can turn it off and a setting a player changed somewhere else should
                // be visible where settings live. Replay turns it back on by itself, so
                // this is for reading the answer and for changing your mind without
                // sitting through the cards again.
                // ⚠ THE DEFAULTS, like every other checkbox on this panel. It was the
                // only one in the file passing alignFar/labelLeft as false, which put
                // its box on the LEFT with the label beside it while all sixteen others
                // put the label on the left and the box out at the panel's right edge -
                // so the one switch a player comes here to read sat differently from
                // its neighbours. Nothing asked for that: it arrived with the first
                // tutorial card and the commit that added it says nothing about
                // alignment.
                if (FUCK::Checkbox("$FR_Set_TutorialsOn"_T, &cfg.tutorialsOn)) {
                    dirty = true;
                }
                Tip("$FR_Set_TutorialsOnTip"_T);

                // ⚠⚠ IT IS IN THE PLAYSTYLE TAB AND THE TWO PRESET BUTTONS ABOVE
                // DELIBERATELY DO NOT TOUCH IT (user 2026-08-14). That is the rule
                // those buttons follow rather than an exception to it: a field one
                // sets and the other leaves alone stops meaning one thing, and this
                // is not a playstyle. It is a statement about whether characters
                // share progression at all, and it survives switching presets.
                //
                // ⚠ SO THE PROXIMITY IS THE HAZARD. Sitting under Lore-friendly and
                // Free-form invites "the preset will set this", and it will not. The
                // separator below says so on screen rather than only here.
                FUCK::SeparatorText("$FR_Set_SharedHdr"_T);
                FUCK::TextDisabled("%s", "$FR_Set_SharedNote"_T);
                // ---- shared across characters (Playstyle, 2026-08-14) ----
                //
                // ⚠ IT LIVED UNDER Dye AND ITS OWN COMMENT DEFENDED THAT: the gear
                // half sat beside the colour half because the panel groups by
                // subject, and splitting one decision across two headings is worse
                // than a heading that fits loosely. That held while the panel was one
                // scroll. With tabs it stopped holding, because a TAB is a claim about
                // what is inside it, and "the looks your characters share" read as
                // wrong under Dye in a way it never did under a separator (user
                // 2026-08-14, "SHARE gear is in the category dye which is slightly
                // jarring").
                //
                // ⚠⚠ THE WHOLE BLOCK MOVED, NOT THE GEAR HALF. Both checkboxes and
                // the Forget button are one feature: that button is the ONLY eraser
                // either file has and it deletes BOTH. Moving one mark to another tab
                // and leaving the eraser behind is exactly the split
                // a-mark-and-its-eraser-must-ask-one-question exists to forbid.
                // Sharing progress between characters is a setup question anyway.
                // Colours earned on one character offered to all of them (OS-198).
                //
                // ⚠ NOT GATED ON dyeUnlocks ABOVE, and it reads like it should be.
                // Promotion runs whatever that setting says, so a character earns
                // colours with the gate off and they are waiting when it goes on. A
                // player who plays free-form now and lore-friendly later would
                // otherwise have shared nothing from the free-form run, which is the
                // same asymmetry dyeUnlocks' own header already argues for.
                //
                // ⚠ NEITHER PLAYSTYLE BUTTON TOUCHES IT, and that IS the rule the two
                // buttons follow rather than an exception to it: a field one sets and
                // the other does not stops meaning one thing. This is not a playstyle,
                // it is a statement about whether characters share progression at all,
                // and it survives switching between the two presets.
                // ⚠⚠ TURNING IT ON RECONCILES IMMEDIATELY, and it did not until
                // 2026-08-11. The read half ran at kNewGame and kPostLoadGame only, so
                // ticking this box did NOTHING for the character already loaded: no
                // colours came in, none went out, and nothing said so. A field test
                // ticked it, earned a colour, checked another character and reported
                // sharing broken. It was not broken; it had not been asked yet.
                //
                // ⚠ MARSHALLED. This panel draws on FUCK's present thread and the sync
                // takes DyeUnlocks' lock and touches the filesystem latch, both of
                // which are main-thread contracts. The same deferral ImGuiOverlay's
                // RequestOpen uses, and for the same reason.
                //
                // ⚠ ON THE RISING EDGE ONLY. Turning it OFF publishes nothing and takes
                // nothing back: the file is add-only by design, which its own tooltip
                // says, so there is nothing to undo and no reason to touch the disk.
                {
                    const bool sharedWas = cfg.dyeUnlocksShared;
                    dirty |= FUCK::Checkbox("$FR_Set_DyeUnlocksShared"_T, &cfg.dyeUnlocksShared);
                    Tip("$FR_Set_DyeUnlocksSharedTip"_T);
                    if (!sharedWas && cfg.dyeUnlocksShared) {
                        if (auto* task = SKSE::GetTaskInterface()) {
                            task->AddTask([] {
                                OS::Persistence::SyncSharedDyeUnlocks("the setting being "
                                                                      "switched on");
                            });
                        }
                    }
                }

                // The gear half of the same idea (user 2026-08-11). Same shape, same
                // add-only file, same immediate reconcile on the rising edge.
                //
                // ⚠ IT SITS UNDER Dye BECAUSE THAT IS WHERE ITS TWIN IS, and the panel
                // groups by subject rather than by INI section, which bDyeUnlocks'
                // own note already establishes. Splitting the pair across two headings
                // to satisfy the sections would put the two halves of one decision on
                // opposite ends of the panel.
                {
                    const bool sharedWas = cfg.collectionShared;
                    dirty |= FUCK::Checkbox("$FR_Set_CollectionShared"_T, &cfg.collectionShared);
                    Tip("$FR_Set_CollectionSharedTip"_T);
                    if (!sharedWas && cfg.collectionShared) {
                        if (auto* task = SKSE::GetTaskInterface()) {
                            task->AddTask([] {
                                OS::Persistence::SyncSharedCollection("the setting being "
                                                                      "switched on");
                            });
                        }
                    }
                }

                // ⚠ NO SYNC ON THE EDGE, UNLIKE THE TWO ABOVE, and that is the
                // whole difference between this setting and them. Those two
                // merge a pool into the character standing here, so flipping
                // them on has something to do right now. This one decides what
                // the NEXT NEW CHARACTER starts with and can do nothing at all
                // to the one already loaded, whose library is their own save's.
                // A sync here would be a write looking for a reason.
                dirty |= FUCK::Checkbox("$FR_Set_OutfitsShared"_T, &cfg.outfitsShared);
                Tip("$FR_Set_OutfitsSharedTip"_T);

                // ---- and the way back out of it ---------------------------------
                //
                // ⚠⚠ THE FILE IS ADD-ONLY AND THIS IS ITS ONLY ERASER (user
                // 2026-08-11). Nothing else in the feature ever removes an id, which
                // its own tooltip promises, so before this the answer to "I want the
                // account to start over" was to go and find the JSON by hand.
                //
                // ⚠ IT DOES NOT TOUCH A SINGLE SAVE, and the label has to say so.
                // Every character keeps the colours in their own co-save, because those
                // were earned and are theirs. What goes is the pool a NEW character
                // would be handed. Anyone who reads this as "reset my progress" and
                // finds their current character untouched was mis-sold it by the
                // wording, not by the code.
                //
                // ⚠ TWO PRESSES, AND NOT BECAUSE A MODAL WAS TOO MUCH WORK. This panel
                // is drawn by FLICK's sidebar as well as by the editor's gear popup,
                // and OS::ui's modal helpers are the editor's; a modal opened from the
                // sidebar has no editor id stack to hash against. Arming is the answer
                // that works from both, and an irreversible delete has to cost more
                // than a stray click on a row of checkboxes.
                {
                    static bool s_armed = false;
                    if (!s_armed) {
                        const auto armBtn = ChamferPanel::Button("$FR_Set_ForgetShared"_T);
                        if (armBtn.clicked) {
                            s_armed = true;
                        }
                        Tip(armBtn.hovered, "$FR_Set_ForgetSharedTip"_T);
                    } else {
                        const auto confirmBtn =
                            ChamferPanel::Button("$FR_Set_ForgetSharedConfirm"_T);
                        if (confirmBtn.clicked) {
                            s_armed = false;
                            // Marshalled for the sync's reason: the latch this clears
                            // is main-thread state.
                            // ⚠ BOTH FILES, because the button says "shared progress"
                            // and a player who presses it having shared gear as well
                            // would otherwise find half of it still there. Two calls
                            // rather than one that knows about both: each module owns
                            // its own file and its own latch.
                            if (auto* task = SKSE::GetTaskInterface()) {
                                task->AddTask([] {
                                    const bool dyes  = OS::SharedDyeUnlocks::Forget();
                                    const bool looks = OS::SharedCollection::Forget();
                                    if (dyes && looks) {
                                        spdlog::info("Shared progress: both files are gone. Every "
                                                     "character keeps what is in their own save; a "
                                                     "new one now starts from nothing.");
                                    } else {
                                        // Named separately: one surviving is a different
                                        // problem from both surviving, and the player
                                        // will see half their progress still shared.
                                        spdlog::error("Shared progress: could not delete {}{}{}. "
                                                      "What is left is still on disk and still "
                                                      "being shared.",
                                                      dyes ? "" : "the colour file",
                                                      (!dyes && !looks) ? " and " : "",
                                                      looks ? "" : "the look file");
                                    }
                                });
                            }
                            EditorStyle::PlayUISound("UIMenuOK");
                        }
                        Tip(confirmBtn.hovered, "$FR_Set_ForgetSharedConfirmTip"_T);
                        FUCK::SameLine();
                        if (ChamferPanel::Button("$FR_Set_ForgetSharedCancel"_T).clicked) {
                            s_armed = false;
                            EditorStyle::PlayUISound("UIMenuCancel");
                        }
                    }
                }

                FUCK::EndTabItem();
            }
            if (FUCK::BeginTabItem("$FR_Set_Lore"_T)) {
                // ⚠ ON THE SEAMSTONE TAB RATHER THAN THE EDITOR ONE. The
                // transition is the stone doing something, not a piece of
                // interface: it plays in the world, from the hotkey, with the
                // editor closed. The Editor tab is where the panel's own
                // behaviour lives and this would read as a UI option there.
                dirty |= FUCK::Checkbox("Seamstone transition", &cfg.requipFlourish);
                Tip("A flash covers the swap when you change outfit yourself. The old "
                    "pieces fade out in it and the new ones fade in.\n\nOutfit rules "
                    "that dress you automatically never play it");
                if (cfg.requipFlourish) {
                    dirty |= FUCK::Checkbox("Light every piece", &cfg.requipFlashAll);
                    Tip("On, everything you are wearing lights up. Off, only the pieces "
                        "that actually changed do.\n\nEither way the transition only plays "
                        "when something really changed");
                    dirty |= FUCK::Checkbox("Magic effect on the body", &cfg.requipAura);
                    Tip("The glow that washes over you while the outfit changes.\n\nOff "
                        "leaves the garments lighting on their own");
                    dirty |= FUCK::Checkbox("Transition sound", &cfg.requipSoundOn);
                    Tip("A magic sound when you change outfit.\n\nIt plays over Skyrim's "
                        "own equipment sounds rather than replacing them, because those "
                        "belong to the game rather than to Fitting Room. Pick which sound "
                        "with sRequipSoundId in the INI");
                }
                // ⚠ ONE CONTROL WITH THREE ANSWERS, NOT TWO TICK BOXES. The cost is a
                // single decision, and a pair of independent checkboxes would let a
                // player select both currencies and only find out at Apply which one
                // the code picked. FUCK has no RadioButton, so this is the array Combo.
                //
                // ⚠ THE ORDER IS THE ENUM'S ORDER, and the index IS the mode value.
                // Reordering these labels silently reassigns every player's setting on
                // the next panel save, so the static_asserts hold the two together.
                {
                    static const char* const kCostLabels[] = { "Nothing", "Gold",
                                                               "Seamstone charge" };
                    static_assert(static_cast<int>(OS::CostMode::kFree) == 0);
                    static_assert(static_cast<int>(OS::CostMode::kGold) == 1);
                    static_assert(static_cast<int>(OS::CostMode::kCharge) == 2);
                    int m = static_cast<int>(cfg.costMode);
                    if (FUCK::Combo("Applying an outfit costs", &m, kCostLabels,
                                    IM_ARRAYSIZE(kCostLabels))) {
                        cfg.costMode = OS::CostModeFrom(static_cast<long>(m));
                        dirty        = true;
                    }
                    Tip("Gold is charged per styled slot when you press Apply. Seamstone "
                        "charge works the same way but drains the stone instead of your "
                        "purse, and you refill it with filled soul gems.\n\nThe charge is "
                        "kept per character in your save, so it works whether or not you "
                        "installed the lore plugin that adds the stone itself.\n\nWith "
                        "\"Nothing\" selected there is no Apply button and the editor "
                        "saves as you go");

                    // Only the selected mode's rate, so the panel never shows a number
                    // that is doing nothing.
                    if (cfg.costMode == OS::CostMode::kGold) {
                        int gold = static_cast<int>(cfg.goldPerSlot);
                        if (FUCK::SliderInt("$FR_Set_GoldPerSlot"_T, &gold, 0, 500)) {
                            cfg.goldPerSlot = static_cast<std::uint32_t>(gold < 0 ? 0 : gold);
                            dirty           = true;
                        }
                        int goldDye = static_cast<int>(cfg.goldPerDye);
                        if (FUCK::SliderInt("Gold per dyed channel", &goldDye, 0, 500)) {
                            cfg.goldPerDye = static_cast<std::uint32_t>(goldDye < 0 ? 0 : goldDye);
                            dirty          = true;
                        }
                        Tip("Charged per colour you changed, on top of the slots."
                            "\n\nStarts at 0 so this does not reprice a game you were "
                            "already playing. This is not the price of unlocking a "
                            "colour, which is its own setting under Dye");
                    } else if (cfg.costMode == OS::CostMode::kCharge) {
                        int per = static_cast<int>(cfg.chargePerSlot);
                        if (FUCK::SliderInt("Charge per styled slot", &per, 0, 500)) {
                            cfg.chargePerSlot = static_cast<std::uint32_t>(per < 0 ? 0 : per);
                            dirty             = true;
                        }
                        int perDye = static_cast<int>(cfg.chargePerDye);
                        if (FUCK::SliderInt("Charge per dyed channel", &perDye, 0, 500)) {
                            cfg.chargePerDye = static_cast<std::uint32_t>(perDye < 0 ? 0 : perDye);
                            dirty            = true;
                        }
                        Tip("Charged per colour you changed, on top of the slots. "
                            "Dyeing a whole eight-piece armour costs eight of these."
                            "\n\nThis is not the price of unlocking a colour, which is "
                            "its own setting under Dye");
                        int cap = static_cast<int>(cfg.seamstoneCapacity);
                        if (FUCK::SliderInt("Seamstone capacity", &cap, 100, 20000)) {
                            cfg.seamstoneCapacity = static_cast<std::uint32_t>(cap < 1 ? 1 : cap);
                            dirty                 = true;
                        }
                        Tip("How much the stone holds when full.\n\nLowering it leaves a "
                            "stone that is already fuller alone rather than draining it "
                            "down to the new ceiling");
                    }
                }
                // Plain literal, not a "$FR_..."_T key. Adding a key means editing
                // dist/Interface/Translations, and this branch's copy is behind the
                // live one - shipping ours would delete keys another branch added, so
                // the slot deliberately carries no translations file at all. The
                // project's newest controls (EditorUI's "Delete Export", the whole
                // Rules tab) are plain-literal for the same reason.
                // ⚠ THIS ONE HAS TO RE-DRESS, NOT JUST SAVE. It changes what the
                // styling pass is allowed to render, and that pass only runs on a biped
                // rebuild, so flipping it left the character exactly as they were until
                // something else happened to rebuild them. The field report was a helmet
                // transmog with no real helmet under it surviving the toggle and only
                // vanishing once that slot was touched (2026-08-06), which is the
                // setting working correctly on a stale biped.
                //
                // Player AND loaded NPCs, because the rule is not player-only: a
                // follower dressed out of nothing is exactly what this switch is for.
                if (FUCK::Checkbox("Transmog needs real gear underneath",
                                   &cfg.requireWornForStyles)) {
                    dirty = true;
                    OutfitSession::RequestRefresh();
                    OutfitSession::RequestRefreshLoadedNpcs();
                }
                Tip("A style only shows if you are wearing real gear under some part of "
                    "it, rather than dressing you out of nothing. Shields have always "
                    "worked this way.\n\nOne piece can cover several slots at once. "
                    "Wearing gear under any one of them is enough, and the piece still "
                    "covers the others.\n\nInside the fitting room the whole look shows "
                    "while you browse, and the page says which pieces have nothing "
                    "under them.\n\nTurning this on can stop parts of existing "
                    "outfits from showing");

                dirty |= FUCK::Checkbox("Imported sets hide the gear they do not cover",
                                        &cfg.hideEquippedOnImport);
                Tip("A preset only says what to wear on the slots it covers. Without this, "
                    "the slots it says nothing about keep showing what you really have on, "
                    "so importing a set can leave you wearing your own helmet with it.\n\n"
                    "Only slots you actually have gear on are hidden, and never the ones "
                    "the set dresses");

                dirty |= FUCK::Checkbox("$FR_Set_RequireSeamstone"_T,
                                        &cfg.requireSeamstone);
                Tip("$FR_Set_RequireSeamstoneTip"_T);
                // The requirement is AND-ed with the ESP being loaded, so without it
                // this switch silently does nothing - say so rather than let someone
                // wonder why their hotkey still opens the editor.
                if (cfg.requireSeamstone && !OS::LoreModule::Available()) {
                    FUCK::TextColored(kWarn, "%s", "$FR_Set_SeamstoneMissing"_T);
                }

                FUCK::EndTabItem();
            }
            if (FUCK::BeginTabItem("$FR_Set_Editor"_T)) {
                dirty |= FUCK::Checkbox("$FR_Set_HoverPreview"_T, &cfg.hoverPreview);
                // ⚠ TWO CHECKBOXES, NOT ONE WITH A WIDER MEANING (user 2026-08-11).
                // Which one is live depends on the Cards toggle on the filter row, not
                // on which list is open; EditorUI::HoverPreviewOn is the one place that
                // decides, and nothing else may ask.
                //
                // ⚠ NOT INI-ONLY. It defaults OFF, and a setting that is off out of the
                // box and reachable only by editing a file is a feature nobody finds -
                // which is the same complaint the shared-colours checkbox answered.
                dirty |= FUCK::Checkbox("$FR_Set_HoverPreviewCards"_T, &cfg.hoverPreviewCards);
                Tip("$FR_Set_HoverPreviewCardsTip"_T);
                // The escape hatch the persisted-failure store needs from day one: a
                // NIF fixed on disk stays blocked until its failure entry goes, and
                // "my previews are blank and stay blank" must have a one-click answer.
                const auto rebuildBtn = ChamferPanel::Button("$FR_Set_RebuildPreviews"_T);
                if (rebuildBtn.clicked) {
                    // ⚠⚠ BOTH HALVES, AND FOR A WHILE IT WAS ONLY THE FIRST.
                    // Wiping the disk leaves every card already decoded in
                    // memory drawing exactly what the player pressed this to be
                    // rid of, so the button did nothing anyone could see until
                    // the next launch (field 2026-08-15: "rebuilt previews,
                    // some eyes still invisible in the cards", with an empty
                    // cache directory and a drain reporting built=0 loaded=51).
                    OS::PreviewDiskCache::RebuildAll();
                    OS::PreviewCache::ForgetAll();
                    EditorStyle::PlayUISound("UIMenuOK");
                }
                Tip(rebuildBtn.hovered, "$FR_Set_RebuildPreviewsTip"_T);
                // ⚠⚠ THE ONLY PLACE THE CARD / LIST SWITCH IS DRAWN NOW (user
                // 2026-08-16: "move the card / list toggle to the FLICK settings").
                // The browser's filter row carried a second view of this same bool
                // until then; the row is gone and this is what it moved to, so a
                // rename or a retitle here has nowhere else to drift to.
                //
                // ⚠ IT WAS HERE FIRST BECAUSE THE PRESETS PANE HAS NO FILTER ROW. Its own
                // header is a source tab bar and a search field that was just cleared
                // of a button to give it back its row (user 2026-08-12), so the toggle
                // that turns preset cards on had nowhere inline to go. The card SIZE
                // slider is already here, and the pair that governs cards reads better
                // in one place than split across a pane that cannot show half of it.
                dirty |= FUCK::Checkbox("$FR_GridView"_T, &cfg.previewGrid);
                Tip("$FR_GridViewTip"_T);
                // Card size, in font-size units so it follows the UI scale rather
                // than fighting it. Cached thumbnails stay valid: the texture is
                // rendered at its own resolution and the card only decides how big
                // it draws.
                if (FUCK::SliderFloat("$FR_Set_CardSize"_T, &cfg.previewCardScale,
                                      OS::Settings::kCardScaleMin,
                                      OS::Settings::kCardScaleMax, "%.1f")) {
                    dirty = true;
                }
                Tip("$FR_Set_CardSizeTip"_T);
                dirty |= FUCK::Checkbox("$FR_Set_EverySlot"_T, &cfg.advancedSlots);
                dirty |= FUCK::Checkbox("$FR_Set_BodyFitFilter"_T, &cfg.bodyFitFilter);
                if (FUCK::IsItemHovered()) {
                    FUCK::SetTooltip("$FR_Set_BodyFitFilterTip"_T);
                }
                // ⚠ ON THE PANEL RATHER THAN INI-ONLY, on the same reasoning as the
                // camera switch below: it defaults OFF, and an option nobody can find
                // is an option nobody has.
                dirty |= FUCK::Checkbox("$FR_Set_SfwBodyCards"_T, &cfg.sfwBodyCards);
                Tip("$FR_Set_SfwBodyCardsTip"_T);
                // ⚠ ON THE PANEL RATHER THAN INI-ONLY, and the reason is worth keeping:
                // it defaults OFF, so an INI-only switch is one nobody finds, and its
                // first field test failed for exactly that. It is also the setting a
                // player is most likely to want to undo mid-session, since a moving
                // camera is either the point or an irritation and there is no telling
                // which from a description.
                dirty |= FUCK::Checkbox("$FR_Set_CameraFocusSlot"_T, &cfg.cameraFocusSlot);
                Tip("$FR_Set_CameraFocusSlotTip"_T);
                // ⚠ HERE RATHER THAN ON THE OVERLAYS PAGE, WHICH IS WHERE IT
                // STARTED. It was put beside the picker's search box so a
                // player who could not find a texture would see why without
                // leaving the page, and the field verdict was that the page had
                // filled up with controls and explanations for a filter that is
                // right nearly always. It belongs with the other things that
                // are correct until you disagree.
                dirty |= FUCK::Checkbox("$FR_Set_OverlayShowAll"_T, &cfg.overlayShowAllArt);
                Tip("$FR_Set_OverlayShowAllTip"_T);
                // Off by default and on the panel for the reason the camera
                // switch above gives: an option nobody can find is an option
                // nobody has. It is read when the roster is built, at editor
                // open, so a change here shows on the next open.
                dirty |= FUCK::Checkbox("$FR_Set_EditOtherNpcs"_T, &cfg.editOtherNpcs);
                Tip("$FR_Set_EditOtherNpcsTip"_T);
                if (FUCK::SliderFloat("$FR_Set_UiScale"_T, &cfg.uiScale,
                                      OS::Settings::kUiScaleMin,
                                      OS::Settings::kUiScaleMax, "%.2f")) {
                    dirty = true;
                }

                // ⚠ ON THIS PAGE RATHER THAN IN A THEME TAB OF ITS OWN. It is
                // one control, and a tab is navigation you have to find before
                // you can change anything. It sits with the UI scale because
                // both are about how the editor looks rather than what it does;
                // if a second theme control ever arrives, that is when a tab
                // earns its place.
                //
                // ⚠⚠ THE COMBO INDEX IS NO LONGER THE INI VALUE, AND THAT IS THE
                // ONE THING TO GET RIGHT HERE. The two lined up while auto was
                // entry 0, and auto was dropped on 2026-08-28 without renumbering
                // the wire format, so carved is still 1 and plain is still 2.
                // Reading the index straight into cfg.frameStyle would write a 0
                // for carved and a 1 for plain, so the file would say carved on
                // the run after a player picked plain.
                {
                    const char* const kFrameStyles[] = {
                        "$FR_Set_FrameStyleCarved"_T,
                        "$FR_Set_FrameStylePlain"_T,
                    };
                    const auto shape = OS::FramePolicy::StyleFromIni(cfg.frameStyle);
                    int        index = (shape == OS::FramePolicy::Style::kPlain) ? 1 : 0;
                    if (FUCK::Combo("$FR_Set_FrameStyle"_T, &index, kFrameStyles, 2)) {
                        cfg.frameStyle = OS::FramePolicy::IniFromStyle(
                            index == 1 ? OS::FramePolicy::Style::kPlain
                                       : OS::FramePolicy::Style::kCarved);
                        dirty = true;
                    }
                    Tip("$FR_Set_FrameStyleTip"_T);
                }

                // Dye. bDyeReflective was INI-only, and its name says nothing about
                // what it costs either way, so the tooltip states BOTH outcomes: this
                // is a genuine tradeoff with no free answer, not a default someone
                // forgot to flip. See OutfitDye.h for why the two cannot coexist.
                FUCK::EndTabItem();
            }
            if (FUCK::BeginTabItem("$FR_Set_Pages"_T)) {
                FUCK::TextWrapped("$FR_Set_PagesHelp"_T);
                bool changed = false;
                changed |= FUCK::Checkbox("$FR_Page_Styles"_T, &cfg.pages.styles);
                changed |= FUCK::Checkbox("$FR_Page_Dye"_T, &cfg.pages.dye);
                changed |= FUCK::Checkbox("$FR_Page_Bodies"_T, &cfg.pages.bodies);
                changed |= FUCK::Checkbox("$FR_Page_Shape"_T, &cfg.pages.shape);
                changed |= FUCK::Checkbox("$FR_Page_Overlays"_T, &cfg.pages.overlays);
                changed |= FUCK::Checkbox("$FR_Page_Presets"_T, &cfg.pages.presets);
                changed |= FUCK::Checkbox("$FR_Page_Looks"_T, &cfg.pages.looks);
                changed |= FUCK::Checkbox("$FR_Page_Rules"_T, &cfg.pages.rules);
                if (changed) {
                    // ⚠ THE SAME CLAMP THE LOAD RUNS, and it has to be here as
                    // well or the panel walks straight into the state the load
                    // exists to prevent. Unticking the last page puts Styles
                    // back rather than leaving the editor with nowhere to
                    // stand, and the tick visibly springs back so the refusal
                    // is something the player can see rather than a silent
                    // correction on the next launch.
                    OS::PagePolicy::EnsureAtLeastOne(cfg.pages);
                    dirty = true;
                }
                // ⚠ NOT A LIST OF WHAT IS MISSING. A page can also be absent
                // because OBody is not installed or the build channel excludes
                // it, and this tab deliberately does not know about any of
                // that: it would need the same requirement checks the rail
                // already makes, and two answers to "is Bodies available" drift
                // the first time one of them grows a condition. The rail's own
                // tooltip names the missing mod at the tile.
                Tip("$FR_Set_PagesTip"_T);
                FUCK::EndTabItem();
            }

            if (FUCK::BeginTabItem("$FR_Set_Dye"_T)) {
                // The INI key is [General] bDyeUnlocks, per the design spec, while the
                // checkbox sits with the other dye levers because that is where someone
                // looking for it will look. bDyeReflective is [Dye] and shown here too,
                // so the panel already groups by subject rather than by INI section.
                //
                // ⚠ It gates PICKING a colour, never earning one and never painting
                // one. A garment already carrying a locked colour keeps rendering it,
                // so turning this on cannot destroy dye work across saved outfits.
                dirty |= FUCK::Checkbox("$FR_Set_DyeUnlocks"_T, &cfg.dyeUnlocks);
                Tip("$FR_Set_DyeUnlocksTip"_T);
                // ⚠ DIRECTLY UNDER THE SETTING IT DECORATES, and it is the
                // announcement's switch rather than the economy's. Off, colours
                // are still earned and the gold corner still marks them in the
                // grid; the player is just not told at the time. The dwell, the
                // stack height and the tick interval stay INI-only ([Dye]
                // fDyeCardDwell, iDyeCardMax, iDyeCardBurst, iDyeTickSeconds):
                // they are shape, not choice, and a panel row each would bury
                // the one question a player actually has about this.
                dirty |= FUCK::Checkbox("$FR_Set_DyeCards"_T, &cfg.dyeUnlockCards);
                Tip("$FR_Set_DyeCardsTip"_T);
                // ⚠⚠ THE BUTTON IS NOT A LUXURY, IT IS THE ONLY WAY TO SEE THIS
                // FEATURE ON A PLAYED CHARACTER. Field run 2026-08-14: every
                // pass for three minutes logged "nothing newly earned, 122
                // held", because a character who has earned most of the palette
                // has nothing left for the tick to hand over. The setting above
                // was therefore a switch with no observable effect, reported as
                // "i don't see any notifications".
                //
                // ⚠ IT MUST FIRE AFTER THE PANEL CLOSES, not while it is up. The
                // card window suppresses itself over the editor, and the FLICK
                // panel is where this button lives, so a card queued here draws
                // once the player is looking at the world again. Preview stamps
                // nothing, so the stack simply waits for its first Draw.
                const auto cardBtn = ChamferPanel::Button("$FR_Set_DyeCardsPreview"_T);
                if (cardBtn.clicked) {
                    OS::DyeUnlockCard::Preview();
                    EditorStyle::PlayUISound("UIMenuOK");
                }
                Tip(cardBtn.hovered, "$FR_Set_DyeCardsPreviewTip"_T);
                // ⚠ ABOVE bDyeReflective, because it changes what that one is FOR. While
                // dyeing metal cost its shine, "do the bulk actions touch metal" was a
                // question about a permanent trade. With the shine kept it is an
                // ordinary reach question, so the setting that decides which of those
                // two worlds the player is in belongs first.
                dirty |= FUCK::Checkbox("$FR_Set_DyeKeepShine"_T, &cfg.dyeKeepShine);
                Tip("$FR_Set_DyeKeepShineTip"_T);
                dirty |= FUCK::Checkbox("$FR_Set_DyeReflective"_T, &cfg.dyeReflective);
                Tip("$FR_Set_DyeReflectiveTip"_T);

                // ⚠ BACK HERE, AND THE SECOND MOVE IS NOT THE FIRST ONE UNDONE.
                // It shipped here, moved to the palette header because a control
                // that changes how a grid LOOKS belongs beside the grid, and
                // came back when the ORDER STOPPED BEING A CHOICE: sorting by
                // colour is the default now, so a tick box above the swatches
                // was one that everybody would find already ticked. A control
                // nobody needs to reach does not earn a row in the pane, and the
                // few who want a pack's own file order can find it here once.
                dirty |= FUCK::Checkbox("$FR_Set_DyeSortHue"_T, &cfg.dyeSortByHue);
                Tip("$FR_Set_DyeSortHueTip"_T);

                // How a dye is mixed with the cloth under it.
                //
                // ⚠ IT LIVES HERE AND NOT IN THE DYE PANE, and that is the setting's
                // SCOPE talking rather than convenience. It is one choice for every dye
                // on every piece, exactly like the two checkboxes above it, and the dye
                // pane's own controls are all per CHANNEL. A global control sitting
                // inside a per-channel accordion would read as "this stripe's blend"
                // and be a lie in the one place a player would most believe it. It
                // moves the day the blend becomes per-dye, and that day it costs a
                // codec bump. an-install-choice-must-not-gate-a-runtime-setting is the
                // neighbouring rule: this is a runtime setting and lives with them.
                //
                // ⚠ THE NAME IS STORED, NOT THE INDEX, so the row is found by resolving
                // both sides through BlendFromName rather than by comparing strings.
                // That also means "color" in a hand-edited INI selects the same row as
                // "colour", and an unreadable spelling shows the default rather than an
                // empty combo.
                {
                    const auto& names   = DyeTexture::kSelectableBlendNames;
                    const auto  current = DyeTexture::BlendFromName(cfg.dyeBlendName);
                    int         sel     = 0;
                    for (int i = 0; i < static_cast<int>(std::size(names)); ++i) {
                        if (DyeTexture::BlendFromName(names[i]) == current) {
                            sel = i;
                            break;
                        }
                    }
                    const char* const kBlendLabels[] = {
                        "$FR_Set_DyeBlendSoftLight"_T, "$FR_Set_DyeBlendMultiply"_T,
                        "$FR_Set_DyeBlendScreen"_T,    "$FR_Set_DyeBlendOverlay"_T,
                        "$FR_Set_DyeBlendColour"_T,    "$FR_Set_DyeBlendLuminosity"_T,
                    };
                    static_assert(std::size(kBlendLabels) ==
                                      std::size(DyeTexture::kSelectableBlendNames),
                                  "every selectable blend needs a label and the two arrays are "
                                  "indexed together");
                    if (FUCK::Combo("$FR_Set_DyeBlend"_T, &sel, kBlendLabels,
                                    IM_ARRAYSIZE(kBlendLabels))) {
                        cfg.dyeBlendName = std::string{ names[sel] };
                        dirty            = true;
                    }
                    Tip("$FR_Set_DyeBlendTip"_T);
                }

                // ⚠ A CONTROL GATE, NOT AN ARITHMETIC ONE, which is what makes it a
                // settings checkbox rather than a [Debug] key. It offers or withholds
                // the eye's second colour swatch; the colour itself lives on the
                // outfit, so turning this off keeps what is stored and simply stops
                // painting it. bEyeDyeRecolour and sEyeDyeSplitHex chose the arithmetic
                // behind the player's back and had to be deleted for it.
                dirty |= FUCK::Checkbox("$FR_Set_EyeAdvanced"_T, &cfg.eyeAdvancedColour);
                Tip("$FR_Set_EyeAdvancedTip"_T);

                FUCK::EndTabItem();
            }
            if (FUCK::BeginTabItem("$FR_Set_Setup"_T)) {

                // ---- the style browser's columns (moved here 2026-08-14) ----
                //
                // ⚠ THEY CAME FROM THE EDITOR'S GEAR POPUP, which had taken them
                // from a gear in the browser under "one settings home" (OS-30).
                // That was the right idea aimed at the wrong home: the popup is
                // the EDITOR's, and this panel is where the rest of the persisted
                // settings already live.
                //
                // ⚠⚠ AND THEY BECAME PERSISTED ON THE WAY. All three were
                // session-local and Hide unfit was re-forced ON at every editor
                // open, which was a deliberate call in August. A switch sitting
                // beside the persisted settings that silently forgets itself
                // every open is the worse of the two surprises, so the reset in
                // EditorUI went with the move. Turning a column off now keeps it
                // off.
                FUCK::SeparatorText("$FR_Set_StyleList"_T);
                dirty |= FUCK::Checkbox("$FR_ColClass"_T, &cfg.browserShowClass);
                dirty |= FUCK::Checkbox("$FR_ColPlugin"_T, &cfg.browserShowPlugin);
                dirty |= FUCK::Checkbox("$FR_HideUnfit"_T, &cfg.browserHideUnfit);
                Tip("$FR_HideUnfitTip"_T);

                // ⚠ RESCAN LANDS HERE (user 2026-08-14), one step further along
                // the road it was already travelling: it left the presets search
                // row in August because a maintenance action was holding the most
                // contested pixels on that page, then sat in the editor's gear
                // popup, which is still the editor's rather than where settings
                // live. It belongs under the style-list switches it acts with.
                //
                // ⚠⚠ THROUGH EditorUI::RequestSourceRescan, NOT BY PASTING THE
                // HANDLER. The old one reset the showcase selection and re-seeded
                // staging, and THIS PANEL OPENS FROM FLICK'S SIDEBAR WITH NO
                // EDITOR AND NO SESSION. The store rescan is global and always
                // safe; everything editor-shaped is guarded on the other side.
                const auto rescanBtn = ChamferPanel::Button("$FR_Rescan"_T);
                if (rescanBtn.clicked) {
                    OS::EditorUI::RequestSourceRescan();
                    EditorStyle::PlayUISound("UIMenuFocus");
                }
                // The tip depends on which source the browser is showing, which
                // is EditorUI's to know and not this panel's.
                Tip(rescanBtn.hovered, OS::EditorUI::SourceRescanTip());

                // ---- physics ------------------------------------------
                //
                // ⚠⚠ THIS IS NOT A BOUNCE AMOUNT AND THE COPY MUST NOT SAY IT
                // IS. CBPC reads breast amplitude off the armour REALLY worn,
                // Fitting Room renders a different piece, and the number below
                // is which of those two wins: it is the `percentage` of
                // ApplyBounceInterpolation, blending our profile onto the
                // actor. The profile mirrors the rig baseline, so turning it up
                // cannot go past what the naked body already does. Raising that
                // ceiling means editing the amplitudes in
                // CBPCBounceinterpolationconfig_FittingRoom.txt, which is a
                // different control on a different day.
                //
                // ⚠⚠ GREYED WITH THE REASON, NEVER HIDDEN. Off by REQUIREMENT
                // and not by choice, which is the rule the whole panel follows:
                // a control that vanishes teaches nobody what would bring it
                // back. And only on kAbsent, never on kUnknown, because "we have
                // not asked CBPC yet" is not an answer about CBPC.
                FUCK::SeparatorText("$FR_Set_Physics"_T);
                {
                    const bool noCbpc = OS::CbpcArmorClass::SupportState() ==
                                        OS::CbpcArmorClass::Support::kAbsent;
                    FUCK::BeginDisabled(noCbpc);
                    int bounce = cfg.cbpcBouncePercent;
                    if (FUCK::SliderInt("$FR_Set_BouncePercent"_T, &bounce, 0, 100)) {
                        cfg.cbpcBouncePercent = bounce;
                        dirty                 = true;
                    }
                    FUCK::EndDisabled();
                    // ⚠ A LINE RATHER THAN A TOOLTIP WHEN IT IS OFF. A disabled
                    // item does not report hover, so the reason would be the one
                    // thing on this panel nobody could read.
                    if (noCbpc) {
                        OS::ui::TextDisabledWrapped("$FR_Set_BounceNoCbpc"_T);
                    } else {
                        Tip("$FR_Set_BouncePercentTip"_T);
                    }
                }

                FUCK::SeparatorText("$FR_Set_Compatibility"_T);
                dirty |= FUCK::Checkbox("$FR_Set_SuspendScenes"_T,
                                        &cfg.sceneCompat);
                // ⚠ AN EXPERIMENT WITH A KNOWN WAY TO FAIL, so it says so on
                // the row rather than only in the ini. On a Grid Inventory
                // that answers a hide by closing, this costs the player the
                // editor the instant it opens, and a player who ticked
                // something a minute ago needs to be able to connect the two.
                // EditorWindow.cpp carries the field history.
                dirty |= FUCK::Checkbox("$FR_Set_GridInventoryHide"_T,
                                        &cfg.gridInventoryHide);
                Tip("$FR_Set_GridInventoryHideTip"_T);

                FUCK::SeparatorText("$FR_Set_Controls"_T);
                // Both hotkeys go through KeyCombo so they cannot drift apart. The
                // "change outfit" one used to be INI-only (iNextOutfitKeyDIK), which
                // meant the one binding most people want to change was the one they
                // could not reach from the panel.
                dirty |= KeyCombo("$FR_Set_Hotkey"_T, cfg.editorKeyDIK,
                                  [](std::uint32_t a_dik) {
                                      OS::InputListener::GetSingleton().SetEditorKey(a_dik);
                                  });
                Tip("$FR_Set_HotkeyTip"_T);

                dirty |= KeyCombo("$FR_Set_NextOutfit"_T, cfg.nextOutfitKeyDIK,
                                  [](std::uint32_t a_dik) {
                                      OS::InputListener::GetSingleton().SetNextOutfitKey(a_dik);
                                  });
                Tip("$FR_Set_NextOutfitTip"_T);

                // ⚠⚠ A DROPDOWN LIKE THE TWO ABOVE, AND IT WAS A FLICK
                // BINDER FOR ONE BUILD. FUCK's ManagedHotkey draws a proper
                // binder with key icons, modifiers and a gamepad half, and in
                // the field it never captured: clicking it started the flashing
                // bind state and stayed there through every key pressed
                // (2026-08-18). Completing a FUCK bind needs the plugin to pump
                // UpdateManagedHotkey from its own async-input hook, and a
                // sidebar tool only exists while it is being drawn. The field's
                // call was to make it match its neighbours, which also makes all
                // three readable in one screenshot of this panel.
                dirty |= KeyCombo("$FR_Set_DirectEntry"_T, cfg.directEntryKeyDIK,
                                  [](std::uint32_t a_dik) {
                                      OS::InputListener::GetSingleton().SetDirectEntryKey(a_dik);
                                  });
                Tip("$FR_Set_DirectEntryTip"_T);

                FUCK::EndTabItem();
            }
            FUCK::EndTabBar();
        }

        FUCK::TextDisabled("%s", "$FR_Set_SaveNote"_T);

        if (dirty) {
            cfg.Save();
            // ⚠ THE TWO DYE-CARD MODULES CACHE WHAT THEY READ, so a commit has
            // to hand it to them or the switch does nothing until the next
            // load, silently. That is the exact failure the shared-unlocks
            // sync already documents one setting over: turning a thing on used
            // to be inert until you reloaded, and nothing said so.
            //
            // ⚠ THIS RUNS ON THE RENDER THREAD, which is why both are written
            // to take it: the card's timing sits behind its own lock and the
            // tick's interval is one atomic.
            OS::DyeUnlockCard::ApplySettings();
            OS::DyeTick::ApplySettings();
        }
    }

}  // namespace OS::SettingsUI

namespace {

    // FLICK sidebar entry: the user opens FUCK (hotkey / controller menu) and
    // picks "Outfit Slots".
    class SettingsTool : public FUCK::ITool {
    public:
        const char* Name() const override {
            static const auto name = OS::BuildChannel::Label();
            return name.c_str();
        }
        void        Draw() override { OS::SettingsUI::DrawPanel(); }
    };

    SettingsTool g_settingsTool;  // process-lifetime; the registered pointer stays valid
}

namespace OS::SettingsUI {

    void Register() {
        // Soft dependency: without FUCK.dll the mod stays INI-only with one log
        // line. The name passed to Connect is what FLICK shows in its sidebar.
        if (!FUCK::Connect("Fitting Room")) {
            spdlog::info("SettingsUI: FUCK / FLICK not present; INI-only mode.");
            return;
        }
        FUCK::RegisterTool(&g_settingsTool);
        spdlog::info("SettingsUI: registered as a FLICK (FUCK) sidebar tool.");
    }

}  // namespace OS::SettingsUI
