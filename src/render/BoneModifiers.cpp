#include "render/BoneModifiers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <assimp/anim.h>
#include <assimp/quaternion.h>
#include <assimp/scene.h>
#include <assimp/vector3.h>

namespace
{

// Small file-local copies of helpers that also exist in RuntimeRenderer.cpp.
// Kept local here so the public header doesn't have to expose them and the
// non-physics sampling path keeps using its own copies unchanged.

std::string ToDisplayString(const aiString& value)
{
    return value.length > 0 ? std::string(value.C_Str()) : std::string();
}

aiMatrix4x4 ComposeTransform(const aiVector3D& scale, const aiQuaternion& rotation, const aiVector3D& translation)
{
    aiMatrix4x4 scale_matrix;
    aiMatrix4x4::Scaling(scale, scale_matrix);
    aiMatrix4x4 rotation_matrix(rotation.GetMatrix());
    aiMatrix4x4 translation_matrix;
    aiMatrix4x4::Translation(translation, translation_matrix);
    return translation_matrix * rotation_matrix * scale_matrix;
}

template <typename TKey>
std::size_t FindKeyframeIndex(double time, unsigned int key_count, const TKey* keys)
{
    if (key_count <= 1) { return 0; }
    for (unsigned int index = 0; index + 1 < key_count; ++index)
    {
        if (time < keys[index + 1].mTime) { return index; }
    }
    return key_count - 2;
}

aiVector3D InterpolatePosition(double animation_time, const aiNodeAnim* channel)
{
    if (channel == nullptr || channel->mNumPositionKeys == 0) { return aiVector3D(0.0f, 0.0f, 0.0f); }
    if (channel->mNumPositionKeys == 1) { return channel->mPositionKeys[0].mValue; }
    const std::size_t index = FindKeyframeIndex(animation_time, channel->mNumPositionKeys, channel->mPositionKeys);
    const std::size_t next_index = index + 1;
    const double delta = channel->mPositionKeys[next_index].mTime - channel->mPositionKeys[index].mTime;
    const double factor = delta > 0.0 ? (animation_time - channel->mPositionKeys[index].mTime) / delta : 0.0;
    return channel->mPositionKeys[index].mValue + static_cast<float>(factor) * (channel->mPositionKeys[next_index].mValue - channel->mPositionKeys[index].mValue);
}

aiVector3D InterpolateScale(double animation_time, const aiNodeAnim* channel)
{
    if (channel == nullptr || channel->mNumScalingKeys == 0) { return aiVector3D(1.0f, 1.0f, 1.0f); }
    if (channel->mNumScalingKeys == 1) { return channel->mScalingKeys[0].mValue; }
    const std::size_t index = FindKeyframeIndex(animation_time, channel->mNumScalingKeys, channel->mScalingKeys);
    const std::size_t next_index = index + 1;
    const double delta = channel->mScalingKeys[next_index].mTime - channel->mScalingKeys[index].mTime;
    const double factor = delta > 0.0 ? (animation_time - channel->mScalingKeys[index].mTime) / delta : 0.0;
    return channel->mScalingKeys[index].mValue + static_cast<float>(factor) * (channel->mScalingKeys[next_index].mValue - channel->mScalingKeys[index].mValue);
}

aiQuaternion InterpolateRotation(double animation_time, const aiNodeAnim* channel)
{
    if (channel == nullptr || channel->mNumRotationKeys == 0) { return aiQuaternion(); }
    if (channel->mNumRotationKeys == 1) { return channel->mRotationKeys[0].mValue; }
    const std::size_t index = FindKeyframeIndex(animation_time, channel->mNumRotationKeys, channel->mRotationKeys);
    const std::size_t next_index = index + 1;
    const double delta = channel->mRotationKeys[next_index].mTime - channel->mRotationKeys[index].mTime;
    const double factor = delta > 0.0 ? (animation_time - channel->mRotationKeys[index].mTime) / delta : 0.0;
    aiQuaternion out;
    aiQuaternion::Interpolate(out, channel->mRotationKeys[index].mValue, channel->mRotationKeys[next_index].mValue, static_cast<float>(factor));
    out.Normalize();
    return out;
}

const aiAnimation* FindAnimationByName(const aiScene* scene, const std::string& clip_name)
{
    if (scene == nullptr || scene->mNumAnimations == 0) { return nullptr; }
    if (!clip_name.empty())
    {
        for (unsigned int index = 0; index < scene->mNumAnimations; ++index)
        {
            const aiAnimation* animation = scene->mAnimations[index];
            if (animation != nullptr && ToDisplayString(animation->mName) == clip_name)
            {
                return animation;
            }
        }
    }
    return scene->mAnimations[0];
}

// Per-node world transform plus parent pointer, recorded by the pointer-keyed
// hierarchy walk so the modifier pass can read animated world positions and
// recompose final bone matrices.
struct NodeWorldRecord
{
    aiMatrix4x4 world;
    const aiNode* parent = nullptr;
};

void EvaluateAnimationHierarchyWithWorldByPtr(
    const aiAnimation* animation,
    double animation_time,
    const aiNode* node,
    const aiNode* parent_node,
    const aiMatrix4x4& parent_transform,
    const std::unordered_map<const aiNode*, const aiNodeAnim*>& channels_by_node,
    std::unordered_map<const aiNode*, NodeWorldRecord>& out_world_by_node)
{
    aiMatrix4x4 node_transform = node->mTransformation;
    const auto channel_it = channels_by_node.find(node);
    if (channel_it != channels_by_node.end())
    {
        const aiNodeAnim* channel = channel_it->second;
        const aiVector3D scale = InterpolateScale(animation_time, channel);
        const aiQuaternion rotation = InterpolateRotation(animation_time, channel);
        const aiVector3D translation = InterpolatePosition(animation_time, channel);
        node_transform = ComposeTransform(scale, rotation, translation);
    }
    const aiMatrix4x4 global_transform = parent_transform * node_transform;
    out_world_by_node[node] = {global_transform, parent_node};
    for (unsigned int c = 0; c < node->mNumChildren; ++c)
    {
        EvaluateAnimationHierarchyWithWorldByPtr(animation, animation_time, node->mChildren[c], node, global_transform, channels_by_node, out_world_by_node);
    }
}

}  // namespace

