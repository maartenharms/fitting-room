#include "Requip.h"

#include "OutfitDye.h"
#include "RequipFlourish.h"
#include "Settings.h"

#include <atomic>
#include <chrono>
#include <cstdio>   // sscanf, for the INI colour
#include <cstdlib>  // strtoul, for the INI form ids
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace OS::Requip {

    namespace {
        using Clock = std::chrono::steady_clock;
        using RequipFlourish::Phase;
        using RequipFlourish::Teardown;

        std::mutex        g_lock;
        Phase             g_phase{ Phase::kIdle };
        Clock::time_point g_phaseStart{};
        std::uint32_t     g_waitFrames{ 0 };
        RE::FormID        g_actorId{ 0 };
        std::size_t       g_armedShapes{ 0 };

        // ⚠ THE MASK IS ITS OWN FIELD AND IT IS NOT THE COMMIT'S BUSINESS.
        // Condense has to arm the SAME slots a second time, on the geometry the
        // rebuild produced, so the mask has to outlive the commit that consumed
        // it.
        std::uint32_t         g_slotMask{ 0 };
        std::function<void()> g_commit;

        // Read without the lock from Tick's first line, so it must be atomic.
        std::atomic<bool> g_sessionLive{ false };

        // The Seamstone violet, parsed from the INI once per flourish.
        RE::NiColor g_peak{ 0.59f, 0.35f, 0.86f };

        // ---- the aura, and why it is allowed to be absent -----------------
        //
        // ⚠⚠ THESE TWO IDS ARE PROVISIONAL AND THE RECORDS DO NOT EXIST YET.
        // 0x806 and 0x807 are the next free pair after LoreModule's 0x800..0x805
        // (see its "never reuse" note). They must be corrected to whatever the
        // Creation Kit actually assigns when the records are authored, read out
        // of the saved ESP rather than assumed.
        constexpr const char* kEsp      = "FittingRoomLore.esp";
        constexpr RE::FormID  kArtID    = 0x806;  // ARTO the ground plume and body aura
        constexpr RE::FormID  kShaderID = 0x807;  // EFSH the membrane pass over the actor

        // ⚠ A LIST, BECAUSE ART OBJECTS STACK AND THAT IS THE POINT. Each one is
        // its own NIF with its own particle system, so depth comes from playing
        // three cheap records together rather than from finding one record that
        // does everything.
        std::vector<RE::BGSArtObject*> g_art;
        // Played per changed garment, on the garment's own node.
        std::vector<RE::BGSArtObject*> g_slotArt;
        RE::TESEffectShader*           g_shader{ nullptr };

        // ⚠⚠ NOT EditorStyle::PlayUISound, AND THE DIFFERENCE IS WHY THE CUE WAS
        // INAUDIBLE. That helper is for interface clicks, which are 2D and play
        // wherever the listener is. A MAG descriptor is a world sound: built and
        // played without a position it sits at the world origin, which is
        // usually a long way from the player. The log said it resolved and
        // played, and it was correct on both counts.
        //
        // ⚠ THE HANDLE IS CHECKED BEFORE IT IS TOUCHED. Every crash on this
        // feature came from writing through something that was never validated,
        // so IsValid gates the position and the play, and a handle that failed
        // to build is a log line rather than a call.
        void PlayCue(RE::Actor* a_actor, const std::string& a_editorID) {
            if (!a_actor || a_editorID.empty()) {
                return;
            }
            auto* const am = RE::BSAudioManager::GetSingleton();
            if (!am) {
                return;
            }
            RE::BSSoundHandle handle;
            am->BuildSoundDataFromEditorID(handle, a_editorID.c_str(), 0x10);
            if (!handle.IsValid()) {
                static std::set<std::string> reported;
                if (reported.insert(a_editorID).second) {
                    spdlog::warn("Requip: no sound descriptor named '{}' in this load "
                                 "order, so the cue is silent (OS-206).", a_editorID);
                }
                return;
            }
            // ⚠ A POSITION RATHER THAN A NODE TO FOLLOW. Following would want a
            // scenegraph pointer, and the thing this whole feature waits on is
            // that scenegraph being torn down and rebuilt. A cue this short does
            // not need to track the actor, and a NiPoint3 cannot dangle.
            handle.SetPosition(a_actor->GetPosition());
            handle.Play();
        }

        // Parse "Plugin.esm|0xFORMID" and look it up. Anything malformed logs
        // and returns nullptr, so a typo in the INI costs the aura rather than
        // the outfit swap.
        template <class T>
        [[nodiscard]] T* LookupSpec(RE::TESDataHandler* a_dh, const std::string& a_spec,
                                    const char* a_key) {
            if (a_spec.empty()) {
                return nullptr;  // not set; the ESP records are the default
            }
            const auto bar = a_spec.find('|');
            if (bar == std::string::npos || bar == 0 || bar + 1 >= a_spec.size()) {
                spdlog::warn("Requip: {} = '{}' is not Plugin.esm|0xFORMID (OS-206).", a_key,
                             a_spec);
                return nullptr;
            }
            const std::string plugin = a_spec.substr(0, bar);
            const std::string idText = a_spec.substr(bar + 1);

            char*             stop = nullptr;
            const unsigned long id = std::strtoul(idText.c_str(), &stop, 16);
            if (stop == idText.c_str() || id == 0) {
                spdlog::warn("Requip: {} = '{}' has no readable form id (OS-206).", a_key,
                             a_spec);
                return nullptr;
            }

            // ⚠ MASKED TO THE LOCAL ID. A form id written down anywhere carries
            // the plugin's LOAD ORDER INDEX in its top byte, and that byte is a
            // property of one load order rather than of the record. LookupForm
            // wants the id WITHIN the plugin, so the copied-from-xEdit form and
            // the same record on a different machine both resolve.
            auto* const form =
                a_dh->LookupForm<T>(static_cast<RE::FormID>(id & 0x00FFFFFFu), plugin);
            if (!form) {
                spdlog::warn("Requip: {} = '{}' did not resolve. Wrong plugin name, wrong id, "
                             "or the wrong record type for this slot (OS-206).", a_key, a_spec);
            }
            return form;
        }

        // Split a comma separated list of specs and resolve each one. A bad
        // entry is dropped with a line of its own rather than killing the rest,
        // because the whole point of the key is trying several at once.
        template <class T>
        [[nodiscard]] std::vector<T*> LookupList(RE::TESDataHandler* a_dh,
                                                 const std::string& a_spec, const char* a_key) {
            std::vector<T*> out;
            std::size_t     at = 0;
            while (at <= a_spec.size()) {
                const auto comma = a_spec.find(',', at);
                auto       piece = a_spec.substr(
                    at, comma == std::string::npos ? std::string::npos : comma - at);

                const auto first = piece.find_first_not_of(" \t");
                const auto last  = piece.find_last_not_of(" \t");
                if (first != std::string::npos) {
                    piece = piece.substr(first, last - first + 1);
                    if (auto* const form = LookupSpec<T>(a_dh, piece, a_key)) {
                        out.push_back(form);
                    }
                }
                if (comma == std::string::npos) {
                    break;
                }
                at = comma + 1;
            }
            return out;
        }

        // ⚠ AN ABSENT FORM DEGRADES, IT DOES NOT REFUSE. The garment animation
        // is the feature and the aura is what hides the cut; a player whose ESP
        // failed to load, or a build running before the records are authored,
        // gets a quieter flourish and a log line naming what was missing rather
        // than an outfit swap that silently stopped working.
        void PlayAura(RE::Actor* a_actor, std::uint32_t a_slotMask) {
            if (!a_actor) {
                return;
            }
            const auto dur = static_cast<float>(RequipFlourish::kBurnSeconds +
                                                RequipFlourish::kCondenseSeconds);
            // ⚠⚠ THE RETURN VALUE OF InstantiateHitShader IS NOT USABLE HERE,
            // MEASURED TWICE BY CRASHING THE GAME. The header types it as
            // ShaderReferenceEffect*, and on this runtime the call came back
            // with 1 in rbx: not a pointer, and truthy, so every `if (fx)` guard
            // waves it through and the first field read off it is an access
            // violation. The second crash was the GUARD for the first one, at
            // `cmp dword ptr [rbx+0C0h]`.
            //
            // ⚠ SO NOTHING MAY DEREFERENCE IT. The call itself is fine and the
            // shader plays; only the thing it hands back is a lie. If a later
            // stint needs the effect object, it has to come from somewhere the
            // engine actually populates, and that has to be proven before it is
            // read.
            //
            // ⚠ WHICH LEAVES ONE HONEST LEVER OVER THE SOUND: play the record
            // or do not. The audio and the visuals are one borrowed record and
            // cannot be separated until FR_RequipShader exists, so the switch
            // takes both or neither and the tooltip says so rather than
            // promising a silence it cannot deliver.
            const auto& settings = Settings::GetSingleton();
            // ⚠ OUR OWN CUE, AND IT ADDS RATHER THAN REPLACES. The armour
            // clatter under it is the engine's own equip audio, fired by the
            // swap itself and not ours to remove. A conjuration sound over it
            // reads as magic; the clatter on its own reads as rummaging.
            if (settings.requipSoundOn) {
                PlayCue(a_actor, settings.requipSoundId);
            }
            // ⚠⚠ ONE SWITCH OVER BOTH HALVES, AND IT ONLY COVERED THE SHADER
            // UNTIL 2026-08-15. The checkbox says "magic effect on the body" and
            // the art object is half of that effect, so unticking it left a NIF
            // playing on the character and made the box a liar. It cost a field
            // A/B: the tester turned the effect off, still heard a sound, and
            // the honest reading of that was "not the shader" when the truth was
            // "half of it was still running". A control that silences part of
            // what it names cannot be used to rule anything out.
            if (settings.requipAura) {
                if (g_shader) {
                    a_actor->InstantiateHitShader(g_shader, dur);
                }
                for (auto* const art : g_art) {
                    a_actor->InstantiateHitArt(art, dur, nullptr, false, false);
                }
            }

            if (!settings.requipAura || g_slotArt.empty()) {
                return;
            }
            // ⚠⚠ THE NODES ARE READ HERE AND NOWHERE ELSE, AND NEVER STORED.
            // These are the partClones the rebuild is about to destroy, which is
            // the whole reason the effect reads as the garment leaving rather
            // than the actor glowing. Handing one of them to anything that
            // outlives this call is a pointer into freed memory a few frames
            // from now. The engine takes its own reference when it attaches.
            const auto nodes = OutfitDye::RequipSlotNodes(a_actor, a_slotMask);
            for (auto* const node : nodes) {
                for (auto* const art : g_slotArt) {
                    a_actor->InstantiateHitArt(art, dur, nullptr, false, false, node);
                }
            }
            spdlog::debug("Requip: aura on {} actor art, {} garment node(s) x {} art (OS-206).",
                          g_art.size(), nodes.size(), g_slotArt.size());
        }

        [[nodiscard]] double ElapsedLocked() {
            const double raw =
                std::chrono::duration<double>(Clock::now() - g_phaseStart).count();
            // ⚠ THE MULTIPLIER SCALES THE CLOCK, NOT THE SPANS. The curves own
            // their own durations and are constexpr; stretching elapsed time is
            // the only way to retune speed without a rebuild. Settings clamps
            // this to 0.25..4.0 on read, so it can never be zero here.
            return raw * static_cast<double>(Settings::GetSingleton().requipSpeed);
        }

        void EnterLocked(Phase a_phase) {
            g_phase      = a_phase;
            g_phaseStart = Clock::now();
            g_waitFrames = 0;
        }

        // ⚠ THE ONLY FUNCTION ALLOWED TO PUT ORIGINALS BACK, and every exit
        // goes through it. A second path that cleared g_phase without calling
        // OutfitDye::EndRequip would leave a garment at the peak with nothing
        // left that knows how to restore it.
        void TeardownLocked(Teardown a_why) {
            OutfitDye::EndRequip();
            g_phase       = Phase::kIdle;
            g_armedShapes = 0;
            g_actorId     = 0;
            g_slotMask    = 0;
            g_commit      = nullptr;
            g_waitFrames  = 0;
            spdlog::info("Requip: ended, reason {} (OS-206).", static_cast<int>(a_why));
        }

        // ⚠ PARSED ONCE PER FLOURISH RATHER THAN PER FRAME. It comes from an INI
        // string, and doing it in the pulse would parse it sixty times a second
        // for the whole burn.
        void ParsePeakColour() {
            const auto& s = Settings::GetSingleton().requipColour;
            int         r = 150, g = 90, b = 220;
            // A malformed string leaves the defaults above in place rather than
            // producing a black flourish nobody can see.
            if (std::sscanf(s.c_str(), "%d,%d,%d", &r, &g, &b) != 3) {
                r = 150;
                g = 90;
                b = 220;
            }
            const auto chan = [](int v) {
                const int c = v < 0 ? 0 : (v > 255 ? 255 : v);
                return static_cast<float>(c) / 255.0f;
            };
            g_peak = RE::NiColor{ chan(r), chan(g), chan(b) };
        }
    }  // namespace

    void Begin(RE::Actor* a_actor, std::uint32_t a_slotMask, std::function<void()> a_commit) {
        if (!a_commit) {
            return;  // nothing to do and nothing to fall back to
        }
        if (!Settings::GetSingleton().requipFlourish || !a_actor || a_slotMask == 0) {
            a_commit();  // the swap still has to happen
            return;
        }

        // ⚠ A SECOND SWAP MID-FLOURISH SUPERSEDES RATHER THAN QUEUES, and it
        // tears the old one down through the one teardown that is allowed to
        // write originals back. Done before the arm below, because ArmRequip
        // would otherwise record the peak values this flourish had written as
        // if they were the garment's own.
        {
            std::scoped_lock l(g_lock);
            if (g_phase != Phase::kIdle) {
                TeardownLocked(Teardown::kSuperseded);
            }
        }

        // ⚠⚠ ABOVE THE ARM, AND THE NAKED SWAP IS THE WHOLE REASON. This sat
        // below the armed == 0 return until 2026-08-15, so the one swap that
        // needed it most was the one swap that never got it: strip to Naked and
        // there is no garment left to light, the arm returns nothing, and the
        // flourish gave up before the only half that could still have shown
        // something. The aura does not depend on there being geometry to paint.
        //
        // Fired at t=0 and given the whole flourish's length, so it outlives
        // both halves and is still covering the cut when the rebuild lands.
        // ⚠⚠ TWO MASKS, AND CONFLATING THEM IS THE MISTAKE THIS SPLIT AVOIDS.
        // a_slotMask answers "did anything change", which is what decides
        // whether the flourish happens at all and must keep coming from the
        // diff. paintMask answers "what gets lit", which the field widened to
        // everything worn on 2026-08-15: a swap that keeps the boots used to
        // leave two dark boots on a violet character.
        const std::uint32_t paintMask =
            Settings::GetSingleton().requipFlashAll ? ~0u : a_slotMask;

        PlayAura(a_actor, paintMask);
        ParsePeakColour();

        const std::size_t armed = OutfitDye::ArmRequip(a_actor, paintMask);

        bool commitNow = false;
        {
            std::scoped_lock l(g_lock);
            g_actorId     = a_actor->GetFormID();
            g_armedShapes = armed;
            g_slotMask    = paintMask;
            if (armed == 0) {
                // ⚠⚠ NOTHING TO BURN IS NOT NOTHING TO DO, AND TREATING IT AS
                // "no flourish" IS WHY DRESSING FROM NAKED WAS SILENT. Arming
                // walks what is worn RIGHT NOW, so leaving Naked finds skin,
                // refuses all of it and returns zero, while the outfit about to
                // arrive is exactly what the player wants to see resolve. The
                // burn genuinely has no subject, so skip it: commit at once and
                // go straight to Wait, where the retry lights the new garments
                // as soon as the rebuild hands them over.
                g_commit = nullptr;
                EnterLocked(Phase::kWait);
                commitNow = true;
            } else {
                g_commit = std::move(a_commit);
                EnterLocked(Phase::kBurn);
            }
        }
        // ⚠ OUTSIDE THE LOCK. The commit posts a refresh and runs whatever the
        // caller handed over, and holding this lock across foreign code is how
        // an ordering nobody designed becomes a deadlock later.
        if (commitNow) {
            a_commit();
        }
    }

    void Tick() {
        if (!g_sessionLive.load(std::memory_order_acquire)) {
            return;
        }
        std::scoped_lock l(g_lock);
        if (g_phase == Phase::kIdle) {
            return;  // the common case, and it costs one compare
        }

        auto* const actor =
            g_actorId != 0 ? RE::TESForm::LookupByID<RE::Actor>(g_actorId) : nullptr;
        const bool alive = actor != nullptr && actor->Get3D(false) != nullptr;

        const double elapsed = ElapsedLocked();
        const auto   why     = RequipFlourish::Classify(
            g_phase, alive, g_sessionLive.load(std::memory_order_acquire),
            /*superseded*/ false, elapsed, g_waitFrames);
        if (RequipFlourish::EndsIt(why)) {
            // ⚠ THE COMMIT RUNS EVEN ON AN ABANDONED FLOURISH, unless it has
            // already run. Burn is the only phase where it has not, so a
            // teardown from Burn still owes the player their outfit change.
            auto owed = g_phase == Phase::kBurn ? std::move(g_commit) : std::function<void()>{};
            TeardownLocked(why);
            if (owed) {
                owed();
            }
            return;
        }

        switch (g_phase) {
            case Phase::kBurn: {
                OutfitDye::PulseRequip(RequipFlourish::BurnMix(elapsed), g_peak);
                if (RequipFlourish::NextPhase(g_phase, false, elapsed) == Phase::kWait) {
                    // ⚠ THE CUT. The commit POSTS a refresh; it does not perform
                    // one. Nothing below may assume the rebuild has happened.
                    auto commit = std::move(g_commit);
                    g_commit    = nullptr;
                    EnterLocked(Phase::kWait);
                    if (commit) {
                        commit();
                    }
                }
                break;
            }
            case Phase::kWait: {
                ++g_waitFrames;
                // ⚠⚠ THE ONLY QUESTION ALLOWED TO MOVE THIS ON. Every armed
                // record whose property no longer matches its geometry is a
                // shape the rebuild replaced, so "none of them match any more"
                // IS the rebuild having landed. A clock cannot answer this.
                const bool ready = OutfitDye::RequipLiveShapes() == 0;
                if (RequipFlourish::NextPhase(g_phase, ready, elapsed) == Phase::kCondense) {
                    // The old records are all stale by construction. Put them
                    // down and arm the new geometry on the same mask.
                    OutfitDye::EndRequip();
                    // ⚠⚠ DETACHING THE OLD AND ATTACHING THE NEW ARE TWO
                    // MOMENTS, NOT ONE, and reading them as one is what made the
                    // flourish intermittent in the field on 2026-08-15: it fired
                    // on some outfit pairs and not others, at random. Every
                    // armed record going stale proves the old geometry is GONE.
                    // It proves nothing about the replacement having arrived,
                    // and on the swaps that showed nothing the log said exactly
                    // that: "armed NOTHING ... 0 refused as character", a walk
                    // that found no geometry rather than one that refused it.
                    //
                    // So an empty arm here is not a verdict, it is "not yet".
                    // Stay in Wait, keep counting frames against the same
                    // budget, and try again next frame. The budget expiring is
                    // still the backstop, and it now means the rebuild really
                    // never landed rather than that we asked one frame early.
                    g_armedShapes = OutfitDye::ArmRequip(actor, g_slotMask, /*logEmpty*/ false);
                    if (g_armedShapes == 0) {
                        break;
                    }
                    EnterLocked(Phase::kCondense);
                }
                break;
            }
            case Phase::kCondense:
                OutfitDye::PulseRequip(RequipFlourish::CondenseMix(elapsed), g_peak);
                break;
            case Phase::kIdle:
                break;
        }
    }

    void ResolveForms() {
        auto* const dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return;
        }

        const auto& settings = Settings::GetSingleton();
        g_art     = LookupList<RE::BGSArtObject>(dh, settings.requipArtForm, "sRequipArtForm");
        g_slotArt = LookupList<RE::BGSArtObject>(dh, settings.requipSlotArtForm,
                                                 "sRequipSlotArtForm");
        g_shader =
            LookupSpec<RE::TESEffectShader>(dh, settings.requipShaderForm, "sRequipShaderForm");
        const bool artFromIni    = !g_art.empty();
        const bool shaderFromIni = g_shader != nullptr;

        // ⚠ THE INI IS AN OVERRIDE, NOT A REPLACEMENT, and it is per record
        // rather than all or nothing. A field test that borrows a vanilla
        // shader while keeping the mod's own art has to be one INI edit, or
        // nobody will run it.
        if ((g_art.empty() || !g_shader) && dh->LookupModByName(kEsp)) {
            if (g_art.empty()) {
                if (auto* const own = dh->LookupForm<RE::BGSArtObject>(kArtID, kEsp)) {
                    g_art.push_back(own);
                }
            }
            if (!g_shader) {
                g_shader = dh->LookupForm<RE::TESEffectShader>(kShaderID, kEsp);
            }
        }

        if (g_art.empty() || !g_shader) {
            spdlog::warn("Requip: art {}, shader {}. The garments still animate; the aura "
                         "does not. Author ARTO {:#x} and EFSH {:#x} in {}, or name a "
                         "vanilla one in sRequipArtForm/sRequipShaderForm (OS-206).",
                         g_art.empty() ? "MISSING" : "ok", g_shader ? "ok" : "MISSING", kArtID,
                         kShaderID, kEsp);
        } else {
            spdlog::info("Requip: aura resolved, {} art from {}, {} per-garment art, shader "
                         "from {} (OS-206).",
                         g_art.size(),
                         artFromIni ? settings.requipArtForm : std::string{ kEsp },
                         g_slotArt.size(),
                         shaderFromIni ? settings.requipShaderForm : std::string{ kEsp });
        }
    }

    void Start() { g_sessionLive.store(true, std::memory_order_release); }

    void Stop() {
        g_sessionLive.store(false, std::memory_order_release);
        std::scoped_lock l(g_lock);
        if (g_phase != Phase::kIdle) {
            auto owed = g_phase == Phase::kBurn ? std::move(g_commit) : std::function<void()>{};
            TeardownLocked(Teardown::kSessionEnded);
            if (owed) {
                owed();
            }
        }
    }

}  // namespace OS::Requip
