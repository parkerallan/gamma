#include "render/SceneViewportRenderer.h"

#include <SDL3/SDL.h>

#include <ImGuizmo.h>

#include "imgui.h"
#include "ui/Codicons.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    float tangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
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

float ComputeOrbitBaseDistance(float radius)
{
    const float safe_radius = (std::max)(radius, 0.001f);
    return (safe_radius / std::tan(DegreesToRadians(55.0f) * 0.5f)) + safe_radius * 1.2f;
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

bool InvertMatrix(const float* matrix, float* inverse)
{
    float inv[16];

    inv[0] = matrix[5] * matrix[10] * matrix[15] -
        matrix[5] * matrix[11] * matrix[14] -
        matrix[9] * matrix[6] * matrix[15] +
        matrix[9] * matrix[7] * matrix[14] +
        matrix[13] * matrix[6] * matrix[11] -
        matrix[13] * matrix[7] * matrix[10];

    inv[4] = -matrix[4] * matrix[10] * matrix[15] +
        matrix[4] * matrix[11] * matrix[14] +
        matrix[8] * matrix[6] * matrix[15] -
        matrix[8] * matrix[7] * matrix[14] -
        matrix[12] * matrix[6] * matrix[11] +
        matrix[12] * matrix[7] * matrix[10];

    inv[8] = matrix[4] * matrix[9] * matrix[15] -
        matrix[4] * matrix[11] * matrix[13] -
        matrix[8] * matrix[5] * matrix[15] +
        matrix[8] * matrix[7] * matrix[13] +
        matrix[12] * matrix[5] * matrix[11] -
        matrix[12] * matrix[7] * matrix[9];

    inv[12] = -matrix[4] * matrix[9] * matrix[14] +
        matrix[4] * matrix[10] * matrix[13] +
        matrix[8] * matrix[5] * matrix[14] -
        matrix[8] * matrix[6] * matrix[13] -
        matrix[12] * matrix[5] * matrix[10] +
        matrix[12] * matrix[6] * matrix[9];

    inv[1] = -matrix[1] * matrix[10] * matrix[15] +
        matrix[1] * matrix[11] * matrix[14] +
        matrix[9] * matrix[2] * matrix[15] -
        matrix[9] * matrix[3] * matrix[14] -
        matrix[13] * matrix[2] * matrix[11] +
        matrix[13] * matrix[3] * matrix[10];

    inv[5] = matrix[0] * matrix[10] * matrix[15] -
        matrix[0] * matrix[11] * matrix[14] -
        matrix[8] * matrix[2] * matrix[15] +
        matrix[8] * matrix[3] * matrix[14] +
        matrix[12] * matrix[2] * matrix[11] -
        matrix[12] * matrix[3] * matrix[10];

    inv[9] = -matrix[0] * matrix[9] * matrix[15] +
        matrix[0] * matrix[11] * matrix[13] +
        matrix[8] * matrix[1] * matrix[15] -
        matrix[8] * matrix[3] * matrix[13] -
        matrix[12] * matrix[1] * matrix[11] +
        matrix[12] * matrix[3] * matrix[9];

    inv[13] = matrix[0] * matrix[9] * matrix[14] -
        matrix[0] * matrix[10] * matrix[13] -
        matrix[8] * matrix[1] * matrix[14] +
        matrix[8] * matrix[2] * matrix[13] +
        matrix[12] * matrix[1] * matrix[10] -
        matrix[12] * matrix[2] * matrix[9];

    inv[2] = matrix[1] * matrix[6] * matrix[15] -
        matrix[1] * matrix[7] * matrix[14] -
        matrix[5] * matrix[2] * matrix[15] +
        matrix[5] * matrix[3] * matrix[14] +
        matrix[13] * matrix[2] * matrix[7] -
        matrix[13] * matrix[3] * matrix[6];

    inv[6] = -matrix[0] * matrix[6] * matrix[15] +
        matrix[0] * matrix[7] * matrix[14] +
        matrix[4] * matrix[2] * matrix[15] -
        matrix[4] * matrix[3] * matrix[14] -
        matrix[12] * matrix[2] * matrix[7] +
        matrix[12] * matrix[3] * matrix[6];

    inv[10] = matrix[0] * matrix[5] * matrix[15] -
        matrix[0] * matrix[7] * matrix[13] -
        matrix[4] * matrix[1] * matrix[15] +
        matrix[4] * matrix[3] * matrix[13] +
        matrix[12] * matrix[1] * matrix[7] -
        matrix[12] * matrix[3] * matrix[5];

    inv[14] = -matrix[0] * matrix[5] * matrix[14] +
        matrix[0] * matrix[6] * matrix[13] +
        matrix[4] * matrix[1] * matrix[14] -
        matrix[4] * matrix[2] * matrix[13] -
        matrix[12] * matrix[1] * matrix[6] +
        matrix[12] * matrix[2] * matrix[5];

    inv[3] = -matrix[1] * matrix[6] * matrix[11] +
        matrix[1] * matrix[7] * matrix[10] +
        matrix[5] * matrix[2] * matrix[11] -
        matrix[5] * matrix[3] * matrix[10] -
        matrix[9] * matrix[2] * matrix[7] +
        matrix[9] * matrix[3] * matrix[6];

    inv[7] = matrix[0] * matrix[6] * matrix[11] -
        matrix[0] * matrix[7] * matrix[10] -
        matrix[4] * matrix[2] * matrix[11] +
        matrix[4] * matrix[3] * matrix[10] +
        matrix[8] * matrix[2] * matrix[7] -
        matrix[8] * matrix[3] * matrix[6];

    inv[11] = -matrix[0] * matrix[5] * matrix[11] +
        matrix[0] * matrix[7] * matrix[9] +
        matrix[4] * matrix[1] * matrix[11] -
        matrix[4] * matrix[3] * matrix[9] -
        matrix[8] * matrix[1] * matrix[7] +
        matrix[8] * matrix[3] * matrix[5];

    inv[15] = matrix[0] * matrix[5] * matrix[10] -
        matrix[0] * matrix[6] * matrix[9] -
        matrix[4] * matrix[1] * matrix[10] +
        matrix[4] * matrix[2] * matrix[9] +
        matrix[8] * matrix[1] * matrix[6] -
        matrix[8] * matrix[2] * matrix[5];

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
    std::memcpy(matrix, object.model_matrix.data(), sizeof(float) * 16);

    // Apply model-only local translation without changing object or physics transforms.
    const SceneVector3& offset = object.model_visual_offset;
    if (std::abs(offset[0]) <= 0.000001f && std::abs(offset[1]) <= 0.000001f && std::abs(offset[2]) <= 0.000001f)
    {
        return;
    }

    matrix[12] += offset[0];
    matrix[13] += offset[1];
    matrix[14] += offset[2];
}

Vec3 TransformDirectionByRotation(const SceneVector3& rotation, const Vec3& direction)
{
    float rotation_x_matrix[16];
    float rotation_y_matrix[16];
    float rotation_z_matrix[16];
    float temp_a[16];
    float rotation_matrix[16];

    BuildRotationXMatrix(DegreesToRadians(rotation[0]), rotation_x_matrix);
    BuildRotationYMatrix(DegreesToRadians(rotation[1]), rotation_y_matrix);
    BuildRotationZMatrix(DegreesToRadians(rotation[2]), rotation_z_matrix);

    MultiplyMatrix(rotation_y_matrix, rotation_x_matrix, temp_a);
    MultiplyMatrix(rotation_z_matrix, temp_a, rotation_matrix);

    return Normalize(Vec3{
        rotation_matrix[0] * direction.x + rotation_matrix[4] * direction.y + rotation_matrix[8] * direction.z,
        rotation_matrix[1] * direction.x + rotation_matrix[5] * direction.y + rotation_matrix[9] * direction.z,
        rotation_matrix[2] * direction.x + rotation_matrix[6] * direction.y + rotation_matrix[10] * direction.z});
}

void StoreVec4(const std::array<float, 4>& source, float* destination)
{
    std::memcpy(destination, source.data(), sizeof(float) * 4);
}

bool ProjectWorldPointToScreen(
    const Vec3& world_point,
    const float* view_projection_matrix,
    const ImVec2& viewport_min,
    const ImVec2& viewport_max,
    ImVec2& screen_point)
{
    const float clip_x =
        view_projection_matrix[0] * world_point.x +
        view_projection_matrix[4] * world_point.y +
        view_projection_matrix[8] * world_point.z +
        view_projection_matrix[12];
    const float clip_y =
        view_projection_matrix[1] * world_point.x +
        view_projection_matrix[5] * world_point.y +
        view_projection_matrix[9] * world_point.z +
        view_projection_matrix[13];
    const float clip_z =
        view_projection_matrix[2] * world_point.x +
        view_projection_matrix[6] * world_point.y +
        view_projection_matrix[10] * world_point.z +
        view_projection_matrix[14];
    const float clip_w =
        view_projection_matrix[3] * world_point.x +
        view_projection_matrix[7] * world_point.y +
        view_projection_matrix[11] * world_point.z +
        view_projection_matrix[15];

    if (clip_w <= 0.0001f)
    {
        return false;
    }

    const float ndc_x = clip_x / clip_w;
    const float ndc_y = clip_y / clip_w;
    const float ndc_z = clip_z / clip_w;
    if (ndc_z < -1.0f || ndc_z > 1.0f)
    {
        return false;
    }

    const float viewport_width = viewport_max.x - viewport_min.x;
    const float viewport_height = viewport_max.y - viewport_min.y;
    screen_point.x = viewport_min.x + (ndc_x * 0.5f + 0.5f) * viewport_width;
    screen_point.y = viewport_min.y + (1.0f - (ndc_y * 0.5f + 0.5f)) * viewport_height;
    return true;
}

Vec3 FindPerpendicularAxis(const Vec3& direction)
{
    const Vec3 up_reference = std::abs(direction.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    return Normalize(Cross(direction, up_reference));
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

void DrawSpotLightConeGizmo(
    ImDrawList* draw_list,
    const ImVec2& viewport_min,
    const ImVec2& viewport_max,
    const float* view_projection_matrix,
    const SceneResolvedObjectPose& pose,
    const SceneObjectSpotLightAttributes& spot_light)
{
    if (draw_list == nullptr)
    {
        return;
    }

    const float range = (std::max)(spot_light.range, 0.01f);
    const float outer_radius = std::tan(DegreesToRadians(spot_light.outer_cone_degrees)) * range;
    const float inner_radius = std::tan(DegreesToRadians(spot_light.inner_cone_degrees)) * range;
    const Vec3 origin = TransformPoint(pose.world_matrix.data(), Vec3{0.0f, 0.0f, 0.0f});
    const Vec3 direction = TransformDirectionByMatrix(pose.world_matrix.data(), Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 right = FindPerpendicularAxis(direction);
    const Vec3 up = Normalize(Cross(right, direction));
    const Vec3 outer_center = Add(origin, Multiply(direction, range));
    const Vec3 inner_center = Add(origin, Multiply(direction, range * 0.65f));
    const float inner_preview_radius = inner_radius * 0.65f;

    constexpr int kSegmentCount = 24;
    ImVec2 projected_origin;
    ImVec2 projected_axis_end;
    if (ProjectWorldPointToScreen(origin, view_projection_matrix, viewport_min, viewport_max, projected_origin))
    {
        const Vec3 axis_end = Add(origin, Multiply(direction, range));
        if (ProjectWorldPointToScreen(axis_end, view_projection_matrix, viewport_min, viewport_max, projected_axis_end))
        {
            draw_list->AddLine(projected_origin, projected_axis_end, IM_COL32(255, 221, 87, 255), 2.0f);
        }
    }

    std::array<ImVec2, kSegmentCount> projected_outer_ring = {};
    std::array<bool, kSegmentCount> outer_visible = {};
    std::array<ImVec2, kSegmentCount> projected_inner_ring = {};
    std::array<bool, kSegmentCount> inner_visible = {};

    for (int segment_index = 0; segment_index < kSegmentCount; ++segment_index)
    {
        const float angle = (static_cast<float>(segment_index) / static_cast<float>(kSegmentCount)) * kPi * 2.0f;
        const float circle_cos = std::cos(angle);
        const float circle_sin = std::sin(angle);

        const Vec3 outer_point = Add(
            outer_center,
            Add(Multiply(right, outer_radius * circle_cos), Multiply(up, outer_radius * circle_sin)));
        outer_visible[segment_index] = ProjectWorldPointToScreen(outer_point, view_projection_matrix, viewport_min, viewport_max, projected_outer_ring[segment_index]);

        const Vec3 inner_point = Add(
            inner_center,
            Add(Multiply(right, inner_preview_radius * circle_cos), Multiply(up, inner_preview_radius * circle_sin)));
        inner_visible[segment_index] = ProjectWorldPointToScreen(inner_point, view_projection_matrix, viewport_min, viewport_max, projected_inner_ring[segment_index]);
    }

    for (int segment_index = 0; segment_index < kSegmentCount; ++segment_index)
    {
        const int next_index = (segment_index + 1) % kSegmentCount;
        if (outer_visible[segment_index] && outer_visible[next_index])
        {
            draw_list->AddLine(projected_outer_ring[segment_index], projected_outer_ring[next_index], IM_COL32(255, 221, 87, 220), 1.8f);
        }
        if (inner_visible[segment_index] && inner_visible[next_index])
        {
            draw_list->AddLine(projected_inner_ring[segment_index], projected_inner_ring[next_index], IM_COL32(255, 243, 176, 150), 1.2f);
        }
    }

    if (ProjectWorldPointToScreen(origin, view_projection_matrix, viewport_min, viewport_max, projected_origin))
    {
        for (int segment_index = 0; segment_index < kSegmentCount; segment_index += 6)
        {
            if (outer_visible[segment_index])
            {
                draw_list->AddLine(projected_origin, projected_outer_ring[segment_index], IM_COL32(255, 221, 87, 200), 1.4f);
            }
        }

        draw_list->AddCircleFilled(projected_origin, 4.0f, IM_COL32(255, 221, 87, 230), 12);
    }
}

void DrawProjectedSegment(
    ImDrawList* draw_list,
    const float* view_projection_matrix,
    const ImVec2& viewport_min,
    const ImVec2& viewport_max,
    const Vec3& start,
    const Vec3& end,
    ImU32 color,
    float thickness)
{
    if (draw_list == nullptr)
    {
        return;
    }

    ImVec2 projected_start;
    ImVec2 projected_end;
    if (ProjectWorldPointToScreen(start, view_projection_matrix, viewport_min, viewport_max, projected_start)
        && ProjectWorldPointToScreen(end, view_projection_matrix, viewport_min, viewport_max, projected_end))
    {
        draw_list->AddLine(projected_start, projected_end, color, thickness);
    }
}

void DrawWireCircle(
    ImDrawList* draw_list,
    const float* view_projection_matrix,
    const ImVec2& viewport_min,
    const ImVec2& viewport_max,
    const Vec3& center,
    const Vec3& axis_a,
    const Vec3& axis_b,
    float radius,
    ImU32 color,
    float thickness)
{
    if (radius <= 0.0001f)
    {
        return;
    }

    constexpr int kCircleSegments = 32;
    Vec3 previous = Add(
        center,
        Add(Multiply(axis_a, radius), Multiply(axis_b, 0.0f)));

    for (int segment = 1; segment <= kCircleSegments; ++segment)
    {
        const float angle = (static_cast<float>(segment) / static_cast<float>(kCircleSegments)) * kPi * 2.0f;
        const Vec3 current = Add(
            center,
            Add(Multiply(axis_a, std::cos(angle) * radius), Multiply(axis_b, std::sin(angle) * radius)));
        DrawProjectedSegment(
            draw_list,
            view_projection_matrix,
            viewport_min,
            viewport_max,
            previous,
            current,
            color,
            thickness);
        previous = current;
    }
}

void DrawWireArc(
    ImDrawList* draw_list,
    const float* view_projection_matrix,
    const ImVec2& viewport_min,
    const ImVec2& viewport_max,
    const Vec3& center,
    const Vec3& axis_a,
    const Vec3& axis_b,
    float radius,
    float start_angle,
    float end_angle,
    ImU32 color,
    float thickness)
{
    if (radius <= 0.0001f)
    {
        return;
    }

    constexpr int kArcSegments = 24;
    Vec3 previous = Add(
        center,
        Add(Multiply(axis_a, std::cos(start_angle) * radius), Multiply(axis_b, std::sin(start_angle) * radius)));

    for (int segment = 1; segment <= kArcSegments; ++segment)
    {
        const float t = static_cast<float>(segment) / static_cast<float>(kArcSegments);
        const float angle = start_angle + (end_angle - start_angle) * t;
        const Vec3 current = Add(
            center,
            Add(Multiply(axis_a, std::cos(angle) * radius), Multiply(axis_b, std::sin(angle) * radius)));
        DrawProjectedSegment(
            draw_list,
            view_projection_matrix,
            viewport_min,
            viewport_max,
            previous,
            current,
            color,
            thickness);
        previous = current;
    }
}

void DrawPhysicsBoxColliderGizmo(
    ImDrawList* draw_list,
    const float* view_projection_matrix,
    const ImVec2& viewport_min,
    const ImVec2& viewport_max,
    const float* model_matrix,
    const SceneVector3& half_extent,
    ImU32 color,
    float thickness)
{
    const float hx = (std::max)(0.01f, half_extent[0]);
    const float hy = (std::max)(0.01f, half_extent[1]);
    const float hz = (std::max)(0.01f, half_extent[2]);

    const std::array<Vec3, 8> local_corners = {
        Vec3{-hx, -hy, -hz}, Vec3{hx, -hy, -hz},
        Vec3{-hx, hy, -hz}, Vec3{hx, hy, -hz},
        Vec3{-hx, -hy, hz}, Vec3{hx, -hy, hz},
        Vec3{-hx, hy, hz}, Vec3{hx, hy, hz}};

    std::array<Vec3, 8> world_corners = {};
    for (std::size_t i = 0; i < local_corners.size(); ++i)
    {
        world_corners[i] = TransformPoint(model_matrix, local_corners[i]);
    }

    constexpr int kEdges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}};

    for (const auto& edge : kEdges)
    {
        DrawProjectedSegment(
            draw_list,
            view_projection_matrix,
            viewport_min,
            viewport_max,
            world_corners[edge[0]],
            world_corners[edge[1]],
            color,
            thickness);
    }
}

void DrawPhysicsSphereColliderGizmo(
    ImDrawList* draw_list,
    const float* view_projection_matrix,
    const ImVec2& viewport_min,
    const ImVec2& viewport_max,
    const float* model_matrix,
    float local_radius,
    ImU32 color,
    float thickness)
{
    const Vec3 center = TransformPoint(model_matrix, Vec3{0.0f, 0.0f, 0.0f});
    const Vec3 axis_x = Vec3{model_matrix[0], model_matrix[1], model_matrix[2]};
    const Vec3 axis_y = Vec3{model_matrix[4], model_matrix[5], model_matrix[6]};
    const Vec3 axis_z = Vec3{model_matrix[8], model_matrix[9], model_matrix[10]};

    const float scale_x = Length(axis_x);
    const float scale_y = Length(axis_y);
    const float scale_z = Length(axis_z);
    const float world_radius = (std::max)(0.01f, local_radius * (std::max)(scale_x, (std::max)(scale_y, scale_z)));

    DrawWireCircle(
        draw_list,
        view_projection_matrix,
        viewport_min,
        viewport_max,
        center,
        Normalize(axis_x),
        Normalize(axis_y),
        world_radius,
        color,
        thickness);
    DrawWireCircle(
        draw_list,
        view_projection_matrix,
        viewport_min,
        viewport_max,
        center,
        Normalize(axis_y),
        Normalize(axis_z),
        world_radius,
        color,
        thickness);
    DrawWireCircle(
        draw_list,
        view_projection_matrix,
        viewport_min,
        viewport_max,
        center,
        Normalize(axis_z),
        Normalize(axis_x),
        world_radius,
        color,
        thickness);
}

void DrawPhysicsCapsuleColliderGizmo(
    ImDrawList* draw_list,
    const float* view_projection_matrix,
    const ImVec2& viewport_min,
    const ImVec2& viewport_max,
    const float* model_matrix,
    float local_radius,
    float local_half_height,
    ImU32 color,
    float thickness)
{
    const Vec3 axis_x = Vec3{model_matrix[0], model_matrix[1], model_matrix[2]};
    const Vec3 axis_y = Vec3{model_matrix[4], model_matrix[5], model_matrix[6]};
    const Vec3 axis_z = Vec3{model_matrix[8], model_matrix[9], model_matrix[10]};

    const float scale_x = Length(axis_x);
    const float scale_y = Length(axis_y);
    const float scale_z = Length(axis_z);
    const float world_radius = (std::max)(0.01f, local_radius * (std::max)(scale_x, scale_z));
    const float world_half_height = (std::max)(0.0f, local_half_height * scale_y);

    const Vec3 up = Normalize(axis_y);
    const Vec3 right = Normalize(axis_x);
    const Vec3 forward = Normalize(axis_z);
    const Vec3 center = TransformPoint(model_matrix, Vec3{0.0f, 0.0f, 0.0f});
    const Vec3 top_center = Add(center, Multiply(up, world_half_height));
    const Vec3 bottom_center = Add(center, Multiply(up, -world_half_height));

    DrawWireCircle(draw_list, view_projection_matrix, viewport_min, viewport_max, top_center, right, forward, world_radius, color, thickness);
    DrawWireCircle(draw_list, view_projection_matrix, viewport_min, viewport_max, bottom_center, right, forward, world_radius, color, thickness);

    DrawWireArc(draw_list, view_projection_matrix, viewport_min, viewport_max, top_center, right, up, world_radius, 0.0f, kPi, color, thickness);
    DrawWireArc(draw_list, view_projection_matrix, viewport_min, viewport_max, top_center, forward, up, world_radius, 0.0f, kPi, color, thickness);
    DrawWireArc(draw_list, view_projection_matrix, viewport_min, viewport_max, bottom_center, right, up, world_radius, kPi, kPi * 2.0f, color, thickness);
    DrawWireArc(draw_list, view_projection_matrix, viewport_min, viewport_max, bottom_center, forward, up, world_radius, kPi, kPi * 2.0f, color, thickness);

    const Vec3 side_offsets[4] = {
        Multiply(right, world_radius),
        Multiply(right, -world_radius),
        Multiply(forward, world_radius),
        Multiply(forward, -world_radius),
    };
    for (const Vec3& side_offset : side_offsets)
    {
        DrawProjectedSegment(
            draw_list,
            view_projection_matrix,
            viewport_min,
            viewport_max,
            Add(bottom_center, side_offset),
            Add(top_center, side_offset),
            color,
            thickness);
    }
}

void DrawPhysicsColliderGizmos(
    ImDrawList* draw_list,
    const SceneMetadata& scene_metadata,
    const SceneResolvedObjectPoseMap& resolved_object_poses,
    const float* view_projection_matrix,
    const ImVec2& viewport_min,
    const ImVec2& viewport_max,
    const std::string& selected_object_name)
{
    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        if (object.physics_shape == SceneObjectPhysicsShape::None)
        {
            continue;
        }

        std::array<float, 16> fallback_world_matrix = {};
        const float* model_matrix = nullptr;
        const auto pose_it = resolved_object_poses.find(object.name);
        if (pose_it != resolved_object_poses.end())
        {
            model_matrix = pose_it->second.world_matrix.data();
        }
        else
        {
            BuildTransformMatrix(object.position, object.rotation, object.scale, fallback_world_matrix.data());
            model_matrix = fallback_world_matrix.data();
        }

        const bool is_selected = !selected_object_name.empty() && selected_object_name == object.name;
        const ImU32 base_color = object.physics_is_trigger
            ? IM_COL32(130, 255, 130, 240)
            : (object.physics_is_dynamic
                ? IM_COL32(255, 182, 66, 240)
                : IM_COL32(70, 220, 255, 240));
        const ImU32 color = is_selected ? IM_COL32(255, 255, 255, 250) : base_color;
        const float thickness = is_selected ? 2.2f : 1.4f;

        if (object.physics_shape == SceneObjectPhysicsShape::Box)
        {
            DrawPhysicsBoxColliderGizmo(
                draw_list,
                view_projection_matrix,
                viewport_min,
                viewport_max,
                model_matrix,
                object.physics_half_extent,
                color,
                thickness);
        }
        else if (object.physics_shape == SceneObjectPhysicsShape::Sphere)
        {
            DrawPhysicsSphereColliderGizmo(
                draw_list,
                view_projection_matrix,
                viewport_min,
                viewport_max,
                model_matrix,
                object.physics_radius,
                color,
                thickness);
        }
        else if (object.physics_shape == SceneObjectPhysicsShape::Capsule)
        {
            DrawPhysicsCapsuleColliderGizmo(
                draw_list,
                view_projection_matrix,
                viewport_min,
                viewport_max,
                model_matrix,
                object.physics_radius,
                object.physics_capsule_half_height,
                color,
                thickness);
        }
    }
}

struct ColliderResizeDragState
{
    bool active = false;
    std::string object_name;
    std::size_t attribute_index = 0;
    SceneObjectPhysicsShape shape = SceneObjectPhysicsShape::None;
    int handle_axis = 0;
    SceneVector3 start_half_extent = {0.5f, 0.5f, 0.5f};
    float start_radius = 0.5f;
    ImVec2 start_mouse = ImVec2(0.0f, 0.0f);
    ImVec2 start_center_screen = ImVec2(0.0f, 0.0f);
    ImVec2 start_handle_screen = ImVec2(0.0f, 0.0f);
};

bool FindRigidbodyAttributeIndex(const SceneObjectMetadata& object, std::size_t& attribute_index)
{
    for (std::size_t index = 0; index < object.attributes.size(); ++index)
    {
        if (object.attributes[index].kind == SceneObjectAttributeKind::Rigidbody)
        {
            attribute_index = index;
            return true;
        }
    }

    return false;
}

bool DrawAndHandleSelectedColliderResize(
    EngineState& state,
    const SceneObjectMetadata& object,
    std::size_t rigidbody_attribute_index,
    const SceneResolvedObjectPose& pose,
    const float* view_projection_matrix,
    const ImVec2& viewport_min,
    const ImVec2& viewport_max,
    bool mouse_over_viewport,
    bool block_interaction)
{
    static ColliderResizeDragState drag_state;

    if (!ImGui::GetIO().MouseDown[ImGuiMouseButton_Left])
    {
        drag_state.active = false;
    }

    if (object.physics_shape == SceneObjectPhysicsShape::None)
    {
        return false;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 center_screen = ImVec2(0.0f, 0.0f);
    const Vec3 center_world = TransformPoint(pose.world_matrix.data(), Vec3{0.0f, 0.0f, 0.0f});
    if (!ProjectWorldPointToScreen(center_world, view_projection_matrix, viewport_min, viewport_max, center_screen))
    {
        return false;
    }

    constexpr float kHandleRadius = 5.5f;
    constexpr float kHitRadius = 11.0f;
    const ImU32 handle_color = IM_COL32(140, 230, 170, 255);
    const ImU32 handle_hover_color = IM_COL32(220, 255, 200, 255);

    struct HandleCandidate
    {
        int axis = 0;
        ImVec2 screen = ImVec2(0.0f, 0.0f);
    };
    std::vector<HandleCandidate> handles;

    if (object.physics_shape == SceneObjectPhysicsShape::Sphere)
    {
        ImVec2 handle_screen = ImVec2(0.0f, 0.0f);
        const Vec3 handle_world = TransformPoint(
            pose.world_matrix.data(),
            Vec3{(std::max)(0.01f, object.physics_radius), 0.0f, 0.0f});
        if (ProjectWorldPointToScreen(handle_world, view_projection_matrix, viewport_min, viewport_max, handle_screen))
        {
            handles.push_back({0, handle_screen});
        }
    }
    else if (object.physics_shape == SceneObjectPhysicsShape::Box)
    {
        const SceneVector3 half = object.physics_half_extent;
        const Vec3 local_handles[3] = {
            Vec3{(std::max)(0.01f, half[0]), 0.0f, 0.0f},
            Vec3{0.0f, (std::max)(0.01f, half[1]), 0.0f},
            Vec3{0.0f, 0.0f, (std::max)(0.01f, half[2])}};

        for (int axis = 0; axis < 3; ++axis)
        {
            ImVec2 handle_screen = ImVec2(0.0f, 0.0f);
            const Vec3 handle_world = TransformPoint(pose.world_matrix.data(), local_handles[axis]);
            if (ProjectWorldPointToScreen(handle_world, view_projection_matrix, viewport_min, viewport_max, handle_screen))
            {
                handles.push_back({axis, handle_screen});
            }
        }
    }

    int hovered_handle_index = -1;
    const ImVec2 mouse = ImGui::GetMousePos();
    for (int i = 0; i < static_cast<int>(handles.size()); ++i)
    {
        const float dx = mouse.x - handles[static_cast<std::size_t>(i)].screen.x;
        const float dy = mouse.y - handles[static_cast<std::size_t>(i)].screen.y;
        const float distance = std::sqrt(dx * dx + dy * dy);
        if (distance <= kHitRadius)
        {
            hovered_handle_index = i;
            break;
        }
    }

    for (int i = 0; i < static_cast<int>(handles.size()); ++i)
    {
        const bool hovered = i == hovered_handle_index;
        draw_list->AddCircleFilled(
            handles[static_cast<std::size_t>(i)].screen,
            kHandleRadius,
            hovered ? handle_hover_color : handle_color,
            16);
        draw_list->AddLine(center_screen, handles[static_cast<std::size_t>(i)].screen, IM_COL32(130, 170, 150, 190), 1.0f);
    }

    bool consumed = drag_state.active;
    if (!block_interaction && mouse_over_viewport && hovered_handle_index >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        const HandleCandidate& handle = handles[static_cast<std::size_t>(hovered_handle_index)];
        drag_state.active = true;
        drag_state.object_name = object.name;
        drag_state.attribute_index = rigidbody_attribute_index;
        drag_state.shape = object.physics_shape;
        drag_state.handle_axis = handle.axis;
        drag_state.start_half_extent = object.physics_half_extent;
        drag_state.start_radius = object.physics_radius;
        drag_state.start_mouse = mouse;
        drag_state.start_center_screen = center_screen;
        drag_state.start_handle_screen = handle.screen;
        consumed = true;
    }

    if (!drag_state.active)
    {
        return consumed;
    }

    if (drag_state.object_name != object.name ||
        drag_state.attribute_index != rigidbody_attribute_index ||
        drag_state.shape != object.physics_shape)
    {
        drag_state.active = false;
        return consumed;
    }

    const ImVec2 axis_screen = ImVec2(
        drag_state.start_handle_screen.x - drag_state.start_center_screen.x,
        drag_state.start_handle_screen.y - drag_state.start_center_screen.y);
    const float axis_length = std::sqrt(axis_screen.x * axis_screen.x + axis_screen.y * axis_screen.y);
    if (axis_length < 1.0f)
    {
        return true;
    }

    const ImVec2 axis_dir = ImVec2(axis_screen.x / axis_length, axis_screen.y / axis_length);
    const ImVec2 mouse_delta = ImVec2(mouse.x - drag_state.start_mouse.x, mouse.y - drag_state.start_mouse.y);
    const float projected_pixels = mouse_delta.x * axis_dir.x + mouse_delta.y * axis_dir.y;

    if (drag_state.shape == SceneObjectPhysicsShape::Sphere)
    {
        const float start_radius = (std::max)(0.01f, drag_state.start_radius);
        const float units_per_pixel = start_radius / axis_length;
        const float new_radius = (std::max)(0.01f, start_radius + projected_pixels * units_per_pixel);
        if (std::abs(new_radius - object.physics_radius) > 0.0005f)
        {
            SetSceneObjectAttributePhysicsRadius(state.active_scene_path, object.name, rigidbody_attribute_index, new_radius);
        }
    }
    else if (drag_state.shape == SceneObjectPhysicsShape::Box)
    {
        SceneVector3 new_half_extent = drag_state.start_half_extent;
        const int axis = drag_state.handle_axis;
        const float start_axis_value = (std::max)(0.01f, drag_state.start_half_extent[static_cast<std::size_t>(axis)]);
        const float units_per_pixel = (std::max)(start_axis_value, 0.1f) / axis_length;
        new_half_extent[static_cast<std::size_t>(axis)] = (std::max)(0.01f, start_axis_value + projected_pixels * units_per_pixel);

        const float diff = std::abs(new_half_extent[0] - object.physics_half_extent[0]) +
            std::abs(new_half_extent[1] - object.physics_half_extent[1]) +
            std::abs(new_half_extent[2] - object.physics_half_extent[2]);
        if (diff > 0.0008f)
        {
            SetSceneObjectAttributePhysicsHalfExtent(state.active_scene_path, object.name, rigidbody_attribute_index, new_half_extent);
        }
    }

    return true;
}

void ExpandBoundsWithObject(Vec3& minimum, Vec3& maximum, const SceneViewportRenderer::QueuedSceneObject& object, const ModelAsset& asset)
{
    if (!asset.bounds.valid)
    {
        const Vec3 position = ToVec3(object.world_position);
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
        const Vec3 position = ToVec3(object.world_position);
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

bool CreateVulkanBuffer(
    const VulkanContext& context,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags properties,
    SceneViewportRenderer::GpuBuffer& buffer)
{
    const VkPhysicalDevice physical_device = context.GetPhysicalDevice();
    const VkDevice device = context.GetDevice();
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

    VkMemoryAllocateFlagsInfo allocate_flags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0)
    {
        allocate_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        allocate_info.pNext = &allocate_flags;
    }

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

// Batched immediate texture upload --------------------------------------
//
// CreateTextureFromAsset opens a command buffer, submits, and waits once per
// call. Loading a single PBR mesh fires up to ~16 of those round-trips per
// material, which dominates scene-load wall time. The helpers below let the
// caller stage all GPU resources up front and then flush every transfer
// through ONE ExecuteImmediateCommands call.

struct PreparedTextureUpload
{
    SceneViewportRenderer::GpuBuffer staging_buffer{};
    VkImage image = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

bool PrepareTextureUpload(
    VulkanContext& context,
    const ModelTextureAsset& texture_asset,
    SceneViewportRenderer::GpuTexture& out_texture,
    PreparedTextureUpload& out_pending)
{
    if (!texture_asset.valid ||
        texture_asset.width <= 0 ||
        texture_asset.height <= 0 ||
        texture_asset.pixels.empty())
    {
        return false;
    }

    const VkDevice device = context.GetDevice();
    const VkPhysicalDevice physical_device = context.GetPhysicalDevice();
    const VkDeviceSize upload_size = static_cast<VkDeviceSize>(texture_asset.width) *
        static_cast<VkDeviceSize>(texture_asset.height) * 4u;
    const VkFormat texture_format = texture_asset.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    if (!CreateVulkanBuffer(
            context,
            upload_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            out_pending.staging_buffer))
    {
        return false;
    }

    if (!UploadBufferData(device, out_pending.staging_buffer, texture_asset.pixels.data(), static_cast<std::size_t>(upload_size)))
    {
        vkFreeMemory(device, out_pending.staging_buffer.memory, context.GetAllocator());
        vkDestroyBuffer(device, out_pending.staging_buffer.buffer, context.GetAllocator());
        out_pending.staging_buffer = {};
        return false;
    }

    if (!CreateVulkanImage(
            physical_device,
            device,
            static_cast<std::uint32_t>(texture_asset.width),
            static_cast<std::uint32_t>(texture_asset.height),
            texture_format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            out_texture.image,
            out_texture.memory,
            out_texture.view))
    {
        vkFreeMemory(device, out_pending.staging_buffer.memory, context.GetAllocator());
        vkDestroyBuffer(device, out_pending.staging_buffer.buffer, context.GetAllocator());
        out_pending.staging_buffer = {};
        return false;
    }

    out_pending.image = out_texture.image;
    out_pending.width = static_cast<std::uint32_t>(texture_asset.width);
    out_pending.height = static_cast<std::uint32_t>(texture_asset.height);
    return true;
}

void RecordPreparedTextureUpload(VkCommandBuffer command_buffer, const PreparedTextureUpload& pending)
{
    TransitionImageLayout(
        command_buffer,
        pending.image,
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
    copy_region.imageExtent.width = pending.width;
    copy_region.imageExtent.height = pending.height;
    copy_region.imageExtent.depth = 1;

    vkCmdCopyBufferToImage(
        command_buffer,
        pending.staging_buffer.buffer,
        pending.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy_region);

    TransitionImageLayout(
        command_buffer,
        pending.image,
        VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT);
}

void ReleasePreparedTextureUploads(VulkanContext& context, std::vector<PreparedTextureUpload>& uploads)
{
    const VkDevice device = context.GetDevice();
    const VkAllocationCallbacks* allocator = context.GetAllocator();
    for (PreparedTextureUpload& pending : uploads)
    {
        if (pending.staging_buffer.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, pending.staging_buffer.memory, allocator);
        }
        if (pending.staging_buffer.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(device, pending.staging_buffer.buffer, allocator);
        }
        pending = {};
    }
    uploads.clear();
}

bool FlushPreparedTextureUploads(
    VulkanContext& context,
    VkCommandPool command_pool,
    std::vector<PreparedTextureUpload>& uploads)
{
    if (uploads.empty())
    {
        return true;
    }
    if (command_pool == VK_NULL_HANDLE)
    {
        ReleasePreparedTextureUploads(context, uploads);
        return false;
    }

    const bool ok = ExecuteImmediateCommands(
        context.GetDevice(),
        command_pool,
        context.GetQueue(),
        [&](VkCommandBuffer command_buffer)
        {
            for (const PreparedTextureUpload& pending : uploads)
            {
                RecordPreparedTextureUpload(command_buffer, pending);
            }
        });

    ReleasePreparedTextureUploads(context, uploads);
    return ok;
}

bool CreateTextureFromAsset(
    VulkanContext& context,
    VkCommandPool command_pool,
    const ModelTextureAsset& texture_asset,
    SceneViewportRenderer::GpuTexture& texture)
{
    if (command_pool == VK_NULL_HANDLE ||
        !texture_asset.valid ||
        texture_asset.width <= 0 ||
        texture_asset.height <= 0 ||
        texture_asset.pixels.empty())
    {
        return false;
    }

    const VkDevice device = context.GetDevice();
    const VkPhysicalDevice physical_device = context.GetPhysicalDevice();
    const VkDeviceSize upload_size = static_cast<VkDeviceSize>(texture_asset.width) * static_cast<VkDeviceSize>(texture_asset.height) * 4u;
    const VkFormat texture_format = texture_asset.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    SceneViewportRenderer::GpuBuffer staging_buffer{};
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
            physical_device,
            device,
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

bool UpdateSceneObjectTransform(
    const EngineState& state,
    const SceneViewportRenderer::QueuedSceneObject& object,
    const SceneVector3& position,
    const SceneVector3& rotation,
    const SceneVector3& scale,
    bool write_position,
    bool write_rotation,
    bool write_scale)
{
    if (state.active_scene_path.empty() || (!write_position && !write_rotation && !write_scale))
    {
        return false;
    }

    float local_matrix[16];
    BuildTransformMatrix(position, rotation, scale, local_matrix);
    if (object.has_parent_transform)
    {
        float inverse_parent_matrix[16];
        if (InvertMatrix(object.parent_matrix.data(), inverse_parent_matrix))
        {
            float converted_local_matrix[16];
            MultiplyMatrix(inverse_parent_matrix, local_matrix, converted_local_matrix);
            std::memcpy(local_matrix, converted_local_matrix, sizeof(converted_local_matrix));
        }
    }

    float local_position_components[3] = {};
    float local_rotation_components[3] = {};
    float local_scale_components[3] = {};
    ImGuizmo::DecomposeMatrixToComponents(local_matrix, local_position_components, local_rotation_components, local_scale_components);
    const SceneVector3 local_position = {local_position_components[0], local_position_components[1], local_position_components[2]};
    const SceneVector3 local_rotation = {local_rotation_components[0], local_rotation_components[1], local_rotation_components[2]};
    const SceneVector3 local_scale = {local_scale_components[0], local_scale_components[1], local_scale_components[2]};

    return SetSceneObjectTransform(
        state.active_scene_path,
        object.name,
        local_position,
        local_rotation,
        local_scale,
        write_position,
        write_rotation,
        write_scale);
}

void ResolveLocalSceneObjectTransform(
    const SceneViewportRenderer::QueuedSceneObject& object,
    const SceneVector3& position,
    const SceneVector3& rotation,
    const SceneVector3& scale,
    SceneVector3& local_position,
    SceneVector3& local_rotation,
    SceneVector3& local_scale)
{
    float local_matrix[16];
    BuildTransformMatrix(position, rotation, scale, local_matrix);
    if (object.has_parent_transform)
    {
        float inverse_parent_matrix[16];
        if (InvertMatrix(object.parent_matrix.data(), inverse_parent_matrix))
        {
            float converted_local_matrix[16];
            MultiplyMatrix(inverse_parent_matrix, local_matrix, converted_local_matrix);
            std::memcpy(local_matrix, converted_local_matrix, sizeof(converted_local_matrix));
        }
    }

    float local_position_components[3] = {};
    float local_rotation_components[3] = {};
    float local_scale_components[3] = {};
    ImGuizmo::DecomposeMatrixToComponents(local_matrix, local_position_components, local_rotation_components, local_scale_components);
    local_position = {local_position_components[0], local_position_components[1], local_position_components[2]};
    local_rotation = {local_rotation_components[0], local_rotation_components[1], local_rotation_components[2]};
    local_scale = {local_scale_components[0], local_scale_components[1], local_scale_components[2]};
}

bool HasSignificantSceneVectorDelta(const SceneVector3& left, const SceneVector3& right, float epsilon)
{
    return std::abs(left[0] - right[0]) > epsilon ||
           std::abs(left[1] - right[1]) > epsilon ||
           std::abs(left[2] - right[2]) > epsilon;
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

    const float base_distance = ComputeOrbitBaseDistance(scene_radius);
    camera_state.zoom = (std::max)(base_distance / distance, 0.001f);
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

constexpr float kViewportHeaderButtonHeight = 24.0f;
constexpr float kViewportHeaderButtonRounding = 5.0f;
constexpr float kViewportHeaderButtonHorizontalPadding = 8.0f;
constexpr float kViewportHeaderButtonVerticalPadding = 4.0f;
constexpr float kViewportHeaderButtonMinWidth = 28.0f;

struct ViewportResolutionPreset
{
    const char* label = "";
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

constexpr std::array<ViewportResolutionPreset, 7> kViewportResolutionPresets = {{
    {"Free", 0, 0},
    {"1280 x 720 (16:9)", 1280, 720},
    {"1920 x 1080 (16:9)", 1920, 1080},
    {"2560 x 1080 (21:9)", 2560, 1080},
    {"3440 x 1440 (21:9)", 3440, 1440},
    {"3840 x 1080 (32:9)", 3840, 1080},
    {"5120 x 1440 (32:9)", 5120, 1440},
}};

float ComputeViewportAspectRatio(const ViewportResolutionPreset& preset)
{
    if (preset.width == 0 || preset.height == 0)
    {
        return 0.0f;
    }

    return static_cast<float>(preset.width) / static_cast<float>(preset.height);
}

int FindBestViewportResolutionPresetIndex(std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return 0;
    }

    for (std::size_t index = 1; index < kViewportResolutionPresets.size(); ++index)
    {
        const ViewportResolutionPreset& preset = kViewportResolutionPresets[index];
        if (preset.width == width && preset.height == height)
        {
            return static_cast<int>(index);
        }
    }

    const float source_aspect = static_cast<float>(width) / static_cast<float>(height);
    float smallest_delta = std::numeric_limits<float>::max();
    int best_index = 0;
    for (std::size_t index = 1; index < kViewportResolutionPresets.size(); ++index)
    {
        const float preset_aspect = ComputeViewportAspectRatio(kViewportResolutionPresets[index]);
        const float delta = std::abs(preset_aspect - source_aspect);
        if (delta < smallest_delta)
        {
            smallest_delta = delta;
            best_index = static_cast<int>(index);
        }
    }

    return best_index;
}

float ComputeViewportHeaderButtonWidth(const char* label)
{
    const float button_width = ImGui::CalcTextSize(label).x + kViewportHeaderButtonHorizontalPadding * 2.0f;
    return (std::max)(button_width, kViewportHeaderButtonMinWidth);
}

bool DrawViewportHeaderButton(const char* label, const char* tooltip)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kViewportHeaderButtonRounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(kViewportHeaderButtonHorizontalPadding, kViewportHeaderButtonVerticalPadding));

    const bool pressed = ImGui::Button(label, ImVec2(ComputeViewportHeaderButtonWidth(label), kViewportHeaderButtonHeight));
    const bool hovered = ImGui::IsItemHovered();

    if (hovered && tooltip != nullptr && tooltip[0] != '\0')
    {
        ImGui::SetTooltip("%s", tooltip);
    }

    ImGui::PopStyleVar(2);

    return pressed;
}

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
    ray_tracing_.Initialize(context);
    ray_tracing_.SetTAAEnabled(true);
    scene_2d_renderer_.Initialize(context);
    video_playback_manager_.Initialize(&scene_2d_renderer_);
    scene_2d_renderer_.SetVideoPlaybackManager(&video_playback_manager_);
    skybox_renderer_.Initialize(context);
    return vulkan_context_ != nullptr;
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
    for (GpuMaterialTextures& material_textures : entry.material_textures)
    {
        ReleaseTexture(material_textures.base_color);
        ReleaseTexture(material_textures.metallic_roughness);
        ReleaseTexture(material_textures.normal);
        ReleaseTexture(material_textures.occlusion);
        ReleaseTexture(material_textures.emissive);
        ReleaseTexture(material_textures.transmission);
        ReleaseTexture(material_textures.specular);
        ReleaseTexture(material_textures.specular_color);
        ReleaseTexture(material_textures.sheen_color);
        ReleaseTexture(material_textures.sheen_roughness);
        ReleaseTexture(material_textures.iridescence);
        ReleaseTexture(material_textures.iridescence_thickness);
        ReleaseTexture(material_textures.volume_thickness);
        ReleaseTexture(material_textures.clearcoat);
        ReleaseTexture(material_textures.clearcoat_roughness);
        ReleaseTexture(material_textures.clearcoat_normal);
    }

    entry.vertex_count = 0;
    entry.index_count = 0;
    entry.sections.clear();
    entry.materials.clear();
    entry.material_textures.clear();
}

void SceneViewportRenderer::Shutdown()
{
    skybox_renderer_.Shutdown();
    scene_2d_renderer_.SetVideoPlaybackManager(nullptr);
    video_playback_manager_.Shutdown();
    scene_2d_renderer_.Shutdown();
    ray_tracing_.Shutdown();

    for (auto& mesh_entry : mesh_cache_)
    {
        ReleaseMeshCacheEntry(mesh_entry.second);
    }
    mesh_cache_.clear();

    queued_objects_.clear();
    render_requested_ = false;
    vulkan_context_ = nullptr;
}

void SceneViewportRenderer::BeginFrame()
{
    render_requested_ = false;
    queued_objects_.clear();
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

    //const std::uint64_t mesh_upload_start_ticks = SDL_GetPerformanceCounter();

    ReleaseMeshCacheEntry(cache_entry);

    std::vector<SceneGpuVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(4096);
    indices.reserve(8192);

    cache_entry.material_textures.resize(resolved_model.asset->materials.size());
    cache_entry.materials.resize(resolved_model.asset->materials.size());

    // Stage every material texture into a single immediate command buffer so a
    // PBR mesh costs ONE GPU round-trip (was up to 16 per material).
    std::vector<PreparedTextureUpload> pending_uploads;
    pending_uploads.reserve(resolved_model.asset->materials.size() * 16);
    auto stage_texture = [&](const ModelTextureAsset& asset, SceneViewportRenderer::GpuTexture& out_texture)
    {
        if (!asset.valid)
        {
            return;
        }
        PreparedTextureUpload pending;
        if (PrepareTextureUpload(*vulkan_context_, asset, out_texture, pending))
        {
            pending_uploads.push_back(std::move(pending));
        }
    };
    for (std::size_t material_index = 0; material_index < resolved_model.asset->materials.size(); ++material_index)
    {
        const ModelMaterialAsset& material = resolved_model.asset->materials[material_index];
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
        stage_texture(material.base_color_texture, cache_entry.material_textures[material_index].base_color);
        stage_texture(material.metallic_roughness_texture, cache_entry.material_textures[material_index].metallic_roughness);
        stage_texture(material.normal_texture, cache_entry.material_textures[material_index].normal);
        stage_texture(material.occlusion_texture, cache_entry.material_textures[material_index].occlusion);
        stage_texture(material.emissive_texture, cache_entry.material_textures[material_index].emissive);
        stage_texture(material.transmission_texture, cache_entry.material_textures[material_index].transmission);
        stage_texture(material.specular_texture, cache_entry.material_textures[material_index].specular);
        stage_texture(material.specular_color_texture, cache_entry.material_textures[material_index].specular_color);
        stage_texture(material.sheen_color_texture, cache_entry.material_textures[material_index].sheen_color);
        stage_texture(material.sheen_roughness_texture, cache_entry.material_textures[material_index].sheen_roughness);
        stage_texture(material.iridescence_texture, cache_entry.material_textures[material_index].iridescence);
        stage_texture(material.iridescence_thickness_texture, cache_entry.material_textures[material_index].iridescence_thickness);
        stage_texture(material.volume_thickness_texture, cache_entry.material_textures[material_index].volume_thickness);
        stage_texture(material.clearcoat_texture, cache_entry.material_textures[material_index].clearcoat);
        stage_texture(material.clearcoat_roughness_texture, cache_entry.material_textures[material_index].clearcoat_roughness);
        stage_texture(material.clearcoat_normal_texture, cache_entry.material_textures[material_index].clearcoat_normal);

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

    // Single submit + single fence wait for ALL material texture uploads.
    FlushPreparedTextureUploads(*vulkan_context_, ray_tracing_.GetCommandPool(), pending_uploads);

    for (const ModelMeshAsset& mesh : resolved_model.asset->meshes)
    {
        GpuMeshSection section;
        section.first_index = static_cast<std::uint32_t>(indices.size());
        section.material_index = mesh.material_index;
        const std::uint32_t base_vertex = static_cast<std::uint32_t>(vertices.size());
        const ModelMaterialAsset* material = mesh.material_index < resolved_model.asset->materials.size() ? &resolved_model.asset->materials[mesh.material_index] : nullptr;
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
        cache_entry.sections.clear();
        return false;
    }

    const VkDeviceSize vertex_buffer_size = static_cast<VkDeviceSize>(vertices.size() * sizeof(SceneGpuVertex));
    const VkDeviceSize index_buffer_size = static_cast<VkDeviceSize>(indices.size() * sizeof(std::uint32_t));
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
            vertex_buffer_size,
            vertex_usage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            cache_entry.vertex_buffer))
    {
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    if (!CreateVulkanBuffer(
            *vulkan_context_,
            index_buffer_size,
            index_usage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            cache_entry.index_buffer))
    {
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    if (!UploadBufferData(vulkan_context_->GetDevice(), cache_entry.vertex_buffer, vertices.data(), vertices.size() * sizeof(SceneGpuVertex)) ||
        !UploadBufferData(vulkan_context_->GetDevice(), cache_entry.index_buffer, indices.data(), indices.size() * sizeof(std::uint32_t)))
    {
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    cache_entry.vertex_count = static_cast<std::uint32_t>(vertices.size());
    cache_entry.index_count = static_cast<std::uint32_t>(indices.size());
    cache_entry.write_time = resolved_model.write_time;

    //const std::uint64_t freq = SDL_GetPerformanceFrequency();
    // if (freq > 0)
    // {
    //     const double upload_ms = static_cast<double>(SDL_GetPerformanceCounter() - mesh_upload_start_ticks) * 1000.0 / static_cast<double>(freq);
    //     SDL_Log(
    //         "Viewport mesh upload '%s': %.2f ms (%zu materials, %u verts, %u indices)",
    //         model_path.filename().string().c_str(),
    //         upload_ms,
    //         resolved_model.asset->materials.size(),
    //         cache_entry.vertex_count,
    //         cache_entry.index_count);
    // }
    return true;
}

void SceneViewportRenderer::SyncRayTracingScene()
{
    if (!ray_tracing_.IsAvailable())
    {
        return;
    }

    std::vector<RayTracing::MeshInput> mesh_inputs;
    std::vector<RayTracing::InstanceInput> instance_inputs;
    std::unordered_map<std::string, std::size_t> mesh_index_by_key;

    mesh_inputs.reserve(queued_objects_.size());
    instance_inputs.reserve(queued_objects_.size());
    mesh_index_by_key.reserve(queued_objects_.size());

    for (const QueuedSceneObject& object : queued_objects_)
    {
        const auto mesh_entry_it = mesh_cache_.find(object.model_path);
        if (mesh_entry_it == mesh_cache_.end())
        {
            continue;
        }

        const GpuMeshCacheEntry& mesh_entry = mesh_entry_it->second;
        if (mesh_entry.vertex_buffer.device_address == 0 ||
            mesh_entry.index_buffer.device_address == 0 ||
            mesh_entry.vertex_count == 0 ||
            mesh_entry.index_count < 3)
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
            mesh_input.sections.reserve(mesh_entry.sections.size());
            for (const GpuMeshSection& section : mesh_entry.sections)
            {
                mesh_input.sections.push_back(RayTracing::MeshSectionRecord{
                    section.first_index,
                    section.index_count,
                    section.material_index,
                    section.uses_alpha_transparency});
            }
            mesh_input.materials = mesh_entry.materials;

            mesh_index_by_key.emplace(mesh_key, mesh_inputs.size());
            mesh_inputs.push_back(std::move(mesh_input));
        }

        RayTracing::InstanceInput instance_input;
        instance_input.key = object.name;
        instance_input.mesh_key = mesh_key;
        BuildModelMatrix(object, instance_input.transform.data());
        instance_inputs.push_back(std::move(instance_input));
    }

    if (!ray_tracing_.UpdateScene(mesh_inputs, instance_inputs))
    {
        SDL_Log("RayTracing::UpdateScene failed: %s", ray_tracing_.GetStatusMessage().c_str());
    }
}

void SceneViewportRenderer::RenderUi(
    EngineState& state,
    const SceneMetadata& scene_metadata,
    const SceneViewportModelResolver& resolve_model_asset,
    SceneViewportCameraState& camera_state)
{
    // Push the editor-tunable TAA debug knobs into the RT subsystem every
    // frame so SettingsPanel sliders are reflected immediately.
    ray_tracing_.SetTAAEnabled(state.taa_enabled);
    RayTracing::TaaDebugSettings taa_dbg{};
    taa_dbg.viz_mode = state.taa_viz_mode;
    taa_dbg.variance_scale = state.taa_variance_scale;
    taa_dbg.variance_scale_moving = state.taa_variance_scale_moving;
    taa_dbg.anti_sparkle = state.taa_anti_sparkle;
    taa_dbg.history_blend = state.taa_history_blend;
    taa_dbg.jitter_compensation = state.taa_jitter_compensation;
    taa_dbg.adaptive_enabled = state.taa_adaptive_enabled;
    taa_dbg.adaptive_max_samples = state.taa_adaptive_max_samples;
    taa_dbg.adaptive_threshold = state.taa_adaptive_threshold;
    taa_dbg.adaptive_preservation = state.taa_adaptive_preservation;
    ray_tracing_.SetTaaDebugSettings(taa_dbg);

    constexpr const char* kBuildButtonLabel = ICON_CI_RUN_WITH_DEPS;
    constexpr const char* kStopBuildButtonLabel = ICON_CI_STOP_CIRCLE;
    constexpr const char* kPlayButtonLabel = ICON_CI_PLAY;
    constexpr const char* kCloseRuntimeButtonLabel = ICON_CI_CLOSE_ALL;
    constexpr const char* kVisibilityButtonLabel = ICON_CI_EYE " " ICON_CI_CHEVRON_DOWN;

    const float header_spacing = ImGui::GetStyle().ItemSpacing.x;
    const bool build_running = state.is_build_running;
    const bool runtime_playing = state.is_playing;
    const char* build_button_label = build_running ? kStopBuildButtonLabel : kBuildButtonLabel;
    const char* build_button_tooltip = build_running ? "Stop Build" : "Build";
    const char* play_button_label = runtime_playing ? kCloseRuntimeButtonLabel : kPlayButtonLabel;
    const char* play_button_tooltip = runtime_playing ? "Close Runtime" : "Play";
    const char* visibility_button_tooltip = "Visibility";
    const char* resolution_tooltip = "Viewport aspect preview";
    static std::filesystem::path active_resolution_scene_path;
    static int selected_resolution_preset_index = 2;
    if (state.active_scene_path != active_resolution_scene_path)
    {
        selected_resolution_preset_index = FindBestViewportResolutionPresetIndex(
            scene_metadata.reference_viewport_width,
            scene_metadata.reference_viewport_height);
        active_resolution_scene_path = state.active_scene_path;
    }

    selected_resolution_preset_index = std::clamp(
        selected_resolution_preset_index,
        0,
        static_cast<int>(kViewportResolutionPresets.size()) - 1);

    const float build_button_width = ComputeViewportHeaderButtonWidth(build_button_label);
    const float play_button_width = ComputeViewportHeaderButtonWidth(play_button_label);
    const float header_toolbar_width = build_button_width + play_button_width + header_spacing;

    if (DrawViewportHeaderButton(kVisibilityButtonLabel, visibility_button_tooltip))
    {
        ImGui::OpenPopup("##SceneViewportVisibilityPopup");
    }

    ImGui::SameLine(0.0f, header_spacing);
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::BeginCombo(
            "##SceneViewportResolutionPreset",
            kViewportResolutionPresets[static_cast<std::size_t>(selected_resolution_preset_index)].label,
            ImGuiComboFlags_HeightLargest))
    {
        for (std::size_t index = 0; index < kViewportResolutionPresets.size(); ++index)
        {
            const bool is_selected = static_cast<int>(index) == selected_resolution_preset_index;
            if (ImGui::Selectable(kViewportResolutionPresets[index].label, is_selected))
            {
                selected_resolution_preset_index = static_cast<int>(index);
            }

            if (is_selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }

        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::SetTooltip("%s", resolution_tooltip);
    }

    const float header_toolbar_x = (std::max)(
        ImGui::GetCursorPosX() + header_spacing,
        ImGui::GetWindowContentRegionMax().x - header_toolbar_width);

    ImGui::SameLine(header_toolbar_x);
    if (DrawViewportHeaderButton(build_button_label, build_button_tooltip))
    {
        if (build_running)
        {
            state.TriggerBuildStopAction();
        }
        else
        {
            state.TriggerBuildAction();
        }
    }

    ImGui::SameLine(0.0f, header_spacing);
    if (DrawViewportHeaderButton(play_button_label, play_button_tooltip))
    {
        if (runtime_playing)
        {
            state.TriggerPlayStopAction();
        }
        else
        {
            state.TriggerPlayAction();
        }
    }

    if (ImGui::BeginPopup("##SceneViewportVisibilityPopup"))
    {
        ImGui::Checkbox("Physics Colliders", &show_physics_colliders_);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Draw wireframe collider shapes from PhysicsShape settings");
        }

        ImGui::Checkbox("FPS", &show_fps_);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Show viewport frames per second");
        }

        ImGui::EndPopup();
    }

    ImGui::Separator();

    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x <= 4.0f || available.y <= 4.0f)
    {
        return;
    }

    const ViewportResolutionPreset& selected_preset =
        kViewportResolutionPresets[static_cast<std::size_t>(selected_resolution_preset_index)];
    ImVec2 canvas_size = available;
    const float target_aspect_ratio = ComputeViewportAspectRatio(selected_preset);
    if (target_aspect_ratio > 0.0f)
    {
        const float available_aspect_ratio = available.x / available.y;
        if (available_aspect_ratio > target_aspect_ratio)
        {
            canvas_size.y = available.y;
            canvas_size.x = available.y * target_aspect_ratio;
        }
        else
        {
            canvas_size.x = available.x;
            canvas_size.y = available.x / target_aspect_ratio;
        }
    }

    if (canvas_size.x <= 4.0f || canvas_size.y <= 4.0f)
    {
        return;
    }

    const ImVec2 host_cursor = ImGui::GetCursorPos();
    const ImVec2 canvas_offset(
        (available.x - canvas_size.x) * 0.5f,
        (available.y - canvas_size.y) * 0.5f);
    ImGui::SetCursorPos(ImVec2(host_cursor.x + canvas_offset.x, host_cursor.y + canvas_offset.y));

    ImGui::BeginChild("##SceneViewportCanvas", canvas_size, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = ImVec2(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
    const float viewport_width = max.x - min.x;
    const float viewport_height = max.y - min.y;
    const bool mouse_over_viewport = ImGui::IsMouseHoveringRect(min, max, true);
    const ImGuiIO& io = ImGui::GetIO();

    if (!io.MouseDown[ImGuiMouseButton_Middle])
    {
        middle_mouse_panning_ = false;
    }
    else if (mouse_over_viewport && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    {
        middle_mouse_panning_ = true;
    }

    if (mouse_over_viewport && io.MouseDown[ImGuiMouseButton_Right])
    {
        const ImVec2 delta = io.MouseDelta;
        camera_state.yaw += delta.x * 0.01f;
        camera_state.pitch = std::clamp(camera_state.pitch - delta.y * 0.01f, -1.2f, 1.2f);
    }
    if (mouse_over_viewport && io.MouseWheel != 0.0f)
    {
        const float zoom_factor = std::exp(io.MouseWheel * 0.12f);
        camera_state.zoom = (std::max)(camera_state.zoom * zoom_factor, 0.001f);
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(min, max, IM_COL32(24, 28, 32, 255), 8.0f);

    const ImVec2 framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
    const std::uint32_t target_width = static_cast<std::uint32_t>((std::max)(1.0f, std::round(viewport_width * framebuffer_scale.x)));
    const std::uint32_t target_height = static_cast<std::uint32_t>((std::max)(1.0f, std::round(viewport_height * framebuffer_scale.y)));

    if (vulkan_context_ == nullptr || !ray_tracing_.EnsureViewportOutput(target_width, target_height))
    {
        draw_list->AddRect(min, max, IM_COL32(92, 99, 110, 255), 8.0f, 0, 1.5f);
        draw_list->AddText(ImVec2(min.x + 12.0f, min.y + 12.0f), IM_COL32(235, 238, 242, 255), "Scene GPU renderer unavailable");
        ImGui::EndChild();
        return;
    }

    if (ray_tracing_.GetOutputDescriptorSet() != VK_NULL_HANDLE)
    {
        draw_list->AddImage(
            reinterpret_cast<ImTextureID>(ray_tracing_.GetOutputDescriptorSet()),
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

    SceneMetadata scene_metadata_for_render = scene_metadata;
    if (gizmo_preview_active_)
    {
        const auto preview_object_it = std::find_if(
            scene_metadata_for_render.objects.begin(),
            scene_metadata_for_render.objects.end(),
            [&](const SceneObjectMetadata& object)
            {
                return object.name == gizmo_preview_object_name_;
            });

        if (preview_object_it != scene_metadata_for_render.objects.end())
        {
            preview_object_it->position = gizmo_preview_local_position_;
            preview_object_it->rotation = gizmo_preview_local_rotation_;
            preview_object_it->scale = gizmo_preview_local_scale_;
        }
        else
        {
            gizmo_preview_active_ = false;
            gizmo_preview_object_name_.clear();
            gizmo_preview_write_position_ = false;
            gizmo_preview_write_rotation_ = false;
            gizmo_preview_write_scale_ = false;
        }
    }

    if (overlay_preview_active_)
    {
        const auto preview_object_it = std::find_if(
            scene_metadata_for_render.objects.begin(),
            scene_metadata_for_render.objects.end(),
            [&](const SceneObjectMetadata& object)
            {
                return object.name == overlay_preview_object_name_;
            });

        if (preview_object_it != scene_metadata_for_render.objects.end() &&
            overlay_preview_attribute_index_ < preview_object_it->attributes.size())
        {
            SceneObjectAttribute& preview_attribute = preview_object_it->attributes[overlay_preview_attribute_index_];
            if (overlay_preview_kind_ == SceneObjectAttributeKind::Text2D && preview_attribute.kind == SceneObjectAttributeKind::Text2D)
            {
                if (overlay_preview_write_position_)
                {
                    preview_attribute.text_2d.x = overlay_preview_x_;
                    preview_attribute.text_2d.y = overlay_preview_y_;
                }
                if (overlay_preview_write_size_)
                {
                    preview_attribute.text_2d.width = overlay_preview_w_;
                    preview_attribute.text_2d.height = overlay_preview_h_;
                }
            }
            else if (overlay_preview_kind_ == SceneObjectAttributeKind::Image2D && preview_attribute.kind == SceneObjectAttributeKind::Image2D)
            {
                if (overlay_preview_write_position_)
                {
                    preview_attribute.image_2d.x = overlay_preview_x_;
                    preview_attribute.image_2d.y = overlay_preview_y_;
                }
                if (overlay_preview_write_size_)
                {
                    preview_attribute.image_2d.width = overlay_preview_w_;
                    preview_attribute.image_2d.height = overlay_preview_h_;
                }
            }
            else if (overlay_preview_kind_ == SceneObjectAttributeKind::Color2D && preview_attribute.kind == SceneObjectAttributeKind::Color2D)
            {
                if (overlay_preview_write_position_)
                {
                    preview_attribute.color_2d.x = overlay_preview_x_;
                    preview_attribute.color_2d.y = overlay_preview_y_;
                }
                if (overlay_preview_write_size_)
                {
                    preview_attribute.color_2d.width = overlay_preview_w_;
                    preview_attribute.color_2d.height = overlay_preview_h_;
                }
            }
            else
            {
                overlay_preview_active_ = false;
                overlay_preview_object_name_.clear();
                overlay_preview_kind_ = SceneObjectAttributeKind::None;
                overlay_preview_write_position_ = false;
                overlay_preview_write_size_ = false;
            }
        }
        else
        {
            overlay_preview_active_ = false;
            overlay_preview_object_name_.clear();
            overlay_preview_kind_ = SceneObjectAttributeKind::None;
            overlay_preview_write_position_ = false;
            overlay_preview_write_size_ = false;
        }
    }

    const SceneMetadata& active_scene_metadata = scene_metadata_for_render;
    const bool has_selected_scene_object = state.selected_item_path == state.active_scene_path && !state.selected_scene_object_name.empty();
    const SceneObjectMetadata* selected_scene_object_metadata = nullptr;
    const SceneResolvedObjectPoseMap resolved_object_poses = ResolveSceneObjectPoses(active_scene_metadata);
    if (has_selected_scene_object)
    {
        const auto selected_object_it = std::find_if(active_scene_metadata.objects.begin(), active_scene_metadata.objects.end(), [&](const SceneObjectMetadata& object)
        {
            return object.name == state.selected_scene_object_name;
        });
        if (selected_object_it != active_scene_metadata.objects.end())
        {
            selected_scene_object_metadata = &(*selected_object_it);
        }
    }
    else if (active_scene_metadata.objects.size() == 1)
    {
        selected_scene_object_metadata = &active_scene_metadata.objects.front();
    }

    queued_objects_.clear();
    for (const SceneObjectMetadata& object : active_scene_metadata.objects)
    {
        QueuedSceneObject queued_object;
        queued_object.name = object.name;
        queued_object.model_visual_offset = object.model_visual_offset;
        queued_object.local_position = object.position;
        queued_object.local_rotation = object.rotation;
        queued_object.local_scale = object.scale;
        queued_object.selected = state.selected_item_path == state.active_scene_path && state.selected_scene_object_name == object.name;
        
        const auto pose_it = resolved_object_poses.find(object.name);
        if (pose_it != resolved_object_poses.end())
        {
            queued_object.model_matrix = pose_it->second.world_matrix;
            queued_object.parent_matrix = pose_it->second.parent_matrix;
            queued_object.has_parent_transform = pose_it->second.has_parent;
            queued_object.world_position = ToSceneVector3(TransformPoint(pose_it->second.world_matrix.data(), Vec3{0.0f, 0.0f, 0.0f}));
        }
        else
        {
            BuildTransformMatrix(queued_object.local_position, queued_object.local_rotation, queued_object.local_scale, queued_object.model_matrix.data());
            SetIdentity(queued_object.parent_matrix.data());
            queued_object.world_position = queued_object.local_position;
        }

        Vec3 object_bounds_min = ToVec3(queued_object.world_position);
        Vec3 object_bounds_max = ToVec3(queued_object.world_position);

        if (!object.model_path.empty())
        {
            const std::filesystem::path model_path = state.project_root / object.model_path;
            const SceneViewportResolvedModel resolved_model = resolve_model_asset(model_path);
            if (resolved_model.asset != nullptr && resolved_model.asset->loaded && 
                EnsureMeshCacheEntry(model_path, resolved_model))
            {
                queued_object.model_path = model_path;
                queued_object.has_bounds = ComputeObjectBounds(queued_object, *resolved_model.asset, object_bounds_min, object_bounds_max);
                ExpandBoundsWithObject(world_min, world_max, queued_object, *resolved_model.asset);
            }
        }
        else
        {
            const float pickup_radius = 0.5f;
            object_bounds_min = Subtract(object_bounds_min, Vec3{pickup_radius, pickup_radius, pickup_radius});
            object_bounds_max = Add(object_bounds_max, Vec3{pickup_radius, pickup_radius, pickup_radius});
            queued_object.has_bounds = true;
        }

        queued_object.bounds_min = ToSceneVector3(object_bounds_min);
        queued_object.bounds_max = ToSceneVector3(object_bounds_max);
        queued_objects_.push_back(queued_object);
    }

    QueuedSceneObject fallback_gizmo_object;
    bool has_fallback_gizmo_object = false;
    if (selected_scene_object_metadata != nullptr)
    {
        fallback_gizmo_object.name = selected_scene_object_metadata->name;
        fallback_gizmo_object.model_visual_offset = selected_scene_object_metadata->model_visual_offset;
        fallback_gizmo_object.local_position = selected_scene_object_metadata->position;
        fallback_gizmo_object.local_rotation = selected_scene_object_metadata->rotation;
        fallback_gizmo_object.local_scale = selected_scene_object_metadata->scale;
        fallback_gizmo_object.selected = selected_scene_object_metadata->name == state.selected_scene_object_name;
        const auto pose_it = resolved_object_poses.find(selected_scene_object_metadata->name);
        if (pose_it != resolved_object_poses.end())
        {
            fallback_gizmo_object.model_matrix = pose_it->second.world_matrix;
            fallback_gizmo_object.parent_matrix = pose_it->second.parent_matrix;
            fallback_gizmo_object.has_parent_transform = pose_it->second.has_parent;
            fallback_gizmo_object.world_position = ToSceneVector3(TransformPoint(pose_it->second.world_matrix.data(), Vec3{0.0f, 0.0f, 0.0f}));
        }
        else
        {
            BuildTransformMatrix(fallback_gizmo_object.local_position, fallback_gizmo_object.local_rotation, fallback_gizmo_object.local_scale, fallback_gizmo_object.model_matrix.data());
            SetIdentity(fallback_gizmo_object.parent_matrix.data());
            fallback_gizmo_object.world_position = fallback_gizmo_object.local_position;
        }
        has_fallback_gizmo_object = true;
    }

    if (queued_objects_.empty() && !has_fallback_gizmo_object)
    {
        SyncRayTracingScene();
        const char* message = "No attached models found in the active scene";
        const ImVec2 message_size = ImGui::CalcTextSize(message);
        draw_list->AddText(ImVec2((min.x + max.x - message_size.x) * 0.5f, (min.y + max.y - message_size.y) * 0.5f), IM_COL32(235, 238, 242, 255), message);
        ImGui::EndChild();
        return;
    }

    Vec3 scene_center = {};
    float scene_radius = 1.5f;
    const Vec3 base_scene_center = !queued_objects_.empty()
        ? Multiply(Add(world_min, world_max), 0.5f)
        : ToVec3(fallback_gizmo_object.world_position);
    if (!queued_objects_.empty())
    {
        scene_center = base_scene_center;
        scene_radius = (std::max)(0.75f, Length(Subtract(world_max, world_min)) * 0.6f);
    }
    else
    {
        scene_center = base_scene_center;
    }

    const QueuedSceneObject* focused_object = nullptr;
    if (has_selected_scene_object)
    {
        const auto selected_object_it = std::find_if(queued_objects_.begin(), queued_objects_.end(), [&](const QueuedSceneObject& object)
        {
            return object.selected;
        });
        if (selected_object_it != queued_objects_.end())
        {
            focused_object = &(*selected_object_it);
        }
    }
    else if (queued_objects_.size() == 1)
    {
        focused_object = &queued_objects_.front();
    }

    if (mouse_over_viewport && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false) && focused_object != nullptr)
    {
        Vec3 focus_center = ToVec3(focused_object->world_position);
        float focus_radius = 0.75f;
        if (focused_object->has_bounds)
        {
            const Vec3 bounds_center = Multiply(Add(ToVec3(focused_object->bounds_min), ToVec3(focused_object->bounds_max)), 0.5f);
            const Vec3 bounds_extent = Multiply(Subtract(ToVec3(focused_object->bounds_max), ToVec3(focused_object->bounds_min)), 0.5f);
            focus_center = bounds_center;
            focus_radius = (std::max)(0.75f, Length(bounds_extent));
        }

        camera_state.pan_offset = ToSceneVector3(Subtract(focus_center, base_scene_center));
        const float scene_base_distance = ComputeOrbitBaseDistance(scene_radius);
        const float focus_distance = ComputeOrbitBaseDistance(focus_radius);
        camera_state.zoom = (std::max)(scene_base_distance / focus_distance, 0.001f);
    }

    scene_center = Add(scene_center, ToVec3(camera_state.pan_offset));
    const std::string selected_object_name = state.HasSelectedSceneObject()
        ? state.selected_scene_object_name
        : std::string{};
    resolved_lighting_ = ResolveSceneLighting(scene_metadata, BuildLightingPoseMap(resolved_object_poses), selected_object_name);
    const Vec3 orbit_direction = Normalize(Vec3{
        std::cos(camera_state.pitch) * std::sin(camera_state.yaw),
        std::sin(camera_state.pitch),
        std::cos(camera_state.pitch) * std::cos(camera_state.yaw)});
    const float distance = ComputeOrbitBaseDistance(scene_radius) / (std::max)(camera_state.zoom, 0.001f);
    if (middle_mouse_panning_ && io.MouseDown[ImGuiMouseButton_Middle])
    {
        const ImVec2 delta = io.MouseDelta;
        Vec3 right = Normalize(Cross(orbit_direction, Vec3{0.0f, 1.0f, 0.0f}));
        if (Length(right) <= 0.0001f)
        {
            right = Vec3{1.0f, 0.0f, 0.0f};
        }
        const Vec3 up = Normalize(Cross(right, orbit_direction));
        const float vertical_world_per_pixel = (2.0f * distance * std::tan(DegreesToRadians(55.0f) * 0.5f)) /
            (std::max)(viewport_height, 1.0f);
        const float horizontal_world_per_pixel = vertical_world_per_pixel * (viewport_width / (std::max)(viewport_height, 1.0f));
        const Vec3 pan_delta = Add(
            Multiply(right, -delta.x * horizontal_world_per_pixel),
            Multiply(up, delta.y * vertical_world_per_pixel));
        camera_state.pan_offset = ToSceneVector3(Add(ToVec3(camera_state.pan_offset), pan_delta));
        scene_center = Add(scene_center, pan_delta);
    }
    const Vec3 camera_position = Add(scene_center, Multiply(orbit_direction, distance));

    SyncRayTracingScene();

    float view_matrix[16];
    float projection_matrix[16];
    BuildLookAtMatrix(camera_position, scene_center, Vec3{0.0f, 1.0f, 0.0f}, view_matrix);
    BuildPerspectiveMatrix(55.0f, static_cast<float>(target_width) / static_cast<float>(target_height), 0.01f, 250.0f, projection_matrix);
    InvertMatrix(view_matrix, view_inverse_.data());
    InvertMatrix(projection_matrix, projection_inverse_.data());
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
    grid_extent_ = (std::max)((std::max)(state.grid_extent, grid_spacing_ * 24.0f), scene_radius * 4.0f);

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
        constexpr float kGizmoTransformEpsilon = 0.0005f;

        float gizmo_matrix[16];
        float delta_matrix[16];
        std::memcpy(gizmo_matrix, gizmo_object->model_matrix.data(), sizeof(gizmo_matrix));
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

        const bool write_position = operation == ImGuizmo::TRANSLATE;
        const bool write_rotation = operation == ImGuizmo::ROTATE;
        const bool write_scale = operation == ImGuizmo::SCALEU;
        const bool is_manipulating = ImGuizmo::Manipulate(view_matrix, projection_matrix, operation, mode, gizmo_matrix, delta_matrix, snap);
        if (is_manipulating)
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

            gizmo_object->world_position = new_position;

            SceneVector3 new_local_position = {};
            SceneVector3 new_local_rotation = {};
            SceneVector3 new_local_scale = {};
            ResolveLocalSceneObjectTransform(
                *gizmo_object,
                new_position,
                new_rotation,
                new_scale,
                new_local_position,
                new_local_rotation,
                new_local_scale);

            const SceneVector3 baseline_position =
                (gizmo_preview_active_ && gizmo_preview_object_name_ == gizmo_object->name) ? gizmo_preview_local_position_ : gizmo_object->local_position;
            const SceneVector3 baseline_rotation =
                (gizmo_preview_active_ && gizmo_preview_object_name_ == gizmo_object->name) ? gizmo_preview_local_rotation_ : gizmo_object->local_rotation;
            const SceneVector3 baseline_scale =
                (gizmo_preview_active_ && gizmo_preview_object_name_ == gizmo_object->name) ? gizmo_preview_local_scale_ : gizmo_object->local_scale;

            const bool position_changed = write_position && HasSignificantSceneVectorDelta(new_local_position, baseline_position, kGizmoTransformEpsilon);
            const bool rotation_changed = write_rotation && HasSignificantSceneVectorDelta(new_local_rotation, baseline_rotation, kGizmoTransformEpsilon);
            const bool scale_changed = write_scale && HasSignificantSceneVectorDelta(new_local_scale, baseline_scale, kGizmoTransformEpsilon);
            if (position_changed || rotation_changed || scale_changed)
            {
                gizmo_preview_active_ = true;
                gizmo_preview_object_name_ = gizmo_object->name;
                gizmo_preview_local_position_ = new_local_position;
                gizmo_preview_local_rotation_ = new_local_rotation;
                gizmo_preview_local_scale_ = new_local_scale;
                gizmo_preview_write_position_ = write_position;
                gizmo_preview_write_rotation_ = write_rotation;
                gizmo_preview_write_scale_ = write_scale;
            }
        }

        if (gizmo_preview_active_ && !ImGuizmo::IsUsing())
        {
            const bool committed = SetSceneObjectTransform(
                state.active_scene_path,
                gizmo_preview_object_name_,
                gizmo_preview_local_position_,
                gizmo_preview_local_rotation_,
                gizmo_preview_local_scale_,
                gizmo_preview_write_position_,
                gizmo_preview_write_rotation_,
                gizmo_preview_write_scale_);
            if (committed)
            {
                state.request_files_tree_refresh = true;
            }

            gizmo_preview_active_ = false;
            gizmo_preview_object_name_.clear();
            gizmo_preview_write_position_ = false;
            gizmo_preview_write_rotation_ = false;
            gizmo_preview_write_scale_ = false;
        }
    }

    if (selected_scene_object_metadata != nullptr)
    {
        const auto selected_pose_it = resolved_object_poses.find(selected_scene_object_metadata->name);
        for (const SceneObjectAttribute& attribute : selected_scene_object_metadata->attributes)
        {
            if (attribute.kind != SceneObjectAttributeKind::SpotLight)
            {
                continue;
            }

            if (selected_pose_it == resolved_object_poses.end())
            {
                continue;
            }

            DrawSpotLightConeGizmo(
                draw_list,
                min,
                max,
                view_projection_.data(),
                selected_pose_it->second,
                attribute.spot_light);
        }
    }

    bool collider_resize_interaction_consumed = false;
    if (show_physics_colliders_)
    {
        DrawPhysicsColliderGizmos(
            draw_list,
            scene_metadata,
            resolved_object_poses,
            view_projection_.data(),
            min,
            max,
            state.selected_scene_object_name);

        if (selected_scene_object_metadata != nullptr)
        {
            const auto selected_pose_it = resolved_object_poses.find(selected_scene_object_metadata->name);
            std::size_t rigidbody_attribute_index = 0;
            if (selected_pose_it != resolved_object_poses.end() &&
                FindRigidbodyAttributeIndex(*selected_scene_object_metadata, rigidbody_attribute_index))
            {
                collider_resize_interaction_consumed = DrawAndHandleSelectedColliderResize(
                    state,
                    *selected_scene_object_metadata,
                    rigidbody_attribute_index,
                    selected_pose_it->second,
                    view_projection_.data(),
                    min,
                    max,
                    mouse_over_viewport,
                    transform_toolbar_hovered || ImGuizmo::IsOver() || ImGuizmo::IsUsing());
            }
        }
    }

    const AxisViewFlipResult axis_view_result = DrawAxisViewFlipControl(min, max, camera_state);
    if (axis_view_result.changed)
    {
        const Vec3 flipped_orbit_direction = GetOrbitCameraDirection(camera_state);
        const Vec3 flipped_camera_position = Add(scene_center, Multiply(flipped_orbit_direction, distance));
        BuildLookAtMatrix(flipped_camera_position, scene_center, Vec3{0.0f, 1.0f, 0.0f}, view_matrix);
        InvertMatrix(view_matrix, view_inverse_.data());
        MultiplyMatrix(projection_matrix, view_matrix, view_projection_.data());
    }

    std::string overlay_hit_object_name;
    // Calculate scaling factors for overlay rendering.
    // Overlays are stored in reference viewport coordinates and need to scale based on:
    // 1. Reference resolution to normalized (0-1)
    // 2. Normalized to current viewport pixels
    const float reference_width = static_cast<float>((std::max)(1u, active_scene_metadata.reference_viewport_width));
    const float reference_height = static_cast<float>((std::max)(1u, active_scene_metadata.reference_viewport_height));
    const float overlay_scale_x = viewport_width / reference_width;
    const float overlay_scale_y = viewport_height / reference_height;
    const float overlay_size_scale = (std::min)(overlay_scale_x, overlay_scale_y);
    const ImVec2 overlay_mouse_pos = ImGui::GetMousePos();
    for (auto object_it = active_scene_metadata.objects.rbegin(); object_it != active_scene_metadata.objects.rend(); ++object_it)
    {
        const SceneObjectMetadata& object = *object_it;
        bool hit = false;
        for (const SceneObjectAttribute& attribute : object.attributes)
        {
            if (attribute.kind != SceneObjectAttributeKind::Text2D &&
                attribute.kind != SceneObjectAttributeKind::Image2D &&
                attribute.kind != SceneObjectAttributeKind::Color2D)
            {
                continue;
            }

            const bool is_text_overlay = attribute.kind == SceneObjectAttributeKind::Text2D;
            const bool is_color_overlay = attribute.kind == SceneObjectAttributeKind::Color2D;
            const float attr_x = is_text_overlay ? attribute.text_2d.x : (is_color_overlay ? attribute.color_2d.x : attribute.image_2d.x);
            const float attr_y = is_text_overlay ? attribute.text_2d.y : (is_color_overlay ? attribute.color_2d.y : attribute.image_2d.y);
            float attr_w = is_text_overlay ? attribute.text_2d.width : (is_color_overlay ? attribute.color_2d.width : attribute.image_2d.width);
            float attr_h = is_text_overlay ? attribute.text_2d.height : (is_color_overlay ? attribute.color_2d.height : attribute.image_2d.height);
            if (is_text_overlay)
            {
                scene_2d_renderer_.GetText2DRenderSize(state.project_root, attribute.text_2d, attr_w, attr_h);
            }

            const float reference_range_x = (std::max)(reference_width - attr_w, 1.0f);
            const float reference_range_y = (std::max)(reference_height - attr_h, 1.0f);
            const float screen_w = attr_w * overlay_size_scale;
            const float screen_h = attr_h * overlay_size_scale;
            const float screen_range_x = (std::max)(viewport_width - screen_w, 0.0f);
            const float screen_range_y = (std::max)(viewport_height - screen_h, 0.0f);
            const ImVec2 rect_min(
                min.x + (attr_x / reference_range_x) * screen_range_x,
                min.y + (attr_y / reference_range_y) * screen_range_y);
            const ImVec2 rect_max(rect_min.x + screen_w, rect_min.y + screen_h);
            if (overlay_mouse_pos.x >= rect_min.x && overlay_mouse_pos.x <= rect_max.x &&
                overlay_mouse_pos.y >= rect_min.y && overlay_mouse_pos.y <= rect_max.y)
            {
                overlay_hit_object_name = object.name;
                hit = true;
                break;
            }
        }

        if (hit)
        {
            break;
        }
    }

    bool overlay_gizmo_interaction_consumed = false;
    if (selected_scene_object_metadata != nullptr)
    {
        const SceneObjectAttribute* selected_overlay_attribute = nullptr;
        std::size_t selected_overlay_attribute_index = 0;
        for (std::size_t attribute_index = 0; attribute_index < selected_scene_object_metadata->attributes.size(); ++attribute_index)
        {
            const SceneObjectAttribute& attribute = selected_scene_object_metadata->attributes[attribute_index];
            if (attribute.kind == SceneObjectAttributeKind::Text2D ||
                attribute.kind == SceneObjectAttributeKind::Image2D ||
                attribute.kind == SceneObjectAttributeKind::Color2D)
            {
                selected_overlay_attribute = &attribute;
                selected_overlay_attribute_index = attribute_index;
                break;
            }
        }

        if (selected_overlay_attribute != nullptr)
        {
            static std::string active_overlay_object_name;
            static std::size_t active_overlay_attribute_index = 0;
            static bool overlay_dragging = false;
            static bool overlay_resizing = false;
            static float overlay_x = 0.0f;
            static float overlay_y = 0.0f;
            static float overlay_w = 1.0f;
            static float overlay_h = 1.0f;

            const bool is_text_overlay = selected_overlay_attribute->kind == SceneObjectAttributeKind::Text2D;
            const bool is_color_overlay = selected_overlay_attribute->kind == SceneObjectAttributeKind::Color2D;
            const float attr_x = is_text_overlay ? selected_overlay_attribute->text_2d.x : (is_color_overlay ? selected_overlay_attribute->color_2d.x : selected_overlay_attribute->image_2d.x);
            const float attr_y = is_text_overlay ? selected_overlay_attribute->text_2d.y : (is_color_overlay ? selected_overlay_attribute->color_2d.y : selected_overlay_attribute->image_2d.y);
            float attr_w = is_text_overlay ? selected_overlay_attribute->text_2d.width : (is_color_overlay ? selected_overlay_attribute->color_2d.width : selected_overlay_attribute->image_2d.width);
            float attr_h = is_text_overlay ? selected_overlay_attribute->text_2d.height : (is_color_overlay ? selected_overlay_attribute->color_2d.height : selected_overlay_attribute->image_2d.height);
            if (is_text_overlay)
            {
                scene_2d_renderer_.GetText2DRenderSize(state.project_root, selected_overlay_attribute->text_2d, attr_w, attr_h);
            }
            const bool lock_aspect_ratio = is_text_overlay
                ? selected_overlay_attribute->text_2d.lock_aspect_ratio
                : (is_color_overlay
                    ? selected_overlay_attribute->color_2d.lock_aspect_ratio
                    : selected_overlay_attribute->image_2d.lock_aspect_ratio);

            const bool same_overlay_target =
                active_overlay_object_name == selected_scene_object_metadata->name &&
                active_overlay_attribute_index == selected_overlay_attribute_index;
            if (!overlay_dragging && !overlay_resizing)
            {
                active_overlay_object_name = selected_scene_object_metadata->name;
                active_overlay_attribute_index = selected_overlay_attribute_index;
                overlay_x = attr_x;
                overlay_y = attr_y;
                overlay_w = attr_w;
                overlay_h = attr_h;
            }
            else if (!same_overlay_target)
            {
                overlay_dragging = false;
                overlay_resizing = false;
                active_overlay_object_name = selected_scene_object_metadata->name;
                active_overlay_attribute_index = selected_overlay_attribute_index;
                overlay_x = attr_x;
                overlay_y = attr_y;
                overlay_w = attr_w;
                overlay_h = attr_h;
            }

            const float reference_range_x = (std::max)(reference_width - overlay_w, 1.0f);
            const float reference_range_y = (std::max)(reference_height - overlay_h, 1.0f);
            const float screen_w = overlay_w * overlay_size_scale;
            const float screen_h = overlay_h * overlay_size_scale;
            const float screen_range_x = (std::max)(viewport_width - screen_w, 0.0f);
            const float screen_range_y = (std::max)(viewport_height - screen_h, 0.0f);
            const ImVec2 rect_min(
                min.x + (overlay_x / reference_range_x) * screen_range_x,
                min.y + (overlay_y / reference_range_y) * screen_range_y);
            const ImVec2 rect_max(rect_min.x + screen_w, rect_min.y + screen_h);
            const ImVec2 resize_handle_min(rect_max.x - 8.0f, rect_max.y - 8.0f);

            draw_list->AddRect(rect_min, rect_max, IM_COL32(250, 196, 52, 255), 0.0f, 0, 2.0f);
            draw_list->AddRectFilled(resize_handle_min, rect_max, IM_COL32(250, 196, 52, 220));

            const ImVec2 mouse_pos = ImGui::GetMousePos();
            const bool mouse_in_rect =
                mouse_pos.x >= rect_min.x && mouse_pos.x <= rect_max.x &&
                mouse_pos.y >= rect_min.y && mouse_pos.y <= rect_max.y;
            const bool mouse_in_resize =
                mouse_pos.x >= resize_handle_min.x && mouse_pos.x <= rect_max.x &&
                mouse_pos.y >= resize_handle_min.y && mouse_pos.y <= rect_max.y;

            const bool allow_overlay_interaction =
                mouse_over_viewport &&
                !transform_toolbar_hovered &&
                !axis_view_result.hovered &&
                !collider_resize_interaction_consumed &&
                !ImGuizmo::IsOver() &&
                !ImGuizmo::IsUsing();

            if (allow_overlay_interaction && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                if (mouse_in_resize)
                {
                    overlay_resizing = true;
                    overlay_dragging = false;
                    overlay_gizmo_interaction_consumed = true;
                }
                else if (mouse_in_rect)
                {
                    overlay_dragging = true;
                    overlay_resizing = false;
                    overlay_gizmo_interaction_consumed = true;
                }
            }

            const bool left_mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (!left_mouse_down && (overlay_dragging || overlay_resizing))
            {
                bool committed = false;
                if (is_text_overlay)
                {
                    if (overlay_dragging)
                    {
                        committed = SetSceneObjectAttributeText2DPosition(
                            state.active_scene_path,
                            selected_scene_object_metadata->name,
                            selected_overlay_attribute_index,
                            overlay_x,
                            overlay_y);
                    }
                    else if (overlay_resizing)
                    {
                        committed = SetSceneObjectAttributeText2DSize(
                            state.active_scene_path,
                            selected_scene_object_metadata->name,
                            selected_overlay_attribute_index,
                            overlay_w,
                            overlay_h);
                    }
                }
                else if (is_color_overlay)
                {
                    if (overlay_dragging)
                    {
                        committed = SetSceneObjectAttributeColor2DPosition(
                            state.active_scene_path,
                            selected_scene_object_metadata->name,
                            selected_overlay_attribute_index,
                            overlay_x,
                            overlay_y);
                    }
                    else if (overlay_resizing)
                    {
                        committed = SetSceneObjectAttributeColor2DSize(
                            state.active_scene_path,
                            selected_scene_object_metadata->name,
                            selected_overlay_attribute_index,
                            overlay_w,
                            overlay_h);
                    }
                }
                else
                {
                    if (overlay_dragging)
                    {
                        committed = SetSceneObjectAttributeImage2DPosition(
                            state.active_scene_path,
                            selected_scene_object_metadata->name,
                            selected_overlay_attribute_index,
                            overlay_x,
                            overlay_y);
                    }
                    else if (overlay_resizing)
                    {
                        committed = SetSceneObjectAttributeImage2DSize(
                            state.active_scene_path,
                            selected_scene_object_metadata->name,
                            selected_overlay_attribute_index,
                            overlay_w,
                            overlay_h);
                    }
                }

                if (committed)
                {
                    state.request_files_tree_refresh = true;
                }

                overlay_dragging = false;
                overlay_resizing = false;
                overlay_preview_active_ = false;
                overlay_preview_object_name_.clear();
                overlay_preview_kind_ = SceneObjectAttributeKind::None;
                overlay_preview_write_position_ = false;
                overlay_preview_write_size_ = false;
                overlay_gizmo_interaction_consumed = true;
            }
            else if (!left_mouse_down)
            {
                overlay_preview_active_ = false;
                overlay_preview_object_name_.clear();
                overlay_preview_kind_ = SceneObjectAttributeKind::None;
                overlay_preview_write_position_ = false;
                overlay_preview_write_size_ = false;
            }

            if ((overlay_dragging || overlay_resizing) && allow_overlay_interaction)
            {
                const float position_scale_x = screen_range_x / reference_range_x;
                const float position_scale_y = screen_range_y / reference_range_y;
                const float safe_position_scale_x = (std::max)(position_scale_x, 0.0001f);
                const float safe_position_scale_y = (std::max)(position_scale_y, 0.0001f);
                const float delta_x = io.MouseDelta.x / safe_position_scale_x;
                const float delta_y = io.MouseDelta.y / safe_position_scale_y;
                const float size_delta_x = io.MouseDelta.x / overlay_size_scale;
                const float size_delta_y = io.MouseDelta.y / overlay_size_scale;
                if (overlay_dragging)
                {
                    overlay_x += delta_x;
                    overlay_y += delta_y;
                }
                else if (overlay_resizing)
                {
                    if (lock_aspect_ratio)
                    {
                        const float aspect = (std::max)(overlay_h, 1.0f) > 0.0f
                            ? (overlay_w / (std::max)(overlay_h, 1.0f))
                            : 1.0f;
                        const float diagonal_delta = std::abs(size_delta_x) >= std::abs(size_delta_y) ? size_delta_x : size_delta_y;
                        const float next_w = (std::max)(1.0f, overlay_w + diagonal_delta);
                        const float safe_aspect = (std::max)(aspect, 0.0001f);
                        overlay_w = next_w;
                        overlay_h = (std::max)(1.0f, next_w / safe_aspect);
                    }
                    else
                    {
                        overlay_w = (std::max)(1.0f, overlay_w + size_delta_x);
                        overlay_h = (std::max)(1.0f, overlay_h + size_delta_y);
                    }
                }

                overlay_preview_active_ = true;
                overlay_preview_object_name_ = selected_scene_object_metadata->name;
                overlay_preview_attribute_index_ = selected_overlay_attribute_index;
                overlay_preview_kind_ = selected_overlay_attribute->kind;
                overlay_preview_x_ = overlay_x;
                overlay_preview_y_ = overlay_y;
                overlay_preview_w_ = overlay_w;
                overlay_preview_h_ = overlay_h;
                overlay_preview_write_position_ = overlay_dragging;
                overlay_preview_write_size_ = overlay_resizing;
                overlay_gizmo_interaction_consumed = true;
            }
        }
    }

    if (mouse_over_viewport &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !overlay_gizmo_interaction_consumed &&
        !collider_resize_interaction_consumed &&
        !transform_toolbar_hovered &&
        !axis_view_result.hovered &&
        !ImGuizmo::IsOver() &&
        !ImGuizmo::IsUsing())
    {
        if (!overlay_hit_object_name.empty())
        {
            state.SetSelectedSceneObject(state.active_scene_path, overlay_hit_object_name);
            overlay_gizmo_interaction_consumed = true;
        }

        if (overlay_gizmo_interaction_consumed)
        {
            pending_scene_metadata_ = active_scene_metadata;
            pending_project_root_ = state.project_root;
            render_requested_ = true;

            if (show_fps_)
            {
                const int viewport_fps = static_cast<int>(std::round(ImGui::GetIO().Framerate));
                const std::string footer = std::to_string(viewport_fps) + " FPS";
                draw_list->AddText(
                    ImGui::GetFont(),
                    ImGui::GetFontSize() + 3.0f,
                    ImVec2(min.x + 12.0f, max.y - 24.0f),
                    IM_COL32(145, 152, 163, 255),
                    footer.c_str());
            }
            ImGui::EndChild();
            return;
        }

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

    pending_scene_metadata_ = active_scene_metadata;
    pending_project_root_ = state.project_root;
    render_requested_ = true;

    if (show_fps_)
    {
        const int viewport_fps = static_cast<int>(std::round(ImGui::GetIO().Framerate));
        const std::string footer = std::to_string(viewport_fps) + " FPS";
        draw_list->AddText(
            ImGui::GetFont(),
            ImGui::GetFontSize() + 3.0f,
            ImVec2(min.x + 12.0f, max.y - 24.0f),
            IM_COL32(145, 152, 163, 255),
            footer.c_str());
    }
    ImGui::EndChild();
}

void SceneViewportRenderer::RenderCameraPreview(
    const EngineState& state,
    const SceneMetadata& scene_metadata,
    const SceneViewportModelResolver& resolve_model_asset,
    const SceneObjectMetadata& camera_object,
    const SceneObjectCameraAttributes& camera_attributes)
{
    ImGui::Spacing();
    ImGui::TextUnformatted("Preview");

    const float available_width = ImGui::GetContentRegionAvail().x;
    if (available_width <= 4.0f)
    {
        ImGui::TextDisabled("Preview unavailable in this layout.");
        return;
    }

    const float preview_height = (std::min)(available_width * (9.0f / 16.0f), 220.0f);
    if (preview_height <= 4.0f)
    {
        return;
    }

    ImGui::BeginChild("##SceneCameraPreviewCanvas", ImVec2(0.0f, preview_height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = ImVec2(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(min, max, IM_COL32(24, 28, 32, 255), 8.0f);

    auto draw_message = [&](const char* message)
    {
        const ImVec2 message_size = ImGui::CalcTextSize(message);
        draw_list->AddText(
            ImVec2((min.x + max.x - message_size.x) * 0.5f, (min.y + max.y - message_size.y) * 0.5f),
            IM_COL32(235, 238, 242, 255),
            message);
    };

    if (!scene_metadata.parsed)
    {
        draw_message("Scene preview unavailable");
        ImGui::EndChild();
        return;
    }

    const ImVec2 framebuffer_scale = ImGui::GetIO().DisplayFramebufferScale;
    const std::uint32_t target_width = static_cast<std::uint32_t>((std::max)(1.0f, std::round(ImGui::GetWindowSize().x * framebuffer_scale.x)));
    const std::uint32_t target_height = static_cast<std::uint32_t>((std::max)(1.0f, std::round(ImGui::GetWindowSize().y * framebuffer_scale.y)));

    if (vulkan_context_ == nullptr || !ray_tracing_.EnsureViewportOutput(target_width, target_height))
    {
        draw_list->AddRect(min, max, IM_COL32(92, 99, 110, 255), 8.0f, 0, 1.5f);
        draw_message("Scene GPU renderer unavailable");
        ImGui::EndChild();
        return;
    }

    if (ray_tracing_.GetOutputDescriptorSet() != VK_NULL_HANDLE)
    {
        draw_list->AddImage(
            reinterpret_cast<ImTextureID>(ray_tracing_.GetOutputDescriptorSet()),
            min,
            max,
            ImVec2(0.0f, 1.0f),
            ImVec2(1.0f, 0.0f));
    }

    draw_list->AddRect(min, max, IM_COL32(92, 99, 110, 255), 8.0f, 0, 1.5f);

    const SceneResolvedObjectPoseMap resolved_object_poses = ResolveSceneObjectPoses(scene_metadata);
    const auto camera_pose_it = resolved_object_poses.find(camera_object.name);
    if (camera_pose_it == resolved_object_poses.end())
    {
        draw_message("Camera transform unavailable");
        ImGui::EndChild();
        return;
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
        queued_object.model_visual_offset = object.model_visual_offset;
        queued_object.local_position = object.position;
        queued_object.local_rotation = object.rotation;
        queued_object.local_scale = object.scale;

        const auto pose_it = resolved_object_poses.find(object.name);
        if (pose_it != resolved_object_poses.end())
        {
            queued_object.model_matrix = pose_it->second.world_matrix;
            queued_object.parent_matrix = pose_it->second.parent_matrix;
            queued_object.has_parent_transform = pose_it->second.has_parent;
            queued_object.world_position = ToSceneVector3(TransformPoint(pose_it->second.world_matrix.data(), Vec3{0.0f, 0.0f, 0.0f}));
        }
        else
        {
            BuildTransformMatrix(queued_object.local_position, queued_object.local_rotation, queued_object.local_scale, queued_object.model_matrix.data());
            SetIdentity(queued_object.parent_matrix.data());
            queued_object.world_position = queued_object.local_position;
        }

        queued_objects_.push_back(queued_object);
    }

    if (queued_objects_.empty())
    {
        SyncRayTracingScene();
        draw_message("No renderable models found in scene");
        ImGui::EndChild();
        return;
    }

    resolved_lighting_ = ResolveSceneLighting(scene_metadata, BuildLightingPoseMap(resolved_object_poses), camera_object.name);
    grid_enabled_ = false;

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

    SyncRayTracingScene();

    float view_matrix[16];
    float projection_matrix[16];
    BuildLookAtMatrix(camera_position, Add(camera_position, camera_forward), camera_up, view_matrix);

    const float aspect = static_cast<float>(target_width) / static_cast<float>(target_height);
    const float field_of_view = std::clamp(camera_attributes.field_of_view_degrees, 1.0f, 179.0f);
    const float near_clip = (std::max)(camera_attributes.near_clip, 0.001f);
    const float far_clip = (std::max)(camera_attributes.far_clip, near_clip + 0.001f);
    BuildPerspectiveMatrix(field_of_view, aspect, near_clip, far_clip, projection_matrix);
    InvertMatrix(view_matrix, view_inverse_.data());
    InvertMatrix(projection_matrix, projection_inverse_.data());
    MultiplyMatrix(projection_matrix, view_matrix, view_projection_.data());

    render_requested_ = true;

    draw_list->AddText(ImVec2(min.x + 12.0f, max.y - 24.0f), IM_COL32(145, 152, 163, 255), "Camera Preview");
    ImGui::EndChild();
}

void SceneViewportRenderer::RenderGpu()
{
    if (!render_requested_ || vulkan_context_ == nullptr)
    {
        return;
    }

    ray_tracing_.SetSkyboxTexture(
        skybox_renderer_.ResolveSkyboxView(pending_scene_metadata_, pending_project_root_));
    ray_tracing_.SetSkyboxRotation(
        skybox_renderer_.ResolveSkyboxRotationDegrees(pending_scene_metadata_));

    if (!ray_tracing_.RenderFrame(
            resolved_lighting_,
            view_inverse_,
            projection_inverse_,
            grid_enabled_,
            grid_spacing_,
            grid_origin_x_,
            grid_origin_z_,
            grid_extent_))
    {
        SDL_Log("RayTracing::RenderFrame failed: %s", ray_tracing_.GetStatusMessage().c_str());
    }

    // Editor preview: pass dt=0 so the manager only decodes the first frame
    // (and any subsequent frame after a video path/play-mode change). The
    // editor does not advance video playback time.
    if (ray_tracing_.WasFrameSubmittedLastCall())
    {
        video_playback_manager_.Update(0.0f, pending_scene_metadata_, pending_project_root_);
        scene_2d_renderer_.CompositeOverlay(
            pending_scene_metadata_,
            pending_project_root_,
            ray_tracing_.GetOutputImage(),
            ray_tracing_.GetOutputImageView(),
            ray_tracing_.GetOutputWidth(),
            ray_tracing_.GetOutputHeight());
    }
}