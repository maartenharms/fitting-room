#include "PCH.h"

#include "ProfileCapture.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include "BodyPresetStore.h"
#include "FaceWait.h"
#include "BodyWeight.h"
#include "MakeupApi.h"
#include "NodeTransformApi.h"
#include "ObodyApi.h"
#include "OutfitSession.h"
#include "PresetBrowse.h"  // ExportedMeshesDir: the head export's nif half
#include "StyleRef.h"
#include "OverlayApi.h"
#include "OverlayPlan.h"
#include "ProfileCodec.h"
#include "ProfileStore.h"
#include "RaceMenuMorphApi.h"
#include "ShapeOverlay.h"
#include "SkinApi.h"
#include "VmCall.h"  // a static call that refuses instead of dereferencing null

namespace OS::ProfileCapture {

    namespace {

        // The jslot shares RaceMenu's own Exported namespace, so the name is
        // FR_ prefixed and sanitized the way profile filenames are.
        [[nodiscard]] std::string JslotNameFor(const std::string& a_profileName) {
            std::string base;
            for (const char c : a_profileName) {
                const auto uc = static_cast<unsigned char>(c);
                base += (std::isalnum(uc) || c == ' ' || c == '-' || c == '_')
                            ? c
                            : '_';
            }
            while (!base.empty() && base.back() == ' ') base.pop_back();
            if (base.empty()) base = "profile";
            return "FR_" + base;
        }

        [[nodiscard]] std::filesystem::path JslotPathFor(const std::string& a_jslot) {
            return std::filesystem::path("Data/SKSE/Plugins/CharGen/Exported") /
                   (a_jslot + ".jslot");
        }

        // ⚠⚠ THE SAME FACE, WRITTEN A SECOND TIME INTO RACEMENU'S OWN PRESET
        // FOLDER, AND IT IS NOT A DUPLICATE FOR ITS OWN SAKE. The Exported
        // copy is what a look's face block loads through LoadCharacterEx. The
        // Presets copy is what RaceMenu's own browser reads and what FR's
        // presets tab lists, and the field says that route puts the face back
        // correctly when the look's does not (user 2026-08-23: "if I load
        // !UBE_Umbrael_August_2 from racemenu presets it works"). Writing both
        // costs one more VM dispatch at save time and gives the user the
        // working route without a trip through RaceMenu.
        //
        // ⚠ ONE WRITER, TWO DESTINATIONS. Both files come out of skee's own
        // SaveJsonPreset against the same actor in the same frame, so this is
        // not a second reader of the face and nothing here parses either file.
        [[nodiscard]] std::filesystem::path PresetPathFor(const std::string& a_jslot) {
            return std::filesystem::path("Data/SKSE/Plugins/CharGen/Presets") /
                   (a_jslot + ".jslot");
        }

        // ---- the synchronous blocks, read straight off the actor ----------

