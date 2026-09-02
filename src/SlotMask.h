#pragma once

#include <bit>
#include <cstdint>
#include <span>

namespace OS {

    // CommonLib's BipedObjectSlot is a bitmask: kHead(1<<0) is editor slot 30.
    // bit = editorSlot - 30.  Cloak (editor 46) is bit 16 (kModChestPrimary).
    // Precondition: a_editorSlot >= 30 (else unsigned underflow -> shift UB).
    constexpr std::uint32_t BitForEditorSlot(std::uint32_t a_editorSlot) {
        return a_editorSlot - 30u;
    }
    // Precondition: a_editorSlot in [30, 61] (else the 1u shift is UB).
    constexpr std::uint32_t MaskForEditorSlot(std::uint32_t a_editorSlot) {
        return 1u << BitForEditorSlot(a_editorSlot);
    }

    inline constexpr std::uint32_t kBitHead    = BitForEditorSlot(30);
    inline constexpr std::uint32_t kBitHair    = BitForEditorSlot(31);  // helmet
    inline constexpr std::uint32_t kBitBody    = BitForEditorSlot(32);
    inline constexpr std::uint32_t kBitHands   = BitForEditorSlot(33);
    inline constexpr std::uint32_t kBitAmulet  = BitForEditorSlot(35);
    inline constexpr std::uint32_t kBitRing    = BitForEditorSlot(36);
    inline constexpr std::uint32_t kBitFeet    = BitForEditorSlot(37);
    inline constexpr std::uint32_t kBitShield  = BitForEditorSlot(39);
    inline constexpr std::uint32_t kBitCirclet = BitForEditorSlot(42);
    inline constexpr std::uint32_t kBitCloak   = BitForEditorSlot(46);

    // Which ONE slot row a multi-slot garment lists under in the editor.
    //
    // ⚠ THE LOWEST BIT IS THE WRONG ANSWER FOR A ROBE, which is how this was
    // found (field 2026-08-10). Mage and monk robes claim 31 for their hood as
    // well as 32 for the body, and 31 is the lower bit, so every hooded robe
    // filed itself under Helmet and the Torso row it belongs on sat empty.
    //
    // The body wins when it is claimed at all, because a garment covering the
    // torso IS a torso garment however many other slots it also takes.
    // Everything else keeps the lowest bit: that answer was right for the
    // items it was chosen for and this is deliberately the smallest change
    // that fixes the reported one.
    //
    // ⚠ Presentation only. primaryBit is derived from the ARMO at catalog
    // build and never persisted, so no saved outfit refers to it and changing
    // it cannot move an existing assignment.
    [[nodiscard]] constexpr std::uint32_t PrimaryBitFor(std::uint32_t a_slotMask) {
        if (a_slotMask == 0) {
            return 0;
        }
        if ((a_slotMask & (1u << kBitBody)) != 0) {
            return kBitBody;
        }
        return static_cast<std::uint32_t>(std::countr_zero(a_slotMask));
    }

    // The engine's two gore slots. 50 holds the severed head a corpse is given
    // and 51 is the flag that puts it there; no wearable garment occupies
    // either, so a transmog entry on them can never render on a living actor.
    // Named here rather than as literals in the editor because they are a fact
    // about the engine, not a decision the editor made.
    inline constexpr std::uint32_t kBitDecapHead   = BitForEditorSlot(50);
    inline constexpr std::uint32_t kBitDecapitate  = BitForEditorSlot(51);

    // ⚠⚠ THE COMMUNITY'S SCHLONG SLOT, and it is NOT a gore slot: real meshes
    // live here. The New Gentleman and SOS both reach the body through 52, and
    // a TNG install parks a GenitalCover on it, so anything else that OWNS the
    // slot displaces that cover the moment it is worn. Field 2026-08-20: a
    // HIMBO male in full armour with his genitals out, because AutoPresets'
    // completion pass had handed two sets a slot-52 piece off an outfit record
    // ('Triss DLC Necklace', 'Elden Smalls').
    //
    // ⚠ SO THE RULE IS "NEVER CHOOSE IT FOR SOMEBODY", not "never allow it".
    // A player who puts a piece on 52 by hand has made a choice and keeps it;
    // what must not happen is a GENERATED set claiming the slot silently.
    // Mannequin.cpp refuses to DRAW slot 52 on a card for the neighbouring
    // reason.
    inline constexpr std::uint32_t kBitGenitals    = BitForEditorSlot(52);

    // The three slots OBody uses to decide whether an actor is clothed for
    // ORefit: body, primary chest, and secondary chest.
    inline constexpr std::uint32_t kORefitTorsoMask =
        MaskForEditorSlot(32) | MaskForEditorSlot(46) | MaskForEditorSlot(56);

