#include "PCH.h"

#include "Inventory3DHooks.h"

#include "EditorWindow.h"
#include "HookSite.h"      // OwningModule, to name whoever else patched the entry
#include "VersionCheck.h"  // IdOk - membership, not "REL gave me an address"

#include <safetyhook.hpp>

#include <atomic>

namespace {

    // The Inventory3DManager appender: it adds a LoadedInventoryModel and hands
    // the node to UI3DSceneManager::AttachChild, VISIBLE. Two callers on each
    // runtime, verified by an exhaustive rel32 scan of .text rather than
    // assumed: UpdateMagic3D and the async load completion inside Render.
    //   AE 51772 rva 0x9281A0, called from 0x140927B13 and 0x140928973
    //   SE 50896 rva 0x888290, called from 0x140887C21 and 0x14088899A
    // ⚠ The SE id was derived STRUCTURALLY, not by offsetting the AE one: SE's
    // UpdateMagic3D was decompiled and the call with the same four-argument
    // shape and position was resolved back through the Address Library. Guessing
    // an id is how you end up calling a silent neighbour.
    constexpr REL::RelocationID kAppendModel{ 50896, 51772 };

    SafetyHookInline           g_hook{};
    std::atomic<bool>          g_installed{ false };
    std::atomic<std::uint32_t> g_hidden{ 0 };
    // ⚠⚠ HOW OFTEN THE DETOUR HAS RUN AT ALL, SEPARATELY FROM HOW OFTEN IT HID
    // SOMETHING. Logging only on a hide is the same instrumentation gap that
    // cost a field round on the UpdateMagic3D attempt: "never fired" and "fired
    // with nothing to do" read identically, and the 2026-08-08 05:32 run hit it
    // AGAIN. The first call always announces itself, so a silent log now means
    // the detour genuinely never ran.
    std::atomic<std::uint32_t> g_calls{ 0 };

    // ⚠ THE GATE IS THE SAME ONE EditorWindow'S REACTIVE HIDE USES, INCLUDING
    // THE SAM TERM. Two hiders of one preview that disagree about WHEN would
    // leave a window where one hides and the other expects to, and the SAM case
    // is exactly that window: the editor is open, but SAM framed the shot and
    // owns what is on screen, so Fitting Room does not touch the preview there.
    bool ShouldHide() {
        return OS::EditorWindow::IsOpen() && !OS::EditorWindow::OpenedFromSam();
    }

    void AppendModel(RE::Inventory3DManager* a_this, std::uintptr_t a_arg2,
                     std::uintptr_t a_arg3, void* a_arg4) {
        // ⚠ CALL THROUGH FIRST. The model does not exist until the engine has
        // appended and attached it, so there is nothing to hide before this
        // returns. This still lands inside Render's load-completion step, well
        // before the scene pass at the end of Render's own body, so the model is
        // hidden in the same frame it is created and never drawn visible.
        g_hook.call<void, RE::Inventory3DManager*, std::uintptr_t, std::uintptr_t, void*>(
            a_this, a_arg2, a_arg3, a_arg4);

        const auto calls = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (calls == 1) {
            spdlog::info("Inventory3D: the appender detour is LIVE (first call, editor {}). "
                         "This line existing at all is what separates 'never ran' from 'ran "
                         "with nothing to hide'.",
                         ShouldHide() ? "open" : "closed");
        }

        if (!a_this || !ShouldHide()) {
            return;
        }

        // Hide EVERY unhidden entry, not just the one just added. Clear3D only
        // ever reaches loadedModels[count-1], and the array is a ring of up to
        // seven attached models. ⚠ The raw kHidden bit rather than SetAppCulled,
        // because that bit is exactly what Clear3D itself sets, so this cannot
        // diverge from the engine's own idea of hidden and needs no second
        // Address Library id to go wrong. Same thread as the scene pass that
        // follows, so there is no race with it.
        // ⚠ GetFlags(), NOT the bare `flags` member: that member is compiled out
        // on the VR layout and Fitting Room ships one universal DLL.
        std::uint32_t hid = 0;
        for (auto& model : a_this->GetRuntimeData().loadedModels) {
            auto* node = model.spModel.get();
            if (node && !node->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
                node->GetFlags().set(RE::NiAVObject::Flag::kHidden);
                ++hid;
            }
        }
        const auto n = g_hidden.fetch_add(hid, std::memory_order_relaxed) + hid;
        // ⚠ Logged even when hid == 0, because "the appender ran with the editor
        // open and there was nothing visible" is a RESULT, not a non-event. It
        // is the line that says the engine hid it first, or that the model was
        // already cached and hidden, rather than that this code is dead.
        spdlog::info("Inventory3D: appender ran with the editor open (call {}), hid {} "
                     "model(s) at creation, {} this session.",
                     calls, hid, n);
    }

}  // namespace

namespace OS::Inventory3DHooks {

    void Install() {
        // ⚠ MEMBERSHIP FIRST, AND IT IS NOT THE SAME QUESTION AS "REL GAVE ME AN
        // ADDRESS". CommonLib's id2offset does a lower_bound and only fails when
        // an id runs past the END of the database, so an id that is simply
        // ABSENT resolves to its NEIGHBOUR silently and hooks the wrong
        // function. AE 51772 was verified present offline against
        // versionlib-1-6-1170-0.bin, and the same parser correctly reports the
        // known-absent 21890 as missing, which is what proves it can see absence
        // at all.
        if (!OS::VersionCheck::IdOk(kAppendModel)) {
            spdlog::error("Inventory3D: appender id {} is ABSENT from this build's Address "
                          "Library, so the preview is NOT hidden at creation. The editor falls "
                          "back to its own reactive hide, one frame late.",
                          kAppendModel.id());
            return;
        }

        const auto addr  = kAppendModel.address();
        const auto owner = OS::HookSite::OwningModule(addr);

        g_hook = safetyhook::create_inline(reinterpret_cast<void*>(addr),
                                           reinterpret_cast<void*>(&AppendModel));
        if (!g_hook) {
            spdlog::error("Inventory3D: could not detour the appender at 0x{:X} (currently "
                          "owned by {}). The editor falls back to its own reactive hide.",
                          addr, owner);
            return;
        }

        g_installed.store(true, std::memory_order_relaxed);
        spdlog::info("Inventory3D: item-preview appender detoured at 0x{:X} (id {}, entry owned "
                     "by {} before us). A model is hidden in the same frame it is created while "
                     "the editor is open, so it is never drawn visible.",
                     addr, kAppendModel.id(), owner);
    }

    bool Installed() {
        return g_installed.load(std::memory_order_relaxed);
    }

    std::uint32_t Hidden() {
        return g_hidden.load(std::memory_order_relaxed);
    }

}  // namespace OS::Inventory3DHooks
