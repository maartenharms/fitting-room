#pragma once

#include <string>
#include <vector>

// The editor's empty-pane hint: a block of text centred in both axes.
//
// ⚠ DECLARED HERE AND DEFINED IN EditorUI.cpp, which is the arrangement
// PreviewCardUI.h already uses and for the same reason. The body wraps against
// this editor's own font metrics and paragraph spacing, so moving it would drag
// half the presentation layer with it; a declaration costs nothing and keeps
// one definition in one place.
//
// ⚠ IT IS NOT TextDisabledWrapped WITH EXTRA STEPS. ImGui wraps inside a single
// item, so a wrapped string is a block of LEFT-aligned lines whose box already
// fills the width, and centring that box moves it by nothing. This breaks the
// text first and centres each line, which is what makes it read as centred at
// every window width.
namespace OS::EditorNotice {

    void DrawCentred(const std::vector<std::string>& a_paragraphs);

}  // namespace OS::EditorNotice
