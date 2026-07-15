#include "EditorStyle.h"

#include "Icons.h"

#include <imgui.h>

#include <filesystem>

namespace OS::EditorStyle {

    namespace {
        ImFont* g_body{ nullptr };
        ImFont* g_title{ nullptr };

        // Candidates in preference order. dMenu ships the Futura Condensed the
        // vanilla UI uses - present on Nolvus. Our own font.ttf wins when the
        // mod ships one (release: verify the license before bundling Futura).
        constexpr const char* kFontCandidates[] = {
            "Data/SKSE/Plugins/FittingRoom/font.ttf",
            "Data/SKSE/Plugins/dmenu/fonts/Futura Condensed Regular.ttf",
            "Data/SKSE/Plugins/dmenu/fonts/English/SovngardeLight.ttf",
        };
    }

    namespace {
        // Merge Font Awesome 5 Solid glyphs into the font just added (g_body),
        // so slot rows and buttons can print icons inline with text. Only the
        // handful of glyphs in Icons::kAll are baked, keeping the atlas small.
        void MergeIcons(float a_size) {
            constexpr const char* kIcons[] = {
                "Data/SKSE/Plugins/FittingRoom/icons.ttf",
                "Data/DIP/qtawesome/fonts/fontawesome5-solid-webfont-5.15.4.ttf",
            };
            const char* path = nullptr;
            for (const auto* p : kIcons) {
                std::error_code ec;
                if (std::filesystem::exists(p, ec)) {
                    path = p;
                    break;
                }
            }
            if (!path) {
                spdlog::warn("EditorStyle: no icons.ttf - slot rows fall back to text labels.");
                return;
            }
            static ImVector<ImWchar> ranges;  // must outlive the atlas Build()
            if (ranges.empty()) {
                ImFontGlyphRangesBuilder b;
                for (const std::uint16_t cp : Icons::kAll) {
                    b.AddChar(cp);
                }
                b.BuildRanges(&ranges);
            }
            ImFontConfig cfg;
            cfg.MergeMode        = true;
            cfg.PixelSnapH       = true;
            cfg.GlyphMinAdvanceX = a_size;  // give icons a uniform monospace box
            ImGui::GetIO().Fonts->AddFontFromFileTTF(path, a_size, &cfg, ranges.Data);
            spdlog::info("EditorStyle: icon font merged from '{}'.", path);
        }
    }

