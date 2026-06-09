#pragma once

#include "assets/ModelAsset.h"
#include "assets/SceneMetadata.h"
#include "state/EngineState.h"

#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class VulkanContext;
class RuntimeRenderer;

class SequencerPanel
{
public:
    SequencerPanel();
    ~SequencerPanel();

    SequencerPanel(const SequencerPanel&) = delete;
    SequencerPanel& operator=(const SequencerPanel&) = delete;

    void Render(EngineState& state, VulkanContext* vulkan_context);
    // Tears down the embedded runtime session and its GPU resources. Must be
    // called by the application while the Vulkan device is still alive (before
    // VulkanContext::Shutdown), mirroring the other panels' Shutdown hooks.
    void Shutdown();

private:
    void RefreshSceneList(const std::filesystem::path& scenes_dir);

    // A script (.lua) or graph (.graph) placed on the timeline. Fires once when
    // the playhead crosses start_time during playback (Phase 2 semantics).
    struct SequencerClip
    {
        std::string id;                   // unique within the sequence; runtime instance key
        std::filesystem::path asset_path; // absolute path on disk (for runtime load)
        std::string label;                // display name (file stem)
        float start_time = 0.0f;          // seconds along the timeline
        int track = 0;                    // track lane index
        bool is_graph = false;            // .graph vs .lua
        bool fired = false;               // already triggered this play-through
    };

    std::filesystem::path SequenceFilePathForScene(const EngineState& state, const std::filesystem::path& scene_path) const;
    void LoadSequenceForScene(const EngineState& state, const std::filesystem::path& scene_path);
    bool SaveSequence(EngineState& state);
    void AddClip(const EngineState& state, const std::filesystem::path& asset_abs_path, int track, float start_time);
    // Fire every not-yet-fired clip whose start lies in (from_time, to_time].
    void FireCrossedClips(float from_time, float to_time);
    void ResetClipFiring();
    // Registers every clip with the live runtime so each clip's script/graph
    // loads (its normal OnStart/OnUpdate run); OnCue fires later at its cue.
    void RegisterClipsWithRuntime();
    void RenderTimelineTracks(EngineState& state, const ImVec2& origin, float width);

    // Result of an off-thread parse of a scene and all of its referenced
    // assets, mirroring WorkspacePanel's async viewport preload so selecting a
    // scene never blocks the editor's main thread on Assimp / disk reads.
    struct PendingLoadResult
    {
        std::filesystem::path scene_path;
        bool parsed = false;
        std::string error;
        SceneMetadata scene_metadata;
        struct ModelEntry
        {
            std::filesystem::path absolute_path;
            std::filesystem::file_time_type write_time{};
            ModelAsset asset;
        };
        std::vector<ModelEntry> models;
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> audio_clip_bytes;
        std::vector<std::pair<std::string, std::vector<std::uint8_t>>> video_bytes;
    };

    // Embedded runtime session lifecycle. The Sequencer hosts its own
    // RuntimeRenderer (independent of the application's separate Play window)
    // so the selected scene can render live inside the panel viewport.
    bool EnsureRuntimeInitialized(VulkanContext* vulkan_context);
    // Requests an async load of scene_path (no-op for an empty path). When the
    // parse finishes, PollSceneLoad() starts the runtime session. autoplay
    // selects whether playback begins immediately once loaded.
    void RequestSceneLoad(EngineState& state, const std::filesystem::path& scene_path, bool autoplay);
    void DispatchSceneLoad(const EngineState& state, const std::filesystem::path& scene_path);
    void PollSceneLoad(EngineState& state, VulkanContext* vulkan_context);
    void StopSession();
    void RestartSession(EngineState& state);
    // True while the user has asked for a scene that has not finished loading
    // into a live session yet (drives the "Loading..." placeholder).
    bool IsLoadingRequestedScene() const;

    // Scene selector
    int selected_scene_index_ = 0;
    int prev_selected_scene_index_ = 0;
    std::vector<std::string> scene_names_;
    std::vector<std::filesystem::path> scene_paths_;
    std::filesystem::path last_scanned_dir_;
    std::uint64_t last_dir_signature_ = 0;

    // Transport
    bool playing_ = false;
    float timeline_seconds_ = 0.0f;
    float timeline_max_seconds_ = 10.0f;
    // Marker time at the previous frame; drives forward-crossing clip firing and
    // detects backward scrubs (which rewind the sequencer).
    float last_marker_time_ = 0.0f;
    // Set when playback (re)starts so the first frame — whose DeltaTime absorbs
    // the synchronous restart hitch — doesn't jump the marker forward.
    bool reset_play_timing_ = false;

    // Timeline tracks / clips.
    std::vector<SequencerClip> clips_;
    int track_count_ = 1;
    std::filesystem::path loaded_sequence_scene_path_; // scene whose sequence is in clips_
    bool sequence_dirty_ = false;
    std::uint64_t next_clip_serial_ = 1;
    std::string selected_clip_id_;
    // Active clip-drag state (moving a clip along the timeline / between tracks).
    bool dragging_clip_ = false;
    float drag_grab_offset_x_ = 0.0f;

    // Embedded runtime renderer (created lazily on first scene load).
    std::unique_ptr<RuntimeRenderer> runtime_;
    VulkanContext* vulkan_context_ = nullptr;
    bool runtime_initialized_ = false;
    bool session_active_ = false;
    std::filesystem::path session_scene_path_;
    // Metadata of the live session's scene, kept so Refresh can restart in
    // place (re-running StartSession) without a fresh async parse.
    SceneMetadata session_scene_metadata_;
    std::string session_error_;

    // Async scene-load state. requested_scene_path_ is the scene the user
    // currently wants live; a single parse runs at a time and stale results
    // (whose scene no longer matches the request) are discarded.
    std::future<PendingLoadResult> pending_load_;
    bool load_in_progress_ = false;
    std::filesystem::path requested_scene_path_;
    bool requested_autoplay_ = false;

    // Set when the embedded viewport must render at least one frame even while
    // paused: on session start/refresh or viewport resize. Lets a paused
    // session freeze on its last frame yet still reflect a new size.
    bool force_render_frame_ = false;
    std::uint32_t last_viewport_width_ = 0;
    std::uint32_t last_viewport_height_ = 0;
};
