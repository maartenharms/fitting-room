#include "SkinApi.h"

#include "MakeupApi.h"   // the tone the head holds, for the seam probe
#include "NpcIdentity.h"
#include "OverlayPlan.h"  // Rgb, the tone probe's type
#include "SkinPacks.h"
#include "SkinPlan.h"
#include "SkinRivals.h"  // kicked from the fit walk: the only place the paths exist

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// RaceMenu's public modder header uses opaque global Skyrim type declarations.
// Kept in this one translation unit and bridged at the call boundary, exactly
// as OverlayApi does; no RaceMenu type escapes through our header.
#include "../extern/RaceMenu/IPluginInterface.h"

// CreateFileW and GetFinalPathNameByHandleW, for naming the default skin: the
// PCH does not supply the Win32 vocabulary here (TextureLoad.cpp learned the
// same thing about SEH).
#include <Windows.h>

namespace OS::SkinApi {

    namespace {

        IOverrideInterface*  g_override{ nullptr };
        IActorUpdateManager* g_updates{ nullptr };
        Status               g_status{ Status::kNotRequested };

        // Which pack each actor wears and what was written for it. Read by the
        // attach observer on the game thread and by the page on the present
        // thread, so it sits under a lock; nothing holds it across a call into
        // skee or the engine.
        std::mutex                                                          g_lock;
        std::unordered_map<NpcKey, SkinPlan::ActorSkin, NpcKeyHash>         g_store;

        // skee's own numbers, the same ones OverlayPlan pins for the overlay
        // page: key 9 is a texture path and its index is the texture slot.
        constexpr std::uint16_t kKeyTexture = 9;
        constexpr std::uint8_t  kSlotCount  = 8;
        // skee's `renderedTexture` for kShaderType_FaceGen: the tint composite
        // the engine builds from the player's 34-slot list. MEASURED from
        // skee64 ShaderUtilities.cpp GetTextureFromIndex, 2026-08-18.
        constexpr std::uint8_t kSlotFaceTintComposite = 6;


        [[nodiscard]] TESObjectREFR* AsRaceMenuRef(RE::Actor* a_actor) {
            return reinterpret_cast<TESObjectREFR*>(a_actor);
        }
        [[nodiscard]] TESObjectARMO* AsRaceMenuArmor(RE::TESObjectARMO* a_armor) {
            return reinterpret_cast<TESObjectARMO*>(a_armor);
        }
        [[nodiscard]] TESObjectARMA* AsRaceMenuAddon(RE::TESObjectARMA* a_addon) {
            return reinterpret_cast<TESObjectARMA*>(a_addon);
        }
        [[nodiscard]] NiAVObject* AsRaceMenuObject(RE::NiAVObject* a_object) {
            return reinterpret_cast<NiAVObject*>(a_object);
        }

        [[nodiscard]] bool IsFemaleActor(RE::Actor* a_actor) {
            auto* const base = a_actor ? a_actor->GetActorBase() : nullptr;
            return base && base->IsFemale();
        }

        class SetString final : public IOverrideInterface::SetVariant {
        public:
            explicit SetString(std::string a_value) : m_value(std::move(a_value)) {}
            Type        GetType() override { return Type::String; }
            const char* String() override { return m_value.c_str(); }

        private:
            std::string m_value;
        };

        // ---- form identity, the same guard HairColor::Describe carries -------
        //
        // ⚠ NEVER GetLocalFormID UNGUARDED: it dereferences GetFile(0) with no
        // null check, and an armour created at run time has no file.
        struct FormRef {
            std::string   mod;
            std::uint32_t local{ 0 };
        };

        [[nodiscard]] std::optional<FormRef> Describe(RE::TESForm* a_form) {
            if (!a_form) {
                return std::nullopt;
            }
            auto* const file = a_form->GetFile(0);
            if (!file) {
                return std::nullopt;
            }
            return FormRef{ std::string{ file->GetFilename() }, a_form->GetLocalFormID() };
        }

        template <class T>
        [[nodiscard]] T* Resolve(const std::string& a_mod, std::uint32_t a_local) {
            if (a_mod.empty()) {
                return nullptr;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            return dh ? dh->LookupForm<T>(a_local, a_mod) : nullptr;
        }

        // ---- the geometry walk ------------------------------------------------

        struct SkinShape {
            RE::BSGeometry*                   geometry{ nullptr };
            RE::BSLightingShaderMaterialBase* material{ nullptr };
        };

        // Every textured geometry under a_object, and how many geometries there
        // are that are NOT skee overlay clones. The count feeds
        // SkinPlan::NodeKeys, which is why overlays are left out of it: skee
        // clones them into this same tree at attach and their number moves.
        //
        // ⚠⚠ a_skinFamilyOnly IS THE WHOLE DIFFERENCE BETWEEN THE TWO CALLERS,
        // AND A FIELD REPORT IS WHY IT EXISTS (2026-08-28: "when i wear armor
        // like iron banded armor, the skin can be different for the body, if we
        // had another skin selected ... when naked or using other amour it's
        // fine"). A body-covering armour supplies the body itself, and nothing
        // makes its author give that shape the skin shader. MEASURED on the
        // winning mesh, !UBE\Armor\Iron\F\CuirassHeavy_1.nif in overwrite:
        // five shapes, and the one called BaseShape carries
        // Textures\!UBE\Body\femalebody_1_d.dds on shader type 0, Default.
        // The naked body next to it is type 5, Skin_Tint. So a filter on the
        // skin family was silently dropping the very shape the skin belongs on,
        // and the character kept the mesh's baked body while her head and hands
        // changed.
        //
        // ⚠⚠ WIDENING IS SAFE HERE BECAUSE THE PACK IS THE REAL GATE. Both
        // painters ask SkinPlan::Match whether the pack has a file for the path
        // a slot currently holds, so an iron plate reading CuirassPlate.dds
        // matches nothing in a skin pack and is passed over. The shader test was
        // a second gate on the same question and it was the one that was wrong.
        //
        // ⚠ THE MEASURING CALLER KEEPS THE OLD STRICTNESS, deliberately. The
        // fit walk turns these shapes into "what this character's skin is made
        // of", which the rival scan then matches mods against; letting armour
        // texture names into that list would make a cuirass look like a skin.
        // The cost is that a fit or rival reading taken while such an armour is
        // worn still cannot see the body. That is a smaller wrong than the one
        // being fixed and it is not the reported bug.
        void Walk(RE::NiAVObject* a_object, std::size_t& a_realCount,
                  std::vector<SkinShape>& a_out, bool a_skinFamilyOnly) {
            if (!a_object) {
                return;
            }
            if (auto* const geom = a_object->AsGeometry()) {
                const char* name = geom->name.c_str();
                if (!SkinPlan::IsOverlayNode(name ? name : "")) {
                    ++a_realCount;
                }
                auto* const lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(
                    geom->GetGeometryRuntimeData()
                        .shaderProperty
                        .get());
                auto* const material =
                    lighting ? static_cast<RE::BSLightingShaderMaterialBase*>(lighting->material)
                             : nullptr;
                const bool family =
                    !a_skinFamilyOnly ||
                    (material &&
                     material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint);
                if (material && family && material->textureSet) {
                    a_out.push_back(SkinShape{ geom, material });
                }
                return;
            }
            if (auto* const node = a_object->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    Walk(child.get(), a_realCount, a_out, a_skinFamilyOnly);
                }
            }
        }

        // ⚠⚠ WHICH SLOTS, NOT JUST HOW MANY. The apply line counted rows and
        // never named them, so `2 moved` was equally consistent with a healthy
        // diffuse-and-normal pair and with a diffuse written twice into the
        // wrong pair of slots. FIELD 2026-08-26: "the normal map is not being
        // swapped", a question the counts could not answer either way. The slot
        // map is skee's own (ShaderUtilities.cpp GetTextureFromIndex, MEASURED
        // 2026-08-18): 0 diffuse, 1 normal, 2 subsurface, 3 detail, 6 the
        // rendered tint composite. So `0,1` is the healthy reading for a
        // two-file pack and anything else is a real bug that a plausible count
        // was hiding.
        //
        // ⚠ READ OFF THE STORE, which is the same list the reverse write walks,
        // so this reports what would actually be put back rather than what the
        // painter believed it did.
        template <class Rows>
        [[nodiscard]] std::string SlotList(const Rows& a_rows) {
            std::vector<std::uint8_t> slots;
            for (const auto& row : a_rows) {
                if (std::find(slots.begin(), slots.end(), row.slot) == slots.end()) {
                    slots.push_back(row.slot);
                }
            }
            std::sort(slots.begin(), slots.end());
            if (slots.empty()) {
                return "none";
            }
            std::string out;
            for (const auto slot : slots) {
                if (!out.empty()) {
                    out += ",";
                }
                out += std::to_string(static_cast<unsigned>(slot));
            }
            return out;
        }

        // ---- the one painter --------------------------------------------------

