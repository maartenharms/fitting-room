#include "EditorStyle.h"

#include "BuildChannel.h"
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
        const std::array<std::filesystem::path, 3> kFontCandidates = {
            BuildChannel::DataPath("font.ttf"),
            "Data/SKSE/Plugins/dmenu/fonts/Futura Condensed Regular.ttf",
            "Data/SKSE/Plugins/dmenu/fonts/English/SovngardeLight.ttf",
        };
    }

    namespace {
        // Merge Font Awesome 5 Solid glyphs into the font just added (g_body),
        // so slot rows and buttons can print icons inline with text. Only the
        // handful of glyphs in Icons::kAll are baked, keeping the atlas small.
        void MergeIcons(float a_size) {
            const std::array<std::filesystem::path, 2> icons = {
                BuildChannel::DataPath("icons.ttf"),
                "Data/DIP/qtawesome/fonts/fontawesome5-solid-webfont-5.15.4.ttf",
            };
            std::filesystem::path path;
            for (const auto& candidate : icons) {
                std::error_code ec;
                if (std::filesystem::exists(candidate, ec)) {
                    path = candidate;
                    break;
                }
            }
            if (path.empty()) {
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
            const auto pathString = path.string();
            ImGui::GetIO().Fonts->AddFontFromFileTTF(pathString.c_str(), a_size, &cfg,
                                                     ranges.Data);
            spdlog::info("EditorStyle: icon font merged from '{}'.", pathString);
        }
    }

    void InitFonts(float a_bodySize) {
        auto& io = ImGui::GetIO();
        for (const auto& path : kFontCandidates) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                continue;
            }
            const auto pathString = path.string();
            g_body = io.Fonts->AddFontFromFileTTF(pathString.c_str(), a_bodySize);
            MergeIcons(a_bodySize);  // into g_body (the last-added font)
            g_title = io.Fonts->AddFontFromFileTTF(pathString.c_str(), a_bodySize * 1.5f);
            if (g_body) {
                io.FontDefault = g_body;
                spdlog::info("EditorStyle: menu font '{}' at {:.0f}px.", pathString,
                             a_bodySize);
                return;
            }
        }
        // No TTF found: scale the built-in font to a comparable size.
        io.FontGlobalScale = a_bodySize / 13.0f;
        spdlog::warn("EditorStyle: no menu font found; using scaled ImGui default.");
    }

    // ⚠⚠ NOTHING BELOW REACHES THE EDITOR, AND HAS NOT SINCE ddb5c4a. This
    // styles ImGui::GetStyle(), which is the context ImGuiOverlay creates in
    // EnsureInit. EnsureInit is reached only from ImGuiOverlay::Toggle, and
    // the only callers of that are inside ImGuiOverlay.cpp itself; the live
    // editor toggle is EditorWindow::Toggle, a different class, since
    // ddb5c4a hosted the editor as a FUCK IWindow. So the context is never
    // created and this function is never called.
    //
    // ⚠⚠ MEASURED, 2026-08-12, off the search-row probe at fUiScale 0.7 on
    // the live rig, which says the same thing from the other end:
    //
    //     framePad=(8.0,4.0)      this file asks for (10, 6)
    //     itemSpacing=(13.3,5.3)  this file asks for (12, 9)
    //
    // The live numbers are FLICK's, resolution-scaled by the 1.333 recorded
    // at ChamferPanel::FrameWidgetHeight. The editor's palette survives
    // because a FLICK theme INI paints it; a FLICK theme is colours only,
    // which is exactly why the SHAPE half of this block is the half that went
    // missing.
    //
    // ⚠⚠ SO EDITING THE NUMBERS HERE CHANGES NOTHING ON SCREEN. That is the
    // trap this comment exists for: the deadspace and the 0.7-scale
    // complaints both live in metrics this file appears to own and does not.
    // A framed widget measures 46.7px around 28px of text, which is 18.7px of
    // vertical padding against the 12 asked for above, and FLICK reaches that
    // by its own fontSize + 2 * (8 * scale) without reading FramePadding at
    // all. Pushing these through FUCK::PushStyleVar would move whatever DOES
    // read them and leave the widget heights where they are, so the rework is
    // a decision about which controls we draw ourselves, not a number here.
    //
    // Left standing rather than deleted: it is the only written record of the
    // palette and metrics the editor was designed to, and it is what the
    // overlay would need if it were ever revived.
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
        // ⚠⚠ THE SELECTED TAB NEEDS MORE THAN ITS FILL, and this is where the
        // reasoning lives even though the fix is not here. Tab is fully
        // transparent, TabHovered is goldDim and TabActive is goldMid: the only
        // thing separating the tab you are ON from the tab you are merely
        // POINTING AT is 0.17 of alpha in the same gold, so hovering along a
        // strip washes the answer out (user 2026-08-27, "quite hard to tell").
        //
        // ⛔ SETTING ImGuiCol_TabSelectedOverline HERE DOES NOTHING AND WAS
        // TRIED. This function feeds ImGui::GetStyle() in OUR context, which
        // only the retired overlay path ever used; the live editor reads FLICK's
        // style through OS::ui::StyleColor. The bar drew in stock ImGui blue
        // because FLICK's preset never restyled that entry and nothing here
        // could reach it. ChamferPanel::PaintTabBox paints the bar from the
        // tab's own fill instead, and that is the only place to change it.
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
            am->GetSoundHandleByName(handle, a_editorID, 0x10);
            if (handle.IsValid()) {
                handle.Play();
                return;
            }
            // ⚠ AN UNKNOWN EDITOR ID IS SILENT, WHICH IS WHY IT IS LOGGED. The
            // three sounds this file shipped with are all verified vanilla, so
            // nothing ever failed here and the failure mode was never visible.
            // A configurable one can be misspelled or name a descriptor this
            // load order does not have, and "I hear nothing" cannot be told
            // from "the call never ran" without this line.
            //
            // Once per id: this is called from draw code and a wrong id would
            // otherwise write a line per click forever.
            static std::set<std::string> s_reported;
            if (s_reported.insert(a_editorID).second) {
                spdlog::warn("sound: no descriptor named '{}' in this load order, so "
                             "that cue is silent.",
                             a_editorID);
            }
        } catch (...) {
        }
    }

}  // namespace OS::EditorStyle
