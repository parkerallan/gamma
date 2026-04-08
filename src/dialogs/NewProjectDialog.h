#pragma once

#include "state/EngineState.h"

#include <filesystem>

class NewProjectDialog
{
public:
    void Open();
    bool Render(EngineState& state);

private:
    std::filesystem::path selected_project_root_;

    bool BrowseForProjectRoot(EngineState& state);
    bool CreateProjectScaffold(const std::filesystem::path& project_root, EngineState& state);
    void ClearSelection();
};