#include "assets/ModelMetadata.h"

#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace
{
unsigned int CountNodes(const aiNode* node)
{
    if (node == nullptr)
    {
        return 0;
    }

    unsigned int count = 1;
    for (unsigned int index = 0; index < node->mNumChildren; ++index)
    {
        count += CountNodes(node->mChildren[index]);
    }

    return count;
}

std::string ToLowerExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return extension;
}

std::string ToDisplayString(const aiString& value)
{
    return value.length > 0 ? std::string(value.C_Str()) : std::string();
}

std::string ToUpper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

std::string NormalizeMaterialName(const aiMaterial* material, unsigned int index)
{
    aiString material_name;
    if (material != nullptr && material->Get(AI_MATKEY_NAME, material_name) == AI_SUCCESS)
    {
        const std::string name = ToDisplayString(material_name);
        if (!name.empty())
        {
            return name;
        }
    }

    return "Material " + std::to_string(index + 1);
}

ModelColorMetadata ReadColor(const aiMaterial* material, const char* pKey, unsigned int type, unsigned int index)
{
    ModelColorMetadata color_metadata;
    if (material == nullptr)
    {
        return color_metadata;
    }

    aiColor4D color_value;
    if (material->Get(pKey, type, index, color_value) == AI_SUCCESS)
    {
        color_metadata.valid = true;
        color_metadata.rgba = {
            color_value.r,
            color_value.g,
            color_value.b,
            color_value.a,
        };
    }

    return color_metadata;
}

ModelColorMetadata ReadColor3(const aiMaterial* material, const char* pKey, unsigned int type, unsigned int index)
{
    ModelColorMetadata color_metadata;
    if (material == nullptr)
    {
        return color_metadata;
    }

    aiColor3D color_value;
    if (material->Get(pKey, type, index, color_value) == AI_SUCCESS)
    {
        color_metadata.valid = true;
        color_metadata.rgba = {color_value.r, color_value.g, color_value.b, 1.0f};
    }

    return color_metadata;
}

bool ReadFloat(const aiMaterial* material, const char* pKey, unsigned int type, unsigned int index, float& value)
{
    if (material == nullptr)
    {
        return false;
    }

    return material->Get(pKey, type, index, value) == AI_SUCCESS;
}

bool ReadBool(const aiMaterial* material, const char* pKey, unsigned int type, unsigned int index, bool& value)
{
    if (material == nullptr)
    {
        return false;
    }

    int raw_value = value ? 1 : 0;
    if (material->Get(pKey, type, index, raw_value) != AI_SUCCESS)
    {
        return false;
    }

    value = raw_value != 0;
    return true;
}

bool ReadString(const aiMaterial* material, const char* pKey, unsigned int type, unsigned int index, std::string& value)
{
    if (material == nullptr)
    {
        return false;
    }

    aiString string_value;
    if (material->Get(pKey, type, index, string_value) != AI_SUCCESS)
    {
        return false;
    }

    value = ToDisplayString(string_value);
    return !value.empty();
}

const std::array<std::pair<aiTextureType, const char*>, 9> kTextureSlots = {{
    {aiTextureType_BASE_COLOR, "Base Color"},
    {aiTextureType_DIFFUSE, "Diffuse"},
    {aiTextureType_UNKNOWN, "Metallic-Roughness"},
    {aiTextureType_NORMALS, "Normal"},
    {aiTextureType_METALNESS, "Metalness"},
    {aiTextureType_DIFFUSE_ROUGHNESS, "Roughness"},
    {aiTextureType_EMISSIVE, "Emissive"},
    {aiTextureType_AMBIENT_OCCLUSION, "Occlusion"},
    {aiTextureType_OPACITY, "Opacity"},
}};

struct GltfExtensionMetadata
{
    bool has_iridescence = false;
    float iridescence_factor = 0.0f;
    float iridescence_ior = 1.3f;
    float iridescence_thickness_min = 100.0f;
    float iridescence_thickness_max = 400.0f;
    bool has_ior = false;
    float index_of_refraction = 1.5f;
    bool has_transmission = false;
    float transmission_factor = 0.0f;
    bool has_volume = false;
    float volume_thickness_factor = 0.0f;
    float attenuation_distance = 0.0f;
    std::array<float, 3> attenuation_color = {1.0f, 1.0f, 1.0f};
    bool has_clearcoat = false;
    float clearcoat_factor = 0.0f;
    float clearcoat_roughness_factor = 0.0f;
    float clearcoat_normal_scale = 1.0f;
    bool has_emissive_strength = false;
    float emissive_strength = 1.0f;
};

