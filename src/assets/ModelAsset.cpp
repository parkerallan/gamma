#include "assets/ModelAsset.h"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/texture.h>

#include <stb_image.h>

#include <algorithm>
#include <cctype>

namespace
{
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

std::array<float, 4> ReadMaterialBaseColor(const aiMaterial* material)
{
    aiColor4D color = aiColor4D(1.0f, 1.0f, 1.0f, 1.0f);
    if (material != nullptr)
    {
        if (material->Get(AI_MATKEY_BASE_COLOR, color) != AI_SUCCESS)
        {
            material->Get(AI_MATKEY_COLOR_DIFFUSE, color);
        }
    }

    return {color.r, color.g, color.b, color.a};
}

void FinalizeTextureAlphaMetadata(ModelTextureAsset& texture_asset)
{
    if (!texture_asset.valid || texture_asset.pixels.empty())
    {
        texture_asset.has_transparency = false;
        texture_asset.alpha_min = 1.0f;
        texture_asset.alpha_max = 1.0f;
        return;
    }

    std::uint8_t min_alpha = 255;
    std::uint8_t max_alpha = 0;
    for (std::size_t pixel_offset = 3; pixel_offset < texture_asset.pixels.size(); pixel_offset += 4)
    {
        const std::uint8_t alpha = texture_asset.pixels[pixel_offset];
        min_alpha = (std::min)(min_alpha, alpha);
        max_alpha = (std::max)(max_alpha, alpha);
    }

    texture_asset.alpha_min = static_cast<float>(min_alpha) / 255.0f;
    texture_asset.alpha_max = static_cast<float>(max_alpha) / 255.0f;
    texture_asset.has_transparency = min_alpha < 255;
}

std::filesystem::path ResolveTexturePath(const std::filesystem::path& model_path, const std::string& texture_reference)
{
    std::filesystem::path texture_path(texture_reference);
    if (texture_path.is_absolute())
    {
        return texture_path;
    }

    return model_path.parent_path() / texture_path;
}

bool LoadTextureFromFile(const std::filesystem::path& texture_path, ModelTextureAsset& texture_asset)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(texture_path.string().c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr || width <= 0 || height <= 0)
    {
        if (pixels != nullptr)
        {
            stbi_image_free(pixels);
        }
        return false;
    }

    texture_asset.valid = true;
    texture_asset.width = width;
    texture_asset.height = height;
    texture_asset.pixels.assign(pixels, pixels + (width * height * 4));
    stbi_image_free(pixels);
    FinalizeTextureAlphaMetadata(texture_asset);
    return true;
}

bool LoadEmbeddedTexture(const aiTexture* embedded_texture, ModelTextureAsset& texture_asset)
{
    if (embedded_texture == nullptr)
    {
        return false;
    }

    if (embedded_texture->mHeight == 0)
    {
        int width = 0;
        int height = 0;
        int channels = 0;
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(embedded_texture->pcData);
        unsigned char* pixels = stbi_load_from_memory(bytes, static_cast<int>(embedded_texture->mWidth), &width, &height, &channels, 4);
        if (pixels == nullptr || width <= 0 || height <= 0)
        {
            if (pixels != nullptr)
            {
                stbi_image_free(pixels);
            }
            return false;
        }

        texture_asset.valid = true;
        texture_asset.width = width;
        texture_asset.height = height;
        texture_asset.pixels.assign(pixels, pixels + (width * height * 4));
        stbi_image_free(pixels);
        FinalizeTextureAlphaMetadata(texture_asset);
        return true;
    }

    texture_asset.valid = true;
    texture_asset.width = static_cast<int>(embedded_texture->mWidth);
    texture_asset.height = static_cast<int>(embedded_texture->mHeight);
    texture_asset.pixels.resize(static_cast<std::size_t>(texture_asset.width) * static_cast<std::size_t>(texture_asset.height) * 4u);

    for (int y = 0; y < texture_asset.height; ++y)
    {
        for (int x = 0; x < texture_asset.width; ++x)
        {
            const aiTexel& source = embedded_texture->pcData[static_cast<std::size_t>(y) * static_cast<std::size_t>(texture_asset.width) + static_cast<std::size_t>(x)];
            const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(texture_asset.width) + static_cast<std::size_t>(x)) * 4u;
            texture_asset.pixels[offset + 0] = source.r;
            texture_asset.pixels[offset + 1] = source.g;
            texture_asset.pixels[offset + 2] = source.b;
            texture_asset.pixels[offset + 3] = source.a;
        }
    }

    FinalizeTextureAlphaMetadata(texture_asset);
    return true;
}

