#include "WeaponHooks.h"

#include "VersionCheck.h"

#include "NpcLookup.h"
#include "OutfitSession.h"
#include "StyleCatalog.h"
#include "WeaponSlots.h"

#include <atomic>

namespace OS {

    namespace {
        // Counts PLAYER part loads that reached the styling decision - the
        // RefreshPlayer tripwire that reads it stays meaningful only if this
        // never advances for anyone else. The fetch_add call site below sits
        // strictly inside the player branch (Task 4 added a sibling NPC branch
        // that never touches this counter): an assigned NPC/follower must never
        // bump it, or the tripwire would stop proving what its name claims.
        std::atomic<std::uint64_t> g_playerWeaponParts{ 0 };

        // The same tripwire for the NPC weapon site (SE 15506+0x1D0 / AE
        // 15683+0x2FA - a DIFFERENT offset from the player site, so a distinct
        // AE-addrlib-drift candidate). Task 4 made that site live; without its
        // own counter an AE drift there would be invisible, which is exactly the
        // inlined-site failure mode that cost a release once. Kept separate so
        // the player tripwire stays player-scoped.
        std::atomic<std::uint64_t> g_npcWeaponParts{ 0 };

        // One debug line per weapon class, ever. The thunk is a per-part-load
        // hot path, so an inert-hide notice must not log per load. Bit i = the
        // class with that enumerator value has already been reported.
        std::atomic<std::uint32_t> g_hideNoticed{ 0 };
        static_assert(kWeaponClassCount <= 32, "g_hideNoticed is a 32-bit class bitmask");

        // ---- the part-3D loader (SE 15526 / AE 15703) ----
        //
        // Main-hand weapons and ammo stage into BipedAnim::objects[32..41].
        // Off-hand weapons use the actor race's shield/editor biped slot
        // instead. The stager
        // (SE 15505, re_verify/wpn_batch2_stagers.c:39-40) writes .item = the
        // form and .part = the form's TESModelTextureSwap. But the 3D only
        // materializes through ONE funnel, this loader: it GetModel()s .part,
        // loads and clones the NIF, applies the texture swap, attaches the
        // clone to the skeleton bone the NIF's `Prn` NiStringExtraData names,
        // and stores the result in objects[slot].partClone.
        //
        // We swap the MODEL ARGUMENT here rather than the staged form, and that
        // is the whole design. Substituting at the model SOURCE means every
        // consumer downstream of the load sees the styled mesh for free, with
        // no cooperation from us: Simple Dual Sheath extracts the `Scb`
        // scabbard from the LOADED 3D, Immersive Equipment Displays
        // re-evaluates per actor after rebuilds, FSMP/CBPC bind physics to the
        // clone, and the enchant glow wraps whatever geometry arrived.
        // Meanwhile objects[slot].item keeps the REAL WEAP/AMMO, so none of the
        // bookkeeping that reads it needs the honesty restore the armor path
        // has to do (BipedPost::RestoreRealItems): armor XP, equip-conflict
        // detection and the AI's own equip state stay true by construction,
        // and there is no change-detect flap because nothing was changed. The
        // .part member itself is never written either - only this one call's
        // argument - so a rebuild re-derives it from vanilla state.
        //
        // Signature, from the SE 1.5.97 decomp of FUN_1401CA470
        // (re_verify/wpn_batch4_attach.c:394-395), cross-checked against the
        // three call sites (wpn_batch3_family.c:72, :82, :397):
        //
        //   NiAVObject* (TESModel* part, int slot, TESObjectREFR* refr,
        //                BSTSmartPointer<BipedAnim>* biped, NiAVObject* root)
        //
        // Each argument's type is derived from what the body DOES with it, then
        // confirmed against the real CommonLib headers - never from the name:
        //   - arg1 is a TESModel*: the body calls vfunc +0x20 for a `char*` NIF
        //     path (GetModel, index 4) and vfunc +0x30 for the swap data
        //     (GetAsModelTextureSwap, index 6). The call sites pass
        //     biped+slot*0x78+0x20, which is objects[slot].part exactly
        //     (BipedAnim.h: objects at +0x10, stride 0x78; BIPOBJECT::part at
        //     +0x10) - so .part is what we are being handed, and what we swap.
        //   - arg2 is a signed int slot: the body guards `slot != -1` (the
        //     AnimObject caller passes -1; we do not hook it).
        //   - arg3 is a TESObjectREFR*: the body defaults arg5 from its vfunc
        //     +0x380 = index 0x70 = Get3D2 (TESObjectREFR.h:304), and the
        //     callers resolve it from BipedAnim::actorRef (+0x2770).
        //   - arg4 is a POINTER TO the biped holder, not the biped: the body
        //     dereferences it (`*arg4 + slot*0x78 + 0x38`). Actor.h:645 types
        //     that holder BSTSmartPointer<BipedAnim>.
        //   - arg5 is the attach root: defaulted from refr->Get3D2() and used
        //     as the AttachChild target.
        //
        // Five arguments = four registers plus ONE stack slot at [rsp+0x20].
        // The thunk declares the identical list, so the displaced call's frame
        // is reproduced verbatim; nothing here needs to know that, but a thunk
        // with the WRONG arity would silently corrupt the caller's stack.
        using LoadPart_t = RE::NiAVObject* (*)(RE::TESModel*, std::int32_t, RE::TESObjectREFR*,
                                               RE::BSTSmartPointer<RE::BipedAnim>*,
                                               RE::NiAVObject*);

