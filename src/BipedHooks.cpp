#include "BipedHooks.h"

#include "ApparelPreviewSignal.h"  // stand the worn-mask shim down during a preview
#include "BipedPost.h"
#include "EditorWindow.h"   // the worn rule stands down for the actor the open editor stages
#include "WeaponPreview.h"  // a conjured shield lives only while its row is up
#include "NpcLookup.h"
#include "OutfitDye.h"
#include "OutfitSession.h"
#include "REAugments.h"
#include "MeasuredGhosts.h"  // what the dismember sweep measured about occupants
#include "RealWorn.h"
#include "Settings.h"
#include "SlotClaims.h"
#include "SlotMask.h"
#include "StyleCoverage.h"
#include "VersionCheck.h"

#include <array>
#include <atomic>
#include <functional>
#include <optional>

namespace OS {

    namespace {
        std::atomic<std::uint64_t> g_playerWornPass{ 0 };

        // ---- 24231+0x81: the call to 15856 (ExecuteVisitorOnWorn) ----
        using VisitWorn_t = void (*)(RE::InventoryChanges*, RE::InventoryChanges::IItemChangeVisitor&);
        VisitWorn_t g_origVisitWorn = nullptr;

        // RealWorn / SnapshotRealWorn moved to RealWorn.h (Task 11 follow-up)
        // so the rules engine's WorldWatch heartbeat can reuse the exact same
        // one-walk-over-entryList shape instead of reintroducing the 32x
        // GetWornArmor cost this file already profiled and abandoned. Same
        // logic, same reasoning, verbatim - see that header for the full
        // comment this one used to carry.

        // ---- what the last styling pass ACTUALLY achieved, per actor ----
        // The worn-mask shim runs in a different hook (24220) from the styling
        // pass (24231) and cannot stage geometry, so it cannot find out for
        // itself whether the engine accepted a style, or how far a hide grew
        // once whole pieces were expanded. Both numbers must be TOLD.
        //
        // ⚠ DO NOT REPLACE THIS WITH A PREDICATE. Two different shortcuts have
        // now been tried and both were wrong, from opposite directions:
        //
        //   * re-running our own fit check in the shim (OS-70c). FittingRoom.log
        //     2026-07-29 15:13 on a female Nord shows ApplyArmorAddon rejecting
        //     'Ahzidal', 'Boiled Netch Leather Helmet' and 'Bloodworm Helm'
        //     while StyleCatalog::EvaluateFitFor passes every one of them.
        //   * believing ApplyArmorAddon's return value (OS-84). Those same
        //     "rejections" mostly RENDER; the bool goes false whenever a piece's
        //     armatures cover fewer slots than its ARMO declares.
        //
        // So neither our fit model nor the engine's own bool answers "what is on
        // this character". Only the pass can answer that, by reading the biped
        // 15500 just wrote (StagedCoverageOf), and the shim runs in a different
        // hook with no biped to read. It has to be TOLD. That is what this is.
        //
        // Both hooks run on the game thread, pass first, in the same rebuild
        // (observed on every entry in that log, same millisecond). The layout
        // below does NOT lean on that observation, and deliberately so - it
        // held one uint32 when review accepted "stale but self-consistent";
        // with TWO correlated numbers a torn read is no longer harmless:
        // old styled + new hidden composes (real & ~newHidden) | oldStyled,
        // which can claim a head slot whose geometry is gone - the exact
        // OS-70 failure this table exists to prevent. So the pair lives in
        // ONE atomic (a single load is always mutually consistent), and the
        // reader re-checks the owner AFTER loading it, so a same-hash
        // neighbour publishing in between reads as a MISS, never as this
        // actor's numbers. A miss falls back to the vanilla mask, erring
        // toward a VISIBLE head: a head culled over an empty slot is a
        // broken character, a head visible under a helmet is at worst
        // clipping.
        struct RenderedCoverage {
            std::atomic<std::uint32_t> actorID{ 0 };  // 0 = empty entry
            std::atomic<std::uint64_t> packed{ 0 };   // hidden << 32 | styled
        };
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        constexpr std::size_t kAppliedSlots = 8;  // a crowded cell styles a handful of actors
        std::array<RenderedCoverage, kAppliedSlots> g_applied{};

        void PublishRenderedCoverage(RE::Actor* a_actor, std::uint32_t a_styled,
                                     std::uint32_t a_hidden) noexcept {
            if (!a_actor) {
                return;
            }
            const auto id   = a_actor->GetFormID();
            auto&      slot = g_applied[id % kAppliedSlots];
            slot.actorID.store(0, std::memory_order_relaxed);  // invalidate while writing
            slot.packed.store((static_cast<std::uint64_t>(a_hidden) << 32) | a_styled,
                              std::memory_order_relaxed);
            slot.actorID.store(id, std::memory_order_release);
        }

        struct CoveragePair {
            std::uint32_t styled{ 0 };
            std::uint32_t hidden{ 0 };
        };

        std::optional<CoveragePair> RenderedCoverageFor(RE::Actor* a_actor) noexcept {
            if (!a_actor) {
                return std::nullopt;
            }
            const auto  id   = a_actor->GetFormID();
            const auto& slot = g_applied[id % kAppliedSlots];
            if (slot.actorID.load(std::memory_order_acquire) != id) {
                return std::nullopt;
            }
            const auto packed = slot.packed.load(std::memory_order_relaxed);
            // Owner re-check AFTER the payload load. If a same-hash neighbour
            // (or a concurrent republish) got in between, the id no longer
            // matches (publishes pass through 0 first) and this is a miss.
            // A republish by the SAME actor that completes entirely inside
            // the window reads as that actor's newer pair - still a pair,
            // never a mixture, because the payload is one load.
            std::atomic_thread_fence(std::memory_order_acquire);
            if (slot.actorID.load(std::memory_order_relaxed) != id) {
                return std::nullopt;
            }
            return CoveragePair{ static_cast<std::uint32_t>(packed),
                                 static_cast<std::uint32_t>(packed >> 32) };
        }

        // The slots a style's geometry ACTUALLY landed on. Thin engine adapter
        // over StagedCoverageOf (SlotMask.h), which carries the full why.
        //
        // MUST be called immediately after ApplyArmorAddon, on the same call
        // stack: 15500 writes objects[].addon inline before that call returns,
        // which is the very synchronicity the honesty restore below already
        // depends on. a_biped must be the biped the AWM staged into.
        std::uint32_t StagedCoverageFor(RE::BipedAnim*     a_biped,
                                        RE::TESObjectARMO* a_armo) noexcept {
            if (!a_biped || !a_armo) {
                return 0;
            }
            std::array<RE::TESObjectARMA*, Outfit::kBitCount> staged{};
            for (std::uint32_t b = 0; b < Outfit::kBitCount; ++b) {
                staged[b] = a_biped->objects[b].addon;
            }
            return StagedCoverageOf<RE::TESObjectARMA*>(
                staged.data(), Outfit::kBitCount, a_armo->armorAddons.data(),
                static_cast<std::uint32_t>(a_armo->armorAddons.size()));
        }

        // ---- Helmet Toggle 2 interop (mirrors Apparel Preview's guard) ----
        // HT2 hides worn headgear through a Dynamic Armor Variants slot swap
        // (replaceBySlot 30/31/42/44 -> invisible ARMA) inside the SAME worn
        // rebuild we style in. Our post-pass injection is last-wins, so a
        // head-slot style would DEFEAT the hide (helmet stays visible although
        // Helmet Toggle says hidden) - and when DAV's swap wins instead it
        // swallows our geometry (bald head, AP field-proven). DAV has no
        // query/suspend API and Papyrus does not run in paused menus, so the
        // honest move is to follow HT2's own state global: while it hides
        // headgear, skip styles that overlap the hidden WORN head slots (re-
        // evaluated every pass, so the toggle stays live) and drop those bits
        // from the mask shim so hair regrows. 0 = shown, 1 = hidden.
        constexpr auto          kHT2Plugin    = "Helmet Toggle 2.esp";
        constexpr RE::FormID    kHT2StateID   = 0x804;  // GLOB 'HT_HelmetState'
        constexpr std::uint32_t kHT2HeadSlots =
            (1u << 0) | (1u << 1) | (1u << 12) | (1u << 14);  // biped 30/31/42/44

        RE::TESGlobal* HT2StateGlobal() noexcept {
            // Resolved once on first use; both thunks run on the game thread.
            static RE::TESGlobal* global = []() -> RE::TESGlobal* {
                auto* dh = RE::TESDataHandler::GetSingleton();
                auto* g  = dh ? dh->LookupForm<RE::TESGlobal>(kHT2StateID, kHT2Plugin)
                              : nullptr;
                if (g) {
                    spdlog::info("Helmet Toggle 2 detected: head-slot styles will follow "
                                 "its hide state (HT_HelmetState).");
                }
                return g;
            }();
            return global;
        }

