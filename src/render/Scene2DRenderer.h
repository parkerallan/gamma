#pragma once

#include "app/VulkanContext.h"
#include "assets/SceneMetadata.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct stbtt_fontinfo;

// Shared 2D overlay renderer. Composites Text2D and Image2D attribute quads
// directly on top of a VkImage (VK_FORMAT_R8G8B8A8_UNORM) that has
// VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT. The image must be in
// VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL on entry and is left in the same
// layout on return. Used by both SceneViewportRenderer and RuntimeRenderer so
// viewport and runtime have identical 2D rendering.
class Scene2DRenderer
{
public:
    ~Scene2DRenderer();

    bool Initialize(VulkanContext* context);
    void Shutdown();

    // Composite all Text2D and Image2D attributes from scene_metadata onto
    // target_image. Paths in attributes are relative to project_root.
    // If out_gpu_wait_ms is non-null, the time spent in vkWaitForFences after
    // submitting the overlay command buffer is written to it. Callers can
    // reattribute that wait (which can stall on previously queued GPU work
    // such as ray tracing) away from the 2D subsystem timing.
    void CompositeOverlay(
        const SceneMetadata& scene_metadata,
        const std::filesystem::path& project_root,
        VkImage target_image,
        VkImageView target_view,
        std::uint32_t width,
        std::uint32_t height,
        float* out_gpu_wait_ms = nullptr);

    // Returns the effective rendered width/height for a Text2D attribute.
    // If lock_aspect_ratio is disabled, size reflects rasterized text bounds.
    bool GetText2DRenderSize(
        const std::filesystem::path& project_root,
        const SceneObjectText2DAttributes& text_attr,
        float& out_width,
        float& out_height);

private:
    struct GpuTexture
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
    };

    struct TextCacheKey
    {
        std::string font_path;
        std::string text;
        float font_size = 0.0f;
        float max_width_px = 0.0f;

        bool operator==(const TextCacheKey& other) const
        {
            return font_path == other.font_path &&
                   text == other.text &&
                   font_size == other.font_size &&
                   max_width_px == other.max_width_px;
        }
    };

    struct FontAtlasKey
    {
        std::string font_path;
        float font_size = 0.0f;

        bool operator==(const FontAtlasKey& other) const
        {
            return font_path == other.font_path &&
                   font_size == other.font_size;
        }
    };

    struct TextCacheKeyHash
    {
        std::size_t operator()(const TextCacheKey& k) const
        {
            std::size_t h = std::hash<std::string>{}(k.font_path);
            h ^= std::hash<std::string>{}(k.text) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<float>{}(k.font_size) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<float>{}(k.max_width_px) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct FontAtlasKeyHash
    {
        std::size_t operator()(const FontAtlasKey& k) const
        {
            std::size_t h = std::hash<std::string>{}(k.font_path);
            h ^= std::hash<float>{}(k.font_size) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    struct AtlasGlyph
    {
        int width = 0;
        int height = 0;
        int offset_x = 0;
        int offset_y = 0;
        float advance = 0.0f;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
    };

    struct FontAtlas
    {
        GpuTexture texture{};
        int width = 1024;
        int height = 1024;
        int next_x = 1;
        int next_y = 1;
        int row_height = 0;
        bool dirty = false;
        std::vector<unsigned char> alpha_bitmap;
        std::unordered_map<std::uint32_t, AtlasGlyph> glyphs;
    };

    struct PositionedGlyph
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
    };

    struct TextLayoutCacheEntry
    {
        FontAtlasKey atlas_key{};
        int width = 1;
        int height = 1;
        std::vector<PositionedGlyph> glyphs;
    };

    VulkanContext* vulkan_context_ = nullptr;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    // Per-frame vertex/index buffers for batched quad drawing
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_buffer_memory_ = VK_NULL_HANDLE;
    VkBuffer index_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory index_buffer_memory_ = VK_NULL_HANDLE;
    std::uint32_t max_quads_ = 4096;

    // Cached framebuffers keyed by target VkImageView
    std::unordered_map<VkImageView, VkFramebuffer> framebuffer_cache_;

    // Cached font bytes and uploaded textures
    std::unordered_map<std::string, std::vector<unsigned char>> font_cache_;
    std::unordered_map<FontAtlasKey, FontAtlas, FontAtlasKeyHash> font_atlas_cache_;
    std::unordered_map<std::string, GpuTexture> image_cache_;
    std::unordered_map<TextCacheKey, TextLayoutCacheEntry, TextCacheKeyHash> text_layout_cache_;

    bool pipeline_valid_ = false;

    void ReleaseGpuTexture(GpuTexture& tex);
    void ClearFramebufferCache();

    bool EnsurePipeline();
    bool EnsureVertexIndexBuffers();

    const std::vector<unsigned char>* GetOrLoadFontBytes(
        const std::filesystem::path& path,
        bool use_pak_streaming);
    FontAtlas* GetOrCreateFontAtlas(
        const std::string& font_path_abs,
        float font_size,
        bool use_pak_streaming,
        stbtt_fontinfo& font_info);
    bool EnsureGlyphInAtlas(
        FontAtlas& atlas,
        const stbtt_fontinfo& font_info,
        std::uint32_t codepoint,
        float scale);
    GpuTexture* GetOrLoadImage(const std::filesystem::path& path, bool use_pak_streaming);
    TextLayoutCacheEntry* GetOrBuildTextLayout(const std::string& font_path_abs,
                                               const std::string& text,
                                               float font_size,
                                               float max_width_px,
                                               bool use_pak_streaming);

    bool UploadTexture(const unsigned char* pixels, int width, int height,
                       bool single_channel, GpuTexture& out_tex);
    bool UpdateTexture(const unsigned char* pixels, int width, int height,
                       bool single_channel, GpuTexture& texture);

    VkFramebuffer GetOrCreateFramebuffer(VkImageView view, std::uint32_t w, std::uint32_t h);

    // Draw a quad with normalized (0-1) coordinates that scale with viewport size.
    // x_norm, y_norm, w_norm, h_norm should be in 0-1 range where:
    // - (0,0) is top-left, (1,1) is bottom-right
    // - coordinates automatically scale with viewport_w and viewport_h
    void DrawQuad(VkCommandBuffer cmd, const GpuTexture& tex,
                  std::uint32_t quad_index,
                  float x_norm, float y_norm, float w_norm, float h_norm,
                  float r, float g, float b, float a,
                  float u0 = 0.0f, float v0 = 0.0f,
                  float u1 = 1.0f, float v1 = 1.0f);
};
