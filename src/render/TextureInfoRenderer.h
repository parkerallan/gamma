#pragma once

#include "app/VulkanContext.h"

#include "imgui.h"

#include <filesystem>

class TextureInfoRenderer
{
public:
    ~TextureInfoRenderer();

    void Shutdown();
    ImTextureID GetTexturePreview(const std::filesystem::path& path, VulkanContext* vulkan_context);
    int GetPreviewWidth() const { return cached_texture_preview_width_; }
    int GetPreviewHeight() const { return cached_texture_preview_height_; }

private:
    void ClearTexturePreview();

    std::filesystem::path cached_texture_preview_path_;
    std::filesystem::file_time_type cached_texture_preview_write_time_{};
    VulkanContext* cached_texture_preview_context_ = nullptr;
    VkImage cached_texture_preview_image_ = VK_NULL_HANDLE;
    VkDeviceMemory cached_texture_preview_memory_ = VK_NULL_HANDLE;
    VkImageView cached_texture_preview_view_ = VK_NULL_HANDLE;
    VkSampler cached_texture_preview_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet cached_texture_preview_descriptor_set_ = VK_NULL_HANDLE;
    int cached_texture_preview_width_ = 0;
    int cached_texture_preview_height_ = 0;
};