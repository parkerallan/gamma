#include "assets/SceneMetadata.h"
#include "vfs/AssetVFS.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
std::string TrimCopy(std::string value)
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

bool StartsWith(std::string_view value, std::string_view prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::string ToLowerCopy(std::string_view value)
{
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return lowered;
}

std::string ExtractValue(std::string_view line, std::string_view prefix)
{
    return TrimCopy(std::string(line.substr(prefix.size())));
}

std::string EscapeSceneString(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (char c : value)
    {
        switch (c)
        {
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

std::string UnescapeSceneString(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    bool escaping = false;
    for (char c : value)
    {
        if (!escaping)
        {
            if (c == '\\')
            {
                escaping = true;
            }
            else
            {
                out.push_back(c);
            }
            continue;
        }

        switch (c)
        {
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case '\\':
            out.push_back('\\');
            break;
        default:
            out.push_back(c);
            break;
        }
        escaping = false;
    }

    if (escaping)
    {
        out.push_back('\\');
    }
    return out;
}

bool IsSceneObjectStart(std::string_view line)
{
    return StartsWith(line, "Object:");
}

bool RewriteSceneObjectLines(
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    const std::function<void(std::vector<std::string>&, std::size_t, std::size_t)>& mutator);

bool ParseVector3(std::string_view value, SceneVector3& result)
{
    std::istringstream stream{std::string(value)};
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    char separator = '\0';
    if (!(stream >> x))
    {
        return false;
    }
    if (!(stream >> separator) || separator != ',')
    {
        return false;
    }
    if (!(stream >> y))
    {
        return false;
    }
    if (!(stream >> separator) || separator != ',')
    {
        return false;
    }
    if (!(stream >> z))
    {
        return false;
    }

    result = {x, y, z};
    return true;
}

std::string FormatVector3(const SceneVector3& value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3)
        << value[0] << ", " << value[1] << ", " << value[2];
    return stream.str();
}

bool ParseColor3(std::string_view value, SceneColor3& result)
{
    std::istringstream stream{std::string(value)};
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    char separator = '\0';
    if (!(stream >> r))
    {
        return false;
    }
    if (!(stream >> separator) || separator != ',')
    {
        return false;
    }
    if (!(stream >> g))
    {
        return false;
    }
    if (!(stream >> separator) || separator != ',')
    {
        return false;
    }
    if (!(stream >> b))
    {
        return false;
    }

    result = {r, g, b};
    return true;
}

std::string FormatColor3(const SceneColor3& value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3)
        << value[0] << ", " << value[1] << ", " << value[2];
    return stream.str();
}

std::string FormatInteger(int value)
{
    return std::to_string(value);
}

bool ParseInteger(std::string_view value, int& result)
{
    const std::string trimmed = TrimCopy(std::string(value));
    if (trimmed.empty())
    {
        return false;
    }
    try
    {
        result = std::stoi(trimmed);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::string FormatScalar(float value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << value;
    return stream.str();
}

std::string FormatBool(bool value)
{
    return value ? "true" : "false";
}

bool ParseScalar(std::string_view value, float& result)
{
    std::istringstream stream{std::string(value)};
    return static_cast<bool>(stream >> result);
}

bool ParseBool(std::string_view value, bool& result)
{
    const std::string normalized = ToLowerCopy(TrimCopy(std::string(value)));
    if (normalized == "true" || normalized == "1" || normalized == "yes" || normalized == "on")
    {
        result = true;
        return true;
    }

    if (normalized == "false" || normalized == "0" || normalized == "no" || normalized == "off")
    {
        result = false;
        return true;
    }

    return false;
}

bool ParseUnsignedInteger(std::string_view value, std::uint32_t& result)
{
    std::istringstream stream{std::string(value)};
    std::uint32_t parsed = 0;
    if (!(stream >> parsed))
    {
        return false;
    }

    result = parsed;
    return true;
}

SceneObjectPhysicsShape ParseSceneObjectPhysicsShape(std::string_view value)
{
    const std::string normalized = ToLowerCopy(TrimCopy(std::string(value)));
    if (normalized == "box")
    {
        return SceneObjectPhysicsShape::Box;
    }

    if (normalized == "sphere")
    {
        return SceneObjectPhysicsShape::Sphere;
    }

    if (normalized == "capsule")
    {
        return SceneObjectPhysicsShape::Capsule;
    }

    if (normalized == "mesh")
    {
        return SceneObjectPhysicsShape::Mesh;
    }

    return SceneObjectPhysicsShape::None;
}

std::vector<std::string> ReadSceneLines(const std::filesystem::path& scene_path)
{
    if (g_asset_reader)
    {
        const auto buffer = g_asset_reader->ReadFile(scene_path.generic_string());
        if (!buffer.empty())
        {
            std::vector<std::string> lines;
            std::string current_line;
            for (unsigned char byte : buffer)
            {
                if (byte == '\n')
                {
                    lines.push_back(current_line);
                    current_line.clear();
                }
                else if (byte != '\r')
                {
                    current_line.push_back(static_cast<char>(byte));
                }
            }
            if (!current_line.empty())
            {
                lines.push_back(current_line);
            }
            return lines;
        }
    }

    std::ifstream input(scene_path, std::ios::binary);
    if (!input)
    {
        return {};
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
    {
        lines.push_back(line);
    }

    return lines;
}

bool WriteSceneLines(const std::filesystem::path& scene_path, const std::vector<std::string>& lines)
{
    std::ofstream output(scene_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }

    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        output << lines[index];
        if (index + 1 < lines.size())
        {
            output << '\n';
        }
    }

    return static_cast<bool>(output);
}

struct SceneObjectLineBlock
{
    std::string name;
    std::size_t start = 0;
    std::size_t end = 0;
};

std::vector<SceneObjectLineBlock> CollectSceneObjectLineBlocks(const std::vector<std::string>& lines)
{
    std::vector<SceneObjectLineBlock> blocks;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::string trimmed = TrimCopy(lines[index]);
        if (!IsSceneObjectStart(trimmed))
        {
            continue;
        }

        SceneObjectLineBlock block;
        block.name = ExtractValue(trimmed, "Object:");
        block.start = index;
        block.end = lines.size();
        for (std::size_t next_index = index + 1; next_index < lines.size(); ++next_index)
        {
            if (IsSceneObjectStart(TrimCopy(lines[next_index])))
            {
                block.end = next_index;
                break;
            }
        }

        blocks.push_back(block);
    }

    return blocks;
}

bool SceneObjectExists(const SceneMetadata& scene_metadata, const std::string& object_name)
{
    return std::any_of(scene_metadata.objects.begin(), scene_metadata.objects.end(), [&](const SceneObjectMetadata& object)
    {
        return object.name == object_name;
    });
}

void ResolveSceneObjectEnabledStateInternal(SceneMetadata& scene_metadata)
{
    std::unordered_map<std::string, std::size_t> object_indices;
    object_indices.reserve(scene_metadata.objects.size());
    for (std::size_t index = 0; index < scene_metadata.objects.size(); ++index)
    {
        if (!scene_metadata.objects[index].name.empty())
        {
            object_indices.emplace(scene_metadata.objects[index].name, index);
        }
    }

    std::vector<unsigned char> visit_state(scene_metadata.objects.size(), 0);
    const auto resolve_object = [&](auto&& self, std::size_t object_index) -> bool
    {
        SceneObjectMetadata& object = scene_metadata.objects[object_index];
        if (visit_state[object_index] == 2)
        {
            return object.enabled_in_hierarchy;
        }
        if (visit_state[object_index] == 1)
        {
            return object.enabled;
        }

        visit_state[object_index] = 1;
        bool parent_enabled = true;
        if (!object.parent_name.empty())
        {
            const auto parent_it = object_indices.find(object.parent_name);
            if (parent_it != object_indices.end())
            {
                parent_enabled = self(self, parent_it->second);
            }
        }

        object.enabled_in_hierarchy = object.enabled && parent_enabled;
        visit_state[object_index] = 2;
        return object.enabled_in_hierarchy;
    };

    for (std::size_t index = 0; index < scene_metadata.objects.size(); ++index)
    {
        resolve_object(resolve_object, index);
    }
}

std::string BuildUniqueSceneObjectName(const SceneMetadata& scene_metadata, const std::string& desired_name, const std::vector<std::string>& reserved_names = {})
{
    auto name_exists = [&](const std::string& candidate)
    {
        if (candidate.empty())
        {
            return true;
        }

        if (SceneObjectExists(scene_metadata, candidate))
        {
            return true;
        }

        return std::find(reserved_names.begin(), reserved_names.end(), candidate) != reserved_names.end();
    };

    if (!name_exists(desired_name))
    {
        return desired_name;
    }

    for (int suffix_index = 1; suffix_index < 10000; ++suffix_index)
    {
        const std::string candidate = desired_name + std::to_string(suffix_index);
        if (!name_exists(candidate))
        {
            return candidate;
        }
    }

    return {};
}

void CollectSceneObjectSubtreeNames(const SceneMetadata& scene_metadata, const std::string& root_name, std::vector<std::string>& names)
{
    names.push_back(root_name);
    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        if (object.parent_name == root_name)
        {
            CollectSceneObjectSubtreeNames(scene_metadata, object.name, names);
        }
    }
}

bool IsSceneObjectDescendant(const SceneMetadata& scene_metadata, const std::string& object_name, const std::string& potential_parent_name)
{
    std::string current_name = potential_parent_name;
    while (!current_name.empty())
    {
        if (current_name == object_name)
        {
            return true;
        }

        const auto object_it = std::find_if(scene_metadata.objects.begin(), scene_metadata.objects.end(), [&](const SceneObjectMetadata& object)
        {
            return object.name == current_name;
        });
        if (object_it == scene_metadata.objects.end())
        {
            break;
        }

        current_name = object_it->parent_name;
    }

    return false;
}

bool IsAttributePropertyLine(std::string_view line)
{
    return StartsWith(line, "AttributeColor:") ||
        StartsWith(line, "AttributeIntensity:") ||
        StartsWith(line, "AttributeRange:") ||
        StartsWith(line, "AttributeSourceRadius:") ||
        StartsWith(line, "AttributeHaloIntensity:") ||
        StartsWith(line, "AttributeHaloRadius:") ||
        StartsWith(line, "AttributeInnerCone:") ||
        StartsWith(line, "AttributeOuterCone:") ||
        StartsWith(line, "AttributeSpotVolumetric:") ||
        StartsWith(line, "AttributeSpotVolumetricIntensity:") ||
        StartsWith(line, "AttributeFov:") ||
        StartsWith(line, "AttributeNearClip:") ||
    StartsWith(line, "AttributeFarClip:") ||
    StartsWith(line, "AttributeActive:") ||
    StartsWith(line, "AttributeCameraType:") ||
    StartsWith(line, "AttributeCameraFollowTarget:") ||
    StartsWith(line, "AttributeCameraFollowOffset:") ||
    StartsWith(line, "AttributeCameraFollowOrbit:") ||
    StartsWith(line, "AttributeCameraFollowRotationOffset:") ||
    StartsWith(line, "AttributeCameraFollowLockPosition:") ||
    StartsWith(line, "AttributeCameraFollowSmoothing:") ||
    StartsWith(line, "AttributeCameraTrackPoints:") ||
    StartsWith(line, "AttributeCameraTrackSpeed:") ||
    StartsWith(line, "AttributeCameraTrackAcceleration:") ||
    StartsWith(line, "AttributeCameraTrackRotationOffset:") ||
    StartsWith(line, "AttributePhysicsShape:") ||
    StartsWith(line, "AttributePhysicsDynamic:") ||
    StartsWith(line, "AttributePhysicsLockRotationX:") ||
    StartsWith(line, "AttributePhysicsLockRotationY:") ||
    StartsWith(line, "AttributePhysicsLockRotationZ:") ||
    StartsWith(line, "AttributePhysicsMass:") ||
    StartsWith(line, "AttributePhysicsFriction:") ||
    StartsWith(line, "AttributePhysicsRadius:") ||
    StartsWith(line, "AttributePhysicsCapsuleHalfHeight:") ||
    StartsWith(line, "AttributePhysicsHalfExtent:") ||
    StartsWith(line, "AttributePhysicsLinearDamping:") ||
    StartsWith(line, "AttributePhysicsAngularDamping:") ||
    StartsWith(line, "AttributeTriggerHalfExtent:") ||
    StartsWith(line, "AttributeAnimatorControllerPath:") ||
    StartsWith(line, "AttributeAnimatorInitialState:") ||
    StartsWith(line, "AttributeAnimatorPlaybackSpeed:") ||
    StartsWith(line, "AttributeAnimatorAutoPlay:") ||
    StartsWith(line, "AttributeText2DFontPath:") ||
    StartsWith(line, "AttributeText2DText:") ||
    StartsWith(line, "AttributeText2DX:") ||
    StartsWith(line, "AttributeText2DY:") ||
    StartsWith(line, "AttributeText2DWidth:") ||
    StartsWith(line, "AttributeText2DHeight:") ||
    StartsWith(line, "AttributeText2DFontSize:") ||
    StartsWith(line, "AttributeText2DColor:") ||
    StartsWith(line, "AttributeText2DAlpha:") ||
    StartsWith(line, "AttributeText2DLockAspectRatio:") ||
    StartsWith(line, "AttributeText2DPriority:") ||
    StartsWith(line, "AttributeImage2DImagePath:") ||
    StartsWith(line, "AttributeImage2DX:") ||
    StartsWith(line, "AttributeImage2DY:") ||
    StartsWith(line, "AttributeImage2DWidth:") ||
    StartsWith(line, "AttributeImage2DHeight:") ||
    StartsWith(line, "AttributeImage2DTint:") ||
    StartsWith(line, "AttributeImage2DAlpha:") ||
    StartsWith(line, "AttributeImage2DLockAspectRatio:") ||
    StartsWith(line, "AttributeImage2DStretchToScreen:") ||
    StartsWith(line, "AttributeImage2DPlayMode:") ||
    StartsWith(line, "AttributeImage2DPriority:") ||
    StartsWith(line, "AttributeColor2DX:") ||
    StartsWith(line, "AttributeColor2DY:") ||
    StartsWith(line, "AttributeColor2DWidth:") ||
    StartsWith(line, "AttributeColor2DHeight:") ||
    StartsWith(line, "AttributeColor2DColor:") ||
    StartsWith(line, "AttributeColor2DAlpha:") ||
    StartsWith(line, "AttributeColor2DLockAspectRatio:") ||
    StartsWith(line, "AttributeColor2DStretchToScreen:") ||
    StartsWith(line, "AttributeColor2DPriority:") ||
    StartsWith(line, "AttributeSkyboxImagePath:") ||
    StartsWith(line, "AttributeSkyboxRotation:") ||
    StartsWith(line, "AttributeAudioClipPath:") ||
    StartsWith(line, "AttributeAudioPlayMode:") ||
    StartsWith(line, "AttributeAudioVolume:") ||
    StartsWith(line, "AttributeAudioPitch:") ||
    StartsWith(line, "AttributeAudioLoop:") ||
    StartsWith(line, "AttributeAudioSpatialize:") ||
    StartsWith(line, "AttributeAudioMinDistance:") ||
    StartsWith(line, "AttributeAudioMaxDistance:") ||
    StartsWith(line, "AttributeAudioDopplerFactor:") ||
    StartsWith(line, "AttributeVideo2DVideoPath:") ||
    StartsWith(line, "AttributeVideo2DX:") ||
    StartsWith(line, "AttributeVideo2DY:") ||
    StartsWith(line, "AttributeVideo2DWidth:") ||
    StartsWith(line, "AttributeVideo2DHeight:") ||
    StartsWith(line, "AttributeVideo2DTint:") ||
    StartsWith(line, "AttributeVideo2DAlpha:") ||
    StartsWith(line, "AttributeVideo2DLockAspectRatio:") ||
    StartsWith(line, "AttributeVideo2DStretchToScreen:") ||
    StartsWith(line, "AttributeVideo2DPriority:") ||
    StartsWith(line, "AttributeVideo2DPlayMode:") ||
    StartsWith(line, "AttributeVideo2DVolume:") ||
    StartsWith(line, "AttributeVideo2DMuted:") ||
    StartsWith(line, "AttributeEffectPath:") ||
    StartsWith(line, "AttributeEffectPlayMode:") ||
    StartsWith(line, "AttributeShaderType:") ||
    StartsWith(line, "AttributeShaderDropScale:") ||
    StartsWith(line, "AttributeShaderDropSpeed:") ||
    StartsWith(line, "AttributeShaderColor:") ||
    StartsWith(line, "AttributeShaderFogDensity:") ||
    StartsWith(line, "AttributeShape3DPath:");
}

bool IsAttributeLine(std::string_view line)
{
    return StartsWith(line, "Attributes:") || IsAttributePropertyLine(line);
}

std::size_t FindSceneObjectAttributeInsertionIndex(const std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
{
    for (std::size_t index = object_start + 1; index < object_end; ++index)
    {
        const std::string trimmed = TrimCopy(lines[index]);
        if (StartsWith(trimmed, "Model:") || StartsWith(trimmed, "Script:") || StartsWith(trimmed, "Graph:"))
        {
            return index;
        }
    }

    return object_end;
}

bool FindSceneObjectAttributeBlock(
    const std::vector<std::string>& lines,
    std::size_t object_start,
    std::size_t object_end,
    std::size_t attribute_index,
    std::size_t& attribute_start,
    std::size_t& attribute_end)
{
    std::size_t current_attribute_index = 0;
    for (std::size_t index = object_start + 1; index < object_end; ++index)
    {
        if (!StartsWith(TrimCopy(lines[index]), "Attributes:"))
        {
            continue;
        }

        if (current_attribute_index == attribute_index)
        {
            attribute_start = index;
            attribute_end = index + 1;
            while (attribute_end < object_end && IsAttributePropertyLine(TrimCopy(lines[attribute_end])))
            {
                ++attribute_end;
            }
            return true;
        }

        ++current_attribute_index;
    }

    return false;
}

bool SetSceneObjectAttributeScalar(
    std::string_view key,
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    std::size_t attribute_index,
    float value)
{
    bool updated = false;
    const bool rewrite_succeeded = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        const std::string key_prefix = std::string(key) + ":";
        const std::string new_line = std::string(key) + ": " + FormatScalar(value);
        for (std::size_t index = attribute_start + 1; index < attribute_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                updated = true;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(attribute_end), new_line);
        updated = true;
    });

    return rewrite_succeeded && updated;
}

bool SetSceneObjectAttributeBoolean(
    std::string_view key,
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    std::size_t attribute_index,
    bool value)
{
    bool updated = false;
    const bool rewrite_succeeded = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        const std::string key_prefix = std::string(key) + ":";
        const std::string new_line = std::string(key) + ": " + FormatBool(value);
        for (std::size_t index = attribute_start + 1; index < attribute_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                updated = true;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(attribute_end), new_line);
        updated = true;
    });

    return rewrite_succeeded && updated;
}

