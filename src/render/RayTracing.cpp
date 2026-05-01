#include "render/Raytracing.h"

#include <SDL3/SDL.h>

#include "backends/imgui_impl_vulkan.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>

namespace
{
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint32_t FindMemoryType(VkPhysicalDevice physical_device, std::uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index)
    {
        const bool matches_filter = (type_filter & (1u << index)) != 0;
        const bool matches_properties = (memory_properties.memoryTypes[index].propertyFlags & properties) == properties;
        if (matches_filter && matches_properties)
        {
            return index;
        }
    }

    return UINT32_MAX;
}

bool HasRequiredDispatch(const VulkanRayTracingDispatch& dispatch)
{
    return dispatch.get_buffer_device_address != nullptr &&
        dispatch.create_acceleration_structure != nullptr &&
        dispatch.destroy_acceleration_structure != nullptr &&
        dispatch.get_acceleration_structure_build_sizes != nullptr &&
        dispatch.get_acceleration_structure_device_address != nullptr &&
        dispatch.cmd_build_acceleration_structures != nullptr &&
        dispatch.create_ray_tracing_pipelines != nullptr &&
        dispatch.get_ray_tracing_shader_group_handles != nullptr &&
        dispatch.cmd_trace_rays != nullptr;
}

bool CreateGpuBuffer(
    const VulkanContext& context,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    RayTracing::GpuBuffer& buffer);

void DestroyGpuBuffer(const VulkanContext* context, RayTracing::GpuBuffer& buffer);

bool UploadGpuBuffer(
    const VulkanContext& context,
    const RayTracing::GpuBuffer& buffer,
    const void* data,
    std::size_t size);

void HashBytes(std::uint64_t& hash, const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= static_cast<std::uint64_t>(bytes[index]);
        hash *= kFnvPrime;
    }
}

template <typename T>
void HashVector(std::uint64_t& hash, const std::vector<T>& values)
{
    const std::uint64_t size = static_cast<std::uint64_t>(values.size());
    HashBytes(hash, &size, sizeof(size));
    if (!values.empty())
    {
        HashBytes(hash, values.data(), values.size() * sizeof(T));
    }
}

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
    const std::vector<std::uint8_t> shader_bytes = ReadBinaryFile(path);
    if (shader_bytes.empty())
    {
        SDL_Log("Failed to read ray tracing shader file: %s", path.string().c_str());
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = shader_bytes.size();
    create_info.pCode = reinterpret_cast<const std::uint32_t*>(shader_bytes.data());

    VkShaderModule shader_module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(device, &create_info, nullptr, &shader_module);
    VulkanContext::CheckVkResult(result);
    return result == VK_SUCCESS ? shader_module : VK_NULL_HANDLE;
}

template <typename T>
T AlignUp(T value, T alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    return (value + alignment - 1) & ~(alignment - 1);
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

bool CreateVulkanImage(
    VkPhysicalDevice physical_device,
    VkDevice device,
    const VkAllocationCallbacks* allocator,
    std::uint32_t width,
    std::uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImageAspectFlags aspect_mask,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& image_view)
{
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = vkCreateImage(device, &image_info, allocator, &image);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements memory_requirements = {};
    vkGetImageMemoryRequirements(device, image, &memory_requirements);

    VkMemoryAllocateInfo allocation_info = {};
    allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation_info.allocationSize = memory_requirements.size;
    allocation_info.memoryTypeIndex = FindMemoryType(
        physical_device,
        memory_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation_info.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyImage(device, image, allocator);
        image = VK_NULL_HANDLE;
        return false;
    }

    result = vkAllocateMemory(device, &allocation_info, allocator, &memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyImage(device, image, allocator);
        image = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindImageMemory(device, image, memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, memory, allocator);
        vkDestroyImage(device, image, allocator);
        memory = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = aspect_mask;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(device, &view_info, allocator, &image_view);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, memory, allocator);
        vkDestroyImage(device, image, allocator);
        memory = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

bool BuildShaderBindingTable(
    const VulkanContext& context,
    VkPipeline pipeline,
    std::uint32_t first_group,
    std::uint32_t group_count,
    RayTracing::ShaderBindingTable& table)
{
    const VulkanRayTracingSupport& support = context.GetRayTracingSupport();
    const std::uint32_t handle_size = support.ray_tracing_pipeline_properties.shaderGroupHandleSize;
    const std::uint32_t handle_alignment = support.ray_tracing_pipeline_properties.shaderGroupHandleAlignment;
    const std::uint32_t base_alignment = support.ray_tracing_pipeline_properties.shaderGroupBaseAlignment;
    const std::uint32_t handle_stride = AlignUp(handle_size, handle_alignment);
    const std::uint32_t sbt_stride = AlignUp(handle_stride, base_alignment);
    const VkDeviceSize sbt_size = static_cast<VkDeviceSize>(sbt_stride) * group_count;

    std::vector<std::uint8_t> handles(static_cast<std::size_t>(handle_size) * group_count);
    VkResult result = context.GetRayTracingDispatch().get_ray_tracing_shader_group_handles(
        context.GetDevice(),
        pipeline,
        first_group,
        group_count,
        handles.size(),
        handles.data());
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    if (!CreateGpuBuffer(
            context,
            sbt_size,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            table.buffer))
    {
        return false;
    }

    std::vector<std::uint8_t> sbt_data(static_cast<std::size_t>(sbt_size), 0);
    for (std::uint32_t group_index = 0; group_index < group_count; ++group_index)
    {
        std::memcpy(
            sbt_data.data() + static_cast<std::size_t>(group_index * sbt_stride),
            handles.data() + static_cast<std::size_t>(group_index * handle_size),
            handle_size);
    }

    if (!UploadGpuBuffer(context, table.buffer, sbt_data.data(), sbt_data.size()))
    {
        DestroyGpuBuffer(&context, table.buffer);
        return false;
    }

    table.region.deviceAddress = table.buffer.device_address;
    table.region.stride = sbt_stride;
    table.region.size = sbt_size;
    return true;
}

VkTransformMatrixKHR ToVkTransformMatrix(const std::array<float, 16>& matrix)
{
    VkTransformMatrixKHR transform = {};
    transform.matrix[0][0] = matrix[0];
    transform.matrix[0][1] = matrix[4];
    transform.matrix[0][2] = matrix[8];
    transform.matrix[0][3] = matrix[12];
    transform.matrix[1][0] = matrix[1];
    transform.matrix[1][1] = matrix[5];
    transform.matrix[1][2] = matrix[9];
    transform.matrix[1][3] = matrix[13];
    transform.matrix[2][0] = matrix[2];
    transform.matrix[2][1] = matrix[6];
    transform.matrix[2][2] = matrix[10];
    transform.matrix[2][3] = matrix[14];
    return transform;
}

bool IsOpaqueMesh(const RayTracing::MeshInput& mesh)
{
    return std::none_of(mesh.sections.begin(), mesh.sections.end(), [](const RayTracing::MeshSectionRecord& section)
    {
        return section.uses_alpha_transparency;
    });
}

bool CreateGpuBuffer(
    const VulkanContext& context,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    RayTracing::GpuBuffer& buffer)
{
    const VkPhysicalDevice physical_device = context.GetPhysicalDevice();
    const VkDevice device = context.GetDevice();
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(device, &buffer_info, context.GetAllocator(), &buffer.buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = FindMemoryType(physical_device, requirements.memoryTypeBits, properties);

    VkMemoryAllocateFlagsInfo allocate_flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
    {
        allocate_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        allocate_info.pNext = &allocate_flags;
    }

    if (allocate_info.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyBuffer(device, buffer.buffer, context.GetAllocator());
        buffer.buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkAllocateMemory(device, &allocate_info, context.GetAllocator(), &buffer.memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyBuffer(device, buffer.buffer, context.GetAllocator());
        buffer.buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, buffer.memory, context.GetAllocator());
        vkDestroyBuffer(device, buffer.buffer, context.GetAllocator());
        buffer.memory = VK_NULL_HANDLE;
        buffer.buffer = VK_NULL_HANDLE;
        return false;
    }

    buffer.size = size;
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
    {
        const VulkanRayTracingDispatch& dispatch = context.GetRayTracingDispatch();
        if (dispatch.get_buffer_device_address != nullptr)
        {
            VkBufferDeviceAddressInfo address_info = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            address_info.buffer = buffer.buffer;
            buffer.device_address = dispatch.get_buffer_device_address(device, &address_info);
        }
    }

    return true;
}

void DestroyGpuBuffer(const VulkanContext* context, RayTracing::GpuBuffer& buffer)
{
    if (context != nullptr)
    {
        const VkDevice device = context->GetDevice();
        const VkAllocationCallbacks* allocator = context->GetAllocator();
        if (buffer.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(device, buffer.buffer, allocator);
        }
        if (buffer.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, buffer.memory, allocator);
        }
    }

    buffer = {};
}

bool UploadGpuBuffer(const VulkanContext& context, const RayTracing::GpuBuffer& buffer, const void* data, std::size_t size)
{
    if (buffer.buffer == VK_NULL_HANDLE || buffer.memory == VK_NULL_HANDLE || data == nullptr || size == 0)
    {
        return false;
    }

    void* mapped = nullptr;
    const VkResult result = vkMapMemory(context.GetDevice(), buffer.memory, 0, size, 0, &mapped);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS || mapped == nullptr)
    {
        return false;
    }

    std::memcpy(mapped, data, size);
    vkUnmapMemory(context.GetDevice(), buffer.memory);
    return true;
}

bool ExecuteImmediateCommands(
    const VulkanContext& context,
    VkCommandPool command_pool,
    const std::function<void(VkCommandBuffer)>& record_commands)
{
    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;

    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(context.GetDevice(), &allocate_info, &command_buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_buffer, &begin_info);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeCommandBuffers(context.GetDevice(), command_pool, 1, &command_buffer);
        return false;
    }

    record_commands(command_buffer);

    result = vkEndCommandBuffer(command_buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeCommandBuffers(context.GetDevice(), command_pool, 1, &command_buffer);
        return false;
    }

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    result = vkQueueSubmit(context.GetQueue(), 1, &submit_info, VK_NULL_HANDLE);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeCommandBuffers(context.GetDevice(), command_pool, 1, &command_buffer);
        return false;
    }

    result = vkQueueWaitIdle(context.GetQueue());
    VulkanContext::CheckVkResult(result);
    vkFreeCommandBuffers(context.GetDevice(), command_pool, 1, &command_buffer);
    return result == VK_SUCCESS;
}

