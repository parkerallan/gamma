#include "panels/PerformancePanel.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cfloat>
#include <string>

namespace
{
struct GraphSeries
{
    const char* label = "";
    const std::vector<float>* history = nullptr;
    const bool* visible = nullptr;
    ImU32 color = 0;
    bool instrumented = true;
};

void AppendWithCapacity(std::vector<float>& history, float value, std::size_t capacity)
{
    history.push_back(value);
    if (history.size() > capacity)
    {
        const std::size_t remove_count = history.size() - capacity;
        history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(remove_count));
    }
}

float FindHistoryMax(const std::vector<float>& history)
{
    if (history.empty())
    {
        return 0.0f;
    }

    const auto it = std::max_element(history.begin(), history.end());
    return (std::max)(*it, 0.0f);
}

float FindRecentHistoryMax(const std::vector<float>& history, std::size_t recent_sample_count)
{
    if (history.empty())
    {
        return 0.0f;
    }

    if (recent_sample_count == 0 || history.size() <= recent_sample_count)
    {
        return FindHistoryMax(history);
    }

    const auto start = history.end() - static_cast<std::ptrdiff_t>(recent_sample_count);
    const auto it = std::max_element(start, history.end());
    return (std::max)(*it, 0.0f);
}

template <std::size_t N>
float FindVisibleHistoryMax(const std::array<GraphSeries, N>& series, std::size_t recent_sample_count = 0)
{
    float max_value = 0.0f;
    for (const GraphSeries& item : series)
    {
        if (item.history == nullptr)
        {
            continue;
        }

        if (item.visible != nullptr && !*item.visible)
        {
            continue;
        }

        max_value = (std::max)(max_value, FindRecentHistoryMax(*item.history, recent_sample_count));
    }

    return max_value;
}

template <std::size_t N>
void DrawOverlayGraph(const char* id, const std::array<GraphSeries, N>& series, float max_value, const char* unit_label)
{
    const ImVec2 size(0.0f, 176.0f);
    ImGui::PushID(id);
    ImGui::InvisibleButton("##OverlayGraph", size);
    const ImVec2 rect_min = ImGui::GetItemRectMin();
    const ImVec2 rect_max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    draw_list->AddRectFilled(rect_min, rect_max, IM_COL32(20, 23, 28, 255), 6.0f);
    draw_list->AddRect(rect_min, rect_max, IM_COL32(58, 64, 74, 255), 6.0f);

    constexpr int kGridLines = 4;
    for (int index = 1; index < kGridLines; ++index)
    {
        const float t = static_cast<float>(index) / static_cast<float>(kGridLines);
        const float y = rect_max.y + (rect_min.y - rect_max.y) * t;
        draw_list->AddLine(ImVec2(rect_min.x, y), ImVec2(rect_max.x, y), IM_COL32(42, 47, 55, 255), 1.0f);
    }

    const float safe_max_value = (std::max)(max_value, 0.001f);
    char top_label[32] = {};
    char mid_label[32] = {};
    const char* label_format = safe_max_value < 1.0f ? "%.3f %s" : "%.1f %s";
    std::snprintf(top_label, sizeof(top_label), label_format, safe_max_value, unit_label);
    std::snprintf(mid_label, sizeof(mid_label), label_format, safe_max_value * 0.5f, unit_label);
    draw_list->AddText(ImVec2(rect_min.x + 8.0f, rect_min.y + 6.0f), IM_COL32(145, 152, 163, 255), top_label);
    draw_list->AddText(ImVec2(rect_min.x + 8.0f, rect_min.y + (rect_max.y - rect_min.y) * 0.5f - 8.0f), IM_COL32(110, 116, 128, 255), mid_label);

    for (const GraphSeries& item : series)
    {
        if (item.history == nullptr || item.history->size() < 2)
        {
            continue;
        }

        if (item.visible != nullptr && !*item.visible)
        {
            continue;
        }

        const std::vector<float>& history = *item.history;
        const float width = rect_max.x - rect_min.x;
        const float height = rect_max.y - rect_min.y;
        for (std::size_t index = 1; index < history.size(); ++index)
        {
            const float prev_x = rect_min.x + (static_cast<float>(index - 1) / static_cast<float>(history.size() - 1)) * width;
            const float current_x = rect_min.x + (static_cast<float>(index) / static_cast<float>(history.size() - 1)) * width;
            const float prev_y = rect_max.y - (std::clamp(history[index - 1], 0.0f, safe_max_value) / safe_max_value) * height;
            const float current_y = rect_max.y - (std::clamp(history[index], 0.0f, safe_max_value) / safe_max_value) * height;
            draw_list->AddLine(ImVec2(prev_x, prev_y), ImVec2(current_x, current_y), item.color, 1.8f);
        }
    }

    ImGui::PopID();
}
}