        // One original per site: write_call<5> returns the displaced target for
        // the site it patched, and the three sites must each chain back through
        // their own (a co-hooked mod may have displaced one and not another).
        //
        // The two weapon sites are NOT peers - they are the two arms of one
        // if/else on the actor (wpn_batch3_family.c:68 `if (refr ==
        // PlayerCharacter singleton)` ... :79 `else`). Through v1's player-only
        // gate the PLAYER site was the only one that could ever style, and the
        // NPC site was dead code, hooked anyway so the eventual un-gating would
        // be a gate change only. Task 4 IS that gate change: LoadPartThunk now
        // has an NPC branch (LookupAssignedNpc), so the NPC site is live -
        // still the same thunk template at both sites, the gate lives inside
        // it, not per-site.
        LoadPart_t g_origWeaponPlayer = nullptr;  // SE 15506+0x17F / AE 15683+0x2B1
        LoadPart_t g_origWeaponNPC    = nullptr;  // SE 15506+0x1D0 / AE 15683+0x2FA
        LoadPart_t g_origQuiver       = nullptr;  // SE 15511+0x141 / AE 15688+0x199

        // The base model of a WEAP/AMMO: its own TESModelTextureSwap subobject.
        // That subobject sits at form+0x40 for both types (TESObjectWEAP.h:59,
        // TESAmmo.h:41), which is literally the `param_2 + 0x40` the stager
        // writes into .part by default - the static_cast performs the same
        // +0x40 adjustment the engine hard-codes. Reached through the concrete
        // type because both inherit TESModelTextureSwap non-virtually, off the
        // primary chain.
        RE::TESModel* BaseModelOf(RE::TESBoundObject* a_form) {
            if (auto* weap = a_form ? a_form->As<RE::TESObjectWEAP>() : nullptr) {
                return static_cast<RE::TESModelTextureSwap*>(weap);
            }
            if (auto* ammo = a_form ? a_form->As<RE::TESAmmo>() : nullptr) {
                return static_cast<RE::TESModelTextureSwap*>(ammo);
            }
            return nullptr;
        }

        // The WNAM (first-person model) of a WEAP, or nullptr when it has none.
        // TESObjectWEAP::firstPersonModelObject is a TESObjectSTAT* at +0x200
        // (TESObjectWEAP.h:261) and TESObjectSTAT inherits TESModelTextureSwap
        // at +0x30 (TESObjectSTAT.h:29) - together exactly the `*(weap+0x200) +
        // 0x30` chain the stager walks. AMMO has no WNAM at all.
        RE::TESModel* WnamModelOf(RE::TESForm* a_form) {
            auto* const weap = a_form ? a_form->As<RE::TESObjectWEAP>() : nullptr;
            auto* const stat = weap ? weap->firstPersonModelObject : nullptr;
            return stat ? static_cast<RE::TESModelTextureSwap*>(stat) : nullptr;
        }

