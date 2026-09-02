#include "SoulGems.h"

#include <algorithm>
#include <array>

namespace OS::SoulGems {

    namespace {

        // Reusable artifacts, excluded outright. Eating one of these to paint a
        // pauldron destroys a Daedric quest reward that can never be replaced,
        // and the gem is the point of the quest rather than loot from it.
        //
        // ⚠ Full FormIDs, compared directly. Both live in Skyrim.esm, which is
        // always load index 0x00, so the constants are stable. Deliberately NOT
        // GetLocalFormID, which dereferences GetFile(0) with no null check.
        constexpr RE::FormID kAzurasStar = 0x00063B27;
        constexpr RE::FormID kBlackStar  = 0x00063B29;

        // ⚠ THE TWO ENUMS ARE THE SAME NUMBERS AND THIS IS WHERE THAT IS
        // CHECKED. SeamstoneCharge::SoulSize is engine-free on purpose, so it
        // cannot simply alias RE::SOUL_LEVEL, and a cast between them is only
        // safe while the orders agree. If the engine ever renumbers, this fails
        // to compile instead of quietly paying Grand rates for petty souls.
        static_assert(static_cast<int>(RE::SOUL_LEVEL::kNone) ==
                      static_cast<int>(SeamstoneCharge::SoulSize::kNone));
        static_assert(static_cast<int>(RE::SOUL_LEVEL::kPetty) ==
                      static_cast<int>(SeamstoneCharge::SoulSize::kPetty));
        static_assert(static_cast<int>(RE::SOUL_LEVEL::kLesser) ==
                      static_cast<int>(SeamstoneCharge::SoulSize::kLesser));
        static_assert(static_cast<int>(RE::SOUL_LEVEL::kCommon) ==
                      static_cast<int>(SeamstoneCharge::SoulSize::kCommon));
        static_assert(static_cast<int>(RE::SOUL_LEVEL::kGreater) ==
                      static_cast<int>(SeamstoneCharge::SoulSize::kGreater));
        static_assert(static_cast<int>(RE::SOUL_LEVEL::kGrand) ==
                      static_cast<int>(SeamstoneCharge::SoulSize::kGrand));

        [[nodiscard]] SeamstoneCharge::SoulSize Ours(RE::SOUL_LEVEL a_lvl) {
            // Anything past Grand is a value this build does not know. Treat it
            // as no soul rather than casting it into the enum: an out-of-range
            // SoulSize would reach ValueOf's switch, fall off the end and read
            // as free charge.
            const auto raw = static_cast<int>(a_lvl);
            if (raw <= static_cast<int>(RE::SOUL_LEVEL::kNone) ||
                raw > static_cast<int>(RE::SOUL_LEVEL::kGrand)) {
                return SeamstoneCharge::SoulSize::kNone;
            }
            return static_cast<SeamstoneCharge::SoulSize>(raw);
        }

        // A gem form we will never touch, whatever it holds.
        [[nodiscard]] bool Excluded(const RE::TESBoundObject* a_obj) {
            const auto id = a_obj->GetFormID();
            return id == kAzurasStar || id == kBlackStar;
        }

        // A quest item, recognised by the alias data the engine attaches rather
        // than by InventoryEntryData::IsQuestObject.
        //
        // ⚠ THAT IS ON PURPOSE. IsQuestObject is a relocated call
        // (RELOCATION_ID(15767, 16005)), and an id CommonLib ships is not proof
        // it is in the player's address database; calling an absent one CTDs.
        // The alias array is plain extra data with no lookup behind it, so it
        // costs nothing to be sure.
        [[nodiscard]] bool QuestHeld(const RE::ExtraDataList* a_list) {
            return a_list && a_list->HasType<RE::ExtraAliasInstanceArray>();
        }

        // Walk one inventory entry's stacks, calling a_fn(soulLevel, count,
        // extraList) for each. extraList is null for the plain remainder, the
        // items in the entry carrying no extra data of their own.
        //
        // The remainder takes the BASE form's contained soul, which is what a
        // pre-filled form like SoulGemGrandFilled carries, and the extra lists
        // take theirs from ExtraSoul, which is how a big gem holding a small
        // soul is stored. Both flavours are ordinary; a gem economy that
        // understood only one of them would be wrong for half the inventory.
        template <class F>
        void ForEachStack(RE::TESBoundObject* a_obj, std::int32_t a_total,
                          RE::InventoryEntryData* a_entry, F&& a_fn) {
            const auto baseSoul = a_obj->Is(RE::FormType::SoulGem)
                                      ? static_cast<RE::TESSoulGem*>(a_obj)->GetContainedSoul()
                                      : RE::SOUL_LEVEL::kNone;

            std::int32_t accountedFor = 0;
            if (a_entry && a_entry->extraLists) {
                for (auto* xList : *a_entry->extraLists) {
                    if (!xList) {
                        continue;
                    }
                    const auto n = xList->GetCount();
                    accountedFor += n;
                    if (n <= 0 || QuestHeld(xList)) {
                        continue;
                    }
                    // A list with no ExtraSoul still describes real items, and
                    // on a pre-filled form those items hold the base soul. Ask
                    // the list first and fall back, which is the order
                    // InventoryEntryData::GetSoulLevel itself uses.
                    const auto lvl = xList->GetSoulLevel() > RE::SOUL_LEVEL::kNone
                                         ? xList->GetSoulLevel()
                                         : baseSoul;
                    a_fn(lvl, static_cast<std::uint32_t>(n), xList);
                }
            }

            // ⚠ Clamped at zero. The extra lists can account for more than the
            // reported total in states this code does not control, and an
            // unsigned subtraction there would wrap into billions of imaginary
            // soul gems.
            const auto remainder = a_total - accountedFor;
            if (remainder > 0 && baseSoul > RE::SOUL_LEVEL::kNone) {
                a_fn(baseSoul, static_cast<std::uint32_t>(remainder),
                     static_cast<RE::ExtraDataList*>(nullptr));
            }
        }

    }  // namespace

