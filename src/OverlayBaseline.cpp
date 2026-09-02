#include "OverlayBaseline.h"

#include "Overlay1P.h"
#include "OverlayApi.h"

#include <atomic>
#include <mutex>

namespace OS::OverlayBaseline {

    namespace {
        // ⚠ ANY THREAD. OverlayApi::Write is documented safe to call from the
        // render thread (the editor draws through FUCK's Present hook and the
        // work is marshaled by handle), so the record it feeds is reachable
        // from there too. The lock is uncontended in practice: a write is a
        // user action, not a per-frame one.
        std::mutex                  g_mutex;
        ProfileCodec::OverlaysBlock g_block;
        bool                        g_any{ false };

        [[nodiscard]] bool IsPlayer(RE::Actor* a_actor) {
            auto* const player = RE::PlayerCharacter::GetSingleton();
            return a_actor && player && a_actor == player;
        }
    }  // namespace

    void Note(RE::Actor* a_actor, const std::string& a_node,
              const OverlayPlan::LayerState& a_state) {
        if (!IsPlayer(a_actor) || a_node.empty()) {
            return;
        }
        // The node string is what skee keys on; the location and index are what
        // a look's block is addressed by, and the installed layer list is the
        // only thing that maps between them.
        const OverlayPlan::Layer* found = nullptr;
        for (const auto& layer : OverlayApi::Layers()) {
            if (layer.node == a_node) {
                found = &layer;
                break;
            }
        }
        if (!found) {
            return;  // a node this install does not have: nothing to record
        }
        const std::lock_guard lock{ g_mutex };
        auto&                 entries = g_block.byLocation[OverlayPlan::Slot(found->location)];
        for (auto& entry : entries) {
            if (entry.index == found->index) {
                entry.state = a_state;  // last write wins, as on the layer itself
                g_any       = true;
                return;
            }
        }
        ProfileCodec::OverlayEntry entry{};
        entry.index = found->index;
        entry.state = a_state;
        entries.push_back(entry);
        g_any = true;
    }

    void Forget(RE::Actor* a_actor, const std::string& a_node) {
        if (!IsPlayer(a_actor) || a_node.empty()) {
            return;
        }
        const OverlayPlan::Layer* found = nullptr;
        for (const auto& layer : OverlayApi::Layers()) {
            if (layer.node == a_node) {
                found = &layer;
                break;
            }
        }
        if (!found) {
            return;  // a node this install does not have: nothing recorded
        }
        const std::lock_guard lock{ g_mutex };
        if (ForgetIn(g_block, found->location, found->index)) {
            g_any = AnyIn(g_block);
        }
    }

    ProfileCodec::OverlaysBlock Snapshot() {
        const std::lock_guard lock{ g_mutex };
        return g_block;
    }

    bool HasAny() {
        const std::lock_guard lock{ g_mutex };
        return g_any;
    }

    void Restore(const ProfileCodec::OverlaysBlock& a_block) {
        const std::lock_guard lock{ g_mutex };
        g_block = a_block;
        g_any   = AnyIn(g_block);
    }

    void Clear() {
        const std::lock_guard lock{ g_mutex };
        g_block = ProfileCodec::OverlaysBlock{};
        g_any   = false;
    }

    namespace {
        // Reset by NoteLoadBoundary, spent by the checks that run after it.
        // ⚠⚠ TWO SHOTS, NOT ONE, AND THE 23:45 FIELD ROUND IS WHY. The single
        // converge ran half a second after the load, its writes and their
        // pushes landed first, and skee's cosave replay and the engine's own
        // rebuilds repainted the clones AFTER us: the store ended right and
        // the drawn face pale. The second armed check runs seconds later, on
        // the far side of that storm, which is the same answer Body-Changer-NG
        // measured for the same fight (verify again after skee's last deferred
        // rebuild). Atomic because the load path and the game-thread check
        // race only in the harmless direction (an extra push is idempotent).
        std::atomic<int> g_convergeShots{ 0 };
    }  // namespace

    void NoteLoadBoundary() {
        g_convergeShots.store(2, std::memory_order_release);
    }

    void SyncFromStore(RE::Actor* a_actor) {
        if (!IsPlayer(a_actor) || !OverlayApi::Available()) {
            return;
        }
        ProfileCodec::OverlaysBlock block;
        std::size_t                 withArt = 0;
        for (const auto& layer : OverlayApi::Layers()) {
            const auto state = OverlayApi::Read(a_actor, layer.node);
            if (!OccupiedLoose(state)) {
                continue;  // bare stays absent; an absent entry is not a claim
            }
            ProfileCodec::OverlayEntry entry{};
            entry.index = layer.index;
            entry.state = state;
            block.byLocation[OverlayPlan::Slot(layer.location)].push_back(entry);
            ++withArt;
        }
        Restore(block);
        spdlog::info(
            "OverlayBaseline: the record re-synced from skee's store ({} "
            "layer(s) with art), so a visit's edits are the record's now too.",
            withArt);
    }

