#include "render/RuntimeRenderer.h"
#include "vfs/AssetVFS.h"

#include <SDL3/SDL.h>

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <unordered_set>

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
    float tangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
};

Vec3 Add(const Vec3& left, const Vec3& right)
{
    return Vec3{left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 Multiply(const Vec3& value, float scalar)
{
    return Vec3{value.x * scalar, value.y * scalar, value.z * scalar};
}

Vec3 Subtract(const Vec3& left, const Vec3& right)
{
    return Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
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

bool NearlyEqualFloat(float left, float right, float epsilon = 0.0001f)
{
    return std::abs(left - right) <= epsilon;
}

bool NearlyEqualVector3(const SceneVector3& left, const SceneVector3& right)
{
    return NearlyEqualFloat(left[0], right[0]) &&
        NearlyEqualFloat(left[1], right[1]) &&
        NearlyEqualFloat(left[2], right[2]);
}

bool PhysicsRelevantObjectDataMatches(const SceneObjectMetadata& left, const SceneObjectMetadata& right)
{
    return left.name == right.name &&
        left.parent_name == right.parent_name &&
        NearlyEqualVector3(left.position, right.position) &&
        NearlyEqualVector3(left.rotation, right.rotation) &&
        NearlyEqualVector3(left.scale, right.scale) &&
        left.model_path == right.model_path &&
        left.physics_shape == right.physics_shape &&
        left.physics_is_dynamic == right.physics_is_dynamic &&
        left.physics_is_trigger == right.physics_is_trigger &&
        left.physics_lock_rotation_x == right.physics_lock_rotation_x &&
        left.physics_lock_rotation_y == right.physics_lock_rotation_y &&
        left.physics_lock_rotation_z == right.physics_lock_rotation_z &&
        NearlyEqualFloat(left.physics_mass, right.physics_mass) &&
        NearlyEqualFloat(left.physics_friction, right.physics_friction) &&
        NearlyEqualFloat(left.physics_radius, right.physics_radius) &&
        NearlyEqualFloat(left.physics_capsule_half_height, right.physics_capsule_half_height) &&
        NearlyEqualVector3(left.physics_half_extent, right.physics_half_extent) &&
        NearlyEqualFloat(left.physics_linear_damping, right.physics_linear_damping) &&
        NearlyEqualFloat(left.physics_angular_damping, right.physics_angular_damping);
}

bool SceneChangeRequiresPhysicsReset(const SceneMetadata& previous, const SceneMetadata& current)
{
    if (previous.objects.size() != current.objects.size())
    {
        return true;
    }

    for (std::size_t index = 0; index < previous.objects.size(); ++index)
    {
        if (!PhysicsRelevantObjectDataMatches(previous.objects[index], current.objects[index]))
        {
            return true;
        }
    }

    return false;
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
    float temp[16];
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            temp[column * 4 + row] = 0.0f;
            for (int inner = 0; inner < 4; ++inner)
            {
                temp[column * 4 + row] += left[inner * 4 + row] * right[column * 4 + inner];
            }
        }
    }

    std::memcpy(result, temp, sizeof(temp));
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

void BuildTransformMatrix(const SceneVector3& position, const SceneVector3& rotation, const SceneVector3& scale, float* matrix)
{
    float scale_matrix[16];
    float rotation_x_matrix[16];
    float rotation_y_matrix[16];
    float rotation_z_matrix[16];
    float translation_matrix[16];
    float temp_a[16];
    float temp_b[16];

    BuildScaleMatrix(scale, scale_matrix);
    BuildRotationXMatrix(DegreesToRadians(rotation[0]), rotation_x_matrix);
    BuildRotationYMatrix(DegreesToRadians(rotation[1]), rotation_y_matrix);
    BuildRotationZMatrix(DegreesToRadians(rotation[2]), rotation_z_matrix);
    BuildTranslationMatrix(position, translation_matrix);

    MultiplyMatrix(rotation_x_matrix, scale_matrix, temp_a);
    MultiplyMatrix(rotation_y_matrix, temp_a, temp_b);
    MultiplyMatrix(rotation_z_matrix, temp_b, temp_a);
    MultiplyMatrix(translation_matrix, temp_a, matrix);
}

void ApplyLocalModelOffset(float* matrix, const SceneVector3& offset)
{
    if (std::abs(offset[0]) <= 0.000001f && std::abs(offset[1]) <= 0.000001f && std::abs(offset[2]) <= 0.000001f)
    {
        return;
    }

    matrix[12] += offset[0];
    matrix[13] += offset[1];
    matrix[14] += offset[2];
}

SceneVector3 ExtractScaleFromMatrix(const float* matrix)
{
    return SceneVector3{
        Length(Vec3{matrix[0], matrix[1], matrix[2]}),
        Length(Vec3{matrix[4], matrix[5], matrix[6]}),
        Length(Vec3{matrix[8], matrix[9], matrix[10]}),
    };
}

void BuildTransformMatrixFromPhysicsTransform(const PhysicsBodyTransform& transform, const SceneVector3& scale, float* matrix)
{
    const float x = transform.rotation[0];
    const float y = transform.rotation[1];
    const float z = transform.rotation[2];
    const float w = transform.rotation[3];

    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float xz = x * z;
    const float yz = y * z;
    const float wx = w * x;
    const float wy = w * y;
    const float wz = w * z;

    SetIdentity(matrix);
    matrix[0] = (1.0f - 2.0f * (yy + zz)) * scale[0];
    matrix[1] = (2.0f * (xy + wz)) * scale[0];
    matrix[2] = (2.0f * (xz - wy)) * scale[0];

    matrix[4] = (2.0f * (xy - wz)) * scale[1];
    matrix[5] = (1.0f - 2.0f * (xx + zz)) * scale[1];
    matrix[6] = (2.0f * (yz + wx)) * scale[1];

    matrix[8] = (2.0f * (xz + wy)) * scale[2];
    matrix[9] = (2.0f * (yz - wx)) * scale[2];
    matrix[10] = (1.0f - 2.0f * (xx + yy)) * scale[2];

    matrix[12] = transform.position[0];
    matrix[13] = transform.position[1];
    matrix[14] = transform.position[2];
}

Vec3 TransformPoint(const float* matrix, const Vec3& point)
{
    return Vec3{
        matrix[0] * point.x + matrix[4] * point.y + matrix[8] * point.z + matrix[12],
        matrix[1] * point.x + matrix[5] * point.y + matrix[9] * point.z + matrix[13],
        matrix[2] * point.x + matrix[6] * point.y + matrix[10] * point.z + matrix[14]};
}

Vec3 TransformDirectionByMatrix(const float* matrix, const Vec3& direction)
{
    return Normalize(Vec3{
        matrix[0] * direction.x + matrix[4] * direction.y + matrix[8] * direction.z,
        matrix[1] * direction.x + matrix[5] * direction.y + matrix[9] * direction.z,
        matrix[2] * direction.x + matrix[6] * direction.y + matrix[10] * direction.z});
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

bool InvertMatrix(const float* matrix, float* inverse)
{
    float inv[16];

    inv[0] = matrix[5] * matrix[10] * matrix[15] - matrix[5] * matrix[11] * matrix[14] - matrix[9] * matrix[6] * matrix[15] + matrix[9] * matrix[7] * matrix[14] + matrix[13] * matrix[6] * matrix[11] - matrix[13] * matrix[7] * matrix[10];
    inv[4] = -matrix[4] * matrix[10] * matrix[15] + matrix[4] * matrix[11] * matrix[14] + matrix[8] * matrix[6] * matrix[15] - matrix[8] * matrix[7] * matrix[14] - matrix[12] * matrix[6] * matrix[11] + matrix[12] * matrix[7] * matrix[10];
    inv[8] = matrix[4] * matrix[9] * matrix[15] - matrix[4] * matrix[11] * matrix[13] - matrix[8] * matrix[5] * matrix[15] + matrix[8] * matrix[7] * matrix[13] + matrix[12] * matrix[5] * matrix[11] - matrix[12] * matrix[7] * matrix[9];
    inv[12] = -matrix[4] * matrix[9] * matrix[14] + matrix[4] * matrix[10] * matrix[13] + matrix[8] * matrix[5] * matrix[14] - matrix[8] * matrix[6] * matrix[13] - matrix[12] * matrix[5] * matrix[10] + matrix[12] * matrix[6] * matrix[9];
    inv[1] = -matrix[1] * matrix[10] * matrix[15] + matrix[1] * matrix[11] * matrix[14] + matrix[9] * matrix[2] * matrix[15] - matrix[9] * matrix[3] * matrix[14] - matrix[13] * matrix[2] * matrix[11] + matrix[13] * matrix[3] * matrix[10];
    inv[5] = matrix[0] * matrix[10] * matrix[15] - matrix[0] * matrix[11] * matrix[14] - matrix[8] * matrix[2] * matrix[15] + matrix[8] * matrix[3] * matrix[14] + matrix[12] * matrix[2] * matrix[11] - matrix[12] * matrix[3] * matrix[10];
    inv[9] = -matrix[0] * matrix[9] * matrix[15] + matrix[0] * matrix[11] * matrix[13] + matrix[8] * matrix[1] * matrix[15] - matrix[8] * matrix[3] * matrix[13] - matrix[12] * matrix[1] * matrix[11] + matrix[12] * matrix[3] * matrix[9];
    inv[13] = matrix[0] * matrix[9] * matrix[14] - matrix[0] * matrix[10] * matrix[13] - matrix[8] * matrix[1] * matrix[14] + matrix[8] * matrix[2] * matrix[13] + matrix[12] * matrix[1] * matrix[10] - matrix[12] * matrix[2] * matrix[9];
    inv[2] = matrix[1] * matrix[6] * matrix[15] - matrix[1] * matrix[7] * matrix[14] - matrix[5] * matrix[2] * matrix[15] + matrix[5] * matrix[3] * matrix[14] + matrix[13] * matrix[2] * matrix[7] - matrix[13] * matrix[3] * matrix[6];
    inv[6] = -matrix[0] * matrix[6] * matrix[15] + matrix[0] * matrix[7] * matrix[14] + matrix[4] * matrix[2] * matrix[15] - matrix[4] * matrix[3] * matrix[14] - matrix[12] * matrix[2] * matrix[7] + matrix[12] * matrix[3] * matrix[6];
    inv[10] = matrix[0] * matrix[5] * matrix[15] - matrix[0] * matrix[7] * matrix[13] - matrix[4] * matrix[1] * matrix[15] + matrix[4] * matrix[3] * matrix[13] + matrix[12] * matrix[1] * matrix[7] - matrix[12] * matrix[3] * matrix[5];
    inv[14] = -matrix[0] * matrix[5] * matrix[14] + matrix[0] * matrix[6] * matrix[13] + matrix[4] * matrix[1] * matrix[14] - matrix[4] * matrix[2] * matrix[13] - matrix[12] * matrix[1] * matrix[6] + matrix[12] * matrix[2] * matrix[5];
    inv[3] = -matrix[1] * matrix[6] * matrix[11] + matrix[1] * matrix[7] * matrix[10] + matrix[5] * matrix[2] * matrix[11] - matrix[5] * matrix[3] * matrix[10] - matrix[9] * matrix[2] * matrix[7] + matrix[9] * matrix[3] * matrix[6];
    inv[7] = matrix[0] * matrix[6] * matrix[11] - matrix[0] * matrix[7] * matrix[10] - matrix[4] * matrix[2] * matrix[11] + matrix[4] * matrix[3] * matrix[10] + matrix[8] * matrix[2] * matrix[7] - matrix[8] * matrix[3] * matrix[6];
    inv[11] = -matrix[0] * matrix[5] * matrix[11] + matrix[0] * matrix[7] * matrix[9] + matrix[4] * matrix[1] * matrix[11] - matrix[4] * matrix[3] * matrix[9] - matrix[8] * matrix[1] * matrix[7] + matrix[8] * matrix[3] * matrix[5];
    inv[15] = matrix[0] * matrix[5] * matrix[10] - matrix[0] * matrix[6] * matrix[9] - matrix[4] * matrix[1] * matrix[10] + matrix[4] * matrix[2] * matrix[9] + matrix[8] * matrix[1] * matrix[6] - matrix[8] * matrix[2] * matrix[5];

    float determinant = matrix[0] * inv[0] + matrix[1] * inv[4] + matrix[2] * inv[8] + matrix[3] * inv[12];
    if (std::abs(determinant) <= 0.000001f)
    {
        return false;
    }

    determinant = 1.0f / determinant;
    for (int index = 0; index < 16; ++index)
    {
        inverse[index] = inv[index] * determinant;
    }

    return true;
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

struct SceneResolvedObjectPose
{
    std::array<float, 16> world_matrix = {};
    std::array<float, 16> parent_matrix = {};
    bool has_parent = false;
    bool resolved = false;
    bool resolving = false;
};

using SceneResolvedObjectPoseMap = std::unordered_map<std::string, SceneResolvedObjectPose>;

SceneLightingResolvedObjectPoseMap BuildLightingPoseMap(const SceneResolvedObjectPoseMap& resolved_poses)
{
    SceneLightingResolvedObjectPoseMap lighting_poses;
    lighting_poses.reserve(resolved_poses.size());
    for (const auto& [name, pose] : resolved_poses)
    {
        lighting_poses.emplace(name, SceneLightingResolvedObjectPose{pose.world_matrix});
    }
    return lighting_poses;
}

SceneResolvedObjectPoseMap ResolveSceneObjectPoses(const SceneMetadata& scene_metadata)
{
    std::unordered_map<std::string, const SceneObjectMetadata*> objects_by_name;
    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        objects_by_name[object.name] = &object;
    }

    SceneResolvedObjectPoseMap poses;
    std::function<const SceneResolvedObjectPose&(const std::string&)> resolve_pose = [&](const std::string& object_name) -> const SceneResolvedObjectPose&
    {
        SceneResolvedObjectPose& pose = poses[object_name];
        if (pose.resolved)
        {
            return pose;
        }

        SetIdentity(pose.world_matrix.data());
        SetIdentity(pose.parent_matrix.data());

        const auto object_it = objects_by_name.find(object_name);
        if (object_it == objects_by_name.end())
        {
            pose.resolved = true;
            return pose;
        }

        if (pose.resolving)
        {
            pose.resolved = true;
            return pose;
        }

        pose.resolving = true;

        float local_matrix[16];
        BuildTransformMatrix(object_it->second->position, object_it->second->rotation, object_it->second->scale, local_matrix);

        const std::string& parent_name = object_it->second->parent_name;
        const auto parent_it = objects_by_name.find(parent_name);
        if (!parent_name.empty() && parent_it != objects_by_name.end() && parent_name != object_name)
        {
            const SceneResolvedObjectPose& parent_pose = resolve_pose(parent_name);
            std::memcpy(pose.parent_matrix.data(), parent_pose.world_matrix.data(), sizeof(float) * 16);
            MultiplyMatrix(parent_pose.world_matrix.data(), local_matrix, pose.world_matrix.data());
            pose.has_parent = true;
        }
        else
        {
            std::memcpy(pose.world_matrix.data(), local_matrix, sizeof(local_matrix));
        }

        pose.resolving = false;
        pose.resolved = true;
        return pose;
    };

    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        resolve_pose(object.name);
    }

    return poses;
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

bool CreateVulkanBuffer(
    const VulkanContext& context,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    RuntimeRenderer::GpuBuffer& buffer)
{
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(context.GetDevice(), &buffer_info, context.GetAllocator(), &buffer.buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements requirements = {};
    vkGetBufferMemoryRequirements(context.GetDevice(), buffer.buffer, &requirements);

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = FindMemoryType(context.GetPhysicalDevice(), requirements.memoryTypeBits, properties);

    VkMemoryAllocateFlagsInfo allocate_flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
    {
        allocate_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        allocate_info.pNext = &allocate_flags;
    }

    if (allocate_info.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyBuffer(context.GetDevice(), buffer.buffer, context.GetAllocator());
        buffer.buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkAllocateMemory(context.GetDevice(), &allocate_info, context.GetAllocator(), &buffer.memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyBuffer(context.GetDevice(), buffer.buffer, context.GetAllocator());
        buffer.buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindBufferMemory(context.GetDevice(), buffer.buffer, buffer.memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(context.GetDevice(), buffer.memory, context.GetAllocator());
        vkDestroyBuffer(context.GetDevice(), buffer.buffer, context.GetAllocator());
        buffer = {};
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
            buffer.device_address = dispatch.get_buffer_device_address(context.GetDevice(), &address_info);
        }
    }

    return true;
}

bool UploadBufferData(VkDevice device, const RuntimeRenderer::GpuBuffer& buffer, const void* data, std::size_t size)
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
    allocation_info.memoryTypeIndex = FindMemoryType(physical_device, memory_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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

RuntimeRenderer::GpuTexture* SelectTextureSlot(RuntimeRenderer::GpuMaterialTextures& material_textures, std::size_t slot_index)
{
    switch (slot_index)
    {
    case 0:
        return &material_textures.base_color;
    case 1:
        return &material_textures.metallic_roughness;
    case 2:
        return &material_textures.normal;
    case 3:
        return &material_textures.occlusion;
    case 4:
        return &material_textures.emissive;
    default:
        return nullptr;
    }
}

bool CreateTextureFromAsset(
    VulkanContext& context,
    VkCommandPool command_pool,
    const ModelTextureAsset& texture_asset,
    RuntimeRenderer::GpuTexture& texture)
{
    if (command_pool == VK_NULL_HANDLE || !texture_asset.valid || texture_asset.width <= 0 || texture_asset.height <= 0 || texture_asset.pixels.empty())
    {
        return false;
    }

    const VkDevice device = context.GetDevice();
    const VkDeviceSize upload_size = static_cast<VkDeviceSize>(texture_asset.width) * static_cast<VkDeviceSize>(texture_asset.height) * 4u;
    const VkFormat texture_format = texture_asset.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    RuntimeRenderer::GpuBuffer staging_buffer{};
    if (!CreateVulkanBuffer(
            context,
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
            context.GetPhysicalDevice(),
            device,
            context.GetAllocator(),
            static_cast<std::uint32_t>(texture_asset.width),
            static_cast<std::uint32_t>(texture_asset.height),
            texture_format,
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
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
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

    return true;
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
    gpu_vertex.tangent[0] = vertex.tangent[0];
    gpu_vertex.tangent[1] = vertex.tangent[1];
    gpu_vertex.tangent[2] = vertex.tangent[2];
    gpu_vertex.tangent[3] = vertex.tangent[3];
    return gpu_vertex;
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streampos length = input.tellg();
    if (length < 0)
    {
        return false;
    }

    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(length));
    if (!bytes.empty())
    {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input)
        {
            return false;
        }
    }

    return true;
}

std::string BuildScriptInstanceKey(const std::string& object_name, const std::filesystem::path& script_path)
{
    return object_name + "|" + script_path.generic_string();
}
}

bool RuntimeRenderer::Initialize(VulkanContext* context)
{
    vulkan_context_ = context;
    if (vulkan_context_ == nullptr)
    {
        return false;
    }

    if (!InitializeScriptRuntime(nullptr))
    {
        return false;
    }

    if (!physics_world_.Initialize(nullptr))
    {
        ShutdownScriptRuntime();
        return false;
    }

    if (!ray_tracing_.Initialize(context))
    {
        physics_world_.Shutdown();
        ShutdownScriptRuntime();
        return false;
    }

    scene_2d_renderer_.Initialize(context);
    skybox_renderer_.Initialize(context);

    return true;
}

void RuntimeRenderer::ReleaseBuffer(GpuBuffer& buffer)
{
    if (vulkan_context_ == nullptr)
    {
        buffer = {};
        return;
    }

    if (buffer.buffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(vulkan_context_->GetDevice(), buffer.buffer, vulkan_context_->GetAllocator());
    }
    if (buffer.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vulkan_context_->GetDevice(), buffer.memory, vulkan_context_->GetAllocator());
    }
    buffer = {};
}

void RuntimeRenderer::ReleaseTexture(GpuTexture& texture)
{
    if (vulkan_context_ == nullptr)
    {
        texture = {};
        return;
    }

    if (texture.view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(vulkan_context_->GetDevice(), texture.view, vulkan_context_->GetAllocator());
    }
    if (texture.image != VK_NULL_HANDLE)
    {
        vkDestroyImage(vulkan_context_->GetDevice(), texture.image, vulkan_context_->GetAllocator());
    }
    if (texture.memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(vulkan_context_->GetDevice(), texture.memory, vulkan_context_->GetAllocator());
    }
    texture = {};
}

void RuntimeRenderer::ReleaseMeshCacheEntry(GpuMeshCacheEntry& entry)
{
    ReleaseBuffer(entry.vertex_buffer);
    ReleaseBuffer(entry.index_buffer);
    for (GpuMaterialTextures& textures : entry.material_textures)
    {
        ReleaseTexture(textures.base_color);
        ReleaseTexture(textures.metallic_roughness);
        ReleaseTexture(textures.normal);
        ReleaseTexture(textures.occlusion);
        ReleaseTexture(textures.emissive);
    }

    entry = {};
}

void RuntimeRenderer::Shutdown()
{
    skybox_renderer_.Shutdown();
    ShutdownScriptRuntime();
    physics_world_.Shutdown();
    scene_2d_renderer_.Shutdown();
    ray_tracing_.Shutdown();
    for (auto& [path, entry] : mesh_cache_)
    {
        ReleaseMeshCacheEntry(entry);
    }

    mesh_cache_.clear();
    script_cache_.clear();
    ClearScriptEventSubscriptions();
    ClearScriptTimers();
    runtime_spawned_objects_.clear();
    runtime_destroyed_objects_.clear();
    physics_object_transforms_.clear();
    script_object_position_overrides_.clear();
    script_object_rotation_overrides_.clear();
    script_object_scale_overrides_.clear();
    script_active_instance_key_.clear();
    script_active_object_name_.clear();
    script_prev_keys_down_.clear();
    script_frame_collision_events_.clear();
    model_asset_cache_.clear();
    queued_objects_.clear();
    cached_scene_path_.clear();
    cached_scene_metadata_ = SceneMetadata{};
    has_cached_scene_metadata_ = false;
    project_root_.clear();
    scene_path_.clear();
    active_camera_object_name_.clear();
    active_camera_attribute_index_ = 0;
    vulkan_context_ = nullptr;
}

bool RuntimeRenderer::StartSession(
    const std::filesystem::path& project_root,
    const std::filesystem::path& scene_path,
    const ActiveSceneCameraSelection& active_camera,
    std::string* error_message)
{
    if (vulkan_context_ == nullptr)
    {
        if (error_message != nullptr)
        {
            *error_message = "Runtime renderer has no Vulkan context";
        }
        return false;
    }

    if (!active_camera.found)
    {
        if (error_message != nullptr)
        {
            *error_message = "Play requires an active scene camera";
        }
        return false;
    }

    project_root_ = project_root;
    scene_path_ = scene_path;
    active_camera_object_name_ = active_camera.object_name;
    active_camera_attribute_index_ = active_camera.attribute_index;
    cached_scene_path_.clear();
    cached_scene_metadata_ = SceneMetadata{};
    has_cached_scene_metadata_ = false;
    queued_objects_.clear();
    DestroyAllScriptInstances();
    ClearScriptEventSubscriptions();
    ClearScriptTimers();
    runtime_spawned_objects_.clear();
    runtime_destroyed_objects_.clear();
    physics_object_transforms_.clear();
    script_object_position_overrides_.clear();
    script_object_rotation_overrides_.clear();
    script_object_scale_overrides_.clear();
    script_active_instance_key_.clear();
    script_active_object_name_.clear();
    script_prev_keys_down_.clear();
    script_frame_collision_events_.clear();
    script_next_timer_id_ = 1;
    script_timer_pending_clear_.clear();
    const std::uint64_t now_ms = static_cast<std::uint64_t>(SDL_GetTicks());
    script_last_tick_ms_ = now_ms;
    script_session_start_ms_ = now_ms;
    physics_world_built_ = false;
    return true;
}

const RuntimeRenderer::CachedModelAssetEntry& RuntimeRenderer::GetModelAssetEntry(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    CachedModelAssetEntry& cache_entry = model_asset_cache_[path];

    const bool has_filesystem_time = !error;
    const bool exists_in_vfs = g_asset_reader && g_asset_reader->FileExists(path.generic_string());
    const bool should_reload =
        !cache_entry.asset.loaded ||
        (has_filesystem_time && cache_entry.write_time != write_time) ||
        (!has_filesystem_time && !exists_in_vfs);

    if (should_reload)
    {
        cache_entry.write_time = has_filesystem_time ? write_time : std::filesystem::file_time_type::min();
        cache_entry.asset = LoadModelAsset(path);
    }

    return cache_entry;
}

const SceneMetadata& RuntimeRenderer::GetSceneMetadata()
{
    if (scene_path_.empty())
    {
        static SceneMetadata empty_metadata{};
        return empty_metadata;
    }

    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(scene_path_, error);

    const bool has_filesystem_time = !error;
    const bool exists_in_vfs = g_asset_reader && g_asset_reader->FileExists(scene_path_.generic_string());
    const bool cache_valid =
        has_cached_scene_metadata_ &&
        cached_scene_path_ == scene_path_ &&
        ((has_filesystem_time && cached_scene_write_time_ == write_time) || (!has_filesystem_time && exists_in_vfs));

    if (!cache_valid)
    {
        const bool scene_changed = has_cached_scene_metadata_ && cached_scene_path_ == scene_path_;
        const SceneMetadata previous_scene_metadata = cached_scene_metadata_;
        cached_scene_path_ = scene_path_;
        cached_scene_write_time_ = has_filesystem_time ? write_time : std::filesystem::file_time_type::min();
        cached_scene_metadata_ = LoadSceneMetadata(scene_path_);
        has_cached_scene_metadata_ = true;

        if (scene_changed && SceneChangeRequiresPhysicsReset(previous_scene_metadata, cached_scene_metadata_))
        {
            physics_world_built_ = false;
            physics_object_transforms_.clear();
        }
    }

    return cached_scene_metadata_;
}

bool RuntimeRenderer::EnsureMeshCacheEntry(const std::filesystem::path& model_path, const CachedModelAssetEntry& model_asset_entry)
{
    if (vulkan_context_ == nullptr || !model_asset_entry.asset.loaded)
    {
        return false;
    }

    GpuMeshCacheEntry& cache_entry = mesh_cache_[model_path];
    if (cache_entry.vertex_buffer.buffer != VK_NULL_HANDLE && cache_entry.index_buffer.buffer != VK_NULL_HANDLE && cache_entry.write_time == model_asset_entry.write_time)
    {
        return true;
    }

    ReleaseMeshCacheEntry(cache_entry);

    std::vector<SceneGpuVertex> vertices;
    std::vector<std::uint32_t> indices;
    cache_entry.material_textures.resize(model_asset_entry.asset.materials.size());
    cache_entry.materials.resize(model_asset_entry.asset.materials.size());

    for (std::size_t material_index = 0; material_index < model_asset_entry.asset.materials.size(); ++material_index)
    {
        if ((material_index & 7u) == 0u)
        {
            SDL_PumpEvents();
        }

        const ModelMaterialAsset& material = model_asset_entry.asset.materials[material_index];
        cache_entry.materials[material_index].base_color = material.base_color;
        cache_entry.materials[material_index].emissive_color = material.emissive_color;
        cache_entry.materials[material_index].metallic_factor = material.metallic_factor;
        cache_entry.materials[material_index].roughness_factor = material.roughness_factor;
        cache_entry.materials[material_index].normal_scale = material.normal_scale;
        cache_entry.materials[material_index].occlusion_strength = material.occlusion_strength;
        cache_entry.materials[material_index].uses_alpha_transparency = material.uses_alpha_transparency;

        const ModelTextureAsset* texture_assets[5] = {
            &material.base_color_texture,
            &material.metallic_roughness_texture,
            &material.normal_texture,
            &material.occlusion_texture,
            &material.emissive_texture,
        };

        for (std::size_t texture_index = 0; texture_index < std::size(texture_assets); ++texture_index)
        {
            if (!texture_assets[texture_index]->valid)
            {
                continue;
            }

            SDL_PumpEvents();

            GpuTexture* texture_slot = SelectTextureSlot(cache_entry.material_textures[material_index], texture_index);
            if (texture_slot != nullptr)
            {
                CreateTextureFromAsset(*vulkan_context_, ray_tracing_.GetCommandPool(), *texture_assets[texture_index], *texture_slot);
            }
        }

        cache_entry.materials[material_index].base_color_view = cache_entry.material_textures[material_index].base_color.view;
        cache_entry.materials[material_index].metallic_roughness_view = cache_entry.material_textures[material_index].metallic_roughness.view;
        cache_entry.materials[material_index].normal_view = cache_entry.material_textures[material_index].normal.view;
        cache_entry.materials[material_index].occlusion_view = cache_entry.material_textures[material_index].occlusion.view;
        cache_entry.materials[material_index].emissive_view = cache_entry.material_textures[material_index].emissive.view;
    }

    std::size_t mesh_scan_count = 0;
    for (const ModelMeshAsset& mesh : model_asset_entry.asset.meshes)
    {
        if ((mesh_scan_count++ & 7u) == 0u)
        {
            SDL_PumpEvents();
        }

        GpuMeshSection section;
        section.first_index = static_cast<std::uint32_t>(indices.size());
        section.material_index = mesh.material_index;
        const std::uint32_t base_vertex = static_cast<std::uint32_t>(vertices.size());
        const ModelMaterialAsset* material = mesh.material_index < model_asset_entry.asset.materials.size() ? &model_asset_entry.asset.materials[mesh.material_index] : nullptr;
        section.uses_alpha_transparency = material != nullptr && material->uses_alpha_transparency;

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
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    VkBufferUsageFlags vertex_usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBufferUsageFlags index_usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (ray_tracing_.IsAvailable())
    {
        vertex_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        index_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    if (!CreateVulkanBuffer(
            *vulkan_context_,
            static_cast<VkDeviceSize>(vertices.size() * sizeof(SceneGpuVertex)),
            vertex_usage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            cache_entry.vertex_buffer) ||
        !CreateVulkanBuffer(
            *vulkan_context_,
            static_cast<VkDeviceSize>(indices.size() * sizeof(std::uint32_t)),
            index_usage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            cache_entry.index_buffer) ||
        !UploadBufferData(vulkan_context_->GetDevice(), cache_entry.vertex_buffer, vertices.data(), vertices.size() * sizeof(SceneGpuVertex)) ||
        !UploadBufferData(vulkan_context_->GetDevice(), cache_entry.index_buffer, indices.data(), indices.size() * sizeof(std::uint32_t)))
    {
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    cache_entry.vertex_count = static_cast<std::uint32_t>(vertices.size());
    cache_entry.index_count = static_cast<std::uint32_t>(indices.size());
    cache_entry.write_time = model_asset_entry.write_time;
    return true;
}

bool RuntimeRenderer::EnsureScriptCacheEntry(const std::filesystem::path& script_path, std::string* error_message)
{
    CachedScriptSourceEntry& cache_entry = script_cache_[script_path];

    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(script_path, error);
    const bool has_filesystem_time = !error;
    const bool exists_in_vfs = g_asset_reader && g_asset_reader->FileExists(script_path.generic_string());
    if (!has_filesystem_time && !exists_in_vfs)
    {
        if (error_message != nullptr)
        {
            *error_message = "Missing script asset: " + script_path.generic_string();
        }
        return false;
    }

    const bool should_reload =
        !cache_entry.loaded ||
        (has_filesystem_time && cache_entry.write_time != write_time);
    if (!should_reload)
    {
        return true;
    }

    std::vector<std::uint8_t> source_bytes;
    if (has_filesystem_time)
    {
        if (!ReadFileBytes(script_path, source_bytes))
        {
            if (error_message != nullptr)
            {
                *error_message = "Failed to read script: " + script_path.generic_string();
            }
            return false;
        }
    }
    else
    {
        source_bytes = g_asset_reader->ReadFile(script_path.generic_string());
    }

    cache_entry.write_time = has_filesystem_time ? write_time : std::filesystem::file_time_type::min();
    cache_entry.source_bytes = std::move(source_bytes);
    cache_entry.loaded = true;
    return true;
}

bool RuntimeRenderer::InitializeScriptRuntime(std::string* error_message)
{
    if (script_lua_state_ != nullptr)
    {
        return true;
    }

    script_lua_state_ = luaL_newstate();
    if (script_lua_state_ == nullptr)
    {
        if (error_message != nullptr)
        {
            *error_message = "Failed to initialize Lua runtime";
        }
        return false;
    }

    luaL_openlibs(script_lua_state_);

    lua_newtable(script_lua_state_);

    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaLog, 1);
    lua_setfield(script_lua_state_, -2, "Log");

    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaSetObjectPosition, 1);
    lua_setfield(script_lua_state_, -2, "SetObjectPosition");

    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaGetObjectPosition, 1);
    lua_setfield(script_lua_state_, -2, "GetObjectPosition");

    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaSetObjectRotation, 1);
    lua_setfield(script_lua_state_, -2, "SetObjectRotation");

    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaGetObjectRotation, 1);
    lua_setfield(script_lua_state_, -2, "GetObjectRotation");

    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaSetObjectScale, 1);
    lua_setfield(script_lua_state_, -2, "SetObjectScale");

    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaGetObjectScale, 1);
    lua_setfield(script_lua_state_, -2, "GetObjectScale");

    struct AttributeAccessorBinding
    {
        const char* table_name;
        const char* method_name;
        ScriptAttributeAccessorId accessor_id;
    };

    const AttributeAccessorBinding attribute_bindings[] = {
        {"EnvironmentLightAttr", "Color", ScriptAttributeAccessorId::EnvironmentLightColor},
        {"EnvironmentLightAttr", "Intensity", ScriptAttributeAccessorId::EnvironmentLightIntensity},
        {"DirectionalLightAttr", "Color", ScriptAttributeAccessorId::DirectionalLightColor},
        {"DirectionalLightAttr", "Intensity", ScriptAttributeAccessorId::DirectionalLightIntensity},
        {"PointLightAttr", "Color", ScriptAttributeAccessorId::PointLightColor},
        {"PointLightAttr", "Intensity", ScriptAttributeAccessorId::PointLightIntensity},
        {"PointLightAttr", "Range", ScriptAttributeAccessorId::PointLightRange},
        {"PointLightAttr", "Radius", ScriptAttributeAccessorId::PointLightRadius},
        {"PointLightAttr", "HaloIntensity", ScriptAttributeAccessorId::PointLightHaloIntensity},
        {"PointLightAttr", "HaloRadius", ScriptAttributeAccessorId::PointLightHaloRadius},
        {"SpotLightAttr", "Color", ScriptAttributeAccessorId::SpotLightColor},
        {"SpotLightAttr", "Intensity", ScriptAttributeAccessorId::SpotLightIntensity},
        {"SpotLightAttr", "Range", ScriptAttributeAccessorId::SpotLightRange},
        {"SpotLightAttr", "InnerCone", ScriptAttributeAccessorId::SpotLightInnerCone},
        {"SpotLightAttr", "OuterCone", ScriptAttributeAccessorId::SpotLightOuterCone},
        {"CameraAttr", "FieldOfView", ScriptAttributeAccessorId::CameraFieldOfView},
        {"CameraAttr", "NearClip", ScriptAttributeAccessorId::CameraNearClip},
        {"CameraAttr", "FarClip", ScriptAttributeAccessorId::CameraFarClip},
        {"CameraAttr", "Active", ScriptAttributeAccessorId::CameraActive},
        {"RigidbodyAttr", "Shape", ScriptAttributeAccessorId::RigidbodyShape},
        {"RigidbodyAttr", "Dynamic", ScriptAttributeAccessorId::RigidbodyDynamic},
        {"RigidbodyAttr", "LockRotationX", ScriptAttributeAccessorId::RigidbodyLockRotationX},
        {"RigidbodyAttr", "LockRotationY", ScriptAttributeAccessorId::RigidbodyLockRotationY},
        {"RigidbodyAttr", "LockRotationZ", ScriptAttributeAccessorId::RigidbodyLockRotationZ},
        {"RigidbodyAttr", "Mass", ScriptAttributeAccessorId::RigidbodyMass},
        {"RigidbodyAttr", "Friction", ScriptAttributeAccessorId::RigidbodyFriction},
        {"RigidbodyAttr", "Radius", ScriptAttributeAccessorId::RigidbodyRadius},
        {"RigidbodyAttr", "CapsuleHalfHeight", ScriptAttributeAccessorId::RigidbodyCapsuleHalfHeight},
        {"RigidbodyAttr", "HalfExtent", ScriptAttributeAccessorId::RigidbodyHalfExtent},
        {"RigidbodyAttr", "LinearDamping", ScriptAttributeAccessorId::RigidbodyLinearDamping},
        {"RigidbodyAttr", "AngularDamping", ScriptAttributeAccessorId::RigidbodyAngularDamping},
        {"TriggerVolumeAttr", "HalfExtent", ScriptAttributeAccessorId::TriggerVolumeHalfExtent},
        {"Text2DAttr", "FontPath", ScriptAttributeAccessorId::Text2DFontPath},
        {"Text2DAttr", "Text", ScriptAttributeAccessorId::Text2DText},
        {"Text2DAttr", "Position", ScriptAttributeAccessorId::Text2DPosition},
        {"Text2DAttr", "Size", ScriptAttributeAccessorId::Text2DSize},
        {"Text2DAttr", "LockAspectRatio", ScriptAttributeAccessorId::Text2DLockAspectRatio},
        {"Text2DAttr", "FontSize", ScriptAttributeAccessorId::Text2DFontSize},
        {"Text2DAttr", "Color", ScriptAttributeAccessorId::Text2DColor},
        {"Text2DAttr", "Alpha", ScriptAttributeAccessorId::Text2DAlpha},
        {"Text2DAttr", "Priority", ScriptAttributeAccessorId::Text2DPriority},
        {"Image2DAttr", "ImagePath", ScriptAttributeAccessorId::Image2DImagePath},
        {"Image2DAttr", "Position", ScriptAttributeAccessorId::Image2DPosition},
        {"Image2DAttr", "Size", ScriptAttributeAccessorId::Image2DSize},
        {"Image2DAttr", "LockAspectRatio", ScriptAttributeAccessorId::Image2DLockAspectRatio},
        {"Image2DAttr", "Tint", ScriptAttributeAccessorId::Image2DTint},
        {"Image2DAttr", "Alpha", ScriptAttributeAccessorId::Image2DAlpha},
        {"Image2DAttr", "Priority", ScriptAttributeAccessorId::Image2DPriority},
        {"SkyboxAttr", "ImagePath", ScriptAttributeAccessorId::SkyboxImagePath},
        {"SkyboxAttr", "Rotation", ScriptAttributeAccessorId::SkyboxRotation},
    };

    const auto bind_attribute_accessor = [&](const char* table_name, const char* method_name, ScriptAttributeAccessorId accessor_id)
    {
        lua_getfield(script_lua_state_, -1, table_name);
        if (lua_isnil(script_lua_state_, -1))
        {
            lua_pop(script_lua_state_, 1);
            lua_newtable(script_lua_state_);
        }

        lua_pushlightuserdata(script_lua_state_, this);
        lua_pushinteger(script_lua_state_, static_cast<lua_Integer>(accessor_id));
        lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaAttributeAccessor, 2);
        lua_setfield(script_lua_state_, -2, method_name);
        lua_setfield(script_lua_state_, -2, table_name);
    };

    for (const AttributeAccessorBinding& binding : attribute_bindings)
    {
        bind_attribute_accessor(binding.table_name, binding.method_name, binding.accessor_id);
    }

    lua_setglobal(script_lua_state_, "Engine");

    // Time table — fields updated each frame in UpdateScriptsForFrame
    lua_newtable(script_lua_state_);
    lua_pushnumber(script_lua_state_, 0.0);
    lua_setfield(script_lua_state_, -2, "DeltaTime");
    lua_pushnumber(script_lua_state_, 0.0);
    lua_setfield(script_lua_state_, -2, "TotalTime");
    lua_setglobal(script_lua_state_, "Time");

    // Input table
    lua_newtable(script_lua_state_);
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaInputIsKeyDown, 1);
    lua_setfield(script_lua_state_, -2, "IsKeyDown");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaInputWasKeyPressed, 1);
    lua_setfield(script_lua_state_, -2, "WasKeyPressed");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaInputMousePosition, 1);
    lua_setfield(script_lua_state_, -2, "MousePosition");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaInputMouseDelta, 1);
    lua_setfield(script_lua_state_, -2, "MouseDelta");
    lua_setglobal(script_lua_state_, "Input");

    // World table for script-to-script events.
    lua_newtable(script_lua_state_);
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldSubscribe, 1);
    lua_setfield(script_lua_state_, -2, "Subscribe");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldEmit, 1);
    lua_setfield(script_lua_state_, -2, "Emit");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldSpawn, 1);
    lua_setfield(script_lua_state_, -2, "Spawn");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldSpawnFromObject, 1);
    lua_setfield(script_lua_state_, -2, "SpawnFromObject");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldDestroy, 1);
    lua_setfield(script_lua_state_, -2, "Destroy");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldDestroyByPrefix, 1);
    lua_setfield(script_lua_state_, -2, "DestroyByPrefix");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldExists, 1);
    lua_setfield(script_lua_state_, -2, "Exists");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldGetAll, 1);
    lua_setfield(script_lua_state_, -2, "GetAll");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldFindByPrefix, 1);
    lua_setfield(script_lua_state_, -2, "FindByPrefix");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldGetCollisions, 1);
    lua_setfield(script_lua_state_, -2, "GetCollisions");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldGetCollisionsFor, 1);
    lua_setfield(script_lua_state_, -2, "GetCollisionsFor");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldGetCollisionsByPhase, 1);
    lua_setfield(script_lua_state_, -2, "GetCollisionsByPhase");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldSetTimeout, 1);
    lua_setfield(script_lua_state_, -2, "SetTimeout");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldSetInterval, 1);
    lua_setfield(script_lua_state_, -2, "SetInterval");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldClearTimer, 1);
    lua_setfield(script_lua_state_, -2, "ClearTimer");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaWorldLoadScene, 1);
    lua_setfield(script_lua_state_, -2, "LoadScene");
    lua_setglobal(script_lua_state_, "World");

    // Physics table
    lua_newtable(script_lua_state_);
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaPhysicsRaycast, 1);
    lua_setfield(script_lua_state_, -2, "Raycast");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaPhysicsSetVelocity, 1);
    lua_setfield(script_lua_state_, -2, "SetVelocity");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaPhysicsGetVelocity, 1);
    lua_setfield(script_lua_state_, -2, "GetVelocity");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaPhysicsAddImpulse, 1);
    lua_setfield(script_lua_state_, -2, "AddImpulse");
    lua_pushlightuserdata(script_lua_state_, this);
    lua_pushcclosure(script_lua_state_, &RuntimeRenderer::LuaPhysicsAddForce, 1);
    lua_setfield(script_lua_state_, -2, "AddForce");
    lua_setglobal(script_lua_state_, "Physics");

    const std::uint64_t now_ms = static_cast<std::uint64_t>(SDL_GetTicks());
    script_last_tick_ms_ = now_ms;
    script_session_start_ms_ = now_ms;
    return true;
}

