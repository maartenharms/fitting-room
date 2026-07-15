#pragma once

// Bridges for the handful of ImGui APIs that FUCK does not wrap 1:1, so the
// editor draw code can port ImGui:: -> FUCK:: mechanically and route the deltas
// through here. FUCK owns the ImGui context; we reach it only via FUCK::.
// (Drawlist text/rect helpers for the slot-row glyphs land with the icon port.)

#include <SimpleIni.h>  // CSimpleIniA, referenced by FUCK_API.h
#include "FUCK_API.h"
#include <imgui.h>  // ImVec2/ImVec4, ImGuiCol_/ImGuiStyleVar_, ColorConvertFloat4ToU32

#include <cstdarg>  // va_list - FUCK::SetTooltip is not variadic
#include <cstdio>   // vsnprintf

namespace OS::ui {

    // ImGui::GetFontSize() == the current font's pixel size == text line height.
    inline float FontSize() { return FUCK::GetTextLineHeight(); }

    // ImGui::GetStyle().<member> replacements (FUCK exposes style vars, not the struct).
    inline ImVec2 FramePadding() { return FUCK::GetStyleVarVec(ImGuiStyleVar_FramePadding); }
    inline ImVec2 ItemSpacing() { return FUCK::GetStyleVarVec(ImGuiStyleVar_ItemSpacing); }
    inline ImVec2 WindowPadding() { return FUCK::GetStyleVarVec(ImGuiStyleVar_WindowPadding); }
    inline float  FrameRounding() { return FUCK::GetStyleVar(ImGuiStyleVar_FrameRounding); }

    // ImGui::GetColorU32(...) - FUCK returns ImVec4; pack to U32 for draw calls.
    inline ImU32 Col(ImGuiCol idx) { return ImGui::ColorConvertFloat4ToU32(FUCK::GetStyleColorVec4(idx)); }
    inline ImU32 Col(const ImVec4& v) { return ImGui::ColorConvertFloat4ToU32(v); }

    // ImGui::SetTooltip is printf-style; FUCK::SetTooltip takes only a plain
    // string, so pre-format here.
    inline void SetTooltipF(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        FUCK::SetTooltip(buf);
    }

    // ImDrawList::AddRectFilled replacement. FUCK::DrawRectFilled uses the WINDOW
    // drawlist (same as ImGui's), so hover boxes sit behind following text just
    // as before. (DrawScreenRectFilled is a foreground/on-top overlay - not this.)
    inline void RectFilled(const ImVec2& a, const ImVec2& b, ImU32 col, float rounding = 0.0f) {
        FUCK::DrawRectFilled(a, b, ImGui::ColorConvertU32ToFloat4(col), rounding);
    }

    // ImDrawList::AddText replacement. FUCK has no positioned-text primitive, so
    // move the layout cursor, emit colored text, and restore the cursor (Text*
    // registers a real item + advances the cursor; AddText did not). Respects
    // text_end so a search-match substring can be drawn in its own colour.
    // restore=false leaves the cursor at the drawn text (the last op is an item,
    // so no dangling SetCursorPos → no ImGui "extend boundaries" warning) - use it
    // where the next op resets the cursor anyway (e.g. a table column). restore=true
    // is for inline use where a following SameLine must align to the original line
    // (a subsequent real item then clears the SetCursorPos flag before window end).
    inline void TextAt(const ImVec2& pos, ImU32 col, const char* text, const char* text_end = nullptr,
                       bool restore = true) {
        [[maybe_unused]] const ImVec2 saved = FUCK::GetCursorScreenPos();
        FUCK::SetCursorScreenPos(pos);
        FUCK::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
        FUCK::TextUnformatted(text, text_end);
        FUCK::PopStyleColor();
        if (restore) {
            FUCK::SetCursorScreenPos(saved);
        }
    }

}  // namespace OS::ui
