#pragma once

#include "state/EngineState.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class VulkanContext;

class SequencerPanel
{
public:
    void Render(EngineState& state, VulkanContext* vulkan_context);

private:
    void RefreshSceneList(const std::filesystem::path& scenes_dir);

    // Scene selector
    int selected_scene_index_ = 0;
    std::vector<std::string> scene_names_;
    std::vector<std::filesystem::path> scene_paths_;
    std::filesystem::path last_scanned_dir_;
    std::uint64_t last_dir_signature_ = 0;

    // Transport
    bool playing_ = false;
    float timeline_seconds_ = 0.0f;
    float timeline_max_seconds_ = 10.0f;
};
