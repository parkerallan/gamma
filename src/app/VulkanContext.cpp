#include "app/VulkanContext.h"

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace
{
constexpr const char* kRequiredRayTracingDeviceExtensions[] = {
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
};

bool HasExtension(const std::vector<VkExtensionProperties>& properties, const char* extension_name)
{
    return std::any_of(properties.begin(), properties.end(), [&](const VkExtensionProperties& property)
    {
        return std::strcmp(property.extensionName, extension_name) == 0;
    });
}

bool HasRequiredRayTracingExtensions(const std::vector<VkExtensionProperties>& properties)
{
    for (const char* extension_name : kRequiredRayTracingDeviceExtensions)
    {
        if (!HasExtension(properties, extension_name))
        {
            return false;
        }
    }

    return true;
}

void TransitionImageLayout(
    VkCommandBuffer command_buffer,
    VkImage image,
    VkImageAspectFlags aspect_mask,
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
    barrier.subresourceRange.aspectMask = aspect_mask;
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

    ray_tracing_support_ = {};
    ray_tracing_dispatch_ = {};

    VkPhysicalDeviceDescriptorIndexingFeatures descriptor_indexing_features = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceBufferDeviceAddressFeatures buffer_device_address_features = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration_structure_features = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR ray_tracing_pipeline_features = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};

    descriptor_indexing_features.pNext = &buffer_device_address_features;
    buffer_device_address_features.pNext = &acceleration_structure_features;
    acceleration_structure_features.pNext = &ray_tracing_pipeline_features;

    VkPhysicalDeviceFeatures2 available_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    available_features.pNext = &descriptor_indexing_features;

    const bool has_ray_tracing_extensions = HasRequiredRayTracingExtensions(available_extensions);
    if (has_ray_tracing_extensions)
    {
        vkGetPhysicalDeviceFeatures2(physical_device_, &available_features);

        if (descriptor_indexing_features.runtimeDescriptorArray == VK_TRUE &&
            descriptor_indexing_features.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
            buffer_device_address_features.bufferDeviceAddress == VK_TRUE &&
            acceleration_structure_features.accelerationStructure == VK_TRUE &&
            ray_tracing_pipeline_features.rayTracingPipeline == VK_TRUE)
        {
            for (const char* extension_name : kRequiredRayTracingDeviceExtensions)
            {
                device_extensions.push_back(extension_name);
            }

            ray_tracing_support_.supported = true;

            VkPhysicalDeviceAccelerationStructurePropertiesKHR acceleration_structure_properties = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_pipeline_properties = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
            acceleration_structure_properties.pNext = &ray_tracing_pipeline_properties;

            VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext = &acceleration_structure_properties;
            vkGetPhysicalDeviceProperties2(physical_device_, &properties);
            ray_tracing_support_.acceleration_structure_properties = acceleration_structure_properties;
            ray_tracing_support_.ray_tracing_pipeline_properties = ray_tracing_pipeline_properties;

            descriptor_indexing_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
            descriptor_indexing_features.runtimeDescriptorArray = VK_TRUE;
            descriptor_indexing_features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            buffer_device_address_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
            buffer_device_address_features.bufferDeviceAddress = VK_TRUE;
            acceleration_structure_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
            acceleration_structure_features.accelerationStructure = VK_TRUE;
            ray_tracing_pipeline_features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
            ray_tracing_pipeline_features.rayTracingPipeline = VK_TRUE;

            descriptor_indexing_features.pNext = &buffer_device_address_features;
            buffer_device_address_features.pNext = &acceleration_structure_features;
            acceleration_structure_features.pNext = &ray_tracing_pipeline_features;
        }
    }

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
    if (ray_tracing_support_.supported)
    {
        create_info.pNext = &descriptor_indexing_features;
    }

    result = vkCreateDevice(physical_device_, &create_info, allocator_, &device_);
    CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    if (ray_tracing_support_.supported)
    {
        ray_tracing_support_.enabled = true;
        ray_tracing_dispatch_.get_buffer_device_address =
            reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(vkGetDeviceProcAddr(device_, "vkGetBufferDeviceAddressKHR"));
        ray_tracing_dispatch_.create_acceleration_structure =
            reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(vkGetDeviceProcAddr(device_, "vkCreateAccelerationStructureKHR"));
        ray_tracing_dispatch_.destroy_acceleration_structure =
            reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(vkGetDeviceProcAddr(device_, "vkDestroyAccelerationStructureKHR"));
        ray_tracing_dispatch_.get_acceleration_structure_build_sizes =
            reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureBuildSizesKHR"));
        ray_tracing_dispatch_.get_acceleration_structure_device_address =
            reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(vkGetDeviceProcAddr(device_, "vkGetAccelerationStructureDeviceAddressKHR"));
        ray_tracing_dispatch_.cmd_build_acceleration_structures =
            reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(vkGetDeviceProcAddr(device_, "vkCmdBuildAccelerationStructuresKHR"));
        ray_tracing_dispatch_.create_ray_tracing_pipelines =
            reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(vkGetDeviceProcAddr(device_, "vkCreateRayTracingPipelinesKHR"));
        ray_tracing_dispatch_.get_ray_tracing_shader_group_handles =
            reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(vkGetDeviceProcAddr(device_, "vkGetRayTracingShaderGroupHandlesKHR"));
        ray_tracing_dispatch_.cmd_trace_rays =
            reinterpret_cast<PFN_vkCmdTraceRaysKHR>(vkGetDeviceProcAddr(device_, "vkCmdTraceRaysKHR"));
    }
    return queue_ != VK_NULL_HANDLE;
}

