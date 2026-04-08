#pragma once

#include "components/CreationMenu.h"
#include "components/FileContextMenu.h"
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
    std::vector<FileTreeNode> children;
};

class FilesPanel
{
public:
    FilesPanel();

    void Render(EngineState& state);

private:
    static constexpr const char* kFileTreeDragDropPayload = "FILE_TREE_PATH";

    std::filesystem::path current_root_;
    std::vector<FileTreeNode> roots_;
    std::array<char, 160> search_buffer_{};
    bool refresh_requested_ = false;
    FileContextMenu file_context_menu_;
    CreationMenu creation_menu_;
    NewProjectDialog new_project_dialog_;
    OpenProjectDialog open_project_dialog_;

    void RebuildTree(const std::filesystem::path& root);
    FileTreeNode BuildNode(const std::filesystem::path& path) const;
    void RenderNode(const FileTreeNode& node, EngineState& state, std::string_view filter);
    bool RenderProjectRootDropTarget(EngineState& state);
    bool RenderMoveSource(const FileTreeNode& node, EngineState& state);
    bool RenderMoveTarget(const std::filesystem::path& destination_directory, EngineState& state);
    bool MovePath(const std::filesystem::path& source_path, const std::filesystem::path& destination_directory, EngineState& state);
    bool NodeMatchesFilter(const FileTreeNode& node, std::string_view filter) const;
};