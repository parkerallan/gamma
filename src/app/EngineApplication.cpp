#include "app/EngineApplication.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "imgui_internal.h"

#include <filesystem>
#include <string>

namespace
{
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
}

bool EngineApplication::Init()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    display_scale_ = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

    const SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
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

    gpu_device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan");
    if (gpu_device_ == nullptr)
    {
        SDL_Log("SDL_CreateGPUDevice failed: %s", SDL_GetError());
        return false;
    }

    if (!SDL_ClaimWindowForGPUDevice(gpu_device_, window_))
    {
        SDL_Log("SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return false;
    }

    SDL_SetGPUSwapchainParameters(gpu_device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
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

    ImGui_ImplSDL3_InitForSDLGPU(window_);
    ImGui_ImplSDLGPU3_InitInfo init_info = {};
    init_info.Device = gpu_device_;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu_device_, window_);
    init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    ImGui_ImplSDLGPU3_Init(&init_info);

    if (io.Fonts->AddFontDefaultVector() == nullptr)
    {
        io.Fonts->AddFontDefaultBitmap();
    }

    if (!workspace_panel_.InitializeSceneRenderer(gpu_device_, init_info.ColorTargetFormat))
    {
        SDL_Log("Scene viewport renderer initialization failed: %s", SDL_GetError());
    }

    state_.SetWorkspaceRoot(ResolveWorkspaceRoot());
    state_.AddLog("Workspace root: " + state_.workspace_root.generic_string());
    state_.AddLog("Engine started");
    running_ = true;
    return true;
}

void EngineApplication::RunLoop()
{
    while (running_)
    {
        ProcessEvents();

        if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        workspace_panel_.BeginFrame();
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        RenderUI();

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f;

        SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(gpu_device_);
        workspace_panel_.RenderSceneGpuPass(command_buffer);
        SDL_GPUTexture* swapchain_texture = nullptr;
        SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, window_, &swapchain_texture, nullptr, nullptr);

        if (swapchain_texture != nullptr && !is_minimized)
        {
            ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);

            SDL_GPUColorTargetInfo target_info = {};
            target_info.texture = swapchain_texture;
            target_info.clear_color = SDL_FColor{0.08f, 0.09f, 0.11f, 1.0f};
            target_info.load_op = SDL_GPU_LOADOP_CLEAR;
            target_info.store_op = SDL_GPU_STOREOP_STORE;
            target_info.mip_level = 0;
            target_info.layer_or_depth_plane = 0;
            target_info.cycle = false;

            SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);
            SDL_EndGPURenderPass(render_pass);
        }

        SDL_SubmitGPUCommandBuffer(command_buffer);
    }
}

void EngineApplication::Shutdown()
{
    workspace_panel_.Shutdown();
    info_panel_.Shutdown();

    if (gpu_device_ != nullptr)
    {
        SDL_WaitForGPUIdle(gpu_device_);
    }

    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (gpu_device_ != nullptr && window_ != nullptr)
    {
        SDL_ReleaseWindowFromGPUDevice(gpu_device_, window_);
    }

    if (gpu_device_ != nullptr)
    {
        SDL_DestroyGPUDevice(gpu_device_);
        gpu_device_ = nullptr;
    }

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
    info_panel_.Render(state_, nullptr);
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