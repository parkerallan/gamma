#include "render/EffectsPreviewRenderer.h"

#include "app/VulkanContext.h"

#include "imgui_impl_vulkan.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
constexpr VkFormat kPreviewFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr std::uint32_t kMaxPreviewDimension = 2048;
constexpr float kPi = 3.1415926535f;
constexpr float kGridNearForward = 0.12f;
constexpr int kGridExtent = 200;

using Vec3 = std::array<float, 3>;

Vec3 Add(const Vec3& lhs, const Vec3& rhs)
{
    return {lhs[0] + rhs[0], lhs[1] + rhs[1], lhs[2] + rhs[2]};
}

Vec3 Subtract(const Vec3& lhs, const Vec3& rhs)
{
    return {lhs[0] - rhs[0], lhs[1] - rhs[1], lhs[2] - rhs[2]};
}

Vec3 Scale(const Vec3& value, float scale)
{
    return {value[0] * scale, value[1] * scale, value[2] * scale};
}

float Dot(const Vec3& lhs, const Vec3& rhs)
{
    return lhs[0] * rhs[0] + lhs[1] * rhs[1] + lhs[2] * rhs[2];
}

Vec3 Cross(const Vec3& lhs, const Vec3& rhs)
{
    return {
        lhs[1] * rhs[2] - lhs[2] * rhs[1],
        lhs[2] * rhs[0] - lhs[0] * rhs[2],
        lhs[0] * rhs[1] - lhs[1] * rhs[0]};
}

Vec3 Normalize(const Vec3& value)
{
    const float length = std::sqrt(Dot(value, value));
    if (length <= 0.00001f)
    {
        return {0.0f, 1.0f, 0.0f};
    }
    return Scale(value, 1.0f / length);
}

struct PreviewCamera
{
    Vec3 eye = {0.0f, 0.0f, 8.0f};
    Vec3 focus = {0.0f, 0.8f, 0.0f};
    Vec3 front = {0.0f, 0.0f, -1.0f};
    Vec3 right = {1.0f, 0.0f, 0.0f};
    Vec3 up = {0.0f, 1.0f, 0.0f};
};

struct CameraPoint
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

PreviewCamera BuildPreviewCamera(float yaw, float pitch, float distance, const Vec3& focus)
{
    const float clamped_pitch = std::clamp(pitch, -1.45f, 1.45f);
    const float cp = std::cos(clamped_pitch);
    const Vec3 orbit_direction = {
        std::sin(yaw) * cp,
        std::sin(clamped_pitch),
        std::cos(yaw) * cp};

    PreviewCamera camera;
    camera.focus = focus;
    camera.eye = Add(focus, Scale(orbit_direction, distance));
    camera.front = Normalize(Subtract(focus, camera.eye));
    camera.right = Normalize(Cross(camera.front, Vec3{0.0f, 1.0f, 0.0f}));
    if (Dot(camera.right, camera.right) <= 0.00001f)
    {
        camera.right = {1.0f, 0.0f, 0.0f};
    }
    camera.up = Normalize(Cross(camera.right, camera.front));
    return camera;
}

CameraPoint ToCameraPoint(const Vec3& world, const PreviewCamera& camera)
{
    const Vec3 relative = Subtract(world, camera.eye);
    return CameraPoint{Dot(relative, camera.right), Dot(relative, camera.up), Dot(relative, camera.front)};
}

CameraPoint LerpCameraPoint(const CameraPoint& a, const CameraPoint& b, float t)
{
    return CameraPoint{
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t};
}