bool LoadMaterialBaseColorTexture(
    const aiScene* scene,
    const aiMaterial* material,
    const std::filesystem::path& model_path,
    ModelMaterialAsset& material_asset)
{
    if (material == nullptr)
    {
        return false;
    }

    const aiTextureType texture_types[] = {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE};
    for (aiTextureType texture_type : texture_types)
    {
        if (material->GetTextureCount(texture_type) == 0)
        {
            continue;
        }

        aiString texture_path_string;
        if (material->GetTexture(texture_type, 0, &texture_path_string) != AI_SUCCESS)
        {
            continue;
        }

        const std::string texture_reference = ToDisplayString(texture_path_string);
        if (texture_reference.empty())
        {
            continue;
        }

        material_asset.base_color_texture_source = texture_reference;
        const aiTexture* embedded_texture = scene != nullptr ? scene->GetEmbeddedTexture(texture_reference.c_str()) : nullptr;
        if (embedded_texture != nullptr)
        {
            return LoadEmbeddedTexture(embedded_texture, material_asset.base_color_texture);
        }

        const std::filesystem::path resolved_path = ResolveTexturePath(model_path, texture_reference);
        material_asset.base_color_texture_source = resolved_path.string();
        return LoadTextureFromFile(resolved_path, material_asset.base_color_texture);
    }

    return false;
}

void ExpandBounds(ModelBounds& bounds, const std::array<float, 3>& point)
{
    if (!bounds.valid)
    {
        bounds.valid = true;
        bounds.minimum = point;
        bounds.maximum = point;
        return;
    }

    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        bounds.minimum[axis] = (std::min)(bounds.minimum[axis], point[axis]);
        bounds.maximum[axis] = (std::max)(bounds.maximum[axis], point[axis]);
    }
}
}

bool ModelAsset::IsSupportedPath(const std::filesystem::path& path)
{
    const std::string extension = ToLowerExtension(path);
    return extension == ".fbx" || extension == ".glb";
}

ModelAsset LoadModelAsset(const std::filesystem::path& path)
{
    ModelAsset asset;
    if (!ModelAsset::IsSupportedPath(path))
    {
        asset.error_message = "Unsupported model format.";
        return asset;
    }

    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(
        path.string(),
        aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_ImproveCacheLocality |
            aiProcess_GenSmoothNormals |
            aiProcess_ValidateDataStructure |
            aiProcess_SortByPType);

    if (scene == nullptr)
    {
        asset.error_message = importer.GetErrorString();
        if (asset.error_message.empty())
        {
            asset.error_message = "Assimp failed to load the model asset.";
        }
        return asset;
    }

    asset.materials.reserve(scene->mNumMaterials);
    for (unsigned int material_index = 0; material_index < scene->mNumMaterials; ++material_index)
    {
        const aiMaterial* source_material = scene->mMaterials[material_index];
        ModelMaterialAsset material_asset;

        aiString material_name;
        if (source_material != nullptr && source_material->Get(AI_MATKEY_NAME, material_name) == AI_SUCCESS)
        {
            material_asset.name = ToDisplayString(material_name);
        }
        if (material_asset.name.empty())
        {
            material_asset.name = "Material " + std::to_string(material_index + 1);
        }

        material_asset.base_color = ReadMaterialBaseColor(source_material);
        LoadMaterialBaseColorTexture(scene, source_material, path, material_asset);
        material_asset.uses_alpha_transparency =
            material_asset.base_color[3] < 0.999f ||
            material_asset.base_color_texture.has_transparency;
        asset.materials.push_back(std::move(material_asset));
    }

    asset.meshes.reserve(scene->mNumMeshes);
    for (unsigned int mesh_index = 0; mesh_index < scene->mNumMeshes; ++mesh_index)
    {
        const aiMesh* source_mesh = scene->mMeshes[mesh_index];
        if (source_mesh == nullptr)
        {
            continue;
        }

        ModelMeshAsset mesh;
        mesh.name = ToDisplayString(source_mesh->mName);
        if (mesh.name.empty())
        {
            mesh.name = "Mesh " + std::to_string(mesh_index + 1);
        }
        mesh.material_index = source_mesh->mMaterialIndex;
        mesh.vertices.reserve(source_mesh->mNumVertices);

        for (unsigned int vertex_index = 0; vertex_index < source_mesh->mNumVertices; ++vertex_index)
        {
            ModelVertex vertex;
            const aiVector3D& position = source_mesh->mVertices[vertex_index];
            vertex.position = {position.x, position.y, position.z};

            if (source_mesh->HasNormals())
            {
                const aiVector3D& normal = source_mesh->mNormals[vertex_index];
                vertex.normal = {normal.x, normal.y, normal.z};
            }

            if (source_mesh->HasTextureCoords(0))
            {
                const aiVector3D& uv = source_mesh->mTextureCoords[0][vertex_index];
                vertex.uv0 = {uv.x, uv.y};
            }

            ExpandBounds(mesh.bounds, vertex.position);
            ExpandBounds(asset.bounds, vertex.position);
            mesh.vertices.push_back(vertex);
        }

        mesh.indices.reserve(source_mesh->mNumFaces * 3);
        for (unsigned int face_index = 0; face_index < source_mesh->mNumFaces; ++face_index)
        {
            const aiFace& face = source_mesh->mFaces[face_index];
            for (unsigned int index = 0; index < face.mNumIndices; ++index)
            {
                mesh.indices.push_back(face.mIndices[index]);
            }
        }

        asset.meshes.push_back(std::move(mesh));
    }

    asset.loaded = true;
    return asset;
}