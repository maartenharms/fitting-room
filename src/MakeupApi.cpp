#include "MakeupApi.h"

#include "LookFaceTint.h"  // Held/PathFor: which export is this character's own face
#include "MakeupBaseline.h"  // the record every write batch re-syncs
#include "ProfileApply.h"    // InFlight: the worn-set watch stands aside during an apply
#include "OverlayApi.h"    // ArmNodePropertyPush: a skin repaint takes the layers with it
#include "OverlayBake.h"   // OS-209 on makeup: Ensure and ReadSidecar are source agnostic
#include "RaceTint.h"
#include "Settings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OS::MakeupApi {

    namespace {

        // ---- the mask, measured ------------------------------------------------
        //
        // MEASURED in Ghidra on both builds, and written out here because
        // CommonLibSSE-NG has no definition to borrow. The evidence, so the next
        // reader does not have to take it on trust:
        //
        //   +0x10 type    GetTintMask and GetNumTints both compare this field to
        //                 their type argument while walking the list.
        //   +0x08 colour  the character dump prints it with %08X.
        //                 ⚠⚠ AND IT IS ABGR, NOT ARGB. RE::Color is four
        //                 BYTES, red, green, blue, alpha, so as one word this
        //                 reads 0xAABBGGRR while a .jslot on disk stores
        //                 0xAARRGGBB. Reading one as the other swaps red and
        //                 blue and leaves green alone, which is the field
        //                 report of 2026-08-16. Both forms and the converters
        //                 between them are in MakeupPlan.h; use the Engine
        //                 pair here and never PackColour.
        //   +0x0C alpha   the same dump prints it with %f, and the engine's own
        //                 colour setter writes both +0x08 and +0x0C together.
        //   +0x00 texture eight bytes ahead of the colour, and TESTexture is
        //                 0x10 bytes, so it cannot be embedded by value. A
        //                 pointer is the only thing that fits.
        struct TintMask {
            RE::TESTexture* texture;  // 00
            std::uint32_t   colour;   // 08 - 0xAABBGGRR, see the note above
            float           alpha;    // 0C
            std::uint32_t   type;     // 10
        };

        // ⚠ THE LIST OFFSET IS VERSION GATED AND THE PUBLISHED MEMBER IS NOT.
        // See the note on the header. Returns 0 for a runtime whose offset has
        // not been measured, which is a page that says it cannot work rather
        // than a page that reads a neighbouring field as an array pointer.
        [[nodiscard]] std::uintptr_t ListRoot(RE::PlayerCharacter* a_player) {
            if (!a_player) {
                return 0;
            }
            // ⚠ VR IS REFUSED RATHER THAN GUESSED. Its PlayerCharacter layout
            // diverges long before this offset and nothing here has been
            // measured against it. [[version-floor-must-not-key-on-header-current]]
            // is the rule: same declaration does not mean same layout.
            if (REL::Module::IsVR()) {
                return 0;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(a_player);
            return base + (REL::Module::IsAE() ? 0xB18u : 0xB10u);
        }

        // BSTArray is data at +0x00, capacity at +0x08 and size at +0x10. Both
        // builds' engine code reads exactly that pair off the base above.
        [[nodiscard]] TintMask** ListData(std::uintptr_t a_base) {
            return a_base ? *reinterpret_cast<TintMask***>(a_base) : nullptr;
        }

        [[nodiscard]] std::uint32_t ListSize(std::uintptr_t a_base) {
            return a_base ? *reinterpret_cast<std::uint32_t*>(a_base + 0x10) : 0u;
        }

        // ⚠⚠ AND THE PLAYER CARRIES TWO OF THESE LISTS, WHICH IS THE BUG THIS
        // FILE HELD FOR A WEEK. MEASURED in Ghidra on both builds: eight bytes
        // past the end of the array above sits a POINTER to a second array of
        // exactly the same shape, and the engine's own accessor prefers it
        // whenever it is set:
        //
        //   SE 1.5.97   FUN_140699020 (id 39339) } if (*(root + 0x18) != 0)
        //   AE 1.6.1170 FUN_14072cb20 (id 40410) }     root = *(root + 0x18);
        //
        // That is SKSE's `overlayTintMasks`, SE 0xB28 and AE 0xB30, and
        // FaceGen::RegenerateHead calls the accessor (SE 0x3D2DD4, AE
        // RegenerateHead+0x4a3), so THE OVERLAY IS WHAT THE FACE BAKES FROM.
        // Its installer is the neighbouring function, SE 39338 / AE 40409: it
        // frees the old overlay, deep copies an array of 0x18 byte masks into a
        // fresh one and hangs it here, or destroys it when handed null.
        //
        // ⚠⚠ A PLAYER RACE CHANGE GOES STRAIGHT THROUGH IT. TESNPC::SetRace
        // (SE FUN_14035e7d0 / AE FUN_1403b7300) destroys the overlay, paints the
        // BODY from the base list's skin tone slot, and then, for a race whose
        // flags carry 0xC000000, installs a copy of the OLD list with only the
        // skin tone taken from the new race. It never builds the new race's own
        // slots, which is why a 34 slot Nord list survives a switch to a 108
        // slot race. skee's preset apply then installs its own overlay carrying
        // the preset's tints.
        //
        // ⚠⚠ SO READING THE BASE ALONE IS WHAT THE FIELD SAW AS "an Umbrael
        // face over a Nord body with a seam at the neck": the face baked from
        // the overlay the preset installed, while every read and write here, and
        // the body sync that rides them, went to the stale base list. GetTintMask
        // reads the base too, which is why RaceMenu's own skin tone slot showed a
        // Nord's white beside a dark elf face.
        [[nodiscard]] std::uintptr_t OverlayList(std::uintptr_t a_root) {
            return a_root ? *reinterpret_cast<std::uintptr_t*>(a_root + 0x18) : 0u;
        }

        // Whether the last look found the overlay gone. Said on the EDGE only:
        // LiveList is called from the face watch, so a line per call would be a
        // line per frame. Starts false, so a session that begins already on the
        // base still announces itself on its first look.
        bool g_onBaseListSaid{ false };

        // The list the ENGINE would use. Every read and every write in this file
        // goes through here; nothing reaches past it to the base.
        //
        // ⚠⚠ THE FALLBACK TO THE BASE IS THE ONE THAT SHOWS ON A FACE, AND IT
        // WAS SILENT UNTIL 2026-09-01. When the overlay is gone this returns the
        // STALE BASE LIST and every read and write in this file lands there,
        // which is the state the note above measured as "a Nord's white beside a
        // dark elf face". The field reports of a head turning WHITE on a cell
        // change, sometimes wearing a vanilla war paint the player never set,
        // are that symptom exactly, and the fallback said nothing while it
        // happened. It says so now.
        //
        // ⚠ THIS LINE IS AN INSTRUMENT, NOT A FIX. It does not change which
        // list is used. Whether falling back is even wrong here is the open
        // question: the base is the right answer when no overlay was ever
        // installed, and the wrong one when the overlay was destroyed under us.
        // The log is what tells those apart on a reporter's machine.
        [[nodiscard]] std::uintptr_t LiveList(RE::PlayerCharacter* a_player) {
            const auto root    = ListRoot(a_player);
            const auto overlay = OverlayList(root);
            if (root) {
                const bool onBase = overlay == 0u;
                if (onBase != g_onBaseListSaid) {
                    g_onBaseListSaid = onBase;
                    if (onBase) {
                        spdlog::warn(
                            "Makeup: the tint OVERLAY list is gone, so every face read and "
                            "write now lands on the character's BASE list. That list is the "
                            "one measured as a Nord's white beside a dark elf face, and a "
                            "vanilla war paint the player never chose can come back with it. "
                            "If a head just turned white or grew a war paint, this line is why.");
                    } else {
                        spdlog::info("Makeup: the tint overlay list is back, so face reads and "
                                     "writes are on the character's own layers again.");
                    }
                }
            }
            return overlay ? overlay : root;
        }

        [[nodiscard]] RE::PlayerCharacter* PlayerFor(RE::Actor* a_actor) {
            auto* const player = RE::PlayerCharacter::GetSingleton();
            if (!player || !a_actor || a_actor != static_cast<RE::Actor*>(player)) {
                return nullptr;
            }
            return player;
        }

        [[nodiscard]] TintMask* MaskAt(RE::Actor* a_actor, std::size_t a_index) {
            auto* const player = PlayerFor(a_actor);
            const auto  base   = LiveList(player);
            auto* const data   = ListData(base);
            if (!data || a_index >= ListSize(base)) {
                return nullptr;
            }
            return data[a_index];
        }

        [[nodiscard]] std::string TexturePathOf(const TintMask* a_mask) {
            if (!a_mask || !a_mask->texture) {
                return {};
            }
            const char* const name = a_mask->texture->textureName.c_str();
            return name ? std::string{ name } : std::string{};
        }

        // ---- our own texture objects, so a write never lands in the race ----
        //
        // ⚠⚠ MEASURED 2026-08-16, census on the live character: 108 of 108
        // layers point AT THE RACE RECORD's OWN TESTexture. The engine builds
        // the player's list by aliasing the race's objects, so assigning a
        // texture path onto a mask edits the RACE and every character of it.
        // The first build refused the write, which was honest and useless: no
        // war paint could ever be picked. This is the useful version: the
        // first texture write to a layer swaps in a TESTexture WE allocated,
        // and from then on that layer's art is ours to change.
        //
        //   * One allocation per layer index, kept for the whole session in
        //     the pool below and reused by every later write to that layer.
        //     Bounded by the list length, never freed: the engine may hold the
        //     pointer at teardown and a leak of a few dozen 0x10 structs is
        //     the safe side of a use-after-free.
        //   * The vfptr is COPIED FROM THE RACE'S OWN OBJECT rather than taken
        //     from a class our DLL instantiated, so any virtual the engine
        //     calls lands in the engine's code, not in a table our module
        //     unloads with.
        //   * A write whose path equals the RACE's own texture restores the
        //     race's pointer instead. That is what Reset and an Undo back to
        //     arrival send, and it means "default" is the race's actual
        //     object again, not a copy that drifts.
        struct OwnedTexture {
            RE::TESTexture* object{ nullptr };
        };
        std::unordered_map<std::size_t, OwnedTexture> g_ownedTextures;

        [[nodiscard]] RE::TESTexture* OwnedTextureFor(std::size_t a_index,
                                                      const RE::TESTexture* a_template) {
            auto& slot = g_ownedTextures[a_index];
            if (slot.object) {
                return slot.object;
            }
            auto* const raw = RE::malloc(sizeof(RE::TESTexture));
            if (!raw) {
                return nullptr;
            }
            // vfptr from the engine's own object, name constructed in place.
            std::memcpy(raw, a_template, sizeof(void*));
            auto* const tex = static_cast<RE::TESTexture*>(raw);
            new (&tex->textureName) RE::BSFixedString();
            slot.object = tex;
            return tex;
        }

        // ⚠⚠ THE RETINT IS A REBAKE. RELOCATION_ID(51521, 52396), SE 0x8B40C0
        // and AE 0x954AE0, found by the string "Player face tint" which it
        // assigns as the name of the texture it builds. Call it ONCE after a
        // batch of writes and never per slider frame.
        //
        // ⚠ IT TAKES NO ARGUMENT AND THE CALL SITES PASS ONE ANYWAY. Ghidra
        // types it void(void) and the body reads the player singleton rather
        // than any parameter, while every caller hands it a pointer in RCX.
        // Declaring it as taking none is the honest reading of the body and both
        // forms work on x64, where a surplus register argument is simply
        // ignored.
        void Retint() {
            using func_t = void (*)();
            static REL::Relocation<func_t> func{ REL::RelocationID(51521, 52396) };
            func();
        }

        // ⚠⚠ THE ENGINE'S OWN FORMULA, NOT A COPY OF THE RGB. MEASURED in
        // Ghidra (TESNPC::SetSkinFromTint, RELOCATION_ID(24206, 24710)): the
        // body colour is tintRGB * alpha + (1 - alpha) * 0x80, per channel. The
        // alpha PARTICIPATES: turning the SkinTone strength down fades the body
        // toward mid grey exactly as the face fades toward its base texture. The
        // first version copied the rgb and dropped the alpha, which is the field
        // report "the face changes alpha but the body doesn't." RaceMenu's preset
        // apply calls this same function, and so does the engine's own race
        // change, off the BASE list's skin tone slot.
        //
        // ⚠ THE PLAYER'S ACTOR BASE IS THE PLAYER'S OWN unique record, so this
        // write shares nothing; the reason the FOLLOWER path must never edit a
        // base does not apply here.
        //
        // ⚠ GAME THREAD ONLY. Both callers are already inside a task.
        void PaintBodyFrom(RE::Actor* a_actor, TintMask* a_mask) {
            auto* const npc = a_actor ? a_actor->GetActorBase() : nullptr;
            if (!npc || !a_mask) {
                return;
            }
            RE::NiColorA result{};
            npc->SetSkinFromTint(&result, reinterpret_cast<RE::TintMask*>(a_mask), true);
            a_actor->UpdateSkinColor();
            // ⚠ AN INSTRUMENT, for the one-commit strip. The write and the
            // read-back in one line, because "the call ran" and "the field is
            // what we asked for" are different claims and r38 needed both.
            spdlog::info(
                "Makeup: body paint asked ({},{},{}) alpha={:.3f}; "
                "bodyTintColor now ({},{},{}).",
                a_mask->colour & 0xFFu, (a_mask->colour >> 8) & 0xFFu,
                (a_mask->colour >> 16) & 0xFFu, a_mask->alpha,
                npc->bodyTintColor.red, npc->bodyTintColor.green,
                npc->bodyTintColor.blue);
            // ⚠⚠ UpdateSkinColor REPAINTS THE OVERLAY CLONES TOO, because skee
            // gives them the skin's flags. Our layers keep their stored colours
            // and lose their painted ones, so the stored appearance is asked for
            // again once the repaints stop. See OverlayApi::ArmNodePropertyPush.
            OverlayApi::ArmNodePropertyPush(a_actor);
        }

        // The live list's skin tone layer, or null. ⚠ THE TYPE COMES FROM THE
        // RACE WHEN THE LENGTHS AGREE, for the same reason Layers() takes it
        // from there: a reloaded save reads every live type field as zero, and
        // zero is Freckles.
        [[nodiscard]] TintMask* SkinToneMaskIn(RE::Actor* a_actor) {
            auto* const player = PlayerFor(a_actor);
            const auto  live   = LiveList(player);
            auto* const data   = ListData(live);
            const auto  size   = ListSize(live);
            if (!data || size == 0) {
                return nullptr;
            }
            const auto raceSlots         = RaceTint::Slots(a_actor);
            const bool raceAuthoritative = !raceSlots.empty() && raceSlots.size() == size;
            for (std::uint32_t i = 0; i < size; ++i) {
                auto* const mask = data[i];
                if (!mask) {
                    continue;
                }
                const auto type = raceAuthoritative && raceSlots[i].known ? raceSlots[i].type
                                                                          : mask->type;
                if (type == static_cast<std::uint32_t>(MakeupPlan::Type::kSkinTone)) {
                    return mask;
                }
            }
            // ⚠⚠ BY ITS FILE NAME WHEN THE TYPES CANNOT SAY. A list that is not
            // this race's reads every type as zero after a reload, and the
            // loop above then finds no tone slot at all: field 2026-09-02
            // 02:58, the re-assert painted from a scratch mask every heartbeat
            // for the rest of the session. See MakeupPlan::IsSkinToneTexture.
            for (std::uint32_t i = 0; i < size; ++i) {
                auto* const mask = data[i];
                if (mask && mask->texture &&
                    MakeupPlan::IsSkinToneTexture(mask->texture->textureName.c_str())) {
                    return mask;
                }
            }
            return nullptr;
        }

        // Which of the two lists a read just used, for a log line that a field
        // round can act on. See the note on OverlayList: the two disagreeing is
        // the whole of the race switch bug, so the length of each belongs in the
        // record whenever something reports on this list.
        [[nodiscard]] std::string OriginPhrase(RE::Actor* a_actor) {
            auto* const player = PlayerFor(a_actor);
            const auto  root   = ListRoot(player);
            if (!root) {
                return "no tint list (this runtime's offset is not measured)";
            }
            const auto overlay = OverlayList(root);
            if (!overlay) {
                return fmt::format("the base tint list, {} layer(s), no overlay installed",
                                   ListSize(root));
            }
            return fmt::format(
                "the OVERLAY tint list, {} layer(s), over a base of {}", ListSize(overlay),
                ListSize(root));
        }

        // ⚠ A CENSUS, ONCE PER SESSION, AND IT ANSWERS A REAL WRITE HAZARD. The
        // texture is a POINTER, so two masks sharing one TESTexture would mean
        // setting one layer's art silently sets another's. Nothing measured says
        // whether the player's list is built with a TESTexture per mask or with
        // pointers into the race record, and the cheapest honest answer is to
        // count them the first time a character's list is read.
        //
        // ⚠ THIS IS NOT A FUSED PROBE AND MUST NOT BECOME ONE. It runs on page
        // open rather than on write, it logs one line, and the line names the
        // condition rather than dumping the list. If it ever reports a shared
        // pointer, the texture control needs to allocate rather than assign and
        // that is a build's worth of work, not a guess.
        bool g_censusDone{ false };

        // ⚠⚠ AND THE SECOND QUESTION, WHICH THE RACE READ MADE ASKABLE AND
        // WHICH IS THE LOUDER OF THE TWO. A race's tint slot owns a TESTexture
        // BY VALUE inside its TintAsset, at asset+0x08. If the engine builds
        // the player's list by pointing each mask at the race's own object
        // rather than at a copy, then writing a texture path onto a layer
        // writes it into the RACE RECORD, and every character of that race
        // wearing that slot changes with it, for the rest of the session.
        //
        // ⚠ NOTHING MEASURED SAYS WHICH IT IS. The static read answers what the
        // record holds, not what the list was built from, so this is asked of
        // the live character on page open and the answer goes in the log. It
        // costs one comparison per layer, once.
        void CensusTextures(RE::Actor* a_actor, const std::vector<TintMask*>& a_masks) {
            if (g_censusDone) {
                return;
            }
            g_censusDone = true;
            std::unordered_map<const void*, std::size_t> seen;
            std::size_t                                  shared = 0;
            std::size_t                                  absent = 0;
            for (const auto* mask : a_masks) {
                if (!mask) {
                    continue;
                }
                if (!mask->texture) {
                    ++absent;
                    continue;
                }
                if (++seen[mask->texture] == 2) {
                    ++shared;
                }
            }

            const auto  raceSlots = RaceTint::Slots(a_actor);
            std::size_t fromRace  = 0;
            for (std::size_t i = 0; i < a_masks.size() && i < raceSlots.size(); ++i) {
                if (a_masks[i] && a_masks[i]->texture &&
                    a_masks[i]->texture == raceSlots[i].textureObject) {
                    ++fromRace;
                }
            }

            spdlog::info(
                "Makeup: {} layers, {} race slots, {} with no TESTexture, {} texture objects "
                "shared by more than one layer, {} pointing AT THE RACE RECORD's own "
                "TESTexture.{}{}",
                a_masks.size(), raceSlots.size(), absent, shared, fromRace,
                shared != 0 ? " SHARED TEXTURES: setting one layer's art would change another's,"
                              " so the texture control must allocate rather than assign."
                            : "",
                fromRace != 0
                    ? " RACE OWNED TEXTURES: writing a texture path onto one of those layers"
                      " edits the race record and changes every character of that race."
                    : "");
        }

        [[nodiscard]] std::vector<TintMask*> CollectMasks(RE::Actor* a_actor) {
            std::vector<TintMask*> out;
            auto* const            player = PlayerFor(a_actor);
            const auto             base   = LiveList(player);
            auto* const            data   = ListData(base);
            const auto             size   = ListSize(base);
            if (!data) {
                return out;
            }
            out.reserve(size);
            for (std::uint32_t i = 0; i < size; ++i) {
                out.push_back(data[i]);
            }
            return out;
        }

    }  // namespace

    bool IsPlayer(RE::Actor* a_actor) { return PlayerFor(a_actor) != nullptr; }

    Status GetStatus(RE::Actor* a_actor) {
        auto* const player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return Status::kNoPlayer;
        }
        if (!IsPlayer(a_actor)) {
            return Status::kNotThePlayer;
        }
        return ListSize(LiveList(player)) == 0 ? Status::kEmptyList : Status::kReady;
    }

    std::vector<MakeupPlan::Layer> Layers(RE::Actor* a_actor) {
        std::vector<MakeupPlan::Layer> out;
        const auto                     masks = CollectMasks(a_actor);
        out.reserve(masks.size());
        for (std::size_t i = 0; i < masks.size(); ++i) {
            MakeupPlan::Layer layer{};
            layer.index = static_cast<std::uint32_t>(i);
            layer.type  = masks[i] ? masks[i]->type : 0u;
            out.push_back(layer);
        }
        // ⚠⚠ THE RACE IS THE TYPE AUTHORITY WHEN THE LIST MATCHES IT, because
        // the live type field does not survive a reload (field 2026-08-16:
        // every layer read type 0 after a save and reload). This retype used
        // to live only in the Makeup page's ReadMakeup, so the page showed
        // the right types while ProfileCapture stored zeroes off this same
        // call - both Umbrael captures of 2026-08-22 carry all-'freckles'
        // makeup blocks that the index+type match rule then rightly refuses
        // at apply. The fix lives HERE now, at the one source every reader
        // shares; the engine builds the list from the race in race order
        // (census 2026-08-16: 108 of 108 texture pointers matched the race
        // slot at the same index), so when the lengths agree each layer takes
        // its race slot's type, and when they do not, the live field, zeroes
        // and all, is the only truth available.
        const auto raceSlots = RaceTint::Slots(a_actor);
        if (raceSlots.size() == out.size()) {
            std::size_t retyped = 0;
            for (std::size_t i = 0; i < out.size(); ++i) {
                if (raceSlots[i].known && out[i].type != raceSlots[i].type) {
                    out[i].type = raceSlots[i].type;
                    ++retyped;
                }
            }
            // ⚠⚠ ONCE PER ANSWER, NOT ONCE PER CALL, AND THE BURST IS WHY.
            // This is a pure READER and AppearanceWatch samples through it, so
            // the line's cadence is the watcher's: 992 copies of it in one
            // sixty-second post-load burst on 2026-08-25, which buried the very
            // window the burst was widened to expose. Worse, it read like a
            // writer sitting next to the fault and cost a round being
            // eliminated by hand. It says something real the first time the
            // answer changes and nothing at all after that.
            if (retyped != 0) {
                static std::size_t s_lastSaid = 0;
                const std::size_t  key        = (retyped << 16) ^ out.size();
                if (key != s_lastSaid) {
                    s_lastSaid = key;
                    spdlog::info(
                        "Makeup: {} of {} layer types taken from the race record; "
                        "the live fields read differently, which is what a "
                        "reloaded save looks like. ⚠ THIS IS A READER, not a "
                        "writer: it retypes a local copy and returns it. Said "
                        "once per answer.",
                        retyped, out.size());
                }
            }
        }
        return out;
    }

    MakeupPlan::Snapshot Read(RE::Actor* a_actor) {
        MakeupPlan::Snapshot out;
        const auto           masks = CollectMasks(a_actor);
        CensusTextures(a_actor, masks);
        out.reserve(masks.size());
        for (const auto* mask : masks) {
            MakeupPlan::LayerState state{};
            if (mask) {
                state.texture    = TexturePathOf(mask);
                state.hasTexture = !state.texture.empty();
                // ⚠ A BAKED PATH IS DECODED BACK INTO ITS ART AND ITS TRANSFORM
                // (OS-209 on makeup), through the sidecar beside the file, the
                // same way OverlayApi::Read does it. Without this the page shows
                // a hash where the art's name belongs and reads every moved
                // layer as unmoved, so the next edit would resample the bake
                // instead of the source. A bake whose sidecar is gone stays as
                // it is: still a texture, still worn, its transform read as
                // identity. Said once per path so a page redraw does not repeat
                // it.
                if (OverlayTransform::IsBakedPath(state.texture)) {
                    const auto decoded = OverlayBake::ReadSidecar(state.texture);
                    if (decoded.ok) {
                        state.transform = decoded.transform;
                        state.texture   = decoded.source;
                    } else {
                        static std::unordered_set<std::string> s_said;
                        if (s_said.insert(state.texture).second) {
                            spdlog::warn(
                                "Makeup: '{}' is a baked layer with no sidecar beside it, so "
                                "its art and position cannot be read back; shown as it is.",
                                state.texture);
                        }
                    }
                }
                // ⚠⚠ THE ENGINE'S WORD, NOT THE PRESET'S. `RE::Color` is bytes
                // red, green, blue, alpha, so this field is ABGR while a .jslot
                // stores ARGB. Reading it the preset way swaps red and blue,
                // which is the field report of 2026-08-16.
                state.tint       = MakeupPlan::EngineTintFrom(mask->colour);
                // ⚠ THE FLOAT IS THE AUTHORITY AND THE PACKED BYTE IS NOT.
                // Both hold the strength and the engine writes both, but the
                // float is the one its own setter reads back, so a mask left
                // inconsistent by another mod reads here as whatever the engine
                // itself would use.
                state.strength = MakeupPlan::ClampStrength(mask->alpha);
            }
            out.push_back(std::move(state));
        }
        return out;
    }

    // ---- the write itself, on the game thread -------------------------------
    //
    // ⚠ SPLIT OUT OF Write SO THE BAKE CAN STAND IN FRONT OF IT (OS-209 on
    // makeup). Two callers: Write's fast path queues it as a task, and the bake
    // join below calls it straight, because OverlayBake::Ensure already
    // continues on the game thread. The body is unchanged and still re-checks
    // the actor and the ceiling, since either can have moved between the queue
    // and here.
    static void WriteNow(RE::ActorHandle a_handle, const std::vector<std::size_t>& a_indices,
                         const MakeupPlan::Snapshot& a_state) {
        const auto  handle  = a_handle;
        const auto& indices = a_indices;
        const auto& state   = a_state;
        {
            const auto  ptr   = handle.get();
            auto* const actor = ptr ? ptr.get() : nullptr;
            if (!actor || !IsPlayer(actor)) {
                return;
            }
            // ⚠ AND AGAIN ON THE GAME THREAD. The check above ran when the task
            // was queued; between then and here the player could have loaded a
            // save or changed race, and the list this writes into is not the one
            // that was counted. Cheap, and the thing it prevents is a CTD.
            if (!MakeupPlan::WithinCeiling(state)) {
                return;
            }
            // The race's slots, read once for the batch: they carry the object
            // a texture write must never touch, the name that means "back to
            // default", and the per-slot type the body sync below needs on a
            // list whose own types a reload zeroes.
            const auto raceSlots = RaceTint::Slots(actor);
            const bool raceAuthoritative =
                !raceSlots.empty() && raceSlots.size() == ListSize(LiveList(PlayerFor(actor)));
            bool      wrote    = false;
            TintMask* skinMask = nullptr;
            // The colour that layer is being SET to, kept beside the mask
            // because the hold below wants the value asked for rather than
            // whatever the packed field reads back as.
            OverlayPlan::Rgb skinTint{};
            float            skinStrength{ 1.0f };
            for (const auto index : indices) {
                if (index >= state.size()) {
                    continue;
                }
                auto* const mask = MaskAt(actor, index);
                if (!mask) {
                    continue;
                }
                const auto& want = state[index];
                // ⚠ BOTH HALVES OF THE STRENGTH, ALWAYS TOGETHER. The packed
                // colour's top byte and the float are one value in two fields
                // and the engine's own setter writes them in one breath.
                // Sending only one leaves the pair to drift, and the presets on
                // disk are dumps of these fields: a layer whose byte and float
                // disagree reads back differently depending on which the reader
                // trusts.
                mask->colour = MakeupPlan::PackEngineColour(want.tint, want.strength);
                mask->alpha  = MakeupPlan::ClampStrength(want.strength);
                // ⚠ THE TEXTURE IS ONLY TOUCHED WHEN THE PAGE HAS ONE. A layer
                // the player has not repainted keeps the art its race gave it,
                // and blanking that would empty a slot the player never asked
                // to empty.
                if (want.hasTexture && !want.texture.empty() && mask->texture) {
                    // ⚠⚠ AND THE RACE SLOT IS ONLY THIS LAYER'S SLOT WHEN THE
                    // TWO LISTS ARE THE SAME LENGTH. `index` counts the LIVE
                    // list; `raceSlots` counts the race record. With 34 live
                    // layers against 108 race slots, r35's pairing, this lookup
                    // named a stranger's slot and both branches below, the one
                    // that means "back to default" and the one that refuses to
                    // write through a race-owned pointer, were keyed on it.
                    // Layers() has held this guard since 2026-08-16 and this
                    // half was simply missed; unguarded, the write could put a
                    // texture path into the RACE RECORD.
                    const auto* const raceSlot =
                        raceAuthoritative && index < raceSlots.size() && raceSlots[index].known
                            ? &raceSlots[index]
                            : nullptr;
                    const auto* const raceObject =
                        raceSlot ? static_cast<const RE::TESTexture*>(raceSlot->textureObject)
                                 : nullptr;
                    // ⚠⚠ THE PATH THAT GOES ON THE MASK IS THE WIRE PATH, NOT
                    // THE SOURCE (OS-209 on makeup). They are the same string
                    // until the layer is moved, and then the wire is the baked
                    // file, whose existence Write has already waited for. Every
                    // branch below writes this one value, comparison included:
                    // a MOVED layer whose source happens to be the race's own
                    // art is NOT back at default, so it must not take the race
                    // object and lose its offset.
                    const auto wire = MakeupPlan::WirePath(want);
                    if (raceSlot && !raceSlot->texture.empty() &&
                        _stricmp(raceSlot->texture.c_str(), wire.c_str()) == 0) {
                        // ⚠ BACK TO DEFAULT IS THE RACE'S OWN OBJECT, not a
                        // copy carrying the same name. Reset and an Undo to
                        // arrival come through here.
                        if (raceObject && mask->texture != raceObject) {
                            mask->texture =
                                const_cast<RE::TESTexture*>(raceObject);
                        }
                    } else if (raceObject && mask->texture == raceObject) {
                        // ⚠⚠ NEVER WRITE THROUGH A RACE-OWNED POINTER. Swap in
                        // our own object instead; see OwnedTextureFor.
                        if (auto* const own = OwnedTextureFor(index, raceObject)) {
                            own->textureName = RE::BSFixedString{ wire.c_str() };
                            mask->texture    = own;
                        }
                    } else {
                        // The mask's texture is already not the race's: either
                        // our earlier swap or an object another mod gave the
                        // list. Writing its name is what every editor of this
                        // list does, RaceMenu included.
                        mask->texture->textureName = RE::BSFixedString{ wire.c_str() };
                    }
                }
                // ⚠ THE BODY IS PAINTED FROM A DIFFERENT FIELD AND WOULD NOT
                // FOLLOW. Field 2026-08-16: "skin tone only appears to change
                // the face skin color not the body color." The SkinTone tint
                // layer feeds the FACE bake; the body reads
                // TESNPC::bodyTintColor through Actor::UpdateSkinColor. Two
                // painters of one appearance, so a SkinTone edit writes both.
                // The type comes from the RACE when the list matches it,
                // because a reload zeroes every live type field.
                {
                    const auto type =
                        raceAuthoritative && index < raceSlots.size() && raceSlots[index].known
                            ? raceSlots[index].type
                            : mask->type;
                    if (type == static_cast<std::uint32_t>(MakeupPlan::Type::kSkinTone)) {
                        skinMask     = mask;
                        skinTint     = want.tint;
                        skinStrength = want.strength;
                    }
                }
                // ⚠⚠ READ BACK WHAT THE WRITE ACTUALLY LEFT, because a census
                // taken seconds later cannot tell "the name never took" apart
                // from "something wrote over it". FIELD 2026-08-23 (r33): at
                // +2.5 s and +6 s slot 12 carried FR's alpha (99, the captured
                // strength) beside the RACE's own `MaleHeadWarPaint_01.dds`,
                // on a female Dark Elf whose capture names a community overlay.
                // Colour landed and art did not, and this line is what says at
                // which end. ⚠ AN INSTRUMENT, for the one-commit strip.
                //
                // ⚠ IT READS BACK AGAINST THE WIRE PATH, because that is what
                // the write put there. Comparing a moved layer's mask against
                // its SOURCE would shout MISMATCH on every offset ever set, and
                // the instrument would be reporting the feature working as a
                // fault. The source is printed beside it so the line still says
                // which art the player picked.
                const auto wrote_ = MakeupPlan::WirePath(want);
                spdlog::debug(
                    "Makeup: wrote {:2d} alpha={:.3f} wanted='{}'{} -> mask now "
                    "'{}'{}",
                    index, want.strength,
                    want.hasTexture ? want.texture : "(kept)",
                    (want.hasTexture && !OverlayTransform::IsIdentity(want.transform))
                        ? std::format(" moved (x{:.3f} y{:.3f} s{:.3f} r{:.1f}) -> '{}'",
                                      want.transform.offsetX, want.transform.offsetY,
                                      want.transform.scale, want.transform.rotationDeg, wrote_)
                        : std::string{},
                    mask->texture ? TexturePathOf(mask) : "(no TESTexture)",
                    (want.hasTexture && mask->texture &&
                     _stricmp(TexturePathOf(mask).c_str(), wrote_.c_str()) != 0)
                        ? "  ⚠ MISMATCH"
                        : "");
                wrote = true;
            }
            // ⚠⚠ THE HELD TONE IS REFRESHED FIRST, AND A STALE ONE IS THE WHOLE
            // OF THE FIELD REPORT "the skin doesn't change color of the body, only
            // the head". MEASURED 2026-08-26 18:19: every colour picked on the
            // Overlays page painted the body once and was overwritten in the SAME
            // millisecond by the tone a look apply had held minutes earlier.
            //
            //   body paint asked (0,255,255) -> bodyTintColor (43,211,211)
            //   body paint asked (0,63,97)   -> bodyTintColor (43,85,107)
            //   skin tone re-asserted after a head build (0,63,97)
            //
            // The head kept the new colour because the retint below bakes the face
            // out of the mask and neither ReassertSkinTone nor WorldWatch's
            // heartbeat ever rebakes; they repaint the BODY alone. Two painters of
            // one appearance, drifting with nothing on screen saying why.
            //
            // ⚠ AND IT IS SET EVEN WHEN NOTHING WAS HELD BEFORE. Without a hold
            // the same edit is taken away instead of overwritten, by the next head
            // build restoring the character's own tint layers: one write into that
            // list never holds, which is the lesson HoldSkinTone was written for.
            // An edit of this layer IS Fitting Room's skin tone from here on,
            // Reset included, where the race default is exactly what should stick.
            if (skinMask) {
                HoldSkinTone(skinTint, skinStrength);
                spdlog::debug(
                    "Makeup: the skin tone edit becomes the held tone ({},{},{}) at "
                    "alpha {:.3f}, so the reassert has nothing older to put back.",
                    skinTint.r, skinTint.g, skinTint.b,
                    MakeupPlan::ClampStrength(skinStrength));
            }
            // ⚠ ONCE, AFTER THE WHOLE BATCH, AND ONLY IF SOMETHING LANDED. See
            // the note on Retint: this rebuilds a texture.
            if (wrote) {
                // ⚠⚠ A TONE EDIT REBUILDS THE FACE, AND FOR ONE ROUND IT DID
                // NOT. The exception that stood here skipped the retint when
                // the batch was the tone alone and the head wore an export,
                // which is exactly the edit whose whole point is that the head
                // follows the colour. Field 2026-08-27 r88, 06:45:25, the tone
                // set to (121,154,171) and the face left as it was, and the
                // user: "the head goes a different color or rather doesn't
                // update to the set color".
                //
                // ⚠ THE HEAD AND THE BODY ARE STILL TWO PAINTERS. This puts the
                // tone on the face; whether the two AGREE is a separate
                // question, and the neck is where the field reads the answer.
                Retint();
                // ⚠ AND WATCH FOR IT BEING TAKEN AWAY. The retint above is
                // correct and lands; what happens next is skee re-binding its
                // preset file over the slot about a second later. See
                // ArmDelayedFaceRebake.
                ArmDelayedFaceRebake();
            }
            if (skinMask) {
                PaintBodyFrom(actor, skinMask);
            }
            // The record follows every batch this choke point lands, so
            // skee's cosave is not the only copy of the list a save can
            // carry. See MakeupBaseline.h for the 00:14 load that taught it.
            MakeupBaseline::SyncFromList(actor);
            // And the character's SAVED layers follow it too, because the
            // character editor's init rebuilds the list from them and put a
            // previous character's makeup back on 2026-09-02. See
            // MakeupPlan::PlanSavedLayers for the measurement.
            SyncSavedLayers(actor, false);
            // And the worn-set watch learns this write was ours.
            NoteOwnWrite(actor);
        }
    }

    void Write(RE::Actor* a_actor, const std::vector<std::size_t>& a_indices,
               const MakeupPlan::Snapshot& a_state) {
        if (a_indices.empty() || !IsPlayer(a_actor)) {
            return;
        }
        auto* const task = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        const auto handle = a_actor->GetHandle();
        // ⚠⚠ THE CEILING IS CHECKED HERE AND NOT ONLY IN THE UI, because THIS
        // is the last frame before the engine. The crash of 2026-08-16 is an
        // access violation inside the retint itself, so a page that merely
        // declined to draw a control would still let a preset load, a target
        // switch or a future caller walk straight into it. Refusing the whole
        // batch is right rather than clamping it: a half applied batch leaves
        // the page and the character disagreeing, and the retint at the end of
        // it would still be the one that crashes.
        if (!MakeupPlan::WithinCeiling(a_state)) {
            spdlog::warn(
                "Makeup: REFUSED a write of {} layer(s). It would leave {} worn and the "
                "ceiling is {}. The face retint composites through a sixteen slot shader "
                "(TintMask0..15) and going past it crashes the game, so nothing is sent "
                "and no retint runs.",
                a_indices.size(), MakeupPlan::UsedIn(a_state), MakeupPlan::kMaxWornLayers);
            return;
        }

        // ⚠⚠ EVERY BAKE IN THE BATCH FINISHES BEFORE THE ONE RETINT (OS-209 on
        // makeup). A moved layer's path is the baked file, and putting that path
        // on a mask before the file exists makes the engine cache the
        // placeholder it substitutes and never see the file that arrives a
        // moment later. The overlays half has the same rule for key 9; what
        // differs here is that a makeup write is a BATCH ending in a single face
        // retint, so this cannot bake and write per layer. It counts the bakes
        // out and sends the whole batch when the last one lands.
        std::vector<std::size_t> needBake;
        for (const auto index : a_indices) {
            if (index >= a_state.size()) {
                continue;
            }
            const auto& want = a_state[index];
            if (want.hasTexture && !want.texture.empty() &&
                !OverlayTransform::IsIdentity(want.transform)) {
                needBake.push_back(index);
            }
        }
        if (needBake.empty()) {
            task->AddTask([handle, indices = a_indices, state = a_state] {
                WriteNow(handle, indices, state);
            });
            return;
        }

        // ⚠ THE JOIN IS SHARED AND THE CALLBACKS ARE ALL ON THE GAME THREAD, so
        // the counter needs no lock: OverlayBake::Ensure continues through
        // RunOnGameThread whether it hit, queued or refused.
        struct Join {
            std::vector<std::size_t> indices;
            MakeupPlan::Snapshot     state;
            std::size_t              waiting{ 0 };
        };
        auto join     = std::make_shared<Join>();
        join->indices = a_indices;
        join->state   = a_state;
        join->waiting = needBake.size();
        for (const auto index : needBake) {
            const auto source = a_state[index].texture;
            OverlayBake::Ensure(
                source, a_state[index].transform, [handle, join, index, source](bool a_ok) {
                    if (!a_ok) {
                        // ⚠ A FAILED BAKE FALLS BACK TO THE SOURCE, the same
                        // answer the overlays half gives: the art shows where
                        // its author drew it rather than as a placeholder. The
                        // PAGE keeps its numbers; only the path written out
                        // loses them, so a later edit bakes again rather than
                        // silently forgetting where the player put it.
                        spdlog::warn(
                            "Makeup: the bake for '{}' on layer {} could not be made, so the "
                            "layer is written at the art as authored.",
                            source, index);
                        if (index < join->state.size()) {
                            join->state[index].transform = OverlayTransform::Transform{};
                        }
                    }
                    if (--join->waiting == 0) {
                        WriteNow(handle, join->indices, join->state);
                    }
                });
        }
    }

    void SyncBodyToSkinTone(RE::Actor* a_actor) {
        if (!IsPlayer(a_actor)) {
            return;
        }
        auto* const task = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        const auto handle = a_actor->GetHandle();
        task->AddTask([handle] {
            const auto  ptr   = handle.get();
            auto* const actor = ptr ? ptr.get() : nullptr;
            if (!actor || !IsPlayer(actor)) {
                return;
            }
            // ⚠⚠ A LIST THAT IS NOT THIS RACE'S IS NOT A COLOUR SOURCE, and
            // this guard is the r37 field's own lesson. The first build of this
            // sync read the live list unconditionally, and on a Nord switched to
            // a 108 slot race that list is still the Nord's 34: it painted the
            // body the OLD race's white at full strength, which is the pale body
            // under an Umbrael head. Standing aside leaves the body as it was,
            // which is no worse, and the caller with a captured skin tone should
            // be using PaintBodyFromTint instead.
            const auto raceSlots = RaceTint::Slots(actor);
            const auto liveSize  = ListSize(LiveList(PlayerFor(actor)));
            if (!raceSlots.empty() && raceSlots.size() != liveSize) {
                spdlog::warn(
                    "Makeup: the body sync STOOD ASIDE. {} and this race's "
                    "record carries {}, so its skin tone belongs to another "
                    "race and painting the body from it would spread the "
                    "mismatch.",
                    OriginPhrase(actor), raceSlots.size());
                return;
            }
            auto* const mask = SkinToneMaskIn(actor);
            if (!mask) {
                spdlog::info(
                    "Makeup: the body sync found no skin tone layer in {}, so "
                    "the body keeps the colour it has.",
                    OriginPhrase(actor));
                return;
            }
            PaintBodyFrom(actor, mask);
            spdlog::info(
                "Makeup: the body takes its colour from the skin tone layer of "
                "{} (alpha {:.3f}).",
                OriginPhrase(actor), mask->alpha);
        });
    }

    void PaintBodyFromTint(RE::Actor* a_actor, const OverlayPlan::Rgb& a_tint,
                           float a_strength) {
        if (!IsPlayer(a_actor)) {
            return;
        }
        auto* const task = SKSE::GetTaskInterface();
        if (!task) {
            return;
        }
        const auto handle = a_actor->GetHandle();
        task->AddTask([handle, tint = a_tint, strength = a_strength] {
            const auto  ptr   = handle.get();
            auto* const actor = ptr ? ptr.get() : nullptr;
            if (!actor || !IsPlayer(actor)) {
                return;
            }
            // ⚠⚠ THE LIVE SKIN TONE SLOT IS WRITTEN FIRST, AND THAT IS THE
            // WHOLE OF WHY r38 LOOKED UNCHANGED. Painting bodyTintColor alone
            // does not hold: SetSkinFromTint handed a NULL mask walks the LIVE
            // list for a type 6 slot and paints the body from THAT (measured in
            // the decompile, the param_3 == 0 branch), and something re-derives
            // it after every apply. With the live list still the old race's, the
            // re-derive found that race's pale skin tone at full strength and
            // put it straight back. The r38 field body reads (167,134,122),
            // which is exactly what the probe printed in slot 0.
            //
            // ⚠ ONE SLOT, FOUND BY TYPE, COLOUR AND ALPHA ONLY. This is not the
            // capture-by-index write that made a mess of the war paint: a skin
            // tone slot means the same thing in every race's list, the texture
            // is left exactly as it was, and nothing else in the list is
            // touched. It is the one value the engine will keep re-reading.
            //
            // ⛔ AND NO RETINT. Rebaking the face from a list that still belongs
            // to another race would throw away the face skee's preset apply just
            // painted, which is the half that IS right.
            TintMask  scratch{};
            TintMask* live = SkinToneMaskIn(actor);
            TintMask* use  = live;
            if (live) {
                live->colour = MakeupPlan::PackEngineColour(tint, strength);
                live->alpha  = MakeupPlan::ClampStrength(strength);
            } else {
                // No slot to hold it, so the paint is one-shot: SetSkinFromTint
                // reads +0x08 and +0x0C off the mask handed in and never touches
                // the texture pointer, so a scratch carries the colour.
                scratch.texture = nullptr;
                scratch.colour  = MakeupPlan::PackEngineColour(tint, strength);
                scratch.alpha   = MakeupPlan::ClampStrength(strength);
                scratch.type    = static_cast<std::uint32_t>(MakeupPlan::Type::kSkinTone);
                use             = &scratch;
            }
            PaintBodyFrom(actor, use);
            // The slot write above is ours, so the worn-set watch must not
            // read it as a foreign writer: field 2026-09-02 03:44:06, the tone
            // landing at 167 was called "not ours" and answered.
            if (live) {
                NoteOwnWrite(actor);
            }
            // Held, so the head build that lands a second later puts it back
            // instead of taking it away. See the note on HoldSkinTone.
            HoldSkinTone(tint, strength);
            spdlog::info(
                "Makeup: the body takes the CAPTURED skin tone ({},{},{}) at "
                "alpha {:.3f}, written into {} of {}.",
                tint.r, tint.g, tint.b, MakeupPlan::ClampStrength(strength),
                live ? "the live skin tone slot" : "a scratch mask (no live slot)",
                OriginPhrase(actor));
        });
    }

    // ---- the held skin tone -------------------------------------------------
    //
    // ⚠⚠ A HEAD BUILD PUTS THE TINT LIST BACK AND THE BODY WITH IT. MEASURED
    // r39, and this is the writer three rounds went looking for. The captured
    // tone lands in the live skin tone slot and in bodyTintColor, and both hold
    // at +0.25 s, +0.5 s and +1 s: alpha 167, body (43,85,107). A head build
    // fires at +1.2 s, and at +2 s the slot reads alpha 255 again and the body
    // is back to (167,134,122). The list is restored from the character's own
    // saved tint layers, so every write into it is on borrowed time.
    //
    // ⚠ SO IT IS RE-ASSERTED, NOT FOUGHT. Same shape as the hair colour, which
    // a head build wipes for the same reason and which HeadBuildHook has put
    // back since July. Session scope: a load restores the character's own
    // colour and clears this, and the RaceSex menu owns the colour while it is
    // open.
    namespace {
        bool             g_holdSkin{ false };
        bool             g_rebakeOwed{ false };
        std::chrono::steady_clock::time_point g_rebakeWindowUntil{};
        OverlayPlan::Rgb g_holdTint{};
        float            g_holdStrength{ 1.0f };
        // The delayed rebake's whole state. A zero due-time means nothing is
        // armed, which is the case almost always and costs one comparison a tick.
        std::chrono::steady_clock::time_point g_faceCheckDueAt{};
        int                                   g_faceChecksLeft{ 0 };
        // ⚠ THE STAND-DOWN, AND IT IS A DEADLINE RATHER THAN A FLAG. A look
        // apply keeps rebuilding the head after it reports complete, so a flag
        // cleared at the end of the apply would be cleared too early; a deadline
        // outlives the whole settle without anyone having to find its end.
        std::chrono::steady_clock::time_point g_faceWatchQuietUntil{};
        // Said once per hold, not once per write. A slider drag arms this
        // dozens of times and the decline is the same answer every time.
        bool                                  g_faceWatchNoHoldSaid{ false };
        // Which edge armed the watch, because the two answer to different
        // rules. A write earns a re-bake only while we hold this character's
        // colour; a head build earns one regardless, since the composite it
        // threw away was the character's own.
        bool                                  g_faceCheckFromBuild{ false };
        // ⚠⚠ THIS HOLD HAS NO IDENTITY ON IT AND CANNOT USEFULLY BE GIVEN ONE.
        // The blue body (field 2026-08-24) came from the hold outliving the
        // character it was captured from: `coc qasmoke` from the main menu
        // delivers neither kNewGame nor kPostLoadGame, the only release lived
        // in that handler, and the next character's first head build re-asserted
        // the last one's tone onto their body.
        //
        // The obvious second guard, an identity term compared at re-assert
        // time, was written and then removed. There is nothing to compare: the
        // player is the same ref and the same base form in every save, so a
        // form id, a pointer and an IsPlayer check all answer the same for a
        // character this tone never belonged to. A generation counter is worse
        // than nothing, because it would have to be bumped by the same revert
        // event that already releases the hold, so it could only ever fire when
        // the release had fired too.
        //
        // What actually discriminates one playthrough from another here is
        // Persistence's revert callback, which is measured to run on BOTH
        // paths. That is where the release belongs and where it now is. If this
        // ever bleeds again, the question is which event was missed, not which
        // field was unchecked.
    }

    // The name of the texture on the facegen head's tint slot, or empty when
    // there is no facegen head to read. Same walk AppearanceWatch does for its
    // HEAD TEXTURE line, which is where the measurement came from.
    //
    // ⚠ THE FIRST FACEGEN SHAPE AND THEN STOP, exactly as that instrument does:
    // the mouth is facegen too and its tint never moves, so walking on would
    // sometimes answer about the wrong shape.
    namespace {
        // a_found separates "there is no facegen head" from "there is one and its
        // tint texture has no name". The caller must not treat those alike.
        [[nodiscard]] std::string HeadTintTextureName(RE::Actor* a_actor,
                                                      bool*      a_found) {
            if (a_found) {
                *a_found = false;
            }
            // ⚠⚠ NEVER As<PlayerCharacter>() ON AN ACTOR, AND THIS ONE COST THE
            // WHOLE FEATURE TWICE OVER. TESForm::As<T> is a switch over FormType
            // whose arms are `if constexpr (is_convertible_v<const Case*, const
            // T*>)`, and PlayerCharacter has no arm of its own: an actor reference
            // is FormType::ActorCharacter, whose arm is Character, and Character*
            // does not convert to PlayerCharacter* because that is a DOWNcast.
            // Every arm is discarded, so the call folds to a compile-time nullptr.
            //
            // The compiler then proved this function always returns with a_found
            // false and DELETED the caller's entire fire path. Field 2026-08-26:
            // two shipped builds, not one line from the watch, and no literal of
            // it anywhere in either DLL. The guard was blamed and fixed and it was
            // never reached. AppearanceWatch runs the identical walk and works,
            // because it asks PlayerCharacter::GetSingleton() instead.
            //
            // GetFaceNodeSkinned is virtual on TESObjectREFR and Character
            // overrides it, so there is nothing to cast for: ask the actor. The
            // caller has already established this is the player.
            auto* const face = a_actor ? a_actor->GetFaceNodeSkinned() : nullptr;
            if (!face) {
                return {};
            }
            std::string out;
            bool        found = false;
            RE::BSVisit::TraverseScenegraphGeometries(
                face, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                        a_geom->GetGeometryRuntimeData()
                            .properties[RE::BSGeometry::States::kEffect]
                            .get());
                    if (!prop) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    auto* const mat =
                        static_cast<RE::BSLightingShaderMaterialBase*>(prop->material);
                    if (!mat ||
                        mat->GetFeature() != RE::BSShaderMaterial::Feature::kFaceGen) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    auto* const fg =
                        static_cast<RE::BSLightingShaderMaterialFacegen*>(mat);
                    if (auto* const tt = fg->tintTexture.get()) {
                        out = tt->name.c_str();
                    }
                    found = true;
                    return RE::BSVisit::BSVisitControl::kStop;
                });
            if (a_found) {
                *a_found = found;
            }
            return out;
        }
    }

    void StandDownFaceWatch(int a_milliseconds) {
        g_faceChecksLeft      = 0;
        g_faceCheckFromBuild  = false;
        g_faceWatchQuietUntil = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(a_milliseconds);
        spdlog::info("Makeup: the face watch stands down for {} ms, because this "
                     "face was applied on purpose.",
                     a_milliseconds);
    }

    void ArmDelayedFaceRebake() {
        // ⚠⚠ THE QUIET PERIOD OUTRANKS EVERY ARMING EDGE. A look apply keeps
        // rebuilding the head after it completes, and each of those rebuilds
        // would otherwise arm a watch that then repairs the imported face away.
        if (std::chrono::steady_clock::now() < g_faceWatchQuietUntil) {
            return;
        }
        if (!g_holdSkin) {
            // ⚠⚠ THE WATCH'S SILENCE WAS INDISTINGUISHABLE FROM ITS ABSENCE.
            // Field 2026-08-26 20:53: the bake was demonstrably displaced with a
            // hold on and this said nothing at all, and the log could not tell
            // "never armed" from "armed, looked, stood down". Those are
            // different faults with different fixes, so each path names itself.
            if (!g_faceWatchNoHoldSaid) {
                g_faceWatchNoHoldSaid = true;
                spdlog::debug("Makeup: a write asked for the face watch and no "
                              "skin tone is held, so there is no face of ours to "
                              "put back. Said once per hold.");
            }
            return;  // not driving this character's colour; not our face to fix
        }
        // ⚠ PUSHED OUT, NOT ACCUMULATED. Re-arming during a drag resets the
        // clock, so the rebake lands once after the last edit instead of once per
        // edit. Retint rebuilds a texture and the note on it says never per frame.
        const bool wasArmed = g_faceChecksLeft > 0;
        g_faceCheckDueAt = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(MakeupPlan::kFaceWriteFirstLookMs);
        g_faceChecksLeft = MakeupPlan::kFaceLookCount;
        // ⚠ THE RISING EDGE AND NOT EVERY CALL. Arming is coalescing on purpose,
        // so a drag would otherwise write one line per frame of the drag.
        if (!wasArmed) {
            // ⚠ A FRESH CYCLE ARMED BY A WRITE IS WRITE-ONLY. Clearing this on
            // exhaustion instead would disarm the LAST look of a build-armed
            // cycle, which is the one most likely to catch a late re-bind.
            g_faceCheckFromBuild = false;
            spdlog::debug(
                "Makeup: the face watch is armed, {} looks over the next {:.1f} "
                "seconds and the first of them in {:.1f}.",
                MakeupPlan::kFaceLookCount,
                MakeupPlan::FaceWatchWindowMs(MakeupPlan::kFaceWriteFirstLookMs) /
                    1000.0,
                MakeupPlan::kFaceWriteFirstLookMs / 1000.0);
        }
    }

    void ArmFaceRebakeAfterHeadBuild() {
        if (std::chrono::steady_clock::now() < g_faceWatchQuietUntil) {
            return;
        }
        // ⚠ NO HOLD GATE HERE, ON PURPOSE. See the note in the header: a
        // build discards the character's own composite and the live tint list is
        // what rebuilds it. Gating this on g_holdSkin is what left the face with
        // no composite for three and a half minutes on 2026-08-26.
        const bool wasArmed  = g_faceChecksLeft > 0;
        g_faceCheckFromBuild = true;
        // ⚠ HALF A SECOND NOW, AND AN EARLY LOOK IS FREE. The write edge waits
        // on skee's delayed re-bind; this one waits only for the build to finish
        // binding its own material, which the AppearanceWatch samples put inside
        // a second. A look that fires before that bind reads kOurs or kNoHead,
        // costs one predicate and says so, and there are enough looks to spare
        // it.
        g_faceCheckDueAt = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(MakeupPlan::kFaceBuildFirstLookMs);
        g_faceChecksLeft = MakeupPlan::kFaceLookCount;
        if (!wasArmed) {
            spdlog::debug(
                "Makeup: a head build armed the face watch, {} looks over the next "
                "{:.1f} seconds and the first of them in {:.1f}.",
                MakeupPlan::kFaceLookCount,
                MakeupPlan::FaceWatchWindowMs(MakeupPlan::kFaceBuildFirstLookMs) /
                    1000.0,
                MakeupPlan::kFaceBuildFirstLookMs / 1000.0);
        }
    }

    void RunDelayedFaceRebake(RE::Actor* a_actor) {
        if (g_faceChecksLeft <= 0) {
            return;
        }
        // Belt as well as braces: a cycle armed a frame before the stand-down
        // must not get its looks in afterwards.
        if (std::chrono::steady_clock::now() < g_faceWatchQuietUntil) {
            g_faceChecksLeft     = 0;
            g_faceCheckFromBuild = false;
            return;
        }
        if (std::chrono::steady_clock::now() < g_faceCheckDueAt) {
            return;
        }
        --g_faceChecksLeft;
        // ⚠ A BOUNDED WINDOW AND THEN IT STOPS, WHICH IS THE FIGHT GUARD.
        // skee's re-bind can land late, so one look would miss it; an unbounded
        // watch would rebake against a preset the player actually chose forever.
        // The count and the spacing both live in MakeupPlan, so shortening the
        // gap the face is visibly wrong for can never quietly lengthen the
        // fight.
        g_faceCheckDueAt =
            g_faceChecksLeft > 0
                ? std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(MakeupPlan::kFaceLookSpacingMs)
                : std::chrono::steady_clock::time_point{};
        const bool isPlayer = IsPlayer(a_actor);
        if (!isPlayer) {
            spdlog::debug("Makeup: the face watch gives up before looking; the "
                          "actor is not the player.");
            g_faceChecksLeft     = 0;
            g_faceCheckFromBuild = false;
            return;
        }
        if (!g_holdSkin && !g_faceCheckFromBuild) {
            spdlog::debug("Makeup: the face watch gives up before looking; no tone "
                          "is held and no head build asked.");
            g_faceChecksLeft = 0;
            return;
        }
        // ⚠⚠ FOUR ANSWERS, ONE TABLE, AND MakeupPlan OWNS IT. Reading an
        // unnamed texture as "nothing to do" is the mistake that cost this
        // feature twice: once here, once in HeadBuildHook's late-build answer.
        // JudgeFaceTint carries the distinction and the suite pins it.
        bool       haveHead = false;
        const auto name     = HeadTintTextureName(a_actor, &haveHead);
        switch (MakeupPlan::JudgeFaceTint(haveHead, name)) {
            case MakeupPlan::FaceTintVerdict::kNoHead:
                spdlog::debug("Makeup: the face watch found no facegen head to "
                              "read ({} look(s) left).",
                              g_faceChecksLeft);
                return;
            case MakeupPlan::FaceTintVerdict::kOurs:
                spdlog::debug("Makeup: the face watch looked and the head still "
                              "wears the live bake ({} look(s) left).",
                              g_faceChecksLeft);
                return;
            case MakeupPlan::FaceTintVerdict::kCompositeGone:
                // ⚠⚠ THE REBUILD'S SIGNATURE, AND THE BODY IS NO WITNESS TO
                // IT. A new material carries an unnamed tint and the composite
                // that was on the old one is simply gone, while bodyTintColor is
                // a different field and stays right. That asymmetry is the whole
                // field report: the body keeps the colour, the head does not.
                spdlog::info(
                    "Makeup: the head's tint slot carries an UNNAMED texture, "
                    "which is what a rebuilt material carries, so the composite "
                    "is gone and the face is re-baked ({} look(s) left).",
                    g_faceChecksLeft);
                break;
            case MakeupPlan::FaceTintVerdict::kPresetFace:
                // ⚠⚠ THIS ARM STOOD DOWN FOR ONE ROUND AND IT WAS WRONG TWICE
                // OVER. It was built on reading "changing the skin color makes
                // the head different texture" as "leave my imported face
                // alone". The next round said what the report actually meant:
                // "the head goes a different color or rather doesn't update to
                // the set color". The head is wanted FOLLOWING the tone, which
                // is the opposite of standing down.
                //
                // ⛔⛔ AND STANDING DOWN HERE DISARMS THE CROSS-SAVE BLEED CURE,
                // which is the part that made it a regression rather than a
                // preference. RebakeFaceTint exists because nothing rebuilds
                // the baked face tint on a load, so a face baked while the
                // character was somebody else survives into the next save. The
                // texture it survives as is an export, so it lands here. Field
                // 2026-08-27 r88, one second after the save loaded:
                //
                //   06:43:56  the head wears 'FR_Umbrael 19.dds' ... left alone
                //
                // and the user: "when i load my save Umbrael's head can
                // literally be another color". That is the bleed, back, because
                // this arm sent it home.
                //
                // So an export is re-baked like any other displacement. The
                // verdict stays its own answer because the log is better for
                // naming which of the two it met, and IsChargenExport is what
                // names it.
                // ⚠⚠ UNLESS IT IS THIS CHARACTER'S OWN FACE, which is the one
                // export that is not residue. Everything above is about a face
                // baked for SOMEBODY ELSE surviving into this save; a face this
                // character is currently holding is the answer, not the
                // displacement. MEASURED walking out of a building 2026-09-01:
                // a head build bound 'FR_Almalexia.dds' and armed the watch, the
                // watch met it 0.6 s later, re-baked it, and the head wore
                // 'Player face tint' from then on. The look was 70 seconds old.
                //
                // ⚠ IDENTITY, NOT A STAND-DOWN: only the export whose jslot
                // LookFaceTint holds is spared, so a bled face, which carries a
                // jslot this character does not hold, is re-baked exactly as
                // before and the r88 cure is untouched.
                if (std::string held; LookFaceTint::Held(held) &&
                                      MakeupPlan::NamesTheSameExport(
                                          name, LookFaceTint::PathFor(held))) {
                    spdlog::info("Makeup: the head's tint slot carries '{}', which is the "
                                 "face this character is holding rather than one left by "
                                 "somebody else, so it is left alone ({} look(s) left).",
                                 name, g_faceChecksLeft);
                    break;
                }
                spdlog::info("Makeup: the head's tint slot carries '{}', a face exported on "
                             "purpose and NOT the one this character holds, so the face is "
                             "re-baked ({} look(s) left). The body reads bodyTintColor, a "
                             "different field, and was never affected.",
                             name, g_faceChecksLeft);
                break;
            case MakeupPlan::FaceTintVerdict::kDisplaced:
                // ⚠ A NAMED TEXTURE THAT IS NOT OURS IS SOMEBODY'S CHOICE UNTIL
                // WE HOLD A TONE. Re-baking over a preset the player picked on
                // purpose is the failure the fight guard exists to avoid, and a
                // build-armed look has no business making that call.
                if (!g_holdSkin) {
                    spdlog::debug("Makeup: the face watch found '{}' on the head "
                                  "and no tone held, so it is left alone ({} "
                                  "look(s) left).",
                                  name, g_faceChecksLeft);
                    return;
                }
                // ⚠ AN INSTRUMENT AND A FIX IN ONE LINE. "Something displaced
                // the bake" and "we put it back" are different claims, and the
                // field needs the displacer's name to know whether this is skee
                // or a third party.
                spdlog::info(
                    "Makeup: the head's tint slot carries '{}' rather than the "
                    "live bake, so the face is re-baked ({} look(s) left). The "
                    "body was never affected: it reads bodyTintColor, a "
                    "different field.",
                    name, g_faceChecksLeft);
                break;
        }
        Retint();
    }

    void HoldSkinTone(const OverlayPlan::Rgb& a_tint, float a_strength) {
        g_faceWatchNoHoldSaid = false;  // a new hold earns a fresh answer
        g_holdSkin     = true;
        g_holdTint     = a_tint;
        g_holdStrength = MakeupPlan::ClampStrength(a_strength);
    }

    void ReleaseSkinTone() {
        // The watch belongs to the hold. Leaving it armed across a revert is how
        // the blue body got onto the next character (2026-08-24), one field over.
        g_faceChecksLeft     = 0;
        g_faceCheckDueAt     = {};
        g_faceCheckFromBuild = false;
        if (g_holdSkin) {
            spdlog::info("Makeup: the held skin tone is released; the character's own "
                         "colour is whatever this save carries.");
        }
        g_holdSkin = false;
    }

    bool HoldsSkinTone() { return g_holdSkin; }

    bool HeldSkinTone(OverlayPlan::Rgb& a_tint, float& a_strength) {
        if (!g_holdSkin) {
            return false;
        }
        a_tint     = g_holdTint;
        a_strength = g_holdStrength;
        return true;
    }

    static void ReassertSkinToneNow(RE::Actor* a_actor) {
        // ⚠⚠ THIS ONE IS THE REPORTED REGRESSION. A head build repaints the
        // head and leaves the body alone, so standing this down does not put
        // the character back the way they were: it leaves the head on the
        // engine's tint and the body on ours, which is the "head skin different
        // to the body" report of 2026-08-29. A tone is a colour, so it answers
        // to the colour flag.
        if (!Settings::GetSingleton().keepColoursAfterRebuild) {
            static std::atomic<bool> said{ false };
            if (!said.exchange(true)) {
                spdlog::info("Makeup: the skin tone reassert stood down for the rest "
                             "of this session because [Compat] "
                             "bKeepColoursAfterRebuild is off. A head build's own tint "
                             "list is what the body wears, and the head and the body "
                             "may stop matching.");
            }
            return;
        }
        if (!g_holdSkin || !IsPlayer(a_actor)) {
            return;
        }
        auto* const mask = SkinToneMaskIn(a_actor);
        const auto  want = MakeupPlan::PackEngineColour(g_holdTint, g_holdStrength);
        if (mask && mask->colour == want &&
            mask->alpha == MakeupPlan::ClampStrength(g_holdStrength)) {
            return;  // nothing put it back this time
        }
        TintMask scratch{};
        TintMask* use = mask;
        if (mask) {
            mask->colour = want;
            mask->alpha  = MakeupPlan::ClampStrength(g_holdStrength);
        } else {
            scratch.colour = want;
            scratch.alpha  = MakeupPlan::ClampStrength(g_holdStrength);
            scratch.type   = static_cast<std::uint32_t>(MakeupPlan::Type::kSkinTone);
            use            = &scratch;
        }
        PaintBodyFrom(a_actor, use);
        spdlog::debug("Makeup: skin tone re-asserted after a head build ({},{},{}).",
                      g_holdTint.r, g_holdTint.g, g_holdTint.b);
        // The held tone is a write into the list like any other, and the saved
        // layers are what the editor's init would otherwise put back over it.
        // ⚠ ONLY WHEN A SLOT WAS WRITTEN. The scratch path above paints the
        // body and touches no list, and it runs every heartbeat while no tone
        // slot can be found (field 2026-09-02 02:58, 1465 mirror lines).
        if (mask) {
            SyncSavedLayers(a_actor, false);
            NoteOwnWrite(a_actor);
        }
    }

    // ---- the worn-set watch, an instrument that answers ---------------------
    //
    // ⚠⚠ THE LIST HAS A WRITER NONE OF OUR INSTRUMENTS SEE. Field 2026-09-02
    // 02:58: Nord 3 applied, the clear-only pass wrote slots 11 to 17 to alpha
    // 0 (logged), and by the next screenshot the six wore strength again with
    // no FR write, no converge, no load and no editor visit in between. This
    // reads the worn set every heartbeat and says the moment it changes, so
    // the neighbouring lines name the writer.
    //
    // ⚠⚠ THE WRITER IS RACEMENU REPLAYING ITS OWN COPY OF THE LIST, BY INDEX
    // (03:11, 03:13 and 03:44). About a second after every load and every
    // LoadCharacterEx, and inside the character editor right after a slider's
    // head build: texture, colour and alpha per slot, the rest cleared, no
    // write of ours beside it. The copy is the list as it stood at the last
    // RaceSexMenu close, and nothing we write reaches it. So the watch
    // ANSWERS: a change that was not ours puts the record back. Outside the
    // editor every such change is answered. Inside it the change is read by
    // CONTENT (MakeupPlan::JudgeEditorChange): the editor's own init in its
    // first seconds, then one slot is the user's slider, many slots equal to
    // the remembered copy are the replay, and many slots that are not the copy
    // are the user's own preset load, which the record must not undo. The
    // first cut answered by time alone, three seconds from the open, and
    // missed a replay at seven.
    namespace {
        struct WornReading {
            std::string         signature;
            std::string         worn;
            MakeupPlan::WornSet set;
            std::size_t         count{ 0 };
            std::uint32_t       size{ 0 };
        };

        WornReading g_last;
        bool        g_wornSeeded{ false };
        // When the character editor opened, for the init window the watch
        // answers by time in. Zero when it is not open.
        std::chrono::steady_clock::time_point g_editorOpenedAt{};
        constexpr auto kEditorInitWindow = std::chrono::seconds{ 3 };
        // RaceMenu's copy, as far as this session has seen it: the list at the
        // last editor close, or the content of the replay the watch answered
        // after a load or an apply. Dropped at every load, because a load
        // brings the cosave's copy and this session has not seen that one yet.
        MakeupPlan::WornSet g_copy;
        bool                g_copyKnown{ false };
        // When the last load edge and the last in-flight apply were seen, for
        // the windows a replay's content is remembered in. After a load the
        // replay is the FIRST wholesale change that was not ours, one per
        // load, under a ceiling that only guards a load that never replays:
        // measured 11, 16, 25 and 32 s after the post-load edge over four
        // loads (2026-09-02 04:43 to 05:26), so a ten second window caught
        // none. After an apply it came 0.6 s after LoadCharacterEx (03:13:50).
        std::chrono::steady_clock::time_point g_loadEdgeAt{};
        std::chrono::steady_clock::time_point g_applySeenAt{};
        bool                                  g_replayOwedAfterLoad{ false };
        constexpr auto kReplayAfterLoad  = std::chrono::seconds{ 90 };
        constexpr auto kReplayAfterApply = std::chrono::seconds{ 3 };

        [[nodiscard]] WornReading ReadWornSet(RE::PlayerCharacter* a_player) {
            WornReading out;
            const auto  live = LiveList(a_player);
            auto* const data = ListData(live);
            out.size         = ListSize(live);
            out.set.size     = out.size;
            if (data) {
                for (std::uint32_t i = 0; i < out.size; ++i) {
                    auto* const mask = data[i];
                    if (!mask || mask->alpha <= 0.0f) {
                        continue;
                    }
                    ++out.count;
                    const char* const name =
                        mask->texture ? mask->texture->textureName.c_str() : nullptr;
                    MakeupPlan::WornSlot slot;
                    slot.index   = i;
                    slot.alpha   = OverlayPlan::AlphaByte(mask->alpha);
                    slot.texture = name ? name : "";
                    // By its file name, the identity the type field cannot
                    // give on a list that is not this race's.
                    slot.tone = MakeupPlan::IsSkinToneTexture(slot.texture);
                    // ⚠ THE ART IS IN THE SIGNATURE, so a swap at one strength
                    // is seen: the editor's init resets every mask to the
                    // race's art and restores only colour and strength from
                    // the saved layers, which a strength-only reading calls
                    // no change at all.
                    out.signature += fmt::format("{}@{}:{};", i, slot.alpha, slot.texture);
                    if (!out.worn.empty()) {
                        out.worn += "  ";
                    }
                    out.worn += fmt::format("{}@{} '{}'", i, slot.alpha,
                                            name ? MakeupPlan::DisplayName(name)
                                                 : std::string{ "(none)" });
                    out.set.slots.push_back(std::move(slot));
                }
            }
            out.signature += fmt::format("|{}|{}", out.size, live == 0 ? "none" : "list");
            return out;
        }

        // The push; whatever it did, the watch starts again from what it left.
        [[nodiscard]] std::size_t PushRecord(RE::PlayerCharacter* a_player) {
            const auto written = MakeupBaseline::ReassertRecord(a_player);
            g_last             = ReadWornSet(a_player);
            return written;
        }

        void RememberCopy(const WornReading& a_reading, const char* a_why) {
            g_copy      = a_reading.set;
            g_copyKnown = true;
            spdlog::info(
                "Makeup: remembered as RaceMenu's copy of the tint list ({}): {} worn of "
                "{}: {}. That copy replays by index after a head build inside the next "
                "editor visit, and the watch answers it by content.",
                a_why, a_reading.count, a_reading.size,
                a_reading.worn.empty() ? "(none)" : a_reading.worn);
        }
    }  // namespace

    void NoteOwnWrite(RE::Actor* a_actor) {
        auto* const player = PlayerFor(a_actor);
        if (!player) {
            return;
        }
        g_last       = ReadWornSet(player);
        g_wornSeeded = true;
    }

    void NoteEditorOpened() {
        g_editorOpenedAt = std::chrono::steady_clock::now();
    }

    void NoteEditorClosed(RE::Actor* a_actor) {
        g_editorOpenedAt = {};
        auto* const player = PlayerFor(a_actor);
        if (!player) {
            return;
        }
        // ⚠ AFTER the close edge's own writes (the tone re-assert, the record
        // and the mirror), so this is the list RaceMenu closed on. The match
        // ignores the tone, because the copy may be taken either side of that
        // re-assert.
        RememberCopy(ReadWornSet(player), "the character editor closed on it");
    }

    void NoteHeartbeat(RE::Actor* a_actor) {
        auto* const player = PlayerFor(a_actor);
        if (!player) {
            return;
        }
        auto reading = ReadWornSet(player);
        if (!g_wornSeeded) {
            g_wornSeeded = true;
            g_last       = std::move(reading);
            return;
        }
        if (reading.signature == g_last.signature) {
            return;
        }
        const auto now     = std::chrono::steady_clock::now();
        const auto changed = MakeupPlan::WornSlotsChanged(g_last.set, reading.set);
        const auto change  = MakeupPlan::ClassifyWornChange(g_last.set, reading.set);
        g_last             = reading;
        const std::string& worn = reading.worn.empty() ? std::string{ "(none)" } : reading.worn;
        // ⚠ NOT DURING AN APPLY: its steps own the list, and RaceMenu's
        // LoadCharacterEx writes the preset's tints between them on purpose.
        if (ProfileApply::InFlight()) {
            g_applySeenAt = now;
            spdlog::info("Makeup: the worn set CHANGED on the heartbeat, mid-apply: {} worn "
                         "of {} ({}): {}",
                         reading.count, reading.size, ListOrigin(player), worn);
            return;
        }
        auto* const ui         = RE::UI::GetSingleton();
        const bool  editorOpen = ui && ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME);
        if (!editorOpen) {
            spdlog::info("Makeup: the worn set CHANGED on the heartbeat: {} worn of {} ({}): {}",
                         reading.count, reading.size, ListOrigin(player), worn);
            // A wholesale change shortly after a load or an apply is the
            // replay, and its content is the copy the next editor visit will
            // replay again.
            const bool firstAfterLoad =
                g_replayOwedAfterLoad &&
                g_loadEdgeAt != std::chrono::steady_clock::time_point{} &&
                now - g_loadEdgeAt < kReplayAfterLoad;
            const bool afterApply =
                g_applySeenAt != std::chrono::steady_clock::time_point{} &&
                now - g_applySeenAt < kReplayAfterApply;
            if (MakeupPlan::RemembersAsCopy(change, reading.size, firstAfterLoad, afterApply)) {
                g_replayOwedAfterLoad = false;
                RememberCopy(reading, firstAfterLoad ? "it replayed after the load"
                                                     : "it replayed after the apply");
            }
            const auto written = PushRecord(player);
            if (written != 0 || reading.count != 0) {
                spdlog::info("Makeup: the change was not ours, so the record was put back "
                             "over it ({} written).",
                             written);
            }
            return;
        }
        // Inside the character editor: by content, not by time alone.
        const bool withinInit =
            g_editorOpenedAt != std::chrono::steady_clock::time_point{} &&
            now - g_editorOpenedAt <= kEditorInitWindow;
        const bool matches = g_copyKnown && MakeupPlan::SameWornSet(g_copy, reading.set);
        switch (MakeupPlan::JudgeEditorChange(withinInit, change, g_copyKnown, matches)) {
            case MakeupPlan::EditorVerdict::kNothing:
                return;
            case MakeupPlan::EditorVerdict::kUsersSlot:
                spdlog::debug("Makeup: one slot moved inside the character editor, the "
                              "user's, left alone: {} worn of {}: {}",
                              reading.count, reading.size, worn);
                return;
            case MakeupPlan::EditorVerdict::kUsersWholesale:
                spdlog::info(
                    "Makeup: the worn set CHANGED inside the character editor, {} slot(s) in "
                    "one tick, and it is NOT RaceMenu's copy, so it is the user's own (a "
                    "preset loaded or a reset in there) and is left alone: {} worn of {}: {}",
                    changed, reading.count, reading.size, worn);
                return;
            case MakeupPlan::EditorVerdict::kUnknownWholesale:
                spdlog::info(
                    "Makeup: the worn set CHANGED inside the character editor, {} slot(s) in "
                    "one tick, and no copy of RaceMenu's is on record this session to judge "
                    "it against, so it is left alone: {} worn of {}: {}",
                    changed, reading.count, reading.size, worn);
                return;
            case MakeupPlan::EditorVerdict::kInit:
                spdlog::info("Makeup: the worn set CHANGED inside the character editor's init "
                             "window ({} slot(s)): {} worn of {}: {}",
                             changed, reading.count, reading.size, worn);
                break;
            case MakeupPlan::EditorVerdict::kReplay:
                spdlog::info(
                    "Makeup: the worn set CHANGED inside the character editor, {} slot(s) in "
                    "one tick and EQUAL to RaceMenu's copy, which is its replay after a head "
                    "build: {} worn of {}: {}",
                    changed, reading.count, reading.size, worn);
                break;
        }
        const auto written = PushRecord(player);
        if (written != 0 || reading.count != 0) {
            spdlog::info("Makeup: the change was not ours, so the record was put back over "
                         "it ({} written).",
                         written);
        }
    }

    bool RebuildListForRace(RE::Actor* a_actor, bool a_switched) {
        auto* const player = PlayerFor(a_actor);
        if (!player) {
            return false;
        }
        const auto root = ListRoot(player);
        if (!root) {
            return false;  // VR, or a runtime whose offset is not measured
        }
        const auto raceSlots = RaceTint::Slots(a_actor);
        const auto before    = ListSize(root);
        if (!MakeupPlan::NeedsRaceRebuild(a_switched, raceSlots.size(), before)) {
            return false;
        }
        // ⚠⚠ THE ENGINE'S OWN REBUILD, THE ONE THE CHARACTER EDITOR'S INIT RUNS
        // FIRST. MEASURED in Ghidra 2026-09-02: AE 40696 (FUN_140749e90) and SE
        // 39610 (FUN_1406b4e50), the same shape on both builds. The BASE list
        // at the offset ListRoot measures is freed when its length is not the
        // race's and rebuilt slot by slot from
        // race->faceRelatedData[sex]->tintMasks: texture pointer, type, default
        // colour and default alpha per asset; when the length already matches
        // every mask is rewritten in place. The editor's init then restores
        // the base's saved layers over it (AE 40697 / SE 39611, which calls
        // this first); this does not, because those layers are the OLD race's
        // mirror and the look's makeup is written right after this.
        using func_t = void (*)(RE::PlayerCharacter*);
        static REL::Relocation<func_t> rebuild{ REL::RelocationID(39610, 40696) };
        rebuild(player);
        const auto after   = ListSize(root);
        const auto overlay = OverlayList(root);
        spdlog::info(
            "Makeup: the live tint list was rebuilt to this race's slots the way the "
            "character editor's init does it ({} -> {} layer(s), the race carries {}), "
            "because {}. Every mask wears the race's own art at its default now; the "
            "look's makeup is written after this, into a list this race owns, which is "
            "what the makeup step, the body sync and the record have been refusing (r35).",
            before, after, raceSlots.size(),
            a_switched ? "this apply switched the race or the sex"
                       : "the list was another race's length");
        if (overlay) {
            spdlog::warn(
                "Makeup: an OVERLAY tint list of {} layer(s) is installed over the rebuilt "
                "base, and the rebuild reaches the base only. Reads and writes land on the "
                "overlay, so if its length is not this race's the r35 refusals still hold "
                "there. Not measured on this rig, where the overlay is never installed; this "
                "line is the instrument.",
                ListSize(overlay));
        }
        // The rebuild is ours, so the watch must not read it as a foreign
        // write, and the saved layers (the third copy the editor's init
        // restores from) follow it, so the next visit's init restores these
        // defaults rather than the old race's mirror at matching indices.
        NoteOwnWrite(a_actor);
        SyncSavedLayers(a_actor, false);
        return true;
    }

    void ReassertSkinTone(RE::Actor* a_actor) {
        // ⚠⚠ STAND DOWN WHILE THE CHARACTER EDITOR OWNS THE TONE, HERE AND NOT
        // AT THE CALL SITES. The head-build hook has stood down in there since
        // 2026-08-24, and the heartbeat WorldWatch added on 08-25 for the seven
        // second white flash never learned the gate. Field 2026-09-02 02:24,
        // with RaceMenu open: every skin tone drag went (167,134,122) to the
        // dragged colour and back to (167,134,122) fifty milliseconds later,
        // "skin tone re-asserted after a head build" beside each one. Two
        // writers of one slot, and the user's was losing. The close edge puts
        // the held tone back through NoteHeadEditorClosed, which bypasses this
        // because the menu still reports itself open inside its own close
        // event.
        if (auto* const ui = RE::UI::GetSingleton();
            ui && ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME)) {
            return;
        }
        ReassertSkinToneNow(a_actor);
    }

    void DumpWorn(RE::Actor* a_actor, const char* a_where) {
        if (!spdlog::should_log(spdlog::level::debug)) {
            return;
        }
        auto* const player = PlayerFor(a_actor);
        if (!player) {
            return;
        }
        const auto  layers = Layers(player);
        const auto  state  = Read(player);
        std::string worn;
        std::size_t count = 0;
        for (std::size_t i = 0; i < layers.size() && i < state.size(); ++i) {
            if (!MakeupPlan::Occupied(state[i])) {
                continue;
            }
            ++count;
            if (!worn.empty()) {
                worn += "  ";
            }
            worn += fmt::format("{}:{}@{} '{}'", i, MakeupPlan::IdFor(layers[i].type),
                                OverlayPlan::AlphaByte(state[i].strength),
                                state[i].hasTexture ? MakeupPlan::DisplayName(state[i].texture)
                                                    : std::string{ "(none)" });
        }
        spdlog::debug("Makeup: live list [{}]: {}; {} worn of {}: {}", a_where,
                      ListOrigin(player), count, layers.size(),
                      worn.empty() ? "(none)" : worn);
    }

    void DumpSavedLayers(RE::Actor* a_actor, const char* a_where) {
        if (!spdlog::should_log(spdlog::level::debug)) {
            return;
        }
        auto* const player = PlayerFor(a_actor);
        auto* const base   = player ? player->GetActorBase() : nullptr;
        if (!base) {
            return;
        }
        if (!base->tintLayers) {
            spdlog::debug("Makeup: saved tint layers [{}]: none, the base carries no array.",
                          a_where);
            return;
        }
        std::string worn;
        std::size_t count = 0;
        std::size_t total = 0;
        for (auto* const layer : *base->tintLayers) {
            if (!layer) {
                continue;
            }
            ++total;
            if (layer->interpolationValue == 0) {
                continue;
            }
            ++count;
            if (!worn.empty()) {
                worn += ' ';
            }
            worn += fmt::format("{}@{}", layer->tintIndex, layer->interpolationValue);
        }
        spdlog::debug("Makeup: saved tint layers [{}]: {} of {} carry strength: {}", a_where,
                      count, total, worn.empty() ? "(none)" : worn);
    }

    void SyncSavedLayers(RE::Actor* a_actor, bool a_editorClosing) {
        auto* const player = PlayerFor(a_actor);
        if (!player) {
            return;
        }
        // ⚠ NOT WHILE THE CHARACTER EDITOR OWNS THE LIST. Its init restores the
        // layers into the list and its accept writes them back; a mirror taken
        // in between would be taken from a list RaceMenu is mid-way through
        // editing. The close edge says so explicitly, because the menu can
        // still report itself open inside its own close event.
        if (!a_editorClosing) {
            if (auto* const ui = RE::UI::GetSingleton();
                ui && ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME)) {
                return;
            }
        }
        auto* const base = player->GetActorBase();
        if (!base) {
            return;
        }
        // The ACTOR's race first, which is the one the editor's init reads
        // (player+0x1F8 in 40696 and 40697); the base's is the fallback.
        auto* const race = player->GetRace() ? player->GetRace() : base->GetRace();
        if (!race) {
            return;
        }
        // Same sex rule as RaceTint::Slots: kNone is -1 as an unsigned.
        const auto  sex   = base->GetSex();
        const auto  which = sex == RE::SEXES::kFemale ? RE::SEXES::kFemale : RE::SEXES::kMale;
        auto* const face  = race->faceRelatedData[which];
        if (!face || !face->tintMasks) {
            return;
        }
        const auto  live = LiveList(player);
        auto* const data = ListData(live);
        const auto  size = ListSize(live);
        if (!data || size == 0) {
            return;
        }
        // The pure plan: every live state beside the race's TINI index at the
        // same position, kNoTintIndex where the race has no asset there.
        MakeupPlan::Snapshot states;
        std::vector<int>     indices;
        states.reserve(size);
        indices.reserve(size);
        for (std::uint32_t i = 0; i < size; ++i) {
            MakeupPlan::LayerState state{};
            int                    index = MakeupPlan::kNoTintIndex;
            if (auto* const mask = data[i]) {
                state.tint     = MakeupPlan::EngineTintFrom(mask->colour);
                state.strength = mask->alpha;
                if (i < face->tintMasks->size()) {
                    if (auto* const asset = (*face->tintMasks)[i]) {
                        index = asset->texture.index;
                    }
                }
            }
            states.push_back(std::move(state));
            indices.push_back(index);
        }
        const auto plan = MakeupPlan::PlanSavedLayers(states, indices);
        if (plan.empty()) {
            return;
        }
        // A character the editor has never seen carries no layer array at
        // all; it is made the way the engine makes everything else it frees.
        if (!base->tintLayers) {
            void* const mem = RE::malloc(sizeof(RE::BSTArray<RE::TESNPC::Layer*>));
            if (!mem) {
                return;
            }
            base->tintLayers = new (mem) RE::BSTArray<RE::TESNPC::Layer*>();
        }
        auto&       layers  = *base->tintLayers;
        std::size_t added   = 0;
        std::size_t updated = 0;
        for (const auto& want : plan) {
            // First match by index, which is the lookup the engine's own
            // restore uses (24782), so two assets sharing an index resolve the
            // same way on both sides.
            RE::TESNPC::Layer* layer = nullptr;
            for (auto* const candidate : layers) {
                if (candidate && candidate->tintIndex == want.tintIndex) {
                    layer = candidate;
                    break;
                }
            }
            if (!layer) {
                void* const mem = RE::malloc(sizeof(RE::TESNPC::Layer));
                if (!mem) {
                    continue;
                }
                layer            = new (mem) RE::TESNPC::Layer{};
                layer->tintIndex = want.tintIndex;
                layers.push_back(layer);
                ++added;
            } else {
                ++updated;
            }
            // ⚠ BYTES, NOT A WORD: RE::Color names its four components, and
            // the engine copies this struct's word straight onto the mask's
            // 0xAABBGGRR field, so red first is the order that survives.
            layer->tintColor.red     = want.tint.r;
            layer->tintColor.green   = want.tint.g;
            layer->tintColor.blue    = want.tint.b;
            layer->tintColor.alpha   = OverlayPlan::AlphaByte(want.strength);
            layer->interpolationValue = MakeupPlan::InterpolationOf(want.strength);
        }
        // ⚠⚠ THE SAVE ONLY CARRIES WHAT THE CHANGE FLAG SAYS CHANGED. The
        // layers are the player base's face data (the NPC_ change form's
        // face block, flag 1 << 11), and a write that leaves the flag alone is
        // a write the next save does not carry and the next load does not
        // reset, which is how a previous session's layers can outlive a load.
        // The engine's own chargen sets it; so does this.
        base->AddChange(RE::TESNPC::ChangeFlags::kFace);
        spdlog::debug(
            "Makeup: {} saved tint layer(s) mirrored from the live list ({} new, {} "
            "updated{}), so the character editor's rebuild restores what the face "
            "wears rather than what chargen last wrote.",
            plan.size(), added, updated, a_editorClosing ? ", at the editor's close edge" : "");
    }

    void NoteHeadEditorClosed(RE::Actor* a_actor) {
        if (!g_holdSkin || !IsPlayer(a_actor)) {
            return;
        }
        // Same posture the hair colour has held at this seam since July: what
        // Fitting Room drives, Fitting Room puts back once the editor that
        // owned the face has finished with it.
        spdlog::info("Makeup: the character editor closed and the look's skin tone is "
                     "re-asserted over the list its head build restored.");
        ReassertSkinToneNow(a_actor);
    }

    void ArmFaceRebake() {
        g_rebakeOwed = true;
        // The load edge, for the worn-set watch: a replay in the next seconds
        // is RaceMenu's copy from the cosave, which this session has not seen,
        // so whatever copy it remembered belongs to the character before.
        g_loadEdgeAt          = std::chrono::steady_clock::now();
        g_copyKnown           = false;
        g_replayOwedAfterLoad = true;
        // ⚠⚠ AND A WINDOW, NOT JUST THE ONE SHOT. r47 MEASURED the movie
        // twice: the load's rebake lands on the FIRST head build and paints
        // the face right, and a SECOND build at +7-12 s re-binds skee's
        // restored preset tint FILE (Textures\CharGen\Exported\FR_*.dds) and
        // the face goes wrong again - "fine at first, then the wrong colours
        // kick in", in the field's words. The window lets the head-build hook
        // keep answering late builds; the RACE-MISMATCH gate that decides
        // whether an answer is owed lives at the hook, which can read the
        // profile store.
        g_rebakeWindowUntil =
            std::chrono::steady_clock::now() + std::chrono::seconds(20);
    }

    void CancelFaceRebake() {
        // An apply owns the face from its first step: the load's debt and its
        // window are both void, or the window would wipe the preset face the
        // apply is about to bind (r42 applied 28 s after a load - inside it).
        g_rebakeOwed        = false;
        g_rebakeWindowUntil = {};
    }

    bool FaceRebakeWindowOpen() {
        return std::chrono::steady_clock::now() < g_rebakeWindowUntil;
    }

    void RunOwedFaceRebake() {
        if (!g_rebakeOwed) {
            return;
        }
        g_rebakeOwed = false;
        RebakeFaceTint();
        spdlog::info("Makeup: the face tint was rebaked after the load, from the tint "
                     "list this save restored. A baked face outlives the character it "
                     "was baked for, which is the cross-save bleed.");
    }

    void RebakeFaceTint() {
        // ⚠⚠ THIS IS THE BLEED'S CURE AND THE r39 LOG IS WHY. After loading an
        // earlier save the Nord's tint list reads his OWN pale skin tone, skee's
        // overlay store is empty, and the head's texture set names the vanilla
        // `MaleHead.dds`, yet the face still wears the abandoned look. None of
        // the three stores is carrying it: what carries it is the BAKED face
        // tint texture, which the retint built while the character was somebody
        // else and which nothing rebuilds on a load. Baking again from the list
        // the load restored is what a fresh start would have done anyway.
        Retint();
    }

    std::string ListOrigin(RE::Actor* a_actor) { return OriginPhrase(a_actor); }

    void Restore(RE::Actor* a_actor, const MakeupPlan::Snapshot& a_from,
                 const MakeupPlan::Snapshot& a_to) {
        const auto changed = MakeupPlan::Differences(a_from, a_to);
        Write(a_actor, changed, a_to);
    }

    std::uint64_t Fingerprint(RE::Actor* a_actor) {
        auto* const player = PlayerFor(a_actor);
        const auto  base   = LiveList(player);
        auto* const data   = ListData(base);
        const auto  size   = ListSize(base);
        // FNV-1a over the size and the mask pointers. The POINTERS and not the
        // contents, deliberately: our own writes change contents, and a
        // fingerprint that moved under our own writes would re-read the page
        // out from under every edit. A REBUILD allocates new masks, and new
        // masks are new pointers.
        std::uint64_t hash = 14695981039346656037ull;
        const auto    mix  = [&hash](std::uint64_t a_value) {
            hash ^= a_value;
            hash *= 1099511628211ull;
        };
        mix(size);
        if (data) {
            for (std::uint32_t i = 0; i < size; ++i) {
                mix(reinterpret_cast<std::uint64_t>(data[i]));
            }
        }
        return hash;
    }

}  // namespace OS::MakeupApi
