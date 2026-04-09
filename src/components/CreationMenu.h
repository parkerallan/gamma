#pragma once

#include "state/EngineState.h"

#include <array>
#include <filesystem>

class CreationMenu
{
public:
    bool RenderButton(EngineState& state, const std::filesystem::path& directory_path, const char* label = "+", bool align_right = true);
    bool Render(EngineState& state);

private:
    static constexpr const char* kCreateItemPopupName = "Create Item";

    enum class CreateTarget
    {
        None,
        Folder,
        Script,
        Scene,
        Material,
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
    bool ImportMaterial(EngineState& state, const std::filesystem::path& directory_path);
    bool ImportTexture(EngineState& state, const std::filesystem::path& directory_path);
    void OpenCreateDialog(const std::filesystem::path& directory_path, CreateTarget create_target, std::filesystem::path target_scene_path = {});
    void Reset();
};