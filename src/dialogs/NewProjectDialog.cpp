#include "dialogs/NewProjectDialog.h"

#include "imgui.h"

#include <fstream>
#include <system_error>
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

std::filesystem::path ShowNewProjectDialog()
{
    ScopedComInitialization com;
    if (FAILED(com.Result()))
    {
        return {};
    }

    IFileSaveDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog));
    if (FAILED(result) || dialog == nullptr)
    {
        return {};
    }

    DWORD options = 0;
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result))
    {
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT;
        dialog->SetOptions(options);
        dialog->SetTitle(L"Create Project Folder");
        dialog->SetOkButtonLabel(L"Create Project");
        dialog->SetFileName(L"NewProject");
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

void NewProjectDialog::Open()
{
    ImGui::OpenPopup("New Project");
}

bool NewProjectDialog::Render(EngineState& state)
{
    if (!ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return false;
    }

    ImGui::TextUnformatted("Create a new scaffolded project folder.");
    ImGui::TextUnformatted("Choose a project location in Explorer and type the folder name there.");
    ImGui::Separator();

    ImGui::TextWrapped("Selected folder: %s", selected_project_root_.empty() ? "None" : selected_project_root_.generic_string().c_str());
    if (ImGui::Button("Browse In Explorer"))
    {
        BrowseForProjectRoot(state);
    }

    bool created_project = false;
    if (ImGui::Button("Create"))
    {
        created_project = CreateProjectScaffold(selected_project_root_, state);
        if (created_project)
        {
            ClearSelection();
            ImGui::CloseCurrentPopup();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Cancel"))
    {
        ClearSelection();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    return created_project;
}

bool NewProjectDialog::BrowseForProjectRoot(EngineState& state)
{
#ifdef _WIN32
    const std::filesystem::path selected_path = ShowNewProjectDialog();
    if (selected_path.empty())
    {
        return false;
    }

    selected_project_root_ = selected_path;
    state.AddLog("Selected new project folder: " + selected_project_root_.generic_string());
    return true;
#else
    state.AddLog("Native new project browsing is only implemented on Windows");
    return false;
#endif
}

bool NewProjectDialog::CreateProjectScaffold(const std::filesystem::path& project_root, EngineState& state)
{
    if (project_root.empty())
    {
        state.AddLog("Cannot create project: choose a project folder in Explorer");
        return false;
    }

    if (std::filesystem::exists(project_root))
    {
        state.AddLog("Cannot create project: folder already exists: " + state.GetDisplayPath(project_root));
        return false;
    }

    const std::string project_name = project_root.filename().string();
    if (project_name.empty())
    {
        state.AddLog("Cannot create project: invalid folder name");
        return false;
    }

    const std::vector<std::filesystem::path> directories = {
        project_root / "Scenes",
        project_root / "Graphs",
        project_root / "Assets",
        project_root / "Assets" / "Models",
        project_root / "Assets" / "Images",
        project_root / "Assets" / "Fonts",
        project_root / "Scripts",
        project_root / "Config",
    };

    std::error_code error;
    for (const std::filesystem::path& directory : directories)
    {
        std::filesystem::create_directories(directory, error);
        if (error)
        {
            state.AddLog("Failed to create project directory: " + state.GetDisplayPath(directory));
            return false;
        }
    }

    const std::filesystem::path manifest_path = project_root / (project_name + ".engineproj");
    const std::filesystem::path scene_path = project_root / "Scenes" / "Main.scene";
    const std::filesystem::path config_path = project_root / "Config" / "editor.ini";
    const std::filesystem::path script_path = project_root / "Scripts" / "Game.cpp";

    {
        std::ofstream manifest_file(manifest_path, std::ios::binary | std::ios::trunc);
        if (!manifest_file)
        {
            state.AddLog("Failed to create project manifest: " + state.GetDisplayPath(manifest_path));
            return false;
        }

        manifest_file
            << "{\n"
            << "  \"name\": \"" << project_name << "\",\n"
            << "  \"type\": \"engine-project\",\n"
            << "  \"version\": 1,\n"
            << "  \"startupScene\": \"Scenes/Main.scene\"\n"
            << "}\n";
    }

    {
        std::ofstream scene_file(scene_path, std::ios::binary | std::ios::trunc);
        scene_file << "Scene: Main\n";
    }

    {
        std::ofstream config_file(config_path, std::ios::binary | std::ios::trunc);
        config_file << "[Editor]\nlastScene=Scenes/Main.scene\n";
    }

    {
        std::ofstream script_file(script_path, std::ios::binary | std::ios::trunc);
        script_file
            << "#include <iostream>\n\n"
            << "int main()\n"
            << "{\n"
            << "    std::cout << \"" << project_name << " script stub\\n\";\n"
            << "    return 0;\n"
            << "}\n";
    }

    state.AddLog("Created project scaffold: " + state.GetDisplayPath(project_root));
    return state.LoadProject(manifest_path);
}

void NewProjectDialog::ClearSelection()
{
    selected_project_root_.clear();
}