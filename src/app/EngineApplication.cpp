#include "app/EngineApplication.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "imgui_internal.h"
#include "components/graph/GraphTranspiler.h"
#include "vfs/PakArchive.h"
#include "ui/Codicons.h"

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>
#include <string>
#include <unordered_set>
#include <vector>
#include <stb_image.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
constexpr float kBottomBarHeight = 20.0f;

constexpr ImWchar kCodiconGlyphRanges[] = {
    static_cast<ImWchar>(ICON_MIN_CI),
    static_cast<ImWchar>(ICON_MAX_16_CI),
    0,
};

bool IsWorkspaceRoot(const std::filesystem::path& path)
{
    return std::filesystem::exists(path / "CMakeLists.txt") && std::filesystem::exists(path / "src");
}

std::filesystem::path ResolveWorkspaceRoot()
{
    std::error_code error;
    std::filesystem::path current = std::filesystem::current_path(error);
    if (!error && IsWorkspaceRoot(current))
    {
        return current;
    }

    const char* base_path_raw = SDL_GetBasePath();
    if (base_path_raw != nullptr)
    {
        std::filesystem::path base_path(base_path_raw);
        for (std::filesystem::path candidate = base_path; !candidate.empty(); candidate = candidate.parent_path())
        {
            if (IsWorkspaceRoot(candidate))
            {
                return candidate;
            }

            if (candidate == candidate.root_path())
            {
                break;
            }
        }
    }

    return current;
}

std::filesystem::path ResolveCodiconFontPath(const std::filesystem::path& workspace_root)
{
    const std::array<std::filesystem::path, 2> relative_paths = {
        std::filesystem::path("src/ui/codicon.ttf"),
        std::filesystem::path("ui/codicon.ttf"),
    };

    std::error_code error;
    if (!workspace_root.empty())
    {
        for (const std::filesystem::path& relative_path : relative_paths)
        {
            const std::filesystem::path candidate = workspace_root / relative_path;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
            error.clear();
        }
    }

    const std::filesystem::path current = std::filesystem::current_path(error);
    if (!error)
    {
        for (const std::filesystem::path& relative_path : relative_paths)
        {
            const std::filesystem::path candidate = current / relative_path;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
            error.clear();
        }
    }

    const char* base_path_raw = SDL_GetBasePath();
    if (base_path_raw != nullptr)
    {
        const std::filesystem::path base_path(base_path_raw);
        for (const std::filesystem::path& relative_path : relative_paths)
        {
            const std::filesystem::path candidate = base_path / relative_path;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
            error.clear();
        }
    }

    return {};
}

std::filesystem::path ResolveWindowIconPath(const std::filesystem::path& workspace_root)
{
    const std::array<std::filesystem::path, 3> relative_paths = {
        std::filesystem::path("src/ui/logo256.png"),
        std::filesystem::path("ui/logo256.png"),
        std::filesystem::path("logo256.png"),
    };

    std::error_code error;
    if (!workspace_root.empty())
    {
        for (const std::filesystem::path& relative_path : relative_paths)
        {
            const std::filesystem::path candidate = workspace_root / relative_path;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
            error.clear();
        }
    }

    const std::filesystem::path current = std::filesystem::current_path(error);
    if (!error)
    {
        for (const std::filesystem::path& relative_path : relative_paths)
        {
            const std::filesystem::path candidate = current / relative_path;
            if (std::filesystem::exists(candidate, error))
            {
                return candidate;
            }
            error.clear();
        }
    }

    return {};
}

void ApplyWindowIcon(SDL_Window* window, const std::filesystem::path& workspace_root)
{
    if (window == nullptr)
    {
        return;
    }

    const std::filesystem::path icon_path = ResolveWindowIconPath(workspace_root);
    if (icon_path.empty())
    {
        SDL_Log("Window icon not found; expected src/ui/logo256.png");
        return;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* rgba_pixels = stbi_load(icon_path.string().c_str(), &width, &height, &channels, 4);
    if (rgba_pixels == nullptr)
    {
        SDL_Log("Failed to load window icon %s", icon_path.string().c_str());
        return;
    }

    SDL_Surface* surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, rgba_pixels, width * 4);
    if (surface == nullptr)
    {
        SDL_Log("Failed to create icon surface: %s", SDL_GetError());
        stbi_image_free(rgba_pixels);
        return;
    }

    SDL_SetWindowIcon(window, surface);
    SDL_DestroySurface(surface);
    stbi_image_free(rgba_pixels);
}

void LoadUserInterfaceFonts(ImGuiIO& io, const std::filesystem::path& workspace_root)
{
    ImFont* default_font = io.Fonts->AddFontDefaultVector();
    if (default_font == nullptr)
    {
        default_font = io.Fonts->AddFontDefaultBitmap();
    }

    if (default_font == nullptr)
    {
        SDL_Log("Failed to initialize the default ImGui font");
        return;
    }

    const std::filesystem::path codicon_font_path = ResolveCodiconFontPath(workspace_root);
    if (codicon_font_path.empty())
    {
        SDL_Log("Codicon font not found; viewport toolbar icons will use missing glyphs");
        return;
    }

    ImFontConfig icon_font_config;
    icon_font_config.MergeMode = true;
    icon_font_config.PixelSnapH = true;
    icon_font_config.GlyphOffset.y = 2.0f;
    icon_font_config.Flags |= ImFontFlags_NoLoadError;

    const float icon_font_size = default_font->LegacySize > 0.0f ? default_font->LegacySize : 13.0f;
    if (io.Fonts->AddFontFromFileTTF(
            codicon_font_path.string().c_str(),
            icon_font_size,
            &icon_font_config,
            kCodiconGlyphRanges) == nullptr)
    {
        SDL_Log("Failed to merge Codicon font from %s", codicon_font_path.string().c_str());
    }
}

std::string QuoteCommandArgument(const std::string& value)
{
    std::string quoted = "\"";
    for (const char character : value)
    {
        if (character == '"')
        {
            quoted += '\\';
        }
        quoted += character;
    }
    quoted += "\"";
    return quoted;
}

// Run a command, calling line_cb for each line of combined stdout+stderr.
// Returns the process exit code, or -1 on failure to launch.
// On Windows uses CreateProcess with CREATE_NO_WINDOW so no console window
// appears and the SDL/Vulkan window is not disrupted.
int RunCommand(
    const std::string& command,
    const std::function<void(const std::string&)>& line_cb,
    std::atomic<bool>* cancel_requested = nullptr,
#ifdef _WIN32
    std::mutex* active_process_mutex = nullptr,
    void** active_process = nullptr
#else
    void* = nullptr,
    void* = nullptr
#endif
)
{
#ifdef _WIN32
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&read_end, &write_end, &sa, 0))
    {
        return -1;
    }
    // Read end must NOT be inherited by the child.
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(STARTUPINFOA);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_end;
    si.hStdError  = write_end;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};

    // Route through cmd.exe so that shell redirections work correctly.
    const std::string full_cmd = "cmd.exe /C " + command;
    std::vector<char> cmd_buf(full_cmd.begin(), full_cmd.end());
    cmd_buf.push_back('\0');

    const BOOL launched = CreateProcessA(
        nullptr, cmd_buf.data(),
        nullptr, nullptr,
        /*bInheritHandles=*/TRUE,
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    // Parent doesn't write to the pipe; close the write end now.
    CloseHandle(write_end);

    if (!launched)
    {
        CloseHandle(read_end);
        return -1;
    }
    CloseHandle(pi.hThread);

    if (active_process_mutex != nullptr && active_process != nullptr)
    {
        std::lock_guard<std::mutex> lock(*active_process_mutex);
        *active_process = pi.hProcess;
    }

    std::string line;
    bool was_cancelled = false;
    while (true)
    {
        if (cancel_requested != nullptr && cancel_requested->load())
        {
            TerminateProcess(pi.hProcess, ERROR_CANCELLED);
            was_cancelled = true;
        }

        DWORD bytes_available = 0;
        if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &bytes_available, nullptr))
        {
            break;
        }

        if (bytes_available > 0)
        {
            char ch = '\0';
            DWORD bytes_read = 0;
            if (!ReadFile(read_end, &ch, 1, &bytes_read, nullptr) || bytes_read == 0)
            {
                break;
            }

            if (ch == '\n' || ch == '\r')
            {
                if (!line.empty())
                {
                    line_cb(line);
                    line.clear();
                }
            }
            else
            {
                line += ch;
            }
        }
        else
        {
            const DWORD wait_result = WaitForSingleObject(pi.hProcess, 25);
            if (wait_result == WAIT_OBJECT_0)
            {
                break;
            }
        }
    }
    if (!line.empty())
    {
        line_cb(line);
    }

    CloseHandle(read_end);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    if (active_process_mutex != nullptr && active_process != nullptr)
    {
        std::lock_guard<std::mutex> lock(*active_process_mutex);
        *active_process = nullptr;
    }

    CloseHandle(pi.hProcess);
    if (was_cancelled)
    {
        return -2;
    }
    return static_cast<int>(exit_code);

