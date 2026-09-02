#pragma once

#include <cstdint>
#include <functional>

namespace RE {
    class Actor;
}

// The requip transition's driver (OS-206). The curves and the phase machine are
// in RequipFlourish.h, the scenegraph write is in OutfitDye.cpp, and this is the
// clock, the trigger and the teardown between them.
namespace OS::Requip {

    // Start a flourish on a_actor over the armour bits in a_slotMask, then run
    // a_commit at the cut. a_commit is what actually changes the outfit and
    // posts the refresh, so it runs a fraction of a second AFTER this returns.
    //
    // ⚠ THE CALLER MUST NOT COMMIT THE SWAP ITSELF. Handing the mutation in is
    // what puts it at the peak of the burn rather than at the start of it. A
    // caller that mutates and then calls this gets a flourish over a garment
    // that has already changed, which is the effect playing on the wrong outfit.
    //
    // ⚠ a_commit ALWAYS RUNS, on every path. When the feature is off, when the
    // mask is empty, when nothing could be armed, and when a later teardown
    // abandons the sequence, the swap still has to happen. A flourish that can
    // swallow the player's outfit change is worse than no flourish.
    //
    // std::function rather than a raw pointer because the editor's Apply has to
    // capture an ActorHandle to refresh a follower, and a follower is half the
    // feature.
    void Begin(RE::Actor* a_actor, std::uint32_t a_slotMask, std::function<void()> a_commit);

    // One frame. Called from the Present hook. Cheap when nothing is armed.
    void Tick();

    // Resolve the aura's two records once. kDataLoaded.
    //
    // ⚠ FAILURE IS NOT AN ERROR HERE. The garment animation is the feature and
    // the aura only covers the cut, so a missing record logs what is missing
    // and the flourish runs quieter. Never gates the swap.
    void ResolveForms();

    // A save is live and the driver may run. kPostLoadGame and kNewGame.
    void Start();

    // ⚠ TEARS DOWN WHATEVER IS RUNNING FIRST. kPreLoadGame with a flourish up
    // means records pointing at geometry that is about to be replaced by a
    // different character's, and a restore that arrives after that has nothing
    // it can honestly write to.
    void Stop();

}  // namespace OS::Requip
