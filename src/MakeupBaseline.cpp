#include "MakeupBaseline.h"

#include "MakeupApi.h"
#include "RaceTint.h"
#include "StyleRef.h"  // Make: the {modName, localFormID} race key the stamp holds

#include <atomic>
#include <mutex>

namespace OS::MakeupBaseline {

    namespace {
        std::mutex                                  g_mutex;
        std::vector<ProfileCodec::MakeupEntry>      g_entries;
        std::optional<ProfileCodec::CharacterBlock> g_who;
        // True while the record claims anything, a bare list included. See
        // Recorded() in the header for the save that taught the difference.
        bool                                        g_recorded{ false };
        // Two shots per load, spent by the checks; see the overlay baseline's
        // NoteLoadBoundary for the storm that taught the number.
        std::atomic<int> g_convergeShots{ 0 };

        [[nodiscard]] bool IsPlayer(RE::Actor* a_actor) {
            auto* const player = RE::PlayerCharacter::GetSingleton();
            return a_actor && player && a_actor == player;
        }

        // The r35 refusal, shared by the sync and the reassert: a list that is
        // not this race's makes every index a stranger's slot.
        [[nodiscard]] bool ListIsThisRaces(RE::Actor* a_actor, std::size_t a_layers) {
            const auto raceSlots = RaceTint::Slots(a_actor);
            return raceSlots.empty() || raceSlots.size() == a_layers;
        }

        // One layer state against another, on the channels a write moves.
        // ⚠ BOTH SIDES ARE STORED BYTES, so a byte that moved is a divergence;
        // the strength float gets a codec-roundtrip epsilon and nothing more.
        // a_want is the plan's target for a slot, a_live what the slot holds.
        //
        // ⚠⚠ A TARGET WITHOUT A TEXTURE MAKES NO CLAIM ON THE ART, AND FOR ONE
        // NIGHT THIS CONVICTED ON IT. A cleared slot's target is a bare
        // LayerState, WirePath leaves the mask's texture alone for it, and the
        // live slot keeps the race's or a pack's art by design. Comparing
        // hasTexture across that pair made EVERY converge after a load log
        // "skee restored a DIFFERENT tint list" and push, whether or not skee
        // had, and the line was read as evidence more than once on 2026-09-02.
        // Two off layers agree whatever colour their bytes still hold, too.
        [[nodiscard]] bool StateNear(const MakeupPlan::LayerState& a_want,
                                     const MakeupPlan::LayerState& a_live) {
            const float d = a_want.strength - a_live.strength;
            if ((d < 0.0f ? -d : d) >= 0.01f) {
                return false;
            }
            if (a_want.strength <= 0.0f && a_live.strength <= 0.0f) {
                return true;  // off is off; an unseen colour is not a divergence
            }
            if (!(a_want.tint == a_live.tint)) {
                return false;
            }
            if (!a_want.hasTexture) {
                return true;  // the art is the slot's own, whatever it is
            }
            return a_live.hasTexture &&
                   MakeupPlan::SameTexturePath(a_want.texture, a_live.texture);
        }

        // The stamp for the character as they stand right now, or nullopt when
        // the race key cannot be built. Same fields, same source as
        // ProfileCapture's character block, so the two never disagree about
        // what "who" means.
        [[nodiscard]] std::optional<ProfileCodec::CharacterBlock> WhoNow(
            RE::Actor* a_actor) {
            auto* const race = a_actor ? a_actor->GetRace() : nullptr;
            ProfileCodec::CharacterBlock who;
            if (!race || !StyleRef::Make(race, who.race)) {
                return std::nullopt;
            }
            auto* const base = a_actor->GetActorBase();
            who.female       = base && base->IsFemale();
            return who;
        }
    }  // namespace

    void SyncFromList(RE::Actor* a_actor) {
        if (!IsPlayer(a_actor)) {
            return;
        }
        const auto layers = MakeupApi::Layers(a_actor);
        if (layers.empty() || !ListIsThisRaces(a_actor, layers.size())) {
            spdlog::debug("MakeupBaseline: not re-synced, the live list is not "
                          "this race's (or empty); the record keeps its state.");
            return;
        }
        const auto live = MakeupApi::Read(a_actor);
        std::vector<ProfileCodec::MakeupEntry> entries;
        for (std::size_t i = 0; i < layers.size() && i < live.size(); ++i) {
            if (!MakeupPlan::Occupied(live[i])) {
                continue;  // unworn stays absent; absence is not a claim
            }
            ProfileCodec::MakeupEntry entry{};
            entry.index = static_cast<std::uint32_t>(i);
            entry.type  = layers[i].type;
            entry.state = live[i];
            entries.push_back(std::move(entry));
        }
        const auto count = entries.size();
        // Stamped beside the entries, never separately: a stamp that outlived
        // the list it was made for would vouch for a stranger.
        auto who = WhoNow(a_actor);
        {
            const std::lock_guard lock{ g_mutex };
            g_entries  = std::move(entries);
            g_who      = std::move(who);
            g_recorded = true;
        }
        spdlog::debug("MakeupBaseline: the record re-synced from the live list "
                      "({} worn entr{}).",
                      count, count == 1 ? "y" : "ies");
    }

