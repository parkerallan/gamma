#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct ModelBounds
{
    bool valid = false;
    std::array<float, 3> minimum = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> maximum = {0.0f, 0.0f, 0.0f};
};

struct ModelVertex
{
    std::array<float, 3> position = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> normal = {0.0f, 1.0f, 0.0f};
    std::array<float, 4> tangent = {1.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 2> uv0 = {0.0f, 0.0f};
};

struct ModelMeshAsset
{
    std::string name;
    std::uint32_t material_index = 0;
    std::vector<ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    ModelBounds bounds;
};

struct ModelTextureAsset
{
    bool valid = false;
    int width = 0;
    int height = 0;
    bool srgb = false;
    bool has_transparency = false;
    float alpha_min = 1.0f;
    float alpha_max = 1.0f;
    std::vector<std::uint8_t> pixels;
};

enum class ModelAlphaMode
{
    Opaque,
    Mask,
    Blend,
};

struct ModelMaterialAsset
{
    std::string name;
    std::array<float, 4> base_color = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 3> emissive_color = {0.0f, 0.0f, 0.0f};
    float opacity_factor = 1.0f;
    float metallic_factor = 1.0f;
    float roughness_factor = 1.0f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    float alpha_cutoff = 0.5f;
    ModelAlphaMode alpha_mode = ModelAlphaMode::Opaque;
    bool double_sided = false;
    bool unlit = false;
    std::string base_color_texture_source;
    ModelTextureAsset base_color_texture;
    std::string metallic_roughness_texture_source;
    ModelTextureAsset metallic_roughness_texture;
    std::string metallic_texture_source;
    ModelTextureAsset metallic_texture;
    std::string roughness_texture_source;
    ModelTextureAsset roughness_texture;
    std::string normal_texture_source;
    ModelTextureAsset normal_texture;
    std::string occlusion_texture_source;
    ModelTextureAsset occlusion_texture;
    std::string emissive_texture_source;
    ModelTextureAsset emissive_texture;
    bool uses_alpha_transparency = false;
};

struct ModelAsset
{
    bool loaded = false;
    std::string error_message;
    std::vector<ModelMeshAsset> meshes;
    std::vector<ModelMaterialAsset> materials;
    ModelBounds bounds;

    static bool IsSupportedPath(const std::filesystem::path& path);
};

ModelAsset LoadModelAsset(const std::filesystem::path& path);