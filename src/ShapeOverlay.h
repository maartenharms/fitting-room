#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The Shape overlay: node scales laid over a character, PER CHARACTER.
//
// ⚠ NOTHING IS STORED HERE OR ANYWHERE ELSE IN FITTING ROOM, AND THAT IS THE
// DESIGN. RaceMenu's NiTransformInterface serialises its own overrides into its
// co-save, keyed by reference, node and key name, so a value written under our
// key comes back on the next load without us keeping a copy. This is the same
// call BodyWeight makes and for the same reason: a second copy of a value
// somebody else persists disagrees with the original the moment anything but us
// moves it. It is also why the Shape page needs no Outfit codec bump and no
// term in EditorGate::StillDirty. There is no staged edit to commit, because
// the slider IS the edit.
//
// ⚠ PER CHARACTER WAS THE USER'S CALL (2026-08-06), not an inherited default.
// Eyes and brows shipped per character and were moved to per outfit; body
// weight stayed per character; this was asked rather than assumed a third time.
// Read that before "making it consistent with the outfit" - it already is
// consistent, with the other per-character dimension.
//
// ⚠ NO HEAD REBUILD ANYWHERE IN THIS, which is the whole reason it is worth
// more than the eye and brow work. A node transform moves a bone; it does not
// re-bake a face, so neither the hair-tint loss nor the complexion damage that
// confines eyes and brows to the player applies here. The overrides are keyed
// by REFERENCE rather than by actor base, so there is no unique-base refusal
// either: a generic actor may be shaped and only that one instance changes.
// Whether followers survive it in practice is a field question, not a
// derivation, and it is the first thing to test.
namespace OS::ShapeOverlay {

    // Our key inside RaceMenu's transform store, matching the idiom
    // RaceMenuMorphApi already uses for body morphs. Everything written goes
    // under this and nothing else is ever touched: a real preset in the
    // reference load order carries transforms under RMX_Head and RMXPlugin, so
    // other mods are layering through the same system by name and a broad
    // clear would take their work with ours.
    inline constexpr const char* kOwnedKey = "FittingRoom.Shape";

    // A scale of exactly this is "not adjusted" and is never written.
    inline constexpr float kDefaultScale = 1.0f;

    // How close to kDefaultScale still counts as untouched. A slider drag lands
    // on values like 0.99999994, and writing those as overrides would leave a
    // key on every bone the user ever nudged and then put back.
    inline constexpr float kDefaultEpsilon = 0.0005f;

    enum class Group : std::uint8_t {
        kHeadNeck = 0,
        kTorso    = 1,
        kArms     = 2,
        kLegs     = 3,
    };

    [[nodiscard]] inline constexpr const char* GroupLabel(Group a_group) {
        switch (a_group) {
            case Group::kHeadNeck: return "Head and neck";
            case Group::kTorso:    return "Torso";
            case Group::kArms:     return "Arms";
            case Group::kLegs:     return "Legs";
        }
        return "";
    }

    // One control on the page, and the one or two bones it drives.
    //
    // ⚠ NAMED FOR WHAT IT DOES, NOT FOR THE BONE. "Forearms" is a control; "NPC
    // L Forearm [LLar]" is an implementation detail, and a free-text node field
    // would be a debug tool rather than a feature.
    //
    // ⚠ LEFT AND RIGHT MOVE TOGETHER. Every paired control writes both bones
    // with one value. Asymmetry is a sculpting request and this is not a
    // sculptor; RaceMenu is, and it is already installed.
    // ⚠ label AND key ARE BOTH HERE ON PURPOSE. key is what the page draws,
    // through FUCK::Translate; label is the English behind it, and it is what
    // the log prints. Logging the key would put "$FR_Shape_Forearms" in a field
    // report, and logging the translated string would put it there in the
    // player's language, which is worse. The value in tools/add_keys.py for
    // each key is this label.
    struct Slider {
        const char* id;       // stable across renames, used in logs
        const char* label;    // the English, and the fallback if a key is missing
        const char* key;      // "$FR_Shape_..." as the translations file spells it
        Group       group;
        const char* bones[2]; // second is nullptr for the unpaired ones
        float       minScale;
        float       maxScale;
    };

