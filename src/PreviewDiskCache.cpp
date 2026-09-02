#include "PreviewDiskCache.h"

#include "BuildChannel.h"
#include "FuckCompat.h"  // FUCK::GetPluginConfigPath, the fallback root
#include "PreviewGrid.h"
#include "Settings.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>

namespace OS::PreviewDiskCache {

    namespace {

        // v2: the AABB-driven framing (PreviewFraming.h). v3: effect-shader
        // geometry skipped (the Dawnbreaker cream ellipse). v4: positions
        // read at their measured width and dynamic shapes from their morph
        // buffer (armour round 1 baked polygon soup). v5: worn gear stands
        // upright (armour round 2 lay on the weapon diagonal). A bump
        // refuses the whole manifest, which is the designed total-reset
        // path: every thumbnail baked under the old rule rebuilds itself.
        // v9: the lone-shield pose mirrored to the face side (kShield); a
        // bump is the designed reset for a rendered-output change.
        // v10: two-sided lighting. The rasteriser draws back faces (CULL_NONE)
        // and the shader lit them off a normal pointing away from the key, so
        // every back-facing strand card came out nearly black; hair showed it
        // because the alpha pass writes no depth and cannot hide them.
        //
        // ⚠ A BUMP IS THE RIGHT TOOL HERE AND WAS THE WRONG ONE TWICE TODAY.
        // OS-191 and OS-200 both changed rendered output and both avoided this
        // by folding their cause into the disk key, so only the affected cards
        // rebuilt. That is always preferable and it is not available here: the
        // fix is in the SHADER, it touches any mesh with a visible back face,
        // and capture cannot know which those are without loading the NIF. So
        // the whole cache rebuilds once, which is what this constant is for.
        // v11: head parts that carry no skeleton are moved onto the mannequin's
        // head instead of rendering at the origin. Eight of forty eye cards put
        // their mesh at z 2.57..4.04 against a window covering 121.5..123.8 and
        // came out blank; they are the separate faceparts/eyes*left.nif and
        // eyes*right.nif that the half-blind records use, authored in head
        // space with nothing to say so.
        //
        // ⚠⚠ AND THE BUMP IS THE REAL LESSON OF THAT ROUND. The placement fix
        // shipped twice and the field saw no change BOTH times, because a card
        // already on disk is served without rendering anything: the first miss
        // was a gate that made the code unreachable, the second was this
        // constant not moving, and only the second one was avoidable by reading
        // the file it lives in. The note above says outright that a bump
        // "refuses the whole manifest, which is the designed total-reset path",
        // and the alternative it recommends - folding the cause into the disk
        // key - is not available here for the same reason it was not available
        // to v10: the cause is a property of the NIF's contents, and capture
        // cannot know it without loading the mesh.
        //
        // ⚠ ASKING THE PLAYER TO PRESS 'Rebuild previews' IS NOT THE
        // ALTERNATIVE. It was tried across two rounds and cost both of them.
        // A render change that needs a human to remember a button is a render
        // change that ships broken to everyone who does not.
        // v14: the head-space correction stopped firing on a mesh nobody
        // measured. A skinned hair carries its placement in the skin, so the
        // file leaves the bounding sphere at zero, and the geometry test read
        // that zero as "authored at the origin" and lifted the hair a head
        // height above the mannequin. Most of the hair page rendered a bald
        // bust (field 2026-08-16). Every card built under v13 has the same
        // wrong picture baked into it, and MeshExtractor's own note says why
        // folding the cause into the disk key is not available here: the cause
        // is a property of the NIF's contents.
        // ⚠ v16: the eye card is framed off the EYE now, after v15's head
        // anchor came back from the field low on both rigs in one session. Their
        // head meshes differ and their eye meshes differ by 64% in height, so no
        // share of the head could centre both. v15 also stopped the male
        // mannequin's physics proxies stretching the reference box (OS-205).
        // None of it changes anything the disk key can see, so every card built
        // under an older version has the old framing baked into it.
        // ⚠ v18: the eye window is centred on the item's box CENTRE, which is
        // the pupil axis on a globe and the middle of the aperture on a patch,
        // after v17 hung it from the box's TOP and rode 0.49 units high on the
        // one mesh that models the whole eyeball. v17 took the eye's size
        // from the head, because v16 filled the frame with the item's whole box
        // and one mesh carries 0.82 units of globe below the lid, and it moved
        // x from 0.52 of the pair's half width to 0.65, where one eye actually
        // sits. Three field notes, three versions, one card.
        // v21 places a skinned shape at its BIND POSE rather than at whatever
        // its vertex buffer says, which is a different picture for any armour
        // whose author did not build in body space. Field 2026-08-20: Practical
        // Pirate's male mesh drew at z -86.79..-3.81, entirely below the
        // mannequin's feet, and its own preview.boxes line reported that range
        // to two decimals. Three cards in four are unchanged, because their
        // bind pose is the identity and the correction is then a no-op.
        // v22 draws legacy NiTriShape geometry, which every card built before
        // it left out entirely. Those meshes are not a BSGeometry, so the walk
        // dropped them before any filter ran and the card came back as the
        // mannequin alone with status=ready and nothing in the log naming a
        // shape. Field 2026-08-20, the user's screenshot: two Night Lurker
        // cards showing a bare grey body beside a correct Abyss Top. 1676 of
        // 16607 worn meshes on that load order carry legacy geometry, so this
        // is a different picture for a tenth of the catalog and identical for
        // the rest.
        // v23 was an attempt at Bound Arrow's violet slab that shipped, was
        // measured in the field and was reverted the same day: skipping any
        // geometry whose blend modes the pass owns no state for. ENB Light
        // re-authors the ARROW ITSELF as blend on, srcAlpha to ONE with alpha
        // test GREATER 128, so the rule ate seven arrow shapes and left the
        // slab, which carries no alpha property at all and so had no blend
        // modes to judge. The number is kept rather than rolled back: cards
        // were rebuilt at v23 already, and rolling it back would rebuild every
        // card a second time to reach the same picture v22 drew. The slab is
        // handled where it belongs, as an authored scope over
        // weapons/boundweapons/ (OS-244), and a scope tag re-keys exactly the
        // cards it covers.
        // v24 draws an unreproducible blend that carries an alpha test in the
        // OPAQUE pass instead of the alpha one. ENB Light re-authors bound
        // weapon geometry as blend on, srcAlpha to ONE, alpha test GREATER
        // 128, and the alpha pass tests depth without writing it, so the Bound
        // Arrow's own six arrows drew as translucent shapes ghosting through
        // each other (user 2026-08-21, third round, with the card read back
        // out of the PNG cache). The test is what shapes such a mesh, so it
        // draws opaque with its cutoff intact.
        // v25 skips the two kinds of FX plane no shader class can see: an
        // overlay blend (unreproducible modes with NO alpha test to shape it)
        // and a plane carrying no alpha property at all, which draws fully
        // opaque and is caught by its FX name. Field 2026-08-21: after the
        // bound arrow was fixed, the same slab was still on Ordinator's trick
        // arrows, Darkend's torment arrow and the Creation Club magic arrows.
        // v26 makes the FX rescue TIERED. Vigilant's Umaril arrow authors all
        // eight of its arrow shapes as 0x100D, blend on, srcAlpha to ONE, test
        // off, which is indistinguishable by blend from a glow plane, so the
        // scene emptied and the all-or-nothing rescue put the glow plane back
        // with them. Rung one now restores only the blend-skipped geometry and
        // keeps dropping effect shaders and FX names.
        //
        // v27 changes no renderer code at all: the de-shadow revert of
        // 2026-08-21 moved 164 BodySlide meshes back over PGPatcher's TruePBR
        // builds, so the NIFs the cards read changed under a key that did not.
        // Cards are keyed by their inputs and the meshes are an input
        // (cache-key-must-be-the-inputs-not-the-subject); the bump is how the
        // 164 stale PBR-era cards leave.
        constexpr const char* kRendererVersion = "fittingroom-preview-png-v27";

