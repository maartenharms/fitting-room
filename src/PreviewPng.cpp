#include "PreviewPng.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <system_error>

namespace OS::PreviewPng {

    namespace {
        using Microsoft::WRL::ComPtr;

        // COM on the render thread: the game (and FUCK's own WIC use) has
        // almost certainly initialised it, and RPC_E_CHANGED_MODE means
        // exactly "already initialised under the other threading model",
        // which is fine, WIC works under either. Never CoUninitialize for
        // the CHANGED_MODE case, so no uninit here at all.
        bool EnsureCom() {
            const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        }
    }  // namespace

    bool WriteRgba(const std::filesystem::path& a_dest,
                   const std::vector<std::uint8_t>& a_rgba, std::uint32_t a_width,
                   std::uint32_t a_height) {
        if (a_rgba.size() != static_cast<std::size_t>(a_width) * a_height * 4u) {
            spdlog::warn("PreviewPng: refusing a buffer of {} bytes for {}x{}.",
                         a_rgba.size(), a_width, a_height);
            return false;
        }
        if (!EnsureCom()) {
            spdlog::warn("PreviewPng: COM would not initialise, no PNG written.");
            return false;
        }

        // ⚠⚠ WRITTEN STRAIGHT TO THE DESTINATION, AND THE .tmp SIBLING IS GONE
        // WITH THE RENAME THAT FOLLOWED IT. Field CTD 2026-08-16, switching the
        // editor to male and opening the eyes page:
        //     EXCEPTION_ACCESS_VIOLATION at usvfs_x64.dll+0051BAB
        //     ?hook_MoveFileExW@usvfs@@YAHPEB_W0K@Z, lock xadd [rbx],esi
        //     FittingRoom.dll -> PreviewPng::WriteRgba (this line)
        //                     -> PreviewCache::BuildOne -> Drain
        // The fault is a locked increment on a rotten pointer INSIDE Mod
        // Organizer's MoveFileExW hook, reached from the rename this function
        // used to do. Our cache root is under the virtualised Data tree, so
        // both the source and the destination are rerouted by that hook, and a
        // card rebuild renames once per card: a sex switch invalidates the whole
        // head-part list and fires hundreds of them back to back, off the render
        // thread, while the game's own threads are doing their own file work.
        //
        // ⚠ THE ATOMICITY IT BOUGHT IS REPLACED RATHER THAN DROPPED. A torn PNG
        // can now only happen if the process dies mid-write, and the caller
        // loads every card back through FUCK immediately after this returns and
        // marks a file it cannot decode as failed, so the next session rebuilds
        // it instead of serving it. That is a self-healing rare case against a
        // crash we have now seen in the field.
        //
        // ⚠ THE OTHER RENAMES IN THIS REPO ARE LEFT ALONE ON PURPOSE. The
        // manifest, the outfit library and the rule seed each rename ONCE per
        // save rather than once per card, and a torn manifest costs a rebuild
        // rather than a wrong picture. If the same crash ever appears on one of
        // those stacks, this note is the precedent.
        std::error_code ec;
        std::filesystem::create_directories(a_dest.parent_path(), ec);

        ComPtr<IWICImagingFactory>    factory;
        ComPtr<IWICStream>            stream;
        ComPtr<IWICBitmapEncoder>     encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        // ⚠ ONE EXIT FOR EVERY FAILURE PAST THE OPEN, because the destination
        // IS the working file now. Every one of these paths used to be able to
        // return with the target half written; the temp file made that
        // somebody else's problem and there is no temp file any more.
        const auto abandon = [&](const char* a_why) {
            spdlog::warn("PreviewPng: {} for '{}'.", a_why, a_dest.string());
            frame.Reset();
            encoder.Reset();
            stream.Reset();
            std::error_code rmEc;
            std::filesystem::remove(a_dest, rmEc);
            return false;
        };

        if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(factory.GetAddressOf())))) {
            spdlog::warn("PreviewPng: no WIC factory.");
            return false;
        }
        if (FAILED(factory->CreateStream(stream.GetAddressOf())) ||
            FAILED(stream->InitializeFromFilename(a_dest.c_str(), GENERIC_WRITE))) {
            spdlog::warn("PreviewPng: could not open '{}'.", a_dest.string());
            return false;
        }
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                          encoder.GetAddressOf())) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) ||
            FAILED(frame->Initialize(nullptr)) ||
            FAILED(frame->SetSize(a_width, a_height))) {
            return abandon("encoder setup failed");
        }

        // ⚠ SetPixelFormat IS IN-OUT AND CAN SUBSTITUTE. Ask for BGRA, then
        // CHECK THE RETURNED GUID rather than the HRESULT alone: a
        // substituted format written as if it were BGRA is a corrupted image
        // with a clean log. The spec prices this hazard by name.
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&fmt)) ||
            !IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
            return abandon("BGRA was refused");
        }

        // RGBA (the readback) to BGRA (what WIC takes): one swizzled copy.
        std::vector<std::uint8_t> bgra(a_rgba.size());
        for (std::size_t i = 0; i + 3 < a_rgba.size(); i += 4) {
            bgra[i + 0] = a_rgba[i + 2];
            bgra[i + 1] = a_rgba[i + 1];
            bgra[i + 2] = a_rgba[i + 0];
            bgra[i + 3] = a_rgba[i + 3];
        }
        const UINT stride = a_width * 4u;
        if (FAILED(frame->WritePixels(a_height, stride, static_cast<UINT>(bgra.size()),
                                      bgra.data())) ||
            FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
            return abandon("encode failed");
        }
        // The frame and encoder each hold their own stream reference; the file
        // handle stays open until all three are gone, and the caller loads this
        // file back the moment we return. Release in reverse construction order
        // so the handle is closed before that read.
        frame.Reset();
        encoder.Reset();
        stream.Reset();
        return true;
    }

}  // namespace OS::PreviewPng
