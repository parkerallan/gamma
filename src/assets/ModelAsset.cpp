#include "assets/ModelAsset.h"
#include "vfs/AssetVFS.h"

#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/texture.h>

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <initializer_list>

namespace
{
bool LoadTextureFromFile(const std::filesystem::path& texture_path, ModelTextureAsset& texture_asset);
bool LoadEmbeddedTexture(const aiTexture* embedded_texture, ModelTextureAsset& texture_asset);

std::string ToLowerExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return extension;
}

std::string ToUpper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::toupper(character));
    });
    return value;
}

std::string ToDisplayString(const aiString& value)
{
    return value.length > 0 ? std::string(value.C_Str()) : std::string();
}

std::string NormalizeAssimpPath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');

    while (path.rfind("./", 0) == 0)
    {
        path.erase(0, 2);
    }

    if (path.size() > 3 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':' && path[2] == '/')
    {
        path.erase(0, 3);
    }

    if (!path.empty() && path[0] == '/')
    {
        path.erase(0, 1);
    }

    const std::string lowered = ToUpper(path);
    const std::string marker = "CONTENT/";
    if (lowered.rfind(marker, 0) == 0)
    {
        path = path.substr(marker.size());
    }
    else
    {
        const std::string slash_marker = "/CONTENT/";
        const std::size_t marker_pos = lowered.find(slash_marker);
        if (marker_pos != std::string::npos)
        {
            path = path.substr(marker_pos + slash_marker.size());
        }
    }

    return path;
}

class PakMemoryIOStream : public Assimp::IOStream
{
public:
    explicit PakMemoryIOStream(std::vector<std::uint8_t> bytes)
        : bytes_(std::move(bytes))
    {
    }

    ~PakMemoryIOStream() override = default;

    size_t Read(void* pvBuffer, size_t pSize, size_t pCount) override
    {
        if (pSize == 0 || pCount == 0 || cursor_ >= bytes_.size())
        {
            return 0;
        }

        const size_t requested = pSize * pCount;
        const size_t available = bytes_.size() - cursor_;
        const size_t to_copy = (std::min)(requested, available);
        std::memcpy(pvBuffer, bytes_.data() + cursor_, to_copy);
        cursor_ += to_copy;
        return to_copy / pSize;
    }

    size_t Write(const void*, size_t, size_t) override
    {
        return 0;
    }

    aiReturn Seek(size_t pOffset, aiOrigin pOrigin) override
    {
        size_t new_cursor = cursor_;
        if (pOrigin == aiOrigin_SET)
        {
            new_cursor = pOffset;
        }
        else if (pOrigin == aiOrigin_CUR)
        {
            new_cursor = cursor_ + pOffset;
        }
        else if (pOrigin == aiOrigin_END)
        {
            if (pOffset > bytes_.size())
            {
                return aiReturn_FAILURE;
            }
            new_cursor = bytes_.size() - pOffset;
        }

        if (new_cursor > bytes_.size())
        {
            return aiReturn_FAILURE;
        }

        cursor_ = new_cursor;
        return aiReturn_SUCCESS;
    }

    size_t Tell() const override
    {
        return cursor_;
    }

    size_t FileSize() const override
    {
        return bytes_.size();
    }

    void Flush() override {}

private:
    std::vector<std::uint8_t> bytes_;
    size_t cursor_ = 0;
};

class PakAssetIOSystem : public Assimp::IOSystem
{
public:
    ~PakAssetIOSystem() override = default;

    bool Exists(const char* pFile) const override
    {
        if (!g_asset_reader || pFile == nullptr)
        {
            return false;
        }

        return g_asset_reader->FileExists(ResolvePath(pFile));
    }

    char getOsSeparator() const override
    {
        return '/';
    }

    Assimp::IOStream* Open(const char* pFile, const char* pMode = "rb") override
    {
        if (!g_asset_reader || pFile == nullptr)
        {
            return nullptr;
        }

        if (pMode != nullptr && pMode[0] != 'r')
        {
            return nullptr;
        }

        const std::string resolved = ResolvePath(pFile);
        auto bytes = g_asset_reader->ReadFile(resolved);
        if (bytes.empty())
        {
            return nullptr;
        }

        return new PakMemoryIOStream(std::move(bytes));
    }

    void Close(Assimp::IOStream* pFile) override
    {
        delete pFile;
    }

private:
    static std::string ResolvePath(const std::string& raw_path)
    {
        const std::string normalized = NormalizeAssimpPath(raw_path);
        if (!cwd_.empty())
        {
            const std::string combined = NormalizeAssimpPath(cwd_ + "/" + normalized);
            if (g_asset_reader && g_asset_reader->FileExists(combined))
            {
                return combined;
            }
        }

        std::filesystem::path path_obj(normalized);
        cwd_ = NormalizeAssimpPath(path_obj.parent_path().generic_string());
        return normalized;
    }

    static std::string cwd_;
};

std::string PakAssetIOSystem::cwd_;

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

