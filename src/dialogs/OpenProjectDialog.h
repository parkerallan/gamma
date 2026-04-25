#pragma once

#include "state/EngineState.h"

class OpenProjectDialog
{
public:
    void Open();
    bool Render(EngineState& state);

private:
    bool is_open_ = false;

    bool BrowseAndLoadFolder(EngineState& state);
    bool BrowseAndLoadFile(EngineState& state);
    bool LoadPath(const std::filesystem::path& path, EngineState& state);
};