#else
    const std::string redirected = command + " 2>&1";
    FILE* pipe = popen(redirected.c_str(), "r");
    if (pipe == nullptr)
    {
        return -1;
    }
    std::string line;
    char ch = '\0';
    while (std::fread(&ch, 1, 1, pipe) == 1)
    {
        if (ch == '\n' || ch == '\r')
        {
            if (!line.empty())
            {
                line_cb(line);
                line.clear();
            }
        }
        else
        {
            line += ch;
        }
    }
    if (!line.empty())
    {
        line_cb(line);
    }
    return pclose(pipe);
#endif
}

bool IsPathWithin(const std::filesystem::path& parent, const std::filesystem::path& candidate)
{
    if (parent.empty() || candidate.empty())
    {
        return false;
    }

    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(candidate, parent, error);
    if (error || relative.empty())
    {
        return false;
    }

    const std::string relative_string = relative.generic_string();
    return relative == "." || (relative_string != ".." && relative_string.rfind("../", 0) != 0);
}

std::string BuildTypeToConfigName(EngineBuildType build_type)
{
    return build_type == EngineBuildType::Debug ? "Debug" : "Release";
}

std::string BuildTypeToFolderSuffix(EngineBuildType build_type)
{
    return build_type == EngineBuildType::Debug ? "debug" : "release";
}

std::string BuildPlatformToFolderSuffix(EngineBuildPlatform build_platform)
{
    return build_platform == EngineBuildPlatform::Windows ? "windows" : "linux";
}

std::string BuildPlatformToLogLabel(EngineBuildPlatform build_platform)
{
    return build_platform == EngineBuildPlatform::Windows ? "Windows (MSVC)" : "Linux (GCC)";
}

std::filesystem::path GetExternalBuildDirectory(const EngineBuildRequest& request)
{
    std::string build_folder_name = request.folder_name + "-build-" + BuildPlatformToFolderSuffix(request.build_platform);
    if (request.build_platform == EngineBuildPlatform::Linux)
    {
        // Linux builds are single-config, so keep separate caches per config.
        build_folder_name += "-" + BuildTypeToFolderSuffix(request.build_type);
    }

    return request.output_root / build_folder_name;
}

std::filesystem::path GetBuiltOutputDirectory(
    EngineBuildPlatform build_platform,
    const std::filesystem::path& external_build_directory,
    const std::string& config_name)
{
    if (build_platform == EngineBuildPlatform::Windows)
    {
        return external_build_directory / config_name;
    }

    return external_build_directory;
}

std::filesystem::path GetBuiltGameExecutablePath(
    EngineBuildPlatform build_platform,
    const std::filesystem::path& built_output_directory)
{
    return built_output_directory / (build_platform == EngineBuildPlatform::Windows ? "game.exe" : "game");
}

bool ReadTextFile(const std::filesystem::path& path, std::string& out_contents)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    out_contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

bool LinuxBuildCacheNeedsRefresh(const std::filesystem::path& external_build_directory)
{
    const std::filesystem::path cmake_cache = external_build_directory / "CMakeCache.txt";
    const std::filesystem::path makefile_path = external_build_directory / "Makefile";
    std::string cache_contents;
    if (!ReadTextFile(cmake_cache, cache_contents))
    {
        return true;
    }

    return !std::filesystem::exists(makefile_path) ||
        cache_contents.find("CMAKE_GENERATOR:INTERNAL=Unix Makefiles") == std::string::npos ||
        cache_contents.find("CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND") != std::string::npos;
}

std::string NormalizeCachePathForCompare(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool WindowsGameIconCacheNeedsRefresh(
    const std::filesystem::path& external_build_directory,
    const std::filesystem::path& requested_icon_path)
{
    const std::filesystem::path cmake_cache = external_build_directory / "CMakeCache.txt";
    std::string cache_contents;
    if (!ReadTextFile(cmake_cache, cache_contents))
    {
        return true;
    }

    const std::string marker = "GAME_WINDOWS_ICON_PATH:STRING=";
    const std::size_t marker_pos = cache_contents.find(marker);
    std::string cached_path;
    if (marker_pos != std::string::npos)
    {
        const std::size_t value_start = marker_pos + marker.size();
        const std::size_t value_end = cache_contents.find_first_of("\r\n", value_start);
        cached_path = cache_contents.substr(value_start, value_end == std::string::npos ? std::string::npos : value_end - value_start);
    }

    std::string requested_path;
    if (!requested_icon_path.empty())
    {
        requested_path = requested_icon_path.lexically_normal().generic_string();
    }

    return NormalizeCachePathForCompare(cached_path) != NormalizeCachePathForCompare(requested_path);
}

// Convert an absolute Windows path to the equivalent WSL path under /mnt/<drive>/...
// e.g.  E:\Projects\engine  ->  /mnt/e/Projects/engine
std::string WindowsPathToWsl(const std::filesystem::path& windows_path)
{
    const std::string generic = windows_path.lexically_normal().generic_string();
    if (generic.size() >= 2 && std::isalpha(static_cast<unsigned char>(generic[0])) && generic[1] == ':')
    {
        std::string wsl_path = "/mnt/";
        wsl_path += static_cast<char>(std::tolower(static_cast<unsigned char>(generic[0])));
        wsl_path += generic.substr(2);
        return wsl_path;
    }
    return generic;
}

// Quote a path for use inside a wsl bash -lc '...' command.
std::string QuoteWslPath(const std::filesystem::path& path)
{
    std::string quoted = "'";
    for (const char c : WindowsPathToWsl(path))
    {
        if (c == '\'')
        {
            quoted += "'\\''";
        }
        else
        {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ShouldSkipStagedProjectEntry(
    const std::filesystem::path& entry_path,
    const std::filesystem::path& stage_directory,
    const std::filesystem::path& external_build_directory)
{
    const std::string file_name = entry_path.filename().string();
    if (entry_path == stage_directory || entry_path == external_build_directory)
    {
        return true;
    }

    return file_name == "Build" ||
        file_name == "build" ||
        file_name == ".engine-game-build" ||
        file_name == ".vs";
}
}

bool EngineApplication::CancelActiveBuildProcess()
{
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(active_build_process_mutex_);
    if (active_build_process_ == nullptr)
    {
        return false;
    }

    return TerminateProcess(static_cast<HANDLE>(active_build_process_), ERROR_CANCELLED) != 0;
#else
    return false;
#endif
}

bool EngineApplication::Init()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    display_scale_ = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

    const SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(
        "Gamma",
        static_cast<int>(1600.0f * display_scale_),
        static_cast<int>(900.0f * display_scale_),
        window_flags);

    if (window_ == nullptr)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    const std::filesystem::path workspace_root = ResolveWorkspaceRoot();
    ApplyWindowIcon(window_, workspace_root);

    if (!vulkan_context_.Initialize(window_))
    {
        return false;
    }
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
    io.ConfigDockingAlwaysTabBar = true;

    ApplyStyle();

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

    LoadUserInterfaceFonts(io, workspace_root);

    if (!workspace_panel_.InitializeSceneRenderer(&vulkan_context_))
    {
        SDL_Log("Scene viewport renderer initialization failed");
    }
    if (!info_panel_.InitializeSceneRenderer(&vulkan_context_))
    {
        SDL_Log("Info panel scene preview renderer initialization failed");
    }

    state_.SetWorkspaceRoot(workspace_root);
    state_.AddLog("Workspace root: " + state_.workspace_root.generic_string());
    state_.AddLog("Rendering backend: raw Vulkan API");
    state_.AddLog("Gamma started");
    running_ = true;
    return true;
}

void EngineApplication::RunLoop()
{
    while (running_)
    {
        if (window_ != nullptr)
        {
            const bool wants_text_input = ImGui::GetIO().WantTextInput;
            if (wants_text_input && !SDL_TextInputActive(window_))
            {
                SDL_StartTextInput(window_);
            }
            else if (!wants_text_input && SDL_TextInputActive(window_))
            {
                SDL_StopTextInput(window_);
            }
        }

        ProcessEvents();
        HandleBuildRequests();
        HandlePlayRequests();

        const bool editor_minimized = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0;
        const bool runtime_minimized = runtime_window_ != nullptr && (SDL_GetWindowFlags(runtime_window_) & SDL_WINDOW_MINIMIZED) != 0;
        if (editor_minimized && (!state_.is_playing || runtime_minimized))
        {
            SDL_Delay(10);
            continue;
        }

        workspace_panel_.BeginFrame();
        info_panel_.BeginFrame();
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        RenderUI();

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        workspace_panel_.RenderSceneGpuPass();
        info_panel_.RenderSceneGpuPass();
        RenderRuntimeWindow();
        vulkan_context_.RenderFrame(window_, draw_data, ImVec4(0.08f, 0.09f, 0.11f, 1.0f));
    }
}

void EngineApplication::Shutdown()
{
    // Wait for any in-progress background build to finish before tearing down.
    if (build_thread_.joinable())
    {
        build_thread_.join();
    }

    StopRuntimeSession();
    workspace_panel_.Shutdown();
    info_panel_.Shutdown();
    sequencer_panel_.Shutdown();

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

void EngineApplication::ProcessEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (runtime_window_ != nullptr &&
            event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == SDL_GetWindowID(runtime_window_))
        {
            state_.play_stop_requested = true;
            state_.play_restart_requested = false;
            continue;
        }

        ImGui_ImplSDL3_ProcessEvent(&event);

        if (event.type == SDL_EVENT_QUIT)
        {
            running_ = false;
        }

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window_))
        {
            running_ = false;
        }

        if (event.type == SDL_EVENT_KEY_DOWN && (event.key.mod & SDL_KMOD_CTRL) != 0 && event.key.key == SDLK_S)
        {
            if (state_.active_tab == WorkspaceTab::Graph && state_.HasOpenGraph())
            {
                workspace_panel_.SaveOpenGraph(state_);
            }
            else if (state_.HasOpenFile())
            {
                state_.SaveOpenFile();
            }
        }
    }
}

