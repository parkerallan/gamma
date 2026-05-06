#include "panels/EffectsPanel.h"

#include "imgui.h"
#include "ui/Codicons.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

void EffectsPanel::Render(EngineState& state)
{
    if (!state.show_effects_panel)
    {
        return;
    }

    if (!ImGui::Begin("Effects", &state.show_effects_panel))
    {
        ImGui::End();
        return;
    }

    static char effect_name[128] = "NewEffect";
    static bool preview_playing = false;
    static bool preview_paused = false;
    static int selected_effect_index = 0;
    static int random_seed = 1337;
    static int max_particles = 20000;
    static int burst_count = 120;
    static int flipbook_columns = 8;
    static int flipbook_rows = 8;
    static int preview_frame = 0;
    static int collision_bounce = 25;
    static int timeline_frame = 0;
    static float duration_seconds = 2.5f;
    static float spawn_rate = 80.0f;
    static float prewarm_seconds = 0.0f;
    static float lifetime_min = 0.6f;
    static float lifetime_max = 1.8f;
    static float speed_min = 0.4f;
    static float speed_max = 6.0f;
    static float drag = 0.12f;
    static float gravity_scale = -0.6f;
    static float emitter_radius = 0.35f;
    static float cone_angle = 18.0f;
    static float turbulence_strength = 1.2f;
    static float turbulence_scale = 0.75f;
    static float turbulence_scroll = 0.9f;
    static float start_size = 0.16f;
    static float mid_size = 0.42f;
    static float end_size = 0.08f;
    static float softness = 0.6f;
    static float distortion = 0.15f;
    static float emissive = 2.0f;
    static float temperature = 0.7f;
    static float shadow_density = 0.25f;
    static float timeline_seconds = 0.0f;
    static float timeline_max_seconds = 6.0f;
    static float flipbook_fps = 24.0f;
    static bool looping = true;
    static bool world_space = true;
    static bool sort_back_to_front = true;
    static bool receive_lighting = true;
    static bool cast_shadows = false;
    static bool collision_enabled = false;
    static bool use_flipbook = false;
    static bool local_space_noise = false;
    static bool use_soft_particles = true;

    static std::vector<std::string> effect_names;
    static std::vector<std::filesystem::path> effect_paths;
    static std::filesystem::path last_scanned_dir;
    static std::uint64_t last_dir_signature = 0;

    // Rebuild effect list only when the directory changes
    const std::filesystem::path current_effects_dir = state.project_root.empty() 
        ? std::filesystem::path() 
        : state.project_root / "Assets" / "Effects";
    
    auto compute_dir_signature = [](const std::filesystem::path& dir) -> std::uint64_t {
        if (dir.empty() || !std::filesystem::is_directory(dir))
        {
            return 0;
        }
        
        std::uint64_t signature = 1;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".fx")
            {
                signature ^= std::hash<std::string>{}(entry.path().filename().string());
                signature *= 1099511628211ull;
            }
        }
        return signature;
    };
    
    const std::uint64_t current_dir_signature = compute_dir_signature(current_effects_dir);
    if (current_effects_dir != last_scanned_dir || current_dir_signature != last_dir_signature)
    {
        last_scanned_dir = current_effects_dir;
        last_dir_signature = current_dir_signature;
        
        effect_names.clear();
        effect_paths.clear();
        effect_names.push_back("New +");
        effect_paths.push_back({});

        if (!current_effects_dir.empty() && std::filesystem::is_directory(current_effects_dir))
        {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(current_effects_dir, ec))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".fx")
                {
                    effect_names.push_back(entry.path().stem().string());
                    effect_paths.push_back(entry.path());
                }
            }
        }
    }

    if (selected_effect_index >= static_cast<int>(effect_paths.size()))
    {
        selected_effect_index = 0;
    }
    if (selected_effect_index == 0 && effect_name[0] == '\0')
    {
        std::snprintf(effect_name, sizeof(effect_name), "%s", "NewEffect");
    }

    timeline_max_seconds = (std::max)(6.0f, duration_seconds);
    if (preview_playing && !preview_paused)
    {
        timeline_seconds += ImGui::GetIO().DeltaTime;
        if (timeline_seconds > timeline_max_seconds)
        {
            if (looping)
            {
                timeline_seconds = std::fmod(timeline_seconds, timeline_max_seconds);
            }
            else
            {
                timeline_seconds = timeline_max_seconds;
                preview_playing = false;
            }
        }
    }
    timeline_seconds = std::clamp(timeline_seconds, 0.0f, timeline_max_seconds);
    timeline_frame = static_cast<int>(std::round(timeline_seconds * 60.0f));

    const std::string save_label = std::string(ICON_CI_SAVE) + " Save";
    const std::string play_pause_label = preview_playing && !preview_paused
        ? std::string(ICON_CI_DEBUG_PAUSE)
        : std::string(ICON_CI_DEBUG_START);
    const std::string restart_label = std::string(ICON_CI_DEBUG_RESTART);
    const std::string stop_label = std::string(ICON_CI_DEBUG_STOP);

    std::vector<const char*> effect_name_ptrs;
    effect_name_ptrs.reserve(effect_names.size());
    for (const auto& name : effect_names)
    {
        effect_name_ptrs.push_back(name.c_str());
    }

    ImGui::SetNextItemWidth(180.0f);
    const int previous_effect_index = selected_effect_index;
    ImGui::Combo("##EffectsSelector", &selected_effect_index, effect_name_ptrs.data(), static_cast<int>(effect_name_ptrs.size()));
    if (selected_effect_index != previous_effect_index)
    {
        if (selected_effect_index == 0)
        {
            if (effect_name[0] == '\0')
            {
                std::snprintf(effect_name, sizeof(effect_name), "%s", "NewEffect");
            }
        }
        else
        {
            effect_name[0] = '\0';
        }
    }

    const bool editing_new_effect = selected_effect_index == 0;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);
    if (!editing_new_effect)
    {
        ImGui::BeginDisabled();
    }
    ImGui::InputTextWithHint("##EffectsName", "", effect_name, sizeof(effect_name));
    if (!editing_new_effect)
    {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (ImGui::Button(save_label.c_str()))
    {
        if (editing_new_effect)
        {
            if (state.project_root.empty() || effect_name[0] == '\0')
            {
                state.AddLog("Cannot create effect: missing project root or effect name");
            }
            else
            {
                const std::filesystem::path effects_dir = state.project_root / "Assets" / "Effects";
                std::error_code ec;
                std::filesystem::create_directories(effects_dir, ec);
                const std::string created_effect_name = effect_name;
                const std::filesystem::path new_effect_path = effects_dir / (created_effect_name + ".fx");

                std::ofstream out(new_effect_path, std::ios::binary);
                if (!out)
                {
                    state.AddLog("Failed to create effect: " + state.GetDisplayPath(new_effect_path));
                }
                else
                {
                    out << "{\n";
                    out << "  \"name\": \"" << created_effect_name << "\",\n";
                    out << "  \"version\": 1\n";
                    out << "}\n";
                    out.close();

                    effect_names.push_back(created_effect_name);
                    effect_paths.push_back(new_effect_path);
                    selected_effect_index = static_cast<int>(effect_paths.size()) - 1;
                    effect_name[0] = '\0';
                    state.request_files_tree_refresh = true;
                    state.AddLog("Created effect: " + created_effect_name);
                }
            }
        }
        else
        {
            const std::filesystem::path active_effect_path = effect_paths[selected_effect_index];
            std::ofstream out(active_effect_path, std::ios::binary);
            if (!out)
            {
                state.AddLog("Failed to save effect: " + state.GetDisplayPath(active_effect_path));
            }
            else
            {
                const std::string active_effect_name = active_effect_path.stem().string();
                out << "{\n";
                out << "  \"name\": \"" << active_effect_name << "\",\n";
                out << "  \"version\": 1\n";
                out << "}\n";
                out.close();
                state.AddLog("Saved effect: " + active_effect_name);
            }
        }
    }

    const float content_height = ImGui::GetContentRegionAvail().y;
    const float region_height = (std::max)(120.0f, content_height);
    const float left_width = ImGui::GetContentRegionAvail().x * 0.44f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float timeline_height = 110.0f;
    const float preview_height = (std::max)(120.0f, region_height - timeline_height - spacing);

    ImGui::BeginChild("##EffectsPreviewColumn", ImVec2(left_width, region_height), true);

    const ImVec2 preview_size = ImVec2(ImGui::GetContentRegionAvail().x, preview_height);
    ImGui::BeginChild("##EffectsPreviewCanvas", preview_size, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 min = ImGui::GetWindowPos();
    const ImVec2 max = ImVec2(min.x + ImGui::GetWindowSize().x, min.y + ImGui::GetWindowSize().y);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilledMultiColor(
        min,
        max,
        IM_COL32(18, 20, 26, 255),
        IM_COL32(26, 31, 40, 255),
        IM_COL32(12, 14, 18, 255),
        IM_COL32(18, 22, 28, 255));
    draw_list->AddRect(min, max, IM_COL32(84, 92, 105, 255), 8.0f, 0, 1.5f);

    const char* status_text = !preview_playing ? "Stopped" : (preview_paused ? "Paused" : "Playing");
    draw_list->AddText(ImVec2(min.x + 14.0f, min.y + 12.0f), IM_COL32(236, 240, 245, 255), status_text);

    const std::string stats_line = std::string("Particles: ") + std::to_string(max_particles) +
        "  |  Spawn/s: " + std::to_string(static_cast<int>(spawn_rate));
    draw_list->AddText(ImVec2(min.x + 14.0f, min.y + 32.0f), IM_COL32(166, 178, 195, 255), stats_line.c_str());

    const char* placeholder = "RT effect preview scaffold";
    const ImVec2 text_size = ImGui::CalcTextSize(placeholder);
    draw_list->AddText(
        ImVec2((min.x + max.x - text_size.x) * 0.5f, (min.y + max.y - text_size.y) * 0.5f),
        IM_COL32(220, 226, 236, 255),
        placeholder);
    ImGui::EndChild();

    ImGui::Spacing();

    if (ImGui::Button(play_pause_label.c_str()))
    {
        if (!preview_playing)
        {
            preview_playing = true;
            preview_paused = false;
            state.AddLog("Effects preview started");
        }
        else
        {
            preview_paused = !preview_paused;
            state.AddLog(preview_paused ? "Effects preview paused" : "Effects preview resumed");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(restart_label.c_str()))
    {
        preview_playing = true;
        preview_paused = false;
        timeline_seconds = 0.0f;
        timeline_frame = 0;
        state.AddLog("Effects preview restarted");
    }
    ImGui::SameLine();
    if (ImGui::Button(stop_label.c_str()))
    {
        preview_playing = false;
        preview_paused = false;
        timeline_seconds = 0.0f;
        timeline_frame = 0;
        state.AddLog("Effects preview stopped");
    }
    ImGui::SameLine();
    ImGui::Text("Time %.2fs / %.2fs", timeline_seconds, timeline_max_seconds);
    ImGui::SameLine();
    ImGui::Text("Frame %d", timeline_frame);

    ImGui::Spacing();

    const ImVec2 timeline_origin = ImGui::GetCursorScreenPos();
    const float timeline_width = (std::max)(120.0f, ImGui::GetContentRegionAvail().x);
    const float track_height = 34.0f;
    const ImVec2 timeline_size = ImVec2(timeline_width, track_height);
    ImGui::InvisibleButton("##EffectsTimelineScrub", timeline_size);

    const bool timeline_hovered = ImGui::IsItemHovered();
    if (timeline_hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const float mouse_x = ImGui::GetIO().MousePos.x;
        const float normalized = std::clamp((mouse_x - timeline_origin.x) / timeline_width, 0.0f, 1.0f);
        timeline_seconds = normalized * timeline_max_seconds;
        timeline_frame = static_cast<int>(std::round(timeline_seconds * 60.0f));
        preview_paused = true;
        preview_playing = true;
    }

    ImDrawList* timeline_draw_list = ImGui::GetWindowDrawList();
    const ImVec2 track_min = timeline_origin;
    const ImVec2 track_max = ImVec2(timeline_origin.x + timeline_width, timeline_origin.y + track_height);
    timeline_draw_list->AddRectFilled(track_min, track_max, IM_COL32(20, 24, 30, 255), 6.0f);
    timeline_draw_list->AddRect(track_min, track_max, IM_COL32(76, 86, 98, 255), 6.0f, 0, 1.0f);

    const int major_tick_count = (std::max)(1, static_cast<int>(std::ceil(timeline_max_seconds)));
    for (int tick_index = 0; tick_index <= major_tick_count; ++tick_index)
    {
        const float normalized = major_tick_count == 0 ? 0.0f : static_cast<float>(tick_index) / static_cast<float>(major_tick_count);
        const float x = track_min.x + normalized * timeline_width;
        const float tick_height = (tick_index % 5 == 0) ? 18.0f : 10.0f;
        timeline_draw_list->AddLine(
            ImVec2(x, track_max.y - tick_height),
            ImVec2(x, track_max.y - 4.0f),
            IM_COL32(125, 138, 153, 255),
            1.0f);

        char label[16]{};
        std::snprintf(label, sizeof(label), "%ds", tick_index);
        const ImVec2 label_size = ImGui::CalcTextSize(label);
        timeline_draw_list->AddText(
            ImVec2(x - label_size.x * 0.5f, track_max.y + 6.0f),
            IM_COL32(166, 178, 195, 255),
            label);
    }

    const float playhead_normalized = timeline_max_seconds > 0.0f ? timeline_seconds / timeline_max_seconds : 0.0f;
    const float playhead_x = track_min.x + playhead_normalized * timeline_width;
    timeline_draw_list->AddLine(
        ImVec2(playhead_x, track_min.y + 2.0f),
        ImVec2(playhead_x, track_max.y - 2.0f),
        IM_COL32(198, 232, 58, 255),
        2.0f);
    timeline_draw_list->AddCircleFilled(ImVec2(playhead_x, track_min.y + 8.0f), 5.0f, IM_COL32(198, 232, 58, 255));

    ImGui::Dummy(ImVec2(0.0f, 32.0f));
    ImGui::EndChild();

    ImGui::SameLine(0.0f, spacing);

    ImGui::BeginChild("##EffectsSettingsColumn", ImVec2(0.0f, region_height), true);
    
    if (ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Duration (s)", &duration_seconds, 0.05f, 30.0f, "%.2f");
        ImGui::SliderFloat("Prewarm (s)", &prewarm_seconds, 0.0f, 10.0f, "%.2f");
        ImGui::SliderFloat("Spawn Rate", &spawn_rate, 0.0f, 2000.0f, "%.1f");
        ImGui::SliderInt("Burst Count", &burst_count, 0, 5000);
        ImGui::SliderInt("Max Particles", &max_particles, 64, 300000);
        ImGui::SliderInt("Random Seed", &random_seed, 0, 65535);
        ImGui::Checkbox("Looping", &looping);
        ImGui::Checkbox("World Space", &world_space);
    }

    if (ImGui::CollapsingHeader("Lifetime", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Lifetime Min", &lifetime_min, 0.01f, 12.0f, "%.2f");
        ImGui::SliderFloat("Lifetime Max", &lifetime_max, 0.01f, 12.0f, "%.2f");
        if (lifetime_max < lifetime_min)
        {
            lifetime_max = lifetime_min;
        }
        ImGui::SliderFloat("Speed Min", &speed_min, 0.0f, 40.0f, "%.2f");
        ImGui::SliderFloat("Speed Max", &speed_max, 0.0f, 40.0f, "%.2f");
        if (speed_max < speed_min)
        {
            speed_max = speed_min;
        }
        ImGui::SliderFloat("Drag", &drag, 0.0f, 10.0f, "%.2f");
        ImGui::SliderFloat("Gravity Scale", &gravity_scale, -8.0f, 8.0f, "%.2f");
    }

    if (ImGui::CollapsingHeader("Emitter Shape", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Radius", &emitter_radius, 0.0f, 8.0f, "%.2f");
        ImGui::SliderFloat("Cone Angle", &cone_angle, 0.0f, 180.0f, "%.1f");
        ImGui::Checkbox("Collision Enabled", &collision_enabled);
        if (collision_enabled)
        {
            ImGui::SliderInt("Collision Bounce %", &collision_bounce, 0, 100);
        }
    }

    if (ImGui::CollapsingHeader("Noise and Turbulence", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Strength", &turbulence_strength, 0.0f, 12.0f, "%.2f");
        ImGui::SliderFloat("Scale", &turbulence_scale, 0.01f, 8.0f, "%.2f");
        ImGui::SliderFloat("Scroll Speed", &turbulence_scroll, 0.0f, 10.0f, "%.2f");
        ImGui::Checkbox("Local Space Noise", &local_space_noise);
    }

    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderFloat("Start Size", &start_size, 0.001f, 5.0f, "%.3f");
        ImGui::SliderFloat("Mid Size", &mid_size, 0.001f, 5.0f, "%.3f");
        ImGui::SliderFloat("End Size", &end_size, 0.001f, 5.0f, "%.3f");
        ImGui::SliderFloat("Softness", &softness, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Distortion", &distortion, 0.0f, 2.0f, "%.2f");
        ImGui::SliderFloat("Emissive", &emissive, 0.0f, 32.0f, "%.2f");
        ImGui::SliderFloat("Temperature", &temperature, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("Shadow Density", &shadow_density, 0.0f, 1.0f, "%.2f");
        ImGui::Checkbox("Sort Back-To-Front", &sort_back_to_front);
        ImGui::Checkbox("Receive Lighting", &receive_lighting);
        ImGui::Checkbox("Cast Shadows", &cast_shadows);
        ImGui::Checkbox("Soft Particles", &use_soft_particles);
    }

    if (ImGui::CollapsingHeader("Flipbook", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Use Flipbook", &use_flipbook);
        if (use_flipbook)
        {
            ImGui::SliderInt("Columns", &flipbook_columns, 1, 32);
            ImGui::SliderInt("Rows", &flipbook_rows, 1, 32);
            ImGui::SliderFloat("Frames Per Second", &flipbook_fps, 1.0f, 120.0f, "%.1f");
            const int frame_count = (std::max)(1, flipbook_columns * flipbook_rows);
            if (preview_frame >= frame_count)
            {
                preview_frame = frame_count - 1;
            }
            ImGui::SliderInt("Preview Frame", &preview_frame, 0, frame_count - 1);
        }
    }

    ImGui::EndChild();

    ImGui::End();
}
