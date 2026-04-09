#include "panels/SceneViewportRenderer.h"

#include <SDL3/SDL.h>

#include <ImGuizmo.h>

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
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

SDL_GPUShader* LoadShaderFromFile(
    SDL_GPUDevice* device,
    const std::filesystem::path& path,
    SDL_GPUShaderStage stage,
    Uint32 num_uniform_buffers,
    Uint32 num_samplers)
{
    const std::vector<std::uint8_t> shader_bytes = ReadBinaryFile(path);
    if (shader_bytes.empty())
    {
        SDL_Log("Failed to read shader file: %s", path.string().c_str());
        return nullptr;
    }

    SDL_GPUShaderCreateInfo create_info = {};
    create_info.format = SDL_GPU_SHADERFORMAT_SPIRV;
    create_info.code = shader_bytes.data();
    create_info.code_size = static_cast<Uint32>(shader_bytes.size());
    create_info.entrypoint = "main";
    create_info.stage = stage;
    create_info.num_uniform_buffers = num_uniform_buffers;
    create_info.num_samplers = num_samplers;
    create_info.num_storage_buffers = 0;
    create_info.num_storage_textures = 0;
    return SDL_CreateGPUShader(device, &create_info);
}

bool UploadTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture, const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height)
{
    if (device == nullptr || texture == nullptr || pixels == nullptr || width == 0 || height == 0)
    {
        return false;
    }

    const Uint32 upload_size = width * height * 4u;

    SDL_GPUTransferBufferCreateInfo transfer_info = {};
    transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transfer_info.size = upload_size;
    SDL_GPUTransferBuffer* transfer_buffer = SDL_CreateGPUTransferBuffer(device, &transfer_info);
    if (transfer_buffer == nullptr)
    {
        return false;
    }

    void* mapped = SDL_MapGPUTransferBuffer(device, transfer_buffer, false);
    if (mapped == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(device, transfer_buffer);
        return false;
    }

    std::memcpy(mapped, pixels, upload_size);
    SDL_UnmapGPUTransferBuffer(device, transfer_buffer);

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(device);
    if (command_buffer == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(device, transfer_buffer);
        return false;
    }

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);

    SDL_GPUTextureTransferInfo source = {};
    source.transfer_buffer = transfer_buffer;
    source.offset = 0;
    source.pixels_per_row = width;
    source.rows_per_layer = height;

    SDL_GPUTextureRegion destination = {};
    destination.texture = texture;
    destination.mip_level = 0;
    destination.layer = 0;
    destination.x = 0;
    destination.y = 0;
    destination.z = 0;
    destination.w = width;
    destination.h = height;
    destination.d = 1;

    SDL_UploadToGPUTexture(copy_pass, &source, &destination, false);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(command_buffer);
    SDL_ReleaseGPUTransferBuffer(device, transfer_buffer);
    return true;
}

SDL_GPUTexture* CreateTextureFromAsset(SDL_GPUDevice* device, const ModelTextureAsset& texture_asset)
{
    if (device == nullptr || !texture_asset.valid || texture_asset.width <= 0 || texture_asset.height <= 0 || texture_asset.pixels.empty())
    {
        return nullptr;
    }

    SDL_GPUTextureCreateInfo texture_info = {};
    texture_info.type = SDL_GPU_TEXTURETYPE_2D;
    texture_info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    texture_info.width = static_cast<Uint32>(texture_asset.width);
    texture_info.height = static_cast<Uint32>(texture_asset.height);
    texture_info.layer_count_or_depth = 1;
    texture_info.num_levels = 1;
    texture_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    texture_info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;

    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &texture_info);
    if (texture == nullptr)
    {
        return nullptr;
    }

    if (!UploadTexture(device, texture, texture_asset.pixels.data(), texture_info.width, texture_info.height))
    {
        SDL_ReleaseGPUTexture(device, texture);
        return nullptr;
    }

    return texture;
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

bool SceneViewportRenderer::Initialize(SDL_GPUDevice* device, SDL_GPUTextureFormat color_target_format)
{
    device_ = device;
    color_target_format_ = color_target_format;
    return EnsurePipeline();
}