    std::vector<ProfileCodec::MakeupEntry> Snapshot() {
        const std::lock_guard lock{ g_mutex };
        return g_entries;
    }

    bool HasAny() {
        const std::lock_guard lock{ g_mutex };
        return !g_entries.empty();
    }

    bool Recorded() {
        const std::lock_guard lock{ g_mutex };
        return g_recorded;
    }

    void Drop() {
        const std::lock_guard lock{ g_mutex };
        g_entries.clear();
        g_who.reset();
        g_recorded = true;
    }

    void Adopt(const std::vector<ProfileCodec::MakeupEntry>& a_look) {
        const std::lock_guard lock{ g_mutex };
        g_entries  = a_look;
        g_who.reset();
        g_recorded = true;
    }

    bool BareOfMakeup() {
        const std::lock_guard lock{ g_mutex };
        if (!g_recorded) {
            return false;
        }
        for (const auto& entry : g_entries) {
            if (entry.type != static_cast<std::uint32_t>(MakeupPlan::Type::kSkinTone) &&
                MakeupPlan::Occupied(entry.state)) {
                return false;
            }
        }
        return true;
    }

    std::optional<ProfileCodec::CharacterBlock> Who() {
        const std::lock_guard lock{ g_mutex };
        return g_who;
    }

    void Restore(const std::vector<ProfileCodec::MakeupEntry>&      a_entries,
                 const std::optional<ProfileCodec::CharacterBlock>& a_who) {
        const std::lock_guard lock{ g_mutex };
        g_entries  = a_entries;
        g_who      = a_who;
        g_recorded = true;  // a save that carried the record carried a claim
    }

    void Clear() {
        const std::lock_guard lock{ g_mutex };
        g_entries.clear();
        g_who.reset();
        g_recorded = false;
    }

    void NoteLoadBoundary() {
        g_convergeShots.store(2, std::memory_order_release);
    }