void PerformancePanel::Render(EngineState& state, const RuntimeRenderer& runtime_renderer)
{
    if (!state.show_performance_panel)
    {
        return;
    }

    if (!ImGui::Begin("Performance", &state.show_performance_panel))
    {
        ImGui::End();
        return;
    }

    const RuntimeRenderer::RuntimePerformanceStats& stats = runtime_renderer.GetPerformanceStats();

    if (!state.is_playing)
    {
        if (!fps_history_.empty())
        {
            ClearHistory();
        }

        ImGui::TextUnformatted("Runtime is not playing.");
        ImGui::End();
        return;
    }

    if (!stats.valid)
    {
        ImGui::TextUnformatted("Waiting for runtime frame data...");
        ImGui::End();
        return;
    }

    PushSample(stats);

    const float fps_display = stats.fps;
    
    ImGui::Text("Total FPS: %.1f", fps_display);

    const std::array<GraphSeries, 1> fps_series = {{
        {"All", &fps_history_, nullptr, IM_COL32(255, 231, 122, 255), true},
    }};
    const float fps_graph_max = (std::max)(FindVisibleHistoryMax(fps_series, 60) * 1.10f, 60.0f);
    DrawOverlayGraph("FPSGraph", fps_series, fps_graph_max, "FPS");

    ImGui::Separator();
    ImGui::TextUnformatted("Subsystem Time (ms)");

    const std::array<GraphSeries, 7> subsystem_series = {{
        {"Physics", &physics_ms_history_, &show_physics_, IM_COL32(120, 214, 255, 255), true},
        {"Scripts", &scripts_ms_history_, &show_scripts_, IM_COL32(150, 255, 160, 255), true},
        {"Render", &render_ms_history_, &show_render_, IM_COL32(255, 150, 120, 255), true},
        {"2D", &overlay_2d_ms_history_, &show_2d_, IM_COL32(205, 160, 255, 255), true},
        {"Animation", &animation_ms_history_, &show_animation_, IM_COL32(255, 196, 92, 180), false},
        {"Audio", &audio_ms_history_, &show_audio_, IM_COL32(255, 120, 190, 180), false},
        {"Video", &video_ms_history_, &show_video_, IM_COL32(180, 180, 180, 180), false},
    }};
    const float subsystem_graph_max = (std::max)(FindVisibleHistoryMax(subsystem_series, 12) * 1.10f, 0.001f);
    DrawOverlayGraph("SubsystemTimingGraph", subsystem_series, subsystem_graph_max, "ms");

    ImGui::Separator();
    ImGui::TextUnformatted("Subsystem Filters");
    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Define filter state and colors parallel to series
    std::array<std::pair<const char*, std::pair<bool*, ImU32>>, 7> filters = {{
        {"Physics", {&show_physics_, IM_COL32(120, 214, 255, 255)}},
        {"Scripts", {&show_scripts_, IM_COL32(150, 255, 160, 255)}},
        {"Render", {&show_render_, IM_COL32(255, 150, 120, 255)}},
        {"2D", {&show_2d_, IM_COL32(205, 160, 255, 255)}},
        {"Animation", {&show_animation_, IM_COL32(255, 196, 92, 180)}},
        {"Audio", {&show_audio_, IM_COL32(255, 120, 190, 180)}},
        {"Video", {&show_video_, IM_COL32(180, 180, 180, 180)}},
    }};

    for (std::size_t index = 0; index < filters.size(); ++index)
    {
        const char* label = filters[index].first;
        bool* visible_ptr = filters[index].second.first;
        const ImU32 color = filters[index].second.second;

        ImGui::Checkbox(label, visible_ptr);
        
        // Draw colored outline around the checkbox
        const ImVec2 item_min = ImGui::GetItemRectMin();
        const ImVec2 item_max = ImGui::GetItemRectMax();
        const ImVec2 padded_min(item_min.x - 4.0f, item_min.y - 2.0f);
        const ImVec2 padded_max(item_max.x + 4.0f, item_max.y + 2.0f);
        draw_list->AddRect(padded_min, padded_max, color, 3.0f, 0, 1.5f);

        if (index < 4 || index == 5)
        {
            ImGui::SameLine();
        }
    }
    ImGui::NewLine();

    if (ImGui::Button("Clear History"))
    {
        ClearHistory();
    }

    ImGui::Text(
        "Latest ms: physics %.3f | scripts %.3f | render %.3f | 2D %.3f",
        stats.physics_time_ms,
        stats.scripts_time_ms,
        stats.render_time_ms,
        stats.overlay_2d_time_ms);

    ImGui::End();
}