        // ---- Apparel Preview of headgear suppresses OUR headgear -------------
        //
        // ⚠ THIS IS A PRESENTATION CHOICE, NOT A CORRECTNESS FIX, and it is the
        // one place in this file where that is true. OS-141, OS-145 and OS-151
        // each fixed a claim that had become FALSE. Nothing is false here: a
        // style on slot 30 and a previewed piece on slot 31 are different slots,
        // both geometries really are on the biped, and vanilla renders both. If
        // you equipped that piece for real you would get exactly this.
        //
        // It is still the wrong answer to the question a preview asks. Hovering
        // a helmet means "what does THIS look like on me", and answering with
        // it intersecting the helmet you are already transmogged into answers
        // nothing. So for the length of the hover, ours steps aside.
        //
        // ⚠ HEADGEAR ONLY, AND THE GROUPING IS THE ONE OS-148 ALREADY EARNED.
        // Slots 30, 31, 42 and 43 are mutually exclusive in practice, which is
        // precisely why a helmet ARMO declares all four at once. Nothing else
        // groups like that: a cuirass and a cloak genuinely layer, so previewing
        // one must not strip the other. Do not "generalise" this to every slot.
        //
        // ⚠ SUPPRESSES THE WHOLE GROUP, NOT THE PREVIEWED SLOT. Suppressing only
        // the slot the preview occupies is already what happens by itself, since
        // AP replaces the geometry there, and it is exactly what leaves a
        // slot-30 style sitting inside a slot-31 preview.
        std::uint32_t PreviewSuppressedHeadgear() noexcept {
            // Zero when no preview is live OR when this Apparel Preview is too
            // old to name its slots. Unknown must change nothing, which is the
            // same fail-safe direction the dye walk takes.
            return (ApparelPreviewKnownSlots() & kHeadgearSlotMask) ? kHeadgearSlotMask
                                                                    : 0u;
        }

        bool HT2HidesHeadgear() noexcept {
            auto* g = HT2StateGlobal();
            return g && g->value > 0.0f;
        }

        // ---- engine worn-pass visitor identity (Cause-B fix support) ----
        // 24231 builds its visitor on its own stack frame
        // (re_verify/disasm_24231_prologue.txt):
        //   +0x00 vtbl (REL::ID 241890)   +0x08 TESNPC*   +0x10 Actor*
        //   +0x18 NiPointer<BipedAnim> - a refcounted copy of *holder
        // and Visit (24433) forwards &(+0x18) as ApplyArmorAddon's holder, so the
        // biped at +0x18 is THE biped this pass stages into. It is also the only
        // chain-safe way to tell the 3rd-person pass from the 1st-person one:
        // 24231 runs once per biped and its C++ arguments are otherwise
        // identical. Read it ONLY behind the vtable check - a co-hooked mod
        // above us could hand down a wrapper with its own layout (exactly what
        // our Cause-A FilterVisitor once did to IED).
        std::uintptr_t g_wornVisitorVtbl = 0;  // resolved at install

        RE::BipedAnim* EnginePassBiped(RE::InventoryChanges::IItemChangeVisitor& a_visitor) noexcept {
            const auto base = reinterpret_cast<std::uintptr_t>(&a_visitor);
            if (!g_wornVisitorVtbl || *reinterpret_cast<std::uintptr_t*>(base) != g_wornVisitorVtbl) {
                return nullptr;  // foreign/wrapped visitor - cannot identify the pass
            }
            return *reinterpret_cast<RE::BipedAnim**>(base + 0x18);
        }

        // Whether the look on this actor right now is a PREVIEW: the editor is
        // open and this is the actor it is staging, the player or the follower
        // under edit alike. Read by the style gate so the worn rule
        // (bRequireWornForStyles) can stand down for a picture the player asked
        // to see, the way ShieldStyleMayConjure's second term already does for
        // the shield (field 2026-09-02: in lore friendly a preset click drew
        // nothing on a character wearing only skin, every piece refused for
        // having no real gear under it, while the page said "Trying on").
        //
        // The close edge needs no hook of its own. SetOpen flips IsOpen() to
        // false BEFORE EditorUI::OnClose runs, and OnClose discards the
        // staging, which kicks RequestRefresh / RequestRefreshActor for exactly
        // this actor; the rebuild that follows a close reads false here and the
        // rule is back, applied or not.
        //
        // StagingTarget takes the session lock, which this pass already takes
        // on the same stack through Display() and BareBodyHideMask(); nothing
        // here runs under it.
        bool PreviewingActor(const OutfitSession& a_session, RE::ActorHandle a_handle) {
            if (!EditorWindow::IsOpen()) {
                return false;
            }
            // Not const: this CommonLib's native_handle() is a non-const member.
            auto target = a_session.StagingTarget();
            return target.has_value() &&
                   target->native_handle() == a_handle.native_handle();
        }

