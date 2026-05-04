#pragma once

#include "app/VulkanContext.h"
#include "assets/ModelAsset.h"
#include "assets/SceneMetadata.h"
#include "render/Lighting.h"
#include "render/PhysicsWorld.h"
#include "render/Raytracing.h"
#include "render/Scene2DRenderer.h"
#include "render/SkyboxRenderer.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_State;

class RuntimeRenderer
{
public:
    struct RuntimePerformanceStats
    {
        bool valid = false;
        float frame_time_ms = 0.0f;
        float fps = 0.0f;
        float physics_time_ms = 0.0f;
        float scripts_time_ms = 0.0f;
        float render_time_ms = 0.0f;
        float overlay_2d_time_ms = 0.0f;
        float animation_time_ms = 0.0f;
        float audio_time_ms = 0.0f;
        float video_time_ms = 0.0f;
    };

    bool Initialize(VulkanContext* context);
    void Shutdown();
    bool StartSession(
        const std::filesystem::path& project_root,
        const std::filesystem::path& scene_path,
        const ActiveSceneCameraSelection& active_camera,
        std::string* error_message = nullptr);
    bool RenderFrame(std::uint32_t target_width, std::uint32_t target_height, std::string* error_message = nullptr);

    VkImage GetOutputImage() const { return ray_tracing_.GetOutputImage(); }
    VkImageLayout GetOutputLayout() const { return ray_tracing_.GetOutputLayout(); }
    std::uint32_t GetOutputWidth() const { return ray_tracing_.GetOutputWidth(); }
    std::uint32_t GetOutputHeight() const { return ray_tracing_.GetOutputHeight(); }
    const RuntimePerformanceStats& GetPerformanceStats() const { return performance_stats_; }

    struct CachedModelAssetEntry
    {
        std::filesystem::file_time_type write_time{};
        ModelAsset asset{};
    };

    struct GpuMeshSection
    {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material_index = 0;
        bool uses_alpha_transparency = false;
    };

    struct GpuBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        VkDeviceAddress device_address = 0;
    };

    struct GpuTexture
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    struct GpuMaterialTextures
    {
        GpuTexture base_color{};
        GpuTexture metallic_roughness{};
        GpuTexture normal{};
        GpuTexture occlusion{};
        GpuTexture emissive{};
    };

    struct GpuMeshCacheEntry
    {
        std::filesystem::file_time_type write_time{};
        GpuBuffer vertex_buffer{};
        GpuBuffer index_buffer{};
        std::uint32_t vertex_count = 0;
        std::uint32_t index_count = 0;
        std::vector<GpuMeshSection> sections;
        std::vector<RayTracing::MaterialRecord> materials;
        std::vector<GpuMaterialTextures> material_textures;
    };

    struct QueuedSceneObject
    {
        std::filesystem::path model_path;
        std::string name;
        SceneVector3 model_visual_offset = {0.0f, 0.0f, 0.0f};
        std::array<float, 16> model_matrix{};
        std::vector<std::filesystem::path> script_paths;
    };

    struct CachedScriptSourceEntry
    {
        std::filesystem::file_time_type write_time{};
        std::vector<std::uint8_t> source_bytes;
        bool loaded = false;
    };

    struct RuntimeScriptInstance
    {
        std::string instance_key;
        std::string object_name;
        std::filesystem::path script_path;
        int table_ref = -2;
    };

    struct ScriptEventSubscription
    {
        std::string instance_key;
        int handler_ref = -2;
    };

    struct RuntimeSpawnedObject
    {
        std::string name;
        std::string model_path;
        std::string script_path;
        SceneVector3 model_visual_offset = {0.0f, 0.0f, 0.0f};
        SceneVector3 position = {0.0f, 0.0f, 0.0f};
        SceneVector3 rotation = {0.0f, 0.0f, 0.0f};
        SceneVector3 scale = {1.0f, 1.0f, 1.0f};
    };

    struct ScriptTimer
    {
        std::uint64_t id = 0;
        std::string owner_instance_key;
        int callback_ref = -2;
        float remaining_seconds = 0.0f;
        float interval_seconds = 0.0f;
        bool repeating = false;
    };

