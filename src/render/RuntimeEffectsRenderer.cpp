#include "render/RuntimeEffectsRenderer.h"

#include "vfs/AssetVFS.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_set>
#include <vector>

namespace
{
constexpr VkFormat kRuntimeEffectFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr float kPi = 3.1415926535f;

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 Add(const Vec3& left, const Vec3& right)
{
    return {left.x + right.x, left.y + right.y, left.z + right.z};
}

Vec3 Subtract(const Vec3& left, const Vec3& right)
{
    return {left.x - right.x, left.y - right.y, left.z - right.z};
}

Vec3 Scale(const Vec3& value, float scalar)
{
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

float Dot(const Vec3& left, const Vec3& right)
{
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

float Length(const Vec3& value)
{
    return std::sqrt(Dot(value, value));
}

Vec3 Normalize(const Vec3& value)
{
    const float length = Length(value);
    if (length <= 0.0001f)
    {
        return {0.0f, 0.0f, -1.0f};
    }
    return Scale(value, 1.0f / length);
}

Vec3 CameraPositionFromMatrix(const std::array<float, 16>& matrix)
{
    return {matrix[12], matrix[13], matrix[14]};
}

Vec3 CameraForwardFromMatrix(const std::array<float, 16>& matrix)
{
    return Normalize({-matrix[8], -matrix[9], -matrix[10]});
}

Vec3 CameraUpFromMatrix(const std::array<float, 16>& matrix)
{
    return Normalize({matrix[4], matrix[5], matrix[6]});
}

std::uint32_t FindMemoryType(VkPhysicalDevice physical_device, std::uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
    {
        const bool type_matches = (type_filter & (1u << i)) != 0;
        const bool props_match = (memory_properties.memoryTypes[i].propertyFlags & properties) == properties;
        if (type_matches && props_match)
            return i;
    }
    return UINT32_MAX;
}

std::filesystem::path ResolveEffectShaderPath(const char* file_name)
{
    const char* base_path_raw = SDL_GetBasePath();
    const std::filesystem::path base_path =
        base_path_raw != nullptr ? std::filesystem::path(base_path_raw) : std::filesystem::current_path();
    return base_path / "shaders" / file_name;
}

VkShaderModule LoadEffectShaderModule(VkDevice device, const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        SDL_Log("RuntimeEffectsRenderer: failed to read shader: %s", path.string().c_str());
        return VK_NULL_HANDLE;
    }
    std::vector<std::uint8_t> bytes;
    bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (bytes.empty())
        return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes.size();
    ci.pCode = reinterpret_cast<const std::uint32_t*>(bytes.data());
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(device, &ci, nullptr, &mod);
    return mod;
}

void TransitionImage(
    VkCommandBuffer command_buffer,
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
    vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

std::string FromEffekseerPath(const char16_t* path)
{
    if (path == nullptr || path[0] == u'\0')
    {
        return {};
    }
    return std::filesystem::path(std::u16string(path)).generic_string();
}

std::vector<std::uint8_t> ReadDiskFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

#ifdef GAMMA_WITH_EFFEKSEER
class RuntimeEffectFileReader final : public Effekseer::FileReader
{
public:
    explicit RuntimeEffectFileReader(std::vector<std::uint8_t> bytes)
        : bytes_(std::move(bytes))
    {
    }

    size_t Read(void* buffer, size_t size) override
    {
        if (buffer == nullptr || size == 0 || position_ >= bytes_.size())
        {
            return 0;
        }

        const size_t remaining = bytes_.size() - position_;
        const size_t read_size = (std::min)(size, remaining);
        std::memcpy(buffer, bytes_.data() + position_, read_size);
        position_ += read_size;
        return read_size;
    }

    void Seek(int position) override
    {
        position_ = static_cast<size_t>((std::max)(0, position));
        position_ = (std::min)(position_, bytes_.size());
    }

    int GetPosition() const override
    {
        return static_cast<int>(position_);
    }

    size_t GetLength() const override
    {
        return bytes_.size();
    }

private:
    std::vector<std::uint8_t> bytes_;
    size_t position_ = 0;
};

class RuntimeEffectFileInterface final : public Effekseer::FileInterface
{
public:
    Effekseer::FileReaderRef OpenRead(const char16_t* path) override
    {
        const std::string generic_path = FromEffekseerPath(path);
        if (generic_path.empty())
        {
            return nullptr;
        }

        std::vector<std::uint8_t> bytes;
        if (g_asset_reader)
        {
            bytes = g_asset_reader->ReadFile(generic_path);
        }
        if (bytes.empty())
        {
            bytes = ReadDiskFile(std::filesystem::path(std::u16string(path)));
        }
        if (bytes.empty())
        {
            return nullptr;
        }
        return Effekseer::MakeRefPtr<RuntimeEffectFileReader>(std::move(bytes));
    }

    Effekseer::FileWriterRef OpenWrite(const char16_t*) override
    {
        return nullptr;
    }
};

Effekseer::Matrix43 ToEffekseerMatrix43(const std::array<float, 16>& matrix)
{
    Effekseer::Matrix43 out;
    out.Value[0][0] = matrix[0];
    out.Value[0][1] = matrix[1];
    out.Value[0][2] = matrix[2];
    out.Value[1][0] = matrix[4];
    out.Value[1][1] = matrix[5];
    out.Value[1][2] = matrix[6];
    out.Value[2][0] = matrix[8];
    out.Value[2][1] = matrix[9];
    out.Value[2][2] = matrix[10];
    out.Value[3][0] = matrix[12];
    out.Value[3][1] = matrix[13];
    out.Value[3][2] = matrix[14];
    return out;
}
#endif
} // namespace

RuntimeEffectsRenderer::~RuntimeEffectsRenderer()
{
    Shutdown();
}

bool RuntimeEffectsRenderer::IsAvailable() const
{
#ifdef GAMMA_WITH_EFFEKSEER
    return true;
#else
    return false;
#endif
}

bool RuntimeEffectsRenderer::Initialize(VulkanContext* context)
{
    if (context_ == context && command_pool_ != VK_NULL_HANDLE)
    {
        return true;
    }

    Shutdown();
    context_ = context;
    return EnsureInitialized(nullptr);
}

void RuntimeEffectsRenderer::Shutdown()
{
    DestroyRenderTarget();
    DestroyDepthFill();
    DestroyEffekseer();
    context_ = nullptr;
    last_update_ticks_ = 0;
}

void RuntimeEffectsRenderer::ResetPlayback()
{
#ifdef GAMMA_WITH_EFFEKSEER
    if (manager_)
    {
        for (auto& [key, active] : active_effects_)
        {
            (void)key;
            if (active.handle >= 0)
            {
                manager_->StopEffect(active.handle);
            }
            active.handle = -1;
            active.play_once_started = false;
        }
    }
#endif
    last_update_ticks_ = 0;
}

bool RuntimeEffectsRenderer::RenderEffects(
    VkImage target_image,
    VkImageView target_view,
    VkImageLayout current_layout,
    std::uint32_t target_width,
    std::uint32_t target_height,
    VkImage scene_depth_image,
    VkImageView scene_depth_view,
    const std::array<float, 16>& camera_world_matrix,
    const SceneObjectCameraAttributes& camera,
    const std::vector<QueuedEffect>& effects,
    VkImageLayout& out_layout,
    std::string* error_message)
{
    out_layout = current_layout;

#ifndef GAMMA_WITH_EFFEKSEER
    (void)target_image;
    (void)target_view;
    (void)target_width;
    (void)target_height;
    (void)scene_depth_image;
    (void)scene_depth_view;
    (void)camera_world_matrix;
    (void)camera;
    (void)effects;
    SetError(error_message, "Effekseer support is not enabled in this build");
    return false;
#else
    if (effects.empty())
    {
        StopMissingEffects(effects);
        return true;
    }

    if (context_ == nullptr || target_image == VK_NULL_HANDLE || target_view == VK_NULL_HANDLE || target_width == 0 || target_height == 0)
    {
        return true;
    }

    if (!EnsureInitialized(error_message) || !EnsureRenderTarget(target_view, target_width, target_height, error_message))
    {
        return false;
    }

    StopMissingEffects(effects);

    const std::uint64_t now_ticks = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
    const std::uint64_t frequency = static_cast<std::uint64_t>(SDL_GetPerformanceFrequency());
    float delta_seconds = 1.0f / 60.0f;
    if (last_update_ticks_ != 0 && now_ticks > last_update_ticks_ && frequency > 0)
    {
        delta_seconds = static_cast<float>(static_cast<double>(now_ticks - last_update_ticks_) / static_cast<double>(frequency));
        delta_seconds = std::clamp(delta_seconds, 0.0f, 1.0f / 15.0f);
    }
    last_update_ticks_ = now_ticks;

    bool any_playing = false;
    for (const QueuedEffect& queued : effects)
    {
        if (queued.effect_path.empty())
        {
            continue;
        }

        ActiveEffect& active = active_effects_[queued.key];
        const bool changed = active.path != queued.effect_path || active.play_mode != queued.play_mode;
        if (changed && active.handle >= 0)
        {
            manager_->StopEffect(active.handle);
            active.handle = -1;
            active.play_once_started = false;
        }
        active.path = queued.effect_path;
        active.play_mode = queued.play_mode;

        if (queued.play_mode == SceneObjectEffectsPlayMode::Stop)
        {
            if (active.handle >= 0)
            {
                manager_->StopEffect(active.handle);
                active.handle = -1;
            }
            continue;
        }

        if (active.handle >= 0 && !manager_->Exists(active.handle))
        {
            active.handle = -1;
        }

        const bool should_start_loop = queued.play_mode == SceneObjectEffectsPlayMode::Loop && active.handle < 0;
        const bool should_start_once = queued.play_mode == SceneObjectEffectsPlayMode::PlayOnce && active.handle < 0 && !active.play_once_started;
        if (should_start_loop || should_start_once)
        {
            Effekseer::EffectRef effect;
            if (!LoadEffect(queued.effect_path, effect, error_message))
            {
                continue;
            }
            active.handle = manager_->Play(effect, 0.0f, 0.0f, 0.0f);
            active.play_once_started = true;
        }

        if (active.handle >= 0)
        {
            manager_->SetMatrix(active.handle, ToEffekseerMatrix43(queued.world_matrix));
            any_playing = true;
        }
    }

    Effekseer::Manager::LayerParameter layer_parameter;
    const Vec3 viewer_position = CameraPositionFromMatrix(camera_world_matrix);
    layer_parameter.ViewerPosition = Effekseer::Vector3D(viewer_position.x, viewer_position.y, viewer_position.z);
    manager_->SetLayerParameter(0, layer_parameter);

    if (any_playing && delta_seconds > 0.0f)
    {
        Effekseer::Manager::UpdateParameter update_parameter;
        update_parameter.DeltaFrame = delta_seconds * 60.0f;
        update_parameter.UpdateInterval = 1.0f;
        update_parameter.SyncUpdate = true;
        manager_->Update(update_parameter);
    }

    memory_pool_->NewFrame();
    const VkDevice device = context_->GetDevice();
    if (render_fence_ != VK_NULL_HANDLE)
    {
        vkWaitForFences(device, 1, &render_fence_, VK_TRUE, UINT64_MAX);
        vkResetFences(device, 1, &render_fence_);
    }

    VkResult result = vkResetCommandBuffer(command_buffer_, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to reset Effekseer runtime command buffer");
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_buffer_, &begin_info);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to begin Effekseer runtime command buffer");
        return false;
    }

    const VkPipelineStageFlags source_stage = current_layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
        : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkAccessFlags source_access = current_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
    TransitionImage(
        command_buffer_,
        target_image,
        current_layout,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        source_stage,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        source_access,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    // If scene linear depth is available, add a memory barrier so the fragment
    // shader can safely read it from the ray-tracing / TAA compute passes.
    if (scene_depth_image != VK_NULL_HANDLE && scene_depth_view != VK_NULL_HANDLE)
    {
        VkImageMemoryBarrier scene_depth_barrier = {};
        scene_depth_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        scene_depth_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        scene_depth_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        scene_depth_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        scene_depth_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        scene_depth_barrier.image = scene_depth_image;
        scene_depth_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        scene_depth_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        scene_depth_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(
            command_buffer_,
            VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &scene_depth_barrier);
    }

    // On first use after (re)creation, transition the hardware depth buffer from
    // UNDEFINED to DEPTH_STENCIL_ATTACHMENT_OPTIMAL so both passes can use it.
    if (depth_image_layout_ == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        VkImageMemoryBarrier init_barrier = {};
        init_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        init_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        init_barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        init_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        init_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        init_barrier.image = depth_image_;
        init_barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        init_barrier.srcAccessMask = 0;
        init_barrier.dstAccessMask =
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(
            command_buffer_,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &init_barrier);
        depth_image_layout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    // Depth fill pass: clears hardware depth to 1.0, then (if scene depth is
    // available) overwrites it with NDC depth derived from the ray-tracer's
    // linear hit-distance image so that effects are occluded by scene geometry.
    if (depth_fill_rp_ != VK_NULL_HANDLE && depth_fill_fb_ != VK_NULL_HANDLE)
    {
        VkClearValue fill_clear = {};
        fill_clear.depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo fill_begin = {};
        fill_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        fill_begin.renderPass = depth_fill_rp_;
        fill_begin.framebuffer = depth_fill_fb_;
        fill_begin.renderArea.extent.width = target_width;
        fill_begin.renderArea.extent.height = target_height;
        fill_begin.clearValueCount = 1;
        fill_begin.pClearValues = &fill_clear;
        vkCmdBeginRenderPass(command_buffer_, &fill_begin, VK_SUBPASS_CONTENTS_INLINE);

        if (scene_depth_image != VK_NULL_HANDLE && scene_depth_view != VK_NULL_HANDLE
            && depth_fill_pipeline_ != VK_NULL_HANDLE && depth_fill_set_ != VK_NULL_HANDLE)
        {
            // Bind the scene linear depth image as a storage image.
            VkDescriptorImageInfo img_info = {};
            img_info.imageView = scene_depth_view;
            img_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet write = {};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = depth_fill_set_;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            write.descriptorCount = 1;
            write.pImageInfo = &img_info;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

            VkViewport fill_vp = {};
            fill_vp.x = 0.0f;
            fill_vp.y = 0.0f;
            fill_vp.width = static_cast<float>(target_width);
            fill_vp.height = static_cast<float>(target_height);
            fill_vp.minDepth = 0.0f;
            fill_vp.maxDepth = 1.0f;
            vkCmdSetViewport(command_buffer_, 0, 1, &fill_vp);

            VkRect2D fill_scissor = {};
            fill_scissor.extent.width = target_width;
            fill_scissor.extent.height = target_height;
            vkCmdSetScissor(command_buffer_, 0, 1, &fill_scissor);

            vkCmdBindPipeline(command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_fill_pipeline_);
            vkCmdBindDescriptorSets(
                command_buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                depth_fill_pipeline_layout_, 0, 1, &depth_fill_set_, 0, nullptr);

            const float push_constants[2] = {
                std::max(camera.near_clip, 0.001f),
                std::max(camera.far_clip, camera.near_clip + 0.001f)};
            vkCmdPushConstants(
                command_buffer_, depth_fill_pipeline_layout_,
                VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), push_constants);

            vkCmdDraw(command_buffer_, 3, 1, 0, 0); // full-screen triangle
        }

        vkCmdEndRenderPass(command_buffer_);
    }

    EffekseerRendererVulkan::BeginCommandList(efk_command_list_, command_buffer_);
    renderer_->SetCommandList(efk_command_list_);

    // Clear value index 0 = color (ignored, LOAD_OP_LOAD); depth is also LOAD so no clear needed.
    VkRenderPassBeginInfo render_pass_begin = {};
    render_pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin.renderPass = render_pass_;
    render_pass_begin.framebuffer = framebuffer_;
    render_pass_begin.renderArea.extent.width = target_width;
    render_pass_begin.renderArea.extent.height = target_height;
    render_pass_begin.clearValueCount = 0;
    render_pass_begin.pClearValues = nullptr;
    vkCmdBeginRenderPass(command_buffer_, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);

    // Flip the Vulkan viewport Y so Effekseer's right-handed projection
    // (Y-up in NDC) matches the scene: NDC y>0 → top, y<0 → bottom.
    // Without this, world Y and screen Y are inverted for effects.
    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = static_cast<float>(target_height);
    viewport.width = static_cast<float>(target_width);
    viewport.height = -static_cast<float>(target_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffer_, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.extent.width = target_width;
    scissor.extent.height = target_height;
    vkCmdSetScissor(command_buffer_, 0, 1, &scissor);

    const float aspect = static_cast<float>(target_width) / static_cast<float>((std::max)(1u, target_height));
    const Vec3 eye = CameraPositionFromMatrix(camera_world_matrix);
    const Vec3 forward = CameraForwardFromMatrix(camera_world_matrix);
    Vec3 up = CameraUpFromMatrix(camera_world_matrix);
    if (std::abs(Dot(forward, up)) >= 0.999f)
    {
        up = {0.0f, 1.0f, 0.0f};
    }

    Effekseer::Matrix44 projection_matrix;
    projection_matrix.PerspectiveFovRH(std::clamp(camera.field_of_view_degrees, 1.0f, 179.0f) / 180.0f * kPi, aspect, (std::max)(camera.near_clip, 0.001f), (std::max)(camera.far_clip, camera.near_clip + 0.001f));

    Effekseer::Matrix44 camera_matrix;
    camera_matrix.LookAtRH(
        Effekseer::Vector3D(eye.x, eye.y, eye.z),
        Effekseer::Vector3D(eye.x + forward.x, eye.y + forward.y, eye.z + forward.z),
        Effekseer::Vector3D(up.x, up.y, up.z));

    renderer_->SetTime(static_cast<float>(now_ticks) / static_cast<float>((std::max)(std::uint64_t{1}, frequency)));
    renderer_->SetProjectionMatrix(projection_matrix);
    renderer_->SetCameraMatrix(camera_matrix);

    if (renderer_->BeginRendering())
    {
        Effekseer::Manager::DrawParameter draw_parameter;
        draw_parameter.ZNear = (std::max)(camera.near_clip, 0.001f);
        draw_parameter.ZFar = (std::max)(camera.far_clip, camera.near_clip + 0.001f);
        draw_parameter.ViewProjectionMatrix = renderer_->GetCameraProjectionMatrix();
        draw_parameter.CameraPosition = Effekseer::Vector3D(eye.x, eye.y, eye.z);
        draw_parameter.CameraFrontDirection = Effekseer::Vector3D(forward.x, forward.y, forward.z);
        draw_parameter.CameraCullingMask = 0x7fffffff;
        draw_parameter.IsSortingEffectsEnabled = true;
        manager_->Draw(draw_parameter);
        renderer_->EndRendering();
    }

    vkCmdEndRenderPass(command_buffer_);

    renderer_->SetCommandList(nullptr);
    EffekseerRendererVulkan::EndCommandList(efk_command_list_);

    TransitionImage(
        command_buffer_,
        target_image,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT);

    result = vkEndCommandBuffer(command_buffer_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to finish Effekseer runtime command buffer");
        return false;
    }

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer_;
    result = vkQueueSubmit(context_->GetQueue(), 1, &submit_info, render_fence_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to submit Effekseer runtime commands");
        return false;
    }

    out_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
#endif
}

bool RuntimeEffectsRenderer::EnsureInitialized(std::string* error_message)
{
    if (context_ == nullptr)
    {
        return false;
    }
    if (command_pool_ != VK_NULL_HANDLE)
    {
        return true;
    }

    const VkDevice device = context_->GetDevice();
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = context_->GetQueueFamily();
    VkResult result = vkCreateCommandPool(device, &pool_info, context_->GetAllocator(), &command_pool_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to create Effekseer runtime command pool");
        return false;
    }

    VkCommandBufferAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate_info.commandPool = command_pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &allocate_info, &command_buffer_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to allocate Effekseer runtime command buffer");
        return false;
    }

    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    result = vkCreateFence(device, &fence_info, context_->GetAllocator(), &render_fence_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to create Effekseer runtime fence");
        return false;
    }

#ifdef GAMMA_WITH_EFFEKSEER
    graphics_device_ = EffekseerRendererVulkan::CreateGraphicsDevice(
        context_->GetPhysicalDevice(),
        context_->GetDevice(),
        context_->GetQueue(),
        command_pool_,
        2);
    if (!graphics_device_)
    {
        SetError(error_message, "Failed to create Effekseer runtime graphics device");
        return false;
    }

    EffekseerRendererVulkan::RenderPassInformation render_pass_info = {};
    render_pass_info.DoesPresentToScreen = false;
    render_pass_info.RenderTextureCount = 1;
    render_pass_info.RenderTextureFormats[0] = kRuntimeEffectFormat;
    render_pass_info.DepthFormat = VK_FORMAT_D32_SFLOAT;
    renderer_ = EffekseerRendererVulkan::Create(graphics_device_, render_pass_info, 8000);
    if (!renderer_)
    {
        SetError(error_message, "Failed to create Effekseer runtime renderer");
        return false;
    }

    memory_pool_ = EffekseerRenderer::CreateSingleFrameMemoryPool(renderer_->GetGraphicsDevice());
    efk_command_list_ = EffekseerRenderer::CreateCommandList(renderer_->GetGraphicsDevice(), memory_pool_);
    manager_ = Effekseer::Manager::Create(8000);
    file_interface_ = Effekseer::MakeRefPtr<RuntimeEffectFileInterface>();
    if (!memory_pool_ || !efk_command_list_ || !manager_ || !file_interface_)
    {
        SetError(error_message, "Failed to create Effekseer runtime objects");
        return false;
    }

    manager_->SetSpriteRenderer(renderer_->CreateSpriteRenderer());
    manager_->SetRibbonRenderer(renderer_->CreateRibbonRenderer());
    manager_->SetRingRenderer(renderer_->CreateRingRenderer());
    manager_->SetTrackRenderer(renderer_->CreateTrackRenderer());
    manager_->SetModelRenderer(renderer_->CreateModelRenderer());
    manager_->SetEffectLoader(Effekseer::Effect::CreateEffectLoader(file_interface_));
    manager_->SetTextureLoader(renderer_->CreateTextureLoader(file_interface_));
    manager_->SetModelLoader(renderer_->CreateModelLoader(file_interface_));
    manager_->SetMaterialLoader(renderer_->CreateMaterialLoader(file_interface_));
    manager_->SetCurveLoader(Effekseer::MakeRefPtr<Effekseer::CurveLoader>(file_interface_));

    // --- Depth fill pass pipeline ---
    // Depth-only render pass (CLEAR → DEPTH_STENCIL_ATTACHMENT_OPTIMAL).
    {
        VkAttachmentDescription fill_depth_attach = {};
        fill_depth_attach.format = VK_FORMAT_D32_SFLOAT;
        fill_depth_attach.samples = VK_SAMPLE_COUNT_1_BIT;
        fill_depth_attach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        fill_depth_attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        fill_depth_attach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        fill_depth_attach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        fill_depth_attach.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        fill_depth_attach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference fill_depth_ref = {};
        fill_depth_ref.attachment = 0;
        fill_depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription fill_subpass = {};
        fill_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        fill_subpass.pDepthStencilAttachment = &fill_depth_ref;

        VkRenderPassCreateInfo fill_rp_info = {};
        fill_rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        fill_rp_info.attachmentCount = 1;
        fill_rp_info.pAttachments = &fill_depth_attach;
        fill_rp_info.subpassCount = 1;
        fill_rp_info.pSubpasses = &fill_subpass;
        vkCreateRenderPass(device, &fill_rp_info, context_->GetAllocator(), &depth_fill_rp_);
    }

    // Descriptor set layout: binding 0 = r32f storage image (scene linear depth).
    {
        VkDescriptorSetLayoutBinding dsl_binding = {};
        dsl_binding.binding = 0;
        dsl_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        dsl_binding.descriptorCount = 1;
        dsl_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dsl_info = {};
        dsl_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl_info.bindingCount = 1;
        dsl_info.pBindings = &dsl_binding;
        vkCreateDescriptorSetLayout(device, &dsl_info, context_->GetAllocator(), &depth_fill_dsl_);
    }

    // Pipeline layout: one descriptor set + push constants (z_near, z_far).
    if (depth_fill_dsl_ != VK_NULL_HANDLE)
    {
        VkPushConstantRange pc_range = {};
        pc_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc_range.size = sizeof(float) * 2;

        VkPipelineLayoutCreateInfo pl_info = {};
        pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl_info.setLayoutCount = 1;
        pl_info.pSetLayouts = &depth_fill_dsl_;
        pl_info.pushConstantRangeCount = 1;
        pl_info.pPushConstantRanges = &pc_range;
        vkCreatePipelineLayout(device, &pl_info, context_->GetAllocator(), &depth_fill_pipeline_layout_);
    }

    // Graphics pipeline: full-screen triangle writes NDC depth from scene linear depth.
    if (depth_fill_rp_ != VK_NULL_HANDLE && depth_fill_pipeline_layout_ != VK_NULL_HANDLE)
    {
        VkShaderModule vert_mod = LoadEffectShaderModule(device, ResolveEffectShaderPath("effects_depth.vert.spv"));
        VkShaderModule frag_mod = LoadEffectShaderModule(device, ResolveEffectShaderPath("effects_depth.frag.spv"));
        if (vert_mod != VK_NULL_HANDLE && frag_mod != VK_NULL_HANDLE)
        {
            VkPipelineShaderStageCreateInfo stages[2] = {};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vert_mod;
            stages[0].pName = "main";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = frag_mod;
            stages[1].pName = "main";

            VkPipelineVertexInputStateCreateInfo vi = {};
            vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineInputAssemblyStateCreateInfo ia = {};
            ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo vs = {};
            vs.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            vs.viewportCount = 1;
            vs.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rs = {};
            rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo ms = {};
            ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo ds = {};
            ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            ds.depthTestEnable = VK_TRUE;
            ds.depthWriteEnable = VK_TRUE;
            ds.depthCompareOp = VK_COMPARE_OP_ALWAYS; // always write gl_FragDepth

            VkPipelineColorBlendStateCreateInfo cb = {};
            cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            // no color attachments in the depth-only pass

            VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dy = {};
            dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dy.dynamicStateCount = 2;
            dy.pDynamicStates = dynamic_states;

            VkGraphicsPipelineCreateInfo pci = {};
            pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pci.stageCount = 2;
            pci.pStages = stages;
            pci.pVertexInputState = &vi;
            pci.pInputAssemblyState = &ia;
            pci.pViewportState = &vs;
            pci.pRasterizationState = &rs;
            pci.pMultisampleState = &ms;
            pci.pDepthStencilState = &ds;
            pci.pColorBlendState = &cb;
            pci.pDynamicState = &dy;
            pci.layout = depth_fill_pipeline_layout_;
            pci.renderPass = depth_fill_rp_;
            vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, context_->GetAllocator(), &depth_fill_pipeline_);
        }
        if (vert_mod != VK_NULL_HANDLE) { vkDestroyShaderModule(device, vert_mod, context_->GetAllocator()); }
        if (frag_mod != VK_NULL_HANDLE) { vkDestroyShaderModule(device, frag_mod, context_->GetAllocator()); }
    }

    // Descriptor pool and set for the fill pass.
    if (depth_fill_dsl_ != VK_NULL_HANDLE)
    {
        VkDescriptorPoolSize pool_size = {};
        pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        pool_size.descriptorCount = 1;

        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        vkCreateDescriptorPool(device, &pool_info, context_->GetAllocator(), &depth_fill_pool_);

        if (depth_fill_pool_ != VK_NULL_HANDLE)
        {
            VkDescriptorSetAllocateInfo alloc_info = {};
            alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            alloc_info.descriptorPool = depth_fill_pool_;
            alloc_info.descriptorSetCount = 1;
            alloc_info.pSetLayouts = &depth_fill_dsl_;
            vkAllocateDescriptorSets(device, &alloc_info, &depth_fill_set_);
        }
    }
#else
    (void)error_message;
#endif

    return true;
}

bool RuntimeEffectsRenderer::EnsureRenderTarget(VkImageView target_view, std::uint32_t target_width, std::uint32_t target_height, std::string* error_message)
{
    if (framebuffer_ != VK_NULL_HANDLE && framebuffer_view_ == target_view && target_width_ == target_width && target_height_ == target_height)
    {
        return true;
    }

    DestroyRenderTarget();

    // --- depth image ---
    VkImageCreateInfo depth_image_info = {};
    depth_image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_image_info.imageType = VK_IMAGE_TYPE_2D;
    depth_image_info.format = VK_FORMAT_D32_SFLOAT;
    depth_image_info.extent = {target_width, target_height, 1};
    depth_image_info.mipLevels = 1;
    depth_image_info.arrayLayers = 1;
    depth_image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depth_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    const VkDevice device = context_->GetDevice();
    VkResult result = vkCreateImage(device, &depth_image_info, context_->GetAllocator(), &depth_image_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to create Effekseer runtime depth image");
        return false;
    }

    VkMemoryRequirements depth_mem_reqs = {};
    vkGetImageMemoryRequirements(device, depth_image_, &depth_mem_reqs);
    VkMemoryAllocateInfo depth_alloc_info = {};
    depth_alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    depth_alloc_info.allocationSize = depth_mem_reqs.size;
    depth_alloc_info.memoryTypeIndex = FindMemoryType(
        context_->GetPhysicalDevice(), depth_mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    result = vkAllocateMemory(device, &depth_alloc_info, context_->GetAllocator(), &depth_memory_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to allocate Effekseer runtime depth memory");
        return false;
    }
    vkBindImageMemory(device, depth_image_, depth_memory_, 0);

    VkImageViewCreateInfo depth_view_info = {};
    depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_info.image = depth_image_;
    depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_info.format = VK_FORMAT_D32_SFLOAT;
    depth_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_view_info.subresourceRange.levelCount = 1;
    depth_view_info.subresourceRange.layerCount = 1;
    result = vkCreateImageView(device, &depth_view_info, context_->GetAllocator(), &depth_view_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to create Effekseer runtime depth view");
        return false;
    }

    // --- render pass ---
    VkAttachmentDescription color_attachment = {};
    color_attachment.format = kRuntimeEffectFormat;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depth_attachment = {};
    depth_attachment.format = VK_FORMAT_D32_SFLOAT;
    depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;     // depth pre-filled by fill pass
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_reference = {};
    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_reference = {};
    depth_reference.attachment = 1;
    depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;

    const VkAttachmentDescription render_pass_attachments[2] = {color_attachment, depth_attachment};
    VkRenderPassCreateInfo render_pass_info = {};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 2;
    render_pass_info.pAttachments = render_pass_attachments;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;

    result = vkCreateRenderPass(device, &render_pass_info, context_->GetAllocator(), &render_pass_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to create Effekseer runtime render pass");
        return false;
    }

    const VkImageView fb_views[2] = {target_view, depth_view_};
    VkFramebufferCreateInfo framebuffer_info = {};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass_;
    framebuffer_info.attachmentCount = 2;
    framebuffer_info.pAttachments = fb_views;
    framebuffer_info.width = target_width;
    framebuffer_info.height = target_height;
    framebuffer_info.layers = 1;

    result = vkCreateFramebuffer(device, &framebuffer_info, context_->GetAllocator(), &framebuffer_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError(error_message, "Failed to create Effekseer runtime framebuffer");
        DestroyRenderTarget();
        return false;
    }

    // Depth fill framebuffer (depth-only, used by the fill pass).
    if (depth_fill_rp_ != VK_NULL_HANDLE)
    {
        VkFramebufferCreateInfo fill_fb_info = {};
        fill_fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fill_fb_info.renderPass = depth_fill_rp_;
        fill_fb_info.attachmentCount = 1;
        fill_fb_info.pAttachments = &depth_view_;
        fill_fb_info.width = target_width;
        fill_fb_info.height = target_height;
        fill_fb_info.layers = 1;
        // Ignore creation failure — fill pass is optional.
        vkCreateFramebuffer(device, &fill_fb_info, context_->GetAllocator(), &depth_fill_fb_);
    }

    depth_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED; // will be transitioned on first RenderEffects call

    framebuffer_view_ = target_view;
    target_width_ = target_width;
    target_height_ = target_height;
    return true;
}

#ifdef GAMMA_WITH_EFFEKSEER
bool RuntimeEffectsRenderer::LoadEffect(const std::filesystem::path& effect_path, Effekseer::EffectRef& out_effect, std::string* error_message)
{
    const std::string key = effect_path.generic_string();
    const auto found = effect_cache_.find(key);
    if (found != effect_cache_.end())
    {
        out_effect = found->second;
        return out_effect != nullptr;
    }

    if (!g_asset_reader)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(effect_path, error))
        {
            SetError(error_message, "Effect file does not exist: " + effect_path.generic_string());
            return false;
        }
    }

