#include "OverlayApi.h"

#include "MakeupApi.h"  // ArmDelayedFaceRebake: a layer write provokes the re-bind

#include "OverlayBake.h"   // OS-209: the bake a moved layer's path names, and the probe
#include "OverlayBaseline.h"  // every write is recorded so a load can put it back
#include "OverlayReconcile.h"  // OS-233: NoteFaceInstall, the repair's trigger

#include <chrono>
#include <cstdint>
#include <cstring>  // strncmp, the Face node test in the callback
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// RaceMenu's public modder header uses opaque global Skyrim type declarations.
// Keep it in this one translation unit and bridge the actor pointer at the call
// boundary, exactly as NodeTransformApi and RaceMenuMorphApi do; no RaceMenu
// type escapes through our header.
#include "../extern/RaceMenu/IPluginInterface.h"

namespace OS::OverlayApi {

    namespace {

        IOverlayInterface*  g_overlay{ nullptr };
        IOverrideInterface* g_override{ nullptr };
        Status              g_status{ Status::kNotRequested };

        std::vector<OverlayPlan::Layer> g_layers;

        [[nodiscard]] TESObjectREFR* AsRaceMenuRef(RE::Actor* a_actor) {
            return reinterpret_cast<TESObjectREFR*>(a_actor);
        }

        // OS-226 stage S0: skee's own install callback, and the only place
        // RaceMenu's opaque pointer types are turned back into engine ones.
        //
        // ⚠ THE CAST IS THE SAME BRIDGE AsRaceMenuRef MAKES, RUN BACKWARDS.
        // skee's header declares `TESObjectREFR` and `NiAVObject` as its own
        // global forward declarations, so the parameters arrive typed as
        // RaceMenu's and are the engine's objects; no RaceMenu type leaves this
        // translation unit, which is the rule the whole file is written to.
        //
        // ⚠ IT RUNS INSIDE skee's INSTALL, per node and per actor, so it does
        // as little as a function can: a counter, a name, and (OS-233) one
        // atomic arm when the node is a Face clone on the player, which is the
        // one fire that reliably follows RaceMenu's post-load body revert.
        void OnOverlayInstalled(TESObjectREFR* a_ref, NiAVObject* a_node) {
            auto* const ref  = reinterpret_cast<RE::TESObjectREFR*>(a_ref);
            auto* const node = reinterpret_cast<RE::NiAVObject*>(a_node);
            if (ref && node && ref == RE::PlayerCharacter::GetSingleton()) {
                const char* name = node->name.c_str();
                if (name && std::strncmp(name, "Face [", 6) == 0) {
                    OverlayReconcile::NoteFaceInstall();
                }
            }
        }

        [[nodiscard]] bool IsFemaleActor(RE::Actor* a_actor) {
            auto* const base = a_actor ? a_actor->GetActorBase() : nullptr;
            return base && base->IsFemale();
        }

        // ⚠ THE PAGE'S ORDER IS NOT skee's, AND THIS IS THE ONLY PLACE THAT
        // KNOWS IT. OverlayPlan leads with Face because that is what a player
        // opens the page for; IOverlayInterface::OverlayLocation runs Body,
        // Hand, Feet, Face. Mapping in one function keeps the two orders from
        // being silently assumed equal anywhere else, which would put body
        // overlays on the face.
        [[nodiscard]] IOverlayInterface::OverlayLocation ToSkee(
            OverlayPlan::Location a_location) {
            switch (a_location) {
                case OverlayPlan::Location::kFace:
                    return IOverlayInterface::OverlayLocation::Face;
                case OverlayPlan::Location::kBody:
                    return IOverlayInterface::OverlayLocation::Body;
                case OverlayPlan::Location::kHands:
                    return IOverlayInterface::OverlayLocation::Hand;
                case OverlayPlan::Location::kFeet:
                    return IOverlayInterface::OverlayLocation::Feet;
            }
            return IOverlayInterface::OverlayLocation::Body;
        }

        // ---- the variant adaptors ------------------------------------------
        //
        // skee takes a value through a visitor rather than a union, so every
        // write needs one of these and every read needs the other half.

        class SetString final : public IOverrideInterface::SetVariant {
        public:
            explicit SetString(std::string a_value) : m_value(std::move(a_value)) {}
            Type        GetType() override { return Type::String; }
            const char* String() override { return m_value.c_str(); }

        private:
            std::string m_value;
        };

        class SetInt final : public IOverrideInterface::SetVariant {
        public:
            explicit SetInt(skee_i32 a_value) : m_value(a_value) {}
            Type     GetType() override { return Type::Int; }
            skee_i32 Int() override { return m_value; }