        // Add every override a_pack has for the skin shapes under a_object that
        // skee does not already hold, and ask skee to apply what was added.
        // Game thread. The store lock is NOT held on entry; this takes it for
        // the bookkeeping and releases it before calling into skee.
        //
        // Returns the number of overrides added.
        std::size_t PaintAddon(RE::Actor* a_actor, const NpcKey& a_key,
                               const SkinPlan::Pack& a_pack, RE::TESObjectARMO* a_armor,
                               RE::TESObjectARMA* a_addon, RE::NiAVObject* a_object) {
            const auto armorRef = Describe(a_armor);
            const auto addonRef = Describe(a_addon);
            if (!armorRef || !addonRef) {
                return 0;  // a runtime armour cannot be remembered, so it is not written
            }

            std::size_t            realCount = 0;
            std::vector<SkinShape> shapes;
            // Painting: every textured shape, because the pack decides. See Walk.
            Walk(a_object, realCount, shapes, false);
            if (shapes.empty()) {
                return 0;
            }

            auto* const refr   = AsRaceMenuRef(a_actor);
            const bool  female = IsFemaleActor(a_actor);
            auto* const armor  = AsRaceMenuArmor(a_armor);
            auto* const addon  = AsRaceMenuAddon(a_addon);

            std::size_t                    added = 0;
            std::vector<SkinPlan::Written> fresh;
            for (const auto& shape : shapes) {
                const char* geomName = shape.geometry->name.c_str();
                const auto  keys     = SkinPlan::NodeKeys(geomName ? geomName : "", realCount);
                for (std::uint8_t slot = 0; slot < kSlotCount; ++slot) {
                    const char* current = shape.material->textureSet->GetTexturePath(
                        static_cast<RE::BSTextureSet::Texture>(slot));
                    if (!current || !*current) {
                        continue;
                    }
                    const auto match = SkinPlan::Match(a_pack, current);
                    if (match.empty()) {
                        continue;
                    }
                    for (const auto& key : keys) {
                        // ⚠ skee's STORE IS THE TRUTH ABOUT "ALREADY WRITTEN",
                        // not this mod's memory. Its own OnAttach applied
                        // whatever it holds before this observer ran, so an
                        // override it holds is on the shape already, and one it
                        // does not hold is missing whatever the record says.
                        if (g_override->HasArmorOverride(refr, female, armor, addon,
                                                         key.c_str(), kKeyTexture, slot)) {
                            continue;
                        }
                        SetString value{ match };
                        g_override->AddArmorOverride(refr, female, armor, addon, key.c_str(),
                                                     kKeyTexture, slot, value);
                        ++added;
                        fresh.push_back(SkinPlan::Written{ armorRef->mod, armorRef->local,
                                                           addonRef->mod, addonRef->local, key,
                                                           slot, std::string{ current } });
                    }
                }
            }
            if (added == 0) {
                return 0;
            }

            {
                std::scoped_lock l(g_lock);
                auto&            row = g_store[a_key];
                for (auto& w : fresh) {
                    // A row this mod already remembers keeps its original: the
                    // path on the shape now may be a pack's, if skee's store
                    // and ours disagree after a partial load.
                    if (!SkinPlan::FindKey(row.written, w)) {
                        row.written.push_back(std::move(w));
                    }
                }
            }

            // ⚠ DEFERRED, NEVER IMMEDIATE, from inside an attach. skee's
            // immediate path loads the texture on the spot; the deferred one
            // queues its own task per value, which is what its own OnAttach
            // does with g_immediateArmor off.
            g_override->ApplyArmorOverrides(refr, armor, addon, AsRaceMenuObject(a_object), false);
            return added;
        }

        // ---- the head ---------------------------------------------------------
        //
        // The face node is the middle-high process's faceNodeSkinned, the same
        // pointer the eye pass in OutfitDye reads. Null when the actor is not
        // loaded high enough or the head has not been built yet, and both are
        // "nothing to paint".
        [[nodiscard]] RE::BSFaceGenNiNode* FaceNodeOf(RE::Actor* a_actor) {
            auto* const proc = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
            auto* const mid  = proc ? proc->middleHigh : nullptr;
            return mid ? mid->faceNodeSkinned : nullptr;
        }

        // Every named, lit geometry under the face node with a texture set. No
        // feature filter, because the pack's FILE NAMES are the filter: a hair
        // or an eye whose slot reads femalehead_d.dds does not exist, and a
        // face, a mouth or an overlay clone whose slot does is exactly what a
        // replacer on disk would have changed. Overlay clones are IN on
        // purpose: skee copies slots 1..8 from the face material into a face
        // overlay at every install, so the pack's normal registered on the clone
        // is what keeps the overlay carrying it after the next install
        // (skee-override-keys-and-overlay-normals).
        void WalkHead(RE::NiAVObject* a_object, std::vector<SkinShape>& a_out) {
            if (!a_object) {
                return;
            }
            if (auto* const geom = a_object->AsGeometry()) {
                const char* name     = geom->name.c_str();
                auto* const lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(
                    geom->GetGeometryRuntimeData()
                        .shaderProperty
                        .get());
                auto* const material =
                    lighting ? static_cast<RE::BSLightingShaderMaterialBase*>(lighting->material)
                             : nullptr;
                if (name && *name && material && material->textureSet) {
                    a_out.push_back(SkinShape{ geom, material });
                }
                return;
            }
            if (auto* const node = a_object->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    WalkHead(child.get(), a_out);
                }
            }
        }

        // Add every head override a_pack has for the actor's face node that skee
        // does not hold yet, and PAINT every one it has, held or fresh.
        //
        // ⚠⚠ PAINTS WHAT IT ALREADY HOLDS, WHICH THE BODY'S PAINTER MUST NOT
        // AND THIS ONE MUST. skee re-applies an armour override at every attach
        // of the addon, so a held armour override is on the shape by the time
        // the observer runs. A NODE override it re-applies at load and on
        // demand and never after the engine rebuilds a head (MEASURED, skee64
        // ActorUpdateManager::Flush and SKEEHooks.cpp: UpdateHeadState
        // reinstalls face OVERLAYS and touches nothing else). So the face that
        // comes out of a rebuild wears the NIF's own paths again with skee's
        // store still saying ours are on it, and this runs on the far side of
        // exactly those rebuilds (HeadBuildHook) to put them back.
        //
        // Game thread. Returns how many slots were painted; the fresh ones are
        // recorded so Default can take them off.
        std::size_t PaintHead(RE::Actor* a_actor, const NpcKey& a_key,
                              const SkinPlan::Pack& a_pack, std::size_t* a_added) {
            auto* const face = FaceNodeOf(a_actor);
            if (!face) {
                return 0;
            }
            std::vector<SkinShape> shapes;
            WalkHead(face, shapes);
            if (shapes.empty()) {
                return 0;
            }

            auto* const refr   = AsRaceMenuRef(a_actor);
            const bool  female = IsFemaleActor(a_actor);

            std::size_t                        added = 0, painted = 0;
            std::vector<SkinPlan::HeadWritten> fresh;
            std::string                        unmatchedDiffuse;
            for (const auto& shape : shapes) {
                const std::string name = shape.geometry->name.c_str();
                for (std::uint8_t slot = 0; slot < kSlotCount; ++slot) {
                    const char* current = shape.material->textureSet->GetTexturePath(
                        static_cast<RE::BSTextureSet::Texture>(slot));
                    if (!current || !*current) {
                        continue;
                    }
                    const auto match = SkinPlan::Match(a_pack, current);
                    if (match.empty()) {
                        // ⚠⚠ SLOT 0 ONLY, AND SAID OUT LOUD. A neck seam is the
                        // body taking a pack the face did not, and the counts
                        // cannot tell "this pack has no head file" from "it has
                        // one and the face reads a name nothing could match". A
                        // face baked by facegen or exported by a preset carries
                        // a name of its own, and no replacer is called that.
                        if (slot == 0 && unmatchedDiffuse.size() < 300) {
                            if (!unmatchedDiffuse.empty()) {
                                unmatchedDiffuse += "; ";
                            }
                            unmatchedDiffuse += "'" + name + "' reads '" + current + "'";
                        }
                        continue;
                    }
                    if (!g_override->HasNodeOverride(refr, female, name.c_str(), kKeyTexture,
                                                     slot)) {
                        ++added;
                        // The path the slot shows at the moment it is first
                        // written IS the original, exactly as the body's rows
                        // are recorded.
                        fresh.push_back(SkinPlan::HeadWritten{ name, slot, std::string{ current } });
                    }
                    // ⚠⚠ THE STORED OVERRIDE IS REFRESHED ON EVERY WRITE, not
                    // only the first. Field 2026-09-02 00:53: it was written
                    // once, so skee's store kept the FIRST pack forever, and
                    // its delayed re-bind a second after any tint write put
                    // Sayble 4K back over a head the user had just painted
                    // Siren. A hold must be refreshed by every writer of what
                    // it holds; the guard above now only decides whether the
                    // ORIGINAL is worth recording.
                    SetString value{ match };
                    g_override->AddNodeOverride(refr, female, name.c_str(), kKeyTexture,
                                                slot, value);
                    // ⚠ DEFERRED: skee queues one task per value, the same
                    // path its own load-time apply takes. Painting a value that
                    // is already on the shape reloads a texture the engine's
                    // cache already holds, which is the cost of not knowing
                    // whether a rebuild just happened.
                    SetString paint{ match };
                    g_override->SetNodeProperty(refr, false, name.c_str(), kKeyTexture, slot,
                                                paint, false);
                    ++painted;
                }
            }
            if (!fresh.empty()) {
                std::scoped_lock l(g_lock);
                auto&            row = g_store[a_key];
                for (auto& h : fresh) {
                    if (!SkinPlan::FindKey(row.head, h)) {
                        row.head.push_back(std::move(h));
                    }
                }
            }
            if (!unmatchedDiffuse.empty()) {
                spdlog::info("SkinApi: pack '{}' carries no file named for {} face diffuse "
                             "slot(s), which keep what they had: {}. A pack matches on FILE NAME, "
                             "so a face whose diffuse is a facegen bake or a preset's export can "
                             "never be matched by one and the body will change without it.",
                             a_pack.id, std::count(unmatchedDiffuse.begin(),
                                                   unmatchedDiffuse.end(), ';') + 1,
                             unmatchedDiffuse);
            }
            if (a_added) {
                *a_added = added;
            }
            return painted;
        }

