#include "panels/LogPanel.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "ui/Codicons.h"

#include <cstdio>
#include <string>

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

    std::string log_text;
    std::size_t total_size = 0;
    for (const std::string& message : state.log_messages)
    {
        total_size += message.size() + 1;
    }
    log_text.reserve(total_size);
    for (const std::string& message : state.log_messages)
    {
        log_text.append(message);
        log_text.push_back('\n');
    }

    const float button_size = ImGui::GetFrameHeight();
    const float toolbar_width = button_size + ImGui::GetStyle().FramePadding.x * 2.0f;
    if (ImGui::BeginTable("##LogPanelLayout", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoPadInnerX | ImGuiTableFlags_NoPadOuterX))
    {
        ImGui::TableSetupColumn("Toolbar", ImGuiTableColumnFlags_WidthFixed, toolbar_width);
        ImGui::TableSetupColumn("Log", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextColumn();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        if (ImGui::Button(ICON_CI_CLEAR_ALL, ImVec2(button_size, button_size)))
        {
            state.log_messages.clear();
        }
        ImGui::PopStyleVar(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Clear All Logs");
        }

        const char* auto_scroll_icon = state.auto_scroll_log ? ICON_CI_REFRESH : ICON_CI_SYNC_IGNORED;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
        if (ImGui::Button(auto_scroll_icon, ImVec2(button_size, button_size)))
        {
            state.auto_scroll_log = !state.auto_scroll_log;
        }
        ImGui::PopStyleVar(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("Auto-scroll: %s", state.auto_scroll_log ? "On" : "Off");
        }

        ImGui::TableNextColumn();
        const ImGuiID log_text_id = ImGui::GetCurrentWindow()->GetID("##LogText");
        ImGui::InputTextMultiline(
            "##LogText",
            log_text.data(),
            log_text.size() + 1,
            ImVec2(-1.0f, -1.0f),
            ImGuiInputTextFlags_ReadOnly);

        if (state.auto_scroll_log)
        {
            char child_name[256];
            std::snprintf(child_name, sizeof(child_name), "Log/##LogText_%08X", log_text_id);
            if (ImGuiWindow* log_child = ImGui::FindWindowByName(child_name))
            {
                ImGui::SetScrollY(log_child, log_child->ScrollMax.y);
            }
        }

        ImGui::EndTable();
    }

    ImGui::End();
}