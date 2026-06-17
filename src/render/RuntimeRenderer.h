#pragma once

#include "app/VulkanContext.h"
#include "assets/AnimatorControllerAsset.h"
#include "assets/FaceClipAsset.h"
#include "assets/ModelAsset.h"
#include "assets/SceneMetadata.h"
#include "audio/AudioEngine.h"
#include "input/ControllerMapping.h"
#include "render/Lighting.h"
#include "render/PhysicsWorld.h"
#include "render/Raytracing.h"
#include "render/RuntimeEffectsRenderer.h"
#include "render/Scene2DRenderer.h"
#include "render/VideoPlaybackManager.h"
#include "render/SkyboxRenderer.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
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
        float gpu_time_ms = 0.0f;
        float cpu_time_ms = 0.0f;
    };

    bool Initialize(VulkanContext* context);
    void Shutdown();
    bool StartSession(
        const std::filesystem::path& project_root,
        const std::filesystem::path& scene_path,
        const ActiveSceneCameraSelection& active_camera,
        std::string* error_message = nullptr);
    bool RenderFrame(std::uint32_t target_width, std::uint32_t target_height, std::string* error_message = nullptr);

    // Push editor-tunable TAA debug knobs into the runtime RT subsystem so
    // SettingsPanel sliders take effect during play-mode (mirrors the
    // SceneViewportRenderer wiring used in the editor viewport).
    void SetTAAEnabled(bool enabled) { ray_tracing_.SetTAAEnabled(enabled); }
    void SetTaaDebugSettings(const RayTracing::TaaDebugSettings& settings) { ray_tracing_.SetTaaDebugSettings(settings); }
    // When enabled, LoadGraphInstance writes the transpiled Lua source to
    // <project>/Graphs/Transpiled/<graph_basename>.lua so it appears in the
    // file tree. Disabling removes the directory on the next call.
    void SetTranspiledLuaDumpEnabled(bool enabled) { transpiled_lua_dump_enabled_ = enabled; }

    // Editor->runtime hand-off: pre-populate caches so the first runtime frame
    // doesn't pay the Assimp + scene-text parse cost again. Call AFTER
    // StartSession() (which clears these caches for the incoming scene path).
    void SeedSceneMetadata(const std::filesystem::path& scene_path, SceneMetadata metadata);
    void SeedModelAsset(const std::filesystem::path& absolute_model_path,
                        std::filesystem::file_time_type write_time,
                        ModelAsset asset);
    // Hand pre-read video file bytes (keyed by the scene-relative
    // video_path) to the video subsystem so the first Update() on the
    // main thread skips a synchronous pak read.
    void SeedVideoBytes(const std::string& video_path, std::vector<std::uint8_t> bytes);
    // Hand pre-read audio clip bytes (keyed by clip_path) to the audio
    // engine so PlaySound at runtime avoids any disk I/O.
    void SeedAudioClipBytes(const std::string& clip_path, std::vector<std::uint8_t> bytes);
    AudioEngine& GetAudioEngine() { return audio_engine_; }

    // --- Sequencer (timeline scripts/graphs) -----------------------------
    // Registers a timeline clip's script (.lua) or graph (.graph). On the next
    // RenderFrame it is loaded as a runtime script instance so its normal
    // lifecycle runs unchanged (OnCreate/OnStart at load, OnUpdate every frame).
    // instance_id makes the instance unique per clip. Idempotent. The instance
    // is exempt from per-frame scene-object pruning until StartSession() or
    // ClearSequencerInstances(). This does NOT call OnCue.
    void RegisterSequencerClip(const std::string& instance_id, const std::filesystem::path& asset_path);
    // Queues a one-shot OnCue() call on the clip's instance — the timeline cue,
    // fired when the playhead crosses the clip. Runs on the next RenderFrame
    // (after any pending registration load), so a just-added clip still cues.
    void FireSequencerCue(const std::string& instance_id);
    // Destroys all sequencer-spawned instances (runs their OnDestroy) and drops
    // any pending loads/cues. Used when the timeline is rewound.
    void ClearSequencerInstances();
    // When true (the editor Sequencer panel's preview), the runtime does NOT
    // drive the timeline itself — the panel advances the clock and feeds cues
    // via FireSequencerCue. When false (the Play window and the built game),
    // StartSession loads the scene's "<scene>.seq" and auto-plays it: the
    // runtime advances its own clock and fires OnCue as it crosses each clip.
    void SetSequencerExternallyDriven(bool driven);

    VkImage GetOutputImage() const { return ray_tracing_.GetOutputImage(); }
    VkImageLayout GetOutputLayout() const { return ray_tracing_.GetOutputLayout(); }
    // ImGui-sampleable descriptor over the output image (valid after the first
    // RenderFrame leaves it in SHADER_READ_ONLY_OPTIMAL). Used to embed the
    // runtime view inside an editor panel via ImGui::Image / AddImage.
    VkDescriptorSet GetOutputDescriptorSet() const { return ray_tracing_.GetOutputDescriptorSet(); }
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
        GpuTexture transmission{};
        GpuTexture specular{};
        GpuTexture specular_color{};
        GpuTexture sheen_color{};
        GpuTexture sheen_roughness{};
        GpuTexture iridescence{};
        GpuTexture iridescence_thickness{};
        GpuTexture volume_thickness{};
        GpuTexture clearcoat{};
        GpuTexture clearcoat_roughness{};
        GpuTexture clearcoat_normal{};
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

    // GPU compute-skinning resources for a single animated mesh.
    // Owned per model_path; rebuilt when the source clip-model or vertex layout
    // changes. Output is the mesh's existing vertex_buffer (also the BLAS input).
    struct GpuSkinningResources
    {
        std::filesystem::path source_clip_model_path; // anim cache entry source
        std::filesystem::file_time_type source_clip_write_time{};
        std::uint32_t vertex_count = 0;
        std::uint32_t bone_count = 0;
        GpuBuffer bind_pose_buffer{};   // SceneGpuVertex layout, uploaded once
        GpuBuffer influence_buffer{};   // (uvec4 + vec4) per vertex, uploaded once
        GpuBuffer palette_buffer{};     // mat4 * bone_count, host-coherent, written every frame
        // per-vertex previous-frame skinned object-space positions
        // (3 floats per vertex). Written by the skinning compute pass at the
        // start of each frame (it copies the *previous* frame's output_vertices
        // position into here before overwriting it with the new skinned
        // position). Surfaced to the RT closest-hit via `MeshRecord` so the
        // primary motion-vector pass can compute `pos_obj_prev` for skinned
        // meshes.
        GpuBuffer prev_position_buffer{};
        // ---- Morph-target (blendshape) resources -------------------------
        // Sparse per-vertex deltas (binding 5, device-local, uploaded once),
        // model-global weights (binding 6, host-coherent, written per frame),
        // and per-vertex (delta_base, count) descriptors (binding 7,
        // device-local, uploaded once). All three are always created -- when
        // the model has no blendshapes they are 1-element dummies and
        // morph_target_count is 0, so the shader's morph loop is skipped.
        GpuBuffer morph_delta_buffer{};
        GpuBuffer morph_weight_buffer{};
        GpuBuffer morph_descriptor_buffer{};
        std::uint32_t morph_target_count = 0;
        // Model-global ordered ARKit target names; index N == weight slot N in
        // morph_weight_buffer. Drives the per-frame weight upload.
        std::vector<std::string> morph_target_names;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        bool ready = false;
    };

    struct QueuedSceneObject
    {
        std::filesystem::path model_path;
        std::string name;
        SceneVector3 model_visual_offset = {0.0f, 0.0f, 0.0f};
        std::array<float, 16> model_matrix{};
        std::vector<std::filesystem::path> script_paths;
        std::vector<std::filesystem::path> graph_paths;
        bool is_water_surface = false;
        bool is_cloud = false;
        bool is_fire = false;
        bool is_rain = false;
        bool is_puddle = false;
        float puddle_drop_scale = 1.0f;
        float puddle_drop_speed = 1.0f;
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
        std::vector<std::string> tags;
        // Optional: full attribute block carried over from a prefab spawn.
        // Empty for World.Spawn / World.SpawnFromObject. Used by the runtime
        // queue loop to drive attribute-based rendering paths (procedural
        // shaders like Cloud/Water, etc.).
        std::vector<SceneObjectAttribute> attributes;
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

    struct CachedAnimatorControllerEntry
    {
        std::filesystem::file_time_type write_time{};
        AnimatorControllerAsset asset{};
        bool loaded = false;
        // Perf-counter ticks at which the next on-disk timestamp check is allowed.
        // Throttles std::filesystem::last_write_time to avoid per-frame syscalls
        // (Windows AV / file indexer can stall these, producing visible stutter).
        std::uint64_t next_disk_check_perf_ticks = 0;
    };

    struct JiggleSimEntry
    {
        std::string bone_name;
        std::array<float, 3> sim_pos{};
        std::array<float, 3> sim_vel{};
        bool initialized = false;
    };

    // Per-bone persistent state for Rigidbody bone colliders: the resolved
    // (non-penetrating) world-space box center from the previous frame. The
    // sweep from prev -> animated target is what stops the bone at walls; a
    // single-frame depenetration cannot, because mesh colliders are hollow
    // and a box fully inside the wall volume overlaps no triangles.
    // applied_offset is the smoothed world-space correction currently baked
    // into the skeleton (IK target = animated pose + offset); smoothing it
    // removes pops when contact starts and ends.
    struct BoneSweepEntry
    {
        std::string bone_name;
        std::array<float, 3> prev_center{};
        std::array<float, 3> applied_offset{};
        bool initialized = false;
    };

    struct RuntimeAnimatorState
    {
        std::string runtime_key;
        std::string controller_path;
        std::string active_state;
        std::string active_clip_name;
        std::string active_clip_source_model_path;
        std::string previous_state;
        std::string previous_clip_name;
        std::string previous_clip_source_model_path;
        float state_time_seconds = 0.0f;
        float previous_state_time_seconds = 0.0f;
        float previous_state_playback_speed = 1.0f;
        float blend_duration_seconds = 0.0f;
        float blend_time_remaining_seconds = 0.0f;
        bool previous_pose_snapshot_valid = false;
        std::filesystem::file_time_type controller_write_time = std::filesystem::file_time_type::min();
        std::unordered_map<std::string, float> float_parameters;
        std::unordered_map<std::string, bool> bool_parameters;
        std::unordered_set<std::string> triggers;
        // Bone-physics per-instance simulation state. Preserved across frames
        // so springs don't reset when the user nudges parameters.
        std::vector<JiggleSimEntry> jiggle_states;
        float physics_accumulator_seconds = 0.0f;
        // Object world origin at the previous jiggle update. The jiggle
        // springs carry their state along with the object's frame-to-frame
        // rigid translation so uniform motion cannot excite them through
        // the quantized-substep clock (see SampleClipBoneMatricesWithPhysics).
        std::array<float, 3> jiggle_prev_object_origin = {0.0f, 0.0f, 0.0f};
        bool jiggle_prev_object_origin_valid = false;
        // Rigidbody bone-collider sweep state (prev resolved box centers).
        std::vector<BoneSweepEntry> bone_sweep_states;

        // ---- Face (blendshape) layer state -------------------------------
        // Facial expression target (set via Animator.SetFacePose): the pose to
        // blend toward ("" = the controller's default_pose), its intensity
        // (0..1), and the ease speed (0 = snap, >0 = units/second). The current
        // per-shape weights are eased toward the target each frame so poses
        // cross-fade and can be held at partial strength.
        std::string face_pose_target;
        float face_pose_target_weight = 1.0f;
        float face_pose_blend_speed = 0.0f;
        std::unordered_map<std::string, float> face_pose_current;

        // Gaze / look-at (set via Animator.SetEyeTarget / LookAt / ClearEyeTarget).
        // The eyes drive the ARKit eyeLook* shapes toward a world point or, when
        // gaze_target_object is set, that object's position (resolved each
        // frame). gaze_cur_h/v are the smoothed horizontal/vertical deflections
        // (-1..1) so the eyes ease rather than snap, and ease back to center
        // when gaze is cleared.
        bool gaze_active = false;
        std::array<float, 3> gaze_target_point = {0.0f, 0.0f, 0.0f};
        std::string gaze_target_object;
        float gaze_cur_h = 0.0f;
        float gaze_cur_v = 0.0f;
        // Lip-sync playback (driven by Animator.PlayLipSync). lipsync_clip_path
        // is the project-relative .faceclip currently playing, or empty.
        std::string lipsync_clip_path;
        float lipsync_time_seconds = 0.0f;
        bool lipsync_playing = false;
        bool lipsync_loop = false;
        // Handle of the source-audio instance PlayLipSync started; the lip-sync
        // curve time is driven from this sound's playback cursor so audio and
        // mouth stay locked. kInvalidHandle when no audio is playing.
        AudioEngine::SoundHandle lipsync_sound = AudioEngine::kInvalidHandle;
    };

private:

    const CachedModelAssetEntry& GetModelAssetEntry(const std::filesystem::path& path);
    const SceneMetadata& GetSceneMetadata();
    // Tears down the current scene's runtime state (script instances,
    // subscriptions, timers, overrides, queued objects, physics, timing) and
    // switches scene_path_ / active camera to the supplied scene. Shared by the
    // instant loading-scene switch and the final target swap in async loads.
    void SwapToScene(const std::filesystem::path& scene_file,
                     const SceneMetadata& scene_metadata,
                     const ActiveSceneCameraSelection& camera);
    // Begins an async load for a pending World.LoadScene request: validates the
    // target cheaply (metadata + camera) on the main thread, dispatches a single
    // background worker to parse models + read audio/video bytes, and switches to
    // the loading scene (if any) immediately. No-op when no request is pending or
    // a load is already in progress. On a validation failure it logs and keeps
    // the current scene untouched.
    void BeginAsyncSceneLoad();
    // Polls an in-flight async load; when the worker is done it seeds the parsed
    // assets into the runtime caches and begins the GPU warm-up phase.
    void PollAsyncSceneLoad();
    // During the warm-up phase, uploads a bounded number of the target scene's
    // models to the GPU each frame (so the loading screen keeps rendering), then
    // swaps to the target once every model is resident.
    void WarmTargetScene();
    void ReleaseBuffer(GpuBuffer& buffer);
    void ReleaseTexture(GpuTexture& texture);
    void ReleaseMeshCacheEntry(GpuMeshCacheEntry& entry);
    bool EnsureMeshCacheEntry(const std::filesystem::path& model_path, const CachedModelAssetEntry& model_asset_entry);
    // GPU compute-skinning pipeline lifecycle.
    bool EnsureSkinningPipeline();
    void DestroySkinningPipeline();
    void ReleaseSkinningResources(GpuSkinningResources& resources);
    bool EnsureScriptCacheEntry(const std::filesystem::path& script_path, std::string* error_message);
    bool InitializeScriptRuntime(std::string* error_message);
    void ShutdownScriptRuntime();
    void DestroyAllScriptInstances();
    bool LoadScriptInstance(const std::string& object_name, const std::filesystem::path& script_path, std::string* error_message);
    // Transpiles a .graph document to Lua in-memory and loads it via the
    // same path as LoadScriptInstance. The graph path is used as the
    // instance-key suffix so multiple graphs can coexist on one object.
    bool LoadGraphInstance(const std::string& object_name, const std::filesystem::path& graph_path, std::string* error_message);
    bool SyncScriptInstances(std::string* error_message);
    // Drains pending sequencer loads (into live instances) then pending cues
    // (OnCue calls). Called at the top of UpdateScriptsForFrame so a newly
    // loaded clip and its cue resolve before the same frame's OnUpdate pass.
    bool ProcessSequencerQueue(std::string* error_message);
    // Runtime-owned timeline (used when not externally driven). LoadAutoSequence
    // reads the scene's .seq and registers its clips; AdvanceAutoSequence ticks
    // the clock each frame and fires OnCue as it crosses each clip's start.
    void LoadAutoSequence();
    void AdvanceAutoSequence();
    bool CallScriptMethod(RuntimeScriptInstance& instance, const char* method_name, float delta_time, bool include_delta_time, std::string* error_message);
    bool CallScriptTriggerMethod(RuntimeScriptInstance& instance, const char* method_name, const std::string& other_object_name, const std::string& phase, std::string* error_message, const std::string& bone_name = {});
    bool UpdateScriptsForFrame(std::string* error_message);
    bool UpdateScriptTimers(float delta_time, std::string* error_message);
    void UpdateAnimatorControllersForFrame(const SceneMetadata& scene_metadata);
    void UpdateAudioSourcesForFrame(const SceneMetadata& scene_metadata, const std::array<float, 16>& camera_world_matrix);
    bool UpdateAnimatedMeshForObject(const QueuedSceneObject& object);
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
    bool SetScriptObjectEnabled(const std::string& object_name, bool enabled);
    bool TryGetScriptObjectEnabled(const std::string& object_name, bool& enabled) const;
    bool SetScriptCameraActive(const std::string& object_name, bool active);
    RuntimeAnimatorState* FindRuntimeAnimatorState(const std::string& object_name, std::size_t occurrence_index = 0);
    const RuntimeAnimatorState* FindRuntimeAnimatorState(const std::string& object_name, std::size_t occurrence_index = 0) const;
    RuntimeAnimatorState* EnsureRuntimeAnimatorState(const std::string& object_name, std::size_t occurrence_index = 0);
    bool SetRuntimeAnimatorParameter(const std::string& object_name, const std::string& parameter_name, float value, std::size_t occurrence_index = 0);
    bool SetRuntimeAnimatorBoolParameter(const std::string& object_name, const std::string& parameter_name, bool value, std::size_t occurrence_index = 0);
    bool TryGetRuntimeAnimatorParameter(const std::string& object_name, const std::string& parameter_name, float& out_value, bool& out_is_bool, std::size_t occurrence_index = 0) const;
    bool SetRuntimeAnimatorTrigger(const std::string& object_name, const std::string& trigger_name, std::size_t occurrence_index = 0);
    bool SetRuntimeAnimatorState(const std::string& object_name, const std::string& state_name, std::size_t occurrence_index = 0);
    // Blends the face toward an expression pose. pose_name "" reverts to the
    // controller's default pose; weight (0..1) scales the pose intensity;
    // blend_speed eases the transition (0 = snap, >0 = units/second). Returns
    // false if the object has no animator.
    bool SetRuntimeAnimatorFacePose(const std::string& object_name, const std::string& pose_name, float weight, float blend_speed, std::size_t occurrence_index = 0);
    // Starts lip-sync playback of a baked clip. clip_name matches a clip on the
    // controller by file stem or project-relative path. Returns false if the
    // object has no animator or the clip can't be resolved.
    bool PlayRuntimeAnimatorLipSync(const std::string& object_name, const std::string& clip_name, bool loop, std::size_t occurrence_index = 0);
    bool StopRuntimeAnimatorLipSync(const std::string& object_name, std::size_t occurrence_index = 0);
    bool IsRuntimeAnimatorLipSyncPlaying(const std::string& object_name, std::size_t occurrence_index = 0) const;
    // Gaze: make the eyes track a world point, or another object's position,
    // until cleared. Returns false if the object has no animator.
    bool SetRuntimeAnimatorEyeTarget(const std::string& object_name, float x, float y, float z, std::size_t occurrence_index = 0);
    bool LookAtRuntimeAnimator(const std::string& object_name, const std::string& target_object, std::size_t occurrence_index = 0);
    bool ClearRuntimeAnimatorEyeTarget(const std::string& object_name, std::size_t occurrence_index = 0);
    // Loads (and caches) a baked FaceClip by project-relative path. Returns an
    // empty clip (duration 0) on failure; cached either way to avoid retries.
    const FaceClipAsset& GetOrLoadFaceClip(const std::string& clip_path);
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
        Image2DStretchToScreen,
        Image2DPlayMode,
        Image2DTint,
        Image2DAlpha,
        Image2DPriority,
        Color2DPosition,
        Color2DSize,
        Color2DLockAspectRatio,
        Color2DStretchToScreen,
        Color2DColor,
        Color2DAlpha,
        Color2DPriority,
        SkyboxImagePath,
        SkyboxRotation,
        AnimatorControllerPath,
        AnimatorInitialState,
        AnimatorPlaybackSpeed,
        AnimatorAutoPlay,
        AnimatorActiveState,
        AnimatorStateTime,
        AnimatorSetBool,
        AnimatorGetBool,
        AnimatorSetTrigger,
        AnimatorSetState,
        AnimatorGetState,
        AnimatorSetDefaultState,
        AnimatorGetDefaultState,
        AnimatorSetFacePose,
        AnimatorPlayLipSync,
        AnimatorStopLipSync,
        AnimatorIsLipSyncPlaying,
        AnimatorSetEyeTarget,
        AnimatorLookAt,
        AnimatorClearEyeTarget,
        AudioClipPath,
        AudioPlayMode,
        AudioVolume,
        AudioLoop,
        AudioSpatialize3D,
        AudioPitch,
        AudioMinDistance,
        AudioMaxDistance,
        AudioDopplerFactor,
        Video2DVideoPath,
        Video2DPosition,
        Video2DSize,
        Video2DLockAspectRatio,
        Video2DStretchToScreen,
        Video2DTint,
        Video2DAlpha,
        Video2DPriority,
        Video2DPlayMode,
        Video2DVolume,
        Video2DMuted,
        EffectsEffectPath,
        EffectsPlayMode,
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
    static int LuaSetObjectEnabled(lua_State* lua_state);
    static int LuaGetObjectEnabled(lua_State* lua_state);
    static int LuaSetCameraActive(lua_State* lua_state);
    static int LuaAttributeAccessor(lua_State* lua_state);
    static int LuaInputIsKeyDown(lua_State* lua_state);
    static int LuaInputWasKeyPressed(lua_State* lua_state);
    static int LuaInputMousePosition(lua_State* lua_state);
    static int LuaInputMouseDelta(lua_State* lua_state);
    // Controller mapping: load the project's key->controller bindings, manage
    // open gamepads, and fold controller state into keyboard-key queries so
    // existing Input.IsKeyDown/WasKeyPressed calls respond to the controller.
    void LoadControllerMappings();
    void RefreshGamepads();
    void CloseGamepads();
    bool IsControllerBindingActive(const input::ControllerBinding& binding) const;
    bool EffectiveKeyDown(SDL_Scancode scancode) const;
    // Analog deflection [0,1] of a binding across open gamepads (buttons read as
    // 0/1); used to drive mouse-look from a controller stick.
    float ControllerBindingMagnitude(const input::ControllerBinding& binding) const;
    // Adds the controller's mouse-move contribution to a mouse delta.
    void AddControllerMouseDelta(float& dx, float& dy) const;
    static int LuaWorldSubscribe(lua_State* lua_state);
    static int LuaWorldEmit(lua_State* lua_state);
    static int LuaWorldSpawn(lua_State* lua_state);
    static int LuaWorldSpawnFromObject(lua_State* lua_state);
    static int LuaWorldSpawnPrefab(lua_State* lua_state);
    static int LuaWorldDestroy(lua_State* lua_state);
    static int LuaWorldDestroyByPrefix(lua_State* lua_state);
    static int LuaWorldExists(lua_State* lua_state);
    static int LuaWorldGetAll(lua_State* lua_state);
    static int LuaWorldFindByPrefix(lua_State* lua_state);
    static int LuaWorldFindByTag(lua_State* lua_state);
    static int LuaGetObjectTags(lua_State* lua_state);
    static int LuaObjectHasTag(lua_State* lua_state);
    static int LuaAddObjectTag(lua_State* lua_state);
    static int LuaRemoveObjectTag(lua_State* lua_state);
    static int LuaWorldGetCollisions(lua_State* lua_state);
    static int LuaWorldGetCollisionsFor(lua_State* lua_state);
    static int LuaWorldGetCollisionsByPhase(lua_State* lua_state);
    static int LuaWorldSetTimeout(lua_State* lua_state);
    static int LuaWorldSetInterval(lua_State* lua_state);
    static int LuaWorldClearTimer(lua_State* lua_state);
        static int LuaWorldLoadScene(lua_State* lua_state);
    static int LuaWorldGetSceneLoadProgress(lua_State* lua_state);
    static int LuaWorldIsSceneLoading(lua_State* lua_state);
    static int LuaPhysicsRaycast(lua_State* lua_state);
    static int LuaPhysicsSetVelocity(lua_State* lua_state);
    static int LuaPhysicsGetVelocity(lua_State* lua_state);
    static int LuaPhysicsAddImpulse(lua_State* lua_state);
    static int LuaPhysicsAddForce(lua_State* lua_state);
    static int LuaAudioPlay(lua_State* lua_state);
    static int LuaAudioStop(lua_State* lua_state);
    static int LuaAudioIsPlaying(lua_State* lua_state);
    static int LuaAudioSetVolume(lua_State* lua_state);
    static int LuaAudioSetPitch(lua_State* lua_state);
    static int LuaAudioSetLoop(lua_State* lua_state);
    static int LuaVideoPlay(lua_State* lua_state);
    static int LuaVideoStop(lua_State* lua_state);
    static int LuaVideoIsPlaying(lua_State* lua_state);
    static int LuaVideoSetVolume(lua_State* lua_state);
    static int LuaVideoSetMuted(lua_State* lua_state);
    static int LuaEffectPlay(lua_State* lua_state);
    static int LuaEffectStop(lua_State* lua_state);
    static int LuaEffectIsPlaying(lua_State* lua_state);
    bool BuildQueuedScene(
        const SceneMetadata& scene_metadata,
        const SceneObjectMetadata& active_camera_object,
        const SceneObjectCameraAttributes& active_camera,
        std::array<float, 16>& view_inverse,
        std::array<float, 16>& projection_inverse,
        ResolvedSceneLighting& lighting,
        std::string* error_message);
    std::vector<RuntimeEffectsRenderer::QueuedEffect> BuildQueuedEffects(const SceneMetadata& scene_metadata) const;
    bool SyncRayTracingScene(std::string* error_message, float* out_skinning_ms = nullptr);

    VulkanContext* vulkan_context_ = nullptr;
    RayTracing ray_tracing_{};
    RuntimeEffectsRenderer effects_renderer_{};
    Scene2DRenderer scene_2d_renderer_{};
    VideoPlaybackManager video_playback_manager_{};
    SkyboxRenderer skybox_renderer_{};
    std::filesystem::path project_root_;
    std::filesystem::path scene_path_;
    std::string active_camera_object_name_;
    std::size_t active_camera_attribute_index_ = 0;
    bool transpiled_lua_dump_enabled_ = false;
    std::filesystem::path cached_scene_path_;
    std::filesystem::file_time_type cached_scene_write_time_{};
    SceneMetadata cached_scene_metadata_{};
    bool has_cached_scene_metadata_ = false;
    std::unordered_map<std::filesystem::path, CachedModelAssetEntry> model_asset_cache_;
    std::unordered_map<std::filesystem::path, std::uint64_t> animated_mesh_revisions_;
    std::unordered_map<std::filesystem::path, CachedAnimatorControllerEntry> animator_controller_cache_;
    // Phase C — GPU compute skinning.
    // One pipeline + descriptor-set layout for all animated meshes; per-object
    // resources (bind-pose VB, influence SSBO, palette SSBO, descriptor set)
    // live on `gpu_skinning_resources_` keyed by model path.
    VkDescriptorSetLayout skinning_descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout skinning_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline skinning_pipeline_ = VK_NULL_HANDLE;
    std::unordered_map<std::filesystem::path, GpuSkinningResources> gpu_skinning_resources_;
    std::unordered_map<std::string, RuntimeAnimatorState> runtime_animator_states_;
    // Baked lip-sync FaceClips loaded for runtime playback, keyed by their
    // project-relative path. Populated lazily by GetOrLoadFaceClip.
    std::unordered_map<std::string, FaceClipAsset> face_clip_cache_;
    std::unordered_map<std::filesystem::path, GpuMeshCacheEntry> mesh_cache_;
    std::unordered_map<std::filesystem::path, CachedScriptSourceEntry> script_cache_;
    PhysicsWorld physics_world_{};
    bool physics_world_built_ = false;

    // ---- Animated bone colliders ----------------------------------------
    // Rebuilt each frame from the animation pass (UpdateAnimatedMeshForObject)
    // and consumed by the physics block the following frame: defs feed
    // SetBoneColliders when the set changes; targets drive their kinematic
    // transforms before each Step. The signature detects set changes so we
    // only recreate bodies when colliders are added/removed/resized.
    std::vector<PhysicsWorld::BoneColliderDef> bone_collider_defs_;
    std::unordered_map<std::string, PhysicsBodyTransform> bone_collider_targets_;
    std::string bone_collider_signature_;
    AudioEngine audio_engine_{};
    bool audio_engine_ready_ = false;

    struct ActiveAudioSource
    {
        AudioEngine::SoundHandle handle = AudioEngine::kInvalidHandle;
        SceneObjectAudioPlayMode last_play_mode = SceneObjectAudioPlayMode::Off;
        std::string clip_path;
    };
    // Keyed by "<object_name>#<attribute_index>".
    std::unordered_map<std::string, ActiveAudioSource> active_audio_sources_;

    lua_State* script_lua_state_ = nullptr;
    std::unordered_map<std::string, RuntimeScriptInstance> script_instances_;
    std::unordered_map<std::string, std::vector<ScriptEventSubscription>> script_event_subscriptions_;
    std::vector<ScriptTimer> script_timers_;
    std::uint64_t script_next_timer_id_ = 1;
    bool script_timer_update_in_progress_ = false;
    std::unordered_set<std::uint64_t> script_timer_pending_clear_;
    std::unordered_map<std::string, RuntimeSpawnedObject> runtime_spawned_objects_;
    std::unordered_set<std::string> runtime_destroyed_objects_;
    // Sequencer timeline clips. Registered clips are loaded into live script
    // instances (pending_sequencer_loads_), then keep their keys in
    // sequencer_instance_keys_ so SyncScriptInstances doesn't prune them.
    // sequencer_clip_keys_ maps a clip's instance_id to its instance key so a
    // cue can find the instance to call OnCue on.
    struct PendingSequencerLoad
    {
        std::string instance_id;
        std::filesystem::path asset_path;
    };
    std::vector<PendingSequencerLoad> pending_sequencer_loads_;
    std::vector<std::string> pending_sequencer_cues_;
    std::unordered_map<std::string, std::string> sequencer_clip_keys_;
    std::unordered_set<std::string> sequencer_instance_keys_;
    // Runtime-owned timeline. Populated from the scene's .seq by
    // LoadAutoSequence when sequencer_externally_driven_ is false.
    struct AutoSequenceClip
    {
        std::string id;
        std::filesystem::path asset_path;
        float start_time = 0.0f;
        bool fired = false;
    };
    bool sequencer_externally_driven_ = false;
    std::vector<AutoSequenceClip> auto_sequence_clips_;
    float auto_sequence_time_ = 0.0f;
    bool auto_sequence_active_ = false;
    std::uint64_t auto_sequence_last_tick_ms_ = 0;
    std::unordered_map<std::string, PhysicsBodyTransform> physics_object_transforms_;
    // Previous and current physics simulation snapshots used to render at a
    // smooth (interpolated) pose even when the variable per-frame dt would
    // otherwise produce jittery motion for parented children (e.g. a camera
    // attached to a velocity-driven player). Updated at a fixed timestep via
    // an accumulator; the resulting alpha is used to lerp into
    // physics_object_transforms_.
    std::unordered_map<std::string, PhysicsBodyTransform> physics_object_transforms_prev_;
    std::unordered_map<std::string, PhysicsBodyTransform> physics_object_transforms_curr_;
    float physics_accumulator_seconds_ = 0.0f;
    std::uint64_t physics_last_tick_counter_ = 0;
    bool physics_has_curr_snapshot_ = false;
    // Smoothed follow-camera world position. Tracked across frames so we can
    // ease toward the raw (target + offset) position when follow_smoothing > 0.
    // Reset whenever the active follow camera or its target changes.
    std::array<float, 3> follow_camera_smoothed_position_ = {0.0f, 0.0f, 0.0f};
    // Smoothed forward/up vectors. Used so Lock Position cameras (where the
    // position is fixed and only the look-at rotation changes) still benefit
    // from follow_smoothing.
    std::array<float, 3> follow_camera_smoothed_forward_ = {0.0f, 0.0f, -1.0f};
    std::array<float, 3> follow_camera_smoothed_up_ = {0.0f, 1.0f, 0.0f};
    bool follow_camera_smoothed_position_valid_ = false;
    std::string follow_camera_smoothed_key_;
    std::uint64_t follow_camera_last_tick_counter_ = 0;
    // Previous frame's RAW (unsmoothed) follow target position and the
    // per-frame target velocity estimate derived from it. Used by the
    // ramp-exact smoothing update: a plain "ease toward the current
    // sample" filter has a steady-state lag of v*tau with a first-order
    // dependence on frame dt, so normal frame-time jitter modulates the
    // lag by ~v*ddt and the camera visibly vibrates against a smoothly
    // moving player. The closed-form update for a linearly moving target
    // makes the lag exactly v*tau independent of the dt sequence.
    std::array<float, 3> follow_camera_raw_target_prev_ = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> follow_camera_target_velocity_ = {0.0f, 0.0f, 0.0f};
    // Track-camera runtime motion state. track_camera_distance_ is the arc
    // length traveled along the spline; track_camera_speed_ ramps up to the
    // camera's cruise speed via its acceleration. Reset (via an empty key)
    // whenever the active Track camera changes so it restarts from point 0.
    float track_camera_distance_ = 0.0f;
    float track_camera_speed_ = 0.0f;
    std::string track_camera_key_;
    std::uint64_t track_camera_last_tick_counter_ = 0;
    // Last frame's resolved world matrices for the active follow target.
    // Used to build the camera's `prev_view_projection` from
    // (target_prev_position + current offset), guaranteeing the followed
    // object's motion vector is zero regardless of physics interpolation
    // timing. Keyed by target object name; we only need the active target
    // so we keep a single (key, matrix) pair rather than a full map.
    std::string follow_camera_prev_target_key_;
    std::array<float, 16> follow_camera_prev_target_world_matrix_ = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    bool follow_camera_prev_target_valid_ = false;    std::unordered_map<std::string, SceneVector3> script_object_position_overrides_;
    std::unordered_map<std::string, SceneVector3> script_object_rotation_overrides_;
    std::unordered_map<std::string, SceneVector3> script_object_scale_overrides_;
    std::string script_active_instance_key_;
    std::string script_active_object_name_;
    std::vector<bool> script_prev_keys_down_;
    // Controller bindings loaded from the project (scancode -> inputs) and the
    // gamepads currently opened for the play session.
    input::BindingMap controller_mappings_;
    std::vector<SDL_Gamepad*> open_gamepads_;
    std::vector<PhysicsCollisionEvent> script_frame_collision_events_;
    std::uint64_t animation_last_tick_ms_ = 0;
    std::uint64_t animation_last_perf_ticks_ = 0;
    // Most-recent animation frame delta in seconds, exposed for downstream
    // per-object work (e.g. bone-physics simulation in UpdateAnimatedMeshForObject).
    float animation_last_delta_time_seconds_ = 0.0f;
    std::uint64_t video_last_perf_ticks_ = 0;
    std::uint64_t script_last_tick_ms_ = 0;
    std::uint64_t script_session_start_ms_ = 0;
    std::vector<QueuedSceneObject> queued_objects_;

    // --- World.LoadScene async streaming --------------------------------
    // A model parsed off the main thread, ready to seed into model_asset_cache_.
    struct PreloadedSceneModel
    {
        std::filesystem::path path;
        std::filesystem::file_time_type write_time = std::filesystem::file_time_type::min();
        ModelAsset asset;
    };
    // Result of the background asset worker for one async scene load.
    struct PreloadedSceneAssets
    {
        std::vector<PreloadedSceneModel> models;
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> audio_bytes;
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> video_bytes;
    };
    // Set by World.LoadScene; consumed by BeginAsyncSceneLoad on the next frame.
    std::string pending_scene_load_path_;
    std::string pending_scene_load_loading_scene_;
    // In-flight async load state.
    bool scene_load_in_progress_ = false;
    std::future<PreloadedSceneAssets> pending_async_scene_load_;
    std::filesystem::path async_load_target_path_;
    SceneMetadata async_load_target_metadata_{};
    ActiveSceneCameraSelection async_load_target_camera_{};
    // Resolved model cache keys for the target, used to warm the GPU mesh cache
    // incrementally (a few per frame) before swapping so the loading screen never
    // freezes on a one-frame upload of every model.
    std::vector<std::filesystem::path> async_load_model_keys_;
    bool scene_load_warming_ = false;
    std::size_t async_load_warm_index_ = 0;
    // Progress counters updated by the worker thread; read on the main thread to
    // expose World.GetSceneLoadProgress().
    std::shared_ptr<std::atomic<int>> async_load_models_done_;
    int async_load_models_total_ = 0;
    float scene_load_progress_ = 1.0f;

    RuntimePerformanceStats performance_stats_{};
    bool first_frame_logged_ = false;
};