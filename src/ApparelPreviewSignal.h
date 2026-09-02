#pragma once

// PCH.h explicitly, matching every other engine-type-using header here: the
// build's per-target PCH makes it redundant inside this project, but a header
// naming SKSE:: types must not depend on its includer's include order.
#include "PCH.h"

// The wire format itself, engine-free so a pure-logic target can test it.
#include "ApparelPreviewWire.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

// Cross-mod contract with Apparel Preview (same studio), the mirror of AP's
// src/FittingRoomSignal.h - change BOTH or neither.
//
//   message type 'CLRP', receiver plugin "ApparelPreview"
//   no payload            = legacy "clear once"          (pre-0.5 AP)
//   one byte, nonzero     = editor OPENED: clear AND stay suspended
//   one byte, zero        = editor CLOSED: resume
//
// The suspend half exists because a one-shot clear was field-proven
// insufficient (2026-07-30): the editor alpha-hides the InventoryMenu, and an
// alpha-0 Scaleform panel STILL hit-tests, so item highlights kept moving
// under the editor and AP previewed every one of them over our staged look.
// AP decodes strictly (exactly one byte or it treats the message as legacy),
// and its own menu-close handling clears the suspend flag, so a crash or a
// missed close can never strand it suspended past the inventory session.
namespace OS {

    // ---- the reverse direction: Apparel Preview -> Fitting Room -----------
    //
    //   message type 'APPV', sender plugin "ApparelPreview"
    //   one byte, nonzero = a hover preview is on the player right now
    //   one byte, zero    = it is not
    //
    // ⚠ WHY THIS EXISTS: TWO SHIMS ON ONE MASK. Both mods hook
    // InventoryChanges::GetWornMask, and AP hooks after us, so the engine
    // calls AP, AP calls us, and each ORs its own bits into the other's
    // answer. Read straight off the two logs on 2026-08-04:
    //
    //   FR  wornmask real=0000008C -> 0000008D (hidden=00000001 styled=00000081)
    //   AP  wornmask real=0000008D -> shimmed=0000108F
    //
    // AP's "real" is our output. The consumer reads exactly two bits of the
    // result (see the GetWornMask shim in BipedHooks.cpp): slot 30 toggles
    // kHidden on the FaceGen head node, slot 31 on the hair. Our styled
    // coverage keeps slot 30 set, AP's preview replaces the geometry that was
    // standing in for the culled head, and the player goes headless. A slot-30
    // style plus a previewed helmet reproduces it every time.
    //
    // NEITHER SIDE CAN FIX THIS FROM BITS ALONE, which is why the answer is a
    // signal and not more mask arithmetic. Our slot-30 claim is honest, a
    // style does cover it; AP's preview mask is honest, the helmet does cover
    // 31 and 42. What changed is WHOSE geometry sits on the biped, and no mask
    // records that - our styled part was on biped object 1 while the bit we
    // set was 0. So while a preview is live we answer with the vanilla mask
    // and let AP union its coverage onto that instead. It is the fail-safe
    // direction the shim already documents: a head visible under a helmet is
    // at worst clipping, a head culled over nothing is a broken character.
    //
    // Anything other than exactly one byte reads as "no preview", so an older
    // or malformed sender leaves our shim alone rather than disabling it.
    //
    // ⚠ AND A SECOND MESSAGE, 'APSM', CARRIES THE SLOT MASK (OS-145). The worn
    // mask was only ever half of what a preview breaks: the DYE walk paints by
    // channel index within a slot, a preview swaps the geometry on that slot,
    // and the channel that coloured your cuirass then colours whatever Apparel
    // Preview just hung there. Standing the dye walk down wholesale is not the
    // answer - it strips every other garment for the length of a hover - so the
    // walk needs to skip exactly the previewed slots and nothing else. The wire
    // format, and why the mask travels beside 'APPV' rather than inside it, are
    // in ApparelPreviewWire.h.
    //
    // ⚠ THE PAIR IS ONE ATOMIC WORD. The mask means nothing without the active
    // flag and the flag is useless without the mask, and they arrive in two
    // messages, so they are packed and swapped together rather than stored as
    // two values a reader has to catch in agreement.
    inline std::atomic<std::uint64_t> g_apparelPreviewState{ 0 };

