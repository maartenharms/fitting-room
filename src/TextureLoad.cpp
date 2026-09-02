#include "TextureLoad.h"

#include "OverlayPlan.h"    // ToOverridePath, the one spelling rule for a relative path
#include "VersionCheck.h"   // IdOk

#include <Windows.h>  // the SEH vocabulary

#include <cstdint>

namespace OS::TextureLoad {

    namespace {

        // BSShaderTextureSet creation, the one REL surface here: SE 99886 is
        // the ctor CommonLib mallocs behind, AE 107172 the engine's own Create.
        // Same pair PreviewSwapCapture::kCreateId names; kept in one place so
        // the two never disagree about which id gates the loader.
        inline constexpr REL::RelocationID kCreateId{ 99886, 107172 };

        // The set's loader: vfunc slot 38 fills an out NiPointer for one
        // texture slot. The vendored header declares SetTexture on this slot
        // with a storing signature the engine body does not have, so the call
        // goes through the vtable with the real one.
        using LoadTexture_t = void(RE::BSTextureSet*, std::uint32_t,
                                   RE::NiPointer<RE::NiSourceTexture>&);

        // A DDS is third-party bytes; the frame holds no C++ objects (MSVC
        // forbids __try beside unwinding), the callee owns them all.
        __declspec(noinline) bool LoadTextureSEH(RE::BSTextureSet*                    a_set,
                                                 RE::NiPointer<RE::NiSourceTexture>* a_out,
                                                 std::uint32_t*                      a_code) {
            *a_code = 0;
            __try {
                auto* vtbl = *reinterpret_cast<std::uintptr_t**>(a_set);
                reinterpret_cast<LoadTexture_t*>(vtbl[38])(a_set, 0, *a_out);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                *a_code = static_cast<std::uint32_t>(GetExceptionCode());
                return false;
            }
        }

    }  // namespace

    bool LoaderOk() {
        static const bool ok = [] {
            const bool present = OS::VersionCheck::IdOk(kCreateId);
            if (!present) {
                spdlog::warn("TextureLoad: the BSShaderTextureSet create id is absent on this "
                             "runtime, so nothing can be loaded by path this session.");
            }
            return present;
        }();
        return ok;
    }

    std::string Rooted(std::string_view a_relative) {
        return "Data\\Textures\\" + OverlayPlan::ToOverridePath(a_relative);
    }

    RE::NiPointer<RE::NiSourceTexture> LoadRooted(const std::string& a_rooted) {
        RE::NiPointer<RE::NiSourceTexture> tex;
        if (!LoaderOk() || a_rooted.empty()) {
            return tex;
        }
        RE::NiPointer<RE::BSShaderTextureSet> set{ RE::BSShaderTextureSet::Create() };
        if (!set) {
            return tex;
        }
        // SetTexturePath copies.
        set->SetTexturePath(RE::BSTextureSet::Texture::kDiffuse, a_rooted.c_str());
        std::uint32_t code = 0;
        if (!LoadTextureSEH(set.get(), &tex, &code)) {
            spdlog::warn("TextureLoad: '{}' FAULTED with 0x{:08X}.", a_rooted, code);
            tex.reset();
        }
        return tex;
    }

    RE::NiPointer<RE::NiSourceTexture> LoadRelative(std::string_view a_relative) {
        return LoadRooted(Rooted(a_relative));
    }

}  // namespace OS::TextureLoad