void EngineApplication::HandlePlayRequests()
{
    if (state_.play_stop_requested)
    {
        const bool restart_requested = state_.play_restart_requested;
        StopRuntimeSession();
        state_.play_stop_requested = false;
        state_.play_restart_requested = false;
        if (restart_requested)
        {
            state_.play_start_requested = true;
        }
    }

    if (state_.play_start_requested)
    {
        state_.play_start_requested = false;
        StartRuntimeSession();
    }
}

bool EngineApplication::StartRuntimeSession()
{
    if (!state_.HasActiveScene())
    {
        state_.SetPlayError("Cannot play: no active scene is available");
        return false;
    }

    const std::uint64_t play_start_ticks = SDL_GetPerformanceCounter();
    const std::uint64_t perf_freq = SDL_GetPerformanceFrequency();
    auto ms_since = [&](std::uint64_t start) -> double
    {
        if (perf_freq == 0)
        {
            return 0.0;
        }
        return static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0 / static_cast<double>(perf_freq);
    };

    // The editor preloads scene metadata (and all referenced models) on a
    // worker thread the first time the Scene tab is shown. If that load is
    // still in flight when the user clicks Play, block on it now so we can
    // hand its results to the runtime renderer instead of re-parsing.
    workspace_panel_.WaitForPendingViewportLoad(state_);

    SceneMetadata scene_metadata;
    if (const SceneMetadata* cached = workspace_panel_.TryGetCachedSceneMetadata(state_.active_scene_path))
    {
        scene_metadata = *cached;
    }
    else
    {
        scene_metadata = LoadSceneMetadata(state_.active_scene_path);
    }
    if (!scene_metadata.parsed)
    {
        state_.SetPlayError(scene_metadata.error_message.empty() ? "Failed to load active scene for Play" : scene_metadata.error_message);
        return false;
    }

    const ActiveSceneCameraSelection active_camera = FindActiveSceneCamera(scene_metadata);
    if (!active_camera.found)
    {
        state_.SetPlayError("Play requires one active camera in the scene");
        return false;
    }

    StopRuntimeSession();

    const SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    runtime_window_ = SDL_CreateWindow(
        "Gamma Runtime",
        static_cast<int>(1280.0f * display_scale_),
        static_cast<int>(720.0f * display_scale_),
        window_flags);
    if (runtime_window_ == nullptr)
    {
        state_.SetPlayError(std::string("Failed to create runtime window: ") + SDL_GetError());
        return false;
    }

    ApplyWindowIcon(runtime_window_, state_.workspace_root);

    // The runtime window must use a non-blocking present mode (MAILBOX) so
    // that two FIFO swapchains (editor + runtime) on one queue do not stutter
    // by serializing on each window's vblank. The standalone game has only
    // one swapchain and is smooth on FIFO; this gives play-mode comparable
    // pacing.
    if (!vulkan_context_.CreateWindowContext(runtime_window_, runtime_window_context_, /*prefer_low_latency=*/true))
    {
        SDL_DestroyWindow(runtime_window_);
        runtime_window_ = nullptr;
        state_.SetPlayError("Failed to initialize runtime Vulkan window");
        return false;
    }

    if (!runtime_renderer_.Initialize(&vulkan_context_))
    {
        vulkan_context_.DestroyWindowContext(runtime_window_context_);
        SDL_DestroyWindow(runtime_window_);
        runtime_window_ = nullptr;
        state_.SetPlayError("Failed to initialize runtime renderer backend");
        return false;
    }

    std::string runtime_error;
    if (!runtime_renderer_.StartSession(state_.project_root, state_.active_scene_path, active_camera, &runtime_error))
    {
        runtime_renderer_.Shutdown();
        vulkan_context_.DestroyWindowContext(runtime_window_context_);
        SDL_DestroyWindow(runtime_window_);
        runtime_window_ = nullptr;
        state_.SetPlayError(runtime_error.empty() ? "Failed to start runtime renderer session" : runtime_error);
        return false;
    }

    // Hand the editor's already-parsed scene metadata + model assets to the
    // runtime so the first runtime frame skips Assimp / scene-text parses.
    runtime_renderer_.SeedSceneMetadata(state_.active_scene_path, scene_metadata);
    std::size_t seeded_count = 0;
    for (const auto& [model_path, entry] : workspace_panel_.GetCachedModelAssets())
    {
        if (entry.asset.loaded)
        {
            runtime_renderer_.SeedModelAsset(model_path, entry.write_time, entry.asset);
            ++seeded_count;
        }
    }
    std::size_t seeded_audio = 0;
    for (const auto& [clip_path, bytes] : workspace_panel_.GetCachedAudioClipBytes())
    {
        if (!bytes.empty())
        {
            runtime_renderer_.SeedAudioClipBytes(clip_path, bytes);
            ++seeded_audio;
        }
    }
    std::size_t seeded_video = 0;
    for (const auto& [video_path, bytes] : workspace_panel_.GetCachedVideoBytes())
    {
        if (!bytes.empty())
        {
            runtime_renderer_.SeedVideoBytes(video_path, bytes);
            ++seeded_video;
        }
    }
    SDL_Log(
        "Play start: %.2f ms total, seeded scene + %zu models + %zu audio clip(s) + %zu video(s) from editor cache",
        ms_since(play_start_ticks),
        seeded_count,
        seeded_audio,
        seeded_video);

    SDL_SetWindowPosition(runtime_window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(runtime_window_);
    SDL_RaiseWindow(runtime_window_);
    SDL_PumpEvents();
    state_.is_playing = true;
    state_.playing_scene_path = state_.active_scene_path;
    state_.last_play_error.clear();
    focus_performance_panel_next_frame_ = true;
    focus_log_panel_next_frame_ = false;
    state_.AddLog("Runtime window opened for Play");
    return true;
}

void EngineApplication::StopRuntimeSession()
{
    if (state_.is_playing || runtime_window_ != nullptr)
    {
        vulkan_context_.WaitIdle();
    }

    runtime_renderer_.Shutdown();
    if (runtime_window_ != nullptr)
    {
        vulkan_context_.DestroyWindowContext(runtime_window_context_);
        SDL_DestroyWindow(runtime_window_);
        runtime_window_ = nullptr;
    }

    state_.is_playing = false;
    state_.playing_scene_path.clear();
    focus_log_panel_next_frame_ = true;
    focus_performance_panel_next_frame_ = false;
}

void EngineApplication::RenderRuntimeWindow()
{
    if (!state_.is_playing || runtime_window_ == nullptr)
    {
        return;
    }

    if ((SDL_GetWindowFlags(runtime_window_) & SDL_WINDOW_MINIMIZED) != 0)
    {
        return;
    }

    int width = 0;
    int height = 0;
    SDL_GetWindowSize(runtime_window_, &width, &height);
    if (width <= 0 || height <= 0)
    {
        return;
    }

    std::string runtime_error;
    // Mirror the editor's TAA debug-knob plumbing so SettingsPanel sliders
    // affect play-mode TAA as well (SceneViewportRenderer wires the same set
    // for the editor viewport).
    runtime_renderer_.SetTAAEnabled(state_.taa_enabled);
    {
        RayTracing::TaaDebugSettings taa_dbg{};
        taa_dbg.viz_mode             = state_.taa_viz_mode;
        taa_dbg.variance_scale       = state_.taa_variance_scale;
        taa_dbg.variance_scale_moving= state_.taa_variance_scale_moving;
        taa_dbg.anti_sparkle         = state_.taa_anti_sparkle;
        taa_dbg.history_blend        = state_.taa_history_blend;
        taa_dbg.jitter_compensation  = state_.taa_jitter_compensation;
        taa_dbg.adaptive_enabled     = state_.taa_adaptive_enabled;
        taa_dbg.adaptive_max_samples = state_.taa_adaptive_max_samples;
        taa_dbg.adaptive_threshold   = state_.taa_adaptive_threshold;
        taa_dbg.adaptive_preservation= state_.taa_adaptive_preservation;
        taa_dbg.dynamic_shadow_samples = state_.rt_dynamic_shadow_samples;
        runtime_renderer_.SetTaaDebugSettings(taa_dbg);
    }
    runtime_renderer_.SetTranspiledLuaDumpEnabled(state_.show_transpiled_lua);
    // When the setting is off, remove the Transpiled folder so it disappears from the file tree.
    if (!state_.show_transpiled_lua && state_.HasOpenProject())
    {
        std::error_code transpiled_ec;
        const std::filesystem::path transpiled_dir = state_.project_root / "Graphs" / "Transpiled";
        if (std::filesystem::exists(transpiled_dir, transpiled_ec))
        {
            std::filesystem::remove_all(transpiled_dir, transpiled_ec);
        }
    }
    if (!runtime_renderer_.RenderFrame(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), &runtime_error))
    {
        state_.SetPlayError(runtime_error.empty() ? "Runtime frame render failed" : runtime_error);
        state_.play_stop_requested = true;
        state_.play_restart_requested = false;
        return;
    }

    if (!vulkan_context_.PresentImageToWindow(
            runtime_window_,
            runtime_window_context_,
            runtime_renderer_.GetOutputImage(),
            runtime_renderer_.GetOutputLayout(),
            runtime_renderer_.GetOutputWidth(),
            runtime_renderer_.GetOutputHeight()))
    {
        state_.SetPlayError("Runtime present failed");
        state_.play_stop_requested = true;
        state_.play_restart_requested = false;
    }
}

