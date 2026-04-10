#version 450

layout(set = 0, binding = 0) uniform sampler2D base_color_texture;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) in float in_lighting;

layout(location = 0) out vec4 out_color;

void main()
{
    vec4 albedo = texture(base_color_texture, in_uv) * in_color;
    out_color = vec4(albedo.rgb * in_lighting, albedo.a);
}