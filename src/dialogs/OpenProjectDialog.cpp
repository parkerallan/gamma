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
    ImGui::OpenPopup("Open Project");
}

bool OpenProjectDialog::Render(EngineState& state)
{
    if (!ImGui::BeginPopupModal("Open Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return false;
    }

    ImGui::TextUnformatted("Load a project folder or a .engineproj file.");
    ImGui::TextUnformatted("The project folder will appear in Files and the startup scene will open in Editor.");
    ImGui::Separator();

    ImGui::TextWrapped("Selected path: %s", path_buffer_[0] == '\0' ? "None" : path_buffer_.data());
    if (ImGui::Button("Browse Folder"))
    {
        BrowseForProjectFolder(state);
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse File"))
    {
        BrowseForProjectFile(state);
    }

    bool loaded = false;
    if (ImGui::Button("Open"))
    {
        loaded = TryLoadProject(state);
        if (loaded)
        {
            Reset();
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        Reset();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    return loaded;
}

bool OpenProjectDialog::TryLoadProject(EngineState& state)
{
    std::filesystem::path requested_path(path_buffer_.data());
    if (requested_path.empty())
    {
        state.AddLog("Cannot open project: choose a project folder or .engineproj file");
        return false;
    }

    if (requested_path.is_relative() && !state.workspace_root.empty())
    {
        requested_path = state.workspace_root / requested_path;
    }

    requested_path = requested_path.lexically_normal();
    const std::filesystem::path manifest_path = ResolveManifestPath(requested_path);
    if (manifest_path.empty())
    {
        state.AddLog("Cannot open project: no .engineproj file found at path");
        return false;
    }

    return state.LoadProject(manifest_path);
}

bool OpenProjectDialog::BrowseForProjectFolder(EngineState& state)
{
#ifdef _WIN32
    const std::filesystem::path selected_path = ShowNativeOpenDialog(true);
    if (selected_path.empty())
    {
        return false;
    }

    SetSelectedPath(selected_path);
    state.AddLog("Selected project folder: " + selected_path.generic_string());
    return true;
#else
    state.AddLog("Folder browsing is only implemented on Windows");
    return false;
#endif
}

bool OpenProjectDialog::BrowseForProjectFile(EngineState& state)
{
#ifdef _WIN32
    const std::filesystem::path selected_path = ShowNativeOpenDialog(false);
    if (selected_path.empty())
    {
        return false;
    }

    SetSelectedPath(selected_path);
    state.AddLog("Selected project file: " + selected_path.generic_string());
    return true;
#else
    state.AddLog("Project file browsing is only implemented on Windows");
    return false;
#endif
}

void OpenProjectDialog::SetSelectedPath(const std::filesystem::path& path)
{
    const std::string value = path.generic_string();
    std::fill(path_buffer_.begin(), path_buffer_.end(), '\0');
    const std::size_t length = (std::min)(value.size(), path_buffer_.size() - 1);
    std::copy_n(value.begin(), static_cast<std::ptrdiff_t>(length), path_buffer_.begin());
}

void OpenProjectDialog::Reset()
{
    std::fill(path_buffer_.begin(), path_buffer_.end(), '\0');
}