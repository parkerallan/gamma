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
    vec4 iridescence_data;
    vec4 transmission_data;
    vec4 attenuation_data;
    vec4 clearcoat_data;
    vec4 specular_data;
    vec4 sheen_data;
    uint base_color_texture_index;
    uint metallic_roughness_texture_index;
    uint normal_texture_index;
    uint occlusion_texture_index;
    uint emissive_texture_index;
    uint transmission_texture_index;
    uint specular_texture_index;
    uint specular_color_texture_index;
    uint sheen_color_texture_index;
    uint sheen_roughness_texture_index;
    uint iridescence_texture_index;
    uint iridescence_thickness_texture_index;
    uint volume_thickness_texture_index;
    uint clearcoat_texture_index;
    uint clearcoat_roughness_texture_index;
    uint clearcoat_normal_texture_index;
    uint uses_alpha_transparency;
    uint alpha_mode; // 0=OPAQUE, 1=MASK, 2=BLEND
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
const uint ROUGH_TRANSMISSION_SAMPLE_COUNT = 1u;

// Resolve the per-frame shadow sample count for a primary-depth shadow ray.
// When the host sets accumulation_data.z it acts as a minimum override -- this
// is enabled in play / dynamic-geometry mode where temporal accumulation is
// unavailable, so each frame must produce a smoother soft-shadow result on
// its own. Secondary depths always stay at 1 sample to keep cost bounded.
uint effective_shadow_samples(bool is_primary_depth)
{
    if (!is_primary_depth)
    {
        return 1u;
    }
    return max(SOFT_SHADOW_SAMPLE_COUNT, scene_uniforms.accumulation_data.z);
}
const mat3 XYZ_TO_REC709 = mat3(
     3.2404542, -0.9692660,  0.0556434,
    -1.5371385,  1.8760108, -0.2040259,
    -0.4985314,  0.0415560,  1.0572252);

uint pcg_hash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float square(float value)
{
    return value * value;
}

