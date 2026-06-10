#pragma once

#include <array>
#include <cstdint>
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
    Model,
    Script,
    Graph,
    Shape3D,
    EnvironmentLight,
    DirectionalLight,
    PointLight,
    SpotLight,
    Camera,
    Rigidbody,
    TriggerVolume,
    Animator,
    Text2D,
    Image2D,
    Color2D,
    Skybox,
    Audio,
    Video2D,
    Effects,
    Shader,
};

enum class SceneObjectShaderType
{
    None,
    Water,
    Cloud,
};

enum class SceneObjectCameraType
{
    Fixed,
    Follow,
};

enum class SceneObjectAudioPlayMode
{
    Off,
    On,
};

enum class SceneObjectVideoPlayMode
{
    Off,
    PlayOnce,
    Loop,
};

enum class SceneObjectImagePlayMode
{
    Off,
    PlayOnce,
    Loop,
};

enum class SceneObjectEffectsPlayMode
{
    Stop,
    PlayOnce,
    Loop,
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

struct SceneObjectPointLightAttributes
{
    SceneColor3 color = {1.0f, 1.0f, 1.0f};
    float intensity = 25.0f;
    float range = 15.0f;
    float source_radius = 0.1f;
    float halo_intensity = 1.0f;
    float halo_radius = 1.0f;
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
    SceneObjectCameraType type = SceneObjectCameraType::Fixed;
    std::string follow_target_object;
    SceneVector3 follow_offset = {0.0f, 2.0f, 5.0f};
    // Orbit around the follow target's position. (x = yaw degrees around
    // world Y, y = pitch degrees around the orbited X axis, z = unused).
    // Rotates the Follow Offset vector around the target before adding it,
    // so the camera circles the target without changing Follow Offset's
    // length. Has no effect when Lock Position is on.
    SceneVector3 follow_orbit = {0.0f, 0.0f, 0.0f};
    // Additional Euler rotation (XYZ, degrees) applied on top of the camera
    // object's own rotation in Follow mode. Lets the user re-aim the camera
    // without having to rotate the camera scene object itself.
    SceneVector3 follow_rotation_offset = {0.0f, 0.0f, 0.0f};
    // When true, the camera stays at its authored world position and only
    // rotates to look at the follow target. Follow Offset is ignored.
    bool follow_lock_position = false;
    // Exponential smoothing time constant in seconds. 0 = no smoothing
    // (camera snaps to target+offset every frame). Larger values = laggier
    // chase camera.
    float follow_smoothing = 0.0f;
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

struct SceneObjectAnimatorAttributes
{
    std::string controller_path;
    std::string initial_state;
    float playback_speed = 1.0f;
    bool auto_play = true;
};

struct SceneObjectText2DAttributes
{
    std::string font_path;
    std::string text;
    float x = 0.05f;
    float y = 0.05f;
    float width = 400.0f;
    float height = 80.0f;
    float font_size = 32.0f;
    SceneColor3 color = {1.0f, 1.0f, 1.0f};
    float alpha = 1.0f;
    bool lock_aspect_ratio = false;
    int priority = 1;
};

struct SceneObjectImage2DAttributes
{
    std::string image_path;
    float x = 0.05f;
    float y = 0.05f;
    float width = 256.0f;
    float height = 256.0f;
    SceneColor3 tint = {1.0f, 1.0f, 1.0f};
    float alpha = 1.0f;
    bool lock_aspect_ratio = false;
    bool stretch_to_screen = false;
    SceneObjectImagePlayMode play_mode = SceneObjectImagePlayMode::Loop;
    int priority = 1;
};

struct SceneObjectColor2DAttributes
{
    float x = 0.05f;
    float y = 0.05f;
    float width = 256.0f;
    float height = 256.0f;
    SceneColor3 color = {1.0f, 1.0f, 1.0f};
    float alpha = 1.0f;
    bool lock_aspect_ratio = false;
    bool stretch_to_screen = false;
    int priority = 1;
};

struct SceneObjectSkyboxAttributes
{
    std::string image_path;
    float rotation_degrees = 0.0f;
};

struct SceneObjectAudioAttributes
{
    std::string clip_path;
    SceneObjectAudioPlayMode play_mode = SceneObjectAudioPlayMode::Off;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool spatialize_3d = true;
    float min_distance = 1.0f;
    float max_distance = 50.0f;
    float doppler_factor = 1.0f;
};

struct SceneObjectVideo2DAttributes
{
    std::string video_path;
    float x = 0.0f;
    float y = 0.0f;
    float width = 480.0f;
    float height = 270.0f;
    SceneColor3 tint = {1.0f, 1.0f, 1.0f};
    float alpha = 1.0f;
    bool lock_aspect_ratio = true;
    bool stretch_to_screen = false;
    int priority = 1;
    SceneObjectVideoPlayMode play_mode = SceneObjectVideoPlayMode::Loop;
    float volume = 1.0f;
    bool muted = false;
};

struct SceneObjectEffectsAttributes
{
    std::string effect_path;
    SceneObjectEffectsPlayMode trigger_mode = SceneObjectEffectsPlayMode::Loop;  // serialized: how to play when triggered
    SceneObjectEffectsPlayMode play_mode    = SceneObjectEffectsPlayMode::Stop;  // runtime only: always starts Stop
};

struct SceneObjectShape3DAttributes
{
    std::string shape_path;
};

struct SceneObjectShaderAttributes
{
    SceneObjectShaderType type = SceneObjectShaderType::None;
};

struct SceneObjectAttribute
{
    SceneObjectAttributeKind kind = SceneObjectAttributeKind::None;
    SceneObjectEnvironmentLightAttributes environment_light{};
    SceneObjectDirectionalLightAttributes directional_light{};
    SceneObjectPointLightAttributes point_light{};
    SceneObjectSpotLightAttributes spot_light{};
    SceneObjectCameraAttributes camera{};
    SceneObjectRigidbodyAttributes rigidbody{};
    SceneObjectTriggerVolumeAttributes trigger_box{};
    SceneObjectAnimatorAttributes animator{};
    SceneObjectText2DAttributes text_2d{};
    SceneObjectImage2DAttributes image_2d{};
    SceneObjectColor2DAttributes color_2d{};
    SceneObjectSkyboxAttributes skybox{};
    SceneObjectAudioAttributes audio{};
    SceneObjectVideo2DAttributes video_2d{};
    SceneObjectShape3DAttributes shape_3d{};
    SceneObjectEffectsAttributes effects{};
    SceneObjectShaderAttributes shader{};
};

struct SceneObjectMetadata
{
    std::string name;
    std::string parent_name;
    bool enabled = true;
    bool enabled_in_hierarchy = true;
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
    std::vector<std::string> tags;
};

struct SceneMetadata
{
    bool parsed = false;
    std::string error_message;
    std::string scene_name;
    std::vector<SceneObjectMetadata> objects;
    // Reference viewport size for scaling 2D overlays when viewport is resized.
    // Text and image positions are stored relative to this reference resolution.
    std::uint32_t reference_viewport_width = 1920;
    std::uint32_t reference_viewport_height = 1080;
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
void ResolveSceneObjectEnabledState(SceneMetadata& scene_metadata);
ActiveSceneCameraSelection FindActiveSceneCamera(const SceneMetadata& scene_metadata);
bool IsSceneObjectEnabledInHierarchy(const SceneMetadata& scene_metadata, const std::string& object_name);
const char* ToDisplayName(SceneObjectAttributeKind kind);
const char* ToStorageName(SceneObjectAttributeKind kind);
SceneObjectAttributeKind ParseSceneObjectAttributeKind(std::string_view value);
SceneObjectAttribute MakeDefaultSceneObjectAttribute(SceneObjectAttributeKind kind);
bool RenameSceneObject(const std::filesystem::path& scene_path, const std::string& object_name, const std::string& new_name);
bool DuplicateSceneObject(const std::filesystem::path& scene_path, const std::string& object_name, std::string* duplicated_root_name = nullptr);
bool DeleteSceneObject(const std::filesystem::path& scene_path, const std::string& object_name);
bool SetSceneObjectParent(const std::filesystem::path& scene_path, const std::string& object_name, const std::string& parent_name);
bool SetSceneObjectEnabled(const std::filesystem::path& scene_path, const std::string& object_name, bool enabled);
bool SetSceneObjectPosition(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& position);
bool SetSceneObjectRotation(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& rotation);
bool SetSceneObjectScale(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& scale);
bool SetSceneObjectTransform(
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    const SceneVector3& position,
    const SceneVector3& rotation,
    const SceneVector3& scale,
    bool write_position,
    bool write_rotation,
    bool write_scale);
bool SetSceneObjectPhysicsShape(const std::filesystem::path& scene_path, const std::string& object_name, SceneObjectPhysicsShape shape);
bool SetSceneObjectPhysicsDynamic(const std::filesystem::path& scene_path, const std::string& object_name, bool is_dynamic);
bool SetSceneObjectPhysicsMass(const std::filesystem::path& scene_path, const std::string& object_name, float mass);
bool SetSceneObjectPhysicsFriction(const std::filesystem::path& scene_path, const std::string& object_name, float friction);
bool SetSceneObjectPhysicsRadius(const std::filesystem::path& scene_path, const std::string& object_name, float radius);
bool SetSceneObjectPhysicsHalfExtent(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& half_extent);
bool SetSceneObjectPhysicsLinearDamping(const std::filesystem::path& scene_path, const std::string& object_name, float linear_damping);
bool SetSceneObjectPhysicsAngularDamping(const std::filesystem::path& scene_path, const std::string& object_name, float angular_damping);
std::string SanitizeSceneObjectTag(const std::string& tag);
bool SetSceneObjectTags(const std::filesystem::path& scene_path, const std::string& object_name, const std::vector<std::string>& tags);
bool AddSceneObjectTag(const std::filesystem::path& scene_path, const std::string& object_name, const std::string& tag);
bool RemoveSceneObjectTag(const std::filesystem::path& scene_path, const std::string& object_name, const std::string& tag);
bool AddSceneObjectAttribute(const std::filesystem::path& scene_path, const std::string& object_name, SceneObjectAttributeKind kind);
bool RemoveSceneObjectAttribute(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index);
bool SetSceneObjectAttributeKind(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectAttributeKind kind);
bool SetSceneObjectAttributeColor(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& color);
bool SetSceneObjectAttributeIntensity(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float intensity);
bool SetSceneObjectAttributeRange(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float range);
bool SetSceneObjectAttributeSourceRadius(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float source_radius);
bool SetSceneObjectAttributeHaloIntensity(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float halo_intensity);
bool SetSceneObjectAttributeHaloRadius(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float halo_radius);
bool SetSceneObjectAttributeInnerConeDegrees(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float inner_cone_degrees);
bool SetSceneObjectAttributeOuterConeDegrees(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float outer_cone_degrees);
bool SetSceneObjectAttributeFieldOfView(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float field_of_view_degrees);
bool SetSceneObjectAttributeNearClip(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float near_clip);
bool SetSceneObjectAttributeFarClip(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float far_clip);
bool SetSceneObjectCameraActive(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool active);
bool SetSceneObjectAttributeCameraType(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectCameraType type);
bool SetSceneObjectAttributeCameraFollowTarget(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& follow_target_object);
bool SetSceneObjectAttributeCameraFollowOffset(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneVector3& follow_offset);
bool SetSceneObjectAttributeCameraFollowOrbit(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneVector3& follow_orbit);
bool SetSceneObjectAttributeCameraFollowRotationOffset(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneVector3& follow_rotation_offset);
bool SetSceneObjectAttributeCameraFollowLockPosition(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool follow_lock_position);
bool SetSceneObjectAttributeCameraFollowSmoothing(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float follow_smoothing);
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
bool SetSceneObjectAttributeAnimatorControllerPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& controller_path);
bool SetSceneObjectAttributeAnimatorInitialState(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& initial_state);
bool SetSceneObjectAttributeAnimatorPlaybackSpeed(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float playback_speed);
bool SetSceneObjectAttributeAnimatorAutoPlay(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool auto_play);
bool SetSceneObjectAttributeText2DFontPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& font_path);
bool SetSceneObjectAttributeText2DText(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& text);
bool SetSceneObjectAttributeText2DPosition(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float x, float y);
bool SetSceneObjectAttributeText2DSize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float width, float height);
bool SetSceneObjectAttributeText2DFontSize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float font_size);
bool SetSceneObjectAttributeText2DColor(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& color);
bool SetSceneObjectAttributeText2DAlpha(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float alpha);
bool SetSceneObjectAttributeText2DPriority(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, int priority);
bool SetSceneObjectAttributeText2DLockAspectRatio(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool lock_aspect_ratio);
bool SetSceneObjectAttributeImage2DImagePath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& image_path);
bool SetSceneObjectAttributeImage2DPosition(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float x, float y);
bool SetSceneObjectAttributeImage2DSize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float width, float height);
bool SetSceneObjectAttributeImage2DTint(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& tint);
bool SetSceneObjectAttributeImage2DAlpha(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float alpha);
bool SetSceneObjectAttributeImage2DPriority(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, int priority);
bool SetSceneObjectAttributeImage2DLockAspectRatio(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool lock_aspect_ratio);
bool SetSceneObjectAttributeImage2DStretchToScreen(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool stretch_to_screen);
bool SetSceneObjectAttributeImage2DPlayMode(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectImagePlayMode play_mode);
bool SetSceneObjectAttributeColor2DPosition(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float x, float y);
bool SetSceneObjectAttributeColor2DSize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float width, float height);
bool SetSceneObjectAttributeColor2DColor(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& color);
bool SetSceneObjectAttributeColor2DAlpha(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float alpha);
bool SetSceneObjectAttributeColor2DPriority(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, int priority);
bool SetSceneObjectAttributeColor2DLockAspectRatio(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool lock_aspect_ratio);
bool SetSceneObjectAttributeColor2DStretchToScreen(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool stretch_to_screen);
bool SetSceneObjectAttributeSkyboxImagePath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& image_path);
bool SetSceneObjectAttributeSkyboxRotation(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float rotation_degrees);
bool SetSceneObjectAttributeAudioClipPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& clip_path);
bool SetSceneObjectAttributeAudioPlayMode(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectAudioPlayMode play_mode);
bool SetSceneObjectAttributeAudioVolume(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float volume);
bool SetSceneObjectAttributeAudioPitch(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float pitch);
bool SetSceneObjectAttributeAudioLoop(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool loop);
bool SetSceneObjectAttributeAudioSpatialize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool spatialize_3d);
bool SetSceneObjectAttributeAudioMinDistance(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float min_distance);
bool SetSceneObjectAttributeAudioMaxDistance(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float max_distance);
bool SetSceneObjectAttributeAudioDopplerFactor(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float doppler_factor);
bool SetSceneObjectAttributeVideo2DVideoPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& video_path);
bool SetSceneObjectAttributeVideo2DPosition(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float x, float y);
bool SetSceneObjectAttributeVideo2DSize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float width, float height);
bool SetSceneObjectAttributeVideo2DTint(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& tint);
bool SetSceneObjectAttributeVideo2DAlpha(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float alpha);
bool SetSceneObjectAttributeVideo2DLockAspectRatio(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool lock_aspect_ratio);
bool SetSceneObjectAttributeVideo2DStretchToScreen(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool stretch_to_screen);
bool SetSceneObjectAttributeVideo2DPriority(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, int priority);
bool SetSceneObjectAttributeVideo2DPlayMode(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectVideoPlayMode play_mode);
bool SetSceneObjectAttributeVideo2DVolume(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float volume);
bool SetSceneObjectAttributeVideo2DMuted(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool muted);
bool SetSceneObjectAttributeEffectsPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& effect_path);
bool SetSceneObjectAttributeEffectsPlayMode(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectEffectsPlayMode play_mode);
bool SetSceneObjectAttributeShaderType(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectShaderType type);
bool SetSceneObjectAttributeShape3DPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& shape_path);
bool SetSceneReferenceViewportSize(const std::filesystem::path& scene_path, std::uint32_t width, std::uint32_t height);
bool SetSceneObjectModel(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& model_path);
bool SetSceneObjectModelVisualOffset(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& model_visual_offset);
bool ClearSceneObjectModel(const std::filesystem::path& scene_path, const std::string& object_name);
bool AddSceneObjectScript(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& script_path);
bool RemoveSceneObjectScript(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& script_path);
bool AddSceneObjectGraph(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& graph_path);
bool RemoveSceneObjectGraph(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& graph_path);