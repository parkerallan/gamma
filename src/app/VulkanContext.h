#pragma once

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

class VulkanContext
{
public:
    bool Initialize(SDL_Window* window);
    void Shutdown();
    void WaitIdle();
    void RenderFrame(SDL_Window* window, ImDrawData* draw_data, const ImVec4& clear_color);

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

    static void CheckVkResult(VkResult err);

private:
    bool CreateInstance();
    bool PickPhysicalDevice();
    bool CreateDevice();
    bool CreateDescriptorPool();
    bool CreateSurface(SDL_Window* window);
    void SetupWindowData(SDL_Window* window);
    void EnsureSwapchain(SDL_Window* window);
    void CleanupWindowData();

    const VkAllocationCallbacks* allocator_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = UINT32_MAX;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    ImGui_ImplVulkanH_Window main_window_data_{};
    std::uint32_t min_image_count_ = 2;
    bool swapchain_rebuild_ = false;
};