#include "HeadPart.h"

#include "BipedPost.h"           // RestoreHeadPartPartitions: round eighteen's parts[131-]
#include "HairColor.h"
#include "HeadPartPlan.h"
#include "OutfitDye.h"           // QueueRepaint: a head dye lives on geometry the swap replaces
#include "PreviewFilter.h"       // the scalp rule the scene resolution applies
#include "PreviewGrid.h"         // FoldPath, the subsystem's one fold
#include "PreviewSwapCapture.h"  // the TNAM capture (OS-192)
#include "StyleRef.h"            // Make, for the persistable half of a capture
#include "VersionCheck.h"        // IdOk: membership, not "REL gave me an address"

#include <algorithm>
#include <chrono>  // the mesh check's own budget and the ms it reports
#include <map>
#include <mutex>
#include <unordered_map>  // the extra-owner index (the extra flag is a lie)
#include <unordered_set>

namespace OS::HeadPart {

    namespace {

        // Per actor AND slot: the part they had before Fitting Room first
        // changed it. Keyed on both, because a character can have their hair,
        // eyes and brows each captured independently and a map keyed on the
        // actor alone would have the third Apply overwrite the first's
        // original. Locked for the reason HairColor's state map is: the editor
        // touches it from the FUCK present thread while refreshes and loads
        // arrive on the game thread, and a rehash under a concurrent reader is
        // the shape of a CTD this project has already paid for once.
        //
        // ⚠ THE SLOT NUMBER IS THE KEY, NOT Kind, since the slot is what the
        // engine itself keys a head part on and a discovered slot has no Kind
        // to be. The four shipped kinds reach this through SlotOf, so nothing
        // about their behaviour moved.
        std::mutex                                              g_lock;
        std::map<std::pair<RE::FormID, Slot>, RE::BGSHeadPart*> g_original;

        // How many times we have written each actor's part of each slot, under
        // the same lock and keyed the same way.
        //
        // ⚠ A SEPARATE MAP RATHER THAN A FIELD BESIDE THE CAPTURE, because the
        // capture is ERASED by a restore and the count must not be. The one
        // question it answers - "did Fitting Room move this while the character
        // editor was open" - is asked across a window in which a restore is the
        // most likely thing to have happened, so a counter that a restore reset
        // would report zero on precisely the case it exists for.
        std::map<std::pair<RE::FormID, Slot>, std::uint32_t>    g_writes;

        // Both writers go through here, so a write can never land without being
        // counted. Called with g_lock NOT held.
        void CountWrite(RE::FormID a_actor, Slot a_slot) {
            std::scoped_lock l(g_lock);
            ++g_writes[{ a_actor, a_slot }];
        }

        RE::TESNPC* BaseOf(RE::Actor* a_actor) {
            return a_actor ? a_actor->GetActorBase() : nullptr;
        }

        // ⚠⚠ THE ARMOUR RACE COUNTS AS THIS ACTOR, and StyleCatalog's own note
        // on EvaluateFitFor is the long version: TESRace::armorParentRace is
        // the CK's Armor Race (RNAM), a custom race points it at a vanilla one
        // precisely so ordinary content works on that character, and almost no
        // content mod lists the custom race itself. It was worth four styles in
        // ten there; here it is worth the whole hair library, because a hair's
        // validRaces names the vanilla playable races and nothing else.
        //
        // ⚠ ONE LEVEL, NOT A WALK, with the same identity guard: vanilla races
        // point RNAM at DefaultRace and DefaultRace's own RNAM is itself, so a
        // walk would spin and the second HasForm would be a repeat.
        [[nodiscard]] bool ListNamesActor(RE::BGSListForm* a_list, RE::TESRace* a_race) {
            if (!a_list || !a_race) {
                return false;
            }
            if (a_list->HasForm(a_race)) {
                return true;
            }
            auto* const armourRace = a_race->armorParentRace;
            return armourRace && armourRace != a_race && a_list->HasForm(armourRace);
        }

        // Is there ANY part of this type, anywhere in the load order, whose
        // race list names this actor? While the answer is yes the filter has
        // something to filter on and nothing here changes. When it is no, every
        // list in the game is silent about this character and the rule can only
        // return an empty page, which is what the field reported: 0 hair of
        // 2485 for a follower on her own mod's race (user 2026-08-19).
        //
        // ⚠ EARLY OUT ON THE FIRST HIT, which is what makes this free for the
        // ordinary case. A Nord is named by the first hair the walk reaches, so
        // the loop ends immediately; only a race nothing names pays the full
        // walk, and it pays it once.
        //
        // ⚠ CACHED PER RACE AND TYPE, NOT PER RACE. A race can be named by the
        // hair lists of the mod that invented it and by no brow list anywhere,
        // and a per-race verdict would leave the brows page empty with the hair
        // page full. The head-part form array is fixed after load, so the
        // answer cannot go stale. Unsynchronised like ModelResolves' cache
        // below, and read from the same places: this file's judges run on the
        // frame the editor draws.
        [[nodiscard]] bool RaceListsSayNothing(RE::BGSHeadPart::HeadPartType a_wanted,
                                               RE::TESRace* a_race) {
            if (!a_race) {
                return false;  // no race to be silent about
            }
            static std::map<std::pair<std::uint32_t, RE::FormID>, bool> cache;
            const auto key = std::make_pair(static_cast<std::uint32_t>(a_wanted),
                                            a_race->GetFormID());
            if (const auto it = cache.find(key); it != cache.end()) {
                return it->second;
            }
            bool        silent = true;
            auto* const dh     = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return false;  // cannot tell, so leave the filter as it was
            }
            for (auto* part : dh->GetFormArray<RE::BGSHeadPart>()) {
                if (!part || part->type != a_wanted || !part->validRaces) {
                    continue;
                }
                if (ListNamesActor(part->validRaces, a_race)) {
                    silent = false;
                    break;
                }
            }
            cache[key] = silent;
            if (silent) {
                spdlog::info("HeadPart: no {} part in the load order names race {:08X} or its "
                             "armour race, so the race filter is off for it.",
                             static_cast<std::uint32_t>(a_wanted), a_race->GetFormID());
            }
            return silent;
        }

        // Everything Judge needs about one record, read off the form.
        //
        // ⚠ ONE BUILDER FOR BOTH CALLERS, AND THAT IS THE POINT. AvailableFor
        // decides what the browser OFFERS and IsValidFor decides what the push
        // is allowed to WEAR, and those two must be the same question or the
        // editor ends up wearing a part it would not let you pick. Two copies
        // of these seven lines is exactly how they would drift apart - and that
        // is why the race fallback above is read HERE rather than at the two
        // list-building call sites, where the push would never have seen it.
        HeadPartPlan::Candidate CandidateFor(RE::BGSHeadPart* a_part,
                                             RE::BGSHeadPart::HeadPartType a_wanted,
                                             RE::TESRace* a_race) {
            using Flag = RE::BGSHeadPart::Flag;
            HeadPartPlan::Candidate c;
            c.typeMatches = a_part->type == a_wanted;
            c.playable    = a_part->flags.all(Flag::kPlayable);
            c.extraPart   = a_part->IsExtraPart();
            c.flagMale    = a_part->flags.all(Flag::kMale);
            c.flagFemale  = a_part->flags.all(Flag::kFemale);
            c.hasRaceList = a_part->validRaces != nullptr;
            c.raceListed  = c.hasRaceList && ListNamesActor(a_part->validRaces, a_race);
            // ⚠ ASKED ONLY WHEN IT COULD CHANGE THE ANSWER. The walk behind it
            // is cached, but the cache lookup is still a map probe per record
            // over thousands of records, and a part that already passed on its
            // own list has no use for a fallback.
            c.raceListsSayNothing =
                c.hasRaceList && !c.raceListed && RaceListsSayNothing(a_wanted, a_race);
            return c;
        }

        RE::BGSHeadPart::HeadPartType EngineType(Kind a_kind) {
            using T = RE::BGSHeadPart::HeadPartType;
            switch (a_kind) {
                case Kind::kHair:       return T::kHair;
                case Kind::kEyes:       return T::kEyes;
                case Kind::kBrows:      return T::kEyebrows;
                case Kind::kFacialHair: return T::kFacialHair;
            }
            return T::kHair;
        }

        // ⚠ THE PURE HEADER'S BOUNDARY AND THE ENGINE'S MUST AGREE, and this is
        // where that is enforced. HeadPartSlotPlan.h cannot say
        // RE::BGSHeadPart::HeadPartType::kTotal without pulling the engine into
        // a header that exists to stay out of it, so it names the number and
        // this asserts. A CommonLib that adds an eighth named type breaks the
        // build here rather than silently reclassifying that type as a mod's
        // invented slot.
        static_assert(
            static_cast<std::uint32_t>(RE::BGSHeadPart::HeadPartType::kTotal) ==
                HeadPartSlotPlan::kFirstCustomType,
            "the engine named a new head-part type; HeadPartSlotPlan::kFirstCustomType "
            "must follow it or that type will be treated as a discovered slot");

