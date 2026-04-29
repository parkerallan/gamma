#pragma once

#include "assets/SceneMetadata.h"

#include <array>
#include <string>
#include <unordered_map>

struct SceneLightingResolvedObjectPose
{
    std::array<float, 16> world_matrix = {};
};

using SceneLightingResolvedObjectPoseMap = std::unordered_map<std::string, SceneLightingResolvedObjectPose>;

struct ResolvedSceneLighting
{
    std::array<float, 4> ambient_light = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> directional_light_color = {1.0f, 1.0f, 1.0f, 0.0f};
    std::array<float, 4> directional_light_direction = {0.0f, -1.0f, 0.0f, 1.0f};
    std::array<float, 4> directional_light_data = {0.004712389f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> point_light_color = {1.0f, 1.0f, 1.0f, 0.0f};
    std::array<float, 4> point_light_position = {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4> point_light_data = {0.1f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> spot_light_color = {1.0f, 1.0f, 1.0f, 0.0f};
    std::array<float, 4> spot_light_direction = {0.0f, -1.0f, 0.0f, 1.0f};
    std::array<float, 4> spot_light_position = {0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 4> spot_light_data = {0.0f, 0.1f, 0.0f, 0.0f};
};

ResolvedSceneLighting ResolveSceneLighting(
    const SceneMetadata& scene_metadata,
    const SceneLightingResolvedObjectPoseMap& resolved_poses,
    const std::string& selected_object_name);