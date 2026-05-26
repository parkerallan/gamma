#pragma once

#include "imgui.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <string>

#ifdef GAMMA_WITH_EFFEKSEER
#include <Effekseer.h>
#include <EffekseerRendererVulkan.h>
#endif

class VulkanContext;

class EffectsPreviewRenderer
{
public:
    EffectsPreviewRenderer() = default;
    ~EffectsPreviewRenderer();

    EffectsPreviewRenderer(const EffectsPreviewRenderer&) = delete;
    EffectsPreviewRenderer& operator=(const EffectsPreviewRenderer&) = delete;

    ImTextureID Render(
        VulkanContext* vulkan_context,
        const std::filesystem::path& effect_path,
        std::uint32_t width,
        std::uint32_t height,
        float timeline_seconds,
        bool playing,
        bool paused,
        bool loop);

    void Stop();
    void Restart();
    void Clear();
    void HandleMouseControls(const ImVec2& region_min, const ImVec2& region_max);
    void DrawGridOverlay(ImDrawList* draw_list, const ImVec2& region_min, const ImVec2& region_max) const;
    void ResetCamera();

    bool IsAvailable() const;
    const std::string& LastError() const { return last_error_; }

private:
    bool EnsureInitialized(VulkanContext* vulkan_context);
    bool EnsureRenderTarget(VulkanContext* vulkan_context, std::uint32_t width, std::uint32_t height);
    bool LoadEffect(const std::filesystem::path& effect_path);
    void ValidateLoadedEffectResources(const std::filesystem::path& effect_path);
    bool RenderFrame(VulkanContext* vulkan_context, std::uint32_t width, std::uint32_t height);
    void DestroyRenderTarget();
    void DestroyEffekseer();
    void ResetPlayback();
    void AdvanceSimulation(float target_seconds, bool playing, bool paused, bool loop);
    void SetError(const std::string& message);

    VulkanContext* context_ = nullptr;
    std::filesystem::path loaded_effect_path_;
    std::string last_error_;
    bool restart_requested_ = false;
    bool stopped_ = true;
    float simulated_seconds_ = 0.0f;
    float camera_yaw_ = 0.0f;
    float camera_pitch_ = 0.25f;
    float camera_distance_ = 8.0f;
    float camera_focus_x_ = 0.0f;
    float camera_focus_y_ = 0.8f;
    float camera_focus_z_ = 0.0f;
    bool middle_mouse_panning_ = false;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkFramebuffer framebuffer_ = VK_NULL_HANDLE;
    VkImage color_image_ = VK_NULL_HANDLE;
    VkDeviceMemory color_memory_ = VK_NULL_HANDLE;
    VkImageView color_view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    std::uint32_t target_width_ = 0;
    std::uint32_t target_height_ = 0;

#ifdef GAMMA_WITH_EFFEKSEER
    Effekseer::ManagerRef manager_;
    Effekseer::EffectRef effect_;
    Effekseer::Handle handle_ = -1;
    Effekseer::Backend::GraphicsDeviceRef graphics_device_;
    EffekseerRenderer::RendererRef renderer_;
    Effekseer::RefPtr<EffekseerRenderer::SingleFrameMemoryPool> memory_pool_;
    Effekseer::RefPtr<EffekseerRenderer::CommandList> efk_command_list_;
#endif
};