#include "render/ImageInfoRenderer.h"

#include "imgui_impl_vulkan.h"

#include <stb_image.h>
#include <tinyexr.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string>
#include <vector>

namespace
{
constexpr int kMaxPreviewDimension = 1024;

struct PreviewSize
{
    int width = 0;
    int height = 0;
};

PreviewSize ComputePreviewSize(int source_width, int source_height)
{
    if (source_width <= 0 || source_height <= 0)
    {
        return {};
    }

    const int max_dimension = (std::max)(source_width, source_height);
    if (max_dimension <= kMaxPreviewDimension)
    {
        return {source_width, source_height};
    }

    const float scale = static_cast<float>(kMaxPreviewDimension) / static_cast<float>(max_dimension);
    return {
        (std::max)(1, static_cast<int>(std::round(static_cast<float>(source_width) * scale))),
        (std::max)(1, static_cast<int>(std::round(static_cast<float>(source_height) * scale)))};
}

std::string GetLowerExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return extension;
}

bool IsAsyncPreviewExtension(const std::filesystem::path& path)
{
    const std::string extension = GetLowerExtension(path);
    return extension == ".hdr" || extension == ".exr";
}

std::uint8_t LinearToPreviewByte(float value)
{
    const float clamped_linear = (std::max)(0.0f, value);
    const float mapped = clamped_linear / (1.0f + clamped_linear);
    const float gamma = std::pow((std::max)(mapped, 0.0f), 1.0f / 2.2f);
    const float byte_value = std::round((std::clamp)(gamma, 0.0f, 1.0f) * 255.0f);
    return static_cast<std::uint8_t>(byte_value);
}

std::vector<std::uint8_t> ConvertFloatRgbaToPreviewBytes(const float* pixels, int width, int height)
{
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        return {};
    }

    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> converted(pixel_count * 4u, 0);
    for (std::size_t pixel_index = 0; pixel_index < pixel_count; ++pixel_index)
    {
        const std::size_t src_offset = pixel_index * 4u;
        converted[src_offset + 0] = LinearToPreviewByte(pixels[src_offset + 0]);
        converted[src_offset + 1] = LinearToPreviewByte(pixels[src_offset + 1]);
        converted[src_offset + 2] = LinearToPreviewByte(pixels[src_offset + 2]);
        converted[src_offset + 3] = static_cast<std::uint8_t>(
            std::round((std::clamp)(pixels[src_offset + 3], 0.0f, 1.0f) * 255.0f));
    }

    return converted;
}

std::vector<std::uint8_t> ConvertFloatRgbaToPreviewBytes(
    const float* pixels,
    int source_width,
    int source_height,
    int target_width,
    int target_height)
{
    if (pixels == nullptr || source_width <= 0 || source_height <= 0 || target_width <= 0 || target_height <= 0)
    {
        return {};
    }

    std::vector<std::uint8_t> converted(static_cast<std::size_t>(target_width) * static_cast<std::size_t>(target_height) * 4u, 0);
    const float x_scale = static_cast<float>(source_width) / static_cast<float>(target_width);
    const float y_scale = static_cast<float>(source_height) / static_cast<float>(target_height);
    for (int y = 0; y < target_height; ++y)
    {
        const int source_y = (std::min)(source_height - 1, static_cast<int>(y * y_scale));
        for (int x = 0; x < target_width; ++x)
        {
            const int source_x = (std::min)(source_width - 1, static_cast<int>(x * x_scale));
            const std::size_t src_offset = (static_cast<std::size_t>(source_y) * static_cast<std::size_t>(source_width) + static_cast<std::size_t>(source_x)) * 4u;
            const std::size_t dst_offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(target_width) + static_cast<std::size_t>(x)) * 4u;
            converted[dst_offset + 0] = LinearToPreviewByte(pixels[src_offset + 0]);
            converted[dst_offset + 1] = LinearToPreviewByte(pixels[src_offset + 1]);
            converted[dst_offset + 2] = LinearToPreviewByte(pixels[src_offset + 2]);
            converted[dst_offset + 3] = static_cast<std::uint8_t>(
                std::round((std::clamp)(pixels[src_offset + 3], 0.0f, 1.0f) * 255.0f));
        }
    }

    return converted;
}