bool ClipCameraSegment(CameraPoint& a, CameraPoint& b, float aspect, float fov_y)
{
    const float tan_y = std::tan(fov_y * 0.5f);
    const float tan_x = tan_y * std::max(0.001f, aspect);
    const CameraPoint original_a = a;
    const CameraPoint original_b = b;
    const CameraPoint delta{original_b.x - original_a.x, original_b.y - original_a.y, original_b.z - original_a.z};
    float t0 = 0.0f;
    float t1 = 1.0f;

    auto clip_half_space = [&](float q, float p) -> bool
    {
        if (std::abs(p) <= 0.000001f)
        {
            return q >= 0.0f;
        }

        const float t = -q / p;
        if (p > 0.0f)
        {
            if (t > t1)
            {
                return false;
            }
            t0 = std::max(t0, t);
        }
        else
        {
            if (t < t0)
            {
                return false;
            }
            t1 = std::min(t1, t);
        }
        return true;
    };

    if (!clip_half_space(original_a.z - kGridNearForward, delta.z) ||
        !clip_half_space(original_a.x + tan_x * original_a.z, delta.x + tan_x * delta.z) ||
        !clip_half_space(-original_a.x + tan_x * original_a.z, -delta.x + tan_x * delta.z) ||
        !clip_half_space(original_a.y + tan_y * original_a.z, delta.y + tan_y * delta.z) ||
        !clip_half_space(-original_a.y + tan_y * original_a.z, -delta.y + tan_y * delta.z))
    {
        return false;
    }

    if (t0 > t1)
    {
        return false;
    }

    a = LerpCameraPoint(original_a, original_b, t0);
    b = LerpCameraPoint(original_a, original_b, t1);
    return a.z > 0.0f && b.z > 0.0f;
}

bool ProjectCameraPoint(
    const CameraPoint& point,
    float aspect,
    float fov_y,
    const ImVec2& region_min,
    const ImVec2& region_max,
    ImVec2& out_screen)
{
    if (point.z <= kGridNearForward)
    {
        return false;
    }

    const float tan_y = std::tan(fov_y * 0.5f);
    const float tan_x = tan_y * std::max(0.001f, aspect);
    const float normalized_x = point.x / (tan_x * point.z);
    const float normalized_y = point.y / (tan_y * point.z);
    out_screen = ImVec2(
        region_min.x + (normalized_x * 0.5f + 0.5f) * (region_max.x - region_min.x),
        region_min.y + (0.5f - normalized_y * 0.5f) * (region_max.y - region_min.y));
    return std::isfinite(out_screen.x) && std::isfinite(out_screen.y);
}

std::uint32_t ClampDimension(std::uint32_t value)
{
    return std::clamp(value, 1u, kMaxPreviewDimension);
}

std::uint32_t FindMemoryType(VkPhysicalDevice physical_device, std::uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);

    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
    {
        const bool type_matches = (type_filter & (1u << i)) != 0;
        const bool properties_match = (memory_properties.memoryTypes[i].propertyFlags & properties) == properties;
        if (type_matches && properties_match)
        {
            return i;
        }
    }

    return UINT32_MAX;
}

bool CreatePreviewImage(
    VulkanContext* context,
    std::uint32_t width,
    std::uint32_t height,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view)
{
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = kPreviewFormat;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    const VkDevice device = context->GetDevice();
    VkResult result = vkCreateImage(device, &image_info, context->GetAllocator(), &image);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(device, image, &requirements);

    const std::uint32_t memory_type = FindMemoryType(
        context->GetPhysicalDevice(),
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type == UINT32_MAX)
    {
        return false;
    }

    VkMemoryAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate_info.allocationSize = requirements.size;
    allocate_info.memoryTypeIndex = memory_type;

    result = vkAllocateMemory(device, &allocate_info, context->GetAllocator(), &memory);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    result = vkBindImageMemory(device, image, memory, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        return false;
    }

    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kPreviewFormat;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    result = vkCreateImageView(device, &view_info, context->GetAllocator(), &view);
    VulkanContext::CheckVkResult(result);
    return result == VK_SUCCESS;
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

std::u16string ToEffekseerPath(const std::filesystem::path& path)
{
    return path.u16string();
}

std::string FromEffekseerPath(const char16_t* path)
{
    if (path == nullptr || path[0] == u'\0')
    {
        return {};
    }
    return std::filesystem::path(std::u16string(path)).generic_string();
}
}

