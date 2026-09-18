#include "OverlayReconcile.h"
#include <cctype>
#include <algorithm>

#include "MakeupBaseline.h"   // the tint list we hold ourselves, for the same load
#include "OutfitSession.h"  // ReconcilePlayerOverlays: the two passes in one drain
#include "OverlayBaseline.h"  // the art we hold ourselves, put back into an empty store
#include "OverlayApi.h"     // Layers, Read: what RaceMenu holds for the body layers
#include "OverlayPlan.h"    // Occupied, Location

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

namespace OS::OverlayReconcile {

    namespace {

        [[nodiscard]] double NowSeconds() {
            using clock = std::chrono::steady_clock;
            return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
        }

        // Armed state, written from skee's callback thread and from the load
        // path, read on the game thread. One atomic double for the due time
        // and one flag; a torn pair is harmless (the check re-reads everything
        // that matters when it runs).
        std::atomic<bool>   g_armed{ false };
        std::atomic<double> g_dueAt{ 0.0 };
        std::atomic<int>    g_attemptsThisLoad{ 0 };
        std::atomic<bool>   g_applyRanThisLoad{ false };
        // One extra attempt, granted by an overlay edit once the budget is
        // spent and consumed by the next attempt that runs.
        std::atomic<bool>   g_editRearm{ false };
        // Monotonic, never reset. See StoreGeneration and OnLoad.
        std::atomic<std::uint32_t> g_storeGeneration{ 0 };
        constexpr int    kMaxAttemptsPerLoad = 2;
        constexpr double kFaceDelaySeconds   = 0.15;
        constexpr double kLoadDelaySeconds   = 2.0;

        void Arm(double a_delay) {
            const double due = NowSeconds() + a_delay;
            // The earlier of two arms wins; a later fire cannot push a pending
            // check further out.
            double cur = g_dueAt.load(std::memory_order_relaxed);
            if (!g_armed.load(std::memory_order_relaxed) || due < cur) {
                g_dueAt.store(due, std::memory_order_relaxed);
            }
            g_armed.store(true, std::memory_order_release);
        }

        // How many of RaceMenu's layers at one location carry art, read from
        // what skee holds rather than from the 3D. Zero means a rebuild would
        // draw nothing there.
        [[nodiscard]] int OccupiedLayers(RE::Actor* a_player, OverlayPlan::Location a_loc) {
            int n = 0;
            for (const auto& layer : OverlayApi::Layers()) {
                if (layer.location != a_loc) {
                    continue;
                }
                if (OverlayPlan::Occupied(OverlayApi::Read(a_player, layer.node))) {
                    ++n;
                }
            }
            return n;
        }

        // skee's own reset diffuse, whatever case the path is written in.
        // ⚠⚠ THE BACKSLASH MUST BE ESCAPED. This shipped as "overlays\default"
        // (C4129), which compiles to "overlaysdefault.dds" and matches NOTHING,
        // so every default-wearing clone counted as PAINTED from 53ba8b7
        // through r43 - the r40 "7 painted clones" census and every blank
        // trigger read through that lie.
        [[nodiscard]] bool IsResetDiffuse(const char* a_path) {
            std::string lower{ a_path };
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower.find("overlays\\default.dds") != std::string::npos ||
                   lower.find("overlays/default.dds") != std::string::npos;
        }

