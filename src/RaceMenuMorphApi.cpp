#include "RaceMenuMorphApi.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <utility>

// RaceMenu's public modder header deliberately uses opaque global Skyrim type
// declarations. Keep it in this one translation unit and bridge the actor
// pointer at the call boundary; no RaceMenu type escapes through our header.
#include "../extern/RaceMenu/IPluginInterface.h"

namespace OS::RaceMenuMorphApi {

    namespace {
        IBodyMorphInterface* g_bodyMorphs{ nullptr };
        Status               g_status{ Status::kNotRequested };

        // ⚠ THE FLOOR IS THE VERSION WE AUDITED, NOT THE HEADER'S
        // kCurrentPluginVersion. That constant means "the version this header
        // was published at", so keying a floor on it silently demands the
        // newest skee64.dll in existence from every user, and rejects the one
        // most of them actually have.
        //
        // It did exactly that. BodyMorph is v5 only in RaceMenu 0.4.20.0, and
        // ANY mod that bundles an older skee64.dll wins the overwrite and
        // downgrades the whole load order: on the reference install RM Tags
        // ships 0.4.19.16, sits above RaceMenu in the priority order, and took
        // BodyMorph back to v4. The Bodies page and the body fit filter then do
        // nothing for reasons no user can see. NiTransform never showed this
        // because its header version and its shipped version both happen to be
        // 3.
        //
        // v4 IS SAFE BECAUSE THE VTABLE IS IDENTICAL WHERE WE TOUCH IT. Audited
        // 2026-08-08 across three shipped builds - 0.4.20.0 (v5, AE),
        // 0.4.19.16 (v4, AE) and 4.0.0.0 (v4, SE 1.5.97) - by walking each
        // BodyMorphInterface vtable from its RTTI locator: slots 0..24 carry
        // matching prologues in all three, with SetCacheLimit landing on slot
        // 15 as `mov [rcx+0xA0], rdx; ret` in each, which is what pins the
        // alignment rather than assuming it. Everything below is inside that
        // range - SetMorph 3, GetMorph 4, ClearMorph 5, ApplyBodyMorphs 13,
        // HasBodyMorphKey 20, ClearBodyMorphKeys 21. v5's one addition,
        // AddMorphShapeCallback, is slot 25 and appears nowhere in this plugin.
        //
        // ⚠ DO NOT LOWER IT FURTHER WITHOUT REPEATING THAT AUDIT. v4 is the
        // oldest build measured, not a version known to be a safe floor; the SE
        // 4.0.0.0 build already has one fewer slot than its AE contemporary, so
        // the two lines do not track each other by version number alone.
        constexpr std::uint32_t kRequiredVersion = IBodyMorphInterface::kPluginVersion4;

        [[nodiscard]] TESObjectREFR* AsRaceMenuRef(RE::Actor* a_actor) {
            return reinterpret_cast<TESObjectREFR*>(a_actor);
        }

        [[nodiscard]] ApplyResult Refresh(IBodyMorphInterface& a_api,
                                          RE::Actor* a_actor,
                                          std::size_t a_valuesSet) {
            ApplyResult result;
            result.available = true;
            result.valuesSet = a_valuesSet;
            if (!a_actor) {
                return result;
            }
            result.actorLoaded = a_actor->Is3DLoaded();
            if (result.actorLoaded) {
                a_api.ApplyBodyMorphs(AsRaceMenuRef(a_actor), true);
                result.refreshed = true;
            }
            return result;
        }
    }  // namespace

    void Request() {
        if (g_bodyMorphs) {
            return;
        }
        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            g_status = Status::kNoMessaging;
            spdlog::error("Body Studio proof: SKSE messaging unavailable; RaceMenu not acquired.");
            return;
        }