    // Every slot a piece of HEADGEAR can sit on, which is not the same set as
    // kHeadPartMask below and must not be confused with it. kHeadPartMask is
    // the two bits 24220 actually reads; this is the four slots a helmet ARMO
    // routinely DECLARES: head, hair, circlet and ears.
    //
    // ⚠ IT EXISTS BECAUSE HIDING ANY ONE OF THEM HIDES ALL FOUR (OS-148).
    // ExpandHideOverCoverage expands a hide over the whole piece occupying the
    // slot, so hiding the circlet bit of an Iron Helmet also hides its head,
    // hair and EAR bits, because that is what the ARMO declares. Excluding only
    // the two bits of kHeadPartMask would therefore have changed nothing: the
    // helmet sits on 31 and 42, and 42 alone drags the rest in.
    //
    // ⚠ 43 IS EARS AND IT IS THE ONE THAT BITES. A helmet declares it and
    // stages no geometry there, so hiding the helmet takes the ear head part
    // with it, and the ear does not come back cleanly: head parts are decided
    // when the head is BUILT, and clearing a mask bit afterwards leaves the
    // seam the field reported (screenshot 2026-08-06).
    //
    // ⚠⚠ THE PARAGRAPH ABOVE IS RIGHT ABOUT 43 AND WRONG ABOUT "HEAD PART".
    // Closed 2026-08-12 by EarProbe plus one offline read of the mesh.
    //
    // THE EARS ARE A DISMEMBER PARTITION OF THE HEAD MESH, NOT A HEAD PART AND
    // NOT AN ATTACHMENT. `!UBE\Head\FemaleHead_tangent.nif` carries a
    // BSDismemberSkinInstance with exactly two partitions:
    //
    //     partition 0   bodyPart 30   10758 verts, 4 bones   the face
    //     partition 1   bodyPart 43     856 verts, 1 bone    THE EARS
    //
    // The engine hides a dismember partition when a staged ARMA declares its
    // body part. A helmet declares 30/31/42/43, so staging one hides partition
    // 1 and the ears go. A circlet declares 42 alone and leaves them: field
    // confirmed 2026-08-12, ears fine under a circlet, gone under a helmet.
    //
    // ⚠⚠ AND THAT IS WHY FIVE ROUNDS OF SCENEGRAPH CENSUS CAME BACK CLEAN. A
    // partition is hidden BELOW the geometry: nothing is detached, no kHidden is
    // set anywhere, no bone moves, no shader or alpha flag changes, and the
    // vertex, triangle, bone and palette counts are identical whether the ears
    // draw or not. GONE, CULLED and a skinning fault were each eliminated by
    // measurement, and all three were eliminated because none of them was it.
    // The head's four bones are Spine2, both clavicles and NPC Head, all
    // ordinary and all resolving, so nothing physics-driven is involved either.
    //
    // ⚠ FITTING ROOM DOES NOT CAUSE IT AND CANNOT FIX IT BY MASK. Hiding a
    // helmet removes its GEOMETRY, not its equipped state, so the partition
    // stays hidden and the player now sees the gap the helmet used to cover.
    // RenderedWornMask cannot reach this: the cull is driven by the staged
    // ARMA's declared body parts at 3D BUILD time, not by the worn mask the
    // shim publishes, and the engine's own mask does not even carry bit 43 here
    // (measured real=0x4000108E).
    //
    // ✅ FIXED AND FIELD CONFIRMED 2026-08-12 by BipedPost::RestoreDismember-
    // Partitions, which re-enables the suppressed partitions after the cull and
    // again on the task queue. The deferred half is not optional: the
    // synchronous call alone landed and was undone, because the 3D attach
    // finishes after the pass that stages the armature.
    inline constexpr std::uint32_t kHeadgearSlotMask =
        MaskForEditorSlot(30) | MaskForEditorSlot(31) | MaskForEditorSlot(42) |
        MaskForEditorSlot(43);

    // Whether importing a preset should hide what the character is really
    // wearing on one slot (OS-142, narrowed by OS-148, widened back
    // 2026-08-12).
    //
    // A predicate rather than two conditions inline at the call site, because
    // the call site is in EditorUI.cpp and nothing compiles EditorUI.cpp. Every
    // genuine defect in this area has landed in that file.
    //
    //   a_worn           the slot really has gear on it
    //   a_presetNamesIt  the preset already decides this slot, so it is not ours
    //
    // ⚠⚠ THE HEADGEAR EXEMPTION IS GONE, AND BOTH OF ITS REASONS WENT FIRST.
    // OS-148 held headgear back for two of them, and neither survives.
    //
    // The damage was the real one: hiding any one of those four hides all four,
    // because the hide expands over the whole piece and a helmet ARMO declares
    // head, hair, circlet AND ears. Slot 43 is the ears, and a hidden helmet
    // used to leave the ear partition suppressed with no helmet over the gap.
    // That is FIXED and field-confirmed, by RestoreDismemberPartitions and its
    // deferred half; the note is at kHeadgearSlotMask above, and
    // BipedHooks fires exactly that restore whenever a hide mask carries one of
    // these bits. So the machinery this needs has been in place and unused.
    //
    // The second reason was that the click did something the hover had not
    // offered: the preview staged the preset raw while the click staged it with
    // the hides applied, so a set hid your helmet only once you committed. That
    // was a disagreement between two call sites rather than a fact about
    // headgear, and the preview builds the same imported outfit now.
    //
    // ⚠ SO THE PREDICATE IS THE WHOLE OF THE RULE AGAIN: gear you are really
    // wearing, on a slot the preset does not dress, is not part of the look the
    // preset is advertising. A player who saves that to their outfits should
    // get the preset, not the preset plus whatever they had on their head
    // (user 2026-08-12).
    constexpr bool ImportHidesWornSlot(std::uint32_t a_bit, bool a_worn,
                                       bool a_presetNamesIt) {
        (void)a_bit;
        return a_worn && !a_presetNamesIt;
    }

