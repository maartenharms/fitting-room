#include "PCH.h"

#include "HeadBuildHook.h"

#include "BipedPost.h"     // RestoreHeadPartPartitions: round eighteen's parts[131-]
#include "EditorWindow.h"  // IsOpen: the dismember census's in-FR gate
#include "FsmpBridge.h"    // CooperativeSkin: the settling switched head's wig binding
#include "HairColor.h"
#include "MakeupApi.h"
#include "HookSite.h"      // OwningModule, to name whoever else patched the entry
#include "NpcHair.h"       // NoteEngineHeadBuild: a follower's attached style, re-culled
#include "OverlayApi.h"  // Read: what skee holds against a node
#include "OutfitDye.h"     // QueueRepaint: a head dye does not survive a rebuild
#include "OutfitSession.h"
#include "ProfileApply.h"  // SwitchSettling: the window the FSMP publish is gated on
#include "LookFaceTint.h"  // the look's baked face, re-bound after every build
#include "ProfileStore.h"  // the jslot-to-race lookup the late-rebake gate needs
#include "SculptProbe.h"   // Dump: the sculpt route's shape-for-shape census
#include "Settings.h"
#include "SkinApi.h"       // ReapplyHead: a skin pack's head files do not survive one either
#include "VersionCheck.h"  // IdOk - membership, not "REL gave me an address"

#include <safetyhook.hpp>

