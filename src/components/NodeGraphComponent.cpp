#include "components/NodeGraphComponent.h"

#include "imgui.h"
#include "state/EngineState.h"
#include "ui/Codicons.h"

#include <filesystem>
#include <fstream>
#include <vector>

NodeGraphComponent::NodeGraphComponent() = default;

NodeGraphComponent::~NodeGraphComponent()
{
    Shutdown();
}

void NodeGraphComponent::Render(EngineState& state)
{
    if (!state.requested_graph_path.empty())
    {
        state.open_graph_path = state.requested_graph_path;
        state.requested_graph_path.clear();
        state.graph_reload_requested = false;
        state.open_graph_dirty = false;
        state.RequestTab(WorkspaceTab::Graph);
    }

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
        if (dir.empty() || !std::filesystem::is_directory(dir))
        {
            return 0;
        }

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

    if (selected_graph_index < 0)
    {
        selected_graph_index = 0;
    }
    if (selected_graph_index >= static_cast<int>(graph_paths.size()))
    {
        selected_graph_index = static_cast<int>(graph_paths.size()) - 1;
    }

    std::vector<const char*> graph_name_ptrs;
    graph_name_ptrs.reserve(graph_names.size());
    for (const auto& name : graph_names)
    {
        graph_name_ptrs.push_back(name.c_str());
    }

    const int prev_index = selected_graph_index;
    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("##NodeGraphSelector", &selected_graph_index, graph_name_ptrs.data(), static_cast<int>(graph_name_ptrs.size()));
    if (selected_graph_index != prev_index)
    {
        if (selected_graph_index == 0)
        {
            std::snprintf(new_graph_name, sizeof(new_graph_name), "NewGraph");
        }
        else
        {
            new_graph_name[0] = '\0';
            if (selected_graph_index >= 0 && selected_graph_index < static_cast<int>(graph_paths.size()))
            {
                state.open_graph_path = graph_paths[selected_graph_index];
                state.open_graph_dirty = false;
            }
        }
    }

    const bool is_new = selected_graph_index == 0;

    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if (!is_new)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputTextWithHint("##NodeGraphName", "", new_graph_name, sizeof(new_graph_name)))
    {
        selected_graph_index = 0;
    }
    if (!is_new)
    {
        ImGui::EndDisabled();
    }

    {
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float save_w = ImGui::CalcTextSize(ICON_CI_SAVE).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float build_w = ImGui::CalcTextSize(ICON_CI_RUN_WITH_DEPS).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float play_w = ImGui::CalcTextSize(ICON_CI_DEBUG_START).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float toolbar_total = save_w + build_w + play_w + sp * 2.0f;
        const float cur_x = ImGui::GetCursorPosX();
        const float avail = ImGui::GetContentRegionAvail().x;
        if (state.open_graph_dirty)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Unsaved changes");
        }
        ImGui::SameLine();
        if (avail > toolbar_total)
        {
            ImGui::SetCursorPosX(cur_x + avail - toolbar_total);
        }
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

                    std::ofstream output(new_path, std::ios::binary | std::ios::trunc);
                    if (output)
                    {
                        output << "{}\n";
                        const std::string created_graph_name = new_graph_name;
                        graph_names.push_back(created_graph_name);
                        graph_paths.push_back(new_path);
                        selected_graph_index = static_cast<int>(graph_paths.size()) - 1;
                        new_graph_name[0] = '\0';
                        state.open_graph_path = new_path;
                        state.open_graph_dirty = false;
                        state.AddLog(std::string("Created graph: ") + created_graph_name);
                    }
                    else
                    {
                        state.AddLog("Failed to create graph: " + state.GetDisplayPath(new_path));
                    }
                }
            }
            else if (selected_graph_index >= 0 && selected_graph_index < static_cast<int>(graph_paths.size()))
            {
                state.open_graph_dirty = false;
                state.AddLog("Saved graph: " + graph_paths[selected_graph_index].filename().string());
            }
        }
        ImGui::SameLine();
        if (!state.CanBuildProject()) { ImGui::BeginDisabled(); }
        if (ImGui::Button(ICON_CI_RUN_WITH_DEPS)) { state.TriggerBuildAction(); }
        if (!state.CanBuildProject()) { ImGui::EndDisabled(); }
        ImGui::SameLine();
        if (!state.CanPlayScene()) { ImGui::BeginDisabled(); }
        if (ImGui::Button(ICON_CI_DEBUG_START)) { state.TriggerPlayAction(); }
        if (!state.CanPlayScene()) { ImGui::EndDisabled(); }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Graph editor disabled (ImNodeFlow removed).");
}

void NodeGraphComponent::Shutdown()
{
}

bool NodeGraphComponent::SaveOpenGraph(EngineState& state)
{
    if (!state.HasOpenGraph())
    {
        return false;
    }

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

    state.graph_reload_requested = false;
    state.open_graph_dirty = false;
    state.AddLog("Reloaded graph: " + state.GetOpenGraphDisplayPath());
    return true;
}
