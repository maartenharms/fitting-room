#pragma once

struct ImFont;

namespace OS::EditorStyle {

    // Load the menu fonts. MUST run after ImGui::CreateContext and before the
    // first NewFrame (the DX11 backend bakes the atlas on first use). Prefers,
    // in order: our own font.ttf, dMenu's Futura Condensed (the Skyrim look -
    // present on Nolvus), dMenu's SovngardeLight, then ImGui's default scaled.
    void InitFonts(float a_bodySize);

    // Skyrim-menu look: square corners, near-black translucent panels, warm
    // parchment text, vanilla-gold accents. Benchmarked against Photo Mode.
    void Apply();

    [[nodiscard]] ImFont* Body();
    [[nodiscard]] ImFont* Title();

    // "UIMenuOK" / "UIMenuCancel" / "UIMenuFocus" - vanilla UI sounds.
    void PlayUISound(const char* a_editorID) noexcept;

}  // namespace OS::EditorStyle
