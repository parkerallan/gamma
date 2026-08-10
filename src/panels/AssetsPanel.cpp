#include "panels/AssetsPanel.h"

#include "assets/PrefabAsset.h"
#include "ui/Codicons.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <system_error>

namespace
{
constexpr const char* kFileTreeDragDropPayload = "FILE_TREE_PATH";
constexpr const char* kPrefabDragDropPayload = "PREFAB_NAME";

constexpr float kTileWidth = 96.0f;
constexpr float kTileHeight = 104.0f;
constexpr float kTileSpacing = 10.0f;
constexpr float kGlyphSize = 34.0f;

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
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

std::string Ellipsize(const std::string& value, float max_width)
{
    if (ImGui::CalcTextSize(value.c_str()).x <= max_width)
    {
        return value;
    }

    std::string trimmed = value;
    while (!trimmed.empty() && ImGui::CalcTextSize((trimmed + "..").c_str()).x > max_width)
    {
        trimmed.pop_back();
    }
    return trimmed + "..";
}

void DrawGlyph(ImDrawList* draw_list, ImVec2 center, const char* glyph, ImU32 color)
{
    ImFont* font = ImGui::GetFont();
    const ImVec2 size = font->CalcTextSizeA(kGlyphSize, FLT_MAX, 0.0f, glyph);
    draw_list->AddText(font, kGlyphSize, ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f), color, glyph);
}
}

void AssetsPanel::Render(EngineState& state)
{
    if (!state.show_assets_panel)
    {
        return;
    }

    if (!ImGui::Begin("Assets", &state.show_assets_panel))
    {
        ImGui::End();
        return;
    }

    if (!state.HasOpenProject())
    {
        ImGui::TextDisabled("No project open.");
        ImGui::End();
        return;
    }

    refresh_requested_ = false;

    const std::filesystem::path assets_root = state.GetAssetsDirectory();
    const std::filesystem::path directory = assets_root / state.assets_cwd;

    // A folder that disappeared underneath us (deleted, or a project switch)
    // would otherwise leave the panel stuck on an empty view.
    std::error_code directory_error;
    if (!std::filesystem::is_directory(directory, directory_error))
    {
        state.assets_cwd.clear();
    }

    RenderBreadcrumb(state);
    ImGui::Separator();

    RefreshEntries(state, assets_root / state.assets_cwd);
    RenderGrid(state, assets_root / state.assets_cwd);

    if (ApplyPendingMove(state))
    {
        refresh_requested_ = true;
    }

    refresh_requested_ = file_context_menu_.Render(state) || refresh_requested_;
    refresh_requested_ = creation_menu_.Render(state) || refresh_requested_;

    if (refresh_requested_)
    {
        cached_directory_.clear();
        // The Files tree still shows Scenes / Scripts / Graphs, and a rename
        // or delete here can move a path it has cached.
        state.request_files_tree_refresh = true;
    }

    ImGui::End();
}

void AssetsPanel::RenderBreadcrumb(EngineState& state)
{
    if (ImGui::Button(ICON_CI_FOLDER_OPENED " Assets"))
    {
        state.assets_cwd.clear();
    }
    RenderMoveTarget(state, state.GetAssetsDirectory());

    // Navigation is deferred: assigning to assets_cwd inside this loop would
    // invalidate the path iterator walking it, which hangs rather than crashes.
    std::filesystem::path pending_navigation;
    bool navigate = false;

    std::filesystem::path accumulated;
    int crumb_index = 0;
    for (const std::filesystem::path& part : state.assets_cwd)
    {
        accumulated /= part;
        ImGui::SameLine(0.0f, 4.0f);
        ImGui::TextUnformatted("/");
        ImGui::SameLine(0.0f, 4.0f);
        // Repeated folder names along the path (Assets/foo/foo) would otherwise
        // share an ID and click as one button.
        ImGui::PushID(crumb_index++);
        if (ImGui::Button(part.string().c_str()))
        {
            pending_navigation = accumulated;
            navigate = true;
        }
        RenderMoveTarget(state, state.GetAssetsDirectory() / accumulated);
        ImGui::PopID();
    }

    if (navigate)
    {
        state.assets_cwd = pending_navigation;
        selected_.clear();
    }

    const char* import_label = ICON_CI_DESKTOP_DOWNLOAD " Import";
    const float import_width = ImGui::CalcTextSize(import_label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - import_width);
    refresh_requested_ =
        creation_menu_.RenderImportButton(state, state.GetAssetsDirectory() / state.assets_cwd, import_label) ||
        refresh_requested_;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::SetTooltip("Import into %s", state.GetDisplayPath(state.GetAssetsDirectory() / state.assets_cwd).c_str());
    }
}

