#pragma once

#include "HeadPartPlan.h"      // Reject, the offer rule the apply path now shares
#include "HeadPartSlotPlan.h"  // Slot, the discovered-type rules (pure)
#include "PreviewGrid.h"       // TextureSwapEntry (pure)

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Swapping a head part on the PLAYER: hair, eyes or brows. One mechanism, one
// set of hazards, three types of part.
//
// This is HairStyle's machinery, which shipped and was field proven for hair
// alone, generalised over the part type when OS-161 asked for eyes and brows.
// HairStyle.h is now a forwarder onto it, so hair keeps running the same code
// it always did rather than a copy of it that can drift.
//
// ⚠ PLAYER ONLY, AND ENFORCED HERE RATHER THAN ASKED OF CALLERS. Apply writes
// the actor base and then puts the change on screen. Until OS-230 that was a
// full head rebuild, and on a named NPC a rebuild replaces her complexion:
// measured on 2026-07-31, a dark elf came back with a pale human skin, and it
// is the WRONG SKIN rather than the right one mis-shaded, so no repaint can put
// it back. NpcHair exists precisely because of that, and does scenegraph
// surgery instead so it never rebuilds anything.
//
// ⚠ THE REFUSAL STAYS NOW THAT THE PLAYER ROUTE IS THE ENGINE'S PER-PART SWAP
// (HeadPart.cpp, QueueRebuildAndRepaint), and not out of caution for its own
// sake: NpcHair is field-proven across dozens of rounds on exactly the heads
// this would touch, the swap has been measured on the player only, and the
// full reset is still this route's fallback when the engine call is absent.
// Widening it is its own change with its own field round.
//
// The refusal lives in this module because NpcHair.h's own hard-won note
// applies here word for word: one place that cannot be forgotten beats a rule
// in a comment. HairStyle stated the same restriction in prose and relied on
// every caller checking IsPlayerRef, which held only because there were two
// callers. Eyes and brows add more.
//
// ⚠ THE FOLLOWER ROUTE IS NOT A SCOPING VARIANT OF THIS ONE. It is NpcHair's
// cull-and-attach, seven steps in a non-negotiable order (docs/re/
// head-part-attach.md), and porting it to a new part type is a separate piece
// of work with its own field runs. Do not "widen" this module to NPCs.
namespace RE {
    class Actor;
    class BGSHeadPart;
}

namespace OS::HeadPart {

    // The part types Fitting Room offers, deliberately a closed set of our own
    // rather than the engine's HeadPartType. Two reasons: the header stays free
    // of RE:: types like its neighbours, and /we4062 turns a switch over this
    // enum into the compiler listing every site that needs a case when a new
    // kind arrives. Face and scars are the remaining candidates and neither is
    // in scope, so leave the enum small until one is.
    //
    // ⚠ kFacialHair ARRIVED THAT WAY (OS-196) AND THE NET WORKED, but only
    // because the ternaries were converted to default-less switches FIRST, in
    // their own commit. Adding a case to a ternary is silent
    // ([[widen-an-enum-by-making-the-compiler-list-the-sites]]): the staged-key
    // accessor would have written brows for beards, on a path no test reaches.
    enum class Kind : std::uint8_t {
        kHair,
        kEyes,
        kBrows,
        kFacialHair,
    };

    // The engine's head-part type number, which is what every lookup below is
    // really keyed on. The four Kinds above are four particular values of it.
    //
    // ⚠ THIS EXISTS BECAUSE A LOAD ORDER CAN INVENT SLOTS AND Kind CANNOT
    // GROW TO MATCH. RaceMenu's convention is that a mod wanting a head-part
    // slot of its own picks an unused number above the engine's seven and owns
    // it; MEASURED on this rig 2026-08-13, horns use 32 and 106 and Chooey's
    // ears use 110. Those are discovered at run time and unbounded, so they can
    // never be compile-time cases, which is exactly the property Kind's
    // closedness is for. See HeadPartSlotPlan.h.
    //
    // Everything below comes in a Kind flavour and a Slot flavour, and the Kind
    // one is the Slot one with SlotOf applied. One implementation, so the four
    // shipped kinds cannot drift away from the discovered ones.
    using Slot = std::uint32_t;