void EngineApplication::RenderUI()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float bottom_bar_height = kBottomBarHeight * display_scale_;
    const float host_height = (std::max)(0.0f, viewport->Size.y - bottom_bar_height);

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, host_height));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_window_flags = ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_MenuBar |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("EngineDockHost", nullptr, host_window_flags);
    ImGui::PopStyleVar(3);

    RenderMainMenuBar();

    const ImGuiID dockspace_id = ImGui::GetID("EngineDockSpace");
    BuildDefaultDockLayout(dockspace_id);
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();

    RenderBottomBar();

    files_panel_.Render(state_);
    version_control_panel_.Render(state_);
    workspace_panel_.Render(state_);
    effects_panel_.Render(state_, &vulkan_context_);
    animator_panel_.Render(state_, &vulkan_context_);
    sequencer_panel_.Render(state_, &vulkan_context_);
    mapping_panel_.Render(state_);
    settings_panel_.Render(state_);
    info_panel_.Render(state_, &vulkan_context_);

    log_panel_.Render(state_);

    performance_panel_.Render(state_, runtime_renderer_);

    if (focus_log_panel_next_frame_ && state_.show_log_panel)
    {
        ImGui::SetWindowFocus("Log");
    }

    if (focus_performance_panel_next_frame_ && state_.show_performance_panel)
    {
        ImGui::SetWindowFocus("Performance");
    }

    focus_log_panel_next_frame_ = false;
    focus_performance_panel_next_frame_ = false;
}

void EngineApplication::RenderBottomBar()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float bottom_bar_height = kBottomBarHeight * display_scale_;
    const ImVec2 bar_min(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - bottom_bar_height);
    const ImVec2 bar_max(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y);

    ImDrawList* draw_list = ImGui::GetForegroundDrawList(viewport);
    draw_list->AddRectFilled(bar_min, bar_max, IM_COL32(126, 185, 0, 255));

    const char* version_label = "Version 0.0.1";
    const ImVec2 text_size = ImGui::CalcTextSize(version_label);
    const float text_x = bar_min.x + 12.0f * display_scale_;
    const float text_y = bar_min.y + (bottom_bar_height - text_size.y) * 0.5f;
    draw_list->AddText(ImVec2(text_x, text_y), IM_COL32(20, 31, 13, 255), version_label);
}

