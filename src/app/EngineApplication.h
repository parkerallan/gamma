#pragma once

#include "app/VulkanContext.h"
#include "state/EngineState.h"
#include "panels/FilesPanel.h"
#include "panels/InfoPanel.h"
#include "panels/LogPanel.h"
#include "panels/PerformancePanel.h"
#include "panels/SettingsPanel.h"
#include "panels/VersionControlPanel.h"
#include "panels/WorkspacePanel.h"
#include "render/RuntimeRenderer.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class EngineApplication
{
public:
    bool Init();
    void RunLoop();
    void Shutdown();

private:
    SDL_Window* window_ = nullptr;
    SDL_Window* runtime_window_ = nullptr;
    VulkanContext vulkan_context_{};
    VulkanWindowContext runtime_window_context_{};
    RuntimeRenderer runtime_renderer_{};
    float display_scale_ = 1.0f;
    bool running_ = false;
    bool focus_log_panel_next_frame_ = true;
    bool focus_performance_panel_next_frame_ = false;

    EngineState state_;
    FilesPanel files_panel_;
    VersionControlPanel version_control_panel_;
    WorkspacePanel workspace_panel_;
    SettingsPanel settings_panel_;
    InfoPanel info_panel_;
    LogPanel log_panel_;
    PerformancePanel performance_panel_;

    // Background game build
    std::thread build_thread_;
    std::atomic<bool> is_build_running_{false};
    std::atomic<bool> build_succeeded_{false};
    std::atomic<bool> build_stop_requested_{false};
    std::mutex build_log_mutex_;
    std::vector<std::string> pending_build_log_;
    std::string pending_build_error_; // written once, under build_log_mutex_
#ifdef _WIN32
    std::mutex active_build_process_mutex_;
    void* active_build_process_ = nullptr;
#endif

    void ProcessEvents();
    void HandleBuildRequests();
    void DrainBuildLog();
    void HandlePlayRequests();
    void ExecuteBuildRequest(
        const EngineBuildRequest& request,
        const std::filesystem::path& project_root,
        const std::filesystem::path& active_scene_path,
        const std::filesystem::path& project_file_path); // runs on build_thread_
    bool StageBuiltGame(
        const EngineBuildRequest& request,
        const std::filesystem::path& project_root,
        const std::filesystem::path& active_scene_path,
        const std::filesystem::path& project_file_path,
        const std::filesystem::path& external_build_directory,
        const std::filesystem::path& built_output_directory,
        const std::filesystem::path& built_game_executable_path,
        const std::function<void(const std::string&)>& log,
        std::string& out_error);
    bool StartRuntimeSession();
    void StopRuntimeSession();
    void RenderRuntimeWindow();
    void RenderUI();
    void RenderMainMenuBar();
    void ApplyStyle();
    void BuildDefaultDockLayout(ImGuiID dockspace_id);
    bool CancelActiveBuildProcess();
};