    // ⚠ CLAMPS ARE PER CONTROL AND NOT GLOBAL. A hand at 1.15 reads as a
    // character with big hands; a chest at 1.15 reads as a bug, because scale
    // is inherited down the skeleton and a spine bone drags everything above it
    // with it. The parts nobody looks at closely get room, the parts that carry
    // the silhouette get less.
    //
    // ⚠ SCALE INHERITS DOWN THE HIERARCHY, which is why there is no separate
    // control for anything below a bone already listed. Scaling the upper arm
    // takes the forearm and the hand with it, and Forearms and Hands then act
    // relative to whatever Arms already did. That is the SAM-style behaviour
    // this is imitating, not a defect.
    //
    // ⚠ VANILLA SKELETON NODES ONLY, ON PURPOSE, FOR THE FIRST PASS. Breast and
    // butt bones exist on XPMSSE and the 3BA-family skeletons and are the
    // obvious next entries, but a bone that is absent fails silently: the
    // override is stored and nothing moves. Putting a skeleton-dependent
    // control in the first field test would make a null result unreadable,
    // because "the mechanism does not work" and "your skeleton lacks that bone"
    // look identical from the chair.
    //
    // ⚠ THE TRAILING SPACE IN "NPC L Foot [Lft ]" IS REAL. Vanilla's foot and
    // COM nodes pad their bracketed short name to four characters, and a lookup
    // for "[Lft]" matches nothing at all.
    inline constexpr Slider kSliders[] = {
        { "head", "Head size", "$FR_Shape_Head", Group::kHeadNeck,
          { "NPC Head [Head]", nullptr },                        0.90f, 1.10f },
        { "neck", "Neck", "$FR_Shape_Neck", Group::kHeadNeck,
          { "NPC Neck [Neck]", nullptr },                        0.90f, 1.10f },

        { "chest", "Chest", "$FR_Shape_Chest", Group::kTorso,
          { "NPC Spine2 [Spn2]", nullptr },                      0.92f, 1.08f },
        { "waist", "Waist", "$FR_Shape_Waist", Group::kTorso,
          { "NPC Spine1 [Spn1]", nullptr },                      0.92f, 1.08f },
        { "shoulders", "Shoulders", "$FR_Shape_Shoulders", Group::kTorso,
          { "NPC L Clavicle [LClv]", "NPC R Clavicle [RClv]" },   0.90f, 1.10f },

        { "arms", "Arms", "$FR_Shape_Arms", Group::kArms,
          { "NPC L UpperArm [LUar]", "NPC R UpperArm [RUar]" },   0.90f, 1.10f },
        { "forearms", "Forearms", "$FR_Shape_Forearms", Group::kArms,
          { "NPC L Forearm [LLar]", "NPC R Forearm [RLar]" },     0.90f, 1.10f },
        { "hands", "Hands", "$FR_Shape_Hands", Group::kArms,
          { "NPC L Hand [LHnd]", "NPC R Hand [RHnd]" },           0.85f, 1.15f },

        { "thighs", "Thighs", "$FR_Shape_Thighs", Group::kLegs,
          { "NPC L Thigh [LThg]", "NPC R Thigh [RThg]" },         0.90f, 1.10f },
        { "calves", "Calves", "$FR_Shape_Calves", Group::kLegs,
          { "NPC L Calf [LClf]", "NPC R Calf [RClf]" },           0.90f, 1.10f },
        { "feet", "Feet", "$FR_Shape_Feet", Group::kLegs,
          { "NPC L Foot [Lft ]", "NPC R Foot [Rft ]" },           0.85f, 1.15f },
    };

    inline constexpr std::size_t kSliderCount = sizeof(kSliders) / sizeof(kSliders[0]);

    // Every value the page holds, in kSliders order. Index-parallel by
    // construction: the page draws from kSliders and writes back by the same
    // index, so a new control can never be added to one and not the other.
    using Values = std::vector<float>;

    [[nodiscard]] inline Values DefaultValues() {
        return Values(kSliderCount, kDefaultScale);
    }

    [[nodiscard]] inline constexpr bool IsDefault(float a_scale) {
        const float d = a_scale - kDefaultScale;
        return (d < 0.0f ? -d : d) <= kDefaultEpsilon;
    }

