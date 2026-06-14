#pragma once

// Self-contained preview viewport for the Animator panel.
//
// Loads a model with Assimp on demand, samples animations on the CPU, and
// draws the result (skinned vertex point cloud + skeleton bone segments)
// directly with ImDrawList. Intentionally avoids the ray-traced viewport
// pipeline so the preview is independent of scene/camera plumbing and can
// animate every frame without rebuilding GPU acceleration structures.

#include "imgui.h"

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

class VulkanContext;

namespace Assimp
{
class Importer;
}

struct aiAnimation;
struct aiNode;
struct aiNodeAnim;
struct aiScene;

class AnimatorPreviewRenderer
{
public:
    AnimatorPreviewRenderer();
    ~AnimatorPreviewRenderer();

    AnimatorPreviewRenderer(const AnimatorPreviewRenderer&) = delete;
    AnimatorPreviewRenderer& operator=(const AnimatorPreviewRenderer&) = delete;

    // Loads (or reloads if path changed) the preview model. Returns true on success.
    bool SetModel(const std::filesystem::path& absolute_model_path);
    void ClearModel();

    bool HasModel() const { return loaded_ && scene_ != nullptr; }
    // True while a model is being parsed on the background worker thread.
    // Callers can show a placeholder until HasModel() returns true.
    bool IsLoading() const { return loading_.load(std::memory_order_acquire); }
    const std::string& LastError() const { return last_error_; }

    // List of clip names found in the loaded model (in source order). Empty
    // if no model is loaded or the model has no animations.
    const std::vector<std::string>& ClipNames() const { return clip_names_; }

    // Selects which animation to play by name. If the name is empty or not
    // found, the first animation is used. Safe to call before/after SetModel.
    void SetActiveClip(const std::string& clip_name);
    const std::string& ActiveClip() const { return active_clip_name_; }

    // Animation playback in seconds. Wraps for looping clips when ticked via
    // Tick(); manual scrub callers can set it directly.
    float CurrentTime() const { return current_time_seconds_; }
    void SetCurrentTime(float seconds);
    float ClipDurationSeconds() const;

    void SetPlaying(bool playing) { playing_ = playing; }
    bool IsPlaying() const { return playing_; }

    void SetPlaybackSpeed(float speed) { playback_speed_ = speed; }
    float PlaybackSpeed() const { return playback_speed_; }

    void SetShowSkeleton(bool show) { show_skeleton_ = show; }
    bool ShowSkeleton() const { return show_skeleton_; }

    void SetShowMeshPoints(bool show) { show_mesh_points_ = show; }
    bool ShowMeshPoints() const { return show_mesh_points_; }

    void SetShowMeshSolid(bool show) { show_mesh_solid_ = show; }
    bool ShowMeshSolid() const { return show_mesh_solid_; }

    void SetShowTexture(bool show) { show_texture_ = show; }
    bool ShowTexture() const { return show_texture_; }

    // Bone selection: lists every bone-or-helper joint drawn in the skeleton
    // overlay (matches the skeleton_nodes_ set). Clicking a joint while the
    // skeleton is visible selects it; clicking empty space clears.
    std::vector<std::string> SelectableBoneNames() const;
    const std::string& SelectedBoneName() const { return selected_bone_name_; }
    void SetSelectedBoneName(const std::string& name) { selected_bone_name_ = name; }
    void ClearSelectedBone() { selected_bone_name_.clear(); }

    // Plain-data view of an AnimatorBoneModifier so the renderer stays
    // decoupled from the asset definition. Panel translates per frame.
    struct BonePhysicsParams
    {
        std::string bone_name;
        float strength = 0.5f;
        float stiffness = 0.5f;
        float damping = 0.5f;
        float mass = 1.0f;
        float drag = 0.05f;
        float gravity_scale = 0.0f;
        std::array<float, 3> gravity_dir = {0.0f, -1.0f, 0.0f};
        float angle_limit_deg = 60.0f;
        bool affects_children = true;
    };