        // All state is render-thread-only (the drain is the sole caller), so
        // plain members and no lock, the PreviewCache contract.
        struct Store {
            std::filesystem::path       root;
            PreviewManifest::Manifest   manifest;
            std::uint64_t               tick{ 0 };
            std::uint64_t               bytes{ 0 };  // ready entries' file sizes
            bool                        dirty{ false };
            bool                        loaded{ false };
            bool                        collisionSaid{ false };
        };
        Store* g_store = nullptr;  // leaked at exit on purpose, IconImages' reason

        std::filesystem::path ManifestPath(const Store& a_s) {
            return a_s.root / "manifest.json";
        }

        // ⚠ THE ROOT IS HARDCODED UNDER THE MOD'S OWN DATA DIRECTORY, never
        // an INI string: Wardrobe remove_all's a path read from its INI and a
        // typo there deletes whatever it names. The one probe file answers
        // whether this process can WRITE into the VFS-visible tree (under MO2
        // the write lands in overwrite and reads come back through the merged
        // view); when it cannot, FUCK's host-designated config dir is the
        // fallback and the log says which root won.
        std::filesystem::path ResolveRoot() {
            const auto primary = BuildChannel::DataPath("PreviewCache");
            std::error_code ec;
            std::filesystem::create_directories(primary, ec);
            const auto probe = primary / "probe.tmp";
            {
                std::ofstream out(probe, std::ios::binary | std::ios::trunc);
                out << "probe";
            }
            std::ifstream in(probe, std::ios::binary);
            std::string   text{ std::istreambuf_iterator<char>(in),
                              std::istreambuf_iterator<char>() };
            in.close();
            std::filesystem::remove(probe, ec);
            if (text == "probe") {
                spdlog::info("PreviewDiskCache: root '{}' (VFS write probe passed).",
                             primary.string());
                return primary;
            }
            char buf[MAX_PATH]{};
            FUCK::GetPluginConfigPath("FittingRoom", buf, sizeof buf);
            if (buf[0] != '\0') {
                const auto fallback = std::filesystem::path{ buf } / "PreviewCache";
                std::filesystem::create_directories(fallback, ec);
                spdlog::warn(
                    "PreviewDiskCache: the Data tree refused the write probe, so the "
                    "cache lives at '{}' instead.",
                    fallback.string());
                return fallback;
            }
            spdlog::warn(
                "PreviewDiskCache: no writable root anywhere, so thumbnails rebuild "
                "every session.");
            return primary;  // writes will fail and be logged per file
        }