#include <atomic>
#include <cmath>     // fabs, the detour-exit FOD sum
#include <cstring>
#include <intrin.h>  // _ReturnAddress, for the switched-head census
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {

    // BSFaceGenManager::PrepareHeadPartForShaders. CommonLib already declares
    // it with these ids and this signature, so nothing here is a hand-measured
    // offset and there is no call site to locate: the FUNCTION is the target.
    //
    // ⚠ THE FUNCTION AND NOT ITS CALLERS, DELIBERATELY. Every other hook in
    // this plugin patches a call site because the worker is inlined or shared,
    // and both of those come with the AE trap that the call COUNT differs from
    // SE. This one has a published id on both runtimes and one body, so
    // detouring the body catches every caller including the ones nobody has
    // enumerated - which is the entire point, since the rebuild that cost 28
    // seconds of wrong colour came from a caller Fitting Room cannot see.
    constexpr REL::RelocationID kPrepareHeadPart{ 26259, 26838 };

    SafetyHookInline           g_hook{};
    std::atomic<bool>          g_installed{ false };
    // The entry as SafetyHook left it, and where it pointed. Compared later
    // rather than trusted: see ReportEntryIntegrity in the header for the two
    // faults this separates.
    std::uintptr_t             g_entry{ 0 };
    std::uint8_t               g_entryAfterInstall[16]{};

    // ⚠ BOTH OF THESE MOVED TO HookSite.h ON 2026-08-17 and are used here
    // through these two names so every call site below reads as it always did.
    // FsmpBridge needs the same branch decode to prove FSMP owns an entry
    // before it cooperates through it, and a second copy of a decoder is a
    // second thing that learns about a jump shape late. The FF 25 story that
    // grew JmpTargetAt lives with the function.
    using OS::HookSite::BytesAt;
    using OS::HookSite::JmpTargetAt;

    std::atomic<std::uint32_t> g_hairBuilds{ 0 };
    // ⚠⚠ A SEPARATE BUDGET FROM g_hairBuilds, AND PER LOAD.
    // g_hairBuilds is a session total and HairBuilds() reports it, so it
    // cannot be re-armed. r85 spent all eight of its log lines inside the
    // FIRST load, four of them on followers, and the second load, the one
    // carrying the previous character's hair colour, printed nothing. Same
    // failure r65 had: a cap exists to bound one burst, not to hand the
    // whole session to whichever burst came first.
    std::atomic<std::uint32_t> g_hairLogged{ 0 };
    std::atomic<std::uint32_t> g_repaints{ 0 };
    // One repaint per drain however many parts were built. A head build calls
    // the detour once per part and only one of them is hair, so this mostly
    // guards the case of several actors rebuilding in the same frame.
    std::atomic<bool>          g_queued{ false };

    // ⚠⚠ EVERY EXIT FROM THIS PATH SAYS SO ONCE, and that is not belt and
    // braces. The first version of this file logged only on a successful
    // repaint, which is the exact instrumentation gap Inventory3DHooks.cpp
    // warns about in its own comment and which cost a field round here on
    // 2026-08-12: the detour installed, the colour was still wiped, and a
    // silent log could equally mean "never fired", "fired for somebody else"
    // or "fired and was gated out". Those have three different fixes.
    std::atomic<bool> g_saidLive{ false };
    std::atomic<bool> g_saidBail{ false };
    std::atomic<bool> g_saidStoodDown{ false };

    void SayOnce(std::atomic<bool>& a_latch, const char* a_what) {
        if (!a_latch.exchange(true, std::memory_order_acq_rel)) {
            spdlog::info("HeadBuildHook: {}", a_what);
        }
    }

    // The late-rebake gate (r47). Reads the player head's facegen material:
    // if the BOUND tint is one of FR's exported preset files and the look it
    // belongs to was captured on a race the player's head does not currently
    // wear, the binding is residue from a split-brain save and the face is
    // rebaked from the tint list this save actually holds. Any other binding
    // - the live 'Player face tint' bake, another mod's file, an FR file
    // whose race matches - is left alone.
    bool RebakeIfForeignPresetTintBound(RE::PlayerCharacter* a_pc, const char* a_edge) {
        auto* const face = a_pc->GetFaceNodeSkinned();
        if (!face) {
            return false;
        }
        std::string boundFile;
        RE::BSVisit::TraverseScenegraphGeometries(
            face, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                    a_geom->GetGeometryRuntimeData()
                        .properties[RE::BSGeometry::States::kEffect]
                        .get());
                if (!prop || !prop->material ||
                    prop->material->GetFeature() !=
                        RE::BSShaderMaterial::Feature::kFaceGen) {
                    return RE::BSVisit::BSVisitControl::kContinue;
                }
                auto* const fg =
                    static_cast<RE::BSLightingShaderMaterialFacegen*>(prop->material);
                if (auto* const tt = fg->tintTexture.get()) {
                    if (const char* n = tt->name.c_str(); n && *n) {
                        boundFile = n;
                    }
                }
                return RE::BSVisit::BSVisitControl::kStop;
            });
        if (boundFile.empty()) {
            return false;
        }
        std::string lower = boundFile;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        const auto dir = lower.find("chargen\\exported\\");
        if (dir == std::string::npos) {
            return false;  // the live bake or another mod's business
        }
        // The stem between the folder and the extension is the jslot name the
        // face block stores ("CharGen name, no path, no extension").
        std::string stem = boundFile.substr(dir + 17);
        if (const auto dot = stem.rfind('.'); dot != std::string::npos) {
            stem.resize(dot);
        }
        const auto entry = OS::ProfileStore::GetSingleton().Snapshot();
        const RE::TESRace* lookRace = nullptr;
        bool               isOurs   = false;
        for (const auto& e : entry) {
            if (!e.profile.face || e.profile.face->jslot != stem) {
                continue;
            }
            isOurs = true;
            if (e.profile.character) {
                if (auto* const dh = RE::TESDataHandler::GetSingleton()) {
                    lookRace = dh->LookupForm<RE::TESRace>(
                        e.profile.character->race.localFormID,
                        e.profile.character->race.modName);
                }
            }
            break;
        }
        if (!isOurs || !lookRace) {
            return false;  // not our file, or a face-only look whose race is unknowable
        }
        auto* const base = a_pc->GetActorBase();
        auto* const worn = a_pc->GetRaceData().charGenRace
                               ? a_pc->GetRaceData().charGenRace
                               : (base ? base->race : nullptr);
        if (!worn || worn == lookRace) {
            return false;  // a legit look'd save: the head wears the look's race
        }
        OS::MakeupApi::RebakeFaceTint();
        spdlog::info(
            "HeadBuildHook: a late build bound '{}', captured on race {:08X}, "
            "onto a head wearing {:08X}; that is residue from a split save, "
            "so the face was rebaked from this save's own tint list. Caught on "
            "the {} edge.",
            boundFile, lookRace->GetFormID(), worn->GetFormID(), a_edge);
        return true;
    }

    // Runs on the main thread, off the engine's stack. Everything that needs a
    // lock or a scenegraph walk lives HERE and not in the detour.
    void RepaintNow() {
        g_queued.store(false, std::memory_order_release);

        // ---- the FSMP publish for a settling switched head ------------------
        //
        // Round twelve isolated the bald wig to the one layer left after the
        // census proved everything else: KSSMP_Kaysa_Elf attached, visible,
        // unit scale, built and painted through the live entry, and rendering
        // as nothing. On this rig's FSMP line the SkinAll hook replaces
        // vanilla skinning and never calls the original, so a build it
        // swallows without binding leaves the wig with no skin at all: a
        // zero-extent bound at the origin, frustum-culled, which reads as
        // bald ([[an-empty-bound-is-not-a-measurement]] made flesh). The cure
        // is the same cooperative pass NpcHair ships for follower SMP
        // styles: publish the face node to FSMP once per build while the
        // switched apply settles, so every rebuild in the window, the
        // race-switch reload included, ends bound. Gated on the settle
        // window so steady-state behaviour is untouched, and on
        // IsAvailable() per FsmpBridge's contract.
        //
        // ⚠ Get3D(false), the THIRD-person root, deliberately: Get3D() is
        // the first-person root whenever the player is in first person, and
        // the face node hangs off 3p (memory: player-get3d-is-first-person).
        if (OS::ProfileApply::SwitchSettling() && OS::FsmpBridge::IsAvailable()) {
            if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                auto* const face = player->GetFaceNodeSkinned();
                auto* const root = player->Get3D(false);
                auto* const skel = root ? root->AsNode() : nullptr;
                if (face && skel) {
                    std::vector<std::string> names;
                    for (const auto& child : face->GetChildren()) {
                        if (auto* const obj = child.get();
                            obj && obj->name.c_str() && *obj->name.c_str()) {
                            names.emplace_back(obj->name.c_str());
                        }
                    }
                    OS::FsmpBridge::CooperativeSkin(face, skel, names);
                    spdlog::info("HeadBuildHook: FSMP publish for the settling "
                                 "switched head ({} geometry name(s)).",
                                 names.size());
                }
                // Round eighteen's probe rewrote the bald mechanism: the
                // strands are fully skinned with a healthy bound and their
                // ONE partition, head-family slot 131, sits editorVisible
                // OFF with nothing worn on the matching biped slot. The
                // publish above cannot touch that layer (a partition is
                // disabled below the geometry), so the restore runs beside
                // it: once now, once deferred, the body-slot restorer's own
                // two-pass doctrine.
                OS::BipedPost::RestoreHeadPartPartitions(player, "repaint-now");
                OS::BipedPost::QueueRestoreHeadPartPartitions(player->GetHandle(),
                                                              "repaint-now-queued");
            }
        }

        // ---- step 5: a head dye does not survive the rebuild ----------------
        //
        // ⚠⚠ MEASURED, NOT ASSUMED. The head census that measured it is gone
        // with the rest of the dyeing-eyes instruments; the reading stands.
        // Field
        // 2026-08-17: `00UBE_FemaleHead`'s MATERIAL pointer took five distinct
        // values across six readings, and the one repeat was the pair with no
        // rebuild between them, while its textureSet pointer was identical all
        // six times. So the rebuild replaces the material a swap record points
        // at, and a dyed horn comes back undyed with the record naming something
        // that no longer paints anything.
        //
        // ⚠ ABOVE BOTH GATES, and for a sharper reason. A head
        // dye is not the hair colour: DrivesPlayerHairTint is false on a
        // character whose outfit dyes a horn and sets no hair colour, which is
        // the ordinary case, so a repaint armed below that gate would never fire
        // for the feature it exists for.
        //
        // ⚠ QueueRepaint, NEVER Repaint, and it is the same choice the worn-pass
        // hook made for the same measured reason: the head is still being built
        // inside this call chain, and QueueRepaint re-resolves the actor and the
        // outfit when the task drains. It also RESTORES before it repaints, which
        // is what stops a second pass recording a swapped material as the
        // original and stranding the real one.
        //
        // ⚠ NOT GATED ON "does this outfit dye a head part". The chain re-resolves
        // the outfit itself, so asking here would mean reading the session under
        // a lock at a point where the answer can still change before the task
        // runs, and a Repaint with nothing to do is a walk that writes nothing.
        // Cheap, and honest about who owns the decision.
        if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
            OS::OutfitDye::QueueRepaint(player->GetHandle());
        }

        // ---- the skin pack's head files do not survive the rebuild either --
        //
        // Same reason as the head dye and the same place: the face is node
        // overrides skee applies at load and on demand and never after a
        // rebuild, and this is the far side of the rebuild. Above both gates
        // for the head dye's reason (a skin pack is not the hair colour), and
        // not queued through anything: this IS the queued task, on the main
        // thread, after the whole build. A no-op for an actor wearing no pack.
        if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
            OS::SkinApi::ReapplyHead(player);
        }

        // ---- the sculpt route's measurement, and it runs before its feature -
        //
        // ⚠ AN INSTRUMENT (SculptProbe.h), here because this is the far side
        // of the build and the main thread. Skipped with the editor open,
        // where one slider drag rebuilds the head over and over and the
        // census would say the same thing a hundred times. It also dumps only
        // when the head actually changed, so a quiet session costs one block.
        if (auto* const ui = RE::UI::GetSingleton();
            !ui || !ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME)) {
            OS::SculptProbe::Dump("head-build");
        }

        // ⚠ STAND ASIDE FOR THE CHARACTER EDITOR, and this is the one gate that
        // is about correctness rather than cost. RaceMenu's own slider drives
        // the same engine painter, so every drag of it lands in this detour.
        // Repainting on the far side of that would fight the user for their own
        // hair colour, one frame at a time, in the menu they opened to change
        // it. HeadEditorSink already owns the handoff at both edges; this only
        // has to keep out of the way in between.
        // ⚠⚠ THE RESIDUE CHECK RUNS FIRST, ABOVE THE EDITOR GATE, AND THAT
        // ORDERING IS THE WHOLE FIX. Standing aside for the editor is about not
        // fighting the user for a colour they are editing. It is NOT about
        // leaving another character's baked face on this one, and until this
        // moved the bail below returned before the check could ever look.
        //
        // MEASURED r83, the round that named the cause. The player's TESNPC is
        // ONE OBJECT for the whole session: base 00000007 kept the same pointer
        // across every load. RaceMenu's LoadPreset writes the exported tint path
        // into that NPC ("Details may be saved to the TESNPC, make sure this
        // character is unique!", IPluginInterface.h), so loading a different
        // save does not clear it, and the next head build binds it again:
        //
        //   Umbrael save  head='00UBE_FemaleHead' tintTex=0x2D384C7EDA0 'FR_Umbrael 15.dds'
        //   NORD save     head='MaleHeadNord'     tintTex=0x2D384C7EDA0 'FR_Umbrael 15.dds'
        //
        // Same texture POINTER, different character, across the boundary. The
        // load's own rebake cleared it at +2.5 s and RaceMenu's build put it
        // back at +16 s, inside the editor, where this function used to return
        // two lines above the only code that could tell.
        //
        // ⚠ AND NO WINDOW. It used to sit behind FaceRebakeWindowOpen(), 20 s
        // from the load, which an editor opened a minute later walks straight
        // past. The check pays for itself instead: it early-returns on the
        // first geometry whose bound tint is not in CharGen\Exported, and it
        // only rebakes when the file is OURS and was captured on a race this
        // head does not wear. A legit look'd save fails that test and is left
        // alone.
        if (auto* const pc = RE::PlayerCharacter::GetSingleton()) {
            RebakeIfForeignPresetTintBound(pc, "queued head build");
        }
        if (auto* const ui = RE::UI::GetSingleton();
            ui && ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME)) {
            SayOnce(g_saidBail, "a head build finished with the character editor open, so "
                                "the repaint stood aside. RaceMenu owns the colour there.");
            // An instrument: what the list wears on the far side of each build
            // the editor makes, so the log says which build brought a previous
            // character's makeup back (field 2026-09-02).
            if (auto* const pc = RE::PlayerCharacter::GetSingleton()) {
                OS::MakeupApi::DumpWorn(pc, "head build in the editor");
            }
            return;
        }
        // ⚠⚠ THE SKIN TONE FIRST, AND BEFORE THE HAIR GATE BELOW. A head build
        // restores the tint list from the character's saved layers, which takes
        // the body colour with it: MEASURED r39, the captured tone held at
        // +0.25 s, +0.5 s and +1 s and was gone by +2 s, with the build in
        // between. Held rather than fought, exactly as the hair is, and silent
        // when nothing put it back. It sits ahead of the outfit-hair gate
        // because a look with no hair colour still owns its skin tone.
        if (auto* const pc = RE::PlayerCharacter::GetSingleton()) {
            OS::MakeupApi::ReassertSkinTone(pc);
            // And the rebake a load owes, here rather than at kPostLoadGame,
            // because it has to land on the far side of the build the load
            // starts. See MakeupApi::RebakeFaceTint: a baked face outlives the
            // character it was baked for.
            // ⚠⚠ THE LOOK'S OWN BAKED FACE FIRST, AND THE REBAKE ONLY IF
            // THERE IS NONE. A look whose face came from a preset carries its
            // painted detail in one exported file and NOWHERE ELSE: the
            // 'makeup' step stands aside for such a look, so the 34-slot list
            // the rebake composites from is empty of it. Re-binding here and
            // then compositing over it would land us back where the field
            // report started, with Umbrael returning without her skull.
            if (OS::LookFaceTint::Apply(pc)) {
                OS::MakeupApi::CancelFaceRebake();
            }
            OS::MakeupApi::RunOwedFaceRebake();
            // ⚠⚠ AND EVERY BUILD AFTER THE FIRST, WHICH THE ONE SHOT ABOVE
            // CANNOT REACH AND THE RESIDUE CHECK REFUSES TO. Field 2026-08-26
            // 20:54: the load's debt was paid on the first build and the face
            // was right, then eight more builds followed over three and a half
            // minutes, each one leaving a NEW material with an UNNAMED tint.
            // RebakeIfForeignPresetTintBound stands down on all eight, because
            // an unnamed texture is not a path under CharGen\\Exported: the
            // right answer to the question IT asks and the wrong one for this.
            // The face carried no composite at all for the rest of the session
            // while the body stayed correct, because the body reads
            // bodyTintColor and the head wears a texture.
            //
            // ⚠ ARMED RATHER THAN DONE HERE. Reading the head inside the build
            // that is still binding its own material answers about a state one
            // second from settling, and MakeupApi already owns a bounded delayed
            // look with a fight guard on it. One watch, not two painters.
            OS::MakeupApi::ArmFaceRebakeAfterHeadBuild();
            // ⚠⚠ AND THE LATE BUILDS, WHICH THE ONE SHOT ABOVE CANNOT REACH.
            // r47 MEASURED, twice: the owed rebake wins the FIRST build and
            // the face is right; a SECOND build at +7-12 s re-binds the
            // preset tint FILE skee's co-save restored
            // (Textures\CharGen\Exported\FR_*.dds) and the face goes wrong
            // and STAYS wrong to +60 s. The save is split-brain: the head is
            // the save's own race while the bound tint was baked for the
            // look's - the user saved mid-mess.
            //
            // The gate is a RACE MISMATCH, and it is what keeps a LEGIT
            // look'd save intact: there the character block committed the
            // look's race into the save, the head is the look's head, and
            // the file matches - no rebake. Only a file whose look was
            // captured on a race the player's head does not wear is residue.
            // A file that is not one of our FR_ jslots is another mod's
            // business and stays untouched.
            // The residue check that used to live here runs above the editor
            // gate now, unwindowed. See the comment there.
        }
        // ⚠ THE COLOUR FLAG, NOT THE HEAD-PART ONE. This puts back a hair
        // COLOUR the outfit is driving and swaps nothing, so it belongs with
        // the skin tone repair and not with the eyes. Gating it on
        // bReassertAppearance is what left a rebuilt head wearing the engine's
        // paint from 1.1.3 until now.
        if (!OS::Settings::GetSingleton().keepColoursAfterRebuild) {
            SayOnce(g_saidStoodDown,
                    "a head build finished and [Compat] bKeepColoursAfterRebuild is "
                    "off, so the engine's own paint is what stays on the hair.");
            return;
        }
        // Asked here rather than in the detour because it takes the session
        // lock, and taking a lock inside an engine call the game thread is
        // halfway through is how a deadlock gets written. Also the honest
        // gate: with no outfit colour there is nothing of ours to put back and
        // the engine's own paint is the right answer.
        if (!OS::OutfitSession::GetSingleton().DrivesPlayerHairTint()) {
            SayOnce(g_saidBail, "a head build finished and no outfit hair colour is in "
                                "force, so there is nothing of ours to put back.");
            return;
        }
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        // Repaint already prefers the exact RGB over the snapped form, and only
        // while Fitting Room's colour is still the one on the actor base. That
        // pairing is what stops this repainting over a colour something else
        // set, so no extra guard is needed or wanted here.
        const auto n = OS::HairColor::Repaint(player);
        const auto count = g_repaints.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count == 1) {
            spdlog::info("HeadBuildHook: repainted the player's hair after an engine head "
                         "build ({} material(s)). This line existing at all is what "
                         "separates 'the detour never fired' from 'it fired and had "
                         "nothing to do'.",
                         n);
        } else {
            spdlog::debug("HeadBuildHook: repainted the player's hair after a head build "
                          "({} material(s), {} this session).",
                          n, count);
        }
    }

    // The switched-head census: who rebuilds the player's head, out of what,
    // onto which node. Round seven (2026-08-22 11:12) had the reconcile land
    // right and the head revert anyway, and the existing lines could not
    // separate "a build from a reverted part list" from "a paint-free
    // reattach of a cached node": they named neither the part nor the node
    // nor the caller. Raw pointers only, resolved offline against the module
    // bases Install logs, because naming a module from inside the detour
    // means loader locks the detour rules forbid.
    std::atomic<std::uint32_t> g_censusLines{ 0 };
    // 128 since round eleven: 64 ran out before the race-switch reload's
    // own build, which is the one the bald-hair question needs to see.
    constexpr std::uint32_t    kCensusCap = 128;

    // The base-revert census, round eight's instrument. The 11:34 census
    // proved the reconcile lands the whole UBE head on the live node and the
    // base then READS NORD AGAIN minutes of frames later with not one build
    // in between: something rewrites the TESNPC itself, silently. The one
    // engine function that re-defaults a base's parts from a race is
    // TESNPC::ChangeRace (SE 24165 / AE 24669, the callee SetRace runs), so
    // its callers ARE the suspects, and this names each one by return
    // address. Player base only, capped, atomics and formatting only.
    constexpr REL::RelocationID kChangeRace{ 24165, 24669 };
    SafetyHookInline            g_raceHook{};
    std::atomic<std::uint32_t>  g_raceLines{ 0 };

    // Round nineteen's handoff instrument: WHO turns head-family partition
    // 131 off. The restore is measured as losing a race against a writer
    // nobody has named; this detours the one engine accessor
    // (BSDismemberSkinInstance::UpdateDismemberPartion, the id BipedPost
    // already membership-checks and field-proved on the ears) and logs every
    // head-family write with the skin-instance pointer (matches WigProbe's
    // skin=), the slot, the direction and the caller's return address,
    // resolved offline against the census bases line. One switched apply
    // names every writer of 131 IN ORDER.
    //
    // ⚠ AN EMPTY CENSUS ACROSS A ROUND WHERE 131 STILL WENT OFF IS ITSELF
    // THE FINDING: editorVisible is a plain byte in the partition array, so
    // a writer that pokes it directly never passes this entry, and the next
    // instrument is a data breakpoint, not a wider gate.
    //
    // Gated in this order, cheapest first: slot in the head family (kills
    // the body-slot churn every equip generates), then the windows - the
    // switched apply's settle, the FR editor being open (the in-FR change),
    // or a swap ladder pending (the churn right after an in-FR change whose
    // editor already closed). Atomics and formatting only, per the detour
    // rules above.
    constexpr REL::RelocationID kUpdateDismember{ 15576, 15753 };
    SafetyHookInline            g_dismemberHook{};
    std::atomic<std::uint32_t>  g_dismemberLines{ 0 };
    std::atomic<bool>           g_dismemberSaidLive{ false };
    constexpr std::uint32_t     kDismemberCap = 192;

    // The morph census, r66's instrument for the load-double. r65 proved the
    // double is MADE during the post-load second build (build one read S0
    // bit-exact with fresh pointers, the erase had landed 6 s earlier, and the
    // cache was clean), so the writer is one of the morph passes inside that
    // build. This names each whole-face and per-part morph pass over the
    // player with its caller, and the FOD pulse beside it says which pass the
    // value doubled under.
    //
    // ⚠ THE WORKER, NOT SKEE'S CALL SITE. skee Write5Calls the CALLER of the
    // whole-face pass (its UpdateMorphs_Hooked wraps the call site at
    // 26835+0xB7), so the worker's own entry is unowned and a detour here
    // sees every caller, skee's wrapper included. skee's ApplyMorphs runs
    // AFTER the worker returns and is invisible here by design: the pulse's
    // job is to say whether the double lands inside this pass or after it.
    //
    // ⚠ THE SE IDS ARE NOT SKEE'S COMMENTS. Current skee master's "Steam"
    // block is 1.6.640 and its comment ids are AE-numbering; the 1.5.97 pair
    // was located by the NoseType slider tables both twins reference
    // (SE 24209/24210, sizes 482/474, first bytes identical to AE
    // 24713/24714).
    constexpr REL::RelocationID kUpdateNPCMorphs{ 24209, 24713 };
    constexpr REL::RelocationID kUpdateNPCMorph{ 24210, 24714 };
    SafetyHookInline            g_morphsHook{};
    SafetyHookInline            g_morphHook{};
    std::atomic<std::uint32_t>  g_morphsLines{ 0 };
    std::atomic<std::uint32_t>  g_morphLines{ 0 };
    std::atomic<bool>           g_morphsSaidLive{ false };
    constexpr std::uint32_t     kMorphCap = 48;

    void UpdateNPCMorphsDetour(RE::TESNPC* a_npc, void* a_parts,
                               RE::BSFaceGenNiNode* a_node) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        g_morphsHook.call<void, RE::TESNPC*, void*, RE::BSFaceGenNiNode*>(
            a_npc, a_parts, a_node);
        if (!g_morphsSaidLive.exchange(true, std::memory_order_acq_rel)) {
            spdlog::info("HeadBuildHook: morph census LIVE (first whole-face pass, "
                         "npc {:08X}, caller 0x{:X}).",
                         a_npc ? a_npc->GetFormID() : 0u, caller);
        }
        if (auto* const pc = RE::PlayerCharacter::GetSingleton();
            pc && a_npc && pc->GetActorBase() == a_npc) {
            if (const auto n = g_morphsLines.fetch_add(1, std::memory_order_relaxed);
                n < kMorphCap) {
                spdlog::info("HeadBuildHook: morph census: WHOLE-FACE pass on the "
                             "player, node {}, caller 0x{:X}.",
                             static_cast<const void*>(a_node), caller);
                // r77: the reading between the halves. The pre-face and
                // post-wholeface pulses bracket skee's whole pipeline; skee's
                // wrapper owns the call site ABOVE this worker, so its
                // sculpt+extended ApplyMorphs half runs after this detour
                // returns. A synchronous read here therefore carries the
                // engine half's output alone, and against post-wholeface it
                // names which half the r76 doubling rides. Bounded for the
                // detour: the passed node's direct children, no traversal,
                // reads and formatting only.
                if (a_node) {
                    std::string line;
                    for (const auto& child : a_node->GetChildren()) {
                        auto* const geom = child ? child->AsGeometry() : nullptr;
                        if (!geom || geom->name.empty() ||
                            geom->GetType().get() !=
                                RE::BSGeometry::Type::kDynamicTriShape) {
                            continue;
                        }
                        auto* const ds =
                            static_cast<RE::BSDynamicTriShape*>(geom);
                        const auto  verts =
                            ds->GetTrishapeRuntimeData().vertexCount;
                        const auto  fod = OS::FaceGen::ResolveFod(
                            geom, static_cast<std::uint16_t>(verts));
                        if (!fod) {
                            continue;
                        }
                        double sum = 0.0;
                        for (std::uint32_t i = 0; i < fod.count; ++i) {
                            const float* const d =
                                fod.verts +
                                i * OS::FaceGen::kFodFloatsPerVertex;
                            sum += std::fabs(d[0]) + std::fabs(d[1]) +
                                   std::fabs(d[2]);
                        }
                        line += fmt::format("{}'{}'={:.2f}@{}",
                                            line.empty() ? "" : " ",
                                            geom->name.c_str(), sum,
                                            static_cast<const void*>(fod.verts));
                    }
                    spdlog::info("FodPulse[detour-exit]: {}",
                                 line.empty() ? "(no FOD shapes)" : line);
                    // ⚠⚠ THE EXIT HALF OF A BRACKET, AND THE ENTRY HALF
                    // IS THE HAIR-BUILD LINE'S COLOUR FORM. r88 measured the
                    // painter being handed 000A042E rgb=(57,55,40), the Nord's
                    // OWN colour, three builds running, while the strands read
                    // (14,16,17) both before and after. AppearanceWatch samples
                    // once a second, so it cannot say whether the paint landed
                    // and was overwritten or never landed at all. This reads on
                    // the engine's stack the instant the pass returns, which is
                    // the only place those two stories look different.
                    spdlog::info("HairTintPulse[detour-exit]: {}",
                                 OS::HairColor::GeometryTintReadingOf(a_node));
                }
                // r73's bracket: a pulse queued from the hook (the RepaintNow
                // posture - the walk runs on the task drain, after the pass
                // chain, never in the detour). Paired with the pre-face pulse
                // in ProfileApply, the two lines bracket skee's whole apply
                // pipeline: whatever adds the extra look does it between
                // them, and the census rows in between are its company.
                if (auto* const task = SKSE::GetTaskInterface()) {
                    task->AddTask(
                        [] { OS::SculptProbe::PulseHead("post-wholeface"); });
                }
            }
        }
    }

    // The model-morph census, r67's instrument, and it sits on the ONE entry
    // every morph application funnels through: BSFaceGenModel::ApplyMorph.
    // r66 proved the store lands on the head through a path that never
    // touches UpdateNPCMorphs (skee's post-load Flush queues
    // SKSETaskApplyMorphs, which calls its ApplyMorphs directly), so the
    // worker census above is blind to exactly the pass that matters. skee
    // Write5Branches THIS entry (its ApplyChargenMorph_Hooked adds the
    // custom-slider visit), FR installs at kDataLoaded which is later, so our
    // detour runs first and chains into skee's - the same on-top posture the
    // painter hook has held since 08-12.
    //
    // ⚠ AE ONLY, DELIBERATELY. The AE id is 26831 (the packer skee names
    // FaceGenApplyMorph). The 1.5.97 twin is NOT one fixed id away: the SE
    // build factors the pack/worker split differently (SE 26252/26253 are the
    // slider ITERATORS, not this), current skee master supports only 1.6.x so
    // its source names no 1.5.97 offset, and a guessed id on SE would detour
    // a neighbour. The double under hunt reproduces on AE; SE inherits the
    // fix, not the instrument.
    constexpr std::uint64_t    kApplyModelMorphAE = 26831;
    SafetyHookInline           g_modelMorphHook{};
    std::atomic<std::uint32_t> g_modelMorphLines{ 0 };
    std::atomic<bool>          g_modelMorphSaidLive{ false };
    // 256, not the 48 the worker census uses: this entry fires per morph per
    // part per ACTOR, followers rebuild at every load right beside the
    // player, and the burst the hunt needs sits inside the first seconds.
    constexpr std::uint32_t    kModelMorphCap = 256;

    std::uint8_t ApplyModelMorphDetour(RE::BSFaceGenModel* a_model,
                                       RE::BSFixedString* a_name, void* a_tri,
                                       RE::NiAVObject** a_node, float a_relative,
                                       std::uint8_t a_unk) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        const auto ret =
            g_modelMorphHook.call<std::uint8_t, RE::BSFaceGenModel*,
                                  RE::BSFixedString*, void*, RE::NiAVObject**,
                                  float, std::uint8_t>(a_model, a_name, a_tri,
                                                       a_node, a_relative, a_unk);
        if (!g_modelMorphSaidLive.exchange(true, std::memory_order_acq_rel)) {
            spdlog::info("HeadBuildHook: model-morph census LIVE (first apply, "
                         "morph '{}', caller 0x{:X}).",
                         a_name && a_name->c_str() ? a_name->c_str() : "?", caller);
        }
        if (const auto n = g_modelMorphLines.fetch_add(1, std::memory_order_relaxed);
            n < kModelMorphCap) {
            spdlog::info("HeadBuildHook: model-morph census: model {} morph '{}' "
                         "rel={:.3f} node {} caller 0x{:X}.",
                         static_cast<const void*>(a_model),
                         a_name && a_name->c_str() ? a_name->c_str() : "?",
                         a_relative,
                         static_cast<const void*>(a_node ? *a_node : nullptr),
                         caller);
        }
        return ret;
    }

    void UpdateNPCMorphDetour(RE::TESNPC* a_npc, RE::BGSHeadPart* a_part,
                              RE::BSFaceGenNiNode* a_node) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        g_morphHook.call<void, RE::TESNPC*, RE::BGSHeadPart*, RE::BSFaceGenNiNode*>(
            a_npc, a_part, a_node);
        if (auto* const pc = RE::PlayerCharacter::GetSingleton();
            pc && a_npc && pc->GetActorBase() == a_npc) {
            if (const auto n = g_morphLines.fetch_add(1, std::memory_order_relaxed);
                n < kMorphCap) {
                spdlog::info("HeadBuildHook: morph census: part {:08X} pass on the "
                             "player, node {}, caller 0x{:X}.",
                             a_part ? a_part->GetFormID() : 0u,
                             static_cast<const void*>(a_node), caller);
            }
        }
    }

    void UpdateDismemberPartionCensus(RE::BSDismemberSkinInstance* a_this,
                                      std::uint16_t a_slot, bool a_enable) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        g_dismemberHook.call<void, RE::BSDismemberSkinInstance*, std::uint16_t, bool>(
            a_this, a_slot, a_enable);
        // The first call of ANY slot announces itself once, the g_saidLive
        // doctrine: an installed-but-never-called detour and a gated-out one
        // need different fixes.
        if (!g_dismemberSaidLive.exchange(true, std::memory_order_acq_rel)) {
            spdlog::info("HeadBuildHook: dismember census LIVE (first call: skin {} "
                         "slot {} -> {}, caller 0x{:X}).",
                         static_cast<const void*>(a_this), a_slot,
                         a_enable ? "on" : "OFF", caller);
        }
        if (a_slot < 130 || a_slot > 143) {
            return;
        }
        if (!OS::ProfileApply::SwitchSettling() && !OS::EditorWindow::IsOpen() &&
            !OS::BipedPost::HeadSettleActive()) {
            return;
        }
        if (const auto n = g_dismemberLines.fetch_add(1, std::memory_order_relaxed);
            n < kDismemberCap) {
            spdlog::info("HeadBuildHook: dismember census: skin {} slot {} -> {} "
                         "caller 0x{:X}.",
                         static_cast<const void*>(a_this), a_slot,
                         a_enable ? "on" : "OFF", caller);
        }
    }

    char ChangeRaceDetour(RE::TESNPC* a_npc, RE::TESRace* a_race) {
        const auto  caller  = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        auto* const oldRace = a_npc ? a_npc->race : nullptr;
        const auto  result =
            g_raceHook.call<char, RE::TESNPC*, RE::TESRace*>(a_npc, a_race);
        if (a_npc) {
            if (auto* const pc = RE::PlayerCharacter::GetSingleton();
                pc && pc->GetActorBase() == a_npc) {
                if (const auto n = g_raceLines.fetch_add(1, std::memory_order_relaxed);
                    n < 32) {
                    spdlog::info(
                        "HeadBuildHook: ChangeRace census: player base {:08X} -> "
                        "race {:08X} (was {:08X}), ok={}, caller 0x{:X}.",
                        a_npc->GetFormID(), a_race ? a_race->GetFormID() : 0u,
                        oldRace ? oldRace->GetFormID() : 0u,
                        static_cast<int>(result), caller);
                }
            }
        }
        return result;
    }

    void PrepareHeadPartForShaders(RE::BSFaceGenManager* a_this, RE::BSFaceGenNiNode* a_node,
                                   RE::BGSHeadPart* a_part, RE::TESNPC* a_npc) {
        const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
        // ⚠ CALL THROUGH FIRST. The engine's paint is the thing being undone,
        // so writing before it would simply be overwritten, which is the same
        // ordering mistake the colour-then-refresh contract exists for.
        g_hook.call<void, RE::BSFaceGenManager*, RE::BSFaceGenNiNode*, RE::BGSHeadPart*,
                    RE::TESNPC*>(a_this, a_node, a_part, a_npc);

        // Census line for every PLAYER-base build, capped per session.
        // Atomics and formatting only: no locks, no walks, per the rules
        // above this function.
        if (a_part && a_npc) {
            if (auto* const pc = RE::PlayerCharacter::GetSingleton();
                pc && pc->GetActorBase() == a_npc) {
                if (const auto n = g_censusLines.fetch_add(1, std::memory_order_relaxed);
                    n < kCensusCap) {
                    spdlog::info("HeadBuildHook: census: part {:08X} type {} node {} "
                                 "caller 0x{:X}.",
                                 a_part->GetFormID(),
                                 static_cast<int>(a_part->type.underlying()),
                                 static_cast<const void*>(a_node), caller);
                }
            }
        }

        // ⚠ THE FIRST CALL ANNOUNCES ITSELF WHATEVER IT IS FOR. This is the one
        // line that separates "the detour never ran" from every other
        // explanation, and its absence from the 2026-08-12 log is what made
        // that round inconclusive: the hook reported a clean install and then
        // said nothing at all, which is compatible with the painter never being
        // the thing that wipes the colour.
        if (!g_saidLive.exchange(true, std::memory_order_acq_rel)) {
            spdlog::info("HeadBuildHook: the detour is LIVE (first call, part type {}, npc "
                         "{:08X}). This line existing at all is what separates 'never ran' "
                         "from 'ran and was gated out'.",
                         a_part ? static_cast<int>(a_part->type.get()) : -1,
                         a_npc ? a_npc->GetFormID() : 0u);
        }

        // Cheapest gates first, and every one of them is a plain member read.
        // This runs for every head part of every actor the engine builds, so
        // nothing here may take a lock, walk a scenegraph or touch the session.
        //
        // ⚠ HAIR OR FACE, SINCE 2026-08-18. Hair alone was the gate while the
        // only thing on the far side was the hair colour, and a head with no
        // hair part never queued. The skin pack's head files ride RepaintNow
        // now, and a bald head is still a head that got rebuilt; the face part
        // is the one every head has. The hair counters below stay hair-only,
        // because that is what they count.
        if (!a_part || !a_npc) {
            return;
        }
        const auto type = a_part->type.get();
        const bool hair = type == RE::BGSHeadPart::HeadPartType::kHair;
        const bool face = type == RE::BGSHeadPart::HeadPartType::kFace;
        if (!hair && !face) {
            return;
        }
        auto* const player = RE::PlayerCharacter::GetSingleton();
        // ⚠ EVERY HAIR BUILD IS NAMED FOR THE FIRST FEW, MATCH OR NOT. "The
        // painter ran for somebody else" and "the painter ran for the player
        // and the base pointer did not compare equal" are different faults and
        // the second one is entirely plausible: the player's TESNPC is not
        // guaranteed to be the object handed to this call.
        if (hair) {
            g_hairBuilds.fetch_add(1, std::memory_order_relaxed);
            // ⚠⚠ OUTSIDE THE LOG CAP ON PURPOSE. The pre-load erase's retry
            // has to be disarmed by the paint itself, and the eighth hair
            // build is not a reason to stop caring. See
            // ProfileApply::NotePlayerHairBuilt.
            if (player && player->GetActorBase() == a_npc) {
                OS::ProfileApply::NotePlayerHairBuilt();
            }
            if (const auto n = g_hairLogged.fetch_add(1, std::memory_order_relaxed) + 1;
                n <= 8) {
                // ⚠⚠ READ OFF a_npc, WHICH IS THE OBJECT THE PAINTER
                // IS ABOUT TO DERIVE THE TINT FROM, and read HERE because this
                // is the only place it can be caught. r87 measured a Nord whose
                // strands came out of a load wearing (14,16,17), the previous
                // character's near-black, when a fresh process had painted the
                // same character (57,55,40) from the nif's own value. Between
                // those two the base reported `hair (none)` on every one-second
                // sample. If the form is present at THIS instant and absent a
                // second later, that gap is the whole mechanism; if it is
                // absent here too, the painter is not the writer and the hunt
                // moves on. Either answer costs one line.
                auto* const hrd   = a_npc->headRelatedData;
                auto* const color = hrd ? hrd->hairColor : nullptr;
                spdlog::info("HeadBuildHook: hair part built for npc {:08X}; the player's base "
                             "is {:08X} ({}). Its hair colour form is {}.",
                             a_npc->GetFormID(),
                             player && player->GetActorBase()
                                 ? player->GetActorBase()->GetFormID()
                                 : 0u,
                             player && player->GetActorBase() == a_npc ? "MATCH"
                                                                       : "not the player",
                             color ? fmt::format("{:08X} rgb=({},{},{})",
                                                 color->GetFormID(), color->color.red,
                                                 color->color.green, color->color.blue)
                                   : "(none)");
                // ⚠ POST-PAINT, and this line is the only place that is
                // true. The detour calls through FIRST, so by here the
                // engine has finished painting this part. The whole-face
                // pulse fires BEFORE the head parts are painted, which
                // makes the pair a bracket rather than two of the same
                // reading.
                spdlog::info("HairTintPulse[post-paint]: {}",
                             OS::HairColor::GeometryTintReadingOf(a_node));
                // ⚠⚠ r80: NAME THE HOLDER BEFORE WRITING A CURE. The
                // colour that lands between the two pulses has no Fitting Room
                // line in the window and the actor base reads (none), so
                // whoever paints it is holding it somewhere else. The candidate
                // worth testing first is a skee node override, because it is
                // the one with a published clear: RemoveNodeOverride is in
                // RaceMenu's modder header, this plugin already drives it from
                // SkinApi and OverlayApi, and the very same build re-binds a
                // look's exported face tint, which IS one of these.
                //
                // A HELD line here means the cure is a RemoveNodeOverride at
                // the load boundary and nothing more. An absent line on every
                // node kills that route outright and sends the hunt to whoever
                // else owns these materials, which is worth as much.
                if (OS::OverlayApi::Available()) {
                    for (const auto& name : OS::HairColor::HairTintNodeNamesOf(a_node)) {
                        const auto held = OS::OverlayApi::Read(player, name);
                        spdlog::info(
                            "HairHolderProbe: '{}' skee tint override {}.", name,
                            held.hasTint
                                ? fmt::format("HELD rgb=({},{},{})",
                                              static_cast<int>(held.tint.r),
                                              static_cast<int>(held.tint.g),
                                              static_cast<int>(held.tint.b))
                                : "absent");
                    }
                }
            }
            // The bleed probe's build-time reader, and it does NOT share the
            // count cap above: a player build whose charGenRace differs from
            // the base race took its part list from the STORED overlay parts
            // (TESNPC::HasOverlays' form-id-7 branch), and the boundary dumps
            // sit seconds to either side of the build that matters. Silence
            // means the pair was clean; the cap would eat exactly the late
            // build the repro round exists to catch.
            if (player && player->GetActorBase() == a_npc) {
                auto* const cgr      = player->GetRaceData().charGenRace;
                auto* const baseRace = player->GetActorBase()->race;
                if (cgr && baseRace && cgr != baseRace) {
                    spdlog::warn(
                        "CrossSaveProbe[head-build]: PAIR DIFFERS - charGenRace "
                        "{:08X} vs base race {:08X}; this build read the stored "
                        "overlay parts, not npc->headParts.",
                        cgr->GetFormID(), baseRace->GetFormID());
                }
            }
        }
        if (!player || player->GetActorBase() != a_npc) {
            // ⚠⚠ THE FOLLOWER'S HALF, AND IT IS A HANDOFF RATHER THAN WORK.
            // This used to return here, on the reasoning that getting from a
            // TESNPC back to the Actor wearing it means walking the process
            // lists and that is exactly the work this call may not do. That
            // part still holds and nothing here does it. What changed is that
            // the follower case turned out to be a real field bug rather than
            // a hypothetical one: a follower wearing one of our attached
            // styles has her OWN hair re-shown by a build like this, and every
            // re-assert Fitting Room has hangs off a seam it drives itself, so
            // nothing was asking (user 2026-08-19, "when i change her hair and
            // exit i see her old hair and her new hair together").
            //
            // NoteEngineHeadBuild is atomics only and queues one task per
            // burst. The walking happens there, on the game thread, after the
            // build - which is the same posture RepaintNow below has.
            OS::NpcHair::NoteEngineHeadBuild();
            return;
        }

        // ⚠ QUEUED, NEVER DONE HERE. The head is mid-construction: the node
        // being painted is not necessarily attached to the actor's 3D yet, so a
        // walk from Get3D() would either miss it or read a tree the engine is
        // still writing. The task drains on the main thread after this frame's
        // work, which is after the whole rebuild rather than in the middle of
        // it. Queued from a HOOK and not from another task, so the same-pass
        // drain that makes a self-requeue hang cannot apply.
        if (g_queued.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([] { RepaintNow(); });
        } else {
            g_queued.store(false, std::memory_order_release);
        }
    }

}  // namespace