        [[nodiscard]] ProfileCodec::Profile ReadBlocks(RE::Actor* a_player,
                                                       const std::string& a_name) {
            ProfileCodec::Profile profile;
            profile.name = a_name;

            // Outfit: the active outfit travels whole.
            if (auto outfit =
                    OutfitSession::GetSingleton().ActiveOutfitFor(a_player)) {
                profile.outfit = std::move(*outfit);
            }

            // Body: from what DRIVES the body right now, not from the outfit
            // block (the user's split, 2026-08-22: the body is its own block
            // and applies without the outfit). DisplayBody is the same answer
            // the refresh's body pass paints from, so capture and apply read
            // one source; a rig with neither an outfit body nor a Body Studio
            // custom falls to the live OBody assignment, which is what "the
            // body as it stands" means there. A custom preset id resolves
            // against the store so the profile embeds the payload and stays
            // self-contained on a rig without the store file.
            {
                ProfileCodec::BodyBlock body;
                const auto display = OutfitSession::GetSingleton().DisplayBody();
                if (!display.customPresetId.empty()) {
                    if (auto preset = BodyPresetStore::GetSingleton().Find(
                            display.customPresetId)) {
                        body.custom = std::move(*preset);
                    } else {
                        spdlog::warn(
                            "ProfileCapture: the body names custom preset "
                            "'{}' and the store has no such file; the body "
                            "block keeps only the OBody name.",
                            display.customPresetId);
                        body.obodyPreset = display.preset;
                    }
                } else if (!display.preset.empty()) {
                    body.obodyPreset = display.preset;
                } else {
                    body.obodyPreset = ObodyApi::AssignedPreset(a_player);
                }
                if (!body.obodyPreset.empty() || body.custom) {
                    profile.body = std::move(body);
                }
            }

            // Who this look was captured on. Always written: a library of
            // looks has to know what each one assumes, and the apply side
            // reads it to say when the character differs.
            {
                ProfileCodec::CharacterBlock character;
                auto* const base = a_player->GetActorBase();
                if (auto* const race = a_player->GetRace();
                    race && StyleRef::Make(race, character.race)) {
                    character.female = base && base->IsFemale();
                    profile.character = character;
                }
            }

            // Overlays: every layer that carries something of ours. An empty
            // LayerState is a slot nobody wrote, and capturing it would make
            // the apply write emptiness over another save's art.
            if (OverlayApi::Available()) {
                ProfileCodec::OverlaysBlock overlays;
                bool any = false;
                for (const auto& layer : OverlayApi::Layers()) {
                    auto state = OverlayApi::Read(a_player, layer.node);
                    const bool occupied =
                        state.hasTexture || state.hasNormal || state.hasTint ||
                        state.hasAlpha || state.hasFinish ||
                        state.glowStrength != 0.0f ||
                        !OverlayTransform::IsIdentity(state.transform);
                    if (!occupied) continue;
                    overlays.byLocation[OverlayPlan::Slot(layer.location)]
                        .push_back(ProfileCodec::OverlayEntry{ layer.index,
                                                               std::move(state) });
                    any = true;
                }
                if (any) profile.overlays = std::move(overlays);
            }

            // Makeup: occupied layers only (strength zero is a layer that is
            // off, not a layer that is black).
            {
                const auto layers   = MakeupApi::Layers(a_player);
                const auto snapshot = MakeupApi::Read(a_player);
                std::vector<ProfileCodec::MakeupEntry> makeup;
                for (std::size_t i = 0;
                     i < layers.size() && i < snapshot.size(); ++i) {
                    if (!MakeupPlan::Occupied(snapshot[i])) continue;
                    makeup.push_back(ProfileCodec::MakeupEntry{
                        layers[i].index, layers[i].type, snapshot[i] });
                }
                if (!makeup.empty()) profile.makeup = std::move(makeup);
            }

            // Shape: the owned morph keys and the proportion controls.
            if (RaceMenuMorphApi::Available()) {
                ProfileCodec::ShapeBlock shape;
                for (const auto& mv :
                     RaceMenuMorphApi::SnapshotShape(a_player)) {
                    shape.morphs.emplace(mv.name, mv.value);
                }
                if (NodeTransformApi::Available()) {
                    const auto readings = NodeTransformApi::Read(a_player);
                    for (std::size_t i = 0; i < ShapeOverlay::kSliderCount;
                         ++i) {
                        const auto& slider = ShapeOverlay::kSliders[i];
                        for (const auto& reading : readings) {
                            if (reading.present &&
                                slider.bones[0] &&
                                reading.bone == slider.bones[0]) {
                                shape.scales.emplace(slider.id, reading.scale);
                                break;
                            }
                        }
                    }
                }
                if (!shape.morphs.empty() || !shape.scales.empty()) {
                    profile.shape = std::move(shape);
                }
            }

            // Always, including the empty pack: "wears no pack" is a value,
            // and applying it takes a pack off (user 2026-08-22, "capture the
            // skin even if it's default").
            profile.skin =
                ProfileCodec::SkinBlock{ SkinApi::Current(a_player) };

            profile.weight = BodyWeight::Of(a_player);
            return profile;
        }

        void SaveProfile(ProfileCodec::Profile a_profile) {
            std::string error;
            if (!ProfileStore::GetSingleton().Save(a_profile, error)) {
                spdlog::warn("ProfileCapture: '{}' not saved: {}",
                             a_profile.name, error);
                return;
            }
            spdlog::info(
                "ProfileCapture: '{}' saved (face={}, outfit={}, overlays={}, "
                "makeup={}, body={}, shape={}, skin={}, weight={}).",
                a_profile.name, a_profile.face.has_value(),
                a_profile.outfit.has_value(), a_profile.overlays.has_value(),
                a_profile.makeup.has_value(), a_profile.body.has_value(),
                a_profile.shape.has_value(), a_profile.skin.has_value(),
                a_profile.weight.has_value());
        }