    // Body armor meshes CONTAIN the body: hiding these means re-applying the
    // race skin's per-slot ARMA, not culling a node.
    inline constexpr std::uint32_t kBodySkinMask =
        MaskForEditorSlot(32) | MaskForEditorSlot(33) | MaskForEditorSlot(37);

    // The player first-person biped renders torso/arms, hands/forearms and a
    // shield. Head and feet live only on the third-person biped. Shield hiding
    // is filtered by kNeverHideMask before these helpers are reached.
    inline constexpr std::uint32_t kFirstPersonArmorMask =
        MaskForEditorSlot(32) | MaskForEditorSlot(33) |
        MaskForEditorSlot(34) | MaskForEditorSlot(39);

    // Per-outfit hair visibility. kAuto is the zero so an outfit written before
    // this existed decodes to today's behaviour.
    //
    // The lever is engine 24220, which hides the hair head-part when the worn
    // mask's RACE_DATA::hairObject bit (slot 31) is set - verified on the 1.5.97
    // binary, see kHeadPartMask below. HAIR ONLY: the neighbouring bit culls the
    // entire head node, which is the symptom three rounds of headless-character
    // reports went into fixing, so it is deliberately not exposed.
    enum class HairMode : std::uint8_t { kAuto = 0, kShow = 1, kHide = 2 };

    // The ONLY two worn-mask bits engine function 24220 reads. Verified on the
    // 1.5.97 binary at 24220+0x081 and +0x0F8: it loads RACE_DATA::headObject
    // (+0x12C) and RACE_DATA::hairObject (+0x130) - kHead and kHair for every
    // vanilla and RaceCompatibility race - and tests the worn mask against
    // 1 << each. Nothing else in that function looks at the mask.
    //
    // ⚠ SLOT 30 HIDES THE WHOLE HEAD, not a head-part:
    //     +0x09A  test eax, edx              ; wornMask & (1 << headObject)
    //     +0x09E  or dword [head+0xf4], 1    ; NiAVObject::kHidden on the HEAD
    // which is exactly why full-face pieces (dragon priest masks) take slot 30
    // and open helmets do not. A bit left set for a slot whose geometry we
    // removed therefore yields a character with no head at all - the
    // Krosis/Ahzidal report, and OS-70. Slot 42 (circlet) used to be in this
    // mask; 24220 never reads it, so it was inert.
    inline constexpr std::uint32_t kHeadPartMask =
        MaskForEditorSlot(30) | MaskForEditorSlot(31);

    // The r25 GHOST FILTER, the at-source end of the bald-wig war (r18-r25).
    //
    // The engine hides the hair head-part because the worn mask carries bit 31
    // - and 24220's hide flag is LITERALLY `mask & (1 << slot)` (AE 24724
    // disassembly, 2026-08-23: `bl = wornMask & (1 << [part+0x130])`, then the
    // per-head-part hide 24913 is called with bl). On this save an 'Iron
    // Helmet' is really worn on 31+42 and stages NO geometry on the UBE race,
    // so the player went bald behind an invisible occupant and BipedPost's
    // ladders spent rounds re-enabling what the next build re-hid - a worn
    // slot is not a drawn slot. Dropping the ghost's bit BEFORE
    // the engine reads the mask ends the war instead of racing it - the
    // engine now calls its own hide with "show", which re-enables the
    // partition through the author's door.
    //
    // a_ghostMask carries the bits whose biped slot is OCCUPIED yet staged
    // NOTHING: objects[bit].item set with .addon and .part both empty. Those
    // two are written SYNCHRONOUSLY by 15500 on the worn-pass stack (DyeGate.h
    // records the field discipline), so the test cannot race a legit helmet
    // whose deferred partClone has simply not landed yet - that helmet has its
    // .addon, keeps its bit, and hides hair exactly as before.
    //
    // Only kHeadPartMask bits are dropped: 24220 reads nothing else, and the
    // fail-safe direction is the file's usual one - a wrongly-dropped bit is
    // hair visible under a helmet (clipping at worst), a wrongly-kept bit on
    // slot 30 is a headless character.
    constexpr std::uint32_t DropGhostHeadPartBits(std::uint32_t a_mask,
                                                  std::uint32_t a_ghostMask) {
        return a_mask & ~(a_ghostMask & kHeadPartMask);
    }
    // The r25 field case: real=0x108E (30-family gear incl. the ghost on 31),
    // ghost on 31 only - the hair bit falls, everything else stands.
    static_assert(DropGhostHeadPartBits(0x0000108Eu, MaskForEditorSlot(31)) ==
                  0x0000108Cu);
    // A ghost outside the head family (feet, 37) must change nothing.
    static_assert(DropGhostHeadPartBits(0x0000108Eu, MaskForEditorSlot(37)) ==
                  0x0000108Eu);
    // No ghosts, no change.
    static_assert(DropGhostHeadPartBits(0x0000108Eu, 0u) == 0x0000108Eu);

