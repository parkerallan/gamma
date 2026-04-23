#include "app/EngineApplication.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "imgui_internal.h"
#include "pak/PakArchive.h"
#include "ui/Codicons.h"

#include <array>
#include <cctype>
#include <filesystem>
#include <functional>
#include <system_error>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{
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
int RunCommand(const std::string& command, const std::function<void(const std::string&)>& line_cb)
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

    std::string line;
    char ch = '\0';
    DWORD bytes_read = 0;
    while (ReadFile(read_end, &ch, 1, &bytes_read, nullptr) && bytes_read > 0)
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

    CloseHandle(read_end);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
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
        "Engine",
        static_cast<int>(1600.0f * display_scale_),
        static_cast<int>(900.0f * display_scale_),
        window_flags);

    if (window_ == nullptr)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    if (!vulkan_context_.Initialize(window_))
    {
        return false;
    }
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);

    const std::filesystem::path workspace_root = ResolveWorkspaceRoot();

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
    state_.AddLog("Engine started");
    running_ = true;
    return true;
}

void EngineApplication::RunLoop()
{
    while (running_)
    {
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

    const SceneMetadata scene_metadata = LoadSceneMetadata(state_.active_scene_path);
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
        "Runtime",
        static_cast<int>(1280.0f * display_scale_),
        static_cast<int>(720.0f * display_scale_),
        window_flags);
    if (runtime_window_ == nullptr)
    {
        state_.SetPlayError(std::string("Failed to create runtime window: ") + SDL_GetError());
        return false;
    }

    if (!vulkan_context_.CreateWindowContext(runtime_window_, runtime_window_context_))
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

    SDL_SetWindowPosition(runtime_window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(runtime_window_);
    state_.is_playing = true;
    state_.playing_scene_path = state_.active_scene_path;
    state_.last_play_error.clear();
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

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
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

    files_panel_.Render(state_);
    workspace_panel_.Render(state_);
    settings_panel_.Render(state_);
    info_panel_.Render(state_, &vulkan_context_);
    log_panel_.Render(state_);
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
        if (ImGui::MenuItem("Build", "Ctrl+B", false, state_.CanBuildProject() && !build_running))
        {
            state_.TriggerBuildAction();
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
        ImGui::MenuItem("Workspace", nullptr, &state_.show_workspace_panel);
        ImGui::MenuItem("Settings", nullptr, &state_.show_settings_panel);
        ImGui::MenuItem("Info", nullptr, &state_.show_info_panel);
        ImGui::MenuItem("Log", nullptr, &state_.show_log_panel);
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
    colors[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.13f, 0.16f, 0.98f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.26f, 0.33f, 1.00f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.34f, 0.43f, 1.00f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.29f, 0.38f, 0.49f, 1.00f);
    colors[ImGuiCol_Button] = ImVec4(0.19f, 0.24f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.33f, 0.41f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.31f, 0.40f, 0.50f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.13f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.23f, 0.29f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.29f, 0.36f, 1.00f);
    colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.19f, 0.24f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.23f, 0.30f, 0.38f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.28f, 0.38f, 0.47f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.36f, 0.52f, 0.67f, 0.70f);
}

void EngineApplication::BuildDefaultDockLayout(ImGuiID dockspace_id)
{
    if (state_.dock_layout_built)
    {
        return;
    }

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->Size);

    ImGuiID center_id = dockspace_id;
    ImGuiID left_id = 0;
    ImGuiID right_id = 0;
    ImGuiID bottom_id = 0;

    left_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Left, 0.22f, nullptr, &center_id);
    right_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Right, 0.24f, nullptr, &center_id);
    bottom_id = ImGui::DockBuilderSplitNode(center_id, ImGuiDir_Down, 0.25f, nullptr, &center_id);

    ImGui::DockBuilderDockWindow("Files", left_id);
    ImGui::DockBuilderDockWindow("Workspace", center_id);
    ImGui::DockBuilderDockWindow("Settings", center_id);
    ImGui::DockBuilderDockWindow("Info", right_id);
    ImGui::DockBuilderDockWindow("Log", bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);

    state_.dock_layout_built = true;
    state_.AddLog("Created default dock layout");
}