bool VulkanContext::CreateDescriptorPool()
{
    constexpr std::uint32_t descriptor_capacity = 4096;
    constexpr VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, descriptor_capacity},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, descriptor_capacity},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptor_capacity},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, descriptor_capacity},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, descriptor_capacity},
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
    return CreateSurface(window, main_window_data_);
}

bool VulkanContext::CreateSurface(SDL_Window* window, ImGui_ImplVulkanH_Window& window_data)
{
    if (!SDL_Vulkan_CreateSurface(window, instance_, allocator_, &window_data.Surface))
    {
        SDL_Log("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }

    return true;
}

void VulkanContext::SetupWindowData(SDL_Window* window)
{
    SetupWindowData(window, main_window_data_);
}

void VulkanContext::SetupWindowData(SDL_Window* window, ImGui_ImplVulkanH_Window& window_data)
{
    VkBool32 present_supported = VK_FALSE;
    VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, queue_family_, window_data.Surface, &present_supported);
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

    window_data.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        physical_device_,
        window_data.Surface,
        request_formats.data(),
        static_cast<int>(request_formats.size()),
        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    constexpr VkPresentModeKHR request_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
    window_data.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        physical_device_,
        window_data.Surface,
        request_modes,
        static_cast<int>(std::size(request_modes)));
    window_data.ClearValue.color.float32[0] = 0.08f;
    window_data.ClearValue.color.float32[1] = 0.09f;
    window_data.ClearValue.color.float32[2] = 0.11f;
    window_data.ClearValue.color.float32[3] = 1.0f;

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        instance_,
        physical_device_,
        device_,
        &window_data,
        queue_family_,
        allocator_,
        width,
        height,
        min_image_count_,
        0);
}

void VulkanContext::EnsureSwapchain(SDL_Window* window)
{
    VulkanWindowContext main_window_context{};
    main_window_context.window_data = main_window_data_;
    main_window_context.swapchain_rebuild = swapchain_rebuild_;

    EnsureSwapchain(window, main_window_context);

    main_window_data_ = main_window_context.window_data;
    swapchain_rebuild_ = main_window_context.swapchain_rebuild;
}

void VulkanContext::EnsureSwapchain(SDL_Window* window, VulkanWindowContext& window_context)
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(window, &width, &height);
    if (width <= 0 || height <= 0)
    {
        return;
    }

    if (window_context.swapchain_rebuild || window_context.window_data.Width != width || window_context.window_data.Height != height)
    {
        ImGui_ImplVulkan_SetMinImageCount(min_image_count_);
        ImGui_ImplVulkanH_CreateOrResizeWindow(
            instance_,
            physical_device_,
            device_,
            &window_context.window_data,
            queue_family_,
            allocator_,
            width,
            height,
            min_image_count_,
            0);
        window_context.window_data.FrameIndex = 0;
        window_context.swapchain_rebuild = false;
    }
}

void VulkanContext::CleanupWindowData()
{
    CleanupWindowData(main_window_data_);
}

