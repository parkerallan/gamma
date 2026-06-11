#pragma once

#include "app/VulkanContext.h"
#include "render/Lighting.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class RayTracing
{
public:
    struct MaterialRecord
    {
        std::array<float, 4> base_color = {1.0f, 1.0f, 1.0f, 1.0f};
        std::array<float, 3> emissive_color = {0.0f, 0.0f, 0.0f};
        std::array<float, 3> attenuation_color = {1.0f, 1.0f, 1.0f};
        std::array<float, 3> specular_color = {1.0f, 1.0f, 1.0f};
        std::array<float, 3> sheen_color = {0.0f, 0.0f, 0.0f};
        float metallic_factor = 1.0f;
        float roughness_factor = 1.0f;
        float normal_scale = 1.0f;
        float occlusion_strength = 1.0f;
        float specular_factor = 1.0f;
        float sheen_roughness_factor = 0.0f;
        float iridescence_factor = 0.0f;
        float iridescence_ior = 1.3f;
        float iridescence_thickness_minimum = 100.0f;
        float iridescence_thickness_maximum = 400.0f;
        float index_of_refraction = 1.5f;
        float transmission_factor = 0.0f;
        float volume_thickness_factor = 0.0f;
        float attenuation_distance = 0.0f;
        float clearcoat_factor = 0.0f;
        float clearcoat_roughness_factor = 0.0f;
        float clearcoat_normal_scale = 1.0f;
        float alpha_cutoff = 0.5f;
        std::uint32_t alpha_mode = 0; // 0=OPAQUE, 1=MASK, 2=BLEND
        bool uses_alpha_transparency = false;
        // Tag flagged by the host when the material's authored name starts
        // with "Hair". Forwarded to the rgen via the primary payload and
        // used to selectively fire extra sub-pixel rays on hair strands.
        bool supersample = false;
        VkImageView base_color_view = VK_NULL_HANDLE;
        VkImageView metallic_roughness_view = VK_NULL_HANDLE;
        VkImageView normal_view = VK_NULL_HANDLE;
        VkImageView occlusion_view = VK_NULL_HANDLE;
        VkImageView emissive_view = VK_NULL_HANDLE;
        VkImageView transmission_view = VK_NULL_HANDLE;
        VkImageView specular_view = VK_NULL_HANDLE;
        VkImageView specular_color_view = VK_NULL_HANDLE;
        VkImageView sheen_color_view = VK_NULL_HANDLE;
        VkImageView sheen_roughness_view = VK_NULL_HANDLE;
        VkImageView iridescence_view = VK_NULL_HANDLE;
        VkImageView iridescence_thickness_view = VK_NULL_HANDLE;
        VkImageView volume_thickness_view = VK_NULL_HANDLE;
        VkImageView clearcoat_view = VK_NULL_HANDLE;
        VkImageView clearcoat_roughness_view = VK_NULL_HANDLE;
        VkImageView clearcoat_normal_view = VK_NULL_HANDLE;
    };

    struct MeshSectionRecord
    {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material_index = 0;
        bool uses_alpha_transparency = false;
    };

    struct MeshInput
    {
        std::string key;
        VkDeviceAddress vertex_device_address = 0;
        VkDeviceAddress index_device_address = 0;
        // Optional: device address of a tightly-packed `vec3` per-vertex
        // buffer holding the *previous* frame's skinned object-space
        // position. Non-zero only for animated/skinned meshes; the RT
        // closest-hit interpolates `pos_obj_prev` from this buffer when
        // computing per-object motion vectors. For static meshes
        // and the very first frame it stays 0 and the shader falls back
        // to `pos_obj_curr` (per-vertex contribution is zero, motion is
        // driven entirely by the per-instance prev_transform).
        VkDeviceAddress prev_position_device_address = 0;
        std::uint32_t vertex_count = 0;
        std::uint32_t vertex_stride = 0;
        std::uint32_t index_count = 0;
        std::uint64_t geometry_revision = 0;
        std::vector<MeshSectionRecord> sections;
        std::vector<MaterialRecord> materials;
    };

    struct InstanceInput
    {
        std::string key;
        std::string mesh_key;
        std::array<float, 16> transform = {};
        std::uint32_t shader_type = 0; // 0=default, 1=water
        // Optional caller-supplied previous-frame transform. If the caller
        // does not supply one (left default-initialized to all zeros), the
        // ray tracer uses its own cached previous transform for this
        // instance key, falling back to `transform` on the first frame.
        std::array<float, 16> prev_transform = {};
        bool has_prev_transform = false;
    };

    struct GpuBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        VkDeviceAddress device_address = 0;
    };

    struct AccelerationStructure
    {
        VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
        GpuBuffer buffer{};
        VkDeviceAddress device_address = 0;
    };

    struct BottomLevelCacheEntry
    {
        AccelerationStructure acceleration_structure{};
        VkDeviceAddress vertex_device_address = 0;
        VkDeviceAddress index_device_address = 0;
        std::uint32_t vertex_count = 0;
        std::uint32_t index_count = 0;
        std::uint64_t geometry_revision = 0;
        bool opaque = true;
        // Persistent device-local scratch reused across BLAS refits (MODE_UPDATE).
        // Allocated once (sized to max(buildScratchSize, updateScratchSize)) and
        // kept alive for the lifetime of the BLAS to avoid per-frame
        // vkAllocateMemory / vkFreeMemory churn.
        GpuBuffer update_scratch_buffer{};
        VkDeviceSize update_scratch_size = 0;
        bool allow_update = false;
    };

    struct ShaderBindingTable
    {
        GpuBuffer buffer{};
        VkStridedDeviceAddressRegionKHR region{};
    };

    bool Initialize(VulkanContext* context);
    void Shutdown();
    bool EnsureViewportOutput(std::uint32_t width, std::uint32_t height);
    bool UpdateScene(const std::vector<MeshInput>& meshes, const std::vector<InstanceInput>& instances);
    bool RenderFrame(
        const ResolvedSceneLighting& lighting,
        const std::array<float, 16>& view_inverse,
        const std::array<float, 16>& projection_inverse,
        bool grid_enabled,
        float grid_spacing,
        float grid_origin_x,
        float grid_origin_z,
        float grid_extent);
    void SetSkyboxTexture(VkImageView skybox_view);
    void SetSkyboxRotation(float rotation_degrees);
    void SetClouds(const std::vector<std::array<float, 4>>& clouds);

    // TAA (temporal anti-aliasing) controls. Off by default until the host
    // explicitly opts in (the editor viewport and the runtime renderer both
    // enable it on startup).
    void SetTAAEnabled(bool enabled);
    bool IsTAAEnabled() const { return taa_enabled_; }

    // Runtime-tunable TAA debug knobs (wired to the editor SettingsPanel).
    struct TaaDebugSettings
    {
        int   viz_mode               = 0;     // 0=normal, 1=mv, 2=pixel weight, 3=jitter, 4=current, 5=history
        float variance_scale         = 1.25f; // 3x3 NCC clamp width on static pixels
        float variance_scale_moving  = 0.75f; // 3x3 NCC clamp width on moving pixels (lerped by motion_weight). Tighter than static to kill disocclusion ghost trails (e.g. a foreground mover passing in front of a static surface).
        float anti_sparkle           = 0.25f; // firefly clamp
        float history_blend          = 0.1f;  // max current weight on static pixels (~10-frame memory)
        // Max current weight on moving pixels (lerped by motion_weight).
        // Default equals the static value (no behavioral change); raising it
        // shortens the history memory on movers, trading shimmer for noise.
        float history_blend_moving   = 0.1f;
        float jitter_compensation    = 0.0f;  // 0..1 strength of jitter_curr subtraction in motion vectors
        // Selective supersampling on hair materials (material name starts
        // with "Hair"). The hit material's flag is forwarded to the rgen
        // via the primary payload; threshold is retained for serialization
        // compatibility only.
        bool  adaptive_enabled       = true;
        int   adaptive_max_samples   = 4;     // 1..8 primary rays for tagged pixels
        float adaptive_threshold     = 0.25f; // legacy; unused
        // Per-pixel soft-shadow ray count when temporal accumulation is
        // unavailable (playmode / dynamic geometry). 1..16, default 4.
        int   dynamic_shadow_samples = 4;
        // 0 = pure average across samples (clean, but subpixel strands look
        // transparent because most samples miss them). 1 = bias toward the
        // brightest sample on high-spread pixels (preserves hair/highlight
        // opacity but can amplify HDR fireflies). 0.7 is a balanced default.
        float adaptive_preservation  = 0.7f;
    };
    void SetTaaDebugSettings(const TaaDebugSettings& settings) { taa_debug_ = settings; }
    const TaaDebugSettings& GetTaaDebugSettings() const { return taa_debug_; }

    bool IsAvailable() const { return available_; }
    const std::string& GetStatusMessage() const { return status_message_; }

    // External (Phase C) compute-skinning dispatches recorded as part of the
    // RT immediate command buffer just before the BLAS refits. Submitting the
    // skin compute on the same CB as the BLAS update removes the extra
    // queue-wait roundtrip and lets us insert the storage-write -> AS-build
    // barrier directly. EnqueueSkinningDispatch is called by RuntimeRenderer
    // during scene preparation; the dispatches are drained inside RenderFrame.
    struct PendingSkinningDispatch
    {
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
        VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
        VkBuffer output_vertex_buffer = VK_NULL_HANDLE;
        // Per-vertex prev-position buffer written by the same compute
        // dispatch. Needs the same compute->RT barrier as output_vertex_buffer
        // because the closest-hit shader reads it for motion vectors.
        VkBuffer prev_position_buffer = VK_NULL_HANDLE;
        std::uint32_t group_count_x = 0;
        std::uint32_t vertex_count = 0;
        std::uint32_t bone_count = 0;
    };
    void EnqueueSkinningDispatch(const PendingSkinningDispatch& dispatch);

    VkDescriptorSet GetOutputDescriptorSet() const { return output_descriptor_set_; }
    VkCommandPool GetCommandPool() const { return command_pool_; }
    VkCommandBuffer GetCommandBuffer() const { return command_buffer_; }
    VkFence GetRenderFence() const { return render_fence_; }
    // Wall-clock GPU time for the most recently completed RT submit (ms).
    // Returns 0 until at least one frame has been submitted and read back.
    float GetLastGpuTimeMs() const { return last_gpu_time_ms_; }
    bool WasFrameSubmittedLastCall() const { return frame_submitted_last_call_; }
    VkImage GetOutputImage() const { return output_image_; }
    VkImageView GetOutputImageView() const { return output_view_; }
    std::uint32_t GetOutputWidth() const { return output_width_; }
    std::uint32_t GetOutputHeight() const { return output_height_; }
    VkImageLayout GetOutputLayout() const { return output_layout_; }
    void SetOutputLayout(VkImageLayout layout) { output_layout_ = layout; }
    // Current-frame linear depth image (R32G32_SFLOAT, VK_IMAGE_LAYOUT_GENERAL
    // after the ray-tracing pass). Written by the rgen shader: .r = primary-
    // ray world-space hit distance, .g = expected previous-frame depth used
    // only by the TAA disocclusion test; sky/miss pixels contain 1e30.
    VkImage GetCurrentDepthImage() const { return depth_images_[taa_parity_]; }
    VkImageView GetCurrentDepthView() const { return depth_views_[taa_parity_]; }
    VkAccelerationStructureKHR GetTopLevelAccelerationStructure() const { return top_level_as_.handle; }

