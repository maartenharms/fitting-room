#pragma once

// Where a legacy `NiGeometry` keeps its four blocks, measured off both game
// binaries rather than taken from CommonLib.
//
// A BSVersion 83 mesh loads as `NiTriShape`, not `BSTriShape`, and AE keeps
// that class live rather than converting it. Its data hangs off `NiGeometry`,
// and CommonLibSSE-NG's `RE/N/NiGeometry.h` gets that block WRONG: it declares
//
//     m_spPropertyState 0x110, m_spEffectState 0x118,
//     m_spSkinInstance  0x120, m_spModelData   0x128
//
// and the last two are SWAPPED against what both executables do. Reading
// `GetRuntimeData().m_spModelData` therefore hands back the skin instance and
// `m_spSkinInstance` hands back the geometry data, which is what took the game
// down at `MeshExtractor.cpp:115` on 2026-08-20 and returned 0xC0000005 on all
// 17 probe reads. The base 0x110 is right on both runtimes; only the order is
// not.
//
// ⚠⚠ MEASURED 2026-08-20 (OS-237), THREE FUNCTIONS AGREEING, BOTH BUILDS.
// `NiGeometry::SaveBinary` (AE 0x140D46290 id 71242, SE 0x140C7F250 id 69819)
// writes the four refs in NIF stream order, and nif.xml fixes what that order
// means: Data, Skin Instance, [Material Data], Shader Property, Alpha Property.
// The two decompiles are instruction-for-instruction the same shape, so SE and
// AE share this layout and no runtime branch is needed.
//
//     [this+0x120]  NiGeometryData*   the 1st ref, Data
//     [this+0x128]  NiSkinInstance*   the 2nd ref, Skin Instance
//     [this+0x118]  the 3rd ref, Shader Property
//     [this+0x110]  the 4th ref, Alpha Property
//
// `NiGeometry::LinkObject` (AE 0x140D460F0 id 71240) stores the same four in
// the same order, its 4th through vtable slot 39, which is a bare setter for
// `[this+0x110]`. `NiGeometry::RegisterStreamables` (AE 0x140D461F0) reads all
// four, and dereferences 0x120 with no null check while null-checking the
// other three: the Data ref is the one a geometry cannot be without.
// Independently, `FUN_140D466C0`, the vertex-transform stub shared by six
// geometry vtables, reads `[this+0x120]`, calls `GetActiveVertexCount` at byte
// offset 0x130 on it and walks `+0x28` and `+0x30`. Those are
// `NiGeometryData::vertex` and `::normal`, so 0x120 is the geometry data twice
// over, from a reader and from a writer.
//
// ⚠ VR IS NOT MEASURED and this declines there rather than guessing. CommonLib
// puts the block at 0x138 on VR, which is arithmetic off a bigger NiAVObject,
// and an unmeasured offset in this area is a crash rather than a wrong number.
//
// ⚠⚠ EVERY ACCESSOR HERE READS MEMORY WHOSE SHAPE A THIRD PARTY CHOSE, so the
// caller reads under SEH. These do the arithmetic and the null check; they
// cannot make a bad pointer safe.

#include "PCH.h"

#include <cstddef>
#include <cstdint>

namespace OS::LegacyGeometry {

    // The four `NiGeometry` members, SE and AE alike.
    inline constexpr std::ptrdiff_t kAlphaProperty  = 0x110;
    inline constexpr std::ptrdiff_t kShaderProperty = 0x118;
    inline constexpr std::ptrdiff_t kModelData      = 0x120;
    inline constexpr std::ptrdiff_t kSkinInstance   = 0x128;

    // `NiTriShapeData`, which CommonLib does not carry at all. Measured off its
    // `LoadBinary` at vtable slot 24 (AE 0x140D52470, SE 0x140C8A310 id 70128),
    // which reads the count then allocates exactly `count * 2` bytes for the
    // buffer: the indices are uint16 and the count is of POINTS, not triangles.
    // The two decompiles match, so this is one layout as well.
    inline constexpr std::ptrdiff_t kNumTrianglePoints = 0x70;  // std::uint32_t
    inline constexpr std::ptrdiff_t kTriangles         = 0x78;  // std::uint16_t*

    // False on VR, where none of the above was measured. Every accessor answers
    // null there, so a legacy shape is skipped rather than read at a guess.
    [[nodiscard]] inline bool LayoutIsMeasured() noexcept {
        return !REL::Module::IsVR();
    }

    template <class T>
    [[nodiscard]] inline T* MemberPtr(const void* a_object, std::ptrdiff_t a_offset) noexcept {
        if (!a_object || !LayoutIsMeasured()) {
            return nullptr;
        }
        return *reinterpret_cast<T* const*>(reinterpret_cast<const std::byte*>(a_object) +
                                            a_offset);
    }

    [[nodiscard]] inline RE::NiGeometryData* ModelData(const RE::NiGeometry* a_geometry) noexcept {
        return MemberPtr<RE::NiGeometryData>(a_geometry, kModelData);
    }

    [[nodiscard]] inline RE::NiSkinInstance* SkinInstance(
        const RE::NiGeometry* a_geometry) noexcept {
        return MemberPtr<RE::NiSkinInstance>(a_geometry, kSkinInstance);
    }

    // The 3rd stream ref. On a Skyrim-era file this is where a
    // `BSLightingShaderProperty` lands, and it is the same offset BSGeometry's
    // `properties[States::kEffect]` occupies, so `ResolveMaterial` asks the same
    // question of both. ⚠ WHAT CLASS ACTUALLY ARRIVES HERE IS UNMEASURED: a
    // legacy file may carry `NiMaterialProperty` and `NiTexturingProperty`
    // instead, which this renderer has never read. Check the RTTI name.
    [[nodiscard]] inline RE::NiProperty* ShaderProperty(
        const RE::NiGeometry* a_geometry) noexcept {
        return MemberPtr<RE::NiProperty>(a_geometry, kShaderProperty);
    }

    // The 4th stream ref, the alpha property.
    [[nodiscard]] inline RE::NiProperty* AlphaProperty(
        const RE::NiGeometry* a_geometry) noexcept {
        return MemberPtr<RE::NiProperty>(a_geometry, kAlphaProperty);
    }

    // The index buffer of a `NiTriShapeData`. The caller has already proven the
    // data's RTTI name, because these offsets mean nothing on a
    // `NiTriStripsData`, which stores strips rather than a triangle list.
    [[nodiscard]] inline std::uint32_t NumTrianglePoints(
        const RE::NiGeometryData* a_data) noexcept {
        if (!a_data || !LayoutIsMeasured()) {
            return 0;
        }
        return *reinterpret_cast<const std::uint32_t*>(
            reinterpret_cast<const std::byte*>(a_data) + kNumTrianglePoints);
    }

    [[nodiscard]] inline const std::uint16_t* Triangles(
        const RE::NiGeometryData* a_data) noexcept {
        return MemberPtr<const std::uint16_t>(a_data, kTriangles);
    }

}  // namespace OS::LegacyGeometry