        // Clones under one root that are actually PAINTED: the same walk, but
        // counting only the ones whose diffuse is not skee's reset texture.
        //
        // ⚠⚠ A CLONE OUTLIVES THE STORE THAT PAINTED IT, and that is the
        // cross-save bleed. MEASURED r40: after loading an earlier save skee's
        // layer store reads EMPTY at post-load and at +2 s, +5 s and +12 s, the
        // tint list holds the loaded character's own colour and the head names
        // its own texture, and the body still wears the previous character's
        // seven overlays. The clones are geometry with a material on them, and
        // an empty store simply means nobody repaints them.
        [[nodiscard]] int CountPaintedClonesUnder(RE::NiAVObject* a_root,
                                                  const char*     a_prefix) {
            if (!a_root) {
                return -1;
            }
            const auto prefixLen = std::strlen(a_prefix);
            int        n         = 0;
            RE::BSVisit::TraverseScenegraphGeometries(
                a_root, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    const char* name = a_geom->name.c_str();
                    if (!name || std::strncmp(name, a_prefix, prefixLen) != 0 ||
                        !std::strstr(name, "Ovl")) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    if (auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                            a_geom->GetGeometryRuntimeData()
                                .shaderProperty
                                .get())) {
                        if (auto* const mat = static_cast<RE::BSLightingShaderMaterialBase*>(
                                prop->material)) {
                            if (const auto ts = mat->textureSet) {
                                const char* d = ts->GetTexturePath(
                                    RE::BSTextureSet::Texture::kDiffuse);
                                if (d && *d && !IsResetDiffuse(d)) {
                                    ++n;
                                }
                            }
                        }
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            return n;
        }

        // Clones under one root whose name starts with a_prefix and carries
        // "Ovl" (which also matches the spell "[SOvl" nodes; both are skee's).
        [[nodiscard]] int CountClonesUnder(RE::NiAVObject* a_root, const char* a_prefix) {
            if (!a_root) {
                return -1;
            }
            const auto prefixLen = std::strlen(a_prefix);
            int        n         = 0;
            RE::BSVisit::TraverseScenegraphGeometries(
                a_root, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    const char* name = a_geom->name.c_str();
                    if (name && std::strncmp(name, a_prefix, prefixLen) == 0 &&
                        std::strstr(name, "Ovl")) {
                        ++n;
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            return n;
        }

        // ⚠⚠ A CLONE'S MATERIAL HOLDS TWO DIFFUSES AND ONLY ONE IS DRAWN. The
        // texture set carries the PATH; the renderer draws the BOUND texture
        // in `diffuseTexture`, and the two move independently (our own dye
        // pass swaps the bound half and never touches the path). r44/r45
        // FIELD + MEASURED: after a reload every clone's PATH read the
        // default while the face still wore the other character's art - the
        // stale BOUND texture. The store, the path census and the blank pass
        // were all reading the half that was clean.
        //
        // The repair converges BOUND to SET wherever exactly one half reads
        // as art. Clone materials are the clone's own (never pooled), so
        // rebinding here breaks nobody else. Deliberately NOT comparing two
        // art paths for equality: set and bound spell the same file with
        // different prefixes ('Actors\...' vs 'textures\actors\...'), and a
        // string mismatch would rebind forever.
        int RebindDivergedClones(RE::NiAVObject* a_root, const char* a_who) {
            if (!a_root) {
                return 0;
            }
            int fixed = 0;
            RE::BSVisit::TraverseScenegraphGeometries(
                a_root, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    const char* name = a_geom->name.c_str();
                    if (!name || !std::strstr(name, "Ovl") ||
                        (std::strncmp(name, "Body [", 6) != 0 &&
                         std::strncmp(name, "Hands [", 7) != 0 &&
                         std::strncmp(name, "Feet [", 6) != 0 &&
                         std::strncmp(name, "Face [", 6) != 0)) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                        a_geom->GetGeometryRuntimeData()
                            .shaderProperty
                            .get());
                    if (!prop) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    auto* const mat =
                        static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);
                    if (!mat || !mat->textureSet) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const char* setPath =
                        mat->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse);
                    const char* boundName = nullptr;
                    if (auto* const bound = mat->diffuseTexture.get()) {
                        boundName = bound->name.c_str();
                    }
                    const bool setArt   = setPath && *setPath && !IsResetDiffuse(setPath);
                    const bool boundArt = boundName && *boundName && !IsResetDiffuse(boundName);
                    if (setArt == boundArt) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    mat->OnLoadTextureSet(0, mat->textureSet.get());
                    ++fixed;
                    spdlog::info(
                        "OverlayReconcile: {} '{}' drew bound '{}' while the set "
                        "says '{}'; rebound to the set.",
                        a_who, name, boundName ? boundName : "",
                        setPath ? setPath : "");
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            return fixed;
        }

    }  // namespace

    int CountPlayerBodyClones() {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        return CountClonesUnder(player ? player->Get3D(false) : nullptr, "Body [");
    }

    int CountPlayerFirstPersonClones() {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        auto* const root   = player ? player->Get3D(true) : nullptr;
        if (!root) {
            return -1;
        }
        return CountClonesUnder(root, "Body [") + CountClonesUnder(root, "Hands [");
    }

