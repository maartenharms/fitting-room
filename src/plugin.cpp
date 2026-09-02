#include "PCH.h"

#include "ApparelPreviewSignal.h"
#include "AutoPresets.h"
#include "BipedHooks.h"
#include "BuildChannel.h"
#include "CbpcArmorClass.h"
#include "Collection.h"
#include "MenuStudioCompat.h"
#include "SamCompat.h"
#include "CrashGuard.h"
#include "Diagnostics.h"
#include "DyePalette.h"
#include "DyePromotion.h"
#include "DyeRules.h"
#include "DyeSchemes.h"
#include "DyeStats.h"
#include "DirectEntry.h"  // the key that enters the editor from gameplay
#include "DyeTexture.h"  // EnsurePresent, the requip driver's per-frame source
#include "DyeTick.h"
#include "Requip.h"  // the requip transition's driver (OS-206)
#include "DyeUnlockCard.h"
#include "DyeWorld.h"
#include "PreviewScopes.h"
#include "SharedDyeUnlocks.h"
#include "EditorWindow.h"
#include "DefaultBody.h"
#include "DefaultLook.h"
// ⚠ BOTH OF THESE REACH RACEMENU AND NEITHER IS UNDER THE FR_BODY_STUDIO GUARD
// BELOW. The Shape page ships on every channel and drives node transforms and
// body morphs alike, each under its own Fitting Room key, so gating either
// would mean a normal build shows the page with nothing behind it. Their
// Request() calls sit outside the guard to match.
#include "CharacterSex.h"  // Apply: the sex a look states, at the boundary the others use
#include "BipedPost.h"  // ArmHeadPartitionSettle: the load boundary's head builds re-run the engine's 131-off dance
#include "OverlayBaseline.h"  // Clear: the record belongs to the outgoing character
#include "NodeTransformApi.h"
#include "OverlayApi.h"
#include "AppearanceWatch.h"   // the one transition-logging appearance watchdog
#include "SkeeHookCensus.h"    // who owns the skee entries OverlayFix detours
#include "SculptProbe.h"      // the FOD pulse ladder the face arc still owes
#include "OverlayReconcile.h"  // OS-233: OnLoad and the fallback arm at kPostLoadGame
#include "MakeupApi.h"  // the held skin tone and the face rebake a load owes
#include "ProfileApply.h"  // the load boundary's mapped-preset erase (r61)
#include "SkinApi.h"
#include "OverlayLocations.h"
#include "RaceMenuMorphApi.h"
#include "Favorites.h"
#include "FsmpBridge.h"
#include "HeadEditorSink.h"
#include "HostGuard.h"
#include "ImGuiOverlay.h"
#include "InputListener.h"
#include "HeadBuildHook.h"   // detour the engine hair painter
#include "Inventory3DHooks.h"  // OS-154: hide the item preview at creation
#include "ItemCardCharge.h"  // OS-140 spike: the Seamstone's charge on its item card
#include "LoreModule.h"
#include "MenuButton.h"
#include "Migration.h"
#include "NpcHair.h"
#include "NpcHairSettle.h"
#include "ObodyApi.h"
#include "OutfitSession.h"
#include "Persistence.h"
#include "PresetStore.h"
#include "NpcLoadSink.h"
#include "RaceSwitchSink.h"
#include "RecentMods.h"
#include "RuleStore.h"
#include "SceneGuard.h"
#include "SeamstoneRecharge.h"  // OS-140: Skyrim's own Charge prompt fills the stone
#include "Settings.h"
#include "SettingsUI.h"
#include "StyleCatalog.h"
#include "VersionCheck.h"
#include "WeaponHooks.h"
#include "WorldWatch.h"

#if defined(FR_BODY_STUDIO)
#include "BodyPresetStore.h"
#include "FsmpXmlOverrides.h"
#endif

#if defined(FR_BODY_STUDIO)
#include "BodyStudioProof.h"
#endif

// fmt::join, for naming the rule keys a rules file locked. spdlog vendors fmt,
// and this shim resolves to the bundled copy or an external one depending on
// how spdlog was built, so it is the include that works either way. Naming the
// keys matters: a user with a locked tier and a log line saying only "1 rule
// rejected" has to bisect their own file to find it.
#include <spdlog/fmt/ranges.h>

namespace {
    // Keep in sync with project(... VERSION) in CMakeLists.txt and vcpkg.json; used only for the load log line.
    constexpr auto kVersion = "1.1.7";

    // Resolve where the log goes, and never fail silently.
    //
    // The old version asked CommonLib for the SKSE log directory and simply
    // RETURNED if it got nothing back - the mod then ran with no log at all and
    // no way to tell. That is what happened on AE 1.6.1170 under MO2 on
    // 2026-07-18: skse64.log recorded "plugin FittingRoom.dll (... 00020000)
    // loaded correctly" while no FittingRoom.log was written anywhere on disk,
    // which cost a debugging session and blocked the field test.
    //
    // Two things in SKSE::log::log_directory() can produce that: the Documents
    // known-folder lookup can fail outright (-> nullopt), and it distinguishes a
    // Steam install from a GOG one by probing for "steam_api64.dll" with a
    // RELATIVE path - i.e. against the process working directory, not the game
    // folder - so any launcher whose CWD is elsewhere silently redirects us to a
    // "Skyrim Special Edition GOG" path. A diagnostic you cannot rely on is
    // worse than no diagnostic, so try each candidate in turn, create the
    // directory first (a missing one must not throw), never let a throwing sink
    // escape, and state which candidate won on the first line.
    //
    // The fallback is Data/SKSE/Plugins next to the DLL: MO2 maps that to
    // Overwrite, and several mods in a normal load order already log there, so
    // it is proven writable in exactly the setup where the primary path failed.
    // ⚠⚠ THE ONLY INSTRUMENT THAT CAN NAME A 0xC0000409, AND A WEEK OF SILENT
    // CRASHES IS WHY IT EXISTS. Windows recorded eighteen SkyrimSE faults
    // between 2026-08-21 and 2026-08-28, every one of them ucrtbase.dll,
    // exception 0xC0000409, fault offset 0x7F6FE, and Trainwreck logged none of
    // them while it logged the ordinary 0xC0000005 faults sitting beside them in
    // the same list. That is not a gap in the crash logger. 0xC0000409 is
    // STATUS_STACK_BUFFER_OVERRUN, the CRT's fail-fast, and a fail-fast goes
    // straight to Windows Error Reporting WITHOUT passing through the
    // unhandled-exception filter that every crash logger hooks. Nothing hooking
    // that filter can ever see one.
    //
    // ⚠ SO THESE CATCH IT ONE STEP EARLIER, at the two places the CRT decides
    // to fail fast: an uncaught C++ exception reaching terminate, and an invalid
    // parameter handed to a CRT function. Both handlers are PROCESS-WIDE, so
    // this names any plugin's fault and not only ours. If the next report says
    // the exception came from somewhere else entirely, that is the instrument
    // working rather than a wrong answer.
    //
    // ⚠⚠ THE FLUSH IS THE WHOLE POINT. The process is about to die, and a
    // line sitting in a buffer is a line nobody reads. spdlog is already set to
    // flush on debug, so this is belt and braces, and it costs nothing on a path
    // that runs once ever.
    //
    // ⚠ A HANDLER THAT THROWS IS A SECOND CRASH ON TOP OF THE FIRST, which is
    // why both swallow everything. And terminate hands on to whoever was there
    // before us rather than replacing them: last writer wins on this hook, so
    // another plugin may well have set one, and eating its handler would trade
    // one silence for another.
    std::terminate_handler g_previousTerminate{ nullptr };

    [[nodiscard]] std::string NarrowOrNone(const wchar_t* a_text) {
        if (!a_text || !*a_text) {
            return "(none)";
        }
        const auto u8 = std::filesystem::path{ a_text }.u8string();
        return std::string{ reinterpret_cast<const char*>(u8.data()), u8.size() };
    }

    void OnTerminate() {
        try {
            std::string what = "terminate was called with no active exception";
            if (const auto active = std::current_exception()) {
                try {
                    std::rethrow_exception(active);
                } catch (const std::exception& e) {
                    what = e.what() ? e.what() : "a std::exception with no message";
                } catch (...) {
                    what = "an exception that does not derive from std::exception";
                }
            }
            spdlog::critical("FAIL-FAST: {}", what);
            if (const auto logger = spdlog::default_logger()) {
                logger->flush();
            }
        } catch (...) {
        }
        if (g_previousTerminate && g_previousTerminate != &OnTerminate) {
            g_previousTerminate();
        }
        std::abort();
    }

