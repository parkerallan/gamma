#include "components/CreationMenu.h"

#include "imgui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

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
using json = nlohmann::json;

std::string TrimName(std::string value)
{
    const auto is_space = [](unsigned char character)
    {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char character)
    {
        return !is_space(character);
    }));

    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char character)
    {
        return !is_space(character);
    }).base(), value.end());

    return value;
}

std::string SanitizeName(std::string value)
{
    value = TrimName(std::move(value));
    std::replace(value.begin(), value.end(), ' ', '_');
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char character)
    {
        return character == '<' ||
            character == '>' ||
            character == ':' ||
            character == '"' ||
            character == '/' ||
            character == '\\' ||
            character == '|' ||
            character == '?' ||
            character == '*';
    }), value.end());
    return value;
}

std::string BuildScriptStub(const std::string& file_stem)
{
    return std::string("-- Script: ") + file_stem + "\n" +
        "local script = {}\n\n" +
        "function script:OnCreate(entity)\n" +
    "    Engine.Log(\"" + file_stem + " created for \" .. tostring(entity))\n" +
        "    World.Subscribe(\"Ping\", self.OnPing)\n" +
        "end\n\n" +
        "function script:OnPing(sender, payload)\n" +
        "    Engine.Log(\"Ping from \" .. tostring(sender) .. \" payload=\" .. tostring(payload))\n" +
        "end\n\n" +
        "function script:OnUpdate(entity, delta_time)\n" +
        "    if Input.WasKeyPressed(\"P\") then\n" +
        "        World.Emit(\"Ping\", \"time=\" .. tostring(Time.TotalTime))\n" +
        "    end\n" +
        "end\n\n" +
        "function script:OnDestroy(entity)\n" +
        "    Engine.Log(\"" + file_stem + " destroyed for \" .. tostring(entity))\n" +
        "end\n\n" +
        "return script\n";
}

std::string BuildGraphStub(const std::string& graph_name)
{
    return std::string("{\n") +
        "  \"name\": \"" + graph_name + "\",\n" +
        "  \"nodes\": [],\n" +
        "  \"links\": []\n" +
        "}\n";
}

std::string BuildSceneStub(const std::string& scene_name)
{
    return "Scene: " + scene_name + "\n";
}

std::string BuildObjectStub(const std::string& object_name)
{
    return "\nObject: " + object_name + "\n"
        "Position: 0, 0, 0\n"
        "Rotation: 0, 0, 0\n"
        "Scale: 1, 1, 1\n";
}

bool IsPathWithin(const std::filesystem::path& parent_path, const std::filesystem::path& candidate_path)
{
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(candidate_path, parent_path, error);
    if (error)
    {
        return false;
    }

    const std::string relative_string = relative.generic_string();
    return relative == "." || (!relative.empty() && relative_string != ".." && relative_string.rfind("../", 0) != 0);
}

bool HasExtension(const std::filesystem::path& path, std::initializer_list<const char*> extensions)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });

    for (const char* candidate : extensions)
    {
        if (extension == candidate)
        {
            return true;
        }
    }

    return false;
}

std::filesystem::path GetAvailablePath(const std::filesystem::path& destination_directory, const std::filesystem::path& source_path)
{
    std::filesystem::path candidate_path = destination_directory / source_path.filename();
    if (!std::filesystem::exists(candidate_path))
    {
        return candidate_path;
    }

    const std::string stem = source_path.stem().string();
    const std::string extension = source_path.extension().string();
    for (int index = 1; index < 1000; ++index)
    {
        candidate_path = destination_directory / (stem + " (" + std::to_string(index) + ")" + extension);
        if (!std::filesystem::exists(candidate_path))
        {
            return candidate_path;
        }
    }

    return {};
}

std::filesystem::path GetAvailableDirectoryPath(const std::filesystem::path& destination_directory, const std::string& directory_name)
{
    std::filesystem::path candidate_path = destination_directory / directory_name;
    if (!std::filesystem::exists(candidate_path))
    {
        return candidate_path;
    }

    for (int index = 1; index < 1000; ++index)
    {
        candidate_path = destination_directory / (directory_name + " (" + std::to_string(index) + ")");
        if (!std::filesystem::exists(candidate_path))
        {
            return candidate_path;
        }
    }

    return {};
}

