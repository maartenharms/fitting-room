#include "PCH.h"

#include "SettingsUI.h"

#include "EditorGate.h"
#include "InputListener.h"
#include "Settings.h"

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

    // The panel body. Edits the Settings singleton in place; a single Save() at
    // the end persists to OutfitSlots.ini when anything changed. Outfit Slots
    // reads settings at editor-open / apply-time, so edits take effect the next
    // time the editor opens.
    void DrawPanel() {
        auto& cfg   = OS::Settings::GetSingleton();
        bool  dirty = false;

        FUCK::SeparatorText("$FR_Set_Gameplay"_T);
        dirty |= FUCK::Checkbox("$FR_Set_ChargeGold"_T, &cfg.useGold, false, false);
        Tip("$FR_Set_ChargeGoldTip"_T);
        if (cfg.useGold) {
            int gold = static_cast<int>(cfg.goldPerSlot);
            if (FUCK::SliderInt("$FR_Set_GoldPerSlot"_T, &gold, 0, 500)) {
                cfg.goldPerSlot = static_cast<std::uint32_t>(gold < 0 ? 0 : gold);
                dirty           = true;
            }
        }
        dirty |= FUCK::Checkbox("$FR_Set_RequireSeamstone"_T,
                                &cfg.requireSeamstone, false, false);
        Tip("$FR_Set_RequireSeamstoneTip"_T);

        FUCK::SeparatorText("$FR_Set_Editor"_T);
        dirty |= FUCK::Checkbox("$FR_Set_CollectedOnly"_T, &cfg.collectionOnly, false, false);
        dirty |= FUCK::Checkbox("$FR_Set_HoverPreview"_T, &cfg.hoverPreview, false, false);
        dirty |= FUCK::Checkbox("$FR_Set_EverySlot"_T, &cfg.advancedSlots, false, false);
        if (FUCK::SliderFloat("$FR_Set_UiScale"_T, &cfg.uiScale, 0.8f, 1.6f, "%.2f")) {
            dirty = true;
        }

        FUCK::SeparatorText("$FR_Set_Compatibility"_T);
        dirty |= FUCK::Checkbox("$FR_Set_SuspendScenes"_T,
                                &cfg.sceneCompat, false, false);

        FUCK::SeparatorText("$FR_Set_Controls"_T);
        {
            // Editor hotkey as a dropdown. FUCK::Combo is the array form (no
            // BeginCombo), so gather the bindable-key names and map the index.
            constexpr int kCount = static_cast<int>(std::size(OS::EditorGate::kBindableKeys));
            std::vector<const char*> names;
            names.reserve(kCount);
            int current = 0;
            for (int i = 0; i < kCount; ++i) {
                names.push_back(OS::EditorGate::kBindableKeys[i].name);
                if (OS::EditorGate::kBindableKeys[i].dik == cfg.editorKeyDIK) {
                    current = i;
                }
            }
            if (FUCK::Combo("$FR_Set_Hotkey"_T, &current, names.data(), kCount)) {
                const std::uint32_t dik = OS::EditorGate::kBindableKeys[current].dik;
                if (dik != cfg.editorKeyDIK) {
                    cfg.editorKeyDIK = dik;
                    OS::InputListener::GetSingleton().SetEditorKey(dik);
                    dirty = true;
                }
            }
        }
        Tip("$FR_Set_HotkeyTip"_T);

        FUCK::TextDisabled("%s", "$FR_Set_SaveNote"_T);

        if (dirty) {
            cfg.Save();
        }
    }

    // FLICK sidebar entry: the user opens FUCK (hotkey / controller menu) and
    // picks "Outfit Slots".
    class SettingsTool : public FUCK::ITool {
    public:
        const char* Name() const override { return "Fitting Room"; }
        void        Draw() override { DrawPanel(); }
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
