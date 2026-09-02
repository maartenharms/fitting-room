#include "Settings.h"

#include "BuildChannel.h"

#define SI_NO_CONVERSION 1
#include <SimpleIni.h>

#include <algorithm>
#include <cstdlib>

namespace OS {

    namespace {
        const std::string& IniPath() {
            static const auto path = BuildChannel::IniPath().string();
            return path;
        }
    }

    Settings& Settings::GetSingleton() {
        static Settings instance;
        return instance;
    }

    void Settings::Load() {
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(IniPath().c_str()) < 0) {
            // ⚠⚠ WARN, NOT INFO, BECAUSE THE NEXT LINE OVERWRITES THE ANSWER THE
            // INSTALLER GAVE. Every FOMOD install ships a FittingRoom.ini, so
            // reaching here on a fresh install means the file the player's
            // choices were written into is not the one we are reading, and the
            // Save below pins the code defaults over it. That was invisible at
            // info level for the whole life of the bug it belongs to.
            spdlog::warn("Settings: no INI at {}, so the code defaults are in "
                         "use and are being written there. A FOMOD install "
                         "should always have one; check for a FittingRoom.ini "
                         "in Overwrite or in a mod above this one.",
                         IniPath());
            Save();
            return;
        }
        enabled        = ini.GetBoolValue("General", "bEnabled", enabled);
        // Migration: the legacy bLoreMode bundled gold + the Seamstone
        // requirement. bUseGold still inherits it (gold on by default, ESO-style);
        // bRequireSeamstone now defaults OFF so the default Y hotkey opens the
        // editor out of the box, with the Seamstone as an optional extra way in.
        const bool legacyLore = ini.GetBoolValue("General", "bLoreMode", true);
        // ⚠ THE CHAIN IS bLoreMode -> bUseGold -> iCostMode, AND EACH LINK
        // FEEDS THE NEXT AS ITS DEFAULT. An absent iCostMode must inherit what
        // bUseGold said, NEVER the C++ default: no shipped INI has the new key,
        // so a plain GetLongValue against the member would put every existing
        // free-form save (bUseGold=0) back onto gold. The reverse mistake is
        // worse still - defaulting to kCharge would hand every lore-friendly
        // save an empty stone, and an empty stone blocks styling outright.
        //
        // Read the old key even when the new one is present. It costs one INI
        // lookup and it keeps this correct if the new key is later deleted by
        // hand.
        const bool legacyGold = ini.GetBoolValue("General", "bUseGold", legacyLore);
        costMode              = CostModeFrom(ini.GetLongValue(
            "General", "iCostMode",
            static_cast<long>(legacyGold ? CostMode::kGold : CostMode::kFree)));
        requireSeamstone = ini.GetBoolValue("General", "bRequireSeamstone", false);
        stayInEditorAfterApply = ini.GetBoolValue(
            "General", "bStayInEditorAfterApply", stayInEditorAfterApply);
        replaceOnLookApply = ini.GetBoolValue(
            "General", "bReplaceOnLookApply", replaceOnLookApply);
        collectionOnly = ini.GetBoolValue("General", "bCollectionOnly", collectionOnly);
        collectionShared =
            ini.GetBoolValue("General", "bCollectionShared", collectionShared);
        // ⚠ ABSENT KEEPS THE C++ DEFAULT, and here that default CHANGES what
        // every existing INI does: outfits used to seed a new character
        // unconditionally and now do not. Deliberate. Nothing already saved
        // moves, because a save with its own 'LIBR' record was never reading
        // this file anyway; only a brand new character starts differently.
        outfitsShared = ini.GetBoolValue("General", "bOutfitsShared", outfitsShared);
        // ⚠ ABSENT KEEPS THE C++ DEFAULT, which is what carries this one for
        // every player who already has an INI. The key is new, so no shipped
        // file has it, and every one of those loads with unlocks ON. Checked
        // against the deployed FittingRoom.ini rather than assumed.
        dyeUnlocks = ini.GetBoolValue("General", "bDyeUnlocks", dyeUnlocks);
        dyeUnlocksShared =
            ini.GetBoolValue("General", "bDyeUnlocksShared", dyeUnlocksShared);
        requireWornForStyles =
            ini.GetBoolValue("General", "bRequireWornForStyles", requireWornForStyles);
        // ⚠⚠ THE INSTALLER SHIPPED FOUR OF THE PLAYSTYLE'S FIVE FIELDS UNTIL
        // 2026-08-27, and the one it left out defaults to the lore value. So a
        // Free-form install came up with the collection filter ON: the browser
        // offered only looks the character had already owned the gear for, and
        // applying any other was refused, which is the lore-friendly half of
        // "earn it, then wear it" installed by the page that promised the
        // opposite. Reported from a fresh FOMOD install; the template carries
        // bCollectionOnly now, and this is for the files already written.
        //
        // ⚠ THE TEST IS THE OTHER THREE GATES, NOT A VERSION NUMBER. A file with
        // no bCollectionOnly that reads free on all three was written by one of
        // those installers and by nothing else, because Save has always written
        // this key: anything the panel has touched has it. A lore install fails
        // the test on all three, so this cannot reach one, and a player who
        // turned the filter on by hand has the key and is left alone.
        if (ini.GetValue("General", "bCollectionOnly", nullptr) == nullptr &&
            costMode == CostMode::kFree && !requireSeamstone && !requireWornForStyles) {
            collectionOnly = false;
            spdlog::info("Settings: a Free-form INI with no bCollectionOnly, so the "
                         "collection filter is off. The installer used to leave that "
                         "key out and its default is the lore-friendly one.");
        }
        hideEquippedOnImport = ini.GetBoolValue("General", "bHideEquippedOnImport",
                                                hideEquippedOnImport);
        // Onboarding. Absent keeps the C++ default of false, so an install that
        // has never seen these keys is offered every tutorial once. See
        // Settings.h for why there is one key per tutorial rather than one for
        // the lot.
        pages.styles   = ini.GetBoolValue("Pages", "bStyles", pages.styles);
        pages.presets  = ini.GetBoolValue("Pages", "bPresets", pages.presets);
        pages.dye      = ini.GetBoolValue("Pages", "bDye", pages.dye);
        pages.rules    = ini.GetBoolValue("Pages", "bRules", pages.rules);
        pages.bodies   = ini.GetBoolValue("Pages", "bBodies", pages.bodies);
        pages.shape    = ini.GetBoolValue("Pages", "bShape", pages.shape);
        pages.overlays = ini.GetBoolValue("Pages", "bOverlays", pages.overlays);
        pages.looks    = ini.GetBoolValue("Pages", "bLooks", pages.looks);
        // ⚠⚠ THE INI IS A TEXT FILE AND ALL-OFF IS ONE EDIT AWAY. Clamping here
        // is what lets the rail assume it always has at least one tile, so
        // there is no empty state to draw in a rail that would have nothing to
        // draw it beside. The panel calls the same function after every change.
        if (PagePolicy::EnsureAtLeastOne(pages)) {
            spdlog::warn("Settings: every page was switched off, so the Styles page "
                         "is back on. The editor needs somewhere to stand.");
        }
        tutorialsOn        = ini.GetBoolValue("Tutorial", "bEnabled", tutorialsOn);
        tutorialWelcome    = ini.GetBoolValue("Tutorial", "bWelcome", tutorialWelcome);
        tutorialOutfits    = ini.GetBoolValue("Tutorial", "bOutfits", tutorialOutfits);
        tutorialDye        = ini.GetBoolValue("Tutorial", "bDye", tutorialDye);
        tutorialPresets    = ini.GetBoolValue("Tutorial", "bPresets", tutorialPresets);
        tutorialRules      = ini.GetBoolValue("Tutorial", "bRules", tutorialRules);
        tutorialShape      = ini.GetBoolValue("Tutorial", "bShape", tutorialShape);
        tutorialBodyStudio = ini.GetBoolValue("Tutorial", "bBodyStudio", tutorialBodyStudio);
        // Absent from every INI written before the Overlays page had a
        // tutorial, so it defaults unseen and the page is taught on first
        // open, which is the behaviour the per-tutorial keys exist to buy.
        tutorialOverlays   = ini.GetBoolValue("Tutorial", "bOverlays", tutorialOverlays);
        // Same story as bOverlays above, one page later: absent from every INI
        // written before the Looks page had a tutorial, so it defaults unseen.
        tutorialLooks      = ini.GetBoolValue("Tutorial", "bLooks", tutorialLooks);
        // ⚠ THIS LINE WAS MISSING AND THE SEND-OFF CAME BACK ON EVERY LAUNCH.
        // Save has written bClosing since the closing tutorial was added; Load
        // never read it, so the flag came up false on every start, the send-off
        // re-fired the moment the editor opened, and the INI on disk said
        // bClosing=true the whole time (field 2026-08-12, measured on the live
        // install). A written key that nothing reads is not half a feature, it
        // is a setting that silently does not exist.
        tutorialClosing = ini.GetBoolValue("Tutorial", "bClosing", tutorialClosing);
        sceneKick = ini.GetBoolValue("Advanced", "bSceneKick", sceneKick);
        dumpBiped = ini.GetBoolValue("Debug", "bDumpBiped", dumpBiped);
        // ⚠ DEFAULTS TRUE WHEN THE FEATURE IS COMPILED IN, and the default is
        // the whole point rather than a detail. This flag does not merely log:
        // BodyStudioProof owns the queue that actually applies a body, so the
        // rail tile draws greyed while it is false. Existing INIs predate the
        // key entirely, and an absent key keeps the default, so a false one
        // would ship the page permanently disabled to every install that
        // already has a FittingRoom.ini. Set it to 0 to switch the workbench
        // off; do not switch the default.
        bodyStudioProof = BuildChannel::kBodyStudio &&
                          ini.GetBoolValue("Debug", "bBodyStudioProof", true);
        // ⚠ bHairProbe, bHairProbeAttach and sHairProbePart are GONE, not
        // dropped by accident in the merge. They drove HairProbe.{h,cpp}, the
        // OS-95 spike, which the follower hair branch deleted once the real
        // implementation landed. Their Settings members went with them.
        hairFaceDump = ini.GetBoolValue("Debug", "bHairFaceDump", hairFaceDump);
        dyeCensus = ini.GetBoolValue("Debug", "bDyeCensus", dyeCensus);
        appearanceWatch =
            ini.GetBoolValue("Debug", "bAppearanceWatch", appearanceWatch);
        // ⚠ bHeadCensus and bHeadPartTypeCounter are GONE, like the hair probe
        // keys above. They drove src/HeadCensus.* and the type counter in
        // HeadBuildHook, the dyeing-eyes instruments, and both were deleted at
        // the 1.0.0 release once the eye and head-part rules they existed to
        // author had shipped and been field-confirmed. Save() deletes them from
        // any INI that still carries them.
        eyeDyeSpike = ini.GetBoolValue("Debug", "bEyeDyeSpike", eyeDyeSpike);
        eyeDyeSpikeHex =
            ini.GetValue("Debug", "sEyeDyeSpikeHex", eyeDyeSpikeHex.c_str());
        eyeDyeCapPx = static_cast<std::uint32_t>(
            ini.GetLongValue("Debug", "iDyeEyeCapPx", static_cast<long>(eyeDyeCapPx)));
        eyeDyeNegativeControl =
            ini.GetBoolValue("Debug", "bEyeDyeNegativeControl", eyeDyeNegativeControl);
        eyeDyeSkipRepick = ini.GetBoolValue("Debug", "bEyeDyeSkipRepick", eyeDyeSkipRepick);
        dyeSpikeRung = static_cast<std::uint32_t>(
            ini.GetLongValue("Debug", "iDyeSpikeRung", static_cast<long>(dyeSpikeRung)));
        dyeEnvScale      = static_cast<float>(
            ini.GetDoubleValue("Debug", "fDyeEnvScale", static_cast<double>(dyeEnvScale)));
        dyeTintReflection =
            ini.GetBoolValue("Debug", "bDyeTintReflection", dyeTintReflection);
        dyeTextureBudgetMiB = static_cast<std::uint32_t>(ini.GetLongValue(
            "Debug", "iDyeTextureBudgetMiB", static_cast<long>(dyeTextureBudgetMiB)));
        dyePreviewCapPx     = static_cast<std::uint32_t>(ini.GetLongValue(
            "Debug", "iDyePreviewCapPx", static_cast<long>(dyePreviewCapPx)));
        dyeCommitCapPx      = static_cast<std::uint32_t>(ini.GetLongValue(
            "Debug", "iDyeCommitCapPx", static_cast<long>(dyeCommitCapPx)));
        dyeSettleMs         = static_cast<std::uint32_t>(
            ini.GetLongValue("Debug", "iDyeSettleMs", static_cast<long>(dyeSettleMs)));
        overlayBakeCapPx    = static_cast<std::uint32_t>(ini.GetLongValue(
            "Debug", "iOverlayBakeCapPx", static_cast<long>(overlayBakeCapPx)));
        overlayBakeCacheMiB = static_cast<std::uint32_t>(ini.GetLongValue(
            "Debug", "iOverlayBakeCacheMiB", static_cast<long>(overlayBakeCacheMiB)));
        dyeEmissiveProbe = ini.GetBoolValue("Debug", "bDyeEmissiveProbe", dyeEmissiveProbe);
        dyeGpuTask       = static_cast<std::uint32_t>(
            ini.GetLongValue("Debug", "iDyeGpuTask", static_cast<long>(dyeGpuTask)));
        profileProbeKeyDIK = static_cast<std::uint32_t>(ini.GetLongValue(
            "Debug", "iProfileProbeKeyDIK", static_cast<long>(profileProbeKeyDIK)));
        paintScrape = ini.GetBoolValue("Debug", "bPaintScrape", paintScrape);
        // ⚠ THE OLD `[Debug] bItemCardCharge` IS NOT READ AT ALL ANY MORE, not
        // even as a fallback. Reading it would honour the explicit `false` that
        // every long-time install carries, which is the exact state that left a
        // player on iCostMode=2 unable to obtain the only currency the mod
        // would accept. Save() deletes the key.
        seamstoneCharging =
            ini.GetBoolValue("Dye", "bSeamstoneCharging", seamstoneCharging);
        itemCardType     = static_cast<std::int32_t>(
            ini.GetLongValue("Debug", "iItemCardType", static_cast<long>(itemCardType)));
        itemCardShape    = static_cast<std::int32_t>(
            ini.GetLongValue("Debug", "iItemCardShape", static_cast<long>(itemCardShape)));
        // ⚠ bSeamstoneVanillaCharge, bSeamstoneMakeExtraList and
        // iSeamstoneEnchant are GONE, and a live INI still carrying them is
        // meant to be ignored rather than honoured. They drove
        // SeamstoneEnchant.{h,cpp}, which lent the stone an ExtraEnchantment so
        // the vanilla soul-gem recharge would have a capacity to work against.
        // ⚠ AND THE REASON GIVEN HERE WHEN THEY WERE DELETED WAS WRONG, which
        // is worth leaving on the record: it said the recharge is gated on the
        // item really being enchanted. It is not. The apply's guard only asks
        // whether anything is selected, and the real block was a clamp against a
        // max charge of 0. SeamstoneRecharge answers that with three read-only
        // call-site hooks and writes nothing, which is why deleting these keys
        // was still right - it is the permanent save write they cost that has no
        // place here, not the feature.
        // Presence-checked so an absent key keeps the (empty) default.
        if (const char* v = ini.GetValue("Debug", "sDiagnosePlugin", nullptr); v && *v) {
            diagnosePlugin = v;
        }

