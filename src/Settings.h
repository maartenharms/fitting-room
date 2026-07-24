#pragma once
#include "PCH.h"

#include <string>

namespace OS {

    // Runtime settings, INI-persisted (Data/SKSE/Plugins/FittingRoom.ini).
    struct Settings {
        static constexpr float kUiScaleMin     = 0.4f;
        static constexpr float kUiScaleMax     = 1.2f;
        static constexpr float kUiScaleDefault = 0.8f;

        static Settings& GetSingleton();

        void Load();  // kDataLoaded
        void Save();  // on change from the settings UI

        bool enabled{ true };    // [General] bEnabled
        bool sceneKick{ true };  // [Advanced] bSceneKick - paused-menu render kick
        // Gold cost and the Seamstone requirement, split out of the old single
        // bLoreMode toggle (which is migrated into both on load). Independent:
        // gold needs no lore ESP; the Seamstone requirement only bites when the
        // ESP actually provides the stone.
        bool useGold{ true };          // [General] bUseGold - charge gold on Apply
        bool requireSeamstone{ false }; // [General] bRequireSeamstone - default off: the hotkey opens the editor, the stone is an optional extra way in
        bool collectionOnly{ true };  // [General] bCollectionOnly - browser shows owned looks
        bool dumpBiped{ false };  // [Debug] bDumpBiped - biped dump logging
        // [Debug] sDiagnosePlugin - when non-empty, log the full pipeline fate
        // (catalog entry/drop, fit reason, name-stem, cluster membership) of
        // every ARMO whose display name OR source plugin contains this
        // substring. Answers "why is mod X producing no Discovered set?" from
        // one OutfitSlots.log. Blank (default) = off; e.g. sDiagnosePlugin = Abyss.
        std::string diagnosePlugin{};

        std::uint32_t editorKeyDIK{ 0x15 };      // [Input] iEditorKeyDIK - 0x15 = Y (default)
        std::uint32_t editorGamepadButton{ 0 };  // [Input] iEditorGamepadButton
        std::uint32_t nextOutfitKeyDIK{ 0 };     // [Input] iNextOutfitKeyDIK

        std::uint32_t goldPerSlot{ 100 };    // [Lore] iGoldPerSlot
        std::uint32_t slotBlocklist{ 0 };    // [Advanced] uSlotBlocklist
        float         menuFontSize{ 26.0f }; // [UI] fFontSize (px)
        float         uiScale{ kUiScaleDefault };  // [UI] fUiScale - complete editor scale
        bool          hoverPreview{ true };   // [UI] bHoverPreview - preview styles on hover
        bool          advancedSlots{ false }; // [UI] bAdvancedSlots - editor shows ALL slots
        bool          lockLayout{ true };     // [UI] bLockLayout - lock window pos/size/chrome (gear unlocks)

        // [Scene] scene-framework coexistence (OStim etc.): while a scene runs,
        // suspend the transmog override (player shows real/undressed gear) and
        // block the editor. Scenes are recognized by Papyrus SendModEvent NAMES
        // (comma-separated, case-insensitive); defaults cover OStim NG plus a
        // stable OutfitSlots-specific pair any mod can fire to integrate.
        bool        sceneCompat{ true };  // [Scene] bSceneCompat
        std::string sceneSuspendEvents{ "ostim_start,OutfitSlots_SuspendTransmog" };  // [Scene] sSuspendEvents
        std::string sceneResumeEvents{ "ostim_end,OutfitSlots_ResumeTransmog" };      // [Scene] sResumeEvents

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
    };

}  // namespace OS
