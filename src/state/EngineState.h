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

enum class EngineBuildType
{
    Debug,
    Final,
};

struct EngineBuildRequest
{
    std::string game_name;
    std::filesystem::path output_root;
    EngineBuildType build_type = EngineBuildType::Debug;

    std::filesystem::path GetStageDirectory() const
    {
        return output_root / game_name;
    }
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
    bool request_build_game_dialog = false;
    bool show_files_panel = true;
    bool show_workspace_panel = true;
    bool show_settings_panel = true;
    bool show_info_panel = true;
    bool show_log_panel = true;
    std::filesystem::path workspace_root;
    std::filesystem::path project_root;
    std::filesystem::path project_file_path;
    std::filesystem::path active_scene_path;
    std::filesystem::path selected_item_path;
    std::string selected_scene_object_name;
    std::filesystem::path open_file_path;
    std::filesystem::path requested_graph_path;
    std::filesystem::path open_graph_path;
    float ui_scale = 1.0f;
    bool show_grid_overlay = true;
    bool snap_to_grid = true;
    float grid_size = 1.0f;
    float grid_extent = 64.0f;
    bool auto_open_startup_scene = true;
    bool confirm_before_delete = true;
    bool auto_save_on_focus_loss = false;
    int auto_save_interval_minutes = 5;
    bool highlight_drop_targets = true;
    bool wrap_editor_text = false;
    bool request_files_tree_refresh = false;
    bool play_start_requested = false;
    bool play_stop_requested = false;
    bool play_restart_requested = false;
    bool is_playing = false;
    bool has_pending_build_request = false;
    std::filesystem::path playing_scene_path;
    std::string last_play_error;
    std::string last_build_error;
    std::string saved_file_contents;
    std::string open_file_contents;
    EngineBuildRequest pending_build_request{};
    std::vector<char> editor_buffer = std::vector<char>(kEditorBufferCapacity, '\0');
    bool open_file_dirty = false;
    bool open_graph_dirty = false;
    bool graph_reload_requested = false;
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

    bool CanBuildProject() const
    {
        return HasOpenProject();
    }

    bool CanPlayScene() const
    {
        return HasActiveScene();
    }

    void TriggerBuildAction()
    {
        if (!CanBuildProject())
        {
            SetBuildError("Cannot build: no project is loaded");
            return;
        }

        request_build_game_dialog = true;
        last_build_error.clear();
        AddLog("Opening Build Game dialog");
    }

    void SetBuildError(std::string message)
    {
        last_build_error = std::move(message);
        AddLog(last_build_error);
    }

    void QueueBuildRequest(EngineBuildRequest request)
    {
        pending_build_request = std::move(request);
        has_pending_build_request = true;
        last_build_error.clear();

        const std::string build_type_label = pending_build_request.build_type == EngineBuildType::Debug ? "Debug" : "Final";
        AddLog(
            "Queued game build request: name='" + pending_build_request.game_name +
            "', config=" + build_type_label +
            ", output='" + pending_build_request.GetStageDirectory().generic_string() + "'");
    }

    void TriggerPlayAction()
    {
        if (!CanPlayScene())
        {
            last_play_error = "Cannot play: no active scene is available";
            AddLog(last_play_error);
            return;
        }

        if (is_playing)
        {
            play_restart_requested = true;
            play_stop_requested = true;
            AddLog("Restarting runtime session");
            return;
        }

        play_start_requested = true;
        play_stop_requested = false;
        play_restart_requested = false;
        last_play_error.clear();
        AddLog("Starting runtime session");
    }

    void ClearPlayRequests()
    {
        play_start_requested = false;
        play_stop_requested = false;
        play_restart_requested = false;
    }

    void SetPlayError(std::string message)
    {
        last_play_error = std::move(message);
        AddLog(last_play_error);
    }

    bool HasSelectedItem() const
    {
        return !selected_item_path.empty();
    }

    bool HasActiveScene() const
    {
        return !active_scene_path.empty();
    }

    bool IsActiveScene(const std::filesystem::path& path) const
    {
        return !active_scene_path.empty() && active_scene_path == path;
    }

    bool HasSelectedSceneObject() const
    {
        return !selected_scene_object_name.empty();
    }

    void SetSelectedItem(std::filesystem::path path)
    {
        selected_item_path = std::move(path);
        selected_scene_object_name.clear();
    }

    void SetSelectedSceneObject(std::filesystem::path scene_path, std::string object_name)
    {
        selected_item_path = std::move(scene_path);
        selected_scene_object_name = std::move(object_name);
    }

