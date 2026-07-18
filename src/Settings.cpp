#include "Settings.h"

#define SI_NO_CONVERSION 1
#include <SimpleIni.h>

#include <cstdlib>

namespace OS {

    namespace {
        const char* IniPath() { return "Data/SKSE/Plugins/FittingRoom.ini"; }
    }

    Settings& Settings::GetSingleton() {
        static Settings instance;
        return instance;
    }

    void Settings::Load() {
        CSimpleIniA ini;
        ini.SetUnicode();
        if (ini.LoadFile(IniPath()) < 0) {
            spdlog::info("Settings: no INI yet, writing defaults.");
            Save();
            return;
        }
        enabled        = ini.GetBoolValue("General", "bEnabled", enabled);
        // Migration: the legacy bLoreMode bundled gold + the Seamstone
        // requirement. bUseGold still inherits it (gold on by default, ESO-style);
        // bRequireSeamstone now defaults OFF so the default Y hotkey opens the
        // editor out of the box, with the Seamstone as an optional extra way in.
        const bool legacyLore = ini.GetBoolValue("General", "bLoreMode", true);
        useGold          = ini.GetBoolValue("General", "bUseGold", legacyLore);
        requireSeamstone = ini.GetBoolValue("General", "bRequireSeamstone", false);
        collectionOnly = ini.GetBoolValue("General", "bCollectionOnly", collectionOnly);
        sceneKick = ini.GetBoolValue("Advanced", "bSceneKick", sceneKick);
        dumpBiped = ini.GetBoolValue("Debug", "bDumpBiped", dumpBiped);
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

        goldPerSlot = static_cast<std::uint32_t>(
            ini.GetLongValue("Lore", "iGoldPerSlot", static_cast<long>(goldPerSlot)));
        slotBlocklist = static_cast<std::uint32_t>(
            std::strtoul(ini.GetValue("Advanced", "uSlotBlocklist", "0"), nullptr, 0));
        menuFontSize = static_cast<float>(
            ini.GetDoubleValue("UI", "fFontSize", static_cast<double>(menuFontSize)));
        uiScale = static_cast<float>(
            ini.GetDoubleValue("UI", "fUiScale", static_cast<double>(uiScale)));
        hoverPreview    = ini.GetBoolValue("UI", "bHoverPreview", hoverPreview);
        advancedSlots   = ini.GetBoolValue("UI", "bAdvancedSlots", advancedSlots);
        lockLayout      = ini.GetBoolValue("UI", "bLockLayout", lockLayout);

        sceneCompat = ini.GetBoolValue("Scene", "bSceneCompat", sceneCompat);
        // Presence-checked so an absent key keeps the C++ default (assigning a
        // GetValue default pointer that aliases our own buffer is unsafe).
        if (const char* v = ini.GetValue("Scene", "sSuspendEvents", nullptr); v && *v) {
            sceneSuspendEvents = v;
        }
        if (const char* v = ini.GetValue("Scene", "sResumeEvents", nullptr); v && *v) {
            sceneResumeEvents = v;
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

        spdlog::info("Settings loaded (blocklist=0x{:X}, editor key=0x{:X}).",
                     slotBlocklist, editorKeyDIK);
    }

    void Settings::Save() {
        CSimpleIniA ini;
        ini.SetUnicode();
        // Preserve sections Save() does not own (notably the hand-written
        // [Outfit] stopgap) instead of rewriting the file from scratch.
        ini.LoadFile(IniPath());
        ini.SetBoolValue("General", "bEnabled", enabled);
        ini.SetBoolValue("General", "bUseGold", useGold,
                         "; charge gold per changed slot when you Apply an outfit");
        ini.SetBoolValue("General", "bRequireSeamstone", requireSeamstone,
                         "; require carrying the Seamstone to open the editor (needs the lore ESP)");
        ini.SetBoolValue("General", "bCollectionOnly", collectionOnly,
                         "; style browser default: only looks you have owned");
        ini.SetBoolValue("Advanced", "bSceneKick", sceneKick);
        ini.SetBoolValue("Debug", "bDumpBiped", dumpBiped);
        ini.SetValue("Debug", "sDiagnosePlugin", diagnosePlugin.c_str(),
                     "; log the catalog/detection fate of ARMOs whose name or "
                     "plugin contains this substring (blank = off); e.g. Abyss");
        ini.SetLongValue("Input", "iEditorKeyDIK", static_cast<long>(editorKeyDIK),
                         "; 0 = unbound");
        ini.SetLongValue("Input", "iEditorGamepadButton", static_cast<long>(editorGamepadButton),
                         "; 0 = unbound");
        ini.SetLongValue("Input", "iNextOutfitKeyDIK", static_cast<long>(nextOutfitKeyDIK),
                         "; 0 = unbound");
        ini.SetLongValue("Lore", "iGoldPerSlot", static_cast<long>(goldPerSlot),
                         "; gold charged per styled slot when bLoreMode is on");
        ini.SetLongValue("Advanced", "uSlotBlocklist", static_cast<long>(slotBlocklist),
                         "; biped-slot bitmask the mod must never touch (hex ok)", true /*a_bUseHex*/);
        ini.SetDoubleValue("UI", "fFontSize", static_cast<double>(menuFontSize),
                           "; editor menu font size in pixels");
        ini.SetDoubleValue("UI", "fUiScale", static_cast<double>(uiScale),
                           "; live editor UI scale (0.8-1.6); the in-editor slider sets this");
        ini.SetBoolValue("UI", "bHoverPreview", hoverPreview,
                         "; preview a style on the character just by hovering its row");
        ini.SetBoolValue("UI", "bAdvancedSlots", advancedSlots,
                         "; editor default: show every biped slot (else the common set)");
        ini.SetBoolValue("UI", "bLockLayout", lockLayout,
                         "; lock the editor window position/size (uncheck the gear's Lock window to move/resize)");
        ini.SetBoolValue("Scene", "bSceneCompat", sceneCompat,
                         "; suspend transmog + block the editor while a scene mod (OStim) runs");
        ini.SetValue("Scene", "sSuspendEvents", sceneSuspendEvents.c_str(),
                     "; Papyrus mod-event names (comma list) that START a scene");
        ini.SetValue("Scene", "sResumeEvents", sceneResumeEvents.c_str(),
                     "; mod-event names that END a scene (restore transmog)");
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
        std::error_code ec;
        std::filesystem::create_directories("Data/SKSE/Plugins", ec);
        ini.SaveFile(IniPath());
        spdlog::info("Settings saved.");
    }

}  // namespace OS
