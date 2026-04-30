#version 450

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform PushConstants
{
    vec4 color; // rgba tint
} pc;

void main()
{
    vec4 sampled = texture(tex, in_uv);
    out_color = sampled * pc.color;
}
