#include "panels/WorkspacePanel.h"

#include "imgui.h"

#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace
{
constexpr const char* kGraphNodeDragPayload = "GRAPH_NODE_LIBRARY_ITEM";

std::uint32_t FindPinIndex(const std::vector<std::shared_ptr<ImFlow::Pin>>& pins, const ImFlow::Pin* pin)
{
    for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(pins.size()); ++index)
    {
        if (pins[index].get() == pin)
        {
            return index;
        }
    }

    return static_cast<std::uint32_t>(pins.size());
}
}

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
            RenderEditorViewport(state);
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

ImFlow::ImNodeFlow& WorkspacePanel::GetGraph()
{
    if (!graph_)
    {
        graph_ = std::make_unique<ImFlow::ImNodeFlow>("GraphEditor");
        graph_->droppedLinkPopUpContent([](ImFlow::Pin* dragged)
        {
            if (dragged != nullptr)
            {
                dragged->deleteLink();
            }

            ImGui::CloseCurrentPopup();
        });
    }

    return *graph_;
}

void WorkspacePanel::Shutdown()
{
    scene_view_renderer_.Shutdown();
    graph_.reset();
    current_graph_document_ = GraphDocument{};
    saved_graph_contents_.clear();
    cached_scene_path_.clear();
    cached_scene_metadata_ = SceneMetadata{};
    has_cached_scene_metadata_ = false;
    model_asset_cache_.clear();
}

bool WorkspacePanel::LoadGraphFile(EngineState& state, const std::filesystem::path& path)
{
    GraphDocument document = LoadGraphDocument(path);
    if (!document.parsed)
    {
        state.AddLog("Failed to open graph: " + state.GetDisplayPath(path));
        if (!document.error_message.empty())
        {
            state.AddLog("Graph load reason: " + document.error_message);
        }
        state.requested_graph_path.clear();
        state.graph_reload_requested = false;
        return false;
    }

    current_graph_document_ = std::move(document);
    saved_graph_contents_ = SerializeGraphDocument(current_graph_document_);
    state.open_graph_path = path;
    state.requested_graph_path.clear();
    state.graph_reload_requested = false;
    state.open_graph_dirty = false;
    RebuildGraphFromDocument();
    state.RequestTab(WorkspaceTab::Graph);
    state.AddLog("Opened graph: " + state.GetDisplayPath(path));
    return true;
}

void WorkspacePanel::HandleGraphSessionRequests(EngineState& state)
{
    if (!state.requested_graph_path.empty())
    {
        if (state.open_graph_path != state.requested_graph_path || graph_ == nullptr)
        {
            LoadGraphFile(state, state.requested_graph_path);
            return;
        }

        state.requested_graph_path.clear();
        state.RequestTab(WorkspaceTab::Graph);
    }

    if (state.graph_reload_requested && state.HasOpenGraph())
    {
        LoadGraphFile(state, state.open_graph_path);
    }
}

void WorkspacePanel::RebuildGraphFromDocument()
{
    graph_.reset();
    ImFlow::ImNodeFlow& graph = GetGraph();
    std::unordered_map<std::uint64_t, std::shared_ptr<GraphNodeBase>> nodes_by_id;

    for (const GraphNodeRecord& node_record : current_graph_document_.nodes)
    {
        std::shared_ptr<GraphNodeBase> node = SpawnGraphNodeAtGridPosition(
            graph,
            node_record.type_key,
            ImVec2(node_record.position_x, node_record.position_y));
        if (!node)
        {
            continue;
        }

        node->setUID(static_cast<ImFlow::NodeUID>(node_record.id));
        node->DeserializeProperties(node_record.properties);

        nodes_by_id[node_record.id] = node;
    }

    for (const GraphLinkRecord& link_record : current_graph_document_.links)
    {
        const auto source_it = nodes_by_id.find(link_record.source_node_id);
        const auto target_it = nodes_by_id.find(link_record.target_node_id);
        if (source_it == nodes_by_id.end() || target_it == nodes_by_id.end())
        {
            continue;
        }

        const std::vector<std::shared_ptr<ImFlow::Pin>>& source_pins = source_it->second->getOuts();
        const std::vector<std::shared_ptr<ImFlow::Pin>>& target_pins = target_it->second->getIns();
        ImFlow::Pin* source_pin = link_record.source_port_index < source_pins.size() ? source_pins[link_record.source_port_index].get() : nullptr;
        ImFlow::Pin* target_pin = link_record.target_port_index < target_pins.size() ? target_pins[link_record.target_port_index].get() : nullptr;
        if (source_pin != nullptr && target_pin != nullptr)
        {
            source_pin->createLink(target_pin);
        }
    }
}