void EngineApplication::RenderMainMenuBar()
{
    if (!ImGui::BeginMenuBar())
    {
        return;
    }

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("Open Project..."))
        {
            state_.request_open_project_dialog = true;
        }

        if (ImGui::MenuItem("New Project..."))
        {
            state_.request_new_project_dialog = true;
        }

        const bool has_open_project = state_.HasOpenProject();
        if (ImGui::MenuItem("Close Project", nullptr, false, has_open_project))
        {
            state_.ClearOpenProject();
        }

        ImGui::Separator();

        const bool can_save_or_reload = state_.HasOpenFile() || state_.HasOpenGraph();
        if (ImGui::MenuItem("Save", "Ctrl+S", false, can_save_or_reload))
        {
            if (state_.active_tab == WorkspaceTab::Graph && state_.HasOpenGraph())
            {
                workspace_panel_.SaveOpenGraph(state_);
            }
            else
            {
                state_.SaveOpenFile();
            }
        }

        if (ImGui::MenuItem("Reload", nullptr, false, can_save_or_reload))
        {
            if (state_.active_tab == WorkspaceTab::Graph && state_.HasOpenGraph())
            {
                workspace_panel_.ReloadOpenGraph(state_);
            }
            else
            {
                state_.OpenTextFile(state_.open_file_path);
            }
        }

        ImGui::Separator();

        const bool build_running = is_build_running_.load();
        if (ImGui::MenuItem(build_running ? "Stop Build" : "Build", "Ctrl+B", false, state_.CanBuildProject()))
        {
            if (build_running)
            {
                state_.TriggerBuildStopAction();
            }
            else
            {
                state_.TriggerBuildAction();
            }
        }

        if (ImGui::MenuItem("Play", "F5", false, state_.CanPlayScene()))
        {
            state_.TriggerPlayAction();
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Exit"))
        {
            running_ = false;
        }

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit"))
    {
        const bool has_open_file = state_.HasOpenFile() || state_.HasOpenGraph();

        if (ImGui::MenuItem("Save Current File", "Ctrl+S", false, has_open_file))
        {
            if (state_.active_tab == WorkspaceTab::Graph && state_.HasOpenGraph())
            {
                workspace_panel_.SaveOpenGraph(state_);
            }
            else
            {
                state_.SaveOpenFile();
            }
        }

        if (ImGui::MenuItem("Reload From Disk", nullptr, false, has_open_file))
        {
            if (state_.active_tab == WorkspaceTab::Graph && state_.HasOpenGraph())
            {
                workspace_panel_.ReloadOpenGraph(state_);
            }
            else
            {
                state_.OpenTextFile(state_.open_file_path);
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Scene Tab", nullptr, state_.active_tab == WorkspaceTab::Scene, true))
        {
            state_.show_workspace_panel = true;
            state_.RequestTab(WorkspaceTab::Scene);
        }

        if (ImGui::MenuItem("Graph Tab", nullptr, state_.active_tab == WorkspaceTab::Graph, true))
        {
            state_.show_workspace_panel = true;
            state_.RequestTab(WorkspaceTab::Graph);
        }

        if (ImGui::MenuItem("Editor Tab", nullptr, state_.active_tab == WorkspaceTab::Editor, true))
        {
            state_.show_workspace_panel = true;
            state_.RequestTab(WorkspaceTab::Editor);
        }

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Settings"))
    {
        if (ImGui::MenuItem("Open Settings Panel"))
        {
            state_.show_settings_panel = true;
        }

        ImGui::Separator();
        ImGui::MenuItem("Auto-open startup scene", nullptr, &state_.auto_open_startup_scene);
        ImGui::MenuItem("Confirm before delete", nullptr, &state_.confirm_before_delete);
        ImGui::MenuItem("Highlight drop targets", nullptr, &state_.highlight_drop_targets);
        ImGui::MenuItem("Wrap editor text", nullptr, &state_.wrap_editor_text);
        ImGui::MenuItem("Auto-scroll log", nullptr, &state_.auto_scroll_log);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Panels"))
    {
        ImGui::MenuItem("Files", nullptr, &state_.show_files_panel);
        ImGui::MenuItem("Version Control", nullptr, &state_.show_version_control_panel);
        ImGui::MenuItem("Workspace", nullptr, &state_.show_workspace_panel);
        ImGui::MenuItem("Effects", nullptr, &state_.show_effects_panel);
        ImGui::MenuItem("Animator", nullptr, &state_.show_animator_panel);
        ImGui::MenuItem("Sequencer", nullptr, &state_.show_sequencer_panel);
        ImGui::MenuItem("Mapping", nullptr, &state_.show_mapping_panel);
        ImGui::MenuItem("Settings", nullptr, &state_.show_settings_panel);
        ImGui::MenuItem("Info", nullptr, &state_.show_info_panel);
        ImGui::MenuItem("Log", nullptr, &state_.show_log_panel);
        ImGui::MenuItem("Performance", nullptr, &state_.show_performance_panel);
        ImGui::EndMenu();
    }

    // Build progress indicator on the right side of the menu bar.
    if (is_build_running_.load())
    {
        const float spinner_radius = 5.0f * display_scale_;
        const float t = static_cast<float>(ImGui::GetTime());
        const int num_dots = 4;
        const int dot = static_cast<int>(t * 4.0f) % num_dots;
        std::string label = "Building";
        for (int i = 0; i < num_dots; ++i) { label += (i <= dot) ? '.' : ' '; }

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x
                             - ImGui::CalcTextSize(label.c_str()).x - spinner_radius * 2.0f - 8.0f);
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", label.c_str());
    }

    ImGui::EndMenuBar();
}

void EngineApplication::ApplyStyle()
{
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(display_scale_);
    style.FontScaleDpi = display_scale_;

    style.WindowRounding = 7.0f;
    style.ChildRounding = 7.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(10.0f, 6.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.49f, 0.73f, 0.00f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.35f, 0.45f, 0.20f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.13f, 0.13f, 0.14f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.13f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.22f, 0.23f, 0.24f, 1.00f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_Header] = ImVec4(0.18f, 0.19f, 0.20f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.30f, 0.10f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.41f, 0.08f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.17f, 0.17f, 0.18f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.30f, 0.10f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.41f, 0.08f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.16f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.22f, 0.14f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.29f, 0.12f, 1.00f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.49f, 0.73f, 0.00f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.49f, 0.73f, 0.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.60f, 0.85f, 0.10f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.14f, 0.14f, 0.15f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.30f, 0.10f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.27f, 0.35f, 0.09f, 1.00f);
    colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.49f, 0.73f, 0.00f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.12f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.11f, 0.11f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.29f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.38f, 0.10f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.36f, 0.46f, 0.10f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.26f, 0.31f, 0.14f, 1.00f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.35f, 0.44f, 0.11f, 1.00f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.43f, 0.55f, 0.10f, 1.00f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.30f, 0.38f, 0.10f, 0.75f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.39f, 0.50f, 0.10f, 0.90f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.49f, 0.63f, 0.10f, 1.00f);
    colors[ImGuiCol_TabDimmed] = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.22f, 0.27f, 0.10f, 1.00f);
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.49f, 0.73f, 0.00f, 1.00f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.49f, 0.73f, 0.00f, 0.35f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.32f, 0.41f, 0.10f, 0.50f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.49f, 0.73f, 0.00f, 0.90f);
    colors[ImGuiCol_NavCursor] = ImVec4(0.49f, 0.73f, 0.00f, 0.85f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(0.49f, 0.73f, 0.00f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.35f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.55f);
}

void EngineApplication::BuildDefaultDockLayout(ImGuiID dockspace_id)
{
    if (state_.dock_layout_built)
    {
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float bottom_bar_height = kBottomBarHeight * display_scale_;
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(viewport->Size.x, (std::max)(0.0f, viewport->Size.y - bottom_bar_height)));

    ImGuiID center_id = dockspace_id;
    ImGuiID left_id = 0;
    ImGuiID right_id = 0;
    ImGuiID bottom_id = 0;

    left_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Left, 0.22f, nullptr, &center_id);
    right_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.24f, nullptr, &center_id);
    bottom_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Down, 0.25f, nullptr, &center_id);

    ImGui::DockBuilderDockWindow("Version Control", left_id);
    ImGui::DockBuilderDockWindow("Files", left_id);
    ImGui::DockBuilderDockWindow("Workspace", center_id);
    ImGui::DockBuilderDockWindow("Effects", center_id);
    ImGui::DockBuilderDockWindow("Animator", center_id);
    ImGui::DockBuilderDockWindow("Sequencer", center_id);
    ImGui::DockBuilderDockWindow("Mapping", center_id);
    ImGui::DockBuilderDockWindow("Settings", center_id);
    ImGui::DockBuilderDockWindow("Info", right_id);
    ImGui::DockBuilderDockWindow("Performance", bottom_id);
    ImGui::DockBuilderDockWindow("Log", bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);

    state_.dock_layout_built = true;
    state_.AddLog("Created default dock layout");
}

