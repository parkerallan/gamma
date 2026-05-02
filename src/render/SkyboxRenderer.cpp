#include "render/SkyboxRenderer.h"

#include "vfs/AssetVFS.h"

#include <SDL3/SDL.h>
#include <stb_image.h>
#include <tinyexr.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <iterator>

namespace
{
constexpr std::uint32_t kMaxSkyboxDimension = 4096;
constexpr std::uint32_t kDownscalePumpRowInterval = 64;
constexpr std::size_t kMemcopyPumpChunkBytes = 8u * 1024u * 1024u;

void PumpMainThreadEvents()
{
    // Keep the Windows message queue active while doing long synchronous work.
    SDL_PumpEvents();
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
    {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::uint8_t> ReadFileBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> ReadSkyboxBytes(const std::filesystem::path& path, bool use_pak_streaming)
{
    if (use_pak_streaming)
    {
        if (!g_asset_reader)
        {
            return {};
        }
        return ReadAssetFileAsBytes(path.generic_string());
    }

    return ReadFileBytes(path);
}

std::uint32_t FindMemoryType(VkPhysicalDevice physical_device, std::uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index)
    {
        if ((type_filter & (1u << index)) != 0u &&
            (memory_properties.memoryTypes[index].propertyFlags & properties) == properties)
        {
            return index;
        }
    }

    return UINT32_MAX;
}

bool CreateBuffer(
    VkPhysicalDevice physical_device,
    VkDevice device,
    const VkAllocationCallbacks* allocator,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    VkBuffer& out_buffer,
    VkDeviceMemory& out_memory)
{
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(device, &buffer_info, allocator, &out_buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(device, out_buffer, &requirements);

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = FindMemoryType(physical_device, requirements.memoryTypeBits, properties);
    if (allocate_info.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyBuffer(device, out_buffer, allocator);
        out_buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkAllocateMemory(device, &allocate_info, allocator, &out_memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyBuffer(device, out_buffer, allocator);
        out_buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindBufferMemory(device, out_buffer, out_memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, out_memory, allocator);
        vkDestroyBuffer(device, out_buffer, allocator);
        out_memory = VK_NULL_HANDLE;
        out_buffer = VK_NULL_HANDLE;
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
    VkAccessFlags src_access,
    VkAccessFlags dst_access)
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
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;

    vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool ExecuteImmediate(VulkanContext& context, VkCommandPool command_pool, const std::function<void(VkCommandBuffer)>& record)
{
    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;

    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(context.GetDevice(), &allocate_info, &command_buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    result = vkBeginCommandBuffer(command_buffer, &begin_info);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeCommandBuffers(context.GetDevice(), command_pool, 1, &command_buffer);
        return false;
    }

    record(command_buffer);

    result = vkEndCommandBuffer(command_buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeCommandBuffers(context.GetDevice(), command_pool, 1, &command_buffer);
        return false;
    }

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence submit_fence = VK_NULL_HANDLE;
    result = vkCreateFence(context.GetDevice(), &fence_info, context.GetAllocator(), &submit_fence);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeCommandBuffers(context.GetDevice(), command_pool, 1, &command_buffer);
        return false;
    }

    result = vkQueueSubmit(context.GetQueue(), 1, &submit_info, submit_fence);
    VulkanContext::CheckVkResult(result);
    if (result == VK_SUCCESS)
    {
        while (true)
        {
            const VkResult fence_status = vkGetFenceStatus(context.GetDevice(), submit_fence);
            if (fence_status == VK_SUCCESS)
            {
                break;
            }

            if (fence_status != VK_NOT_READY)
            {
                result = fence_status;
                VulkanContext::CheckVkResult(result);
                break;
            }

            PumpMainThreadEvents();
        }
    }

    vkDestroyFence(context.GetDevice(), submit_fence, context.GetAllocator());
    vkFreeCommandBuffers(context.GetDevice(), command_pool, 1, &command_buffer);
    return result == VK_SUCCESS;
}

std::vector<float> DownscaleFloatRgba(
    const float* src_pixels,
    std::uint32_t src_width,
    std::uint32_t src_height,
    std::uint32_t dst_width,
    std::uint32_t dst_height)
{
    std::vector<float> resized(static_cast<std::size_t>(dst_width) * static_cast<std::size_t>(dst_height) * 4u, 0.0f);
    if (src_pixels == nullptr || src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0)
    {
        return resized;
    }

    const float scale_x = static_cast<float>(src_width) / static_cast<float>(dst_width);
    const float scale_y = static_cast<float>(src_height) / static_cast<float>(dst_height);

    for (std::uint32_t y = 0; y < dst_height; ++y)
    {
        if ((y % kDownscalePumpRowInterval) == 0u)
        {
            PumpMainThreadEvents();
        }

        const std::uint32_t src_y = (std::min)(static_cast<std::uint32_t>(y * scale_y), src_height - 1u);
        for (std::uint32_t x = 0; x < dst_width; ++x)
        {
            const std::uint32_t src_x = (std::min)(static_cast<std::uint32_t>(x * scale_x), src_width - 1u);
            const std::size_t src_index = (static_cast<std::size_t>(src_y) * src_width + src_x) * 4u;
            const std::size_t dst_index = (static_cast<std::size_t>(y) * dst_width + x) * 4u;
            resized[dst_index + 0] = src_pixels[src_index + 0];
            resized[dst_index + 1] = src_pixels[src_index + 1];
            resized[dst_index + 2] = src_pixels[src_index + 2];
            resized[dst_index + 3] = src_pixels[src_index + 3];
        }
    }

    return resized;
}

bool DecodeSkyboxRgba32f(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes,
    bool use_pak_streaming,
    std::vector<float>& out_rgba,
    std::uint32_t& out_width,
    std::uint32_t& out_height)
{
    const std::string extension = ToLowerAscii(path.extension().string());

    if (extension == ".hdr")
    {
        int width = 0;
        int height = 0;
        int channels = 0;
        float* pixels = nullptr;
        if (use_pak_streaming)
        {
            if (bytes.empty())
            {
                return false;
            }
            pixels = stbi_loadf_from_memory(
                bytes.data(),
                static_cast<int>(bytes.size()),
                &width,
                &height,
                &channels,
                4);
        }
        else
        {
            pixels = stbi_loadf(path.string().c_str(), &width, &height, &channels, 4);
        }

        if (pixels == nullptr || width <= 0 || height <= 0)
        {
            return false;
        }

        out_width = static_cast<std::uint32_t>(width);
        out_height = static_cast<std::uint32_t>(height);
        const std::size_t pixel_count = static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height) * 4u;
        PumpMainThreadEvents();
        out_rgba.assign(pixels, pixels + pixel_count);
        stbi_image_free(pixels);
        return true;
    }

    if (extension == ".exr")
    {
        float* pixels = nullptr;
        int width = 0;
        int height = 0;
        const char* error_message = nullptr;

        int load_result = TINYEXR_ERROR_INVALID_DATA;
        if (use_pak_streaming)
        {
            if (bytes.empty())
            {
                return false;
            }
            load_result = LoadEXRFromMemory(
                &pixels,
                &width,
                &height,
                bytes.data(),
                static_cast<size_t>(bytes.size()),
                &error_message);
        }
        else
        {
            load_result = LoadEXR(&pixels, &width, &height, path.string().c_str(), &error_message);
        }

        if (load_result != TINYEXR_SUCCESS || pixels == nullptr || width <= 0 || height <= 0)
        {
            if (error_message != nullptr)
            {
                SDL_Log("SkyboxRenderer: EXR decode failed (%s): %s", path.string().c_str(), error_message);
                FreeEXRErrorMessage(error_message);
            }
            return false;
        }

        if (error_message != nullptr)
        {
            FreeEXRErrorMessage(error_message);
        }

        out_width = static_cast<std::uint32_t>(width);
        out_height = static_cast<std::uint32_t>(height);
        const std::size_t pixel_count = static_cast<std::size_t>(out_width) * static_cast<std::size_t>(out_height) * 4u;
        PumpMainThreadEvents();
        out_rgba.assign(pixels, pixels + pixel_count);
        std::free(pixels);
        return true;
    }

    return false;
}

std::filesystem::path FindSceneSkyboxPath(const SceneMetadata& scene_metadata)
{
    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        for (const SceneObjectAttribute& attribute : object.attributes)
        {
            if (attribute.kind == SceneObjectAttributeKind::Skybox && !attribute.skybox.image_path.empty())
            {
                return std::filesystem::path(attribute.skybox.image_path);
            }
        }
    }

    return {};
}
}

SkyboxRenderer::~SkyboxRenderer()
{
    Shutdown();
}

bool SkyboxRenderer::Initialize(VulkanContext* context)
{
    if (context == nullptr)
    {
        return false;
    }

    vulkan_context_ = context;

    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = context->GetQueueFamily();
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    VkResult result = vkCreateCommandPool(context->GetDevice(), &pool_info, context->GetAllocator(), &command_pool_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        command_pool_ = VK_NULL_HANDLE;
        vulkan_context_ = nullptr;
        return false;
    }

    return true;
}

void SkyboxRenderer::Shutdown()
{
    if (vulkan_context_ == nullptr)
    {
        return;
    }

    VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    vkDeviceWaitIdle(device);

    for (auto& [key, texture] : texture_cache_)
    {
        ReleaseTexture(texture);
    }
    texture_cache_.clear();

    if (command_pool_ != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(device, command_pool_, allocator);
        command_pool_ = VK_NULL_HANDLE;
    }

    vulkan_context_ = nullptr;
}

VkImageView SkyboxRenderer::ResolveSkyboxView(const SceneMetadata& scene_metadata, const std::filesystem::path& project_root)
{
    if (vulkan_context_ == nullptr)
    {
        return VK_NULL_HANDLE;
    }

    const std::filesystem::path scene_skybox_path = FindSceneSkyboxPath(scene_metadata);
    if (scene_skybox_path.empty())
    {
        return VK_NULL_HANDLE;
    }

    const bool use_pak_streaming = project_root.empty() && g_asset_reader != nullptr;
    const std::filesystem::path resolved_path = use_pak_streaming || scene_skybox_path.is_absolute()
        ? scene_skybox_path
        : (project_root / scene_skybox_path);

    GpuTexture* texture = GetOrLoadTexture(resolved_path, use_pak_streaming);
    return texture != nullptr ? texture->view : VK_NULL_HANDLE;
}

float SkyboxRenderer::ResolveSkyboxRotationDegrees(const SceneMetadata& scene_metadata) const
{
    const std::filesystem::path scene_skybox_path = FindSceneSkyboxPath(scene_metadata);
    if (scene_skybox_path.empty())
    {
        return 0.0f;
    }

    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        for (const SceneObjectAttribute& attribute : object.attributes)
        {
            if (attribute.kind == SceneObjectAttributeKind::Skybox && !attribute.skybox.image_path.empty())
            {
                return attribute.skybox.rotation_degrees;
            }
        }
    }

