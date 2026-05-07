#include "panels/AnimatorPanel.h"

#include "imgui.h"
#include "state/EngineState.h"
#include "ui/Codicons.h"

#include <filesystem>
#include <fstream>
#include <vector>

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

    if (graph_names.empty() || graph_paths.empty())
    {
        graph_names.clear();
        graph_paths.clear();
        graph_names.push_back("New +");
        graph_paths.push_back({});
    }

    if (selected_graph_index < 0)
    {
        selected_graph_index = 0;
    }
    if (selected_graph_index >= static_cast<int>(graph_paths.size()))
    {
        selected_graph_index = static_cast<int>(graph_paths.size()) - 1;
    }

    // Sync combo selection only when the open graph changes externally (e.g. file tree click)
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

                std::ofstream output(new_path, std::ios::binary | std::ios::trunc);
                if (output)
                {
                    output << "{}\n";
                    const std::string created_graph_name = new_graph_name;
                    graph_names.push_back(created_graph_name);
                    graph_paths.push_back(new_path);
                    selected_graph_index = static_cast<int>(graph_paths.size()) - 1;
                    new_graph_name[0] = '\0';
                    state.AddLog(std::string("Created graph: ") + created_graph_name);
                }
                else
                {
                    state.AddLog("Failed to create graph: " + state.GetDisplayPath(new_path));
                }
            }
        }
        else
        {
            if (selected_graph_index >= 0 && selected_graph_index < static_cast<int>(graph_paths.size()))
            {
                state.AddLog("Saved graph: " + graph_paths[selected_graph_index].filename().string());
            }
            else
            {
                selected_graph_index = 0;
                state.AddLog("Saved graph");
            }
        }
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
    (void)editing_graph_name;
    ImGui::EndChild();

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
    // No runtime graph state to clean up.
}

void AnimatorPanel::RenderNodeLibrary()
{
    ImGui::TextUnformatted("Animations");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("No animations present.");
}


