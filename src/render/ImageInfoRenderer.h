#pragma once

#include "app/VulkanContext.h"

#include "imgui.h"

#include <cstdint>
#include <filesystem>
#include <future>
#include <vector>

class ImageInfoRenderer
{
public:
    ~ImageInfoRenderer();

    void Shutdown();
    ImTextureID GetImagePreview(const std::filesystem::path& path, VulkanContext* vulkan_context);
    bool IsPreviewLoading(const std::filesystem::path& path) const;
    int GetPreviewWidth() const { return cached_image_preview_width_; }
    int GetPreviewHeight() const { return cached_image_preview_height_; }

private:
    struct DecodedPreviewResult
    {
        std::filesystem::path path;
        std::filesystem::file_time_type write_time{};
        std::vector<std::uint8_t> pixels;
        int width = 0;
        int height = 0;
        bool success = false;
    };

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

    std::future<DecodedPreviewResult> pending_preview_future_;
    bool pending_preview_active_ = false;
    std::filesystem::path pending_preview_path_;
    std::filesystem::file_time_type pending_preview_write_time_{};

    std::filesystem::path failed_preview_path_;
    std::filesystem::file_time_type failed_preview_write_time_{};
};