void SceneViewportRenderer::ReleaseMeshCacheEntry(GpuMeshCacheEntry& entry)
{
    if (device_ != nullptr)
    {
        if (entry.vertex_buffer != nullptr)
        {
            SDL_ReleaseGPUBuffer(device_, entry.vertex_buffer);
            entry.vertex_buffer = nullptr;
        }
        if (entry.index_buffer != nullptr)
        {
            SDL_ReleaseGPUBuffer(device_, entry.index_buffer);
            entry.index_buffer = nullptr;
        }
        for (SDL_GPUTexture*& material_texture : entry.material_textures)
        {
            if (material_texture != nullptr)
            {
                SDL_ReleaseGPUTexture(device_, material_texture);
                material_texture = nullptr;
            }
        }
    }

    entry.sections.clear();
    entry.material_textures.clear();
}

void SceneViewportRenderer::ReleaseGridCacheEntry()
{
    if (device_ != nullptr)
    {
        if (grid_cache_.vertex_buffer != nullptr)
        {
            SDL_ReleaseGPUBuffer(device_, grid_cache_.vertex_buffer);
            grid_cache_.vertex_buffer = nullptr;
        }
        if (grid_cache_.index_buffer != nullptr)
        {
            SDL_ReleaseGPUBuffer(device_, grid_cache_.index_buffer);
            grid_cache_.index_buffer = nullptr;
        }
    }

    grid_cache_.index_count = 0;
    grid_cache_.spacing = 0.0f;
    grid_cache_.extent = 0.0f;
    grid_cache_.origin_x = 0.0f;
    grid_cache_.origin_z = 0.0f;
}

void SceneViewportRenderer::DestroyRenderTargets()
{
    if (device_ != nullptr)
    {
        if (color_texture_ != nullptr)
        {
            SDL_ReleaseGPUTexture(device_, color_texture_);
            color_texture_ = nullptr;
        }
        if (depth_texture_ != nullptr)
        {
            SDL_ReleaseGPUTexture(device_, depth_texture_);
            depth_texture_ = nullptr;
        }
    }

    target_width_ = 0;
    target_height_ = 0;
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

    if (device_ != nullptr && fallback_texture_ != nullptr)
    {
        SDL_ReleaseGPUTexture(device_, fallback_texture_);
        fallback_texture_ = nullptr;
    }

    if (device_ != nullptr && material_sampler_ != nullptr)
    {
        SDL_ReleaseGPUSampler(device_, material_sampler_);
        material_sampler_ = nullptr;
    }

    if (device_ != nullptr && pipeline_ != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(device_, pipeline_);
        pipeline_ = nullptr;
    }

    queued_objects_.clear();
    render_requested_ = false;
    device_ = nullptr;
    color_target_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
}

void SceneViewportRenderer::BeginFrame()
{
    render_requested_ = false;
    queued_objects_.clear();
}

bool SceneViewportRenderer::EnsureMaterialResources()
{
    if (device_ == nullptr)
    {
        return false;
    }

    if (material_sampler_ == nullptr)
    {
        SDL_GPUSamplerCreateInfo sampler_info = {};
        sampler_info.min_filter = SDL_GPU_FILTER_LINEAR;
        sampler_info.mag_filter = SDL_GPU_FILTER_LINEAR;
        sampler_info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        sampler_info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        sampler_info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        sampler_info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        sampler_info.compare_op = SDL_GPU_COMPAREOP_NEVER;
        sampler_info.min_lod = 0.0f;
        sampler_info.max_lod = 0.0f;
        material_sampler_ = SDL_CreateGPUSampler(device_, &sampler_info);
        if (material_sampler_ == nullptr)
        {
            return false;
        }
    }

    if (fallback_texture_ == nullptr)
    {
        ModelTextureAsset fallback_texture_asset;
        fallback_texture_asset.valid = true;
        fallback_texture_asset.width = 1;
        fallback_texture_asset.height = 1;
        fallback_texture_asset.pixels = {255, 255, 255, 255};
        fallback_texture_ = CreateTextureFromAsset(device_, fallback_texture_asset);
        if (fallback_texture_ == nullptr)
        {
            return false;
        }
    }

    return true;
}

