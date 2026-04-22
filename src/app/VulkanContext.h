#pragma once

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

struct VulkanRayTracingDispatch
{
    PFN_vkGetBufferDeviceAddressKHR get_buffer_device_address = nullptr;
    PFN_vkCreateAccelerationStructureKHR create_acceleration_structure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroy_acceleration_structure = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR get_acceleration_structure_build_sizes = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR get_acceleration_structure_device_address = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmd_build_acceleration_structures = nullptr;
    PFN_vkCreateRayTracingPipelinesKHR create_ray_tracing_pipelines = nullptr;
    PFN_vkGetRayTracingShaderGroupHandlesKHR get_ray_tracing_shader_group_handles = nullptr;
    PFN_vkCmdTraceRaysKHR cmd_trace_rays = nullptr;
};

struct VulkanRayTracingSupport
{
    bool supported = false;
    bool enabled = false;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_properties = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_pipeline_properties = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
};

struct VulkanWindowContext
{
    ImGui_ImplVulkanH_Window window_data{};
    bool swapchain_rebuild = false;
};

class VulkanContext
{
public:
    bool Initialize(SDL_Window* window);
    void Shutdown();
    void WaitIdle();
    void RenderFrame(SDL_Window* window, ImDrawData* draw_data, const ImVec4& clear_color);
    bool CreateWindowContext(SDL_Window* window, VulkanWindowContext& window_context);
    void DestroyWindowContext(VulkanWindowContext& window_context);
    bool PresentImageToWindow(
        SDL_Window* window,
        VulkanWindowContext& window_context,
        VkImage source_image,
        VkImageLayout source_layout,
        std::uint32_t source_width,
        std::uint32_t source_height);

    // Present a rendered image directly to the main (engine-initialized) window
    // without a secondary VulkanWindowContext. Used by the standalone game.
    bool PresentImageToMainWindow(
        SDL_Window* window,
        VkImage source_image,
        VkImageLayout source_layout,
        std::uint32_t source_width,
        std::uint32_t source_height);

    VkInstance GetInstance() const { return instance_; }
    VkPhysicalDevice GetPhysicalDevice() const { return physical_device_; }
    VkDevice GetDevice() const { return device_; }
    std::uint32_t GetQueueFamily() const { return queue_family_; }
    VkQueue GetQueue() const { return queue_; }
    VkPipelineCache GetPipelineCache() const { return pipeline_cache_; }
    VkDescriptorPool GetDescriptorPool() const { return descriptor_pool_; }
    const VkAllocationCallbacks* GetAllocator() const { return allocator_; }
    std::uint32_t GetMinImageCount() const { return min_image_count_; }
    std::uint32_t GetImageCount() const { return main_window_data_.ImageCount; }
    VkRenderPass GetRenderPass() const { return main_window_data_.RenderPass; }
    const VulkanRayTracingSupport& GetRayTracingSupport() const { return ray_tracing_support_; }
    const VulkanRayTracingDispatch& GetRayTracingDispatch() const { return ray_tracing_dispatch_; }

    static void CheckVkResult(VkResult err);

private:
    bool CreateInstance();
    bool PickPhysicalDevice();
    bool CreateDevice();
    bool CreateDescriptorPool();
    bool CreateSurface(SDL_Window* window);
    bool CreateSurface(SDL_Window* window, ImGui_ImplVulkanH_Window& window_data);
    void SetupWindowData(SDL_Window* window);
    void SetupWindowData(SDL_Window* window, ImGui_ImplVulkanH_Window& window_data);
    void EnsureSwapchain(SDL_Window* window);
    void EnsureSwapchain(SDL_Window* window, VulkanWindowContext& window_context);
    void CleanupWindowData();
    void CleanupWindowData(ImGui_ImplVulkanH_Window& window_data);

    const VkAllocationCallbacks* allocator_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = UINT32_MAX;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VulkanRayTracingSupport ray_tracing_support_{};
    VulkanRayTracingDispatch ray_tracing_dispatch_{};
    ImGui_ImplVulkanH_Window main_window_data_{};
    std::uint32_t min_image_count_ = 2;
    bool swapchain_rebuild_ = false;
};