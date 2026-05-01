#include "components/EditorComponent.h"

#include "TextEditor.h"
#include "imgui.h"
#include "ui/Codicons.h"
#include "state/EngineState.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace
{
TextEditor::Coordinates OffsetToCoordinates(const std::string& text, std::size_t offset)
{
    int line = 0;
    int column = 0;
    const std::size_t bounded_offset = std::min(offset, text.size());
    for (std::size_t i = 0; i < bounded_offset; ++i)
    {
        if (text[i] == '\n')
        {
            ++line;
            column = 0;
            continue;
        }

        if (text[i] != '\r')
        {
            ++column;
        }
    }

    return TextEditor::Coordinates(line, column);
}

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
}

EditorComponent::EditorComponent()
    : editor_(std::make_unique<TextEditor>())
{
    editor_->SetShowWhitespaces(false);
    editor_->SetReadOnly(false);
    editor_->SetHandleKeyboardInputs(true);
    editor_->SetHandleMouseInputs(true);
    editor_->SetImGuiChildIgnored(false);
}

EditorComponent::~EditorComponent() = default;

bool EditorComponent::FindNextMatch()
{
    const std::string query(search_query_.data());
    if (query.empty())
    {
        return false;
    }

    std::string haystack = editor_->GetText();
    if (haystack.empty())
    {
        return false;
    }

    const std::string needle = ToLowerCopy(query);
    const std::string haystack_lower = ToLowerCopy(haystack);

    if (last_search_query_ != needle)
    {
        last_search_query_ = needle;
        last_match_offset_ = std::string::npos;
        last_search_direction_ = 0;
    }

    std::size_t search_start = 0;
    if (last_match_offset_ != std::string::npos)
    {
        if (last_search_direction_ > 0)
        {
            search_start = last_match_offset_ + query.size();
        }
        else
        {
            search_start = last_match_offset_ + 1;
        }
    }

    if (search_start > haystack_lower.size())
    {
        search_start = 0;
    }

    std::size_t pos = haystack_lower.find(needle, search_start);
    if (pos == std::string::npos)
    {
        pos = haystack_lower.find(needle, 0);
        if (pos == std::string::npos)
        {
            return false;
        }
    }

    const TextEditor::Coordinates selection_start = OffsetToCoordinates(haystack, pos);
    const TextEditor::Coordinates selection_end = OffsetToCoordinates(haystack, pos + query.size());
    editor_->SetCursorPosition(selection_start);
    editor_->SetSelection(selection_start, selection_end);
    last_match_offset_ = pos;
    last_search_direction_ = 1;
    return true;
}

bool EditorComponent::FindPreviousMatch()
{
    const std::string query(search_query_.data());
    if (query.empty())
    {
        return false;
    }

    std::string haystack = editor_->GetText();
    if (haystack.empty())
    {
        return false;
    }

    const std::string needle = ToLowerCopy(query);
    const std::string haystack_lower = ToLowerCopy(haystack);

    if (last_search_query_ != needle)
    {
        last_search_query_ = needle;
        last_match_offset_ = std::string::npos;
        last_search_direction_ = 0;
    }

    std::size_t search_from = std::string::npos;
    if (last_match_offset_ != std::string::npos)
    {
        if (last_match_offset_ == 0)
        {
            search_from = std::string::npos;
        }
        else
        {
            search_from = last_match_offset_ - 1;
        }
    }

    std::size_t pos = haystack_lower.rfind(needle, search_from);
    if (pos == std::string::npos)
    {
        pos = haystack_lower.rfind(needle);
        if (pos == std::string::npos)
        {
            return false;
        }
    }

    const TextEditor::Coordinates selection_start = OffsetToCoordinates(haystack, pos);
    const TextEditor::Coordinates selection_end = OffsetToCoordinates(haystack, pos + query.size());
    editor_->SetCursorPosition(selection_start);
    editor_->SetSelection(selection_start, selection_end);
    last_match_offset_ = pos;
    last_search_direction_ = -1;
    return true;
}

void EditorComponent::ApplyLanguage(const std::filesystem::path& path)
{
    const std::string ext = path.extension().string();
    if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".cc" || ext == ".cxx")
        editor_->SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
    else if (ext == ".glsl" || ext == ".vert" || ext == ".frag" ||
             ext == ".rgen" || ext == ".rmiss" || ext == ".rchit" || ext == ".rahit")
        editor_->SetLanguageDefinition(TextEditor::LanguageDefinition::GLSL());
    else if (ext == ".lua")
        editor_->SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    else if (ext == ".c")
        editor_->SetLanguageDefinition(TextEditor::LanguageDefinition::C());
}