bool SetSceneObjectAttributeColorValue(
    std::string_view key,
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    std::size_t attribute_index,
    const SceneColor3& color)
{
    bool updated = false;
    const bool rewrite_succeeded = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        const std::string key_prefix = std::string(key) + ":";
        const std::string new_line = std::string(key) + ": " + FormatColor3(color);
        for (std::size_t index = attribute_start + 1; index < attribute_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                updated = true;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(attribute_end), new_line);
        updated = true;
    });

    return rewrite_succeeded && updated;
}

std::string MakeRelativePath(const std::filesystem::path& project_root, const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(path, project_root, error);
    return error ? path.generic_string() : relative.generic_string();
}

bool UpdateSceneObjectAttachment(
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    const std::filesystem::path& project_root,
    const std::filesystem::path& attachment_path,
    std::string_view key)
{
    std::vector<std::string> lines = ReadSceneLines(scene_path);
    if (lines.empty() && !std::filesystem::exists(scene_path))
    {
        return false;
    }

    const std::string object_header = "Object: " + object_name;
    const std::string key_prefix = std::string(key) + ":";
    const std::string new_line = std::string(key) + ": " + MakeRelativePath(project_root, attachment_path);

    bool inside_target_object = false;
    bool updated_existing_line = false;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::string trimmed = TrimCopy(lines[index]);
        if (IsSceneObjectStart(trimmed))
        {
            if (inside_target_object && !updated_existing_line)
            {
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index), new_line);
                updated_existing_line = true;
                break;
            }

            inside_target_object = trimmed == object_header;
            continue;
        }

        if (!inside_target_object)
        {
            continue;
        }

        if (StartsWith(trimmed, key_prefix))
        {
            lines[index] = new_line;
            updated_existing_line = true;
            break;
        }
    }

    if (inside_target_object && !updated_existing_line)
    {
        lines.push_back(new_line);
        updated_existing_line = true;
    }

    if (!updated_existing_line)
    {
        return false;
    }

    return WriteSceneLines(scene_path, lines);
}

bool RewriteSceneObjectLines(
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    const std::function<void(std::vector<std::string>&, std::size_t, std::size_t)>& mutator)
{
    std::vector<std::string> lines = ReadSceneLines(scene_path);
    if (lines.empty() && !std::filesystem::exists(scene_path))
    {
        return false;
    }

    const std::string object_header = "Object: " + object_name;
    std::size_t object_start = lines.size();
    std::size_t object_end = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::string trimmed = TrimCopy(lines[index]);
        if (!IsSceneObjectStart(trimmed))
        {
            continue;
        }

        if (trimmed == object_header)
        {
            object_start = index;
            object_end = lines.size();
            for (std::size_t child_index = index + 1; child_index < lines.size(); ++child_index)
            {
                if (IsSceneObjectStart(TrimCopy(lines[child_index])))
                {
                    object_end = child_index;
                    break;
                }
            }
            break;
        }
    }

    if (object_start == lines.size())
    {
        return false;
    }

    mutator(lines, object_start, object_end);

    return WriteSceneLines(scene_path, lines);
}

bool SetSceneObjectVector3(const std::filesystem::path& scene_path, const std::string& object_name, std::string_view key, const SceneVector3& value)
{
    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        const std::string key_prefix = std::string(key) + ":";
        const std::string new_line = std::string(key) + ": " + FormatVector3(value);
        for (std::size_t index = object_start + 1; index < object_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(object_end), new_line);
    });
}

bool SetSceneObjectScalar(const std::filesystem::path& scene_path, const std::string& object_name, std::string_view key, float value)
{
    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        const std::string key_prefix = std::string(key) + ":";
        const std::string new_line = std::string(key) + ": " + FormatScalar(value);
        for (std::size_t index = object_start + 1; index < object_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(object_end), new_line);
    });
}

bool SetSceneObjectBoolean(const std::filesystem::path& scene_path, const std::string& object_name, std::string_view key, bool value)
{
    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        const std::string key_prefix = std::string(key) + ":";
        const std::string new_line = std::string(key) + ": " + FormatBool(value);
        for (std::size_t index = object_start + 1; index < object_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(object_end), new_line);
    });
}

// ---- Hierarchy transform helpers ----------------------------------------------------
// These mirror the matrix conventions used by the viewport/runtime renderers
// (column-major, transform = T * Rz * Ry * Rx * S, rotations in degrees, XYZ Euler).

void Mat4Identity(float* m)
{
    std::fill(m, m + 16, 0.0f);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void Mat4Multiply(const float* left, const float* right, float* out)
{
    float tmp[16];
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            float v = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                v += left[i * 4 + row] * right[col * 4 + i];
            }
            tmp[col * 4 + row] = v;
        }
    }
    std::memcpy(out, tmp, sizeof(tmp));
}

bool Mat4Invert(const float* m, float* out)
{
    float inv[16];
    inv[0]  =  m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4]  = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8]  =  m[4] * m[9]  * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9]  * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1]  = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5]  =  m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9]  = -m[0] * m[9]  * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] =  m[0] * m[9]  * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2]  =  m[1] * m[6]  * m[15] - m[1] * m[7]  * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7]  - m[13] * m[3] * m[6];
    inv[6]  = -m[0] * m[6]  * m[15] + m[0] * m[7]  * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7]  + m[12] * m[3] * m[6];
    inv[10] =  m[0] * m[5]  * m[15] - m[0] * m[7]  * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7]  - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5]  * m[14] + m[0] * m[6]  * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6]  + m[12] * m[2] * m[5];
    inv[3]  = -m[1] * m[6]  * m[11] + m[1] * m[7]  * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9]  * m[2] * m[7]  + m[9]  * m[3] * m[6];
    inv[7]  =  m[0] * m[6]  * m[11] - m[0] * m[7]  * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8]  * m[2] * m[7]  - m[8]  * m[3] * m[6];
    inv[11] = -m[0] * m[5]  * m[11] + m[0] * m[7]  * m[9]  + m[4] * m[1] * m[11] - m[4] * m[3] * m[9]  - m[8]  * m[1] * m[7]  + m[8]  * m[3] * m[5];
    inv[15] =  m[0] * m[5]  * m[10] - m[0] * m[6]  * m[9]  - m[4] * m[1] * m[10] + m[4] * m[2] * m[9]  + m[8]  * m[1] * m[6]  - m[8]  * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::abs(det) < 1e-12f)
    {
        Mat4Identity(out);
        return false;
    }
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i)
    {
        out[i] = inv[i] * det;
    }
    return true;
}

void BuildLocalTransformMatrix(const SceneVector3& position, const SceneVector3& rotation_degrees, const SceneVector3& scale, float* out)
{
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    const float cx = std::cos(rotation_degrees[0] * kDegToRad);
    const float sx = std::sin(rotation_degrees[0] * kDegToRad);
    const float cy = std::cos(rotation_degrees[1] * kDegToRad);
    const float sy = std::sin(rotation_degrees[1] * kDegToRad);
    const float cz = std::cos(rotation_degrees[2] * kDegToRad);
    const float sz = std::sin(rotation_degrees[2] * kDegToRad);

    // R = Rz * Ry * Rx, column-major. Expanded coefficients:
    const float r00 =  cz * cy;
    const float r10 =  sz * cy;
    const float r20 = -sy;
    const float r01 = -sz * cx + cz * sy * sx;
    const float r11 =  cz * cx + sz * sy * sx;
    const float r21 =  cy * sx;
    const float r02 =  sz * sx + cz * sy * cx;
    const float r12 = -cz * sx + sz * sy * cx;
    const float r22 =  cy * cx;

    out[0]  = r00 * scale[0];
    out[1]  = r10 * scale[0];
    out[2]  = r20 * scale[0];
    out[3]  = 0.0f;
    out[4]  = r01 * scale[1];
    out[5]  = r11 * scale[1];
    out[6]  = r21 * scale[1];
    out[7]  = 0.0f;
    out[8]  = r02 * scale[2];
    out[9]  = r12 * scale[2];
    out[10] = r22 * scale[2];
    out[11] = 0.0f;
    out[12] = position[0];
    out[13] = position[1];
    out[14] = position[2];
    out[15] = 1.0f;
}

void DecomposeLocalTransformMatrix(const float* m, SceneVector3& position, SceneVector3& rotation_degrees, SceneVector3& scale)
{
    position = {m[12], m[13], m[14]};

    float sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2]  * m[2]);
    float sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6]  * m[6]);
    float sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);

    const float det3 =
        m[0] * (m[5] * m[10] - m[9] * m[6]) -
        m[4] * (m[1] * m[10] - m[9] * m[2]) +
        m[8] * (m[1] * m[6]  - m[5] * m[2]);
    if (det3 < 0.0f)
    {
        sx = -sx;
    }

    const float inv_sx = std::abs(sx) > 1e-8f ? 1.0f / sx : 0.0f;
    const float inv_sy = std::abs(sy) > 1e-8f ? 1.0f / sy : 0.0f;
    const float inv_sz = std::abs(sz) > 1e-8f ? 1.0f / sz : 0.0f;

    const float r00 = m[0]  * inv_sx;
    const float r10 = m[1]  * inv_sx;
    const float r20 = m[2]  * inv_sx;
    const float r11 = m[5]  * inv_sy;
    const float r21 = m[6]  * inv_sy;
    const float r12 = m[9]  * inv_sz;
    const float r22 = m[10] * inv_sz;

    // From R = Rz*Ry*Rx: r20 = -sin(ry).
    constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;
    const float clamped = std::clamp(-r20, -1.0f, 1.0f);
    const float ry = std::asin(clamped);
    float rx;
    float rz;
    if (std::abs(r20) < 0.99999f)
    {
        rx = std::atan2(r21, r22);
        rz = std::atan2(r10, r00);
    }
    else
    {
        // Gimbal lock: roll rolled into yaw; pick rz = 0.
        rx = std::atan2(-r12, r11);
        rz = 0.0f;
    }

    rotation_degrees = {rx * kRadToDeg, ry * kRadToDeg, rz * kRadToDeg};
    scale = {sx, sy, sz};
}

void ResolveObjectWorldMatrix(const SceneMetadata& scene_metadata, const std::string& object_name, float* out)
{
    Mat4Identity(out);
    if (object_name.empty())
    {
        return;
    }

    std::vector<const SceneObjectMetadata*> chain;
    std::unordered_set<std::string> visited;
    std::string current = object_name;
    while (!current.empty() && visited.insert(current).second)
    {
        const auto it = std::find_if(scene_metadata.objects.begin(), scene_metadata.objects.end(),
            [&](const SceneObjectMetadata& object) { return object.name == current; });
        if (it == scene_metadata.objects.end())
        {
            break;
        }
        chain.push_back(&(*it));
        current = it->parent_name;
    }

    // chain.front() is the object itself, chain.back() is the (deepest) ancestor.
    // Accumulate world = ancestor_local * ... * self_local (apply parents first).
    float accum[16];
    Mat4Identity(accum);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
        float local[16];
        BuildLocalTransformMatrix((*it)->position, (*it)->rotation, (*it)->scale, local);
        float next[16];
        Mat4Multiply(accum, local, next);
        std::memcpy(accum, next, sizeof(next));
    }
    std::memcpy(out, accum, sizeof(accum));
}
}

