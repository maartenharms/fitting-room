#include "MenuStudioApi.h"

#include <Windows.h>

namespace OS::MenuStudioApi {

    namespace {

        using GetApiVersion_t = std::uint32_t (*)();
        using SetFramedCompanion_t = bool (*)(std::uint32_t);
        using TookEffect_t = bool (*)();
        using CanBeFramed_t = bool (*)(std::uint32_t);
        using FocusShotOnNode_t = void (*)(const char*, float);
        using FocusShotOnAttachment_t = void (*)(const char*, float);
        using ClearShotFocus_t = void (*)();
        using ShotWasPanned_t = bool (*)();
        using SetActionVisible_t = bool (*)(const char*, bool);
        using SetActionEnabled_t = bool (*)(const char*, bool, const char*);
        using SetOwnerContext_t = bool (*)(const char*, bool);
        using SetOwnerWorldLive_t = bool (*)(const char*, bool);
        using SetItemPreviewSuppressed_t = bool (*)(const char*, bool);
        using SetActionMeter_t = bool (*)(const char*, float);

        // ⚠ A FLOOR, NOT AN EQUALITY, AND THE DIFFERENCE ALREADY MATTERED ONCE.
        // This read `v != kSupportedApi` with kSupportedApi at 1, so the day
        // Menu Studio shipped API 2 this file would have stood down completely
        // and taken the follower framing with it - a feature that works today,
        // broken by a version bump that added an entry point and changed none
        // of ours. That is the co-save lesson in a different costume: a refused
        // read on a bump destroys what was already working.
        //
        // Menu Studio's own rule is that it bumps for a BREAKING change to an
        // existing entry point, so the honest test is "at least what we were
        // written against", with each newer call gated on the version that
        // introduced it. A hard ceiling belongs here only if Menu Studio ever
        // breaks one of these, and then it belongs as a named maximum with the
        // reason beside it.
        constexpr std::uint32_t kMinApi = 1;
        constexpr std::uint32_t kShotApi = 2;  // FocusShotOnNode / ClearShotFocus

        struct Api {
            SetFramedCompanion_t setCompanion{ nullptr };
            TookEffect_t         tookEffect{ nullptr };
            // Added after the first three, so a Menu Studio that predates it
            // still reports API version 1 and still resolves everything else.
            // That is the version contract working as documented, not a gap.
            CanBeFramed_t        canBeFramed{ nullptr };
            // API 2. Null on an older Menu Studio, which is what makes the
            // editor's slot focus a silent no-op there rather than an error.
            FocusShotOnNode_t    focusShot{ nullptr };
            ClearShotFocus_t     clearShot{ nullptr };
            // Added to the shot family after the pair above, so a Menu Studio
            // can report API 2 and still not export it. Null there, and
            // ShotWasPanned's own comment says which way that resolves.
            ShotWasPanned_t      shotWasPanned{ nullptr };
            // Added to the shot family without a bump, like shotWasPanned. Null
            // on a Menu Studio that predates it, and the weapon rows then get
            // the plain node focus they had before.
            FocusShotOnAttachment_t focusAttachment{ nullptr };
            // Added to the action family without a version bump, per Menu
            // Studio's stated policy (additive export, name is new). The
            // lookup is the gate, the same shape canBeFramed took when it
            // joined after the first three.
            SetActionVisible_t   setActionVisible{ nullptr };
            // Newer than setActionVisible and resolved the same way. Null on a
            // Menu Studio that predates it, and SetActionEnabled's own comment
            // says which way that resolves: the button stays fully live, which
            // is honest, because an older strip genuinely cannot refuse.
            SetActionEnabled_t   setActionEnabled{ nullptr };
            // Additive again, and by name again. Null on a Menu Studio that
            // predates the wake gate, where saying nothing is exactly right:
            // that build never holds its bubble down waiting to be told.
            SetOwnerContext_t    setOwnerContext{ nullptr };
            // Newer again (2026-08-23). Null on a Menu Studio without the
            // world-live wish, where the editor keeps the paused studio it has
            // had since the wake gate - physics frozen, everything else as
            // before. Nothing to degrade beyond that.
            SetOwnerWorldLive_t  setOwnerWorldLive{ nullptr };
            // Owner-scoped item-preview suppression. Null on Menu Studio builds
            // that predate the cooperative claim; Fitting Room's local hiders
            // remain active either way.
            SetItemPreviewSuppressed_t setItemPreviewSuppressed{ nullptr };
            // Absent on any Menu Studio older than the meter, which is a
            // plain no-op: the tile simply draws without a fill, exactly
            // as it did before this existed.
            SetActionMeter_t setActionMeter{ nullptr };
            bool                 resolved{ false };
        };