void EngineApplication::HandleBuildRequests()
{
    // Drain log lines produced by the background build thread each frame.
    DrainBuildLog();

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
    const std::filesystem::path external_build_directory = request.output_root / (request.game_name + "-build");

    if (IsPathWithin(state_.workspace_root, stage_directory) || IsPathWithin(state_.workspace_root, external_build_directory))
    {
        state_.SetBuildError("Cannot build game: staged output and game build directory must be outside the engine workspace");
        return;
    }

    // Capture everything the thread needs by value.
    is_build_running_.store(true);
    build_succeeded_.store(false);
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

    const std::string config_name = BuildTypeToConfigName(request.build_type);
    const std::filesystem::path external_build_directory = request.output_root / (request.game_name + "-build");
    const std::filesystem::path built_output_directory = external_build_directory / config_name;
    const std::filesystem::path built_game_executable_path = built_output_directory / "game.exe";

    std::error_code error;
    std::filesystem::create_directories(external_build_directory, error);
    if (error)
    {
        fail("Failed to create external game build directory: " + external_build_directory.generic_string());
        return;
    }

    // Delete stale CMakeCache.txt so changed cache variables (e.g.
    // ASSIMP_USE_STATIC_CRT) are picked up on every configure.
    const std::filesystem::path cmake_cache = external_build_directory / "CMakeCache.txt";
    if (std::filesystem::exists(cmake_cache, error))
    {
        std::filesystem::remove(cmake_cache, error);
        error.clear();
    }

    log("[Build] Configuring: " + external_build_directory.generic_string());
    const std::string configure_command =
        "cmake -S " + QuoteCommandArgument(state_.workspace_root.string()) +
        " -B " + QuoteCommandArgument(external_build_directory.string()) +
        " -DENGINE_BUILD_GAME=ON";

    const int configure_exit_code = RunCommand(configure_command, [&log](const std::string& line)
    {
        log("[cmake] " + line);
    });
    if (configure_exit_code != 0)
    {
        fail("Game configure failed with exit code " + std::to_string(configure_exit_code));
        return;
    }

    log("[Build] Compiling: config=" + config_name);
    const std::string build_command =
        "cmake --build " + QuoteCommandArgument(external_build_directory.string()) +
        " --config " + config_name +
        " --target game";

    const int build_exit_code = RunCommand(build_command, [&log](const std::string& line)
    {
        log("[cmake] " + line);
    });
    if (build_exit_code != 0)
    {
        fail("Game build failed with exit code " + std::to_string(build_exit_code));
        return;
    }

    if (!std::filesystem::exists(built_game_executable_path))
    {
        fail("Game build completed but game.exe was not found in the build output");
        return;
    }

    std::string stage_error;
    if (!StageBuiltGame(request, project_root, active_scene_path, project_file_path, external_build_directory, built_output_directory, built_game_executable_path, log, stage_error))
    {
        fail(stage_error);
        return;
    }

    std::error_code cleanup_error;
    std::filesystem::remove_all(external_build_directory, cleanup_error);
    if (cleanup_error)
    {
        log("[Build] Warning: failed to remove temporary build directory: " + external_build_directory.generic_string());
    }
    else
    {
        log("[Build] Removed temporary build directory: " + external_build_directory.generic_string());
    }

    build_succeeded_.store(true);
    log("[Build] Done! Staged output: " + request.GetStageDirectory().generic_string());
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
    const std::filesystem::path stage_game_executable_path = stage_directory / (request.game_name + ".exe");
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
    std::size_t packed_graph_count = 0;

    while (!pending_directories.empty())
    {
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

            if (!pak.AddFile(rel_path.generic_string(), file_path))
            {
                out_error = "Failed to add file to pak: " + file_path.generic_string();
                return false;
            }

            const std::string rel_generic = rel_path.generic_string();
            if (rel_generic.rfind("Scripts/", 0) == 0)
            {
                ++packed_script_count;
            }
            else if (rel_generic.rfind("Graphs/", 0) == 0)
            {
                ++packed_graph_count;
            }

            ++packed_file_count;
        }
    }

    if (packed_file_count == 0)
    {
        out_error = "Packed 0 files from content root: " + content_root.generic_string();
        return false;
    }

    if (!pak.Write(assets_pak_path))
    {
        out_error = "Failed to write assets.pak: " + assets_pak_path.generic_string();
        return false;
    }

    log("[Build] Packed " + std::to_string(packed_file_count) + " files into assets.pak");
    log("[Build] Included script assets: " + std::to_string(packed_script_count));
    log("[Build] Included graph assets: " + std::to_string(packed_graph_count));

    // -----------------------------------------------------------------
    // Game executable
    // -----------------------------------------------------------------
    std::filesystem::copy_file(built_game_executable_path, stage_game_executable_path, std::filesystem::copy_options::overwrite_existing, error);
    if (error)
    {
        out_error = "Failed to stage game executable: " + stage_game_executable_path.generic_string();
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
        << "windowTitle=" << request.game_name << "\n"
        << "contentRoot=Content\n"
        << "scriptRoot=Scripts\n"
        << "graphRoot=Graphs\n"
        << "startupScene=" << startup_scene_relative_path.generic_string() << "\n";

    if (!config_output)
    {
        out_error = "Failed while writing staged runtime config: " + config_path.generic_string();
        return false;
    }

    return true;
}