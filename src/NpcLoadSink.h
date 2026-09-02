#pragma once

namespace OS::NpcLoadSink {

    // OS-109. RE-APPLY A FOLLOWER'S LOOK WHEN HER 3D COMES BACK.
    //
    // WHY IT WAS MISSING. Every follower-side write this mod makes lands on LIVE
    // GEOMETRY: NpcHair attaches head parts to the loaded 3D rather than editing
    // the record, the body morph targets the built nodes, and the biped shim
    // runs at attach time. None of that survives the 3D being thrown away and
    // rebuilt, and nothing was watching for the rebuild. The full re-apply
    // already existed as OutfitSession::RequestRefreshActor - it was simply
    // wired to only one trigger, OBody signalling ready, which a cell change
    // never fires.
    //
    // The field report was `coc` reverting a follower to her vanilla look. That
    // is the same defect as fast travel, a load door, or her simply streaming
    // out and back in while you walk away and return; coc is just the fastest
    // way to reproduce it.
    //
    // ⚠ NOT A CELL-CHANGE HOOK, deliberately. TESCellAttachDetachEvent says the
    // CELL moved, which is a different question from "this actor has geometry
    // again", and on a coc the followers arrive slightly after the player's own
    // cell swap - a single sweep fired on the player's change lands before they
    // are in the high process and finds nobody. Object-loaded fires once per
    // actor at the exact moment there is something to re-apply to, whatever
    // caused it.
    void Register();  // event sink; call at kDataLoaded alongside the others

}  // namespace OS::NpcLoadSink