const char* ToDisplayName(SceneObjectAttributeKind kind)
{
    switch (kind)
    {
    case SceneObjectAttributeKind::Model:
        return "Model";
    case SceneObjectAttributeKind::Script:
        return "Script";
    case SceneObjectAttributeKind::Graph:
        return "Graph";
    case SceneObjectAttributeKind::Shape3D:
        return "3D Shape";
    case SceneObjectAttributeKind::EnvironmentLight:
        return "Environment Light";
    case SceneObjectAttributeKind::DirectionalLight:
        return "Directional Light";
    case SceneObjectAttributeKind::PointLight:
        return "Point Light";
    case SceneObjectAttributeKind::SpotLight:
        return "Spot Light";
    case SceneObjectAttributeKind::Camera:
        return "Camera";
    case SceneObjectAttributeKind::Rigidbody:
        return "Rigidbody";
    case SceneObjectAttributeKind::TriggerVolume:
        return "Trigger Volume";
    case SceneObjectAttributeKind::Animator:
        return "Animator";
    case SceneObjectAttributeKind::Text2D:
        return "Text 2D";
    case SceneObjectAttributeKind::Image2D:
        return "Image 2D";
    case SceneObjectAttributeKind::Color2D:
        return "Color 2D";
    case SceneObjectAttributeKind::Skybox:
        return "Skybox";
    case SceneObjectAttributeKind::Audio:
        return "Audio";
    case SceneObjectAttributeKind::Video2D:
        return "Video 2D";
    case SceneObjectAttributeKind::Effects:
        return "Effects";
    case SceneObjectAttributeKind::Shader:
        return "Shader";
    case SceneObjectAttributeKind::None:
    default:
        return "None";
    }
}

const char* ToStorageName(SceneObjectAttributeKind kind)
{
    switch (kind)
    {
    case SceneObjectAttributeKind::Model:
        return "Model";
    case SceneObjectAttributeKind::Script:
        return "Script";
    case SceneObjectAttributeKind::Graph:
        return "Graph";
    case SceneObjectAttributeKind::Shape3D:
        return "Shape3D";
    case SceneObjectAttributeKind::EnvironmentLight:
        return "EnvironmentLight";
    case SceneObjectAttributeKind::DirectionalLight:
        return "DirectionalLight";
    case SceneObjectAttributeKind::PointLight:
        return "PointLight";
    case SceneObjectAttributeKind::SpotLight:
        return "SpotLight";
    case SceneObjectAttributeKind::Camera:
        return "Camera";
    case SceneObjectAttributeKind::Rigidbody:
        return "Rigidbody";
    case SceneObjectAttributeKind::TriggerVolume:
        return "TriggerVolume";
    case SceneObjectAttributeKind::Animator:
        return "Animator";
    case SceneObjectAttributeKind::Text2D:
        return "Text2D";
    case SceneObjectAttributeKind::Image2D:
        return "Image2D";
    case SceneObjectAttributeKind::Color2D:
        return "Color2D";
    case SceneObjectAttributeKind::Skybox:
        return "Skybox";
    case SceneObjectAttributeKind::Audio:
        return "Audio";
    case SceneObjectAttributeKind::Video2D:
        return "Video2D";
    case SceneObjectAttributeKind::Effects:
        return "Effects";
    case SceneObjectAttributeKind::Shader:
        return "Shader";
    case SceneObjectAttributeKind::None:
    default:
        return "None";
    }
}

SceneObjectAttributeKind ParseSceneObjectAttributeKind(std::string_view value)
{
    const std::string trimmed = TrimCopy(std::string(value));
    if (trimmed == "Model")
    {
        return SceneObjectAttributeKind::Model;
    }
    if (trimmed == "Script")
    {
        return SceneObjectAttributeKind::Script;
    }
    if (trimmed == "Graph")
    {
        return SceneObjectAttributeKind::Graph;
    }
    if (trimmed == "Shape3D")
    {
        return SceneObjectAttributeKind::Shape3D;
    }
    if (trimmed == "EnvironmentLight")
    {
        return SceneObjectAttributeKind::EnvironmentLight;
    }
    if (trimmed == "DirectionalLight")
    {
        return SceneObjectAttributeKind::DirectionalLight;
    }
    if (trimmed == "PointLight")
    {
        return SceneObjectAttributeKind::PointLight;
    }
    if (trimmed == "SpotLight")
    {
        return SceneObjectAttributeKind::SpotLight;
    }
    if (trimmed == "Camera")
    {
        return SceneObjectAttributeKind::Camera;
    }
    if (trimmed == "Rigidbody")
    {
        return SceneObjectAttributeKind::Rigidbody;
    }
    if (trimmed == "TriggerVolume")
    {
        return SceneObjectAttributeKind::TriggerVolume;
    }
    if (trimmed == "Animator")
    {
        return SceneObjectAttributeKind::Animator;
    }
    if (trimmed == "Text2D")
    {
        return SceneObjectAttributeKind::Text2D;
    }
    if (trimmed == "Image2D")
    {
        return SceneObjectAttributeKind::Image2D;
    }
    if (trimmed == "Color2D")
    {
        return SceneObjectAttributeKind::Color2D;
    }
    if (trimmed == "Skybox")
    {
        return SceneObjectAttributeKind::Skybox;
    }
    if (trimmed == "Audio")
    {
        return SceneObjectAttributeKind::Audio;
    }
    if (trimmed == "Video2D")
    {
        return SceneObjectAttributeKind::Video2D;
    }
    if (trimmed == "Effects")
    {
        return SceneObjectAttributeKind::Effects;
    }
    if (trimmed == "Shader")
    {
        return SceneObjectAttributeKind::Shader;
    }

    return SceneObjectAttributeKind::None;
}

SceneObjectAttribute MakeDefaultSceneObjectAttribute(SceneObjectAttributeKind kind)
{
    SceneObjectAttribute attribute;
    attribute.kind = kind;
    return attribute;
}

bool RenameSceneObject(const std::filesystem::path& scene_path, const std::string& object_name, const std::string& new_name)
{
    if (object_name.empty() || new_name.empty() || object_name == new_name)
    {
        return false;
    }

    const SceneMetadata scene_metadata = LoadSceneMetadata(scene_path);
    if (!scene_metadata.parsed || !SceneObjectExists(scene_metadata, object_name) || SceneObjectExists(scene_metadata, new_name))
    {
        return false;
    }

    std::vector<std::string> lines = ReadSceneLines(scene_path);
    if (lines.empty() && !std::filesystem::exists(scene_path))
    {
        return false;
    }

    for (std::string& line : lines)
    {
        const std::string trimmed = TrimCopy(line);
        if (trimmed == "Object: " + object_name)
        {
            line = "Object: " + new_name;
        }
        else if (trimmed == "Parent: " + object_name)
        {
            line = "Parent: " + new_name;
        }
    }

    return WriteSceneLines(scene_path, lines);
}

bool DuplicateSceneObject(const std::filesystem::path& scene_path, const std::string& object_name, std::string* duplicated_root_name)
{
    const SceneMetadata scene_metadata = LoadSceneMetadata(scene_path);
    if (!scene_metadata.parsed || !SceneObjectExists(scene_metadata, object_name))
    {
        return false;
    }

    std::vector<std::string> subtree_names;
    CollectSceneObjectSubtreeNames(scene_metadata, object_name, subtree_names);
    if (subtree_names.empty())
    {
        return false;
    }

    std::vector<std::string> lines = ReadSceneLines(scene_path);
    if (lines.empty() && !std::filesystem::exists(scene_path))
    {
        return false;
    }

    const std::vector<SceneObjectLineBlock> blocks = CollectSceneObjectLineBlocks(lines);
    std::vector<std::string> reserved_names;
    std::vector<std::pair<std::string, std::string>> name_mapping;
    name_mapping.reserve(subtree_names.size());
    for (const std::string& source_name : subtree_names)
    {
        const std::string base_name = source_name == object_name ? source_name + "_Copy" : source_name + "_Copy";
        const std::string duplicated_name = BuildUniqueSceneObjectName(scene_metadata, base_name, reserved_names);
        if (duplicated_name.empty())
        {
            return false;
        }

        reserved_names.push_back(duplicated_name);
        name_mapping.emplace_back(source_name, duplicated_name);
    }

    auto map_name = [&](const std::string& source_name) -> std::string
    {
        const auto mapping_it = std::find_if(name_mapping.begin(), name_mapping.end(), [&](const auto& entry)
        {
            return entry.first == source_name;
        });
        return mapping_it != name_mapping.end() ? mapping_it->second : source_name;
    };

    for (const std::string& source_name : subtree_names)
    {
        const auto block_it = std::find_if(blocks.begin(), blocks.end(), [&](const SceneObjectLineBlock& block)
        {
            return block.name == source_name;
        });
        if (block_it == blocks.end())
        {
            return false;
        }

        if (!lines.empty() && !TrimCopy(lines.back()).empty())
        {
            lines.push_back(std::string());
        }

        const std::string duplicated_name = map_name(source_name);
        for (std::size_t index = block_it->start; index < block_it->end; ++index)
        {
            std::string copied_line = lines[index];
            const std::string trimmed = TrimCopy(copied_line);
            if (trimmed == "Object: " + source_name)
            {
                copied_line = "Object: " + duplicated_name;
            }
            else if (StartsWith(trimmed, "Parent:"))
            {
                const std::string parent_name = ExtractValue(trimmed, "Parent:");
                copied_line = parent_name.empty() ? copied_line : "Parent: " + map_name(parent_name);
            }
            else if (StartsWith(trimmed, "AttributeActive:"))
            {
                copied_line = "AttributeActive: false";
            }

            lines.push_back(copied_line);
        }
    }

    if (duplicated_root_name != nullptr)
    {
        *duplicated_root_name = map_name(object_name);
    }

    return WriteSceneLines(scene_path, lines);
}

bool DeleteSceneObject(const std::filesystem::path& scene_path, const std::string& object_name)
{
    const SceneMetadata scene_metadata = LoadSceneMetadata(scene_path);
    if (!scene_metadata.parsed || !SceneObjectExists(scene_metadata, object_name))
    {
        return false;
    }

    std::vector<std::string> subtree_names;
    CollectSceneObjectSubtreeNames(scene_metadata, object_name, subtree_names);
    if (subtree_names.empty())
    {
        return false;
    }

    std::vector<std::string> lines = ReadSceneLines(scene_path);
    if (lines.empty() && !std::filesystem::exists(scene_path))
    {
        return false;
    }

    const std::vector<SceneObjectLineBlock> blocks = CollectSceneObjectLineBlocks(lines);
    std::vector<std::string> rewritten_lines;
    std::size_t block_index = 0;
    for (std::size_t line_index = 0; line_index < lines.size();)
    {
        if (block_index < blocks.size() && line_index == blocks[block_index].start)
        {
            const bool remove_block = std::find(subtree_names.begin(), subtree_names.end(), blocks[block_index].name) != subtree_names.end();
            if (!remove_block)
            {
                for (std::size_t index = blocks[block_index].start; index < blocks[block_index].end; ++index)
                {
                    rewritten_lines.push_back(lines[index]);
                }
            }

            line_index = blocks[block_index].end;
            ++block_index;
            continue;
        }

        rewritten_lines.push_back(lines[line_index]);
        ++line_index;
    }

    while (!rewritten_lines.empty() && TrimCopy(rewritten_lines.back()).empty())
    {
        rewritten_lines.pop_back();
    }

    return WriteSceneLines(scene_path, rewritten_lines);
}

bool SetSceneObjectParent(const std::filesystem::path& scene_path, const std::string& object_name, const std::string& parent_name)
{
    if (object_name.empty() || object_name == parent_name)
    {
        return false;
    }

    const SceneMetadata scene_metadata = LoadSceneMetadata(scene_path);
    if (!scene_metadata.parsed || !SceneObjectExists(scene_metadata, object_name))
    {
        return false;
    }
    if (!parent_name.empty() && (!SceneObjectExists(scene_metadata, parent_name) || IsSceneObjectDescendant(scene_metadata, object_name, parent_name)))
    {
        return false;
    }

    // Locate the object so we can preserve its current world transform across the
    // reparent. Without this, a child's existing position/rotation/scale would be
    // re-interpreted in the new parent's space and the child would visibly jump.
    const auto object_it = std::find_if(scene_metadata.objects.begin(), scene_metadata.objects.end(),
        [&](const SceneObjectMetadata& object) { return object.name == object_name; });
    const std::string previous_parent_name = object_it != scene_metadata.objects.end() ? object_it->parent_name : std::string{};

    SceneVector3 new_local_position = object_it != scene_metadata.objects.end() ? object_it->position : SceneVector3{0.0f, 0.0f, 0.0f};
    SceneVector3 new_local_rotation = object_it != scene_metadata.objects.end() ? object_it->rotation : SceneVector3{0.0f, 0.0f, 0.0f};
    SceneVector3 new_local_scale = object_it != scene_metadata.objects.end() ? object_it->scale : SceneVector3{1.0f, 1.0f, 1.0f};
    const bool parent_changed = previous_parent_name != parent_name;

    if (parent_changed && object_it != scene_metadata.objects.end())
    {
        float object_world[16];
        ResolveObjectWorldMatrix(scene_metadata, object_name, object_world);

        float new_parent_world[16];
        if (parent_name.empty())
        {
            Mat4Identity(new_parent_world);
        }
        else
        {
            ResolveObjectWorldMatrix(scene_metadata, parent_name, new_parent_world);
        }

        float inverse_new_parent_world[16];
        if (Mat4Invert(new_parent_world, inverse_new_parent_world))
        {
            float new_local_matrix[16];
            Mat4Multiply(inverse_new_parent_world, object_world, new_local_matrix);
            DecomposeLocalTransformMatrix(new_local_matrix, new_local_position, new_local_rotation, new_local_scale);
        }
    }

    const bool parent_written = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t existing_parent_index = object_end;
        for (std::size_t index = object_start + 1; index < object_end; ++index)
        {
            const std::string trimmed = TrimCopy(lines[index]);
            if (StartsWith(trimmed, "Parent:"))
            {
                existing_parent_index = index;
                break;
            }

            if (StartsWith(trimmed, "Position:") || StartsWith(trimmed, "Rotation:") || StartsWith(trimmed, "Scale:") || StartsWith(trimmed, "PhysicsShape:") || StartsWith(trimmed, "PhysicsDynamic:") || StartsWith(trimmed, "PhysicsMass:") || StartsWith(trimmed, "PhysicsFriction:") || StartsWith(trimmed, "PhysicsRadius:") || StartsWith(trimmed, "PhysicsHalfExtent:") || StartsWith(trimmed, "PhysicsLinearDamping:") || StartsWith(trimmed, "PhysicsAngularDamping:") || StartsWith(trimmed, "Attributes:") || StartsWith(trimmed, "Model:") || StartsWith(trimmed, "ModelVisualOffset:") || StartsWith(trimmed, "Script:") || StartsWith(trimmed, "Graph:"))
            {
                existing_parent_index = index;
                break;
            }
        }

        if (parent_name.empty())
        {
            if (existing_parent_index < object_end && StartsWith(TrimCopy(lines[existing_parent_index]), "Parent:"))
            {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(existing_parent_index));
            }
            return;
        }

        const std::string new_line = "Parent: " + parent_name;
        if (existing_parent_index < object_end && StartsWith(TrimCopy(lines[existing_parent_index]), "Parent:"))
        {
            lines[existing_parent_index] = new_line;
            return;
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(existing_parent_index), new_line);
    });

    if (!parent_written)
    {
        return false;
    }

    if (parent_changed)
    {
        SetSceneObjectTransform(
            scene_path,
            object_name,
            new_local_position,
            new_local_rotation,
            new_local_scale,
            true,
            true,
            true);
    }

    return true;
}

