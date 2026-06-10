#pragma once

#include "assets/SceneMetadata.h"

#include <array>
#include <functional>
#include <string>

// Resolved camera view-space basis in world coordinates.
struct CameraView
{
    std::array<float, 3> position = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> forward  = {0.0f, 0.0f, -1.0f};
    std::array<float, 3> up       = {0.0f, 1.0f, 0.0f};
};

// Returns the active camera's view basis based on the camera attribute type.
//
// - Fixed:  position/forward/up come directly from the camera's current world matrix.
// - Follow: camera position = target world position + camera.follow_offset, oriented
//           to look at the target world position. Falls back to Fixed if the follow
//           target cannot be resolved.
CameraView ResolveCameraView(
    const SceneObjectCameraAttributes& camera,
    const std::array<float, 16>& camera_world_matrix,
    const std::function<const std::array<float, 16>*(const std::string&)>& target_world_matrix_lookup);
