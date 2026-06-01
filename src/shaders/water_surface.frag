#version 450

layout(location = 0) in vec3 in_world_vert;
layout(location = 1) in float in_wave_height;
layout(location = 2) in vec3 in_vertex_normal_world;
layout(location = 3) in vec3 in_view_position;
layout(location = 4) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0, std140) uniform WaterSurfaceUniforms
{
    mat4 view;
    mat4 projection;
    mat4 inv_view;
    mat4 inv_projection;
    vec4 viewport_time;
    vec4 depth_settings;
    vec4 wave_settings_a;
    vec4 wave_settings_b;
    vec4 refraction_settings;
    vec4 tint_settings_a;
    vec4 tint_settings_b;
    vec4 pbr_settings;
    vec4 caustics_settings_a;
    vec4 caustics_settings_b;
    vec4 foam_settings_a;
    vec4 foam_settings_b;
    vec4 foam_settings_c;
} uniforms;

layout(set = 0, binding = 1) uniform sampler2D scene_color_texture;
layout(set = 0, binding = 2, r32f) uniform readonly image2D scene_depth_texture;
layout(set = 0, binding = 3) uniform sampler2D caustics_texture;

const int ITER_FRAGMENT = 5;
const mat2 octave_m = mat2(vec2(1.6, 1.2), vec2(-1.2, 1.6));

float hash12(vec2 p)
{
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334677u, 3812015801u);
    uint n = (q.x ^ q.y) * 1597334677u;
    return float(n) * (1.0 / 4294967295.0);
}

float hash13(vec3 p)
{
    uvec3 q = uvec3(ivec3(p)) * uvec3(1597334677u, 3812015801u, 2798796415u);
    uint n = (q.x ^ q.y ^ q.z) * 1597334677u;
    return float(n) * (1.0 / 4294967295.0);
}

float hash3d(vec3 p)
{
    return hash13(p);
}

vec2 hash22(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx + p3.yz) * p3.zy);
}

float noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return -1.0 + 2.0 * mix(
        mix(hash12(i + vec2(0.0, 0.0)), hash12(i + vec2(1.0, 0.0)), u.x),
        mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x),
        u.y);
}

float sea_octave(vec2 uv, float choppy)
{
    uv += noise(uv);
    vec2 wv = 1.0 - abs(sin(uv));
    vec2 swv = abs(cos(uv));
    wv = mix(wv, swv, wv);
    return pow(1.0 - pow(wv.x * wv.y, 0.65), choppy);
}

float map_detailed(vec3 p, float time_value)
{
    float freq = uniforms.wave_settings_a.w;
    float amp = uniforms.wave_settings_a.x;
    float choppy = uniforms.wave_settings_a.y;
    float sea_speed = uniforms.wave_settings_a.z;
    vec2 uv = p.xz;
    uv.x *= 0.75;
    float h = 0.0;
    for (int i = 0; i < ITER_FRAGMENT; ++i)
    {
        float d = sea_octave((uv + time_value * sea_speed) * freq, choppy);
        d += sea_octave((uv - time_value * sea_speed) * freq, choppy);
        h += d * amp;
        uv *= octave_m;
        freq *= 1.9;
        amp *= 0.22;
        choppy = mix(choppy, 1.0, 0.2);
    }
    return p.y - h;
}

vec3 get_normal_detailed(vec3 p, float eps, float time_value)
{
    vec3 n;
    n.y = map_detailed(p, time_value);
    n.x = map_detailed(vec3(p.x + eps, p.y, p.z), time_value) - n.y;
    n.z = map_detailed(vec3(p.x, p.y, p.z + eps), time_value) - n.y;
    n.y = eps;
    return normalize(n);
}

float voronoi(vec2 uv)
{
    vec2 n = floor(uv);
    vec2 f = fract(uv);
    float m_dist = 1.0;
    for (int j = -1; j <= 1; ++j)
    {
        for (int i = -1; i <= 1; ++i)
        {
            vec2 g = vec2(float(i), float(j));
            vec2 o = hash22(n + g);
            o = 0.5 + 0.5 * sin(uniforms.viewport_time.z * uniforms.foam_settings_a.w + 6.2831 * o);
            vec2 r = g - f + o;
            float d = dot(r, r);
            m_dist = min(m_dist, d);
        }
    }
    return m_dist;
}

