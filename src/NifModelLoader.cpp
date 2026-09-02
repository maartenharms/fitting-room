#include "NifModelLoader.h"

#include "VersionCheck.h"  // IdOk - membership, not "REL gave me an address"

#include <Windows.h>  // the SEH vocabulary; nothing else here includes it

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <memory>

namespace OS::NifModelLoader {

    namespace {

        // Wardrobe ships AE-only REL::ID(70324)/REL::ID(13169); Fitting Room
        // is one universal DLL, so both are SE/AE pairs. The SE ids were
        // derived STRUCTURALLY on 2026-08-09, not by offsetting: every
        // function RIP-referencing a class's vtable is a ctor/dtor candidate,
        // each reference maps to its containing function through the Address
        // Library, and the method had to recover Wardrobe's proven AE ids
        // before its SE answers were trusted. It did, exactly:
        //   NiStream ctor  SE 68971 rva 0xC59690 (vtbl ref fn+0x23)
        //                  AE 70324 rva 0xD1EF80 (vtbl ref fn+0x23, same)
        //   BSStream dtor  SE 13007 rva 0x14CC20  AE 13169 rva 0x195DC0
        //     (both install VTABLE_BSStream then destroy [this+0x620], the
        //      first BSStream-owned member past sizeof(NiStream) == 0x620)
        // ⚠ SE is DERIVED AND SHAPE-CHECKED, not field-run: the dev machine
        // is AE. The IdOk gate below is what makes a wrong id safe, and the
        // SE field check is delegated like every SE verify on this project.
        constexpr REL::RelocationID kNiStreamCtor{ 68971, 70324 };
        constexpr REL::RelocationID kBSStreamDtor{ 13007, 13169 };

        using NiStreamCtor = RE::NiStream* (*)(RE::NiStream*);
        using BSStreamDtor = void (*)(RE::NiStream*);

        // 0x638 is sizeof(RE::BSStream) and 0x620 sizeof(RE::NiStream), both
        // static_assert'd in this CommonLib, which is what makes the
        // hand-built stream below a construction rather than a guess.
        constexpr std::size_t  kStreamStorageSize = 0x638;
        constexpr std::size_t  kGuardSize         = 64;
        constexpr std::uint8_t kGuardByte         = 0xFD;

        std::atomic<bool> g_latched{ false };

        bool IdsOk() {
            // ⚠ MEMBERSHIP FIRST. CommonLib's id2offset does a lower_bound and
            // an ABSENT id resolves to its NEIGHBOUR with no error, so a bare
            // .address() on a wrong-runtime id constructs garbage and calls
            // it (Inventory3DHooks carries the whole argument). Checked and
            // logged once per session.
            static const bool ok = [] {
                const bool ctor = OS::VersionCheck::IdOk(kNiStreamCtor);
                const bool dtor = OS::VersionCheck::IdOk(kBSStreamDtor);
                if (!ctor || !dtor) {
                    spdlog::error(
                        "NifModelLoader: NiStream ctor id ok={} / BSStream dtor id ok={} "
                        "on this runtime, so the preview grid renders no thumbnails and "
                        "every card keeps its placeholder.",
                        ctor, dtor);
                }
                return ctor && dtor;
            }();
            return ok;
        }

        // MSVC forbids __try in a function with unwinding, so this holds no
        // C++ objects. A NIF is third-party bytes and Load1 walks them.
        __declspec(noinline) bool SafeLoad(RE::NiStream* a_stream,
                                           RE::NiBinaryStream* a_binary,
                                           std::uint32_t& a_code) {
            a_code = 0;
            __try {
                return a_stream->Load1(a_binary);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                a_code = static_cast<std::uint32_t>(GetExceptionCode());
                return false;
            }
        }

        // Wardrobe's StreamHolder, with the guard check moved BEFORE the dtor
        // and made fatal. A guard trip means the parse wrote past the stream,
        // the heap is already damaged, and running the dtor over it then
        // continuing to build previews is a gamble this subsystem refuses.
        class StreamHolder {
        public:
            StreamHolder(NiStreamCtor a_ctor, BSStreamDtor a_dtor, std::uintptr_t a_vtable)
                : dtor_(a_dtor) {
                if (!a_ctor || !a_dtor || a_vtable == 0) {
                    return;
                }
                storage_ = std::make_unique<std::uint8_t[]>(kStreamStorageSize + kGuardSize);
                std::memset(storage_.get(), 0, kStreamStorageSize);
                std::memset(storage_.get() + kStreamStorageSize, kGuardByte, kGuardSize);
                stream_ = reinterpret_cast<RE::NiStream*>(storage_.get());
                a_ctor(stream_);
                // The ctor installed NiStream's vtable; this object is a
                // BSStream by size and by contract, so it gets BSStream's.
                *reinterpret_cast<std::uintptr_t*>(storage_.get()) = a_vtable;
                valid_ = true;
            }

            ~StreamHolder() {
                if (!valid_) {
                    return;
                }
                const auto* guard = storage_.get() + kStreamStorageSize;
                for (std::size_t i = 0; i < kGuardSize; ++i) {
                    if (guard[i] != kGuardByte) {
                        g_latched.store(true, std::memory_order_release);
                        spdlog::error(
                            "NifModelLoader: guard byte {} overwritten behind the "
                            "stream, so the heap is already damaged. The preview "
                            "subsystem is DOWN for this session; cards keep their "
                            "placeholders and nothing else is touched.",
                            i);
                        break;
                    }
                }
                dtor_(stream_);
            }