SceneMetadata LoadSceneMetadata(const std::filesystem::path& scene_path)
{
    SceneMetadata metadata;

    const std::vector<std::string> lines = ReadSceneLines(scene_path);
    const bool exists_in_vfs = g_asset_reader && g_asset_reader->FileExists(scene_path.generic_string());
    if (lines.empty() && !exists_in_vfs && !std::filesystem::exists(scene_path))
    {
        metadata.error_message = "Failed to open scene file.";
        return metadata;
    }

    SceneObjectMetadata* current_object = nullptr;
    SceneObjectAttribute* current_attribute = nullptr;
    for (const std::string& line : lines)
    {
        const std::string trimmed = TrimCopy(line);
        if (trimmed.empty())
        {
            continue;
        }

        if (StartsWith(trimmed, "Scene:"))
        {
            metadata.scene_name = ExtractValue(trimmed, "Scene:");
            current_object = nullptr;
            current_attribute = nullptr;
            continue;
        }

        if (StartsWith(trimmed, "ReferenceViewportWidth:"))
        {
            ParseUnsignedInteger(ExtractValue(trimmed, "ReferenceViewportWidth:"), metadata.reference_viewport_width);
            continue;
        }

        if (StartsWith(trimmed, "ReferenceViewportHeight:"))
        {
            ParseUnsignedInteger(ExtractValue(trimmed, "ReferenceViewportHeight:"), metadata.reference_viewport_height);
            continue;
        }

        if (StartsWith(trimmed, "Object:"))
        {
            SceneObjectMetadata object;
            object.name = ExtractValue(trimmed, "Object:");
            metadata.objects.push_back(std::move(object));
            current_object = &metadata.objects.back();
            current_attribute = nullptr;
            continue;
        }

        if (current_object == nullptr)
        {
            continue;
        }

        if (StartsWith(trimmed, "Parent:"))
        {
            current_object->parent_name = ExtractValue(trimmed, "Parent:");
        }
        else if (StartsWith(trimmed, "Enabled:"))
        {
            ParseBool(ExtractValue(trimmed, "Enabled:"), current_object->enabled);
        }
        else if (StartsWith(trimmed, "Position:"))
        {
            ParseVector3(ExtractValue(trimmed, "Position:"), current_object->position);
        }
        else if (StartsWith(trimmed, "Rotation:"))
        {
            ParseVector3(ExtractValue(trimmed, "Rotation:"), current_object->rotation);
        }
        else if (StartsWith(trimmed, "Scale:"))
        {
            ParseVector3(ExtractValue(trimmed, "Scale:"), current_object->scale);
        }
        else if (StartsWith(trimmed, "PhysicsShape:"))
        {
            current_object->physics_shape = ParseSceneObjectPhysicsShape(ExtractValue(trimmed, "PhysicsShape:"));
        }
        else if (StartsWith(trimmed, "PhysicsDynamic:"))
        {
            ParseBool(ExtractValue(trimmed, "PhysicsDynamic:"), current_object->physics_is_dynamic);
        }
        else if (StartsWith(trimmed, "PhysicsLockRotationX:"))
        {
            ParseBool(ExtractValue(trimmed, "PhysicsLockRotationX:"), current_object->physics_lock_rotation_x);
        }
        else if (StartsWith(trimmed, "PhysicsLockRotationY:"))
        {
            ParseBool(ExtractValue(trimmed, "PhysicsLockRotationY:"), current_object->physics_lock_rotation_y);
        }
        else if (StartsWith(trimmed, "PhysicsLockRotationZ:"))
        {
            ParseBool(ExtractValue(trimmed, "PhysicsLockRotationZ:"), current_object->physics_lock_rotation_z);
        }
        else if (StartsWith(trimmed, "PhysicsMass:"))
        {
            ParseScalar(ExtractValue(trimmed, "PhysicsMass:"), current_object->physics_mass);
        }
        else if (StartsWith(trimmed, "PhysicsFriction:"))
        {
            ParseScalar(ExtractValue(trimmed, "PhysicsFriction:"), current_object->physics_friction);
        }
        else if (StartsWith(trimmed, "PhysicsRadius:"))
        {
            ParseScalar(ExtractValue(trimmed, "PhysicsRadius:"), current_object->physics_radius);
        }
        else if (StartsWith(trimmed, "PhysicsCapsuleHalfHeight:"))
        {
            ParseScalar(ExtractValue(trimmed, "PhysicsCapsuleHalfHeight:"), current_object->physics_capsule_half_height);
        }
        else if (StartsWith(trimmed, "PhysicsHalfExtent:"))
        {
            ParseVector3(ExtractValue(trimmed, "PhysicsHalfExtent:"), current_object->physics_half_extent);
        }
        else if (StartsWith(trimmed, "PhysicsLinearDamping:"))
        {
            ParseScalar(ExtractValue(trimmed, "PhysicsLinearDamping:"), current_object->physics_linear_damping);
        }
        else if (StartsWith(trimmed, "PhysicsAngularDamping:"))
        {
            ParseScalar(ExtractValue(trimmed, "PhysicsAngularDamping:"), current_object->physics_angular_damping);
        }
        else if (StartsWith(trimmed, "Tags:"))
        {
            current_object->tags.clear();
            const std::string tags_value = ExtractValue(trimmed, "Tags:");
            std::stringstream tags_stream(tags_value);
            std::string token;
            while (std::getline(tags_stream, token, ','))
            {
                const std::string sanitized = SanitizeSceneObjectTag(token);
                if (sanitized.empty())
                {
                    continue;
                }
                if (std::find(current_object->tags.begin(), current_object->tags.end(), sanitized) == current_object->tags.end())
                {
                    current_object->tags.push_back(sanitized);
                }
            }
        }
        else if (StartsWith(trimmed, "Attributes:"))
        {
            current_object->attributes.push_back(MakeDefaultSceneObjectAttribute(ParseSceneObjectAttributeKind(ExtractValue(trimmed, "Attributes:"))));
            current_attribute = &current_object->attributes.back();

            if (current_attribute->kind == SceneObjectAttributeKind::TriggerVolume)
            {
                current_object->physics_shape = SceneObjectPhysicsShape::Box;
                current_object->physics_is_dynamic = false;
                current_object->physics_is_trigger = true;
                current_object->physics_half_extent = current_attribute->trigger_box.half_extent;
            }
            else if (current_attribute->kind == SceneObjectAttributeKind::Rigidbody)
            {
                current_object->physics_shape = current_attribute->rigidbody.shape;
                current_object->physics_is_dynamic = current_attribute->rigidbody.is_dynamic;
                current_object->physics_is_trigger = false;
                current_object->physics_half_extent = current_attribute->rigidbody.half_extent;
            }
        }
        else if (StartsWith(trimmed, "AttributeColor:") && current_attribute != nullptr)
        {
            SceneColor3 color;
            if (ParseColor3(ExtractValue(trimmed, "AttributeColor:"), color))
            {
                current_attribute->environment_light.color = color;
                current_attribute->directional_light.color = color;
                current_attribute->point_light.color = color;
                current_attribute->spot_light.color = color;
            }
        }
        else if (StartsWith(trimmed, "AttributeIntensity:") && current_attribute != nullptr)
        {
            float intensity = 0.0f;
            if (ParseScalar(ExtractValue(trimmed, "AttributeIntensity:"), intensity))
            {
                current_attribute->environment_light.intensity = intensity;
                current_attribute->directional_light.intensity = intensity;
                current_attribute->point_light.intensity = intensity;
                current_attribute->spot_light.intensity = intensity;
            }
        }
        else if (StartsWith(trimmed, "AttributeRange:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeRange:"), current_attribute->point_light.range);
            ParseScalar(ExtractValue(trimmed, "AttributeRange:"), current_attribute->spot_light.range);
        }
        else if (StartsWith(trimmed, "AttributeSourceRadius:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeSourceRadius:"), current_attribute->point_light.source_radius);
        }
        else if (StartsWith(trimmed, "AttributeHaloIntensity:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeHaloIntensity:"), current_attribute->point_light.halo_intensity);
        }
        else if (StartsWith(trimmed, "AttributeHaloRadius:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeHaloRadius:"), current_attribute->point_light.halo_radius);
        }
        else if (StartsWith(trimmed, "AttributeInnerCone:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeInnerCone:"), current_attribute->spot_light.inner_cone_degrees);
        }
        else if (StartsWith(trimmed, "AttributeOuterCone:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeOuterCone:"), current_attribute->spot_light.outer_cone_degrees);
        }
        else if (StartsWith(trimmed, "AttributeSpotVolumetric") && current_attribute != nullptr)
        {
            // Both spot-light volumetric keys are dispatched inside this single
            // branch so the outer parse chain stays shallow (MSVC C1061 limits
            // how deeply if/else-if blocks may nest). The Intensity key is
            // checked first because it also begins with "AttributeSpotVolumetric".
            if (StartsWith(trimmed, "AttributeSpotVolumetricIntensity:"))
            {
                ParseScalar(ExtractValue(trimmed, "AttributeSpotVolumetricIntensity:"), current_attribute->spot_light.volumetric_intensity);
            }
            else if (StartsWith(trimmed, "AttributeSpotVolumetric:"))
            {
                ParseBool(ExtractValue(trimmed, "AttributeSpotVolumetric:"), current_attribute->spot_light.volumetric_enabled);
            }
        }
        else if (StartsWith(trimmed, "AttributeFov:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeFov:"), current_attribute->camera.field_of_view_degrees);
        }
        else if (StartsWith(trimmed, "AttributeNearClip:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeNearClip:"), current_attribute->camera.near_clip);
        }
        else if (StartsWith(trimmed, "AttributeFarClip:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeFarClip:"), current_attribute->camera.far_clip);
        }
        else if (StartsWith(trimmed, "AttributeActive:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeActive:"), current_attribute->camera.active);
        }
        else if (StartsWith(trimmed, "AttributeCamera") && current_attribute != nullptr)
        {
            // All camera attribute lines are dispatched inside this single
            // branch so the outer parse chain stays shallow (MSVC C1061 limits
            // how deeply if/else-if blocks may nest).
            if (StartsWith(trimmed, "AttributeCameraType:"))
            {
                const std::string type_name = TrimCopy(ExtractValue(trimmed, "AttributeCameraType:"));
                if (type_name == "Follow")
                {
                    current_attribute->camera.type = SceneObjectCameraType::Follow;
                }
                else if (type_name == "Track")
                {
                    current_attribute->camera.type = SceneObjectCameraType::Track;
                }
                else
                {
                    current_attribute->camera.type = SceneObjectCameraType::Fixed;
                }
            }
            else if (StartsWith(trimmed, "AttributeCameraFollowTarget:"))
            {
                current_attribute->camera.follow_target_object = TrimCopy(ExtractValue(trimmed, "AttributeCameraFollowTarget:"));
            }
            else if (StartsWith(trimmed, "AttributeCameraFollowOffset:"))
            {
                ParseVector3(ExtractValue(trimmed, "AttributeCameraFollowOffset:"), current_attribute->camera.follow_offset);
            }
            else if (StartsWith(trimmed, "AttributeCameraFollowOrbit:"))
            {
                ParseVector3(ExtractValue(trimmed, "AttributeCameraFollowOrbit:"), current_attribute->camera.follow_orbit);
            }
            else if (StartsWith(trimmed, "AttributeCameraFollowRotationOffset:"))
            {
                ParseVector3(ExtractValue(trimmed, "AttributeCameraFollowRotationOffset:"), current_attribute->camera.follow_rotation_offset);
            }
            else if (StartsWith(trimmed, "AttributeCameraFollowLockPosition:"))
            {
                ParseBool(ExtractValue(trimmed, "AttributeCameraFollowLockPosition:"), current_attribute->camera.follow_lock_position);
            }
            else if (StartsWith(trimmed, "AttributeCameraFollowSmoothing:"))
            {
                ParseScalar(ExtractValue(trimmed, "AttributeCameraFollowSmoothing:"), current_attribute->camera.follow_smoothing);
            }
            else if (StartsWith(trimmed, "AttributeCameraTrackPoints:"))
            {
                current_attribute->camera.track_points.clear();
                const std::string points_value = ExtractValue(trimmed, "AttributeCameraTrackPoints:");
                std::size_t cursor = 0;
                while (cursor <= points_value.size())
                {
                    const std::size_t separator = points_value.find('|', cursor);
                    const std::string token = points_value.substr(
                        cursor, separator == std::string::npos ? std::string::npos : separator - cursor);
                    SceneVector3 point{};
                    if (ParseVector3(TrimCopy(token), point))
                    {
                        current_attribute->camera.track_points.push_back(point);
                    }
                    if (separator == std::string::npos)
                    {
                        break;
                    }
                    cursor = separator + 1;
                }
            }
            else if (StartsWith(trimmed, "AttributeCameraTrackSpeed:"))
            {
                ParseScalar(ExtractValue(trimmed, "AttributeCameraTrackSpeed:"), current_attribute->camera.track_speed);
            }
            else if (StartsWith(trimmed, "AttributeCameraTrackAcceleration:"))
            {
                ParseScalar(ExtractValue(trimmed, "AttributeCameraTrackAcceleration:"), current_attribute->camera.track_acceleration);
            }
            else if (StartsWith(trimmed, "AttributeCameraTrackRotationOffset:"))
            {
                ParseVector3(ExtractValue(trimmed, "AttributeCameraTrackRotationOffset:"), current_attribute->camera.track_rotation_offset);
            }
        }
        else if (StartsWith(trimmed, "AttributePhysicsShape:") && current_attribute != nullptr)
        {
            current_attribute->rigidbody.shape = ParseSceneObjectPhysicsShape(ExtractValue(trimmed, "AttributePhysicsShape:"));
            current_object->physics_shape = current_attribute->rigidbody.shape;
            current_object->physics_is_trigger = false;
        }
        else if (StartsWith(trimmed, "AttributePhysicsDynamic:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributePhysicsDynamic:"), current_attribute->rigidbody.is_dynamic);
            current_object->physics_is_dynamic = current_attribute->rigidbody.is_dynamic;
            current_object->physics_is_trigger = false;
        }
        else if (StartsWith(trimmed, "AttributePhysicsLockRotationX:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributePhysicsLockRotationX:"), current_attribute->rigidbody.lock_rotation_x);
            current_object->physics_lock_rotation_x = current_attribute->rigidbody.lock_rotation_x;
        }
        else if (StartsWith(trimmed, "AttributePhysicsLockRotationY:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributePhysicsLockRotationY:"), current_attribute->rigidbody.lock_rotation_y);
            current_object->physics_lock_rotation_y = current_attribute->rigidbody.lock_rotation_y;
        }
        else if (StartsWith(trimmed, "AttributePhysicsLockRotationZ:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributePhysicsLockRotationZ:"), current_attribute->rigidbody.lock_rotation_z);
            current_object->physics_lock_rotation_z = current_attribute->rigidbody.lock_rotation_z;
        }
        else if (StartsWith(trimmed, "AttributePhysicsMass:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributePhysicsMass:"), current_attribute->rigidbody.mass);
            current_object->physics_mass = current_attribute->rigidbody.mass;
        }
        else if (StartsWith(trimmed, "AttributePhysicsFriction:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributePhysicsFriction:"), current_attribute->rigidbody.friction);
            current_object->physics_friction = current_attribute->rigidbody.friction;
        }
        else if (StartsWith(trimmed, "AttributePhysicsRadius:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributePhysicsRadius:"), current_attribute->rigidbody.radius);
            current_object->physics_radius = current_attribute->rigidbody.radius;
        }
        else if (StartsWith(trimmed, "AttributePhysicsCapsuleHalfHeight:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributePhysicsCapsuleHalfHeight:"), current_attribute->rigidbody.capsule_half_height);
            current_object->physics_capsule_half_height = current_attribute->rigidbody.capsule_half_height;
        }
        else if (StartsWith(trimmed, "AttributePhysicsHalfExtent:") && current_attribute != nullptr)
        {
            ParseVector3(ExtractValue(trimmed, "AttributePhysicsHalfExtent:"), current_attribute->rigidbody.half_extent);
            current_object->physics_half_extent = current_attribute->rigidbody.half_extent;
            current_object->physics_is_trigger = false;
        }
        else if (StartsWith(trimmed, "AttributePhysicsLinearDamping:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributePhysicsLinearDamping:"), current_attribute->rigidbody.linear_damping);
            current_object->physics_linear_damping = current_attribute->rigidbody.linear_damping;
        }
        else if (StartsWith(trimmed, "AttributePhysicsAngularDamping:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributePhysicsAngularDamping:"), current_attribute->rigidbody.angular_damping);
            current_object->physics_angular_damping = current_attribute->rigidbody.angular_damping;
            current_object->physics_is_trigger = false;
        }
        else if (StartsWith(trimmed, "AttributeTriggerHalfExtent:") && current_attribute != nullptr)
        {
            ParseVector3(ExtractValue(trimmed, "AttributeTriggerHalfExtent:"), current_attribute->trigger_box.half_extent);
            current_object->physics_shape = SceneObjectPhysicsShape::Box;
            current_object->physics_is_dynamic = false;
            current_object->physics_is_trigger = true;
            current_object->physics_half_extent = current_attribute->trigger_box.half_extent;
        }
        else if (StartsWith(trimmed, "AttributeAnimatorControllerPath:") && current_attribute != nullptr)
        {
            current_attribute->animator.controller_path = ExtractValue(trimmed, "AttributeAnimatorControllerPath:");
        }
        else if (StartsWith(trimmed, "AttributeAnimatorInitialState:") && current_attribute != nullptr)
        {
            current_attribute->animator.initial_state = ExtractValue(trimmed, "AttributeAnimatorInitialState:");
        }
        else if (StartsWith(trimmed, "AttributeAnimatorPlaybackSpeed:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeAnimatorPlaybackSpeed:"), current_attribute->animator.playback_speed);
        }
        else if (StartsWith(trimmed, "AttributeAnimatorAutoPlay:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeAnimatorAutoPlay:"), current_attribute->animator.auto_play);
        }
        else if (StartsWith(trimmed, "AttributeText2DFontPath:") && current_attribute != nullptr)
        {
            current_attribute->text_2d.font_path = ExtractValue(trimmed, "AttributeText2DFontPath:");
        }
        else if (StartsWith(trimmed, "AttributeText2DText:") && current_attribute != nullptr)
        {
            current_attribute->text_2d.text = UnescapeSceneString(ExtractValue(trimmed, "AttributeText2DText:"));
        }
        else if (StartsWith(trimmed, "AttributeText2DX:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeText2DX:"), current_attribute->text_2d.x);
        }
        else if (StartsWith(trimmed, "AttributeText2DY:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeText2DY:"), current_attribute->text_2d.y);
        }
        else if (StartsWith(trimmed, "AttributeText2DWidth:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeText2DWidth:"), current_attribute->text_2d.width);
        }
        else if (StartsWith(trimmed, "AttributeText2DHeight:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeText2DHeight:"), current_attribute->text_2d.height);
        }
        else if (StartsWith(trimmed, "AttributeText2DFontSize:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeText2DFontSize:"), current_attribute->text_2d.font_size);
        }
        else if (StartsWith(trimmed, "AttributeText2DColor:") && current_attribute != nullptr)
        {
            ParseColor3(ExtractValue(trimmed, "AttributeText2DColor:"), current_attribute->text_2d.color);
        }
        else if (StartsWith(trimmed, "AttributeText2DAlpha:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeText2DAlpha:"), current_attribute->text_2d.alpha);
        }
        else if (StartsWith(trimmed, "AttributeText2DLockAspectRatio:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeText2DLockAspectRatio:"), current_attribute->text_2d.lock_aspect_ratio);
        }
        else if (StartsWith(trimmed, "AttributeText2DPriority:") && current_attribute != nullptr)
        {
            ParseInteger(ExtractValue(trimmed, "AttributeText2DPriority:"), current_attribute->text_2d.priority);
        }
        else if (StartsWith(trimmed, "AttributeImage2DImagePath:") && current_attribute != nullptr)
        {
            current_attribute->image_2d.image_path = ExtractValue(trimmed, "AttributeImage2DImagePath:");
        }
        else if (StartsWith(trimmed, "AttributeImage2DX:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeImage2DX:"), current_attribute->image_2d.x);
        }
        else if (StartsWith(trimmed, "AttributeImage2DY:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeImage2DY:"), current_attribute->image_2d.y);
        }
        else if (StartsWith(trimmed, "AttributeImage2DWidth:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeImage2DWidth:"), current_attribute->image_2d.width);
        }
        else if (StartsWith(trimmed, "AttributeImage2DHeight:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeImage2DHeight:"), current_attribute->image_2d.height);
        }
        else if (StartsWith(trimmed, "AttributeImage2DTint:") && current_attribute != nullptr)
        {
            ParseColor3(ExtractValue(trimmed, "AttributeImage2DTint:"), current_attribute->image_2d.tint);
        }
        else if (StartsWith(trimmed, "AttributeImage2DAlpha:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeImage2DAlpha:"), current_attribute->image_2d.alpha);
        }
        else if (StartsWith(trimmed, "AttributeImage2DLockAspectRatio:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeImage2DLockAspectRatio:"), current_attribute->image_2d.lock_aspect_ratio);
        }
        else if (StartsWith(trimmed, "AttributeImage2DStretchToScreen:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeImage2DStretchToScreen:"), current_attribute->image_2d.stretch_to_screen);
        }
        else if (StartsWith(trimmed, "AttributeImage2DPlayMode:") && current_attribute != nullptr)
        {
            const std::string mode = TrimCopy(ExtractValue(trimmed, "AttributeImage2DPlayMode:"));
            if (mode == "Loop")
            {
                current_attribute->image_2d.play_mode = SceneObjectImagePlayMode::Loop;
            }
            else if (mode == "PlayOnce" || mode == "On")
            {
                current_attribute->image_2d.play_mode = SceneObjectImagePlayMode::PlayOnce;
            }
            else
            {
                current_attribute->image_2d.play_mode = SceneObjectImagePlayMode::Off;
            }
        }
        else if (StartsWith(trimmed, "AttributeImage2DPriority:") && current_attribute != nullptr)
        {
            ParseInteger(ExtractValue(trimmed, "AttributeImage2DPriority:"), current_attribute->image_2d.priority);
        }
        else if (StartsWith(trimmed, "AttributeColor2DX:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeColor2DX:"), current_attribute->color_2d.x);
        }
        else if (StartsWith(trimmed, "AttributeColor2DY:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeColor2DY:"), current_attribute->color_2d.y);
        }
        else if (StartsWith(trimmed, "AttributeColor2DWidth:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeColor2DWidth:"), current_attribute->color_2d.width);
        }
        else if (StartsWith(trimmed, "AttributeColor2DHeight:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeColor2DHeight:"), current_attribute->color_2d.height);
        }
        else if (StartsWith(trimmed, "AttributeColor2DColor:") && current_attribute != nullptr)
        {
            ParseColor3(ExtractValue(trimmed, "AttributeColor2DColor:"), current_attribute->color_2d.color);
        }
        else if (StartsWith(trimmed, "AttributeColor2DAlpha:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeColor2DAlpha:"), current_attribute->color_2d.alpha);
        }
        else if (StartsWith(trimmed, "AttributeColor2DLockAspectRatio:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeColor2DLockAspectRatio:"), current_attribute->color_2d.lock_aspect_ratio);
        }
        else if (StartsWith(trimmed, "AttributeColor2DStretchToScreen:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeColor2DStretchToScreen:"), current_attribute->color_2d.stretch_to_screen);
        }
        else if (StartsWith(trimmed, "AttributeColor2DPriority:") && current_attribute != nullptr)
        {
            ParseInteger(ExtractValue(trimmed, "AttributeColor2DPriority:"), current_attribute->color_2d.priority);
        }
        else if (StartsWith(trimmed, "AttributeSkyboxImagePath:") && current_attribute != nullptr)
        {
            current_attribute->skybox.image_path = ExtractValue(trimmed, "AttributeSkyboxImagePath:");
        }
        else if (StartsWith(trimmed, "AttributeSkyboxRotation:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeSkyboxRotation:"), current_attribute->skybox.rotation_degrees);
        }
        else if (StartsWith(trimmed, "AttributeAudioClipPath:") && current_attribute != nullptr)
        {
            current_attribute->audio.clip_path = ExtractValue(trimmed, "AttributeAudioClipPath:");
        }
        else if (StartsWith(trimmed, "AttributeAudioPlayMode:") && current_attribute != nullptr)
        {
            const std::string mode = TrimCopy(ExtractValue(trimmed, "AttributeAudioPlayMode:"));
            if (mode == "On" || mode == "Autoplay")
            {
                current_attribute->audio.play_mode = SceneObjectAudioPlayMode::On;
            }
            else
            {
                current_attribute->audio.play_mode = SceneObjectAudioPlayMode::Off;
            }
        }
        else if (StartsWith(trimmed, "AttributeAudioVolume:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeAudioVolume:"), current_attribute->audio.volume);
        }
        else if (StartsWith(trimmed, "AttributeAudioPitch:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeAudioPitch:"), current_attribute->audio.pitch);
        }
        else if (StartsWith(trimmed, "AttributeAudioLoop:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeAudioLoop:"), current_attribute->audio.loop);
        }
        else if (StartsWith(trimmed, "AttributeAudioSpatialize:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeAudioSpatialize:"), current_attribute->audio.spatialize_3d);
        }
        else if (StartsWith(trimmed, "AttributeAudioMinDistance:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeAudioMinDistance:"), current_attribute->audio.min_distance);
        }
        else if (StartsWith(trimmed, "AttributeAudioMaxDistance:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeAudioMaxDistance:"), current_attribute->audio.max_distance);
        }
        else if (StartsWith(trimmed, "AttributeAudioDopplerFactor:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeAudioDopplerFactor:"), current_attribute->audio.doppler_factor);
        }
        else if (StartsWith(trimmed, "AttributeVideo2DVideoPath:") && current_attribute != nullptr)
        {
            current_attribute->video_2d.video_path = ExtractValue(trimmed, "AttributeVideo2DVideoPath:");
        }
        else if (StartsWith(trimmed, "AttributeVideo2DX:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeVideo2DX:"), current_attribute->video_2d.x);
        }
        else if (StartsWith(trimmed, "AttributeVideo2DY:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeVideo2DY:"), current_attribute->video_2d.y);
        }
        else if (StartsWith(trimmed, "AttributeVideo2DWidth:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeVideo2DWidth:"), current_attribute->video_2d.width);
        }
        else if (StartsWith(trimmed, "AttributeVideo2DHeight:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeVideo2DHeight:"), current_attribute->video_2d.height);
        }
        else if (StartsWith(trimmed, "AttributeVideo2DTint:") && current_attribute != nullptr)
        {
            ParseColor3(ExtractValue(trimmed, "AttributeVideo2DTint:"), current_attribute->video_2d.tint);
        }
        else if (StartsWith(trimmed, "AttributeVideo2DAlpha:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeVideo2DAlpha:"), current_attribute->video_2d.alpha);
        }
        else if (StartsWith(trimmed, "AttributeVideo2DLockAspectRatio:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeVideo2DLockAspectRatio:"), current_attribute->video_2d.lock_aspect_ratio);
        }
        else if (StartsWith(trimmed, "AttributeVideo2DStretchToScreen:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeVideo2DStretchToScreen:"), current_attribute->video_2d.stretch_to_screen);
        }
        else if (StartsWith(trimmed, "AttributeVideo2DPriority:") && current_attribute != nullptr)
        {
            ParseInteger(ExtractValue(trimmed, "AttributeVideo2DPriority:"), current_attribute->video_2d.priority);
        }
        else if (StartsWith(trimmed, "AttributeVideo2DPlayMode:") && current_attribute != nullptr)
        {
            const std::string mode = TrimCopy(ExtractValue(trimmed, "AttributeVideo2DPlayMode:"));
            if (mode == "Loop")
            {
                current_attribute->video_2d.play_mode = SceneObjectVideoPlayMode::Loop;
            }
            else if (mode == "PlayOnce" || mode == "On")
            {
                current_attribute->video_2d.play_mode = SceneObjectVideoPlayMode::PlayOnce;
            }
            else
            {
                current_attribute->video_2d.play_mode = SceneObjectVideoPlayMode::Off;
            }
        }
        else if (StartsWith(trimmed, "AttributeVideo2DVolume:") && current_attribute != nullptr)
        {
            ParseScalar(ExtractValue(trimmed, "AttributeVideo2DVolume:"), current_attribute->video_2d.volume);
        }
        else if (StartsWith(trimmed, "AttributeVideo2DMuted:") && current_attribute != nullptr)
        {
            ParseBool(ExtractValue(trimmed, "AttributeVideo2DMuted:"), current_attribute->video_2d.muted);
        }
        else if (StartsWith(trimmed, "AttributeEffectPath:") && current_attribute != nullptr)
        {
            current_attribute->effects.effect_path = ExtractValue(trimmed, "AttributeEffectPath:");
        }
        else if (StartsWith(trimmed, "AttributeEffectPlayMode:") && current_attribute != nullptr)
        {
            const std::string mode = TrimCopy(ExtractValue(trimmed, "AttributeEffectPlayMode:"));
            if (mode == "PlayOnce" || mode == "On")
            {
                current_attribute->effects.trigger_mode = SceneObjectEffectsPlayMode::PlayOnce;
            }
            else
            {
                // "Loop" and legacy "Stop" both default to Loop
                current_attribute->effects.trigger_mode = SceneObjectEffectsPlayMode::Loop;
            }
        }
        // All shader-attribute keys share the "AttributeShader" prefix and are
        // dispatched inside this single outer branch to keep the LoadSceneMetadata
        // else-if chain under the MSVC C1061 nesting limit.
        else if (StartsWith(trimmed, "AttributeShader") && current_attribute != nullptr)
        {
            if (StartsWith(trimmed, "AttributeShaderType:"))
            {
                const std::string type_name = TrimCopy(ExtractValue(trimmed, "AttributeShaderType:"));
                if (type_name == "Water")
                {
                    current_attribute->shader.type = SceneObjectShaderType::Water;
                }
                else if (type_name == "Cloud")
                {
                    current_attribute->shader.type = SceneObjectShaderType::Cloud;
                }
                else if (type_name == "Fire")
                {
                    current_attribute->shader.type = SceneObjectShaderType::Fire;
                }
                else if (type_name == "Rain")
                {
                    current_attribute->shader.type = SceneObjectShaderType::Rain;
                }
                else if (type_name == "Puddle")
                {
                    current_attribute->shader.type = SceneObjectShaderType::Puddle;
                }
                else if (type_name == "RainParticles")
                {
                    current_attribute->shader.type = SceneObjectShaderType::RainParticles;
                }
                else if (type_name == "Fog")
                {
                    current_attribute->shader.type = SceneObjectShaderType::Fog;
                }
                else
                {
                    current_attribute->shader.type = SceneObjectShaderType::None;
                }
            }
            else if (StartsWith(trimmed, "AttributeShaderDropScale:"))
            {
                ParseScalar(ExtractValue(trimmed, "AttributeShaderDropScale:"), current_attribute->shader.drop_scale);
            }
            else if (StartsWith(trimmed, "AttributeShaderDropSpeed:"))
            {
                ParseScalar(ExtractValue(trimmed, "AttributeShaderDropSpeed:"), current_attribute->shader.rain_speed);
            }
            else if (StartsWith(trimmed, "AttributeShaderColor:"))
            {
                ParseColor3(ExtractValue(trimmed, "AttributeShaderColor:"), current_attribute->shader.color);
            }
            else if (StartsWith(trimmed, "AttributeShaderFogDensity:"))
            {
                ParseScalar(ExtractValue(trimmed, "AttributeShaderFogDensity:"), current_attribute->shader.fog_density);
            }
        }
        else if (StartsWith(trimmed, "AttributeShape3DPath:") && current_attribute != nullptr)
        {
            current_attribute->shape_3d.shape_path = TrimCopy(ExtractValue(trimmed, "AttributeShape3DPath:"));
        }
        else if (StartsWith(trimmed, "Model:"))
        {
            current_attribute = nullptr;
            current_object->model_path = ExtractValue(trimmed, "Model:");
        }
        else if (StartsWith(trimmed, "ModelVisualOffset:"))
        {
            current_attribute = nullptr;
            ParseVector3(ExtractValue(trimmed, "ModelVisualOffset:"), current_object->model_visual_offset);
        }
        else if (StartsWith(trimmed, "Script:"))
        {
            current_attribute = nullptr;
            current_object->script_paths.push_back(ExtractValue(trimmed, "Script:"));
        }
        else if (StartsWith(trimmed, "Graph:"))
        {
            current_attribute = nullptr;
            current_object->graph_paths.push_back(ExtractValue(trimmed, "Graph:"));
        }
    }

    ResolveSceneObjectEnabledStateInternal(metadata);
    metadata.parsed = true;
    return metadata;
}