        // Which of the style's two possible models to hand the loader.
        //
        // The stager picks .part BEFORE we run, and it does not always pick the
        // base model (re_verify/wpn_batch2_stagers.c:32-40): when the biped
        // being staged is the PLAYER's - either holder, first- OR third-person,
        // both compared via the player's vfunc +0x3F0, which is GetBiped1(bool)
        // at virtual slot 0x7E, the same one BipedHooks derives its holder from
        // - AND the WEAP carries a WNAM, it stages the WNAM STAT's model
        // instead of weap+0x40.
        //
        // So on the player the incoming .part may be EITHER model, and which
        // one arrived tells us which role the engine is filling right now.
        // Matching that role is the point: substituting a base model where the
        // engine asked for a WNAM one would render the wrong mesh for every
        // weapon whose first-person model differs from its world model. We
        // never re-derive the role ourselves - the incoming pointer IS the
        // engine's answer, and comparing against it is chain-safe in a way that
        // re-running the stager's player check would not be.
        RE::TESModel* PickModel(RE::TESBoundObject* a_style, RE::TESModel* a_incomingPart,
                                RE::TESForm* a_realForm) {
            if (auto* const realWnam = WnamModelOf(a_realForm);
                realWnam && a_incomingPart == realWnam) {
                if (auto* const styleWnam = WnamModelOf(a_style)) {
                    return styleWnam;
                }
                // The style has no WNAM of its own. Its base model is what the
                // stager would have chosen for it in this same position, so it
                // is the honest answer rather than a fallback.
            }
            return BaseModelOf(a_style);
        }

