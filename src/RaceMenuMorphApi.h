#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace RE {
    class Actor;
}

namespace OS::RaceMenuMorphApi {

    // Body Studio's key: the whole custom body it authors, replaced wholesale
    // on every apply.
    inline constexpr const char* kOwnedKey = "FittingRoom.CustomBody";

    // The Shape page's key, and a SECOND OWNED KEY RATHER THAN A PARAMETER.
    // The header note below still holds: there is no arbitrary-key overload,
    // because a key is an ownership claim and handing one in from a caller is
    // how a mod ends up clearing somebody else's. Two named keys for two named
    // features keeps that property.
    //
    // ⚠ SEPARATE FROM kOwnedKey ON PURPOSE, AND THIS IS WHAT MAKES THE OVERLAY
    // WORK. RaceMenu sums a morph's value across every key that sets it, so a
    // Shape slider adds to whatever the outfit's OBody preset already put on
    // the character instead of replacing it. Sharing one key would make the
    // next body apply wipe the user's shape, and the shape apply wipe the
    // outfit's body.
    inline constexpr const char* kShapeKey = "FittingRoom.Shape";

    // Push-up's key, and a THIRD one for the reason the note above gives for
    // the second. An outfit's lift adds to whatever the body already is: the
    // player's BodySlide build, Body Studio's preset on kOwnedKey and the Shape
    // page's sliders on kShapeKey are all still underneath it.
    //
    // ⚠⚠ SHARING kOwnedKey WOULD HAVE BEEN A BUG WITH BODY STUDIO'S NAME ON
    // IT. That key is replaced wholesale on every apply, so an outfit with lift
    // would have wiped the custom body Body Studio authored, and the next body
    // apply would have wiped the lift. Two named features, two keys, and
    // RaceMenu sums them.
    inline constexpr const char* kPushUpKey = "FittingRoom.PushUp";

    struct MorphValue {
        std::string name;
        float       value{ 0.0f };
    };

    struct ApplyResult {
        bool        available{ false };
        bool        actorLoaded{ false };
        bool        refreshed{ false };
        std::size_t valuesSet{ 0 };
    };

    // Acquire RaceMenu's current public body-morph interface, at kPostPostLoad.
    //
    // ⚠ NO LONGER GATED ON THE BODY STUDIO CHANNEL. It was, and the gate was
    // right while body morphs were only Body Studio's feature. The Shape page
    // ships on every channel and drives the same interface under its own key,
    // so a gated build would have the page and no way to move anything.
    void Request();

    // Why the interface is unusable, so a page can say something true instead of
    // blaming an absent RaceMenu for a RaceMenu that answered.
    //
    // ⚠ THE DISTINCTION IS NOT COSMETIC. "RaceMenu is not loaded" sent a
    // playtester hunting a phantom install problem on a list where RaceMenu was
    // loaded, had answered the exchange, and had already handed us NiTransform
    // one line earlier in the log. The real cause was another mod's bundled
    // skee64.dll winning the overwrite, which no message about loading could
    // ever have pointed at.
    enum class Status {
        kNotRequested,
        kNoMessaging,
        kRaceMenuAbsent,  // no interface map came back: RaceMenu is not there
        kNoBodyMorph,     // it answered, with no BodyMorph entry in the map
        kTooOld,          // it answered, below the floor below
        kReady,
    };

    [[nodiscard]] Status        GetStatus();
    [[nodiscard]] bool          Available();
    [[nodiscard]] std::uint32_t Version();

    // Replace or clear only Fitting Room's fixed morph key. There is no
    // arbitrary-key overload by design, and no wrapper for RaceMenu's global
    // ClearMorphs operation. ApplyOwned performs exactly one RaceMenu refresh
    // after the complete SetMorph batch when the actor's 3D is loaded.
    [[nodiscard]] ApplyResult ApplyOwned(RE::Actor* a_actor,
                                         const std::vector<MorphValue>& a_values);
    [[nodiscard]] ApplyResult ClearOwned(RE::Actor* a_actor);
    [[nodiscard]] bool        HasOwned(RE::Actor* a_actor);

    // The same clear-and-rewrite on kPushUpKey. An empty list is a clear, so a
    // caller that has just worked out the outfit asks for nothing does not need
    // to pick a different function for it.
    [[nodiscard]] ApplyResult ApplyPushUp(RE::Actor* a_actor,
                                          const std::vector<MorphValue>& a_values);
    [[nodiscard]] bool        HasPushUp(RE::Actor* a_actor);