std::vector<std::uint8_t> ResizeRgbaToPreviewBytes(
    const std::uint8_t* pixels,
    int source_width,
    int source_height,
    int target_width,
    int target_height)
{
    if (pixels == nullptr || source_width <= 0 || source_height <= 0 || target_width <= 0 || target_height <= 0)
    {
        return {};
    }

    if (source_width == target_width && source_height == target_height)
    {
        const std::size_t size = static_cast<std::size_t>(source_width) * static_cast<std::size_t>(source_height) * 4u;
        return std::vector<std::uint8_t>(pixels, pixels + size);
    }

    std::vector<std::uint8_t> resized(static_cast<std::size_t>(target_width) * static_cast<std::size_t>(target_height) * 4u, 0);
    const float x_scale = static_cast<float>(source_width) / static_cast<float>(target_width);
    const float y_scale = static_cast<float>(source_height) / static_cast<float>(target_height);
    for (int y = 0; y < target_height; ++y)
    {
        const int source_y = (std::min)(source_height - 1, static_cast<int>(y * y_scale));
        for (int x = 0; x < target_width; ++x)
        {
            const int source_x = (std::min)(source_width - 1, static_cast<int>(x * x_scale));
            const std::size_t src_offset = (static_cast<std::size_t>(source_y) * static_cast<std::size_t>(source_width) + static_cast<std::size_t>(source_x)) * 4u;
            const std::size_t dst_offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(target_width) + static_cast<std::size_t>(x)) * 4u;
            resized[dst_offset + 0] = pixels[src_offset + 0];
            resized[dst_offset + 1] = pixels[src_offset + 1];
            resized[dst_offset + 2] = pixels[src_offset + 2];
            resized[dst_offset + 3] = pixels[src_offset + 3];
        }
    }

    return resized;
}

bool LoadPreviewPixels(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& pixels,
    int& width,
    int& height)
{
    width = 0;
    height = 0;
    pixels.clear();

    const std::string extension = GetLowerExtension(path);
    if (extension == ".exr")
    {
        float* exr_pixels = nullptr;
        const char* error_message = nullptr;
        int result = LoadEXR(&exr_pixels, &width, &height, path.string().c_str(), &error_message);
        if (result != TINYEXR_SUCCESS || exr_pixels == nullptr)
        {
            if (error_message != nullptr)
            {
                FreeEXRErrorMessage(error_message);
            }
            return false;
        }

        const PreviewSize preview_size = ComputePreviewSize(width, height);
        pixels = ConvertFloatRgbaToPreviewBytes(exr_pixels, width, height, preview_size.width, preview_size.height);
        width = preview_size.width;
        height = preview_size.height;
        std::free(exr_pixels);
        return !pixels.empty();
    }

    if (extension == ".hdr")
    {
        int channels = 0;
        float* hdr_pixels = stbi_loadf(path.string().c_str(), &width, &height, &channels, 4);
        if (hdr_pixels == nullptr)
        {
            return false;
        }

        const PreviewSize preview_size = ComputePreviewSize(width, height);
        pixels = ConvertFloatRgbaToPreviewBytes(hdr_pixels, width, height, preview_size.width, preview_size.height);
        width = preview_size.width;
        height = preview_size.height;
        stbi_image_free(hdr_pixels);
        return !pixels.empty();
    }

    int channels = 0;
    unsigned char* ldr_pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (ldr_pixels == nullptr)
    {
        return false;
    }

    const PreviewSize preview_size = ComputePreviewSize(width, height);
    pixels = ResizeRgbaToPreviewBytes(ldr_pixels, width, height, preview_size.width, preview_size.height);
    width = preview_size.width;
    height = preview_size.height;
    stbi_image_free(ldr_pixels);
    return !pixels.empty();
}

std::uint32_t FindMemoryType(VkPhysicalDevice physical_device, std::uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index)
    {
        const bool type_matches = (type_filter & (1u << index)) != 0;
        const bool properties_match = (memory_properties.memoryTypes[index].propertyFlags & properties) == properties;
        if (type_matches && properties_match)
        {
            return index;
        }
    }

    return UINT32_MAX;
}