bool SceneViewportRenderer::EnsurePipeline()
{
    if (pipeline_ != nullptr)
    {
        return true;
    }

    if (device_ == nullptr || color_target_format_ == SDL_GPU_TEXTUREFORMAT_INVALID)
    {
        return false;
    }

    if (!EnsureMaterialResources())
    {
        return false;
    }

    SDL_GPUShader* vertex_shader = LoadShaderFromFile(device_, ResolveShaderPath("scene_viewport.vert.spv"), SDL_GPU_SHADERSTAGE_VERTEX, 1, 0);
    SDL_GPUShader* fragment_shader = LoadShaderFromFile(device_, ResolveShaderPath("scene_viewport.frag.spv"), SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (vertex_shader == nullptr || fragment_shader == nullptr)
    {
        if (vertex_shader != nullptr)
        {
            SDL_ReleaseGPUShader(device_, vertex_shader);
        }
        if (fragment_shader != nullptr)
        {
            SDL_ReleaseGPUShader(device_, fragment_shader);
        }
        return false;
    }

    SDL_GPUColorTargetDescription color_target_desc = {};
    color_target_desc.format = color_target_format_;
    color_target_desc.blend_state.color_write_mask = 0xF;

    SDL_GPUVertexBufferDescription vertex_buffer_desc = {};
    vertex_buffer_desc.slot = 0;
    vertex_buffer_desc.pitch = sizeof(SceneGpuVertex);
    vertex_buffer_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertex_attributes[4] = {};
    vertex_attributes[0].location = 0;
    vertex_attributes[0].buffer_slot = 0;
    vertex_attributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertex_attributes[0].offset = 0;
    vertex_attributes[1].location = 1;
    vertex_attributes[1].buffer_slot = 0;
    vertex_attributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertex_attributes[1].offset = sizeof(float) * 3;
    vertex_attributes[2].location = 2;
    vertex_attributes[2].buffer_slot = 0;
    vertex_attributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertex_attributes[2].offset = sizeof(float) * 6;
    vertex_attributes[3].location = 3;
    vertex_attributes[3].buffer_slot = 0;
    vertex_attributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    vertex_attributes[3].offset = sizeof(float) * 8;

    SDL_GPUGraphicsPipelineCreateInfo pipeline_desc = {};
    pipeline_desc.vertex_shader = vertex_shader;
    pipeline_desc.fragment_shader = fragment_shader;
    pipeline_desc.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipeline_desc.target_info.num_color_targets = 1;
    pipeline_desc.target_info.color_target_descriptions = &color_target_desc;
    pipeline_desc.target_info.has_depth_stencil_target = true;
    pipeline_desc.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    pipeline_desc.depth_stencil_state.enable_depth_test = true;
    pipeline_desc.depth_stencil_state.enable_depth_write = true;
    pipeline_desc.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    pipeline_desc.rasterizer_state.enable_depth_clip = true;
    pipeline_desc.vertex_input_state.num_vertex_buffers = 1;
    pipeline_desc.vertex_input_state.vertex_buffer_descriptions = &vertex_buffer_desc;
    pipeline_desc.vertex_input_state.num_vertex_attributes = 4;
    pipeline_desc.vertex_input_state.vertex_attributes = vertex_attributes;

    pipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipeline_desc);

    SDL_ReleaseGPUShader(device_, vertex_shader);
    SDL_ReleaseGPUShader(device_, fragment_shader);

    return pipeline_ != nullptr;
}

