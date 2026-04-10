#include "app/VulkanContext.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace
{
bool HasExtension(const std::vector<VkExtensionProperties>& properties, const char* extension_name)
{
    return std::any_of(properties.begin(), properties.end(), [&](const VkExtensionProperties& property)
    {
        return std::strcmp(property.extensionName, extension_name) == 0;
    });
}
}

void VulkanContext::CheckVkResult(VkResult err)
{
    if (err == VK_SUCCESS)
    {
        return;
    }

    SDL_Log("Vulkan error: VkResult = %d", static_cast<int>(err));
}

bool VulkanContext::Initialize(SDL_Window* window)
{
    if (window == nullptr)
    {
        SDL_Log("VulkanContext::Initialize called with a null window");
        return false;
    }

    if (!CreateInstance() || !PickPhysicalDevice() || !CreateDevice() || !CreateDescriptorPool() || !CreateSurface(window))
    {
        Shutdown();
        return false;
    }

    SetupWindowData(window);
    return main_window_data_.Surface != VK_NULL_HANDLE && main_window_data_.RenderPass != VK_NULL_HANDLE;
}

void VulkanContext::Shutdown()
{
    WaitIdle();
    CleanupWindowData();

    if (descriptor_pool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device_, descriptor_pool_, allocator_);
        descriptor_pool_ = VK_NULL_HANDLE;
    }

    if (pipeline_cache_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE)
    {
        vkDestroyPipelineCache(device_, pipeline_cache_, allocator_);
        pipeline_cache_ = VK_NULL_HANDLE;
    }

    if (device_ != VK_NULL_HANDLE)
    {
        vkDestroyDevice(device_, allocator_);
        device_ = VK_NULL_HANDLE;
    }

    if (instance_ != VK_NULL_HANDLE)
    {
        vkDestroyInstance(instance_, allocator_);
        instance_ = VK_NULL_HANDLE;
    }

    physical_device_ = VK_NULL_HANDLE;
    queue_family_ = UINT32_MAX;
    queue_ = VK_NULL_HANDLE;
    swapchain_rebuild_ = false;
}

void VulkanContext::WaitIdle()
{
    if (device_ != VK_NULL_HANDLE)
    {
        vkDeviceWaitIdle(device_);
    }
}

bool VulkanContext::CreateInstance()
{
    Uint32 extension_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (sdl_extensions == nullptr)
    {
        SDL_Log("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return false;
    }

    std::vector<const char*> instance_extensions(sdl_extensions, sdl_extensions + extension_count);

    std::uint32_t available_extension_count = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &available_extension_count, nullptr);
    CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    std::vector<VkExtensionProperties> available_extensions(available_extension_count);
    result = vkEnumerateInstanceExtensionProperties(nullptr, &available_extension_count, available_extensions.data());
    CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    if (HasExtension(available_extensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
    {
        instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }

    VkApplicationInfo application_info = {};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Engine";
    application_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    application_info.pEngineName = "EngineSkeleton";
    application_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    application_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &application_info;
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(instance_extensions.size());
    create_info.ppEnabledExtensionNames = instance_extensions.data();

    result = vkCreateInstance(&create_info, allocator_, &instance_);
    CheckVkResult(result);
    return result == VK_SUCCESS;
}

bool VulkanContext::PickPhysicalDevice()
{
    physical_device_ = ImGui_ImplVulkanH_SelectPhysicalDevice(instance_);
    queue_family_ = ImGui_ImplVulkanH_SelectQueueFamilyIndex(physical_device_);
    return physical_device_ != VK_NULL_HANDLE && queue_family_ != UINT32_MAX;
}

bool VulkanContext::CreateDevice()
{
    std::uint32_t extension_count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &extension_count, nullptr);
    CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    std::vector<VkExtensionProperties> available_extensions(extension_count);
    result = vkEnumerateDeviceExtensionProperties(physical_device_, nullptr, &extension_count, available_extensions.data());
    CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    std::vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
    if (HasExtension(available_extensions, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
    {
        device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }
#endif

    const float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;

    VkDeviceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = 1;
    create_info.pQueueCreateInfos = &queue_info;
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    create_info.ppEnabledExtensionNames = device_extensions.data();

    result = vkCreateDevice(physical_device_, &create_info, allocator_, &device_);
    CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return queue_ != VK_NULL_HANDLE;
}

bool VulkanContext::CreateDescriptorPool()
{
    constexpr std::uint32_t descriptor_capacity = 4096;
    constexpr VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, descriptor_capacity},
    };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.poolSizeCount = static_cast<std::uint32_t>(std::size(pool_sizes));
    pool_info.pPoolSizes = pool_sizes;
    for (const VkDescriptorPoolSize& pool_size : pool_sizes)
    {
        pool_info.maxSets += pool_size.descriptorCount;
    }

    const VkResult result = vkCreateDescriptorPool(device_, &pool_info, allocator_, &descriptor_pool_);
    CheckVkResult(result);
    return result == VK_SUCCESS;
}

bool VulkanContext::CreateSurface(SDL_Window* window)
{
    if (!SDL_Vulkan_CreateSurface(window, instance_, allocator_, &main_window_data_.Surface))
    {
        SDL_Log("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }

    return true;
}

void VulkanContext::SetupWindowData(SDL_Window* window)
{
    VkBool32 present_supported = VK_FALSE;
    VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_family_, main_window_data_.Surface, &present_supported);
    CheckVkResult(result);
    if (result != VK_SUCCESS || present_supported != VK_TRUE)
    {
        SDL_Log("Selected Vulkan queue family does not support presentation");
        return;
    }

    constexpr std::array<VkFormat, 4> request_formats = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,
        VK_FORMAT_R8G8B8_UNORM,
    };

    main_window_data_.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        physical_device_,
        main_window_data_.Surface,
        request_formats.data(),
        static_cast<int>(request_formats.size()),
        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    constexpr VkPresentModeKHR request_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
    main_window_data_.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        physical_device_,
        main_window_data_.Surface,
        request_modes,
        static_cast<int>(std::size(request_modes)));
    main_window_data_.ClearValue.color.float32[0] = 0.08f;
    main_window_data_.ClearValue.color.float32[1] = 0.09f;
    main_window_data_.ClearValue.color.float32[2] = 0.11f;
    main_window_data_.ClearValue.color.float32[3] = 1.0f;

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        instance_,
        physical_device_,
        device_,
        &main_window_data_,
        queue_family_,
        allocator_,
        width,
        height,
        min_image_count_,
        0);
}

