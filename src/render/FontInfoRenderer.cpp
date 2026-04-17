#include "render/FontInfoRenderer.h"

#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
constexpr float kDefaultFontPreviewSizePixels = 34.0f;
constexpr int kFontPreviewWidth = 512;
constexpr int kFontPreviewMinHeight = 320;
constexpr int kFontPreviewMaxHeight = 1400;
constexpr int kFontPreviewPaddingX = 18;
constexpr int kFontPreviewPaddingY = 20;
constexpr int kFontPreviewLineGap = 10;
constexpr int kFontPreviewContentWidth = 360;
constexpr int kFontPreviewHeaderHeight = 14;
constexpr ImWchar kFontPreviewGlyphRanges[] = {32, 126, 0};
constexpr std::array<const char*, 5> kFontPreviewGroups = {
    "abcdefghijklmnopqrstuvwxyz",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "0123456789",
    "!@#$%^&*()_+-=[]{}",
    ";:'\",.<>/?\\|`~"
};

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

bool UploadFontPreview(
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

ImFontGlyph* ResolvePreviewGlyph(ImFontBaked* baked_font, unsigned char character)
{
    ImFontGlyph* glyph = baked_font->FindGlyphNoFallback(static_cast<ImWchar>(character));
    if (glyph == nullptr)
    {
        glyph = baked_font->FindGlyph(static_cast<ImWchar>('?'));
    }

    return glyph;
}

int CountWrappedPreviewLines(ImFontBaked* baked_font)
{
    const float content_width = static_cast<float>(kFontPreviewContentWidth);
    int line_count = 0;
    for (const char* group : kFontPreviewGroups)
    {
        float pen_x = 0.0f;
        bool started_group = false;
        for (const char* character = group; *character != '\0'; ++character)
        {
            ImFontGlyph* glyph = ResolvePreviewGlyph(baked_font, static_cast<unsigned char>(*character));
            const float advance = glyph != nullptr ? glyph->AdvanceX : baked_font->FallbackAdvanceX;
            if (!started_group)
            {
                ++line_count;
                started_group = true;
            }
            else if (pen_x + advance > content_width)
            {
                ++line_count;
                pen_x = 0.0f;
            }

            pen_x += advance;
        }
    }

    return (std::max)(1, line_count);
}

void FillPreviewBackground(std::vector<unsigned char>& preview_pixels, int preview_width, int preview_height)
{
    for (std::size_t index = 0; index < preview_pixels.size(); index += 4)
    {
        preview_pixels[index + 0] = 244;
        preview_pixels[index + 1] = 240;
        preview_pixels[index + 2] = 233;
        preview_pixels[index + 3] = 255;
    }

    const int header_height = (std::min)(kFontPreviewHeaderHeight, preview_height);
    for (int row = 0; row < header_height; ++row)
    {
        for (int column = 0; column < preview_width; ++column)
        {
            unsigned char* pixel = preview_pixels.data() + ((row * preview_width + column) * 4);
            pixel[0] = 224;
            pixel[1] = 216;
            pixel[2] = 203;
            pixel[3] = 255;
        }
    }
}

void BlendPreviewPixel(unsigned char* destination, const unsigned char* source, bool colored)
{
    const unsigned int alpha = source[3];
    if (alpha == 0)
    {
        return;
    }

    const unsigned int inverse_alpha = 255u - alpha;
    const unsigned int source_red = colored ? source[0] : 34u;
    const unsigned int source_green = colored ? source[1] : 34u;
    const unsigned int source_blue = colored ? source[2] : 40u;

    destination[0] = static_cast<unsigned char>((source_red * alpha + destination[0] * inverse_alpha) / 255u);
    destination[1] = static_cast<unsigned char>((source_green * alpha + destination[1] * inverse_alpha) / 255u);
    destination[2] = static_cast<unsigned char>((source_blue * alpha + destination[2] * inverse_alpha) / 255u);
    destination[3] = 255;
}

void BlitGlyph(
    const ImFontGlyph& glyph,
    const unsigned char* atlas_pixels,
    int atlas_width,
    int atlas_height,
    std::vector<unsigned char>& preview_pixels,
    int preview_width,
    int preview_height,
    float pen_x,
    float baseline_y)
{
    const int source_x0 = (std::max)(0, (std::min)(atlas_width, static_cast<int>(std::floor(glyph.U0 * static_cast<float>(atlas_width)))));
    const int source_y0 = (std::max)(0, (std::min)(atlas_height, static_cast<int>(std::floor(glyph.V0 * static_cast<float>(atlas_height)))));
    const int source_x1 = (std::max)(0, (std::min)(atlas_width, static_cast<int>(std::ceil(glyph.U1 * static_cast<float>(atlas_width)))));
    const int source_y1 = (std::max)(0, (std::min)(atlas_height, static_cast<int>(std::ceil(glyph.V1 * static_cast<float>(atlas_height)))));
    const int glyph_width = source_x1 - source_x0;
    const int glyph_height = source_y1 - source_y0;
    if (glyph_width <= 0 || glyph_height <= 0)
    {
        return;
    }

    const int destination_x0 = static_cast<int>(std::lround(pen_x + glyph.X0));
    const int destination_y0 = static_cast<int>(std::lround(baseline_y + glyph.Y0));

    for (int row = 0; row < glyph_height; ++row)
    {
        const int destination_y = destination_y0 + row;
        if (destination_y < 0 || destination_y >= preview_height)
        {
            continue;
        }

        for (int column = 0; column < glyph_width; ++column)
        {
            const int destination_x = destination_x0 + column;
            if (destination_x < 0 || destination_x >= preview_width)
            {
                continue;
            }

            const unsigned char* source = atlas_pixels + (((source_y0 + row) * atlas_width + (source_x0 + column)) * 4);
            unsigned char* destination = preview_pixels.data() + ((destination_y * preview_width + destination_x) * 4);
            BlendPreviewPixel(destination, source, glyph.Colored != 0);
        }
    }
}

bool BuildFontPreviewBitmap(
    const std::filesystem::path& path,
    float preview_size_pixels,
    std::vector<unsigned char>& preview_pixels,
    int& preview_width,
    int& preview_height)
{
    ImFontAtlas preview_atlas;
    preview_atlas.Flags |= ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoBakedLines;

    ImFontConfig font_config;
    font_config.Flags |= ImFontFlags_NoLoadError;

    ImFont* font = preview_atlas.AddFontFromFileTTF(
        path.string().c_str(),
        preview_size_pixels,
        &font_config,
        kFontPreviewGlyphRanges);
    if (font == nullptr)
    {
        return false;
    }

    unsigned char* atlas_pixels = nullptr;
    int atlas_width = 0;
    int atlas_height = 0;
    preview_atlas.GetTexDataAsRGBA32(&atlas_pixels, &atlas_width, &atlas_height);
    if (atlas_pixels == nullptr || atlas_width <= 0 || atlas_height <= 0)
    {
        return false;
    }

    ImFontBaked* baked_font = font->GetFontBaked(font->LegacySize);
    if (baked_font == nullptr)
    {
        return false;
    }

    const float line_height = baked_font->Ascent - baked_font->Descent;
    const int line_count = CountWrappedPreviewLines(baked_font);
    preview_width = kFontPreviewWidth;
    preview_height = static_cast<int>(std::ceil(
        static_cast<float>(kFontPreviewPaddingY * 2) +
        static_cast<float>(line_count) * line_height +
        static_cast<float>((std::max)(0, line_count - 1)) * static_cast<float>(kFontPreviewLineGap)));
    preview_height = (std::max)(kFontPreviewMinHeight, preview_height);
    preview_height = (std::min)(kFontPreviewMaxHeight, preview_height);
    preview_pixels.assign(static_cast<std::size_t>(preview_width * preview_height * 4), 0);
    FillPreviewBackground(preview_pixels, preview_width, preview_height);

    float baseline_y = static_cast<float>(kFontPreviewPaddingY) + baked_font->Ascent;
    const float content_left = static_cast<float>(kFontPreviewPaddingX);
    const float content_right = (std::min)(
        static_cast<float>(preview_width - kFontPreviewPaddingX),
        content_left + static_cast<float>(kFontPreviewContentWidth));

    for (const char* group : kFontPreviewGroups)
    {
        float pen_x = content_left;
        bool started_group = false;
        for (const char* character = group; *character != '\0'; ++character)
        {
            ImFontGlyph* glyph = ResolvePreviewGlyph(baked_font, static_cast<unsigned char>(*character));
            const float advance = glyph != nullptr ? glyph->AdvanceX : baked_font->FallbackAdvanceX;
            if (started_group && pen_x + advance > content_right)
            {
                baseline_y += line_height + static_cast<float>(kFontPreviewLineGap);
                pen_x = content_left;
            }

            if (glyph != nullptr && glyph->Visible)
            {
                BlitGlyph(*glyph, atlas_pixels, atlas_width, atlas_height, preview_pixels, preview_width, preview_height, pen_x, baseline_y);
            }

            pen_x += advance;
            started_group = true;
        }

        baseline_y += line_height + static_cast<float>(kFontPreviewLineGap);
    }

    return true;
}
}

