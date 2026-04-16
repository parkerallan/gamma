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
    vec4 tangent;
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
    vec4 emissive_data;
    vec4 surface_data;
    uint base_color_texture_index;
    uint metallic_roughness_texture_index;
    uint normal_texture_index;
    uint occlusion_texture_index;
    uint emissive_texture_index;
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

layout(set = 0, binding = 6) uniform sampler2D material_textures[256];

const float PI = 3.1415926535897932384626433832795;

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
    if (material.base_color_texture_index != 0xFFFFFFFFu && material.base_color_texture_index < scene_uniforms.counts.w)
    {
        sampled = texture(material_textures[nonuniformEXT(int(material.base_color_texture_index))], uv);
        sampled.rgb = clamp(sampled.rgb, vec3(0.0), vec3(1.0));
    }

    return sampled;
}

vec2 sample_metallic_roughness(MaterialRecord material, vec2 uv)
{
    vec2 sampled = vec2(clamp(material.surface_data.x, 0.0, 1.0), clamp(material.surface_data.y, 0.045, 1.0));
    if (material.metallic_roughness_texture_index != 0xFFFFFFFFu && material.metallic_roughness_texture_index < scene_uniforms.counts.w)
    {
        vec4 packed = texture(material_textures[nonuniformEXT(int(material.metallic_roughness_texture_index))], uv);
        sampled.x *= packed.b;
        sampled.y *= packed.g;
    }

    sampled.y = clamp(sampled.y, 0.045, 1.0);
    return sampled;
}