    // ⚠ THE RELEASE CRT HANDS THIS HANDLER NOTHING, and that is expected
    // rather than a bug: expression, function and file are all null unless the
    // debug CRT is in play. The line still earns its place, because "a CRT
    // invalid parameter fired" is a different fault from "an exception escaped"
    // and the two are indistinguishable from the Windows event log alone.
    void OnInvalidParameter(const wchar_t* a_expression, const wchar_t* a_function,
                            const wchar_t* a_file, unsigned int a_line, std::uintptr_t) {
        try {
            spdlog::critical("FAIL-FAST: the CRT refused a parameter. expression {}, "
                             "function {}, file {}, line {}.",
                             NarrowOrNone(a_expression), NarrowOrNone(a_function),
                             NarrowOrNone(a_file), a_line);
            if (const auto logger = spdlog::default_logger()) {
                logger->flush();
            }
        } catch (...) {
        }
    }

    void InstallFaultHandlers() {
        g_previousTerminate = std::set_terminate(&OnTerminate);
        _set_invalid_parameter_handler(&OnInvalidParameter);
        spdlog::info("fault handlers installed: an uncaught exception or a refused CRT "
                     "parameter now names itself here before the process dies. Windows "
                     "has recorded this crash as ucrtbase 0xC0000409 since 2026-08-21 and "
                     "no crash logger can see it, because a fail-fast skips their hook.");
    }