bool SampleClipBoneMatricesWithPhysics(
    RuntimeAnimationModelCacheEntry& anim_cache_entry,
    const std::string& clip_name,
    float state_time_seconds,
    float frame_delta_seconds,
    const std::vector<AnimatorBoneModifier>& modifiers,
    const std::array<float, 16>& object_world_matrix,
    RuntimeRenderer::RuntimeAnimatorState& runtime_state,
    std::vector<aiMatrix4x4>& out_bone_matrices)
{
    if (anim_cache_entry.scene == nullptr)
    {
        return false;
    }

    // Tolerate a missing/empty clip: fall back to bind pose so jiggle still
    // runs (driven purely by object world-matrix motion).
    const aiAnimation* const animation = FindAnimationByName(anim_cache_entry.scene, clip_name);

    double animation_time = 0.0;
    static const std::unordered_map<const aiNode*, const aiNodeAnim*> kEmptyChannelsByNode;
    const std::unordered_map<const aiNode*, const aiNodeAnim*>* channels_by_node = &kEmptyChannelsByNode;
    if (animation != nullptr)
    {
        const double ticks_per_second = animation->mTicksPerSecond > 0.0 ? animation->mTicksPerSecond : 25.0;
        const double duration = animation->mDuration > 0.0 ? animation->mDuration : 1.0;
        animation_time = std::fmod(static_cast<double>(state_time_seconds) * ticks_per_second, duration);

        auto channels_it = anim_cache_entry.channels_by_node_per_animation.find(animation);
        if (channels_it == anim_cache_entry.channels_by_node_per_animation.end())
        {
            std::unordered_map<const aiNode*, const aiNodeAnim*> channels_by_node_map;
            channels_by_node_map.reserve(animation->mNumChannels);
            for (unsigned int channel_index = 0; channel_index < animation->mNumChannels; ++channel_index)
            {
                const aiNodeAnim* channel = animation->mChannels[channel_index];
                if (channel != nullptr)
                {
                    const aiNode* n = anim_cache_entry.scene->mRootNode->FindNode(channel->mNodeName);
                    if (n != nullptr)
                    {
                        channels_by_node_map.emplace(n, channel);
                    }
                }
            }
            channels_it = anim_cache_entry.channels_by_node_per_animation.emplace(animation, std::move(channels_by_node_map)).first;
        }
        channels_by_node = &channels_it->second;
    }

    std::unordered_map<const aiNode*, NodeWorldRecord> world_by_node;
    // Skeletons here are ~600 nodes; pre-reserve to avoid per-frame rehash.
    world_by_node.reserve(1024);
    EvaluateAnimationHierarchyWithWorldByPtr(
        animation,
        animation_time,
        anim_cache_entry.scene->mRootNode,
        nullptr,
        aiMatrix4x4(),
        *channels_by_node,
        world_by_node);

    // Fixed-timestep accumulator (1/120s) so simulation rate is decoupled
    // from frame rate. Cap substeps to avoid spiral of death after stalls.
    constexpr float kStep = 1.0f / 120.0f;
    constexpr int kMaxSteps = 4;
    runtime_state.physics_accumulator_seconds += (std::max)(0.0f, frame_delta_seconds);
    int steps = 0;
    while (runtime_state.physics_accumulator_seconds >= kStep && steps < kMaxSteps)
    {
        runtime_state.physics_accumulator_seconds -= kStep;
        ++steps;
    }
    if (runtime_state.physics_accumulator_seconds > kStep * static_cast<float>(kMaxSteps))
    {
        runtime_state.physics_accumulator_seconds = 0.0f;
    }
    const float dt = kStep * static_cast<float>(steps);

    // Per-bone simulation state. Stale persistent entries are pruned at end
    // using a parallel "used" bitset indexed into runtime_state.jiggle_states.
    std::unordered_map<const aiNode*, RuntimeRenderer::JiggleSimEntry*> state_by_node;
    std::vector<bool> jiggle_used(runtime_state.jiggle_states.size(), false);

    auto get_or_create_state = [&](const aiNode* n) -> RuntimeRenderer::JiggleSimEntry* {
        auto cached = state_by_node.find(n);
        if (cached != state_by_node.end())
        {
            return cached->second;
        }
        for (std::size_t i = 0; i < runtime_state.jiggle_states.size(); ++i)
        {
            auto& e = runtime_state.jiggle_states[i];
            if (e.bone_name.size() == n->mName.length &&
                std::memcmp(e.bone_name.data(), n->mName.C_Str(), n->mName.length) == 0)
            {
                jiggle_used[i] = true;
                state_by_node[n] = &e;
                return &e;
            }
        }
        RuntimeRenderer::JiggleSimEntry fresh;
        fresh.bone_name.assign(n->mName.C_Str(), n->mName.length);
        runtime_state.jiggle_states.push_back(std::move(fresh));
        jiggle_used.push_back(true);
        RuntimeRenderer::JiggleSimEntry* ptr = &runtime_state.jiggle_states.back();
        state_by_node[n] = ptr;
        return ptr;
    };

    // Decompose object world matrix (column-major, no shear) into per-axis
    // basis vectors + scales. Used to convert world-space deltas back into
    // the model space the bone hierarchy lives in.
    const std::array<float, 16>& M = object_world_matrix;
    auto v3_len = [](float x, float y, float z) {
        return std::sqrt(x * x + y * y + z * z);
    };
    const float sx = v3_len(M[0], M[1], M[2]);
    const float sy = v3_len(M[4], M[5], M[6]);
    const float sz = v3_len(M[8], M[9], M[10]);
    const float inv_sx = (sx > 1e-8f) ? (1.0f / sx) : 1.0f;
    const float inv_sy = (sy > 1e-8f) ? (1.0f / sy) : 1.0f;
    const float inv_sz = (sz > 1e-8f) ? (1.0f / sz) : 1.0f;
    const std::array<float, 3> bx{M[0] * inv_sx, M[1] * inv_sx, M[2] * inv_sx};
    const std::array<float, 3> by{M[4] * inv_sy, M[5] * inv_sy, M[6] * inv_sy};
    const std::array<float, 3> bz{M[8] * inv_sz, M[9] * inv_sz, M[10] * inv_sz};
    const std::array<float, 3> origin{M[12], M[13], M[14]};

    auto model_point_to_world = [&](float mx, float my, float mz) -> std::array<float, 3> {
        return {
            bx[0] * mx * sx + by[0] * my * sy + bz[0] * mz * sz + origin[0],
            bx[1] * mx * sx + by[1] * my * sy + bz[1] * mz * sz + origin[1],
            bx[2] * mx * sx + by[2] * my * sy + bz[2] * mz * sz + origin[2],
        };
    };
    auto world_vec_to_model = [&](float wx, float wy, float wz) -> std::array<float, 3> {
        return {
            (bx[0] * wx + bx[1] * wy + bx[2] * wz) * inv_sx,
            (by[0] * wx + by[1] * wy + by[2] * wz) * inv_sy,
            (bz[0] * wx + bz[1] * wy + bz[2] * wz) * inv_sz,
        };
    };

    // Result of simulate_bone, used to drive how the modifier DFS treats
    // descendants:
    //   Normal        : bake own_delta; pass own_delta*parent_delta to children.
    //   EdgeTranslate : no-parent edge case already mutated world_by_node;
    //                   do not re-bake, reset child parent_delta to identity.
    //   Skip          : nothing applied; descendants inherit parent_delta.
    enum class SimulateBoneResult { Normal, EdgeTranslate, Skip };

    // Spring in world space, then compute a pivot rotation around the parent.
    // Does not mutate world_by_node for the common case: returns the delta
    // via out_own_delta, the modifier walk accumulates it down the chain,
    // and one post-walk pass bakes each bone. Collapses the old O(N^2)
    // per-chain subtree-rotation loop to O(N).
    auto simulate_bone = [&](const aiNode* bone_node, const AnimatorBoneModifier& m,
                              const aiMatrix4x4& parent_delta, aiMatrix4x4& out_own_delta)
        -> SimulateBoneResult {
        out_own_delta = aiMatrix4x4();  // identity
        auto wit = world_by_node.find(bone_node);
        if (wit == world_by_node.end()) { return SimulateBoneResult::Skip; }
        RuntimeRenderer::JiggleSimEntry* sp = get_or_create_state(bone_node);
        if (sp == nullptr) { return SimulateBoneResult::Skip; }
        RuntimeRenderer::JiggleSimEntry& s = *sp;

        const aiMatrix4x4 corrected_bone = parent_delta * wit->second.world;
        const float mx = corrected_bone.a4;
        const float my = corrected_bone.b4;
        const float mz = corrected_bone.c4;
        const auto target_world = model_point_to_world(mx, my, mz);
        const float tx = target_world[0];
        const float ty = target_world[1];
        const float tz = target_world[2];

        if (!s.initialized)
        {
            s.sim_pos = {tx, ty, tz};
            s.sim_vel = {0.0f, 0.0f, 0.0f};
            s.initialized = true;
            return SimulateBoneResult::Skip;
        }

        const float strength = std::clamp(m.strength, 0.0f, 2.0f);

        if (steps > 0)
        {
            const float stiff_k = 180.0f * (std::max)(0.0f, m.stiffness);
            const float mass = (std::max)(0.001f, m.mass);
            // Damping is a critical-damping fraction. frac=1 yields no overshoot.
            const float damp_k = 2.0f * (std::max)(0.0f, m.damping) * std::sqrt(stiff_k / mass);
            const float drag_k = std::clamp(m.drag, 0.0f, 1.0f);

            std::array<float, 3> accel{0.0f, 0.0f, 0.0f};
            for (int a = 0; a < 3; ++a)
            {
                accel[a] = (target_world[a] - s.sim_pos[a]) * stiff_k / mass;
                accel[a] += m.gravity_dir[a] * m.gravity_scale * 9.81f;
            }
            for (int a = 0; a < 3; ++a)
            {
                // Clamp per-step multiplier to [0,1] so overdamped+large dt
                // can't flip velocity sign.
                const float decay = std::clamp(1.0f - damp_k * dt, 0.0f, 1.0f);
                s.sim_vel[a] *= decay;
                s.sim_vel[a] += accel[a] * dt;
                s.sim_vel[a] *= (1.0f - drag_k * dt);
                s.sim_pos[a] += s.sim_vel[a] * dt;
            }

            // Cone limit relative to parent's corrected world position.
            if (m.angle_limit_deg < 179.5f && wit->second.parent != nullptr)
            {
                auto pit = world_by_node.find(wit->second.parent);
                if (pit != world_by_node.end())
                {
                    const aiMatrix4x4 corrected_parent = parent_delta * pit->second.world;
                    const auto pw = model_point_to_world(corrected_parent.a4, corrected_parent.b4, corrected_parent.c4);
                    std::array<float, 3> ad{tx - pw[0], ty - pw[1], tz - pw[2]};
                    std::array<float, 3> sd{s.sim_pos[0] - pw[0], s.sim_pos[1] - pw[1], s.sim_pos[2] - pw[2]};
                    const float al = std::sqrt(ad[0] * ad[0] + ad[1] * ad[1] + ad[2] * ad[2]);
                    const float sl = std::sqrt(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
                    if (al > 1e-5f && sl > 1e-5f)
                    {
                        const std::array<float, 3> an{ad[0] / al, ad[1] / al, ad[2] / al};
                        const std::array<float, 3> sn{sd[0] / sl, sd[1] / sl, sd[2] / sl};
                        const float dot = std::clamp(an[0] * sn[0] + an[1] * sn[1] + an[2] * sn[2], -1.0f, 1.0f);
                        const float angle = std::acos(dot);
                        const float limit_rad = m.angle_limit_deg * (3.14159265f / 180.0f);
                        if (angle > limit_rad)
                        {
                            const float t = (angle - limit_rad) / angle;
                            std::array<float, 3> corrected{
                                sn[0] * (1.0f - t) + an[0] * t,
                                sn[1] * (1.0f - t) + an[1] * t,
                                sn[2] * (1.0f - t) + an[2] * t,
                            };
                            const float cl = std::sqrt(corrected[0] * corrected[0] + corrected[1] * corrected[1] + corrected[2] * corrected[2]);
                            if (cl > 1e-5f)
                            {
                                corrected[0] /= cl;
                                corrected[1] /= cl;
                                corrected[2] /= cl;
                                s.sim_pos[0] = pw[0] + corrected[0] * sl;
                                s.sim_pos[1] = pw[1] + corrected[1] * sl;
                                s.sim_pos[2] = pw[2] + corrected[2] * sl;
                            }
                        }
                    }
                }
            }
        }

        // Clamp the world-space spring offset to a fraction of the bone's
        // natural length so fast translation can't stretch the bone visibly.
        float ox = s.sim_pos[0] - tx;
        float oy = s.sim_pos[1] - ty;
        float oz = s.sim_pos[2] - tz;
        {
            float bone_world_len = 0.0f;
            auto pit_len = world_by_node.find(wit->second.parent);
            if (pit_len != world_by_node.end())
            {
                const aiMatrix4x4 corrected_pl = parent_delta * pit_len->second.world;
                const auto pw_w = model_point_to_world(corrected_pl.a4, corrected_pl.b4, corrected_pl.c4);
                const float dxl = tx - pw_w[0];
                const float dyl = ty - pw_w[1];
                const float dzl = tz - pw_w[2];
                bone_world_len = std::sqrt(dxl * dxl + dyl * dyl + dzl * dzl);
            }
            const float max_offset = (bone_world_len > 1e-4f) ? (bone_world_len * 0.6f) : 0.05f;
            const float off_len = std::sqrt(ox * ox + oy * oy + oz * oz);
            if (off_len > max_offset && off_len > 1e-6f)
            {
                const float scale = max_offset / off_len;
                ox *= scale;
                oy *= scale;
                oz *= scale;
                s.sim_pos[0] = tx + ox;
                s.sim_pos[1] = ty + oy;
                s.sim_pos[2] = tz + oz;
                // Kill the outward component of velocity so the next step
                // doesn't immediately re-stretch.
                const float vdot = s.sim_vel[0] * ox + s.sim_vel[1] * oy + s.sim_vel[2] * oz;
                const float o2 = ox * ox + oy * oy + oz * oz;
                if (vdot > 0.0f && o2 > 1e-8f)
                {
                    const float k = vdot / o2;
                    s.sim_vel[0] -= k * ox;
                    s.sim_vel[1] -= k * oy;
                    s.sim_vel[2] -= k * oz;
                }
            }
        }
        const float wdx = ox * strength;
        const float wdy = oy * strength;
        const float wdz = oz * strength;
        const auto md = world_vec_to_model(wdx, wdy, wdz);
        if (md[0] == 0.0f && md[1] == 0.0f && md[2] == 0.0f) { return SimulateBoneResult::Skip; }

        auto pit_apply = world_by_node.find(wit->second.parent);
        if (pit_apply == world_by_node.end())
        {
            // No parent in world_by_node (modifier root near scene root).
            // Mutate eagerly and skip the bake/propagation path.
            wit->second.world.a4 += md[0];
            wit->second.world.b4 += md[1];
            wit->second.world.c4 += md[2];
            return SimulateBoneResult::EdgeTranslate;
        }
        const aiMatrix4x4 corrected_parent_apply = parent_delta * pit_apply->second.world;
        const std::array<float, 3> ppos{corrected_parent_apply.a4, corrected_parent_apply.b4, corrected_parent_apply.c4};
        const std::array<float, 3> anim_v{mx - ppos[0], my - ppos[1], mz - ppos[2]};
        const std::array<float, 3> new_v{
            mx + md[0] - ppos[0],
            my + md[1] - ppos[1],
            mz + md[2] - ppos[2],
        };
        const float anim_len = std::sqrt(anim_v[0] * anim_v[0] + anim_v[1] * anim_v[1] + anim_v[2] * anim_v[2]);
        const float new_len = std::sqrt(new_v[0] * new_v[0] + new_v[1] * new_v[1] + new_v[2] * new_v[2]);
        if (anim_len <= 1e-5f || new_len <= 1e-5f) { return SimulateBoneResult::Skip; }
        const std::array<float, 3> an{anim_v[0] / anim_len, anim_v[1] / anim_len, anim_v[2] / anim_len};
        const std::array<float, 3> nn{new_v[0] / new_len, new_v[1] / new_len, new_v[2] / new_len};
        const float cos_a = std::clamp(an[0] * nn[0] + an[1] * nn[1] + an[2] * nn[2], -1.0f, 1.0f);
        const float angle = std::acos(cos_a);
        if (angle <= 1e-5f) { return SimulateBoneResult::Skip; }
        std::array<float, 3> axis{
            an[1] * nn[2] - an[2] * nn[1],
            an[2] * nn[0] - an[0] * nn[2],
            an[0] * nn[1] - an[1] * nn[0],
        };
        float axis_len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (axis_len <= 1e-6f) { return SimulateBoneResult::Skip; }
        axis[0] /= axis_len; axis[1] /= axis_len; axis[2] /= axis_len;
        const float c = std::cos(angle);
        const float si = std::sin(angle);
        const float tt = 1.0f - c;
        aiMatrix4x4 R;
        R.a1 = tt * axis[0] * axis[0] + c;
        R.a2 = tt * axis[0] * axis[1] - si * axis[2];
        R.a3 = tt * axis[0] * axis[2] + si * axis[1];
        R.a4 = 0.0f;
        R.b1 = tt * axis[0] * axis[1] + si * axis[2];
        R.b2 = tt * axis[1] * axis[1] + c;
        R.b3 = tt * axis[1] * axis[2] - si * axis[0];
        R.b4 = 0.0f;
        R.c1 = tt * axis[0] * axis[2] - si * axis[1];
        R.c2 = tt * axis[1] * axis[2] + si * axis[0];
        R.c3 = tt * axis[2] * axis[2] + c;
        R.c4 = 0.0f;
        R.d1 = 0.0f; R.d2 = 0.0f; R.d3 = 0.0f; R.d4 = 1.0f;
        aiMatrix4x4 t_neg; t_neg.a4 = -ppos[0]; t_neg.b4 = -ppos[1]; t_neg.c4 = -ppos[2];
        aiMatrix4x4 t_pos; t_pos.a4 =  ppos[0]; t_pos.b4 =  ppos[1]; t_pos.c4 =  ppos[2];
        out_own_delta = t_pos * R * t_neg;
        return SimulateBoneResult::Normal;
    };

    // Process each tagged modifier. When affects_children is set, walk the
    // chain depth-first, threading parent_delta through the DFS. One bake
    // pass at the end -- O(N) per modifier instead of O(N^2).
    struct ModifierBakeEntry { const aiNode* node; aiMatrix4x4 bake; };
    struct ModifierWalkFrame { const aiNode* node; aiMatrix4x4 parent_delta; };
    thread_local std::vector<ModifierWalkFrame> walk_stack;
    thread_local std::vector<ModifierBakeEntry> walk_bakes;
    const aiMatrix4x4 kIdentityDelta;
    for (const AnimatorBoneModifier& m : modifiers)
    {
        if (m.bone_name.empty() || m.strength <= 0.0001f) { continue; }
        const aiNode* root = anim_cache_entry.scene->mRootNode->FindNode(m.bone_name.c_str());
        if (root == nullptr) { continue; }

        walk_bakes.clear();

        if (!m.affects_children)
        {
            aiMatrix4x4 own_delta;
            const SimulateBoneResult r = simulate_bone(root, m, kIdentityDelta, own_delta);
            if (r == SimulateBoneResult::Normal)
            {
                walk_bakes.push_back({root, own_delta});
            }
        }
        else
        {
            walk_stack.clear();
            walk_stack.push_back({root, kIdentityDelta});
            while (!walk_stack.empty())
            {
                const ModifierWalkFrame frame = walk_stack.back();
                walk_stack.pop_back();
                aiMatrix4x4 own_delta;
                const SimulateBoneResult r = simulate_bone(frame.node, m, frame.parent_delta, own_delta);
                aiMatrix4x4 child_accum;
                switch (r)
                {
                    case SimulateBoneResult::Normal:
                        child_accum = own_delta * frame.parent_delta;
                        walk_bakes.push_back({frame.node, child_accum});
                        break;
                    case SimulateBoneResult::EdgeTranslate:
                        // Edge case already mutated world_by_node; descendants
                        // read the updated parent via their own lookup.
                        child_accum = aiMatrix4x4();
                        break;
                    case SimulateBoneResult::Skip:
                    default:
                        child_accum = frame.parent_delta;
                        break;
                }
                for (unsigned int c = 0; c < frame.node->mNumChildren; ++c)
                {
                    walk_stack.push_back({frame.node->mChildren[c], child_accum});
                }
            }
        }

        // Bake each chain bone's accumulated transform into world_by_node.
        for (const ModifierBakeEntry& be : walk_bakes)
        {
            auto it = world_by_node.find(be.node);
            if (it != world_by_node.end())
            {
                it->second.world = be.bake * it->second.world;
            }
        }
    }

    // Drop simulation state for bones no longer referenced this frame.
    if (!runtime_state.jiggle_states.empty())
    {
        std::size_t write = 0;
        for (std::size_t read = 0; read < runtime_state.jiggle_states.size(); ++read)
        {
            if (read < jiggle_used.size() && jiggle_used[read])
            {
                if (write != read)
                {
                    runtime_state.jiggle_states[write] = std::move(runtime_state.jiggle_states[read]);
                }
                ++write;
            }
        }
        runtime_state.jiggle_states.resize(write);
    }

    // Build a pointer-keyed bone index cache so the emit loop doesn't have
    // to allocate a std::string per node.
    if (!anim_cache_entry.bone_index_by_node_built)
    {
        anim_cache_entry.bone_index_by_node.reserve(anim_cache_entry.bone_index_by_name.size());
        for (const auto& kv : anim_cache_entry.bone_index_by_name)
        {
            const aiNode* n = anim_cache_entry.scene->mRootNode->FindNode(kv.first.c_str());
            if (n != nullptr)
            {
                anim_cache_entry.bone_index_by_node.emplace(n, kv.second);
            }
        }
        anim_cache_entry.bone_index_by_node_built = true;
    }

    // Emit final bone matrices from the (possibly modified) world transforms.
    out_bone_matrices.assign(anim_cache_entry.bone_offsets.size(), aiMatrix4x4());
    // Iterate the bone set (small) rather than all world_by_node entries.
    for (const auto& kv : anim_cache_entry.bone_index_by_node)
    {
        const std::size_t bone_index = kv.second;
        if (bone_index >= out_bone_matrices.size())
        {
            continue;
        }
        const auto wit = world_by_node.find(kv.first);
        if (wit == world_by_node.end())
        {
            continue;
        }
        out_bone_matrices[bone_index] =
            anim_cache_entry.global_inverse * wit->second.world * anim_cache_entry.bone_offsets[bone_index];
    }
    return true;
}