        // ---- bare skin --------------------------------------------------------
        //
        // ⚠⚠ THE BODY HALF ONLY EVER REACHED WORN ARMOUR, AND A NAKED
        // CHARACTER HAS NONE. Field 2026-08-27: "when changing the skin now
        // only the head changes". Every apply that round read `0 moved, 0
        // removed, 0 added` on the body with `head 2 painted`, and the
        // dismember sweep beside it read `worn 0x0 drawn 0x0`. Nothing was
        // equipped, so PaintAddon had nothing to hang an override on, while
        // PaintHead walks the face geometry directly and went on working.
        //
        // The face solved this problem first and this is its answer moved one
        // limb down: bare skin belongs to no addon, so it goes through skee's
        // NODE channel, keyed by (actor, sex, geometry name), and carries the
        // head's row shape.
        //
        // ⚠⚠ A WORN CLONE IS SKIPPED WHOLE, SUBTREE AND ALL. A body that IS
        // worn is PaintAddon's through the ARMOUR channel, and two painters of
        // one appearance always drift. Stopping at the top of a biped part
        // clone rather than testing each geometry under it is what stops a
        // multi-shape garment being half-claimed by each channel. The face node
        // is skipped for the same reason: PaintHead owns it.
        //
        // ⚠ THE FEATURE FILTER IS THE BODY'S, NOT THE HEAD'S. A body is skin
        // tint (kFaceGenRGBTint) and a head is facegen; asking for the wrong
        // family here is a silent zero rather than an error, which is the whole
        // reason this walk mirrors Walk() and not WalkHead().
        void WalkBare(RE::NiAVObject*                                  a_object,
                      const std::unordered_set<const RE::NiAVObject*>& a_skip,
                      std::vector<SkinShape>&                          a_out) {
            if (!a_object || a_skip.contains(a_object)) {
                return;
            }
            if (auto* const geom = a_object->AsGeometry()) {
                const char* name = geom->name.c_str();
                // An overlay clone wears whatever skee copied onto it and is
                // not this character's skin; the head's walk takes them on
                // purpose and this one must not, or a pack would write the body
                // diffuse into a body overlay layer.
                if (!name || !*name || SkinPlan::IsOverlayNode(name)) {
                    return;
                }
                auto* const lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(
                    geom->GetGeometryRuntimeData()
                        .shaderProperty
                        .get());
                auto* const material =
                    lighting ? static_cast<RE::BSLightingShaderMaterialBase*>(lighting->material)
                             : nullptr;
                if (material &&
                    material->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGenRGBTint &&
                    material->textureSet) {
                    a_out.push_back(SkinShape{ geom, material });
                }
                return;
            }
            if (auto* const node = a_object->AsNode()) {
                for (const auto& child : node->GetChildren()) {
                    WalkBare(child.get(), a_skip, a_out);
                }
            }
        }

        std::size_t PaintBareSkin(RE::Actor* a_actor, const NpcKey& a_key,
                                  const SkinPlan::Pack& a_pack, std::size_t* a_added,
                                  std::size_t* a_shapes) {
            std::unordered_set<const RE::NiAVObject*> skip;
            for (int person = 0; person < 2; ++person) {
                auto* const biped = a_actor->GetBiped1(person == 1).get();
                if (!biped) {
                    continue;
                }
                for (std::uint32_t bit = 0; bit < 32; ++bit) {
                    if (auto* const clone = biped->objects[bit].partClone.get()) {
                        skip.insert(clone);
                    }
                }
            }
            if (auto* const face = FaceNodeOf(a_actor)) {
                skip.insert(face);
            }

            // ⚠ BOTH ROOTS, AND THE NAME IS WHAT DEDUPES THEM. Get3D's person
            // argument does not mean for the player what it means for anyone
            // else, so both are walked rather than reasoned about; a node
            // override is keyed by NAME, so first person and third person
            // sharing a geometry name is one write either way and writing it
            // twice would only reload a texture the cache already holds.
            std::vector<SkinShape>          shapes;
            std::unordered_set<std::string> seenNames;
            for (int person = 0; person < 2; ++person) {
                std::vector<SkinShape> found;
                WalkBare(a_actor->Get3D(person == 1), skip, found);
                for (const auto& shape : found) {
                    const char* name = shape.geometry->name.c_str();
                    if (name && *name && seenNames.insert(name).second) {
                        shapes.push_back(shape);
                    }
                }
            }
            if (a_shapes) {
                *a_shapes = shapes.size();
            }
            if (shapes.empty()) {
                return 0;
            }

            auto* const refr   = AsRaceMenuRef(a_actor);
            const bool  female = IsFemaleActor(a_actor);

            std::size_t                        added = 0, painted = 0;
            std::vector<SkinPlan::HeadWritten> fresh;
            for (const auto& shape : shapes) {
                const std::string name = shape.geometry->name.c_str();
                for (std::uint8_t slot = 0; slot < kSlotCount; ++slot) {
                    const char* current = shape.material->textureSet->GetTexturePath(
                        static_cast<RE::BSTextureSet::Texture>(slot));
                    if (!current || !*current) {
                        continue;
                    }
                    const auto match = SkinPlan::Match(a_pack, current);
                    if (match.empty()) {
                        continue;
                    }
                    if (!g_override->HasNodeOverride(refr, female, name.c_str(), kKeyTexture,
                                                     slot)) {
                        ++added;
                        // The path the slot shows the first time it is written
                        // IS the original, exactly as the armour and head rows
                        // record it.
                        fresh.push_back(SkinPlan::HeadWritten{ name, slot, std::string{ current } });
                    }
                    // Refreshed on every write, for PaintHead's 00:53 reason:
                    // a store that keeps the first pack hands it back at the
                    // next re-bind.
                    SetString value{ match };
                    g_override->AddNodeOverride(refr, female, name.c_str(), kKeyTexture,
                                                slot, value);
                    SetString paint{ match };
                    g_override->SetNodeProperty(refr, false, name.c_str(), kKeyTexture, slot,
                                                paint, false);
                    ++painted;
                }
            }
            if (!fresh.empty()) {
                std::scoped_lock l(g_lock);
                auto&            row = g_store[a_key];
                for (auto& b : fresh) {
                    if (!SkinPlan::FindKey(row.bare, b)) {
                        row.bare.push_back(std::move(b));
                    }
                }
            }
            if (a_added) {
                *a_added = added;
            }
            return painted;
        }

        // ---- the observer -----------------------------------------------------

        // Whether a stored row names this armour and this addon.
        [[nodiscard]] bool RowNames(const SkinPlan::Written& a_row, const FormRef& a_armor,
                                    const FormRef& a_addon) {
            return a_row.armorMod == a_armor.mod && a_row.armorLocal == a_armor.local &&
                   a_row.addonMod == a_addon.mod && a_row.addonLocal == a_addon.local;
        }