void PerformancePanel::PushSample(const RuntimeRenderer::RuntimePerformanceStats& stats)
{
    // Downsample graph updates by averaging over a short time window.
    sample_window_seconds_ += stats.frame_time_ms * 0.001f;
    sample_window_count_ += 1;
    fps_accumulator_ += stats.fps;
    physics_accumulator_ += (std::max)(stats.physics_time_ms, 0.0f);
    scripts_accumulator_ += (std::max)(stats.scripts_time_ms, 0.0f);
    render_accumulator_ += (std::max)(stats.render_time_ms, 0.0f);
    overlay_2d_accumulator_ += (std::max)(stats.overlay_2d_time_ms, 0.0f);
    animation_accumulator_ += (std::max)(stats.animation_time_ms, 0.0f);
    audio_accumulator_ += (std::max)(stats.audio_time_ms, 0.0f);
    video_accumulator_ += (std::max)(stats.video_time_ms, 0.0f);

    if (sample_window_seconds_ < kSampleIntervalSeconds || sample_window_count_ == 0)
    {
        return;
    }

    const float inv_count = 1.0f / static_cast<float>(sample_window_count_);
    AppendWithCapacity(fps_history_, fps_accumulator_ * inv_count, kHistoryCapacity);
    AppendWithCapacity(physics_ms_history_, physics_accumulator_ * inv_count, kHistoryCapacity);
    AppendWithCapacity(scripts_ms_history_, scripts_accumulator_ * inv_count, kHistoryCapacity);
    AppendWithCapacity(render_ms_history_, render_accumulator_ * inv_count, kHistoryCapacity);
    AppendWithCapacity(overlay_2d_ms_history_, overlay_2d_accumulator_ * inv_count, kHistoryCapacity);
    AppendWithCapacity(animation_ms_history_, animation_accumulator_ * inv_count, kHistoryCapacity);
    AppendWithCapacity(audio_ms_history_, audio_accumulator_ * inv_count, kHistoryCapacity);
    AppendWithCapacity(video_ms_history_, video_accumulator_ * inv_count, kHistoryCapacity);

    sample_window_seconds_ = 0.0f;
    sample_window_count_ = 0;
    fps_accumulator_ = 0.0f;
    physics_accumulator_ = 0.0f;
    scripts_accumulator_ = 0.0f;
    render_accumulator_ = 0.0f;
    overlay_2d_accumulator_ = 0.0f;
    animation_accumulator_ = 0.0f;
    audio_accumulator_ = 0.0f;
    video_accumulator_ = 0.0f;
}

void PerformancePanel::ClearHistory()
{
    fps_history_.clear();
    physics_ms_history_.clear();
    scripts_ms_history_.clear();
    render_ms_history_.clear();
    overlay_2d_ms_history_.clear();
    animation_ms_history_.clear();
    audio_ms_history_.clear();
    video_ms_history_.clear();

    sample_window_seconds_ = 0.0f;
    sample_window_count_ = 0;
    fps_accumulator_ = 0.0f;
    physics_accumulator_ = 0.0f;
    scripts_accumulator_ = 0.0f;
    render_accumulator_ = 0.0f;
    overlay_2d_accumulator_ = 0.0f;
    animation_accumulator_ = 0.0f;
    audio_accumulator_ = 0.0f;
    video_accumulator_ = 0.0f;
}
