#include "components/NodeGraphComponent.h"

#include "components/graph/GraphDocument.h"
#include "components/graph/GraphTranspiler.h"
#include "components/graph/NodeSpec.h"

#include "imgui.h"
#include "imgui_node_editor.h"
#include "widgets.h"
#include "state/EngineState.h"
#include "ui/Codicons.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ed = ax::NodeEditor;

namespace
{

// Map an engine PinType to a Blueprint-style icon + ABGR color.
// Colors follow the imgui-node-editor blueprints-example conventions:
//   Exec   -> white triangle (Flow)
//   Bool   -> red circle
//   Number -> cyan circle (mint-ish)
//   String -> pink circle
//   Vec3   -> yellow circle
//   Object -> light-blue circle
//   Any    -> gray circle
struct PinAppearance
{
    ax::Drawing::IconType icon;
    ImU32 color;          // outline / filled-icon color (ABGR)
    ImU32 inner_color;    // inner fill when "filled" (ABGR, alpha=0 = transparent)
};

PinAppearance GetPinAppearance(graph::PinType type)
{
    using IT = ax::Drawing::IconType;
    switch (type)
    {
        case graph::PinType::Exec:   return { IT::Flow,   IM_COL32(255, 255, 255, 255), IM_COL32(0,0,0,0) };
        case graph::PinType::Bool:   return { IT::Circle, IM_COL32(220,  48,  48, 255), IM_COL32(0,0,0,0) };
        case graph::PinType::Number: return { IT::Circle, IM_COL32( 68, 201, 156, 255), IM_COL32(0,0,0,0) };
        case graph::PinType::String: return { IT::Circle, IM_COL32(218,   0, 183, 255), IM_COL32(0,0,0,0) };
        case graph::PinType::Vec3:   return { IT::Circle, IM_COL32(255, 226,  85, 255), IM_COL32(0,0,0,0) };
        case graph::PinType::Object: return { IT::Circle, IM_COL32( 51, 150, 215, 255), IM_COL32(0,0,0,0) };
        case graph::PinType::Any:    default:
                                     return { IT::Diamond,IM_COL32(170, 170, 170, 255), IM_COL32(0,0,0,0) };
    }
}

constexpr float kPinIconSize = 18.0f;

// Pin ids must be unique across the editor canvas. Pack
// (node_id, pin_index, input_flag) into a single uint64 with the high bit set
// so they never collide with raw node/link ids.
constexpr std::uint64_t kPinIdMarker     = 0x8000000000000000ull;
constexpr std::uint64_t kPinKindInputBit = 0x4000000000000000ull;
constexpr std::uint64_t kNodeIdMask      = 0x00000000FFFFFFFFull;

std::uint64_t MakePinId(std::uint64_t node_id, std::uint16_t pin_index, bool is_input)
{
    std::uint64_t id = (node_id & kNodeIdMask) | (static_cast<std::uint64_t>(pin_index) << 40);
    if (is_input) id |= kPinKindInputBit;
    id |= kPinIdMarker;
    return id;
}

bool DecodePinId(std::uint64_t pin_id, std::uint64_t& node_id_out, std::uint16_t& pin_index_out, bool& is_input_out)
{
    if ((pin_id & kPinIdMarker) == 0) return false;
    node_id_out = pin_id & kNodeIdMask;
    pin_index_out = static_cast<std::uint16_t>((pin_id >> 40) & 0xFFFFu);
    is_input_out = (pin_id & kPinKindInputBit) != 0;
    return true;
}

ImU32 PinTypeColor(graph::PinType type)
{
    switch (type)
    {
    case graph::PinType::Exec:   return IM_COL32(255, 255, 255, 255);
    case graph::PinType::Bool:   return IM_COL32(220,  48,  48, 255);
    case graph::PinType::Number: return IM_COL32( 68, 201, 156, 255);
    case graph::PinType::String: return IM_COL32(218, 152,  46, 255);
    case graph::PinType::Vec3:   return IM_COL32(255, 200,   0, 255);
    case graph::PinType::Object: return IM_COL32( 81, 142, 200, 255);
    case graph::PinType::Any:    return IM_COL32(180, 180, 180, 255);
    }
    return IM_COL32(200, 200, 200, 255);
}

bool PinTypesCompatible(graph::PinType a, graph::PinType b)
{
    if (a == b) return true;
    if (a == graph::PinType::Any || b == graph::PinType::Any) return true;
    if ((a == graph::PinType::String && b == graph::PinType::Object) ||
        (b == graph::PinType::String && a == graph::PinType::Object))
    {
        return true;
    }
    return false;
}

} // namespace

struct NodeGraphComponent::Impl
{
    ed::EditorContext* editor = nullptr;
    std::filesystem::path loaded_path;
    graph::GraphDocument doc;
    bool loaded = false;

    char palette_filter[64] = "";
    ImVec2 palette_spawn_pos{0.0f, 0.0f};
    bool palette_open_pending = false;

    // Right-click context menu targets.
    std::uint64_t ctx_node = 0;
    std::uint64_t ctx_link = 0;
    std::uint64_t ctx_pin  = 0;

    // Tracks which node ids have had their saved canvas position pushed into
    // the editor at least once.
    std::unordered_map<std::uint64_t, bool> position_initialized;