    [[nodiscard]] inline constexpr float Clamp(const Slider& a_slider, float a_scale) {
        if (a_scale < a_slider.minScale) {
            return a_slider.minScale;
        }
        if (a_scale > a_slider.maxScale) {
            return a_slider.maxScale;
        }
        return a_scale;
    }

    // One bone's worth of work for the engine boundary to carry out.
    //
    // ⚠ remove IS NOT "write 1.0". Storing an identity override leaves our key
    // sitting on a bone the user put back, which then shows up in anyone else's
    // VisitNodes and in RaceMenu's own co-save forever. A control returned to
    // its default drops the override instead, so an untouched character carries
    // no Fitting Room key at all.
    struct Adjustment {
        std::string bone;
        float       scale{ kDefaultScale };
        bool        remove{ false };
    };

    // What one control's bones need, which is what a slider drag pushes.
    //
    // Out-of-range values are clamped here rather than refused. They can only
    // arrive from a stored override that a different mod or an older build
    // wrote wider than we allow, and clamping shows the user something they can
    // then move, where refusing would show a control that does nothing.
    [[nodiscard]] inline std::vector<Adjustment> PlanFor(std::size_t a_index,
                                                         float       a_scale) {
        std::vector<Adjustment> plan;
        if (a_index >= kSliderCount) {
            return plan;
        }
        const auto& slider = kSliders[a_index];
        const bool  drop   = IsDefault(a_scale);
        const float scale  = Clamp(slider, a_scale);
        for (const char* bone : slider.bones) {
            if (!bone) {
                continue;
            }
            Adjustment adj;
            adj.bone   = bone;
            adj.scale  = drop ? kDefaultScale : scale;
            adj.remove = drop;
            plan.push_back(std::move(adj));
        }
        return plan;
    }

    // What to push for a complete set of values. Always covers every bone of
    // every control, because the page is authoritative: a control the user
    // dragged back to default has to produce a removal, not an absence, or the
    // previous value stays on the character.
    [[nodiscard]] inline std::vector<Adjustment> BuildPlan(const Values& a_values) {
        std::vector<Adjustment> plan;
        plan.reserve(kSliderCount * 2);
        for (std::size_t i = 0; i < kSliderCount; ++i) {
            const float raw = i < a_values.size() ? a_values[i] : kDefaultScale;
            for (auto& adj : PlanFor(i, raw)) {
                plan.push_back(std::move(adj));
            }
        }
        return plan;
    }

    // Whether anything on the page differs from an untouched character. Drives
    // the reset control and the "this character is unshaped" line.
    [[nodiscard]] inline bool AnyAdjusted(const Values& a_values) {
        for (std::size_t i = 0; i < kSliderCount && i < a_values.size(); ++i) {
            if (!IsDefault(a_values[i])) {
                return true;
            }
        }
        return false;
    }

    // What one bone reported back, as the engine boundary read it.
    struct Reading {
        std::string bone;
        float       scale{ kDefaultScale };
        bool        present{ false };  // an override under our key exists
    };

    // Rebuild the page's values from what is actually on the character.
    //
    // ⚠ THE FIRST BONE OF A PAIR WINS AND THE SECOND IS ONLY A FALLBACK. We
    // always write both sides with one value, so the two agreeing is the normal
    // case and there is no meaning to average. They disagree only when
    // something outside Fitting Room wrote our key, which is not a state to
    // model - taking the left is a defined answer where averaging would invent
    // a third value neither bone holds.
    //
    // A bone with no override reads as the default, so a character shaped
    // before an update that added controls comes back with the new ones
    // untouched rather than at whatever the vector happened to be sized to.
    [[nodiscard]] inline Values ReadBack(const std::vector<Reading>& a_readings) {
        Values values = DefaultValues();
        for (std::size_t i = 0; i < kSliderCount; ++i) {
            const auto& slider = kSliders[i];
            for (const char* bone : slider.bones) {
                if (!bone) {
                    continue;
                }
                bool found = false;
                for (const auto& r : a_readings) {
                    if (r.present && r.bone == bone) {
                        values[i] = Clamp(slider, r.scale);
                        found     = true;
                        break;
                    }
                }
                if (found) {
                    break;  // the left side answered; the right is only a fallback
                }
            }
        }
        return values;
    }

}  // namespace OS::ShapeOverlay