void EngineApplication::HandleBuildRequests()
{
    // Drain log lines produced by the background build thread each frame.
    DrainBuildLog();
    state_.is_build_running = is_build_running_.load();

    if (state_.request_build_stop)
    {
        state_.request_build_stop = false;
        build_stop_requested_.store(true);
        CancelActiveBuildProcess();
    }

    // If a build is already running, don't start another.
    if (is_build_running_.load())
    {
        return;
    }

    // Join a finished thread before possibly starting a new one.
    if (build_thread_.joinable())
    {
        build_thread_.join();

        // Report the final result (success / error already in pending_build_error_).
        const bool ok = build_succeeded_.load();
        if (!ok)
        {
            std::string err;
            {
                std::lock_guard<std::mutex> lock(build_log_mutex_);
                err = pending_build_error_;
            }
            if (!err.empty())
            {
                state_.SetBuildError(err);
            }
        }
    }
    state_.is_build_running = is_build_running_.load();

    if (!state_.has_pending_build_request)
    {
        return;
    }

    const EngineBuildRequest request = state_.pending_build_request;
    state_.has_pending_build_request = false;

    // Validate on the main thread before handing off.
    if (state_.workspace_root.empty())
    {
        state_.SetBuildError("Cannot build game: workspace root is unavailable");
        return;
    }

    if (state_.project_file_path.empty())
    {
        state_.SetBuildError("Cannot build game: project manifest is unavailable");
        return;
    }

    const std::filesystem::path stage_directory = request.GetStageDirectory();
    const std::filesystem::path external_build_directory = GetExternalBuildDirectory(request);

    if (IsPathWithin(state_.workspace_root, stage_directory) || IsPathWithin(state_.workspace_root, external_build_directory))
    {
        state_.SetBuildError("Cannot build game: staged output and game build directory must be outside the engine workspace");
        return;
    }

    // Capture everything the thread needs by value.
    is_build_running_.store(true);
    state_.is_build_running = true;
    build_succeeded_.store(false);
    build_stop_requested_.store(false);
    {
        std::lock_guard<std::mutex> lock(build_log_mutex_);
        pending_build_error_.clear();
    }

    const std::filesystem::path project_root = state_.project_root;
    const std::filesystem::path active_scene_path = state_.active_scene_path;
    const std::filesystem::path project_file_path = state_.project_file_path;

    state_.AddLog("[Build] Starting background build for '" + request.game_name + "'...");

    build_thread_ = std::thread([this, request, project_root, active_scene_path, project_file_path]()
    {
        try
        {
            ExecuteBuildRequest(request, project_root, active_scene_path, project_file_path);
        }
        catch (const std::exception& ex)
        {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            pending_build_error_ = std::string("Build thread exception: ") + ex.what();
            pending_build_log_.push_back("[Build] FATAL ERROR: " + pending_build_error_);
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(build_log_mutex_);
            pending_build_error_ = "Build thread crashed with unknown exception";
            pending_build_log_.push_back("[Build] FATAL ERROR: " + pending_build_error_);
        }
        is_build_running_.store(false);
    });
}

void EngineApplication::DrainBuildLog()
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(build_log_mutex_);
        lines.swap(pending_build_log_);
    }
    for (const std::string& line : lines)
    {
        state_.AddLog(line);
    }
}

void EngineApplication::ExecuteBuildRequest(
    const EngineBuildRequest& request,
    const std::filesystem::path& project_root,
    const std::filesystem::path& active_scene_path,
    const std::filesystem::path& project_file_path)
{
    // Helper: push a log line from the build thread into the pending queue.
    const auto log = [this](const std::string& message)
    {
        std::lock_guard<std::mutex> lock(build_log_mutex_);
        pending_build_log_.push_back(message);
    };

    const auto fail = [this, &log](const std::string& message) -> bool
    {
        log("[Build] ERROR: " + message);
        std::lock_guard<std::mutex> lock(build_log_mutex_);
        pending_build_error_ = message;
        return false;
    };

    const auto was_cancelled = [this]()
    {
        return build_stop_requested_.load();
    };

    const std::string config_name = BuildTypeToConfigName(request.build_type);
    const std::filesystem::path external_build_directory = GetExternalBuildDirectory(request);
    const std::filesystem::path built_output_directory = GetBuiltOutputDirectory(request.build_platform, external_build_directory, config_name);
    const std::filesystem::path built_game_executable_path = GetBuiltGameExecutablePath(request.build_platform, built_output_directory);

    std::error_code error;
    std::filesystem::create_directories(external_build_directory, error);
    if (error)
    {
        fail("Failed to create external game build directory: " + external_build_directory.generic_string());
        return;
    }

    // Only configure when no cached build exists. On subsequent builds the cache
    // is reused, skipping the ~90s compiler/SDK detection phase. The user can
    // force a clean configure by deleting the build directory from the engine UI.
    const std::filesystem::path cmake_cache = external_build_directory / "CMakeCache.txt";
    bool needs_configure = !std::filesystem::exists(cmake_cache, error);
    error.clear();

    if (!needs_configure && request.build_platform == EngineBuildPlatform::Linux && LinuxBuildCacheNeedsRefresh(external_build_directory))
    {
        log("[Build] Linux build cache is stale or incomplete; recreating build directory");
        std::filesystem::remove_all(external_build_directory, error);
        error.clear();
        std::filesystem::create_directories(external_build_directory, error);
        if (error)
        {
            fail("Failed to recreate external game build directory: " + external_build_directory.generic_string());
            return;
        }
        needs_configure = true;
    }

    if (!needs_configure && request.build_platform == EngineBuildPlatform::Windows && WindowsGameIconCacheNeedsRefresh(external_build_directory, request.app_icon_path))
    {
        log("[Build] Build cache icon path changed; rerunning configure step");
        needs_configure = true;
    }

    if (was_cancelled())
    {
        fail("Game build cancelled");
        return;
    }

    if (needs_configure)
    {
        log("[Build] Configuring: " + external_build_directory.generic_string());
        std::string configure_command;
        if (request.build_platform == EngineBuildPlatform::Linux)
        {
            // Route through WSL so Linux toolchain commands run in the Linux environment.
            std::string inner =
                "cmake -S " + QuoteWslPath(state_.workspace_root) +
                " -B " + QuoteWslPath(external_build_directory) +
                " -DENGINE_BUILD_GAME=ON" +
                " --log-level=WARNING" +
                " -Wno-dev" +
                " -G \"Unix Makefiles\"" +
                " -DCMAKE_BUILD_TYPE=" + config_name +
                " -DCMAKE_C_COMPILER=gcc" +
                " -DCMAKE_CXX_COMPILER=g++";
            configure_command = "wsl bash -lc " + QuoteCommandArgument(inner);
        }
        else
        {
            std::string icon_path_for_cmake;
            if (!request.app_icon_path.empty())
            {
                const std::string icon_extension = ToLowerCopy(request.app_icon_path.extension().string());
                if (icon_extension == ".ico")
                {
                    icon_path_for_cmake = request.app_icon_path.lexically_normal().generic_string();
                }
            }

            configure_command =
                "cmake -S " + QuoteCommandArgument(state_.workspace_root.string()) +
                " -B " + QuoteCommandArgument(external_build_directory.string()) +
                " -DENGINE_BUILD_GAME=ON" +
                " -DGAME_WINDOWS_ICON_PATH=" + QuoteCommandArgument(icon_path_for_cmake) +
                " --log-level=WARNING" +
                " -Wno-dev";
        }

        const int configure_exit_code = RunCommand(configure_command, [&log](const std::string& line)
        {
            log("[cmake] " + line);
        }, &build_stop_requested_
#ifdef _WIN32
        , &active_build_process_mutex_, &active_build_process_
#endif
        );
        if (configure_exit_code == -2 || was_cancelled())
        {
            fail("Game build cancelled");
            return;
        }
        if (configure_exit_code != 0)
        {
            fail("Game configure failed with exit code " + std::to_string(configure_exit_code));
            return;
        }
    }
    else
    {
        log("[Build] Using cached configuration (skipping configure step)");
    }

    log("[Build] Compiling: platform=" + BuildPlatformToLogLabel(request.build_platform) + ", config=" + config_name);
    std::string build_command;
    if (request.build_platform == EngineBuildPlatform::Linux)
    {
        std::string inner =
            "cmake --build " + QuoteWslPath(external_build_directory) +
            " --target game" +
            " --parallel";
        build_command = "wsl bash -lc " + QuoteCommandArgument(inner);
    }
    else
    {
        build_command =
            "cmake --build " + QuoteCommandArgument(external_build_directory.string()) +
            " --config " + config_name +
            " --target game" +
            " --parallel" +
            " -- /v:m /nologo";
    }

    const int build_exit_code = RunCommand(build_command, [&log](const std::string& line)
    {
        log("[cmake] " + line);
    }, &build_stop_requested_
#ifdef _WIN32
    , &active_build_process_mutex_, &active_build_process_
#endif
    );
    if (build_exit_code == -2 || was_cancelled())
    {
        fail("Game build cancelled");
        return;
    }
    if (build_exit_code != 0)
    {
        fail("Game build failed with exit code " + std::to_string(build_exit_code));
        return;
    }

    if (!std::filesystem::exists(built_game_executable_path))
    {
        fail("Game build completed but " + built_game_executable_path.filename().generic_string() + " was not found in the build output");
        return;
    }

    if (was_cancelled())
    {
        fail("Game build cancelled");
        return;
    }

    std::string stage_error;
    if (!StageBuiltGame(request, project_root, active_scene_path, project_file_path, external_build_directory, built_output_directory, built_game_executable_path, log, stage_error))
    {
        fail(stage_error);
        return;
    }

    build_succeeded_.store(true);
    log("[Build] Done! Staged output: " + request.GetStageDirectory().generic_string());
    log("[Build] Build cache retained at: " + external_build_directory.generic_string() + " (delete to force a clean rebuild)");
}

