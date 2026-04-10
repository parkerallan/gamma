#include "panels/FilesPanel.h"

#include "imgui.h"

#include "assets/SceneMetadata.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <string_view>
#include <system_error>

namespace
{
bool IsSceneFile(const std::filesystem::path& path)
{
    return path.extension() == ".scene";
}

bool IsCppFile(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value)
    {
        return static_cast<char>(std::tolower(value));
    });
    return extension == ".cpp";
}

bool ShouldSkipPath(const std::filesystem::path& path)
{
    const std::string name = path.filename().string();
    return name == "build" || name == ".git" || name == ".vs";
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

std::string ToLowerCopy(std::string_view value)
{
    std::string lowered(value);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return lowered;
}

std::string TrimCopy(std::string value)
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

bool CanMutateSceneObject(const EngineState& state, const std::filesystem::path& scene_path)
{
    return !(state.HasOpenFile() && state.open_file_path == scene_path && state.open_file_dirty);
}
}

FilesPanel::FilesPanel() = default;

void FilesPanel::Render(EngineState& state)
{
    if (!state.show_files_panel)
    {
        return;
    }

    if (!ImGui::Begin("Files", &state.show_files_panel))
    {
        ImGui::End();
        return;
    }
    refresh_requested_ = false;

    if (current_root_ != state.project_root)
    {
        RebuildTree(state.project_root);
    }

    if (state.request_open_project_dialog)
    {
        open_project_dialog_.Open();
        state.request_open_project_dialog = false;
    }
    if (state.request_new_project_dialog)
    {
        new_project_dialog_.Open();
        state.request_new_project_dialog = false;
    }

    const bool project_changed = open_project_dialog_.Render(state) || new_project_dialog_.Render(state);
    bool file_tree_changed = false;
    if (project_changed)
    {
        RebuildTree(state.project_root);
    }

    if (state.workspace_root.empty())
    {
        ImGui::TextUnformatted("Workspace root not resolved.");
        ImGui::End();
        return;
    }

    if (!state.HasOpenProject())
    {
        ImGui::TextUnformatted("No project loaded.");
        ImGui::TextWrapped("Use Open Project to load a .engineproj file or project folder, or create a new one.");
        if (ImGui::Button("Open Project"))
        {
            state.request_open_project_dialog = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("New Project"))
        {
            state.request_new_project_dialog = true;
        }
        ImGui::End();
        return;
    }

    if (state.project_root.empty())
    {
        ImGui::TextUnformatted("Project root is unavailable.");
        ImGui::End();
        return;
    }

    file_tree_changed = creation_menu_.RenderButton(state, state.project_root, "Add", false) || file_tree_changed;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##FilesSearch", "Search", search_buffer_.data(), search_buffer_.size());
    ImGui::Separator();

    const std::string filter = ToLowerCopy(search_buffer_.data());

    for (const FileTreeNode& node : roots_)
    {
        RenderNode(node, state, filter);
    }

    RenderProjectRootDropTarget(state);

    file_tree_changed = file_context_menu_.Render(state) || file_tree_changed;
    file_tree_changed = RenderSceneObjectPopups(state) || file_tree_changed;
    file_tree_changed = creation_menu_.Render(state) || file_tree_changed;
    if (file_tree_changed || refresh_requested_)
    {
        RebuildTree(state.project_root);
    }

    ImGui::End();
}

void FilesPanel::RebuildTree(const std::filesystem::path& root)
{
    current_root_ = root;
    roots_.clear();

    if (root.empty())
    {
        return;
    }

    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(root, error))
    {
        if (error)
        {
            break;
        }

        if (ShouldSkipPath(entry.path()))
        {
            continue;
        }

        roots_.push_back(BuildNode(entry.path()));
    }

    std::sort(roots_.begin(), roots_.end(), [](const FileTreeNode& left, const FileTreeNode& right)
    {
        if (left.is_directory != right.is_directory)
        {
            return left.is_directory > right.is_directory;
        }

        return left.label < right.label;
    });
}

