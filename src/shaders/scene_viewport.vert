#version 450

layout(push_constant) uniform ScenePushConstants
{
    mat4 model;
    mat4 model_view_projection;
    vec4 ambient_light;
    vec4 directional_light_color;
    vec4 directional_light_direction;
    vec4 spot_light_color;
    vec4 spot_light_direction;
    vec4 spot_light_position;
    vec4 spot_light_data;
} scene_push_constants;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) out vec3 out_world_normal;
layout(location = 3) out vec3 out_world_position;

void main()
{
    vec4 world_position = scene_push_constants.model * vec4(in_position, 1.0);
    mat3 normal_matrix = mat3(scene_push_constants.model);

    out_uv = in_uv;
    out_color = in_color;
    out_world_normal = normalize(normal_matrix * in_normal);
    out_world_position = world_position.xyz;
    gl_Position = scene_push_constants.model_view_projection * vec4(in_position, 1.0);
}