private:

    const CachedModelAssetEntry& GetModelAssetEntry(const std::filesystem::path& path);
    const SceneMetadata& GetSceneMetadata();
    void ReleaseBuffer(GpuBuffer& buffer);
    void ReleaseTexture(GpuTexture& texture);
    void ReleaseMeshCacheEntry(GpuMeshCacheEntry& entry);
    bool EnsureMeshCacheEntry(const std::filesystem::path& model_path, const CachedModelAssetEntry& model_asset_entry);
    bool EnsureScriptCacheEntry(const std::filesystem::path& script_path, std::string* error_message);
    bool InitializeScriptRuntime(std::string* error_message);
    void ShutdownScriptRuntime();
    void DestroyAllScriptInstances();
    bool LoadScriptInstance(const std::string& object_name, const std::filesystem::path& script_path, std::string* error_message);
    bool SyncScriptInstances(std::string* error_message);
    bool CallScriptMethod(RuntimeScriptInstance& instance, const char* method_name, float delta_time, bool include_delta_time, std::string* error_message);
    bool CallScriptTriggerMethod(RuntimeScriptInstance& instance, const char* method_name, const std::string& other_object_name, const std::string& phase, std::string* error_message);
    bool UpdateScriptsForFrame(std::string* error_message);
    bool UpdateScriptTimers(float delta_time, std::string* error_message);
    void ClearScriptTimers();
    void RemoveScriptTimersForInstance(const std::string& instance_key);
    void ClearScriptEventSubscriptions();
    void RemoveScriptEventSubscriptionsForInstance(const std::string& instance_key);
    bool SpawnRuntimeObject(const RuntimeSpawnedObject& object, std::string* error_message);
    void DestroyRuntimeObject(const std::string& object_name);
    bool RuntimeObjectExists(const std::string& object_name) const;
    void SetScriptObjectPosition(const std::string& object_name, const SceneVector3& position);
    bool TryGetScriptObjectPosition(const std::string& object_name, SceneVector3& position) const;
    void SetScriptObjectRotation(const std::string& object_name, const SceneVector3& rotation);
    bool TryGetScriptObjectRotation(const std::string& object_name, SceneVector3& rotation) const;
    void SetScriptObjectScale(const std::string& object_name, const SceneVector3& scale);
    bool TryGetScriptObjectScale(const std::string& object_name, SceneVector3& scale) const;
    SceneObjectAttribute* FindScriptAttribute(const std::string& object_name, SceneObjectAttributeKind kind, std::size_t occurrence_index = 0);
    const SceneObjectAttribute* FindScriptAttribute(const std::string& object_name, SceneObjectAttributeKind kind, std::size_t occurrence_index = 0) const;
    enum class ScriptAttributeAccessorId
    {
        EnvironmentLightColor = 1,
        EnvironmentLightIntensity,
        DirectionalLightColor,
        DirectionalLightIntensity,
        PointLightColor,
        PointLightIntensity,
        PointLightRange,
        PointLightRadius,
        PointLightHaloIntensity,
        PointLightHaloRadius,
        SpotLightColor,
        SpotLightIntensity,
        SpotLightRange,
        SpotLightInnerCone,
        SpotLightOuterCone,
        CameraFieldOfView,
        CameraNearClip,
        CameraFarClip,
        CameraActive,
        RigidbodyShape,
        RigidbodyDynamic,
        RigidbodyLockRotationX,
        RigidbodyLockRotationY,
        RigidbodyLockRotationZ,
        RigidbodyMass,
        RigidbodyFriction,
        RigidbodyRadius,
        RigidbodyCapsuleHalfHeight,
        RigidbodyHalfExtent,
        RigidbodyLinearDamping,
        RigidbodyAngularDamping,
        TriggerVolumeHalfExtent,
        Text2DFontPath,
        Text2DText,
        Text2DPosition,
        Text2DSize,
        Text2DLockAspectRatio,
        Text2DFontSize,
        Text2DColor,
        Text2DAlpha,
        Text2DPriority,
        Image2DImagePath,
        Image2DPosition,
        Image2DSize,
        Image2DLockAspectRatio,
        Image2DTint,
        Image2DAlpha,
        Image2DPriority,
        SkyboxImagePath,
        SkyboxRotation,
    };
    void RefreshActiveScriptCameraSelection();
    void HandleScriptAttributeMutation(SceneObjectAttributeKind kind, ScriptAttributeAccessorId accessor_id);
    static int LuaLog(lua_State* lua_state);
    static int LuaSetObjectPosition(lua_State* lua_state);
    static int LuaGetObjectPosition(lua_State* lua_state);
    static int LuaSetObjectRotation(lua_State* lua_state);
    static int LuaGetObjectRotation(lua_State* lua_state);
    static int LuaSetObjectScale(lua_State* lua_state);
    static int LuaGetObjectScale(lua_State* lua_state);
    static int LuaAttributeAccessor(lua_State* lua_state);
    static int LuaInputIsKeyDown(lua_State* lua_state);
    static int LuaInputWasKeyPressed(lua_State* lua_state);
    static int LuaInputMousePosition(lua_State* lua_state);
    static int LuaInputMouseDelta(lua_State* lua_state);
    static int LuaWorldSubscribe(lua_State* lua_state);
    static int LuaWorldEmit(lua_State* lua_state);
    static int LuaWorldSpawn(lua_State* lua_state);
    static int LuaWorldSpawnFromObject(lua_State* lua_state);
    static int LuaWorldDestroy(lua_State* lua_state);
    static int LuaWorldDestroyByPrefix(lua_State* lua_state);
    static int LuaWorldExists(lua_State* lua_state);
    static int LuaWorldGetAll(lua_State* lua_state);
    static int LuaWorldFindByPrefix(lua_State* lua_state);
    static int LuaWorldGetCollisions(lua_State* lua_state);
    static int LuaWorldGetCollisionsFor(lua_State* lua_state);
    static int LuaWorldGetCollisionsByPhase(lua_State* lua_state);
    static int LuaWorldSetTimeout(lua_State* lua_state);
    static int LuaWorldSetInterval(lua_State* lua_state);
    static int LuaWorldClearTimer(lua_State* lua_state);
        static int LuaWorldLoadScene(lua_State* lua_state);
    static int LuaPhysicsRaycast(lua_State* lua_state);
    static int LuaPhysicsSetVelocity(lua_State* lua_state);
    static int LuaPhysicsGetVelocity(lua_State* lua_state);
    static int LuaPhysicsAddImpulse(lua_State* lua_state);
    static int LuaPhysicsAddForce(lua_State* lua_state);
    bool BuildQueuedScene(
        const SceneMetadata& scene_metadata,
        const SceneObjectMetadata& active_camera_object,
        const SceneObjectCameraAttributes& active_camera,
        std::array<float, 16>& view_inverse,
        std::array<float, 16>& projection_inverse,
        ResolvedSceneLighting& lighting,
        std::string* error_message);
    bool SyncRayTracingScene(std::string* error_message);

    VulkanContext* vulkan_context_ = nullptr;
    RayTracing ray_tracing_{};
    Scene2DRenderer scene_2d_renderer_{};
    SkyboxRenderer skybox_renderer_{};
    std::filesystem::path project_root_;
    std::filesystem::path scene_path_;
    std::string active_camera_object_name_;
    std::size_t active_camera_attribute_index_ = 0;
    std::filesystem::path cached_scene_path_;
    std::filesystem::file_time_type cached_scene_write_time_{};
    SceneMetadata cached_scene_metadata_{};
    bool has_cached_scene_metadata_ = false;
    std::unordered_map<std::filesystem::path, CachedModelAssetEntry> model_asset_cache_;
    std::unordered_map<std::filesystem::path, GpuMeshCacheEntry> mesh_cache_;
    std::unordered_map<std::filesystem::path, CachedScriptSourceEntry> script_cache_;
    PhysicsWorld physics_world_{};
    bool physics_world_built_ = false;
    lua_State* script_lua_state_ = nullptr;
    std::unordered_map<std::string, RuntimeScriptInstance> script_instances_;
    std::unordered_map<std::string, std::vector<ScriptEventSubscription>> script_event_subscriptions_;
    std::vector<ScriptTimer> script_timers_;
    std::uint64_t script_next_timer_id_ = 1;
    bool script_timer_update_in_progress_ = false;
    std::unordered_set<std::uint64_t> script_timer_pending_clear_;
    std::unordered_map<std::string, RuntimeSpawnedObject> runtime_spawned_objects_;
    std::unordered_set<std::string> runtime_destroyed_objects_;
    std::unordered_map<std::string, PhysicsBodyTransform> physics_object_transforms_;
    std::unordered_map<std::string, SceneVector3> script_object_position_overrides_;
    std::unordered_map<std::string, SceneVector3> script_object_rotation_overrides_;
    std::unordered_map<std::string, SceneVector3> script_object_scale_overrides_;
    std::string script_active_instance_key_;
    std::string script_active_object_name_;
    std::vector<bool> script_prev_keys_down_;
    std::vector<PhysicsCollisionEvent> script_frame_collision_events_;
    std::uint64_t script_last_tick_ms_ = 0;
    std::uint64_t script_session_start_ms_ = 0;
    std::vector<QueuedSceneObject> queued_objects_;
    std::string pending_scene_load_path_;
    RuntimePerformanceStats performance_stats_{};
};