        // ---- the face half, answer + file or no face block ----------------

        // The pending set is what the page's "still saving" reads; the
        // tainted set is every name whose wait a save load abandoned, so a
        // jslot that lands on the wrong side of the load is never healed in
        // this session; the generation is bumped by OnRevert and each
        // watcher carries the value it armed under.
        std::mutex                 g_faceLock;
        std::set<std::string>      g_facePending;
        std::set<std::string>      g_faceTainted;
        std::atomic<std::uint32_t> g_loadGeneration{ 0 };

        void SetFacePending(const std::string& a_name, bool a_pending) {
            std::scoped_lock l(g_faceLock);
            if (a_pending) {
                g_facePending.insert(a_name);
            } else {
                g_facePending.erase(a_name);
            }
        }

        // Per capture, not global: a second Save can start while an earlier
        // face is still settling (the press unblocks as soon as the profile
        // lands), and two dispatches must not share one answered flag. The
        // VM holds the functor until it answers, the watcher holds the other
        // reference, and whichever finishes last frees it.
        class SaveAnswer : public RE::BSScript::IStackCallbackFunctor {
        public:
            explicit SaveAnswer(std::shared_ptr<std::atomic<bool>> a_flag)
                : flag_(std::move(a_flag)) {}
            void operator()(RE::BSScript::Variable) override {
                flag_->store(true);
            }
            bool CanSave() const override { return false; }
            void SetObject(const RE::BSTSmartPointer<RE::BSScript::Object>&) override {}

        private:
            std::shared_ptr<std::atomic<bool>> flag_;
        };

        // The face arrives AFTER the profile: patch the stored file rather
        // than holding the whole save hostage to the VM. Re-found by name at
        // patch time because anything can have happened to the file since -
        // renamed (the patch misses, one line), deleted (same), or already
        // carrying a face from a faster capture under the same name (left
        // alone).
        // Returns whether a block was actually added, so the heal can say
        // how many it touched and the page can be told to re-read.
        bool PatchFaceIn(const std::string& a_name, std::string a_jslot,
                         bool a_healed = false) {
            auto& store = ProfileStore::GetSingleton();
            store.Load();
            auto entry = store.Find(a_name);
            if (!entry) {
                spdlog::warn("ProfileCapture: '{}' is no longer in the store; "
                             "its face preset '{}' stays in CharGen's folder "
                             "unreferenced.", a_name, a_jslot);
                return false;
            }
            if (entry->profile.face) {
                return false;
            }
            // ⚠⚠ READ THE COLOUR OURSELVES, AND SAY SO WHEN THERE IS NONE.
            // skee only writes its own `hairColor` key when the base holds a
            // colour form at save time, so a capture taken inside the window
            // where the base reads (none) produces a jslot that applies BLACK
            // and never says why. Recording it here gives the look a colour
            // that does not depend on skee's key, and the warning below is the
            // one chance the user gets to notice before the file is written.
            std::optional<HairTint> captured;
            if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                auto* const base = player->GetActorBase();
                auto* const hrd  = base ? base->headRelatedData : nullptr;
                if (auto* const colour = hrd ? hrd->hairColor : nullptr) {
                    captured = HairTint{ true, colour->color.red,
                                         colour->color.green,
                                         colour->color.blue };
                    spdlog::info("ProfileCapture: '{}' carries hair colour "
                                 "({},{},{}) from form {:08X}, so applying it "
                                 "does not depend on the jslot's own key.",
                                 a_name, colour->color.red, colour->color.green,
                                 colour->color.blue, colour->GetFormID());
                } else {
                    // ⚠⚠ TWO CAUSES, AND ONLY ONE OF THEM IS WAITABLE. A
                    // character mid-load reads (none) for up to ~70 s and then
                    // fills in (r88). A character coc'd from the main menu
                    // skipped chargen altogether, so nothing ever writes the
                    // form and waiting achieves nothing: opening RaceMenu once
                    // is what creates it. Nord 1 and Nord 2 on the dev rig were
                    // both coc'd defaults and captured colourless; Nord 3 had
                    // been through RaceMenu and captured (56,59,44) first try.
                    // Saying only "wait" would send half the field the wrong
                    // way. See coc-from-main-menu-skips-newgame.
                    spdlog::warn("ProfileCapture: '{}' was captured while the "
                                 "actor base carried NO hair colour form, so "
                                 "neither this look nor its jslot states one "
                                 "and applying it will paint black. Either the "
                                 "load has not filled the base in yet, in which "
                                 "case recapturing in a minute fixes it, or this "
                                 "character came from a coc and never went "
                                 "through chargen, in which case the form does "
                                 "not exist until RaceMenu has been opened once.",
                                 a_name);
                }
            }
            entry->profile.face = ProfileCodec::FaceBlock{
                std::move(a_jslot), ProfileCodec::FaceSource::kCaptured, 0,
                ProfileCodec::FaceFolder::kExported, captured
            };
            std::string error;
            if (ProfileStore::GetSingleton().Save(entry->profile, error)) {
                if (a_healed) {
                    spdlog::info("ProfileCapture: face block added to '{}' "
                                 "from the jslot already on disk.", a_name);
                } else {
                    spdlog::info("ProfileCapture: face block added to '{}' "
                                 "after SaveCharacter settled.", a_name);
                }
                return true;
            }
            spdlog::warn("ProfileCapture: face block for '{}' not saved: "
                         "{}", a_name, error);
            return false;
        }

