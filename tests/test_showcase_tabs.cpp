#include "imgui.h"
#include "OutfitTabs.h"
#include "ShowcaseTabs.h"

#include <cstdio>

namespace {
    OS::ShowcaseTabs::State g_tabs;
    ImVec2                  g_curatedMin{};
    ImVec2                  g_curatedMax{};
    ImVec2                  g_exportedMin{};
    ImVec2                  g_exportedMax{};

    void DrawTabs() {
        ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400.0f, 200.0f), ImGuiCond_Always);
        ImGui::Begin("repro");
        if (ImGui::BeginTabBar("showcase_sources")) {
            const auto sourceTab = [](const char* a_label, int a_source) {
                const auto flags = OS::ShowcaseTabs::ShouldForce(g_tabs, a_source)
                                       ? ImGuiTabItemFlags_SetSelected
                                       : ImGuiTabItemFlags_None;
                const bool active = ImGui::BeginTabItem(a_label, nullptr, flags);
                if (a_source == 0) {
                    g_curatedMin = ImGui::GetItemRectMin();
                    g_curatedMax = ImGui::GetItemRectMax();
                } else if (a_source == OS::ShowcaseTabs::kExported) {
                    g_exportedMin = ImGui::GetItemRectMin();
                    g_exportedMax = ImGui::GetItemRectMax();
                }
                static_cast<void>(
                    OS::ShowcaseTabs::ObserveActive(g_tabs, a_source, active));
                if (active) {
                    ImGui::EndTabItem();
                }
            };
            sourceTab("Discovered", OS::ShowcaseTabs::kDiscovered);
            sourceTab("Curated", OS::ShowcaseTabs::kCurated);
            sourceTab("Exported", OS::ShowcaseTabs::kExported);
            ImGui::EndTabBar();
            OS::ShowcaseTabs::FinishTabBar(g_tabs);
        }
        ImGui::End();
    }

    void Frame(ImVec2 a_mouse, bool a_down) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(a_mouse.x, a_mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, a_down);
        ImGui::NewFrame();
        DrawTabs();
        ImGui::Render();
    }
}

int main() {
    ImGui::CreateContext();
    auto& io       = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.DeltaTime   = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    unsigned char* pixels = nullptr;
    int            width  = 0;
    int            height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    Frame(ImVec2(-1.0f, -1.0f), false);
    Frame(ImVec2(-1.0f, -1.0f), false);

    const ImVec2 curatedCenter{
        (g_curatedMin.x + g_curatedMax.x) * 0.5f,
        (g_curatedMin.y + g_curatedMax.y) * 0.5f
    };
    Frame(curatedCenter, false);
    Frame(curatedCenter, true);
    Frame(curatedCenter, false);
    Frame(curatedCenter, false);
    const int afterClick = g_tabs.source;
    Frame(curatedCenter, false);
    const int afterBounce = g_tabs.source;

    std::printf("after_click=%d after_next_frame=%d\n", afterClick, afterBounce);
    const ImVec2 exportedCenter{
        (g_exportedMin.x + g_exportedMax.x) * 0.5f,
        (g_exportedMin.y + g_exportedMax.y) * 0.5f
    };
    Frame(exportedCenter, false);
    Frame(exportedCenter, true);
    Frame(exportedCenter, false);
    Frame(exportedCenter, false);
    const int afterExportedClick = g_tabs.source;
    std::printf("after_exported_click=%d\n", afterExportedClick);

    OS::ShowcaseTabs::Request(g_tabs, OS::ShowcaseTabs::kDiscovered);
    Frame(curatedCenter, false);
    Frame(curatedCenter, false);
    const int afterControllerSwitch = g_tabs.source;
    std::printf("after_programmatic_switch=%d\n", afterControllerSwitch);
    const bool threeSourceCycle =
        OS::ShowcaseTabs::Cycle(OS::ShowcaseTabs::kDiscovered, true) ==
            OS::ShowcaseTabs::kExported &&
        OS::ShowcaseTabs::Cycle(OS::ShowcaseTabs::kExported, true) ==
            OS::ShowcaseTabs::kCurated &&
        OS::ShowcaseTabs::Cycle(OS::ShowcaseTabs::kCurated, false) ==
            OS::ShowcaseTabs::kExported;

    // ⚠ THE "+" LAYOUT TEST RETIRED WITH OutfitTabAdd.h ON 2026-08-12, and
    // it is worth saying what replaced it rather than leaving a gap. That
    // header existed to anchor the add affordance to a tab height MEASURED off
    // a FLICK tab item, because a FUCK::Button was taller than one and would
    // have grown the row. The strip is hand-rolled now, so there is no
    // foreign height to track: OutfitTabStrip::Layout gives the "+" its box
    // out of the same array as every other tab, and OutfitTabStripTests
    // covers it there.

    const bool equippedForceIsOneShot =
        OS::OutfitTabs::ShouldForceEquipped(
            /*libraryInactive*/ true, OS::OutfitTabs::kNoForcedSelection) == false &&
        OS::OutfitTabs::ShouldForceEquipped(
            /*libraryInactive*/ true, OS::OutfitTabs::kForceEquippedGear) == true;
    std::printf("equipped_force_is_one_shot=%s\n",
                equippedForceIsOneShot ? "true" : "false");

    ImGui::DestroyContext();
    return afterClick == OS::ShowcaseTabs::kCurated &&
                   afterBounce == OS::ShowcaseTabs::kCurated &&
                   afterExportedClick == OS::ShowcaseTabs::kExported &&
                   afterControllerSwitch == OS::ShowcaseTabs::kDiscovered &&
                   threeSourceCycle && equippedForceIsOneShot
               ? 0
               : 1;
}
