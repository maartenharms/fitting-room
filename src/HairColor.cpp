#include "HairColor.h"

#include "ColorSnap.h"
#include "DefaultLook.h"  // the character's default colour, the ladder's middle rung

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace OS::HairColor {

    namespace {
        // Session-cached palette. g_channels is colour-major rgb and is written
        // three bytes per g_palette entry and nowhere else, so
        // g_channels.size() / 3 == g_palette.size() always holds. That is the
        // invariant NearestColorIndex relies on to derive its own colour count,
        // and it is why the index it returns is a valid g_palette index.
        std::vector<RE::BGSColorForm*> g_palette;
        std::vector<std::uint8_t>      g_channels;
        bool                           g_warm{ false };

        // Its OWN lock, not the state lock below. The two guard unrelated things
        // with opposite lifetimes: the palette is built once and then read
        // forever, while g_state churns per actor. Sharing one lock would put a
        // per-frame editor read (the snapped-swatch Snap in the Appearance
        // section) behind the map's contention for no reason.
        //
        // Not a std::once_flag, tempting as write-once data makes it: call_once
        // CONSUMES its flag on a normal return, and WarmPaletteLocked
        // deliberately returns WITHOUT warming when the data handler does not
        // exist yet, so the next call can retry. A once_flag would latch that
        // early return and disable the whole feature for the session, which is
        // exactly the outcome the retry was written to avoid.
        std::mutex g_paletteLock;

        // Assumes g_paletteLock is held. Split out so Snap can warm and read in
        // ONE acquisition: the lock is non-recursive, so a public WarmPalette
        // call from inside Snap's critical section would deadlock.
        void WarmPaletteLocked() {
            if (g_warm) {
                return;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                // Called before the data handler exists. Stay COLD and retry on the
                // next call rather than caching an empty palette for the session,
                // which would silently disable the whole feature.
                return;
            }
            g_warm = true;
            for (auto* form : dh->GetFormArray<RE::BGSColorForm>()) {
                if (!form) {
                    continue;
                }
                g_palette.push_back(form);
                g_channels.push_back(form->color.red);
                g_channels.push_back(form->color.green);
                g_channels.push_back(form->color.blue);
            }
            spdlog::info("HairColor: palette warmed with {} colour forms.", g_palette.size());
        }

        // Per actor: what they had before FR touched them, and what FR last set.
        // "Last applied" is what makes the re-capture check in Apply possible.
        //
        // ⚠ LOCKED, and it has to be. This map is reached from THREE threads: the
        // FUCK present thread (the editor's staging paths, which is where
        // OutfitSession pushes a colour from), the input thread (the quick-switch
        // hotkey), and the game thread (save, load and revert through
        // Persistence). An unordered_map rehashing on insert while another thread
        // reads it is the same shape as the window-state map race that already
        // cost this project a CTD during menu churn. "Those paths cannot really
        // overlap in time" was the reasoning that made THAT one look safe too.
        //
        // ⚠ Non-recursive, so no function below may call another PUBLIC function
        // in this module while holding it. Apply's delegation to Restore is the
        // live case: it happens before the lock is taken and must stay there.
        std::mutex g_stateLock;

        struct ActorState {
            CapturedColor     captured;
            // Whether capture has been ATTEMPTED for this actor, as distinct from
            // whether it succeeded. Describe() refuses a runtime (0xFF) hair
            // colour because it cannot be described portably, and without this
            // flag the next Apply would re-enter the capture branch, find FR's
            // OWN colour already applied, and store that as the original. In
            // memory that is merely useless; persisted it is actively wrong,
            // because a later session would restore FR's colour as if it were
            // the user's. In-memory only, deliberately: the persisted shape
            // stays as it is.
            bool              attempted{ false };
            RE::BGSColorForm* lastApplied{ nullptr };
            // The colour the user actually PICKED, before Snap quantized it to a
            // colour form. Repaint writes tintColor straight onto the geometry
            // and can carry any 24-bit value, so there is no reason for what is
            // on screen to inherit the snap's error.
            //
            // Measured on the 2026-07-30 field log, 426 paired samples: the snap
            // landed exactly 5 times (1.2%), mean per-channel error 10.3/255,
            // worst 50. It is not a hair palette - it is every BGSColorForm in
            // the load order, so picks resolve to lip, skin and warpaint colours.
            // The form write still has to happen (it is what an engine-driven
            // head rebuild re-derives from), but it is the FALLBACK now, not the
            // thing the player looks at.
            bool              exactSet{ false };
            std::uint8_t      exactR{ 0 };
            std::uint8_t      exactG{ 0 };
            std::uint8_t      exactB{ 0 };

            // GEOMETRY-ONLY actors: the base has no HeadRelatedData at all, so
            // there is no hairColor member to write and `lastApplied` stays
            // null. Vanilla Skyrim.esm has 384 NPC records with neither a hair
            // colour nor a template to inherit one from, 86 of them unique with
            // head parts, and the follower Jenassa is one - which is how this
            // was found. Those characters used to get nothing at all.
            //
            // The colour still reaches the screen, because Repaint writes the
            // material directly and never needed the base for that. What is
            // lost is durability: nothing persists, so an engine-driven head
            // rebuild reverts it until the next refresh.
            //
            // Session-scoped ON PURPOSE, and not part of CapturedColor. The
            // effect itself is session-scoped, so the undo matches it exactly,
            // and CapturedColor is a form reference that lands in the co-save -
            // a raw RGB does not fit it without another schema bump, and the
            // schema is already at v6 with no downgrade path.
            bool              baseless{ false };
            bool              originalTintSet{ false };
            std::uint8_t      originalR{ 0 };
            std::uint8_t      originalG{ 0 };
            std::uint8_t      originalB{ 0 };
        };
        std::unordered_map<RE::FormID, ActorState> g_state;

        // How many times we have written each actor's colour, under g_stateLock
        // and beside the state it describes.
        //
        // ⚠ ITS OWN MAP RATHER THAN A FIELD ON ActorState, for the reason
        // HeadPart's counter is its own map: Restore ERASES the entry, and the
        // question this answers - "did Fitting Room move this colour while the
        // character editor was open" - is asked across a window in which a
        // restore is one of the likelier things to have happened. A count that
        // vanished with the entry would read zero on the case it exists for.
        std::unordered_map<RE::FormID, std::uint32_t> g_writes;

        // Both writers go through here, so no SetHairColor can land uncounted.
        // Called with g_stateLock NOT held.
        void CountWrite(RE::FormID a_actor) {
            std::scoped_lock l(g_stateLock);
            ++g_writes[a_actor];
        }

        RE::TESNPC* BaseOf(RE::Actor* a_actor) {
            return a_actor ? a_actor->GetActorBase() : nullptr;
        }

        // The engine's own conversion, from PrepareHeadPartForShaders: each byte
        // scaled by (1/255) then doubled. RE::Actor::UpdateHairColor divides by
        // 128 instead, which is close but not identical, and a later head rebuild
        // would re-derive with THIS constant and visibly shift the colour.
        constexpr float kTintScale = (1.0f / 255.0f) * 2.0f;

        // The inverse of kTintScale, hoisted so every reader of a hair
        // material agrees with the writer byte for byte. Clamped because
        // nothing guarantees a material's existing tint came from this scale
        // at all: a nif can ship any value.
        [[nodiscard]] std::uint8_t ToByte(float a_v) {
            const float b = a_v / kTintScale + 0.5f;
            return static_cast<std::uint8_t>(b < 0.0f ? 0.0f : b > 255.0f ? 255.0f : b);
        }

        // A read-only census of what the hair materials are ACTUALLY wearing,
        // filled by the same traversal that paints them so the three readers
        // cannot drift apart.
        //
        // ⚠⚠ THE MATERIAL POINTER IS A FIELD BECAUSE VALUE ALONE CANNOT
        // TELL A WRITE FROM A REBUILD. The exported face tint was chased for
        // three rounds on values that looked plausible; what named the cause
        // was one NiTexture POINTER turning up on two different characters.
        struct TintCensus {
            std::size_t    count{ 0 };
            bool           uniform{ true };
            bool           haveFirst{ false };
            std::string    firstName;
            std::uintptr_t firstMat{ 0 };
            RE::NiColor    first{};

            // ⚠⚠ THE ROLL, AND IT IS WHAT r80 WENT LOOKING FOR. Every
            // hair pulse in the bleed session came back NOT UNIFORM across two
            // materials and then reported only the first one, so nobody has
            // ever seen what the second carried. An instrument that says two
            // readings disagree without saying how stops one question short of
            // the answer, and that is the question: whether the strands hold
            // one stale colour beside one fresh one, or two of the same wrong
            // value.
            std::vector<std::string> roll;
            std::vector<std::string> names;
        };

        // Visit every hair-tint material under a_root and return how many there
        // were. Writes a_write onto each when it is non-null; when a_previous is
        // non-null, reports what the FIRST one carried beforehand.
        //
        // ONE function for all three uses - paint, restore and sample - because
        // they have to agree exactly on the traversal predicate and the byte
        // conversion. A restore that matched a different set of materials than
        // the apply it undoes would leave part of the hair tinted, and a sample
        // that read a different material than the paint later writes would seed
        // the picker from the wrong thing.
        std::size_t TraverseHairTint(RE::NiAVObject* a_root, const HairTint* a_write,
                                     HairTint* a_previous, TintCensus* a_census = nullptr) {
            RE::NiColor tint{};
            if (a_write) {
                tint = RE::NiColor{ a_write->r * kTintScale, a_write->g * kTintScale,
                                    a_write->b * kTintScale };
            }
            std::size_t written = 0;
            RE::BSVisit::TraverseScenegraphGeometries(
                a_root, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    // properties[] lives behind the runtime-data accessor: its offset
                    // differs between VR and SE/AE, which is exactly what that
                    // indirection exists to hide.
                    auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                        a_geom->GetGeometryRuntimeData()
                            .properties[RE::BSGeometry::States::kEffect]
                            .get());
                    if (!prop || !prop->material) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    if (prop->material->GetFeature() !=
                        RE::BSShaderMaterial::Feature::kHairTint) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    auto* const mat =
                        static_cast<RE::BSLightingShaderMaterialHairTint*>(prop->material);
                    if (a_previous && !a_previous->set) {
                        a_previous->set = true;
                        a_previous->r   = ToByte(mat->tintColor.red);
                        a_previous->g   = ToByte(mat->tintColor.green);
                        a_previous->b   = ToByte(mat->tintColor.blue);
                    }
                    // ⚠ BEFORE THE WRITE BELOW, or the census reports what
                    // this very call just painted instead of what it found.
                    if (a_census) {
                        ++a_census->count;
                        const char* const gname = a_geom->name.c_str();
                        a_census->names.emplace_back(gname ? gname : "(unnamed)");
                        a_census->roll.push_back(fmt::format(
                            "{}=({},{},{})@0x{:X}", a_census->names.back(),
                            ToByte(mat->tintColor.red), ToByte(mat->tintColor.green),
                            ToByte(mat->tintColor.blue),
                            reinterpret_cast<std::uintptr_t>(mat)));
                        if (!a_census->haveFirst) {
                            a_census->haveFirst = true;
                            a_census->firstMat  = reinterpret_cast<std::uintptr_t>(mat);
                            a_census->first     = mat->tintColor;
                            if (const char* n = a_geom->name.c_str(); n) {
                                a_census->firstName = n;
                            }
                        } else if (a_census->first.red != mat->tintColor.red ||
                                   a_census->first.green != mat->tintColor.green ||
                                   a_census->first.blue != mat->tintColor.blue) {
                            a_census->uniform = false;
                        }
                    }
                    if (a_write) {
                        mat->tintColor = tint;
                    }
                    ++written;
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            return written;
        }

        // What the editor's picker seeds from when the actor base has no hair
        // colour to read. Populated on the GAME thread by Repaint, read on the
        // FUCK present thread by CurrentColorOf.
        //
        // ⚠ This indirection is the whole point. The value lives on the hair
        // material, and the editor must not walk the scenegraph to get it: the
        // present thread can be reading while the game thread rebuilds an actor,
        // which is a use-after-free waiting to happen. Sampling where the paint
        // already happens costs one extra traversal per unmanaged geometry-only
        // actor per session and needs no new thread discipline.
        std::mutex                                g_observedLock;
        std::unordered_map<RE::FormID, HairTint>  g_observed;

        // ⚠ NEVER call GetLocalFormID unguarded. It derefs GetFile(0) with no
        // null check, so ANY runtime (0xFF) form CTDs it, and another mod may
        // well have set this actor's hair colour to a dynamic form. Guard on the
        // DEFINING FILE, not on IsDynamicForm(): the file is what GetLocalFormID
        // actually dereferences, so it is the only guard that covers every way
        // sourceFiles can come back empty.
        CapturedColor Describe(RE::BGSColorForm* a_form) {
            CapturedColor out;
            if (!a_form) {
                return out;  // a null original is a real, capturable state
            }
            auto* const file = a_form->GetFile(0);
            if (!file) {
                // Runtime-created colour. It cannot be described portably and it
                // will not exist next session, so refuse to capture it rather
                // than store a reference that cannot be honoured.
                spdlog::warn("HairColor: actor's hair colour {:08X} has no defining "
                             "file (runtime form); not capturing it.",
                             a_form->GetFormID());
                return out;
            }
            out.captured    = true;
            out.modName     = std::string{ file->GetFilename() };
            out.localFormID = a_form->GetLocalFormID();
            return out;
        }

        RE::BGSColorForm* Resolve(const CapturedColor& a_captured) {
            if (!a_captured.captured || a_captured.modName.empty()) {
                return nullptr;
            }
            auto* dh = RE::TESDataHandler::GetSingleton();
            return dh ? dh->LookupForm<RE::BGSColorForm>(a_captured.localFormID,
                                                         a_captured.modName)
                      : nullptr;
        }
    }

    void WarmPalette() {
        std::scoped_lock l(g_paletteLock);
        WarmPaletteLocked();
    }

    RE::BGSColorForm* Snap(const HairTint& a_tint) {
        // Warm and read under ONE acquisition. Splitting them would let a second
        // thread read g_channels while the first is still push_back-ing into it,
        // which is a reallocation racing a span read - the vectors are only
        // immutable AFTER warming completes, not during.
        //
        // NearestColorIndex runs inside the critical section deliberately: it is
        // pure integer arithmetic over ~180 triples with no engine access, so it
        // cannot re-enter anything, and hoisting it would mean copying the
        // channel buffer out just to avoid a microsecond of holding.
        std::scoped_lock l(g_paletteLock);
        WarmPaletteLocked();
        const auto idx = NearestColorIndex(a_tint.r, a_tint.g, a_tint.b, g_channels);
        // ⚠ The returned pointer OUTLIVES the lock, and that is fine: a
        // BGSColorForm is owned by the data handler and stable for the whole
        // session, so the lock protects the CONTAINERS, never the forms in them.
        return idx == kNoColorMatch ? nullptr : g_palette[idx];
    }

    std::string NameOf(RE::BGSColorForm* a_form) {
        if (!a_form) {
            return {};
        }
        const char* name = a_form->GetFullName();
        return name ? name : std::string{};
    }

    void Apply(RE::Actor* a_actor, const Outfit& a_outfit) {
        ApplyTint(a_actor, a_outfit.hairTint);
    }

    void Push(RE::Actor* a_actor, const Outfit* a_outfit) {
        if (!a_actor) {
            return;
        }
        // ⚠ THE OUTFIT WINS BECAUSE IT IS THE MORE SPECIFIC STATEMENT, the same
        // precedence the hair STYLE ladder applies one rung over. An outfit
        // naming no colour falls to what this character is normally coloured,
        // and only a character with no default goes back to their own.
        //
        // ⚠ A CLEARED OUTFIT TINT IS "NAMES NOTHING", NOT "PAINT NOTHING".
        // HairTint has a presence bit precisely so the two are different, and
        // reading a cleared one as an instruction would make every outfit that
        // does not mention hair silently override the character's default.
        const HairTint want = (a_outfit && a_outfit->hairTint.set)
                                  ? a_outfit->hairTint
                                  : DefaultLook::HairColour(a_actor);
        ApplyTint(a_actor, want);
    }

    void ApplyTint(RE::Actor* a_actor, const HairTint& a_tint) {
        if (!a_actor) {
            return;
        }
        auto* const base = BaseOf(a_actor);
        if (!base) {
            return;
        }
        if (!a_tint.set) {
            // ⚠ Delegated BEFORE g_stateLock is taken, and it has to stay that
            // way: Restore takes the same non-recursive lock, so moving this
            // below the critical section is an instant self-deadlock.
            Restore(a_actor);
            return;
        }
        auto* const want = Snap(a_tint);
        if (!want) {
            spdlog::warn("HairColor: no colour form to snap to; hair colour skipped.");
            return;
        }

        auto* const hrd = base->headRelatedData;
        if (!hrd) {
            // No HeadRelatedData means no hairColor member to write, so the form
            // channel does not exist for this character. It is NOT a reason to
            // do nothing: Repaint writes the material directly and never needed
            // the base for the visual, only for durability. This used to return
            // here, which is why followers like Jenassa got no colour at all.
            //
            // Record the picked colour with lastApplied left null, which is
            // precisely what Repaint's "is the base still ours" test compares
            // against for these actors, and mark the entry baseless so the first
            // repaint knows to keep the tint it is about to overwrite.
            //
            // ⚠ No capture and no SetHairColor. There is nothing to capture (the
            // engine has no colour recorded for them) and nothing to write it
            // through, so CapturedFor still reports nothing and Persistence
            // still writes no baseline. Nothing about this touches the save.
            std::scoped_lock l(g_stateLock);
            auto&            st = g_state[a_actor->GetFormID()];
            st.baseless    = true;
            st.lastApplied = nullptr;
            st.exactSet    = true;
            st.exactR      = a_tint.r;
            st.exactG      = a_tint.g;
            st.exactB      = a_tint.b;
            spdlog::debug("HairColor: actor {:08X} has no HeadRelatedData; geometry-only "
                          "tint ({},{},{}), not durable across a head rebuild.",
                          a_actor->GetFormID(), a_tint.r, a_tint.g, a_tint.b);
            return;
        }
        auto* const cur = hrd->hairColor;

        {
            // Everything that touches g_state happens here, and the one engine
            // MUTATION (SetHairColor) happens after the release. Describe stays
            // inside: it only reads a form's defining file, cannot re-enter this
            // module, and its decision depends on state read under this lock, so
            // hoisting it would mean either a second acquisition or calling it
            // unconditionally - and unconditional Describe would fire its
            // runtime-form warning on every apply instead of only on a capture.
            std::scoped_lock l(g_stateLock);
            auto&            st = g_state[a_actor->GetFormID()];

            // Capture, and RE-capture when someone else changed the colour behind us.
            //
            // Without the second half of this condition the feature has a nasty
            // footgun: change your hair in RaceMenu while an FR colour is active and
            // FR's captured "original" is stale, so restoring it would silently undo
            // the RaceMenu edit. If the current colour is not what FR last applied,
            // it was changed externally and becomes the new original.
            if (!st.attempted || cur != st.lastApplied) {
                st.captured  = Describe(cur);
                st.attempted = true;
            }

            // Recorded in the SAME critical section as the decision above, NOT
            // after the SetHairColor below. That ordering is the whole reason
            // this is one acquisition and not two: lastApplied is what the
            // recapture test compares against, so a window where the decision
            // has been made but lastApplied still names the previous colour lets
            // a concurrent Apply conclude "someone else changed it" and store
            // FR's OWN colour as the user's original - re-arming the exact
            // footgun the test above exists to disarm. Writing it here shrinks
            // that window from a whole engine call to nothing this module can
            // observe. It cannot be closed completely without holding the lock
            // across SetHairColor, which is not worth a deadlock risk: it would
            // take two threads tinting the SAME actor simultaneously, and every
            // caller is one edge-triggered user action on one subject.
            st.lastApplied = want;

            // Recorded in the same acquisition as lastApplied, and paired with
            // it on purpose: Repaint only trusts this exact colour while
            // lastApplied is still what sits on the actor base. That pairing is
            // what stops Fitting Room repainting its own colour over one the
            // user set somewhere else, so the two must never be written apart.
            st.exactSet = true;
            st.exactR   = a_tint.r;
            st.exactG   = a_tint.g;
            st.exactB   = a_tint.b;
        }

        base->SetHairColor(want);
        CountWrite(a_actor->GetFormID());
        spdlog::debug("HairColor: actor {:08X} -> {:08X} '{}' for tint ({},{},{})",
                      a_actor->GetFormID(), want->GetFormID(), NameOf(want), a_tint.r,
                      a_tint.g, a_tint.b);
    }

    void Restore(RE::Actor* a_actor) {
        if (!a_actor) {
            return;
        }
        // Geometry-only actors first. Their undo is a repaint, not a form write,
        // and the base test below would send them straight out of the function
        // with Fitting Room's colour still on the hair.
        {
            HairTint original{};
            bool     baseless = false;
            {
                std::scoped_lock l(g_stateLock);
                const auto       it = g_state.find(a_actor->GetFormID());
                if (it != g_state.end() && it->second.baseless) {
                    baseless = true;
                    if (it->second.originalTintSet) {
                        original.set = true;
                        original.r   = it->second.originalR;
                        original.g   = it->second.originalG;
                        original.b   = it->second.originalB;
                    }
                    // Erased here, unlike the form path below, and safely so:
                    // there is no Resolve that can fail. The colour to put back
                    // is three bytes already in hand, not a form reference that
                    // depends on a plugin still being in the load order.
                    g_state.erase(it);
                }
            }
            if (baseless) {
                // Painted OUTSIDE the lock: PaintHairTint walks the scenegraph,
                // and g_stateLock is not held across engine work anywhere else
                // in this module either.
                //
                // A clear original means Repaint never ran for this actor, so
                // nothing was ever painted and there is nothing to undo.
                if (original.set) {
                    if (auto* const root = a_actor->Get3D(false)) {
                        const auto n = TraverseHairTint(root, &original, nullptr);
                        spdlog::debug("HairColor: actor {:08X} geometry-only restore, "
                                      "repainted {} material(s) to ({},{},{}).",
                                      a_actor->GetFormID(), n, original.r, original.g,
                                      original.b);
                    }
                }
                return;
            }
        }
        auto* const base = BaseOf(a_actor);
        if (!base || !base->headRelatedData) {
            // Same reasoning as Apply: nothing to write through, so keep the
            // capture rather than erasing state we were never able to undo.
            return;
        }
        // Take the baseline by VALUE under the lock, resolve and write outside it,
        // and erase ONLY once the restore actually happened.
        //
        // ⚠ THE ERASE MUST NOT MOVE BACK ABOVE THE RESOLVE. It used to sit in the
        // critical section, on the reasoning that a baseline which no longer
        // resolves is forgotten either way. That is what made a failed Resolve
        // destroy the only record of the user's real hair colour:
        //   * player: CapturedFor then reports nothing, so Persistence writes no
        //     HCOL record at all and the baseline is gone from the save too.
        //   * follower: worse, because the caller also clears the NPCO triple. With
        //     both the record and this entry gone while OUR colour is still on the
        //     actor base, the next tinted Apply takes the !attempted branch, calls
        //     Describe on our own colour, and persists THAT as the follower's
        //     pre-Fitting-Room original. A later Restore then paints it on as if it
        //     were theirs, permanently.
        // Resolve fails when the plugin that defined the captured HCLF leaves the
        // load order. For most characters that is Skyrim.esm, so it is not reachable
        // by accident, but it is trivially reachable the moment anyone disables a
        // plugin - which the field-test protocol asks for by name.
        CapturedColor captured;
        {
            std::scoped_lock l(g_stateLock);
            const auto       it = g_state.find(a_actor->GetFormID());
            if (it == g_state.end() || !it->second.captured.captured) {
                return;  // never touched this actor: nothing to undo
            }
            captured = it->second.captured;  // copy, do not erase yet
        }
        auto* const original = Resolve(captured);
        if (!original) {
            // KEEP the baseline. The colour on the actor is still ours, and this
            // entry is the only thing that knows what to put back, so holding it
            // lets a later attempt succeed once the plugin returns. Reporting the
            // failure and keeping the record is strictly better than reporting it
            // and throwing the record away.
            spdlog::warn("HairColor: captured original for actor {:08X} ('{}' {:06X}) no "
                         "longer resolves; leaving the current colour alone and KEEPING "
                         "the baseline.",
                         a_actor->GetFormID(), captured.modName, captured.localFormID);
            return;
        }
        base->SetHairColor(original);
        CountWrite(a_actor->GetFormID());
        {
            // Re-acquire to drop the consumed entry. No iterator is held across the
            // engine call, which is the thing that would actually break if another
            // thread inserted meanwhile.
            //
            // Only erase if it is still OUR entry: between the two critical sections
            // another thread could have captured something newer for this actor, and
            // dropping that would lose a live baseline. Compare the value rather than
            // trusting presence.
            std::scoped_lock l(g_stateLock);
            const auto       it = g_state.find(a_actor->GetFormID());
            if (it != g_state.end() && it->second.captured.captured &&
                it->second.captured.modName == captured.modName &&
                it->second.captured.localFormID == captured.localFormID) {
                g_state.erase(it);
            }
        }
        spdlog::debug("HairColor: actor {:08X} restored to {:08X}", a_actor->GetFormID(),
                      original->GetFormID());
    }

    bool RestoreIfStillOurs(RE::Actor* a_actor) {
        if (!a_actor) {
            return false;
        }
        auto* const base = BaseOf(a_actor);
        auto* const hrd  = base ? base->headRelatedData : nullptr;
        if (!hrd) {
            return false;  // geometry-only actors have no form to contaminate
        }
        {
            // The guard, under the lock; the restore itself runs through
            // Restore() OUTSIDE it (non-recursive lock, same rule Apply's
            // no-tint branch follows).
            std::scoped_lock l(g_stateLock);
            const auto       it = g_state.find(a_actor->GetFormID());
            if (it == g_state.end() || !it->second.captured.captured ||
                !it->second.lastApplied ||
                hrd->hairColor != it->second.lastApplied) {
                return false;  // not ours any more (or never was); leave it
            }
        }
        spdlog::info(
            "HairColor: actor {:08X} still wears this session's colour after a "
            "load that names no look; restoring the captured original.",
            a_actor->GetFormID());
        Restore(a_actor);
        return true;
    }

    std::optional<HairTint> CurrentColorOf(RE::Actor* a_actor) {
        if (!a_actor) {
            return std::nullopt;
        }
        auto* const base  = BaseOf(a_actor);
        auto* const hrd   = base ? base->headRelatedData : nullptr;
        auto* const color = hrd ? hrd->hairColor : nullptr;
        if (!color) {
            // Geometry-only actor: no colour form to read, so fall back to what
            // Repaint sampled off the hair material on the game thread. Absent
            // until that actor has been refreshed at least once with Fitting
            // Room loaded, which for a follower you can see in the editor has
            // always already happened.
            std::scoped_lock l(g_observedLock);
            const auto       it = g_observed.find(a_actor->GetFormID());
            if (it == g_observed.end()) {
                return std::nullopt;
            }
            return it->second;
        }
        HairTint t;
        t.set = true;
        t.r   = color->color.red;
        t.g   = color->color.green;
        t.b   = color->color.blue;
        return t;
    }

    std::optional<std::uint32_t> CurrentColourFormId(RE::Actor* a_actor) {
        if (!a_actor) {
            return std::nullopt;
        }
        auto* const base = BaseOf(a_actor);
        auto* const hrd  = base ? base->headRelatedData : nullptr;
        if (!hrd) {
            // ⚠ THE GEOMETRY-ONLY ACTOR IS "COULD NOT READ", NOT "NO COLOUR",
            // and the difference is the whole reason this is not CurrentColorOf.
            // There is no colour channel on this character at all, so no reading
            // taken here can be compared with another one.
            return std::nullopt;
        }
        auto* const color = hrd->hairColor;
        return color ? color->GetFormID() : 0u;
    }

    std::uint32_t WriteCountFor(RE::Actor* a_actor) {
        if (!a_actor) {
            return 0;
        }
        std::scoped_lock l(g_stateLock);
        const auto       it = g_writes.find(a_actor->GetFormID());
        return it == g_writes.end() ? 0u : it->second;
    }

    void ReseedCaptured(RE::Actor* a_actor) {
        if (!a_actor) {
            return;
        }
        auto* const base = BaseOf(a_actor);
        auto* const hrd  = base ? base->headRelatedData : nullptr;
        if (!hrd) {
            return;  // no colour channel to read a new baseline from
        }
        auto* const cur = hrd->hairColor;

        std::scoped_lock l(g_stateLock);
        const auto       it = g_state.find(a_actor->GetFormID());
        if (it == g_state.end() || !it->second.captured.captured) {
            // Nothing was ever captured for this actor. Inventing a baseline
            // here is exactly what the header refuses: a later Restore would
            // paint it on as though it were theirs.
            return;
        }
        const auto fresh = Describe(cur);
        if (!fresh.captured && cur) {
            // Describe refused a runtime form. Keeping the old reference beats
            // replacing it with an empty one, which Restore reads as "never
            // touched" and which would strand our colour on the character.
            return;
        }
        if (fresh.captured == it->second.captured.captured &&
            fresh.modName == it->second.captured.modName &&
            fresh.localFormID == it->second.captured.localFormID) {
            return;
        }
        spdlog::info("HairColor: actor {:08X} baseline re-pointed at '{}'|{:06X}; what "
                     "they had before was '{}'|{:06X}.",
                     a_actor->GetFormID(), fresh.modName, fresh.localFormID,
                     it->second.captured.modName, it->second.captured.localFormID);
        it->second.captured = fresh;
        // ⚠ STATED RATHER THAN LEFT ALONE. It is already true on every path
        // that reaches here, and it must stay true: a cleared `attempted` sends
        // the next Apply into the capture branch, where it would Describe the
        // colour Fitting Room itself is about to put on.
        it->second.attempted = true;
    }

    std::size_t Repaint(RE::Actor* a_actor) {
        if (!a_actor) {
            return 0;
        }
        auto* const root = a_actor->Get3D(false);
        if (!root) {
            return 0;  // not loaded; the next head build will derive it from the base
        }
        auto* const base  = BaseOf(a_actor);
        auto* const hrd   = base ? base->headRelatedData : nullptr;
        auto* const color = hrd ? hrd->hairColor : nullptr;

        // Prefer the colour the user actually picked over the form the snap
        // chose. Both go through the SAME conversion below, so this changes the
        // VALUE painted and nothing about how it is painted.
        //
        // ⚠ Gated on lastApplied still being the colour on the actor base, not
        // merely on having an exact colour recorded. If anything outside Fitting
        // Room has since written the base - the character editor is the known
        // case - then the base is the truth and repainting our own colour over
        // it would be fighting the user. Falling back to the form there is what
        // makes this safe to run unconditionally on every refresh.
        //
        // The same test carries the GEOMETRY-ONLY actor for free, which is why
        // there is no second branch for it: a base with no HeadRelatedData gives
        // color == nullptr, Apply left lastApplied == nullptr for exactly that
        // reason, and null == null matches. Those actors have no other source,
        // so when the test fails there is nothing to fall back to.
        std::uint8_t srcR  = 0;
        std::uint8_t srcG  = 0;
        std::uint8_t srcB  = 0;
        bool         exact = false;
        bool         wantOriginal = false;
        // ⚠⚠ THE ONE EXTERNAL WRITE THAT IS NOT AN EDIT: THE BASE PUT BACK TO
        // THE VERY COLOUR FR CAPTURED AS THE ORIGINAL. Field 2026-08-18 22:27:
        // kPostLoadGame's reassert wrote Steel Grey and painted [exact] at
        // 22:27:12; a second head build on the player at 22:27:15, from
        // nobody in this log, found the base holding RaceMenu.esp|000801
        // again, the player's own colour, and this function yielded to it
        // ([form] rgb=(43,10,13)) until the editor opened. That is RaceMenu (or
        // the engine's own change form) re-applying the player's recorded
        // colour AFTER our post-load push, on its own deferred tick, and it is
        // not a choice anyone made: the "external edit" rule below exists for a
        // colour picked in the head editor, and a pick that lands exactly on
        // the captured original is (a) rare and (b) overridden anyway when the
        // head editor closes (HeadEditorSink re-asserts). So a base that reads
        // as the captured original while an exact colour is recorded is a
        // RESET, and the answer to a reset is to assert again: the form goes
        // back on the base so the next head build derives the right thing,
        // and the exact colour goes on the geometry.
        RE::BGSColorForm* reassert = nullptr;
        {
            std::scoped_lock l(g_stateLock);
            const auto       it = g_state.find(a_actor->GetFormID());
            if (it != g_state.end() && it->second.exactSet) {
                if (it->second.lastApplied == color) {
                    exact = true;
                } else if (color && it->second.lastApplied &&
                           color == Resolve(it->second.captured)) {
                    exact    = true;
                    reassert = it->second.lastApplied;
                }
            }
            if (exact) {
                srcR = it->second.exactR;
                srcG = it->second.exactG;
                srcB = it->second.exactB;
                // First paint on a geometry-only actor: the tint currently on
                // the material is the only record of what they looked like
                // before, and it is about to be overwritten. Capturing it here
                // rather than in Apply is not a preference - Apply runs on the
                // FUCK present thread and must not walk the scenegraph, while
                // this function is documented main-thread only.
                wantOriginal = it->second.baseless && !it->second.originalTintSet;
            }
        }
        if (reassert) {
            // Outside the lock, the way Apply writes it. lastApplied is
            // untouched: it already names this form, which is the whole reason
            // the recapture test in Apply will not fire on the next push.
            if (auto* const base2 = BaseOf(a_actor)) {
                base2->SetHairColor(reassert);
                CountWrite(a_actor->GetFormID());
            }
            spdlog::info("HairColor: actor {:08X} base was reset to its captured original "
                         "{:08X} behind Fitting Room; re-asserting {:08X} '{}' and painting "
                         "the exact colour. Something re-applies the character's own hair "
                         "colour after our post-load push; RaceMenu is the usual author.",
                         a_actor->GetFormID(), color->GetFormID(), reassert->GetFormID(),
                         NameOf(reassert));
        }
        if (!exact) {
            if (!color) {
                // No picked colour and no form, so nothing to paint. Take the
                // one useful reading available instead: this actor's hair as it
                // stands is what the editor's picker should start from if the
                // user ever tints them, and this is the only thread allowed to
                // look. Once per actor - the map is checked first - so a
                // follower being refreshed repeatedly costs one traversal, not
                // one per refresh.
                bool needed = false;
                {
                    std::scoped_lock l(g_observedLock);
                    needed = !g_observed.contains(a_actor->GetFormID());
                }
                if (needed) {
                    HairTint seen{};
                    TraverseHairTint(root, nullptr, &seen);
                    if (seen.set) {
                        std::scoped_lock l(g_observedLock);
                        g_observed[a_actor->GetFormID()] = seen;
                    }
                }
                return 0;
            }
            srcR = color->color.red;
            srcG = color->color.green;
            srcB = color->color.blue;
        }

        const HairTint want{ true, srcR, srcG, srcB };
        HairTint       previous{};
        std::size_t    written =
            TraverseHairTint(root, &want, wantOriginal ? &previous : nullptr);
        if (wantOriginal && previous.set) {
            std::scoped_lock l(g_stateLock);
            const auto       it = g_state.find(a_actor->GetFormID());
            // Re-found under a fresh acquisition, and re-tested: Restore can
            // have run and erased the entry while the scenegraph walk was in
            // flight, and recreating it here would leave a baseline for an
            // actor Fitting Room no longer manages.
            if (it != g_state.end() && it->second.baseless &&
                !it->second.originalTintSet) {
                it->second.originalTintSet = true;
                it->second.originalR       = previous.r;
                it->second.originalG       = previous.g;
                it->second.originalB       = previous.b;
            }
        }

        // Log unconditionally, including the zero. Zero is not noise: it means
        // this character's hair carries no hair-tint material, so nothing can
        // paint it and the honest answer to the user is that the hair does not
        // support tinting. Distinguishing that from "wrote some, still looks
        // wrong" is the only way to tell a renderer problem from a mesh problem.
        // Logs what was actually PAINTED, and which of the two sources it came
        // from. The tag comes from the BRANCH TAKEN, not from comparing the two
        // colours: when the snap happens to land exactly - 5 times in 426 field
        // samples - a value comparison would report "form" for a paint that
        // really did come from the exact path, which is precisely backwards for
        // the diagnostic this line exists to serve.
        spdlog::debug("HairColor: repaint actor {:08X} wrote {} hair-tint material(s) "
                      "rgb=({},{},{}) [{}]",
                      a_actor->GetFormID(), written, srcR, srcG, srcB,
                      exact ? "exact" : "form");
        return written;
    }

    CapturedColor CapturedFor(RE::Actor* a_actor) {
        if (!a_actor) {
            return {};
        }
        std::scoped_lock l(g_stateLock);
        const auto       it = g_state.find(a_actor->GetFormID());
        return it == g_state.end() ? CapturedColor{} : it->second.captured;
    }

    void SeedCaptured(RE::Actor* a_actor, const CapturedColor& a_captured) {
        if (!a_actor || !a_captured.captured) {
            return;
        }
        // Read the actor's CURRENT colour before the lock. It is only needed for
        // the lastApplied seed at the bottom, but hoisting it empties the
        // critical section of engine access entirely, and reading it even when
        // the seed is refused costs two dereferences and has no side effect.
        auto* const baseNow  = BaseOf(a_actor);
        auto* const hrdNow   = baseNow ? baseNow->headRelatedData : nullptr;
        auto* const colorNow = hrdNow ? hrdNow->hairColor : nullptr;

        std::scoped_lock l(g_stateLock);
        auto&            st = g_state[a_actor->GetFormID()];
        if (st.captured.captured) {
            // Load-time seed only, and a live capture is newer than one. A live
            // capture is the CURRENT truth about what this actor looked like
            // before FR touched them this session, and the saved one is a
            // snapshot of that same fact from an earlier session. If a co-save
            // load lands after Apply already captured (OBody readiness
            // re-refreshes, a race switch, a second load into a running
            // session), overwriting would replace the fresh reading with a stale
            // one and Restore would put back the wrong colour.
            return;
        }
        st.captured  = a_captured;
        st.attempted = true;

        // Seed lastApplied too, or the FIRST Apply of the session recaptures and
        // overwrites the seed just restored: the capture condition also fires on
        // cur != lastApplied, and a null lastApplied differs from any real
        // colour, so the seed would be replaced by FR's own applied colour.
        //
        // It does not need persisting, because it is derivable. A persisted
        // capture means FR was active when the save was written, and
        // SetHairColor writes ACTOR BASE state that lands in the save's
        // form-change record, so on load the actor's CURRENT colour IS what FR
        // last applied. Deriving it here keeps the wire format unchanged.
        //
        // A genuine external edit made between load and the first Apply is still
        // caught: cur would then differ from this seeded value.
        st.lastApplied = colorNow;
    }

    void Clear() {
        // g_state only, deliberately NOT g_palette/g_channels/g_warm. The palette
        // is keyed to the LOAD ORDER (see WarmPalette's own comment: "the load
        // order cannot change mid-session, so this never needs invalidating"), and
        // a save revert changes which CHARACTER we are, not which plugins are
        // loaded. Clearing it here would buy nothing and cost a full re-scan of
        // every BGSColorForm on the next Snap().
        //
        // A plain container clear - no actor touched, no SetHairColor called, by
        // construction. See Clear()'s own comment in HairColor.h for why that
        // matters at the point this is called from. Locked all the same: a clear
        // rehashes nothing but it invalidates every iterator and node in the map,
        // so a concurrent reader is exactly as exposed as it is on an insert.
        {
            std::scoped_lock l(g_stateLock);
            g_state.clear();
            // The write counts describe the character being torn down, and
            // their key is a form id the next character reuses: the player is
            // 0x14 in every save.
            g_writes.clear();
        }
        // The observed tints go too. They describe how a specific character's
        // hair looked, and after a revert the same form ID can be a character
        // with a different appearance entirely, so keeping them would seed the
        // picker from somebody else's hair. Sampled again on the next refresh
        // at the cost of one traversal.
        //
        // Separate acquisition, not one scoped_lock over both: taking two locks
        // together is how a lock-order rule gets created, and there is no
        // invariant spanning these two maps that would need one.
        {
            std::scoped_lock l(g_observedLock);
            g_observed.clear();
        }
    }

    std::string GeometryTintReading(RE::Actor* a_actor) {
        if (!a_actor) {
            return {};
        }
        // Get3D(false) is the THIRD-person root, deliberately, and it is the
        // same root Repaint paints from: Get3D() is the first-person one
        // whenever the player is in first person, and a reader that walked a
        // different tree than the writer would grade the wrong strands.
        auto* const root = a_actor->Get3D(false);
        if (!root) {
            return {};
        }
        return GeometryTintReadingOf(root);
    }

    std::string GeometryTintReadingOf(RE::NiAVObject* a_root) {
        if (!a_root) {
            return {};
        }
        TintCensus census{};
        TraverseHairTint(a_root, nullptr, nullptr, &census);
        if (!census.haveFirst) {
            return "(no hair-tint material)";
        }
        auto line = fmt::format("'{}' rgb=({},{},{}) mat=0x{:X} across {} material(s){}",
                                census.firstName, ToByte(census.first.red),
                                ToByte(census.first.green), ToByte(census.first.blue),
                                census.firstMat, census.count,
                                census.uniform ? "" : ", NOT UNIFORM");
        // Only when they disagree. A uniform reading is already fully described
        // by the line above, and the roll would just make every repaint pulse
        // longer to no purpose.
        if (!census.uniform) {
            line += " [";
            for (std::size_t i = 0; i < census.roll.size(); ++i) {
                if (i != 0) {
                    line += ' ';
                }
                line += census.roll[i];
            }
            line += ']';
        }
        return line;
    }

    std::vector<std::string> HairTintNodeNamesOf(RE::NiAVObject* a_root) {
        if (!a_root) {
            return {};
        }
        TintCensus census{};
        TraverseHairTint(a_root, nullptr, nullptr, &census);
        auto names = std::move(census.names);
        // Two materials can sit under one geometry name, and asking skee the
        // same question twice would read as two holders in the log.
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

}  // namespace OS::HairColor
