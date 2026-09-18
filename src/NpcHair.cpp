#include "NpcHair.h"

#include "NpcHairNames.h"  // OwnRootName: the one prefix the dye walk reads back
#include "OutfitDye.h"     // QueueRepaint: a re-attached style gets its ornament colour back

#include "FsmpXmlOverrides.h"  // SwapIfMapped: wig xmls that hold the body's bones

#include "BuildChannel.h"

#include "FaceGenFod.h"  // DynVertex + the FOD reader, shared with SculptProbe
#include "FsmpBridge.h"
#include "HairColor.h"
#include "HeadPart.h"  // Kind, SlotOf and KindName: which of her pieces to cull (OS-225)
#include "JsonCodec.h"
#include "NpcHairPlan.h"
#include "Settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OS::NpcHair {

    namespace {

        // The engine's own head-part machinery. Every id pair is derived in
        // docs/re/head-part-attach.md from both binaries, and the AE side was
        // read out of the AE attach-all's own call sites rather than guessed
        // from the id family. Argument types come from what each body DOES with
        // them, never from a name.
        using AttachHeadPart_t   = void (*)(RE::BSFaceGenManager*, RE::NiNode*,
                                          RE::BGSHeadPart*, RE::TESNPC*, bool);
        using UpdateFaceModels_t = void (*)(RE::NiAVObject*);
        using ApplyFaceGenData_t = void (*)(RE::BSFaceGenManager*, RE::NiNode*,
                                            RE::BGSHeadPart*, RE::TESNPC*);

        // ---- one capture per (actor, kind), OS-225 ------------------------
        //
        // ⚠ THE KEY IS THE ACTOR AND THE KIND TOGETHER. A follower can wear a
        // style of ours and a beard of ours at once, and each is culled,
        // attached, restored and re-asserted on its own; keying on the actor
        // alone would make the beard's Apply RESTORE the hair first, because
        // ApplyNow takes the previous capture off before it puts the new one
        // on. Packed into one integer so the three maps below need no custom
        // hash and no equality: the form id in the high bits, the kind in the
        // low byte.
        using Kind = HeadPart::Kind;
        using Slot = std::uint64_t;

        [[nodiscard]] constexpr Slot SlotKey(RE::FormID a_id, Kind a_kind) {
            return (static_cast<Slot>(a_id) << 8) | static_cast<Slot>(a_kind);
        }
        [[nodiscard]] constexpr RE::FormID SlotId(Slot a_slot) {
            return static_cast<RE::FormID>(a_slot >> 8);
        }
        [[nodiscard]] constexpr Kind SlotKind(Slot a_slot) {
            return static_cast<Kind>(a_slot & 0xFFu);
        }

        // The two kinds this route takes, in the order Reassert and the
        // pre-load teardown walk them. ⚠ NOT eyes and NOT brows: see the header.
        inline constexpr Kind kKinds[] = { Kind::kHair, Kind::kFacialHair };

        // A part's kind, read off its engine type, or nothing for a part this
        // module does not handle. ⚠ OFF THE PART AND NEVER OFF THE CALLER,
        // because the kind decides which of her own pieces get culled
        // (GetCurrentHeadPartByType), and a caller naming the wrong one would
        // cull her hair to attach a beard.
        [[nodiscard]] std::optional<Kind> KindOfPart(const RE::BGSHeadPart* a_part) {
            if (!a_part) {
                return std::nullopt;
            }
            for (const auto kind : kKinds) {
                if (a_part->type.underlying() == HeadPart::SlotOf(kind)) {
                    return kind;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] RE::BGSHeadPart::HeadPartType EngineTypeOf(Kind a_kind) {
            return static_cast<RE::BGSHeadPart::HeadPartType>(HeadPart::SlotOf(a_kind));
        }

        // The word for the log lines. "hair" reads as it always did; a beard's
        // lines say so, because a log with both kinds in it has to say which
        // one just did what.
        [[nodiscard]] const char* KindWord(Kind a_kind) {
            return a_kind == Kind::kFacialHair ? "facial hair" : "hair";
        }

        // OS-99 (Task 6.5): one stripped FMD block. The NiPointer keeps the
        // extra-data object and the m_model chain hanging off it alive while
        // the block is off its geometry; see StripFmd for why it comes off.
        struct StrippedFmd {
            std::string                    geomName;
            RE::NiPointer<RE::NiExtraData> data;
        };

        struct Applied {
            RE::BGSHeadPart*         part{ nullptr };
            std::vector<std::string> hidden;    // her own pieces, culled
            std::vector<std::string> attached;  // ours, added
            bool                     smp{ false };  // admitted via kAdmitSmp
            // The style's FMD blocks, held OFF the geometries for the attach
            // lifetime (StripFmd's shield). Riding in the record so nothing
            // dangles even when SkinSingle never fired; the refs release
            // naturally when the record drops at restore or back-out.
            std::vector<StrippedFmd> fmd;
        };

        std::mutex                        g_lock;
        std::unordered_map<Slot, Applied> g_applied;  // keyed by SlotKey (OS-225)

        // ⚠ READ FROM INSIDE A DETOUR, WHICH IS WHY THEY ARE ATOMICS. The head
        // build hook may not take g_lock (its own note is explicit: no lock, no
        // walk, no session), so the two questions it is allowed to ask are
        // "does this module hold anything at all" and "is a reassert already on
        // its way". Both are one relaxed load.
        //
        // g_everApplied is never cleared. A false positive costs one queued
        // task that finds an empty map and returns; a false negative costs the
        // doubled hair this exists to catch.
        std::atomic<bool>                 g_everApplied{ false };
        std::atomic<bool>                 g_buildReassertQueued{ false };

        // What we have DECIDED this actor should wear, recorded the instant the
        // editor asks rather than when the queued work lands. Two reasons, and
        // the second is the one that bites:
        //
        //  * the row would otherwise label itself from stale state for a frame,
        //  * and the hover preview edge-detects on "is this already current",
        //    so a decision that is not visible yet re-fires every frame until
        //    the task drains, queuing a pile of duplicate applies.
        //
        // An entry present with a null value means "restore to her own", which
        // is distinct from having no entry at all.
        std::unordered_map<Slot, RE::BGSHeadPart*> g_pending;  // keyed by SlotKey

        // OS-99 tripwire v2 (Task 6.5). An FSMP admit records itself here and
        // the NEXT NpcHair entry for the actor (ApplyNow or RestoreNow, the
        // only two that mutate the head) measures it. Same-pass measuring was
        // wrong in both directions: SkinSingle registers the system but the
        // WORLD steps it, so in the admit's own pass a rigid mesh still reads
        // its stale attach-time bound (dead gate) and a freshly skinned
        // physics mesh can read a transient (false trip). In a browse the
        // next entry lands within a second; and with FMD shielded (StripFmd)
        // a real FSMP-side failure renders as a rigid drape rather than a
        // stretch, so this gate is belt-and-braces rather than the only
        // safety. Guarded by g_lock; cleared by the check itself, by Clear,
        // and implicitly by whichever of the two entries runs first.
        struct SmpPendingCheck {
            RE::FormID               styleFormID{ 0 };
            std::string              edid;        // for the check's log lines
            std::vector<std::string> geomNames;   // what landed, what to measure
            // Task 6.6, the dwell hole: an admit the user just LOOKS at gets
            // judged by age instead of by the next entry. The handle is what
            // TickPendingSmpChecks marshals its one-shot with; writtenAt both
            // ages the entry and serves as its GENERATION token, so a tick
            // queued against this admit can never consume a newer one.
            RE::ActorHandle                       handle;
            std::chrono::steady_clock::time_point writtenAt{};
            bool queuedTick{ false };  // an aged one-shot is already queued
            // Task 6.7, the settle window: how many looks this entry has
            // taken. A mid-settle look re-arms the entry with a FRESH
            // writtenAt (the generation token moves with each re-arm) rather
            // than consuming it; firstArmedAt survives re-arms so the final
            // verdict can name the total time watched.
            std::uint32_t                         looks{ 0 };
            std::chrono::steady_clock::time_point firstArmedAt{};
        };
        // ⚠ KEYED BY SlotKey LIKE THE CAPTURES, so a pending look on her hair
        // is never consumed by, or mistaken for, a look on her beard. In
        // practice a beard is never SMP-capable and never arms one of these;
        // the keying is for the invariant, not the case.
        std::unordered_map<Slot, SmpPendingCheck> g_smpPendingCheck;

        // The post-apply settle ladder (the cell-return doubled hair,
        // 2026-08-21). Field sequence, prev.log 16:32:46: cell return, the
        // load-side reassert re-applies and logs culled 3/3, and her own hair
        // is back on screen anyway - something un-hides her pieces AFTER our
        // cull, through a pass that fires no head-part build, so no seam we
        // hold ever asks the second question again. The ladder is that
        // second ask, three times, at fixed delays after every publish:
        // ReassertNow is idempotent (ours present + hers hidden = a handful
        // of name lookups and silence), so a quiet head costs three walks
        // and the entry dies; a turbulent one re-arms through the same
        // ApplyNow/recovery sites that armed it.
        //
        // ⚠ DRAINED FROM THE RENDER THREAD by the settle ticker window
        // (NpcHairSettle.{h,cpp}), the same FUCK gameplay-frame seam the dye
        // cards draw from, because the one other deferred mechanism here
        // (TickPendingSmpChecks) ticks only while the EDITOR draws and this
        // symptom's whole habitat is the editor being shut. Same discipline
        // as that tick: collect under the lock, marshal through OnGameThread,
        // never touch the engine on the calling thread.
        struct SettleWatch {
            RE::ActorHandle                       handle;
            std::chrono::steady_clock::time_point armedAt{};   // generation token
            std::size_t                           stage{ 0 };  // next delay index
            bool                                  queued{ false };
        };
        std::unordered_map<RE::FormID, SettleWatch> g_settle;
        // Mirrors g_settle.empty() so the ticker's IsOpen stays a lock-free
        // data query, per the card window's contract.
        std::atomic<bool> g_settlePending{ false };

        inline constexpr std::array<std::chrono::milliseconds, 3> kSettleDelays{
            std::chrono::milliseconds{ 750 },   // the 16:31:31.817 comeback was +745 ms
            std::chrono::milliseconds{ 2000 },
            std::chrono::milliseconds{ 5000 },  // RaceMenu's own post-load pass is ~3 s
        };

        // The ceiling and the admission rule both live in NpcHairPlan.h, with
        // the derivation of why it is EIGHT and not the nine the engine's own
        // guard implies. This file measures; that one decides.
        using NpcHairPlan::kMaxHeadPartBones;

        // Styles already measured as unbindable. Keyed on the part rather
        // than the actor: the bone count is a property of the mesh, so once one
        // follower has proved it the answer holds for all of them.
        std::unordered_set<RE::FormID> g_unsupported;

        // The session-only subset of g_unsupported (OS-99 Task 6.5): FSMP-side
        // failures, declined until restart and never persisted. Tracked apart
        // so the editor can say "failed this session, retries next" instead
        // of "can never bind". Guarded by g_lock with the rest.
        std::unordered_set<RE::FormID> g_unsupportedSession;

        // The OS-106 subset of g_unsupportedSession: styles declined ONLY
        // because no FSMP route was armed at the time.
        //
        // ⚠⚠ IT EXISTS SO A LATE ARM CAN UNDO ITS OWN DAMAGE (OS-247). The
        // route can arm at kPostLoadGame rather than at kDataLoaded, because
        // FSMP installs its head hooks from its own low-priority pass and a
        // foreign hook can land on the entry between the two. Every SMP style
        // browsed before that moment is already sitting in the session set with
        // a cause that has since stopped being true, and without this it stays
        // grey until the player restarts the game, which reads as the fix not
        // working. Tracked apart from the rest of the session set because an
        // FSMP-side BUILD failure is a different cause and arming cures nothing
        // about it. Guarded by g_lock with the rest.
        std::unordered_set<RE::FormID> g_unsupportedFsmpAbsent;

        // The persisted half of g_unsupported, as mod|id keys, INCLUDING
        // entries whose plugin is currently absent (they ride along so the
        // answer is still there when the plugin returns). Guarded by g_lock
        // with everything else here.
        std::vector<StyleRefKey> g_unsupportedKeys;

        // OS-105: every style that has measured smpCapable at least once this
        // session. It exists to keep a DEMOTION out of the sidecar.
        //
        // A physics style whose MeasureSmp answers not-capable falls straight
        // through JudgeStyle to JudgeBinding, and its strand chains are long by
        // construction, so the verdict is kTooManyBones and the decline path
        // PERSISTS it. One bad measurement therefore blacklists a working style
        // on disk for good. The measurement is known to flip (a style read
        // capable=true three times and capable=false immediately after a save
        // load), so a decline derived from it is not evidence about the mesh.
        // Guarded by g_lock with the rest.
        std::unordered_set<RE::FormID> g_smpMeasuredCapable;

        const auto kUnsupportedPath = BuildChannel::DataPath("unsupported-hair.json");

        // Same atomic-replace shape as Persistence's library save: write a
        // sibling tmp, rename over. Called on the GAME thread from ApplyNow's
        // decline path; the file is a few hundred bytes and declines are rare
        // (once ever per style), so synchronous is fine.
        void SaveUnsupportedLocked() {
            Json::Value root = JsonCodec::UnsupportedHairToJson(g_unsupportedKeys);
            std::error_code   ec;
            const std::string tmp = kUnsupportedPath.string() + ".tmp";
            std::filesystem::create_directories(
                std::filesystem::path(kUnsupportedPath).parent_path(), ec);
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out) {
                    spdlog::warn("NpcHair: cannot write {}.", tmp);
                    return;
                }
                out << root;
            }
            std::filesystem::rename(tmp, kUnsupportedPath, ec);
            if (ec) {
                std::filesystem::copy_file(
                    tmp, kUnsupportedPath,
                    std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove(tmp, ec);
            }
        }

        // Record a declined part in both halves of the set and persist. The
        // identity is defining-file + local id, the same pair every StyleRefKey
        // in the codebase carries.
        //
        // ⚠ GetLocalFormID derefs GetFile(0) with NO null check (see
        // [[commonlib-getlocalformid-pitfall]] and NpcIdentity.h), so the file
        // is fetched and tested first. A runtime-created part has no defining
        // file and nothing useful to persist anyway: it cannot name the same
        // mesh next session.
        void MarkUnsupportedLocked(RE::BGSHeadPart* a_part) {
            if (!g_unsupported.insert(a_part->GetFormID()).second) {
                return;  // already known, nothing new to persist
            }
            auto* const file = a_part->GetFile(0);
            if (!file) {
                return;
            }
            StyleRefKey key{ std::string(file->GetFilename()),
                             a_part->GetLocalFormID() };
            for (const auto& k : g_unsupportedKeys) {
                if (k == key) {
                    return;  // already on disk (loaded this session)
                }
            }
            g_unsupportedKeys.push_back(std::move(key));
            SaveUnsupportedLocked();
        }

        // OS-99: a decline that must NOT outlive its cause. An FSMP-side
        // build failure is transient (wrong config, world pressure, a bad
        // xml edit mid-session), so it never enters the sidecar; the style
        // re-measures next session. The session set carries the cause so the
        // editor's tooltip can name it (IsSessionUnsupported).
        // a_fsmpAbsent distinguishes the two causes that reach the session set.
        // An FSMP-side BUILD failure is transient but nothing we do cures it; a
        // decline taken because no route was armed is cured exactly when one
        // arms, so it is recorded separately (OS-247).
        //
        // ⚠ ONLY OUR OWN INSERT IS RECORDED AS REVERSIBLE. If the style was
        // already in g_unsupported before this call it got there some other
        // way, most likely resolved out of unsupported-hair.json at startup,
        // and clearing it later would un-grey a mesh whose decline has nothing
        // to do with FSMP. Recording only what this call added is what makes
        // the clear an exact undo instead of an amnesty.
        void MarkUnsupportedSessionLocked(RE::BGSHeadPart* a_part, bool a_fsmpAbsent) {
            const auto id       = a_part->GetFormID();
            const bool wasKnown = g_unsupported.find(id) != g_unsupported.end();
            g_unsupported.insert(id);
            g_unsupportedSession.insert(id);
            if (a_fsmpAbsent && !wasKnown) {
                g_unsupportedFsmpAbsent.insert(id);
            }
        }

        // What the engine names the geometry it builds for a head part: the
        // part's editor ID, stamped from BGSHeadPart+0x118 (formEditorID). That
        // is why a name is enough to find a piece again without holding a
        // pointer across frames, and why both the cull and the undo key on it.
        void CollectPartNames(RE::BGSHeadPart* a_part, std::vector<std::string>& a_out) {
            if (!a_part) {
                return;
            }
            if (const char* id = a_part->GetFormEditorID(); id && *id) {
                a_out.emplace_back(id);
            }
            for (auto* const ep : a_part->extraParts) {
                if (!ep) {
                    continue;
                }
                if (const char* id = ep->GetFormEditorID(); id && *id) {
                    a_out.emplace_back(id);
                }
            }
        }

        // ⚠⚠ A NAME IS NOT AN OWNER (OS-246, the bald follower). The engine
        // stamps every head-part geometry with the PART's editor ID, ours and
        // hers alike, and hair packs share extra parts (a hairline, a scalp)
        // across styles, so a follower whose own hair is family to the style
        // we attach ends up with TWO objects under ONE name. Every question
        // this module used to ask by bare name could then land on the wrong
        // body: the ours-check found HER rebuilt hairline under OUR recorded
        // name and skipped the re-apply, and the come-back cull hid OUR fresh
        // extras as "her pieces returned". Jagyr Stormhand, 17:50-17:51, one
        // cell change: first our hairline eaten live, then bald on return.
        //
        // The answer is that OUR pieces stop wearing engine names at all:
        // ApplyNow renames every root it just attached to kOwnPrefix + name,
        // records the renamed form, and from there bare-name lookups can only
        // ever resolve HER pieces while kOwnPrefix lookups can only resolve
        // OURS. No engine system resolves our attachments by name afterwards;
        // everything of ours that does (reassert, restore, detach, the SMP
        // deferred look) reads the recorded, renamed strings.
        constexpr std::string_view kOwnPrefix = NpcHairNames::kOwnPrefix;

        // ⚠ FOUND IS NOT THE SAME QUESTION AS SHOWING, and the reassert needs
        // the second one. SetHiddenByName below answers "was there a node by
        // that name", so it cannot tell a piece that came back from one that is
        // still hidden exactly where we left it, and a re-cull driven off it
        // would report work it did not do on every pass.
        [[nodiscard]] bool VisibleByName(RE::NiAVObject* a_root, const std::string& a_name) {
            if (!a_root || a_name.empty()) {
                return false;
            }
            const RE::BSFixedString name{ a_name.c_str() };
            auto* const             obj = a_root->GetObjectByName(name);
            return obj && !obj->GetFlags().all(RE::NiAVObject::Flag::kHidden);
        }

        // BipedPost's mechanism, deliberately not SetAppCulled: the flags
        // member sits at a different offset on each runtime (+0xF4 SE, +0x10C
        // AE) and GetFlags() is the accessor that hides that.
        bool SetHiddenByName(RE::NiAVObject* a_root, const std::string& a_name, bool a_hide) {
            if (!a_root || a_name.empty()) {
                return false;
            }
            const RE::BSFixedString name{ a_name.c_str() };
            auto* const             obj = a_root->GetObjectByName(name);
            if (!obj) {
                return false;
            }
            if (a_hide) {
                obj->GetFlags().set(RE::NiAVObject::Flag::kHidden);
            } else {
                obj->GetFlags().reset(RE::NiAVObject::Flag::kHidden);
            }
            return true;
        }

        // Everything the admission test needs, measured off the loaded mesh.
        // It has to run on the geometry rather than the record because nothing
        // in a BGSHeadPart says how its mesh is rigged, which is why the check
        // sits after the attach and not before.
        struct BindingReading {
            std::uint32_t maxBones{ 0 };
            std::uint32_t unbound{ 0 };
            std::string   worstGeom;
            std::string   unboundGeom;
            std::string   firstUnboundBone;
            // The partition defect (NpcHairPlan::JudgePartition), plus the
            // numbers for the log line that names it.
            bool          partDefect{ false };
            std::string   partGeom;
            std::uint32_t partMapLen{ 0 };
            std::uint32_t partSkinBones{ 0 };
            // ⚠ EVERY GEOMETRY, NOT JUST THE WORST, AND IT IS A ROUTE DECISION
            // RATHER THAN A DIAGNOSTIC. The decline line names one loser -
            // 'VirtualHairCollision_2' at 17 bones - and that mesh is an SMP
            // COLLISION PROXY, not anything the player sees. What it does not
            // say is what the VISIBLE strands bind, and the two answers point
            // at completely different implementations:
            //
            //   visible strands <= 8: the ceiling is only being blown by the
            //     proxies. Drop them from the attach list and SMP styles go
            //     through the existing head-part path, losing hair-to-body
            //     collision and keeping the swing. Small change.
            //   visible strands > 8: the head-part path can never carry these,
            //     because the ceiling is FixSkinInstances' override block. The
            //     route is then to attach them as ordinary skinned meshes on
            //     the skeleton the way wigs work, where the block is not
            //     involved and the limit does not exist.
            struct GeomBinding {
                std::string   name;
                std::uint32_t bones{ 0 };
                std::uint32_t unbound{ 0 };
            };
            std::vector<GeomBinding> perGeom;
        };

        // ⚠ TWO READINGS, BECAUSE THEY ARE TWO FAULTS. The count is the
        // engine's off-by-one on its override block. The unbound tally is
        // whether the bones the mesh names actually EXIST on this actor, and a
        // mesh can fail that at any count: KSSMP_Ominous_Elf binds seven, all
        // under the ceiling, six of them strand-chain bones that live only
        // inside the hair nif. Counting alone shipped that to a follower's head
        // and it stretched to infinity.
        BindingReading MeasureBinding(RE::NiAVObject* a_faceNode, RE::NiNode* a_skeleton,
                                      const std::vector<std::string>& a_names) {
            BindingReading r;
            for (const auto& n : a_names) {
                const RE::BSFixedString nm{ n.c_str() };
                auto* const             obj = a_faceNode->GetObjectByName(nm);
                if (!obj) {
                    continue;
                }
                RE::BSVisit::TraverseScenegraphGeometries(
                    obj, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                        auto* const skin = a_geom->GetGeometryRuntimeData().skinInstance.get();
                        auto* const sd   = skin ? skin->skinData.get() : nullptr;
                        if (!sd) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        if (sd->bones > r.maxBones) {
                            r.maxBones  = sd->bones;
                            r.worstGeom = a_geom->name.c_str();
                        }
                        r.perGeom.push_back({ a_geom->name.empty() ? "(unnamed)"
                                                                   : a_geom->name.c_str(),
                                              sd->bones, 0 });
                        // ⚠ THE PARTITION MAP, read exactly as the renderer
                        // will: one uploaded matrix per map entry. A defect
                        // here is per-mesh and permanent, so first hit wins.
                        if (!r.partDefect) {
                            if (auto* const sp = skin->skinPartition.get();
                                sp && sp->numPartitions >= 1 && sp->partitions.data()) {
                                std::uint32_t maxEntry = 0;
                                const auto&   p0       = sp->partitions[0];
                                for (std::uint16_t i = 0; i < p0.numBones; ++i) {
                                    if (p0.bones && p0.bones[i] > maxEntry) {
                                        maxEntry = p0.bones[i];
                                    }
                                }
                                if (NpcHairPlan::JudgePartition(
                                        sp->numPartitions, p0.numBones, maxEntry,
                                        sd->bones)) {
                                    r.partDefect    = true;
                                    r.partGeom      = a_geom->name.c_str();
                                    r.partMapLen    = p0.numBones;
                                    r.partSkinBones = sd->bones;
                                }
                            }
                        }
                        if (!skin->bones || !a_skeleton) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        // ⚠ THE SAME LOOKUP FixSkinInstances PERFORMS, against
                        // the same root we are about to hand it. A name that
                        // misses here misses there, and there it says nothing:
                        // the engine leaves the entry pointing at the nif's own
                        // node and the mesh draws with no world transform.
                        //
                        // Reading all sd->bones entries is in bounds. The
                        // engine's overrun is on the eight-entry EXTRA DATA
                        // block, a different array from this one, which the
                        // skin sizes to its own bone count.
                        for (std::uint32_t i = 0; i < sd->bones; ++i) {
                            auto* const bone = skin->bones[i];
                            if (!bone) {
                                continue;
                            }
                            if (!a_skeleton->GetObjectByName(bone->name)) {
                                ++r.unbound;
                                if (!r.perGeom.empty()) {
                                    ++r.perGeom.back().unbound;
                                }
                                if (r.firstUnboundBone.empty()) {
                                    r.firstUnboundBone = bone->name.c_str();
                                    r.unboundGeom      = a_geom->name.c_str();
                                }
                            }
                        }
                        return RE::BSVisit::BSVisitControl::kContinue;
                    });
            }
            return r;
        }

        // OS-99. Whether FSMP could build physics for the attached style:
        // the same chain its processGeometry walks, checked on the same
        // objects, logged either way. FMD -> BSFaceGenModel -> the retained
        // original part root -> a NiStringExtraData named
        // 'HDT Skinned Mesh Physics Object' whose value is the physics XML.
        //
        // ⚠ The two hops below are raw offsets because CommonLib only
        // forward-declares BSFaceGenModel. They are verified for the loaded
        // builds in docs/re/fsmp-cooperative-hair.md; both recovered pointers
        // are range-gated to user space (ResolveFod's gate, which also covers
        // null) before any dereference, AsNode() is the first virtual call on
        // the recovered root and runs only after its gate, and the result is
        // only ever used as a boolean plus a log string, never dereferenced
        // further.
        struct SmpReading {
            bool        capable{ false };
            std::string xml;        // first physics file seen, for the log
            std::string geomName;   // geometry that carried it
            // OS-99 (Task 6.5): EVERY geometry whose own chain resolves a
            // physics file. Until Task 6.8 this was also the SkinSingle set,
            // which is exactly what left the physics-less _HL geometries
            // unprocessed and dangling by adoption; the skin loop now keys
            // on fmdGeoms below, and this set keeps its admission meaning
            // (capable requires the physics pointer).
            std::vector<std::string> capableGeoms;
            // Task 6.8: every traversed geometry whose FMD chain resolves
            // to a non-null original root, physics pointer or not. This is
            // the set FSMP's processGeometry can repoint, and the set that
            // MUST be repointed; see the SkinSingle loop for the round-3
            // mechanism (an unprocessed geometry dangles by adoption).
            std::vector<std::string> fmdGeoms;
            // How many of fmdGeoms are DIRECT children of the face node and
            // tri-shapes.
            //
            // ⚠ COVERAGE FOR THE 4.0 ROUTE, WHICH IS ONE LEVEL DEEP. That
            // line's whole-head sweep walks the face node's immediate
            // AsTriShape children and nothing below them, so a geometry nested
            // under a part root is never handed to FSMP at all. Expected to
            // equal fmdGeoms.size(), because we attach through the engine's own
            // head-part path which ends in AttachChild on the face node, and
            // because vanilla heads render at all. Measured rather than assumed
            // so the case where it does not hold arrives as a warning instead
            // of as an invisible strand. ⚠ NOT A GATE: a gate here would
            // decline the exact styles this route exists to enable.
            std::size_t fmdDirectChildren{ 0 };
        };

        SmpReading MeasureSmp(RE::NiAVObject* a_faceNode,
                              const std::vector<std::string>& a_names) {
            SmpReading r;
            static const RE::BSFixedString fmdKey{ "FMD" };
            for (const auto& n : a_names) {
                const RE::BSFixedString nm{ n.c_str() };
                auto* const             obj = a_faceNode->GetObjectByName(nm);
                if (!obj) {
                    continue;
                }
                RE::BSVisit::TraverseScenegraphGeometries(
                    obj, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                        auto* const fmd = static_cast<RE::BSFaceGenModelExtraData*>(
                            a_geom->GetExtraData(fmdKey));
                        if (!fmd || !fmd->m_model) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        const auto* const model =
                            reinterpret_cast<const std::byte*>(fmd->m_model);
                        std::uintptr_t entry = 0;
                        std::memcpy(&entry, model + 0x10, sizeof(entry));
                        if (entry < 0x10000 || entry > 0x7FFFFFFFFFFFULL) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        std::uintptr_t rootAddr = 0;
                        std::memcpy(&rootAddr,
                                    reinterpret_cast<const std::byte*>(entry) + 0x08,
                                    sizeof(rootAddr));
                        if (rootAddr < 0x10000 || rootAddr > 0x7FFFFFFFFFFFULL) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        auto* const root =
                            reinterpret_cast<RE::NiAVObject*>(rootAddr);
                        // Task 6.8: the FMD chain resolved to a live original
                        // root, so FSMP's processGeometry can process this
                        // geometry whether or not a physics file turns up in
                        // the scan below. Recorded here, before the AsNode
                        // narrowing, because FSMP does not require the root
                        // to be a node either.
                        r.fmdGeoms.emplace_back(a_geom->name.c_str());
                        // The exact test 4.0.1's sweep applies to decide
                        // whether it will see this geometry at all.
                        if (a_geom->parent == a_faceNode && a_geom->AsTriShape()) {
                            ++r.fmdDirectChildren;
                        }
                        auto* const node = root->AsNode();
                        if (!node) {
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        // First capable geometry wins, and it can be a
                        // Virtual* collision proxy rather than the visible
                        // strands; the boolean is unaffected, readers of the
                        // log's via '...' should know.
                        const auto count = node->GetExtraDataSize();
                        for (std::uint16_t i = 0; i < count; ++i) {
                            auto* const xd = node->GetExtraDataAt(i);
                            if (!xd || !xd->name.c_str() ||
                                std::strcmp(xd->name.c_str(),
                                            "HDT Skinned Mesh Physics Object") != 0) {
                                continue;
                            }
                            // OS-99 adaptation: CommonLib's NiStringExtraData
                            // declares 'value' as a raw 'char*' (see
                            // build/release/vcpkg_installed/.../RE/N/NiStringExtraData.h,
                            // "char* value; // 18"), not a BSFixedString/
                            // std::string wrapper. No .c_str() here.
                            auto* const sd =
                                netimmerse_cast<RE::NiStringExtraData*>(xd);
                            if (sd && sd->value && sd->value[0]) {
                                if (!r.capable) {
                                    r.capable  = true;
                                    r.xml      = sd->value;
                                    r.geomName = a_geom->name.c_str();
                                }
                                // Task 6.5: keep scanning rather than kStop.
                                // The capable SET is the product now; the
                                // first hit only names the log line.
                                r.capableGeoms.emplace_back(a_geom->name.c_str());
                                return RE::BSVisit::BSVisitControl::kContinue;
                            }
                        }
                        return RE::BSVisit::BSVisitControl::kContinue;
                    });
            }
            return r;
        }

        // OS-99 (Task 6.5): THE FMD SHIELD, and it is the fix for the 9-bone
        // CTD the fourth gate was wrongly credited with covering. The FMD
        // block is dual-use: FSMP's processGeometry walks it to find its
        // physics file, and the engine's skinning worker (AE 0x432280, the
        // SkinAllGeometry path every follower publish drives) reads its
        // per-geometry bone-name override at `block + (bone + 4) * 8` behind
        // the guard `ed == 0 || 8 < bone`, off by one against the
        // eight-entry block. FSMP's bone patches cover ITS OWN reads, not
        // that inlined one (docs/re/fsmp-cooperative-hair.md), so a
        // kAdmitSmp style with >8-bone strands re-enters the field-confirmed
        // OOB-read CTD on every publish if the block is present.
        //
        // ABSENCE is the safe state: the worker's `ed == 0` arm skips the
        // override read entirely, and its plain rebind leaves non-resolving
        // strand entries untouched, so FSMP's repaired bone pointers survive
        // every later publish. The block therefore lives ON the geometry
        // only for the one instant FSMP reads it (RestoreFmd, SkinSingle,
        // strip again), and off it the rest of the attach lifetime, held in
        // the Applied record so nothing dangles.
        [[nodiscard]] std::vector<StrippedFmd> StripFmd(
            RE::NiAVObject* a_faceNode, const std::vector<std::string>& a_names) {
            std::vector<StrippedFmd>       out;
            static const RE::BSFixedString fmdKey{ "FMD" };
            for (const auto& n : a_names) {
                const RE::BSFixedString nm{ n.c_str() };
                auto* const             obj = a_faceNode->GetObjectByName(nm);
                if (!obj) {
                    continue;
                }
                RE::BSVisit::TraverseScenegraphGeometries(
                    obj, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                        if (auto* const xd = a_geom->GetExtraData(fmdKey)) {
                            // Ref taken BEFORE the remove, which may drop the
                            // array's own reference.
                            out.push_back({ a_geom->name.c_str(),
                                            RE::NiPointer<RE::NiExtraData>{ xd } });
                            a_geom->RemoveExtraData(fmdKey);
                        }
                        return RE::BSVisit::BSVisitControl::kContinue;
                    });
            }
            return out;
        }

        // The other half of the shield: re-add for the SkinSingle instant.
        // Resolves by geometry name, which is stable within the game-thread
        // pass the pair brackets (nothing renames between strip and restore).
        void RestoreFmd(RE::NiAVObject*                 a_faceNode,
                        const std::vector<StrippedFmd>& a_stripped) {
            for (const auto& s : a_stripped) {
                if (!s.data) {
                    continue;
                }
                const RE::BSFixedString nm{ s.geomName.c_str() };
                if (auto* const obj = a_faceNode->GetObjectByName(nm)) {
                    obj->AddExtraData(s.data.get());
                }
            }
        }

        // Shared by Restore and by the admission test's back-out, because a
        // rejected style has to come off exactly as thoroughly as a good one.
        //
        // ⚠ GetObjectByName searches the whole subtree, DetachChild only scans
        // direct children, so a name that resolves to something nested detaches
        // nothing. The re-query is what tells those two cases apart.
        struct DetachTally {
            std::size_t detached{ 0 }, stuck{ 0 }, missing{ 0 };
        };

        DetachTally DetachByNames(RE::NiNode* a_faceNode, const std::vector<std::string>& a_names) {
            DetachTally t;
            for (const auto& n : a_names) {
                const RE::BSFixedString nm{ n.c_str() };
                auto* const             obj = a_faceNode->GetObjectByName(nm);
                if (!obj) {
                    ++t.missing;  // gone already, most likely a head rebuild
                    continue;
                }
                {
                    RE::NiPointer<RE::NiAVObject> keep{ obj };  // survive the detach
                    a_faceNode->DetachChild(obj);
                }
                if (a_faceNode->GetObjectByName(nm)) {
                    ++t.stuck;  // still there: the detach did NOT take
                } else {
                    ++t.detached;
                }
            }
            return t;
        }

        // OS-99 (Task 6.5): detach the geometry LEAVES first, for styles that
        // went through FSMP. Its ActorManager::cleanHead reclaims a tracked
        // physics system only when the tracked LEAF geometry's parent link
        // breaks, and DetachByNames breaks the link of the NAMED node: a
        // wrapper-node part detached whole keeps its leaves parented inside
        // the detached subtree, so the system leaks for the session. Nested
        // leaves are unparented here; a part whose named node IS the geometry
        // is left to DetachByNames, which breaks that link itself and keeps
        // its detached/stuck tally honest.
        void DetachGeometryLeaves(RE::NiNode*                     a_faceNode,
                                  const std::vector<std::string>& a_names) {
            for (const auto& n : a_names) {
                const RE::BSFixedString nm{ n.c_str() };
                auto* const             obj = a_faceNode->GetObjectByName(nm);
                if (!obj) {
                    continue;
                }
                // Collect first: detaching mid-traversal would mutate the
                // child lists the visit is walking.
                std::vector<RE::NiPointer<RE::BSGeometry>> leaves;
                RE::BSVisit::TraverseScenegraphGeometries(
                    obj, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                        if (a_geom != obj) {
                            leaves.emplace_back(a_geom);
                        }
                        return RE::BSVisit::BSVisitControl::kContinue;
                    });
                for (const auto& g : leaves) {
                    if (auto* const parent = g->parent) {
                        parent->DetachChild(g.get());
                    }
                }
            }
        }

        // OS-246: claim the roots THIS attach just planted, by pointer, and
        // rename them into our own namespace. By pointer and not by name,
        // because at this instant a shared-family style has two objects per
        // shared name (hers, hidden by the cull; ours, fresh) and a name
        // lookup picks one of them by child order, which is exactly the
        // ambiguity being removed. Returns the renamed names, which replace
        // toAttach for every downstream lookup: the measures, the FMD strip,
        // the decline back-out, the record. A root the engine reused rather
        // than created stays in the before-set and keeps its engine name;
        // the count line below is what says that happened.
        [[nodiscard]] std::vector<std::string> ClaimFreshRoots(
            RE::NiNode* a_faceNode, const std::vector<RE::NiAVObject*>& a_before,
            std::size_t a_asked) {
            std::vector<std::string> renamed;
            if (!a_faceNode) {
                return renamed;
            }
            for (auto& child : a_faceNode->GetChildren()) {
                auto* const obj = child.get();
                if (!obj) {
                    continue;
                }
                if (std::find(a_before.begin(), a_before.end(), obj) !=
                    a_before.end()) {
                    continue;  // hers, or older ours: not this attach's work
                }
                std::string fresh{ kOwnPrefix };
                fresh += obj->name.c_str() ? obj->name.c_str() : "part";
                obj->name = RE::BSFixedString(fresh.c_str());
                // r53: a wig xml that registers body-physics bones has FSMP
                // re-pose them over CBPC every frame; the authored map swaps
                // such a root's physics file for the stripped copy, HERE,
                // before the publish reads the extra data.
                OS::FsmpXmlOverrides::SwapIfMapped(obj);
                renamed.push_back(std::move(fresh));
            }
            if (renamed.size() != a_asked) {
                spdlog::debug("NpcHair: attach planted {} fresh root(s) against {} "
                              "part name(s); a reused root keeps its engine name and "
                              "the old ambiguity with it.",
                              renamed.size(), a_asked);
            }
            return renamed;
        }

        // OS-99 (Task 6.6): how many of FSMP's per-head renamed bones
        // ("hdtSSEPhysics_AutoRename_*") currently live under her skeleton.
        // The field failure is stale rename state poisoning the next style's
        // merge, and this number read before and after the skin is the direct
        // evidence: growth across swaps is accumulation (our state), while a
        // fresh-launch first-apply failure at zero is upstream authoring
        // (the style's own xml/nif drift), not ours.
        std::size_t CountRenamedBones(RE::NiNode* a_skeleton) {
            std::size_t n = 0;
            if (!a_skeleton) {
                return n;
            }
            RE::BSVisit::TraverseScenegraphObjects(
                a_skeleton, [&](RE::NiAVObject* a_obj) -> RE::BSVisit::BSVisitControl {
                    const char* const nm = a_obj->name.c_str();
                    if (nm && std::string_view{ nm }.starts_with(
                                  "hdtSSEPhysics_AutoRename_")) {
                        ++n;
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            return n;
        }

        // How far above the face node's own origin a correctly placed mesh
        // sits, with a wide margin. Measured on Jenassa: her head's bound
        // reaches down to z 173.9 against a face node at 67.2, her own hair to
        // 168.6, a working accessory to 165.8. Roughly a hundred units of
        // clearance, so twenty is generous.
        constexpr float kUnplacedMarginZ = 20.0f;

        // ⚠ THE SIGNATURE OF A MESH THE PUBLISH DID NOT FULLY PLACE, and it is
        // measured rather than guessed at. An unplaced vertex reads as zero in
        // the node's own space, which puts it at the FACE NODE ORIGIN, down at
        // the actor's root. So a partly bound mesh has a bound stretching from
        // the head all the way down to that origin, which is what renders as a
        // fan or frill spilling over the neck and shoulders.
        //
        // The tell is exact rather than approximate: the broken
        // 0WuMeiNiangAccAzure measured a low edge of 67.28 against a face node
        // at 67.24, while everything correct stayed about a hundred above.
        //
        // ⚠ NOT gated on bHairFaceDump, deliberately. Which styles carry this
        // could not be found by clicking, because the broken meshes are
        // numbered and colour variants indistinguishable in the picker, and
        // three field runs missed them. Always-on means ordinary browsing
        // produces the list instead of a hunt.
        void ReportUnplacedGeometry(RE::NiNode* a_faceNode, const char* a_edid,
                                    RE::FormID a_actor,
                                    const std::vector<std::string>& a_names) {
            if (!a_faceNode) {
                return;
            }
            const float originZ = a_faceNode->world.translate.z;
            for (const auto& n : a_names) {
                const RE::BSFixedString nm{ n.c_str() };
                auto* const             obj = a_faceNode->GetObjectByName(nm);
                if (!obj) {
                    continue;
                }
                RE::BSVisit::TraverseScenegraphGeometries(
                    obj, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                        const auto& b   = a_geom->worldBound;
                        const float low = b.center.z - b.radius;
                        if (low < originZ + kUnplacedMarginZ) {
                            spdlog::warn(
                                "NpcHair: actor {:08X} style '{}' geometry '{}' reaches the "
                                "face node origin (low z {:.2f} vs origin {:.2f}, "
                                "centre z {:.2f}, r={:.2f}). The publish left vertices "
                                "unplaced, which renders as a fan from the head down to "
                                "the actor's root.",
                                a_actor, a_edid, a_geom->name.c_str(), low, originZ,
                                b.center.z, b.radius);
                        }
                        return RE::BSVisit::BSVisitControl::kContinue;
                    });
            }
        }

        struct Head {
            RE::TESNPC*           base{ nullptr };
            RE::BSFaceGenNiNode*  faceNode{ nullptr };
            RE::NiNode*           skeleton{ nullptr };
            RE::BSFaceGenManager* mgr{ nullptr };
            [[nodiscard]] bool    Ok() const { return base && faceNode && skeleton && mgr; }
        };

        Head ResolveHead(RE::Actor* a_actor) {
            Head h;
            if (!a_actor) {
                return h;
            }
            h.base = a_actor->GetActorBase();
            auto* const proc = a_actor->GetActorRuntimeData().currentProcess;
            auto* const mid  = proc ? proc->middleHigh : nullptr;
            h.faceNode = mid ? mid->faceNodeSkinned : nullptr;
            if (auto* const root = a_actor->Get3D(false)) {
                h.skeleton = root->AsNode();
            }
            h.mgr = RE::BSFaceGenManager::GetSingleton();
            return h;
        }

        // How many times this actor's face node has been published. Drift is
        // the open question, so the count is the x-axis of every reading.
        std::unordered_map<RE::FormID, int> g_publishes;

        // ⚠ THE READING THE LOG LINES COULD NOT GIVE. Both of them printed the
        // size of a name list rather than what the scenegraph did, so an apply
        // that attached nothing and a restore that detached nothing each
        // reported success. That is the same blind spot that made the attach
        // take five field runs, and it is now costing crashes.
        //
        // What this dumps is aimed at ONE question. PublishHead runs
        // FixSkinInstances over the face node, and the engine worker behind it
        // (AE 0x140432280) walks EVERY child and, per bone, WRITES
        // skinInstance->bones[i] and ->boneWorldTransforms[i], looping to
        // skinData->bones. Her head is one of those children, so we rewrite her
        // head's skin binding on every hover. The numbers that matter are
        // therefore the child count (is our geometry accumulating) and, for
        // each geometry, boneCount plus bone0 (is the binding moving under
        // repetition). A neck seam that appears only after the editor opens is
        // what drift would look like from the outside.
        void DumpFace(const Head& a_head, RE::FormID a_id, const char* a_when) {
            if (!Settings::GetSingleton().hairFaceDump || !a_head.faceNode) {
                return;
            }
            // GetChildren(), not the member: this DLL is built for SE, AE and
            // VR at once, so SKYRIM_CROSS_VR compiles the direct field out and
            // only the relocating accessor resolves on every runtime.
            auto&       kids = a_head.faceNode->GetChildren();
            std::size_t live = 0;
            for (const auto& c : kids) {
                if (c) {
                    ++live;
                }
            }
            const auto& fw = a_head.faceNode->world.translate;
            spdlog::info("[hair/face] {:08X} {} publish#{} slots={} live={} "
                         "faceWorld=({:.2f},{:.2f},{:.2f})",
                         a_id, a_when, g_publishes[a_id],
                         static_cast<std::size_t>(kids.size()), live, fw.x, fw.y, fw.z);
            RE::BSVisit::TraverseScenegraphGeometries(
                a_head.faceNode, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    const auto& gd    = a_geom->GetGeometryRuntimeData();
                    auto* const skin  = gd.skinInstance.get();
                    auto* const sdata = skin ? skin->skinData.get() : nullptr;
                    const char* bone0 = "(no skin)";
                    if (skin) {
                        bone0 = (skin->bones && skin->bones[0]) ? skin->bones[0]->name.c_str()
                                                                : "(no bones)";
                    }
                    // ⚠ THE NUMBERS THE FIRST CENSUS DID NOT HAVE, and their
                    // absence is why a growing neck seam was written off as
                    // pre-existing. Child count, bone count and bone0 all stay
                    // IDENTICAL while geometry drifts or deforms, so 31
                    // publishes of "nothing changed" proved less than it looked.
                    //
                    // world.translate and worldBound separate the two ways it
                    // can go: a moving translate with a stable radius is the
                    // head being re-placed (FixSkinInstances rebinding), while
                    // a changing RADIUS is the mesh itself deforming, which is
                    // what repeatedly re-applying a base morph to an already
                    // morphed head would do. Read them against publish#, which
                    // is the x-axis this whole line exists to provide.
                    //
                    // Both members sit above CommonLib's ENABLE_SKYRIM_VR guard
                    // in NiAVObject, so unlike the flags field they are at one
                    // offset on every runtime and need no accessor.
                    // ⚠ THE DYNAMIC BUFFER, and it is the last thing that can
                    // separate an accessory that renders from one that fans out
                    // to the node origin. A BSDynamicTriShape draws from
                    // dynamicData rather than from its static vertices, so a
                    // buffer that was never filled reads as zeros, and a vertex
                    // at zero sits at the FACE NODE'S OWN ORIGIN. That is
                    // exactly the z the broken meshes' bounds reach down to,
                    // which is what put this measurement here.
                    //
                    // Read it against 0WuHairpin, which is an accessory on the
                    // same head in the same publish and renders correctly. One
                    // differing field between those two is the answer.
                    const char*   gtype   = "other";
                    std::uint16_t verts   = 0;
                    std::uint16_t tris    = 0;
                    const void*   dynData = nullptr;
                    std::uint32_t dynSize = 0;
                    const auto    ty      = a_geom->GetType().get();
                    if (ty == RE::BSGeometry::Type::kTriShape ||
                        ty == RE::BSGeometry::Type::kDynamicTriShape) {
                        const auto& td = static_cast<RE::BSTriShape*>(a_geom)
                                             ->GetTrishapeRuntimeData();
                        verts = td.vertexCount;
                        tris  = td.triangleCount;
                        gtype = "tri";
                    }
                    if (ty == RE::BSGeometry::Type::kDynamicTriShape) {
                        const auto& dd = static_cast<RE::BSDynamicTriShape*>(a_geom)
                                             ->GetDynamicTrishapeRuntimeData();
                        dynData = dd.dynamicData;
                        dynSize = dd.dataSize;
                        gtype   = "dyn";
                    }
                    // ⚠ SHADING STATE, and its absence is why five probes in a
                    // row "exonerated" the hair code. A neck seam is not
                    // geometry: the head is kFaceGen, carrying a BAKED
                    // tintTexture that is her complexion, while the body is
                    // kFaceGenRGBTint carrying a plain skin texture. They match
                    // only because the facetint was generated against that body.
                    // Rebind or recompute the head's tintTexture and the two
                    // stop agreeing, which is a seam at the neck and nowhere
                    // else, with every vertex still exactly where it was.
                    //
                    // Step 5 of the attach sequence is the facegen data applier
                    // (SE 26259 / AE 26838), the one call this repo documents as
                    // the only thing that paints facegen tint, and we run it
                    // against the WHOLE face node once per part per hair change.
                    // So the head's textures are read here before and after the
                    // publish; if they differ, the sequence is repainting her.
                    const char* feat    = "(no shader)";
                    const char* tintTex = "";
                    char        tintRGB[32]{};
                    if (auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                            a_geom->GetGeometryRuntimeData()
                                .shaderProperty
                                .get());
                        prop && prop->material) {
                        using F = RE::BSShaderMaterial::Feature;
                        switch (prop->material->GetFeature()) {
                            case F::kFaceGen: {
                                feat        = "faceGen";
                                auto* const m = static_cast<
                                    RE::BSLightingShaderMaterialFacegen*>(prop->material);
                                tintTex = m->tintTexture ? m->tintTexture->name.c_str()
                                                         : "(NULL tintTexture)";
                                break;
                            }
                            case F::kFaceGenRGBTint: {
                                feat        = "faceGenRGB";
                                auto* const m = static_cast<
                                    RE::BSLightingShaderMaterialFacegenTint*>(prop->material);
                                std::snprintf(tintRGB, sizeof(tintRGB), "%.4f,%.4f,%.4f",
                                              m->tintColor.red, m->tintColor.green,
                                              m->tintColor.blue);
                                break;
                            }
                            case F::kHairTint: {
                                feat        = "hairTint";
                                auto* const m = static_cast<
                                    RE::BSLightingShaderMaterialHairTint*>(prop->material);
                                std::snprintf(tintRGB, sizeof(tintRGB), "%.4f,%.4f,%.4f",
                                              m->tintColor.red, m->tintColor.green,
                                              m->tintColor.blue);
                                break;
                            }
                            case F::kDefault: feat = "default"; break;
                            case F::kEye: feat = "eye"; break;
                            default: feat = "other"; break;
                        }
                    }
                    const auto& w = a_geom->world.translate;
                    const auto& b = a_geom->worldBound;
                    spdlog::info("[hair/face]   geom '{}' skin={} boneCount={} bone0='{}' "
                                 "world=({:.2f},{:.2f},{:.2f}) "
                                 "bound=({:.2f},{:.2f},{:.2f}) r={:.3f} "
                                 "type={} verts={} tris={} dyn={} dynSize={} "
                                 "feat={} tintTex='{}' tintRGB={}",
                                 a_geom->name.c_str(), skin ? "yes" : "no",
                                 sdata ? sdata->bones : 0u, bone0, w.x, w.y, w.z,
                                 b.center.x, b.center.y, b.center.z, b.radius,
                                 gtype, verts, tris, dynData ? "yes" : "no", dynSize,
                                 feat, tintTex, tintRGB[0] ? tintRGB : "-");
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
        }

        // ⚠ THE MEASUREMENT EVERY EARLIER PROBE WAS TOO COARSE TO MAKE. Five
        // rounds concluded "her head does not deform" from its bounding-sphere
        // radius holding at 12.11 to 12.25. A bound over ~996 vertices cannot
        // see a neck RING of a few dozen drifting a few units, so that number
        // never had the resolution to answer the question it was being used to
        // answer, and cumulative deformation was never actually eliminated.
        //
        // The head's dynamic buffer is {x,y,z,w} floats per vertex, which is
        // why dynSize is always vertexCount * 16. NPC Visual Editor's neck-seam
        // patch bands the head by HEIGHT (preserve below minZ + ~4.5, blend
        // above), so the neck is the lowest slice of the mesh. This reports
        // that slice's centroid per publish; drift across publishes is the
        // thing to read, not any single value.
        //
        // Always on and ONE line, unlike bHairFaceDump's ~44 per publish, so
        // ordinary browsing produces the series.
        using OS::FaceGen::DynVertex;

        // The head is the ONE kFaceGen geometry under the face node; brows,
        // eyes, mouth and every hair part carry other material features. That
        // makes the feature a sharper identifier than any name match.
        RE::BSDynamicTriShape* FindHeadDynShape(RE::NiAVObject* a_faceNode) {
            RE::BSDynamicTriShape* found = nullptr;
            if (!a_faceNode) {
                return nullptr;
            }
            RE::BSVisit::TraverseScenegraphGeometries(
                a_faceNode, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    auto* const prop = netimmerse_cast<RE::BSLightingShaderProperty*>(
                        a_geom->GetGeometryRuntimeData()
                            .shaderProperty
                            .get());
                    if (!prop || !prop->material ||
                        prop->material->GetFeature() !=
                            RE::BSShaderMaterial::Feature::kFaceGen ||
                        a_geom->GetType().get() != RE::BSGeometry::Type::kDynamicTriShape) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    found = static_cast<RE::BSDynamicTriShape*>(a_geom);
                    return RE::BSVisit::BSVisitControl::kStop;
                });
            return found;
        }

        // OS-99 (Task 6.6, honesty pass in the review): one SkinSingle on her
        // own head geometry, fired at every SMP teardown site. It enters
        // FSMP's processGeometry, whose FIRST act is the cleanHead sweep
        // (2.5 source, ActorManager.cpp:1035), so parts whose leaf parent
        // links the teardown just broke are reclaimed immediately, renamed
        // bones included. Her head resolves no physics xml, so FSMP records
        // a no-physics part and creates no system: the documented no-op
        // entry cost. ⚠ One log-reading side effect: the entry increments
        // FSMP's head.id before that early-out, so AutoRename generation
        // numbers skip ahead one per teardown.
        //
        // ⚠ WHAT THIS PROVABLY BUYS, AND WHAT IT DOES NOT. The same source
        // shows the head-path skeleton merge runs only inside
        // processGeometry, AFTER cleanHead in that same call, so even with
        // no nudge the new style's own first SkinSingle sweeps before it
        // merges. Source alone therefore cannot show that reclaiming
        // earlier changes what the incoming merge sees; the field failure's
        // true poisoning step is unidentified, and stale rename state is
        // the LEADING HYPOTHESIS rather than a proven mechanism (the
        // evidence: mixed AutoRename generations inside single constraints,
        // strand bone names shared across KS styles, failures concentrated
        // in browse sequences). This call moves reclaim to the earliest
        // possible point at no-op cost; the merged-bone census around the
        // skin loop is what actually adjudicates, in field round 2.
        void NudgeFsmpReclaim(const Head& a_head) {
            if (!FsmpBridge::IsAvailable()) {
                return;
            }
            // ⚠ NOTHING TO NUDGE ON THE 4.0 ROUTE. Both teardown sites publish
            // immediately before calling here, and on that line the publish is
            // itself a whole-head sweep into processGeometry, whose first act is
            // the cleanHead reclaim. So it has already happened, against the
            // links DetachGeometryLeaves just broke, and it covered EVERY
            // tracked part rather than only the one entered through her head
            // shape. Calling anyway would only skip FSMP's head.id forward.
            if (FsmpBridge::Route() == FsmpBridge::Coop::kSkinAllEntry) {
                return;
            }
            if (auto* const headShape = FindHeadDynShape(a_head.faceNode)) {
                FsmpBridge::NudgeSingle(a_head.faceNode, a_head.skeleton, headShape);
            }
        }

        // EVERY piece of her own face as it was BEFORE this module first touched
        // her, keyed by geometry name, captured once per actor.
        //
        // ⚠ HER HEAD WAS NEVER THE ONLY THING AT RISK, and pinning only the head
        // is why a second growing artefact survived the first fix. FixSkinInstances
        // rebinds EVERY child of the face node on every publish: her hair, her
        // hairline, her brows, her eyes and her mouth all take the same treatment
        // her head does. Her own hair deforming looks like a fan growing off her
        // head, and it stays visible for exactly as long as styles keep getting
        // declined, because a decline leaves her wearing her own hair.
        //
        // Keyed by NAME so only HERS is restored: attached style geometry was not
        // present at capture, has no baseline entry, and is left alone.
        using FaceBaseline = std::unordered_map<std::string, std::vector<DynVertex>>;
        std::unordered_map<RE::FormID, FaceBaseline> g_faceSnapshot;

        // ⚠ THE FRILL AND THE NECK SEAM ARE ONE FAULT, AND THIS IS WHERE IT
        // LIVES. The frill is a flared collar spreading straight out of the
        // neck join, which is exactly what 298 neck-ring vertices marching
        // 10.4 units outward looks like. It was read as a separate, much
        // larger artefact for one round because it was being sized against the
        // actor's BOW, which happens to arc past her head in the same frame.
        //
        // So there is one accumulator. Restoring dynamicData reads back clean
        // every single publish and changes nothing on screen, because
        // dynamicData is the morphed OUTPUT. UpdateNeck compounds into the
        // base vertex data the engine hangs off each geometry as extra data
        // 'FOD', and the morph pipeline regenerates the output from it, which
        // overwrites our restore before the frame draws.
        //
        // The FOD layout, its runtime gate and the derivation story live in
        // FaceGenFod.h now, shared with SculptProbe.
        using OS::FaceGen::FodView;
        using OS::FaceGen::ResolveFod;
        using OS::FaceGen::kFodFloatsPerVertex;

        using FodBaseline = std::unordered_map<std::string, std::vector<float>>;
        std::unordered_map<RE::FormID, FodBaseline> g_fodSnapshot;

        // ⚠ THE ACCUMULATOR IS NOT dynamicData, AND THIS IS THE HUNT FOR WHERE
        // IT IS. Step 1 measured a deformation climbing by exactly 0.5780 per
        // publish on 298 of her head's vertices, never returning, while the
        // buffer we restore reads back AT baseline every single time. A restore
        // that provably lands and still leaves the next cycle worse means
        // UpdateNeck recomputes that buffer from a source carrying the running
        // total, and the source is not the buffer.
        //
        // For a facegen head the candidate is the base vertex data the engine
        // hangs off the scenegraph as extra data. CommonLib carries
        // NiRTTI_BSFaceGenBaseMorphExtraData but no class for it, so nothing
        // can be read out of one until its layout is derived - and no layout
        // should be derived until we know the block is there at all and what it
        // hangs off.
        //
        // ⚠ NAMES ONLY. Nothing here dereferences past NiExtraData's own 0x18
        // bytes, because the derived size is exactly what is not known yet.
        // Once per actor, not per publish.
        void DumpExtraBlocks(const RE::NiObjectNET* a_obj, RE::FormID a_id, const char* a_what,
                             std::uint16_t a_verts) {
            if (!a_obj) {
                return;
            }
            const auto n = a_obj->GetExtraDataSize();
            spdlog::info("[hair/xdata] actor {:08X} {} '{}' verts={} blocks={}", a_id, a_what,
                         a_obj->name.empty() ? "(unnamed)" : a_obj->name.c_str(), a_verts, n);
            for (std::uint16_t i = 0; i < n; ++i) {
                const auto* const xd = a_obj->GetExtraDataAt(i);
                if (!xd) {
                    continue;
                }
                const auto* const rtti = xd->GetRTTI();
                spdlog::info("[hair/xdata]   [{}] class='{}' key='{}'", i,
                             rtti && rtti->GetName() ? rtti->GetName() : "(no rtti)",
                             xd->name.empty() ? "(unnamed)" : xd->name.c_str());
            }
        }

        void DumpFaceExtraData(const Head& a_head, RE::FormID a_id) {
            if (!a_head.faceNode) {
                return;
            }
            // The node first: the block may hang off the face node rather than
            // off any one geometry, and assuming which would be a guess.
            DumpExtraBlocks(a_head.faceNode, a_id, "faceNode", 0);
            RE::BSVisit::TraverseScenegraphGeometries(
                a_head.faceNode, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    std::uint16_t verts = 0;
                    if (a_geom->GetType().get() == RE::BSGeometry::Type::kDynamicTriShape) {
                        verts = static_cast<RE::BSDynamicTriShape*>(a_geom)
                                    ->GetTrishapeRuntimeData()
                                    .vertexCount;
                    }
                    DumpExtraBlocks(a_geom, a_id, "geom", verts);
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
        }

        void EnsureFaceSnapshot(const Head& a_head, RE::FormID a_id) {
            {
                std::scoped_lock l(g_lock);
                if (g_faceSnapshot.contains(a_id)) {
                    return;
                }
            }
            if (!a_head.faceNode) {
                return;
            }
            FaceBaseline baseline;
            FodBaseline  fodBase;
            RE::BSVisit::TraverseScenegraphGeometries(
                a_head.faceNode, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    if (a_geom->GetType().get() != RE::BSGeometry::Type::kDynamicTriShape) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    auto* const ds = static_cast<RE::BSDynamicTriShape*>(a_geom);
                    const auto& dd = ds->GetDynamicTrishapeRuntimeData();
                    const auto  n  = ds->GetTrishapeRuntimeData().vertexCount;
                    if (!dd.dynamicData || n == 0 || a_geom->name.empty()) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const auto* const v = static_cast<const DynVertex*>(dd.dynamicData);
                    baseline.emplace(a_geom->name.c_str(),
                                     std::vector<DynVertex>(v, v + n));
                    if (const auto fod = ResolveFod(a_geom, n)) {
                        const std::size_t floats = fod.count * kFodFloatsPerVertex;
                        fodBase.emplace(a_geom->name.c_str(),
                                        std::vector<float>(fod.verts, fod.verts + floats));
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            if (baseline.empty()) {
                return;
            }
            std::size_t total = 0;
            for (const auto& [name, verts] : baseline) {
                total += verts.size();
            }
            std::scoped_lock l(g_lock);
            const auto       fodGeoms = fodBase.size();
            g_faceSnapshot.emplace(a_id, std::move(baseline));
            g_fodSnapshot.emplace(a_id, std::move(fodBase));
            spdlog::info("NpcHair: captured actor {:08X} face baseline, {} geometries, "
                         "{} vertices; FOD baseline on {} of them.",
                         a_id, g_faceSnapshot[a_id].size(), total, fodGeoms);
            if (fodGeoms == 0) {
                spdlog::warn("NpcHair: no FOD block passed the layout gate on actor {:08X}. "
                             "The base-morph offsets are wrong for this runtime and the "
                             "restore below will do nothing; derive them before trusting "
                             "any reading from it.",
                             a_id);
            }
            DumpFaceExtraData(a_head, a_id);
        }

        // ⚠ THE NECK SEAM FIX, and the fault it undoes is CUMULATIVE
        // DEFORMATION rather than anything about shading. Measured on Jenassa
        // over 25 publishes: her crown never moves (maxZ pinned at 11.1497 to
        // four decimals) while the neck ring marches +3.19 in Y and never
        // returns, roughly 0.13 per publish. Pinned at the top, pulled apart at
        // the bottom, which is a neck seam that grows every time you touch her
        // hair, exactly as reported.
        //
        // UpdateNeck is what writes those vertices, and it is NOT idempotent.
        // The engine calls it once, on a head freshly built from facegen; we
        // call it on a head that already carries the blend, so each pass
        // re-blends an already-blended neck and the error compounds.
        //
        // So the neck band is restored from the baseline after every publish.
        // The band is the bottom slice by HEIGHT, the same way NPC Visual
        // Editor bands its neck patch, because the seam lives at the join and
        // nothing above it should be pinned: expression morphs and anything
        // else that legitimately moves her face stay free.
        //
        // ⚠ Restoring rather than skipping the call is deliberate. Step 3's
        // UpdateNeck is field-proven necessary for the attach to render at all
        // (three runs without it produced nothing), so the answer cannot be to
        // stop calling it. Pin the result instead.
        // a_full restores the WHOLE head rather than the neck band. For a
        // DECLINED style that is the correct answer and the banded restore is
        // not: nothing of a declined style ever renders, so it must leave the
        // head bit-identical to before it was tried. Measuring a style costs an
        // attach (the bone count only exists on a loaded mesh), but it must not
        // cost a deformation. Browsing twenty SMP styles that all decline was
        // otherwise twenty full deformation cycles for no rendered result.
        // ⚠ ONE RULE, AND IT IS SIMPLER THAN THE TWO IT REPLACES: her own face
        // geometry is INVARIANT under a hair change. Changing what is on her head
        // has no business moving her head, her hair, her brows, her eyes or her
        // mouth by a single vertex, so all of them are pinned to the baseline
        // after every publish.
        //
        // This started as a banded restore on the head alone, preserving a slice
        // by height and blending above it, because the fault looked like a neck
        // seam. That worked on the neck (56 publishes with every value identical
        // to four decimals) and left a second growing artefact untouched, because
        // the band protected one geometry out of six. A whole-buffer restore of
        // everything that is hers is both simpler and complete, and there is no
        // legitimate deformation to preserve: nothing about her face should
        // respond to a hairstyle at all.
        //
        // Attached style geometry has no baseline entry and is deliberately left
        // alone: it is supposed to change, that is the feature.
        void RestoreFace(const Head& a_head, RE::FormID a_id) {
            if (!a_head.faceNode) {
                return;
            }
            FaceBaseline baseline;
            {
                std::scoped_lock l(g_lock);
                const auto       it = g_faceSnapshot.find(a_id);
                if (it == g_faceSnapshot.end()) {
                    return;
                }
                baseline = it->second;
            }
            RE::BSVisit::TraverseScenegraphGeometries(
                a_head.faceNode, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    if (a_geom->GetType().get() != RE::BSGeometry::Type::kDynamicTriShape ||
                        a_geom->name.empty()) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const auto it = baseline.find(a_geom->name.c_str());
                    if (it == baseline.end()) {
                        return RE::BSVisit::BSVisitControl::kContinue;  // not hers
                    }
                    auto* const ds = static_cast<RE::BSDynamicTriShape*>(a_geom);
                    const auto& dd = ds->GetDynamicTrishapeRuntimeData();
                    const auto  n  = ds->GetTrishapeRuntimeData().vertexCount;
                    // A size mismatch means this is not the mesh we measured, so
                    // writing the baseline into it would be a buffer overrun.
                    if (!dd.dynamicData || n != it->second.size()) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    std::copy(it->second.begin(), it->second.end(),
                              static_cast<DynVertex*>(dd.dynamicData));
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
        }

        // ⚠ WHAT THE NECK CENTROID ON ITS OWN COULD NOT TELL YOU. It is one
        // slice of one geometry, chosen when the fault was believed to be a
        // neck seam, and it is blind to the other five geometries the restore
        // now covers. This is the direct question instead: how far is ANY
        // vertex of hers from where it was before this module first touched
        // her, and on which geometry.
        //
        // Read against a baseline rather than against the previous reading, so
        // a single line is interpretable without diffing the series.
        struct FaceDrift {
            float       maxDelta = 0.0f;
            std::string geom;
            std::size_t moved = 0;  // vertices further than 0.0001 from baseline
        };

        FaceDrift MeasureFaceDrift(const Head& a_head, RE::FormID a_id) {
            FaceDrift drift;
            if (!a_head.faceNode) {
                return drift;
            }
            FaceBaseline baseline;
            {
                std::scoped_lock l(g_lock);
                const auto       it = g_faceSnapshot.find(a_id);
                if (it == g_faceSnapshot.end()) {
                    return drift;
                }
                baseline = it->second;
            }
            RE::BSVisit::TraverseScenegraphGeometries(
                a_head.faceNode, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    if (a_geom->GetType().get() != RE::BSGeometry::Type::kDynamicTriShape ||
                        a_geom->name.empty()) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const auto it = baseline.find(a_geom->name.c_str());
                    if (it == baseline.end()) {
                        return RE::BSVisit::BSVisitControl::kContinue;  // not hers
                    }
                    auto* const ds = static_cast<RE::BSDynamicTriShape*>(a_geom);
                    const auto& dd = ds->GetDynamicTrishapeRuntimeData();
                    const auto  n  = ds->GetTrishapeRuntimeData().vertexCount;
                    // Same guard RestoreFace uses, and the same meaning: a
                    // mismatch here is a geometry the restore SKIPS, so it must
                    // not be reported as pinned either.
                    if (!dd.dynamicData || n != it->second.size()) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const auto* const v = static_cast<const DynVertex*>(dd.dynamicData);
                    for (std::uint16_t i = 0; i < n; ++i) {
                        const float dx = v[i].x - it->second[i].x;
                        const float dy = v[i].y - it->second[i].y;
                        const float dz = v[i].z - it->second[i].z;
                        const float d  = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (d > 0.0001f) {
                            ++drift.moved;
                        }
                        if (d > drift.maxDelta) {
                            drift.maxDelta = d;
                            drift.geom     = a_geom->name.c_str();
                        }
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            return drift;
        }

        // ⚠ THIS PROBE WAS CIRCULAR AND EVERY READING IT PRODUCED SINCE THE PIN
        // WENT IN SAYS NOTHING. It ran only AFTER RestoreFace, so it read back
        // the buffer that function had just written from the baseline. Frozen
        // to four decimals was the only answer it could ever give, and it was
        // being read as evidence that deformation had stopped. Twenty-nine
        // consecutive identical lines meant our own memcpy landed, nothing more.
        //
        // It now runs on BOTH sides of the restore and carries a_phase to say
        // which. Read the pair, not either line:
        //   pre drifts, post pinned -> deformation is still happening every
        //     publish and the restore does undo it CPU-side. If the artefact is
        //     still visible after that, the renderer is not seeing our write.
        //   pre pinned, post pinned -> deformation genuinely stopped. The
        //     artefact is then not her geometry at all and measuring her face
        //     further is wasted.
        //   post NOT pinned -> the restore itself is skipping, most likely on a
        //     vertex-count mismatch, and drift= names the geometry.
        // The source, not the output. RestoreFace pins what the morph pipeline
        // writes; this pins what it writes FROM, which is the buffer that has
        // been carrying the running total all along.
        void RestoreFod(const Head& a_head, RE::FormID a_id) {
            if (!a_head.faceNode) {
                return;
            }
            FodBaseline baseline;
            {
                std::scoped_lock l(g_lock);
                const auto       it = g_fodSnapshot.find(a_id);
                if (it == g_fodSnapshot.end()) {
                    return;
                }
                baseline = it->second;
            }
            if (baseline.empty()) {
                return;
            }
            RE::BSVisit::TraverseScenegraphGeometries(
                a_head.faceNode, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    if (a_geom->GetType().get() != RE::BSGeometry::Type::kDynamicTriShape ||
                        a_geom->name.empty()) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const auto it = baseline.find(a_geom->name.c_str());
                    if (it == baseline.end()) {
                        return RE::BSVisit::BSVisitControl::kContinue;  // not hers
                    }
                    const auto n = static_cast<RE::BSDynamicTriShape*>(a_geom)
                                       ->GetTrishapeRuntimeData()
                                       .vertexCount;
                    const auto fod = ResolveFod(a_geom, n);
                    if (!fod || fod.count * kFodFloatsPerVertex != it->second.size()) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    std::copy(it->second.begin(), it->second.end(), fod.verts);
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
        }

        // Same shape as FaceDrift and read the same way, but against the base
        // buffer. This is the number that should have been growing all along
        // while the dynamicData post reading sat at zero.
        FaceDrift MeasureFodDrift(const Head& a_head, RE::FormID a_id) {
            FaceDrift drift;
            if (!a_head.faceNode) {
                return drift;
            }
            FodBaseline baseline;
            {
                std::scoped_lock l(g_lock);
                const auto       it = g_fodSnapshot.find(a_id);
                if (it == g_fodSnapshot.end()) {
                    return drift;
                }
                baseline = it->second;
            }
            RE::BSVisit::TraverseScenegraphGeometries(
                a_head.faceNode, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    if (a_geom->GetType().get() != RE::BSGeometry::Type::kDynamicTriShape ||
                        a_geom->name.empty()) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const auto it = baseline.find(a_geom->name.c_str());
                    if (it == baseline.end()) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const auto n = static_cast<RE::BSDynamicTriShape*>(a_geom)
                                       ->GetTrishapeRuntimeData()
                                       .vertexCount;
                    const auto fod = ResolveFod(a_geom, n);
                    if (!fod || fod.count * kFodFloatsPerVertex != it->second.size()) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    for (std::uint32_t i = 0; i < fod.count; ++i) {
                        const std::size_t o  = static_cast<std::size_t>(i) * kFodFloatsPerVertex;
                        const float       dx = fod.verts[o + 0] - it->second[o + 0];
                        const float       dy = fod.verts[o + 1] - it->second[o + 1];
                        const float       dz = fod.verts[o + 2] - it->second[o + 2];
                        const float       d  = std::sqrt(dx * dx + dy * dy + dz * dz);
                        if (d > 0.0001f) {
                            ++drift.moved;
                        }
                        if (d > drift.maxDelta) {
                            drift.maxDelta = d;
                            drift.geom     = a_geom->name.c_str();
                        }
                    }
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            return drift;
        }

        void LogNeckRing(const Head& a_head, RE::FormID a_id, const char* a_phase) {
            auto* const ds = FindHeadDynShape(a_head.faceNode);
            if (!ds) {
                return;
            }
            const auto& dd = ds->GetDynamicTrishapeRuntimeData();
            const auto  n  = ds->GetTrishapeRuntimeData().vertexCount;
            if (!dd.dynamicData || n == 0) {
                return;
            }
            const auto* const v = static_cast<const DynVertex*>(dd.dynamicData);
            float             minZ = v[0].z, maxZ = v[0].z;
            for (std::uint16_t i = 1; i < n; ++i) {
                minZ = v[i].z < minZ ? v[i].z : minZ;
                maxZ = v[i].z > maxZ ? v[i].z : maxZ;
            }
            const float band = minZ + std::min((maxZ - minZ) * 0.20f, 4.5f);
            double      sx = 0.0, sy = 0.0, sz = 0.0;
            std::size_t cnt = 0;
            for (std::uint16_t i = 0; i < n; ++i) {
                if (v[i].z <= band) {
                    sx += v[i].x;
                    sy += v[i].y;
                    sz += v[i].z;
                    ++cnt;
                }
            }
            if (cnt == 0) {
                return;
            }
            const auto drift = MeasureFaceDrift(a_head, a_id);
            const auto fod   = MeasureFodDrift(a_head, a_id);
            spdlog::info("[hair/neck] actor {:08X} publish#{} {} head='{}' verts={} "
                         "minZ={:.4f} maxZ={:.4f} neckVerts={} "
                         "neckCentroid=({:.4f},{:.4f},{:.4f}) "
                         "drift={:.4f} on '{}' moved={} "
                         "fod={:.4f} on '{}' fodMoved={}",
                         a_id, g_publishes[a_id], a_phase, ds->name.c_str(), n, minZ, maxZ,
                         cnt, sx / cnt, sy / cnt, sz / cnt, drift.maxDelta,
                         drift.geom.empty() ? "-" : drift.geom.c_str(), drift.moved,
                         fod.maxDelta, fod.geom.empty() ? "-" : fod.geom.c_str(), fod.moved);
        }

        // ⚠ TWO OF THE THREE CALLS NPC VISUAL EDITOR MAKES AND WE NEVER DID.
        // It follows every vertex copy with UpdateWorldData, its own
        // UpdateFaceNodeBounds, and QueueUpdateNiObject. The theory is that a
        // restore the renderer is never told about leaves the deformed buffer
        // on screen, which is what "the numbers say frozen and my eyes say it
        // is still growing" would look like.
        //
        // ⚠ THE THIRD CALL IS NOT HERE AND WAS NOT GUESSED. QueueUpdateNiObject
        // is not reachable from this CommonLib: TaskQueueInterface binds vfuncs
        // 45, 64 and 93 and nothing else, and there is no BSTaskPool binding at
        // all, so it needs an Address Library id derived from the binary first.
        // Inventing a signature for it is exactly how UpdateNeck spent four
        // rounds masquerading as a "base morph".
        //
        // UpdateFaceNodeBounds is NVE's own helper rather than an engine
        // export, so the name is theirs and this body is ours. Recomputing each
        // model bound from the vertices just written, and letting the world
        // bound follow, is the only thing the name can reasonably mean.
        void PushToRenderer(const Head& a_head, RE::FormID a_id) {
            if (!a_head.faceNode) {
                return;
            }
            // OS-99: copy her baseline out under lock once, the same
            // pattern RestoreFace uses against this same per-actor map,
            // rather than re-taking g_lock inside the traversal below for
            // every geometry visited. No entry for a_id (snapshot not
            // captured yet) leaves baseline empty, which is correct here:
            // UpdateWorldData still runs, and the traversal below then
            // recomputes nothing, since nothing can be confirmed hers.
            FaceBaseline baseline;
            {
                std::scoped_lock l(g_lock);
                const auto       it = g_faceSnapshot.find(a_id);
                if (it != g_faceSnapshot.end()) {
                    baseline = it->second;
                }
            }
            RE::NiUpdateData data{};
            data.time  = a_head.faceNode->GetRuntimeData().lastTime;
            data.flags = RE::NiUpdateData::Flag::kNone;
            a_head.faceNode->UpdateWorldData(&data);

            RE::BSVisit::TraverseScenegraphGeometries(
                a_head.faceNode,
                [&baseline](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    if (a_geom->GetType().get() != RE::BSGeometry::Type::kDynamicTriShape) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    // OS-99: only recompute bounds for HER geometry, looked
                    // up the same way RestoreFace looks it up. An attached
                    // SMP mesh has no baseline entry - its bounds come from
                    // skinning, and a CPU-side recompute from dynamicData
                    // would fight the physics-driven pose.
                    if (baseline.find(a_geom->name.c_str()) == baseline.end()) {
                        return RE::BSVisit::BSVisitControl::kContinue;  // not hers
                    }
                    auto* const ds = static_cast<RE::BSDynamicTriShape*>(a_geom);
                    const auto& dd = ds->GetDynamicTrishapeRuntimeData();
                    const auto  n  = ds->GetTrishapeRuntimeData().vertexCount;
                    if (!dd.dynamicData || n == 0) {
                        return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    const auto* const v = static_cast<const DynVertex*>(dd.dynamicData);
                    // Centred on the midpoint of the extent rather than on the
                    // mean of the vertices. A bound has to CONTAIN the mesh,
                    // and a centroid is pulled toward whichever end is densely
                    // tessellated, which on a head is the face.
                    float loX = v[0].x, hiX = v[0].x;
                    float loY = v[0].y, hiY = v[0].y;
                    float loZ = v[0].z, hiZ = v[0].z;
                    for (std::uint16_t i = 1; i < n; ++i) {
                        loX = std::min(loX, v[i].x);
                        hiX = std::max(hiX, v[i].x);
                        loY = std::min(loY, v[i].y);
                        hiY = std::max(hiY, v[i].y);
                        loZ = std::min(loZ, v[i].z);
                        hiZ = std::max(hiZ, v[i].z);
                    }
                    const RE::NiPoint3 c{ (loX + hiX) * 0.5f, (loY + hiY) * 0.5f,
                                          (loZ + hiZ) * 0.5f };
                    float              r2 = 0.0f;
                    for (std::uint16_t i = 0; i < n; ++i) {
                        const float dx = v[i].x - c.x;
                        const float dy = v[i].y - c.y;
                        const float dz = v[i].z - c.z;
                        const float d2 = dx * dx + dy * dy + dz * dz;
                        r2             = d2 > r2 ? d2 : r2;
                    }
                    auto& md             = ds->GetModelData();
                    md.modelBound.center = c;
                    md.modelBound.radius = std::sqrt(r2);
                    ds->UpdateWorldBound();
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
        }

        // ⚠ WRITTEN TO FIND A FRILL OUTSIDE THE FACE NODE, AND IT IS NOT OUT
        // THERE. The frill is a flared collar spreading out of the neck join,
        // which is her own head geometry after all. This ran for one round on
        // the mistaken reading that the artefact was several head-widths wide,
        // which came from sizing it against the actor's BOW arcing past her
        // head in the same frame.
        //
        // Kept, unwired, because "walk the actor rather than the face node" is
        // the one angle four rounds of probes never took, and the next artefact
        // that is genuinely not on the face node will want it. Call it from
        // PublishHead to arm it.
        //
        // ⚠ A BIG RADIUS ALONE IS NOT A FAULT. Long hair skinned down the spine
        // is honestly large, and Galactic and CroftBand were both misread that
        // way. This list NAMES what is out there. It does not judge it.
        [[maybe_unused]] void ScanActorGeometry(RE::NiNode* a_root, RE::FormID a_id) {
            if (!a_root) {
                return;
            }
            struct Entry {
                float       radius = 0.0f;
                std::string name;
                std::string parent;
                float       cx = 0.0f, cy = 0.0f, cz = 0.0f;
            };
            std::vector<Entry> all;
            RE::BSVisit::TraverseScenegraphGeometries(
                a_root, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                    const auto& b = a_geom->worldBound;
                    Entry       e;
                    e.radius = b.radius;
                    e.name   = a_geom->name.empty() ? "(unnamed)" : a_geom->name.c_str();
                    e.parent = (a_geom->parent && !a_geom->parent->name.empty())
                                   ? a_geom->parent->name.c_str()
                                   : "(no parent)";
                    e.cx     = b.center.x;
                    e.cy     = b.center.y;
                    e.cz     = b.center.z;
                    all.push_back(std::move(e));
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            if (all.empty()) {
                return;
            }
            const std::size_t top = all.size() < 5 ? all.size() : 5;
            std::partial_sort(all.begin(), all.begin() + top, all.end(),
                              [](const Entry& a, const Entry& b) { return a.radius > b.radius; });
            for (std::size_t i = 0; i < top; ++i) {
                spdlog::info("[hair/scene] actor {:08X} publish#{} #{} r={:.1f} '{}' under '{}' "
                             "at ({:.1f},{:.1f},{:.1f})",
                             a_id, g_publishes[a_id], i, all[i].radius, all[i].name,
                             all[i].parent, all[i].cx, all[i].cy, all[i].cz);
            }
        }

        // Steps 6 and 7, shared by Apply and Restore because BOTH change what
        // is on the head and both need it published. Restore skipping this was
        // a real bug in the probe: un-hiding her own hair left it at whatever
        // transform the last publish gave it.
        void PublishHead(const Head& a_head, RE::FormID a_id) {
            // Counted at the top so one publish carries ONE number across all
            // four lines it can emit. It used to be bumped mid-way, which put
            // the two dumps of a single publish under different numbers.
            ++g_publishes[a_id];
            DumpFace(a_head, a_id, "before");
            a_head.faceNode->FixSkinInstances(a_head.skeleton, false);
            RE::NiUpdateData data{};
            data.time  = 0.0f;
            data.flags = RE::NiUpdateData::Flag::kNone;
            a_head.faceNode->UpdateDownwardPass(data, 0);
            // ⚠ RE-SETTLE THE NECK, AND IT HAS TO BE AFTER THE REBIND. This is
            // the neck seam, and the bug was an ORDERING one rather than a
            // missing call: the sequence already ran UpdateNeck at step 3, but
            // FixSkinInstances above then rebinds EVERY geometry under the face
            // node, her head included, which undoes the blend it had just
            // settled. Nothing put it back, so the seam appeared on the first
            // hair change and every later publish disturbed it again.
            //
            // Ordering confirmed against a second implementation rather than
            // guessed: NPC Visual Editor (github.com/vinymayan/NPC-Visual) does
            // runtime face swaps and calls UpdateNeck AFTER FixSkinInstances in
            // both of its apply paths.
            //
            // Inside PublishHead rather than at the call site, so a restore and
            // a declined style's back-out re-settle too - both of those also
            // rebind, so both can leave the seam.
            //
            // ⚠ AND THEN PIN THE NECK BACK. See RestoreNeckBand: UpdateNeck is
            // not idempotent, so every publish drags the neck ring further from
            // where it started. Restoring the band from the baseline is what
            // stops the seam growing.
            //
            // ⚠ A SECOND UpdateNeck CALL USED TO SIT HERE and it was mine, added
            // on the theory that the seam was an ORDERING fault because NPC
            // Visual Editor calls it after its rebind. That was wrong: it does
            // not matter when you call it, it matters how OFTEN, and the extra
            // call doubled the drift rate. Removed. Do not re-add it.
            // ⚠ READ BEFORE THE RESTORE AS WELL AS AFTER. This line is the only
            // one of the pair that can see deformation; the post line reads a
            // buffer RestoreFace has just overwritten and can only confirm the
            // write landed. See LogNeckRing for how to read the pair.
            LogNeckRing(a_head, a_id, "pre ");
            // ⚠ SOURCE FIRST, THEN OUTPUT. RestoreFod pins the base vertex data
            // UpdateNeck compounds into; RestoreFace pins what the morph
            // pipeline currently has on screen. Doing only the second is what
            // produced twenty-five publishes of a perfect zero reading and a
            // frill that kept growing anyway.
            RestoreFod(a_head, a_id);
            RestoreFace(a_head, a_id);
            PushToRenderer(a_head, a_id);
            DumpFace(a_head, a_id, "after ");
            LogNeckRing(a_head, a_id, "post");

            // ⚠ PAINT THE COLOUR ONTO THE HAIR WE JUST ATTACHED. OS-100, and it
            // is a gap the split between the two mechanisms opened rather than
            // anything subtle. The PLAYER's style path is a base write plus
            // the engine's per-part swap (a full DoReset3D until OS-230), and
            // HeadPart's QueueRebuildAndRepaint calls HairColor::Repaint on
            // the far side of that precisely because the engine's painter
            // resets the tint on what it builds. A follower's path
            // deliberately never rebuilds (a rebuild hands her a human
            // complexion, which no repaint can undo), so it correctly skipped
            // the rebuild - and silently skipped the repaint with it. The
            // repaint was never the rebuild's companion, though. It is the
            // ATTACH's: the geometry hung on her head here is a fresh clone
            // carrying whatever tint its source nif shipped with, and nothing
            // else in this module writes a hair-tint material.
            //
            // Field-reported 2026-08-02: hovering a hair in the browser and
            // switching to an outfit with an SMP hair both left the stock
            // colour, while CLICKING a hair worked - because a click commits
            // the outfit and ends in RefreshActor, which has always called
            // Repaint (REAugments.cpp). Re-entering the hex worked for the same
            // reason, one level nearer. Two symptoms, one missing call.
            //
            // In PublishHead rather than at the three call sites, for the same
            // reason UpdateNeck sits here: a restore and a declined style's
            // back-out both re-attach geometry too, and both would otherwise
            // publish an untinted head.
            //
            // Unconditional, matching REAugments' call and for its stated
            // reason: Repaint is one scenegraph walk, a no-op for an actor with
            // no hair-tint material, and it falls back to the colour form when
            // anything outside Fitting Room owns the base. Its geometry-only
            // branch (no HeadRelatedData, which HirelingJenassa is) is the case
            // this fix is reported against, and it is already handled there.
            //
            // Inline, NOT queued: PublishHead only ever runs inside
            // OnGameThread's task, which is the thread Repaint's scenegraph
            // walk requires. Queuing again would land it a drain later, after
            // the frame that showed the wrong colour.
            if (auto* const form = RE::TESForm::LookupByID(a_id)) {
                if (auto* const actor = form->As<RE::Actor>()) {
                    HairColor::Repaint(actor);
                }
            }
        }

        // ---- OS-99 tripwire v2: the deferred SMP health check --------------
        //
        // ⚠ THE SIGNATURE (round 2 revision): worst world-bound radius among
        // the recorded geometries, NOTHING ELSE. Round 1 shipped a
        // containment test on top (bound must reach the skeleton root) and
        // round 2 killed it: 'KSSMP_Sky6063' logged "clean r=12945.9" while
        // Steammist tripped at r=12974, identical dangle-scale radii split
        // only by center-distance vs radius landing either side of equal
        // within float noise. Radius alone has no borderline here: at the
        // deferred moment, settled styles read r=13-16 and dangles read
        // 12,9xx, three orders of magnitude apart. 150 keeps roughly an
        // order of magnitude of headroom in both directions. The old
        // "honest long hair is large" caution (Galactic, CroftBand) does
        // not collide with radius-only here: those readings were same-pass
        // pre-physics bounds on ENGINE-path styles, which never reach this
        // check, while the deferred check only ever measures FSMP-admitted
        // styles after settle time, where round 2 measured 13-16.
        constexpr float kSmpBrokenMinRadius = 150.0f;

        // Task 6.7: how many looks a style gets before an over-threshold
        // reading becomes a back-out. Round 2's new leading hypothesis is
        // TIME-TO-SETTLE: strand bodies spawn near the world origin and the
        // constraint solver pulls them to her head over seconds, so a
        // single +0.7s look reads mid-flight and backs out styles that
        // would have settled. Three looks about 0.7s apart puts the worst
        // case verdict near +2.1s, still inside a browse's attention span.
        constexpr std::uint32_t kSmpSettleLooks = 3;

        // ⚠ Looks must be SPACED to mean anything. ApplyNow's entry look and
        // the RestoreNow it calls internally would otherwise take two looks
        // milliseconds apart, and a final look that granted no settle time
        // can mis-decline a style mid-swap. A look younger than this since
        // the last (re-)arm is skipped: the entry stays for the tick's next
        // pass, or for the teardown erase when a swap is taking the style
        // off anyway.
        constexpr auto kSmpLookMinSpacing = std::chrono::milliseconds(250);

        // The tick's re-fire age, shared with the look gate below so the
        // FINAL look is held to it too: past the frames the world needs to
        // step a fresh system at least once, far under the seconds a user
        // takes to read a stretch as broken.
        constexpr auto kSmpTickDwell = std::chrono::milliseconds(700);

        // Runs first thing in ApplyNow and RestoreNow, the only two entries
        // that mutate the head, and via TickPendingSmpChecks' aged one-shot
        // (Task 6.6), which passes the entry's writtenAt as a_onlyIfWrittenAt
        // so a tick queued against an old arm can never act on a NEWER one;
        // the entry callers pass nothing and take whatever is there. Since
        // Task 6.7 a call is a LOOK, not necessarily a verdict: a mid-settle
        // look re-arms the entry (fresh writtenAt, looks+1) instead of
        // consuming it, so the entry-erase duty moved to the teardown sites
        // (RestoreNow's take, ReassertNow's drop, Clear) and this function
        // only erases on its own verdicts. A back-out here is an SMP
        // teardown site, so it fires the reclaim nudge itself; callers need
        // nothing back.
        void RunDeferredSmpCheck(
            RE::Actor* a_actor, Kind a_kind,
            std::chrono::steady_clock::time_point a_onlyIfWrittenAt = {}) {
            if (!a_actor) {
                return;
            }
            const Slot slot = SlotKey(a_actor->GetFormID(), a_kind);
            // Look phase 1: snapshot WITHOUT consuming. Whether this look
            // ends the entry is decided by the reading, below.
            SmpPendingCheck check;
            {
                std::scoped_lock l(g_lock);
                const auto       it = g_smpPendingCheck.find(slot);
                if (it == g_smpPendingCheck.end()) {
                    return;
                }
                if (a_onlyIfWrittenAt != std::chrono::steady_clock::time_point{} &&
                    it->second.writtenAt != a_onlyIfWrittenAt) {
                    return;  // a newer arm owns the entry; not ours
                }
                check = it->second;
            }
            // A look milliseconds after the last (re-)arm reads the same
            // frame and proves nothing; see kSmpLookMinSpacing. Tick-driven
            // looks are never this young (the tick queues at kSmpTickDwell).
            // ⚠ The FINAL look is held to the FULL dwell, not just the
            // spacing floor: a swap-triggered final look could otherwise
            // arrive 250 ms after look 2's re-arm, handing the decisive
            // reading the LEAST settle time of the series. The decline
            // verdict must never get less settle time than a probe; a young
            // would-be-final look skips exactly like a young probe, and the
            // caller's teardown erase owns the entry from there.
            const auto age = std::chrono::steady_clock::now() - check.writtenAt;
            if (age < kSmpLookMinSpacing ||
                (check.looks + 1 >= kSmpSettleLooks && age < kSmpTickDwell)) {
                return;
            }
            // Consumes the entry only if it is still OUR generation; a
            // concurrent Clear (revert) wins otherwise.
            const auto eraseOurEntry = [&]() {
                std::scoped_lock l(g_lock);
                const auto       it = g_smpPendingCheck.find(slot);
                if (it != g_smpPendingCheck.end() &&
                    it->second.writtenAt == check.writtenAt) {
                    g_smpPendingCheck.erase(it);
                    return true;
                }
                return false;
            };
            const Head head = ResolveHead(a_actor);
            if (!head.Ok()) {
                eraseOurEntry();  // her head is gone; nothing measurable
                return;
            }
            float       worst = 0.0f;
            std::string worstGeom;
            for (const auto& n : check.geomNames) {
                const RE::BSFixedString nm{ n.c_str() };
                auto* const             obj = head.faceNode->GetObjectByName(nm);
                if (!obj) {
                    continue;  // a rebuild took it already; nothing to judge
                }
                RE::BSVisit::TraverseScenegraphGeometries(
                    obj, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                        if (a_geom->worldBound.radius > worst) {
                            worst     = a_geom->worldBound.radius;
                            worstGeom = a_geom->name.c_str();
                        }
                        return RE::BSVisit::BSVisitControl::kContinue;
                    });
            }
            const std::uint32_t lookN = check.looks + 1;
            if (worst <= kSmpBrokenMinRadius) {
                eraseOurEntry();
                spdlog::info("[hair/smp] '{}' deferred check clean r={:.1f} on '{}' (look {})",
                             check.edid, worst,
                             worstGeom.empty() ? "-" : worstGeom.c_str(), lookN);
                return;
            }
            if (lookN < kSmpSettleLooks) {
                // ⚠ THE SETTLE SERIES IS THE EXPERIMENT. Styles that shrink
                // monotonically across looks (12900 -> 4000 -> 200 -> clean)
                // prove the settle-time hypothesis and may argue for a
                // larger looks budget later; styles pinned at 12,9xx across
                // all looks are true dangles and back out honestly on the
                // final look.
                {
                    std::scoped_lock l(g_lock);
                    const auto       it = g_smpPendingCheck.find(slot);
                    if (it != g_smpPendingCheck.end() &&
                        it->second.writtenAt == check.writtenAt) {
                        // Re-arm: the NEW stamp is the generation token any
                        // later queued task must carry, written before this
                        // lock releases; tasks still in flight with the old
                        // stamp no-op at phase 1. queuedTick reopens the
                        // tick's one-shot for the next look ~0.7s out. The
                        // probe line logs only on this success path, so a
                        // Clear-raced look does not log for a dead entry.
                        it->second.looks      = lookN;
                        it->second.writtenAt  = std::chrono::steady_clock::now();
                        it->second.queuedTick = false;
                        spdlog::info("[hair/smp] '{}' settle probe look={} r={:.1f} on '{}'",
                                     check.edid, lookN, worst,
                                     worstGeom.empty() ? "-" : worstGeom.c_str());
                    }
                }
                return;
            }
            // Final look, still over threshold: this verdict consumes the
            // entry. If the generation moved under us (revert), it is not
            // ours to act on.
            if (!eraseOurEntry()) {
                return;
            }
            // The back-out, same order as the admission decline: detach, then
            // unhide hers, then publish so the rebind only sees geometry it
            // can bind, then pin her face back.
            Applied rec;
            {
                std::scoped_lock l(g_lock);
                const auto       it = g_applied.find(slot);
                if (it == g_applied.end() || !it->second.part ||
                    it->second.part->GetFormID() != check.styleFormID) {
                    // The record moved on under us. Should be unreachable:
                    // both entries that change it run this check first.
                    return;
                }
                rec = std::move(it->second);
                g_applied.erase(it);
            }
            DetachGeometryLeaves(head.faceNode, rec.attached);
            const auto t = DetachByNames(head.faceNode, rec.attached);
            for (const auto& hn : rec.hidden) {
                SetHiddenByName(head.faceNode, hn, false);
            }
            PublishHead(head, a_actor->GetFormID());
            RestoreFace(head, a_actor->GetFormID());
            // Task 6.6 review: this back-out is an SMP teardown site, so it
            // nudges here itself. The tick-driven variant of this check has
            // no enclosing ApplyNow to do it for, and the next style must
            // not depend on one existing.
            NudgeFsmpReclaim(head);
            {
                std::scoped_lock l(g_lock);
                // An FSMP-side build failure, not an absent route: FSMP was
                // live and admitted the style, and its bound stayed blown out.
                MarkUnsupportedSessionLocked(rec.part, false);
            }
            const auto totalS =
                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                              check.firstArmedAt)
                    .count();
            spdlog::warn("NpcHair: '{}' was admitted for FSMP but its bound stayed "
                         "blown out at {:.0f} after {} looks over {:.1f}s, which is "
                         "the dangling-strand signature (settled styles read under "
                         "{:.0f}). Backed out (detached {}, stuck {}); declined for "
                         "this session only.",
                         check.edid, worst, lookN, totalS, kSmpBrokenMinRadius,
                         t.detached, t.stuck);
        }

        // ⚠ EVERY MUTATION GOES THROUGH HERE, and the CTD that made it
        // non-negotiable is worth stating. The editor draws on FUCK's PRESENT
        // thread, and the first wiring called straight through from there. It
        // crashed inside hdt::BSFaceGenNiNodeEx::SkinAllGeometry, because
        // HDT-SMP hooks FixSkinInstances, so a render-thread call re-entered
        // SMP's skinning while the game thread was free to be walking the same
        // BSDismemberSkinInstance. The visual artifacting reported alongside it
        // was the same race, seen rather than caught.
        //
        // Queuing INSIDE the module rather than asking callers to marshal is
        // deliberate. The header used to say "nothing here queues on your
        // behalf", and the very first caller forgot. One place that cannot be
        // forgotten beats a rule in a comment.
        void OnGameThread(RE::ActorHandle a_handle, std::function<void(RE::Actor*)> a_work,
                          std::function<void()> a_onDead = {}) {
            auto* const task = SKSE::GetTaskInterface();
            if (!task) {
                if (a_onDead) {
                    a_onDead();  // nothing will ever drain; un-latch now
                }
                return;
            }
            task->AddTask([a_handle, work = std::move(a_work),
                           onDead = std::move(a_onDead)] {
                auto       ptr   = a_handle.get();
                RE::Actor* actor = ptr.get();
                if (!actor) {
                    // Streamed out between the request and the drain. The
                    // optional dead-path callback lets a caller un-latch
                    // whatever it armed for this task (the tick's one-shot
                    // self-heals through it).
                    if (onDead) {
                        onDead();
                    }
                    return;
                }
                try {
                    work(actor);
                } catch (const std::exception& e) {
                    spdlog::error("NpcHair task threw: {}", e.what());
                } catch (...) {
                    spdlog::error("NpcHair task threw a non-standard exception.");
                }
            });
        }

        // Every pending look this actor has, whichever kind armed it. ⚠ BOTH
        // KINDS ON EVERY ENTRY, because a beard's publish is a FixSkinInstances
        // over her whole face node and walks the pending HAIR admit exactly
        // as a hair apply would; the look has to be taken before either kind
        // touches the head.
        void RunDeferredSmpChecks(RE::Actor* a_actor) {
            for (const auto kind : kKinds) {
                RunDeferredSmpCheck(a_actor, kind);
            }
        }

        // (Re)start the settle ladder for this actor. Called with g_lock NOT
        // held. A refresh resets the ladder AND the generation token, so a
        // one-shot queued against the old arm declines to advance the new one.
        void ArmSettle(RE::Actor* a_actor) {
            if (!a_actor) {
                return;
            }
            std::scoped_lock l(g_lock);
            auto& w   = g_settle[a_actor->GetFormID()];
            w.handle  = a_actor->GetHandle();
            w.armedAt = std::chrono::steady_clock::now();
            w.stage   = 0;
            w.queued  = false;
            g_settlePending.store(true, std::memory_order_release);
        }

        // Arm a ladder only where none is running.
        //
        // ⚠⚠ NEVER ArmSettle FROM A PATH THE LADDER ITSELF REACHES. ArmSettle
        // resets stage to 0, and the ladder's own walk calls ReassertNow, so
        // arming from inside that call would push the ladder back to stage 0 on
        // every walk and it would never expire: one wasted scenegraph walk
        // every 750 ms for the rest of the session. This asks first, so an
        // already-armed actor keeps her existing ladder and it still dies after
        // three stages.
        void ArmSettleIfIdle(RE::Actor* a_actor) {
            if (!a_actor) {
                return;
            }
            {
                std::scoped_lock l(g_lock);
                if (g_settle.find(a_actor->GetFormID()) != g_settle.end()) {
                    return;
                }
            }
            ArmSettle(a_actor);
        }

    }  // namespace

    // The real work, always on the game thread. The public entry points below
    // are thin wrappers that queue these.
    static bool ApplyNow(RE::Actor* a_actor, RE::BGSHeadPart* a_part);
    static bool RestoreNow(RE::Actor* a_actor, Kind a_kind);
    static void ReassertNow(RE::Actor* a_actor);

    static bool ApplyNow(RE::Actor* a_actor, RE::BGSHeadPart* a_part) {
        if (!a_part) {
            return false;
        }
        // ⚠ THE KIND COMES OFF THE PART, and a part of a kind this module does
        // not handle stops here rather than culling her hair to attach it.
        const auto kindOpt = KindOfPart(a_part);
        if (!kindOpt) {
            return false;
        }
        const Kind kind = *kindOpt;
        const Slot slot = SlotKey(a_actor->GetFormID(), kind);
        // OS-99 tripwire v2: take a look at the PREVIOUS admit while it is
        // still on her head, before this apply touches anything. A broken
        // final look backs it out and declines it for the session; a
        // mid-settle look re-arms instead, and it is RestoreNow's teardown
        // erase below, not this look, that guarantees the entry is gone
        // before the applied record changes.
        RunDeferredSmpChecks(a_actor);
        const Head head = ResolveHead(a_actor);
        if (!head.Ok()) {
            return false;
        }
        // ⚠ BEFORE ANYTHING TOUCHES HER. Everything below this line deforms the
        // neck a little, so the baseline has to be taken while the head is
        // still exactly as the engine's own build left it. One baseline per
        // ACTOR, not per kind: it is her face as the engine built it, and the
        // second kind to arrive finds it already taken.
        EnsureFaceSnapshot(head, a_actor->GetFormID());
        auto* const current = head.base->GetCurrentHeadPartByType(EngineTypeOf(kind));
        if (current == a_part && !AppliedTo(a_actor, kind)) {
            return false;  // already her own; nothing to do
        }

        // ⚠ SAY WHAT WE ARE ABOUT TO DO, BEFORE DOING IT. Every other line in
        // this module logs on the way out, so an apply that dies inside the
        // publish leaves NO trace of itself and the last line in the log names
        // the PREVIOUS style. Three crash logs were read that way and the wrong
        // conclusion drawn from all three: the styles that had been cycling
        // fine got blamed, and the one that actually killed the process was
        // invisible. The part count is here for the same reason - it is the
        // one number that separated the crashing styles from the safe ones.
        spdlog::info("NpcHair: actor {:08X} applying {} '{}' ({} part(s) incl. extras)...",
                     a_actor->GetFormID(), KindWord(kind),
                     a_part->GetFormEditorID() ? a_part->GetFormEditorID() : "(no edid)",
                     a_part->extraParts.size() + 1);

        // Anything already applied IN THIS KIND comes off first, so switching
        // styles does not leave the previous one attached and hidden under the
        // new one. ⚠ THIS KIND ONLY: a beard going on must not take her hair
        // of ours off, which is the whole reason the captures are keyed by
        // (actor, kind). If the outgoing style was FSMP's, its teardown fires
        // the reclaim nudge itself, inside RestoreNow (this call) or inside
        // the deferred back-out above: every SMP teardown site owns its own
        // nudge (the Task 6.6 review pass), so this path adds nothing on top.
        RestoreNow(a_actor, kind);

        Applied rec;
        rec.part = a_part;
        std::vector<std::string> toHide;
        CollectPartNames(current, toHide);
        std::vector<std::string> toAttach;
        CollectPartNames(a_part, toAttach);

        // 1. CULL FIRST. Attaching over her own hair leaves her wearing both,
        //    which reads as "nothing happened" when it worked perfectly.
        for (const auto& n : toHide) {
            if (SetHiddenByName(head.faceNode, n, true)) {
                rec.hidden.push_back(n);
            }
        }

        const REL::Relocation<AttachHeadPart_t>   attach{ REL::RelocationID(26257, 26836) };
        const REL::Relocation<UpdateFaceModels_t> updateModels{ REL::RelocationID(26458, 27044) };
        const REL::Relocation<ApplyFaceGenData_t> applyData{ REL::RelocationID(26259, 26838) };

        // OS-246: the face node's children BEFORE the attach, by pointer, so
        // the roots the engine is about to plant can be told apart from her
        // own pieces even when the two share a name.
        std::vector<RE::NiAVObject*> before;
        for (auto& child : head.faceNode->GetChildren()) {
            if (auto* const obj = child.get()) {
                before.push_back(obj);
            }
        }

        // 2. ATTACH the part and its extraParts. Hair without its hairline is a
        //    partial head by construction; the engine's list carries both.
        attach(head.mgr, head.faceNode, a_part, head.base, false);
        for (auto* const ep : a_part->extraParts) {
            if (ep) {
                attach(head.mgr, head.faceNode, ep, head.base, false);
            }
        }

        // 3 and 4. BETWEEN the loops, which is where the engine puts them.
        //
        // ⚠ STEP 3 IS NOT A BASE MORPH. It was reverse engineered as one and
        // carried that name for two days, through every neck-seam theory this
        // feature produced. `RELOCATION_ID(24207, 24711)` is CommonLib's
        // `TESNPC::UpdateNeck(BSFaceGenNiNode*)`, which blends the head into
        // the body at the neck. Named for the exact symptom, sitting in our own
        // sequence, and unrecognised because the hand-derived signature took
        // four arguments where the real one takes a face node. The two extra
        // register arguments were harmless, so nothing ever complained.
        //
        // Now called through CommonLib's typed member, so the signature cannot
        // drift again.
        head.base->UpdateNeck(head.faceNode);
        updateModels(head.faceNode);

        // 5. Facegen data, per part, in a second pass over the same set.
        applyData(head.mgr, head.faceNode, a_part, head.base);
        for (auto* const ep : a_part->extraParts) {
            if (ep) {
                applyData(head.mgr, head.faceNode, ep, head.base);
            }
        }

        // OS-246: the fresh roots move into our namespace HERE, after the
        // engine passes that resolve a part by its own name (attach and
        // applyData both take the part, and nothing of the engine's looks our
        // pieces up by name after this), and before every lookup of ours
        // does. From this line on, toAttach IS the renamed set: the measures,
        // the FMD strip, the decline back-out and the record all key on names
        // only our own pieces can wear, and a bare part name can only ever
        // resolve hers.
        toAttach = ClaimFreshRoots(head.faceNode, before, toAttach.size());

        // ⚠ ADMISSION TEST, AND IT HAS TO SIT EXACTLY HERE. After the attach,
        // because the bone count is a property of the loaded mesh and no field
        // on the record predicts it. Before the publish, because the publish is
        // the step that cannot survive the answer being wrong: everything up to
        // here has run against 10, 11 and 17-bone hair without complaint, and
        // it is FixSkinInstances that reads off the end of the override block.
        //
        // Backing out fully rather than publishing a half-bound head is the
        // point. Detach first and publish afterwards, so the rebind only ever
        // sees geometry it can bind - her own, which is what it was bound to
        // before this call started.
        const auto reading = MeasureBinding(head.faceNode, head.skeleton, toAttach);
        // ⚠ SAME SPOT AS MeasureBinding AND FOR THE SAME REASON: the FMD
        // chain MeasureSmp walks is populated by the applyData pass that just
        // ran, synchronously, on this thread. Moving this call earlier reads
        // a chain that does not exist yet; later costs a publish on a style
        // we may be about to refuse.
        const auto smp  = MeasureSmp(head.faceNode, toAttach);
        const auto fsmp = FsmpBridge::IsAvailable();
        const auto routeName =
            FsmpBridge::Route() == FsmpBridge::Coop::kSkinAllEntry    ? "skinall"
            : FsmpBridge::Route() == FsmpBridge::Coop::kSkinSingleEntry ? "skinsingle"
                                                                       : "none";
        const char* const edid =
            a_part->GetFormEditorID() ? a_part->GetFormEditorID() : "(no edid)";
        spdlog::info("[hair/smp] '{}' capable={} fsmp={} route={} fmd={}/{} xml='{}' via '{}'",
                     edid, smp.capable, fsmp, routeName, smp.fmdDirectChildren,
                     smp.fmdGeoms.size(), smp.capable ? smp.xml.c_str() : "-",
                     smp.capable ? smp.geomName.c_str() : "-");
        // OS-105: the demotion, named where it happens. This reads the FMD
        // chain the applyData pass above just planted on a FRESH clone, so a
        // not-capable answer on a style that has already measured capable is a
        // property of THIS attach, not of the mesh.
        bool demoted = false;
        {
            std::scoped_lock l(g_lock);
            if (smp.capable) {
                g_smpMeasuredCapable.insert(a_part->GetFormID());
            } else {
                demoted = g_smpMeasuredCapable.contains(a_part->GetFormID());
            }
        }
        if (demoted) {
            spdlog::warn("[hair/smp] '{}' DEMOTED: it measured capable earlier this session "
                         "and now measures not-capable, so its FMD chain did not resolve on "
                         "this attach. The binding gates judge it as an ordinary head part "
                         "from here, and its strand chains blow the {}-bone ceiling by "
                         "construction, so expect a decline. Not persisted (OS-105).",
                         edid, kMaxHeadPartBones);
        }
        const auto verdict = NpcHairPlan::JudgeStyle(reading.maxBones, reading.unbound,
                                                     reading.partDefect, smp.capable,
                                                     fsmp);
        if (verdict != NpcHairPlan::Admission::kAdmit &&
            verdict != NpcHairPlan::Admission::kAdmitSmp) {
            // OS-106, read before the back-out below because both halves of it
            // are already decided here: a physics mesh that the binding gates
            // refused only because FSMP is not live.
            const bool fsmpAbsentDecline = smp.capable && !fsmp;
            const auto t = DetachByNames(head.faceNode, toAttach);
            for (const auto& n : rec.hidden) {
                SetHiddenByName(head.faceNode, n, false);
            }
            PublishHead(head, a_actor->GetFormID());
            // ⚠ A DECLINED STYLE MUST LEAVE NO TRACE. Everything above already
            // ran the full attach, the neck update and both facegen passes just
            // to reach a verdict of "no", so without this the measurement
            // itself deforms her. Browsing SMP styles is mostly declines, which
            // made every such browse a deformation with nothing to show for it.
            // Full restore rather than the banded one because a style that
            // renders nothing has no claim on any part of her head.
            RestoreFace(head, a_actor->GetFormID());
            {
                std::scoped_lock l(g_lock);
                // ⚠ WHAT THE SIDECAR IS ALLOWED TO REMEMBER. g_unsupported's own
                // rule is that a decline "is a property of the mesh, so once one
                // follower has proved it the answer holds for all of them", and
                // MarkUnsupportedLocked WRITES unsupported-hair.json for good.
                // Two of the ways to reach this branch are not properties of the
                // mesh at all, and both used to be persisted anyway.
                //
                // OS-106: the style measured SMP-capable and FSMP is not live.
                // JudgeStyle then falls to JudgeBinding, whose gates judge the
                // ENGINE's skinning path, and a physics mesh fails those by
                // construction. That verdict is correct (without FSMP the engine
                // path really would stretch the strand bones) and it is entirely
                // contingent on the INSTALL. Persisting it means one browse on a
                // machine with no FSMP, or with an FSMP build FsmpBridge does not
                // row, permanently greys out every SMP style, and installing a
                // supported build afterwards does not bring them back.
                //
                // OS-105: the style measured not-capable after having measured
                // capable earlier this session. The reading is one we have
                // already watched flip, so it is not evidence about the mesh.
                //
                // Both go to the session set, which is exactly what it was built
                // for: "a decline that must not outlive its cause".
                if (demoted || fsmpAbsentDecline) {
                    MarkUnsupportedSessionLocked(a_part, fsmpAbsentDecline);
                } else {
                    MarkUnsupportedLocked(a_part);
                }
                g_pending.erase(slot);  // the decision is off again
            }
            if (fsmpAbsentDecline) {
                spdlog::warn("NpcHair: '{}' is an SMP style and no supported FSMP build is "
                             "live, so the engine path would stretch its strand bones. "
                             "Declined for THIS SESSION ONLY (OS-106) rather than written "
                             "to unsupported-hair.json, because that is a property of the "
                             "install and not of the mesh.",
                             edid);
            }
            // ⚠ THE ROUTE DECISION FOR SMP SUPPORT, and it is one line per
            // geometry on a style we were going to refuse anyway, so it costs
            // nothing to collect. Read the VISIBLE strands, not the
            // 'VirtualHairCollision_*' proxies: if the strands are at or under
            // the ceiling then only the proxies blow it and dropping them is
            // the whole fix. If the strands are over it too, the head-part path
            // is closed to physics hair for good and the wig route is the only
            // one left. See BindingReading::perGeom.
            for (const auto& g : reading.perGeom) {
                spdlog::info("[hair/bind] '{}' geom '{}' bones={} unbound={} ceiling={}", edid,
                             g.name, g.bones, g.unbound, kMaxHeadPartBones);
            }
            // ⚠⚠ THE PARTITION READING PRINTS WHETHER OR NOT IT IS THE VERDICT,
            // and it did not until 2026-08-21. JudgeBinding's severity order
            // names the bone count first, so a style refused BECAUSE of its
            // partition was reported as "needs 22 bones and the engine binds at
            // most 8". The count in that sentence was true and irrelevant, the
            // partition numbers appeared nowhere, and reading the field log back
            // gave a confident wrong answer about which fault was in play. A
            // measurement that only prints when it is the headline cannot be
            // used to check the headline.
            spdlog::info("[hair/part] '{}' partitionDefect={} on '{}' mapLen={} skinBones={}",
                         edid, reading.partDefect,
                         reading.partDefect ? reading.partGeom : "-", reading.partMapLen,
                         reading.partSkinBones);
            // ⚠ SAY WHICH SET THE DECLINE WENT INTO, because these three lines
            // said "marked unsupported" unconditionally while the branch above
            // had already routed an FSMP-absent or demoted decline to the
            // SESSION set. On the day a rig had no rowed FSMP that read as
            // fifteen styles permanently blacklisted, and cost a chunk of a
            // diagnosis session proving that unsupported-hair.json had never
            // been written at all. One phrase, decided where the routing was.
            const char* const scope = (demoted || fsmpAbsentDecline)
                                          ? "declined for THIS SESSION ONLY"
                                          : "declined and marked unsupported for good";
            // Name the fault, not just the refusal. The two look identical from
            // the outside and want different answers from whoever reads this.
            if (verdict == NpcHairPlan::Admission::kTooManyBones) {
                spdlog::warn("NpcHair: '{}' needs {} bones on '{}' and the engine's head-part "
                             "skinning binds at most {}. So it is {} "
                             "(detached {}, stuck {}).",
                             edid, reading.maxBones, reading.worstGeom, kMaxHeadPartBones,
                             scope, t.detached, t.stuck);
            } else if (verdict == NpcHairPlan::Admission::kBadPartition) {
                spdlog::warn("NpcHair: '{}' has a skin partition on '{}' whose bone map "
                             "addresses {} of the skin's {} bone(s). The renderer uploads "
                             "one matrix per map entry, so its vertices sample matrices "
                             "that are never uploaded, which renders as a fan off the "
                             "head. So it is {} (detached {}, stuck {}).",
                             edid, reading.partGeom, reading.partMapLen,
                             reading.partSkinBones, scope, t.detached, t.stuck);
            } else {
                spdlog::warn("NpcHair: '{}' binds {} bone(s) that are not on this actor's "
                             "skeleton, first '{}' on '{}'. FixSkinInstances would leave "
                             "that mesh on the nif's own bones with no world transform, so "
                             "it is {} (detached {}, stuck {}).",
                             edid, reading.unbound, reading.firstUnboundBone,
                             reading.unboundGeom, scope, t.detached, t.stuck);
            }
            return false;
        }

        // OS-99 (Task 6.5): shield the FMD blocks BEFORE the publish. The
        // attach's applyData pass just planted them, and the publish below is
        // the first FixSkinInstances that would walk them; for a kAdmitSmp
        // style that walk is the unpatched >8-bone override read. StripFmd
        // carries the derivation.
        //
        // ⚠ GATED ON kAdmitSmp, AND `8a86bd9` WIDENED IT TO UNCONDITIONAL FOR
        // TWO ROUNDS. That widening is REVERTED here (OS-105) and must not come
        // back on the reasoning it carried, which was wrong in a way the field
        // then disproved. Recorded so it is not re-derived:
        //
        // Its claim was that the shield keys on our ADMISSION DECISION while the
        // hazard is a PROPERTY OF THE GEOMETRY, on this evidence:
        //
        //   03:16:01  'KSSMP_SweetScar' capable=true  xml='...Sweet Scar.xml'
        //   03:16:09  'KSSMP_SweetScar' capable=true
        //   03:16:18  'KSSMP_SweetScar' capable=true
        //   <save load>
        //   03:16:26  'KSSMP_SweetScar' capable=false xml='-'        -> CTD
        //
        // The demotion is real. The mechanism offered for it was that the
        // shield's own strip removes the block the NEXT MeasureSmp reads, and
        // that cannot happen: every MeasureSmp runs a few lines after this
        // function's own `attach` and `applyData` pass, on a FRESH clone that
        // was planted this pass. It never reads a geometry an earlier publish
        // stripped. The field says the same thing twice over. Forty-eight
        // consecutive publishes each stripped and each then measured capable=
        // true; the single demotion followed a LOAD, not a strip. And round 2
        // ran with the widening already in and still found ZERO blocks to strip
        // on the post-load publish, so the block was gone before we touched it.
        //
        // ⚠ AND THE STRUCTURE SETTLES IT WITHOUT ANY LOG AT ALL. capable=false
        // is what JudgeStyle needs to reach kTooManyBones, so the 03:16 publish
        // took the DECLINE branch, and the decline branch returns above this
        // line. The PublishHead it crashed in is the back-out one, not the admit
        // one. Neither gating of this strip is even reached on that path, so
        // `8a86bd9` could not have changed that crash under any reasoning.
        // (The crash itself is OS-102, fixed at `64a96b1` by the pre-load
        // teardown, and stays closed: the stale FSMP systems are walked from
        // whichever PublishHead runs.)
        //
        // ⚠ THE WIDENING ALSO PROTECTED NOTHING, WHICH IS WHY IT COMES OUT
        // RATHER THAN STAYING AS A CHEAP BELT. This line is only reached for
        // kAdmit or kAdmitSmp; a decline returns above. kAdmit requires
        // maxBones <= kMaxHeadPartBones, so its bone indices run 0..7 and the
        // worker's `ed == 0 || 8 < bone` guard is never crossed. The OOB read
        // the shield exists to prevent is UNREACHABLE on a kAdmit style, so the
        // widening bought nothing and cost a plain head part its FMD for the
        // whole attach lifetime.
        //
        // The demotion keeps its own row (OS-105) and its own tell at the
        // measure above, which is where it belongs: it is a load-path property,
        // not a shield artefact.
        // ⚠⚠ WHICH SIDE OF THE PUBLISH THE COOPERATIVE PASS SITS ON IS DECIDED
        // BY THE FSMP LINE, and getting it wrong is silent. On the 4.0 route the
        // publish's own FixSkinInstances is already a whole-head sweep into
        // FSMP's SkinSingleGeometry handler, and processGeometry dedupes by
        // geometry POINTER and returns before it ever reads the FMD. Its
        // physicsFile comes only from the FMD branch; with no FMD it is scanned
        // off her facegeom nif instead, which holds no hair, so it comes back
        // empty and scanHead prints "No physics file for headpart" and never
        // builds a system. There is exactly ONE sighting per geometry ever, so
        // whoever reaches it first decides, permanently. Publish first on that
        // line and the style is physics-less for good with nothing saying so.
        const auto coop           = FsmpBridge::Route();
        const bool coopIsPreOwned = coop == FsmpBridge::Coop::kSkinAllEntry;

        std::vector<StrippedFmd> fmdStripped;
        if (verdict == NpcHairPlan::Admission::kAdmitSmp && coopIsPreOwned) {
            // ⚠ SAFE TO MAKE A PUBLISH-SHAPED CALL WITH THE FMD PRESENT HERE,
            // and only here: 4.0.1's hook never calls either stored original
            // for anyone, so the vanilla worker whose override reads facegen
            // model data does not execute. That is exactly what the FMD shield
            // exists to dodge on the 2.5 line, where the SkinAll handler DOES
            // call the original for a non-player skeleton. FsmpBridge::
            // ProveRoute has already shown FSMP owns the entry and that vfunc
            // 0x3E still points at it.
            const auto bonesBefore = CountRenamedBones(head.skeleton);
            FsmpBridge::CooperativeSkin(head.faceNode, head.skeleton, smp.fmdGeoms);
            const auto bonesAfter = CountRenamedBones(head.skeleton);
            spdlog::info("[hair/smp] '{}' cooperative pass via SkinAllGeometry (vf62), "
                         "fmd geoms {} of which {} are direct children, merged-bone "
                         "nodes before: {}, after: {}",
                         edid, smp.fmdGeoms.size(), smp.fmdDirectChildren, bonesBefore,
                         bonesAfter);
            if (smp.fmdDirectChildren < smp.fmdGeoms.size()) {
                spdlog::warn("[hair/smp] '{}' has {} FMD geometry(ies) nested below a part "
                             "root; the 4.0 sweep is one level deep and cannot reach them. "
                             "Expect a dangle back-out from the settle window, or "
                             "invisible strands.",
                             edid, smp.fmdGeoms.size() - smp.fmdDirectChildren);
            }
            // The shield goes back on for every LATER publish, and every later
            // publish now dedupes into a no-op for these geometries.
            fmdStripped = StripFmd(head.faceNode, toAttach);
        } else if (verdict == NpcHairPlan::Admission::kAdmitSmp) {
            fmdStripped = StripFmd(head.faceNode, toAttach);
        }

        // 6 and 7. The rebind is what took four field runs to find: a freshly
        // loaded nif arrives bound to its OWN bones, so the mesh was complete
        // and drawing nowhere until this pointed it at her skeleton.
        PublishHead(head, a_actor->GetFormID());

        // OS-99: hand the style to FSMP. One engine call per CAPABLE
        // geometry; a live FSMP detours it into processGeometry -> scanHead
        // -> a registered physics system stepped by the world (and by MS's
        // FsmpDrive while the bubble holds time). FSMP dedupes by physics
        // file, so hair + hairline resolving to one xml build one system.
        // After this first registration FSMP maintains itself: every later
        // publish's FixSkinInstances fires its events and its scanHead
        // regenerates.
        if (verdict == NpcHairPlan::Admission::kAdmitSmp && !coopIsPreOwned) {
            // The FMD window (Task 6.5): FSMP reads the block to find its
            // physics file, so it is back on the geometry for exactly this
            // loop and off again after, before anything else can publish.
            RestoreFmd(head.faceNode, fmdStripped);
            // Task 6.6 instrumentation: the renamed-bone census, before and
            // after the skin (see CountRenamedBones for how to read it).
            const auto bonesBefore = CountRenamedBones(head.skeleton);
            // ⚠ EVERY FMD-BEARING GEOMETRY, not just the capable ones
            // (Task 6.8, the round-3 mechanism). FSMP's generateMeshBody
            // (hdtSkyrimSystem.cpp) resolves each xml-named mesh under the
            // head node and wraps that mesh's CURRENT skin bone pointers
            // (skinInstance->m_ppkBones) as physics bodies, creating
            // unconstrained default bones for names its system does not
            // know ("created without default values"). A geometry we leave
            // unprocessed keeps its skin on clone-local nodes, so the
            // system HALF-ADOPTS it: its bones become orphan bodies with no
            // constraints and physics flings them to 12.9k units. That is
            // the dangle: every one across all three field rounds was an
            // _HL hairline, and ColaHL.nif carries no physics extra data,
            // so HLs were never in capableGeoms and never got processed.
            // processGeometry's repoint is the repair and is safe on a
            // physics-less geometry: an empty physics file just means no
            // system row ("No physics file for headpart", verbose level),
            // and the facegen disk-load fallback fires only on a broken FMD
            // MODEL chain, which every fmdGeom by construction does not
            // have. Admission still keys on capable (the physics pointer);
            // this loop keys on FMD.
            FsmpBridge::CooperativeSkin(head.faceNode, head.skeleton, smp.fmdGeoms);
            fmdStripped = StripFmd(head.faceNode, toAttach);
            const auto bonesAfter = CountRenamedBones(head.skeleton);
            spdlog::info("[hair/smp] '{}' merged-bone nodes before skin: {}, after: {}",
                         edid, bonesBefore, bonesAfter);

            // Evidence, not a gate (Task 6.5): this reads BEFORE any physics
            // step, so it cannot judge the system; the deferred check at the
            // next entry does that (RunDeferredSmpCheck). It stays because a
            // reading taken in the same pass as the SkinSingle calls is the
            // one that pairs with FSMP's own log lines when a round is read.
            float       worst = 0.0f;
            std::string worstGeom;
            for (const auto& n : toAttach) {
                const RE::BSFixedString nm{ n.c_str() };
                if (auto* const obj = head.faceNode->GetObjectByName(nm)) {
                    RE::BSVisit::TraverseScenegraphGeometries(
                        obj, [&](RE::BSGeometry* a_geom) -> RE::BSVisit::BSVisitControl {
                            if (a_geom->worldBound.radius > worst) {
                                worst     = a_geom->worldBound.radius;
                                worstGeom = a_geom->name.c_str();
                            }
                            return RE::BSVisit::BSVisitControl::kContinue;
                        });
                }
            }
            spdlog::info("[hair/smp] '{}' post-skin bound r={:.1f} on '{}'",
                         edid, worst, worstGeom.empty() ? "-" : worstGeom.c_str());
        }

        for (const auto& n : toAttach) {
            const RE::BSFixedString nm{ n.c_str() };
            if (head.faceNode->GetObjectByName(nm)) {
                rec.attached.push_back(n);
            }
        }

        // Measured AFTER the publish, because the publish is what places the
        // vertices, so before it every freshly attached mesh looks unplaced.
        // ⚠ NOT for a fresh SMP admit (Task 6.5): its strands are FSMP's to
        // place and no physics step has run yet, so the low-bound signature
        // this warns on is the healthy pre-step state there. The deferred
        // check owns that signature for SMP styles.
        if (verdict != NpcHairPlan::Admission::kAdmitSmp) {
            ReportUnplacedGeometry(head.faceNode, a_part->GetFormEditorID()
                                                      ? a_part->GetFormEditorID()
                                                      : "(no edid)",
                                   a_actor->GetFormID(), rec.attached);
        }

        // ⚠ ASKED vs LANDED, never one number. This line used to print
        // toAttach.size() for both, so a part that attached nothing logged
        // exactly like one that attached everything.
        const std::size_t hid    = rec.hidden.size();
        const std::size_t landedN = rec.attached.size();
        const bool        landed  = !rec.attached.empty();
        if (verdict == NpcHairPlan::Admission::kAdmitSmp) {
            rec.smp = true;
            rec.fmd = std::move(fmdStripped);  // the shield rides the record
        }
        {
            std::scoped_lock l(g_lock);
            if (rec.smp) {
                // Tripwire v2: looked at by the next NpcHair entry for her,
                // or by age (TickPendingSmpChecks) if the user just dwells;
                // the settle window may take several looks to a verdict.
                SmpPendingCheck pc;
                pc.styleFormID  = a_part->GetFormID();
                pc.edid         = edid;
                // The ATTACHED set (part names), deliberately: the look
                // traverses every geometry under them, _HL included, which
                // is exactly how rounds 2 and 3 caught the HL dangles.
                pc.geomNames    = rec.attached;
                pc.handle       = a_actor->GetHandle();
                pc.writtenAt    = std::chrono::steady_clock::now();
                pc.firstArmedAt = pc.writtenAt;
                g_smpPendingCheck[slot] = std::move(pc);
            }
            g_applied[slot] = std::move(rec);
            // The hook's gate, set where the record is published. See
            // NoteEngineHeadBuild.
            g_everApplied.store(true, std::memory_order_release);
            // The decision has landed, so stop shadowing reality with it.
            g_pending.erase(slot);
        }
        spdlog::info("NpcHair: actor {:08X} {} -> '{}' (culled {}/{}, attached {}/{}).",
                     a_actor->GetFormID(), KindWord(kind),
                     a_part->GetFormEditorID() ? a_part->GetFormEditorID() : "(no edid)",
                     hid, toHide.size(), landedN, toAttach.size());
        // ⚠ THE DYE WALK RUNS AGAIN FOR HER, because the roots it painted are
        // gone: every attach is fresh geometry (a restyle, or Reassert after a
        // cell change), and a follower's dyed hair ornament would otherwise come
        // back plain until something else repainted her (2026-09-04).
        // QueueRepaint touches no 3D here; it arms the chain that does.
        if (landed) {
            OutfitDye::QueueRepaint(a_actor->GetHandle());
        }
        // Every publish arms the settle ladder, because every publish is a
        // moment something later and silent can undo: the cell-return
        // repro's own re-apply logged culled 3/3 and still lost to a pass
        // that fired no seam we hold.
        ArmSettle(a_actor);
        return landed;
    }

    static bool RestoreNow(RE::Actor* a_actor, Kind a_kind) {
        if (!a_actor) {
            return false;
        }
        const Slot slot = SlotKey(a_actor->GetFormID(), a_kind);
        // OS-99 tripwire v2: judge a pending SMP admit BEFORE the record
        // leaves g_applied below; a broken one backs itself out through the
        // check and this restore then finds nothing left to do.
        RunDeferredSmpChecks(a_actor);
        Applied rec;
        {
            std::scoped_lock l(g_lock);
            const auto       it = g_applied.find(slot);
            if (it == g_applied.end()) {
                return false;
            }
            rec = std::move(it->second);
            g_applied.erase(it);
            g_pending.erase(slot);
            // Task 6.7: the settle window lets a mid-settle look re-arm the
            // pending check instead of consuming it, so the record's
            // teardown erases any survivor here; a pending look must not
            // outlive the style it measures.
            g_smpPendingCheck.erase(slot);
        }
        const Head head = ResolveHead(a_actor);
        if (!head.Ok()) {
            // Her head is gone, so the capture describes nodes that no longer
            // exist. Dropping it (above) is the whole restore. No reclaim
            // nudge is possible here either: with her 3D gone there is
            // nothing to enter FSMP through, and the recorded state is keyed
            // on the scenegraph being torn down.
            return false;
        }
        // A restore publishes too, so it deforms too, and on the path where the
        // editor opens straight onto a reset it is the FIRST thing to run.
        EnsureFaceSnapshot(head, a_actor->GetFormID());

        // Detach ours BEFORE un-hiding hers, or she is briefly in two
        // hairstyles.
        //
        // ⚠ COUNT WHAT ACTUALLY LEFT. GetObjectByName searches the whole
        // subtree but DetachChild only scans direct children, so a name that
        // resolves to something nested detaches nothing and says nothing. The
        // re-query below is the difference between "we removed it" and "we
        // asked to".
        //
        // For a style that went through FSMP, break the LEAF parent links
        // first so its cleanHead reclaims the physics system (Task 6.5; see
        // DetachGeometryLeaves). This is ALSO the ordinary style swap's
        // outgoing detach, because ApplyNow restyles through here, and the
        // reclaim matters most on the swap (Task 6.6): cleanHead is what
        // releases the swapped-out part's RENAMED bones, and KS styles reuse
        // strand bone names across styles (Anto92 *, Angels Braid *), so
        // the leading hypothesis has a leaked rename entry from the
        // outgoing style poisoning the incoming one's skeleton merge (see
        // NudgeFsmpReclaim for what is proven and what is not). Breaking
        // links only ARMS the sweep; the nudge after this restore's publish
        // runs one immediately.
        if (rec.smp) {
            DetachGeometryLeaves(head.faceNode, rec.attached);
        }
        const auto  t = DetachByNames(head.faceNode, rec.attached);
        std::size_t reshown = 0;
        for (const auto& n : rec.hidden) {
            if (SetHiddenByName(head.faceNode, n, false)) {
                ++reshown;
            }
        }
        PublishHead(head, a_actor->GetFormID());
        // Task 6.6 review: the restore is an SMP teardown site and every
        // teardown site fires the reclaim nudge itself. Through this one
        // line the nudge covers the explicit restore AND the ordinary style
        // swap, which detaches through here.
        if (rec.smp) {
            NudgeFsmpReclaim(head);
        }
        spdlog::info("NpcHair: actor {:08X} {} restored (re-shown {}/{}, detached {}/{}, "
                     "stuck {}, already gone {}).",
                     a_actor->GetFormID(), KindWord(a_kind), reshown, rec.hidden.size(),
                     t.detached, rec.attached.size(), t.stuck, t.missing);
        return true;
    }

    bool IsUnsupported(RE::BGSHeadPart* a_part) {
        if (!a_part) {
            return false;
        }
        std::scoped_lock l(g_lock);
        return g_unsupported.find(a_part->GetFormID()) != g_unsupported.end();
    }

    bool IsSessionUnsupported(RE::BGSHeadPart* a_part) {
        if (!a_part) {
            return false;
        }
        std::scoped_lock l(g_lock);
        return g_unsupportedSession.find(a_part->GetFormID()) !=
               g_unsupportedSession.end();
    }

    void TickPendingSmpChecks() {
        // Re-fires at kSmpTickDwell, the same constant the look gate holds
        // the final look to. ONE-SHOT per ARM, NOT a watcher: queuedTick
        // admits exactly one game-thread look per arm of a pending entry,
        // the look concludes the entry or re-arms it with a fresh stamp
        // (the settle window, Task 6.7), and the editor's per-frame Draw is
        // the only caller, so no polling exists outside the editor-open
        // frame path and a concluded entry costs nothing.
        struct Aged {
            Slot                                  slot{ 0 };  // (actor, kind)
            RE::ActorHandle                       handle;
            std::chrono::steady_clock::time_point writtenAt;
        };
        std::vector<Aged> aged;
        const auto        now = std::chrono::steady_clock::now();
        {
            std::scoped_lock l(g_lock);
            for (auto& kv : g_smpPendingCheck) {
                auto& pc = kv.second;
                if (pc.queuedTick || now - pc.writtenAt < kSmpTickDwell) {
                    continue;
                }
                pc.queuedTick = true;  // edge-triggered: never queued twice
                aged.push_back({ kv.first, pc.handle, pc.writtenAt });
            }
        }
        // Marshal OUTSIDE the lock, through the module's own queue. The
        // stamp rides along as the generation token: if a newer admit
        // replaced this entry before the task drains, the check declines to
        // consume it and the newer admit keeps its own judgement.
        for (const auto& a : aged) {
            OnGameThread(
                a.handle,
                [kind = SlotKind(a.slot), stamp = a.writtenAt](RE::Actor* a_live) {
                    RunDeferredSmpCheck(a_live, kind, stamp);
                },
                [slot = a.slot, stamp = a.writtenAt] {
                    // Dead handle at drain: the one-shot never ran, so
                    // re-arm it and let a later tick try again if she
                    // streams back in. The stamp comparison keeps this from
                    // touching a newer admit's entry.
                    std::scoped_lock l(g_lock);
                    const auto       it = g_smpPendingCheck.find(slot);
                    if (it != g_smpPendingCheck.end() &&
                        it->second.writtenAt == stamp) {
                        it->second.queuedTick = false;
                    }
                });
        }
    }

    bool SettleChecksPending() {
        // Lock-free on purpose: the ticker window's IsOpen calls this every
        // gameplay frame, and the card window's contract (a data query, never
        // engine work) holds here too.
        return g_settlePending.load(std::memory_order_acquire);
    }

    void TickSettleChecks() {
        // Same discipline as TickPendingSmpChecks one function up: runs on
        // the RENDER thread, collects under the lock, marshals through the
        // module's own queue, touches no engine state here. The armedAt
        // stamp is the generation token: a walk that re-armed the ladder
        // (ApplyNow or the culled-again recovery both do) leaves the fresh
        // arm's own ladder untouched.
        if (!g_settlePending.load(std::memory_order_acquire)) {
            return;
        }
        struct Due {
            RE::FormID                            id{ 0 };
            RE::ActorHandle                       handle;
            std::chrono::steady_clock::time_point armedAt;
        };
        std::vector<Due> due;
        const auto       now = std::chrono::steady_clock::now();
        {
            std::scoped_lock l(g_lock);
            for (auto& [id, w] : g_settle) {
                if (w.queued || w.stage >= kSettleDelays.size() ||
                    now - w.armedAt < kSettleDelays[w.stage]) {
                    continue;
                }
                w.queued = true;  // edge-triggered, one walk in flight per arm
                due.push_back({ id, w.handle, w.armedAt });
            }
        }
        for (const auto& d : due) {
            OnGameThread(
                d.handle,
                [id = d.id, stamp = d.armedAt](RE::Actor* a_live) {
                    ReassertNow(a_live);
                    bool pending = true;
                    {
                        std::scoped_lock l(g_lock);
                        const auto       it = g_settle.find(id);
                        if (it != g_settle.end() && it->second.armedAt == stamp) {
                            it->second.queued = false;
                            if (++it->second.stage >= kSettleDelays.size()) {
                                g_settle.erase(it);  // three quiet walks: done
                            }
                        }
                        pending = !g_settle.empty();
                    }
                    g_settlePending.store(pending, std::memory_order_release);
                },
                [id = d.id, stamp = d.armedAt] {
                    // Streamed out before the walk. Drop the ladder: her next
                    // load goes through NpcLoadSink's reassert, whose apply
                    // arms a fresh one.
                    bool pending = true;
                    {
                        std::scoped_lock l(g_lock);
                        const auto       it = g_settle.find(id);
                        if (it != g_settle.end() && it->second.armedAt == stamp) {
                            g_settle.erase(it);
                        }
                        pending = !g_settle.empty();
                    }
                    g_settlePending.store(pending, std::memory_order_release);
                });
        }
    }

    bool Handles(Kind a_kind) {
        for (const auto kind : kKinds) {
            if (kind == a_kind) {
                return true;
            }
        }
        return false;
    }

    void Apply(RE::Actor* a_actor, RE::BGSHeadPart* a_part) {
        if (!a_actor || !a_part) {
            return;
        }
        // ⚠ THE KIND IS THE PART'S, AND AN UNHANDLED ONE STOPS HERE. Said once
        // per part rather than per call, because a hover preview calls this
        // every frame the mouse rests on a row.
        const auto kind = KindOfPart(a_part);
        if (!kind) {
            static std::unordered_set<RE::FormID> s_said;
            bool first = false;
            {
                std::scoped_lock l(g_lock);
                first = s_said.insert(a_part->GetFormID()).second;
            }
            if (first) {
                spdlog::warn("NpcHair: '{}' is head part type {} and this route takes hair "
                             "and facial hair only; ignored.",
                             a_part->GetFormEditorID() ? a_part->GetFormEditorID()
                                                       : "(no edid)",
                             a_part->type.underlying());
            }
            return;
        }
        // Already measured and declined. Re-attempting costs a real attach, a
        // detach and a publish for an answer that cannot have changed, and the
        // hover preview would pay it every time the mouse rested on the row.
        if (IsUnsupported(a_part)) {
            return;
        }
        {
            std::scoped_lock l(g_lock);
            g_pending[SlotKey(a_actor->GetFormID(), *kind)] = a_part;
        }
        OnGameThread(a_actor->GetHandle(),
                     [a_part](RE::Actor* a_live) { ApplyNow(a_live, a_part); });
    }

    void Restore(RE::Actor* a_actor, Kind a_kind) {
        if (!a_actor || !Handles(a_kind)) {
            return;
        }
        {
            std::scoped_lock l(g_lock);
            // decided: back to her own, in this kind
            g_pending[SlotKey(a_actor->GetFormID(), a_kind)] = nullptr;
        }
        OnGameThread(a_actor->GetHandle(),
                     [a_kind](RE::Actor* a_live) { RestoreNow(a_live, a_kind); });
    }

    void OnPreLoadGame() {
        // OS-102. THE ONE MOMENT HER 3D IS STILL VALID, and the reason this
        // function has to exist at all is written two hundred lines up in
        // RestoreNow's own early-out:
        //
        //   "No reclaim nudge is possible here either: with her 3D gone there
        //    is nothing to enter FSMP through, and the recorded state is keyed
        //    on the scenegraph being torn down."
        //
        // That early-out is exactly where an in-session load used to land.
        // Nothing of ours ran before the engine destroyed her head; the only
        // teardown afterwards is Clear() from Persistence::RevertCallback,
        // which by contract drops every capture WITHOUT TOUCHING A SINGLE
        // ACTOR. Correct for our captures, and it leaves FSMP holding physics
        // systems whose bone pointers name a head that no longer exists. The
        // next publish drives FixSkinInstances into FSMP's SkinAllGeometry
        // hook, which walks them, and that is the CTD.
        //
        // ⚠ THE DISCRIMINATOR THAT FOUND THIS WAS THE USER'S, NOT A DUMP'S:
        // loading from the MAIN MENU never crashed, saving and loading
        // IN-SESSION always did. A fresh FSMP process has no systems to go
        // stale. Two crash dumps agreed on the fault site and neither could
        // separate those two cases; one sentence of field observation did.
        //
        // ⚠ A BARE NudgeFsmpReclaim WOULD NOT DO. Its own derivation says the
        // cleanHead sweep reclaims parts "whose leaf parent links the teardown
        // just broke", and RestoreNow's notes say breaking links only ARMS the
        // sweep. Nothing is detached yet at this point, so a nudge on its own
        // would sweep nothing. The full restore is the documented sequence:
        // detach (arming it), publish, and every teardown site fires its own
        // nudge. Doing scenegraph work on a character about to be destroyed
        // looks wasteful and is the entire point - it is the last instant the
        // work is possible.
        //
        // ⚠ SYNCHRONOUS, NOT QUEUED. kPreLoadGame already runs on the main
        // thread, and a queued task would drain AFTER the load, which is the
        // very ordering this exists to beat. Hence RestoreNow directly rather
        // than the public Restore.
        // Every (actor, kind) capture, so a follower wearing our hair AND our
        // beard is torn down in both.
        std::vector<Slot> slots;
        {
            std::scoped_lock l(g_lock);
            if (g_applied.empty()) {
                return;  // nothing attached, so nothing for FSMP to hold
            }
            slots.reserve(g_applied.size());
            for (const auto& [slot, rec] : g_applied) {
                slots.push_back(slot);
            }
        }
        // Outside the lock: RestoreNow takes g_lock itself.
        std::size_t restored = 0;
        for (const auto slot : slots) {
            auto* const form  = RE::TESForm::LookupByID(SlotId(slot));
            auto* const actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor) {
                continue;  // already gone; RestoreNow's early-out would say the same
            }
            if (RestoreNow(actor, SlotKind(slot))) {
                ++restored;
            }
        }
        spdlog::info("NpcHair: pre-load teardown restored {} of {} applied capture(s) while "
                     "their 3D was still valid. A zero here with a non-zero count means "
                     "every head had already gone, which is the OS-102 shape.",
                     restored, slots.size());
    }

    RE::BGSHeadPart* AppliedTo(RE::Actor* a_actor, Kind a_kind) {
        if (!a_actor) {
            return nullptr;
        }
        const Slot       slot = SlotKey(a_actor->GetFormID(), a_kind);
        std::scoped_lock l(g_lock);
        // The DECISION outranks the applied state, so the editor reflects what
        // was asked for rather than what has drained yet. Without this the hair
        // row lags a frame and the hover preview re-fires against itself.
        const auto pending = g_pending.find(slot);
        if (pending != g_pending.end()) {
            return pending->second;
        }
        const auto it = g_applied.find(slot);
        return it == g_applied.end() ? nullptr : it->second.part;
    }

    std::vector<std::string> AttachedRootNames(RE::Actor* a_actor, Kind a_kind) {
        if (!a_actor) {
            return {};
        }
        const Slot       slot = SlotKey(a_actor->GetFormID(), a_kind);
        std::scoped_lock l(g_lock);
        const auto       it = g_applied.find(slot);
        return it == g_applied.end() ? std::vector<std::string>{} : it->second.attached;
    }

    void Reassert(RE::Actor* a_actor) {
        if (!a_actor) {
            return;
        }
        OnGameThread(a_actor->GetHandle(), [](RE::Actor* a_live) { ReassertNow(a_live); });
    }

    bool HoldsHeadPartFor(RE::Actor* a_actor) {
        if (!a_actor) {
            return false;
        }
        const auto       id = a_actor->GetFormID();
        std::scoped_lock l(g_lock);
        for (const auto& [slot, rec] : g_applied) {
            if (SlotId(slot) == id && rec.part) {
                return true;
            }
        }
        return false;
    }

    void NoteEngineHeadBuild() {
        // Nothing of ours has ever been on a head, so no build can have
        // disturbed it. One relaxed load on the ordinary path.
        if (!g_everApplied.load(std::memory_order_acquire)) {
            return;
        }
        // ⚠ ONE TASK PER BURST. A head build calls the painter once per PART,
        // and a cell load builds a street full of NPCs, so an unlatched queue
        // here would be hundreds of walks for one event. The latch clears
        // inside the task, so the next burst gets its own.
        if (g_buildReassertQueued.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        auto* const task = SKSE::GetTaskInterface();
        if (!task) {
            g_buildReassertQueued.store(false, std::memory_order_release);
            return;
        }
        // ⚠ QUEUED, NEVER DONE HERE, and it is the hook's own argument: the
        // head is mid-construction, so the node this call is painting is not
        // necessarily attached to the actor's 3D yet. The task drains after the
        // frame's work, which is after the whole rebuild.
        task->AddTask([] {
            g_buildReassertQueued.store(false, std::memory_order_release);
            // ⚠ EVERY ACTOR WE HOLD, because the hook cannot say whose head it
            // was: getting from a TESNPC back to the Actor wearing it means
            // walking the process lists, which is exactly the work its note
            // forbids inside the detour. This map is one or two entries in
            // practice, and a reassert on an untouched head is a handful of
            // name lookups that change nothing.
            std::vector<RE::FormID> actors;
            {
                std::scoped_lock l(g_lock);
                actors.reserve(g_applied.size());
                for (const auto& [slot, rec] : g_applied) {
                    const auto id = SlotId(slot);
                    if (std::find(actors.begin(), actors.end(), id) == actors.end()) {
                        actors.push_back(id);
                    }
                }
            }
            for (const auto id : actors) {
                if (auto* const actor = RE::TESForm::LookupByID<RE::Actor>(id)) {
                    ReassertNow(actor);
                }
            }
        });
    }

    // One kind's half of a reassert. Returns nothing: a kind she does not wear
    // through us, or one whose geometry is still there, is simply left alone.
    static void ReassertKindNow(RE::Actor* a_actor, const Head& a_head, Kind a_kind) {
        const Slot               slot = SlotKey(a_actor->GetFormID(), a_kind);
        RE::BGSHeadPart*         want = nullptr;
        std::vector<std::string> attached;
        std::vector<std::string> hidden;
        {
            std::scoped_lock l(g_lock);
            const auto       it = g_applied.find(slot);
            if (it == g_applied.end()) {
                return;
            }
            want     = it->second.part;
            attached = it->second.attached;
            hidden   = it->second.hidden;
        }
        if (!want) {
            return;
        }

        // ⚠ VERIFY BEFORE ACTING. Re-applying unconditionally would attach a
        // SECOND copy every time anything called this, because Apply culls and
        // attaches rather than reconciling. Our geometry still being present
        // means nothing was lost.
        //
        // ⚠ SOUND ONLY BECAUSE THE RECORDED NAMES ARE OURS ALONE (OS-246).
        // These are the kOwnPrefix names ApplyNow stamped, so a hit here is
        // our piece by construction. When this read bare part names, a
        // follower whose own hair shared an extra part with the style found
        // HER rebuilt hairline under OUR recorded name, skipped the re-apply,
        // and the recovery below then culled her way to a bald head.
        bool ours = false;
        for (const auto& n : attached) {
            const RE::BSFixedString nm{ n.c_str() };
            if (a_head.faceNode->GetObjectByName(nm)) {
                ours = true;
                break;
            }
        }

        // ⚠⚠ AND THAT IS ONLY HALF THE QUESTION, WHICH IS THE DOUBLED HAIR.
        // Until 2026-08-19 this returned right here on a present attachment, so
        // an apply that STAYED and a cull that came UNDONE read identically: a
        // pass that re-shows her own head parts leaves our style attached and
        // hers visible under it, and the verify called that healthy. The field
        // report is exact about when it shows - "when i change her hair and
        // exit i see her old hair and her new hair together" - and the log
        // agrees with the reading: her applies say culled 3/3 attached 3/3 and
        // nothing of ours runs afterwards.
        //
        // So the second question: is everything we culled still out of sight?
        // Asked against her CURRENT part rather than only against the recorded
        // names, because a rebuild can hand her a different one, and the names
        // we hid last time would then describe nodes nobody is rendering.
        if (ours) {
            std::vector<std::string> shouldBeHidden = hidden;
            CollectPartNames(a_head.base->GetCurrentHeadPartByType(EngineTypeOf(a_kind)),
                             shouldBeHidden);
            std::size_t recovered = 0;
            for (const auto& n : shouldBeHidden) {
                if (!VisibleByName(a_head.faceNode, n)) {
                    continue;  // absent or still hidden: nothing came back
                }
                if (!SetHiddenByName(a_head.faceNode, n, true)) {
                    continue;
                }
                ++recovered;
                // ⚠ RECORDED, OR RESTORE STOPS BEING EXACT. Restore re-shows
                // what this record says it hid, so a name culled here and not
                // written down would stay invisible after the style comes off.
                std::scoped_lock l(g_lock);
                if (const auto it = g_applied.find(slot); it != g_applied.end()) {
                    auto& names = it->second.hidden;
                    if (std::find(names.begin(), names.end(), n) == names.end()) {
                        names.push_back(n);
                    }
                }
            }
            if (recovered != 0) {
                spdlog::info("NpcHair: actor {:08X} had {} of her own {} piece(s) come back "
                             "under our style; culled again.",
                             a_actor->GetFormID(), recovered, KindWord(a_kind));
                // Something is still un-hiding her pieces, so keep watching:
                // a fresh ladder, from now. A quiet head never reaches this
                // line and its ladder expires.
                ArmSettle(a_actor);
            }
            return;
        }

        // It is gone, so the head was rebuilt. Drop the capture WITHOUT
        // restoring: a rebuild already discarded our geometry and re-created
        // hers, so those names now describe nodes that are either absent or
        // freshly un-hidden, and running Restore against them would detach HER
        // OWN HAIR. Re-apply against whatever is on the head now.
        {
            std::scoped_lock l(g_lock);
            g_applied.erase(slot);
            // Task 6.7: any mid-settle pending look dies with the record;
            // its geometry names describe the pre-rebuild scenegraph.
            g_smpPendingCheck.erase(slot);
        }
        spdlog::info("NpcHair: actor {:08X} lost its attached {} to a rebuild; re-applying.",
                     a_actor->GetFormID(), KindWord(a_kind));
        Apply(a_actor, want);
    }

    static void ReassertNow(RE::Actor* a_actor) {
        if (!a_actor) {
            return;
        }
        const Head head = ResolveHead(a_actor);
        if (!head.Ok()) {
            // ⚠⚠ A DROPPED HEAD BUILD USED TO END HERE IN SILENCE, WITH NOTHING
            // EVER LOOKING AGAIN. NoteEngineHeadBuild queues its walk for the
            // end of the frame the build started in, but a follower streaming
            // into a cell has no currentProcess->middleHigh->faceNodeSkinned
            // yet, so ResolveHead fails, this returned, and the re-cull that
            // takes her own hair back out from under our style never ran. Her
            // head finished publishing a frame or three later with both on it.
            //
            // Hand it to the settle ladder instead: 750 ms, 2 s and 5 s is
            // exactly the window a head needs to finish publishing, and the
            // ticker runs it during ordinary gameplay. IfIdle, never ArmSettle,
            // for the reason written at that function.
            if (HoldsHeadPartFor(a_actor)) {
                spdlog::info("NpcHair: actor {:08X} had a head build while her face node "
                             "was not resolvable yet, so the re-cull is handed to the "
                             "settle ladder rather than dropped.",
                             a_actor->GetFormID());
                ArmSettleIfIdle(a_actor);
            }
            return;  // nothing on screen to re-assert onto
        }
        // ⚠ EVERY KIND, because a rebuild drops the beard and the hair alike
        // and the seams that call this (a load, a race switch, a 3D refresh)
        // do not know which of the two she was wearing through us.
        for (const auto kind : kKinds) {
            ReassertKindNow(a_actor, head, kind);
        }
    }

    void Clear() {
        std::scoped_lock l(g_lock);
        g_applied.clear();
        g_pending.clear();
        // A pending SMP check names scenegraph nodes on the character being
        // torn down, so it goes with the captures (Task 6.5).
        g_smpPendingCheck.clear();
        // The settle ladders watch captures that no longer exist.
        g_settle.clear();
        g_settlePending.store(false, std::memory_order_release);
        // The head baselines describe vertex buffers on a character being torn
        // down, so they go with the captures. The next character's first touch
        // takes its own.
        g_faceSnapshot.clear();
        // g_unsupported and g_unsupportedKeys survive on purpose: they
        // describe meshes, and no save boundary changes a mesh.
        // g_unsupportedSession survives with them: an FSMP-side failure is
        // per-PROCESS state, and a save revert does not restart FSMP.
        // g_smpMeasuredCapable survives for the same reason and it MUST: the
        // demotion it guards against is observed exactly at a load boundary,
        // so clearing it here would drop the memory one instant before the
        // reading that needs it (OS-105).
    }

    void ClearFsmpAbsentDeclines() {
        std::size_t cleared = 0;
        {
            std::scoped_lock l(g_lock);
            for (const auto id : g_unsupportedFsmpAbsent) {
                g_unsupported.erase(id);
                g_unsupportedSession.erase(id);
                ++cleared;
            }
            g_unsupportedFsmpAbsent.clear();
        }
        // ⚠ SILENT WHEN THERE IS NOTHING TO UNDO, which is the common case: the
        // route usually arms before the player has browsed anything. A line per
        // load boundary saying "cleared 0" would bury the one that matters.
        if (cleared != 0) {
            spdlog::info("NpcHair: an FSMP route armed after {} style(s) had already been "
                         "declined for want of one (OS-106). Those declines are dropped, so "
                         "the rows offer again without a relaunch. Nothing persisted was "
                         "touched.",
                         cleared);
        }
    }

    void LoadUnsupportedAtStartup() {
        std::error_code ec;
        if (!std::filesystem::exists(kUnsupportedPath, ec)) {
            return;  // first run, or nothing ever declined
        }
        Json::Value root;
        {
            std::ifstream in(kUnsupportedPath);
            if (!in || !(in >> root)) {
                spdlog::warn("NpcHair: {} exists but does not parse; a freshly "
                             "declined style will overwrite it.",
                             kUnsupportedPath.string());
                return;
            }
        }
        std::vector<StyleRefKey> keys;
        if (!JsonCodec::JsonToUnsupportedHair(root, keys)) {
            spdlog::warn("NpcHair: '{}' is unreadable or an outdated schema; "
                         "starting with no persisted declines (each style "
                         "re-measures once).",
                         kUnsupportedPath.string());
            return;
        }
        auto* const dh = RE::TESDataHandler::GetSingleton();
        std::size_t resolved = 0;
        {
            std::scoped_lock l(g_lock);
            g_unsupportedKeys = std::move(keys);
            for (const auto& k : g_unsupportedKeys) {
                // An unresolvable key stays in the list (its plugin may
                // return) but gates nothing this session - exactly how an
                // NPC assignment with an absent plugin behaves.
                auto* const part =
                    dh ? dh->LookupForm<RE::BGSHeadPart>(k.localFormID, k.modName)
                       : nullptr;
                if (part) {
                    g_unsupported.insert(part->GetFormID());
                    ++resolved;
                }
            }
        }
        spdlog::info("NpcHair: {} unsupported style(s) loaded from disk ({} "
                     "resolved in this load order). Their rows grey out "
                     "without the measuring attach this time.",
                     g_unsupportedKeys.size(), resolved);
    }

}  // namespace OS::NpcHair