FileTreeNode FilesPanel::BuildNode(const std::filesystem::path& path) const
{
    FileTreeNode node;
    node.path = path;
    node.label = path.filename().string();
    node.is_directory = std::filesystem::is_directory(path);

    if (!node.is_directory)
    {
        if (IsSceneFile(path))
        {
            node.children = BuildSceneObjectNodes(path);
        }
        return node;
    }

    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(path, error))
    {
        if (error)
        {
            break;
        }

        if (ShouldSkipPath(entry.path()))
        {
            continue;
        }

        node.children.push_back(BuildNode(entry.path()));
    }

    std::sort(node.children.begin(), node.children.end(), [](const FileTreeNode& left, const FileTreeNode& right)
    {
        if (left.is_directory != right.is_directory)
        {
            return left.is_directory > right.is_directory;
        }

        return left.label < right.label;
    });

    return node;
}

std::vector<FileTreeNode> FilesPanel::BuildSceneObjectNodes(const std::filesystem::path& scene_path) const
{
    const SceneMetadata scene_metadata = LoadSceneMetadata(scene_path);
    if (!scene_metadata.parsed)
    {
        return {};
    }

    auto build_child_nodes = [&](const auto& self, const std::string& parent_name) -> std::vector<FileTreeNode>
    {
        std::vector<FileTreeNode> child_nodes;
        for (const SceneObjectMetadata& object : scene_metadata.objects)
        {
            const bool has_valid_parent = std::any_of(scene_metadata.objects.begin(), scene_metadata.objects.end(), [&](const SceneObjectMetadata& candidate)
            {
                return candidate.name == object.parent_name;
            });
            const bool belongs_to_parent = parent_name.empty()
                ? object.parent_name.empty() || !has_valid_parent
                : object.parent_name == parent_name;
            if (!belongs_to_parent)
            {
                continue;
            }

            FileTreeNode object_node;
            object_node.path = scene_path;
            object_node.label = object.name;
            object_node.is_scene_object = true;
            object_node.children = self(self, object.name);
            child_nodes.push_back(std::move(object_node));
        }

        std::sort(child_nodes.begin(), child_nodes.end(), [](const FileTreeNode& left, const FileTreeNode& right)
        {
            return left.label < right.label;
        });
        return child_nodes;
    };

    return build_child_nodes(build_child_nodes, std::string{});
}