void VulkanContext::EnsureSwapchain(SDL_Window* window)
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    if (width <= 0 || height <= 0)
    {
        return;
    }

    if (swapchain_rebuild_ || main_window_data_.Width != width || main_window_data_.Height != height)
    {
        ImGui_ImplVulkan_SetMinImageCount(min_image_count_);
        ImGui_ImplVulkanH_CreateOrResizeWindow(
            instance_,
            physical_device_,
            device_,
            &main_window_data_,
            queue_family_,
            allocator_,
            width,
            height,
            min_image_count_,
            0);
        main_window_data_.FrameIndex = 0;
        swapchain_rebuild_ = false;
    }
}

void VulkanContext::CleanupWindowData()
{
    if (main_window_data_.RenderPass != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkanH_DestroyWindow(instance_, device_, &main_window_data_, allocator_);
    }
    if (main_window_data_.Surface != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE)
    {
        SDL_Vulkan_DestroySurface(instance_, main_window_data_.Surface, allocator_);
        main_window_data_.Surface = VK_NULL_HANDLE;
    }
}

void VulkanContext::RenderFrame(SDL_Window* window, ImDrawData* draw_data, const ImVec4& clear_color)
{
    if (window == nullptr || draw_data == nullptr || device_ == VK_NULL_HANDLE)
    {
        return;
    }

    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
    {
        return;
    }

    EnsureSwapchain(window);
    main_window_data_.ClearValue.color.float32[0] = clear_color.x;
    main_window_data_.ClearValue.color.float32[1] = clear_color.y;
    main_window_data_.ClearValue.color.float32[2] = clear_color.z;
    main_window_data_.ClearValue.color.float32[3] = clear_color.w;

    const VkSemaphore image_acquired_semaphore = main_window_data_.FrameSemaphores[main_window_data_.SemaphoreIndex].ImageAcquiredSemaphore;
    const VkSemaphore render_complete_semaphore = main_window_data_.FrameSemaphores[main_window_data_.SemaphoreIndex].RenderCompleteSemaphore;
    VkResult result = vkAcquireNextImageKHR(device_, main_window_data_.Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &main_window_data_.FrameIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        swapchain_rebuild_ = true;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return;
    }
    CheckVkResult(result);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        return;
    }

    ImGui_ImplVulkanH_Frame* frame = &main_window_data_.Frames[main_window_data_.FrameIndex];
    result = vkWaitForFences(device_, 1, &frame->Fence, VK_TRUE, UINT64_MAX);
    CheckVkResult(result);
    result = vkResetFences(device_, 1, &frame->Fence);
    CheckVkResult(result);
    result = vkResetCommandPool(device_, frame->CommandPool, 0);
    CheckVkResult(result);

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(frame->CommandBuffer, &begin_info);
    CheckVkResult(result);

    VkRenderPassBeginInfo render_pass_info = {};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = main_window_data_.RenderPass;
    render_pass_info.framebuffer = frame->Framebuffer;
    render_pass_info.renderArea.extent.width = static_cast<std::uint32_t>(main_window_data_.Width);
    render_pass_info.renderArea.extent.height = static_cast<std::uint32_t>(main_window_data_.Height);
    render_pass_info.clearValueCount = 1;
    render_pass_info.pClearValues = &main_window_data_.ClearValue;
    vkCmdBeginRenderPass(frame->CommandBuffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(draw_data, frame->CommandBuffer);

    vkCmdEndRenderPass(frame->CommandBuffer);
    result = vkEndCommandBuffer(frame->CommandBuffer);
    CheckVkResult(result);

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_acquired_semaphore;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &frame->CommandBuffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_complete_semaphore;
    result = vkQueueSubmit(queue_, 1, &submit_info, frame->Fence);
    CheckVkResult(result);

    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_complete_semaphore;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &main_window_data_.Swapchain;
    present_info.pImageIndices = &main_window_data_.FrameIndex;
    result = vkQueuePresentKHR(queue_, &present_info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        swapchain_rebuild_ = true;
    }
    CheckVkResult(result);
    main_window_data_.SemaphoreIndex = (main_window_data_.SemaphoreIndex + 1) % main_window_data_.SemaphoreCount;
}