    // Which worn-mask bits still describe geometry Fitting Room actually
    // renders. Both inputs are measured by the render pass and published to the
    // shim - see PublishRenderedCoverage in BipedHooks.cpp. Nothing here is
    // inferred any more: predicting the engine's accept/reject was proven wrong
    // (OS-70c), and inferring which worn piece survived a style was the
    // `displaced` heuristic this replaces.
    //
    //   a_real    the engine's own worn mask
    //   a_hidden  coverage of everything the pass hid, expanded over whole
    //             pieces (ExpandHideOverCoverage)
    //   a_styled  the slots the pass's styles actually landed on, measured off
    //             the biped by StagedCoverageOf - NOT what they declare
    constexpr std::uint32_t RenderedWornMask(std::uint32_t a_real, std::uint32_t a_hidden,
                                             std::uint32_t a_styled) {
        return (a_real & ~a_hidden) | a_styled;
    }

    // What to answer the engine while Apparel Preview owns the look.
    //
    // ⚠ THREE KINDS OF CLAIM SHARE ONE MASK AND THEY STAND DOWN DIFFERENTLY.
    // This is the third one, found the same way the second was (OS-141, the
    // hair mode) and for the same reason: a preview invalidates SOME of what
    // Fitting Room was asserting, and dropping the rest with it is a bug each
    // time.
    //
    //   * STYLED - "our geometry covers these slots". Apparel Preview may have
    //     replaced exactly that geometry, so this cannot survive. Dropped.
    //   * HIDDEN - "the real gear on these slots is GONE from the scene".
    //     Previewing something does not put a hidden helmet back, so this is
    //     still true and must survive. Kept, and it is what this function adds.
    //   * The hair mode - what the PLAYER asked for. A hover cannot make that
    //     untrue. Kept, applied by the caller.
    //
    // The bug it fixes: with a helmet slot hidden, standing the whole shim down
    // returned the raw mask, which still carries the real helmet's bits because
    // the helmet is still EQUIPPED - only its geometry was removed. The engine
    // then culled hair for a helmet nobody could see (field 2026-08-06).
    //
    // Apparel Preview unions its own preview coverage onto this afterwards, so
    // a previewed helmet still hides hair correctly - by the piece actually on
    // the biped rather than by the one Fitting Room took off.
    constexpr std::uint32_t PreviewStandDownMask(std::uint32_t a_real,
                                                 std::uint32_t a_hidden) {
        return RenderedWornMask(a_real, a_hidden, 0u);
    }

    // Which biped slots hold geometry belonging to ONE piece, MEASURED off the
    // biped rather than inferred from anything the piece declares or the engine
    // returns.
    //
    // ⚠ THIS EXISTS BECAUSE ApplyArmorAddon's RETURN VALUE IS NOT A RENDER
    // SIGNAL. Settled in the field 2026-07-30 (OS-84). 'Steel Spell Knight
    // Helmet' declares slots 30/31/42/43; its only race-valid armature declares
    // slot 30 and carries both sex meshes; ApplyArmorAddon returned FALSE and
    // the helmet visibly RENDERED anyway (user-confirmed). 59% of that
    // session's head-slot injections "refused" the same way, which is simply
    // how common it is for an ARMO to declare more slots than its armatures
    // cover. Gating coverage on that bool claimed nothing for pieces that were
    // on screen, so the head drew inside the helmet (OS-86) and hair was never
    // hidden (OS-87).
    //
    // The measurement: 15500 writes objects[slot].addon = the ARMA it staged,
    // synchronously, before ApplyArmorAddon returns (see
    // research/hide-mechanism-final.md). The slots a piece really occupies are
    // therefore the ones whose staged armature is one of ITS OWN armatures.
    //
    // ⚠ Match on the piece's own armature list, never on "this slot holds
    // something". The race skin's armature occupies slot 30 on a bare
    // character, so a mere non-null test sets the head bit permanently and
    // every character goes headless - worse than the bug being fixed. An
    // identity match cannot make that mistake, because the skin's armatures are
    // not in a style's list.
    //
    // ⚠ Per-slot, never the declared mask. The two are NOT interchangeable
    // here: an open hood that declares 30+31 while its armature only covers 31
    // would, on the declared mask, get slot 30 claimed - and 24220 culls the
    // WHOLE HEAD on slot 30 (see kHeadPartMask). Measured claims the hair bit
    // and leaves the face alone. Under-claiming is at worst clipping;
    // over-claiming slot 30 is a broken character.
    //
    //   a_staged[b]   BipedAnim::objects[b].addon, or null where nothing staged
    //   a_slots       how many entries of a_staged are valid (clamped to 32)
    //   a_own         the piece's armature list; null entries never match
    template <class ArmaPtr>
    [[nodiscard]] constexpr std::uint32_t StagedCoverageOf(const ArmaPtr* a_staged,
                                                          std::uint32_t   a_slots,
                                                          const ArmaPtr*  a_own,
                                                          std::uint32_t   a_ownCount) {
        if (!a_staged || !a_own) {
            return 0;
        }
        std::uint32_t       covered = 0;
        const std::uint32_t slots   = a_slots < 32u ? a_slots : 32u;
        for (std::uint32_t b = 0; b < slots; ++b) {
            const ArmaPtr staged = a_staged[b];
            if (!staged) {
                continue;
            }
            for (std::uint32_t i = 0; i < a_ownCount; ++i) {
                if (a_own[i] == staged) {  // a null own entry never equals a staged one
                    covered |= (1u << b);
                    break;
                }
            }
        }
        return covered;
    }