        // The shared post-pass styling body (steps 4-7), factored out of
        // HandleWornPass so the player and NPC paths run IDENTICAL logic with
        // actor-derived inputs. EVERYTHING here is derived from a_actor and
        // a_holder, never a register and never the player singleton, so it is
        // correct for any actor whose biped the engine just rebuilt. The player
        // and NPC callers differ only in three inputs, all passed in: the
        // DisplaySet (NPC: worn-required masked), the style source, and the HT2
        // suppression mask (player-only; NPC passes 0).
        //
        // a_holder MUST be the actor's own 3rd-person holder (actor->GetBiped1(
        // false)) so the AWM cast keeps pointing at the actor's smart-pointer
        // field; a_biped is a_holder.get(), which the caller has already
        // null-checked and pass-gated.
        //
        // a_emitStyles(apply) is invoked once and must call apply(bit, ARMO*) for
        // each style to inject: the player hands session.VisitStyles (its own
        // allowed mask, unconditional); the NPC hands the snapshot's resolved
        // styles filtered to the worn-required styleMask. Templated so the NPC
        // emitter injects with no std::function allocation on the hot path (the
        // player path still constructs one inside VisitStyles, exactly as HEAD).
        template <class EmitStyles>
        void ApplyStyledPass(RE::Actor*                                a_actor,
                             const RE::BSTSmartPointer<RE::BipedAnim>& a_holder,
                             const DisplaySet&                         a_display,
                             const RealWorn&                           a_real,
                             EmitStyles&&                              a_emitStyles,
                             std::uint32_t                             a_suppressedSlots,
                             RE::ActorHandle                           a_handle) {
            RE::BipedAnim* biped = a_holder.get();

            auto* base = a_actor->GetActorBase();
            auto* race = a_actor->GetRace();
            if (!race) {
                // ApplyArmorAddon needs the race to pick armatures. Effectively
                // never null for the player (HEAD relied on that); un-gating
                // widens the domain to arbitrary NPCs, so guard it - a raceless
                // actor just keeps the vanilla pass's result, unstyled.
                return;
            }
            // ⚠ THE PER-PASS `masks:` DUMP LIVED HERE and is gone as of
            // 2026-08-04, the hide behaviour having held for a session. It
            // printed styleMask/hideMask/hiddenBodySkinMask/hiddenAttachmentMask
            // on EVERY pass, which is 543 lines in a normal play session, and it
            // did its job: paired with the cull path's own logging it settled
            // the "hiding a helmet does nothing" report, because the cull only
            // logs when it FLIPS a node's flag, so a hide that never reaches it
            // leaves no trace and an empty log cannot be told from a mask that
            // was never set. Put it back verbatim if that ambiguity returns.
            const bool  isFemale  = base && base->IsFemale();
            auto* const nakedSkin = REAug::GetActorSkin(a_actor);
            auto* const awm       = reinterpret_cast<REAug::ActorWeightModel*>(
                const_cast<RE::BSTSmartPointer<RE::BipedAnim>*>(&a_holder));

            // (4) HIDE, body-class slots (32/33/37): those meshes CONTAIN the
            //     body, so the only clean hide is re-staging skin over them
            //     (hide-mechanism-final.md §Fallbacks #1). Re-apply the actor
            //     skin (npc->skin override, else race->skin - exactly 15499's
            //     own choice, disasm_24221_verify.txt +0x15E..+0x176); its
            //     ARMAs last-wins over every skin-covered slot, so afterwards
            //     re-apply each real worn ARMO that is NOT hide-intersected
            //     to bring non-hidden body-class gear back. Item-granularity
            //     caveat: a single ARMO spanning a hidden and a non-hidden
            //     slot stays fully skinned (same limit the visitor-skip had).
            if (a_display.hiddenBodySkinMask) {
                if (nakedSkin) {
                    const bool ok = REAug::ApplyArmorAddon(nakedSkin, race, awm, isFemale);
                    spdlog::debug("  hide: skin reapply '{}' -> {}", nakedSkin->GetName(), ok);

                    std::uint32_t skinCoverage = 0;
                    for (auto* arma : nakedSkin->armorAddons) {
                        if (arma) {
                            skinCoverage |= arma->GetSlotMask().underlying();
                        }
                    }
                    for (std::uint32_t bit = 0; bit < 32; ++bit) {
                        auto* armo = a_real.armo[bit];
                        if (!armo || armo == nakedSkin) {
                            continue;
                        }
                        bool seen = false;  // dedupe: one re-apply per ARMO
                        for (std::uint32_t j = 0; j < bit && !seen; ++j) {
                            seen = a_real.armo[j] == armo;
                        }
                        const auto mask = armo->GetSlotMask().underlying();
                        if (seen || (mask & a_display.hideMask) != 0 || (mask & skinCoverage) == 0) {
                            continue;
                        }
                        const bool back = REAug::ApplyArmorAddon(armo, race, awm, isFemale);
                        spdlog::debug("  hide: re-apply kept gear '{}' -> {}", armo->GetName(), back);
                    }
                } else {
                    spdlog::warn("hide: no actor skin resolvable; body-class hide skipped.");
                }
            }

            // (5) STYLE: last-wins staging replaces the worn piece's 3D (and any
            //     skin re-applied in (4)). ApplyArmorAddon writes objects[]
            //     synchronously via 15500. Track the styles' FULL slot coverage:
            //     a multi-slot style ARMO stages .item into every covered slot,
            //     and each one needs the honesty restore. a_suppressedSlots is 0
            //     for NPCs (Helmet Toggle 2's GLOB is player-only, §3), so the
            //     suppression check short-circuits away entirely for them.
            std::uint32_t styledCoverage = 0;
            // Whichever style MEASURED onto slot 30 this pass, for the head
            // displacement rule below. Last one wins, which matches the
            // staging: 15500 is last-wins per slot, so the final writer of
            // objects[0] is the piece actually on the head.
            RE::TESObjectARMO* headStyleArmo = nullptr;
            // Hoisted out of the per-slot lambda: one settings read per biped
            // pass rather than one per biped object.
            const bool requireWorn = Settings::GetSingleton().requireWornForStyles;
            // And one lock per pass for the preview question, asked only when
            // the rule it relaxes is on. See PreviewingActor.
            const bool previewing =
                requireWorn && PreviewingActor(OutfitSession::GetSingleton(), a_handle);
            auto          applyStyle = [&](std::uint32_t a_bit, RE::TESObjectARMO* a_armo) {
                // ⚠⚠ THE DISPLAYSET'S styleMask IS HONOURED HERE, AT THE
                // CONSUMER, and it was not before. The player emitter is
                // session.VisitStyles, which recomputes the allowed mask from
                // EffectiveLocked on its own; the follower emitter filters on
                // display.styleMask before it ever calls this. So a DisplaySet
                // amended AFTER ComputeDisplaySet - the bare view clearing
                // styleMask - reached the follower and never the player. Field
                // log 2026-08-16 18:08:21.203: "bare: ... (styles off)" and,
                // the same millisecond, five "style bit N inject" lines. Two
                // readers of one answer, drifting in the gap; one gate on the
                // consumer closes it for every emitter at once.
                if (((a_display.styleMask >> a_bit) & 1u) == 0) {
                    return;  // the DisplaySet says no; whoever emitted this is behind it
                }
                // The gate asks about the style's whole coverage, not the slot
                // it is anchored to, because that is what gets staged below.
                // Declared (not measured) coverage on purpose: the staging does
                // not exist yet to be measured, and a superset never refuses a
                // style wrongly. Same reasoning as the 1P pass.
                // ⚠ THE SHIELD TERM IS MEASURED OFF THIS BIPED, EVERY CALL.
                // Object 9 is shared with the off-hand weapon, and an off-hand
                // WEAPON is not in the actor's armour worn-mask at all, so the
                // mask cannot answer "is it free" and only objects[9] can. It
                // is read here rather than hoisted because the staging above
                // can fill it: a style that already landed on 9 this pass must
                // not let a second one conjure over it.
                const bool shieldConjurable = ShieldStyleMayConjure(
                    biped && biped->objects[kBitShield].item == nullptr,
                    WeaponPreview::ShieldRowActive());
                if (!CanApplyStyleBit(a_bit, a_real.coverage, StyleCoverageOf(a_armo),
                                      requireWorn, shieldConjurable, previewing)) {
                    return;  // shield over an occupied object 9; any covered slot
                             // when bRequireWornForStyles is on and nobody is
                             // previewing this actor
                }
                if (a_suppressedSlots &&
                    (a_armo->GetSlotMask().underlying() & a_suppressedSlots)) {
                    spdlog::debug("  style bit {} '{}' skipped: something else owns this slot "
                                  "right now (Helmet Toggle, or an Apparel Preview "
                                  "of headgear).",
                                  a_bit, a_armo->GetName());
                    return;
                }
                const bool ok = REAug::ApplyArmorAddon(a_armo, race, awm, isFemale);
                // ⚠ COUNT WHAT IS ON THE BIPED, NOT WHAT THE CALL RETURNED.
                // OS-84, settled in the field 2026-07-30: ApplyArmorAddon's bool
                // is NOT a render signal. It returned false for 'Steel Spell
                // Knight Helmet' - ARMO declaring 30/31/42/43, race-valid
                // armature declaring 30 with both sex meshes - while the helmet
                // rendered on screen, and it does that for any piece whose
                // armatures cover fewer slots than its ARMO advertises, which is
                // 59% of head-slot injections in that log.
                //
                // Gating on the bool therefore claimed NO coverage for pieces
                // that were visibly worn: the head bit stayed clear and the head
                // drew inside the helmet (OS-86), and the hair bit stayed clear
                // so hair was never hidden (OS-87).
                //
                // The measurement below still refuses the case the bool-gate was
                // added for (OS-70b, 'Boiled Netch Leather Helmet'): a style that
                // staged nothing owns no slot, so nothing claims a head slot with
                // an empty slot underneath. One rule now covers both directions.
                const auto staged = StagedCoverageFor(biped, a_armo);
                styledCoverage |= staged;
                if (staged & MaskForEditorSlot(30)) {
                    headStyleArmo = a_armo;
                }
                if (!staged) {
                    // Not on the biped at all, so it covers nothing - and the
                    // user picked this style and it is not going to appear, so
                    // say so. This is the OS-70b direction.
                    spdlog::debug("  style bit {} '{}' renders nothing: staged no geometry.",
                                  a_bit, a_armo->GetName());
                } else if (!ok) {
                    // The ordinary ARMO/ARMA slot mismatch. Kept at debug and
                    // deliberately NOT a warning: it fired 568 times in one
                    // session before it was understood.
                    spdlog::debug("  style bit {} '{}' staged 0x{:X} although ApplyArmorAddon "
                                  "returned false (declared 0x{:X}).",
                                  a_bit, a_armo->GetName(), staged,
                                  a_armo->GetSlotMask().underlying());
                }
                spdlog::debug("  style bit {} inject '{}' -> {} staged=0x{:X}", a_bit,
                              a_armo->GetName(), ok, staged);
            };
            a_emitStyles(applyStyle);

            // (5b) HEAD DISPLACEMENT. A style that MEASURED onto slot 30 takes
            //      the real piece on slot 31 with it, because two head pieces
            //      on one head is the "both render" report (field 2026-08-11).
            //
            // ⚠ THE RULE ITSELF IS HeadDisplacementCull IN SlotMask.h AND ALL
            // OF IT IS GUARDS. Read its header before touching this block:
            // three independent reviews refuted the unguarded version, twice
            // with a path to a WORSE defect than the one being fixed. Nothing
            // here may be inlined into a condition - the rule lives in the pure
            // header because nothing compiles this file, which is also why the
            // decline below is logged rather than silent.
            //
            // ⚠ AND WHY THE MASK TERM COMES WITH THE CULL, NOT AFTER IT. If the
            // 31-piece's geometry goes but its bit stays in the published mask,
            // 24220 culls the hair head-part for a hood that is no longer there
            // and the character goes bald. Folding headDisplaced into lostMask
            // clears the bit, and over-claiming `hidden` errs toward VISIBLE,
            // which is this file's stated fail-safe direction.
            std::uint32_t headDisplaced = 0;
            if (headStyleArmo) {
                auto* const wornHair = a_real.armo[kBitHair];
                if (!wornHair) {
                    spdlog::debug("  head displace: style '{}' on slot 30, nothing worn on "
                                  "slot 31; nothing to displace.",
                                  headStyleArmo->GetName());
                } else {
                    const auto wornStaged   = StagedCoverageFor(biped, wornHair);
                    const auto wornDeclared = StyleCoverageOf(wornHair);
                    const bool shared =
                        SharesAnyArmature<RE::TESObjectARMA*>(headStyleArmo, wornHair);
                    headDisplaced = HeadDisplacementCull(styledCoverage, wornStaged,
                                                         wornDeclared, shared);
                    if (headDisplaced) {
                        spdlog::debug("  head displace: style '{}' on slot 30 displaces worn "
                                      "'{}' -> cull 0x{:X} (staged 0x{:X}).",
                                      headStyleArmo->GetName(), wornHair->GetName(),
                                      headDisplaced, wornStaged);
                    } else if (shared) {
                        // The enchanted-variant collision. Declining is correct
                        // and the pieces look identical anyway, so the visible
                        // result is right even though the rule stood down.
                        spdlog::debug("  head displace: DECLINED, style '{}' and worn '{}' "
                                      "share an armature; the measurement cannot tell them "
                                      "apart, so culling would take the style's own clone.",
                                      headStyleArmo->GetName(), wornHair->GetName());
                    } else if ((wornDeclared & ~kHeadgearSlotMask) != 0) {
                        spdlog::debug("  head displace: DECLINED, worn '{}' declares 0x{:X} "
                                      "which reaches outside the headgear group; it is a "
                                      "robe or similar, not a helmet.",
                                      wornHair->GetName(), wornDeclared);
                    } else {
                        spdlog::debug("  head displace: DECLINED, worn '{}' holds no "
                                      "headgear geometry the style does not already own "
                                      "(staged 0x{:X}, styled 0x{:X}).",
                                      wornHair->GetName(), wornStaged, styledCoverage);
                    }
                }
            }

            // The groin, and it is the same shape of problem as the head above:
            // a style landing where the ENGINE still reads bare skin. TNG and
            // SOS choose their slot-52 mesh from the armour actually equipped
            // on 32, so a transmogged cuirass is invisible to them and the bare
            // piece comes through it. The rule and every guard on it live in
            // GenitalDisplacementCull; see there for why real worn body gear
            // declines and why a revealing style is a known exception.
            //
            // ⚠ THE OBJECT, NOT THE WORN ARMO. See the rule's own header: TNG
            // reaches slot 52 through the SKIN, which is never worn inventory.
            const bool genitalObject =
                biped && biped->objects[kBitGenitals].item != nullptr;
            const std::uint32_t genitalDisplaced = GenitalDisplacementCull(
                styledCoverage, a_real.armo[kBitBody] != nullptr, genitalObject);
            if (genitalDisplaced) {
                spdlog::debug("  groin displace: body style over a bare body slot, "
                              "culling slot 52 (styled 0x{:X}).",
                              styledCoverage);
            } else if ((styledCoverage & MaskForEditorSlot(32)) != 0) {
                // ⚠ NAME THE GUARD THAT REFUSED. A rule that declines silently
                // and a rule that never ran read identically in a log, and the
                // first round of this fix was lost to exactly that: zero lines
                // said nothing about which of three conditions said no.
                spdlog::debug("  groin displace: DECLINED, bodyWorn={} genitalObject={} "
                              "(styled 0x{:X}).",
                              a_real.armo[kBitBody] != nullptr, genitalObject,
                              styledCoverage);
            }

            // Hand the shim the truth. It runs next, in a different hook, and
            // has no other way to learn which of these styles the engine took.
            //
            // H3: a hide lands on a whole worn piece, not one of its slots. The
            // per-slot coverage table is built from the REAL worn gear so
            // ExpandHideOverCoverage can find the piece occupying each hidden
            // slot regardless of which of its slots the user clicked.
            //
            // A STYLE displaces worn gear too, but only its HEAD-slot landings
            // count here, and the scoping is evidence, not caution: the Krosis
            // field report proves a style taking one head slot of a worn
            // 30+31 mask leaves the head bare (the piece's geometry is gone,
            // so its head bit must fall out of the mask or the engine culls a
            // head nothing covers) - while OS-83's gauntlet report implies the
            // OPPOSITE for body gear, residue left attached. Until 15500
            // settles which is general, the style-displacement claim stays on
            // the two bits the consumer reads and the field has proven.
            // Dropping this term regresses the original Krosis repro; it is
            // exactly the style half of the old DisplacedRealCoverage.
            // Nothing hidden and no head slot styled -> the expansion is
            // Expand(0) == 0 by definition, so skip building the 32-entry
            // coverage table. This file's own doctrine: a styled market
            // square pays this site per actor per rebuild, and the common
            // outfit touches neither.
            const auto    lostMask =
                a_display.hideMask | (styledCoverage & kHeadPartMask) | headDisplaced;
            std::uint32_t hiddenCoverage = 0;
            if (lostMask) {
                std::array<std::uint32_t, Outfit::kBitCount> wornCoverage{};
                for (std::uint32_t b = 0; b < Outfit::kBitCount; ++b) {
                    wornCoverage[b] = StyleCoverageOf(a_real.armo[b]);
                }
                hiddenCoverage = ExpandHideOverCoverage(lostMask, wornCoverage);
            }
            PublishRenderedCoverage(a_actor, styledCoverage, hiddenCoverage);

            // (6) Gameplay honesty. objects[] was written inline by 15500 on this
            //     very call stack, so restoring here has a zero race window.
            //     Armor-skill XP (37673->37589->37688) and equip-conflict
            //     (36979->14026) read objects[i].item - hidden body slots now
            //     hold the skin ARMO after (4), so this also keeps hidden-but-
            //     worn gear honest for XP.
            // Use only APPLIED style coverage here, never the raw requested
            // style mask. A shield request is actor-gated above; when an
            // off-hand weapon occupies shared biped object 9, including a
            // rejected shield bit would replace that WEAP item with skin ARMO
            // and crash Skyrim's following UpdateEquipment pass.
            BipedPost::RestoreRealItems(
                biped, PostPassArmorRestoreMask(a_display.hideMask, styledCoverage),
                a_real.armo, nakedSkin);

            // (7) HIDE, attachment slots (helmet/amulet/ring/cloak/…): their
            //     meshes are self-contained, so cull the staged 3D
            //     (NiAVObject::kHidden on objects[slot].partClone). Sync first
            //     for models 15500 already cloned on this stack; the deferred
            //     sweep (scoped to THIS actor's handle) catches clones the
            //     BSTaskPool attaches late. Hair/head-part regrowth stays the
            //     mask shim's job.
            // ⚠ SHOW BEFORE CULL, EVERY PASS, NOT ONLY WHEN SOMETHING IS HIDDEN.
            // Un-hiding a slot cleared the mask but left NiAVObject::kHidden set
            // on the clone, so the piece never came back (field 2026-08-04:
            // hide a helmet, unhide it, it stays gone while the gear is
            // transmogged). BipedPost's own header already records the cause for
            // the Presets preview: "Skyrim may reuse the same partClone across a
            // refresh, so leaving Presets must clear kHidden explicitly rather
            // than assume a rebuild replaces the clone." The preview got a
            // symmetric restore; this path never did. Transmog is what makes it
            // visible, because a styled slot is precisely where the clone
            // survives the refresh.
            //
            // ⚠ SCOPED TO WHAT THIS SYSTEM CAN CULL. Clearing kHidden across all
            // 32 armour objects would fight two other owners: the body-class
            // slots are hidden by re-staging skin rather than by a flag, and the
            // shield's object is culled by the Presets preview, whose suppressed
            // set is (1 << kBitShield) | weapons. kNeverHideMask is exactly the
            // shield, so subtracting both leaves only the slots a hide here can
            // ever have flagged.
            constexpr std::uint32_t kCullableAttachments =
                ~(kBodySkinMask | kNeverHideMask);
            // headDisplaced joins the explicit hides here and NOT in the
            // DisplaySet, because it is measured off this pass's biped and so
            // cannot be known when the DisplaySet is computed. It is bounded to
            // kHeadgearSlotMask by its own guard, and all four of those bits
            // are inside kCullableAttachments - so unlike a body-class bit it
            // is always restorable by the show sweep above once the style goes.
            // ⚠ genitalDisplaced rides here for headDisplaced's exact reason:
            // it is measured off THIS pass's worn state, so the DisplaySet
            // cannot know it. Bit 22 is inside kCullableAttachments (the body
            // skin mask is 32/33/37 and kNeverHideMask is the shield), so the
            // show sweep above restores it by itself the moment the style goes.
            const std::uint32_t cullMask =
                a_display.hiddenAttachmentMask | headDisplaced | genitalDisplaced;
            const std::uint32_t showMask = kCullableAttachments & ~cullMask;
            BipedPost::ShowObjectNodes(biped, showMask);
            BipedPost::QueueObjectNodeShow(a_handle, showMask);
            if (cullMask) {
                BipedPost::CullNodes(biped, cullMask);
                BipedPost::QueueNodeCull(a_handle, cullMask);
            }

            // (8) HIDE, the SKIN half, for the headgear group. Culling the
            //     helmet's node does not undo the skin partition its armature
            //     suppressed, so the ears stay gone and the player sees the gap
            //     the helmet used to cover. See RestoreDismemberPartitions.
            //
            // ⚠ FROM THE HIDE ALONE, NEVER FROM hiddenCoverage. That mask also
            // carries the head bits a STYLE displaced, and a style covering the
            // ears has its own geometry there: putting the skin back under it
            // would push the ears through whatever the player chose to wear.
            // Expanded separately for the same reason ExpandHideOverCoverage
            // exists at all - a hide lands on a whole worn piece - and skipped
            // outright when no headgear is hidden, which is the common outfit.
            if ((a_display.hideMask & kHeadgearSlotMask) != 0) {
                std::array<std::uint32_t, Outfit::kBitCount> wornCoverage{};
                for (std::uint32_t b = 0; b < Outfit::kBitCount; ++b) {
                    wornCoverage[b] = StyleCoverageOf(a_real.armo[b]);
                }
                const auto hiddenPieces =
                    ExpandHideOverCoverage(a_display.hideMask, wornCoverage);
                const auto restore = hiddenPieces & kHeadgearSlotMask & ~styledCoverage;
                BipedPost::RestoreDismemberPartitions(a_actor, restore);
                BipedPost::QueueRestoreDismemberPartitions(a_handle, restore);
            }
        }