vec3 square(vec3 value)
{
    return value * value;
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

float sample_specular(MaterialRecord material, vec2 uv)
{
    float factor = max(material.specular_data.w, 0.0);
    if (material.specular_texture_index != 0xFFFFFFFFu && material.specular_texture_index < scene_uniforms.counts.w)
    {
        factor *= texture(material_textures[nonuniformEXT(int(material.specular_texture_index))], uv).a;
    }

    return max(factor, 0.0);
}

vec3 sample_specular_color(MaterialRecord material, vec2 uv)
{
    vec3 color = max(material.specular_data.rgb, vec3(0.0));
    if (material.specular_color_texture_index != 0xFFFFFFFFu && material.specular_color_texture_index < scene_uniforms.counts.w)
    {
        color *= texture(material_textures[nonuniformEXT(int(material.specular_color_texture_index))], uv).rgb;
    }

    return max(color, vec3(0.0));
}

vec3 sample_sheen_color(MaterialRecord material, vec2 uv)
{
    vec3 sheen_color = max(material.sheen_data.rgb, vec3(0.0));
    if (material.sheen_color_texture_index != 0xFFFFFFFFu && material.sheen_color_texture_index < scene_uniforms.counts.w)
    {
        sheen_color *= texture(material_textures[nonuniformEXT(int(material.sheen_color_texture_index))], uv).rgb;
    }

    return max(sheen_color, vec3(0.0));
}

float sample_sheen_roughness(MaterialRecord material, vec2 uv)
{
    float roughness = clamp(material.sheen_data.w, 0.0, 1.0);
    if (material.sheen_roughness_texture_index != 0xFFFFFFFFu && material.sheen_roughness_texture_index < scene_uniforms.counts.w)
    {
        roughness *= texture(material_textures[nonuniformEXT(int(material.sheen_roughness_texture_index))], uv).a;
    }

    return clamp(roughness, 0.0, 1.0);
}

float sample_iridescence(MaterialRecord material, vec2 uv)
{
    float factor = clamp(material.iridescence_data.x, 0.0, 1.0);
    if (material.iridescence_texture_index != 0xFFFFFFFFu && material.iridescence_texture_index < scene_uniforms.counts.w)
    {
        factor *= texture(material_textures[nonuniformEXT(int(material.iridescence_texture_index))], uv).r;
    }

    return clamp(factor, 0.0, 1.0);
}

float sample_iridescence_thickness(MaterialRecord material, vec2 uv)
{
    float minimum_thickness = material.iridescence_data.z;
    float maximum_thickness = material.iridescence_data.w;
    float t = 1.0;
    if (material.iridescence_thickness_texture_index != 0xFFFFFFFFu && material.iridescence_thickness_texture_index < scene_uniforms.counts.w)
    {
        t = texture(material_textures[nonuniformEXT(int(material.iridescence_thickness_texture_index))], uv).g;
    }

    return max(mix(minimum_thickness, maximum_thickness, t), 0.0);
}

float sample_volume_thickness(MaterialRecord material, vec2 uv)
{
    float thickness = max(material.transmission_data.z, 0.0);
    if (material.volume_thickness_texture_index != 0xFFFFFFFFu && material.volume_thickness_texture_index < scene_uniforms.counts.w)
    {
        thickness *= texture(material_textures[nonuniformEXT(int(material.volume_thickness_texture_index))], uv).g;
    }

    return max(thickness, 0.0);
}

float sample_transmission(MaterialRecord material, vec2 uv)
{
    float factor = clamp(material.transmission_data.x, 0.0, 1.0);
    if (material.transmission_texture_index != 0xFFFFFFFFu && material.transmission_texture_index < scene_uniforms.counts.w)
    {
        factor *= texture(material_textures[nonuniformEXT(int(material.transmission_texture_index))], uv).r;
    }

    return clamp(factor, 0.0, 1.0);
}

vec3 sample_rough_transmission_direction(vec3 ideal_direction, float roughness, vec2 disk_sample)
{
    vec3 direction = normalize(ideal_direction);
    float roughness_amount = clamp(roughness, 0.0, 1.0);
    if (roughness_amount <= 0.001)
    {
        return direction;
    }

    vec3 tangent;
    vec3 bitangent;
    build_basis(direction, tangent, bitangent);
    float spread = roughness_amount * roughness_amount * 0.45;
    vec3 rough_direction = direction + tangent * disk_sample.x * spread + bitangent * disk_sample.y * spread;
    return normalize(rough_direction);
}

float sample_clearcoat(MaterialRecord material, vec2 uv)
{
    float factor = clamp(material.clearcoat_data.x, 0.0, 1.0);
    if (material.clearcoat_texture_index != 0xFFFFFFFFu && material.clearcoat_texture_index < scene_uniforms.counts.w)
    {
        factor *= texture(material_textures[nonuniformEXT(int(material.clearcoat_texture_index))], uv).r;
    }

    return clamp(factor, 0.0, 1.0);
}

float sample_clearcoat_roughness(MaterialRecord material, vec2 uv)
{
    float roughness = clamp(material.clearcoat_data.y, 0.0, 1.0);
    if (material.clearcoat_roughness_texture_index != 0xFFFFFFFFu && material.clearcoat_roughness_texture_index < scene_uniforms.counts.w)
    {
        roughness *= texture(material_textures[nonuniformEXT(int(material.clearcoat_roughness_texture_index))], uv).g;
    }

    return max(clamp(roughness, 0.0, 1.0), 0.001);
}

vec3 fallback_tangent(vec3 normal)
{
    vec3 reference_axis = abs(normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    return normalize(cross(reference_axis, normal));
}

vec3 sample_normal_texture(uint texture_index, float normal_scale, vec2 uv, vec3 object_normal, vec4 object_tangent)
{
    if (texture_index == 0xFFFFFFFFu || texture_index >= scene_uniforms.counts.w)
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
    vec3 tangent_space_normal = texture(material_textures[nonuniformEXT(int(texture_index))], uv).xyz * 2.0 - 1.0;
    tangent_space_normal.xy *= normal_scale;
    tangent_space_normal = normalize(tangent_space_normal);

    mat3 tbn = mat3(tangent, bitangent, normalize(object_normal));
    return normalize(tbn * tangent_space_normal);
}

vec3 sample_normal(MaterialRecord material, vec2 uv, vec3 object_normal, vec4 object_tangent)
{
    return sample_normal_texture(material.normal_texture_index, material.emissive_data.w, uv, object_normal, object_tangent);
}

vec3 sample_clearcoat_normal(MaterialRecord material, vec2 uv, vec3 object_normal, vec4 object_tangent)
{
    return sample_normal_texture(material.clearcoat_normal_texture_index, material.clearcoat_data.z, uv, object_normal, object_tangent);
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

float distribution_charlie(float dot_nh, float roughness)
{
    float alpha = max(roughness * roughness, 1e-4);
    float inv_alpha = 1.0 / alpha;
    float sin2h = max(1.0 - dot_nh * dot_nh, 1e-6);
    return (2.0 + inv_alpha) * pow(sin2h, 0.5 * inv_alpha) / (2.0 * PI);
}

float visibility_sheen(float dot_nl, float dot_nv)
{
    return 1.0 / max(4.0 * (dot_nl + dot_nv - dot_nl * dot_nv), 1e-4);
}

vec3 fresnel_schlick(float cos_theta, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);
}

vec3 fresnel_schlick(vec3 f0, vec3 f90, float cos_theta)
{
    return f0 + (f90 - f0) * pow(1.0 - cos_theta, 5.0);
}

float fresnel_schlick_scalar(float f0, float cos_theta)
{
    return f0 + (1.0 - f0) * pow(1.0 - cos_theta, 5.0);
}

vec3 fresnel0_to_ior(vec3 fresnel0)
{
    vec3 clamped_f0 = clamp(fresnel0, vec3(0.0), vec3(0.9999));
    vec3 sqrt_f0 = sqrt(clamped_f0);
    return (vec3(1.0) + sqrt_f0) / max(vec3(1.0) - sqrt_f0, vec3(1e-4));
}

vec3 ior_to_fresnel0(vec3 transmitted_ior, float incident_ior)
{
    return square((transmitted_ior - vec3(incident_ior)) / max(transmitted_ior + vec3(incident_ior), vec3(1e-4)));
}

float ior_to_fresnel0(float transmitted_ior, float incident_ior)
{
    float denom = max(transmitted_ior + incident_ior, 1e-4);
    return square((transmitted_ior - incident_ior) / denom);
}

vec3 eval_iridescence_sensitivity(float optical_path_difference, vec3 phase_shift)
{
    float phase = 2.0 * PI * optical_path_difference * 1.0e-9;
    vec3 value = vec3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    vec3 position = vec3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    vec3 variance = vec3(4.3278e+09, 9.3046e+09, 6.6121e+09);

    vec3 xyz = value * sqrt(2.0 * PI * variance) * cos(position * phase + phase_shift) * exp(-square(phase) * variance);
    xyz.x += 9.7470e-14 * sqrt(2.0 * PI * 4.5282e+09) * cos(2.2399e+06 * phase + phase_shift.x) * exp(-4.5282e+09 * square(phase));
    xyz /= 1.0685e-7;
    return XYZ_TO_REC709 * xyz;
}

vec3 evaluate_iridescence_fresnel(vec3 base_f0, float cos_theta, float iridescence_strength, float iridescence_ior, float thickness_nm)
{
    vec3 base_fresnel = fresnel_schlick(cos_theta, base_f0);
    if (iridescence_strength <= 0.0 || thickness_nm <= 0.0)
    {
        return base_fresnel;
    }

    float outer_ior = 1.0;
    float film_ior = mix(outer_ior, max(iridescence_ior, outer_ior), smoothstep(0.0, 0.03, thickness_nm));
    float sin_theta2_sq = square(outer_ior / film_ior) * (1.0 - square(cos_theta));
    float cos_theta2_sq = 1.0 - sin_theta2_sq;
    if (cos_theta2_sq < 0.0)
    {
        return vec3(1.0);
    }

    float cos_theta2 = sqrt(cos_theta2_sq);
    float r0 = ior_to_fresnel0(film_ior, outer_ior);
    float r12 = fresnel_schlick_scalar(r0, cos_theta);
    float transmission = 1.0 - r12;
    float phase_12 = film_ior < outer_ior ? PI : 0.0;
    float phase_21 = PI - phase_12;

    vec3 base_ior = fresnel0_to_ior(base_f0);
    vec3 r1 = ior_to_fresnel0(base_ior, film_ior);
    vec3 r23 = fresnel_schlick(cos_theta2, r1);
    vec3 phase_23 = vec3(0.0);
    if (base_ior.x < film_ior)
    {
        phase_23.x = PI;
    }
    if (base_ior.y < film_ior)
    {
        phase_23.y = PI;
    }
    if (base_ior.z < film_ior)
    {
        phase_23.z = PI;
    }

    float optical_path_difference = 2.0 * film_ior * thickness_nm * cos_theta2;
    vec3 phase = vec3(phase_21) + phase_23;
    vec3 r123 = clamp(r12 * r23, 1e-5, 0.9999);
    vec3 attenuation = sqrt(r123);
    vec3 reflected_series = square(vec3(transmission)) * r23 / max(vec3(1.0) - r123, vec3(1e-4));

    vec3 color = vec3(r12) + reflected_series;
    vec3 series_term = reflected_series - transmission;
    for (int order = 1; order <= 2; ++order)
    {
        series_term *= attenuation;
        vec3 sensitivity = 2.0 * eval_iridescence_sensitivity(float(order) * optical_path_difference, float(order) * phase);
        color += series_term * sensitivity;
    }

    vec3 iridescent_fresnel = max(color, vec3(0.0));
    return clamp(mix(base_fresnel, iridescent_fresnel, clamp(iridescence_strength, 0.0, 1.0)), 0.0, 1.0);
}

float max_component(vec3 value)
{
    return max(value.x, max(value.y, value.z));
}

float average_world_scale()
{
    vec3 axis_x = vec3(gl_ObjectToWorldEXT[0][0], gl_ObjectToWorldEXT[1][0], gl_ObjectToWorldEXT[2][0]);
    vec3 axis_y = vec3(gl_ObjectToWorldEXT[0][1], gl_ObjectToWorldEXT[1][1], gl_ObjectToWorldEXT[2][1]);
    vec3 axis_z = vec3(gl_ObjectToWorldEXT[0][2], gl_ObjectToWorldEXT[1][2], gl_ObjectToWorldEXT[2][2]);
    return max((length(axis_x) + length(axis_y) + length(axis_z)) / 3.0, 1e-4);
}

vec3 evaluate_volume_attenuation(MaterialRecord material, float travel_distance)
{
    if (travel_distance <= 0.0)
    {
        return vec3(1.0);
    }

    float attenuation_distance = material.attenuation_data.w;
    if (attenuation_distance <= 0.0)
    {
        return vec3(1.0);
    }

    vec3 attenuation_color = clamp(material.attenuation_data.rgb, vec3(1e-4), vec3(1.0));
    return pow(attenuation_color, vec3(travel_distance / attenuation_distance));
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

vec3 evaluate_direct_brdf(
    vec3 albedo_rgb,
    float metallic,
    float roughness,
    vec3 f0,
    vec3 f90,
    float iridescence_strength,
    float iridescence_ior,
    float iridescence_thickness,
    float transmission_factor,
    vec3 world_normal,
    vec3 view_direction,
    vec3 light_direction)
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
    vec3 f = iridescence_strength > 0.0
        ? evaluate_iridescence_fresnel(f0, dot_hv, iridescence_strength, iridescence_ior, iridescence_thickness)
        : fresnel_schlick(f0, f90, dot_hv);
    vec3 specular = (d * g * f) / max(4.0 * dot_nl * dot_nv, 0.001);
    vec3 kd = vec3((1.0 - metallic) * (1.0 - transmission_factor) * (1.0 - clamp(max_component(f), 0.0, 1.0)));
    if (iridescence_strength > 0.0)
    {
        kd = vec3((1.0 - metallic) * (1.0 - transmission_factor) * (1.0 - clamp(max_component(f), 0.0, 1.0)));
    }
    return (kd * albedo_rgb / PI + specular) * dot_nl;
}

vec3 evaluate_sheen_direct_brdf(vec3 sheen_color, float sheen_roughness, vec3 world_normal, vec3 view_direction, vec3 light_direction)
{
    float dot_nl = max(dot(world_normal, light_direction), 0.0);
    float dot_nv = max(dot(world_normal, view_direction), 0.0);
    if (dot_nl <= 0.0 || dot_nv <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 half_vector = view_direction + light_direction;
    if (dot(half_vector, half_vector) <= 1e-6)
    {
        return vec3(0.0);
    }

    half_vector = normalize(half_vector);
    float dot_nh = max(dot(world_normal, half_vector), 0.0);
    float distribution = distribution_charlie(dot_nh, max(sheen_roughness, 1e-3));
    float visibility = visibility_sheen(dot_nl, dot_nv);
    return sheen_color * distribution * visibility * dot_nl;
}

// Approximate Charlie sheen directional albedo (energy taken by the sheen lobe).
// Bounded so the base-layer scaling never collapses to zero -- the previous formula
// drove base color to 0 at silhouettes which produced the blue/black outline
// artifacts on SheenDamask / SheenChair. Roughness factor keeps narrow (low-r)
// sheen lobes from stealing too much base energy.
float compute_sheen_base_scaling(vec3 sheen_color, float dot_nv)
{
    float strength = clamp(max_component(sheen_color), 0.0, 1.0);
    float grazing = pow(1.0 - clamp(dot_nv, 0.0, 1.0), 2.5);
    float directional_albedo = 0.157 * grazing;
    return clamp(1.0 - strength * directional_albedo, 0.0, 1.0);
}

vec3 layer_sheen_over_brdf(vec3 base_brdf, vec3 sheen_color, float sheen_roughness, float base_scaling, vec3 world_normal, vec3 view_direction, vec3 light_direction)
{
    if (max_component(sheen_color) <= 0.0)
    {
        return base_brdf;
    }

    return base_brdf * base_scaling + evaluate_sheen_direct_brdf(sheen_color, sheen_roughness, world_normal, view_direction, light_direction);
}

float compute_clearcoat_weight(float clearcoat, vec3 clearcoat_normal, vec3 view_direction)
{
    if (clearcoat <= 0.0)
    {
        return 0.0;
    }

    float dot_nv = clamp(abs(dot(clearcoat_normal, view_direction)), 0.0, 1.0);
    return clamp(clearcoat * fresnel_schlick_scalar(0.04, dot_nv), 0.0, 1.0);
}

vec3 evaluate_clearcoat_direct_brdf(float clearcoat_roughness, vec3 clearcoat_normal, vec3 view_direction, vec3 light_direction)
{
    float dot_nl = max(dot(clearcoat_normal, light_direction), 0.0);
    if (dot_nl <= 0.0)
    {
        return vec3(0.0);
    }

    float dot_nv = max(abs(dot(clearcoat_normal, view_direction)), 0.0);
    vec3 half_vector = view_direction + light_direction;
    if (dot(half_vector, half_vector) <= 1e-6)
    {
        return vec3(0.0);
    }

    half_vector = normalize(half_vector);
    float dot_nh = max(dot(clearcoat_normal, half_vector), 0.0);
    float d = distribution_ggx(dot_nh, clearcoat_roughness);
    float g = geometry_smith(dot_nl, dot_nv, clearcoat_roughness);
    float specular = (d * g) / max(4.0 * dot_nl * dot_nv, 0.001);
    return vec3(specular * dot_nl);
}

vec3 layer_clearcoat_over_brdf(
    vec3 base_brdf,
    float clearcoat_weight,
    float clearcoat_roughness,
    vec3 clearcoat_normal,
    vec3 view_direction,
    vec3 light_direction)
{
    if (clearcoat_weight <= 0.0)
    {
        return base_brdf;
    }

    vec3 clearcoat_brdf = evaluate_clearcoat_direct_brdf(clearcoat_roughness, clearcoat_normal, view_direction, light_direction);
    return mix(base_brdf, clearcoat_brdf, clearcoat_weight);
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
    float iridescence = sample_iridescence(material, uv);
    float iridescence_thickness = sample_iridescence_thickness(material, uv);
    float iridescence_ior = max(material.iridescence_data.y, 1.0);
    float transmission_factor = sample_transmission(material, uv);
    float base_ior = max(material.transmission_data.y, 1.0);
    float volume_thickness = sample_volume_thickness(material, uv);
    float specular_strength = sample_specular(material, uv);
    vec3 specular_color = sample_specular_color(material, uv);
    vec3 sheen_color = sample_sheen_color(material, uv);
    float sheen_roughness = sample_sheen_roughness(material, uv);
    float clearcoat = sample_clearcoat(material, uv);
    float clearcoat_roughness = sample_clearcoat_roughness(material, uv);
    vec3 shading_object_normal = sample_normal(material, uv, interpolated_object_normal, object_tangent);
    vec3 clearcoat_object_normal = sample_clearcoat_normal(material, uv, interpolated_object_normal, object_tangent);

    vec3 world_position = gl_WorldRayOriginEXT + gl_WorldRayDirectionEXT * gl_HitTEXT;
    vec3 world_vertex_normal = normalize(mat3(gl_ObjectToWorldEXT) * interpolated_object_normal);
    vec3 world_normal = normalize(mat3(gl_ObjectToWorldEXT) * shading_object_normal);
    vec3 world_clearcoat_normal = normalize(mat3(gl_ObjectToWorldEXT) * clearcoat_object_normal);
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
    if (dot(world_clearcoat_normal, world_geometric_normal) < 0.0)
    {
        world_clearcoat_normal = -world_clearcoat_normal;
    }

    vec3 view_direction = normalize(-gl_WorldRayDirectionEXT);
    float dot_nv = max(dot(world_normal, view_direction), 0.0);
    vec3 dielectric_f0 = min(vec3(ior_to_fresnel0(base_ior, 1.0)) * specular_color, vec3(1.0)) * specular_strength;
    vec3 dielectric_f90 = vec3(specular_strength);
    vec3 f0 = mix(dielectric_f0, albedo.rgb, metallic);
    vec3 f90 = mix(dielectric_f90, vec3(1.0), metallic);
    vec3 view_fresnel = iridescence > 0.0
        ? evaluate_iridescence_fresnel(f0, dot_nv, iridescence, iridescence_ior, iridescence_thickness)
        : fresnel_schlick(f0, f90, dot_nv);
    float sheen_base_scaling = compute_sheen_base_scaling(sheen_color, dot_nv);
    float clearcoat_weight = compute_clearcoat_weight(clearcoat, world_clearcoat_normal, view_direction);
    float base_layer_weight = 1.0 - clearcoat_weight;
    float diffuse_visibility = 1.0 - metallic;
    if (iridescence > 0.0)
    {
        diffuse_visibility *= 1.0 - clamp(max_component(view_fresnel), 0.0, 1.0);
    }
    uint sample_seed = make_sample_seed(world_position);
    const bool is_primary_depth = primary_payload.depth == 0u;
    const bool skip_direct_lighting = !is_primary_depth && transmission_factor > 0.001;
    vec3 shadow_offset_normal = dot(world_vertex_normal, world_geometric_normal) > 0.25 ? world_vertex_normal : world_geometric_normal;

    vec3 lighting = scene_uniforms.ambient_light.rgb * scene_uniforms.ambient_light.a * albedo.rgb * diffuse_visibility * (1.0 - transmission_factor) * ambient_occlusion * base_layer_weight * sheen_base_scaling;
    vec3 shadow_origin = world_position + shadow_offset_normal * 0.0015;

    if (!skip_direct_lighting && scene_uniforms.directional_light_color.a > 0.0)
    {
        vec3 light_direction = normalize(-scene_uniforms.directional_light_direction.xyz);
        vec3 sample_tangent;
        vec3 sample_bitangent;
        build_basis(light_direction, sample_tangent, sample_bitangent);
        float angular_radius = max(scene_uniforms.directional_light_data.x, 0.0);
        float disk_radius = tan(angular_radius);
        uint sample_count = angular_radius > 0.00001 ? effective_shadow_samples(is_primary_depth) : 1u;
        float sample_rotation = hash_to_unit_float(sample_seed ^ 0x68bc21ebu) * (2.0 * PI);
        vec3 light_sum = vec3(0.0);

        for (uint sample_index = 0u; sample_index < sample_count; ++sample_index)
        {
            vec2 disk_sample = sample_count > 1u ? sample_concentric_disk(hammersley(sample_index, sample_count, sample_seed ^ 0x9e3779b9u)) : vec2(0.0);
            disk_sample = rotate_disk_sample(disk_sample, sample_rotation);
            vec3 sampled_light_direction = normalize(light_direction + (sample_tangent * disk_sample.x + sample_bitangent * disk_sample.y) * disk_radius);
            vec3 brdf = evaluate_direct_brdf(
                albedo.rgb,
                metallic,
                roughness,
                f0,
                f90,
                iridescence,
                iridescence_ior,
                iridescence_thickness,
                transmission_factor,
                world_normal,
                view_direction,
                sampled_light_direction);
            brdf = layer_sheen_over_brdf(
                brdf,
                sheen_color,
                sheen_roughness,
                sheen_base_scaling,
                world_normal,
                view_direction,
                sampled_light_direction);
            brdf = layer_clearcoat_over_brdf(
                brdf,
                clearcoat_weight,
                clearcoat_roughness,
                world_clearcoat_normal,
                view_direction,
                sampled_light_direction);
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

    if (!skip_direct_lighting && scene_uniforms.point_light_color.a > 0.0)
    {
        float source_radius = max(scene_uniforms.point_light_data.x, 0.0);
        uint sample_count = source_radius > 0.00001 ? effective_shadow_samples(is_primary_depth) : 1u;
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

            vec3 brdf = evaluate_direct_brdf(
                albedo.rgb,
                metallic,
                roughness,
                f0,
                f90,
                iridescence,
                iridescence_ior,
                iridescence_thickness,
                transmission_factor,
                world_normal,
                view_direction,
                light_direction);
            brdf = layer_sheen_over_brdf(
                brdf,
                sheen_color,
                sheen_roughness,
                sheen_base_scaling,
                world_normal,
                view_direction,
                light_direction);
            brdf = layer_clearcoat_over_brdf(
                brdf,
                clearcoat_weight,
                clearcoat_roughness,
                world_clearcoat_normal,
                view_direction,
                light_direction);
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

    if (!skip_direct_lighting && scene_uniforms.spot_light_color.a > 0.0)
    {
        vec3 light_axis = normalize(-scene_uniforms.spot_light_direction.xyz);
        vec3 sample_tangent;
        vec3 sample_bitangent;
        build_basis(light_axis, sample_tangent, sample_bitangent);
        float source_radius = max(scene_uniforms.spot_light_data.y, 0.0);
        uint sample_count = source_radius > 0.00001 ? effective_shadow_samples(is_primary_depth) : 1u;
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

            vec3 brdf = evaluate_direct_brdf(
                albedo.rgb,
                metallic,
                roughness,
                f0,
                f90,
                iridescence,
                iridescence_ior,
                iridescence_thickness,
                transmission_factor,
                world_normal,
                view_direction,
                light_direction);
            brdf = layer_sheen_over_brdf(
                brdf,
                sheen_color,
                sheen_roughness,
                sheen_base_scaling,
                world_normal,
                view_direction,
                light_direction);
            brdf = layer_clearcoat_over_brdf(
                brdf,
                clearcoat_weight,
                clearcoat_roughness,
                world_clearcoat_normal,
                view_direction,
                light_direction);
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

    vec3 shaded_color = lighting + emissive * base_layer_weight;

    if (primary_payload.depth < 2u)
    {
        vec3 fresnel = view_fresnel;
        float reflection_sharpness = clamp(1.0 - roughness, 0.0, 1.0);
        float reflection_weight = reflection_sharpness * reflection_sharpness;
        reflection_weight *= reflection_weight;
        // Metals at high roughness would otherwise have weight ~= 0 and never trace,
        // leaving gold/silver surfaces pitch black when no environment is reflected.
        // For a metal the GGX lobe integrates to ~1 (no diffuse fallback exists), so a
        // single-ray reflection estimate needs full weight to match the energy that
        // would arrive via prefiltered IBL. Dielectrics keep the original (1-r)^4
        // attenuation so silhouettes are unchanged.
        reflection_weight = max(reflection_weight, metallic);
        if (max(max(fresnel.r, fresnel.g), fresnel.b) * reflection_weight * base_layer_weight > 0.01)
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
            shaded_color += reflection_color * fresnel * reflection_weight * ambient_occlusion * base_layer_weight * sheen_base_scaling;
            primary_payload.depth = current_depth;
        }

        float clearcoat_reflection_weight = clearcoat_weight;
        float clearcoat_reflection_sharpness = clamp(1.0 - clearcoat_roughness, 0.0, 1.0);
        clearcoat_reflection_weight *= clearcoat_reflection_sharpness * clearcoat_reflection_sharpness;
        clearcoat_reflection_weight *= clearcoat_reflection_sharpness * clearcoat_reflection_sharpness;
        if (clearcoat_reflection_weight > 0.01 && is_primary_depth)
        {
            vec3 reflection_direction = reflect(gl_WorldRayDirectionEXT, world_clearcoat_normal);
            reflection_direction = normalize(mix(reflection_direction, world_clearcoat_normal, clearcoat_roughness * clearcoat_roughness));
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
            shaded_color += reflection_color * clearcoat_reflection_weight * ambient_occlusion;
            primary_payload.depth = current_depth;
        }
    }

    if (transmission_factor > 0.001 && primary_payload.depth < 2u)
    {
        bool front_face = dot(world_geometric_normal, view_direction) > 0.0;
        vec3 transmission_normal = front_face ? world_normal : -world_normal;
        float eta = front_face ? (1.0 / max(base_ior, 1.0001)) : max(base_ior, 1.0001);
        const uint current_depth = primary_payload.depth;
        float thickness_world = volume_thickness * average_world_scale();
        float transmission_offset = max(0.01, 0.01 * max(thickness_world, 1.0));
        vec3 transmission_origin = world_position - transmission_normal * transmission_offset;
        // Attenuation is computed from actual ray travel distance, not the baked thickness factor.
        // volume_thickness > 0 indicates volumetric material; the ray's hit_distance gives
        // the true world-space path length through the medium after tracing.
        bool volumetric = thickness_world > 0.001;
        uint transmission_sample_count = (is_primary_depth && roughness > 0.25) ? ROUGH_TRANSMISSION_SAMPLE_COUNT : 1u;
        float transmission_sample_rotation = hash_to_unit_float(sample_seed ^ scene_uniforms.accumulation_data.w ^ 0x4f1bbcdcu) * (2.0 * PI);
        vec3 transmitted_radiance = vec3(0.0);

        for (uint transmission_sample_index = 0u; transmission_sample_index < transmission_sample_count; ++transmission_sample_index)
        {
            vec3 refraction_direction = refract(gl_WorldRayDirectionEXT, transmission_normal, eta);
            if (dot(refraction_direction, refraction_direction) <= 0.0001)
            {
                refraction_direction = reflect(gl_WorldRayDirectionEXT, transmission_normal);
            }

            vec2 disk_sample = transmission_sample_count > 1u
                ? sample_concentric_disk(hammersley(transmission_sample_index, transmission_sample_count, sample_seed ^ 0x4f1bbcdcu))
                : sample_concentric_disk(hammersley(scene_uniforms.accumulation_data.w & 0xFFu, 256u, sample_seed ^ 0x4f1bbcdcu));
            disk_sample = rotate_disk_sample(disk_sample, transmission_sample_rotation);
            refraction_direction = sample_rough_transmission_direction(refraction_direction, roughness, disk_sample);

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
                transmission_origin,
                0.001,
                normalize(refraction_direction),
                10000.0,
                0);

            float ray_distance = primary_payload.hit_distance < 9e29 ? primary_payload.hit_distance : thickness_world;
            vec3 attenuation = (volumetric && is_primary_depth) ? evaluate_volume_attenuation(material, ray_distance) : vec3(1.0);
            transmitted_radiance += primary_payload.color.rgb * attenuation;
        }

        vec3 transmitted_color = (transmitted_radiance / float(transmission_sample_count)) * clamp(albedo.rgb, vec3(0.0), vec3(1.0));
        float transmission_weight = transmission_factor * (1.0 - clamp(max_component(view_fresnel), 0.0, 1.0));
        shaded_color = mix(shaded_color, transmitted_color, transmission_weight);
        primary_payload.depth = current_depth;
    }

    if (alpha < 0.999 && material.alpha_mode == 2u && primary_payload.depth < 2u)
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
