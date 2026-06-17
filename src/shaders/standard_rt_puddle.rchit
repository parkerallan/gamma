#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

// Raindrops-on-puddle closest-hit shader (the "Image pass" of the 4rknova
// article, src/shaders/rainfall.txt).
//
// The wave-equation ripple field is simulated separately in rain_wave.comp and
// delivered here as an RG height texture (binding 12). This shader is the
// water surface: it samples that field for a height-gradient normal, then
// shades the water as the article describes — Fresnel-Schlick at the water IOR
// blending a reflection against a refraction. Because we are in a path tracer
// the refracted ray is traced into the REAL scene below (not a flat plane);
// Beer-Lambert turbidity tints it toward the water colour with depth.
//
// The mesh's interpolated normal + UV are fetched from the vertex buffers (like
// standard_rt.rchit) so the surface works for ANY plane orientation — the wave
// field is mapped by the mesh UV, and a procedural shape mask turns a square
// proxy into an organic puddle outline (outside the mask the proxy is invisible).
//
// Runs when a ray hits a mesh whose material shader type is Puddle
// (instance shader_type == 5, SBT hit-group offset 8).

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

// ---- Geometry buffers (same layout as standard_rt.rchit) ------------------
struct SceneVertex { vec3 position; vec3 normal; vec2 uv; vec4 color; vec4 tangent; };
layout(buffer_reference, scalar) readonly buffer VertexBuffer { SceneVertex vertices[]; };
layout(buffer_reference, scalar) readonly buffer IndexBuffer  { uint indices[]; };
layout(buffer_reference, scalar) readonly buffer PrevPositionBuffer { vec3 positions[]; };
struct MeshRecord
{
    VertexBuffer vertex_buffer;
    IndexBuffer index_buffer;
    PrevPositionBuffer prev_position_buffer;
    uint vertex_count;
    uint vertex_stride;
    uint index_count;
    uint section_offset;
    uint section_count;
    uint material_offset;
};
layout(set = 0, binding = 3, scalar) readonly buffer MeshRecordBuffer { MeshRecord meshes[]; };

// Wave height field from rain_wave.comp (R = current height, G = previous),
// neutral 0.5. Mapped across the mesh UV.
layout(set = 0, binding = 12) uniform sampler2D wave_height;

// ---- Tunables -------------------------------------------------------------
// Surface-normal slope amplification from the height-field gradient.
#define PUDDLE_NORMAL_AMP   18.0
#define WATER_IOR           1.333
#define PUDDLE_REFLECTIVITY 0.8
#define PUDDLE_TURBIDITY    0.05
#define PUDDLE_TINT         vec3(0.06, 0.10, 0.13)
#define PUDDLE_MAX_DEPTH    8u
// Puddle shape mask: 1 = fill the whole proxy (rectangular pool), <1 wobbles
// and rounds the outline so a square plane reads as an organic puddle.
#define PUDDLE_SHAPE        1
#define PUDDLE_EDGE_SOFT    0.18

vec3 world_normal(vec3 nl)
{
    return normalize(transpose(mat3(gl_WorldToObjectEXT)) * nl);
}

vec3 trace_segment(vec3 origin, vec3 dir, uint next_depth)
{
    primary_payload.color = vec4(0.0, 0.0, 0.0, 1.0);
    primary_payload.hit_distance = 1e30;
    primary_payload.depth = next_depth;
    traceRayEXT(top_level_as, gl_RayFlagsNoneEXT, 0xFF, 0, 1, 0,
                origin, 1.0e-3, dir, 10000.0, 0);
    return primary_payload.color.rgb;
}

float puddle_hash11(float p) { return fract(sin(p * 127.1) * 43758.5453); }

// Soft organic puddle outline from the mesh UV, RANDOMIZED per puddle by `seed`
// (derived from the object's world position) so no two puddles share an outline.
// Returns ~1 inside, 0 outside.
float puddle_mask(vec2 uv, float seed)
{
#if PUDDLE_SHAPE
    vec2 q = uv * 2.0 - 1.0;             // [-1,1]
    // Per-puddle rotation.
    float rot = seed * 6.2831853;
    float cs = cos(rot), sn = sin(rot);
    q = mat2(cs, -sn, sn, cs) * q;

    float d = length(q);
    float a = atan(q.y, q.x);

    // Per-puddle randomized harmonic frequencies, phases and amplitudes.
    float p1 = seed * 6.283;
    float p2 = puddle_hash11(seed + 1.0) * 6.283;
    float p3 = puddle_hash11(seed + 2.0) * 6.283;
    float f1 = 3.0  + floor(puddle_hash11(seed + 3.0) * 3.0);   // 3..5
    float f2 = 6.0  + floor(puddle_hash11(seed + 4.0) * 4.0);   // 6..9
    float f3 = 11.0 + floor(puddle_hash11(seed + 5.0) * 6.0);   // 11..16
    float wob = 0.10  * sin(a * f1 + p1)
              + 0.05  * sin(a * f2 + p2)
              + 0.035 * sin(a * f3 + p3);
    float radius = 0.90 + 0.06 * (puddle_hash11(seed + 6.0) - 0.5) * 2.0 + wob;
    return smoothstep(radius, radius - PUDDLE_EDGE_SOFT, d);
#else
    return 1.0;
#endif
}