    const std::u16string path16 = effect_path.u16string();
    Effekseer::EffectRef effect = Effekseer::Effect::Create(manager_, reinterpret_cast<const char16_t*>(path16.c_str()));
    if (!effect)
    {
        SetError(error_message, "Effekseer failed to load runtime effect: " + effect_path.generic_string());
        return false;
    }

    effect_cache_[key] = effect;
    out_effect = effect;
    return true;
}
#endif

void RuntimeEffectsRenderer::StopMissingEffects(const std::vector<QueuedEffect>& effects)
{
#ifdef GAMMA_WITH_EFFEKSEER
    std::unordered_set<std::string> live_keys;
    live_keys.reserve(effects.size());
    for (const QueuedEffect& effect : effects)
    {
        live_keys.insert(effect.key);
    }

    for (auto it = active_effects_.begin(); it != active_effects_.end();)
    {
        if (live_keys.find(it->first) != live_keys.end())
        {
            ++it;
            continue;
        }
        if (manager_ && it->second.handle >= 0)
        {
            manager_->StopEffect(it->second.handle);
        }
        it = active_effects_.erase(it);
    }
#else
    (void)effects;
#endif
}

void RuntimeEffectsRenderer::DestroyRenderTarget()
{
    if (context_ == nullptr)
    {
        return;
    }
    if (render_fence_ != VK_NULL_HANDLE)
    {
        vkWaitForFences(context_->GetDevice(), 1, &render_fence_, VK_TRUE, UINT64_MAX);
    }
    const VkDevice device = context_->GetDevice();
    if (depth_fill_fb_ != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(device, depth_fill_fb_, context_->GetAllocator());
        depth_fill_fb_ = VK_NULL_HANDLE;
    }
    if (framebuffer_ != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(device, framebuffer_, context_->GetAllocator());
        framebuffer_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(device, render_pass_, context_->GetAllocator());
        render_pass_ = VK_NULL_HANDLE;
    }
    if (depth_view_ != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, depth_view_, context_->GetAllocator());
        depth_view_ = VK_NULL_HANDLE;
    }
    if (depth_image_ != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, depth_image_, context_->GetAllocator());
        depth_image_ = VK_NULL_HANDLE;
    }
    if (depth_memory_ != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, depth_memory_, context_->GetAllocator());
        depth_memory_ = VK_NULL_HANDLE;
    }
    framebuffer_view_ = VK_NULL_HANDLE;
    depth_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    target_width_ = 0;
    target_height_ = 0;
}

