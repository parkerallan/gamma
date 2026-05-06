#pragma once

#include "app/VulkanContext.h"
#include "assets/ModelAsset.h"
#include "assets/SceneMetadata.h"
#include "components/EditorComponent.h"
#include "components/NodeGraphComponent.h"
#include "render/SceneViewportRenderer.h"
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
    NodeGraphComponent node_graph_component_;
    SceneViewportRenderer scene_view_renderer_;
    SceneViewportCameraState scene_view_camera_{};
    EditorComponent editor_component_;
    std::filesystem::path cached_scene_path_;
    std::filesystem::file_time_type cached_scene_write_time_{};
    SceneMetadata cached_scene_metadata_{};
    bool has_cached_scene_metadata_ = false;
    std::unordered_map<std::filesystem::path, CachedModelAssetEntry> model_asset_cache_;

    const CachedModelAssetEntry& GetModelAssetEntry(const std::filesystem::path& path);
    const SceneMetadata& GetSceneMetadata(const std::filesystem::path& path);
    const ModelAsset& GetModelAsset(const std::filesystem::path& path);
    void RenderSceneViewport(EngineState& state);
    void RenderGraphViewport(EngineState& state);
};