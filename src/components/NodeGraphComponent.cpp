#include "components/NodeGraphComponent.h"

#include "graph/NodeLibrary.h"
#include "imgui.h"
#include "state/EngineState.h"
#include "ui/Codicons.h"

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

NodeGraphComponent::NodeGraphComponent() = default;

NodeGraphComponent::~NodeGraphComponent()
{
    Shutdown();
}

void NodeGraphComponent::Render(EngineState& state)
{
    HandleGraphSessionRequests(state);

    ImGui::TextUnformatted("Node Graph");
    if (state.HasOpenGraph())
    {
        ImGui::SameLine();
        ImGui::TextUnformatted(state.GetOpenGraphDisplayPath().c_str());
        ImGui::SameLine();

        // Right-align Save and Reload buttons.
        const std::string save_button_label = ICON_CI_SAVE;
        const std::string reload_button_label = ICON_CI_REFRESH;
        float button_width = ImGui::CalcTextSize(save_button_label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        button_width += ImGui::CalcTextSize(reload_button_label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
        button_width += ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - button_width);

        if (ImGui::Button(save_button_label.c_str()))
        {
            SaveOpenGraph(state);
        }
        ImGui::SameLine();
        if (ImGui::Button(reload_button_label.c_str()))
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

void NodeGraphComponent::Shutdown()
{
    graph_.reset();
    current_graph_document_ = GraphDocument{};
    saved_graph_contents_.clear();
}

bool NodeGraphComponent::SaveOpenGraph(EngineState& state)
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

bool NodeGraphComponent::ReloadOpenGraph(EngineState& state)
{
    if (!state.HasOpenGraph())
    {
        return false;
    }

    state.RequestReloadOpenGraph();
    HandleGraphSessionRequests(state);
    return state.HasOpenGraph();
}

ImFlow::ImNodeFlow& NodeGraphComponent::GetGraph()
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

bool NodeGraphComponent::LoadGraphFile(EngineState& state, const std::filesystem::path& path)
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

void NodeGraphComponent::HandleGraphSessionRequests(EngineState& state)
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

void NodeGraphComponent::RebuildGraphFromDocument()
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

void NodeGraphComponent::SyncGraphDocumentFromUi(EngineState& state)
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

void NodeGraphComponent::RenderNodeLibrary()
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

void NodeGraphComponent::RenderNodeLibrarySection(const GraphNodeDefinition& definition)
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

void NodeGraphComponent::HandleGraphNodeDrop(EngineState& state)
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