        class Observer final : public IAddonAttachmentInterface {
        public:
            void OnAttach(TESObjectREFR* a_refr, TESObjectARMO* a_armor, TESObjectARMA* a_addon,
                          NiAVObject* a_object, bool /*a_isFirstPerson*/, NiNode* /*a_skeleton*/,
                          NiNode* /*a_root*/) override {
                if (!a_refr || !a_armor || !a_addon || !a_object || !g_override) {
                    return;
                }
                auto* const ref   = reinterpret_cast<RE::TESObjectREFR*>(a_refr);
                auto* const actor = ref->As<RE::Actor>();
                if (!actor) {
                    return;
                }
                const auto key = NpcKeyFor(actor->GetActorBase());
                if (!key) {
                    return;
                }
                std::string packId;
                bool        holding = false;
                {
                    std::scoped_lock l(g_lock);
                    const auto       it = g_store.find(*key);
                    if (it == g_store.end()) {
                        return;
                    }
                    packId  = it->second.pack;
                    holding = !it->second.written.empty();
                    if (packId.empty() && !holding) {
                        return;
                    }
                }
                // ⚠⚠ A HELD ROW IS A CLEAR THAT COULD NOT LAND. No pack and
                // rows still standing means a clearing apply ran while this
                // addon was detached: the override came off, but the original
                // could not be written back onto a shape that was not there.
                // The addon is here now, so pay it back and let the row go.
                // Without this the clone comes back carrying the file the pack
                // put on it and nothing ever takes it off again.
                if (packId.empty()) {
                    const auto armorRef = Describe(reinterpret_cast<RE::TESObjectARMO*>(a_armor));
                    const auto addonRef = Describe(reinterpret_cast<RE::TESObjectARMA*>(a_addon));
                    if (!armorRef || !addonRef) {
                        return;
                    }
                    std::vector<SkinPlan::Written> owed;
                    {
                        std::scoped_lock l(g_lock);
                        auto&            rows = g_store[*key].written;
                        for (const auto& w : rows) {
                            if (RowNames(w, *armorRef, *addonRef)) {
                                owed.push_back(w);
                            }
                        }
                        std::erase_if(rows, [&](const SkinPlan::Written& a_w) {
                            return RowNames(a_w, *armorRef, *addonRef);
                        });
                    }
                    if (owed.empty()) {
                        return;
                    }
                    auto* const refr   = AsRaceMenuRef(actor);
                    auto* const armor  = AsRaceMenuArmor(
                        reinterpret_cast<RE::TESObjectARMO*>(a_armor));
                    auto* const addon  = AsRaceMenuAddon(
                        reinterpret_cast<RE::TESObjectARMA*>(a_addon));
                    const bool  female = IsFemaleActor(actor);
                    for (const auto& w : owed) {
                        if (w.original.empty()) {
                            continue;
                        }
                        g_override->RemoveArmorOverride(refr, female, armor, addon, w.node.c_str(),
                                                        kKeyTexture, w.slot);
                        SetString original{ w.original };
                        g_override->SetArmorProperty(refr, false, armor, addon, w.node.c_str(),
                                                     kKeyTexture, w.slot, original, false);
                        g_override->SetArmorProperty(refr, true, armor, addon, w.node.c_str(),
                                                     kKeyTexture, w.slot, original, false);
                    }
                    spdlog::info("SkinApi: '{}' attach of {:08X}/{:08X}: {} slot(s) put back to "
                                 "the file they had before a skin was cleared while this piece "
                                 "was off.",
                                 actor->GetName() ? actor->GetName() : "(unnamed)",
                                 reinterpret_cast<RE::TESObjectARMO*>(a_armor)->GetFormID(),
                                 reinterpret_cast<RE::TESObjectARMA*>(a_addon)->GetFormID(),
                                 owed.size());
                    return;
                }
                const auto  snap = SkinPacks::Get();
                const auto* pack = SkinPacks::Find(*snap, packId);
                if (!pack) {
                    return;  // this rig has no such pack; the page says so
                }
                const auto added = PaintAddon(actor, *key,
                                              *pack, reinterpret_cast<RE::TESObjectARMO*>(a_armor),
                                              reinterpret_cast<RE::TESObjectARMA*>(a_addon),
                                              reinterpret_cast<RE::NiAVObject*>(a_object));
                if (added != 0) {
                    spdlog::info("SkinApi: '{}' attach of {:08X}/{:08X}: {} slot(s) written from "
                                 "pack '{}'.",
                                 actor->GetName() ? actor->GetName() : "(unnamed)",
                                 reinterpret_cast<RE::TESObjectARMO*>(a_armor)->GetFormID(),
                                 reinterpret_cast<RE::TESObjectARMA*>(a_addon)->GetFormID(),
                                 added, packId);
                }
            }
        };

        Observer g_observer;
        bool     g_observing{ false };

        // ---- what is on the actor this second ----------------------------------

        [[nodiscard]] std::uint64_t PairKey(RE::TESObjectARMO* a_armor,
                                            RE::TESObjectARMA* a_addon) {
            return (static_cast<std::uint64_t>(a_armor->GetFormID()) << 32) | a_addon->GetFormID();
        }

        // Every (armour, addon) pair attached right now, one key each. Both
        // persons, because an armour override is not first-person scoped and a
        // row written from either rig describes the same slot.
        [[nodiscard]] std::unordered_set<std::uint64_t> WornPairs(RE::Actor* a_actor) {
            std::unordered_set<std::uint64_t> worn;
            for (int person = 0; person < 2; ++person) {
                auto* const biped = a_actor->GetBiped1(person == 1).get();
                if (!biped) {
                    continue;
                }
                for (std::uint32_t bit = 0; bit < 32; ++bit) {
                    const auto& obj   = biped->objects[bit];
                    auto* const armor = obj.item ? obj.item->As<RE::TESObjectARMO>() : nullptr;
                    if (!obj.partClone.get() || !armor || !obj.addon) {
                        continue;
                    }
                    worn.insert(PairKey(armor, obj.addon));
                }
            }
            return worn;
        }

        // Every skin-shape geometry name on the actor right now, the face and
        // the bare body alike. A node override is keyed by NAME, so this is the
        // question WornPairs asks of an armour row put to a node row: is the
        // thing this row describes here to be written on.
        [[nodiscard]] std::unordered_set<std::string> LiveNodeNames(RE::Actor* a_actor) {
            std::unordered_set<std::string> names;
            const auto take = [&](const std::vector<SkinShape>& a_shapes) {
                for (const auto& shape : a_shapes) {
                    const char* name = shape.geometry->name.c_str();
                    if (name && *name) {
                        names.insert(name);
                    }
                }
            };
            std::unordered_set<const RE::NiAVObject*> skip;
            for (int person = 0; person < 2; ++person) {
                if (auto* const biped = a_actor->GetBiped1(person == 1).get()) {
                    for (std::uint32_t bit = 0; bit < 32; ++bit) {
                        if (auto* const clone = biped->objects[bit].partClone.get()) {
                            skip.insert(clone);
                        }
                    }
                }
            }
            if (auto* const face = FaceNodeOf(a_actor)) {
                std::vector<SkinShape> shapes;
                WalkHead(face, shapes);
                take(shapes);
                skip.insert(face);
            }
            for (int person = 0; person < 2; ++person) {
                std::vector<SkinShape> found;
                WalkBare(a_actor->Get3D(person == 1), skip, found);
                take(found);
            }
            return names;
        }

        // ---- Apply's game-thread body ------------------------------------------

