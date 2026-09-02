#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// The two CPU-side vertex stores a facegen head hangs off its geometry, and
// the one derived reader both NpcHair and SculptProbe share. Hoisted out of
// NpcHair so the two probes cannot drift apart on the layout.
namespace OS::FaceGen {

    // The head's dynamic buffer is {x,y,z,w} floats per vertex, which is why
    // dynSize is always vertexCount * 16. Layout confirmed against NPC Visual
    // Editor, which reads the same buffer the same way. dynamicData is the
    // morphed OUTPUT; the morph pipeline regenerates it from the base vertex
    // data below, so a write here does not survive the next publish.
    struct DynVertex {
        float x, y, z, w;
    };

    // ⚠ THE LAYOUT IS CHECKSUMMED AT RUNTIME, NOT ASSUMED. CommonLib has the
    // RTTI constant for BSFaceGenBaseMorphExtraData and no class, and a
    // hand-derived layout is precisely how UpdateNeck spent four rounds
    // masquerading as a "base morph". The count field has to equal THIS
    // geometry's own vertex count before anything is dereferenced; the
    // original derivation checked six independent geometries with six
    // different counts (996, 1239, 226, 102, 141 and 176). If the gate fails
    // the reader returns nothing at all rather than following a garbage
    // pointer.
    constexpr std::size_t kFodPtrOffset   = 0x18;
    constexpr std::size_t kFodCountOffset = 0x20;

    // Three floats per vertex. If the real stride turns out to be wider, a
    // reader walks the first three quarters of the array and never runs off
    // the end, so a wrong guess here is a PARTIAL read rather than an
    // overrun.
    constexpr std::size_t kFodFloatsPerVertex = 3;

    struct FodView {
        float*        verts = nullptr;
        std::uint32_t count = 0;
        [[nodiscard]] explicit operator bool() const { return verts && count; }
    };

    inline FodView ResolveFod(const RE::BSGeometry* a_geom, std::uint16_t a_verts) {
        FodView v;
        if (!a_geom || a_verts == 0) {
            return v;
        }
        static const RE::BSFixedString key{ "FOD" };
        const auto* const              xd = a_geom->GetExtraData(key);
        if (!xd) {
            return v;
        }
        const auto* const raw = reinterpret_cast<const std::byte*>(xd);
        std::uintptr_t    ptr = 0;
        std::uint32_t     cnt = 0;
        std::memcpy(&ptr, raw + kFodPtrOffset, sizeof(ptr));
        std::memcpy(&cnt, raw + kFodCountOffset, sizeof(cnt));
        if (cnt != a_verts || ptr < 0x10000 || ptr > 0x7FFFFFFFFFFFULL) {
            return v;
        }
        v.verts = reinterpret_cast<float*>(ptr);
        v.count = cnt;
        return v;
    }

}  // namespace OS::FaceGen