namespace OS::HeadBuildHook {

    void Install() {
        // The base-revert census first, independent of the painter's own id:
        // a diagnosis instrument, removed when round eight's reverter is
        // named. Membership-checked like every hand-held id here.
        if (OS::VersionCheck::IdOk(kChangeRace)) {
            const auto raceAddr = kChangeRace.address();
            g_raceHook = safetyhook::create_inline(
                reinterpret_cast<void*>(raceAddr),
                reinterpret_cast<void*>(&ChangeRaceDetour));
            if (g_raceHook) {
                spdlog::info("HeadBuildHook: ChangeRace census armed at 0x{:X} (id {}).",
                             raceAddr, kChangeRace.id());
            } else {
                spdlog::warn("HeadBuildHook: ChangeRace census could not install; the "
                             "revert hunt runs on the watcher alone.");
            }
        } else {
            spdlog::warn("HeadBuildHook: ChangeRace id {} is absent from this build's "
                         "Address Library; the revert census is off.",
                         kChangeRace.id());
        }

        // The dismember census, round nineteen's handoff instrument, and it
        // installs before the painter hook so an absent painter id cannot
        // take it down with it. Membership-checked like every id here.
        if (OS::VersionCheck::IdOk(kUpdateDismember)) {
            const auto disAddr = kUpdateDismember.address();
            // Foreign-detour report first (hook-entry-is-first-come-last-
            // served; read-foreign-hook-link rules if somebody is there).
            if (const auto sitting = JmpTargetAt(disAddr); sitting != 0) {
                spdlog::info("HeadBuildHook: UpdateDismemberPartion is ALREADY detoured "
                             "into {} ([{}]); installing on top, chaining into theirs.",
                             OS::HookSite::OwningModule(sitting), BytesAt(disAddr, 8));
            }
            g_dismemberHook = safetyhook::create_inline(
                reinterpret_cast<void*>(disAddr),
                reinterpret_cast<void*>(&UpdateDismemberPartionCensus));
            if (g_dismemberHook) {
                spdlog::info("HeadBuildHook: dismember census armed at 0x{:X} (id {}, "
                             "head-family slots, settle/FR-open/ladder windows, cap {}).",
                             disAddr, kUpdateDismember.id(), kDismemberCap);
            } else {
                spdlog::warn("HeadBuildHook: dismember census could not install; the "
                             "author hunt runs on the restore's own lines alone.");
            }
        } else {
            spdlog::warn("HeadBuildHook: UpdateDismemberPartion id {} is absent from "
                         "this build's Address Library; the dismember census is off.",
                         kUpdateDismember.id());
        }

        // The morph census pair, r66's instrument. Same doctrine as every
        // detour here: membership first, foreign-jump report, install-on-top.
        for (const auto& [id, hook, fn, what] :
             { std::tuple{ kUpdateNPCMorphs, &g_morphsHook,
                           reinterpret_cast<void*>(&UpdateNPCMorphsDetour),
                           "whole-face morph pass" },
               std::tuple{ kUpdateNPCMorph, &g_morphHook,
                           reinterpret_cast<void*>(&UpdateNPCMorphDetour),
                           "per-part morph pass" } }) {
            if (!OS::VersionCheck::IdOk(id)) {
                spdlog::warn("HeadBuildHook: the {} id {} is absent from this build's "
                             "Address Library; that half of the morph census is off.",
                             what, id.id());
                continue;
            }
            const auto addr = id.address();
            if (const auto sitting = JmpTargetAt(addr); sitting != 0) {
                spdlog::info("HeadBuildHook: the {} entry is ALREADY detoured into {} "
                             "([{}]); installing on top, chaining into theirs.",
                             what, OS::HookSite::OwningModule(sitting), BytesAt(addr, 8));
            }
            *hook = safetyhook::create_inline(reinterpret_cast<void*>(addr), fn);
            if (*hook) {
                spdlog::info("HeadBuildHook: {} detoured at 0x{:X} (id {}).", what,
                             addr, id.id());
            } else {
                spdlog::warn("HeadBuildHook: the {} at 0x{:X} refused the detour; "
                             "that half of the morph census is off.",
                             what, addr);
            }
        }

        // The model-morph census, r67, AE only (see the constant's comment
        // for why the 1.5.97 twin is not safely nameable).
        if (REL::Module::IsAE() && OS::VersionCheck::IdOk(kApplyModelMorphAE)) {
            const auto addr = REL::ID(kApplyModelMorphAE).address();
            if (const auto sitting = JmpTargetAt(addr); sitting != 0) {
                spdlog::info("HeadBuildHook: the model-morph entry is ALREADY detoured "
                             "into {} ([{}]); installing on top, chaining into theirs.",
                             OS::HookSite::OwningModule(sitting), BytesAt(addr, 8));
            }
            g_modelMorphHook = safetyhook::create_inline(
                reinterpret_cast<void*>(addr),
                reinterpret_cast<void*>(&ApplyModelMorphDetour));
            if (g_modelMorphHook) {
                spdlog::info("HeadBuildHook: model-morph census armed at 0x{:X} "
                             "(AE id {}, cap {} per load).",
                             addr, kApplyModelMorphAE, kModelMorphCap);
            } else {
                spdlog::warn("HeadBuildHook: the model-morph entry at 0x{:X} refused "
                             "the detour; that census is off.",
                             addr);
            }
        } else if (!REL::Module::IsAE()) {
            spdlog::info("HeadBuildHook: model-morph census is AE-only and this is SE; "
                         "the hunt's instrument set runs without it here.");
        }

        // The bases every census line resolves against, logged HERE on the
        // game thread where module lookups are allowed, and BEFORE the
        // painter's own membership check so an absent painter id cannot
        // silence them. The censuses log raw caller addresses; subtracting
        // these names the module offline. hdtSMP64 is in the list because
        // the wig is SMP and FSMP is a live suspect for late writes.
        spdlog::info("HeadBuildHook: census bases: SkyrimSE=0x{:X} skee64=0x{:X} "
                     "hdtSMP64=0x{:X} FittingRoom=0x{:X}.",
                     REL::Module::get().base(),
                     reinterpret_cast<std::uintptr_t>(GetModuleHandleA("skee64.dll")),
                     reinterpret_cast<std::uintptr_t>(GetModuleHandleA("hdtSMP64.dll")),
                     reinterpret_cast<std::uintptr_t>(GetModuleHandleA("FittingRoom.dll")));

        // ⚠ MEMBERSHIP FIRST, AND IT IS NOT THE SAME QUESTION AS "REL GAVE ME AN
        // ADDRESS". CommonLib's id2offset does a lower_bound and only fails when
        // an id runs past the END of the database, so an id that is simply
        // ABSENT resolves to its NEIGHBOUR silently - and detouring the
        // neighbour of the facegen painter would be a detour on an unrelated
        // engine function, called at an unrelated rate, with our code on the
        // far side of it.
        if (!OS::VersionCheck::IdOk(kPrepareHeadPart)) {
            spdlog::error("HeadBuildHook: the facegen hair painter id {} is ABSENT from this "
                          "build's Address Library, so a head build will go on silently "
                          "dropping the outfit's hair colour. The re-asserts at load, at "
                          "race switch and on the editor closing still run, so this is the "
                          "old behaviour rather than a broken one.",
                          kPrepareHeadPart.id());
            return;
        }

        const auto addr  = kPrepareHeadPart.address();
        const auto owner = OS::HookSite::OwningModule(addr);

        // ⚠ WHAT THE ENTRY LOOKED LIKE BEFORE WE TOUCHED IT, and it is the
        // difference between "we are first" and "we are on top of somebody".
        // Installing at plugin load put us first and a later plugin simply
        // overwrote our jump, so this line is what says whether moving the
        // install actually got us above them.
        if (const auto sitting = JmpTargetAt(addr); sitting != 0) {
            spdlog::info("HeadBuildHook: the entry is ALREADY detoured before us, into {} "
                         "([{}]). Installing on top, so our detour runs and chains into "
                         "theirs.",
                         OS::HookSite::OwningModule(sitting), BytesAt(addr, 8));
        } else {
            spdlog::info("HeadBuildHook: the entry is untouched ([{}]), so nothing else has "
                         "claimed the painter yet.",
                         BytesAt(addr, 8));
        }

        g_hook = safetyhook::create_inline(reinterpret_cast<void*>(addr),
                                           reinterpret_cast<void*>(&PrepareHeadPartForShaders));
        if (!g_hook) {
            spdlog::error("HeadBuildHook: could not detour the facegen hair painter at 0x{:X} "
                          "(currently owned by {}). The re-asserts still run.",
                          addr, owner);
            return;
        }

        g_installed.store(true, std::memory_order_relaxed);
        g_entry = addr;
        std::memcpy(g_entryAfterInstall, reinterpret_cast<const void*>(addr),
                    sizeof(g_entryAfterInstall));
        spdlog::info("HeadBuildHook: facegen hair painter detoured at 0x{:X} (id {}, entry "
                     "owned by {} before us). Entry now reads [{}] and jumps into {}.",
                     addr, kPrepareHeadPart.id(), owner, BytesAt(addr, 8),
                     OS::HookSite::OwningModule(JmpTargetAt(addr)));
    }

