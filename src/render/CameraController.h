#pragma once

#include "assets/SceneMetadata.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

// Resolved camera view-space basis in world coordinates.
struct CameraView
{
    std::array<float, 3> position = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> forward  = {0.0f, 0.0f, -1.0f};
    std::array<float, 3> up       = {0.0f, 1.0f, 0.0f};
};

// A point on the track spline plus the normalized direction of travel there.
struct TrackSample
{
    std::array<float, 3> position = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> tangent  = {0.0f, 0.0f, -1.0f};
};

// Total arc length of the spline fitted through `points`.
// Returns 0 when fewer than two points are supplied.
float TrackTotalLength(const std::vector<SceneVector3>& points);

// Samples the camera-path spline through `points` at the given arc-length
// `distance` (clamped to [0, total length]). The path is a C2-continuous uniform
// cubic B-spline with clamped (tripled) endpoints: it passes through the first and
// last points and near the interior ones, with no curvature kink at control points.
// Returns position + normalized travel tangent.
TrackSample SampleTrackAtDistance(const std::vector<SceneVector3>& points, float distance);

// Builds a camera view that faces along the track tangent, with an extra Euler
// rotation offset (XYZ, degrees) applied on top of the travel direction.
CameraView ResolveTrackCameraView(const TrackSample& sample, const SceneVector3& rotation_offset_degrees);

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
