#include "panels/WorkspacePanel.h"

#include "imgui.h"
#include "components/EditorComponent.h"

WorkspacePanel::WorkspacePanel() = default;

WorkspacePanel::~WorkspacePanel()
{
    Shutdown();
}

bool WorkspacePanel::InitializeSceneRenderer(VulkanContext* context)
{
    return scene_view_renderer_.Initialize(context);
}

void WorkspacePanel::BeginFrame()
{
    scene_view_renderer_.BeginFrame();
}

void WorkspacePanel::RenderSceneGpuPass()
{
    scene_view_renderer_.RenderGpu();
}

const CachedModelAssetEntry& WorkspacePanel::GetModelAssetEntry(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    CachedModelAssetEntry& cache_entry = model_asset_cache_[path];
    if (error || cache_entry.write_time != write_time || !cache_entry.asset.loaded)
    {
        cache_entry.write_time = error ? std::filesystem::file_time_type::min() : write_time;
        cache_entry.asset = LoadModelAsset(path);
    }

    return cache_entry;
}

const SceneMetadata& WorkspacePanel::GetSceneMetadata(const std::filesystem::path& path)
{
    if (path.empty())
    {
        static SceneMetadata empty_metadata{};
        return empty_metadata;
    }

    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool cache_valid = has_cached_scene_metadata_ && cached_scene_path_ == path && !error && cached_scene_write_time_ == write_time;
    if (!cache_valid)
    {
        cached_scene_path_ = path;
        cached_scene_write_time_ = write_time;
        cached_scene_metadata_ = LoadSceneMetadata(path);
        has_cached_scene_metadata_ = true;
    }

    return cached_scene_metadata_;
}

const ModelAsset& WorkspacePanel::GetModelAsset(const std::filesystem::path& path)
{
    return GetModelAssetEntry(path).asset;
}

void WorkspacePanel::Render(EngineState& state)
{
    if (!state.show_workspace_panel)
    {
        return;
    }

    if (!ImGui::Begin("Workspace", &state.show_workspace_panel))
    {
        ImGui::End();
        return;
    }

    WorkspaceTab new_active_tab = state.active_tab;

    if (ImGui::BeginTabBar("WorkspaceTabs", ImGuiTabBarFlags_None))
    {
        if (ImGui::BeginTabItem("Scene", nullptr, state.GetTabSelectionFlags(WorkspaceTab::Scene)))
        {
            state.CompleteTabRequest(WorkspaceTab::Scene);
            new_active_tab = WorkspaceTab::Scene;
            RenderSceneViewport(state);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Graph", nullptr, state.GetTabSelectionFlags(WorkspaceTab::Graph)))
        {
            state.CompleteTabRequest(WorkspaceTab::Graph);
            new_active_tab = WorkspaceTab::Graph;
            RenderGraphViewport(state);
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Editor", nullptr, state.GetTabSelectionFlags(WorkspaceTab::Editor)))
        {
            state.CompleteTabRequest(WorkspaceTab::Editor);
            new_active_tab = WorkspaceTab::Editor;
            editor_component_.Render(state);
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    if (new_active_tab != state.active_tab)
    {
        state.active_tab = new_active_tab;

        const char* tab_name = state.active_tab == WorkspaceTab::Scene
            ? "Scene"
            : state.active_tab == WorkspaceTab::Graph ? "Graph" : "Editor";
        state.AddLog(std::string("Switched workspace tab: ") + tab_name);
    }

    ImGui::End();
}

void WorkspacePanel::Shutdown()
{
    scene_view_renderer_.Shutdown();
    node_graph_component_.Shutdown();
    cached_scene_path_.clear();
    cached_scene_metadata_ = SceneMetadata{};
    has_cached_scene_metadata_ = false;
    model_asset_cache_.clear();
}

bool WorkspacePanel::SaveOpenGraph(EngineState& state)
{
    return node_graph_component_.SaveOpenGraph(state);
}

bool WorkspacePanel::ReloadOpenGraph(EngineState& state)
{
    return node_graph_component_.ReloadOpenGraph(state);
}

void WorkspacePanel::RenderSceneViewport(EngineState& state)
{
    if (!state.HasActiveScene())
    {
        ImGui::TextUnformatted("Scene Viewport");
        ImGui::Separator();
        ImGui::TextWrapped("Open a project with an active scene to preview attached models here.");
        return;
    }

    const SceneMetadata& scene_metadata = GetSceneMetadata(state.active_scene_path);
    if (!scene_metadata.parsed)
    {
        ImGui::TextUnformatted("Scene Viewport");
        ImGui::Separator();
        ImGui::TextWrapped("Failed to load active scene: %s", scene_metadata.error_message.empty() ? "unknown error" : scene_metadata.error_message.c_str());
        return;
    }

    scene_view_renderer_.RenderUi(state, scene_metadata, [this](const std::filesystem::path& path) -> SceneViewportResolvedModel
    {
        const CachedModelAssetEntry& entry = GetModelAssetEntry(path);
        return SceneViewportResolvedModel{&entry.asset, entry.write_time};
    }, scene_view_camera_);
}

void WorkspacePanel::RenderGraphViewport(EngineState& state)
{
    node_graph_component_.Render(state);
}