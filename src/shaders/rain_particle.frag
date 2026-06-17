#version 460
#extension GL_EXT_scalar_block_layout : require

// Rain streak shading + depth occlusion. The streak shape is a radial gradient
// brightest at the bottom-centre of the quad (the drop), fading up/out — the
// same `exp(distance(uv,(.5,0)))` look as the three.js reference. Each fragment
// is occluded against the ray-traced scene depth so drops vanish behind geometry
// from the camera; sky pixels carry a 1e30 sentinel so rain shows there.

layout(set = 0, binding = 1, std140) uniform RainView
{
    mat4 view_projection;
    mat4 plane_to_world;
    vec4 cam_pos;
    vec4 color;       // rgb tint, a = opacity
    vec4 params;      // x = streak_width, y = streak_length, z = time, w = column_height
    vec4 depth_size;  // xy = scene-depth image dimensions
} u;

// Ray-traced linear depth (rg32f): .r = primary-ray hit distance from the camera
// (1e30 on a miss / sky). Matches RayTracing's current-frame depth image.
layout(set = 0, binding = 2, rg32f) uniform readonly image2D scene_depth;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec3 in_world;

layout(location = 0) out vec4 out_color;

void main()
{
    float d = distance(in_uv, vec2(0.5, 0.0));
    float streak = clamp(0.1 * exp(3.0 * (1.0 - d)), 0.0, 1.0);
    if (streak < 0.01)
    {
        discard;
    }

    // Soft depth occlusion against the ray-traced scene.
    vec2 dsz = max(u.depth_size.xy, vec2(1.0));
    ivec2 texel = ivec2(clamp(gl_FragCoord.xy, vec2(0.0), dsz - 1.0));
    float scene_dist = imageLoad(scene_depth, texel).r;
    float part_dist = length(in_world - u.cam_pos.xyz);
    float occlusion = clamp((scene_dist - part_dist) / 0.5, 0.0, 1.0);
    if (occlusion <= 0.0)
    {
        discard;
    }

    float alpha = streak * u.color.a * occlusion;
    vec3 col = u.color.rgb * (0.6 + 0.4 * streak);
    out_color = vec4(col, alpha);
}