        void WipeImages(const Store& a_s) {
            std::error_code ec;
            for (const auto& entry :
                 std::filesystem::directory_iterator(a_s.root, ec)) {
                std::error_code entryEc;
                if (entry.is_directory(entryEc) &&
                    entry.path().filename().string().size() == 2) {
                    std::filesystem::remove_all(entry.path(), entryEc);
                }
            }
            std::filesystem::remove(ManifestPath(a_s), ec);
        }

        Store& Get() {
            if (g_store) {
                return *g_store;
            }
            const auto t0 = std::chrono::steady_clock::now();
            g_store       = new Store{};
            g_store->root = ResolveRoot();
            g_store->manifest.thumbnailSize =
                Settings::GetSingleton().previewThumbPx;
            g_store->manifest.rendererVersion = kRendererVersion;

            std::size_t   statted = 0;
            std::ifstream in(ManifestPath(*g_store), std::ios::binary);
            if (in) {
                const std::string text{ std::istreambuf_iterator<char>(in),
                                        std::istreambuf_iterator<char>() };
                const auto parsed = PreviewManifest::FromText(
                    text, g_store->manifest.thumbnailSize, kRendererVersion);
                if (parsed) {
                    g_store->manifest = *parsed;
                    // ⚠ THE FILESYSTEM IS ASKED ONLY ABOUT ENTRIES THAT HAVE NO
                    // RECORDED SIZE. This loop used to stat EVERY ready entry,
                    // and on a 14,991-entry cache that was 2012 ms spent on the
                    // thread that opens the editor: 134 us per entry, which is
                    // a USVFS round trip rather than any kind of parsing. The
                    // sizes are in the manifest now, so a warm cache asks the
                    // filesystem nothing at all.
                    for (auto& [hash, e] : g_store->manifest.entries) {
                        g_store->tick = (std::max)(g_store->tick, e.updatedTick);
                        if (e.status != PreviewManifest::Status::kReady) {
                            continue;
                        }
                        if (e.bytes == 0) {
                            std::error_code ec;
                            const auto      size = std::filesystem::file_size(
                                g_store->root / e.file, ec);
                            if (ec) {
                                continue;
                            }
                            // Written back so this is paid once per entry for
                            // the life of the cache rather than once per
                            // session. dirty is set below, so the next flush
                            // persists every answer this loop just bought.
                            e.bytes = size;
                            ++statted;
                        }
                        g_store->bytes += e.bytes;
                    }
                    if (statted > 0) {
                        g_store->dirty = true;
                    }
                } else {
                    // Stale or unreadable: the free total reset the three
                    // staleness mechanisms exist to buy. Every image goes
                    // with the manifest, or orphaned files pile up forever.
                    spdlog::info(
                        "PreviewDiskCache: manifest stale or unreadable, starting the "
                        "cache over.");
                    WipeImages(*g_store);
                }
            }
            g_store->loaded = true;
            // The stat count is on the line on purpose. A warm cache reads zero
            // and a first run after the upgrade reads the whole entry count,
            // so "why was this open slow" is answered by the same line that
            // reports the time rather than by guessing.
            spdlog::debug("PreviewDiskCache: disk-cache.manifest.load {} entries in "
                          "{:.1f} ms ({} needed a size from disk).",
                          g_store->manifest.entries.size(),
                          std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count(),
                          statted);
            return *g_store;
        }