void ResolveSceneObjectEnabledState(SceneMetadata& scene_metadata)
{
    ResolveSceneObjectEnabledStateInternal(scene_metadata);
}

bool IsSceneObjectEnabledInHierarchy(const SceneMetadata& scene_metadata, const std::string& object_name)
{
    const auto object_it = std::find_if(scene_metadata.objects.begin(), scene_metadata.objects.end(), [&](const SceneObjectMetadata& object)
    {
        return object.name == object_name;
    });
    return object_it == scene_metadata.objects.end() || object_it->enabled_in_hierarchy;
}

ActiveSceneCameraSelection FindActiveSceneCamera(const SceneMetadata& scene_metadata)
{
    ActiveSceneCameraSelection selection;
    if (!scene_metadata.parsed)
    {
        return selection;
    }

    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        if (!object.enabled_in_hierarchy)
        {
            continue;
        }

        for (std::size_t attribute_index = 0; attribute_index < object.attributes.size(); ++attribute_index)
        {
            const SceneObjectAttribute& attribute = object.attributes[attribute_index];
            if (attribute.kind != SceneObjectAttributeKind::Camera || !attribute.camera.active)
            {
                continue;
            }

            selection.found = true;
            selection.object_name = object.name;
            selection.attribute_index = attribute_index;
            selection.object = object;
            selection.camera = attribute.camera;
            return selection;
        }
    }

    return selection;
}

