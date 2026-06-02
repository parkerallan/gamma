#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Volumetric cloud closest-hit shader.
// Based on clouds.txt (maximeheckel / WebGL reference).
// Cloud_params.xyz = world-space center, .w = radius.
// The actual volumetric raymarch is invoked from standard_rt.rmiss (sky rays).
// This shader fires when a ray hits the cloud proxy geometry (if one is placed),
// but the primary path is the miss shader — no proxy mesh required.

struct PrimaryPayload
{
    vec4 color;
    float hit_distance;
    uint depth;
    vec3 pos_ws_curr;
    vec3 pos_ws_prev;
    vec3 shading_normal;
};

layout(location = 0) rayPayloadInEXT PrimaryPayload primary_payload;
hitAttributeEXT vec2 hit_attributes;

layout(set = 0, binding = 0) uniform accelerationStructureEXT top_level_as;
layout(set = 0, binding = 2, std140) uniform SceneUniforms
{
    mat4 view_inverse;
    mat4 projection_inverse;
    vec4 ambient_light;
    vec4 directional_light_color;
    vec4 directional_light_direction;
    vec4 directional_light_data;
    vec4 point_light_color;
    vec4 point_light_position;
    vec4 point_light_data;
    vec4 spot_light_color;
    vec4 spot_light_direction;
    vec4 spot_light_position;
    vec4 spot_light_data;
    vec4 grid_data;
    vec4 grid_origin_extent;
    vec4 skybox_data;
    uvec4 counts;
    uvec4 accumulation_data;
    vec4 animation_time_data;
    mat4 prev_view_projection;
    mat4 current_view_projection;
    vec4 taa_params;
    vec4 jitter_offset;
    vec4 adaptive_params;
    vec4 underwater_data;
    mat4 underwater_world_to_local;
    uvec4 cloud_count;       // .x = number of active clouds
    vec4 cloud_params[8];    // xyz=center, w=radius per cloud
} scene_uniforms;

// ---- Volumetric cloud helpers (clouds.txt port) ----

#define CLOUD_MAX_STEPS      120
#define CLOUD_MAX_LIGHT      6
#define CLOUD_ABSORPTION     0.9
#define CLOUD_ANISO          0.3
#define CLOUD_PI             3.14159265359

float cloud_hash(float n)
{
    return fract(sin(n) * 753.5453123);
}

float cloud_noise(vec3 x)
{
    vec3 p = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float n = p.x + p.y * 157.0 + 113.0 * p.z;
    return mix(
        mix(mix(cloud_hash(n + 0.0),   cloud_hash(n + 1.0),   f.x),
            mix(cloud_hash(n + 157.0), cloud_hash(n + 158.0), f.x), f.y),
        mix(mix(cloud_hash(n + 113.0), cloud_hash(n + 114.0), f.x),
            mix(cloud_hash(n + 270.0), cloud_hash(n + 271.0), f.x), f.y),
        f.z) * 2.0 - 1.0;
}

float cloud_fbm(vec3 p, bool lowRes)
{
    float t = scene_uniforms.animation_time_data.x;
    vec3  q = p + t * 0.02 * vec3(1.0, -0.2, -1.0);
    float f = 0.0;
    float scale = 0.8;
    float factor = 2.02;
    for (int i = 0; i < 6; i++)
    {
        if (lowRes && i >= 3) break;
        f += scale * cloud_noise(q);
        q *= factor;
        factor += 0.21;
        scale  *= 0.5;
    }
    return f;
}

float cloud_density(vec3 p_world, vec3 center, float radius, bool lowRes)
{
    vec3  p = (p_world - center) / radius;
    float r = length(p);
    if (r > 2.8) return 0.0;
    vec3 seed = fract(center * 0.137) * 10.0;
    return -(r - 1.2) + cloud_fbm(p + seed, lowRes);
}

float cloud_lightmarch(vec3 pos, vec3 sunDir, vec3 center, float radius)
{
    float total = 0.0;
    float ms    = 0.03 * radius;
    for (int i = 0; i < CLOUD_MAX_LIGHT; i++)
    {
        pos   += sunDir * ms * float(i);
        total += cloud_density(pos, center, radius, true);
    }
    return exp(-total * CLOUD_ABSORPTION);
}

bool cloud_hit_sphere(vec3 ro, vec3 rd, vec3 c, float r, out float t0, out float t1)
{
    vec3  oc   = ro - c;
    float b    = dot(oc, rd);
    float disc = b * b - dot(oc, oc) + r * r;
    if (disc < 0.0) return false;
    float sq = sqrt(disc);
    t0 = -b - sq;
    t1 = -b + sq;
    return t1 > 0.0;
}

float cloud_HG(float g, float mu)
{
    float gg = g * g;
    return (1.0 / (4.0 * CLOUD_PI)) * ((1.0 - gg) / pow(1.0 + gg - 2.0 * g * mu, 1.5));
}

// Composites volumetric cloud energy into `color`.
void cloud_raymarch(vec3 ro, vec3 rd, vec3 sunDir, vec3 center, float radius, inout vec3 color)
{
    float t0, t1;
    if (!cloud_hit_sphere(ro, rd, center, radius * 2.8, t0, t1) || t1 < 0.0) return;
    t0 = max(t0, 0.0);

    float ms    = 0.08 * radius;
    float phase = cloud_HG(CLOUD_ANISO, dot(rd, sunDir));
    float T     = 1.0;
    vec3  Lc    = vec3(0.0);
    float depth = t0;
    vec3  p     = ro + depth * rd;

    for (int i = 0; i < CLOUD_MAX_STEPS; i++)
    {
        if (depth > t1 || T < 0.01) break;
        float d = cloud_density(p, center, radius, false);
        if (d > 0.0)
        {
            float dT = exp(-d * 0.5 * ms);
            float lt = cloud_lightmarch(p, sunDir, center, radius);
            vec3 Ls = vec3(0.95, 0.95, 1.0) * lt * (1.0 + phase)
                    + vec3(0.35, 0.40, 0.50);
            Lc += T * (1.0 - dT) * Ls;
            T  *= dT;
        }
        depth += ms;
        p      = ro + depth * rd;
    }
    color = color * T + Lc;
}

void main()
{
    // Reconstruct sky color at this hit point (same logic as miss shader).
    vec3 rd = normalize(gl_WorldRayDirectionEXT);

    if (scene_uniforms.cloud_count.x > 0u)
    {
        vec3 sky = scene_uniforms.ambient_light.rgb;
        vec3 sunDir = normalize(-scene_uniforms.directional_light_direction.xyz);
        for (int ci = 0; ci < int(scene_uniforms.cloud_count.x); ci++)
        {
            cloud_raymarch(
                gl_WorldRayOriginEXT,
                rd,
                sunDir,
                scene_uniforms.cloud_params[ci].xyz,
                scene_uniforms.cloud_params[ci].w,
                sky);
        }
        primary_payload.color        = vec4(sky, 1.0);
        primary_payload.hit_distance = gl_HitTEXT;
        primary_payload.pos_ws_curr  = gl_WorldRayOriginEXT + rd * gl_HitTEXT;
        primary_payload.pos_ws_prev  = primary_payload.pos_ws_curr;
    }
    else
    {
        primary_payload.color        = vec4(0.0, 0.0, 0.0, 0.0);
        primary_payload.hit_distance = gl_HitTEXT;
        primary_payload.pos_ws_curr  = gl_WorldRayOriginEXT + rd * gl_HitTEXT;
        primary_payload.pos_ws_prev  = primary_payload.pos_ws_curr;
    }
}