void FilesPanel::RenderNode(const FileTreeNode& node, EngineState& state, std::string_view filter)
{
    if (!NodeMatchesFilter(node, filter))
    {
        return;
    }

    const std::string full_path = state.GetDisplayPath(node.path);
    const std::string tree_id = node.is_scene_object
        ? full_path + "##" + node.label
        : full_path;
    const bool is_scene_file = !node.is_directory && !node.is_scene_object && IsSceneFile(node.path);
    const bool filter_active = !filter.empty();
    const bool is_selected = node.is_scene_object
        ? state.selected_item_path == node.path && state.selected_scene_object_name == node.label
        : state.selected_item_path == node.path && !state.HasSelectedSceneObject();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_None;
    if (!node.is_directory && node.children.empty())
    {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanFullWidth;
    }
    else if (filter_active)
    {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    if (node.is_directory || !node.children.empty())
    {
        flags |= ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
    }
    if (is_selected)
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    if (is_scene_file && !state.IsActiveScene(node.path) && !is_selected)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.46f, 0.48f, 0.52f, 1.0f));
    }

    const ImVec2 node_pos = ImGui::GetCursorScreenPos();
    const bool opened = ImGui::TreeNodeEx(tree_id.c_str(), flags, "%s", node.label.c_str());

    if (is_scene_file && !state.IsActiveScene(node.path) && !is_selected)
    {
        ImGui::PopStyleColor();
    }
    const bool has_toggle_arrow = node.is_directory || !node.children.empty();
    const bool tree_item_released = ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool tree_item_toggled = ImGui::IsItemToggledOpen();
    bool released_on_arrow = false;
    if (has_toggle_arrow && tree_item_released)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float arrow_hit_x1 = node_pos.x - style.TouchExtraPadding.x;
        const float arrow_hit_x2 = node_pos.x + ImGui::GetFontSize() + (style.FramePadding.x * 2.0f) + style.TouchExtraPadding.x;
        const float mouse_x = ImGui::GetIO().MousePos.x;
        released_on_arrow = mouse_x >= arrow_hit_x1 && mouse_x < arrow_hit_x2;
    }
    const bool moved_from_source = node.is_scene_object ? RenderSceneObjectMoveSource(node, state) : RenderMoveSource(node, state);
    bool moved_to_directory = false;
    bool scene_object_parent_changed = false;

    if (node.is_scene_object)
    {
        if (RenderSceneObjectMenu(node, state))
        {
            refresh_requested_ = true;
        }
    }
    else if (file_context_menu_.RenderItemMenu(node.path, node.is_directory, state))
    {
        refresh_requested_ = true;
    }

    if (node.is_directory)
    {
        moved_to_directory = RenderMoveTarget(node.path, state);
    }
    else if (node.is_scene_object)
    {
        scene_object_parent_changed = RenderSceneObjectMoveTarget(node, state);
        if (scene_object_parent_changed)
        {
            refresh_requested_ = true;
        }
    }
    else if (is_scene_file)
    {
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kSceneObjectDragDropPayload))
            {
                std::filesystem::path source_scene_path;
                std::string source_object_name;
                if (DecodeSceneObjectPayload(static_cast<const char*>(payload->Data), source_scene_path, source_object_name) && source_scene_path == node.path)
                {
                    if (!CanMutateSceneObject(state, node.path))
                    {
                        state.AddLog("Save the open scene before reparenting scene objects");
                    }
                    else if (SetSceneObjectParent(node.path, source_object_name, std::string{}))
                    {
                        state.OpenTextFile(node.path);
                        state.SetSelectedSceneObject(node.path, source_object_name);
                        refresh_requested_ = true;
                        scene_object_parent_changed = true;
                        state.AddLog("Moved scene object to root: " + source_object_name);
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
    }

    if (tree_item_released && !tree_item_toggled && !released_on_arrow && !moved_from_source && !moved_to_directory && !scene_object_parent_changed)
    {
        if (node.is_scene_object)
        {
            state.SetSelectedSceneObject(node.path, node.label);
            state.OpenTextFile(node.path);
            state.RequestTab(WorkspaceTab::Scene);
            state.AddLog("Selected scene object: " + node.label + " in " + full_path);
        }
        else if (node.is_directory)
        {
            state.SetSelectedItem(node.path);
            state.AddLog("Selected folder: " + full_path);
        }
        else
        {
            state.SetSelectedItem(node.path);
            if (!state.OpenTextFile(node.path))
            {
                state.AddLog("Selected file item: " + full_path);
            }
            else if (IsSceneFile(node.path))
            {
                state.RequestTab(WorkspaceTab::Scene);
            }
            else if (IsCppFile(node.path))
            {
                state.RequestTab(WorkspaceTab::Editor);
            }
        }
    }

    if (opened && (!node.is_directory ? !node.children.empty() : true))
    {
        for (const FileTreeNode& child : node.children)
        {
            RenderNode(child, state, filter);
        }
        ImGui::TreePop();
    }
}

bool FilesPanel::RenderSceneObjectMenu(const FileTreeNode& node, EngineState& state)
{
    bool changed = false;
    ImGui::PushID((std::string("SceneObjectMenu") + node.label).c_str());
    if (ImGui::BeginPopupContextItem("SceneObjectContextMenu"))
    {
        const SceneMetadata scene_metadata = LoadSceneMetadata(node.path);
        if (ImGui::MenuItem("Rename"))
        {
            QueueSceneObjectRename(node.path, node.label);
        }

        if (ImGui::MenuItem("Copy"))
        {
            scene_object_clipboard_scene_path_ = node.path;
            scene_object_clipboard_name_ = node.label;
            state.AddLog("Copied scene object: " + node.label);
        }

        const bool can_paste = !scene_object_clipboard_name_.empty() && scene_object_clipboard_scene_path_ == node.path;
        if (ImGui::MenuItem("Paste", nullptr, false, can_paste))
        {
            changed = HandleSceneObjectPaste(node.path, node.label, state);
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Delete"))
        {
            QueueSceneObjectDelete(node.path, node.label);
        }

        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

bool FilesPanel::RenderSceneObjectPopups(EngineState& state)
{
    bool changed = false;

    if (open_scene_object_rename_popup_)
    {
        ImGui::OpenPopup("Rename Scene Object");
        open_scene_object_rename_popup_ = false;
    }

    if (ImGui::BeginPopupModal("Rename Scene Object", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (focus_scene_object_name_input_)
        {
            ImGui::SetKeyboardFocusHere();
            focus_scene_object_name_input_ = false;
        }

        ImGui::TextUnformatted("Rename Object");
        ImGui::Separator();
        ImGui::PushItemWidth(260.0f);
        const bool submitted = ImGui::InputText("Name", scene_object_name_buffer_.data(), scene_object_name_buffer_.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();
        if (submitted || ImGui::Button("Rename"))
        {
            changed = RenameSceneObjectFromUi(state);
            if (changed)
            {
                ResetSceneObjectRename();
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            ResetSceneObjectRename();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (open_scene_object_delete_popup_)
    {
        ImGui::OpenPopup("Delete Scene Object");
        open_scene_object_delete_popup_ = false;
    }

    if (ImGui::BeginPopupModal("Delete Scene Object", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Delete Object");
        ImGui::TextWrapped("Delete %s and its children? This cannot be undone.", scene_object_action_name_.c_str());
        ImGui::Separator();
        if (ImGui::Button("Delete"))
        {
            changed = DeleteSceneObjectFromUi(state);
            if (changed)
            {
                ResetSceneObjectDelete();
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            ResetSceneObjectDelete();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    return changed;
}

bool FilesPanel::RenderSceneObjectMoveSource(const FileTreeNode& node, EngineState&)
{
    if (!ImGui::BeginDragDropSource())
    {
        return false;
    }

    const std::string payload = EncodeSceneObjectPayload(node.path, node.label);
    ImGui::SetDragDropPayload(kSceneObjectDragDropPayload, payload.c_str(), payload.size() + 1);
    ImGui::Text("Move object %s", node.label.c_str());
    ImGui::EndDragDropSource();
    return true;
}

bool FilesPanel::RenderSceneObjectMoveTarget(const FileTreeNode& node, EngineState& state)
{
    if (!ImGui::BeginDragDropTarget())
    {
        return false;
    }

    bool changed = false;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kSceneObjectDragDropPayload))
    {
        std::filesystem::path source_scene_path;
        std::string source_object_name;
        if (DecodeSceneObjectPayload(static_cast<const char*>(payload->Data), source_scene_path, source_object_name) && source_scene_path == node.path && source_object_name != node.label)
        {
            if (!CanMutateSceneObject(state, node.path))
            {
                state.AddLog("Save the open scene before reparenting scene objects");
            }
            else if (SetSceneObjectParent(node.path, source_object_name, node.label))
            {
                state.OpenTextFile(node.path);
                state.SetSelectedSceneObject(node.path, source_object_name);
                state.AddLog("Parented scene object " + source_object_name + " to " + node.label);
                changed = true;
            }
        }
    }

    ImGui::EndDragDropTarget();
    return changed;
}

bool FilesPanel::HandleSceneObjectPaste(const std::filesystem::path& scene_path, const std::string& sibling_object_name, EngineState& state)
{
    if (scene_object_clipboard_name_.empty() || scene_object_clipboard_scene_path_ != scene_path)
    {
        return false;
    }
    if (!CanMutateSceneObject(state, scene_path))
    {
        state.AddLog("Save the open scene before duplicating scene objects");
        return false;
    }

    std::string duplicated_name;
    if (!DuplicateSceneObject(scene_path, scene_object_clipboard_name_, &duplicated_name))
    {
        state.AddLog("Failed to copy scene object: " + scene_object_clipboard_name_);
        return false;
    }

    const SceneMetadata scene_metadata = LoadSceneMetadata(scene_path);
    const auto sibling_it = std::find_if(scene_metadata.objects.begin(), scene_metadata.objects.end(), [&](const SceneObjectMetadata& object)
    {
        return object.name == sibling_object_name;
    });
    const std::string parent_name = sibling_it != scene_metadata.objects.end() ? sibling_it->parent_name : std::string{};
    if (!parent_name.empty())
    {
        if (!SetSceneObjectParent(scene_path, duplicated_name, parent_name))
        {
            state.AddLog("Failed to parent copied scene object: " + duplicated_name);
            return false;
        }
    }

    state.OpenTextFile(scene_path);
    state.SetSelectedSceneObject(scene_path, duplicated_name);
    state.AddLog("Copied scene object: " + duplicated_name);
    return true;
}

bool FilesPanel::RenameSceneObjectFromUi(EngineState& state)
{
    if (!CanMutateSceneObject(state, scene_object_action_scene_path_))
    {
        state.AddLog("Save the open scene before renaming scene objects");
        return false;
    }

    const std::string new_name = SanitizeSceneObjectName(scene_object_name_buffer_.data());
    if (new_name.empty())
    {
        state.AddLog("Cannot rename object: enter a valid name");
        return false;
    }

    if (!RenameSceneObject(scene_object_action_scene_path_, scene_object_action_name_, new_name))
    {
        state.AddLog("Failed to rename scene object: " + scene_object_action_name_);
        return false;
    }

    state.OpenTextFile(scene_object_action_scene_path_);
    state.SetSelectedSceneObject(scene_object_action_scene_path_, new_name);
    state.AddLog("Renamed scene object: " + new_name);
    return true;
}

bool FilesPanel::DeleteSceneObjectFromUi(EngineState& state)
{
    if (!CanMutateSceneObject(state, scene_object_action_scene_path_))
    {
        state.AddLog("Save the open scene before deleting scene objects");
        return false;
    }

    if (!DeleteSceneObject(scene_object_action_scene_path_, scene_object_action_name_))
    {
        state.AddLog("Failed to delete scene object: " + scene_object_action_name_);
        return false;
    }

    state.OpenTextFile(scene_object_action_scene_path_);
    if (state.selected_item_path == scene_object_action_scene_path_ && state.selected_scene_object_name == scene_object_action_name_)
    {
        state.SetSelectedItem(scene_object_action_scene_path_);
    }
    state.AddLog("Deleted scene object: " + scene_object_action_name_);
    return true;
}

void FilesPanel::QueueSceneObjectRename(const std::filesystem::path& scene_path, const std::string& object_name)
{
    scene_object_action_scene_path_ = scene_path;
    scene_object_action_name_ = object_name;
    focus_scene_object_name_input_ = true;
    open_scene_object_rename_popup_ = true;
    std::fill(scene_object_name_buffer_.begin(), scene_object_name_buffer_.end(), '\0');
    const std::size_t copy_length = (std::min)(object_name.size(), scene_object_name_buffer_.size() - 1);
    std::copy_n(object_name.begin(), static_cast<std::ptrdiff_t>(copy_length), scene_object_name_buffer_.begin());
}

void FilesPanel::QueueSceneObjectDelete(const std::filesystem::path& scene_path, const std::string& object_name)
{
    scene_object_action_scene_path_ = scene_path;
    scene_object_action_name_ = object_name;
    open_scene_object_delete_popup_ = true;
}

void FilesPanel::ResetSceneObjectRename()
{
    scene_object_action_scene_path_.clear();
    scene_object_action_name_.clear();
    focus_scene_object_name_input_ = false;
}

void FilesPanel::ResetSceneObjectDelete()
{
    scene_object_action_scene_path_.clear();
    scene_object_action_name_.clear();
}

std::string FilesPanel::SanitizeSceneObjectName(std::string value)
{
    value = TrimCopy(std::move(value));
    std::replace(value.begin(), value.end(), ' ', '_');
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char character)
    {
        return character == ':' || character == '/' || character == '\\';
    }), value.end());
    return value;
}

std::string FilesPanel::EncodeSceneObjectPayload(const std::filesystem::path& scene_path, const std::string& object_name)
{
    return scene_path.generic_string() + "\n" + object_name;
}

bool FilesPanel::DecodeSceneObjectPayload(std::string_view payload, std::filesystem::path& scene_path, std::string& object_name)
{
    const std::size_t separator_index = payload.find('\n');
    if (separator_index == std::string_view::npos)
    {
        return false;
    }

    scene_path = std::filesystem::path(std::string(payload.substr(0, separator_index)));
    object_name = std::string(payload.substr(separator_index + 1));
    return !scene_path.empty() && !object_name.empty();
}

bool FilesPanel::NodeMatchesFilter(const FileTreeNode& node, std::string_view filter) const
{
    if (filter.empty())
    {
        return true;
    }

    if (ToLowerCopy(node.label).find(filter) != std::string::npos)
    {
        return true;
    }

    if (!node.is_directory)
    {
        return false;
    }

    for (const FileTreeNode& child : node.children)
    {
        if (NodeMatchesFilter(child, filter))
        {
            return true;
        }
    }

    return false;
}

bool FilesPanel::RenderProjectRootDropTarget(EngineState& state)
{
    ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.y < 48.0f)
    {
        available.y = 48.0f;
    }

    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("ProjectRootDropTarget", available);
    const ImVec2 max(min.x + available.x, min.y + available.y);

    bool moved = false;
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
        {
            const char* source_raw = static_cast<const char*>(payload->Data);
            moved = MovePath(std::filesystem::path(source_raw), state.project_root, state);
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRect(min, max, IM_COL32(94, 138, 190, 180), 6.0f, 0, 2.0f);

        const char* hint = "Move to project root";
        const ImVec2 hint_size = ImGui::CalcTextSize(hint);
        draw_list->AddText(
            ImVec2(min.x + (available.x - hint_size.x) * 0.5f, min.y + (available.y - hint_size.y) * 0.5f),
            IM_COL32(210, 220, 232, 255),
            hint);
    }

    return moved;
}

bool FilesPanel::RenderMoveSource(const FileTreeNode& node, EngineState& state)
{
    if (!ImGui::BeginDragDropSource())
    {
        return false;
    }

    const std::string source_path = node.path.generic_string();
    ImGui::SetDragDropPayload(kFileTreeDragDropPayload, source_path.c_str(), source_path.size() + 1);
    ImGui::Text("Move %s", state.GetDisplayPath(node.path).c_str());
    ImGui::EndDragDropSource();
    return true;
}

bool FilesPanel::RenderMoveTarget(const std::filesystem::path& destination_directory, EngineState& state)
{
    if (!ImGui::BeginDragDropTarget())
    {
        return false;
    }

    bool moved = false;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
    {
        const char* moved_path_raw = static_cast<const char*>(payload->Data);
        const std::filesystem::path moved_path(moved_path_raw);
        moved = MovePath(moved_path, destination_directory, state);
    }

    ImGui::EndDragDropTarget();
    return moved;
}

bool FilesPanel::MovePath(const std::filesystem::path& source_path, const std::filesystem::path& destination_directory, EngineState& state)
{
    if (source_path.empty() || destination_directory.empty())
    {
        return false;
    }

    if (source_path == state.project_root)
    {
        state.AddLog("Cannot move the project root folder");
        return false;
    }

    const std::filesystem::path normalized_source = source_path.lexically_normal();
    const std::filesystem::path normalized_destination = destination_directory.lexically_normal();

    if (normalized_source.parent_path() == normalized_destination)
    {
        return false;
    }

    if (std::filesystem::is_directory(normalized_source) && IsPathWithin(normalized_source, normalized_destination))
    {
        state.AddLog("Cannot move a folder into itself");
        return false;
    }

    const std::filesystem::path target_path = normalized_destination / normalized_source.filename();
    if (std::filesystem::exists(target_path))
    {
        state.AddLog("Cannot move item: destination already exists: " + state.GetDisplayPath(target_path));
        return false;
    }

    std::error_code error;
    std::filesystem::rename(normalized_source, target_path, error);
    if (error)
    {
        state.AddLog("Failed to move item: " + state.GetDisplayPath(normalized_source));
        return false;
    }

    state.UpdatePathsAfterMove(normalized_source, target_path);
    refresh_requested_ = true;
    state.AddLog("Moved item: " + state.GetDisplayPath(target_path));
    return true;
}
