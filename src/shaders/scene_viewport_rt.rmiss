#version 460
#extension GL_EXT_ray_tracing : require

struct PrimaryPayload
{
    vec4 color;
    float hit_distance;
    uint depth;
};

layout(set = 0, binding = 2, std140) uniform SceneUniforms
{
    mat4 view_inverse;
    mat4 projection_inverse;
    vec4 ambient_light;
    vec4 directional_light_color;
    vec4 directional_light_direction;
    vec4 spot_light_color;
    vec4 spot_light_direction;
    vec4 spot_light_position;
    vec4 spot_light_data;
    vec4 grid_data;
    vec4 grid_origin_extent;
    uvec4 counts;
} scene_uniforms;

layout(location = 0) rayPayloadInEXT PrimaryPayload primary_payload;

vec3 evaluate_sky(vec3 ray_direction)
{
    vec3 ambient = scene_uniforms.ambient_light.rgb * max(scene_uniforms.ambient_light.a, 0.35);
    vec3 horizon = max(vec3(0.10, 0.11, 0.13), ambient * 0.7 + vec3(0.05, 0.05, 0.06));
    vec3 zenith = max(vec3(0.18, 0.22, 0.30), ambient * 1.3 + vec3(0.08, 0.10, 0.14));
    float t = clamp(ray_direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 color = mix(horizon, zenith, smoothstep(0.0, 1.0, t));

    if (scene_uniforms.directional_light_color.a > 0.0)
    {
        vec3 sun_direction = normalize(-scene_uniforms.directional_light_direction.xyz);
        float sun = pow(max(dot(ray_direction, sun_direction), 0.0), 256.0);
        color += scene_uniforms.directional_light_color.rgb * scene_uniforms.directional_light_color.a * sun;
    }

    return color;
}

vec3 evaluate_grid(vec3 world_position)
{
    if (scene_uniforms.grid_data.x < 0.5)
    {
        return vec3(0.08, 0.09, 0.11);
    }

    float spacing = max(scene_uniforms.grid_data.y, 0.001);
    float extent = scene_uniforms.grid_origin_extent.w;
    vec2 grid_origin = scene_uniforms.grid_origin_extent.xz;
    vec2 local = world_position.xz - grid_origin;
    if (abs(local.x) > extent || abs(local.y) > extent)
    {
        return vec3(0.08, 0.09, 0.11);
    }

    float minor_spacing = max(0.0625, spacing * 0.25);
    float minor_width = max(0.01, minor_spacing * 0.045);
    float major_width = max(minor_width * 1.9, spacing * 0.016);

    vec2 minor_mod = abs(mod(local + minor_spacing * 0.5, minor_spacing) - minor_spacing * 0.5);
    float minor_line = (minor_mod.x <= minor_width || minor_mod.y <= minor_width) ? 1.0 : 0.0;

    vec2 major_mod = abs(mod(local + spacing * 0.5, spacing) - spacing * 0.5);
    float major_line = (major_mod.x <= major_width || major_mod.y <= major_width) ? 1.0 : 0.0;

    vec3 base = vec3(0.08, 0.09, 0.11);
    vec3 minor_color = vec3(0.16);
    vec3 major_color = vec3(0.32);
    vec3 color = mix(base, minor_color, minor_line * 0.65);
    color = mix(color, major_color, max(major_line, minor_line * 0.25));
    return color;
}

void main()
{
    vec3 color = evaluate_sky(normalize(gl_WorldRayDirectionEXT));
    if (abs(gl_WorldRayDirectionEXT.y) > 0.0001)
    {
        float plane_t = -gl_WorldRayOriginEXT.y / gl_WorldRayDirectionEXT.y;
        if (plane_t > 0.0)
        {
            vec3 plane_position = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * plane_t;
            color = evaluate_grid(plane_position);
        }
    }

    primary_payload.color = vec4(color, 1.0);
    primary_payload.hit_distance = 1e30;
    primary_payload.depth = primary_payload.depth;
}