    // Push the current jiggle-bone list. Internally reuses runtime state
    // when bone names match the previous frame so springs don't reset on
    // every parameter tweak.
    void SetBonePhysics(const std::vector<BonePhysicsParams>& params);

    // Plain-data view of a Collision-type modifier: an oriented box in the
    // bone's local space. Refreshed every frame by the panel; drawn as a
    // wireframe overlay that follows the bone's animated transform.
    struct BoneCollisionParams
    {
        std::string bone_name;
        std::array<float, 3> half_extents = {0.05f, 0.05f, 0.05f};
        std::array<float, 3> center = {0.0f, 0.0f, 0.0f};
        int mode = 0; // 0 = trigger, 1 = rigidbody (matches AnimatorBoneCollisionMode)
    };

    void SetBoneCollisions(const std::vector<BoneCollisionParams>& params);

    // Sets the facial blendshape weights (ARKit target name -> weight) applied
    // to the previewed mesh. Empty clears all morph deformation. Refreshed each
    // frame by the Face tab so authored expressions show live.
    void SetFaceWeights(const std::vector<std::pair<std::string, float>>& weights);

    // Estimates a box (in the bone's local space) that encloses the segment
    // from the named bone to its child joints, padded so thin bones still get
    // some girth. Returns false when the bone isn't in the loaded model, in
    // which case the outputs are left untouched. Used by the editor's
    // "Fit to Bone" button.
    bool ComputeBoneFitBox(const std::string& bone_name,
                           std::array<float, 3>& out_half_extents,
                           std::array<float, 3>& out_center) const;

    // Advance playback by delta_seconds (real wall time) honoring playback
    // speed and the active clip's loop status (currently always loops).
    void Tick(float delta_seconds);

    // Draws the preview into the current ImGui child window's content
    // rectangle. Handles orbit-camera input from the supplied region. Returns
    // false if no model is loaded (caller may draw a placeholder).
    bool Render(VulkanContext* vulkan_context, const ImVec2& region_min, const ImVec2& region_max);

private:
    struct SkinInfluence
    {
        std::array<int, 4> bone_indices = {-1, -1, -1, -1};
        std::array<float, 4> bone_weights = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    struct MeshBinding
    {
        // Absolute model-space transform of the mesh's owning node in its
        // bind pose. Used as the static fallback when a mesh has no bones.
        std::array<float, 16> bind_node_transform = {};
        std::vector<std::array<float, 3>> bind_positions;
        std::vector<std::array<float, 3>> bind_normals;
        std::vector<std::array<float, 2>> bind_uvs;
        std::vector<std::uint32_t> indices; // triangle list (length % 3 == 0)
        std::vector<SkinInfluence> influences; // empty if mesh has no bones
        // Morph (blendshape) targets: per-target per-vertex deltas vs the bind
        // pose. Used by the Face-tab live preview to deform the mesh.
        struct MorphTarget
        {
            std::string name;
            std::vector<std::array<float, 3>> position_deltas;
            std::vector<std::array<float, 3>> normal_deltas; // may be empty
        };
        std::vector<MorphTarget> morph_targets;
        // Diffuse tint (sRGB, [0,1]). Sampled from the model's material so meshes
        // render with the right base color even without GPU texturing.
        std::array<float, 3> base_color = {0.78f, 0.80f, 0.84f};
        int texture_index = -1; // index into textures_, or -1 if none
    };

    struct Bone
    {
        std::string name;
        std::array<float, 16> offset_matrix = {}; // mesh-space -> bone-space
    };

    void BuildBindings(const aiScene* scene);
    void CollectMeshBindings(const aiScene* scene, const aiNode* node, const std::array<float, 16>& parent);
    void EvaluateHierarchy(const aiNode* node, const std::array<float, 16>& parent, float time_seconds);
    void ComputeFrameBounds();
    void ResetView();
    void EnsureTextures(VulkanContext* vulkan_context);
    void DestroyTextures();