    // Clipboard for Ctrl+C / Ctrl+X / Ctrl+V. Stores deep copies of nodes
    // (with their input literals) and any links whose endpoints are both
    // inside the copied selection. `clipboard_origin` is the top-left of
    // the source bounding box so paste can re-anchor at the mouse cursor.
    std::vector<graph::GraphNode> clipboard_nodes;
    std::vector<graph::GraphLink> clipboard_links;
    ImVec2 clipboard_origin{0.0f, 0.0f};

    void EnsureEditor()
    {
        if (editor == nullptr)
        {
            ed::Config cfg;
            cfg.SettingsFile = nullptr;
            editor = ed::CreateEditor(&cfg);
        }
    }

    void DestroyEditor()
    {
        if (editor != nullptr)
        {
            ed::DestroyEditor(editor);
            editor = nullptr;
        }
    }

    void LoadGraph(EngineState& state, const std::filesystem::path& path)
    {
        std::string error;
        if (!graph::GraphDocument::LoadFromFile(path, doc, error))
        {
            state.AddLog("Failed to load graph: " + error);
            doc = graph::GraphDocument{};
        }
        loaded_path = path;
        loaded = true;
        position_initialized.clear();

        DestroyEditor();
        ed::Config cfg;
        cfg.SettingsFile = nullptr;
        editor = ed::CreateEditor(&cfg);
    }

    bool SaveGraph(EngineState& state)
    {
        if (loaded_path.empty()) return false;
        std::string error;
        if (!doc.SaveToFile(loaded_path, error))
        {
            state.AddLog("Failed to save graph: " + error);
            return false;
        }
        state.open_graph_dirty = false;
        return true;
    }
};

NodeGraphComponent::NodeGraphComponent()
    : impl_(std::make_unique<Impl>())
{
    graph::NodeSpecRegistry::Get();
}

NodeGraphComponent::~NodeGraphComponent()
{
    Shutdown();
}

void NodeGraphComponent::Shutdown()
{
    if (impl_) impl_->DestroyEditor();
}

bool NodeGraphComponent::SaveOpenGraph(EngineState& state)
{
    if (!impl_ || !impl_->loaded || impl_->loaded_path.empty()) return false;
    const bool ok = impl_->SaveGraph(state);
    if (ok) state.AddLog("Saved graph: " + state.GetOpenGraphDisplayPath());
    return ok;
}

bool NodeGraphComponent::ReloadOpenGraph(EngineState& state)
{
    if (!impl_ || !state.HasOpenGraph()) return false;
    impl_->LoadGraph(state, state.open_graph_path);
    state.graph_reload_requested = false;
    state.open_graph_dirty = false;
    state.AddLog("Reloaded graph: " + state.GetOpenGraphDisplayPath());
    return true;
}

