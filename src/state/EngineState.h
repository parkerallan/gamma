#pragma once

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

enum class WorkspaceTab
{
    Scene,
    Graph,
    Editor,
};

struct EngineState
{
    static constexpr std::size_t kEditorBufferCapacity = 512 * 1024;

    WorkspaceTab active_tab = WorkspaceTab::Scene;
    WorkspaceTab requested_tab = WorkspaceTab::Scene;
    bool dock_layout_built = false;
    bool auto_scroll_log = true;
    bool has_requested_tab = false;
    bool request_open_project_dialog = false;
    bool request_new_project_dialog = false;
    std::filesystem::path workspace_root;
    std::filesystem::path project_root;
    std::filesystem::path project_file_path;
    std::filesystem::path open_file_path;
    float ui_scale = 1.0f;
    bool show_grid_overlay = true;
    bool snap_to_grid = true;
    float grid_size = 32.0f;
    bool auto_open_startup_scene = true;
    bool confirm_before_delete = true;
    bool auto_save_on_focus_loss = false;
    int auto_save_interval_minutes = 5;
    bool highlight_drop_targets = true;
    bool wrap_editor_text = false;
    std::string saved_file_contents;
    std::string open_file_contents;
    std::vector<char> editor_buffer = std::vector<char>(kEditorBufferCapacity, '\0');
    bool open_file_dirty = false;
    std::vector<std::string> log_messages;

    void AddLog(const std::string& message)
    {
        log_messages.push_back(message);
        std::cout << message << std::endl;
    }

    void SetWorkspaceRoot(std::filesystem::path root)
    {
        workspace_root = std::move(root);
    }

    bool HasOpenProject() const
    {
        return !project_root.empty();
    }

    void ClearOpenProject()
    {
        project_root.clear();
        project_file_path.clear();
        open_file_path.clear();
        saved_file_contents.clear();
        open_file_contents.clear();
        open_file_dirty = false;
        std::fill(editor_buffer.begin(), editor_buffer.end(), '\0');
        AddLog("Closed active project");
    }

    void RequestTab(WorkspaceTab tab)
    {
        requested_tab = tab;
        has_requested_tab = true;
    }

    ImGuiTabItemFlags GetTabSelectionFlags(WorkspaceTab tab) const
    {
        return has_requested_tab && requested_tab == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    }

    void CompleteTabRequest(WorkspaceTab tab)
    {
        if (has_requested_tab && requested_tab == tab)
        {
            has_requested_tab = false;
        }
    }

    bool HasOpenFile() const
    {
        return !open_file_path.empty();
    }

    static std::filesystem::path RemapMovedPath(const std::filesystem::path& current_path, const std::filesystem::path& from_path, const std::filesystem::path& to_path)
    {
        if (current_path.empty())
        {
            return current_path;
        }

        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(current_path, from_path, error);
        if (error)
        {
            return current_path;
        }

        const std::string relative_string = relative.generic_string();
        if (relative_string == ".." || relative_string.rfind("../", 0) == 0)
        {
            return current_path;
        }

        if (relative == ".")
        {
            return to_path;
        }

        return to_path / relative;
    }

    void UpdatePathsAfterMove(const std::filesystem::path& from_path, const std::filesystem::path& to_path)
    {
        project_file_path = RemapMovedPath(project_file_path, from_path, to_path);
        open_file_path = RemapMovedPath(open_file_path, from_path, to_path);
    }

    void UpdatePathsAfterDelete(const std::filesystem::path& deleted_path)
    {
        if (deleted_path.empty())
        {
            return;
        }

        if (!project_file_path.empty())
        {
            const std::filesystem::path remapped_project_file = RemapMovedPath(project_file_path, deleted_path, {});
            if (remapped_project_file != project_file_path)
            {
                ClearOpenProject();
                return;
            }
        }

        if (!open_file_path.empty())
        {
            const std::filesystem::path remapped_open_file = RemapMovedPath(open_file_path, deleted_path, {});
            if (remapped_open_file != open_file_path)
            {
                open_file_path.clear();
                saved_file_contents.clear();
                open_file_contents.clear();
                open_file_dirty = false;
                std::fill(editor_buffer.begin(), editor_buffer.end(), '\0');
                AddLog("Closed deleted file from editor");
            }
        }
    }

    std::string GetDisplayPath(const std::filesystem::path& path) const
    {
        if (path.empty())
        {
            return std::string();
        }

        std::error_code error;
        if (!project_root.empty())
        {
            const std::filesystem::path project_relative = std::filesystem::relative(path, project_root, error);
            const std::string project_relative_string = project_relative.generic_string();
            if (!error && !project_relative.empty() && project_relative_string != ".." && project_relative_string.rfind("../", 0) != 0)
            {
                if (project_relative == ".")
                {
                    return path.filename().generic_string();
                }

                return project_relative.generic_string();
            }
        }

        error.clear();
        if (!workspace_root.empty())
        {
            const std::filesystem::path relative = std::filesystem::relative(path, workspace_root, error);
            if (!error && !relative.empty())
            {
                return relative.generic_string();
            }
        }

        return path.generic_string();
    }