EffectsPreviewRenderer::~EffectsPreviewRenderer()
{
    Clear();
}

bool EffectsPreviewRenderer::IsAvailable() const
{
#ifdef GAMMA_WITH_EFFEKSEER
    return true;
#else
    return false;
#endif
}

ImTextureID EffectsPreviewRenderer::Render(
    VulkanContext* vulkan_context,
    const std::filesystem::path& effect_path,
    std::uint32_t width,
    std::uint32_t height,
    float timeline_seconds,
    bool playing,
    bool paused,
    bool loop)
{
    if (vulkan_context == nullptr || effect_path.empty())
    {
        Stop();
        return ImTextureID{};
    }

#ifndef GAMMA_WITH_EFFEKSEER
    SetError("Effekseer support is not enabled in this build");
    return ImTextureID{};
#else
    width = ClampDimension(width);
    height = ClampDimension(height);

    if (!EnsureInitialized(vulkan_context) || !EnsureRenderTarget(vulkan_context, width, height) || !LoadEffect(effect_path))
    {
        return ImTextureID{};
    }

    AdvanceSimulation(timeline_seconds, playing, paused, loop);
    if (!RenderFrame(vulkan_context, width, height))
    {
        return ImTextureID{};
    }

    return reinterpret_cast<ImTextureID>(descriptor_set_);
#endif
}

void EffectsPreviewRenderer::Stop()
{
#ifdef GAMMA_WITH_EFFEKSEER
    if (manager_ && handle_ >= 0)
    {
        manager_->StopEffect(handle_);
    }
    handle_ = -1;
#endif
    simulated_seconds_ = 0.0f;
    restart_requested_ = false;
    stopped_ = true;
}

void EffectsPreviewRenderer::Restart()
{
    restart_requested_ = true;
    stopped_ = false;
}

void EffectsPreviewRenderer::Clear()
{
    DestroyRenderTarget();
    DestroyEffekseer();
    context_ = nullptr;
}

void EffectsPreviewRenderer::HandleMouseControls(const ImVec2& region_min, const ImVec2& region_max)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsMouseHoveringRect(region_min, region_max) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const float width = std::max(1.0f, region_max.x - region_min.x);
    const float height = std::max(1.0f, region_max.y - region_min.y);

    if (hovered && io.MouseWheel != 0.0f)
    {
        camera_distance_ *= std::exp(-io.MouseWheel * 0.12f);
        camera_distance_ = std::clamp(camera_distance_, 0.75f, 80.0f);
    }

    if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false))
    {
        ResetCamera();
    }

    if (!io.MouseDown[ImGuiMouseButton_Middle])
    {
        middle_mouse_panning_ = false;
    }
    else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    {
        middle_mouse_panning_ = true;
    }

    if (hovered && io.MouseDown[ImGuiMouseButton_Right])
    {
        camera_yaw_ += io.MouseDelta.x * 0.01f;
        camera_pitch_ = std::clamp(camera_pitch_ - io.MouseDelta.y * 0.01f, -1.2f, 1.2f);
    }

    if (middle_mouse_panning_ && io.MouseDown[ImGuiMouseButton_Middle])
    {
        const Vec3 focus = {camera_focus_x_, camera_focus_y_, camera_focus_z_};
        const PreviewCamera camera = BuildPreviewCamera(camera_yaw_, camera_pitch_, camera_distance_, focus);
        const float vertical_world_per_pixel = (2.0f * camera_distance_ * std::tan(45.0f * kPi / 180.0f * 0.5f)) / height;
        const float horizontal_world_per_pixel = vertical_world_per_pixel * (width / height);
        const Vec3 right_delta = Scale(camera.right, -io.MouseDelta.x * horizontal_world_per_pixel);
        const Vec3 up_delta = Scale(camera.up, io.MouseDelta.y * vertical_world_per_pixel);
        const Vec3 new_focus = Add(focus, Add(right_delta, up_delta));
        camera_focus_x_ = new_focus[0];
        camera_focus_y_ = new_focus[1];
        camera_focus_z_ = new_focus[2];
    }
}

