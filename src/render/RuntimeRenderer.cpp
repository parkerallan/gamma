#include "render/RuntimeRenderer.h"
#include "vfs/AssetVFS.h"

#include <assimp/Importer.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <SDL3/SDL.h>

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace
{
constexpr float kPi = 3.1415926535f;

std::string ToDisplayString(const aiString& value)
{
    return value.length > 0 ? std::string(value.C_Str()) : std::string();
}

float TicksToMilliseconds(std::uint64_t start_ticks, std::uint64_t end_ticks)
{
    if (end_ticks <= start_ticks)
    {
        return 0.0f;
    }

    const std::uint64_t frequency = static_cast<std::uint64_t>(SDL_GetPerformanceFrequency());
    if (frequency == 0)
    {
        return 0.0f;
    }

    const std::uint64_t delta_ticks = end_ticks - start_ticks;
    return static_cast<float>(static_cast<double>(delta_ticks) * 1000.0 / static_cast<double>(frequency));
}

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
    case 5:
        return &material_textures.transmission;
    case 6:
        return &material_textures.specular;
    case 7:
        return &material_textures.specular_color;
    case 8:
        return &material_textures.sheen_color;
    case 9:
        return &material_textures.sheen_roughness;
    case 10:
        return &material_textures.iridescence;
    case 11:
        return &material_textures.iridescence_thickness;
    case 12:
        return &material_textures.volume_thickness;
    case 13:
        return &material_textures.clearcoat;
    case 14:
        return &material_textures.clearcoat_roughness;
    case 15:
        return &material_textures.clearcoat_normal;
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

struct RuntimeSkinInfluence
{
    std::array<int, 4> bone_indices = {-1, -1, -1, -1};
    std::array<float, 4> bone_weights = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct RuntimeSkinnedMeshData
{
    const aiMesh* mesh = nullptr;
    aiMatrix4x4 bind_node_transform;
    std::vector<RuntimeSkinInfluence> influences;
};

struct RuntimeAnimationModelCacheEntry
{
    bool loaded = false;
    std::string error_message;
    std::filesystem::file_time_type write_time{};
    std::unique_ptr<Assimp::Importer> importer;
    const aiScene* scene = nullptr;
    aiMatrix4x4 global_inverse;
    std::unordered_map<std::string, std::size_t> bone_index_by_name;
    std::vector<aiMatrix4x4> bone_offsets;
    std::vector<RuntimeSkinnedMeshData> skinned_meshes;
    // Perf-counter ticks at which the next on-disk timestamp check is allowed.
    // Throttles std::filesystem::last_write_time so it does not stutter the frame.
    std::uint64_t next_disk_check_perf_ticks = 0;
    // Cache of node-name -> aiNodeAnim* per animation, built lazily on first sample.
    // Avoids rebuilding the unordered_map every frame inside SampleClipBoneMatrices.
    std::unordered_map<const aiAnimation*, std::unordered_map<std::string, const aiNodeAnim*>> channels_by_animation;
};

std::unordered_map<std::string, RuntimeAnimationModelCacheEntry> g_runtime_animation_model_cache;
std::unordered_map<std::string, std::vector<aiMatrix4x4>> g_runtime_animator_blend_snapshots;
// Scratch buffers reused across frames during CPU vertex skinning.
// Keyed by model path; cleared only when the runtime tears down.
std::unordered_map<std::string, std::vector<SceneGpuVertex>> g_runtime_skinned_vertex_scratch;

aiMatrix4x4 LerpMatrix(const aiMatrix4x4& from, const aiMatrix4x4& to, float alpha)
{
    aiMatrix4x4 result;
    result.a1 = from.a1 + ((to.a1 - from.a1) * alpha);
    result.a2 = from.a2 + ((to.a2 - from.a2) * alpha);
    result.a3 = from.a3 + ((to.a3 - from.a3) * alpha);
    result.a4 = from.a4 + ((to.a4 - from.a4) * alpha);
    result.b1 = from.b1 + ((to.b1 - from.b1) * alpha);
    result.b2 = from.b2 + ((to.b2 - from.b2) * alpha);
    result.b3 = from.b3 + ((to.b3 - from.b3) * alpha);
    result.b4 = from.b4 + ((to.b4 - from.b4) * alpha);
    result.c1 = from.c1 + ((to.c1 - from.c1) * alpha);
    result.c2 = from.c2 + ((to.c2 - from.c2) * alpha);
    result.c3 = from.c3 + ((to.c3 - from.c3) * alpha);
    result.c4 = from.c4 + ((to.c4 - from.c4) * alpha);
    result.d1 = from.d1 + ((to.d1 - from.d1) * alpha);
    result.d2 = from.d2 + ((to.d2 - from.d2) * alpha);
    result.d3 = from.d3 + ((to.d3 - from.d3) * alpha);
    result.d4 = from.d4 + ((to.d4 - from.d4) * alpha);
    return result;
}

std::string NormalizeAnimationAssimpPath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');

    while (path.rfind("./", 0) == 0)
    {
        path.erase(0, 2);
    }

    if (path.size() > 3 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':' && path[2] == '/')
    {
        path.erase(0, 3);
    }

    if (!path.empty() && path[0] == '/')
    {
        path.erase(0, 1);
    }

    std::string lowered = path;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::toupper(c));
    });

    const std::string marker = "CONTENT/";
    if (lowered.rfind(marker, 0) == 0)
    {
        path = path.substr(marker.size());
    }
    else
    {
        const std::string slash_marker = "/CONTENT/";
        const std::size_t marker_pos = lowered.find(slash_marker);
        if (marker_pos != std::string::npos)
        {
            path = path.substr(marker_pos + slash_marker.size());
        }
    }

    return path;
}

class RuntimePakMemoryIOStream : public Assimp::IOStream
{
public:
    explicit RuntimePakMemoryIOStream(std::vector<std::uint8_t> bytes)
        : bytes_(std::move(bytes))
    {
    }

    size_t Read(void* buffer, size_t size, size_t count) override
    {
        if (size == 0 || count == 0 || cursor_ >= bytes_.size())
        {
            return 0;
        }

        const size_t requested = size * count;
        const size_t available = bytes_.size() - cursor_;
        const size_t to_copy = (std::min)(requested, available);
        std::memcpy(buffer, bytes_.data() + cursor_, to_copy);
        cursor_ += to_copy;
        return to_copy / size;
    }

    size_t Write(const void*, size_t, size_t) override
    {
        return 0;
    }

    aiReturn Seek(size_t offset, aiOrigin origin) override
    {
        size_t new_cursor = cursor_;
        if (origin == aiOrigin_SET)
        {
            new_cursor = offset;
        }
        else if (origin == aiOrigin_CUR)
        {
            new_cursor = cursor_ + offset;
        }
        else if (origin == aiOrigin_END)
        {
            if (offset > bytes_.size())
            {
                return aiReturn_FAILURE;
            }
            new_cursor = bytes_.size() - offset;
        }

        if (new_cursor > bytes_.size())
        {
            return aiReturn_FAILURE;
        }

        cursor_ = new_cursor;
        return aiReturn_SUCCESS;
    }

    size_t Tell() const override
    {
        return cursor_;
    }

    size_t FileSize() const override
    {
        return bytes_.size();
    }

    void Flush() override {}

private:
    std::vector<std::uint8_t> bytes_;
    size_t cursor_ = 0;
};

class RuntimePakAssetIOSystem : public Assimp::IOSystem
{
public:
    bool Exists(const char* file) const override
    {
        if (!g_asset_reader || file == nullptr)
        {
            return false;
        }

        return g_asset_reader->FileExists(ResolvePath(file));
    }

    char getOsSeparator() const override
    {
        return '/';
    }

    Assimp::IOStream* Open(const char* file, const char* mode = "rb") override
    {
        if (!g_asset_reader || file == nullptr)
        {
            return nullptr;
        }

        if (mode != nullptr && mode[0] != 'r')
        {
            return nullptr;
        }

        const std::string resolved = ResolvePath(file);
        std::vector<std::uint8_t> bytes = g_asset_reader->ReadFile(resolved);
        if (bytes.empty())
        {
            return nullptr;
        }

        return new RuntimePakMemoryIOStream(std::move(bytes));
    }

    void Close(Assimp::IOStream* file) override
    {
        delete file;
    }

private:
    static std::string ResolvePath(const std::string& raw_path)
    {
        const std::string normalized = NormalizeAnimationAssimpPath(raw_path);
        if (!cwd_.empty())
        {
            const std::string combined = NormalizeAnimationAssimpPath(cwd_ + "/" + normalized);
            if (g_asset_reader && g_asset_reader->FileExists(combined))
            {
                return combined;
            }
        }

        std::filesystem::path path_obj(normalized);
        cwd_ = NormalizeAnimationAssimpPath(path_obj.parent_path().generic_string());
        return normalized;
    }

    static std::string cwd_;
};

std::string RuntimePakAssetIOSystem::cwd_;

aiMatrix4x4 ComposeTransform(const aiVector3D& scale, const aiQuaternion& rotation, const aiVector3D& translation)
{
    aiMatrix4x4 scale_matrix;
    aiMatrix4x4::Scaling(scale, scale_matrix);

    aiMatrix4x4 rotation_matrix(rotation.GetMatrix());

    aiMatrix4x4 translation_matrix;
    aiMatrix4x4::Translation(translation, translation_matrix);

    return translation_matrix * rotation_matrix * scale_matrix;
}

std::array<float, 2> ApplyUvTransformLocal(const std::array<float, 2>& uv, const ModelTextureTransform& transform)
{
    if (!transform.valid)
    {
        return uv;
    }

    const float sin_rotation = std::sin(transform.rotation);
    const float cos_rotation = std::cos(transform.rotation);
    const float scaled_x = uv[0] * transform.scale[0];
    const float scaled_y = uv[1] * transform.scale[1];
    return {
        (scaled_x * cos_rotation) - (scaled_y * sin_rotation) + transform.translation[0],
        (scaled_x * sin_rotation) + (scaled_y * cos_rotation) + transform.translation[1],
    };
}

template <typename TKey>
std::size_t FindKeyframeIndex(double time, unsigned int key_count, const TKey* keys)
{
    if (key_count <= 1)
    {
        return 0;
    }

    for (unsigned int index = 0; index + 1 < key_count; ++index)
    {
        if (time < keys[index + 1].mTime)
        {
            return index;
        }
    }

    return key_count - 2;
}