        // ---- The 1st-person worn pass ----
        // The 1P arms model is built from GetBiped1(true), which the 3P-only
        // injection originally never touched. During the pass that stages the
        // 1P biped, apply body-skin hides and styles whose coverage includes a
        // slot the 1P model renders (body 32 / hands 33 / forearms 34 - the
        // visible arms belong to the BODY piece's armor addon, plus shield 39).
        // Head and feet have no 1P geometry. Attachment culling remains a 3P
        // concern; hands/body hide by re-staging the actor's naked 1P skin.
        void StyleFirstPersonPass(RE::BipedAnim* a_passBiped, RE::Actor* a_player,
                                  const RealWorn& a_real) {
            const auto&    holder1p = a_player->GetBiped1(true);
            RE::BipedAnim* biped1p  = holder1p.get();
            if (!biped1p || a_passBiped != biped1p) {
                spdlog::debug("wornpass: pass biped matches neither holder; skipped.");
                return;
            }

            auto*       base      = a_player->GetActorBase();
            auto*       race      = a_player->GetRace();
            const bool  isFemale  = base && base->IsFemale();
            auto* const nakedSkin = REAug::GetActorSkin(a_player);
            auto* const awm       = reinterpret_cast<REAug::ActorWeightModel*>(
                const_cast<RE::BSTSmartPointer<RE::BipedAnim>*>(&holder1p));

            // Helmet Toggle 2 parity with the 3P pass: a piece suppressed there
            // (overlapping a hidden worn head slot) must not sneak in here, or
            // the two views would disagree about the same style.
            std::uint32_t suppressedSlots = 0;
            if (HT2HidesHeadgear()) {
                for (auto* wornArmo : a_real.armo) {
                    if (wornArmo) {
                        suppressedSlots |=
                            wornArmo->GetSlotMask().underlying() &
                            kHT2HeadSlots;
                    }
                }
            }
            // Player-only, like HT2's own global: Apparel Preview previews on
            // the player alone. See PreviewSuppressedHeadgear.
            suppressedSlots |= PreviewSuppressedHeadgear();

            // The 1P arms get the same answer as the 3P body. A player who
            // undressed to look at a body and then sheathed would otherwise
            // find their own hands still in gauntlets.
            auto&      session1p = OutfitSession::GetSingleton();
            const auto display   = BareBodyDisplay(
                session1p.Display(),
                session1p.BareBodyHideMask(a_player, a_real.coverage));
            const auto hiddenBodySkin =
                FirstPersonBodySkinHideMask(display.hiddenBodySkinMask);
            if (hiddenBodySkin) {
                if (nakedSkin) {
                    const bool ok =
                        REAug::ApplyArmorAddon(nakedSkin, race, awm, isFemale);
                    spdlog::debug(
                        "  1p hide: skin reapply '{}' -> {}",
                        nakedSkin->GetName(), ok);

                    std::uint32_t skinCoverage = 0;
                    for (auto* arma : nakedSkin->armorAddons) {
                        if (arma) {
                            skinCoverage |= arma->GetSlotMask().underlying();
                        }
                    }
                    for (std::uint32_t bit = 0; bit < 32; ++bit) {
                        auto* armo = a_real.armo[bit];
                        if (!armo || armo == nakedSkin) {
                            continue;
                        }
                        bool seen = false;
                        for (std::uint32_t j = 0; j < bit && !seen; ++j) {
                            seen = a_real.armo[j] == armo;
                        }
                        const auto mask =
                            armo->GetSlotMask().underlying();
                        if (seen || (mask & display.hideMask) != 0 ||
                            (mask & skinCoverage) == 0 ||
                            (mask & kFirstPersonArmorMask) == 0) {
                            continue;
                        }
                        const bool back =
                            REAug::ApplyArmorAddon(armo, race, awm, isFemale);
                        spdlog::debug(
                            "  1p hide: re-apply kept gear '{}' -> {}",
                            armo->GetName(), back);
                    }
                } else {
                    spdlog::warn(
                        "1p hide: no player skin resolvable; body/hand hide skipped.");
                }
            }

            std::uint32_t styled = 0;
            const bool    requireWorn1P = Settings::GetSingleton().requireWornForStyles;
            // The 1P arms get the 3P body's answer to the preview question too,
            // or the sleeves of a previewed robe would vanish in first person.
            const bool previewing1P =
                requireWorn1P && PreviewingActor(session1p, a_player->GetHandle());
            OutfitSession::GetSingleton().VisitStyles(
                [&](std::uint32_t a_bit, RE::TESObjectARMO* a_armo) {
                    // Same consumer-side gate as ApplyStyledPass's applyStyle,
                    // and for the same reason: VisitStyles emits from its own
                    // mask, and the DisplaySet this pass was handed can be
                    // narrower than that. The 1P arms must agree with the 3P
                    // body about whether a style is on.
                    if (((display.styleMask >> a_bit) & 1u) == 0) {
                        return;
                    }
                    const auto coverage = StyleCoverageOf(a_armo);
                    // Coverage, not the anchor slot: one ARMO can dress several
                    // slots and the engine stages it across all of them.
                    // Measured off the 1P biped for the 3P pass's reason: the
                    // first-person rig renders the shield too (it is in
                    // kFirstPersonArmorMask), and it has its own object 9.
                    const bool shieldConjurable1P = ShieldStyleMayConjure(
                        biped1p && biped1p->objects[kBitShield].item == nullptr,
                        WeaponPreview::ShieldRowActive());
                    if (!CanApplyStyleBit(a_bit, a_real.coverage, coverage,
                                          requireWorn1P, shieldConjurable1P, previewing1P)) {
                        return;  // shield over an occupied object 9; any covered
                                 // slot when bRequireWornForStyles is on and
                                 // nobody is previewing the player
                    }
                    if ((coverage & kFirstPersonArmorMask) == 0) {
                        return;  // nothing the 1P model renders
                    }
                    if (suppressedSlots && (coverage & suppressedSlots)) {
                        spdlog::debug("  1p style bit {} '{}' skipped: Helmet Toggle hides this slot.",
                                      a_bit, a_armo->GetName());
                        return;
                    }
                    const bool ok = REAug::ApplyArmorAddon(a_armo, race, awm, isFemale);
                    // Measured off the 1P biped, same rule as the 3P pass: the
                    // return value is not a render signal (OS-84). `coverage`
                    // above stays DECLARED because it gates work that has to
                    // happen before the staging exists to be measured, and it is
                    // a superset, so it never skips a style wrongly.
                    const auto staged = StagedCoverageFor(biped1p, a_armo);
                    styled |= staged;
                    spdlog::debug("  1p style bit {} inject '{}' -> {} staged=0x{:X}", a_bit,
                                  a_armo->GetName(), ok, staged);
                });

            // Same honesty restore as the 3P pass: every hidden or styled 1P
            // slot points .item back at the real gear (or the skin) while its
            // staged model remains visible.
            const auto touched =
                FirstPersonArmorRestoreMask(display.hideMask, styled);
            if (touched) {
                BipedPost::RestoreRealItems(
                    biped1p, touched, a_real.armo, nakedSkin);
            }
        }