float noise3d(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(
            mix(hash3d(i + vec3(0.0, 0.0, 0.0)), hash3d(i + vec3(1.0, 0.0, 0.0)), f.x),
            mix(hash3d(i + vec3(0.0, 1.0, 0.0)), hash3d(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
        mix(
            mix(hash3d(i + vec3(0.0, 0.0, 1.0)), hash3d(i + vec3(1.0, 0.0, 1.0)), f.x),
            mix(hash3d(i + vec3(0.0, 1.0, 1.0)), hash3d(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}

float fbm_voronoi(vec2 uv)
{
    float value = 0.0;
    float amplitude = 0.5;
    vec2 shift = vec2(100.0);
    mat2 rotation = mat2(vec2(cos(0.5), sin(0.5)), vec2(-sin(0.5), cos(0.5)));
    for (int i = 0; i < 3; ++i)
    {
        float cell_value = 1.0 - voronoi(uv);
        cell_value = pow(cell_value, 2.0);
        value += amplitude * cell_value;
        uv = rotation * uv * 2.0 + shift;
        amplitude *= 0.5;
    }
    return value;
}

float get_linear_depth(vec2 uv)
{
    ivec2 size = imageSize(scene_depth_texture);
    vec2 clamped_uv = clamp(uv, vec2(0.0), vec2(1.0));
    ivec2 texel = ivec2(clamped_uv * vec2(max(size - ivec2(1), ivec2(0))));
    return imageLoad(scene_depth_texture, texel).x;
}

void main()
{
    vec2 viewport_size = max(uniforms.viewport_time.xy, vec2(1.0));
    vec2 screen_uv = gl_FragCoord.xy / viewport_size;
    float time_value = uniforms.viewport_time.z;
    float max_depth = max(uniforms.depth_settings.x, 0.001);
    float fade_start_depth = uniforms.depth_settings.y;

    float dist_to_cam = length(in_view_position);
    float lod_epsilon = max(uniforms.refraction_settings.z, dist_to_cam * 0.005);
    vec3 detailed_normal = get_normal_detailed(in_world_vert, lod_epsilon, time_value);
    float smooth_factor = clamp((dist_to_cam - uniforms.refraction_settings.w) / 200.0, 0.0, 1.0);
    vec3 final_normal = normalize(mix(detailed_normal, in_vertex_normal_world, smooth_factor));

    float water_linear_depth = -in_view_position.z;
    float bg_linear_depth = get_linear_depth(screen_uv);
    float thickness = max(0.0, bg_linear_depth - water_linear_depth);

    vec3 view_vertex_normal = normalize((uniforms.view * vec4(in_vertex_normal_world, 0.0)).xyz);
    vec3 view_final_normal = normalize((uniforms.view * vec4(final_normal, 0.0)).xyz);
    vec2 normal_offset = view_final_normal.xy - view_vertex_normal.xy;
    float ref_dist_factor = clamp(uniforms.refraction_settings.y / max(0.1, dist_to_cam), 0.0, 1.0);
    float ref_depth_mask = smoothstep(0.0, max_depth, thickness);
    vec2 refraction_offset = normal_offset * uniforms.refraction_settings.x * ref_dist_factor * ref_depth_mask * 0.05;

    vec2 distorted_uv = screen_uv + refraction_offset;
    float distorted_bg_depth = get_linear_depth(distorted_uv);
    if (distorted_bg_depth < water_linear_depth - 0.001)
    {
        distorted_uv = screen_uv;
        distorted_bg_depth = bg_linear_depth;
    }

    vec3 screen_color = texture(scene_color_texture, distorted_uv).rgb;
    thickness = max(0.0, distorted_bg_depth - water_linear_depth);

    if (thickness > 0.0)
    {
        vec2 caustics_uv1 = in_world_vert.xz * uniforms.caustics_settings_a.x;
        caustics_uv1.x += time_value * uniforms.caustics_settings_a.y;
        vec2 caustics_uv2 = in_world_vert.xz * uniforms.caustics_settings_a.x * 0.7;
        caustics_uv2.y -= time_value * uniforms.caustics_settings_a.y * 0.8;
        float caustics_sample1 = texture(caustics_texture, caustics_uv1).r;
        float caustics_sample2 = texture(caustics_texture, caustics_uv2).r;
        float caustics_value = caustics_sample1 * caustics_sample2;
        float caustics_fade = 1.0 - clamp(thickness * uniforms.caustics_settings_a.w, 0.0, 1.0);
        screen_color += caustics_value * uniforms.caustics_settings_a.z * caustics_fade;
    }

    float fade_range = max(0.001, max_depth - fade_start_depth);
    float depth_ratio = clamp((thickness - fade_start_depth) / fade_range, 0.0, 1.0);
    vec3 underwater_fog_color = uniforms.tint_settings_a.rgb;
    vec3 base_tint_color = uniforms.tint_settings_b.rgb;
    vec3 deep_color = uniforms.tint_settings_b.aaa;
    vec3 water_absorption = uniforms.caustics_settings_b.rgb;

    vec3 transmittance = exp(-thickness * water_absorption);
    vec3 water_volume_color = mix(base_tint_color, deep_color, depth_ratio);
    vec3 apparent_seabed_color = screen_color * water_volume_color;
    vec3 color = mix(underwater_fog_color, apparent_seabed_color, transmittance);

    float depth_foam_factor = smoothstep(uniforms.foam_settings_a.x, uniforms.foam_settings_a.y, thickness);
    float wave_crest_factor = smoothstep(uniforms.foam_settings_b.y, uniforms.foam_settings_b.y - 0.1, final_normal.y);
    wave_crest_factor *= uniforms.foam_settings_b.z;
    float foam_level = clamp(depth_foam_factor + wave_crest_factor, 0.0, 1.0);

    vec2 flow_uv = in_world_vert.xz * 0.5 + time_value * 0.05 * uniforms.foam_settings_a.w;
    vec2 warp = vec2(noise(flow_uv), noise(flow_uv + vec2(5.2, 1.3))) * 0.5;
    float foam_mask_primary = 0.0;
    float foam_mask_shadow = 0.0;
    if (distorted_bg_depth > water_linear_depth)
    {
        vec2 foam_uv = in_world_vert.xz * uniforms.foam_settings_a.z + warp;
        float emergence_noise = noise3d(vec3(in_world_vert.xz * 0.5, time_value * 0.2 * uniforms.foam_settings_a.w));
        emergence_noise = smoothstep(0.0, 1.0, emergence_noise * 0.5 + 0.5);
        float effective_emergence = mix(emergence_noise, 1.0, foam_level);
        float foam_noise_val = fbm_voronoi(foam_uv) * effective_emergence;
        vec2 foam_uv_shadow = (in_world_vert.xz + uniforms.foam_settings_c.xy) * uniforms.foam_settings_a.z + warp;
        float foam_noise_shadow_val = fbm_voronoi(foam_uv_shadow) * effective_emergence;
        float combined_noise = foam_noise_val + foam_level;
        float combined_shadow = foam_noise_shadow_val + foam_level;
        foam_mask_primary = smoothstep(uniforms.foam_settings_b.x + 0.5, uniforms.foam_settings_b.x + 0.6, combined_noise);
        float shadow_shape = smoothstep(uniforms.foam_settings_b.x + 0.5, uniforms.foam_settings_b.x + 0.6, combined_shadow);
        foam_mask_shadow = clamp(shadow_shape - foam_mask_primary, 0.0, 1.0);
    }

    float voronoi_val = 1.0 - sqrt(voronoi(in_world_vert.xz * uniforms.foam_settings_c.z + warp));
    voronoi_val = smoothstep(0.2, 0.8, voronoi_val);
    float bubble_alpha = 1.0 - clamp((1.0 - voronoi_val) * uniforms.foam_settings_c.w, 0.0, 1.0);

    vec3 foam_edge_color = vec3(0.0, 0.0, 0.05);
    vec3 foam_color = vec3(1.0);
    color = mix(color, foam_edge_color, foam_mask_shadow);
    color = mix(color, foam_color, foam_mask_primary * bubble_alpha);

    float roughness = uniforms.pbr_settings.x;
    float metallic = uniforms.pbr_settings.y;
    float specular = uniforms.pbr_settings.z;
    vec3 view_dir = normalize(-in_view_position);
    float fresnel = pow(1.0 - max(dot(view_dir, final_normal), 0.0), 5.0);
    vec3 specular_color = mix(vec3(specular), vec3(1.0), fresnel) * (1.0 - roughness * 0.5);
    color += specular_color * (1.0 - metallic) * 0.15;

    out_color = vec4(color, 1.0);
}
