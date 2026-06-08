#include "panels/SequencerPanel.h"

#include "app/VulkanContext.h"
#include "imgui.h"
#include "ui/Codicons.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace
{
bool IsSceneFile(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".scene";
}

std::uint64_t ComputeSceneDirSignature(const std::filesystem::path& dir)
{
    if (dir.empty() || !std::filesystem::is_directory(dir))
    {
        return 0;
    }
    std::uint64_t sig = 1;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
    {
        if (entry.is_regular_file(ec) && IsSceneFile(entry.path()))
        {
            sig ^= std::hash<std::string>{}(entry.path().generic_string());
            sig *= 1099511628211ull;
        }
    }
    return sig;
}
} // namespace

void SequencerPanel::RefreshSceneList(const std::filesystem::path& scenes_dir)
{
    const std::uint64_t sig = ComputeSceneDirSignature(scenes_dir);
    if (scenes_dir == last_scanned_dir_ && sig == last_dir_signature_)
    {
        return;
    }
    last_scanned_dir_ = scenes_dir;
    last_dir_signature_ = sig;

    const std::filesystem::path prev_path =
        selected_scene_index_ > 0 && selected_scene_index_ < static_cast<int>(scene_paths_.size())
            ? scene_paths_[selected_scene_index_]
            : std::filesystem::path();

    scene_names_.clear();
    scene_paths_.clear();
    scene_names_.push_back("Select scene...");
    scene_paths_.push_back({});

    if (!scenes_dir.empty() && std::filesystem::is_directory(scenes_dir))
    {
        std::error_code ec;
        std::vector<std::filesystem::path> found;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(scenes_dir, ec))
        {
            if (entry.is_regular_file(ec) && IsSceneFile(entry.path()))
            {
                found.push_back(entry.path());
            }
        }
        std::sort(found.begin(), found.end(), [](const std::filesystem::path& a, const std::filesystem::path& b)
        {
            return a.filename().string() < b.filename().string();
        });
        for (const auto& p : found)
        {
            scene_names_.push_back(p.stem().string());
            scene_paths_.push_back(p);
        }
    }

    // Restore previous selection by path
    selected_scene_index_ = 0;
    if (!prev_path.empty())
    {
        for (int i = 1; i < static_cast<int>(scene_paths_.size()); ++i)
        {
            if (scene_paths_[i] == prev_path)
            {
                selected_scene_index_ = i;
                break;
            }
        }
    }
}

