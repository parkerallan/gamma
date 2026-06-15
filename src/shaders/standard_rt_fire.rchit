#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Volumetric fire closest-hit shader.
//
// Flame field + colour ported from Dave Hoskins' "Campfire" Shadertoy
// (https://www.shadertoy.com/view/MdffWj, Nov. 2013) — only the flame is used;
// the logs, stones, ground, camera and texture lookups are dropped. The original
// texture-based Noise() is replaced with a procedural 3D value noise so no sampler
// is needed. The flame is raymarched in the proxy mesh's OBJECT space, bounded by
// the unit cube [-0.5, 0.5]; scaling/rotating the proxy object scales the volume.
//
// This shader runs when a primary (or secondary) ray hits a mesh whose material
// shader type is Fire (instance shader_type == 3, SBT hit-group offset 4).

struct PrimaryPayload
{
    vec4 color;
    float hit_distance;
    uint depth;
    vec3 pos_ws_curr;
    vec3 pos_ws_prev;
    vec3 shading_normal;
    uint material_flags;
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
    uvec4 cloud_count;
    vec4 cloud_params[8];
} scene_uniforms;

// ---- Tunables -------------------------------------------------------------
#define FIRE_STEPS       96      // raymarch samples across the proxy
#define FIRE_BOX_HALF    1.0     // proxy mesh local half-extent (cube.glb is [-1,1])
// The flame fills the cube (so scaling the cube resizes the fire). FILL_SCALE is
// the flame's reference extent mapped across the box: large enough that the whole
// organic flame (with its taper) fits inside with margin, so the cube faces are
// never reached -> no cube-shaped cutoff. Isotropic, so uniform scaling keeps the
// flame's proportions.
#define FIRE_FILL_SCALE  7.0
#define FIRE_RISE_SPEED  4.0     // upward scroll speed (matches Hoskins' iTime*4.0)
#define FIRE_INTENSITY   1.0     // overall brightness/coverage multiplier
#define FIRE_EMISSION    1.0     // emissive strength added over the background
// Soft safety only: trims a stray tongue that reaches a top/side face so there is
// no hard pixel edge. With FILL_SCALE leaving margin it rarely triggers and does
// not sculpt the flame.
#define FIRE_EDGE_SAFETY 0.92

// ---- Procedural 3D value noise (replaces the texture-based Noise) ----------
float fire_hash(vec3 p)
{
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float fire_value_noise(vec3 x)
{
    vec3 p = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(mix(fire_hash(p + vec3(0, 0, 0)), fire_hash(p + vec3(1, 0, 0)), f.x),
            mix(fire_hash(p + vec3(0, 1, 0)), fire_hash(p + vec3(1, 1, 0)), f.x), f.y),
        mix(mix(fire_hash(p + vec3(0, 0, 1)), fire_hash(p + vec3(1, 0, 1)), f.x),
            mix(fire_hash(p + vec3(0, 1, 1)), fire_hash(p + vec3(1, 1, 1)), f.x), f.y),
        f.z);
}

// Hoskins' Noise(vec3) with the upward time advection baked in (x.y -= iTime*4.0).
float fire_noise(vec3 x, float t)
{
    x.y -= t * FIRE_RISE_SPEED;
    return fire_value_noise(x);
}

// Hoskins' DE_Fire — verbatim flame density field (single coordinate). Returns
// small/negative inside the flame; 1.0 - DE_Fire gives the per-sample density.
float fire_de(vec3 p, float t)
{
    p.xz += (fire_noise(p * 0.8, t)) * p.y * 0.3;
    vec3 shape = p * vec3(1.5, 0.35, 1.5);
    if (dot(shape, shape) > 70.0) return 1.0;

    p += 2.5 * (fire_noise(shape * 1.5, t)
              - fire_noise(-shape * 0.945, t) * 0.5
              + fire_noise(shape * 9.6, t) * 0.3);
    float f = (length(shape) - (1.0 + fire_noise(p, t) * 10.0));

    f -= max(3.4 - p.y, 0.0) * 3.0;
    f -= pow(abs(fire_noise(shape * 3.9, t)), 45.0) * 300.0
       * pow(abs(fire_noise(shape * 1.1, t)), 5.0);
    return f;
}

// Hoskins' FlameColour — black-body-ish ramp dark red -> orange -> yellow -> white.
vec3 fire_colour(float f)
{
    f = f * f * (3.0 - 2.0 * f);
    return min(vec3(f + 0.8, f * f * 1.4 + 0.1, f * f * f * 0.7) * f, 1.0);
}