void DestroyAccelerationStructure(const VulkanContext* context, RayTracing::AccelerationStructure& acceleration_structure)
{
    if (context != nullptr && acceleration_structure.handle != VK_NULL_HANDLE)
    {
        context->GetRayTracingDispatch().destroy_acceleration_structure(
            context->GetDevice(),
            acceleration_structure.handle,
            context->GetAllocator());
    }

    acceleration_structure.handle = VK_NULL_HANDLE;
    acceleration_structure.device_address = 0;
    DestroyGpuBuffer(context, acceleration_structure.buffer);
}

VkDeviceAddress GetAccelerationStructureDeviceAddress(const VulkanContext& context, VkAccelerationStructureKHR handle)
{
    if (handle == VK_NULL_HANDLE)
    {
        return 0;
    }

    VkAccelerationStructureDeviceAddressInfoKHR address_info = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    address_info.accelerationStructure = handle;
    return context.GetRayTracingDispatch().get_acceleration_structure_device_address(context.GetDevice(), &address_info);
}

bool CreateAccelerationStructure(
    const VulkanContext& context,
    VkAccelerationStructureTypeKHR type,
    VkDeviceSize size,
    RayTracing::AccelerationStructure& acceleration_structure)
{
    if (!CreateGpuBuffer(
            context,
            size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            acceleration_structure.buffer))
    {
        return false;
    }

    VkAccelerationStructureCreateInfoKHR create_info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    create_info.type = type;
    create_info.size = size;
    create_info.buffer = acceleration_structure.buffer.buffer;

    const VkResult result = context.GetRayTracingDispatch().create_acceleration_structure(
        context.GetDevice(),
        &create_info,
        context.GetAllocator(),
        &acceleration_structure.handle);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        DestroyAccelerationStructure(&context, acceleration_structure);
        return false;
    }

    acceleration_structure.device_address = GetAccelerationStructureDeviceAddress(context, acceleration_structure.handle);
    return acceleration_structure.device_address != 0;
}

bool BuildBottomLevelAccelerationStructure(
    const VulkanContext& context,
    VkCommandPool command_pool,
    const RayTracing::MeshInput& mesh,
    RayTracing::BottomLevelCacheEntry& cache_entry)
{
    DestroyAccelerationStructure(&context, cache_entry.acceleration_structure);

    VkAccelerationStructureGeometryTrianglesDataKHR triangles = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = mesh.vertex_device_address;
    triangles.vertexStride = mesh.vertex_stride;
    triangles.maxVertex = mesh.vertex_count > 0 ? mesh.vertex_count - 1 : 0;
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = mesh.index_device_address;

    VkAccelerationStructureGeometryKHR geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = IsOpaqueMesh(mesh) ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
    geometry.geometry.triangles = triangles;

    const std::uint32_t primitive_count = mesh.index_count / 3;
    VkAccelerationStructureBuildGeometryInfoKHR build_geometry_info = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build_geometry_info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build_geometry_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build_geometry_info.geometryCount = 1;
    build_geometry_info.pGeometries = &geometry;

    VkAccelerationStructureBuildSizesInfoKHR build_sizes = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    context.GetRayTracingDispatch().get_acceleration_structure_build_sizes(
        context.GetDevice(),
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &build_geometry_info,
        &primitive_count,
        &build_sizes);

    if (!CreateAccelerationStructure(context, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, build_sizes.accelerationStructureSize, cache_entry.acceleration_structure))
    {
        return false;
    }

    RayTracing::GpuBuffer scratch_buffer{};
    if (!CreateGpuBuffer(
            context,
            build_sizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            scratch_buffer))
    {
        DestroyAccelerationStructure(&context, cache_entry.acceleration_structure);
        return false;
    }

    build_geometry_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build_geometry_info.dstAccelerationStructure = cache_entry.acceleration_structure.handle;
    build_geometry_info.scratchData.deviceAddress = scratch_buffer.device_address;

    VkAccelerationStructureBuildRangeInfoKHR build_range = {};
    build_range.primitiveCount = primitive_count;
    const VkAccelerationStructureBuildRangeInfoKHR* build_range_ptr = &build_range;

    const bool build_succeeded = ExecuteImmediateCommands(context, command_pool, [&](VkCommandBuffer command_buffer)
    {
        context.GetRayTracingDispatch().cmd_build_acceleration_structures(command_buffer, 1, &build_geometry_info, &build_range_ptr);

        VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(
            command_buffer,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
            0,
            1,
            &barrier,
            0,
            nullptr,
            0,
            nullptr);
    });

    DestroyGpuBuffer(&context, scratch_buffer);
    if (!build_succeeded)
    {
        DestroyAccelerationStructure(&context, cache_entry.acceleration_structure);
        return false;
    }

    cache_entry.vertex_device_address = mesh.vertex_device_address;
    cache_entry.index_device_address = mesh.index_device_address;
    cache_entry.vertex_count = mesh.vertex_count;
    cache_entry.index_count = mesh.index_count;
    cache_entry.opaque = IsOpaqueMesh(mesh);
    return true;
}
}

void RayTracing::ResetAccumulationState()
{
    accumulation_reference_uniforms_ = {};
    accumulation_reference_uniforms_valid_ = false;
    accumulation_frame_count_ = 0;
    accumulation_reset_requested_ = true;
}