void SequencerPanel::Render(EngineState& state, VulkanContext* /*vulkan_context*/)
{
    if (!state.show_sequencer_panel)
    {
        return;
    }

    if (!ImGui::Begin("Sequencer", &state.show_sequencer_panel, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImGui::End();
        return;
    }

    // ---- Toolbar ----
    const std::filesystem::path scenes_dir = state.project_root.empty()
        ? std::filesystem::path()
        : state.project_root / "Scenes";
    RefreshSceneList(scenes_dir);

    std::vector<const char*> name_ptrs;
    name_ptrs.reserve(scene_names_.size());
    for (const auto& n : scene_names_) { name_ptrs.push_back(n.c_str()); }

    ImGui::SetNextItemWidth(200.0f);
    ImGui::Combo("##SequencerSceneSelector", &selected_scene_index_, name_ptrs.data(), static_cast<int>(name_ptrs.size()));

    // Right-aligned: Save, Build, Play
    {
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float save_w  = ImGui::CalcTextSize(ICON_CI_SAVE).x         + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float build_w = ImGui::CalcTextSize(ICON_CI_RUN_WITH_DEPS).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float play_w  = ImGui::CalcTextSize(ICON_CI_DEBUG_START).x   + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float toolbar_total = save_w + build_w + play_w + sp * 2.0f;
        const float cur_x = ImGui::GetCursorPosX();
        const float avail  = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine();
        if (avail > toolbar_total)
        {
            ImGui::SetCursorPosX(cur_x + avail - toolbar_total);
        }
        ImGui::BeginDisabled(true); // Save not yet implemented for sequencer
        ImGui::Button(ICON_CI_SAVE "##SeqSave");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (!state.CanBuildProject()) { ImGui::BeginDisabled(); }
        if (ImGui::Button(ICON_CI_RUN_WITH_DEPS "##SeqBuild")) { state.TriggerBuildAction(); }
        if (!state.CanBuildProject()) { ImGui::EndDisabled(); }
        ImGui::SameLine();
        if (!state.CanPlayScene()) { ImGui::BeginDisabled(); }
        if (ImGui::Button(ICON_CI_DEBUG_START "##SeqPlay")) { state.TriggerPlayAction(); }
        if (!state.CanPlayScene()) { ImGui::EndDisabled(); }
    }

    ImGui::Separator();

    // Advance playback time
    if (playing_)
    {
        timeline_seconds_ += ImGui::GetIO().DeltaTime;
        if (timeline_seconds_ > timeline_max_seconds_)
        {
            timeline_seconds_ = timeline_max_seconds_;
            playing_ = false;
        }
    }
    timeline_seconds_ = std::clamp(timeline_seconds_, 0.0f, timeline_max_seconds_);

    // ---- Viewport (16:9) ----
    const float viewport_width = ImGui::GetContentRegionAvail().x;
    const float viewport_height = std::max(60.0f, viewport_width * (9.0f / 16.0f));
    const ImVec2 viewport_size(viewport_width, viewport_height);

    ImGui::InvisibleButton("##SequencerViewport", viewport_size);
    const ImVec2 vp_min = ImGui::GetItemRectMin();
    const ImVec2 vp_max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    draw_list->AddRectFilledMultiColor(
        vp_min, vp_max,
        IM_COL32(18, 20, 26, 255),
        IM_COL32(26, 31, 40, 255),
        IM_COL32(12, 14, 18, 255),
        IM_COL32(18, 22, 28, 255));
    draw_list->AddRect(vp_min, vp_max, IM_COL32(84, 92, 105, 255), 8.0f, 0, 1.5f);

    const char* status_text = playing_ ? "Playing" : "Stopped";
    draw_list->AddText(ImVec2(vp_min.x + 14.0f, vp_min.y + 12.0f), IM_COL32(236, 240, 245, 255), status_text);

    const char* scene_label = selected_scene_index_ > 0 && selected_scene_index_ < static_cast<int>(scene_names_.size())
        ? scene_names_[selected_scene_index_].c_str()
        : "No scene selected";
    const ImVec2 scene_label_size = ImGui::CalcTextSize(scene_label);
    draw_list->AddText(
        ImVec2((vp_min.x + vp_max.x - scene_label_size.x) * 0.5f, (vp_min.y + vp_max.y - scene_label_size.y) * 0.5f),
        IM_COL32(100, 115, 130, 200),
        scene_label);

    ImGui::Spacing();

    // ---- Transport controls ----
    if (ImGui::Button(playing_ ? ICON_CI_DEBUG_PAUSE "##SeqTransport" : ICON_CI_DEBUG_START "##SeqTransport"))
    {
        playing_ = !playing_;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_CI_DEBUG_STOP "##SeqStop"))
    {
        playing_ = false;
        timeline_seconds_ = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_CI_DEBUG_RESTART "##SeqRestart"))
    {
        timeline_seconds_ = 0.0f;
        playing_ = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(64.0f);
    if (ImGui::InputFloat("##SequencerCurrentTime", &timeline_seconds_, 0.0f, 0.0f, "%.2fs"))
    {
        timeline_seconds_ = std::clamp(timeline_seconds_, 0.0f, timeline_max_seconds_);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("/");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(64.0f);
    if (ImGui::InputFloat("##SequencerLength", &timeline_max_seconds_, 0.0f, 0.0f, "%.1fs"))
    {
        timeline_max_seconds_ = std::max(0.1f, timeline_max_seconds_);
        timeline_seconds_ = std::min(timeline_seconds_, timeline_max_seconds_);
    }

    ImGui::Spacing();

    // ---- Timeline scrubber ----
    const ImVec2 timeline_origin = ImGui::GetCursorScreenPos();
    const float timeline_width = std::max(120.0f, ImGui::GetContentRegionAvail().x);
    const float track_height = 34.0f;
    ImGui::InvisibleButton("##SequencerTimelineScrub", ImVec2(timeline_width, track_height));

    if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const float mouse_x = ImGui::GetIO().MousePos.x;
        const float normalized = std::clamp((mouse_x - timeline_origin.x) / timeline_width, 0.0f, 1.0f);
        timeline_seconds_ = normalized * timeline_max_seconds_;
    }

    ImDrawList* tl = ImGui::GetWindowDrawList();
    const ImVec2 track_min = timeline_origin;
    const ImVec2 track_max(timeline_origin.x + timeline_width, timeline_origin.y + track_height);

    tl->AddRectFilled(track_min, track_max, IM_COL32(20, 24, 30, 255), 6.0f);
    tl->AddRect(track_min, track_max, IM_COL32(76, 86, 98, 255), 6.0f, 0, 1.0f);

    const int major_tick_count = std::max(1, static_cast<int>(std::ceil(timeline_max_seconds_)));
    for (int tick = 0; tick <= major_tick_count; ++tick)
    {
        const float normalized = static_cast<float>(tick) / static_cast<float>(major_tick_count);
        const float x = track_min.x + normalized * timeline_width;
        const float tick_h = (tick % 5 == 0) ? 18.0f : 10.0f;
        tl->AddLine(
            ImVec2(x, track_max.y - tick_h),
            ImVec2(x, track_max.y - 4.0f),
            IM_COL32(125, 138, 153, 255), 1.0f);


    }

    const float playhead_norm = timeline_max_seconds_ > 0.0f ? timeline_seconds_ / timeline_max_seconds_ : 0.0f;
    const float playhead_x = track_min.x + playhead_norm * timeline_width;
    tl->AddLine(
        ImVec2(playhead_x, track_min.y + 2.0f),
        ImVec2(playhead_x, track_max.y - 2.0f),
        IM_COL32(198, 232, 58, 255), 2.0f);
    tl->AddCircleFilled(ImVec2(playhead_x, track_min.y + 8.0f), 5.0f, IM_COL32(198, 232, 58, 255));

    ImGui::End();
}