aiVector3D InterpolatePosition(double animation_time, const aiNodeAnim* channel)
{
    if (channel == nullptr || channel->mNumPositionKeys == 0)
    {
        return aiVector3D(0.0f, 0.0f, 0.0f);
    }
    if (channel->mNumPositionKeys == 1)
    {
        return channel->mPositionKeys[0].mValue;
    }

    const std::size_t index = FindKeyframeIndex(animation_time, channel->mNumPositionKeys, channel->mPositionKeys);
    const std::size_t next_index = index + 1;
    const double delta = channel->mPositionKeys[next_index].mTime - channel->mPositionKeys[index].mTime;
    const double factor = delta > 0.0 ? (animation_time - channel->mPositionKeys[index].mTime) / delta : 0.0;
    return channel->mPositionKeys[index].mValue + static_cast<float>(factor) * (channel->mPositionKeys[next_index].mValue - channel->mPositionKeys[index].mValue);
}

aiVector3D InterpolateScale(double animation_time, const aiNodeAnim* channel)
{
    if (channel == nullptr || channel->mNumScalingKeys == 0)
    {
        return aiVector3D(1.0f, 1.0f, 1.0f);
    }
    if (channel->mNumScalingKeys == 1)
    {
        return channel->mScalingKeys[0].mValue;
    }

    const std::size_t index = FindKeyframeIndex(animation_time, channel->mNumScalingKeys, channel->mScalingKeys);
    const std::size_t next_index = index + 1;
    const double delta = channel->mScalingKeys[next_index].mTime - channel->mScalingKeys[index].mTime;
    const double factor = delta > 0.0 ? (animation_time - channel->mScalingKeys[index].mTime) / delta : 0.0;
    return channel->mScalingKeys[index].mValue + static_cast<float>(factor) * (channel->mScalingKeys[next_index].mValue - channel->mScalingKeys[index].mValue);
}

aiQuaternion InterpolateRotation(double animation_time, const aiNodeAnim* channel)
{
    if (channel == nullptr || channel->mNumRotationKeys == 0)
    {
        return aiQuaternion();
    }
    if (channel->mNumRotationKeys == 1)
    {
        return channel->mRotationKeys[0].mValue;
    }

    const std::size_t index = FindKeyframeIndex(animation_time, channel->mNumRotationKeys, channel->mRotationKeys);
    const std::size_t next_index = index + 1;
    const double delta = channel->mRotationKeys[next_index].mTime - channel->mRotationKeys[index].mTime;
    const double factor = delta > 0.0 ? (animation_time - channel->mRotationKeys[index].mTime) / delta : 0.0;

    aiQuaternion out;
    aiQuaternion::Interpolate(out, channel->mRotationKeys[index].mValue, channel->mRotationKeys[next_index].mValue, static_cast<float>(factor));
    out.Normalize();
    return out;
}

void BuildSkinnedMeshList(
    const aiScene* scene,
    const aiNode* node,
    const aiMatrix4x4& parent_transform,
    RuntimeAnimationModelCacheEntry& cache_entry)
{
    if (scene == nullptr || node == nullptr)
    {
        return;
    }

    const aiMatrix4x4 node_transform = parent_transform * node->mTransformation;
    for (unsigned int node_mesh_index = 0; node_mesh_index < node->mNumMeshes; ++node_mesh_index)
    {
        const unsigned int mesh_index = node->mMeshes[node_mesh_index];
        if (mesh_index >= scene->mNumMeshes)
        {
            continue;
        }

        const aiMesh* mesh = scene->mMeshes[mesh_index];
        if (mesh == nullptr)
        {
            continue;
        }

        RuntimeSkinnedMeshData mesh_data;
        mesh_data.mesh = mesh;
        mesh_data.bind_node_transform = node_transform;
        mesh_data.influences.resize(mesh->mNumVertices);

        for (unsigned int bone_index = 0; bone_index < mesh->mNumBones; ++bone_index)
        {
            const aiBone* bone = mesh->mBones[bone_index];
            if (bone == nullptr)
            {
                continue;
            }

            const std::string bone_name = ToDisplayString(bone->mName);
            if (bone_name.empty())
            {
                continue;
            }

            std::size_t runtime_bone_index = 0;
            const auto existing = cache_entry.bone_index_by_name.find(bone_name);
            if (existing == cache_entry.bone_index_by_name.end())
            {
                runtime_bone_index = cache_entry.bone_offsets.size();
                cache_entry.bone_index_by_name.emplace(bone_name, runtime_bone_index);
                cache_entry.bone_offsets.push_back(bone->mOffsetMatrix);
            }
            else
            {
                runtime_bone_index = existing->second;
            }

            for (unsigned int weight_index = 0; weight_index < bone->mNumWeights; ++weight_index)
            {
                const aiVertexWeight& weight = bone->mWeights[weight_index];
                if (weight.mVertexId >= mesh_data.influences.size() || weight.mWeight <= 0.0f)
                {
                    continue;
                }

                RuntimeSkinInfluence& influence = mesh_data.influences[weight.mVertexId];
                int slot = -1;
                for (int i = 0; i < 4; ++i)
                {
                    if (influence.bone_indices[i] == -1)
                    {
                        slot = i;
                        break;
                    }
                }

                if (slot == -1)
                {
                    int weakest_slot = 0;
                    for (int i = 1; i < 4; ++i)
                    {
                        if (influence.bone_weights[i] < influence.bone_weights[weakest_slot])
                        {
                            weakest_slot = i;
                        }
                    }
                    if (weight.mWeight > influence.bone_weights[weakest_slot])
                    {
                        slot = weakest_slot;
                    }
                }

                if (slot >= 0)
                {
                    influence.bone_indices[slot] = static_cast<int>(runtime_bone_index);
                    influence.bone_weights[slot] = weight.mWeight;
                }
            }
        }

        for (RuntimeSkinInfluence& influence : mesh_data.influences)
        {
            float sum = 0.0f;
            for (float weight : influence.bone_weights)
            {
                sum += weight;
            }

            if (sum > 0.0001f)
            {
                const float inv = 1.0f / sum;
                for (float& weight : influence.bone_weights)
                {
                    weight *= inv;
                }
            }
        }

        cache_entry.skinned_meshes.push_back(std::move(mesh_data));
    }

    for (unsigned int child_index = 0; child_index < node->mNumChildren; ++child_index)
    {
        BuildSkinnedMeshList(scene, node->mChildren[child_index], node_transform, cache_entry);
    }
}

void EvaluateAnimationHierarchy(
    const aiAnimation* animation,
    double animation_time,
    const aiNode* node,
    const aiMatrix4x4& parent_transform,
    const RuntimeAnimationModelCacheEntry& cache_entry,
    const std::unordered_map<std::string, const aiNodeAnim*>& channels_by_name,
    std::vector<aiMatrix4x4>& out_bone_matrices)
{
    aiMatrix4x4 node_transform = node->mTransformation;
    const auto channel_it = channels_by_name.find(ToDisplayString(node->mName));
    if (channel_it != channels_by_name.end())
    {
        const aiNodeAnim* channel = channel_it->second;
        const aiVector3D scale = InterpolateScale(animation_time, channel);
        const aiQuaternion rotation = InterpolateRotation(animation_time, channel);
        const aiVector3D translation = InterpolatePosition(animation_time, channel);
        node_transform = ComposeTransform(scale, rotation, translation);
    }

    const aiMatrix4x4 global_transform = parent_transform * node_transform;
    const auto bone_it = cache_entry.bone_index_by_name.find(ToDisplayString(node->mName));
    if (bone_it != cache_entry.bone_index_by_name.end() && bone_it->second < out_bone_matrices.size())
    {
        out_bone_matrices[bone_it->second] = cache_entry.global_inverse * global_transform * cache_entry.bone_offsets[bone_it->second];
    }

    for (unsigned int child_index = 0; child_index < node->mNumChildren; ++child_index)
    {
        EvaluateAnimationHierarchy(animation, animation_time, node->mChildren[child_index], global_transform, cache_entry, channels_by_name, out_bone_matrices);
    }
}

RuntimeAnimationModelCacheEntry* GetRuntimeAnimationModelCacheEntry(const std::filesystem::path& model_path)
{
    RuntimeAnimationModelCacheEntry& cache_entry = g_runtime_animation_model_cache[model_path.generic_string()];

    // Disk-timestamp checks are intentionally NOT performed during play mode.
    // On Windows, std::filesystem::last_write_time() on a multi-MB asset can
    // trigger Defender real-time scanning (tens of ms hitch). Doing it every
    // 500ms produced a regular periodic stutter visible only in the editor's
    // play-mode (the standalone game serves the asset from the pak archive
    // and never hits the filesystem here, which is why the game build is
    // smooth). Hot-reload-during-play is not a supported feature; assets are
    // loaded once and reused for the duration of the play session.
    bool should_reload = !cache_entry.loaded;
    std::filesystem::file_time_type write_time{};
    bool has_filesystem_time = false;

    if (should_reload)
    {
        cache_entry = {};
        cache_entry.importer = std::make_unique<Assimp::Importer>();
        const unsigned int import_flags =
            aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_ImproveCacheLocality |
            aiProcess_CalcTangentSpace |
            aiProcess_GenSmoothNormals |
            aiProcess_ValidateDataStructure |
            aiProcess_SortByPType;

        const bool use_pak = g_asset_reader != nullptr && g_asset_reader->FileExists(model_path.generic_string());
        const std::string model_load_path = use_pak
            ? NormalizeAnimationAssimpPath(model_path.generic_string())
            : model_path.string();
        if (use_pak)
        {
            cache_entry.importer->SetIOHandler(new RuntimePakAssetIOSystem());
        }

        cache_entry.scene = cache_entry.importer->ReadFile(model_load_path, import_flags);
        if (cache_entry.scene == nullptr || cache_entry.scene->mRootNode == nullptr)
        {
            cache_entry.error_message = cache_entry.importer->GetErrorString();
            if (cache_entry.error_message.empty())
            {
                cache_entry.error_message = "Failed to load animation source model";
            }
            return nullptr;
        }

        cache_entry.loaded = true;
        cache_entry.write_time = has_filesystem_time ? write_time : std::filesystem::file_time_type::min();
        cache_entry.global_inverse = cache_entry.scene->mRootNode->mTransformation;
        cache_entry.global_inverse.Inverse();
        BuildSkinnedMeshList(cache_entry.scene, cache_entry.scene->mRootNode, aiMatrix4x4(), cache_entry);
    }

    return cache_entry.loaded ? &cache_entry : nullptr;
}

const aiAnimation* FindAnimationByName(const aiScene* scene, const std::string& clip_name)
{
    if (scene == nullptr || scene->mNumAnimations == 0)
    {
        return nullptr;
    }

    if (!clip_name.empty())
    {
        for (unsigned int index = 0; index < scene->mNumAnimations; ++index)
        {
            const aiAnimation* animation = scene->mAnimations[index];
            if (animation != nullptr && ToDisplayString(animation->mName) == clip_name)
            {
                return animation;
            }
        }
    }

    return scene->mAnimations[0];
}