        // Resolved once, lazily. GetModuleHandleW does NOT take a reference, so
        // this never keeps Menu Studio loaded; both DLLs live for the process
        // either way, which is the same assumption FUCK_API.h makes.
        //
        // Lazy rather than at kDataLoaded because SKSE plugin load order is not
        // ours to assume: resolving on first use means Menu Studio has certainly
        // finished loading, whoever went first.
        const Api& Resolve() {
            static Api api = [] {
                Api out;
                out.resolved = true;
                auto* mod = GetModuleHandleW(L"MenuStudio.dll");
                if (!mod) {
                    spdlog::info("MenuStudioApi: MenuStudio.dll not loaded - the editor "
                                 "cannot ask for a follower to stay visible, so a follower "
                                 "preview will show whatever the load order leaves on screen.");
                    return out;
                }
                auto version = reinterpret_cast<GetApiVersion_t>(
                    GetProcAddress(mod, "MenuStudio_GetApiVersion"));
                if (!version) {
                    spdlog::warn("MenuStudioApi: MenuStudio.dll is loaded but exports no "
                                 "MenuStudio_GetApiVersion - it predates the public API.");
                    return out;
                }
                const std::uint32_t v = version();
                if (v < kMinApi) {
                    spdlog::warn("MenuStudioApi: Menu Studio reports API version {}, older "
                                 "than the {} this build was written against. Standing down "
                                 "rather than calling into a contract that predates ours.",
                                 v, kMinApi);
                    return out;
                }
                out.setCompanion = reinterpret_cast<SetFramedCompanion_t>(
                    GetProcAddress(mod, "MenuStudio_SetFramedCompanion"));
                out.tookEffect = reinterpret_cast<TookEffect_t>(
                    GetProcAddress(mod, "MenuStudio_FramedCompanionTookEffect"));
                out.canBeFramed = reinterpret_cast<CanBeFramed_t>(
                    GetProcAddress(mod, "MenuStudio_CompanionCanBeFramed"));
                // ⚠ GATED ON THE VERSION AS WELL AS ON GetProcAddress. The
                // lookup alone would be enough today, but a future Menu Studio
                // could keep the name and change the signature, and calling
                // through a stale function-pointer type is a crash rather than
                // the no-op every other absence here produces.
                if (v >= kShotApi) {
                    out.focusShot = reinterpret_cast<FocusShotOnNode_t>(
                        GetProcAddress(mod, "MenuStudio_FocusShotOnNode"));
                    out.clearShot = reinterpret_cast<ClearShotFocus_t>(
                        GetProcAddress(mod, "MenuStudio_ClearShotFocus"));
                    out.shotWasPanned = reinterpret_cast<ShotWasPanned_t>(
                        GetProcAddress(mod, "MenuStudio_ShotWasPanned"));
                    out.focusAttachment = reinterpret_cast<FocusShotOnAttachment_t>(
                        GetProcAddress(mod, "MenuStudio_FocusShotOnAttachment"));
                }
                out.setActionVisible = reinterpret_cast<SetActionVisible_t>(
                    GetProcAddress(mod, "MenuStudio_SetActionVisible"));
                out.setActionEnabled = reinterpret_cast<SetActionEnabled_t>(
                    GetProcAddress(mod, "MenuStudio_SetActionEnabled"));
                out.setOwnerContext = reinterpret_cast<SetOwnerContext_t>(
                    GetProcAddress(mod, "MenuStudio_SetOwnerContext"));
                out.setOwnerWorldLive = reinterpret_cast<SetOwnerWorldLive_t>(
                    GetProcAddress(mod, "MenuStudio_SetOwnerWorldLive"));
                out.setItemPreviewSuppressed =
                    reinterpret_cast<SetItemPreviewSuppressed_t>(
                        GetProcAddress(mod, "MenuStudio_SetItemPreviewSuppressed"));
                out.setActionMeter = reinterpret_cast<SetActionMeter_t>(
                    GetProcAddress(mod, "MenuStudio_SetActionMeter"));
                spdlog::info("MenuStudioApi: Menu Studio API v{} resolved (setCompanion={}, "
                             "tookEffect={}, canBeFramed={}, focusShot={}, clearShot={}, "
                             "shotWasPanned={}, focusAttachment={}, setActionVisible={}, "
                             "setActionEnabled={}, setOwnerContext={}, "
                             "setOwnerWorldLive={}, "
                             "setItemPreviewSuppressed={}, setActionMeter={}).",
                             v, out.setCompanion != nullptr, out.tookEffect != nullptr,
                             out.canBeFramed != nullptr, out.focusShot != nullptr,
                             out.clearShot != nullptr, out.shotWasPanned != nullptr,
                             out.focusAttachment != nullptr,
                             out.setActionVisible != nullptr,
                             out.setActionEnabled != nullptr,
                             out.setOwnerContext != nullptr,
                             out.setOwnerWorldLive != nullptr,
                             out.setItemPreviewSuppressed != nullptr,
                             out.setActionMeter != nullptr);
                return out;
            }();
            return api;
        }

    }  // namespace

