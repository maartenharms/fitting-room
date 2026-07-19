#include "PCH.h"

#include "SettingsUI.h"

#include "EditorGate.h"
#include "InputListener.h"
#include "LoreModule.h"
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
        const ImVec4 kWarn{ 0.95f, 0.75f, 0.25f, 1.0f };

        // The same two setups the installer offers, one click each - so the
        // lore levers are reachable as a single decision instead of three
        // scattered switches. Settings only: the buttons assign exactly what
        // the Lore section below shows, and the note says so out loud. The
        // ESP itself is the one thing only the installer can add.
        FUCK::SeparatorText("$FR_Set_Playstyle"_T);
        if (FUCK::Button("$FR_Set_PlaystyleLore"_T)) {
            cfg.useGold          = true;
            cfg.requireSeamstone = true;
            dirty                = true;
        }
        Tip("$FR_Set_PlaystyleLoreTip"_T);
        FUCK::SameLine(0.0f, 8.0f);
        if (FUCK::Button("$FR_Set_PlaystyleFree"_T)) {
            cfg.useGold          = false;
            cfg.requireSeamstone = false;
            dirty                = true;
        }
        Tip("$FR_Set_PlaystyleFreeTip"_T);
        FUCK::TextDisabled("%s", "$FR_Set_PlaystyleNote"_T);

        FUCK::SeparatorText("$FR_Set_Lore"_T);
        dirty |= FUCK::Checkbox("$FR_Set_ChargeGold"_T, &cfg.useGold);
        Tip("$FR_Set_ChargeGoldTip"_T);
        if (cfg.useGold) {
            int gold = static_cast<int>(cfg.goldPerSlot);
            if (FUCK::SliderInt("$FR_Set_GoldPerSlot"_T, &gold, 0, 500)) {
                cfg.goldPerSlot = static_cast<std::uint32_t>(gold < 0 ? 0 : gold);
                dirty           = true;
            }
        }
        dirty |= FUCK::Checkbox("$FR_Set_RequireSeamstone"_T,
                                &cfg.requireSeamstone);
        Tip("$FR_Set_RequireSeamstoneTip"_T);
        // The requirement is AND-ed with the ESP being loaded, so without it
        // this switch silently does nothing - say so rather than let someone
        // wonder why their hotkey still opens the editor.
        if (cfg.requireSeamstone && !OS::LoreModule::Available()) {
            FUCK::TextColored(kWarn, "%s", "$FR_Set_SeamstoneMissing"_T);
        }

        FUCK::SeparatorText("$FR_Set_Editor"_T);
        dirty |= FUCK::Checkbox("$FR_Set_CollectedOnly"_T, &cfg.collectionOnly);
        dirty |= FUCK::Checkbox("$FR_Set_HoverPreview"_T, &cfg.hoverPreview);
        dirty |= FUCK::Checkbox("$FR_Set_EverySlot"_T, &cfg.advancedSlots);
        if (FUCK::SliderFloat("$FR_Set_UiScale"_T, &cfg.uiScale, 0.8f, 1.6f, "%.2f")) {
            dirty = true;
        }

        FUCK::SeparatorText("$FR_Set_Compatibility"_T);
        dirty |= FUCK::Checkbox("$FR_Set_SuspendScenes"_T,
                                &cfg.sceneCompat);

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
