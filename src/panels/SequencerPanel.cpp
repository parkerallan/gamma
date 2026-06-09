#include "panels/SequencerPanel.h"

#include "app/VulkanContext.h"
#include "assets/ModelAsset.h"
#include "assets/SceneMetadata.h"
#include "imgui.h"
#include "render/RuntimeRenderer.h"
#include "ui/Codicons.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
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

SequencerPanel::SequencerPanel() = default;

SequencerPanel::~SequencerPanel()
{
    Shutdown();
}

void SequencerPanel::Shutdown()
{
    // Wait for any in-flight async parse so its worker thread doesn't touch a
    // half-destroyed panel, then tear down GPU resources while the device is
    // still alive.
    if (pending_load_.valid())
    {
        pending_load_.wait();
        pending_load_ = std::future<PendingLoadResult>{};
    }
    load_in_progress_ = false;

    if (runtime_)
    {
        if (vulkan_context_ != nullptr)
        {
            vulkan_context_->WaitIdle();
        }
        runtime_->Shutdown();
        runtime_.reset();
    }
    runtime_initialized_ = false;
    session_active_ = false;
    session_scene_path_.clear();
    requested_scene_path_.clear();
    playing_ = false;
}

bool SequencerPanel::EnsureRuntimeInitialized(VulkanContext* vulkan_context)
{
    if (runtime_initialized_)
    {
        return true;
    }
    if (vulkan_context == nullptr)
    {
        return false;
    }
    if (!runtime_)
    {
        runtime_ = std::make_unique<RuntimeRenderer>();
    }
    if (!runtime_->Initialize(vulkan_context))
    {
        session_error_ = "Failed to initialize sequencer runtime renderer";
        return false;
    }
    // The panel owns the preview's timeline (clock + cues); stop the runtime
    // from also auto-playing the scene's .seq, which would double-fire cues.
    runtime_->SetSequencerExternallyDriven(true);
    runtime_initialized_ = true;
    return true;
}

bool SequencerPanel::IsLoadingRequestedScene() const
{
    return !requested_scene_path_.empty() && requested_scene_path_ != session_scene_path_;
}

void SequencerPanel::RequestSceneLoad(EngineState& state, const std::filesystem::path& scene_path, bool autoplay)
{
    session_error_.clear();
    requested_scene_path_ = scene_path;
    requested_autoplay_ = autoplay;

    if (scene_path.empty())
    {
        return;
    }

    // Only one parse runs at a time. If one is in flight, PollSceneLoad() will
    // dispatch the latest request once it completes (and discard stale results).
    if (!load_in_progress_)
    {
        DispatchSceneLoad(state, scene_path);
    }
}