    // Whether two pieces share an armature, by pointer identity - the same
    // identity StagedCoverageOf matches on, asked as its own question.
    //
    // ⚠ IT IS NOT A CURIOSITY. StagedCoverageOf can only tell two ARMOs apart
    // when their armature lists are disjoint, and this project's own catalog
    // proves they routinely are NOT: StyleCatalog dedupes enchanted variants by
    // SORTED ARMA POINTER SET equality and shows the player only the base
    // record, precisely because 'Iron Armor of Health' and plain 'Iron Armor'
    // are the same armatures (StyleCatalog.cpp). So a player wearing 'Iron
    // Helmet of Waterbreathing' who styles the base 'Iron Helmet' has ONE
    // pointer at objects[slot].addon that belongs to both lists, and any rule
    // that measures "which slots does the WORN piece still hold" gets the
    // STYLE's slots back. Callers that act on that answer must ask this first
    // and stand down when it is true.
    template <class ArmaPtr, class ArmoPtr>
    [[nodiscard]] inline bool SharesAnyArmature(ArmoPtr a_lhs, ArmoPtr a_rhs) {
        if (!a_lhs || !a_rhs) {
            return false;
        }
        if (a_lhs == a_rhs) {
            return true;  // the same record shares every armature with itself
        }
        for (ArmaPtr l : a_lhs->armorAddons) {
            if (!l) {
                continue;
            }
            for (ArmaPtr r : a_rhs->armorAddons) {
                if (l == r) {
                    return true;
                }
            }
        }
        return false;
    }

    // Which slots to cull when a STYLE lands on slot 30 while real worn gear
    // occupies slot 31 - the "a slot 30 style and a slot 31 helmet both render"
    // report (field 2026-08-11).
    //
    // ⚠⚠ THIS FUNCTION IS ALL GUARDS AND EVERY ONE OF THEM IS LOAD-BEARING.
    // The obvious version - "cull whatever the real 31-piece staged" - was
    // written, reviewed on three independent lenses and REFUTED on all three,
    // twice with a path to a defect WORSE than the one it fixes. The guards are
    // written in the order the review found them. Deleting any one of them
    // reopens a case that has a name below. DECLINING IS THE SAFE ANSWER EVERY
    // TIME: an undisplaced helmet is the clipping the report already describes,
    // and every failure direction here is a broken character.
    //
    //   a_styledCoverage    MEASURED off the biped - what our styles really took
    //   a_wornStagedCov     MEASURED - what the real 31-piece still holds
    //   a_wornDeclaredCov   DECLARED via StyleCoverageOf: ARMO mask u every ARMA
    //   a_sharesArmature    the 30-style and the worn 31-piece share an armature
    //
    // Returns the slots to cull, or 0 when the rule declines.
    // ⚠⚠ THE GROIN IS DECIDED BY WHAT IS WORN, NOT BY WHAT IS SEEN, AND
    // TRANSMOG IS EXACTLY THE GAP BETWEEN THOSE TWO. The New Gentleman and SOS
    // keep a piece on slot 52 (kBitGenitals) and choose its mesh from the
    // armour actually equipped on the body. A transmog puts a cuirass on screen
    // without equipping one, so TNG still reads a naked actor, still shows the
    // bare mesh, and it comes through the styled armour. Field 2026-08-20, and
    // the reporter's own words name the condition exactly: "if i transmog with
    // no body gear actually equipped".
    //
    // ⚠ SO REAL WORN BODY GEAR IS THE FIRST GUARD AND IT DECLINES. When the
    // player IS wearing something on 32, TNG has already answered the question
    // correctly off that piece, and culling here would be this system
    // overruling a decision that was made with better information than we have.
    // The gap only exists where the body slot is bare.
    //
    // ⚠ THE FAIL-SAFE DIRECTION IS "LEAVE IT ALONE", the same one the head
    // rules above take. Every guard returns 0, so anything unmeasured or
    // unexpected keeps today's behaviour rather than removing geometry another
    // mod owns.
    //
    // ⚠ A REVEALING STYLE IS THE KNOWN EXCEPTION AND IT IS NOT HANDLED HERE.
    // Some armours are authored to show the piece deliberately, and a player
    // who styles one of those wants it visible. Telling them apart needs the
    // style's own revealing flag, which is TNG's data and not ours to read
    // through a supported door yet. Hiding is the answer that is right for the
    // ordinary cuirass, which is what was reported; the exception is recorded
    // here so the next person does not think it was missed.
    // ⚠⚠ THE SECOND INPUT IS THE BIPED'S OBJECT, NOT A WORN ARMO, and reading
    // RealWorn for it made this rule decline every time it mattered (field
    // 2026-08-20, second round: zero `groin displace` lines while the fault
    // reproduced). TNG does not equip an inventory item on 52. It extends the
    // actor's SKIN to cover the slot, and a skin is the race's default rather
    // than something worn, so it owns no InventoryEntryData and
    // SnapshotRealWorn cannot see it: the dump read `slot 22: item=FE09AB02`,
    // the skin itself, while a_real.armo[kBitGenitals] was null. The biped
    // object is where the truth is, which is the same place headDisplaced
    // measures from.
    constexpr std::uint32_t GenitalDisplacementCull(std::uint32_t a_styledCoverage,
                                                   bool          a_bodyGearWorn,
                                                   bool          a_genitalObjectPresent) {
        if (a_bodyGearWorn) {
            return 0;  // TNG read a real piece and answered off it; do not argue
        }
        if (!a_genitalObjectPresent) {
            return 0;  // nothing on 52 to cull, so nothing to say
        }
        if ((a_styledCoverage & MaskForEditorSlot(32)) == 0) {
            return 0;  // no body style, so the actor looks as bare as it is
        }
        return MaskForEditorSlot(52);
    }