std::array<float, 3> ReadMaterialEmissiveColor(const aiMaterial* material)
{
    aiColor3D color = aiColor3D(0.0f, 0.0f, 0.0f);
    if (material != nullptr)
    {
        material->Get(AI_MATKEY_COLOR_EMISSIVE, color);
    }

    return {color.r, color.g, color.b};
}

float ReadMaterialFloatProperty(
    const aiMaterial* material,
    const char* key,
    unsigned int type,
    unsigned int index,
    float fallback_value)
{
    if (material == nullptr)
    {
        return fallback_value;
    }

    float value = fallback_value;
    return material->Get(key, type, index, value) == AI_SUCCESS ? value : fallback_value;
}

float ReadMaterialOpacityFactor(const aiMaterial* material)
{
    return ReadMaterialFloatProperty(material, AI_MATKEY_OPACITY, 1.0f);
}

float ReadMaterialMetallicFactor(const aiMaterial* material)
{
    return ReadMaterialFloatProperty(material, AI_MATKEY_METALLIC_FACTOR, 1.0f);
}

float ReadMaterialRoughnessFactor(const aiMaterial* material)
{
    return ReadMaterialFloatProperty(material, AI_MATKEY_ROUGHNESS_FACTOR, 1.0f);
}

float ReadMaterialNormalScale(const aiMaterial* material)
{
    if (material == nullptr)
    {
        return 1.0f;
    }

    float scale = 1.0f;
    return material->Get(AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0), scale) == AI_SUCCESS ? scale : 1.0f;
}

float ReadMaterialOcclusionStrength(const aiMaterial* material)
{
    if (material == nullptr)
    {
        return 1.0f;
    }

    float strength = 1.0f;
    return material->Get(AI_MATKEY_GLTF_TEXTURE_STRENGTH(aiTextureType_AMBIENT_OCCLUSION, 0), strength) == AI_SUCCESS ? strength : 1.0f;
}

bool ReadMaterialBooleanProperty(
    const aiMaterial* material,
    const char* key,
    unsigned int type,
    unsigned int index,
    bool fallback_value)
{
    if (material == nullptr)
    {
        return fallback_value;
    }

    int value = fallback_value ? 1 : 0;
    return material->Get(key, type, index, value) == AI_SUCCESS ? value != 0 : fallback_value;
}

bool ReadMaterialDoubleSided(const aiMaterial* material)
{
    return ReadMaterialBooleanProperty(material, AI_MATKEY_TWOSIDED, false);
}

bool ReadMaterialUnlit(const aiMaterial* material)
{
    if (material == nullptr)
    {
        return false;
    }

    int shading_model = 0;
    return material->Get(AI_MATKEY_SHADING_MODEL, shading_model) == AI_SUCCESS &&
        (shading_model == aiShadingMode_NoShading || shading_model == aiShadingMode_Unlit);
}

ModelAlphaMode ParseMaterialAlphaMode(std::string value)
{
    value = ToUpper(std::move(value));
    if (value == "MASK")
    {
        return ModelAlphaMode::Mask;
    }
    if (value == "BLEND")
    {
        return ModelAlphaMode::Blend;
    }
    return ModelAlphaMode::Opaque;
}

ModelAlphaMode ReadMaterialAlphaMode(const aiMaterial* material)
{
    if (material == nullptr)
    {
        return ModelAlphaMode::Opaque;
    }

    aiString alpha_mode;
    if (material->Get(AI_MATKEY_GLTF_ALPHAMODE, alpha_mode) == AI_SUCCESS)
    {
        return ParseMaterialAlphaMode(ToDisplayString(alpha_mode));
    }

    return ModelAlphaMode::Opaque;
}

