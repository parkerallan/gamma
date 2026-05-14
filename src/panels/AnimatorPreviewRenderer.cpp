#include "panels/AnimatorPreviewRenderer.h"

#include "app/VulkanContext.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/texture.h>

#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>

#define STB_IMAGE_IMPLEMENTATION_GUARD // sentinel - we do NOT define impl here
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>

namespace
{
// All matrices in this file are 4x4 row-major (m[row*4+col]) consistent with
// aiMatrix4x4. Vectors are treated as columns: v' = M * v.

using Mat4 = std::array<float, 16>;
using Vec3 = std::array<float, 3>;

Mat4 Identity()
{
    Mat4 m{};
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    return m;
}

Mat4 FromAi(const aiMatrix4x4& a)
{
    Mat4 m{};
    m[0] = a.a1; m[1] = a.a2; m[2] = a.a3; m[3] = a.a4;
    m[4] = a.b1; m[5] = a.b2; m[6] = a.b3; m[7] = a.b4;
    m[8] = a.c1; m[9] = a.c2; m[10] = a.c3; m[11] = a.c4;
    m[12] = a.d1; m[13] = a.d2; m[14] = a.d3; m[15] = a.d4;
    return m;
}

Mat4 Multiply(const Mat4& a, const Mat4& b)
{
    Mat4 r{};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                sum += a[row * 4 + k] * b[k * 4 + col];
            }
            r[row * 4 + col] = sum;
        }
    }
    return r;
}

Vec3 TransformPoint(const Mat4& m, const Vec3& v)
{
    return Vec3{
        m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3],
        m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7],
        m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11],
    };
}

Vec3 TransformDirection(const Mat4& m, const Vec3& v)
{
    // Rotation/scale only; no translation. Adequate for normals as long as
    // there's no non-uniform scale (we re-normalize after skinning anyway).
    return Vec3{
        m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
        m[4] * v[0] + m[5] * v[1] + m[6] * v[2],
        m[8] * v[0] + m[9] * v[1] + m[10] * v[2],
    };
}

Vec3 Normalize(const Vec3& v)
{
    const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len < 1e-6f) { return Vec3{0.0f, 1.0f, 0.0f}; }
    return Vec3{v[0] / len, v[1] / len, v[2] / len};
}

Mat4 FromTRS(const aiVector3D& t, const aiQuaternion& r, const aiVector3D& s)
{
    // aiQuaternion -> 3x3
    const float x = r.x, y = r.y, z = r.z, w = r.w;
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    Mat4 m{};
    m[0] = (1.0f - 2.0f * (yy + zz)) * s.x;
    m[1] = (2.0f * (xy - wz)) * s.y;
    m[2] = (2.0f * (xz + wy)) * s.z;
    m[3] = t.x;
    m[4] = (2.0f * (xy + wz)) * s.x;
    m[5] = (1.0f - 2.0f * (xx + zz)) * s.y;
    m[6] = (2.0f * (yz - wx)) * s.z;
    m[7] = t.y;
    m[8] = (2.0f * (xz - wy)) * s.x;
    m[9] = (2.0f * (yz + wx)) * s.y;
    m[10] = (1.0f - 2.0f * (xx + yy)) * s.z;
    m[11] = t.z;
    m[15] = 1.0f;
    return m;
}

aiVector3D InterpolateVec(double time, unsigned int count, const aiVectorKey* keys, const aiVector3D& fallback)
{
    if (count == 0)
    {
        return fallback;
    }
    if (count == 1)
    {
        return keys[0].mValue;
    }

    unsigned int index = 0;
    for (unsigned int i = 0; i + 1 < count; ++i)
    {
        if (time < keys[i + 1].mTime)
        {
            index = i;
            break;
        }
        index = i;
    }
    if (index + 1 >= count)
    {
        return keys[count - 1].mValue;
    }
    const double delta = keys[index + 1].mTime - keys[index].mTime;
    const double factor = delta > 0.0 ? (time - keys[index].mTime) / delta : 0.0;
    const float f = static_cast<float>(std::clamp(factor, 0.0, 1.0));
    return keys[index].mValue + f * (keys[index + 1].mValue - keys[index].mValue);
}

aiQuaternion InterpolateRot(double time, unsigned int count, const aiQuatKey* keys)
{
    if (count == 0)
    {
        return aiQuaternion();
    }
    if (count == 1)
    {
        return keys[0].mValue;
    }

    unsigned int index = 0;
    for (unsigned int i = 0; i + 1 < count; ++i)
    {
        if (time < keys[i + 1].mTime)
        {
            index = i;
            break;
        }
        index = i;
    }
    if (index + 1 >= count)
    {
        return keys[count - 1].mValue;
    }
    const double delta = keys[index + 1].mTime - keys[index].mTime;
    const double factor = delta > 0.0 ? (time - keys[index].mTime) / delta : 0.0;
    aiQuaternion out;
    aiQuaternion::Interpolate(out, keys[index].mValue, keys[index + 1].mValue, static_cast<float>(std::clamp(factor, 0.0, 1.0)));
    out.Normalize();
    return out;
}

void BuildLookAt(const Vec3& eye, const Vec3& target, const Vec3& up, Mat4& out)
{
    Vec3 f{target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl > 1e-6f) { f[0] /= fl; f[1] /= fl; f[2] /= fl; }

    // s = normalize(cross(f, up))
    Vec3 s{f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]};
    const float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl > 1e-6f) { s[0] /= sl; s[1] /= sl; s[2] /= sl; }

    Vec3 u{s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};

    out = Identity();
    out[0] = s[0]; out[1] = s[1]; out[2] = s[2];
    out[4] = u[0]; out[5] = u[1]; out[6] = u[2];
    out[8] = -f[0]; out[9] = -f[1]; out[10] = -f[2];
    out[3] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    out[7] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    out[11] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
}

void BuildPerspective(float fov_y_radians, float aspect, float near_clip, float far_clip, Mat4& out)
{
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    out = {};
    out[0] = f / std::max(aspect, 0.0001f);
    out[5] = f;
    out[10] = (far_clip + near_clip) / (near_clip - far_clip);
    out[11] = (2.0f * far_clip * near_clip) / (near_clip - far_clip);
    out[14] = -1.0f;
}

// Returns true if the point is in front of the camera. Outputs screen-space
// (pixel) coordinates inside the supplied viewport rectangle and the clip-space
// w used for back-clipping.
bool ProjectPoint(
    const Vec3& world,
    const Mat4& view_proj,
    const ImVec2& min,
    const ImVec2& max,
    ImVec2& out_screen,
    float& out_w,
    float& out_depth)
{
    const float x = view_proj[0] * world[0] + view_proj[1] * world[1] + view_proj[2] * world[2] + view_proj[3];
    const float y = view_proj[4] * world[0] + view_proj[5] * world[1] + view_proj[6] * world[2] + view_proj[7];
    const float z = view_proj[8] * world[0] + view_proj[9] * world[1] + view_proj[10] * world[2] + view_proj[11];
    const float w = view_proj[12] * world[0] + view_proj[13] * world[1] + view_proj[14] * world[2] + view_proj[15];

    out_w = w;
    if (w <= 0.0001f)
    {
        return false;
    }
    const float nx = x / w;
    const float ny = y / w;
    out_depth = z / w;
    out_screen = ImVec2(
        min.x + (nx * 0.5f + 0.5f) * (max.x - min.x),
        min.y + (1.0f - (ny * 0.5f + 0.5f)) * (max.y - min.y));
    return true;
}

ImU32 BoneColor(int depth)
{
    static const ImU32 palette[] = {
        IM_COL32(255, 196, 64, 255),
        IM_COL32(120, 220, 255, 255),
        IM_COL32(160, 255, 160, 255),
        IM_COL32(255, 140, 220, 255),
    };
    return palette[depth % (sizeof(palette) / sizeof(palette[0]))];
}

// ---- Vulkan helpers for one-shot RGBA8 texture upload. -------------------
// Mirrors the pattern used by ImageInfoRenderer but kept local to keep the
// preview self-contained. Synchronously uploads via a transient command pool
// and waits on the queue once per texture.

std::uint32_t FindMemoryType(VkPhysicalDevice physical_device, std::uint32_t type_filter, VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memory_properties = {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
    {
        if ((type_filter & (1u << i)) != 0
            && (memory_properties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

bool UploadRgbaTexture(
    VulkanContext& ctx,
    const unsigned char* pixels,
    std::uint32_t width,
    std::uint32_t height,
    VkImage& out_image,
    VkDeviceMemory& out_memory,
    VkImageView& out_view,
    VkSampler& out_sampler)
{
    const VkDevice device = ctx.GetDevice();
    const VkPhysicalDevice phys = ctx.GetPhysicalDevice();

    // Image + image view.
    {
        VkImageCreateInfo image_info = {};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {width, height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &image_info, nullptr, &out_image) != VK_SUCCESS) { return false; }

        VkMemoryRequirements req = {};
        vkGetImageMemoryRequirements(device, out_image, &req);
        VkMemoryAllocateInfo allocate = {};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = req.size;
        allocate.memoryTypeIndex = FindMemoryType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(device, &allocate, nullptr, &out_memory) != VK_SUCCESS
            || vkBindImageMemory(device, out_image, out_memory, 0) != VK_SUCCESS)
        {
            return false;
        }

        VkImageViewCreateInfo view_info = {};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = out_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &view_info, nullptr, &out_view) != VK_SUCCESS) { return false; }
    }

    // Staging buffer.
    const VkDeviceSize upload_size = static_cast<VkDeviceSize>(width) * height * 4u;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    {
        VkBufferCreateInfo buffer_info = {};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = upload_size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &buffer_info, nullptr, &staging) != VK_SUCCESS) { return false; }

        VkMemoryRequirements req = {};
        vkGetBufferMemoryRequirements(device, staging, &req);
        VkMemoryAllocateInfo allocate = {};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = req.size;
        allocate.memoryTypeIndex = FindMemoryType(phys, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(device, &allocate, nullptr, &staging_memory) != VK_SUCCESS
            || vkBindBufferMemory(device, staging, staging_memory, 0) != VK_SUCCESS)
        {
            vkDestroyBuffer(device, staging, nullptr);
            if (staging_memory != VK_NULL_HANDLE) { vkFreeMemory(device, staging_memory, nullptr); }
            return false;
        }

        void* mapped = nullptr;
        if (vkMapMemory(device, staging_memory, 0, upload_size, 0, &mapped) != VK_SUCCESS || mapped == nullptr)
        {
            vkFreeMemory(device, staging_memory, nullptr);
            vkDestroyBuffer(device, staging, nullptr);
            return false;
        }
        std::memcpy(mapped, pixels, static_cast<std::size_t>(upload_size));
        vkUnmapMemory(device, staging_memory);
    }

    // Transient command buffer to copy + transition.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = ctx.GetQueueFamily();
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(device, &pool_info, ctx.GetAllocator(), &pool) != VK_SUCCESS)
    {
        vkFreeMemory(device, staging_memory, nullptr);
        vkDestroyBuffer(device, staging, nullptr);
        return false;
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cba = {};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool = pool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cba, &cmd);

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkAccessFlags srca, VkAccessFlags dsta)
    {
        VkImageMemoryBarrier b = {};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = out_image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        b.srcAccessMask = srca;
        b.dstAccessMask = dsta;
        vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, VK_ACCESS_TRANSFER_WRITE_BIT);

    VkBufferImageCopy copy = {};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging, out_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx.GetQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.GetQueue());

    vkDestroyCommandPool(device, pool, ctx.GetAllocator());
    vkFreeMemory(device, staging_memory, nullptr);
    vkDestroyBuffer(device, staging, nullptr);

    // Sampler.
    VkSamplerCreateInfo sampler_info = {};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.maxLod = 1.0f;
    if (vkCreateSampler(device, &sampler_info, ctx.GetAllocator(), &out_sampler) != VK_SUCCESS) { return false; }

    return true;
}
}

