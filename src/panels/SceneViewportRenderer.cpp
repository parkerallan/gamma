#include "panels/SceneViewportRenderer.h"

#include <SDL3/SDL.h>

#include <ImGuizmo.h>

#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

namespace
{
constexpr float kPi = 3.1415926535f;

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct SceneGpuVertex
{
    float position[3] = {0.0f, 0.0f, 0.0f};
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float uv[2] = {0.0f, 0.0f};
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct SceneUniformBlock
{
    float model[16] = {};
    float model_view_projection[16] = {};
};

std::uint32_t FindMemoryType(VkPhysicalDevice physical_device, std::uint32_t type_filter, VkMemoryPropertyFlags properties);
bool CreateVulkanImage(
    VkPhysicalDevice physical_device,
    VkDevice device,
    std::uint32_t width,
    std::uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImageAspectFlags aspect_mask,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& image_view);

SceneGpuVertex BuildColoredVertex(float x, float y, float z, float nx, float ny, float nz, float r, float g, float b, float a)
{
    SceneGpuVertex vertex;
    vertex.position[0] = x;
    vertex.position[1] = y;
    vertex.position[2] = z;
    vertex.normal[0] = nx;
    vertex.normal[1] = ny;
    vertex.normal[2] = nz;
    vertex.uv[0] = 0.0f;
    vertex.uv[1] = 0.0f;
    vertex.color[0] = r;
    vertex.color[1] = g;
    vertex.color[2] = b;
    vertex.color[3] = a;
    return vertex;
}

Vec3 ToVec3(const SceneVector3& value)
{
    return Vec3{value[0], value[1], value[2]};
}

SceneVector3 ToSceneVector3(const Vec3& value)
{
    return SceneVector3{value.x, value.y, value.z};
}

Vec3 Add(const Vec3& left, const Vec3& right)
{
    return Vec3{left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 Subtract(const Vec3& left, const Vec3& right)
{
    return Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 Multiply(const Vec3& value, float scalar)
{
    return Vec3{value.x * scalar, value.y * scalar, value.z * scalar};
}

float Dot(const Vec3& left, const Vec3& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vec3 Cross(const Vec3& left, const Vec3& right)
{
    return Vec3{
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

float Length(const Vec3& value)
{
    return std::sqrt(Dot(value, value));
}

Vec3 Normalize(const Vec3& value)
{
    const float length = Length(value);
    if (length <= 0.0001f)
    {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    return Multiply(value, 1.0f / length);
}

float DegreesToRadians(float degrees)
{
    return degrees * (kPi / 180.0f);
}

void SetIdentity(float* matrix)
{
    for (int index = 0; index < 16; ++index)
    {
        matrix[index] = 0.0f;
    }

    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

void MultiplyMatrix(const float* left, const float* right, float* result)
{
    float tmp[16];
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            tmp[column * 4 + row] = 0.0f;
            for (int inner = 0; inner < 4; ++inner)
            {
                tmp[column * 4 + row] += left[inner * 4 + row] * right[column * 4 + inner];
            }
        }
    }

    std::memcpy(result, tmp, sizeof(tmp));
}

void BuildScaleMatrix(const SceneVector3& scale, float* matrix)
{
    SetIdentity(matrix);
    matrix[0] = scale[0];
    matrix[5] = scale[1];
    matrix[10] = scale[2];
}

void BuildTranslationMatrix(const SceneVector3& translation, float* matrix)
{
    SetIdentity(matrix);
    matrix[12] = translation[0];
    matrix[13] = translation[1];
    matrix[14] = translation[2];
}

void BuildRotationXMatrix(float radians, float* matrix)
{
    SetIdentity(matrix);
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    matrix[5] = c;
    matrix[6] = s;
    matrix[9] = -s;
    matrix[10] = c;
}

void BuildRotationYMatrix(float radians, float* matrix)
{
    SetIdentity(matrix);
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    matrix[0] = c;
    matrix[2] = -s;
    matrix[8] = s;
    matrix[10] = c;
}

void BuildRotationZMatrix(float radians, float* matrix)
{
    SetIdentity(matrix);
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    matrix[0] = c;
    matrix[1] = s;
    matrix[4] = -s;
    matrix[5] = c;
}

void BuildPerspectiveMatrix(float fovy_degrees, float aspect, float z_near, float z_far, float* matrix)
{
    for (int index = 0; index < 16; ++index)
    {
        matrix[index] = 0.0f;
    }

    const float f = 1.0f / std::tan(DegreesToRadians(fovy_degrees) * 0.5f);
    matrix[0] = f / aspect;
    matrix[5] = f;
    matrix[10] = (z_near + z_far) / (z_near - z_far);
    matrix[11] = -1.0f;
    matrix[14] = (2.0f * z_near * z_far) / (z_near - z_far);
}

float SnapScalar(float value, float spacing)
{
    if (spacing <= 0.0f)
    {
        return value;
    }

    return std::round(value / spacing) * spacing;
}

void SnapVector(SceneVector3& value, float spacing)
{
    for (float& component : value)
    {
        component = SnapScalar(component, spacing);
    }
}

void BuildLookAtMatrix(const Vec3& eye, const Vec3& center, const Vec3& up, float* matrix)
{
    const Vec3 forward = Normalize(Subtract(center, eye));
    Vec3 side = Normalize(Cross(forward, up));
    if (Length(side) <= 0.0001f)
    {
        side = Vec3{1.0f, 0.0f, 0.0f};
    }
    const Vec3 true_up = Cross(side, forward);

    SetIdentity(matrix);
    matrix[0] = side.x;
    matrix[4] = side.y;
    matrix[8] = side.z;
    matrix[1] = true_up.x;
    matrix[5] = true_up.y;
    matrix[9] = true_up.z;
    matrix[2] = -forward.x;
    matrix[6] = -forward.y;
    matrix[10] = -forward.z;
    matrix[12] = -Dot(side, eye);
    matrix[13] = -Dot(true_up, eye);
    matrix[14] = Dot(forward, eye);
}

void BuildModelMatrix(const SceneViewportRenderer::QueuedSceneObject& object, float* matrix)
{
    float scale_matrix[16];
    float rotation_x_matrix[16];
    float rotation_y_matrix[16];
    float rotation_z_matrix[16];
    float translation_matrix[16];
    float temp_a[16];
    float temp_b[16];

    BuildScaleMatrix(object.scale, scale_matrix);
    BuildRotationXMatrix(DegreesToRadians(object.rotation[0]), rotation_x_matrix);
    BuildRotationYMatrix(DegreesToRadians(object.rotation[1]), rotation_y_matrix);
    BuildRotationZMatrix(DegreesToRadians(object.rotation[2]), rotation_z_matrix);
    BuildTranslationMatrix(object.position, translation_matrix);

    MultiplyMatrix(rotation_x_matrix, scale_matrix, temp_a);
    MultiplyMatrix(rotation_y_matrix, temp_a, temp_b);
    MultiplyMatrix(rotation_z_matrix, temp_b, temp_a);
    MultiplyMatrix(translation_matrix, temp_a, matrix);
}

void ExpandBoundsWithObject(Vec3& minimum, Vec3& maximum, const SceneViewportRenderer::QueuedSceneObject& object, const ModelAsset& asset)
{
    if (!asset.bounds.valid)
    {
        const Vec3 position = ToVec3(object.position);
        minimum.x = (std::min)(minimum.x, position.x);
        minimum.y = (std::min)(minimum.y, position.y);
        minimum.z = (std::min)(minimum.z, position.z);
        maximum.x = (std::max)(maximum.x, position.x);
        maximum.y = (std::max)(maximum.y, position.y);
        maximum.z = (std::max)(maximum.z, position.z);
        return;
    }

    float model_matrix[16];
    BuildModelMatrix(object, model_matrix);
    for (int corner_index = 0; corner_index < 8; ++corner_index)
    {
        const Vec3 local = Vec3{
            (corner_index & 1) == 0 ? asset.bounds.minimum[0] : asset.bounds.maximum[0],
            (corner_index & 2) == 0 ? asset.bounds.minimum[1] : asset.bounds.maximum[1],
            (corner_index & 4) == 0 ? asset.bounds.minimum[2] : asset.bounds.maximum[2]};
        const Vec3 world = Vec3{
            model_matrix[0] * local.x + model_matrix[4] * local.y + model_matrix[8] * local.z + model_matrix[12],
            model_matrix[1] * local.x + model_matrix[5] * local.y + model_matrix[9] * local.z + model_matrix[13],
            model_matrix[2] * local.x + model_matrix[6] * local.y + model_matrix[10] * local.z + model_matrix[14]};

        minimum.x = (std::min)(minimum.x, world.x);
        minimum.y = (std::min)(minimum.y, world.y);
        minimum.z = (std::min)(minimum.z, world.z);
        maximum.x = (std::max)(maximum.x, world.x);
        maximum.y = (std::max)(maximum.y, world.y);
        maximum.z = (std::max)(maximum.z, world.z);
    }
}

bool ComputeObjectBounds(const SceneViewportRenderer::QueuedSceneObject& object, const ModelAsset& asset, Vec3& minimum, Vec3& maximum)
{
    if (!asset.bounds.valid)
    {
        const Vec3 position = ToVec3(object.position);
        minimum = position;
        maximum = position;
        return false;
    }

    minimum = Vec3{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    maximum = Vec3{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};

    float model_matrix[16];
    BuildModelMatrix(object, model_matrix);
    for (int corner_index = 0; corner_index < 8; ++corner_index)
    {
        const Vec3 local = Vec3{
            (corner_index & 1) == 0 ? asset.bounds.minimum[0] : asset.bounds.maximum[0],
            (corner_index & 2) == 0 ? asset.bounds.minimum[1] : asset.bounds.maximum[1],
            (corner_index & 4) == 0 ? asset.bounds.minimum[2] : asset.bounds.maximum[2]};
        const Vec3 world = Vec3{
            model_matrix[0] * local.x + model_matrix[4] * local.y + model_matrix[8] * local.z + model_matrix[12],
            model_matrix[1] * local.x + model_matrix[5] * local.y + model_matrix[9] * local.z + model_matrix[13],
            model_matrix[2] * local.x + model_matrix[6] * local.y + model_matrix[10] * local.z + model_matrix[14]};

        minimum.x = (std::min)(minimum.x, world.x);
        minimum.y = (std::min)(minimum.y, world.y);
        minimum.z = (std::min)(minimum.z, world.z);
        maximum.x = (std::max)(maximum.x, world.x);
        maximum.y = (std::max)(maximum.y, world.y);
        maximum.z = (std::max)(maximum.z, world.z);
    }

    return true;
}

std::filesystem::path ResolveShaderPath(const char* file_name)
{
    const char* base_path_raw = SDL_GetBasePath();
    const std::filesystem::path base_path = base_path_raw != nullptr ? std::filesystem::path(base_path_raw) : std::filesystem::current_path();
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

SceneGpuVertex BuildSceneGpuVertex(const ModelVertex& vertex, const ModelMaterialAsset* material)
{
    SceneGpuVertex gpu_vertex;
    gpu_vertex.position[0] = vertex.position[0];
    gpu_vertex.position[1] = vertex.position[1];
    gpu_vertex.position[2] = vertex.position[2];
    gpu_vertex.normal[0] = vertex.normal[0];
    gpu_vertex.normal[1] = vertex.normal[1];
    gpu_vertex.normal[2] = vertex.normal[2];
    gpu_vertex.uv[0] = vertex.uv0[0];
    gpu_vertex.uv[1] = 1.0f - vertex.uv0[1];

    const std::array<float, 4> tint = material != nullptr ? material->base_color : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
    gpu_vertex.color[0] = tint[0];
    gpu_vertex.color[1] = tint[1];
    gpu_vertex.color[2] = tint[2];
    gpu_vertex.color[3] = tint[3];
    return gpu_vertex;
}

VkShaderModule LoadShaderModule(VkDevice device, const std::filesystem::path& path)
{
    const std::vector<std::uint8_t> shader_bytes = ReadBinaryFile(path);
    if (shader_bytes.empty())
    {
        SDL_Log("Failed to read shader file: %s", path.string().c_str());
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

bool CreateVulkanBuffer(
    VkPhysicalDevice physical_device,
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    SceneViewportRenderer::GpuBuffer& buffer)
{
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(device, &buffer_info, nullptr, &buffer.buffer);
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
    if (allocate_info.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyBuffer(device, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkAllocateMemory(device, &allocate_info, nullptr, &buffer.memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyBuffer(device, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, buffer.memory, nullptr);
        vkDestroyBuffer(device, buffer.buffer, nullptr);
        buffer.memory = VK_NULL_HANDLE;
        buffer.buffer = VK_NULL_HANDLE;
        return false;
    }

    buffer.size = size;
    return true;
}

bool UploadBufferData(VkDevice device, const SceneViewportRenderer::GpuBuffer& buffer, const void* data, std::size_t size)
{
    if (buffer.buffer == VK_NULL_HANDLE || buffer.memory == VK_NULL_HANDLE || data == nullptr || size == 0)
    {
        return false;
    }

    void* mapped = nullptr;
    const VkResult result = vkMapMemory(device, buffer.memory, 0, size, 0, &mapped);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS || mapped == nullptr)
    {
        return false;
    }

    std::memcpy(mapped, data, size);
    vkUnmapMemory(device, buffer.memory);
    return true;
}

bool ExecuteImmediateCommands(
    VkDevice device,
    VkCommandPool command_pool,
    VkQueue queue,
    const std::function<void(VkCommandBuffer)>& record_commands)
{
    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = command_pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;

    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(device, &allocate_info, &command_buffer);
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
        vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
        return false;
    }

    record_commands(command_buffer);

    result = vkEndCommandBuffer(command_buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
        return false;
    }

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    result = vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
        return false;
    }

    result = vkQueueWaitIdle(queue);
    VulkanContext::CheckVkResult(result);
    vkFreeCommandBuffers(device, command_pool, 1, &command_buffer);
    return result == VK_SUCCESS;
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

bool AllocateMaterialDescriptorSet(
    VkDevice device,
    VkDescriptorPool descriptor_pool,
    VkDescriptorSetLayout descriptor_set_layout,
    VkSampler sampler,
    VkImageView image_view,
    VkDescriptorSet& descriptor_set)
{
    VkDescriptorSetAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = descriptor_pool;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &descriptor_set_layout;

    VkResult result = vkAllocateDescriptorSets(device, &allocate_info, &descriptor_set);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkDescriptorImageInfo image_info = {};
    image_info.sampler = sampler;
    image_info.imageView = image_view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    return true;
}

bool CreateTextureFromAsset(
    VulkanContext& context,
    VkCommandPool command_pool,
    VkDescriptorSetLayout descriptor_set_layout,
    VkSampler sampler,
    const ModelTextureAsset& texture_asset,
    SceneViewportRenderer::GpuTexture& texture)
{
    if (!texture_asset.valid || texture_asset.width <= 0 || texture_asset.height <= 0 || texture_asset.pixels.empty())
    {
        return false;
    }

    const VkDevice device = context.GetDevice();
    const VkPhysicalDevice physical_device = context.GetPhysicalDevice();
    const VkDeviceSize upload_size = static_cast<VkDeviceSize>(texture_asset.width) * static_cast<VkDeviceSize>(texture_asset.height) * 4u;

    SceneViewportRenderer::GpuBuffer staging_buffer{};
    if (!CreateVulkanBuffer(
            physical_device,
            device,
            upload_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging_buffer))
    {
        return false;
    }

    if (!UploadBufferData(device, staging_buffer, texture_asset.pixels.data(), static_cast<std::size_t>(upload_size)))
    {
        vkFreeMemory(device, staging_buffer.memory, context.GetAllocator());
        vkDestroyBuffer(device, staging_buffer.buffer, context.GetAllocator());
        return false;
    }

    if (!CreateVulkanImage(
            physical_device,
            device,
            static_cast<std::uint32_t>(texture_asset.width),
            static_cast<std::uint32_t>(texture_asset.height),
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            texture.image,
            texture.memory,
            texture.view))
    {
        vkFreeMemory(device, staging_buffer.memory, context.GetAllocator());
        vkDestroyBuffer(device, staging_buffer.buffer, context.GetAllocator());
        return false;
    }

    const bool upload_succeeded = ExecuteImmediateCommands(device, command_pool, context.GetQueue(), [&](VkCommandBuffer command_buffer)
    {
        TransitionImageLayout(
            command_buffer,
            texture.image,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            VK_ACCESS_TRANSFER_WRITE_BIT);

        VkBufferImageCopy copy_region = {};
        copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.imageSubresource.layerCount = 1;
        copy_region.imageExtent.width = static_cast<std::uint32_t>(texture_asset.width);
        copy_region.imageExtent.height = static_cast<std::uint32_t>(texture_asset.height);
        copy_region.imageExtent.depth = 1;

        vkCmdCopyBufferToImage(
            command_buffer,
            staging_buffer.buffer,
            texture.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copy_region);

        TransitionImageLayout(
            command_buffer,
            texture.image,
            VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    });

    vkFreeMemory(device, staging_buffer.memory, context.GetAllocator());
    vkDestroyBuffer(device, staging_buffer.buffer, context.GetAllocator());
    if (!upload_succeeded)
    {
        vkDestroyImageView(device, texture.view, context.GetAllocator());
        vkDestroyImage(device, texture.image, context.GetAllocator());
        vkFreeMemory(device, texture.memory, context.GetAllocator());
        texture = {};
        return false;
    }

    if (!AllocateMaterialDescriptorSet(device, context.GetDescriptorPool(), descriptor_set_layout, sampler, texture.view, texture.descriptor_set))
    {
        vkDestroyImageView(device, texture.view, context.GetAllocator());
        vkDestroyImage(device, texture.image, context.GetAllocator());
        vkFreeMemory(device, texture.memory, context.GetAllocator());
        texture = {};
        return false;
    }

    return true;
}

bool UpdateSceneObjectTransform(
    const EngineState& state,
    const SceneViewportRenderer::QueuedSceneObject& object,
    const SceneVector3& position,
    const SceneVector3& rotation,
    const SceneVector3& scale)
{
    if (state.active_scene_path.empty())
    {
        return false;
    }

    bool changed = false;
    changed = SetSceneObjectPosition(state.active_scene_path, object.name, position) || changed;
    changed = SetSceneObjectRotation(state.active_scene_path, object.name, rotation) || changed;
    changed = SetSceneObjectScale(state.active_scene_path, object.name, scale) || changed;
    return changed;
}

void AppendGridQuad(
    std::vector<SceneGpuVertex>& vertices,
    std::vector<std::uint32_t>& indices,
    float min_x,
    float max_x,
    float min_z,
    float max_z,
    float y,
    float r,
    float g,
    float b,
    float a)
{
    const std::uint32_t base_index = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back(BuildColoredVertex(min_x, y, min_z, 0.0f, 1.0f, 0.0f, r, g, b, a));
    vertices.push_back(BuildColoredVertex(max_x, y, min_z, 0.0f, 1.0f, 0.0f, r, g, b, a));
    vertices.push_back(BuildColoredVertex(max_x, y, max_z, 0.0f, 1.0f, 0.0f, r, g, b, a));
    vertices.push_back(BuildColoredVertex(min_x, y, max_z, 0.0f, 1.0f, 0.0f, r, g, b, a));

    indices.push_back(base_index + 0);
    indices.push_back(base_index + 1);
    indices.push_back(base_index + 2);
    indices.push_back(base_index + 0);
    indices.push_back(base_index + 2);
    indices.push_back(base_index + 3);
}

void RecoverOrbitCameraFromView(
    const float* view_matrix,
    const Vec3& target,
    float scene_radius,
    SceneViewportCameraState& camera_state)
{
    const Vec3 side = {view_matrix[0], view_matrix[4], view_matrix[8]};
    const Vec3 up = {view_matrix[1], view_matrix[5], view_matrix[9]};
    const Vec3 forward = {-view_matrix[2], -view_matrix[6], -view_matrix[10]};
    const float tx = view_matrix[12];
    const float ty = view_matrix[13];
    const float tz = view_matrix[14];

    const Vec3 eye = Add(Add(Multiply(side, -tx), Multiply(up, -ty)), Multiply(forward, tz));
    const Vec3 from_target = Subtract(eye, target);
    const float distance = (std::max)(0.001f, Length(from_target));
    const Vec3 direction = Normalize(from_target);
    camera_state.yaw = std::atan2(direction.x, direction.z);
    camera_state.pitch = std::asin(std::clamp(direction.y, -1.0f, 1.0f));

    const float base_distance = ((scene_radius / std::tan(DegreesToRadians(55.0f) * 0.5f)) + scene_radius * 1.2f);
    camera_state.zoom = std::clamp(base_distance / distance, 0.25f, 3.5f);
}

void SetOrbitCameraDirection(const Vec3& orbit_direction, SceneViewportCameraState& camera_state)
{
    const Vec3 direction = Normalize(orbit_direction);
    camera_state.yaw = std::atan2(direction.x, direction.z);
    camera_state.pitch = std::asin(std::clamp(direction.y, -0.999f, 0.999f));
}

Vec3 GetOrbitCameraDirection(const SceneViewportCameraState& camera_state)
{
    return Normalize(Vec3{
        std::cos(camera_state.pitch) * std::sin(camera_state.yaw),
        std::sin(camera_state.pitch),
        std::cos(camera_state.pitch) * std::cos(camera_state.yaw)});
}

bool IntersectRayAabb(const Vec3& origin, const Vec3& direction, const Vec3& bounds_min, const Vec3& bounds_max, float& distance)
{
    float t_min = 0.0f;
    float t_max = std::numeric_limits<float>::max();

    const float origin_components[3] = {origin.x, origin.y, origin.z};
    const float direction_components[3] = {direction.x, direction.y, direction.z};
    const float min_components[3] = {bounds_min.x, bounds_min.y, bounds_min.z};
    const float max_components[3] = {bounds_max.x, bounds_max.y, bounds_max.z};

    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(direction_components[axis]) <= 0.00001f)
        {
            if (origin_components[axis] < min_components[axis] || origin_components[axis] > max_components[axis])
            {
                return false;
            }
            continue;
        }

        const float inv_direction = 1.0f / direction_components[axis];
        float t0 = (min_components[axis] - origin_components[axis]) * inv_direction;
        float t1 = (max_components[axis] - origin_components[axis]) * inv_direction;
        if (t0 > t1)
        {
            std::swap(t0, t1);
        }

        t_min = (std::max)(t_min, t0);
        t_max = (std::min)(t_max, t1);
        if (t_min > t_max)
        {
            return false;
        }
    }

    distance = t_min;
    return true;
}

int PickSceneObject(
    const std::vector<SceneViewportRenderer::QueuedSceneObject>& objects,
    const ImVec2& mouse_position,
    const ImVec2& viewport_min,
    const ImVec2& viewport_max,
    const Vec3& camera_position,
    const Vec3& camera_target)
{
    const float viewport_width = viewport_max.x - viewport_min.x;
    const float viewport_height = viewport_max.y - viewport_min.y;
    if (viewport_width <= 1.0f || viewport_height <= 1.0f)
    {
        return -1;
    }

    const float normalized_x = ((mouse_position.x - viewport_min.x) / viewport_width) * 2.0f - 1.0f;
    const float normalized_y = 1.0f - ((mouse_position.y - viewport_min.y) / viewport_height) * 2.0f;
    const float aspect = viewport_width / viewport_height;
    const float tan_half_fov = std::tan(DegreesToRadians(55.0f) * 0.5f);

    const Vec3 forward = Normalize(Subtract(camera_target, camera_position));
    Vec3 right = Normalize(Cross(forward, Vec3{0.0f, 1.0f, 0.0f}));
    if (Length(right) <= 0.0001f)
    {
        right = Vec3{1.0f, 0.0f, 0.0f};
    }
    const Vec3 up = Normalize(Cross(right, forward));

    const Vec3 ray_direction = Normalize(Add(
        Add(forward, Multiply(right, normalized_x * aspect * tan_half_fov)),
        Multiply(up, normalized_y * tan_half_fov)));

    int best_index = -1;
    float best_distance = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < objects.size(); ++index)
    {
        const auto& object = objects[index];
        if (!object.has_bounds)
        {
            continue;
        }

        float hit_distance = 0.0f;
        if (!IntersectRayAabb(camera_position, ray_direction, ToVec3(object.bounds_min), ToVec3(object.bounds_max), hit_distance))
        {
            continue;
        }

        if (hit_distance < best_distance)
        {
            best_distance = hit_distance;
            best_index = static_cast<int>(index);
        }
    }

    return best_index;
}

std::uint32_t FindMemoryType(VkPhysicalDevice physical_device, std::uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index)
    {
        const bool type_matches = (type_filter & (1u << index)) != 0;
        const bool properties_match = (memory_properties.memoryTypes[index].propertyFlags & properties) == properties;
        if (type_matches && properties_match)
        {
            return index;
        }
    }

    return UINT32_MAX;
}

bool CreateVulkanImage(
    VkPhysicalDevice physical_device,
    VkDevice device,
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

    VkResult result = vkCreateImage(device, &image_info, nullptr, &image);
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
    allocation_info.memoryTypeIndex = FindMemoryType(physical_device, memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation_info.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    result = vkAllocateMemory(device, &allocation_info, nullptr, &memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindImageMemory(device, image, memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
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

    result = vkCreateImageView(device, &view_info, nullptr, &image_view);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, memory, nullptr);
        vkDestroyImage(device, image, nullptr);
        memory = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

struct AxisViewFlipResult
{
    bool changed = false;
    bool hovered = false;
};

bool DrawTransformModeToolbar(const ImVec2& viewport_min, std::uint32_t& gizmo_operation, bool& gizmo_local_mode)
{
    const float button_height = 24.0f;
    bool hovered = false;

    ImGui::SetCursorScreenPos(ImVec2(viewport_min.x + 12.0f, viewport_min.y + 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));

    auto draw_mode_button = [&](const char* label, std::uint32_t value)
    {
        const bool selected = gizmo_operation == value;
        if (selected)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(70, 86, 106, 235));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(82, 100, 122, 235));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(92, 112, 136, 235));
        }

        if (ImGui::Button(label, ImVec2(0.0f, button_height)))
        {
            gizmo_operation = value;
        }
        hovered = hovered || ImGui::IsItemHovered();

        if (selected)
        {
            ImGui::PopStyleColor(3);
        }
    };

    draw_mode_button("Move", 0);
    ImGui::SameLine();
    draw_mode_button("Rotate", 1);
    ImGui::SameLine();
    draw_mode_button("Scale", 2);
    ImGui::SameLine();
    ImGui::Checkbox("Local", &gizmo_local_mode);
    hovered = hovered || ImGui::IsItemHovered();

    ImGui::PopStyleVar(2);
    return hovered;
}

AxisViewFlipResult DrawAxisViewFlipControl(const ImVec2& viewport_min, const ImVec2& viewport_max, SceneViewportCameraState& camera_state)
{
    const float button_width = 28.0f;
    const float button_height = 24.0f;
    const float spacing = 6.0f;
    const float panel_width = button_width * 3.0f + spacing * 2.0f;

    ImGui::SetCursorScreenPos(ImVec2(viewport_max.x - panel_width - 12.0f, viewport_min.y + 12.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 4.0f));

    AxisViewFlipResult result;
    const Vec3 current_direction = GetOrbitCameraDirection(camera_state);

    auto draw_axis_button = [&](const char* id, const char* label, const ImVec4& color, const Vec3& positive_axis)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x + 0.08f, color.y + 0.08f, color.z + 0.08f, color.w));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x + 0.14f, color.y + 0.14f, color.z + 0.14f, color.w));
        if (ImGui::Button(id, ImVec2(button_width, button_height)))
        {
            const float alignment = Dot(current_direction, positive_axis);
            const Vec3 flipped_direction = alignment >= 0.0f ? Multiply(positive_axis, -1.0f) : positive_axis;
            SetOrbitCameraDirection(flipped_direction, camera_state);
            result.changed = true;
        }
        result.hovered = result.hovered || ImGui::IsItemHovered();
        const ImVec2 text_size = ImGui::CalcTextSize(label);
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(min.x + (max.x - min.x - text_size.x) * 0.5f, min.y + (max.y - min.y - text_size.y) * 0.5f),
            IM_COL32(245, 247, 250, 255),
            label);
        ImGui::PopStyleColor(3);
    };

    draw_axis_button("##FlipX", "X", ImVec4(0.55f, 0.20f, 0.20f, 0.92f), Vec3{1.0f, 0.0f, 0.0f});
    ImGui::SameLine(0.0f, spacing);
    draw_axis_button("##FlipY", "Y", ImVec4(0.20f, 0.48f, 0.22f, 0.92f), Vec3{0.0f, 1.0f, 0.0f});
    ImGui::SameLine(0.0f, spacing);
    draw_axis_button("##FlipZ", "Z", ImVec4(0.20f, 0.33f, 0.60f, 0.92f), Vec3{0.0f, 0.0f, 1.0f});

    ImGui::PopStyleVar(2);
    return result;
}
}

bool SceneViewportRenderer::Initialize(VulkanContext* context)
{
    vulkan_context_ = context;
    return EnsurePipeline();
}

void SceneViewportRenderer::ReleaseBuffer(GpuBuffer& buffer)
{
    if (vulkan_context_ == nullptr)
    {
        buffer = {};
        return;
    }

    VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();
    if (buffer.buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(device, buffer.buffer, allocator);
    }
    if (buffer.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, buffer.memory, allocator);
    }
    buffer = {};
}

