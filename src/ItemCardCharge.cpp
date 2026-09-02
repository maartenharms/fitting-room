#include "ItemCardCharge.h"

#include <array>
#include <utility>

#include "DyeUnlocks.h"
#include "HookSite.h"  // brings Windows.h and the GetObject #undef with it
#include "LoreModule.h"
#include "SeamstoneCharge.h"
#include "SeamstoneRecharge.h"
#include "Settings.h"

namespace OS::ItemCardCharge {

    namespace {

        using Populate_t = void (*)(RE::ItemCard*, RE::InventoryEntryData*, bool);

        // ⚠ AE IDS ONLY, AND THAT IS DELIBERATE FOR A SPIKE. The SE 1.5.97
        // twins have not been measured; the dev game is AE and this answers an
        // AE question. Install() refuses on anything else rather than guessing
        // an id, because a wrong id does not fail loudly: CommonLib's
        // id2offset does a lower_bound and a missing mid-range id silently
        // returns its NEIGHBOUR'S offset. Writing a call into a neighbouring
        // function is how you get a CTD with no clue in it.
        constexpr std::uint64_t kPopulateAE = 51897;

        // Every function that populates an item card, one per item menu
        // (inventory, barter, container, gift, magic, favourites).
        //
        // ⚠ THE OFFSET IS A HINT AND ONLY A HINT. It is where the call to the
        // callee sits in the 1.6.1170 binary, read out of the file with the
        // same E8-target scan this code runs, and verified there before it
        // shipped. It exists ONLY for the case where the primary anchor cannot
        // match because somebody else displaced the call first, and it is never
        // trusted without an E8 check.
        struct CallerSite {
            std::uint64_t id;
            std::size_t   hintOffset;
        };
        constexpr CallerSite kCallersAE[] = {
            { 50949, 0x80 }, { 51130, 0xB2 }, { 51218, 0x35 },
            { 51458, 0x87 }, { 51569, 0x7A }, { 51852, 0x7A },
        };
        constexpr std::size_t kSiteCount = std::size(kCallersAE);

        // ⚠ ONE ORIGINAL PER SITE, NOT ONE SHARED. While every site pointed at
        // the same callee a single pointer was right. Once another plugin may
        // have displaced SOME of them, each site's original is its own, and a
        // shared pointer would send the inventory menu's call into the barter
        // menu's hook chain. That is also why there are six thunks below: a
        // single hook function cannot tell which site called it.
        std::array<Populate_t, kSiteCount> g_orig{};
        bool                               g_installed = false;