AnimatorPreviewRenderer::AnimatorPreviewRenderer() = default;
AnimatorPreviewRenderer::~AnimatorPreviewRenderer()
{
    // Block on the worker so it can't write into our members after we
    // start tearing down GPU state.
    if (load_thread_.joinable())
    {
        load_thread_.join();
    }
    DestroyGpu();
    DestroyTextures();
}

void AnimatorPreviewRenderer::ClearModel()
{
    // Drain any in-flight async load so the worker doesn't race with the
    // teardown below. Discard whatever it produced.
    if (load_thread_.joinable())
    {
        load_thread_.join();
    }
    loading_.store(false, std::memory_order_release);
    load_ready_.store(false, std::memory_order_release);
    pending_importer_.reset();
    pending_scene_ = nullptr;
    pending_path_.clear();
    pending_error_.clear();

    // Tear down GPU mesh buffers (but keep the shared pipeline alive) so a
    // new model can be loaded without recreating the entire renderer state.
    if (texture_context_ != nullptr && !gpu_meshes_.empty())
    {
        VkDevice device = texture_context_->GetDevice();
        const VkAllocationCallbacks* allocator = texture_context_->GetAllocator();
        vkDeviceWaitIdle(device);
        for (GpuMesh& mesh : gpu_meshes_)
        {
            if (mesh.material_ubo_mapped != nullptr && mesh.material_ubo_memory != VK_NULL_HANDLE)
            {
                vkUnmapMemory(device, mesh.material_ubo_memory);
                mesh.material_ubo_mapped = nullptr;
            }
            if (mesh.material_ubo != VK_NULL_HANDLE) { vkDestroyBuffer(device, mesh.material_ubo, allocator); }
            if (mesh.material_ubo_memory != VK_NULL_HANDLE) { vkFreeMemory(device, mesh.material_ubo_memory, allocator); }
            if (mesh.vertex_buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, mesh.vertex_buffer, allocator); }
            if (mesh.vertex_memory != VK_NULL_HANDLE) { vkFreeMemory(device, mesh.vertex_memory, allocator); }
            if (mesh.index_buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, mesh.index_buffer, allocator); }
            if (mesh.index_memory != VK_NULL_HANDLE) { vkFreeMemory(device, mesh.index_memory, allocator); }
            // Descriptor sets are freed when the pool is reset/destroyed.
        }
        if (descriptor_pool_ != VK_NULL_HANDLE)
        {
            vkResetDescriptorPool(device, descriptor_pool_, 0);
        }
        if (bone_buffer_mapped_ != nullptr && bone_buffer_memory_ != VK_NULL_HANDLE)
        {
            vkUnmapMemory(device, bone_buffer_memory_);
            bone_buffer_mapped_ = nullptr;
        }
        if (bone_buffer_ != VK_NULL_HANDLE) { vkDestroyBuffer(device, bone_buffer_, allocator); bone_buffer_ = VK_NULL_HANDLE; }
        if (bone_buffer_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device, bone_buffer_memory_, allocator); bone_buffer_memory_ = VK_NULL_HANDLE; }
        bone_buffer_size_ = 0;
        bone_buffer_capacity_ = 0;
    }
    gpu_meshes_.clear();

    DestroyTextures();
    importer_.reset();
    scene_ = nullptr;
    loaded_ = false;
    model_path_.clear();
    mesh_bindings_.clear();
    bones_.clear();
    bone_index_by_name_.clear();
    clip_names_.clear();
    channels_by_clip_.clear();
    active_clip_name_.clear();
    node_index_.clear();
    skeleton_nodes_.clear();
    animated_bone_matrices_.clear();
    node_world_transforms_.clear();
    pending_textures_.clear();
    last_error_.clear();
    current_time_seconds_ = 0.0f;
    selected_bone_name_.clear();
    jiggle_states_.clear();
    physics_accumulator_seconds_ = 0.0f;
}

void AnimatorPreviewRenderer::DestroyTextures()
{
    if (texture_context_ != nullptr && !textures_.empty())
    {
        VkDevice device = texture_context_->GetDevice();
        const VkAllocationCallbacks* allocator = texture_context_->GetAllocator();
        vkDeviceWaitIdle(device);
        for (GpuTexture& texture : textures_)
        {
            if (texture.descriptor_set != VK_NULL_HANDLE)
            {
                ImGui_ImplVulkan_RemoveTexture(texture.descriptor_set);
            }
            if (texture.sampler != VK_NULL_HANDLE)
            {
                vkDestroySampler(device, texture.sampler, allocator);
            }
            if (texture.view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(device, texture.view, nullptr);
            }
            if (texture.image != VK_NULL_HANDLE)
            {
                vkDestroyImage(device, texture.image, nullptr);
            }
            if (texture.memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(device, texture.memory, nullptr);
            }
        }
    }
    textures_.clear();
    texture_context_ = nullptr;
}

