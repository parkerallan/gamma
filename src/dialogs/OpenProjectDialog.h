#pragma once

#include "state/EngineState.h"

#include <array>

class OpenProjectDialog
{
public:
    void Open();
    bool Render(EngineState& state);

private:
    static constexpr std::size_t kPathCapacity = 512;

    std::array<char, kPathCapacity> path_buffer_{};

    bool TryLoadProject(EngineState& state);
    bool BrowseForProjectFolder(EngineState& state);
    bool BrowseForProjectFile(EngineState& state);
    void SetSelectedPath(const std::filesystem::path& path);
    void Reset();
};