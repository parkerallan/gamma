#pragma once

#include "state/EngineState.h"

#include <array>
#include <filesystem>

class CreationMenu
{
public:
    void RenderButton(const std::filesystem::path& directory_path, const char* label = "+", bool align_right = true);
    bool Render(EngineState& state);

private:
    static constexpr const char* kCreateItemPopupName = "Create Item";

    enum class CreateTarget
    {
        None,
        Folder,
        Script,
    };

    static constexpr std::size_t kNameCapacity = 128;

    std::filesystem::path target_directory_;
    std::array<char, kNameCapacity> name_buffer_{};
    CreateTarget create_target_ = CreateTarget::None;
    bool focus_name_input_ = false;
    bool open_create_popup_ = false;

    bool CreateItem(EngineState& state);
    void OpenCreateDialog(const std::filesystem::path& directory_path, CreateTarget create_target);
    void Reset();
};