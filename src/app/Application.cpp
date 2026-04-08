#include "app/EditorApplication.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
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

bool EditorApplication::Init()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    display_scale_ = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

    const SDL_WindowFlags window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    window_ = SDL_CreateWindow(
        "Engine Editor Skeleton",
        static_cast<int>(1600.0f * display_scale_),
        static_cast<int>(900.0f * display_scale_),
        window_flags);

    if (window_ == nullptr)
    {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    renderer_ = SDL_CreateRenderer(window_, nullptr);
    if (renderer_ == nullptr)
    {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }

    SDL_SetRenderVSync(renderer_, 1);
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window_);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;

    ApplyStyle();

    ImGui_ImplSDL3_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer3_Init(renderer_);

    if (io.Fonts->AddFontDefaultVector() == nullptr)
    {
        io.Fonts->AddFontDefaultBitmap();
    }

    state_.SetWorkspaceRoot(ResolveWorkspaceRoot());
    state_.AddLog("Workspace root: " + state_.workspace_root.generic_string());
    state_.AddLog("Editor started");
    running_ = true;
    return true;
}

void EditorApplication::RunLoop()
{
    while (running_)
    {
        ProcessEvents();

        if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)
        {
            SDL_Delay(10);
            continue;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        RenderUI();

        ImGui::Render();
        ImGuiIO& io = ImGui::GetIO();

        SDL_SetRenderScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(renderer_, 0.08f, 0.09f, 0.11f, 1.0f);
        SDL_RenderClear(renderer_);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }
}

void EditorApplication::Shutdown()
{
    workspace_panel_.Shutdown();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    if (renderer_ != nullptr)
    {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }

    if (window_ != nullptr)
    {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    SDL_Quit();
}

void EditorApplication::ProcessEvents()
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
    }
}

void EditorApplication::RenderUI()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_window_flags = ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("EditorDockHost", nullptr, host_window_flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspace_id = ImGui::GetID("EditorDockSpace");
    BuildDefaultDockLayout(dockspace_id);
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    ImGui::End();

    files_panel_.Render(state_);
    workspace_panel_.Render(state_);
    settings_panel_.Render(state_);
    log_panel_.Render(state_);
}

void EditorApplication::ApplyStyle()
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

void EditorApplication::BuildDefaultDockLayout(ImGuiID dockspace_id)
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
    ImGui::DockBuilderDockWindow("Settings", right_id);
    ImGui::DockBuilderDockWindow("Log", bottom_id);
    ImGui::DockBuilderFinish(dockspace_id);

    state_.dock_layout_built = true;
    state_.AddLog("Created default dock layout");
}