        private:
            skee_i32 m_value;
        };

        class SetFloat final : public IOverrideInterface::SetVariant {
        public:
            explicit SetFloat(float a_value) : m_value(a_value) {}
            Type  GetType() override { return Type::Float; }
            float Float() override { return m_value; }

        private:
            float m_value;
        };

        // ⚠ THE VISITOR IS CALLED FOR WHICHEVER TYPE THE STORE HOLDS, not for
        // the one asked for. A tint written by another mod as a float would
        // arrive through Float and not Int, so each hook records what it saw
        // and the caller decides whether that is usable, rather than every
        // unhandled type quietly reading as zero.
        class Reader final : public IOverrideInterface::GetVariant {
        public:
            void Int(const skee_i32 a_i) override {
                gotInt = true;
                i      = a_i;
            }
            void Float(const float a_f) override {
                gotFloat = true;
                f        = a_f;
            }
            void String(const char* a_str) override {
                gotString = true;
                if (a_str) {
                    s = a_str;
                }
            }
            void Bool(const bool a_b) override {
                gotBool = true;
                b       = a_b;
            }
            void TextureSet(const BGSTextureSet*) override { gotTextureSet = true; }

            bool        gotInt{ false };
            bool        gotFloat{ false };
            bool        gotString{ false };
            bool        gotBool{ false };
            bool        gotTextureSet{ false };
            skee_i32    i{ 0 };
            float       f{ 0.0f };
            bool        b{ false };
            std::string s;
        };

        // Every call goes through these two so the index rule in OverlayPlan is
        // applied in one place. See the ⚠⚠ on OverlayPlan::IndexFor: skee
        // normalises the index when it stores an override and not when it looks
        // one up, so a read or a remove with the wrong index silently finds
        // nothing.
        void PutString(TESObjectREFR* a_refr, bool a_female, const char* a_node,
                       std::uint16_t a_key, std::uint8_t a_slot, const std::string& a_value) {
            SetString value{ a_value };
            g_override->AddNodeOverride(a_refr, a_female, a_node, a_key,
                                        OverlayPlan::IndexFor(a_key, a_slot), value);
        }

        void Drop(TESObjectREFR* a_refr, bool a_female, const char* a_node,
                  std::uint16_t a_key, std::uint8_t a_slot) {
            g_override->RemoveNodeOverride(a_refr, a_female, a_node, a_key,
                                           OverlayPlan::IndexFor(a_key, a_slot));
        }

        [[nodiscard]] bool Fetch(TESObjectREFR* a_refr, bool a_female, const char* a_node,
                                 std::uint16_t a_key, std::uint8_t a_slot, Reader& a_out) {
            return g_override->GetNodeOverride(a_refr, a_female, a_node, a_key,
                                               OverlayPlan::IndexFor(a_key, a_slot), a_out);
        }

        void BuildLayerList() {
            OverlayPlan::Formats formats;
            OverlayPlan::Counts  counts{};
            for (const auto& info : OverlayPlan::kLocations) {
                const auto slot     = OverlayPlan::Slot(info.location);
                const auto skeeLoc  = ToSkee(info.location);
                counts[slot]        = g_overlay->GetOverlayCount(
                    IOverlayInterface::OverlayType::Normal, skeeLoc);
                const char* format = g_overlay->GetOverlayFormat(
                    IOverlayInterface::OverlayType::Normal, skeeLoc);
                formats[slot] = format ? format : "";
            }
            g_layers = OverlayPlan::BuildLayers(formats, counts);

            // ⚠ SPELL OVERLAYS ARE NOT LISTED, ON PURPOSE. They are the slots
            // magic effects drive, they are installed and cleared by whatever
            // owns the effect, and a page that let a player paint one would be
            // authoring state another system overwrites without warning.
            spdlog::info("Overlays: {} layers (face {}, body {}, hands {}, feet {}).",
                         g_layers.size(),
                         counts[OverlayPlan::Slot(OverlayPlan::Location::kFace)],
                         counts[OverlayPlan::Slot(OverlayPlan::Location::kBody)],
                         counts[OverlayPlan::Slot(OverlayPlan::Location::kHands)],
                         counts[OverlayPlan::Slot(OverlayPlan::Location::kFeet)]);
        }

    }  // namespace

    void Request() {
        if (g_overlay && g_override) {
            return;
        }
        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            g_status = Status::kNoMessaging;
            spdlog::error("Overlays: SKSE messaging unavailable; RaceMenu not acquired.");
            return;
        }

