#include "components/CreationMenu.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace
{
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
    return std::string("#include <iostream>\n\n") +
        "void " + file_stem + "()\n" +
        "{\n" +
        "    std::cout << \"" + file_stem + " running\\n\";\n" +
        "}\n";
}
}

void CreationMenu::RenderButton(const std::filesystem::path& directory_path, const char* label, bool align_right)
{
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

        if (ImGui::MenuItem("New Script (.cpp)"))
        {
            OpenCreateDialog(directory_path, CreateTarget::Script);
        }

        ImGui::EndPopup();
    }

    ImGui::PopID();
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
    ImGui::TextUnformatted(creating_folder ? "Create Folder" : "Create Script");
    ImGui::Text("Target: %s", state.GetDisplayPath(target_directory_).c_str());
    ImGui::Separator();

    if (focus_name_input_)
    {
        ImGui::SetKeyboardFocusHere();
        focus_name_input_ = false;
    }

    ImGui::PushItemWidth(240.0f);
    const bool submitted = ImGui::InputText(creating_folder ? "Folder Name" : "Script Name", name_buffer_.data(), name_buffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
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

    std::filesystem::path target_path = target_directory_ / item_name;
    if (create_target_ == CreateTarget::Script && target_path.extension() != ".cpp")
    {
        target_path += ".cpp";
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

    std::ofstream output(target_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        state.AddLog("Failed to create script: " + state.GetDisplayPath(target_path));
        return false;
    }

    output << BuildScriptStub(target_path.stem().string());
    if (!output)
    {
        state.AddLog("Failed while writing script: " + state.GetDisplayPath(target_path));
        return false;
    }

    state.AddLog("Created script: " + state.GetDisplayPath(target_path));
    state.OpenTextFile(target_path);
    return true;
}

void CreationMenu::OpenCreateDialog(const std::filesystem::path& directory_path, CreateTarget create_target)
{
    target_directory_ = directory_path;
    create_target_ = create_target;
    focus_name_input_ = true;
    open_create_popup_ = true;
    std::fill(name_buffer_.begin(), name_buffer_.end(), '\0');
}

void CreationMenu::Reset()
{
    target_directory_.clear();
    create_target_ = CreateTarget::None;
    focus_name_input_ = false;
    open_create_popup_ = false;
    std::fill(name_buffer_.begin(), name_buffer_.end(), '\0');
}