        // Chain-safe by construction (the Cause-B fix,
        // docs/handoffs/2026-07-10-f10-ctd.md): a plain thunk that consumes ONLY
        // the displaced call's real C++ arguments (rcx = InventoryChanges*,
        // rdx = visitor&). With Apparel Preview hooked in at kPostLoad we are
        // entered from AP's compiled handler, not from the engine site, so every
        // other register holds AP's locals - the old xbyak stub captured rbx/rdi
        // there and injected through an AP vtable pointer into exe .rdata
        // (crash-…-20-46-41 / -21-23-53). Nothing here may read registers, ever;
        // the biped holder is derived from the actor instead.
        //
        // noexcept: called from an engine/co-hooked-mod frame, so a propagating
        // exception = UB. The body wraps every throwing call (mutex lock, spdlog,
        // std::function/vector alloc in VisitStyles); the noexcept is a hard
        // backstop that turns any impossible escape into a defined
        // std::terminate instead of UB. Invariant enforced below: the worn
        // skinning pass runs EXACTLY once, even on error, so the actor is never
        // left naked by a mid-pass throw.
        void HandleWornPass(RE::InventoryChanges* a_changes,
                            RE::InventoryChanges::IItemChangeVisitor& a_visitor) noexcept {
            auto&      session   = OutfitSession::GetSingleton();
            auto*      player    = RE::PlayerCharacter::GetSingleton();
            auto*      owner     = a_changes ? a_changes->owner : nullptr;
            const bool forPlayer = player && owner == player;

            if (forPlayer) {
                g_playerWornPass.fetch_add(1, std::memory_order_relaxed);
            }

            bool passRan = false;
            try {
                if (forPlayer) {
                    // ---- PLAYER PATH (behavior-identical to HEAD) -----------
                    // Only the shared styling body (steps 4-7) was factored into
                    // ApplyStyledPass; the gate, worn-pass, holder derivation,
                    // pass gate, and HT2 suppression below are byte-for-byte the
                    // original player flow.
                    if (!session.IsActive()) {
                        g_origVisitWorn(a_changes, a_visitor);  // vanilla, untouched
                        return;
                    }

                    const auto real = SnapshotRealWorn(a_changes);
                    // ⚠ AFTER SnapshotRealWorn AND NOT BEFORE. The bare view is
                    // bounded by what the actor MEASURABLY wears; see
                    // BareBodyDisplay for what a hide bit on an empty slot does
                    // to the head.
                    const auto display = BareBodyDisplay(
                        session.Display(),
                        session.BareBodyHideMask(player, real.coverage));

                    // (1) Worn pass - ALWAYS with the engine's own visitor, never
                    //     a proxy: co-hooked mods on the shared 24231 chain read
                    //     the visitor's engine layout (a proxy deterministically
                    //     crashed Immersive Equipment Displays - crash-2026-07-10-
                    //     {19-02,20-05}.log). Hide therefore acts strictly AFTER
                    //     the pass, handing nothing foreign down-chain.
                    g_origVisitWorn(a_changes, a_visitor);
                    passRan = true;  // from here a throw cannot leave the actor naked

                    // (2) The biped holder, derived register-free from the actor
                    //     (the Cause-B fix). GetBiped1 is virtual slot 0x7E and
                    //     returns the ADDRESS of the biped smart-pointer field -
                    //     PlayerCharacter+0x260 for firstPerson=false (verified:
                    //     re_verify/disasm_24231_prologue.txt, IDs 39399/39189).
                    //     ApplyArmorAddon and its callees only ever *read* the
                    //     holder, so pointing them at the actor's own field is
                    //     ABI-identical to the engine's &visitor.biped.
                    const auto&    holder = player->GetBiped1(false);
                    RE::BipedAnim* biped  = holder.get();
                    if (!biped) {
                        spdlog::debug("wornpass: player has no 3rd-person biped; nothing to style.");
                        return;
                    }

                    // (3) Pass gate: 24231 runs once per biped (3rd- and 1st-
                    //     person for the player). The 3rd-person pass gets the
                    //     full treatment below; the 1st-person pass gets
                    //     style-only injection (BUG-2 - styled hands showed the
                    //     real gauntlets in first person because GetBiped1(true)
                    //     was never staged). If the visitor is foreign we cannot
                    //     discriminate - run the 3P work anyway (idempotent, and
                    //     it always targets the actor-derived holder, never a
                    //     guess), matching the pre-BUG-2 behavior.
                    if (auto* passBiped = EnginePassBiped(a_visitor); passBiped && passBiped != biped) {
                        StyleFirstPersonPass(passBiped, player, real);
                        return;
                    }

                    // Helmet Toggle 2: while it hides worn headgear, suppress
                    // styles that overlap the hidden WORN head slots (intersection
                    // keeps unrelated headwear styled - a circlet style in 42
                    // survives a hidden hood covering 30/31). PLAYER-ONLY: the
                    // GLOB describes the player, not followers (spec §3); the NPC
                    // path passes 0 and worn-required covers followers.
                    std::uint32_t suppressedSlots = 0;
                    if (HT2HidesHeadgear()) {
                        for (auto* wornArmo : real.armo) {
                            if (wornArmo) {
                                suppressedSlots |=
                                    wornArmo->GetSlotMask().underlying() &
                                    kHT2HeadSlots;
                            }
                        }
                    }
                    // ⚠ THE 1P PASS DOES THE IDENTICAL THING AND THAT MATTERS.
                    // A style suppressed in one view and not the other means
                    // first and third person disagree about the same helmet,
                    // which is the parity the HT2 term above already exists to
                    // keep. See PreviewSuppressedHeadgear.
                    suppressedSlots |= PreviewSuppressedHeadgear();

                    // Style source: session.VisitStyles (the player's own allowed
                    // mask, unconditional - the player is styled everywhere the
                    // outfit says, not just where real gear is worn). The lambda
                    // is converted to a std::function inside VisitStyles, exactly
                    // as the original inline call did.
                    ApplyStyledPass(
                        player, holder, display, real,
                        [&](const std::function<void(std::uint32_t, RE::TESObjectARMO*)>& a_apply) {
                            session.VisitStyles(a_apply);
                        },
                        suppressedSlots, player->GetHandle());

                    // (8) DYE. This is the seam that makes a dye survive a
                    //     rebuild Fitting Room did not ask for. Until now
                    //     OutfitDye::Repaint had exactly one caller, at the tail
                    //     of REAug::RefreshActor, so a persisted dye reached the
                    //     screen only when our OWN refresh happened to run: a
                    //     save load, a re-equip, a cell change and a race-menu
                    //     exit each rebuild the biped through the engine and
                    //     left the armour undyed (B1, 2026-07-31 review). Every
                    //     one of those rebuilds passes through THIS pass, which
                    //     is why the arm belongs here and not in a fourth event
                    //     sink.
                    //
                    //     Queued rather than painted inline, for the same
                    //     measured reason step (7) queues its cull: UpdateEquipment
                    //     has only just written .addon and .part on this stack
                    //     and BSTaskPool attaches .partClone later, so a
                    //     synchronous walk here would find a null clone on every
                    //     slot whose model was not already cached (B2). The chain
                    //     re-arms while any dyed slot is still waiting.
                    //
                    //     Unconditional: Repaint also refreshes the shape
                    //     snapshot the editor labels its channels from, and the
                    //     moment the user needs those labels is exactly when the
                    //     slot has no dye yet. QueueRepaint resolves the outfit
                    //     itself and does nothing when there is none, so a
                    //     dye-free session costs one task and a lookup.
                    //
                    //     The player's arm. The follower's is the matching call
                    //     at the tail of the NPC branch below, on lk.actor's
                    //     handle and deliberately not inside ApplyStyledPass,
                    //     which would arm a chain per styled NPC per rebuild in
                    //     a crowded cell (OS-128).
                    OutfitDye::QueueRepaint(player->GetHandle());
                    return;
                }

                // ---- NPC PATH (un-gated Task 3) -----------------------------
                // Count/scene gate + snapshot lookup, shared with GetWornMaskThunk
                // and (Task 4) the weapon thunk via LookupAssignedNpc
                // (NpcLookup.h): no lock, snapshot load, or alloc when nothing is
                // assigned, and the whole NPC path stands down while a scene runs
                // (SceneGuard is NOT in the snapshot - it flips asynchronously and
                // is read lock-free there, mirroring EffectiveLocked's scene gate
                // on the player side).
                auto lk = LookupAssignedNpc(session, owner);
                if (!lk.entry) {
                    g_origVisitWorn(a_changes, a_visitor);  // gated / unassigned / not an actor
                    return;
                }
                const ResolvedNpcDisplay& entry = *lk.entry;

                // Real worn coverage for the §3 worn-required rule (ONE walk,
                // shared with the player path).
                const auto real = SnapshotRealWorn(a_changes);

                // (1) Worn pass - engine visitor, same chain contract as the
                //     player. From here a throw degrades to what the pass built.
                g_origVisitWorn(a_changes, a_visitor);
                passRan = true;

                // (2) NPC biped holder, virtual GetBiped1(false) (absorbs the AE
                //     +8 layout shift; never hardcode +0x260 - spec §preconditions).
                const auto&    holder = lk.actor->GetBiped1(false);
                RE::BipedAnim* biped  = holder.get();
                if (!biped) {
                    spdlog::debug("wornpass: NPC {:08X} has no biped this pass.", lk.baseFormID);
                    return;
                }

                // (3) Pass gate still applies; trivially true for NPCs (they have
                //     no 1st-person biped, so the sole pass is the 3rd-person one).
                if (auto* passBiped = EnginePassBiped(a_visitor); passBiped && passBiped != biped) {
                    spdlog::debug("wornpass: NPC non-3rd-person pass; skipped.");
                    return;
                }

                // NPC styles are visual and may fill an otherwise unworn slot,
                // matching the player path (a styled helmet does not require a
                // gameplay helmet). Hide remains worn-required. The derived
                // hide submasks stay consistent through WornRequiredDisplay.
                // Forms were pre-resolved - the hook never resolves one.
                // ⚠ THE BARE VIEW GOES ON AFTER THE WORN-REQUIRED MASKING, not
                // before it. WornRequiredDisplay rebuilds a fresh DisplaySet
                // field by field, so anything folded in first would be dropped
                // on the floor here and nowhere else - the follower half of the
                // bug that struct's own header warns about.
                const DisplaySet display = BareBodyDisplay(
                    NpcResolve::WornRequiredDisplay(entry.display, real.coverage),
                    session.BareBodyHideMask(lk.actor, real.coverage));

                // The NPC pass does NOT bump g_playerWornPass (the tripwire stays
                // player-scoped, spec §3). Style source: the snapshot's resolved
                // styles, filtered to the allowed styleMask; no HT2
                // suppression (mask 0 - player-only). Handle-scoped deferred cull
                // re-resolves THIS NPC, never the player.
                ApplyStyledPass(
                    lk.actor, holder, display, real,
                    [&](const auto& a_apply) {
                        for (const auto& styled : entry.styles) {
                            if (styled.armo && ((display.styleMask >> styled.bit) & 1u)) {
                                a_apply(styled.bit, styled.armo);
                            }
                        }
                    },
                    /*suppressedSlots*/ 0u, lk.actor->GetHandle());

                // OS-128. Her dye rides the same rebuild her styles do, exactly
                // as the player's does at the tail of his branch above.
                //
                // ⚠ HERE, NOT INSIDE ApplyStyledPass. Inside, a crowded cell
                // arms one chain per styled NPC per rebuild instead of one
                // chain per rebuild. This site was named as the follower arm
                // point before followers had dye at all.
                //
                // QueueRepaint takes only its own lock, writes a retry budget
                // and posts, so it never reaches lock_ from inside this
                // noexcept thunk. It resolves the outfit later on the task
                // drain and does nothing when there is none.
                OutfitDye::QueueRepaint(lk.actor->GetHandle());
            } catch (...) {
                // Never unwind into the engine. If we threw BEFORE the worn pass
                // ran, run the vanilla pass so the actor isn't left naked; a later
                // throw (style/restore) is simply swallowed - the biped keeps
                // whatever the filtered pass built.
                if (!passRan) {
                    try {
                        g_origVisitWorn(a_changes, a_visitor);
                    } catch (...) {
                    }
                }
                try {
                    spdlog::error("HandleWornPass threw; degraded to vanilla for this pass.");
                } catch (...) {
                }
            }
        }