bool EngineApplication::StageBuiltGame(
    const EngineBuildRequest& request,
    const std::filesystem::path& project_root,
    const std::filesystem::path& active_scene_path,
    const std::filesystem::path& project_file_path,
    const std::filesystem::path& external_build_directory,
    const std::filesystem::path& built_output_directory,
    const std::filesystem::path& built_game_executable_path,
    const std::function<void(const std::string&)>& log,
    std::string& out_error)
{
    const auto was_cancelled = [this]()
    {
        return build_stop_requested_.load();
    };

    std::filesystem::path content_root = project_root;
    std::error_code root_error;
    if (content_root.empty() || !std::filesystem::exists(content_root, root_error) || !std::filesystem::is_directory(content_root, root_error))
    {
        root_error.clear();
        if (!project_file_path.empty())
        {
            content_root = project_file_path.parent_path();
        }
    }
    if (content_root.empty() || !std::filesystem::exists(content_root, root_error) || !std::filesystem::is_directory(content_root, root_error))
    {
        out_error = "Cannot stage game: resolved project content root is invalid";
        return false;
    }

    const std::filesystem::path stage_directory = request.GetStageDirectory();
    const std::filesystem::path stage_game_executable_path = stage_directory / request.GetExecutableFileName();
    const std::filesystem::path stage_shader_directory = stage_directory / "shaders";
    const std::filesystem::path stage_vulkan_directory = stage_directory / "Vulkan";
    const std::filesystem::path built_shader_directory = built_output_directory / "shaders";
    const std::filesystem::path assets_pak_path = stage_directory / "assets.pak";
    const std::filesystem::path config_path = stage_directory / "config.ini";

    if (IsPathWithin(content_root, stage_directory))
    {
        out_error = "Cannot stage game output inside the source project directory";
        return false;
    }

    std::error_code relative_error;
    const std::filesystem::path startup_scene_relative_path = std::filesystem::relative(active_scene_path, content_root, relative_error);
    if (relative_error || startup_scene_relative_path.empty())
    {
        out_error = "Failed to compute startup scene path relative to the project root";
        return false;
    }

    std::error_code error;
    std::filesystem::remove_all(stage_directory, error);
    error.clear();
    std::filesystem::create_directories(stage_shader_directory, error);
    if (error)
    {
        out_error = "Failed to create staged output directory: " + stage_directory.generic_string();
        return false;
    }

    // -----------------------------------------------------------------
    // Pack all project content into assets.pak
    // -----------------------------------------------------------------
    log("[Build] Project root (captured): " + project_root.generic_string());
    log("[Build] Content root (resolved): " + content_root.generic_string());
    log("[Build] Packing project content into assets.pak...");
    PakArchive pak;

    std::vector<std::filesystem::path> pending_directories;
    pending_directories.push_back(content_root);
    std::size_t packed_file_count = 0;
    std::size_t packed_script_count = 0;
    std::string app_icon_rel_path;

    while (!pending_directories.empty())
    {
        if (was_cancelled())
        {
            out_error = "Game build cancelled";
            return false;
        }

        const std::filesystem::path current_directory = pending_directories.back();
        pending_directories.pop_back();

        std::error_code dir_error;
        std::filesystem::directory_iterator dir_it(
            current_directory,
            std::filesystem::directory_options::skip_permission_denied,
            dir_error);
        std::filesystem::directory_iterator dir_end;
        if (dir_error)
        {
            continue;
        }

        while (dir_it != dir_end)
        {
            if (was_cancelled())
            {
                out_error = "Game build cancelled";
                return false;
            }

            const std::filesystem::directory_entry entry = *dir_it;

            std::error_code advance_error;
            dir_it.increment(advance_error);
            if (advance_error)
            {
                continue;
            }

            const std::filesystem::path file_path = entry.path();

            std::error_code is_dir_error;
            if (entry.is_directory(is_dir_error) && !is_dir_error)
            {
                if (!ShouldSkipStagedProjectEntry(file_path, stage_directory, external_build_directory))
                {
                    pending_directories.push_back(file_path);
                }
                continue;
            }

            std::error_code regular_error;
            if (!entry.is_regular_file(regular_error) || regular_error)
            {
                continue;
            }

            if (ShouldSkipStagedProjectEntry(file_path, stage_directory, external_build_directory))
            {
                continue;
            }

            std::error_code rel_error;
            const std::filesystem::path rel_path = std::filesystem::relative(file_path, content_root, rel_error);
            if (rel_error || rel_path.empty())
            {
                continue;
            }

            // Skip build artefact directories inside the project folder
            const std::string first_component = rel_path.begin()->string();
            if (first_component == "Build" || first_component == "build" || first_component == ".vs")
            {
                continue;
            }

            // .graph documents are editor-only; the runtime in the built
            // game streams pre-transpiled Lua just like any other script.
            // We transpile each .graph here and stage ONLY the resulting
            // "<rel_path>.lua" companion into assets.pak (the .graph JSON
            // itself is intentionally not packed). RuntimeRenderer::
            // LoadGraphInstance loads the .lua directly via the VFS with
            // no graph parsing or registry lookups at runtime.
            const std::string rel_generic = rel_path.generic_string();
            const bool is_graph = ToLowerCopy(file_path.extension().string()) == ".graph";
            if (is_graph)
            {
                std::string transpiled_lua;
                std::string transpile_error;
                if (!graph::TranspileGraphFile(file_path, transpiled_lua, transpile_error))
                {
                    out_error = "Failed to transpile graph for pak: "
                        + file_path.generic_string() + ": " + transpile_error;
                    return false;
                }
                const std::string lua_rel_path = rel_generic + ".lua";
                std::vector<std::uint8_t> lua_bytes(transpiled_lua.begin(), transpiled_lua.end());
                if (!pak.AddBuffer(lua_rel_path, lua_bytes))
                {
                    out_error = "Failed to add transpiled graph to pak: " + lua_rel_path;
                    return false;
                }
                log("[Build] Transpiled graph: " + rel_generic + " -> " + lua_rel_path
                    + " (" + std::to_string(lua_bytes.size()) + " bytes)");
                // The transpiled .lua is what actually ships in the pak,
                // so account for it under script assets. The original
                // .graph is not packed (editor-only authoring format).
                ++packed_script_count;
                ++packed_file_count;
                continue;
            }

            if (!pak.AddFile(rel_generic, file_path))
            {
                out_error = "Failed to add file to pak: " + file_path.generic_string();
                return false;
            }

            if (rel_generic.rfind("Scripts/", 0) == 0)
            {
                ++packed_script_count;
            }

            ++packed_file_count;
        }
    }

    if (packed_file_count == 0)
    {
        out_error = "Packed 0 files from content root: " + content_root.generic_string();
        return false;
    }

    // -----------------------------------------------------------------
    // Pack builtin shapes referenced by Shape3D attributes in all scenes.
    // Shapes live in src/shapes/ which is outside the project content_root
    // and are not picked up by the directory walk above. We resolve them by
    // scanning every packed scene, then copy each unique shape file into the
    // pak under Shapes/<filename> so RuntimeRenderer can find it via the VFS.
    // -----------------------------------------------------------------
    {
        const std::filesystem::path engine_shapes_dir =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "shapes";

        std::unordered_set<std::string> seen_shape_files;

        // Collect every .scene file that was just packed.
        std::vector<std::filesystem::path> packed_scene_files;
        {
            std::vector<std::filesystem::path> scan_dirs;
            scan_dirs.push_back(content_root);
            while (!scan_dirs.empty())
            {
                const std::filesystem::path scan_dir = scan_dirs.back();
                scan_dirs.pop_back();
                std::error_code scan_err;
                std::filesystem::directory_iterator it(scan_dir,
                    std::filesystem::directory_options::skip_permission_denied, scan_err);
                for (; it != std::filesystem::directory_iterator(); it.increment(scan_err))
                {
                    if (scan_err) { break; }
                    const std::filesystem::path p = it->path();
                    if (it->is_directory(scan_err))
                    {
                        if (!ShouldSkipStagedProjectEntry(p, stage_directory, external_build_directory))
                        {
                            scan_dirs.push_back(p);
                        }
                    }
                    else if (it->is_regular_file(scan_err))
                    {
                        if (ToLowerCopy(p.extension().string()) == ".scene")
                        {
                            packed_scene_files.push_back(p);
                        }
                    }
                }
            }
        }

        for (const std::filesystem::path& scene_file : packed_scene_files)
        {
            const SceneMetadata scene_meta = LoadSceneMetadata(scene_file);
            if (!scene_meta.parsed)
            {
                continue;
            }
            for (const SceneObjectMetadata& obj : scene_meta.objects)
            {
                // Determine the shape filename: prefer the explicit attribute field,
                // fall back to the filename embedded in model_path for legacy scenes.
                std::string shape_file_name;
                for (const SceneObjectAttribute& attr : obj.attributes)
                {
                    if (attr.kind == SceneObjectAttributeKind::Shape3D)
                    {
                        if (!attr.shape_3d.shape_path.empty())
                        {
                            shape_file_name = attr.shape_3d.shape_path;
                        }
                        else if (!obj.model_path.empty())
                        {
                            shape_file_name = std::filesystem::path(obj.model_path).filename().string();
                        }
                        break;
                    }
                }

                if (shape_file_name.empty() || !seen_shape_files.insert(shape_file_name).second)
                {
                    continue;
                }

                const std::filesystem::path shape_src = engine_shapes_dir / shape_file_name;
                std::error_code shape_err;
                if (!std::filesystem::exists(shape_src, shape_err) || !std::filesystem::is_regular_file(shape_src, shape_err))
                {
                    log("[Build] Warning: builtin shape not found on disk, skipping: " + shape_src.generic_string());
                    continue;
                }

                const std::string pak_key = "Shapes/" + shape_file_name;
                if (!pak.AddFile(pak_key, shape_src))
                {
                    out_error = "Failed to add builtin shape to pak: " + pak_key;
                    return false;
                }

                ++packed_file_count;
                log("[Build] Added builtin shape: " + pak_key);
            }
        }
    }

    if (!request.app_icon_path.empty())
    {
        std::error_code icon_error;
        if (!std::filesystem::exists(request.app_icon_path, icon_error) || !std::filesystem::is_regular_file(request.app_icon_path, icon_error))
        {
            out_error = "App icon file does not exist: " + request.app_icon_path.generic_string();
            return false;
        }

        const std::string icon_extension = ToLowerCopy(request.app_icon_path.extension().string());
        if (icon_extension != ".png" && icon_extension != ".ico")
        {
            out_error = "App icon must be .png or .ico";
            return false;
        }

        app_icon_rel_path = "App/icon" + icon_extension;
        if (!pak.AddFile(app_icon_rel_path, request.app_icon_path))
        {
            out_error = "Failed to add app icon to pak: " + request.app_icon_path.generic_string();
            return false;
        }

        ++packed_file_count;
        log("[Build] Added app icon for streaming: " + app_icon_rel_path);
    }

    if (!pak.Write(assets_pak_path))
    {
        out_error = "Failed to write assets.pak: " + assets_pak_path.generic_string();
        return false;
    }

    if (was_cancelled())
    {
        out_error = "Game build cancelled";
        return false;
    }

    log("[Build] Packed " + std::to_string(packed_file_count) + " files into assets.pak");
    log("[Build] Included script assets: " + std::to_string(packed_script_count)
        + " (includes transpiled graphs)");

    // -----------------------------------------------------------------
    // Game executable
    // -----------------------------------------------------------------
    std::filesystem::copy_file(built_game_executable_path, stage_game_executable_path, std::filesystem::copy_options::overwrite_existing, error);
    if (error)
    {
        out_error = "Failed to stage game executable: " + stage_game_executable_path.generic_string();
        return false;
    }

    if (was_cancelled())
    {
        out_error = "Game build cancelled";
        return false;
    }

    // -----------------------------------------------------------------
    // Compiled shaders
    // -----------------------------------------------------------------
    if (std::filesystem::exists(built_shader_directory))
    {
        std::filesystem::copy(
            built_shader_directory,
            stage_shader_directory,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
            error);
        if (error)
        {
            out_error = "Failed to stage shader directory: " + stage_shader_directory.generic_string();
            return false;
        }
    }

    // -----------------------------------------------------------------
    // DLLs from the build output.
    // With full static linking (SDL static, assimp static) there should
    // be no DLLs for those libraries. Any Vulkan-related DLLs (e.g.
    // validation layers for Debug builds) go into a Vulkan/ subfolder;
    // all other DLLs go in the stage root.
    // -----------------------------------------------------------------
    std::error_code dll_iterator_error;
    std::filesystem::directory_iterator dll_it(
        built_output_directory,
        std::filesystem::directory_options::skip_permission_denied,
        dll_iterator_error);
    std::filesystem::directory_iterator dll_end;
    if (dll_iterator_error)
    {
        out_error = "Failed to enumerate build output directory: " + built_output_directory.generic_string();
        return false;
    }

    while (dll_it != dll_end)
    {
        const std::filesystem::directory_entry entry = *dll_it;

        std::error_code dll_advance_error;
        dll_it.increment(dll_advance_error);
        if (dll_advance_error)
        {
            dll_advance_error.clear();
        }

        std::error_code regular_error;
        if (!entry.is_regular_file(regular_error) || regular_error)
        {
            continue;
        }

        const std::filesystem::path file_path = entry.path();
        if (file_path.extension() != ".dll")
        {
            continue;
        }

        const std::string stem_lower = [&]()
        {
            std::string s = file_path.stem().string();
            for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            return s;
        }();

        const bool is_vulkan_dll =
            stem_lower.rfind("vklayer", 0) == 0 ||
            stem_lower.rfind("vulkan", 0) == 0;

        std::filesystem::path destination_path;
        if (is_vulkan_dll)
        {
            std::filesystem::create_directories(stage_vulkan_directory, error);
            error.clear();
            destination_path = stage_vulkan_directory / file_path.filename();
        }
        else
        {
            destination_path = stage_directory / file_path.filename();
        }

        std::filesystem::copy_file(file_path, destination_path, std::filesystem::copy_options::overwrite_existing, error);
        if (error)
        {
            out_error = "Failed to stage DLL: " + destination_path.generic_string();
            return false;
        }
    }

    std::ofstream config_output(config_path, std::ios::binary | std::ios::trunc);
    if (!config_output)
    {
        out_error = "Failed to write staged runtime config: " + config_path.generic_string();
        return false;
    }

    config_output
        << "projectId=" << (!project_file_path.empty() ? project_file_path.stem().string() : request.game_name) << "\n"
        << "buildId=" << BuildTypeToConfigName(request.build_type) << "\n"
        << "windowTitle=" << request.window_title << "\n"
        << "contentRoot=Content\n"
        << "scriptRoot=Scripts\n"
        << "graphRoot=Graphs\n"
        << "startupScene=" << startup_scene_relative_path.generic_string() << "\n";

    if (!app_icon_rel_path.empty())
    {
        config_output << "appIcon=" << app_icon_rel_path << "\n";
    }

    if (!config_output)
    {
        out_error = "Failed while writing staged runtime config: " + config_path.generic_string();
        return false;
    }

    return true;
}