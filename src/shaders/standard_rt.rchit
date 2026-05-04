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
const uint SOFT_SHADOW_SAMPLE_COUNT = 6u;

uint pcg_hash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float hash_to_unit_float(uint value)
{
    return float(pcg_hash(value)) * (1.0 / 4294967296.0);
}

float radical_inverse_vdc(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 hammersley(uint sample_index, uint sample_count, uint scramble)
{
    return vec2(
        (float(sample_index) + hash_to_unit_float(scramble ^ sample_index)) / float(sample_count),
        radical_inverse_vdc(sample_index ^ scramble));
}

vec2 sample_concentric_disk(vec2 xi)
{
    vec2 offset = 2.0 * xi - vec2(1.0);
    if (offset.x == 0.0 && offset.y == 0.0)
    {
        return vec2(0.0);
    }

    float radius = 0.0;
    float theta = 0.0;
    if (abs(offset.x) > abs(offset.y))
    {
        radius = offset.x;
        theta = (PI * 0.25) * (offset.y / offset.x);
    }
    else
    {
        radius = offset.y;
        theta = (PI * 0.5) - (PI * 0.25) * (offset.x / offset.y);
    }

    return radius * vec2(cos(theta), sin(theta));
}

void build_basis(vec3 normal, out vec3 tangent, out vec3 bitangent)
{
    vec3 reference_axis = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    tangent = normalize(cross(reference_axis, normal));
    bitangent = cross(normal, tangent);
}

vec2 rotate_disk_sample(vec2 sample_point, float rotation)
{
    float s = sin(rotation);
    float c = cos(rotation);
    return vec2(c * sample_point.x - s * sample_point.y, s * sample_point.x + c * sample_point.y);
}

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

uint make_sample_seed(vec3 world_position)
{
    uvec3 position_bits = floatBitsToUint(world_position * 64.0);
    return pcg_hash(
        position_bits.x ^
        (position_bits.y * 31u) ^
    (position_bits.z * 131u) ^
    (gl_InstanceCustomIndexEXT * 3571u));
}

vec3 evaluate_direct_brdf(vec3 albedo_rgb, float metallic, float roughness, vec3 f0, vec3 world_normal, vec3 view_direction, vec3 light_direction)
{
    float dot_nl = max(dot(world_normal, light_direction), 0.0);
    if (dot_nl <= 0.0)
    {
        return vec3(0.0);
    }

    float dot_nv = max(dot(world_normal, view_direction), 0.0);
    vec3 half_vector = normalize(view_direction + light_direction);
    float dot_nh = max(dot(world_normal, half_vector), 0.0);
    float dot_hv = max(dot(half_vector, view_direction), 0.0);
    float d = distribution_ggx(dot_nh, roughness);
    float g = geometry_smith(dot_nl, dot_nv, roughness);
    vec3 f = fresnel_schlick(dot_hv, f0);
    vec3 specular = (d * g * f) / max(4.0 * dot_nl * dot_nv, 0.001);
    vec3 kd = (vec3(1.0) - f) * (1.0 - metallic);
    return (kd * albedo_rgb / PI + specular) * dot_nl;
}

float trace_shadow_visibility(vec3 origin, vec3 direction, float max_distance)
{
    shadow_payload = 1.0;
    traceRayEXT(
        top_level_as,
        gl_RayFlagsNoneEXT,
        0xFF,
        1,
        1,
        1,
        origin,
        0.001,
        direction,
        max_distance,
        1);
    return shadow_payload;
}

vec3 evaluate_point_light_emitter(vec3 ray_origin, vec3 ray_direction, float scene_hit_distance)
{
    if (scene_uniforms.point_light_color.a <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 light_center = scene_uniforms.point_light_position.xyz;
    float source_radius = max(scene_uniforms.point_light_data.x, 0.02);
    float halo_intensity = max(scene_uniforms.point_light_data.y, 0.0);
    float halo_radius = max(scene_uniforms.point_light_data.z, 0.01);
    vec3 base_color = max(scene_uniforms.point_light_color.rgb, vec3(0.0));
    float intensity = max(scene_uniforms.point_light_color.a, 0.0);

    vec3 center_delta = light_center - ray_origin;
    float closest_t = dot(center_delta, ray_direction);
    vec3 closest_point = ray_origin + ray_direction * max(closest_t, 0.0);
    float radial_distance = length(light_center - closest_point);

    vec3 result = vec3(0.0);

    if (closest_t > 0.0 && closest_t < scene_hit_distance)
    {
        float inv_dist = 1.0 / max(closest_t, 0.001);
        float angular_radial = radial_distance * inv_dist;
        float angular_source = source_radius * inv_dist;

        float halo_sigma_outer_ang = max(angular_source * 4.2 * halo_radius, 0.00015);
        float halo_sigma_inner_ang = max(angular_source * 1.6 * halo_radius, 0.00008);

        float halo_outer = exp(-(angular_radial * angular_radial) / max(halo_sigma_outer_ang * halo_sigma_outer_ang, 1e-6));
        float halo_inner = exp(-(angular_radial * angular_radial) / max(halo_sigma_inner_ang * halo_sigma_inner_ang, 1e-6));

        float halo_hotness = clamp(halo_inner * 0.75 + halo_outer * 0.25, 0.0, 1.0);
        vec3 halo_color = mix(base_color * 0.55, vec3(1.0, 0.985, 0.955), halo_hotness * 0.55);
        float halo_energy = (halo_outer * 0.55 + halo_inner * 0.45);
        result += halo_color * intensity * halo_intensity * halo_energy * 1.1;
    }

    vec3 oc = ray_origin - light_center;
    float b = dot(oc, ray_direction);
    float c = dot(oc, oc) - source_radius * source_radius;
    float h = b * b - c;
    if (h >= 0.0)
    {
        float s = sqrt(h);
        float t_near = -b - s;
        float t_far = -b + s;
        float hit_t = t_near > 0.0 ? t_near : (t_far > 0.0 ? t_far : -1.0);
        if (hit_t > 0.0 && hit_t < scene_hit_distance)
        {
            vec3 hit_position = ray_origin + ray_direction * hit_t;
            vec3 sphere_normal = normalize(hit_position - light_center);
            float facing = clamp(dot(-ray_direction, sphere_normal), 0.0, 1.0);
            float radial_unit = clamp(radial_distance / max(source_radius, 1e-6), 0.0, 1.0);
            float radial_center = 1.0 - radial_unit;

            float body_weight = pow(radial_center, 1.8);
            float core_weight = pow(radial_center, 3.2);
            float limb = 0.5 + 0.5 * facing;

            float whiten = clamp(body_weight * 0.55 + core_weight * 0.45, 0.0, 1.0);
            vec3 warm_white = vec3(1.0, 0.992, 0.97);
            vec3 radial_color = mix(base_color, warm_white, whiten);

            float brightness = (1.2 + 4.5 * body_weight + 7.5 * core_weight) * limb;
            result += radial_color * intensity * brightness;
        }
    }

    return result;
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
    vec3 interpolated_object_normal = normalize(vertex0.normal * barycentrics.x + vertex1.normal * barycentrics.y + vertex2.normal * barycentrics.z);
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
    vec3 shading_object_normal = sample_normal(material, uv, interpolated_object_normal, object_tangent);

    vec3 world_position = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    vec3 world_vertex_normal = normalize(mat3(gl_ObjectToWorldEXT) * interpolated_object_normal);
    vec3 world_normal = normalize(mat3(gl_ObjectToWorldEXT) * shading_object_normal);
    vec3 object_geometric_normal = normalize(cross(vertex1.position - vertex0.position, vertex2.position - vertex0.position));
    vec3 world_geometric_normal = normalize(mat3(gl_ObjectToWorldEXT) * object_geometric_normal);
    if (dot(world_geometric_normal, gl_WorldRayDirectionEXT) > 0.0)
    {
        world_geometric_normal = -world_geometric_normal;
    }
    if (dot(world_normal, world_geometric_normal) < 0.0)
    {
        world_normal = -world_normal;
    }
    if (dot(world_vertex_normal, world_geometric_normal) < 0.0)
    {
        world_vertex_normal = -world_vertex_normal;
    }

    vec3 view_direction = normalize(-gl_WorldRayDirectionEXT);
    float dot_nv = max(dot(world_normal, view_direction), 0.0);
    vec3 f0 = mix(vec3(0.04), albedo.rgb, metallic);
    uint sample_seed = make_sample_seed(world_position);
    vec3 shadow_offset_normal = dot(world_vertex_normal, world_geometric_normal) > 0.25 ? world_vertex_normal : world_geometric_normal;

    vec3 lighting = scene_uniforms.ambient_light.rgb * scene_uniforms.ambient_light.a * albedo.rgb * (1.0 - metallic) * ambient_occlusion;
    vec3 shadow_origin = world_position + shadow_offset_normal * 0.0015;

    if (scene_uniforms.directional_light_color.a > 0.0)
    {
        vec3 light_direction = normalize(-scene_uniforms.directional_light_direction.xyz);
        vec3 sample_tangent;
        vec3 sample_bitangent;
        build_basis(light_direction, sample_tangent, sample_bitangent);
        float angular_radius = max(scene_uniforms.directional_light_data.x, 0.0);
        float disk_radius = tan(angular_radius);
        uint sample_count = angular_radius > 0.00001 ? SOFT_SHADOW_SAMPLE_COUNT : 1u;
        float sample_rotation = hash_to_unit_float(sample_seed ^ 0x68bc21ebu) * (2.0 * PI);
        vec3 light_sum = vec3(0.0);

        for (uint sample_index = 0u; sample_index < sample_count; ++sample_index)
        {
            vec2 disk_sample = sample_count > 1u ? sample_concentric_disk(hammersley(sample_index, sample_count, sample_seed ^ 0x9e3779b9u)) : vec2(0.0);
            disk_sample = rotate_disk_sample(disk_sample, sample_rotation);
            vec3 sampled_light_direction = normalize(light_direction + (sample_tangent * disk_sample.x + sample_bitangent * disk_sample.y) * disk_radius);
            vec3 brdf = evaluate_direct_brdf(albedo.rgb, metallic, roughness, f0, world_normal, view_direction, sampled_light_direction);
            if (max(brdf.r, max(brdf.g, brdf.b)) <= 0.0)
            {
                continue;
            }

            float visibility = trace_shadow_visibility(shadow_origin + sampled_light_direction * 0.0025, sampled_light_direction, 10000.0);
            vec3 radiance = scene_uniforms.directional_light_color.rgb * scene_uniforms.directional_light_color.a * visibility;
            light_sum += brdf * radiance;
        }

        lighting += light_sum / float(sample_count);
    }

    if (scene_uniforms.point_light_color.a > 0.0)
    {
        float source_radius = max(scene_uniforms.point_light_data.x, 0.0);
        uint sample_count = source_radius > 0.00001 ? SOFT_SHADOW_SAMPLE_COUNT : 1u;
        float sample_rotation = hash_to_unit_float(sample_seed ^ 0x2f6e2b1du) * (2.0 * PI);
        vec3 light_sum = vec3(0.0);
        vec3 center_to_light = scene_uniforms.point_light_position.xyz - world_position;
        float center_distance = length(center_to_light);
        vec3 sample_axis = center_distance > 0.0001 ? (center_to_light / center_distance) : vec3(0.0, 1.0, 0.0);
        vec3 sample_tangent;
        vec3 sample_bitangent;
        build_basis(sample_axis, sample_tangent, sample_bitangent);

        for (uint sample_index = 0u; sample_index < sample_count; ++sample_index)
        {
            vec2 disk_sample = sample_count > 1u ? sample_concentric_disk(hammersley(sample_index, sample_count, sample_seed ^ 0xb7e15162u)) : vec2(0.0);
            disk_sample = rotate_disk_sample(disk_sample, sample_rotation) * source_radius;
            vec3 sampled_light_position = scene_uniforms.point_light_position.xyz + sample_tangent * disk_sample.x + sample_bitangent * disk_sample.y;
            vec3 to_light = sampled_light_position - world_position;
            float distance_to_light = length(to_light);
            if (distance_to_light <= 0.0001)
            {
                continue;
            }

            vec3 light_direction = to_light / distance_to_light;
            float range = max(scene_uniforms.point_light_position.w, 0.0001);
            float normalized_distance = distance_to_light / range;
            float range_fade = clamp(1.0 - normalized_distance * normalized_distance * normalized_distance * normalized_distance, 0.0, 1.0);
            float smooth_range = range_fade * range_fade;
            float inverse_square = 1.0 / max(distance_to_light * distance_to_light, 0.01);
            float attenuation = inverse_square * smooth_range;
            if (attenuation <= 0.0)
            {
                continue;
            }

            vec3 brdf = evaluate_direct_brdf(albedo.rgb, metallic, roughness, f0, world_normal, view_direction, light_direction);
            if (max(brdf.r, max(brdf.g, brdf.b)) <= 0.0)
            {
                continue;
            }

            float visibility = trace_shadow_visibility(shadow_origin + light_direction * 0.0025, light_direction, max(distance_to_light - 0.01, 0.001));
            vec3 radiance = scene_uniforms.point_light_color.rgb * scene_uniforms.point_light_color.a * attenuation * visibility;
            light_sum += brdf * radiance;
        }

        lighting += light_sum / float(sample_count);
    }

    if (scene_uniforms.spot_light_color.a > 0.0)
    {
        vec3 light_axis = normalize(-scene_uniforms.spot_light_direction.xyz);
        vec3 sample_tangent;
        vec3 sample_bitangent;
        build_basis(light_axis, sample_tangent, sample_bitangent);
        float source_radius = max(scene_uniforms.spot_light_data.y, 0.0);
        uint sample_count = source_radius > 0.00001 ? SOFT_SHADOW_SAMPLE_COUNT : 1u;
        float sample_rotation = hash_to_unit_float(sample_seed ^ 0x51f15e5du) * (2.0 * PI);
        vec3 light_sum = vec3(0.0);

        for (uint sample_index = 0u; sample_index < sample_count; ++sample_index)
        {
            vec2 disk_sample = sample_count > 1u ? sample_concentric_disk(hammersley(sample_index, sample_count, sample_seed ^ 0x243f6a88u)) : vec2(0.0);
            disk_sample = rotate_disk_sample(disk_sample, sample_rotation) * source_radius;
            vec3 sampled_light_position = scene_uniforms.spot_light_position.xyz + sample_tangent * disk_sample.x + sample_bitangent * disk_sample.y;
            vec3 to_light = sampled_light_position - world_position;
            float distance_to_light = length(to_light);
            if (distance_to_light <= 0.0001)
            {
                continue;
            }

            vec3 light_direction = to_light / distance_to_light;
            float range = max(scene_uniforms.spot_light_position.w, 0.0001);
            float range_factor = clamp(1.0 - (distance_to_light / range), 0.0, 1.0);
            float cone_cos = dot(light_axis, light_direction);
            float cone_factor = smoothstep(scene_uniforms.spot_light_data.x, scene_uniforms.spot_light_direction.w, cone_cos);
            float attenuation = range_factor * range_factor * cone_factor;
            if (attenuation <= 0.0)
            {
                continue;
            }

            vec3 brdf = evaluate_direct_brdf(albedo.rgb, metallic, roughness, f0, world_normal, view_direction, light_direction);
            if (max(brdf.r, max(brdf.g, brdf.b)) <= 0.0)
            {
                continue;
            }

            float visibility = trace_shadow_visibility(shadow_origin + light_direction * 0.0025, light_direction, max(distance_to_light - 0.01, 0.001));
            vec3 radiance = scene_uniforms.spot_light_color.rgb * scene_uniforms.spot_light_color.a * attenuation * visibility;
            light_sum += brdf * radiance;
        }

        lighting += light_sum / float(sample_count);
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
            reflection_color += evaluate_point_light_emitter(shadow_origin, normalize(reflection_direction), primary_payload.hit_distance);
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
