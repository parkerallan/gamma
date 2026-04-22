#pragma once

#include "components/CreationMenu.h"
#include "components/FileContextMenu.h"
#include "dialogs/BuildGameDialog.h"
#include "dialogs/NewProjectDialog.h"
#include "dialogs/OpenProjectDialog.h"
#include "state/EngineState.h"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct FileTreeNode
{
    std::filesystem::path path;
    std::string label;
    bool is_directory = false;
    bool is_scene_object = false;
    bool has_camera_attribute = false;
    bool is_active_camera = false;
    std::vector<FileTreeNode> children;
};

class FilesPanel
{
public:
    FilesPanel();

    void Render(EngineState& state);

private:
    static constexpr const char* kFileTreeDragDropPayload = "FILE_TREE_PATH";
    static constexpr const char* kSceneObjectDragDropPayload = "SCENE_OBJECT_PATH";

    std::filesystem::path current_root_;
    std::vector<FileTreeNode> roots_;
    std::array<char, 160> search_buffer_{};
    std::array<char, 160> scene_object_name_buffer_{};
    bool refresh_requested_ = false;
    std::filesystem::path scene_object_clipboard_scene_path_;
    std::string scene_object_clipboard_name_;
    std::filesystem::path scene_object_action_scene_path_;
    std::string scene_object_action_name_;
    bool focus_scene_object_name_input_ = false;
    bool open_scene_object_rename_popup_ = false;
    bool open_scene_object_delete_popup_ = false;
    FileContextMenu file_context_menu_;
    CreationMenu creation_menu_;
    BuildGameDialog build_game_dialog_;
    NewProjectDialog new_project_dialog_;
    OpenProjectDialog open_project_dialog_;

    void RebuildTree(const std::filesystem::path& root);
    FileTreeNode BuildNode(const std::filesystem::path& path) const;
    std::vector<FileTreeNode> BuildSceneObjectNodes(const std::filesystem::path& scene_path) const;
    void RenderNode(
        const FileTreeNode& node,
        EngineState& state,
        std::string_view filter,
        int depth,
        const std::vector<bool>& ancestor_has_next,
        bool is_last_sibling);
    bool RenderSceneObjectMenu(const FileTreeNode& node, EngineState& state);
    bool RenderSceneObjectPopups(EngineState& state);
    bool RenderProjectRootDropTarget(EngineState& state);
    bool RenderMoveSource(const FileTreeNode& node, EngineState& state);
    bool RenderMoveTarget(const std::filesystem::path& destination_directory, EngineState& state);
    bool RenderSceneObjectMoveSource(const FileTreeNode& node, EngineState& state);
    bool RenderSceneObjectMoveTarget(const FileTreeNode& node, EngineState& state);
    bool HandleSceneObjectPaste(const std::filesystem::path& scene_path, const std::string& sibling_object_name, EngineState& state);
    bool RenameSceneObjectFromUi(EngineState& state);
    bool DeleteSceneObjectFromUi(EngineState& state);
    void QueueSceneObjectRename(const std::filesystem::path& scene_path, const std::string& object_name);
    void QueueSceneObjectDelete(const std::filesystem::path& scene_path, const std::string& object_name);
    void ResetSceneObjectRename();
    void ResetSceneObjectDelete();
    bool UpdateSceneObjectSelectionAfterMutation(EngineState& state, const std::string& new_selected_name = std::string());
    static std::string SanitizeSceneObjectName(std::string value);
    static std::string EncodeSceneObjectPayload(const std::filesystem::path& scene_path, const std::string& object_name);
    static bool DecodeSceneObjectPayload(std::string_view payload, std::filesystem::path& scene_path, std::string& object_name);
    bool MovePath(const std::filesystem::path& source_path, const std::filesystem::path& destination_directory, EngineState& state);
    bool NodeMatchesFilter(const FileTreeNode& node, std::string_view filter) const;
};