    void LogOverlayCensus(RE::Actor* a_actor) {
        if (!a_actor) {
            return;
        }
        for (const bool firstPerson : { false, true }) {
            auto* const root = a_actor->Get3D(firstPerson);
            const char* who  = firstPerson ? "1p" : "3p";
            if (!root) {
                spdlog::debug("OverlayCensus: {} root absent on '{}'.", who,
                              a_actor->GetName());
                continue;
            }
            const int body  = CountClonesUnder(root, "Body [");
            const int hands = CountClonesUnder(root, "Hands [");
            const int feet  = CountClonesUnder(root, "Feet [");
            const int face  = CountClonesUnder(root, "Face [");
            spdlog::debug("OverlayCensus: {} on '{}': body {}, hands {}, feet {}, face {}.",
                          who, a_actor->GetName(), body, hands, feet, face);
            // Every clone that wears something OTHER than skee's reset
            // diffuse, at EVERY location. r43: the store read artless through
            // +60 s and the body clones read default at every look, yet the
            // field still saw the other character's overlays - and nothing
            // was reading the FACE clones or the 1p root at those moments.
            // The census at the rungs is the geometry sensor the store dumps
            // are not; counts split installed-but-default from painted, and
            // this names the painted ones.
            int shown = 0;
            RE::BSVisit::TraverseScenegraphGeometries(
                root, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    if (shown >= 10) {
                        return RE::BSVisit::BSVisitControl::kStop;
                    }
                    const char* name = a_geom->name.c_str();
                    if (!name || !std::strstr(name, "Ovl") ||
                        (std::strncmp(name, "Body [", 6) != 0 &&
                         std::strncmp(name, "Hands [", 7) != 0 &&
                         std::strncmp(name, "Feet [", 6) != 0 &&
                         std::strncmp(name, "Face [", 6) != 0)) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    if (auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                            a_geom->GetGeometryRuntimeData()
                                .shaderProperty
                                .get())) {
                        if (auto* const mat = static_cast<RE::BSLightingShaderMaterialBase*>(
                                prop->material)) {
                            // ⚠⚠ TWO ANSWERS, NOT ONE. The texture set holds a
                            // PATH; the renderer draws the BOUND texture in
                            // `diffuseTexture`, and our own dye pass proves
                            // they move independently (DyeGpu swaps the bound
                            // half and never touches the path). r44: every
                            // path read default through +60 s and the field
                            // still saw the other character's art, so the path
                            // alone is not allowed to answer any more.
                            const char* setPath = nullptr;
                            if (const auto ts = mat->textureSet) {
                                setPath = ts->GetTexturePath(
                                    RE::BSTextureSet::Texture::kDiffuse);
                            }
                            const char* boundName = nullptr;
                            if (auto* const bound = mat->diffuseTexture.get()) {
                                boundName = bound->name.c_str();
                            }
                            const bool setArt = setPath && *setPath &&
                                                !IsResetDiffuse(setPath);
                            const bool boundArt = boundName && *boundName &&
                                                  !IsResetDiffuse(boundName);
                            if (setArt || boundArt) {
                                ++shown;
                                spdlog::debug(
                                    "OverlayCensus:   {} '{}' set='{}' bound='{}'{}",
                                    who, name, setPath ? setPath : "",
                                    boundName ? boundName : "",
                                    setArt != boundArt ? " (HALVES DISAGREE)" : "");
                            }
                        }
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            if (shown == 0) {
                spdlog::debug("OverlayCensus:   {} every clone wears the default, "
                              "path and bound both.",
                              who);
            }
        }
    }

    void OnLoad() {
        g_attemptsThisLoad.store(0, std::memory_order_relaxed);
        g_armed.store(false, std::memory_order_relaxed);
        g_applyRanThisLoad.store(false, std::memory_order_relaxed);
        // The first check after this load pushes the record even on store
        // agreement; the rebuilt 3D is the half the store cannot vouch for.
        OverlayBaseline::NoteLoadBoundary();
        MakeupBaseline::NoteLoadBoundary();
        // ⚠ BUMPED, NOT RESET. A reader comparing a number it took before the
        // load against a counter that went back to zero could match by accident
        // and keep showing the last character's layers. Monotonic is the whole
        // contract; the value itself means nothing.
        g_storeGeneration.fetch_add(1, std::memory_order_release);
    }

    void ArmAfterLoadRefresh() { Arm(kLoadDelaySeconds); }

    void NoteFaceInstall() {
        // ⚠ THE COUNTER IS FOR A READER, NOT FOR THE REPAIR. skee reinstalling
        // the player's face is the one cheap signal that its override store may
        // have been rewritten underneath anybody holding a copy, and the
        // Overlays page holds exactly such a copy for as long as it is open.
        // See StoreGeneration.
        g_storeGeneration.fetch_add(1, std::memory_order_release);
        Arm(kFaceDelaySeconds);
    }

    void NoteOverlayEdit() {
        if (g_attemptsThisLoad.load(std::memory_order_relaxed) >= kMaxAttemptsPerLoad &&
            !g_editRearm.exchange(true, std::memory_order_acq_rel)) {
            spdlog::info("OverlayReconcile: an overlay edit re-armed one repair attempt "
                         "after this load's budget of {} was spent; the check runs in "
                         "{:.1f} s.",
                         kMaxAttemptsPerLoad, kLoadDelaySeconds);
        }
        Arm(kLoadDelaySeconds);
    }

    void NoteProfileApply() {
        g_storeGeneration.fetch_add(1, std::memory_order_release);
        g_applyRanThisLoad.store(true, std::memory_order_release);
    }

    std::uint32_t StoreGeneration() {
        return g_storeGeneration.load(std::memory_order_acquire);
    }

    void Tick() {
        const double now = NowSeconds();
        if (!g_armed.load(std::memory_order_acquire) ||
            now < g_dueAt.load(std::memory_order_relaxed)) {
            return;
        }
        g_armed.store(false, std::memory_order_relaxed);

        if (g_attemptsThisLoad.load(std::memory_order_relaxed) >= kMaxAttemptsPerLoad &&
            !g_editRearm.load(std::memory_order_acquire)) {
            return;
        }
        // ⚠⚠ A CHECK THAT CANNOT RUN YET IS DEFERRED, NOT CONSUMED. Field
        // 2026-09-01 21:03: a load taken with the editor open came due before
        // the player's 3D was up, both of the load's armed checks burned on
        // this guard without a line in the log, and the baseline never ran
        // against the store skee's cosave had rewritten. The attempt budget is
        // only spent by a check that actually runs.
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Get3D(false) || !OverlayApi::Available()) {
            Arm(kLoadDelaySeconds);
            return;
        }
        // RaceMenu owns the overlays while its menu is open, and it does a full
        // rebuild on close; a repair in the middle would fight it. Deferred for
        // the same reason as above: the close edge is exactly when the store is
        // worth judging.
        if (auto* const ui = RE::UI::GetSingleton();
            ui && ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME)) {
            Arm(kLoadDelaySeconds);
            return;
        }
        // The bound-diffuse repair runs on every armed check, before and
        // regardless of the blank logic below: converging the drawn texture
        // to the set is safe on either side of an apply, and the diverged
        // case is invisible to everything below (the blank pass counts
        // "painted" by the PATH, which reads clean exactly when this is
        // wrong).
        if (const int diverged =
                RebindDivergedClones(player->Get3D(false), "3p") +
                RebindDivergedClones(player->Get3D(true), "1p");
            diverged > 0) {
            spdlog::info(
                "OverlayReconcile: {} clone(s) drew a texture their set does "
                "not name any more; all rebound to their sets.",
                diverged);
        }
        // ⚠⚠ BEFORE THE COUNTS BELOW, AND THAT ORDER IS THE POINT. Putting the
        // recorded art back fills the store, so `occupiedBody` on the next line
        // sees it and the blank pass further down correctly stands down. Run
        // after those counts instead and this pass would restore the art while
        // the same tick read the store as empty.
        //
        // A no-op when the store agrees with the record, which is the ordinary
        // case, so running it on both of a load's armed checks costs one store
        // read and a debug line the second time. When they disagree the record
        // wins; OverlayBaseline.h carries why that is safe now and was not
        // before the close-edge re-sync existed.
        OverlayBaseline::ReassertRecord(player);
        // The tint list rides the same checks: skee restores ITS copy of the
        // 108-slot list on every load, which predates our in-session writes,
        // and the load-time rebake composites the face from it. Same split,
        // one container down; MakeupBaseline.h carries the 00:14 skull.
        MakeupBaseline::ReassertRecord(player);