bool RayTracing::Initialize(VulkanContext* context)
{
    vulkan_context_ = context;
    available_ = false;
    status_message_.clear();

    if (vulkan_context_ == nullptr)
    {
        status_message_ = "No Vulkan context available";
        return false;
    }

    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    if (command_pool_ == VK_NULL_HANDLE)
    {
        VkCommandPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = vulkan_context_->GetQueueFamily();
        VkResult result = vkCreateCommandPool(device, &pool_info, allocator, &command_pool_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            status_message_ = "Failed to create viewport RT command pool";
            return false;
        }

        VkCommandBufferAllocateInfo allocate_info = {};
        allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool = command_pool_;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(device, &allocate_info, &command_buffer_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            status_message_ = "Failed to allocate viewport RT command buffer";
            DestroyFrameResources();
            return false;
        }
    }

    if (render_fence_ == VK_NULL_HANDLE)
    {
        VkFenceCreateInfo fence_info = {};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        const VkResult result = vkCreateFence(device, &fence_info, allocator, &render_fence_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            status_message_ = "Failed to create viewport RT render fence";
            DestroyFrameResources();
            return false;
        }
    }

    if (output_sampler_ == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo sampler_info = {};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 1.0f;
        const VkResult result = vkCreateSampler(device, &sampler_info, allocator, &output_sampler_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            status_message_ = "Failed to create viewport RT preview sampler";
            DestroyFrameResources();
            return false;
        }
    }

    const VulkanRayTracingSupport& support = vulkan_context_->GetRayTracingSupport();
    if (!support.supported)
    {
        status_message_ = "Ray tracing extensions or features are unavailable on the selected GPU";
        return false;
    }

    if (!support.enabled)
    {
        status_message_ = "Ray tracing support was detected but not enabled on the Vulkan device";
        return false;
    }

    if (!HasRequiredDispatch(vulkan_context_->GetRayTracingDispatch()))
    {
        status_message_ = "Ray tracing device entry points were not loaded";
        return false;
    }

    available_ = true;
    status_message_ = "Ray tracing ready";
    return true;
}

void RayTracing::Shutdown()
{
    DestroyOutputResources();
    DestroyPipelineResources();
    DestroySceneResources();
    DestroyFrameResources();
    vulkan_context_ = nullptr;
    available_ = false;
    status_message_.clear();
}

bool RayTracing::EnsureViewportOutput(std::uint32_t width, std::uint32_t height)
{
    if (vulkan_context_ == nullptr || width == 0 || height == 0 || output_sampler_ == VK_NULL_HANDLE)
    {
        return false;
    }

    if (output_image_ != VK_NULL_HANDLE && history_image_ != VK_NULL_HANDLE && output_width_ == width && output_height_ == height)
    {
        return true;
    }

    DestroyOutputResources();

    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult result = vkCreateImage(device, &image_info, allocator, &output_image_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        status_message_ = "Failed to create viewport RT output image";
        DestroyOutputResources();
        return false;
    }

    VkMemoryRequirements memory_requirements = {};
    vkGetImageMemoryRequirements(device, output_image_, &memory_requirements);

    VkMemoryAllocateInfo allocation_info = {};
    allocation_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation_info.allocationSize = memory_requirements.size;
    allocation_info.memoryTypeIndex = FindMemoryType(
        vulkan_context_->GetPhysicalDevice(),
        memory_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation_info.memoryTypeIndex == UINT32_MAX)
    {
        status_message_ = "Failed to find memory type for viewport RT output image";
        DestroyOutputResources();
        return false;
    }

    result = vkAllocateMemory(device, &allocation_info, allocator, &output_memory_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        status_message_ = "Failed to allocate viewport RT output image memory";
        DestroyOutputResources();
        return false;
    }

    result = vkBindImageMemory(device, output_image_, output_memory_, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        status_message_ = "Failed to bind viewport RT output image memory";
        DestroyOutputResources();
        return false;
    }

    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = output_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(device, &view_info, allocator, &output_view_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        status_message_ = "Failed to create viewport RT output image view";
        DestroyOutputResources();
        return false;
    }

    output_descriptor_set_ = ImGui_ImplVulkan_AddTexture(output_sampler_, output_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (output_descriptor_set_ == VK_NULL_HANDLE)
    {
        status_message_ = "Failed to register viewport RT preview texture";
        DestroyOutputResources();
        return false;
    }

    output_width_ = width;
    output_height_ = height;
    output_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!CreateVulkanImage(
            vulkan_context_->GetPhysicalDevice(),
            device,
            allocator,
            width,
            height,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            history_image_,
            history_memory_,
            history_view_))
    {
        status_message_ = "Failed to create viewport RT history image";
        DestroyOutputResources();
        return false;
    }

    history_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    ResetAccumulationState();
    return true;
}

