#include "assets/SceneMetadata.h"
#include "vfs/AssetVFS.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>

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
        StartsWith(line, "AttributeFov:") ||
        StartsWith(line, "AttributeNearClip:") ||
    StartsWith(line, "AttributeFarClip:") ||
    StartsWith(line, "AttributeActive:") ||
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
    StartsWith(line, "AttributeImage2DImagePath:") ||
    StartsWith(line, "AttributeImage2DX:") ||
    StartsWith(line, "AttributeImage2DY:") ||
    StartsWith(line, "AttributeImage2DWidth:") ||
    StartsWith(line, "AttributeImage2DHeight:") ||
    StartsWith(line, "AttributeImage2DTint:") ||
    StartsWith(line, "AttributeImage2DAlpha:") ||
    StartsWith(line, "AttributeImage2DLockAspectRatio:") ||
    StartsWith(line, "AttributeSkyboxImagePath:");
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
}

const char* ToDisplayName(SceneObjectAttributeKind kind)
{
    switch (kind)
    {
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
    case SceneObjectAttributeKind::Text2D:
        return "Text 2D";
    case SceneObjectAttributeKind::Image2D:
        return "Image 2D";
    case SceneObjectAttributeKind::Skybox:
        return "Skybox";
    case SceneObjectAttributeKind::None:
    default:
        return "None";
    }
}

const char* ToStorageName(SceneObjectAttributeKind kind)
{
    switch (kind)
    {
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
    case SceneObjectAttributeKind::Text2D:
        return "Text2D";
    case SceneObjectAttributeKind::Image2D:
        return "Image2D";
    case SceneObjectAttributeKind::Skybox:
        return "Skybox";
    case SceneObjectAttributeKind::None:
    default:
        return "None";
    }
}

SceneObjectAttributeKind ParseSceneObjectAttributeKind(std::string_view value)
{
    const std::string trimmed = TrimCopy(std::string(value));
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
    if (trimmed == "Text2D")
    {
        return SceneObjectAttributeKind::Text2D;
    }
    if (trimmed == "Image2D")
    {
        return SceneObjectAttributeKind::Image2D;
    }
    if (trimmed == "Skybox")
    {
        return SceneObjectAttributeKind::Skybox;
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

    return RewriteSceneObjectLines(scene_path, object_name, [&](std::vector<std::string>& lines, std::size_t object_start, std::size_t object_end)
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
        else if (StartsWith(trimmed, "AttributeSkyboxImagePath:") && current_attribute != nullptr)
        {
            current_attribute->skybox.image_path = ExtractValue(trimmed, "AttributeSkyboxImagePath:");
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

    metadata.parsed = true;
    return metadata;
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

bool SetSceneObjectAttributeImage2DLockAspectRatio(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, bool lock_aspect_ratio)
{
    return SetSceneObjectAttributeBoolean("AttributeImage2DLockAspectRatio", scene_path, object_name, attribute_index, lock_aspect_ratio);
}

bool SetSceneObjectAttributeSkyboxImagePath(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index, const std::string& image_path)
{
    return SetSceneObjectAttributeStringValue("AttributeSkyboxImagePath", scene_path, object_name, attribute_index, image_path);
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