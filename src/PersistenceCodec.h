#pragma once

#include "Outfit.h"
#include "SkinPlan.h"  // 'SKIN': the rows below are its ActorSkin per actor

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace OS {

    inline constexpr std::uint32_t kCodecVersion = 25;

    // How many dyed shapes ONE head-part slot may carry on the wire.
    //
    // ⚠ A DECODE CEILING, NOT A DESIGN LIMIT, and it is separate from
    // kMaxCustomHeadParts because the two bound different things: that one is
    // "at most one part per slot", while a head-part dye is keyed by slot AND
    // part, so a hair slot legitimately holds one entry per shape. MEASURED
    // 2026-08-17: a hair contributed nine parts on slot 3, of which one was
    // dyeable. Thirty-two is generous against that by a wide margin and still
    // small enough that a corrupt count cannot commit a large allocation.
    inline constexpr std::uint32_t kMaxDyedShapesPerHeadSlot = 32;
    // v1 -> v2: appended a per-outfit weapon block (weapon + quiver
    // transmog) after the existing armor slot block. CRITICAL: the co-save
    // 'LIBR' record is every save's own outfit library, so Decode MUST keep
    // accepting v1 bytes forever - if it ever stopped, every save written
    // before this version would silently lose its whole library on load.
    // See the v1-branch below and the v1-decode test in test_persistence.cpp.
    //
    // v2 -> v3: appended a per-outfit BODY block (OBody preset name + ORefit
    // mode). Same rule, same shape: every older version stays readable
    // forever, each block stops where its version stopped, and a missing
    // block leaves its fields at the "changes nothing" default. There is
    // deliberately NO migration pass - nothing is transformed, an old record
    // simply does not fill in the newer fields.
    //
    // v3 -> v4: appended optional Right/Left weapon overrides after the body
    // block. The original v2 class entry is still the Both fallback, so every
    // existing co-save remains semantically identical.
    //
    // v4 -> v5: appended a per-outfit hair-visibility byte after the per-hand
    // weapon block. v4 bytes simply stop before it and decode to kAuto.
    //
    // v5 -> v6: appended a per-outfit hair-COLOUR block (enable byte + r + g + b)
    // after the hair-visibility byte. v5 bytes stop before it and decode to a
    // disabled tint, which means "leave the character's own colour alone". The
    // RGB is stored and the colour FORM is not, so nothing load-order dependent
    // is ever written; see HairTint in Outfit.h for why.
    //
    // v6 -> v7: appended a per-outfit hair STYLE reference (mod name + local
    // form ID) after the hair-colour block. An empty reference is the
    // leave-their-own-hair-alone value.
    //
    // v7 -> v8: appended a per-outfit DYE block (a non-empty-only list keyed
    // by slot bit, three channels per entry) after the hair-style block. v7
    // bytes stop before it and decode with every dye channel off.
    //
    // v8 -> v9: the dye block's per-slot entry gained a CHANNEL COUNT ahead of
    // the channels, and the channel count itself rose from three to eight. v8
    // entries are still read as exactly three, which is what their version
    // implies. The count exists so that the next change to the number of
    // channels is NOT a version bump: a reader consumes every channel on the
    // wire and stores the ones it has room for.
    //
    // v9 -> v10: appended a per-outfit WEAPON DYE block after the armour dye
    // block. Same non-empty-only shape, keyed by weapon class and hand instead
    // of slot bit, with its own channel count per entry for the same reason.
    // v9 bytes stop before it and decode with no weapon dye at all.
    //
    // ⚠ AN ENTRY IS WRITTEN FOR EVERY STORED VALUE, INCLUDING AN EMPTY ONE ON
    // A HAND. "Left is explicitly empty" renders undyed while "Left has no
    // override" inherits the Both colour, so dropping empties on the way out
    // would silently turn the first into the second across a save.
    //
    // v10 -> v11: appended the stable custom body-preset ID after weapon dye.
    // An empty string means the outfit uses its installed OBody name (or does
    // not manage the body). Existing v10 records stop before it and decode to
    // that exact default.
    //
    // v11 -> v12: appended per-outfit EYE and BROW references (OS-161) after the
    // custom body ID, each a mod name plus local form ID exactly like the hair
    // style at v7. An empty reference is the leave-their-own-alone value, so v11
    // bytes stop before both and decode to precisely that.
    //
    // v15 -> v16: appended a per-outfit FACIAL HAIR reference (OS-196) behind
    // the brow pair, the same shape and the same empty-means-leave-alone rule,
    // so v15 bytes stop before it and decode to exactly that.
    //
    // v16 -> v17: appended the per-outfit EYE colour, four bytes on hairTint's
    // v6 terms: a presence bit and an RGB whose cleared value is the
    // leave-her-own-eyes-alone answer.
    //
    // v24 -> v25: the per-channel bytes gained the CUT, where metal starts on
    // the piece for the envmask dye modes, one byte appended after the v19
    // blend. Written inside the channel like v13's strength and v19's blend,
    // so the boundary is pinned on GetChannel directly, and a v24 channel
    // decodes to 128: halfway up the class gap, which is where the automatic
    // cut landed on every mask measured, so every existing dye paints as it
    // did. ⚠ v24 IS THE VERSION 1.1.8 WRITES, the one live on Nexus, so the
    // dyed v24 forge in test_persistence.cpp is the layout every player's
    // save carries.
    //
    // v23 -> v24: the ARMOUR DYE block gained the GARMENT in its key. An entry
    // was a slot bit and its channels; it is a bit, a mod name, a form id and
    // its channels now. This is the one dye change that is NOT an append, and
    // it could not be: the fault was that a colour belonged to a SLOT, so
    // restyling handed the old piece's colour to the new one (see
    // ArmourDyeEntry for the field report). A block that names only a bit
    // cannot express the fix.
    //
    // ⚠ READING BACKWARDS IS FREE AND EXACT. The armor slot block decodes
    // BEFORE this one, so a v23 entry naming bit 2 arrives when the outfit
    // already knows which garment sits at bit 2, and SetDye keys the colour to
    // it. Every old save migrates onto the piece the player was actually
    // looking at. Nothing is guessed and nothing is dropped.
    //
    // ⚠ WRITING FORWARDS IS NOT. A v23 reader handed v24 bytes would read the
    // mod name where it expects a channel count, so kCodecVersion gates it the
    // way every other break does.
    //
    // v22 -> v23: appended the per-outfit PUSH-UP mode, one byte, range checked
    // on the way in. A v22 record stops before it and decodes to kNone, which is
    // what every outfit written before this meant. The byte is the MODE and not
    // the morphs: which named morphs it becomes depends on the body the player
    // is wearing at the time, so nothing about the recipe is stored here and a
    // save moved to another body picks up that body's recipe.
    //
    // v20 -> v21: appended the per-outfit CUSTOM HEAD-PART SLOTS, a count then
    // that many (slot number, mod name, local form id) triples. These are the
    // head-part types a load order invented rather than ones the engine named,
    // which is why they are counted rather than fixed fields like the eyes and
    // the brows: the numbers belong to whichever mod claimed them and are only
    // known at run time. A v20 record stops before the count and decodes to an
    // outfit naming none, which is what every outfit written before this meant.
    //
    // v19 -> v20: appended the per-outfit SECOND EYE colour, four bytes on the
    // terms the eye tint and the sclera already sit on: a presence bit and an
    // RGB whose cleared value means the eye takes one colour. v19 bytes stop
    // before it and decode to precisely that.
    //
    // ⚠ THE SECOND BUMP IN A DAY, and that is a cost paid deliberately rather
    // than an oversight. v19's own note says a refused read here loses the
    // whole library, so the ritual runs in full again: 19 goes into both
    // accepted lists as a literal, NpcoInnerLibrVersion freezes it, and the
    // forged end-to-end test for it sits beside the others. The alternative was
    // to guess a week ago at a field a field report had not asked for yet.
    //
    // v18 -> v19: TWO fields, one bump. Every dye channel gained a BLEND byte
    // (an OS::DyeBlend::Choice, written inside the channel behind the flake),
    // and the outfit gained an EYE BLEND byte appended behind the sclera
    // colour. Both defaults are zero and zero DEFERS rather than naming a
    // curve, so a v18 record decodes to the install's own setting on armour and
    // to the eye's own default on eyes, which is byte for byte what it already
    // renders.
    //
    // ⚠ ONE BUMP FOR BOTH HALVES, on the v14 note's reasoning below. A refused
    // read here is destructive - a version this decoder does not accept costs
    // the whole outfit library and every NPC assignment - so the armour byte
    // and the eye byte are widened together rather than as each is needed. One
    // migration event, not two.
    //
    // v17 -> v18: appended the per-outfit SCLERA colour, four more bytes on
    // exactly the same terms, directly behind the eye colour it partners. The
    // iris mask splits an eye texture into the disc and the white around it,
    // and this is the white's half of the pair.
    //
    // ⚠ THE FEATURE SHIPPED FOR ONE BUILD WITH NO FORMAT AT ALL. Eyes and brows
    // were a character edit written straight to the actor base, which the game
    // saves by itself, so nothing was stored here. Making them per outfit (user
    // 2026-08-06) is what created the need for a version. Nothing has to migrate:
    // an outfit made under that build simply names no eyes and no brows, which
    // is the same as any older record and is the correct answer for it.
    //
    // v13 -> v14: the per-channel bytes gained a MODE and a SECOND STOP, five
    // more bytes inside every channel: mode, secondSet, r2, g2, b2. Same shape
    // as v13's strength byte and the same reasoning, one version on: it is
    // written INSIDE the channel rather than appended to the record, so a dyed
    // library's v13 bytes are not a prefix of its v14 bytes and the boundary
    // test cannot forge its fixture by truncating. Every default is today's
    // behaviour, so a v13 record decodes to a flat single-colour channel that
    // paints exactly as it always did.
    //
    // ⚠ ONE BUMP FOR ALL THREE FAMILIES, DELIBERATELY. The mode, the second stop
    // and the DyeMaterial finish blocks were widened together rather than as
    // they were needed, because a refused read here is destructive: a v9 save
    // opened on a branch expecting less logged `refused library record` and
    // dropped the whole outfit library plus every NPC assignment. One migration
    // event rather than three. After this, every further dye is a JSON entry
    // with no save-format risk at all.
    //
    // v14 -> v15: the per-channel bytes gained the FLAKE byte and both
    // DyeMaterial blocks (palette then player: sheenSet, sheenR, sheenG,
    // sheenB, glossSet, gloss), thirteen more bytes inside every channel.
    // Written inside the channel like v13's strength and v14's ramp, so the
    // boundary is pinned on GetChannel directly with an undyed forge, the same
    // way. The v14 note below saying the finish blocks were "deliberately not
    // here" ends at this version: the finish gained its first writer on
    // 2026-08-08 (EffectiveMaterial went live at rung 5), which is exactly the
    // "the moment something does, it is a version bump" that note promised.
    //
    // ⚠ The per-channel bytes moved into PutChannel/GetChannel at this version
    // and BOTH blocks call them. The dye finish work widens what a channel
    // carries; with the bytes written inline in two places that change is two
    // encoders and two decoders to find, and a miss in either is a silently
    // truncated record. GetChannel takes the version explicitly rather than
    // implying it, because the channel COUNT on the wire only makes the number
    // of channels free to move. It says nothing about how many bytes each one
    // occupies, so widening the struct is still a version bump and the decoder
    // still has to know which layout it is reading.

    // Guard against absurd allocations from corrupt data.
    inline constexpr std::uint32_t kMaxStringLen = 512;
    inline constexpr std::uint32_t kMaxOutfitCount = static_cast<std::uint32_t>(kMaxOutfits);

    namespace detail {
        inline void PutU32(std::vector<std::byte>& a_out, std::uint32_t a_v) {
            for (int i = 0; i < 4; ++i) {
                a_out.push_back(static_cast<std::byte>((a_v >> (i * 8)) & 0xFF));
            }
        }
        inline void PutU8(std::vector<std::byte>& a_out, std::uint8_t a_v) {
            a_out.push_back(static_cast<std::byte>(a_v));
        }
        // Clamped, not just length-prefixed: Reader::Str below refuses
        // anything over kMaxStringLen, so writing more than that would
        // produce bytes this codec's own decoder rejects - turning one
        // over-long field (an outfit name, a pinned name, a rule id) into
        // total data loss for whatever record it rides in, encode-clean and
        // decode-refused. A probe with a 600-char pinnedName demonstrated
        // exactly this for the 'RULE' record before this clamp existed.
        // Truncating confines the damage to the one field that was too
        // long, matching what Str() would have accepted anyway.
        inline void PutStr(std::vector<std::byte>& a_out, const std::string& a_s) {
            const std::size_t len =
                a_s.size() > kMaxStringLen ? std::size_t{ kMaxStringLen } : a_s.size();
            PutU32(a_out, static_cast<std::uint32_t>(len));
            for (std::size_t i = 0; i < len; ++i) {
                a_out.push_back(static_cast<std::byte>(a_s[i]));
            }
        }

        // A large blob (the rules JSON document below is the one user of
        // this today), for a field that can legitimately run well past
        // kMaxStringLen - PutStr/Str above are sized for short things like
        // mod/outfit names. Unlike PutStr, this does NOT clamp: the only
        // ceiling is whatever the caller enforces on the record as a whole
        // (Persistence.cpp's kMaxRecordBytes, already checked before these
        // bytes are ever written), so a large field is never silently
        // truncated the way a short one is.
        inline void PutBlob(std::vector<std::byte>& a_out, const std::string& a_s) {
            PutU32(a_out, static_cast<std::uint32_t>(a_s.size()));
            for (char c : a_s) {
                a_out.push_back(static_cast<std::byte>(c));
            }
        }

        // ---- one channel on the wire, in ONE place ---------------------------
        //
        // ⚠ EVERY BLOCK THAT WRITES A DyeChannel CALLS THESE. The armour dye
        // block and the weapon dye block both do. That is the whole point: the
        // dye finish work widens what a channel carries, and with these bytes
        // written inline per block that becomes two encoders and two decoders
        // to find, where a miss in either is a silently truncated record.
        //
        // ⚠ WHAT IS NOT HERE, DELIBERATELY: the DyeMaterial blocks. DyeChannel
        // already carries `palette` and `player`, and nothing writes them yet.
        // The moment something does, it is a version bump and it happens here,
        // gated on a_version. Adding them without one loses every finish on the
        // first save; adding them here without the gate makes every v10 record
        // unreadable.
        inline void PutChannel(std::vector<std::byte>& a_out, const DyeChannel& a_ch);

        struct Reader {
            std::span<const std::byte> data;
            std::size_t                pos{ 0 };
            bool                       ok{ true };

            bool Need(std::size_t n) {
                if (!ok || pos + n > data.size()) {
                    ok = false;
                    return false;
                }
                return true;
            }
            std::uint32_t U32() {
                if (!Need(4)) {
                    return 0;
                }
                std::uint32_t v = 0;
                for (int i = 0; i < 4; ++i) {
                    v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[pos + i])) << (i * 8);
                }
                pos += 4;
                return v;
            }
            std::uint8_t U8() {
                if (!Need(1)) {
                    return 0;
                }
                return std::to_integer<std::uint8_t>(data[pos++]);
            }
            std::string Str() {
                const auto len = U32();
                if (!ok || len > kMaxStringLen || !Need(len)) {
                    ok = false;
                    return {};
                }
                std::string s(reinterpret_cast<const char*>(data.data() + pos), len);
                pos += len;
                return s;
            }
            // Counterpart to PutBlob above: same large-string exception,
            // parameterized by a_maxLen instead of the hardcoded
            // kMaxStringLen Str() enforces - the caller supplies its own
            // ceiling (typically the remaining buffer size, already bounded
            // upstream), rather than being stuck with the short-field cap.
            std::string Blob(std::size_t a_maxLen) {
                const auto len = U32();
                if (!ok || len > a_maxLen || !Need(len)) {
                    ok = false;
                    return {};
                }
                std::string s(reinterpret_cast<const char*>(data.data() + pos), len);
                pos += len;
                return s;
            }
        };

        inline void PutChannel(std::vector<std::byte>& a_out, const DyeChannel& a_ch) {
            PutU8(a_out, a_ch.set ? 1u : 0u);
            PutU8(a_out, a_ch.r);
            PutU8(a_out, a_ch.g);
            PutU8(a_out, a_ch.b);
            // v13. Unconditional because Encode only ever emits the CURRENT
            // version; only the reader has to know about older shapes.
            //
            // ⚠ THIS IS THE FIRST FIELD THIS FORMAT ADDED IN THE MIDDLE RATHER
            // THAN AT THE END, and every version before it appended. That is
            // why the v13 boundary test cannot forge its fixture by truncating
            // the way all the earlier ones do: a dyed library's v12 bytes are
            // not a prefix of its v13 bytes. An UNDYED library is the clean
            // forge, because the two callers below reach this function only for
            // slots that carry a dye at all.
            PutU8(a_out, a_ch.strength);
            // v14. Unconditional for the same reason: Encode only ever emits the
            // CURRENT version, and only the reader has to know older shapes.
            //
            // ⚠ FIVE BYTES, AND secondSet IS ONE OF THEM RATHER THAN BEING
            // INFERRED FROM THE STOPS. "No second stop" and "a second stop that
            // happens to be black" are different dyes, and a reader that guessed
            // from r2|g2|b2 == 0 would turn the second into the first.
            PutU8(a_out, a_ch.mode);
            PutU8(a_out, a_ch.secondSet ? 1u : 0u);
            PutU8(a_out, a_ch.r2);
            PutU8(a_out, a_ch.g2);
            PutU8(a_out, a_ch.b2);
            // v15: flake, then the two finish blocks, palette before player.
            // Presence bits travel explicitly for the reason DyeMaterial keeps
            // two of them: "no sheen" and "a sheen that is black" are different
            // materials and a reader must not have to guess from the bytes.
            PutU8(a_out, a_ch.flake);
            PutU8(a_out, a_ch.palette.sheenSet ? 1u : 0u);
            PutU8(a_out, a_ch.palette.sheenR);
            PutU8(a_out, a_ch.palette.sheenG);
            PutU8(a_out, a_ch.palette.sheenB);
            PutU8(a_out, a_ch.palette.glossSet ? 1u : 0u);
            PutU8(a_out, a_ch.palette.gloss);
            PutU8(a_out, a_ch.player.sheenSet ? 1u : 0u);
            PutU8(a_out, a_ch.player.sheenR);
            PutU8(a_out, a_ch.player.sheenG);
            PutU8(a_out, a_ch.player.sheenB);
            PutU8(a_out, a_ch.player.glossSet ? 1u : 0u);
            PutU8(a_out, a_ch.player.gloss);
            // v19: the blend, behind both finish blocks rather than beside the
            // flake it belongs with in the struct. The wire order is history,
            // not layout: appending is what lets a v18 reader stop where v18
            // stopped, and inserting it at the flake would move eighteen bytes
            // that every already-saved channel has in the older places.
            PutU8(a_out, a_ch.blend);
            // v25: the cut, appended after the blend for the reason the blend
            // sits after the finish blocks: the wire order is history, and an
            // appended byte is one a v24 reader stops before.
            PutU8(a_out, a_ch.cut);
        }

        // Returns false only when the stream ran out.
        [[nodiscard]] inline bool GetChannel(Reader& a_r, std::uint32_t a_version,
                                             DyeChannel& a_out) {
            const auto s = a_r.U8();
            const auto r = a_r.U8();
            const auto g = a_r.U8();
            const auto b = a_r.U8();
            if (!a_r.ok) {
                return false;
            }
            a_out = DyeChannel{ s != 0, r, g, b };
            // ⚠ v13 AND LATER ONLY, and the default of 255 on the struct is what
            // makes an older record correct rather than merely readable: a
            // channel written before this byte existed was applied at full
            // force, so full force is what it must decode to. A default of 0
            // would read every older save without error and repaint it grey.
            if (a_version >= 13) {
                a_out.strength = a_r.U8();
                if (!a_r.ok) {
                    return false;
                }
            }
            // ⚠ v14 AND LATER ONLY, and the struct defaults are what make an
            // older record correct rather than merely readable. A channel
            // written before these bytes existed was a single flat colour, and
            // mode 0 with secondSet false is exactly that. Reading garbage into
            // mode would turn every dye in every existing save into a ramp
            // between a colour and black.
            //
            // ⚠ THE ASSIGNMENT ABOVE IS WHAT MAKES SKIPPING THIS SAFE.
            // `a_out = DyeChannel{ s != 0, r, g, b }` replaces the caller's
            // buffer wholesale, so an older version leaves these five at their
            // struct defaults rather than at whatever the previous channel in
            // the loop happened to put there.
            if (a_version >= 14) {
                a_out.mode      = a_r.U8();
                a_out.secondSet = a_r.U8() != 0;
                a_out.r2        = a_r.U8();
                a_out.g2        = a_r.U8();
                a_out.b2        = a_r.U8();
                if (!a_r.ok) {
                    return false;
                }
            }
            // ⚠ v15 AND LATER ONLY. A v14 channel decodes with flake 0 and both
            // finish blocks at their defaults, which is exactly what a v14 build
            // rendered for it: the finish had no writer until the same day this
            // version was cut, so nothing real is being dropped.
            if (a_version >= 15) {
                a_out.flake            = a_r.U8();
                a_out.palette.sheenSet = a_r.U8() != 0;
                a_out.palette.sheenR   = a_r.U8();
                a_out.palette.sheenG   = a_r.U8();
                a_out.palette.sheenB   = a_r.U8();
                a_out.palette.glossSet = a_r.U8() != 0;
                a_out.palette.gloss    = a_r.U8();
                a_out.player.sheenSet  = a_r.U8() != 0;
                a_out.player.sheenR    = a_r.U8();
                a_out.player.sheenG    = a_r.U8();
                a_out.player.sheenB    = a_r.U8();
                a_out.player.glossSet  = a_r.U8() != 0;
                a_out.player.gloss     = a_r.U8();
                if (!a_r.ok) {
                    return false;
                }
            }
            // ⚠ v19 AND LATER ONLY, AND ITS OWN GATE RATHER THAN THE ONE ABOVE.
            // A v18 channel decodes with blend 0, which DEFERS to the install's
            // setting: that is what a v18 build rendered for it, because the
            // setting was the only answer that existed.
            if (a_version >= 19) {
                a_out.blend = a_r.U8();
                if (!a_r.ok) {
                    return false;
                }
            }
            // ⚠ v25 AND LATER ONLY. A v24 channel decodes with cut 128, halfway
            // up the class gap, which is what every v24 build's automatic cut
            // drew for it (Otsu landed at 0.49 to 0.51 of the gap on every mask
            // measured), so nothing an existing save shows moves.
            if (a_version >= 25) {
                a_out.cut = a_r.U8();
                if (!a_r.ok) {
                    return false;
                }
            }
            return true;
        }
    }  // namespace detail

    inline std::vector<std::byte> Encode(const OutfitLibrary& a_lib) {
        using namespace detail;
        std::vector<std::byte> out;
        PutU32(out, static_cast<std::uint32_t>(a_lib.Count()));
        PutU32(out, static_cast<std::uint32_t>(a_lib.ActiveIndex() + 1));  // 0 == none

        for (const auto& o : a_lib.All()) {
            PutStr(out, o.name);
            PutU8(out, o.favorite ? 1 : 0);

            // Only non-passthrough slots are written.
            std::uint32_t count = 0;
            for (std::uint32_t b = 0; b < kBitCount; ++b) {
                if (o.EntryFor(b).kind != SlotEntry::Kind::kPassthrough) {
                    ++count;
                }
            }
            PutU32(out, count);
            for (std::uint32_t b = 0; b < kBitCount; ++b) {
                const auto& e = o.EntryFor(b);
                if (e.kind == SlotEntry::Kind::kPassthrough) {
                    continue;
                }
                PutU32(out, b);
                PutU8(out, static_cast<std::uint8_t>(e.kind));
                PutStr(out, e.style.modName);
                PutU32(out, e.style.localFormID);
            }

            // Weapon dimension (v2), same non-passthrough-only shape as the
            // armor slot block above, indexed by WeaponClass instead of an
            // editor-slot bit.
            std::uint32_t weaponCount = 0;
            for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
                if (o.WeaponEntryFor(static_cast<WeaponClass>(c)).kind != SlotEntry::Kind::kPassthrough) {
                    ++weaponCount;
                }
            }
            PutU32(out, weaponCount);
            for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
                const auto  wc = static_cast<WeaponClass>(c);
                const auto& e  = o.WeaponEntryFor(wc);
                if (e.kind == SlotEntry::Kind::kPassthrough) {
                    continue;
                }
                PutU8(out, static_cast<std::uint8_t>(c));
                PutU8(out, static_cast<std::uint8_t>(e.kind));
                PutStr(out, e.style.modName);
                PutU32(out, e.style.localFormID);
            }

            // Body dimension (v3): OBody preset name + ORefit mode. Written
            // unconditionally (unlike the two blocks above, which are
            // non-passthrough-only) because it is a fixed two-field record,
            // not a variable-length list - an empty name IS the "no change"
            // value and costs 4 bytes.
            PutStr(out, o.obodyPreset);
            PutU8(out, static_cast<std::uint8_t>(o.orefit));

            // Per-hand weapon overrides (v4). Presence is meaningful:
            // kPassthrough means explicitly show the real weapon in this hand,
            // while no record means inherit the legacy Both class value.
            std::uint32_t handCount = 0;
            for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
                const auto wc = static_cast<WeaponClass>(c);
                for (const auto hand : { WeaponHand::Right, WeaponHand::Left }) {
                    if (o.WeaponOverrideFor(wc, hand)) {
                        ++handCount;
                    }
                }
            }
            PutU32(out, handCount);
            for (std::size_t c = 0; c < kWeaponClassCount; ++c) {
                const auto wc = static_cast<WeaponClass>(c);
                for (const auto hand : { WeaponHand::Right, WeaponHand::Left }) {
                    const auto& over = o.WeaponOverrideFor(wc, hand);
                    if (!over) {
                        continue;
                    }
                    PutU8(out, static_cast<std::uint8_t>(c));
                    PutU8(out, static_cast<std::uint8_t>(hand));
                    PutU8(out, static_cast<std::uint8_t>(over->kind));
                    PutStr(out, over->style.modName);
                    PutU32(out, over->style.localFormID);
                }
            }

            // Hair visibility (v5). One byte, written unconditionally: like the
            // body block above it is a fixed field, not a variable-length list,
            // and kAuto IS the "leave it alone" value.
            PutU8(out, static_cast<std::uint8_t>(o.hair));

            // Hair colour (v6). Four fixed bytes, written unconditionally for the
            // same reason the visibility byte above is: it is a fixed field, not a
            // variable-length list, and "disabled" IS the leave-it-alone value.
            PutU8(out, o.hairTint.set ? 1u : 0u);
            PutU8(out, o.hairTint.r);
            PutU8(out, o.hairTint.g);
            PutU8(out, o.hairTint.b);

            // Hair style (v7). A form reference written unconditionally, for the
            // same reason as the two blocks above: it is a fixed field, and an
            // EMPTY reference IS the leave-their-own-hair-alone value.
            PutStr(out, o.hairStyle.modName);
            PutU32(out, o.hairStyle.localFormID);

            // Dye (v8, widened in v9). A non-empty-only list keyed by slot bit,
            // the same shape as the armor slot block above rather than the
            // fixed-field shape the hair blocks use: the channels times four
            // bytes across 32 slots would cost hundreds of bytes on every
            // outfit, and almost every outfit dyes nothing.
            //
            // ⚠ v9 WRITES THE CHANNEL COUNT PER DYED SLOT. v8 wrote exactly
            // three with the number implied by the version, which made the
            // channel count part of the format and every change to it another
            // version bump. With the count on the wire a reader takes
            // min(wire, kDyeChannelCount) and skips the rest, so the constant
            // moves freely from here on and this block never needs versioning
            // again for that reason alone.
            // ⚠⚠ v24 PUTS THE GARMENT IN THE KEY. Up to v23 an entry was a bit
            // and its channels, which made a colour a property of the SLOT: see
            // ArmourDyeEntry for the field report that cost. The entry carries
            // the garment's mod and form now, so a save holds a colour per
            // piece and a piece that is not being worn keeps its colour rather
            // than having nowhere on the wire to live.
            //
            // ⚠ THE READER MIGRATES v23 AND EARLIER FOR FREE, and that is not
            // luck: the armor slot block is decoded ABOVE this one, so by the
            // time an old (bit, channels) entry arrives the outfit already knows
            // what is in that bit, and SetDye keys it to exactly that garment.
            // An old save therefore lands on the piece the player was looking at
            // when they dyed it, which is the only defensible reading of it.
            std::uint32_t dyeCount = 0;
            o.ForEachArmourDye(
                [&](std::uint32_t, const StyleRefKey&, const SlotDye&) { ++dyeCount; });
            PutU32(out, dyeCount);
            o.ForEachArmourDye([&](std::uint32_t a_bit, const StyleRefKey& a_garment,
                                   const SlotDye& a_dye) {
                PutU32(out, a_bit);
                PutStr(out, a_garment.modName);
                PutU32(out, a_garment.localFormID);
                PutU32(out, static_cast<std::uint32_t>(kDyeChannelCount));
                for (const auto& ch : a_dye.channels) {
                    PutChannel(out, ch);
                }
            });

            // Weapon dye (v10). The same non-empty-only shape as the armour dye
            // block above, keyed by class and hand rather than by slot bit,
            // because weapon slots are an unrelated index space from the armour
            // editor bits and the off-hand weapon shares biped 9 with the
            // shield. Merging the two keys would collide there head on.
            //
            // ⚠ EVERY STORED ENTRY IS WRITTEN, INCLUDING AN EMPTY ONE ON A
            // HAND. Filtering on Any() here would be the obvious mirror of the
            // armour block above and it would be wrong: an explicitly emptied
            // Left override renders UNDYED, while no Left override at all
            // inherits the Both colour. Dropping the first turns it into the
            // second on the next load, silently. ForEachWeaponDye yields
            // exactly what is stored, and Outfit prunes an empty Both itself so
            // that "stored" and "meaningful" are the same set.
            //
            // ⚠ CANONICAL ORDER comes from ForEachWeaponDye, not from insertion
            // order, so two outfits holding the same colours encode to the same
            // bytes and the golden test below pins a real layout rather than
            // whichever swatch a fixture happened to click first.
            std::uint32_t weaponDyeCount = 0;
            o.ForEachWeaponDye(
                [&](WeaponClass, WeaponHand, const SlotDye&) { ++weaponDyeCount; });
            PutU32(out, weaponDyeCount);
            o.ForEachWeaponDye([&](WeaponClass c, WeaponHand h, const SlotDye& d) {
                PutU8(out, static_cast<std::uint8_t>(c));
                PutU8(out, static_cast<std::uint8_t>(h));
                PutU32(out, static_cast<std::uint32_t>(kDyeChannelCount));
                for (const auto& ch : d.channels) {
                    PutChannel(out, ch);
                }
            });

            // Custom body reference (v11). The custom preset's mutable display
            // name and slider payload live in the isolated store; an outfit
            // persists only this rename-stable identity.
            PutStr(out, o.customBodyPresetId);

            // Eye and brow type (v12). Two form references written
            // unconditionally, for the reason the hair style at v7 is: they are
            // fixed fields, and an EMPTY reference IS the leave-their-own-alone
            // value rather than an absent one.
            PutStr(out, o.eyes.modName);
            PutU32(out, o.eyes.localFormID);
            PutStr(out, o.brows.modName);
            PutU32(out, o.brows.localFormID);

            // Facial hair (v16), appended on exactly the terms the pair above
            // sits on: one more fixed field whose empty value is the
            // leave-their-own-alone answer, so a v15 record decodes to it.
            PutStr(out, o.facialHair.modName);
            PutU32(out, o.facialHair.localFormID);

            // Eye TINT (v17), four bytes on exactly the terms hairTint
            // sits on at v6: a presence bit and an RGB, written
            // unconditionally, whose cleared value IS the
            // leave-her-own-eyes-alone answer. So a v16 record decodes to
            // an outfit that tints no eyes, which is what every outfit
            // made before this feature existed meant.
            PutU8(out, o.eyeTint.set ? 1u : 0u);
            PutU8(out, o.eyeTint.r);
            PutU8(out, o.eyeTint.g);
            PutU8(out, o.eyeTint.b);

            // Sclera colour (v18), four bytes behind the eye colour it
            // partners, same terms: cleared means leave the white alone.
            PutU8(out, o.scleraTint.set ? 1u : 0u);
            PutU8(out, o.scleraTint.r);
            PutU8(out, o.scleraTint.g);
            PutU8(out, o.scleraTint.b);

            // Eye BLEND (v19), one byte behind the pair it governs. Zero
            // defers to the eye's own default, so a v18 record decodes to
            // the recolour every confirmed iris already renders through.
            PutU8(out, o.eyeBlend);

            // The SECOND eye colour (v20), four bytes on the eye tint's own
            // terms. Cleared means the eye takes one colour, which is what
            // every outfit written before this meant.
            PutU8(out, o.eyeTint2.set ? 1u : 0u);
            PutU8(out, o.eyeTint2.r);
            PutU8(out, o.eyeTint2.g);
            PutU8(out, o.eyeTint2.b);

            // Head-part slots this load order invented (v21): horns, cat ears
            // and anything else authored at a head-part type the engine never
            // named. MEASURED on the dev rig 2026-08-13: five of them, at 32,
            // 69, 71, 106 and 110.
            //
            // ⚠ COUNTED AND VARIABLE, unlike every head-part field above it,
            // and that is forced rather than chosen. Eyes, brows and facial
            // hair are fixed fields because the engine names exactly those
            // types; a discovered slot's number belongs to whichever mod
            // claimed it, so the set is only known at run time and cannot be
            // named fields. A count of zero is what every outfit written before
            // this meant and is what a v20 record decodes to.
            //
            // ⚠ THE SLOT NUMBER IS WRITTEN BESIDE THE REFERENCE, not implied by
            // position. A load order that loses the horn mod must leave the ear
            // entry meaning ears, and an entry whose slot no longer exists has
            // to be recognisable as such rather than silently becoming the next
            // slot's.
            PutU32(out, static_cast<std::uint32_t>(o.customHeadParts.size()));
            for (const auto& part : o.customHeadParts) {
                PutU32(out, part.slot);
                PutStr(out, part.key.modName);
                PutU32(out, part.key.localFormID);
            }

            // HEAD-PART DYE (v22): a colour on a horn, an ear or one of the
            // shapes a modded hair carries that is not a strand.
            //
            // ⚠ THE KEY IS A SLOT AND A PART REF, not the slot alone, and the
            // block above is why it cannot be folded into that one. Those entries
            // say which part the outfit PUTS ON a slot, at most one per slot;
            // these say what colour one part's geometry takes, and a hair
            // contributes nine parts on one slot (field 2026-08-17). Two entries
            // here can share a slot and neither is the other's key.
            //
            // ⚠ CANONICAL ORDER comes from ForEachHeadPartDye, on the weapon
            // block's terms: two outfits holding the same colours must encode to
            // the same bytes, and the sort cannot fall out of a walk over a fixed
            // index space because the slots are invented at run time.
            //
            // A count of zero is what every outfit written before this meant, and
            // is what a v21 record decodes to.
            std::uint32_t headDyeCount = 0;
            o.ForEachHeadPartDye(
                [&](std::uint32_t, const StyleRefKey&, const SlotDye&) { ++headDyeCount; });
            PutU32(out, headDyeCount);
            o.ForEachHeadPartDye([&](std::uint32_t a_slot, const StyleRefKey& a_part,
                                     const SlotDye& a_dye) {
                PutU32(out, a_slot);
                PutStr(out, a_part.modName);
                PutU32(out, a_part.localFormID);
                PutU32(out, static_cast<std::uint32_t>(kDyeChannelCount));
                for (const auto& ch : a_dye.channels) {
                    PutChannel(out, ch);
                }
            });

            // PUSH-UP (v23): how much lift this outfit asks the body for. One
            // byte, and deliberately the mode rather than the morphs it becomes:
            // the recipe is per body family and belongs to whichever body is
            // worn when the outfit is applied, not to the save.
            PutU8(out, static_cast<std::uint8_t>(o.pushUp));
        }
        return out;
    }

    // Returns false on a version mismatch or malformed data; a_lib is only
    // mutated on success.
    inline bool Decode(std::span<const std::byte> a_bytes, std::uint32_t a_version,
                       OutfitLibrary& a_lib) {
        using namespace detail;
        // ⚠ HISTORICAL VERSIONS AS LITERALS, THE CURRENT ONE AS kCodecVersion,
        // which is how DecodeNpcAssignments already spells the same list. Every
        // past version stays accepted forever because the 'LIBR' record is each
        // save's own outfit library, and a decoder that stopped taking an old
        // version would silently empty every existing player's library.
        //
        // The current version was a literal here until v10 and that is exactly
        // the trap it looks like: bumping kCodecVersion without also editing
        // this line made the NEW version undecodable, so every round trip in
        // the suite failed at once. Loud rather than silent, but there is no
        // reason to leave the tripwire armed.
        // ⚠ THE OUTGOING VERSION BECOMES A LITERAL HERE ON EVERY BUMP. This
        // list ends with kCodecVersion rather than a literal, so the version
        // that just lost current status falls out of it unless it is written
        // down. Missing that refuses the whole record rather than one entry of
        // it, which loses strictly more than the NpcoInnerLibrVersion trap
        // NpcAssignments.h documents.
        // ⚠ 13 IS WRITTEN DOWN HERE, AND IT IS THE VERSION THE SHIPPED 0.4.0
        // PLAYTEST BUILD WRITES. Leaving it to fall out of this list when
        // kCodecVersion moved to 14 would refuse the whole 'LIBR' record of every
        // save made on that build: the entire outfit library and every NPC
        // assignment, not one entry of either.
        // ⚠ AND 14 THE SAME DAY IT WAS CUT: 14 never left this machine, but the
        // user's own saves from the 2026-08-08 field sessions carry it, and
        // refusing those drops their whole library exactly as any other refusal
        // would.
        // ⚠ AND 15 THE SAME WAY: it is the version the 2026-08-09 texture-swap
        // builds write, and the dev machine's saves carry it.
        if (a_version != 1 && a_version != 2 && a_version != 3 && a_version != 4 &&
            a_version != 5 && a_version != 6 && a_version != 7 && a_version != 8 &&
            a_version != 9 && a_version != 10 && a_version != 11 &&
            a_version != 12 && a_version != 13 && a_version != 14 &&
            a_version != 15 && a_version != 16 && a_version != 17 &&
            a_version != 18 && a_version != 19 && a_version != 20 &&
            // ⚠ 21 WRITTEN DOWN THE DAY IT STOPPED BEING CURRENT, which is what
            // the note above says to do on every bump. It is the version the
            // 2026-08-16 and 2026-08-17 appearance-stint builds write, so the
            // user's own saves from those field sessions carry it, and refusing
            // one drops that save's whole outfit library rather than one entry.
            a_version != 21 &&
            // ⚠ 22 WRITTEN DOWN THE DAY IT STOPPED BEING CURRENT, which is what
            // the note above says to do on every bump. It is the version the
            // 2026-08-26 face and dye stint builds write, and the user's own
            // saves from those field sessions carry it.
            a_version != 22 &&
            // ⚠ 23 WRITTEN DOWN THE DAY IT STOPPED BEING CURRENT, which is what
            // the note above says to do on every bump. It is the version the
            // 2026-08-27 and 2026-08-28 builds write, INCLUDING the 1.1.1 zips
            // the playtesters are already carrying.
            a_version != 23 &&
            // ⚠ 24 WRITTEN DOWN THE DAY IT STOPPED BEING CURRENT, which is what
            // the note above says to do on every bump. It is the version 1.1.8
            // writes, LIVE ON NEXUS since 2026-09-02, so every player's save
            // carries it and refusing it would empty every library out there.
            a_version != 24 &&
            a_version != kCodecVersion) {
            return false;
        }
        Reader r{ a_bytes };

        const auto count  = r.U32();
        const auto active = r.U32();
        if (!r.ok || count > kMaxOutfitCount || active > count) {
            return false;
        }

        OutfitLibrary tmp;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto name = r.Str();
            const auto fav  = r.U8();
            if (!r.ok) {
                return false;
            }
            const int idx = tmp.Create(name);
            if (idx < 0) {
                return false;
            }
            auto* o    = tmp.At(static_cast<std::size_t>(idx));
            o->favorite = fav != 0;

            const auto slots = r.U32();
            if (!r.ok || slots > kBitCount) {
                return false;
            }
            for (std::uint32_t s = 0; s < slots; ++s) {
                const auto bit  = r.U32();
                const auto kind = r.U8();
                const auto mod  = r.Str();
                const auto fid  = r.U32();
                if (!r.ok || bit >= kBitCount || kind > static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    return false;
                }
                if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kStyle)) {
                    o->SetStyle(bit, StyleRefKey{ mod, fid });
                } else if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    o->SetHiddenWithRestore(bit, StyleRefKey{ mod, fid });
                }
            }

            if (a_version == 1) {
                continue;  // v1 bytes stop here - no weapon block; leave it all-passthrough
            }

            // Cap at what the wire can express (the class index is a u8, so a
            // same-version writer can never emit more than 256 entries), NOT at
            // this build's kWeaponClassCount - a future writer with appended
            // classes must still decode here, with its unknown entries skipped
            // per-entry below. Absurd counts from corruption die on the
            // r.ok / kMaxStringLen guards (and the record itself is capped at
            // kMaxRecordBytes before Decode ever sees it).
            const auto weaponCount = r.U32();
            if (!r.ok || weaponCount > 256) {
                return false;
            }
            for (std::uint32_t w = 0; w < weaponCount; ++w) {
                const auto classIdx = r.U8();
                const auto kind     = r.U8();
                const auto mod      = r.Str();
                const auto fid      = r.U32();
                if (!r.ok) {
                    return false;
                }
                if (classIdx >= kWeaponClassCount ||
                    kind > static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    continue;  // forward-tolerant: unknown class/kind, consumed and skipped
                }
                const auto wc = static_cast<WeaponClass>(classIdx);
                if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kStyle)) {
                    o->SetWeaponStyle(wc, StyleRefKey{ mod, fid });
                } else if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    o->SetWeaponHide(wc);
                }
            }

            if (a_version == 2) {
                continue;  // v2 bytes stop here - no body block; leave it at "no change"
            }

            // Body dimension (v3). Fixed shape, so no count to guard - but the
            // mode is still range-checked: an out-of-range byte from a future
            // writer (or corruption) falls back to kDefault rather than being
            // cast into a value the switch statements below do not handle.
            const auto preset = r.Str();
            const auto mode   = r.U8();
            if (!r.ok) {
                return false;
            }
            o->obodyPreset = preset;
            o->orefit      = (mode <= static_cast<std::uint8_t>(ORefitMode::kForceOff))
                                 ? static_cast<ORefitMode>(mode)
                                 : ORefitMode::kDefault;

            if (a_version == 3) {
                continue;
            }

            const auto handCount = r.U32();
            if (!r.ok || handCount > 256) {
                return false;
            }
            for (std::uint32_t h = 0; h < handCount; ++h) {
                const auto classIdx = r.U8();
                const auto handByte = r.U8();
                const auto kind     = r.U8();
                const auto mod      = r.Str();
                const auto fid      = r.U32();
                if (!r.ok) {
                    return false;
                }
                if (classIdx >= kWeaponClassCount ||
                    (handByte != static_cast<std::uint8_t>(WeaponHand::Right) &&
                     handByte != static_cast<std::uint8_t>(WeaponHand::Left)) ||
                    kind > static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    continue;
                }
                const auto wc   = static_cast<WeaponClass>(classIdx);
                const auto hand = static_cast<WeaponHand>(handByte);
                if (!SupportsHandOverrides(wc)) {
                    continue;
                }
                if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kStyle)) {
                    o->SetWeaponStyle(wc, StyleRefKey{ mod, fid }, hand);
                } else if (kind == static_cast<std::uint8_t>(SlotEntry::Kind::kHide)) {
                    o->SetWeaponHide(wc, hand);
                } else {
                    o->SetWeaponPassthrough(wc, hand);
                }
            }

            if (a_version == 4) {
                continue;  // v4 bytes stop here - leave hair at kAuto
            }

            const auto hairByte = r.U8();
            if (!r.ok) {
                return false;
            }
            o->hair = (hairByte <= static_cast<std::uint8_t>(HairMode::kHide))
                          ? static_cast<HairMode>(hairByte)
                          : HairMode::kAuto;

            if (a_version == 5) {
                continue;  // v5 bytes stop here - leave hairTint disabled
            }

            const auto tintSet = r.U8();
            const auto tintR   = r.U8();
            const auto tintG   = r.U8();
            const auto tintB   = r.U8();
            if (!r.ok) {
                return false;
            }
            o->hairTint = HairTint{ tintSet != 0, tintR, tintG, tintB };

            if (a_version == 6) {
                continue;  // v6 bytes stop here - leave hairStyle empty
            }

            auto styleMod = r.Str();
            auto styleID  = r.U32();
            if (!r.ok) {
                return false;
            }
            o->hairStyle = StyleRefKey{ std::move(styleMod), styleID };

            if (a_version == 7) {
                continue;  // v7 bytes stop here - leave every dye channel off
            }

            const auto dyeCount = r.U32();
            if (!r.ok) {
                return false;
            }
            // Bound the loop by the wire count only after checking it against
            // the key space, the same guard the armor block above uses: a
            // same-version writer can never emit more than it has keys for.
            // ⚠ THE CEILING MOVED WITH v24. Up to v23 one bit meant one entry,
            // so kBitCount was the whole key space; a v24 record holds one entry
            // per (bit, garment) pair and kMaxArmourDyes is what bounds it.
            if (dyeCount > (a_version >= 24 ? kMaxArmourDyes : kBitCount)) {
                return false;
            }
            for (std::uint32_t d = 0; d < dyeCount; ++d) {
                const auto bit = r.U32();
                if (!r.ok) {
                    return false;
                }
                // v24's garment half of the key. Read before the channel count
                // so the record reads in the order it was written.
                StyleRefKey garment;
                if (a_version >= 24) {
                    auto mod = r.Str();
                    auto id  = r.U32();
                    if (!r.ok) {
                        return false;
                    }
                    garment = StyleRefKey{ std::move(mod), id };
                }
                // How many channels this record actually carries. v8 wrote
                // exactly three with the number implied by its version; v9 and
                // later put the count on the wire so this constant can move
                // without the format moving with it.
                std::size_t wireChannels = 3;
                if (a_version >= 9) {
                    const auto n = r.U32();
                    if (!r.ok) {
                        return false;
                    }
                    // A same-version writer never emits more than it has
                    // channels for, and an absurd count here would otherwise
                    // spin this loop over the rest of the buffer.
                    if (n > kBitCount) {
                        return false;
                    }
                    wireChannels = n;
                }
                // An out-of-range bit is TOLERATED here, unlike the armor
                // block's fatal check above: the channel bytes are consumed
                // either way, and SetDye's own bounds guard makes the writes
                // land nowhere, so the stream stays aligned and the outfit's
                // other slots survive. Deliberate, and pinned by a forged
                // payload test - do not "harmonize" this loop with the armor
                // block's reject without moving that test.
                SlotDye staged{};
                for (std::size_t c = 0; c < wireChannels; ++c) {
                    DyeChannel ch;
                    if (!GetChannel(r, a_version, ch)) {
                        return false;
                    }
                    // Every channel on the wire is CONSUMED, but only the ones
                    // this build has room for are stored. That is what lets a
                    // narrower build read a wider save without the stream
                    // sliding out of alignment.
                    if (c < kDyeChannelCount) {
                        staged.channels[c] = ch;
                    }
                }
                // ⚠ THE TWO VERSIONS TAKE DIFFERENT DOORS, and it is the whole
                // migration. v24 carries its own key, so it is written straight
                // through. Anything older names only a bit, and SetDye keys it
                // to whatever garment the armor block above already put there,
                // which turns a slot colour into that piece's colour.
                //
                // An out-of-range bit is TOLERATED here, unlike the armor
                // block's fatal check above: the channel bytes are consumed
                // either way, and the writes below land nowhere on their own
                // bounds guards, so the stream stays aligned and the outfit's
                // other slots survive. Deliberate, and pinned by a forged
                // payload test - do not "harmonize" this loop with the armor
                // block's reject without moving that test.
                if (a_version >= 24) {
                    o->SetArmourDye(bit, garment, staged);
                } else {
                    for (std::size_t c = 0; c < kDyeChannelCount; ++c) {
                        if (staged.channels[c].set) {
                            o->SetDye(bit, static_cast<DyeChannelId>(c),
                                      staged.channels[c]);
                        }
                    }
                }
            }

            if (a_version < 10) {
                continue;  // v9 bytes stop here - no weapon dye at all
            }

            const auto weaponDyeCount = r.U32();
            if (!r.ok) {
                return false;
            }
            // Bounded by the key space, the same guard the two blocks above
            // use: a same-version writer can never emit more than one entry per
            // class-and-hand pair.
            if (weaponDyeCount > kWeaponClassCount * kWeaponHandCount) {
                return false;
            }
            for (std::uint32_t d = 0; d < weaponDyeCount; ++d) {
                const auto clsByte  = r.U8();
                const auto handByte = r.U8();
                const auto n        = r.U32();
                if (!r.ok) {
                    return false;
                }
                // An absurd count here would otherwise spin this loop over the
                // rest of the buffer. Same ceiling as the armour block.
                if (n > kBitCount) {
                    return false;
                }
                // An out-of-range class or hand is TOLERATED, exactly like the
                // armour block's out-of-range bit: the channel bytes are
                // consumed either way so the stream stays aligned, and the
                // outfit's other weapon dyes survive a record written by a
                // build that knows a class this one does not.
                const bool known = clsByte < kWeaponClassCount &&
                                   handByte < kWeaponHandCount;
                const auto cls  = static_cast<WeaponClass>(clsByte);
                const auto hand = static_cast<WeaponHand>(handByte);
                // ⚠ MATERIALISE THE OVERRIDE BEFORE READING ITS CHANNELS. An
                // entry whose channels are all off is a real stored value on a
                // hand: it renders undyed rather than inheriting Both. Relying
                // on SetWeaponDye to create it would lose exactly that entry,
                // because a wire count of zero calls SetWeaponDye zero times.
                if (known && hand != WeaponHand::Both) {
                    o->ClearWeaponDye(cls, hand);
                }
                for (std::uint32_t c = 0; c < n; ++c) {
                    DyeChannel ch;
                    if (!GetChannel(r, a_version, ch)) {
                        return false;
                    }
                    if (known && c < kDyeChannelCount) {
                        o->SetWeaponDye(cls, static_cast<DyeChannelId>(c), ch, hand);
                    }
                }
            }

            if (a_version >= 11) {
                o->customBodyPresetId = r.Str();
                if (!r.ok) {
                    return false;
                }
                if (!o->customBodyPresetId.empty()) {
                    o->obodyPreset.clear();
                }
            }

            if (a_version < 12) {
                continue;  // v11 bytes stop here - no eyes, no brows
            }

            auto eyesMod   = r.Str();
            auto eyesID    = r.U32();
            auto browsMod  = r.Str();
            auto browsID   = r.U32();
            if (!r.ok) {
                return false;
            }
            o->eyes  = StyleRefKey{ std::move(eyesMod), eyesID };
            o->brows = StyleRefKey{ std::move(browsMod), browsID };

            if (a_version < 16) {
                continue;  // v15 bytes stop here - no facial hair
            }

            auto facialMod = r.Str();
            auto facialID  = r.U32();
            if (!r.ok) {
                return false;
            }
            o->facialHair = StyleRefKey{ std::move(facialMod), facialID };

            if (a_version < 17) {
                continue;  // v16 bytes stop here - leave eyeTint disabled
            }

            const auto eyeSet = r.U8();
            const auto eyeR   = r.U8();
            const auto eyeG   = r.U8();
            const auto eyeB   = r.U8();
            if (!r.ok) {
                return false;
            }
            o->eyeTint = HairTint{ eyeSet != 0, eyeR, eyeG, eyeB };

            if (a_version < 18) {
                continue;  // v17 bytes stop here - leave the sclera alone
            }

            const auto sclSet = r.U8();
            const auto sclR   = r.U8();
            const auto sclG   = r.U8();
            const auto sclB   = r.U8();
            if (!r.ok) {
                return false;
            }
            o->scleraTint = HairTint{ sclSet != 0, sclR, sclG, sclB };

            if (a_version < 19) {
                continue;  // v18 bytes stop here - the eye keeps its own default
            }

            const auto eyeBlend = r.U8();
            if (!r.ok) {
                return false;
            }
            o->eyeBlend = eyeBlend;

            if (a_version < 20) {
                continue;  // v19 bytes stop here - the eye takes one colour
            }

            const auto e2Set = r.U8();
            const auto e2R   = r.U8();
            const auto e2G   = r.U8();
            const auto e2B   = r.U8();
            if (!r.ok) {
                return false;
            }
            o->eyeTint2 = HairTint{ e2Set != 0, e2R, e2G, e2B };

            if (a_version < 21) {
                continue;  // v20 bytes stop here - no invented head-part slots
            }

            const auto slotCount = r.U32();
            if (!r.ok) {
                return false;
            }
            // ⚠ BOUNDED BEFORE IT IS TRUSTED, the way kMaxNpcAssignments bounds
            // its own count. A corrupt or truncated record can present a
            // enormous count, and reserving on it before reading would commit
            // the allocation the record claims rather than the one it carries.
            if (slotCount > kMaxCustomHeadParts) {
                return false;
            }
            o->customHeadParts.clear();
            o->customHeadParts.reserve(slotCount);
            for (std::uint32_t s = 0; s < slotCount; ++s) {
                const auto slot = r.U32();
                auto       mod  = r.Str();
                const auto id   = r.U32();
                if (!r.ok) {
                    return false;
                }
                o->customHeadParts.push_back(
                    CustomHeadPartRef{ slot, StyleRefKey{ std::move(mod), id } });
            }

            if (a_version < 22) {
                continue;  // v21 bytes stop here - no head part carries a colour
            }

            const auto headDyeCount = r.U32();
            if (!r.ok) {
                return false;
            }
            // ⚠ BOUNDED BEFORE IT IS TRUSTED, like every counted block here, and
            // the ceiling is deliberately not kMaxCustomHeadParts. That one
            // bounds "one entry per slot"; this key is a slot AND a part, so one
            // slot legitimately carries an entry per shape. A hair with nine
            // parts on slot 3 is real, so the ceiling is per-slot entries times
            // the slot ceiling rather than the slot ceiling itself.
            if (headDyeCount > kMaxCustomHeadParts * kMaxDyedShapesPerHeadSlot) {
                return false;
            }
            for (std::uint32_t d = 0; d < headDyeCount; ++d) {
                const auto slotNum = r.U32();
                auto       partMod = r.Str();
                const auto partId  = r.U32();
                const auto n       = r.U32();
                if (!r.ok) {
                    return false;
                }
                // An absurd channel count would spin this loop over the rest of
                // the buffer. Same ceiling as the armour and weapon blocks.
                if (n > kBitCount) {
                    return false;
                }
                const StyleRefKey part{ std::move(partMod), partId };
                for (std::uint32_t c = 0; c < n; ++c) {
                    DyeChannel ch;
                    if (!GetChannel(r, a_version, ch)) {
                        return false;
                    }
                    // An out-of-range channel is TOLERATED and its bytes are
                    // consumed, exactly as the two blocks above tolerate an
                    // unknown bit or class: the stream stays aligned and the
                    // outfit's other colours survive a record written by a build
                    // with more channels than this one.
                    //
                    // ⚠ AN EMPTY PART REF IS DROPPED RATHER THAN STORED, which
                    // SetHeadPartDye enforces on its own. It is the value a
                    // "this outfit says nothing" lookup returns, so an entry
                    // keyed on one would be a colour every unresolved lookup
                    // collided on.
                    if (c < kDyeChannelCount) {
                        o->SetHeadPartDye(slotNum, part, static_cast<DyeChannelId>(c),
                                          ch);
                    }
                }
            }

            if (a_version < 23) {
                continue;  // v22 bytes stop here - no outfit asks for lift
            }

            const auto push = r.U8();
            if (!r.ok) {
                return false;
            }
            // Range checked exactly as orefit's mode is: a byte from a future
            // writer with more levels falls back to kNone rather than becoming a
            // mode PushUpStrength has no arm for.
            o->pushUp = (push <= static_cast<std::uint8_t>(PushUpMode::kFull))
                            ? static_cast<PushUpMode>(push)
                            : PushUpMode::kNone;
        }
        if (r.pos != a_bytes.size()) {
            return false;  // trailing garbage
        }

        a_lib = std::move(tmp);
        if (active > 0) {
            a_lib.Activate(active - 1);
        } else {
            a_lib.Deactivate();
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // 'HCOL': the player's captured pre-Fitting-Room hair colour (see
    // OS::HairColor::CapturedColor and the write/read blocks in
    // Persistence.cpp). Its own record with its own version, NOT part of the
    // LIBR codec above - it is character state, not outfit state, and must
    // not ride kCodecVersion.
    //
    // Pulled out as pure functions rather than inlined at the two call sites
    // in Persistence.cpp, because that file talks to the live SKSE
    // serialization interface and cannot compile into a pure test target. The
    // read side parses untrusted save bytes, and this codebase's convention
    // is that wire-format logic spanning more than one field gets a pure
    // round-trip test (Decode and DecodeNpcAssignments above and beside this
    // file both do). The shape mirrors CapturedColor - a plugin name plus a
    // local form ID - as loose fields rather than that type itself, so this
    // header (and PersistenceTests, which compiles it standalone with no
    // engine dependency) does not have to start including HairColor.h.
    [[nodiscard]] inline std::vector<std::byte> EncodeHairBaseline(
        const std::string& a_modName, std::uint32_t a_localFormID) {
        using namespace detail;
        std::vector<std::byte> out;
        PutStr(out, a_modName);
        PutU32(out, a_localFormID);
        return out;
    }

    // Returns false on truncated bytes, trailing garbage, or an empty mod
    // name; a_modName/a_localFormID must not be trusted when it does. An
    // empty mod name can never resolve back to a form (see
    // HairColor::Resolve's own modName.empty() guard), so a record claiming a
    // capture with no plugin name is malformed rather than a genuine captured
    // value - the caller's "captured" flag must come from THIS return, never
    // from "decode consumed the bytes without error" alone.
    [[nodiscard]] inline bool DecodeHairBaseline(std::span<const std::byte> a_bytes,
                                                 std::string&               a_modName,
                                                 std::uint32_t&             a_localFormID) {
        using namespace detail;
        Reader r{ a_bytes };
        a_modName     = r.Str();
        a_localFormID = r.U32();
        return r.ok && r.pos == a_bytes.size() && !a_modName.empty();
    }

    // -----------------------------------------------------------------------
    // 'HPBS': the player's captured pre-Fitting-Room head PARTS, one row per
    // slot. The style half of what 'HCOL' does for hair colour, and it exists
    // for a measured fault rather than for symmetry.
    //
    // ⚠⚠ THE CAPTURE USED TO BE SESSION STATE AND THE KEY IS THE PLAYER'S FORM
    // ID, which is 0x14 in every save. Nothing cleared it between loads, so
    // character B was restored to character A's hair by the load path itself
    // (HeadPart.h's Clear note carries the full chain). Clearing it on revert
    // is half the fix; without a record like this one, the other half of the
    // report, "Base gear leaves the hair on", would get worse rather than
    // better, because the actor base change IS in the save and the only note of
    // what came before it would now be dropped at every load.
    //
    // The slot number travels with each row rather than being implied by
    // position, for the reason CustomHeadPartRef gives: a load order that drops
    // the horn mod must leave the ear row still meaning ears.
    struct HeadPartBaselineRow {
        std::uint32_t slot{ 0 };
        std::string   modName;
        std::uint32_t localFormID{ 0 };

        friend bool operator==(const HeadPartBaselineRow&,
                               const HeadPartBaselineRow&) = default;
    };

    // A corrupt record can claim any count, so the decoder bounds it before
    // trusting it. Same ceiling as kMaxCustomHeadParts and for the same reason:
    // one row per head-part slot this load order has, and a hundred and
    // twenty-eight of those would already be a load order nobody has.
    inline constexpr std::uint32_t kMaxHeadPartBaselineRows = 128;

    [[nodiscard]] inline std::vector<std::byte> EncodeHeadPartBaseline(
        const std::vector<HeadPartBaselineRow>& a_rows) {
        using namespace detail;
        std::vector<std::byte> out;
        PutU32(out, static_cast<std::uint32_t>(a_rows.size()));
        for (const auto& row : a_rows) {
            PutU32(out, row.slot);
            PutStr(out, row.modName);
            PutU32(out, row.localFormID);
        }
        return out;
    }

    // Returns false on a truncated record, trailing garbage, or a count past
    // the cap, and a_out must not be trusted when it does.
    //
    // ⚠ A ROW WITH AN EMPTY PLUGIN NAME AND A NON-ZERO ID FAILS THE WHOLE
    // RECORD rather than being skipped, exactly as DecodeHairBaseline refuses
    // one: that name can never resolve back to a form, so a record containing
    // it is malformed rather than partially useful, and half-adopting a
    // baseline is how a character ends up with someone else's face in one slot.
    //
    // ⚠ AN EMPTY NAME WITH A ZERO ID IS A ROW, NOT A FAULT. It is the "nothing"
    // capture: the player had no part in that slot before Fitting Room touched
    // it, which HeadPart::SnapshotCaptures writes that way because RestoreSlot
    // can act on it (it wears the slot's placeholder). Same record version: a
    // record from before this row existed carries none of them and reads as
    // it always did, and a build from before it fails the whole record on the
    // empty name, which is the same answer it gave any row it could not read.
    [[nodiscard]] inline bool DecodeHeadPartBaseline(std::span<const std::byte>        a_bytes,
                                                     std::vector<HeadPartBaselineRow>& a_out) {
        using namespace detail;
        Reader r{ a_bytes };
        const auto count = r.U32();
        if (!r.ok || count > kMaxHeadPartBaselineRows) {
            return false;
        }
        std::vector<HeadPartBaselineRow> tmp;
        tmp.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            HeadPartBaselineRow row;
            row.slot        = r.U32();
            row.modName     = r.Str();
            row.localFormID = r.U32();
            if (!r.ok || (row.modName.empty() && row.localFormID != 0)) {
                return false;
            }
            tmp.push_back(std::move(row));
        }
        if (!r.ok || r.pos != a_bytes.size()) {
            return false;
        }
        a_out = std::move(tmp);
        return true;
    }

    // -----------------------------------------------------------------------
    // 'SKIN': which skin pack each actor wears and every armour override this
    // mod wrote to put it there. Character state, keyed by the base NPC's
    // {plugin, localFormID} exactly as 'NPCO' keys its rows, so the row for a
    // follower resolves whatever the load order does. The player is a row like
    // any other.
    //
    // ⚠ THE PACK TRAVELS AS THE FOLDER NAME AND NOTHING ELSE. The files it
    // resolves to are re-read from disk at every attach, so a pack renamed or
    // removed simply stops matching, and a save from a rig that had it reads
    // as "this actor wears a pack this rig does not have", which the page says
    // rather than guesses about.
    struct SkinRow {
        std::string          npcMod;
        std::uint32_t        npcLocal{ 0 };
        SkinPlan::ActorSkin  skin;

        friend bool operator==(const SkinRow&, const SkinRow&) = default;
    };

    // Ceilings a corrupt record cannot talk its way past. Actors: a follower
    // framework's worth with room to spare. Written rows per actor: a body is
    // three shapes and two keys each on this rig, and an actor who has worn a
    // hundred different skin-bearing pieces since the pack went on is still
    // under a thousand.
    inline constexpr std::uint32_t kMaxSkinRows           = 256;
    inline constexpr std::uint32_t kMaxSkinWrittenPerActor = 1024;
    // Head rows per actor: a face, a mouth and a handful of overlay clones,
    // a few slots each. A hundred is already generous.
    inline constexpr std::uint32_t kMaxSkinHeadPerActor = 256;
    // Bare rows per actor: a body, two hands and two feet on this rig, a few
    // slots each, and nothing worn can add to it. The head's ceiling is already
    // generous for a shorter list.
    inline constexpr std::uint32_t kMaxSkinBarePerActor = 256;

    // The record's layout versions. v1 carried the pack and the armour rows;
    // v2 (2026-08-18) appends the head rows after them, per actor; v3
    // (2026-08-27) appends the bare-skin rows after those, which is the naked
    // body a pack could not reach until it had a list of its own. The encoder
    // always writes the newest; the decoder is told which it is reading, and a
    // v1 record simply has no head list to read, a v2 no bare list.
    inline constexpr std::uint32_t kSkinLayoutV1 = 1;
    inline constexpr std::uint32_t kSkinLayoutV2 = 2;
    inline constexpr std::uint32_t kSkinLayoutV3 = 3;
    inline constexpr std::uint32_t kSkinLayout   = kSkinLayoutV3;

    [[nodiscard]] inline std::vector<std::byte> EncodeSkinRows(const std::vector<SkinRow>& a_rows) {
        using namespace detail;
        std::vector<std::byte> out;
        PutU32(out, static_cast<std::uint32_t>(a_rows.size()));
        for (const auto& row : a_rows) {
            PutStr(out, row.npcMod);
            PutU32(out, row.npcLocal);
            PutStr(out, row.skin.pack);
            PutU32(out, static_cast<std::uint32_t>(row.skin.written.size()));
            for (const auto& w : row.skin.written) {
                PutStr(out, w.armorMod);
                PutU32(out, w.armorLocal);
                PutStr(out, w.addonMod);
                PutU32(out, w.addonLocal);
                PutStr(out, w.node);
                PutU8(out, w.slot);
                PutStr(out, w.original);
            }
            // v2: the head rows.
            PutU32(out, static_cast<std::uint32_t>(row.skin.head.size()));
            for (const auto& h : row.skin.head) {
                PutStr(out, h.node);
                PutU8(out, h.slot);
                PutStr(out, h.original);
            }
            // v3: the bare-skin rows, same three fields and the same node
            // channel, on their own list so a body is never counted as a face.
            PutU32(out, static_cast<std::uint32_t>(row.skin.bare.size()));
            for (const auto& b : row.skin.bare) {
                PutStr(out, b.node);
                PutU8(out, b.slot);
                PutStr(out, b.original);
            }
        }
        return out;
    }

    // Returns false on a truncated record, trailing garbage, a count past a
    // cap, or a row with an empty plugin name; a_out is only mutated on
    // success. A row with no plugin name can never find its actor again and a
    // written entry with no plugin name can never find its armour, so either
    // is a malformed record rather than a partly useful one: half-adopting it
    // would leave overrides this mod wrote with no way to take them off.
    //
    // a_layout is the record's version as the co-save stamped it: v1 has no
    // head list and reading one there would eat the next actor's row.
    [[nodiscard]] inline bool DecodeSkinRows(std::span<const std::byte> a_bytes,
                                             std::vector<SkinRow>&      a_out,
                                             std::uint32_t              a_layout = kSkinLayout) {
        using namespace detail;
        if (a_layout < kSkinLayoutV1 || a_layout > kSkinLayout) {
            return false;
        }
        Reader     r{ a_bytes };
        const auto count = r.U32();
        if (!r.ok || count > kMaxSkinRows) {
            return false;
        }
        std::vector<SkinRow> tmp;
        tmp.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            SkinRow row;
            row.npcMod    = r.Str();
            row.npcLocal  = r.U32();
            row.skin.pack = r.Str();
            const auto written = r.U32();
            if (!r.ok || row.npcMod.empty() || written > kMaxSkinWrittenPerActor) {
                return false;
            }
            row.skin.written.reserve(written);
            for (std::uint32_t j = 0; j < written; ++j) {
                SkinPlan::Written w;
                w.armorMod   = r.Str();
                w.armorLocal = r.U32();
                w.addonMod   = r.Str();
                w.addonLocal = r.U32();
                w.node       = r.Str();
                w.slot       = r.U8();
                w.original   = r.Str();
                // ⚠ AN EMPTY ORIGINAL IS A ROW, NOT A FAULT: a slot that showed
                // no path at all before it was written is a real thing to put
                // back. Only a slot number the texture set cannot hold is
                // malformed.
                if (!r.ok || w.armorMod.empty() || w.addonMod.empty() || w.slot >= 8) {
                    return false;
                }
                row.skin.written.push_back(std::move(w));
            }
            if (a_layout >= kSkinLayoutV2) {
                const auto heads = r.U32();
                if (!r.ok || heads > kMaxSkinHeadPerActor) {
                    return false;
                }
                row.skin.head.reserve(heads);
                for (std::uint32_t j = 0; j < heads; ++j) {
                    SkinPlan::HeadWritten h;
                    h.node     = r.Str();
                    h.slot     = r.U8();
                    h.original = r.Str();
                    // A head row with no node name could never be found or
                    // taken off again; that is malformed. An empty original
                    // is a row, as above.
                    if (!r.ok || h.node.empty() || h.slot >= 8) {
                        return false;
                    }
                    row.skin.head.push_back(std::move(h));
                }
            }
            if (a_layout >= kSkinLayoutV3) {
                const auto bares = r.U32();
                if (!r.ok || bares > kMaxSkinBarePerActor) {
                    return false;
                }
                row.skin.bare.reserve(bares);
                for (std::uint32_t j = 0; j < bares; ++j) {
                    SkinPlan::HeadWritten b;
                    b.node     = r.Str();
                    b.slot     = r.U8();
                    b.original = r.Str();
                    // Same two rules the head rows hold: a row with no node
                    // name could never be taken off again, and an empty
                    // original is a slot that showed no path, which is a real
                    // thing to put back.
                    if (!r.ok || b.node.empty() || b.slot >= 8) {
                        return false;
                    }
                    row.skin.bare.push_back(std::move(b));
                }
            }
            tmp.push_back(std::move(row));
        }
        if (!r.ok || r.pos != a_bytes.size()) {
            return false;
        }
        a_out = std::move(tmp);
        return true;
    }

    // -----------------------------------------------------------------------
    // 'SEXF': the sex a look states for this character.
    //
    // ⚠⚠ THE ENGINE WILL NOT KEEP IT. ProfileApply writes the flag onto the
    // player's base, and form 00000007 is a static form out of Skyrim.esm whose
    // default is male; nothing marks the base as changed, so the save never
    // carries it and the ESM value wins at the next load. See CharacterSex.h.
    //
    // One byte, and it still gets a length-checked decoder for the reason every
    // other record here does: a record that decodes "successfully" while
    // ignoring bytes it did not understand is how a later version silently
    // loses half its content.
    [[nodiscard]] inline std::vector<std::byte> EncodeCharacterSex(bool a_female) {
        using namespace detail;
        std::vector<std::byte> out;
        PutU8(out, a_female ? 1u : 0u);
        return out;
    }

    // Returns false on truncated bytes or trailing garbage; a_female is only
    // written on success.
    [[nodiscard]] inline bool DecodeCharacterSex(std::span<const std::byte> a_bytes,
                                                 bool&                      a_female) {
        using namespace detail;
        Reader     r{ a_bytes };
        const auto v = r.U8();
        if (!r.ok || r.pos != a_bytes.size()) {
            return false;
        }
        a_female = v != 0;
        return true;
    }

    // -----------------------------------------------------------------------
    // 'FTNT': the CharGen jslot whose EXPORTED face tint this character wears.
    //
    // ⚠⚠ A LOOK WHOSE FACE IS A BAKED EXPORT HAS NO OTHER SURVIVAL STORY.
    // ProfileApply's 'makeup' step stands aside when a look carries a face
    // block, because a preset face arrives with its tints already baked into
    // Textures\CharGen\Exported\FR_<jslot>.dds, so the player's 34-slot tint
    // list never receives the layers that could rebuild it. Field 2026-08-25
    // 16:5x, the same head either side of a quit to desktop:
    //
    //     apply   tintTex = 'Textures\CharGen\Exported\FR_Umbrael 17.dds'
    //     reload  tintTex = 'Player face tint'
    //
    // ⛔ Not an overlay: 'Face [Ovl0]' reads the same third-party file on both
    // sides and OverlayBaseline correctly stands down. See LookFaceTint.h.
    //
    // The jslot NAME and not the path. The folder is skee's and the extension
    // is the exporter's, so a record storing the whole path would need
    // rewriting the day either moves, and the late-rebake gate already
    // reconstructs both ends from the same pieces.
    [[nodiscard]] inline std::vector<std::byte> EncodeLookFaceTint(const std::string& a_jslot) {
        using namespace detail;
        std::vector<std::byte> out;
        PutStr(out, a_jslot);
        return out;
    }

    // Returns false on truncated bytes, trailing garbage, or an empty name;
    // a_jslot must not be trusted when it does. An empty jslot can never name a
    // file, so a record claiming one is malformed rather than a genuine "this
    // character has no baked face": absence is carried by the record NOT BEING
    // WRITTEN, the same convention 'SKTN' and 'SEXF' use.
    [[nodiscard]] inline bool DecodeLookFaceTint(std::span<const std::byte> a_bytes,
                                                 std::string&               a_jslot) {
        using namespace detail;
        Reader r{ a_bytes };
        a_jslot = r.Str();
        return r.ok && r.pos == a_bytes.size() && !a_jslot.empty();
    }

    // -----------------------------------------------------------------------
    // 'SKTN': the skin tone Fitting Room is HOLDING for the player, so that the
    // reassert has something to put back after a load's head build.
    //
    // ⚠⚠ THE HOLD WAS SESSION STATE AND THAT IS WHY A LOAD CAME BACK WHITE.
    // MakeupApi's reassert was already wired to the head build and already ran;
    // it stood down silently because nothing had held a tone since the revert.
    // The strength travels as a byte over 255 rather than a float: it is
    // clamped to 0..1 at every use and the engine's own alpha is a byte, so a
    // float would be four bytes of precision nobody reads.
    [[nodiscard]] inline std::vector<std::byte> EncodeSkinToneHold(
        std::uint8_t a_r, std::uint8_t a_g, std::uint8_t a_b, float a_strength) {
        using namespace detail;
        const float clamped = a_strength < 0.0f   ? 0.0f
                              : a_strength > 1.0f ? 1.0f
                                                  : a_strength;
        std::vector<std::byte> out;
        PutU8(out, a_r);
        PutU8(out, a_g);
        PutU8(out, a_b);
        PutU8(out, static_cast<std::uint32_t>(clamped * 255.0f + 0.5f));
        return out;
    }

    // Returns false on truncated bytes or trailing garbage; the outputs are
    // only written on success, the same discipline as every decoder here.
    [[nodiscard]] inline bool DecodeSkinToneHold(std::span<const std::byte> a_bytes,
                                                 std::uint8_t& a_r, std::uint8_t& a_g,
                                                 std::uint8_t& a_b, float& a_strength) {
        using namespace detail;
        Reader     r{ a_bytes };
        const auto rr = r.U8();
        const auto gg = r.U8();
        const auto bb = r.U8();
        const auto ss = r.U8();
        if (!r.ok || r.pos != a_bytes.size()) {
            return false;
        }
        a_r        = static_cast<std::uint8_t>(rr);
        a_g        = static_cast<std::uint8_t>(gg);
        a_b        = static_cast<std::uint8_t>(bb);
        a_strength = static_cast<float>(ss) / 255.0f;
        return true;
    }

    // -----------------------------------------------------------------------
    // 'OVLB': the player's overlay ART, as the same JSON a look already stores
    // it in. Written and parsed in Persistence.cpp, which is linked against
    // jsoncpp and ProfileCodec; this header only packs and unpacks the string,
    // exactly as it does for 'RULE' below and for the same reason.
    //
    // ⚠ THE JSON IS THE POINT, NOT A SHORTCUT. LayerState has a dozen fields
    // and grows (the OS-226 finish and the OS-209 transform both arrived after
    // it shipped). Hand-rolling a second copy of that list here would go stale
    // the first time a thirteenth is added, and the failure would be silent: a
    // record that decodes cleanly and drops whatever it had not learned. The
    // look codec is the one that must already know every field.
    //
    // ⚠ PutBlob, NOT PutStr, and that is the same call RuleRecordFields makes.
    // PutStr caps at kMaxStringLen (512 bytes), which a block of populated
    // overlay layers passes without trying.
    [[nodiscard]] inline std::vector<std::byte> EncodeOverlayBaseline(
        const std::string& a_overlaysJson) {
        using namespace detail;
        std::vector<std::byte> out;
        PutBlob(out, a_overlaysJson);
        return out;
    }

    // Returns false on truncated bytes or trailing garbage. a_out is only
    // mutated on success, the same discipline every decoder in this file
    // follows: half a record is worse than none, because none is a state the
    // load path already handles ("this character has had no overlay written by
    // us") and half is a character wearing part of somebody's art.
    //
    // ⚠ AN EMPTY STRING DECODES SUCCESSFULLY and means "recorded, and it was
    // empty". That is NOT the same as an absent record, and the caller must
    // keep them apart: absent is a save from before this build, empty is a
    // character we have written no overlay for. Both end with nothing to
    // reassert, so the distinction costs nothing today and is exactly the sort
    // of thing the default-looks record's own note says gets lost.
    [[nodiscard]] inline bool DecodeOverlayBaseline(std::span<const std::byte> a_bytes,
                                                    std::string&               a_out) {
        using namespace detail;
        Reader      r{ a_bytes };
        std::string tmp = r.Blob(a_bytes.size());
        if (!r.ok || r.pos != a_bytes.size()) {
            return false;
        }
        a_out = std::move(tmp);
        return true;
    }

    // -----------------------------------------------------------------------
    // 'RULE': per-save rule-engine state (Task 10). The rule SET itself
    // travels as an opaque JSON string - the same document shape
    // RuleCodec::RulesToJson/JsonToRules and rules.json already use -
    // produced and parsed in Persistence.cpp, which is already linked
    // against jsoncpp and RuleCodec. Keeping jsoncpp out of THIS header
    // mirrors why EncodeHairBaseline/DecodeHairBaseline above stay
    // JSON-free too: it is what lets PersistenceTests compile this file
    // standalone with no engine or jsoncpp dependency. EncodeRuleState /
    // DecodeRuleState below only pack and unpack that string alongside the
    // four small scalars that ride next to it, using the same primitives
    // as every other record in this file.
    struct RuleRecordFields {
        std::string rulesJson;  // RuleCodec::RulesToJson(...), Json::writeString'd
        bool        engineEnabled{ true };
        bool        pinned{ false };
        std::string pinnedName;
        std::string lastAppliedRuleId;
        std::vector<std::string> disabledPackIds;

        friend bool operator==(const RuleRecordFields&, const RuleRecordFields&) = default;
    };

    // rulesJson travels through detail::PutBlob/Reader::Blob above, NOT
    // PutStr/Str: those cap at kMaxStringLen (512 bytes), sized for short
    // fields like mod/outfit names, and a save with more than a handful of
    // rules easily produces a JSON document past that. The only ceiling
    // here is the outer co-save record's own cap (Persistence.cpp's
    // kMaxRecordBytes, already enforced on a_bytes before Decode ever sees
    // it), so Blob only needs to guard against reading past the buffer it
    // was actually given.
    [[nodiscard]] inline std::vector<std::byte> EncodeRuleState(const RuleRecordFields& a_f) {
        using namespace detail;
        std::vector<std::byte> out;
        PutBlob(out, a_f.rulesJson);
        PutU8(out, a_f.engineEnabled ? 1u : 0u);
        PutU8(out, a_f.pinned ? 1u : 0u);
        PutStr(out, a_f.pinnedName);
        PutStr(out, a_f.lastAppliedRuleId);
        PutU32(out, static_cast<std::uint32_t>(a_f.disabledPackIds.size()));
        for (const auto& id : a_f.disabledPackIds) {
            PutStr(out, id);
        }
        return out;
    }

    // Returns false on truncated bytes or trailing garbage; a_out is only
    // mutated on success (same discipline as Decode/DecodeHairBaseline
    // above) - a load that decodes this record must never end up with a
    // half-written EngineState, half default and half stale.
    [[nodiscard]] inline bool DecodeRuleState(std::span<const std::byte> a_bytes,
                                              RuleRecordFields&           a_out) {
        using namespace detail;
        Reader           r{ a_bytes };
        RuleRecordFields tmp;

        tmp.rulesJson = r.Blob(a_bytes.size());
        if (!r.ok) {
            return false;
        }

        const auto engineByte     = r.U8();
        const auto pinnedByte     = r.U8();
        tmp.pinnedName            = r.Str();
        tmp.lastAppliedRuleId     = r.Str();
        const auto idCount        = r.U32();
        // Sanity cap, same reasoning as kMaxOutfitCount above: a real save
        // has at most a few dozen disabled pack-rule ids. This is the ONLY
        // hard allocation limit standing between a corrupted idCount and
        // reserve() attempting to allocate space for billions of strings -
        // see the idCount > 4096 test in test_persistence.cpp.
        if (!r.ok || idCount > 4096) {
            return false;
        }
        tmp.disabledPackIds.reserve(idCount);
        for (std::uint32_t i = 0; i < idCount; ++i) {
            tmp.disabledPackIds.push_back(r.Str());
        }
        if (!r.ok || r.pos != a_bytes.size()) {
            return false;  // truncated or trailing garbage
        }
        tmp.engineEnabled = engineByte != 0;
        tmp.pinned        = pinnedByte != 0;
        a_out             = std::move(tmp);
        return true;
    }

    // -----------------------------------------------------------------------
    // 'DFLK' and 'DFBD': this save's character defaults.
    //
    // ⚠ THESE USED TO BE FILES ON DISK AND THAT WAS A CURRENCY EXPLOIT, which
    // is the whole reason they are here (user 2026-08-08: "you can load a save
    // and then spend soulgems to edit and then go back to that save"). Setting
    // a default costs a look, and the charge it costs comes out of the co-save;
    // the default it bought lived in default-looks.json, outside every save. So
    // paying, reloading an earlier save and keeping the default was free, and
    // the charge came back with the reload. **A thing the player PAYS FOR must
    // live in the same place as the currency they paid with.**
    //
    // The look and the body get SEPARATE records for the reason 'DYHI' is not
    // part of 'DYES': one malformed block must not take the other down with it,
    // and DefaultBody is FR_BODY_STUDIO-gated while DefaultLook ships
    // everywhere, so a single record would have to encode a field half the
    // builds cannot populate.
    //
    // ⚠ THE KEY TRAVELS AS THE SAME TEXT THE FILES USED, "Plugin.esp|0008F0".
    // It is already plugin plus LOCAL form id, which is the property that
    // matters (a runtime form id carries the load order's index and rebinds one
    // character onto another when a plugin moves), and keeping the exact text
    // is what lets the one-time import from the old file be a straight copy
    // rather than a parse.
    // ⚠ THIS RECORD IS VERSIONED SEPARATELY FROM 'LIBR' AND ITS READ PATH USED
    // TO TEST EQUALITY. 'DFLK' v2 appends a facial-hair pair (OS-196), and a
    // bare bump would have made every v1 record fail that test and be SKIPPED,
    // silently deleting every default the player had set. So the decode takes
    // the version and Persistence.cpp accepts both.
    // See [[cosave-version-equality-destroys-on-bump]].
    // ⚠ 'DFLK' v3 APPENDS A DEFAULT HAIR COLOUR, four bytes on the terms the
    // per-outfit tints already sit on: a presence bit and an RGB whose cleared
    // value means "leave the character's colour alone". v1 and v2 bytes stop
    // before it and decode to exactly that, which is what those saves already
    // did. This is the 'LIBR' v16 -> v17 shape one record over, and BOTH older
    // versions stay in the accepted list below and in Persistence.cpp: the
    // moment 3 became current, every save written since facial hair landed
    // became an older half, and refusing one costs every default in it.
    //
    // ⚠ THE COLOUR IS RGB AND NOT A COLOUR FORM, for the reason HairTint gives
    // in Outfit.h: a form id carries the load order's index and would rebind
    // one character's hair onto another the moment a plugin moves. HairColor
    // snaps the RGB to the nearest installed colour form at the point of use.
    // ⚠ 'DFLK' v4 APPENDS THE SLOTS THIS LOAD ORDER INVENTED, a counted list
    // per character of (slot, plugin, local form id). Same append-only terms as
    // v2 and v3: v1, v2 and v3 bytes stop before the count and decode to a
    // character with nothing pinned, which is exactly what those saves said.
    // All four stay in the accepted list below AND in Persistence.cpp, because
    // the moment 4 became current every save written since the hair colour
    // landed became an older half.
    //
    // ⚠ THE LIST IS COUNTED RATHER THAN FIXED-WIDTH because a discovered slot
    // is a raw type number a mod picked for itself, so there is no known set to
    // reserve fields for. See DefaultLook.h for why that is also what let this
    // exist at all.
    inline constexpr std::uint32_t kDefaultLookVersion = 4;

    // One pinned slot on the wire. The slot number travels WITH the reference
    // for the reason CustomHeadPartRef gives: a load order that drops the horn
    // mod must leave the ear entry still meaning ears.
    struct DefaultLookSlotRecord {
        std::uint32_t slot{ 0 };
        std::string   mod;
        std::uint32_t id{ 0 };

        friend bool operator==(const DefaultLookSlotRecord&,
                               const DefaultLookSlotRecord&) = default;
    };

    struct DefaultLookRecord {
        std::string modAndId;  // "Plugin.esp|0008F0"
        std::string hairMod;
        std::uint32_t hairId{ 0 };
        std::string eyesMod;
        std::uint32_t eyesId{ 0 };
        std::string browsMod;
        std::uint32_t browsId{ 0 };
        std::string facialMod;    // v2
        std::uint32_t facialId{ 0 };  // v2
        // v3, and a SEPARATE FLAG rather than a sentinel colour. Black is a
        // colour somebody sets on purpose, so "no default" cannot be spelled
        // as an RGB and has to be spelled as the absence of one.
        bool          hairTintSet{ false };
        std::uint8_t  hairTintR{ 0 };
        std::uint8_t  hairTintG{ 0 };
        std::uint8_t  hairTintB{ 0 };
        // v4. Empty on every older record, which is the same statement "this
        // character has nothing pinned" makes.
        std::vector<DefaultLookSlotRecord> customHeadParts;  // v4

        friend bool operator==(const DefaultLookRecord&, const DefaultLookRecord&) = default;
    };

    struct DefaultBodyRecord {
        std::string modAndId;
        std::string installed;
        std::string customId;

        friend bool operator==(const DefaultBodyRecord&, const DefaultBodyRecord&) = default;
    };

    // A corrupt count must not make us allocate without bound. The same 4096
    // the two files already capped themselves at, so a save cannot carry more
    // characters than the file could.
    inline constexpr std::uint32_t kMaxDefaultEntries = 4096;

    [[nodiscard]] inline std::vector<std::byte> EncodeDefaultLooks(
        const std::vector<DefaultLookRecord>& a_rows) {
        using namespace detail;
        std::vector<std::byte> out;
        PutU32(out, static_cast<std::uint32_t>(a_rows.size()));
        for (const auto& r : a_rows) {
            PutStr(out, r.modAndId);
            PutStr(out, r.hairMod);
            PutU32(out, r.hairId);
            PutStr(out, r.eyesMod);
            PutU32(out, r.eyesId);
            PutStr(out, r.browsMod);
            PutU32(out, r.browsId);
            PutStr(out, r.facialMod);
            PutU32(out, r.facialId);
            PutU8(out, r.hairTintSet ? 1u : 0u);
            PutU8(out, r.hairTintR);
            PutU8(out, r.hairTintG);
            PutU8(out, r.hairTintB);
            PutU32(out, static_cast<std::uint32_t>(r.customHeadParts.size()));
            for (const auto& part : r.customHeadParts) {
                PutU32(out, part.slot);
                PutStr(out, part.mod);
                PutU32(out, part.id);
            }
        }
        return out;
    }

    // False on a truncated record, trailing garbage or an absurd count, and
    // a_out must not be trusted when it does. The caller keeps an EMPTY map on
    // false rather than falling back to the old file: falling back would
    // resurrect the cross-save leak this record exists to end.
    // ⚠ a_version IS NOT OPTIONAL AND NOT DECORATION. v1 records carry no
    // facial-hair pair, and reading one from them runs off the end of the row
    // and fails the whole record, which is the same data loss a bare version
    // bump would have caused by a different route.
    [[nodiscard]] inline bool DecodeDefaultLooks(std::span<const std::byte> a_bytes,
                                                 std::uint32_t a_version,
                                                 std::vector<DefaultLookRecord>& a_out) {
        using namespace detail;
        // ⚠ EVERY SHIPPED VERSION IS LISTED AS A LITERAL, not as "<= current".
        // A range test accepts a version nobody has written a branch for the
        // day someone bumps the constant, and this decoder losing a record
        // costs the player every default they have set.
        if (a_version != 1 && a_version != 2 && a_version != 3 && a_version != 4) {
            return false;
        }
        Reader r{ a_bytes };
        const auto count = r.U32();
        if (!r.ok || count > kMaxDefaultEntries) {
            return false;
        }
        std::vector<DefaultLookRecord> tmp;
        tmp.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            DefaultLookRecord row;
            row.modAndId = r.Str();
            row.hairMod  = r.Str();
            row.hairId   = r.U32();
            row.eyesMod  = r.Str();
            row.eyesId   = r.U32();
            row.browsMod = r.Str();
            row.browsId  = r.U32();
            if (a_version >= 2) {
                row.facialMod = r.Str();
                row.facialId  = r.U32();
            }
            if (a_version >= 3) {
                row.hairTintSet = r.U8() != 0;
                row.hairTintR   = r.U8();
                row.hairTintG   = r.U8();
                row.hairTintB   = r.U8();
            }
            if (a_version >= 4) {
                const auto slots = r.U32();
                // ⚠ BOUNDED BEFORE IT IS TRUSTED, and reserve() comes after the
                // check rather than before it: a corrupt count is a large
                // allocation otherwise, which is the whole reason every counted
                // field in this file has a ceiling.
                if (!r.ok || slots > kMaxCustomHeadParts) {
                    return false;
                }
                row.customHeadParts.reserve(slots);
                for (std::uint32_t j = 0; j < slots; ++j) {
                    DefaultLookSlotRecord part;
                    part.slot = r.U32();
                    part.mod  = r.Str();
                    part.id   = r.U32();
                    row.customHeadParts.push_back(std::move(part));
                }
            }
            tmp.push_back(std::move(row));
        }
        if (!r.ok || r.pos != a_bytes.size()) {
            return false;
        }
        a_out = std::move(tmp);
        return true;
    }

    [[nodiscard]] inline std::vector<std::byte> EncodeDefaultBodies(
        const std::vector<DefaultBodyRecord>& a_rows) {
        using namespace detail;
        std::vector<std::byte> out;
        PutU32(out, static_cast<std::uint32_t>(a_rows.size()));
        for (const auto& r : a_rows) {
            PutStr(out, r.modAndId);
            PutStr(out, r.installed);
            PutStr(out, r.customId);
        }
        return out;
    }

    [[nodiscard]] inline bool DecodeDefaultBodies(std::span<const std::byte> a_bytes,
                                                  std::vector<DefaultBodyRecord>& a_out) {
        using namespace detail;
        Reader r{ a_bytes };
        const auto count = r.U32();
        if (!r.ok || count > kMaxDefaultEntries) {
            return false;
        }
        std::vector<DefaultBodyRecord> tmp;
        tmp.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            DefaultBodyRecord row;
            row.modAndId  = r.Str();
            row.installed = r.Str();
            row.customId  = r.Str();
            tmp.push_back(std::move(row));
        }
        if (!r.ok || r.pos != a_bytes.size()) {
            return false;
        }
        a_out = std::move(tmp);
        return true;
    }

}  // namespace OS