void main()
{
    vec3 hit_ws = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    vec3 rd = normalize(gl_WorldRayDirectionEXT);
    const uint current_depth = primary_payload.depth;

    // ---- Fetch the hit triangle (interpolated normal + UV + tangent) -------
    MeshRecord mesh = meshes[gl_InstanceCustomIndexEXT];
    uint pfi = gl_PrimitiveID * 3u;
    uint i0 = mesh.index_buffer.indices[pfi + 0u];
    uint i1 = mesh.index_buffer.indices[pfi + 1u];
    uint i2 = mesh.index_buffer.indices[pfi + 2u];
    SceneVertex v0 = mesh.vertex_buffer.vertices[i0];
    SceneVertex v1 = mesh.vertex_buffer.vertices[i1];
    SceneVertex v2 = mesh.vertex_buffer.vertices[i2];
    vec3 bary = vec3(1.0 - hit_attributes.x - hit_attributes.y, hit_attributes.x, hit_attributes.y);
    vec3 onormal = normalize(v0.normal * bary.x + v1.normal * bary.y + v2.normal * bary.z);
    vec2 uv = v0.uv * bary.x + v1.uv * bary.y + v2.uv * bary.z;
    vec4 otan = v0.tangent * bary.x + v1.tangent * bary.y + v2.tangent * bary.z;

    // ---- Puddle outline (randomized per puddle by world position) ----------
    vec3 puddle_origin = (gl_ObjectToWorldEXT * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    float shape_seed = fract(sin(dot(puddle_origin.xz, vec2(12.9898, 78.233))
                                 + puddle_origin.y * 37.0) * 43758.5453);
    float mask = puddle_mask(uv, shape_seed);

    // The straight-through background (what the proxy would NOT have covered).
    // Used both outside the puddle and at the soft edge.
    vec3 background = vec3(0.0);
    bool traced_bg = false;
    if (current_depth < PUDDLE_MAX_DEPTH && mask < 0.999)
    {
        background = trace_segment(hit_ws, rd, current_depth + 1u);
        traced_bg = true;
    }

    vec3 water = PUDDLE_TINT;
    if (mask > 0.001 && current_depth < PUDDLE_MAX_DEPTH)
    {
        // World surface frame from the real mesh normal, faced toward the viewer.
        vec3 Ng = world_normal(onormal);
        if (dot(Ng, rd) > 0.0) Ng = -Ng;
        vec3 tan_u = mat3(gl_ObjectToWorldEXT) * otan.xyz;
        if (dot(tan_u, tan_u) < 1e-8)                  // mesh has no tangents
            tan_u = (abs(Ng.y) < 0.99) ? cross(vec3(0.0, 1.0, 0.0), Ng) : vec3(1.0, 0.0, 0.0);
        tan_u = normalize(tan_u - Ng * dot(Ng, tan_u));
        vec3 tan_v = normalize(cross(Ng, tan_u)) * (otan.w >= 0.0 ? 1.0 : -1.0);

        // Height-field gradient -> ripple normal (article scene_normal).
        vec2 texel = 1.0 / vec2(textureSize(wave_height, 0));
        float hL = texture(wave_height, uv - vec2(texel.x, 0.0)).x;
        float hR = texture(wave_height, uv + vec2(texel.x, 0.0)).x;
        float hD = texture(wave_height, uv - vec2(0.0, texel.y)).x;
        float hU = texture(wave_height, uv + vec2(0.0, texel.y)).x;
        vec3 N = normalize(Ng - ((hR - hL) * tan_u + (hU - hD) * tan_v) * PUDDLE_NORMAL_AMP);

        // Fresnel-Schlick at the water IOR.
        float F0 = (1.0 - WATER_IOR) / (1.0 + WATER_IOR);
        F0 *= F0;
        float cos_i = clamp(dot(N, -rd), 0.0, 1.0);
        float fresnel = F0 + (1.0 - F0) * pow(1.0 - cos_i, 5.0);

        // Reflection (sky / scene).
        vec3 reflected = trace_segment(hit_ws, reflect(rd, N), current_depth + 1u);

        // Refraction into the real scene below + Beer-Lambert turbidity.
        vec3 rd_in = refract(rd, N, 1.0 / WATER_IOR);
        if (dot(rd_in, rd_in) < 1e-6) rd_in = reflect(rd, N);
        vec3 ground = trace_segment(hit_ws, rd_in, current_depth + 1u);
        float ground_dist = primary_payload.hit_distance;
        float visibility = (ground_dist >= 1e29) ? 0.0 : exp(-ground_dist * PUDDLE_TURBIDITY);
        vec3 refracted = mix(PUDDLE_TINT, ground, visibility);

        float reflMix = mix(0.3, 0.8, PUDDLE_REFLECTIVITY);
        water = mix(refracted, reflected, fresnel * (reflMix - 0.3) + 0.3);
        water += PUDDLE_TINT * 0.15 * (1.0 - fresnel);
    }

    vec3 col = traced_bg ? mix(background, water, mask) : water;
    primary_payload.color = vec4(col, 1.0);

    // TAA: record the (stable) puddle surface point, not the animated reflection.
    primary_payload.hit_distance   = gl_HitTEXT;
    primary_payload.pos_ws_curr    = hit_ws;
    primary_payload.pos_ws_prev    = hit_ws;
    primary_payload.shading_normal = world_normal(onormal);
    primary_payload.material_flags = 0u;
    primary_payload.depth = current_depth;
}