    const char* Name(SeamstoneCharge::SoulSize a_size) {
        switch (a_size) {
            case SeamstoneCharge::SoulSize::kNone:
                return "Empty";
            case SeamstoneCharge::SoulSize::kPetty:
                return "Petty";
            case SeamstoneCharge::SoulSize::kLesser:
                return "Lesser";
            case SeamstoneCharge::SoulSize::kCommon:
                return "Common";
            case SeamstoneCharge::SoulSize::kGreater:
                return "Greater";
            case SeamstoneCharge::SoulSize::kGrand:
                return "Grand";
        }
        return "Empty";
    }

    std::vector<Held> Available() {
        std::vector<Held> out;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return out;
        }

        // One counter per size, so the walk cannot produce two rows for the
        // same soul out of two different gem forms.
        std::array<std::uint32_t,
                   static_cast<std::size_t>(SeamstoneCharge::SoulSize::kGrand) + 1>
            counts{};

        const auto inv = player->GetInventory([](RE::TESBoundObject& a_o) {
            return a_o.Is(RE::FormType::SoulGem);
        });
        for (const auto& [obj, data] : inv) {
            if (!obj || data.first <= 0 || Excluded(obj)) {
                continue;
            }
            ForEachStack(obj, data.first, data.second.get(),
                         [&](RE::SOUL_LEVEL a_lvl, std::uint32_t a_n,
                             RE::ExtraDataList*) {
                             const auto size = Ours(a_lvl);
                             if (size == SeamstoneCharge::SoulSize::kNone) {
                                 return;
                             }
                             counts[static_cast<std::size_t>(size)] += a_n;
                         });
        }

        // Biggest soul first: it is the order a player thinks in when deciding
        // what to burn, and it puts the wasteful choice at the top where the
        // "wastes N" note is read rather than buried under five petties.
        for (auto i = static_cast<int>(SeamstoneCharge::SoulSize::kGrand);
             i >= static_cast<int>(SeamstoneCharge::SoulSize::kPetty); --i) {
            const auto n = counts[static_cast<std::size_t>(i)];
            if (n > 0) {
                out.push_back({ static_cast<SeamstoneCharge::SoulSize>(i), n });
            }
        }
        return out;
    }

    bool ConsumeOne(SeamstoneCharge::SoulSize a_size) {
        if (a_size == SeamstoneCharge::SoulSize::kNone) {
            return false;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }

        const auto inv = player->GetInventory([](RE::TESBoundObject& a_o) {
            return a_o.Is(RE::FormType::SoulGem);
        });
        for (const auto& [obj, data] : inv) {
            if (!obj || data.first <= 0 || Excluded(obj)) {
                continue;
            }
            RE::ExtraDataList* target = nullptr;
            bool               found  = false;
            ForEachStack(obj, data.first, data.second.get(),
                         [&](RE::SOUL_LEVEL a_lvl, std::uint32_t, RE::ExtraDataList* a_list) {
                             if (!found && Ours(a_lvl) == a_size) {
                                 target = a_list;
                                 found  = true;
                             }
                         });
            if (!found) {
                continue;
            }
            // ⚠ THE EXTRA LIST IS PASSED THROUGH, and passing null instead
            // would be the bug this whole file is shaped around: the engine
            // would pick a stack of its own choosing out of the same form, and
            // on a form holding both filled and empty gems that is a coin flip
            // between destroying the soul we just paid for and destroying an
            // empty gem for free charge.
            //
            // moveToRef null: the gem is destroyed, not dropped. Vanilla
            // enchanting does the same, and a gem on the floor would let a
            // player refill from the one they just spent.
            player->RemoveItem(obj, 1, RE::ITEM_REMOVE_REASON::kRemove, target, nullptr);
            spdlog::info("Seamstone: consumed one {} soul gem.", Name(a_size));
            return true;
        }
        spdlog::warn("Seamstone: asked to consume a {} soul gem and found none.",
                     Name(a_size));
        return false;
    }

}  // namespace OS::SoulGems