void AnimatorPreviewRenderer::EnsureTextures(VulkanContext* vulkan_context)
{
    if (vulkan_context == nullptr || pending_textures_.empty())
    {
        return;
    }

    // Re-sync if the context changes between renders. In practice this only
    // happens on context reinit; cheaper than tracking dirty flags everywhere.
    if (texture_context_ != nullptr && texture_context_ != vulkan_context)
    {
        DestroyTextures();
    }
    texture_context_ = vulkan_context;

    if (textures_.size() < pending_textures_.size())
    {
        textures_.resize(pending_textures_.size());
    }

    for (std::size_t i = 0; i < pending_textures_.size(); ++i)
    {
        GpuTexture& gpu = textures_[i];
        if (gpu.uploaded || gpu.failed) { continue; }
        const PendingTexture& pending = pending_textures_[i];

        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = nullptr;
        if (pending.embedded_index >= 0 && scene_ != nullptr
            && static_cast<unsigned int>(pending.embedded_index) < scene_->mNumTextures)
        {
            const aiTexture* embedded = scene_->mTextures[pending.embedded_index];
            if (embedded != nullptr)
            {
                if (embedded->mHeight == 0)
                {
                    // Compressed payload (png/jpg bytes). mWidth holds the size.
                    pixels = stbi_load_from_memory(
                        reinterpret_cast<const stbi_uc*>(embedded->pcData),
                        static_cast<int>(embedded->mWidth),
                        &width, &height, &channels, 4);
                }
                else
                {
                    // Raw BGRA8 already; we need RGBA. Copy and swizzle.
                    width = static_cast<int>(embedded->mWidth);
                    height = static_cast<int>(embedded->mHeight);
                    pixels = static_cast<stbi_uc*>(std::malloc(static_cast<std::size_t>(width) * height * 4));
                    if (pixels != nullptr)
                    {
                        for (int p = 0; p < width * height; ++p)
                        {
                            const aiTexel& texel = embedded->pcData[p];
                            pixels[p * 4 + 0] = texel.r;
                            pixels[p * 4 + 1] = texel.g;
                            pixels[p * 4 + 2] = texel.b;
                            pixels[p * 4 + 3] = texel.a;
                        }
                    }
                }
            }
        }
        else if (!pending.source_path.empty())
        {
            pixels = stbi_load(pending.source_path.c_str(), &width, &height, &channels, 4);
        }

        if (pixels == nullptr || width <= 0 || height <= 0)
        {
            gpu.failed = true;
            continue;
        }

        const bool ok = UploadRgbaTexture(
            *vulkan_context,
            pixels,
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
            gpu.image, gpu.memory, gpu.view, gpu.sampler);
        stbi_image_free(pixels);
        if (!ok)
        {
            gpu.failed = true;
            continue;
        }

        gpu.descriptor_set = ImGui_ImplVulkan_AddTexture(
            gpu.sampler, gpu.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (gpu.descriptor_set == VK_NULL_HANDLE)
        {
            gpu.failed = true;
            continue;
        }

        gpu.source_key = pending.source_key;
        gpu.width = width;
        gpu.height = height;
        gpu.uploaded = true;
    }
}

bool AnimatorPreviewRenderer::SetModel(const std::filesystem::path& absolute_model_path)
{
    // Already showing this exact model — no work.
    if (loaded_ && model_path_ == absolute_model_path)
    {
        return true;
    }
    // Already queued for this path — wait for the worker.
    if (loading_.load(std::memory_order_acquire) && pending_path_ == absolute_model_path)
    {
        return true;
    }

    // If a different load is in-flight, drain it before kicking off the new
    // request. The completed/orphaned result is simply discarded.
    if (load_thread_.joinable())
    {
        load_thread_.join();
    }
    loading_.store(false, std::memory_order_release);
    load_ready_.store(false, std::memory_order_release);
    pending_importer_.reset();
    pending_scene_ = nullptr;
    pending_error_.clear();

    ClearModel();

    if (absolute_model_path.empty() || !std::filesystem::exists(absolute_model_path))
    {
        last_error_ = "File not found";
        return false;
    }

    pending_path_ = absolute_model_path;
    loading_.store(true, std::memory_order_release);
    load_ready_.store(false, std::memory_order_release);

    // Worker thread: do the heavy Assimp parse off the main thread. Only
    // touches the pending_* members (no main-thread state is mutated until
    // PollPendingLoad runs on the main thread).
    const std::filesystem::path path_copy = absolute_model_path;
    load_thread_ = std::thread([this, path_copy]()
    {
        auto importer = std::make_unique<Assimp::Importer>();
        const unsigned int flags =
            aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_GenSmoothNormals |
            aiProcess_LimitBoneWeights |
            aiProcess_ImproveCacheLocality |
            aiProcess_SortByPType;
        const aiScene* scene = importer->ReadFile(path_copy.string(), flags);
        if (scene == nullptr || scene->mRootNode == nullptr)
        {
            std::string err = importer->GetErrorString();
            if (err.empty()) { err = "Assimp failed to load the model"; }
            pending_error_ = std::move(err);
            pending_scene_ = nullptr;
            pending_importer_.reset();
        }
        else
        {
            pending_scene_ = scene;
            pending_importer_ = std::move(importer);
            pending_error_.clear();
        }
        load_ready_.store(true, std::memory_order_release);
    });

    return true;
}

void AnimatorPreviewRenderer::PollPendingLoad()
{
    if (!loading_.load(std::memory_order_acquire))
    {
        return;
    }
    if (!load_ready_.load(std::memory_order_acquire))
    {
        return;
    }

    // Worker is done — drain it.
    if (load_thread_.joinable())
    {
        load_thread_.join();
    }

    if (pending_scene_ == nullptr)
    {
        last_error_ = pending_error_.empty() ? std::string("Failed to load model") : pending_error_;
        pending_importer_.reset();
        pending_path_.clear();
        load_ready_.store(false, std::memory_order_release);
        loading_.store(false, std::memory_order_release);
        return;
    }

    // Promote worker results to live state.
    importer_ = std::move(pending_importer_);
    scene_ = pending_scene_;
    pending_scene_ = nullptr;
    loaded_ = true;
    model_path_ = pending_path_;
    pending_path_.clear();

    aiMatrix4x4 gi = scene_->mRootNode->mTransformation;
    gi.Inverse();
    global_inverse_ = FromAi(gi);

    BuildBindings(scene_);

    if (!active_clip_name_.empty())
    {
        bool found = false;
        for (const std::string& name : clip_names_)
        {
            if (name == active_clip_name_) { found = true; break; }
        }
        if (!found)
        {
            active_clip_name_ = clip_names_.empty() ? std::string() : clip_names_.front();
        }
    }
    else if (!clip_names_.empty())
    {
        active_clip_name_ = clip_names_.front();
    }

    ComputeFrameBounds();
    ResetView();

    load_ready_.store(false, std::memory_order_release);
    loading_.store(false, std::memory_order_release);
}

void AnimatorPreviewRenderer::BuildBindings(const aiScene* scene)
{
    // Index all nodes in pre-order for fast lookups.
    std::function<void(const aiNode*)> assign = [&](const aiNode* node)
    {
        node_index_[node] = node_index_.size();
        for (unsigned int i = 0; i < node->mNumChildren; ++i)
        {
            assign(node->mChildren[i]);
        }
    };
    assign(scene->mRootNode);
    node_world_transforms_.assign(node_index_.size(), Identity());

    CollectMeshBindings(scene, scene->mRootNode, Identity());

    animated_bone_matrices_.assign(bones_.size(), Identity());

    // Animation channels.
    clip_names_.clear();
    channels_by_clip_.clear();
    for (unsigned int i = 0; i < scene->mNumAnimations; ++i)
    {
        const aiAnimation* anim = scene->mAnimations[i];
        if (anim == nullptr) { continue; }
        std::string name = anim->mName.length > 0 ? std::string(anim->mName.C_Str()) : ("Animation_" + std::to_string(i + 1));
        // Disambiguate duplicate names.
        std::string unique = name;
        int suffix = 2;
        while (channels_by_clip_.count(unique) != 0)
        {
            unique = name + "_" + std::to_string(suffix++);
        }
        clip_names_.push_back(unique);
        auto& by_name = channels_by_clip_[unique];
        for (unsigned int c = 0; c < anim->mNumChannels; ++c)
        {
            const aiNodeAnim* channel = anim->mChannels[c];
            if (channel == nullptr) { continue; }
            by_name[std::string(channel->mNodeName.C_Str())] = channel;
        }
    }

    // Skeleton display set: every bone-referenced node plus its ancestors up
    // to the root. Lets us draw the skeleton even when the rig has helper
    // nodes that aren't directly skinned.
    std::vector<const aiNode*> ordered_skeleton_nodes;
    std::unordered_map<const aiNode*, std::size_t> skeleton_lookup;

    std::function<void(const aiNode*)> include = [&](const aiNode* node)
    {
        if (node == nullptr) { return; }
        if (skeleton_lookup.count(node) != 0) { return; }
        if (node->mParent != nullptr)
        {
            include(node->mParent);
        }
        skeleton_lookup[node] = ordered_skeleton_nodes.size();
        ordered_skeleton_nodes.push_back(node);
    };

    for (const Bone& bone : bones_)
    {
        const aiNode* node = scene->mRootNode->FindNode(bone.name.c_str());
        include(node);
    }

    skeleton_nodes_.clear();
    skeleton_nodes_.reserve(ordered_skeleton_nodes.size());
    for (const aiNode* node : ordered_skeleton_nodes)
    {
        SkeletonNode entry;
        entry.node = node;
        if (node->mParent != nullptr)
        {
            const auto it = skeleton_lookup.find(node->mParent);
            entry.parent_skeleton_index = it != skeleton_lookup.end() ? static_cast<int>(it->second) : -1;
        }
        skeleton_nodes_.push_back(entry);
    }
}

void AnimatorPreviewRenderer::CollectMeshBindings(const aiScene* scene, const aiNode* node, const Mat4& parent)
{
    const Mat4 node_world = Multiply(parent, FromAi(node->mTransformation));

    for (unsigned int i = 0; i < node->mNumMeshes; ++i)
    {
        const unsigned int mesh_index = node->mMeshes[i];
        if (mesh_index >= scene->mNumMeshes) { continue; }
        const aiMesh* mesh = scene->mMeshes[mesh_index];
        if (mesh == nullptr || mesh->mNumVertices == 0) { continue; }

        MeshBinding binding;
        binding.bind_node_transform = node_world;
        binding.bind_positions.reserve(mesh->mNumVertices);
        binding.bind_normals.reserve(mesh->mNumVertices);
        binding.bind_uvs.reserve(mesh->mNumVertices);
        for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
        {
            const aiVector3D& p = mesh->mVertices[v];
            binding.bind_positions.push_back({p.x, p.y, p.z});
            if (mesh->HasNormals())
            {
                const aiVector3D& n = mesh->mNormals[v];
                binding.bind_normals.push_back({n.x, n.y, n.z});
            }
            else
            {
                binding.bind_normals.push_back({0.0f, 1.0f, 0.0f});
            }
            if (mesh->HasTextureCoords(0))
            {
                const aiVector3D& uv = mesh->mTextureCoords[0][v];
                // Match RuntimeRenderer's convention: flip V so glTF/FBX
                // textures (origin top-left) sample correctly with Vulkan's
                // top-left texel addressing.
                binding.bind_uvs.push_back({uv.x, 1.0f - uv.y});
            }
            else
            {
                binding.bind_uvs.push_back({0.0f, 0.0f});
            }
        }

        binding.indices.reserve(static_cast<std::size_t>(mesh->mNumFaces) * 3);
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
        {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3) { continue; } // mesh was triangulated; skip degenerate
            binding.indices.push_back(face.mIndices[0]);
            binding.indices.push_back(face.mIndices[1]);
            binding.indices.push_back(face.mIndices[2]);
        }

        // Sample base color from the assimp material if available so meshes
        // render with their authored tint instead of a flat gray.
        if (mesh->mMaterialIndex < scene->mNumMaterials)
        {
            const aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
            if (material != nullptr)
            {
                aiColor4D color(1.0f, 1.0f, 1.0f, 1.0f);
                if (material->Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS
                    || material->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS)
                {
                    binding.base_color = {
                        std::clamp(color.r, 0.0f, 1.0f),
                        std::clamp(color.g, 0.0f, 1.0f),
                        std::clamp(color.b, 0.0f, 1.0f),
                    };
                }

                // Resolve a base-color or diffuse texture. We accept either the
                // glTF base-color slot or the legacy diffuse slot so both PBR
                // and classic materials show up textured in the preview.
                aiString texture_path;
                bool has_path = false;
                for (aiTextureType type : {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE})
                {
                    if (material->GetTextureCount(type) > 0
                        && material->GetTexture(type, 0, &texture_path) == AI_SUCCESS
                        && texture_path.length > 0)
                    {
                        has_path = true;
                        break;
                    }
                }
                if (has_path)
                {
                    PendingTexture entry;
                    const std::string raw_path(texture_path.C_Str(), texture_path.length);
                    if (!raw_path.empty() && raw_path[0] == '*')
                    {
                        // Embedded texture reference (glTF, FBX with embedded media).
                        entry.embedded_index = std::atoi(raw_path.c_str() + 1);
                        entry.source_key = raw_path;
                    }
                    else
                    {
                        std::filesystem::path resolved(raw_path);
                        if (resolved.is_relative())
                        {
                            resolved = model_path_.parent_path() / resolved;
                        }
                        entry.source_path = resolved.string();
                        entry.source_key = resolved.lexically_normal().string();
                    }

                    int existing_index = -1;
                    for (int idx = 0; idx < static_cast<int>(pending_textures_.size()); ++idx)
                    {
                        if (pending_textures_[idx].source_key == entry.source_key)
                        {
                            existing_index = idx;
                            break;
                        }
                    }
                    if (existing_index >= 0)
                    {
                        binding.texture_index = existing_index;
                    }
                    else
                    {
                        binding.texture_index = static_cast<int>(pending_textures_.size());
                        pending_textures_.push_back(std::move(entry));
                    }
                }
            }
        }

        if (mesh->HasBones())
        {
            binding.influences.resize(mesh->mNumVertices);
            for (unsigned int b = 0; b < mesh->mNumBones; ++b)
            {
                const aiBone* bone = mesh->mBones[b];
                if (bone == nullptr) { continue; }
                const std::string bone_name = std::string(bone->mName.C_Str());
                std::size_t runtime_index;
                auto existing = bone_index_by_name_.find(bone_name);
                if (existing == bone_index_by_name_.end())
                {
                    runtime_index = bones_.size();
                    Bone entry;
                    entry.name = bone_name;
                    entry.offset_matrix = FromAi(bone->mOffsetMatrix);
                    bones_.push_back(std::move(entry));
                    bone_index_by_name_[bone_name] = runtime_index;
                }
                else
                {
                    runtime_index = existing->second;
                }

                for (unsigned int w = 0; w < bone->mNumWeights; ++w)
                {
                    const aiVertexWeight& weight = bone->mWeights[w];
                    if (weight.mVertexId >= binding.influences.size() || weight.mWeight <= 0.0f) { continue; }
                    SkinInfluence& influence = binding.influences[weight.mVertexId];
                    int slot = -1;
                    for (int s = 0; s < 4; ++s)
                    {
                        if (influence.bone_indices[s] == -1) { slot = s; break; }
                    }
                    if (slot == -1)
                    {
                        int weakest = 0;
                        for (int s = 1; s < 4; ++s)
                        {
                            if (influence.bone_weights[s] < influence.bone_weights[weakest]) { weakest = s; }
                        }
                        if (weight.mWeight > influence.bone_weights[weakest]) { slot = weakest; }
                    }
                    if (slot >= 0)
                    {
                        influence.bone_indices[slot] = static_cast<int>(runtime_index);
                        influence.bone_weights[slot] = weight.mWeight;
                    }
                }
            }

            for (SkinInfluence& influence : binding.influences)
            {
                float sum = 0.0f;
                for (float w : influence.bone_weights) { sum += w; }
                if (sum > 1e-4f)
                {
                    const float inv = 1.0f / sum;
                    for (float& w : influence.bone_weights) { w *= inv; }
                }
            }
        }

        mesh_bindings_.push_back(std::move(binding));
    }

    for (unsigned int c = 0; c < node->mNumChildren; ++c)
    {
        CollectMeshBindings(scene, node->mChildren[c], node_world);
    }
}

