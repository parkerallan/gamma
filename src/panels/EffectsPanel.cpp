#include "panels/EffectsPanel.h"

#include "app/VulkanContext.h"
#include "imgui.h"
#include "ui/Codicons.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace
{
std::string LowerExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return extension;
}

bool IsEffectFile(const std::filesystem::path& path)
{
    const std::string extension = LowerExtension(path);
    return extension == ".efk" || extension == ".efkefc";
}

std::string MakeEffectDisplayName(const std::filesystem::path& effects_dir, const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path relative_path = std::filesystem::relative(path, effects_dir, error);
    if (error)
    {
        relative_path = path.filename();
    }
    return relative_path.generic_string();
}
}

void EffectsPanel::Render(EngineState& state, VulkanContext* vulkan_context)
{
    if (!state.show_effects_panel)
    {
        return;
    }

    if (!ImGui::Begin("Effects", &state.show_effects_panel, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImGui::End();
        return;
    }

    static bool preview_playing = false;
    static bool preview_paused = false;
    static int selected_effect_index = 0;
    static int timeline_frame = 0;
    static float timeline_seconds = 0.0f;
    static float timeline_max_seconds = 6.0f;
    static bool looping = true;

    static std::vector<std::string> effect_names;
    static std::vector<std::filesystem::path> effect_paths;
    static std::filesystem::path selected_effect_path;
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
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
        {
            if (entry.is_regular_file(ec) && IsEffectFile(entry.path()))
            {
                signature ^= std::hash<std::string>{}(entry.path().generic_string());
                std::error_code time_error;
                const auto write_time = entry.last_write_time(time_error);
                if (!time_error)
                {
                    signature ^= std::hash<std::int64_t>{}(write_time.time_since_epoch().count());
                }
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
        effect_names.push_back("Select effect...");
        effect_paths.push_back({});

        if (!current_effects_dir.empty() && std::filesystem::is_directory(current_effects_dir))
        {
            std::error_code ec;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(current_effects_dir, ec))
            {
                if (entry.is_regular_file(ec) && IsEffectFile(entry.path()))
                {
                    effect_names.push_back(MakeEffectDisplayName(current_effects_dir, entry.path()));
                    effect_paths.push_back(entry.path());
                }
            }
        }

        std::vector<std::size_t> sorted_indices;
        sorted_indices.reserve(effect_paths.size() > 0 ? effect_paths.size() - 1 : 0);
        for (std::size_t index = 1; index < effect_paths.size(); ++index)
        {
            sorted_indices.push_back(index);
        }
        std::sort(sorted_indices.begin(), sorted_indices.end(), [&](std::size_t lhs, std::size_t rhs)
        {
            return effect_names[lhs] < effect_names[rhs];
        });

        std::vector<std::string> sorted_names;
        std::vector<std::filesystem::path> sorted_paths;
        sorted_names.push_back(effect_names.front());
        sorted_paths.push_back(effect_paths.front());
        for (const std::size_t index : sorted_indices)
        {
            sorted_names.push_back(effect_names[index]);
            sorted_paths.push_back(effect_paths[index]);
        }
        effect_names = std::move(sorted_names);
        effect_paths = std::move(sorted_paths);

        selected_effect_index = 0;
        if (!selected_effect_path.empty())
        {
            for (std::size_t index = 1; index < effect_paths.size(); ++index)
            {
                if (effect_paths[index] == selected_effect_path)
                {
                    selected_effect_index = static_cast<int>(index);
                    break;
                }
            }
        }
        if (selected_effect_index == 0 && effect_paths.size() > 1)
        {
            selected_effect_index = 1;
            selected_effect_path = effect_paths[selected_effect_index];
        }
    }

    if (selected_effect_index >= static_cast<int>(effect_paths.size()))
    {
        selected_effect_index = 0;
    }

    timeline_max_seconds = 6.0f;
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

    ImGui::SetNextItemWidth(260.0f);
    const int previous_effect_index = selected_effect_index;
    ImGui::Combo("##CurrentEffect", &selected_effect_index, effect_name_ptrs.data(), static_cast<int>(effect_name_ptrs.size()));
    if (selected_effect_index != previous_effect_index)
    {
        selected_effect_path = selected_effect_index > 0 && selected_effect_index < static_cast<int>(effect_paths.size())
            ? effect_paths[selected_effect_index]
            : std::filesystem::path();
        timeline_seconds = 0.0f;
        timeline_frame = 0;
        preview_renderer_.Restart();
    }

    const float content_height = ImGui::GetContentRegionAvail().y;
    const float region_height = (std::max)(120.0f, content_height);
    const float timeline_height = 110.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    const float preview_height = (std::max)(120.0f, region_height - timeline_height - spacing);

    const ImVec2 preview_size = ImVec2(ImGui::GetContentRegionAvail().x, preview_height);
    ImGui::InvisibleButton("##EffectsPreviewCanvas", preview_size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    preview_renderer_.HandleMouseControls(min, max);
    const std::filesystem::path active_effect_path =
        selected_effect_index > 0 && selected_effect_index < static_cast<int>(effect_paths.size())
            ? effect_paths[selected_effect_index]
            : std::filesystem::path();

    const std::uint32_t preview_width = static_cast<std::uint32_t>((std::max)(1.0f, preview_size.x));
    const std::uint32_t preview_height_u = static_cast<std::uint32_t>((std::max)(1.0f, preview_size.y));
    const ImTextureID effect_preview = preview_renderer_.Render(
        vulkan_context,
        active_effect_path,
        preview_width,
        preview_height_u,
        timeline_seconds,
        preview_playing,
        preview_paused,
        looping);

    if (effect_preview)
    {
        draw_list->AddImage(effect_preview, min, max);
    }
    else
    {
        draw_list->AddRectFilledMultiColor(
            min,
            max,
            IM_COL32(18, 20, 26, 255),
            IM_COL32(26, 31, 40, 255),
            IM_COL32(12, 14, 18, 255),
            IM_COL32(18, 22, 28, 255));
    }
    preview_renderer_.DrawGridOverlay(draw_list, min, max);
    draw_list->AddRect(min, max, IM_COL32(84, 92, 105, 255), 8.0f, 0, 1.5f);

    const char* status_text = !preview_playing ? "Stopped" : (preview_paused ? "Paused" : "Playing");
    draw_list->AddText(ImVec2(min.x + 14.0f, min.y + 12.0f), IM_COL32(236, 240, 245, 255), status_text);

    const char* placeholder = active_effect_path.empty()
        ? "Select an Effekseer effect"
        : (preview_renderer_.LastError().empty() ? nullptr : preview_renderer_.LastError().c_str());
    if (placeholder != nullptr && !effect_preview)
    {
        const ImVec2 text_size = ImGui::CalcTextSize(placeholder);
        draw_list->AddText(
            ImVec2((min.x + max.x - text_size.x) * 0.5f, (min.y + max.y - text_size.y) * 0.5f),
            IM_COL32(220, 226, 236, 255),
            placeholder);
    }
    else if (effect_preview && !preview_renderer_.LastError().empty())
    {
        const std::string warning = preview_renderer_.LastError();
        const ImVec2 text_size = ImGui::CalcTextSize(warning.c_str());
        const ImVec2 warning_min(min.x + 10.0f, max.y - text_size.y - 18.0f);
        const ImVec2 warning_max((std::min)(max.x - 10.0f, warning_min.x + text_size.x + 16.0f), max.y - 8.0f);
        draw_list->AddRectFilled(warning_min, warning_max, IM_COL32(35, 26, 15, 225), 5.0f);
        draw_list->AddRect(warning_min, warning_max, IM_COL32(210, 150, 75, 210), 5.0f);
        const ImVec4 clip_rect(warning_min.x + 8.0f, warning_min.y + 4.0f, warning_max.x - 8.0f, warning_max.y - 4.0f);
        draw_list->AddText(
            ImGui::GetFont(),
            ImGui::GetFontSize(),
            ImVec2(warning_min.x + 8.0f, warning_min.y + 5.0f),
            IM_COL32(255, 218, 160, 255),
            warning.c_str(),
            nullptr,
            0.0f,
            &clip_rect);
    }
    ImGui::Spacing();

    if (ImGui::Button(play_pause_label.c_str()))
    {
        if (!preview_playing)
        {
            preview_playing = true;
            preview_paused = false;
            preview_renderer_.Restart();
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
        preview_renderer_.Restart();
        state.AddLog("Effects preview restarted");
    }
    ImGui::SameLine();
    if (ImGui::Button(stop_label.c_str()))
    {
        preview_playing = false;
        preview_paused = false;
        timeline_seconds = 0.0f;
        timeline_frame = 0;
        preview_renderer_.Stop();
        state.AddLog("Effects preview stopped");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &looping);
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

    ImGui::End();
}
