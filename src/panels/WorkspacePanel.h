#pragma once

#include "app/VulkanContext.h"
#include "assets/ModelAsset.h"
#include "assets/SceneMetadata.h"
#include "graph/GraphDocument.h"
#include "graph/NodeLibrary.h"
#include "panels/SceneViewportRenderer.h"
#include "state/EngineState.h"

#include <filesystem>
#include <memory>
#include <unordered_map>

struct CachedModelAssetEntry
{
    std::filesystem::file_time_type write_time{};
    ModelAsset asset{};
};

class WorkspacePanel
{
public:
    WorkspacePanel();
    ~WorkspacePanel();

    WorkspacePanel(const WorkspacePanel&) = delete;
    WorkspacePanel& operator=(const WorkspacePanel&) = delete;

    void Render(EngineState& state);
    void Shutdown();
    bool InitializeSceneRenderer(VulkanContext* context);
    void BeginFrame();
    void RenderSceneGpuPass();
    bool SaveOpenGraph(EngineState& state);
    bool ReloadOpenGraph(EngineState& state);

private:
    std::unique_ptr<ImFlow::ImNodeFlow> graph_;
    GraphDocument current_graph_document_;
    std::string saved_graph_contents_;
    SceneViewportRenderer scene_view_renderer_;
    SceneViewportCameraState scene_view_camera_{};
    std::filesystem::path cached_scene_path_;
    std::filesystem::file_time_type cached_scene_write_time_{};
    SceneMetadata cached_scene_metadata_{};
    bool has_cached_scene_metadata_ = false;
    std::unordered_map<std::filesystem::path, CachedModelAssetEntry> model_asset_cache_;

    ImFlow::ImNodeFlow& GetGraph();
    const CachedModelAssetEntry& GetModelAssetEntry(const std::filesystem::path& path);
    const SceneMetadata& GetSceneMetadata(const std::filesystem::path& path);
    const ModelAsset& GetModelAsset(const std::filesystem::path& path);
    bool LoadGraphFile(EngineState& state, const std::filesystem::path& path);
    void HandleGraphSessionRequests(EngineState& state);
    void RebuildGraphFromDocument();
    void SyncGraphDocumentFromUi(EngineState& state);
    void RenderSceneViewport(EngineState& state);
    void RenderGraphViewport(EngineState& state);
    void RenderEditorViewport(EngineState& state);
    void RenderNodeLibrary();
    void RenderNodeLibrarySection(const GraphNodeDefinition& definition);
    void HandleGraphNodeDrop(EngineState& state);
};