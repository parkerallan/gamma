#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

using SceneVector3 = std::array<float, 3>;
using SceneColor3 = std::array<float, 3>;

enum class SceneObjectPhysicsShape
{
    None,
    Box,
    Sphere,
    Capsule,
    Mesh,
};

enum class SceneObjectAttributeKind
{
    None,
    EnvironmentLight,
    DirectionalLight,
    SpotLight,
    Camera,
    Rigidbody,
    TriggerVolume,
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

struct SceneObjectRigidbodyAttributes
{
    SceneObjectPhysicsShape shape = SceneObjectPhysicsShape::None;
    bool is_dynamic = false;
    bool lock_rotation_x = false;
    bool lock_rotation_y = false;
    bool lock_rotation_z = false;
    float mass = 1.0f;
    float friction = 0.0f;
    float radius = 0.5f;
    float capsule_half_height = 0.5f;
    SceneVector3 half_extent = {0.5f, 0.5f, 0.5f};
    float linear_damping = 0.05f;
    float angular_damping = 0.05f;
};

struct SceneObjectTriggerVolumeAttributes
{
    SceneVector3 half_extent = {0.5f, 0.5f, 0.5f};
};

struct SceneObjectAttribute
{
    SceneObjectAttributeKind kind = SceneObjectAttributeKind::None;
    SceneObjectEnvironmentLightAttributes environment_light{};
    SceneObjectDirectionalLightAttributes directional_light{};
    SceneObjectSpotLightAttributes spot_light{};
    SceneObjectCameraAttributes camera{};
    SceneObjectRigidbodyAttributes rigidbody{};
    SceneObjectTriggerVolumeAttributes trigger_box{};
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
    SceneVector3 model_visual_offset = {0.0f, 0.0f, 0.0f};
    std::vector<std::string> script_paths;
    std::vector<std::string> graph_paths;
    // Physics
    SceneObjectPhysicsShape physics_shape = SceneObjectPhysicsShape::None;
    bool physics_is_dynamic = false;
    bool physics_is_trigger = false;
    bool physics_lock_rotation_x = false;
    bool physics_lock_rotation_y = false;
    bool physics_lock_rotation_z = false;
    float physics_mass = 1.0f;
    float physics_friction = 0.0f;
    float physics_radius = 0.5f;
    float physics_capsule_half_height = 0.5f;
    SceneVector3 physics_half_extent = {0.5f, 0.5f, 0.5f};
    float physics_linear_damping = 0.05f;
    float physics_angular_damping = 0.05f;
};

struct SceneMetadata
{
    bool parsed = false;
    std::string error_message;
    std::string scene_name;
    std::vector<SceneObjectMetadata> objects;
};

struct ActiveSceneCameraSelection
{
    bool found = false;
    std::string object_name;
    std::size_t attribute_index = 0;
    SceneObjectMetadata object{};
    SceneObjectCameraAttributes camera{};
};

SceneMetadata LoadSceneMetadata(const std::filesystem::path& scene_path);
ActiveSceneCameraSelection FindActiveSceneCamera(const SceneMetadata& scene_metadata);
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
bool SetSceneObjectPhysicsShape(const std::filesystem::path& scene_path, const std::string& object_name, SceneObjectPhysicsShape shape);
bool SetSceneObjectPhysicsDynamic(const std::filesystem::path& scene_path, const std::string& object_name, bool is_dynamic);
bool SetSceneObjectPhysicsMass(const std::filesystem::path& scene_path, const std::string& object_name, float mass);
bool SetSceneObjectPhysicsFriction(const std::filesystem::path& scene_path, const std::string& object_name, float friction);
bool SetSceneObjectPhysicsRadius(const std::filesystem::path& scene_path, const std::string& object_name, float radius);
bool SetSceneObjectPhysicsHalfExtent(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& half_extent);
bool SetSceneObjectPhysicsLinearDamping(const std::filesystem::path& scene_path, const std::string& object_name, float linear_damping);
bool SetSceneObjectPhysicsAngularDamping(const std::filesystem::path& scene_path, const std::string& object_name, float angular_damping);
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
bool SetSceneObjectAttributePhysicsShape(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectPhysicsShape shape);
bool SetSceneObjectAttributePhysicsDynamic(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool is_dynamic);
bool SetSceneObjectAttributePhysicsLockRotationX(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool locked);
bool SetSceneObjectAttributePhysicsLockRotationY(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool locked);
bool SetSceneObjectAttributePhysicsLockRotationZ(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool locked);
bool SetSceneObjectAttributePhysicsMass(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float mass);
bool SetSceneObjectAttributePhysicsFriction(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float friction);
bool SetSceneObjectAttributePhysicsRadius(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float radius);
bool SetSceneObjectAttributePhysicsCapsuleHalfHeight(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float capsule_half_height);
bool SetSceneObjectAttributePhysicsHalfExtent(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneVector3& half_extent);
bool SetSceneObjectAttributePhysicsLinearDamping(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float linear_damping);
bool SetSceneObjectAttributePhysicsAngularDamping(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float angular_damping);
bool SetSceneObjectAttributeTriggerHalfExtent(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneVector3& half_extent);
bool SetSceneObjectModel(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& model_path);
bool SetSceneObjectModelVisualOffset(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& model_visual_offset);
bool ClearSceneObjectModel(const std::filesystem::path& scene_path, const std::string& object_name);
bool AddSceneObjectScript(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& script_path);
bool RemoveSceneObjectScript(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& script_path);
bool AddSceneObjectGraph(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& graph_path);
bool RemoveSceneObjectGraph(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& graph_path);