float ReadJsonFloat(const json& object, const char* key, float fallback)
{
    if (!object.is_object())
    {
        return fallback;
    }
    const auto it = object.find(key);
    return (it != object.end() && it->is_number()) ? it->get<float>() : fallback;
}

std::array<float, 3> ReadJsonFloat3(const json& object, const char* key, const std::array<float, 3>& fallback)
{
    if (!object.is_object())
    {
        return fallback;
    }
    const auto it = object.find(key);
    if (it == object.end() || !it->is_array() || it->size() < 3)
    {
        return fallback;
    }
    std::array<float, 3> value = fallback;
    for (std::size_t i = 0; i < 3; ++i)
    {
        if ((*it)[i].is_number())
        {
            value[i] = (*it)[i].get<float>();
        }
    }
    return value;
}

// Parses KHR extension scalars from a .gltf JSON file. Returns one entry per material index.
std::vector<GltfExtensionMetadata> LoadGltfExtensionMetadata(const std::filesystem::path& path)
{
    if (ToLowerExtension(path) != ".gltf")
    {
        return {};
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }

    std::ostringstream stream;
    stream << input.rdbuf();
    const std::string contents = stream.str();
    if (contents.empty())
    {
        return {};
    }

    json root = json::parse(contents, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        return {};
    }

    const auto materials_it = root.find("materials");
    if (materials_it == root.end() || !materials_it->is_array())
    {
        return {};
    }

    std::vector<GltfExtensionMetadata> result(materials_it->size());
    for (std::size_t mat_index = 0; mat_index < materials_it->size(); ++mat_index)
    {
        const json& mat = (*materials_it)[mat_index];
        if (!mat.is_object())
        {
            continue;
        }

        const auto ext_it = mat.find("extensions");
        if (ext_it == mat.end() || !ext_it->is_object())
        {
            continue;
        }

        GltfExtensionMetadata& ext = result[mat_index];

        if (const auto it = ext_it->find("KHR_materials_iridescence"); it != ext_it->end() && it->is_object())
        {
            ext.has_iridescence = true;
            ext.iridescence_factor = ReadJsonFloat(*it, "iridescenceFactor", 0.0f);
            ext.iridescence_ior = ReadJsonFloat(*it, "iridescenceIor", 1.3f);
            ext.iridescence_thickness_min = ReadJsonFloat(*it, "iridescenceThicknessMinimum", 100.0f);
            ext.iridescence_thickness_max = ReadJsonFloat(*it, "iridescenceThicknessMaximum", 400.0f);
        }

        if (const auto it = ext_it->find("KHR_materials_ior"); it != ext_it->end() && it->is_object())
        {
            ext.has_ior = true;
            ext.index_of_refraction = ReadJsonFloat(*it, "ior", 1.5f);
        }

        if (const auto it = ext_it->find("KHR_materials_transmission"); it != ext_it->end() && it->is_object())
        {
            ext.has_transmission = true;
            ext.transmission_factor = ReadJsonFloat(*it, "transmissionFactor", 0.0f);
        }

        if (const auto it = ext_it->find("KHR_materials_volume"); it != ext_it->end() && it->is_object())
        {
            ext.has_volume = true;
            ext.volume_thickness_factor = ReadJsonFloat(*it, "thicknessFactor", 0.0f);
            ext.attenuation_distance = ReadJsonFloat(*it, "attenuationDistance", 0.0f);
            ext.attenuation_color = ReadJsonFloat3(*it, "attenuationColor", {1.0f, 1.0f, 1.0f});
        }

        if (const auto it = ext_it->find("KHR_materials_clearcoat"); it != ext_it->end() && it->is_object())
        {
            ext.has_clearcoat = true;
            ext.clearcoat_factor = ReadJsonFloat(*it, "clearcoatFactor", 0.0f);
            ext.clearcoat_roughness_factor = ReadJsonFloat(*it, "clearcoatRoughnessFactor", 0.0f);
            const json normal_tex = it->value("clearcoatNormalTexture", json::object());
            ext.clearcoat_normal_scale = ReadJsonFloat(normal_tex, "scale", 1.0f);
        }

        if (const auto it = ext_it->find("KHR_materials_emissive_strength"); it != ext_it->end() && it->is_object())
        {
            ext.has_emissive_strength = true;
            ext.emissive_strength = ReadJsonFloat(*it, "emissiveStrength", 1.0f);
        }
    }

    return result;
}
}