        // The wait lives on its own thread that SLEEPS between checks, never
        // on the task queue. ⚠⚠ THE OLD TASK-REQUEUE WAIT IS THE 2026-08-22
        // 01:25 FREEZE: the SKSE drain runs until its queue is empty, so a
        // task re-added from inside a task runs in the SAME drain (measured
        // 2026-07-31, WorldWatch.cpp's heartbeat scar; FaceWait.h carries the
        // full account), and the pause-aware no-increment turned that spin
        // from the old build's 8-second hang into a hang with no exit. This
        // thread hands the queue single, non-requeueing tasks only.
        //
        // WorldWatch's heartbeat idiom: detached, short slices, and the loop
        // owns its own exit (landed or budget spent), so teardown never waits
        // on it longer than one slice.
        void StartFaceWatcher(std::string a_name, std::string a_jslot,
                              std::shared_ptr<std::atomic<bool>> a_answered) {
            SetFacePending(a_name, true);
            try {
                // Captured by copy, not move: if the thread itself fails to
                // start, the catch below still owns the names it logs.
                std::thread([name = a_name, jslot = a_jslot,
                             answered = std::move(a_answered),
                             gen = g_loadGeneration.load()] {
                    constexpr auto   kSlice = std::chrono::milliseconds(250);
                    constexpr double kSliceSeconds = 0.25;
                    FaceWait::Budget budget;
                    double           lastNote = 0.0;
                    for (;;) {
                        std::this_thread::sleep_for(kSlice);
                        // ⚠⚠ A SAVE LOAD ABANDONS THE WAIT, before anything
                        // else is read: the dispatch was aimed at the
                        // character being torn down, and a SaveCharacter that
                        // settles on the other side of the load captures
                        // whoever the player is THEN. Patching that in would
                        // put a stranger's head on the look, silently. The
                        // name is tainted so a late-landing jslot is not
                        // healed from disk this session either; a fresh save
                        // of the look clears it.
                        if (g_loadGeneration.load() != gen) {
                            {
                                std::scoped_lock l(g_faceLock);
                                g_facePending.erase(name);
                                g_faceTainted.insert(name);
                            }
                            spdlog::warn(
                                "ProfileCapture: the save changed while "
                                "SaveCharacter '{}' was settling; '{}' stays "
                                "without a face block (save the look again "
                                "on its own character).",
                                jslot, name);
                            return;
                        }
                        std::error_code ec;
                        const bool      file =
                            std::filesystem::exists(JslotPathFor(jslot), ec);
                        // Off-thread read of a plain pause counter: worst
                        // case is one slice charged or spared wrongly, and
                        // the budget is thirty seconds of them.
                        auto* const ui     = RE::UI::GetSingleton();
                        const bool  paused = ui && ui->GameIsPaused();
                        const auto  step   = FaceWait::Advance(
                            budget, answered->load(), file, paused,
                            kSliceSeconds);
                        if (step == FaceWait::Step::kPatch) {
                            SetFacePending(name, false);
                            if (auto* tasks = SKSE::GetTaskInterface()) {
                                tasks->AddTask([name, jslot]() {
                                    PatchFaceIn(name, jslot);
                                });
                            }
                            return;
                        }
                        if (step == FaceWait::Step::kGiveUp) {
                            SetFacePending(name, false);
                            spdlog::warn(
                                "ProfileCapture: SaveCharacter for '{}' never "
                                "settled ({} s unpaused; answer {}, file {}); "
                                "'{}' stays without a face block.",
                                jslot, FaceWait::kBudgetSeconds,
                                answered->load() ? "arrived" : "pending",
                                file ? "present" : "missing", name);
                            return;
                        }
                        // A liveness line roughly every ten unpaused seconds:
                        // a healthy watcher prints a few of these and stops,
                        // and their spacing in the field log is the proof this
                        // wait paces itself instead of spinning a drain.
                        if (budget.unpausedSeconds - lastNote >= 10.0) {
                            lastNote = budget.unpausedSeconds;
                            spdlog::info(
                                "ProfileCapture: still waiting on "
                                "SaveCharacter '{}' ({:.0f} s unpaused; "
                                "answer {}, file {}).",
                                jslot, budget.unpausedSeconds,
                                answered->load() ? "arrived" : "pending",
                                file ? "present" : "missing");
                        }
                    }
                }).detach();
            } catch (const std::exception& e) {
                // Thread construction can fail under resource exhaustion
                // (WorldWatch review finding 1). The dispatch already ran;
                // only the patch-in goes missing, and it says so.
                SetFacePending(a_name, false);
                spdlog::error(
                    "ProfileCapture: could not start the face watcher for "
                    "'{}' ({}); '{}' stays without a face block.",
                    a_jslot, e.what(), a_name);
            }
        }