    constexpr std::uint32_t HeadDisplacementCull(std::uint32_t a_styledCoverage,
                                                 std::uint32_t a_wornStagedCov,
                                                 std::uint32_t a_wornDeclaredCov,
                                                 bool          a_sharesArmature) {
        // GUARD 1 - ONE DIRECTION ONLY, and the asymmetry is the point. A style
        // measuring onto 30 displaces the piece on 31; a style on 31 NEVER
        // displaces a piece on 30. Not an oversight and not symmetric: a
        // full-face piece legitimately covers a hood, while a hood does not
        // cover a face. MEASURED, not declared - a helmet declaring 30/31/42/43
        // whose armature only reaches 31 has not landed on the head.
        if ((a_styledCoverage & MaskForEditorSlot(30)) == 0) {
            return 0;
        }

        // GUARD 2 - THE ENCHANTED-VARIANT COLLISION. See SharesAnyArmature.
        // Without this, culling the "worn" piece culls the STYLE's own clone,
        // while styledCoverage still ORs bit 30 into the published mask
        // (RenderedWornMask applies `styled` LAST, so a wider `hidden` cannot
        // undo it) and 24220 then executes its `or dword [head+0xf4], 1` on the
        // HEAD node. No helmet AND no head, stable across every later rebuild.
        if (a_sharesArmature) {
            return 0;
        }

        // GUARD 3 - THE HOODED ROBE, and this is the one that surprises. SLOT
        // 31 IS NOT A HEADGEAR-ONLY SLOT: mage and monk robes claim 31 for the
        // hood as well as 32 for the body (see PrimaryBitFor above, field
        // 2026-08-10), and RealWorn fills armo[bit] from the DECLARED mask, so
        // armo[kBitHair] is routinely a ROBE rather than a helmet. Culling what
        // that piece staged reaches slot 32, and body-class bits sit permanently
        // outside kCullableAttachments, so ShowObjectNodes can never clear the
        // flag again and no skin is re-staged underneath. The result is an
        // invisible torso, re-culled on every rebuild - strictly worse than the
        // headless reports this file exists to prevent. A piece reaching outside
        // the headgear group is not headgear, so the rule stands down.
        if ((a_wornDeclaredCov & ~kHeadgearSlotMask) != 0) {
            return 0;
        }

        // GUARD 4 - never cull geometry this pass measured as its OWN, and
        // GUARD 5 - never name a slot outside the headgear group even if the
        // measurement somehow did. Guard 5 is belt-and-braces over guard 3 and
        // is kept because the two are refuted independently: guard 3 is about
        // WHICH PIECE, guard 5 is about WHICH SLOTS, and a future edit that
        // relaxes one must not silently inherit the other's protection.
        return a_wornStagedCov & kHeadgearSlotMask & ~a_styledCoverage;
    }

    // Force the hair head-part on or off, per the outfit's HairMode.
    //
    // Engine 24220 hides the hair head-part when the mask's
    // RACE_DATA::hairObject bit (slot 31) is set. Setting it hides hair,
    // clearing it shows hair, and kAuto leaves whatever the coverage decided.
    // HAIR ONLY - the neighbouring bit culls the whole head node, which is the
    // symptom three rounds of headless-character reports went into fixing, so
    // it is deliberately not exposed.
    constexpr std::uint32_t ApplyHairMode(std::uint32_t a_mask, HairMode a_mode) {
        switch (a_mode) {
            case HairMode::kShow:
                return a_mask & ~MaskForEditorSlot(31);
            case HairMode::kHide:
                return a_mask | MaskForEditorSlot(31);
            default:
                return a_mask;
        }
    }

    // Styling and hiding are separate policies. Shield styling is proven on
    // both live player biped passes, but shield hiding remains intentionally
    // unsupported. Keeping distinct masks prevents Hide All or a persisted
    // kHide entry from silently expanding the feature's scope.
    inline constexpr std::uint32_t kNeverStyleMask = 0;
    inline constexpr std::uint32_t kNeverHideMask  = MaskForEditorSlot(39);

