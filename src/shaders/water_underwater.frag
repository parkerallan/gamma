#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform WaterUnderwaterUniforms
{
    mat4 view;
    mat4 projection;
    mat4 inv_view;
    mat4 inv_projection;
    vec4 viewport_time;
    vec4 wave_settings_a;
    vec4 wave_settings_b;
    vec4 underwater_settings_a;
    vec4 underwater_settings_b;
    vec4 refraction_settings;
} uniforms;

layout(set = 0, binding = 1) uniform sampler2D scene_color_texture;
layout(set = 0, binding = 2) uniform sampler2D scene_depth_texture;

const int ITER_GEOMETRY = 3;
const mat2 octave_m = mat2(vec2(1.6, 1.2), vec2(-1.2, 1.6));

float hash12(vec2 p)
{
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334677u, 3812015801u);
    uint n = (q.x ^ q.y) * 1597334677u;
    return float(n) * (1.0 / 4294967295.0);
}

float noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return -1.0 + 2.0 * mix(
        mix(hash12(i + vec2(0.0, 0.0)), hash12(i + vec2(1.0, 0.0)), u.x),
        mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

float sea_octave(vec2 uv, float choppy)
{
    uv += noise(uv);
    vec2 wv = 1.0 - abs(sin(uv));
    vec2 swv = abs(cos(uv));
    wv = mix(wv, swv, wv);
    return pow(1.0 - pow(wv.x * wv.y, 0.65), choppy);
}

float reconstruct_surface_height(vec3 world_pos)
{
    float freq = uniforms.wave_settings_a.w;
    float amp = uniforms.wave_settings_a.x;
    float choppy = uniforms.wave_settings_a.y;
    float sea_speed = uniforms.wave_settings_a.z;
    vec2 uv = world_pos.xz;
    uv.x *= 0.75;
    float height = 0.0;
    for (int i = 0; i < ITER_GEOMETRY; ++i)
    {
        float d = sea_octave((uv + uniforms.viewport_time.z * sea_speed) * freq, choppy);
        d += sea_octave((uv - uniforms.viewport_time.z * sea_speed) * freq, choppy);
        height += d * amp;
        uv *= octave_m;
        freq *= 1.9;
        amp *= 0.22;
        choppy = mix(choppy, 1.0, 0.2);
    }
    return height + uniforms.wave_settings_b.x + uniforms.wave_settings_b.y;
}

void main()
{
    float depth_raw = texture(scene_depth_texture, in_uv).x;
    vec3 ndc = vec3(in_uv * 2.0 - 1.0, depth_raw);
    vec4 view_pos = uniforms.inv_projection * vec4(ndc, 1.0);
    view_pos /= max(view_pos.w, 0.0001);
    vec4 world_pos4 = uniforms.inv_view * vec4(view_pos.xyz, 1.0);
    vec3 world_pos = world_pos4.xyz / max(world_pos4.w, 0.0001);

    float surface_y = reconstruct_surface_height(world_pos);
    if (world_pos.y > surface_y)
    {
        discard;
    }

    vec2 noise_dir = vec2(
        noise(in_uv * 12.0 + uniforms.viewport_time.z * uniforms.refraction_settings.y * vec2(1.0, 0.4)),
        noise(in_uv * 12.0 + uniforms.viewport_time.z * uniforms.refraction_settings.y * vec2(0.6, 1.0))) * 2.0 - 1.0;
    vec2 uv_distorted = in_uv + noise_dir * uniforms.refraction_settings.x;

    vec3 scene_color = texture(scene_color_texture, uv_distorted).rgb;
    float distorted_depth = texture(scene_depth_texture, uv_distorted).x;
    vec3 distorted_ndc = vec3(uv_distorted * 2.0 - 1.0, distorted_depth);
    vec4 distorted_view = uniforms.inv_projection * vec4(distorted_ndc, 1.0);
    distorted_view /= max(distorted_view.w, 0.0001);
    float scene_view_z = -distorted_view.z;

    float thickness = max(scene_view_z - (-view_pos.z), 0.0);
    float absorption_strength = uniforms.underwater_settings_a.w;
    float transmittance = exp(-thickness * absorption_strength);
    vec3 underwater_color = uniforms.underwater_settings_a.rgb;
    vec3 tinted = mix(scene_color, underwater_color, uniforms.underwater_settings_b.x);
    vec3 final_color = tinted * transmittance;
    float fog = 1.0 - exp(-thickness * absorption_strength * 0.7);
    final_color = mix(final_color, underwater_color, fog * 0.85);

    out_color = vec4(final_color, 1.0);
}