void RuntimeEffectsRenderer::DestroyDepthFill()
{
    if (context_ == nullptr)
        return;
    const VkDevice device = context_->GetDevice();
    const VkAllocationCallbacks* allocator = context_->GetAllocator();
    if (depth_fill_pipeline_ != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, depth_fill_pipeline_, allocator);
        depth_fill_pipeline_ = VK_NULL_HANDLE;
    }
    if (depth_fill_pipeline_layout_ != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device, depth_fill_pipeline_layout_, allocator);
        depth_fill_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (depth_fill_pool_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device, depth_fill_pool_, allocator);
        depth_fill_pool_ = VK_NULL_HANDLE;
        depth_fill_set_ = VK_NULL_HANDLE;
    }
    if (depth_fill_dsl_ != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device, depth_fill_dsl_, allocator);
        depth_fill_dsl_ = VK_NULL_HANDLE;
    }
    if (depth_fill_rp_ != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(device, depth_fill_rp_, allocator);
        depth_fill_rp_ = VK_NULL_HANDLE;
    }
}

void RuntimeEffectsRenderer::DestroyEffekseer()
{
    if (context_ != nullptr)
    {
        context_->WaitIdle();
    }

#ifdef GAMMA_WITH_EFFEKSEER
    if (renderer_)
    {
        renderer_->SetCommandList(nullptr);
    }
    active_effects_.clear();
    effect_cache_.clear();
    efk_command_list_.Reset();
    memory_pool_.Reset();
    manager_.Reset();
    renderer_.Reset();
    graphics_device_.Reset();
    file_interface_.Reset();
#endif

    if (context_ != nullptr)
    {
        const VkDevice device = context_->GetDevice();
        if (render_fence_ != VK_NULL_HANDLE)
        {
            vkDestroyFence(device, render_fence_, context_->GetAllocator());
            render_fence_ = VK_NULL_HANDLE;
        }
        if (command_pool_ != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(device, command_pool_, context_->GetAllocator());
            command_pool_ = VK_NULL_HANDLE;
            command_buffer_ = VK_NULL_HANDLE;
        }
    }
}

void RuntimeEffectsRenderer::SetError(std::string* error_message, const std::string& message)
{
    if (error_message != nullptr && error_message->empty())
    {
        *error_message = message;
    }
}