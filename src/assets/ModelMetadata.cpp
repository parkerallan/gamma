#include "assets/ModelMetadata.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cctype>

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

bool ReadFloat(const aiMaterial* material, const char* pKey, unsigned int type, unsigned int index, float& value)
{
    if (material == nullptr)
    {
        return false;
    }

    return material->Get(pKey, type, index, value) == AI_SUCCESS;
}

const std::array<std::pair<aiTextureType, const char*>, 8> kTextureSlots = {{
    {aiTextureType_BASE_COLOR, "Base Color"},
    {aiTextureType_DIFFUSE, "Diffuse"},
    {aiTextureType_NORMALS, "Normal"},
    {aiTextureType_METALNESS, "Metalness"},
    {aiTextureType_DIFFUSE_ROUGHNESS, "Roughness"},
    {aiTextureType_EMISSIVE, "Emissive"},
    {aiTextureType_AMBIENT_OCCLUSION, "Occlusion"},
    {aiTextureType_OPACITY, "Opacity"},
}};
}

bool ModelMetadata::IsSupportedModelPath(const std::filesystem::path& path)
{
    const std::string extension = ToLowerExtension(path);
    return extension == ".fbx" || extension == ".glb";
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
    const aiScene* scene = importer.ReadFile(
        path.string(),
        aiProcess_JoinIdenticalVertices |
            aiProcess_Triangulate |
            aiProcess_SortByPType |
            aiProcess_ValidateDataStructure);

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

    metadata.materials.reserve(scene->mNumMaterials);
    for (unsigned int material_index = 0; material_index < scene->mNumMaterials; ++material_index)
    {
        const aiMaterial* material = scene->mMaterials[material_index];
        ModelMaterialMetadata material_metadata;
        material_metadata.name = NormalizeMaterialName(material, material_index);
        material_metadata.base_color = ReadColor(material, AI_MATKEY_BASE_COLOR);
        if (!material_metadata.base_color.valid)
        {
            material_metadata.base_color = ReadColor(material, AI_MATKEY_COLOR_DIFFUSE);
        }
        material_metadata.emissive_color = ReadColor(material, AI_MATKEY_COLOR_EMISSIVE);
        material_metadata.has_opacity = ReadFloat(material, AI_MATKEY_OPACITY, material_metadata.opacity);
        material_metadata.has_roughness = ReadFloat(material, AI_MATKEY_ROUGHNESS_FACTOR, material_metadata.roughness);
        material_metadata.has_metalness = ReadFloat(material, AI_MATKEY_METALLIC_FACTOR, material_metadata.metalness);

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