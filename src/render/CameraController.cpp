#include "render/CameraController.h"

#include <cmath>

namespace
{
using Vec3Array = std::array<float, 3>;

Vec3Array TransformPointArray(const std::array<float, 16>& matrix, const Vec3Array& point)
{
    return {
        matrix[0] * point[0] + matrix[4] * point[1] + matrix[8]  * point[2] + matrix[12],
        matrix[1] * point[0] + matrix[5] * point[1] + matrix[9]  * point[2] + matrix[13],
        matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14],
    };
}

Vec3Array TransformDirectionArray(const std::array<float, 16>& matrix, const Vec3Array& direction)
{
    return {
        matrix[0] * direction[0] + matrix[4] * direction[1] + matrix[8]  * direction[2],
        matrix[1] * direction[0] + matrix[5] * direction[1] + matrix[9]  * direction[2],
        matrix[2] * direction[0] + matrix[6] * direction[1] + matrix[10] * direction[2],
    };
}

float LengthArray(const Vec3Array& v)
{
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

Vec3Array NormalizeArray(const Vec3Array& v)
{
    const float length = LengthArray(v);
    if (length <= 0.0001f)
    {
        return {0.0f, 0.0f, 0.0f};
    }
    const float inv = 1.0f / length;
    return {v[0] * inv, v[1] * inv, v[2] * inv};
}

float DotArray(const Vec3Array& a, const Vec3Array& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// Rotate a direction by Euler XYZ degrees in the engine's convention
// (matches BuildTransformMatrix: rotation = Rz * Ry * Rx applied to vector).
Vec3Array RotateByEulerDegreesXYZ(const Vec3Array& v, const std::array<float, 3>& euler_degrees)
{
    constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
    const float rx = euler_degrees[0] * kDeg2Rad;
    const float ry = euler_degrees[1] * kDeg2Rad;
    const float rz = euler_degrees[2] * kDeg2Rad;
    const float cx = std::cos(rx), sx = std::sin(rx);
    const float cy = std::cos(ry), sy = std::sin(ry);
    const float cz = std::cos(rz), sz = std::sin(rz);

    // Rx
    Vec3Array a = {
        v[0],
        cx * v[1] - sx * v[2],
        sx * v[1] + cx * v[2],
    };
    // Ry
    Vec3Array b = {
        cy * a[0] + sy * a[2],
        a[1],
        -sy * a[0] + cy * a[2],
    };
    // Rz
    Vec3Array c = {
        cz * b[0] - sz * b[1],
        sz * b[0] + cz * b[1],
        b[2],
    };
    return c;
}

CameraView ResolveFixedCameraView(const std::array<float, 16>& camera_world_matrix)
{
    CameraView view;
    view.position = TransformPointArray(camera_world_matrix, {0.0f, 0.0f, 0.0f});
    Vec3Array forward = TransformDirectionArray(camera_world_matrix, {0.0f, 0.0f, -1.0f});
    Vec3Array up      = TransformDirectionArray(camera_world_matrix, {0.0f, 1.0f, 0.0f});
    if (LengthArray(forward) <= 0.0001f)
    {
        forward = {0.0f, 0.0f, -1.0f};
    }
    if (LengthArray(up) <= 0.0001f || std::abs(DotArray(NormalizeArray(forward), NormalizeArray(up))) >= 0.999f)
    {
        up = {0.0f, 1.0f, 0.0f};
    }
    view.forward = forward;
    view.up      = up;
    return view;
}
}

CameraView ResolveCameraView(
    const SceneObjectCameraAttributes& camera,
    const std::array<float, 16>& camera_world_matrix,
    const std::function<const std::array<float, 16>*(const std::string&)>& target_world_matrix_lookup)
{
    if (camera.type != SceneObjectCameraType::Follow || camera.follow_target_object.empty())
    {
        return ResolveFixedCameraView(camera_world_matrix);
    }

    const std::array<float, 16>* target_matrix =
        target_world_matrix_lookup ? target_world_matrix_lookup(camera.follow_target_object) : nullptr;
    if (target_matrix == nullptr)
    {
        return ResolveFixedCameraView(camera_world_matrix);
    }

    const Vec3Array target_position = TransformPointArray(*target_matrix, {0.0f, 0.0f, 0.0f});

    CameraView view = ResolveFixedCameraView(camera_world_matrix);
    if (camera.follow_lock_position)
    {
        // Camera stays at its authored world position (plus Follow Offset as a
        // static offset) and auto-aims at the target. The camera object's own
        // rotation is overridden by the look-at.
        view.position = {
            view.position[0] + camera.follow_offset[0],
            view.position[1] + camera.follow_offset[1],
            view.position[2] + camera.follow_offset[2],
        };
        Vec3Array forward = NormalizeArray({
            target_position[0] - view.position[0],
            target_position[1] - view.position[1],
            target_position[2] - view.position[2],
        });
        if (LengthArray(forward) <= 0.0001f)
        {
            forward = {0.0f, 0.0f, -1.0f};
        }
        Vec3Array up = {0.0f, 1.0f, 0.0f};
        if (std::abs(DotArray(forward, up)) >= 0.999f)
        {
            up = {0.0f, 0.0f, 1.0f};
        }
        view.forward = forward;
        view.up      = up;
    }
    else
    {
        // Orbit Follow Offset around the target. Yaw rotates around world Y;
        // pitch then rotates around the orbited local X axis.
        const float yaw_deg   = camera.follow_orbit[0];
        const float pitch_deg = camera.follow_orbit[1];
        Vec3Array offset = {camera.follow_offset[0], camera.follow_offset[1], camera.follow_offset[2]};
        if (std::abs(yaw_deg) > 0.0001f || std::abs(pitch_deg) > 0.0001f)
        {
            constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
            const float yaw   = yaw_deg * kDeg2Rad;
            const float pitch = pitch_deg * kDeg2Rad;
            const float cy = std::cos(yaw),   sy = std::sin(yaw);
            const float cp = std::cos(pitch), sp = std::sin(pitch);
            // Yaw around world Y.
            const Vec3Array after_yaw = {
                cy * offset[0] + sy * offset[2],
                offset[1],
                -sy * offset[0] + cy * offset[2],
            };
            // Pitch around local X (the X axis of the yawed frame, which is
            // (cy, 0, -sy) in world space). Using Rodrigues for that axis.
            const Vec3Array axis = {cy, 0.0f, -sy};
            const float dot = after_yaw[0] * axis[0] + after_yaw[1] * axis[1] + after_yaw[2] * axis[2];
            const Vec3Array cross = {
                axis[1] * after_yaw[2] - axis[2] * after_yaw[1],
                axis[2] * after_yaw[0] - axis[0] * after_yaw[2],
                axis[0] * after_yaw[1] - axis[1] * after_yaw[0],
            };
            offset = {
                after_yaw[0] * cp + cross[0] * sp + axis[0] * dot * (1.0f - cp),
                after_yaw[1] * cp + cross[1] * sp + axis[1] * dot * (1.0f - cp),
                after_yaw[2] * cp + cross[2] * sp + axis[2] * dot * (1.0f - cp),
            };
        }
        view.position = {
            target_position[0] + offset[0],
            target_position[1] + offset[1],
            target_position[2] + offset[2],
        };
    }

    const std::array<float, 3> rotation_offset = {
        camera.follow_rotation_offset[0],
        camera.follow_rotation_offset[1],
        camera.follow_rotation_offset[2],
    };
    if (std::abs(rotation_offset[0]) > 0.0001f
        || std::abs(rotation_offset[1]) > 0.0001f
        || std::abs(rotation_offset[2]) > 0.0001f)
    {
        view.forward = NormalizeArray(RotateByEulerDegreesXYZ(view.forward, rotation_offset));
        view.up      = NormalizeArray(RotateByEulerDegreesXYZ(view.up, rotation_offset));
        if (LengthArray(view.forward) <= 0.0001f)
        {
            view.forward = {0.0f, 0.0f, -1.0f};
        }
        if (LengthArray(view.up) <= 0.0001f
            || std::abs(DotArray(NormalizeArray(view.forward), NormalizeArray(view.up))) >= 0.999f)
        {
            view.up = {0.0f, 1.0f, 0.0f};
        }
    }
    return view;
}