void EffectsPreviewRenderer::DrawGridOverlay(ImDrawList* draw_list, const ImVec2& region_min, const ImVec2& region_max) const
{
    if (draw_list == nullptr)
    {
        return;
    }

    const float width = std::max(1.0f, region_max.x - region_min.x);
    const float height = std::max(1.0f, region_max.y - region_min.y);
    const float aspect = width / height;
    const float fov_y = 45.0f * kPi / 180.0f;
    const Vec3 focus = {camera_focus_x_, camera_focus_y_, camera_focus_z_};
    const PreviewCamera camera = BuildPreviewCamera(camera_yaw_, camera_pitch_, camera_distance_, focus);
    const float grid_center_x = std::floor(camera_focus_x_);
    const float grid_center_z = std::floor(camera_focus_z_);
    const int first_x = static_cast<int>(grid_center_x - kGridExtent);
    const int last_x = static_cast<int>(grid_center_x + kGridExtent);
    const int first_z = static_cast<int>(grid_center_z - kGridExtent);
    const int last_z = static_cast<int>(grid_center_z + kGridExtent);

    draw_list->PushClipRect(region_min, region_max, true);

    auto draw_grid_line = [&](Vec3 a_world, Vec3 b_world, ImU32 color, float thickness)
    {
        CameraPoint a_camera = ToCameraPoint(a_world, camera);
        CameraPoint b_camera = ToCameraPoint(b_world, camera);
        if (!ClipCameraSegment(a_camera, b_camera, aspect, fov_y))
        {
            return;
        }

        ImVec2 a;
        ImVec2 b;
        if (ProjectCameraPoint(a_camera, aspect, fov_y, region_min, region_max, a) &&
            ProjectCameraPoint(b_camera, aspect, fov_y, region_min, region_max, b))
        {
            draw_list->AddLine(a, b, color, thickness);
        }
    };

    for (int z = first_z; z <= last_z; ++z)
    {
        const bool axis = z == 0;
        const bool major = (z % 5) == 0;
        const ImU32 color = axis ? IM_COL32(130, 170, 120, 125) : (major ? IM_COL32(118, 128, 142, 74) : IM_COL32(92, 102, 116, 44));
        draw_grid_line(
            Vec3{static_cast<float>(first_x), 0.0f, static_cast<float>(z)},
            Vec3{static_cast<float>(last_x), 0.0f, static_cast<float>(z)},
            color,
            axis ? 1.4f : 1.0f);
    }

    for (int x = first_x; x <= last_x; ++x)
    {
        const bool axis = x == 0;
        const bool major = (x % 5) == 0;
        const ImU32 color = axis ? IM_COL32(130, 170, 120, 125) : (major ? IM_COL32(118, 128, 142, 74) : IM_COL32(92, 102, 116, 44));
        draw_grid_line(
            Vec3{static_cast<float>(x), 0.0f, static_cast<float>(first_z)},
            Vec3{static_cast<float>(x), 0.0f, static_cast<float>(last_z)},
            color,
            axis ? 1.4f : 1.0f);
    }

    draw_list->PopClipRect();
}

void EffectsPreviewRenderer::ResetCamera()
{
    camera_yaw_ = 0.0f;
    camera_pitch_ = 0.25f;
    camera_distance_ = 8.0f;
    camera_focus_x_ = 0.0f;
    camera_focus_y_ = 0.8f;
    camera_focus_z_ = 0.0f;
}