        std::string HashOf(const std::string& a_diskKey) {
            char buf[20]{};
            std::snprintf(buf, sizeof buf, "%016llX",
                          static_cast<unsigned long long>(
                              PreviewGrid::Fnv1a64(a_diskKey)));
            return buf;
        }

        // Take an entry's contribution back out of the running total, by hash.
        //
        // ⚠⚠ THE TOTAL USED TO HEAL ITSELF EVERY SESSION AND NOW IT DOES NOT,
        // which is what makes this function necessary rather than tidy. While
        // the load re-summed every file from disk, an entry overwritten without
        // subtracting its old size merely inflated the total until the next
        // launch. Persisted, that same slip is permanent: re-rendering one card
        // enough times would push the cache over its budget and start evicting
        // pictures that fit. Both overwrite paths below were doing exactly that
        // - MarkReady added the new size over the old, and MarkFailed dropped a
        // ready entry's size on the floor - so persisting the total without
        // this would have turned two harmless leaks into a durable one.
        //
        // Does nothing for an entry that is not ready or has no recorded size,
        // which is the same "zero means not recorded" rule the field carries.
        void ForgetBytes(Store& a_s, const std::string& a_hash) {
            const auto it = a_s.manifest.entries.find(a_hash);
            if (it == a_s.manifest.entries.end() ||
                it->second.status != PreviewManifest::Status::kReady) {
                return;
            }
            a_s.bytes -= (std::min)(a_s.bytes, it->second.bytes);
        }

        // While the summed ready bytes exceed the budget, the oldest ready
        // entry loses its file. updatedTick is a plain counter persisted
        // through the entries, monotonic enough for an LRU and free of the
        // wall clock.
        void EvictToBudget(Store& a_s) {
            const std::uint64_t budget =
                static_cast<std::uint64_t>(Settings::GetSingleton().previewCacheMiB) *
                1024ull * 1024ull;
            while (a_s.bytes > budget) {
                std::string   victim;
                std::uint64_t oldest = ~0ull;
                for (const auto& [hash, e] : a_s.manifest.entries) {
                    if (e.status == PreviewManifest::Status::kReady &&
                        e.updatedTick < oldest) {
                        oldest = e.updatedTick;
                        victim = hash;
                    }
                }
                if (victim.empty()) {
                    break;
                }
                auto&           e = a_s.manifest.entries[victim];
                std::error_code ec;
                const auto      path = a_s.root / e.file;
                // The recorded size, not a fresh stat: this is the number that
                // was ADDED to the total, so it is the only number that can
                // take it back out exactly. Statting here also asked the
                // filesystem for a size it was about to delete anyway.
                ForgetBytes(a_s, victim);
                std::filesystem::remove(path, ec);
                a_s.manifest.entries.erase(victim);
                a_s.dirty = true;
            }
        }

    }  // namespace