void AnimatorPreviewRenderer::EvaluateHierarchy(const aiNode* node, const Mat4& parent, float time_seconds)
{
    Mat4 node_local = FromAi(node->mTransformation);

    if (!active_clip_name_.empty())
    {
        const auto clip_it = channels_by_clip_.find(active_clip_name_);
        if (clip_it != channels_by_clip_.end())
        {
            const auto channel_it = clip_it->second.find(std::string(node->mName.C_Str()));
            if (channel_it != clip_it->second.end())
            {
                const aiNodeAnim* channel = channel_it->second;
                // Convert seconds to ticks using clip's own ticks-per-second.
                const aiAnimation* anim = nullptr;
                for (unsigned int i = 0; i < scene_->mNumAnimations; ++i)
                {
                    if (scene_->mAnimations[i] != nullptr && std::string(scene_->mAnimations[i]->mName.C_Str()) == active_clip_name_)
                    {
                        anim = scene_->mAnimations[i];
                        break;
                    }
                }
                if (anim == nullptr && scene_->mNumAnimations > 0)
                {
                    anim = scene_->mAnimations[0];
                }
                double ticks_per_second = (anim != nullptr && anim->mTicksPerSecond > 0.0) ? anim->mTicksPerSecond : 25.0;
                double total_ticks = (anim != nullptr) ? anim->mDuration : 0.0;
                double t = static_cast<double>(time_seconds) * ticks_per_second;
                if (total_ticks > 0.0)
                {
                    t = std::fmod(t, total_ticks);
                    if (t < 0.0) { t += total_ticks; }
                }

                const aiVector3D pos = InterpolateVec(t, channel->mNumPositionKeys, channel->mPositionKeys, aiVector3D(0, 0, 0));
                const aiVector3D scl = InterpolateVec(t, channel->mNumScalingKeys, channel->mScalingKeys, aiVector3D(1, 1, 1));
                const aiQuaternion rot = InterpolateRot(t, channel->mNumRotationKeys, channel->mRotationKeys);
                node_local = FromTRS(pos, rot, scl);
            }
        }
    }

    const Mat4 node_world = Multiply(parent, node_local);
    const auto node_it = node_index_.find(node);
    if (node_it != node_index_.end() && node_it->second < node_world_transforms_.size())
    {
        node_world_transforms_[node_it->second] = node_world;
    }

    const auto bone_it = bone_index_by_name_.find(std::string(node->mName.C_Str()));
    if (bone_it != bone_index_by_name_.end() && bone_it->second < animated_bone_matrices_.size())
    {
        animated_bone_matrices_[bone_it->second] = Multiply(Multiply(global_inverse_, node_world), bones_[bone_it->second].offset_matrix);
    }

    for (unsigned int i = 0; i < node->mNumChildren; ++i)
    {
        EvaluateHierarchy(node->mChildren[i], node_world, time_seconds);
    }
}

