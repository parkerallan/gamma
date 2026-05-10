#include "assets/AssetMetadata.h"

#include "imgui.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <tinyexr.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>

namespace
{
constexpr float kFontMetadataPreviewSizePixels = 48.0f;
constexpr ImWchar kFontPreviewGlyphRanges[] = {32, 126, 0};

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

bool ParseIcoMetadata(const std::filesystem::path& path, ImageMetadata& metadata)
{
    struct IcoDirEntry
    {
        std::uint8_t width;
        std::uint8_t height;
        std::uint8_t color_count;
        std::uint8_t reserved;
        std::uint16_t planes;
        std::uint16_t bit_count;
        std::uint32_t bytes_in_res;
        std::uint32_t image_offset;
    };

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        metadata.error_message = "Failed to open ICO file.";
        return false;
    }

    std::uint16_t reserved = 0;
    std::uint16_t type = 0;
    std::uint16_t count = 0;
    input.read(reinterpret_cast<char*>(&reserved), sizeof(reserved));
    input.read(reinterpret_cast<char*>(&type), sizeof(type));
    input.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!input)
    {
        metadata.error_message = "Failed to read ICO header.";
        return false;
    }

    if (reserved != 0 || type != 1 || count == 0)
    {
        metadata.error_message = "Invalid ICO header.";
        return false;
    }

    int best_width = 0;
    int best_height = 0;
    int best_bit_count = 0;
    for (std::uint16_t index = 0; index < count; ++index)
    {
        IcoDirEntry entry{};
        input.read(reinterpret_cast<char*>(&entry), sizeof(entry));
        if (!input)
        {
            metadata.error_message = "Failed to read ICO directory entry.";
            return false;
        }

        const int entry_width = entry.width == 0 ? 256 : static_cast<int>(entry.width);
        const int entry_height = entry.height == 0 ? 256 : static_cast<int>(entry.height);
        const int entry_bit_count = static_cast<int>(entry.bit_count);
        const int current_score = (entry_width * entry_height * 100) + entry_bit_count;
        const int best_score = (best_width * best_height * 100) + best_bit_count;
        if (current_score > best_score)
        {
            best_width = entry_width;
            best_height = entry_height;
            best_bit_count = entry_bit_count;
        }
    }

    if (best_width <= 0 || best_height <= 0)
    {
        metadata.error_message = "ICO file does not contain valid image entries.";
        return false;
    }

    metadata.width = best_width;
    metadata.height = best_height;
    metadata.channel_count = best_bit_count >= 32 ? 4 : 3;
    metadata.bits_per_channel = 8;
    metadata.parsed = true;
    return true;
}
}

bool ParsedMaterialMetadata::IsSupportedPath(const std::filesystem::path& path)
{
    return GetLowerExtension(path) == ".mat";
}

bool ImageMetadata::IsSupportedPath(const std::filesystem::path& path)
{
    const std::string extension = GetLowerExtension(path);
    return extension == ".png" ||
        extension == ".jpg" ||
        extension == ".jpeg" ||
        extension == ".tga" ||
        extension == ".bmp" ||
        extension == ".psd" ||
        extension == ".gif" ||
        extension == ".hdr" ||
        extension == ".exr" ||
        extension == ".ico";
}

bool FontMetadata::IsSupportedPath(const std::filesystem::path& path)
{
    const std::string extension = GetLowerExtension(path);
    return extension == ".ttf" || extension == ".otf";
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

ImageMetadata LoadImageMetadata(const std::filesystem::path& path)
{
    ImageMetadata metadata;
    if (!ImageMetadata::IsSupportedPath(path))
    {
        metadata.error_message = "Unsupported image format.";
        return metadata;
    }

    const std::string extension = GetLowerExtension(path);
    if (extension == ".exr")
    {
        EXRVersion version;
        int result = ParseEXRVersionFromFile(&version, path.string().c_str());
        if (result != TINYEXR_SUCCESS)
        {
            metadata.error_message = "Failed to read EXR version.";
            return metadata;
        }

        if (version.multipart)
        {
            metadata.error_message = "Multipart EXR metadata is not supported in this panel.";
            return metadata;
        }

        EXRHeader header;
        InitEXRHeader(&header);
        const char* error_message = nullptr;
        result = ParseEXRHeaderFromFile(&header, &version, path.string().c_str(), &error_message);
        if (result != TINYEXR_SUCCESS)
        {
            metadata.error_message = error_message != nullptr ? error_message : "Failed to parse EXR header.";
            if (error_message != nullptr)
            {
                FreeEXRErrorMessage(error_message);
            }
            FreeEXRHeader(&header);
            return metadata;
        }

        metadata.width = header.data_window.max_x - header.data_window.min_x + 1;
        metadata.height = header.data_window.max_y - header.data_window.min_y + 1;
        metadata.channel_count = header.num_channels;
        if (header.num_channels > 0 && header.pixel_types != nullptr)
        {
            if (header.pixel_types[0] == TINYEXR_PIXELTYPE_HALF)
            {
                metadata.bits_per_channel = 16;
            }
            else
            {
                metadata.bits_per_channel = 32;
            }
        }
        else
        {
            metadata.bits_per_channel = 32;
        }

        metadata.parsed = true;
        FreeEXRHeader(&header);
        return metadata;
    }

    if (extension == ".ico")
    {
        if (ParseIcoMetadata(path, metadata))
        {
            return metadata;
        }
    }

    if (stbi_info(path.string().c_str(), &metadata.width, &metadata.height, &metadata.channel_count) == 0)
    {
        metadata.error_message = stbi_failure_reason() != nullptr ? stbi_failure_reason() : "Failed to read image metadata.";
        return metadata;
    }

    metadata.bits_per_channel = stbi_is_16_bit(path.string().c_str()) != 0 ? 16 : 8;
    metadata.parsed = true;
    return metadata;
}

FontMetadata LoadFontMetadata(const std::filesystem::path& path)
{
    FontMetadata metadata;
    if (!FontMetadata::IsSupportedPath(path))
    {
        metadata.error_message = "Unsupported font format.";
        return metadata;
    }

    ImFontAtlas preview_atlas;
    preview_atlas.Flags |= ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoBakedLines;

    ImFontConfig font_config;
    font_config.Flags |= ImFontFlags_NoLoadError;

    ImFont* font = preview_atlas.AddFontFromFileTTF(
        path.string().c_str(),
        kFontMetadataPreviewSizePixels,
        &font_config,
        kFontPreviewGlyphRanges);
    if (font == nullptr)
    {
        metadata.error_message = "Failed to load font data.";
        return metadata;
    }

    unsigned char* atlas_pixels = nullptr;
    int atlas_width = 0;
    int atlas_height = 0;
    preview_atlas.GetTexDataAsRGBA32(&atlas_pixels, &atlas_width, &atlas_height);
    if (atlas_pixels == nullptr || atlas_width <= 0 || atlas_height <= 0)
    {
        metadata.error_message = "Failed to rasterize font preview data.";
        return metadata;
    }

    ImFontBaked* baked_font = font->GetFontBaked(font->LegacySize);
    if (baked_font == nullptr)
    {
        metadata.error_message = "Failed to bake font metrics.";
        return metadata;
    }

    metadata.glyph_count = baked_font->Glyphs.Size;
    metadata.parsed = true;
    return metadata;
}