FontInfoRenderer::~FontInfoRenderer()
{
    Shutdown();
}

void FontInfoRenderer::Shutdown()
{
    ClearFontPreview();
}

void FontInfoRenderer::ClearFontPreview()
{
    if (cached_font_preview_context_ != nullptr)
    {
        cached_font_preview_context_->WaitIdle();
        const VkDevice device = cached_font_preview_context_->GetDevice();
        const VkAllocationCallbacks* allocator = cached_font_preview_context_->GetAllocator();
        if (cached_font_preview_descriptor_set_ != VK_NULL_HANDLE)
        {
            ImGui_ImplVulkan_RemoveTexture(cached_font_preview_descriptor_set_);
            cached_font_preview_descriptor_set_ = VK_NULL_HANDLE;
        }
        if (cached_font_preview_sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, cached_font_preview_sampler_, allocator);
            cached_font_preview_sampler_ = VK_NULL_HANDLE;
        }
        if (cached_font_preview_view_ != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, cached_font_preview_view_, allocator);
            cached_font_preview_view_ = VK_NULL_HANDLE;
        }
        if (cached_font_preview_image_ != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, cached_font_preview_image_, allocator);
            cached_font_preview_image_ = VK_NULL_HANDLE;
        }
        if (cached_font_preview_memory_ != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, cached_font_preview_memory_, allocator);
            cached_font_preview_memory_ = VK_NULL_HANDLE;
        }
    }

    cached_font_preview_context_ = nullptr;
    cached_font_preview_path_.clear();
    cached_font_preview_write_time_ = std::filesystem::file_time_type{};
    cached_font_preview_size_pixels_ = 0.0f;
    cached_font_preview_width_ = 0;
    cached_font_preview_height_ = 0;
}