bool SampleClipBoneMatrices(
    RuntimeAnimationModelCacheEntry& anim_cache_entry,
    const std::string& clip_name,
    float state_time_seconds,
    std::vector<aiMatrix4x4>& out_bone_matrices)
{
    if (anim_cache_entry.scene == nullptr)
    {
        return false;
    }

    const aiAnimation* const animation = FindAnimationByName(anim_cache_entry.scene, clip_name);
    if (animation == nullptr)
    {
        return false;
    }

    const double ticks_per_second = animation->mTicksPerSecond > 0.0 ? animation->mTicksPerSecond : 25.0;
    const double duration = animation->mDuration > 0.0 ? animation->mDuration : 1.0;
    const double raw_time_ticks = static_cast<double>(state_time_seconds) * ticks_per_second;
    const double animation_time = std::fmod(raw_time_ticks, duration);

    // Lazily build (and cache) the channel-by-node-name lookup for this animation.
    // Previously this map was rebuilt every frame.
    auto channels_it = anim_cache_entry.channels_by_animation.find(animation);
    if (channels_it == anim_cache_entry.channels_by_animation.end())
    {
        std::unordered_map<std::string, const aiNodeAnim*> channels_by_name;
        channels_by_name.reserve(animation->mNumChannels);
        for (unsigned int channel_index = 0; channel_index < animation->mNumChannels; ++channel_index)
        {
            const aiNodeAnim* channel = animation->mChannels[channel_index];
            if (channel != nullptr)
            {
                channels_by_name.emplace(ToDisplayString(channel->mNodeName), channel);
            }
        }
        channels_it = anim_cache_entry.channels_by_animation.emplace(animation, std::move(channels_by_name)).first;
    }

    out_bone_matrices.assign(anim_cache_entry.bone_offsets.size(), aiMatrix4x4());
    EvaluateAnimationHierarchy(
        animation,
        animation_time,
        anim_cache_entry.scene->mRootNode,
        aiMatrix4x4(),
        anim_cache_entry,
        channels_it->second,
        out_bone_matrices);
    return true;
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
        ReleaseTexture(textures.transmission);
        ReleaseTexture(textures.specular);
        ReleaseTexture(textures.specular_color);
        ReleaseTexture(textures.sheen_color);
        ReleaseTexture(textures.sheen_roughness);
        ReleaseTexture(textures.iridescence);
        ReleaseTexture(textures.iridescence_thickness);
        ReleaseTexture(textures.volume_thickness);
        ReleaseTexture(textures.clearcoat);
        ReleaseTexture(textures.clearcoat_roughness);
        ReleaseTexture(textures.clearcoat_normal);
    }

    entry = {};
}

namespace
{

std::filesystem::path ResolveSkinningShaderPath(const char* file_name)
{
    const char* base_path_raw = SDL_GetBasePath();
    const std::filesystem::path base_path =
        base_path_raw != nullptr ? std::filesystem::path(base_path_raw) : std::filesystem::current_path();
    return base_path / "shaders" / file_name;
}

VkShaderModule LoadSkinningShaderModule(VkDevice device, const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        SDL_Log("Failed to read GPU skinning shader file: %s", path.string().c_str());
        return VK_NULL_HANDLE;
    }
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (bytes.empty())
    {
        SDL_Log("GPU skinning shader file is empty: %s", path.string().c_str());
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = bytes.size();
    create_info.pCode = reinterpret_cast<const std::uint32_t*>(bytes.data());

    VkShaderModule shader = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(device, &create_info, nullptr, &shader);
    VulkanContext::CheckVkResult(result);
    return result == VK_SUCCESS ? shader : VK_NULL_HANDLE;
}

} // namespace

bool RuntimeRenderer::EnsureSkinningPipeline()
{
    if (vulkan_context_ == nullptr)
    {
        return false;
    }

    VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    if (skinning_descriptor_set_layout_ == VK_NULL_HANDLE)
    {
        std::array<VkDescriptorSetLayoutBinding, 4> bindings = {};
        for (std::uint32_t i = 0; i < bindings.size(); ++i)
        {
            bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo layout_info = {};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        VkResult result = vkCreateDescriptorSetLayout(device, &layout_info, allocator, &skinning_descriptor_set_layout_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            return false;
        }
    }

    if (skinning_pipeline_layout_ == VK_NULL_HANDLE)
    {
        VkPushConstantRange push = {};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.offset = 0;
        push.size = 2 * sizeof(std::uint32_t);

        VkPipelineLayoutCreateInfo layout_info = {};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &skinning_descriptor_set_layout_;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        VkResult result = vkCreatePipelineLayout(device, &layout_info, allocator, &skinning_pipeline_layout_);
        VulkanContext::CheckVkResult(result);
        if (result != VK_SUCCESS)
        {
            return false;
        }
    }

    if (skinning_pipeline_ == VK_NULL_HANDLE)
    {
        VkShaderModule shader = LoadSkinningShaderModule(device, ResolveSkinningShaderPath("skinning.comp.spv"));
        if (shader == VK_NULL_HANDLE)
        {
            return false;
        }

        VkComputePipelineCreateInfo compute_info = {};
        compute_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        compute_info.layout = skinning_pipeline_layout_;
        compute_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute_info.stage.module = shader;
        compute_info.stage.pName = "main";

        VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute_info, allocator, &skinning_pipeline_);
        VulkanContext::CheckVkResult(result);
        vkDestroyShaderModule(device, shader, allocator);
        if (result != VK_SUCCESS)
        {
            skinning_pipeline_ = VK_NULL_HANDLE;
            return false;
        }
    }

    return true;
}

void RuntimeRenderer::DestroySkinningPipeline()
{
    if (vulkan_context_ == nullptr)
    {
        skinning_pipeline_ = VK_NULL_HANDLE;
        skinning_pipeline_layout_ = VK_NULL_HANDLE;
        skinning_descriptor_set_layout_ = VK_NULL_HANDLE;
        return;
    }
    VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    if (skinning_pipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, skinning_pipeline_, allocator);
        skinning_pipeline_ = VK_NULL_HANDLE;
    }
    if (skinning_pipeline_layout_ != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device, skinning_pipeline_layout_, allocator);
        skinning_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (skinning_descriptor_set_layout_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device, skinning_descriptor_set_layout_, allocator);
        skinning_descriptor_set_layout_ = VK_NULL_HANDLE;
    }
}

void RuntimeRenderer::ReleaseSkinningResources(GpuSkinningResources& resources)
{
    ReleaseBuffer(resources.bind_pose_buffer);
    ReleaseBuffer(resources.influence_buffer);
    ReleaseBuffer(resources.palette_buffer);
    // Descriptor sets are freed implicitly when the pool is reset/destroyed.
    resources.descriptor_set = VK_NULL_HANDLE;
    resources.ready = false;
    resources.vertex_count = 0;
    resources.bone_count = 0;
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

    for (auto& [path, resources] : gpu_skinning_resources_)
    {
        ReleaseSkinningResources(resources);
    }
    gpu_skinning_resources_.clear();
    DestroySkinningPipeline();

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
    runtime_animator_states_.clear();
    g_runtime_animator_blend_snapshots.clear();
    animator_controller_cache_.clear();
    animated_mesh_revisions_.clear();
    script_active_instance_key_.clear();
    script_active_object_name_.clear();
    script_prev_keys_down_.clear();
    script_frame_collision_events_.clear();
    animation_last_tick_ms_ = 0;
    model_asset_cache_.clear();
    queued_objects_.clear();
    cached_scene_path_.clear();
    cached_scene_metadata_ = SceneMetadata{};
    has_cached_scene_metadata_ = false;
    project_root_.clear();
    scene_path_.clear();
    active_camera_object_name_.clear();
    active_camera_attribute_index_ = 0;
    performance_stats_ = RuntimePerformanceStats{};
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
    animation_last_tick_ms_ = now_ms;
    animation_last_perf_ticks_ = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
    script_last_tick_ms_ = now_ms;
    script_session_start_ms_ = now_ms;
    runtime_animator_states_.clear();
    g_runtime_animator_blend_snapshots.clear();
    animator_controller_cache_.clear();
    animated_mesh_revisions_.clear();
    physics_world_built_ = false;
    performance_stats_ = RuntimePerformanceStats{};
    return true;
}