        // The stamp itself, shared by every thunk.
        void Stamp(RE::ItemCard* a_card, RE::InventoryEntryData* a_entry) {
            try {
                if (!a_card || !a_entry) {
                    return;
                }
                auto* stone = LoreModule::Stone();
                if (!stone || a_entry->GetObject() != stone) {
                    return;
                }
                const auto&  cfg  = Settings::GetSingleton();
                const auto   held = DyeUnlocks::CurrentCharge();
                const double pct =
                    static_cast<double>(SeamstoneCharge::Fraction(held, cfg.seamstoneCapacity)) *
                    100.0;
                // The weapon path's own scale: current * 100 / max, the 100
                // being a float constant read out of the binary.
                a_card->obj.SetMember("charge", RE::GFxValue(pct));
                // What would be left after one use. Nothing is spent by looking
                // at the stone, so this equals charge; the weapon path puts the
                // post-swing figure here and the meter draws the gap between
                // them as the part about to go.
                a_card->obj.SetMember("usedCharge", RE::GFxValue(pct));

                // ⚠ THE FIELD ALONE WAS NOT ENOUGH FOR THE METER, though it WAS
                // enough for the button bar: with charge set and nothing else,
                // a MISC already offers Charge and opens the vanilla soul-gem
                // list. The card's layout comes from "type", which the populate
                // function writes from a per-form-type constant: Weapon and Ammo
                // 2, MISC 3, SoulGem 12 (the full table is on itemCardType in
                // Settings.h). Only the two arms that write 2 write a charge, so
                // 2 is the only value that can draw a meter for us.
                // ⚠ THE SWEEP IS THE ONLY WAY TO ASK THE .swf A QUESTION, and
                // it costs one session rather than one restart per value. -2
                // walks 0..kSweepMax on each rebuild of the stone's card, so
                // scrolling off the stone and back onto it steps to the next
                // layout, and the log line below says which number drew what.
                constexpr std::int32_t kSweepMax = 16;
                std::int32_t           type      = cfg.itemCardType;
                static std::int32_t    sweep     = 0;
                if (type == -2) {
                    type  = sweep;
                    sweep = sweep >= kSweepMax ? 0 : sweep + 1;
                }
                if (type >= 0) {
                    a_card->obj.SetMember("type", RE::GFxValue(static_cast<double>(type)));
                }

                // ⚠ AND THE REST OF THE WEAPON SHAPE, BECAUSE THE FIELDS ABOVE
                // MEASURABLY ARE NOT ENOUGH. A stone carrying type, charge and
                // usedCharge draws a DAMAGE row and no meter, while an enchanted
                // weapon on the same card draws a meter and its effect text. The
                // difference the engine makes between them is everything below,
                // so the dial walks up to "everything a real enchanted weapon
                // gets" and a card that still draws nothing there is a card that
                // never will.
                //
                // ⚠ "effects" IS A STRING, NOT AN ARRAY, which is worth stating
                // because the name reads like a list. The populate function
                // writes the card's own infoText into it (kString), which is the
                // "50% chance for each element..." line under an enchanted
                // weapon's meter.
                if (cfg.itemCardShape >= 1) {
                    a_card->obj.SetMember(
                        "effects",
                        RE::GFxValue("Holds the charge that styles your gear. "
                                     "Refill it with filled soul gems."));
                }
                if (cfg.itemCardShape >= 2) {
                    // Zero rather than absent. The DAMAGE row is drawn by the
                    // weapon layout whatever we do, so the only choice here is
                    // between a blank label and a number, and a blank one reads
                    // as a bug in the card rather than as a stone that does no
                    // damage.
                    a_card->obj.SetMember("damage", RE::GFxValue(0.0));
                    a_card->obj.SetMember("damageChange", RE::GFxValue(0.0));
                    a_card->obj.SetMember("poisoned", RE::GFxValue(false));
                }

                // ⚠ LOGGED ON CHANGE, NOT ONCE. A one-shot line answered
                // "did the stamp run" and then went quiet, which is the wrong
                // half: the live question is whether the charge MOVES when a
                // soul gem is used, and a single line at 0% cannot show that.
                //
                // ⚠ THE SHAPE IS IN THE LINE for the same reason: the INI can
                // change it between runs, so a report about what the card looked
                // like is worthless without knowing what was written to it.
                //
                // ⚠ AND THE SWEEP LOGS EVERY TIME, not only on a change, since
                // the whole point of it is that the number moves while nothing
                // else does. Reading it back afterwards is how a screenshot gets
                // matched to the value that produced it.
                static std::uint32_t lastLogged = ~0u;
                static std::int32_t  lastShape  = -1;
                static std::int32_t  lastType   = INT32_MIN;
                if (cfg.itemCardType == -2 || held != lastLogged ||
                    cfg.itemCardShape != lastShape || type != lastType) {
                    lastLogged = held;
                    lastShape  = cfg.itemCardShape;
                    lastType   = type;
                    spdlog::info("ItemCardCharge: stamped charge={:.1f}% ({} of {}), type={}, "
                                 "shape={}.",
                                 pct, held, cfg.seamstoneCapacity, type, cfg.itemCardShape);
                }
            } catch (...) {
                // A UI nicety must never take the menu down with it.
            }
        }

        // ⚠ Original FIRST, always and unconditionally. Everything we do is a
        // re-stamp on what it produced, and skipping it for any reason would
        // blank the card rather than merely leave our field off.
        template <std::size_t I>
        void Thunk(RE::ItemCard* a_card, RE::InventoryEntryData* a_entry, bool a_flag) {
            if (g_orig[I]) {
                g_orig[I](a_card, a_entry, a_flag);
            }
            Stamp(a_card, a_entry);
        }

        template <std::size_t... I>
        constexpr auto MakeThunks(std::index_sequence<I...>) {
            return std::array<Populate_t, sizeof...(I)>{ &Thunk<I>... };
        }
        constexpr auto kThunks = MakeThunks(std::make_index_sequence<kSiteCount>{});

    }  // namespace

    bool Installed() { return g_installed; }

