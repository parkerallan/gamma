#include "panels/WorkspacePanel.h"

#include "imgui.h"

#include <cstdint>

namespace
{
constexpr const char* kGraphNodeDragPayload = "GRAPH_NODE_LIBRARY_ITEM";
}

WorkspacePanel::WorkspacePanel() = default;

WorkspacePanel::~WorkspacePanel()
{
    Shutdown();
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
            RenderSceneViewport();
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
    graph_.reset();
}

void WorkspacePanel::RenderSceneViewport()
{
    ImGui::TextUnformatted("Scene Viewport");
    ImGui::Separator();

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + available.x, min.y + available.y);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    draw_list->AddRectFilled(min, max, IM_COL32(24, 28, 32, 255), 8.0f);
    draw_list->AddRect(min, max, IM_COL32(92, 99, 110, 255), 8.0f, 0, 1.5f);

    const char* primary = "Scene Viewport";
    const char* secondary = "Future framebuffer texture area";
    const ImVec2 primary_size = ImGui::CalcTextSize(primary);
    const ImVec2 secondary_size = ImGui::CalcTextSize(secondary);
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);

    draw_list->AddText(
        ImVec2(center.x - primary_size.x * 0.5f, center.y - primary_size.y),
        IM_COL32(235, 238, 242, 255),
        primary);
    draw_list->AddText(
        ImVec2(center.x - secondary_size.x * 0.5f, center.y + 8.0f),
        IM_COL32(145, 152, 163, 255),
        secondary);

    ImGui::Dummy(available);
}

void WorkspacePanel::RenderGraphViewport(EngineState& state)
{
    ImGui::TextUnformatted("Node Graph");
    ImGui::Separator();

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
    if (state.open_file_dirty)
    {
        ImGui::TextUnformatted("(modified)");
    }

    if (ImGui::Button("Save"))
    {
        state.SaveOpenFile();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
    {
        state.OpenTextFile(state.open_file_path);
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