bool RayTracing::UpdateScene(const std::vector<MeshInput>& meshes, const std::vector<InstanceInput>& instances)
{
    if (!available_)
    {
        return true;
    }

    if (vulkan_context_ == nullptr || command_pool_ == VK_NULL_HANDLE)
    {
        status_message_ = "RT scene update requested before module initialization";
        return false;
    }

    // Build CPU-side records into local vectors first so we can compute signatures before
    // committing any GPU work.  This avoids touching the GPU entirely on the hot drag path.
    std::unordered_map<std::string, const MeshInput*> meshes_by_key;
    std::unordered_map<std::string, std::uint32_t> mesh_index_by_key;
    std::unordered_map<std::uintptr_t, std::uint32_t> texture_index_by_view;
    meshes_by_key.reserve(meshes.size());
    mesh_index_by_key.reserve(meshes.size());
    texture_index_by_view.reserve(meshes.size());

    std::vector<MeshRecordGpu> new_mesh_records;
    std::vector<SectionRecordGpu> new_section_records;
    std::vector<MaterialRecordGpu> new_material_records;
    std::vector<VkDescriptorImageInfo> new_texture_descriptors;

    for (const MeshInput& mesh : meshes)
    {
        if (mesh.key.empty() || mesh.vertex_device_address == 0 || mesh.index_device_address == 0 || mesh.vertex_count == 0 || mesh.index_count < 3)
        {
            continue;
        }

        meshes_by_key[mesh.key] = &mesh;
        mesh_index_by_key.emplace(mesh.key, static_cast<std::uint32_t>(new_mesh_records.size()));

        MeshRecordGpu mesh_record{};
        mesh_record.vertex_buffer_address = mesh.vertex_device_address;
        mesh_record.index_buffer_address = mesh.index_device_address;
        mesh_record.vertex_count = mesh.vertex_count;
        mesh_record.vertex_stride = mesh.vertex_stride;
        mesh_record.index_count = mesh.index_count;
        mesh_record.section_offset = static_cast<std::uint32_t>(new_section_records.size());
        mesh_record.section_count = static_cast<std::uint32_t>(mesh.sections.size());
        mesh_record.material_offset = static_cast<std::uint32_t>(new_material_records.size());

        for (const MeshSectionRecord& section : mesh.sections)
        {
            SectionRecordGpu section_record{};
            section_record.first_index = section.first_index;
            section_record.index_count = section.index_count;
            section_record.material_index = mesh_record.material_offset + section.material_index;
            section_record.uses_alpha_transparency = section.uses_alpha_transparency ? 1u : 0u;
            new_section_records.push_back(section_record);
        }

        for (const MaterialRecord& material : mesh.materials)
        {
            MaterialRecordGpu material_record{};
            material_record.base_color = material.base_color;
            material_record.emissive_data[0] = material.emissive_color[0];
            material_record.emissive_data[1] = material.emissive_color[1];
            material_record.emissive_data[2] = material.emissive_color[2];
            material_record.emissive_data[3] = material.normal_scale;
            material_record.surface_data[0] = material.metallic_factor;
            material_record.surface_data[1] = material.roughness_factor;
            material_record.surface_data[2] = material.occlusion_strength;
            material_record.uses_alpha_transparency = material.uses_alpha_transparency ? 1u : 0u;

            auto resolve_texture_index = [&](VkImageView image_view) -> std::uint32_t
            {
                if (image_view == VK_NULL_HANDLE || new_texture_descriptors.size() >= kMaxTextures)
                {
                    return 0xFFFFFFFFu;
                }

                const std::uintptr_t view_key = reinterpret_cast<std::uintptr_t>(image_view);
                auto texture_it = texture_index_by_view.find(view_key);
                if (texture_it == texture_index_by_view.end())
                {
                    const std::uint32_t texture_index = static_cast<std::uint32_t>(new_texture_descriptors.size());
                    texture_index_by_view.emplace(view_key, texture_index);

                    VkDescriptorImageInfo image_info = {};
                    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    image_info.imageView = image_view;
                    new_texture_descriptors.push_back(image_info);
                    return texture_index;
                }

                return texture_it->second;
            };

            material_record.base_color_texture_index = resolve_texture_index(material.base_color_view);
            material_record.metallic_roughness_texture_index = resolve_texture_index(material.metallic_roughness_view);
            material_record.normal_texture_index = resolve_texture_index(material.normal_view);
            material_record.occlusion_texture_index = resolve_texture_index(material.occlusion_view);
            material_record.emissive_texture_index = resolve_texture_index(material.emissive_view);

            new_material_records.push_back(material_record);
        }

        new_mesh_records.push_back(mesh_record);
    }

    // Geometry signature covers mesh/section/material/texture records.
    // It is stable during a gizmo drag (only instance transforms change), so we can skip
    // the storage-buffer rebuild and descriptor update on the hot path.
    std::uint64_t new_geometry_sig = kFnvOffsetBasis;
    HashVector(new_geometry_sig, new_mesh_records);
    HashVector(new_geometry_sig, new_section_records);
    HashVector(new_geometry_sig, new_material_records);
    HashVector(new_geometry_sig, new_texture_descriptors);

    const bool geometry_changed = !geometry_signature_valid_ || geometry_signature_ != new_geometry_sig;
    if (geometry_changed)
    {
        mesh_records_cpu_      = std::move(new_mesh_records);
        section_records_cpu_   = std::move(new_section_records);
        material_records_cpu_  = std::move(new_material_records);
        texture_descriptors_cpu_ = std::move(new_texture_descriptors);
        geometry_signature_       = new_geometry_sig;
        geometry_signature_valid_ = true;

        auto rebuild_storage_buffer = [&](const auto& cpu_data, GpuBuffer& gpu_buffer, const char* failure_message) -> bool
        {
            DestroyGpuBuffer(vulkan_context_, gpu_buffer);
            if (cpu_data.empty())
            {
                return true;
            }

            const VkDeviceSize buffer_size = static_cast<VkDeviceSize>(cpu_data.size() * sizeof(cpu_data[0]));
            if (!CreateGpuBuffer(
                    *vulkan_context_,
                    buffer_size,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    gpu_buffer))
            {
                status_message_ = failure_message;
                return false;
            }

            if (!UploadGpuBuffer(*vulkan_context_, gpu_buffer, cpu_data.data(), static_cast<std::size_t>(buffer_size)))
            {
                status_message_ = failure_message;
                DestroyGpuBuffer(vulkan_context_, gpu_buffer);
                return false;
            }

            return true;
        };

        if (!rebuild_storage_buffer(mesh_records_cpu_, mesh_record_buffer_, "Failed to upload viewport RT mesh records") ||
            !rebuild_storage_buffer(section_records_cpu_, section_record_buffer_, "Failed to upload viewport RT section records") ||
            !rebuild_storage_buffer(material_records_cpu_, material_record_buffer_, "Failed to upload viewport RT material records"))
        {
            return false;
        }

        descriptors_dirty_ = true;
        ResetAccumulationState();
    }

    if (meshes_by_key.empty() || instances.empty())
    {
        // No renderable instances – schedule a TLAS clear to be handled in RenderFrame.
        pending_acceleration_instances_.clear();
        tlas_rebuild_pending_         = true;
        tlas_refit_pending_           = false;
        tlas_topology_signature_valid_ = false;
        scene_signature_valid_        = false;
        ResetAccumulationState();
        status_message_ = "RT scene cleared";
        return true;
    }

    // Build / reuse bottom-level acceleration structures (only when mesh geometry changes).
    for (const auto& [mesh_key, mesh] : meshes_by_key)
    {
        BottomLevelCacheEntry& cache_entry = bottom_level_cache_[mesh_key];
        const bool needs_rebuild =
            cache_entry.acceleration_structure.handle == VK_NULL_HANDLE ||
            cache_entry.vertex_device_address != mesh->vertex_device_address ||
            cache_entry.index_device_address != mesh->index_device_address ||
            cache_entry.vertex_count != mesh->vertex_count ||
            cache_entry.index_count != mesh->index_count ||
            cache_entry.opaque != IsOpaqueMesh(*mesh);

        if (!needs_rebuild)
        {
            continue;
        }

        if (!BuildBottomLevelAccelerationStructure(*vulkan_context_, command_pool_, *mesh, cache_entry))
        {
            status_message_ = "Failed to build viewport RT bottom-level acceleration structure";
            return false;
        }
    }

    // Build the instance list (BLAS references + per-instance transforms).
    std::vector<VkAccelerationStructureInstanceKHR> new_instances;
    new_instances.reserve(instances.size());
    for (const InstanceInput& instance : instances)
    {
        const auto mesh_it = bottom_level_cache_.find(instance.mesh_key);
        const auto mesh_index_it = mesh_index_by_key.find(instance.mesh_key);
        if (mesh_it == bottom_level_cache_.end() ||
            mesh_index_it == mesh_index_by_key.end() ||
            mesh_it->second.acceleration_structure.device_address == 0)
        {
            continue;
        }

        VkAccelerationStructureInstanceKHR acceleration_instance = {};
        acceleration_instance.transform = ToVkTransformMatrix(instance.transform);
        acceleration_instance.instanceCustomIndex = mesh_index_it->second;
        acceleration_instance.mask = 0xFF;
        acceleration_instance.instanceShaderBindingTableRecordOffset = 0;
        acceleration_instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        acceleration_instance.accelerationStructureReference = mesh_it->second.acceleration_structure.device_address;
        new_instances.push_back(acceleration_instance);
    }

    if (new_instances.empty())
    {
        pending_acceleration_instances_.clear();
        tlas_rebuild_pending_         = true;
        tlas_refit_pending_           = false;
        tlas_topology_signature_valid_ = false;
        scene_signature_valid_        = false;
        ResetAccumulationState();
        status_message_ = "RT scene has no buildable instances";
        return true;
    }

    // Topology signature hashes BLAS device-addresses and instance count.
    // This is stable during a gizmo drag, enabling a cheap TLAS refit instead of a rebuild.
    std::uint64_t new_topo_sig = kFnvOffsetBasis;
    const std::uint32_t new_instance_count = static_cast<std::uint32_t>(new_instances.size());
    HashBytes(new_topo_sig, &new_instance_count, sizeof(new_instance_count));
    for (const auto& inst : new_instances)
    {
        HashBytes(new_topo_sig, &inst.accelerationStructureReference, sizeof(inst.accelerationStructureReference));
    }

    const bool topology_changed = !tlas_topology_signature_valid_ || tlas_topology_signature_ != new_topo_sig;

    // Full scene signature for accumulation-reset detection (includes transforms).
    std::uint64_t full_sig = new_geometry_sig;
    HashVector(full_sig, new_instances);
    const bool scene_changed = !scene_signature_valid_ || scene_signature_ != full_sig;
    if (scene_changed)
    {
        scene_signature_       = full_sig;
        scene_signature_valid_ = true;
        ResetAccumulationState();
    }

    // Store the prepared instances; actual GPU work is deferred to RenderFrame where it
    // can be recorded directly into the render command buffer, avoiding an extra queue stall.
    pending_acceleration_instances_ = std::move(new_instances);

    if (topology_changed)
    {
        tlas_topology_signature_       = new_topo_sig;
        tlas_topology_signature_valid_ = true;
        tlas_rebuild_pending_          = true;
        tlas_refit_pending_            = false;
    }
    else if (scene_changed && !tlas_rebuild_pending_)
    {
        // Only transforms changed – schedule a fast refit.
        tlas_refit_pending_ = true;
    }
    else
    {
        tlas_refit_pending_ = false;
    }

    status_message_ = "RT scene update queued";
    return true;
}

