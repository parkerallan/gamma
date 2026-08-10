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

EditorComponent::EditorComponent() = default;
EditorComponent::~EditorComponent() = default;

std::string EditorComponent::NormalizeNewlines(const std::string& raw)
{
    // Normalize CRLF to LF so the editor round-trips Windows files cleanly.
    std::string normalized;
    normalized.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n')
        {
            continue;
        }
        normalized += raw[i];
    }
    return normalized;
}

bool EditorComponent::FindNextMatch(Document& document)
{
    const std::string query(document.search_query.data());
    if (query.empty())
    {
        return false;
    }

    std::string haystack = document.editor->GetText();
    if (haystack.empty())
    {
        return false;
    }

    const std::string needle = ToLowerCopy(query);
    const std::string haystack_lower = ToLowerCopy(haystack);

    if (document.last_search_query != needle)
    {
        document.last_search_query = needle;
        document.last_match_offset = std::string::npos;
        document.last_search_direction = 0;
    }

    std::size_t search_start = 0;
    if (document.last_match_offset != std::string::npos)
    {
        if (document.last_search_direction > 0)
        {
            search_start = document.last_match_offset + query.size();
        }
        else
        {
            search_start = document.last_match_offset + 1;
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
    document.editor->SetCursorPosition(selection_start);
    document.editor->SetSelection(selection_start, selection_end);
    document.last_match_offset = pos;
    document.last_search_direction = 1;
    return true;
}

bool EditorComponent::FindPreviousMatch(Document& document)
{
    const std::string query(document.search_query.data());
    if (query.empty())
    {
        return false;
    }

    std::string haystack = document.editor->GetText();
    if (haystack.empty())
    {
        return false;
    }

    const std::string needle = ToLowerCopy(query);
    const std::string haystack_lower = ToLowerCopy(haystack);

    if (document.last_search_query != needle)
    {
        document.last_search_query = needle;
        document.last_match_offset = std::string::npos;
        document.last_search_direction = 0;
    }

    std::size_t search_from = std::string::npos;
    if (document.last_match_offset != std::string::npos && document.last_match_offset != 0)
    {
        search_from = document.last_match_offset - 1;
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
    document.editor->SetCursorPosition(selection_start);
    document.editor->SetSelection(selection_start, selection_end);
    document.last_match_offset = pos;
    document.last_search_direction = -1;
    return true;
}

void EditorComponent::ApplyLanguage(TextEditor& editor, const std::filesystem::path& path)
{
    const std::string ext = ToLowerCopy(path.extension().string());
    if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".cc" || ext == ".cxx")
        editor.SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());
    else if (ext == ".glsl" || ext == ".vert" || ext == ".frag" ||
             ext == ".rgen" || ext == ".rmiss" || ext == ".rchit" || ext == ".rahit")
        editor.SetLanguageDefinition(TextEditor::LanguageDefinition::GLSL());
    else if (ext == ".lua")
        editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    else if (ext == ".c")
        editor.SetLanguageDefinition(TextEditor::LanguageDefinition::C());
}

EditorComponent::Document* EditorComponent::FindDocument(const std::filesystem::path& path)
{
    for (Document& document : documents_)
    {
        if (document.path == path)
        {
            return &document;
        }
    }
    return nullptr;
}

void EditorComponent::OpenDocument(EngineState& state)
{
    pending_focus_path_ = state.open_file_path;

    // Already open: just bring its tab forward.
    if (Document* existing = FindDocument(state.open_file_path))
    {
        // The file may have been re-read from disk (Reload, or a scene write
        // from the attribute editor) while the tab sat in the background.
        if (existing->saved_contents != state.saved_file_contents)
        {
            existing->editor->SetText(NormalizeNewlines(state.saved_file_contents));
            existing->saved_contents = state.saved_file_contents;
            existing->dirty = false;
        }
        return;
    }

    Document document;
    document.path = state.open_file_path;
    document.editor = std::make_unique<TextEditor>();
    document.editor->SetShowWhitespaces(false);
    document.editor->SetReadOnly(false);
    document.editor->SetHandleKeyboardInputs(true);
    document.editor->SetHandleMouseInputs(true);
    document.editor->SetImGuiChildIgnored(false);
    document.editor->SetText(NormalizeNewlines(state.open_file_contents));
    ApplyLanguage(*document.editor, document.path);
    document.saved_contents = state.saved_file_contents;
    document.dirty = state.open_file_dirty;

    documents_.push_back(std::move(document));
}

void EditorComponent::ActivateDocument(Document& document, EngineState& state)
{
    active_path_ = document.path;
    state.open_file_path = document.path;
    state.saved_file_contents = document.saved_contents;
    state.open_file_contents = document.editor->GetText();
    state.open_file_dirty = document.dirty;
}

void EditorComponent::CloseDocument(std::size_t index, EngineState& state)
{
    const Document& document = documents_[index];
    const bool was_active = document.path == active_path_;
    if (document.dirty)
    {
        state.AddWarning("Closed without saving: " + state.GetDisplayPath(document.path));
    }
    if (pending_focus_path_ == document.path)
    {
        pending_focus_path_.clear();
    }

    documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));

    if (!was_active)
    {
        return;
    }

    if (documents_.empty())
    {
        active_path_.clear();
        state.open_file_path.clear();
        state.open_file_contents.clear();
        state.saved_file_contents.clear();
        state.open_file_dirty = false;
        return;
    }

    const std::size_t next_index = std::min(index, documents_.size() - 1);
    ActivateDocument(documents_[next_index], state);
}

