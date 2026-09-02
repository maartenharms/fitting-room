#pragma once

#include "NpcAssignments.h"  // NpcKey

#include <optional>
#include <string>

// The ONE way to turn a live base NPC into the persistable {plugin,
// localFormID} identity assignments are keyed by. Engine-coupled, so it lives
// here rather than in NpcAssignments.h, which is deliberately pure logic.
namespace OS {

    // A base NPC's load-order-independent key, or nullopt when it has none.
    //
    // ⚠ RE::TESForm::GetLocalFormID() DEREFERENCES GetFile(0) WITH NO NULL
    // CHECK (CommonLibSSE-NG, RE/T/TESForm.h: `file->compileIndex` straight
    // off the call). A base NPC created at run time - a mod duplicating a base
    // record, an engine-templated actor - has no source file, so calling it on
    // one is an immediate access violation, not a wrong answer. PROVEN, do not
    // "simplify" this back to a bare GetLocalFormID():
    // crash-2026-07-29-18-42-41, FittingRoom.dll+0x48EBA
    // `movzx edx, byte ptr [rcx+0x478]` with rcx = 0 (TESFile::compileIndex),
    // base NPC 0xFF0008F0 on an actor from SeranaDeadSexyExpandedCults.esp,
    // reached through OutfitSession::DisplayBodyForNpc -> ClassifyTarget.
    //
    // nullopt is also the semantically right answer, not just the safe one: a
    // runtime base has no identity that survives a reload, so it can never own
    // a persisted assignment. Callers already treat a keyless target as inert.
    [[nodiscard]] inline std::optional<NpcKey> NpcKeyFor(RE::TESNPC* a_base) {
        if (!a_base) {
            return std::nullopt;
        }
        auto* file = a_base->GetFile(0);
        if (!file) {
            return std::nullopt;
        }
        return NpcKey{ std::string{ file->GetFilename() }, a_base->GetLocalFormID() };
    }

}  // namespace OS