    std::size_t ReassertRecord(RE::Actor* a_actor) {
        if (!IsPlayer(a_actor)) {
            return 0;
        }
        if (!Recorded()) {
            return 0;  // no claim on this character's list; the game's stands
        }
        // ⚠⚠ THE STAMP OUTRANKS THE LENGTH CHECK BELOW, AND THE NORD 3
        // LEFTOVERS ARE WHY (field 2026-09-02 01:18:40). Umbrael's record
        // crossed a race-switching save, the post-load live list was the NEW
        // race's and so PASSED the r35 length check, and the converge painted
        // her eleven entries onto Nord 3. A record captured on another
        // character is a stranger's claim however well its slots line up.
        // Kept, not dropped: the read of the CURRENT character can itself be
        // transient (a switch mid-flight), and the next successful sync
        // replaces record and stamp together anyway.
        if (const auto who = Who()) {
            const auto now = WhoNow(a_actor);
            if (!now || !(now->race == who->race) || now->female != who->female) {
                spdlog::info(
                    "MakeupBaseline: stood down, the record was captured on "
                    "{}|{:06X} ({}) and this character is not them; a "
                    "stranger's makeup is not re-asserted.",
                    who->race.modName, who->race.localFormID,
                    who->female ? "female" : "male");
                return 0;
            }
        }
        const auto layers = MakeupApi::Layers(a_actor);
        // ⚠ A RECORD BARE OF MAKEUP IS PUSHED ONTO ANY LIST. The r35 refusal
        // exists because a write by index into a stranger's list lands in a
        // stranger's slot; a bare record writes nothing by index, it only
        // clears, and the tone is spared by type or by file name. Field
        // 2026-09-02 03:13: Nord 3 on a 108 slot list, RaceMenu replayed
        // Umbrael's six a second later, and this stand-down left them there.
        if (layers.empty() || (!BareOfMakeup() && !ListIsThisRaces(a_actor, layers.size()))) {
            spdlog::debug("MakeupBaseline: stood down, the live list is not "
                          "this race's. The record is kept for the next look.");
            return 0;
        }
        const auto live   = MakeupApi::Read(a_actor);
        const auto record = Snapshot();
        // The converge IS the face-carried plan: write what the record wears,
        // clear what it does not, keep the skin tone out of it. Same code,
        // same tests, same complexion rule as the apply path.
        auto plan = MakeupPlan::PlanFaceCarried(record, layers, live);
        // ⚠⚠ THE HELD TONE RIDES EVERY CONVERGE BATCH. PlanFaceCarried leaves
        // the skin tone alone on purpose, and the 09-02 field round showed
        // what that costs HERE: a head build restores the list, this converge
        // rewrites the makeup, and its retint composites the HEAD from the
        // list's reverted tone slot while the BODY keeps the held tone. The
        // user's words: "when i edit the skin tone, the skin of the head can
        // change from the body". Carrying the hold in the batch makes
        // WriteNow paint head and body from one value in one breath, which is
        // the two-painter cure the write path has carried since 2026-08-16.
        // No hold, no injection: an unheld tone belongs to the character and
        // the list's own slot is already its truth.
        {
            OverlayPlan::Rgb heldTint{};
            float            heldStrength = 0.0f;
            if (MakeupApi::HeldSkinTone(heldTint, heldStrength)) {
                for (std::size_t i = 0;
                     i < layers.size() && i < plan.target.size(); ++i) {
                    if (layers[i].type ==
                        static_cast<std::uint32_t>(MakeupPlan::Type::kSkinTone)) {
                        auto& slot    = plan.target[i];
                        slot.tint     = heldTint;
                        slot.strength = heldStrength;
                        plan.indices.push_back(i);
                        break;
                    }
                }
            }
        }
        // ⚠ THE FIRST DIVERGENCE IS NAMED, slot and channel, because a
        // colour-only change is invisible to the worn-set watch (its
        // signature is index, strength and art) and this line is then the
        // only witness. Field 2026-09-02 05:27:17: a second load shot found
        // the list DIFFERENT with no CHANGED line beside it.
        bool        diverged = false;
        std::size_t firstIndex = 0;
        std::string firstWhy;
        for (const auto index : plan.indices) {
            if (index < live.size() && !StateNear(plan.target[index], live[index])) {
                diverged   = true;
                firstIndex = index;
                const auto& w = plan.target[index];
                const auto& l = live[index];
                const float d = w.strength - l.strength;
                if ((d < 0.0f ? -d : d) >= 0.01f) {
                    firstWhy = fmt::format("strength {:.3f} wanted, {:.3f} live", w.strength,
                                           l.strength);
                } else if (!(w.tint == l.tint)) {
                    firstWhy = fmt::format("colour ({},{},{}) wanted, ({},{},{}) live", w.tint.r,
                                           w.tint.g, w.tint.b, l.tint.r, l.tint.g, l.tint.b);
                } else {
                    firstWhy = fmt::format("art '{}' wanted, '{}' live", w.texture, l.texture);
                }
                break;
            }
        }
        bool convergeOwed = false;
        for (int shots = g_convergeShots.load(std::memory_order_acquire);
             shots > 0;) {
            if (g_convergeShots.compare_exchange_weak(shots, shots - 1,
                                                      std::memory_order_acq_rel)) {
                convergeOwed = true;
                break;
            }
        }
        if (!diverged && !convergeOwed) {
            spdlog::debug("MakeupBaseline: the live tint list agrees with the "
                          "record; nothing to do.");
            return 0;
        }
        if (!plan.indices.empty()) {
            MakeupApi::Write(a_actor, plan.indices, plan.target);
        }
        if (diverged) {
            spdlog::info(
                "MakeupBaseline: skee restored a DIFFERENT tint list than this "
                "save's record, so the record went back on: {} written, {} "
                "stale cleared, {} skipped for missing slots, {} skin tone "
                "entr{} left alone. The retint that ends the write composites "
                "the face from the corrected list. First divergence at slot {}: {}.",
                plan.written, plan.cleared, plan.skipped, plan.kept,
                plan.kept == 1 ? "y" : "ies", firstIndex, firstWhy);
        } else {
            spdlog::info(
                "MakeupBaseline: the tint list agrees with the record and the "
                "record was pushed once anyway ({} written, {} cleared), so "
                "the composite cannot have drifted from what the list agrees "
                "on.",
                plan.written, plan.cleared);
        }
        return plan.written;
    }

}  // namespace OS::MakeupBaseline
