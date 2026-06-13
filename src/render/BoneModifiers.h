#pragma once

#include <array>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <assimp/matrix4x4.h>

#include "assets/AnimatorControllerAsset.h"
#include "render/RuntimeAnimationCache.h"
#include "render/RuntimeRenderer.h"

// Resolves a Rigidbody bone collider's motion against static geometry.
// Given the box's animated world transform (column-major, translation in
// 12-14), its bone-local center offset, its world-space half extents, and the
// previous frame's resolved world-space center, returns the resolved world
// center: the animated target when the path is clear, otherwise clamped at
// the first surface hit (sweep), slid along it, and depenetrated. The sweep
// is what prevents tunneling through hollow mesh colliders.
using BoneCollisionResolveQuery = std::function<std::array<float, 3>(
    const std::array<float, 16>& world_box_colmajor,
    const std::array<float, 3>& box_center_local,
    const std::array<float, 3>& half_extents_world,
    const std::array<float, 3>& prev_center_world,
    bool has_prev)>;

// Samples a clip's bone matrices and applies the AnimatorBoneModifier
// list (currently spring-damper "jiggle" physics) on top of the result.
// Mirrors the signature SampleClipBoneMatrices used to have here so the
// RuntimeRenderer call site is unchanged.
bool SampleClipBoneMatricesWithPhysics(
    RuntimeAnimationModelCacheEntry& anim_cache_entry,
    const std::string& clip_name,
    float state_time_seconds,
    float frame_delta_seconds,
    const std::vector<AnimatorBoneModifier>& modifiers,
    const std::array<float, 16>& object_world_matrix,
    // World-space linear velocity of the owning object (units/s), taken from
    // the physics fixed-step snapshots ((curr - prev) / step) so it contains
    // no measured frame time. Drives the steady "trailing" offset of jiggle
    // springs under uniform motion; pass zeros for non-physics objects.
    const std::array<float, 3>& object_velocity,
    RuntimeRenderer::RuntimeAnimatorState& runtime_state,
    std::vector<aiMatrix4x4>& out_bone_matrices,
    // Optional: when non-null, filled with the model-space world transform
    // (column-major, translation in elements 12-14 to match
    // object_world_matrix) of every bone named by a Collision-type modifier.
    // Used to drive animated bone colliders. Bones not found are skipped.
    std::unordered_map<std::string, std::array<float, 16>>* out_bone_world_by_name = nullptr,
    // Optional: when set, each Rigidbody-mode Collision bone's motion is
    // swept against static geometry and clamped (child bones carried along)
    // so it stops at walls instead of clipping through.
    const BoneCollisionResolveQuery& collision_resolve_query = {});
