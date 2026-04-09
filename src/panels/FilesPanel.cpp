#include "panels/FilesPanel.h"

#include "imgui.h"

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
    std::vector<FileTreeNode> object_nodes;

    std::ifstream input(scene_path, std::ios::binary);
    if (!input)
    {
        return object_nodes;
    }

    std::string line;
    while (std::getline(input, line))
    {
        const std::string trimmed = TrimCopy(line);
        constexpr std::string_view object_prefix = "Object:";
        if (trimmed.rfind(object_prefix, 0) != 0)
        {
            continue;
        }

        const std::string object_name = TrimCopy(trimmed.substr(object_prefix.size()));
        if (object_name.empty())
        {
            continue;
        }

        FileTreeNode object_node;
        object_node.path = scene_path;
        object_node.label = object_name;
        object_node.is_scene_object = true;
        object_nodes.push_back(std::move(object_node));
    }

    return object_nodes;
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

    const bool opened = ImGui::TreeNodeEx(tree_id.c_str(), flags, "%s", node.label.c_str());

    if (is_scene_file && !state.IsActiveScene(node.path) && !is_selected)
    {
        ImGui::PopStyleColor();
    }
    const bool tree_item_released = ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    const bool tree_item_toggled = ImGui::IsItemToggledOpen();
    const bool moved_from_source = !node.is_scene_object && RenderMoveSource(node, state);
    bool moved_to_directory = false;

    if (!node.is_scene_object && file_context_menu_.RenderItemMenu(node.path, node.is_directory, state))
    {
        refresh_requested_ = true;
    }

    if (node.is_directory)
    {
        moved_to_directory = RenderMoveTarget(node.path, state);
    }

    if (tree_item_released && !tree_item_toggled && !moved_from_source && !moved_to_directory)
    {
        if (node.is_scene_object)
        {
            state.SetSelectedSceneObject(node.path, node.label);
            state.OpenTextFile(node.path);
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