        editorKeyDIK = static_cast<std::uint32_t>(
            ini.GetLongValue("Input", "iEditorKeyDIK", static_cast<long>(editorKeyDIK)));
        editorGamepadButton = static_cast<std::uint32_t>(
            ini.GetLongValue("Input", "iEditorGamepadButton", static_cast<long>(editorGamepadButton)));
        nextOutfitKeyDIK = static_cast<std::uint32_t>(
            ini.GetLongValue("Input", "iNextOutfitKeyDIK", static_cast<long>(nextOutfitKeyDIK)));
        directEntryKeyDIK = static_cast<std::uint32_t>(
            ini.GetLongValue("Input", "iDirectEntryKeyDIK",
                             static_cast<long>(directEntryKeyDIK)));

        goldPerSlot = static_cast<std::uint32_t>(
            ini.GetLongValue("Lore", "iGoldPerSlot", static_cast<long>(goldPerSlot)));
        dyeCost = static_cast<std::uint32_t>(
            ini.GetLongValue("Lore", "iDyeCost", static_cast<long>(dyeCost)));
        goldPerDye = static_cast<std::uint32_t>(
            ini.GetLongValue("Lore", "iGoldPerDye", static_cast<long>(goldPerDye)));
        chargePerSlot = static_cast<std::uint32_t>(
            ini.GetLongValue("Lore", "iChargePerSlot", static_cast<long>(chargePerSlot)));
        chargePerDye = static_cast<std::uint32_t>(
            ini.GetLongValue("Lore", "iChargePerDye", static_cast<long>(chargePerDye)));
        goldPerLook = static_cast<std::uint32_t>(
            ini.GetLongValue("Lore", "iGoldPerLook", static_cast<long>(goldPerLook)));
        chargePerLook = static_cast<std::uint32_t>(
            ini.GetLongValue("Lore", "iChargePerLook", static_cast<long>(chargePerLook)));
        goldPerLooksMenu = static_cast<std::uint32_t>(ini.GetLongValue(
            "Lore", "iGoldPerLooksMenu", static_cast<long>(goldPerLooksMenu)));
        chargePerLooksMenu = static_cast<std::uint32_t>(ini.GetLongValue(
            "Lore", "iChargePerLooksMenu", static_cast<long>(chargePerLooksMenu)));
        chargeSpendSound =
            ini.GetValue("Lore", "sChargeSpendSound", chargeSpendSound.c_str());
        chargeFillSound =
            ini.GetValue("Lore", "sChargeFillSound", chargeFillSound.c_str());
        goldSpendSound =
            ini.GetValue("Lore", "sGoldSpendSound", goldSpendSound.c_str());
        // ⚠ MIGRATION FOR A DEFAULT THAT NAMED NOTHING. `UIEnchantingCharge`
        // does not exist in Skyrim.esm, so the refill cue was silent, and the
        // settings panel writes every key back - so anyone who opened it once
        // has the dead id on disk where the corrected default cannot reach
        // them. Only that EXACT string is replaced, so a hand-picked value,
        // including a deliberate empty one, is left alone.
        if (chargeFillSound == "UIEnchantingCharge") {
            chargeFillSound = "UIEnchantRecharge";
            spdlog::info(
                "Settings: sChargeFillSound named 'UIEnchantingCharge', which is "
                "not a descriptor in this game, so the refill cue was silent. "
                "Using 'UIEnchantRecharge' instead.");
        }
        enchantingXpPerCharge = static_cast<float>(ini.GetDoubleValue(
            "Lore", "fEnchantingXpPerCharge",
            static_cast<double>(enchantingXpPerCharge)));
        // A negative rate would subtract experience, which no setting here
        // should be able to do by typo.
        if (enchantingXpPerCharge < 0.0f) {
            enchantingXpPerCharge = 0.0f;
        }
        // ⚠⚠ THE 0.1 THIS SHIPPED WITH IS 90,000 EXPERIENCE PER COMMON SOUL.
        // The engine multiplies what we hand it by AVEnchanting's UseMult of
        // 900; Settings.h carries the whole account. Save() had already written
        // the broken rate into every file that had ever loaded, so the code
        // default alone reaches nobody who has played.
        //
        // ⚠ A CEILING, NOT AN EQUALITY TEST, and that is the difference from
        // the UI-scale adopt above. There the question was "is this the default
        // we handed out"; here every value in the old scale is wrong by the
        // same 900, including one a player typed themselves, so anything above
        // the ceiling comes down. Below it they are already in the new scale
        // and are left alone.
        // ⚠ HOISTED. The UI-scale adopt below reads this same stamp; it used to
        // declare it and now uses it, because this migration runs first.
        const long iniVersion = ini.GetLongValue("General", "iSettingsVersion", 0);
        if (iniVersion < kEnchantingXpSettingsVersion &&
            enchantingXpPerCharge > kEnchantingXpLegacyCeiling) {
            spdlog::info("Settings: fEnchantingXpPerCharge {:g} is from before "
                         "the rate accounted for Enchanting's 900x skill use "
                         "multiplier, and would pay {:.0f} experience for a "
                         "common soul. Moving it to {:g}, which pays about 54.",
                         enchantingXpPerCharge,
                         enchantingXpPerCharge * 1000.0f * 900.0f,
                         kEnchantingXpDefault);
            enchantingXpPerCharge = kEnchantingXpDefault;
        }
        seamstoneCapacity = static_cast<std::uint32_t>(ini.GetLongValue(
            "Lore", "iSeamstoneCapacity", static_cast<long>(seamstoneCapacity)));
        slotBlocklist = static_cast<std::uint32_t>(
            std::strtoul(ini.GetValue("Advanced", "uSlotBlocklist", "0"), nullptr, 0));
        menuFontSize = static_cast<float>(
            ini.GetDoubleValue("UI", "fFontSize", static_cast<double>(menuFontSize)));
        uiScale = static_cast<float>(
            ini.GetDoubleValue("UI", "fUiScale", static_cast<double>(uiScale)));
        uiScale = std::clamp(uiScale, kUiScaleMin, kUiScaleMax);
        // ⚠⚠ AN OLD DEFAULT IS NOT A PREFERENCE, AND THE INI CANNOT TELL THEM
        // APART ON ITS OWN. Save writes fUiScale, so an install from before
        // 2026-08-14 carries the default of its day for good and no later
        // default ever reaches it (user 2026-08-27: the editor "could sometimes
        // be 0.8"). The stamp read below is the only thing that separates the
        // two, and the whole reasoning is on OS::UiScale.
        //
        // ⚠ AFTER THE CLAMP, DELIBERATELY. The slider's old range topped out at
        // 1.6, so an INI from that era lands on exactly kUiScaleMax here
        // whatever it said, and checking the clamped value covers that road as
        // well as the handed-out default.
        const float adoptedScale =
            UiScale::Adopt(uiScale, static_cast<int>(iniVersion), kUiScaleDefault);
        if (adoptedScale != uiScale) {
            spdlog::info("Settings: fUiScale {:.2f} is a default this build no "
                         "longer ships and the INI stamp is {} rather than {}, "
                         "so the editor starts at {:.2f}. Moving the slider "
                         "stamps the file and is left alone from then on.",
                         uiScale, iniVersion, UiScale::kSettingsVersion,
                         adoptedScale);
            uiScale = adoptedScale;
        }
        // ⚠⚠ NO MIGRATION ON THIS KEY ANY MORE, AND IT NEEDS NONE. There was
        // one for a day: carved shipped as 1.0.0's default, Save pinned it into
        // every install, and auto had to be adopted in over the top. Auto is
        // gone as of 2026-08-28 and carved is the default again, so every value
        // already in the wild lands where it should. 1 stays carved, 0 was auto
        // and StyleFromIni reads it as carved, and a 2 is a player who went and
        // found plain. Adopting anything here would only move somebody off a
        // shape they can see.
        frameStyle = static_cast<int>(ini.GetLongValue("UI", "iFrameStyle", frameStyle));
        // ⚠ fPlainRounding IS NOT READ. It was the plain path's single radius
        // for one day; the radius is per surface now, see Settings.h and
        // ChamferPanel::PlainRadiusScope. Save() deletes it from the file - not
        // here, because Load never writes one back and a Delete on this local
        // copy would be discarded with it.
        hoverPreview    = ini.GetBoolValue("UI", "bHoverPreview", hoverPreview);
        hoverPreviewCards =
            ini.GetBoolValue("UI", "bHoverPreviewCards", hoverPreviewCards);
        previewGrid      = ini.GetBoolValue("UI", "bPreviewGrid", previewGrid);
        browserShowClass  = ini.GetBoolValue("UI", "bBrowserShowClass", browserShowClass);
        browserShowPlugin = ini.GetBoolValue("UI", "bBrowserShowPlugin", browserShowPlugin);
        browserHideUnfit  = ini.GetBoolValue("UI", "bBrowserHideUnfit", browserHideUnfit);
        previewCardScale = static_cast<float>(ini.GetDoubleValue(
            "UI", "fPreviewCardScale", static_cast<double>(previewCardScale)));
        previewCardScale = std::clamp(previewCardScale, kCardScaleMin, kCardScaleMax);
        previewDiskCache = ini.GetBoolValue("UI", "bPreviewDiskCache", previewDiskCache);
        previewThumbPx   = static_cast<std::uint32_t>(ini.GetLongValue(
            "UI", "iPreviewThumbPx", static_cast<long>(previewThumbPx)));
        previewCacheMiB  = static_cast<std::uint32_t>(ini.GetLongValue(
            "UI", "iPreviewCacheMiB", static_cast<long>(previewCacheMiB)));
        bodyFitFilter   = ini.GetBoolValue("UI", "bBodyFitFilter", bodyFitFilter);
        sfwBodyCards    = ini.GetBoolValue("UI", "bSfwBodyCards", sfwBodyCards);
        advancedSlots   = ini.GetBoolValue("UI", "bAdvancedSlots", advancedSlots);
        lockLayout      = ini.GetBoolValue("UI", "bLockLayout", lockLayout);
        uiWidth         = static_cast<float>(
            ini.GetDoubleValue("UI", "fWidth", static_cast<double>(uiWidth)));
        if (uiWidth < 0.0f) {
            uiWidth = 0.0f;
        }