    // ---- the push-up survival probe ------------------------------------------
    //
    // ⚠⚠ DIAGNOSTIC ONLY, AND IT ANSWERS THE QUESTION THE FIELD LOG CANNOT.
    // Field 2026-08-26 22:55: thirty-four push-up writes all landed and RaceMenu
    // accepted every one, then OBody rebuilt the body 0.2 s later, every time.
    // Two very different faults look identical from there. A rebuild that CLEARS
    // our key is an ordering bug and the fix is to re-assert after it; a recipe
    // whose values are too subtle to see is not an ordering bug at all and the
    // fix is different numbers. Reading the key back once the rebuild has
    // settled is the only thing that separates them.
    //
    // Arm it after a write. RunPushUpProbe reads the key back on the world tick
    // and writes one line per read. Silent unless something armed it.
    void ArmPushUpProbe(RE::Actor* a_actor, std::size_t a_written);
    void RunPushUpProbe();

    // ---- the Shape page's key -------------------------------------------
    //
    // ⚠ ONE MORPH AT A TIME, unlike ApplyOwned's clear-and-rewrite. A Shape
    // slider changes one value and the rest of the page must not be touched by
    // it; clearing the key first would mean every drag rewrote every slider,
    // and a value the user set on a control they have since scrolled past would
    // be rebuilt from whatever the page happens to hold.
    //
    // A value of exactly zero REMOVES the morph rather than storing it, for the
    // reason ShapeOverlay::BuildPlan gives about identity node scales: a stored
    // zero leaves our key on a character nobody shaped.
    //
    // ⚠ SAFE TO CALL FROM THE RENDER THREAD. The engine work is marshaled onto
    // the game thread by handle, as NodeTransformApi::Apply does. ApplyOwned
    // above does not, because its caller is already on the game thread.
    ApplyResult SetShapeMorph(RE::Actor* a_actor, const std::string& a_name,
                              float a_value);

    // Drop every Shape morph and nothing else. The outfit's own body preset is
    // under a different key and survives.
    ApplyResult ClearShape(RE::Actor* a_actor);

    // Wear a whole saved shape: clear the key, then write the set, then one
    // refresh.
    //
    // ⚠ CLEARS FIRST, UNLIKE SetShapeMorph. "Apply this shape" means the
    // character ends up wearing that shape and not that shape merged with
    // whatever was already there, which is what leaving the key in place would
    // give. The outfit's own body preset is under a different key throughout
    // and is not touched either way.
    ApplyResult ApplyShape(RE::Actor* a_actor, const std::vector<MorphValue>& a_values);

    // What this character currently holds under the Shape key, for the named
    // morphs. Reads on the calling thread: these are map lookups in RaceMenu's
    // store rather than mesh work, and the page needs them in the frame it
    // opens.
    [[nodiscard]] std::vector<MorphValue> ReadShape(
        RE::Actor* a_actor, const std::vector<std::string>& a_names);

    // EVERYTHING this character holds under the Shape key, without being told
    // what to look for.
    //
    // ⚠⚠ NOT ReadShape WITH A LONGER LIST. ReadShape can only see morphs some
    // installed category file happens to name, which is a vocabulary rather
    // than the truth about this character; this enumerates the store itself.
    // The difference is the whole point when the job is to put a shape back
    // exactly as it was, because a name-driven snapshot restores the part it
    // could see and silently zeroes the rest.
    //
    // Reads on the calling thread, as ReadShape does and for the same reason.
    [[nodiscard]] std::vector<MorphValue> SnapshotShape(RE::Actor* a_actor);

    // One census of EVERY morph key on the actor: per key, how many morphs and
    // their summed magnitude; then every slider name two or more keys write,
    // with each key's value. Field 2026-08-22 (round sixteen): the switched
    // character reads visibly thicker than the look, "like two body slider
    // values on top of each other", and RaceMenu SUMS keys, so the doubled
    // silhouette has to show up here as one name under two keys. Every writer
    // on our side clears before it writes and OBody's config blacklists the
    // player from distribution, so the second painter is unidentified until a
    // round runs with this line in the log. a_moment names the call site.
    void LogKeyCensus(RE::Actor* a_actor, const char* a_moment);

}  // namespace OS::RaceMenuMorphApi
