#include "render/Lighting.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kPi = 3.1415926535f;
constexpr float kDefaultDirectionalAngularRadiusDegrees = 0.27f;
constexpr float kDefaultSpotLightSourceRadius = 0.1f;

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 Multiply(const Vec3& value, float scalar)
{
    return Vec3{value.x * scalar, value.y * scalar, value.z * scalar};
}

float Dot(const Vec3& left, const Vec3& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
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

void AccumulateEnvironmentLight(ResolvedSceneLighting& lighting, const SceneObjectAttribute& attribute, bool& found_environment_light)
{
    if (!found_environment_light)
    {
        lighting.ambient_light = {0.0f, 0.0f, 0.0f, 0.0f};
        found_environment_light = true;
    }

    lighting.ambient_light[0] += attribute.environment_light.color[0] * attribute.environment_light.intensity;
    lighting.ambient_light[1] += attribute.environment_light.color[1] * attribute.environment_light.intensity;
    lighting.ambient_light[2] += attribute.environment_light.color[2] * attribute.environment_light.intensity;
    lighting.ambient_light[3] = 1.0f;
}

void ApplyDirectionalLight(ResolvedSceneLighting& lighting, const SceneLightingResolvedObjectPose& pose, const SceneObjectAttribute& attribute)
{
    const Vec3 direction = TransformDirectionByMatrix(pose.world_matrix.data(), Vec3{0.0f, 0.0f, -1.0f});
    lighting.directional_light_color = {
        attribute.directional_light.color[0],
        attribute.directional_light.color[1],
        attribute.directional_light.color[2],
        attribute.directional_light.intensity};
    lighting.directional_light_direction = {direction.x, direction.y, direction.z, 1.0f};
    lighting.directional_light_data = {DegreesToRadians(kDefaultDirectionalAngularRadiusDegrees), 0.0f, 0.0f, 0.0f};
}

void ApplySpotLight(ResolvedSceneLighting& lighting, const SceneLightingResolvedObjectPose& pose, const SceneObjectAttribute& attribute)
{
    const Vec3 direction = TransformDirectionByMatrix(pose.world_matrix.data(), Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 position = TransformPoint(pose.world_matrix.data(), Vec3{0.0f, 0.0f, 0.0f});
    lighting.spot_light_color = {
        attribute.spot_light.color[0],
        attribute.spot_light.color[1],
        attribute.spot_light.color[2],
        attribute.spot_light.intensity};
    lighting.spot_light_direction = {
        direction.x,
        direction.y,
        direction.z,
        std::cos(DegreesToRadians(attribute.spot_light.inner_cone_degrees))};
    lighting.spot_light_position = {
        position.x,
        position.y,
        position.z,
        (std::max)(attribute.spot_light.range, 0.001f)};
    lighting.spot_light_data = {
        std::cos(DegreesToRadians(attribute.spot_light.outer_cone_degrees)),
        kDefaultSpotLightSourceRadius,
        0.0f,
        0.0f};
}

void ResolvePreferredDirectLights(
    ResolvedSceneLighting& lighting,
    const SceneMetadata& scene_metadata,
    const SceneLightingResolvedObjectPoseMap& resolved_poses,
    const std::string& selected_object_name,
    bool& found_directional_light,
    bool& found_spot_light)
{
    if (selected_object_name.empty())
    {
        return;
    }

    const auto object_it = std::find_if(scene_metadata.objects.begin(), scene_metadata.objects.end(), [&](const SceneObjectMetadata& object)
    {
        return object.name == selected_object_name;
    });
    if (object_it == scene_metadata.objects.end())
    {
        return;
    }

    const auto pose_it = resolved_poses.find(object_it->name);
    if (pose_it == resolved_poses.end())
    {
        return;
    }

    for (const SceneObjectAttribute& attribute : object_it->attributes)
    {
        if (!found_directional_light && attribute.kind == SceneObjectAttributeKind::DirectionalLight)
        {
            ApplyDirectionalLight(lighting, pose_it->second, attribute);
            found_directional_light = true;
        }
        else if (!found_spot_light && attribute.kind == SceneObjectAttributeKind::SpotLight)
        {
            ApplySpotLight(lighting, pose_it->second, attribute);
            found_spot_light = true;
        }
    }
}
}

ResolvedSceneLighting ResolveSceneLighting(
    const SceneMetadata& scene_metadata,
    const SceneLightingResolvedObjectPoseMap& resolved_poses,
    const std::string& selected_object_name)
{
    ResolvedSceneLighting lighting{};

    bool found_environment_light = false;
    bool found_directional_light = false;
    bool found_spot_light = false;

    ResolvePreferredDirectLights(lighting, scene_metadata, resolved_poses, selected_object_name, found_directional_light, found_spot_light);

    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        const auto pose_it = resolved_poses.find(object.name);
        if (pose_it == resolved_poses.end())
        {
            continue;
        }

        for (const SceneObjectAttribute& attribute : object.attributes)
        {
            switch (attribute.kind)
            {
            case SceneObjectAttributeKind::EnvironmentLight:
                AccumulateEnvironmentLight(lighting, attribute, found_environment_light);
                break;

            case SceneObjectAttributeKind::DirectionalLight:
                if (!found_directional_light)
                {
                    ApplyDirectionalLight(lighting, pose_it->second, attribute);
                    found_directional_light = true;
                }
                break;

            case SceneObjectAttributeKind::SpotLight:
                if (!found_spot_light)
                {
                    ApplySpotLight(lighting, pose_it->second, attribute);
                    found_spot_light = true;
                }
                break;

            case SceneObjectAttributeKind::Camera:
            case SceneObjectAttributeKind::None:
            default:
                break;
            }
        }
    }

    return lighting;
}