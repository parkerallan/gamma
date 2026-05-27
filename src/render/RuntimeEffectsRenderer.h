#pragma once

#include "app/VulkanContext.h"
#include "assets/SceneMetadata.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef GAMMA_WITH_EFFEKSEER
#include <Effekseer.h>
#include <EffekseerRendererVulkan.h>
#endif

class RuntimeEffectsRenderer
{
public:
    struct QueuedEffect
    {
        std::string key;
        std::filesystem::path effect_path;
        SceneObjectEffectsPlayMode play_mode = SceneObjectEffectsPlayMode::Stop;
        std::array<float, 16> world_matrix{};
    };

    RuntimeEffectsRenderer() = default;
    ~RuntimeEffectsRenderer();

    RuntimeEffectsRenderer(const RuntimeEffectsRenderer&) = delete;
    RuntimeEffectsRenderer& operator=(const RuntimeEffectsRenderer&) = delete;

    bool Initialize(VulkanContext* context);
    void Shutdown();
    void ResetPlayback();

    bool RenderEffects(
        VkImage target_image,
        VkImageView target_view,
        VkImageLayout current_layout,
        std::uint32_t target_width,
        std::uint32_t target_height,
        VkImage scene_depth_image,
        VkImageView scene_depth_view,
        const std::array<float, 16>& camera_world_matrix,
        const SceneObjectCameraAttributes& camera,
        const std::vector<QueuedEffect>& effects,
        VkImageLayout& out_layout,
        std::vector<std::string>* out_completed_play_once_keys = nullptr,
        std::string* error_message = nullptr);

    bool IsAvailable() const;

private:
    bool EnsureInitialized(std::string* error_message);
    bool EnsureRenderTarget(VkImageView target_view, std::uint32_t target_width, std::uint32_t target_height, std::string* error_message);
#ifdef GAMMA_WITH_EFFEKSEER
    bool LoadEffect(const std::filesystem::path& effect_path, Effekseer::EffectRef& out_effect, std::string* error_message);
#endif
    void StopMissingEffects(const std::vector<QueuedEffect>& effects);
    void DestroyRenderTarget();
    void DestroyEffekseer();
    void DestroyDepthFill();
    void SetError(std::string* error_message, const std::string& message);

    VulkanContext* context_ = nullptr;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence render_fence_ = VK_NULL_HANDLE;
    // Main effects render pass (color+depth, depth loadOp=LOAD).
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkImageView framebuffer_view_ = VK_NULL_HANDLE;
    // Standalone hardware depth buffer for the effects pass.
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    VkImageLayout depth_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    // Depth-fill pass: renders scene linear depth → NDC hardware depth before effects.
    VkRenderPass depth_fill_rp_ = VK_NULL_HANDLE;
    VkFramebuffer depth_fill_fb_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout depth_fill_dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout depth_fill_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline depth_fill_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool depth_fill_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet depth_fill_set_ = VK_NULL_HANDLE;
    std::uint32_t target_width_ = 0;
    std::uint32_t target_height_ = 0;
    std::uint64_t last_update_ticks_ = 0;

#ifdef GAMMA_WITH_EFFEKSEER
    struct ActiveEffect
    {
        Effekseer::Handle handle = -1;
        std::filesystem::path path;
        SceneObjectEffectsPlayMode play_mode = SceneObjectEffectsPlayMode::Stop;
        bool play_once_started = false;
        bool draining = false;  // StopRootEffect called; waiting for natural finish
    };

    Effekseer::FileInterfaceRef file_interface_;
    Effekseer::ManagerRef manager_;
    Effekseer::Backend::GraphicsDeviceRef graphics_device_;
    EffekseerRenderer::RendererRef renderer_;
    Effekseer::RefPtr<EffekseerRenderer::SingleFrameMemoryPool> memory_pool_;
    Effekseer::RefPtr<EffekseerRenderer::CommandList> efk_command_list_;
    std::unordered_map<std::string, Effekseer::EffectRef> effect_cache_;
    std::unordered_map<std::string, ActiveEffect> active_effects_;
#endif
};