bool SetSceneObjectEnabled(const std::filesystem::path& scene_path, const std::string& object_name, bool enabled)
{
    return SetSceneObjectBoolean(scene_path, object_name, "Enabled", enabled);
}

bool SetSceneObjectPosition(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& position)
{
    return SetSceneObjectVector3(scene_path, object_name, "Position", position);
}

bool SetSceneObjectRotation(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& rotation)
{
    return SetSceneObjectVector3(scene_path, object_name, "Rotation", rotation);
}

bool SetSceneObjectScale(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& scale)
{
    return SetSceneObjectVector3(scene_path, object_name, "Scale", scale);
}

bool SetSceneObjectTransform(
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    const SceneVector3& position,
    const SceneVector3& rotation,
    const SceneVector3& scale,
    bool write_position,
    bool write_rotation,
    bool write_scale)
{
    if (!write_position && !write_rotation && !write_scale)
    {
        return false;
    }

    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t insert_index = object_end;

        auto upsert_vector_line = [&](std::string_view key, const SceneVector3& value)
        {
            const std::string key_prefix = std::string(key) + ":";
            const std::string new_line = std::string(key) + ": " + FormatVector3(value);
            for (std::size_t index = object_start + 1; index < insert_index; ++index)
            {
                if (StartsWith(TrimCopy(lines[index]), key_prefix))
                {
                    lines[index] = new_line;
                    return;
                }
            }

            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_index), new_line);
            ++insert_index;
        };

        if (write_position)
        {
            upsert_vector_line("Position", position);
        }
        if (write_rotation)
        {
            upsert_vector_line("Rotation", rotation);
        }
        if (write_scale)
        {
            upsert_vector_line("Scale", scale);
        }
    });
}

bool SetSceneObjectModelVisualOffset(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& model_visual_offset)
{
    return SetSceneObjectVector3(scene_path, object_name, "ModelVisualOffset", model_visual_offset);
}

bool SetSceneObjectPhysicsShape(const std::filesystem::path& scene_path, const std::string& object_name, SceneObjectPhysicsShape shape)
{
    const char* shape_name = "None";
    if (shape == SceneObjectPhysicsShape::Box)
    {
        shape_name = "Box";
    }
    else if (shape == SceneObjectPhysicsShape::Sphere)
    {
        shape_name = "Sphere";
    }
    else if (shape == SceneObjectPhysicsShape::Capsule)
    {
        shape_name = "Capsule";
    }
    else if (shape == SceneObjectPhysicsShape::Mesh)
    {
        shape_name = "Mesh";
    }

    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        const std::string key_prefix = "PhysicsShape:";
        const std::string new_line = std::string("PhysicsShape: ") + shape_name;
        for (std::size_t index = object_start + 1; index < object_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(object_end), new_line);
    });
}

bool SetSceneObjectPhysicsDynamic(const std::filesystem::path& scene_path, const std::string& object_name, bool is_dynamic)
{
    return SetSceneObjectBoolean(scene_path, object_name, "PhysicsDynamic", is_dynamic);
}

bool SetSceneObjectPhysicsMass(const std::filesystem::path& scene_path, const std::string& object_name, float mass)
{
    return SetSceneObjectScalar(scene_path, object_name, "PhysicsMass", mass);
}

bool SetSceneObjectPhysicsFriction(const std::filesystem::path& scene_path, const std::string& object_name, float friction)
{
    return SetSceneObjectScalar(scene_path, object_name, "PhysicsFriction", friction);
}

bool SetSceneObjectPhysicsRadius(const std::filesystem::path& scene_path, const std::string& object_name, float radius)
{
    return SetSceneObjectScalar(scene_path, object_name, "PhysicsRadius", radius);
}

bool SetSceneObjectPhysicsHalfExtent(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& half_extent)
{
    return SetSceneObjectVector3(scene_path, object_name, "PhysicsHalfExtent", half_extent);
}

bool SetSceneObjectPhysicsLinearDamping(const std::filesystem::path& scene_path, const std::string& object_name, float linear_damping)
{
    return SetSceneObjectScalar(scene_path, object_name, "PhysicsLinearDamping", linear_damping);
}

bool SetSceneObjectPhysicsAngularDamping(const std::filesystem::path& scene_path, const std::string& object_name, float angular_damping)
{
    return SetSceneObjectScalar(scene_path, object_name, "PhysicsAngularDamping", angular_damping);
}

std::string SanitizeSceneObjectTag(const std::string& tag)
{
    std::string result;
    result.reserve(tag.size());
    for (const char ch : tag)
    {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (ch == ',' || ch == '\n' || ch == '\r')
        {
            continue;
        }
        if (std::isspace(uc) && (result.empty() || result.back() == ' '))
        {
            continue;
        }
        result.push_back(std::isspace(uc) ? ' ' : ch);
    }
    while (!result.empty() && result.back() == ' ')
    {
        result.pop_back();
    }
    return result;
}

bool SetSceneObjectTags(const std::filesystem::path& scene_path, const std::string& object_name, const std::vector<std::string>& tags)
{
    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::string joined;
        for (const std::string& tag : tags)
        {
            const std::string sanitized = SanitizeSceneObjectTag(tag);
            if (sanitized.empty())
            {
                continue;
            }
            if (!joined.empty())
            {
                joined += ',';
            }
            joined += sanitized;
        }

        const std::string key_prefix = "Tags:";
        for (std::size_t index = object_start + 1; index < object_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                if (joined.empty())
                {
                    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
                }
                else
                {
                    lines[index] = "Tags: " + joined;
                }
                return;
            }
        }

        if (!joined.empty())
        {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(object_end), "Tags: " + joined);
        }
    });
}

bool AddSceneObjectTag(const std::filesystem::path& scene_path, const std::string& object_name, const std::string& tag)
{
    const std::string sanitized = SanitizeSceneObjectTag(tag);
    if (sanitized.empty())
    {
        return false;
    }

    const SceneMetadata scene = LoadSceneMetadata(scene_path);
    const auto it = std::find_if(scene.objects.begin(), scene.objects.end(),
        [&](const SceneObjectMetadata& obj) { return obj.name == object_name; });
    if (it == scene.objects.end())
    {
        return false;
    }

    std::vector<std::string> updated = it->tags;
    if (std::find(updated.begin(), updated.end(), sanitized) != updated.end())
    {
        return false;
    }
    updated.push_back(sanitized);
    return SetSceneObjectTags(scene_path, object_name, updated);
}

bool RemoveSceneObjectTag(const std::filesystem::path& scene_path, const std::string& object_name, const std::string& tag)
{
    const std::string sanitized = SanitizeSceneObjectTag(tag);
    if (sanitized.empty())
    {
        return false;
    }

    const SceneMetadata scene = LoadSceneMetadata(scene_path);
    const auto it = std::find_if(scene.objects.begin(), scene.objects.end(),
        [&](const SceneObjectMetadata& obj) { return obj.name == object_name; });
    if (it == scene.objects.end())
    {
        return false;
    }

    std::vector<std::string> updated = it->tags;
    const auto erase_it = std::remove(updated.begin(), updated.end(), sanitized);
    if (erase_it == updated.end())
    {
        return false;
    }
    updated.erase(erase_it, updated.end());
    return SetSceneObjectTags(scene_path, object_name, updated);
}

bool AddSceneObjectAttribute(const std::filesystem::path& scene_path, const std::string& object_name, SceneObjectAttributeKind kind)
{
    if (kind == SceneObjectAttributeKind::None)
    {
        return false;
    }

    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        const std::size_t insert_index = FindSceneObjectAttributeInsertionIndex(lines, object_start, object_end);
        lines.insert(
            lines.begin() + static_cast<std::ptrdiff_t>(insert_index),
            std::string("Attributes: ") + ToStorageName(kind));
    });
}

bool RemoveSceneObjectAttribute(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index)
{
    bool removed = false;
    RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        lines.erase(
            lines.begin() + static_cast<std::ptrdiff_t>(attribute_start),
            lines.begin() + static_cast<std::ptrdiff_t>(attribute_end));
        removed = true;
    });

    return removed;
}

bool SetSceneObjectAttributeKind(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectAttributeKind kind)
{
    if (kind == SceneObjectAttributeKind::None)
    {
        return RemoveSceneObjectAttribute(scene_path, object_name, attribute_index);
    }

    bool updated = false;
    RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        lines.erase(
            lines.begin() + static_cast<std::ptrdiff_t>(attribute_start),
            lines.begin() + static_cast<std::ptrdiff_t>(attribute_end));
        lines.insert(
            lines.begin() + static_cast<std::ptrdiff_t>(attribute_start),
            std::string("Attributes: ") + ToStorageName(kind));
        updated = true;
    });

    return updated;
}

bool SetSceneObjectAttributeColor(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& color)
{
    return SetSceneObjectAttributeColorValue("AttributeColor", scene_path, object_name, attribute_index, color);
}

bool SetSceneObjectAttributeIntensity(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float intensity)
{
    return SetSceneObjectAttributeScalar("AttributeIntensity", scene_path, object_name, attribute_index, intensity);
}

bool SetSceneObjectAttributeRange(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float range)
{
    return SetSceneObjectAttributeScalar("AttributeRange", scene_path, object_name, attribute_index, range);
}

bool SetSceneObjectAttributeSourceRadius(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float source_radius)
{
    return SetSceneObjectAttributeScalar("AttributeSourceRadius", scene_path, object_name, attribute_index, source_radius);
}

bool SetSceneObjectAttributeHaloIntensity(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float halo_intensity)
{
    return SetSceneObjectAttributeScalar("AttributeHaloIntensity", scene_path, object_name, attribute_index, halo_intensity);
}

bool SetSceneObjectAttributeHaloRadius(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float halo_radius)
{
    return SetSceneObjectAttributeScalar("AttributeHaloRadius", scene_path, object_name, attribute_index, halo_radius);
}

bool SetSceneObjectAttributeInnerConeDegrees(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float inner_cone_degrees)
{
    return SetSceneObjectAttributeScalar("AttributeInnerCone", scene_path, object_name, attribute_index, inner_cone_degrees);
}

bool SetSceneObjectAttributeOuterConeDegrees(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float outer_cone_degrees)
{
    return SetSceneObjectAttributeScalar("AttributeOuterCone", scene_path, object_name, attribute_index, outer_cone_degrees);
}

bool SetSceneObjectAttributeSpotVolumetricEnabled(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool enabled)
{
    return SetSceneObjectAttributeBoolean("AttributeSpotVolumetric", scene_path, object_name, attribute_index, enabled);
}

bool SetSceneObjectAttributeSpotVolumetricIntensity(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float intensity)
{
    return SetSceneObjectAttributeScalar("AttributeSpotVolumetricIntensity", scene_path, object_name, attribute_index, intensity);
}

