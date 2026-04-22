#pragma once

#include "state/EngineState.h"

#include <array>

class BuildGameDialog
{
public:
    void Open(const EngineState& state);
    bool Render(EngineState& state);

private:
    static constexpr std::size_t kNameCapacity = 128;
    static constexpr std::size_t kPathCapacity = 512;

    std::array<char, kNameCapacity> game_name_buffer_{};
    std::array<char, kPathCapacity> output_root_buffer_{};
    int build_type_index_ = 0;

    bool BrowseForOutputRoot(EngineState& state);
    bool SubmitBuildRequest(EngineState& state);
    void ResetFromState(const EngineState& state);
    void SetOutputRoot(const std::filesystem::path& path);
};