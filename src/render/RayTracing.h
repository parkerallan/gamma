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

    bool IsAvailable() const { return available_; }
    const std::string& GetStatusMessage() const { return status_message_; }
    VkDescriptorSet GetOutputDescriptorSet() const { return output_descriptor_set_; }
    VkCommandPool GetCommandPool() const { return command_pool_; }
    VkCommandBuffer GetCommandBuffer() const { return command_buffer_; }
    VkFence GetRenderFence() const { return render_fence_; }
    VkImage GetOutputImage() const { return output_image_; }
    VkImageView GetOutputImageView() const { return output_view_; }
    std::uint32_t GetOutputWidth() const { return output_width_; }
    std::uint32_t GetOutputHeight() const { return output_height_; }
    VkImageLayout GetOutputLayout() const { return output_layout_; }
    void SetOutputLayout(VkImageLayout layout) { output_layout_ = layout; }
    VkAccelerationStructureKHR GetTopLevelAccelerationStructure() const { return top_level_as_.handle; }

private:
    static constexpr std::uint32_t kMaxTextures = 256;
    static constexpr std::uint32_t kMaxAccumulationFrames = 64;

    struct MeshRecordGpu
    {
        VkDeviceAddress vertex_buffer_address = 0;
        VkDeviceAddress index_buffer_address = 0;
        std::uint32_t vertex_count = 0;
        std::uint32_t vertex_stride = 0;
        std::uint32_t index_count = 0;
        std::uint32_t section_offset = 0;
        std::uint32_t section_count = 0;
        std::uint32_t material_offset = 0;
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
    };

    void ResetAccumulationState();
    void DestroyOutputResources();
    void DestroyFrameResources();
    void DestroySceneResources();
    void DestroyPipelineResources();
    bool EnsurePipelineResources();
    bool UpdateDescriptors();

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
    VkImageLayout output_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout history_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    std::uint32_t output_width_ = 0;
    std::uint32_t output_height_ = 0;
    GpuBuffer uniform_buffer_{};
    GpuBuffer mesh_record_buffer_{};
    GpuBuffer section_record_buffer_{};
    GpuBuffer material_record_buffer_{};
    GpuBuffer instance_buffer_{};
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

    // Persistent device-local scratch buffer for TLAS build / refit.
    // Sized to max(buildScratchSize, updateScratchSize) at last full build.
    GpuBuffer tlas_scratch_buffer_{};

    // Guards vkUpdateDescriptorSets – set only when descriptor bindings actually change.
    bool descriptors_dirty_ = false;
};