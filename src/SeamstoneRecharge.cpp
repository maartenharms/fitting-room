#include "SeamstoneRecharge.h"

#include <array>

#include "DyeUnlocks.h"
#include "HookSite.h"  // brings Windows.h and the GetObject #undef with it
#include "LoreModule.h"
#include "SeamstoneCharge.h"
#include "Settings.h"

namespace OS::SeamstoneRecharge {

    namespace {

        // The recharge apply, "ItemCardListCallback". Every hook below is a
        // call INSIDE this one function, so nothing outside the recharge menu
        // can reach any of them.
        constexpr std::uint64_t kApplyAE = 51859;

        // The three callees, in the order the apply calls them.
        constexpr std::uint64_t kMaxChargeAE = 15992;  // float(entry)
        constexpr std::uint64_t kPercentAE   = 51895;  // float(entry, bool)
        constexpr std::uint64_t kSetChargeAE = 16021;  // void(entry, float, void*, void*)

        using MaxCharge_t = float (*)(RE::InventoryEntryData*);
        using Percent_t   = float (*)(RE::InventoryEntryData*, bool);
        using SetCharge_t = void (*)(RE::InventoryEntryData*, float, void*, void*);

        MaxCharge_t g_origMax{};
        Percent_t   g_origPct{};
        SetCharge_t g_origSet{};
        bool        g_installed = false;

        // Is this entry the Seamstone? Everything here answers for our stone
        // and defers for every other item in the game, so this is the one test
        // that decides whether the engine keeps its own behaviour.
        [[nodiscard]] bool IsStone(RE::InventoryEntryData* a_entry) {
            if (!a_entry) {
                return false;
            }
            auto* stone = LoreModule::Stone();
            return stone && a_entry->GetObject() == stone;
        }

        [[nodiscard]] float Capacity() {
            return static_cast<float>(Settings::GetSingleton().seamstoneCapacity);
        }

        // ⚠ THE CAPACITY, SO THE CLAMP PASSES. Without this the apply computes
        // a new charge, finds it larger than a max of 0, and writes 0 - having
        // already eaten the gem.
        float MaxChargeHook(RE::InventoryEntryData* a_entry) {
            if (IsStone(a_entry)) {
                return Capacity();
            }
            return g_origMax ? g_origMax(a_entry) : 0.0f;
        }

        // ⚠ A PERCENTAGE, BECAUSE THAT IS WHAT THE ENGINE MULTIPLIES BACK OUT.
        // The apply does `percent * max * 0.01` to recover the absolute charge,
        // so answering 0 here would make every gem overwrite the stone instead
        // of topping it up, and the second gem you spent would be worth less
        // than the first.
        float PercentHook(RE::InventoryEntryData* a_entry, bool a_arg) {
            if (IsStone(a_entry)) {
                const auto cap = Settings::GetSingleton().seamstoneCapacity;
                return SeamstoneCharge::Fraction(DyeUnlocks::CurrentCharge(), cap) * 100.0f;
            }
            return g_origPct ? g_origPct(a_entry, a_arg) : 0.0f;
        }

        // ⚠ THE ORIGINAL IS NOT CALLED FOR THE STONE, AND THAT IS THE POINT.
        // Its first act is an `__RTDynamicCast` to an enchantable form, which a
        // MISC fails, so it would return having done nothing. Ours is the write.
        void SetChargeHook(RE::InventoryEntryData* a_entry, float a_charge, void* a_c, void* a_d) {
            if (!IsStone(a_entry)) {
                if (g_origSet) {
                    g_origSet(a_entry, a_charge, a_c, a_d);
                }
                return;
            }
            try {
                const auto& cfg  = Settings::GetSingleton();
                const auto  held = DyeUnlocks::CurrentCharge();
                // ⚠ ONLY A RISE COUNTS, AND IT IS APPLIED AS A GAIN RATHER THAN
                // AS AN ABSOLUTE. Writing a_charge straight in would look
                // simpler and would silently drain a save: a stone that is over
                // the cap because the player LOWERED iSeamstoneCapacity comes
                // back from the engine clamped down to the new ceiling, and
                // AddCharge exists precisely to refuse that rather than cut the
                // stone down. Taking the difference and going through AddCharge
                // keeps the one tested path in charge of the arithmetic.
                const auto asked = a_charge <= 0.0f
                                       ? 0u
                                       : static_cast<std::uint32_t>(a_charge);
                if (asked <= held) {
                    spdlog::warn("Seamstone recharge: the engine offered {} against a held {}, "
                                 "which is not a rise, so nothing was credited and the gem is "
                                 "already gone.",
                                 asked, held);
                    return;
                }
                const auto    gained = asked - held;
                std::uint32_t applied = 0;
                DyeUnlocks::With([&](DyeUnlockSet& a_set) {
                    applied = a_set.AddCharge(gained, cfg.seamstoneCapacity);
                });
                spdlog::info("Seamstone recharge: the vanilla soul-gem recharge added {}, of "
                             "which {} fit. Charge is now {} of {}.",
                             gained, applied, DyeUnlocks::CurrentCharge(), cfg.seamstoneCapacity);
            } catch (...) {
                spdlog::error("Seamstone recharge: threw while crediting; the gem is gone and "
                              "the charge did not move.");
            }
        }