bool EffectsPreviewRenderer::EnsureInitialized(VulkanContext* vulkan_context)
{
    if (context_ != nullptr && context_ != vulkan_context)
    {
        Clear();
    }

    if (context_ == vulkan_context && command_pool_ != VK_NULL_HANDLE)
    {
        return true;
    }

    context_ = vulkan_context;
    const VkDevice device = vulkan_context->GetDevice();

    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = vulkan_context->GetQueueFamily();

    VkResult result = vkCreateCommandPool(device, &pool_info, vulkan_context->GetAllocator(), &command_pool_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError("Failed to create Effekseer preview command pool");
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
        SetError("Failed to allocate Effekseer preview command buffer");
        return false;
    }

#ifdef GAMMA_WITH_EFFEKSEER
    graphics_device_ = EffekseerRendererVulkan::CreateGraphicsDevice(
        vulkan_context->GetPhysicalDevice(),
        vulkan_context->GetDevice(),
        vulkan_context->GetQueue(),
        command_pool_,
        std::max(2u, vulkan_context->GetImageCount()));
    if (!graphics_device_)
    {
        SetError("Failed to create Effekseer Vulkan graphics device");
        return false;
    }

    EffekseerRendererVulkan::RenderPassInformation render_pass_info = {};
    render_pass_info.DoesPresentToScreen = false;
    render_pass_info.RenderTextureCount = 1;
    render_pass_info.RenderTextureFormats[0] = kPreviewFormat;
    render_pass_info.DepthFormat = VK_FORMAT_UNDEFINED;

    renderer_ = EffekseerRendererVulkan::Create(graphics_device_, render_pass_info, 8000);
    if (!renderer_)
    {
        SetError("Failed to create Effekseer Vulkan renderer");
        return false;
    }

    memory_pool_ = EffekseerRenderer::CreateSingleFrameMemoryPool(renderer_->GetGraphicsDevice());
    efk_command_list_ = EffekseerRenderer::CreateCommandList(renderer_->GetGraphicsDevice(), memory_pool_);
    manager_ = Effekseer::Manager::Create(8000);
    if (!memory_pool_ || !efk_command_list_ || !manager_)
    {
        SetError("Failed to create Effekseer preview runtime objects");
        return false;
    }

    manager_->SetSpriteRenderer(renderer_->CreateSpriteRenderer());
    manager_->SetRibbonRenderer(renderer_->CreateRibbonRenderer());
    manager_->SetRingRenderer(renderer_->CreateRingRenderer());
    manager_->SetTrackRenderer(renderer_->CreateTrackRenderer());
    manager_->SetModelRenderer(renderer_->CreateModelRenderer());
    manager_->SetTextureLoader(renderer_->CreateTextureLoader());
    manager_->SetModelLoader(renderer_->CreateModelLoader());
    manager_->SetMaterialLoader(renderer_->CreateMaterialLoader());
    manager_->SetCurveLoader(Effekseer::MakeRefPtr<Effekseer::CurveLoader>());
#endif

    last_error_.clear();
    return true;
}