    std::string GetOpenFileDisplayPath() const
    {
        return GetDisplayPath(open_file_path);
    }

    std::string GetOpenProjectDisplayPath() const
    {
        return GetDisplayPath(project_root);
    }

    static bool IsSupportedTextFile(const std::filesystem::path& path)
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });

        return extension == ".cpp" ||
            extension == ".c" ||
            extension == ".cc" ||
            extension == ".cxx" ||
            extension == ".h" ||
            extension == ".hpp" ||
            extension == ".hh" ||
            extension == ".hxx" ||
            extension == ".inl" ||
            extension == ".ipp" ||
            extension == ".ixx" ||
            extension == ".txt" ||
            extension == ".md" ||
            extension == ".cmake" ||
            extension == ".json" ||
            extension == ".ini" ||
            extension == ".toml" ||
            extension == ".yml" ||
            extension == ".yaml" ||
                extension == ".engineproj" ||
            extension == ".scene" ||
            extension == ".mat" ||
            path.filename() == "CMakeLists.txt";
    }

    static std::string ExtractProjectValue(const std::string& manifest_contents, const std::string& key)
    {
        const std::string needle = "\"" + key + "\"";
        const std::size_t key_position = manifest_contents.find(needle);
        if (key_position == std::string::npos)
        {
            return std::string();
        }

        const std::size_t colon_position = manifest_contents.find(':', key_position + needle.size());
        if (colon_position == std::string::npos)
        {
            return std::string();
        }

        const std::size_t first_quote = manifest_contents.find('"', colon_position + 1);
        if (first_quote == std::string::npos)
        {
            return std::string();
        }

        const std::size_t second_quote = manifest_contents.find('"', first_quote + 1);
        if (second_quote == std::string::npos)
        {
            return std::string();
        }

        return manifest_contents.substr(first_quote + 1, second_quote - first_quote - 1);
    }

    bool LoadProject(const std::filesystem::path& manifest_path)
    {
        if (manifest_path.extension() != ".engineproj")
        {
            AddLog("Failed to load project: expected a .engineproj file");
            return false;
        }

        std::ifstream input(manifest_path, std::ios::binary);
        if (!input)
        {
            AddLog("Failed to load project file: " + manifest_path.generic_string());
            return false;
        }

        const std::string manifest_contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const std::filesystem::path resolved_project_root = manifest_path.parent_path();
        const std::string startup_scene = ExtractProjectValue(manifest_contents, "startupScene");

        project_root = resolved_project_root;
        project_file_path = manifest_path;
        AddLog("Opened project: " + GetDisplayPath(project_root));

        if (!startup_scene.empty())
        {
            const std::filesystem::path scene_path = resolved_project_root / startup_scene;
            if (std::filesystem::exists(scene_path) && OpenTextFile(scene_path))
            {
                AddLog("Loaded startup scene: " + GetDisplayPath(scene_path));
                return true;
            }
        }

        return OpenTextFile(manifest_path);
    }

    bool OpenTextFile(const std::filesystem::path& path)
    {
        if (!IsSupportedTextFile(path))
        {
            AddLog("Skipped non-text file: " + GetDisplayPath(path));
            return false;
        }

        std::error_code error;
        const std::uintmax_t file_size = std::filesystem::file_size(path, error);
        if (error)
        {
            AddLog("Failed to inspect file: " + GetDisplayPath(path));
            return false;
        }

        if (file_size >= kEditorBufferCapacity)
        {
            AddLog("Refused to open large file in engine: " + GetDisplayPath(path));
            return false;
        }

        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            AddLog("Failed to open text file: " + GetDisplayPath(path));
            return false;
        }

        saved_file_contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        open_file_contents = saved_file_contents;
        open_file_path = path;
        open_file_dirty = false;
        std::fill(editor_buffer.begin(), editor_buffer.end(), '\0');
        std::copy(open_file_contents.begin(), open_file_contents.end(), editor_buffer.begin());
        RequestTab(WorkspaceTab::Editor);
        AddLog("Opened text file in engine: " + GetDisplayPath(path));
        return true;
    }

    void SyncEditorTextFromBuffer()
    {
        open_file_contents = editor_buffer.data();
    }

    bool SaveOpenFile()
    {
        if (!HasOpenFile())
        {
            return false;
        }

        SyncEditorTextFromBuffer();

        std::ofstream output(open_file_path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            AddLog("Failed to save file: " + GetOpenFileDisplayPath());
            return false;
        }

        output.write(open_file_contents.data(), static_cast<std::streamsize>(open_file_contents.size()));
        if (!output)
        {
            AddLog("Failed while writing file: " + GetOpenFileDisplayPath());
            return false;
        }

        saved_file_contents = open_file_contents;
        open_file_dirty = false;
        AddLog("Saved file: " + GetOpenFileDisplayPath());
        return true;
    }
};