        void ApplyOnGameThread(RE::Actor* a_actor, const std::string& a_packId) {
            // ⚠ PROBE (field 2026-08-28): "when using UBE Natsuko v3 - CS
            // Advanced Skin [LM] i noticed i get a seam between the head and
            // body". The head diffuse DOES follow the pack, measured in the
            // 09:35 log, so the swap itself is working. What differs across the
            // neck is the TONE, and head and body take theirs from two different
            // fields with two different writers: MakeupApi holds a tone for the
            // head and re-bakes the face tint with it, while the body reads
            // TESNPC::bodyTintColor, which that path says in as many words it
            // never touches. If those two disagree at the moment a skin lands,
            // that is the seam and nothing further needs reading.
            //
            // ⚠ THE LOG ALREADY CARRIES THE OTHER HALF. AppearanceWatch prints
            // the head's bound diffuse and its tint texture around the same
            // moment, so this deliberately does not repeat them; put the two
            // lines side by side.
            //
            // ⚠ ONE LINE PER APPLY, not per frame: an apply is something a
            // player does by hand. Delete with the fix.
            {
                OverlayPlan::Rgb held{};
                float            strength = 0.0f;
                const bool       holds    = MakeupApi::HeldSkinTone(held, strength);
                const auto* const base    = a_actor->GetActorBase();
                if (base) {
                    spdlog::info(
                        "skin tone probe: pack '{}', head hold {} ({},{},{}) strength "
                        "{:.2f}, body bodyTintColor ({},{},{}).",
                        a_packId.empty() ? "(clearing)" : a_packId,
                        holds ? "HELD" : "none", held.r, held.g, held.b, strength,
                        base->bodyTintColor.red, base->bodyTintColor.green,
                        base->bodyTintColor.blue);
                }
            }
            const auto key = NpcKeyFor(a_actor->GetActorBase());
            if (!key) {
                spdlog::warn("SkinApi: '{}' has no persistable identity; skin not changed.",
                             a_actor->GetName() ? a_actor->GetName() : "(unnamed)");
                return;
            }
            // ⚠⚠ EnsureScanned, NOT Get. A named pack that the snapshot does
            // not hold has two possible meanings and they want opposite
            // answers: the pack is genuinely not installed, or nothing has
            // walked the folder yet this session. Get() cannot tell them apart
            // and used to report the first, so the very first look-import of a
            // session lost the skin it named while the log said the pack was
            // missing (field 2026-08-27). Only an EMPTY id skips this: clearing
            // the skin needs no pack list at all.
            const auto  snap = a_packId.empty() ? SkinPacks::Get() : SkinPacks::EnsureScanned();
            const auto* pack = a_packId.empty() ? nullptr : SkinPacks::Find(*snap, a_packId);
            if (!a_packId.empty() && !pack) {
                spdlog::warn("SkinApi: pack '{}' is not on this rig; skin not changed. The scan "
                             "saw {} pack(s) from {} file(s), so this is a missing pack rather "
                             "than an unread folder.",
                             a_packId, snap->packs.size(), snap->filesSeen);
                return;
            }

            auto* const refr   = AsRaceMenuRef(a_actor);
            const bool  female = IsFemaleActor(a_actor);

            // 1. Every override already written: move it to the new pack's
            //    file of the same name, or take it off and put the original
            //    back.
            std::vector<SkinPlan::Written> rows;
            std::string                    was;
            {
                std::scoped_lock l(g_lock);
                auto&            row = g_store[*key];
                was                  = row.pack;
                row.pack             = a_packId;
                rows                 = row.written;
            }
            // ⚠⚠ WHAT IS ATTACHED RIGHT NOW, MEASURED BEFORE THE LOOP RUNS. A
            // row whose addon is away has to survive the clear; see
            // SkinPlan::FateOf for what happened when it did not.
            const auto worn = WornPairs(a_actor);
            std::size_t moved = 0, removed = 0, held = 0, unresolved = 0;
            std::vector<SkinPlan::Written> kept;
            for (const auto& w : rows) {
                auto* const armorForm = Resolve<RE::TESObjectARMO>(w.armorMod, w.armorLocal);
                auto* const addonForm = Resolve<RE::TESObjectARMA>(w.addonMod, w.addonLocal);
                if (!armorForm || !addonForm) {
                    // The armour left the load order. There is nothing to
                    // remove from and nothing to paint; drop the row so it
                    // stops being carried.
                    ++unresolved;
                    continue;
                }
                auto* const armor = AsRaceMenuArmor(armorForm);
                auto* const addon = AsRaceMenuAddon(addonForm);
                const auto  fate =
                    SkinPlan::FateOf(pack, w, worn.contains(PairKey(armorForm, addonForm)));
                if (fate == SkinPlan::RowFate::Move) {
                    SetString value{ SkinPlan::Match(*pack, w.original) };
                    g_override->AddArmorOverride(refr, female, armor, addon, w.node.c_str(),
                                                 kKeyTexture, w.slot, value);
                    kept.push_back(w);
                    ++moved;
                    continue;
                }
                g_override->RemoveArmorOverride(refr, female, armor, addon, w.node.c_str(),
                                                kKeyTexture, w.slot);
                if (fate == SkinPlan::RowFate::Hold) {
                    // The addon is off the actor, so there is no shape to write
                    // the original onto and no honest way to say the row is
                    // finished with. The observer pays it back at the next
                    // attach; until then the row is the only thing that knows
                    // what this slot read before we touched it.
                    kept.push_back(w);
                    ++held;
                    continue;
                }
                ++removed;
                // ⚠ REMOVING A STORED VALUE REPAINTS NOTHING, the same trap
                // OverlayApi::Clear names, so the original goes straight onto
                // the shape if the addon is worn. Both persons, because an
                // armour override is not first-person scoped and the write is.
                // A no-op when the addon is not attached, which is the right
                // answer: the next attach loads the file the NIF names.
                if (!w.original.empty()) {
                    SetString original{ w.original };
                    g_override->SetArmorProperty(refr, false, armor, addon, w.node.c_str(),
                                                 kKeyTexture, w.slot, original, false);
                    g_override->SetArmorProperty(refr, true, armor, addon, w.node.c_str(),
                                                 kKeyTexture, w.slot, original, false);
                }
            }
            // 1b. The head rows, the same three ways: to the new pack's file of
            //     the same name, or off with the original written straight
            //     back onto the face. A node override is not first-person
            //     scoped and neither is the head, so one write.
            // ⚠ ONE MOVER FOR BOTH NODE LISTS. The face and the bare body are
            // the same three fields on the same channel and differ only in
            // which list they live on, so a second copy of this loop would be
            // two chances for them to drift about what a pack change means.
            // The counts stay apart because the log has to say which is which.
            // ⚠⚠ AND THE SAME HOLD THE ARMOUR ROWS GOT, because the head had
            // the identical wipe and only the body was fixed. Field 2026-08-27
            // r85, one line after the body half started working:
            // `head 0 moved, 9 removed ... over 0 worn addon(s)`, and every
            // apply after it read `head slots none`. A race switch rebuilds the
            // face, so the named geometry is gone for a moment and the clear
            // dropped nine rows onto nothing.
            const auto liveNames = LiveNodeNames(a_actor);
            const auto moveNodeRows = [&](std::vector<SkinPlan::HeadWritten> a_rows,
                                          std::size_t& a_moved, std::size_t& a_removed,
                                          std::size_t& a_held) {
                std::vector<SkinPlan::HeadWritten> kept_;
                for (const auto& h : a_rows) {
                    const auto fate =
                        SkinPlan::FateOf(pack, h.original, liveNames.contains(h.node));
                    if (fate == SkinPlan::RowFate::Move) {
                        SetString value{ SkinPlan::Match(*pack, h.original) };
                        g_override->AddNodeOverride(refr, female, h.node.c_str(), kKeyTexture,
                                                    h.slot, value);
                        SetString paint{ SkinPlan::Match(*pack, h.original) };
                        g_override->SetNodeProperty(refr, false, h.node.c_str(), kKeyTexture,
                                                    h.slot, paint, false);
                        kept_.push_back(h);
                        ++a_moved;
                        continue;
                    }
                    g_override->RemoveNodeOverride(refr, female, h.node.c_str(), kKeyTexture,
                                                   h.slot);
                    if (fate == SkinPlan::RowFate::Hold) {
                        // The geometry is not on the actor, so the original has
                        // nothing to be written onto and the row is all that
                        // knows it. PaintHead cannot re-derive it either: it
                        // reads the live shape, and there is no live shape.
                        kept_.push_back(h);
                        ++a_held;
                        continue;
                    }
                    ++a_removed;
                    if (!h.original.empty()) {
                        SetString original{ h.original };
                        g_override->SetNodeProperty(refr, false, h.node.c_str(), kKeyTexture,
                                                    h.slot, original, false);
                    }
                }
                return kept_;
            };

            std::vector<SkinPlan::HeadWritten> headRows, bareRows;
            {
                std::scoped_lock l(g_lock);
                headRows = g_store[*key].head;
                bareRows = g_store[*key].bare;
            }
            std::size_t headMoved = 0, headRemoved = 0, headHeld = 0;
            std::size_t bareMoved = 0, bareRemoved = 0, bareHeld = 0;
            auto headKept = moveNodeRows(std::move(headRows), headMoved, headRemoved, headHeld);
            auto bareKept = moveNodeRows(std::move(bareRows), bareMoved, bareRemoved, bareHeld);
            {
                std::scoped_lock l(g_lock);
                auto&            row = g_store[*key];
                row.written          = std::move(kept);
                row.head             = std::move(headKept);
                row.bare             = std::move(bareKept);
            }
            // Push every stored armour override the actor has onto what is
            // worn, which is how the moved ones land.
            if (moved != 0) {
                g_override->SetArmorProperties(refr, false);
            }

            // 2. Whatever the new pack has that nothing was written for yet, on
            //    every worn addon. The same painter the observer runs, so a
            //    pack chosen while dressed and a pack met at an attach agree.
            // ⚠⚠ THE BODY HALF ONLY EVER REACHES ARMOUR ADDONS, AND A FIELD
            // ROUND READ THAT AS THE PACK BEING BROKEN. 2026-08-27: "when
            // changing the skin now only the head changes". Every apply in that
            // round said `0 moved, 0 removed, 0 added ... head 2 painted`, and
            // the dismember sweep beside it said `worn 0x0 drawn 0x0`. Nothing
            // was equipped, so this walk had no armour to paint and the head
            // half, which walks the face geometry directly, went on working.
            //
            // ⚠ The store made it look intermittent rather than constant. Rows
            // written at an earlier attach MOVE to each new pack, so a
            // character who had once been dressed kept changing skin until
            // something emptied the store, and then stopped. What emptied it
            // was replace-on-apply's own `SkinApi::Apply(player, "")`.
            //
            // So the counts alone could not tell "nothing to paint" from
            // "painting failed", which is why the candidate count is measured
            // and said out loud rather than inferred from `added`.
            std::size_t added = 0, bodyCandidates = 0;
            if (pack) {
                std::unordered_set<std::uintptr_t> seen;
                for (int person = 0; person < 2; ++person) {
                    auto* const biped = a_actor->GetBiped1(person == 1).get();
                    if (!biped) {
                        continue;
                    }
                    for (std::uint32_t bit = 0; bit < 32; ++bit) {
                        const auto& obj    = biped->objects[bit];
                        auto* const object = obj.partClone.get();
                        auto* const armor  = obj.item ? obj.item->As<RE::TESObjectARMO>() : nullptr;
                        auto* const addon  = obj.addon;
                        if (!object || !armor || !addon) {
                            continue;
                        }
                        ++bodyCandidates;
                        // One armour addon covering two slots gives both bits
                        // the same clone; paint it once.
                        if (!seen.insert(reinterpret_cast<std::uintptr_t>(object)).second) {
                            continue;
                        }
                        added += PaintAddon(a_actor, *key, *pack, armor, addon, object);
                    }
                }
            }
            // 2b. And the head: what the pack has for the face that nothing
            //     was written for yet, plus a repaint of what was (see
            //     PaintHead for why the head paints what it holds).
            std::size_t headAdded = 0, headPainted = 0;
            if (pack) {
                headPainted = PaintHead(a_actor, *key, *pack, &headAdded);
            }
            // 2c. And the skin nobody is wearing: the body, hands and feet on a
            //     character with no armour over them, which the addon walk in
            //     step 2 cannot reach at all.
            std::size_t bareAdded = 0, barePainted = 0, bareShapes = 0;
            if (pack) {
                barePainted = PaintBareSkin(a_actor, *key, *pack, &bareAdded, &bareShapes);
            }

            // The slots the store ends up holding, so the counts above can be read
            // as a swap of specific textures rather than as a number of rows.
            std::string bodySlots, headSlots;
            {
                std::scoped_lock l(g_lock);
                auto&            row = g_store[*key];
                bodySlots            = SlotList(row.written);
                headSlots            = SlotList(row.head);
            }
            // ⚠ THE CANDIDATE COUNT IS ON THE LINE, not only in the sentence
            // below, or the two ways the body can come back empty look
            // identical. Zero candidates is "nothing is worn"; candidates with
            // nothing added is "the pack has no file of that name", and the
            // counts alone said the same thing for both.
            spdlog::info("SkinApi: '{}' skin '{}' -> '{}': {} moved, {} removed, {} held for a "
                         "detached addon, {} added over {} worn addon(s), {} unresolvable row(s) "
                         "dropped; head {} moved, {} removed, {} held for absent geometry, "
                         "{} added, {} painted. Body slots {}, head slots {} "
                         "(0 diffuse, 1 normal, 2 subsurface, 3 detail).",
                         a_actor->GetName() ? a_actor->GetName() : "(unnamed)",
                         was.empty() ? "(default)" : was.c_str(),
                         a_packId.empty() ? "(default)" : a_packId.c_str(), moved, removed, held,
                         added, bodyCandidates, unresolved, headMoved, headRemoved, headHeld,
                         headAdded, headPainted, bodySlots, headSlots);
            if (pack && (bareMoved || bareRemoved || bareHeld || bareAdded || barePainted ||
                         bareShapes)) {
                spdlog::info("SkinApi: bare skin, worn by no addon: {} moved, {} removed, "
                             "{} held for absent geometry, {} added, {} painted over {} shape(s).",
                             bareMoved, bareRemoved, bareHeld, bareAdded, barePainted, bareShapes);
            }
            // ⚠ SAID OUT LOUD, because the counts above cannot say it. A pack
            // that changes nothing at all is what the player sees, and the two
            // halves failing for two different reasons reads as one symptom.
            if (pack && bodyCandidates == 0 && moved == 0 && barePainted == 0 && bareMoved == 0) {
                spdlog::info(
                    "SkinApi: the body kept the skin it had. Nothing is worn for "
                    "the addon half to paint, and the bare walk found {} skin-tint "
                    "shape(s) outside the worn clones and the face.",
                    bareShapes);
            }
        }

