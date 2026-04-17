#pragma once

#include "app/VulkanContext.h"

#include "imgui.h"

#include <filesystem>

class FontInfoRenderer
{
public:
    ~FontInfoRenderer();

    void Shutdown();
    ImTextureID GetFontPreview(const std::filesystem::path& path, VulkanContext* vulkan_context, float preview_size_pixels);
    int GetPreviewWidth() const { return cached_font_preview_width_; }
    int GetPreviewHeight() const { return cached_font_preview_height_; }

private:
    void ClearFontPreview();

    std::filesystem::path cached_font_preview_path_;
    std::filesystem::file_time_type cached_font_preview_write_time_{};
    VulkanContext* cached_font_preview_context_ = nullptr;
    VkImage cached_font_preview_image_ = VK_NULL_HANDLE;
    VkDeviceMemory cached_font_preview_memory_ = VK_NULL_HANDLE;
    VkImageView cached_font_preview_view_ = VK_NULL_HANDLE;
    VkSampler cached_font_preview_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet cached_font_preview_descriptor_set_ = VK_NULL_HANDLE;
    float cached_font_preview_size_pixels_ = 0.0f;
    int cached_font_preview_width_ = 0;
    int cached_font_preview_height_ = 0;
};