int DecodeHexNibble(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f')
    {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}

std::string PercentDecode(std::string_view value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index)
    {
        if (value[index] == '%' && index + 2 < value.size())
        {
            const int high = DecodeHexNibble(value[index + 1]);
            const int low = DecodeHexNibble(value[index + 2]);
            if (high >= 0 && low >= 0)
            {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }

        decoded.push_back(value[index]);
    }

    return decoded;
}

bool IsPortableRelativeSubpath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute())
    {
        return false;
    }

    const std::string normalized = path.generic_string();
    return normalized != ".." && normalized.rfind("../", 0) != 0;
}

std::vector<std::filesystem::path> CollectGltfExternalDependencies(const std::filesystem::path& source_path)
{
    std::ifstream input(source_path, std::ios::binary);
    if (!input)
    {
        return {};
    }

    json document;
    try
    {
        input >> document;
    }
    catch (...)
    {
        return {};
    }

    std::vector<std::filesystem::path> dependencies;
    const auto append_dependencies = [&](const char* key)
    {
        const auto it = document.find(key);
        if (it == document.end() || !it->is_array())
        {
            return;
        }

        for (const json& entry : *it)
        {
            if (!entry.is_object())
            {
                continue;
            }

            const auto uri_it = entry.find("uri");
            if (uri_it == entry.end() || !uri_it->is_string())
            {
                continue;
            }

            const std::string uri = uri_it->get<std::string>();
            if (uri.empty() || uri.rfind("data:", 0) == 0 || uri.find("://") != std::string::npos)
            {
                continue;
            }

            const std::filesystem::path decoded_path = std::filesystem::path(PercentDecode(uri)).lexically_normal();
            if (!IsPortableRelativeSubpath(decoded_path))
            {
                continue;
            }

            dependencies.push_back(decoded_path);
        }
    };

    append_dependencies("buffers");
    append_dependencies("images");

    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    return dependencies;
}

bool CopyFileWithParentDirectories(
    const std::filesystem::path& source_path,
    const std::filesystem::path& destination_path,
    std::string& failure_reason)
{
    std::error_code error;
    std::filesystem::create_directories(destination_path.parent_path(), error);
    if (error)
    {
        failure_reason = "Failed to prepare import directory: " + destination_path.parent_path().string();
        return false;
    }

    std::filesystem::copy_file(source_path, destination_path, std::filesystem::copy_options::none, error);
    if (error)
    {
        failure_reason = "Failed to copy import dependency: " + source_path.string();
        return false;
    }

    return true;
}

bool CopyImportedModel(
    const std::filesystem::path& source_path,
    const std::filesystem::path& destination_directory,
    std::filesystem::path& imported_model_path,
    std::string& failure_reason)
{
    const std::string extension = source_path.extension().string();
    std::string lowered_extension = extension;
    std::transform(lowered_extension.begin(), lowered_extension.end(), lowered_extension.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });

    if (lowered_extension != ".gltf")
    {
        imported_model_path = GetAvailablePath(destination_directory, source_path);
        if (imported_model_path.empty())
        {
            failure_reason = "Cannot import model: failed to choose a destination name";
            return false;
        }

        std::error_code copy_error;
        std::filesystem::copy_file(source_path, imported_model_path, std::filesystem::copy_options::none, copy_error);
        if (copy_error)
        {
            failure_reason = "Failed to import model into: " + destination_directory.string();
            return false;
        }

        return true;
    }

    const std::vector<std::filesystem::path> dependencies = CollectGltfExternalDependencies(source_path);
    if (dependencies.empty())
    {
        imported_model_path = GetAvailablePath(destination_directory, source_path);
        if (imported_model_path.empty())
        {
            failure_reason = "Cannot import model: failed to choose a destination name";
            return false;
        }

        std::error_code copy_error;
        std::filesystem::copy_file(source_path, imported_model_path, std::filesystem::copy_options::none, copy_error);
        if (copy_error)
        {
            failure_reason = "Failed to import model into: " + destination_directory.string();
            return false;
        }

        return true;
    }

    const std::filesystem::path package_directory = GetAvailableDirectoryPath(destination_directory, source_path.stem().string());
    if (package_directory.empty())
    {
        failure_reason = "Cannot import model: failed to choose a destination folder";
        return false;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(package_directory, directory_error);
    if (directory_error)
    {
        failure_reason = "Failed to prepare model import folder: " + package_directory.string();
        return false;
    }

    imported_model_path = package_directory / source_path.filename();
    if (!CopyFileWithParentDirectories(source_path, imported_model_path, failure_reason))
    {
        std::error_code cleanup_error;
        std::filesystem::remove_all(package_directory, cleanup_error);
        return false;
    }

    for (const std::filesystem::path& dependency : dependencies)
    {
        const std::filesystem::path source_dependency_path = (source_path.parent_path() / dependency).lexically_normal();
        if (!std::filesystem::exists(source_dependency_path))
        {
            failure_reason = "Cannot import glTF: missing referenced file " + dependency.generic_string();
            std::error_code cleanup_error;
            std::filesystem::remove_all(package_directory, cleanup_error);
            return false;
        }

        const std::filesystem::path destination_dependency_path = package_directory / dependency;
        if (!CopyFileWithParentDirectories(source_dependency_path, destination_dependency_path, failure_reason))
        {
            std::error_code cleanup_error;
            std::filesystem::remove_all(package_directory, cleanup_error);
            return false;
        }
    }

    return true;
}