float ReadMaterialAlphaCutoff(const aiMaterial* material)
{
    if (material == nullptr)
    {
        return 0.5f;
    }

    float value = 0.5f;
    return material->Get(AI_MATKEY_GLTF_ALPHACUTOFF, value) == AI_SUCCESS ? value : 0.5f;
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

bool LoadTextureReference(
    const aiScene* scene,
    const std::filesystem::path& model_path,
    const std::string& texture_reference,
    bool srgb,
    std::string& texture_source,
    ModelTextureAsset& texture_asset)
{
    if (texture_reference.empty())
    {
        return false;
    }

    texture_source = texture_reference;
    texture_asset = {};
    texture_asset.srgb = srgb;

    const aiTexture* embedded_texture = scene != nullptr ? scene->GetEmbeddedTexture(texture_reference.c_str()) : nullptr;
    if (embedded_texture != nullptr)
    {
        return LoadEmbeddedTexture(embedded_texture, texture_asset);
    }

    const std::filesystem::path resolved_path = ResolveTexturePath(model_path, texture_reference);
    texture_source = resolved_path.string();
    return LoadTextureFromFile(resolved_path, texture_asset);
}

bool LoadTextureFromFile(const std::filesystem::path& texture_path, ModelTextureAsset& texture_asset)
{
    if (g_asset_reader)
    {
        const auto bytes = g_asset_reader->ReadFile(texture_path.generic_string());
        if (!bytes.empty())
        {
            int width = 0;
            int height = 0;
            int channels = 0;
            unsigned char* pixels = stbi_load_from_memory(
                bytes.data(),
                static_cast<int>(bytes.size()),
                &width,
                &height,
                &channels,
                4);
            if (pixels != nullptr && width > 0 && height > 0)
            {
                texture_asset.valid = true;
                texture_asset.width = width;
                texture_asset.height = height;
                texture_asset.pixels.assign(pixels, pixels + (width * height * 4));
                stbi_image_free(pixels);
                FinalizeTextureAlphaMetadata(texture_asset);
                return true;
            }

            if (pixels != nullptr)
            {
                stbi_image_free(pixels);
            }
        }
    }

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

bool LoadMaterialTexture(
    const aiScene* scene,
    const aiMaterial* material,
    const std::filesystem::path& model_path,
    std::initializer_list<aiTextureType> texture_types,
    bool srgb,
    std::string& texture_source,
    ModelTextureAsset& texture_asset)
{
    if (material == nullptr)
    {
        return false;
    }

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

        return LoadTextureReference(scene, model_path, texture_reference, srgb, texture_source, texture_asset);
    }

    return false;
}

bool DetermineMaterialTransparency(const ModelMaterialAsset& material_asset)
{
    if (material_asset.alpha_mode != ModelAlphaMode::Opaque)
    {
        return true;
    }

    return material_asset.base_color[3] < 0.999f ||
        material_asset.opacity_factor < 0.999f ||
        material_asset.base_color_texture.has_transparency;
}

float Dot(const std::array<float, 3>& left, const std::array<float, 3>& right)
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

std::array<float, 3> Cross(const std::array<float, 3>& left, const std::array<float, 3>& right)
{
    return {
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    };
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
    return extension == ".fbx" || extension == ".glb" || extension == ".gltf";
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
    const unsigned int import_flags =
        aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_ImproveCacheLocality |
        aiProcess_CalcTangentSpace |
        aiProcess_GenSmoothNormals |
        aiProcess_ValidateDataStructure |
        aiProcess_SortByPType;

    const bool use_pak = g_asset_reader != nullptr;
    const std::string model_path = use_pak ? NormalizeAssimpPath(path.generic_string()) : path.generic_string();
    if (use_pak)
    {
        importer.SetIOHandler(new PakAssetIOSystem());
    }

    const aiScene* scene = importer.ReadFile(model_path, import_flags);

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
        material_asset.emissive_color = ReadMaterialEmissiveColor(source_material);
        material_asset.opacity_factor = ReadMaterialOpacityFactor(source_material);
        material_asset.metallic_factor = ReadMaterialMetallicFactor(source_material);
        material_asset.roughness_factor = ReadMaterialRoughnessFactor(source_material);
        material_asset.normal_scale = ReadMaterialNormalScale(source_material);
        material_asset.occlusion_strength = ReadMaterialOcclusionStrength(source_material);
        material_asset.alpha_mode = ReadMaterialAlphaMode(source_material);
        material_asset.alpha_cutoff = ReadMaterialAlphaCutoff(source_material);
        material_asset.double_sided = ReadMaterialDoubleSided(source_material);
        material_asset.unlit = ReadMaterialUnlit(source_material);

        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE},
            true,
            material_asset.base_color_texture_source,
            material_asset.base_color_texture);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_UNKNOWN},
            false,
            material_asset.metallic_roughness_texture_source,
            material_asset.metallic_roughness_texture);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_METALNESS},
            false,
            material_asset.metallic_texture_source,
            material_asset.metallic_texture);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_DIFFUSE_ROUGHNESS},
            false,
            material_asset.roughness_texture_source,
            material_asset.roughness_texture);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA},
            false,
            material_asset.normal_texture_source,
            material_asset.normal_texture);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_AMBIENT_OCCLUSION},
            false,
            material_asset.occlusion_texture_source,
            material_asset.occlusion_texture);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR},
            true,
            material_asset.emissive_texture_source,
            material_asset.emissive_texture);

        material_asset.base_color[3] = std::clamp(material_asset.base_color[3] * material_asset.opacity_factor, 0.0f, 1.0f);
        material_asset.uses_alpha_transparency = DetermineMaterialTransparency(material_asset);
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

            if (source_mesh->HasTangentsAndBitangents())
            {
                const aiVector3D& tangent = source_mesh->mTangents[vertex_index];
                vertex.tangent = {tangent.x, tangent.y, tangent.z, 1.0f};

                if (source_mesh->mBitangents != nullptr && source_mesh->HasNormals())
                {
                    const aiVector3D& bitangent = source_mesh->mBitangents[vertex_index];
                    const std::array<float, 3> tangent_axis = {tangent.x, tangent.y, tangent.z};
                    const std::array<float, 3> normal_axis = {vertex.normal[0], vertex.normal[1], vertex.normal[2]};
                    const std::array<float, 3> bitangent_axis = {bitangent.x, bitangent.y, bitangent.z};
                    vertex.tangent[3] = Dot(Cross(normal_axis, tangent_axis), bitangent_axis) < 0.0f ? -1.0f : 1.0f;
                }
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