    bool Available() { return Resolve().setCompanion != nullptr; }

    bool SetFramedCompanion(RE::Actor* a_actor) {
        const auto& api = Resolve();
        if (!api.setCompanion) {
            return false;
        }
        // The REFERENCE's form id, not the base NPC's - Menu Studio needs the
        // actor standing in the room, and a base id would resolve to a TESNPC
        // and be refused.
        return api.setCompanion(a_actor ? a_actor->GetFormID() : 0u);
    }

    void SetFramedCompanionDeferred(RE::ActorHandle a_actor) {
        // Resolve INSIDE the task, not here: between a target switch on the
        // render thread and the task running on the main thread the follower
        // can unload, and a handle that fails to resolve then correctly clears
        // rather than framing a character who is gone.
        if (auto* task = SKSE::GetTaskInterface()) {
            task->AddTask([a_actor]() {
                const auto ptr = a_actor.get();
                SetFramedCompanion(ptr ? ptr.get() : nullptr);
            });
        }
    }

    bool TookEffect() {
        const auto& api = Resolve();
        return api.tookEffect ? api.tookEffect() : false;
    }

    bool CanBeFramed(RE::Actor* a_actor) {
        if (!a_actor) {
            return false;
        }
        const auto& api = Resolve();
        // No Menu Studio, or a build too old to answer: everyone stays
        // selectable. Marking targets unusable on a guess would take followers
        // away from users whose setup never had the framing to begin with.
        if (!api.canBeFramed) {
            return true;
        }
        return api.canBeFramed(a_actor->GetFormID());
    }

    void FocusShotOnNode(const char* a_nodeName, float a_closeness) {
        const auto& api = Resolve();
        if (!api.focusShot) {
            return;
        }
        api.focusShot(a_nodeName, a_closeness);
    }