void SceneViewportRenderer::ReleaseTexture(GpuTexture& texture)
{
    if (vulkan_context_ == nullptr)
    {
        texture = {};
        return;
    }

    VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();
    if (texture.descriptor_set != VK_NULL_HANDLE)
    {
        vkFreeDescriptorSets(device, vulkan_context_->GetDescriptorPool(), 1, &texture.descriptor_set);
        texture.descriptor_set = VK_NULL_HANDLE;
    }
    if (texture.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, texture.view, allocator);
        texture.view = VK_NULL_HANDLE;
    }
    if (texture.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, texture.image, allocator);
        texture.image = VK_NULL_HANDLE;
    }
    if (texture.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, texture.memory, allocator);
        texture.memory = VK_NULL_HANDLE;
    }
}

void SceneViewportRenderer::ReleaseMeshCacheEntry(GpuMeshCacheEntry& entry)
{
    ReleaseBuffer(entry.vertex_buffer);
    ReleaseBuffer(entry.index_buffer);
    for (GpuTexture& material_texture : entry.material_textures)
    {
        ReleaseTexture(material_texture);
    }

    entry.sections.clear();
    entry.material_textures.clear();
}

void SceneViewportRenderer::ReleaseGridCacheEntry()
{
    ReleaseBuffer(grid_cache_.vertex_buffer);
    ReleaseBuffer(grid_cache_.index_buffer);

    grid_cache_.index_count = 0;
    grid_cache_.spacing = 0.0f;
    grid_cache_.extent = 0.0f;
    grid_cache_.origin_x = 0.0f;
    grid_cache_.origin_z = 0.0f;
}