    // Most armor styles may render without real gear in that slot. A shield is
    // different: it is a render-only replacement for an actually equipped
    // gameplay shield, never a way to conjure one.
    constexpr bool StyleRequiresWornItem(std::uint32_t a_bit) {
        return a_bit == kBitShield;
    }

    // a_requireWornEverywhere is the [General] bRequireWornForStyles setting:
    // it widens the shield's rule so a style only dresses an actor who is
    // already wearing something under it.
    //
    // ⚠ ASKS ABOUT THE STYLE'S COVERAGE, NOT ITS ANCHOR SLOT. One ARMO can
    // dress several slots at once and the engine stages it across all of them
    // (StyleCoverageOf: the ARMO's own mask unioned with every armor addon's).
    // Testing the anchor bit made the gate disagree with the thing it gates -
    // a robe covering body, hands and feet, anchored on a bare body while the
    // actor wore gauntlets, was thrown away even though there was real gear
    // under part of it. Coverage is what the engine acts on, so coverage is
    // what the rule asks about.
    //
    // ANY covered slot is enough, not every one. The strict reading refuses
    // that robe unless the actor wears body AND hands AND feet, which turns the
    // setting from "do not conjure a look out of nothing" into "wear a full set
    // or see nothing".
    //
    // ⚠ So this does NOT mean a bare slot always stays bare. A multi-slot style
    // still renders over the bare slots it covers, because coverage belongs to
    // the mesh: a robe addon spanning body and hands is one mesh with sleeves,
    // and there is no way to stage the robe without them. The setting is worth
    // describing to the player in those terms, not as a promise about slots.
    //
    // ⚠ The shield stays STRICT and slot-specific, above the widened rule and
    // regardless of it. A shield transmog replaces an equipped shield and must
    // never conjure one, so it asks about slot 39 itself; letting it ride in on
    // some other slot its ARMO happens to declare would hand the player a
    // shield they do not have.
    //
    // Kept as PARAMETERS rather than read from Settings in here: this header
    // compiles into the pure-logic test executables and names no engine or
    // config type, the same discipline as RuleModel.h. a_requireWornEverywhere
    // defaults to false so the shield rule - which is structural, not a
    // preference - still applies at any call site with no setting to hand.
    // a_styleCoverage has NO default on purpose: a site that forgot it would
    // silently fall back to the anchor-bit bug this replaced, so the compiler
    // asks for it instead.
    // Whether a shield style may render with NO real shield underneath.
    //
    // The rule above it says a shield is a render-only replacement for one the
    // actor really carries, never a way to conjure one, and that stood until
    // the field asked to preview shields while browsing (2026-08-11 evening).
    //
    // ⚠⚠ THE ONE CONDITION IS THAT BIPED OBJECT 9 IS EMPTY, AND IT IS THE
    // CRASH GUARD, NOT A TIDINESS RULE. Object 9 is SHARED between the shield
    // and the off-hand weapon. Once a style lands there, the pass's own
    // honesty restore walks every touched bit and writes
    // `objects[bit].item = realWorn[bit] ? realWorn[bit] : nakedSkin`
    // (BipedPost::RestoreRealItems). With no real shield that resolves to the
    // SKIN ARMO - so a shield conjured over a dual-wielder's off-hand WEAPON
    // replaces that weapon's item with an armour record, and Skyrim's next
    // UpdateEquipment pass crashes on it. PostPassArmorRestoreMask's own header
    // records exactly this, which is why it takes APPLIED coverage rather than
    // the requested mask.
    //
    // Measured off the biped, never inferred: an off-hand WEAPON is not in the
    // actor's armour worn-mask at all, so the mask cannot answer this and only
    // objects[9] can. A torch is the same shape.
    //
    // ⚠ THIS SAYS NOTHING ABOUT HIDING. kNeverHideMask still holds the shield:
    // conjuring geometry into an empty object is the reverse of removing
    // geometry another owner is using, and only the first one is safe.
    //
    // ⚠⚠ AND IT IS A PREVIEW, WHICH IS THE SECOND TERM. The first cut had only
    // the free-object test, so a shield style rendered whenever object 9
    // happened to be empty - INCLUDING AFTER THE EDITOR CLOSED, and for good.
    // The player walked out of Fitting Room carrying a shield they did not own
    // (field 2026-08-11 evening). That is precisely the thing the original
    // "never a way to conjure one" rule was protecting, and relaxing it for
    // browsing must not relax it for play. While the editor is open the shield
    // is a picture the player asked to see; the moment it closes, a shield
    // nobody equipped is a lie about what the character is carrying.
    constexpr bool ShieldStyleMayConjure(bool a_shieldObjectFree, bool a_previewing) {
        return a_shieldObjectFree && a_previewing;
    }

