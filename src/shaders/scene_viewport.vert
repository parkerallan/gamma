#version 450

layout(set = 1, binding = 0) uniform SceneUniforms
{
    mat4 model;
    mat4 model_view_projection;
} scene_uniforms;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) out float out_lighting;

void main()
{
    mat3 normal_matrix = mat3(scene_uniforms.model);
    vec3 normal = normalize(normal_matrix * in_normal);
    vec3 light_direction = normalize(vec3(0.45, 0.7, 0.35));

    out_uv = in_uv;
    out_color = in_color;
    out_lighting = 0.35 + 0.65 * max(dot(normal, light_direction), 0.0);
    gl_Position = scene_uniforms.model_view_projection * vec4(in_position, 1.0);
}