#include "assets/ModelAsset.h"
#include "vfs/AssetVFS.h"

#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/matrix3x3.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/texture.h>

#include <nlohmann/json.hpp>

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include <stb_image.h>

#include <nlohmann/json.hpp>

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>

using json = nlohmann::json;

namespace
{
bool LoadTextureFromFile(const std::filesystem::path& texture_path, ModelTextureAsset& texture_asset);
bool LoadEmbeddedTexture(const aiTexture* embedded_texture, ModelTextureAsset& texture_asset);
std::string NormalizeAssimpPath(std::string path);

struct GltfMaterialExtensionData
{
    bool has_iridescence = false;
    bool has_volume = false;
    bool has_transmission = false;
    bool has_ior = false;
    bool has_clearcoat = false;
    bool has_emissive_strength = false;
    bool has_specular = false;
    float factor = 0.0f;
    float ior = 1.3f;
    float thickness_minimum = 100.0f;
    float thickness_maximum = 400.0f;
    float base_ior = 1.5f;
    float transmission_factor = 0.0f;
    float volume_thickness_factor = 0.0f;
    float attenuation_distance = 0.0f;
    float clearcoat_factor = 0.0f;
    float clearcoat_roughness_factor = 0.0f;
    float clearcoat_normal_scale = 1.0f;
    float emissive_strength = 1.0f;
    float specular_factor = 1.0f;
    std::array<float, 3> attenuation_color = {1.0f, 1.0f, 1.0f};
    std::array<float, 3> specular_color_factor = {1.0f, 1.0f, 1.0f};
    std::string transmission_texture_reference;
    std::string texture_reference;
    std::string thickness_texture_reference;
    std::string volume_thickness_texture_reference;
    std::string clearcoat_texture_reference;
    std::string clearcoat_roughness_texture_reference;
    std::string clearcoat_normal_texture_reference;
    std::string specular_texture_reference;
    std::string specular_color_texture_reference;
};

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

float ReadJsonFloat(const json& object, const char* key, float fallback_value)
{
    if (!object.is_object())
    {
        return fallback_value;
    }

    const auto it = object.find(key);
    if (it == object.end() || !it->is_number())
    {
        return fallback_value;
    }

    return it->get<float>();
}

bool ReadTextFile(const std::filesystem::path& path, std::string& contents)
{
    if (g_asset_reader != nullptr)
    {
        const auto bytes = g_asset_reader->ReadFile(NormalizeAssimpPath(path.generic_string()));
        if (!bytes.empty())
        {
            contents.assign(bytes.begin(), bytes.end());
            return true;
        }
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    std::ostringstream stream;
    stream << input.rdbuf();
    if (!input.good() && !input.eof())
    {
        return false;
    }

    contents = stream.str();
    return true;
}

std::string ResolveGltfTextureReference(
    const json& texture_info,
    const std::vector<int>& texture_sources,
    const std::vector<std::string>& image_uris)
{
    if (!texture_info.is_object())
    {
        return {};
    }

    const auto texture_index_it = texture_info.find("index");
    if (texture_index_it == texture_info.end() || !texture_index_it->is_number_integer())
    {
        return {};
    }

    const int texture_index = texture_index_it->get<int>();
    if (texture_index < 0 || texture_index >= static_cast<int>(texture_sources.size()))
    {
        return {};
    }

    const int image_index = texture_sources[static_cast<std::size_t>(texture_index)];
    if (image_index < 0 || image_index >= static_cast<int>(image_uris.size()))
    {
        return {};
    }

    return image_uris[static_cast<std::size_t>(image_index)];
}

std::array<float, 3> ReadJsonFloat3(const json& object, const char* key, const std::array<float, 3>& fallback_value)
{
    if (!object.is_object())
    {
        return fallback_value;
    }

    const auto it = object.find(key);
    if (it == object.end() || !it->is_array() || it->size() < 3)
    {
        return fallback_value;
    }

    std::array<float, 3> value = fallback_value;
    for (std::size_t index = 0; index < 3; ++index)
    {
        if ((*it)[index].is_number())
        {
            value[index] = (*it)[index].get<float>();
        }
    }

    return value;
}

std::vector<GltfMaterialExtensionData> LoadGltfMaterialExtensionData(const std::filesystem::path& model_path)
{
    if (ToLowerExtension(model_path) != ".gltf")
    {
        return {};
    }

    std::string contents;
    if (!ReadTextFile(model_path, contents) || contents.empty())
    {
        return {};
    }

    json root = json::parse(contents, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        return {};
    }

    std::vector<std::string> image_uris;
    if (const auto images_it = root.find("images"); images_it != root.end() && images_it->is_array())
    {
        image_uris.reserve(images_it->size());
        for (const json& image : *images_it)
        {
            if (!image.is_object())
            {
                image_uris.emplace_back();
                continue;
            }

            const auto uri_it = image.find("uri");
            image_uris.push_back(uri_it != image.end() && uri_it->is_string() ? uri_it->get<std::string>() : std::string());
        }
    }

    std::vector<int> texture_sources;
    if (const auto textures_it = root.find("textures"); textures_it != root.end() && textures_it->is_array())
    {
        texture_sources.reserve(textures_it->size());
        for (const json& texture : *textures_it)
        {
            if (!texture.is_object())
            {
                texture_sources.push_back(-1);
                continue;
            }

            const auto source_it = texture.find("source");
            texture_sources.push_back(source_it != texture.end() && source_it->is_number_integer() ? source_it->get<int>() : -1);
        }
    }

    std::vector<GltfMaterialExtensionData> material_data;
    if (const auto materials_it = root.find("materials"); materials_it != root.end() && materials_it->is_array())
    {
        material_data.resize(materials_it->size());
        for (std::size_t material_index = 0; material_index < materials_it->size(); ++material_index)
        {
            const json& material = (*materials_it)[material_index];
            if (!material.is_object())
            {
                continue;
            }

            const auto extensions_it = material.find("extensions");
            if (extensions_it == material.end() || !extensions_it->is_object())
            {
                continue;
            }

            GltfMaterialExtensionData& extension_data = material_data[material_index];
            const auto iridescence_it = extensions_it->find("KHR_materials_iridescence");
            if (iridescence_it != extensions_it->end() && iridescence_it->is_object())
            {
                extension_data.has_iridescence = true;
                extension_data.factor = ReadJsonFloat(*iridescence_it, "iridescenceFactor", 0.0f);
                extension_data.ior = ReadJsonFloat(*iridescence_it, "iridescenceIor", 1.3f);
                extension_data.thickness_minimum = ReadJsonFloat(*iridescence_it, "iridescenceThicknessMinimum", 100.0f);
                extension_data.thickness_maximum = ReadJsonFloat(*iridescence_it, "iridescenceThicknessMaximum", 400.0f);
                extension_data.texture_reference = ResolveGltfTextureReference(
                    iridescence_it->value("iridescenceTexture", json::object()),
                    texture_sources,
                    image_uris);
                extension_data.thickness_texture_reference = ResolveGltfTextureReference(
                    iridescence_it->value("iridescenceThicknessTexture", json::object()),
                    texture_sources,
                    image_uris);
            }

            if (const auto ior_it = extensions_it->find("KHR_materials_ior"); ior_it != extensions_it->end() && ior_it->is_object())
            {
                extension_data.has_ior = true;
                extension_data.base_ior = ReadJsonFloat(*ior_it, "ior", 1.5f);
            }

            if (const auto transmission_it = extensions_it->find("KHR_materials_transmission"); transmission_it != extensions_it->end() && transmission_it->is_object())
            {
                extension_data.has_transmission = true;
                extension_data.transmission_factor = ReadJsonFloat(*transmission_it, "transmissionFactor", 0.0f);
                extension_data.transmission_texture_reference = ResolveGltfTextureReference(
                    transmission_it->value("transmissionTexture", json::object()),
                    texture_sources,
                    image_uris);
            }

            if (const auto volume_it = extensions_it->find("KHR_materials_volume"); volume_it != extensions_it->end() && volume_it->is_object())
            {
                extension_data.has_volume = true;
                extension_data.volume_thickness_factor = ReadJsonFloat(*volume_it, "thicknessFactor", 0.0f);
                extension_data.attenuation_distance = ReadJsonFloat(*volume_it, "attenuationDistance", 0.0f);
                extension_data.attenuation_color = ReadJsonFloat3(*volume_it, "attenuationColor", {1.0f, 1.0f, 1.0f});
                extension_data.volume_thickness_texture_reference = ResolveGltfTextureReference(
                    volume_it->value("thicknessTexture", json::object()),
                    texture_sources,
                    image_uris);
            }

            if (const auto clearcoat_it = extensions_it->find("KHR_materials_clearcoat"); clearcoat_it != extensions_it->end() && clearcoat_it->is_object())
            {
                extension_data.has_clearcoat = true;
                extension_data.clearcoat_factor = ReadJsonFloat(*clearcoat_it, "clearcoatFactor", 0.0f);
                extension_data.clearcoat_roughness_factor = ReadJsonFloat(*clearcoat_it, "clearcoatRoughnessFactor", 0.0f);
                extension_data.clearcoat_texture_reference = ResolveGltfTextureReference(
                    clearcoat_it->value("clearcoatTexture", json::object()),
                    texture_sources,
                    image_uris);
                extension_data.clearcoat_roughness_texture_reference = ResolveGltfTextureReference(
                    clearcoat_it->value("clearcoatRoughnessTexture", json::object()),
                    texture_sources,
                    image_uris);
                const json clearcoat_normal_texture = clearcoat_it->value("clearcoatNormalTexture", json::object());
                extension_data.clearcoat_normal_scale = ReadJsonFloat(clearcoat_normal_texture, "scale", 1.0f);
                extension_data.clearcoat_normal_texture_reference = ResolveGltfTextureReference(
                    clearcoat_normal_texture,
                    texture_sources,
                    image_uris);
            }

            if (const auto emissive_strength_it = extensions_it->find("KHR_materials_emissive_strength"); emissive_strength_it != extensions_it->end() && emissive_strength_it->is_object())
            {
                extension_data.has_emissive_strength = true;
                extension_data.emissive_strength = ReadJsonFloat(*emissive_strength_it, "emissiveStrength", 1.0f);
            }

            // KHR_materials_specular: parsed here because Assimp's GLTF importer does
            // not reliably populate AI_MATKEY_SPECULAR_FACTOR / AI_MATKEY_COLOR_SPECULAR
            // for this extension. Without this, materials with specularFactor=0 fall
            // back to the default 1.0 and incorrectly render shiny.
            if (const auto specular_it = extensions_it->find("KHR_materials_specular"); specular_it != extensions_it->end() && specular_it->is_object())
            {
                extension_data.has_specular = true;
                extension_data.specular_factor = ReadJsonFloat(*specular_it, "specularFactor", 1.0f);
                extension_data.specular_color_factor = ReadJsonFloat3(*specular_it, "specularColorFactor", {1.0f, 1.0f, 1.0f});
                extension_data.specular_texture_reference = ResolveGltfTextureReference(
                    specular_it->value("specularTexture", json::object()),
                    texture_sources,
                    image_uris);
                extension_data.specular_color_texture_reference = ResolveGltfTextureReference(
                    specular_it->value("specularColorTexture", json::object()),
                    texture_sources,
                    image_uris);
            }
        }
    }

    return material_data;
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

std::array<float, 3> ReadMaterialSpecularColor(const aiMaterial* material)
{
    aiColor3D color = aiColor3D(1.0f, 1.0f, 1.0f);
    if (material != nullptr)
    {
        material->Get(AI_MATKEY_COLOR_SPECULAR, color);
    }

    return {color.r, color.g, color.b};
}

std::array<float, 3> ReadMaterialSheenColor(const aiMaterial* material)
{
    aiColor3D color = aiColor3D(0.0f, 0.0f, 0.0f);
    if (material != nullptr)
    {
        material->Get(AI_MATKEY_SHEEN_COLOR_FACTOR, color);
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

float ReadMaterialSpecularFactor(const aiMaterial* material)
{
    return ReadMaterialFloatProperty(material, AI_MATKEY_SPECULAR_FACTOR, 1.0f);
}

float ReadMaterialSheenRoughness(const aiMaterial* material)
{
    return ReadMaterialFloatProperty(material, AI_MATKEY_SHEEN_ROUGHNESS_FACTOR, 0.0f);
}

bool ReadMaterialTextureTransform(
    const aiMaterial* material,
    aiTextureType texture_type,
    unsigned int texture_index,
    ModelTextureTransform& texture_transform)
{
    if (material == nullptr)
    {
        return false;
    }

    aiUVTransform transform;
    if (material->Get(AI_MATKEY_UVTRANSFORM(texture_type, texture_index), transform) != AI_SUCCESS)
    {
        return false;
    }

    texture_transform.valid = true;
    texture_transform.translation = {transform.mTranslation.x, transform.mTranslation.y};
    texture_transform.scale = {transform.mScaling.x, transform.mScaling.y};
    texture_transform.rotation = transform.mRotation;
    return true;
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
    ModelTextureAsset& texture_asset,
    unsigned int texture_index = 0,
    ModelTextureTransform* texture_transform = nullptr)
{
    if (material == nullptr)
    {
        return false;
    }

    for (aiTextureType texture_type : texture_types)
    {
        if (material->GetTextureCount(texture_type) <= texture_index)
        {
            continue;
        }

        aiString texture_path_string;
        if (material->GetTexture(texture_type, texture_index, &texture_path_string) != AI_SUCCESS)
        {
            continue;
        }

        const std::string texture_reference = ToDisplayString(texture_path_string);
        if (texture_reference.empty())
        {
            continue;
        }

        if (texture_transform != nullptr && !texture_transform->valid)
        {
            ReadMaterialTextureTransform(material, texture_type, texture_index, *texture_transform);
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

std::array<float, 2> ApplyTextureTransform(const std::array<float, 2>& uv, const ModelTextureTransform& texture_transform)
{
    if (!texture_transform.valid)
    {
        return uv;
    }

    const float scaled_u = uv[0] * texture_transform.scale[0];
    const float scaled_v = uv[1] * texture_transform.scale[1];
    const float c = std::cos(texture_transform.rotation);
    const float s = std::sin(texture_transform.rotation);
    return {
        scaled_u * c - scaled_v * s + texture_transform.translation[0],
        scaled_u * s + scaled_v * c + texture_transform.translation[1],
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

std::string BuildImportedMeshName(const aiMesh* source_mesh, const aiNode* source_node, unsigned int mesh_slot)
{
    std::string name;
    if (source_node != nullptr)
    {
        name = ToDisplayString(source_node->mName);
    }
    if (name.empty() && source_mesh != nullptr)
    {
        name = ToDisplayString(source_mesh->mName);
    }
    if (name.empty())
    {
        name = "Mesh";
    }
    if (source_node != nullptr && source_node->mNumMeshes > 1)
    {
        name += " " + std::to_string(mesh_slot + 1);
    }
    return name;
}

void AppendNodeMeshes(const aiScene* scene, const aiNode* node, const aiMatrix4x4& parent_transform, ModelAsset& asset)
{
    if (scene == nullptr || node == nullptr)
    {
        return;
    }

    const aiMatrix4x4 node_transform = parent_transform * node->mTransformation;
    aiMatrix3x3 tangent_transform(node_transform);
    aiMatrix3x3 normal_transform(node_transform);
    normal_transform.Inverse().Transpose();

    for (unsigned int node_mesh_index = 0; node_mesh_index < node->mNumMeshes; ++node_mesh_index)
    {
        const unsigned int mesh_index = node->mMeshes[node_mesh_index];
        if (mesh_index >= scene->mNumMeshes)
        {
            continue;
        }

        const aiMesh* source_mesh = scene->mMeshes[mesh_index];
        if (source_mesh == nullptr)
        {
            continue;
        }

        ModelMeshAsset mesh;
        mesh.name = BuildImportedMeshName(source_mesh, node, node_mesh_index);
        mesh.material_index = source_mesh->mMaterialIndex;
        mesh.vertices.reserve(source_mesh->mNumVertices);

        for (unsigned int vertex_index = 0; vertex_index < source_mesh->mNumVertices; ++vertex_index)
        {
            ModelVertex vertex;

            aiVector3D position = source_mesh->mVertices[vertex_index];
            position *= node_transform;
            vertex.position = {position.x, position.y, position.z};

            aiVector3D normal(0.0f, 1.0f, 0.0f);
            if (source_mesh->HasNormals())
            {
                normal = source_mesh->mNormals[vertex_index];
                normal *= normal_transform;
                normal.NormalizeSafe();
                vertex.normal = {normal.x, normal.y, normal.z};
            }

            if (source_mesh->HasTangentsAndBitangents())
            {
                aiVector3D tangent = source_mesh->mTangents[vertex_index];
                tangent *= tangent_transform;
                tangent.NormalizeSafe();

                aiVector3D bitangent(0.0f, 0.0f, 0.0f);
                if (source_mesh->mBitangents != nullptr)
                {
                    bitangent = source_mesh->mBitangents[vertex_index];
                    bitangent *= tangent_transform;
                    bitangent.NormalizeSafe();
                }

                vertex.tangent = {tangent.x, tangent.y, tangent.z, 1.0f};
                if (source_mesh->HasNormals() && bitangent.SquareLength() > 0.0f)
                {
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
                if (mesh.material_index < asset.materials.size())
                {
                    vertex.uv0 = ApplyTextureTransform(vertex.uv0, asset.materials[mesh.material_index].uv_transform);
                }
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

    for (unsigned int child_index = 0; child_index < node->mNumChildren; ++child_index)
    {
        AppendNodeMeshes(scene, node->mChildren[child_index], node_transform, asset);
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

    //const std::uint64_t parse_start_ticks = SDL_GetPerformanceCounter();

    Assimp::Importer importer;
    // Note: aiProcess_ValidateDataStructure is intentionally omitted. Assimp's validator
    // false-positives on several glTF 2.0 PBR extensions (notably KHR_materials_specular,
    // which registers specularTexture and specularColorTexture as two SPECULAR slots
    // without updating the slot count, causing
    //   "Specular #1 is set, but there are only 1 specular textures"
    // and rejecting otherwise valid assets).
    const unsigned int import_flags =
        aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_ImproveCacheLocality |
        aiProcess_CalcTangentSpace |
        aiProcess_GenSmoothNormals |
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

    const std::vector<GltfMaterialExtensionData> gltf_material_extensions = LoadGltfMaterialExtensionData(path);

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
        material_asset.specular_color = ReadMaterialSpecularColor(source_material);
        material_asset.sheen_color = ReadMaterialSheenColor(source_material);
        material_asset.opacity_factor = ReadMaterialOpacityFactor(source_material);
        material_asset.metallic_factor = ReadMaterialMetallicFactor(source_material);
        material_asset.roughness_factor = ReadMaterialRoughnessFactor(source_material);
        material_asset.normal_scale = ReadMaterialNormalScale(source_material);
        material_asset.occlusion_strength = ReadMaterialOcclusionStrength(source_material);
        material_asset.specular_factor = ReadMaterialSpecularFactor(source_material);
        material_asset.sheen_roughness_factor = ReadMaterialSheenRoughness(source_material);
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
            material_asset.base_color_texture,
            0,
            &material_asset.uv_transform);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_UNKNOWN},
            false,
            material_asset.metallic_roughness_texture_source,
            material_asset.metallic_roughness_texture,
            0,
            &material_asset.uv_transform);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_METALNESS},
            false,
            material_asset.metallic_texture_source,
            material_asset.metallic_texture,
            0,
            &material_asset.uv_transform);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_DIFFUSE_ROUGHNESS},
            false,
            material_asset.roughness_texture_source,
            material_asset.roughness_texture,
            0,
            &material_asset.uv_transform);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA},
            false,
            material_asset.normal_texture_source,
            material_asset.normal_texture,
            0,
            &material_asset.uv_transform);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_AMBIENT_OCCLUSION},
            false,
            material_asset.occlusion_texture_source,
            material_asset.occlusion_texture,
            0,
            &material_asset.uv_transform);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR},
            true,
            material_asset.emissive_texture_source,
            material_asset.emissive_texture,
            0,
            &material_asset.uv_transform);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_SPECULAR},
            false,
            material_asset.specular_texture_source,
            material_asset.specular_texture,
            0,
            &material_asset.uv_transform);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_SPECULAR},
            true,
            material_asset.specular_color_texture_source,
            material_asset.specular_color_texture,
            1,
            &material_asset.uv_transform);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_SHEEN},
            true,
            material_asset.sheen_color_texture_source,
            material_asset.sheen_color_texture,
            0,
            &material_asset.uv_transform);
        LoadMaterialTexture(
            scene,
            source_material,
            path,
            {aiTextureType_SHEEN},
            false,
            material_asset.sheen_roughness_texture_source,
            material_asset.sheen_roughness_texture,
            1,
            &material_asset.uv_transform);

        if (material_index < gltf_material_extensions.size())
        {
            const GltfMaterialExtensionData& extension_data = gltf_material_extensions[material_index];
            material_asset.index_of_refraction = extension_data.base_ior;
            material_asset.transmission_factor = extension_data.transmission_factor;
            material_asset.volume_thickness_factor = extension_data.volume_thickness_factor;
            material_asset.attenuation_distance = extension_data.attenuation_distance;
            material_asset.attenuation_color = extension_data.attenuation_color;

            if (!extension_data.transmission_texture_reference.empty())
            {
                LoadTextureReference(
                    scene,
                    path,
                    extension_data.transmission_texture_reference,
                    false,
                    material_asset.transmission_texture_source,
                    material_asset.transmission_texture);
            }

            if (extension_data.has_emissive_strength)
            {
                for (float& channel : material_asset.emissive_color)
                {
                    channel *= extension_data.emissive_strength;
                }
            }

            if (extension_data.has_clearcoat)
            {
                material_asset.clearcoat_factor = extension_data.clearcoat_factor;
                material_asset.clearcoat_roughness_factor = extension_data.clearcoat_roughness_factor;
                material_asset.clearcoat_normal_scale = extension_data.clearcoat_normal_scale;

                if (!extension_data.clearcoat_texture_reference.empty())
                {
                    LoadTextureReference(
                        scene,
                        path,
                        extension_data.clearcoat_texture_reference,
                        false,
                        material_asset.clearcoat_texture_source,
                        material_asset.clearcoat_texture);
                }

                if (!extension_data.clearcoat_roughness_texture_reference.empty())
                {
                    LoadTextureReference(
                        scene,
                        path,
                        extension_data.clearcoat_roughness_texture_reference,
                        false,
                        material_asset.clearcoat_roughness_texture_source,
                        material_asset.clearcoat_roughness_texture);
                }

                if (!extension_data.clearcoat_normal_texture_reference.empty())
                {
                    LoadTextureReference(
                        scene,
                        path,
                        extension_data.clearcoat_normal_texture_reference,
                        false,
                        material_asset.clearcoat_normal_texture_source,
                        material_asset.clearcoat_normal_texture);
                }
            }

            if (extension_data.has_iridescence)
            {
                material_asset.iridescence_factor = extension_data.factor;
                material_asset.iridescence_ior = extension_data.ior;
                material_asset.iridescence_thickness_minimum = extension_data.thickness_minimum;
                material_asset.iridescence_thickness_maximum = extension_data.thickness_maximum;

                if (!extension_data.texture_reference.empty())
                {
                    LoadTextureReference(
                        scene,
                        path,
                        extension_data.texture_reference,
                        false,
                        material_asset.iridescence_texture_source,
                        material_asset.iridescence_texture);
                }

                if (!extension_data.thickness_texture_reference.empty())
                {
                    LoadTextureReference(
                        scene,
                        path,
                        extension_data.thickness_texture_reference,
                        false,
                        material_asset.iridescence_thickness_texture_source,
                        material_asset.iridescence_thickness_texture);
                }
            }

            if (!extension_data.volume_thickness_texture_reference.empty())
            {
                LoadTextureReference(
                    scene,
                    path,
                    extension_data.volume_thickness_texture_reference,
                    false,
                    material_asset.volume_thickness_texture_source,
                    material_asset.volume_thickness_texture);
            }

            if (extension_data.has_specular)
            {
                material_asset.specular_factor = extension_data.specular_factor;
                material_asset.specular_color = extension_data.specular_color_factor;

                if (!extension_data.specular_texture_reference.empty())
                {
                    LoadTextureReference(
                        scene,
                        path,
                        extension_data.specular_texture_reference,
                        false,
                        material_asset.specular_texture_source,
                        material_asset.specular_texture);
                }

                if (!extension_data.specular_color_texture_reference.empty())
                {
                    LoadTextureReference(
                        scene,
                        path,
                        extension_data.specular_color_texture_reference,
                        true,
                        material_asset.specular_color_texture_source,
                        material_asset.specular_color_texture);
                }
            }
        }

        material_asset.base_color[3] = std::clamp(material_asset.base_color[3] * material_asset.opacity_factor, 0.0f, 1.0f);
        material_asset.uses_alpha_transparency = DetermineMaterialTransparency(material_asset);
        asset.materials.push_back(std::move(material_asset));
    }

    asset.meshes.reserve(scene->mNumMeshes);
    AppendNodeMeshes(scene, scene->mRootNode, aiMatrix4x4(), asset);

    asset.loaded = true;

    //const std::uint64_t freq = SDL_GetPerformanceFrequency();
    // if (freq > 0)
    // {
    //     const double parse_ms = static_cast<double>(SDL_GetPerformanceCounter() - parse_start_ticks) * 1000.0 / static_cast<double>(freq);
    //     SDL_Log(
    //         "ModelAsset parse '%s': %.2f ms (%zu meshes, %zu materials)",
    //         path.filename().string().c_str(),
    //         parse_ms,
    //         asset.meshes.size(),
    //         asset.materials.size());
    // }

    return asset;
}