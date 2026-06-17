#include "render/RainParticleRenderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace
{
// --- Small Vulkan helpers (mirrors the per-renderer pattern used by
// Scene2DRenderer / RayTracing; each renderer self-contains these). ---

std::filesystem::path ResolveShaderPath(const char* file_name)
{
    const char* base_path_raw = SDL_GetBasePath();
    const std::filesystem::path base_path =
        base_path_raw != nullptr ? std::filesystem::path(base_path_raw) : std::filesystem::current_path();
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

VkShaderModule LoadShaderModule(VkDevice device, const std::filesystem::path& path)
{
    const std::vector<std::uint8_t> bytes = ReadBinaryFile(path);
    if (bytes.empty())
    {
        SDL_Log("RainParticleRenderer: failed to read shader: %s", path.string().c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes.size();
    ci.pCode = reinterpret_cast<const std::uint32_t*>(bytes.data());
    VkShaderModule module = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(device, &ci, nullptr, &module);
    VulkanContext::CheckVkResult(result);
    return result == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

std::uint32_t FindMemoryType(VkPhysicalDevice physical_device, std::uint32_t type_filter, VkMemoryPropertyFlags props)
{
    VkPhysicalDeviceMemoryProperties mem_props = {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
    for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i)
    {
        if ((type_filter & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & props) == props)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

bool CreateGpuBuffer(
    VkPhysicalDevice physical_device,
    VkDevice device,
    const VkAllocationCallbacks* allocator,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkMemoryPropertyFlags props,
    VkBuffer& buffer,
    VkDeviceMemory& memory)
{
    VkBufferCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result = vkCreateBuffer(device, &ci, allocator, &buffer);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements req = {};
    vkGetBufferMemoryRequirements(device, buffer, &req);

    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(physical_device, req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyBuffer(device, buffer, allocator);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkAllocateMemory(device, &ai, allocator, &memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkDestroyBuffer(device, buffer, allocator);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    result = vkBindBufferMemory(device, buffer, memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        vkFreeMemory(device, memory, allocator);
        vkDestroyBuffer(device, buffer, allocator);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void TransitionImage(
    VkCommandBuffer cmd,
    VkImage image,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage,
    VkAccessFlags src_access,
    VkAccessFlags dst_access)
{
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// std140 uniform block layouts (must match the GLSL shaders exactly).
struct ComputeParamsBlock
{
    float params[4];          // dt, time, fall_speed, max_fall_dist
    std::uint32_t control[4]; // count, reset, pad, pad
    float plane_to_world[16]; // emitter plane world matrix (for the collision ray)
};

struct ViewParamsBlock
{
    float view_projection[16];
    float plane_to_world[16];
    float cam_pos[4];
    float color[4];
    float params[4];     // streak_width, streak_length, time, column_height
    float depth_size[4]; // width, height, 0, 0
};

// World-space tuning constants.
constexpr float kMaxFallDist = 500.0f;     // how far below the emitter to search for a surface
constexpr float kFallSpeedBase = 9.0f;     // world units/sec at fall_speed = 1
constexpr float kStreakWidthBase = 0.06f;  // at drop_size = 1
constexpr float kStreakLengthBase = 0.5f;  // at drop_size = 1
} // namespace

bool RainParticleRenderer::Initialize(VulkanContext* context)
{
    if (context == nullptr)
    {
        return false;
    }
    vulkan_context_ = context;

    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    VkCommandPoolCreateInfo pool_ci = {};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.queueFamilyIndex = vulkan_context_->GetQueueFamily();
    pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkResult result = vkCreateCommandPool(device, &pool_ci, allocator, &command_pool_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    std::array<VkDescriptorPoolSize, 4> pool_sizes = {};
    pool_sizes[0] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    pool_sizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2};
    pool_sizes[2] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    pool_sizes[3] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
    VkDescriptorPoolCreateInfo dp_ci = {};
    dp_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp_ci.maxSets = 2;
    dp_ci.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
    dp_ci.pPoolSizes = pool_sizes.data();
    result = vkCreateDescriptorPool(device, &dp_ci, allocator, &descriptor_pool_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    return true;
}

void RainParticleRenderer::Shutdown()
{
    if (vulkan_context_ == nullptr)
    {
        return;
    }
    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    vkDeviceWaitIdle(device);

    ClearFramebufferCache();

    auto destroy_buffer = [&](VkBuffer& b, VkDeviceMemory& m, void*& mapped)
    {
        if (mapped != nullptr) { vkUnmapMemory(device, m); mapped = nullptr; }
        if (b != VK_NULL_HANDLE) { vkDestroyBuffer(device, b, allocator); b = VK_NULL_HANDLE; }
        if (m != VK_NULL_HANDLE) { vkFreeMemory(device, m, allocator); m = VK_NULL_HANDLE; }
    };
    void* no_map = nullptr;
    destroy_buffer(particle_buffer_, particle_memory_, no_map);
    destroy_buffer(compute_ubo_, compute_ubo_memory_, compute_ubo_mapped_);
    destroy_buffer(view_ubo_, view_ubo_memory_, view_ubo_mapped_);

    if (compute_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device, compute_pipeline_, allocator); compute_pipeline_ = VK_NULL_HANDLE; }
    if (compute_pipeline_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, compute_pipeline_layout_, allocator); compute_pipeline_layout_ = VK_NULL_HANDLE; }
    if (compute_set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, compute_set_layout_, allocator); compute_set_layout_ = VK_NULL_HANDLE; }

    if (gfx_pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device, gfx_pipeline_, allocator); gfx_pipeline_ = VK_NULL_HANDLE; }
    if (gfx_pipeline_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, gfx_pipeline_layout_, allocator); gfx_pipeline_layout_ = VK_NULL_HANDLE; }
    if (gfx_set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, gfx_set_layout_, allocator); gfx_set_layout_ = VK_NULL_HANDLE; }
    if (render_pass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device, render_pass_, allocator); render_pass_ = VK_NULL_HANDLE; }

    if (fence_ != VK_NULL_HANDLE) { vkDestroyFence(device, fence_, allocator); fence_ = VK_NULL_HANDLE; }
    if (descriptor_pool_ != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, descriptor_pool_, allocator); descriptor_pool_ = VK_NULL_HANDLE; }
    if (command_pool_ != VK_NULL_HANDLE) { vkDestroyCommandPool(device, command_pool_, allocator); command_pool_ = VK_NULL_HANDLE; }

    resources_ready_ = false;
    in_flight_ = false;
    cmd_ = VK_NULL_HANDLE;
    bound_depth_view_ = VK_NULL_HANDLE;
    vulkan_context_ = nullptr;
}

bool RainParticleRenderer::EnsureResources()
{
    if (resources_ready_)
    {
        return true;
    }
    if (vulkan_context_ == nullptr)
    {
        return false;
    }

    const VkPhysicalDevice physical = vulkan_context_->GetPhysicalDevice();
    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    // Device-local particle SSBO.
    const VkDeviceSize particle_bytes = static_cast<VkDeviceSize>(particle_count_) * sizeof(float) * 4;
    if (!CreateGpuBuffer(physical, device, allocator, particle_bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            particle_buffer_, particle_memory_))
    {
        return false;
    }

    // Host-visible uniform buffers (persistently mapped).
    if (!CreateGpuBuffer(physical, device, allocator, sizeof(ComputeParamsBlock),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            compute_ubo_, compute_ubo_memory_))
    {
        return false;
    }
    if (!CreateGpuBuffer(physical, device, allocator, sizeof(ViewParamsBlock),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            view_ubo_, view_ubo_memory_))
    {
        return false;
    }
    if (vkMapMemory(device, compute_ubo_memory_, 0, VK_WHOLE_SIZE, 0, &compute_ubo_mapped_) != VK_SUCCESS ||
        vkMapMemory(device, view_ubo_memory_, 0, VK_WHOLE_SIZE, 0, &view_ubo_mapped_) != VK_SUCCESS)
    {
        return false;
    }

    if (!CreateComputePipeline() || !CreateGraphicsPipeline())
    {
        return false;
    }

    UpdateComputeDescriptor();

    VkFenceCreateInfo fence_ci = {};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device, &fence_ci, allocator, &fence_) != VK_SUCCESS)
    {
        return false;
    }

    resources_ready_ = true;
    return true;
}

bool RainParticleRenderer::CreateComputePipeline()
{
    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    std::array<VkDescriptorSetLayoutBinding, 3> bindings = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dsl_ci = {};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = static_cast<std::uint32_t>(bindings.size());
    dsl_ci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &dsl_ci, allocator, &compute_set_layout_) != VK_SUCCESS)
    {
        return false;
    }

    VkPipelineLayoutCreateInfo pl_ci = {};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &compute_set_layout_;
    if (vkCreatePipelineLayout(device, &pl_ci, allocator, &compute_pipeline_layout_) != VK_SUCCESS)
    {
        return false;
    }

    VkShaderModule module = LoadShaderModule(device, ResolveShaderPath("rain_particles.comp.spv"));
    if (module == VK_NULL_HANDLE)
    {
        return false;
    }
    VkPipelineShaderStageCreateInfo stage = {};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";
    VkComputePipelineCreateInfo cp_ci = {};
    cp_ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp_ci.stage = stage;
    cp_ci.layout = compute_pipeline_layout_;
    const VkResult result = vkCreateComputePipelines(device, vulkan_context_->GetPipelineCache(), 1, &cp_ci, allocator, &compute_pipeline_);
    vkDestroyShaderModule(device, module, allocator);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        compute_pipeline_ = VK_NULL_HANDLE;
        return false;
    }

    VkDescriptorSetAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = descriptor_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &compute_set_layout_;
    return vkAllocateDescriptorSets(device, &ai, &compute_set_) == VK_SUCCESS;
}