bool SetSceneObjectAttributeFieldOfView(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float field_of_view_degrees)
{
    return SetSceneObjectAttributeScalar("AttributeFov", scene_path, object_name, attribute_index, field_of_view_degrees);
}

bool SetSceneObjectAttributeNearClip(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float near_clip)
{
    return SetSceneObjectAttributeScalar("AttributeNearClip", scene_path, object_name, attribute_index, near_clip);
}

bool SetSceneObjectAttributeFarClip(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float far_clip)
{
    return SetSceneObjectAttributeScalar("AttributeFarClip", scene_path, object_name, attribute_index, far_clip);
}

bool SetSceneObjectCameraActive(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool active)
{
    const SceneMetadata scene_metadata = LoadSceneMetadata(scene_path);
    if (!scene_metadata.parsed)
    {
        return false;
    }

    const SceneObjectAttribute* target_attribute = nullptr;
    for (const SceneObjectMetadata& object : scene_metadata.objects)
    {
        if (object.name == object_name && attribute_index < object.attributes.size())
        {
            target_attribute = &object.attributes[attribute_index];
            break;
        }
    }

    if (target_attribute == nullptr || target_attribute->kind != SceneObjectAttributeKind::Camera)
    {
        return false;
    }

    if (active)
    {
        for (const SceneObjectMetadata& object : scene_metadata.objects)
        {
            for (std::size_t index = 0; index < object.attributes.size(); ++index)
            {
                const SceneObjectAttribute& attribute = object.attributes[index];
                if (attribute.kind != SceneObjectAttributeKind::Camera)
                {
                    continue;
                }

                const bool is_target = object.name == object_name && index == attribute_index;
                if (!is_target && attribute.camera.active)
                {
                    if (!SetSceneObjectAttributeBoolean("AttributeActive", scene_path, object.name, index, false))
                    {
                        return false;
                    }
                }
            }
        }
    }

    return SetSceneObjectAttributeBoolean("AttributeActive", scene_path, object_name, attribute_index, active);
}

bool SetSceneObjectAttributeVector3Value(
    std::string_view key,
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    std::size_t attribute_index,
    const SceneVector3& value)
{
    bool updated = false;
    const bool rewrite_succeeded = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        const std::string key_prefix = std::string(key) + ":";
        const std::string new_line = std::string(key) + ": " + FormatVector3(value);
        for (std::size_t index = attribute_start + 1; index < attribute_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                updated = true;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(attribute_end), new_line);
        updated = true;
    });

    return rewrite_succeeded && updated;
}

bool SetSceneObjectAttributeStringValue(
    std::string_view key,
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    std::size_t attribute_index,
    std::string_view string_value)
{
    bool updated = false;
    const bool rewrite_succeeded = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        const std::string key_prefix = std::string(key) + ":";
        const std::string new_line = std::string(key) + ": " + std::string(string_value);
        for (std::size_t index = attribute_start + 1; index < attribute_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                updated = true;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(attribute_end), new_line);
        updated = true;
    });

    return rewrite_succeeded && updated;
}

bool SetSceneObjectAttributeCameraType(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectCameraType type)
{
    std::string value = "Fixed";
    if (type == SceneObjectCameraType::Follow)
    {
        value = "Follow";
    }
    else if (type == SceneObjectCameraType::Track)
    {
        value = "Track";
    }
    return SetSceneObjectAttributeStringValue("AttributeCameraType", scene_path, object_name, attribute_index, value);
}

bool SetSceneObjectAttributeCameraFollowTarget(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& follow_target_object)
{
    return SetSceneObjectAttributeStringValue("AttributeCameraFollowTarget", scene_path, object_name, attribute_index, follow_target_object);
}

bool SetSceneObjectAttributeCameraFollowOffset(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneVector3& follow_offset)
{
    return SetSceneObjectAttributeVector3Value("AttributeCameraFollowOffset", scene_path, object_name, attribute_index, follow_offset);
}

bool SetSceneObjectAttributeCameraFollowOrbit(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneVector3& follow_orbit)
{
    return SetSceneObjectAttributeVector3Value("AttributeCameraFollowOrbit", scene_path, object_name, attribute_index, follow_orbit);
}

bool SetSceneObjectAttributeCameraFollowRotationOffset(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneVector3& follow_rotation_offset)
{
    return SetSceneObjectAttributeVector3Value("AttributeCameraFollowRotationOffset", scene_path, object_name, attribute_index, follow_rotation_offset);
}

bool SetSceneObjectAttributeCameraFollowLockPosition(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool follow_lock_position)
{
    return SetSceneObjectAttributeBoolean("AttributeCameraFollowLockPosition", scene_path, object_name, attribute_index, follow_lock_position);
}

bool SetSceneObjectAttributeCameraFollowSmoothing(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float follow_smoothing)
{
    return SetSceneObjectAttributeScalar("AttributeCameraFollowSmoothing", scene_path, object_name, attribute_index, follow_smoothing);
}

bool SetSceneObjectAttributeCameraTrackPoints(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::vector<SceneVector3>& track_points)
{
    std::string value;
    for (std::size_t index = 0; index < track_points.size(); ++index)
    {
        if (index != 0)
        {
            value += " | ";
        }
        value += FormatVector3(track_points[index]);
    }
    return SetSceneObjectAttributeStringValue("AttributeCameraTrackPoints", scene_path, object_name, attribute_index, value);
}

bool SetSceneObjectAttributeCameraTrackSpeed(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float track_speed)
{
    return SetSceneObjectAttributeScalar("AttributeCameraTrackSpeed", scene_path, object_name, attribute_index, track_speed);
}

bool SetSceneObjectAttributeCameraTrackAcceleration(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float track_acceleration)
{
    return SetSceneObjectAttributeScalar("AttributeCameraTrackAcceleration", scene_path, object_name, attribute_index, track_acceleration);
}

bool SetSceneObjectAttributeCameraTrackRotationOffset(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneVector3& track_rotation_offset)
{
    return SetSceneObjectAttributeVector3Value("AttributeCameraTrackRotationOffset", scene_path, object_name, attribute_index, track_rotation_offset);
}

bool SetSceneObjectAttributePhysicsShape(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectPhysicsShape shape)
{
    const char* shape_string = "None";
    if (shape == SceneObjectPhysicsShape::Box)
    {
        shape_string = "Box";
    }
    else if (shape == SceneObjectPhysicsShape::Sphere)
    {
        shape_string = "Sphere";
    }
    else if (shape == SceneObjectPhysicsShape::Capsule)
    {
        shape_string = "Capsule";
    }
    else if (shape == SceneObjectPhysicsShape::Mesh)
    {
        shape_string = "Mesh";
    }
    return SetSceneObjectAttributeStringValue("AttributePhysicsShape", scene_path, object_name, attribute_index, shape_string);
}

bool SetSceneObjectAttributePhysicsDynamic(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool is_dynamic)
{
    return SetSceneObjectAttributeBoolean("AttributePhysicsDynamic", scene_path, object_name, attribute_index, is_dynamic);
}

bool SetSceneObjectAttributePhysicsLockRotationX(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool locked)
{
    return SetSceneObjectAttributeBoolean("AttributePhysicsLockRotationX", scene_path, object_name, attribute_index, locked);
}

bool SetSceneObjectAttributePhysicsLockRotationY(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool locked)
{
    return SetSceneObjectAttributeBoolean("AttributePhysicsLockRotationY", scene_path, object_name, attribute_index, locked);
}

bool SetSceneObjectAttributePhysicsLockRotationZ(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool locked)
{
    return SetSceneObjectAttributeBoolean("AttributePhysicsLockRotationZ", scene_path, object_name, attribute_index, locked);
}

bool SetSceneObjectAttributePhysicsMass(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float mass)
{
    return SetSceneObjectAttributeScalar("AttributePhysicsMass", scene_path, object_name, attribute_index, mass);
}

bool SetSceneObjectAttributePhysicsFriction(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float friction)
{
    return SetSceneObjectAttributeScalar("AttributePhysicsFriction", scene_path, object_name, attribute_index, friction);
}

bool SetSceneObjectAttributePhysicsRadius(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float radius)
{
    return SetSceneObjectAttributeScalar("AttributePhysicsRadius", scene_path, object_name, attribute_index, radius);
}

bool SetSceneObjectAttributePhysicsCapsuleHalfHeight(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float capsule_half_height)
{
    return SetSceneObjectAttributeScalar("AttributePhysicsCapsuleHalfHeight", scene_path, object_name, attribute_index, capsule_half_height);
}

bool SetSceneObjectAttributePhysicsHalfExtent(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneVector3& half_extent)
{
    return SetSceneObjectAttributeVector3Value("AttributePhysicsHalfExtent", scene_path, object_name, attribute_index, half_extent);
}

bool SetSceneObjectAttributePhysicsLinearDamping(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float linear_damping)
{
    return SetSceneObjectAttributeScalar("AttributePhysicsLinearDamping", scene_path, object_name, attribute_index, linear_damping);
}

bool SetSceneObjectAttributePhysicsAngularDamping(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float angular_damping)
{
    return SetSceneObjectAttributeScalar("AttributePhysicsAngularDamping", scene_path, object_name, attribute_index, angular_damping);
}

bool SetSceneObjectAttributeTriggerHalfExtent(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneVector3& half_extent)
{
    return SetSceneObjectAttributeVector3Value("AttributeTriggerHalfExtent", scene_path, object_name, attribute_index, half_extent);
}

bool SetSceneObjectAttributeAnimatorControllerPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& controller_path)
{
    return SetSceneObjectAttributeStringValue("AttributeAnimatorControllerPath", scene_path, object_name, attribute_index, controller_path);
}

bool SetSceneObjectAttributeAnimatorInitialState(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& initial_state)
{
    return SetSceneObjectAttributeStringValue("AttributeAnimatorInitialState", scene_path, object_name, attribute_index, initial_state);
}

bool SetSceneObjectAttributeAnimatorPlaybackSpeed(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float playback_speed)
{
    return SetSceneObjectAttributeScalar("AttributeAnimatorPlaybackSpeed", scene_path, object_name, attribute_index, playback_speed);
}

bool SetSceneObjectAttributeAnimatorAutoPlay(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool auto_play)
{
    return SetSceneObjectAttributeBoolean("AttributeAnimatorAutoPlay", scene_path, object_name, attribute_index, auto_play);
}

namespace
{
bool SetSceneObjectAttributeStringValue(
    std::string_view key,
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    std::size_t attribute_index,
    const std::string& value)
{
    bool updated = false;
    const bool rewrite_succeeded = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        const std::string key_prefix = std::string(key) + ":";
        const std::string new_line = std::string(key) + ": " + value;
        for (std::size_t index = attribute_start + 1; index < attribute_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                updated = true;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(attribute_end), new_line);
        updated = true;
    });

    return rewrite_succeeded && updated;
}

bool SetSceneObjectAttributeFloat2Value(
    std::string_view key_x,
    std::string_view key_y,
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    std::size_t attribute_index,
    float value_x,
    float value_y)
{
    bool updated = false;
    const bool rewrite_succeeded = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        std::size_t insert_index = attribute_end;
        auto upsert_scalar = [&](std::string_view key, float scalar)
        {
            const std::string key_prefix = std::string(key) + ":";
            const std::string new_line = std::string(key) + ": " + FormatScalar(scalar);
            for (std::size_t index = attribute_start + 1; index < insert_index; ++index)
            {
                if (StartsWith(TrimCopy(lines[index]), key_prefix))
                {
                    lines[index] = new_line;
                    return;
                }
            }

            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_index), new_line);
            ++insert_index;
        };

        upsert_scalar(key_x, value_x);
        upsert_scalar(key_y, value_y);
        updated = true;
    });

    return rewrite_succeeded && updated;
}
}

bool SetSceneObjectAttributeText2DFontPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& font_path)
{
    return SetSceneObjectAttributeStringValue("AttributeText2DFontPath", scene_path, object_name, attribute_index, font_path);
}

bool SetSceneObjectAttributeText2DText(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& text)
{
    return SetSceneObjectAttributeStringValue("AttributeText2DText", scene_path, object_name, attribute_index, EscapeSceneString(text));
}

bool SetSceneObjectAttributeText2DPosition(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float x, float y)
{
    return SetSceneObjectAttributeFloat2Value(
        "AttributeText2DX",
        "AttributeText2DY",
        scene_path,
        object_name,
        attribute_index,
        x,
        y);
}

bool SetSceneObjectAttributeText2DSize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float width, float height)
{
    return SetSceneObjectAttributeFloat2Value(
        "AttributeText2DWidth",
        "AttributeText2DHeight",
        scene_path,
        object_name,
        attribute_index,
        width,
        height);
}

bool SetSceneObjectAttributeText2DFontSize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float font_size)
{
    return SetSceneObjectAttributeScalar("AttributeText2DFontSize", scene_path, object_name, attribute_index, font_size);
}

bool SetSceneObjectAttributeText2DColor(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& color)
{
    return SetSceneObjectAttributeColorValue("AttributeText2DColor", scene_path, object_name, attribute_index, color);
}

bool SetSceneObjectAttributeText2DAlpha(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float alpha)
{
    return SetSceneObjectAttributeScalar("AttributeText2DAlpha", scene_path, object_name, attribute_index, alpha);
}

bool SetSceneObjectAttributeText2DPriority(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, int priority)
{
    bool updated = false;
    const bool rewrite_succeeded = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        const std::string key_prefix = std::string("AttributeText2DPriority") + ":";
        const std::string new_line = std::string("AttributeText2DPriority") + ": " + FormatInteger(priority);
        for (std::size_t index = attribute_start + 1; index < attribute_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                updated = true;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(attribute_end), new_line);
        updated = true;
    });

    return rewrite_succeeded && updated;
}

bool SetSceneObjectAttributeText2DLockAspectRatio(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool lock_aspect_ratio)
{
    return SetSceneObjectAttributeBoolean("AttributeText2DLockAspectRatio", scene_path, object_name, attribute_index, lock_aspect_ratio);
}

bool SetSceneObjectAttributeImage2DImagePath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& image_path)
{
    return SetSceneObjectAttributeStringValue("AttributeImage2DImagePath", scene_path, object_name, attribute_index, image_path);
}

bool SetSceneObjectAttributeImage2DPosition(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float x, float y)
{
    return SetSceneObjectAttributeFloat2Value(
        "AttributeImage2DX",
        "AttributeImage2DY",
        scene_path,
        object_name,
        attribute_index,
        x,
        y);
}

bool SetSceneObjectAttributeImage2DSize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float width, float height)
{
    return SetSceneObjectAttributeFloat2Value(
        "AttributeImage2DWidth",
        "AttributeImage2DHeight",
        scene_path,
        object_name,
        attribute_index,
        width,
        height);
}

bool SetSceneObjectAttributeImage2DTint(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& tint)
{
    return SetSceneObjectAttributeColorValue("AttributeImage2DTint", scene_path, object_name, attribute_index, tint);
}

bool SetSceneObjectAttributeImage2DAlpha(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float alpha)
{
    return SetSceneObjectAttributeScalar("AttributeImage2DAlpha", scene_path, object_name, attribute_index, alpha);
}

