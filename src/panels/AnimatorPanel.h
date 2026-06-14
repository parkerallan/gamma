#pragma once

#include "app/VulkanContext.h"
#include "assets/AnimatorControllerAsset.h"
#include "assets/FaceClipAsset.h"
#include "panels/AnimatorPreviewRenderer.h"
#include "state/EngineState.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class AnimatorPanel
{
public:
    void Render(EngineState& state, VulkanContext* vulkan_context);
    void Shutdown();

private:
    void RefreshControllerList(const std::filesystem::path& animators_dir);
    void EnsureControllerLoaded(const std::filesystem::path& controller_path, EngineState& state);
    bool SaveCurrentController(const std::filesystem::path& controller_path, EngineState& state);
    void MarkControllerListDirty();
    void RenderControllerEditor(EngineState& state);
    void RenderFaceTab(EngineState& state);
    void RenderNodeLibrary(EngineState& state);
    void RenderPreviewViewport(EngineState& state);
    bool ImportAnimationsFromModel(const std::filesystem::path& model_path, EngineState& state);
    void SetPreviewModelPath(EngineState& state, const std::filesystem::path& absolute_path);
    void EnsurePreviewModelLoaded(EngineState& state);

    VulkanContext* vulkan_context_ = nullptr;

    int selected_controller_index_ = 0;
    char new_controller_name_[128] = "NewAnimator";
    std::vector<std::string> controller_names_;
    std::vector<std::filesystem::path> controller_paths_;
    std::filesystem::path last_scanned_dir_;
    std::uint64_t last_dir_signature_ = 0;

    std::filesystem::path loaded_controller_path_;
    AnimatorControllerAsset controller_{};
    bool controller_loaded_ = false;
    bool controller_dirty_ = false;

    AnimatorPreviewRenderer preview_renderer_;
    std::string last_loaded_preview_path_; // tracks which path is currently bound

    // Index of the face expression pose currently being edited/previewed in the
    // Face tab, or -1 for none. Drives the live blendshape preview.
    int selected_face_pose_index_ = -1;

    // ---- Lip-sync (Face tab) bake + preview state ----
    std::string face_bake_wav_path_;        // path of the WAV to bake (set by drag-drop)
    std::string face_bake_status_;          // last bake result message
    FaceClipAsset face_preview_clip_;       // loaded clip for preview playback
    std::string face_preview_clip_loaded_;  // which clip path is currently loaded
    bool face_lipsync_playing_ = false;
    float face_lipsync_time_seconds_ = 0.0f;
    int face_lipsync_clip_index_ = -1;      // which clip in the list is previewing
};