    void ClearOpenProject()
    {
        project_root.clear();
        project_file_path.clear();
        request_build_game_dialog = false;
        active_scene_path.clear();
        selected_item_path.clear();
        selected_scene_object_name.clear();
        open_file_path.clear();
        requested_graph_path.clear();
        open_graph_path.clear();
        saved_file_contents.clear();
        open_file_contents.clear();
        open_file_dirty = false;
        open_graph_dirty = false;
        graph_reload_requested = false;
        request_files_tree_refresh = false;
        ClearPlayRequests();
        is_playing = false;
        has_pending_build_request = false;
        playing_scene_path.clear();
        last_play_error.clear();
        last_build_error.clear();
        pending_build_request = {};
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

    bool HasOpenGraph() const
    {
        return !open_graph_path.empty();
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
        const std::filesystem::path previous_active_scene_path = active_scene_path;
        active_scene_path = RemapMovedPath(active_scene_path, from_path, to_path);
        selected_item_path = RemapMovedPath(selected_item_path, from_path, to_path);
        open_file_path = RemapMovedPath(open_file_path, from_path, to_path);
        requested_graph_path = RemapMovedPath(requested_graph_path, from_path, to_path);
        open_graph_path = RemapMovedPath(open_graph_path, from_path, to_path);

        if (!previous_active_scene_path.empty() && active_scene_path != previous_active_scene_path)
        {
            SaveActiveSceneToProject();
        }
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

        if (!selected_item_path.empty())
        {
            const std::filesystem::path remapped_selected_item = RemapMovedPath(selected_item_path, deleted_path, {});
            if (remapped_selected_item != selected_item_path)
            {
                selected_item_path.clear();
                selected_scene_object_name.clear();
                AddLog("Cleared deleted selection");
            }
        }

        if (!active_scene_path.empty())
        {
            const std::filesystem::path remapped_active_scene = RemapMovedPath(active_scene_path, deleted_path, {});
            if (remapped_active_scene != active_scene_path)
            {
                active_scene_path.clear();
                SaveActiveSceneToProject();
                AddLog("Cleared deleted active scene");
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

        if (!requested_graph_path.empty())
        {
            const std::filesystem::path remapped_requested_graph = RemapMovedPath(requested_graph_path, deleted_path, {});
            if (remapped_requested_graph != requested_graph_path)
            {
                requested_graph_path.clear();
            }
        }

        if (!open_graph_path.empty())
        {
            const std::filesystem::path remapped_open_graph = RemapMovedPath(open_graph_path, deleted_path, {});
            if (remapped_open_graph != open_graph_path)
            {
                open_graph_path.clear();
                open_graph_dirty = false;
                graph_reload_requested = false;
                AddLog("Closed deleted graph asset");
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

    std::string GetOpenGraphDisplayPath() const
    {
        return GetDisplayPath(open_graph_path);
    }

    std::string GetSelectedItemDisplayPath() const
    {
        return GetDisplayPath(selected_item_path);
    }

    std::string GetOpenProjectDisplayPath() const
    {
        return GetDisplayPath(project_root);
    }

    std::string GetActiveSceneDisplayPath() const
    {
        return GetDisplayPath(active_scene_path);
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
            extension == ".graph" ||
                extension == ".engineproj" ||
            extension == ".scene" ||
            extension == ".mat" ||
            path.filename() == "CMakeLists.txt";
    }

    static bool IsGraphFile(const std::filesystem::path& path)
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        return extension == ".graph";
    }

    void RequestOpenGraphFile(const std::filesystem::path& path)
    {
        requested_graph_path = path;
        RequestTab(WorkspaceTab::Graph);
    }

    void RequestReloadOpenGraph()
    {
        if (HasOpenGraph())
        {
            graph_reload_requested = true;
            RequestTab(WorkspaceTab::Graph);
        }
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

    static bool ReplaceProjectValue(std::string& manifest_contents, const std::string& key, const std::string& value)
    {
        const std::string needle = "\"" + key + "\"";
        const std::size_t key_position = manifest_contents.find(needle);
        if (key_position == std::string::npos)
        {
            return false;
        }

        const std::size_t colon_position = manifest_contents.find(':', key_position + needle.size());
        if (colon_position == std::string::npos)
        {
            return false;
        }

        const std::size_t first_quote = manifest_contents.find('"', colon_position + 1);
        if (first_quote == std::string::npos)
        {
            return false;
        }

        const std::size_t second_quote = manifest_contents.find('"', first_quote + 1);
        if (second_quote == std::string::npos)
        {
            return false;
        }

        manifest_contents.replace(first_quote + 1, second_quote - first_quote - 1, value);
        return true;
    }

    bool SaveActiveSceneToProject()
    {
        if (project_file_path.empty())
        {
            return false;
        }

        std::ifstream input(project_file_path, std::ios::binary);
        if (!input)
        {
            AddLog("Failed to open project manifest for active scene update");
            return false;
        }

        std::string manifest_contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        const std::string relative_scene_path = active_scene_path.empty()
            ? std::string()
            : std::filesystem::relative(active_scene_path, project_root).generic_string();
        if (!ReplaceProjectValue(manifest_contents, "startupScene", relative_scene_path))
        {
            AddLog("Failed to update startupScene in project manifest");
            return false;
        }

        std::ofstream output(project_file_path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            AddLog("Failed to write project manifest after active scene update");
            return false;
        }

        output.write(manifest_contents.data(), static_cast<std::streamsize>(manifest_contents.size()));
        return static_cast<bool>(output);
    }

    bool SetActiveScene(const std::filesystem::path& scene_path)
    {
        if (scene_path.extension() != ".scene")
        {
            AddLog("Cannot set active scene: selected item is not a scene");
            return false;
        }

        if (!std::filesystem::exists(scene_path))
        {
            AddLog("Cannot set active scene: scene file does not exist");
            return false;
        }

        active_scene_path = scene_path;
        if (!SaveActiveSceneToProject())
        {
            return false;
        }

        AddLog("Set active scene: " + GetDisplayPath(active_scene_path));
        return true;
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
            active_scene_path = scene_path;
            if (std::filesystem::exists(scene_path) && OpenTextFile(scene_path))
            {
                AddLog("Loaded startup scene: " + GetDisplayPath(scene_path));
                return true;
            }
        }

        active_scene_path.clear();

        return OpenTextFile(manifest_path);
    }

    bool OpenTextFile(const std::filesystem::path& path)
    {
        if (IsGraphFile(path))
        {
            RequestOpenGraphFile(path);
            AddLog("Opening graph asset: " + GetDisplayPath(path));
            return true;
        }

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