bool EffectsPreviewRenderer::EnsureRenderTarget(VulkanContext* vulkan_context, std::uint32_t width, std::uint32_t height)
{
    if (descriptor_set_ != VK_NULL_HANDLE && width == target_width_ && height == target_height_)
    {
        return true;
    }

    DestroyRenderTarget();

    const VkDevice device = vulkan_context->GetDevice();
    VkAttachmentDescription color_attachment = {};
    color_attachment.format = kPreviewFormat;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_reference = {};
    color_reference.attachment = 0;
    color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;

    VkRenderPassCreateInfo render_pass_info = {};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &color_attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;

    VkResult result = vkCreateRenderPass(device, &render_pass_info, vulkan_context->GetAllocator(), &render_pass_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError("Failed to create Effekseer preview render pass");
        return false;
    }

    if (!CreatePreviewImage(vulkan_context, width, height, color_image_, color_memory_, color_view_))
    {
        SetError("Failed to create Effekseer preview color image");
        DestroyRenderTarget();
        return false;
    }

    VkFramebufferCreateInfo framebuffer_info = {};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass_;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &color_view_;
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = 1;

    result = vkCreateFramebuffer(device, &framebuffer_info, vulkan_context->GetAllocator(), &framebuffer_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError("Failed to create Effekseer preview framebuffer");
        DestroyRenderTarget();
        return false;
    }

    VkSamplerCreateInfo sampler_info = {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 1.0f;

    result = vkCreateSampler(device, &sampler_info, vulkan_context->GetAllocator(), &sampler_);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError("Failed to create Effekseer preview sampler");
        DestroyRenderTarget();
        return false;
    }

    descriptor_set_ = ImGui_ImplVulkan_AddTexture(sampler_, color_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (descriptor_set_ == VK_NULL_HANDLE)
    {
        SetError("Failed to register Effekseer preview image with ImGui");
        DestroyRenderTarget();
        return false;
    }

    target_width_ = width;
    target_height_ = height;
    return true;
}

bool EffectsPreviewRenderer::LoadEffect(const std::filesystem::path& effect_path)
{
#ifndef GAMMA_WITH_EFFEKSEER
    (void)effect_path;
    return false;
#else
    if (effect_ && loaded_effect_path_ == effect_path)
    {
        return true;
    }

    Stop();
    effect_.Reset();
    loaded_effect_path_.clear();

    std::error_code error;
    if (!std::filesystem::is_regular_file(effect_path, error))
    {
        SetError("Effect file does not exist");
        return false;
    }

    const std::u16string path16 = ToEffekseerPath(effect_path);
    effect_ = Effekseer::Effect::Create(manager_, reinterpret_cast<const char16_t*>(path16.c_str()));
    if (!effect_)
    {
        SetError("Effekseer failed to load effect: " + effect_path.filename().string());
        return false;
    }

    loaded_effect_path_ = effect_path;
    restart_requested_ = true;
    ValidateLoadedEffectResources(effect_path);
    return true;
#endif
}

void EffectsPreviewRenderer::ValidateLoadedEffectResources(const std::filesystem::path& effect_path)
{
#ifdef GAMMA_WITH_EFFEKSEER
    if (!effect_)
    {
        return;
    }

    std::vector<std::string> missing_paths;
    auto check_texture = [&](Effekseer::TextureRef texture, const char16_t* path)
    {
        if (!texture)
        {
            std::string display_path = FromEffekseerPath(path);
            if (display_path.empty())
            {
                display_path = "unnamed texture";
            }
            missing_paths.push_back(display_path);
        }
    };

    for (int32_t index = 0; index < effect_->GetColorImageCount(); ++index)
    {
        check_texture(effect_->GetColorImage(index), effect_->GetColorImagePath(index));
    }
    for (int32_t index = 0; index < effect_->GetNormalImageCount(); ++index)
    {
        check_texture(effect_->GetNormalImage(index), effect_->GetNormalImagePath(index));
    }
    for (int32_t index = 0; index < effect_->GetDistortionImageCount(); ++index)
    {
        check_texture(effect_->GetDistortionImage(index), effect_->GetDistortionImagePath(index));
    }

    if (missing_paths.empty())
    {
        last_error_.clear();
        return;
    }

    std::string message = "Missing Effekseer resources next to " + effect_path.filename().string() + ": ";
    const std::size_t path_count = std::min<std::size_t>(missing_paths.size(), 3u);
    for (std::size_t index = 0; index < path_count; ++index)
    {
        if (index > 0)
        {
            message += ", ";
        }
        message += missing_paths[index];
    }
    if (missing_paths.size() > path_count)
    {
        message += ", ...";
    }
    SetError(message);
#else
    (void)effect_path;
#endif
}

void EffectsPreviewRenderer::AdvanceSimulation(float target_seconds, bool playing, bool paused, bool loop)
{
#ifdef GAMMA_WITH_EFFEKSEER
    if (!manager_ || !effect_)
    {
        return;
    }

    if (!playing)
    {
        Stop();
        return;
    }

    if (restart_requested_ || stopped_ || handle_ < 0 || !manager_->Exists(handle_) || target_seconds + 0.001f < simulated_seconds_)
    {
        ResetPlayback();
    }

    const float clamped_target = std::max(0.0f, target_seconds);
    float remaining_seconds = std::max(0.0f, clamped_target - simulated_seconds_);
    if (paused && remaining_seconds < (1.0f / 60.0f))
    {
        return;
    }

    if (!loop)
    {
        remaining_seconds = std::min(remaining_seconds, 60.0f);
    }

    Effekseer::Manager::LayerParameter layer_parameter;
    layer_parameter.ViewerPosition = Effekseer::Vector3D(0.0f, 0.0f, 8.0f);
    manager_->SetLayerParameter(0, layer_parameter);

    while (remaining_seconds > 0.0001f)
    {
        const float step_seconds = std::min(remaining_seconds, 1.0f / 30.0f);
        Effekseer::Manager::UpdateParameter update_parameter;
        update_parameter.DeltaFrame = step_seconds * 60.0f;
        update_parameter.UpdateInterval = 1.0f;
        update_parameter.SyncUpdate = true;
        manager_->Update(update_parameter);
        simulated_seconds_ += step_seconds;
        remaining_seconds -= step_seconds;
    }
#else
    (void)target_seconds;
    (void)playing;
    (void)paused;
    (void)loop;
#endif
}

bool EffectsPreviewRenderer::RenderFrame(VulkanContext* vulkan_context, std::uint32_t width, std::uint32_t height)
{
#ifndef GAMMA_WITH_EFFEKSEER
    (void)vulkan_context;
    (void)width;
    (void)height;
    return false;
#else
    if (!renderer_ || !manager_ || command_buffer_ == VK_NULL_HANDLE || framebuffer_ == VK_NULL_HANDLE)
    {
        return false;
    }

    memory_pool_->NewFrame();

    VkResult result = vkResetCommandBuffer(command_buffer_, 0);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError("Failed to reset Effekseer preview command buffer");
        return false;
    }

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(command_buffer_, &begin_info);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError("Failed to begin Effekseer preview command buffer");
        return false;
    }

    TransitionImage(
        command_buffer_,
        color_image_,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

    EffekseerRendererVulkan::BeginCommandList(efk_command_list_, command_buffer_);
    renderer_->SetCommandList(efk_command_list_);

    VkClearValue clear_value = {};
    clear_value.color = {{0.02f, 0.025f, 0.032f, 1.0f}};

    VkRenderPassBeginInfo render_pass_begin = {};
    render_pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin.renderPass = render_pass_;
    render_pass_begin.framebuffer = framebuffer_;
    render_pass_begin.renderArea.extent.width = width;
    render_pass_begin.renderArea.extent.height = height;
    render_pass_begin.clearValueCount = 1;
    render_pass_begin.pClearValues = &clear_value;

    vkCmdBeginRenderPass(command_buffer_, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffer_, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.extent.width = width;
    scissor.extent.height = height;
    vkCmdSetScissor(command_buffer_, 0, 1, &scissor);

    const float aspect = static_cast<float>(width) / static_cast<float>(std::max(1u, height));
    const Vec3 focus = {camera_focus_x_, camera_focus_y_, camera_focus_z_};
    const PreviewCamera camera = BuildPreviewCamera(camera_yaw_, camera_pitch_, camera_distance_, focus);
    const Effekseer::Vector3D viewer_position(camera.eye[0], camera.eye[1], camera.eye[2]);
    const Effekseer::Vector3D focus_position(camera.focus[0], camera.focus[1], camera.focus[2]);
    const Effekseer::Vector3D up_direction(camera.up[0], camera.up[1], camera.up[2]);

    Effekseer::Matrix44 projection_matrix;
    projection_matrix.PerspectiveFovRH(45.0f / 180.0f * kPi, aspect, 0.1f, 200.0f);

    Effekseer::Matrix44 camera_matrix;
    camera_matrix.LookAtRH(viewer_position, focus_position, up_direction);

    renderer_->SetTime(simulated_seconds_);
    renderer_->SetProjectionMatrix(projection_matrix);
    renderer_->SetCameraMatrix(camera_matrix);

    if (renderer_->BeginRendering())
    {
        Effekseer::Manager::DrawParameter draw_parameter;
        draw_parameter.ZNear = 0.1f;
        draw_parameter.ZFar = 200.0f;
        draw_parameter.ViewProjectionMatrix = renderer_->GetCameraProjectionMatrix();
        draw_parameter.CameraPosition = viewer_position;
        draw_parameter.CameraFrontDirection = Effekseer::Vector3D(camera.front[0], camera.front[1], camera.front[2]);
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
        color_image_,
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
        SetError("Failed to finish Effekseer preview command buffer");
        return false;
    }

    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer_;

    result = vkQueueSubmit(vulkan_context->GetQueue(), 1, &submit_info, VK_NULL_HANDLE);
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError("Failed to submit Effekseer preview commands");
        return false;
    }

    result = vkQueueWaitIdle(vulkan_context->GetQueue());
    VulkanContext::CheckVkResult(result);
    if (result != VK_SUCCESS)
    {
        SetError("Failed to wait for Effekseer preview rendering");
        return false;
    }

    last_error_.clear();
    return true;
#endif
}