void VulkanContext::CleanupWindowData(ImGui_ImplVulkanH_Window& window_data)
{
    if (window_data.RenderPass != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkanH_DestroyWindow(instance_, device_, &window_data, allocator_);
    }
    if (window_data.Surface != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE)
    {
        SDL_Vulkan_DestroySurface(instance_, window_data.Surface, allocator_);
        window_data.Surface = VK_NULL_HANDLE;
    }
}

bool VulkanContext::CreateWindowContext(SDL_Window* window, VulkanWindowContext& window_context)
{
    window_context = {};
    if (window == nullptr || instance_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE)
    {
        return false;
    }

    if (!CreateSurface(window, window_context.window_data))
    {
        return false;
    }

    SetupWindowData(window, window_context.window_data);
    if (window_context.window_data.Surface == VK_NULL_HANDLE || window_context.window_data.RenderPass == VK_NULL_HANDLE)
    {
        DestroyWindowContext(window_context);
        return false;
    }

    return true;
}

void VulkanContext::DestroyWindowContext(VulkanWindowContext& window_context)
{
    CleanupWindowData(window_context.window_data);
    window_context = {};
}

bool VulkanContext::PresentImageToWindow(
    SDL_Window* window,
    VulkanWindowContext& window_context,
    VkImage source_image,
    VkImageLayout source_layout,
    std::uint32_t source_width,
    std::uint32_t source_height)
{
    if (window == nullptr ||
        source_image == VK_NULL_HANDLE ||
        source_width == 0 ||
        source_height == 0 ||
        device_ == VK_NULL_HANDLE)
    {
        return false;
    }

    EnsureSwapchain(window, window_context);

    ImGui_ImplVulkanH_Window& window_data = window_context.window_data;
    const VkSemaphore image_acquired_semaphore = window_data.FrameSemaphores[window_data.SemaphoreIndex].ImageAcquiredSemaphore;
    const VkSemaphore render_complete_semaphore = window_data.FrameSemaphores[window_data.SemaphoreIndex].RenderCompleteSemaphore;
    VkResult result = vkAcquireNextImageKHR(device_, window_data.Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &window_data.FrameIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        window_context.swapchain_rebuild = true;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return false;
    }
    CheckVkResult(result);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        return false;
    }

    ImGui_ImplVulkanH_Frame* frame = &window_data.Frames[window_data.FrameIndex];
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
    if (result != VK_SUCCESS)
    {
        return false;
    }

    TransitionImageLayout(
        frame->CommandBuffer,
        source_image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        source_layout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        source_layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        source_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);

    TransitionImageLayout(
        frame->CommandBuffer,
        window_data.Frames[window_data.FrameIndex].Backbuffer,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        VK_ACCESS_TRANSFER_WRITE_BIT);

    VkImageBlit blit_region = {};
    blit_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit_region.srcSubresource.layerCount = 1;
    blit_region.srcOffsets[1].x = static_cast<int32_t>(source_width);
    blit_region.srcOffsets[1].y = static_cast<int32_t>(source_height);
    blit_region.srcOffsets[1].z = 1;
    blit_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit_region.dstSubresource.layerCount = 1;
    blit_region.dstOffsets[0].y = static_cast<int32_t>(window_data.Height);
    blit_region.dstOffsets[1].x = static_cast<int32_t>(window_data.Width);
    blit_region.dstOffsets[1].z = 1;
    vkCmdBlitImage(
        frame->CommandBuffer,
        source_image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        window_data.Frames[window_data.FrameIndex].Backbuffer,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &blit_region,
        VK_FILTER_LINEAR);

    TransitionImageLayout(
        frame->CommandBuffer,
        source_image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        source_layout,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        source_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT);

    TransitionImageLayout(
        frame->CommandBuffer,
        window_data.Frames[window_data.FrameIndex].Backbuffer,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        0);

    result = vkEndCommandBuffer(frame->CommandBuffer);
    CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
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
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_complete_semaphore;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &window_data.Swapchain;
    present_info.pImageIndices = &window_data.FrameIndex;
    result = vkQueuePresentKHR(queue_, &present_info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
    {
        window_context.swapchain_rebuild = true;
    }
    CheckVkResult(result);
    window_data.SemaphoreIndex = (window_data.SemaphoreIndex + 1) % window_data.SemaphoreCount;
    return result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
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