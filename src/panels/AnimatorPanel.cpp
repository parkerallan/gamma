#include "panels/AnimatorPanel.h"

#include "graph/GraphDocument.h"
#include "graph/NodeLibrary.h"
#include "imgui.h"
#include "state/EngineState.h"
#include "ui/Codicons.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <vector>

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

void AnimatorPanel::Render(EngineState& state)
{
    if (!state.show_animator_panel)
    {
        return;
    }

    if (!ImGui::Begin("Animator", &state.show_animator_panel))
    {
        ImGui::End();
        return;
    }

    HandleGraphSessionRequests(state);

    // --- Graph selector bar ---
    static int selected_graph_index = 0;
    static char new_graph_name[128] = "NewAnimator";
    static std::vector<std::string> graph_names;
    static std::vector<std::filesystem::path> graph_paths;
    static std::filesystem::path last_scanned_dir;
    static std::uint64_t last_dir_signature = 0;

    // Rebuild graph list only when the directory changes
    const std::filesystem::path current_animators_dir = state.project_root.empty() 
        ? std::filesystem::path() 
        : state.project_root / "Assets" / "Animators";
    
    auto compute_dir_signature = [](const std::filesystem::path& dir) -> std::uint64_t {
        if (dir.empty() || !std::filesystem::is_directory(dir))
        {
            return 0;
        }
        
        std::uint64_t signature = 1;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".anim")
            {
                signature ^= std::hash<std::string>{}(entry.path().filename().string());
                signature *= 1099511628211ull;
            }
        }
        return signature;
    };
    
    const std::uint64_t current_dir_signature = compute_dir_signature(current_animators_dir);
    if (current_animators_dir != last_scanned_dir || current_dir_signature != last_dir_signature)
    {
        last_scanned_dir = current_animators_dir;
        last_dir_signature = current_dir_signature;
        
        graph_names.clear();
        graph_paths.clear();
        graph_names.push_back("New +");
        graph_paths.push_back({});
        
        if (!current_animators_dir.empty() && std::filesystem::is_directory(current_animators_dir))
        {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(current_animators_dir, ec))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".anim")
                {
                    graph_names.push_back(entry.path().stem().string());
                    graph_paths.push_back(entry.path());
                }
            }
        }
    }

    // Sync combo selection only when the open graph changes externally (e.g. file tree click)
    static std::filesystem::path last_synced_graph_path;
    if (state.open_graph_path != last_synced_graph_path)
    {
        last_synced_graph_path = state.open_graph_path;
        if (state.HasOpenGraph())
        {
            for (int i = 1; i < static_cast<int>(graph_paths.size()); ++i)
            {
                if (graph_paths[i] == state.open_graph_path)
                {
                    selected_graph_index = i;
                    break;
                }
            }
        }
        else
        {
            selected_graph_index = 0;
        }
    }

    std::vector<const char*> graph_name_ptrs;
    for (const auto& name : graph_names)
    {
        graph_name_ptrs.push_back(name.c_str());
    }

    const int prev_index = selected_graph_index;
    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("##AnimatorGraphSelector", &selected_graph_index, graph_name_ptrs.data(), static_cast<int>(graph_name_ptrs.size()));
    if (selected_graph_index != prev_index)
    {
        if (selected_graph_index == 0)
        {
            std::snprintf(new_graph_name, sizeof(new_graph_name), "NewAnimator");
        }
        else
        {
            new_graph_name[0] = '\0';
            state.RequestOpenGraphFile(graph_paths[selected_graph_index]);
        }
    }

    const bool is_new = selected_graph_index == 0;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if (!is_new)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputTextWithHint("##AnimatorGraphName", "", new_graph_name, sizeof(new_graph_name)))
    {
        selected_graph_index = 0;
    }
    if (!is_new)
    {
        ImGui::EndDisabled();
    }
    const bool editing_graph_name = ImGui::IsItemActive();

    ImGui::SameLine();
    const std::string save_label = std::string(ICON_CI_SAVE) + " Save";
    if (ImGui::Button(save_label.c_str()))
    {
        if (is_new)
        {
            if (!state.project_root.empty() && new_graph_name[0] != '\0')
            {
                const std::filesystem::path graphs_dir = state.project_root / "Assets" / "Animators";
                std::error_code ec;
                std::filesystem::create_directories(graphs_dir, ec);
                const std::filesystem::path new_path = graphs_dir / (std::string(new_graph_name) + ".anim");
                GraphDocument doc;
                doc.parsed = true;
                doc.graph_name = new_graph_name;
                if (SaveGraphDocument(new_path, doc))
                {
                    const std::string created_graph_name = new_graph_name;
                    graph_names.push_back(created_graph_name);
                    graph_paths.push_back(new_path);
                    selected_graph_index = static_cast<int>(graph_paths.size()) - 1;
                    new_graph_name[0] = '\0';
                    state.RequestOpenGraphFile(new_path);
                    state.AddLog(std::string("Created graph: ") + created_graph_name);
                }
            }
        }
        else
        {
            SaveOpenGraph(state);
        }
    }

    if (state.HasOpenGraph() && state.open_graph_dirty)
    {
        ImGui::SameLine();
        ImGui::TextUnformatted("(modified)");
    }

    ImGui::Separator();

    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float total_width = ImGui::GetContentRegionAvail().x;
    const float available_height = ImGui::GetContentRegionAvail().y;
    const float library_height = 200.0f;
    const float top_height = available_height - library_height - spacing;
    const float half_width = (total_width - spacing) * 0.5f;

    // Left half: viewport
    ImGui::BeginChild("AnimatorViewport", ImVec2(half_width, top_height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 vp_min = ImGui::GetWindowPos();
    const ImVec2 vp_max = ImVec2(vp_min.x + ImGui::GetWindowSize().x, vp_min.y + ImGui::GetWindowSize().y);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilledMultiColor(
        vp_min, vp_max,
        IM_COL32(18, 20, 26, 255),
        IM_COL32(26, 31, 40, 255),
        IM_COL32(12, 14, 18, 255),
        IM_COL32(18, 22, 28, 255));
    draw_list->AddRect(vp_min, vp_max, IM_COL32(84, 92, 105, 255), 0.0f, 0, 1.5f);
    const char* placeholder = "Animation preview";
    const ImVec2 text_size = ImGui::CalcTextSize(placeholder);
    draw_list->AddText(
        ImVec2((vp_min.x + vp_max.x - text_size.x) * 0.5f, (vp_min.y + vp_max.y - text_size.y) * 0.5f),
        IM_COL32(220, 226, 236, 255),
        placeholder);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, spacing);

    // Right half: node graph
    ImGui::BeginChild("NodeGraphCanvas", ImVec2(0.0f, top_height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImFlow::ImNodeFlow& graph = GetGraph();
    graph.setSize(ImGui::GetContentRegionAvail());
    if (!editing_graph_name)
    {
        graph.update();
    }
    ImGui::EndChild();

    HandleGraphNodeDrop(state);
    SyncGraphDocumentFromUi(state);

    // Bottom area: left half table, right half node library
    ImGui::BeginChild("BottomTablePanel", ImVec2(half_width, library_height), true);
    {
        static char table_input[128] = "";
        static int table_dropdown_index = 0;
        
        ImGui::TextUnformatted("Bone Modifiers");
        ImGui::Separator();
        
        // Top row with controls
        if (ImGui::Button(ICON_CI_ADD "##TableAddRow"))
        {
            // Add row action
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::BeginDisabled();
        ImGui::InputText("##TableInput", table_input, sizeof(table_input));
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        const char* dropdown_items[] = {"Option 1", "Option 2", "Option 3"};
        ImGui::Combo("##TableDropdown", &table_dropdown_index, dropdown_items, IM_ARRAYSIZE(dropdown_items));
        
        ImGui::Spacing();
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0f, spacing);

    ImGui::BeginChild("NodeLibraryPanel", ImVec2(0.0f, library_height), true);
    RenderNodeLibrary();
    ImGui::EndChild();

    ImGui::End();
}

void AnimatorPanel::Shutdown()
{
    graph_.reset();
    current_graph_document_ = GraphDocument{};
    saved_graph_contents_.clear();
}

bool AnimatorPanel::SaveOpenGraph(EngineState& state)
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

bool AnimatorPanel::ReloadOpenGraph(EngineState& state)
{
    if (!state.HasOpenGraph())
    {
        return false;
    }

    state.RequestReloadOpenGraph();
    HandleGraphSessionRequests(state);
    return state.HasOpenGraph();
}

ImFlow::ImNodeFlow& AnimatorPanel::GetGraph()
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

bool AnimatorPanel::LoadGraphFile(EngineState& state, const std::filesystem::path& path)
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

void AnimatorPanel::HandleGraphSessionRequests(EngineState& state)
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

void AnimatorPanel::RebuildGraphFromDocument()
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

void AnimatorPanel::SyncGraphDocumentFromUi(EngineState& state)
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

void AnimatorPanel::RenderNodeLibrary()
{
    ImGui::TextUnformatted("Animations");
    ImGui::Separator();
    ImGui::Spacing();

    const std::vector<GraphNodeDefinition>& definitions = GetGraphNodeDefinitions();
    const char* current_section = nullptr;
    const float available_width = ImGui::GetContentRegionAvail().x;
    const float pad_x = 12.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float cursor_x = 0.0f;

    for (const GraphNodeDefinition& definition : definitions)
    {
        if (current_section == nullptr || std::strcmp(current_section, definition.section_name) != 0)
        {
            current_section = definition.section_name;
            cursor_x = 0.0f;
            if (!ImGui::CollapsingHeader(current_section, ImGuiTreeNodeFlags_DefaultOpen))
            {
                continue;
            }
            ImGui::Spacing();
            cursor_x = 0.0f;
        }

        const float pill_width = ImGui::CalcTextSize(definition.label).x + pad_x * 2.0f;
        if (cursor_x + pill_width > available_width && cursor_x > 0.0f)
        {
            cursor_x = 0.0f;
        }
        else if (cursor_x > 0.0f)
        {
            ImGui::SameLine(0.0f, spacing);
        }

        RenderNodeLibrarySection(definition);
        cursor_x += pill_width + spacing;
    }
}

void AnimatorPanel::RenderNodeLibrarySection(const GraphNodeDefinition& definition)
{
    ImGui::PushID(static_cast<int>(definition.type));

    const ImVec2 label_size = ImGui::CalcTextSize(definition.label);
    const float pad_x = 12.0f;
    const float pad_y = 4.0f;
    const ImVec2 pill_size = ImVec2(label_size.x + pad_x * 2.0f, label_size.y + pad_y * 2.0f);
    const float rounding = pill_size.y * 0.5f;

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const ImVec2 pill_min = cursor;
    const ImVec2 pill_max = ImVec2(cursor.x + pill_size.x, cursor.y + pill_size.y);

    const bool hovered = ImGui::IsMouseHoveringRect(pill_min, pill_max);
    const ImU32 bg_color = hovered ? IM_COL32(72, 82, 100, 255) : IM_COL32(45, 52, 65, 255);
    const ImU32 border_color = hovered ? IM_COL32(140, 155, 180, 255) : IM_COL32(90, 100, 120, 255);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(pill_min, pill_max, bg_color, rounding);
    draw_list->AddRect(pill_min, pill_max, border_color, rounding, 0, 1.0f);
    draw_list->AddText(ImVec2(cursor.x + pad_x, cursor.y + pad_y), IM_COL32(220, 226, 236, 255), definition.label);

    ImGui::InvisibleButton("##pill", pill_size);
    ImGui::Spacing();

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
    {
        const std::int32_t payload_value = static_cast<std::int32_t>(definition.type);
        ImGui::SetDragDropPayload(kGraphNodeDragPayload, &payload_value, sizeof(payload_value));
        ImGui::Text("Create %s node", definition.label);
        ImGui::EndDragDropSource();
    }

    ImGui::PopID();
}

void AnimatorPanel::HandleGraphNodeDrop(EngineState& state)
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


