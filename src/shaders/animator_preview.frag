#version 460

// Animator preview fragment shader: textured + simple diffuse lighting.

layout(location = 0) in vec3 in_world_normal;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 1) uniform sampler2D base_color_texture;

layout(set = 0, binding = 2, std140) uniform MaterialBlock {
    vec4 base_color_factor; // xyz = tint, w = has_texture (0 or 1)
    vec4 rendering_flags;
} material;

void main()
{
    vec3 normal = normalize(in_world_normal);
    // Fixed light direction in view space-ish: matches the camera-relative
    // light used by the previous CPU preview, so lighting reads consistently.
    vec3 light_dir = normalize(vec3(0.4, 0.7, 0.6));
    float ndotl = max(dot(normal, light_dir), 0.0);
    float ambient = 0.30;
    float shade = clamp(ambient + (1.0 - ambient) * ndotl, 0.0, 1.0);

    vec3 albedo = material.base_color_factor.rgb;
    if (material.base_color_factor.w > 0.5)
    {
        vec4 tex = texture(base_color_texture, in_uv);
        albedo = tex.rgb * material.base_color_factor.rgb;
    }

    out_color = vec4(albedo * shade, 1.0);
}