void SceneViewportRenderer::DestroyRenderTargets()
{
    target_width_ = 0;
    target_height_ = 0;
    offscreen_color_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    offscreen_depth_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    if (offscreen_color_descriptor_set_ != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkan_RemoveTexture(offscreen_color_descriptor_set_);
        offscreen_color_descriptor_set_ = VK_NULL_HANDLE;
    }

    if (vulkan_context_ != nullptr)
    {
        VkDevice vk_device = vulkan_context_->GetDevice();
        if (offscreen_framebuffer_ != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(vk_device, offscreen_framebuffer_, vulkan_context_->GetAllocator());
            offscreen_framebuffer_ = VK_NULL_HANDLE;
        }
        if (offscreen_depth_view_ != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk_device, offscreen_depth_view_, vulkan_context_->GetAllocator());
            offscreen_depth_view_ = VK_NULL_HANDLE;
        }
        if (offscreen_depth_image_ != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk_device, offscreen_depth_image_, vulkan_context_->GetAllocator());
            offscreen_depth_image_ = VK_NULL_HANDLE;
        }
        if (offscreen_depth_memory_ != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk_device, offscreen_depth_memory_, vulkan_context_->GetAllocator());
            offscreen_depth_memory_ = VK_NULL_HANDLE;
        }
        if (offscreen_color_view_ != VK_NULL_HANDLE)
        {
            vkDestroyImageView(vk_device, offscreen_color_view_, vulkan_context_->GetAllocator());
            offscreen_color_view_ = VK_NULL_HANDLE;
        }
        if (offscreen_color_image_ != VK_NULL_HANDLE)
        {
            vkDestroyImage(vk_device, offscreen_color_image_, vulkan_context_->GetAllocator());
            offscreen_color_image_ = VK_NULL_HANDLE;
        }
        if (offscreen_color_memory_ != VK_NULL_HANDLE)
        {
            vkFreeMemory(vk_device, offscreen_color_memory_, vulkan_context_->GetAllocator());
            offscreen_color_memory_ = VK_NULL_HANDLE;
        }
    }
}