void EditorComponent::LoadFile(EngineState& state)
{
    // Normalize CRLF to LF so the editor round-trips Windows files cleanly
    const std::string& raw = state.open_file_contents;
    std::string normalized;
    normalized.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n')
            continue;
        normalized += raw[i];
    }

    editor_->SetText(normalized);
    ApplyLanguage(state.open_file_path);
    loaded_path_ = state.open_file_path;
    focus_editor_ = true;
    last_search_query_.clear();
    last_match_offset_ = std::string::npos;
    last_search_direction_ = 0;
}

void EditorComponent::Render(EngineState& state)
{
    if (!state.HasOpenFile())
    {
        ImGui::TextUnformatted("Text Editor");
        ImGui::Separator();
        ImGui::TextWrapped("Select a supported text file from the Files panel to open it here.");
        return;
    }

    // Load content when a new file is opened or a reload was requested
    if (state.open_file_path != loaded_path_)
        LoadFile(state);

    ImGui::TextUnformatted(state.GetOpenFileDisplayPath().c_str());
    ImGui::SameLine();

    // Search controls on the left, Save/Reload on the right
    const float search_width = 240.0f;
    const std::string search_previous_label = ICON_CI_TRIANGLE_LEFT;
    const std::string search_next_label = ICON_CI_TRIANGLE_RIGHT;
    const std::string save_button_label = ICON_CI_SAVE;
    const std::string reload_button_label = ICON_CI_REFRESH;
    const std::string build_button_label = ICON_CI_RUN_WITH_DEPS;
    const std::string play_button_label = ICON_CI_DEBUG_START;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float save_width = ImGui::CalcTextSize(save_button_label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
    const float reload_width = ImGui::CalcTextSize(reload_button_label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
    const float build_width = ImGui::CalcTextSize(build_button_label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
    const float play_width = ImGui::CalcTextSize(play_button_label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2;
    const float save_reload_width = save_width + reload_width + build_width + play_width + (spacing * 3.0f);

    ImGui::SetNextItemWidth(search_width);
    const bool submit_search = ImGui::InputTextWithHint(
        "##EditorSearch",
        "Search",
        search_query_.data(),
        search_query_.size(),
        ImGuiInputTextFlags_EnterReturnsTrue);
    const bool search_is_active = ImGui::IsItemActive() || ImGui::IsItemFocused();
    ImGui::SameLine();

    bool search_triggered = false;
    if (ImGui::Button(search_previous_label.c_str()))
    {
        FindPreviousMatch();
        search_triggered = true;
    }
    ImGui::SameLine();

    if (ImGui::Button(search_next_label.c_str()) || submit_search)
    {
        FindNextMatch();
        search_triggered = true;
    }

    if (!search_triggered && search_is_active && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
    {
        FindPreviousMatch();
    }

    if (!search_triggered && search_is_active && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
    {
        FindNextMatch();
    }

    ImGui::SameLine();
    const float current_x = ImGui::GetCursorPosX();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > save_reload_width)
    {
        ImGui::SetCursorPosX(current_x + avail - save_reload_width);
    }

    if (ImGui::Button(save_button_label.c_str()))
    {
        // Sync editor text to open_file_contents before saving
        state.open_file_contents = editor_->GetText();
        state.SaveOpenFile();
    }
    ImGui::SameLine();

    if (ImGui::Button(reload_button_label.c_str()))
    {
        state.OpenTextFile(state.open_file_path);
        loaded_path_.clear(); // force re-sync on next frame
    }

    ImGui::SameLine();
    if (!state.CanBuildProject())
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(build_button_label.c_str()))
    {
        state.TriggerBuildAction();
    }
    if (!state.CanBuildProject())
    {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    if (!state.CanPlayScene())
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(play_button_label.c_str()))
    {
        state.TriggerPlayAction();
    }
    if (!state.CanPlayScene())
    {
        ImGui::EndDisabled();
    }

    if (state.open_file_dirty)
    {
        ImGui::SameLine();
        ImGui::TextUnformatted("(modified)");
    }

    // Let search input consume keys while focused; otherwise editor handles keys.
    editor_->SetHandleKeyboardInputs(!search_is_active);
    if (focus_editor_ && !search_is_active)
    {
        ImGui::SetNextWindowFocus();
        focus_editor_ = false;
    }
    editor_->Render("##TextEditor", ImGui::GetContentRegionAvail());

    // Sync text changes back to engine state
    if (editor_->IsTextChanged())
    {
        state.open_file_contents = editor_->GetText();
        state.open_file_dirty = (state.open_file_contents != state.saved_file_contents);
    }
}