        // ---- what fits, and what the default is called ------------------------

        std::unordered_map<NpcKey, FitInfo, NpcKeyHash> g_fit;  // under g_lock

        // The path a slot spells, as something CreateFile can open from the
        // game's working directory. Texture sets carry three spellings on this
        // rig alone: "data\TEXTURES\...", "textures\..." and a bare
        // "!UBE\...", and the engine takes all three.
        [[nodiscard]] std::string ToOpenable(std::string_view a_slotPath) {
            const auto low = SkinPlan::Lower(a_slotPath);
            if (low.rfind("data\\", 0) == 0) {
                return std::string{ a_slotPath };
            }
            if (low.rfind("textures\\", 0) == 0) {
                return "data\\" + std::string{ a_slotPath };
            }
            return "data\\textures\\" + std::string{ a_slotPath };
        }

        // Where the engine's own file open lands for this path. Under Mod
        // Organizer usvfs redirects the open to the real file, and the handle's
        // final path is that file's; without a VFS it is the same path back.
        // Empty when the file cannot be opened at all.
        [[nodiscard]] std::string ResolveRealPath(const std::string& a_openable,
                                                  bool a_directory) {
            // UTF-8 in, wide out, through the one converter this build already
            // links: a path.
            const std::filesystem::path wide{
                std::u8string_view{ reinterpret_cast<const char8_t*>(a_openable.data()),
                                    a_openable.size() }
            };
            // ⚠ A DIRECTORY CANNOT BE OPENED WITHOUT BACKUP SEMANTICS.
            // CreateFileW refuses one outright otherwise, and the skins folder
            // has to be resolved this way to learn which mod really supplies
            // it rather than assuming the name.
            HANDLE h = ::CreateFileW(wide.c_str(), GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING,
                                     a_directory ? FILE_FLAG_BACKUP_SEMANTICS
                                                 : FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                return {};
            }
            wchar_t    buf[1024]{};
            const auto n = ::GetFinalPathNameByHandleW(h, buf, static_cast<DWORD>(std::size(buf)),
                                                       FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            ::CloseHandle(h);
            if (n == 0 || n >= std::size(buf)) {
                return {};
            }
            std::wstring_view real{ buf, n };
            if (real.rfind(L"\\\\?\\", 0) == 0) {
                real.remove_prefix(4);
            }
            const auto u8 = std::filesystem::path{ real }.u8string();
            return std::string{ reinterpret_cast<const char*>(u8.data()), u8.size() };
        }

        void MeasureFitOnGameThread(RE::Actor* a_actor) {
            const auto key = NpcKeyFor(a_actor->GetActorBase());
            if (!key) {
                return;
            }
            FitInfo info;
            info.measured = true;
            std::unordered_set<std::string> seen;
            // ⚠ A SECOND SET, KEYED ON THE WHOLE PATH, because `seen` is keyed
            // on the LEAF NAME and two skin textures routinely share one:
            // female\femalebody_1.dds and male\femalebody_1.dds are the
            // ordinary case. Deduping the rival scan's paths by leaf would
            // throw away one of every such pair and hide half a skin.
            std::unordered_set<std::string> seenPaths;
            std::string                     bodyDiffuse;
            const auto take = [&](const std::vector<SkinShape>& a_shapes, bool a_body) {
                for (const auto& shape : a_shapes) {
                    for (std::uint8_t slot = 0; slot < kSlotCount; ++slot) {
                        const char* path = shape.material->textureSet->GetTexturePath(
                            static_cast<RE::BSTextureSet::Texture>(slot));
                        if (!path || !*path) {
                            continue;
                        }
                        if (a_body && slot == 0 && bodyDiffuse.empty()) {
                            bodyDiffuse = path;
                        }
                        const auto name = SkinPlan::FileName(path);
                        if (!name.empty() && seen.insert(name).second) {
                            info.names.push_back(name);
                        }
                        if (seenPaths.insert(path).second) {
                            info.paths.emplace_back(path);
                        }
                    }
                }
            };
            // The worn skin addons: every FaceGenRGBTint shape, body first so
            // the body's own diffuse is the one that names the default.
            if (auto* const biped = a_actor->GetBiped1(false).get()) {
                // Biped index 2 is the body slot (32); the rest in order.
                for (const std::uint32_t bit : { 2u, 3u, 7u, 0u, 1u, 4u, 5u, 6u, 8u, 9u,
                                                 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u,
                                                 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u,
                                                 28u, 29u, 30u, 31u }) {
                    auto* const object = biped->objects[bit].partClone.get();
                    if (!object) {
                        continue;
                    }
                    std::size_t            realCount = 0;
                    std::vector<SkinShape> shapes;
                    // Measuring: the skin family only. See Walk.
                    Walk(object, realCount, shapes, true);
                    take(shapes, bit == 2u);
                }
            }
            if (auto* const face = FaceNodeOf(a_actor)) {
                std::vector<SkinShape> shapes;
                WalkHead(face, shapes);
                take(shapes, false);
            }

            // The default's name: from the body's diffuse as the game gives it,
            // which is the recorded original while a pack is worn.
            //
            // ⚠⚠ THE LIVE SLOT IS ONLY A FALLBACK, AND IT CAN BE OUR OWN FILE.
            // The store's recorded original is the honest answer, but the store
            // is keyed on the pack being worn, and a clearing apply that runs
            // while nothing is equipped drops the rows without restoring the
            // slots they described (field 2026-08-27: `-> (default)` at 04:09:57
            // reported `8 removed over 0 worn addon(s)`). Re-equip after that
            // and the addons still read this plugin's staged file with no row
            // left to name what was under it, so `bodyDiffuse` hands back a
            // path inside our own skins folder and the default gets called
            // "Fitting Room". Everything downstream then measures a pack
            // against what we ourselves wrote: the guard below is the same rule
            // the rival list already applies a few lines down, applied to the
            // one path that was skipping it.
            std::string virtualPath = bodyDiffuse;
            {
                std::scoped_lock l(g_lock);
                // ⚠ THE ROWS ANSWER THIS, NOT THE PACK NAME. Gating on a pack
                // being worn used to skip the rows held across a detach, which
                // are the honest originals for a character mid-clear and the
                // one thing that keeps the page from blaming Mod Organizer for
                // a slot this plugin wrote. A store with no rows falls through
                // either way.
                if (const auto it = g_store.find(*key); it != g_store.end()) {
                    for (const auto& w : it->second.written) {
                        // ⚠ A ROW CAN BE POISONED TOO. If a slot was already
                        // reading one of our files when it was first recorded,
                        // the row learned that path as the original and is no
                        // more honest than the live slot. Skipping it lets a
                        // reload, which rebuilds the clone off the NIF, put a
                        // real path back where a stored lie would have won.
                        if (w.slot == 0 && !w.original.empty() &&
                            !SkinPlan::IsOwnFile(w.original)) {
                            virtualPath = w.original;
                            break;
                        }
                    }
                    // ⚠⚠ A WORN PACK HIDES EVERY PATH A RIVAL COULD SHARE, and
                    // the field log caught it: with Sayble 4K on, the live slots
                    // read
                    //   textures\FittingRoom\skins\Sayble 4K\!UBE\Body\...
                    // and no mod in the world carries that path, so the rival
                    // scan matched every skin mod against this plugin's own
                    // folder and found none of them.
                    //
                    // The originals are recorded exactly so this is answerable.
                    // Swapping them back in leaves `paths` describing the skin
                    // the LOAD ORDER gives this character, which is the only
                    // thing a rival can be compared against, while a slot the
                    // pack never touched keeps the live path it already had.
                    for (const auto& w : it->second.written) {
                        if (w.original.empty()) {
                            continue;
                        }
                        if (seenPaths.insert(w.original).second) {
                            info.paths.push_back(w.original);
                        }
                    }
                    // ⚠⚠ AND THE HEAD'S, WHICH IS A SECOND LIST. ActorSkin keeps
                    // `written` for the armour addons and `head` for the face,
                    // because the head is painted through node overrides rather
                    // than armour ones. Merging only the first cost the field a
                    // round: every auto-linked skin arrived with
                    // femalebody_1_d and femalebody_1_n and NO femalehead_d, so
                    // a new body turned up under the old face and the two did
                    // not match.
                    for (const auto& w : it->second.head) {
                        if (w.original.empty()) {
                            continue;
                        }
                        if (seenPaths.insert(w.original).second) {
                            info.paths.push_back(w.original);
                        }
                    }
                    // ⚠ AND THE BARE BODY'S, WHICH IS A THIRD. On a character
                    // wearing nothing this is the ONLY list holding a body
                    // path, so leaving it out would put the head's cost back on
                    // the body: every rival compared against a face and no
                    // torso.
                    for (const auto& w : it->second.bare) {
                        if (w.original.empty()) {
                            continue;
                        }
                        if (seenPaths.insert(w.original).second) {
                            info.paths.push_back(w.original);
                        }
                    }
                }
            }

            // The guard the comment above this block describes. A path under
            // our own skins folder is something we wrote, so it can never name
            // what the load order gives this character. Saying nothing is the
            // honest answer and the cards already draw that: an empty default
            // is "the game's own skin, unnamed", where a wrong one is a rival
            // list measured against a file we put there ourselves.
            if (!virtualPath.empty() && SkinPlan::IsOwnFile(virtualPath)) {
                spdlog::warn("SkinApi: the body slot for '{}' still reads this plugin's own file "
                             "'{}', and no stored row names what was under it, so the default "
                             "cannot be named from either. Picking a skin and clearing it while "
                             "the body is worn writes the row back.",
                             a_actor->GetName() ? a_actor->GetName() : "(unnamed)", virtualPath);
                virtualPath.clear();
                info.ownFileUnderneath = true;
            }

            // ⚠ AND THIS PLUGIN'S OWN FILES COME BACK OUT. Whatever the pack
            // put on the character lives under the skins folder; a rival is a
            // mod that supplies the path the pack REPLACED, never the
            // replacement.
            std::erase_if(info.paths, [](const std::string& a_path) {
                return SkinPlan::IsOwnFile(a_path);
            });
            if (!virtualPath.empty()) {
                const auto openable = ToOpenable(virtualPath);
                const auto real     = ResolveRealPath(openable, false);
                info.defaultPath    = virtualPath;
                info.defaultName    = SkinPlan::DefaultNameFromPath(real, virtualPath);
                // Once per actor: the three answers, so the field says which
                // one named the default (the mods folder, the family folder,
                // or nothing) without a second round.
                static std::unordered_set<std::string> told;
                if (told.insert(virtualPath).second) {
                    spdlog::info("SkinApi: default skin for '{}': slot reads '{}', the open "
                                 "resolves to '{}', so the default is called '{}'. {} live "
                                 "skin file name(s).",
                                 a_actor->GetName() ? a_actor->GetName() : "(unnamed)",
                                 virtualPath, real.empty() ? "(could not open)" : real,
                                 info.defaultName.empty() ? "(nothing)" : info.defaultName,
                                 info.names.size());
                }
            }
            // ⚠⚠ THE RIVAL SCAN IS KICKED FROM HERE BECAUSE THIS IS THE ONLY
            // PLACE THE PATHS EXIST. RequestFit is queued, so the page cannot
            // start the scan when it opens on a target: no slot has been read
            // yet and the list would be built from nothing. Starting it at the
            // end of the walk means it runs exactly once per measurement and the
            // page never polls for the data to turn up.
            auto paths = info.paths;
            auto body  = info.defaultPath;  // the body's own diffuse: what a skin IS
            {
                std::scoped_lock l(g_lock);
                g_fit[*key] = std::move(info);
            }
            SkinRivals::RequestScan(std::move(paths), std::move(body));
        }

        // ---- the head after a rebuild ------------------------------------------

        void ReapplyHeadOnGameThread(RE::Actor* a_actor) {
            const auto key = NpcKeyFor(a_actor->GetActorBase());
            if (!key) {
                return;
            }
            std::string packId;
            {
                std::scoped_lock l(g_lock);
                const auto       it = g_store.find(*key);
                if (it == g_store.end() || it->second.pack.empty()) {
                    return;
                }
                packId = it->second.pack;
            }
            const auto  snap = SkinPacks::Get();
            const auto* pack = SkinPacks::Find(*snap, packId);
            if (!pack) {
                return;  // this rig has no such pack; the page says so
            }
            std::size_t added   = 0;
            const auto  painted = PaintHead(a_actor, *key, *pack, &added);
            // Once at info so a field log can say the seam fired, then debug:
            // this runs after every head build for as long as a pack is worn.
            static bool s_said = false;
            if (!s_said && painted != 0) {
                s_said = true;
                spdlog::info("SkinApi: '{}' head repainted after a head build: {} slot(s) from "
                             "pack '{}' ({} newly written). This line existing at all is what "
                             "separates 'the seam never fired' from 'it fired and had nothing "
                             "to do'.",
                             a_actor->GetName() ? a_actor->GetName() : "(unnamed)", painted,
                             packId, added);
            } else {
                spdlog::debug("SkinApi: '{}' head repainted after a head build: {} slot(s) from "
                              "pack '{}' ({} newly written).",
                              a_actor->GetName() ? a_actor->GetName() : "(unnamed)", painted,
                              packId, added);
            }
        }

    }  // namespace