        InterfaceExchangeMessage exchange{};
        messaging->Dispatch(
            static_cast<std::uint32_t>(InterfaceExchangeMessage::kMessage_ExchangeInterface),
            &exchange, sizeof(exchange), "skee");
        if (!exchange.interfaceMap) {
            g_status = Status::kRaceMenuAbsent;
            spdlog::warn("Overlays: RaceMenu interface exchange got no map; the Overlays "
                         "page is unavailable this session.");
            return;
        }

        auto* overlay = static_cast<IOverlayInterface*>(
            exchange.interfaceMap->QueryInterface("Overlay"));
        if (!overlay) {
            g_status = Status::kNoOverlay;
            spdlog::warn("Overlays: RaceMenu has no Overlay interface.");
            return;
        }
        auto* overrides = static_cast<IOverrideInterface*>(
            exchange.interfaceMap->QueryInterface("Override"));
        if (!overrides) {
            g_status = Status::kNoOverride;
            spdlog::warn("Overlays: RaceMenu has no Override interface; the slots exist "
                         "and nothing could be painted into them.");
            return;
        }

        const auto overlayVersion  = overlay->GetVersion();
        const auto overrideVersion = overrides->GetVersion();
        if (overlayVersion < IOverlayInterface::kCurrentPluginVersion ||
            overrideVersion < IOverrideInterface::kCurrentPluginVersion) {
            g_status = Status::kTooOld;
            spdlog::error("Overlays: RaceMenu Overlay v{} / Override v{} is older than the "
                          "v{} / v{} this build was written against; no overlay calls will "
                          "be made. Another mod is probably shipping an older skee64.dll.",
                          overlayVersion, overrideVersion,
                          static_cast<std::uint32_t>(IOverlayInterface::kCurrentPluginVersion),
                          static_cast<std::uint32_t>(
                              IOverrideInterface::kCurrentPluginVersion));
            return;
        }

        g_overlay  = overlay;
        g_override = overrides;
        BuildLayerList();

        // ⚠ NO SLOTS MEANS THE FEATURE IS TURNED OFF, NOT THAT SOMETHING BROKE.
        // bEnableOverlays=0 in skee64.ini is a supported choice, and it arrives
        // here as every count reading zero. Saying so beats an empty page.
        if (g_layers.empty()) {
            g_status = Status::kOverlaysDisabled;
            spdlog::warn("Overlays: RaceMenu reports no overlay slots at all; overlays are "
                         "switched off in skee64.ini.");
            return;
        }

