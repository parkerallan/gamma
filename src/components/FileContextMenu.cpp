#include "components/FileContextMenu.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <system_error>

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
}

bool FileContextMenu::RenderItemMenu(const std::filesystem::path& target_path, bool is_directory, EngineState& state)
{
    bool changed = false;
    ImGui::PushID(target_path.generic_string().c_str());
    if (ImGui::BeginPopupContextItem("FileItemContextMenu"))
    {
        const bool is_scene_file = !is_directory && target_path.extension() == ".scene";
        if (is_scene_file)
        {
            const bool is_active_scene = state.IsActiveScene(target_path);
            if (ImGui::MenuItem("Set as Active", nullptr, false, !is_active_scene))
            {
                changed = state.SetActiveScene(target_path) || changed;
            }

            ImGui::Separator();
        }

        if (ImGui::MenuItem("Rename"))
        {
            QueueRename(target_path, is_directory);
        }

        if (ImGui::MenuItem("Copy"))
        {
            clipboard_path_ = target_path;
            state.AddLog("Copied item: " + state.GetDisplayPath(target_path));
        }

        const std::filesystem::path paste_directory = is_directory ? target_path : target_path.parent_path();
        if (ImGui::MenuItem("Paste", nullptr, false, !clipboard_path_.empty()))
        {
            changed = HandlePaste(paste_directory, state);
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Delete"))
        {
            QueueDelete(target_path, is_directory);
        }

        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

bool FileContextMenu::Render(EngineState& state)
{
    bool changed = false;

    if (open_rename_popup_)
    {
        ImGui::OpenPopup(kRenamePopupName);
        open_rename_popup_ = false;
    }

    if (ImGui::BeginPopupModal(kRenamePopupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(action_target_is_directory_ ? "Rename Folder" : "Rename File");
        ImGui::Separator();

        if (focus_name_input_)
        {
            ImGui::SetKeyboardFocusHere();
            focus_name_input_ = false;
        }

        ImGui::PushItemWidth(260.0f);
        const bool submitted = ImGui::InputText("Name", name_buffer_.data(), name_buffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();

        if (submitted || ImGui::Button("Rename"))
        {
            changed = HandleRename(state);
            if (changed)
            {
                ResetRename();
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            ResetRename();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    if (open_delete_popup_)
    {
        ImGui::OpenPopup(kDeletePopupName);
        open_delete_popup_ = false;
    }

    if (ImGui::BeginPopupModal(kDeletePopupName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(action_target_is_directory_ ? "Delete Folder" : "Delete File");
        ImGui::TextWrapped("Delete %s? This cannot be undone.", action_target_path_.filename().generic_string().c_str());
        ImGui::Separator();

        if (ImGui::Button("Delete"))
        {
            changed = HandleDelete(state);
            if (changed)
            {
                ResetDelete();
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            ResetDelete();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    return changed;
}

bool FileContextMenu::HandleRename(EngineState& state)
{
    const std::string new_name = SanitizeName(name_buffer_.data());
    if (new_name.empty())
    {
        state.AddLog("Cannot rename item: enter a valid name");
        return false;
    }

    const std::filesystem::path new_path = action_target_path_.parent_path() / new_name;
    if (new_path == action_target_path_)
    {
        return false;
    }

    if (std::filesystem::exists(new_path))
    {
        state.AddLog("Cannot rename item: destination already exists: " + state.GetDisplayPath(new_path));
        return false;
    }

    std::error_code error;
    std::filesystem::rename(action_target_path_, new_path, error);
    if (error)
    {
        state.AddLog("Failed to rename item: " + state.GetDisplayPath(action_target_path_));
        return false;
    }

    state.UpdatePathsAfterMove(action_target_path_, new_path);
    state.AddLog("Renamed item: " + state.GetDisplayPath(new_path));
    return true;
}

bool FileContextMenu::HandleDelete(EngineState& state)
{
    std::error_code error;
    if (action_target_is_directory_)
    {
        std::filesystem::remove_all(action_target_path_, error);
    }
    else
    {
        std::filesystem::remove(action_target_path_, error);
    }

    if (error)
    {
        state.AddLog("Failed to delete item: " + state.GetDisplayPath(action_target_path_));
        return false;
    }

    state.UpdatePathsAfterDelete(action_target_path_);
    state.AddLog("Deleted item: " + state.GetDisplayPath(action_target_path_));
    return true;
}

bool FileContextMenu::HandlePaste(const std::filesystem::path& destination_directory, EngineState& state)
{
    if (clipboard_path_.empty())
    {
        return false;
    }

    if (std::filesystem::is_directory(clipboard_path_))
    {
        std::error_code relative_error;
        const std::filesystem::path relative = std::filesystem::relative(destination_directory, clipboard_path_, relative_error);
        const std::string relative_string = relative.generic_string();
        if (!relative_error && (relative == "." || (!relative.empty() && relative_string != ".." && relative_string.rfind("../", 0) != 0)))
        {
            state.AddLog("Cannot paste a folder into itself");
            return false;
        }
    }

    const std::filesystem::path target_path = GetNextPastePath(destination_directory, clipboard_path_);

    std::error_code error;
    if (std::filesystem::is_directory(clipboard_path_))
    {
        std::filesystem::copy(clipboard_path_, target_path, std::filesystem::copy_options::recursive, error);
    }
    else
    {
        std::filesystem::copy_file(clipboard_path_, target_path, std::filesystem::copy_options::none, error);
    }

    if (error)
    {
        state.AddLog("Failed to paste item into: " + state.GetDisplayPath(destination_directory));
        return false;
    }

    state.AddLog("Pasted item: " + state.GetDisplayPath(target_path));
    return true;
}

std::filesystem::path FileContextMenu::GetNextPastePath(const std::filesystem::path& destination_directory, const std::filesystem::path& source_path) const
{
    const std::filesystem::path initial_target = destination_directory / source_path.filename();
    if (!std::filesystem::exists(initial_target))
    {
        return initial_target;
    }

    const bool is_directory = std::filesystem::is_directory(source_path);
    const std::string extension = is_directory ? std::string() : source_path.extension().string();
    const std::string base_name = is_directory ? source_path.filename().string() : source_path.stem().string();

    for (int copy_index = 1; copy_index < 10000; ++copy_index)
    {
        const std::string candidate_name = base_name + "(" + std::to_string(copy_index) + ")" + extension;
        const std::filesystem::path candidate_path = destination_directory / candidate_name;
        if (!std::filesystem::exists(candidate_path))
        {
            return candidate_path;
        }
    }

    return initial_target;
}

void FileContextMenu::QueueRename(const std::filesystem::path& target_path, bool is_directory)
{
    action_target_path_ = target_path;
    action_target_is_directory_ = is_directory;
    focus_name_input_ = true;
    open_rename_popup_ = true;
    std::fill(name_buffer_.begin(), name_buffer_.end(), '\0');
    const std::string initial_name = target_path.filename().string();
    const std::size_t copy_length = (std::min)(initial_name.size(), name_buffer_.size() - 1);
    std::copy_n(initial_name.begin(), static_cast<std::ptrdiff_t>(copy_length), name_buffer_.begin());
}

void FileContextMenu::QueueDelete(const std::filesystem::path& target_path, bool is_directory)
{
    action_target_path_ = target_path;
    action_target_is_directory_ = is_directory;
    open_delete_popup_ = true;
}

void FileContextMenu::ResetRename()
{
    action_target_path_.clear();
    action_target_is_directory_ = false;
    focus_name_input_ = false;
    open_rename_popup_ = false;
    std::fill(name_buffer_.begin(), name_buffer_.end(), '\0');
}

void FileContextMenu::ResetDelete()
{
    action_target_path_.clear();
    action_target_is_directory_ = false;
    open_delete_popup_ = false;
}