void AnimatorPreviewRenderer::ComputeFrameBounds()
{
    if (mesh_bindings_.empty())
    {
        model_center_ = {0, 0, 0};
        model_radius_ = 1.0f;
        return;
    }

    Vec3 lo{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    Vec3 hi{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    for (const MeshBinding& binding : mesh_bindings_)
    {
        for (const auto& p : binding.bind_positions)
        {
            const Vec3 wp = TransformPoint(binding.bind_node_transform, {p[0], p[1], p[2]});
            for (int i = 0; i < 3; ++i)
            {
                lo[i] = std::min(lo[i], wp[i]);
                hi[i] = std::max(hi[i], wp[i]);
            }
        }
    }

    model_center_ = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    const float dx = hi[0] - lo[0];
    const float dy = hi[1] - lo[1];
    const float dz = hi[2] - lo[2];
    model_radius_ = std::max(0.1f, 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz));
}

void AnimatorPreviewRenderer::ResetView()
{
    yaw_ = 0.6f;
    pitch_ = 0.25f;
    distance_ = model_radius_ * 3.0f;
    focus_ = model_center_;
}

void AnimatorPreviewRenderer::SetActiveClip(const std::string& clip_name)
{
    active_clip_name_ = clip_name;
    current_time_seconds_ = 0.0f;
}

void AnimatorPreviewRenderer::SetCurrentTime(float seconds)
{
    current_time_seconds_ = std::max(0.0f, seconds);
}

float AnimatorPreviewRenderer::ClipDurationSeconds() const
{
    if (scene_ == nullptr || active_clip_name_.empty())
    {
        return 0.0f;
    }
    for (unsigned int i = 0; i < scene_->mNumAnimations; ++i)
    {
        const aiAnimation* anim = scene_->mAnimations[i];
        if (anim != nullptr && std::string(anim->mName.C_Str()) == active_clip_name_)
        {
            const double tps = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 25.0;
            return static_cast<float>(anim->mDuration / tps);
        }
    }
    if (scene_->mNumAnimations > 0)
    {
        const aiAnimation* anim = scene_->mAnimations[0];
        const double tps = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 25.0;
        return static_cast<float>(anim->mDuration / tps);
    }
    return 0.0f;
}

void AnimatorPreviewRenderer::Tick(float delta_seconds)
{
    PollPendingLoad();
    if (!playing_)
    {
        return;
    }
    current_time_seconds_ += delta_seconds * playback_speed_;
    const float duration = ClipDurationSeconds();
    if (duration > 0.0f)
    {
        current_time_seconds_ = std::fmod(current_time_seconds_, duration);
        if (current_time_seconds_ < 0.0f) { current_time_seconds_ += duration; }
    }
}

std::vector<std::string> AnimatorPreviewRenderer::SelectableBoneNames() const
{
    std::vector<std::string> names;
    names.reserve(skeleton_nodes_.size());
    for (const SkeletonNode& entry : skeleton_nodes_)
    {
        if (entry.node != nullptr)
        {
            names.emplace_back(entry.node->mName.C_Str());
        }
    }
    return names;
}

void AnimatorPreviewRenderer::SetBonePhysics(const std::vector<BonePhysicsParams>& params)
{
    // Preserve simulation state for bones whose name persisted between
    // frames so the spring doesn't reset every time the user nudges a slider.
    std::vector<JiggleRuntimeState> next_states;
    next_states.reserve(params.size());
    for (const BonePhysicsParams& p : params)
    {
        JiggleRuntimeState state;
        state.bone_name = p.bone_name;
        bool reused = false;
        for (const JiggleRuntimeState& prev : jiggle_states_)
        {
            if (prev.bone_name == p.bone_name && prev.initialized)
            {
                state = prev;
                reused = true;
                break;
            }
        }
        (void)reused;
        next_states.push_back(std::move(state));
    }
    bone_physics_ = params;
    jiggle_states_ = std::move(next_states);
}

void AnimatorPreviewRenderer::StepBonePhysics(float frame_seconds)
{
    if (bone_physics_.empty() || node_world_transforms_.empty())
    {
        return;
    }

    // Use a fixed timestep so the simulation behaves the same regardless of
    // editor frame rate. Cap total work to 4 substeps to avoid spiral of
    // death after long ImGui stalls.
    constexpr float kStep = 1.0f / 120.0f;
    constexpr int kMaxSteps = 4;
    physics_accumulator_seconds_ += std::max(0.0f, frame_seconds);
    int steps = 0;
    while (physics_accumulator_seconds_ >= kStep && steps < kMaxSteps)
    {
        physics_accumulator_seconds_ -= kStep;
        ++steps;
    }
    if (steps == 0)
    {
        return;
    }
    const float dt = kStep * static_cast<float>(steps);

    // Resolve each bone's node-index lookup once per call.
    for (std::size_t i = 0; i < bone_physics_.size(); ++i)
    {
        const BonePhysicsParams& p = bone_physics_[i];
        if (i >= jiggle_states_.size()) { break; }
        JiggleRuntimeState& s = jiggle_states_[i];
        if (p.bone_name.empty() || p.strength <= 0.0001f) { continue; }

        // Look up the bone's animated node by name.
        const aiNode* node = (scene_ != nullptr && scene_->mRootNode != nullptr)
            ? scene_->mRootNode->FindNode(p.bone_name.c_str())
            : nullptr;
        if (node == nullptr) { continue; }
        const auto node_it = node_index_.find(node);
        if (node_it == node_index_.end() || node_it->second >= node_world_transforms_.size()) { continue; }

        const Mat4& target_world = node_world_transforms_[node_it->second];
        const Vec3 target_pos{target_world[3], target_world[7], target_world[11]};

        if (!s.initialized)
        {
            s.sim_pos = {target_pos[0], target_pos[1], target_pos[2]};
            s.sim_vel = {0.0f, 0.0f, 0.0f};
            s.initialized = true;
            continue;
        }

        // Spring constants. Damping is interpreted as a critical-damping
        // fraction: damping=1 yields no overshoot, <1 oscillates, >1 is
        // sluggish. c/m = 2*frac*sqrt(k/m).
        const float stiff_k = 180.0f * std::max(0.0f, p.stiffness);
        const float mass = std::max(0.001f, p.mass);
        const float damp_k = 2.0f * std::max(0.0f, p.damping) * std::sqrt(stiff_k / mass);
        const float drag_k = std::clamp(p.drag, 0.0f, 1.0f);
        const float strength = std::clamp(p.strength, 0.0f, 2.0f);

        Vec3 accel{0, 0, 0};
        for (int axis = 0; axis < 3; ++axis)
        {
            const float displacement = target_pos[axis] - s.sim_pos[axis];
            accel[axis] = displacement * stiff_k / mass;
            accel[axis] += p.gravity_dir[axis] * p.gravity_scale * 9.81f;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            const float decay = std::clamp(1.0f - damp_k * dt, 0.0f, 1.0f);
            s.sim_vel[axis] *= decay;
            s.sim_vel[axis] += accel[axis] * dt;
            s.sim_vel[axis] *= (1.0f - drag_k * dt);
            s.sim_pos[axis] += s.sim_vel[axis] * dt;
        }

        // Optional cone clamp: limit how far the simulated point may deflect
        // from the animated direction relative to the parent. Skipped when
        // angle_limit is at its 180-degree extent (effectively disabled).
        if (p.angle_limit_deg < 179.5f && node->mParent != nullptr)
        {
            const auto parent_it = node_index_.find(node->mParent);
            if (parent_it != node_index_.end() && parent_it->second < node_world_transforms_.size())
            {
                const Mat4& parent_world = node_world_transforms_[parent_it->second];
                const Vec3 parent_pos{parent_world[3], parent_world[7], parent_world[11]};

                Vec3 anim_dir{target_pos[0] - parent_pos[0], target_pos[1] - parent_pos[1], target_pos[2] - parent_pos[2]};
                Vec3 sim_dir{s.sim_pos[0] - parent_pos[0], s.sim_pos[1] - parent_pos[1], s.sim_pos[2] - parent_pos[2]};
                const float anim_len = std::sqrt(anim_dir[0] * anim_dir[0] + anim_dir[1] * anim_dir[1] + anim_dir[2] * anim_dir[2]);
                const float sim_len = std::sqrt(sim_dir[0] * sim_dir[0] + sim_dir[1] * sim_dir[1] + sim_dir[2] * sim_dir[2]);
                if (anim_len > 1e-5f && sim_len > 1e-5f)
                {
                    const float inv_anim = 1.0f / anim_len;
                    const float inv_sim = 1.0f / sim_len;
                    const Vec3 anim_n{anim_dir[0] * inv_anim, anim_dir[1] * inv_anim, anim_dir[2] * inv_anim};
                    const Vec3 sim_n{sim_dir[0] * inv_sim, sim_dir[1] * inv_sim, sim_dir[2] * inv_sim};
                    const float dot = std::clamp(anim_n[0] * sim_n[0] + anim_n[1] * sim_n[1] + anim_n[2] * sim_n[2], -1.0f, 1.0f);
                    const float angle = std::acos(dot);
                    const float limit_rad = p.angle_limit_deg * (3.14159265f / 180.0f);
                    if (angle > limit_rad)
                    {
                        // Slerp sim direction toward anim direction so the
                        // angle equals the limit. Then re-place sim_pos.
                        const float t = (angle - limit_rad) / angle;
                        Vec3 corrected{
                            sim_n[0] * (1.0f - t) + anim_n[0] * t,
                            sim_n[1] * (1.0f - t) + anim_n[1] * t,
                            sim_n[2] * (1.0f - t) + anim_n[2] * t,
                        };
                        const float cl = std::sqrt(corrected[0] * corrected[0] + corrected[1] * corrected[1] + corrected[2] * corrected[2]);
                        if (cl > 1e-5f)
                        {
                            corrected[0] /= cl; corrected[1] /= cl; corrected[2] /= cl;
                            // Preserve simulated chain length so jiggle still translates.
                            for (int axis = 0; axis < 3; ++axis)
                            {
                                s.sim_pos[axis] = parent_pos[axis] + corrected[axis] * sim_len;
                            }
                        }
                    }
                }
            }
        }

        // Blend simulated position with the animated target by strength. At
        // strength=1.0 the simulation fully replaces the animated position;
        // at lower values we mix so the bone gently lags rather than
        // following the spring exclusively.
        Vec3 final_pos{
            target_pos[0] + (s.sim_pos[0] - target_pos[0]) * strength,
            target_pos[1] + (s.sim_pos[1] - target_pos[1]) * strength,
            target_pos[2] + (s.sim_pos[2] - target_pos[2]) * strength,
        };

        Mat4& world = node_world_transforms_[node_it->second];
        // Write only the translation column; orientation/scale remain whatever
        // EvaluateHierarchy produced so the limb still rotates with the clip.
        const Vec3 delta{final_pos[0] - target_pos[0], final_pos[1] - target_pos[1], final_pos[2] - target_pos[2]};
        world[3] = final_pos[0];
        world[7] = final_pos[1];
        world[11] = final_pos[2];

        // Translate descendants by the same delta so they ride along with
        // the simulated bone (cheap stand-in for chain propagation).
        if (p.affects_children && (delta[0] != 0.0f || delta[1] != 0.0f || delta[2] != 0.0f))
        {
            std::function<void(const aiNode*)> drift = [&](const aiNode* n)
            {
                for (unsigned int c = 0; c < n->mNumChildren; ++c)
                {
                    const aiNode* child = n->mChildren[c];
                    const auto cit = node_index_.find(child);
                    if (cit != node_index_.end() && cit->second < node_world_transforms_.size())
                    {
                        Mat4& cw = node_world_transforms_[cit->second];
                        cw[3] += delta[0];
                        cw[7] += delta[1];
                        cw[11] += delta[2];
                    }
                    drift(child);
                }
            };
            drift(node);
        }
    }
}

void AnimatorPreviewRenderer::RecomputeBoneMatricesFromNodeTransforms()
{
    // Recompute the bone matrices using the (possibly physics-modified) node
    // transforms. Mirrors the per-bone composition done inside EvaluateHierarchy.
    if (bones_.empty()) { return; }
    if (animated_bone_matrices_.size() != bones_.size())
    {
        animated_bone_matrices_.assign(bones_.size(), Identity());
    }
    for (std::size_t b = 0; b < bones_.size(); ++b)
    {
        const Bone& bone = bones_[b];
        const aiNode* node = (scene_ != nullptr && scene_->mRootNode != nullptr)
            ? scene_->mRootNode->FindNode(bone.name.c_str())
            : nullptr;
        if (node == nullptr) { continue; }
        const auto it = node_index_.find(node);
        if (it == node_index_.end() || it->second >= node_world_transforms_.size()) { continue; }
        const Mat4& world = node_world_transforms_[it->second];
        animated_bone_matrices_[b] = Multiply(Multiply(global_inverse_, world), bone.offset_matrix);
    }
}

// --- GPU rasterization implementation appended below ---
// === AnimatorPreviewRenderer: standalone Vulkan GPU rasterizer ============
// Lazy-initialized pipeline that renders the loaded animated model to an
// offscreen color attachment which ImGui samples via ImGui::Image. All
// matrices in this file follow the row-major convention used elsewhere in
// this translation unit; we transpose to column-major when uploading to GLSL.

namespace
{
// Vertex format pushed to the GPU. Mirror this exactly in the pipeline's
// vertex input description.
struct GpuVertex
{
    float position[3];
    float normal[3];
    float uv[2];
    std::int32_t bone_indices[4];
    float bone_weights[4];
};
static_assert(sizeof(GpuVertex) == 64, "GpuVertex layout drift would corrupt vertex buffer reads");

struct GpuMaterialBlock
{
    float base_color_factor[4]; // rgb tint, w = has_texture (0 or 1)
    float rendering_flags[4];   // x = use_skinning, yzw unused
};

// SDL is used by Scene2DRenderer to discover the executable directory; mirror
// that pattern so shaders found beside the engine.exe also work here.
std::filesystem::path AnimatorResolveShaderPath(const char* file_name)
{
    const char* base_raw = SDL_GetBasePath();
    const std::filesystem::path base = (base_raw != nullptr)
        ? std::filesystem::path(base_raw)
        : std::filesystem::current_path();
    return base / "shaders" / file_name;
}

VkShaderModule AnimatorLoadShaderModule(VkDevice device, const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return VK_NULL_HANDLE;
    }
    std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{});
    if (bytes.empty())
    {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes.size();
    ci.pCode = reinterpret_cast<const std::uint32_t*>(bytes.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS)
    {
        return VK_NULL_HANDLE;
    }
    return module;
}

// Helper for one-shot transient command-buffer submission. Mirrors the
// pattern used by UploadRgbaTexture in this file but accepts a caller-owned
// pool so we don't keep recreating it per frame.
void AnimatorSubmitImmediate(VulkanContext& ctx, VkCommandPool pool, const std::function<void(VkCommandBuffer)>& record)
{
    VkDevice device = ctx.GetDevice();
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cba = {};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool = pool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device, &cba, &cmd) != VK_SUCCESS) { return; }

    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
    {
        vkFreeCommandBuffers(device, pool, 1, &cmd);
        return;
    }
    record(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit = {};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(ctx.GetQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.GetQueue());
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

// Create a host-visible buffer + copy data into it; for small persistent
// uploads we don't need a staging step. Used for material UBOs and the bone
// SSBO (both updated every frame).
bool AnimatorCreateHostBuffer(
    VulkanContext& ctx,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkBuffer& out_buffer,
    VkDeviceMemory& out_memory,
    void** out_mapped)
{
    VkDevice device = ctx.GetDevice();
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bi, ctx.GetAllocator(), &out_buffer) != VK_SUCCESS) { return false; }

    VkMemoryRequirements req = {};
    vkGetBufferMemoryRequirements(device, out_buffer, &req);

    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(ctx.GetPhysicalDevice(), req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX
        || vkAllocateMemory(device, &ai, ctx.GetAllocator(), &out_memory) != VK_SUCCESS
        || vkBindBufferMemory(device, out_buffer, out_memory, 0) != VK_SUCCESS)
    {
        return false;
    }
    if (out_mapped != nullptr)
    {
        if (vkMapMemory(device, out_memory, 0, size, 0, out_mapped) != VK_SUCCESS)
        {
            return false;
        }
    }
    return true;
}

// Create a device-local buffer and upload data via a transient staging copy.
bool AnimatorCreateDeviceBuffer(
    VulkanContext& ctx,
    VkCommandPool pool,
    const void* data,
    VkDeviceSize size,
    VkBufferUsageFlags usage,
    VkBuffer& out_buffer,
    VkDeviceMemory& out_memory)
{
    VkDevice device = ctx.GetDevice();

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    if (!AnimatorCreateHostBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, staging, staging_mem, &mapped))
    {
        if (staging != VK_NULL_HANDLE) vkDestroyBuffer(device, staging, ctx.GetAllocator());
        if (staging_mem != VK_NULL_HANDLE) vkFreeMemory(device, staging_mem, ctx.GetAllocator());
        return false;
    }
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vkUnmapMemory(device, staging_mem);

    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bi, ctx.GetAllocator(), &out_buffer) != VK_SUCCESS)
    {
        vkDestroyBuffer(device, staging, ctx.GetAllocator());
        vkFreeMemory(device, staging_mem, ctx.GetAllocator());
        return false;
    }
    VkMemoryRequirements req = {};
    vkGetBufferMemoryRequirements(device, out_buffer, &req);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(ctx.GetPhysicalDevice(), req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX
        || vkAllocateMemory(device, &ai, ctx.GetAllocator(), &out_memory) != VK_SUCCESS
        || vkBindBufferMemory(device, out_buffer, out_memory, 0) != VK_SUCCESS)
    {
        vkDestroyBuffer(device, staging, ctx.GetAllocator());
        vkFreeMemory(device, staging_mem, ctx.GetAllocator());
        return false;
    }

    AnimatorSubmitImmediate(ctx, pool, [&](VkCommandBuffer cmd)
    {
        VkBufferCopy copy = {};
        copy.size = size;
        vkCmdCopyBuffer(cmd, staging, out_buffer, 1, &copy);
    });

    vkDestroyBuffer(device, staging, ctx.GetAllocator());
    vkFreeMemory(device, staging_mem, ctx.GetAllocator());
    return true;
}

// Transpose a 4x4 row-major matrix (our CPU convention) to the column-major
// layout GLSL expects when uploaded as `mat4`.
void TransposeToColumnMajor(const std::array<float, 16>& src, float dst[16])
{
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            dst[c * 4 + r] = src[r * 4 + c];
        }
    }
}
}