        // One export at a time; the page reads this to disable the button.
        std::atomic<bool> g_exportPending{ false };

    }  // namespace

    void ExportHeadTrio(const std::string& a_name) {
        if (a_name.empty() ||
            g_exportPending.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        auto work = [name = a_name]() {
            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                spdlog::warn("ProfileCapture: no Papyrus VM; the head export "
                             "'{}' cannot run.", name);
                g_exportPending.store(false, std::memory_order_release);
                return;
            }
            {
                // The stale-file rule the capture path already follows: a
                // leftover trio under this name would read as "landed"
                // before this dispatch ran.
                std::error_code ec;
                std::filesystem::remove(JslotPathFor(name), ec);
                std::filesystem::remove(
                    PresetBrowse::ExportedMeshesDir() / (name + ".nif"), ec);
            }
            // ⚠⚠ BEFORE THE DISPATCH, NOT AFTER: skee opens its three files
            // and never makes the folders they live in, so a missing folder
            // is a write that goes nowhere quietly. This is the whole reason
            // the mesh half of the export had never landed on the reference
            // rig (PresetBrowse.h carries the measurement).
            if (!PresetBrowse::EnsureExportDirs()) {
                spdlog::warn("ProfileCapture: could not make the CharGen "
                             "export folders, so '{}' may land in pieces.",
                             name);
            }
            auto answered = std::make_shared<std::atomic<bool>>(false);
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{
                new SaveAnswer(answered)
            };
            auto* args = RE::MakeFunctionArguments(RE::BSFixedString(name));
            if (!VmCall::Static(vm, "CharGen", "SaveExternalCharacter",
                                args, cb)) {
                spdlog::warn("ProfileCapture: CharGen.SaveExternalCharacter "
                             "refused for '{}'.", name);
                g_exportPending.store(false, std::memory_order_release);
                return;
            }
            spdlog::info("ProfileCapture: SaveExternalCharacter '{}' "
                         "dispatched; it settles when the VM runs (with the "
                         "editor open, after it closes).", name);
            try {
                std::thread([name, answered = std::move(answered),
                             gen = g_loadGeneration.load()] {
                    constexpr auto   kSlice = std::chrono::milliseconds(250);
                    constexpr double kSliceSeconds = 0.25;
                    FaceWait::Budget budget;
                    double           lastNote = 0.0;
                    for (;;) {
                        std::this_thread::sleep_for(kSlice);
                        if (g_loadGeneration.load() != gen) {
                            g_exportPending.store(false,
                                                  std::memory_order_release);
                            spdlog::warn(
                                "ProfileCapture: the save changed while the "
                                "head export '{}' was settling; whatever "
                                "landed describes the OLD character.", name);
                            return;
                        }
                        std::error_code ec;
                        const bool      jslot =
                            std::filesystem::exists(JslotPathFor(name), ec);
                        auto* const ui     = RE::UI::GetSingleton();
                        const bool  paused = ui && ui->GameIsPaused();
                        const auto  step   = FaceWait::Advance(
                            budget, answered->load(), jslot, paused,
                            kSliceSeconds);
                        if (step == FaceWait::Step::kPatch) {
                            // ⚠ BOTH HALVES BY NAME (a-verify-that-asks-one-
                            // question-calls-half-a-failure-healthy): the
                            // jslot is skee's synchronous half, the nif rides
                            // the task it queues, so the mesh gets its own
                            // dwell rather than one read on the slice the
                            // jslot appeared (FaceWait::kMeshBudgetSeconds).
                            const auto meshPath =
                                PresetBrowse::ExportedMeshesDir() /
                                (name + ".nif");
                            FaceWait::Budget mesh;
                            bool             nif = false;
                            for (;;) {
                                nif = std::filesystem::exists(meshPath, ec);
                                auto* const ui2 = RE::UI::GetSingleton();
                                if (FaceWait::Advance(
                                        mesh, true, nif,
                                        ui2 && ui2->GameIsPaused(),
                                        kSliceSeconds,
                                        FaceWait::kMeshBudgetSeconds) !=
                                    FaceWait::Step::kWait) {
                                    break;
                                }
                                std::this_thread::sleep_for(kSlice);
                            }
                            g_exportPending.store(false,
                                                  std::memory_order_release);
                            spdlog::info(
                                "ProfileCapture: head export '{}' landed: "
                                "jslot present, head nif {}.",
                                name,
                                nif ? "present"
                                    : "ABSENT after the mesh dwell (its "
                                      "folder could not be made, or "
                                      "RaceMenu's bEnableHeadExport is off; "
                                      "the jslot alone still loads)");
                            return;
                        }
                        if (step == FaceWait::Step::kGiveUp) {
                            g_exportPending.store(false,
                                                  std::memory_order_release);
                            spdlog::warn(
                                "ProfileCapture: SaveExternalCharacter '{}' "
                                "never settled ({} s unpaused; answer {}, "
                                "jslot {}).",
                                name, FaceWait::kBudgetSeconds,
                                answered->load() ? "arrived" : "pending",
                                jslot ? "present" : "missing");
                            return;
                        }
                        if (budget.unpausedSeconds - lastNote >= 10.0) {
                            lastNote = budget.unpausedSeconds;
                            spdlog::info(
                                "ProfileCapture: still waiting on the head "
                                "export '{}' ({:.0f} s unpaused).",
                                name, budget.unpausedSeconds);
                        }
                    }
                }).detach();
            } catch (const std::exception& e) {
                g_exportPending.store(false, std::memory_order_release);
                spdlog::error("ProfileCapture: could not start the export "
                              "watcher for '{}' ({}); read the folder by "
                              "hand.", name, e.what());
            }
        };
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask(std::move(work));
        } else {
            work();
        }
    }

    bool ExportPending() {
        return g_exportPending.load(std::memory_order_acquire);
    }

    ProfileCodec::Profile SnapshotBlocks(RE::Actor* a_player,
                                         const std::string& a_name) {
        if (!a_player) {
            ProfileCodec::Profile empty;
            empty.name = a_name;
            return empty;
        }
        return ReadBlocks(a_player, a_name);
    }

    void Capture(RE::Actor* a_player, const std::string& a_name,
                 bool a_captureFace) {
        if (!a_player) {
            return;
        }
        // Actor reads stay on the calling thread, the way every page reads
        // the live actor; they were correct in the freeze session's profile.
        auto profile = ReadBlocks(a_player, a_name);

        // ⚠ EVERYTHING PAST THE READS RUNS ON A GAME-THREAD TASK, NOT ON THE
        // PRESS. The profile still saves first, faceless, and the face still
        // patches in when SaveCharacter really settles - but the button press
        // no longer does file IO or VM dispatch on the present thread. The
        // save's rename-and-rescan, the stale-jslot remove and the dispatch
        // ride one task; the page's poll picks the profile up the same way it
        // always has.
        auto work = [profile = std::move(profile), name = a_name,
                     captureFace = a_captureFace]() mutable {
            SaveProfile(profile);

            if (!captureFace) {
                return;
            }

            auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
            if (!vm) {
                spdlog::warn("ProfileCapture: no Papyrus VM; '{}' stays "
                             "without a face block.", name);
                return;
            }
            auto jslot = JslotNameFor(name);
            {
                // A leftover file under this name would make "exists" true
                // before this capture's save ran, the same stale-YES the
                // probe closed.
                std::error_code ec;
                std::filesystem::remove(JslotPathFor(jslot), ec);
            }
            {
                // A fresh save of the look is the cure for an abandoned
                // wait, so it clears the taint.
                std::scoped_lock l(g_faceLock);
                g_faceTainted.erase(name);
            }
            auto answered = std::make_shared<std::atomic<bool>>(false);
            RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> cb{
                new SaveAnswer(answered)
            };
            auto* args = RE::MakeFunctionArguments(RE::BSFixedString(jslot));
            if (!VmCall::Static(vm, "CharGen", "SaveCharacter", args,
                                cb)) {
                spdlog::warn("ProfileCapture: CharGen.SaveCharacter refused; "
                             "'{}' stays without a face block.", name);
                return;
            }
            spdlog::info("ProfileCapture: SaveCharacter '{}' dispatched for "
                         "'{}'.", jslot, name);
            // The RaceMenu-side copy, dispatched in the same frame against the
            // same actor. ⚠ NOT WATCHED and never gating the look: the face
            // block rides the Exported copy, and a rig where this refuses
            // still gets everything it got before.
            if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                std::error_code ec;
                std::filesystem::remove(PresetPathFor(jslot), ec);
                auto* const presetArgs = RE::MakeFunctionArguments(
                    static_cast<RE::Actor*>(player), RE::BSFixedString(jslot));
                RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCb{};
                if (VmCall::Static(vm, "CharGen", "SaveCharacterPreset",
                                   presetArgs, noCb)) {
                    spdlog::info("ProfileCapture: SaveCharacterPreset '{}' "
                                 "dispatched; the look is a RaceMenu preset "
                                 "too.", jslot);
                } else {
                    spdlog::warn("ProfileCapture: CharGen.SaveCharacterPreset "
                                 "refused for '{}'; the look still has its "
                                 "Exported face.", jslot);
                }
            }
            StartFaceWatcher(name, std::move(jslot), std::move(answered));
        };
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask(std::move(work));
        } else {
            // No task interface is a load-order accident this plugin has
            // never seen; running inline is exactly the deployed behaviour
            // and only costs the press thread what it already paid.
            work();
        }
    }

    bool FacePending(const std::string& a_name) {
        std::scoped_lock l(g_faceLock);
        return g_facePending.contains(a_name);
    }

    void OnRevert() {
        // The watchers notice the bump on their next slice and abandon; the
        // taint is written THERE, by the thread that owns the wait, so a
        // watcher that already patched cannot be tainted retroactively.
        g_loadGeneration.fetch_add(1);
    }

    void HealFacelessFromDisk(void (*a_onHealed)()) {
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            return;
        }
        tasks->AddTask([a_onHealed]() {
            auto& store = ProfileStore::GetSingleton();
            store.Load();
            std::size_t healed = 0;
            for (const auto& entry : store.Snapshot()) {
                if (entry.profile.face) {
                    continue;
                }
                const auto name  = entry.profile.name;
                const auto jslot = JslotNameFor(name);
                {
                    std::scoped_lock l(g_faceLock);
                    if (g_facePending.contains(name) ||
                        g_faceTainted.contains(name)) {
                        continue;
                    }
                }
                std::error_code ec;
                if (!std::filesystem::exists(JslotPathFor(jslot), ec)) {
                    continue;
                }
                if (PatchFaceIn(name, jslot, /*a_healed=*/true)) {
                    ++healed;
                }
            }
            if (healed != 0 && a_onHealed) {
                a_onHealed();
            }
        });
    }

}  // namespace OS::ProfileCapture