        sceneCompat = ini.GetBoolValue("Scene", "bSceneCompat", sceneCompat);
        // Presence-checked so an absent key keeps the C++ default (assigning a
        // GetValue default pointer that aliases our own buffer is unsafe).
        if (const char* v = ini.GetValue("Scene", "sSuspendEvents", nullptr); v && *v) {
            sceneSuspendEvents = v;
        }
        if (const char* v = ini.GetValue("Scene", "sResumeEvents", nullptr); v && *v) {
            sceneResumeEvents = v;
        }
        // ⚠ CLAMPED ON THE WAY IN. A hand-edited INI is the only way this can be
        // out of range, and CBPC is handed the number without looking at it.
        {
            const auto raw = ini.GetLongValue("Physics", "iBouncePercent",
                                              static_cast<long>(cbpcBouncePercent));
            cbpcBouncePercent = static_cast<int>(raw < 0 ? 0 : (raw > 100 ? 100 : raw));
        }
        if (const char* v = ini.GetValue("Compat", "sSamMenuName", nullptr); v && *v) {
            samMenuName = v;
        }
        blockInputWhileOpen =
            ini.GetBoolValue("Compat", "bBlockInputWhileOpen", blockInputWhileOpen);
        cameraDragWhileOpen =
            ini.GetBoolValue("Compat", "bCameraDragWhileOpen", cameraDragWhileOpen);
        cameraDragSensitivity = static_cast<float>(ini.GetDoubleValue(
            "Compat", "fCameraDragSensitivity", static_cast<double>(cameraDragSensitivity)));
        cameraFrameTarget =
            ini.GetBoolValue("Compat", "bCameraFrameTarget", cameraFrameTarget);
        cameraFocusSlot =
            ini.GetBoolValue("Compat", "bCameraFocusSlot", cameraFocusSlot);
        // ⚠ ABSENT KEEPS THE C++ DEFAULT, WHICH IS ON AGAIN AS OF 1.1.7. It
        // shipped off from 2026-08-29, and every ini written since carries an
        // explicit 0 that would be read here and kept, FOMOD or not: the
        // installer's rewrite reaches a mod-manager install and nothing else.
        // So the three face keys go through OS::LooksOn below, which moves a 0
        // under an old stamp once and leaves a stamped one alone.
        reassertAppearance =
            ini.GetBoolValue("Compat", "bReassertAppearance", reassertAppearance);
        // ⚠ ABSENT MEANS ON HERE, WHICH IS THE OPPOSITE POSTURE TO ITS
        // NEIGHBOURS AND IS THE POINT. Every ini written before 2026-08-29
        // lacks this key, so every existing install reads the default and gets
        // the colour repair back. See Settings.h for why this is a new key
        // instead of a migration of bReassertAppearance.
        keepColoursAfterRebuild = ini.GetBoolValue(
            "Compat", "bKeepColoursAfterRebuild", keepColoursAfterRebuild);
        // The RaceMenu half of a look apply. Same posture: absent takes the
        // C++ default, which is on.
        looksRaceMenu =
            ini.GetBoolValue("Compat", "bLooksRaceMenu", looksRaceMenu);
        // ⚠⚠ THE ONE-TIME FLIP (user 2026-09-02, "override the ini settings
        // that might be stale when people install the new fomod"). The Looks
        // page and the two keys above are one decision in three keys, and the
        // installer shipped all three off from 2026-08-29 to 1.1.6. A file
        // stamped below 4 holds that answer whoever wrote it; a file stamped 4
        // or later holds the player's, whichever way it points. Read here,
        // after the [Compat] keys and the [Pages] key are all in.
        {
            const int  stamp       = static_cast<int>(iniVersion);
            const bool wasLooks    = pages.looks;
            const bool wasReassert = reassertAppearance;
            const bool wasRaceMenu = looksRaceMenu;
            pages.looks        = LooksOn::Adopt(pages.looks, stamp);
            reassertAppearance = LooksOn::Adopt(reassertAppearance, stamp);
            looksRaceMenu      = LooksOn::Adopt(looksRaceMenu, stamp);
            if (wasLooks != pages.looks || wasReassert != reassertAppearance ||
                wasRaceMenu != looksRaceMenu) {
                spdlog::info("Settings: the Looks page and the face features are on "
                             "for this install once (bLooks {}->{}, "
                             "bReassertAppearance {}->{}, bLooksRaceMenu {}->{}): "
                             "the INI stamp is {} and 1.1.7 ships them on. Turning "
                             "any of them off in the panel or the INI stamps the "
                             "file and is left alone from then on.",
                             wasLooks, pages.looks, wasReassert, reassertAppearance,
                             wasRaceMenu, looksRaceMenu, iniVersion);
            }
        }
        gridInventoryHide =
            ini.GetBoolValue("Compat", "bGridInventoryHide", gridInventoryHide);
        preLoadPresetErase = ini.GetBoolValue("Compat", "bPreLoadPresetErase",
                                              preLoadPresetErase);

        overlayShowAllArt =
            ini.GetBoolValue("Overlays", "bOverlayShowAllArt", overlayShowAllArt);
        overlayKeepOffset =
            ini.GetBoolValue("Overlays", "bOverlayKeepOffset", overlayKeepOffset);

        editOtherNpcs = ini.GetBoolValue("Targets", "bEditOtherNpcs", editOtherNpcs);

        requipFlourish = ini.GetBoolValue("Requip", "bRequipFlourish", requipFlourish);
        requipAura     = ini.GetBoolValue("Requip", "bRequipAura", requipAura);
        requipSoundOn  = ini.GetBoolValue("Requip", "bRequipSoundOn", requipSoundOn);
        requipSoundId  = ini.GetValue("Requip", "sRequipSoundId", requipSoundId.c_str());
        requipFlashAll = ini.GetBoolValue("Requip", "bRequipFlashAllGear", requipFlashAll);
        requipSpeed    = static_cast<float>(
            ini.GetDoubleValue("Requip", "fRequipSpeed", static_cast<double>(requipSpeed)));
        // ⚠ CLAMPED HERE RATHER THAN AT THE CALL SITE, so no reader can meet an
        // unusable value. See the field's comment: a zero collapses the curve's
        // span and drops the garment to alpha 0 on the first frame.
        if (requipSpeed < 0.25f) {
            requipSpeed = 0.25f;
        } else if (requipSpeed > 4.0f) {
            requipSpeed = 4.0f;
        }
        requipColour = ini.GetValue("Requip", "sRequipColour", requipColour.c_str());
        requipArtForm =
            ini.GetValue("Requip", "sRequipArtForm", requipArtForm.c_str());
        requipShaderForm =
            ini.GetValue("Requip", "sRequipShaderForm", requipShaderForm.c_str());
        requipSlotArtForm =
            ini.GetValue("Requip", "sRequipSlotArtForm", requipSlotArtForm.c_str());

        dyeReflective = ini.GetBoolValue("Dye", "bDyeReflective", dyeReflective);
        dyeSortByHue  = ini.GetBoolValue("Dye", "bDyeSortByHue", dyeSortByHue);
        dyeKeepShine  = ini.GetBoolValue("Dye", "bDyeKeepShine", dyeKeepShine);
        dyePbrOnPbrPath =
            ini.GetBoolValue("Dye", "bDyePbrOnPbrPath", dyePbrOnPbrPath);
        dyePbrPearl = ini.GetBoolValue("Dye", "bDyePbrPearl", dyePbrPearl);
        dyePbrPearlSheenMax = static_cast<float>(ini.GetDoubleValue(
            "Dye", "fDyePbrPearlSheenMax", static_cast<double>(dyePbrPearlSheenMax)));
        dyeBlendName  = ini.GetValue("Dye", "sDyeBlend", dyeBlendName.c_str());
        dyePbrBlendName =
            ini.GetValue("Dye", "sDyePbrBlend", dyePbrBlendName.c_str());
        dyeUnlockCards =
            ini.GetBoolValue("Dye", "bDyeUnlockCards", dyeUnlockCards);
        dyeCardDwell = static_cast<float>(
            ini.GetDoubleValue("Dye", "fDyeCardDwell", static_cast<double>(dyeCardDwell)));
        dyeCardMax = static_cast<std::int32_t>(
            ini.GetLongValue("Dye", "iDyeCardMax", static_cast<long>(dyeCardMax)));
        dyeCardSound = ini.GetValue("Dye", "sDyeCardSound", dyeCardSound.c_str());
        dyeCardBurst = static_cast<std::int32_t>(
            ini.GetLongValue("Dye", "iDyeCardBurst", static_cast<long>(dyeCardBurst)));
        dyeTickSeconds = static_cast<std::int32_t>(
            ini.GetLongValue("Dye", "iDyeTickSeconds", static_cast<long>(dyeTickSeconds)));
        eyeAdvancedColour =
            ini.GetBoolValue("Dye", "bEyeAdvancedColour", eyeAdvancedColour);
        dyeRampDiffuseLo    = static_cast<float>(ini.GetDoubleValue(
            "Dye", "fDyeRampDiffuseLo", static_cast<double>(dyeRampDiffuseLo)));
        dyeRampDiffuseHi    = static_cast<float>(ini.GetDoubleValue(
            "Dye", "fDyeRampDiffuseHi", static_cast<double>(dyeRampDiffuseHi)));
        dyeRampReflectionLo = static_cast<float>(ini.GetDoubleValue(
            "Dye", "fDyeRampReflectionLo", static_cast<double>(dyeRampReflectionLo)));
        dyeRampReflectionHi = static_cast<float>(ini.GetDoubleValue(
            "Dye", "fDyeRampReflectionHi", static_cast<double>(dyeRampReflectionHi)));
        dyeRampReflectionAxis = static_cast<std::uint32_t>(ini.GetLongValue(
            "Dye", "iDyeRampReflectionAxis", static_cast<long>(dyeRampReflectionAxis)));
        dyeRampSheen = static_cast<float>(
            ini.GetDoubleValue("Dye", "fDyeRampSheen", static_cast<double>(dyeRampSheen)));
        dyeRampSpectral = ini.GetBoolValue("Dye", "bDyeRampSpectral", dyeRampSpectral);
        dyeRampAutoWindow = ini.GetBoolValue("Dye", "bDyeRampAutoWindow", dyeRampAutoWindow);
        dyeRampAutoLo     = static_cast<float>(
            ini.GetDoubleValue("Dye", "fDyeRampAutoLo", static_cast<double>(dyeRampAutoLo)));
        dyeRampAutoHi     = static_cast<float>(
            ini.GetDoubleValue("Dye", "fDyeRampAutoHi", static_cast<double>(dyeRampAutoHi)));
        dyeEyeMaskLo      = static_cast<float>(
            ini.GetDoubleValue("Dye", "fDyeEyeMaskLo", static_cast<double>(dyeEyeMaskLo)));
        dyeEyeMaskHi      = static_cast<float>(
            ini.GetDoubleValue("Dye", "fDyeEyeMaskHi", static_cast<double>(dyeEyeMaskHi)));
        dyeEyeIrisRadius  = static_cast<float>(
            ini.GetDoubleValue("Dye", "fDyeEyeIrisRadius", static_cast<double>(dyeEyeIrisRadius)));
        dyeEyeIrisSoft    = static_cast<float>(
            ini.GetDoubleValue("Dye", "fDyeEyeIrisSoft", static_cast<double>(dyeEyeIrisSoft)));
        dyeEyeMaskBroad   = static_cast<float>(
            ini.GetDoubleValue("Dye", "fDyeEyeMaskBroad", static_cast<double>(dyeEyeMaskBroad)));