bool AnimatorPreviewRenderer::EnsureGpuPipeline(VulkanContext* vulkan_context)
{
    if (vulkan_context == nullptr) { return false; }
    if (gpu_pipeline_ready_) { return true; }
    if (gpu_pipeline_failed_) { return false; }

    VkDevice device = vulkan_context->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context->GetAllocator();
    texture_context_ = vulkan_context;

    // Persistent transient command pool used for one-shot uploads and the
    // per-frame offscreen render submission.
    VkCommandPoolCreateInfo pool_ci = {};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.queueFamilyIndex = vulkan_context->GetQueueFamily();
    pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(device, &pool_ci, allocator, &command_pool_) != VK_SUCCESS)
    {
        gpu_pipeline_failed_ = true; return false;
    }

    // 1x1 opaque white fallback so the fragment shader always has a valid
    // sampler binding, even on meshes whose material had no base color map.
    {
        const unsigned char white_pixel[4] = {255, 255, 255, 255};
        VkSampler tmp_sampler = VK_NULL_HANDLE;
        if (!UploadRgbaTexture(*vulkan_context, white_pixel, 1, 1, white_image_, white_memory_, white_view_, tmp_sampler))
        {
            gpu_pipeline_failed_ = true; return false;
        }
        default_sampler_ = tmp_sampler;
    }

    // Descriptor set layout: 0 = bone SSBO (vertex), 1 = sampler (frag), 2 = material UBO (vertex+frag).
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dsl_ci = {};
    dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl_ci.bindingCount = 3;
    dsl_ci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device, &dsl_ci, allocator, &descriptor_set_layout_) != VK_SUCCESS)
    {
        gpu_pipeline_failed_ = true; return false;
    }

    // Descriptor pool sized for a generous number of meshes. If a model
    // exceeds it we simply skip the remainder for that load (rare).
    constexpr std::uint32_t kMaxMeshes = 128;
    VkDescriptorPoolSize pool_sizes[3] = {};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[0].descriptorCount = kMaxMeshes;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = kMaxMeshes;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[2].descriptorCount = kMaxMeshes;

    VkDescriptorPoolCreateInfo dp_ci = {};
    dp_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp_ci.maxSets = kMaxMeshes;
    dp_ci.poolSizeCount = 3;
    dp_ci.pPoolSizes = pool_sizes;
    dp_ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    if (vkCreateDescriptorPool(device, &dp_ci, allocator, &descriptor_pool_) != VK_SUCCESS)
    {
        gpu_pipeline_failed_ = true; return false;
    }

    // Pipeline layout: one descriptor set + 128 bytes of push constants
    // (mat4 view_proj, mat4 model) used by the vertex shader only.
    VkPushConstantRange pc_range = {};
    pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc_range.offset = 0;
    pc_range.size = 128;

    VkPipelineLayoutCreateInfo pl_ci = {};
    pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &descriptor_set_layout_;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &pc_range;
    if (vkCreatePipelineLayout(device, &pl_ci, allocator, &pipeline_layout_) != VK_SUCCESS)
    {
        gpu_pipeline_failed_ = true; return false;
    }

    // Render pass: one color attachment (UNORM, transitions to SHADER_READ_ONLY)
    // and one depth attachment. The depth attachment is transient.
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[1].format = VK_FORMAT_D32_SFLOAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = {};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depth_ref = {};
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rp_ci = {};
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 2;
    rp_ci.pAttachments = attachments;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &subpass;
    rp_ci.dependencyCount = 2;
    rp_ci.pDependencies = deps;
    if (vkCreateRenderPass(device, &rp_ci, allocator, &render_pass_) != VK_SUCCESS)
    {
        gpu_pipeline_failed_ = true; return false;
    }

    // Shader modules. Locate next to the engine executable just like Scene2D.
    const char* base_path_raw = SDL_GetBasePath();
    const std::filesystem::path base_path = (base_path_raw != nullptr)
        ? std::filesystem::path(base_path_raw)
        : std::filesystem::current_path();
    VkShaderModule vert = AnimatorLoadShaderModule(device, base_path / "shaders" / "animator_preview.vert.spv");
    VkShaderModule frag = AnimatorLoadShaderModule(device, base_path / "shaders" / "animator_preview.frag.spv");
    if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
    {
        if (vert != VK_NULL_HANDLE) vkDestroyShaderModule(device, vert, allocator);
        if (frag != VK_NULL_HANDLE) vkDestroyShaderModule(device, frag, allocator);
        last_error_ = "AnimatorPreviewRenderer: shaders/animator_preview.*.spv missing beside engine executable";
        gpu_pipeline_failed_ = true; return false;
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

    VkVertexInputBindingDescription vbind = {};
    vbind.binding = 0;
    vbind.stride = sizeof(GpuVertex);
    vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription vattr[5] = {};
    vattr[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, position)};
    vattr[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, normal)};
    vattr[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT,    offsetof(GpuVertex, uv)};
    vattr[3] = {3, 0, VK_FORMAT_R32G32B32A32_SINT, offsetof(GpuVertex, bone_indices)};
    vattr[4] = {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, bone_weights)};

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vbind;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = vattr;

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // No culling: animation rigs sometimes import with mixed winding and we
    // care more about getting a complete silhouette than perfect culling.
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
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gp = {};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = pipeline_layout_;
    gp.renderPass = render_pass_;
    gp.subpass = 0;

    const VkResult pr = vkCreateGraphicsPipelines(device, vulkan_context->GetPipelineCache(), 1, &gp, allocator, &pipeline_);
    vkDestroyShaderModule(device, vert, allocator);
    vkDestroyShaderModule(device, frag, allocator);
    if (pr != VK_SUCCESS)
    {
        gpu_pipeline_failed_ = true; return false;
    }

    gpu_pipeline_ready_ = true;
    return true;
}

bool AnimatorPreviewRenderer::EnsureRenderTarget(VulkanContext* vulkan_context, std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0) { return false; }
    if (rt_color_image_ != VK_NULL_HANDLE && rt_width_ == width && rt_height_ == height) { return true; }

    VkDevice device = vulkan_context->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context->GetAllocator();
    vkDeviceWaitIdle(device);

    if (rt_imgui_descriptor_ != VK_NULL_HANDLE)
    {
        ImGui_ImplVulkan_RemoveTexture(rt_imgui_descriptor_);
        rt_imgui_descriptor_ = VK_NULL_HANDLE;
    }
    if (rt_framebuffer_ != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, rt_framebuffer_, allocator); rt_framebuffer_ = VK_NULL_HANDLE; }
    if (rt_color_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device, rt_color_view_, allocator); rt_color_view_ = VK_NULL_HANDLE; }
    if (rt_color_image_ != VK_NULL_HANDLE) { vkDestroyImage(device, rt_color_image_, allocator); rt_color_image_ = VK_NULL_HANDLE; }
    if (rt_color_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device, rt_color_memory_, allocator); rt_color_memory_ = VK_NULL_HANDLE; }
    if (rt_depth_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device, rt_depth_view_, allocator); rt_depth_view_ = VK_NULL_HANDLE; }
    if (rt_depth_image_ != VK_NULL_HANDLE) { vkDestroyImage(device, rt_depth_image_, allocator); rt_depth_image_ = VK_NULL_HANDLE; }
    if (rt_depth_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device, rt_depth_memory_, allocator); rt_depth_memory_ = VK_NULL_HANDLE; }

    auto make_image = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                          VkImage& image, VkDeviceMemory& memory, VkImageView& view) -> bool
    {
        VkImageCreateInfo ii = {};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {width, height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ii, allocator, &image) != VK_SUCCESS) { return false; }
        VkMemoryRequirements req = {};
        vkGetImageMemoryRequirements(device, image, &req);
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(vulkan_context->GetPhysicalDevice(), req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(device, &ai, allocator, &memory) != VK_SUCCESS
            || vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS)
        {
            return false;
        }
        VkImageViewCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange.aspectMask = aspect;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        return vkCreateImageView(device, &vi, allocator, &view) == VK_SUCCESS;
    };

    if (!make_image(VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,
            rt_color_image_, rt_color_memory_, rt_color_view_))
    {
        return false;
    }
    if (!make_image(VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT,
            rt_depth_image_, rt_depth_memory_, rt_depth_view_))
    {
        return false;
    }

    VkImageView fb_attachments[2] = {rt_color_view_, rt_depth_view_};
    VkFramebufferCreateInfo fb = {};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = render_pass_;
    fb.attachmentCount = 2;
    fb.pAttachments = fb_attachments;
    fb.width = width;
    fb.height = height;
    fb.layers = 1;
    if (vkCreateFramebuffer(device, &fb, allocator, &rt_framebuffer_) != VK_SUCCESS) { return false; }

    rt_imgui_descriptor_ = ImGui_ImplVulkan_AddTexture(default_sampler_, rt_color_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (rt_imgui_descriptor_ == VK_NULL_HANDLE) { return false; }

    rt_width_ = width;
    rt_height_ = height;
    return true;
}

bool AnimatorPreviewRenderer::EnsureBoneBuffer(VulkanContext* vulkan_context)
{
    const std::size_t needed = std::max<std::size_t>(animated_bone_matrices_.size(), 1u);
    if (bone_buffer_ != VK_NULL_HANDLE && needed <= bone_buffer_capacity_) { return true; }

    VkDevice device = vulkan_context->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context->GetAllocator();
    if (bone_buffer_mapped_ != nullptr && bone_buffer_memory_ != VK_NULL_HANDLE)
    {
        vkUnmapMemory(device, bone_buffer_memory_);
        bone_buffer_mapped_ = nullptr;
    }
    if (bone_buffer_ != VK_NULL_HANDLE) { vkDestroyBuffer(device, bone_buffer_, allocator); bone_buffer_ = VK_NULL_HANDLE; }
    if (bone_buffer_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device, bone_buffer_memory_, allocator); bone_buffer_memory_ = VK_NULL_HANDLE; }

    const VkDeviceSize size = static_cast<VkDeviceSize>(needed) * sizeof(float) * 16;
    if (!AnimatorCreateHostBuffer(*vulkan_context, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            bone_buffer_, bone_buffer_memory_, &bone_buffer_mapped_))
    {
        return false;
    }
    bone_buffer_size_ = size;
    bone_buffer_capacity_ = needed;
    return true;
}