void EffectsPreviewRenderer::DestroyRenderTarget()
{
    if (context_ == nullptr)
    {
        return;
    }

    context_->WaitIdle();

    const VkDevice device = context_->GetDevice();
    const VkAllocationCallbacks* allocator = context_->GetAllocator();

    if (descriptor_set_ != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkan_RemoveTexture(descriptor_set_);
        descriptor_set_ = VK_NULL_HANDLE;
    }
    if (sampler_ != VK_NULL_HANDLE)
    {
        vkDestroySampler(device, sampler_, allocator);
        sampler_ = VK_NULL_HANDLE;
    }
    if (framebuffer_ != VK_NULL_HANDLE)
    {
        vkDestroyFramebuffer(device, framebuffer_, allocator);
        framebuffer_ = VK_NULL_HANDLE;
    }
    if (color_view_ != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, color_view_, allocator);
        color_view_ = VK_NULL_HANDLE;
    }
    if (color_image_ != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, color_image_, allocator);
        color_image_ = VK_NULL_HANDLE;
    }
    if (color_memory_ != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, color_memory_, allocator);
        color_memory_ = VK_NULL_HANDLE;
    }
    if (render_pass_ != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(device, render_pass_, allocator);
        render_pass_ = VK_NULL_HANDLE;
    }

    target_width_ = 0;
    target_height_ = 0;
}

void EffectsPreviewRenderer::DestroyEffekseer()
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
    efk_command_list_.Reset();
    memory_pool_.Reset();
    effect_.Reset();
    manager_.Reset();
    renderer_.Reset();
    graphics_device_.Reset();
    handle_ = -1;
#endif

    if (context_ != nullptr && command_pool_ != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(context_->GetDevice(), command_pool_, context_->GetAllocator());
    }
    command_pool_ = VK_NULL_HANDLE;
    command_buffer_ = VK_NULL_HANDLE;
    loaded_effect_path_.clear();
    ResetPlayback();
}

void EffectsPreviewRenderer::ResetPlayback()
{
#ifdef GAMMA_WITH_EFFEKSEER
    if (manager_ && handle_ >= 0)
    {
        manager_->StopEffect(handle_);
    }
    handle_ = effect_ ? manager_->Play(effect_, 0.0f, 0.0f, 0.0f) : -1;
#endif
    simulated_seconds_ = 0.0f;
    restart_requested_ = false;
    stopped_ = false;
}

void EffectsPreviewRenderer::SetError(const std::string& message)
{
    if (last_error_ != message)
    {
        last_error_ = message;
    }
}