vec3 fallback_tangent(vec3 normal)
{
    vec3 reference_axis = abs(normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    return normalize(cross(reference_axis, normal));
}

vec3 sample_normal(MaterialRecord material, vec2 uv, vec3 object_normal, vec4 object_tangent)
{
    if (material.normal_texture_index == 0xFFFFFFFFu || material.normal_texture_index >= scene_uniforms.counts.w)
    {
        return normalize(object_normal);
    }

    vec3 tangent = object_tangent.xyz;
    if (dot(tangent, tangent) <= 0.0001)
    {
        tangent = fallback_tangent(object_normal);
    }
    tangent = tangent - object_normal * dot(object_normal, tangent);
    if (dot(tangent, tangent) <= 0.0001)
    {
        tangent = fallback_tangent(object_normal);
    }
    tangent = normalize(tangent);

    vec3 bitangent = normalize(cross(object_normal, tangent)) * (object_tangent.w >= 0.0 ? 1.0 : -1.0);
    vec3 tangent_space_normal = texture(material_textures[nonuniformEXT(int(material.normal_texture_index))], uv).xyz * 2.0 - 1.0;
    tangent_space_normal.xy *= material.emissive_data.w;
    tangent_space_normal = normalize(tangent_space_normal);

    mat3 tbn = mat3(tangent, bitangent, normalize(object_normal));
    return normalize(tbn * tangent_space_normal);
}

float sample_occlusion(MaterialRecord material, vec2 uv)
{
    if (material.occlusion_texture_index == 0xFFFFFFFFu || material.occlusion_texture_index >= scene_uniforms.counts.w)
    {
        return 1.0;
    }

    float occlusion = texture(material_textures[nonuniformEXT(int(material.occlusion_texture_index))], uv).r;
    return mix(1.0, occlusion, clamp(material.surface_data.z, 0.0, 1.0));
}

vec3 sample_emissive(MaterialRecord material, vec2 uv)
{
    vec3 emissive = material.emissive_data.rgb;
    if (material.emissive_texture_index != 0xFFFFFFFFu && material.emissive_texture_index < scene_uniforms.counts.w)
    {
        emissive *= texture(material_textures[nonuniformEXT(int(material.emissive_texture_index))], uv).rgb;
    }

    return emissive;
}

float distribution_ggx(float dot_nh, float roughness)
{
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denom = dot_nh * dot_nh * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(PI * denom * denom, 0.0001);
}

float geometry_schlick_ggx(float dot_value, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return dot_value / max(dot_value * (1.0 - k) + k, 0.0001);
}

float geometry_smith(float dot_nl, float dot_nv, float roughness)
{
    return geometry_schlick_ggx(dot_nl, roughness) * geometry_schlick_ggx(dot_nv, roughness);
}

vec3 fresnel_schlick(float cos_theta, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);
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
    vec4 object_tangent = vertex0.tangent * barycentrics.x + vertex1.tangent * barycentrics.y + vertex2.tangent * barycentrics.z;

    vec4 albedo = sample_base_color(material, uv) * vertex_color;
    float alpha = clamp(albedo.a, 0.0, 1.0);
    vec2 metallic_roughness = sample_metallic_roughness(material, uv);
    float metallic = metallic_roughness.x;
    float roughness = metallic_roughness.y;
    float ambient_occlusion = sample_occlusion(material, uv);
    vec3 emissive = sample_emissive(material, uv);
    object_normal = sample_normal(material, uv, object_normal, object_tangent);
    vec3 world_position = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    vec3 world_normal = normalize(mat3(gl_ObjectToWorldEXT) * object_normal);
    if (dot(world_normal, gl_WorldRayDirectionEXT) > 0.0)
    {
        world_normal = -world_normal;
    }

    vec3 view_direction = normalize(-gl_WorldRayDirectionEXT);
    float dot_nv = max(dot(world_normal, view_direction), 0.0);
    vec3 f0 = mix(vec3(0.04), albedo.rgb, metallic);

    vec3 lighting = scene_uniforms.ambient_light.rgb * scene_uniforms.ambient_light.a * albedo.rgb * (1.0 - metallic) * ambient_occlusion;
    vec3 shadow_origin = world_position + world_normal * 0.01;

    if (scene_uniforms.directional_light_color.a > 0.0)
    {
        vec3 light_direction = normalize(-scene_uniforms.directional_light_direction.xyz);
        float dot_nl = max(dot(world_normal, light_direction), 0.0);
        if (dot_nl > 0.0)
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

            vec3 half_vector = normalize(view_direction + light_direction);
            float dot_nh = max(dot(world_normal, half_vector), 0.0);
            float dot_hv = max(dot(half_vector, view_direction), 0.0);
            float d = distribution_ggx(dot_nh, roughness);
            float g = geometry_smith(dot_nl, dot_nv, roughness);
            vec3 f = fresnel_schlick(dot_hv, f0);
            vec3 specular = (d * g * f) / max(4.0 * dot_nl * dot_nv, 0.001);
            vec3 kd = (vec3(1.0) - f) * (1.0 - metallic);
            vec3 radiance = scene_uniforms.directional_light_color.rgb * scene_uniforms.directional_light_color.a * shadow_payload;
            lighting += (kd * albedo.rgb / PI + specular) * radiance * dot_nl;
        }
    }

    if (scene_uniforms.spot_light_color.a > 0.0)
    {
        vec3 to_light = scene_uniforms.spot_light_position.xyz - world_position;
        float distance_to_light = length(to_light);
        if (distance_to_light > 0.0001)
        {
            vec3 light_direction = to_light / distance_to_light;
            float dot_nl = max(dot(world_normal, light_direction), 0.0);
            if (dot_nl > 0.0)
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

                    vec3 half_vector = normalize(view_direction + light_direction);
                    float dot_nh = max(dot(world_normal, half_vector), 0.0);
                    float dot_hv = max(dot(half_vector, view_direction), 0.0);
                    float d = distribution_ggx(dot_nh, roughness);
                    float g = geometry_smith(dot_nl, dot_nv, roughness);
                    vec3 f = fresnel_schlick(dot_hv, f0);
                    vec3 specular = (d * g * f) / max(4.0 * dot_nl * dot_nv, 0.001);
                    vec3 kd = (vec3(1.0) - f) * (1.0 - metallic);
                    vec3 radiance = scene_uniforms.spot_light_color.rgb * scene_uniforms.spot_light_color.a * attenuation * shadow_payload;
                    lighting += (kd * albedo.rgb / PI + specular) * radiance * dot_nl;
                }
            }
        }
    }

    vec3 shaded_color = lighting + emissive;

    if (primary_payload.depth < 3u)
    {
        vec3 fresnel = fresnel_schlick(dot_nv, f0);
        float reflection_sharpness = clamp(1.0 - roughness, 0.0, 1.0);
        float reflection_weight = reflection_sharpness * reflection_sharpness;
        reflection_weight *= reflection_weight;
        if (max(max(fresnel.r, fresnel.g), fresnel.b) * reflection_weight > 0.01)
        {
            vec3 reflection_direction = reflect(gl_WorldRayDirectionEXT, world_normal);
            reflection_direction = normalize(mix(reflection_direction, world_normal, roughness * roughness));
            const uint current_depth = primary_payload.depth;
            primary_payload.color = vec4(0.12, 0.14, 0.18, 1.0);
            primary_payload.hit_distance = 1e30;
            primary_payload.depth = current_depth + 1u;
            traceRayEXT(
                top_level_as,
                gl_RayFlagsNoneEXT,
                0xFF,
                0,
                1,
                0,
                shadow_origin,
                0.001,
                normalize(reflection_direction),
                10000.0,
                0);
            vec3 reflection_color = primary_payload.color.rgb;
            shaded_color += reflection_color * fresnel * reflection_weight * ambient_occlusion;
            primary_payload.depth = current_depth;
        }
    }

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