bool SetSceneObjectAttributeImage2DPriority(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, int priority)
{
    bool updated = false;
    const bool rewrite_succeeded = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        const std::string key_prefix = std::string("AttributeImage2DPriority") + ":";
        const std::string new_line = std::string("AttributeImage2DPriority") + ": " + FormatInteger(priority);
        for (std::size_t index = attribute_start + 1; index < attribute_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                updated = true;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(attribute_end), new_line);
        updated = true;
    });

    return rewrite_succeeded && updated;
}

bool SetSceneObjectAttributeImage2DLockAspectRatio(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool lock_aspect_ratio)
{
    return SetSceneObjectAttributeBoolean("AttributeImage2DLockAspectRatio", scene_path, object_name, attribute_index, lock_aspect_ratio);
}

bool SetSceneObjectAttributeImage2DStretchToScreen(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool stretch_to_screen)
{
    return SetSceneObjectAttributeBoolean("AttributeImage2DStretchToScreen", scene_path, object_name, attribute_index, stretch_to_screen);
}

bool SetSceneObjectAttributeImage2DPlayMode(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectImagePlayMode play_mode)
{
    const std::string value = (play_mode == SceneObjectImagePlayMode::Loop) ? "Loop"
        : (play_mode == SceneObjectImagePlayMode::PlayOnce) ? "PlayOnce"
        : "Off";
    return SetSceneObjectAttributeStringValue("AttributeImage2DPlayMode", scene_path, object_name, attribute_index, value);
}

bool SetSceneObjectAttributeColor2DPosition(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float x, float y)
{
    return SetSceneObjectAttributeFloat2Value(
        "AttributeColor2DX",
        "AttributeColor2DY",
        scene_path,
        object_name,
        attribute_index,
        x,
        y);
}

bool SetSceneObjectAttributeColor2DSize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float width, float height)
{
    return SetSceneObjectAttributeFloat2Value(
        "AttributeColor2DWidth",
        "AttributeColor2DHeight",
        scene_path,
        object_name,
        attribute_index,
        width,
        height);
}

bool SetSceneObjectAttributeColor2DColor(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& color)
{
    return SetSceneObjectAttributeColorValue("AttributeColor2DColor", scene_path, object_name, attribute_index, color);
}

bool SetSceneObjectAttributeColor2DAlpha(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float alpha)
{
    return SetSceneObjectAttributeScalar("AttributeColor2DAlpha", scene_path, object_name, attribute_index, alpha);
}

bool SetSceneObjectAttributeColor2DLockAspectRatio(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool lock_aspect_ratio)
{
    return SetSceneObjectAttributeBoolean("AttributeColor2DLockAspectRatio", scene_path, object_name, attribute_index, lock_aspect_ratio);
}

bool SetSceneObjectAttributeColor2DStretchToScreen(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool stretch_to_screen)
{
    return SetSceneObjectAttributeBoolean("AttributeColor2DStretchToScreen", scene_path, object_name, attribute_index, stretch_to_screen);
}

bool SetSceneObjectAttributeColor2DPriority(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, int priority)
{
    bool updated = false;
    const bool rewrite_succeeded = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        const std::string key_prefix = std::string("AttributeColor2DPriority") + ":";
        const std::string new_line = std::string("AttributeColor2DPriority") + ": " + FormatInteger(priority);
        for (std::size_t index = attribute_start + 1; index < attribute_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                updated = true;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(attribute_end), new_line);
        updated = true;
    });

    return rewrite_succeeded && updated;
}

bool SetSceneObjectAttributeSkyboxImagePath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& image_path)
{
    return SetSceneObjectAttributeStringValue("AttributeSkyboxImagePath", scene_path, object_name, attribute_index, image_path);
}

bool SetSceneObjectAttributeSkyboxRotation(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float rotation_degrees)
{
    return SetSceneObjectAttributeScalar("AttributeSkyboxRotation", scene_path, object_name, attribute_index, rotation_degrees);
}

bool SetSceneObjectAttributeAudioClipPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& clip_path)
{
    return SetSceneObjectAttributeStringValue("AttributeAudioClipPath", scene_path, object_name, attribute_index, clip_path);
}

bool SetSceneObjectAttributeAudioPlayMode(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectAudioPlayMode play_mode)
{
    std::string value = play_mode == SceneObjectAudioPlayMode::On ? "On" : "Off";
    return SetSceneObjectAttributeStringValue("AttributeAudioPlayMode", scene_path, object_name, attribute_index, value);
}

bool SetSceneObjectAttributeAudioVolume(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float volume)
{
    return SetSceneObjectAttributeScalar("AttributeAudioVolume", scene_path, object_name, attribute_index, volume);
}

bool SetSceneObjectAttributeAudioPitch(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float pitch)
{
    return SetSceneObjectAttributeScalar("AttributeAudioPitch", scene_path, object_name, attribute_index, pitch);
}

bool SetSceneObjectAttributeAudioLoop(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool loop)
{
    return SetSceneObjectAttributeBoolean("AttributeAudioLoop", scene_path, object_name, attribute_index, loop);
}

bool SetSceneObjectAttributeAudioSpatialize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool spatialize_3d)
{
    return SetSceneObjectAttributeBoolean("AttributeAudioSpatialize", scene_path, object_name, attribute_index, spatialize_3d);
}

bool SetSceneObjectAttributeAudioMinDistance(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float min_distance)
{
    return SetSceneObjectAttributeScalar("AttributeAudioMinDistance", scene_path, object_name, attribute_index, min_distance);
}

bool SetSceneObjectAttributeAudioMaxDistance(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float max_distance)
{
    return SetSceneObjectAttributeScalar("AttributeAudioMaxDistance", scene_path, object_name, attribute_index, max_distance);
}

bool SetSceneObjectAttributeAudioDopplerFactor(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float doppler_factor)
{
    return SetSceneObjectAttributeScalar("AttributeAudioDopplerFactor", scene_path, object_name, attribute_index, doppler_factor);
}

bool SetSceneObjectAttributeVideo2DVideoPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& video_path)
{
    return SetSceneObjectAttributeStringValue("AttributeVideo2DVideoPath", scene_path, object_name, attribute_index, video_path);
}

bool SetSceneObjectAttributeVideo2DPosition(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float x, float y)
{
    return SetSceneObjectAttributeFloat2Value(
        "AttributeVideo2DX",
        "AttributeVideo2DY",
        scene_path,
        object_name,
        attribute_index,
        x,
        y);
}

bool SetSceneObjectAttributeVideo2DSize(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float width, float height)
{
    return SetSceneObjectAttributeFloat2Value(
        "AttributeVideo2DWidth",
        "AttributeVideo2DHeight",
        scene_path,
        object_name,
        attribute_index,
        width,
        height);
}

bool SetSceneObjectAttributeVideo2DTint(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& tint)
{
    return SetSceneObjectAttributeColorValue("AttributeVideo2DTint", scene_path, object_name, attribute_index, tint);
}

bool SetSceneObjectAttributeVideo2DAlpha(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float alpha)
{
    return SetSceneObjectAttributeScalar("AttributeVideo2DAlpha", scene_path, object_name, attribute_index, alpha);
}

bool SetSceneObjectAttributeVideo2DLockAspectRatio(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool lock_aspect_ratio)
{
    return SetSceneObjectAttributeBoolean("AttributeVideo2DLockAspectRatio", scene_path, object_name, attribute_index, lock_aspect_ratio);
}

bool SetSceneObjectAttributeVideo2DStretchToScreen(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool stretch_to_screen)
{
    return SetSceneObjectAttributeBoolean("AttributeVideo2DStretchToScreen", scene_path, object_name, attribute_index, stretch_to_screen);
}

bool SetSceneObjectAttributeVideo2DPriority(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, int priority)
{
    bool updated = false;
    const bool rewrite_succeeded = RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        std::size_t attribute_start = 0;
        std::size_t attribute_end = 0;
        if (!FindSceneObjectAttributeBlock(lines, object_start, object_end, attribute_index, attribute_start, attribute_end))
        {
            return;
        }

        const std::string key_prefix = std::string("AttributeVideo2DPriority") + ":";
        const std::string new_line = std::string("AttributeVideo2DPriority") + ": " + FormatInteger(priority);
        for (std::size_t index = attribute_start + 1; index < attribute_end; ++index)
        {
            if (StartsWith(TrimCopy(lines[index]), key_prefix))
            {
                lines[index] = new_line;
                updated = true;
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(attribute_end), new_line);
        updated = true;
    });

    return rewrite_succeeded && updated;
}

bool SetSceneObjectAttributeVideo2DPlayMode(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectVideoPlayMode play_mode)
{
    const std::string value = (play_mode == SceneObjectVideoPlayMode::Loop) ? "Loop"
        : (play_mode == SceneObjectVideoPlayMode::PlayOnce) ? "PlayOnce"
        : "Off";
    return SetSceneObjectAttributeStringValue("AttributeVideo2DPlayMode", scene_path, object_name, attribute_index, value);
}

bool SetSceneObjectAttributeVideo2DVolume(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float volume)
{
    return SetSceneObjectAttributeScalar("AttributeVideo2DVolume", scene_path, object_name, attribute_index, volume);
}

bool SetSceneObjectAttributeVideo2DMuted(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool muted)
{
    return SetSceneObjectAttributeBoolean("AttributeVideo2DMuted", scene_path, object_name, attribute_index, muted);
}

bool SetSceneObjectAttributeEffectsPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& effect_path)
{
    return SetSceneObjectAttributeStringValue("AttributeEffectPath", scene_path, object_name, attribute_index, effect_path);
}

bool SetSceneObjectAttributeEffectsPlayMode(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectEffectsPlayMode play_mode)
{
    const std::string value = (play_mode == SceneObjectEffectsPlayMode::PlayOnce) ? "PlayOnce" : "Loop";
    return SetSceneObjectAttributeStringValue("AttributeEffectPlayMode", scene_path, object_name, attribute_index, value);
}

bool SetSceneObjectAttributeShaderType(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, SceneObjectShaderType type)
{
    std::string value;
    switch (type)
    {
    case SceneObjectShaderType::Water: value = "Water"; break;
    case SceneObjectShaderType::Cloud: value = "Cloud"; break;
    case SceneObjectShaderType::Fire:  value = "Fire";  break;
    case SceneObjectShaderType::Rain:  value = "Rain";  break;
    case SceneObjectShaderType::Puddle:     value = "Puddle";     break;
    case SceneObjectShaderType::RainParticles: value = "RainParticles"; break;
    case SceneObjectShaderType::Fog:   value = "Fog";   break;
    default:                           value = "None";  break;
    }
    return SetSceneObjectAttributeStringValue("AttributeShaderType", scene_path, object_name, attribute_index, value);
}

bool SetSceneObjectAttributeShaderDropScale(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float drop_scale)
{
    return SetSceneObjectAttributeScalar("AttributeShaderDropScale", scene_path, object_name, attribute_index, drop_scale);
}

bool SetSceneObjectAttributeShaderDropSpeed(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float drop_speed)
{
    return SetSceneObjectAttributeScalar("AttributeShaderDropSpeed", scene_path, object_name, attribute_index, drop_speed);
}

bool SetSceneObjectAttributeShaderColor(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const SceneColor3& color)
{
    return SetSceneObjectAttributeColorValue("AttributeShaderColor", scene_path, object_name, attribute_index, color);
}

bool SetSceneObjectAttributeShaderFogDensity(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, float fog_density)
{
    return SetSceneObjectAttributeScalar("AttributeShaderFogDensity", scene_path, object_name, attribute_index, fog_density);
}

bool SetSceneObjectAttributeShape3DPath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& shape_path)
{
    return SetSceneObjectAttributeStringValue("AttributeShape3DPath", scene_path, object_name, attribute_index, shape_path);
}

bool SetSceneReferenceViewportSize(const std::filesystem::path& scene_path, std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return false;
    }

    std::vector<std::string> lines = ReadSceneLines(scene_path);
    if (lines.empty() && !std::filesystem::exists(scene_path))
    {
        return false;
    }

    const std::string width_line = "ReferenceViewportWidth: " + std::to_string(width);
    const std::string height_line = "ReferenceViewportHeight: " + std::to_string(height);

    bool width_found = false;
    bool height_found = false;
    std::size_t insert_index = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::string trimmed = TrimCopy(lines[index]);
        if (insert_index == lines.size() && IsSceneObjectStart(trimmed))
        {
            insert_index = index;
        }

        if (StartsWith(trimmed, "ReferenceViewportWidth:"))
        {
            lines[index] = width_line;
            width_found = true;
            continue;
        }

        if (StartsWith(trimmed, "ReferenceViewportHeight:"))
        {
            lines[index] = height_line;
            height_found = true;
            continue;
        }
    }

    if (insert_index == lines.size())
    {
        insert_index = lines.size();
    }

    if (!width_found)
    {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_index), width_line);
        ++insert_index;
    }

    if (!height_found)
    {
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_index), height_line);
    }

    return WriteSceneLines(scene_path, lines);
}

bool SetSceneObjectModel(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& model_path)
{
    return UpdateSceneObjectAttachment(scene_path, object_name, project_root, model_path, "Model");
}

bool ClearSceneObjectModel(const std::filesystem::path& scene_path, const std::string& object_name)
{
    return RewriteSceneObjectLines(scene_path, object_name, [](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        for (std::size_t index = object_start + 1; index < object_end;)
        {
            const std::string trimmed = TrimCopy(lines[index]);
            if (StartsWith(trimmed, "Model:") || StartsWith(trimmed, "ModelVisualOffset:"))
            {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
                --object_end;
                continue;
            }
            ++index;
        }
    });
}

bool AddSceneObjectScript(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& script_path)
{
    const std::string relative_script_path = MakeRelativePath(project_root, script_path);
    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        const std::string new_line = "Script: " + relative_script_path;
        for (std::size_t index = object_start + 1; index < object_end; ++index)
        {
            if (TrimCopy(lines[index]) == new_line)
            {
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(object_end), new_line);
    });
}

bool RemoveSceneObjectScript(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& script_path)
{
    const std::string relative_script_path = MakeRelativePath(project_root, script_path);
    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        const std::string target_line = "Script: " + relative_script_path;
        for (std::size_t index = object_start + 1; index < object_end; ++index)
        {
            if (TrimCopy(lines[index]) == target_line)
            {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
                return;
            }
        }
    });
}

bool AddSceneObjectGraph(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& graph_path)
{
    const std::string relative_graph_path = MakeRelativePath(project_root, graph_path);
    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        const std::string new_line = "Graph: " + relative_graph_path;
        for (std::size_t index = object_start + 1; index < object_end; ++index)
        {
            if (TrimCopy(lines[index]) == new_line)
            {
                return;
            }
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(object_end), new_line);
    });
}

bool RemoveSceneObjectGraph(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& graph_path)
{
    const std::string relative_graph_path = MakeRelativePath(project_root, graph_path);
    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
    {
        const std::string target_line = "Graph: " + relative_graph_path;
        for (std::size_t index = object_start + 1; index < object_end; ++index)
        {
            if (TrimCopy(lines[index]) == target_line)
            {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index));
                return;
            }
        }
    });
}