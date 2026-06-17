#pragma once

#include "app/VulkanContext.h"

#include <array>
#include <cstdint>
#include <unordered_map>

#include <vulkan/vulkan.h>

// Renders GPU-simulated rain as camera-facing streak billboards composited over
// the ray-traced scene image. Mirrors Scene2DRenderer's compositing model: it
// owns its own command pool / render pass / pipelines and a persistent 1-frame-
// in-flight command buffer, and blends into the existing RT color image (which
// it expects in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL on entry and restores
// to the same layout on exit).
//
// The simulation runs in a compute shader over a device-local particle SSBO;
// particles are stored emitter-relative to a proxy plane and fall straight down
// in world space. The fragment shader occludes drops against the ray-traced
// scene-depth image so they clip at the ground and behind geometry.
class RainParticleRenderer
{
public:
    struct Params
    {
        // Column-major, matching the rest of the engine.
        std::array<float, 16> view_projection{};
        std::array<float, 16> plane_to_world{};
        std::array<float, 3> camera_position{};
        std::array<float, 3> color{0.6f, 0.7f, 0.9f};
        float opacity = 0.35f;
        float fall_speed = 1.0f;  // user multiplier
        float drop_size = 1.0f;   // user multiplier
    };

    bool Initialize(VulkanContext* context);
    void Shutdown();

    // Dispatch the simulation and composite the streaks over target_image.
    // depth_image / depth_view are RayTracing's current-frame linear-depth image
    // (rg32f, GENERAL layout) used for occlusion.
    void Render(
        const Params& params,
        VkAccelerationStructureKHR scene_tlas,
        VkImage target_image,
        VkImageView target_view,
        std::uint32_t width,
        std::uint32_t height,
        VkImage depth_image,
        VkImageView depth_view,
        std::uint32_t depth_width,
        std::uint32_t depth_height);

private:
    bool EnsureResources();
    bool CreateComputePipeline();
    bool CreateGraphicsPipeline();
    void UpdateComputeDescriptor();
    void UpdateComputeTlas(VkAccelerationStructureKHR tlas);
    void UpdateGraphicsDescriptor(VkImageView depth_view);
    VkFramebuffer GetOrCreateFramebuffer(VkImageView view, std::uint32_t w, std::uint32_t h);
    void ClearFramebufferCache();

    VulkanContext* vulkan_context_ = nullptr;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout compute_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout compute_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline compute_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSet compute_set_ = VK_NULL_HANDLE;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout gfx_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout gfx_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline gfx_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSet gfx_set_ = VK_NULL_HANDLE;

    VkBuffer particle_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory particle_memory_ = VK_NULL_HANDLE;

    VkBuffer compute_ubo_ = VK_NULL_HANDLE;
    VkDeviceMemory compute_ubo_memory_ = VK_NULL_HANDLE;
    void* compute_ubo_mapped_ = nullptr;

    VkBuffer view_ubo_ = VK_NULL_HANDLE;
    VkDeviceMemory view_ubo_memory_ = VK_NULL_HANDLE;
    void* view_ubo_mapped_ = nullptr;

    std::unordered_map<VkImageView, VkFramebuffer> framebuffer_cache_;

    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool in_flight_ = false;

    VkImageView bound_depth_view_ = VK_NULL_HANDLE;
    VkAccelerationStructureKHR bound_tlas_ = VK_NULL_HANDLE;

    std::uint32_t particle_count_ = 16384;
    bool reset_pending_ = true;
    bool resources_ready_ = false;
    std::uint64_t last_ticks_ = 0;
    float time_seconds_ = 0.0f;
};