        // ---- 24220+0x7C: InventoryChanges::GetWornMask ----
        // 24220 is the mask's ONLY consumer, and it reads exactly two bits of
        // it: RACE_DATA::headObject (slot 30) toggles kHidden on the FaceGen
        // HEAD node, RACE_DATA::hairObject (slot 31) on the hair head-part.
        // See kHeadPartMask in SlotMask.h for the verified disassembly.
        //
        // The raw mask answers "what is EQUIPPED". After our pass that is no
        // longer the same question as "what is RENDERED", and the engine is
        // asking the second one. So the shim rebuilds the mask from what we
        // actually put on the biped: real worn coverage, minus everything the
        // pass measured as hidden, plus the coverage of the styles that were
        // really applied. RenderedWornMask is that expression.
        //
        // The old shim was `(real | styleMask) & ~(hideMask & kHeadPartMask)`
        // with slot 30 absent from kHeadPartMask, so the head bit could never
        // be cleared: hide or restyle a dragon priest mask (slots 30+31) and
        // its geometry went away while the head stayed culled. That is the
        // Krosis/Ahzidal "invisible head" report and OS-70.
        using GetWornMask_t = std::uint32_t (*)(RE::InventoryChanges*);
        GetWornMask_t g_origGetWornMask = nullptr;

