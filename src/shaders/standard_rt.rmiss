#version 460
#extension GL_EXT_ray_tracing : require

const float kPi = 3.1415926535;
const float kTwoPi = 6.2831853070;

struct PrimaryPayload
{
    vec4 color;
    float hit_distance;
    uint depth;
    // See standard_rt.rgen for the full contract. On a sky miss this stays
    // at vec3(0) (rgen uses direction reprojection w=0 and ignores the
    // field); on a grid hit we set it to the world plane intersection so
    // the grid reprojects as a static surface.
    vec3 prev_world_pos;
};

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
    mat4 view_proj_curr;
    mat4 view_proj_prev;
    vec4 jitter_state;
} scene_uniforms;

layout(set = 0, binding = 8) uniform sampler2D skybox_texture;

layout(location = 0) rayPayloadInEXT PrimaryPayload primary_payload;

vec3 evaluate_sky(vec3 ray_direction)
{
    if (scene_uniforms.skybox_data.x > 0.5)
    {
        vec3 direction = normalize(ray_direction);
        float u = atan(direction.z, direction.x) / kTwoPi + 0.5 + scene_uniforms.skybox_data.y / kTwoPi;
        float v = acos(clamp(direction.y, -1.0, 1.0)) / kPi;
        return texture(skybox_texture, vec2(fract(u), clamp(v, 0.0, 1.0))).rgb;
    }

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
    // Default hit distance for sky pixels.
    float final_hit_distance = 1e30;
    if (abs(gl_WorldRayDirectionEXT.y) > 0.0001)
    {
        float plane_t = -gl_WorldRayOriginEXT.y / gl_WorldRayDirectionEXT.y;
        if (plane_t > 0.0 && scene_uniforms.grid_data.x >= 0.5)
        {
            vec3 plane_position = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * plane_t;
            float extent = scene_uniforms.grid_origin_extent.w;
            vec2 grid_origin = scene_uniforms.grid_origin_extent.xz;
            vec2 local = plane_position.xz - grid_origin;
            // Only treat this miss as a grid hit (with finite hit_distance
            // so the TAA reprojection uses world-point reprojection rather
            // than direction-only sky reprojection) when we're inside the
            // grid extent. Outside the grid we fall back to sky shading.
            if (abs(local.x) <= extent && abs(local.y) <= extent)
            {
                color = evaluate_grid(plane_position);
                final_hit_distance = plane_t;
            }
        }
        else if (plane_t > 0.0)
        {
            // Grid disabled but the legacy code still shaded the floor at
            // y=0; preserve that visual but keep hit_distance as sky so we
            // don't fight the sky reprojection path.
            vec3 plane_position = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * plane_t;
            color = evaluate_grid(plane_position);
        }
    }

    primary_payload.color = vec4(color, 1.0);
    primary_payload.hit_distance = final_hit_distance;
    primary_payload.depth = primary_payload.depth;
    // Default: sky -- rgen branches on hit_distance and uses direction
    // reprojection (w=0) for the sky case, so prev_world_pos is unused.
    // For a grid hit we store the plane intersection so the grid is
    // treated as a static world-space surface in the MV reprojection.
    if (final_hit_distance < 1e29)
    {
        primary_payload.prev_world_pos = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * final_hit_distance;
    }
    else
    {
        primary_payload.prev_world_pos = vec3(0.0);
    }
}