        g_status = Status::kReady;
        spdlog::info("Overlays: RaceMenu Overlay v{} and Override v{} acquired.",
                     overlayVersion, overrideVersion);
    }

    void RegisterInstallCallback() {
        // OS-226 stage S0. ⚠⚠ THE RETURN VALUE IS A MEASUREMENT, NOT A CHECK.
        // The interface reports version 2 here, so the slot exists in the
        // shipped vtable; whether registering does anything, and whether the
        // callback is ever called, are two further facts and neither can be read
        // off the header. S3 has to redo its material swap after every install
        // or Fitting Room becomes the second painter of an appearance skee
        // repaints, so a registration that returns true and never fires is the
        // failure that would sink it silently. Log both, separately.
        //
        // ⚠⚠ NOT CALLED FROM Request, AND THE FIELD ROUND OF 20:51 IS WHY. An
        // earlier version armed this off a [Debug] key; Request runs at
        // kPostPostLoad and Settings::Load runs at kDataLoaded, so the arming
        // question was asked one message too early and answered with the
        // compiled default. The log said `registration not attempted` while the
        // key was plainly bound and the probe itself was plainly running, which
        // is the same class of mistake as reading any other mutable predicate
        // before it is loaded. The interface is acquired by then and skee does
        // not mind being told late.
        //
        // ⚠ UNCONDITIONAL SINCE OS-233 (2026-08-19). It used to be armed by the
        // probe key alone; the reconcile's trigger rides the same callback, and
        // a repair that only runs on a rig with a debug key bound is no repair.
        if (!Available()) {
            return;
        }
        const bool registered =
            g_overlay->RegisterInstallCallback("FittingRoom", &OnOverlayInstalled);
        spdlog::info("Overlays: RegisterInstallCallback(\"FittingRoom\") returned {}. That says "
                     "the call was accepted and says nothing about whether the callback will "
                     "ever fire; OverlayReconcile arms off its Face fires.",
                     registered);
    }

    Status GetStatus() { return g_status; }

    bool Available() { return g_status == Status::kReady; }

    std::uint32_t OverlayVersion() { return g_overlay ? g_overlay->GetVersion() : 0u; }

    std::uint32_t OverrideVersion() { return g_override ? g_override->GetVersion() : 0u; }

    const std::vector<OverlayPlan::Layer>& Layers() { return g_layers; }

    bool HasOverlays(RE::Actor* a_actor) {
        if (!Available() || !a_actor) {
            return false;
        }
        return g_overlay->HasOverlays(AsRaceMenuRef(a_actor));
    }

    void Install(RE::Actor* a_actor) {
        if (!Available() || !a_actor) {
            return;
        }
        const auto handle = a_actor->GetHandle();
        auto*      task   = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        task->AddTask([handle] {
            const auto ptr   = handle.get();
            auto*      actor = ptr ? ptr.get() : nullptr;
            if (!actor || !g_overlay) {
                return;
            }
            auto* const refr = AsRaceMenuRef(actor);
            if (g_overlay->HasOverlays(refr)) {
                return;
            }
            // ⚠ DEFERRED, NOT IMMEDIATE. This clones the skin geometry once per
            // slot, which on the reference rig is fifteen clones, and the whole
            // point of being on the game thread is not to hold it for all of
            // them. skee's own installer defers for the same reason.
            g_overlay->AddOverlays(refr, true);
            spdlog::info("Overlays: installed slots on '{}'.",
                         actor->GetName() ? actor->GetName() : "(unnamed)");
        });
    }

    OverlayPlan::LayerState Read(RE::Actor* a_actor, const std::string& a_node) {
        OverlayPlan::LayerState state;
        if (!Available() || !a_actor || a_node.empty()) {
            return state;
        }
        auto* const refr   = AsRaceMenuRef(a_actor);
        const bool  female = IsFemaleActor(a_actor);

        Reader diffuse;
        if (Fetch(refr, female, a_node.c_str(), OverlayPlan::kKeyTexture,
                  OverlayPlan::kSlotDiffuse, diffuse) &&
            diffuse.gotString) {
            state.hasTexture = true;
            state.texture    = diffuse.s;
            // ⚠ A BAKED PATH IS DECODED BACK INTO ITS ART AND ITS TRANSFORM
            // (OS-209), through the sidecar beside the file. The page then
            // shows the art the player picked and the sliders where they left
            // them. A bake whose sidecar is gone stays as it is: still a
            // texture, still shown, its transform read as identity, so moving
            // it again resamples the bake rather than the art. Said once per
            // path so a page redraw does not repeat it.
            if (OverlayTransform::IsBakedPath(state.texture)) {
                const auto decoded = OverlayBake::ReadSidecar(state.texture);
                if (decoded.ok) {
                    state.transform = decoded.transform;
                    state.texture   = decoded.source;
                } else {
                    static std::unordered_set<std::string> s_said;
                    if (s_said.insert(state.texture).second) {
                        spdlog::warn("Overlays: '{}' on '{}' is a baked overlay with no sidecar "
                                     "beside it, so its art and position cannot be read back; "
                                     "shown as it is.",
                                     state.texture, a_node);
                    }
                }
            }
        }
        Reader normal;
        if (Fetch(refr, female, a_node.c_str(), OverlayPlan::kKeyTexture,
                  OverlayPlan::kSlotNormal, normal) &&
            normal.gotString) {
            state.hasNormal = true;
            state.normal    = normal.s;
        }
        Reader tint;
        if (Fetch(refr, female, a_node.c_str(), OverlayPlan::kKeyTint,
                  OverlayPlan::kSlotDiffuse, tint) &&
            tint.gotInt) {
            state.hasTint = true;
            state.tint    = OverlayPlan::UnpackTint(static_cast<std::uint32_t>(tint.i));
        }
        Reader alpha;
        if (Fetch(refr, female, a_node.c_str(), OverlayPlan::kKeyAlpha,
                  OverlayPlan::kSlotDiffuse, alpha) &&
            alpha.gotFloat) {
            state.hasAlpha = true;
            state.alpha    = OverlayPlan::ClampAlpha(alpha.f);
        }
        // The glow, so the control opens on what the layer is actually wearing
        // rather than on this page's defaults. A layer RaceMenu set up, or one
        // that arrived with a preset, has both keys and they are read as one.
        Reader emissive;
        if (Fetch(refr, female, a_node.c_str(), OverlayPlan::kKeyEmissiveColour,
                  OverlayPlan::kSlotDiffuse, emissive) &&
            emissive.gotInt) {
            state.glow = OverlayPlan::UnpackTint(static_cast<std::uint32_t>(emissive.i));
        }
        // The finish (OS-226 S1). ⚠ EITHER KEY PRESENT MEANS THE LAYER CARRIES
        // ONE, and the other falls back to the material's own value rather than
        // to zero. Fifteen installed presets write key 2 and fifteen write key
        // 3; nothing guarantees the same layer carries both, and a missing
        // specular read as 0 would show a preset's glossy layer as matte in the
        // page while it renders shiny on the character.
        Reader gloss;
        if (Fetch(refr, female, a_node.c_str(), OverlayPlan::kKeyGloss,
                  OverlayPlan::kSlotDiffuse, gloss) &&
            gloss.gotFloat) {
            state.hasFinish = true;
            state.gloss     = OverlayPlan::ClampGloss(gloss.f);
        }
        Reader specular;
        if (Fetch(refr, female, a_node.c_str(), OverlayPlan::kKeySpecular,
                  OverlayPlan::kSlotDiffuse, specular) &&
            specular.gotFloat) {
            state.hasFinish = true;
            state.specular  = OverlayPlan::ClampSpecular(specular.f);
        }
        return state;
    }

    namespace {

        // The write proper: every key of one layer, then one push. a_diffuse is
        // what key 9 gets, the source or the bake of it, decided by the caller.
        void WriteNow(RE::ActorHandle a_handle, const std::string& a_node,
                      const OverlayPlan::LayerState& a_state, const std::string& a_diffuse) {
            auto* task = SKSE::GetTaskInterface();
            if (!task) {
                return;
            }
            task->AddTask([handle = a_handle, node = a_node, state = a_state,
                           diffuse = a_diffuse] {
                const auto ptr   = handle.get();
                auto*      actor = ptr ? ptr.get() : nullptr;
                if (!actor || !g_override) {
                    return;
                }
                auto* const refr   = AsRaceMenuRef(actor);
                const bool  female = IsFemaleActor(actor);

                PutString(refr, female, node.c_str(), OverlayPlan::kKeyTexture,
                          OverlayPlan::kSlotDiffuse, diffuse);

                // ⚠ THE NORMAL IS REMOVED RATHER THAN BLANKED WHEN IT IS OFF. An
                // empty string in slot 1 is a texture path the engine cannot
                // resolve, which lands on the placeholder and lights the overlay
                // wrongly; dropping the override lets the slot fall back to the
                // skin's own normal, which is what an overlay is supposed to wear.
                if (state.hasNormal && !state.normal.empty()) {
                    PutString(refr, female, node.c_str(), OverlayPlan::kKeyTexture,
                              OverlayPlan::kSlotNormal, state.normal);
                } else {
                    Drop(refr, female, node.c_str(), OverlayPlan::kKeyTexture,
                         OverlayPlan::kSlotNormal);
                }

                // ⚠ THE ALPHA GOES IN TWICE, HERE AND UNDER KEY 8, because that is
                // what RaceMenu's own controls wrote into every preset on disk.
                // See the measurement above OverlayPlan::PackTint: sending the
                // colour with a zero alpha byte is what made black unreachable.
                const auto packedTint = OverlayPlan::PackTint(
                    state.tint.r, state.tint.g, state.tint.b,
                    OverlayPlan::AlphaByte(OverlayPlan::ClampAlpha(state.alpha)));
                SetInt tint{ static_cast<skee_i32>(packedTint) };
                g_override->AddNodeOverride(
                    refr, female, node.c_str(), OverlayPlan::kKeyTint,
                    OverlayPlan::IndexFor(OverlayPlan::kKeyTint, 0), tint);

                SetFloat alpha{ OverlayPlan::ClampAlpha(state.alpha) };
                g_override->AddNodeOverride(
                    refr, female, node.c_str(), OverlayPlan::kKeyAlpha,
                    OverlayPlan::IndexFor(OverlayPlan::kKeyAlpha, 0), alpha);

                // ⚠⚠ THE GLOW IS ITS OWN COLOUR AND IT IS WRITTEN ON EVERY LAYER,
                // AND THIS IS WHY A BLACK OVERLAY CAME OUT WHITE. Field, 2026-08-16:
                // "some overlays can be dyed and appear black, but some can't and
                // they still appear quite white even when the color selected is
                // pure black", and then: "we have a glow map on some overlays, in
                // racemenu glow map had it's own color".
                //
                // What a shape shows is its tinted diffuse PLUS its emissive. The
                // emissive half is inherited from whatever the overlay was cloned
                // off, so a layer whose diffuse is tinted black still emits, and
                // whether it does depends on the material underneath rather than on
                // anything this page did. That is exactly "some can and some
                // cannot".
                //
                // ⚠ BOTH KEYS, ALWAYS, AND THE STRENGTH DEFAULTS TO ZERO. Not
                // writing them is RaceMenu's own behaviour and it leaves the
                // inherited glow in place, which is the bug. Writing a strength of
                // zero makes the colour control mean what it says on every layer.
                // The cost is real and was accepted rather than hidden: a pack
                // whose art is meant to glow does not glow until the player raises
                // this, and the control is there to raise.
                //
                // ⚠ THE STRENGTH RIDES IN THE COLOUR'S ALPHA BYTE TOO. RaceMenu's
                // OnOverlayGlowColorChange packs it that way and divides by ten to
                // get the multiple, so a glow written any other way reads back
                // wrong in its own UI.
                const auto glowAlpha  = OverlayPlan::GlowAlphaByte(state.glowStrength);
                const auto packedGlow = OverlayPlan::PackTint(state.glow.r, state.glow.g,
                                                              state.glow.b, glowAlpha);
                SetInt     emissive{ static_cast<skee_i32>(packedGlow) };
                g_override->AddNodeOverride(
                    refr, female, node.c_str(), OverlayPlan::kKeyEmissiveColour,
                    OverlayPlan::IndexFor(OverlayPlan::kKeyEmissiveColour, 0), emissive);

                SetFloat multiple{ OverlayPlan::ClampGlow(state.glowStrength) };
                g_override->AddNodeOverride(
                    refr, female, node.c_str(), OverlayPlan::kKeyEmissiveMultiple,
                    OverlayPlan::IndexFor(OverlayPlan::kKeyEmissiveMultiple, 0), multiple);

                // ⚠⚠ THE FINISH IS DROPPED WHEN IT IS OFF, NOT WRITTEN AT THE
                // DEFAULT, AND THAT IS DELIBERATELY THE OPPOSITE OF THE GLOW
                // TWO FIELDS UP (OS-226 S1). The glow is written on every layer
                // because an inherited emissive is a fault. An inherited finish
                // is not: skee copies the SOURCE SKIN's material values onto the
                // clone at install, measured 2026-08-18, so an untouched layer
                // is already wearing the body's own glossiness. Writing 30 and 3
                // over that would take a shiny body's overlays down to a
                // constant nobody asked for. Removing the override is what the
                // normal map does above, for the same reason and in the same
                // words: let the slot fall back to the skin's.
                // ⚠⚠ THE FINISH SAYS WHAT IT WROTE, AND THE FIRST FIELD ROUND
                // IS WHY. "I played with the sliders and did not notice any
                // changes" is three different faults wearing one face: the page
                // never wrote, skee took the write and does not apply these two
                // keys, or both happened and the shape simply does not look
                // different. Nothing in the log separated them, so the round
                // could not be read at all. One line per actual change does.
                //
                // ⚠ DEDUPED, NOT PER CALL. Write runs on every frame of a drag,
                // so an undeduped line would bury the log in a smear of near
                // identical numbers. Game thread only, which is what makes a
                // plain static safe here.
                {
                    struct Last {
                        bool  has{ false };
                        float gloss{ 0.0f };
                        float specular{ 0.0f };
                        bool  seen{ false };
                    };
                    static std::unordered_map<std::string, Last> s_lastFinish;
                    auto&      last = s_lastFinish[node];
                    const Last now{ state.hasFinish, OverlayPlan::ClampGloss(state.gloss),
                                    OverlayPlan::ClampSpecular(state.specular), true };
                    if (!last.seen || last.has != now.has || last.gloss != now.gloss ||
                        last.specular != now.specular) {
                        if (now.has) {
                            spdlog::info("Overlays: finish on '{}' -> gloss {:.1f}, specular "
                                         "{:.2f} (skee keys 2 and 3).",
                                         node, now.gloss, now.specular);
                        } else if (last.seen) {
                            spdlog::info("Overlays: finish on '{}' removed, so the layer takes "
                                         "the skin's own again.",
                                         node);
                        }
                        last = now;
                    }
                }

                if (state.hasFinish) {
                    SetFloat gloss{ OverlayPlan::ClampGloss(state.gloss) };
                    g_override->AddNodeOverride(
                        refr, female, node.c_str(), OverlayPlan::kKeyGloss,
                        OverlayPlan::IndexFor(OverlayPlan::kKeyGloss, 0), gloss);

                    SetFloat specular{ OverlayPlan::ClampSpecular(state.specular) };
                    g_override->AddNodeOverride(
                        refr, female, node.c_str(), OverlayPlan::kKeySpecular,
                        OverlayPlan::IndexFor(OverlayPlan::kKeySpecular, 0), specular);
                } else {
                    Drop(refr, female, node.c_str(), OverlayPlan::kKeyGloss,
                         OverlayPlan::kSlotDiffuse);
                    Drop(refr, female, node.c_str(), OverlayPlan::kKeySpecular,
                         OverlayPlan::kSlotDiffuse);
                }

                // ⚠ ONE PUSH AFTER THE WHOLE LAYER, never one per key. Same rule
                // NodeTransformApi holds for its single scenegraph update: the
                // per-value version costs a walk for each of four writes and shows
                // the character mid-way through its own edit.
                // ⚠ DEFERRED, WHICH IS THE SHIPPING FORM. It was immediate for one
                // stint so a probe could read the material in the same pass it was
                // written; that measurement is taken and lives in
                // docs/handoffs/2026-08-16-makeup-tint-masks-measured.md. An
                // immediate apply costs a scenegraph walk inside the write and buys
                // the feature nothing.
                g_override->SetNodeProperties(refr, false);
                // ⚠⚠ AND THIS IS THE EDIT THE FIELD CAUGHT TAKING THE FACE
                // AWAY. Writing a layer makes skee re-apply this actor's node
                // overrides, and one of the overrides it holds for the head is
                // the tint slot, pointing at a preset's exported file. About a
                // second later the head stops wearing the live bake and the
                // skin tone the player just picked is gone from the FACE while
                // the body still has it. Armed for a BODY layer too, on
                // purpose: the field reported both, the arming is a timestamp,
                // and the check that follows costs nothing when the head is
                // still wearing ours.
                MakeupApi::ArmDelayedFaceRebake();
            });
        }

    }  // namespace

    void Write(RE::Actor* a_actor, const std::string& a_node,
               const OverlayPlan::LayerState& a_state) {
        if (!Available() || !a_actor || a_node.empty()) {
            return;
        }
        // ⚠ THE RECORD IS TAKEN HERE BECAUSE EVERY WRITE IS HERE. Undo, redo, a
        // paste, the Overlays page, a look apply and a save load all arrive at
        // this one function, so a copy taken at this line cannot drift from
        // what was painted the way a poller or a capture button would
        // (OverlayBaseline.h carries the fault it exists for). It records the
        // STATE, not the wire path: the bake below is a derived file and the
        // art the player picked is what has to survive a load.
        OverlayBaseline::Note(a_actor, a_node, a_state);
        const auto handle = a_actor->GetHandle();

        // ⚠⚠ A MOVED LAYER'S PATH GOES IN ONLY ONCE ITS FILE EXISTS (OS-209).
        // OverlayBake::Ensure continues on the game thread with true when the
        // bake is on disk (found, or just written) and with false when it could
        // not be made; on false the SOURCE goes in, so the art shows where its
        // author drew it rather than as a placeholder, and the log says why.
        // An identity transform is the source and continues at once. Every
        // Write goes through here, so undo, redo, a paste and a save load all
        // re-bake an evicted file the same way the slider does.
        if (a_state.hasTexture && !a_state.texture.empty() &&
            !OverlayTransform::IsIdentity(a_state.transform)) {
            const auto wire = OverlayPlan::WirePath(a_state);
            OverlayBake::Ensure(a_state.texture, a_state.transform,
                                [handle, node = a_node, state = a_state, wire](bool a_ok) {
                                    if (a_ok) {
                                        WriteNow(handle, node, state, wire);
                                        // The field probe: what the [Ovl] material
                                        // actually shows two seconds from now.
                                        OverlayBake::ProbeLater(handle, node, wire);
                                    } else {
                                        spdlog::warn("Overlays: the bake for '{}' on '{}' could "
                                                     "not be made, so the layer shows the art "
                                                     "as authored.",
                                                     state.texture, node);
                                        WriteNow(handle, node, state, state.texture);
                                    }
                                });
            return;
        }
        WriteNow(handle, a_node, a_state, OverlayPlan::WirePath(a_state));
    }

    void Clear(RE::Actor* a_actor, const std::string& a_node) {
        if (!Available() || !a_actor || a_node.empty()) {
            return;
        }
        // ⚠⚠ AND THE RECORD COMES OFF HERE, FOR THE REASON WRITE PUTS IT ON
        // THERE. Every clear in the mod arrives at this one function, so the
        // eraser is as single as the mark. Without it the baseline kept naming
        // art nobody was painting and put it back on the next empty store, 196
        // ms after a look import had taken it off; OverlayBaseline.h carries
        // the round that measured it. Before the marshal, where Write's Note
        // sits, so the pair cannot drift apart in ordering either.
        OverlayBaseline::Forget(a_actor, a_node);
        const auto handle = a_actor->GetHandle();
        auto*      task   = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        task->AddTask([handle, node = a_node] {
            const auto ptr   = handle.get();
            auto*      actor = ptr ? ptr.get() : nullptr;
            if (!actor || !g_override) {
                return;
            }
            auto* const refr   = AsRaceMenuRef(actor);
            const bool  female = IsFemaleActor(actor);

            Drop(refr, female, node.c_str(), OverlayPlan::kKeyTint, 0);
            Drop(refr, female, node.c_str(), OverlayPlan::kKeyAlpha, 0);
            // ⚠ THE EMISSIVE COLOUR COMES OFF WITH THE TINT, or an emptied
            // layer keeps this page's last colour on a slot it no longer
            // paints, and the next texture dropped into that slot inherits it.
            // Write puts them on together and Clear has to take them off
            // together.
            Drop(refr, female, node.c_str(), OverlayPlan::kKeyEmissiveColour, 0);
            Drop(refr, female, node.c_str(), OverlayPlan::kKeyEmissiveMultiple, 0);
            Drop(refr, female, node.c_str(), OverlayPlan::kKeyTexture,
                 OverlayPlan::kSlotNormal);

            // ⚠ THE DIFFUSE IS SET TO THE DEFAULT RATHER THAN REMOVED. Dropping
            // the override leaves whatever art the slot is currently showing on
            // the character until something rebuilds it, because removing a
            // stored value does not repaint anything. Writing the texture skee
            // itself uses for a reset slot makes the layer go away now.
            PutString(refr, female, node.c_str(), OverlayPlan::kKeyTexture,
                      OverlayPlan::kSlotDiffuse,
                      std::string{ OverlayPlan::kDefaultTexture });

            g_override->SetNodeProperties(refr, false);
        });
    }

    namespace {

        // ⚠ A QUARTER SECOND, PUSHED OUT ON EVERY REPAINT RATHER THAN
        // ACCUMULATED. A skin tone drag paints on every frame, the overlays are
        // wrong for the length of the drag either way, and what this must not do
        // is run skee's whole node walk once per frame of it. Re-arming resets
        // the clock so the push lands once after the last repaint.
        constexpr auto kNodePushDelay = std::chrono::milliseconds(250);

        RE::ActorHandle                       g_pushActor{};
        std::chrono::steady_clock::time_point g_pushDueAt{};
        bool                                  g_pushArmed{ false };

        // ⚠ A DEADLINE RATHER THAN A FLAG, the same shape the face watch uses
        // and for the same measured reason: a look apply keeps repainting after
        // its steps report done, so a flag cleared at the end of the overlays
        // step would be cleared before the repaint that re-arms the push.
        std::chrono::steady_clock::time_point g_pushQuietUntil{};

    }

    void StandDownNodePush(int a_milliseconds) {
        g_pushArmed      = false;
        g_pushQuietUntil = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(a_milliseconds);
        spdlog::info("Overlays: the layer push stands down for {} ms, because these layers "
                     "were just chosen rather than remembered.",
                     a_milliseconds);
    }

    void ArmNodePropertyPush(RE::Actor* a_actor) {
        if (!Available() || !a_actor) {
            return;
        }
        // ⚠⚠ THE QUIET PERIOD OUTRANKS EVERY ARMING EDGE, because the thing it
        // protects is a decision and the thing it refuses is a memory.
        if (std::chrono::steady_clock::now() < g_pushQuietUntil) {
            return;
        }
        g_pushActor = a_actor->GetHandle();
        g_pushDueAt = std::chrono::steady_clock::now() + kNodePushDelay;
        g_pushArmed = true;
    }

    void RunNodePropertyPush() {
        if (!g_pushArmed || std::chrono::steady_clock::now() < g_pushDueAt) {
            return;
        }
        // Belt as well as braces: a push armed a frame before the stand-down
        // must not get in afterwards.
        if (std::chrono::steady_clock::now() < g_pushQuietUntil) {
            g_pushArmed = false;
            return;
        }
        g_pushArmed       = false;
        const auto  ptr   = g_pushActor.get();
        auto* const actor = ptr ? ptr.get() : nullptr;
        if (!actor) {
            return;
        }
        // ⚠ AN INSTRUMENT AND THE FIX IN ONE LINE. "the skin was repainted"
        // and "we put the layers back" are different claims, and the field needs
        // to see the second one to know this ran at all.
        spdlog::debug("Overlays: the skin was repainted, so the character's "
                      "stored layer appearance is pushed back onto it.");
        PushNodeProperties(actor);
    }

    void PushNodeProperties(RE::Actor* a_actor) {
        if (!Available() || !a_actor) {
            return;
        }
        const auto handle = a_actor->GetHandle();
        auto*      task   = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        task->AddTask([handle] {
            const auto ptr   = handle.get();
            auto*      actor = ptr ? ptr.get() : nullptr;
            if (!actor || !g_override) {
                return;
            }
            g_override->SetNodeProperties(AsRaceMenuRef(actor), false);
        });
    }

}  // namespace OS::OverlayApi
