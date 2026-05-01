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

enum class EngineBuildPlatform
{
    Windows,
    Linux,
};

struct EngineBuildRequest
{
    std::string game_name;
    std::string folder_name;
    std::string window_title;
    std::filesystem::path output_root;
    std::filesystem::path app_icon_path;
    EngineBuildType build_type = EngineBuildType::Debug;
    EngineBuildPlatform build_platform = EngineBuildPlatform::Windows;

    std::filesystem::path GetStageDirectory() const
    {
        const std::string stage_folder = folder_name.empty() ? game_name : folder_name;
        return output_root / stage_folder;
    }

    std::string GetExecutableFileName() const
    {
        return build_platform == EngineBuildPlatform::Windows ? game_name + ".exe" : game_name;
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
    bool show_version_control_panel = true;
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
    std::string version_control_remote_url;
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
    bool is_build_running = false;
    bool has_pending_build_request = false;
    bool request_build_stop = false;
    std::filesystem::path playing_scene_path;
    std::string last_play_error;
    std::string last_build_error;
    std::string saved_file_contents;
    std::string open_file_contents;
    std::string build_executable_name = "Game";
    std::string build_folder_name = "Game";
    std::string build_window_title = "Game";
    EngineBuildPlatform build_target_platform = EngineBuildPlatform::Windows;
    std::filesystem::path build_output_root;
    std::filesystem::path build_app_icon_path;
    EngineBuildRequest pending_build_request{};
    std::vector<char> editor_buffer = std::vector<char>(kEditorBufferCapacity, '\0');
    bool open_file_dirty = false;
    bool open_graph_dirty = false;
    bool graph_reload_requested = false;
    std::vector<std::string> log_messages;
    std::vector<std::filesystem::path> recent_projects;

    void AddLog(const std::string& message)
    {
        log_messages.push_back(message);
        std::cout << message << std::endl;
    }

    void SetWorkspaceRoot(std::filesystem::path root)
    {
        workspace_root = std::move(root);
        LoadRecentProjects();
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

    void TriggerBuildStopAction()
    {
        if (!is_build_running)
        {
            return;
        }

        request_build_stop = true;
        AddLog("Stopping build...");
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
        const std::string build_platform_label = pending_build_request.build_platform == EngineBuildPlatform::Windows ? "Windows" : "Linux";
        AddLog(
            "Queued game build request: name='" + pending_build_request.game_name +
            "', folder='" + pending_build_request.folder_name +
            "', platform=" + build_platform_label +
            ", config=" + build_type_label +
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

    void TriggerPlayStopAction()
    {
        if (!is_playing)
        {
            return;
        }

        play_stop_requested = true;
        play_restart_requested = false;
        AddLog("Closing runtime session");
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
        version_control_remote_url.clear();
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
        is_build_running = false;
        has_pending_build_request = false;
        request_build_stop = false;
        playing_scene_path.clear();
        last_play_error.clear();
        last_build_error.clear();
        pending_build_request = {};
        build_executable_name = "Game";
        build_folder_name = "Game";
        build_window_title = "Game";
        build_target_platform = EngineBuildPlatform::Windows;
        build_output_root.clear();
        build_app_icon_path.clear();
        auto_open_startup_scene = true;
        confirm_before_delete = true;
        highlight_drop_targets = true;
        show_grid_overlay = true;
        snap_to_grid = true;
        grid_size = 1.0f;
        grid_extent = 64.0f;
        wrap_editor_text = false;
        auto_save_on_focus_loss = false;
        auto_save_interval_minutes = 5;
        ui_scale = 1.0f;
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
            extension == ".lua" ||
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

    static bool UpsertProjectValue(std::string& manifest_contents, const std::string& key, const std::string& value)
    {
        if (ReplaceProjectValue(manifest_contents, key, value))
        {
            return true;
        }

        const std::size_t closing_brace = manifest_contents.rfind('}');
        if (closing_brace == std::string::npos)
        {
            return false;
        }

        std::size_t non_ws_position = closing_brace;
        while (non_ws_position > 0 && std::isspace(static_cast<unsigned char>(manifest_contents[non_ws_position - 1])) != 0)
        {
            --non_ws_position;
        }

        const bool has_existing_entries = non_ws_position > 0 && manifest_contents[non_ws_position - 1] != '{';
        const std::string insertion =
            std::string(has_existing_entries ? ",\n" : "\n") +
            "  \"" + key + "\": \"" + value + "\"";

        manifest_contents.insert(closing_brace, insertion);
        return true;
    }

    static std::string SanitizeProjectValue(std::string value)
    {
        std::replace(value.begin(), value.end(), '"', '\'');
        std::replace(value.begin(), value.end(), '\\', '/');
        value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
        value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
        return value;
    }

    static bool IsPathWithin(const std::filesystem::path& parent, const std::filesystem::path& candidate)
    {
        if (parent.empty() || candidate.empty())
        {
            return false;
        }

        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(candidate, parent, error);
        if (error || relative.empty())
        {
            return false;
        }

        const std::string relative_string = relative.generic_string();
        return relative == "." || (relative_string != ".." && relative_string.rfind("../", 0) != 0);
    }

    static constexpr std::size_t kMaxRecentProjects = 10;

    std::filesystem::path GetRecentProjectsPath() const
    {
        if (workspace_root.empty())
        {
            return {};
        }
        return workspace_root / "recent_projects.txt";
    }

    void AddRecentProject(const std::filesystem::path& manifest_path)
    {
        if (manifest_path.empty())
        {
            return;
        }

        const std::filesystem::path normal = manifest_path.lexically_normal();

        auto it = std::find(recent_projects.begin(), recent_projects.end(), normal);
        if (it != recent_projects.end())
        {
            recent_projects.erase(it);
        }

        recent_projects.insert(recent_projects.begin(), normal);

        if (recent_projects.size() > kMaxRecentProjects)
        {
            recent_projects.resize(kMaxRecentProjects);
        }

        SaveRecentProjects();
    }

    bool LoadRecentProjects()
    {
        recent_projects.clear();

        const std::filesystem::path path = GetRecentProjectsPath();
        if (path.empty())
        {
            return false;
        }

        std::ifstream input(path);
        if (!input)
        {
            return false;
        }

        std::string line;
        while (std::getline(input, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            {
                line.pop_back();
            }

            if (line.empty())
            {
                continue;
            }

            recent_projects.emplace_back(line);

            if (recent_projects.size() >= kMaxRecentProjects)
            {
                break;
            }
        }

        return true;
    }

    bool SaveRecentProjects() const
    {
        const std::filesystem::path path = GetRecentProjectsPath();
        if (path.empty())
        {
            return false;
        }

        std::ofstream output(path, std::ios::trunc);
        if (!output)
        {
            return false;
        }

        for (const std::filesystem::path& p : recent_projects)
        {
            output << p.generic_string() << "\n";
        }

        return output.good();
    }

    std::filesystem::path GetProjectSettingsPath() const
    {
        if (project_root.empty())
        {
            return {};
        }
        return project_root / "Config" / "settings.ini";
    }

    bool LoadProjectSettings()
    {
        const std::filesystem::path settings_path = GetProjectSettingsPath();
        if (settings_path.empty())
        {
            return false;
        }

        std::ifstream input(settings_path);
        if (!input)
        {
            return false;
        }

        const auto parse_bool = [](const std::string& value) -> bool
        {
            return value == "true" || value == "1";
        };

        const auto parse_float = [](const std::string& value, float fallback) -> float
        {
            if (value.empty())
            {
                return fallback;
            }
            try { return std::stof(value); }
            catch (...) { return fallback; }
        };

        const auto parse_int = [](const std::string& value, int fallback) -> int
        {
            if (value.empty())
            {
                return fallback;
            }
            try { return std::stoi(value); }
            catch (...) { return fallback; }
        };

        std::string line;
        while (std::getline(input, line))
        {
            // Strip inline comments and trailing whitespace
            const auto comment_pos = line.find('#');
            if (comment_pos != std::string::npos)
            {
                line.erase(comment_pos);
            }

            while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\n' || line.back() == '\t'))
            {
                line.pop_back();
            }

            const auto eq_pos = line.find('=');
            if (eq_pos == std::string::npos || eq_pos == 0)
            {
                continue;
            }

            const std::string key = line.substr(0, eq_pos);
            const std::string value = line.substr(eq_pos + 1);

            if (key == "autoOpenStartupScene") auto_open_startup_scene = parse_bool(value);
            else if (key == "confirmBeforeDelete") confirm_before_delete = parse_bool(value);
            else if (key == "highlightDropTargets") highlight_drop_targets = parse_bool(value);
            else if (key == "showGridOverlay") show_grid_overlay = parse_bool(value);
            else if (key == "snapToGrid") snap_to_grid = parse_bool(value);
            else if (key == "gridSize") grid_size = parse_float(value, 1.0f);
            else if (key == "gridExtent") grid_extent = parse_float(value, 64.0f);
            else if (key == "wrapEditorText") wrap_editor_text = parse_bool(value);
            else if (key == "autoSaveOnFocusLoss") auto_save_on_focus_loss = parse_bool(value);
            else if (key == "autoSaveIntervalMinutes") auto_save_interval_minutes = parse_int(value, 5);
            else if (key == "uiScale") ui_scale = parse_float(value, 1.0f);
            else if (key == "versionControlRemoteUrl") version_control_remote_url = value;
        }

        return true;
    }

    bool SaveProjectSettings() const
    {
        const std::filesystem::path settings_path = GetProjectSettingsPath();
        if (settings_path.empty())
        {
            return false;
        }

        std::error_code error;
        std::filesystem::create_directories(settings_path.parent_path(), error);
        if (error)
        {
            return false;
        }

        std::ofstream output(settings_path, std::ios::trunc);
        if (!output)
        {
            return false;
        }

        const auto write_bool = [](bool value) -> const char*
        {
            return value ? "true" : "false";
        };

        output << "# Project settings — auto-saved by the editor. Do not edit manually.\n";
        output << "\n";
        output << "# Project\n";
        output << "autoOpenStartupScene=" << write_bool(auto_open_startup_scene) << "\n";
        output << "confirmBeforeDelete=" << write_bool(confirm_before_delete) << "\n";
        output << "highlightDropTargets=" << write_bool(highlight_drop_targets) << "\n";
        output << "\n";
        output << "# Viewport\n";
        output << "showGridOverlay=" << write_bool(show_grid_overlay) << "\n";
        output << "snapToGrid=" << write_bool(snap_to_grid) << "\n";
        output << "gridSize=" << grid_size << "\n";
        output << "gridExtent=" << grid_extent << "\n";
        output << "\n";
        output << "# Editor\n";
        output << "wrapEditorText=" << write_bool(wrap_editor_text) << "\n";
        output << "autoSaveOnFocusLoss=" << write_bool(auto_save_on_focus_loss) << "\n";
        output << "autoSaveIntervalMinutes=" << auto_save_interval_minutes << "\n";
        output << "uiScale=" << ui_scale << "\n";
        output << "\n";
        output << "# Version Control\n";
        output << "versionControlRemoteUrl=" << version_control_remote_url << "\n";

        return output.good();
    }

    bool SaveBuildSettingsToProject()
    {
        if (project_file_path.empty())
        {
            return false;
        }

        std::ifstream input(project_file_path, std::ios::binary);
        if (!input)
        {
            AddLog("Failed to open project manifest for build settings update");
            return false;
        }

        std::string manifest_contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};

        const auto encode_path = [this](const std::filesystem::path& path_value)
        {
            if (path_value.empty())
            {
                return std::string();
            }

            std::filesystem::path encoded_path = path_value.lexically_normal();
            std::error_code error;
            if (!project_root.empty() && encoded_path.is_absolute() && IsPathWithin(project_root, encoded_path))
            {
                encoded_path = std::filesystem::relative(encoded_path, project_root, error);
                if (error)
                {
                    encoded_path = path_value;
                }
            }

            return SanitizeProjectValue(encoded_path.generic_string());
        };

        if (!UpsertProjectValue(manifest_contents, "buildExeName", SanitizeProjectValue(build_executable_name)) ||
            !UpsertProjectValue(manifest_contents, "buildFolderName", SanitizeProjectValue(build_folder_name)) ||
            !UpsertProjectValue(manifest_contents, "buildWindowTitle", SanitizeProjectValue(build_window_title)) ||
            !UpsertProjectValue(manifest_contents, "buildPlatform", build_target_platform == EngineBuildPlatform::Linux ? "Linux" : "Windows") ||
            !UpsertProjectValue(manifest_contents, "buildOutputRoot", encode_path(build_output_root)) ||
            !UpsertProjectValue(manifest_contents, "buildAppIcon", encode_path(build_app_icon_path)))
        {
            AddLog("Failed to update build settings in project manifest");
            return false;
        }

        std::ofstream output(project_file_path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            AddLog("Failed to write project manifest after build settings update");
            return false;
        }

        output.write(manifest_contents.data(), static_cast<std::streamsize>(manifest_contents.size()));
        return static_cast<bool>(output);
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
        const std::string manifest_name = ExtractProjectValue(manifest_contents, "name");

        const std::string default_build_name = !manifest_name.empty() ? manifest_name : manifest_path.stem().string();

        build_executable_name = ExtractProjectValue(manifest_contents, "buildExeName");
        if (build_executable_name.empty())
        {
            build_executable_name = default_build_name.empty() ? "Game" : default_build_name;
        }

        build_folder_name = ExtractProjectValue(manifest_contents, "buildFolderName");
        if (build_folder_name.empty())
        {
            build_folder_name = build_executable_name;
        }

        build_window_title = ExtractProjectValue(manifest_contents, "buildWindowTitle");
        if (build_window_title.empty())
        {
            build_window_title = build_executable_name;
        }

        build_target_platform = EngineBuildPlatform::Windows;
        std::string build_platform_value = ExtractProjectValue(manifest_contents, "buildPlatform");
        std::transform(build_platform_value.begin(), build_platform_value.end(), build_platform_value.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (build_platform_value == "linux")
        {
            build_target_platform = EngineBuildPlatform::Linux;
        }

        build_output_root.clear();
        const std::string build_output_root_value = ExtractProjectValue(manifest_contents, "buildOutputRoot");
        if (!build_output_root_value.empty())
        {
            std::filesystem::path output_root_path(build_output_root_value);
            if (output_root_path.is_relative())
            {
                output_root_path = resolved_project_root / output_root_path;
            }
            build_output_root = output_root_path.lexically_normal();
        }

        build_app_icon_path.clear();
        const std::string build_app_icon_value = ExtractProjectValue(manifest_contents, "buildAppIcon");
        if (!build_app_icon_value.empty())
        {
            std::filesystem::path app_icon_path(build_app_icon_value);
            if (app_icon_path.is_relative())
            {
                app_icon_path = resolved_project_root / app_icon_path;
            }
            build_app_icon_path = app_icon_path.lexically_normal();
        }

        project_root = resolved_project_root;
        project_file_path = manifest_path;
        AddLog("Opened project: " + GetDisplayPath(project_root));
        LoadProjectSettings();
        AddRecentProject(manifest_path);

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