    [[nodiscard]] Slot SlotOf(Kind a_kind);

    // ⚠ THERE IS NO "NeedsFacegenBake" HERE ANY MORE. It refused every
    // morph-carrying part on an invented slot for nine hours on 2026-08-18 and
    // the field called it a regression the same day: NK's horns render through
    // ChangeHeadPart + our rebuild (measured off the 18:29 log, see the note
    // above CurrentForSlot in HeadPart.cpp). Do not bring a rule over the
    // record back for a fault that belongs to one mesh.

    // Every head-part slot in this load order that the engine did not name, in
    // display order, each with the count of parts carrying it, the plugin that
    // defines the first non-extra one, and a label that is never empty.
    //
    // Walked once and cached: the form array does not change after load. Empty
    // on a load order with no such mods, which is the common case and is not an
    // error.
    //
    // ⚠ NOT FILTERED BY ACTOR. A slot appearing here means the load order has
    // it, not that this character is offered anything in it. Ask
    // AvailableForSlot before drawing a row, or a male character gets an empty
    // ears row that reads as a broken feature.
    [[nodiscard]] const std::vector<HeadPartSlotPlan::Slot>& DiscoveredSlots();

    struct Entry {
        RE::BGSHeadPart* part{ nullptr };
        std::string      name;    // full name when the record has one, else editor ID
        std::string      source;  // defining plugin filename, empty when it has none

        // The card scene: the part's own model plus each extra part's (a
        // hair ships its hairline as an extra part, and a card without it
        // shows a bald rim). Resolved here, on the main thread, when the
        // list is built, because a preview build must never walk the form
        // graph (the preview grid's standing rule). Empty when the part has
        // no model, which is exactly "no scene" and draws the cross.
        std::vector<std::string> modelPaths;

        // The part's TNAM texture set as swap data (OS-192), parallel to
        // modelPaths when present: one apply-to-every-mesh entry on the MAIN
        // model, nothing on the extras, which keep their own look the way
        // the engine leaves them. EMPTY for a TNAM-less part, so its disk
        // key stays byte-identical (the pinned rule).
        std::vector<std::vector<PreviewGrid::TextureSwapEntry>> swaps;
    };

    // Everything the OFFERED list depends on: the actor, plus the race and sex
    // of its BASE, which is exactly what AvailableFor filters on.
    //
    // ⚠⚠ A CACHE OF THAT LIST MUST KEY ON THIS AND NOT ON THE ACTOR'S FORM ID.
    // RaceMenu changes race and sex without changing the form id, so a cache
    // keyed on identity alone never invalidates and keeps offering the previous
    // character's parts. Field 2026-08-09: switching to male still listed the
    // female eyes, brows, facial hair and hair, and it survived closing and
    // reopening the editor because the open path deliberately keeps the cached
    // lists (clearing them there is what made the plugin filter vanish,
    // 2026-08-08). The armour fit cache learned the same lesson separately;
    // StyleCatalog::EnsureFitCurrent's note says "SEX COUNTS TOO, AND IT DID
    // NOT BEFORE".
    struct OfferKey {
        std::uint32_t actor{ 0 };
        std::uint32_t race{ 0 };
        std::int32_t  sex{ -1 };

        friend bool operator==(const OfferKey&, const OfferKey&) = default;

        // False for an actor with no base, so a caller cannot mistake "could
        // not read the character" for a real, cacheable answer.
        [[nodiscard]] bool Valid() const { return actor != 0 && sex >= 0; }
    };

    // ⚠ READS THE SAME TWO FIELDS AvailableFor DOES, off the ACTOR BASE rather
    // than off Actor::GetRace(), and it lives beside it so the pair cannot
    // drift. Those two race answers are not always the same one and one can lag
    // the other (StyleCatalog::EnsureFitCurrent records a field case).
    [[nodiscard]] OfferKey OfferKeyFor(RE::Actor* a_actor);