void AssetsPanel::RefreshEntries(EngineState& state, const std::filesystem::path& directory)
{
    // The Files tree throttles its own walk because filesystem calls are
    // expensive here (AV interception) and hitch play mode; one directory is
    // far cheaper than that recursive scan, but it still isn't free per frame.
    const double now = ImGui::GetTime();
    const bool directory_changed = cached_directory_ != directory;
    const bool refresh_due = !state.is_playing && (now - last_refresh_time_) >= 0.5;
    if (!directory_changed && !refresh_due)
    {
        return;
    }

    cached_directory_ = directory;
    last_refresh_time_ = now;
    entries_.clear();

    const std::filesystem::path prefabs_directory = GetPrefabsDirectory(state.project_root);
    const bool in_prefabs_directory = directory.lexically_normal() == prefabs_directory.lexically_normal();

    std::error_code error;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (error)
        {
            break;
        }

        const std::filesystem::path path = entry.path();

        // The prefab store is one file holding many prefabs; its entries are
        // listed as tiles instead, so the raw file is not shown.
        if (IsPrefabMetadataFile(path))
        {
            continue;
        }

        AssetEntry asset;
        asset.path = path;
        asset.name = path.filename().string();

        std::error_code kind_error;
        if (entry.is_directory(kind_error))
        {
            asset.kind = AssetKind::Folder;
        }
        else
        {
            const std::string extension = ToLowerCopy(path.extension().string());
            const auto is_one_of = [&extension](std::initializer_list<const char*> candidates)
            {
                for (const char* candidate : candidates)
                {
                    if (extension == candidate)
                    {
                        return true;
                    }
                }
                return false;
            };

            if (is_one_of({".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".dds", ".hdr", ".exr"}))
            {
                asset.kind = AssetKind::Image;
            }
            else if (is_one_of({".wav", ".ogg", ".mp3", ".flac"}))
            {
                asset.kind = AssetKind::Audio;
            }
            else if (is_one_of({".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v", ".wmv", ".mpg", ".mpeg"}))
            {
                asset.kind = AssetKind::Video;
            }
            else if (is_one_of({".fbx", ".glb", ".gltf", ".obj", ".dae"}))
            {
                asset.kind = AssetKind::Model;
            }
            else if (is_one_of({".ttf", ".otf"}))
            {
                asset.kind = AssetKind::Font;
            }
            else if (is_one_of({".efk", ".efkefc"}))
            {
                asset.kind = AssetKind::Effect;
            }
            else if (is_one_of({".lua", ".graph", ".scene", ".txt", ".json", ".md", ".ini", ".metadata", ".animator"}))
            {
                asset.kind = AssetKind::Text;
            }
        }

        entries_.push_back(std::move(asset));
    }

    if (in_prefabs_directory)
    {
        const PrefabMetadata metadata = LoadPrefabMetadata(state.project_root);
        for (const PrefabEntry& prefab : metadata.prefabs)
        {
            AssetEntry asset;
            asset.name = prefab.name;
            asset.kind = AssetKind::Prefab;
            asset.is_prefab_entry = true;
            entries_.push_back(std::move(asset));
        }
    }

    std::sort(entries_.begin(), entries_.end(), [](const AssetEntry& left, const AssetEntry& right)
    {
        const bool left_is_folder = left.kind == AssetKind::Folder;
        const bool right_is_folder = right.kind == AssetKind::Folder;
        if (left_is_folder != right_is_folder)
        {
            return left_is_folder;
        }
        return left.name < right.name;
    });
}

void AssetsPanel::RenderGrid(EngineState& state, const std::filesystem::path& directory)
{
    if (!ImGui::BeginChild("##AssetsGrid", ImVec2(0.0f, 0.0f)))
    {
        ImGui::EndChild();
        return;
    }

    const float available_width = ImGui::GetContentRegionAvail().x;
    const int columns = (std::max)(1, static_cast<int>((available_width + kTileSpacing) / (kTileWidth + kTileSpacing)));
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    std::filesystem::path pending_navigation;
    bool navigate = false;
    int column = 0;

    for (const AssetEntry& entry : entries_)
    {
        ImGui::PushID(entry.name.c_str());

        const ImVec2 tile_min = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tile", ImVec2(kTileWidth, kTileHeight));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        const bool double_clicked = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        const ImVec2 tile_max(tile_min.x + kTileWidth, tile_min.y + kTileHeight);

        // Same payloads the Files tree emitted, so every existing drop target
        // (attribute editor, animator, sequencer, info panel, scene nodes)
        // keeps working from here.
        if (ImGui::BeginDragDropSource())
        {
            if (entry.is_prefab_entry)
            {
                ImGui::SetDragDropPayload(kPrefabDragDropPayload, entry.name.c_str(), entry.name.size() + 1);
                ImGui::Text("Instantiate prefab %s", entry.name.c_str());
            }
            else
            {
                const std::string source_path = entry.path.generic_string();
                ImGui::SetDragDropPayload(kFileTreeDragDropPayload, source_path.c_str(), source_path.size() + 1);
                ImGui::Text("Move %s", state.GetDisplayPath(entry.path).c_str());
            }
            ImGui::EndDragDropSource();
        }

        if (entry.kind == AssetKind::Folder)
        {
            RenderMoveTarget(state, entry.path);
        }

        if (!entry.is_prefab_entry)
        {
            refresh_requested_ =
                file_context_menu_.RenderItemMenu(entry.path, entry.kind == AssetKind::Folder, state) ||
                refresh_requested_;
        }

        const bool selected = !entry.is_prefab_entry && selected_ == entry.path;
        if (selected)
        {
            draw_list->AddRectFilled(tile_min, tile_max, ImGui::GetColorU32(ImGuiCol_Header), 6.0f);
        }
        else if (hovered)
        {
            draw_list->AddRectFilled(tile_min, tile_max, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 6.0f);
        }

        const char* glyph = ICON_CI_FILE;
        ImU32 glyph_color = ImGui::GetColorU32(ImGuiCol_Text);
        switch (entry.kind)
        {
        case AssetKind::Folder:  glyph = ICON_CI_FOLDER;               glyph_color = IM_COL32(95, 155, 225, 255);  break;
        case AssetKind::Image:   glyph = ICON_CI_FILE_MEDIA;           glyph_color = IM_COL32(80, 185, 115, 255);  break;
        case AssetKind::Audio:   glyph = ICON_CI_MUSIC;                glyph_color = IM_COL32(175, 120, 205, 255); break;
        case AssetKind::Video:   glyph = ICON_CI_DEVICE_CAMERA_VIDEO;  glyph_color = IM_COL32(215, 130, 95, 255);  break;
        case AssetKind::Model:   glyph = ICON_CI_PACKAGE;              glyph_color = IM_COL32(205, 150, 80, 255);  break;
        case AssetKind::Font:    glyph = ICON_CI_SYMBOL_COLOR;         glyph_color = IM_COL32(120, 175, 205, 255); break;
        case AssetKind::Effect:  glyph = ICON_CI_SPARKLE;              glyph_color = IM_COL32(225, 195, 105, 255); break;
        case AssetKind::Prefab:  glyph = ICON_CI_TYPE_HIERARCHY;       glyph_color = IM_COL32(140, 200, 190, 255); break;
        case AssetKind::Text:    glyph = ICON_CI_FILE_CODE;            break;
        default:                                                       break;
        }
        DrawGlyph(draw_list, ImVec2(tile_min.x + kTileWidth * 0.5f, tile_min.y + 36.0f), glyph, glyph_color);

        const std::string label = Ellipsize(entry.name, kTileWidth - 10.0f);
        const float label_width = ImGui::CalcTextSize(label.c_str()).x;
        draw_list->AddText(
            ImVec2(tile_min.x + (kTileWidth - label_width) * 0.5f, tile_min.y + 74.0f),
            ImGui::GetColorU32(ImGuiCol_Text),
            label.c_str());

        if (clicked && !entry.is_prefab_entry)
        {
            selected_ = entry.path;
            state.SetSelectedItem(entry.path);
        }

        if (double_clicked)
        {
            if (entry.kind == AssetKind::Folder)
            {
                pending_navigation = state.assets_cwd / entry.name;
                navigate = true;
            }
            else if (!entry.is_prefab_entry && state.OpenTextFile(entry.path))
            {
                state.RequestTab(WorkspaceTab::Editor);
            }
        }

        ImGui::PopID();

        if (++column < columns)
        {
            ImGui::SameLine(0.0f, kTileSpacing);
        }
        else
        {
            column = 0;
        }
    }

    if (navigate)
    {
        state.assets_cwd = pending_navigation;
        selected_.clear();
    }

    // Creating assets is a right-click on empty space rather than a toolbar
    // button; the new item lands in the folder being browsed.
    if (ImGui::BeginPopupContextWindow("##AssetsBackground", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        creation_menu_.RenderAssetCreateItems(state, directory);
        ImGui::EndPopup();
    }

    ImGui::EndChild();

    // Dropping onto the empty area of the grid moves an asset into the folder
    // currently being browsed.
    RenderMoveTarget(state, directory);
}

void AssetsPanel::RenderMoveTarget(EngineState& state, const std::filesystem::path& destination_directory)
{
    // Folder tiles are nested inside the grid child, which is a target too.
    // First accept wins, so dropping on a folder moves into it rather than
    // into the folder being browsed.
    if (has_pending_move_ || !ImGui::BeginDragDropTarget())
    {
        return;
    }

    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
    {
        pending_move_.source = std::filesystem::path(static_cast<const char*>(payload->Data));
        pending_move_.destination_directory = destination_directory;
        has_pending_move_ = true;
    }

    ImGui::EndDragDropTarget();
    (void)state;
}

bool AssetsPanel::ApplyPendingMove(EngineState& state)
{
    if (!has_pending_move_)
    {
        return false;
    }
    has_pending_move_ = false;

    const std::filesystem::path source = pending_move_.source.lexically_normal();
    const std::filesystem::path destination = pending_move_.destination_directory.lexically_normal();
    if (source.empty() || destination.empty() || source.parent_path() == destination)
    {
        return false;
    }

    std::error_code error;
    if (std::filesystem::is_directory(source, error) && IsPathWithin(source, destination))
    {
        state.AddWarning("Cannot move a folder into itself");
        return false;
    }

    const std::filesystem::path target_path = destination / source.filename();
    if (std::filesystem::exists(target_path))
    {
        state.AddWarning("Cannot move item: destination already exists: " + state.GetDisplayPath(target_path));
        return false;
    }

    std::filesystem::rename(source, target_path, error);
    if (error)
    {
        state.AddError("Failed to move item: " + state.GetDisplayPath(source));
        return false;
    }

    state.UpdatePathsAfterMove(source, target_path);
    if (selected_ == source)
    {
        selected_ = target_path;
    }
    state.AddLog("Moved item: " + state.GetDisplayPath(target_path));
    return true;
}
