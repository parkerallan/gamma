#pragma once

#include "state/EngineState.h"

#include <SDL3/SDL.h>

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

class MappingPanel
{
public:
    void Render(EngineState& state);

    static int KeyCount();

private:
    void StopMapping();
    void SaveMappings(EngineState& state) const;
    void LoadMappings(EngineState& state);

    // Tracks which project's mappings are currently loaded so we reload when the
    // open project changes.
    std::filesystem::path loaded_project_root_;
    bool mappings_dirty_ = false;

    int selected_device_index_ = -1;
    std::vector<std::string>    device_names_;
    std::vector<SDL_JoystickID> device_ids_;
    std::vector<std::pair<std::string, std::string>> custom_rows_;

    // Controller mapping session
    bool         mapping_active_     = false;
    bool         mapping_single_row_ = false; // true = stop after one row
    int          mapping_row_        = 0;
    SDL_Gamepad* active_gamepad_ = nullptr;
    std::array<bool,  SDL_GAMEPAD_BUTTON_COUNT> prev_button_state_{};
    std::array<Sint16, SDL_GAMEPAD_AXIS_COUNT>  prev_axis_state_{};
};