    // Would this actor be OFFERED this part right now? The same question
    // AvailableFor asks of every record, asked of one.
    //
    // ⚠⚠ THIS GATES THE APPLY PATH, AND NOTHING ELSE DOES. The engine does not
    // police it: TESNPC::ChangeHeadPart matches on head-part TYPE alone and
    // never reads the part's sex flags or race list (decompiled on both
    // runtimes, 2026-08-09), so a stored reference to a female eye lands on a
    // male character's record and stays there. That is the whole defect behind
    // "I changed sex and the same eyes persist".
    //
    // WhyNotFor returns kNone for "yes", a reason for "no", and NULLOPT for
    // "cannot read this character", which is not a verdict about the part.
    [[nodiscard]] std::optional<HeadPartPlan::Reject> WhyNotFor(RE::Actor* a_actor,
                                                                Kind a_kind,
                                                                RE::BGSHeadPart* a_part);

    // ⚠ FALSE when the character cannot be read. This decides whether a part
    // is WRITTEN to an actor's record, so uncertainty must not authorise it.
    [[nodiscard]] bool IsValidFor(RE::Actor* a_actor, Kind a_kind,
                                  RE::BGSHeadPart* a_part);

    // The same questions for a discovered slot. Same posture on an unreadable
    // character: nullopt is not a verdict, and IsValidForSlot reads it as false
    // because it gates a write.
    [[nodiscard]] std::optional<HeadPartPlan::Reject> WhyNotForSlot(
        RE::Actor* a_actor, Slot a_slot, RE::BGSHeadPart* a_part);
    [[nodiscard]] bool IsValidForSlot(RE::Actor* a_actor, Slot a_slot,
                                      RE::BGSHeadPart* a_part);

    // Every part of this kind the engine would offer this actor, filtered the
    // way character creation filters. The rule is HeadPartPlan::Judge, which is
    // unit tested; this is the lookup around it, which cannot be.
    //
    // Sorted by name and deduplicated by form, so the order is stable across
    // sessions on an unchanged load order and an index means the same thing
    // twice. Empty when the actor has no race or no parts to reason about.
    [[nodiscard]] std::vector<Entry> AvailableFor(RE::Actor* a_actor, Kind a_kind);

    // The actor's current part of this kind, honouring overlays the way the
    // engine's own GetCurrentHeadPartByType does. Null when they have none,
    // which is a real state and not an error.
    [[nodiscard]] RE::BGSHeadPart* CurrentFor(RE::Actor* a_actor, Kind a_kind);

    // Swap the part on the base, then swap its geometry on the live face node
    // the way the character editor's own sliders do (OS-230; the full 3D reset
    // this used to issue is the fallback when that engine call is absent).
    //
    // Captures the original on the first call for this actor and kind, so
    // Restore can undo it. Returns false when nothing changed, when the actor
    // cannot be styled, or when the actor is not the player.
    bool Apply(RE::Actor* a_actor, Kind a_kind, RE::BGSHeadPart* a_part);

    // Put back whatever they had before Fitting Room first touched this kind,
    // and forget the capture. A no-op when nothing was ever applied.
    bool Restore(RE::Actor* a_actor, Kind a_kind);

    // What Restore would put back, for the editor to label a reset control.
    // nullopt when this actor's part of this kind has not been touched.
    [[nodiscard]] std::optional<Entry> CapturedFor(RE::Actor* a_actor, Kind a_kind);

    // Whether Restore would do anything, without building the Entry to find
    // out. The editor asks this per frame, per kind, to decide whether to draw
    // a reset control at all, and CapturedFor allocates two strings answering a
    // question that needs none.
    [[nodiscard]] bool HasCapture(RE::Actor* a_actor, Kind a_kind);

    // How many times Fitting Room has WRITTEN this actor's part of this kind:
    // every Apply that reached ChangeHeadPart plus every Restore that put one
    // back. Monotonic for the session, never reset by a restore, so two
    // readings taken either side of some other event can be compared.
    //
    // ⚠ THE DIFFERENCE IS THE ANSWER, NOT THE VALUE. HeadEditorSink reads it at
    // both edges of a character-editor visit to ask "did WE move this part
    // while the menu was open", which is the whole difference between the
    // player changing their look and our own stand-down being mistaken for it.
    // The capture cannot answer that question: it is written once and dropped.
    [[nodiscard]] std::uint32_t WriteCountFor(RE::Actor* a_actor, Kind a_kind);

