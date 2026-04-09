#include "assets/SceneMetadata.h"

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

std::string ExtractValue(std::string_view line, std::string_view prefix)
{
    return TrimCopy(std::string(line.substr(prefix.size())));
}

bool IsSceneObjectStart(std::string_view line)
{
    return StartsWith(line, "Object:");
}

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
    std::ifstream input(scene_path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
    {
        lines.push_back(line);
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

bool RewriteSceneObjectLines(
    const std::filesystem::path& scene_path,
    const std::string& object_name,
    const std::function<void(std::vector<std::string>&, std::size_t, std::size_t)>& mutator)
{
    std::ifstream input(scene_path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
    {
        lines.push_back(line);
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
}

SceneMetadata LoadSceneMetadata(const std::filesystem::path& scene_path)
{
    SceneMetadata metadata;

    std::ifstream input(scene_path, std::ios::binary);
    if (!input)
    {
        metadata.error_message = "Failed to open scene file.";
        return metadata;
    }

    SceneObjectMetadata* current_object = nullptr;
    std::string line;
    while (std::getline(input, line))
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
            continue;
        }

        if (StartsWith(trimmed, "Object:"))
        {
            SceneObjectMetadata object;
            object.name = ExtractValue(trimmed, "Object:");
            metadata.objects.push_back(std::move(object));
            current_object = &metadata.objects.back();
            continue;
        }

        if (current_object == nullptr)
        {
            continue;
        }

        if (StartsWith(trimmed, "Type:"))
        {
            current_object->type = ExtractValue(trimmed, "Type:");
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
        else if (StartsWith(trimmed, "Model:"))
        {
            current_object->model_path = ExtractValue(trimmed, "Model:");
        }
        else if (StartsWith(trimmed, "Script:"))
        {
            current_object->script_paths.push_back(ExtractValue(trimmed, "Script:"));
        }
        else if (StartsWith(trimmed, "Graph:"))
        {
            current_object->graph_paths.push_back(ExtractValue(trimmed, "Graph:"));
        }
    }

    metadata.parsed = true;
    return metadata;
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
            if (StartsWith(TrimCopy(lines[index]), "Model:"))
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