    void ReportEntryIntegrity() {
        if (!g_installed.load(std::memory_order_relaxed) || g_entry == 0) {
            spdlog::info("HeadBuildHook: nothing was installed, so there is no entry to "
                         "check. The painter was never a candidate on this runtime.");
            return;
        }
        const bool same = std::memcmp(g_entryAfterInstall,
                                      reinterpret_cast<const void*>(g_entry),
                                      sizeof(g_entryAfterInstall)) == 0;
        const auto target = JmpTargetAt(g_entry);
        // ⚠ ONE LINE CARRYING BOTH HALVES, because a field report is one
        // screenshot and the verdict is the pair: whether the bytes still match
        // AND which module the jump lands in. Ours and unchanged with no call
        // logged means the engine does not call this function here, so the
        // target is wrong. Changed, or landing in somebody else, means the
        // target may be right and we were displaced.
        spdlog::info("HeadBuildHook: entry integrity at 0x{:X}: bytes {} since install "
                     "([{}]), jump lands in {}. Calls seen: {}.",
                     g_entry, same ? "UNCHANGED" : "CHANGED BY SOMEBODY ELSE",
                     BytesAt(g_entry, 8),
                     target ? OS::HookSite::OwningModule(target)
                            : std::string{ "<no E9 at the entry at all>" },
                     g_hairBuilds.load(std::memory_order_relaxed));
    }

    bool          Installed() { return g_installed.load(std::memory_order_relaxed); }
    std::uint32_t HairBuilds() { return g_hairBuilds.load(std::memory_order_relaxed); }
    std::uint32_t Repaints() { return g_repaints.load(std::memory_order_relaxed); }

    void ResetLoadCensus() {
        g_censusLines.store(0, std::memory_order_relaxed);
        g_morphsLines.store(0, std::memory_order_relaxed);
        g_morphLines.store(0, std::memory_order_relaxed);
        g_modelMorphLines.store(0, std::memory_order_relaxed);
        g_hairLogged.store(0, std::memory_order_relaxed);
    }

    bool RebakeForeignPresetTintNow(const char* a_edge) {
        auto* const pc = RE::PlayerCharacter::GetSingleton();
        return pc && RebakeIfForeignPresetTintBound(pc, a_edge);
    }

}  // namespace OS::HeadBuildHook