    // Re-point an EXISTING capture at whatever the actor wears right now.
    //
    // ⚠ ONLY WHEN THE ROW ALREADY EXISTS, and the refusal is load bearing
    // rather than tidy. Creating one would tell a later Wear::kOwn push to
    // write a part Fitting Room never replaced, which is the restore path
    // putting a face back that nobody asked it to touch.
    void ReseedCapture(RE::Actor* a_actor, Kind a_kind);

    // Whether this actor can be edited at all, and it is the player test. The
    // editor asks so it can disable a row and say why, rather than offering a
    // control that silently refuses.
    [[nodiscard]] bool CanEdit(RE::Actor* a_actor);

    // Drop every capture, for every kind, for a save revert. Deliberately
    // restores nothing: the old character's base is being torn down and
    // replaced by the incoming save, so writing to it would hit the wrong
    // character. Same posture as HairColor::Clear.
    //
    // ⚠⚠ THIS HAD NO CALLER UNTIL 2026-08-16 AND THAT WAS THE HAIR LEAK. The
    // key is the actor's form id, and the player is 0x14 in EVERY save, so a
    // capture taken on one character sat there describing the next one. The
    // ladder's own bottom rung is Restore (OutfitSession's PushPlayerHeadParts,
    // Wear::kOwn), which runs on the load path, so character B was WRITTEN
    // character A's hair with nobody touching anything: reported as hair
    // leaking across characters, and as Base gear leaving the hair on. Wired
    // into Persistence::RevertCallback beside HairColor::Clear, where this
    // comment always said it ran.
    void Clear();

    // One captured "what they had before Fitting Room touched this slot",
    // flattened to something a save can hold: the defining plugin plus the
    // local form id, exactly the pair StyleRefKey uses and for the same reason
    // (a runtime form id carries the load order's index in its top byte).
    //
    // ⚠ NOT AN Entry. Entry carries live pointers, names and model paths for
    // the editor; this is wire shape, and PersistenceCodec has to be able to
    // talk about it without engine types.
    struct Capture {
        Slot          slot{ 0 };
        std::string   modName;
        std::uint32_t localFormID{ 0 };
    };

    // The PLAYER's captures, for the co-save.
    //
    // ⚠ A NULL CAPTURE IS REPORTED, as an empty plugin name and a zero form id.
    // It means "they had no part in this slot before", and RestoreSlot can act
    // on it now by wearing the slot's placeholder. It used to be dropped here
    // as a row that could only ever be refused; with the refusal gone, dropping
    // it would mean a staged horn or ear that survives one save could never be
    // taken off again by the reset, because the reload path finds the part
    // already worn and captures nothing.
    [[nodiscard]] std::vector<Capture> SnapshotCaptures();

    // Put a save's captures back, keyed onto the player as they are now.
    //
    // ⚠ WITHOUT THIS, CLEARING ON REVERT WOULD TRADE ONE BUG FOR ANOTHER.
    // The capture is the only record of what a character looked like before
    // Fitting Room, and the actor base it describes is written into the save,
    // so a reload with no capture leaves Base gear with nothing to put back.
    // Clear-on-revert kills the leak; this is what keeps undo working across
    // that same load. A row whose plugin is gone is dropped with a log line,
    // never guessed at.
    void AdoptCaptures(const std::vector<Capture>& a_rows);

    // For log lines and UI labels.
    [[nodiscard]] const char* KindName(Kind a_kind);