void NodeGraphComponent::Render(EngineState& state)
{
    // Activate requested graph (file double-click etc).
    if (!state.requested_graph_path.empty())
    {
        state.open_graph_path = state.requested_graph_path;
        state.requested_graph_path.clear();
        state.graph_reload_requested = true;
        state.RequestTab(WorkspaceTab::Graph);
    }

    // ---- Toolbar ----------------------------------------------------------
    static int selected_graph_index = 0;
    static char new_graph_name[128] = "NewGraph";
    static std::vector<std::string> graph_names;
    static std::vector<std::filesystem::path> graph_paths;
    static std::filesystem::path last_scanned_dir;
    static std::uint64_t last_dir_signature = 0;

    const std::filesystem::path current_graphs_dir = state.project_root.empty()
        ? std::filesystem::path()
        : state.project_root / "Graphs";

    auto compute_dir_signature = [](const std::filesystem::path& dir) -> std::uint64_t
    {
        if (dir.empty() || !std::filesystem::is_directory(dir)) return 0;
        std::uint64_t signature = 1;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".graph")
            {
                signature ^= std::hash<std::string>{}(entry.path().filename().string());
                signature *= 1099511628211ull;
            }
        }
        return signature;
    };

    const std::uint64_t current_dir_signature = compute_dir_signature(current_graphs_dir);
    if (current_graphs_dir != last_scanned_dir || current_dir_signature != last_dir_signature)
    {
        last_scanned_dir = current_graphs_dir;
        last_dir_signature = current_dir_signature;
        graph_names.clear();
        graph_paths.clear();
        graph_names.push_back("New +");
        graph_paths.push_back({});
        if (!current_graphs_dir.empty() && std::filesystem::is_directory(current_graphs_dir))
        {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(current_graphs_dir, ec))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".graph")
                {
                    graph_names.push_back(entry.path().stem().string());
                    graph_paths.push_back(entry.path());
                }
            }
        }
    }
    if (graph_names.empty() || graph_paths.empty())
    {
        graph_names = {"New +"};
        graph_paths = {std::filesystem::path{}};
    }

    if (state.HasOpenGraph())
    {
        for (int i = 0; i < static_cast<int>(graph_paths.size()); ++i)
        {
            if (graph_paths[i] == state.open_graph_path) { selected_graph_index = i; break; }
        }
    }
    if (selected_graph_index < 0) selected_graph_index = 0;
    if (selected_graph_index >= static_cast<int>(graph_paths.size()))
        selected_graph_index = static_cast<int>(graph_paths.size()) - 1;

    std::vector<const char*> graph_name_ptrs;
    graph_name_ptrs.reserve(graph_names.size());
    for (const auto& name : graph_names) graph_name_ptrs.push_back(name.c_str());

    const int prev_index = selected_graph_index;
    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("##NodeGraphSelector", &selected_graph_index, graph_name_ptrs.data(), static_cast<int>(graph_name_ptrs.size()));
    if (selected_graph_index != prev_index)
    {
        if (selected_graph_index == 0)
        {
            std::snprintf(new_graph_name, sizeof(new_graph_name), "NewGraph");
            state.open_graph_path.clear();
            state.open_graph_dirty = false;
            impl_->loaded = false;
            impl_->loaded_path.clear();
            impl_->doc = graph::GraphDocument{};
        }
        else
        {
            new_graph_name[0] = '\0';
            state.open_graph_path = graph_paths[selected_graph_index];
            state.graph_reload_requested = true;
        }
    }

    const bool is_new = selected_graph_index == 0;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if (!is_new) ImGui::BeginDisabled();
    if (ImGui::InputTextWithHint("##NodeGraphName", "", new_graph_name, sizeof(new_graph_name)))
        selected_graph_index = 0;
    if (!is_new) ImGui::EndDisabled();

    {
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float save_w  = ImGui::CalcTextSize(ICON_CI_SAVE).x          + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float build_w = ImGui::CalcTextSize(ICON_CI_RUN_WITH_DEPS).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float play_w  = ImGui::CalcTextSize(ICON_CI_DEBUG_START).x   + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float toolbar_total = save_w + build_w + play_w + sp * 2.0f;
        const float cur_x = ImGui::GetCursorPosX();
        const float avail = ImGui::GetContentRegionAvail().x;
        if (state.open_graph_dirty)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Unsaved changes");
        }
        ImGui::SameLine();
        if (avail > toolbar_total) ImGui::SetCursorPosX(cur_x + avail - toolbar_total);
        if (ImGui::Button(ICON_CI_SAVE))
        {
            if (is_new)
            {
                if (!state.project_root.empty() && new_graph_name[0] != '\0')
                {
                    const std::filesystem::path graphs_dir = state.project_root / "Graphs";
                    std::error_code ec;
                    std::filesystem::create_directories(graphs_dir, ec);
                    const std::filesystem::path new_path = graphs_dir / (std::string(new_graph_name) + ".graph");
                    graph::GraphDocument empty_doc;
                    std::string error;
                    if (empty_doc.SaveToFile(new_path, error))
                    {
                        const std::string created_graph_name = new_graph_name;
                        graph_names.push_back(created_graph_name);
                        graph_paths.push_back(new_path);
                        selected_graph_index = static_cast<int>(graph_paths.size()) - 1;
                        new_graph_name[0] = '\0';
                        state.open_graph_path = new_path;
                        state.graph_reload_requested = true;
                        state.AddLog("Created graph: " + created_graph_name);
                    }
                    else
                    {
                        state.AddLog("Failed to create graph: " + error);
                    }
                }
            }
            else
            {
                SaveOpenGraph(state);
            }
        }
        ImGui::SameLine();
        if (!state.CanBuildProject()) ImGui::BeginDisabled();
        if (ImGui::Button(ICON_CI_RUN_WITH_DEPS)) state.TriggerBuildAction();
        if (!state.CanBuildProject()) ImGui::EndDisabled();
        ImGui::SameLine();
        if (!state.CanPlayScene()) ImGui::BeginDisabled();
        if (ImGui::Button(ICON_CI_DEBUG_START)) state.TriggerPlayAction();
        if (!state.CanPlayScene()) ImGui::EndDisabled();
    }

    ImGui::Separator();

    if (state.graph_reload_requested && state.HasOpenGraph())
    {
        impl_->LoadGraph(state, state.open_graph_path);
        state.graph_reload_requested = false;
        state.open_graph_dirty = false;
    }

    if (!impl_->loaded)
    {
        ImGui::TextDisabled("Select or create a graph to begin editing.");
        return;
    }

    impl_->EnsureEditor();

    // ---- Canvas -----------------------------------------------------------
    auto& reg = graph::NodeSpecRegistry::Get();
    auto& doc = impl_->doc;

    ed::SetCurrentEditor(impl_->editor);
    ed::Begin("NodeGraphEditor", ImVec2(0.0f, 0.0f));

    bool any_change = false;

    for (auto& node : doc.nodes)
    {
        const graph::NodeSpec* spec = reg.Find(node.type_key);
        if (spec == nullptr) continue;

        const ed::NodeId node_id = static_cast<ed::NodeId>(node.id);

        // Push saved position into the editor on first frame this node is drawn.
        if (!impl_->position_initialized[node.id])
        {
            ed::SetNodePosition(node_id, ImVec2(node.position_x, node.position_y));
            impl_->position_initialized[node.id] = true;
        }

        // Node-editor style: give nodes a bit of padding and rounding so the
        // header band reads as a proper title bar. PinRect/PinPivot make link
        // attachment lines come out of the node edge instead of the icon
        // center - matching the Blueprint look.
        ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(8.0f, 4.0f, 8.0f, 8.0f));
        ed::PushStyleVar(ed::StyleVar_NodeRounding, 6.0f);
        ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 1.5f);

        ed::BeginNode(node_id);
        ImGui::PushID(static_cast<int>(node.id));

        // Force all in-node text to bright white so it reads against the
        // colored header band and the dark node body.
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));

        // ---- Header (title) - colored band painted after EndNode ----------
        // Record header rect in screen coords so we can paint a colored band
        // that spans the full node width with rounded top corners.
        const ImVec2 header_min_screen = ImGui::GetCursorScreenPos();
        ImGui::TextUnformatted(spec->display_name.c_str());
        ImGui::Dummy(ImVec2(0.0f, 2.0f));   // breathing room under title text
        const float header_max_y       = ImGui::GetItemRectMax().y;
        ImGui::Dummy(ImVec2(0.0f, 4.0f));   // gap between header band and body

        // ---- Body: two-column table (inputs left, outputs right) ----------
        const std::size_t row_count = std::max(spec->inputs.size(), spec->outputs.size());
        const bool has_outputs = !spec->outputs.empty();
        const bool has_inputs  = !spec->inputs.empty();

        if (row_count > 0)
        {
            const int col_count = (has_inputs && has_outputs) ? 2 : 1;
            const std::string table_id = "##nodebody_" + std::to_string(node.id);
            if (ImGui::BeginTable(table_id.c_str(), col_count,
                                  ImGuiTableFlags_SizingFixedFit |
                                  ImGuiTableFlags_NoHostExtendX |
                                  ImGuiTableFlags_NoSavedSettings))
            {
                if (has_inputs)
                    ImGui::TableSetupColumn("in",  ImGuiTableColumnFlags_WidthFixed);
                if (has_outputs)
                    ImGui::TableSetupColumn("out", ImGuiTableColumnFlags_WidthFixed);

                for (std::size_t row = 0; row < row_count; ++row)
                {
                    ImGui::TableNextRow();

                    // ---- Input pin (left column) -------------------------
                    if (has_inputs)
                    {
                        ImGui::TableSetColumnIndex(0);
                        if (row < spec->inputs.size())
                        {
                            const std::uint16_t i = static_cast<std::uint16_t>(row);
                            const graph::PinSpec& pin = spec->inputs[i];
                            const std::uint64_t pin_id = MakePinId(node.id, i, true);
                            const bool linked = doc.HasLinkIntoInput(node.id, pin.name);
                            const PinAppearance appearance = GetPinAppearance(pin.type);
                            const ImVec4 icon_color  = ImGui::ColorConvertU32ToFloat4(appearance.color);
                            const ImVec4 inner_color = ImGui::ColorConvertU32ToFloat4(appearance.inner_color);

                            // Pin attaches at the LEFT edge of the icon.
                            ed::PushStyleVar(ed::StyleVar_PivotAlignment, ImVec2(0.0f, 0.5f));
                            ed::PushStyleVar(ed::StyleVar_PivotSize, ImVec2(0.0f, 0.0f));
                            ed::BeginPin(static_cast<ed::PinId>(pin_id), ed::PinKind::Input);
                            const float in_row_y = ImGui::GetCursorPosY();
                            ax::Widgets::Icon(ImVec2(kPinIconSize, kPinIconSize),
                                              appearance.icon, linked, icon_color, inner_color);
                            ImGui::SameLine(0.0f, 6.0f);
                            // Vertically center the label against the icon.
                            ImGui::SetCursorPosY(in_row_y + (kPinIconSize - ImGui::GetFontSize()) * 0.5f);
                            ImGui::TextUnformatted(pin.name.c_str());
                            ed::EndPin();
                            ed::PopStyleVar(2);

                            if (pin.type != graph::PinType::Exec && !linked)
                            {
                                std::string& literal = node.input_literals[pin.name];
                                if (literal.empty()) literal = pin.default_literal;
                                ImGui::SameLine(0.0f, 6.0f);
                                ImGui::SetCursorPosY(in_row_y);  // back to row baseline
                                const std::string id_str = "##lit_" + std::to_string(node.id) + "_" + pin.name;
                                // Bigger, higher-contrast input box so literals are easy to read/edit.
                                ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32( 18,  18,  24, 255));
                                ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32( 30,  30,  40, 255));
                                ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  IM_COL32( 42,  42,  56, 255));
                                ImGui::PushStyleColor(ImGuiCol_Border,         IM_COL32(120, 120, 140, 200));
                                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
                                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                                ImGui::PushItemWidth(140.0f);
                                if (pin.type == graph::PinType::Bool)
                                {
                                    bool b = (literal == "true" || literal == "1");
                                    if (ImGui::Checkbox(id_str.c_str(), &b))
                                    {
                                        literal = b ? "true" : "false";
                                        any_change = true;
                                    }
                                }
                                else
                                {
                                    char buf[256];
                                    std::snprintf(buf, sizeof(buf), "%s", literal.c_str());
                                    if (ImGui::InputText(id_str.c_str(), buf, sizeof(buf)))
                                    {
                                        literal = buf;
                                        any_change = true;
                                    }
                                }
                                ImGui::PopItemWidth();
                                ImGui::PopStyleVar(2);
                                ImGui::PopStyleColor(4);
                            }
                        }
                    }

                    // ---- Output pin (right column) -----------------------
                    if (has_outputs)
                    {
                        ImGui::TableSetColumnIndex(has_inputs ? 1 : 0);
                        if (row < spec->outputs.size())
                        {
                            const std::uint16_t i = static_cast<std::uint16_t>(row);
                            const graph::PinSpec& pin = spec->outputs[i];
                            const std::uint64_t pin_id = MakePinId(node.id, i, false);
                            const PinAppearance appearance = GetPinAppearance(pin.type);
                            const ImVec4 icon_color  = ImGui::ColorConvertU32ToFloat4(appearance.color);
                            const ImVec4 inner_color = ImGui::ColorConvertU32ToFloat4(appearance.inner_color);
                            bool linked = false;
                            for (const auto& link : doc.links)
                            {
                                if (link.from_node == node.id && link.from_pin == pin.name)
                                { linked = true; break; }
                            }

                            // Pin attaches at the RIGHT edge of the icon.
                            ed::PushStyleVar(ed::StyleVar_PivotAlignment, ImVec2(1.0f, 0.5f));
                            ed::PushStyleVar(ed::StyleVar_PivotSize, ImVec2(0.0f, 0.0f));
                            ed::BeginPin(static_cast<ed::PinId>(pin_id), ed::PinKind::Output);
                            const float out_row_y = ImGui::GetCursorPosY();
                            // Center label vertically against the icon.
                            ImGui::SetCursorPosY(out_row_y + (kPinIconSize - ImGui::GetFontSize()) * 0.5f);
                            ImGui::TextUnformatted(pin.name.c_str());
                            ImGui::SameLine(0.0f, 6.0f);
                            ImGui::SetCursorPosY(out_row_y);  // back to row baseline for the icon
                            ax::Widgets::Icon(ImVec2(kPinIconSize, kPinIconSize),
                                              appearance.icon, linked, icon_color, inner_color);
                            ed::EndPin();
                            ed::PopStyleVar(2);
                        }
                    }
                }
                ImGui::EndTable();
            }
        }

        ImGui::PopStyleColor();  // ImGuiCol_Text
        ImGui::PopID();
        ed::EndNode();
        ed::PopStyleVar(3);

        // ---- Paint the header band into the node background --------------
        // The header band sits behind the node content but in front of the
        // node background fill. We extend it sideways to the full node width
        // (including padding) and upward to the node's top edge so it reads
        // as a true title bar with rounded top corners.
        if (ImGui::IsItemVisible())
        {
            const ImVec2 node_pos  = ed::GetNodePosition(node_id);
            const ImVec2 node_size = ed::GetNodeSize(node_id);
            if (node_size.x > 0.0f && node_size.y > 0.0f)
            {
                ImDrawList* bg = ed::GetNodeBackgroundDrawList(node_id);
                if (bg != nullptr)
                {
                    const ImVec2 band_min(node_pos.x, node_pos.y);
                    const ImVec2 band_max(node_pos.x + node_size.x,
                                          header_max_y + 2.0f);
                    bg->AddRectFilled(band_min, band_max,
                                      spec->header_color,
                                      ed::GetStyle().NodeRounding,
                                      ImDrawFlags_RoundCornersTop);
                    // Soft separator line under the header band.
                    bg->AddLine(ImVec2(band_min.x + 2.0f, band_max.y),
                                ImVec2(band_max.x - 2.0f, band_max.y),
                                IM_COL32(255, 255, 255, 60), 1.0f);
                }
            }
            (void)header_min_screen;
        }

        // Pick up drag-induced position changes.
        const ImVec2 ed_pos = ed::GetNodePosition(node_id);
        if (std::abs(ed_pos.x - node.position_x) > 0.01f || std::abs(ed_pos.y - node.position_y) > 0.01f)
        {
            node.position_x = ed_pos.x;
            node.position_y = ed_pos.y;
            any_change = true;
        }
    }

    // Draw existing links.
    for (const auto& link : doc.links)
    {
        const graph::GraphNode* fn = doc.FindNode(link.from_node);
        const graph::GraphNode* tn = doc.FindNode(link.to_node);
        if (!fn || !tn) continue;
        const graph::NodeSpec* fs = reg.Find(fn->type_key);
        const graph::NodeSpec* ts = reg.Find(tn->type_key);
        if (!fs || !ts) continue;
        std::uint16_t from_idx = 0;
        std::uint16_t to_idx = 0;
        for (std::uint16_t i = 0; i < static_cast<std::uint16_t>(fs->outputs.size()); ++i)
            if (fs->outputs[i].name == link.from_pin) { from_idx = i; break; }
        for (std::uint16_t i = 0; i < static_cast<std::uint16_t>(ts->inputs.size()); ++i)
            if (ts->inputs[i].name == link.to_pin) { to_idx = i; break; }
        ed::Link(static_cast<ed::LinkId>(link.id),
            static_cast<ed::PinId>(MakePinId(link.from_node, from_idx, false)),
            static_cast<ed::PinId>(MakePinId(link.to_node,   to_idx,   true)));
    }

    // ---- Interactive create -----------------------------------------------
    if (ed::BeginCreate())
    {
        ed::PinId start_pin_id, end_pin_id;
        if (ed::QueryNewLink(&start_pin_id, &end_pin_id))
        {
            if (start_pin_id && end_pin_id)
            {
                std::uint64_t s_node = 0, e_node = 0;
                std::uint16_t s_idx = 0, e_idx = 0;
                bool s_input = false, e_input = false;
                const bool s_ok = DecodePinId(static_cast<std::uint64_t>(start_pin_id.Get()), s_node, s_idx, s_input);
                const bool e_ok = DecodePinId(static_cast<std::uint64_t>(end_pin_id.Get()),   e_node, e_idx, e_input);
                if (!s_ok || !e_ok || s_input == e_input || s_node == e_node)
                {
                    ed::RejectNewItem();
                }
                else
                {
                    const std::uint64_t out_node = s_input ? e_node : s_node;
                    const std::uint16_t out_idx  = s_input ? e_idx  : s_idx;
                    const std::uint64_t in_node  = s_input ? s_node : e_node;
                    const std::uint16_t in_idx   = s_input ? s_idx  : e_idx;

                    const graph::GraphNode* fn = doc.FindNode(out_node);
                    const graph::GraphNode* tn = doc.FindNode(in_node);
                    bool reject = true;
                    if (fn && tn)
                    {
                        const graph::NodeSpec* fs = reg.Find(fn->type_key);
                        const graph::NodeSpec* ts = reg.Find(tn->type_key);
                        if (fs && ts && out_idx < fs->outputs.size() && in_idx < ts->inputs.size())
                        {
                            const graph::PinSpec& op = fs->outputs[out_idx];
                            const graph::PinSpec& ip = ts->inputs[in_idx];
                            if (PinTypesCompatible(op.type, ip.type))
                            {
                                // Do NOT call RejectNewItem when AcceptNewItem returns false:
                                // AcceptNewItem only returns true on the release frame (when
                                // the library has transitioned to the Create stage). Calling
                                // RejectNewItem would overwrite m_UserAction=UserAccept with
                                // UserReject and prevent the Possible->Create transition.
                                reject = false;
                                if (ed::AcceptNewItem())
                                {
                                    doc.links.erase(std::remove_if(doc.links.begin(), doc.links.end(),
                                        [&](const graph::GraphLink& l) {
                                            return l.to_node == in_node && l.to_pin == ip.name;
                                        }), doc.links.end());
                                    graph::GraphLink new_link;
                                    new_link.id = doc.AllocateId();
                                    new_link.from_node = out_node;
                                    new_link.from_pin = op.name;
                                    new_link.to_node = in_node;
                                    new_link.to_pin = ip.name;
                                    doc.links.push_back(std::move(new_link));
                                    any_change = true;
                                }
                            }
                        }
                    }
                    if (reject) ed::RejectNewItem();
                }
            }
        }
    }
    ed::EndCreate();

    // ---- Backspace deletes the current selection --------------------------
    // The node-editor only listens for the Delete key. Many users (and our
    // user explicitly) expect Backspace to remove the selected nodes/links.
    // We feed the selection through ed::DeleteNode/DeleteLink so the
    // BeginDelete/EndDelete block below picks them up in the same frame.
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
    {
        const int sel_count = ed::GetSelectedObjectCount();
        if (sel_count > 0)
        {
            std::vector<ed::NodeId> sel_nodes(static_cast<std::size_t>(sel_count));
            std::vector<ed::LinkId> sel_links(static_cast<std::size_t>(sel_count));
            const int nn = ed::GetSelectedNodes(sel_nodes.data(), sel_count);
            const int nl = ed::GetSelectedLinks(sel_links.data(), sel_count);
            for (int i = 0; i < nn; ++i) ed::DeleteNode(sel_nodes[i]);
            for (int i = 0; i < nl; ++i) ed::DeleteLink(sel_links[i]);
        }
    }

    // ---- Copy / Cut / Paste (Ctrl+C/X/V and right-click menus) ----------
    // Shared lambdas so the keyboard handler and the context-menu items
    // below dispatch the same logic. Copy snapshots selected nodes (plus
    // links whose endpoints are both inside the selection) into the impl
    // clipboard. Cut snapshots then deletes. Paste re-allocates ids,
    // anchors the bounding box at `anchor` (canvas space), and selects
    // the freshly pasted nodes.
    auto do_copy = [&]()
    {
        impl_->clipboard_nodes.clear();
        impl_->clipboard_links.clear();

        const int sel_count = ed::GetSelectedObjectCount();
        if (sel_count <= 0) return;
        std::vector<ed::NodeId> sel_nodes(static_cast<std::size_t>(sel_count));
        const int nn = ed::GetSelectedNodes(sel_nodes.data(), sel_count);
        if (nn <= 0) return;

        std::unordered_set<std::uint64_t> id_set;
        id_set.reserve(static_cast<std::size_t>(nn));
        float min_x = 0.0f, min_y = 0.0f;
        bool have_origin = false;
        for (int i = 0; i < nn; ++i)
        {
            const std::uint64_t nid = static_cast<std::uint64_t>(sel_nodes[i].Get());
            const graph::GraphNode* src = doc.FindNode(nid);
            if (!src) continue;
            // Pull the live editor position so a dragged-but-unsaved node
            // still copies at its on-screen location.
            const ImVec2 ed_pos = ed::GetNodePosition(sel_nodes[i]);
            graph::GraphNode copy = *src;
            if (!std::isnan(ed_pos.x) && !std::isnan(ed_pos.y))
            {
                copy.position_x = ed_pos.x;
                copy.position_y = ed_pos.y;
            }
            if (!have_origin || copy.position_x < min_x) min_x = copy.position_x;
            if (!have_origin || copy.position_y < min_y) min_y = copy.position_y;
            have_origin = true;
            id_set.insert(nid);
            impl_->clipboard_nodes.push_back(std::move(copy));
        }
        impl_->clipboard_origin = ImVec2(min_x, min_y);

        for (const auto& link : doc.links)
        {
            if (id_set.count(link.from_node) && id_set.count(link.to_node))
                impl_->clipboard_links.push_back(link);
        }
    };

    auto do_cut = [&]()
    {
        do_copy();
        for (const auto& n : impl_->clipboard_nodes)
            ed::DeleteNode(static_cast<ed::NodeId>(n.id));
    };

    auto do_paste = [&](ImVec2 anchor)
    {
        if (impl_->clipboard_nodes.empty()) return;
        const ImVec2 offset(anchor.x - impl_->clipboard_origin.x,
                            anchor.y - impl_->clipboard_origin.y);

        std::unordered_map<std::uint64_t, std::uint64_t> id_remap;
        id_remap.reserve(impl_->clipboard_nodes.size());

        ed::ClearSelection();
        for (const auto& src : impl_->clipboard_nodes)
        {
            graph::GraphNode n = src;
            n.id = doc.AllocateId();
            n.position_x = src.position_x + offset.x;
            n.position_y = src.position_y + offset.y;
            id_remap[src.id] = n.id;
            impl_->position_initialized[n.id] = false;
            const ed::NodeId new_ed_id = static_cast<ed::NodeId>(n.id);
            doc.nodes.push_back(std::move(n));
            ed::SelectNode(new_ed_id, true);
        }
        for (const auto& src : impl_->clipboard_links)
        {
            auto a = id_remap.find(src.from_node);
            auto b = id_remap.find(src.to_node);
            if (a == id_remap.end() || b == id_remap.end()) continue;
            graph::GraphLink l = src;
            l.id = doc.AllocateId();
            l.from_node = a->second;
            l.to_node = b->second;
            doc.links.push_back(std::move(l));
        }
        any_change = true;
    };

    if (!ImGui::GetIO().WantTextInput)
    {
        const bool ctrl = ImGui::GetIO().KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
            do_copy();
        else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false))
            do_cut();
        else if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
        {
            // Anchor at the mouse if it's over the editor; else stack with a
            // small fixed offset so repeated Ctrl+V is predictable.
            ImVec2 anchor(impl_->clipboard_origin.x + 20.0f,
                          impl_->clipboard_origin.y + 20.0f);
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_RootWindow))
                anchor = ed::ScreenToCanvas(ImGui::GetMousePos());
            do_paste(anchor);
        }
    }

    if (ed::BeginDelete())
    {
        ed::LinkId deleted_link;
        while (ed::QueryDeletedLink(&deleted_link))
        {
            if (ed::AcceptDeletedItem())
            {
                doc.RemoveLink(static_cast<std::uint64_t>(deleted_link.Get()));
                any_change = true;
            }
        }
        ed::NodeId deleted_node;
        while (ed::QueryDeletedNode(&deleted_node))
        {
            if (ed::AcceptDeletedItem())
            {
                doc.RemoveNode(static_cast<std::uint64_t>(deleted_node.Get()));
                any_change = true;
            }
        }
    }
    ed::EndDelete();

    // ---- Right-click delete (node / link context menus) -------------------
    // The Delete key is wired through BeginDelete/EndDelete above, but most
    // users reach for right-click. These popups give them an explicit menu.
    ed::Suspend();
    {
        ed::NodeId ctx_node_id = 0;
        ed::LinkId ctx_link_id = 0;
        ed::PinId  ctx_pin_id  = 0;
        if (ed::ShowNodeContextMenu(&ctx_node_id))
        {
            impl_->ctx_node = static_cast<std::uint64_t>(ctx_node_id.Get());
            ImGui::OpenPopup("graph_node_ctx");
        }
        else if (ed::ShowLinkContextMenu(&ctx_link_id))
        {
            impl_->ctx_link = static_cast<std::uint64_t>(ctx_link_id.Get());
            ImGui::OpenPopup("graph_link_ctx");
        }
        else if (ed::ShowPinContextMenu(&ctx_pin_id))
        {
            impl_->ctx_pin = static_cast<std::uint64_t>(ctx_pin_id.Get());
            ImGui::OpenPopup("graph_pin_ctx");
        }
    }
    if (ImGui::BeginPopup("graph_node_ctx"))
    {
        if (ImGui::MenuItem("Copy", "Ctrl+C"))
        {
            // The user may right-click a node that isn't part of the
            // current selection — make sure that node is selected so
            // do_copy() picks it up.
            const ed::NodeId nid = static_cast<ed::NodeId>(impl_->ctx_node);
            if (!ed::IsNodeSelected(nid))
            {
                ed::ClearSelection();
                ed::SelectNode(nid, true);
            }
            do_copy();
        }
        if (ImGui::MenuItem("Cut", "Ctrl+X"))
        {
            const ed::NodeId nid = static_cast<ed::NodeId>(impl_->ctx_node);
            if (!ed::IsNodeSelected(nid))
            {
                ed::ClearSelection();
                ed::SelectNode(nid, true);
            }
            do_cut();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete Node", "Backspace"))
        {
            ed::DeleteNode(static_cast<ed::NodeId>(impl_->ctx_node));
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("graph_link_ctx"))
    {
        if (ImGui::MenuItem("Delete Connection"))
        {
            ed::DeleteLink(static_cast<ed::LinkId>(impl_->ctx_link));
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("graph_pin_ctx"))
    {
        if (ImGui::MenuItem("Break All Connections"))
        {
            std::uint64_t node_of_pin = 0;
            std::uint16_t pin_idx = 0;
            bool is_in = false;
            if (DecodePinId(impl_->ctx_pin, node_of_pin, pin_idx, is_in))
            {
                const graph::GraphNode* gn = doc.FindNode(node_of_pin);
                const graph::NodeSpec*  ps = gn ? reg.Find(gn->type_key) : nullptr;
                if (ps != nullptr)
                {
                    const auto& pins = is_in ? ps->inputs : ps->outputs;
                    if (pin_idx < pins.size())
                    {
                        const std::string& pin_name = pins[pin_idx].name;
                        doc.links.erase(std::remove_if(doc.links.begin(), doc.links.end(),
                            [&](const graph::GraphLink& l) {
                                return is_in ? (l.to_node == node_of_pin && l.to_pin == pin_name)
                                             : (l.from_node == node_of_pin && l.from_pin == pin_name);
                            }), doc.links.end());
                        any_change = true;
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
    ed::Resume();

    // ---- Palette (background context menu) --------------------------------
    ed::Suspend();
    if (ed::ShowBackgroundContextMenu())
    {
        impl_->palette_open_pending = true;
        impl_->palette_spawn_pos = ed::ScreenToCanvas(ImGui::GetMousePos());
        impl_->palette_filter[0] = '\0';
        ImGui::OpenPopup("graph_palette");
    }
    if (ImGui::BeginPopup("graph_palette"))
    {
        // Quick Paste at the cursor when there's clipboard content
        if (!impl_->clipboard_nodes.empty())
        {
            if (ImGui::MenuItem("Paste", "Ctrl+V"))
            {
                do_paste(impl_->palette_spawn_pos);
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
        }
        ImGui::SetNextItemWidth(240.0f);
        if (impl_->palette_open_pending)
        {
            ImGui::SetKeyboardFocusHere();
            impl_->palette_open_pending = false;
        }
        ImGui::InputTextWithHint("##palette_filter", "search...", impl_->palette_filter, sizeof(impl_->palette_filter));

        // Build a tree of categories split on '/' so e.g. "Attribute/PointLightAttr"
        // becomes a nested menu: Attribute > PointLightAttr > Set/Get.
        struct MenuNode {
            std::vector<std::string> child_order;
            std::unordered_map<std::string, MenuNode> children;
            std::vector<const graph::NodeSpec*> specs;
        };
        MenuNode root;
        for (const auto& s : reg.All())
        {
            if (impl_->palette_filter[0] != '\0')
            {
                const std::string filter = impl_->palette_filter;
                auto contains_ci = [](const std::string& hay, const std::string& needle) {
                    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                        [](char a, char b) {
                            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
                        });
                    return it != hay.end();
                };
                if (!contains_ci(s.display_name, filter) && !contains_ci(s.type_key, filter)) continue;
            }
            MenuNode* cur = &root;
            const std::string& cat = s.category;
            std::string::size_type start = 0;
            while (start <= cat.size())
            {
                const std::string::size_type slash = cat.find('/', start);
                const std::string seg = (slash == std::string::npos)
                    ? cat.substr(start)
                    : cat.substr(start, slash - start);
                if (cur->children.find(seg) == cur->children.end())
                    cur->child_order.push_back(seg);
                cur = &cur->children[seg];
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
            cur->specs.push_back(&s);
        }

        auto spawn_node = [&](const graph::NodeSpec* s)
        {
            graph::GraphNode new_node;
            new_node.id = doc.AllocateId();
            new_node.type_key = s->type_key;
            new_node.position_x = impl_->palette_spawn_pos.x;
            new_node.position_y = impl_->palette_spawn_pos.y;
            for (const auto& pin : s->inputs)
            {
                if (pin.type != graph::PinType::Exec && !pin.default_literal.empty())
                    new_node.input_literals[pin.name] = pin.default_literal;
            }
            impl_->position_initialized[new_node.id] = false;
            doc.nodes.push_back(std::move(new_node));
            any_change = true;
        };

        auto render_menu = [&](auto& self, const MenuNode& n) -> void
        {
            for (const auto& key : n.child_order)
            {
                const MenuNode& child = n.children.at(key);
                if (ImGui::BeginMenu(key.c_str()))
                {
                    self(self, child);
                    ImGui::EndMenu();
                }
            }
            for (const graph::NodeSpec* s : n.specs)
            {
                if (ImGui::MenuItem(s->display_name.c_str()))
                    spawn_node(s);
            }
        };
        render_menu(render_menu, root);
        ImGui::EndPopup();
    }
    ed::Resume();

    ed::End();
    ed::SetCurrentEditor(nullptr);

    if (any_change) state.open_graph_dirty = true;
}
