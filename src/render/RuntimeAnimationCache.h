#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/anim.h>
#include <assimp/matrix4x4.h>
#include <assimp/mesh.h>
#include <assimp/scene.h>

struct RuntimeSkinInfluence
{
    std::array<int, 4> bone_indices = {-1, -1, -1, -1};
    std::array<float, 4> bone_weights = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct RuntimeSkinnedMeshData
{
    const aiMesh* mesh = nullptr;
    aiMatrix4x4 bind_node_transform;
    std::vector<RuntimeSkinInfluence> influences;
};

struct RuntimeAnimationModelCacheEntry
{
    bool loaded = false;
    std::string error_message;
    std::filesystem::file_time_type write_time{};
    std::unique_ptr<Assimp::Importer> importer;
    const aiScene* scene = nullptr;
    aiMatrix4x4 global_inverse;
    std::unordered_map<std::string, std::size_t> bone_index_by_name;
    std::vector<aiMatrix4x4> bone_offsets;
    std::vector<RuntimeSkinnedMeshData> skinned_meshes;
    // Throttles std::filesystem::last_write_time so it does not stutter the frame.
    std::uint64_t next_disk_check_perf_ticks = 0;
    // Lazy node-name -> channel cache, built on first sample per animation.
    std::unordered_map<const aiAnimation*, std::unordered_map<std::string, const aiNodeAnim*>> channels_by_animation;
    // Pointer-keyed mirror used by the physics path; avoids per-node string allocs.
    std::unordered_map<const aiAnimation*, std::unordered_map<const aiNode*, const aiNodeAnim*>> channels_by_node_per_animation;
    // Pointer-keyed bone index lookup, built lazily once.
    std::unordered_map<const aiNode*, std::size_t> bone_index_by_node;
    bool bone_index_by_node_built = false;
};
