#include "panels/LogPanel.h"

#include "imgui.h"

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

    if (ImGui::Button("Clear"))
    {
        state.log_messages.clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &state.auto_scroll_log);
    ImGui::Separator();

    ImGui::BeginChild("LogScrollRegion", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    for (const std::string& message : state.log_messages)
    {
        ImGui::TextUnformatted(message.c_str());
    }

    if (state.auto_scroll_log && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
    {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
    ImGui::End();
}