            StreamHolder(const StreamHolder&)            = delete;
            StreamHolder& operator=(const StreamHolder&) = delete;

            [[nodiscard]] bool Valid() const { return valid_; }

            [[nodiscard]] bool Load(const std::string& a_path, std::uint32_t& a_code) const {
                if (!valid_) {
                    return false;
                }
                // BSResourceNiBinaryStream, not a filesystem read, so a mesh
                // packed in a BSA resolves exactly like a loose one.
                RE::BSResourceNiBinaryStream file(a_path.c_str());
                if (!file.good()) {
                    return false;
                }
                return SafeLoad(stream_, &file, a_code);
            }

            [[nodiscard]] RE::NiNode* Root() const {
                if (!valid_) {
                    return nullptr;
                }
                for (auto& object : stream_->topObjects) {
                    if (object) {
                        if (auto* node = object->AsNode()) {
                            return node;
                        }
                    }
                }
                return nullptr;
            }

        private:
            std::unique_ptr<std::uint8_t[]> storage_;
            RE::NiStream*                   stream_{ nullptr };
            BSStreamDtor                    dtor_{ nullptr };
            bool                            valid_{ false };
        };

        // Backslashes, and a leading "data\\" removed. BSResource is rooted at
        // Data, so this is as far as normalisation can go without knowing what
        // KIND of path it was handed.
        std::string NormalizeDataPath(std::string a_path) {
            std::replace(a_path.begin(), a_path.end(), '/', '\\');
            if (a_path.size() >= 5) {
                std::string prefix = a_path.substr(0, 5);
                std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                               [](unsigned char a_c) {
                                   return static_cast<char>(std::tolower(a_c));
                               });
                if (prefix == "data\\") {
                    a_path = a_path.substr(5);
                }
            }
            return a_path;
        }

        // A form's model path is authored without the meshes prefix and
        // sometimes with a data one; BSResource wants "meshes\\...".
        std::string NormalizeMeshPath(std::string a_path) {
            a_path = NormalizeDataPath(std::move(a_path));
            std::string lowered = a_path;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char a_c) {
                               return static_cast<char>(std::tolower(a_c));
                           });
            if (!lowered.empty() && lowered.rfind("meshes\\", 0) != 0) {
                a_path = "meshes\\" + a_path;
            }
            return a_path;
        }

    }  // namespace

    namespace {

        // Both entry points, differing only in how the path was rooted.
        RE::NiPointer<RE::NiNode> LoadNormalized(const std::string& a_modelPath,
                                                 bool a_meshRelative) {
        if (g_latched.load(std::memory_order_acquire) || !IdsOk() ||
            a_modelPath.empty()) {
            return {};
        }

        // Resolved once, after the IdOk gate: .address() on an absent id is
        // exactly the silent-neighbour call the gate exists to prevent.
        static const auto ctor =
            reinterpret_cast<NiStreamCtor>(kNiStreamCtor.address());
        static const auto dtor =
            reinterpret_cast<BSStreamDtor>(kBSStreamDtor.address());
        static const auto vtable = REL::Relocation<std::uintptr_t>{
            RE::VTABLE_BSStream[0]
        }.address();

        // ⚠ WHICH ROOT, AND THE CALLER SAYS SO RATHER THAN A GUESS. Almost
        // everything here is a form's model path, authored relative to
        // meshes\. BodySlide's REFERENCE meshes are not: they live at
        // CalienteTools\BodySlide\ShapeData\..., and re-rooting one under
        // meshes\ asks for a file that does not exist. Sniffing the leading
        // directory would work today and break on the first mod that ships a
        // mesh under a folder of the same name.
        const auto path = a_meshRelative ? NormalizeMeshPath(a_modelPath)
                                         : NormalizeDataPath(a_modelPath);

        StreamHolder stream(ctor, dtor, vtable);
        if (!stream.Valid()) {
            return {};
        }
        std::uint32_t code = 0;
        if (!stream.Load(path, code)) {
            if (code != 0) {
                spdlog::warn("NifModelLoader: SEH 0x{:08X} loading '{}'.", code, path);
            } else {
                spdlog::warn("NifModelLoader: could not load '{}'.", path);
            }
            return {};
        }
        auto* root = stream.Root();
        if (!root) {
            spdlog::warn("NifModelLoader: '{}' loaded without a root node.", path);
            return {};
        }
        // The NiPointer takes its reference BEFORE StreamHolder's dtor runs,
        // which is what keeps the tree alive when the stream goes away.
        return RE::NiPointer<RE::NiNode>(root);
        }

    }  // namespace

    RE::NiPointer<RE::NiNode> Load(const std::string& a_modelPath) {
        return LoadNormalized(a_modelPath, true);
    }

    RE::NiPointer<RE::NiNode> LoadDataRelative(const std::string& a_dataPath) {
        return LoadNormalized(a_dataPath, false);
    }

    bool Latched() {
        return g_latched.load(std::memory_order_acquire);
    }

}  // namespace OS::NifModelLoader
