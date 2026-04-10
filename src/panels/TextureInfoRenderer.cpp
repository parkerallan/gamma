#include "panels/TextureInfoRenderer.h"

#include "imgui_impl_vulkan.h"

#include <stb_image.h>

#include <cstdint>
#include <cstring>

namespace
{
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

bool UploadTexturePreview(
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

TextureInfoRenderer::~TextureInfoRenderer()
{
    Shutdown();
}

void TextureInfoRenderer::Shutdown()
{
    ClearTexturePreview();
}

void TextureInfoRenderer::ClearTexturePreview()
{
    if (cached_texture_preview_context_ != nullptr)
    {
        cached_texture_preview_context_->WaitIdle();
        const VkDevice device = cached_texture_preview_context_->GetDevice();
        const VkAllocationCallbacks* allocator = cached_texture_preview_context_->GetAllocator();
        if (cached_texture_preview_descriptor_set_ != VK_NULL_HANDLE)
        {
            ImGui_ImplVulkan_RemoveTexture(cached_texture_preview_descriptor_set_);
            cached_texture_preview_descriptor_set_ = VK_NULL_HANDLE;
        }
        if (cached_texture_preview_sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, cached_texture_preview_sampler_, allocator);
            cached_texture_preview_sampler_ = VK_NULL_HANDLE;
        }
        if (cached_texture_preview_view_ != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, cached_texture_preview_view_, allocator);
            cached_texture_preview_view_ = VK_NULL_HANDLE;
        }
        if (cached_texture_preview_image_ != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, cached_texture_preview_image_, allocator);
            cached_texture_preview_image_ = VK_NULL_HANDLE;
        }
        if (cached_texture_preview_memory_ != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, cached_texture_preview_memory_, allocator);
            cached_texture_preview_memory_ = VK_NULL_HANDLE;
        }
    }

    cached_texture_preview_context_ = nullptr;
    cached_texture_preview_path_.clear();
    cached_texture_preview_write_time_ = std::filesystem::file_time_type{};
    cached_texture_preview_width_ = 0;
    cached_texture_preview_height_ = 0;
}

ImTextureID TextureInfoRenderer::GetTexturePreview(const std::filesystem::path& path, VulkanContext* vulkan_context)
{
    if (vulkan_context == nullptr)
    {
        ClearTexturePreview();
        return ImTextureID{};
    }

    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool cache_valid =
        cached_texture_preview_context_ == vulkan_context &&
        cached_texture_preview_descriptor_set_ != VK_NULL_HANDLE &&
        cached_texture_preview_path_ == path &&
        !error &&
        cached_texture_preview_write_time_ == write_time;
    if (cache_valid)
    {
        return reinterpret_cast<ImTextureID>(cached_texture_preview_descriptor_set_);
    }

    ClearTexturePreview();

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr)
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
            cached_texture_preview_image_,
            cached_texture_preview_memory_,
            cached_texture_preview_view_))
    {
        stbi_image_free(pixels);
        return ImTextureID{};
    }

    if (!UploadTexturePreview(
            *vulkan_context,
            pixels,
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
            cached_texture_preview_image_))
    {
        stbi_image_free(pixels);
        ClearTexturePreview();
        return ImTextureID{};
    }

    stbi_image_free(pixels);

    VkSamplerCreateInfo sampler_info = {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 1.0f;

    VkResult result = vkCreateSampler(vulkan_context->GetDevice(), &sampler_info, vulkan_context->GetAllocator(), &cached_texture_preview_sampler_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        ClearTexturePreview();
        return ImTextureID{};
    }

    cached_texture_preview_descriptor_set_ = ImGui_ImplVulkan_AddTexture(
        cached_texture_preview_sampler_,
        cached_texture_preview_view_,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (cached_texture_preview_descriptor_set_ == VK_NULL_HANDLE)
    {
        ClearTexturePreview();
        return ImTextureID{};
    }

    cached_texture_preview_context_ = vulkan_context;
    cached_texture_preview_path_ = path;
    cached_texture_preview_write_time_ = error ? std::filesystem::file_time_type::min() : write_time;
    cached_texture_preview_width_ = width;
    cached_texture_preview_height_ = height;
    return reinterpret_cast<ImTextureID>(cached_texture_preview_descriptor_set_);
}