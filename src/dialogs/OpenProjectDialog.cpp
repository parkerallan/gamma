#include "dialogs/OpenProjectDialog.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ShObjIdl.h>
#include <combaseapi.h>
#include <windows.h>
#endif

namespace
{
std::filesystem::path ResolveManifestPath(const std::filesystem::path& input_path)
{
    if (input_path.empty())
    {
        return {};
    }

    if (input_path.extension() == ".engineproj")
    {
        return input_path;
    }

    if (!std::filesystem::is_directory(input_path))
    {
        return {};
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(input_path))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".engineproj")
        {
            return entry.path();
        }
    }

    return {};
}

#ifdef _WIN32
class ScopedComInitialization
{
public:
    ScopedComInitialization()
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))
    {
    }

    ~ScopedComInitialization()
    {
        if (SUCCEEDED(result_))
        {
            CoUninitialize();
        }
    }

    HRESULT Result() const
    {
        return result_;
    }

private:
    HRESULT result_;
};

std::filesystem::path ShowNativeOpenDialog(bool pick_folder)
{
    ScopedComInitialization com;
    if (FAILED(com.Result()))
    {
        return {};
    }

    IFileDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog));
    if (FAILED(result) || dialog == nullptr)
    {
        return {};
    }

    DWORD options = 0;
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result))
    {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (pick_folder)
        {
            options |= FOS_PICKFOLDERS;
            dialog->SetTitle(L"Open Project Folder");
        }
        else
        {
            COMDLG_FILTERSPEC filters[] = {
                {L"Engine Project", L"*.engineproj"},
                {L"All Files", L"*.*"},
            };
            dialog->SetTitle(L"Open Project File");
            dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
            dialog->SetFileTypeIndex(1);
        }
        dialog->SetOptions(options);
    }

    std::filesystem::path selected_path;
    result = dialog->Show(nullptr);
    if (SUCCEEDED(result))
    {
        IShellItem* shell_item = nullptr;
        result = dialog->GetResult(&shell_item);
        if (SUCCEEDED(result) && shell_item != nullptr)
        {
            PWSTR display_name = nullptr;
            result = shell_item->GetDisplayName(SIGDN_FILESYSPATH, &display_name);
            if (SUCCEEDED(result) && display_name != nullptr)
            {
                selected_path = display_name;
                CoTaskMemFree(display_name);
            }
            shell_item->Release();
        }
    }

    dialog->Release();
    return selected_path;
}
#endif
}

void OpenProjectDialog::Open()
{
    is_open_ = true;
    ImGui::OpenPopup("Open Project");
}