bool AnimatorPreviewRenderer::EnsureGpuMeshes(VulkanContext* vulkan_context)
{
    if (mesh_bindings_.empty()) { return true; }
    if (gpu_meshes_.size() == mesh_bindings_.size())
    {
        // Check if textures became available since last frame; rewrite the
        // texture descriptor binding if so. Cheap because we only touch
        // entries whose texture status changed.
        for (std::size_t i = 0; i < gpu_meshes_.size(); ++i)
        {
            GpuMesh& gpu = gpu_meshes_[i];
            if (!gpu.uploaded) { continue; }
            const MeshBinding& binding = mesh_bindings_[i];
            VkImageView view = white_view_;
            VkSampler sampler = default_sampler_;
            float has_tex = 0.0f;
            if (binding.texture_index >= 0 && binding.texture_index < static_cast<int>(textures_.size()))
            {
                const GpuTexture& t = textures_[static_cast<std::size_t>(binding.texture_index)];
                if (t.uploaded && t.view != VK_NULL_HANDLE && t.sampler != VK_NULL_HANDLE)
                {
                    view = t.view;
                    sampler = t.sampler;
                    has_tex = 1.0f;
                }
            }

            // Refresh the material UBO every frame; cheap and lets toggles
            // (texture/skeleton) take effect immediately.
            if (gpu.material_ubo_mapped != nullptr)
            {
                GpuMaterialBlock block = {};
                block.base_color_factor[0] = binding.base_color[0];
                block.base_color_factor[1] = binding.base_color[1];
                block.base_color_factor[2] = binding.base_color[2];
                block.base_color_factor[3] = (show_texture_ ? has_tex : 0.0f);
                block.rendering_flags[0] = gpu.has_skin ? 1.0f : 0.0f;
                std::memcpy(gpu.material_ubo_mapped, &block, sizeof(block));
            }

            VkDescriptorImageInfo image_info = {};
            image_info.sampler = sampler;
            image_info.imageView = view;
            image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet write = {};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = gpu.descriptor_set;
            write.dstBinding = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &image_info;
            vkUpdateDescriptorSets(vulkan_context->GetDevice(), 1, &write, 0, nullptr);
        }
        return true;
    }

    // First time we see this model on the GPU: create vertex/index buffers
    // and descriptor sets for each mesh binding.
    gpu_meshes_.assign(mesh_bindings_.size(), GpuMesh{});
    VkDevice device = vulkan_context->GetDevice();
    const VkAllocationCallbacks* allocator = vulkan_context->GetAllocator();

    for (std::size_t i = 0; i < mesh_bindings_.size(); ++i)
    {
        const MeshBinding& binding = mesh_bindings_[i];
        GpuMesh& gpu = gpu_meshes_[i];

        if (binding.bind_positions.empty() || binding.indices.empty()) { continue; }

        std::vector<GpuVertex> vertices(binding.bind_positions.size(), GpuVertex{});
        for (std::size_t v = 0; v < binding.bind_positions.size(); ++v)
        {
            GpuVertex& gv = vertices[v];
            gv.position[0] = binding.bind_positions[v][0];
            gv.position[1] = binding.bind_positions[v][1];
            gv.position[2] = binding.bind_positions[v][2];
            gv.normal[0] = binding.bind_normals[v][0];
            gv.normal[1] = binding.bind_normals[v][1];
            gv.normal[2] = binding.bind_normals[v][2];
            gv.uv[0] = binding.bind_uvs[v][0];
            gv.uv[1] = binding.bind_uvs[v][1];
            if (v < binding.influences.size())
            {
                const SkinInfluence& inf = binding.influences[v];
                for (int s = 0; s < 4; ++s)
                {
                    gv.bone_indices[s] = std::max(inf.bone_indices[s], 0);
                    gv.bone_weights[s] = inf.bone_weights[s];
                }
            }
            else
            {
                for (int s = 0; s < 4; ++s) { gv.bone_indices[s] = 0; gv.bone_weights[s] = 0.0f; }
            }
        }

        if (!AnimatorCreateDeviceBuffer(*vulkan_context, command_pool_,
                vertices.data(), vertices.size() * sizeof(GpuVertex),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, gpu.vertex_buffer, gpu.vertex_memory))
        {
            continue;
        }
        if (!AnimatorCreateDeviceBuffer(*vulkan_context, command_pool_,
                binding.indices.data(), binding.indices.size() * sizeof(std::uint32_t),
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT, gpu.index_buffer, gpu.index_memory))
        {
            continue;
        }
        gpu.index_count = static_cast<std::uint32_t>(binding.indices.size());
        gpu.has_skin = !binding.influences.empty();

        // Material UBO: persistent mapped, refreshed each frame.
        if (!AnimatorCreateHostBuffer(*vulkan_context, sizeof(GpuMaterialBlock),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, gpu.material_ubo, gpu.material_ubo_memory, &gpu.material_ubo_mapped))
        {
            continue;
        }

        // Descriptor set for this mesh.
        VkDescriptorSetAllocateInfo asi = {};
        asi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        asi.descriptorPool = descriptor_pool_;
        asi.descriptorSetCount = 1;
        asi.pSetLayouts = &descriptor_set_layout_;
        if (vkAllocateDescriptorSets(device, &asi, &gpu.descriptor_set) != VK_SUCCESS)
        {
            continue;
        }

        // Make sure the bone buffer exists before we write the SSBO binding.
        if (!EnsureBoneBuffer(vulkan_context)) { continue; }

        VkDescriptorBufferInfo bone_info = {};
        bone_info.buffer = bone_buffer_;
        bone_info.offset = 0;
        bone_info.range = VK_WHOLE_SIZE;

        VkDescriptorImageInfo image_info = {};
        image_info.sampler = default_sampler_;
        image_info.imageView = white_view_;
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo mat_info = {};
        mat_info.buffer = gpu.material_ubo;
        mat_info.offset = 0;
        mat_info.range = sizeof(GpuMaterialBlock);

        VkWriteDescriptorSet writes[3] = {};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = gpu.descriptor_set;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bone_info;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = gpu.descriptor_set;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &image_info;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = gpu.descriptor_set;
        writes[2].dstBinding = 2;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo = &mat_info;
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

        gpu.uploaded = true;
    }
    (void)allocator;
    return true;
}

void AnimatorPreviewRenderer::DestroyGpu()
{
    if (texture_context_ == nullptr) { return; }
    VkDevice device = texture_context_->GetDevice();
    const VkAllocationCallbacks* allocator = texture_context_->GetAllocator();
    vkDeviceWaitIdle(device);

    for (GpuMesh& mesh : gpu_meshes_)
    {
        if (mesh.material_ubo_mapped != nullptr && mesh.material_ubo_memory != VK_NULL_HANDLE)
        {
            vkUnmapMemory(device, mesh.material_ubo_memory);
            mesh.material_ubo_mapped = nullptr;
        }
        if (mesh.material_ubo != VK_NULL_HANDLE) { vkDestroyBuffer(device, mesh.material_ubo, allocator); }
        if (mesh.material_ubo_memory != VK_NULL_HANDLE) { vkFreeMemory(device, mesh.material_ubo_memory, allocator); }
        if (mesh.vertex_buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, mesh.vertex_buffer, allocator); }
        if (mesh.vertex_memory != VK_NULL_HANDLE) { vkFreeMemory(device, mesh.vertex_memory, allocator); }
        if (mesh.index_buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, mesh.index_buffer, allocator); }
        if (mesh.index_memory != VK_NULL_HANDLE) { vkFreeMemory(device, mesh.index_memory, allocator); }
    }
    gpu_meshes_.clear();

    if (bone_buffer_mapped_ != nullptr && bone_buffer_memory_ != VK_NULL_HANDLE)
    {
        vkUnmapMemory(device, bone_buffer_memory_);
        bone_buffer_mapped_ = nullptr;
    }
    if (bone_buffer_ != VK_NULL_HANDLE) { vkDestroyBuffer(device, bone_buffer_, allocator); bone_buffer_ = VK_NULL_HANDLE; }
    if (bone_buffer_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device, bone_buffer_memory_, allocator); bone_buffer_memory_ = VK_NULL_HANDLE; }
    bone_buffer_size_ = 0;
    bone_buffer_capacity_ = 0;

    if (rt_imgui_descriptor_ != VK_NULL_HANDLE) { ImGui_ImplVulkan_RemoveTexture(rt_imgui_descriptor_); rt_imgui_descriptor_ = VK_NULL_HANDLE; }
    if (rt_framebuffer_ != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, rt_framebuffer_, allocator); rt_framebuffer_ = VK_NULL_HANDLE; }
    if (rt_color_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device, rt_color_view_, allocator); rt_color_view_ = VK_NULL_HANDLE; }
    if (rt_color_image_ != VK_NULL_HANDLE) { vkDestroyImage(device, rt_color_image_, allocator); rt_color_image_ = VK_NULL_HANDLE; }
    if (rt_color_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device, rt_color_memory_, allocator); rt_color_memory_ = VK_NULL_HANDLE; }
    if (rt_depth_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device, rt_depth_view_, allocator); rt_depth_view_ = VK_NULL_HANDLE; }
    if (rt_depth_image_ != VK_NULL_HANDLE) { vkDestroyImage(device, rt_depth_image_, allocator); rt_depth_image_ = VK_NULL_HANDLE; }
    if (rt_depth_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device, rt_depth_memory_, allocator); rt_depth_memory_ = VK_NULL_HANDLE; }
    rt_width_ = 0;
    rt_height_ = 0;

    if (descriptor_pool_ != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, descriptor_pool_, allocator); descriptor_pool_ = VK_NULL_HANDLE; }
    if (descriptor_set_layout_ != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, descriptor_set_layout_, allocator); descriptor_set_layout_ = VK_NULL_HANDLE; }
    if (pipeline_ != VK_NULL_HANDLE) { vkDestroyPipeline(device, pipeline_, allocator); pipeline_ = VK_NULL_HANDLE; }
    if (pipeline_layout_ != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, pipeline_layout_, allocator); pipeline_layout_ = VK_NULL_HANDLE; }
    if (render_pass_ != VK_NULL_HANDLE) { vkDestroyRenderPass(device, render_pass_, allocator); render_pass_ = VK_NULL_HANDLE; }
    if (default_sampler_ != VK_NULL_HANDLE) { vkDestroySampler(device, default_sampler_, allocator); default_sampler_ = VK_NULL_HANDLE; }
    if (white_view_ != VK_NULL_HANDLE) { vkDestroyImageView(device, white_view_, allocator); white_view_ = VK_NULL_HANDLE; }
    if (white_image_ != VK_NULL_HANDLE) { vkDestroyImage(device, white_image_, allocator); white_image_ = VK_NULL_HANDLE; }
    if (white_memory_ != VK_NULL_HANDLE) { vkFreeMemory(device, white_memory_, allocator); white_memory_ = VK_NULL_HANDLE; }
    if (command_pool_ != VK_NULL_HANDLE) { vkDestroyCommandPool(device, command_pool_, allocator); command_pool_ = VK_NULL_HANDLE; }

    gpu_pipeline_ready_ = false;
    gpu_pipeline_failed_ = false;
}