    void SetupLog() {
        const auto logName     = OS::BuildChannel::LogName();
        const auto prevLogName = OS::BuildChannel::PreviousLogName();
        struct Candidate {
            std::filesystem::path dir;
            const char*           what;
        };

        std::vector<Candidate> candidates;
        if (auto dir = SKSE::log::log_directory()) {
            candidates.push_back({ *dir, "SKSE log directory" });
        }
        std::error_code ec;
        if (auto cwd = std::filesystem::current_path(ec); !ec) {
            candidates.push_back({ cwd / "Data" / "SKSE" / "Plugins", "Data/SKSE/Plugins fallback" });
        }

        for (const auto& candidate : candidates) {
            std::error_code mkdirEc;
            std::filesystem::create_directories(candidate.dir, mkdirEc);  // best effort; the open decides

            // ⚠ KEEP THE PREVIOUS RUN. The sink below opens with truncate, so
            // launching the game destroys the log of the run before it. That is
            // exactly backwards after a crash: the first thing anyone does is
            // relaunch, and relaunching is what deletes the evidence. It cost a
            // real diagnosis on 2026-08-05, where the run that produced a
            // trainwreck dump had already been overwritten by the time the log
            // was read.
            //
            // Rename rather than copy, and ignore every failure: the old file
            // may not exist yet, or may still be held by a previous process,
            // and neither is a reason to start the game without a log.
            std::error_code rotateEc;
            std::filesystem::rename(candidate.dir / logName, candidate.dir / prevLogName,
                                    rotateEc);
            try {
                auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                    (candidate.dir / logName).string(), true);
                auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
                logger->set_level(spdlog::level::debug);
                logger->flush_on(spdlog::level::debug);

                spdlog::set_default_logger(std::move(logger));
                spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
                // ⚠ AFTER THE SINK, NOT BEFORE IT. A handler that fires with no
                // logger behind it writes into the void, which is the state
                // this whole instrument exists to end.
                InstallFaultHandlers();
                // First line, every run: names the directory that won and how we
                // got there, so "the log is missing" is one glance from an answer.
                spdlog::info("log: writing to '{}' ({}).", candidate.dir.string(), candidate.what);
                return;
            } catch (const std::exception&) {
                // Try the next candidate. Nothing else in the plugin needs a
                // sink, so exhausting them leaves spdlog's default logger and
                // the mod still loads - quietly, but it loads.
            }
        }
    }

    // One promotion pass: gather the world, walk the palette, add what is now
    // earned. MAIN THREAD ONLY.
    //
    // ⚠ Returns nothing on purpose. It handed back the gained count until
    // 2026-08-02, the one caller dropped it, and it was not [[nodiscard]], so
    // the value was pure decoration. Everything worth knowing about a pass is
    // in the log below, where the person who needs it is actually looking.
    // The account-wide unlock file, reconciled with THIS character (OS-198).
    //
    // ⚠⚠ HERE, AND NOT IN Persistence::LoadCallback, WHICH IS WHERE THE PLAN
    // FOR THIS SAID IT WOULD GO. LoadCallback never fires for a genuinely new
    // game, because there is no co-save to read, and a brand new character is
    // the headline case for the entire feature: it would have worked on every
    // save except the one that matters, and looked correct doing it. This
    // handler covers both paths, because kNewGame falls through to
    // kPostLoadGame, and it is the same reason RunDyePromotion below already
    // lives here rather than at kDataLoaded.
    //
    // ⚠ AHEAD OF PROMOTION AND OUTSIDE ITS HEALTH GATE. Promotion refuses to
    // run off a rules file that would not parse, because absent keys read as
    // "no requirement" and unlocks are add only. That reasoning does not reach
    // this: the file holds ids EARNED elsewhere, under whatever rules were
    // healthy at the time, and nothing here consults a rule at all. A player
    // whose rules broke today should still be offered the colours another
    // character already owns.
    //
    // ⚠ THE BODY MOVED TO Persistence::SyncSharedDyeUnlocks and grew a publish
    // half. It could not stay in SharedDyeUnlocks.cpp, which a test compiles
    // with no engine, and the settings panel needs to reach it too: turning the
    // setting on used to do nothing at all until the next load, silently.

    void RunDyePromotion() {
        // ⚠ REFUSE ON A BROKEN RULES FILE. A file that would not parse, or a
        // directory that could not be read, contributes none of its keys, so
        // every dye it covered falls through to "no rule authored", which means
        // free. One stray comma in eso.json would write all 306 ids into the
        // player's save, permanently, because unlocks are add only.
        //
        // Freezing progression is the reversible failure and it is loud in the
        // log. Handing out the whole palette is neither.
        //
        // ⚠ ONE acquisition, and the API is shaped so there is no other way.
        // Asking Healthy() and then taking a snapshot is two, and a Load
        // landing between them hands this pass the trust bit from before a
        // reload with the rules from after a FAILED one, which grants off a set
        // it was told to believe. Not reachable while both callers are main
        // thread only; it becomes reachable the moment the pane takes its own
        // snapshot, which is already anticipated.
        //
        // SnapshotUnchecked() is the other one, and its name is the warning.
        const auto checked = OS::DyeRules::SnapshotChecked();
        if (!checked.healthy) {
            // Said again here, not only at kDataLoaded. This runs on every save
            // load, and a user reading the log around the load that surprised
            // them would otherwise see nothing at all.
            spdlog::warn("Dye unlocks: promotion SKIPPED this load, because the "
                         "rules did not all load cleanly. Nothing new is "
                         "earned; nothing already earned is lost. See the dye "
                         "rules errors logged at startup.");
            return;
        }
        const auto& rules = checked.rules;
        // ⚠ Gather takes the unlock set rather than fetching it, and the
        // snapshot is taken HERE, outside the With below, on purpose. Moving
        // the Gather inside the With is now safe, which it was not before the
        // parameter existed, but it would hold the unlock lock across eighteen
        // virtual actor value reads and every quest form lookup. That lock is
        // the one the dye pane's draw loop will take, and this project already
        // walked g_lock back out from around the co-save write for the same
        // reason.
        //
        // It buys nothing either. Deeds are written on the main thread and
        // promotion runs on the main thread, so there is no writer that can
        // move the counters between this line and the With.
        const auto world =
            OS::DyeWorld::Gather(rules, OS::DyeUnlocks::Snapshot());

        std::vector<OS::DyePromotable> palette;
        for (const auto& dye : OS::DyePalette::Snapshot()) {
            palette.push_back({ dye.id, dye.rarity });
        }

        OS::PromotionResult result;
        std::size_t         held = 0;
        OS::DyeUnlocks::With([&](OS::DyeUnlockSet& a_set) {
            result = OS::Promote(palette, rules, world, a_set);
            held   = a_set.Size();
        });
        // ⚠ A LINE EITHER WAY. Only the gained case said anything until
        // 2026-08-02, so a pass that ran and earned nothing was character for
        // character indistinguishable from a pass that never ran at all. That
        // is the difference the field test is trying to see, and a human runs
        // it off this log.
        if (result.gained.empty()) {
            spdlog::info("Dye unlocks: promotion ran, nothing newly earned, {} "
                         "held.",
                         held);
        } else {
            // ⚠ NAMED, for the reason the editor-open pass's twin says: a count
            // cannot tell a gate opening apart from two passes reading
            // different worlds, and on 2026-08-14 those two disagreed.
            std::string named;
            for (const auto& id : result.gained) {
                if (!named.empty()) {
                    named += ", ";
                }
                named += id;
            }
            spdlog::info("Dye unlocks: {} newly earned, {} held: {}.",
                         result.gained.size(), held, named);
        }
        // ⚠ TELL THE PLAYER, AND LET THE CARD DECIDE WHETHER TO SPEAK.
        // Announce is deliberately called from EVERY pass, including the two
        // this load runs before anybody is watching: it absorbs them as the
        // baseline while it is unarmed, which is what stops a returning player
        // being met with a stack of cards for last session's colours. Putting
        // the decision there rather than here means there is one rule about it
        // instead of one per call site.
        //
        // ⚠ THE TWO LOAD PASSES CANNOT DOUBLE-ANNOUNCE ANYTHING even once
        // armed, and that is by construction rather than by coalescing:
        // promotion adds only what is NOT already held, so the second pass can
        // only ever gain what the first could not see. The handoff's worry was
        // about a single "N new dyes" card firing twice with two different Ns,
        // which one card per colour does not have.
        OS::DyeUnlockCard::Announce(result.gained);
        // ⚠ A FREE-DYE WARNING, and this is the only place that can raise it:
        // nothing else holds the palette and the rule set at the same time. A
        // rarity no tier matched resolves to no conditions, and no conditions
        // means the dye is handed out at level 1 and written permanently into
        // the save.
        //
        // Not fatal, deliberately. A third party pack may ship a bucket this
        // mod never heard of, and an unknown rarity staying free is the
        // documented fallback. It just must not be silent, because the same
        // report is what a mistyped tier name in a hand authored file looks
        // like, and that one frees a whole bucket.
        if (!result.unmatchedRarities.empty()) {
            std::string named;
            for (const auto& [rarity, count] : result.unmatchedRarities) {
                if (!named.empty()) {
                    named += ", ";
                }
                named += "\"" + rarity + "\" (" + std::to_string(count) +
                         " dye(s))";
            }
            spdlog::warn("Dye unlocks: no rule and no tier matched {}. Those "
                         "dyes have no requirement, so they are FREE. Rarity "
                         "names are matched exactly, capitals and spacing "
                         "included, so check them against the tier names in "
                         "SKSE/Plugins/FittingRoom/Unlocks.",
                         named);
        }
        // ⚠ THE ORPHANED-OVERRIDE WARNING, and the rarity report above cannot
        // stand in for it. A per-dye key no colour claims merges cleanly and
        // drops its dye onto the RARITY TIER, which exists, so Covers answers
        // true and the report above stays empty. An orphan loosens a gate
        // rather than locking one, which is why this is a warning and not a
        // freeze; thirteen of the 220 sit on Common, which is free at level 1
        // and permanent once written. tools/check_dye_rules.py is what keeps
        // "loosens rather than locks" true on the shipped file, and
        // DyePromotion.h says how far that check reaches.
        //
        // Named, not just counted, and not fatal: a rules pack covering a
        // colour pack the user has not installed is legitimate, so this reads
        // as a thing to check rather than a thing to fix.
        if (!result.unmatchedDyeRules.empty()) {
            constexpr std::size_t kMaxNamedKeys = 20;
            std::string           named;
            const auto&           keys = result.unmatchedDyeRules;
            for (std::size_t i = 0; i < keys.size() && i < kMaxNamedKeys; ++i) {
                if (!named.empty()) {
                    named += ", ";
                }
                named += "\"" + keys[i] + "\"";
            }
            if (keys.size() > kMaxNamedKeys) {
                named += ", and " +
                         std::to_string(keys.size() - kMaxNamedKeys) + " more";
            }
            spdlog::warn("Dye unlocks: {} rule(s) name a dye id no installed "
                         "colour claims: {}. Those rules do nothing, and any "
                         "dye they were meant to gate falls back to its rarity "
                         "tier, which is usually looser. Ids are matched "
                         "exactly. Expected if the rules cover a colour pack "
                         "that is not installed; a typo otherwise.",
                         keys.size(), named);
        }
    }

    // Ask Skyrim for every misc stat the rules gate on, and run promotion again
    // when the answers land.
    //
    // ⚠ SEPARATE FROM RunDyePromotion RATHER THAN INSIDE IT, and that is what
    // stops this recursing forever. The settle callback calls the promotion
    // pass; if the promotion pass also asked for stats, every pass would ask
    // for stats and every answer would start another pass. One asker, one
    // re-run, and the re-run is a plain promotion.
    void RequestDyeStats() {
        // ⚠ THE SAME HEALTH GATE PROMOTION USES. A rules file that would not
        // parse contributes no keys, and dispatching off it would ask for a
        // list of stats that does not describe the rules anybody is being
        // graded against. Cheap, and it keeps the two passes reading the same
        // world.
        const auto checked = OS::DyeRules::SnapshotChecked();
        if (!checked.healthy) {
            return;
        }
        const auto names = checked.rules.StatNames();
        if (names.empty()) {
            // Nothing gates on a Skyrim counter, which is a legitimate state
            // for a third party rules pack and for a user who deleted ours. No
            // dispatch, no second pass, no line: the first pass already saw
            // everything there was to see.
            return;
        }
        OS::DyeStats::Request(names, []() {
            // ⚠ MAIN THREAD. DyeStats marshals this through SKSE's task
            // interface precisely so this line may do what it does.
            spdlog::info("Dye stats: answers are in, so promotion runs a second "
                         "time. Anything it earns now was gated on a counter "
                         "the first pass could not read yet.");
            RunDyePromotion();
        });
    }

    void OnMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) {
            return;
        }
        switch (a_msg->type) {
            case SKSE::MessagingInterface::kPostLoad: {
                // Apparel Preview (same studio) tells us when a hover preview
                // is on the player, so our GetWornMask shim can stand down for
                // the duration and our dye walk can skip the slots the preview
                // is standing on. See ApparelPreviewSignal.h for the whole
                // reason - in short, both mods shim the same mask and the
                // union of two honest answers culls the head, and the dye walk
                // paints by channel index into a slot whose geometry AP has
                // replaced.
                //
                // ⚠ TWO TYPES, AND AN UNRECOGNISED ONE IS DROPPED IN SILENCE.
                // 'APSM' arrives immediately before its matching 'APPV' and
                // carries the slot mask; an Apparel Preview older than that
                // sends only 'APPV' and this listener never sees the other.
                // Dropping the unknown is what lets AP add a third message
                // later without needing us to ship the same day.
                //
                // Sender-named registration only resolves from kPostLoad on,
                // which is the same point AP registers its listener for us.
                // Failing is normal and silent-ish: it just means AP is not
                // installed, and the shim keeps its full behaviour.
                if (auto* const messaging = SKSE::GetMessagingInterface()) {
                    const bool ok = messaging->RegisterListener(
                        "ApparelPreview", [](SKSE::MessagingInterface::Message* a_m) {
                            if (!a_m) {
                                return;
                            }
                            if (a_m->type == OS::kPreviewSlotsMsg) {
                                OS::SetApparelPreviewSlots(a_m->data, a_m->dataLen);
                            } else if (a_m->type == OS::kPreviewStateMsg) {
                                OS::SetApparelPreviewActive(a_m->data, a_m->dataLen);
                            }
                        });
                    if (!ok) {
                        spdlog::info("Apparel Preview not present - worn-mask shim stays fully "
                                     "active. Normal unless you run both mods.");
                    }
                }
                break;
            }
            case SKSE::MessagingInterface::kDataLoaded:
                // ⚠⚠ HERE, AND NOT IN SKSEPluginLoad WITH EVERY OTHER HOOK,
                // BECAUSE WE LOSE THE ENTRY THERE. Installed at plugin load,
                // Fitting Room was FIRST onto the facegen hair painter and
                // measured its own E9 in place - and by the time a save was
                // loaded the entry read FF 25 (the six-byte absolute form
                // Detours writes), our jump was gone and the detour had been
                // called exactly zero times (field 2026-08-12). Being first is
                // worth nothing when the entry is a first-come, last-served
                // resource. kDataLoaded runs after every plugin has loaded, so
                // this installs ON TOP of whoever took it, and SafetyHook
                // relocates their jump into our trampoline so theirs still
                // runs.
                //
                // ⚠ It logs what was sitting on the entry before it patched,
                // which is the only way to tell "we are on top now" from "we
                // lost the race again".
                OS::HeadBuildHook::Install();
                // Same reason, one step further: kDataLoaded is the first
                // moment every plugin that could have patched skee has
                // done it, so this is when reading those entries answers
                // who owns them rather than who got there early.
                OS::SkeeHookCensus::Run();
                // "Fitting Room" rename: move user data from the old OutfitSlots path
                // before anything reads it (Settings INI, then the library below).
                OS::Migration::RunOnce();
                OS::Settings::GetSingleton().Load();
                OS::AppearanceWatch::Start();
                OS::OutfitSession::GetSingleton().SetBlocklist(
                    OS::Settings::GetSingleton().slotBlocklist);
#if defined(FR_BODY_STUDIO)
                // Resolve custom outfit references before loading the library
                // can queue its first actor refresh.
                OS::BodyPresetStore::GetSingleton().Load();
                OS::FsmpXmlOverrides::Load();
#endif
                // Global outfit library (persists across saves) - before any
                // save load so legacy co-save migration sees the right state.
                OS::Persistence::LoadLibraryFileAtStartup();
                // Before the catalog: promote any style whose preview crashed
                // last session to the crashers list so Build/RefreshFit flag it.
                OS::CrashGuard::LoadAtStartup();
                // Fingerprint hdtsmp64.dll (if loaded) against the known-build
                // table, before anything decides whether SMP hair can take
                // FSMP's cooperative head path this session.
                OS::FsmpBridge::Init();
                // CBPC classes the body physics by the REAL worn chest piece;
                // create the override keywords and start following the SHOWN
                // one (r55: iron under a cloth look froze the breasts).
                OS::CbpcArmorClass::Install();
                // Hair styles measured as unbindable to a follower (bone
                // limit / unbound bones / partition map) - load-order-scoped,
                // so a declined style greys without its dud attach this time.
                OS::NpcHair::LoadUnsupportedAtStartup();
                // Starred looks (global, load-order independent) - read before
                // the editor can open; the browser filter reads this set.
                OS::Favorites::LoadAtStartup();
                // ⚠ THE CHARACTER DEFAULTS ARE NOT READ HERE ANY MORE. They were
                // global files loaded beside the other global stores; since
                // 2026-08-08 they are per-save co-save records ('DFLK', 'DFBD')
                // and Persistence's load callback owns them, because a default
                // costs a look and the currency it costs lives in the save. See
                // DefaultLook.h for the exploit that forced the move.
                //
                // Loading them here would be worse than redundant: it would put
                // one save's defaults in the map before another save's records
                // were read, which is the leak in a different costume.
                // Last launch's plugin set - Build() diffs against it to flag
                // styles from newly-added mods (OS-26), then re-baselines.
                OS::RecentMods::LoadAtStartup();
                OS::StyleCatalog::GetSingleton().Build();
                OS::PresetStore::GetSingleton().Load();
                // Saved dye schemes - global files beside the presets, read
                // here for the same reason: the editor draws off a snapshot on
                // the render thread and must never touch the disk itself.
                OS::DyeSchemes::Load();
                // The curated dye palette, read here for the same reason the
                // schemes are: the editor draws off a snapshot on the render
                // thread and must never touch the disk itself.
                {
                    const auto report =
                        OS::DyePalette::Load(OS::Settings::GetSingleton().dyeCost);
                    for (const auto& skip : report.fileSkips) {
                        spdlog::warn("DyePalette: SKIP {}.", skip);
                    }
                    spdlog::info("DyePalette: {} dye(s) loaded from {} file(s) scanned.",
                                 report.dyesAccepted, report.filesScanned);
                    if (report.entriesRejected > 0) {
                        spdlog::warn("DyePalette: {} entrie(s) rejected as malformed "
                                     "(bad id, name, or hex).",
                                     report.entriesRejected);
                    }
                    if (report.idCollisions > 0) {
                        spdlog::warn("DyePalette: {} dye id collision(s) refused. Packs "
                                     "load in sorted filename order, so the first pack "
                                     "to claim an id keeps it.",
                                     report.idCollisions);
                    }
                    // ⚠ The colour was KEPT and its tier was not, which is the
                    // right trade and is not a free pass. A dye with no rarity
                    // matches no tier, and no tier means no requirement, so
                    // every one of these is unlocked at level 1. A dye that
                    // simply omits "rarity" is a different thing, is
                    // deliberate, and is not counted here.
                    if (report.raritiesDropped > 0) {
                        spdlog::warn("DyePalette: {} dye(s) wrote a \"rarity\" that is "
                                     "not a string, so it was dropped and the colour "
                                     "kept. Those dyes have no tier, which the unlock "
                                     "rules read as FREE.",
                                     report.raritiesDropped);
                    }
                }
                // Unlock rules, read here for the same reason the palette is:
                // the editor draws off a snapshot on the render thread and
                // must never touch the disk itself.
                //
                // ⚠ The report is a LOCAL. Promotion takes the gate and the
                // rules together through DyeRules::SnapshotChecked rather than
                // off a remembered copy, so there is no file scope report here
                // and no way for the gate to drift out of step with the data.
                // The preview scene fixups (OS-191). Disk work, so it belongs
                // here beside the other loads and not on the render thread,
                // which draws off a snapshot. Loaded whether or not the grid is
                // ever opened, because it is a few small files and a lazy load
                // would put a first-open stall on the one path that is already
                // the slowest.
                static_cast<void>(OS::PreviewScopes::Load());
                // Which overlay art belongs on which location, generated
                // offline from what each pack registers with RaceMenu. Beside
                // the other loads for the fixups' reason: it is one small file
                // and the picker would otherwise stall on first open.
                //
                // ⚠ IT REPORTS RATHER THAN LOGGING, so the line is written
                // here. OverlayLocations.cpp is compiled into a pure-logic test
                // as well as into the DLL and spdlog reaches only one of them.
                {
                    const auto table = OS::OverlayLocations::Load();
                    if (table.loaded) {
                        spdlog::info("OverlayLocations: {}", table.diagnostic);
                    } else {
                        // ⚠ NOT AN ERROR, AND THE LINE SAYS WHAT IS LOST. With
                        // no table the picker still works and shows MORE art
                        // rather than less, so this is a note about filtering
                        // quality and not a fault.
                        spdlog::info("OverlayLocations: {} The Overlays picker will offer "
                                     "every texture on every location unless a file name "
                                     "places it.",
                                     table.diagnostic);
                    }
                }
                {
                    const auto dyeRules = OS::DyeRules::Load();
                    if (dyeRules.filesScanned == 0 && dyeRules.filesFailed > 0) {
                        // ⚠ SCANNED ZERO AND STILL BROKEN, which is four
                        // separate routes and every one of them used to print
                        // the reassuring line below instead. filesScanned only
                        // counts files that reached the read loop, and the
                        // directory walk fails an entry without ever adding it
                        // to that list: exists() answering with an error, a
                        // dangling junction on Unlocks itself, a scan that
                        // failed at construction or mid increment, and an entry
                        // named *.json that is not a regular file.
                        //
                        // All four are exactly what an earlier commit's
                        // error_code work exists to catch, so telling the user
                        // every colour is free here undid half of that fix's
                        // value. Nothing is free in this state: promotion is
                        // frozen by the check further down, and a user reading
                        // top to bottom was being told the opposite first.
                        spdlog::error("Dye rules: the Unlocks directory could "
                                      "not be read and nothing was loaded. "
                                      "This is NOT the same as having no rules; "
                                      "see the error(s) below.");
                    } else if (dyeRules.filesScanned == 0) {
                        // ⚠ A SUPPORTED STATE, not a failure, and it is worth
                        // one line anyway. No rules at all means no dye has a
                        // requirement, so the whole palette is free. That is
                        // deliberate, because a pack that shipped colours
                        // before this economy existed must not have them
                        // silently locked; it is also what an Unlocks folder
                        // someone emptied looks like, and the two are
                        // indistinguishable from in here.
                        spdlog::info("Dye rules: no rules files found, so no "
                                     "dye has a requirement and every colour "
                                     "is free.");
                    } else {
                        spdlog::info("Dye rules: {} file(s) scanned.",
                                     dyeRules.filesScanned);
                    }
                    if (dyeRules.rulesRejected > 0) {
                        spdlog::warn("Dye rules: {} rule(s) rejected and LOCKED: {}",
                                     dyeRules.rulesRejected,
                                     fmt::join(dyeRules.rejectedKeys, ", "));
                    }
                    // ⚠ NOT a failure list and must not be logged as one. A
                    // present but empty section is a legitimate thing to
                    // author, so it freezes nothing, but it does leave every
                    // dye that section covered free and it looks exactly like a
                    // section a bad edit emptied. Say it, do not alarm over it.
                    for (const auto& note : dyeRules.notes) {
                        spdlog::info("Dye rules: {}", note);
                    }
                    for (const auto& failure : dyeRules.failures) {
                        spdlog::error("Dye rules: {}", failure);
                    }
                    if (dyeRules.filesFailed > 0) {
                        spdlog::error("Dye rules: {} file(s) failed to load, so dye "
                                      "unlocks are FROZEN this session. Fix the "
                                      "file(s) and reload. Nothing new will be "
                                      "earned until then, and nothing already "
                                      "earned is lost.",
                                      dyeRules.filesFailed);
                    }
                }
                OS::Diagnostics::WarnOnConflicts();
                OS::Collection::GetSingleton().Register();
                OS::ImGuiOverlay::GetSingleton().RegisterMenuGuard();
                OS::MenuButton::Register();
                OS::LoreModule::Init();
                // OS-206: the requip aura's two records live in the same ESP,
                // so they resolve beside it and on the same terms. Missing ones
                // are logged and the flourish runs without them.
                OS::Requip::ResolveForms();
                // AFTER LoreModule::Init and after Settings::Load above: the
                // hook compares against the Seamstone form and reads its own
                // debug key, and both have to exist before it goes in. The
                // trampoline was allocated in SKSEPluginLoad.
                // ⚠ THE RECHARGE GOES IN FIRST AND THE ORDER IS A SAFETY
                // RULE, not a preference. ItemCardCharge refuses to stamp a
                // charge onto the card unless SeamstoneRecharge::Installed()
                // is already true, because the stamp is what makes Skyrim
                // offer its Charge prompt and the engine eats a soul gem the
                // moment it is used. Install them the other way round and the
                // stamp reads a false and silently disables itself.
                OS::SeamstoneRecharge::Install();
                OS::ItemCardCharge::Install();
                // Scene coexistence (OStim etc.) - listens for scene mod-events
                // to suspend transmog; reads its config from Settings::Load above.
                OS::SceneGuard::Init();
                // Race-switch suspension (vampire lord, werewolf, etc.) - the
                // player path (OS-65 audit gap) and the per-actor NPC path.
                OS::RaceSwitchSink::Register();
                // OS-109. Every follower-side write lands on live geometry, so
                // a rebuilt 3D comes back vanilla. Re-apply when it does; `coc`
                // is the fastest repro but fast travel, load doors and ordinary
                // streaming all did it too.
                OS::NpcLoadSink::Register();
                // Head-editor coexistence: the character editor repaints the
                // hair from the actor base on its way in, so an active outfit
                // colour is re-asserted when it closes.
                OS::HeadEditorSink::Register();
                // In-game config panel (FUCK sidebar tool, soft dependency).
                OS::SettingsUI::Register();
                // The editor as a FUCK IWindow (Phase 3). Must follow
                // SettingsUI::Register (which calls FUCK::Connect).
                OS::EditorWindow::Register();
                // The unlock cards, as a second FUCK IWindow. Same requirement
                // (FUCK::Connect must have run) plus one of its own: it asks
                // EditorWindow::IsOpen every frame to stay silent over the
                // editor, so it registers after it.
                OS::DyeUnlockCard::Register();
                // The invisible settle ticker for styled followers, a third
                // FUCK IWindow: NpcHair's post-apply ladders need a gameplay
                // frame to drain from with the editor shut (the cell-return
                // doubled hair, 2026-08-21). Same FUCK::Connect requirement
                // as its two neighbours.
                OS::NpcHairSettle::Register();
                // ⚠ THE PASS THE GAMEPLAY TICK RUNS, INSTALLED HERE BECAUSE
                // BOTH HALVES LIVE IN THIS FILE. It is the same pair the load
                // path runs a few cases down, and deliberately so: promotion
                // for everything readable now, then the stats request for the
                // counters that answer ~180 ms late and re-runs promotion when
                // they land. A tick that only did the first would never grant a
                // counter-gated colour during play, which is most of what the
                // 08-13 economy stint moved behind gates.
                OS::DyeTick::SetPass([] {
                    RunDyePromotion();
                    RequestDyeStats();
                });
                // Mod-event bridge: OutfitSlots_Open/_Close/_Toggle (the SAM
                // addon and any mod can open the editor via SendModEvent).
                OS::SamCompat::Register();
                // One "Fitting Room" button on Menu Studio's action bar, when
                // MS is present and new enough to have one. After EditorWindow
                // for the same reason as HostGuard: the click asks IsOpen.
                OS::MenuStudioCompat::Register();
                // Close the editor if the menu hosting it goes away, and release
                // the world camera if it is ever held while the editor is not
                // open. Registered after EditorWindow so IsOpen is answerable.
                OS::HostGuard::Register();
                // The key that enters the editor from gameplay (OS-207): its
                // menu sink, which opens the editor once a summoned host is up.
                // After EditorWindow for HostGuard's reason, that it asks
                // IsOpen. Before InputListener, which is what compares the key.
                OS::DirectEntry::Register();
                OS::InputListener::Register();
                // skee's overlay install callback. It carries OS-233's trigger
                // (RaceMenu's post-load pass reinstalls the player's Face right
                // after it takes the body clones, and that fire is what arms the
                // repair), so it is registered on every run now; the OS-226 S0
                // probe still reads the same fires when its key is bound. Here
                // rather than beside OverlayApi::Request because Request runs at
                // kPostPostLoad, before Settings::Load, and the probe half asks
                // an INI question.
                OS::OverlayApi::RegisterInstallCallback();
                // Author rule packs (Data/SKSE/Plugins/FittingRoom/Rules/*.json),
                // read before WorldWatch::Init() starts evaluating so the very
                // first pass already sees them merged in.
                OS::RuleStore::ScanPacks();
                // The rules engine's own sinks (menu/combat/death/equip) and
                // heartbeat. Everything up to here only builds the pieces;
                // this is what makes the feature actually run.
                OS::WorldWatch::Init();
                break;
            case SKSE::MessagingInterface::kPreLoadGame:
                // A load with the editor nominally open strands the camera on a
                // follower, and the subject is held as a reference FormID, so it
                // would RE-RESOLVE in the new save rather than going stale.
                OS::HostGuard::OnPreLoadGame();
                // OS-206: and take any requip flourish down while the 3D it
                // recorded still exists. Same reason as NpcHair below: this
                // message is the last point at which it does, and a restore
                // that arrives after the load has nothing honest to write to.
                OS::Requip::Stop();
                // OS-102: and tear the follower hair down while her 3D still
                // EXISTS. This message is the last point at which it does. The
                // revert callback that runs later drops our captures without
                // touching an actor, which is right for them and leaves FSMP
                // holding physics systems pointing at a destroyed head - the
                // in-session-load CTD. Ordering, not cleanup; see
                // NpcHair::OnPreLoadGame.
                OS::NpcHair::OnPreLoadGame();
                // ⚠⚠ r91, AND IT IS AN EXPERIMENT RATHER THAN A FIX. r90
                // measured skee repainting the outgoing character's hair
                // colour onto the incoming one during the LOAD SCREEN, out of
                // a mapped preset that survives every load - 42 s before
                // kPostLoadGame, which is where r63-r70 dispatched every erase
                // they tried. Those rounds were downstream of the paint and
                // still manufactured the load-double; this message is the only
                // window upstream of it and the only one never measured.
                // Gated, and it logs both halves because whether Papyrus even
                // answers this late is half the question.
                OS::ProfileApply::ErasePresetAtPreLoad();
                // ⚠⚠ THE OUTGOING CHARACTER'S COUNTERS GO NOW, NOT WHEN THE
                // NEXT ONES ARRIVE. Stats are per character and the answers
                // take ~180 ms, so without this the map still holds the save
                // being left behind while the save being loaded runs its first
                // promotion pass, and a counter that is too high grants a
                // colour permanently. Add-only means nothing takes that back.
                //
                // ⚠ AND IT MOVES THE GENERATION, so an answer already in flight
                // for the outgoing character cannot refill the map a moment
                // after this line clears it.
                OS::DyeStats::Forget();
                // ⚠ THE OUTGOING CHARACTER'S CARDS GO WITH THEIR COUNTERS. A
                // card still on screen, or still queued, would announce the
                // save being left behind over the first frames of the one being
                // loaded. The tick stops for the same reason: it must not run a
                // promotion pass against a half-loaded world.
                OS::DyeUnlockCard::Forget();
                OS::DyeTick::Stop();
                // Round eighteen's cross-save latch: Body Studio's ownership
                // map and OBody's once-per-character baseline both outlived
                // the save they were minted against.
#if defined(FR_BODY_STUDIO)
                OS::BodyStudioProof::ForgetSession();
#endif
                OS::ObodyApi::ForgetPlayerBaseline();
                break;
            case SKSE::MessagingInterface::kPostPostLoad:
                // OBody NG's native plugin API (4.4.0+). kPostPostLoad is the
                // message its own documented example uses - OBody does not
                // answer earlier, so requesting from kPostLoad silently gets
                // nothing and the body category would just never appear.
                // Absence is a supported state; see ObodyApi.h.
                // OBody may answer synchronously and announce ready from
                // inside Request(), so acquire RaceMenu before that callback
                // can queue a custom-key reassertion.
                //
                // ⚠ NO LONGER UNDER THE BODY STUDIO GUARD. Body morphs stopped
                // being only Body Studio's feature when the Shape page started
                // driving the same interface under its own key on every
                // channel; a guarded build would ship the page with nothing
                // behind its sliders.
                OS::RaceMenuMorphApi::Request();
                // ⚠ NOT INSIDE THE BODY STUDIO GATE ABOVE, deliberately. That
                // gate is there because body morphs ARE the Body Studio
                // feature; shaping a character is not, and gating it here would
                // mean a normal build never ships it. Same message for the same
                // reason: skee does not answer before kPostPostLoad.
                OS::NodeTransformApi::Request();
                // The Overlays page, on the same message and for the same
                // reason. It asks for two interfaces rather than one, since a
                // slot without a way to paint into it is no use.
                OS::OverlayApi::Request();
                // The skin changer: the same Override interface plus the
                // ActorUpdateManager, whose attach observer is the one place
                // this mod paints a skin from. Same message, same reason.
                OS::SkinApi::Request();
                OS::ObodyApi::Request();
                break;
            case SKSE::MessagingInterface::kNewGame:
                // No co-save to load for a genuinely new game - the
                // serialization LoadCallback never fires, since there is no
                // save file yet - so RuleStore's per-save rule state has to
                // be seeded here instead, from rules.json. The loaded-game
                // equivalent lives in Persistence::LoadCallback (Task 10);
                // RuleStore::OnRevert already ran (the engine's own revert
                // hook fires before kNewGame reaches this listener), so this
                // starts from a clean slate.
                OS::RuleStore::OnNewGame();
                // ⚠⚠ AND THE OUTFITS THEMSELVES, WHICH NEVER GOT THE ARGUMENT
                // THE RULES ABOVE HAVE ALWAYS HAD (user 2026-08-24: "my
                // outfits appear on every character, is this intended?"). It
                // was not. LoadLibraryFileAtStartup puts outfits.json into the
                // session at kDataLoaded and a new game has no LoadCallback to
                // replace it, so every new character inherited the last one's
                // wardrobe, and the first save baked it into that save's own
                // 'LIBR' record where it stopped looking like inheritance at
                // all. Persistence.h argues exactly this for rules and the same
                // words apply here.
                //
                // ⚠ THE SEED ONLY, AND NOTHING ON DISK IS TOUCHED. OnLoad
                // replaces the session library and queues no file write, so
                // outfits.json still holds what it held; bOutfitsShared is how
                // a player asks for it back.
                //
                // ⚠⚠ AND THIS IS NO LONGER THE ONLY GATE, WHICH IS WHY IT
                // SURVIVED RATHER THAN MOVED. It was the only one until
                // 2026-08-26, and `coc` from the main menu delivers no kNewGame
                // at all, so that start inherited the global library and the
                // field read it as the toggle doing nothing. The clear lives at
                // RevertCallback now, which both starts cross. This one stays
                // because it is idempotent and because it names the setting in
                // the log at the moment a player would look for it.
                if (!OS::Settings::GetSingleton().outfitsShared) {
                    OS::OutfitSession::GetSingleton().OnLoad(OS::OutfitLibrary{});
                    spdlog::info("Persistence: new game, so the outfit library starts "
                                 "EMPTY. outfits.json is untouched; set [General] "
                                 "bOutfitsShared to seed a new character from it.");
                } else {
                    spdlog::info("Persistence: new game seeded from outfits.json "
                                 "([General] bOutfitsShared is on).");
                }
                // HERE AS WELL AS AT kPreLoadGame: a new game from the main
                // menu after a load fires no kPreLoadGame
                // (coc-from-main-menu-skips-newgame), and both latches are
                // idempotent to clear twice.
#if defined(FR_BODY_STUDIO)
                OS::BodyStudioProof::ForgetSession();
#endif
                OS::ObodyApi::ForgetPlayerBaseline();
                [[fallthrough]];
            case SKSE::MessagingInterface::kPostLoadGame:
                // The watchdog's clock restarts here and its next sample
                // prints a baseline, BEFORE the reasserts below, so the first
                // reading is what the LOAD produced rather than what our own
                // painters then did. Every transition after it is timed from
                // this moment.
                OS::AppearanceWatch::NoteLoadBoundary("post-load");
                // Instruments (field 2026-09-02 02:41): the list and the base's
                // saved layers once the engine has settled the load.
                if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                    OS::MakeupApi::DumpWorn(player, "post-load");
                    OS::MakeupApi::DumpSavedLayers(player, "post-load");
                }
                OS::SculptProbe::ArmPulseLadder();
                // The census caps re-arm per load: r65's doubling load was
                // blind because an apply a minute earlier had spent the
                // session budget.
                OS::HeadBuildHook::ResetLoadCensus();
                // ⛔⛔ THE SKIN TONE IS NOT RELEASED HERE ANY MORE, AND THE OLD
                // REASONING IS WHAT EXPIRED. It read "a load restores the
                // character's own colour, so nothing of ours is owed to the
                // body", which was true while the hold was session state. The
                // hold now rides the co-save as 'SKTN', so the save's own
                // record IS the character's colour and releasing it here throws
                // the load's own answer away.
                //
                // This runs at kPostLoadGame, AFTER LoadCallback, and the field
                // round of 2026-08-25 08:20 caught it doing exactly that, 312
                // ms after the restore:
                //
                //   08:20:42.536  Persistence: restored the held skin tone (0,63,97) at 0.655
                //   08:20:42.848  Makeup: the held skin tone is released
                //   08:20:54.468  tint skintone (136,177,198) -> (167,134,122)
                //
                // The release still happens, in Persistence::RevertCallback,
                // which runs BEFORE the load and is where a per-character clear
                // belongs. Same seam, same lesson, as the overlay baseline's
                // clear two boundaries ago.
                //
                // ⚠ The face rebake below is NOT part of that and stays: the
                // bake this session left behind really is not this character's,
                // and it is owed to the head build the load starts (r39).
                OS::MakeupApi::ArmFaceRebake();
                // ⚠⚠ NO MAPPED-PRESET ERASE RUNS HERE ANY MORE, and that is a
                // MEASURED decision, not an omission. r63-r70: dispatching
                // skee's CharGen.ClearPreset anywhere inside the load window
                // - at this boundary or +2 s delayed - made the +5-7 s
                // rebuild apply the cosave sculpt TWICE (the load-double),
                // while skipping it entirely loads clean even with the map
                // alive. The apply-time erase chains inside the face step
                // now, and the boundary's one remaining job is the epoch
                // note: skee re-mints its string table at every load, and
                // the face step keys its double pass on it (r75).
                OS::ProfileApply::NoteLoadBoundary();
                // ⚠ THE SECOND AND LAST ASK FOR THE FSMP ROUTE, and it exists
                // because we cannot order ourselves after another plugin's
                // install pass at kDataLoaded: FSMP 4.0 installs its head hooks
                // from its own low-priority pass, so the proof can legitimately
                // fail the first time. Idempotent, latches only the positive,
                // and this is still the main thread and still before the player
                // can open the editor, which is what keeps IsAvailable() a
                // constant to the FUCK present thread that reads it while
                // drawing. ⚠ Do NOT make this a lazy arm inside the apply path;
                // that turns it into mutable state read from the present
                // thread.
                // ⚠ THE BEFORE READ IS WHAT SEPARATES AN ARM FROM A REPEAT.
                // ProveRoute latches its positive and answers the same yes on
                // every later call, so the transition has to be measured here;
                // the alternative is clearing declines on every load boundary
                // for a route that armed hours ago (OS-247).
                {
                    const bool wasArmed = OS::FsmpBridge::IsAvailable();
                    if (OS::FsmpBridge::ProveRoute() && !wasArmed) {
                        // Styles browsed while the route was dormant were
                        // declined for a cause that has just stopped being
                        // true. Without this they stay grey until a relaunch.
                        OS::NpcHair::ClearFsmpAbsentDeclines();
                    }
                }
                // After the co-save (if any) has loaded: fold the CURRENT
                // inventory into the collection - covers saves that predate
                // the collection record.
                OS::Collection::GetSingleton().SeedFromPlayerInventory();
                // ⚠ AFTER THE SEED, NEVER BEFORE IT, and Collection.h says why:
                // on a save that predates the seen-marks record the seed is what
                // adds the player's whole current kit, so a baseline taken
                // ahead of it would leave exactly those looks flashing as newly
                // found. Does nothing once a record has spoken.
                OS::Collection::GetSingleton().TakeAckBaselineIfOwed();
                // The save decides the player's race (UBE etc. are custom
                // races) - only now can style fit be evaluated.
                OS::StyleCatalog::GetSingleton().EnsureFitCurrent();
                // Discovered presets depend on fit (which pieces render on this
                // character), so generate only now, after EnsureFitCurrent.
                OS::AutoPresets::GetSingleton().Generate();
                OS::LoreModule::OnPostLoadGame();
                // OS-111. REPAINT THE PLAYER, because the exact tint does not
                // survive a save.
                //
                // A hair colour the user picks is an arbitrary RGB and lives on
                // screen ONLY as a geometry paint - HairColor.h carries the why:
                // the actor base can hold a BGSColorForm reference and nothing
                // finer, so what a save can persist is the SNAPPED vanilla form,
                // not the value. On load the engine's own head build paints from
                // that form and the character comes back visibly desaturated
                // (field 2026-08-02: #97F9FF set, #7BF4FB after a reload).
                //
                // Nothing here re-applied the player's look at all, so the only
                // thing that ever fixed it was opening the editor, whose own
                // path repaints. This is the player's half of what OS-109 did
                // for followers: the state was always in the co-save, nobody was
                // putting it back.
                //
                // ⚠ REASSERT, NOT A BARE RequestRefresh, and the difference is
                // the whole fix. A refresh only REPAINTS, and Repaint uses the
                // exact RGB only while g_state carries exactSet for this actor
                // (HairColor.cpp:576). That map is session-scoped on purpose -
                // a raw RGB does not fit CapturedColor's form reference and the
                // co-save schema had no downgrade path - so after a load it is
                // empty, Repaint falls back to the SNAPPED colour form, and the
                // character comes back visibly wrong (field 2026-08-02: painted
                // [form] rgb=(90,95,105) where the outfit says (121,82,129)).
                //
                // ReassertPlayerHairColor pushes the colour from the effective
                // outfit FIRST, which re-establishes exactSet, and requests the
                // refresh SECOND. That ordering is HairColor.h's stated contract
                // and what the 2026-07-30 spike proved; reversed, the rebuild
                // carries the old colour and the fix lands one refresh late.
                //
                // ⚠ Safe HERE and not earlier: PushPlayerHairColor's own comment
                // warns that a load-time push trips the 'ACTV'-before-'HCOL'
                // ordering trap. kPostLoadGame runs after the whole co-save is
                // read - both records are already in - so the trap is closed by
                // the time this fires. Do not move it into the record handlers.
                //
                // ⚠ AHEAD OF PROMOTION AND THE RULES ENGINE, decided at the
                // 2026-08-03 merge rather than inherited. Both branches added to
                // this handler independently and neither knew the other's line
                // would be here. exactSet is session state that every later
                // repaint reads, and WorldWatch below can apply a different
                // outfit and queue a refresh of its own; establishing exactSet
                // first means such a refresh carries the exact RGB instead of
                // falling back to the snapped colour form, which is the failure
                // this call exists to prevent.
                // ⚠ EYES AND BROWS FIRST, AND THE ORDER IS LOAD BEARING. This is
                // the same hole the hair colour note above describes, one
                // dimension over: nothing here re-applied the player's eyes, so
                // a character whose look came from an outfit or from a default
                // loaded with their record eyes and only corrected when the
                // editor opened, whose own path pushes (field 2026-08-07).
                //
                // It runs BEFORE the hair colour because applying a head part is
                // a head REBUILD, and a rebuild re-derives the hair tint from
                // the actor base and overwrites an exact RGB with whatever
                // palette form it snapped to. Reversed, the eye push would undo
                // the colour the line below just re-established, which is the
                // trap HeadPart and HairColor have both hit before.
                // OS-233: a fresh attempt budget for this load, and the fallback
                // arm two seconds behind the refresh the line after next
                // requests. The primary trigger is skee's own Face install fire
                // (OverlayApi's callback); this covers a rig with no face layers
                // for it to fire on.
                OS::OverlayReconcile::OnLoad();
                // ⛔ THE OVERLAY BASELINE IS NOT CLEARED HERE. It was, and that
                // was the bug: this runs AFTER LoadCallback, so it wiped the
                // record the save had just restored. The clear belongs in
                // Persistence::RevertCallback, which runs before the load, and
                // it is there now.
                // ⚠ THE SEX FLAG FIRST, because the head parts and the hair
                // colour below are both chosen by sex, and because this is the
                // seam every other reassert already uses: the player is fully
                // constructed here, which the deserialization callback cannot
                // promise. A build that wrote it in LoadCallback instead CTD'd
                // on load twice with no surviving log (CharacterSex.h).
                OS::CharacterSex::Apply(RE::PlayerCharacter::GetSingleton());
                OS::OutfitSession::GetSingleton().ReassertPlayerHeadParts();
                OS::OutfitSession::GetSingleton().ReassertPlayerHairColor(
                    /*a_atLoadBoundary=*/true);
                OS::OverlayReconcile::ArmAfterLoadRefresh();
                // Round twenty: the reasserts above rebuild the head, every
                // head build re-runs the engine's own partition dance (one
                // call site, RVA 0x3C57D9, turns 131 off when a worn item
                // declares biped 31 - drawn or not), and NOTHING on the
                // plain load path restored after it: RepaintNow's restore is
                // settle-gated to the apply window. So a save carrying a
                // switched look loaded bald with no apply in sight
                // (coc-from-main-menu-skips-newgame's shape: the reconcile
                // must run where the player goes). The ladder posts single
                // restore tasks at +1 s, +2.5 s and +5 s, on the far side of
                // every deferred attach this boundary queues.
                if (auto* const player = RE::PlayerCharacter::GetSingleton()) {
                    OS::BipedPost::ArmHeadPartitionSettle(player->GetHandle());
                }
                // ⚠⚠ A TRIPWIRE, NOT A SPIKE, AND IT IS THE ONE LINE THAT WOULD
                // CATCH THIS BUG COMING BACK. The facegen hair painter is an
                // entry another plugin also wants: skee64 held it on
                // 2026-08-12 and Fitting Room's detour, installed first, had
                // been silently overwritten and called zero times. The fix is
                // to install LAST, which works only for as long as we really
                // are last. A RaceMenu update that moves its own install later
                // would take the entry back, the colour would start dropping
                // again, and nothing else in the log would say so - the hook
                // would go on reporting a clean install exactly as it did then.
                //
                // Once per load, and it says both halves: whether the entry
                // bytes are still the ones we wrote, and which module the jump
                // lands in.
                OS::HeadBuildHook::ReportEntryIntegrity();
                // ⚠ HERE, not at kDataLoaded. Promotion has to run AFTER the
                // co-save record has been decoded, or it would evaluate against
                // an empty unlock set and against whatever character the main
                // menu happened to be showing. This case already exists because
                // style fit and presets need the same thing: the save decides
                // who the player is.
                //
                // ⚠ THE SHARED MERGE GOES FIRST, and the order is deliberate
                // rather than incidental. Both are add only, so the end state
                // is the same either way, but this way DyeWorld::Gather inside
                // the promotion pass sees the shared ids, and the two log lines
                // read in the order a player would tell it: here is what you
                // already had, here is what you just earned. It shares this
                // handler's one requirement, which is that the co-save record
                // is already decoded.
                OS::Persistence::SyncSharedDyeUnlocks("this load");
                // ⚠ AFTER THE MERGE AND BEFORE PROMOTION. After, so a colour
                // inherited from another character is adopted as already seen
                // rather than announced as newly earned by this one; before,
                // so anything promotion grants below IS announced.
                if (const auto adopted = OS::DyeUnlocks::TakeAckBaselineIfOwed();
                    adopted > 0) {
                    spdlog::info("Dye unlocks: this save had no seen-marks record, so its "
                                 "{} earned colour(s) are adopted as already seen. Only "
                                 "colours earned from now on are marked new.",
                                 adopted);
                }
                // ⚠ AFTER TakeAckBaselineIfOwed ABOVE, and the order is load
                // bearing. Shared looks arrive UNACKNOWLEDGED on purpose, and
                // a baseline taken after they landed would adopt every one of
                // them as already seen, which is precisely the discovery this
                // exists to hand the player.
                OS::Persistence::SyncSharedCollection("this load");
                // ⚠⚠ HERE AS WELL AS AT kPreLoadGame, AND IT IS NOT BELT AND
                // BRACES. A new game started from the main menu fires no
                // kPreLoadGame (coc-from-main-menu-skips-newgame), and
                // kNewGame falls through into this case, so this is the only
                // line both paths cross. Forget is idempotent, so the load that
                // did get a kPreLoadGame pays nothing for it.
                //
                // ⚠ AND IT MUST PRECEDE THE PASS BELOW. Forget disarms the
                // announcer, which is what makes the load's own gains a silent
                // baseline; running it afterwards would clear the queue the
                // pass had just filled and look identical in the log.
                OS::DyeUnlockCard::Forget();
                RunDyePromotion();
                // ⚠⚠ AND THEN AGAIN, ~180 ms FROM NOW, BECAUSE THE PASS ABOVE
                // COULD NOT SEE SKYRIM'S OWN COUNTERS. Game.QueryStat answers
                // asynchronously, so every stat-gated colour read 0 in the pass
                // that just ran. This asks for them and re-runs the identical
                // pass once the last answer lands.
                //
                // ⚠ AFTER PROMOTION AND OUTSIDE EVERY LOCK, ON PURPOSE, which
                // is the one thing the spike this replaces was for: dispatching
                // from inside DyeUnlocks::With would be asking about the hang by
                // causing it.
                //
                // ⚠ TWO PASSES IS THE DESIGN, NOT A RETRY. Promotion is
                // add-only and idempotent, so the second can only ever hand
                // over what the first could not see, and a dispatch that never
                // answers costs one load rather than a hang.
                RequestDyeStats();
                // ⚠ AND FROM NOW ON, EVERY FEW SECONDS, WHICH IS THE ONLY
                // REASON AN UNLOCK CAN BE ANNOUNCED AT ALL. The two passes
                // above are the only ones that ever ran until 2026-08-14, and
                // both of them happen while the player is looking at a loading
                // screen. This is what turns "you earned this at some point"
                // into "you just earned this".
                //
                // ⚠ AFTER the passes above, so the tick's own first-pass
                // baseline counts from a set they have already filled.
                OS::DyeTick::Start();
                // OS-206: the requip transition may run from here on.
                //
                // ⚠ THE PRESENT HOOK IS ARMED HERE RATHER THAN BY THE FIRST
                // SWAP. ImGuiOverlay hooks Present lazily on first editor open
                // and DyeTexture on first dye, so a player who has done neither
                // has no per-frame driver at all, and the first flourish of the
                // session would lose its opening frames waiting for one.
                OS::Requip::Start();
                OS::DyeTexture::EnsurePresent();
                // Seed the rules engine's pin/enabled state from RuleStore::
                // EngineState (populated by Persistence's load path just
                // before this message fires) and run the first evaluate.
                //
                // After promotion, and the order is free rather than load
                // bearing: both need the co-save decoded and neither reads what
                // the other writes. Kept in this order because a rule that
                // switches the outfit should evaluate against the unlock set
                // this save actually has.
                OS::WorldWatch::OnSaveLoaded();
                break;
            default:
                break;
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse) {
    SetupLog();
    // ⚠ BOTH AXES ON THE LOAD LINE, because they answer different questions and
    // a field report needs both. "Is the Body Studio page in this build" is
    // bodyStudio; "is this build reading its own isolated data" is channel. One
    // combined word for the two of them is what made "the page is missing" and
    // "my outfits are missing" hard to tell apart.
    spdlog::info("{} v{} loading (channel={}, bodyStudio={}, build={})...",
                 OS::BuildChannel::Label(), kVersion,
                 OS::BuildChannel::kBodyStudioDev ? "body-studio-dev" : "release",
                 OS::BuildChannel::kBodyStudio ? "on" : "off",
                 OS::BuildChannel::kBuildId);

    // ⚠⚠ WHICH FILE ON DISK IS THIS, and it is the second line of the log for a
    // measured reason. Under MO2 every branch of this mod sits in its own slot
    // and exactly one wins; modlist.txt is rewritten AT LAUNCH, so tools that
    // read it beforehand report the order the PREVIOUS session started with.
    // Twice on 2026-08-16 a field round was judged against another branch's
    // DLL, and both times the only way to find out was to notice a log line
    // whose FORMAT belonged to an older build. The binary knows its own path;
    // asking it costs one call and turns "which build was that" from an
    // inference into the second thing the log says.
    //
    // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS off a local, so it resolves THIS
    // module rather than the exe, and UNCHANGED_REFCOUNT so there is no handle
    // to release.
    //
    // ⚠ IT REPORTS THE VIRTUAL PATH, NOT THE SLOT, and that was measured rather
    // than predicted: under MO2 this prints the STOCK GAME data directory, not
    // 'Fitting Room [outfit-dye]'. usvfs answers the loader with the merged
    // Data tree, so the mod folder behind a file is not recoverable this way.
    // The line still earns its place - it names the file the loader actually
    // opened, which settles "is this even the build I deployed" - but the SLOT
    // question is only answerable from modlist.txt, and only after the launch
    // that rewrote it.
    {
        HMODULE self = nullptr;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(&kVersion), &self) &&
            self) {
            wchar_t path[MAX_PATH]{};
            if (const auto n = ::GetModuleFileNameW(self, path, MAX_PATH); n > 0) {
                // ⚠⚠ AND WHEN WAS IT BUILT, which the path above cannot answer.
                // Every branch of this mod resolves to the same usvfs path and
                // every build of a version prints the same version, so until
                // this stamp landed the only way to tell a fresh deploy from a
                // stale one was to notice a log line whose FORMAT had changed.
                // A field round on 2026-08-19 came back with no way to name the
                // DLL it ran; Menu Studio has printed one since the same
                // mistake cost it a whole round.
                //
                // ⚠ THE DLL'S OWN FILE TIME, NOT __DATE__/__TIME__. Those expand
                // when THIS translation unit compiles, so a build that changed
                // EditorUI.cpp and relinked would leave plugin.cpp untouched and
                // the stamp reporting the previous build. A stamp that lags
                // silently is worse than none because it is trusted. The
                // module's last write is the LINK moment and is always right.
                std::string               built = "unknown";
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                SYSTEMTIME                st{};
                if (::GetFileAttributesExW(path, GetFileExInfoStandard, &fad) &&
                    ::FileTimeToSystemTime(&fad.ftLastWriteTime, &st)) {
                    built = fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}Z", st.wYear,
                                        st.wMonth, st.wDay, st.wHour, st.wMinute,
                                        st.wSecond);
                }
                spdlog::info("  this DLL is '{}', built {}.",
                             std::filesystem::path(path).string(), built);
            }
        }
    }

    // Universal SE + AE build: SE 1.5.97 and every AE from 1.6.317 up.
    //
    // ⚠ THE OLD GATE STOPPED AT 1.6.1130, AND THAT WAS THE BUG. Users on the
    // mid 1.6 builds got SKSE's "reported as incompatible during load" dialog,
    // which is this function returning false - our own message, never a crash.
    // The comment that used to sit here said pre-next-gen AE "would need its
    // own re-verify", which was true and was never done, so the gate stood in
    // for the missing evidence.
    //
    // The evidence exists now. Every id Fitting Room uses resolves on every AE
    // database from 1.6.317 up (docs/research/callsites.py), and what actually
    // varies - the hand-measured byte offsets into functions - is no longer
    // trusted: VersionCheck locates each call site by what it CALLS, and for
    // the worn pass by what it calls AND the visitor it hands over. The gate
    // now refuses on a MEASUREMENT rather than on a version number.
    const auto ver = a_skse->RuntimeVersion();
    if (ver != SKSE::RUNTIME_SSE_1_5_97 && ver < REL::Version(1, 6, 317, 0)) {
        spdlog::error("Unsupported Skyrim runtime {}. Fitting Room needs SE 1.5.97 or AE "
                      "1.6.317 and later; not loading.", ver.string());
        return false;
    }

    OS::VersionCheck::Run();
    if (!OS::VersionCheck::CriticalOk()) {
        spdlog::error("Address self-check FAILED on runtime {} - the biped hooks have nowhere to "
                      "install, so Fitting Room would load and change nothing you wear. Not "
                      "loading. Please send FittingRoom.log to the author.", ver.string());
        return false;
    }

    SKSE::Init(a_skse);
    // 256 covered the biped and weapon hooks. The item-card charge spike adds
    // up to six more displaced calls, so the pool is widened rather than left
    // to run out silently partway through installing them.
    SKSE::AllocTrampoline(512);

    // Install the two biped-rebuild hooks. Trampoline must be allocated first.
    OS::BipedHooks::Install();

    // The weapon/quiver override's three part-3D call-site hooks. Independent
    // of the armor hooks above in both directions: these can all fail their
    // byte checks and armor transmog still works, and vice versa.
    OS::WeaponHooks::Install();

    // OS-154: hide the inventory item preview in the frame it is created, while
    // the editor is open. ⚠ Independent of every hook above and of the reactive
    // hide in EditorWindow::Draw, in both directions: this can fail to install
    // and the editor still hides the preview one frame late, which is why its
    // failure path logs and returns rather than joining CriticalOk.
    OS::Inventory3DHooks::Install();

    // ⚠ THE FACEGEN HAIR PAINTER IS DELIBERATELY NOT INSTALLED HERE. It is the
    // one hook in this plugin that another mod also wants, and installing at
    // plugin load lost the entry to whoever patched it later. It goes on at
    // kDataLoaded instead; see the comment there.

    // Co-save (de)serialization must be registered before kDataLoaded.
    OS::Persistence::Register();

    auto* messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(OnMessage)) {
        spdlog::error("Failed to register SKSE messaging listener; aborting load.");
        return false;
    }

    spdlog::info("{} loaded.", OS::BuildChannel::Label());
    return true;
}