        // kHide on a weapon class is INERT in v1: it renders the real weapon.
        // The data model, codec and JSON still carry it (forward compat), and
        // the editor offers no hide affordance on weapon rows, so this is a
        // fallback for a hand-edited or forward-dated outfit rather than
        // something a user can reach.
        //
        // Two independent engine facts kill every hide mechanism available at
        // this seam, which is why this is deferred rather than merely unbuilt:
        //
        //  1. We cannot cull the node here. This loader does NOT write
        //     partClone - its CALLER does, from our return value, immediately
        //     after we return (wpn_batch3_family.c:75-76 and :80/:84 write
        //     biped+slot*0x78+0x30 = objects[slot].partClone). Anything we read
        //     during the thunk is the PREVIOUS clone or null.
        //
        //  2. Hiding our return value instead would work exactly once. The
        //     engine actively OWNS NiAVObject::kHidden on weapon nodes as a
        //     function of the actor's draw state, and writes it BOTH ways:
        //     wpn_batch4_attach.c:1016-1021 sets flags+0xF4 |= 1 or &= ~1 on
        //     the 3-bit draw state at +0xC4>>5, and FUN_14067b940 (:659) is a
        //     bare unconditional clear. We run at LOAD time, not per draw-state
        //     transition, so an attach-time hide is cleared by the first
        //     draw/sheathe cycle and never re-applied. A toggle that silently
        //     stops working after the player draws once is worse than no toggle.
        //
        // The remaining candidate - skip the load, return nullptr - breaks
        // archery: the nocked arrow is resolved out of the QUIVER's own 3D and
        // the entire attach is gated on it being non-null
        // (wpn_batch8_attacharrow.c:23 then :30, which then does
        // GetObjectByName inside that node at :34). No quiver 3D = no visible
        // nocked arrow for every archer.
        //
        // A real hide needs a seam that tracks draw state, not attach time.
        void NoteHideInert(WeaponClass a_class) {
            const auto bit = 1u << static_cast<std::uint32_t>(a_class);
            if ((g_hideNoticed.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
                spdlog::debug("weapon class {}: hide is not supported in this version and is "
                              "inert; rendering the real weapon.",
                              ClassJsonName(a_class));
            }
        }

        // Chain-safe by construction, exactly as BipedHooks::HandleWornPass:
        // consume ONLY the displaced call's real C++ arguments and NEVER read a
        // register. The weapon stagers are popular hook targets, so we may be
        // entered from another mod's compiled handler rather than from the
        // engine site - in which case every register that is not one of our
        // five arguments holds THAT mod's locals. Reading one is what produced
        // two historical CTDs on the armor chain; the biped comes from arg4 and
        // the actor from arg3, and nothing else is ours to touch.
        //
        // Structure note: the decision is computed FIRST and the vanilla call
        // happens exactly ONCE afterwards, on every path - a double-load is
        // unrepresentable rather than merely avoided.
        //
        // noexcept: called from an engine (or co-hooked mod) frame, so a
        // propagating exception is UB. Every throwing call - the session mutex,
        // spdlog - sits inside a try; the noexcept is the hard backstop that
        // turns an impossible escape into a defined terminate. On ANY throw we
        // still make the vanilla call, so a weapon can never go missing because
        // we failed.
        template <LoadPart_t* Orig>
        RE::NiAVObject* LoadPartThunk(RE::TESModel* a_part, std::int32_t a_slot,
                                      RE::TESObjectREFR* a_refr,
                                      RE::BSTSmartPointer<RE::BipedAnim>* a_biped,
                                      RE::NiAVObject* a_root) noexcept {
            RE::TESModel* model = a_part;  // what the loader will be handed

            try {
                auto& session = OutfitSession::GetSingleton();
                // Combined fast-out FIRST, before anything else: this runs on
                // every weapon and ammo part load in the game, for every actor.
                // Two acquire loads and no lock (GetSingleton is a function-
                // local static, so it costs a magic-static guard load first).
                // AnyWeaponStyling's acquire pairs with the release store under
                // the session lock, so a thunk that sees true is guaranteed to
                // also see the outfit that made it true rather than merely
                // usually seeing it; NpcRenderCount() (Task 4) is the same
                // acquire/release discipline for the actor dimension. Either
                // being nonzero means "maybe style something" for SOME actor -
                // proceed; both false/zero -> vanilla immediately, no snapshot
                // load, no lock, the "nobody assigned anything" case costing
                // exactly what it did before this task. Read once - the value
                // cannot change mid-thunk (game thread) and the player path
                // re-checks it below.
                const bool anyPlayerStyling = session.AnyWeaponStyling();
                if (anyPlayerStyling || session.NpcRenderCount() != 0) {
                    auto* const player = RE::PlayerCharacter::GetSingleton();
                    if (a_refr && player && a_refr == player) {
                        // ---- PLAYER PATH (byte-for-byte HEAD behavior) --------
                        // Re-tests AnyWeaponStyling() explicitly: the combined
                        // fast-out above also lets an NPC-only NpcRenderCount()
                        // through, and that must NOT style the player - the
                        // product of the two checks below is identical to the
                        // single `if (session.AnyWeaponStyling())` gate this
                        // branch lived under before Task 4.
                        if (anyPlayerStyling && a_biped && a_biped->get() &&
                            a_slot >= 0 &&
                            IsWeaponOrQuiverBipedSlot(static_cast<std::uint32_t>(a_slot))) {
                            auto* const biped = a_biped->get();
                            // The REAL form - read, never written. This is the
                            // pointer that keeps armor XP, equip-conflict and AI
                            // equip state honest without any restore pass.
                            auto* const realForm = biped->objects[a_slot].item;
                            if (const auto cls = ClassOfWeaponForm(realForm)) {
                                // A player weapon part IS loading through this
                                // site. Counted before the styling decision on
                                // purpose: even a pass-through class proves the
                                // chain is live, which is the entire point of the
                                // tripwire. PLAYER-SCOPED - see g_playerWeaponParts.
                                g_playerWeaponParts.fetch_add(1, std::memory_order_relaxed);

                                const auto hand = HandForBipedSlot(
                                    *cls, static_cast<std::uint32_t>(a_slot));
                                const auto entry =
                                    session.WeaponDisplayFor(*cls, hand);
                                if (entry.kind == SlotEntry::Kind::kHide) {
                                    // Inert in v1 - falls through to the vanilla
                                    // call below and renders the real weapon. See
                                    // NoteHideInert for the engine facts that
                                    // deferred this.
                                    NoteHideInert(*cls);
                                } else if (entry.kind == SlotEntry::Kind::kStyle && entry.style &&
                                           ClassOfWeaponForm(entry.style) == cls) {
                                    // Belt and suspenders on the class match: a
                                    // style is only ever rendered against the class
                                    // it was indexed under. Were the two to
                                    // disagree - a stale preset, a plugin whose
                                    // WEAP changed animType underneath it - the
                                    // mesh would land on the wrong biped slot's
                                    // bone, e.g. a greatsword on the dagger's hip.
                                    if (auto* const styled = PickModel(entry.style, a_part, realForm)) {
                                        model = styled;
                                    }
                                }
                            }
                        }
                    } else if (a_refr && a_biped && a_biped->get() &&
                               a_slot >= 0 &&
                               IsWeaponOrQuiverBipedSlot(static_cast<std::uint32_t>(a_slot))) {
                        // ---- NPC PATH (Task 4) ---------------------------------
                        // LookupAssignedNpc (NpcLookup.h, shared with BipedHooks)
                        // folds in the count/scene gate + the snapshot lookup: a
                        // scene stands this whole branch down (matching Task 3's
                        // armor rule) while the PLAYER branch above is untouched
                        // by SceneGuard here - its own WeaponDisplayFor ->
                        // EffectiveLocked already handles player-side scene
                        // suppression, so the player gate stays exactly as it
                        // was. No OutfitSession lock and no form resolution on
                        // this path - the snapshot was pre-resolved on the game
                        // thread when it was built.
                        const auto lk = LookupAssignedNpc(session, a_refr);
                        if (lk.entry) {
                            auto* const biped = a_biped->get();
                            // The REAL form, exactly as the player path reads it.
                            auto* const realForm = biped->objects[a_slot].item;
                            if (const auto cls = ClassOfWeaponForm(realForm)) {
                                // An assigned-NPC weapon part IS loading through
                                // the NPC site - counted before the styling
                                // decision, like the player counter, so the site
                                // proves live even when the class passes through.
                                g_npcWeaponParts.fetch_add(1, std::memory_order_relaxed);

                                const auto idx = static_cast<std::size_t>(*cls);
                                // Defensive bounds check: ClassOfWeaponForm never
                                // yields kTotal today (ClassFromAnimType/
                                // ClassForAmmo only return real classes), but this
                                // indexes the resolved array directly - no
                                // session lock or resolve like the player path's
                                // WeaponDisplayFor - so trust the invariant only
                                // after checking it, not forever.
                                if (idx < kWeaponClassCount) {
                                    const auto hand = HandForBipedSlot(
                                        *cls, static_cast<std::uint32_t>(a_slot));
                                    const auto handIdx =
                                        static_cast<std::size_t>(hand);
                                    const auto& weapon =
                                        lk.entry->weapons[idx][handIdx];
                                    if (weapon.kind == SlotEntry::Kind::kHide) {
                                        // Inert in v1, same engine facts as the
                                        // player path (NoteHideInert).
                                        NoteHideInert(*cls);
                                    } else if (weapon.kind == SlotEntry::Kind::kStyle && weapon.form &&
                                               ClassOfWeaponForm(weapon.form) == cls) {
                                        // Belt-and-suspenders class match, same
                                        // reasoning as the player path. NPCs have
                                        // no 1st-person biped, so ALWAYS the
                                        // style's base model here - never
                                        // PickModel/WNAM (that branch is player-
                                        // biped-only by construction: WNAM only
                                        // ever stages when the biped being staged
                                        // is the PLAYER's, see PickModel above).
                                        if (auto* const styled = BaseModelOf(weapon.form)) {
                                            model = styled;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (...) {
                model = a_part;  // any throw degrades this part to pure vanilla
                try {
                    spdlog::error("WeaponHooks: part thunk threw; degraded to vanilla for this part.");
                } catch (...) {
                }
            }

            return (*Orig)(model, a_slot, a_refr, a_biped, a_root);
        }

        // One site: byte-check, then displace. The E8 check is not merely a
        // "did the offset drift" guard - write_call<5> derives the original
        // target by decoding the displaced rel32, so on a NON-branch it would
        // hand back a garbage "original" and we would call into the middle of
        // nowhere (the write_branch pitfall). Verifying E8 first is what makes
        // the returned pointer meaningful.
        bool InstallSite(std::uintptr_t a_address, const char* a_what, LoadPart_t& a_orig,
                         LoadPart_t a_thunk) {
            const auto op = *reinterpret_cast<std::uint8_t*>(a_address);
            if (op != 0xE8) {
                spdlog::error("WeaponHooks: expected E8 at the part-3D call site {}, found {:02X}; "
                              "that site NOT installed (armor transmog is unaffected).",
                              a_what, op);
                return false;
            }
            a_orig = reinterpret_cast<LoadPart_t>(
                SKSE::GetTrampoline().write_call<5>(a_address, a_thunk));
            return true;
        }
    }

    void WeaponHooks::Install() {
        // The loader's weapon and quiver callers. Byte-verified on both
        // runtimes against the real binaries (2026-07-17): every site below is
        // an E8 rel32 whose computed target is the loader, and both containing
        // functions have live callers on AE (15683 <- 19769; 15688 <- 15678 /
        // 19773 / 36991), so neither is a dead inlined-away original - the
        // specific trap that made an earlier AE release ship a hook that
        // resolved, byte-checked and never fired.
        //
        // The two weapon sites are the two arms of ONE if/else on the actor,
        // not two independent call paths (wpn_batch3_family.c:68 `if (refr ==
        // PlayerCharacter singleton)` -> +0x17F, :79 `else` -> +0x1D0). Both
        // install the SAME thunk template unchanged - the gate that decides
        // player vs. NPC lives inside LoadPartThunk, not per-site, so un-gating
        // the NPC arm (Task 4) needed no change here at all.
        //
        // Deliberately NOT hooked, pass-through by omission rather than by a
        // runtime check: the torch stager's two sites (SE 15514 / AE 15691) -
        // a torch has no styleable class - and the AnimObject site (SE 42420 /
        // AE 43576), which passes slot -1 and so has no biped slot at all.
        //
        // NOTE for future work: the loader's own Address Library ID is NOT
        // stable across runtimes. SE 15526 corresponds to AE 15703; AE 15526 is
        // an unrelated function 0x95F0 away. Nothing here resolves the loader
        // by ID - we hook its CALLERS and take the original from write_call's
        // return - but anyone adding a direct RelocationID for it must write
        // RelocationID(15526, 15703) or silently hook the wrong code on AE.
        // ⚠ OFFSETS COME FROM VersionCheck, NOT FROM CONSTANTS HERE. On any
        // build but the two measured by hand these are somewhere else. The two
        // weapon sites are the awkward case: 15506/15683 calls the loader
        // TWICE, for the player and for an NPC, and nothing tells the calls
        // apart - so they are located by ORDINAL (player first, NPC second,
        // verified in that order on both binaries). The quiver goes through a
        // different parent, so its call is unique.
        const auto playerOff = VersionCheck::WeaponPlayerCallOffset();
        const auto npcOff    = VersionCheck::WeaponNpcCallOffset();
        const auto quiverOff = VersionCheck::QuiverCallOffset();
        const REL::Relocation<std::uintptr_t> weaponPlayer{ REL::RelocationID(15506, 15683),
                                                            playerOff };
        const REL::Relocation<std::uintptr_t> weaponNPC{ REL::RelocationID(15506, 15683),
                                                         npcOff };
        const REL::Relocation<std::uintptr_t> quiver{ REL::RelocationID(15511, 15688), quiverOff };

        weaponPlayerOk_ = playerOff != 0 &&
                          InstallSite(weaponPlayer.address(), "weapon/player (located)",
                                      g_origWeaponPlayer, &LoadPartThunk<&g_origWeaponPlayer>);
        weaponNPCOk_    = npcOff != 0 &&
                          InstallSite(weaponNPC.address(), "weapon/NPC (located)",
                                      g_origWeaponNPC, &LoadPartThunk<&g_origWeaponNPC>);
        quiverOk_       = quiverOff != 0 &&
                          InstallSite(quiver.address(), "quiver (located)",
                                      g_origQuiver, &LoadPartThunk<&g_origQuiver>);

        spdlog::info("WeaponHooks: part-3D call-site hooks installed (weapon/player={}, "
                     "weapon/NPC={}, quiver={}); register-free thunks.",
                     weaponPlayerOk_, weaponNPCOk_, quiverOk_);
    }

    bool WeaponHooks::AllInstalled() { return weaponPlayerOk_ && weaponNPCOk_ && quiverOk_; }

    std::uint64_t WeaponHooks::PlayerWeaponPartCount() {
        return g_playerWeaponParts.load(std::memory_order_relaxed);
    }

    std::uint64_t WeaponHooks::NpcWeaponPartCount() {
        return g_npcWeaponParts.load(std::memory_order_relaxed);
    }

}  // namespace OS
