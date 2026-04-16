#pragma once

#include <filesystem>
#include <array>
#include <string>
#include <vector>

struct ModelColorMetadata
{
    bool valid = false;
    std::array<float, 4> rgba = {1.0f, 1.0f, 1.0f, 1.0f};
};

struct ModelTextureReference
{
    std::string slot_label;
    std::string path;
};

struct ModelMaterialMetadata
{
    std::string name;
    ModelColorMetadata base_color;
    ModelColorMetadata emissive_color;
    std::string alpha_mode = "OPAQUE";
    float alpha_cutoff = 0.5f;
    bool has_alpha_cutoff = false;
    bool double_sided = false;
    bool unlit = false;
    float opacity = 1.0f;
    bool has_opacity = false;
    float normal_scale = 1.0f;
    bool has_normal_scale = false;
    float occlusion_strength = 1.0f;
    bool has_occlusion_strength = false;
    float roughness = 0.0f;
    bool has_roughness = false;
    float metalness = 0.0f;
    bool has_metalness = false;
    std::vector<ModelTextureReference> textures;
};

struct ModelAnimationMetadata
{
    std::string name;
    double duration = 0.0;
    double ticks_per_second = 0.0;
    unsigned int channel_count = 0;
};

struct ModelMetadata
{
    bool parsed = false;
    bool supported = false;
    std::string error_message;
    unsigned int node_count = 0;
    unsigned int mesh_count = 0;
    unsigned int material_count = 0;
    unsigned int animation_count = 0;
    unsigned int embedded_texture_count = 0;
    std::vector<ModelMaterialMetadata> materials;
    std::vector<ModelAnimationMetadata> animations;

    static bool IsSupportedModelPath(const std::filesystem::path& path);
};

ModelMetadata LoadModelMetadata(const std::filesystem::path& path);