#pragma once

// CommonLibSSE-NG umbrella headers. alandtse's library keeps Win32 behind
// REX::W32 and never includes <Windows.h> itself; the logger's file sink and a
// dozen of this mod's own files do. Including it here, once, makes every later
// include a guarded no-op and gives one place to put its macros down.
#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <spdlog/sinks/basic_file_sink.h>

#include <Windows.h>

// <Windows.h> macros that rename this mod's own identifiers. GetObject would
// turn every InventoryEntryData::GetObject into GetObjectW, and rpcndr's small
// becomes char, which turns BodyMorphData's `small` field into `a_rule.char`
// in any file that pulls rpcndr.h in after this point. The .cpp files that
// include <Windows.h> themselves hit its include guard here, so nothing below
// brings these back.
//
// ⚠ far AND near STAY DEFINED. minwindef.h spells FAR as `far`, and
// guiddef.h's DEFINE_GUID reads `EXTERN_C const GUID FAR name`, so undefining
// them turns every GUID in d3d11.h and wincodec.h into a syntax error. A local
// of either name is renamed instead (OverlayBaseline.h had one).
#ifdef GetObject
#    undef GetObject
#endif
#ifdef small
#    undef small
#endif

// Matches the CommonLibSSE-NG template convention (""sv literals in the
// auto-generated plugin declaration).
using namespace std::literals;
