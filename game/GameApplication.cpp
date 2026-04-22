#include "GameApplication.h"

#include "assets/SceneMetadata.h"
#include "pak/PakArchive.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace
{
std::unordered_map<std::string, std::string> ParseConfig(const std::filesystem::path& path)
{
    std::unordered_map<std::string, std::string> result;
    std::ifstream file(path);
    if (!file)
    {
        return result;
    }

    std::string line;
    while (std::getline(file, line))
    {
        const auto pos = line.find('=');
        if (pos == std::string::npos)
        {
            continue;
        }

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        const auto trim = [](std::string& s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\n'))
            {
                s.erase(s.begin());
            }
            while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n'))
            {
                s.pop_back();
            }
        };

        trim(key);
        trim(value);
        result[key] = value;
    }

    return result;
}
} // namespace

std::filesystem::path GameApplication::ResolveExeDirectory(const char* argv0) const
{
    const char* base_path = SDL_GetBasePath();
    if (base_path != nullptr)
    {
        return std::filesystem::path(base_path);
    }

    if (argv0 != nullptr)
    {
        const std::filesystem::path exe_path(argv0);
        if (exe_path.has_parent_path())
        {
            return exe_path.parent_path();
        }
    }

    std::error_code ec;
    return std::filesystem::current_path(ec);
}

bool GameApplication::LoadConfig(const std::filesystem::path& exe_dir)
{
    const std::filesystem::path config_path = exe_dir / "config.ini";
    const auto config = ParseConfig(config_path);

    const auto title_it = config.find("windowTitle");
    window_title_ = title_it != config.end() ? title_it->second : "Game";

    const auto content_it = config.find("contentRoot");
    const std::string content_root_rel = content_it != config.end() ? content_it->second : "Content";
    content_root_ = exe_dir / content_root_rel;

    const auto scene_it = config.find("startupScene");
    if (scene_it == config.end() || scene_it->second.empty())
    {
        SDL_Log("config.ini: startupScene is missing or empty");
        return false;
    }

    startup_scene_path_ = std::filesystem::path(scene_it->second);
    return true;
}

bool GameApplication::ExtractPakIfNeeded(const std::filesystem::path& exe_dir)
{
    std::error_code ec;
    if (std::filesystem::exists(content_root_, ec))
    {
        const bool empty = std::filesystem::is_empty(content_root_, ec);
        if (!ec && !empty)
        {
            return true;
        }
    }

    const std::filesystem::path pak_path = exe_dir / "assets.pak";
    if (!std::filesystem::exists(pak_path, ec))
    {
        // No PAK present — content may already be unpacked alongside the executable.
        return true;
    }

    PakArchive archive;
    if (!archive.Open(pak_path))
    {
        SDL_Log("Failed to open assets.pak at: %s", pak_path.string().c_str());
        return false;
    }

    std::filesystem::create_directories(content_root_, ec);
    if (ec)
    {
        SDL_Log("Failed to create content directory: %s", content_root_.string().c_str());
        return false;
    }

    if (!archive.ExtractAll(content_root_))
    {
        SDL_Log("Failed to extract assets.pak to: %s", content_root_.string().c_str());
        return false;
    }

    return true;
}

bool GameApplication::Init(int argc, char* argv[])
{
    const std::filesystem::path exe_dir = ResolveExeDirectory(argc > 0 ? argv[0] : nullptr);

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    if (!LoadConfig(exe_dir))
    {
        return false;
    }

    if (!ExtractPakIfNeeded(exe_dir))
    {
        return false;
    }

    const SDL_WindowFlags window_flags =
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(window_title_.c_str(), 1280, 720, window_flags);
    if (window_ == nullptr)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    if (!vulkan_context_.Initialize(window_))
    {
        SDL_Log("VulkanContext::Initialize failed");
        return false;
    }

    // Minimal ImGui init — required because VulkanContext uses ImGui data
    // structures for swapchain resource management. No ImGui widgets are shown.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplSDL3_InitForVulkan(window_);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = vulkan_context_.GetInstance();
    init_info.PhysicalDevice = vulkan_context_.GetPhysicalDevice();
    init_info.Device = vulkan_context_.GetDevice();
    init_info.QueueFamily = vulkan_context_.GetQueueFamily();
    init_info.Queue = vulkan_context_.GetQueue();
    init_info.PipelineCache = vulkan_context_.GetPipelineCache();
    init_info.DescriptorPool = vulkan_context_.GetDescriptorPool();
    init_info.MinImageCount = vulkan_context_.GetMinImageCount();
    init_info.ImageCount = vulkan_context_.GetImageCount();
    init_info.Allocator = vulkan_context_.GetAllocator();
    init_info.PipelineInfoMain.RenderPass = vulkan_context_.GetRenderPass();
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = VulkanContext::CheckVkResult;
    ImGui_ImplVulkan_Init(&init_info);

    if (!renderer_.Initialize(&vulkan_context_))
    {
        SDL_Log("RuntimeRenderer::Initialize failed");
        return false;
    }

    const std::filesystem::path scene_path = content_root_ / startup_scene_path_;
    const SceneMetadata scene_metadata = LoadSceneMetadata(scene_path);
    const ActiveSceneCameraSelection camera = FindActiveSceneCamera(scene_metadata);

    std::string start_error;
    if (!renderer_.StartSession(content_root_, scene_path, camera, &start_error))
    {
        SDL_Log("RuntimeRenderer::StartSession failed: %s", start_error.c_str());
        return false;
    }

    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);
    running_ = true;
    return true;
}

void GameApplication::RunLoop()
{
    while (running_)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);

            if (event.type == SDL_EVENT_QUIT)
            {
                running_ = false;
            }
            else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                     event.window.windowID == SDL_GetWindowID(window_))
            {
                running_ = false;
            }
        }

        if (!running_)
        {
            break;
        }

        if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0)
        {
            SDL_Delay(10);
            continue;
        }

        int width = 0;
        int height = 0;
        SDL_GetWindowSize(window_, &width, &height);
        if (width <= 0 || height <= 0)
        {
            SDL_Delay(10);
            continue;
        }

        std::string render_error;
        if (!renderer_.RenderFrame(
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height),
                &render_error))
        {
            SDL_Log("RenderFrame failed: %s", render_error.c_str());
            running_ = false;
            break;
        }

        if (!vulkan_context_.PresentImageToMainWindow(
                window_,
                renderer_.GetOutputImage(),
                renderer_.GetOutputLayout(),
                renderer_.GetOutputWidth(),
                renderer_.GetOutputHeight()))
        {
            SDL_Log("PresentImageToMainWindow failed");
        }
    }
}

void GameApplication::Shutdown()
{
    renderer_.Shutdown();

    vulkan_context_.WaitIdle();

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    vulkan_context_.Shutdown();

    if (window_ != nullptr)
    {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    SDL_Quit();
}
