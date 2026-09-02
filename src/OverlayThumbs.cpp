#include "OverlayThumbs.h"

#include "BuildChannel.h"
#include "FuckCompat.h"
#include "PreviewPng.h"
#include "WorkerGuard.h"  // no worker body may reach terminate

#include <DirectXTex.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace OS::OverlayThumbs {

    namespace {

        // ⚠ ONE SIZE, NOT Settings::previewThumbPx. The outfit cards are a
        // rendered scene and their size is a taste setting; these are flat art
        // and the picker's grid is built around a fixed tile. Sharing the
        // setting would let a large preview size quietly triple the decode cost
        // of a page nobody asked to be bigger.
        constexpr std::uint32_t kThumbPx = 128;

        enum class Stage : std::uint8_t {
            kQueued,   // wanted, worker has not finished
            kOnDisk,   // PNG written, waiting for a Pump to make it a handle
            kReady,
            kFailed,
        };

        struct Entry {
            Stage                 stage{ Stage::kQueued };
            Alpha                 alpha{ Alpha::kUnknown };
            std::filesystem::path png;
            FUCK::Image           image;
        };

        // ⚠ THE CUT SITS IN A MEASURED GAP, so its exact value cannot matter.
        // Of the 449 thumbnails this rig had decoded, 68 had not one pixel
        // below alpha 8 and the other 381 were over half transparent. Nothing
        // landed between 0 and 50 percent. Five percent is the middle of a hole
        // rather than a tuned number, and a texture that lands in that hole
        // some day is shown, because kArt is the answer that hides nothing.
        constexpr double kMaskClearFraction = 0.05;
        constexpr std::uint8_t kClearAlpha  = 8;
        // ⚠ THE WHITE MASK (OS-218): among the pixels that ARE painted, the
        // share that is near white. A tint mask carries its shape in the alpha
        // and its RGB is white or nearly so, and the compositor multiplies the
        // makeup colour in; a tattoo carries its ink in the RGB. Near white is
        // every channel at 200 or more and no more than 40 apart, which the
        // FANT box filter's edge blends stay inside for a white sheet and a
        // dark tattoo never reaches. Ninety percent, not all, because the
        // resize bleeds the transparent surround's colour into the edge
        // texels of a mask, and a mask is judged on its body, not its rim.
        constexpr double        kWhiteMaskFraction = 0.90;
        constexpr std::uint8_t  kWhiteMin          = 200;
        constexpr std::uint8_t  kWhiteSpread       = 40;
        // ⚠ THE CACHE'S OWN VERSION, IN THE KEY. The verdict rides in the PNG's
        // file name, so a thumbnail decoded before kWhiteMask existed carries a
        // verdict of kArt for a file this build would call kWhiteMask, and no
        // amount of reading the directory can tell. Bumping this re-decodes
        // every thumbnail once (a few thousand small decodes on the worker) and
        // every verdict is this build's. Bump it whenever ClassifyAlpha's
        // answer for a given picture can change.
        constexpr int kVerdictVersion = 2;

        std::mutex                                     g_lock;
        std::unordered_map<std::string, Entry>         g_entries;  // keyed by texture path
        std::deque<std::string>                        g_queue;
        std::condition_variable                        g_wake;
        bool                                           g_stopping{ false };
        std::unique_ptr<std::thread>                   g_worker;
        std::filesystem::path                          g_root;
        bool                                           g_rootResolved{ false };

        // Mirrors PreviewDiskCache::ResolveRoot, including the write probe. The
        // VFS is writable on this install and not on every install, and the
        // fallback is where FUCK puts a plugin's own config.
        std::filesystem::path ResolveRoot() {
            const auto      primary = BuildChannel::DataPath("OverlayThumbs");
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
                return primary;
            }
            char buf[MAX_PATH]{};
            FUCK::GetPluginConfigPath("FittingRoom", buf, sizeof buf);
            if (buf[0] != '\0') {
                const auto fallback = std::filesystem::path{ buf } / "OverlayThumbs";
                std::filesystem::create_directories(fallback, ec);
                return fallback;
            }
            return primary;
        }

        // ⚠ THE KEY CARRIES THE FILE'S OWN STAMP, NOT JUST ITS NAME. Two mods
        // ship art at the same relative path all the time, and the winner
        // changes with load order, so a name-only key would serve one mod's
        // thumbnail for another mod's texture for as long as the cache lived.
        // Size and write time settle it without reading the file.
        // [[cache-key-must-be-the-inputs-not-the-subject]] is the general form.
        [[nodiscard]] std::string KeyFrom(const std::string& a_texturePath,
                                          std::uint64_t a_size, std::uint64_t a_stamp) {
            std::string lowered = a_texturePath;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto hash = std::hash<std::string>{}(
                lowered + "|" + std::to_string(a_size) + "|" + std::to_string(a_stamp) + "|" +
                std::to_string(kThumbPx) + "|v" + std::to_string(kVerdictVersion));
            char buf[32]{};
            std::snprintf(buf, sizeof buf, "%016llx",
                          static_cast<unsigned long long>(hash));
            return std::string{ buf };
        }

        [[nodiscard]] std::string DiskKey(const std::string&           a_texturePath,
                                          const std::filesystem::path& a_source) {
            std::error_code sizeEc;
            const auto      rawSize = std::filesystem::file_size(a_source, sizeEc);
            const std::uint64_t size = sizeEc ? 0u : static_cast<std::uint64_t>(rawSize);
            std::error_code timeEc;
            const auto      wt = std::filesystem::last_write_time(a_source, timeEc);
            const std::uint64_t stamp =
                timeEc ? 0u : static_cast<std::uint64_t>(wt.time_since_epoch().count());
            return KeyFrom(a_texturePath, size, stamp);
        }

        // ⚠ AN ARCHIVED SOURCE HAS NO WRITE TIME TO ASK FOR, so the byte count
        // stands in for the pair. It is weaker than size-and-stamp and it is
        // still the thing that matters here: two packs shipping the same
        // relative path are what the stamp was defending against, and two DDS
        // files of byte-identical length that are different art is a much
        // narrower coincidence than two files of the same NAME. A repacked
        // archive whose art changed but whose size did not keeps a stale
        // thumbnail; clearing the cache folder is the escape hatch, and the
        // verdict version below is the one we control.
        [[nodiscard]] std::string DiskKey(const std::string& a_texturePath,
                                          std::size_t        a_archivedSize) {
            return KeyFrom(a_texturePath, static_cast<std::uint64_t>(a_archivedSize), 0u);
        }

        // ⚠ THE VERDICT RIDES IN THE FILE NAME, so a texture decoded in any
        // earlier session is known before this one draws a single card. The
        // alternative was a sidecar file or re-reading the PNG's pixels, and
        // both cost a read for something a directory entry can carry. It also
        // invalidates for free: the stem is the same hash as before, so a
        // changed source file gets a new name and a fresh verdict.
        [[nodiscard]] const char* SuffixFor(Alpha a_alpha) {
            switch (a_alpha) {
                case Alpha::kMask:      return "-mask.png";
                case Alpha::kWhiteMask: return "-white.png";
                case Alpha::kArt:
                case Alpha::kUnknown:
                    break;
            }
            return "-art.png";
        }

        [[nodiscard]] std::filesystem::path PngFor(const std::filesystem::path& a_root,
                                                   const std::string&           a_key,
                                                   Alpha                        a_alpha) {
            return a_root / (a_key + SuffixFor(a_alpha));
        }

        // Whether a decoded thumbnail is real overlay art, a tint mask with no
        // transparency, or a tint mask with a shape (white RGB under the alpha).
        [[nodiscard]] Alpha ClassifyAlpha(const std::vector<std::uint8_t>& a_rgba) {
            if (a_rgba.empty()) {
                return Alpha::kArt;
            }
            const std::size_t pixels = a_rgba.size() / 4;
            std::size_t       clear  = 0;
            std::size_t       white  = 0;
            for (std::size_t i = 0; i < pixels; ++i) {
                const auto* px = &a_rgba[i * 4];
                if (px[3] < kClearAlpha) {
                    ++clear;
                    continue;
                }
                const auto lo = (std::min)({ px[0], px[1], px[2] });
                const auto hi = (std::max)({ px[0], px[1], px[2] });
                if (lo >= kWhiteMin && hi - lo <= kWhiteSpread) {
                    ++white;
                }
            }
            const double fraction = static_cast<double>(clear) / static_cast<double>(pixels);
            if (fraction < kMaskClearFraction) {
                return Alpha::kMask;
            }
            const std::size_t painted = pixels - clear;
            if (painted != 0 &&
                static_cast<double>(white) / static_cast<double>(painted) >= kWhiteMaskFraction) {
                return Alpha::kWhiteMask;
            }
            return Alpha::kArt;
        }

        // Where the source DDS actually lives. The stored path is relative to
        // textures\, which is what the override wants and not what the disk
        // wants.
        [[nodiscard]] std::filesystem::path SourceFor(const std::string& a_texturePath) {
            std::string native = a_texturePath;
            std::replace(native.begin(), native.end(), '\\', '/');
            return std::filesystem::path{ "Data/textures" } / native;
        }

        // ⚠⚠ AND THE OTHER HALF OF THE LIBRARY IS NOT ON THE DISK AT ALL.
        // Community overlay packs ship inside a BSA, so SourceFor names a file
        // that is not there and every card off one of those packs would draw as
        // a failed thumbnail beside its own perfectly good row. The picker's
        // list learned to ask the engine on 2026-08-19; this is the same
        // question asked for the pixels.
        //
        // ⚠ THE WHOLE FILE, ONCE, AND ONLY WHEN THE DISK SAID NO. A thumbnail
        // is built once ever per source and then lives in the PNG cache, so the
        // read is paid on the first sight of a card and never again. The cap is
        // there because a 4K BC7 overlay is tens of megabytes and this runs on
        // a worker thread with no idea what it was handed.
        constexpr std::size_t kMaxArchivedDds = 128ull * 1024ull * 1024ull;

        [[nodiscard]] std::vector<std::uint8_t> ArchiveBytes(const std::string& a_texturePath) {
            std::string full = a_texturePath;
            std::replace(full.begin(), full.end(), '/', '\\');
            full = "textures\\" + full;
            RE::BSResourceNiBinaryStream stream{ full.c_str() };
            if (!stream.good() || !stream.stream) {
                return {};
            }
            const std::size_t total = stream.stream->totalSize;
            if (total == 0 || total > kMaxArchivedDds) {
                return {};
            }
            std::vector<std::uint8_t> bytes(total);
            if (!stream.read(reinterpret_cast<char*>(bytes.data()),
                             static_cast<std::uint32_t>(total))) {
                return {};
            }
            return bytes;
        }

        // The decode itself. No device, no context, no engine types: this runs
        // on the worker thread and nothing it touches belongs to another one.
        //
        // ⚠ SPLIT AT THE LOAD, so the two front doors below - a loose file and
        // an archived one - share every line after it. The resize, the row
        // pitch, the classification and the PNG name are the part that is easy
        // to get subtly wrong, and there is one copy of it.
        [[nodiscard]] bool BuildPngFrom(DirectX::ScratchImage&       loaded,
                                        const std::filesystem::path& a_root,
                                        const std::string&           a_key,
                                        std::filesystem::path&       a_dest,
                                        Alpha&                       a_alpha) {
            HRESULT hr = S_OK;
            const auto& meta = loaded.GetMetadata();
            const auto* top  = loaded.GetImage(0, 0, 0);
            if (!top) {
                return false;
            }

            // ⚠ DECOMPRESS AND CONVERT ARE DIFFERENT CALLS AND THE WRONG ONE IS
            // A NO-OP. Convert refuses a block compressed source, so a picker
            // built on Convert alone would work on the loose uncompressed art
            // and fail on every DXT and BC7 file, which is most of the library.
            DirectX::ScratchImage rgba;
            if (DirectX::IsCompressed(meta.format)) {
                hr = DirectX::Decompress(*top, DXGI_FORMAT_R8G8B8A8_UNORM, rgba);
            } else {
                hr = DirectX::Convert(*top, DXGI_FORMAT_R8G8B8A8_UNORM,
                                      DirectX::TEX_FILTER_DEFAULT,
                                      DirectX::TEX_THRESHOLD_DEFAULT, rgba);
            }
            if (FAILED(hr)) {
                return false;
            }

            // ⚠ FANT RATHER THAN THE DEFAULT FILTER, because the default routes
            // through WIC and WIC wants a COM apartment this thread has not
            // initialised. FANT is DirectXTex's own box filter and touches
            // nothing outside the process's heap.
            // ⚠ NOT NAMED small. <rpcndr.h>, which arrives with Windows.h
            // through the PCH, does `#define small char`, so that name turns
            // the declaration below into a syntax error several lines away from
            // anything that looks wrong.
            DirectX::ScratchImage scaled;
            hr = DirectX::Resize(*rgba.GetImage(0, 0, 0), kThumbPx, kThumbPx,
                                 DirectX::TEX_FILTER_FANT, scaled);
            if (FAILED(hr)) {
                return false;
            }
            const auto* out = scaled.GetImage(0, 0, 0);
            if (!out || !out->pixels) {
                return false;
            }

            // ⚠ ROW PITCH IS NOT WIDTH TIMES FOUR. DirectXTex pads rows, and a
            // straight copy of the whole buffer skews the picture into a
            // diagonal smear that still looks like an image, which is the sort
            // of wrong that survives a glance at a 128 pixel tile.
            std::vector<std::uint8_t> packed;
            packed.resize(static_cast<std::size_t>(kThumbPx) * kThumbPx * 4);
            for (std::uint32_t y = 0; y < kThumbPx; ++y) {
                std::memcpy(packed.data() + static_cast<std::size_t>(y) * kThumbPx * 4,
                            out->pixels + static_cast<std::size_t>(y) * out->rowPitch,
                            static_cast<std::size_t>(kThumbPx) * 4);
            }
            // ⚠ CLASSIFIED FROM THE SCALED THUMBNAIL RATHER THAN THE SOURCE,
            // and that is the cheap choice rather than a compromise. FANT is a
            // box filter, so a transparent region of the original is still
            // transparent here and a fully opaque sheet cannot acquire holes.
            // The alternative walks up to a 4K surface for an answer a 128
            // pixel one already gives.
            a_alpha = ClassifyAlpha(packed);
            a_dest  = PngFor(a_root, a_key, a_alpha);
            return PreviewPng::WriteRgba(a_dest, packed, kThumbPx, kThumbPx);
        }

        [[nodiscard]] bool BuildPng(const std::filesystem::path& a_source,
                                    const std::filesystem::path& a_root,
                                    const std::string&           a_key,
                                    std::filesystem::path&       a_dest,
                                    Alpha&                       a_alpha) {
            DirectX::ScratchImage loaded;
            if (FAILED(DirectX::LoadFromDDSFile(a_source.wstring().c_str(),
                                                DirectX::DDS_FLAGS_NONE, nullptr, loaded))) {
                return false;
            }
            return BuildPngFrom(loaded, a_root, a_key, a_dest, a_alpha);
        }

        // The same decode over bytes that never touched the filesystem.
        [[nodiscard]] bool BuildPngFromMemory(const std::vector<std::uint8_t>& a_bytes,
                                              const std::filesystem::path&     a_root,
                                              const std::string&               a_key,
                                              std::filesystem::path&           a_dest,
                                              Alpha&                           a_alpha) {
            DirectX::ScratchImage loaded;
            if (FAILED(DirectX::LoadFromDDSMemory(a_bytes.data(), a_bytes.size(),
                                                  DirectX::DDS_FLAGS_NONE, nullptr, loaded))) {
                return false;
            }
            return BuildPngFrom(loaded, a_root, a_key, a_dest, a_alpha);
        }

        // One queued thumbnail, start to finish. Split out of WorkerLoop so the
        // loop can guard exactly one item: see the ⚠⚠ on the call below.
        void BuildOne(const std::string& wanted) {
                const auto source = SourceFor(wanted);
                std::error_code ec;
                bool            ok = false;
                std::filesystem::path dest;
                Alpha                 alpha = Alpha::kUnknown;
                // ⚠ THE DISK FIRST, THE ARCHIVES ONLY IF IT SAID NO. Same order
                // and same reason as the picker's own backfill: a loose hit is
                // one stat call, and the read below is the whole file.
                const bool                loose = std::filesystem::exists(source, ec) && !ec;
                std::vector<std::uint8_t> archived;
                if (!loose) {
                    archived = ArchiveBytes(wanted);
                }
                if (loose || !archived.empty()) {
                    const auto key = loose ? DiskKey(wanted, source)
                                           : DiskKey(wanted, archived.size());
                    // Already built by an earlier session: the whole point of
                    // the disk cache is that this is paid once ever. Which of
                    // the two names is on disk is also the verdict, so a cached
                    // thumbnail costs no decode to classify.
                    for (const auto candidate : { Alpha::kArt, Alpha::kMask, Alpha::kWhiteMask }) {
                        const auto path = PngFor(g_root, key, candidate);
                        if (std::filesystem::exists(path, ec) && !ec) {
                            dest  = path;
                            alpha = candidate;
                            ok    = true;
                            break;
                        }
                    }
                    if (!ok) {
                        ok = loose ? BuildPng(source, g_root, key, dest, alpha)
                                   : BuildPngFromMemory(archived, g_root, key, dest, alpha);
                    }
                }

                std::lock_guard lock(g_lock);
                auto            it = g_entries.find(wanted);
                if (it == g_entries.end()) {
                    return;
                }
                if (ok) {
                    it->second.png   = dest;
                    it->second.alpha = alpha;
                    it->second.stage = Stage::kOnDisk;
                } else {
                    it->second.stage = Stage::kFailed;
                    spdlog::debug("OverlayThumbs: '{}' could not be read.", wanted);
                }
        }

        void WorkerLoop() {
            for (;;) {
                std::string wanted;
                {
                    std::unique_lock lock(g_lock);
                    g_wake.wait(lock, [] { return g_stopping || !g_queue.empty(); });
                    if (g_stopping) {
                        return;
                    }
                    wanted = std::move(g_queue.front());
                    g_queue.pop_front();
                }

                // ⚠⚠ GUARDED PER ITEM, NOT PER THREAD, AND THE DIFFERENCE IS THE
                // WHOLE POINT HERE. This worker services a queue for the life of
                // the process: wrapping it from the outside would let the first
                // unreadable texture end the thread, and every thumbnail
                // requested afterwards would sit at kPending forever behind a
                // worker that is never coming back. One bad item costs one
                // card. See WorkerGuard.h for what an unguarded throw does to
                // the process, and why this page in particular earned it: this
                // loop reads whatever art the player installed, and a path with
                // no mapping in the active code page throws on the spot.
                if (!WorkerGuard::RunOnce("OverlayThumbs", [&] { BuildOne(wanted); })) {
                    // ⚠ THE ROW MUST BE RETIRED OR THE GRID WAITS ON IT FOREVER.
                    // BuildOne is the only writer of this entry's stage, so a
                    // throw part-way leaves it at kPending and the card spins
                    // for the rest of the session.
                    std::lock_guard lock(g_lock);
                    auto            it = g_entries.find(wanted);
                    if (it != g_entries.end()) {
                        it->second.stage = Stage::kFailed;
                    }
                }
            }
        }

        void EnsureWorker() {
            if (!g_rootResolved) {
                g_rootResolved = true;
                g_root         = ResolveRoot();
                spdlog::info("OverlayThumbs: cache root '{}'.", g_root.string());
            }
            if (!g_worker) {
                g_worker = std::make_unique<std::thread>(WorkerLoop);
            }
        }

    }  // namespace

    ImTextureID Texture(const std::string& a_texturePath) {
        if (a_texturePath.empty()) {
            return static_cast<ImTextureID>(0);
        }
        std::lock_guard lock(g_lock);
        EnsureWorker();
        auto it = g_entries.find(a_texturePath);
        if (it == g_entries.end()) {
            g_entries.emplace(a_texturePath, Entry{});
            g_queue.push_back(a_texturePath);
            g_wake.notify_one();
            return static_cast<ImTextureID>(0);
        }
        if (it->second.stage == Stage::kReady && it->second.image.IsLoaded()) {
            return it->second.image.GetID();
        }
        return static_cast<ImTextureID>(0);
    }

    bool Failed(const std::string& a_texturePath) {
        std::lock_guard lock(g_lock);
        const auto      it = g_entries.find(a_texturePath);
        return it != g_entries.end() && it->second.stage == Stage::kFailed;
    }

    Alpha AlphaKind(const std::string& a_texturePath) {
        std::lock_guard lock(g_lock);
        const auto      it = g_entries.find(a_texturePath);
        // ⚠ NO QUEUEING FROM HERE, unlike Texture above. A caller that filters
        // on this walks the whole library every frame, and asking would queue
        // a decode for every texture in the load order whether or not its card
        // is anywhere near the screen.
        return it == g_entries.end() ? Alpha::kUnknown : it->second.alpha;
    }

    void Pump() {
        std::string           wanted;
        std::filesystem::path png;
        {
            std::lock_guard lock(g_lock);
            for (auto& [path, entry] : g_entries) {
                if (entry.stage == Stage::kOnDisk) {
                    wanted = path;
                    png    = entry.png;
                    break;  // at most one per present
                }
            }
        }
        if (wanted.empty()) {
            return;
        }

        // ⚠ CONSTRUCTED OUTSIDE THE LOCK. The image load is the expensive half
        // and it calls into FUCK, which draws on this same thread; holding the
        // lock across it would stall every card's Texture call behind one file
        // read for no benefit.
        FUCK::Image image(png.string().c_str());

        std::lock_guard lock(g_lock);
        auto            it = g_entries.find(wanted);
        if (it == g_entries.end()) {
            return;
        }
        if (image.IsLoaded()) {
            it->second.image = std::move(image);
            it->second.stage = Stage::kReady;
        } else {
            it->second.stage = Stage::kFailed;
            spdlog::debug("OverlayThumbs: '{}' built but would not load.", png.string());
        }
    }

    void ForgetAll() {
        std::lock_guard lock(g_lock);
        g_entries.clear();
        g_queue.clear();
    }

}  // namespace OS::OverlayThumbs
