#pragma once

#include "assets/ModelAsset.h"
#include "assets/SceneMetadata.h"
#include "state/EngineState.h"

#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct SceneViewportCameraState
{
    float yaw = 0.8f;
    float pitch = 0.45f;
    float zoom = 1.0f;
};

struct SceneViewportResolvedModel
{
    const ModelAsset* asset = nullptr;
    std::filesystem::file_time_type write_time{};
};

using SceneViewportModelResolver = std::function<SceneViewportResolvedModel(const std::filesystem::path&)>;

class SceneViewportRenderer
{
public:
    bool Initialize(SDL_GPUDevice* device, SDL_GPUTextureFormat color_target_format);
    void Shutdown();
    void BeginFrame();
    void RenderUi(
        EngineState& state,
        const SceneMetadata& scene_metadata,
        const SceneViewportModelResolver& resolve_model_asset,
        SceneViewportCameraState& camera_state);
    void RenderGpu(SDL_GPUCommandBuffer* command_buffer);

public:
    struct GpuMeshSection
    {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material_index = 0;
    };

    struct GpuMeshCacheEntry
    {
        std::filesystem::file_time_type write_time{};
        SDL_GPUBuffer* vertex_buffer = nullptr;
        SDL_GPUBuffer* index_buffer = nullptr;
        std::vector<GpuMeshSection> sections;
        std::vector<SDL_GPUTexture*> material_textures;
    };

    struct GridCacheEntry
    {
        SDL_GPUBuffer* vertex_buffer = nullptr;
        SDL_GPUBuffer* index_buffer = nullptr;
        std::uint32_t index_count = 0;
        float spacing = 0.0f;
        float extent = 0.0f;
        float origin_x = 0.0f;
        float origin_z = 0.0f;
    };

    struct QueuedSceneObject
    {
        std::filesystem::path model_path;
        std::string name;
        SceneVector3 position = {0.0f, 0.0f, 0.0f};
        SceneVector3 rotation = {0.0f, 0.0f, 0.0f};
        SceneVector3 scale = {1.0f, 1.0f, 1.0f};
        SceneVector3 bounds_min = {0.0f, 0.0f, 0.0f};
        SceneVector3 bounds_max = {0.0f, 0.0f, 0.0f};
        bool has_bounds = false;
        bool selected = false;
    };

private:
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTextureFormat color_target_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUSampler* material_sampler_ = nullptr;
    SDL_GPUTexture* fallback_texture_ = nullptr;
    SDL_GPUTexture* color_texture_ = nullptr;
    SDL_GPUTexture* depth_texture_ = nullptr;
    std::uint32_t target_width_ = 0;
    std::uint32_t target_height_ = 0;
    bool render_requested_ = false;
    std::uint32_t gizmo_operation_ = 0;
    bool gizmo_local_mode_ = true;
    bool grid_enabled_ = false;
    float grid_spacing_ = 1.0f;
    float grid_origin_x_ = 0.0f;
    float grid_origin_z_ = 0.0f;
    float grid_extent_ = 16.0f;
    std::array<float, 16> view_projection_{};
    std::vector<QueuedSceneObject> queued_objects_;
    std::unordered_map<std::filesystem::path, GpuMeshCacheEntry> mesh_cache_;
    GridCacheEntry grid_cache_{};

    void ReleaseMeshCacheEntry(GpuMeshCacheEntry& entry);
    void ReleaseGridCacheEntry();
    void DestroyRenderTargets();
    bool EnsurePipeline();
    bool EnsureRenderTargets(std::uint32_t width, std::uint32_t height);
    bool EnsureMaterialResources();
    bool EnsureGridCacheEntry();
    bool EnsureMeshCacheEntry(const std::filesystem::path& model_path, const SceneViewportResolvedModel& resolved_model);
};