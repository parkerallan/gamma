#include "components/BuildSettingsComponent.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

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
constexpr std::size_t kNameCapacity = 128;
constexpr std::size_t kTitleCapacity = 256;
constexpr std::size_t kPathCapacity = 512;

bool EditStringField(const char* label, std::string& value, std::size_t capacity)
{
    std::vector<char> buffer(capacity, '\0');
    const std::size_t copy_size = (std::min)(value.size(), buffer.size() - 1);
    std::copy_n(value.begin(), static_cast<std::ptrdiff_t>(copy_size), buffer.begin());

    if (!ImGui::InputText(label, buffer.data(), buffer.size()))
    {
        return false;
    }

    value = buffer.data();
    return true;
}

bool EditPathField(const char* label, std::filesystem::path& value)
{
    std::string string_value = value.generic_string();
    if (!EditStringField(label, string_value, kPathCapacity))
    {
        return false;
    }

    value = std::filesystem::path(string_value);
    return true;
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

std::filesystem::path ShowFolderDialog()
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
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_PICKFOLDERS;
        dialog->SetOptions(options);
        dialog->SetTitle(L"Choose Game Build Output Folder");
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

std::filesystem::path ShowIconFileDialog()
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
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST;
        dialog->SetOptions(options);
        dialog->SetTitle(L"Choose App Icon");

        COMDLG_FILTERSPEC filters[] = {
            {L"App Icon (*.png;*.ico)", L"*.png;*.ico"},
            {L"All Files", L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetFileTypeIndex(1);
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

bool BuildSettingsComponent::Render(EngineState& state)
{
    bool changed = false;

    changed |= EditStringField("Exe name", state.build_executable_name, kNameCapacity);
    changed |= EditStringField("Folder name", state.build_folder_name, kNameCapacity);
    changed |= EditStringField("Window title", state.build_window_title, kTitleCapacity);

    int build_type_index = state.build_target_type == EngineBuildType::Debug ? 0 : 1;
    const char* build_type_labels[] = {"Debug", "Final"};
    if (ImGui::Combo("Build type", &build_type_index, build_type_labels, IM_ARRAYSIZE(build_type_labels)))
    {
        state.build_target_type = build_type_index == 0 ? EngineBuildType::Debug : EngineBuildType::Final;
        changed = true;
    }

    int platform_index = state.build_target_platform == EngineBuildPlatform::Windows ? 0 : 1;
    const char* platform_labels[] = {"Windows (MSVC)", "Linux (GCC)"};
    if (ImGui::Combo("Build platform", &platform_index, platform_labels, IM_ARRAYSIZE(platform_labels)))
    {
        state.build_target_platform = platform_index == 0 ? EngineBuildPlatform::Windows : EngineBuildPlatform::Linux;
        changed = true;
    }

    changed |= EditPathField("Build location", state.build_output_root);
    ImGui::SameLine();
    if (ImGui::Button("Browse Folder"))
    {
#ifdef _WIN32
        const std::filesystem::path selected_path = ShowFolderDialog();
        if (!selected_path.empty())
        {
            state.build_output_root = selected_path.lexically_normal();
            state.AddLog("Selected game build output folder: " + state.GetDisplayPath(selected_path));
            changed = true;
        }
#else
        state.SetBuildError("Build output folder browsing is only implemented on Windows");
#endif
    }

    changed |= EditPathField("App icon", state.build_app_icon_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse Icon"))
    {
#ifdef _WIN32
        const std::filesystem::path selected_path = ShowIconFileDialog();
        if (!selected_path.empty())
        {
            state.build_app_icon_path = selected_path.lexically_normal();
            state.AddLog("Selected app icon: " + state.GetDisplayPath(selected_path));
            changed = true;
        }
#else
        state.SetBuildError("App icon browsing is only implemented on Windows");
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Icon"))
    {
        state.build_app_icon_path.clear();
        changed = true;
    }

    ImGui::TextDisabled("Supported icon formats: .png, .ico");
    return changed;
}
