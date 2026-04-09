#include "assets/AssetMetadata.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace
{
std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string Trim(std::string value)
{
    const auto is_space = [](unsigned char character)
    {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char character)
    {
        return !is_space(character);
    }));

    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char character)
    {
        return !is_space(character);
    }).base(), value.end());

    return value;
}

bool ParseFloatList4(const std::string& value, std::array<float, 4>& result)
{
    std::array<float, 4> parsed{};
    std::stringstream stream(value);
    std::string token;
    for (int index = 0; index < 4; ++index)
    {
        if (!std::getline(stream, token, ','))
        {
            return false;
        }

        try
        {
            parsed[static_cast<std::size_t>(index)] = std::stof(Trim(token));
        }
        catch (...)
        {
            return false;
        }
    }

    result = parsed;
    return true;
}

bool ParseFloatValue(const std::string& value, float& result)
{
    try
    {
        result = std::stof(Trim(value));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::string GetLowerExtension(const std::filesystem::path& path)
{
    return ToLower(path.extension().string());
}

bool IsTextureKey(const std::string& lowered_key)
{
    return lowered_key == "basecolortexture" ||
        lowered_key == "diffusetexture" ||
        lowered_key == "normaltexture" ||
        lowered_key == "emissivetexture" ||
        lowered_key == "occlusiontexture" ||
        lowered_key == "roughnesstexture" ||
        lowered_key == "metalnesstexture" ||
        lowered_key == "opacitytexture";
}

std::string GetTextureSlotLabel(const std::string& lowered_key)
{
    if (lowered_key == "basecolortexture")
    {
        return "Base Color";
    }
    if (lowered_key == "diffusetexture")
    {
        return "Diffuse";
    }
    if (lowered_key == "normaltexture")
    {
        return "Normal";
    }
    if (lowered_key == "emissivetexture")
    {
        return "Emissive";
    }
    if (lowered_key == "occlusiontexture")
    {
        return "Occlusion";
    }
    if (lowered_key == "roughnesstexture")
    {
        return "Roughness";
    }
    if (lowered_key == "metalnesstexture")
    {
        return "Metalness";
    }
    if (lowered_key == "opacitytexture")
    {
        return "Opacity";
    }

    return "Texture";
}
}

bool ParsedMaterialMetadata::IsSupportedPath(const std::filesystem::path& path)
{
    return GetLowerExtension(path) == ".mat";
}

bool TextureMetadata::IsSupportedPath(const std::filesystem::path& path)
{
    const std::string extension = GetLowerExtension(path);
    return extension == ".png" ||
        extension == ".jpg" ||
        extension == ".jpeg" ||
        extension == ".tga" ||
        extension == ".bmp" ||
        extension == ".psd" ||
        extension == ".gif" ||
        extension == ".hdr";
}

ParsedMaterialMetadata LoadMaterialMetadata(const std::filesystem::path& path)
{
    ParsedMaterialMetadata metadata;
    if (!ParsedMaterialMetadata::IsSupportedPath(path))
    {
        metadata.error_message = "Unsupported material format.";
        return metadata;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        metadata.error_message = "Failed to open material file.";
        return metadata;
    }

    std::string line;
    while (std::getline(input, line))
    {
        line = Trim(line);
        if (line.empty())
        {
            continue;
        }

        const std::size_t separator = line.find(':');
        if (separator == std::string::npos)
        {
            continue;
        }

        ParsedMaterialEntry entry;
        entry.key = Trim(line.substr(0, separator));
        entry.value = Trim(line.substr(separator + 1));
        if (entry.key.empty())
        {
            continue;
        }

        const std::string lowered_key = ToLower(entry.key);
        if (lowered_key == "material")
        {
            metadata.material_name = entry.value;
        }
        else if (lowered_key == "shader")
        {
            metadata.shader_name = entry.value;
        }
        else if (lowered_key == "basecolor")
        {
            metadata.has_base_color = ParseFloatList4(entry.value, metadata.base_color);
        }
        else if (lowered_key == "emissive")
        {
            metadata.has_emissive_color = ParseFloatList4(entry.value, metadata.emissive_color);
        }
        else if (lowered_key == "opacity")
        {
            metadata.has_opacity = ParseFloatValue(entry.value, metadata.opacity);
        }
        else if (lowered_key == "roughness")
        {
            metadata.has_roughness = ParseFloatValue(entry.value, metadata.roughness);
        }
        else if (lowered_key == "metalness")
        {
            metadata.has_metalness = ParseFloatValue(entry.value, metadata.metalness);
        }
        else if (IsTextureKey(lowered_key))
        {
            ParsedMaterialTextureReference texture_reference;
            texture_reference.slot_label = GetTextureSlotLabel(lowered_key);
            texture_reference.path = entry.value;
            metadata.textures.push_back(std::move(texture_reference));
        }

        metadata.entries.push_back(std::move(entry));
    }

    metadata.parsed = true;
    if (metadata.material_name.empty())
    {
        metadata.material_name = path.stem().string();
    }
    return metadata;
}

TextureMetadata LoadTextureMetadata(const std::filesystem::path& path)
{
    TextureMetadata metadata;
    if (!TextureMetadata::IsSupportedPath(path))
    {
        metadata.error_message = "Unsupported texture format.";
        return metadata;
    }

    if (stbi_info(path.string().c_str(), &metadata.width, &metadata.height, &metadata.channel_count) == 0)
    {
        metadata.error_message = stbi_failure_reason() != nullptr ? stbi_failure_reason() : "Failed to read texture metadata.";
        return metadata;
    }

    metadata.bits_per_channel = stbi_is_16_bit(path.string().c_str()) != 0 ? 16 : 8;
    metadata.parsed = true;
    return metadata;
}