        // Third person: the OS-233 check, body clones against body layers.
        const int  clones       = CountPlayerBodyClones();
        const int  occupiedBody = OccupiedLayers(player, OverlayPlan::Location::kBody);
        const bool thirdBroken  = clones == 0 && occupiedBody > 0;

        // ⚠⚠ THE FIRST PERSON ARM IS GONE AGAIN, ON PURPOSE. It was added on
        // 2026-08-21 on the theory that RaceMenu's post-load reapply took the 1p
        // clones the way it takes the body's. The census shipped in the same
        // build measured otherwise: the 1p root carries ZERO clones in every
        // reading of every load, because the installed skee never installs
        // there at all. So the rebuild could not have repaired it, and both of
        // this load's attempts burned at every load doing nothing. Overlay1P
        // now BUILDS the first person clones instead, which is a different
        // repair with a different trigger, and these two attempts belong to the
        // third person body again.
        //
        // CountPlayerFirstPersonClones and the census stay: they are what
        // measured this, and they are what will say whether Overlay1P worked.
        // ⚠⚠ AND THE INVERSE, WHICH IS THE CROSS-SAVE BLEED. Art on the clones
        // with NOTHING behind it in the store is the previous character's
        // overlays surviving on this one's body: skee repaints from its store,
        // so an empty store leaves whatever was there. Clearing a layer removes
        // our overrides and puts skee's own reset diffuse back, which is the
        // blank state a character with no overlays should have.
        if (const int painted = CountPaintedClonesUnder(player->Get3D(false), "Body [");
            occupiedBody == 0 && painted > 0) {
            // ⚠⚠ ONLY UNTIL THE FIRST APPLY OF THIS LOAD. r42 MEASURED,
            // 3-for-3: a face-only preset apply replaces skee's whole store
            // with the preset's (a bare face preset carries no overlays), so
            // the store reads empty while the look's art is still painted -
            // and this pass erased that art 0.15-0.5 s later every time,
            // which is the field's "a look applies correctly once and then
            // stops". Orphans from the character this save is NOT are a
            // load-boundary artefact; once an apply has run, the art on the
            // body is the look's own and the store's silence means skee was
            // handed nothing to say about it.
            if (g_applyRanThisLoad.load(std::memory_order_acquire)) {
                spdlog::info(
                    "OverlayReconcile: the store holds no layer and the body "
                    "wears {} painted clone(s), but an apply has run this load, "
                    "so the art is the look's own and the blank pass stands "
                    "down.",
                    painted);
                return;
            }
            // Name what is about to be blanked; a count alone cannot say whose
            // art died.
            int shown = 0;
            RE::BSVisit::TraverseScenegraphGeometries(
                player->Get3D(false),
                [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    if (shown >= 8) {
                        return RE::BSVisit::BSVisitControl::kStop;
                    }
                    const char* name = a_geom->name.c_str();
                    if (!name || std::strncmp(name, "Body [", 6) != 0 ||
                        !std::strstr(name, "Ovl")) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    if (auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                            a_geom->GetGeometryRuntimeData()
                                .shaderProperty
                                .get())) {
                        if (auto* const mat = static_cast<RE::BSLightingShaderMaterialBase*>(
                                prop->material)) {
                            if (const auto ts = mat->textureSet) {
                                const char* d = ts->GetTexturePath(
                                    RE::BSTextureSet::Texture::kDiffuse);
                                if (d && *d && !IsResetDiffuse(d)) {
                                    ++shown;
                                    spdlog::info(
                                        "OverlayReconcile:   blanking '{}' "
                                        "diffuse='{}'",
                                        name, d);
                                }
                            }
                        }
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            int cleared = 0;
            for (const auto& layer : OverlayApi::Layers()) {
                const auto st = OverlayApi::Read(player, layer.node);
                if (st.hasTexture || st.hasTint || st.hasAlpha) {
                    continue;  // the store owns this one
                }
                OverlayApi::Clear(player, layer.node);
                ++cleared;
            }
            spdlog::info(
                "OverlayReconcile: the store holds no layer and the body still "
                "wore art, so {} orphaned clone layer(s) were blanked. That art "
                "belonged to the character this save is not.",
                cleared);
            return;
        }
        if (!thirdBroken) {
            return;  // healthy, or unreadable; either way not this
        }
        const int attempt = g_attemptsThisLoad.fetch_add(1, std::memory_order_relaxed) + 1;
        g_editRearm.store(false, std::memory_order_release);  // an attempt that runs spends the grant
        spdlog::info("OverlayReconcile: {} layer(s) with art and no clone on the 3p root; "
                     "running the two-pass rebuild (attempt {} this load).",
                     occupiedBody, attempt);
        OutfitSession::ReconcilePlayerOverlays();  // reports its own result when it runs
    }

}  // namespace OS::OverlayReconcile
