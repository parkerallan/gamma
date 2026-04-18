#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

using SceneVector3 = std::array<float, 3>;
using SceneColor3 = std::array<float, 3>;

enum class SceneObjectAttributeKind
{
    None,
    EnvironmentLight,
    DirectionalLight,
    SpotLight,
    Camera,
};

struct SceneObjectEnvironmentLightAttributes
{
    SceneColor3 color = {1.0f, 1.0f, 1.0f};
    float intensity = 0.2f;
};

struct SceneObjectDirectionalLightAttributes
{
    SceneColor3 color = {1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
};

struct SceneObjectSpotLightAttributes
{
    SceneColor3 color = {1.0f, 1.0f, 1.0f};
    float intensity = 20.0f;
    float range = 15.0f;
    float inner_cone_degrees = 20.0f;
    float outer_cone_degrees = 30.0f;
};

struct SceneObjectCameraAttributes
{
    float field_of_view_degrees = 55.0f;
    float near_clip = 0.01f;
    float far_clip = 250.0f;
    bool active = false;
};

struct SceneObjectAttribute
{
    SceneObjectAttributeKind kind = SceneObjectAttributeKind::None;
    SceneObjectEnvironmentLightAttributes environment_light{};
    SceneObjectDirectionalLightAttributes directional_light{};
    SceneObjectSpotLightAttributes spot_light{};
    SceneObjectCameraAttributes camera{};
};

struct SceneObjectMetadata
{
    std::string name;
    std::string parent_name;
    SceneVector3 position = {0.0f, 0.0f, 0.0f};
    SceneVector3 rotation = {0.0f, 0.0f, 0.0f};
    SceneVector3 scale = {1.0f, 1.0f, 1.0f};
    std::vector<SceneObjectAttribute> attributes;
    std::string model_path;
    std::vector<std::string> script_paths;
    std::vector<std::string> graph_paths;
};

struct SceneMetadata
{
    bool parsed = false;
    std::string error_message;
    std::string scene_name;
    std::vector<SceneObjectMetadata> objects;
};

SceneMetadata LoadSceneMetadata(const std::filesystem::path& scene_path);
const char* ToDisplayName(SceneObjectAttributeKind kind);
const char* ToStorageName(SceneObjectAttributeKind kind);
SceneObjectAttributeKind ParseSceneObjectAttributeKind(std::string_view value);
SceneObjectAttribute MakeDefaultSceneObjectAttribute(SceneObjectAttributeKind kind);
bool RenameSceneObject(const std::filesystem::path& scene_path, const std::string& object_name, const std::string& new_name);
bool DuplicateSceneObject(const std::filesystem::path& scene_path, const std::string& object_name, std::string* duplicated_root_name = nullptr);
bool DeleteSceneObject(const std::filesystem::path& scene_path, const std::string& object_name);
bool SetSceneObjectParent(const std::filesystem::path& scene_path, const std::string& object_name, const std::string& parent_name);
bool SetSceneObjectPosition(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& position);
bool SetSceneObjectRotation(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& rotation);
bool SetSceneObjectScale(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& scale);
bool AddSceneObjectAttribute(const std::filesystem::path& scene_path, const std::string& object_name, SceneObjectAttributeKind kind);
bool RemoveSceneObjectAttribute(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index);
bool SetSceneObjectAttributeKind(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectAttributeKind kind);
bool SetSceneObjectAttributeColor(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& color);
bool SetSceneObjectAttributeIntensity(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float intensity);
bool SetSceneObjectAttributeRange(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float range);
bool SetSceneObjectAttributeInnerConeDegrees(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float inner_cone_degrees);
bool SetSceneObjectAttributeOuterConeDegrees(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float outer_cone_degrees);
bool SetSceneObjectAttributeFieldOfView(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float field_of_view_degrees);
bool SetSceneObjectAttributeNearClip(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float near_clip);
bool SetSceneObjectAttributeFarClip(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float far_clip);
bool SetSceneObjectCameraActive(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool active);
bool SetSceneObjectModel(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& model_path);
bool ClearSceneObjectModel(const std::filesystem::path& scene_path, const std::string& object_name);
bool AddSceneObjectScript(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& script_path);
bool RemoveSceneObjectScript(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& script_path);
bool AddSceneObjectGraph(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& graph_path);
bool RemoveSceneObjectGraph(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& graph_path);