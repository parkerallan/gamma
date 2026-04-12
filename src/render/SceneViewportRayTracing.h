#pragma once

#include "app/VulkanContext.h"
#include "render/SceneViewportLighting.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class SceneViewportRayTracing
{
public:
    struct MaterialRecord
    {
        std::array<float, 4> base_color = {1.0f, 1.0f, 1.0f, 1.0f};
        bool uses_alpha_transparency = false;
        VkImageView base_color_view = VK_NULL_HANDLE;
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

    bool IsAvailable() const { return available_; }
    const std::string& GetStatusMessage() const { return status_message_; }
    VkImage GetOutputImage() const { return output_image_; }
    VkImageView GetOutputView() const { return output_view_; }
    VkDescriptorSet GetOutputDescriptorSet() const { return output_descriptor_set_; }
    VkCommandPool GetCommandPool() const { return command_pool_; }
    VkCommandBuffer GetCommandBuffer() const { return command_buffer_; }
    VkFence GetRenderFence() const { return render_fence_; }
    VkImageLayout GetOutputLayout() const { return output_layout_; }
    void SetOutputLayout(VkImageLayout layout) { output_layout_ = layout; }
    VkAccelerationStructureKHR GetTopLevelAccelerationStructure() const { return top_level_as_.handle; }

private:
    static constexpr std::uint32_t kMaxTextures = 256;

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
        std::uint32_t texture_index = 0xFFFFFFFFu;
        std::uint32_t uses_alpha_transparency = 0;
        std::uint32_t pad0 = 0;
        std::uint32_t pad1 = 0;
    };

    struct UniformBlock
    {
        std::array<float, 16> view_inverse = {};
        std::array<float, 16> projection_inverse = {};
        std::array<float, 4> ambient_light = {1.0f, 1.0f, 1.0f, 1.0f};
        std::array<float, 4> directional_light_color = {1.0f, 1.0f, 1.0f, 0.0f};
        std::array<float, 4> directional_light_direction = {0.0f, -1.0f, 0.0f, 1.0f};
        std::array<float, 4> spot_light_color = {1.0f, 1.0f, 1.0f, 0.0f};
        std::array<float, 4> spot_light_direction = {0.0f, -1.0f, 0.0f, 1.0f};
        std::array<float, 4> spot_light_position = {0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 4> spot_light_data = {0.0f, 0.0f, 0.0f, 0.0f};
        std::array<float, 4> grid_data = {0.0f, 1.0f, 0.0f, 0.0f};
        std::array<float, 4> grid_origin_extent = {0.0f, 0.0f, 0.0f, 0.0f};
        std::uint32_t mesh_count = 0;
        std::uint32_t material_count = 0;
        std::uint32_t section_count = 0;
        std::uint32_t texture_count = 0;
    };

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
    VkSampler output_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSet output_descriptor_set_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
    VkFence render_fence_ = VK_NULL_HANDLE;
    VkImageLayout output_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
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
};