void RuntimeRenderer::UpdateAnimatorControllersForFrame(const SceneMetadata& scene_metadata)
{
    // Use the high-resolution monotonic counter for animation delta time.
    // SDL_GetTicks() has ~1ms quantization which produced visible jitter in the
    // animated state-machine timing.
    const std::uint64_t now_perf_ticks = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
    const std::uint64_t perf_freq = static_cast<std::uint64_t>(SDL_GetPerformanceFrequency());
    float delta_time = 0.0f;
    if (animation_last_perf_ticks_ != 0 && now_perf_ticks > animation_last_perf_ticks_ && perf_freq > 0)
    {
        delta_time = static_cast<float>(
            static_cast<double>(now_perf_ticks - animation_last_perf_ticks_) /
            static_cast<double>(perf_freq));
    }
    animation_last_perf_ticks_ = now_perf_ticks;
    // Keep the millisecond clock in sync for any other consumers.
    animation_last_tick_ms_ = static_cast<std::uint64_t>(SDL_GetTicks());

    auto load_controller = [&](const std::filesystem::path& controller_path) -> const AnimatorControllerAsset*
    {
        if (controller_path.empty())
        {
            return nullptr;
        }

        // No periodic disk-timestamp checks during play. See the matching
        // comment in GetRuntimeAnimationModelCacheEntry — Defender scanning of
        // the .anim file every 500 ms produces a periodic editor-only stutter.
        CachedAnimatorControllerEntry& cache_entry = animator_controller_cache_[controller_path];

        bool should_reload = !cache_entry.loaded;
        if (should_reload)
        {
            std::error_code error;
            const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(controller_path, error);
            const bool has_filesystem_time = !error;
            std::string load_error;
            AnimatorControllerAsset asset;
            cache_entry.loaded = LoadAnimatorControllerAsset(controller_path, asset, load_error);
            if (cache_entry.loaded)
            {
                cache_entry.asset = std::move(asset);
                cache_entry.write_time = has_filesystem_time ? write_time : std::filesystem::file_time_type::min();
            }
        }

        return cache_entry.loaded ? &cache_entry.asset : nullptr;
    };

    auto find_state = [](const AnimatorControllerAsset& controller, const std::string& state_name) -> const AnimatorStateDefinition*
    {
        const auto it = std::find_if(controller.states.begin(), controller.states.end(), [&](const AnimatorStateDefinition& state)
        {
            return state.name == state_name;
        });
        return it != controller.states.end() ? &(*it) : nullptr;
    };

    auto trim = [](std::string value) -> std::string
    {
        const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch)
        {
            return std::isspace(ch) != 0;
        });
        if (first == value.end())
        {
            return {};
        }

        const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch)
        {
            return std::isspace(ch) != 0;
        }).base();
        return std::string(first, last);
    };

    auto evaluate_condition = [&](RuntimeAnimatorState& runtime_state, const std::string& condition, std::string* consumed_trigger) -> bool
    {
        if (consumed_trigger != nullptr)
        {
            consumed_trigger->clear();
        }

        const std::string expr = trim(condition);
        if (expr.empty())
        {
            return true;
        }

        const auto parse_bool_literal = [](const std::string& token, bool& value) -> bool
        {
            if (token == "true" || token == "True" || token == "TRUE")
            {
                value = true;
                return true;
            }
            if (token == "false" || token == "False" || token == "FALSE")
            {
                value = false;
                return true;
            }
            return false;
        };

        const char* operators[] = {"==", "!=", ">=", "<=", ">", "<"};
        for (const char* op : operators)
        {
            const std::size_t op_pos = expr.find(op);
            if (op_pos == std::string::npos)
            {
                continue;
            }

            const std::string parameter_name = trim(expr.substr(0, op_pos));
            const std::string right = trim(expr.substr(op_pos + std::strlen(op)));
            if (parameter_name.empty() || right.empty())
            {
                return false;
            }

            const auto bool_it = runtime_state.bool_parameters.find(parameter_name);
            if (bool_it != runtime_state.bool_parameters.end())
            {
                bool rhs_bool = false;
                if (!parse_bool_literal(right, rhs_bool))
                {
                    return false;
                }

                if (std::strcmp(op, "==") == 0)
                {
                    return bool_it->second == rhs_bool;
                }
                if (std::strcmp(op, "!=") == 0)
                {
                    return bool_it->second != rhs_bool;
                }
                return false;
            }

            const auto float_it = runtime_state.float_parameters.find(parameter_name);
            if (float_it == runtime_state.float_parameters.end())
            {
                return false;
            }

            char* parse_end = nullptr;
            const float rhs_value = std::strtof(right.c_str(), &parse_end);
            if (parse_end == right.c_str() || *parse_end != '\0')
            {
                return false;
            }

            const float lhs_value = float_it->second;
            if (std::strcmp(op, "==") == 0)
            {
                return std::abs(lhs_value - rhs_value) <= 0.0001f;
            }
            if (std::strcmp(op, "!=") == 0)
            {
                return std::abs(lhs_value - rhs_value) > 0.0001f;
            }
            if (std::strcmp(op, ">=") == 0)
            {
                return lhs_value >= rhs_value;
            }
            if (std::strcmp(op, "<=") == 0)
            {
                return lhs_value <= rhs_value;
            }
            if (std::strcmp(op, ">") == 0)
            {
                return lhs_value > rhs_value;
            }
            return lhs_value < rhs_value;
        }

        if (expr.front() == '!')
        {
            const std::string name = trim(expr.substr(1));
            if (name.empty())
            {
                return false;
            }

            const auto bool_it = runtime_state.bool_parameters.find(name);
            if (bool_it != runtime_state.bool_parameters.end())
            {
                return !bool_it->second;
            }

            return runtime_state.triggers.find(name) == runtime_state.triggers.end();
        }

        const auto bool_it = runtime_state.bool_parameters.find(expr);
        if (bool_it != runtime_state.bool_parameters.end())
        {
            return bool_it->second;
        }

        if (runtime_state.triggers.find(expr) != runtime_state.triggers.end())
        {
            if (consumed_trigger != nullptr)
            {
                *consumed_trigger = expr;
            }
            return true;
        }

        return false;
    };

    auto capture_blended_pose_snapshot = [&](const RuntimeAnimatorState& runtime_state, std::vector<aiMatrix4x4>& out_snapshot) -> bool
    {
        if (runtime_state.previous_clip_name.empty() ||
            runtime_state.active_clip_name.empty() ||
            runtime_state.previous_clip_source_model_path.empty() ||
            runtime_state.active_clip_source_model_path.empty() ||
            runtime_state.blend_duration_seconds <= 0.0f ||
            runtime_state.blend_time_remaining_seconds <= 0.0f)
        {
            return false;
        }

        const auto resolve_clip_path = [&](const std::string& clip_source_model_path) -> std::filesystem::path
        {
            return std::filesystem::path(clip_source_model_path).is_absolute()
                ? std::filesystem::path(clip_source_model_path)
                : (project_root_ / clip_source_model_path);
        };

        RuntimeAnimationModelCacheEntry* const previous_cache = GetRuntimeAnimationModelCacheEntry(resolve_clip_path(runtime_state.previous_clip_source_model_path));
        RuntimeAnimationModelCacheEntry* const active_cache = GetRuntimeAnimationModelCacheEntry(resolve_clip_path(runtime_state.active_clip_source_model_path));
        if (previous_cache == nullptr || previous_cache->scene == nullptr || active_cache == nullptr || active_cache->scene == nullptr)
        {
            return false;
        }

        std::vector<aiMatrix4x4> previous_matrices;
        std::vector<aiMatrix4x4> active_matrices;
        if (!SampleClipBoneMatrices(*previous_cache, runtime_state.previous_clip_name, runtime_state.previous_state_time_seconds, previous_matrices) ||
            !SampleClipBoneMatrices(*active_cache, runtime_state.active_clip_name, runtime_state.state_time_seconds, active_matrices) ||
            previous_matrices.size() != active_matrices.size())
        {
            return false;
        }

        const float blend_alpha = 1.0f - std::clamp(
            runtime_state.blend_time_remaining_seconds / (std::max)(0.0001f, runtime_state.blend_duration_seconds),
            0.0f,
            1.0f);
        out_snapshot.resize(previous_matrices.size());
        for (std::size_t matrix_index = 0; matrix_index < previous_matrices.size(); ++matrix_index)
        {
            out_snapshot[matrix_index] = LerpMatrix(previous_matrices[matrix_index], active_matrices[matrix_index], blend_alpha);
        }

        return true;
    };

    std::unordered_set<std::string> live_keys;

    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        for (std::size_t attribute_index = 0; attribute_index < object.attributes.size(); ++attribute_index)
        {
            const SceneObjectAttribute& attribute = object.attributes[attribute_index];
            if (attribute.kind != SceneObjectAttributeKind::Animator || attribute.animator.controller_path.empty())
            {
                continue;
            }

            const std::filesystem::path controller_path = std::filesystem::path(attribute.animator.controller_path).is_absolute()
                ? std::filesystem::path(attribute.animator.controller_path)
                : (project_root_ / attribute.animator.controller_path);
            const AnimatorControllerAsset* controller = load_controller(controller_path);
            if (controller == nullptr || controller->states.empty())
            {
                continue;
            }

            const std::string runtime_key = object.name + "#" + std::to_string(attribute_index);
            live_keys.insert(runtime_key);

            RuntimeAnimatorState& runtime_state = runtime_animator_states_[runtime_key];
            runtime_state.runtime_key = runtime_key;
            const bool controller_changed = runtime_state.controller_path != attribute.animator.controller_path;

            // Hot-reload of the controller's .anim file during play is not a
            // supported feature — and `last_write_time` per-animator-per-frame
            // is a measurable Defender-driven stutter source on Windows. Only
            // react to a logical scene-side change of controller_path.
            const bool controller_reloaded = false;

            if (controller_changed || controller_reloaded)
            {
                runtime_state.controller_path = attribute.animator.controller_path;
                runtime_state.controller_write_time = std::filesystem::file_time_type::min();
                runtime_state.active_state.clear();
                runtime_state.active_clip_name.clear();
                runtime_state.active_clip_source_model_path.clear();
                runtime_state.previous_state.clear();
                runtime_state.previous_clip_name.clear();
                runtime_state.previous_clip_source_model_path.clear();
                runtime_state.state_time_seconds = 0.0f;
                runtime_state.previous_state_time_seconds = 0.0f;
                runtime_state.previous_state_playback_speed = 1.0f;
                runtime_state.blend_duration_seconds = 0.0f;
                runtime_state.blend_time_remaining_seconds = 0.0f;
                runtime_state.previous_pose_snapshot_valid = false;
                g_runtime_animator_blend_snapshots.erase(runtime_key);
                runtime_state.float_parameters.clear();
                runtime_state.bool_parameters.clear();
                runtime_state.triggers.clear();
            }

            const bool active_state_valid = !runtime_state.active_state.empty() && find_state(*controller, runtime_state.active_state) != nullptr;
            if (!active_state_valid)
            {
                std::string next_state = attribute.animator.initial_state;
                if (next_state.empty() || find_state(*controller, next_state) == nullptr)
                {
                    next_state = controller->default_state;
                }
                if (next_state.empty() || find_state(*controller, next_state) == nullptr)
                {
                    next_state = controller->states.front().name;
                }

                runtime_state.active_state = next_state;
                runtime_state.state_time_seconds = 0.0f;
            }

            const AnimatorStateDefinition* active_state = find_state(*controller, runtime_state.active_state);
            if (active_state == nullptr)
            {
                continue;
            }

            if (runtime_state.blend_time_remaining_seconds > 0.0f)
            {
                runtime_state.blend_time_remaining_seconds = (std::max)(0.0f, runtime_state.blend_time_remaining_seconds - delta_time);
                if (runtime_state.blend_time_remaining_seconds <= 0.0f)
                {
                    runtime_state.blend_duration_seconds = 0.0f;
                    runtime_state.previous_state.clear();
                    runtime_state.previous_clip_name.clear();
                    runtime_state.previous_clip_source_model_path.clear();
                    runtime_state.previous_state_time_seconds = 0.0f;
                    runtime_state.previous_state_playback_speed = 1.0f;
                    runtime_state.previous_pose_snapshot_valid = false;
                    g_runtime_animator_blend_snapshots.erase(runtime_key);
                }
            }

            runtime_state.active_clip_name.clear();
            runtime_state.active_clip_source_model_path.clear();
            if (!active_state->clip_id.empty())
            {
                const auto clip_it = std::find_if(controller->clips.begin(), controller->clips.end(), [&](const AnimatorClipReference& clip)
                {
                    return clip.id == active_state->clip_id;
                });
                if (clip_it != controller->clips.end())
                {
                    runtime_state.active_clip_name = clip_it->clip_name;
                    runtime_state.active_clip_source_model_path = clip_it->source_model_path;
                }
            }

            const float object_speed = (std::max)(0.0f, attribute.animator.playback_speed);
            const float active_state_speed = (std::max)(0.0f, active_state->playback_speed);
            if (attribute.animator.auto_play)
            {
                runtime_state.state_time_seconds += delta_time * object_speed * active_state_speed;
                if (runtime_state.blend_time_remaining_seconds > 0.0f && !runtime_state.previous_clip_name.empty())
                {
                    runtime_state.previous_state_time_seconds += delta_time * object_speed * runtime_state.previous_state_playback_speed;
                }
            }

            for (const AnimatorTransitionDefinition& transition : controller->transitions)
            {
                if (transition.from_state != runtime_state.active_state)
                {
                    continue;
                }
                if (transition.has_exit_time && runtime_state.state_time_seconds < transition.exit_time)
                {
                    continue;
                }

                std::string consumed_trigger;
                if (!evaluate_condition(runtime_state, transition.condition, &consumed_trigger))
                {
                    continue;
                }

                if (find_state(*controller, transition.to_state) != nullptr)
                {
                    if (!consumed_trigger.empty())
                    {
                        runtime_state.triggers.erase(consumed_trigger);
                    }

                    const float blend_duration = (std::max)(0.0f, transition.blend_duration);
                    if (blend_duration > 0.0f && !runtime_state.active_clip_name.empty())
                    {
                        std::vector<aiMatrix4x4> blended_pose_snapshot;
                        if (runtime_state.blend_time_remaining_seconds > 0.0f &&
                            capture_blended_pose_snapshot(runtime_state, blended_pose_snapshot))
                        {
                            g_runtime_animator_blend_snapshots[runtime_key] = std::move(blended_pose_snapshot);
                            runtime_state.previous_pose_snapshot_valid = true;
                            runtime_state.previous_state = runtime_state.active_state;
                            runtime_state.previous_clip_name.clear();
                            runtime_state.previous_clip_source_model_path.clear();
                            runtime_state.previous_state_time_seconds = 0.0f;
                            runtime_state.previous_state_playback_speed = active_state_speed;
                        }
                        else
                        {
                            runtime_state.previous_pose_snapshot_valid = false;
                            g_runtime_animator_blend_snapshots.erase(runtime_key);
                            runtime_state.previous_state = runtime_state.active_state;
                            runtime_state.previous_clip_name = runtime_state.active_clip_name;
                            runtime_state.previous_clip_source_model_path = runtime_state.active_clip_source_model_path;
                            runtime_state.previous_state_time_seconds = runtime_state.state_time_seconds;
                            runtime_state.previous_state_playback_speed = active_state_speed;
                        }
                        runtime_state.blend_duration_seconds = blend_duration;
                        runtime_state.blend_time_remaining_seconds = blend_duration;
                    }
                    else
                    {
                        runtime_state.previous_state.clear();
                        runtime_state.previous_clip_name.clear();
                        runtime_state.previous_clip_source_model_path.clear();
                        runtime_state.previous_state_time_seconds = 0.0f;
                        runtime_state.previous_state_playback_speed = 1.0f;
                        runtime_state.blend_duration_seconds = 0.0f;
                        runtime_state.blend_time_remaining_seconds = 0.0f;
                        runtime_state.previous_pose_snapshot_valid = false;
                        g_runtime_animator_blend_snapshots.erase(runtime_key);
                    }

                    runtime_state.active_state = transition.to_state;
                    runtime_state.state_time_seconds = 0.0f;
                    break;
                }
            }
        }
    }

    for (auto it = runtime_animator_states_.begin(); it != runtime_animator_states_.end();)
    {
        if (live_keys.find(it->first) == live_keys.end())
        {
            g_runtime_animator_blend_snapshots.erase(it->first);
            it = runtime_animator_states_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

const RuntimeRenderer::CachedModelAssetEntry& RuntimeRenderer::GetModelAssetEntry(const std::filesystem::path& path)
{
    CachedModelAssetEntry& cache_entry = model_asset_cache_[path];

    // Cached for the lifetime of the play session — no per-frame re-stat,
    // which is the dominant editor-only stutter source for the fox model.
    if (cache_entry.asset.loaded)
    {
        return cache_entry;
    }

    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool has_filesystem_time = !error;
    cache_entry.write_time = has_filesystem_time ? write_time : std::filesystem::file_time_type::min();
    cache_entry.asset = LoadModelAsset(path);
    return cache_entry;
}

const SceneMetadata& RuntimeRenderer::GetSceneMetadata()
{
    if (scene_path_.empty())
    {
        static SceneMetadata empty_metadata{};
        return empty_metadata;
    }

    // The scene file is loaded once per play session. Re-stat'ing it every
    // frame triggers Defender scans on Windows and produces a periodic
    // play-mode hitch that is not present in the standalone game build (the
    // game serves the scene from the pak archive).
    if (has_cached_scene_metadata_ && cached_scene_path_ == scene_path_)
    {
        return cached_scene_metadata_;
    }

    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(scene_path_, error);

    const bool has_filesystem_time = !error;
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
        cache_entry.materials[material_index].attenuation_color = material.attenuation_color;
        cache_entry.materials[material_index].specular_color = material.specular_color;
        cache_entry.materials[material_index].sheen_color = material.sheen_color;
        cache_entry.materials[material_index].metallic_factor = material.metallic_factor;
        cache_entry.materials[material_index].roughness_factor = material.roughness_factor;
        cache_entry.materials[material_index].normal_scale = material.normal_scale;
        cache_entry.materials[material_index].occlusion_strength = material.occlusion_strength;
        cache_entry.materials[material_index].specular_factor = material.specular_factor;
        cache_entry.materials[material_index].sheen_roughness_factor = material.sheen_roughness_factor;
        cache_entry.materials[material_index].iridescence_factor = material.iridescence_factor;
        cache_entry.materials[material_index].iridescence_ior = material.iridescence_ior;
        cache_entry.materials[material_index].iridescence_thickness_minimum = material.iridescence_thickness_minimum;
        cache_entry.materials[material_index].iridescence_thickness_maximum = material.iridescence_thickness_maximum;
        cache_entry.materials[material_index].index_of_refraction = material.index_of_refraction;
        cache_entry.materials[material_index].transmission_factor = material.transmission_factor;
        cache_entry.materials[material_index].volume_thickness_factor = material.volume_thickness_factor;
        cache_entry.materials[material_index].attenuation_distance = material.attenuation_distance;
        cache_entry.materials[material_index].clearcoat_factor = material.clearcoat_factor;
        cache_entry.materials[material_index].clearcoat_roughness_factor = material.clearcoat_roughness_factor;
        cache_entry.materials[material_index].clearcoat_normal_scale = material.clearcoat_normal_scale;
        cache_entry.materials[material_index].alpha_cutoff = material.alpha_cutoff;
        cache_entry.materials[material_index].alpha_mode = static_cast<std::uint32_t>(material.alpha_mode);
        cache_entry.materials[material_index].uses_alpha_transparency = material.uses_alpha_transparency;

        const ModelTextureAsset* texture_assets[16] = {
            &material.base_color_texture,
            &material.metallic_roughness_texture,
            &material.normal_texture,
            &material.occlusion_texture,
            &material.emissive_texture,
            &material.transmission_texture,
            &material.specular_texture,
            &material.specular_color_texture,
            &material.sheen_color_texture,
            &material.sheen_roughness_texture,
            &material.iridescence_texture,
            &material.iridescence_thickness_texture,
            &material.volume_thickness_texture,
            &material.clearcoat_texture,
            &material.clearcoat_roughness_texture,
            &material.clearcoat_normal_texture,
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
        cache_entry.materials[material_index].transmission_view = cache_entry.material_textures[material_index].transmission.view;
        cache_entry.materials[material_index].specular_view = cache_entry.material_textures[material_index].specular.view;
        cache_entry.materials[material_index].specular_color_view = cache_entry.material_textures[material_index].specular_color.view;
        cache_entry.materials[material_index].sheen_color_view = cache_entry.material_textures[material_index].sheen_color.view;
        cache_entry.materials[material_index].sheen_roughness_view = cache_entry.material_textures[material_index].sheen_roughness.view;
        cache_entry.materials[material_index].iridescence_view = cache_entry.material_textures[material_index].iridescence.view;
        cache_entry.materials[material_index].iridescence_thickness_view = cache_entry.material_textures[material_index].iridescence_thickness.view;
        cache_entry.materials[material_index].volume_thickness_view = cache_entry.material_textures[material_index].volume_thickness.view;
        cache_entry.materials[material_index].clearcoat_view = cache_entry.material_textures[material_index].clearcoat.view;
        cache_entry.materials[material_index].clearcoat_roughness_view = cache_entry.material_textures[material_index].clearcoat_roughness.view;
        cache_entry.materials[material_index].clearcoat_normal_view = cache_entry.material_textures[material_index].clearcoat_normal.view;
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

    // Once a script is loaded, do not re-stat it every frame. Per-frame
    // last_write_time on Lua source files trips Windows Defender scanning
    // and contributes to play-mode stutter.
    if (cache_entry.loaded)
    {
        return true;
    }

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
        {"Animator", "ControllerPath", ScriptAttributeAccessorId::AnimatorControllerPath},
        {"Animator", "InitialState", ScriptAttributeAccessorId::AnimatorInitialState},
        {"Animator", "PlaybackSpeed", ScriptAttributeAccessorId::AnimatorPlaybackSpeed},
        {"Animator", "AutoPlay", ScriptAttributeAccessorId::AnimatorAutoPlay},
        {"Animator", "GetState", ScriptAttributeAccessorId::AnimatorGetState},
        {"Animator", "StateTime", ScriptAttributeAccessorId::AnimatorStateTime},
        {"Animator", "SetBool", ScriptAttributeAccessorId::AnimatorSetBool},
        {"Animator", "GetBool", ScriptAttributeAccessorId::AnimatorGetBool},
        {"Animator", "SetTrigger", ScriptAttributeAccessorId::AnimatorSetTrigger},
        {"Animator", "SetState", ScriptAttributeAccessorId::AnimatorSetState},
        {"Animator", "SetDefaultState", ScriptAttributeAccessorId::AnimatorSetDefaultState},
        {"Animator", "GetDefaultState", ScriptAttributeAccessorId::AnimatorGetDefaultState},
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

RuntimeRenderer::RuntimeAnimatorState* RuntimeRenderer::FindRuntimeAnimatorState(
    const std::string& object_name,
    std::size_t occurrence_index)
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
        for (std::size_t attribute_index = 0; attribute_index < object.attributes.size(); ++attribute_index)
        {
            const SceneObjectAttribute& attribute = object.attributes[attribute_index];
            if (attribute.kind != SceneObjectAttributeKind::Animator)
            {
                continue;
            }

            if (current_occurrence == occurrence_index)
            {
                const std::string runtime_key = object.name + "#" + std::to_string(attribute_index);
                const auto state_it = runtime_animator_states_.find(runtime_key);
                return state_it != runtime_animator_states_.end() ? &state_it->second : nullptr;
            }

            ++current_occurrence;
        }

        return nullptr;
    }

    return nullptr;
}

RuntimeRenderer::RuntimeAnimatorState* RuntimeRenderer::EnsureRuntimeAnimatorState(
    const std::string& object_name,
    std::size_t occurrence_index)
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
        for (std::size_t attribute_index = 0; attribute_index < object.attributes.size(); ++attribute_index)
        {
            const SceneObjectAttribute& attribute = object.attributes[attribute_index];
            if (attribute.kind != SceneObjectAttributeKind::Animator)
            {
                continue;
            }

            if (current_occurrence == occurrence_index)
            {
                const std::string runtime_key = object.name + "#" + std::to_string(attribute_index);
                RuntimeAnimatorState& state = runtime_animator_states_[runtime_key];
                if (state.controller_path != attribute.animator.controller_path)
                {
                    state.controller_path = attribute.animator.controller_path;
                    state.active_state.clear();
                    state.active_clip_name.clear();
                    state.active_clip_source_model_path.clear();
                    state.previous_state.clear();
                    state.previous_clip_name.clear();
                    state.previous_clip_source_model_path.clear();
                    state.state_time_seconds = 0.0f;
                    state.previous_state_time_seconds = 0.0f;
                    state.previous_state_playback_speed = 1.0f;
                    state.blend_duration_seconds = 0.0f;
                    state.blend_time_remaining_seconds = 0.0f;
                    state.float_parameters.clear();
                    state.bool_parameters.clear();
                    state.triggers.clear();
                }
                return &state;
            }

            ++current_occurrence;
        }

        return nullptr;
    }

    return nullptr;
}