void RuntimeRenderer::ShutdownScriptRuntime()
{
    if (script_lua_state_ == nullptr)
    {
        return;
    }

    DestroyAllScriptInstances();
    ClearScriptTimers();
    script_active_instance_key_.clear();
    script_active_object_name_.clear();
    script_frame_collision_events_.clear();
    lua_close(script_lua_state_);
    script_lua_state_ = nullptr;
    script_last_tick_ms_ = 0;
}

void RuntimeRenderer::DestroyAllScriptInstances()
{
    if (script_lua_state_ == nullptr)
    {
        script_event_subscriptions_.clear();
        script_timers_.clear();
        script_instances_.clear();
        return;
    }

    std::string ignored_error;
    for (auto& [key, instance] : script_instances_)
    {
        CallScriptMethod(instance, "OnDestroy", 0.0f, false, &ignored_error);
        RemoveScriptEventSubscriptionsForInstance(instance.instance_key);
        RemoveScriptTimersForInstance(instance.instance_key);
        if (instance.table_ref != LUA_NOREF && instance.table_ref != LUA_REFNIL)
        {
            luaL_unref(script_lua_state_, LUA_REGISTRYINDEX, instance.table_ref);
            instance.table_ref = LUA_NOREF;
        }
    }

    script_instances_.clear();
}