void EditorComponent::Render(EngineState& state)
{
    // A panel asked for a file to be opened (selection, double-click, or the
    // "Open in Editor" menu item), which shows up as a change to open_file_path.
    if (state.HasOpenFile() && state.open_file_path != active_path_)
    {
        OpenDocument(state);
        active_path_ = state.open_file_path;
    }

    if (documents_.empty())
    {
        ImGui::TextUnformatted("Text Editor");
        ImGui::Separator();
        ImGui::TextWrapped("Select a supported text file from the Files or Assets panel to open it here.");
        return;
    }

    if (ImGui::BeginTabBar(
            "##EditorTabs",
            ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs |
                ImGuiTabBarFlags_FittingPolicyScroll))
    {
        for (std::size_t index = 0; index < documents_.size();)
        {
            Document& document = documents_[index];
            ImGui::PushID(static_cast<int>(index));

            bool keep_open = true;
            const bool wants_focus = !pending_focus_path_.empty() && document.path == pending_focus_path_;
            const ImGuiTabItemFlags flags = wants_focus ? ImGuiTabItemFlags_SetSelected : 0;

            const std::string label = document.path.filename().string() + (document.dirty ? " *" : "");
            if (ImGui::BeginTabItem(label.c_str(), &keep_open, flags))
            {
                // While a focus change is queued, the tab bar still shows the
                // outgoing tab; leave state pointed at the incoming document.
                const bool focus_pending_elsewhere = !pending_focus_path_.empty() && !wants_focus;
                if (!focus_pending_elsewhere)
                {
                    pending_focus_path_.clear();
                    if (document.path != active_path_)
                    {
                        ActivateDocument(document, state);
                    }
                }

                RenderDocument(document, state, document.path == active_path_);
                ImGui::EndTabItem();
            }

            ImGui::PopID();

            if (!keep_open)
            {
                CloseDocument(index, state);
            }
            else
            {
                ++index;
            }
        }

        ImGui::EndTabBar();
    }
}

void EditorComponent::RenderDocument(Document& document, EngineState& state, bool is_active)
{
    if (!is_active)
    {
        // Transitional frame: EngineState's open-file fields belong to another
        // document right now, so draw the text and touch nothing else.
        document.editor->Render("##TextEditor", ImGui::GetContentRegionAvail());
        return;
    }

    // The file was re-read from disk behind this tab's back.
    if (document.saved_contents != state.saved_file_contents)
    {
        if (document.editor->GetText() != NormalizeNewlines(state.saved_file_contents))
        {
            document.editor->SetText(NormalizeNewlines(state.saved_file_contents));
        }
        document.saved_contents = state.saved_file_contents;
        document.dirty = false;
    }

    // Search controls on the left, Save/Reload/Build/Play on the right. The
    // tab label already names the file, so the path isn't repeated here.
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
        document.search_query.data(),
        document.search_query.size(),
        ImGuiInputTextFlags_EnterReturnsTrue);
    const bool search_is_active = ImGui::IsItemActive() || ImGui::IsItemFocused();
    ImGui::SameLine();

    bool search_triggered = false;
    if (ImGui::Button(search_previous_label.c_str()))
    {
        FindPreviousMatch(document);
        search_triggered = true;
    }
    ImGui::SameLine();

    if (ImGui::Button(search_next_label.c_str()) || submit_search)
    {
        FindNextMatch(document);
        search_triggered = true;
    }

    if (!search_triggered && search_is_active && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false))
    {
        FindPreviousMatch(document);
    }

    if (!search_triggered && search_is_active && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false))
    {
        FindNextMatch(document);
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
        state.open_file_contents = document.editor->GetText();
        if (state.SaveOpenFile())
        {
            document.saved_contents = state.saved_file_contents;
            document.dirty = false;
        }
    }
    ImGui::SameLine();

    if (ImGui::Button(reload_button_label.c_str()))
    {
        if (state.OpenTextFile(document.path))
        {
            document.editor->SetText(NormalizeNewlines(state.saved_file_contents));
            document.saved_contents = state.saved_file_contents;
            document.dirty = false;
        }
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

    if (document.dirty)
    {
        ImGui::SameLine();
        ImGui::TextUnformatted("(modified)");
    }

    // Let search input consume keys while focused; otherwise editor handles keys.
    document.editor->SetHandleKeyboardInputs(!search_is_active);
    document.editor->Render("##TextEditor", ImGui::GetContentRegionAvail());

    // Sync text changes back to engine state so Ctrl+S / File > Save, which
    // both go through SaveOpenFile, write what is on screen.
    if (document.editor->IsTextChanged())
    {
        state.open_file_contents = document.editor->GetText();
        state.open_file_dirty = (state.open_file_contents != state.saved_file_contents);
        document.dirty = state.open_file_dirty;
    }
}