bool RuntimeRenderer::SetRuntimeAnimatorParameter(
    const std::string& object_name,
    const std::string& parameter_name,
    float value,
    std::size_t occurrence_index)
{
    if (parameter_name.empty())
    {
        return false;
    }

    RuntimeAnimatorState* const state = EnsureRuntimeAnimatorState(object_name, occurrence_index);
    if (state == nullptr)
    {
        return false;
    }

    state->float_parameters[parameter_name] = value;
    state->bool_parameters.erase(parameter_name);
    return true;
}

bool RuntimeRenderer::SetRuntimeAnimatorBoolParameter(
    const std::string& object_name,
    const std::string& parameter_name,
    bool value,
    std::size_t occurrence_index)
{
    if (parameter_name.empty())
    {
        return false;
    }

    RuntimeAnimatorState* const state = EnsureRuntimeAnimatorState(object_name, occurrence_index);
    if (state == nullptr)
    {
        return false;
    }

    state->bool_parameters[parameter_name] = value;
    state->float_parameters.erase(parameter_name);
    return true;
}

bool RuntimeRenderer::TryGetRuntimeAnimatorParameter(
    const std::string& object_name,
    const std::string& parameter_name,
    float& out_value,
    bool& out_is_bool,
    std::size_t occurrence_index) const
{
    out_value = 0.0f;
    out_is_bool = false;
    if (parameter_name.empty())
    {
        return false;
    }

    const RuntimeAnimatorState* const state = FindRuntimeAnimatorState(object_name, occurrence_index);
    if (state == nullptr)
    {
        return false;
    }

    const auto bool_it = state->bool_parameters.find(parameter_name);
    if (bool_it != state->bool_parameters.end())
    {
        out_value = bool_it->second ? 1.0f : 0.0f;
        out_is_bool = true;
        return true;
    }

    const auto float_it = state->float_parameters.find(parameter_name);
    if (float_it != state->float_parameters.end())
    {
        out_value = float_it->second;
        return true;
    }

    return false;
}