        // The rendered mask for one actor. Everything the engine's head/hair
        // culling needs was measured by the styling pass and published; nothing
        // is re-derived here. Predicting the engine's accept/reject was proven
        // wrong in OS-70c, and inferring which worn piece a style displaced was
        // the DisplacedRealCoverage heuristic this replaces. A miss (no pass
        // yet, or another actor evicted the entry) leaves the vanilla mask,
        // which errs toward a VISIBLE head: a head culled over an empty slot is
        // a broken character, a head visible under a helmet is at worst
        // clipping.
        // The head-part bits whose biped slot is occupied yet staged NOTHING.
        // .item with .addon and .part both empty is the ghost's synchronous
        // mark (15500 writes all three on this stack, DyeGate.h); partClone is
        // deferred and deliberately NOT consulted - a legit helmet mid-attach
        // keeps its bit. See DropGhostHeadPartBits in SlotMask.h for why this
        // exists and which war it ends.
        std::uint32_t GhostHeadPartMask(RE::Actor* a_actor) noexcept {
            // ⚠ GetBiped1(false), not GetCurrentBiped(): in first person the latter is
            // the first-person biped, which stages no head geometry, so every helmet
            // and hood on it would read as a ghost here (BipedPost.cpp's sweep, the
            // 2026-09-14 reading). The engine's two hide bits act on the third-person
            // head, so the third-person biped is the one that answers.
            auto* const biped = a_actor ? a_actor->GetBiped1(false).get() : nullptr;
            if (!biped) {
                return 0;
            }
            std::uint32_t ghost = 0;
            for (const auto bit : { kBitHead, kBitHair }) {
                const auto& obj = biped->objects[bit];
                if (obj.item && !obj.addon && !obj.part) {
                    ghost |= 1u << bit;
                }
            }
            // ⚠⚠ AND WHAT THE SWEEP MEASURED, WHICH IS THE HALF THE TEST ABOVE
            // CANNOT SEE. r28 named the liar: the occupant on slot 31 answers
            // `addon=set part=set partClone=null`, so the synchronous marks call
            // it a real helmet while it draws nothing, and the engine hid the
            // hair behind it on every head build (`worn 0x108E drawn 0x8C`,
            // `re-enabled 0x2` at repaint, at settle+4s and again at
            // settle+10s - one bald flash each). BipedPost's sweep already
            // walks the biped and gets this right; MeasuredGhosts carries its
            // answer here.
            //
            // ⚠ ADDITIVE, AND A MISS CHANGES NOTHING. Until an actor has been
            // swept there is no measurement, and the strict test above stands
            // alone - so a genuine helmet still hides hair from the frame it is
            // worn, and nothing here can invent a ghost for a piece that is
            // merely mid-attach.
            if (std::uint32_t measured = 0;
                MeasuredGhosts::Read(a_actor ? a_actor->GetFormID() : 0u, measured)) {
                ghost |= measured & kHeadPartMask;
            }
            // ⚠ THE NEAR MISS IS THE INTERESTING CASE, AND r27 IS WHY. The field
            // round dropped the HEAD bit and left the HAIR bit standing, while
            // the ladder in the same second reported slot 31's Iron Helmet as
            // staging no geometry and re-enabled 0x2 behind it - one bald flash
            // per build, exactly the war this filter was meant to end. So the
            // occupant on 31 carries an addon or a part that renders nothing,
            // and relaxing the test blind would let a REAL helmet's hair-hide
            // slip mid-attach. This says which of the four marks is lying, so
            // the next round names the mechanism instead of guessing at it.
            for (const auto bit : { kBitHead, kBitHair }) {
                const auto& obj = biped->objects[bit];
                if (!obj.item || (ghost & (1u << bit)) != 0) {
                    continue;
                }
                static std::uint32_t s_lastNearMiss = 0;
                const std::uint32_t  key =
                    (a_actor ? a_actor->GetFormID() : 0u) ^ (bit << 28) ^
                    (obj.addon ? 0x4000000u : 0u) ^ (obj.part ? 0x8000000u : 0u) ^
                    (obj.partClone ? 0x10000000u : 0u);
                if (key == s_lastNearMiss) {
                    continue;
                }
                s_lastNearMiss = key;
                spdlog::info(
                    "wornmask: actor {:08X} head-part slot {} is OCCUPIED and kept "
                    "its bit - addon={} part={} partClone={}. If the ladder calls "
                    "this slot a ghost in the same second, the mark that is set "
                    "here is the one that lies.",
                    a_actor ? a_actor->GetFormID() : 0, bit == kBitHead ? 30 : 31,
                    obj.addon ? "set" : "null", obj.part ? "set" : "null",
                    obj.partClone ? "set" : "null");
            }
            return ghost;
        }

        // One line when the answer CHANGES, not one per engine ask - the mask
        // is read on every head build and a steady ghost would flood the log.
        void LogGhostDrop(RE::Actor* a_actor, std::uint32_t a_ghost) {
            static std::uint32_t s_lastKey = 0;
            const std::uint32_t  key =
                a_actor ? (a_actor->GetFormID() ^ (a_ghost << 24)) : 0;
            if (key == s_lastKey) {
                return;
            }
            s_lastKey = key;
            if (a_ghost != 0) {
                spdlog::info(
                    "wornmask: actor {:08X} ghost occupant on head-part slot(s) "
                    "{:08X} (worn, stages nothing) - bits dropped before the engine "
                    "reads them; the hide never fires.",
                    a_actor ? a_actor->GetFormID() : 0, a_ghost);
            }
        }

        std::uint32_t ShimmedWornMask(std::uint32_t a_real, const DisplaySet& a_display,
                                      RE::Actor* a_actor) {
            auto mask = a_real;
            if (const auto cov = RenderedCoverageFor(a_actor)) {
                mask = RenderedWornMask(a_real, cov->hidden, cov->styled);
                spdlog::debug("wornmask real={:08X} -> {:08X} (hidden={:08X} styled={:08X})",
                              a_real, mask, cov->hidden, cov->styled);
            }
            // Ghosts fall BEFORE the hair mode so an explicit kHide - the
            // player's own "hide my hair" - still lands on the engine.
            const auto ghost = GhostHeadPartMask(a_actor);
            LogGhostDrop(a_actor, ghost);
            mask = DropGhostHeadPartBits(mask, ghost);
            return ApplyHairMode(mask, a_display.hair);
        }

