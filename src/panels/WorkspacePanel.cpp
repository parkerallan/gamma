#include "panels/WorkspacePanel.h"

#include "imgui.h"
#include "components/EditorComponent.h"

#include <chrono>
#include <unordered_set>

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

bool WorkspacePanel::BeginAsyncViewportLoad(
    const EngineState& state,
    const std::filesystem::path& scene_path,
    std::filesystem::file_time_type scene_write_time)
{
    if (viewport_load_in_progress_)
    {
        return pending_viewport_scene_path_ == scene_path && pending_viewport_scene_write_time_ == scene_write_time;
    }

    pending_viewport_scene_path_ = scene_path;
    pending_viewport_scene_write_time_ = scene_write_time;
    viewport_load_in_progress_ = true;
    pending_viewport_load_ = std::async(std::launch::async, [scene_path, project_root = state.project_root, scene_write_time]()
    {
        AsyncViewportLoadResult result;
        result.scene_path = scene_path;
        result.scene_write_time = scene_write_time;
        result.scene_metadata = LoadSceneMetadata(scene_path);
        if (!result.scene_metadata.parsed)
        {
            return result;
        }

        std::unordered_set<std::filesystem::path> unique_model_paths;
        unique_model_paths.reserve(result.scene_metadata.objects.size());
        for (const SceneObjectMetadata& object : result.scene_metadata.objects)
        {
            if (object.model_path.empty())
            {
                continue;
            }

            unique_model_paths.insert((project_root / object.model_path).lexically_normal());
        }

        result.model_cache.reserve(unique_model_paths.size());
        for (const std::filesystem::path& model_path : unique_model_paths)
        {
            std::error_code error;
            const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(model_path, error);
            CachedModelAssetEntry entry;
            entry.write_time = error ? std::filesystem::file_time_type::min() : write_time;
            entry.asset = LoadModelAsset(model_path);
            result.model_cache.emplace(model_path, std::move(entry));
        }

        return result;
    });

    return true;
}

bool WorkspacePanel::TryConsumeAsyncViewportLoad(
    EngineState& state,
    const std::filesystem::path& scene_path,
    std::filesystem::file_time_type scene_write_time)
{
    if (!viewport_load_in_progress_ || !pending_viewport_load_.valid())
    {
        return false;
    }

    const auto status = pending_viewport_load_.wait_for(std::chrono::milliseconds(0));
    if (status != std::future_status::ready)
    {
        return false;
    }

    AsyncViewportLoadResult result;
    try
    {
        result = pending_viewport_load_.get();
    }
    catch (...)
    {
        viewport_load_in_progress_ = false;
        pending_viewport_scene_path_.clear();
        pending_viewport_scene_write_time_ = std::filesystem::file_time_type{};
        state.AddLog("Viewport preload failed: unexpected async loader exception");
        return false;
    }

    viewport_load_in_progress_ = false;
    pending_viewport_scene_path_.clear();
    pending_viewport_scene_write_time_ = std::filesystem::file_time_type{};

    if (result.scene_path != scene_path || result.scene_write_time != scene_write_time)
    {
        return false;
    }

    if (state.active_scene_path != scene_path)
    {
        return false;
    }

    cached_scene_path_ = result.scene_path;
    cached_scene_write_time_ = result.scene_write_time;
    cached_scene_metadata_ = std::move(result.scene_metadata);
    has_cached_scene_metadata_ = true;
    model_asset_cache_ = std::move(result.model_cache);
    return true;
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

        const char* tab_name = "Scene";
        if (state.active_tab == WorkspaceTab::Graph)
        {
            tab_name = "Graph";
        }
        else if (state.active_tab == WorkspaceTab::Editor)
        {
            tab_name = "Editor";
        }
        state.AddLog(std::string("Switched workspace tab: ") + tab_name);
    }

    ImGui::End();
}

