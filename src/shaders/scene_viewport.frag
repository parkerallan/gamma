#version 450

layout(push_constant) uniform ScenePushConstants
{
    mat4 model;
    mat4 model_view_projection;
    vec4 ambient_light;
    vec4 directional_light_color;
    vec4 directional_light_direction;
    vec4 spot_light_color;
    vec4 spot_light_direction;
    vec4 spot_light_position;
    vec4 spot_light_data;
} scene_push_constants;

layout(set = 0, binding = 0) uniform sampler2D base_color_texture;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec3 in_world_normal;
layout(location = 3) in vec3 in_world_position;

layout(location = 0) out vec4 out_color;

void main()
{
    vec4 albedo = texture(base_color_texture, in_uv) * in_color;
    vec3 normal = normalize(in_world_normal);
    vec3 lighting = scene_push_constants.ambient_light.rgb * scene_push_constants.ambient_light.a;

    if (scene_push_constants.directional_light_color.a > 0.0)
    {
        vec3 light_direction = normalize(-scene_push_constants.directional_light_direction.xyz);
        float diffuse = max(dot(normal, light_direction), 0.0);
        lighting += scene_push_constants.directional_light_color.rgb * scene_push_constants.directional_light_color.a * diffuse;
    }

    if (scene_push_constants.spot_light_color.a > 0.0)
    {
        vec3 light_direction = normalize(-scene_push_constants.spot_light_direction.xyz);
        vec3 to_light = scene_push_constants.spot_light_position.xyz - in_world_position;
        float distance_to_light = length(to_light);
        if (distance_to_light > 0.0001)
        {
            light_direction = normalize(to_light);
        }

        float range = max(scene_push_constants.spot_light_position.w, 0.0001);
        float range_factor = clamp(1.0 - (distance_to_light / range), 0.0, 1.0);
        float cone_cos = dot(normalize(-scene_push_constants.spot_light_direction.xyz), light_direction);
        float cone_factor = smoothstep(scene_push_constants.spot_light_data.x, scene_push_constants.spot_light_direction.w, cone_cos);
        float attenuation = range_factor * range_factor * cone_factor;
        float diffuse = max(dot(normal, light_direction), 0.0);
        lighting += scene_push_constants.spot_light_color.rgb * scene_push_constants.spot_light_color.a * diffuse * attenuation;
    }

    out_color = vec4(albedo.rgb * lighting, albedo.a);
}