void SceneViewportRenderer::Shutdown()
{
    DestroyRenderTargets();

    for (auto& mesh_entry : mesh_cache_)
    {
        ReleaseMeshCacheEntry(mesh_entry.second);
    }
    mesh_cache_.clear();
    ReleaseGridCacheEntry();

    ReleaseTexture(fallback_texture_);

    if (vulkan_context_ != nullptr)
    {
        VkDevice vk_device = vulkan_context_->GetDevice();
        if (scene_pipeline_ != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(vk_device, scene_pipeline_, vulkan_context_->GetAllocator());
            scene_pipeline_ = VK_NULL_HANDLE;
        }
        if (scene_pipeline_layout_ != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(vk_device, scene_pipeline_layout_, vulkan_context_->GetAllocator());
            scene_pipeline_layout_ = VK_NULL_HANDLE;
        }
        if (material_descriptor_set_layout_ != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(vk_device, material_descriptor_set_layout_, vulkan_context_->GetAllocator());
            material_descriptor_set_layout_ = VK_NULL_HANDLE;
        }
        if (material_sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(vk_device, material_sampler_, vulkan_context_->GetAllocator());
            material_sampler_ = VK_NULL_HANDLE;
        }
        if (offscreen_color_sampler_ != VK_NULL_HANDLE)
        {
            vkDestroySampler(vk_device, offscreen_color_sampler_, vulkan_context_->GetAllocator());
            offscreen_color_sampler_ = VK_NULL_HANDLE;
        }
        if (offscreen_render_pass_ != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(vk_device, offscreen_render_pass_, vulkan_context_->GetAllocator());
            offscreen_render_pass_ = VK_NULL_HANDLE;
        }
        if (offscreen_render_fence_ != VK_NULL_HANDLE)
        {
            vkDestroyFence(vk_device, offscreen_render_fence_, vulkan_context_->GetAllocator());
            offscreen_render_fence_ = VK_NULL_HANDLE;
        }
        if (offscreen_command_pool_ != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(vk_device, offscreen_command_pool_, vulkan_context_->GetAllocator());
            offscreen_command_pool_ = VK_NULL_HANDLE;
            offscreen_command_buffer_ = VK_NULL_HANDLE;
        }
    }

    queued_objects_.clear();
    render_requested_ = false;
    vulkan_context_ = nullptr;
}