bool RainParticleRenderer::CreateGraphicsPipeline()
{
    const VkDevice device = vulkan_context_->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context_->GetAllocator();

    // Render pass: load existing RT color, alpha-blend rain, store. Matches the
    // R8G8B8A8_UNORM / COLOR_ATTACHMENT_OPTIMAL contract used by Scene2DRenderer.
    VkAttachmentDescription attachment = {};
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = {};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_ci = {};
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 1;
    rp_ci.pAttachments = &attachment;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &subpass;
    rp_ci.dependencyCount = 1;
    rp_ci.pDependencies = &dep;
    if (vkCreateRenderPass(device, &rp_ci, allocator, &render_pass_) != VK_SUCCESS)
    {
        return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 3> bindings = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dsl_ci = {};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = static_cast<std::uint32_t>(bindings.size());
    dsl_ci.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &dsl_ci, allocator, &gfx_set_layout_) != VK_SUCCESS)
    {
        return false;
    }

    VkPipelineLayoutCreateInfo pl_ci = {};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &gfx_set_layout_;
    if (vkCreatePipelineLayout(device, &pl_ci, allocator, &gfx_pipeline_layout_) != VK_SUCCESS)
    {
        return false;
    }

    VkShaderModule vert = LoadShaderModule(device, ResolveShaderPath("rain_particle.vert.spv"));
    VkShaderModule frag = LoadShaderModule(device, ResolveShaderPath("rain_particle.frag.spv"));
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
    {
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(device, vert, allocator);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(device, frag, allocator);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    // No vertex buffers: the quad is generated from gl_VertexIndex.
    VkPipelineVertexInputStateCreateInfo vi_ci = {};
    vi_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia_ci = {};
    ia_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia_ci.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

    VkPipelineViewportStateCreateInfo vp_ci = {};
    vp_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp_ci.viewportCount = 1;
    vp_ci.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs_ci = {};
    rs_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs_ci.polygonMode = VK_POLYGON_MODE_FILL;
    rs_ci.cullMode = VK_CULL_MODE_NONE;
    rs_ci.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs_ci.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms_ci = {};
    ms_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms_ci.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Alpha blend over the scene; preserve destination alpha (opaque target).
    VkPipelineColorBlendAttachmentState blend = {};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb_ci = {};
    cb_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb_ci.attachmentCount = 1;
    cb_ci.pAttachments = &blend;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn_ci = {};
    dyn_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn_ci.dynamicStateCount = 2;
    dyn_ci.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo gp_ci = {};
    gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp_ci.stageCount = 2;
    gp_ci.pStages = stages;
    gp_ci.pVertexInputState = &vi_ci;
    gp_ci.pInputAssemblyState = &ia_ci;
    gp_ci.pViewportState = &vp_ci;
    gp_ci.pRasterizationState = &rs_ci;
    gp_ci.pMultisampleState = &ms_ci;
    gp_ci.pColorBlendState = &cb_ci;
    gp_ci.pDynamicState = &dyn_ci;
    gp_ci.layout = gfx_pipeline_layout_;
    gp_ci.renderPass = render_pass_;
    gp_ci.subpass = 0;

    const VkResult result = vkCreateGraphicsPipelines(device, vulkan_context_->GetPipelineCache(), 1, &gp_ci, allocator, &gfx_pipeline_);
    vkDestroyShaderModule(device, vert, allocator);
    vkDestroyShaderModule(device, frag, allocator);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        gfx_pipeline_ = VK_NULL_HANDLE;
        return false;
    }

    VkDescriptorSetAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = descriptor_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &gfx_set_layout_;
    return vkAllocateDescriptorSets(device, &ai, &gfx_set_) == VK_SUCCESS;
}