private:
    static constexpr std::uint32_t kMaxTextures = 256;
    static constexpr std::uint32_t kMaxAccumulationFrames = 64;

    struct MeshRecordGpu
    {
        VkDeviceAddress vertex_buffer_address = 0;
        VkDeviceAddress index_buffer_address = 0;
        // Per-vertex previous-frame skinned position buffer (vec3
        // per vertex). Zero for static meshes; non-zero for skinned
        // meshes, populated by the GPU skinning compute pass before the
        // BLAS refit. The closest-hit shader treats a zero address as
        // "no per-vertex prev positions, use pos_obj_curr instead".
        VkDeviceAddress prev_position_buffer_address = 0;
        std::uint32_t vertex_count = 0;
        std::uint32_t vertex_stride = 0;
        std::uint32_t index_count = 0;
        std::uint32_t section_offset = 0;
        std::uint32_t section_count = 0;
        std::uint32_t material_offset = 0;
    };

    // Per-instance motion-vector record. Indexed in the closest
    // hit shader via `gl_InstanceID` (matches the index of the TLAS
    // instance descriptor created in UpdateScene). Both transforms are
    // 4x4 row-major-on-host / column-major-in-GLSL (mat4 in scalar layout
    // round-trips identically when uploaded as a flat std::array<float,16>).
    struct InstanceRecordGpu
    {
        std::array<float, 16> current_transform = {1.0f, 0.0f, 0.0f, 0.0f,
                                                   0.0f, 1.0f, 0.0f, 0.0f,
                                                   0.0f, 0.0f, 1.0f, 0.0f,
                                                   0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 16> prev_transform    = {1.0f, 0.0f, 0.0f, 0.0f,
                                                   0.0f, 1.0f, 0.0f, 0.0f,
                                                   0.0f, 0.0f, 1.0f, 0.0f,
                                                   0.0f, 0.0f, 0.0f, 1.0f};
        std::array<std::uint32_t, 4> shader_data = {0, 0, 0, 0};
    };

    struct SectionRecordGpu
    {
        std::uint32_t first_index = 0;
        std::uint32_t index_count = 0;
        std::uint32_t material_index = 0;
        std::uint32_t uses_alpha_transparency = 0;
    };

    struct MaterialRecordGpu
    {
        std::array<float, 4> base_color = {1.0f, 1.0f, 1.0f, 1.0f};
        std::array<float, 4> emissive_data = {0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 4> surface_data = {1.0f, 1.0f, 1.0f, 0.5f}; // xyz=metallic/roughness/occlusion, w=alpha_cutoff
        std::array<float, 4> iridescence_data = {0.0f, 1.3f, 100.0f, 400.0f};
        std::array<float, 4> transmission_data = {0.0f, 1.5f, 0.0f, 0.0f};
        std::array<float, 4> attenuation_data = {1.0f, 1.0f, 1.0f, 0.0f};
        std::array<float, 4> clearcoat_data = {0.0f, 0.0f, 1.0f, 0.0f};
        std::array<float, 4> specular_data = {1.0f, 1.0f, 1.0f, 1.0f};
        std::array<float, 4> sheen_data = {0.0f, 0.0f, 0.0f, 0.0f};
        std::uint32_t base_color_texture_index = 0xFFFFFFFFu;
        std::uint32_t metallic_roughness_texture_index = 0xFFFFFFFFu;
        std::uint32_t normal_texture_index = 0xFFFFFFFFu;
        std::uint32_t occlusion_texture_index = 0xFFFFFFFFu;
        std::uint32_t emissive_texture_index = 0xFFFFFFFFu;
        std::uint32_t transmission_texture_index = 0xFFFFFFFFu;
        std::uint32_t specular_texture_index = 0xFFFFFFFFu;
        std::uint32_t specular_color_texture_index = 0xFFFFFFFFu;
        std::uint32_t sheen_color_texture_index = 0xFFFFFFFFu;
        std::uint32_t sheen_roughness_texture_index = 0xFFFFFFFFu;
        std::uint32_t iridescence_texture_index = 0xFFFFFFFFu;
        std::uint32_t iridescence_thickness_texture_index = 0xFFFFFFFFu;
        std::uint32_t volume_thickness_texture_index = 0xFFFFFFFFu;
        std::uint32_t clearcoat_texture_index = 0xFFFFFFFFu;
        std::uint32_t clearcoat_roughness_texture_index = 0xFFFFFFFFu;
        std::uint32_t clearcoat_normal_texture_index = 0xFFFFFFFFu;
        std::uint32_t uses_alpha_transparency = 0;
        std::uint32_t alpha_mode = 0; // 0=OPAQUE, 1=MASK, 2=BLEND
        std::uint32_t supersample = 0; // 1 = primary rgen should fire extra rays on hits to this material
    };

    struct UniformBlock
    {
        std::array<float, 16> view_inverse = {};
        std::array<float, 16> projection_inverse = {};
        std::array<float, 4> ambient_light = {0.0f, 0.0f, 0.0f, 0.0f};
        std::array<float, 4> directional_light_color = {1.0f, 1.0f, 1.0f, 0.0f};
        std::array<float, 4> directional_light_direction = {0.0f, -1.0f, 0.0f, 1.0f};
        std::array<float, 4> directional_light_data = {0.004712389f, 0.0f, 0.0f, 0.0f};
        std::array<float, 4> point_light_color = {1.0f, 1.0f, 1.0f, 0.0f};
        std::array<float, 4> point_light_position = {0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 4> point_light_data = {0.1f, 0.0f, 0.0f, 0.0f};
        std::array<float, 4> spot_light_color = {1.0f, 1.0f, 1.0f, 0.0f};
        std::array<float, 4> spot_light_direction = {0.0f, -1.0f, 0.0f, 1.0f};
        std::array<float, 4> spot_light_position = {0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 4> spot_light_data = {0.0f, 0.1f, 0.0f, 0.0f};
        std::array<float, 4> grid_data = {0.0f, 1.0f, 0.0f, 0.0f};
        std::array<float, 4> grid_origin_extent = {0.0f, 0.0f, 0.0f, 0.0f};
        std::array<float, 4> skybox_data = {0.0f, 0.0f, 0.0f, 0.0f};
        std::uint32_t mesh_count = 0;
        std::uint32_t material_count = 0;
        std::uint32_t section_count = 0;
        std::uint32_t texture_count = 0;
        std::array<std::uint32_t, 4> accumulation_data = {0, 0, 0, 0};
        std::array<float, 4> animation_time_data = {0.0f, 0.0f, 0.0f, 0.0f};
        // Row-major previous-frame view*projection matrix used by the ray-gen
        // shader to compute camera-space motion vectors for TAA reprojection.
        std::array<float, 16> prev_view_projection = {1.0f, 0.0f, 0.0f, 0.0f,
                                                      0.0f, 1.0f, 0.0f, 0.0f,
                                                      0.0f, 0.0f, 1.0f, 0.0f,
                                                      0.0f, 0.0f, 0.0f, 1.0f};
        // Current-frame view*projection used by the ray-gen shader to
        // project the per-instance interpolated current world position to
        // screen space (motion-vector convention).
        std::array<float, 16> current_view_projection = {1.0f, 0.0f, 0.0f, 0.0f,
                                                         0.0f, 1.0f, 0.0f, 0.0f,
                                                         0.0f, 0.0f, 1.0f, 0.0f,
                                                         0.0f, 0.0f, 0.0f, 1.0f};
        // (enabled, variance_scale, anti_sparkle, _).
        std::array<float, 4> taa_params = {0.0f, 1.0f, 1.0f, 0.0f};
        // (jitter_x_px, jitter_y_px, prev_jitter_x_px, prev_jitter_y_px).
        std::array<float, 4> jitter_offset = {0.0f, 0.0f, 0.0f, 0.0f};
        // Adaptive supersampling driven by last frame's 3x3 luma contrast.
        // .x = enabled (0/1), .y = max_samples (float, 1..8),
        // .z = contrast_threshold (e.g. 0.25), .w = feature_preservation (0..1).
        std::array<float, 4> adaptive_params = {0.0f, 1.0f, 0.25f, 0.7f};
        // Global water surface controls for full-screen underwater shading.
        // .x = has water surface (0/1), .y = base water height in world Y.
        std::array<float, 4> underwater_data = {0.0f, 0.0f, 0.0f, 0.0f};
        // World->local transform of the water plane used to confine
        // underwater logic to the plane footprint.
        std::array<float, 16> underwater_world_to_local = {1.0f, 0.0f, 0.0f, 0.0f,
                                                           0.0f, 1.0f, 0.0f, 0.0f,
                                                           0.0f, 0.0f, 1.0f, 0.0f,
                                                           0.0f, 0.0f, 0.0f, 1.0f};
        // Cloud volumes: up to 8 clouds, each xyz=world-space center, w=radius.
        // cloud_count.x = number of active clouds (0 = disabled).
        std::array<std::uint32_t, 4> cloud_count = {0, 0, 0, 0};
        std::array<float, 32> cloud_params = {};  // 8 * vec4
        // Previous frame's camera world position (.xyz; .w unused). The rgen
        // uses it to write the expected previous-frame depth consumed by the
        // TAA depth-disocclusion test. Appended at the end of the block so
        // shaders that declare only a prefix of SceneUniforms stay
        // layout-compatible.
        std::array<float, 4> prev_camera_position = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    // CPU-side parameters fed into the TAA compute UBO each frame.
    struct TaaUniformBlock
    {
        std::array<std::uint32_t, 4> extent = {0, 0, 0, 0};
        std::array<float, 4> jitter = {0.0f, 0.0f, 0.0f, 0.0f};
        // .x = history_valid, .y = variance_scale, .z = anti_sparkle, .w = history_blend_max
        std::array<float, 4> params = {0.0f, 1.0f, 1.0f, 0.1f};
        // .x = viz_mode (int cast to float), .yzw = reserved
        std::array<float, 4> debug = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    void ResetAccumulationState();
    void DestroyOutputResources();
    void DestroyFrameResources();
    void DestroySceneResources();
    void DestroyPipelineResources();
    bool EnsurePipelineResources();
    bool UpdateDescriptors();

    // ---- TAA helpers ----
    bool EnsureTaaResources();
    bool UpdateTaaDescriptors();
    void DestroyTaaResources();
    // Compute and cache an updated previous-frame view*projection matrix
    // from the current frame's inverses; updates the host UBO field.
    void ComputePrevViewProjection(
        const std::array<float, 16>& view_inverse,
        const std::array<float, 16>& projection_inverse,
        std::array<float, 16>& out_view_projection);

    VulkanContext* vulkan_context_ = nullptr;
    bool available_ = false;
    std::string status_message_;
    VkImage output_image_ = VK_NULL_HANDLE;
    VkDeviceMemory output_memory_ = VK_NULL_HANDLE;
    VkImageView output_view_ = VK_NULL_HANDLE;
    VkImage history_image_ = VK_NULL_HANDLE;
    VkDeviceMemory history_memory_ = VK_NULL_HANDLE;
    VkImageView history_view_ = VK_NULL_HANDLE;
    VkSampler output_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet output_descriptor_set_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence render_fence_ = VK_NULL_HANDLE;
    // GPU timestamp queries written around each RT command buffer (top of
    // pipe at the start, bottom of pipe at the end). Read on the *next*
    // frame after we know render_fence_ has been signaled. Provides a real
    // GPU-work measurement instead of the previous "duration of a CPU stall
    // on vkWaitForFences" misnomer.
    VkQueryPool timestamp_pool_ = VK_NULL_HANDLE;
    bool timestamp_pending_ = false;
    float last_gpu_time_ms_ = 0.0f;
    double timestamp_period_ns_ = 0.0;
    VkImageLayout output_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout history_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t output_width_ = 0;
    std::uint32_t output_height_ = 0;
    GpuBuffer uniform_buffer_{};
    GpuBuffer mesh_record_buffer_{};
    GpuBuffer section_record_buffer_{};
    GpuBuffer material_record_buffer_{};
    GpuBuffer instance_buffer_{};
    // Per-instance record buffer (current + previous transform),
    // indexed by `gl_InstanceID` in the closest-hit shader. Rebuilt every
    // UpdateScene call (every frame on dynamic scenes).
    GpuBuffer instance_record_buffer_{};
    std::vector<InstanceRecordGpu> instance_records_cpu_;
    std::uint32_t instance_record_buffer_capacity_ = 0;
    // Cache of the previous frame's transform per instance key. Looked up
    // in UpdateScene to populate `InstanceRecordGpu::prev_transform`; new
    // instances use their current transform as their first prev (so the
    // first frame collapses motion to camera-only reprojection).
    std::unordered_map<std::string, std::array<float, 16>> prev_instance_transforms_;
    // Transforms staged by the most recent UpdateScene (what the next
    // rendered frame will use). Promoted into prev_instance_transforms_
    // only when a frame is actually submitted, so "prev" always means
    // "last RENDERED frame" even when RenderFrame skips on a busy fence
    // (UpdateScene runs every app frame; renders may not).
    std::unordered_map<std::string, std::array<float, 16>> latest_instance_transforms_;
    bool has_water_surface_ = false;
    float water_surface_base_height_ = 0.0f;
    std::array<float, 16> water_surface_world_to_local_ = {1.0f, 0.0f, 0.0f, 0.0f,
                                                            0.0f, 1.0f, 0.0f, 0.0f,
                                                            0.0f, 0.0f, 1.0f, 0.0f,
                                                            0.0f, 0.0f, 0.0f, 1.0f};
    std::vector<std::array<float, 4>> clouds_;
    VkImage fallback_texture_image_ = VK_NULL_HANDLE;
    VkDeviceMemory fallback_texture_memory_ = VK_NULL_HANDLE;
    VkImageView fallback_texture_view_ = VK_NULL_HANDLE;
    VkImageView skybox_texture_view_ = VK_NULL_HANDLE;
    float skybox_rotation_degrees_ = 0.0f;
    VkSampler texture_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    ShaderBindingTable raygen_sbt_{};
    ShaderBindingTable miss_sbt_{};
    ShaderBindingTable hit_sbt_{};
    AccelerationStructure top_level_as_{};
    std::unordered_map<std::string, BottomLevelCacheEntry> bottom_level_cache_;
    std::vector<MeshRecordGpu> mesh_records_cpu_;
    std::vector<SectionRecordGpu> section_records_cpu_;
    std::vector<MaterialRecordGpu> material_records_cpu_;
    std::vector<VkDescriptorImageInfo> texture_descriptors_cpu_;
    UniformBlock accumulation_reference_uniforms_{};
    bool accumulation_reference_uniforms_valid_ = false;
    std::uint32_t accumulation_frame_count_ = 0;
    std::uint32_t raw_frame_count_ = 0;
    std::uint64_t animation_time_start_ticks_ = 0;
    bool accumulation_reset_requested_ = true;
    bool dynamic_geometry_present_ = false;
    std::uint64_t scene_signature_ = 0;
    bool scene_signature_valid_ = false;

    // Geometry signature – hashes mesh/section/material/texture records only.
    // Unchanged during gizmo drag (only transforms change), so storage buffers are skipped.
    std::uint64_t geometry_signature_ = 0;
    bool geometry_signature_valid_ = false;

    // TLAS topology signature – hashes BLAS device-addresses and instance count.
    // Unchanged during gizmo drag, enabling TLAS refit instead of a full rebuild.
    std::uint64_t tlas_topology_signature_ = 0;
    bool tlas_topology_signature_valid_ = false;
    std::uint32_t tlas_capacity_ = 0;  // max instances the current TLAS AS was sized for
    std::uint32_t instance_buffer_capacity_ = 0;  // capacity of instance_buffer_ in instance count

    // Pending TLAS data set by UpdateScene, consumed by RenderFrame command buffer.
    std::vector<VkAccelerationStructureInstanceKHR> pending_acceleration_instances_;
    bool tlas_rebuild_pending_ = false;  // full rebuild required (topology or capacity changed)
    bool tlas_refit_pending_   = false;  // transform-only update via VK UPDATE mode

    // Pending BLAS refit requests collected by UpdateScene and submitted from
    // RenderFrame. Recording the BLAS UPDATE on the same immediate command
    // buffer as the TLAS work avoids a per-animated-frame extra
    // vkQueueWaitIdle (which previously stalled the CPU on the prior frame's
    // ray-tracing dispatch — the dominant source of animated-mesh stutter).
    struct PendingBlasRefit
    {
        std::string mesh_key;
        VkDeviceAddress vertex_device_address = 0;
        VkDeviceAddress index_device_address = 0;
        std::uint32_t vertex_count = 0;
        std::uint32_t vertex_stride = 0;
        std::uint32_t index_count = 0;
        std::uint64_t geometry_revision = 0;
        bool opaque = true;
    };
    std::vector<PendingBlasRefit> pending_blas_refits_;

    // Phase C — pending compute-skinning dispatches recorded on the same
    // immediate CB as BLAS refits, with a STORAGE_WRITE -> AS_BUILD_INPUT_READ
    // memory barrier separating the two passes.
    std::vector<PendingSkinningDispatch> pending_skinning_dispatches_;

    // Persistent device-local scratch buffer for TLAS build / refit.
    // Sized to max(buildScratchSize, updateScratchSize) at last full build.
    GpuBuffer tlas_scratch_buffer_{};

    // Guards vkUpdateDescriptorSets – set only when descriptor bindings actually change.
    bool descriptors_dirty_ = false;

    // ---- TAA state ----
    // Motion vector image: rg16f, written by the ray-gen shader, sampled
    // (point) by the TAA compute pass.
    VkImage motion_image_ = VK_NULL_HANDLE;
    VkDeviceMemory motion_memory_ = VK_NULL_HANDLE;
    VkImageView motion_view_ = VK_NULL_HANDLE;
    VkImageLayout motion_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    // Linear-depth G-buffer ping-pong (R32G32_SFLOAT). Each frame the rgen
    // writes into `depth_images_[taa_parity_]`: .r = primary-ray hit
    // distance from the current camera, .g = expected previous-frame depth
    // (distance from the previous camera to the hit's previous-frame world
    // position). The TAA compute pass compares this frame's .g against the
    // *other* slot's .r (the actual previous-frame depth) for the bilateral
    // disocclusion test — both sides measured from the same camera origin,
    // so camera translation alone cannot fire it. On a miss the rgen writes
    // 1e30 so sky->sky reprojections accept and sky->object reject.
    VkImage depth_images_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory depth_memories_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView depth_views_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageLayout depth_layouts_[2] = {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED};
    // Ping-pong storage for the previous and current TAA results (PQ-encoded
    // HDR). Index `taa_parity_` is the *current* output; the other slot is
    // sampled as the "previous frame" input.
    VkImage taa_images_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory taa_memories_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView taa_views_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageLayout taa_layouts_[2] = {VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_UNDEFINED};
    VkSampler taa_sampler_ = VK_NULL_HANDLE;
    GpuBuffer taa_uniform_buffer_{};
    VkDescriptorSetLayout taa_descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout taa_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline taa_pipeline_ = VK_NULL_HANDLE;
    VkDescriptorSet taa_descriptor_sets_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool taa_descriptors_dirty_ = false;
    bool taa_enabled_ = false;
    bool taa_history_valid_ = false;
    bool frame_submitted_last_call_ = false;
    std::uint32_t taa_parity_ = 0;
    TaaDebugSettings taa_debug_{};
    std::array<float, 16> prev_view_projection_ = {1.0f, 0.0f, 0.0f, 0.0f,
                                                   0.0f, 1.0f, 0.0f, 0.0f,
                                                   0.0f, 0.0f, 1.0f, 0.0f,
                                                   0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 2> taa_prev_jitter_px_ = {0.0f, 0.0f};
    std::uint32_t taa_jitter_index_ = 0;
    // Camera world position of the last rendered frame; cached alongside
    // prev_view_projection_ and fed to the rgen as
    // SceneUniforms.prev_camera_position.
    std::array<float, 4> prev_camera_position_ = {0.0f, 0.0f, 0.0f, 0.0f};
};