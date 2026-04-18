#pragma once

#include "app/VulkanContext.h"
#include "assets/ModelAsset.h"
#include "assets/SceneMetadata.h"
#include "render/Lighting.h"
#include "render/Raytracing.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

class RuntimeRenderer
{
public:
    bool Initialize(VulkanContext* context);
    void Shutdown();
    bool StartSession(
        const std::filesystem::path& project_root,
        const std::filesystem::path& scene_path,
        const ActiveSceneCameraSelection& active_camera,
        std::string* error_message = nullptr);
    bool RenderFrame(std::uint32_t target_width, std::uint32_t target_height, std::string* error_message = nullptr);

    VkImage GetOutputImage() const { return ray_tracing_.GetOutputImage(); }
    VkImageLayout GetOutputLayout() const { return ray_tracing_.GetOutputLayout(); }
    std::uint32_t GetOutputWidth() const { return ray_tracing_.GetOutputWidth(); }
    std::uint32_t GetOutputHeight() const { return ray_tracing_.GetOutputHeight(); }

    struct CachedModelAssetEntry
    {
        std::filesystem::file_time_type write_time{};
        ModelAsset asset{};
    };

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
    };

    struct GpuMaterialTextures
    {
        GpuTexture base_color{};
        GpuTexture metallic_roughness{};
        GpuTexture normal{};
        GpuTexture occlusion{};
        GpuTexture emissive{};
    };

    struct GpuMeshCacheEntry
    {
        std::filesystem::file_time_type write_time{};
        GpuBuffer vertex_buffer{};
        GpuBuffer index_buffer{};
        std::uint32_t vertex_count = 0;
        std::uint32_t index_count = 0;
        std::vector<GpuMeshSection> sections;
        std::vector<RayTracing::MaterialRecord> materials;
        std::vector<GpuMaterialTextures> material_textures;
    };

    struct QueuedSceneObject
    {
        std::filesystem::path model_path;
        std::string name;
        std::array<float, 16> model_matrix{};
    };

private:

    const CachedModelAssetEntry& GetModelAssetEntry(const std::filesystem::path& path);
    const SceneMetadata& GetSceneMetadata();
    void ReleaseBuffer(GpuBuffer& buffer);
    void ReleaseTexture(GpuTexture& texture);
    void ReleaseMeshCacheEntry(GpuMeshCacheEntry& entry);
    bool EnsureMeshCacheEntry(const std::filesystem::path& model_path, const CachedModelAssetEntry& model_asset_entry);
    bool BuildQueuedScene(
        const SceneMetadata& scene_metadata,
        const SceneObjectMetadata& active_camera_object,
        const SceneObjectCameraAttributes& active_camera,
        std::array<float, 16>& view_inverse,
        std::array<float, 16>& projection_inverse,
        ResolvedSceneLighting& lighting,
        std::string* error_message);
    bool SyncRayTracingScene(std::string* error_message);

    VulkanContext* vulkan_context_ = nullptr;
    RayTracing ray_tracing_{};
    std::filesystem::path project_root_;
    std::filesystem::path scene_path_;
    std::string active_camera_object_name_;
    std::size_t active_camera_attribute_index_ = 0;
    std::filesystem::path cached_scene_path_;
    std::filesystem::file_time_type cached_scene_write_time_{};
    SceneMetadata cached_scene_metadata_{};
    bool has_cached_scene_metadata_ = false;
    std::unordered_map<std::filesystem::path, CachedModelAssetEntry> model_asset_cache_;
    std::unordered_map<std::filesystem::path, GpuMeshCacheEntry> mesh_cache_;
    std::vector<QueuedSceneObject> queued_objects_;
};