bool RuntimeRenderer::LoadScriptInstance(const std::string& object_name, const std::filesystem::path& script_path, std::string* error_message)
{
    if (script_lua_state_ == nullptr && !InitializeScriptRuntime(error_message))
    {
        return false;
    }

    if (!EnsureScriptCacheEntry(script_path, error_message))
    {
        return false;
    }

    const auto cache_it = script_cache_.find(script_path);
    if (cache_it == script_cache_.end())
    {
        if (error_message != nullptr)
        {
            *error_message = "Missing script cache entry: " + script_path.generic_string();
        }
        return false;
    }

    const CachedScriptSourceEntry& cache_entry = cache_it->second;
    if (cache_entry.source_bytes.empty())
    {
        if (error_message != nullptr)
        {
            *error_message = "Script is empty: " + script_path.generic_string();
        }
        return false;
    }

    lua_State* const lua_state = script_lua_state_;
    const int load_result = luaL_loadbuffer(
        lua_state,
        reinterpret_cast<const char*>(cache_entry.source_bytes.data()),
        cache_entry.source_bytes.size(),
        script_path.generic_string().c_str());
    if (load_result != LUA_OK)
    {
        if (error_message != nullptr)
        {
            const char* message = lua_tostring(lua_state, -1);
            *error_message = "Failed to load script " + script_path.generic_string() + ": " + (message != nullptr ? message : "unknown error");
        }
        lua_pop(lua_state, 1);
        return false;
    }

    const int run_result = lua_pcall(lua_state, 0, 1, 0);
    if (run_result != LUA_OK)
    {
        if (error_message != nullptr)
        {
            const char* message = lua_tostring(lua_state, -1);
            *error_message = "Failed to execute script " + script_path.generic_string() + ": " + (message != nullptr ? message : "unknown error");
        }
        lua_pop(lua_state, 1);
        return false;
    }

    if (!lua_istable(lua_state, -1))
    {
        if (error_message != nullptr)
        {
            *error_message = "Script must return a table: " + script_path.generic_string();
        }
        lua_pop(lua_state, 1);
        return false;
    }

    RuntimeScriptInstance instance;
    instance.instance_key = BuildScriptInstanceKey(object_name, script_path);
    instance.object_name = object_name;
    instance.script_path = script_path;
    instance.table_ref = luaL_ref(lua_state, LUA_REGISTRYINDEX);

    const std::string instance_key = instance.instance_key;
    auto it = script_instances_.find(instance_key);
    if (it != script_instances_.end())
    {
        RemoveScriptEventSubscriptionsForInstance(instance_key);
        RemoveScriptTimersForInstance(instance_key);
        if (it->second.table_ref != LUA_NOREF && it->second.table_ref != LUA_REFNIL)
        {
            luaL_unref(lua_state, LUA_REGISTRYINDEX, it->second.table_ref);
        }
        it->second = std::move(instance);
    }
    else
    {
        auto insert_result = script_instances_.emplace(instance_key, std::move(instance));
        it = insert_result.first;
    }

    RuntimeScriptInstance& loaded_instance = it->second;
    if (!CallScriptMethod(loaded_instance, "OnCreate", 0.0f, false, error_message))
    {
        RemoveScriptEventSubscriptionsForInstance(loaded_instance.instance_key);
        RemoveScriptTimersForInstance(loaded_instance.instance_key);
        if (loaded_instance.table_ref != LUA_NOREF && loaded_instance.table_ref != LUA_REFNIL)
        {
            luaL_unref(lua_state, LUA_REGISTRYINDEX, loaded_instance.table_ref);
        }
        script_instances_.erase(instance_key);
        return false;
    }

    return true;
}