void RainParticleRenderer::UpdateComputeDescriptor()
{
    VkDescriptorBufferInfo ssbo = {particle_buffer_, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo ubo = {compute_ubo_, 0, sizeof(ComputeParamsBlock)};

    std::array<VkWriteDescriptorSet, 2> writes = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = compute_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &ssbo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = compute_set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &ubo;
    vkUpdateDescriptorSets(vulkan_context_->GetDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void RainParticleRenderer::UpdateComputeTlas(VkAccelerationStructureKHR tlas)
{
    VkWriteDescriptorSetAccelerationStructureKHR as_info = {};
    as_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    as_info.accelerationStructureCount = 1;
    as_info.pAccelerationStructures = &tlas;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext = &as_info;
    write.dstSet = compute_set_;
    write.dstBinding = 2;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    vkUpdateDescriptorSets(vulkan_context_->GetDevice(), 1, &write, 0, nullptr);
    bound_tlas_ = tlas;
}

void RainParticleRenderer::UpdateGraphicsDescriptor(VkImageView depth_view)
{
    VkDescriptorBufferInfo ssbo = {particle_buffer_, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo ubo = {view_ubo_, 0, sizeof(ViewParamsBlock)};
    VkDescriptorImageInfo depth = {};
    depth.imageView = depth_view;
    depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    std::array<VkWriteDescriptorSet, 3> writes = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = gfx_set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &ssbo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = gfx_set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &ubo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = gfx_set_;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &depth;
    vkUpdateDescriptorSets(vulkan_context_->GetDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    bound_depth_view_ = depth_view;
}

VkFramebuffer RainParticleRenderer::GetOrCreateFramebuffer(VkImageView view, std::uint32_t w, std::uint32_t h)
{
    auto it = framebuffer_cache_.find(view);
    if (it != framebuffer_cache_.end())
    {
        return it->second;
    }
    VkFramebufferCreateInfo fb_ci = {};
    fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_ci.renderPass = render_pass_;
    fb_ci.attachmentCount = 1;
    fb_ci.pAttachments = &view;
    fb_ci.width = w;
    fb_ci.height = h;
    fb_ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(vulkan_context_->GetDevice(), &fb_ci, vulkan_context_->GetAllocator(), &fb) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    framebuffer_cache_[view] = fb;
    return fb;
}

void RainParticleRenderer::ClearFramebufferCache()
{
    if (vulkan_context_ == nullptr)
    {
        return;
    }
    for (auto& [view, fb] : framebuffer_cache_)
    {
        if (fb != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(vulkan_context_->GetDevice(), fb, vulkan_context_->GetAllocator());
        }
    }
    framebuffer_cache_.clear();
}

void RainParticleRenderer::Render(
    const Params& params,
    VkAccelerationStructureKHR scene_tlas,
    VkImage target_image,
    VkImageView target_view,
    std::uint32_t width,
    std::uint32_t height,
    VkImage depth_image,
    VkImageView depth_view,
    std::uint32_t depth_width,
    std::uint32_t depth_height)
{
    if (vulkan_context_ == nullptr || target_image == VK_NULL_HANDLE || target_view == VK_NULL_HANDLE ||
        depth_view == VK_NULL_HANDLE || scene_tlas == VK_NULL_HANDLE || width == 0 || height == 0)
    {
        return;
    }
    if (!EnsureResources())
    {
        return;
    }
    if (scene_tlas != bound_tlas_)
    {
        UpdateComputeTlas(scene_tlas);
    }

    // Per-call delta time (clamped to avoid jumps after stalls / pauses).
    float dt = 0.0f;
    const std::uint64_t ticks = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
    const std::uint64_t freq = static_cast<std::uint64_t>(SDL_GetPerformanceFrequency());
    if (last_ticks_ != 0 && ticks > last_ticks_ && freq > 0)
    {
        dt = static_cast<float>(static_cast<double>(ticks - last_ticks_) / static_cast<double>(freq));
    }
    last_ticks_ = ticks;
    dt = std::clamp(dt, 0.0f, 0.1f);
    time_seconds_ += dt;

    const VkDevice device = vulkan_context_->GetDevice();

    // 1-frame-in-flight pacing: wait for the previous submit before recycling.
    if (fence_ != VK_NULL_HANDLE && in_flight_)
    {
        vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &fence_);
        in_flight_ = false;
    }

    // Upload uniforms.
    ComputeParamsBlock cp = {};
    cp.params[0] = dt;
    cp.params[1] = time_seconds_;
    cp.params[2] = kFallSpeedBase * std::max(params.fall_speed, 0.01f);
    cp.params[3] = kMaxFallDist;
    cp.control[0] = particle_count_;
    cp.control[1] = reset_pending_ ? 1u : 0u;
    std::memcpy(cp.plane_to_world, params.plane_to_world.data(), sizeof(cp.plane_to_world));
    std::memcpy(compute_ubo_mapped_, &cp, sizeof(cp));

    ViewParamsBlock vp = {};
    std::memcpy(vp.view_projection, params.view_projection.data(), sizeof(vp.view_projection));
    std::memcpy(vp.plane_to_world, params.plane_to_world.data(), sizeof(vp.plane_to_world));
    vp.cam_pos[0] = params.camera_position[0];
    vp.cam_pos[1] = params.camera_position[1];
    vp.cam_pos[2] = params.camera_position[2];
    vp.color[0] = params.color[0];
    vp.color[1] = params.color[1];
    vp.color[2] = params.color[2];
    vp.color[3] = params.opacity;
    vp.params[0] = kStreakWidthBase * std::max(params.drop_size, 0.05f);
    vp.params[1] = kStreakLengthBase * std::max(params.drop_size, 0.05f);
    vp.params[2] = time_seconds_;
    vp.params[3] = kMaxFallDist;
    vp.depth_size[0] = static_cast<float>(depth_width);
    vp.depth_size[1] = static_cast<float>(depth_height);
    std::memcpy(view_ubo_mapped_, &vp, sizeof(vp));

    reset_pending_ = false;

    if (depth_view != bound_depth_view_)
    {
        UpdateGraphicsDescriptor(depth_view);
    }

    VkFramebuffer framebuffer = GetOrCreateFramebuffer(target_view, width, height);
    if (framebuffer == VK_NULL_HANDLE)
    {
        return;
    }

    if (cmd_ == VK_NULL_HANDLE)
    {
        VkCommandBufferAllocateInfo cmd_ai = {};
        cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmd_ai.commandPool = command_pool_;
        cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd_ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &cmd_ai, &cmd_) != VK_SUCCESS)
        {
            cmd_ = VK_NULL_HANDLE;
            return;
        }
    }

    VkCommandBuffer cmd = cmd_;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Make the ray tracer's depth writes (prior submit) visible to our reads.
    if (depth_image != VK_NULL_HANDLE)
    {
        VkImageMemoryBarrier depth_barrier = {};
        depth_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        depth_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        depth_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depth_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depth_barrier.image = depth_image;
        depth_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        depth_barrier.subresourceRange.levelCount = 1;
        depth_barrier.subresourceRange.layerCount = 1;
        depth_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        depth_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &depth_barrier);
    }

    // The collision ray query reads the TLAS built by a prior submit; make that
    // build visible to this compute dispatch.
    VkMemoryBarrier tlas_barrier = {};
    tlas_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    tlas_barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    tlas_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &tlas_barrier, 0, nullptr, 0, nullptr);

    // --- Simulation ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_pipeline_layout_, 0, 1, &compute_set_, 0, nullptr);
    const std::uint32_t groups = (particle_count_ + 255u) / 256u;
    vkCmdDispatch(cmd, groups, 1, 1);

    // Compute write -> vertex read of the particle SSBO.
    VkBufferMemoryBarrier ssbo_barrier = {};
    ssbo_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    ssbo_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ssbo_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ssbo_barrier.buffer = particle_buffer_;
    ssbo_barrier.offset = 0;
    ssbo_barrier.size = VK_WHOLE_SIZE;
    ssbo_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    ssbo_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0, 0, nullptr, 1, &ssbo_barrier, 0, nullptr);

    // --- Composite the streaks over the RT image ---
    TransitionImage(cmd, target_image,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    VkRenderPassBeginInfo rp_begin = {};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass_;
    rp_begin.framebuffer = framebuffer;
    rp_begin.renderArea.extent.width = width;
    rp_begin.renderArea.extent.height = height;
    vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gfx_pipeline_layout_, 0, 1, &gfx_set_, 0, nullptr);

    VkViewport viewport = {};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {};
    scissor.extent.width = width;
    scissor.extent.height = height;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // 4 vertices (triangle-strip quad) per particle instance.
    vkCmdDraw(cmd, 4, particle_count_, 0, 0);

    vkCmdEndRenderPass(cmd);

    TransitionImage(cmd, target_image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (vkQueueSubmit(vulkan_context_->GetQueue(), 1, &submit, fence_) == VK_SUCCESS)
    {
        in_flight_ = true;
    }
}
