#include "RaceTint.h"

#include <utility>

namespace OS::RaceTint {

    std::vector<Slot> Slots(RE::Actor* a_actor) {
        std::vector<Slot> out;
        if (!a_actor) {
            return out;
        }
        auto* const base = a_actor->GetActorBase();
        if (!base) {
            return out;
        }
        auto* const race = base->GetRace();
        if (!race) {
            return out;
        }
        // ⚠ THE SEX PICKS THE HALF, and a race carries both. NordRace has 60
        // slots, 30 male followed by 30 female, and the engine reads the half
        // that matches the character. Reading the wrong one would hand back
        // another sex's masks with types that happen to line up.
        // ⚠ AND kNone FALLS BACK TO MALE RATHER THAN INDEXING WITH IT. The
        // enum's kNone is -1 as an unsigned, so using it as a subscript reads
        // far outside the object.
        const auto  sex   = base->GetSex();
        const auto  which = sex == RE::SEXES::kFemale ? RE::SEXES::kFemale : RE::SEXES::kMale;
        auto* const face  = race->faceRelatedData[which];
        if (!face || !face->tintMasks) {
            return out;
        }

        out.reserve(face->tintMasks->size());
        for (auto* const asset : *face->tintMasks) {
            Slot slot;
            if (!asset) {
                // ⚠ A HOLE KEEPS ITS POSITION. The live list and this one are
                // matched by index, so skipping a null would shift every slot
                // after it and offer each one the neighbour's default.
                out.push_back(std::move(slot));
                continue;
            }
            slot.known = true;
            slot.type  = static_cast<std::uint32_t>(asset->texture.skinTone.underlying());
            if (const char* const name = asset->texture.file.textureName.c_str()) {
                slot.texture = name;
            }
            slot.textureObject = &asset->texture.file;
            if (auto* const preset = asset->texture.presetDefault) {
                // ⚠ READ AS BYTES, WHICH IS WHY THIS ONE CANNOT GET THE
                // PACKING WRONG. RE::Color names its four components, so there
                // is no word here to read in the wrong order: the ABGR against
                // ARGB trap that MakeupPlan documents is a property of the
                // PACKED forms, and this is not one of them.
                slot.hasTint = true;
                slot.tint    = OverlayPlan::Rgb{ preset->color.red, preset->color.green,
                                                 preset->color.blue };
            }
            out.push_back(std::move(slot));
        }
        return out;
    }

}  // namespace OS::RaceTint
