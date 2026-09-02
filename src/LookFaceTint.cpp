#include "LookFaceTint.h"

#include "PCH.h"
#include "SkinApi.h"  // the node-override write; the pooled material is not ours to touch

namespace OS::LookFaceTint {

    namespace {
        std::string g_jslot;
    }  // namespace

    void Hold(std::string a_jslot) { g_jslot = std::move(a_jslot); }

    bool Held(std::string& a_jslot) {
        if (g_jslot.empty()) {
            return false;
        }
        a_jslot = g_jslot;
        return true;
    }

    void Clear() { g_jslot.clear(); }

    std::string PathFor(const std::string& a_jslot) {
        if (a_jslot.empty()) {
            return {};
        }
        // ⚠ THE FOLDER IS skee's AND THE SHAPE IS THE EXPORTER'S. The late
        // rebake gate reads the other end of this same string
        // (HeadBuildHook's "chargen\\exported\\" find), so the two must agree;
        // they are kept as one literal here rather than two.
        return "Textures\\CharGen\\Exported\\" + a_jslot + ".dds";
    }

    bool Apply(RE::Actor* a_actor) {
        if (!a_actor || g_jslot.empty()) {
            return false;
        }
        const auto path = PathFor(g_jslot);
        if (path.empty()) {
            return false;
        }
        // ⚠⚠ std::filesystem IS BLIND TO ARCHIVES and a look's export is
        // ordinarily a loose file in the overwrite, but a packed one is a
        // legitimate install. Ask the resource system, which sees both.
        {
            RE::BSResourceNiBinaryStream stream{ path.c_str() };
            if (!stream.good()) {
                // ⚠ NOT AN ERROR, and deliberately not a warning either: a
                // look whose export the user deleted is a normal state, and
                // the owed rebake is the correct fallback for it. Once, so a
                // field log can tell "the file is gone" from "we never tried".
                static bool s_said = false;
                if (!s_said) {
                    s_said = true;
                    spdlog::info("LookFaceTint: '{}' names no readable file, so the face "
                                 "falls back to the rebake from this save's tint list. "
                                 "That is the right answer for a deleted export and the "
                                 "wrong one for a look that still has its detail.",
                                 path);
                }
                return false;
            }
        }
        return SkinApi::ApplyFaceTintFile(a_actor, path);
    }

}  // namespace OS::LookFaceTint
