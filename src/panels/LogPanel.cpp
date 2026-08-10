#include "panels/LogPanel.h"

#include "core/Log.h"

#include "imgui.h"
#include "ui/Codicons.h"

#include <string>

namespace
{
    const char* Tag(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Error:   return "[ERROR] ";
        case LogLevel::Warning: return "[WARN]  ";
        case LogLevel::Build:   return "[BUILD] ";
        case LogLevel::Script:  return "[LOG]   ";
        default:                return "[INFO]  ";
        }
    }

    ImVec4 Color(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Error:   return ImVec4(0.95f, 0.42f, 0.38f, 1.0f);
        case LogLevel::Warning: return ImVec4(0.95f, 0.75f, 0.25f, 1.0f);
        case LogLevel::Build:   return ImVec4(0.45f, 0.72f, 1.00f, 1.0f);
        case LogLevel::Script:  return ImVec4(0.55f, 0.85f, 0.65f, 1.0f);
        default:                return ImGui::GetStyleColorVec4(ImGuiCol_Text);
        }
    }

    bool* FilterFlag(EngineState& state, LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Error:   return &state.log_show_error;
        case LogLevel::Warning: return &state.log_show_warning;
        case LogLevel::Build:   return &state.log_show_build;
        case LogLevel::Script:  return &state.log_show_script;
        default:                return &state.log_show_info;
        }
    }

    void FilterCheckbox(EngineState& state, LogLevel level, const char* label, int count)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Color(level));
        ImGui::Checkbox(label, FilterFlag(state, level));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%d", count);
    }
}