    void Install() {
        if (!Settings::GetSingleton().seamstoneCharging) {
            return;
        }
        if (REL::Module::IsSE() || REL::Module::IsVR()) {
            spdlog::warn("ItemCardCharge: spike is AE-only (the SE call sites have not been "
                         "measured); not installed.");
            return;
        }
        // ⚠ THE STAMP IS THE FAIL-SAFE FOR THE WHOLE FEATURE, WHICH IS WHY IT
        // CHECKS SOMEBODY ELSE'S STATE. Writing a charge onto the card is what
        // makes Skyrim offer its Charge prompt on the stone, and the engine's
        // own apply then eats a soul gem whatever happens next. With the
        // recharge hooks unarmed, that gem buys nothing - which is exactly the
        // "I can eat up soul gems but the charge never goes up" report. So the
        // card carries no charge unless the thing that credits it is in place.
        if (!SeamstoneRecharge::Installed()) {
            spdlog::error("ItemCardCharge: the recharge hooks are not armed, so stamping a "
                          "charge would offer a Charge prompt that eats soul gems for nothing. "
                          "Not installed.");
            return;
        }

        const REL::Relocation<std::uintptr_t> callee{ REL::ID(kPopulateAE) };
        spdlog::info("ItemCardCharge: populate callee (AE {}) resolves to 0x{:X} in {}.",
                     kPopulateAE, callee.address(),
                     HookSite::OwningModule(callee.address()));

        std::size_t installed = 0;
        std::size_t chained   = 0;
        for (std::size_t n = 0; n < kSiteCount; ++n) {
            const auto&                           c = kCallersAE[n];
            const REL::Relocation<std::uintptr_t> caller{ REL::ID(c.id) };
            auto offset = HookSite::FindLoneCallTo(caller.address(), callee.address(),
                                                   "ItemCardCharge");
            const bool byAnchor = offset != 0;
            if (!byAnchor) {
                // ⚠ NOT AN ERROR ON ITS OWN. Nothing here points at the callee
                // any more, which on a heavy load order usually means another
                // plugin displaced this very call before us. Inventory
                // Interface Information Injector and moreHUD Inventory Edition
                // both exist to put their own data on item cards, and both are
                // common. Fall back to the verified offset, prove what is
                // there, and say whose it is.
                offset = c.hintOffset;
            }
            const auto site = caller.address() + offset;
            if (*reinterpret_cast<std::uint8_t*>(site) != 0xE8) {
                spdlog::error("ItemCardCharge: no E8 at AE {}+0x{:X} (found {:02X}); skipped.",
                              c.id, offset, *reinterpret_cast<std::uint8_t*>(site));
                continue;
            }
            if (!byAnchor) {
                std::int32_t rel = 0;
                std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), sizeof(rel));
                const auto current = site + 5 + static_cast<std::intptr_t>(rel);
                spdlog::warn("ItemCardCharge: AE {}+0x{:X} already targets 0x{:X} in {}, not the "
                             "callee. Chaining BELOW it.",
                             c.id, offset, current, HookSite::OwningModule(current));
                ++chained;
            }
            // ⚠ CHAINING IS SAFE HERE AND THE ORDER IS THE POINT. write_call
            // hands back whatever the site currently targets, which is the
            // other plugin's hook, and we call that as the original. Their work
            // happens first and our stamp lands last, which is what we want:
            // the final writer decides what the card says. This is a clean
            // displaced call carrying the callee's own ABI, so none of the
            // stub-captured-register hazards of chaining apply.
            //
            // ⚠ write_call, NOT write_branch. write_branch<5> does not relocate
            // stolen bytes: on a normal prologue the "original" it hands back
            // is computed from whatever the first five bytes happen to be, and
            // calling that garbage crashes later, somewhere else. This is a
            // real E8, which is exactly what write_call is for.
            g_orig[n] = reinterpret_cast<Populate_t>(
                SKSE::GetTrampoline().write_call<5>(site, kThunks[n]));
            ++installed;
            spdlog::info("ItemCardCharge: hooked AE {}+0x{:X} ({}), original 0x{:X} in {}.", c.id,
                         offset, byAnchor ? "by callee anchor" : "chained",
                         reinterpret_cast<std::uintptr_t>(g_orig[n]),
                         HookSite::OwningModule(reinterpret_cast<std::uintptr_t>(g_orig[n])));
        }

        g_installed = installed > 0;
        if (g_installed) {
            spdlog::info("ItemCardCharge: SPIKE ACTIVE on {} of {} item menus, {} of them "
                         "chained below another plugin. Open your inventory and look at the "
                         "Seamstone.",
                         installed, kSiteCount, chained);
        } else {
            spdlog::error("ItemCardCharge: no call site could be hooked; spike NOT active.");
        }
    }

}  // namespace OS::ItemCardCharge
