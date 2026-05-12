#pragma once

#include "app/VulkanContext.h"
#include "assets/ModelAsset.h"
#include "assets/SceneMetadata.h"
#include "components/EditorComponent.h"
#include "components/NodeGraphComponent.h"
#include "render/SceneViewportRenderer.h"
#include "state/EngineState.h"

#include <filesystem>
#include <future>
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

    // Hand-off accessors for Play / build flows. The editor already parses
    // scene metadata and all referenced models on a worker thread the first
    // time the Scene tab is shown; exposing those caches lets the runtime
    // skip a redundant Assimp + scene-text parse on play.
    const SceneMetadata* TryGetCachedSceneMetadata(const std::filesystem::path& scene_path) const;
    const std::unordered_map<std::filesystem::path, CachedModelAssetEntry>& GetCachedModelAssets() const
    {
        return model_asset_cache_;
    }
    // Bytes for audio clips / video files referenced by the cached scene
    // that the async preloader has already pulled from disk. Used by the
    // editor->runtime hand-off so PlaySound and the first video Update()
    // don't have to read from disk on the main thread.
    const std::unordered_map<std::string, std::vector<std::uint8_t>>& GetCachedAudioClipBytes() const
    {
        return audio_clip_bytes_cache_;
    }
    const std::unordered_map<std::string, std::vector<std::uint8_t>>& GetCachedVideoBytes() const
    {
        return video_bytes_cache_;
    }
    // Block until any pending async scene preload finishes (bounded by the
    // worker thread's parse time) AND consume the result into the cache so
    // GetCachedModelAssets / TryGetCachedSceneMetadata see it. No-op when no
    // load is pending.
    void WaitForPendingViewportLoad(EngineState& state);

private:
    struct AsyncViewportLoadResult
    {
        std::filesystem::path scene_path;
        std::filesystem::file_time_type scene_write_time{};
        SceneMetadata scene_metadata{};
        std::unordered_map<std::filesystem::path, CachedModelAssetEntry> model_cache;
        // Keyed by the path as stored in the scene attribute (relative,
        // matches the runtime lookup keys). Empty entries mean the file
        // couldn't be read; consumers should fall back to disk.
        std::unordered_map<std::string, std::vector<std::uint8_t>> audio_clip_bytes;
        std::unordered_map<std::string, std::vector<std::uint8_t>> video_bytes;
    };

    NodeGraphComponent node_graph_component_;
    SceneViewportRenderer scene_view_renderer_;
    SceneViewportCameraState scene_view_camera_{};
    EditorComponent editor_component_;
    std::filesystem::path cached_scene_path_;
    std::filesystem::file_time_type cached_scene_write_time_{};
    SceneMetadata cached_scene_metadata_{};
    bool has_cached_scene_metadata_ = false;
    std::unordered_map<std::filesystem::path, CachedModelAssetEntry> model_asset_cache_;
    std::unordered_map<std::string, std::vector<std::uint8_t>> audio_clip_bytes_cache_;
    std::unordered_map<std::string, std::vector<std::uint8_t>> video_bytes_cache_;
    std::future<AsyncViewportLoadResult> pending_viewport_load_;
    std::filesystem::path pending_viewport_scene_path_;
    std::filesystem::file_time_type pending_viewport_scene_write_time_{};
    bool viewport_load_in_progress_ = false;
    std::uint64_t pending_viewport_load_dispatch_ticks_ = 0;

    const CachedModelAssetEntry& GetModelAssetEntry(const std::filesystem::path& path);
    const SceneMetadata& GetSceneMetadata(const std::filesystem::path& path);
    bool BeginAsyncViewportLoad(const EngineState& state, const std::filesystem::path& scene_path, std::filesystem::file_time_type scene_write_time);
    bool TryConsumeAsyncViewportLoad(EngineState& state, const std::filesystem::path& scene_path, std::filesystem::file_time_type scene_write_time);
    const ModelAsset& GetModelAsset(const std::filesystem::path& path);
    void RenderSceneViewport(EngineState& state);
    void RenderGraphViewport(EngineState& state);
};