void SceneViewportRenderer::BeginFrame()
{
    render_requested_ = false;
    queued_objects_.clear();
}

bool SceneViewportRenderer::EnsureMaterialResources()
{
    if (vulkan_context_ == nullptr || offscreen_command_pool_ == VK_NULL_HANDLE || material_descriptor_set_layout_ == VK_NULL_HANDLE)
    {
        return false;
    }

    if (material_sampler_ == VK_NULL_HANDLE)
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
        const VkResult result = vkCreateSampler(vulkan_context_->GetDevice(), &sampler_info, vulkan_context_->GetAllocator(), &material_sampler_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            return false;
        }
    }

    if (fallback_texture_.image == VK_NULL_HANDLE)
    {
        ModelTextureAsset fallback_texture_asset;
        fallback_texture_asset.valid = true;
        fallback_texture_asset.width = 1;
        fallback_texture_asset.height = 1;
        fallback_texture_asset.pixels = {255, 255, 255, 255};
        if (!CreateTextureFromAsset(
                *vulkan_context_,
                offscreen_command_pool_,
                material_descriptor_set_layout_,
                material_sampler_,
                fallback_texture_asset,
                fallback_texture_))
        {
            return false;
        }
    }

    return true;
}

bool SceneViewportRenderer::EnsurePipeline()
{
    if (vulkan_context_ == nullptr)
    {
        return false;
    }

    if (offscreen_render_pass_ != VK_NULL_HANDLE &&
        offscreen_command_pool_ != VK_NULL_HANDLE &&
        offscreen_render_fence_ != VK_NULL_HANDLE &&
        offscreen_color_sampler_ != VK_NULL_HANDLE &&
        material_descriptor_set_layout_ != VK_NULL_HANDLE &&
        scene_pipeline_layout_ != VK_NULL_HANDLE &&
        scene_pipeline_ != VK_NULL_HANDLE)
    {
        return true;
    }

    const VkDevice vk_device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    if (offscreen_render_pass_ == VK_NULL_HANDLE)
    {
        VkAttachmentDescription color_attachment = {};
        color_attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depth_attachment = {};
        depth_attachment.format = VK_FORMAT_D32_SFLOAT;
        depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_reference = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_reference;
        subpass.pDepthStencilAttachment = &depth_reference;

        std::array<VkAttachmentDescription, 2> attachments = {color_attachment, depth_attachment};
        VkRenderPassCreateInfo render_pass_info = {};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        render_pass_info.pAttachments = attachments.data();
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;

        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        render_pass_info.dependencyCount = 1;
        render_pass_info.pDependencies = &dependency;

        const VkResult result = vkCreateRenderPass(vk_device, &render_pass_info, allocator, &offscreen_render_pass_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            return false;
        }
    }

    if (material_descriptor_set_layout_ == VK_NULL_HANDLE)
    {
        VkDescriptorSetLayoutBinding texture_binding = {};
        texture_binding.binding = 0;
        texture_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texture_binding.descriptorCount = 1;
        texture_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layout_info = {};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &texture_binding;

        VkResult result = vkCreateDescriptorSetLayout(vk_device, &layout_info, allocator, &material_descriptor_set_layout_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            return false;
        }
    }

    if (scene_pipeline_layout_ == VK_NULL_HANDLE)
    {
        VkPushConstantRange push_constant_range = {};
        push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_constant_range.offset = 0;
        push_constant_range.size = sizeof(SceneUniformBlock);

        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &material_descriptor_set_layout_;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_constant_range;

        VkResult result = vkCreatePipelineLayout(vk_device, &layout_info, allocator, &scene_pipeline_layout_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            return false;
        }
    }

    if (scene_pipeline_ == VK_NULL_HANDLE)
    {
        VkShaderModule vertex_shader = LoadShaderModule(vk_device, ResolveShaderPath("scene_viewport.vert.spv"));
        VkShaderModule fragment_shader = LoadShaderModule(vk_device, ResolveShaderPath("scene_viewport.frag.spv"));
        if (vertex_shader == VK_NULL_HANDLE || fragment_shader == VK_NULL_HANDLE)
        {
            if (vertex_shader != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(vk_device, vertex_shader, allocator);
            }
            if (fragment_shader != VK_NULL_HANDLE)
            {
                vkDestroyShaderModule(vk_device, fragment_shader, allocator);
            }
            return false;
        }

        VkPipelineShaderStageCreateInfo shader_stages[2] = {};
        shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        shader_stages[0].module = vertex_shader;
        shader_stages[0].pName = "main";
        shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        shader_stages[1].module = fragment_shader;
        shader_stages[1].pName = "main";

        VkVertexInputBindingDescription binding_description = {};
        binding_description.binding = 0;
        binding_description.stride = sizeof(SceneGpuVertex);
        binding_description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 4> attributes = {};
        attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(SceneGpuVertex, position))};
        attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(SceneGpuVertex, normal))};
        attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(SceneGpuVertex, uv))};
        attributes[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<std::uint32_t>(offsetof(SceneGpuVertex, color))};

        VkPipelineVertexInputStateCreateInfo vertex_input = {};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding_description;
        vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo input_assembly = {};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport_state = {};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization = {};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.lineWidth = 1.0f;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo multisample = {};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth_stencil = {};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = VK_TRUE;
        depth_stencil.depthWriteEnable = VK_TRUE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

        VkPipelineColorBlendAttachmentState color_blend_attachment = {};
        color_blend_attachment.blendEnable = VK_TRUE;
        color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color_blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color_blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        color_blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo color_blend = {};
        color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        color_blend.attachmentCount = 1;
        color_blend.pAttachments = &color_blend_attachment;

        std::array<VkDynamicState, 2> dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic_state = {};
        dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_state.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic_state.pDynamicStates = dynamic_states.data();

        VkGraphicsPipelineCreateInfo pipeline_info = {};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = shader_stages;
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth_stencil;
        pipeline_info.pColorBlendState = &color_blend;
        pipeline_info.pDynamicState = &dynamic_state;
        pipeline_info.layout = scene_pipeline_layout_;
        pipeline_info.renderPass = offscreen_render_pass_;
        pipeline_info.subpass = 0;

        VkResult result = vkCreateGraphicsPipelines(vk_device, vulkan_context_->GetPipelineCache(), 1, &pipeline_info, allocator, &scene_pipeline_);
        VulkanContext::CheckVkResult(result);
        vkDestroyShaderModule(vk_device, vertex_shader, allocator);
        vkDestroyShaderModule(vk_device, fragment_shader, allocator);
        if (result != VK_SUCCESS)
        {
            return false;
        }
    }

    if (offscreen_color_sampler_ == VK_NULL_HANDLE)
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
        const VkResult result = vkCreateSampler(vk_device, &sampler_info, allocator, &offscreen_color_sampler_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            return false;
        }
    }

    if (offscreen_command_pool_ == VK_NULL_HANDLE)
    {
        VkCommandPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = vulkan_context_->GetQueueFamily();
        VkResult result = vkCreateCommandPool(vk_device, &pool_info, allocator, &offscreen_command_pool_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            return false;
        }

        VkCommandBufferAllocateInfo allocate_info = {};
        allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool = offscreen_command_pool_;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        result = vkAllocateCommandBuffers(vk_device, &allocate_info, &offscreen_command_buffer_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            return false;
        }
    }

    if (offscreen_render_fence_ == VK_NULL_HANDLE)
    {
        VkFenceCreateInfo fence_info = {};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        const VkResult result = vkCreateFence(vk_device, &fence_info, allocator, &offscreen_render_fence_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            return false;
        }
    }

    return true;
}