void RayTracing::DestroyOutputResources()
{
    if (vulkan_context_ != nullptr &&
        (output_descriptor_set_ != VK_NULL_HANDLE || output_view_ != VK_NULL_HANDLE || output_image_ != VK_NULL_HANDLE || history_view_ != VK_NULL_HANDLE || history_image_ != VK_NULL_HANDLE))
    {
        // The previous UI frame can still be sampling the old viewport image when a resize triggers reallocation.
        vulkan_context_->WaitIdle();
    }

    output_width_ = 0;
    output_height_ = 0;
    output_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    history_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    ResetAccumulationState();

        // Mark descriptors dirty so they're rebuilt with new image views on next frame
        descriptors_dirty_ = true;

    if (output_descriptor_set_ != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkan_RemoveTexture(output_descriptor_set_);
        output_descriptor_set_ = VK_NULL_HANDLE;
    }

    if (vulkan_context_ == nullptr)
    {
        output_image_ = VK_NULL_HANDLE;
        output_memory_ = VK_NULL_HANDLE;
        output_view_ = VK_NULL_HANDLE;
        history_image_ = VK_NULL_HANDLE;
        history_memory_ = VK_NULL_HANDLE;
        history_view_ = VK_NULL_HANDLE;
        return;
    }

    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();
    if (output_view_ != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, output_view_, allocator);
        output_view_ = VK_NULL_HANDLE;
    }
    if (output_image_ != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, output_image_, allocator);
        output_image_ = VK_NULL_HANDLE;
    }
    if (output_memory_ != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, output_memory_, allocator);
        output_memory_ = VK_NULL_HANDLE;
    }
    if (history_view_ != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, history_view_, allocator);
        history_view_ = VK_NULL_HANDLE;
    }
    if (history_image_ != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, history_image_, allocator);
        history_image_ = VK_NULL_HANDLE;
    }
    if (history_memory_ != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, history_memory_, allocator);
        history_memory_ = VK_NULL_HANDLE;
    }
}

void RayTracing::DestroyFrameResources()
{
    if (vulkan_context_ != nullptr)
    {
        const VkDevice device = vulkan_context_->GetDevice();
        const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();
        if (render_fence_ != VK_NULL_HANDLE)
        {
            vkDestroyFence(device, render_fence_, allocator);
            render_fence_ = VK_NULL_HANDLE;
        }
        if (command_pool_ != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(device, command_pool_, allocator);
            command_pool_ = VK_NULL_HANDLE;
            command_buffer_ = VK_NULL_HANDLE;
        }
        if (output_sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, output_sampler_, allocator);
            output_sampler_ = VK_NULL_HANDLE;
        }
    }
    else
    {
        render_fence_ = VK_NULL_HANDLE;
        command_pool_ = VK_NULL_HANDLE;
        command_buffer_ = VK_NULL_HANDLE;
        output_sampler_ = VK_NULL_HANDLE;
    }
}

void RayTracing::DestroySceneResources()
{
    DestroyAccelerationStructure(vulkan_context_, top_level_as_);
    DestroyGpuBuffer(vulkan_context_, instance_buffer_);
    DestroyGpuBuffer(vulkan_context_, tlas_scratch_buffer_);
    DestroyGpuBuffer(vulkan_context_, uniform_buffer_);
    DestroyGpuBuffer(vulkan_context_, mesh_record_buffer_);
    DestroyGpuBuffer(vulkan_context_, section_record_buffer_);
    DestroyGpuBuffer(vulkan_context_, material_record_buffer_);
    for (auto& entry : bottom_level_cache_)
    {
        DestroyAccelerationStructure(vulkan_context_, entry.second.acceleration_structure);
    }
    bottom_level_cache_.clear();
    mesh_records_cpu_.clear();
    section_records_cpu_.clear();
    material_records_cpu_.clear();
    texture_descriptors_cpu_.clear();
    pending_acceleration_instances_.clear();
    tlas_rebuild_pending_         = false;
    tlas_refit_pending_           = false;
    tlas_capacity_                = 0;
    instance_buffer_capacity_     = 0;
    geometry_signature_valid_     = false;
    tlas_topology_signature_valid_ = false;
    scene_signature_valid_        = false;
    descriptors_dirty_            = false;
}

void RayTracing::DestroyPipelineResources()
{
    if (vulkan_context_ != nullptr)
    {
        const VkDevice device = vulkan_context_->GetDevice();
        const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

        DestroyGpuBuffer(vulkan_context_, raygen_sbt_.buffer);
        DestroyGpuBuffer(vulkan_context_, miss_sbt_.buffer);
        DestroyGpuBuffer(vulkan_context_, hit_sbt_.buffer);
        raygen_sbt_.region = {};
        miss_sbt_.region = {};
        hit_sbt_.region = {};

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
        if (descriptor_set_layout_ != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, descriptor_set_layout_, allocator);
            descriptor_set_layout_ = VK_NULL_HANDLE;
        }
        if (texture_sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, texture_sampler_, allocator);
            texture_sampler_ = VK_NULL_HANDLE;
        }
        if (fallback_texture_view_ != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, fallback_texture_view_, allocator);
            fallback_texture_view_ = VK_NULL_HANDLE;
        }
        if (fallback_texture_image_ != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, fallback_texture_image_, allocator);
            fallback_texture_image_ = VK_NULL_HANDLE;
        }
        if (fallback_texture_memory_ != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, fallback_texture_memory_, allocator);
            fallback_texture_memory_ = VK_NULL_HANDLE;
        }
    }
    else
    {
        raygen_sbt_ = {};
        miss_sbt_ = {};
        hit_sbt_ = {};
        pipeline_ = VK_NULL_HANDLE;
        pipeline_layout_ = VK_NULL_HANDLE;
        descriptor_set_layout_ = VK_NULL_HANDLE;
        texture_sampler_ = VK_NULL_HANDLE;
        fallback_texture_view_ = VK_NULL_HANDLE;
        fallback_texture_image_ = VK_NULL_HANDLE;
        fallback_texture_memory_ = VK_NULL_HANDLE;
    }

    descriptor_set_ = VK_NULL_HANDLE;
}