bool RuntimeRenderer::SyncScriptInstances(std::string* error_message)
{
    if (script_lua_state_ == nullptr && !InitializeScriptRuntime(error_message))
    {
        return false;
    }

    std::unordered_set<std::string> desired_instance_keys;
    for (const QueuedSceneObject& queued_object : queued_objects_)
    {
        for (const std::filesystem::path& script_path : queued_object.script_paths)
        {
            const std::string instance_key = BuildScriptInstanceKey(queued_object.name, script_path);
            desired_instance_keys.insert(instance_key);
            if (script_instances_.find(instance_key) == script_instances_.end())
            {
                if (!LoadScriptInstance(queued_object.name, script_path, error_message))
                {
                    return false;
                }
            }
        }
    }

    for (auto it = script_instances_.begin(); it != script_instances_.end();)
    {
        if (desired_instance_keys.find(it->first) != desired_instance_keys.end())
        {
            ++it;
            continue;
        }

        std::string ignored_error;
        CallScriptMethod(it->second, "OnDestroy", 0.0f, false, &ignored_error);
        RemoveScriptEventSubscriptionsForInstance(it->second.instance_key);
        RemoveScriptTimersForInstance(it->second.instance_key);
        if (it->second.table_ref != LUA_NOREF && it->second.table_ref != LUA_REFNIL)
        {
            luaL_unref(script_lua_state_, LUA_REGISTRYINDEX, it->second.table_ref);
        }

        it = script_instances_.erase(it);
    }

    return true;
}