    // a_shieldConjurable is ShieldStyleMayConjure's verdict - measured object 9
    // AND the editor being open - and is consulted ONLY for the shield bit. It
    // defaults to false so every existing caller keeps the old refusal: a
    // caller that cannot measure the biped, or does not know whether anyone is
    // looking, must not be able to conjure a shield by forgetting an argument.
    //
    // a_previewing is the same second term for the WORN-EVERYWHERE test, and
    // for that test alone. In lore friendly (the setting on) a preset click
    // stages the whole look, and a character wearing only skin saw none of it
    // while the Presets page said "Trying on" (field 2026-09-02): every piece
    // was refused for having no real gear under it, which is the rule doing
    // outside the editor exactly what it is for. While the editor is open the
    // look on the actor being staged is a picture the player asked to see, so
    // the rule stands down for that actor; the close edge rebuilds (OnClose
    // discards the staging, and the discard kicks the refresh) and the rule is
    // back the moment the editor shuts. Same default and same reason as the
    // shield term: a caller that does not know whether anyone is looking keeps
    // the refusal. It does NOT stand in for the shield term - object 9 being
    // busy is a crash, not a preference - and it says nothing about the NPC
    // worn-required rule (RealWorn.h), which is a different rule with a
    // different job.
    constexpr bool CanApplyStyleBit(std::uint32_t a_bit, std::uint32_t a_realWornMask,
                                    std::uint32_t a_styleCoverage,
                                    bool a_requireWornEverywhere = false,
                                    bool a_shieldConjurable      = false,
                                    bool a_previewing            = false) {
        if (StyleRequiresWornItem(a_bit) && ((a_realWornMask >> a_bit) & 1u) == 0 &&
            !a_shieldConjurable) {
            return false;
        }
        if (!a_requireWornEverywhere || a_previewing) {
            return true;
        }
        return (a_realWornMask & a_styleCoverage) != 0;
    }

    // The Presets note, as numbers: how many pieces of a staged look resolve to
    // something the pass can stage, and how many of those draw in the fitting
    // room only, because the worn rule stands down for the preview and comes
    // back outside. Asked with the gate itself, previewing and not, so the
    // note and the render pass can only agree. a_coverage is indexed by bit
    // and holds 0 where a style did not resolve: a missing plugin is the row's
    // own message and not a piece of the look.
    struct PreviewOnlyTally {
        std::uint32_t pieces{ 0 };
        std::uint32_t bare{ 0 };
    };

    constexpr PreviewOnlyTally TallyPreviewOnly(std::uint32_t                  a_styleMask,
                                                std::span<const std::uint32_t> a_coverage,
                                                std::uint32_t                  a_realWornMask,
                                                bool a_requireWornEverywhere,
                                                bool a_shieldConjurable) {
        PreviewOnlyTally tally;
        const auto       bits =
            a_coverage.size() < 32 ? static_cast<std::uint32_t>(a_coverage.size()) : 32u;
        for (std::uint32_t bit = 0; bit < bits; ++bit) {
            if (((a_styleMask >> bit) & 1u) == 0 || a_coverage[bit] == 0) {
                continue;
            }
            ++tally.pieces;
            const bool here = CanApplyStyleBit(bit, a_realWornMask, a_coverage[bit],
                                               a_requireWornEverywhere, a_shieldConjurable,
                                               /*previewing*/ true);
            // Outside, nobody is looking and nothing conjures a shield.
            const bool outside = CanApplyStyleBit(bit, a_realWornMask, a_coverage[bit],
                                                  a_requireWornEverywhere, false, false);
            if (here && !outside) {
                ++tally.bare;
            }
        }
        return tally;
    }

    // Restore only slots hidden by the pass or covered by a style that actually
    // passed its actor-specific gate. The raw requested style mask is unsafe:
    // shield and off-hand weapon share biped object 9, so a rejected shield
    // style must not make armor honesty replace an off-hand WEAP with skin ARMO.
    constexpr std::uint32_t PostPassArmorRestoreMask(
        std::uint32_t a_hideMask, std::uint32_t a_appliedStyleCoverage) {
        return a_hideMask | a_appliedStyleCoverage;
    }

    // A body-class hide must restage naked skin separately on the 1P biped.
    // In practice this is torso and hands; feet are body-class but have no 1P
    // geometry.
    constexpr std::uint32_t FirstPersonBodySkinHideMask(
        std::uint32_t a_hiddenBodySkinMask) {
        return a_hiddenBodySkinMask & kFirstPersonArmorMask;
    }

    // Keep objects[].item honest only for first-person-visible objects touched
    // by the pass. Style ARMA coverage can add forearms even though it is not
    // an independently selected outfit bit.
    constexpr std::uint32_t FirstPersonArmorRestoreMask(
        std::uint32_t a_hideMask, std::uint32_t a_appliedStyleCoverage) {
        return PostPassArmorRestoreMask(a_hideMask, a_appliedStyleCoverage) &
               kFirstPersonArmorMask;
    }

    // Precondition for both: a_bit < 32 (else the mask shift is UB).
    constexpr bool IsBodySkinBit(std::uint32_t a_bit) { return (kBodySkinMask >> a_bit) & 1u; }
    constexpr bool IsHeadPartBit(std::uint32_t a_bit) { return (kHeadPartMask >> a_bit) & 1u; }

}  // namespace OS
