#pragma once

#include "app/VulkanContext.h"
#include "assets/SceneMetadata.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

class SkyboxRenderer
{
public:
    ~SkyboxRenderer();

    bool Initialize(VulkanContext* context);
    void Shutdown();

    // Resolves and uploads the active scene skybox texture if needed.
    // Returns VK_NULL_HANDLE when no valid skybox is configured.
    VkImageView ResolveSkyboxView(const SceneMetadata& scene_metadata, const std::filesystem::path& project_root);
    float ResolveSkyboxRotationDegrees(const SceneMetadata& scene_metadata) const;

private:
    struct GpuTexture
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    VulkanContext* vulkan_context_ = nullptr;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::unordered_map<std::string, GpuTexture> texture_cache_;

    void ReleaseTexture(GpuTexture& texture);
    GpuTexture* GetOrLoadTexture(const std::filesystem::path& skybox_path, bool use_pak_streaming);
    bool UploadFloatTexture(
        const float* pixels_rgba,
        std::uint32_t width,
        std::uint32_t height,
        GpuTexture& out_texture);
};