    void FocusShotOnAttachment(const char* a_nodeName, float a_fallbackCloseness) {
        const auto& api = Resolve();
        if (api.focusAttachment) {
            api.focusAttachment(a_nodeName, a_fallbackCloseness);
            return;
        }
        // An older Menu Studio still frames the node, just without the
        // measurement. Falling back rather than doing nothing keeps a weapon
        // selection moving the camera at all on those builds.
        FocusShotOnNode(a_nodeName, a_fallbackCloseness);
    }

    void ClearShotFocus() {
        const auto& api = Resolve();
        if (api.clearShot) {
            api.clearShot();
        }
    }

    void SetActionEnabled(const char* a_id, bool a_enabled, const char* a_reason) {
        const auto& api = Resolve();
        if (!api.setActionEnabled || !a_id) {
            return;
        }
        api.setActionEnabled(a_id, a_enabled, a_reason);
    }

    void SetActionVisible(const char* a_id, bool a_visible) {
        const auto& api = Resolve();
        if (!api.setActionVisible || !a_id) {
            return;
        }
        api.setActionVisible(a_id, a_visible);
    }

    void SetAppearanceButtonVisible(bool a_visible) {
        const auto& api = Resolve();
        if (!api.setActionVisible) {
            return;
        }
        // Menu Studio's id for its own button, from its ActionBar::Install.
        // This side knows the NAME rather than the button: a Menu Studio
        // that renames the id makes this a silent miss, which its own warn
        // line ("visibility for '...' ignored - no such button") surfaces
        // on the Menu Studio side of the log.
        api.setActionVisible("MenuStudio.RaceMenu", a_visible);
    }

    void SetOwnerContext(bool a_active) {
        const auto& api = Resolve();
        if (!api.setOwnerContext) {
            return;
        }
        // The id Menu Studio files this mod's context under. Spelled once,
        // here, because the open and the close must pass the SAME string or
        // the withdrawal never finds what the publication left behind. Unlike
        // SetAppearanceButtonVisible's id, this one is ours to choose, so a
        // Menu Studio that does not recognise it is not a silent miss - the
        // set is keyed on whatever callers bring.
        //
        // The world-live wish rides the SAME id and is re-asserted on every
        // publish (Menu Studio's own doc offers exactly that): the studio
        // still arms - camera, lights, backdrop - but takes no force-pause,
        // so SMP hair and body keep simulating while the editor styles them.
        // The user's correction (2026-08-23) outranks the paused default:
        // physics has to be visible in the editor, and it was until the wake
        // gate's pause arrived with `8608d1a`. Asserted before the publish so
        // the session that enters on this very call already sees the wish.
        if (a_active && api.setOwnerWorldLive) {
            api.setOwnerWorldLive("FittingRoom", true);
        }
        api.setOwnerContext("FittingRoom", a_active);
    }

    bool SetItemPreviewSuppressed(bool a_suppressed) {
        const auto& api = Resolve();
        if (!api.setItemPreviewSuppressed) {
            return false;
        }
        // The owner id is baked in here so acquisition and release cannot
        // publish under different names. Menu Studio holds the shared set;
        // Fitting Room keeps its local appender and reactive hiders regardless.
        return api.setItemPreviewSuppressed("FittingRoom", a_suppressed);
    }

    bool SetActionMeter(const char* a_id, float a_fraction) {
        const auto& api = Resolve();
        if (!api.setActionMeter || !a_id) {
            return false;
        }
        return api.setActionMeter(a_id, a_fraction);
    }

    bool ShotWasPanned() {
        const auto& api = Resolve();
        // ⚠ TRUE WHEN ABSENT, WHICH MEANS "LEAVE THE CAMERA ALONE". The header
        // carries the reasoning and the fact that no public Menu Studio reaches
        // this line. Note what it costs where the export IS missing but the
        // shot pair is not: page changes stop handing the whole character back,
        // because the editor is being told every framing was deliberate.
        return api.shotWasPanned ? api.shotWasPanned() : true;
    }

}  // namespace OS::MenuStudioApi