    // Drains a completed background-loader thread (if any) and promotes the
    // worker-built scene into the live renderer state. Cheap no-op when no
    // load is pending. Called from Tick() and Render() every frame.
    void PollPendingLoad();

    // Runs a fixed-timestep spring-damper integration over node_world_transforms_
    // for every entry in bone_physics_, then recomputes descendant transforms
    // (when affects_children) and animated bone matrices. Called between
    // EvaluateHierarchy() and the GPU bone-matrix upload in Render().
    void StepBonePhysics(float frame_seconds);
    void RecomputeBoneMatricesFromNodeTransforms();

    // Standalone Vulkan rasterizer used to render the preview to an offscreen
    // color attachment that ImGui samples via ImGui::Image. All GPU resources
    // are created lazily on first frame and destroyed in DestroyGpu().
    bool EnsureGpuPipeline(VulkanContext* vulkan_context);
    bool EnsureRenderTarget(VulkanContext* vulkan_context, std::uint32_t width, std::uint32_t height);
    bool EnsureGpuMeshes(VulkanContext* vulkan_context);
    bool EnsureBoneBuffer(VulkanContext* vulkan_context);
    void DestroyGpu();

    std::unique_ptr<Assimp::Importer> importer_;
    const aiScene* scene_ = nullptr;
    bool loaded_ = false;
    std::filesystem::path model_path_;
    std::string last_error_;

    // ---- Async load state (parsed on worker thread) ----------------------
    // While loading_ is true, the worker thread owns pending_importer_ and
    // pending_scene_. When load_ready_ flips to true, the main thread joins
    // the worker, promotes the importer/scene, and runs BuildBindings.
    std::thread load_thread_;
    std::atomic<bool> loading_{false};
    std::atomic<bool> load_ready_{false};
    std::filesystem::path pending_path_;
    std::unique_ptr<Assimp::Importer> pending_importer_;
    const aiScene* pending_scene_ = nullptr;
    std::string pending_error_;

    // Source-data caches built once per loaded model.
    std::vector<MeshBinding> mesh_bindings_;
    std::vector<Bone> bones_;
    std::unordered_map<std::string, std::size_t> bone_index_by_name_;
    std::array<float, 16> global_inverse_ = {};

    // Animation channels indexed by clip then by node name.
    std::vector<std::string> clip_names_;
    std::unordered_map<std::string, std::unordered_map<std::string, const aiNodeAnim*>> channels_by_clip_;
    std::string active_clip_name_;

    // Per-evaluation scratch state.
    std::vector<std::array<float, 16>> animated_bone_matrices_; // global pose * offset, per bone
    std::vector<std::array<float, 16>> node_world_transforms_;  // animated world transform per ai node (by node ptr)
    std::unordered_map<const aiNode*, std::size_t> node_index_;

    // Skeleton-display node list (every node that has an animation channel or
    // is referenced as a bone), with parent indices for line drawing.
    struct SkeletonNode
    {
        const aiNode* node = nullptr;
        int parent_skeleton_index = -1;
    };
    std::vector<SkeletonNode> skeleton_nodes_;

    // Camera/UI state.
    float yaw_ = 0.6f;
    float pitch_ = 0.25f;
    float distance_ = 3.0f;
    std::array<float, 3> focus_ = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> model_center_ = {0.0f, 0.0f, 0.0f};
    float model_radius_ = 1.0f;

    // Playback.
    float current_time_seconds_ = 0.0f;
    float playback_speed_ = 1.0f;
    bool playing_ = true;

    // Display toggles.
    bool show_skeleton_ = true;
    bool show_mesh_points_ = false;
    bool show_mesh_solid_ = true;
    bool show_texture_ = true;

    // Bone selection driven by clicks on skeleton joints.
    std::string selected_bone_name_;

    // Active jiggle-bone parameter set (refreshed every frame by the panel).
    std::vector<BonePhysicsParams> bone_physics_;

    // Active collision-box set (refreshed every frame by the panel). Drawn as
    // wireframe overlays; carries no simulation state.
    std::vector<BoneCollisionParams> bone_collisions_;