        // One site: find it by what it calls, chain below whoever is already
        // there, and hand back whether it went in.
        template <typename Fn>
        [[nodiscard]] bool HookOne(std::uintptr_t a_caller, std::uint64_t a_calleeId,
                                   const char* a_name, Fn a_hook, Fn& a_orig) {
            const REL::Relocation<std::uintptr_t> callee{ REL::ID(a_calleeId) };
            const auto offset = HookSite::FindLoneCallTo(a_caller, callee.address(),
                                                         "SeamstoneRecharge");
            if (offset == 0) {
                // ⚠ NO VERIFIED-OFFSET FALLBACK HERE, unlike ItemCardCharge.
                // That one falls back because a missing item-card stamp costs a
                // line of text. This one decides whether a soul gem is eaten
                // for nothing, so a site we cannot positively identify is a
                // site we refuse, and Install() then arms none of it.
                spdlog::error("SeamstoneRecharge: no unique call to {} (AE {}) inside the "
                              "recharge apply; refusing.",
                              a_name, a_calleeId);
                return false;
            }
            const auto site = a_caller + offset;
            if (*reinterpret_cast<std::uint8_t*>(site) != 0xE8) {
                spdlog::error("SeamstoneRecharge: no E8 at the {} site (+0x{:X}); refusing.",
                              a_name, offset);
                return false;
            }
            // ⚠ write_call, NOT write_branch. write_branch<5> does not relocate
            // stolen bytes; on anything that is not already a rel32 branch the
            // "original" it returns is garbage that crashes later, somewhere
            // else. This is a real E8, which is what write_call is for.
            a_orig = reinterpret_cast<Fn>(SKSE::GetTrampoline().write_call<5>(site, a_hook));
            spdlog::info("SeamstoneRecharge: hooked {} at +0x{:X}, original 0x{:X} in {}.", a_name,
                         offset, reinterpret_cast<std::uintptr_t>(a_orig),
                         HookSite::OwningModule(reinterpret_cast<std::uintptr_t>(a_orig)));
            return true;
        }

    }  // namespace

    bool Installed() { return g_installed; }

    void Install() {
        if (!Settings::GetSingleton().seamstoneCharging) {
            return;
        }
        if (REL::Module::IsSE() || REL::Module::IsVR()) {
            spdlog::warn("SeamstoneRecharge: AE only (the SE call sites have not been "
                         "measured); not installed.");
            return;
        }
        if (Settings::GetSingleton().seamstoneCapacity == 0) {
            spdlog::warn("SeamstoneRecharge: iSeamstoneCapacity is 0, so there is nowhere for a "
                         "soul to go; not installed.");
            return;
        }

        const REL::Relocation<std::uintptr_t> apply{ REL::ID(kApplyAE) };
        spdlog::info("SeamstoneRecharge: recharge apply (AE {}) resolves to 0x{:X} in {}.",
                     kApplyAE, apply.address(), HookSite::OwningModule(apply.address()));

        // ⚠ ALL THREE OR NONE, AND THE ORDER OF THESE && IS LOAD BEARING ONLY
        // IN THAT IT SHORT-CIRCUITS. A half-installed recharge is the same
        // destructive state as no recharge at all: the T prompt appears because
        // the card carries a charge field, the gem is eaten by the engine, and
        // whichever hook is missing decides whether anything is credited.
        const bool ok = HookOne(apply.address(), kMaxChargeAE, "GetMaxCharge",
                                &MaxChargeHook, g_origMax) &&
                        HookOne(apply.address(), kPercentAE, "GetChargePercent",
                                &PercentHook, g_origPct) &&
                        HookOne(apply.address(), kSetChargeAE, "SetCharge",
                                &SetChargeHook, g_origSet);
        if (!ok) {
            // ⚠ A PARTIAL INSTALL IS NOT LEFT STANDING. write_call cannot be
            // undone here, so the hooks that DID go in stay - but they are all
            // no-ops for anything that is not the Seamstone, and with
            // g_installed false ItemCardCharge refuses to stamp, so no charge
            // field reaches the card, so no T prompt appears, so nothing can be
            // eaten. The fail-safe is the stamp, not the hooks.
            spdlog::error("SeamstoneRecharge: NOT armed. The item card will carry no charge "
                          "either, so the Charge prompt cannot appear and no soul gem can be "
                          "spent on the stone.");
            return;
        }
        g_installed = true;
        spdlog::info("SeamstoneRecharge: armed. Skyrim's own Charge prompt will now fill the "
                     "Seamstone, up to {}.",
                     Settings::GetSingleton().seamstoneCapacity);
    }

}  // namespace OS::SeamstoneRecharge
