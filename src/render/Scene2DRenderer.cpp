#include "render/Scene2DRenderer.h"
#include "render/VideoPlaybackManager.h"
#include "vfs/AssetVFS.h"

#include <SDL3/SDL.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
// Resolve path to a compiled shader SPIR-V beside the executable.
std::filesystem::path ResolveShaderPath(const char* file_name)
{
    const char* base_path_raw = SDL_GetBasePath();
    const std::filesystem::path base_path =
        base_path_raw != nullptr ? std::filesystem::path(base_path_raw) : std::filesystem::current_path();
    return base_path / "shaders" / file_name;
}

std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

VkShaderModule LoadShaderModule(VkDevice device, const std::filesystem::path& path)
{
    const std::vector<std::uint8_t> bytes = ReadBinaryFile(path);
    if (bytes.empty())
    {
        SDL_Log("Scene2DRenderer: failed to read shader: %s", path.string().c_str());
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes.size();
    ci.pCode = reinterpret_cast<const std::uint32_t*>(bytes.data());
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(device, &ci, nullptr, &module);
    VulkanContext::CheckVkResult(result);
    return result == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

std::uint32_t FindMemoryType(VkPhysicalDevice physical_device,
                              std::uint32_t type_filter,
                              VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem_props = {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
    {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

bool CreateGpuBuffer(
    VkPhysicalDevice physical_device,
    VkDevice device,
    const VkAllocationCallbacks* allocator,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags props,
    VkBuffer& buffer,
    VkDeviceMemory& memory)
{
    VkBufferCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(device, &ci, allocator, &buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements req = {};
    vkGetBufferMemoryRequirements(device, buffer, &req);

    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(physical_device, req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyBuffer(device, buffer, allocator);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkAllocateMemory(device, &ai, allocator, &memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyBuffer(device, buffer, allocator);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindBufferMemory(device, buffer, memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, memory, allocator);
        vkDestroyBuffer(device, buffer, allocator);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

void TransitionImage(
    VkCommandBuffer cmd,
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

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Read a whole font file from disk.
std::vector<unsigned char> ReadFontFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return {};
    }
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::vector<unsigned char> ReadFontBytes(
    const std::filesystem::path& path,
    bool use_pak_streaming)
{
    if (use_pak_streaming)
    {
        if (!g_asset_reader)
        {
            return {};
        }
        return ReadAssetFileAsBytes(path.generic_string());
    }

    return ReadFontFile(path);
}

// Expand grayscale alpha-only bitmap to RGBA for GPU upload.
std::vector<unsigned char> ExpandAlphaBitmapToRGBA(
    const unsigned char* alpha_pixels,
    int width, int height,
    std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    std::vector<unsigned char> rgba(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    for (int i = 0; i < width * height; ++i)
    {
        rgba[static_cast<std::size_t>(i) * 4 + 0] = r;
        rgba[static_cast<std::size_t>(i) * 4 + 1] = g;
        rgba[static_cast<std::size_t>(i) * 4 + 2] = b;
        rgba[static_cast<std::size_t>(i) * 4 + 3] = alpha_pixels[i];
    }
    return rgba;
}

struct Vertex2D
{
    float x, y;   // NDC
    float u, v;   // UV
};

struct TextLayoutData
{
    std::vector<std::string> lines;
    float scale = 1.0f;
    int line_height_px = 1;
    int baseline_offset_px = 0;
    int bitmap_width = 1;
    int bitmap_height = 1;
};

float MeasureTextLineWidth(
    const stbtt_fontinfo& font_info,
    const std::string& text,
    float scale)
{
    float width = 0.0f;
    int previous_codepoint = 0;
    for (unsigned char ch : text)
    {
        int advance = 0;
        int bearing = 0;
        stbtt_GetCodepointHMetrics(&font_info, ch, &advance, &bearing);
        width += (advance + stbtt_GetCodepointKernAdvance(&font_info, previous_codepoint, ch)) * scale;
        previous_codepoint = ch;
    }

    return width;
}

TextLayoutData BuildTextLayout(
    const stbtt_fontinfo& font_info,
    const std::string& text,
    float font_size,
    float max_width_px)
{
    TextLayoutData layout;
    layout.scale = stbtt_ScaleForPixelHeight(&font_info, font_size);

    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &line_gap);
    layout.line_height_px = static_cast<int>(std::ceil((ascent - descent + line_gap) * layout.scale));
    layout.baseline_offset_px = static_cast<int>(std::ceil(ascent * layout.scale));

    std::string current_line;
    for (char ch : text)
    {
        if (ch == '\n')
        {
            layout.lines.push_back(current_line);
            current_line.clear();
        }
        else
        {
            current_line.push_back(ch);
        }
    }
    layout.lines.push_back(current_line);

    if (max_width_px > 0.0f)
    {
        std::vector<std::string> wrapped_lines;
        for (const std::string& raw_line : layout.lines)
        {
            if (raw_line.empty())
            {
                wrapped_lines.push_back("");
                continue;
            }

            std::string output_line;
            std::size_t position = 0;
            while (position <= raw_line.size())
            {
                const std::size_t space = raw_line.find(' ', position);
                const std::size_t word_end = (space != std::string::npos) ? space : raw_line.size();
                const std::string word = raw_line.substr(position, word_end - position);

                if (!word.empty())
                {
                    const std::string candidate = output_line.empty() ? word : (output_line + ' ' + word);
                    if (!output_line.empty() && MeasureTextLineWidth(font_info, candidate, layout.scale) > max_width_px)
                    {
                        wrapped_lines.push_back(output_line);
                        output_line.clear();
                    }

                    if (MeasureTextLineWidth(font_info, word, layout.scale) > max_width_px)
                    {
                        for (char ch : word)
                        {
                            const std::string trial = output_line + ch;
                            if (!output_line.empty() && MeasureTextLineWidth(font_info, trial, layout.scale) > max_width_px)
                            {
                                wrapped_lines.push_back(output_line);
                                output_line.clear();
                            }
                            output_line.push_back(ch);
                        }
                    }
                    else
                    {
                        output_line = output_line.empty() ? word : (output_line + ' ' + word);
                    }
                }

                if (space == std::string::npos)
                {
                    break;
                }

                position = space + 1;
            }

            wrapped_lines.push_back(output_line);
        }

        layout.lines = std::move(wrapped_lines);
    }

    int max_line_width = 1;
    for (const std::string& line : layout.lines)
    {
        max_line_width = (std::max)(
            max_line_width,
            static_cast<int>(std::ceil(MeasureTextLineWidth(font_info, line, layout.scale))));
    }

    layout.bitmap_width = (std::max)(1, max_line_width + 2);
    layout.bitmap_height = (std::max)(1, static_cast<int>(layout.lines.size()) * layout.line_height_px + 4);
    return layout;
}

} // namespace

Scene2DRenderer::~Scene2DRenderer()
{
    Shutdown();
}

bool Scene2DRenderer::CreateOrUpdateExternalTexture(
    const unsigned char* rgba_pixels,
    int width,
    int height,
    GpuTexture& tex)
{
    if (rgba_pixels == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }
    // If size changed, release and re-upload.
    if (tex.image != VK_NULL_HANDLE && (tex.width != width || tex.height != height))
    {
        ReleaseGpuTexture(tex);
    }
    if (tex.image == VK_NULL_HANDLE)
    {
        return UploadTexture(rgba_pixels, width, height, false, tex);
    }
    return UpdateTexture(rgba_pixels, width, height, false, tex);
}

void Scene2DRenderer::DestroyExternalTexture(GpuTexture& tex)
{
    ReleaseGpuTexture(tex);
}

bool Scene2DRenderer::Initialize(VulkanContext* context)
{
    if (context == nullptr)
    {
        return false;
    }
    vulkan_context_ = context;

    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    // Command pool
    VkCommandPoolCreateInfo pool_ci = {};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.queueFamilyIndex = vulkan_context_->GetQueueFamily();
    pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkResult result = vkCreateCommandPool(device, &pool_ci, allocator, &command_pool_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    // Sampler
    VkSamplerCreateInfo sampler_ci = {};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    result = vkCreateSampler(device, &sampler_ci, allocator, &sampler_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    // Descriptor set layout: single combined image sampler at binding 0
    VkDescriptorSetLayoutBinding dsl_binding = {};
    dsl_binding.binding = 0;
    dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dsl_binding.descriptorCount = 1;
    dsl_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dsl_ci = {};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 1;
    dsl_ci.pBindings = &dsl_binding;
    result = vkCreateDescriptorSetLayout(device, &dsl_ci, allocator, &descriptor_set_layout_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    // Descriptor pool
    VkDescriptorPoolSize pool_size = {};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 512;

    VkDescriptorPoolCreateInfo pool_pool_ci = {};
    pool_pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_pool_ci.maxSets = 512;
    pool_pool_ci.poolSizeCount = 1;
    pool_pool_ci.pPoolSizes = &pool_size;
    pool_pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    result = vkCreateDescriptorPool(device, &pool_pool_ci, allocator, &descriptor_pool_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    return true;
}

void Scene2DRenderer::Shutdown()
{
    if (vulkan_context_ == nullptr)
    {
        return;
    }

    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    // Wait for all GPU work to complete before releasing resources.
    vkDeviceWaitIdle(device);

    for (auto& [key, tex] : image_cache_)
    {
        ReleaseGpuTexture(tex);
    }
    image_cache_.clear();

    text_layout_cache_.clear();

    for (auto& [key, atlas] : font_atlas_cache_)
    {
        ReleaseGpuTexture(atlas.texture);
    }
    font_atlas_cache_.clear();

    font_cache_.clear();

    ClearFramebufferCache();

    if (vertex_buffer_ != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(device, vertex_buffer_, allocator);
        vertex_buffer_ = VK_NULL_HANDLE;
    }
    if (vertex_buffer_memory_ != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, vertex_buffer_memory_, allocator);
        vertex_buffer_memory_ = VK_NULL_HANDLE;
    }
    if (index_buffer_ != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(device, index_buffer_, allocator);
        index_buffer_ = VK_NULL_HANDLE;
    }
    if (index_buffer_memory_ != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, index_buffer_memory_, allocator);
        index_buffer_memory_ = VK_NULL_HANDLE;
    }

    if (pipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, pipeline_, allocator);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipeline_layout_ != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device, pipeline_layout_, allocator);
        pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(device, render_pass_, allocator);
        render_pass_ = VK_NULL_HANDLE;
    }
    if (descriptor_pool_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device, descriptor_pool_, allocator);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    if (descriptor_set_layout_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device, descriptor_set_layout_, allocator);
        descriptor_set_layout_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE)
    {
        vkDestroySampler(device, sampler_, allocator);
        sampler_ = VK_NULL_HANDLE;
    }
    if (command_pool_ != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(device, command_pool_, allocator);
        command_pool_ = VK_NULL_HANDLE;
    }

    pipeline_valid_ = false;
    vulkan_context_ = nullptr;
}

const std::vector<unsigned char>* Scene2DRenderer::GetOrLoadFontBytes(
    const std::filesystem::path& path,
    bool use_pak_streaming)
{
    const std::string mode_prefix = use_pak_streaming ? "pak:" : "fs:";
    const std::string cache_key = mode_prefix + path.generic_string();
    auto it = font_cache_.find(cache_key);
    if (it != font_cache_.end())
    {
        return &it->second;
    }

    std::vector<unsigned char> font_bytes = ReadFontBytes(path, use_pak_streaming);
    if (font_bytes.empty())
    {
        return nullptr;
    }

    auto insert_result = font_cache_.emplace(cache_key, std::move(font_bytes));
    return &insert_result.first->second;
}

Scene2DRenderer::FontAtlas* Scene2DRenderer::GetOrCreateFontAtlas(
    const std::string& font_path_abs,
    float font_size,
    bool use_pak_streaming,
    stbtt_fontinfo& font_info)
{
    const std::vector<unsigned char>* font_data = GetOrLoadFontBytes(font_path_abs, use_pak_streaming);
    if (font_data == nullptr)
    {
        return nullptr;
    }

    if (!stbtt_InitFont(&font_info, font_data->data(), stbtt_GetFontOffsetForIndex(font_data->data(), 0)))
    {
        return nullptr;
    }

    const std::string mode_prefix = use_pak_streaming ? "pak:" : "fs:";
    const FontAtlasKey key{mode_prefix + font_path_abs, font_size};
    auto it = font_atlas_cache_.find(key);
    if (it != font_atlas_cache_.end())
    {
        return &it->second;
    }

    FontAtlas atlas;
    atlas.alpha_bitmap.resize(static_cast<std::size_t>(atlas.width) * static_cast<std::size_t>(atlas.height), 0u);
    if (!UploadTexture(atlas.alpha_bitmap.data(), atlas.width, atlas.height, true, atlas.texture))
    {
        return nullptr;
    }

    auto insert_result = font_atlas_cache_.emplace(key, std::move(atlas));
    return &insert_result.first->second;
}

bool Scene2DRenderer::EnsureGlyphInAtlas(
    FontAtlas& atlas,
    const stbtt_fontinfo& font_info,
    std::uint32_t codepoint,
    float scale)
{
    if (atlas.glyphs.find(codepoint) != atlas.glyphs.end())
    {
        return true;
    }

    AtlasGlyph glyph;
    int advance = 0;
    int bearing = 0;
    stbtt_GetCodepointHMetrics(&font_info, static_cast<int>(codepoint), &advance, &bearing);
    glyph.advance = advance * scale;

    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    stbtt_GetCodepointBitmapBox(&font_info, static_cast<int>(codepoint), scale, scale, &x0, &y0, &x1, &y1);
    glyph.offset_x = x0;
    glyph.offset_y = y0;
    glyph.width = x1 - x0;
    glyph.height = y1 - y0;

    if (glyph.width > 0 && glyph.height > 0)
    {
        constexpr int kPadding = 1;
        if (atlas.next_x + glyph.width + kPadding > atlas.width)
        {
            atlas.next_x = 1;
            atlas.next_y += atlas.row_height + kPadding;
            atlas.row_height = 0;
        }

        if (atlas.next_y + glyph.height + kPadding > atlas.height)
        {
            SDL_Log("Scene2DRenderer: font atlas full for glyph %u", codepoint);
            return false;
        }

        std::vector<unsigned char> glyph_bitmap(static_cast<std::size_t>(glyph.width) * static_cast<std::size_t>(glyph.height));
        stbtt_MakeCodepointBitmap(
            &font_info,
            glyph_bitmap.data(),
            glyph.width,
            glyph.height,
            glyph.width,
            scale,
            scale,
            static_cast<int>(codepoint));

        for (int row = 0; row < glyph.height; ++row)
        {
            const int atlas_y = atlas.next_y + row;
            const std::size_t atlas_row_offset = static_cast<std::size_t>(atlas_y) * static_cast<std::size_t>(atlas.width);
            const std::size_t glyph_row_offset = static_cast<std::size_t>(row) * static_cast<std::size_t>(glyph.width);
            for (int column = 0; column < glyph.width; ++column)
            {
                atlas.alpha_bitmap[atlas_row_offset + static_cast<std::size_t>(atlas.next_x + column)] =
                    glyph_bitmap[glyph_row_offset + static_cast<std::size_t>(column)];
            }
        }

        glyph.u0 = static_cast<float>(atlas.next_x) / static_cast<float>(atlas.width);
        glyph.v0 = static_cast<float>(atlas.next_y) / static_cast<float>(atlas.height);
        glyph.u1 = static_cast<float>(atlas.next_x + glyph.width) / static_cast<float>(atlas.width);
        glyph.v1 = static_cast<float>(atlas.next_y + glyph.height) / static_cast<float>(atlas.height);

        atlas.next_x += glyph.width + kPadding;
        atlas.row_height = (std::max)(atlas.row_height, glyph.height);
        atlas.dirty = true;
    }

    atlas.glyphs.emplace(codepoint, glyph);
    return true;
}

void Scene2DRenderer::ReleaseGpuTexture(GpuTexture& tex)
{
    if (vulkan_context_ == nullptr)
    {
        tex = {};
        return;
    }
    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    if (tex.descriptor_set != VK_NULL_HANDLE && descriptor_pool_ != VK_NULL_HANDLE)
    {
        vkFreeDescriptorSets(device, descriptor_pool_, 1, &tex.descriptor_set);
    }
    if (tex.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, tex.view, allocator);
    }
    if (tex.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, tex.image, allocator);
    }
    if (tex.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, tex.memory, allocator);
    }
    tex = {};
}

void Scene2DRenderer::ClearFramebufferCache()
{
    if (vulkan_context_ == nullptr)
    {
        framebuffer_cache_.clear();
        return;
    }
    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    for (auto& [view, fb] : framebuffer_cache_)
    {
        if (fb != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(device, fb, allocator);
        }
    }
    framebuffer_cache_.clear();
}

bool Scene2DRenderer::EnsurePipeline()
{
    if (pipeline_valid_)
    {
        return true;
    }
    if (vulkan_context_ == nullptr)
    {
        return false;
    }

    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    // Render pass targeting R8G8B8A8_UNORM, load existing content (LOAD_OP_LOAD).
    VkAttachmentDescription attachment = {};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = {};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_ci = {};
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 1;
    rp_ci.pAttachments = &attachment;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &subpass;
    rp_ci.dependencyCount = 1;
    rp_ci.pDependencies = &dep;

    VkResult result = vkCreateRenderPass(device, &rp_ci, allocator, &render_pass_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    // Pipeline layout: descriptor set 0 + push constant (vec4 color)
    VkPushConstantRange pc_range = {};
    pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc_range.offset = 0;
    pc_range.size = sizeof(float) * 4;

    VkPipelineLayoutCreateInfo pl_ci = {};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &descriptor_set_layout_;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &pc_range;

    result = vkCreatePipelineLayout(device, &pl_ci, allocator, &pipeline_layout_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkShaderModule vert = LoadShaderModule(device, ResolveShaderPath("overlay2d.vert.spv"));
    VkShaderModule frag = LoadShaderModule(device, ResolveShaderPath("overlay2d.frag.spv"));
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
    {
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(device, vert, allocator);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(device, frag, allocator);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    // Vertex input: vec2 position (location 0), vec2 uv (location 1)
    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = sizeof(Vertex2D);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attribs[2] = {};
    attribs[0].location = 0;
    attribs[0].binding = 0;
    attribs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attribs[0].offset = 0;
    attribs[1].location = 1;
    attribs[1].binding = 0;
    attribs[1].format = VK_FORMAT_R32G32_SFLOAT;
    attribs[1].offset = sizeof(float) * 2;

    VkPipelineVertexInputStateCreateInfo vi_ci = {};
    vi_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi_ci.vertexBindingDescriptionCount = 1;
    vi_ci.pVertexBindingDescriptions = &binding;
    vi_ci.vertexAttributeDescriptionCount = 2;
    vi_ci.pVertexAttributeDescriptions = attribs;

    VkPipelineInputAssemblyStateCreateInfo ia_ci = {};
    ia_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia_ci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp_ci = {};
    vp_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_ci.viewportCount = 1;
    vp_ci.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs_ci = {};
    rs_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs_ci.polygonMode = VK_POLYGON_MODE_FILL;
    rs_ci.cullMode = VK_CULL_MODE_NONE;
    rs_ci.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs_ci.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms_ci = {};
    ms_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms_ci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Alpha blending: src_alpha * src + (1 - src_alpha) * dst
    VkPipelineColorBlendAttachmentState blend_attachment = {};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb_ci = {};
    cb_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb_ci.attachmentCount = 1;
    cb_ci.pAttachments = &blend_attachment;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn_ci = {};
    dyn_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn_ci.dynamicStateCount = 2;
    dyn_ci.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo gp_ci = {};
    gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_ci.stageCount = 2;
    gp_ci.pStages = stages;
    gp_ci.pVertexInputState = &vi_ci;
    gp_ci.pInputAssemblyState = &ia_ci;
    gp_ci.pViewportState = &vp_ci;
    gp_ci.pRasterizationState = &rs_ci;
    gp_ci.pMultisampleState = &ms_ci;
    gp_ci.pColorBlendState = &cb_ci;
    gp_ci.pDynamicState = &dyn_ci;
    gp_ci.layout = pipeline_layout_;
    gp_ci.renderPass = render_pass_;
    gp_ci.subpass = 0;

    result = vkCreateGraphicsPipelines(device, vulkan_context_->GetPipelineCache(), 1, &gp_ci, allocator, &pipeline_);
    vkDestroyShaderModule(device, vert, allocator);
    vkDestroyShaderModule(device, frag, allocator);

    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    pipeline_valid_ = true;
    return true;
}

bool Scene2DRenderer::EnsureVertexIndexBuffers()
{
    if (vertex_buffer_ != VK_NULL_HANDLE)
    {
        return true;
    }

    const VkDeviceSize vertex_size = static_cast<VkDeviceSize>(max_quads_) * 4 * sizeof(Vertex2D);
    const VkDeviceSize index_size = static_cast<VkDeviceSize>(max_quads_) * 6 * sizeof(std::uint16_t);

    if (!CreateGpuBuffer(
            vulkan_context_->GetPhysicalDevice(),
            vulkan_context_->GetDevice(),
            vulkan_context_->GetAllocator(),
            vertex_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            vertex_buffer_,
            vertex_buffer_memory_))
    {
        return false;
    }

    if (!CreateGpuBuffer(
            vulkan_context_->GetPhysicalDevice(),
            vulkan_context_->GetDevice(),
            vulkan_context_->GetAllocator(),
            index_size,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            index_buffer_,
            index_buffer_memory_))
    {
        return false;
    }

    // Pre-fill index buffer: each quad uses 6 indices (two triangles).
    void* mapped = nullptr;
    vkMapMemory(vulkan_context_->GetDevice(), index_buffer_memory_, 0, index_size, 0, &mapped);
    if (mapped != nullptr)
    {
        auto* indices = static_cast<std::uint16_t*>(mapped);
        for (std::uint32_t q = 0; q < max_quads_; ++q)
        {
            const std::uint16_t base = static_cast<std::uint16_t>(q * 4);
            indices[q * 6 + 0] = base + 0;
            indices[q * 6 + 1] = base + 1;
            indices[q * 6 + 2] = base + 2;
            indices[q * 6 + 3] = base + 2;
            indices[q * 6 + 4] = base + 3;
            indices[q * 6 + 5] = base + 0;
        }
        vkUnmapMemory(vulkan_context_->GetDevice(), index_buffer_memory_);
    }

    return true;
}

bool Scene2DRenderer::UploadTexture(
    const unsigned char* pixels,
    int width,
    int height,
    bool single_channel,
    GpuTexture& out_tex)
{
    if (vulkan_context_ == nullptr || pixels == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }

    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    // Always upload as RGBA8
    std::vector<unsigned char> rgba_data;
    const unsigned char* upload_pixels = pixels;
    if (single_channel)
    {
        rgba_data = ExpandAlphaBitmapToRGBA(pixels, width, height, 255, 255, 255);
        upload_pixels = rgba_data.data();
    }

    const VkDeviceSize upload_size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4u;

    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (!CreateGpuBuffer(
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
    if (result != VK_SUCCESS || mapped == nullptr)
    {
        vkFreeMemory(device, staging_memory, allocator);
        vkDestroyBuffer(device, staging_buffer, allocator);
        return false;
    }
    std::memcpy(mapped, upload_pixels, static_cast<std::size_t>(upload_size));
    vkUnmapMemory(device, staging_memory);

    // Create image
    VkImageCreateInfo img_ci = {};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_ci.extent.width = static_cast<std::uint32_t>(width);
    img_ci.extent.height = static_cast<std::uint32_t>(height);
    img_ci.extent.depth = 1;
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    result = vkCreateImage(device, &img_ci, allocator, &out_tex.image);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, staging_memory, allocator);
        vkDestroyBuffer(device, staging_buffer, allocator);
        return false;
    }

    VkMemoryRequirements mem_req = {};
    vkGetImageMemoryRequirements(device, out_tex.image, &mem_req);

    VkMemoryAllocateInfo mem_ai = {};
    mem_ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mem_ai.allocationSize = mem_req.size;
    mem_ai.memoryTypeIndex = FindMemoryType(
        vulkan_context_->GetPhysicalDevice(),
        mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    result = vkAllocateMemory(device, &mem_ai, allocator, &out_tex.memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, staging_memory, allocator);
        vkDestroyBuffer(device, staging_buffer, allocator);
        vkDestroyImage(device, out_tex.image, allocator);
        out_tex.image = VK_NULL_HANDLE;
        return false;
    }

    vkBindImageMemory(device, out_tex.image, out_tex.memory, 0);

    VkImageViewCreateInfo view_ci = {};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = out_tex.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;

    result = vkCreateImageView(device, &view_ci, allocator, &out_tex.view);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, staging_memory, allocator);
        vkDestroyBuffer(device, staging_buffer, allocator);
        ReleaseGpuTexture(out_tex);
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo ds_ai = {};
    ds_ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ds_ai.descriptorPool = descriptor_pool_;
    ds_ai.descriptorSetCount = 1;
    ds_ai.pSetLayouts = &descriptor_set_layout_;

    result = vkAllocateDescriptorSets(device, &ds_ai, &out_tex.descriptor_set);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, staging_memory, allocator);
        vkDestroyBuffer(device, staging_buffer, allocator);
        ReleaseGpuTexture(out_tex);
        return false;
    }

    VkDescriptorImageInfo img_info = {};
    img_info.sampler = sampler_;
    img_info.imageView = out_tex.view;
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = out_tex.descriptor_set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &img_info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    // Upload via staging buffer in an immediate command buffer
    VkCommandBufferAllocateInfo cmd_ai = {};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = command_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(device, &cmd_ai, &cmd);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, staging_memory, allocator);
        vkDestroyBuffer(device, staging_buffer, allocator);
        ReleaseGpuTexture(out_tex);
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    TransitionImage(cmd, out_tex.image,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy copy = {};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = static_cast<std::uint32_t>(width);
    copy.imageExtent.height = static_cast<std::uint32_t>(height);
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, staging_buffer, out_tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    TransitionImage(cmd, out_tex.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    result = vkCreateFence(device, &fence_info, allocator, &fence);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeCommandBuffers(device, command_pool_, 1, &cmd);
        vkFreeMemory(device, staging_memory, allocator);
        vkDestroyBuffer(device, staging_buffer, allocator);
        ReleaseGpuTexture(out_tex);
        return false;
    }

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    result = vkQueueSubmit(vulkan_context_->GetQueue(), 1, &submit, fence);
    VulkanContext::CheckVkResult(result);
    if (result == VK_SUCCESS)
    {
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        VulkanContext::CheckVkResult(result);
    }

    vkDestroyFence(device, fence, allocator);
    vkFreeCommandBuffers(device, command_pool_, 1, &cmd);

    vkFreeMemory(device, staging_memory, allocator);
    vkDestroyBuffer(device, staging_buffer, allocator);

    if (result != VK_SUCCESS)
    {
        ReleaseGpuTexture(out_tex);
        return false;
    }

    out_tex.width = width;
    out_tex.height = height;
    return true;
}

bool Scene2DRenderer::UpdateTexture(
    const unsigned char* pixels,
    int width,
    int height,
    bool single_channel,
    GpuTexture& texture)
{
    if (texture.image == VK_NULL_HANDLE)
    {
        return UploadTexture(pixels, width, height, single_channel, texture);
    }

    if (vulkan_context_ == nullptr || pixels == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }

    if (texture.width != width || texture.height != height)
    {
        return false;
    }

    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    std::vector<unsigned char> rgba_data;
    const unsigned char* upload_pixels = pixels;
    if (single_channel)
    {
        rgba_data = ExpandAlphaBitmapToRGBA(pixels, width, height, 255, 255, 255);
        upload_pixels = rgba_data.data();
    }

    const VkDeviceSize upload_size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4u;
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    if (!CreateGpuBuffer(
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
    if (result != VK_SUCCESS || mapped == nullptr)
    {
        vkFreeMemory(device, staging_memory, allocator);
        vkDestroyBuffer(device, staging_buffer, allocator);
        return false;
    }

    std::memcpy(mapped, upload_pixels, static_cast<std::size_t>(upload_size));
    vkUnmapMemory(device, staging_memory);

    VkCommandBufferAllocateInfo cmd_ai = {};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = command_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    result = vkAllocateCommandBuffers(device, &cmd_ai, &cmd);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, staging_memory, allocator);
        vkDestroyBuffer(device, staging_buffer, allocator);
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    TransitionImage(cmd, texture.image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy copy = {};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = static_cast<std::uint32_t>(width);
    copy.imageExtent.height = static_cast<std::uint32_t>(height);
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, staging_buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    TransitionImage(cmd, texture.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    result = vkCreateFence(device, &fence_info, allocator, &fence);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeCommandBuffers(device, command_pool_, 1, &cmd);
        vkFreeMemory(device, staging_memory, allocator);
        vkDestroyBuffer(device, staging_buffer, allocator);
        return false;
    }

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    result = vkQueueSubmit(vulkan_context_->GetQueue(), 1, &submit, fence);
    VulkanContext::CheckVkResult(result);
    if (result == VK_SUCCESS)
    {
        result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        VulkanContext::CheckVkResult(result);
    }

    vkDestroyFence(device, fence, allocator);
    vkFreeCommandBuffers(device, command_pool_, 1, &cmd);
    vkFreeMemory(device, staging_memory, allocator);
    vkDestroyBuffer(device, staging_buffer, allocator);
    return result == VK_SUCCESS;
}

Scene2DRenderer::GpuTexture* Scene2DRenderer::GetOrLoadImage(const std::filesystem::path& path, bool use_pak_streaming)
{
    const std::string mode_prefix = use_pak_streaming ? "pak:" : "fs:";
    const std::string key = mode_prefix + path.generic_string();
    auto it = image_cache_.find(key);
    if (it != image_cache_.end())
    {
        return &it->second;
    }

    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = nullptr;

    if (use_pak_streaming)
    {
        const std::vector<std::uint8_t> image_bytes = ReadAssetFileAsBytes(path.generic_string());
        if (!image_bytes.empty())
        {
            pixels = stbi_load_from_memory(
                image_bytes.data(),
                static_cast<int>(image_bytes.size()),
                &width,
                &height,
                &channels,
                4);
        }
    }
    else
    {
        pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    }

    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        return nullptr;
    }

    GpuTexture tex{};
    const bool ok = UploadTexture(pixels, width, height, false, tex);
    stbi_image_free(pixels);
    if (!ok)
    {
        return nullptr;
    }

    image_cache_[key] = tex;
    return &image_cache_[key];
}

bool Scene2DRenderer::GetText2DRenderSize(
    const std::filesystem::path& project_root,
    const SceneObjectText2DAttributes& text_attr,
    float& out_width,
    float& out_height)
{
    out_width = (std::max)(1.0f, text_attr.width);
    out_height = (std::max)(1.0f, text_attr.height);

    if (vulkan_context_ == nullptr || text_attr.text.empty())
    {
        return false;
    }

    const std::filesystem::path font_abs = text_attr.font_path.empty()
        ? std::filesystem::path{}
        : (text_attr.font_path.front() == '/' || (text_attr.font_path.size() >= 2 && text_attr.font_path[1] == ':')
            ? std::filesystem::path(text_attr.font_path)
            : project_root / text_attr.font_path);

    const bool use_pak_streaming = project_root.empty() && g_asset_reader != nullptr;
    const std::vector<unsigned char>* font_data = GetOrLoadFontBytes(font_abs, use_pak_streaming);
    if (font_data == nullptr)
    {
        return false;
    }

    stbtt_fontinfo font_info;
    if (!stbtt_InitFont(&font_info, font_data->data(), stbtt_GetFontOffsetForIndex(font_data->data(), 0)))
    {
        return false;
    }

    const TextLayoutData layout = BuildTextLayout(font_info, text_attr.text, text_attr.font_size, 0.0f);
    const float natural_width = (std::max)(1.0f, static_cast<float>(layout.bitmap_width));
    const float natural_height = (std::max)(1.0f, static_cast<float>(layout.bitmap_height));

    if (text_attr.lock_aspect_ratio)
    {
        const float stored_width = (std::max)(1.0f, text_attr.width);
        const float stored_height = (std::max)(1.0f, text_attr.height);
        const float scale_x = stored_width / natural_width;
        const float scale_y = stored_height / natural_height;
        const float scale = (std::max)(1.0f, (std::max)(scale_x, scale_y));
        out_width = natural_width * scale;
        out_height = natural_height * scale;
    }
    else
    {
        out_width = (std::max)((std::max)(1.0f, text_attr.width), natural_width);
        out_height = (std::max)((std::max)(1.0f, text_attr.height), natural_height);
    }
    return true;
}

Scene2DRenderer::TextLayoutCacheEntry* Scene2DRenderer::GetOrBuildTextLayout(
    const std::string& font_path_abs,
    const std::string& text,
    float font_size,
    float max_width_px,
    bool use_pak_streaming)
{
    if (font_path_abs.empty() || text.empty())
    {
        return nullptr;
    }

    const std::string mode_prefix = use_pak_streaming ? "pak:" : "fs:";
    const TextCacheKey key{mode_prefix + font_path_abs, text, font_size, max_width_px};
    auto it = text_layout_cache_.find(key);
    if (it != text_layout_cache_.end())
    {
        return &it->second;
    }

    stbtt_fontinfo font_info;
    FontAtlas* atlas = GetOrCreateFontAtlas(font_path_abs, font_size, use_pak_streaming, font_info);
    if (atlas == nullptr)
    {
        return nullptr;
    }

    const TextLayoutData layout = BuildTextLayout(font_info, text, font_size, max_width_px);

    TextLayoutCacheEntry entry;
    entry.atlas_key = FontAtlasKey{mode_prefix + font_path_abs, font_size};
    entry.width = layout.bitmap_width;
    entry.height = layout.bitmap_height;

    for (int line_index = 0; line_index < static_cast<int>(layout.lines.size()); ++line_index)
    {
        const std::string& line = layout.lines[static_cast<std::size_t>(line_index)];
        float pen_x = 1.0f;
        const int baseline_y = line_index * layout.line_height_px + layout.baseline_offset_px;
        int prev_codepoint = 0;
        for (unsigned char ch : line)
        {
            const std::uint32_t codepoint = static_cast<std::uint32_t>(ch);
            if (!EnsureGlyphInAtlas(*atlas, font_info, codepoint, layout.scale))
            {
                return nullptr;
            }

            const AtlasGlyph& glyph = atlas->glyphs[codepoint];
            if (glyph.width > 0 && glyph.height > 0)
            {
                PositionedGlyph positioned_glyph;
                positioned_glyph.x = static_cast<float>(static_cast<int>(std::round(pen_x)) + glyph.offset_x);
                positioned_glyph.y = static_cast<float>(baseline_y + glyph.offset_y);
                positioned_glyph.width = static_cast<float>(glyph.width);
                positioned_glyph.height = static_cast<float>(glyph.height);
                positioned_glyph.u0 = glyph.u0;
                positioned_glyph.v0 = glyph.v0;
                positioned_glyph.u1 = glyph.u1;
                positioned_glyph.v1 = glyph.v1;
                entry.glyphs.push_back(positioned_glyph);
            }

            int advance = 0, bearing = 0;
            stbtt_GetCodepointHMetrics(&font_info, ch, &advance, &bearing);
            int kern = stbtt_GetCodepointKernAdvance(&font_info, prev_codepoint, ch);
            pen_x += (advance + kern) * layout.scale;
            prev_codepoint = static_cast<int>(codepoint);
        }
    }

    if (atlas->dirty)
    {
        if (!UpdateTexture(atlas->alpha_bitmap.data(), atlas->width, atlas->height, true, atlas->texture))
        {
            return nullptr;
        }
        atlas->dirty = false;
    }

    auto insert_result = text_layout_cache_.emplace(key, std::move(entry));
    return &insert_result.first->second;
}

VkFramebuffer Scene2DRenderer::GetOrCreateFramebuffer(
    VkImageView view,
    std::uint32_t w,
    std::uint32_t h)
{
    auto it = framebuffer_cache_.find(view);
    if (it != framebuffer_cache_.end())
    {
        return it->second;
    }

    VkFramebufferCreateInfo fb_ci = {};
    fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_ci.renderPass = render_pass_;
    fb_ci.attachmentCount = 1;
    fb_ci.pAttachments = &view;
    fb_ci.width = w;
    fb_ci.height = h;
    fb_ci.layers = 1;

    VkFramebuffer fb = VK_NULL_HANDLE;
    const VkResult result = vkCreateFramebuffer(
        vulkan_context_->GetDevice(), &fb_ci, vulkan_context_->GetAllocator(), &fb);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }

    framebuffer_cache_[view] = fb;
    return fb;
}

void Scene2DRenderer::DrawQuad(
    VkCommandBuffer cmd,
    const GpuTexture& tex,
    std::uint32_t quad_index,
    float x_norm, float y_norm, float w_norm, float h_norm,
    float r, float g, float b, float a,
    float u0, float v0,
    float u1, float v1)
{
    if (tex.descriptor_set == VK_NULL_HANDLE || quad_index >= max_quads_)
    {
        return;
    }

    // Convert normalized (0-1) coordinates directly to NDC.
    // Normalized coordinates: (0,0) = top-left, (1,1) = bottom-right
    // NDC: (-1,-1) = bottom-left, (1,1) = top-right
    const float ndc_x0 = x_norm * 2.0f - 1.0f;
    const float ndc_x1 = (x_norm + w_norm) * 2.0f - 1.0f;
    const float ndc_y0 = 1.0f - y_norm * 2.0f;
    const float ndc_y1 = 1.0f - (y_norm + h_norm) * 2.0f;

    // Quad: top-left, top-right, bottom-right, bottom-left
    const Vertex2D verts[4] = {
        {ndc_x0, ndc_y0, u0, v0}, // top-left
        {ndc_x1, ndc_y0, u1, v0}, // top-right
        {ndc_x1, ndc_y1, u1, v1}, // bottom-right
        {ndc_x0, ndc_y1, u0, v1}, // bottom-left
    };

    void* mapped = nullptr;
    const VkDeviceSize vertex_offset_bytes = static_cast<VkDeviceSize>(quad_index) * 4 * sizeof(Vertex2D);
    vkMapMemory(vulkan_context_->GetDevice(), vertex_buffer_memory_, vertex_offset_bytes,
                4 * sizeof(Vertex2D), 0, &mapped);
    if (mapped != nullptr)
    {
        std::memcpy(mapped, verts, 4 * sizeof(Vertex2D));
        vkUnmapMemory(vulkan_context_->GetDevice(), vertex_buffer_memory_);
    }

    // Bind descriptor set and push constants
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_layout_, 0, 1, &tex.descriptor_set, 0, nullptr);

    const float push[4] = {r, g, b, a};
    vkCmdPushConstants(cmd, pipeline_layout_,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(push), push);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_, &offset);
    vkCmdBindIndexBuffer(cmd, index_buffer_, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cmd, 6, 1, quad_index * 6, 0, 0);
}

void Scene2DRenderer::CompositeOverlay(
    const SceneMetadata& scene_metadata,
    const std::filesystem::path& project_root,
    VkImage target_image,
    VkImageView target_view,
    std::uint32_t width,
    std::uint32_t height,
    float* out_gpu_wait_ms)
{
    if (out_gpu_wait_ms != nullptr)
    {
        *out_gpu_wait_ms = 0.0f;
    }

        const bool use_pak_streaming = project_root.empty() && g_asset_reader != nullptr;

    if (vulkan_context_ == nullptr || target_image == VK_NULL_HANDLE || target_view == VK_NULL_HANDLE)
    {
        return;
    }
    if (width == 0 || height == 0)
    {
        return;
    }

    // Collect 2D draw commands from scene
    bool has_any = false;
    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        for (const SceneObjectAttribute& attr : object.attributes)
        {
            if (attr.kind == SceneObjectAttributeKind::Text2D ||
                attr.kind == SceneObjectAttributeKind::Image2D ||
                attr.kind == SceneObjectAttributeKind::Video2D)
            {
                has_any = true;
                break;
            }
        }
        if (has_any) break;
    }

    if (!has_any)
    {
        return;
    }

    if (!EnsurePipeline() || !EnsureVertexIndexBuffers())
    {
        return;
    }

    // Invalidate framebuffer cache if size changed (simple heuristic: check if
    // we have cached framebuffers for this view at the right size).
    // For now, just recreate if the view is not cached.
    VkFramebuffer framebuffer = GetOrCreateFramebuffer(target_view, width, height);
    if (framebuffer == VK_NULL_HANDLE)
    {
        return;
    }

    const VkDevice device = vulkan_context_->GetDevice();

    // Record and submit overlay commands
    VkCommandBufferAllocateInfo cmd_ai = {};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = command_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(device, &cmd_ai, &cmd);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin_info);

    // Transition: SHADER_READ_ONLY_OPTIMAL -> COLOR_ATTACHMENT_OPTIMAL
    TransitionImage(cmd, target_image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderPassBeginInfo rp_begin = {};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass_;
    rp_begin.framebuffer = framebuffer;
    rp_begin.renderArea.extent.width = width;
    rp_begin.renderArea.extent.height = height;
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport vp = {};
    vp.width = static_cast<float>(width);
    vp.height = static_cast<float>(height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor = {};
    scissor.extent.width = width;
    scissor.extent.height = height;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    const float safe_ref_width = static_cast<float>((std::max)(1u, scene_metadata.reference_viewport_width));
    const float safe_ref_height = static_cast<float>((std::max)(1u, scene_metadata.reference_viewport_height));
    const float scale_x = static_cast<float>(width) / safe_ref_width;
    const float scale_y = static_cast<float>(height) / safe_ref_height;
    const float overlay_size_scale = (std::min)(scale_x, scale_y);

    // Collect overlays (Text2D and Image2D) with their priorities, then sort by priority (lower priority first = render in front)
    struct OverlayEntry
    {
        std::size_t object_index;
        std::size_t attr_index;
        int priority;
    };
    
    std::vector<OverlayEntry> overlay_entries;
    for (std::size_t obj_idx = 0; obj_idx < scene_metadata.objects.size(); ++obj_idx)
    {
        const SceneObjectMetadata& object = scene_metadata.objects[obj_idx];
        for (std::size_t attr_idx = 0; attr_idx < object.attributes.size(); ++attr_idx)
        {
            const SceneObjectAttribute& attr = object.attributes[attr_idx];
            if (attr.kind == SceneObjectAttributeKind::Text2D)
            {
                const SceneObjectText2DAttributes& t = attr.text_2d;
                overlay_entries.push_back({obj_idx, attr_idx, t.priority});
            }
            else if (attr.kind == SceneObjectAttributeKind::Image2D)
            {
                const SceneObjectImage2DAttributes& img = attr.image_2d;
                if (img.image_path.empty())
                {
                    continue;
                }
                overlay_entries.push_back({obj_idx, attr_idx, img.priority});
            }
            else if (attr.kind == SceneObjectAttributeKind::Video2D)
            {
                const SceneObjectVideo2DAttributes& vid = attr.video_2d;
                if (vid.video_path.empty())
                {
                    continue;
                }
                overlay_entries.push_back({obj_idx, attr_idx, vid.priority});
            }
        }
    }

    // Sort by priority descending (higher priority drawn first = behind, lower priority drawn last = on top)
    std::stable_sort(overlay_entries.begin(), overlay_entries.end(),
        [](const OverlayEntry& a, const OverlayEntry& b) { return a.priority > b.priority; });

    std::uint32_t quad_index = 0;
    for (const OverlayEntry& entry : overlay_entries)
    {
        if (quad_index >= max_quads_)
        {
            break;
        }

        const SceneObjectMetadata& object = scene_metadata.objects[entry.object_index];
        const SceneObjectAttribute& attr = object.attributes[entry.attr_index];
        if (attr.kind == SceneObjectAttributeKind::Text2D)
        {
            const SceneObjectText2DAttributes& t = attr.text_2d;
                if (t.text.empty())
                {
                    continue;
                }

                const std::filesystem::path font_abs = t.font_path.empty()
                    ? std::filesystem::path{}
                    : (t.font_path.front() == '/' || (t.font_path.size() >= 2 && t.font_path[1] == ':')
                        ? std::filesystem::path(t.font_path)
                        : project_root / t.font_path);

                const float wrap_width = 0.0f;
                TextLayoutCacheEntry* layout = GetOrBuildTextLayout(
                    font_abs.generic_string(), t.text, t.font_size, wrap_width, use_pak_streaming);
                if (layout == nullptr)
                {
                    continue;
                }

                auto atlas_it = font_atlas_cache_.find(layout->atlas_key);
                if (atlas_it == font_atlas_cache_.end())
                {
                    continue;
                }

                const GpuTexture& atlas_texture = atlas_it->second.texture;

                float render_w = t.width;
                float render_h = t.height;
                GetText2DRenderSize(project_root, t, render_w, render_h);
                
                const float screen_w = render_w * overlay_size_scale;
                const float screen_h = render_h * overlay_size_scale;
                const float reference_range_x = (std::max)(safe_ref_width - render_w, 1.0f);
                const float reference_range_y = (std::max)(safe_ref_height - render_h, 1.0f);
                const float screen_range_x = (std::max)(static_cast<float>(width) - screen_w, 0.0f);
                const float screen_range_y = (std::max)(static_cast<float>(height) - screen_h, 0.0f);
                const float screen_x = (t.x / reference_range_x) * screen_range_x;
                const float screen_y = (t.y / reference_range_y) * screen_range_y;
                const float natural_width = (std::max)(1.0f, static_cast<float>(layout->width));
                const float natural_height = (std::max)(1.0f, static_cast<float>(layout->height));
                const float glyph_scale_x = screen_w / natural_width;
                const float glyph_scale_y = screen_h / natural_height;

                for (const PositionedGlyph& glyph : layout->glyphs)
                {
                    if (quad_index >= max_quads_)
                    {
                        break;
                    }

                    const float glyph_screen_x = screen_x + glyph.x * glyph_scale_x;
                    const float glyph_screen_y = screen_y + glyph.y * glyph_scale_y;
                    const float glyph_screen_w = glyph.width * glyph_scale_x;
                    const float glyph_screen_h = glyph.height * glyph_scale_y;
                    const float norm_x = glyph_screen_x / static_cast<float>((std::max)(1u, width));
                    const float norm_y = glyph_screen_y / static_cast<float>((std::max)(1u, height));
                    const float norm_w = glyph_screen_w / static_cast<float>((std::max)(1u, width));
                    const float norm_h = glyph_screen_h / static_cast<float>((std::max)(1u, height));

                    DrawQuad(cmd, atlas_texture,
                        quad_index,
                        norm_x, norm_y, norm_w, norm_h,
                        t.color[0], t.color[1], t.color[2], t.alpha,
                        glyph.u0, glyph.v0, glyph.u1, glyph.v1);
                    ++quad_index;
                }
            }
            else if (attr.kind == SceneObjectAttributeKind::Image2D)
            {
                const SceneObjectImage2DAttributes& img = attr.image_2d;
                if (img.image_path.empty())
                {
                    continue;
                }

                const std::filesystem::path img_abs =
                    (img.image_path.front() == '/' || (img.image_path.size() >= 2 && img.image_path[1] == ':')
                        ? std::filesystem::path(img.image_path)
                        : project_root / img.image_path);

                GpuTexture* tex = GetOrLoadImage(img_abs, use_pak_streaming);
                if (tex == nullptr)
                {
                    continue;
                }

                const float screen_w = img.width * overlay_size_scale;
                const float screen_h = img.height * overlay_size_scale;
                const float reference_range_x = (std::max)(safe_ref_width - img.width, 1.0f);
                const float reference_range_y = (std::max)(safe_ref_height - img.height, 1.0f);
                const float screen_range_x = (std::max)(static_cast<float>(width) - screen_w, 0.0f);
                const float screen_range_y = (std::max)(static_cast<float>(height) - screen_h, 0.0f);
                const float screen_x = (img.x / reference_range_x) * screen_range_x;
                const float screen_y = (img.y / reference_range_y) * screen_range_y;
                const float norm_x = screen_x / static_cast<float>((std::max)(1u, width));
                const float norm_y = screen_y / static_cast<float>((std::max)(1u, height));
                const float norm_w = screen_w / static_cast<float>((std::max)(1u, width));
                const float norm_h = screen_h / static_cast<float>((std::max)(1u, height));
                
                DrawQuad(cmd, *tex,
                    quad_index,
                    norm_x, norm_y, norm_w, norm_h,
                    img.tint[0], img.tint[1], img.tint[2], img.alpha);
                ++quad_index;
            }
            else if (attr.kind == SceneObjectAttributeKind::Video2D)
            {
                const SceneObjectVideo2DAttributes& vid = attr.video_2d;
                if (vid.video_path.empty() || video_playback_manager_ == nullptr)
                {
                    continue;
                }

                const GpuTexture* tex = video_playback_manager_->GetFrameTexture(object.name, entry.attr_index);
                if (tex == nullptr || tex->descriptor_set == VK_NULL_HANDLE)
                {
                    continue;
                }

                if (vid.stretch_to_screen)
                {
                    DrawQuad(cmd, *tex,
                        quad_index,
                        0.0f, 0.0f, 1.0f, 1.0f,
                        vid.tint[0], vid.tint[1], vid.tint[2], vid.alpha);
                    ++quad_index;
                    continue;
                }

                // Optionally honor lock_aspect_ratio against the video's natural
                // size (texture dims). When locked, fit inside vid.width/vid.height.
                float effective_w = vid.width;
                float effective_h = vid.height;
                if (vid.lock_aspect_ratio && tex->width > 0 && tex->height > 0)
                {
                    const float src_aspect = static_cast<float>(tex->width) / static_cast<float>(tex->height);
                    const float box_aspect = effective_w / (std::max)(1.0f, effective_h);
                    if (src_aspect > box_aspect)
                    {
                        effective_h = effective_w / src_aspect;
                    }
                    else
                    {
                        effective_w = effective_h * src_aspect;
                    }
                }

                const float screen_w = effective_w * overlay_size_scale;
                const float screen_h = effective_h * overlay_size_scale;
                const float reference_range_x = (std::max)(safe_ref_width - effective_w, 1.0f);
                const float reference_range_y = (std::max)(safe_ref_height - effective_h, 1.0f);
                const float screen_range_x = (std::max)(static_cast<float>(width) - screen_w, 0.0f);
                const float screen_range_y = (std::max)(static_cast<float>(height) - screen_h, 0.0f);
                const float screen_x = (vid.x / reference_range_x) * screen_range_x;
                const float screen_y = (vid.y / reference_range_y) * screen_range_y;
                const float norm_x = screen_x / static_cast<float>((std::max)(1u, width));
                const float norm_y = screen_y / static_cast<float>((std::max)(1u, height));
                const float norm_w = screen_w / static_cast<float>((std::max)(1u, width));
                const float norm_h = screen_h / static_cast<float>((std::max)(1u, height));

                DrawQuad(cmd, *tex,
                    quad_index,
                    norm_x, norm_y, norm_w, norm_h,
                    vid.tint[0], vid.tint[1], vid.tint[2], vid.alpha);
                ++quad_index;
            }
    }

    vkCmdEndRenderPass(cmd);

    // Transition back: COLOR_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
    TransitionImage(cmd, target_image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT);

    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    result = vkCreateFence(device, &fence_info, vulkan_context_->GetAllocator(), &fence);
    VulkanContext::CheckVkResult(result);
    if (result == VK_SUCCESS)
    {
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        result = vkQueueSubmit(vulkan_context_->GetQueue(), 1, &submit, fence);
        VulkanContext::CheckVkResult(result);
        if (result == VK_SUCCESS)
        {
            const std::uint64_t wait_start_ticks = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
            result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            VulkanContext::CheckVkResult(result);
            if (out_gpu_wait_ms != nullptr)
            {
                const std::uint64_t wait_end_ticks = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
                const std::uint64_t freq = static_cast<std::uint64_t>(SDL_GetPerformanceFrequency());
                if (freq > 0)
                {
                    const double elapsed_ms = static_cast<double>(wait_end_ticks - wait_start_ticks) * 1000.0 / static_cast<double>(freq);
                    *out_gpu_wait_ms = static_cast<float>(elapsed_ms);
                }
            }
        }
        vkDestroyFence(device, fence, vulkan_context_->GetAllocator());
    }
    vkFreeCommandBuffers(device, command_pool_, 1, &cmd);
}
