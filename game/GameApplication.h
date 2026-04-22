#pragma once

#include "app/VulkanContext.h"
#include "render/RuntimeRenderer.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>

class GameApplication
{
public:
    bool Init(int argc, char* argv[]);
    void RunLoop();
    void Shutdown();

private:
    SDL_Window* window_ = nullptr;
    VulkanContext vulkan_context_{};
    VulkanWindowContext window_context_{};
    RuntimeRenderer renderer_{};
    bool running_ = false;

    std::string window_title_;
    std::filesystem::path content_root_;
    std::filesystem::path startup_scene_path_;

    std::filesystem::path ResolveExeDirectory(const char* argv0) const;
    bool LoadConfig(const std::filesystem::path& exe_dir);
    bool ExtractPakIfNeeded(const std::filesystem::path& exe_dir);
};