void WorkspacePanel::SyncGraphDocumentFromUi(EngineState& state)
{
    if (!state.HasOpenGraph() || graph_ == nullptr)
    {
        return;
    }

    GraphDocument document;
    document.parsed = true;
    document.graph_name = current_graph_document_.graph_name.empty() ? state.open_graph_path.stem().string() : current_graph_document_.graph_name;

    for (const auto& node_entry : graph_->getNodes())
    {
        if (!node_entry.second)
        {
            continue;
        }

        GraphNodeBase* graph_node = dynamic_cast<GraphNodeBase*>(node_entry.second.get());
        if (graph_node == nullptr)
        {
            continue;
        }

        GraphNodeRecord node_record;
        node_record.id = static_cast<std::uint64_t>(node_entry.second->getUID());
        node_record.type_key = GetGraphNodeTypeKey(graph_node->GetNodeType());
        node_record.position_x = node_entry.second->getPos().x;
        node_record.position_y = node_entry.second->getPos().y;
        graph_node->SerializeProperties(node_record.properties);

        document.nodes.push_back(std::move(node_record));
    }

    for (const std::weak_ptr<ImFlow::Link>& weak_link : graph_->getLinks())
    {
        if (weak_link.expired())
        {
            continue;
        }

        const std::shared_ptr<ImFlow::Link> link = weak_link.lock();
        if (!link || link->left() == nullptr || link->right() == nullptr)
        {
            continue;
        }

        GraphLinkRecord link_record;
        link_record.source_node_id = static_cast<std::uint64_t>(link->left()->getParent()->getUID());
        link_record.source_port_index = FindPinIndex(link->left()->getParent()->getOuts(), link->left());
        link_record.target_node_id = static_cast<std::uint64_t>(link->right()->getParent()->getUID());
        link_record.target_port_index = FindPinIndex(link->right()->getParent()->getIns(), link->right());
        document.links.push_back(std::move(link_record));
    }

    current_graph_document_ = std::move(document);
    state.open_graph_dirty = SerializeGraphDocument(current_graph_document_) != saved_graph_contents_;
}

bool WorkspacePanel::SaveOpenGraph(EngineState& state)
{
    if (!state.HasOpenGraph())
    {
        return false;
    }

    SyncGraphDocumentFromUi(state);
    if (!SaveGraphDocument(state.open_graph_path, current_graph_document_))
    {
        state.AddLog("Failed to save graph: " + state.GetOpenGraphDisplayPath());
        return false;
    }

    saved_graph_contents_ = SerializeGraphDocument(current_graph_document_);
    state.open_graph_dirty = false;
    state.AddLog("Saved graph: " + state.GetOpenGraphDisplayPath());
    return true;
}