    // ---- the same questions, asked of a discovered slot -------------------
    // Each of these is what the Kind overload above delegates to. A slot that
    // happens to be one of the four shipped kinds behaves identically through
    // either door, which is the point: there is one implementation.
    //
    // ⚠ PLAYER ONLY, ENFORCED IN ApplySlot, exactly as the Kind route is. A
    // discovered slot changes nothing about why: Apply forces a head rebuild
    // and that replaces a named NPC's complexion.
    [[nodiscard]] std::vector<Entry> AvailableForSlot(RE::Actor* a_actor, Slot a_slot);
    [[nodiscard]] RE::BGSHeadPart*   CurrentForSlot(RE::Actor* a_actor, Slot a_slot);
    // The slot's "none": the part the authoring mod ships to take the thing OFF
    // (No Horns, EDNoHorn, CDE00). It is a part that names no mesh, judged by
    // the same rule the pane lists by, first by name then form id. Null when the
    // slot ships none, which is a real answer: that slot cannot be emptied.
    //
    // ⚠ ONE LOOKUP FOR BOTH HALVES. RestoreSlot wears it when the character had
    // nothing in the slot before, and the row's right click stages it to take
    // a part off; two readers with two definitions of "the none card" would
    // pick two different parts the day a mod ships two nameless ones.
    [[nodiscard]] RE::BGSHeadPart*   PlaceholderForSlot(RE::Actor* a_actor, Slot a_slot);
    bool                             ApplySlot(RE::Actor* a_actor, Slot a_slot,
                                               RE::BGSHeadPart* a_part);
    // Put back what they had before Fitting Room first touched this slot.
    // "Nothing" is expressible now: when the capture is null and the slot ships
    // a placeholder, that placeholder is worn, which is what a player picking
    // the none card does by hand. Only a slot with no placeholder is refused.
    bool                             RestoreSlot(RE::Actor* a_actor, Slot a_slot);
    [[nodiscard]] bool               HasCaptureSlot(RE::Actor* a_actor, Slot a_slot);
    [[nodiscard]] std::uint32_t      WriteCountForSlot(RE::Actor* a_actor, Slot a_slot);
    void                             ReseedCaptureSlot(RE::Actor* a_actor, Slot a_slot);

    // The label for a slot, for logs and rows. The four shipped kinds give
    // KindName's words; a discovered slot gives what DiscoveredSlots resolved.
    [[nodiscard]] std::string SlotName(Slot a_slot);

    // ---- the switched-apply head reconcile --------------------------------
    //
    // After a race or sex switch the actor base's part list is the new truth
    // and the LIVE face node still wears the old character's geometry: the
    // engine's own race-switch reload never rebuilds the player's head out of
    // menu (measured across five field rounds, 2026-08-22; the eyes swap in
    // the 05:05 log is the proof, it detached 'MaleEyesHumanLightBlue' from a
    // node the apply had already "regenerated"). A bare per-part regeneration
    // cannot cure that, because it finds each part's geometry BY NAME and the
    // new parts' names are not in the old node.
    //
    // This walks the base's current parts against a PRE-SWITCH snapshot the
    // caller took, and runs the engine's per-part swap (OS-230's machinery,
    // the character editor's own slider path) for every type that changed
    // hands: old geometry detached, new geometry built, painted AND skinned
    // (SkinSingleGeometry is the call FSMP patches, so SMP hair comes out
    // simulating). Old types the new list dropped are detached with nothing
    // built; identical parts are left alone for the caller to rebake. Both
    // halves of the engine call are null-guarded (decompiled, SE 26468).
    //
    // GAME THREAD ONLY, player only (the module's standing posture), inline:
    // the caller owns the ordering around it. An empty snapshot skips the
    // swaps entirely, because swapping with nothing to detach on a node that
    // still wears its parts would stack two characters' geometry (the
    // follower two-hairs class).
    struct SwitchSwapReport {
        int  swapped{ 0 };    // type changed hands: old out, new built+painted+skinned
        int  kept{ 0 };       // same part both sides: left for the caller's rebake
        int  removed{ 0 };    // old type the new list lacks: detached, nothing built
        bool available{ false };  // engine swap id resolved, face node present, player
    };
    [[nodiscard]] SwitchSwapReport SwapSwitchedParts(
        RE::Actor* a_actor, const std::vector<RE::BGSHeadPart*>& a_preSwitchParts);

}  // namespace OS::HeadPart