bool RayTracing::EnsurePipelineResources()
{
    if (!available_ || vulkan_context_ == nullptr)
    {
        return false;
    }

    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    if (uniform_buffer_.buffer == VK_NULL_HANDLE)
    {
        if (!CreateGpuBuffer(
                *vulkan_context_,
                sizeof(UniformBlock),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                uniform_buffer_))
        {
            status_message_ = "Failed to create viewport RT uniform buffer";
            return false;
        }
    }

    if (texture_sampler_ == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo sampler_info = {};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.maxLod = 1.0f;
        VkResult result = vkCreateSampler(device, &sampler_info, allocator, &texture_sampler_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            status_message_ = "Failed to create viewport RT material sampler";
            return false;
        }
    }

    if (fallback_texture_view_ == VK_NULL_HANDLE)
    {
        if (!CreateVulkanImage(
                vulkan_context_->GetPhysicalDevice(),
                device,
                allocator,
                1,
                1,
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT,
                fallback_texture_image_,
                fallback_texture_memory_,
                fallback_texture_view_))
        {
            status_message_ = "Failed to create viewport RT fallback texture";
            return false;
        }

        const bool uploaded = ExecuteImmediateCommands(*vulkan_context_, command_pool_, [&](VkCommandBuffer command_buffer)
        {
            TransitionImageLayout(
                command_buffer,
                fallback_texture_image_,
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                VK_ACCESS_TRANSFER_WRITE_BIT);

            VkClearColorValue clear_value = {};
            clear_value.float32[0] = 1.0f;
            clear_value.float32[1] = 1.0f;
            clear_value.float32[2] = 1.0f;
            clear_value.float32[3] = 1.0f;
            VkImageSubresourceRange range = {};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;
            vkCmdClearColorImage(command_buffer, fallback_texture_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_value, 1, &range);

            TransitionImageLayout(
                command_buffer,
                fallback_texture_image_,
                VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT);
        });
        if (!uploaded)
        {
            status_message_ = "Failed to upload viewport RT fallback texture";
            return false;
        }
    }

    if (descriptor_set_layout_ == VK_NULL_HANDLE)
    {
        std::array<VkDescriptorSetLayoutBinding, 8> bindings = {};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};
        bindings[2] = {
            2,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            1,
            VK_SHADER_STAGE_RAYGEN_BIT_KHR |
                VK_SHADER_STAGE_MISS_BIT_KHR |
                VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
                VK_SHADER_STAGE_ANY_HIT_BIT_KHR,
            nullptr};
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR, nullptr};
        bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR, nullptr};
        bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR, nullptr};
        bindings[6] = {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_ANY_HIT_BIT_KHR, nullptr};
        bindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_RAYGEN_BIT_KHR, nullptr};

        VkDescriptorSetLayoutCreateInfo layout_info = {};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        VkResult result = vkCreateDescriptorSetLayout(device, &layout_info, allocator, &descriptor_set_layout_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            status_message_ = "Failed to create viewport RT descriptor set layout";
            return false;
        }
    }

    if (descriptor_set_ == VK_NULL_HANDLE)
    {
        VkDescriptorSetAllocateInfo allocate_info = {};
        allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocate_info.descriptorPool = vulkan_context_->GetDescriptorPool();
        allocate_info.descriptorSetCount = 1;
        allocate_info.pSetLayouts = &descriptor_set_layout_;
        VkResult result = vkAllocateDescriptorSets(device, &allocate_info, &descriptor_set_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            status_message_ = "Failed to allocate viewport RT descriptor set";
            descriptor_set_ = VK_NULL_HANDLE;
            return false;
        }
        descriptors_dirty_ = true;
    }

    if (pipeline_layout_ == VK_NULL_HANDLE)
    {
        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &descriptor_set_layout_;
        VkResult result = vkCreatePipelineLayout(device, &layout_info, allocator, &pipeline_layout_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            status_message_ = "Failed to create viewport RT pipeline layout";
            return false;
        }
    }

    if (pipeline_ == VK_NULL_HANDLE)
    {
        VkShaderModule raygen_shader = LoadShaderModule(device, ResolveShaderPath("scene_viewport_rt.rgen.spv"));
        VkShaderModule miss_shader = LoadShaderModule(device, ResolveShaderPath("scene_viewport_rt.rmiss.spv"));
        VkShaderModule shadow_miss_shader = LoadShaderModule(device, ResolveShaderPath("scene_viewport_rt_shadow.rmiss.spv"));
        VkShaderModule closest_hit_shader = LoadShaderModule(device, ResolveShaderPath("scene_viewport_rt.rchit.spv"));
        VkShaderModule shadow_closest_hit_shader = LoadShaderModule(device, ResolveShaderPath("scene_viewport_rt_shadow.rchit.spv"));
        VkShaderModule primary_any_hit_shader = LoadShaderModule(device, ResolveShaderPath("scene_viewport_rt_primary.rahit.spv"));
        VkShaderModule shadow_any_hit_shader = LoadShaderModule(device, ResolveShaderPath("scene_viewport_rt_shadow.rahit.spv"));
        if (raygen_shader == VK_NULL_HANDLE ||
            miss_shader == VK_NULL_HANDLE ||
            shadow_miss_shader == VK_NULL_HANDLE ||
            closest_hit_shader == VK_NULL_HANDLE ||
            shadow_closest_hit_shader == VK_NULL_HANDLE ||
            primary_any_hit_shader == VK_NULL_HANDLE ||
            shadow_any_hit_shader == VK_NULL_HANDLE)
        {
            if (raygen_shader != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(device, raygen_shader, allocator);
            }
            if (miss_shader != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(device, miss_shader, allocator);
            }
            if (shadow_miss_shader != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(device, shadow_miss_shader, allocator);
            }
            if (closest_hit_shader != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(device, closest_hit_shader, allocator);
            }
            if (shadow_closest_hit_shader != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(device, shadow_closest_hit_shader, allocator);
            }
            if (primary_any_hit_shader != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(device, primary_any_hit_shader, allocator);
            }
            if (shadow_any_hit_shader != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(device, shadow_any_hit_shader, allocator);
            }
            status_message_ = "Failed to load viewport RT shaders";
            return false;
        }

        const char* entry_name = "main";
        std::array<VkPipelineShaderStageCreateInfo, 7> stages = {};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_RAYGEN_BIT_KHR, raygen_shader, entry_name, nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_MISS_BIT_KHR, miss_shader, entry_name, nullptr};
        stages[2] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_MISS_BIT_KHR, shadow_miss_shader, entry_name, nullptr};
        stages[3] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, closest_hit_shader, entry_name, nullptr};
        stages[4] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_ANY_HIT_BIT_KHR, primary_any_hit_shader, entry_name, nullptr};
        stages[5] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_ANY_HIT_BIT_KHR, shadow_any_hit_shader, entry_name, nullptr};
        stages[6] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, shadow_closest_hit_shader, entry_name, nullptr};

        std::array<VkRayTracingShaderGroupCreateInfoKHR, 5> groups = {};
        groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[0].generalShader = 0;
        groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

        groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[1].generalShader = 1;
        groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

        groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[2].generalShader = 2;
        groups[2].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

        groups[3].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[3].generalShader = VK_SHADER_UNUSED_KHR;
        groups[3].closestHitShader = 3;
        groups[3].anyHitShader = 4;
        groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

        groups[4].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[4].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[4].generalShader = VK_SHADER_UNUSED_KHR;
        groups[4].closestHitShader = 6;
        groups[4].anyHitShader = 5;
        groups[4].intersectionShader = VK_SHADER_UNUSED_KHR;

        VkRayTracingPipelineCreateInfoKHR pipeline_info = {VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
        pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.groupCount = static_cast<std::uint32_t>(groups.size());
        pipeline_info.pGroups = groups.data();
        pipeline_info.maxPipelineRayRecursionDepth =
            (std::min)(4u, vulkan_context_->GetRayTracingSupport().ray_tracing_pipeline_properties.maxRayRecursionDepth);
        pipeline_info.layout = pipeline_layout_;

        VkResult result = vulkan_context_->GetRayTracingDispatch().create_ray_tracing_pipelines(
            device,
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            1,
            &pipeline_info,
            allocator,
            &pipeline_);
        VulkanContext::CheckVkResult(result);

        vkDestroyShaderModule(device, raygen_shader, allocator);
        vkDestroyShaderModule(device, miss_shader, allocator);
        vkDestroyShaderModule(device, shadow_miss_shader, allocator);
        vkDestroyShaderModule(device, closest_hit_shader, allocator);
        vkDestroyShaderModule(device, shadow_closest_hit_shader, allocator);
        vkDestroyShaderModule(device, primary_any_hit_shader, allocator);
        vkDestroyShaderModule(device, shadow_any_hit_shader, allocator);

        if (result != VK_SUCCESS)
        {
            pipeline_ = VK_NULL_HANDLE;
            status_message_ = "Failed to create viewport RT pipeline";
            return false;
        }

        if (!BuildShaderBindingTable(*vulkan_context_, pipeline_, 0, 1, raygen_sbt_) ||
            !BuildShaderBindingTable(*vulkan_context_, pipeline_, 1, 2, miss_sbt_) ||
            !BuildShaderBindingTable(*vulkan_context_, pipeline_, 3, 2, hit_sbt_))
        {
            status_message_ = "Failed to build viewport RT shader binding table";
            return false;
        }
    }

    if (descriptors_dirty_)
    {
        const bool ok = UpdateDescriptors();
        if (ok)
        {
            descriptors_dirty_ = false;
        }
        return ok;
    }
    return true;
}