bool WorkspacePanel::ReloadOpenGraph(EngineState& state)
{
    if (!state.HasOpenGraph())
    {
        return false;
    }

    state.RequestReloadOpenGraph();
    HandleGraphSessionRequests(state);
    return state.HasOpenGraph();
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
    HandleGraphSessionRequests(state);

    ImGui::TextUnformatted("Node Graph");
    if (state.HasOpenGraph())
    {
        ImGui::SameLine();
        ImGui::TextUnformatted(state.GetOpenGraphDisplayPath().c_str());
        ImGui::SameLine();

        if (ImGui::Button("Save"))
        {
            SaveOpenGraph(state);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload"))
        {
            ReloadOpenGraph(state);
        }

        if (state.open_graph_dirty)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted("(modified)");
        }
    }
    ImGui::Separator();

    if (!state.HasOpenGraph())
    {
        ImGui::TextWrapped("Select a .graph file from the Files panel to open it here.");
        return;
    }

    const float library_width = 220.0f;
    const float splitter_spacing = ImGui::GetStyle().ItemSpacing.x;

    ImGui::BeginChild("NodeLibraryPanel", ImVec2(library_width, 0.0f), true);
    RenderNodeLibrary();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, splitter_spacing);

    ImGui::BeginChild("NodeGraphCanvas", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImFlow::ImNodeFlow& graph = GetGraph();
    graph.setSize(ImGui::GetContentRegionAvail());
    graph.update();
    ImGui::EndChild();

    HandleGraphNodeDrop(state);
    SyncGraphDocumentFromUi(state);
}

void WorkspacePanel::RenderNodeLibrary()
{
    ImGui::TextUnformatted("Node Library");
    ImGui::Separator();
    ImGui::Spacing();

    const std::vector<GraphNodeDefinition>& definitions = GetGraphNodeDefinitions();
    const char* current_section = nullptr;

    for (const GraphNodeDefinition& definition : definitions)
    {
        if (current_section == nullptr || std::strcmp(current_section, definition.section_name) != 0)
        {
            current_section = definition.section_name;
            if (!ImGui::CollapsingHeader(current_section, ImGuiTreeNodeFlags_DefaultOpen))
            {
                continue;
            }
        }

        RenderNodeLibrarySection(definition);
    }
}

void WorkspacePanel::RenderNodeLibrarySection(const GraphNodeDefinition& definition)
{
    ImGui::PushID(static_cast<int>(definition.type));
    if (ImGui::Selectable(definition.label, false, ImGuiSelectableFlags_None, ImVec2(0.0f, 30.0f)))
    {
    }

    if (ImGui::BeginDragDropSource())
    {
        const std::int32_t payload_value = static_cast<std::int32_t>(definition.type);
        ImGui::SetDragDropPayload(kGraphNodeDragPayload, &payload_value, sizeof(payload_value));
        ImGui::Text("Create %s node", definition.label);
        ImGui::EndDragDropSource();
    }

    ImGui::PopID();
}

void WorkspacePanel::HandleGraphNodeDrop(EngineState& state)
{
    if (!ImGui::BeginDragDropTarget())
    {
        return;
    }

    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kGraphNodeDragPayload))
    {
        const std::int32_t payload_value = *static_cast<const std::int32_t*>(payload->Data);
        const GraphNodeType node_type = static_cast<GraphNodeType>(payload_value);
        SpawnGraphNode(GetGraph(), node_type, ImGui::GetMousePos());
        state.AddLog(std::string("Created graph node: ") + GetGraphNodeLabel(node_type));
        SyncGraphDocumentFromUi(state);
    }

    ImGui::EndDragDropTarget();
}

void WorkspacePanel::RenderEditorViewport(EngineState& state)
{
    if (!state.HasOpenFile())
    {
        ImGui::TextUnformatted("Text Editor");
        ImGui::Separator();
        ImGui::TextWrapped("Select a supported text file from the Files panel to open it here.");
        return;
    }

    ImGui::TextUnformatted(state.GetOpenFileDisplayPath().c_str());
    ImGui::SameLine();

    if (ImGui::Button("Save"))
    {
        state.SaveOpenFile();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
    {
        state.OpenTextFile(state.open_file_path);
    }

    if (state.open_file_dirty)
    {
        ImGui::SameLine();
        ImGui::TextUnformatted("(modified)");
    }

    ImGui::Separator();

    ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput;
    if (!state.wrap_editor_text)
    {
        flags |= ImGuiInputTextFlags_NoHorizontalScroll;
    }
    if (ImGui::InputTextMultiline(
            "##TextEditor",
            state.editor_buffer.data(),
            state.editor_buffer.size(),
            ImGui::GetContentRegionAvail(),
            flags))
    {
        state.SyncEditorTextFromBuffer();
        state.open_file_dirty = state.open_file_contents != state.saved_file_contents;
    }
}