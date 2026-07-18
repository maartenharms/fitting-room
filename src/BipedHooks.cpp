#include "BipedHooks.h"

#include "BipedPost.h"
#include "OutfitSession.h"
#include "REAugments.h"
#include "SlotMask.h"

namespace OS {

    namespace {
        std::atomic<std::uint64_t> g_playerWornPass{ 0 };

        // ---- 24231+0x81: the call to 15856 (ExecuteVisitorOnWorn) ----
        using VisitWorn_t = void (*)(RE::InventoryChanges*, RE::InventoryChanges::IItemChangeVisitor&);
        VisitWorn_t g_origVisitWorn = nullptr;

        // Snapshot the player's REAL worn armor before we diverge the biped.
        struct RealWorn {
            RE::TESObjectARMO* armo[32]{};
        };

        RealWorn SnapshotRealWorn(RE::Actor* a_player) {
            RealWorn r;
            using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
            for (std::uint32_t bit = 0; bit < 32; ++bit) {
                r.armo[bit] = a_player->GetWornArmor(static_cast<Slot>(1u << bit));
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

        // ---- BUG-2: the 1st-person worn pass ----
        // The 1P arms model is built from GetBiped1(true), which the 3P-only
        // injection below never touches - so styled hands kept showing the
        // REAL gauntlets in first person. During the pass that stages the 1P
        // biped, inject styles whose coverage includes a slot the 1P model
        // renders (body 32 / hands 33 / forearms 34 - the visible arms belong
        // to the BODY piece's armor addon; shield is kNeverTouch, head slots
        // have no 1P geometry). STYLE ONLY by design: hide/cull on the 1P
        // biped is unexplored territory, and the 3P pass owns every
        // gameplay-visible hide behavior (worn-mask shim included).
        void StyleFirstPersonPass(RE::BipedAnim* a_passBiped, RE::Actor* a_player,
                                  const RealWorn& a_real) {
            const auto&    holder1p = a_player->GetBiped1(true);
            RE::BipedAnim* biped1p  = holder1p.get();
            if (!biped1p || a_passBiped != biped1p) {
                spdlog::debug("wornpass: pass biped matches neither holder; skipped.");
                return;
            }

            constexpr auto k1PStyleSlots = MaskForEditorSlot(32) |
                                           MaskForEditorSlot(33) |
                                           MaskForEditorSlot(34);

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

            std::uint32_t styled = 0;
            OutfitSession::GetSingleton().VisitStyles(
                [&](std::uint32_t a_bit, RE::TESObjectARMO* a_armo) {
                    auto coverage = static_cast<std::uint32_t>(a_armo->GetSlotMask());
                    for (auto* arma : a_armo->armorAddons) {
                        if (arma) {
                            coverage |= static_cast<std::uint32_t>(arma->GetSlotMask());
                        }
                    }
                    if ((coverage & k1PStyleSlots) == 0) {
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

            // Same honesty restore as the 3P pass: staged slots point .item
            // back at the real gear (or the skin) so XP/conflict readers stay
            // honest on this biped too.
            if (styled) {
                BipedPost::RestoreRealItems(biped1p, styled, a_real.armo, nakedSkin);
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
                if (!forPlayer || !session.IsActive()) {
                    g_origVisitWorn(a_changes, a_visitor);  // vanilla, untouched
                    return;
                }

                const auto display = session.Display();
                const auto real    = SnapshotRealWorn(player);

                // (1) Worn pass - ALWAYS with the engine's own visitor, never a
                //     proxy: co-hooked mods on the shared 24231 chain read the
                //     visitor's engine layout (a proxy deterministically crashed
                //     Immersive Equipment Displays - crash-2026-07-10-{19-02,
                //     20-05}.log). Hide therefore acts strictly AFTER the pass,
                //     on staging the engine already built (skin re-apply +
                //     node cull below), handing nothing foreign down-chain.
                g_origVisitWorn(a_changes, a_visitor);
                passRan = true;  // from here a throw cannot leave the actor naked

                // (2) The biped holder, derived register-free from the actor
                //     (the Cause-B fix). GetBiped1 is virtual slot 0x7E and
                //     returns the ADDRESS of the biped smart-pointer field -
                //     PlayerCharacter+0x260 for firstPerson=false (verified:
                //     re_verify/disasm_24231_prologue.txt, IDs 39399/39189).
                //     That is byte-for-byte the holder shape 24221 hands 24231
                //     in r8: ApplyArmorAddon and its callees only ever *read*
                //     the holder (17361+0x1AE `mov rcx,[r12]`), so pointing
                //     them at the actor's own field is ABI-identical to the
                //     engine's &visitor.biped.
                const auto&    holder = player->GetBiped1(false);
                RE::BipedAnim* biped  = holder.get();
                if (!biped) {
                    spdlog::debug("wornpass: player has no 3rd-person biped; nothing to style.");
                    return;
                }

                // (3) Pass gate: 24231 runs once per biped (3rd- and 1st-person
                //     for the player). The 3rd-person pass gets the full
                //     treatment below; the 1st-person pass gets style-only
                //     injection (BUG-2 - styled hands showed real gauntlets in
                //     first person because GetBiped1(true) was never staged).
                //     If the visitor is foreign we cannot discriminate - run
                //     the 3P work anyway (idempotent, at worst duplicated),
                //     matching the pre-BUG-2 behavior.
                if (auto* passBiped = EnginePassBiped(a_visitor); passBiped && passBiped != biped) {
                    StyleFirstPersonPass(passBiped, player, real);
                    return;
                }

                auto*       base      = player->GetActorBase();
                auto*       race      = player->GetRace();
                const bool  isFemale  = base && base->IsFemale();
                auto* const nakedSkin = REAug::GetActorSkin(player);
                auto* const awm       = reinterpret_cast<REAug::ActorWeightModel*>(
                    const_cast<RE::BSTSmartPointer<RE::BipedAnim>*>(&holder));

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
                if (display.hiddenBodySkinMask) {
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
                            auto* armo = real.armo[bit];
                            if (!armo || armo == nakedSkin) {
                                continue;
                            }
                            bool seen = false;  // dedupe: one re-apply per ARMO
                            for (std::uint32_t j = 0; j < bit && !seen; ++j) {
                                seen = real.armo[j] == armo;
                            }
                            const auto mask = static_cast<std::uint32_t>(armo->GetSlotMask());
                            if (seen || (mask & display.hideMask) != 0 || (mask & skinCoverage) == 0) {
                                continue;
                            }
                            const bool back = REAug::ApplyArmorAddon(armo, race, awm, isFemale);
                            spdlog::debug("  hide: re-apply kept gear '{}' -> {}", armo->GetName(), back);
                        }
                    } else {
                        spdlog::warn("hide: no actor skin resolvable; body-class hide skipped.");
                    }
                }

                // (5) STYLE: last-wins staging replaces the worn piece's 3D
                //     (and any skin re-applied in (4)). ApplyArmorAddon writes
                //     objects[] synchronously via 15500. Track the styles' FULL
                //     slot coverage: a multi-slot style ARMO (e.g. body armor
                //     that also claims slot 46) stages .item into every covered
                //     slot, and each one needs the honesty restore.
                // Helmet Toggle 2: while it hides worn headgear, suppress styles
                // that overlap the hidden WORN head slots (intersection keeps
                // unrelated headwear styled - a circlet style in 42 survives a
                // hidden hood covering 30/31). DAV's invisible variant then wins
                // the slot and the hide actually hides.
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

                std::uint32_t styledCoverage = 0;
                session.VisitStyles([&](std::uint32_t a_bit, RE::TESObjectARMO* a_armo) {
                    if (ht2Suppressed &&
                        (static_cast<std::uint32_t>(a_armo->GetSlotMask()) & ht2Suppressed)) {
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
                });

                // (6) Gameplay honesty. objects[] was written inline by 15500 on
                //     this very call stack, so restoring here has a zero race
                //     window. Armor-skill XP (37673->37589->37688) and
                //     equip-conflict (36979->14026) read objects[i].item - hidden
                //     body slots now hold the skin ARMO after (4), so this also
                //     keeps hidden-but-worn gear honest for XP.
                BipedPost::RestoreRealItems(
                    biped, display.styleMask | display.hideMask | styledCoverage, real.armo,
                    nakedSkin);

                // (7) HIDE, attachment slots (helmet/amulet/ring/cloak/…): their
                //     meshes are self-contained, so cull the staged 3D
                //     (NiAVObject::kHidden on objects[slot].partClone - Equipment
                //     Toggle 2's technique). Sync first for models 15500 already
                //     cloned on this stack; the deferred sweep catches clones the
                //     BSTaskPool attaches late (uncached models). Hair/head-part
                //     regrowth for helmet/circlet stays the mask shim's job.
                if (display.hiddenAttachmentMask) {
                    BipedPost::CullNodes(biped, display.hiddenAttachmentMask);
                    BipedPost::QueueNodeCull(display.hiddenAttachmentMask);
                }
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
                if (!session.IsActive() || !a_changes || !player || a_changes->owner != player) {
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
        const REL::Relocation<std::uintptr_t> site{ REL::RelocationID(24231, 24725),
                                                    REL::VariantOffset(0x81, 0x1EF, 0x81) };
        if (*reinterpret_cast<std::uint8_t*>(site.address()) != 0xE8) {
            spdlog::error("BipedHooks: expected E8 at the worn-pass call site (SE 24231+0x81 / AE 24725+0x1EF), found {:02X}; injection NOT installed.",
                          *reinterpret_cast<std::uint8_t*>(site.address()));
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
        const REL::Relocation<std::uintptr_t> site{ REL::RelocationID(24220, 24724),
                                                    REL::VariantOffset(0x7C, 0x80, 0x7C) };
        if (*reinterpret_cast<std::uint8_t*>(site.address()) != 0xE8) {
            spdlog::error("BipedHooks: expected E8 at the worn-mask call site (24220/24724), found {:02X}; mask shim NOT installed.",
                          *reinterpret_cast<std::uint8_t*>(site.address()));
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