void SequencerPanel::DispatchSceneLoad(const EngineState& state, const std::filesystem::path& scene_path)
{
    load_in_progress_ = true;
    pending_load_ = std::async(std::launch::async,
        [scene_path, project_root = state.project_root]() -> PendingLoadResult
    {
        PendingLoadResult result;
        result.scene_path = scene_path;
        result.scene_metadata = LoadSceneMetadata(scene_path);
        if (!result.scene_metadata.parsed)
        {
            result.error = result.scene_metadata.error_message.empty()
                ? "Failed to parse scene"
                : result.scene_metadata.error_message;
            return result;
        }
        result.parsed = true;

        // Collect every referenced asset, keyed exactly as the runtime looks
        // them up so SeedModelAsset / SeedAudioClipBytes / SeedVideoBytes hit.
        std::unordered_set<std::filesystem::path> unique_model_paths;
        std::unordered_set<std::string> unique_audio_paths;
        std::unordered_set<std::string> unique_video_paths;
        for (const SceneObjectMetadata& object : result.scene_metadata.objects)
        {
            if (!object.enabled_in_hierarchy)
            {
                continue;
            }
            if (!object.model_path.empty())
            {
                unique_model_paths.insert((project_root / object.model_path).lexically_normal());
            }
            for (const SceneObjectAttribute& attr : object.attributes)
            {
                if (!attr.audio.clip_path.empty())
                {
                    unique_audio_paths.insert(attr.audio.clip_path);
                }
                if (!attr.video_2d.video_path.empty())
                {
                    unique_video_paths.insert(attr.video_2d.video_path);
                }
            }
        }

        // Fan out model parses (CPU-bound Assimp) across worker threads.
        struct ParsedModel
        {
            std::filesystem::path path;
            std::filesystem::file_time_type write_time{};
            ModelAsset asset;
        };
        std::vector<std::future<ParsedModel>> jobs;
        jobs.reserve(unique_model_paths.size());
        for (const std::filesystem::path& model_path : unique_model_paths)
        {
            jobs.push_back(std::async(std::launch::async, [model_path]() -> ParsedModel
            {
                ParsedModel parsed;
                parsed.path = model_path;
                std::error_code error;
                const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(model_path, error);
                parsed.write_time = error ? std::filesystem::file_time_type::min() : write_time;
                parsed.asset = LoadModelAsset(model_path);
                return parsed;
            }));
        }
        result.models.reserve(jobs.size());
        for (auto& job : jobs)
        {
            ParsedModel parsed = job.get();
            if (parsed.asset.loaded)
            {
                result.models.push_back({std::move(parsed.path), parsed.write_time, std::move(parsed.asset)});
            }
        }

        auto read_bytes = [](const std::filesystem::path& abs_path) -> std::vector<std::uint8_t>
        {
            std::ifstream file(abs_path, std::ios::binary);
            if (!file)
            {
                return {};
            }
            return std::vector<std::uint8_t>(
                (std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
        };

        for (const std::string& clip_path : unique_audio_paths)
        {
            const std::filesystem::path stored(clip_path);
            const std::filesystem::path abs_path = stored.is_absolute() ? stored : (project_root / stored);
            // Cache key must match what RuntimeRenderer passes to PlaySound
            // (absolute on-disk path).
            std::vector<std::uint8_t> bytes = read_bytes(abs_path);
            if (!bytes.empty())
            {
                result.audio_clip_bytes.emplace_back(abs_path.generic_string(), std::move(bytes));
            }
        }

        for (const std::string& video_path : unique_video_paths)
        {
            const std::filesystem::path stored(video_path);
            const std::filesystem::path abs_path = stored.is_absolute() ? stored : (project_root / stored);
            // VideoPlaybackManager keys its preload cache by the raw
            // scene-relative video_path, so preserve that here.
            std::vector<std::uint8_t> bytes = read_bytes(abs_path);
            if (!bytes.empty())
            {
                result.video_bytes.emplace_back(video_path, std::move(bytes));
            }
        }

        return result;
    });
}

void SequencerPanel::PollSceneLoad(EngineState& state, VulkanContext* vulkan_context)
{
    if (!load_in_progress_ || !pending_load_.valid())
    {
        return;
    }
    if (pending_load_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    {
        return;
    }

    PendingLoadResult result = pending_load_.get();
    pending_load_ = std::future<PendingLoadResult>{};
    load_in_progress_ = false;

    // The user may have switched scenes (or stopped) while this parse ran.
    // Discard the stale result and dispatch the latest request instead.
    if (result.scene_path != requested_scene_path_)
    {
        if (!requested_scene_path_.empty())
        {
            DispatchSceneLoad(state, requested_scene_path_);
        }
        return;
    }

    if (!result.parsed)
    {
        session_error_ = result.error.empty() ? "Failed to parse scene" : result.error;
        requested_scene_path_.clear();
        return;
    }

    const ActiveSceneCameraSelection active_camera = FindActiveSceneCamera(result.scene_metadata);
    if (!active_camera.found)
    {
        session_error_ = "Scene needs one active camera to sequence";
        requested_scene_path_.clear();
        return;
    }

    if (!EnsureRuntimeInitialized(vulkan_context))
    {
        requested_scene_path_.clear();
        return;
    }

    // Stop audio from any previous session. No vkDeviceWaitIdle is needed:
    // StartSession touches no GPU resources, and the persistent mesh cache that
    // any in-flight runtime frame references is left intact; the next
    // RenderFrame drains its own fence before reusing the command buffer.
    if (session_active_ && runtime_)
    {
        runtime_->GetAudioEngine().StopAll();
    }

    std::string err;
    if (!runtime_->StartSession(state.project_root, result.scene_path, active_camera, &err))
    {
        session_active_ = false;
        session_scene_path_.clear();
        requested_scene_path_.clear();
        session_error_ = err.empty() ? "Failed to start runtime session" : err;
        return;
    }

    // Hand the freshly-parsed scene + assets to the runtime so the first
    // RenderFrame does no Assimp / scene-text parsing on the main thread.
    runtime_->SeedSceneMetadata(result.scene_path, result.scene_metadata);
    for (auto& model : result.models)
    {
        runtime_->SeedModelAsset(model.absolute_path, model.write_time, std::move(model.asset));
    }
    for (auto& clip : result.audio_clip_bytes)
    {
        runtime_->SeedAudioClipBytes(clip.first, std::move(clip.second));
    }
    for (auto& video : result.video_bytes)
    {
        runtime_->SeedVideoBytes(video.first, std::move(video.second));
    }

    session_active_ = true;
    session_scene_path_ = result.scene_path;
    session_scene_metadata_ = std::move(result.scene_metadata);
    timeline_seconds_ = 0.0f;
    last_marker_time_ = 0.0f;
    reset_play_timing_ = true;
    ResetClipFiring();
    RegisterClipsWithRuntime();
    playing_ = requested_autoplay_;
    force_render_frame_ = true;
    last_viewport_width_ = 0;
    last_viewport_height_ = 0;
    session_error_.clear();
    state.AddLog("Sequencer loaded scene: " + state.GetDisplayPath(result.scene_path));
}

void SequencerPanel::StopSession()
{
    playing_ = false;
    timeline_seconds_ = 0.0f;
    force_render_frame_ = false;
    // Cancel any pending load request so a completing parse won't start a
    // session after the user pressed Stop.
    requested_scene_path_.clear();

    if (!session_active_)
    {
        return;
    }

    // No vkDeviceWaitIdle: StopSession frees no GPU resources (the runtime stays
    // initialized for a fast restart), so there is nothing in flight to guard.
    if (runtime_)
    {
        runtime_->GetAudioEngine().StopAll();
    }
    session_active_ = false;
    session_scene_path_.clear();
    session_scene_metadata_ = SceneMetadata{};
}

void SequencerPanel::RestartSession(EngineState& state)
{
    // Fast path: a live session restarts in place from the metadata we already
    // parsed — no async re-load. The runtime keeps its model/mesh caches across
    // StartSession, so this just resets scripts/animation/physics and replays
    // from t=0.
    if (session_active_ && runtime_ && session_scene_metadata_.parsed)
    {
        const ActiveSceneCameraSelection active_camera = FindActiveSceneCamera(session_scene_metadata_);
        if (!active_camera.found)
        {
            session_error_ = "Scene needs one active camera to sequence";
            return;
        }
        runtime_->GetAudioEngine().StopAll();
        std::string err;
        if (!runtime_->StartSession(state.project_root, session_scene_path_, active_camera, &err))
        {
            session_error_ = err.empty() ? "Failed to restart runtime session" : err;
            return;
        }
        runtime_->SeedSceneMetadata(session_scene_path_, session_scene_metadata_);
        timeline_seconds_ = 0.0f;
        last_marker_time_ = 0.0f;
        reset_play_timing_ = true;
        ResetClipFiring();
        RegisterClipsWithRuntime();
        playing_ = true;
        force_render_frame_ = true;
        session_error_.clear();
        return;
    }

    // No live session yet — (re)load the selected scene from scratch, autoplay.
    const std::filesystem::path scene_path =
        selected_scene_index_ > 0 && selected_scene_index_ < static_cast<int>(scene_paths_.size())
            ? scene_paths_[selected_scene_index_]
            : std::filesystem::path();
    if (!scene_path.empty())
    {
        RequestSceneLoad(state, scene_path, /*autoplay=*/true);
    }
}

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

namespace
{
bool IsSequencerAsset(const std::filesystem::path& path, bool& out_is_graph)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".graph") { out_is_graph = true;  return true; }
    if (ext == ".lua")   { out_is_graph = false; return true; }
    return false;
}
} // namespace

std::filesystem::path SequencerPanel::SequenceFilePathForScene(const EngineState& state, const std::filesystem::path& scene_path) const
{
    if (state.project_root.empty() || scene_path.empty())
    {
        return {};
    }
    return state.project_root / "Sequences" / (scene_path.stem().string() + ".seq");
}

void SequencerPanel::LoadSequenceForScene(const EngineState& state, const std::filesystem::path& scene_path)
{
    loaded_sequence_scene_path_ = scene_path;
    clips_.clear();
    selected_clip_id_.clear();
    dragging_clip_ = false;
    sequence_dirty_ = false;
    track_count_ = 1;

    const std::filesystem::path seq_path = SequenceFilePathForScene(state, scene_path);
    if (seq_path.empty() || !std::filesystem::exists(seq_path))
    {
        return;
    }

    std::ifstream input(seq_path);
    if (!input)
    {
        return;
    }

    std::string line;
    while (std::getline(input, line))
    {
        std::istringstream stream(line);
        std::string keyword;
        stream >> keyword;
        if (keyword == "tracks")
        {
            int count = 2;
            stream >> count;
            track_count_ = std::clamp(count, 1, 16);
        }
        else if (keyword == "clip")
        {
            int track = 0;
            float start = 0.0f;
            int is_graph = 0;
            stream >> track >> start >> is_graph;
            stream >> std::ws;
            std::string rel_path;
            std::getline(stream, rel_path);
            while (!rel_path.empty() && (rel_path.back() == '\r' || rel_path.back() == '\n' ||
                                         rel_path.back() == ' ' || rel_path.back() == '\t'))
            {
                rel_path.pop_back();
            }
            if (rel_path.empty())
            {
                continue;
            }

            SequencerClip clip;
            clip.id = "seqclip_" + std::to_string(next_clip_serial_++);
            clip.asset_path = (state.project_root / rel_path).lexically_normal();
            clip.label = clip.asset_path.stem().string();
            clip.start_time = std::max(0.0f, start);
            clip.track = std::clamp(track, 0, track_count_ - 1);
            clip.is_graph = is_graph != 0;
            clips_.push_back(std::move(clip));
        }
    }
}

bool SequencerPanel::SaveSequence(EngineState& state)
{
    const std::filesystem::path seq_path = SequenceFilePathForScene(state, loaded_sequence_scene_path_);
    if (seq_path.empty())
    {
        state.AddLog("Sequencer: cannot save — no scene selected");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(seq_path.parent_path(), ec);

    // Binary mode keeps line endings LF-only so the runtime (which reads the
    // file / pak bytes raw) doesn't get a trailing '\r' on each clip path.
    std::ofstream output(seq_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        state.AddLog("Sequencer: failed to write " + seq_path.generic_string());
        return false;
    }

    output << "tracks " << track_count_ << "\n";
    for (const SequencerClip& clip : clips_)
    {
        std::error_code rel_ec;
        std::filesystem::path rel = std::filesystem::relative(clip.asset_path, state.project_root, rel_ec);
        const std::string rel_str = (rel_ec || rel.empty()) ? clip.asset_path.generic_string() : rel.generic_string();
        output << "clip " << clip.track << " " << clip.start_time << " " << (clip.is_graph ? 1 : 0) << " " << rel_str << "\n";
    }

    sequence_dirty_ = false;
    state.AddLog("Sequencer: saved " + seq_path.generic_string());
    return true;
}

void SequencerPanel::AddClip(const EngineState& /*state*/, const std::filesystem::path& asset_abs_path, int track, float start_time)
{
    bool is_graph = false;
    if (!IsSequencerAsset(asset_abs_path, is_graph))
    {
        return;
    }

    SequencerClip clip;
    clip.id = "seqclip_" + std::to_string(next_clip_serial_++);
    clip.asset_path = asset_abs_path.lexically_normal();
    clip.label = clip.asset_path.stem().string();
    clip.start_time = std::clamp(start_time, 0.0f, timeline_max_seconds_);
    clip.track = std::clamp(track, 0, track_count_ - 1);
    clip.is_graph = is_graph;
    selected_clip_id_ = clip.id;
    // Load it into the live session now (if any) so its OnStart runs like a
    // normal script; its OnCue fires when the playhead later reaches it.
    if (session_active_ && runtime_)
    {
        runtime_->RegisterSequencerClip(clip.id, clip.asset_path);
    }
    clips_.push_back(std::move(clip));
    sequence_dirty_ = true;
}

void SequencerPanel::FireCrossedClips(float from_time, float to_time)
{
    if (!session_active_ || !runtime_)
    {
        return;
    }
    for (SequencerClip& clip : clips_)
    {
        if (!clip.fired && clip.track < track_count_ &&
            clip.start_time >= from_time && clip.start_time <= to_time)
        {
            runtime_->FireSequencerCue(clip.id);
            clip.fired = true;
        }
    }
}

void SequencerPanel::ResetClipFiring()
{
    for (SequencerClip& clip : clips_)
    {
        clip.fired = false;
    }
}

void SequencerPanel::RegisterClipsWithRuntime()
{
    if (!session_active_ || !runtime_)
    {
        return;
    }
    for (const SequencerClip& clip : clips_)
    {
        runtime_->RegisterSequencerClip(clip.id, clip.asset_path);
    }
}

void SequencerPanel::Render(EngineState& state, VulkanContext* vulkan_context)
{
    if (!state.show_sequencer_panel)
    {
        return;
    }

    vulkan_context_ = vulkan_context;

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

    // Consume any finished async scene parse (may start a live session).
    PollSceneLoad(state, vulkan_context);

    // A loaded session whose scene was deleted, or whose project was closed,
    // can no longer be sequenced — drop it.
    if (session_active_ && (!state.HasOpenProject() || !std::filesystem::exists(session_scene_path_)))
    {
        StopSession();
    }

    // React to the user picking a different scene in the dropdown. Comparing
    // against the loaded scene path (rather than the raw index) avoids spurious
    // reloads when a directory rescan remaps indices.
    const std::filesystem::path selected_path =
        selected_scene_index_ > 0 && selected_scene_index_ < static_cast<int>(scene_paths_.size())
            ? scene_paths_[selected_scene_index_]
            : std::filesystem::path();

    // Keep the timeline in sync with the selected scene (independent of whether
    // a live session is running).
    if (selected_path != loaded_sequence_scene_path_)
    {
        LoadSequenceForScene(state, selected_path);
    }

    if (selected_scene_index_ != prev_selected_scene_index_)
    {
        prev_selected_scene_index_ = selected_scene_index_;
        if (selected_path.empty())
        {
            StopSession();
        }
        else if (selected_path != session_scene_path_)
        {
            RequestSceneLoad(state, selected_path, /*autoplay=*/false);
        }
    }

    std::vector<const char*> name_ptrs;
    name_ptrs.reserve(scene_names_.size());
    for (const auto& n : scene_names_) { name_ptrs.push_back(n.c_str()); }

    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo("##SequencerSceneSelector", &selected_scene_index_, name_ptrs.data(), static_cast<int>(name_ptrs.size())))
    {
        prev_selected_scene_index_ = selected_scene_index_;
        if (selected_scene_index_ <= 0)
        {
            StopSession();
        }
        else
        {
            const std::filesystem::path picked = scene_paths_[selected_scene_index_];
            if (picked != session_scene_path_)
            {
                RequestSceneLoad(state, picked, /*autoplay=*/false);
            }
        }
    }

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
        ImGui::BeginDisabled(selected_scene_index_ <= 0);
        if (ImGui::Button(ICON_CI_SAVE "##SeqSave")) { SaveSequence(state); }
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
        float marker_dt = ImGui::GetIO().DeltaTime;
        if (reset_play_timing_)
        {
            // The frame right after a (re)start carries the restart stall in its
            // DeltaTime; don't let the marker (or cue firing) jump by it.
            marker_dt = 0.0f;
            reset_play_timing_ = false;
        }
        marker_dt = std::min(marker_dt, 0.1f); // bound any single-frame stall
        timeline_seconds_ += marker_dt;
        if (timeline_seconds_ > timeline_max_seconds_)
        {
            timeline_seconds_ = timeline_max_seconds_;
            playing_ = false;
        }
    }
    timeline_seconds_ = std::clamp(timeline_seconds_, 0.0f, timeline_max_seconds_);

    // Fire timeline clips as the playhead advances. A backward jump (scrub or
    // refresh) rewinds: clear fired-state and drop the runtime instances so the
    // clips re-fire on the next forward pass.
    if (timeline_seconds_ < last_marker_time_ - 1e-4f)
    {
        ResetClipFiring();
        if (session_active_ && runtime_)
        {
            runtime_->ClearSequencerInstances();
        }
    }
    else if (playing_ && session_active_)
    {
        FireCrossedClips(last_marker_time_, timeline_seconds_);
    }
    last_marker_time_ = timeline_seconds_;

    // ---- Viewport (16:9) ----
    const float viewport_width = ImGui::GetContentRegionAvail().x;
    const float viewport_height = std::max(60.0f, viewport_width * (9.0f / 16.0f));
    const ImVec2 viewport_size(viewport_width, viewport_height);

    ImGui::InvisibleButton("##SequencerViewport", viewport_size);
    const ImVec2 vp_min = ImGui::GetItemRectMin();
    const ImVec2 vp_max = ImGui::GetItemRectMax();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    bool drew_runtime = false;
    if (session_active_ && runtime_)
    {
        const std::uint32_t target_width = static_cast<std::uint32_t>(std::max(16.0f, viewport_width));
        const std::uint32_t target_height = static_cast<std::uint32_t>(std::max(16.0f, viewport_height));
        if (target_width != last_viewport_width_ || target_height != last_viewport_height_)
        {
            last_viewport_width_ = target_width;
            last_viewport_height_ = target_height;
            force_render_frame_ = true; // re-render so a paused session matches the new size
        }

        if (playing_ || force_render_frame_)
        {
            // Mirror the application's TAA debug-knob plumbing so SettingsPanel
            // sliders affect the sequencer preview just like the Play window.
            runtime_->SetTAAEnabled(state.taa_enabled);
            RayTracing::TaaDebugSettings taa_dbg{};
            taa_dbg.viz_mode              = state.taa_viz_mode;
            taa_dbg.variance_scale        = state.taa_variance_scale;
            taa_dbg.variance_scale_moving = state.taa_variance_scale_moving;
            taa_dbg.anti_sparkle          = state.taa_anti_sparkle;
            taa_dbg.history_blend         = state.taa_history_blend;
            taa_dbg.jitter_compensation   = state.taa_jitter_compensation;
            taa_dbg.adaptive_enabled      = state.taa_adaptive_enabled;
            taa_dbg.adaptive_max_samples  = state.taa_adaptive_max_samples;
            taa_dbg.adaptive_threshold    = state.taa_adaptive_threshold;
            taa_dbg.adaptive_preservation = state.taa_adaptive_preservation;
            taa_dbg.dynamic_shadow_samples = state.rt_dynamic_shadow_samples;
            runtime_->SetTaaDebugSettings(taa_dbg);

            std::string render_error;
            if (runtime_->RenderFrame(target_width, target_height, &render_error))
            {
                force_render_frame_ = false;
            }
            else
            {
                session_error_ = render_error.empty() ? "Runtime frame render failed" : render_error;
                StopSession();
            }
        }

        if (session_active_)
        {
            if (VkDescriptorSet output = runtime_->GetOutputDescriptorSet())
            {
                // The RT output is rendered bottom-up; flip V so it displays
                // upright (matches the editor viewport / camera preview).
                draw_list->AddImage(
                    reinterpret_cast<ImTextureID>(output),
                    vp_min, vp_max,
                    ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
                draw_list->AddRect(vp_min, vp_max, IM_COL32(84, 92, 105, 255), 8.0f, 0, 1.5f);
                drew_runtime = true;
            }
        }
    }

    if (!drew_runtime)
    {
        draw_list->AddRectFilledMultiColor(
            vp_min, vp_max,
            IM_COL32(18, 20, 26, 255),
            IM_COL32(26, 31, 40, 255),
            IM_COL32(12, 14, 18, 255),
            IM_COL32(18, 22, 28, 255));
        draw_list->AddRect(vp_min, vp_max, IM_COL32(84, 92, 105, 255), 8.0f, 0, 1.5f);

        const bool loading = IsLoadingRequestedScene();
        const char* center_label = nullptr;
        if (loading)
        {
            center_label = "Loading...";
        }
        else if (!session_error_.empty())
        {
            center_label = session_error_.c_str();
        }
        else if (selected_scene_index_ > 0 && selected_scene_index_ < static_cast<int>(scene_names_.size()))
        {
            center_label = scene_names_[selected_scene_index_].c_str();
        }
        else
        {
            center_label = "No scene selected";
        }
        const ImVec2 center_size = ImGui::CalcTextSize(center_label);
        ImU32 center_color = IM_COL32(100, 115, 130, 200);
        if (loading)
        {
            center_color = IM_COL32(235, 238, 242, 255);
        }
        else if (!session_error_.empty())
        {
            center_color = IM_COL32(220, 120, 110, 230);
        }
        draw_list->AddText(
            ImVec2((vp_min.x + vp_max.x - center_size.x) * 0.5f, (vp_min.y + vp_max.y - center_size.y) * 0.5f),
            center_color,
            center_label);
    }

    // Status text (top-left overlay).
    const char* status_text = "Stopped";
    if (session_active_)
    {
        status_text = playing_ ? "Playing" : "Paused";
    }
    else if (IsLoadingRequestedScene())
    {
        status_text = "Loading";
    }
    draw_list->AddText(ImVec2(vp_min.x + 14.0f, vp_min.y + 12.0f), IM_COL32(236, 240, 245, 255), status_text);

    ImGui::Spacing();

    // ---- Transport controls ----
    const bool has_scene_selection = selected_scene_index_ > 0;

    // Play: start the scene (loading first if needed) or resume from pause.
    ImGui::BeginDisabled(!has_scene_selection || playing_);
    if (ImGui::Button(ICON_CI_DEBUG_START "##SeqPlayBtn"))
    {
        if (session_active_)
        {
            if (timeline_seconds_ >= timeline_max_seconds_)
            {
                timeline_seconds_ = 0.0f;
            }
            playing_ = true;
            force_render_frame_ = true;
            reset_play_timing_ = true;
        }
        else if (!selected_path.empty())
        {
            // No scene loaded yet — kick off an async load that auto-plays once
            // the parse finishes (PollSceneLoad starts the session).
            RequestSceneLoad(state, selected_path, /*autoplay=*/true);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    // Pause: hold playback on the current frame.
    ImGui::BeginDisabled(!playing_);
    if (ImGui::Button(ICON_CI_DEBUG_PAUSE "##SeqPause"))
    {
        playing_ = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    // Refresh: restart the scene from t=0.
    ImGui::BeginDisabled(!has_scene_selection);
    if (ImGui::Button(ICON_CI_DEBUG_RESTART "##SeqRestart"))
    {
        RestartSession(state);
    }
    ImGui::EndDisabled();
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

    // ---- Timeline tracks (drag scripts/graphs here) ----
    const float timeline_width = std::max(120.0f, ImGui::GetContentRegionAvail().x);
    ImGui::TextDisabled("Tracks");
    ImGui::SameLine();
    if (ImGui::SmallButton("+##SeqAddTrack"))
    {
        track_count_ = std::min(track_count_ + 1, 16);
        sequence_dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(track_count_ <= 1);
    if (ImGui::SmallButton("-##SeqRemoveTrack"))
    {
        track_count_ = std::max(1, track_count_ - 1);
        const int removed_track = track_count_;
        clips_.erase(std::remove_if(clips_.begin(), clips_.end(),
            [removed_track](const SequencerClip& c) { return c.track >= removed_track; }), clips_.end());
        sequence_dirty_ = true;
    }
    ImGui::EndDisabled();
    if (sequence_dirty_)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(unsaved)");
    }

    RenderTimelineTracks(state, ImGui::GetCursorScreenPos(), timeline_width);

    ImGui::End();
}

void SequencerPanel::RenderTimelineTracks(EngineState& state, const ImVec2& origin, float width)
{
    const float lane_height = 26.0f;
    const float lane_gap = 4.0f;
    const float lane_stride = lane_height + lane_gap;
    const float tracks_height = static_cast<float>(track_count_) * lane_stride;
    const float max_seconds = std::max(0.1f, timeline_max_seconds_);

    ImGui::InvisibleButton("##SeqTracksArea", ImVec2(width, tracks_height));
    const bool area_hovered = ImGui::IsItemHovered();
    const bool area_active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    const auto time_to_x = [&](float seconds) -> float
    {
        return origin.x + std::clamp(seconds / max_seconds, 0.0f, 1.0f) * width;
    };
    const auto x_to_time = [&](float x) -> float
    {
        return std::clamp((x - origin.x) / width, 0.0f, 1.0f) * max_seconds;
    };

    // Accept scripts/graphs dropped from the file tree. Handled immediately
    // after the InvisibleButton item so BeginDragDropTarget() binds to it.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_TREE_PATH"))
        {
            const char* payload_text = static_cast<const char*>(payload->Data);
            const std::size_t payload_size = payload->DataSize > 0 ? static_cast<std::size_t>(payload->DataSize - 1) : 0;
            const std::filesystem::path dropped(std::string(payload_text, payload_size));
            bool is_graph = false;
            if (IsSequencerAsset(dropped, is_graph))
            {
                const int drop_track = std::clamp(static_cast<int>((mouse.y - origin.y) / lane_stride), 0, track_count_ - 1);
                AddClip(state, dropped, drop_track, x_to_time(mouse.x));
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Lane backgrounds.
    for (int t = 0; t < track_count_; ++t)
    {
        const ImVec2 lane_min(origin.x, origin.y + static_cast<float>(t) * lane_stride);
        const ImVec2 lane_max(origin.x + width, lane_min.y + lane_height);
        dl->AddRectFilled(lane_min, lane_max, IM_COL32(22, 26, 32, 255), 4.0f);
        dl->AddRect(lane_min, lane_max, IM_COL32(54, 61, 72, 255), 4.0f, 0, 1.0f);
    }

    // Clips. Track hit rectangles for interaction in the same pass.
    int hovered_clip = -1;
    for (std::size_t i = 0; i < clips_.size(); ++i)
    {
        const SequencerClip& clip = clips_[i];
        if (clip.track < 0 || clip.track >= track_count_)
        {
            continue;
        }
        const float cx = time_to_x(clip.start_time);
        const float label_w = ImGui::CalcTextSize(clip.label.c_str()).x;
        const float clip_w = std::clamp(label_w + 16.0f, 28.0f, 170.0f);
        const float top = origin.y + static_cast<float>(clip.track) * lane_stride + 2.0f;
        const ImVec2 a(cx, top);
        const ImVec2 b(cx + clip_w, top + lane_height - 4.0f);

        const bool selected = clip.id == selected_clip_id_;
        const bool is_hit = area_hovered && mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y;
        if (is_hit)
        {
            hovered_clip = static_cast<int>(i);
        }

        ImU32 fill = clip.is_graph ? IM_COL32(70, 96, 140, 255) : IM_COL32(78, 120, 86, 255);
        if (clip.fired)
        {
            fill = clip.is_graph ? IM_COL32(96, 124, 170, 255) : IM_COL32(104, 150, 112, 255);
        }
        dl->AddRectFilled(a, b, fill, 4.0f);
        dl->AddRect(a, b, selected ? IM_COL32(232, 236, 240, 255) : IM_COL32(20, 24, 30, 200), 4.0f, 0, selected ? 2.0f : 1.0f);
        // Start marker line.
        dl->AddLine(ImVec2(cx, top - 2.0f), ImVec2(cx, top + lane_height - 2.0f), IM_COL32(232, 236, 240, 220), 1.5f);
        dl->PushClipRect(a, b, true);
        dl->AddText(ImVec2(a.x + 6.0f, a.y + 3.0f), IM_COL32(236, 240, 245, 255), clip.label.c_str());
        dl->PopClipRect();
    }

    // ---- Interaction ----
    // Begin a drag / selection on left-press.
    if (area_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        if (hovered_clip >= 0)
        {
            selected_clip_id_ = clips_[hovered_clip].id;
            dragging_clip_ = true;
            drag_grab_offset_x_ = mouse.x - time_to_x(clips_[hovered_clip].start_time);
        }
        else
        {
            selected_clip_id_.clear();
        }
    }

    // Continue dragging the selected clip.
    if (dragging_clip_ && area_active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && !selected_clip_id_.empty())
    {
        for (SequencerClip& clip : clips_)
        {
            if (clip.id != selected_clip_id_)
            {
                continue;
            }
            clip.start_time = x_to_time(mouse.x - drag_grab_offset_x_);
            const int new_track = static_cast<int>((mouse.y - origin.y) / lane_stride);
            clip.track = std::clamp(new_track, 0, track_count_ - 1);
            sequence_dirty_ = true;
            break;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        dragging_clip_ = false;
    }

    // Right-click a clip to open its context menu.
    if (area_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hovered_clip >= 0)
    {
        selected_clip_id_ = clips_[hovered_clip].id;
        ImGui::OpenPopup("##SeqClipContext");
    }
    if (ImGui::BeginPopup("##SeqClipContext"))
    {
        if (ImGui::MenuItem("Delete clip"))
        {
            clips_.erase(std::remove_if(clips_.begin(), clips_.end(),
                [this](const SequencerClip& c) { return c.id == selected_clip_id_; }), clips_.end());
            selected_clip_id_.clear();
            sequence_dirty_ = true;
        }
        ImGui::EndPopup();
    }

    // Delete key removes the selected clip while the panel is focused.
    if (!selected_clip_id_.empty() && ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete))
    {
        clips_.erase(std::remove_if(clips_.begin(), clips_.end(),
            [this](const SequencerClip& c) { return c.id == selected_clip_id_; }), clips_.end());
        selected_clip_id_.clear();
        sequence_dirty_ = true;
    }

    // Playhead across the tracks.
    const float playhead_x = time_to_x(timeline_seconds_);
    dl->AddLine(ImVec2(playhead_x, origin.y), ImVec2(playhead_x, origin.y + tracks_height), IM_COL32(198, 232, 58, 220), 1.5f);

    // Hint when empty.
    if (clips_.empty())
    {
        const char* hint = "Drag .lua / .graph here";
        const ImVec2 hint_size = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(origin.x + (width - hint_size.x) * 0.5f, origin.y + (tracks_height - hint_size.y) * 0.5f),
            IM_COL32(110, 122, 136, 180), hint);
    }
}
