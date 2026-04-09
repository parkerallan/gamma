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
    std::vector<std::uint8_t> pixels;
};

struct ModelMaterialAsset
{
    std::string name;
    std::array<float, 4> base_color = {1.0f, 1.0f, 1.0f, 1.0f};
    std::string base_color_texture_source;
    ModelTextureAsset base_color_texture;
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