    Hit Lookup(const std::string& a_diskKey) {
        auto& s = Get();
        Hit   hit;
        const auto it = s.manifest.entries.find(HashOf(a_diskKey));
        if (it == s.manifest.entries.end()) {
            return hit;
        }
        // ⚠ THE FULL KEY IS RE-COMPARED ON EVERY READ. A cache keyed on the
        // hash alone silently serves the wrong picture on a collision;
        // detected, it is merely a rebuild.
        if (it->second.key != a_diskKey) {
            if (!s.collisionSaid) {
                s.collisionSaid = true;
                spdlog::warn(
                    "PreviewDiskCache: FNV collision between '{}' and '{}'; the "
                    "second key rebuilds every session. Worth knowing, harmless.",
                    it->second.key, a_diskKey);
            }
            return hit;
        }
        switch (it->second.status) {
            case PreviewManifest::Status::kReady:
                hit.status = PreviewManifest::Status::kReady;
                hit.file   = s.root / it->second.file;
                break;
            case PreviewManifest::Status::kFailed:
                hit.status        = PreviewManifest::Status::kFailed;
                hit.failureReason = it->second.failureReason;
                break;
            case PreviewManifest::Status::kStale:
            case PreviewManifest::Status::kMissing:
                break;  // both read as missing, so the card re-renders
        }
        return hit;
    }

    std::filesystem::path PathFor(const std::string& a_diskKey) {
        auto& s = Get();
        return s.root / PreviewGrid::ShardPathFor(PreviewGrid::Fnv1a64(a_diskKey));
    }

    void MarkReady(const std::string& a_diskKey, std::uint32_t a_w, std::uint32_t a_h) {
        auto&      s    = Get();
        const auto hash = HashOf(a_diskKey);
        // BEFORE the overwrite, because after it the old size is gone. A card
        // re-rendered under the same key replaces a ready entry, and its file
        // is replaced on disk too, so the old bytes are no longer on the disk
        // OR in the entry and counting them twice is a pure invention.
        ForgetBytes(s, hash);
        PreviewManifest::Entry e;
        e.key         = a_diskKey;
        e.status      = PreviewManifest::Status::kReady;
        e.file        = PreviewGrid::ShardPathFor(PreviewGrid::Fnv1a64(a_diskKey));
        e.width       = a_w;
        e.height      = a_h;
        e.updatedTick = ++s.tick;
        // The one stat this module still makes, and it is the right one: the
        // file was just written, so this is where its size becomes known. Every
        // later reader takes it from the entry instead of asking again.
        std::error_code ec;
        if (const auto size = std::filesystem::file_size(s.root / e.file, ec); !ec) {
            e.bytes = size;
        }
        s.bytes += e.bytes;
        s.manifest.entries[hash] = std::move(e);
        s.dirty = true;
        EvictToBudget(s);
    }

    void MarkFailed(const std::string& a_diskKey, const std::string& a_reason) {
        auto&      s    = Get();
        const auto hash = HashOf(a_diskKey);
        // A key that was READY and now fails leaves the total holding bytes
        // for an entry that is no longer eligible for eviction, so the cache
        // would count storage it can never reclaim and evict healthy pictures
        // to make room for it.
        ForgetBytes(s, hash);
        PreviewManifest::Entry e;
        e.key           = a_diskKey;
        e.status        = PreviewManifest::Status::kFailed;
        e.failureReason = a_reason;
        e.updatedTick   = ++s.tick;
        s.manifest.entries[hash] = std::move(e);
        s.dirty = true;
    }

    void FlushIfDirty() {
        auto& s = Get();
        if (!s.dirty) {
            return;
        }
        const auto t0   = std::chrono::steady_clock::now();
        const auto text = PreviewManifest::ToText(s.manifest);
        const auto tmp  = std::filesystem::path{ ManifestPath(s) }.concat(L".tmp");
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                spdlog::warn("PreviewDiskCache: manifest write failed, staying dirty.");
                return;
            }
            out << text;
            if (!out) {
                spdlog::warn("PreviewDiskCache: manifest write failed, staying dirty.");
                return;
            }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, ManifestPath(s), ec);
        if (ec) {
            std::filesystem::remove(ManifestPath(s), ec);
            std::filesystem::rename(tmp, ManifestPath(s), ec);
        }
        if (ec) {
            spdlog::warn("PreviewDiskCache: manifest rename failed: {}.", ec.message());
            return;
        }
        s.dirty = false;
        spdlog::debug("PreviewDiskCache: disk-cache.manifest.save {} entries in "
                      "{:.1f} ms.",
                      s.manifest.entries.size(),
                      std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count());
    }

    void RebuildAll() {
        auto& s = Get();
        WipeImages(s);
        s.manifest.entries.clear();
        s.bytes = 0;
        s.tick  = 0;
        s.dirty = false;
        spdlog::info("PreviewDiskCache: rebuilt empty on request; thumbnails render "
                     "again as they are browsed.");
    }

}  // namespace OS::PreviewDiskCache
