#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;
layout(location = 4) in vec4 in_tangent;

layout(location = 0) out vec3 out_world_vert;
layout(location = 1) out float out_wave_height;
layout(location = 2) out vec3 out_vertex_normal_world;
layout(location = 3) out vec3 out_view_position;
layout(location = 4) out vec2 out_uv;

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

layout(push_constant) uniform WaterSurfaceObject
{
    mat4 model;
} object_data;

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

float map(vec3 p, float time_value)
{
    float sea_height = uniforms.wave_settings_a.x;
    float sea_choppy = uniforms.wave_settings_a.y;
    float sea_speed = uniforms.wave_settings_a.z;
    float sea_freq = uniforms.wave_settings_a.w;

    float freq = sea_freq;
    float amp = sea_height;
    float choppy = sea_choppy;
    vec2 uv = p.xz;
    uv.x *= 0.75;
    float h = 0.0;
    for (int i = 0; i < ITER_GEOMETRY; ++i)
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

void main()
{
    vec3 world_pos = (object_data.model * vec4(in_position, 1.0)).xyz;
    float time_value = uniforms.viewport_time.z;
    out_wave_height = -map(world_pos, time_value);

    vec3 displaced_local = in_position;
    displaced_local.y = out_wave_height;
    out_world_vert = (object_data.model * vec4(displaced_local, 1.0)).xyz;

    float vertex_eps = 0.1;
    float h_center = out_wave_height;
    float h_x = -map(world_pos + vec3(vertex_eps, 0.0, 0.0), time_value);
    float h_z = -map(world_pos + vec3(0.0, 0.0, vertex_eps), time_value);
    out_vertex_normal_world = normalize(vec3(h_center - h_x, vertex_eps, h_center - h_z));

    vec4 view_position = uniforms.view * vec4(out_world_vert, 1.0);
    out_view_position = view_position.xyz;
    out_uv = in_uv;
    gl_Position = uniforms.projection * view_position;
}