        // A label for a head part, in descending order of quality. This NEVER
        // returns empty, and that is the entire point of the last two arms.
        //
        // ⚠ An earlier version returned empty and the caller dropped the part.
        // That silently hid EVERY vanilla hair: Skyrim.esm is localized, so a
        // head part's FULL is a string-table index that resolves to an empty
        // string, and GetFormEditorID returns nothing on a runtime that does not
        // retain editor IDs. Modded parts ship non-localized plugins with real
        // FULL names, so the browser filled up with modded parts only and looked
        // like it was working. Measured: 26 vanilla parts qualify for a female
        // Nord and all 26 were being discarded.
        // a_slotLabel names the slot for the last resort only, so it is taken as
        // text rather than as a Kind: a discovered slot has no Kind, and the
        // caller already has its label in hand.
        std::string NameOf(RE::BGSHeadPart* a_part, std::string_view a_slotLabel) {
            if (!a_part) {
                return {};
            }
            if (const char* full = a_part->GetFullName(); full && *full) {
                return full;
            }
            if (const char* edid = a_part->GetFormEditorID(); edid && *edid) {
                return edid;
            }
            // The mesh's own file name, which for vanilla is exactly what the
            // player would recognise: "FemaleNord21" out of
            // Actors\Character\Character Assets\Hair\FemaleNord21.nif.
            if (const char* model = a_part->GetModel(); model && *model) {
                std::string_view path{ model };
                if (const auto slash = path.find_last_of("\\/");
                    slash != std::string_view::npos) {
                    path.remove_prefix(slash + 1);
                }
                if (const auto dot = path.find_last_of('.'); dot != std::string_view::npos) {
                    path = path.substr(0, dot);
                }
                if (!path.empty()) {
                    return std::string{ path };
                }
            }
            // Last resort. Unhelpful, but present and selectable, which beats a
            // part the user owns and cannot see.
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.*s %08X",
                          static_cast<int>(a_slotLabel.size()), a_slotLabel.data(),
                          a_part->GetFormID());
            return buf;
        }

        // The part's model plus each extra part's, ONE level, no recursion:
        // chargen nests one level, and a cycle in authored data must not
        // hang the list build. The vendored header carries extraParts at
        // 070; locate by member name on a CommonLib update.
        std::vector<std::string> ScenePathsOf(RE::BGSHeadPart* a_part) {
            std::vector<std::string> out;
            if (!a_part) {
                return out;
            }
            if (const char* m = a_part->GetModel(); m && *m) {
                out.emplace_back(m);
            }
            for (auto* extra : a_part->extraParts) {
                if (!extra) {
                    continue;
                }
                if (const char* m = extra->GetModel(); m && *m) {
                    // Generic scalp caps stay out of the scene: they are
                    // fitted to a head the card never draws and hang
                    // misaligned under the hair (field 2026-08-09). The
                    // per-hair hairline pieces pass the same test and stay.
                    if (PreviewFilter::IsScalpPath(PreviewGrid::FoldPath(m))) {
                        continue;
                    }
                    out.emplace_back(m);
                }
            }
            return out;
        }

        // The plugin that defines this part, for the browser's Plugin column.
        //
        // ⚠ GetFile(0) can be null, and this project has already paid for
        // assuming otherwise: GetLocalFormID derefs it with no check and CTDs on
        // any runtime-created form. Return empty rather than guessing, and never
        // let a missing file reach a formatter.
        std::string SourceOf(RE::BGSHeadPart* a_part) {
            if (!a_part) {
                return {};
            }
            auto* const file = a_part->GetFile(0);
            if (!file) {
                return {};
            }
            return std::string{ file->GetFilename() };
        }

        // ---- putting a changed part on screen -------------------------------
        //
        // The engine's own per-part swap on a LIVE face node,
        // `(Actor*, BGSHeadPart* old, BGSHeadPart* new)`. It is what the
        // character editor's hair, eyes and brows sliders drive: read off the
        // AE 1.6.1170 binary on 2026-08-19, RaceSexMenu's part handler writes
        // the base through TESNPC::ChangeHeadPart and then calls this on the
        // player with the part it took off and the part it put on. SE 26468 /
        // AE 27063, same shape on both builds (AE inlined the extra-parts
        // loops; SE keeps them as calls).
        //
        // What it does, per the decompile: detaches the old part's geometry and
        // each of its extra parts from the face node (matched by parent == the
        // face node and name == the part's editor name), then for the new part
        // and each of its extras builds the geometry through the facegen
        // manager, paints it (BSFaceGenManager::PrepareHeadPartForShaders, so
        // HeadBuildHook's detour fires exactly as it does on a full build), and
        // skins it (BSFaceGenNiNode::SkinSingleGeometry, at the ONE call site
        // FSMP patches on its 4.0 line and the entry it detours on its 2.5 line;
        // docs/re/fsmp-cooperative-hair.md). Nothing else on the actor is
        // touched: not the biped, not the other head parts, not the overlays.
        //
        // ⚠⚠ THIS REPLACED Actor::DoReset3D FOR OS-230, AND THE REASON IS
        // MEASURED. DoReset3D is a FULL 3D reset of the actor: every worn addon
        // and the whole head, plus everything RaceMenu re-applies on a head
        // build (sculpt, overlays, transforms, morphs) and everything FSMP
        // re-binds. On a fresh character that cost ~40 ms a switch; after a
        // Countenance preset was loaded it cost ~900 ms on the rig's own
        // character and three seconds in the field, all of it inside the
        // engine's build (23:12 log, 2026-08-18: `rebuild issued` to the head
        // build landing). RaceMenu's own slider never stalled because it never
        // resets: it calls this. The reason DoReset3D was chosen originally, that
        // AIProcess::UpdateEquipment will not touch head geometry without the
        // kHead flag it sets, does not apply here, since this route never goes
        // through UpdateEquipment at all.
        //
        // ⚠ CommonLib does not declare it. TESNPC::ChangeHeadPart (the base
        // write) is declared; the actor-level swap is not, so the ids are ours
        // and are membership-checked like every other hand-held id in this
        // plugin. An absent id falls back to the full reset: slow, and correct.
        constexpr REL::RelocationID kChangeActorHeadPart{ 26468, 27063 };

        bool LiveSwapAvailable() {
            static const bool s_ok = [] {
                if (OS::VersionCheck::IdOk(kChangeActorHeadPart)) {
                    return true;
                }
                spdlog::error("HeadPart: the engine's per-part swap id {} is ABSENT from this "
                              "build's Address Library, so every head part change falls "
                              "back to a full 3D reset. Correct, and slow after a heavy "
                              "preset (OS-230).",
                              kChangeActorHeadPart.id());
                return false;
            }();
            return s_ok;
        }

        void ChangeActorHeadPart(RE::Actor* a_actor, RE::BGSHeadPart* a_old,
                                 RE::BGSHeadPart* a_new) {
            using func_t = void (*)(RE::Actor*, RE::BGSHeadPart*, RE::BGSHeadPart*);
            static REL::Relocation<func_t> func{ kChangeActorHeadPart };
            func(a_actor, a_old, a_new);
        }

        // Put the changed part on screen, then put the hair colour back on top
        // of it.
        //
        // ⚠ BOTH HALVES ARE LOAD-BEARING, FOR EVERY KIND AND NOT JUST HAIR.
        // Either route runs the ENGINE's hair painter on what it builds, and
        // that painter derives the tint from the snapped colour form on the
        // actor base rather than from the colour the user picked, so the build
        // on its own silently reverts an exact colour to its nearest palette
        // match. On the full reset that happens whichever part triggered it, so
        // changing BROWS reverted the HAIR colour unless the repaint ran here
        // too; on the swap it happens to the new part. Visible as the hair
        // changing shade slightly on hover and correcting itself on click, back
        // when only the click path reached a repaint.
        //
        // ⚠ Queued onto the GAME thread rather than run inline. Every caller is
        // on the FUCK present thread, and both the swap and Repaint walk the
        // scenegraph, which must never happen there while the game thread may
        // be rebuilding an actor. Handle-based and re-resolved inside the task,
        // mirroring RequestRefreshActor: an actor that streamed out between the
        // request and the drain is skipped rather than dereferenced.
        //
        // ---- orphaned extras in the base list (round twenty-one) ------------
        //
        // Every main-part write this plugin makes lands through
        // TESNPC::ChangeHeadPart, which replaces the MAIN in npc->headParts
        // and nothing else, while the actor-level swap moves geometry for the
        // main AND its extras. A record list that carried the old main's
        // extra (KS hairlines ride PNAM beside their hair) is therefore left
        // holding an extra whose owner is gone, and every later build bakes
        // it forever: the r21 field round wore TWO hairlines, Kaysa's own
        // (attached by the swap) plus the replaced hair's
        // 0_HAIRLINE_Female_Elf_Straight (baked from the stale list entry,
        // its slot-141 partition force-enabled by the engine's per-part
        // hide). The new main's extra is meanwhile NOT in the list, so a
        // full rebuild from the base would build the stale hairline and
        // not the right one.
        //
        // The repair walks a SNAPSHOT (the base write moves the ground, the
        // 05:2x scar) and shifts each orphan out of the array in place,
        // shrink-only; geometry is detached through the null-guarded engine
        // swap.
        //
        // ⚠ NO type-matched ChangeHeadPart replacement here, for two
        // measured reasons. Hairline extras are TYPE kHair - there is no
        // hairline HeadPartType - so a type-matched replace can land on the
        // MAIN hair's list slot and evict the hair itself. And the new
        // main's extra does not need listing at all: the r21 cold load
        // built KSSMP_Kaysa_Elf_HL with the extra absent from the list,
        // so the engine's full build walks main->extraParts on its own.
        // ⚠⚠ THE EXTRA FLAG IS A LIE ON THIS RIG'S HAIRLINES, measured
        // 2026-08-22 23:5x: two sessions of browse swaps stacked `hairline`
        // and `0_HAIRLINE_Female_Human_Straight` in the base list beside the
        // current hair's own hairline, and the flag-gated prune fired ZERO
        // times, because KS-family hairline parts ship without kIsExtraPart.
        // Worse, a look SAVED while the list was contaminated captures the
        // orphan into its jslot, and every load re-seeds it. So extra-ness
        // comes from the DATA instead: a one-time index over every loaded
        // BGSHeadPart's extraParts array. A part somebody lists as an extra
        // IS an extra, flag or no flag; it is legitimate exactly when one of
        // its owners is in the current list.
        const std::unordered_map<RE::BGSHeadPart*,
                                 std::vector<RE::BGSHeadPart*>>&
        ExtraOwnerIndex() {
            static const auto s_index = [] {
                std::unordered_map<RE::BGSHeadPart*,
                                   std::vector<RE::BGSHeadPart*>> index;
                if (auto* const dh = RE::TESDataHandler::GetSingleton()) {
                    for (auto* const part :
                         dh->GetFormArray<RE::BGSHeadPart>()) {
                        if (!part) {
                            continue;
                        }
                        for (auto* const extra : part->extraParts) {
                            if (extra) {
                                index[extra].push_back(part);
                            }
                        }
                    }
                }
                spdlog::info("HeadPart: extra-owner index built, {} part(s) "
                             "are listed as somebody's extra.",
                             index.size());
                return index;
            }();
            return s_index;
        }

        std::uint32_t PruneOrphanedExtras(RE::Actor* a_actor) {
            auto* const npc = a_actor ? a_actor->GetActorBase() : nullptr;
            if (!npc || !npc->headParts || npc->numHeadParts <= 0) {
                return 0;
            }
            const auto& owners = ExtraOwnerIndex();
            std::vector<RE::BGSHeadPart*> list(npc->headParts,
                                               npc->headParts + npc->numHeadParts);
            // Extra-like = flagged OR listed in anybody's extraParts.
            const auto extraLike = [&owners](RE::BGSHeadPart* a_part) {
                return a_part->IsExtraPart() || owners.contains(a_part);
            };
            std::vector<RE::BGSHeadPart*> mains;
            for (auto* const part : list) {
                if (part && !extraLike(part)) {
                    mains.push_back(part);
                }
            }
            const auto inList = [&list](RE::BGSHeadPart* a_part) {
                for (auto* const part : list) {
                    if (part == a_part) {
                        return true;
                    }
                }
                return false;
            };
            // Legit = one of the extra's OWNERS is in the current list. The
            // owner scan covers unflagged extras; the mains'-extraParts scan
            // keeps the old behaviour for a flagged extra the index never
            // saw.
            const auto legit = [&](RE::BGSHeadPart* a_extra) {
                if (const auto it = owners.find(a_extra); it != owners.end()) {
                    for (auto* const owner : it->second) {
                        if (inList(owner)) {
                            return true;
                        }
                    }
                    return false;
                }
                for (auto* const main : mains) {
                    for (auto* const extra : main->extraParts) {
                        if (extra == a_extra) {
                            return true;
                        }
                    }
                }
                return false;
            };
            std::uint32_t pruned = 0;
            for (auto* const part : list) {
                if (!part || !extraLike(part) || legit(part)) {
                    continue;
                }
                // Shift the entry out in place. Shrink-only on the engine's
                // own array; ChangeHeadPart is not asked because it cannot
                // say "none" (and its type match could land on a main).
                std::int8_t w = 0;
                for (std::int8_t i = 0; i < npc->numHeadParts; ++i) {
                    if (npc->headParts[i] != part) {
                        npc->headParts[w++] = npc->headParts[i];
                    }
                }
                npc->numHeadParts = w;
                if (LiveSwapAvailable() && a_actor->GetFaceNodeSkinned()) {
                    ChangeActorHeadPart(a_actor, part, nullptr);
                }
                ++pruned;
                spdlog::info("HeadPart: pruned orphaned extra '{}' ({:08X}) left behind "
                             "by a replaced main.",
                             part->GetFormEditorID() ? part->GetFormEditorID()
                                                     : "(no edid)",
                             part->GetFormID());
            }
            return pruned;
        }

        // a_old is what the actor wore in the slot before the caller wrote the
        // base (null when they wore nothing), a_new is what the base now says.
        // The swap needs both: the old one to know which geometry to detach.
        void QueueRebuildAndRepaint(RE::Actor* a_actor, RE::BGSHeadPart* a_old,
                                    RE::BGSHeadPart* a_new) {
            if (!a_actor) {
                return;
            }
            const auto handle = a_actor->GetHandle();
            const auto work = [handle, a_old, a_new]() {
                auto  ptr   = handle.get();
                auto* actor = ptr.get();
                if (!actor) {
                    return;
                }
                // ⚠ THE SWAP NEEDS A FACE NODE TO OPERATE ON, and it is checked
                // here rather than trusted to the engine's own null check so the
                // fallback is explicit: with no face node there is nothing on
                // screen to swap, and the base write already landed, so the
                // engine builds the head with the new part when the 3D loads.
                // DoReset3D covers that case the way it always did.
                if (LiveSwapAvailable() && actor->GetFaceNodeSkinned()) {
                    const auto t0 = std::chrono::steady_clock::now();
                    ChangeActorHeadPart(actor, a_old, a_new);
                    const auto ms =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
                    // ⚠ THE NUMBER IS THE FIELD GATE FOR OS-230, so it is on
                    // every switch and at info. Tens of milliseconds after a
                    // preset load is the fix; hundreds is the old cost wearing a
                    // new name.
                    spdlog::info("HeadPart: actor {:08X} swapped part {:08X} -> {:08X} on the "
                                 "live face node in {:.1f} ms (no 3D reset).",
                                 actor->GetFormID(), a_old ? a_old->GetFormID() : 0u,
                                 a_new ? a_new->GetFormID() : 0u, ms);
                } else {
                    actor->DoReset3D(false);
                }
                // The caller's base write replaced a MAIN and left its extras
                // in the list (and the new main's out of it) - the r21 double
                // hairline. Pruned here, after the swap, so the detach half
                // can still find the orphan's geometry by name.
                PruneOrphanedExtras(actor);
                HairColor::Repaint(actor);
                // A head dye is a material swap on the part's geometry, and the
                // geometry just changed. On the full reset HeadBuildHook queues
                // this from the far side of the build, because a full build
                // always builds the hair and the face and its gate lets those
                // two through. On the swap the painter fires only for the part
                // that changed, and for eyes, brows and the discovered slots
                // the gate drops it, so it is asked for here for every kind.
                // QueueRepaint re-resolves the actor and the outfit when it
                // drains and restores before it paints, and one with nothing to
                // do is a walk that writes nothing.
                OutfitDye::QueueRepaint(actor->GetHandle());
                // Round eighteen: a swapped-in SMP style can arrive with its
                // head-family partition off (parts[131-], skinned, bounded,
                // invisible), and the in-FR change went FULLY bald on the
                // field. Worn-gated, so a real helmet keeps its hide; once
                // now and once deferred, the restorer's two-pass doctrine.
                //
                // Round nineteen: those two passes both run BEFORE the swap's
                // deferred attach lands (the queued one rides the SKSE queue,
                // the attach the BSTaskPool), and the in-FR change went
                // fully bald WITH them deployed. The ladder is the provably
                // late half: +1 s, +2.5 s, +5 s off a watcher thread.
                BipedPost::RestoreHeadPartPartitions(actor, "swap-sync");
                BipedPost::QueueRestoreHeadPartPartitions(actor->GetHandle(),
                                                          "swap-queued");
                BipedPost::ArmHeadPartitionSettle(actor->GetHandle());
            };
            if (auto* task = SKSE::GetTaskInterface()) {
                task->AddTask(work);
            } else {
                work();  // no task interface (tests, early load): better than nothing
            }
        }

    }  // namespace

    const char* KindName(Kind a_kind) {
        switch (a_kind) {
            case Kind::kHair:       return "hair";
            case Kind::kEyes:       return "eyes";
            case Kind::kBrows:      return "brows";
            case Kind::kFacialHair: return "facial hair";
        }
        return "head part";
    }

    bool CanEdit(RE::Actor* a_actor) {
        return a_actor && a_actor->IsPlayerRef();
    }

    OfferKey OfferKeyFor(RE::Actor* a_actor) {
        OfferKey key;
        auto* const base = BaseOf(a_actor);
        if (!base) {
            return key;  // invalid, and Valid() says so
        }
        key.actor = a_actor->GetFormID();
        // ⚠ THE BASE'S RACE AND SEX, the two AvailableFor below reads. Keep
        // these two lines and that filter in step; a key that samples
        // something else is a cache that invalidates on the wrong event.
        if (auto* const race = base->GetRace()) {
            key.race = race->GetFormID();
        }
        key.sex = static_cast<std::int32_t>(base->GetSex());
        return key;
    }

    std::optional<HeadPartPlan::Reject> WhyNotFor(RE::Actor* a_actor, Kind a_kind,
                                                  RE::BGSHeadPart* a_part) {
        auto* const base = BaseOf(a_actor);
        if (!base || !a_part) {
            return std::nullopt;  // cannot read the character: not a verdict
        }
        // ⚠ THE BASE'S RACE, not Actor::GetRace(), for OfferKeyFor's reason:
        // they are two answers and one can lag the other. AvailableFor reads
        // the base, so this must too or the browser and the push disagree.
        return HeadPartPlan::Judge(
            CandidateFor(a_part, EngineType(a_kind), base->GetRace()),
            base->GetSex() == RE::SEX::kFemale);
    }

    Slot SlotOf(Kind a_kind) {
        return static_cast<Slot>(EngineType(a_kind));
    }

    const std::vector<HeadPartSlotPlan::Slot>& DiscoveredSlots() {
        // Walked once. The head-part form array is fixed after load, so a
        // second walk could only produce the same answer at the cost of another
        // pass over thousands of forms.
        static const std::vector<HeadPartSlotPlan::Slot> slots = [] {
            std::vector<HeadPartSlotPlan::Slot> out;
            auto* const                         dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return out;
            }
            // Insertion-ordered accumulation into `out`, with this index to
            // find a slot again. A map would order by type on its own, but the
            // display order is SortForDisplay's decision and belongs there
            // rather than falling out of the container.
            std::map<Slot, std::size_t> at;
            for (auto* part : dh->GetFormArray<RE::BGSHeadPart>()) {
                if (!part) {
                    continue;
                }
                const auto type = static_cast<Slot>(part->type.get());
                if (!HeadPartSlotPlan::IsCustomType(type)) {
                    continue;
                }
                auto it = at.find(type);
                if (it == at.end()) {
                    it = at.emplace(type, out.size()).first;
                    out.push_back(HeadPartSlotPlan::Slot{ type, 0, {}, {} });
                }
                auto& slot = out[it->second];
                ++slot.parts;
                // ⚠ THE PLUGIN OF THE FIRST NON-EXTRA PART, not of the first
                // part. Chooey's ships 55 extra-part children and 11 selectable
                // parents in one plugin, and a load order where a patch defines
                // only children would otherwise have the patch name the slot.
                if (slot.plugin.empty() && !part->IsExtraPart()) {
                    slot.plugin = SourceOf(part);
                }
            }
            for (auto& slot : out) {
                // ⚠ THE TRANSLATION ARM OF THE LADDER IS NOT WIRED YET, so this
                // resolves to the plugin stem or to the type number. The rule
                // takes the value as a parameter precisely so reading the file
                // can be added without touching the decision, and the file read
                // is deliberately deferred: it has to survive a plugin whose
                // table lives in a BSA and a game not running in English, and
                // neither is worth solving before the engine has agreed to
                // render a part applied this way at all. See the design note.
                slot.label = HeadPartSlotPlan::LabelFor(slot.type, {}, slot.plugin);
            }
            HeadPartSlotPlan::SortForDisplay(out);
            if (out.empty()) {
                spdlog::info("HeadPart: no custom head-part slots in this load order.");
            } else {
                spdlog::info("HeadPart: {} custom head-part slot(s) discovered.",
                             out.size());
                for (const auto& slot : out) {
                    spdlog::info("HeadPart:   type {:>5}: {} part(s), '{}' from {}",
                                 slot.type, slot.parts, slot.label,
                                 slot.plugin.empty() ? "(no plugin)" : slot.plugin);
                }
            }
            return out;
        }();
        return slots;
    }

    std::string SlotName(Slot a_slot) {
        // The four shipped kinds keep the words they already log with, so no
        // existing log line changes wording because this function arrived.
        if (a_slot == SlotOf(Kind::kHair)) {
            return KindName(Kind::kHair);
        }
        if (a_slot == SlotOf(Kind::kEyes)) {
            return KindName(Kind::kEyes);
        }
        if (a_slot == SlotOf(Kind::kBrows)) {
            return KindName(Kind::kBrows);
        }
        if (a_slot == SlotOf(Kind::kFacialHair)) {
            return KindName(Kind::kFacialHair);
        }
        for (const auto& slot : DiscoveredSlots()) {
            if (slot.type == a_slot) {
                return slot.label;
            }
        }
        // A vanilla type we do not offer (face, scars, misc), or a number no
        // part in this load order carries. Both are askable and neither is an
        // error, so the label says what it is rather than pretending.
        return HeadPartSlotPlan::LabelFor(a_slot, {}, {});
    }

    std::optional<HeadPartPlan::Reject> WhyNotForSlot(RE::Actor* a_actor, Slot a_slot,
                                                      RE::BGSHeadPart* a_part) {
        auto* const base = BaseOf(a_actor);
        if (!base || !a_part) {
            return std::nullopt;  // cannot read the character: not a verdict
        }
        return HeadPartPlan::Judge(
            CandidateFor(a_part, static_cast<RE::BGSHeadPart::HeadPartType>(a_slot),
                         base->GetRace()),
            base->GetSex() == RE::SEX::kFemale);
    }

    bool IsValidForSlot(RE::Actor* a_actor, Slot a_slot, RE::BGSHeadPart* a_part) {
        // ⚠ AN UNREADABLE CHARACTER ANSWERS FALSE, as on the Kind route: this
        // gates a WRITE, so "I could not tell" has to mean "do not".
        const auto why = WhyNotForSlot(a_actor, a_slot, a_part);
        return why && *why == HeadPartPlan::Reject::kNone;
    }

    bool IsValidFor(RE::Actor* a_actor, Kind a_kind, RE::BGSHeadPart* a_part) {
        // ⚠ AN UNREADABLE CHARACTER ANSWERS FALSE, not true. This gates
        // WRITING a part onto an actor's record, so "I could not tell" has to
        // mean "do not", or the one case we cannot reason about is the one
        // that gets the write.
        const auto why = WhyNotFor(a_actor, a_kind, a_part);
        return why && *why == HeadPartPlan::Reject::kNone;
    }

    namespace {
        // Does the mesh this part names actually exist?
        //
        // ⚠⚠ THROUGH THE ENGINE'S RESOURCE SYSTEM, NEVER std::filesystem. Almost
        // every vanilla mesh and most modded ones live inside a BSA, so a
        // filesystem check would answer "missing" for the majority of parts that
        // work perfectly. BSResourceNiBinaryStream is the same path the engine
        // loads through, so it sees loose files and archives alike and answers
        // the question the renderer will ask.
        //
        // ⚠ CACHED BY PATH, because this runs over the whole BGSHeadPart form
        // array every time the offered list is rebuilt, and that happens on any
        // race or sex change. NK Horn Slider alone is 357 parts. The cache is
        // keyed on the path rather than the form so parts sharing a mesh only
        // pay once, which they frequently do.
        //
        // ⚠⚠ AND THE CACHE IS THE ONLY THING THAT MADE THIS SURVIVABLE, WHICH
        // IS WHY IT FROZE THE GAME FOR FIVE SECONDS ON THE FIRST OPEN AND NEVER
        // AGAIN. Measured on the dev rig 2026-08-15, from the log's own
        // timestamps: the hair pass took 4.926 s and the eyes pass that ran
        // immediately after it took 0.053 s, over comparable part counts (1001
        // and 1002). The difference is DISTINCT PATHS. Eyes are one shared mesh
        // plus per-record texture sets, so the second eye part onwards is a
        // cache hit; every hairstyle is its own nif, so KS Hairdo's alone is 882
        // separate resolves. Field-reported as "the game freezes for like 10
        // seconds when I open FR when I have just booted the game" on a tester's
        // heavier load order.
        //
        // Two things were wrong, and only the first of them is free to fix.
        std::size_t g_resolveCalls = 0;    // cache misses, i.e. real work
        std::size_t g_resolveOpens = 0;    // stream constructions attempted
        std::size_t g_resolveSecond = 0;   // answered only by the SECOND spelling
        std::size_t g_resolveSkipped = 0;  // offered unchecked, budget spent
        double      g_resolveMs = 0.0;

        [[nodiscard]] bool ModelResolves(std::string_view a_model, double& a_budgetMs) {
            static std::unordered_map<std::string, bool> cache;
            std::string key{ a_model };
            if (const auto it = cache.find(key); it != cache.end()) {
                return it->second;
            }
            // ⚠⚠ OUT OF BUDGET MEANS OFFER IT, NEVER DROP IT, and the direction
            // is the whole safety of this. An unchecked part behaves exactly as
            // it did before this filter existed, so the worst case is the bug
            // the filter was written for (a horn that applies and grows
            // nothing). Failing the other way would hide working parts on a
            // slow disk, which is a filter inventing a broken load order. The
            // answer is deliberately NOT cached either: it was never asked.
            if (a_budgetMs <= 0.0) {
                ++g_resolveSkipped;
                return true;
            }
            const auto start = std::chrono::steady_clock::now();
            const auto tryOpen = [](const std::string& a_path) {
                ++g_resolveOpens;
                RE::BSResourceNiBinaryStream s{ a_path.c_str() };
                return s.good();
            };
            // ⚠⚠ THE PREFIXED SPELLING IS TRIED FIRST, AND IT USED TO BE
            // SECOND. NifModelLoader.cpp:181 already had this settled and said
            // so in one line - "a form's model path is authored without the
            // meshes prefix and sometimes with a data one; BSResource wants
            // meshes\..." - and it simply prepends rather than guessing. This
            // function guessed, and it guessed the rare way round first, so
            // every head part in the load order paid a guaranteed miss before
            // the spelling that works was tried at all.
            //
            // ⚠ BOTH SPELLINGS STILL, because the records are not consistent:
            // this load order carries both "ed horns/horna.nif" and
            // "meshes/nightshade/wearable horns/...". Only the ORDER changed,
            // so nothing that resolved before stops resolving.
            const bool prefixed = key.size() >= 7 &&
                                  _strnicmp(key.c_str(), "meshes", 6) == 0 &&
                                  (key[6] == '\\' || key[6] == '/');
            bool ok = tryOpen(prefixed ? key : "meshes\\" + key);
            if (!ok) {
                ++g_resolveSecond;
                ok = tryOpen(prefixed ? key.substr(7) : key);
            }
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
            a_budgetMs -= ms;
            g_resolveMs += ms;
            ++g_resolveCalls;
            cache.emplace(std::move(key), ok);
            return ok;
        }

        // ⚠⚠ A CEILING ON THE FREEZE, NOT A TARGET, AND IT IS PER SLOT PASS.
        // Reordering the spellings halves the work but cannot bound it: the
        // remaining half is a real stream construction per distinct mesh, and
        // how many of those a load order has is not ours to know. A player with
        // three hair packs would still wait, and the number would grow with
        // every mod they added, which is the shape of a complaint that never
        // stops arriving.
        //
        // Whatever is left unchecked is OFFERED and stays uncached, so the next
        // rebuild picks it up with a fresh budget and the list converges over
        // the first few opens. The entries that eventually disappear are ones
        // that never worked.
        constexpr double kResolveBudgetMs = 20.0;
    }  // namespace

    std::vector<Entry> AvailableForSlot(RE::Actor* a_actor, Slot a_slot) {
        std::vector<Entry> out;
        auto* const        base = BaseOf(a_actor);
        if (!base) {
            return out;
        }
        auto* const dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return out;
        }
        const auto  slotLabel = SlotName(a_slot);
        auto* const race      = base->GetRace();
        const bool  isFemale  = base->GetSex() == RE::SEX::kFemale;
        const auto  wanted = static_cast<RE::BGSHeadPart::HeadPartType>(a_slot);

        // Counted by reason rather than totalled. A bare total cannot say what
        // was dropped or why, and this project has already lost a round to
        // exactly that: 1011 parts looked healthy while every one of the 26
        // vanilla ones was being discarded for having no resolvable name.
        std::map<HeadPartPlan::Reject, std::size_t> rejected;
        // ⚠ COUNTED BESIDE THE JUDGE'S REASONS RATHER THAN AS ONE OF THEM.
        // HeadPartPlan::Reject is a closed enum whose switch sites the compiler
        // lists; this is not a judgement about the RECORD, it is about what is
        // installed alongside it, and the same record is fine on a load order
        // that has the mesh. Widening the enum would put a load-order fact in a
        // vocabulary that describes records.
        std::size_t missingModel = 0;

        // Spent by ModelResolves, and only on paths it has not already answered.
        // A warm cache costs nothing, which is why this is per pass rather than
        // per process: it is the FIRST pass over a load order that needs
        // bounding, and once bounded it never needs bounding again.
        double budgetMs = kResolveBudgetMs;
        const auto callsBefore   = g_resolveCalls;
        const auto opensBefore   = g_resolveOpens;
        const auto secondBefore  = g_resolveSecond;
        const auto skippedBefore = g_resolveSkipped;
        const auto msBefore      = g_resolveMs;

        std::unordered_set<RE::FormID> seen;
        for (auto* part : dh->GetFormArray<RE::BGSHeadPart>()) {
            if (!part) {
                continue;
            }
            const auto c = CandidateFor(part, wanted, race);

            if (const auto why = HeadPartPlan::Judge(c, isFemale);
                why != HeadPartPlan::Reject::kNone) {
                // Wrong type is nearly the whole form array on every call and
                // says nothing about this load order. Counting it would bury
                // the reasons that matter.
                if (why != HeadPartPlan::Reject::kWrongType) {
                    ++rejected[why];
                }
                continue;
            }
            if (!seen.insert(part->GetFormID()).second) {
                continue;
            }
            // ⚠⚠ A PART THAT NAMES A MESH NOBODY HAS IS NOT AN OPTION, and
            // NK Horn Slider is why. That mod is a FRAMEWORK: its records point
            // at horn meshes owned by other mods, so on any given load order a
            // large share of its 357 parts name a file that is not installed.
            // Applying one succeeded at every layer we own - the log says
            // "rebuild issued" - and the character simply grew no horns, which
            // is the worst kind of failure because nothing reports it (user
            // 2026-08-14, "a lot of the horns in NK Horn slider don't work").
            //
            // ⚠⚠ NAMED-BUT-MISSING IS DROPPED, NAMELESS IS KEPT, AND CONFLATING
            // THE TWO WOULD BREAK TAKING HORNS OFF. The removal placeholders
            // these mods ship - No Horns, CDE00, EDNoHorn - deliberately carry
            // NO model at all, because ChangeHeadPart cannot express "none" and
            // a mesh-less part is the only way to say it. Filtering on "has no
            // mesh" would remove exactly the entries that let a player undo a
            // choice. The question is whether a NAMED mesh resolves.
            if (const char* m = part->GetModel(); m && *m && !ModelResolves(m, budgetMs)) {
                ++missingModel;
                continue;
            }
            // ⚠ NO name filter here. NameOf never returns empty now, and the
            // check that used to sit here is what hid every vanilla part.
            Entry e{ part, NameOf(part, slotLabel), SourceOf(part),
                     ScenePathsOf(part) };
            // The part's TNAM rides the MAIN model only (OS-192): eye colour
            // is one shared mesh plus this per-record texture set, and the
            // extras keep their own look the way the engine leaves them.
            // Guarded on the part's own model being paths[0]: a model-less
            // part whose scene is all extras must not retexture an extra.
            if (const char* m = part->GetModel();
                m && *m && !e.modelPaths.empty()) {
                if (auto tnam = PreviewSwapCapture::FromTextureSet(part->textureSet);
                    !tnam.empty()) {
                    e.swaps.resize(e.modelPaths.size());
                    e.swaps[0] = std::move(tnam);
                }
            }
            out.push_back(std::move(e));
        }

        // Sorted by name, with the form ID breaking ties, so the order is stable
        // across sessions on an unchanged load order. An index into this list
        // therefore means the same thing twice, which is what lets the editor
        // remember a position.
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
            if (a.name != b.name) {
                return a.name < b.name;
            }
            return a.part->GetFormID() < b.part->GetFormID();
        });

        std::map<std::string, std::size_t> bySource;
        for (const auto& e : out) {
            ++bySource[e.source.empty() ? "(no plugin)" : e.source];
        }
        spdlog::info("HeadPart: {} {} part(s) offered for actor {:08X}, from {} plugin(s).",
                     out.size(), slotLabel,
                     a_actor ? a_actor->GetFormID() : 0, bySource.size());
        for (const auto& [src, n] : bySource) {
            spdlog::info("HeadPart:   {:>5} from {}", n, src);
        }
        for (const auto& [why, n] : rejected) {
            spdlog::info("HeadPart:   {:>5} rejected, {}", n,
                         HeadPartPlan::ReasonName(why));
        }
        // ⚠ ITS OWN LINE, because it is the one rejection reason that is about
        // the INSTALL rather than the record, and it is the number that answers
        // "why are there fewer horns than the mod advertises".
        if (missingModel > 0) {
            spdlog::info("HeadPart:   {:>5} rejected, model file not found", missingModel);
        }
        // ⚠⚠ THE COST OF THE FILTER, SAID OUT LOUD, because it was invisible
        // and it froze the game. Before this line the only evidence that the
        // mesh check existed at all was a five second hole between two
        // timestamps, which is a shape somebody has to go looking for. A pass
        // that resolves nothing prints nothing, so a warm cache stays silent.
        //
        // Read it as: `resolved` is how many distinct meshes this pass had to
        // ask about, `opens` how many stream constructions that took (the ideal
        // is one each; two means the prefixed spelling missed), `secondSpelling`
        // how many needed the fallback, and `skipped` how many were offered
        // unchecked because the budget ran out. A nonzero `skipped` is the line
        // that says this load order is bigger than the ceiling and the list will
        // settle over the next few opens.
        if (const auto calls = g_resolveCalls - callsBefore; calls > 0) {
            spdlog::info("HeadPart:   mesh check for {}: resolved {} distinct mesh(es) in "
                         "{:.1f} ms ({} open(s), {} needed the second spelling), {} "
                         "offered unchecked once the {:.0f} ms budget was spent.",
                         slotLabel, calls, g_resolveMs - msBefore,
                         g_resolveOpens - opensBefore, g_resolveSecond - secondBefore,
                         g_resolveSkipped - skippedBefore, kResolveBudgetMs);
        }
        return out;
    }

    // ⚠⚠ THERE IS NO MORPH GUARD HERE ANY MORE, AND THE RECORD OF WHY IT CAME
    // AND WENT IS WORTH MORE THAN THE GUARD WAS. For nine hours on 2026-08-18
    // (`6818b72`) ApplySlot and RestoreSlot refused any invented-slot part that
    // carried a morph tri, on the theory that such a part is finished by
    // RaceMenu's facegen bake and comes back invisible through a plain
    // ChangeHeadPart + rebuild. The evidence was one part: NK's 'Argonian
    // Feathers 2' grew zero geometry that way. The field answered within the
    // day ("i can only change the head extra of ED Horns RM integration, when i
    // change any others like Cat ears or NK horns, i don't see any change, this
    // is a regression"), and the 18:29 log refuted the theory outright: the
    // character wore NK's '_NK_DragonboneHelmHorns' (every NK part carries the
    // same 'HornTestUnified.tri'), the ED horn change at 18:30:06 issued OUR
    // rebuild, and the census after it listed the NK horn's geometry, in the
    // scene, matched to its part by name. A morph-carrying part renders through
    // this route. Whatever 'Argonian Feathers 2' did wrong, it was that part's
    // MESH, not its tri, and a mesh problem is per part: the instrument is the
    // census tally line for the slot after the rebuild, and the answer, if it
    // comes back, is reading that NIF, not a rule over the record.
    //
    // The morph triple is still LOGGED after every apply (below), so a part
    // that does come back invisible is on record with the thing that was
    // suspected of it.

    RE::BGSHeadPart* CurrentForSlot(RE::Actor* a_actor, Slot a_slot) {
        auto* const base = BaseOf(a_actor);
        if (!base) {
            return nullptr;
        }
        // GetCurrentHeadPartByType, not GetHeadPartByType: the former consults
        // the overlay list first, which is what the engine itself renders from
        // when an actor has overlays.
        //
        // ⚠ THE TYPE IS COMPARED, NOT VALIDATED, and that is what makes a
        // discovered slot work at all. The engine matches head parts on the
        // number in this field and never asks whether the number is one it
        // named, so passing 32 finds the part carrying 32 and nothing else.
        return base->GetCurrentHeadPartByType(
            static_cast<RE::BGSHeadPart::HeadPartType>(a_slot));
    }

    bool ApplySlot(RE::Actor* a_actor, Slot a_slot, RE::BGSHeadPart* a_part) {
        auto* const base = BaseOf(a_actor);
        if (!base || !a_part) {
            return false;
        }
        const auto slotLabel = SlotName(a_slot);
        // See the header. The rebuild below is what puts the change on screen,
        // and on a named NPC it replaces her complexion with one no repaint can
        // undo. Refusing here rather than trusting callers is deliberate.
        if (!CanEdit(a_actor)) {
            spdlog::warn("HeadPart: refusing to change {} on actor {:08X}: this route "
                         "rebuilds the head, which replaces a named NPC's complexion "
                         "irreversibly. Followers need NpcHair's attach instead.",
                         slotLabel, a_actor->GetFormID());
            return false;
        }
        auto* const current = CurrentForSlot(a_actor, a_slot);
        if (current == a_part) {
            return false;  // already wearing it; a rebuild would be pure cost
        }

        {
            std::scoped_lock l(g_lock);
            // Write-once, and for the same reason HairColor's capture is: the
            // first thing we see is the truth about what they looked like
            // before, and any later one is describing our own work. A null
            // current is a real capture, meaning they had no part of this kind,
            // and it has to round-trip as null rather than as "not captured".
            g_original.try_emplace({ a_actor->GetFormID(), a_slot }, current);
        }

        base->ChangeHeadPart(a_part);
        CountWrite(a_actor->GetFormID(), a_slot);

        // The base write above renders nothing on its own; see
        // QueueRebuildAndRepaint for what puts it on screen and why the colour
        // has to be reapplied on the far side of that. `current` is the part
        // the swap has to take off.
        QueueRebuildAndRepaint(a_actor, current, a_part);

        spdlog::info("HeadPart: actor {:08X} {} '{}' -> '{}' ({:08X}), swap queued.",
                     a_actor->GetFormID(), slotLabel, NameOf(current, slotLabel),
                     NameOf(a_part, slotLabel), a_part->GetFormID());
        // ⚠⚠ THE MORPH TRIPLE, BECAUSE IT IS THE ONE THING THAT SEPARATES THE
        // HORN MOD THAT WORKS FROM THE ONE THAT DOES NOT (user 2026-08-16, "ED
        // horns RMintegration works but not NK horns"). Measured off both
        // plugins: the records are otherwise alike - both invented types (106
        // and 32), both Male+Female, both the same valid-races form, both
        // naming a mesh that resolves, and NK's NIF is a properly skinned
        // BSTriShape bounded at head height. The difference is that every NK
        // part carries a race/chargen morph triple ('HornTestUnified.tri') and
        // no ED part carries one at all.
        //
        // A part with a chargen morph is finished by the facegen pass rather
        // than drawn as authored, so "the base write landed and nothing grew"
        // is exactly what an unmorphed one would look like. This line does not
        // decide that - it RECORDS which of the two kinds was just applied, so
        // one field round can tell them apart instead of a fifth guess.
        {
            const auto* const tri = a_part->morphs;
            const auto        name = [](const RE::TESModelTri& m) {
                return (m.model.empty()) ? "-" : m.model.c_str();
            };
            spdlog::info("HeadPart:   morphs race='{}' default='{}' chargen='{}', model='{}'",
                         name(tri[RE::BGSHeadPart::MorphIndices::kRaceMorph]),
                         name(tri[RE::BGSHeadPart::MorphIndices::kDefaultMorph]),
                         name(tri[RE::BGSHeadPart::MorphIndices::kChargenMorph]),
                         a_part->GetModel() ? a_part->GetModel() : "");
        }
        return true;
    }

    RE::BGSHeadPart* PlaceholderForSlot(RE::Actor* a_actor, Slot a_slot) {
        auto* const base = BaseOf(a_actor);
        if (!base) {
            return nullptr;
        }
        auto* const dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return nullptr;
        }
        auto* const race     = base->GetRace();
        const bool  isFemale = base->GetSex() == RE::SEX::kFemale;
        const auto  wanted   = static_cast<RE::BGSHeadPart::HeadPartType>(a_slot);
        // ⚠ THE SAME JUDGE AvailableForSlot USES, so a placeholder this returns
        // is one the pane would have listed, and never a part the character
        // could not wear. No mesh resolve here: the whole point of the part is
        // that it names no mesh, so there is nothing to resolve.
        //
        // ⚠ THE FIRST BY NAME, THEN BY FORM ID, the pane's own order, so a slot
        // that ships two nameless parts answers the same way on every launch.
        RE::BGSHeadPart* best = nullptr;
        std::string      bestName;
        const auto       slotLabel = SlotName(a_slot);
        for (auto* part : dh->GetFormArray<RE::BGSHeadPart>()) {
            if (!part) {
                continue;
            }
            if (const char* m = part->GetModel(); m && *m) {
                continue;  // names a mesh, so it is a part rather than an absence
            }
            if (HeadPartPlan::Judge(CandidateFor(part, wanted, race), isFemale) !=
                HeadPartPlan::Reject::kNone) {
                continue;
            }
            auto name = NameOf(part, slotLabel);
            if (!best || name < bestName ||
                (name == bestName && part->GetFormID() < best->GetFormID())) {
                best     = part;
                bestName = std::move(name);
            }
        }
        return best;
    }

    bool RestoreSlot(RE::Actor* a_actor, Slot a_slot) {
        auto* const base = BaseOf(a_actor);
        if (!base) {
            return false;
        }
        const auto slotLabel = SlotName(a_slot);
        RE::BGSHeadPart* original = nullptr;
        {
            std::scoped_lock l(g_lock);
            const auto       it = g_original.find({ a_actor->GetFormID(), a_slot });
            if (it == g_original.end()) {
                return false;  // never touched this actor's part of this kind
            }
            original = it->second;
            g_original.erase(it);
        }
        if (!original) {
            // They had no part of this kind before, and ChangeHeadPart has no
            // way to say "remove". What it CAN say is the placeholder the
            // authoring mod ships for exactly this (No Horns, EDNoHorn, CDE00):
            // a part that names no mesh, so wearing it draws nothing. That is
            // what a player picking the "none" card does by hand, and it is what
            // "put back nothing" has to mean here (field 2026-08-17: a staged
            // ear was cleared, this path said it could not express that, and
            // the ear stayed on).
            //
            // ⚠ THE CAPTURE STAYS ERASED EITHER WAY. It said "nothing", and
            // nothing is what is on screen now; a later Apply captures the
            // placeholder as the new baseline, which restores to the same
            // absence.
            if (auto* const none = PlaceholderForSlot(a_actor, a_slot)) {
                auto* const worn = CurrentForSlot(a_actor, a_slot);
                if (worn == none) {
                    return false;  // already wearing the absence
                }
                base->ChangeHeadPart(none);
                CountWrite(a_actor->GetFormID(), a_slot);
                QueueRebuildAndRepaint(a_actor, worn, none);
                spdlog::info("HeadPart: actor {:08X} had NO {} part originally; wearing the "
                             "slot's placeholder '{}' ({:08X}) to say so, swap queued.",
                             a_actor->GetFormID(), slotLabel, NameOf(none, slotLabel),
                             none->GetFormID());
                return true;
            }
            // No placeholder shipped for this slot, so this is honestly
            // unsupported rather than silently wrong: say so, and leave what is
            // there.
            spdlog::warn("HeadPart: actor {:08X} had NO {} part originally and the slot "
                         "ships no placeholder; cannot express that through "
                         "ChangeHeadPart, leaving the current one.",
                         a_actor->GetFormID(), slotLabel);
            return false;
        }
        // ⚠⚠ A CAPTURE BELONGS TO THE CHARACTER WHO MADE IT, AND THEY CAN STOP
        // BEING THAT CHARACTER. The capture is keyed on the actor's form id,
        // which survives a RaceMenu race or sex change, so "what they had
        // before we touched it" can be a part the actor may no longer wear.
        // Field 2026-08-09: male picks a beard (capture = his own HumanBeard32),
        // switches to female, the ladder correctly declines the outfit and the
        // default, falls through to here, and Restore put a male-only beard on
        // her. Gating the rungs and leaving this unguarded fixed nothing for
        // the case that actually reached the screen.
        //
        // The capture is DROPPED rather than kept: it was erased from the map
        // above, and re-adding it would only queue the same wrong write for the
        // next push. Nothing is written, so the character keeps whatever the
        // engine's own rebuild gave them, which is the right part for who they
        // are now.
        if (!IsValidForSlot(a_actor, a_slot, original)) {
            spdlog::info("HeadPart: actor {:08X} was wearing '{}' before Fitting Room "
                         "touched their {}, but that part is not offered to them any "
                         "more; dropping it rather than putting it back.",
                         a_actor->GetFormID(), NameOf(original, slotLabel),
                         slotLabel);
            return false;
        }
        // What they wear NOW is what the swap takes off; the capture is what
        // goes on. Read before the base write, which is the only moment the
        // engine still answers with the outgoing part.
        auto* const worn = CurrentForSlot(a_actor, a_slot);
        if (worn == original) {
            return false;  // already wearing it, same as ApplySlot's early return
        }
        base->ChangeHeadPart(original);
        CountWrite(a_actor->GetFormID(), a_slot);
        QueueRebuildAndRepaint(a_actor, worn, original);
        spdlog::info("HeadPart: actor {:08X} {} restored to '{}' ({:08X}), swap queued.",
                     a_actor->GetFormID(), slotLabel,
                     NameOf(original, slotLabel), original->GetFormID());
        return true;
    }

    std::optional<Entry> CapturedFor(RE::Actor* a_actor, Kind a_kind) {
        if (!a_actor) {
            return std::nullopt;
        }
        std::scoped_lock l(g_lock);
        const auto       it = g_original.find({ a_actor->GetFormID(), SlotOf(a_kind) });
        if (it == g_original.end()) {
            return std::nullopt;
        }
        return Entry{ it->second, NameOf(it->second, KindName(a_kind)), SourceOf(it->second) };
    }

    bool HasCaptureSlot(RE::Actor* a_actor, Slot a_slot) {
        if (!a_actor) {
            return false;
        }
        std::scoped_lock l(g_lock);
        return g_original.contains({ a_actor->GetFormID(), a_slot });
    }

    bool HasCapture(RE::Actor* a_actor, Kind a_kind) {
        return HasCaptureSlot(a_actor, SlotOf(a_kind));
    }

    std::uint32_t WriteCountForSlot(RE::Actor* a_actor, Slot a_slot) {
        if (!a_actor) {
            return 0;
        }
        std::scoped_lock l(g_lock);
        const auto       it = g_writes.find({ a_actor->GetFormID(), a_slot });
        return it == g_writes.end() ? 0u : it->second;
    }

    void ReseedCaptureSlot(RE::Actor* a_actor, Slot a_slot) {
        auto* const base = BaseOf(a_actor);
        if (!base) {
            return;
        }
        // Read the actor OUTSIDE the lock, exactly as ApplySlot does: g_lock
        // guards the map and nothing in this module holds it across engine
        // work.
        auto* const now = CurrentForSlot(a_actor, a_slot);

        const auto slotLabel = SlotName(a_slot);
        std::scoped_lock l(g_lock);
        const auto       it = g_original.find({ a_actor->GetFormID(), a_slot });
        if (it == g_original.end()) {
            // ⚠ NOT try_emplace. See the header: a capture created here would
            // authorise a later Wear::kOwn push to write a part we never
            // replaced. "Nothing captured" is a state to leave alone.
            return;
        }
        if (it->second == now) {
            return;
        }
        spdlog::info("HeadPart: actor {:08X} {} baseline re-pointed at '{}' ({:08X}); "
                     "what they had before was '{}'.",
                     a_actor->GetFormID(), slotLabel, NameOf(now, slotLabel),
                     now ? now->GetFormID() : 0, NameOf(it->second, slotLabel));
        it->second = now;
    }

    // ---- the four shipped kinds, which are four particular slots -----------
    // Delegates rather than copies, so a discovered slot and a shipped kind
    // cannot answer the same question two ways.
    std::vector<Entry> AvailableFor(RE::Actor* a_actor, Kind a_kind) {
        return AvailableForSlot(a_actor, SlotOf(a_kind));
    }

    RE::BGSHeadPart* CurrentFor(RE::Actor* a_actor, Kind a_kind) {
        return CurrentForSlot(a_actor, SlotOf(a_kind));
    }

    bool Apply(RE::Actor* a_actor, Kind a_kind, RE::BGSHeadPart* a_part) {
        return ApplySlot(a_actor, SlotOf(a_kind), a_part);
    }

    bool Restore(RE::Actor* a_actor, Kind a_kind) {
        return RestoreSlot(a_actor, SlotOf(a_kind));
    }

    std::uint32_t WriteCountFor(RE::Actor* a_actor, Kind a_kind) {
        return WriteCountForSlot(a_actor, SlotOf(a_kind));
    }

    void ReseedCapture(RE::Actor* a_actor, Kind a_kind) {
        ReseedCaptureSlot(a_actor, SlotOf(a_kind));
    }

    void Clear() {
        std::scoped_lock l(g_lock);
        g_original.clear();
        // The counts describe the character being torn down, and the sink that
        // reads them keys on the player's form id, which is 0x14 in every save.
        g_writes.clear();
    }

    std::vector<Capture> SnapshotCaptures() {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return {};
        }
        const RE::FormID  id = player->GetFormID();
        std::vector<Capture> out;
        std::scoped_lock  l(g_lock);
        for (const auto& [key, part] : g_original) {
            // ⚠ THE PLAYER'S ROWS ONLY. Apply refuses every other actor, so
            // today this is the whole map; asking anyway means a widening of
            // that rule cannot quietly start writing a follower's baseline
            // into the player's record.
            if (key.first != id) {
                continue;
            }
            // ⚠ "NOTHING" TRAVELS AS AN EMPTY NAME AND A ZERO ID, and it has to
            // travel: RestoreSlot wears the slot's placeholder for a null
            // capture now, and the reload path finds the staged part already
            // on the character and captures nothing, so without this row a
            // horn staged before one save could never be taken off after it.
            if (!part) {
                out.push_back(Capture{ key.second, std::string{}, 0u });
                continue;
            }
            StyleRefKey ref;
            if (!StyleRef::Make(part, ref)) {
                // A part with no defining file cannot be named in a way the
                // next load could resolve. Dropping it is the same answer
                // HairColor's baseline gives to the same question.
                continue;
            }
            out.push_back(Capture{ key.second, std::move(ref.modName), ref.localFormID });
        }
        return out;
    }

    void AdoptCaptures(const std::vector<Capture>& a_rows) {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        auto* const dh     = RE::TESDataHandler::GetSingleton();
        if (!player || !dh) {
            return;
        }
        const RE::FormID id = player->GetFormID();
        std::scoped_lock l(g_lock);
        for (const auto& row : a_rows) {
            // The "nothing" row SnapshotCaptures writes: an empty name and a
            // zero id, meaning they had no part in this slot before Fitting
            // Room. Adopted as the null capture it was, so RestoreSlot can wear
            // the placeholder for it after a load exactly as it would have
            // before the save.
            if (row.modName.empty() && row.localFormID == 0) {
                g_original.insert_or_assign({ id, row.slot }, nullptr);
                continue;
            }
            auto* const part = dh->LookupForm<RE::BGSHeadPart>(row.localFormID, row.modName);
            if (!part) {
                spdlog::warn("HeadPart: this save's baseline for slot {} names '{}'|{:06X}, "
                             "which no longer resolves. Dropping it: the character keeps "
                             "what they have on rather than being given a guess.",
                             row.slot, row.modName, row.localFormID);
                continue;
            }
            // insert_or_assign rather than try_emplace: this runs on a map the
            // revert has just emptied, and if anything did beat it here, the
            // save's own record is the better answer.
            g_original.insert_or_assign({ id, row.slot }, part);
        }
        spdlog::info("HeadPart: adopted {} baseline row(s) from this save.", a_rows.size());
    }

    SwitchSwapReport SwapSwitchedParts(
        RE::Actor* a_actor, const std::vector<RE::BGSHeadPart*>& a_preSwitchParts) {
        SwitchSwapReport report{};
        auto* const npc = a_actor ? a_actor->GetActorBase() : nullptr;
        if (!npc || a_actor != RE::PlayerCharacter::GetSingleton()) {
            return report;
        }
        // The same three gates the OS-230 route checks before it swaps, plus
        // the empty-snapshot refusal the header explains: an add with nothing
        // to detach stacks two characters' geometry.
        if (!LiveSwapAvailable() || !a_actor->GetFaceNodeSkinned() ||
            a_preSwitchParts.empty()) {
            return report;
        }
        report.available = true;
        // ⚠⚠ WHAT THE NODE ALREADY WEARS GATES EVERY SWAP, round eleven's
        // scar. With charGenRace committed before the switch the engine's
        // own build attaches the whole new head BEFORE this runs, and the
        // 12:33 head-watch read every part TWICE: the swap detached old
        // geometry that was not there and added new geometry that was.
        // Geometry is named by its part's editor id, so the child list IS
        // the ground truth of what needs moving: a new part whose name is
        // already on the node is kept, a swap whose old name is absent
        // degrades to an add, and a removal is only issued for a name the
        // node actually wears.
        std::vector<std::string> nodeNames;
        if (auto* const faceNode = a_actor->GetFaceNodeSkinned()) {
            for (const auto& child : faceNode->GetChildren()) {
                if (auto* const obj = child.get(); obj && obj->name.c_str()) {
                    nodeNames.emplace_back(obj->name.c_str());
                }
            }
        }
        const auto onNode = [&nodeNames](RE::BGSHeadPart* a_part) {
            const char* const edid = a_part ? a_part->GetFormEditorID() : nullptr;
            if (!edid || !*edid) {
                return false;
            }
            for (const auto& name : nodeNames) {
                if (_stricmp(name.c_str(), edid) == 0) {
                    return true;
                }
            }
            return false;
        };
        // ⚠⚠ A STABLE COPY OF THE NEW LIST, MAINS ONLY, BEFORE ANY ENGINE
        // CALL, and both halves of that are field scars from the same round
        // (05:2x, "113 swapped, 0 kept, 3 removed, 0 baked" from a
        // seventeen-part head). ChangeActorHeadPart's add half ends in
        // TESNPC::ChangeHeadPart, which WRITES npc->headParts: iterating
        // that array live while swapping meant every write shifted the
        // ground under the loop, and by the end the base's list was empty
        // and the next engine build had nothing to build. And the base list
        // carries EXTRA parts beside the mains; the engine call walks
        // old->extraParts and new->extraParts itself (decompiled, SE 26468),
        // so swapping an extra individually double-attaches what its main
        // already brought.
        // ⚠ Extra-ness via the owner index, not the flag: an unflagged
        // hairline in either list would otherwise pose as a kHair MAIN and
        // steal the type claim from the real hair (the 23:5x stacking round;
        // the index's own comment carries the measurement).
        const auto& ownerIndex = ExtraOwnerIndex();
        const auto  isMainPart = [&ownerIndex](RE::BGSHeadPart* a_part) {
            return a_part && !a_part->IsExtraPart() &&
                   !ownerIndex.contains(a_part);
        };
        std::vector<RE::BGSHeadPart*> newMains;
        for (std::int8_t i = 0; i < npc->numHeadParts; ++i) {
            auto* const part = npc->headParts[i];
            if (isMainPart(part)) {
                newMains.push_back(part);
            }
        }
        std::vector<RE::BGSHeadPart*> oldMains;
        for (auto* const part : a_preSwitchParts) {
            if (isMainPart(part)) {
                oldMains.push_back(part);
            }
        }
        // First-unclaimed-by-type matching: a list can carry two parts of one
        // type (misc rides beside the mains), and claiming keeps one old part
        // from serving two new ones.
        std::vector<bool> claimed(oldMains.size(), false);
        for (auto* const newPart : newMains) {
            RE::BGSHeadPart* old = nullptr;
            for (std::size_t j = 0; j < oldMains.size(); ++j) {
                if (!claimed[j] &&
                    oldMains[j]->type.underlying() == newPart->type.underlying()) {
                    claimed[j] = true;
                    old        = oldMains[j];
                    break;
                }
            }
            if (old == newPart || onNode(newPart)) {
                ++report.kept;
                continue;
            }
            ChangeActorHeadPart(a_actor, onNode(old) ? old : nullptr, newPart);
            ++report.swapped;
            // r60 owed the swap a NAME: "1 swapped" alone could not say which
            // feature the switched head traded.
            spdlog::info("HeadPart: switch swap {} '{}' -> '{}' (type {}).",
                         old && onNode(old) ? "replaced" : "added",
                         old ? old->GetFormEditorID() : "(nothing)",
                         newPart->GetFormEditorID(),
                         newPart->type.underlying());
        }
        // Old types the new list dropped (a beard on a character switching to
        // a beardless chargen set): detach-only, the null-guarded second half
        // of the engine call builds nothing. Only for geometry the node
        // actually wears; detaching a name that is not there is a no-op the
        // count would misreport as work.
        for (std::size_t j = 0; j < oldMains.size(); ++j) {
            if (!claimed[j] && onNode(oldMains[j])) {
                ChangeActorHeadPart(a_actor, oldMains[j], nullptr);
                ++report.removed;
                spdlog::info("HeadPart: switch removal '{}' (type {}).",
                             oldMains[j]->GetFormEditorID(),
                             oldMains[j]->type.underlying());
            }
        }
        // The swaps above replaced mains in the base list; any extra a
        // replaced main left in that list is now an orphan the bake right
        // after this call would otherwise regenerate forever (the r21
        // double hairline). Pruned before returning so the bake walks a
        // clean list.
        report.removed += static_cast<int>(PruneOrphanedExtras(a_actor));
        return report;
    }

}  // namespace OS::HeadPart