std::filesystem::path ResolveSceneTarget(const EngineState& state)
{
    if (state.HasSelectedItem() && HasExtension(state.selected_item_path, {".scene"}))
    {
        return state.selected_item_path;
    }

    if (state.HasOpenFile() && HasExtension(state.open_file_path, {".scene"}))
    {
        return state.open_file_path;
    }

    const std::filesystem::path default_scene_path = state.project_root / "Scenes" / "Main.scene";
    if (std::filesystem::exists(default_scene_path))
    {
        return default_scene_path;
    }

    const std::filesystem::path scenes_directory = state.project_root / "Scenes";
    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(scenes_directory, error))
    {
        if (error)
        {
            break;
        }

        if (entry.is_regular_file() && HasExtension(entry.path(), {".scene"}))
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

std::filesystem::path ShowNativeImportDialog(const wchar_t* title, const COMDLG_FILTERSPEC* filters, std::size_t filter_count)
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
        options |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        dialog->SetOptions(options);
        dialog->SetTitle(title);
        dialog->SetFileTypes(static_cast<UINT>(filter_count), filters);
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

std::filesystem::path ShowNativeModelImportDialog()
{
    const COMDLG_FILTERSPEC filters[] = {
        {L"Supported Models", L"*.fbx;*.glb;*.gltf"},
        {L"FBX", L"*.fbx"},
        {L"Binary glTF", L"*.glb"},
        {L"glTF", L"*.gltf"},
        {L"All Files", L"*.*"},
    };
    return ShowNativeImportDialog(L"Import Model (.fbx/.glb/.gltf)", filters, std::size(filters));
}

std::filesystem::path ShowNativeFontImportDialog()
{
    const COMDLG_FILTERSPEC filters[] = {
        {L"Font Files", L"*.ttf;*.otf"},
        {L"TrueType", L"*.ttf"},
        {L"OpenType", L"*.otf"},
        {L"All Files", L"*.*"},
    };
    return ShowNativeImportDialog(L"Import Font (.ttf/.otf)", filters, std::size(filters));
}

std::filesystem::path ShowNativeImageImportDialog()
{
    const COMDLG_FILTERSPEC filters[] = {
        {L"Images", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.gif;*.psd;*.hdr"},
        {L"PNG", L"*.png"},
        {L"JPEG", L"*.jpg;*.jpeg"},
        {L"Targa", L"*.tga"},
        {L"Bitmap", L"*.bmp"},
        {L"All Files", L"*.*"},
    };
    return ShowNativeImportDialog(L"Import Image", filters, std::size(filters));
}
#endif
}

bool CreationMenu::RenderButton(EngineState& state, const std::filesystem::path& directory_path, const char* label, bool align_right)
{
    bool changed = false;
    ImGui::PushID(directory_path.generic_string().c_str());

    if (align_right)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float button_width = ImGui::CalcTextSize(label).x + style.FramePadding.x * 2.0f;
        const float button_x = ImGui::GetWindowContentRegionMax().x - button_width;
        ImGui::SameLine(button_x);
    }

    const bool pressed = align_right ? ImGui::SmallButton(label) : ImGui::Button(label);
    if (pressed)
    {
        ImGui::OpenPopup("CreateMenu");
    }

    if (ImGui::BeginPopup("CreateMenu"))
    {
        if (ImGui::MenuItem("New Folder"))
        {
            OpenCreateDialog(directory_path, CreateTarget::Folder);
        }

        const std::filesystem::path script_directory = directory_path == state.project_root ? state.project_root / "Scripts" : directory_path;
        if (ImGui::MenuItem("New Script (.lua)"))
        {
            OpenCreateDialog(script_directory, CreateTarget::Script);
        }

        const std::filesystem::path graph_directory = directory_path == state.project_root ? state.project_root / "Graphs" : directory_path;
        if (ImGui::MenuItem("New Graph (.graph)"))
        {
            OpenCreateDialog(graph_directory, CreateTarget::Graph);
        }

        const std::filesystem::path scene_directory = directory_path == state.project_root ? state.project_root / "Scenes" : directory_path;
        if (ImGui::MenuItem("New Scene (.scene)"))
        {
            OpenCreateDialog(scene_directory, CreateTarget::Scene);
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Import Model (.fbx/.glb/.gltf)..."))
        {
            changed = ImportModel(state, directory_path) || changed;
        }

        if (ImGui::MenuItem("Import Font (.ttf/.otf)..."))
        {
            changed = ImportFont(state, directory_path) || changed;
        }

        if (ImGui::MenuItem("Import Image..."))
        {
            changed = ImportImage(state, directory_path) || changed;
        }

        const std::filesystem::path target_scene_path = ResolveSceneTarget(state);
        if (ImGui::MenuItem("Add Object", nullptr, false, !target_scene_path.empty()))
        {
            OpenCreateDialog(directory_path, CreateTarget::Object, target_scene_path);
        }

        ImGui::EndPopup();
    }

    ImGui::PopID();
    return changed;
}

bool CreationMenu::Render(EngineState& state)
{
    if (open_create_popup_)
    {
        ImGui::OpenPopup(kCreateItemPopupName);
        open_create_popup_ = false;
    }

    if (!ImGui::BeginPopupModal(kCreateItemPopupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return false;
    }

    const bool creating_folder = create_target_ == CreateTarget::Folder;
    const bool creating_script = create_target_ == CreateTarget::Script;
    const bool creating_graph = create_target_ == CreateTarget::Graph;
    const bool creating_scene = create_target_ == CreateTarget::Scene;
    const bool creating_object = create_target_ == CreateTarget::Object;

    const char* title = creating_folder ? "Create Folder"
        : creating_script ? "Create Script"
        : creating_graph ? "Create Graph"
        : creating_scene ? "Create Scene"
        : "Add Object";
    ImGui::TextUnformatted(title);
    ImGui::Text("Target: %s", state.GetDisplayPath(target_directory_).c_str());
    if (creating_object)
    {
        ImGui::Text("Scene: %s", state.GetDisplayPath(target_scene_path_).c_str());
    }
    ImGui::Separator();

    if (focus_name_input_)
    {
        ImGui::SetKeyboardFocusHere();
        focus_name_input_ = false;
    }

    ImGui::PushItemWidth(240.0f);
    const char* input_label = creating_folder ? "Folder Name"
        : creating_script ? "Script Name"
        : creating_graph ? "Graph Name"
        : creating_scene ? "Scene Name"
        : "Object Name";
    const bool submitted = ImGui::InputText(input_label, name_buffer_.data(), name_buffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();

    bool created_item = false;
    if (submitted || ImGui::Button("Create"))
    {
        created_item = CreateItem(state);
        if (created_item)
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
    return created_item;
}

bool CreationMenu::CreateItem(EngineState& state)
{
    std::string item_name = SanitizeName(name_buffer_.data());
    if (item_name.empty())
    {
        state.AddLog("Cannot create item: enter a valid name");
        return false;
    }

    if (create_target_ == CreateTarget::Object)
    {
        if (target_scene_path_.empty())
        {
            state.AddLog("Cannot add object: no target scene is available");
            return false;
        }

        if (state.HasOpenFile() && state.open_file_path == target_scene_path_ && state.open_file_dirty)
        {
            state.AddLog("Cannot add object: save the open scene before editing it from the add menu");
            return false;
        }

        std::ofstream output(target_scene_path_, std::ios::binary | std::ios::app);
        if (!output)
        {
            state.AddLog("Failed to add object to scene: " + state.GetDisplayPath(target_scene_path_));
            return false;
        }

        output << BuildObjectStub(item_name);
        if (!output)
        {
            state.AddLog("Failed while writing scene object: " + state.GetDisplayPath(target_scene_path_));
            return false;
        }

        state.AddLog("Added object to scene: " + state.GetDisplayPath(target_scene_path_));
        state.SetSelectedItem(target_scene_path_);
        state.OpenTextFile(target_scene_path_);
        return true;
    }

    std::filesystem::path target_path = target_directory_ / item_name;
    if (create_target_ == CreateTarget::Script && target_path.extension() != ".lua")
    {
        target_path += ".lua";
    }
    else if (create_target_ == CreateTarget::Graph && target_path.extension() != ".graph")
    {
        target_path += ".graph";
    }
    else if (create_target_ == CreateTarget::Scene && target_path.extension() != ".scene")
    {
        target_path += ".scene";
    }

    if (std::filesystem::exists(target_path))
    {
        state.AddLog("Cannot create item: already exists: " + state.GetDisplayPath(target_path));
        return false;
    }

    if (create_target_ == CreateTarget::Folder)
    {
        std::error_code error;
        std::filesystem::create_directory(target_path, error);
        if (error)
        {
            state.AddLog("Failed to create folder: " + state.GetDisplayPath(target_path));
            return false;
        }

        state.AddLog("Created folder: " + state.GetDisplayPath(target_path));
        return true;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(target_path.parent_path(), directory_error);
    if (directory_error)
    {
        state.AddLog("Failed to create item directory: " + state.GetDisplayPath(target_path.parent_path()));
        return false;
    }

    std::ofstream output(target_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        state.AddLog("Failed to create item: " + state.GetDisplayPath(target_path));
        return false;
    }

    if (create_target_ == CreateTarget::Script)
    {
        output << BuildScriptStub(target_path.stem().string());
    }
    else if (create_target_ == CreateTarget::Graph)
    {
        output << BuildGraphStub(target_path.stem().string());
    }
    else if (create_target_ == CreateTarget::Scene)
    {
        output << BuildSceneStub(target_path.stem().string());
    }

    if (!output)
    {
        state.AddLog("Failed while writing item: " + state.GetDisplayPath(target_path));
        return false;
    }

    const char* item_kind = create_target_ == CreateTarget::Script ? "script"
        : create_target_ == CreateTarget::Graph ? "graph"
        : create_target_ == CreateTarget::Scene ? "scene"
        : "item";
    state.AddLog(std::string("Created ") + item_kind + ": " + state.GetDisplayPath(target_path));
    state.SetSelectedItem(target_path);
    state.OpenTextFile(target_path);
    return true;
}

bool CreationMenu::ImportModel(EngineState& state, const std::filesystem::path& directory_path)
{
    if (!state.HasOpenProject())
    {
        state.AddLog("Cannot import model: no project is loaded");
        return false;
    }

#ifdef _WIN32
    std::filesystem::path destination_directory = state.project_root / "Assets" / "Models";
    if (!directory_path.empty() && !state.project_root.empty() && IsPathWithin(state.project_root, directory_path))
    {
        const std::filesystem::path assets_directory = state.project_root / "Assets";
        if (IsPathWithin(assets_directory, directory_path))
        {
            destination_directory = directory_path;
        }
    }

    std::error_code directory_error;
    std::filesystem::create_directories(destination_directory, directory_error);
    if (directory_error)
    {
        state.AddLog("Failed to prepare model directory: " + state.GetDisplayPath(destination_directory));
        return false;
    }

    const std::filesystem::path source_path = ShowNativeModelImportDialog();
    if (source_path.empty())
    {
        return false;
    }

    if (!HasExtension(source_path, {".fbx", ".glb", ".gltf"}))
    {
        state.AddLog("Cannot import model: only .fbx, .glb, and .gltf are supported");
        return false;
    }

    std::filesystem::path destination_path;
    std::string import_failure_reason;
    if (!CopyImportedModel(source_path, destination_directory, destination_path, import_failure_reason))
    {
        state.AddLog(import_failure_reason.empty() ? "Failed to import model" : import_failure_reason);
        return false;
    }

    state.SetSelectedItem(destination_path);
    state.AddLog("Imported model: " + state.GetDisplayPath(destination_path));
    return true;
#else
    state.AddLog("Model import is only implemented on Windows");
    return false;
#endif
}

bool CreationMenu::ImportFont(EngineState& state, const std::filesystem::path& directory_path)
{
    if (!state.HasOpenProject())
    {
        state.AddLog("Cannot import font: no project is loaded");
        return false;
    }

#ifdef _WIN32
    std::filesystem::path destination_directory = state.project_root / "Assets" / "Fonts";
    const std::filesystem::path fonts_directory = state.project_root / "Assets" / "Fonts";
    if (!directory_path.empty() && IsPathWithin(fonts_directory, directory_path))
    {
        destination_directory = directory_path;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(destination_directory, directory_error);
    if (directory_error)
    {
        state.AddLog("Failed to prepare font directory: " + state.GetDisplayPath(destination_directory));
        return false;
    }

    const std::filesystem::path source_path = ShowNativeFontImportDialog();
    if (source_path.empty())
    {
        return false;
    }

    if (!HasExtension(source_path, {".ttf", ".otf"}))
    {
        state.AddLog("Cannot import font: only .ttf and .otf are supported");
        return false;
    }

    const std::filesystem::path destination_path = GetAvailablePath(destination_directory, source_path);
    if (destination_path.empty())
    {
        state.AddLog("Cannot import font: failed to choose a destination name");
        return false;
    }

    std::error_code copy_error;
    std::filesystem::copy_file(source_path, destination_path, std::filesystem::copy_options::none, copy_error);
    if (copy_error)
    {
        state.AddLog("Failed to import font into: " + state.GetDisplayPath(destination_directory));
        return false;
    }

    state.SetSelectedItem(destination_path);
    state.AddLog("Imported font: " + state.GetDisplayPath(destination_path));
    return true;
#else
    state.AddLog("Font import is only implemented on Windows");
    return false;
#endif
}

bool CreationMenu::ImportImage(EngineState& state, const std::filesystem::path& directory_path)
{
    if (!state.HasOpenProject())
    {
        state.AddLog("Cannot import Image: no project is loaded");
        return false;
    }

#ifdef _WIN32
    std::filesystem::path destination_directory = state.project_root / "Assets" / "Images";
    const std::filesystem::path Images_directory = state.project_root / "Assets" / "Images";
    if (!directory_path.empty() && IsPathWithin(Images_directory, directory_path))
    {
        destination_directory = directory_path;
    }

    std::error_code directory_error;
    std::filesystem::create_directories(destination_directory, directory_error);
    if (directory_error)
    {
        state.AddLog("Failed to prepare Image directory: " + state.GetDisplayPath(destination_directory));
        return false;
    }

    const std::filesystem::path source_path = ShowNativeImageImportDialog();
    if (source_path.empty())
    {
        return false;
    }

    if (!HasExtension(source_path, {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".gif", ".psd", ".hdr"}))
    {
        state.AddLog("Cannot import Image: unsupported Image format");
        return false;
    }

    const std::filesystem::path destination_path = GetAvailablePath(destination_directory, source_path);
    if (destination_path.empty())
    {
        state.AddLog("Cannot import Image: failed to choose a destination name");
        return false;
    }

    std::error_code copy_error;
    std::filesystem::copy_file(source_path, destination_path, std::filesystem::copy_options::none, copy_error);
    if (copy_error)
    {
        state.AddLog("Failed to import Image into: " + state.GetDisplayPath(destination_directory));
        return false;
    }

    state.SetSelectedItem(destination_path);
    state.AddLog("Imported Image: " + state.GetDisplayPath(destination_path));
    return true;
#else
    state.AddLog("Image import is only implemented on Windows");
    return false;
#endif
}

void CreationMenu::OpenCreateDialog(const std::filesystem::path& directory_path, CreateTarget create_target, std::filesystem::path target_scene_path)
{
    target_directory_ = directory_path;
    target_scene_path_ = std::move(target_scene_path);
    create_target_ = create_target;
    focus_name_input_ = true;
    open_create_popup_ = true;
    std::fill(name_buffer_.begin(), name_buffer_.end(), '\0');
}

void CreationMenu::Reset()
{
    target_directory_.clear();
    target_scene_path_.clear();
    create_target_ = CreateTarget::None;
    focus_name_input_ = false;
    open_create_popup_ = false;
    std::fill(name_buffer_.begin(), name_buffer_.end(), '\0');
}