bool SceneViewportRenderer::EnsureGridCacheEntry()
{
    if (!grid_enabled_ || vulkan_context_ == nullptr || grid_spacing_ <= 0.0f)
    {
        ReleaseGridCacheEntry();
        return false;
    }

    if (grid_cache_.vertex_buffer.buffer != VK_NULL_HANDLE &&
        grid_cache_.index_buffer.buffer != VK_NULL_HANDLE &&
        std::abs(grid_cache_.spacing - grid_spacing_) < 0.0001f &&
        std::abs(grid_cache_.extent - grid_extent_) < 0.0001f &&
        std::abs(grid_cache_.origin_x - grid_origin_x_) < 0.0001f &&
        std::abs(grid_cache_.origin_z - grid_origin_z_) < 0.0001f)
    {
        return true;
    }

    ReleaseGridCacheEntry();

    const float major_spacing = grid_spacing_;
    const float minor_spacing = (std::max)(0.0625f, major_spacing * 0.25f);
    const int half_steps = (std::max)(1, static_cast<int>(std::ceil(grid_extent_ / minor_spacing)));
    const int subdivisions = (std::max)(1, static_cast<int>(std::round(major_spacing / minor_spacing)));
    const float minor_thickness = (std::max)(0.004f, minor_spacing * 0.045f);
    const float major_thickness = (std::max)(minor_thickness * 1.9f, major_spacing * 0.016f);
    const float y = 0.0f;

    std::vector<SceneGpuVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(static_cast<std::size_t>((half_steps * 2 + 2) * 8));
    indices.reserve(static_cast<std::size_t>((half_steps * 2 + 2) * 12));

    for (int step = -half_steps; step <= half_steps; ++step)
    {
        const float offset = static_cast<float>(step) * minor_spacing;
        const bool major_line = (step % subdivisions) == 0;
        const float thickness = major_line ? major_thickness : minor_thickness;
        const float shade = major_line ? 0.32f : 0.16f;

        const float x = grid_origin_x_ + offset;
        AppendGridQuad(
            vertices,
            indices,
            x - thickness,
            x + thickness,
            grid_origin_z_ - grid_extent_,
            grid_origin_z_ + grid_extent_,
            y,
            shade,
            shade,
            shade,
            1.0f);

        const float z = grid_origin_z_ + offset;
        AppendGridQuad(
            vertices,
            indices,
            grid_origin_x_ - grid_extent_,
            grid_origin_x_ + grid_extent_,
            z - thickness,
            z + thickness,
            y,
            shade,
            shade,
            shade,
            1.0f);
    }

    if (vertices.empty() || indices.empty())
    {
        return false;
    }

    const VkDevice device = vulkan_context_->GetDevice();
    const VkPhysicalDevice physical_device = vulkan_context_->GetPhysicalDevice();
    const VkDeviceSize vertex_buffer_size = static_cast<VkDeviceSize>(vertices.size() * sizeof(SceneGpuVertex));
    const VkDeviceSize index_buffer_size = static_cast<VkDeviceSize>(indices.size() * sizeof(std::uint32_t));

    if (!CreateVulkanBuffer(
            physical_device,
            device,
            vertex_buffer_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            grid_cache_.vertex_buffer))
    {
        ReleaseGridCacheEntry();
        return false;
    }

    if (!CreateVulkanBuffer(
            physical_device,
            device,
            index_buffer_size,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            grid_cache_.index_buffer))
    {
        ReleaseGridCacheEntry();
        return false;
    }

    if (!UploadBufferData(device, grid_cache_.vertex_buffer, vertices.data(), vertices.size() * sizeof(SceneGpuVertex)) ||
        !UploadBufferData(device, grid_cache_.index_buffer, indices.data(), indices.size() * sizeof(std::uint32_t)))
    {
        ReleaseGridCacheEntry();
        return false;
    }

    grid_cache_.index_count = static_cast<std::uint32_t>(indices.size());
    grid_cache_.spacing = grid_spacing_;
    grid_cache_.extent = grid_extent_;
    grid_cache_.origin_x = grid_origin_x_;
    grid_cache_.origin_z = grid_origin_z_;
    return true;
}