bool RayTracing::UpdateDescriptors()
{
    if (vulkan_context_ == nullptr || descriptor_set_ == VK_NULL_HANDLE || descriptor_set_layout_ == VK_NULL_HANDLE)
    {
        return false;
    }

    if (output_view_ == VK_NULL_HANDLE || history_view_ == VK_NULL_HANDLE || uniform_buffer_.buffer == VK_NULL_HANDLE || fallback_texture_view_ == VK_NULL_HANDLE)
    {
        return false;
    }

    std::array<VkDescriptorImageInfo, kMaxTextures> texture_infos = {};
    for (VkDescriptorImageInfo& texture_info : texture_infos)
    {
        texture_info.sampler = texture_sampler_;
        texture_info.imageView = fallback_texture_view_;
        texture_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    for (std::size_t texture_index = 0; texture_index < texture_descriptors_cpu_.size() && texture_index < kMaxTextures; ++texture_index)
    {
        texture_infos[texture_index] = texture_descriptors_cpu_[texture_index];
        texture_infos[texture_index].sampler = texture_sampler_;
    }

    VkWriteDescriptorSetAccelerationStructureKHR acceleration_write = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    acceleration_write.accelerationStructureCount = 1;
    acceleration_write.pAccelerationStructures = &top_level_as_.handle;

    VkDescriptorImageInfo output_image_info = {};
    output_image_info.imageView = output_view_;
    output_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo history_image_info = {};
    history_image_info.imageView = history_view_;
    history_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorBufferInfo uniform_info = {};
    uniform_info.buffer = uniform_buffer_.buffer;
    uniform_info.range = sizeof(UniformBlock);

    VkDescriptorBufferInfo mesh_info = {};
    mesh_info.buffer = mesh_record_buffer_.buffer;
    mesh_info.range = mesh_record_buffer_.size;

    VkDescriptorBufferInfo section_info = {};
    section_info.buffer = section_record_buffer_.buffer;
    section_info.range = section_record_buffer_.size;

    VkDescriptorBufferInfo material_info = {};
    material_info.buffer = material_record_buffer_.buffer;
    material_info.range = material_record_buffer_.size;

    std::array<VkWriteDescriptorSet, 8> writes = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].pNext = &acceleration_write;
    writes[0].dstSet = descriptor_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = descriptor_set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &output_image_info;

    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[2].dstSet = descriptor_set_;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &uniform_info;

    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[3].dstSet = descriptor_set_;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &mesh_info;

    writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[4].dstSet = descriptor_set_;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[4].pBufferInfo = &section_info;

    writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[5].dstSet = descriptor_set_;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[5].pBufferInfo = &material_info;

    writes[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[6].dstSet = descriptor_set_;
    writes[6].dstBinding = 6;
    writes[6].descriptorCount = kMaxTextures;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].pImageInfo = texture_infos.data();

    writes[7] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[7].dstSet = descriptor_set_;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[7].pImageInfo = &history_image_info;

    vkUpdateDescriptorSets(vulkan_context_->GetDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return true;
}

bool RayTracing::RenderFrame(
    const ResolvedSceneLighting& lighting,
    const std::array<float, 16>& view_inverse,
    const std::array<float, 16>& projection_inverse,
    bool grid_enabled,
    float grid_spacing,
    float grid_origin_x,
    float grid_origin_z,
    float grid_extent)
{
    if (!available_ ||
        vulkan_context_ == nullptr ||
        output_image_ == VK_NULL_HANDLE ||
        history_image_ == VK_NULL_HANDLE ||
        command_buffer_ == VK_NULL_HANDLE ||
        render_fence_ == VK_NULL_HANDLE ||
        command_pool_ == VK_NULL_HANDLE)
    {
        return false;
    }

    const VkDevice device = vulkan_context_->GetDevice();
    VkResult result = vkGetFenceStatus(device, render_fence_);
    if (result == VK_NOT_READY)
    {
        // Keep the UI responsive instead of blocking the main thread while the GPU finishes.
        status_message_ = "Viewport RT frame pending";
        return true;
    }
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    result = vkResetFences(device, 1, &render_fence_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    result = vkResetCommandPool(device, command_pool_, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    // --- Handle pending TLAS work (after fence ensures the previous frame is done) ---
    // Instance buffer and TLAS AS are managed here on the CPU side; the actual
    // cmd_build_acceleration_structures call is recorded into the render CB below.
    if (tlas_rebuild_pending_ || tlas_refit_pending_)
    {
        if (!pending_acceleration_instances_.empty())
        {
            const std::uint32_t needed_count =
                static_cast<std::uint32_t>(pending_acceleration_instances_.size());
            const VkDeviceSize needed_size =
                static_cast<VkDeviceSize>(needed_count * sizeof(VkAccelerationStructureInstanceKHR));

            // Grow the instance buffer only when the capacity is exceeded (avoids
            // deallocation/reallocation on every gizmo-drag frame).
            if (needed_count > instance_buffer_capacity_)
            {
                DestroyGpuBuffer(vulkan_context_, instance_buffer_);
                instance_buffer_capacity_ = 0;
                if (!CreateGpuBuffer(
                        *vulkan_context_,
                        needed_size,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        instance_buffer_))
                {
                    status_message_ = "Failed to create viewport RT instance buffer";
                    tlas_rebuild_pending_ = false;
                    tlas_refit_pending_   = false;
                    return false;
                }
                instance_buffer_capacity_ = needed_count;
            }

            if (!UploadGpuBuffer(
                    *vulkan_context_,
                    instance_buffer_,
                    pending_acceleration_instances_.data(),
                    static_cast<std::size_t>(needed_size)))
            {
                status_message_ = "Failed to upload viewport RT instance buffer";
                tlas_rebuild_pending_ = false;
                tlas_refit_pending_   = false;
                return false;
            }
        }

        if (tlas_rebuild_pending_)
        {
            if (pending_acceleration_instances_.empty())
            {
                // No instances – destroy any existing TLAS.
                DestroyAccelerationStructure(vulkan_context_, top_level_as_);
                tlas_capacity_    = 0;
                descriptors_dirty_ = true;
            }
            else
            {
                const std::uint32_t primitive_count =
                    static_cast<std::uint32_t>(pending_acceleration_instances_.size());

                if (primitive_count > tlas_capacity_)
                {
                    // Query required sizes for the new topology.
                    VkAccelerationStructureGeometryInstancesDataKHR size_instances = {
                        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
                    size_instances.data.deviceAddress = instance_buffer_.device_address;

                    VkAccelerationStructureGeometryKHR size_geometry = {
                        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
                    size_geometry.geometryType          = VK_GEOMETRY_TYPE_INSTANCES_KHR;
                    size_geometry.geometry.instances    = size_instances;

                    VkAccelerationStructureBuildGeometryInfoKHR size_query = {
                        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
                    size_query.type          = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
                    size_query.flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                                               VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
                    size_query.geometryCount = 1;
                    size_query.pGeometries   = &size_geometry;

                    VkAccelerationStructureBuildSizesInfoKHR build_sizes = {
                        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
                    vulkan_context_->GetRayTracingDispatch().get_acceleration_structure_build_sizes(
                        device,
                        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                        &size_query,
                        &primitive_count,
                        &build_sizes);

                    // Recreate the TLAS AS buffer sized for the new topology.
                    DestroyAccelerationStructure(vulkan_context_, top_level_as_);
                    if (!CreateAccelerationStructure(
                            *vulkan_context_,
                            VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                            build_sizes.accelerationStructureSize,
                            top_level_as_))
                    {
                        status_message_ = "Failed to create viewport RT top-level acceleration structure";
                        tlas_rebuild_pending_ = false;
                        tlas_refit_pending_   = false;
                        return false;
                    }

                    // Ensure the persistent scratch buffer is large enough for both
                    // full builds and refits.
                    const VkDeviceSize scratch_needed =
                        (std::max)(build_sizes.buildScratchSize, build_sizes.updateScratchSize);
                    if (scratch_needed > tlas_scratch_buffer_.size)
                    {
                        DestroyGpuBuffer(vulkan_context_, tlas_scratch_buffer_);
                        if (!CreateGpuBuffer(
                                *vulkan_context_,
                                scratch_needed,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                tlas_scratch_buffer_))
                        {
                            status_message_ = "Failed to create viewport RT TLAS scratch buffer";
                            tlas_rebuild_pending_ = false;
                            tlas_refit_pending_   = false;
                            return false;
                        }
                    }

                    tlas_capacity_    = primitive_count;
                    descriptors_dirty_ = true;
                }
            }
        }
    }
        if ((tlas_rebuild_pending_ || tlas_refit_pending_) &&
            top_level_as_.handle != VK_NULL_HANDLE &&
            instance_buffer_.device_address != 0 &&
            tlas_scratch_buffer_.device_address != 0 &&
            !pending_acceleration_instances_.empty())
        {
            const std::uint32_t primitive_count =
                static_cast<std::uint32_t>(pending_acceleration_instances_.size());

            VkAccelerationStructureGeometryInstancesDataKHR instances_data = {
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
            instances_data.data.deviceAddress = instance_buffer_.device_address;

            VkAccelerationStructureGeometryKHR geometry = {
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geometry.geometry.instances = instances_data;

            VkAccelerationStructureBuildGeometryInfoKHR build_geometry_info = {
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            build_geometry_info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            build_geometry_info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                                        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            build_geometry_info.geometryCount = 1;
            build_geometry_info.pGeometries = &geometry;
            if (tlas_refit_pending_ && !tlas_rebuild_pending_)
            {
                build_geometry_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
                build_geometry_info.srcAccelerationStructure = top_level_as_.handle;
            }
            else
            {
                build_geometry_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            }
            build_geometry_info.dstAccelerationStructure = top_level_as_.handle;
            build_geometry_info.scratchData.deviceAddress = tlas_scratch_buffer_.device_address;

            VkAccelerationStructureBuildRangeInfoKHR build_range = {};
            build_range.primitiveCount = primitive_count;
            const VkAccelerationStructureBuildRangeInfoKHR* build_range_ptr = &build_range;

            const bool build_succeeded = ExecuteImmediateCommands(*vulkan_context_, command_pool_, [&](VkCommandBuffer command_buffer)
            {
                vulkan_context_->GetRayTracingDispatch().cmd_build_acceleration_structures(
                    command_buffer,
                    1,
                    &build_geometry_info,
                    &build_range_ptr);

                VkMemoryBarrier barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                vkCmdPipelineBarrier(
                    command_buffer,
                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                    VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    0,
                    1,
                    &barrier,
                    0,
                    nullptr,
                    0,
                    nullptr);
            });

            if (!build_succeeded)
            {
                status_message_ = "Failed to build viewport RT top-level acceleration structure";
                tlas_rebuild_pending_ = false;
                tlas_refit_pending_ = false;
                return false;
            }
        }

    tlas_rebuild_pending_ = false;
    tlas_refit_pending_ = false;

    // ---------------------------------------------------------------------------

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_buffer_, &begin_info);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    auto finalize_and_submit = [&]() -> bool
    {
        TransitionImageLayout(
            command_buffer_,
            output_image_,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
        output_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkResult end_result = vkEndCommandBuffer(command_buffer_);
        VulkanContext::CheckVkResult(end_result);
        if (end_result != VK_SUCCESS)
        {
            return false;
        }

        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffer_;
        end_result = vkQueueSubmit(vulkan_context_->GetQueue(), 1, &submit_info, render_fence_);
        VulkanContext::CheckVkResult(end_result);
        if (end_result != VK_SUCCESS)
        {
            return false;
        }

        return true;
    };

    TransitionImageLayout(
        command_buffer_,
        output_image_,
        VK_IMAGE_ASPECT_COLOR_BIT,
        output_layout_,
        VK_IMAGE_LAYOUT_GENERAL,
        output_layout_ == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_TRANSFER_BIT,
        output_layout_ == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    output_layout_ = VK_IMAGE_LAYOUT_GENERAL;

    TransitionImageLayout(
        command_buffer_,
        history_image_,
        VK_IMAGE_ASPECT_COLOR_BIT,
        history_layout_,
        VK_IMAGE_LAYOUT_GENERAL,
        history_layout_ == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        history_layout_ == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    history_layout_ = VK_IMAGE_LAYOUT_GENERAL;

    if (top_level_as_.handle == VK_NULL_HANDLE ||
        mesh_record_buffer_.buffer == VK_NULL_HANDLE ||
        section_record_buffer_.buffer == VK_NULL_HANDLE ||
        material_record_buffer_.buffer == VK_NULL_HANDLE)
    {
        ResetAccumulationState();
        VkClearColorValue clear_value = {};
        clear_value.float32[0] = 0.08f;
        clear_value.float32[1] = 0.09f;
        clear_value.float32[2] = 0.11f;
        clear_value.float32[3] = 1.0f;
        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(command_buffer_, output_image_, VK_IMAGE_LAYOUT_GENERAL, &clear_value, 1, &range);
        return finalize_and_submit();
    }

    if (!EnsurePipelineResources())
    {
        vkEndCommandBuffer(command_buffer_);
        return false;
    }

    UniformBlock uniforms{};
    uniforms.view_inverse = view_inverse;
    uniforms.projection_inverse = projection_inverse;
    uniforms.ambient_light = lighting.ambient_light;
    uniforms.directional_light_color = lighting.directional_light_color;
    uniforms.directional_light_direction = lighting.directional_light_direction;
    uniforms.directional_light_data = lighting.directional_light_data;
    uniforms.point_light_color = lighting.point_light_color;
    uniforms.point_light_position = lighting.point_light_position;
    uniforms.point_light_data = lighting.point_light_data;
    uniforms.spot_light_color = lighting.spot_light_color;
    uniforms.spot_light_direction = lighting.spot_light_direction;
    uniforms.spot_light_position = lighting.spot_light_position;
    uniforms.spot_light_data = lighting.spot_light_data;
    uniforms.grid_data = {grid_enabled ? 1.0f : 0.0f, grid_spacing, 0.0f, 0.0f};
    uniforms.grid_origin_extent = {grid_origin_x, 0.0f, grid_origin_z, grid_extent};
    uniforms.mesh_count = static_cast<std::uint32_t>(mesh_records_cpu_.size());
    uniforms.material_count = static_cast<std::uint32_t>(material_records_cpu_.size());
    uniforms.section_count = static_cast<std::uint32_t>(section_records_cpu_.size());
    uniforms.texture_count = static_cast<std::uint32_t>(texture_descriptors_cpu_.size());

    UniformBlock accumulation_reference = uniforms;
    accumulation_reference.accumulation_data = {0, 0, 0, 0};
    const bool accumulation_reset =
        accumulation_reset_requested_ ||
        !accumulation_reference_uniforms_valid_ ||
        std::memcmp(&accumulation_reference, &accumulation_reference_uniforms_, sizeof(UniformBlock)) != 0;
    if (accumulation_reset)
    {
        accumulation_frame_count_ = 0;
    }
    uniforms.accumulation_data = {
        accumulation_frame_count_,
        accumulation_frame_count_ > 0 ? 1u : 0u,
        accumulation_reset ? 1u : 0u,
        0u};

    if (!UploadGpuBuffer(*vulkan_context_, uniform_buffer_, &uniforms, sizeof(uniforms)))
    {
        status_message_ = "Failed to upload viewport RT uniforms";
        vkEndCommandBuffer(command_buffer_);
        return false;
    }

    if (descriptors_dirty_)
    {
        if (!UpdateDescriptors())
        {
            status_message_ = "Failed to update viewport RT descriptors";
            vkEndCommandBuffer(command_buffer_);
            return false;
        }
        descriptors_dirty_ = false;
    }

    vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipeline_);
    vkCmdBindDescriptorSets(
        command_buffer_,
        VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
        pipeline_layout_,
        0,
        1,
        &descriptor_set_,
        0,
        nullptr);

    VkStridedDeviceAddressRegionKHR callable_region = {};
    vulkan_context_->GetRayTracingDispatch().cmd_trace_rays(
        command_buffer_,
        &raygen_sbt_.region,
        &miss_sbt_.region,
        &hit_sbt_.region,
        &callable_region,
        output_width_,
        output_height_,
        1);

    accumulation_reference_uniforms_ = accumulation_reference;
    accumulation_reference_uniforms_valid_ = true;

    status_message_ = "Viewport RT frame traced";
    const bool submitted = finalize_and_submit();
    if (submitted)
    {
        accumulation_frame_count_ = (std::min)(accumulation_frame_count_ + 1u, kMaxAccumulationFrames);
        accumulation_reset_requested_ = false;
    }
    return submitted;
}