    void Request() {
        if (g_override && g_updates) {
            return;
        }
        auto* messaging = SKSE::GetMessagingInterface();
        if (!messaging) {
            g_status = Status::kNoMessaging;
            return;
        }
        InterfaceExchangeMessage exchange{};
        messaging->Dispatch(
            static_cast<std::uint32_t>(InterfaceExchangeMessage::kMessage_ExchangeInterface),
            &exchange, sizeof(exchange), "skee");
        if (!exchange.interfaceMap) {
            g_status = Status::kRaceMenuAbsent;
            spdlog::warn("SkinApi: RaceMenu interface exchange got no map; the skin changer "
                         "is unavailable this session.");
            return;
        }
        auto* overrides = static_cast<IOverrideInterface*>(
            exchange.interfaceMap->QueryInterface("Override"));
        if (!overrides) {
            g_status = Status::kNoOverride;
            spdlog::warn("SkinApi: RaceMenu has no Override interface.");
            return;
        }
        auto* updates = static_cast<IActorUpdateManager*>(
            exchange.interfaceMap->QueryInterface("ActorUpdateManager"));
        if (!updates) {
            g_status = Status::kNoUpdateManager;
            spdlog::warn("SkinApi: RaceMenu has no ActorUpdateManager interface, so nothing "
                         "could tell this mod when a body attaches.");
            return;
        }
        // The armour override calls are v1 of the Override interface and the
        // observer registration is v1 of the update manager; the floor is the
        // one OverlayApi already holds the same object to.
        if (overrides->GetVersion() < IOverrideInterface::kCurrentPluginVersion ||
            updates->GetVersion() < IActorUpdateManager::kPluginVersion1) {
            g_status = Status::kTooOld;
            spdlog::error("SkinApi: RaceMenu Override v{} / ActorUpdateManager v{} is older "
                          "than this build was written against; the skin changer is off.",
                          overrides->GetVersion(), updates->GetVersion());
            return;
        }
        g_override = overrides;
        g_updates  = updates;
        if (!g_observing) {
            g_updates->AddInterface(&g_observer);
            g_observing = true;
        }
        g_status = Status::kReady;
        spdlog::info("SkinApi: RaceMenu Override v{} and ActorUpdateManager v{} acquired; attach "
                     "observer registered.",
                     overrides->GetVersion(), updates->GetVersion());
    }

