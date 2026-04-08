#pragma once

#include "state/EngineState.h"
#include "panels/FilesPanel.h"
#include "panels/LogPanel.h"
#include "panels/SettingsPanel.h"
#include "panels/WorkspacePanel.h"

#include <SDL3/SDL.h>

class EngineApplication
{
public:
    bool Init();
    void RunLoop();
    void Shutdown();

private:
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    float display_scale_ = 1.0f;
    bool running_ = false;

    EngineState state_;
    FilesPanel files_panel_;
    WorkspacePanel workspace_panel_;
    SettingsPanel settings_panel_;
    LogPanel log_panel_;

    void ProcessEvents();
    void RenderUI();
    void ApplyStyle();
    void BuildDefaultDockLayout(ImGuiID dockspace_id);
};