    // ⚠ COMPARE-EXCHANGE, NOT LOAD-MODIFY-STORE. Each message updates its own
    // half and must leave the other half exactly as it found it. SKSE dispatch
    // is synchronous on the sender's thread today, which makes a plain
    // read-modify-write correct by an invariant this file does not own and
    // cannot enforce; the loop below is correct without it.
    //
    // The transform is passed in and lives in ApparelPreviewWire.h, so the
    // state machine is testable without an engine and this function is only
    // ever about the atomicity.
    template <typename Fn>
    inline void MergeApparelPreviewState(Fn a_transform) {
        std::uint64_t cur = g_apparelPreviewState.load(std::memory_order_relaxed);
        while (!g_apparelPreviewState.compare_exchange_weak(cur, a_transform(cur),
                                                            std::memory_order_relaxed,
                                                            std::memory_order_relaxed)) {
        }
    }

    inline void SetApparelPreviewActive(const void* a_data, std::size_t a_length) {
        const bool active = DecodePreviewActive(a_data, a_length);
        MergeApparelPreviewState(
            [active](std::uint64_t a_cur) { return WithPreviewActive(a_cur, active); });
    }

    inline void SetApparelPreviewSlots(const void* a_data, std::size_t a_length) {
        const auto slots = DecodePreviewSlots(a_data, a_length);
        if (!slots.known) {
            // A malformed 'APSM' is not a statement that the preview covers
            // nothing. Leave the last good pair alone and let the walk keep
            // whatever it already knew.
            return;
        }
        MergeApparelPreviewState(
            [mask = slots.mask](std::uint64_t a_cur) { return WithPreviewSlots(a_cur, mask); });
    }

    // Read from inside the engine's GetWornMask frame, so relaxed and lock-free.
    [[nodiscard]] inline bool ApparelPreviewActive() {
        return (g_apparelPreviewState.load(std::memory_order_relaxed) & kPreviewActiveBit) != 0;
    }

    // The slots a live preview occupies, or ZERO when there is no preview or
    // this Apparel Preview is too old to name them.
    //
    // ⚠ ZERO MEANS "DO NOTHING DIFFERENT", never "the preview covers nothing".
    // Every caller acts by EXCLUDING these slots from something, so an unknown
    // mask has to leave that something alone. See DyeSkipMask's own note.
    //
    // Two callers with the same value and different words for it: the dye walk
    // skips painting them, and the styling pass stands its headgear down when
    // any of them is headgear. Named for what the mask IS rather than for what
    // the first caller did with it.
    [[nodiscard]] inline std::uint32_t ApparelPreviewKnownSlots() {
        return DyeSkipMask(
            UnpackPreviewState(g_apparelPreviewState.load(std::memory_order_relaxed)));
    }

    inline void NotifyApparelPreview(bool a_editorOpen) {
        static constexpr std::uint32_t kClearPreviewMsg = 'CLRP';
        // ⚠ SELF-HEAL, and the reason a stranded flag cannot outlive an editor
        // session. AP suspends while our editor is open and clears on its own
        // menu close, so "no preview is live" is true by construction at both
        // edges. Latching it here means a crashed or missing AP can leave the
        // flag set for at most one editor open/close, instead of disabling our
        // shim until the game restarts.
        //
        // ⚠ THE WHOLE WORD, WHICH CLEARS THE KNOWN BIT TOO, and that is the
        // fail-safe direction rather than an oversight. Zero reads back as "no
        // preview, slots unknown", and unknown skips nothing (see DyeSkipMask),
        // so an Apparel Preview that has stopped talking can never leave a slot
        // permanently exempt from its own dye. A live AP restores both halves
        // on its very next publish, which it sends before every refresh.
        g_apparelPreviewState.store(0, std::memory_order_relaxed);
        if (auto* messaging = SKSE::GetMessagingInterface()) {
            std::uint8_t open = a_editorOpen ? 1 : 0;
            messaging->Dispatch(kClearPreviewMsg, &open, sizeof(open), "ApparelPreview");
        }
    }

}  // namespace OS