    return 0.0f;
}

void SkyboxRenderer::ReleaseTexture(GpuTexture& texture)
{
    if (vulkan_context_ == nullptr)
    {
        texture = {};
        return;
    }

    VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    if (texture.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, texture.view, allocator);
    }
    if (texture.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, texture.image, allocator);
    }
    if (texture.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, texture.memory, allocator);
    }

    texture = {};
}

SkyboxRenderer::GpuTexture* SkyboxRenderer::GetOrLoadTexture(const std::filesystem::path& skybox_path, bool use_pak_streaming)
{
    if (vulkan_context_ == nullptr || command_pool_ == VK_NULL_HANDLE)
    {
        return nullptr;
    }

    const std::string cache_key = (use_pak_streaming ? "pak:" : "fs:") + skybox_path.generic_string();
    const auto cache_it = texture_cache_.find(cache_key);
    if (cache_it != texture_cache_.end())
    {
        return &cache_it->second;
    }

    std::vector<std::uint8_t> skybox_bytes;
    if (use_pak_streaming)
    {
        skybox_bytes = ReadSkyboxBytes(skybox_path, use_pak_streaming);
        if (skybox_bytes.empty())
        {
            SDL_Log("SkyboxRenderer: failed to read skybox bytes: %s", skybox_path.string().c_str());
            return nullptr;
        }
    }

    PumpMainThreadEvents();
    std::vector<float> pixels_rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!DecodeSkyboxRgba32f(skybox_path, skybox_bytes, use_pak_streaming, pixels_rgba, width, height) ||
        pixels_rgba.empty() ||
        width == 0 ||
        height == 0)
    {
        SDL_Log("SkyboxRenderer: failed to decode skybox image: %s", skybox_path.string().c_str());
        return nullptr;
    }

    PumpMainThreadEvents();
    if (width > kMaxSkyboxDimension || height > kMaxSkyboxDimension)
    {
        const float scale = (std::min)(
            static_cast<float>(kMaxSkyboxDimension) / static_cast<float>(width),
            static_cast<float>(kMaxSkyboxDimension) / static_cast<float>(height));
        const std::uint32_t resized_width = (std::max)(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(width) * scale)));
        const std::uint32_t resized_height = (std::max)(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(height) * scale)));
        pixels_rgba = DownscaleFloatRgba(pixels_rgba.data(), width, height, resized_width, resized_height);
        width = resized_width;
        height = resized_height;
    }

    GpuTexture texture;
    if (!UploadFloatTexture(pixels_rgba.data(), width, height, texture))
    {
        ReleaseTexture(texture);
        SDL_Log("SkyboxRenderer: failed to upload skybox texture: %s", skybox_path.string().c_str());
        return nullptr;
    }

    texture.width = width;
    texture.height = height;
    texture_cache_.emplace(cache_key, std::move(texture));
    return &texture_cache_[cache_key];
}

