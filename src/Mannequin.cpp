#include "Mannequin.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace OS::Mannequin {

    namespace {

        // ⚠ BUMP THIS WHENEVER WHAT THE MANNEQUIN DRAWS CHANGES WITHOUT ITS
        // PATHS CHANGING, and nothing else. It re-keys every card carrying a
        // mannequin and leaves weapons, ammo, eyes and brows on the
        // thumbnails they already have, which is why the renderer version has
        // not had to move for this feature.
        //
        // m1: first field build (2026-08-10). Every card drew the item alone:
        //     the garment filters deleted the body and head on the face flags
        //     and the hands and feet on the slot gates.
        // m2: those exemptions.
        // m3: the head slab narrows in X (a slab as wide as the arms made the
        //     camera stand back far enough to show the hips), and hair takes
        //     the same figure as gear instead of a head on its own.
        // m4: every part composes always (the engine's dressing rule was
        //     deleting the figure), and the frame is a band taken from the
        //     item's own bounds instead of from a table of slot numbers.
        // m5: the head alone comes back under the slot-30 rule, the bands
        //     tighten, and an eye card gets a face to sit in.
        // m6: the band takes the item's box in ALL THREE axes. Keeping the
        //     body's width put a two-unit eye in a twenty-two-unit box and the
        //     width decided the camera distance.
        // m7: standard per-slot views taken from the MANNEQUIN, so every card
        //     in a slot puts the body in the same place; no genitals.
        // m8: the view numbers, tuned on cards.
        // m9: the reference body excludes the head, so a full-face helm no
        //     longer slides every window down; eye and hand tuned; a
        //     knee-high boot takes the mannequin's foot with it.
        // m10: the head, hand and eye windows, MEASURED off this rig's own
        //      meshes instead of tuned on cards. kNeckFraction was accused of
        //      both the head and hand reports and was innocent: measured
        //      0.870631 against the 0.87 shipped. The head window centred on
        //      0.965 where the head's centre is 0.92001, the hand window's TOP
        //      edge sat below the hands' centre, and one eye sits at x 0.0371
        //      where the window was offset 0.052.
        // m11: any footwear takes the foot, the calf slot having been measured
        //      dead (5 of 5469 records claim 38, the rule fired on 0 of 20
        //      footwear cards while the field reported feet through boots).
        //      And the reference box is the BODY's alone, so dropping the feet
        //      no longer takes the floor with them: that is what cut three
        //      knee-high boots off at the bottom, and widening the drop rule
        //      would have done it to every boot in the catalog.
        // m12: a hair authored against a different head is moved onto ours.
        //      KS Hairdo's ships two authoring skeletons and 264 of its files
        //      put the head bone 3.3 units low, which is why nine hair cards
        //      sat low with the grey scalp through the top. Measured against
        //      the mannequin's own head bone, so a correctly authored hair
        //      measures zero and does not move.
        // m13: a beard gets its own window instead of sharing the hair's, and
        //      lands on the jaw; a ring alone gets the FINGERS instead of the
        //      whole hand; the feet window's floor drops below the ground so a
        //      high heel's sole stops being clipped. ⚠ The beard and ring
        //      CENTRES are guesses where the head and hands were measured, so
        //      they want a card to judge them. The eye did NOT change: it is
        //      already the size of the eye and its zoom is held by the depth
        //      axis, which nothing has measured yet.
        // m14: the amulet window, recentred on where the amulets actually hang
        //      (measured z 107.0 to 108.6, f 0.812 to 0.824) and cut from 22.4
        //      units to 13.4. It had been centred on 0.785, so nearly half the
        //      card was empty chest below the pendant.
        // m15: the eye finally zooms, and not by tightening the band. The
        //      camera stands off the window's FRONT face, the body's front is
        //      the bust at y 10.88 and the eye's is at 6.97, so the whole-depth
        //      window stood four units further out than the subject needed.
        //      Pulling the front to 0.85 of the body's depth takes the eye from
        //      41% of the card to 66%.
        // m16: the mannequin's head, hands and feet are SHADED. Their partition
        //      buffers carry no normals at all (VERTEX|UV|SKINNED, stride 32),
        //      so every vertex took PreviewVertex's (0, 0, 1) default and the
        //      shader lit a flat plate; the body-card fix reached the body
        //      alone because it was gated on the morph. Nothing about the
        //      mannequin's own geometry, framing or windows moved, but its
        //      PICTURE did, and a picture that changes without its key changing
        //      is served stale forever.
        constexpr const char* kTag = "m16";

        struct Part {
            std::string   path;
            std::uint32_t slots{ 0 };
            bool          isHead{ false };
        };

        struct Store {
            std::vector<Part> parts;
        };

        Store& Get() {
            static Store s;
            return s;
        }

    }  // namespace

    void Clear() {
        Get().parts.clear();
    }

    void Refresh(RE::TESObjectARMO* a_skin, RE::TESRace* a_race, int a_sexIdx) {
        auto& s = Get();
        s.parts.clear();
        if (!a_skin || !a_race) {
            spdlog::info("Mannequin: no skin or no race for this target; cards keep "
                         "today's scene.");
            return;
        }

        // The body, hands and feet, from the skin's own addons. ⚠ THE SAME
        // RACE QUESTION CollectArmourPaths ASKS, RNAM fallback included: a
        // custom-race character whose armour race is DefaultRace resolves its
        // body through exactly that fallback, and a collector without it
        // hands back an empty mannequin on the characters most likely to want
        // one.
        RE::TESRace* const armorRace = a_race->armorParentRace;
        for (auto* arma : a_skin->armorAddons) {
            if (!arma) {
                continue;
            }
            const bool raceValid =
                arma->IsValidRace(a_race) ||
                (armorRace && armorRace != a_race && arma->IsValidRace(armorRace));
            if (!raceValid) {
                continue;
            }
            const char* path = arma->bipedModels[a_sexIdx].GetModel();
            if (!path || !*path) {
                continue;
            }
            const auto slots = static_cast<std::uint32_t>(arma->GetSlotMask());
            // ⚠ NO GENITALS ON A SHOP MANNEQUIN. Biped slot 52 is the
            // community's schlong slot, which is how The New Gentleman and SOS
            // both reach the body, so the slot is the rule and the path is the
            // safety net for a mod that picked its own. The field's word:
            // jarring on a card whose subject is a helmet.
            constexpr std::uint32_t kGenitalSlotBit = 1u << 22;  // biped slot 52
            const auto              folded = PreviewGrid::FoldPath(path);
            if ((slots & kGenitalSlotBit) != 0 ||
                folded.find("/tng/") != std::string::npos ||
                folded.find("genital") != std::string::npos ||
                folded.find("schlong") != std::string::npos) {
                spdlog::info("Mannequin: skipping '{}' (slot 52 / genital mesh).", path);
                continue;
            }
            s.parts.push_back(Part{ path, slots, false });
        }

        // The head, from the race's own head part list. ⚠ TYPE kFace IS THE
        // HEAD MESH; kHair, kEyes and the rest are the character's identity
        // and none of them belongs on a mannequin. It comes out bald and
        // untextured, which is what makes a hair card show hair on a head
        // rather than on a likeness.
        //
        // ⚠ Its slot is the HEAD's (biped 30) rather than the ARMA slot mask
        // an addon carries, because a head part has no biped mask of its own
        // and the rule that hides it is the engine's slot-30 rule.
        if (auto* face = a_race->faceRelatedData[a_sexIdx];
            face && face->headParts) {
            for (auto* part : *face->headParts) {
                if (!part || part->type != RE::BGSHeadPart::HeadPartType::kFace) {
                    continue;
                }
                const char* model = part->GetModel();
                if (model && *model) {
                    s.parts.push_back(Part{ model, PreviewFilter::kHeadSlotBit, true });
                    break;  // one head
                }
            }
        }

        std::string names;
        for (const auto& p : s.parts) {
            if (!names.empty()) {
                names += ", ";
            }
            names += p.path;
        }
        spdlog::info("Mannequin: {} part(s) for skin {:08X} sex {}: {}", s.parts.size(),
                     a_skin->GetFormID(), a_sexIdx == RE::SEXES::kFemale ? "F" : "M",
                     names.empty() ? "(none)" : names);
    }

    std::size_t PartCount() {
        return Get().parts.size();
    }

    std::string BodyPath() {
        for (const auto& p : Get().parts) {
            if ((p.slots & PreviewFilter::kBodySlotBit) != 0) {
                return p.path;
            }
        }
        return {};
    }

    std::vector<std::string> SubjectExtras() {
        std::vector<std::string> out;
        for (const auto& p : Get().parts) {
            // ⚠ THE TORSO IS EXCLUDED BY ITS OWN SLOT, not by being the part
            // that also claims hands or feet. This load order's body addon
            // claims enough slots that testing for 33 or 37 alone would take
            // it too (the same breadth that made the old hide rule delete the
            // whole figure), so slot 32 disqualifies a part outright.
            if ((p.slots & PreviewFilter::kBodySlotBit) != 0) {
                continue;
            }
            const bool wanted =
                p.isHead || (p.slots & (PreviewFilter::kHandsSlotBit |
                                        PreviewFilter::kFeetSlotBit)) != 0;
            if (wanted) {
                out.push_back(p.path);
            }
        }
        return out;
    }

    void Compose(PreviewGrid::SceneIdentity& a_id) {
        const auto& s = Get();
        a_id.mannequinPathCount = 0;
        a_id.mannequinHeadIndex = static_cast<std::size_t>(-1);
        a_id.mannequinFeetIndex = static_cast<std::size_t>(-1);
        a_id.mannequinTag.clear();
        if (s.parts.empty()) {
            return;
        }
        const auto want = PreviewGrid::MannequinFor(a_id.kind, a_id.slotMask);
        if (want == PreviewGrid::MannequinKind::kNone) {
            return;
        }

        // ⚠⚠ EVERY PART, ALWAYS, AND THE HIDE RULE THAT WAS HERE IS GONE.
        // It borrowed the engine's dressing rule: drop a body part when the
        // item occupies a slot that part occupies. That is right for a live
        // actor, where the garment mesh carries the exposed skin itself, and
        // wrong for a card, where the point is a figure to judge the item
        // against. The field killed it three ways at once (2026-08-10): a
        // cuirass took the whole body with it and left a head, two hands and
        // two feet floating; sandals took the feet they were meant to be worn
        // on; and this load order's body addon claims enough slots that even
        // an amulet came back bodiless.
        //
        // The cost is that a fitted garment can z-fight the grey body inside
        // it. That is a wrong pixel here and there on an item the card still
        // reads correctly, against a missing figure on every torso card.
        std::vector<std::string> chosen;
        std::size_t              headAt = static_cast<std::size_t>(-1);
        std::size_t              feetAt = static_cast<std::size_t>(-1);
        for (const auto& p : s.parts) {
            // ⚠ THE HEAD IS THE ONE PART THE ENGINE'S RULE WAS RIGHT ABOUT, and
            // the field asked for it back by name (2026-08-10: "some helmets
            // shouldn't have the white head as it clips into it, but others of
            // course need it"). Biped slot 30 hides the whole head node, which
            // is why full-face pieces take it and open helmets do not: Dukaan
            // and a Dawnguard full helm replace the face, so a head inside one
            // is geometry the game never draws, poking through the mask. A
            // Chitin helmet leaves 30 alone and needs the face it frames.
            //
            // The body, hands and feet stay unconditional. Their version of
            // this rule is what deleted the figure, because a garment mesh
            // carries the exposed skin itself on a live actor and a card has
            // no such thing.
            if (p.isHead && (a_id.slotMask & PreviewFilter::kHeadSlotBit) != 0) {
                continue;
            }
            // ⚠⚠ ANY SLOT-37 ITEM TAKES THE FOOT, and the calf slot that used
            // to qualify this was measured dead. The theory was that heeled
            // and knee-high footwear claims 37 AND 38 while an open sandal
            // claims 37 alone, so the pair could tell a boot from a sandal.
            // It cannot, because almost nothing claims 38: of 5469 winning
            // ARMO records in this load order, 599 claim 37 and FIVE also
            // claim 38, one of which is a dummy helmet claiming every slot.
            // Abyss Boots and Wedding Sandals both declare exactly 0x80. The
            // rule fired on 0 of the 20 footwear cards in the field log while
            // the field was reporting feet through the leather.
            //
            // ⚠ AND THE SANDAL KEEPS ITS FEET ANYWAY, which is what makes the
            // simple rule safe rather than a trade. Open footwear ships its
            // own foot geometry inside the garment NIF, and MeshExtractor's
            // feetHidden gate keeps a garment's own `feet` leaf exactly when
            // the item occupies slot 37. Of the slot-37 meshes carrying their
            // own feet, every one tops out at ankle height; the wedding
            // sandals that started this are one of them.
            const bool isFoot =
                (p.slots & PreviewFilter::kFeetSlotBit) != 0 && !p.isHead;
            if (isFoot && (a_id.slotMask & PreviewFilter::kFeetSlotBit) != 0) {
                continue;
            }
            if (p.isHead) {
                headAt = chosen.size();
            }
            if (isFoot) {
                feetAt = chosen.size();
            }
            chosen.push_back(p.path);
        }
        if (chosen.empty()) {
            return;
        }

        // ⚠ PREPEND, AND THE PARALLEL VECTORS MOVE WITH IT. swaps is indexed
        // BY MODEL PATH, so inserting paths at the front without inserting
        // the same number of empty swap entries repaints the mannequin with
        // the item's alternate textures and leaves the item flat. scopeTags
        // is stamped after this by the caller and needs no help.
        //
        // ⚠ AND swaps STAYS ABSENT WHEN IT WAS ABSENT. A scene carrying no
        // swap anywhere must not grow a sized vector here, or every
        // swap-less key gains a stray suffix and the byte-identity pin dies
        // structurally rather than visibly.
        const auto count = chosen.size();
        chosen.insert(chosen.end(), a_id.modelPaths.begin(), a_id.modelPaths.end());
        a_id.modelPaths        = std::move(chosen);
        a_id.mannequinPathCount = count;
        a_id.mannequinTag       = kTag;
        a_id.mannequinHeadIndex = headAt;
        a_id.mannequinFeetIndex = feetAt;
        if (!a_id.swaps.empty()) {
            a_id.swaps.insert(a_id.swaps.begin(), count,
                              std::vector<PreviewGrid::TextureSwapEntry>{});
        }
    }

}  // namespace OS::Mannequin
