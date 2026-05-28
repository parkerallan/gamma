#pragma once

#include <array>
#include <string>
#include <vector>

#include <assimp/matrix4x4.h>

#include "assets/AnimatorControllerAsset.h"
#include "render/RuntimeAnimationCache.h"
#include "render/RuntimeRenderer.h"

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
    RuntimeRenderer::RuntimeAnimatorState& runtime_state,
    std::vector<aiMatrix4x4>& out_bone_matrices);