        heartbeatSeconds = static_cast<float>(
            ini.GetDoubleValue("Rules", "fHeartbeatSeconds", static_cast<double>(heartbeatSeconds)));
        minDwellSeconds = static_cast<float>(
            ini.GetDoubleValue("Rules", "fMinDwellSeconds", static_cast<double>(minDwellSeconds)));
        castHoldSeconds = static_cast<float>(
            ini.GetDoubleValue("Rules", "fCastHoldSeconds", static_cast<double>(castHoldSeconds)));

        // ⚠ THE PLAYSTYLE KEYS ARE NAMED HERE ON PURPOSE. "The installer put me
        // on the wrong playstyle" is otherwise unanswerable from a log: the
        // three keys the FOMOD writes, and the path they were read from, are
        // the whole of what separates a mis-picked install from a losing MO2
        // conflict. Path first, because more than one FittingRoom.ini can exist
        // and only one of them wins.
        installerStamp = ini.GetValue("General", "sInstallerStamp", "");
        spdlog::info("Settings loaded from '{}' (iCostMode={}, bRequireSeamstone={}, "
                     "bRequireWornForStyles={}, bDyeUnlocks={}, bLooks={}, "
                     "blocklist=0x{:X}, editor key=0x{:X}, installer stamp '{}').",
                     IniPath(), static_cast<long>(costMode), requireSeamstone,
                     requireWornForStyles, dyeUnlocks, pages.looks, slotBlocklist,
                     editorKeyDIK, installerStamp);
        // ⚠⚠ THE INSTALLER'S ANSWERS LAND ONCE PER INSTALL, WHATEVER FILE THIS
        // READ. Measured on the author's rig 2026-09-02 08:30: the FOMOD wrote
        // its ini into FittingRoom-1.1.7\SKSE\Plugins\FittingRoom.ini and the
        // game read and saved MODS\overwrite\SKSE\Plugins\FittingRoom.ini,
        // which outranks every mod, so "Earn dyes" never arrived (user: "i
        // don't think our fomod settings are updating from what we pick from
        // the fomod"). The installer also ships the same file under a name the
        // plugin never writes, and a leftover cannot shadow that one.
        if (SeedFromInstaller()) {
            Save();
        }
    }

    // The installer's answers, applied over whatever Load read.
    //
    // ⚠ ONLY THE EIGHT KEYS THE INSTALLER'S THREE QUESTIONS DECIDE, each read
    // with the live value as its default, so an installer file from a build
    // that lacked one of them leaves that one alone. Everything else in the
    // live file is the player's and is not touched. A same stamp is a no-op,
    // which is what keeps a player's later panel edits: the stamp names the
    // zip and the answers, so only a new install or a different set of
    // answers seeds again.
    bool Settings::SeedFromInstaller() {
        const auto  path = BuildChannel::DataPath("installer.ini").string();
        CSimpleIniA inst;
        inst.SetUnicode();
        if (inst.LoadFile(path.c_str()) < 0) {
            return false;  // no installer file: a hand-copied install
        }
        const std::string stamp = inst.GetValue("General", "sInstallerStamp", "");
        if (!InstallerSeed::ShouldSeed(installerStamp, stamp)) {
            return false;
        }
        const std::string was = installerStamp;
        costMode = CostModeFrom(
            inst.GetLongValue("General", "iCostMode", static_cast<long>(costMode)));
        requireSeamstone =
            inst.GetBoolValue("General", "bRequireSeamstone", requireSeamstone);
        requireWornForStyles =
            inst.GetBoolValue("General", "bRequireWornForStyles", requireWornForStyles);
        collectionOnly = inst.GetBoolValue("General", "bCollectionOnly", collectionOnly);
        dyeUnlocks     = inst.GetBoolValue("General", "bDyeUnlocks", dyeUnlocks);
        pages.looks    = inst.GetBoolValue("Pages", "bLooks", pages.looks);
        reassertAppearance =
            inst.GetBoolValue("Compat", "bReassertAppearance", reassertAppearance);
        looksRaceMenu = inst.GetBoolValue("Compat", "bLooksRaceMenu", looksRaceMenu);
        installerStamp = stamp;
        spdlog::info("Settings: took the installer's answers from '{}' (stamp '{}', the "
                     "file Load read had '{}'): iCostMode={}, bRequireSeamstone={}, "
                     "bRequireWornForStyles={}, bCollectionOnly={}, bDyeUnlocks={}, "
                     "bLooks={}, bReassertAppearance={}, bLooksRaceMenu={}. That file "
                     "is saved with the stamp now, so this happens once per install.",
                     path, stamp, was, static_cast<long>(costMode), requireSeamstone,
                     requireWornForStyles, collectionOnly, dyeUnlocks, pages.looks,
                     reassertAppearance, looksRaceMenu);
        return true;
    }

    void Settings::Save() {
        CSimpleIniA ini;
        ini.SetUnicode();
        // Preserve sections Save() does not own (notably the hand-written
        // [Outfit] stopgap) instead of rewriting the file from scratch.
        ini.LoadFile(IniPath().c_str());
        ini.SetBoolValue("General", "bEnabled", enabled);
        // ⚠ THE STAMP GOES IN ON EVERY SAVE, and that is what makes the
        // migrations in Load ONE-TIME events rather than something that fights
        // a player who really does want the top of the range, or the carve on
        // every theme. Until a save happens they keep handing out the current
        // default, which is the same answer, so nothing is inconsistent in
        // the gap.
        //
        // ⚠⚠ ONE NUMBER FOR ALL OF THEM, AND IT IS THE HIGHEST. Each migration
        // names the stamp it needs and reads the file's own number against
        // that, so a file stamped 2 has been through both; writing anything
        // lower here would let a later build re-run a migration this one has
        // already applied.
        static_assert(kSettingsVersion >= UiScale::kSettingsVersion &&
                          kSettingsVersion >= LooksOn::kSettingsVersion,
                      "the stamp written here must be the highest any migration "
                      "gates on, or an older migration re-runs");
        ini.SetLongValue("General", "iSettingsVersion",
                         static_cast<long>(kSettingsVersion),
                         "; which migrations this file has already been "
                         "through. Do not edit.");
        ini.SetValue("General", "sInstallerStamp", installerStamp.c_str(),
                     "; which installer answers this file has taken, so a fresh "
                     "install's choices land even when an older FittingRoom.ini in "
                     "Overwrite or a mod above shadows the one the installer wrote. "
                     "Do not edit.");
        // ⚠ bUseGold IS NOT WRITTEN BACK, AND IT IS NOT DELETED EITHER. Load
        // still reads it as the default for an absent iCostMode, so a file that
        // already has it keeps a correct answer if the new key is ever removed
        // by hand. It cannot contradict anything: Save always writes iCostMode,
        // and iCostMode always wins.
        ini.SetLongValue("General", "iCostMode", static_cast<long>(costMode),
                         "; what applying an outfit costs. 0 = nothing, 1 = gold "
                         "(iGoldPerSlot per changed slot), 2 = the Seamstone's "
                         "charge (iChargePerSlot per changed slot). Previewing is "
                         "always free either way; you only pay when you commit. "
                         "Replaces the older bUseGold, which is still read when "
                         "this key is missing");
        ini.SetBoolValue("General", "bRequireSeamstone", requireSeamstone,
                         "; require carrying the Seamstone to open the editor (needs the lore ESP)");
        ini.SetBoolValue("General", "bCollectionOnly", collectionOnly,
                         "; style browser default: only looks you have owned");
        ini.SetBoolValue("General", "bReplaceOnLookApply", replaceOnLookApply,
                         "; applying a look also takes off what it does not "
                         "carry (overlays, shape, skin pack), instead of "
                         "leaving the character's own");
        ini.SetBoolValue("General", "bCollectionShared", collectionShared,
                         "; looks found by any character are offered to all of "
                         "them, through collection-shared.json beside this file. "
                         "Which pieces a character has SEEN is never shared, so "
                         "an inherited wardrobe still arrives with something to "
                         "discover. Add-only: nothing removes a look from the "
                         "file, so turning this back off leaves what is in it");
        ini.SetBoolValue("General", "bOutfitsShared", outfitsShared,
                         "; a NEW character starts with the outfits in "
                         "outfits.json instead of an empty list. Off by "
                         "default: outfits belong to the character who made "
                         "them. Characters you have already played keep their "
                         "own either way, and turning this on changes nothing "
                         "for them, only for the next new game");
        ini.SetBoolValue("General", "bDyeUnlocks", dyeUnlocks,
                         "; a dye has to be earned before you can pick it. Off "
                         "makes every colour pickable; colours already on a "
                         "garment keep showing either way");
        ini.SetBoolValue("General", "bDyeUnlocksShared", dyeUnlocksShared,
                         "; colours earned on any character are offered to all "
                         "of them, through dye-unlocks-shared.json beside this "
                         "file. The Seamstone charge and the deed counters are "
                         "NOT shared and stay in each save. Add-only: nothing "
                         "removes a colour from the file, so turning this back "
                         "off leaves it on disk");
        ini.SetBoolValue("General", "bRequireWornForStyles", requireWornForStyles,
                         "; a style only shows if you wear real gear under some part of it");
        ini.SetBoolValue("General", "bHideEquippedOnImport", hideEquippedOnImport,
                         "; when a preset does not cover a slot, hide the gear you "
                         "really have on there, so an imported set does not arrive "
                         "wearing your own helmet");
        ini.SetBoolValue("Pages", "bStyles", pages.styles,
                         "; which pages appear in the editor's rail. A page switched"
                         " off here is one you chose to hide; a page that is missing"
                         " anyway needs a mod you do not have, and the panel says"
                         " which.");
        ini.SetBoolValue("Pages", "bPresets", pages.presets);
        ini.SetBoolValue("Pages", "bDye", pages.dye);
        ini.SetBoolValue("Pages", "bRules", pages.rules);
        ini.SetBoolValue("Pages", "bBodies", pages.bodies);
        ini.SetBoolValue("Pages", "bShape", pages.shape);
        ini.SetBoolValue("Pages", "bOverlays", pages.overlays);
        ini.SetBoolValue("Pages", "bLooks", pages.looks);
        ini.SetBoolValue("Tutorial", "bEnabled", tutorialsOn,
                         "; show the tutorial cards at all. Turned off by "
                         "answering no on the first one, and back on by "
                         "Replay tutorials in the settings panel");
        ini.SetBoolValue("Tutorial", "bWelcome", tutorialWelcome,
                         "; the short tour of the page rail, shown once. Set any "
                         "key in this section to 0 to see that tutorial again, or "
                         "use Replay tutorials in the settings panel");
        ini.SetBoolValue("Tutorial", "bOutfits", tutorialOutfits,
                         "; the Outfits page tutorial, shown the first time you open it");
        ini.SetBoolValue("Tutorial", "bDye", tutorialDye, "; the Dye page tutorial");
        ini.SetBoolValue("Tutorial", "bPresets", tutorialPresets,
                         "; the Presets page tutorial");
        ini.SetBoolValue("Tutorial", "bRules", tutorialRules, "; the Rules page tutorial");
        ini.SetBoolValue("Tutorial", "bShape", tutorialShape, "; the Shape page tutorial");
        // ⚠ THE KEY KEEPS THE OLD WORD AND THE COMMENT DOES NOT. Renaming
        // bBodyStudio would hand every existing FittingRoom.ini a key nothing
        // reads, and the page's tutorial would offer itself again to players
        // who have already dismissed it.
        ini.SetBoolValue("Tutorial", "bBodyStudio", tutorialBodyStudio,
                         "; the Bodies page tutorial");
        ini.SetBoolValue("Tutorial", "bOverlays", tutorialOverlays,
                         "; the Overlays page tutorial");
        ini.SetBoolValue("Tutorial", "bLooks", tutorialLooks,
                         "; the Looks page tutorial");
        ini.SetBoolValue("Tutorial", "bClosing", tutorialClosing,
                         "; the send-off, shown once every other tutorial is done");
        ini.SetBoolValue("Advanced", "bSceneKick", sceneKick);
        ini.SetBoolValue("Debug", "bDumpBiped", dumpBiped);
        if constexpr (BuildChannel::kBodyStudio) {
            ini.SetBoolValue("Debug", "bBodyStudioProof", bodyStudioProof,
                             "; enable the Bodies workbench and its RaceMenu/OBody "
                             "ownership handling. 0 greys the rail tile out");
        }
        ini.SetBoolValue("Debug", "bHairFaceDump", hairFaceDump,
                         "; OS-95: log the follower's face node (children, per-geometry "
                         "bone count and bone0) before and after every publish. Game "
                         "thread only");
        ini.SetBoolValue("Debug", "bDyeCensus", dyeCensus,
                         "; census the material and vertex data of every shape the dye "
                         "walk sees. Read-only: writes to no material and no property");
        ini.SetBoolValue("Debug", "bAppearanceWatch", appearanceWatch,
                         "; watch everything that paints your character and write a "
                         "line only when one of them changes: hair colour and the "
                         "colour form's own value, skin tone, overlays, head parts. "
                         "On by default and silent on a settled save. Turn it off to "
                         "keep the log to what the mod itself did");
        // ⚠ ACTIVELY REMOVED, NOT MERELY NO LONGER WRITTEN, and for the reason
        // iHeadPartSlotProbe below records: SimpleIni keeps every key an INI
        // already on disk has, so a rig that ran the dyeing-eyes instruments
        // would keep bHeadCensus = true forever and get a census out of a build
        // that no longer has one. The eye and head-part rules they authored
        // shipped and were field-confirmed before 1.0.0.
        ini.Delete("Debug", "bHeadCensus", true);
        ini.SetBoolValue("Debug", "bEyeDyeSpike", eyeDyeSpike,
                         "; put sEyeDyeSpikeHex on the iris of whoever the dye walk "
                         "repaints. The target is the CURRENT EYES HEAD PART, matched to "
                         "the geometry whose name equals its editor id, so it works on an "
                         "eye whose material feature is not kEye. Nothing is persisted and "
                         "the existing restore undoes it");
        ini.SetValue("Debug", "sEyeDyeSpikeHex", eyeDyeSpikeHex.c_str(),
                     "; the spike's colour, RRGGBB. Try several: the overlay blend keeps a "
                     "saturated texture's hue, so a deep blue eye asked to go amber may "
                     "not move at all, and that is the question the spike exists to answer");
        // ⚠ sEyeDyeSplitHex IS DELETED FROM THE KEY, AND ALSO FROM THE FILE.
        // SimpleIni writes back only what is set here, but an INI already on
        // disk keeps every key it has, so leaving the delete at "stop writing
        // it" would leave an armed line in every install that ever ran the
        // spike, still doing nothing visible and still explaining nothing. The
        // key is actively removed below.
        ini.Delete("Debug", "sEyeDyeSplitHex", true);
        ini.SetBoolValue("Debug", "bEyeDyeSkipRepick", eyeDyeSkipRepick,
                         "; swap the eye's material without asking the engine to rebuild "
                         "the render pass list. The negative control already eliminated "
                         "the colour, so this is the last thing the swap does that can "
                         "change how the eye is DRAWN");
        ini.SetBoolValue("Debug", "bEyeDyeNegativeControl", eyeDyeNegativeControl,
                         "; run the eye swap with the texture store SKIPPED. The clone, the "
                         "cache round trip, the SetMaterial intern and the render pass "
                         "invalidation all still happen; no tinted texture is built or "
                         "written. If the iris STILL goes purple in this mode then the dye "
                         "colour is not involved at all and the machinery is the cause");
        // ⚠ RETIRED WITH sEyeDyeSplitHex AND DELETED FROM THE FILE FOR ITS
        // REASON. The eye's curve is a per-outfit choice on the dye pane now,
        // so an armed key here would be a second author of it, and an armed key
        // deciding what an eye renders is exactly the bug that ate the sclera.
        // It is removed rather than merely unwritten because an INI on disk
        // keeps every key it has.
        ini.Delete("Debug", "bEyeDyeRecolour", true);
        ini.SetLongValue("Debug", "iDyeEyeCapPx", static_cast<long>(eyeDyeCapPx),
                         "; the longest side a dyed EYE texture may have, in pixels, on "
                         "top of the install's own caps. 0 lets those decide. Measured: an "
                         "uncapped demon eye is a 4096 twin at 87 MiB and cycling colours "
                         "held 508 of a 512 MiB budget, for a surface a few dozen pixels "
                         "across on screen");
        // Retired with bHeadCensus above, and deleted from the file for the
        // same reason.
        ini.Delete("Debug", "bHeadPartTypeCounter", true);
        // ⚠ ACTIVELY REMOVED, NOT MERELY NO LONGER WRITTEN. The custom
        // head-part slots this probe existed to prove shipped on 2026-08-13
        // (item 7, field-confirmed), so the key is a second author of what the
        // character is WEARING: it made the next editor open put a horn or an
        // ear on the player and write it to the actor base. Any install that
        // ever ran the spike still has the line armed, and SimpleIni keeps
        // every key an INI already on disk has, so "stop writing it" would
        // leave it working. Same removal sEyeDyeSplitHex needed above, for the
        // same reason and after the same lesson.
        ini.Delete("Debug", "iHeadPartSlotProbe", true);
        ini.SetLongValue("Debug", "iDyeSpikeRung", static_cast<long>(dyeSpikeRung),
                         "; feature-preserving tint spike, task 2. 0 = off (the shipped "
                         "FacegenTint swap), 1 = rung 1, clone the material at its OWN "
                         "feature and tint the specular fields, 2 = rung 1's negative "
                         "control, 3 = rung 2, tint the property's own emissiveColor and "
                         "touch no material, 4 = rung 2's negative control. Run each "
                         "control before believing its rung. EVERY non-zero mode REPLACES "
                         "the shipped dye rather than running beside it, so nothing is "
                         "dyed normally while one is set");
        ini.SetDoubleValue("Debug", "fDyeEnvScale", static_cast<double>(dyeEnvScale),
                           "; OS-139 finish probe, and it only does anything with "
                           "iDyeSpikeRung = 5. How mirror-like a dyed reflective piece is: "
                           "0 is matte and larger is shinier. NEGATIVE leaves every piece "
                           "with the value its own mesh shipped, which is the default "
                           "because there is no single vanilla number to go back to. The "
                           "log prints each piece's original beside whatever this writes");
        ini.SetBoolValue("Debug", "bDyeTintReflection", dyeTintReflection,
                         "; OS-139, and it needs iDyeSpikeRung = 5. Give the REFLECTION the "
                         "dye's colour too, by tinting the mesh's envmap mask. This is the "
                         "one fDyeEnvScale cannot do: scale only changes how strong an "
                         "unchanged gold reflection is, so nothing shows the dye and keeps "
                         "the metal looking like metal at the same time. Leave fDyeEnvScale "
                         "negative and turn this on. Meshes that bake reflectivity into "
                         "another map instead of shipping a mask are left alone and say so "
                         "in the log");
        ini.SetLongValue("Debug", "iDyeTextureBudgetMiB",
                         static_cast<long>(dyeTextureBudgetMiB),
                         "; how much video memory the OS-139 tinted-texture cache may hold "
                         "before it frees what nothing is rendering from. A 4096 diffuse "
                         "costs about 87 MiB per colour, so this fills faster than it looks. "
                         "Only textures no garment is currently using are freed, so a small "
                         "budget costs a rebuild rather than breaking anything");
        ini.SetLongValue("Debug", "iDyePreviewCapPx", static_cast<long>(dyePreviewCapPx),
                         "; OS-140. The longest side a PREVIEW dye texture may have. Every "
                         "colour is built at this size first, including the ones nobody is "
                         "dragging, so a dye appears in a frame instead of after a full "
                         "build. 512 costs about 1.3 MiB where a 4096 costs 85. It reads a "
                         "smaller level of the texture the game already has, so a capped "
                         "build is cheaper in GPU time too. 0 means no cap");
        ini.SetLongValue("Debug", "iDyeCommitCapPx", static_cast<long>(dyeCommitCapPx),
                         "; OS-140. The longest side a COMMITTED dye texture may have, which "
                         "is the one you keep looking at. This is the only setting that "
                         "changes how much video memory a dressed character costs: 2048 is a "
                         "quarter of 4096 and the difference shows up close and rarely at "
                         "conversation distance. 0 means no cap, which is what the memory "
                         "figures in the log were measured against");
        ini.SetLongValue("Debug", "iDyeSettleMs", static_cast<long>(dyeSettleMs),
                         "; OS-140. How long a colour must hold still before the full "
                         "quality build is queued for it. Dragging the colour picker posts a "
                         "new colour every frame, so without a wait every colour on the way "
                         "past gets built at full size. Larger means fewer wasted builds and "
                         "a longer pause before the dye sharpens");
        ini.SetLongValue("Debug", "iOverlayBakeCapPx", static_cast<long>(overlayBakeCapPx),
                         "; OS-209. The longest side of an overlay baked by the Position "
                         "sliders on the Overlays page. The bake is uncompressed, so 2048 is "
                         "about 21 MiB on disk and in video memory and 4096 is four times "
                         "that. 0 means the overlay's own size");
        ini.SetLongValue("Debug", "iOverlayBakeCacheMiB", static_cast<long>(overlayBakeCacheMiB),
                         "; OS-209. How much the folder of baked overlays under "
                         "textures\\FittingRoom\\baked may hold before the oldest are deleted. "
                         "A save that names a deleted bake shows that layer as the game's "
                         "placeholder until it is baked again. 0 means never delete");
        // ⚠ ACTIVELY REMOVED, AND THIS ONE WAS ARMED IN A LIVE INI. OS-226's
        // overlay probe read every layer on the player into the log on a key
        // press; it went with OverlayProbe.{h,cpp} at the 1.0.0 release, once
        // OS-233's reconcile had landed and been field-confirmed. The
        // development rig still had it bound to F11, and SimpleIni keeps every
        // key an INI on disk already has, so leaving it merely unwritten would
        // leave a dead key in the file for anyone who reads their own INI to
        // find out what the mod does. A spike key must die when its feature
        // lands.
        ini.Delete("Debug", "iOverlayProbeKeyDIK", true);
        // ChargenProbe served phase 0 (all three gates read 2026-08-21) and
        // died with W1 landing, per its own contract. Same rule as the
        // overlay probe above: a spike key must die when its feature lands.
        ini.Delete("Debug", "iChargenProbeKeyDIK", true);
        // W1's field harness on the same sacrificed key: press to capture
        // the player into the profile "F11 Look", press again to apply it
        // back. A spike like its predecessors; it dies with the Looks page
        // (W2). 0 is disarmed; F11 is 87 on this rig.
        ini.SetLongValue("Debug", "iProfileProbeKeyDIK",
                         static_cast<long>(profileProbeKeyDIK),
                         "; W1 probe for character profiles. Bind a key "
                         "(87 = F11): one press captures 'F11 Look', the "
                         "next applies it. 0 turns it off");
        // OS-231. The scrape wraps five ActionScript functions on RaceMenu's
        // live movie, which is code of ours running inside a menu's own
        // AdvanceMovie tick, and the preset load CTD died exactly one menu
        // advance downstream of somebody's scribble. Turning the scrape off
        // for one launch is the A/B that separates "ours" from "not ours"
        // without unticking the whole mod.
        ini.SetBoolValue("Debug", "bPaintScrape", paintScrape,
                         "; read RaceMenu's overlay pack lists while its menu "
                         "is open, so discovered paints show up in the picker. "
                         "Turn off only to test whether the scrape is behind a "
                         "crash in the RaceMenu menu");
        ini.SetBoolValue("Debug", "bDyeEmissiveProbe", dyeEmissiveProbe,
                         "; rung 2's blocking question and only the question. Records the "
                         "emissiveColor POINTER, its multiplier and kOwnEmit per shape and "
                         "counts how many properties name each allocation. Read-only: "
                         "writes to no property, no material and no colour");
        ini.SetLongValue("Debug", "iDyeGpuTask", static_cast<long>(dyeGpuTask),
                         "; OS-139, the GPU diffuse dye spike. 0 = off. 1 = task 1, prove a "
                         "compute dispatch runs on the game's own device. 2 = task 2, time a "
                         "BC7-to-RGBA8 tint at 4096, 2048 and 1024 square. 3 = task 3, tint "
                         "the diffuse a worn shape is really using and put the copy back. "
                         "Each one runs once, after the editor has been opened, and writes "
                         "its verdict to the log. Tasks 1 and 2 touch no material and nothing "
                         "the game renders, though task 2 holds up to 80 MiB of VRAM while it "
                         "measures. TASK 3 CHANGES WHAT YOU SEE: it recolours one garment on "
                         "the player and keeps it that way for the rest of the session. "
                         "Nothing is written to disk, so a reload clears it");
        // ⚠ THE RETIRED SPIKE KEY GOES, rather than being left to sit there
        // reading false at everybody who ever saved their settings. It moved to
        // [Dye] bSeamstoneCharging and defaults ON; leaving the old one on disk
        // would give the same state two authors, and the stale one would win
        // for exactly the players who have used the mod longest.
        ini.Delete("Debug", "bItemCardCharge", true);
        ini.SetLongValue("Debug", "iItemCardType", static_cast<long>(itemCardType),
                         "; with bSeamstoneCharging on: what to tell the item card the "
                         "Seamstone is, which is what decides how it lays itself out. 2 = a "
                         "weapon, the only inventory layout known to draw a charge meter, "
                         "and it brings a DAMAGE row with it. 3 = what a MISC item really "
                         "is. -1 = leave it alone. -2 = step through 0..16, one value per "
                         "time the card is rebuilt, so scrolling off the stone and back "
                         "tries the next layout and the log says which is which");
        ini.SetLongValue("Debug", "iItemCardShape", static_cast<long>(itemCardShape),
                         "; how much of a real enchanted weapon's card to imitate, since what "
                         "makes your skin draw a charge meter lives in its .swf and cannot be "
                         "read out of the game. 0 = the charge fields only, 1 = plus the effect "
                         "text, 2 = plus damage, poisoned and damage change, which is "
                         "everything the game itself writes for an enchanted weapon");
        ini.SetValue("Debug", "sDiagnosePlugin", diagnosePlugin.c_str(),
                     "; log the catalog/detection fate of ARMOs whose name or "
                     "plugin contains this substring (blank = off); e.g. Abyss");
        ini.SetLongValue("Input", "iEditorKeyDIK", static_cast<long>(editorKeyDIK),
                         "; 0 = unbound");
        ini.SetLongValue("Input", "iEditorGamepadButton", static_cast<long>(editorGamepadButton),
                         "; 0 = unbound");
        ini.SetLongValue("Input", "iDirectEntryKeyDIK",
                         static_cast<long>(directEntryKeyDIK),
                         "; Enter Fitting Room from anywhere. 0 = unassigned, which is\n"
                         "; the default: it summons the inventory to be hosted by, so it\n"
                         "; has to be a key the player picked. DIK scan code.");
        ini.SetLongValue("Input", "iNextOutfitKeyDIK", static_cast<long>(nextOutfitKeyDIK),
                         "; 0 = unbound");
        // bLoreMode has not existed for a long time; iCostMode=1 is what turns
        // this on now.
        ini.SetLongValue("Lore", "iGoldPerSlot", static_cast<long>(goldPerSlot),
                         "; gold charged per styled slot while iCostMode=1. 0 disables");
        ini.SetLongValue("Lore", "iDyeCost", static_cast<long>(dyeCost),
                         "; What one dye costs to unlock. Separate from "
                         "iGoldPerSlot, which prices applying an outfit.");
        ini.SetLongValue("Lore", "iGoldPerDye", static_cast<long>(goldPerDye),
                         "; gold charged per changed dye channel while "
                         "iCostMode=1. Defaults to 0, because gold shipped "
                         "billing slots only and turning this on by itself "
                         "would reprice every existing game. Separate from "
                         "iDyeCost, which UNLOCKS a colour");
        ini.SetLongValue("Lore", "iChargePerDye", static_cast<long>(chargePerDye),
                         "; Seamstone charge spent per changed dye channel while "
                         "iCostMode=2. 0 disables");
        ini.SetLongValue("Lore", "iChargePerSlot", static_cast<long>(chargePerSlot),
                         "; Seamstone charge spent per styled slot while "
                         "iCostMode=2. 0 disables. Its own number rather than a "
                         "share of iGoldPerSlot, because gold is loot and charge "
                         "comes out of soul gems");
        ini.SetLongValue("Lore", "iGoldPerLook", static_cast<long>(goldPerLook),
                         "; gold charged per changed appearance dimension while "
                         "iCostMode=1: the body preset, hair visibility, hair "
                         "colour, hair style, eyes, brows. Each counts once. "
                         "These were free until 0.4.0, by accident rather than "
                         "design; set 0 for the old behaviour");
        ini.SetLongValue("Lore", "iChargePerLook", static_cast<long>(chargePerLook),
                         "; Seamstone charge spent per changed appearance "
                         "dimension while iCostMode=2. 0 disables");
        ini.SetLongValue("Lore", "iGoldPerLooksMenu", static_cast<long>(goldPerLooksMenu),
                         "; gold taken once when the character editor opens while "
                         "iCostMode=1. A flat door fee rather than a bill: RaceMenu "
                         "is another mod's window and nothing here can count what "
                         "was sculpted in it. 0 makes entry free");
        ini.SetLongValue("Lore", "iChargePerLooksMenu",
                         static_cast<long>(chargePerLooksMenu),
                         "; Seamstone charge taken once when the character editor "
                         "opens while iCostMode=2. 0 makes entry free");
        ini.SetValue("Lore", "sChargeSpendSound", chargeSpendSound.c_str(),
                     "; sound descriptor editor ID played when the Seamstone is "
                     "spent. Blank for none. If you hear nothing, the log names "
                     "the id it could not find");
        ini.SetValue("Lore", "sChargeFillSound", chargeFillSound.c_str(),
                     "; sound descriptor editor ID played when the Seamstone is "
                     "refilled from a soul gem. Blank for none");
        ini.SetValue("Lore", "sGoldSpendSound", goldSpendSound.c_str(),
                     "; sound descriptor editor ID played when you pay gold for a "
                     "style, with iCostMode=1. Blank for none");
        ini.SetDoubleValue("Lore", "fEnchantingXpPerCharge",
                           static_cast<double>(enchantingXpPerCharge),
                           "; how fast refilling the Seamstone from Fitting "
                           "Room's own Refill button trains Enchanting, per "
                           "point of charge that actually lands. This is NOT "
                           "experience directly: the game multiplies it by "
                           "Enchanting's own 900x skill use multiplier, so "
                           "0.00006 pays about 54 experience for a common soul, "
                           "a tenth of a level at skill 20. A petty soul is 250 "
                           "charge, a grand one 3000. 0 disables");
        ini.SetLongValue("Lore", "iSeamstoneCapacity", static_cast<long>(seamstoneCapacity),
                         "; how much charge the Seamstone holds when full. "
                         "Lowering it leaves an already fuller stone alone rather "
                         "than draining it down to the new ceiling");
        ini.SetLongValue("Advanced", "uSlotBlocklist", static_cast<long>(slotBlocklist),
                         "; biped-slot bitmask the mod must never touch (hex ok)", true /*a_bUseHex*/);
        ini.SetDoubleValue("UI", "fFontSize", static_cast<double>(menuFontSize),
                           "; editor menu font size in pixels");
        ini.SetDoubleValue("UI", "fUiScale", static_cast<double>(uiScale),
                           "; live editor UI scale (0.4-1.2); the in-editor slider sets this");
        ini.SetLongValue("UI", "iFrameStyle", frameStyle,
                         "; corner shape for our own chrome: 1 carved (the "
                         "default), 2 plain");
        // ⚠ REMOVED RATHER THAN LEFT UNWRITTEN, the same rule fPlainRounding
        // below is under. sCarvedPresets told auto which FLICK presets the carve
        // was for, auto is gone, and a key a player can still edit with nothing
        // behind it is worse than no key at all.
        ini.Delete("UI", "sCarvedPresets", true);
        // ⚠ REMOVED RATHER THAN LEFT UNWRITTEN. Save() edits the file in place
        // - it loads the existing INI at the top and writes it back - so simply
        // not setting a key leaves whatever was already on that line. Nothing
        // reads fPlainRounding any more, and a line a player can still edit with
        // nothing behind it is worse than no line at all: the plain radius is a
        // property of each surface now, not one number to set. See Settings.h.
        ini.Delete("UI", "fPlainRounding", true);
        ini.SetBoolValue("UI", "bHoverPreview", hoverPreview,
                         "; preview a style on the character just by hovering its row");
        ini.SetBoolValue("UI", "bHoverPreviewCards", hoverPreviewCards,
                         "; the same for the picture cards, which default to OFF "
                         "because a card already shows you the thing and a grid is "
                         "scanned rather than read");
        ini.SetBoolValue("UI", "bPreviewGrid", previewGrid,
                         "; the weapon browser shows picture cards instead of the "
                         "name list. The toggle on the filter row writes this too");
        ini.SetBoolValue("UI", "bBrowserShowClass", browserShowClass,
                         "; show the Type column in the style browser. The name column "
                         "is always shown");
        ini.SetBoolValue("UI", "bBrowserShowPlugin", browserShowPlugin,
                         "; show the Mod column in the style browser, naming the plugin "
                         "each look came from");
        ini.SetBoolValue("UI", "bBrowserHideUnfit", browserHideUnfit,
                         "; hide looks that do not fit the body you are wearing. They "
                         "are the red rows; off shows them and they can still be picked");
        ini.SetDoubleValue("UI", "fPreviewCardScale", static_cast<double>(previewCardScale),
                           "; picture card side in font-size units (6-14)");
        ini.SetBoolValue("UI", "bPreviewDiskCache", previewDiskCache,
                         "; keep rendered thumbnails on disk between sessions, so "
                         "a browsed page is instant next time");
        ini.SetLongValue("UI", "iPreviewThumbPx", static_cast<long>(previewThumbPx),
                         "; thumbnail texture size in pixels. Changing it discards "
                         "every cached thumbnail, which is correct rather than a bug: "
                         "the cache only serves images rendered at this size");
        ini.SetLongValue("UI", "iPreviewCacheMiB", static_cast<long>(previewCacheMiB),
                         "; how much disk the thumbnail cache may hold before the "
                         "oldest images are dropped");
        ini.SetBoolValue("UI", "bAdvancedSlots", advancedSlots,
                         "; editor default: show every biped slot (else the common set)");
        ini.SetBoolValue("UI", "bBodyFitFilter", bodyFitFilter,
                         "; the body lists offer only presets built for the body "
                         "this character wears. 0 offers every preset, whether it "
                         "suits them or not");
        ini.SetBoolValue("UI", "bSfwBodyCards", sfwBodyCards,
                         "; body preview cards are built from the body mod's own "
                         "covered slider set where one is installed. Works for 3BA, "
                         "CBBE and HIMBO, which all ship a nevernude build. UBE "
                         "ships none, so UBE presets draw unchanged");
        ini.SetBoolValue("UI", "bLockLayout", lockLayout,
                         "; lock the editor window position/size (uncheck the gear's Lock window to move/resize)");
        ini.SetDoubleValue("UI", "fWidth", static_cast<double>(uiWidth),
                           "; editor width in pixels once its right edge has been "
                           "dragged; 0 uses the computed default");
        ini.SetBoolValue("Scene", "bSceneCompat", sceneCompat,
                         "; suspend transmog + block the editor while a scene mod (OStim) runs");
        ini.SetValue("Scene", "sSuspendEvents", sceneSuspendEvents.c_str(),
                     "; Papyrus mod-event names (comma list) that START a scene");
        ini.SetValue("Scene", "sResumeEvents", sceneResumeEvents.c_str(),
                     "; mod-event names that END a scene (restore transmog)");
        ini.SetLongValue("Physics", "iBouncePercent",
                         static_cast<long>(cbpcBouncePercent),
                         "; 0-100: how strongly an outfit's shown chest piece takes the "
                         "breast bounce read off the worn armour. 100 = Fitting Room's "
                         "profile wins, 0 = the worn armour keeps it");
        ini.SetValue("Compat", "sSamMenuName", samMenuName.c_str(),
                     "; the Screen Archer Menu menu name; the editor hotkey opens "
                     "while this menu is up");
        ini.SetBoolValue("Compat", "bBlockInputWhileOpen", blockInputWhileOpen,
                         "; block all input (game + mods like SAM/Wheeler) while the "
                         "editor is open");
        ini.SetBoolValue("Compat", "bCameraDragWhileOpen", cameraDragWhileOpen,
                         "; left-drag over the world (not the editor panels) rotates "
                         "the camera while the editor is open");
        ini.SetDoubleValue("Compat", "fCameraDragSensitivity",
                           static_cast<double>(cameraDragSensitivity),
                           "; camera drag speed, radians per mouse count");
        ini.SetBoolValue("Dye", "bDyeKeepShine", dyeKeepShine,
                         "; keep the reflection when you dye metal, and colour it too. On, "
                         "a dyed piece is recoloured by rewriting its texture on the GPU "
                         "and its reflection takes the dye as well, so it stays as shiny "
                         "as the mesh made it and the shine is the colour you picked. Off, "
                         "dyeing swaps the material instead, which is cheaper but leaves "
                         "reflective armour matte. Turn it off if dyed armour looks wrong "
                         "or you are short of video memory; iDyeTextureBudgetMiB caps what "
                         "it holds");
        ini.SetBoolValue("Dye", "bDyePbrOnPbrPath", dyePbrOnPbrPath,
                         "; dye True PBR armour without flattening it. Community Shaders "
                         "draws some pieces on its own PBR path, and those used to be "
                         "moved off it to be dyed, which took the colour but left the "
                         "piece matte. On, a PBR piece is dyed the same way every other "
                         "piece is, by rewriting its texture, so it keeps its own shading. "
                         "Off, it goes back to the older way. Needs bDyeKeepShine on");
        ini.SetBoolValue("Dye", "bDyePbrPearl", dyePbrPearl,
                         "; let pearlescent and sheen dyes colour the fuzz and coat of a "
                         "True PBR piece, so the second colour rides the light instead of "
                         "vanishing. Only touches pieces Community Shaders draws as PBR, "
                         "and only features the mesh already has. Turn it off if a dyed "
                         "PBR piece looks wrong after a Community Shaders update");
        ini.SetDoubleValue("Dye", "fDyePbrPearlSheenMax",
                           static_cast<double>(dyePbrPearlSheenMax),
                           "; how strong a pearl's sheen may get on a True PBR piece, 0 to "
                           "1. The dye lifts the piece's own sheen toward this at full "
                           "strength and never lowers it. 1 replaces the surface outright "
                           "and reads like an oil slick; 0 keeps every piece exactly as "
                           "its artist made it and the pearl only recolours what is there");
        ini.SetBoolValue("Requip", "bRequipFlourish", requipFlourish,
                         "; play the Seamstone transition when you change outfit "
                         "yourself. A flash covers the swap: the old pieces fade out "
                         "in it and the new ones fade in. Automatic outfit rules never "
                         "play it, only the hotkey and the editor's Apply");
        // ⚠ THE RETIRED KEY GOES, IT DOES NOT LINGER. bRequipSound named a
        // switch that was believed to control audio and never did; leaving it in
        // the file would have a player set it and wonder why nothing changed.
        ini.Delete("Requip", "bRequipSound", true);
        ini.SetBoolValue("Requip", "bRequipAura", requipAura,
                         "; the magic effect that washes over your body during the "
                         "transition. Visual only: it carries no sound of its own. Off "
                         "leaves the garments lighting on their own");
        ini.SetBoolValue("Requip", "bRequipSoundOn", requipSoundOn,
                         "; play a magic sound when you change outfit. It plays OVER "
                         "Skyrim's own equipment sounds rather than replacing them, since "
                         "those belong to the engine's equip handling");
        ini.SetValue("Requip", "sRequipSoundId", requipSoundId.c_str(),
                     "; which sound, as a sound descriptor editor id. Two rules. Keep it "
                     "short, since the transition lasts about half a second. And keep it "
                     "to a one-shot: nothing here stops the sound, so a LOOPING "
                     "descriptor (anything named LP or LPM, and some that are not) plays "
                     "on until something else cuts it off. These are checked and safe: "
                     "UIMagicUnselect about 0.4s, UIMagicSelect about 0.7s, "
                     "MAGMysticismSoulTrapCaptureShader about 1.1s, "
                     "MAGConjurationCharge050 about 1.3s, MAGEnchantedUnsheatheOther "
                     "about 1.9s. An id the game does not know is silent and says so in "
                     "the log");
        ini.SetBoolValue("Requip", "bRequipFlashAllGear", requipFlashAll,
                         "; on, everything worn lights up when you change outfit. Off, only "
                         "the pieces that actually changed do. Either way the transition "
                         "only fires when something really changed");
        ini.SetDoubleValue("Requip", "fRequipSpeed", static_cast<double>(requipSpeed),
                           "; speed multiplier for the transition, 0.25 to 4.0. Higher is "
                           "faster. Values outside that range are clamped on read");
        ini.SetValue("Requip", "sRequipColour", requipColour.c_str(),
                     "; the light the old garments burn into, as R,G,B in 0-255");
        ini.SetValue("Requip", "sRequipArtForm", requipArtForm.c_str(),
                     "; the aura's art objects, as Plugin.esm|0xFORMID, comma separated. "
                     "Several play together. Empty uses FR_RequipArt from "
                     "FittingRoomLore.esp. Worth trying: Skyrim.esm|0x0010F7A2 "
                     "necromancy, Skyrim.esm|0x0007534B a burst from the feet, "
                     "Skyrim.esm|0x000154BD rising soul streams");
        ini.SetValue("Requip", "sRequipSlotArtForm", requipSlotArtForm.c_str(),
                     "; art played once per changed garment, on that garment's own node "
                     "rather than on the actor, comma separated. Empty turns it off, which "
                     "is the default. It plays once PER PIECE, so a full-body swap fires it "
                     "five or six times at once, and anything with a refraction or membrane "
                     "pass bends the scene that many times over. Keep it flat and short");
        ini.SetValue("Requip", "sRequipShaderForm", requipShaderForm.c_str(),
                     "; the aura's effect shader, as Plugin.esm|0xFORMID. Empty uses "
                     "FR_RequipShader from FittingRoomLore.esp. NOTE that a vanilla "
                     "shader can carry its own looping sound, which is where the noise "
                     "on an outfit swap came from. These play nothing of their own: "
                     "Skyrim.esm|0x00103129 violet ghost, Skyrim.esm|0x000D2057 paler, "
                     "Skyrim.esm|0x000C5EF7 darker purple, Skyrim.esm|0x00094162 a "
                     "flesh-spell sheen, Skyrim.esm|0x000ABF08 cooler blue");

        ini.SetBoolValue("Dye", "bDyeReflective", dyeReflective,
                         "; whether the BULK actions colour metal. Clicking a piece's own "
                         "swatch always dyes it whatever this says. This only governs "
                         "paste and apply-a-scheme: on, they colour metal too; off, they "
                         "leave it alone. It mattered most while dyeing metal cost its "
                         "shine, which bDyeKeepShine above now prevents");
        ini.SetBoolValue("Dye", "bDyeSortByHue", dyeSortByHue,
                         "; order the swatches by colour inside each group. Off, the "
                         "historical order, each pack's colours sit in the order its "
                         "author wrote them. On, they walk around the colour wheel from "
                         "red, with the strongest of each colour first and the greys, "
                         "blacks and whites gathered at the end in their own run. The "
                         "groups themselves do not move: your own dyes still lead, the "
                         "rarity ladder keeps its order, and colours you have not earned "
                         "stay behind the ones you can use");
        ini.SetValue("Dye", "sDyeBlend", dyeBlendName.c_str(),
                     "; how a dye is combined with the cloth underneath it, on armour. "
                     "softlight is what the mod has always done and what every install "
                     "had before this setting existed: it tints gently and keeps the "
                     "garment's own shading, but it cannot move a texture's pure blacks "
                     "or pure whites whatever colour you pick. multiply darkens and can "
                     "colour white, screen lightens and can colour black, and overlay is "
                     "the harder-contrast version of softlight. colour repaints the hue "
                     "and keeps the garment's own brightness exactly, which is the "
                     "closest thing here to painting a piece a flat colour while its "
                     "folds and wear still read. luminosity is the opposite and is a "
                     "curiosity rather than a dye: it keeps the garment's colours and "
                     "gives every part of it the same brightness, so the shading goes "
                     "flat. Anything else spelled here is read as softlight. Eyes are "
                     "not affected");
        ini.SetValue("Dye", "sDyePbrBlend", dyePbrBlendName.c_str(),
                     "; the same choice again, for the pieces Community Shaders draws on "
                     "its own PBR path. They get their own setting because they read a "
                     "different texture: a PBR piece ships a flatter, brighter version of "
                     "its own artwork, and multiply is the curve that arrives saturated "
                     "on it where softer ones washed out. If a dyed PBR piece reads too "
                     "dark, try softlight here. Same spellings as sDyeBlend");
        ini.SetBoolValue("Dye", "bDyeUnlockCards", dyeUnlockCards,
                         "; announce a colour on screen the moment you earn it, as a "
                         "card at the top right. Off, colours are still earned and "
                         "still marked with a gold corner in the dye grid, you just "
                         "are not told at the time. The cards never appear while the "
                         "Fitting Room editor is open, because the gold corner is "
                         "already saying the same thing a few inches away");
        ini.SetDoubleValue("Dye", "fDyeCardDwell", static_cast<double>(dyeCardDwell),
                           "; how long an unlock card holds before it fades, in "
                           "seconds. Read between 2 and 20");
        ini.SetLongValue("Dye", "iDyeCardMax", static_cast<long>(dyeCardMax),
                         "; how many unlock cards stack on screen at once. Read "
                         "between 1 and 8. Much above five and the stack reaches the "
                         "compass");
        ini.SetValue("Dye", "sDyeCardSound", dyeCardSound.c_str(),
                     "; sound descriptor editor ID played once when unlock cards "
                     "start arriving. Once per haul, not once per card. Blank for "
                     "none");
        ini.SetLongValue("Dye", "iDyeCardBurst", static_cast<long>(dyeCardBurst),
                         "; how many cards a single haul is allowed before the rest "
                         "collapse into one line counting them. Read between 2 and "
                         "12. Finishing a questline can hand over dozens of colours "
                         "together, and announcing those one at a time would hold the "
                         "corner of the screen for minutes");
        ini.SetBoolValue("Dye", "bSeamstoneCharging", seamstoneCharging,
                         "; fill the Seamstone from your inventory, the way you recharge "
                         "an enchanted weapon. On, the stone carries a charge meter on its "
                         "item card and T Charge opens the usual soul gem list. Off, the "
                         "only way to fill it is the Refill button inside the Fitting Room "
                         "editor, which is awkward if you have set iCostMode to 2, because "
                         "an empty stone will not let you open much. Anniversary Edition "
                         "only: on Skyrim SE the meter cannot be drawn and this does "
                         "nothing. Whether the meter appears also depends on your UI skin, "
                         "since the stone borrows the weapon card layout to get one");
        ini.SetLongValue("Dye", "iDyeTickSeconds", static_cast<long>(dyeTickSeconds),
                         "; how often the unlock rules are checked while you play, in "
                         "seconds, so a colour you just earned arrives while you are "
                         "still standing there. Read between 5 and 600. Set it to 0 to "
                         "turn the check off, which puts colours back to arriving when "
                         "you load a save or open the editor");
        ini.SetBoolValue("Dye", "bEyeAdvancedColour", eyeAdvancedColour,
                         "; offer a SECOND colour on the eye tile, which is experimental "
                         "and off by default. On, a third swatch appears beside the iris "
                         "and the white, and it colours the far half of the eye texture: "
                         "on most eyes that reads as two tones in each iris, and on eye "
                         "meshes built with a separate half per eye it reads as one "
                         "colour per eye. It needs an iris colour to sit beside, and it "
                         "takes the slot the white would use, so the white cannot be "
                         "coloured while it is set. Turning this off keeps any colour "
                         "already stored and simply stops offering the control");
        ini.SetDoubleValue("Dye", "fDyeRampDiffuseLo",
                           static_cast<double>(dyeRampDiffuseLo),
                           "; where a pearlescent dye's colour starts travelling from its "
                           "first stop to its second, measured in the texture's own "
                           "brightness. Below this the cloth is the first colour, above "
                           "fDyeRampDiffuseHi it is the second, and the shift lives in "
                           "between. Skyrim's armour textures are much darker than they "
                           "look, so this window is low on purpose; widen it and the "
                           "second colour disappears");
        ini.SetDoubleValue("Dye", "fDyeRampDiffuseHi",
                           static_cast<double>(dyeRampDiffuseHi),
                           "; the bright end of the window above");
        ini.SetDoubleValue("Dye", "fDyeRampReflectionLo",
                           static_cast<double>(dyeRampReflectionLo),
                           "; how wide the colour shift is on METAL, which is what makes an "
                           "iridescent dye change colour as you walk around a piece. This "
                           "one is a window on the viewing ANGLE rather than on brightness, "
                           "so 0 to 1 covers a full turn around the piece and a wide window "
                           "gives a slow smooth travel. Narrow it and the two colours snap "
                           "from one to the other instead of blending");
        ini.SetDoubleValue("Dye", "fDyeRampReflectionHi",
                           static_cast<double>(dyeRampReflectionHi),
                           "; the far end of the reflection window above");
        ini.SetLongValue("Dye", "iDyeRampReflectionAxis",
                         static_cast<long>(dyeRampReflectionAxis),
                         "; which way the colour travels on metal. 0 = up and down, so a "
                         "piece bands from one colour on top to the other underneath and "
                         "the band moves as you walk. 1 = around, so the colour cycles as "
                         "you circle the piece. 2 = by brightness, which was the first "
                         "attempt and reads as marbling rather than pearl. Which of these "
                         "looks right is a matter of taste, so try them");
        ini.SetDoubleValue("Dye", "fDyeRampSheen", static_cast<double>(dyeRampSheen),
                          "; how much brighter a special dye's second colour is allowed to "
                          "be than its first. At 0 both colours come out equally bright and "
                          "only the hue changes, which is what made these read as a flat "
                          "two-tone rather than as pearl. At 1 a lighter second colour "
                          "shows as a sheen sitting on a paler body, the way real nacre "
                          "does. Turn it down if a bright dye burns out to white");
        ini.SetBoolValue("Dye", "bDyeRampSpectral", dyeRampSpectral,
                         "; whether a special dye sweeps round the colour wheel between its "
                         "two colours instead of mixing them directly. On, a green and a "
                         "purple dye travel through yellow, orange and red on the way, the "
                         "way a real pearl shows several colours at once. Off, they simply "
                         "blend, which is a plain two-tone sheen and was how these looked "
                         "before");
        ini.SetBoolValue("Dye", "bDyeRampAutoWindow", dyeRampAutoWindow,
                         "; scale a special dye's colour window to each garment's own "
                         "brightness instead of one fixed range. On, pearls read on dark "
                         "and pale armour alike; off, the fDyeRampDiffuse/Reflection "
                         "windows below apply as absolute values");
        ini.SetDoubleValue("Dye", "fDyeRampAutoLo", static_cast<double>(dyeRampAutoLo),
                           "; with the auto window on, where the ramp starts, as a "
                           "multiple of the garment's average brightness");
        ini.SetDoubleValue("Dye", "fDyeRampAutoHi", static_cast<double>(dyeRampAutoHi),
                           "; and where it ends, same units");
        ini.SetDoubleValue("Dye", "fDyeEyeMaskLo", static_cast<double>(dyeEyeMaskLo),
                           "; where the iris begins, as a multiple of the eye normal "
                           "map's average alpha. Alpha under this keeps the eye's own "
                           "colour, so the sclera stays white when an eye is dyed");
        ini.SetDoubleValue("Dye", "fDyeEyeMaskHi", static_cast<double>(dyeEyeMaskHi),
                           "; where the dye reaches full strength, same units. Between "
                           "the two the dye fades in, which keeps the iris edge soft");
        ini.SetDoubleValue("Dye", "fDyeEyeMaskBroad", static_cast<double>(dyeEyeMaskBroad),
                           "; how much of an eye texture a mask may cover and still be "
                           "read as the iris. Many eye sets mark the whole eye instead, "
                           "and dyeing that gives a coloured eyeball. Above this the "
                           "iris is worked out from the marked area instead");
        ini.SetDoubleValue("Dye", "fDyeEyeIrisRadius", static_cast<double>(dyeEyeIrisRadius),
                           "; the size of that worked-out iris, as a fraction of the "
                           "marked area's shorter side. Raise it if a dyed iris looks "
                           "too small on your eyes");
        ini.SetDoubleValue("Dye", "fDyeEyeIrisSoft", static_cast<double>(dyeEyeIrisSoft),
                           "; how far the colour fades either side of that edge. Zero is "
                           "a hard ring, which reads as a drawn circle rather than an eye");
        ini.SetBoolValue("Compat", "bCameraFrameTarget", cameraFrameTarget,
                         "; point the camera at the follower being edited instead "
                         "of at the player. Turn off if the shot fights another "
                         "camera mod");
        ini.SetBoolValue("Compat", "bCameraFocusSlot", cameraFocusSlot,
                         "; swing the camera onto the part of the character you are "
                         "editing: the head for a helmet, the feet for boots. Needs "
                         "Menu Studio, and needs its own camera switched on there");
        ini.SetBoolValue("Compat", "bReassertAppearance", reassertAppearance,
                         "; put a look's EYES, BROWS AND FACIAL HAIR back after "
                         "something else replaces them. Off, whatever RaceMenu and "
                         "the game's own head build chose is what your character "
                         "keeps. On since 1.1.7; your colours are the setting below "
                         "and they are kept either way");
        ini.SetBoolValue("Compat", "bKeepColoursAfterRebuild", keepColoursAfterRebuild,
                         "; put the colours Fitting Room painted back after the game "
                         "rebuilds your head over them, which is what an interior "
                         "switch does. Off, your head keeps the game's paint while "
                         "your body keeps the mod's, so the two stop matching and "
                         "the head can go pale. Colours only: this never swaps a "
                         "head part");
        ini.SetBoolValue("Compat", "bGridInventoryHide", gridInventoryHide,
                         "; EXPERIMENT, off unless you are testing it. While the "
                         "editor is open Grid Inventory is asked to stop drawing, "
                         "but it keeps taking the mouse, so its old area swallows "
                         "a click-drag and the camera will not turn there. This "
                         "also asks it to hide properly, which older builds "
                         "answered by CLOSING, which closes the editor with it. "
                         "If the editor vanishes the moment it opens, put this "
                         "back to 0");
        ini.SetBoolValue("Compat", "bLooksRaceMenu", looksRaceMenu,
                         "; let applying a look load its RaceMenu preset and switch "
                         "your race and sex. Off, a look still brings its outfit, "
                         "dyes, body, shape, skin and overlays, and only the "
                         "RaceMenu half stands down. On since 1.1.7");
        ini.SetBoolValue("Compat", "bPreLoadPresetErase", preLoadPresetErase,
                         "; diagnostic. Drop RaceMenu's remembered preset for your "
                         "character as a save starts loading, so the next character "
                         "does not inherit its hair colour. Off restores the "
                         "behaviour every round before r91 measured");
        ini.SetBoolValue("Overlays", "bOverlayShowAllArt", overlayShowAllArt,
                         "; offer every installed overlay texture on every layer. Off, "
                         "the picker shows the art each pack registered for the part of "
                         "the body you are painting, and leaves out the mask copies that "
                         "carry no transparency and draw as a black square");
        ini.SetBoolValue("Overlays", "bOverlayKeepOffset", overlayKeepOffset,
                         "; keep the Position sliders where they are when you pick "
                         "different art for a layer. Off, new art starts where its "
                         "author drew it. Also a checkbox in the Position section on "
                         "the Overlays page");
        ini.SetBoolValue("Targets", "bEditOtherNpcs", editOtherNpcs,
                         "; list everyone loaded around you in the Editing dropdown, not "
                         "only followers. Off, the list is you, your followers and anyone "
                         "you dressed before who is elsewhere");
        ini.SetDoubleValue("Rules", "fHeartbeatSeconds", static_cast<double>(heartbeatSeconds),
                           "; seconds between automatic rule re-checks when nothing else "
                           "(menu close, combat, equip, death) already triggered one");
        ini.SetDoubleValue("Rules", "fMinDwellSeconds", static_cast<double>(minDwellSeconds),
                           "; minimum seconds an auto-switched outfit stays on before a new "
                           "match may replace it (skipped when combat starts)");
        ini.SetDoubleValue("Rules", "fCastHoldSeconds", static_cast<double>(castHoldSeconds),
                           "; how long the \"when casting\" rule condition stays true after "
                           "you stop casting. A spell you hold down, like Flames, keeps the "
                           "condition true for as long as you hold it, so this is only the "
                           "tail on the end. Long enough to cover the gap between two casts "
                           "is what you want; anything more just leaves the outfit on. "
                           "Set 0 to switch the condition off, and rules using it stop "
                           "matching");
        const auto parent = BuildChannel::IniPath().parent_path();
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        ini.SaveFile(IniPath().c_str());
        spdlog::info("Settings saved.");
    }

}  // namespace OS
