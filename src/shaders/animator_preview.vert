#version 460

// Animator preview: skinned, textured vertex shader.
// Standalone pipeline used only by the editor's Animator panel.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in ivec4 in_bone_indices;
layout(location = 4) in vec4 in_bone_weights;

layout(location = 0) out vec3 out_world_normal;
layout(location = 1) out vec2 out_uv;

layout(push_constant) uniform PushConstants {
    mat4 view_projection;
    mat4 model;
} pc;

layout(set = 0, binding = 0, std430) readonly buffer BoneMatrices {
    mat4 bones[];
} bone_buffer;

layout(set = 0, binding = 2, std140) uniform MaterialBlock {
    vec4 base_color_factor;
    vec4 rendering_flags; // x = use_skinning (0 or 1)
} material;

void main()
{
    vec4 base_pos = vec4(in_position, 1.0);
    vec3 base_n   = in_normal;
    vec4 skinned_pos = base_pos;
    vec3 skinned_n   = base_n;

    if (material.rendering_flags.x > 0.5)
    {
        mat4 skin =
              bone_buffer.bones[max(in_bone_indices.x, 0)] * in_bone_weights.x
            + bone_buffer.bones[max(in_bone_indices.y, 0)] * in_bone_weights.y
            + bone_buffer.bones[max(in_bone_indices.z, 0)] * in_bone_weights.z
            + bone_buffer.bones[max(in_bone_indices.w, 0)] * in_bone_weights.w;

        float weight_sum = in_bone_weights.x + in_bone_weights.y + in_bone_weights.z + in_bone_weights.w;
        if (weight_sum > 0.0001)
        {
            skinned_pos = skin * base_pos;
            skinned_n   = mat3(skin) * base_n;
        }
    }

    vec4 world_pos = pc.model * skinned_pos;
    out_world_normal = mat3(pc.model) * skinned_n;
    out_uv = in_uv;
    gl_Position = pc.view_projection * world_pos;
}