bool RuntimeRenderer::CallScriptMethod(
    RuntimeScriptInstance& instance,
    const char* method_name,
    float delta_time,
    bool include_delta_time,
    std::string* error_message)
{
    if (script_lua_state_ == nullptr || method_name == nullptr)
    {
        return false;
    }

    lua_State* const lua_state = script_lua_state_;
    lua_rawgeti(lua_state, LUA_REGISTRYINDEX, instance.table_ref);
    if (!lua_istable(lua_state, -1))
    {
        lua_pop(lua_state, 1);
        if (error_message != nullptr)
        {
            *error_message = "Script did not return a table for " + instance.script_path.generic_string();
        }
        return false;
    }

    lua_getfield(lua_state, -1, method_name);
    if (lua_isnil(lua_state, -1))
    {
        lua_pop(lua_state, 2);
        return true;
    }

    if (!lua_isfunction(lua_state, -1))
    {
        lua_pop(lua_state, 2);
        if (error_message != nullptr)
        {
            *error_message = "Script member is not a function: " + std::string(method_name);
        }
        return false;
    }

    lua_pushvalue(lua_state, -2);
    lua_pushstring(lua_state, instance.object_name.c_str());
    int argument_count = 2;
    if (include_delta_time)
    {
        lua_pushnumber(lua_state, static_cast<lua_Number>(delta_time));
        ++argument_count;
    }

    const std::string previous_instance_key = script_active_instance_key_;
    const std::string previous_object_name = script_active_object_name_;
    script_active_instance_key_ = instance.instance_key;
    script_active_object_name_ = instance.object_name;

    const int call_result = lua_pcall(lua_state, argument_count, 0, 0);
    script_active_instance_key_ = previous_instance_key;
    script_active_object_name_ = previous_object_name;
    if (call_result != LUA_OK)
    {
        if (error_message != nullptr)
        {
            const char* message = lua_tostring(lua_state, -1);
            *error_message = "Script call failed (" + std::string(method_name) + ") in " + instance.script_path.generic_string() + ": " + (message != nullptr ? message : "unknown error");
        }

        lua_pop(lua_state, 2);
        return false;
    }

    lua_pop(lua_state, 1);
    return true;
}

bool RuntimeRenderer::CallScriptTriggerMethod(
    RuntimeScriptInstance& instance,
    const char* method_name,
    const std::string& other_object_name,
    const std::string& phase,
    std::string* error_message)
{
    if (script_lua_state_ == nullptr || method_name == nullptr)
    {
        return false;
    }

    lua_State* const lua_state = script_lua_state_;
    lua_rawgeti(lua_state, LUA_REGISTRYINDEX, instance.table_ref);
    if (!lua_istable(lua_state, -1))
    {
        lua_pop(lua_state, 1);
        if (error_message != nullptr)
        {
            *error_message = "Script did not return a table for " + instance.script_path.generic_string();
        }
        return false;
    }

    lua_getfield(lua_state, -1, method_name);
    if (lua_isnil(lua_state, -1))
    {
        lua_pop(lua_state, 2);
        return true;
    }

    if (!lua_isfunction(lua_state, -1))
    {
        lua_pop(lua_state, 2);
        if (error_message != nullptr)
        {
            *error_message = "Script member is not a function: " + std::string(method_name);
        }
        return false;
    }

    lua_pushvalue(lua_state, -2);
    lua_pushstring(lua_state, instance.object_name.c_str());
    lua_pushstring(lua_state, other_object_name.c_str());
    lua_pushstring(lua_state, phase.c_str());

    const std::string previous_instance_key = script_active_instance_key_;
    const std::string previous_object_name = script_active_object_name_;
    script_active_instance_key_ = instance.instance_key;
    script_active_object_name_ = instance.object_name;

    const int call_result = lua_pcall(lua_state, 4, 0, 0);
    script_active_instance_key_ = previous_instance_key;
    script_active_object_name_ = previous_object_name;
    if (call_result != LUA_OK)
    {
        if (error_message != nullptr)
        {
            const char* message = lua_tostring(lua_state, -1);
            *error_message = "Script call failed (" + std::string(method_name) + ") in " + instance.script_path.generic_string() + ": " + (message != nullptr ? message : "unknown error");
        }

        lua_pop(lua_state, 2);
        return false;
    }

    lua_pop(lua_state, 1);
    return true;
}