bool AnimatorPreviewRenderer::Render(VulkanContext* vulkan_context, const ImVec2& region_min, const ImVec2& region_max)
{
    PollPendingLoad();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->PushClipRect(region_min, region_max, true);
    draw_list->AddRectFilledMultiColor(
        region_min, region_max,
        IM_COL32(18, 20, 26, 255),
        IM_COL32(26, 31, 40, 255),
        IM_COL32(12, 14, 18, 255),
        IM_COL32(18, 22, 28, 255));
    draw_list->AddRect(region_min, region_max, IM_COL32(84, 92, 105, 255), 0.0f, 0, 1.5f);

    if (!HasModel())
    {
        // While the worker thread is parsing, show an animated "Loading…"
        // placeholder instead of the generic drop-target text.
        const bool is_loading = IsLoading();
        std::string placeholder_text;
        if (is_loading)
        {
            const int dots = static_cast<int>(ImGui::GetTime() * 2.0f) % 4;
            placeholder_text = "Loading";
            placeholder_text.append(dots, '.');
        }
        else
        {
            placeholder_text = last_error_.empty() ? "Drop a model here" : last_error_;
        }
        const ImVec2 text_size = ImGui::CalcTextSize(placeholder_text.c_str());
        draw_list->AddText(
            ImVec2((region_min.x + region_max.x - text_size.x) * 0.5f, (region_min.y + region_max.y - text_size.y) * 0.5f),
            is_loading ? IM_COL32(180, 200, 230, 255) : IM_COL32(220, 226, 236, 255),
            placeholder_text.c_str());
        draw_list->PopClipRect();
        return false;
    }

    // ---- Camera input (orbit / zoom). Drives view matrix below. ----------
    const ImVec2 size(region_max.x - region_min.x, region_max.y - region_min.y);
    ImGui::SetCursorScreenPos(region_min);
    ImGui::InvisibleButton("##AnimatorPreviewInput", size);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        {
            yaw_ += io.MouseDelta.x * 0.01f;
            pitch_ += io.MouseDelta.y * 0.01f;
            pitch_ = std::clamp(pitch_, -1.5f, 1.5f);
        }
        if (io.MouseWheel != 0.0f)
        {
            distance_ *= std::pow(0.9f, io.MouseWheel);
            distance_ = std::clamp(distance_, model_radius_ * 0.2f, model_radius_ * 20.0f);
        }
    }

    // ---- Set up GPU resources lazily. ------------------------------------
    if (!EnsureGpuPipeline(vulkan_context))
    {
        const char* msg = last_error_.empty() ? "GPU init failed" : last_error_.c_str();
        const ImVec2 text_size = ImGui::CalcTextSize(msg);
        draw_list->AddText(
            ImVec2((region_min.x + region_max.x - text_size.x) * 0.5f, (region_min.y + region_max.y - text_size.y) * 0.5f),
            IM_COL32(255, 180, 180, 255), msg);
        draw_list->PopClipRect();
        return false;
    }

    const std::uint32_t target_w = std::max(1u, static_cast<std::uint32_t>(size.x));
    const std::uint32_t target_h = std::max(1u, static_cast<std::uint32_t>(size.y));
    if (!EnsureRenderTarget(vulkan_context, target_w, target_h))
    {
        draw_list->PopClipRect();
        return false;
    }

    EnsureTextures(vulkan_context);
    if (!EnsureGpuMeshes(vulkan_context))
    {
        draw_list->PopClipRect();
        return false;
    }

    // ---- Evaluate animation (CPU); writes animated_bone_matrices_. -------
    EvaluateHierarchy(scene_->mRootNode, Identity(), current_time_seconds_);

    // ---- Post-process: spring-damper bone physics (jiggle). --------------
    // Advance simulation in real time so paused/scrubbing visuals reflect
    // the latest parameter tweaks but the springs don't drift around when
    // playback is paused.
    StepBonePhysics(ImGui::GetIO().DeltaTime);
    RecomputeBoneMatricesFromNodeTransforms();

    if (!EnsureBoneBuffer(vulkan_context))
    {
        draw_list->PopClipRect();
        return false;
    }

    // Upload bone matrices, transposing row-major -> column-major for GLSL.
    if (bone_buffer_mapped_ != nullptr && !animated_bone_matrices_.empty())
    {
        float* dst = static_cast<float*>(bone_buffer_mapped_);
        for (std::size_t b = 0; b < animated_bone_matrices_.size(); ++b)
        {
            TransposeToColumnMajor(animated_bone_matrices_[b], dst + b * 16);
        }
    }
    else if (bone_buffer_mapped_ != nullptr)
    {
        // Zero-bone case: write an identity so the SSBO read is well-defined.
        float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::memcpy(bone_buffer_mapped_, ident, sizeof(ident));
    }

    // ---- Build view / projection matrices. -------------------------------
    const Vec3 eye{
        focus_[0] + distance_ * std::cos(pitch_) * std::sin(yaw_),
        focus_[1] + distance_ * std::sin(pitch_),
        focus_[2] + distance_ * std::cos(pitch_) * std::cos(yaw_),
    };
    Mat4 view{};
    BuildLookAt(eye, focus_, Vec3{0, 1, 0}, view);

    const float aspect = (size.y > 0.0f) ? size.x / size.y : 1.0f;
    Mat4 proj{};
    BuildPerspective(0.85f, aspect, 0.01f, std::max(10.0f, model_radius_ * 100.0f), proj);
    // Vulkan clip space has Y inverted vs OpenGL; flip the projection's Y so
    // the image isn't upside-down when we sample it back as an ImGui texture.
    proj[5] = -proj[5];

    const Mat4 view_proj = Multiply(proj, view);
    float vp_column[16];
    TransposeToColumnMajor(view_proj, vp_column);

    // ---- Record + submit the offscreen render pass. ----------------------
    AnimatorSubmitImmediate(*vulkan_context, command_pool_, [&](VkCommandBuffer cmd)
    {
        VkClearValue clears[2] = {};
        clears[0].color = {{0.07f, 0.08f, 0.10f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rpb = {};
        rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpb.renderPass = render_pass_;
        rpb.framebuffer = rt_framebuffer_;
        rpb.renderArea.offset = {0, 0};
        rpb.renderArea.extent = {rt_width_, rt_height_};
        rpb.clearValueCount = 2;
        rpb.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(rt_width_);
        viewport.height = static_cast<float>(rt_height_);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor = {{0, 0}, {rt_width_, rt_height_}};
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

        for (std::size_t i = 0; i < gpu_meshes_.size(); ++i)
        {
            const GpuMesh& mesh = gpu_meshes_[i];
            if (!mesh.uploaded || mesh.index_count == 0) { continue; }
            if (!show_mesh_solid_) { continue; }

            const MeshBinding& binding = mesh_bindings_[i];

            // model matrix: when skinned the bone matrices already carry the
            // global transform (global_inverse * world * offset), so the model
            // matrix is just identity. For static meshes apply the bind-pose
            // node transform so the mesh sits in the correct place.
            Mat4 model = Identity();
            if (!mesh.has_skin) { model = binding.bind_node_transform; }

            float push[32];
            TransposeToColumnMajor(view_proj, push);
            TransposeToColumnMajor(model, push + 16);
            vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &mesh.descriptor_set, 0, nullptr);
            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertex_buffer, &offset);
            vkCmdBindIndexBuffer(cmd, mesh.index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh.index_count, 1, 0, 0, 0);
        }

        vkCmdEndRenderPass(cmd);
    });

    // ---- Composite the rendered image into the panel. --------------------
    if (rt_imgui_descriptor_ != VK_NULL_HANDLE)
    {
        draw_list->AddImage(reinterpret_cast<ImTextureID>(rt_imgui_descriptor_), region_min, region_max);
    }

    // ---- Optional ImGui overlays drawn on top of the rendered image. -----
    if (show_skeleton_)
    {
        std::vector<ImVec2> screen_positions(skeleton_nodes_.size());
        std::vector<bool> visible(skeleton_nodes_.size(), false);
        std::vector<int> depths(skeleton_nodes_.size(), 0);

        // Skeleton overlay uses the SAME view_proj as the GPU draw, but with
        // the un-flipped projection because ProjectPoint maps NDC Y back to
        // top-down screen pixels (matching ImGui's coord system).
        Mat4 proj_overlay{};
        BuildPerspective(0.85f, aspect, 0.01f, std::max(10.0f, model_radius_ * 100.0f), proj_overlay);
        const Mat4 view_proj_overlay = Multiply(proj_overlay, view);

        for (std::size_t i = 0; i < skeleton_nodes_.size(); ++i)
        {
            const SkeletonNode& entry = skeleton_nodes_[i];
            const auto it = node_index_.find(entry.node);
            if (it == node_index_.end()) { continue; }
            const Mat4& world_matrix = node_world_transforms_[it->second];
            const Vec3 origin = TransformPoint(world_matrix, Vec3{0, 0, 0});
            float w, d;
            visible[i] = ProjectPoint(origin, view_proj_overlay, region_min, region_max, screen_positions[i], w, d);
            depths[i] = (entry.parent_skeleton_index >= 0) ? (depths[entry.parent_skeleton_index] + 1) : 0;
        }

        // Bone picking: if the user clicked inside the preview region and we
        // are hovering a joint marker, switch the selection. Click on empty
        // space clears. We hit-test against the visible projected joints.
        const bool clicked_in_view = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f);
        if (clicked_in_view)
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            constexpr float kPickRadiusPixels = 7.0f;
            int picked = -1;
            float best_dist_sq = kPickRadiusPixels * kPickRadiusPixels;
            for (std::size_t i = 0; i < skeleton_nodes_.size(); ++i)
            {
                if (!visible[i]) { continue; }
                const float dx = screen_positions[i].x - mouse.x;
                const float dy = screen_positions[i].y - mouse.y;
                const float dsq = dx * dx + dy * dy;
                if (dsq < best_dist_sq)
                {
                    best_dist_sq = dsq;
                    picked = static_cast<int>(i);
                }
            }
            if (picked >= 0 && skeleton_nodes_[picked].node != nullptr)
            {
                selected_bone_name_ = std::string(skeleton_nodes_[picked].node->mName.C_Str());
            }
            else
            {
                selected_bone_name_.clear();
            }
        }

        // Resolve the skeleton index for the current selection so we can
        // highlight its joint + parent bone segment.
        int selected_skel_idx = -1;
        if (!selected_bone_name_.empty())
        {
            for (std::size_t i = 0; i < skeleton_nodes_.size(); ++i)
            {
                if (skeleton_nodes_[i].node != nullptr
                    && selected_bone_name_ == skeleton_nodes_[i].node->mName.C_Str())
                {
                    selected_skel_idx = static_cast<int>(i);
                    break;
                }
            }
        }

        for (std::size_t i = 0; i < skeleton_nodes_.size(); ++i)
        {
            const SkeletonNode& entry = skeleton_nodes_[i];
            if (entry.parent_skeleton_index < 0) { continue; }
            if (!visible[i] || !visible[entry.parent_skeleton_index]) { continue; }
            const bool selected_segment = (selected_skel_idx >= 0)
                && (static_cast<int>(i) == selected_skel_idx
                    || entry.parent_skeleton_index == selected_skel_idx);
            const ImU32 color = selected_segment
                ? IM_COL32(255, 220, 60, 255)
                : BoneColor(depths[i]);
            const float thickness = selected_segment ? 3.0f : 2.0f;
            draw_list->AddLine(screen_positions[entry.parent_skeleton_index], screen_positions[i], color, thickness);
        }
        for (std::size_t i = 0; i < skeleton_nodes_.size(); ++i)
        {
            if (!visible[i]) { continue; }
            const bool is_selected = (static_cast<int>(i) == selected_skel_idx);
            if (is_selected)
            {
                draw_list->AddCircleFilled(screen_positions[i], 6.0f, IM_COL32(255, 220, 60, 255), 12);
                draw_list->AddCircle(screen_positions[i], 8.0f, IM_COL32(255, 255, 255, 255), 16, 1.5f);
            }
            else
            {
                draw_list->AddCircleFilled(screen_positions[i], 3.0f, IM_COL32(255, 255, 255, 230), 8);
            }
        }
    }

    draw_list->PopClipRect();
    return true;
}

