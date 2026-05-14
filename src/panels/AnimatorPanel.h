#pragma once

#include "app/VulkanContext.h"
#include "assets/AnimatorControllerAsset.h"
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
};