bool RuntimeRenderer::SetRuntimeAnimatorTrigger(
    const std::string& object_name,
    const std::string& trigger_name,
    std::size_t occurrence_index)
{
    if (trigger_name.empty())
    {
        return false;
    }

    RuntimeAnimatorState* const state = EnsureRuntimeAnimatorState(object_name, occurrence_index);
    if (state == nullptr)
    {
        return false;
    }

    state->triggers.insert(trigger_name);
    return true;
}

bool RuntimeRenderer::SetRuntimeAnimatorState(
    const std::string& object_name,
    const std::string& state_name,
    std::size_t occurrence_index)
{
    if (state_name.empty())
    {
        return false;
    }

    RuntimeAnimatorState* const state = EnsureRuntimeAnimatorState(object_name, occurrence_index);
    if (state == nullptr)
    {
        return false;
    }

    state->active_state = state_name;
    state->state_time_seconds = 0.0f;
    return true;
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

const RuntimeRenderer::RuntimeAnimatorState* RuntimeRenderer::FindRuntimeAnimatorState(
    const std::string& object_name,
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
        for (std::size_t attribute_index = 0; attribute_index < object.attributes.size(); ++attribute_index)
        {
            const SceneObjectAttribute& attribute = object.attributes[attribute_index];
            if (attribute.kind != SceneObjectAttributeKind::Animator)
            {
                continue;
            }

            if (current_occurrence == occurrence_index)
            {
                const std::string runtime_key = object.name + "#" + std::to_string(attribute_index);
                const auto state_it = runtime_animator_states_.find(runtime_key);
                return state_it != runtime_animator_states_.end() ? &state_it->second : nullptr;
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

    case SceneObjectAttributeKind::Animator:
        runtime_animator_states_.clear();
        g_runtime_animator_blend_snapshots.clear();
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
        if (!UpdateAnimatedMeshForObject(object))
        {
            if (error_message != nullptr)
            {
                *error_message = "Failed to update animated mesh vertices";
            }
            return false;
        }

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
            const auto revision_it = animated_mesh_revisions_.find(object.model_path);
            mesh_input.geometry_revision = revision_it != animated_mesh_revisions_.end() ? revision_it->second : 0;
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
    performance_stats_ = RuntimePerformanceStats{};
    const std::uint64_t frame_start_ticks = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());

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
        const std::uint64_t physics_start_ticks = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
        const std::uint64_t now_ms = static_cast<std::uint64_t>(SDL_GetTicks());
        const float raw_phys_dt = (script_last_tick_ms_ != 0 && now_ms >= script_last_tick_ms_)
            ? static_cast<float>(now_ms - script_last_tick_ms_) / 1000.0f
            : 0.0f;
        // Cap the physics step at ~30 FPS worth of time. A periodic editor
        // hitch (filesystem scan, build, AV) can otherwise translate a frame
        // dt of 40+ ms into a giant integration step which moves a
        // velocity-driven body 2-3x its normal per-frame distance, producing
        // a visible "snap forward" on the next frame. Capping converts that
        // into a small slowdown instead of a teleport.
        constexpr float kMaxPhysicsDt = 1.0f / 30.0f;
        const float phys_dt = (std::min)(raw_phys_dt, kMaxPhysicsDt);
        physics_world_.Step(phys_dt);
        physics_object_transforms_ = physics_world_.GetSimulatedTransforms();
        const auto simulated = physics_world_.GetSimulatedPositions();
        for (const auto& [name, pos] : simulated)
        {
            // Physics is authoritative for dynamic body positions.
            SetScriptObjectPosition(name, pos);
        }
        performance_stats_.physics_time_ms = TicksToMilliseconds(
            physics_start_ticks,
            static_cast<std::uint64_t>(SDL_GetPerformanceCounter()));
    }

    const std::uint64_t scripts_start_ticks = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
    if (!UpdateScriptsForFrame(error_message))
    {
        return false;
    }
    performance_stats_.scripts_time_ms = TicksToMilliseconds(
        scripts_start_ticks,
        static_cast<std::uint64_t>(SDL_GetPerformanceCounter()));

    const std::uint64_t animation_start_ticks = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
    UpdateAnimatorControllersForFrame(scene_metadata);
    performance_stats_.animation_time_ms = TicksToMilliseconds(
        animation_start_ticks,
        static_cast<std::uint64_t>(SDL_GetPerformanceCounter()));

    if (!SyncRayTracingScene(error_message))
    {
        return false;
    }

    ray_tracing_.SetSkyboxTexture(skybox_renderer_.ResolveSkyboxView(scene_metadata, project_root_));
    ray_tracing_.SetSkyboxRotation(skybox_renderer_.ResolveSkyboxRotationDegrees(scene_metadata));

    const std::uint64_t render_start_ticks = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
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
    performance_stats_.render_time_ms = TicksToMilliseconds(
        render_start_ticks,
        static_cast<std::uint64_t>(SDL_GetPerformanceCounter()));

    const std::uint64_t overlay_start_ticks = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
    scene_2d_renderer_.CompositeOverlay(
        scene_metadata,
        project_root_,
        ray_tracing_.GetOutputImage(),
        ray_tracing_.GetOutputImageView(),
        ray_tracing_.GetOutputWidth(),
        ray_tracing_.GetOutputHeight());

    performance_stats_.overlay_2d_time_ms = TicksToMilliseconds(
        overlay_start_ticks,
        static_cast<std::uint64_t>(SDL_GetPerformanceCounter()));
    performance_stats_.frame_time_ms = TicksToMilliseconds(
        frame_start_ticks,
        static_cast<std::uint64_t>(SDL_GetPerformanceCounter()));
    performance_stats_.fps = performance_stats_.frame_time_ms > 0.0001f
        ? 1000.0f / performance_stats_.frame_time_ms
        : 0.0f;
    performance_stats_.valid = true;

    return true;
}

bool RuntimeRenderer::UpdateAnimatedMeshForObject(const QueuedSceneObject& object)
{
    RuntimeAnimatorState* const runtime_state = FindRuntimeAnimatorState(object.name);
    if (runtime_state == nullptr || runtime_state->active_clip_name.empty())
    {
        return true;
    }

    const auto mesh_cache_it = mesh_cache_.find(object.model_path);
    if (mesh_cache_it == mesh_cache_.end())
    {
        return true;
    }

    const CachedModelAssetEntry& model_asset_entry = GetModelAssetEntry(object.model_path);
    if (!model_asset_entry.asset.loaded || model_asset_entry.asset.meshes.empty())
    {
        return true;
    }

    std::filesystem::path clip_source_path = object.model_path;
    if (!runtime_state->active_clip_source_model_path.empty())
    {
        clip_source_path = std::filesystem::path(runtime_state->active_clip_source_model_path).is_absolute()
            ? std::filesystem::path(runtime_state->active_clip_source_model_path)
            : (project_root_ / runtime_state->active_clip_source_model_path);
    }

    RuntimeAnimationModelCacheEntry* const anim_cache_entry = GetRuntimeAnimationModelCacheEntry(clip_source_path);
    if (anim_cache_entry == nullptr || anim_cache_entry->scene == nullptr)
    {
        return false;
    }

    std::vector<aiMatrix4x4> bone_matrices;
    if (!SampleClipBoneMatrices(*anim_cache_entry, runtime_state->active_clip_name, runtime_state->state_time_seconds, bone_matrices))
    {
        return true;
    }

    bool has_blend = false;
    float blend_alpha = 1.0f;
    std::vector<aiMatrix4x4> previous_bone_matrices;
    if (runtime_state->blend_duration_seconds > 0.0f &&
        runtime_state->blend_time_remaining_seconds > 0.0f)
    {
        blend_alpha = 1.0f - std::clamp(
            runtime_state->blend_time_remaining_seconds / (std::max)(0.0001f, runtime_state->blend_duration_seconds),
            0.0f,
            1.0f);

        const auto snapshot_it = g_runtime_animator_blend_snapshots.find(runtime_state->runtime_key);
        if (runtime_state->previous_pose_snapshot_valid &&
            snapshot_it != g_runtime_animator_blend_snapshots.end() &&
            snapshot_it->second.size() == bone_matrices.size())
        {
            has_blend = true;
            previous_bone_matrices = snapshot_it->second;
        }
        else if (!runtime_state->previous_clip_name.empty())
        {
            std::filesystem::path previous_clip_source_path = object.model_path;
            if (!runtime_state->previous_clip_source_model_path.empty())
            {
                previous_clip_source_path = std::filesystem::path(runtime_state->previous_clip_source_model_path).is_absolute()
                    ? std::filesystem::path(runtime_state->previous_clip_source_model_path)
                    : (project_root_ / runtime_state->previous_clip_source_model_path);
            }

            RuntimeAnimationModelCacheEntry* previous_anim_cache = nullptr;
            if (previous_clip_source_path == clip_source_path)
            {
                previous_anim_cache = anim_cache_entry;
            }
            else
            {
                previous_anim_cache = GetRuntimeAnimationModelCacheEntry(previous_clip_source_path);
            }

            if (previous_anim_cache != nullptr && previous_anim_cache->scene != nullptr)
            {
                std::vector<aiMatrix4x4> sampled_previous_matrices;
                if (SampleClipBoneMatrices(
                        *previous_anim_cache,
                        runtime_state->previous_clip_name,
                        runtime_state->previous_state_time_seconds,
                        sampled_previous_matrices) &&
                    sampled_previous_matrices.size() == bone_matrices.size())
                {
                    has_blend = true;
                    previous_bone_matrices = std::move(sampled_previous_matrices);
                }
            }
        }
    }

    // ---------- GPU compute-skinning fast path ----------
    // Pre-baked bind-pose + per-vertex influences live in device-local-ish
    // SSBOs; we only upload the per-frame bone palette and dispatch a compute
    // shader on the same immediate command buffer the BLAS refit uses. This
    // eliminates the per-vertex CPU loop (the dominant remaining cost in
    // Debug builds) and the per-frame staging-vector reallocations.
    if (ray_tracing_.IsAvailable() &&
        EnsureSkinningPipeline() &&
        mesh_cache_it->second.vertex_buffer.buffer != VK_NULL_HANDLE)
    {
        GpuSkinningResources& resources = gpu_skinning_resources_[object.model_path];

        // Rebuild if source clip changed (write_time bump) or vertex count
        // doesn't match the live mesh cache (model reloaded).
        const bool source_changed =
            resources.source_clip_model_path != clip_source_path ||
            resources.source_clip_write_time != anim_cache_entry->write_time ||
            resources.vertex_count != mesh_cache_it->second.vertex_count;
        if (source_changed && (resources.bind_pose_buffer.buffer != VK_NULL_HANDLE ||
                               resources.influence_buffer.buffer != VK_NULL_HANDLE ||
                               resources.palette_buffer.buffer != VK_NULL_HANDLE))
        {
            ReleaseSkinningResources(resources);
        }

        if (!resources.ready)
        {
            // ---- Build bind-pose + influence buffers (one-time per model) ----
            const std::uint32_t total_vertex_count = mesh_cache_it->second.vertex_count;
            const std::uint32_t total_bone_count =
                static_cast<std::uint32_t>(bone_matrices.size());
            if (total_vertex_count > 0 && total_bone_count > 0)
            {
                std::vector<SceneGpuVertex> bind_pose_vertices;
                bind_pose_vertices.reserve(total_vertex_count);

                struct GpuVertexInfluence
                {
                    std::uint32_t bone_indices[4];
                    float bone_weights[4];
                };
                std::vector<GpuVertexInfluence> influence_storage;
                influence_storage.reserve(total_vertex_count);

                const std::size_t mc =
                    (std::min)(model_asset_entry.asset.meshes.size(),
                               anim_cache_entry->skinned_meshes.size());
                for (std::size_t mi = 0; mi < mc; ++mi)
                {
                    const ModelMeshAsset& model_mesh = model_asset_entry.asset.meshes[mi];
                    const RuntimeSkinnedMeshData& skinned_mesh = anim_cache_entry->skinned_meshes[mi];
                    if (skinned_mesh.mesh == nullptr)
                    {
                        continue;
                    }
                    const ModelMaterialAsset* material = nullptr;
                    if (model_mesh.material_index < model_asset_entry.asset.materials.size())
                    {
                        material = &model_asset_entry.asset.materials[model_mesh.material_index];
                    }

                    aiMatrix3x3 bind_normal_transform(skinned_mesh.bind_node_transform);
                    bind_normal_transform.Inverse().Transpose();
                    aiMatrix3x3 bind_tangent_transform(skinned_mesh.bind_node_transform);

                    for (unsigned int vi = 0; vi < skinned_mesh.mesh->mNumVertices; ++vi)
                    {
                        const aiVector3D raw_pos = skinned_mesh.mesh->mVertices[vi];
                        const aiVector3D raw_n = skinned_mesh.mesh->HasNormals()
                            ? skinned_mesh.mesh->mNormals[vi]
                            : aiVector3D(0.0f, 1.0f, 0.0f);
                        const aiVector3D raw_t = skinned_mesh.mesh->HasTangentsAndBitangents()
                            ? skinned_mesh.mesh->mTangents[vi]
                            : aiVector3D(1.0f, 0.0f, 0.0f);

                        bool has_w = false;
                        GpuVertexInfluence inf{};
                        inf.bone_indices[0] = inf.bone_indices[1] =
                            inf.bone_indices[2] = inf.bone_indices[3] = 0u;
                        inf.bone_weights[0] = inf.bone_weights[1] =
                            inf.bone_weights[2] = inf.bone_weights[3] = 0.0f;
                        if (vi < skinned_mesh.influences.size())
                        {
                            const RuntimeSkinInfluence& src = skinned_mesh.influences[vi];
                            for (int s = 0; s < 4; ++s)
                            {
                                const int b = src.bone_indices[s];
                                const float w = src.bone_weights[s];
                                if (b >= 0 && w > 0.0f &&
                                    static_cast<std::size_t>(b) < bone_matrices.size())
                                {
                                    inf.bone_indices[s] = static_cast<std::uint32_t>(b);
                                    inf.bone_weights[s] = w;
                                    has_w = true;
                                }
                            }
                        }
                        influence_storage.push_back(inf);

                        // Bake bind pose. For weighted vertices, store raw
                        // values (the bone matrices include all needed
                        // transforms). For unweighted vertices, pre-apply
                        // bind_node_transform so the GPU shader's pass-through
                        // path produces the right result.
                        ModelVertex bind_v{};
                        if (has_w)
                        {
                            bind_v.position = {raw_pos.x, raw_pos.y, raw_pos.z};
                            bind_v.normal = {raw_n.x, raw_n.y, raw_n.z};
                            bind_v.tangent = {raw_t.x, raw_t.y, raw_t.z, 1.0f};
                        }
                        else
                        {
                            aiVector3D p = raw_pos; p *= skinned_mesh.bind_node_transform;
                            aiVector3D n = raw_n;   n *= bind_normal_transform;
                            aiVector3D t = raw_t;   t *= bind_tangent_transform;
                            n.NormalizeSafe(); t.NormalizeSafe();
                            bind_v.position = {p.x, p.y, p.z};
                            bind_v.normal = {n.x, n.y, n.z};
                            bind_v.tangent = {t.x, t.y, t.z, 1.0f};
                        }
                        if (skinned_mesh.mesh->HasTextureCoords(0))
                        {
                            std::array<float, 2> uv = {
                                skinned_mesh.mesh->mTextureCoords[0][vi].x,
                                skinned_mesh.mesh->mTextureCoords[0][vi].y,
                            };
                            if (material != nullptr)
                            {
                                uv = ApplyUvTransformLocal(uv, material->uv_transform);
                            }
                            bind_v.uv0 = uv;
                        }
                        bind_pose_vertices.push_back(BuildSceneGpuVertex(bind_v, material));
                    }
                }

                if (bind_pose_vertices.size() == total_vertex_count &&
                    influence_storage.size() == total_vertex_count)
                {
                    const VkDeviceSize bind_size =
                        static_cast<VkDeviceSize>(bind_pose_vertices.size() * sizeof(SceneGpuVertex));
                    const VkDeviceSize inf_size =
                        static_cast<VkDeviceSize>(influence_storage.size() * sizeof(GpuVertexInfluence));
                    const VkDeviceSize palette_size =
                        static_cast<VkDeviceSize>(total_bone_count * 16 * sizeof(float));

                    const bool buffers_ok =
                        CreateVulkanBuffer(*vulkan_context_, bind_size,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            resources.bind_pose_buffer) &&
                        CreateVulkanBuffer(*vulkan_context_, inf_size,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            resources.influence_buffer) &&
                        CreateVulkanBuffer(*vulkan_context_, palette_size,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            resources.palette_buffer);
                    bool uploaded_static =
                        buffers_ok &&
                        UploadBufferData(vulkan_context_->GetDevice(), resources.bind_pose_buffer,
                            bind_pose_vertices.data(), static_cast<std::size_t>(bind_size)) &&
                        UploadBufferData(vulkan_context_->GetDevice(), resources.influence_buffer,
                            influence_storage.data(), static_cast<std::size_t>(inf_size));

                    VkDescriptorSet desc_set = VK_NULL_HANDLE;
                    if (uploaded_static)
                    {
                        VkDescriptorSetAllocateInfo ds_alloc = {};
                        ds_alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                        ds_alloc.descriptorPool = vulkan_context_->GetDescriptorPool();
                        ds_alloc.descriptorSetCount = 1;
                        ds_alloc.pSetLayouts = &skinning_descriptor_set_layout_;
                        VkResult r = vkAllocateDescriptorSets(vulkan_context_->GetDevice(), &ds_alloc, &desc_set);
                        if (r != VK_SUCCESS)
                        {
                            desc_set = VK_NULL_HANDLE;
                            uploaded_static = false;
                        }
                    }

                    if (uploaded_static && desc_set != VK_NULL_HANDLE)
                    {
                        std::array<VkDescriptorBufferInfo, 4> buf_infos = {};
                        buf_infos[0] = {resources.bind_pose_buffer.buffer, 0, VK_WHOLE_SIZE};
                        buf_infos[1] = {mesh_cache_it->second.vertex_buffer.buffer, 0, VK_WHOLE_SIZE};
                        buf_infos[2] = {resources.influence_buffer.buffer, 0, VK_WHOLE_SIZE};
                        buf_infos[3] = {resources.palette_buffer.buffer, 0, VK_WHOLE_SIZE};

                        std::array<VkWriteDescriptorSet, 4> writes = {};
                        for (std::uint32_t i = 0; i < 4; ++i)
                        {
                            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                            writes[i].dstSet = desc_set;
                            writes[i].dstBinding = i;
                            writes[i].descriptorCount = 1;
                            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                            writes[i].pBufferInfo = &buf_infos[i];
                        }
                        vkUpdateDescriptorSets(vulkan_context_->GetDevice(),
                            static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

                        resources.descriptor_set = desc_set;
                        resources.vertex_count = total_vertex_count;
                        resources.bone_count = total_bone_count;
                        resources.source_clip_model_path = clip_source_path;
                        resources.source_clip_write_time = anim_cache_entry->write_time;
                        resources.ready = true;
                    }
                    else
                    {
                        ReleaseSkinningResources(resources);
                    }
                }
            }
        }

        if (resources.ready && resources.bone_count == bone_matrices.size())
        {
            // Per-frame: blend palette on CPU (matrix lerp – cheap and a
            // reasonable approximation of per-vertex cross-fade), upload to
            // host-coherent palette buffer, enqueue compute dispatch.
            std::vector<float> palette_floats(static_cast<std::size_t>(resources.bone_count) * 16);
            for (std::size_t bi = 0; bi < bone_matrices.size(); ++bi)
            {
                aiMatrix4x4 m = bone_matrices[bi];
                if (has_blend && bi < previous_bone_matrices.size())
                {
                    m = LerpMatrix(previous_bone_matrices[bi], m, blend_alpha);
                }
                // Store as column-major (GLSL mat4 default).
                float* dst = palette_floats.data() + bi * 16;
                dst[ 0] = m.a1; dst[ 1] = m.b1; dst[ 2] = m.c1; dst[ 3] = m.d1;
                dst[ 4] = m.a2; dst[ 5] = m.b2; dst[ 6] = m.c2; dst[ 7] = m.d2;
                dst[ 8] = m.a3; dst[ 9] = m.b3; dst[10] = m.c3; dst[11] = m.d3;
                dst[12] = m.a4; dst[13] = m.b4; dst[14] = m.c4; dst[15] = m.d4;
            }

            const bool palette_uploaded = UploadBufferData(
                vulkan_context_->GetDevice(),
                resources.palette_buffer,
                palette_floats.data(),
                palette_floats.size() * sizeof(float));

            if (palette_uploaded)
            {
                RayTracing::PendingSkinningDispatch dispatch{};
                dispatch.pipeline = skinning_pipeline_;
                dispatch.pipeline_layout = skinning_pipeline_layout_;
                dispatch.descriptor_set = resources.descriptor_set;
                dispatch.output_vertex_buffer = mesh_cache_it->second.vertex_buffer.buffer;
                dispatch.vertex_count = resources.vertex_count;
                dispatch.bone_count = resources.bone_count;
                dispatch.group_count_x = (resources.vertex_count + 63u) / 64u;
                ray_tracing_.EnqueueSkinningDispatch(dispatch);

                static bool gpu_skinning_logged = false;
                if (!gpu_skinning_logged)
                {
                    gpu_skinning_logged = true;
                    SDL_Log(
                        "[Skinning] GPU compute skinning ACTIVE (first dispatch: object=\"%s\", vertices=%u, bones=%u)",
                        object.name.c_str(),
                        resources.vertex_count,
                        resources.bone_count);
                }

                ++animated_mesh_revisions_[object.model_path];
                return true;
            }
        }
        // Fall through to CPU path on any failure above.
    }

    {
        static bool cpu_skinning_logged = false;
        if (!cpu_skinning_logged)
        {
            cpu_skinning_logged = true;
            SDL_Log(
                "[Skinning] CPU skinning fallback in use for object=\"%s\" (GPU compute skinning unavailable or setup failed)",
                object.name.c_str());
        }
    }

    std::vector<SceneGpuVertex>& skinned_vertices = g_runtime_skinned_vertex_scratch[object.model_path.generic_string()];
    skinned_vertices.clear();
    skinned_vertices.reserve(mesh_cache_it->second.vertex_count);

    const std::size_t mesh_count = (std::min)(model_asset_entry.asset.meshes.size(), anim_cache_entry->skinned_meshes.size());
    for (std::size_t mesh_index = 0; mesh_index < mesh_count; ++mesh_index)
    {
        const ModelMeshAsset& model_mesh = model_asset_entry.asset.meshes[mesh_index];
        const RuntimeSkinnedMeshData& skinned_mesh = anim_cache_entry->skinned_meshes[mesh_index];
        if (skinned_mesh.mesh == nullptr)
        {
            continue;
        }

        const ModelMaterialAsset* material = nullptr;
        if (model_mesh.material_index < model_asset_entry.asset.materials.size())
        {
            material = &model_asset_entry.asset.materials[model_mesh.material_index];
        }

        aiMatrix3x3 bind_normal_transform(skinned_mesh.bind_node_transform);
        bind_normal_transform.Inverse().Transpose();
        aiMatrix3x3 bind_tangent_transform(skinned_mesh.bind_node_transform);

        for (unsigned int vertex_index = 0; vertex_index < skinned_mesh.mesh->mNumVertices; ++vertex_index)
        {
            const aiVector3D bind_position = skinned_mesh.mesh->mVertices[vertex_index];
            const aiVector3D bind_normal = skinned_mesh.mesh->HasNormals()
                ? skinned_mesh.mesh->mNormals[vertex_index]
                : aiVector3D(0.0f, 1.0f, 0.0f);
            const aiVector3D bind_tangent = skinned_mesh.mesh->HasTangentsAndBitangents()
                ? skinned_mesh.mesh->mTangents[vertex_index]
                : aiVector3D(1.0f, 0.0f, 0.0f);

            aiVector3D out_position(0.0f, 0.0f, 0.0f);
            aiVector3D out_normal(0.0f, 0.0f, 0.0f);
            aiVector3D out_tangent(0.0f, 0.0f, 0.0f);
            bool has_weights = false;

            if (vertex_index < skinned_mesh.influences.size())
            {
                const RuntimeSkinInfluence& influence = skinned_mesh.influences[vertex_index];
                for (int weight_slot = 0; weight_slot < 4; ++weight_slot)
                {
                    const int bone_index = influence.bone_indices[weight_slot];
                    const float weight = influence.bone_weights[weight_slot];
                    if (bone_index < 0 || weight <= 0.0f || static_cast<std::size_t>(bone_index) >= bone_matrices.size())
                    {
                        continue;
                    }

                    const std::size_t bone_idx = static_cast<std::size_t>(bone_index);
                    aiVector3D skinned_position = bone_matrices[bone_idx] * bind_position;
                    aiMatrix3x3 skinned_normal_matrix(bone_matrices[bone_idx]);
                    aiVector3D skinned_normal = skinned_normal_matrix * bind_normal;
                    aiVector3D skinned_tangent = skinned_normal_matrix * bind_tangent;

                    if (has_blend && bone_idx < previous_bone_matrices.size())
                    {
                        const aiMatrix4x4& previous_matrix = previous_bone_matrices[bone_idx];
                        aiVector3D previous_position = previous_matrix * bind_position;
                        aiMatrix3x3 previous_normal_matrix(previous_matrix);
                        aiVector3D previous_normal = previous_normal_matrix * bind_normal;
                        aiVector3D previous_tangent = previous_normal_matrix * bind_tangent;

                        skinned_position = (previous_position * (1.0f - blend_alpha)) + (skinned_position * blend_alpha);
                        skinned_normal = (previous_normal * (1.0f - blend_alpha)) + (skinned_normal * blend_alpha);
                        skinned_tangent = (previous_tangent * (1.0f - blend_alpha)) + (skinned_tangent * blend_alpha);
                    }

                    out_position += skinned_position * weight;
                    out_normal += skinned_normal * weight;
                    out_tangent += skinned_tangent * weight;
                    has_weights = true;
                }
            }

            if (!has_weights)
            {
                out_position = bind_position;
                out_position *= skinned_mesh.bind_node_transform;

                out_normal = bind_normal;
                out_normal *= bind_normal_transform;

                out_tangent = bind_tangent;
                out_tangent *= bind_tangent_transform;
            }

            out_normal.NormalizeSafe();
            out_tangent.NormalizeSafe();

            ModelVertex skinned_vertex;
            skinned_vertex.position = {out_position.x, out_position.y, out_position.z};
            skinned_vertex.normal = {out_normal.x, out_normal.y, out_normal.z};
            skinned_vertex.tangent = {out_tangent.x, out_tangent.y, out_tangent.z, 1.0f};

            if (skinned_mesh.mesh->HasTextureCoords(0))
            {
                std::array<float, 2> uv = {
                    skinned_mesh.mesh->mTextureCoords[0][vertex_index].x,
                    skinned_mesh.mesh->mTextureCoords[0][vertex_index].y,
                };
                if (material != nullptr)
                {
                    uv = ApplyUvTransformLocal(uv, material->uv_transform);
                }
                skinned_vertex.uv0 = uv;
            }

            skinned_vertices.push_back(BuildSceneGpuVertex(skinned_vertex, material));
        }
    }

    if (skinned_vertices.size() != mesh_cache_it->second.vertex_count)
    {
        return true;
    }

    const bool uploaded = UploadBufferData(
        vulkan_context_->GetDevice(),
        mesh_cache_it->second.vertex_buffer,
        skinned_vertices.data(),
        skinned_vertices.size() * sizeof(SceneGpuVertex));
    if (uploaded)
    {
        ++animated_mesh_revisions_[object.model_path];
    }

    return uploaded;
}