void LogPanel::Render(EngineState& state)
{
    if (!state.show_log_panel)
    {
        return;
    }

    if (!ImGui::Begin("Log", &state.show_log_panel))
    {
        ImGui::End();
        return;
    }

    const std::vector<LogEntry>& entries = applog::Entries();
    const ImGuiID first_id = static_cast<ImGuiID>(applog::FirstId());

    // Toolbar: clear, auto-scroll toggle, level filters.
    const float button = ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    if (ImGui::Button(ICON_CI_CLEAR_ALL, ImVec2(button, button)))
    {
        applog::Clear();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::SetTooltip("Clear All Logs");
    }

    ImGui::SameLine();
    const char* auto_scroll_icon = state.auto_scroll_log ? ICON_CI_REFRESH : ICON_CI_SYNC_IGNORED;
    if (ImGui::Button(auto_scroll_icon, ImVec2(button, button)))
    {
        state.auto_scroll_log = !state.auto_scroll_log;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::SetTooltip("Auto-scroll: %s", state.auto_scroll_log ? "On" : "Off");
    }

    const bool all_levels_shown = state.log_show_info && state.log_show_warning &&
                                  state.log_show_error && state.log_show_build &&
                                  state.log_show_script;
    ImGui::SameLine();
    if (ImGui::Button(all_levels_shown ? ICON_CI_FILTER : ICON_CI_FILTER_FILLED, ImVec2(button, button)))
    {
        ImGui::OpenPopup("##logfilter");
    }
    ImGui::PopStyleVar(2);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::SetTooltip(all_levels_shown ? "Filter levels" : "Filter levels (active)");
    }

    if (ImGui::BeginPopup("##logfilter"))
    {
        int counts[5] = {0, 0, 0, 0, 0};
        for (const LogEntry& entry : entries)
        {
            ++counts[static_cast<int>(entry.level)];
        }
        FilterCheckbox(state, LogLevel::Info, "Info", counts[static_cast<int>(LogLevel::Info)]);
        FilterCheckbox(state, LogLevel::Warning, "Warnings", counts[static_cast<int>(LogLevel::Warning)]);
        FilterCheckbox(state, LogLevel::Error, "Errors", counts[static_cast<int>(LogLevel::Error)]);
        FilterCheckbox(state, LogLevel::Build, "Build", counts[static_cast<int>(LogLevel::Build)]);
        FilterCheckbox(state, LogLevel::Script, "Script", counts[static_cast<int>(LogLevel::Script)]);
        ImGui::EndPopup();
    }

    ImGui::Separator();

    if (ImGui::BeginChild("##logscroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        const int count = static_cast<int>(entries.size());

        // Entry ids are never reused, so a stored id stays valid as the log
        // grows and trims. A clear does leave the selection pointing at ids
        // that no longer exist, though, so drop it when the list shrinks.
        if (count < m_last_count)
        {
            m_selection.Clear();
        }
        m_last_count = count;

        m_visible.clear();
        for (int i = 0; i < count; ++i)
        {
            if (*FilterFlag(state, entries[i].level))
            {
                m_visible.push_back(i);
            }
        }

        // The drawn rows are a filtered subset, so the adapter maps a row
        // position to the entry id the selection is actually keyed by.
        // ApplyRequests runs every range/select-all through it.
        m_first_id = first_id;
        m_selection.UserData = this;
        m_selection.AdapterIndexToStorageId =
            [](ImGuiSelectionBasicStorage* self, int row) -> ImGuiID
            {
                const LogPanel* panel = static_cast<const LogPanel*>(self->UserData);
                return panel->m_first_id + static_cast<ImGuiID>(panel->m_visible[row]);
            };

        ImGuiMultiSelectIO* ms = ImGui::BeginMultiSelect(
            ImGuiMultiSelectFlags_BoxSelect1d | ImGuiMultiSelectFlags_ClearOnEscape |
                ImGuiMultiSelectFlags_ClearOnClickVoid,
            m_selection.Size, static_cast<int>(m_visible.size()));
        m_selection.ApplyRequests(ms);

        for (int row = 0; row < static_cast<int>(m_visible.size()); ++row)
        {
            const int index = m_visible[row];
            const LogEntry& entry = entries[index];
            ImGui::SetNextItemSelectionUserData(row);
            ImGui::PushID(index);
            // The Selectable carries an empty label: log text can contain '##',
            // which a label would read as an ID separator and hide everything
            // after it. The real text is drawn back over the row instead.
            const ImVec2 row_pos = ImGui::GetCursorPos();
            ImGui::Selectable("##row", m_selection.Contains(first_id + static_cast<ImGuiID>(index)));
            ImGui::SetCursorPos(row_pos);
            ImGui::PushStyleColor(ImGuiCol_Text, Color(entry.level));
            ImGui::TextUnformatted(Tag(entry.level));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextUnformatted(entry.text.c_str());
            ImGui::PopStyleColor();
            ImGui::PopID();
        }

        ms = ImGui::EndMultiSelect();
        m_selection.ApplyRequests(ms);

        // Ctrl+C is the expected gesture but invisible, so the right-click menu
        // offers the same thing.
        bool copy = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, ImGuiInputFlags_RouteFocused);
        if (ImGui::BeginPopupContextWindow("##logmenu"))
        {
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, m_selection.Size > 0))
            {
                copy = true;
            }
            if (ImGui::MenuItem("Select All", "Ctrl+A", false, !m_visible.empty()))
            {
                for (int row = 0; row < static_cast<int>(m_visible.size()); ++row)
                {
                    m_selection.SetItemSelected(first_id + static_cast<ImGuiID>(m_visible[row]), true);
                }
            }
            ImGui::EndPopup();
        }

        if (copy && m_selection.Size > 0)
        {
            // Walk rows, not the selection storage, so the clipboard comes out
            // in the order the lines are shown.
            std::string text;
            for (int row = 0; row < static_cast<int>(m_visible.size()); ++row)
            {
                const int index = m_visible[row];
                if (!m_selection.Contains(first_id + static_cast<ImGuiID>(index)))
                {
                    continue;
                }
                text += Tag(entries[index].level);
                text += entries[index].text;
                text += '\n';
            }
            ImGui::SetClipboardText(text.c_str());
        }

        if (state.auto_scroll_log && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    ImGui::End();
}