ImTextureID FontInfoRenderer::GetFontPreview(const std::filesystem::path& path, VulkanContext* vulkan_context, float preview_size_pixels)
{
    if (vulkan_context == nullptr)
    {
        ClearFontPreview();
        return ImTextureID{};
    }

    const float requested_preview_size = preview_size_pixels > 0.0f ? preview_size_pixels : kDefaultFontPreviewSizePixels;
    const float clamped_preview_size = (std::clamp)(requested_preview_size, 12.0f, 96.0f);
    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool cache_valid =
        cached_font_preview_context_ == vulkan_context &&
        cached_font_preview_descriptor_set_ != VK_NULL_HANDLE &&
        cached_font_preview_path_ == path &&
        std::abs(cached_font_preview_size_pixels_ - clamped_preview_size) < 0.01f &&
        !error &&
        cached_font_preview_write_time_ == write_time;
    if (cache_valid)
    {
        return reinterpret_cast<ImTextureID>(cached_font_preview_descriptor_set_);
    }

    ClearFontPreview();

    std::vector<unsigned char> preview_pixels;
    int preview_width = 0;
    int preview_height = 0;
    if (!BuildFontPreviewBitmap(path, clamped_preview_size, preview_pixels, preview_width, preview_height))
    {
        return ImTextureID{};
    }

    if (!CreateImage(
            vulkan_context->GetPhysicalDevice(),
            vulkan_context->GetDevice(),
            static_cast<std::uint32_t>(preview_width),
            static_cast<std::uint32_t>(preview_height),
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            cached_font_preview_image_,
            cached_font_preview_memory_,
            cached_font_preview_view_))
    {
        return ImTextureID{};
    }

    if (!UploadFontPreview(
            *vulkan_context,
            preview_pixels.data(),
            static_cast<std::uint32_t>(preview_width),
            static_cast<std::uint32_t>(preview_height),
            cached_font_preview_image_))
    {
        ClearFontPreview();
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

    VkResult result = vkCreateSampler(vulkan_context->GetDevice(), &sampler_info, vulkan_context->GetAllocator(), &cached_font_preview_sampler_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        ClearFontPreview();
        return ImTextureID{};
    }

    cached_font_preview_descriptor_set_ = ImGui_ImplVulkan_AddTexture(
        cached_font_preview_sampler_,
        cached_font_preview_view_,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (cached_font_preview_descriptor_set_ == VK_NULL_HANDLE)
    {
        ClearFontPreview();
        return ImTextureID{};
    }

    cached_font_preview_context_ = vulkan_context;
    cached_font_preview_path_ = path;
    cached_font_preview_write_time_ = error ? std::filesystem::file_time_type::min() : write_time;
    cached_font_preview_size_pixels_ = clamped_preview_size;
    cached_font_preview_width_ = preview_width;
    cached_font_preview_height_ = preview_height;
    return reinterpret_cast<ImTextureID>(cached_font_preview_descriptor_set_);
}