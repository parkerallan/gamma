#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

struct ParsedMaterialEntry
{
    std::string key;
    std::string value;
};

struct ParsedMaterialTextureReference
{
    std::string slot_label;
    std::string path;
};

struct ParsedMaterialMetadata
{
    bool parsed = false;
    std::string error_message;
    std::string material_name;
    std::string shader_name;
    bool has_base_color = false;
    std::array<float, 4> base_color = {1.0f, 1.0f, 1.0f, 1.0f};
    bool has_emissive_color = false;
    std::array<float, 4> emissive_color = {0.0f, 0.0f, 0.0f, 1.0f};
    bool has_opacity = false;
    float opacity = 1.0f;
    bool has_roughness = false;
    float roughness = 0.0f;
    bool has_metalness = false;
    float metalness = 0.0f;
    std::vector<ParsedMaterialTextureReference> textures;
    std::vector<ParsedMaterialEntry> entries;

    static bool IsSupportedPath(const std::filesystem::path& path);
};

struct ImageMetadata
{
    bool parsed = false;
    std::string error_message;
    int width = 0;
    int height = 0;
    int channel_count = 0;
    int bits_per_channel = 0;

    static bool IsSupportedPath(const std::filesystem::path& path);
};

struct FontMetadata
{
    bool parsed = false;
    std::string error_message;
    int glyph_count = 0;

    static bool IsSupportedPath(const std::filesystem::path& path);
};

ParsedMaterialMetadata LoadMaterialMetadata(const std::filesystem::path& path);
ImageMetadata LoadImageMetadata(const std::filesystem::path& path);
FontMetadata LoadFontMetadata(const std::filesystem::path& path);