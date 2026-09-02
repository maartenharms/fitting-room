#include "CharacterSex.h"

#include "PCH.h"

namespace OS::CharacterSex {

    namespace {
        bool g_held{ false };
        bool g_female{ false };
        // What the base read before we first wrote it this epoch, and whether
        // we ever did. Separate from g_held on purpose: g_held is what a LOOK
        // states and travels in the save, this is what the ENGINE had and never
        // does.
        bool g_baselineTaken{ false };
        bool g_baselineFemale{ false };
    }  // namespace

    void Hold(bool a_female) {
        g_held   = true;
        g_female = a_female;
    }

    bool Held(bool& a_female) {
        if (!g_held) {
            return false;
        }
        a_female = g_female;
        return true;
    }

    void Clear() {
        g_held   = false;
        g_female = false;
    }

    void NoteBaselineBeforeWrite(RE::Actor* a_actor) {
        if (g_baselineTaken || !a_actor) {
            return;
        }
        auto* const base = a_actor->GetActorBase();
        if (!base) {
            return;
        }
        using Flag       = RE::ACTOR_BASE_DATA::Flag;
        g_baselineFemale = base->actorData.actorBaseFlags.all(Flag::kFemale);
        g_baselineTaken  = true;
        spdlog::info("CharacterSex: the base read {} before we touched it; that is what the "
                     "load boundary puts back.",
                     g_baselineFemale ? "female" : "male");
    }

    bool RestoreBaseline(RE::Actor* a_actor) {
        if (!g_baselineTaken) {
            return false;  // we never wrote; there is nothing of ours to undo
        }
        const bool wanted = g_baselineFemale;
        // ⚠ SPENT EITHER WAY. The next epoch takes its own baseline, and a
        // failure to write here must not leave a stale one behind to be applied
        // to some third character later.
        g_baselineTaken  = false;
        g_baselineFemale = false;
        if (!a_actor) {
            return false;
        }
        auto* const base = a_actor->GetActorBase();
        if (!base) {
            return false;
        }
        using Flag       = RE::ACTOR_BASE_DATA::Flag;
        const bool isNow = base->actorData.actorBaseFlags.all(Flag::kFemale);
        if (isNow == wanted) {
            return false;  // the load already put it back; say nothing
        }
        if (wanted) {
            base->actorData.actorBaseFlags.set(Flag::kFemale);
        } else {
            base->actorData.actorBaseFlags.reset(Flag::kFemale);
        }
        spdlog::info("CharacterSex: the base read {} at this boundary and we had written it, "
                     "so it goes back to the {} it was before Fitting Room touched it. Without "
                     "this the next save loaded in this session wears the outgoing look's sex.",
                     isNow ? "female" : "male", wanted ? "female" : "male");
        return true;
    }

    bool Apply(RE::Actor* a_actor) {
        if (!g_held || !a_actor) {
            return false;
        }
        auto* const base = a_actor->GetActorBase();
        if (!base) {
            return false;
        }
        using Flag       = RE::ACTOR_BASE_DATA::Flag;
        const bool isNow = base->actorData.actorBaseFlags.all(Flag::kFemale);
        if (isNow == g_female) {
            // ⚠ SILENT WHEN IT ALREADY AGREES. This runs on a load path that
            // may reach it more than once, and a line per visit would say
            // nothing about the fault it exists for.
            return false;
        }
        // ⚠ THE BASELINE FIRST. Apply is the load-boundary writer and it can
        // be the first one of an epoch: a save whose 'SEXF' flips the flag has
        // touched the shared base just as surely as an apply did, and the next
        // save loaded in the same session must not inherit it.
        NoteBaselineBeforeWrite(a_actor);
        if (g_female) {
            base->actorData.actorBaseFlags.set(Flag::kFemale);
        } else {
            base->actorData.actorBaseFlags.reset(Flag::kFemale);
        }
        spdlog::info("CharacterSex: the base read {} and this character's look says {}; "
                     "the flag is put back before the body is built.",
                     isNow ? "female" : "male", g_female ? "female" : "male");
        return true;
    }

}  // namespace OS::CharacterSex