    std::size_t ReassertRecord(RE::Actor* a_actor) {
        if (!IsPlayer(a_actor) || !OverlayApi::Available()) {
            return 0;
        }
        if (!HasAny()) {
            return 0;  // nothing recorded for this character; silence is correct
        }
        // One read of every installed layer feeds both the judgement and the
        // clear pass, so the two cannot see different stores.
        const auto& layers = OverlayApi::Layers();
        std::vector<std::pair<OverlayPlan::Layer, OverlayPlan::LayerState>> live;
        live.reserve(layers.size());
        for (const auto& layer : layers) {
            live.emplace_back(layer, OverlayApi::Read(a_actor, layer.node));
        }
        const auto block = Snapshot();
        std::vector<std::pair<OverlayPlan::Layer, const char*>> disagreements;
        const auto verdict = JudgeStore(block, live, &disagreements);
        // The instrument the 21:41 round was missing: WHICH layer, WHICH
        // channel. Filled by the same pass that reached the verdict, so the
        // two cannot describe different stores.
        for (const auto& [layer, why] : disagreements) {
            spdlog::debug("OverlayBaseline: '{}' disagrees with the record: {}.",
                          layer.node, why);
        }
        // ⚠⚠ AGREEMENT AT THE STORE PROVES NOTHING ABOUT THE 3D. Field
        // 2026-09-01 21:41: the rebuilt clones drew Almalexia's black-tinted
        // skeleton art UNTINTED while the store still agreed with the record,
        // because no skin repaint ran on that load's path and so no node push
        // ever put the stored appearance back. Once per load the record is
        // pushed anyway; the write is idempotent against a store that already
        // agrees and it lands on the nodes, which is the half no store-level
        // judge can see.
        bool convergeOwed = false;
        for (int shots = g_convergeShots.load(std::memory_order_acquire);
             shots > 0;) {
            if (g_convergeShots.compare_exchange_weak(shots, shots - 1,
                                                      std::memory_order_acq_rel)) {
                convergeOwed = true;
                break;
            }
        }
        if (verdict == StoreVerdict::kAgrees && !convergeOwed) {
            spdlog::debug("OverlayBaseline: skee's store agrees with the record; "
                          "nothing to do.");
            return 0;
        }
        if (verdict == StoreVerdict::kVacuum && !OverlayApi::HasOverlays(a_actor)) {
            OverlayApi::Install(a_actor);
        }
        std::size_t written = 0;
        std::size_t skipped = 0;
        for (const auto& info : OverlayPlan::kLocations) {
            for (const auto& entry : block.byLocation[OverlayPlan::Slot(info.location)]) {
                // ⚠ A CLAIM OF THE DEFAULT IS NOT A WRITE. An entry whose art
                // is the default texture, however spelled, means "bare"; the
                // clear pass below is how bareness is made, and writing the
                // spelled-out path would bind a variant the engine resolves
                // wrong (a 'textures\' prefix doubles on the way down).
                if (!OccupiedLoose(entry.state)) {
                    continue;
                }
                const OverlayPlan::Layer* slot = nullptr;
                for (const auto& layer : layers) {
                    if (layer.location == info.location && layer.index == entry.index) {
                        slot = &layer;
                        break;
                    }
                }
                if (!slot) {
                    // Recorded on a rig with more layers of this location than
                    // this one has; the counts come from skee64.ini. Same
                    // reasoning as ProfileApply's own overlay step.
                    ++skipped;
                    continue;
                }
                OverlayApi::Write(a_actor, slot->node, entry.state);
                ++written;
            }
        }
        // ⚠⚠ THE DIVERGED STORE'S EXTRAS COME OFF, or the re-assert is additive
        // and the two looks wear each other, which is the exact 2026-08-27
        // shape StepOverlays already cures at apply time. A layer the record
        // does not claim, wearing art, is the stale half of skee's cosave; a
        // layer it claims AS BARE (a default-art entry) comes off the same
        // way. Clear goes through OverlayApi, whose Forget is a no-op for a
        // layer the record never held, so the record survives its own clear
        // pass.
        std::size_t cleared = 0;
        if (verdict == StoreVerdict::kDiverged) {
            for (const auto& [layer, state] : live) {
                if (!OccupiedLoose(state)) {
                    continue;
                }
                const auto* const rec = FindIn(block, layer.location, layer.index);
                if (rec && OccupiedLoose(*rec)) {
                    continue;  // claimed with art: the write above already set it
                }
                OverlayApi::Clear(a_actor, layer.node);
                ++cleared;
            }
        }
        // The settled point ProfileApply's own step uses: skee never paints the
        // first person clones, so they are done here too or the hands come back
        // bare.
        Overlay1P::PaintPlayer(a_actor);
        // ⚠⚠ AND THE MATERIALS ARE ASKED FOR AGAIN AT SETTLE. The writes above
        // each end in their own deferred push, but the 23:45 field round
        // measured what that is worth half a second into a load: skee's cosave
        // replay and the engine's rebuilds repaint the clones AFTER those
        // pushes, so the store ends right and the face draws the pre-push
        // state (a black-tinted layer as a pale one). The coalescing arm fires
        // once the repaints stop, which is the one point in the storm worth
        // owning.
        OverlayApi::ArmNodePropertyPush(a_actor);
        if (verdict == StoreVerdict::kVacuum) {
            spdlog::info(
                "OverlayBaseline: skee held no art after this load, so {} "
                "recorded layer(s) went back on ({} skipped for missing slots).",
                written, skipped);
        } else if (verdict == StoreVerdict::kAgrees) {
            spdlog::info(
                "OverlayBaseline: the store agrees with the record and the "
                "record was pushed once anyway ({} layer(s), {} skipped), so "
                "the drawn state cannot have drifted from what the store "
                "agrees on.",
                written, skipped);
        } else {
            spdlog::info(
                "OverlayBaseline: skee came up holding a DIFFERENT set than "
                "this save's record, so the record went back on: {} written, {} "
                "stale layer(s) cleared, {} skipped for missing slots. The "
                "record follows every author, RaceMenu visits included, so it "
                "is the newest truth at a load boundary.",
                written, cleared, skipped);
        }
        return written;
    }

}  // namespace OS::OverlayBaseline