    Status GetStatus() { return g_status; }

    bool Available() { return g_status == Status::kReady; }

    const char* UnavailableKey() {
        switch (g_status) {
            case Status::kReady:
                return "";
            case Status::kNotRequested:
            case Status::kNoMessaging:
            case Status::kRaceMenuAbsent:
                return "$FR_Skin_NoRaceMenu";
            case Status::kNoOverride:
            case Status::kNoUpdateManager:
            case Status::kTooOld:
                return "$FR_Skin_OldRaceMenu";
        }
        return "$FR_Skin_NoRaceMenu";
    }

    std::string Current(RE::Actor* a_actor) {
        if (!a_actor) {
            return {};
        }
        const auto key = NpcKeyFor(a_actor->GetActorBase());
        if (!key) {
            return {};
        }
        std::scoped_lock l(g_lock);
        const auto       it = g_store.find(*key);
        return it == g_store.end() ? std::string{} : it->second.pack;
    }

    void Apply(RE::Actor* a_actor, const std::string& a_packId) {
        if (!Available() || !a_actor) {
            return;
        }
        const auto handle = a_actor->GetHandle();
        auto*      task   = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        task->AddTask([handle, packId = a_packId] {
            const auto ptr   = handle.get();
            auto*      actor = ptr ? ptr.get() : nullptr;
            if (!actor || !g_override) {
                return;
            }
            ApplyOnGameThread(actor, packId);
        });
    }

    namespace {
        // Every geometry under the player's face wearing a faceGen material,
        // by node name. That material is the ONE that carries the tint
        // composite.
        //
        // ⚠⚠ NOT WalkHead, AND THE DIFFERENCE IS NOT COSMETIC. WalkHead
        // collects kFaceGenRGBTint(5), the brows and mouth a skin pack
        // repaints; the head itself is kFaceGen(4) and is not in that list at
        // all. Reusing it here would have written the override onto shapes
        // that never read slot 6 and skipped the only shape that does, and the
        // whole feature would have logged success and changed nothing. The
        // late-rebake gate walks for kFaceGen for the same reason.
        void FaceGenHeadNodes(RE::NiAVObject* a_face, std::vector<std::string>& a_out) {
            RE::BSVisit::TraverseScenegraphGeometries(
                a_face, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                        a_geom->GetGeometryRuntimeData()
                            .shaderProperty
                            .get());
                    if (prop && prop->material &&
                        prop->material->GetFeature() ==
                            RE::BSShaderMaterial::Feature::kFaceGen) {
                        if (const char* n = a_geom->name.c_str(); n && *n) {
                            a_out.emplace_back(n);
                        }
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
        }
    }  // namespace

    bool ApplyFaceTintFile(RE::Actor* a_actor, const std::string& a_path) {
        if (!a_actor || a_path.empty() || !g_override) {
            return false;
        }
        auto* const face = FaceNodeOf(a_actor);
        if (!face) {
            return false;  // no head yet; the head-build seam calls again
        }
        std::vector<std::string> nodes;
        FaceGenHeadNodes(face, nodes);
        if (nodes.empty()) {
            return false;
        }
        auto* const refr   = AsRaceMenuRef(a_actor);
        const bool  female = IsFemaleActor(a_actor);
        for (const auto& name : nodes) {
            SetString value{ a_path };
            g_override->AddNodeOverride(refr, female, name.c_str(), kKeyTexture,
                                        kSlotFaceTintComposite, value);
            SetString paint{ a_path };
            g_override->SetNodeProperty(refr, false, name.c_str(), kKeyTexture,
                                        kSlotFaceTintComposite, paint, false);
        }
        // Once at info so a field log can say the seam fired at all, then
        // debug: this runs after every head build for as long as a look with a
        // baked face is worn.
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            spdlog::info("SkinApi: bound '{}' into the face tint composite on {} faceGen "
                         "geometry(s). This line existing at all is what separates 'the "
                         "export was never re-bound' from 'it was bound and something took "
                         "it off again'.",
                         a_path, nodes.size());
        } else {
            spdlog::debug("SkinApi: bound '{}' into the face tint composite on {} faceGen "
                          "geometry(s).",
                          a_path, nodes.size());
        }
        return true;
    }

    void ClearFaceTintFile(RE::Actor* a_actor) {
        if (!a_actor || !g_override) {
            return;
        }
        auto* const face = FaceNodeOf(a_actor);
        if (!face) {
            return;
        }
        std::vector<std::string> nodes;
        FaceGenHeadNodes(face, nodes);
        auto* const refr   = AsRaceMenuRef(a_actor);
        const bool  female = IsFemaleActor(a_actor);
        for (const auto& name : nodes) {
            g_override->RemoveNodeOverride(refr, female, name.c_str(), kKeyTexture,
                                           kSlotFaceTintComposite);
        }
    }

    void ReapplyHead(RE::Actor* a_actor) {
        if (!Available() || !a_actor || !g_override) {
            return;
        }
        ReapplyHeadOnGameThread(a_actor);
    }

    void RequestFit(RE::Actor* a_actor) {
        if (!a_actor) {
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
            if (actor) {
                MeasureFitOnGameThread(actor);
            }
        });
    }

    FitInfo Fit(RE::Actor* a_actor) {
        if (!a_actor) {
            return {};
        }
        const auto key = NpcKeyFor(a_actor->GetActorBase());
        if (!key) {
            return {};
        }
        std::scoped_lock l(g_lock);
        const auto       it = g_fit.find(*key);
        return it == g_fit.end() ? FitInfo{} : it->second;
    }

    std::vector<SkinRow> Snapshot() {
        std::vector<SkinRow> rows;
        std::scoped_lock     l(g_lock);
        rows.reserve(g_store.size());
        for (const auto& [key, skin] : g_store) {
            // A row with no pack and nothing written is an actor put back to
            // default with everything taken off: nothing to carry.
            if (skin.pack.empty() && skin.written.empty()) {
                continue;
            }
            rows.push_back(SkinRow{ key.modName, key.localFormID, skin });
        }
        return rows;
    }

    void Restore(std::vector<SkinRow> a_rows) {
        std::scoped_lock l(g_lock);
        g_store.clear();
        for (auto& row : a_rows) {
            g_store[NpcKey{ row.npcMod, row.npcLocal }] = std::move(row.skin);
        }
    }

    void Revert() {
        std::scoped_lock l(g_lock);
        g_store.clear();
        g_fit.clear();
    }

    // ---- the path edge, exposed ---------------------------------------------
    //
    // ⚠⚠ ONE OWNER, BECAUSE TWO READERS OF ONE ANSWER DRIFT IN THE GAP. The
    // rival scan needs exactly what the fit walk needs: a slot's spelling made
    // openable, and the real file a handle resolves to. Both were private here
    // and copying them into the scan would have left two parsers of the same
    // three texture-path spellings, one of which would eventually stop matching
    // the other. These forward to the originals rather than moving them, so the
    // field-proven code above is untouched.
    std::string OpenablePath(std::string_view a_slotPath) { return ToOpenable(a_slotPath); }

    std::string RealPathOf(const std::string& a_openable, bool a_directory) {
        return ResolveRealPath(a_openable, a_directory);
    }

}  // namespace OS::SkinApi
