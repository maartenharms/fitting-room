#pragma once

// The mannequin's pose, as arithmetic over transforms (OS-204 phase 2).
//
// ⚠⚠ PURE BECAUSE THIS IS THE HIGHEST-RISK ARITHMETIC IN THE PROJECT. The
// extractor has shipped garbage geometry twice, both times with a clean log
// and a green build: half-float position widths read as full, and dynamic tri
// shapes whose positions live in the morph buffer rather than the partition
// buffer. Nothing here touches the engine, so all of it is pinned by test
// before a single vertex moves.
//
// ## Where the pose comes from, and why there is no skeleton.nif
//
// A skinned NIF carries, per bone, `NiSkinData::BoneData::skinToBone`, which
// is the BIND INVERSE: it maps skin space into that bone's space. So the
// bone's rest transform is simply its inverse:
//
//     boneWorld_bind = Inverse(skinToBone)
//
// and the skinning matrix for an unposed mesh is
//
//     boneWorld_bind * skinToBone == identity
//
// which is exactly why today's extractor renders correct geometry while
// ignoring skinning entirely. That identity is this file's first test, and it
// pins Inverse and Multiply at the same time.
//
// ⚠ DO NOT REACH FOR `NiSkinInstance::bones` to get names or parents. It is
// null on a standalone load, and that is the whole reason this design reads
// skinToBone plus the authored table below rather than walking a hierarchy.
// Someone will try to "fix" it.
//
// ## The pose itself
//
// World-space joint rotations, each naming a pivot bone, an axis and an angle,
// applied to that bone and everything below it. Subtree membership comes from
// the authored parent map, not from the NIF, because the NIF's bone list is
// flat. A joint rotates about its own BIND position and the accumulation runs
// root-most first, so bending an elbow and then dropping the shoulder carries
// the already-bent elbow with it, which is what a hierarchy would have done.

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OS::MannequinPose {

    struct Vec3 {
        float x{ 0.0f };
        float y{ 0.0f };
        float z{ 0.0f };
    };

    // Row major: m[row][col], so a rotation applies as out[r] = sum_c m[r][c]*v[c].
    struct Mat3 {
        std::array<std::array<float, 3>, 3> m{ { { 1.0f, 0.0f, 0.0f },
                                                 { 0.0f, 1.0f, 0.0f },
                                                 { 0.0f, 0.0f, 1.0f } } };
    };

    // The engine's own transform shape: v' = trans + scale * (rot * v). Kept
    // as plain values rather than RE::NiTransform so this header compiles in
    // a test with no engine; the caller converts at the seam.
    struct Xform {
        Mat3  rot{};
        Vec3  trans{};
        float scale{ 1.0f };
    };

    [[nodiscard]] inline Vec3 Mul(const Mat3& a_m, const Vec3& a_v) {
        return { a_m.m[0][0] * a_v.x + a_m.m[0][1] * a_v.y + a_m.m[0][2] * a_v.z,
                 a_m.m[1][0] * a_v.x + a_m.m[1][1] * a_v.y + a_m.m[1][2] * a_v.z,
                 a_m.m[2][0] * a_v.x + a_m.m[2][1] * a_v.y + a_m.m[2][2] * a_v.z };
    }

    [[nodiscard]] inline Mat3 Mul(const Mat3& a_a, const Mat3& a_b) {
        Mat3 out;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                out.m[r][c] = a_a.m[r][0] * a_b.m[0][c] + a_a.m[r][1] * a_b.m[1][c] +
                              a_a.m[r][2] * a_b.m[2][c];
            }
        }
        return out;
    }

    [[nodiscard]] inline Mat3 Transpose(const Mat3& a_m) {
        Mat3 out;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                out.m[r][c] = a_m.m[c][r];
            }
        }
        return out;
    }

    // (a . b)(v) = a(b(v)). Expanded: the rotations compose, b's translation
    // rides through a's rotation and scale, and the scales multiply.
    [[nodiscard]] inline Xform Multiply(const Xform& a_a, const Xform& a_b) {
        Xform out;
        out.rot   = Mul(a_a.rot, a_b.rot);
        out.scale = a_a.scale * a_b.scale;
        const Vec3 t = Mul(a_a.rot, a_b.trans);
        out.trans = { a_a.trans.x + a_a.scale * t.x, a_a.trans.y + a_a.scale * t.y,
                      a_a.trans.z + a_a.scale * t.z };
        return out;
    }

    // ⚠ A ROTATION'S INVERSE IS ITS TRANSPOSE ONLY BECAUSE IT IS ORTHONORMAL,
    // which every bind transform in a NIF is. A degenerate scale answers the
    // identity rather than dividing by zero: a wrong pose is a wrong picture,
    // and a NaN one is a mesh scattered across the world.
    [[nodiscard]] inline Xform Inverse(const Xform& a_x) {
        if (std::fabs(a_x.scale) <= 1e-8f) {
            return Xform{};
        }
        Xform out;
        out.rot         = Transpose(a_x.rot);
        out.scale       = 1.0f / a_x.scale;
        const Vec3 rt   = Mul(out.rot, a_x.trans);
        out.trans       = { -out.scale * rt.x, -out.scale * rt.y, -out.scale * rt.z };
        return out;
    }

    // Rodrigues. The axis is normalised here rather than at every call site,
    // so an authored table can name a direction without normalising it.
    [[nodiscard]] inline Mat3 AxisAngle(const Vec3& a_axis, float a_radians) {
        const float len = std::sqrt(a_axis.x * a_axis.x + a_axis.y * a_axis.y +
                                    a_axis.z * a_axis.z);
        if (len <= 1e-8f) {
            return Mat3{};
        }
        const float x = a_axis.x / len, y = a_axis.y / len, z = a_axis.z / len;
        const float c = std::cos(a_radians), s = std::sin(a_radians);
        const float t = 1.0f - c;
        Mat3        out;
        out.m[0] = { t * x * x + c, t * x * y - s * z, t * x * z + s * y };
        out.m[1] = { t * x * y + s * z, t * y * y + c, t * y * z - s * x };
        out.m[2] = { t * x * z - s * y, t * y * z + s * x, t * z * z + c };
        return out;
    }

    // A rotation about a point rather than about the origin: translate the
    // pivot to the origin, rotate, put it back.
    [[nodiscard]] inline Xform RotationAbout(const Vec3& a_pivot, const Vec3& a_axis,
                                             float a_degrees) {
        Xform out;
        out.rot        = AxisAngle(a_axis, a_degrees * 3.14159265f / 180.0f);
        const Vec3 rp  = Mul(out.rot, a_pivot);
        out.trans      = { a_pivot.x - rp.x, a_pivot.y - rp.y, a_pivot.z - rp.z };
        out.scale      = 1.0f;
        return out;
    }

    // ---- the standard skeleton --------------------------------------------
    //
    // ⚠ THESE NAMES ARE IN THE FILE, which was the gate this design turned on.
    // femalebody_1.nif carries thirty of them and Vapor.nif carries
    // `NPC Head [Head]` and `NPC Spine2 [Spn2]`; every body and hair mod
    // shares them because the engine matches bones to the actor's skeleton by
    // name at attach. That is what lets an authored table address a mesh it
    // has never seen.
    //
    // Only the chain a pose needs is listed. A bone absent from this map is
    // its own root, which is the safe answer: it takes its own joint if one is
    // authored and inherits nothing, so an unknown bone can never be swung by
    // somebody else's rotation.
    struct ParentLink {
        std::string_view bone;
        std::string_view parent;
    };

    inline constexpr std::array<ParentLink, 20> kParents{ {
        { "NPC COM [COM ]", "NPC Root [Root]" },
        { "NPC Pelvis [Pelv]", "NPC COM [COM ]" },
        { "NPC Spine [Spn0]", "NPC COM [COM ]" },
        { "NPC Spine1 [Spn1]", "NPC Spine [Spn0]" },
        { "NPC Spine2 [Spn2]", "NPC Spine1 [Spn1]" },
        { "NPC Neck [Neck]", "NPC Spine2 [Spn2]" },
        { "NPC Head [Head]", "NPC Neck [Neck]" },
        { "NPC L Clavicle [LClv]", "NPC Spine2 [Spn2]" },
        { "NPC L UpperArm [LUar]", "NPC L Clavicle [LClv]" },
        { "NPC L Forearm [LLar]", "NPC L UpperArm [LUar]" },
        { "NPC L Hand [LHnd]", "NPC L Forearm [LLar]" },
        { "NPC R Clavicle [RClv]", "NPC Spine2 [Spn2]" },
        { "NPC R UpperArm [RUar]", "NPC R Clavicle [RClv]" },
        { "NPC R Forearm [RLar]", "NPC R UpperArm [RUar]" },
        { "NPC R Hand [RHnd]", "NPC R Forearm [RLar]" },
        { "NPC L Thigh [LThg]", "NPC Pelvis [Pelv]" },
        { "NPC L Calf [LClf]", "NPC L Thigh [LThg]" },
        { "NPC R Thigh [RThg]", "NPC Pelvis [Pelv]" },
        { "NPC R Calf [RClf]", "NPC R Thigh [RThg]" },
        { "NPC Root [Root]", "" },
    } };

    [[nodiscard]] inline std::string_view ParentOf(std::string_view a_bone) {
        for (const auto& link : kParents) {
            if (link.bone == a_bone) {
                return link.parent;
            }
        }
        return {};
    }

    // One authored rotation. The axes are the world's: +X to the character's
    // left, +Y the way they face, +Z up.
    struct Joint {
        std::string_view bone;
        Vec3             axis;
        float            degrees;
    };

    // ⚠ THE TUNING KNOBS LIVE HERE AND NOWHERE ELSE, and they are field
    // knobs: the right angles are whatever looks like a person standing, and
    // that is judged on a card rather than argued in a header. Sign
    // conventions are the only part worth stating, because getting one
    // backwards puts an arm through the ribs: a rotation about +Y swings a
    // limb across the body's width, about +X it swings forward and back.
    //
    // The bind pose already stands with its arms down (which is why the
    // skinning was dropped once and asked for again), so this is a small
    // settling rather than a new pose: shoulders in a little, elbows softened,
    // so the silhouette stops reading as a mannequin held at attention.
    inline constexpr std::array<Joint, 6> kIdleJoints{ {
        { "NPC L UpperArm [LUar]", { 0.0f, 1.0f, 0.0f }, -6.0f },
        { "NPC R UpperArm [RUar]", { 0.0f, 1.0f, 0.0f }, 6.0f },
        { "NPC L Forearm [LLar]", { 1.0f, 0.0f, 0.0f }, -10.0f },
        { "NPC R Forearm [RLar]", { 1.0f, 0.0f, 0.0f }, -10.0f },
        { "NPC L Clavicle [LClv]", { 0.0f, 1.0f, 0.0f }, -2.0f },
        { "NPC R Clavicle [RClv]", { 0.0f, 1.0f, 0.0f }, 2.0f },
    } };

    // ---- the matrices ------------------------------------------------------

    [[nodiscard]] inline const Joint* JointFor(std::span<const Joint> a_joints,
                                               std::string_view       a_bone) {
        for (const auto& j : a_joints) {
            if (j.bone == a_bone) {
                return &j;
            }
        }
        return nullptr;
    }

    // One skinning matrix per bone, in the caller's bone order.
    //
    //     posedWorld(b) = A(b) * boneWorld_bind(b)
    //     skin(b)       = posedWorld(b) * skinToBone(b)
    //
    // where A(b) is every authored joint from b's root-most ancestor down to b
    // itself, composed in that order. ⚠ WITH NO AUTHORED JOINTS EVERY MATRIX
    // COMES OUT IDENTITY, because boneWorld_bind is Inverse(skinToBone) and
    // the two cancel. That is the first test and it is the whole correctness
    // pin for the first commit: the posed mesh must be byte-identical to
    // today's until a joint is authored.
    [[nodiscard]] inline std::vector<Xform> SkinningMatrices(
        std::span<const std::string> a_boneNames, std::span<const Xform> a_skinToBone,
        std::span<const Joint> a_joints) {
        std::vector<Xform> out;
        const std::size_t  count = (std::min)(a_boneNames.size(), a_skinToBone.size());
        out.reserve(count);

        // The pivots come from the BIND pose, and they are looked up by name
        // across the whole bone list rather than per bone, because a joint's
        // pivot may sit on a bone this mesh also skins to.
        const auto bindOf = [&](std::string_view a_bone) -> Xform {
            for (std::size_t i = 0; i < count; ++i) {
                if (a_boneNames[i] == a_bone) {
                    return Inverse(a_skinToBone[i]);
                }
            }
            return Xform{};
        };

        for (std::size_t i = 0; i < count; ++i) {
            // The ancestor chain, leaf first, then applied root-most first so
            // a shoulder carries an already-bent elbow.
            std::vector<std::string_view> chain;
            for (std::string_view name = a_boneNames[i]; !name.empty();) {
                chain.push_back(name);
                const auto parent = ParentOf(name);
                if (parent == name) {
                    break;  // a self-parent in the table cannot loop forever
                }
                name = parent;
            }
            Xform accum{};
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                if (const auto* joint = JointFor(a_joints, *it)) {
                    const Xform bind = bindOf(joint->bone);
                    accum = Multiply(accum, RotationAbout(bind.trans, joint->axis,
                                                          joint->degrees));
                }
            }
            const Xform bind = Inverse(a_skinToBone[i]);
            out.push_back(Multiply(Multiply(accum, bind), a_skinToBone[i]));
        }
        return out;
    }

    // A vertex under its blend weights. ⚠ NORMALISE: authored meshes do not
    // always sum to one, and a mesh whose weights sum to 0.9 shrinks toward
    // the origin by a tenth, which reads as a bad mesh rather than as bad
    // arithmetic.
    [[nodiscard]] inline Vec3 SkinPoint(const Vec3& a_v, std::span<const Xform> a_matrices,
                                        std::span<const std::uint32_t> a_indices,
                                        std::span<const float>         a_weights) {
        float total = 0.0f;
        for (const auto w : a_weights) {
            total += w;
        }
        if (total <= 1e-6f) {
            return a_v;  // an unweighted vertex stays where it was authored
        }
        Vec3        acc{};
        const auto  n = (std::min)(a_indices.size(), a_weights.size());
        for (std::size_t k = 0; k < n; ++k) {
            if (a_indices[k] >= a_matrices.size() || a_weights[k] == 0.0f) {
                continue;
            }
            const auto& x = a_matrices[a_indices[k]];
            const Vec3  r = Mul(x.rot, a_v);
            const float w = a_weights[k] / total;
            acc.x += w * (x.trans.x + x.scale * r.x);
            acc.y += w * (x.trans.y + x.scale * r.y);
            acc.z += w * (x.trans.z + x.scale * r.z);
        }
        return acc;
    }

}  // namespace OS::MannequinPose