        // noexcept: called from the engine's 24220 frame. Compute the vanilla
        // mask first so the catch can always fall back to it; on ANY throw
        // (mutex lock, spdlog), return the unshimmed mask - vanilla behavior.
        std::uint32_t GetWornMaskThunk(RE::InventoryChanges* a_changes) noexcept {
            const auto real = g_origGetWornMask(a_changes);
            try {
                auto& session = OutfitSession::GetSingleton();
                auto* player  = RE::PlayerCharacter::GetSingleton();
                auto* owner   = a_changes ? a_changes->owner : nullptr;
                if (!owner) {
                    return real;
                }

                if (player && owner == player) {
                    // ---- PLAYER PATH ---------------------------------------
                    // ⚠ STAND DOWN WHILE APPAREL PREVIEW OWNS THE LOOK. AP
                    // shims this same call above us and unions the previewed
                    // piece's coverage onto whatever we return, so our styled
                    // bits would survive into a mask describing geometry that
                    // is no longer on the biped - a slot-30 style plus a
                    // previewed helmet left the player headless. Answering
                    // vanilla lets AP union onto reality instead. The whole
                    // derivation, with the two log lines that show the chain,
                    // is in ApparelPreviewSignal.h.
                    //
                    // Player-only on purpose: AP previews on the player alone,
                    // so a follower's head and hair culling is untouched by a
                    // preview and must keep its own shim.
                    if (ApparelPreviewActive()) {
                        // ⚠ CONFIRMED IN THE FIELD 2026-08-06, and this is the
                        // line that proves it:
                        //
                        //   APStandDown: returning real=0000108E instead of
                        //   0000108C (hairMode=1 hairBitReal=true
                        //   hairBitWould=false)
                        //
                        // hairMode 1 is kShow, so the player had asked for hair
                        // visible under headgear. Bit 31 (slot 31, the hair
                        // head-part) is SET in real and CLEAR in what we would
                        // have returned, so standing down handed the engine the
                        // real helmet's hair bit and 24220 culled her hair.
                        //
                        // ⚠ SO THE STAND-DOWN KEEPS ApplyHairMode AND DROPS ONLY
                        // THE COVERAGE. Those are two different kinds of claim
                        // sharing one function. RenderedWornMask says which slots
                        // OUR geometry covers, and Apparel Preview may have
                        // replaced that geometry, so it must not survive. The
                        // hair mode says what the PLAYER asked for, and a hover
                        // preview cannot make that untrue, so it must.
                        //
                        // The fail-safe direction is unchanged: styled coverage
                        // still stands down, so a head culled over nothing is
                        // still impossible. See ApparelPreviewSignal.h.
                        //
                        // ⚠ AND A THIRD CLAIM, FOUND THE SAME WAY (field
                        // 2026-08-06: "hair disappears when I preview helmets,
                        // and it happens when we have a hidden helmet slot").
                        // The HIDE has to survive too. This used to return the
                        // raw mask, which still carries a hidden helmet's bits,
                        // because hiding removes the GEOMETRY and leaves the
                        // item equipped. So the engine culled hair for a helmet
                        // nobody could see.
                        //
                        // Coverage and hide are opposite kinds of statement and
                        // that is why they part company here. "Our geometry
                        // covers this slot" is exactly what Apparel Preview may
                        // have made false. "The real gear on this slot is gone"
                        // is not something a preview can undo: it does not put
                        // the hidden helmet back.
                        //
                        // AP unions its own preview coverage onto the answer
                        // afterwards, so a previewed helmet still hides hair -
                        // by the piece actually on the biped rather than by the
                        // one we took off. The derivation is on
                        // PreviewStandDownMask in SlotMask.h.
                        const auto cov = RenderedCoverageFor(player);
                        return ApplyHairMode(
                            PreviewStandDownMask(real, cov ? cov->hidden : 0u),
                            session.Display().hair);
                    }
                    if (!session.IsActive()) {
                        return real;
                    }
                    // Helmet Toggle 2 needs no handling here any more. The pass
                    // already skips styles overlapping the slots HT2 hides, so
                    // they never reach the published coverage - the mask cannot
                    // claim a helmet HT2 is hiding, and hair regrows as HT2
                    // expects, without this hook re-deriving the intersection.
                    return ShimmedWornMask(real, session.Display(), player);
                }

                // ---- NPC PATH (un-gated Task 3) -----------------------------
                // Same fast-out + snapshot lookup as HandleWornPass, shared via
                // LookupAssignedNpc (NpcLookup.h); HT2 stays player-only (§3).
                // Miss / scene / count == 0 -> the vanilla mask.
                const auto lk = LookupAssignedNpc(session, owner);
                if (!lk.entry) {
                    return real;
                }
                // Hide is worn-required against the engine's own mask; styles
                // may fill an unworn slot, exactly as in the styling pass, so an
                // injected follower helmet still hides hair with no real helmet
                // underneath it. Forms were pre-resolved - never resolve here.
                const DisplaySet d = NpcResolve::WornRequiredDisplay(lk.entry->display, real);
                return ShimmedWornMask(real, d, lk.actor);
            } catch (...) {
                return real;
            }
        }

    }

    void BipedHooks::InstallInjection() {
        // SE: the worn skinning exec (15856) is called from the small wrapper
        // 24231 at +0x81, and 24221 (the rebuild parent) is that wrapper's ONLY
        // caller (whole-exe xref, 2026-07-15). AE: the compiler INLINED the
        // wrapper into the rebuild parent 24725 - 24735 still exists in the
        // binary (its +0x81 call intact, which is why the old byte check
        // passed) but has ZERO callers, so a hook there never fires (Ivy's
        // 1.6.1170 diag log: mask shim ran, "wornpass ran 0x"). The live AE
        // site is the inlined call to 16096 at 24725+0x1EF; same displaced-
        // call ABI (rcx = InventoryChanges*, rdx = visitor&), and the inlined
        // visitor carries the same vtable (id 195851, byte-verified: lea at
        // +0x19E loads 0x17E5488 on 1.6.1170) so the pass gate below works
        // unchanged. The sibling inline call at 24725+0xFC is a different
        // visitor (0x17E54C0, a checker) - do not hook that one.
        // ⚠ THE OFFSET COMES FROM VersionCheck, NOT FROM A CONSTANT HERE. On
        // any build but the two measured by hand it is somewhere else, and on
        // AE it cannot be found by the callee alone: 24725 calls 16096 twice,
        // and +0xFC is the checker pass described above. VersionCheck picks
        // the one that is handed the worn visitor vtable.
        const auto callOffset = VersionCheck::WornPassCallOffset();
        if (callOffset == 0) {
            spdlog::error("BipedHooks: no worn-pass call site on this runtime; injection NOT "
                          "installed. (plugin.cpp refuses to load in this state, so reaching "
                          "here means the self-check was bypassed.)");
            return;
        }
        const REL::Relocation<std::uintptr_t> site{ REL::RelocationID(24231, 24725), callOffset };
        if (*reinterpret_cast<std::uint8_t*>(site.address()) != 0xE8) {
            spdlog::error("BipedHooks: expected E8 at the worn-pass call site (+0x{:X}), found {:02X}; injection NOT installed.",
                          callOffset, *reinterpret_cast<std::uint8_t*>(site.address()));
            return;
        }

        // The engine worn-pass visitor's vtable, for the pass gate. Resolved
        // here so a bad address-library lookup fails at install time, never in
        // the hot path.
        g_wornVisitorVtbl = REL::RelocationID(241890, 195851).address();

        // Plain thunk, no xbyak stub: the displaced call's own arguments are
        // everything we are entitled to (see HandleWornPass).
        g_origVisitWorn = reinterpret_cast<VisitWorn_t>(
            SKSE::GetTrampoline().write_call<5>(site.address(), HandleWornPass));
        injectionOk_ = true;
        spdlog::info("BipedHooks: injection hook installed at SE 24231+0x81 / AE 24725+0x1EF (register-free thunk).");
    }

    void BipedHooks::InstallWornMaskShim() {
        const auto callOffset = VersionCheck::WornMaskCallOffset();
        if (callOffset == 0) {
            spdlog::error("BipedHooks: no worn-mask call site on this runtime; mask shim NOT "
                          "installed.");
            return;
        }
        const REL::Relocation<std::uintptr_t> site{ REL::RelocationID(24220, 24724), callOffset };
        if (*reinterpret_cast<std::uint8_t*>(site.address()) != 0xE8) {
            spdlog::error("BipedHooks: expected E8 at the worn-mask call site (+0x{:X}), found {:02X}; mask shim NOT installed.",
                          callOffset, *reinterpret_cast<std::uint8_t*>(site.address()));
            return;
        }
        g_origGetWornMask = reinterpret_cast<GetWornMask_t>(
            SKSE::GetTrampoline().write_call<5>(site.address(), GetWornMaskThunk));
        maskShimOk_ = true;
        spdlog::info("BipedHooks: worn-mask shim installed at the 24220/24724 call site.");
    }

    void BipedHooks::Install() {
        InstallInjection();
        InstallWornMaskShim();
    }

    bool BipedHooks::AllInstalled() { return injectionOk_ && maskShimOk_; }

    std::uint64_t BipedHooks::PlayerWornPassCount() {
        return g_playerWornPass.load(std::memory_order_relaxed);
    }

}  // namespace OS
