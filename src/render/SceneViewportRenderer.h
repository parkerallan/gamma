#pragma once

#include "app/VulkanContext.h"
#include "assets/ModelAsset.h"
#include "assets/SceneMetadata.h"
#include "render/Lighting.h"
#include "render/Raytracing.h"
#include "state/EngineState.h"

#include <vulkan/vulkan.h>

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
    SceneVector3 pan_offset = {0.0f, 0.0f, 0.0f};
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
    bool Initialize(VulkanContext* context);
    void Shutdown();
    void BeginFrame();
    void RenderUi(
        EngineState& state,
        const SceneMetadata& scene_metadata,
        const SceneViewportModelResolver& resolve_model_asset,
        SceneViewportCameraState& camera_state);
    void RenderCameraPreview(
        const EngineState& state,
        const SceneMetadata& scene_metadata,
        const SceneViewportModelResolver& resolve_model_asset,
        const SceneObjectMetadata& camera_object,
        const SceneObjectCameraAttributes& camera_attributes);
    void RenderGpu();

public:
    struct GpuMeshSection
    {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material_index = 0;
        bool uses_alpha_transparency = false;
    };

    struct GpuBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        VkDeviceAddress device_address = 0;
    };

    struct GpuTexture
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    };

    struct GpuMeshCacheEntry
    {
        std::filesystem::file_time_type write_time{};
        GpuBuffer vertex_buffer{};
        GpuBuffer index_buffer{};
        std::uint32_t vertex_count = 0;
        std::uint32_t index_count = 0;
        std::vector<GpuMeshSection> sections;
        std::vector<SceneViewportRayTracing::MaterialRecord> materials;
        std::vector<GpuTexture> material_textures;
    };

    struct GridCacheEntry
    {
        GpuBuffer vertex_buffer{};
        GpuBuffer index_buffer{};
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
        SceneVector3 local_position = {0.0f, 0.0f, 0.0f};
        SceneVector3 local_rotation = {0.0f, 0.0f, 0.0f};
        SceneVector3 local_scale = {1.0f, 1.0f, 1.0f};
        SceneVector3 world_position = {0.0f, 0.0f, 0.0f};
        std::array<float, 16> model_matrix{};
        std::array<float, 16> parent_matrix{};
        bool has_parent_transform = false;
        SceneVector3 bounds_min = {0.0f, 0.0f, 0.0f};
        SceneVector3 bounds_max = {0.0f, 0.0f, 0.0f};
        bool has_bounds = false;
        bool selected = false;
    };

private:
    VulkanContext* vulkan_context_ = nullptr;
    SceneViewportRayTracing ray_tracing_{};
    VkPipelineLayout scene_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline scene_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout material_descriptor_set_layout_ = VK_NULL_HANDLE;
    VkSampler material_sampler_ = VK_NULL_HANDLE;
    GpuTexture fallback_texture_{};
    VkRenderPass offscreen_render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer offscreen_framebuffer_ = VK_NULL_HANDLE;
    VkImage offscreen_depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory offscreen_depth_memory_ = VK_NULL_HANDLE;
    VkImageView offscreen_depth_view_ = VK_NULL_HANDLE;
    VkImageLayout offscreen_depth_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t target_width_ = 0;
    std::uint32_t target_height_ = 0;
    bool render_requested_ = false;
    bool middle_mouse_panning_ = false;
    std::uint32_t gizmo_operation_ = 0;
    bool gizmo_local_mode_ = true;
    bool grid_enabled_ = false;
    float grid_spacing_ = 1.0f;
    float grid_origin_x_ = 0.0f;
    float grid_origin_z_ = 0.0f;
    float grid_extent_ = 16.0f;
    std::array<float, 16> view_inverse_{};
    std::array<float, 16> projection_inverse_{};
    std::array<float, 16> view_projection_{};
    ResolvedSceneLighting resolved_lighting_{};
    std::vector<QueuedSceneObject> queued_objects_;
    std::unordered_map<std::filesystem::path, GpuMeshCacheEntry> mesh_cache_;
    GridCacheEntry grid_cache_{};

    void ReleaseBuffer(GpuBuffer& buffer);
    void ReleaseTexture(GpuTexture& texture);
    void ReleaseMeshCacheEntry(GpuMeshCacheEntry& entry);
    void ReleaseGridCacheEntry();
    void DestroyRenderTargets();
    bool EnsurePipeline();
    bool EnsureRenderTargets(std::uint32_t width, std::uint32_t height);
    bool EnsureMaterialResources();
    bool EnsureGridCacheEntry();
    bool EnsureMeshCacheEntry(const std::filesystem::path& model_path, const SceneViewportResolvedModel& resolved_model);
    void SyncRayTracingScene();
};