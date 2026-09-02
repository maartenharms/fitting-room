#pragma once

#include <string>
#include <vector>

// The Rules tab (Task 13): the only authoring surface for the outfit rules
// engine. Before this existed, a rule could only be created by hand-editing
// rules.json. Draws player-only content; the caller (EditorUI::Draw) must
// not invoke this while the editor's current target is an NPC/follower.

namespace OS::RulesUI {

    // Draws the complete Rules tab body (pin banner, status strip, rule
    // list, "Load shared rules"). Runs on the render thread
    // (ImGuiOverlay::PresentThunk), same as EditorUI::Draw - every read of
    // rule state goes through RuleStore::Merged()/WorldWatch::GetPublished()
    // (both lock-and-copy), and every mutation goes through
    // RuleStore::WithRules/SetPackRuleEnabled followed by
    // WorldWatch::RequestEvaluation(). Never holds a pointer or reference
    // into store state across a frame.
    //
    // a_playerOutfitNames: the player's outfit library names, for the base
    // picker. Taken as a parameter (review finding 6) rather than calling
    // OutfitSession::GetSingleton().SnapshotLibrary() again in here -
    // EditorUI::Draw already holds an equivalent snapshot for the same
    // frame, and passing the names in also puts the player-only contract
    // into this signature instead of leaving it as prose plus a defensive
    // backstop at the call site alone.
    void Draw(const std::vector<std::string>& a_playerOutfitNames);

}  // namespace OS::RulesUI
