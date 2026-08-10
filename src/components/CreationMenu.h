#pragma once

#include "state/EngineState.h"

#include <array>
#include <filesystem>

class CreationMenu
{
public:
    bool RenderButton(EngineState& state, const std::filesystem::path& directory_path, const char* label = "+", bool align_right = true);
    // Import entries, rendered by the Assets panel. Imports land in
    // `directory_path` when it sits inside the project's Assets folder.
    bool RenderImportButton(EngineState& state, const std::filesystem::path& directory_path, const char* label);
    // Create entries for the Assets panel's right-click menu (New Folder /
    // New Script). Call inside an open popup.
    void RenderAssetCreateItems(EngineState& state, const std::filesystem::path& directory_path);
    bool Render(EngineState& state);

private:
    static constexpr const char* kCreateItemPopupName = "Create Item";

    enum class CreateTarget
    {
        None,
        Folder,
        Script,
        Graph,
        Scene,
        Object,
    };

    static constexpr std::size_t kNameCapacity = 128;

    std::filesystem::path target_directory_;
    std::filesystem::path target_scene_path_;
    std::array<char, kNameCapacity> name_buffer_{};
    CreateTarget create_target_ = CreateTarget::None;
    bool focus_name_input_ = false;
    bool open_create_popup_ = false;

    bool CreateItem(EngineState& state);
    bool ImportModel(EngineState& state, const std::filesystem::path& directory_path);
    bool ImportFont(EngineState& state, const std::filesystem::path& directory_path);
    bool ImportImage(EngineState& state, const std::filesystem::path& directory_path);
    bool ImportVideo(EngineState& state, const std::filesystem::path& directory_path);
    bool ImportAudio(EngineState& state, const std::filesystem::path& directory_path);
    void OpenCreateDialog(const std::filesystem::path& directory_path, CreateTarget create_target, std::filesystem::path target_scene_path = {});
    void Reset();
};