void WorkspacePanel::Shutdown()
{
    if (pending_viewport_load_.valid())
    {
        pending_viewport_load_.wait();
    }

    scene_view_renderer_.Shutdown();
    node_graph_component_.Shutdown();
    cached_scene_path_.clear();
    cached_scene_metadata_ = SceneMetadata{};
    has_cached_scene_metadata_ = false;
    model_asset_cache_.clear();
    pending_viewport_scene_path_.clear();
    pending_viewport_scene_write_time_ = std::filesystem::file_time_type{};
    viewport_load_in_progress_ = false;
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

    std::error_code error;
    const std::filesystem::file_time_type scene_write_time = std::filesystem::last_write_time(state.active_scene_path, error);
    const std::filesystem::file_time_type effective_write_time =
        error ? std::filesystem::file_time_type::min() : scene_write_time;
    const bool has_scene_cache =
        has_cached_scene_metadata_ &&
        cached_scene_path_ == state.active_scene_path;

    if (!has_scene_cache)
    {
        BeginAsyncViewportLoad(state, state.active_scene_path, effective_write_time);
        const bool consumed = TryConsumeAsyncViewportLoad(state, state.active_scene_path, effective_write_time);
        const bool ready_for_active_scene =
            consumed &&
            has_cached_scene_metadata_ &&
            cached_scene_path_ == state.active_scene_path;
        if (!ready_for_active_scene)
        {
            ImGui::TextUnformatted("Scene Viewport");
            ImGui::Separator();
            const ImVec2 canvas_size = ImGui::GetContentRegionAvail();
            ImGui::BeginChild("##SceneViewportLoadingCanvas", canvas_size, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const ImVec2 min = ImGui::GetWindowPos();
            const ImVec2 max = ImVec2(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            draw_list->AddRectFilled(min, max, IM_COL32(24, 28, 32, 255), 8.0f);
            draw_list->AddRect(min, max, IM_COL32(92, 99, 110, 255), 8.0f, 0, 1.5f);

            const char* loading_text = "Loading ...";
            const ImVec2 text_size = ImGui::CalcTextSize(loading_text);
            const ImVec2 center = ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
            const ImVec2 text_pos = ImVec2(center.x - text_size.x * 0.5f, center.y - text_size.y * 0.5f);
            draw_list->AddText(text_pos, IM_COL32(206, 212, 220, 255), loading_text);
            ImGui::EndChild();
            return;
        }
    }
    else if (cached_scene_write_time_ != effective_write_time)
    {
        // Scene edits should stay interactive and not bounce the viewport into a loading state.
        cached_scene_write_time_ = effective_write_time;
        cached_scene_metadata_ = LoadSceneMetadata(state.active_scene_path);
        has_cached_scene_metadata_ = true;

        if (cached_scene_metadata_.parsed)
        {
            std::unordered_set<std::filesystem::path> model_paths_in_scene;
            model_paths_in_scene.reserve(cached_scene_metadata_.objects.size());
            for (const SceneObjectMetadata& object : cached_scene_metadata_.objects)
            {
                if (object.model_path.empty())
                {
                    continue;
                }

                model_paths_in_scene.insert((state.project_root / object.model_path).lexically_normal());
            }

            for (const std::filesystem::path& model_path : model_paths_in_scene)
            {
                GetModelAssetEntry(model_path);
            }
        }
    }

    if (viewport_load_in_progress_)
    {
        TryConsumeAsyncViewportLoad(state, state.active_scene_path, effective_write_time);
    }

    const SceneMetadata& scene_metadata = cached_scene_metadata_;
    if (!scene_metadata.parsed)
    {
        ImGui::TextUnformatted("Scene Viewport");
        ImGui::Separator();
        ImGui::TextWrapped("Failed to load active scene: %s", scene_metadata.error_message.empty() ? "unknown error" : scene_metadata.error_message.c_str());
        return;
    }

    scene_view_renderer_.RenderUi(state, scene_metadata, [this](const std::filesystem::path& path) -> SceneViewportResolvedModel
    {
        const auto cache_it = model_asset_cache_.find(path);
        if (cache_it == model_asset_cache_.end())
        {
            return SceneViewportResolvedModel{};
        }

        return SceneViewportResolvedModel{&cache_it->second.asset, cache_it->second.write_time};
    }, scene_view_camera_);
}

void WorkspacePanel::RenderGraphViewport(EngineState& state)
{
    node_graph_component_.Render(state);
}
