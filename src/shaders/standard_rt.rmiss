#version 460
#extension GL_EXT_ray_tracing : require

const float kPi = 3.1415926535;
const float kTwoPi = 6.2831853070;

struct PrimaryPayload
{
    vec4 color;
    float hit_distance;
    uint depth;
    vec3 pos_ws_curr;
    vec3 pos_ws_prev;
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
    mat4 prev_view_projection;
    mat4 current_view_projection;
    vec4 taa_params;
    vec4 jitter_offset;
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

vec3 evaluate_grid(vec3 world_position, float pixel_footprint)
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

    // Distance from this point to the nearest grid line, in world units,
    // measured along x and z independently. Take the smaller — that's
    // how far we are from the closest line of either family.
    vec2 minor_mod = abs(mod(local + minor_spacing * 0.5, minor_spacing) - minor_spacing * 0.5);
    float minor_dist = min(minor_mod.x, minor_mod.y);
    vec2 major_mod = abs(mod(local + spacing * 0.5, spacing) - spacing * 0.5);
    float major_dist = min(major_mod.x, major_mod.y);

    // Analytic anti-aliasing. `pixel_footprint` is the world-space size
    // of one screen pixel at this hit point — used as the smoothstep
    // softening width. Without this the hard `dist <= width` test
    // produces shimmer under TAA sub-pixel jitter because each frame
    // the line either fully covers or fully misses each pixel.
    float fp = max(pixel_footprint, 1e-5);
    float minor_line = 1.0 - smoothstep(minor_width, minor_width + fp, minor_dist);
    float major_line = 1.0 - smoothstep(major_width, major_width + fp, major_dist);

    vec3 base = vec3(0.08, 0.09, 0.11);
    vec3 minor_color = vec3(0.16);
    vec3 major_color = vec3(0.32);
    vec3 color = mix(base, minor_color, minor_line * 0.65);
    color = mix(color, major_color, max(major_line, minor_line * 0.25));
    return color;
}

// Reconstruct the *unjittered* primary ray direction for an arbitrary
// sub-pixel sample location (in launch-pixel coordinates). Mirrors the
// rgen ray-construction math but ignores scene_uniforms.jitter_offset.
vec3 unjittered_ray_direction(vec2 pixel_center)
{
    vec2 launch_size = vec2(gl_LaunchSizeEXT.xy);
    vec2 ndc = (pixel_center / launch_size) * 2.0 - 1.0;
    vec4 clip_target = vec4(ndc.x, ndc.y, 1.0, 1.0);
    vec4 view_target = scene_uniforms.projection_inverse * clip_target;
    vec3 view_direction = normalize(view_target.xyz / view_target.w);
    return normalize((scene_uniforms.view_inverse * vec4(view_direction, 0.0)).xyz);
}

void main()
{
    vec3 color = evaluate_sky(normalize(gl_WorldRayDirectionEXT));

    vec3 origin = gl_WorldRayOriginEXT;
    vec2 px_c   = vec2(gl_LaunchIDEXT.xy) + vec2(0.5);
    vec3 dir    = unjittered_ray_direction(px_c);
    bool grid_hit = false;
    float grid_hit_t = 0.0;
    vec3 grid_hit_pos = vec3(0.0);

    // Editor-only ground grid. Only draw it when explicitly enabled,
    // and only INSIDE the configured grid extent — otherwise the
    // dark fallback in evaluate_grid() leaks out as a viewport-edge
    // "border" that also shadows the player through secondary bounces.
    //
    // Use an UNJITTERED ray for the grid intersection so the grid is
    // independent of the TAA jitter (cleaner accumulation, no shimmer).
    if (scene_uniforms.grid_data.x >= 0.5)
    {
        if (abs(dir.y) > 0.0001)
        {
            float plane_t = -origin.y / dir.y;
            if (plane_t > 0.0)
            {
                vec3 plane_position = origin + dir * plane_t;
                vec2 local  = plane_position.xz - scene_uniforms.grid_origin_extent.xz;
                float extent = scene_uniforms.grid_origin_extent.w;
                if (abs(local.x) <= extent && abs(local.y) <= extent)
                {
                    // World-space pixel footprint at this hit point —
                    // measured directly by tracing the next pixel over
                    // and comparing world-space intersection points.
                    // This is robust to whatever projection conventions
                    // (perspective, reverse-Z, infinite-far) the engine
                    // uses, unlike algebraic FoV extraction from
                    // projection_inverse.
                    float fp = 0.0;
                    vec3 dir_x = unjittered_ray_direction(px_c + vec2(1.0, 0.0));
                    if (abs(dir_x.y) > 0.0001)
                    {
                        float tx = -origin.y / dir_x.y;
                        if (tx > 0.0)
                        {
                            vec3 px = origin + dir_x * tx;
                            fp = max(fp, length(px.xz - plane_position.xz));
                        }
                    }
                    vec3 dir_y = unjittered_ray_direction(px_c + vec2(0.0, 1.0));
                    if (abs(dir_y.y) > 0.0001)
                    {
                        float ty = -origin.y / dir_y.y;
                        if (ty > 0.0)
                        {
                            vec3 py = origin + dir_y * ty;
                            fp = max(fp, length(py.xz - plane_position.xz));
                        }
                    }
                    color = evaluate_grid(plane_position, fp);
                    grid_hit = true;
                    grid_hit_t = plane_t;
                    grid_hit_pos = plane_position;
                }
            }
        }
    }

    primary_payload.color = vec4(color, 1.0);
    // Populate hit_distance / pos_ws_* on grid hits so the rgen
    // computes proper motion vectors for the static world-space grid
    // point. Without this, motion vector defaults to 0 and TAA blends
    // each pixel with last frame's screen-space pixel — which is a
    // *different* grid coordinate when the camera pans/zooms, producing
    // a noticeable lag on the grid lines. Grid is static, so
    // pos_ws_prev == pos_ws_curr.
    if (grid_hit)
    {
        primary_payload.hit_distance = grid_hit_t;
        primary_payload.pos_ws_curr  = grid_hit_pos;
        primary_payload.pos_ws_prev  = grid_hit_pos;
    }
    else
    {
        primary_payload.hit_distance = 1e30;
        primary_payload.pos_ws_curr  = vec3(0.0);
        primary_payload.pos_ws_prev  = vec3(0.0);
    }
    primary_payload.depth = primary_payload.depth;
}