bool SceneViewportRenderer::EnsureRenderTargets(std::uint32_t width, std::uint32_t height)
{
    if (offscreen_color_image_ != VK_NULL_HANDLE && offscreen_depth_image_ != VK_NULL_HANDLE && offscreen_framebuffer_ != VK_NULL_HANDLE && target_width_ == width && target_height_ == height)
    {
        return true;
    }

    DestroyRenderTargets();

    if (vulkan_context_ == nullptr)
    {
        return false;
    }

    const VkDevice vk_device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    if (!CreateVulkanImage(
            vulkan_context_->GetPhysicalDevice(),
            vk_device,
            width,
            height,
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            offscreen_color_image_,
            offscreen_color_memory_,
            offscreen_color_view_))
    {
        return false;
    }

    if (!CreateVulkanImage(
            vulkan_context_->GetPhysicalDevice(),
            vk_device,
            width,
            height,
            VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            offscreen_depth_image_,
            offscreen_depth_memory_,
            offscreen_depth_view_))
    {
        DestroyRenderTargets();
        return false;
    }

    std::array<VkImageView, 2> attachments = {offscreen_color_view_, offscreen_depth_view_};
    VkFramebufferCreateInfo framebuffer_info = {};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = offscreen_render_pass_;
    framebuffer_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    framebuffer_info.pAttachments = attachments.data();
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = 1;
    VkResult result = vkCreateFramebuffer(vk_device, &framebuffer_info, allocator, &offscreen_framebuffer_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        DestroyRenderTargets();
        return false;
    }

    offscreen_color_descriptor_set_ = ImGui_ImplVulkan_AddTexture(offscreen_color_sampler_, offscreen_color_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (offscreen_color_descriptor_set_ == VK_NULL_HANDLE)
    {
        DestroyRenderTargets();
        return false;
    }

    target_width_ = width;
    target_height_ = height;
    offscreen_color_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    offscreen_depth_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

bool SceneViewportRenderer::EnsureMeshCacheEntry(const std::filesystem::path& model_path, const SceneViewportResolvedModel& resolved_model)
{
    if (vulkan_context_ == nullptr || resolved_model.asset == nullptr || !resolved_model.asset->loaded)
    {
        return false;
    }

    GpuMeshCacheEntry& cache_entry = mesh_cache_[model_path];
    if (cache_entry.vertex_buffer.buffer != VK_NULL_HANDLE && cache_entry.index_buffer.buffer != VK_NULL_HANDLE && cache_entry.write_time == resolved_model.write_time)
    {
        return true;
    }

    ReleaseMeshCacheEntry(cache_entry);

    std::vector<SceneGpuVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(4096);
    indices.reserve(8192);

    cache_entry.material_textures.resize(resolved_model.asset->materials.size());
    for (std::size_t material_index = 0; material_index < resolved_model.asset->materials.size(); ++material_index)
    {
        const ModelMaterialAsset& material = resolved_model.asset->materials[material_index];
        if (material.base_color_texture.valid)
        {
            CreateTextureFromAsset(
                *vulkan_context_,
                offscreen_command_pool_,
                material_descriptor_set_layout_,
                material_sampler_,
                material.base_color_texture,
                cache_entry.material_textures[material_index]);
        }
    }

    for (const ModelMeshAsset& mesh : resolved_model.asset->meshes)
    {
        GpuMeshSection section;
        section.first_index = static_cast<std::uint32_t>(indices.size());
        section.material_index = mesh.material_index;
        const std::uint32_t base_vertex = static_cast<std::uint32_t>(vertices.size());
        const ModelMaterialAsset* material = mesh.material_index < resolved_model.asset->materials.size() ? &resolved_model.asset->materials[mesh.material_index] : nullptr;

        for (const ModelVertex& vertex : mesh.vertices)
        {
            vertices.push_back(BuildSceneGpuVertex(vertex, material));
        }
        for (std::uint32_t index : mesh.indices)
        {
            indices.push_back(base_vertex + index);
        }

        section.index_count = static_cast<std::uint32_t>(indices.size()) - section.first_index;
        if (section.index_count > 0)
        {
            cache_entry.sections.push_back(section);
        }
    }

    if (vertices.empty() || indices.empty() || cache_entry.sections.empty())
    {
        cache_entry.sections.clear();
        return false;
    }

    const VkDevice device = vulkan_context_->GetDevice();
    const VkPhysicalDevice physical_device = vulkan_context_->GetPhysicalDevice();
    const VkDeviceSize vertex_buffer_size = static_cast<VkDeviceSize>(vertices.size() * sizeof(SceneGpuVertex));
    const VkDeviceSize index_buffer_size = static_cast<VkDeviceSize>(indices.size() * sizeof(std::uint32_t));

    if (!CreateVulkanBuffer(
            physical_device,
            device,
            vertex_buffer_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            cache_entry.vertex_buffer))
    {
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    if (!CreateVulkanBuffer(
            physical_device,
            device,
            index_buffer_size,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            cache_entry.index_buffer))
    {
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    if (!UploadBufferData(device, cache_entry.vertex_buffer, vertices.data(), vertices.size() * sizeof(SceneGpuVertex)) ||
        !UploadBufferData(device, cache_entry.index_buffer, indices.data(), indices.size() * sizeof(std::uint32_t)))
    {
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    cache_entry.write_time = resolved_model.write_time;
    return true;
}

void SceneViewportRenderer::RenderUi(
    EngineState& state,
    const SceneMetadata& scene_metadata,
    const SceneViewportModelResolver& resolve_model_asset,
    SceneViewportCameraState& camera_state)
{
    ImGui::TextUnformatted("Scene Viewport");
    ImGui::SameLine();
    ImGui::TextDisabled("Right-drag orbit, wheel zoom");
    ImGui::Separator();

    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x <= 4.0f || available.y <= 4.0f)
    {
        return;
    }

    ImGui::BeginChild("##SceneViewportCanvas", available, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const bool viewport_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = ImVec2(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
    const float viewport_width = max.x - min.x;
    const float viewport_height = max.y - min.y;

    if (viewport_hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
    {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        camera_state.yaw += delta.x * 0.01f;
        camera_state.pitch = std::clamp(camera_state.pitch - delta.y * 0.01f, -1.2f, 1.2f);
    }
    if (viewport_hovered && ImGui::GetIO().MouseWheel != 0.0f)
    {
        camera_state.zoom = std::clamp(camera_state.zoom + ImGui::GetIO().MouseWheel * 0.1f, 0.25f, 3.5f);
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(min, max, IM_COL32(24, 28, 32, 255), 8.0f);

    const ImVec2 framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
    const std::uint32_t target_width = static_cast<std::uint32_t>((std::max)(1.0f, std::round(available.x * framebuffer_scale.x)));
    const std::uint32_t target_height = static_cast<std::uint32_t>((std::max)(1.0f, std::round(available.y * framebuffer_scale.y)));

    if (!EnsurePipeline() || !EnsureMaterialResources() || !EnsureRenderTargets(target_width, target_height))
    {
        draw_list->AddRect(min, max, IM_COL32(92, 99, 110, 255), 8.0f, 0, 1.5f);
        draw_list->AddText(ImVec2(min.x + 12.0f, min.y + 12.0f), IM_COL32(235, 238, 242, 255), "Scene GPU renderer unavailable");
        ImGui::EndChild();
        return;
    }

    if (offscreen_color_descriptor_set_ != VK_NULL_HANDLE)
    {
        draw_list->AddImage(
            reinterpret_cast<ImTextureID>(offscreen_color_descriptor_set_),
            min,
            max,
            ImVec2(0.0f, 1.0f),
            ImVec2(1.0f, 0.0f));
    }

    draw_list->AddRect(min, max, IM_COL32(92, 99, 110, 255), 8.0f, 0, 1.5f);

    Vec3 world_min = Vec3{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max()};
    Vec3 world_max = Vec3{
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
    const bool has_selected_scene_object = state.selected_item_path == state.active_scene_path && !state.selected_scene_object_name.empty();
    const SceneObjectMetadata* selected_scene_object_metadata = nullptr;
    if (has_selected_scene_object)
    {
        const auto selected_object_it = std::find_if(scene_metadata.objects.begin(), scene_metadata.objects.end(), [&](const SceneObjectMetadata& object)
        {
            return object.name == state.selected_scene_object_name;
        });
        if (selected_object_it != scene_metadata.objects.end())
        {
            selected_scene_object_metadata = &(*selected_object_it);
        }
    }
    else if (scene_metadata.objects.size() == 1)
    {
        selected_scene_object_metadata = &scene_metadata.objects.front();
    }

    queued_objects_.clear();
    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        if (object.model_path.empty())
        {
            continue;
        }

        const std::filesystem::path model_path = state.project_root / object.model_path;
        const SceneViewportResolvedModel resolved_model = resolve_model_asset(model_path);
        if (resolved_model.asset == nullptr || !resolved_model.asset->loaded)
        {
            continue;
        }
        if (!EnsureMeshCacheEntry(model_path, resolved_model))
        {
            continue;
        }
        QueuedSceneObject queued_object;
        queued_object.model_path = model_path;
        queued_object.name = object.name;
        queued_object.position = object.position;
        queued_object.rotation = object.rotation;
        queued_object.scale = object.scale;
        queued_object.selected = state.selected_item_path == state.active_scene_path && state.selected_scene_object_name == object.name;
        Vec3 object_bounds_min;
        Vec3 object_bounds_max;
        queued_object.has_bounds = ComputeObjectBounds(queued_object, *resolved_model.asset, object_bounds_min, object_bounds_max);
        queued_object.bounds_min = ToSceneVector3(object_bounds_min);
        queued_object.bounds_max = ToSceneVector3(object_bounds_max);
        queued_objects_.push_back(queued_object);

        ExpandBoundsWithObject(world_min, world_max, queued_object, *resolved_model.asset);
    }

    QueuedSceneObject fallback_gizmo_object;
    bool has_fallback_gizmo_object = false;
    if (selected_scene_object_metadata != nullptr)
    {
        fallback_gizmo_object.name = selected_scene_object_metadata->name;
        fallback_gizmo_object.position = selected_scene_object_metadata->position;
        fallback_gizmo_object.rotation = selected_scene_object_metadata->rotation;
        fallback_gizmo_object.scale = selected_scene_object_metadata->scale;
        fallback_gizmo_object.selected = selected_scene_object_metadata->name == state.selected_scene_object_name;
        has_fallback_gizmo_object = true;
    }

    if (queued_objects_.empty() && !has_fallback_gizmo_object)
    {
        const char* message = "No attached models found in the active scene";
        const ImVec2 message_size = ImGui::CalcTextSize(message);
        draw_list->AddText(ImVec2((min.x + max.x - message_size.x) * 0.5f, (min.y + max.y - message_size.y) * 0.5f), IM_COL32(235, 238, 242, 255), message);
        ImGui::EndChild();
        return;
    }

    Vec3 scene_center = {};
    float scene_radius = 1.5f;
    if (!queued_objects_.empty())
    {
        scene_center = Multiply(Add(world_min, world_max), 0.5f);
        scene_radius = (std::max)(0.75f, Length(Subtract(world_max, world_min)) * 0.6f);
    }
    else
    {
        scene_center = ToVec3(fallback_gizmo_object.position);
    }
    const Vec3 orbit_direction = Normalize(Vec3{
        std::cos(camera_state.pitch) * std::sin(camera_state.yaw),
        std::sin(camera_state.pitch),
        std::cos(camera_state.pitch) * std::cos(camera_state.yaw)});
    const float distance = ((scene_radius / std::tan(DegreesToRadians(55.0f) * 0.5f)) + scene_radius * 1.2f) / camera_state.zoom;
    const Vec3 camera_position = Add(scene_center, Multiply(orbit_direction, distance));

    float view_matrix[16];
    float projection_matrix[16];
    BuildLookAtMatrix(camera_position, scene_center, Vec3{0.0f, 1.0f, 0.0f}, view_matrix);
    BuildPerspectiveMatrix(55.0f, static_cast<float>(target_width) / static_cast<float>(target_height), 0.01f, 250.0f, projection_matrix);
    MultiplyMatrix(projection_matrix, view_matrix, view_projection_.data());

    ImGuizmo::BeginFrame();
    ImGuizmo::Enable(true);
    ImGuizmo::AllowAxisFlip(true);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(draw_list);
    ImGuizmo::SetRect(min.x, min.y, viewport_width, viewport_height);

    const bool transform_toolbar_hovered = DrawTransformModeToolbar(min, gizmo_operation_, gizmo_local_mode_);

    grid_enabled_ = state.show_grid_overlay;
    grid_spacing_ = (std::max)(0.001f, state.grid_size);
    grid_origin_x_ = SnapScalar(scene_center.x, grid_spacing_);
    grid_origin_z_ = SnapScalar(scene_center.z, grid_spacing_);
    grid_extent_ = (std::max)(grid_spacing_ * 24.0f, scene_radius * 4.0f);

    QueuedSceneObject* gizmo_object = nullptr;
    if (has_selected_scene_object)
    {
        const auto selected_object_it = std::find_if(queued_objects_.begin(), queued_objects_.end(), [&](const QueuedSceneObject& object)
        {
            return object.selected;
        });

        if (selected_object_it != queued_objects_.end())
        {
            gizmo_object = &(*selected_object_it);
        }
        else if (has_fallback_gizmo_object)
        {
            gizmo_object = &fallback_gizmo_object;
        }
    }
    else if (queued_objects_.size() == 1)
    {
        gizmo_object = &queued_objects_.front();
    }
    else if (has_fallback_gizmo_object)
    {
        gizmo_object = &fallback_gizmo_object;
    }

    if (gizmo_object != nullptr)
    {
        float gizmo_matrix[16];
        float delta_matrix[16];
        BuildModelMatrix(*gizmo_object, gizmo_matrix);
        SetIdentity(delta_matrix);

        ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
        if (gizmo_operation_ == 1)
        {
            operation = ImGuizmo::ROTATE;
        }
        else if (gizmo_operation_ == 2)
        {
            operation = ImGuizmo::SCALEU;
        }

        const ImGuizmo::MODE mode = (gizmo_operation_ == 2 || gizmo_local_mode_) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
        float snap_values[3] = {state.grid_size, state.grid_size, state.grid_size};
        float angle_snap = 15.0f;
        const float* snap = nullptr;
        if (state.snap_to_grid)
        {
            snap = operation == ImGuizmo::ROTATE ? &angle_snap : snap_values;
        }

        if (ImGuizmo::Manipulate(view_matrix, projection_matrix, operation, mode, gizmo_matrix, delta_matrix, snap))
        {
            float position[3] = {};
            float rotation[3] = {};
            float scale[3] = {};
            ImGuizmo::DecomposeMatrixToComponents(gizmo_matrix, position, rotation, scale);

            SceneVector3 new_position = {position[0], position[1], position[2]};
            SceneVector3 new_rotation = {rotation[0], rotation[1], rotation[2]};
            SceneVector3 new_scale = {scale[0], scale[1], scale[2]};

            if (state.snap_to_grid && operation == ImGuizmo::TRANSLATE)
            {
                SnapVector(new_position, state.grid_size);
            }

            gizmo_object->position = new_position;
            gizmo_object->rotation = new_rotation;
            gizmo_object->scale = new_scale;
            UpdateSceneObjectTransform(state, *gizmo_object, new_position, new_rotation, new_scale);
        }
    }

    const AxisViewFlipResult axis_view_result = DrawAxisViewFlipControl(min, max, camera_state);
    if (axis_view_result.changed)
    {
        const Vec3 flipped_orbit_direction = GetOrbitCameraDirection(camera_state);
        const Vec3 flipped_camera_position = Add(scene_center, Multiply(flipped_orbit_direction, distance));
        BuildLookAtMatrix(flipped_camera_position, scene_center, Vec3{0.0f, 1.0f, 0.0f}, view_matrix);
        MultiplyMatrix(projection_matrix, view_matrix, view_projection_.data());
    }

    if (viewport_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !transform_toolbar_hovered && !axis_view_result.hovered && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing())
    {
        const int picked_object_index = PickSceneObject(
            queued_objects_,
            ImGui::GetMousePos(),
            min,
            max,
            camera_position,
            scene_center);
        if (picked_object_index >= 0)
        {
            state.SetSelectedSceneObject(state.active_scene_path, queued_objects_[static_cast<std::size_t>(picked_object_index)].name);
            for (std::size_t object_index = 0; object_index < queued_objects_.size(); ++object_index)
            {
                queued_objects_[object_index].selected = static_cast<int>(object_index) == picked_object_index;
            }
        }
    }

    render_requested_ = true;

    const int viewport_fps = static_cast<int>(std::round(ImGui::GetIO().Framerate));
    const std::string footer = "Viewport " + std::to_string(viewport_fps) + " FPS";
    draw_list->AddText(ImVec2(min.x + 12.0f, max.y - 24.0f), IM_COL32(145, 152, 163, 255), footer.c_str());
    ImGui::EndChild();
}

void SceneViewportRenderer::RenderGpu()
{
    if (!render_requested_ ||
        vulkan_context_ == nullptr ||
        offscreen_command_buffer_ == VK_NULL_HANDLE ||
        offscreen_render_pass_ == VK_NULL_HANDLE ||
        offscreen_framebuffer_ == VK_NULL_HANDLE ||
        scene_pipeline_ == VK_NULL_HANDLE ||
        scene_pipeline_layout_ == VK_NULL_HANDLE ||
        fallback_texture_.descriptor_set == VK_NULL_HANDLE)
    {
        return;
    }

    EnsureGridCacheEntry();

    VkDevice vk_device = vulkan_context_->GetDevice();
    VkResult result = vkWaitForFences(vk_device, 1, &offscreen_render_fence_, VK_TRUE, UINT64_MAX);
    VulkanContext::CheckVkResult(result);
    result = vkResetFences(vk_device, 1, &offscreen_render_fence_);
    VulkanContext::CheckVkResult(result);
    result = vkResetCommandPool(vk_device, offscreen_command_pool_, 0);
    VulkanContext::CheckVkResult(result);

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(offscreen_command_buffer_, &begin_info);
    VulkanContext::CheckVkResult(result);

    TransitionImageLayout(
        offscreen_command_buffer_,
        offscreen_color_image_,
        VK_IMAGE_ASPECT_COLOR_BIT,
        offscreen_color_layout_,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        offscreen_color_layout_ == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        offscreen_color_layout_ == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    offscreen_color_layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    TransitionImageLayout(
        offscreen_command_buffer_,
        offscreen_depth_image_,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        offscreen_depth_layout_,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        offscreen_depth_layout_ == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
        0,
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    offscreen_depth_layout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    std::array<VkClearValue, 2> clear_values = {};
    clear_values[0].color.float32[0] = 0.08f;
    clear_values[0].color.float32[1] = 0.09f;
    clear_values[0].color.float32[2] = 0.11f;
    clear_values[0].color.float32[3] = 1.0f;
    clear_values[1].depthStencil.depth = 1.0f;

    VkRenderPassBeginInfo render_pass_info = {};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = offscreen_render_pass_;
    render_pass_info.framebuffer = offscreen_framebuffer_;
    render_pass_info.renderArea.extent.width = target_width_;
    render_pass_info.renderArea.extent.height = target_height_;
    render_pass_info.clearValueCount = static_cast<std::uint32_t>(clear_values.size());
    render_pass_info.pClearValues = clear_values.data();

    vkCmdBeginRenderPass(offscreen_command_buffer_, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(offscreen_command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_pipeline_);

    VkViewport viewport = {};
    viewport.width = static_cast<float>(target_width_);
    viewport.height = static_cast<float>(target_height_);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(offscreen_command_buffer_, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.extent.width = target_width_;
    scissor.extent.height = target_height_;
    vkCmdSetScissor(offscreen_command_buffer_, 0, 1, &scissor);

    const VkDeviceSize vertex_offset = 0;

    if (grid_enabled_ &&
        grid_cache_.index_count > 0 &&
        grid_cache_.vertex_buffer.buffer != VK_NULL_HANDLE &&
        grid_cache_.index_buffer.buffer != VK_NULL_HANDLE)
    {
        SceneUniformBlock grid_uniforms = {};
        SetIdentity(grid_uniforms.model);
        std::memcpy(grid_uniforms.model_view_projection, view_projection_.data(), sizeof(grid_uniforms.model_view_projection));

        vkCmdPushConstants(
            offscreen_command_buffer_,
            scene_pipeline_layout_,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(SceneUniformBlock),
            &grid_uniforms);
        vkCmdBindDescriptorSets(
            offscreen_command_buffer_,
            VK_PIPELINE_BIND_POINT_GRAPHICS,
            scene_pipeline_layout_,
            0,
            1,
            &fallback_texture_.descriptor_set,
            0,
            nullptr);
        vkCmdBindVertexBuffers(offscreen_command_buffer_, 0, 1, &grid_cache_.vertex_buffer.buffer, &vertex_offset);
        vkCmdBindIndexBuffer(offscreen_command_buffer_, grid_cache_.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(offscreen_command_buffer_, grid_cache_.index_count, 1, 0, 0, 0);
    }

    for (const QueuedSceneObject& object : queued_objects_)
    {
        const auto mesh_entry_it = mesh_cache_.find(object.model_path);
        if (mesh_entry_it == mesh_cache_.end())
        {
            continue;
        }

        const GpuMeshCacheEntry& mesh_entry = mesh_entry_it->second;
        if (mesh_entry.vertex_buffer.buffer == VK_NULL_HANDLE || mesh_entry.index_buffer.buffer == VK_NULL_HANDLE)
        {
            continue;
        }

        SceneUniformBlock scene_uniforms = {};
        BuildModelMatrix(object, scene_uniforms.model);
        MultiplyMatrix(view_projection_.data(), scene_uniforms.model, scene_uniforms.model_view_projection);
        vkCmdPushConstants(
            offscreen_command_buffer_,
            scene_pipeline_layout_,
            VK_SHADER_STAGE_VERTEX_BIT,
            0,
            sizeof(SceneUniformBlock),
            &scene_uniforms);

        vkCmdBindVertexBuffers(offscreen_command_buffer_, 0, 1, &mesh_entry.vertex_buffer.buffer, &vertex_offset);
        vkCmdBindIndexBuffer(offscreen_command_buffer_, mesh_entry.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        for (const GpuMeshSection& section : mesh_entry.sections)
        {
            VkDescriptorSet descriptor_set = fallback_texture_.descriptor_set;
            if (section.material_index < mesh_entry.material_textures.size())
            {
                const GpuTexture& material_texture = mesh_entry.material_textures[section.material_index];
                if (material_texture.descriptor_set != VK_NULL_HANDLE)
                {
                    descriptor_set = material_texture.descriptor_set;
                }
            }

            vkCmdBindDescriptorSets(
                offscreen_command_buffer_,
                VK_PIPELINE_BIND_POINT_GRAPHICS,
                scene_pipeline_layout_,
                0,
                1,
                &descriptor_set,
                0,
                nullptr);
            vkCmdDrawIndexed(offscreen_command_buffer_, section.index_count, 1, section.first_index, 0, 0);
        }
    }

    vkCmdEndRenderPass(offscreen_command_buffer_);

    TransitionImageLayout(
        offscreen_command_buffer_,
        offscreen_color_image_,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT);
    offscreen_color_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    result = vkEndCommandBuffer(offscreen_command_buffer_);
    VulkanContext::CheckVkResult(result);

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &offscreen_command_buffer_;
    result = vkQueueSubmit(vulkan_context_->GetQueue(), 1, &submit_info, offscreen_render_fence_);
    VulkanContext::CheckVkResult(result);
    result = vkWaitForFences(vk_device, 1, &offscreen_render_fence_, VK_TRUE, UINT64_MAX);
    VulkanContext::CheckVkResult(result);
}