    // Current facial blendshape weights (ARKit target name -> weight), pushed by
    // the Face tab each frame. Applied to mesh morph targets in EnsureGpuMeshes.
    std::vector<std::pair<std::string, float>> face_weights_;

    // Per-modifier persistent simulation state. Indexed in lock-step with
    // bone_physics_; entries carry over across frames as long as bone_name
    // matches. Cleared on model reload.
    struct JiggleRuntimeState
    {
        std::string bone_name;
        std::array<float, 3> sim_pos = {0.0f, 0.0f, 0.0f};
        std::array<float, 3> sim_vel = {0.0f, 0.0f, 0.0f};
        bool initialized = false;
    };
    std::vector<JiggleRuntimeState> jiggle_states_;
    float physics_accumulator_seconds_ = 0.0f;

    // GPU textures (one per unique source path/embedded index).
    struct GpuTexture
    {
        std::string source_key; // model-relative path or "*<embedded index>"
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        int width = 0;
        int height = 0;
        bool uploaded = false;
        bool failed = false;
    };
    std::vector<GpuTexture> textures_;
    // Per-material desired texture entry, populated at load time. Filled into
    // GpuTexture during the first frame with a valid VulkanContext.
    struct PendingTexture
    {
        std::string source_key; // "" if none
        std::string source_path; // absolute filesystem path or empty (embedded)
        int embedded_index = -1; // index into aiScene::mTextures, or -1
    };
    std::vector<PendingTexture> pending_textures_;
    VulkanContext* texture_context_ = nullptr; // remembered for cleanup

    // ---- GPU pipeline state (shared across meshes) -----------------------
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkSampler default_sampler_ = VK_NULL_HANDLE;
    VkImage white_image_ = VK_NULL_HANDLE;
    VkDeviceMemory white_memory_ = VK_NULL_HANDLE;
    VkImageView white_view_ = VK_NULL_HANDLE;
    bool gpu_pipeline_ready_ = false;
    bool gpu_pipeline_failed_ = false;

    // ---- Bone matrices storage buffer (one per renderer) -----------------
    VkBuffer bone_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory bone_buffer_memory_ = VK_NULL_HANDLE;
    void* bone_buffer_mapped_ = nullptr;
    VkDeviceSize bone_buffer_size_ = 0;
    std::size_t bone_buffer_capacity_ = 0;

    // ---- Offscreen render target (recreated on resize) -------------------
    VkImage rt_color_image_ = VK_NULL_HANDLE;
    VkDeviceMemory rt_color_memory_ = VK_NULL_HANDLE;
    VkImageView rt_color_view_ = VK_NULL_HANDLE;
    VkImage rt_depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory rt_depth_memory_ = VK_NULL_HANDLE;
    VkImageView rt_depth_view_ = VK_NULL_HANDLE;
    VkFramebuffer rt_framebuffer_ = VK_NULL_HANDLE;
    VkDescriptorSet rt_imgui_descriptor_ = VK_NULL_HANDLE;
    std::uint32_t rt_width_ = 0;
    std::uint32_t rt_height_ = 0;

    // ---- Per-mesh GPU resources -----------------------------------------
    struct GpuMesh
    {
        VkBuffer vertex_buffer = VK_NULL_HANDLE;
        VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
        // Host-visible mapping of the vertex buffer so morph (blendshape)
        // deformation can be re-applied each frame. Non-null once uploaded.
        void* vertex_mapped = nullptr;
        std::uint32_t vertex_count = 0;
        VkBuffer index_buffer = VK_NULL_HANDLE;
        VkDeviceMemory index_memory = VK_NULL_HANDLE;
        VkBuffer material_ubo = VK_NULL_HANDLE;
        VkDeviceMemory material_ubo_memory = VK_NULL_HANDLE;
        void* material_ubo_mapped = nullptr;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        std::uint32_t index_count = 0;
        bool has_skin = false;
        bool uploaded = false;
    };
    std::vector<GpuMesh> gpu_meshes_;
};
