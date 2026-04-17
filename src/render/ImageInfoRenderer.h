#pragma once

#include "app/VulkanContext.h"

#include "imgui.h"

#include <filesystem>

class ImageInfoRenderer
{
public:
    ~ImageInfoRenderer();

    void Shutdown();
    ImTextureID GetImagePreview(const std::filesystem::path& path, VulkanContext* vulkan_context);
    int GetPreviewWidth() const { return cached_image_preview_width_; }
    int GetPreviewHeight() const { return cached_image_preview_height_; }

private:
    void ClearImagePreview();

    std::filesystem::path cached_image_preview_path_;
    std::filesystem::file_time_type cached_image_preview_write_time_{};
    VulkanContext* cached_image_preview_context_ = nullptr;
    VkImage cached_image_preview_image_ = VK_NULL_HANDLE;
    VkDeviceMemory cached_image_preview_memory_ = VK_NULL_HANDLE;
    VkImageView cached_image_preview_view_ = VK_NULL_HANDLE;
    VkSampler cached_image_preview_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet cached_image_preview_descriptor_set_ = VK_NULL_HANDLE;
    int cached_image_preview_width_ = 0;
    int cached_image_preview_height_ = 0;
};