bool ModelMetadata::IsSupportedModelPath(const std::filesystem::path& path)
{
    const std::string extension = ToLowerExtension(path);
    return extension == ".fbx" || extension == ".glb" || extension == ".gltf";
}

ModelMetadata LoadModelMetadata(const std::filesystem::path& path)
{
    ModelMetadata metadata;
    metadata.supported = ModelMetadata::IsSupportedModelPath(path);
    if (!metadata.supported)
    {
        metadata.error_message = "Unsupported model format.";
        return metadata;
    }

    Assimp::Importer importer;
    // aiProcess_ValidateDataStructure is intentionally omitted to match ModelAsset.cpp;
    // Assimp's validator false-positives on KHR_materials_specular (and a few other PBR
    // extensions) and would otherwise reject valid glTF 2.0 assets here as well.
    const aiScene* scene = importer.ReadFile(
        path.string(),
        aiProcess_JoinIdenticalVertices |
            aiProcess_Triangulate |
            aiProcess_SortByPType);

    if (scene == nullptr)
    {
        metadata.error_message = importer.GetErrorString();
        if (metadata.error_message.empty())
        {
            metadata.error_message = "Assimp failed to load the model.";
        }
        return metadata;
    }

    metadata.parsed = true;
    metadata.node_count = CountNodes(scene->mRootNode);
    metadata.mesh_count = scene->mNumMeshes;
    metadata.material_count = scene->mNumMaterials;
    metadata.animation_count = scene->mNumAnimations;
    metadata.embedded_texture_count = scene->mNumTextures;

    const std::vector<GltfExtensionMetadata> extensions = LoadGltfExtensionMetadata(path);

    // Collect material indices actually referenced by meshes to skip unused defaults
    std::vector<bool> material_used(scene->mNumMaterials, false);
    for (unsigned int mesh_index = 0; mesh_index < scene->mNumMeshes; ++mesh_index)
    {
        const aiMesh* mesh = scene->mMeshes[mesh_index];
        if (mesh != nullptr && mesh->mMaterialIndex < scene->mNumMaterials)
        {
            material_used[mesh->mMaterialIndex] = true;
        }
    }

    metadata.materials.reserve(scene->mNumMaterials);
    for (unsigned int material_index = 0; material_index < scene->mNumMaterials; ++material_index)
    {
        if (!material_used[material_index])
        {
            continue;
        }

        const aiMaterial* material = scene->mMaterials[material_index];
        ModelMaterialMetadata material_metadata;
        material_metadata.name = NormalizeMaterialName(material, material_index);
        material_metadata.base_color = ReadColor(material, AI_MATKEY_BASE_COLOR);
        if (!material_metadata.base_color.valid)
        {
            material_metadata.base_color = ReadColor(material, AI_MATKEY_COLOR_DIFFUSE);
        }
        material_metadata.emissive_color = ReadColor(material, AI_MATKEY_COLOR_EMISSIVE);
        material_metadata.specular_color = ReadColor3(material, AI_MATKEY_COLOR_SPECULAR);
        material_metadata.sheen_color = ReadColor3(material, AI_MATKEY_SHEEN_COLOR_FACTOR);
        ReadString(material, AI_MATKEY_GLTF_ALPHAMODE, material_metadata.alpha_mode);
        material_metadata.alpha_mode = ToUpper(material_metadata.alpha_mode);
        material_metadata.has_alpha_cutoff = ReadFloat(material, AI_MATKEY_GLTF_ALPHACUTOFF, material_metadata.alpha_cutoff);
        ReadBool(material, AI_MATKEY_TWOSIDED, material_metadata.double_sided);
        material_metadata.has_opacity = ReadFloat(material, AI_MATKEY_OPACITY, material_metadata.opacity);
        material_metadata.has_normal_scale = material != nullptr && material->Get(AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0), material_metadata.normal_scale) == AI_SUCCESS;
        material_metadata.has_occlusion_strength = material != nullptr && material->Get(AI_MATKEY_GLTF_TEXTURE_STRENGTH(aiTextureType_AMBIENT_OCCLUSION, 0), material_metadata.occlusion_strength) == AI_SUCCESS;
        material_metadata.has_roughness = ReadFloat(material, AI_MATKEY_ROUGHNESS_FACTOR, material_metadata.roughness);
        material_metadata.has_metalness = ReadFloat(material, AI_MATKEY_METALLIC_FACTOR, material_metadata.metalness);
        material_metadata.has_specular_factor = ReadFloat(material, AI_MATKEY_SPECULAR_FACTOR, material_metadata.specular_factor);
        material_metadata.has_sheen_roughness = ReadFloat(material, AI_MATKEY_SHEEN_ROUGHNESS_FACTOR, material_metadata.sheen_roughness_factor);

        int shading_model = 0;
        material_metadata.unlit = material != nullptr &&
            material->Get(AI_MATKEY_SHADING_MODEL, shading_model) == AI_SUCCESS &&
            (shading_model == aiShadingMode_NoShading || shading_model == aiShadingMode_Unlit);

        // Apply KHR extension data from glTF JSON (only available for .gltf, not .glb)
        if (material_index < extensions.size())
        {
            const GltfExtensionMetadata& ext = extensions[material_index];

            if (ext.has_ior)
            {
                material_metadata.has_ior = true;
                material_metadata.index_of_refraction = ext.index_of_refraction;
            }
            if (ext.has_transmission)
            {
                material_metadata.has_transmission = true;
                material_metadata.transmission_factor = ext.transmission_factor;
            }
            if (ext.has_iridescence)
            {
                material_metadata.has_iridescence = true;
                material_metadata.iridescence_factor = ext.iridescence_factor;
                material_metadata.iridescence_ior = ext.iridescence_ior;
                material_metadata.iridescence_thickness_min = ext.iridescence_thickness_min;
                material_metadata.iridescence_thickness_max = ext.iridescence_thickness_max;
            }
            if (ext.has_volume)
            {
                material_metadata.has_volume = true;
                material_metadata.volume_thickness_factor = ext.volume_thickness_factor;
                material_metadata.attenuation_distance = ext.attenuation_distance;
                material_metadata.attenuation_color.valid = true;
                material_metadata.attenuation_color.rgba = {
                    ext.attenuation_color[0],
                    ext.attenuation_color[1],
                    ext.attenuation_color[2],
                    1.0f,
                };
            }
            if (ext.has_clearcoat)
            {
                material_metadata.has_clearcoat = true;
                material_metadata.clearcoat_factor = ext.clearcoat_factor;
                material_metadata.clearcoat_roughness_factor = ext.clearcoat_roughness_factor;
                material_metadata.clearcoat_normal_scale = ext.clearcoat_normal_scale;
            }
            if (ext.has_emissive_strength)
            {
                material_metadata.has_emissive_strength = true;
                material_metadata.emissive_strength = ext.emissive_strength;
            }
        }

        for (const auto& slot : kTextureSlots)
        {
            const unsigned int texture_count = material != nullptr ? material->GetTextureCount(slot.first) : 0;
            for (unsigned int texture_index = 0; texture_index < texture_count; ++texture_index)
            {
                aiString texture_path;
                if (material->GetTexture(slot.first, texture_index, &texture_path) != AI_SUCCESS)
                {
                    continue;
                }

                ModelTextureReference texture_reference;
                texture_reference.slot_label = slot.second;
                texture_reference.path = ToDisplayString(texture_path);
                if (texture_reference.path.empty())
                {
                    texture_reference.path = "<embedded>";
                }
                material_metadata.textures.push_back(std::move(texture_reference));
            }
        }

        metadata.materials.push_back(std::move(material_metadata));
    }

    metadata.animations.reserve(scene->mNumAnimations);
    for (unsigned int animation_index = 0; animation_index < scene->mNumAnimations; ++animation_index)
    {
        const aiAnimation* animation = scene->mAnimations[animation_index];
        if (animation == nullptr)
        {
            continue;
        }

        ModelAnimationMetadata animation_metadata;
        animation_metadata.name = ToDisplayString(animation->mName);
        if (animation_metadata.name.empty())
        {
            animation_metadata.name = "Animation " + std::to_string(animation_index + 1);
        }
        animation_metadata.duration = animation->mDuration;
        animation_metadata.ticks_per_second = animation->mTicksPerSecond;
        animation_metadata.channel_count = animation->mNumChannels;
        metadata.animations.push_back(std::move(animation_metadata));
    }

    return metadata;
}