bool OpenProjectDialog::Render(EngineState& state)
{
    if (!is_open_)
    {
        return false;
    }

    ImGui::SetNextWindowSize(ImVec2(640.0f, 0.0f), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Open Project", &is_open_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    {
        return false;
    }

    bool loaded = false;

    // Browse buttons — selection opens the project immediately
    if (ImGui::Button("Browse Folder"))
    {
        loaded = BrowseAndLoadFolder(state);
        if (loaded)
        {
            is_open_ = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse File"))
    {
        loaded = BrowseAndLoadFile(state);
        if (loaded)
        {
            is_open_ = false;
            ImGui::CloseCurrentPopup();
        }
    }

    // Recent projects list
    ImGui::Spacing();
    ImGui::SeparatorText("Recent Projects");

    if (state.recent_projects.empty())
    {
        ImGui::TextDisabled("No recent projects.");
    }
    else
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 4.0f));
        const float line_height    = ImGui::GetTextLineHeight();
        const float item_spacing_y = ImGui::GetStyle().ItemSpacing.y;
        const float item_height    = line_height * 2.0f + item_spacing_y;
        const float button_w       = ImGui::GetFrameHeight();
        const float list_height    = (item_height + item_spacing_y) *
            static_cast<float>((std::min)(state.recent_projects.size(), std::size_t{8})) + 8.0f;

        if (ImGui::BeginChild("##recent_list", ImVec2(-FLT_MIN, list_height), ImGuiChildFlags_FrameStyle))
        {
            std::filesystem::path path_to_open;
            std::size_t index_to_remove = state.recent_projects.size();

            for (std::size_t i = 0; i < state.recent_projects.size(); ++i)
            {
                const std::filesystem::path& proj_path = state.recent_projects[i];
                std::error_code ec;
                const bool exists = std::filesystem::exists(proj_path, ec);

                const std::string display_name = proj_path.stem().string();
                const std::string display_path = proj_path.parent_path().generic_string();

                ImGui::PushID(static_cast<int>(i));

                const ImVec2 cursor_start        = ImGui::GetCursorPos();
                const ImVec2 cursor_screen_start = ImGui::GetCursorScreenPos();
                const float  selectable_w        = ImGui::GetContentRegionAvail().x - button_w - ImGui::GetStyle().ItemSpacing.x;

                const bool clicked = ImGui::Selectable(
                    "##sel",
                    false,
                    ImGuiSelectableFlags_AllowOverlap,
                    ImVec2(selectable_w, item_height));

                // Draw name + path text directly over the selectable area
                const ImU32 text_color    = exists
                    ? ImGui::GetColorU32(ImGuiCol_Text)
                    : ImGui::GetColorU32(ImGuiCol_TextDisabled);
                const ImU32 subtext_color = ImGui::GetColorU32(ImGuiCol_TextDisabled);

                ImDrawList* draw_list = ImGui::GetWindowDrawList();
                draw_list->AddText(
                    ImVec2(cursor_screen_start.x + 6.0f, cursor_screen_start.y + 2.0f),
                    text_color,
                    display_name.c_str());
                draw_list->AddText(
                    ImVec2(cursor_screen_start.x + 6.0f, cursor_screen_start.y + line_height + 4.0f),
                    subtext_color,
                    display_path.c_str());

                // x button, vertically centred
                ImGui::SameLine();
                ImGui::SetCursorPosY(cursor_start.y + item_height * 0.5f - ImGui::GetFrameHeight() * 0.5f);
                if (ImGui::Button("x", ImVec2(button_w, ImGui::GetFrameHeight())))
                {
                    index_to_remove = i;
                }

                if (clicked && exists)
                {
                    path_to_open = proj_path;
                }

                ImGui::PopID();
            }

            if (index_to_remove < state.recent_projects.size())
            {
                state.recent_projects.erase(state.recent_projects.begin() + static_cast<std::ptrdiff_t>(index_to_remove));
                state.SaveRecentProjects();
            }

            if (!path_to_open.empty())
            {
                loaded = LoadPath(path_to_open, state);
                if (loaded)
                {
                    is_open_ = false;
                    ImGui::EndChild();
                    ImGui::PopStyleVar();
                    ImGui::EndPopup();
                    return true;
                }
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    ImGui::EndPopup();
    return loaded;
}

bool OpenProjectDialog::LoadPath(const std::filesystem::path& path, EngineState& state)
{
    std::filesystem::path resolved = path.lexically_normal();
    if (resolved.is_relative() && !state.workspace_root.empty())
    {
        resolved = state.workspace_root / resolved;
    }

    const std::filesystem::path manifest_path = ResolveManifestPath(resolved);
    if (manifest_path.empty())
    {
        state.AddLog("Cannot open project: no .engineproj file found at path");
        return false;
    }

    return state.LoadProject(manifest_path);
}

bool OpenProjectDialog::BrowseAndLoadFolder(EngineState& state)
{
#ifdef _WIN32
    const std::filesystem::path selected_path = ShowNativeOpenDialog(true);
    if (selected_path.empty())
    {
        return false;
    }
    return LoadPath(selected_path, state);
#else
    state.AddLog("Folder browsing is only implemented on Windows");
    return false;
#endif
}

bool OpenProjectDialog::BrowseAndLoadFile(EngineState& state)
{
#ifdef _WIN32
    const std::filesystem::path selected_path = ShowNativeOpenDialog(false);
    if (selected_path.empty())
    {
        return false;
    }
    return LoadPath(selected_path, state);
#else
    state.AddLog("Project file browsing is only implemented on Windows");
    return false;
#endif
}