// Slab method: intersect ray (ro,rd) with the local cube [-HALF, HALF].
bool fire_hit_box(vec3 ro, vec3 rd, out float t0, out float t1)
{
    vec3 inv = 1.0 / rd;
    vec3 a = (vec3(-FIRE_BOX_HALF) - ro) * inv;
    vec3 b = (vec3( FIRE_BOX_HALF) - ro) * inv;
    vec3 tmin = min(a, b);
    vec3 tmax = max(a, b);
    t0 = max(max(tmin.x, tmin.y), tmin.z);
    t1 = min(min(tmax.x, tmax.y), tmax.z);
    return t1 >= max(t0, 0.0);
}

void main()
{
    float t = scene_uniforms.animation_time_data.x;

    // Ray into proxy object space; march in local units along the normalised dir.
    vec3 ro_l = (gl_WorldToObjectEXT * vec4(gl_WorldRayOriginEXT, 1.0)).xyz;
    vec3 rd_l = normalize(mat3(gl_WorldToObjectEXT) * gl_WorldRayDirectionEXT);

    float t0, t1;
    bool inside = fire_hit_box(ro_l, rd_l, t0, t1);
    t0 = max(t0, 0.0);

    // ---- Background behind the proxy (continuation primary ray) ----
    // Fire is emissive + transparent, so composite over whatever is behind it.
    vec3 background = vec3(0.0);
    const uint current_depth = primary_payload.depth;
    bool traced_background = false;
    if (current_depth < 8u)
    {
        primary_payload.color = vec4(0.0, 0.0, 0.0, 1.0);
        primary_payload.hit_distance = 1e30;
        primary_payload.depth = current_depth + 1u;
        float continuation_tmin = gl_HitTEXT + max(1e-5, gl_HitTEXT * 1e-5);
        traceRayEXT(
            top_level_as,
            gl_RayFlagsNoneEXT,
            0xFF,
            0,
            1,
            0,
            gl_WorldRayOriginEXT,
            continuation_tmin,
            gl_WorldRayDirectionEXT,
            10000.0,
            0);
        background = primary_payload.color.rgb;
        traced_background = true;
        // primary_payload now carries the background hit's pos/normal/distance,
        // which is what we want TAA/denoise to track through the transparent fire.
    }

    // ---- Raymarch the flame across the proxy ----
    float sum = 0.0;
    if (inside)
    {
        float seg = (t1 - t0) / float(FIRE_STEPS);
        // Normalise per-step length to Hoskins' ~0.1 reference step so the
        // 0.00187 accumulation constant carries over regardless of proxy size.
        float per_step = 0.00187 * (seg * FIRE_FILL_SCALE / 0.1) * FIRE_INTENSITY;
        for (int i = 0; i < FIRE_STEPS; i++)
        {
            float tt = t0 + (float(i) + 0.5) * seg;
            vec3 u = (ro_l + tt * rd_l) / FIRE_BOX_HALF;    // normalised local, [-1,1]

            // Faithful single-coordinate field. Isotropic fill (base at the bottom
            // face), zoomed out so the whole flame fits inside the box with margin.
            vec3 ref = vec3(u.x * FIRE_FILL_SCALE,
                            (u.y + 1.0) * FIRE_FILL_SCALE,
                            u.z * FIRE_FILL_SCALE);
            float v = max(1.0 - fire_de(ref, t), 0.0);

            // Soft safety only (top + sides), so a stray tongue never shows a hard
            // pixel at a face. Does not shape the flame when the box contains it.
            vec3 au = abs(u);
            float side = (1.0 - smoothstep(FIRE_EDGE_SAFETY, 1.0, au.x))
                       * (1.0 - smoothstep(FIRE_EDGE_SAFETY, 1.0, au.z));
            float top  = 1.0 - smoothstep(FIRE_EDGE_SAFETY, 1.0, u.y);
            v *= side * clamp(top, 0.0, 1.0);

            sum += v * per_step;
        }
    }

    float flame = clamp(sum * sum * sum, 0.0, 1.0);
    vec3 col = fire_colour(flame) * FIRE_EMISSION;

    // Composite: emissive flame additively over the background.
    primary_payload.color = vec4(background + col, 1.0);
    if (!traced_background)
    {
        primary_payload.hit_distance = gl_HitTEXT;
        primary_payload.pos_ws_curr  = gl_WorldRayOriginEXT + normalize(gl_WorldRayDirectionEXT) * gl_HitTEXT;
        primary_payload.pos_ws_prev  = primary_payload.pos_ws_curr;
        primary_payload.shading_normal = -normalize(gl_WorldRayDirectionEXT);
        primary_payload.material_flags = 0u;
    }
    primary_payload.depth = current_depth;
}
