#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace OS {

    enum class BodySex : std::uint8_t {
        kUnknown,
        kFemale,
        kMale,
    };

    enum class BodyFamily : std::uint8_t {
        kUnknown,
        k3BA,
        kUBE,
        kHIMBO,
        kGenericV1,
        // ⚠⚠ APPENDED, AND IT HAS TO STAY APPENDED. The value is written as a
        // byte in places and inserting above kGenericV1 would silently retype
        // every body already stored. CBBE earned its own family when push-up
        // arrived: it carries a real PushUp morph, so lumping it in with every
        // body we cannot name would have hidden the control from the people who
        // can actually use it.
        kCBBE,
    };

    // How much lift an outfit asks the body for. Off writes nothing at all
    // rather than writing zeros, because taking the morphs off is ClearOwned's
    // job and a zero is still a value somebody else has to composite.
    enum class PushUpMode : std::uint8_t {
        kNone = 0,
        kSubtle = 1,
        kFull = 2,
    };

    struct BodySliderValue {
        std::string name;
        std::string displayName;
        std::string category{ "Other" };
        float       smallValue{ 0.0f };
        float       bigValue{ 0.0f };

        friend bool operator==(const BodySliderValue&, const BodySliderValue&) = default;
    };

    struct BodyPreset {
        static constexpr std::uint32_t kSchemaVersion = 1;

        std::uint32_t                version{ kSchemaVersion };
        std::string                  id;
        std::string                  name;
        BodySex                     sex{ BodySex::kUnknown };
        BodyFamily                  family{ BodyFamily::kUnknown };
        std::string                  sourceSet;
        std::string                  sourcePreset;
        std::vector<std::string>     groups;
        std::vector<BodySliderValue> sliders;

        friend bool operator==(const BodyPreset&, const BodyPreset&) = default;
    };

    [[nodiscard]] inline const char* BodySexName(BodySex a_sex) {
        switch (a_sex) {
            case BodySex::kFemale: return "Female";
            case BodySex::kMale: return "Male";
            case BodySex::kUnknown: return "Unknown";
        }
        return "Unknown";
    }

    [[nodiscard]] inline std::string_view BodySexId(BodySex a_sex) {
        switch (a_sex) {
            case BodySex::kFemale: return "female";
            case BodySex::kMale: return "male";
            case BodySex::kUnknown: return "unknown";
        }
        return "unknown";
    }

    [[nodiscard]] inline BodySex BodySexFromId(std::string_view a_id) {
        if (a_id == "female") return BodySex::kFemale;
        if (a_id == "male") return BodySex::kMale;
        return BodySex::kUnknown;
    }

    // ⚠ EXPOSED SO THERE IS EXACTLY ONE OF THESE, and it lives in the header
    // so the suite can reach it. The SFW option asks the same question of a set
    // name that the scan does, and two classifiers that can disagree would pair
    // a 3BA body with a CBBE covering on whichever install made them differ.
    //
    // ⚠⚠ THE 3BA TEST MUST COME BEFORE THE CBBE ONE. Every 3BA set is named
    // something like "CBBE 3BBB Body Amazing", so a cbbe test placed first would
    // claim every 3BA body there is. The suite pins that ordering.
    [[nodiscard]] inline BodyFamily FamilyFromSetSignature(std::string_view a_signature) {
        std::string lower{ a_signature };
        for (auto& c : lower) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (lower.find("himbo") != std::string::npos) return BodyFamily::kHIMBO;
        if (lower.find("ube") != std::string::npos) return BodyFamily::kUBE;
        if (lower.find("3ba") != std::string::npos ||
            lower.find("3bbb") != std::string::npos) {
            return BodyFamily::k3BA;
        }
        if (lower.find("cbbe") != std::string::npos) return BodyFamily::kCBBE;
        return BodyFamily::kGenericV1;
    }

    [[nodiscard]] inline const char* BodyFamilyName(BodyFamily a_family) {
        switch (a_family) {
            case BodyFamily::k3BA: return "3BA";
            case BodyFamily::kUBE: return "UBE";
            case BodyFamily::kHIMBO: return "HIMBO";
            case BodyFamily::kCBBE: return "CBBE";
            case BodyFamily::kGenericV1: return "Other";
            case BodyFamily::kUnknown: return "Unknown";
        }
        return "Unknown";
    }

    [[nodiscard]] inline std::string_view BodyFamilyId(BodyFamily a_family) {
        switch (a_family) {
            case BodyFamily::k3BA: return "3ba";
            case BodyFamily::kUBE: return "ube";
            case BodyFamily::kHIMBO: return "himbo";
            case BodyFamily::kCBBE: return "cbbe";
            case BodyFamily::kGenericV1: return "generic-v1";
            case BodyFamily::kUnknown: return "unknown";
        }
        return "unknown";
    }

    [[nodiscard]] inline BodyFamily BodyFamilyFromId(std::string_view a_id) {
        if (a_id == "3ba") return BodyFamily::k3BA;
        if (a_id == "ube") return BodyFamily::kUBE;
        if (a_id == "himbo") return BodyFamily::kHIMBO;
        if (a_id == "cbbe") return BodyFamily::kCBBE;
        if (a_id == "generic-v1") return BodyFamily::kGenericV1;
        return BodyFamily::kUnknown;
    }

    [[nodiscard]] inline bool BodyPresetApplicable(const BodyPreset& a_preset) {
        if (a_preset.id.empty() || a_preset.name.empty() || a_preset.sourceSet.empty() ||
            a_preset.sex == BodySex::kUnknown ||
            a_preset.family == BodyFamily::kUnknown || a_preset.sliders.empty()) {
            return false;
        }
        return std::ranges::all_of(a_preset.sliders, [](const BodySliderValue& a_slider) {
            return !a_slider.name.empty() && std::isfinite(a_slider.smallValue) &&
                   std::isfinite(a_slider.bigValue);
        });
    }

    [[nodiscard]] inline bool BodySexCompatible(BodySex a_preset, bool a_actorFemale) {
        return a_preset != BodySex::kUnknown &&
               ((a_preset == BodySex::kFemale) == a_actorFemale);
    }

    // ---- does this preset suit the body the character is actually on? -------

    // The body a character is wearing, as far as it could be worked out. Both
    // halves are optional and either can be unknown on its own.
    //
    // ⚠ CAPTURED ONCE PER SUBJECT, NEVER READ LIVE FROM THE PREVIEW. Both
    // pickers preview a preset on the character as you click it, so a filter
    // keyed on what the character wears RIGHT NOW would re-answer mid-session:
    // click one preset and the rest of the list rearranges itself under the
    // cursor. The body being filtered against is the one they arrived with.
    struct BuiltBody {
        // ⚠⚠ THE MESH IS THE REAL ANSWER AND THE OTHER TWO ARE THE FALLBACK.
        // This filter first asked OBody which body the character was wearing,
        // and both pickers show a preset by PUTTING IT ON the character, so the
        // answer was always whichever preset had just been clicked. Field
        // 2026-08-08: it walked from UBE to 3BA and back by following the
        // cursor, and could hide 41 of about 44 presets. A mesh path is the
        // opposite kind of fact, because nothing this mod does writes one. It
        // comes from Actor::GetSkin(), the character's SKIN rather than what is
        // covering their body slot, so race and gender both decide it, which is
        // exactly how the body is chosen on the reference install.
        std::string meshKey;    // a BodyMeshKey, "" if not worked out
        std::string sourceSet;  // the BodySlide slider set, "" if not worked out
        BodyFamily  family{ BodyFamily::kUnknown };

        [[nodiscard]] bool Known() const {
            return !meshKey.empty() || !sourceSet.empty() ||
                   family != BodyFamily::kUnknown;
        }

        friend bool operator==(const BuiltBody&, const BuiltBody&) = default;
    };

    [[nodiscard]] inline bool BodySetNameEqual(std::string_view a_left,
                                               std::string_view a_right) {
        return std::ranges::equal(a_left, a_right, [](char a_l, char a_r) {
            return std::tolower(static_cast<unsigned char>(a_l)) ==
                   std::tolower(static_cast<unsigned char>(a_r));
        });
    }

    // ⚠⚠ THIS FAILS OPEN AT EVERY UNKNOWN AND THAT IS THE WHOLE DESIGN. Only a
    // PROVEN mismatch hides a preset. A player whose install we could not
    // classify gets the list they have always had; the alternative is an empty
    // picker and no explanation, which is a worse failure than being offered a
    // preset that does not suit. NpcHair.cpp:2167-2194 is this project's scar
    // from the other choice: a verdict that keyed on the environment greyed
    // hairstyles permanently, and installing the missing mod could not undo it.
    //
    // The rules, in order:
    //   * nothing known about the body    -> show everything
    //   * two DIFFERENT meshes            -> a misfit, proven
    //   * the very same slider set        -> fits by construction
    //   * either side unclassified        -> not ours to hide
    //   * either side a catch-all family  -> not ours to hide
    //   * otherwise                       -> the two NAMED families must match
    //
    // ⚠ ONLY TWO NAMED FAMILIES CAN DISAGREE. kGenericV1 is the catch-all for
    // every body that is not one of the four we can name, so it is never
    // evidence: not against another catch-all, and not against a named family
    // either. Both cases sit on the open side of the fence, where an
    // unclassified case belongs.
    //
    // ⚠ DIFFERENT SET NAMES ARE NOT A MISMATCH. One body ships several sets
    // ("CBBE Body", "CBBE Body Physics", "CBBE 3BBB Body Amazing") and presets
    // move freely between them, so set equality can only ever prove a fit and
    // never a misfit. The family is what carries the negative.
    // a_presetMesh is the mesh that preset's slider set builds, and when both
    // it and the body's are known it is the ONLY thing consulted: two meshes
    // either are the same body or are not, and nothing softer needs to be
    // weighed against that. The set and family arms below are what answer when
    // either mesh could not be read.
    [[nodiscard]] inline bool BodyFitCompatible(std::string_view a_presetSet,
                                                BodyFamily       a_presetFamily,
                                                std::string_view a_presetMesh,
                                                const BuiltBody& a_built) {
        if (!a_built.Known()) {
            return true;
        }
        // ⚠⚠ A DIFFERENT MESH PROVES A MISFIT. THE SAME MESH PROVES NOTHING,
        // and that asymmetry is the correction to the first cut, which returned
        // the comparison outright and so stopped consulting anything else.
        // Several body mods build to the SAME vanilla path, so two presets can
        // agree on the mesh and still be for different bodies: field
        // 2026-08-08, UBE presets went on being offered to a 3BA character
        // because both sets output to actors\character\character assets.
        // Falling through to the family arms below catches those, and keeps
        // every fail-open rule underneath intact.
        if (!a_presetMesh.empty() && !a_built.meshKey.empty() &&
            a_presetMesh != a_built.meshKey) {
            return false;
        }
        if (!a_presetSet.empty() && !a_built.sourceSet.empty() &&
            BodySetNameEqual(a_presetSet, a_built.sourceSet)) {
            return true;
        }
        // ⚠⚠ kGenericV1 IS AN UNKNOWN WEARING A NAME, AND IT USED TO HIDE
        // THINGS. FamilyFromSetSignature returns it for every set whose name
        // carries none of himbo / ube / 3ba / 3bbb / cbbe, which is BHUNP, UNP,
        // Fusion Girl, COCO, TBD, Touched by Dibella and plain vanilla. It is
        // the answer "we could not name this", not a family, so comparing it
        // for equality against a named family turned every one of those presets
        // into a PROVEN misfit on any rig whose body we could name - the exact
        // thing the rule above forbids. Two catch-alls still match each other,
        // as the note further up says, and now so does a catch-all against
        // anything: an unnamed body is evidence of nothing either way.
        if (a_presetFamily == BodyFamily::kUnknown ||
            a_built.family == BodyFamily::kUnknown ||
            a_presetFamily == BodyFamily::kGenericV1 ||
            a_built.family == BodyFamily::kGenericV1) {
            return true;
        }
        return a_presetFamily == a_built.family;
    }

    [[nodiscard]] inline bool BodyFitCompatible(std::string_view a_presetSet,
                                                BodyFamily       a_presetFamily,
                                                const BuiltBody& a_built) {
        return BodyFitCompatible(a_presetSet, a_presetFamily, {}, a_built);
    }

    // ⚠ A BodyPreset CARRIES NO MESH OF ITS OWN, on purpose: it is written to
    // disk and the mesh is a property of the install, not of the preset. The
    // caller looks it up through BodySlideCatalogSnapshot::MeshForSet and uses
    // the four-argument overload. This one is the fallback for callers that
    // have no catalog to hand, and it is weaker.
    [[nodiscard]] inline bool BodyFitCompatible(const BodyPreset& a_preset,
                                                const BuiltBody&  a_built) {
        return BodyFitCompatible(a_preset.sourceSet, a_preset.family, {}, a_built);
    }

    [[nodiscard]] inline bool BodyFitCompatible(const BodyPreset& a_preset,
                                                std::string_view  a_presetMesh,
                                                const BuiltBody&  a_built) {
        return BodyFitCompatible(a_preset.sourceSet, a_preset.family, a_presetMesh,
                                 a_built);
    }

}  // namespace OS
