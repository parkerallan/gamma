#pragma once

#include "app/VulkanContext.h"
#include "assets/SceneMetadata.h"

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

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
    void CompositeOverlay(
        const SceneMetadata& scene_metadata,
        const std::filesystem::path& project_root,
        VkImage target_image,
        VkImageView target_view,
        std::uint32_t width,
        std::uint32_t height);

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
    std::uint32_t max_quads_ = 256;

    // Cached framebuffers keyed by target VkImageView
    std::unordered_map<VkImageView, VkFramebuffer> framebuffer_cache_;

    // Cached uploaded textures
    std::unordered_map<std::string, GpuTexture> image_cache_;
    std::unordered_map<TextCacheKey, GpuTexture, TextCacheKeyHash> text_cache_;

    bool pipeline_valid_ = false;

    void ReleaseGpuTexture(GpuTexture& tex);
    void ClearFramebufferCache();

    bool EnsurePipeline();
    bool EnsureVertexIndexBuffers();

    GpuTexture* GetOrLoadImage(const std::filesystem::path& path);
    GpuTexture* GetOrRasterizeText(const std::string& font_path_abs,
                                   const std::string& text,
                                   float font_size,
                                   float max_width_px);

    bool UploadTexture(const unsigned char* pixels, int width, int height,
                       bool single_channel, GpuTexture& out_tex);

    VkFramebuffer GetOrCreateFramebuffer(VkImageView view, std::uint32_t w, std::uint32_t h);

    void DrawQuad(VkCommandBuffer cmd, const GpuTexture& tex,
                  std::uint32_t quad_index,
                  float x_px, float y_px, float w_px, float h_px,
                  float r, float g, float b, float a,
                  std::uint32_t viewport_w, std::uint32_t viewport_h);
};
