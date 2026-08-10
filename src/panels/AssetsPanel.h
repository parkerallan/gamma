#pragma once

#include "components/CreationMenu.h"
#include "components/FileContextMenu.h"
#include "state/EngineState.h"

#include <filesystem>
#include <string>
#include <vector>

// Tile browser for the project's Assets/ folder, which the Files tree leaves
// to this panel: browsing, import, and the drag sources the other panels
// accept (FILE_TREE_PATH for files, PREFAB_NAME for prefab entries).
class AssetsPanel
{
public:
    void Render(EngineState& state);

private:
    enum class AssetKind
    {
        Folder,
        Image,
        Audio,
        Video,
        Model,
        Font,
        Effect,
        Prefab,
        Text,
        Generic,
    };

    struct AssetEntry
    {
        std::filesystem::path path;
        std::string name;
        AssetKind kind = AssetKind::Generic;
        // Prefab tiles are rows of Prefabs.metadata rather than files on disk;
        // they drag as PREFAB_NAME and have no filesystem operations.
        bool is_prefab_entry = false;
    };

    // Deferred so the filesystem is never mutated while the grid is mid-draw.
    struct PendingMove
    {
        std::filesystem::path source;
        std::filesystem::path destination_directory;
    };

    std::filesystem::path selected_;
    std::filesystem::path cached_directory_;
    std::vector<AssetEntry> entries_;
    PendingMove pending_move_;
    double last_refresh_time_ = 0.0;
    bool has_pending_move_ = false;
    bool refresh_requested_ = false;
    CreationMenu creation_menu_;
    FileContextMenu file_context_menu_;

    void RefreshEntries(EngineState& state, const std::filesystem::path& directory);
    void RenderBreadcrumb(EngineState& state);
    void RenderGrid(EngineState& state, const std::filesystem::path& directory);
    void RenderMoveTarget(EngineState& state, const std::filesystem::path& destination_directory);
    bool ApplyPendingMove(EngineState& state);
};