bool CreateBuffer(
    VkPhysicalDevice physical_device,
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& buffer,
    VkDeviceMemory& memory)
{
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = FindMemoryType(physical_device, requirements.memoryTypeBits, properties);
    if (allocate_info.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkAllocateMemory(device, &allocate_info, nullptr, &memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindBufferMemory(device, buffer, memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyBuffer(device, buffer, nullptr);
        memory = VK_NULL_HANDLE;
        buffer = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

bool CreateImage(
    VkPhysicalDevice physical_device,
    VkDevice device,
    std::uint32_t width,
    std::uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& image_view)
{
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = vkCreateImage(device, &image_info, nullptr, &image);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(device, image, &requirements);

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = FindMemoryType(physical_device, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate_info.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    result = vkAllocateMemory(device, &allocate_info, nullptr, &memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindImageMemory(device, image, memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        memory = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(device, &view_info, nullptr, &image_view);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        image_view = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

void TransitionImageLayout(
    VkCommandBuffer command_buffer,
    VkImage image,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage,
    VkAccessFlags src_access_mask,
    VkAccessFlags dst_access_mask)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = src_access_mask;
    barrier.dstAccessMask = dst_access_mask;

    vkCmdPipelineBarrier(
        command_buffer,
        src_stage,
        dst_stage,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier);
}

bool UploadImagePreview(
    VulkanContext& vulkan_context,
    const unsigned char* pixels,
    std::uint32_t width,
    std::uint32_t height,
    VkImage image)
{
    const VkDevice device = vulkan_context.GetDevice();
    const VkPhysicalDevice physical_device = vulkan_context.GetPhysicalDevice();
    const VkDeviceSize upload_size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4u;

    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (!CreateBuffer(
            physical_device,
            device,
            upload_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging_buffer,
            staging_memory))
    {
        return false;
    }

    void* mapped = nullptr;
    VkResult result = vkMapMemory(device, staging_memory, 0, upload_size, 0, &mapped);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS || mapped == nullptr)
    {
        vkFreeMemory(device, staging_memory, nullptr);
        vkDestroyBuffer(device, staging_buffer, nullptr);
        return false;
    }

    std::memcpy(mapped, pixels, static_cast<std::size_t>(upload_size));
    vkUnmapMemory(device, staging_memory);

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = vulkan_context.GetQueueFamily();
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    result = vkCreateCommandPool(device, &pool_info, vulkan_context.GetAllocator(), &command_pool);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, staging_memory, nullptr);
        vkDestroyBuffer(device, staging_buffer, nullptr);
        return false;
    }

    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &allocate_info, &command_buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyCommandPool(device, command_pool, vulkan_context.GetAllocator());
        vkFreeMemory(device, staging_memory, nullptr);
        vkDestroyBuffer(device, staging_buffer, nullptr);
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_buffer, &begin_info);
    VulkanContext::CheckVkResult(result);
    if (result == VK_SUCCESS)
    {
        TransitionImageLayout(
            command_buffer,
            image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            VK_ACCESS_TRANSFER_WRITE_BIT);

        VkBufferImageCopy copy_region = {};
        copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.imageSubresource.layerCount = 1;
        copy_region.imageExtent.width = width;
        copy_region.imageExtent.height = height;
        copy_region.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(command_buffer, staging_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

        TransitionImageLayout(
            command_buffer,
            image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);

        result = vkEndCommandBuffer(command_buffer);
        VulkanContext::CheckVkResult(result);
    }

    if (result == VK_SUCCESS)
    {
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer;
        result = vkQueueSubmit(vulkan_context.GetQueue(), 1, &submit_info, VK_NULL_HANDLE);
        VulkanContext::CheckVkResult(result);
    }
    if (result == VK_SUCCESS)
    {
        result = vkQueueWaitIdle(vulkan_context.GetQueue());
        VulkanContext::CheckVkResult(result);
    }

    vkDestroyCommandPool(device, command_pool, vulkan_context.GetAllocator());
    vkFreeMemory(device, staging_memory, nullptr);
    vkDestroyBuffer(device, staging_buffer, nullptr);
    return result == VK_SUCCESS;
}
}

ImageInfoRenderer::~ImageInfoRenderer()
{
    Shutdown();
}

void ImageInfoRenderer::Shutdown()
{
    ClearImagePreview();
}

void ImageInfoRenderer::ClearImagePreview()
{
    if (cached_image_preview_context_ != nullptr)
    {
        cached_image_preview_context_->WaitIdle();
        const VkDevice device = cached_image_preview_context_->GetDevice();
        const VkAllocationCallbacks* allocator = cached_image_preview_context_->GetAllocator();
        if (cached_image_preview_descriptor_set_ != VK_NULL_HANDLE)
        {
            ImGui_ImplVulkan_RemoveTexture(cached_image_preview_descriptor_set_);
            cached_image_preview_descriptor_set_ = VK_NULL_HANDLE;
        }
        if (cached_image_preview_sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, cached_image_preview_sampler_, allocator);
            cached_image_preview_sampler_ = VK_NULL_HANDLE;
        }
        if (cached_image_preview_view_ != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, cached_image_preview_view_, allocator);
            cached_image_preview_view_ = VK_NULL_HANDLE;
        }
        if (cached_image_preview_image_ != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, cached_image_preview_image_, allocator);
            cached_image_preview_image_ = VK_NULL_HANDLE;
        }
        if (cached_image_preview_memory_ != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, cached_image_preview_memory_, allocator);
            cached_image_preview_memory_ = VK_NULL_HANDLE;
        }
    }

    cached_image_preview_context_ = nullptr;
    cached_image_preview_path_.clear();
    cached_image_preview_write_time_ = std::filesystem::file_time_type{};
    cached_image_preview_width_ = 0;
    cached_image_preview_height_ = 0;

    pending_preview_active_ = false;
    pending_preview_path_.clear();
    pending_preview_write_time_ = std::filesystem::file_time_type{};
}

