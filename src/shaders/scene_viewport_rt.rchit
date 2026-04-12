#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require

struct PrimaryPayload
{
    vec4 color;
    float hit_distance;
    uint depth;
};

layout(location = 0) rayPayloadInEXT PrimaryPayload primary_payload;
layout(location = 1) rayPayloadEXT float shadow_payload;
hitAttributeEXT vec2 hit_attributes;

layout(set = 0, binding = 0) uniform accelerationStructureEXT top_level_as;
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

struct SceneVertex
{
    vec3 position;
    vec3 normal;
    vec2 uv;
    vec4 color;
};

layout(buffer_reference, scalar) readonly buffer VertexBuffer
{
    SceneVertex vertices[];
};

layout(buffer_reference, scalar) readonly buffer IndexBuffer
{
    uint indices[];
};

struct MeshRecord
{
    VertexBuffer vertex_buffer;
    IndexBuffer index_buffer;
    uint vertex_count;
    uint vertex_stride;
    uint index_count;
    uint section_offset;
    uint section_count;
    uint material_offset;
};

struct SectionRecord
{
    uint first_index;
    uint index_count;
    uint material_index;
    uint uses_alpha_transparency;
};

struct MaterialRecord
{
    vec4 base_color;
    uint texture_index;
    uint uses_alpha_transparency;
    uint pad0;
    uint pad1;
};

layout(set = 0, binding = 3, scalar) readonly buffer MeshRecordBuffer
{
    MeshRecord meshes[];
};

layout(set = 0, binding = 4, scalar) readonly buffer SectionRecordBuffer
{
    SectionRecord sections[];
};

layout(set = 0, binding = 5, scalar) readonly buffer MaterialRecordBuffer
{
    MaterialRecord materials[];
};

layout(set = 0, binding = 6) uniform sampler2D base_color_textures[256];

uint find_section_index(MeshRecord mesh, uint first_index)
{
    for (uint section_offset = 0; section_offset < mesh.section_count; ++section_offset)
    {
        uint section_index = mesh.section_offset + section_offset;
        SectionRecord section = sections[section_index];
        if (first_index >= section.first_index && first_index < section.first_index + section.index_count)
        {
            return section_index;
        }
    }

    return mesh.section_offset;
}

vec4 sample_base_color(MaterialRecord material, vec2 uv)
{
    vec4 sampled = vec4(1.0);
    if (material.texture_index != 0xFFFFFFFFu && material.texture_index < scene_uniforms.counts.w)
    {
        sampled = texture(base_color_textures[nonuniformEXT(int(material.texture_index))], uv);
        if (sampled.a > 0.0001)
        {
            sampled.rgb /= sampled.a;
        }
        sampled.rgb = clamp(sampled.rgb, vec3(0.0), vec3(1.0));
    }

    return sampled;
}

void main()
{
    MeshRecord mesh = meshes[gl_InstanceCustomIndexEXT];
    uint primitive_first_index = gl_PrimitiveID * 3u;
    uint section_index = find_section_index(mesh, primitive_first_index);
    SectionRecord section = sections[section_index];
    MaterialRecord material = materials[section.material_index];

    uint index0 = mesh.index_buffer.indices[primitive_first_index + 0u];
    uint index1 = mesh.index_buffer.indices[primitive_first_index + 1u];
    uint index2 = mesh.index_buffer.indices[primitive_first_index + 2u];

    SceneVertex vertex0 = mesh.vertex_buffer.vertices[index0];
    SceneVertex vertex1 = mesh.vertex_buffer.vertices[index1];
    SceneVertex vertex2 = mesh.vertex_buffer.vertices[index2];

    vec3 barycentrics = vec3(1.0 - hit_attributes.x - hit_attributes.y, hit_attributes.x, hit_attributes.y);
    vec3 object_normal = normalize(vertex0.normal * barycentrics.x + vertex1.normal * barycentrics.y + vertex2.normal * barycentrics.z);
    vec2 uv = vertex0.uv * barycentrics.x + vertex1.uv * barycentrics.y + vertex2.uv * barycentrics.z;
    vec4 vertex_color = vertex0.color * barycentrics.x + vertex1.color * barycentrics.y + vertex2.color * barycentrics.z;

    vec4 albedo = sample_base_color(material, uv) * vertex_color;
    float alpha = clamp(albedo.a, 0.0, 1.0);
    vec3 world_position = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    vec3 world_normal = normalize(mat3(gl_ObjectToWorldEXT) * object_normal);

    vec3 lighting = scene_uniforms.ambient_light.rgb * scene_uniforms.ambient_light.a;
    vec3 shadow_origin = world_position + world_normal * 0.01;

    if (scene_uniforms.directional_light_color.a > 0.0)
    {
        vec3 light_direction = normalize(-scene_uniforms.directional_light_direction.xyz);
        float diffuse = max(dot(world_normal, light_direction), 0.0);
        if (diffuse > 0.0)
        {
            shadow_payload = 1.0;
            traceRayEXT(
                top_level_as,
                gl_RayFlagsNoneEXT,
                0xFF,
                1,
                1,
                1,
                shadow_origin,
                0.001,
                light_direction,
                10000.0,
                1);
            lighting += scene_uniforms.directional_light_color.rgb * scene_uniforms.directional_light_color.a * diffuse * shadow_payload;
        }
    }

    if (scene_uniforms.spot_light_color.a > 0.0)
    {
        vec3 to_light = scene_uniforms.spot_light_position.xyz - world_position;
        float distance_to_light = length(to_light);
        if (distance_to_light > 0.0001)
        {
            vec3 light_direction = to_light / distance_to_light;
            float diffuse = max(dot(world_normal, light_direction), 0.0);
            if (diffuse > 0.0)
            {
                float range = max(scene_uniforms.spot_light_position.w, 0.0001);
                float range_factor = clamp(1.0 - (distance_to_light / range), 0.0, 1.0);
                float cone_cos = dot(normalize(-scene_uniforms.spot_light_direction.xyz), light_direction);
                float cone_factor = smoothstep(scene_uniforms.spot_light_data.x, scene_uniforms.spot_light_direction.w, cone_cos);
                float attenuation = range_factor * range_factor * cone_factor;
                if (attenuation > 0.0)
                {
                    shadow_payload = 1.0;
                    traceRayEXT(
                        top_level_as,
                        gl_RayFlagsNoneEXT,
                        0xFF,
                        1,
                        1,
                        1,
                        shadow_origin,
                        0.001,
                        light_direction,
                        max(distance_to_light - 0.01, 0.001),
                        1);
                    lighting += scene_uniforms.spot_light_color.rgb * scene_uniforms.spot_light_color.a * diffuse * attenuation * shadow_payload;
                }
            }
        }
    }

    vec3 shaded_color = albedo.rgb * lighting;
    if (alpha < 0.999 && primary_payload.depth < 3u)
    {
        const uint current_depth = primary_payload.depth;
        primary_payload.color = vec4(0.08, 0.09, 0.11, 1.0);
        primary_payload.hit_distance = 1e30;
        primary_payload.depth = current_depth + 1u;
        traceRayEXT(
            top_level_as,
            gl_RayFlagsNoneEXT,
            0xFF,
            0,
            1,
            0,
            world_position + gl_WorldRayDirectionEXT * 0.01,
            0.001,
            gl_WorldRayDirectionEXT,
            10000.0,
            0);
        shaded_color = mix(primary_payload.color.rgb, shaded_color, alpha);
        primary_payload.depth = current_depth;
    }

    primary_payload.color = vec4(shaded_color, 1.0);
    primary_payload.hit_distance = gl_HitTEXT;
}
