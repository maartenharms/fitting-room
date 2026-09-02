#pragma once

#include <cstdint>

// The engine's own hair painter, detoured so Fitting Room finds out when a head
// build has just overwritten its colour.
//
// ⚠⚠ THIS IS THE HOLE HairColor.h AND RefreshGate.h BOTH DESCRIBE AND NEITHER
// COULD CLOSE. Setting hairColor on the actor base paints nothing;
// BSFaceGenManager::PrepareHeadPartForShaders (SE 26259 / AE 26838) is the only
// engine code that paints hair, it derives the tint from the actor base, and
// "nothing tells Fitting Room it happened". Every fix so far has been a
// re-assert bolted to some event that USUALLY precedes a rebuild - the head
// editor closing, a save loading, a race switch - which works exactly as well
// as the guess that the rebuild will not happen at any other time.
//
// Measured on 2026-08-12, and this is what the guess costs. Dye the hair
// (86,161,191), save, quit, reload:
//
//   t+0.0s   painted (121,159,179)   the snapped form, refresh has not run
//   t+0.5s   painted (86,161,191)    the load-time re-assert lands. Correct.
//   t+2.0s   painted (121,159,179)   a head build, from nowhere FR can see
//   t+30s    painted (121,159,179)   still wrong, 28 seconds later
//   t+33s    painted (86,161,191)    the player opened the editor
//
// The snapped form is the nearest BGSColorForm in the whole load order, not a
// hair palette, so it is a lip or skin colour a mean 10.3/255 per channel away
// from the pick. That is the "my hair colour reset" two reporters describe.
//
// ⚠ THE ID IS CHECKED FOR MEMBERSHIP, NOT RESOLVED AND TRUSTED. CommonLib's
// id2offset does a lower_bound, so an id that is merely ABSENT resolves to its
// neighbour and would detour a completely unrelated function. Install refuses
// on a failed membership test and says so; the re-asserts all still run, so a
// refusal is the old behaviour rather than a broken one.
namespace OS::HeadBuildHook {

    // Install the detour.
    //
    // ⚠⚠ CALLED AT kDataLoaded, NOT FROM SKSEPluginLoad WITH EVERY OTHER HOOK,
    // AND THE DIFFERENCE IS MEASURED. At plugin load Fitting Room was FIRST
    // onto this entry and logged its own E9 sitting there. By the time a save
    // had loaded the entry read FF 25 - the six-byte absolute jump Detours
    // writes - our jump was gone and the detour had been called zero times
    // (field 2026-08-12). This entry is first-come, LAST-served: whoever
    // patches last owns it. kDataLoaded runs once every plugin has loaded, so
    // installing there puts us on top, and SafetyHook relocates the jump it
    // finds into our trampoline so the other mod's hook still runs.
    void Install();

    [[nodiscard]] bool Installed();

    // How many times the detour has seen a HAIR part built, and how many
    // repaints it has actually asked for. The two differ whenever Fitting Room
    // is not driving the colour, which is the ordinary case.
    [[nodiscard]] std::uint32_t HairBuilds();
    [[nodiscard]] std::uint32_t Repaints();

    // Re-arm the per-load census caps at a load boundary. r65's doubling load
    // was BLIND because the session cap (128 bake lines) had run dry a minute
    // earlier on an apply; the caps exist to bound one burst, not to spend the
    // whole session's budget on whichever burst came first.
    void ResetLoadCensus();

    // Run the late-rebake gate right now, on the caller's thread, and say
    // whether it found residue and rebaked.
    //
    // ⚠⚠ THE DETOUR'S OWN CALL IS ONE FRAME LATE AND THAT FRAME IS
    // VISIBLE. The head is mid-construction inside the detour, so the gate
    // cannot run on the engine's stack; it is posted through the task
    // interface and drains after the frame the build belongs to. r84 measured
    // exactly that: five catches, five corrections, and one frame of the
    // previous character's face in front of each. This entry point exists so
    // an edge that is NOT mid-build can run the same gate synchronously.
    //
    // ⚠ THE ONE EDGE TRIED SO FAR IS UPSTREAM OF THE BUILD AND CATCHES
    // NOTHING. r85 put the character editor's open edge behind this and every
    // catch still came from the queued call; see HeadEditorSink for what that
    // measured and why the call stays anyway.
    //
    // The gate is unchanged and cheap to ask: it returns on the first face
    // geometry whose bound tint is not one of Fitting Room's own exports.
    // a_edge names the caller in the log line, which is how a round tells
    // which edge caught the residue.
    bool RebakeForeignPresetTintNow(const char* a_edge);

    // Re-read the patched entry and say whether it is still ours.
    //
    // ⚠⚠ THE ONE QUESTION A SILENT DETOUR CANNOT ANSWER ABOUT ITSELF. On
    // 2026-08-12 the hook installed cleanly, reported the right id and address,
    // and then never logged a single call across a whole minute in which the
    // player's head was demonstrably rebuilt. Two completely different things
    // produce that: the engine never calls the function on this load order, or
    // it does and somebody installed over our entry afterwards. The first
    // means the target is wrong and no amount of hooking it will help; the
    // second means the target is right and we lost a fight. Comparing the
    // entry's bytes against what SafetyHook left there separates them, and
    // naming the module the jump now lands in says who won.
    //
    // Call from the game thread, some seconds after a load, once other plugins
    // have finished installing. Logs and returns.
    void ReportEntryIntegrity();

}  // namespace OS::HeadBuildHook