bool SceneViewportRenderer::EnsureGridCacheEntry()
{
    if (!grid_enabled_ || device_ == nullptr || grid_spacing_ <= 0.0f)
    {
        ReleaseGridCacheEntry();
        return false;
    }

    if (grid_cache_.vertex_buffer != nullptr &&
        grid_cache_.index_buffer != nullptr &&
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

    SDL_GPUBufferCreateInfo vertex_buffer_info = {};
    vertex_buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertex_buffer_info.size = static_cast<Uint32>(vertices.size() * sizeof(SceneGpuVertex));
    grid_cache_.vertex_buffer = SDL_CreateGPUBuffer(device_, &vertex_buffer_info);
    if (grid_cache_.vertex_buffer == nullptr)
    {
        ReleaseGridCacheEntry();
        return false;
    }

    SDL_GPUBufferCreateInfo index_buffer_info = {};
    index_buffer_info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    index_buffer_info.size = static_cast<Uint32>(indices.size() * sizeof(std::uint32_t));
    grid_cache_.index_buffer = SDL_CreateGPUBuffer(device_, &index_buffer_info);
    if (grid_cache_.index_buffer == nullptr)
    {
        ReleaseGridCacheEntry();
        return false;
    }

    SDL_GPUTransferBufferCreateInfo vertex_transfer_info = {};
    vertex_transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertex_transfer_info.size = vertex_buffer_info.size;
    SDL_GPUTransferBuffer* vertex_transfer = SDL_CreateGPUTransferBuffer(device_, &vertex_transfer_info);

    SDL_GPUTransferBufferCreateInfo index_transfer_info = {};
    index_transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    index_transfer_info.size = index_buffer_info.size;
    SDL_GPUTransferBuffer* index_transfer = SDL_CreateGPUTransferBuffer(device_, &index_transfer_info);
    if (vertex_transfer == nullptr || index_transfer == nullptr)
    {
        if (vertex_transfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(device_, vertex_transfer);
        }
        if (index_transfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(device_, index_transfer);
        }
        ReleaseGridCacheEntry();
        return false;
    }

    void* vertex_map = SDL_MapGPUTransferBuffer(device_, vertex_transfer, false);
    void* index_map = SDL_MapGPUTransferBuffer(device_, index_transfer, false);
    if (vertex_map == nullptr || index_map == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(device_, vertex_transfer);
        SDL_ReleaseGPUTransferBuffer(device_, index_transfer);
        ReleaseGridCacheEntry();
        return false;
    }

    std::memcpy(vertex_map, vertices.data(), vertices.size() * sizeof(SceneGpuVertex));
    std::memcpy(index_map, indices.data(), indices.size() * sizeof(std::uint32_t));
    SDL_UnmapGPUTransferBuffer(device_, vertex_transfer);
    SDL_UnmapGPUTransferBuffer(device_, index_transfer);

    SDL_GPUCommandBuffer* upload_command_buffer = SDL_AcquireGPUCommandBuffer(device_);
    if (upload_command_buffer == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(device_, vertex_transfer);
        SDL_ReleaseGPUTransferBuffer(device_, index_transfer);
        ReleaseGridCacheEntry();
        return false;
    }

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(upload_command_buffer);
    SDL_GPUTransferBufferLocation vertex_location = {vertex_transfer, 0};
    SDL_GPUBufferRegion vertex_region = {grid_cache_.vertex_buffer, 0, vertex_buffer_info.size};
    SDL_UploadToGPUBuffer(copy_pass, &vertex_location, &vertex_region, false);

    SDL_GPUTransferBufferLocation index_location = {index_transfer, 0};
    SDL_GPUBufferRegion index_region = {grid_cache_.index_buffer, 0, index_buffer_info.size};
    SDL_UploadToGPUBuffer(copy_pass, &index_location, &index_region, false);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(upload_command_buffer);

    SDL_ReleaseGPUTransferBuffer(device_, vertex_transfer);
    SDL_ReleaseGPUTransferBuffer(device_, index_transfer);

    grid_cache_.index_count = static_cast<std::uint32_t>(indices.size());
    grid_cache_.spacing = grid_spacing_;
    grid_cache_.extent = grid_extent_;
    grid_cache_.origin_x = grid_origin_x_;
    grid_cache_.origin_z = grid_origin_z_;
    return true;
}

bool SceneViewportRenderer::EnsureRenderTargets(std::uint32_t width, std::uint32_t height)
{
    if (color_texture_ != nullptr && depth_texture_ != nullptr && target_width_ == width && target_height_ == height)
    {
        return true;
    }

    DestroyRenderTargets();

    SDL_GPUTextureCreateInfo color_info = {};
    color_info.type = SDL_GPU_TEXTURETYPE_2D;
    color_info.format = color_target_format_;
    color_info.width = width;
    color_info.height = height;
    color_info.layer_count_or_depth = 1;
    color_info.num_levels = 1;
    color_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    color_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    color_texture_ = SDL_CreateGPUTexture(device_, &color_info);
    if (color_texture_ == nullptr)
    {
        DestroyRenderTargets();
        return false;
    }

    SDL_GPUTextureCreateInfo depth_info = {};
    depth_info.type = SDL_GPU_TEXTURETYPE_2D;
    depth_info.format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    depth_info.width = width;
    depth_info.height = height;
    depth_info.layer_count_or_depth = 1;
    depth_info.num_levels = 1;
    depth_info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    depth_info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depth_texture_ = SDL_CreateGPUTexture(device_, &depth_info);
    if (depth_texture_ == nullptr)
    {
        DestroyRenderTargets();
        return false;
    }

    target_width_ = width;
    target_height_ = height;
    return true;
}

bool SceneViewportRenderer::EnsureMeshCacheEntry(const std::filesystem::path& model_path, const SceneViewportResolvedModel& resolved_model)
{
    if (device_ == nullptr || resolved_model.asset == nullptr || !resolved_model.asset->loaded)
    {
        return false;
    }

    GpuMeshCacheEntry& cache_entry = mesh_cache_[model_path];
    if (cache_entry.vertex_buffer != nullptr && cache_entry.index_buffer != nullptr && cache_entry.write_time == resolved_model.write_time)
    {
        return true;
    }

    ReleaseMeshCacheEntry(cache_entry);

    std::vector<SceneGpuVertex> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(4096);
    indices.reserve(8192);

    cache_entry.material_textures.resize(resolved_model.asset->materials.size(), nullptr);
    for (std::size_t material_index = 0; material_index < resolved_model.asset->materials.size(); ++material_index)
    {
        const ModelMaterialAsset& material = resolved_model.asset->materials[material_index];
        if (material.base_color_texture.valid)
        {
            cache_entry.material_textures[material_index] = CreateTextureFromAsset(device_, material.base_color_texture);
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

    SDL_GPUBufferCreateInfo vertex_buffer_info = {};
    vertex_buffer_info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertex_buffer_info.size = static_cast<Uint32>(vertices.size() * sizeof(SceneGpuVertex));
    cache_entry.vertex_buffer = SDL_CreateGPUBuffer(device_, &vertex_buffer_info);
    if (cache_entry.vertex_buffer == nullptr)
    {
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    SDL_GPUBufferCreateInfo index_buffer_info = {};
    index_buffer_info.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    index_buffer_info.size = static_cast<Uint32>(indices.size() * sizeof(std::uint32_t));
    cache_entry.index_buffer = SDL_CreateGPUBuffer(device_, &index_buffer_info);
    if (cache_entry.index_buffer == nullptr)
    {
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    SDL_GPUTransferBufferCreateInfo vertex_transfer_info = {};
    vertex_transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertex_transfer_info.size = vertex_buffer_info.size;
    SDL_GPUTransferBuffer* vertex_transfer = SDL_CreateGPUTransferBuffer(device_, &vertex_transfer_info);

    SDL_GPUTransferBufferCreateInfo index_transfer_info = {};
    index_transfer_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    index_transfer_info.size = index_buffer_info.size;
    SDL_GPUTransferBuffer* index_transfer = SDL_CreateGPUTransferBuffer(device_, &index_transfer_info);
    if (vertex_transfer == nullptr || index_transfer == nullptr)
    {
        if (vertex_transfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(device_, vertex_transfer);
        }
        if (index_transfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(device_, index_transfer);
        }
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    void* vertex_map = SDL_MapGPUTransferBuffer(device_, vertex_transfer, false);
    void* index_map = SDL_MapGPUTransferBuffer(device_, index_transfer, false);
    if (vertex_map == nullptr || index_map == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(device_, vertex_transfer);
        SDL_ReleaseGPUTransferBuffer(device_, index_transfer);
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    std::memcpy(vertex_map, vertices.data(), vertices.size() * sizeof(SceneGpuVertex));
    std::memcpy(index_map, indices.data(), indices.size() * sizeof(std::uint32_t));
    SDL_UnmapGPUTransferBuffer(device_, vertex_transfer);
    SDL_UnmapGPUTransferBuffer(device_, index_transfer);

    SDL_GPUCommandBuffer* upload_command_buffer = SDL_AcquireGPUCommandBuffer(device_);
    if (upload_command_buffer == nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(device_, vertex_transfer);
        SDL_ReleaseGPUTransferBuffer(device_, index_transfer);
        ReleaseMeshCacheEntry(cache_entry);
        return false;
    }

    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(upload_command_buffer);
    SDL_GPUTransferBufferLocation vertex_location = {vertex_transfer, 0};
    SDL_GPUBufferRegion vertex_region = {cache_entry.vertex_buffer, 0, vertex_buffer_info.size};
    SDL_UploadToGPUBuffer(copy_pass, &vertex_location, &vertex_region, false);

    SDL_GPUTransferBufferLocation index_location = {index_transfer, 0};
    SDL_GPUBufferRegion index_region = {cache_entry.index_buffer, 0, index_buffer_info.size};
    SDL_UploadToGPUBuffer(copy_pass, &index_location, &index_region, false);
    SDL_EndGPUCopyPass(copy_pass);
    SDL_SubmitGPUCommandBuffer(upload_command_buffer);

    SDL_ReleaseGPUTransferBuffer(device_, vertex_transfer);
    SDL_ReleaseGPUTransferBuffer(device_, index_transfer);

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

    if (!EnsurePipeline() || !EnsureRenderTargets(target_width, target_height))
    {
        draw_list->AddRect(min, max, IM_COL32(92, 99, 110, 255), 8.0f, 0, 1.5f);
        draw_list->AddText(ImVec2(min.x + 12.0f, min.y + 12.0f), IM_COL32(235, 238, 242, 255), "Scene GPU renderer unavailable");
        return;
    }

    if (color_texture_ != nullptr)
    {
        draw_list->AddImage(reinterpret_cast<ImTextureID>(color_texture_), min, max);
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

    if (queued_objects_.empty())
    {
        const char* message = "No attached models found in the active scene";
        const ImVec2 message_size = ImGui::CalcTextSize(message);
        draw_list->AddText(ImVec2((min.x + max.x - message_size.x) * 0.5f, (min.y + max.y - message_size.y) * 0.5f), IM_COL32(235, 238, 242, 255), message);
        return;
    }

    const Vec3 scene_center = Multiply(Add(world_min, world_max), 0.5f);
    const float scene_radius = (std::max)(0.75f, Length(Subtract(world_max, world_min)) * 0.6f);
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
    if (state.selected_item_path == state.active_scene_path && !state.selected_scene_object_name.empty())
    {
        const auto selected_object_it = std::find_if(queued_objects_.begin(), queued_objects_.end(), [&](const QueuedSceneObject& object)
        {
            return object.selected;
        });

        if (selected_object_it != queued_objects_.end())
        {
            gizmo_object = &(*selected_object_it);
        }
    }
    else if (queued_objects_.size() == 1)
    {
        gizmo_object = &queued_objects_.front();
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

    const std::string footer = std::to_string(queued_objects_.size()) + " model object" + (queued_objects_.size() == 1 ? "" : "s") + " rendered to GPU target";
    draw_list->AddText(ImVec2(min.x + 12.0f, max.y - 24.0f), IM_COL32(145, 152, 163, 255), footer.c_str());
    ImGui::EndChild();
}

void SceneViewportRenderer::RenderGpu(SDL_GPUCommandBuffer* command_buffer)
{
    if (!render_requested_ || device_ == nullptr || command_buffer == nullptr || pipeline_ == nullptr || color_texture_ == nullptr || depth_texture_ == nullptr)
    {
        return;
    }

    SDL_GPUColorTargetInfo color_target = {};
    color_target.texture = color_texture_;
    color_target.clear_color = SDL_FColor{0.08f, 0.09f, 0.11f, 1.0f};
    color_target.load_op = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo depth_target = {};
    depth_target.texture = depth_texture_;
    depth_target.clear_depth = 1.0f;
    depth_target.load_op = SDL_GPU_LOADOP_CLEAR;
    depth_target.store_op = SDL_GPU_STOREOP_STORE;
    depth_target.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth_target.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(command_buffer, &color_target, 1, &depth_target);
    SDL_BindGPUGraphicsPipeline(render_pass, pipeline_);

    if (grid_enabled_ && EnsureGridCacheEntry())
    {
        SDL_GPUBufferBinding grid_vertex_binding = {grid_cache_.vertex_buffer, 0};
        SDL_GPUBufferBinding grid_index_binding = {grid_cache_.index_buffer, 0};
        SDL_BindGPUVertexBuffers(render_pass, 0, &grid_vertex_binding, 1);
        SDL_BindGPUIndexBuffer(render_pass, &grid_index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        SceneUniformBlock grid_uniform_block = {};
        SetIdentity(grid_uniform_block.model);
        std::memcpy(grid_uniform_block.model_view_projection, view_projection_.data(), sizeof(grid_uniform_block.model_view_projection));
        SDL_PushGPUVertexUniformData(command_buffer, 0, &grid_uniform_block, sizeof(grid_uniform_block));

        SDL_GPUTextureSamplerBinding grid_sampler_binding = {};
        grid_sampler_binding.texture = fallback_texture_;
        grid_sampler_binding.sampler = material_sampler_;
        SDL_BindGPUFragmentSamplers(render_pass, 0, &grid_sampler_binding, 1);
        SDL_DrawGPUIndexedPrimitives(render_pass, grid_cache_.index_count, 1, 0, 0, 0);
    }

    for (const QueuedSceneObject& object : queued_objects_)
    {
        const auto mesh_it = mesh_cache_.find(object.model_path);
        if (mesh_it == mesh_cache_.end())
        {
            continue;
        }

        const GpuMeshCacheEntry& mesh_entry = mesh_it->second;
        SDL_GPUBufferBinding vertex_binding = {mesh_entry.vertex_buffer, 0};
        SDL_GPUBufferBinding index_binding = {mesh_entry.index_buffer, 0};
        SDL_BindGPUVertexBuffers(render_pass, 0, &vertex_binding, 1);
        SDL_BindGPUIndexBuffer(render_pass, &index_binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        SceneUniformBlock uniform_block = {};
        BuildModelMatrix(object, uniform_block.model);
        MultiplyMatrix(view_projection_.data(), uniform_block.model, uniform_block.model_view_projection);
        SDL_PushGPUVertexUniformData(command_buffer, 0, &uniform_block, sizeof(uniform_block));

        for (const GpuMeshSection& section : mesh_entry.sections)
        {
            SDL_GPUTexture* material_texture = fallback_texture_;
            if (section.material_index < mesh_entry.material_textures.size() && mesh_entry.material_textures[section.material_index] != nullptr)
            {
                material_texture = mesh_entry.material_textures[section.material_index];
            }

            SDL_GPUTextureSamplerBinding sampler_binding = {};
            sampler_binding.texture = material_texture;
            sampler_binding.sampler = material_sampler_;
            SDL_BindGPUFragmentSamplers(render_pass, 0, &sampler_binding, 1);
            SDL_DrawGPUIndexedPrimitives(render_pass, section.index_count, 1, section.first_index, 0, 0);
        }
    }

    SDL_EndGPURenderPass(render_pass);
}