        InterfaceExchangeMessage exchange{};
        messaging->Dispatch(
                            static_cast<std::uint32_t>(
                                InterfaceExchangeMessage::kMessage_ExchangeInterface),
                            &exchange, sizeof(exchange), "skee");
        if (!exchange.interfaceMap) {
            g_status = Status::kRaceMenuAbsent;
            spdlog::warn("Body Studio proof: RaceMenu interface exchange got no map.");
            return;
        }
        auto* candidate = static_cast<IBodyMorphInterface*>(
            exchange.interfaceMap->QueryInterface("BodyMorph"));
        if (!candidate) {
            g_status = Status::kNoBodyMorph;
            spdlog::warn("Body Studio proof: RaceMenu has no BodyMorph interface.");
            return;
        }
        const auto version = candidate->GetVersion();
        if (version < kRequiredVersion) {
            g_status = Status::kTooOld;
            // ⚠ NAMES THE OVERWRITE, because that is what it always is. A user
            // who reads "too old" goes and updates RaceMenu, finds it already
            // current, and reports the bug again. The stale copy is another
            // mod's bundled one sitting above RaceMenu in the priority order.
            spdlog::error(
                "Body Studio proof: RaceMenu BodyMorph v{} is below the v{} floor. The "
                "loaded skee64.dll is older than RaceMenu's own - check whether another "
                "mod bundles skee64.dll and overwrites it. No morph calls will be made.",
                version, static_cast<std::uint32_t>(kRequiredVersion));
            return;
        }
        g_bodyMorphs = candidate;
        g_status     = Status::kReady;
        spdlog::info(
            "Body Studio proof: RaceMenu BodyMorph v{} acquired beside OBody; owned key='{}'.",
            version, kOwnedKey);
    }

    Status GetStatus() { return g_status; }

    bool Available() { return g_bodyMorphs != nullptr; }

    std::uint32_t Version() { return g_bodyMorphs ? g_bodyMorphs->GetVersion() : 0u; }

    ApplyResult ApplyOwned(RE::Actor* a_actor,
                           const std::vector<MorphValue>& a_values) {
        if (!g_bodyMorphs || !a_actor) {
            return {};
        }
        auto* refr = AsRaceMenuRef(a_actor);
        g_bodyMorphs->ClearBodyMorphKeys(refr, kOwnedKey);
        std::size_t applied = 0;
        for (const auto& morph : a_values) {
            if (morph.name.empty() || morph.value == 0.0f) {
                continue;
            }
            g_bodyMorphs->SetMorph(refr, morph.name.c_str(), kOwnedKey, morph.value);
            ++applied;
        }
        return Refresh(*g_bodyMorphs, a_actor, applied);
    }

    ApplyResult ClearOwned(RE::Actor* a_actor) {
        if (!g_bodyMorphs || !a_actor) {
            return {};
        }
        g_bodyMorphs->ClearBodyMorphKeys(AsRaceMenuRef(a_actor), kOwnedKey);
        return Refresh(*g_bodyMorphs, a_actor, 0);
    }

    bool HasOwned(RE::Actor* a_actor) {
        return g_bodyMorphs && a_actor &&
               g_bodyMorphs->HasBodyMorphKey(AsRaceMenuRef(a_actor), kOwnedKey);
    }

    ApplyResult ApplyPushUp(RE::Actor* a_actor,
                            const std::vector<MorphValue>& a_values) {
        if (!g_bodyMorphs || !a_actor) {
            return {};
        }
        auto* refr = AsRaceMenuRef(a_actor);
        // ⚠ CLEARED FIRST EVEN WHEN THE LIST IS EMPTY, which is what makes an
        // empty list mean "take it off". Changing outfits has to remove the
        // previous one's lift, and a body family with no recipe has to leave the
        // character alone rather than keep the last body's morphs on them.
        g_bodyMorphs->ClearBodyMorphKeys(refr, kPushUpKey);
        std::size_t applied = 0;
        for (const auto& morph : a_values) {
            if (morph.name.empty() || morph.value == 0.0f) {
                continue;
            }
            g_bodyMorphs->SetMorph(refr, morph.name.c_str(), kPushUpKey, morph.value);
            ++applied;
        }
        return Refresh(*g_bodyMorphs, a_actor, applied);
    }

    bool HasPushUp(RE::Actor* a_actor) {
        return g_bodyMorphs && a_actor &&
               g_bodyMorphs->HasBodyMorphKey(AsRaceMenuRef(a_actor), kPushUpKey);
    }

    namespace {

        // ⚠ TWO READS AND NOT ONE. Present at the first and gone at the second
        // is the rebuild clearing our key; present at both moves the whole
        // question onto the recipe's values, which have never been eyeballed in
        // game. One read could not tell those apart, and a single early read
        // could land before OBody's rebuild rather than after it.
        constexpr int         kPushUpProbeAtMs[] = { 500, 2500 };
        constexpr std::size_t kPushUpProbeCount  = 2;

        RE::ActorHandle                       g_probeActor{};
        std::size_t                           g_probeNext{ kPushUpProbeCount };
        std::size_t                           g_probeWritten{ 0 };
        std::chrono::steady_clock::time_point g_probeArmedAt{};

    }

    void ArmPushUpProbe(RE::Actor* a_actor, std::size_t a_written) {
        if (!a_actor) {
            return;
        }
        // ⚠ RE-ARMING RESTARTS THE CLOCK RATHER THAN QUEUEING. A stepper drag
        // writes several times in a second and every write gets its own rebuild,
        // so the reads that matter are the ones after the LAST write.
        g_probeActor   = a_actor->GetHandle();
        g_probeWritten = a_written;
        g_probeArmedAt = std::chrono::steady_clock::now();
        g_probeNext    = 0;
    }

    void RunPushUpProbe() {
        if (g_probeNext >= kPushUpProbeCount) {
            return;
        }
        const auto due = g_probeArmedAt +
                         std::chrono::milliseconds(kPushUpProbeAtMs[g_probeNext]);
        if (std::chrono::steady_clock::now() < due) {
            return;
        }
        const int elapsed = kPushUpProbeAtMs[g_probeNext];
        ++g_probeNext;
        const auto  ptr   = g_probeActor.get();
        auto* const actor = ptr ? ptr.get() : nullptr;
        if (!actor) {
            spdlog::debug("PushUp probe: the actor is gone {} ms after the write, "
                          "so the key cannot be read.",
                          elapsed);
            g_probeNext = kPushUpProbeCount;
            return;
        }
        spdlog::debug("PushUp probe: our morph key is {} {} ms after a write of {} "
                      "morph(s).",
                      HasPushUp(actor) ? "STILL THERE" : "GONE", elapsed,
                      g_probeWritten);
    }

    namespace {

        // Queue one piece of work against the actor, resolved inside the task.
        // ⚠ THE HANDLE TRAVELS, NOT THE POINTER. The Shape page draws through
        // FUCK's Present hook, so nothing here may dereference an actor on the
        // calling thread, and a handle also fails safe across a game load.
        template <typename Fn>
        [[nodiscard]] ApplyResult QueueOnActor(RE::Actor* a_actor, Fn&& a_fn) {
            ApplyResult result;
            if (!g_bodyMorphs || !a_actor) {
                return result;
            }
            result.available   = true;
            result.actorLoaded = a_actor->Is3DLoaded();
            auto* task         = SKSE::GetTaskInterface();
            if (!task) {
                return result;
            }
            const auto handle = a_actor->GetHandle();
            task->AddTask([handle, fn = std::forward<Fn>(a_fn)] {
                const auto ptr   = handle.get();
                auto*      actor = ptr ? ptr.get() : nullptr;
                if (!actor || !g_bodyMorphs) {
                    return;
                }
                auto* const refr = AsRaceMenuRef(actor);
                fn(*g_bodyMorphs, refr);
                // ⚠ deferUpdate TRUE, matching Refresh above. RaceMenu batches
                // the mesh rebuild rather than doing one per call, which is what
                // makes a slider drag affordable at all.
                g_bodyMorphs->ApplyBodyMorphs(refr, true);
            });
            result.refreshed = result.actorLoaded;
            return result;
        }

    }  // namespace

    ApplyResult SetShapeMorph(RE::Actor* a_actor, const std::string& a_name,
                              float a_value) {
        if (a_name.empty()) {
            return {};
        }
        auto result = QueueOnActor(
            a_actor, [name = a_name, a_value](IBodyMorphInterface& a_api,
                                              TESObjectREFR*       a_refr) {
                if (a_value == 0.0f) {
                    // ⚠ CLEARED RATHER THAN SET TO ZERO. A stored zero is a key
                    // on a character nobody shaped, and it turns up in anyone
                    // else's VisitMorphValues and in RaceMenu's co-save forever.
                    a_api.ClearMorph(a_refr, name.c_str(), kShapeKey);
                } else {
                    a_api.SetMorph(a_refr, name.c_str(), kShapeKey, a_value);
                }
            });
        result.valuesSet = a_value == 0.0f ? 0u : 1u;
        return result;
    }

    ApplyResult ClearShape(RE::Actor* a_actor) {
        return QueueOnActor(a_actor,
                            [](IBodyMorphInterface& a_api, TESObjectREFR* a_refr) {
                                a_api.ClearBodyMorphKeys(a_refr, kShapeKey);
                            });
    }

    ApplyResult ApplyShape(RE::Actor* a_actor, const std::vector<MorphValue>& a_values) {
        std::size_t applied = 0;
        for (const auto& m : a_values) {
            if (!m.name.empty() && m.value != 0.0f) {
                ++applied;
            }
        }
        auto result = QueueOnActor(
            a_actor, [values = a_values](IBodyMorphInterface& a_api,
                                         TESObjectREFR*       a_refr) {
                a_api.ClearBodyMorphKeys(a_refr, kShapeKey);
                for (const auto& m : values) {
                    if (m.name.empty() || m.value == 0.0f) {
                        continue;
                    }
                    a_api.SetMorph(a_refr, m.name.c_str(), kShapeKey, m.value);
                }
            });
        result.valuesSet = applied;
        return result;
    }

    std::vector<MorphValue> SnapshotShape(RE::Actor* a_actor) {
        if (!g_bodyMorphs || !a_actor) {
            return {};
        }
        // ⚠⚠ ENUMERATION, WHICH IS WHY THIS EXISTS BESIDE ReadShape. That one
        // needs a list of names to ask about, so it can only see what the
        // catalogue happens to describe; this walks what the actor ACTUALLY
        // holds. Preserving a shape across somebody else's wipe cannot be done
        // with a vocabulary, because the vocabulary is not the truth about this
        // character - and on a load order whose category files do not cover the
        // installed body, a name-driven snapshot would silently save only part
        // of the shape and "restore" the rest to nothing.
        //
        // ⚠ VisitMorphValues IS VTABLE SLOT 10, inside the v4/v5 range this
        // file's version note audited. Counting the pure virtuals: SetMorph 3,
        // GetMorph 4, ClearMorph 5, GetBodyMorphs 6, ClearBodyMorphNames 7,
        // VisitMorphs 8, VisitKeys 9, VisitMorphValues 10, ClearMorphs 11,
        // ApplyVertexDiff 12, ApplyBodyMorphs 13 - and 13 is where that audit
        // independently places ApplyBodyMorphs, which is what pins the count
        // rather than trusting it.
        class Collector final : public IBodyMorphInterface::MorphValueVisitor {
        public:
            std::vector<MorphValue> found;

            void Visit(TESObjectREFR*, const char* a_name, const char* a_key,
                       float a_value) override {
                if (!a_name || !a_key || a_value == 0.0f) {
                    return;
                }
                // Ours only. The actor is carrying OBody's key and quite
                // possibly two other mods' as well, and putting somebody else's
                // morphs back under OUR key would make this mod the author of
                // a shape it did not write.
                if (std::strcmp(a_key, kShapeKey) != 0) {
                    return;
                }
                found.push_back(MorphValue{ a_name, a_value });
            }
        } collector;
        g_bodyMorphs->VisitMorphValues(AsRaceMenuRef(a_actor), collector);
        return collector.found;
    }

    void LogKeyCensus(RE::Actor* a_actor, const char* a_moment) {
        if (!g_bodyMorphs || !a_actor) {
            return;
        }
        // Same visitor idiom and the same vtable-slot-10 audit note as
        // SnapshotShape above; the only difference is that NOTHING filters on
        // the key, because the key is the question.
        class Census final : public IBodyMorphInterface::MorphValueVisitor {
        public:
            struct PerKey {
                std::size_t count{ 0 };
                float       magnitude{ 0.0f };
            };
            std::map<std::string, PerKey> keys;
            std::map<std::string, std::vector<std::pair<std::string, float>>> byName;

            void Visit(TESObjectREFR*, const char* a_name, const char* a_key,
                       float a_value) override {
                if (!a_name || !a_key || a_value == 0.0f) {
                    return;
                }
                auto& k = keys[a_key];
                ++k.count;
                k.magnitude += a_value < 0.0f ? -a_value : a_value;
                byName[a_name].emplace_back(a_key, a_value);
            }
        } census;
        g_bodyMorphs->VisitMorphValues(AsRaceMenuRef(a_actor), census);
        for (const auto& [key, pk] : census.keys) {
            spdlog::info("BodyKeyCensus[{}]: key '{}' carries {} morph(s), |sum| {:.2f}.",
                         a_moment, key, pk.count, pk.magnitude);
        }
        std::size_t shared = 0;
        for (const auto& [name, rows] : census.byName) {
            if (rows.size() < 2) {
                continue;
            }
            ++shared;
            // A dozen named examples is a diagnosis; every one is a wall.
            if (shared <= 12) {
                std::string line;
                for (const auto& [key, value] : rows) {
                    line += fmt::format(" {}={:.2f}", key, value);
                }
                spdlog::info("BodyKeyCensus[{}]: '{}' written by {} key(s):{}",
                             a_moment, name, rows.size(), line);
            }
        }
        spdlog::info("BodyKeyCensus[{}]: {} name(s) carried by two or more keys.",
                     a_moment, shared);
    }

    std::vector<MorphValue> ReadShape(RE::Actor*                      a_actor,
                                      const std::vector<std::string>& a_names) {
        std::vector<MorphValue> out;
        if (!g_bodyMorphs || !a_actor) {
            return out;
        }
        auto* const refr = AsRaceMenuRef(a_actor);
        out.reserve(a_names.size());
        for (const auto& name : a_names) {
            if (name.empty()) {
                continue;
            }
            // ⚠ GetMorph AND NOT GetBodyMorphs. The second sums every key,
            // which would hand the page the outfit's OBody preset value as
            // though the user had set it, and the first drag would then write
            // that sum back under our key on top of the preset still supplying
            // it.
            const float value = g_bodyMorphs->GetMorph(refr, name.c_str(), kShapeKey);
            if (value != 0.0f) {
                out.push_back(MorphValue{ name, value });
            }
        }
        return out;
    }

}  // namespace OS::RaceMenuMorphApi