bool RuntimeRenderer::UpdateScriptsForFrame(std::string* error_message)
{
    if (script_lua_state_ == nullptr)
    {
        return true;
    }

    // Handle pending scene load requested by World.LoadScene
    if (!pending_scene_load_path_.empty())
    {
        std::string load_path = std::move(pending_scene_load_path_);
        pending_scene_load_path_.clear();

        // Resolve path: if it has no extension, append .scene
        std::filesystem::path scene_file = load_path;
        if (!scene_file.has_extension())
        {
            scene_file += ".scene";
        }
        // If not absolute, resolve relative to project Scenes/ directory
        if (scene_file.is_relative())
        {
            scene_file = project_root_ / "Scenes" / scene_file;
        }

        const SceneMetadata new_scene_metadata = LoadSceneMetadata(scene_file);
        const ActiveSceneCameraSelection new_camera = FindActiveSceneCamera(new_scene_metadata);
        if (!new_camera.found)
        {
            if (error_message != nullptr)
            {
                *error_message = "World.LoadScene: no camera found in scene '" + load_path + "'";
            }
            return false;
        }

        // Reset all runtime state for the new scene
        DestroyAllScriptInstances();
        ClearScriptEventSubscriptions();
        ClearScriptTimers();
        runtime_spawned_objects_.clear();
        runtime_destroyed_objects_.clear();
        script_object_position_overrides_.clear();
        script_object_rotation_overrides_.clear();
        script_object_scale_overrides_.clear();
        script_active_instance_key_.clear();
        script_active_object_name_.clear();
        script_prev_keys_down_.clear();
        script_frame_collision_events_.clear();
        queued_objects_.clear();
        script_next_timer_id_ = 1;
        script_timer_pending_clear_.clear();
        physics_world_built_ = false;
        cached_scene_path_.clear();
        cached_scene_metadata_ = SceneMetadata{};
        has_cached_scene_metadata_ = false;
        const std::uint64_t load_now_ms = static_cast<std::uint64_t>(SDL_GetTicks());
        script_last_tick_ms_ = load_now_ms;
        script_session_start_ms_ = load_now_ms;

        scene_path_ = scene_file;
        active_camera_object_name_ = new_camera.object_name;
        active_camera_attribute_index_ = new_camera.attribute_index;
    }

    const std::uint64_t now_ms = static_cast<std::uint64_t>(SDL_GetTicks());
    float delta_time = 0.0f;
    if (script_last_tick_ms_ != 0 && now_ms >= script_last_tick_ms_)
    {
        delta_time = static_cast<float>(now_ms - script_last_tick_ms_) / 1000.0f;
    }
    const float total_time = static_cast<float>(now_ms - script_session_start_ms_) / 1000.0f;
    script_last_tick_ms_ = now_ms;

    // Update Time table fields
    lua_getglobal(script_lua_state_, "Time");
    lua_pushnumber(script_lua_state_, static_cast<lua_Number>(delta_time));
    lua_setfield(script_lua_state_, -2, "DeltaTime");
    lua_pushnumber(script_lua_state_, static_cast<lua_Number>(total_time));
    lua_setfield(script_lua_state_, -2, "TotalTime");
    lua_pop(script_lua_state_, 1);

    // Snapshot current keyboard state for WasKeyPressed
    int num_keys = 0;
    const bool* keys = SDL_GetKeyboardState(&num_keys);
    const std::vector<bool> current_keys(keys, keys + num_keys);

    // Snapshot collisions once so every script sees the same frame data.
    if (physics_world_.IsInitialized())
    {
        script_frame_collision_events_ = physics_world_.ConsumeCollisionEvents();
    }
    else
    {
        script_frame_collision_events_.clear();
    }

    std::unordered_set<std::string> trigger_objects;
    trigger_objects.reserve(cached_scene_metadata_.objects.size());
    for (const SceneObjectMetadata& object : cached_scene_metadata_.objects)
    {
        if (!object.physics_is_trigger)
        {
            continue;
        }

        if (runtime_destroyed_objects_.find(object.name) != runtime_destroyed_objects_.end())
        {
            continue;
        }

        trigger_objects.insert(object.name);
    }

    for (const PhysicsCollisionEvent& collision : script_frame_collision_events_)
    {
        const char* method_name = nullptr;
        if (collision.phase == "enter")
        {
            method_name = "OnTriggerEnter";
        }
        else if (collision.phase == "stay")
        {
            method_name = "OnTriggerStay";
        }
        else if (collision.phase == "exit")
        {
            method_name = "OnTriggerExit";
        }

        if (method_name == nullptr)
        {
            continue;
        }

        const bool a_is_trigger = trigger_objects.find(collision.object_a) != trigger_objects.end();
        const bool b_is_trigger = trigger_objects.find(collision.object_b) != trigger_objects.end();
        if (!a_is_trigger && !b_is_trigger)
        {
            continue;
        }

        for (auto& [key, instance] : script_instances_)
        {
            if (a_is_trigger && instance.object_name == collision.object_a)
            {
                if (!CallScriptTriggerMethod(instance, method_name, collision.object_b, collision.phase, error_message))
                {
                    return false;
                }
            }

            if (b_is_trigger && instance.object_name == collision.object_b)
            {
                if (!CallScriptTriggerMethod(instance, method_name, collision.object_a, collision.phase, error_message))
                {
                    return false;
                }
            }
        }
    }

    for (auto& [key, instance] : script_instances_)
    {
        if (!CallScriptMethod(instance, "OnUpdate", delta_time, true, error_message))
        {
            return false;
        }
    }

    if (!UpdateScriptTimers(delta_time, error_message))
    {
        return false;
    }

    // Save key state for next frame's WasKeyPressed
    script_prev_keys_down_ = current_keys;

    return true;
}