    void InitFonts(float a_bodySize) {
        auto& io = ImGui::GetIO();
        for (const auto* path : kFontCandidates) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                continue;
            }
            g_body = io.Fonts->AddFontFromFileTTF(path, a_bodySize);
            MergeIcons(a_bodySize);  // into g_body (the last-added font)
            g_title = io.Fonts->AddFontFromFileTTF(path, a_bodySize * 1.5f);
            if (g_body) {
                io.FontDefault = g_body;
                spdlog::info("EditorStyle: menu font '{}' at {:.0f}px.", path, a_bodySize);
                return;
            }
        }
        // No TTF found: scale the built-in font to a comparable size.
        io.FontGlobalScale = a_bodySize / 13.0f;
        spdlog::warn("EditorStyle: no menu font found; using scaled ImGui default.");
    }

    void Apply() {
        auto& style = ImGui::GetStyle();

        // Vanilla menus are square, flat, and dark.
        style.WindowRounding    = 0.0f;
        style.ChildRounding     = 0.0f;
        style.FrameRounding     = 0.0f;
        style.PopupRounding     = 0.0f;
        style.ScrollbarRounding = 0.0f;
        style.GrabRounding      = 0.0f;
        style.TabRounding       = 0.0f;
        style.WindowBorderSize  = 1.0f;
        style.ChildBorderSize   = 1.0f;
        style.FrameBorderSize   = 0.0f;
        style.PopupBorderSize   = 1.0f;
        style.WindowPadding     = ImVec2(26.0f, 22.0f);
        style.FramePadding      = ImVec2(10.0f, 6.0f);
        style.ItemSpacing       = ImVec2(12.0f, 9.0f);
        style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
        style.ScrollbarSize     = 12.0f;
        style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

        // Skyrim palette: parchment text, near-black panels, journal gold.
        const ImVec4 text{ 0.91f, 0.90f, 0.85f, 1.00f };
        const ImVec4 textDim{ 0.55f, 0.54f, 0.50f, 1.00f };
        const ImVec4 gold{ 0.855f, 0.741f, 0.502f, 1.00f };  // #DABD80
        const ImVec4 goldDim{ 0.855f, 0.741f, 0.502f, 0.28f };
        const ImVec4 goldMid{ 0.855f, 0.741f, 0.502f, 0.45f };
        const ImVec4 panel{ 0.015f, 0.015f, 0.02f, 0.94f };
        const ImVec4 inset{ 1.0f, 1.0f, 1.0f, 0.025f };
        const ImVec4 line{ 1.0f, 1.0f, 1.0f, 0.18f };

        auto* c                        = style.Colors;
        c[ImGuiCol_Text]               = text;
        c[ImGuiCol_TextDisabled]       = textDim;
        c[ImGuiCol_WindowBg]           = panel;
        c[ImGuiCol_ChildBg]            = inset;
        c[ImGuiCol_PopupBg]            = ImVec4(0.02f, 0.02f, 0.03f, 0.98f);
        c[ImGuiCol_Border]             = line;
        c[ImGuiCol_BorderShadow]       = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_FrameBg]            = ImVec4(1.0f, 1.0f, 1.0f, 0.05f);
        c[ImGuiCol_FrameBgHovered]     = goldDim;
        c[ImGuiCol_FrameBgActive]      = goldMid;
        c[ImGuiCol_TitleBg]            = panel;
        c[ImGuiCol_TitleBgActive]      = panel;
        c[ImGuiCol_TitleBgCollapsed]   = panel;
        c[ImGuiCol_ScrollbarBg]        = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]      = ImVec4(1.0f, 1.0f, 1.0f, 0.22f);
        c[ImGuiCol_ScrollbarGrabHovered] = goldMid;
        c[ImGuiCol_ScrollbarGrabActive]  = gold;
        c[ImGuiCol_CheckMark]          = gold;
        c[ImGuiCol_SliderGrab]         = goldMid;
        c[ImGuiCol_SliderGrabActive]   = gold;
        c[ImGuiCol_Button]             = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
        c[ImGuiCol_ButtonHovered]      = goldDim;
        c[ImGuiCol_ButtonActive]       = goldMid;
        c[ImGuiCol_Header]             = goldDim;   // Selectable selected
        c[ImGuiCol_HeaderHovered]      = goldMid;
        c[ImGuiCol_HeaderActive]       = goldMid;
        c[ImGuiCol_Separator]          = line;
        c[ImGuiCol_SeparatorHovered]   = goldMid;
        c[ImGuiCol_SeparatorActive]    = gold;
        c[ImGuiCol_Tab]                = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TabHovered]         = goldDim;
        c[ImGuiCol_TabActive]          = goldMid;
        c[ImGuiCol_TabUnfocused]       = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TabUnfocusedActive] = goldDim;
        c[ImGuiCol_NavHighlight]       = gold;
    }

    ImFont* Body() { return g_body; }
    ImFont* Title() { return g_title; }

    void PlayUISound(const char* a_editorID) noexcept {
        try {
            auto* am = RE::BSAudioManager::GetSingleton();
            if (!am || !a_editorID) {
                return;
            }
            RE::BSSoundHandle handle;
            am->BuildSoundDataFromEditorID(handle, a_editorID, 0x10);
            if (handle.IsValid()) {
                handle.Play();
            }
        } catch (...) {
        }
    }

}  // namespace OS::EditorStyle