ImTextureID ImageInfoRenderer::GetImagePreview(const std::filesystem::path& path, VulkanContext* vulkan_context)
{
    if (vulkan_context == nullptr)
    {
        ClearImagePreview();
        return ImTextureID{};
    }

    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool cache_valid =
        cached_image_preview_context_ == vulkan_context &&
        cached_image_preview_descriptor_set_ != VK_NULL_HANDLE &&
        cached_image_preview_path_ == path &&
        !error &&
        cached_image_preview_write_time_ == write_time;
    if (cache_valid)
    {
        return reinterpret_cast<ImTextureID>(cached_image_preview_descriptor_set_);
    }

    const bool use_async_decode = IsAsyncPreviewExtension(path);

    const bool failed_cache_match =
        !error &&
        failed_preview_path_ == path &&
        failed_preview_write_time_ == write_time;
    if (failed_cache_match)
    {
        return ImTextureID{};
    }

    if (pending_preview_active_)
    {
        if (!use_async_decode)
        {
            // LDR previews stay synchronous and should not be blocked by a pending HDR/EXR decode.
        }
        else
        {
        if (pending_preview_future_.valid() &&
            pending_preview_future_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
        {
            DecodedPreviewResult result = pending_preview_future_.get();
            pending_preview_active_ = false;
            pending_preview_path_.clear();
            pending_preview_write_time_ = std::filesystem::file_time_type{};

            if (!result.success)
            {
                failed_preview_path_ = result.path;
                failed_preview_write_time_ = result.write_time;
            }
            else if (!error && result.path == path && result.write_time == write_time)
            {
                ClearImagePreview();

                if (!CreateImage(
                        vulkan_context->GetPhysicalDevice(),
                        vulkan_context->GetDevice(),
                        static_cast<std::uint32_t>(result.width),
                        static_cast<std::uint32_t>(result.height),
                        VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        cached_image_preview_image_,
                        cached_image_preview_memory_,
                        cached_image_preview_view_))
                {
                    return ImTextureID{};
                }

                if (!UploadImagePreview(
                        *vulkan_context,
                        result.pixels.data(),
                        static_cast<std::uint32_t>(result.width),
                        static_cast<std::uint32_t>(result.height),
                        cached_image_preview_image_))
                {
                    ClearImagePreview();
                    return ImTextureID{};
                }

                VkSamplerCreateInfo sampler_info = {};
                sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                sampler_info.magFilter = VK_FILTER_LINEAR;
                sampler_info.minFilter = VK_FILTER_LINEAR;
                sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                sampler_info.maxLod = 1.0f;

                VkResult vk_result = vkCreateSampler(vulkan_context->GetDevice(), &sampler_info, vulkan_context->GetAllocator(), &cached_image_preview_sampler_);
                VulkanContext::CheckVkResult(vk_result);
                if (vk_result != VK_SUCCESS)
                {
                    ClearImagePreview();
                    return ImTextureID{};
                }

                cached_image_preview_descriptor_set_ = ImGui_ImplVulkan_AddTexture(
                    cached_image_preview_sampler_,
                    cached_image_preview_view_,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                if (cached_image_preview_descriptor_set_ == VK_NULL_HANDLE)
                {
                    ClearImagePreview();
                    return ImTextureID{};
                }

                cached_image_preview_context_ = vulkan_context;
                cached_image_preview_path_ = path;
                cached_image_preview_write_time_ = write_time;
                cached_image_preview_width_ = result.width;
                cached_image_preview_height_ = result.height;
                failed_preview_path_.clear();
                failed_preview_write_time_ = std::filesystem::file_time_type{};

                return reinterpret_cast<ImTextureID>(cached_image_preview_descriptor_set_);
            }
        }

            return ImTextureID{};
        }
    }

    if (!use_async_decode)
    {
        ClearImagePreview();

        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pixels;
        if (!LoadPreviewPixels(path, pixels, width, height))
        {
            return ImTextureID{};
        }

        if (!CreateImage(
                vulkan_context->GetPhysicalDevice(),
                vulkan_context->GetDevice(),
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height),
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                cached_image_preview_image_,
                cached_image_preview_memory_,
                cached_image_preview_view_))
        {
            return ImTextureID{};
        }

        if (!UploadImagePreview(
                *vulkan_context,
                pixels.data(),
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height),
                cached_image_preview_image_))
        {
            ClearImagePreview();
            return ImTextureID{};
        }

        VkSamplerCreateInfo sampler_info = {};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 1.0f;

        VkResult vk_result = vkCreateSampler(vulkan_context->GetDevice(), &sampler_info, vulkan_context->GetAllocator(), &cached_image_preview_sampler_);
        VulkanContext::CheckVkResult(vk_result);
        if (vk_result != VK_SUCCESS)
        {
            ClearImagePreview();
            return ImTextureID{};
        }

        cached_image_preview_descriptor_set_ = ImGui_ImplVulkan_AddTexture(
            cached_image_preview_sampler_,
            cached_image_preview_view_,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (cached_image_preview_descriptor_set_ == VK_NULL_HANDLE)
        {
            ClearImagePreview();
            return ImTextureID{};
        }

        cached_image_preview_context_ = vulkan_context;
        cached_image_preview_path_ = path;
        cached_image_preview_write_time_ = error ? std::filesystem::file_time_type::min() : write_time;
        cached_image_preview_width_ = width;
        cached_image_preview_height_ = height;
        return reinterpret_cast<ImTextureID>(cached_image_preview_descriptor_set_);
    }

    pending_preview_path_ = path;
    pending_preview_write_time_ = error ? std::filesystem::file_time_type::min() : write_time;
    pending_preview_active_ = true;
    pending_preview_future_ = std::async(std::launch::async, [path, source_write_time = pending_preview_write_time_]()
    {
        DecodedPreviewResult result;
        result.path = path;
        result.write_time = source_write_time;
        result.success = LoadPreviewPixels(path, result.pixels, result.width, result.height);
        return result;
    });

    return ImTextureID{};
}

bool ImageInfoRenderer::IsPreviewLoading(const std::filesystem::path& path) const
{
    return pending_preview_active_ && pending_preview_path_ == path;
}