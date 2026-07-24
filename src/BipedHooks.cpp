#include "BipedHooks.h"

#include "BipedPost.h"
#include "NpcLookup.h"
#include "OutfitSession.h"
#include "REAugments.h"
#include "SlotMask.h"
#include "VersionCheck.h"

#include <functional>

namespace OS {

    namespace {
        std::atomic<std::uint64_t> g_playerWornPass{ 0 };

        // ---- 24231+0x81: the call to 15856 (ExecuteVisitorOnWorn) ----
        using VisitWorn_t = void (*)(RE::InventoryChanges*, RE::InventoryChanges::IItemChangeVisitor&);
        VisitWorn_t g_origVisitWorn = nullptr;

        // The actor's REAL worn armor, captured before we diverge the biped.
        // armo[bit] = the ARMO the actor wears in biped slot (30 + bit), or null;
        // coverage = the OR of every worn ARMO's slot mask (the actor's real worn
        // slots). The NPC worn-required rule (spec §3) intersects the outfit's
        // style/hide masks against coverage; the player ignores it.
        struct RealWorn {
            RE::TESObjectARMO* armo[32]{};
            std::uint32_t      coverage{ 0 };
        };

        // ONE walk over the actor's worn inventory. The old shape called
        // Actor::GetWornArmor 32x - and each of those builds the FULL armor
        // inventory (Actor.cpp: GetInventory + linear scan), so a styled pass
        // paid 32 inventory builds; that does not scale to a market square of
        // styled NPCs (spec §3). Semantically identical to the per-slot version:
        // CommonLib's GetWornArmor(slot) returns the first worn ARMO whose slot
        // mask covers the slot (count>0 && entry->IsWorn() && armor->HasPartOf,
        // Actor.cpp), and every worn item necessarily owns an InventoryEntryData
        // in entryList (ExtraWorn/ExtraWornLeft live on an entry's extra list -
        // RE/I/InventoryEntryData.cpp::IsWorn), so walking entryList once and
        // OR-ing each worn ARMO's GetSlotMask into the array fills all 32 slots
        // with the same first-worn-wins result. Verified against
        // RE/I/InventoryChanges.h (entryList/owner) + InventoryEntryData.h
        // (object/IsWorn) + Actor.cpp (GetWornArmor). Takes the pass's own
        // InventoryChanges* (a_changes->owner is the actor) - no extra lookup.
        RealWorn SnapshotRealWorn(RE::InventoryChanges* a_changes) {
            RealWorn r;
            if (!a_changes || !a_changes->entryList) {
                return r;
            }
            for (auto* entry : *a_changes->entryList) {
                if (!entry || !entry->object || !entry->object->IsArmor() || !entry->IsWorn()) {
                    continue;
                }
                auto* armo = entry->object->As<RE::TESObjectARMO>();
                if (!armo) {
                    continue;
                }
                const auto mask = static_cast<std::uint32_t>(armo->GetSlotMask());
                r.coverage |= mask;
                for (std::uint32_t bit = 0; bit < 32; ++bit) {
                    if (((mask >> bit) & 1u) && !r.armo[bit]) {
                        // First worn ARMO seen wins the slot. GetWornArmor breaks
                        // the same tie by the inventory map's pointer-key order,
                        // so the winner can differ ONLY when two worn ARMOs cover
                        // one slot - a state the engine's one-item-per-slot equip
                        // never produces. coverage (the §3 input) is tie-order-
                        // independent regardless.
                        r.armo[bit] = armo;
                    }
                }
            }
            return r;
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
                             std::uint32_t                             a_ht2Suppressed,
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
                            skinCoverage |= static_cast<std::uint32_t>(arma->GetSlotMask());
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
                        const auto mask = static_cast<std::uint32_t>(armo->GetSlotMask());
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
            //     and each one needs the honesty restore. a_ht2Suppressed is 0
            //     for NPCs (Helmet Toggle 2's GLOB is player-only, §3), so the
            //     suppression check short-circuits away entirely for them.
            std::uint32_t styledCoverage = 0;
            auto          applyStyle = [&](std::uint32_t a_bit, RE::TESObjectARMO* a_armo) {
                if (!CanApplyStyleBit(a_bit, a_real.coverage)) {
                    return;  // shield style requires a real equipped shield
                }
                if (a_ht2Suppressed &&
                    (static_cast<std::uint32_t>(a_armo->GetSlotMask()) & a_ht2Suppressed)) {
                    spdlog::debug("  style bit {} '{}' skipped: Helmet Toggle hides this slot.",
                                  a_bit, a_armo->GetName());
                    return;
                }
                const bool ok = REAug::ApplyArmorAddon(a_armo, race, awm, isFemale);
                styledCoverage |= static_cast<std::uint32_t>(a_armo->GetSlotMask());
                for (auto* arma : a_armo->armorAddons) {
                    if (arma) {
                        styledCoverage |= static_cast<std::uint32_t>(arma->GetSlotMask());
                    }
                }
                spdlog::debug("  style bit {} inject '{}' -> {}", a_bit, a_armo->GetName(), ok);
            };
            a_emitStyles(applyStyle);

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
            if (a_display.hiddenAttachmentMask) {
                BipedPost::CullNodes(biped, a_display.hiddenAttachmentMask);
                BipedPost::QueueNodeCull(a_handle, a_display.hiddenAttachmentMask);
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
            std::uint32_t ht2Suppressed = 0;
            if (HT2HidesHeadgear()) {
                for (auto* wornArmo : a_real.armo) {
                    if (wornArmo) {
                        ht2Suppressed |=
                            static_cast<std::uint32_t>(wornArmo->GetSlotMask()) &
                            kHT2HeadSlots;
                    }
                }
            }

            const auto display =
                OutfitSession::GetSingleton().Display();
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
                            skinCoverage |= static_cast<std::uint32_t>(
                                arma->GetSlotMask());
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
                            static_cast<std::uint32_t>(armo->GetSlotMask());
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
            OutfitSession::GetSingleton().VisitStyles(
                [&](std::uint32_t a_bit, RE::TESObjectARMO* a_armo) {
                    if (!CanApplyStyleBit(a_bit, a_real.coverage)) {
                        return;  // no first-person shield without real slot 39 gear
                    }
                    auto coverage = static_cast<std::uint32_t>(a_armo->GetSlotMask());
                    for (auto* arma : a_armo->armorAddons) {
                        if (arma) {
                            coverage |= static_cast<std::uint32_t>(arma->GetSlotMask());
                        }
                    }
                    if ((coverage & kFirstPersonArmorMask) == 0) {
                        return;  // nothing the 1P model renders
                    }
                    if (ht2Suppressed && (coverage & ht2Suppressed)) {
                        spdlog::debug("  1p style bit {} '{}' skipped: Helmet Toggle hides this slot.",
                                      a_bit, a_armo->GetName());
                        return;
                    }
                    const bool ok = REAug::ApplyArmorAddon(a_armo, race, awm, isFemale);
                    styled |= coverage;
                    spdlog::debug("  1p style bit {} inject '{}' -> {}", a_bit,
                                  a_armo->GetName(), ok);
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

                    const auto display = session.Display();
                    const auto real    = SnapshotRealWorn(a_changes);

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
                    std::uint32_t ht2Suppressed = 0;
                    if (HT2HidesHeadgear()) {
                        for (auto* wornArmo : real.armo) {
                            if (wornArmo) {
                                ht2Suppressed |=
                                    static_cast<std::uint32_t>(wornArmo->GetSlotMask()) &
                                    kHT2HeadSlots;
                            }
                        }
                    }

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
                        ht2Suppressed, player->GetHandle());
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
                const DisplaySet display =
                    NpcResolve::WornRequiredDisplay(entry.display, real.coverage);

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
                    /*ht2Suppressed*/ 0u, lk.actor->GetHandle());
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
        // 24220 is the mask's ONLY consumer; it uses it solely to set/clear
        // NiAVObject::kHidden on hair/head-part nodes. So: add the bits our
        // styled gear occupies (a styled helmet must still hide hair) and clear
        // the bits of head-part slots we are hiding (a hidden helmet frees hair).
        using GetWornMask_t = std::uint32_t (*)(RE::InventoryChanges*);
        GetWornMask_t g_origGetWornMask = nullptr;

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
                    // ---- PLAYER PATH (behavior-identical to HEAD) -----------
                    if (!session.IsActive()) {
                        return real;
                    }
                    const auto d = session.Display();
                    // Helmet Toggle 2: hidden worn head slots contribute no style
                    // bits - the styled piece is suppressed in the pass above, and
                    // keeping the bit here would leave the character bald-with-
                    // hidden-hair (hair must regrow exactly as HT2 expects).
                    auto styleMask = d.styleMask;
                    if (HT2HidesHeadgear()) {
                        styleMask &= ~(kHT2HeadSlots & real);
                    }
                    const auto shimmed = (real | styleMask) & ~d.hiddenHeadPartMask;
                    spdlog::debug("wornmask real={:08X} -> {:08X} (style={:04X} hideHead={:04X})",
                                  real, shimmed, d.styleMask, d.hiddenHeadPartMask);
                    return shimmed;
                }

                // ---- NPC PATH (un-gated Task 3) -----------------------------
                // Same fast-out + snapshot lookup as HandleWornPass, shared via
                // LookupAssignedNpc (NpcLookup.h); HT2 stays player-only (§3).
                // Miss / scene / count == 0 -> the vanilla mask.
                const auto lk = LookupAssignedNpc(session, owner);
                if (!lk.entry) {
                    return real;
                }
                // Hide is worn-required against the engine's own mask. Styles
                // remain intact and are ORed in here, so an injected follower
                // helmet still hides the appropriate hair/head-part nodes even
                // when no gameplay helmet is equipped underneath it.
                const DisplaySet d = NpcResolve::WornRequiredDisplay(lk.entry->display, real);
                const auto shimmed = (real | d.styleMask) & ~d.hiddenHeadPartMask;
                spdlog::debug("wornmask NPC {:08X} real={:08X} -> {:08X} (hideHead={:04X})",
                              lk.baseFormID, real, shimmed, d.hiddenHeadPartMask);
                return shimmed;
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