bool RuntimeRenderer::UpdateScriptTimers(float delta_time, std::string* error_message)
{
    if (script_lua_state_ == nullptr || script_timers_.empty())
    {
        return true;
    }

    script_timer_update_in_progress_ = true;
    for (std::size_t i = 0; i < script_timers_.size();)
    {
        ScriptTimer& timer = script_timers_[i];

        if (script_timer_pending_clear_.find(timer.id) != script_timer_pending_clear_.end())
        {
            if (timer.callback_ref != LUA_NOREF && timer.callback_ref != LUA_REFNIL)
            {
                luaL_unref(script_lua_state_, LUA_REGISTRYINDEX, timer.callback_ref);
            }
            script_timers_.erase(script_timers_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }

        const auto owner_it = script_instances_.find(timer.owner_instance_key);
        if (owner_it == script_instances_.end())
        {
            if (timer.callback_ref != LUA_NOREF && timer.callback_ref != LUA_REFNIL)
            {
                luaL_unref(script_lua_state_, LUA_REGISTRYINDEX, timer.callback_ref);
            }
            script_timers_.erase(script_timers_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }

        timer.remaining_seconds -= delta_time;
        if (timer.remaining_seconds > 0.0f)
        {
            ++i;
            continue;
        }

        lua_rawgeti(script_lua_state_, LUA_REGISTRYINDEX, timer.callback_ref);
        if (!lua_isfunction(script_lua_state_, -1))
        {
            lua_pop(script_lua_state_, 1);
            luaL_unref(script_lua_state_, LUA_REGISTRYINDEX, timer.callback_ref);
            script_timers_.erase(script_timers_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }

        const std::string previous_instance_key = script_active_instance_key_;
        const std::string previous_object_name = script_active_object_name_;
        script_active_instance_key_ = owner_it->second.instance_key;
        script_active_object_name_ = owner_it->second.object_name;

        const int call_result = lua_pcall(script_lua_state_, 0, 0, 0);

        script_active_instance_key_ = previous_instance_key;
        script_active_object_name_ = previous_object_name;

        if (call_result != LUA_OK)
        {
            if (error_message != nullptr)
            {
                const char* message = lua_tostring(script_lua_state_, -1);
                *error_message = std::string("Timer callback failed: ") + (message != nullptr ? message : "unknown error");
            }
            lua_pop(script_lua_state_, 1);
            script_timer_update_in_progress_ = false;
            return false;
        }

        if (timer.repeating)
        {
            timer.remaining_seconds = timer.interval_seconds;
            ++i;
            continue;
        }

        luaL_unref(script_lua_state_, LUA_REGISTRYINDEX, timer.callback_ref);
        script_timers_.erase(script_timers_.begin() + static_cast<std::ptrdiff_t>(i));
    }

    script_timer_update_in_progress_ = false;
    script_timer_pending_clear_.clear();
    return true;
}

void RuntimeRenderer::ClearScriptTimers()
{
    if (script_lua_state_ != nullptr)
    {
        for (ScriptTimer& timer : script_timers_)
        {
            if (timer.callback_ref != LUA_NOREF && timer.callback_ref != LUA_REFNIL)
            {
                luaL_unref(script_lua_state_, LUA_REGISTRYINDEX, timer.callback_ref);
                timer.callback_ref = LUA_NOREF;
            }
        }
    }

    script_timers_.clear();
    script_timer_pending_clear_.clear();
    script_timer_update_in_progress_ = false;
}

void RuntimeRenderer::RemoveScriptTimersForInstance(const std::string& instance_key)
{
    for (auto it = script_timers_.begin(); it != script_timers_.end();)
    {
        if (it->owner_instance_key != instance_key)
        {
            ++it;
            continue;
        }

        if (script_timer_update_in_progress_)
        {
            script_timer_pending_clear_.insert(it->id);
            ++it;
            continue;
        }

        if (script_lua_state_ != nullptr && it->callback_ref != LUA_NOREF && it->callback_ref != LUA_REFNIL)
        {
            luaL_unref(script_lua_state_, LUA_REGISTRYINDEX, it->callback_ref);
        }
        it = script_timers_.erase(it);
    }
}

void RuntimeRenderer::ClearScriptEventSubscriptions()
{
    if (script_lua_state_ != nullptr)
    {
        for (auto& [event_name, subscriptions] : script_event_subscriptions_)
        {
            for (ScriptEventSubscription& subscription : subscriptions)
            {
                if (subscription.handler_ref != LUA_NOREF && subscription.handler_ref != LUA_REFNIL)
                {
                    luaL_unref(script_lua_state_, LUA_REGISTRYINDEX, subscription.handler_ref);
                    subscription.handler_ref = LUA_NOREF;
                }
            }
        }
    }

    script_event_subscriptions_.clear();
}

void RuntimeRenderer::RemoveScriptEventSubscriptionsForInstance(const std::string& instance_key)
{
    for (auto subscriptions_it = script_event_subscriptions_.begin(); subscriptions_it != script_event_subscriptions_.end();)
    {
        std::vector<ScriptEventSubscription>& subscriptions = subscriptions_it->second;
        for (auto it = subscriptions.begin(); it != subscriptions.end();)
        {
            if (it->instance_key != instance_key)
            {
                ++it;
                continue;
            }

            if (script_lua_state_ != nullptr && it->handler_ref != LUA_NOREF && it->handler_ref != LUA_REFNIL)
            {
                luaL_unref(script_lua_state_, LUA_REGISTRYINDEX, it->handler_ref);
            }
            it = subscriptions.erase(it);
        }

        if (subscriptions.empty())
        {
            subscriptions_it = script_event_subscriptions_.erase(subscriptions_it);
            continue;
        }

        ++subscriptions_it;
    }
}

bool RuntimeRenderer::SpawnRuntimeObject(const RuntimeSpawnedObject& object, std::string* error_message)
{
    if (object.name.empty())
    {
        if (error_message != nullptr)
        {
            *error_message = "World.Spawn requires a non-empty object name";
        }
        return false;
    }

    if (runtime_spawned_objects_.find(object.name) != runtime_spawned_objects_.end())
    {
        if (error_message != nullptr)
        {
            *error_message = "World.Spawn object already exists: " + object.name;
        }
        return false;
    }

    for (const SceneObjectMetadata& scene_object : cached_scene_metadata_.objects)
    {
        if (scene_object.name == object.name)
        {
            if (error_message != nullptr)
            {
                *error_message = "World.Spawn name conflicts with scene object: " + object.name;
            }
            return false;
        }
    }

    runtime_spawned_objects_[object.name] = object;
    runtime_destroyed_objects_.erase(object.name);
    return true;
}

void RuntimeRenderer::DestroyRuntimeObject(const std::string& object_name)
{
    if (object_name.empty())
    {
        return;
    }

    runtime_spawned_objects_.erase(object_name);
    runtime_destroyed_objects_.insert(object_name);

    script_object_position_overrides_.erase(object_name);
    script_object_rotation_overrides_.erase(object_name);
    script_object_scale_overrides_.erase(object_name);

    std::vector<std::string> instances_to_remove;
    for (const auto& [instance_key, instance] : script_instances_)
    {
        if (instance.object_name == object_name)
        {
            instances_to_remove.push_back(instance_key);
        }
    }

    for (const std::string& instance_key : instances_to_remove)
    {
        auto it = script_instances_.find(instance_key);
        if (it == script_instances_.end())
        {
            continue;
        }

        std::string ignored_error;
        CallScriptMethod(it->second, "OnDestroy", 0.0f, false, &ignored_error);
        RemoveScriptEventSubscriptionsForInstance(it->second.instance_key);
        if (script_lua_state_ != nullptr && it->second.table_ref != LUA_NOREF && it->second.table_ref != LUA_REFNIL)
        {
            luaL_unref(script_lua_state_, LUA_REGISTRYINDEX, it->second.table_ref);
        }
        script_instances_.erase(it);
    }
}

bool RuntimeRenderer::RuntimeObjectExists(const std::string& object_name) const
{
    if (object_name.empty())
    {
        return false;
    }

    if (runtime_destroyed_objects_.find(object_name) != runtime_destroyed_objects_.end())
    {
        return false;
    }

    if (runtime_spawned_objects_.find(object_name) != runtime_spawned_objects_.end())
    {
        return true;
    }

    for (const SceneObjectMetadata& object : cached_scene_metadata_.objects)
    {
        if (object.name == object_name)
        {
            return true;
        }
    }

    return false;
}

void RuntimeRenderer::SetScriptObjectPosition(const std::string& object_name, const SceneVector3& position)
{
    script_object_position_overrides_[object_name] = position;
    for (QueuedSceneObject& object : queued_objects_)
    {
        if (object.name != object_name)
        {
            continue;
        }

        object.model_matrix[12] = position[0];
        object.model_matrix[13] = position[1];
        object.model_matrix[14] = position[2];
    }
}

bool RuntimeRenderer::TryGetScriptObjectPosition(const std::string& object_name, SceneVector3& position) const
{
    for (const QueuedSceneObject& object : queued_objects_)
    {
        if (object.name != object_name)
        {
            continue;
        }

        position = {
            object.model_matrix[12],
            object.model_matrix[13],
            object.model_matrix[14],
        };
        return true;
    }

    const auto override_it = script_object_position_overrides_.find(object_name);
    if (override_it == script_object_position_overrides_.end())
    {
        return false;
    }

    position = override_it->second;
    return true;
}

void RuntimeRenderer::SetScriptObjectRotation(const std::string& object_name, const SceneVector3& rotation)
{
    script_object_rotation_overrides_[object_name] = rotation;
}

bool RuntimeRenderer::TryGetScriptObjectRotation(const std::string& object_name, SceneVector3& rotation) const
{
    const auto override_it = script_object_rotation_overrides_.find(object_name);
    if (override_it != script_object_rotation_overrides_.end())
    {
        rotation = override_it->second;
        return true;
    }

    for (const SceneObjectMetadata& object : cached_scene_metadata_.objects)
    {
        if (object.name != object_name)
        {
            continue;
        }
        rotation = object.rotation;
        return true;
    }
    return false;
}

void RuntimeRenderer::SetScriptObjectScale(const std::string& object_name, const SceneVector3& scale)
{
    script_object_scale_overrides_[object_name] = scale;
}

bool RuntimeRenderer::TryGetScriptObjectScale(const std::string& object_name, SceneVector3& scale) const
{
    const auto override_it = script_object_scale_overrides_.find(object_name);
    if (override_it != script_object_scale_overrides_.end())
    {
        scale = override_it->second;
        return true;
    }

    for (const SceneObjectMetadata& object : cached_scene_metadata_.objects)
    {
        if (object.name != object_name)
        {
            continue;
        }
        scale = object.scale;
        return true;
    }
    return false;
}

SceneObjectAttribute* RuntimeRenderer::FindScriptAttribute(
    const std::string& object_name,
    SceneObjectAttributeKind kind,
    std::size_t occurrence_index)
{
    if (object_name.empty())
    {
        return nullptr;
    }

    for (SceneObjectMetadata& object : cached_scene_metadata_.objects)
    {
        if (object.name != object_name)
        {
            continue;
        }

        std::size_t current_occurrence = 0;
        for (SceneObjectAttribute& attribute : object.attributes)
        {
            if (attribute.kind != kind)
            {
                continue;
            }

            if (current_occurrence == occurrence_index)
            {
                return &attribute;
            }

            ++current_occurrence;
        }

        return nullptr;
    }

    return nullptr;
}

const SceneObjectAttribute* RuntimeRenderer::FindScriptAttribute(
    const std::string& object_name,
    SceneObjectAttributeKind kind,
    std::size_t occurrence_index) const
{
    if (object_name.empty())
    {
        return nullptr;
    }

    for (const SceneObjectMetadata& object : cached_scene_metadata_.objects)
    {
        if (object.name != object_name)
        {
            continue;
        }

        std::size_t current_occurrence = 0;
        for (const SceneObjectAttribute& attribute : object.attributes)
        {
            if (attribute.kind != kind)
            {
                continue;
            }

            if (current_occurrence == occurrence_index)
            {
                return &attribute;
            }

            ++current_occurrence;
        }

        return nullptr;
    }

    return nullptr;
}

void RuntimeRenderer::RefreshActiveScriptCameraSelection()
{
    const ActiveSceneCameraSelection new_camera = FindActiveSceneCamera(cached_scene_metadata_);
    if (!new_camera.found)
    {
        return;
    }

    active_camera_object_name_ = new_camera.object_name;
    active_camera_attribute_index_ = new_camera.attribute_index;
}

void RuntimeRenderer::HandleScriptAttributeMutation(SceneObjectAttributeKind kind, ScriptAttributeAccessorId accessor_id)
{
    switch (kind)
    {
    case SceneObjectAttributeKind::Rigidbody:
    case SceneObjectAttributeKind::TriggerVolume:
        physics_world_built_ = false;
        physics_object_transforms_.clear();
        break;

    case SceneObjectAttributeKind::Camera:
        if (accessor_id == ScriptAttributeAccessorId::CameraActive)
        {
            RefreshActiveScriptCameraSelection();
        }
        break;

    default:
        break;
    }
}

bool RuntimeRenderer::BuildQueuedScene(
    const SceneMetadata& scene_metadata,
    const SceneObjectMetadata& active_camera_object,
    const SceneObjectCameraAttributes& active_camera,
    std::array<float, 16>& view_inverse,
    std::array<float, 16>& projection_inverse,
    ResolvedSceneLighting& lighting,
    std::string* error_message)
{
    queued_objects_.clear();

    const SceneResolvedObjectPoseMap resolved_object_poses = ResolveSceneObjectPoses(scene_metadata);
    const auto camera_pose_it = resolved_object_poses.find(active_camera_object.name);
    if (camera_pose_it == resolved_object_poses.end())
    {
        if (error_message != nullptr)
        {
            *error_message = "Active camera transform could not be resolved";
        }
        return false;
    }

    std::size_t queued_scan_count = 0;
    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        if ((queued_scan_count++ & 31u) == 0u)
        {
            // Prevent Windows from flagging the app as hung during heavy first-frame loads.
            SDL_PumpEvents();
        }

        if (runtime_destroyed_objects_.find(object.name) != runtime_destroyed_objects_.end())
        {
            continue;
        }

        QueuedSceneObject queued_object;
        queued_object.name = object.name;
        queued_object.script_paths.reserve(object.script_paths.size());
        for (const std::string& script_path : object.script_paths)
        {
            if (script_path.empty())
            {
                continue;
            }

            queued_object.script_paths.push_back(project_root_ / script_path);
        }

        bool has_renderable_model = false;
        if (!object.model_path.empty())
        {
            const std::filesystem::path model_path = project_root_ / object.model_path;
            const CachedModelAssetEntry& model_asset_entry = GetModelAssetEntry(model_path);
            if (model_asset_entry.asset.loaded && EnsureMeshCacheEntry(model_path, model_asset_entry))
            {
                queued_object.model_path = model_path;
                has_renderable_model = true;
            }
        }

        if (!has_renderable_model && queued_object.script_paths.empty())
        {
            continue;
        }

        const auto pose_it = resolved_object_poses.find(object.name);
        queued_object.model_visual_offset = object.model_visual_offset;
        if (pose_it != resolved_object_poses.end())
        {
            queued_object.model_matrix = pose_it->second.world_matrix;
        }
        else
        {
            BuildTransformMatrix(object.position, object.rotation, object.scale, queued_object.model_matrix.data());
        }

        const bool has_position_override = script_object_position_overrides_.count(queued_object.name) > 0;
        const bool has_rotation_override = script_object_rotation_overrides_.count(queued_object.name) > 0;
        const bool has_scale_override    = script_object_scale_overrides_.count(queued_object.name) > 0;
        const auto physics_transform_it = physics_object_transforms_.find(queued_object.name);

        if (physics_transform_it != physics_object_transforms_.end())
        {
            BuildTransformMatrixFromPhysicsTransform(
                physics_transform_it->second,
                ExtractScaleFromMatrix(queued_object.model_matrix.data()),
                queued_object.model_matrix.data());
        }
        else if (has_rotation_override || has_scale_override)
        {
            const SceneVector3& pos = has_position_override
                ? script_object_position_overrides_[queued_object.name]
                : object.position;
            const SceneVector3& rot = has_rotation_override
                ? script_object_rotation_overrides_[queued_object.name]
                : object.rotation;
            const SceneVector3& scl = has_scale_override
                ? script_object_scale_overrides_[queued_object.name]
                : object.scale;
            BuildTransformMatrix(pos, rot, scl, queued_object.model_matrix.data());
        }
        else if (has_position_override)
        {
            const auto& pos = script_object_position_overrides_[queued_object.name];
            queued_object.model_matrix[12] = pos[0];
            queued_object.model_matrix[13] = pos[1];
            queued_object.model_matrix[14] = pos[2];
        }

        queued_objects_.push_back(std::move(queued_object));
    }

    for (const auto& [name, spawned] : runtime_spawned_objects_)
    {
        if ((queued_scan_count++ & 31u) == 0u)
        {
            SDL_PumpEvents();
        }

        if (runtime_destroyed_objects_.find(name) != runtime_destroyed_objects_.end())
        {
            continue;
        }

        QueuedSceneObject queued_object;
        queued_object.name = spawned.name;
        queued_object.model_visual_offset = spawned.model_visual_offset;

        if (!spawned.model_path.empty())
        {
            const std::filesystem::path model_path = project_root_ / spawned.model_path;
            const CachedModelAssetEntry& model_asset_entry = GetModelAssetEntry(model_path);
            if (model_asset_entry.asset.loaded && EnsureMeshCacheEntry(model_path, model_asset_entry))
            {
                queued_object.model_path = model_path;
            }
        }

        if (!spawned.script_path.empty())
        {
            queued_object.script_paths.push_back(project_root_ / spawned.script_path);
        }

        BuildTransformMatrix(spawned.position, spawned.rotation, spawned.scale, queued_object.model_matrix.data());

        const auto position_override_it = script_object_position_overrides_.find(queued_object.name);
        if (position_override_it != script_object_position_overrides_.end())
        {
            queued_object.model_matrix[12] = position_override_it->second[0];
            queued_object.model_matrix[13] = position_override_it->second[1];
            queued_object.model_matrix[14] = position_override_it->second[2];
        }

        queued_objects_.push_back(std::move(queued_object));
    }

    for (const QueuedSceneObject& queued_object : queued_objects_)
    {
        for (const std::filesystem::path& script_path : queued_object.script_paths)
        {
            if (!EnsureScriptCacheEntry(script_path, error_message))
            {
                return false;
            }
        }
    }

    if (!SyncScriptInstances(error_message))
    {
        return false;
    }

    lighting = ResolveSceneLighting(scene_metadata, BuildLightingPoseMap(resolved_object_poses), active_camera_object.name);

    const Vec3 camera_position = TransformPoint(camera_pose_it->second.world_matrix.data(), Vec3{0.0f, 0.0f, 0.0f});
    Vec3 camera_forward = TransformDirectionByMatrix(camera_pose_it->second.world_matrix.data(), Vec3{0.0f, 0.0f, -1.0f});
    Vec3 camera_up = TransformDirectionByMatrix(camera_pose_it->second.world_matrix.data(), Vec3{0.0f, 1.0f, 0.0f});
    if (Length(camera_forward) <= 0.0001f)
    {
        camera_forward = Vec3{0.0f, 0.0f, -1.0f};
    }
    if (Length(camera_up) <= 0.0001f || std::abs(Dot(camera_forward, camera_up)) >= 0.999f)
    {
        camera_up = Vec3{0.0f, 1.0f, 0.0f};
    }

    float view_matrix[16];
    float projection_matrix[16];
    BuildLookAtMatrix(camera_position, Add(camera_position, camera_forward), camera_up, view_matrix);
    const float aspect = static_cast<float>(ray_tracing_.GetOutputWidth()) / static_cast<float>(ray_tracing_.GetOutputHeight());
    const float field_of_view = std::clamp(active_camera.field_of_view_degrees, 1.0f, 179.0f);
    const float near_clip = (std::max)(active_camera.near_clip, 0.001f);
    const float far_clip = (std::max)(active_camera.far_clip, near_clip + 0.001f);
    BuildPerspectiveMatrix(field_of_view, aspect, near_clip, far_clip, projection_matrix);
    if (!InvertMatrix(view_matrix, view_inverse.data()) || !InvertMatrix(projection_matrix, projection_inverse.data()))
    {
        if (error_message != nullptr)
        {
            *error_message = "Failed to build active camera matrices";
        }
        return false;
    }

    return true;
}

bool RuntimeRenderer::SyncRayTracingScene(std::string* error_message)
{
    if (!ray_tracing_.IsAvailable())
    {
        return true;
    }

    std::vector<RayTracing::MeshInput> mesh_inputs;
    std::vector<RayTracing::InstanceInput> instance_inputs;
    std::unordered_map<std::string, std::size_t> mesh_index_by_key;
    mesh_inputs.reserve(queued_objects_.size());
    instance_inputs.reserve(queued_objects_.size());

    for (const QueuedSceneObject& object : queued_objects_)
    {
        const auto mesh_entry_it = mesh_cache_.find(object.model_path);
        if (mesh_entry_it == mesh_cache_.end())
        {
            continue;
        }

        const GpuMeshCacheEntry& mesh_entry = mesh_entry_it->second;
        if (mesh_entry.vertex_buffer.device_address == 0 || mesh_entry.index_buffer.device_address == 0 || mesh_entry.vertex_count == 0 || mesh_entry.index_count < 3)
        {
            continue;
        }

        const std::string mesh_key = object.model_path.string();
        if (mesh_index_by_key.find(mesh_key) == mesh_index_by_key.end())
        {
            RayTracing::MeshInput mesh_input;
            mesh_input.key = mesh_key;
            mesh_input.vertex_device_address = mesh_entry.vertex_buffer.device_address;
            mesh_input.index_device_address = mesh_entry.index_buffer.device_address;
            mesh_input.vertex_count = mesh_entry.vertex_count;
            mesh_input.vertex_stride = static_cast<std::uint32_t>(sizeof(SceneGpuVertex));
            mesh_input.index_count = mesh_entry.index_count;
            mesh_input.materials = mesh_entry.materials;
            mesh_input.sections.reserve(mesh_entry.sections.size());
            for (const GpuMeshSection& section : mesh_entry.sections)
            {
                mesh_input.sections.push_back(RayTracing::MeshSectionRecord{
                    section.first_index,
                    section.index_count,
                    section.material_index,
                    section.uses_alpha_transparency});
            }

            mesh_index_by_key.emplace(mesh_key, mesh_inputs.size());
            mesh_inputs.push_back(std::move(mesh_input));
        }

        RayTracing::InstanceInput instance_input;
        instance_input.key = object.name;
        instance_input.mesh_key = mesh_key;
        instance_input.transform = object.model_matrix;
        ApplyLocalModelOffset(instance_input.transform.data(), object.model_visual_offset);
        instance_inputs.push_back(std::move(instance_input));
    }

    if (!ray_tracing_.UpdateScene(mesh_inputs, instance_inputs))
    {
        if (error_message != nullptr)
        {
            *error_message = ray_tracing_.GetStatusMessage();
        }
        return false;
    }

    return true;
}

bool RuntimeRenderer::RenderFrame(std::uint32_t target_width, std::uint32_t target_height, std::string* error_message)
{
    if (vulkan_context_ == nullptr || scene_path_.empty())
    {
        if (error_message != nullptr)
        {
            *error_message = "Runtime renderer session is not initialized";
        }
        return false;
    }

    const SceneMetadata& scene_metadata = GetSceneMetadata();
    if (!scene_metadata.parsed)
    {
        if (error_message != nullptr)
        {
            *error_message = scene_metadata.error_message.empty() ? "Failed to parse play scene" : scene_metadata.error_message;
        }
        return false;
    }

    const auto camera_object_it = std::find_if(scene_metadata.objects.begin(), scene_metadata.objects.end(), [&](const SceneObjectMetadata& object)
    {
        return object.name == active_camera_object_name_;
    });
    if (camera_object_it == scene_metadata.objects.end() || active_camera_attribute_index_ >= camera_object_it->attributes.size())
    {
        if (error_message != nullptr)
        {
            *error_message = "Active camera used for Play no longer exists; restart Play";
        }
        return false;
    }

    const SceneObjectAttribute& camera_attribute = camera_object_it->attributes[active_camera_attribute_index_];
    if (camera_attribute.kind != SceneObjectAttributeKind::Camera)
    {
        if (error_message != nullptr)
        {
            *error_message = "Play camera attribute is no longer a camera; restart Play";
        }
        return false;
    }

    if (!ray_tracing_.EnsureViewportOutput(target_width, target_height))
    {
        if (error_message != nullptr)
        {
            *error_message = ray_tracing_.GetStatusMessage();
        }
        return false;
    }

    std::array<float, 16> view_inverse = {};
    std::array<float, 16> projection_inverse = {};
    ResolvedSceneLighting lighting{};
    if (!BuildQueuedScene(scene_metadata, *camera_object_it, camera_attribute.camera, view_inverse, projection_inverse, lighting, error_message))
    {
        return false;
    }

    if (!SyncRayTracingScene(error_message))
    {
        return false;
    }

    // Build physics world once per session (after the first BuildQueuedScene).
    if (!physics_world_built_ && physics_world_.IsInitialized())
    {
        const SceneResolvedObjectPoseMap resolved_object_poses = ResolveSceneObjectPoses(scene_metadata);
        std::unordered_map<std::string, std::array<float, 16>> world_matrices;
        world_matrices.reserve(scene_metadata.objects.size());
        for (const SceneObjectMetadata& object : scene_metadata.objects)
        {
            const auto pose_it = resolved_object_poses.find(object.name);
            if (pose_it != resolved_object_poses.end())
            {
                world_matrices[object.name] = pose_it->second.world_matrix;
            }
        }
        physics_world_.BuildFromScene(scene_metadata, world_matrices, project_root_);
        physics_world_built_ = true;
    }

    // Step physics and push simulated positions into the position overrides.
    if (physics_world_.IsInitialized())
    {
        const std::uint64_t now_ms = static_cast<std::uint64_t>(SDL_GetTicks());
        const float phys_dt = (script_last_tick_ms_ != 0 && now_ms >= script_last_tick_ms_)
            ? static_cast<float>(now_ms - script_last_tick_ms_) / 1000.0f
            : 0.0f;
        physics_world_.Step(phys_dt);
        physics_object_transforms_ = physics_world_.GetSimulatedTransforms();
        const auto simulated = physics_world_.GetSimulatedPositions();
        for (const auto& [name, pos] : simulated)
        {
            // Physics is authoritative for dynamic body positions.
            SetScriptObjectPosition(name, pos);
        }
    }

    if (!UpdateScriptsForFrame(error_message))
    {
        return false;
    }

    ray_tracing_.SetSkyboxTexture(skybox_renderer_.ResolveSkyboxView(scene_metadata, project_root_));
    ray_tracing_.SetSkyboxRotation(skybox_renderer_.ResolveSkyboxRotationDegrees(scene_metadata));

    if (!ray_tracing_.RenderFrame(
            lighting,
            view_inverse,
            projection_inverse,
            false,
            1.0f,
            0.0f,
            0.0f,
            0.0f))
    {
        if (error_message != nullptr)
        {
            *error_message = ray_tracing_.GetStatusMessage();
        }
        return false;
    }

    scene_2d_renderer_.CompositeOverlay(
        scene_metadata,
        project_root_,
        ray_tracing_.GetOutputImage(),
        ray_tracing_.GetOutputImageView(),
        ray_tracing_.GetOutputWidth(),
        ray_tracing_.GetOutputHeight());

    return true;
}