bool SkyboxRenderer::UploadFloatTexture(
    const float* pixels_rgba,
    std::uint32_t width,
    std::uint32_t height,
    GpuTexture& out_texture)
{
    if (vulkan_context_ == nullptr || pixels_rgba == nullptr || width == 0 || height == 0)
    {
        return false;
    }

    VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();
    const VkDeviceSize upload_size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * sizeof(float) * 4u;

    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (!CreateBuffer(
            vulkan_context_->GetPhysicalDevice(),
            device,
            allocator,
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
    if (result != VK_SUCCESS)
    {
        vkDestroyBuffer(device, staging_buffer, allocator);
        vkFreeMemory(device, staging_memory, allocator);
        return false;
    }

    const auto* source_bytes = reinterpret_cast<const std::uint8_t*>(pixels_rgba);
    auto* destination_bytes = reinterpret_cast<std::uint8_t*>(mapped);
    std::size_t copied = 0;
    const std::size_t total_size = static_cast<std::size_t>(upload_size);
    while (copied < total_size)
    {
        const std::size_t chunk = (std::min)(kMemcopyPumpChunkBytes, total_size - copied);
        std::memcpy(destination_bytes + copied, source_bytes + copied, chunk);
        copied += chunk;
        PumpMainThreadEvents();
    }
    vkUnmapMemory(device, staging_memory);

    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    result = vkCreateImage(device, &image_info, allocator, &out_texture.image);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyBuffer(device, staging_buffer, allocator);
        vkFreeMemory(device, staging_memory, allocator);
        return false;
    }

    VkMemoryRequirements image_requirements = {};
    vkGetImageMemoryRequirements(device, out_texture.image, &image_requirements);

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = image_requirements.size;
    allocate_info.memoryTypeIndex = FindMemoryType(
        vulkan_context_->GetPhysicalDevice(),
        image_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (allocate_info.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyImage(device, out_texture.image, allocator);
        out_texture.image = VK_NULL_HANDLE;
        vkDestroyBuffer(device, staging_buffer, allocator);
        vkFreeMemory(device, staging_memory, allocator);
        return false;
    }

    result = vkAllocateMemory(device, &allocate_info, allocator, &out_texture.memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyImage(device, out_texture.image, allocator);
        out_texture.image = VK_NULL_HANDLE;
        vkDestroyBuffer(device, staging_buffer, allocator);
        vkFreeMemory(device, staging_memory, allocator);
        return false;
    }

    result = vkBindImageMemory(device, out_texture.image, out_texture.memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyImage(device, out_texture.image, allocator);
        vkFreeMemory(device, out_texture.memory, allocator);
        out_texture.image = VK_NULL_HANDLE;
        out_texture.memory = VK_NULL_HANDLE;
        vkDestroyBuffer(device, staging_buffer, allocator);
        vkFreeMemory(device, staging_memory, allocator);
        return false;
    }

    const bool upload_success = ExecuteImmediate(*vulkan_context_, command_pool_, [&](VkCommandBuffer command_buffer)
    {
        TransitionImageLayout(
            command_buffer,
            out_texture.image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            VK_ACCESS_TRANSFER_WRITE_BIT);

        VkBufferImageCopy copy_region = {};
        copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.imageSubresource.layerCount = 1;
        copy_region.imageExtent = {width, height, 1};

        vkCmdCopyBufferToImage(
            command_buffer,
            staging_buffer,
            out_texture.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copy_region);

        TransitionImageLayout(
            command_buffer,
            out_texture.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    });

    vkDestroyBuffer(device, staging_buffer, allocator);
    vkFreeMemory(device, staging_memory, allocator);

    if (!upload_success)
    {
        return false;
    }

    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = out_texture.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(device, &view_info, allocator, &out_texture.view);
    VulkanContext::CheckVkResult(result);
    return result == VK_SUCCESS;
}
