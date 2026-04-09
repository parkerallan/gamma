#include "panels/SettingsPanel.h"

#include "imgui.h"

void SettingsPanel::Render(EngineState& state)
{
    if (!state.show_settings_panel)
    {
        return;
    }

    if (!ImGui::Begin("Settings", &state.show_settings_panel))
    {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Project", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextUnformatted(state.HasOpenProject() ? state.GetOpenProjectDisplayPath().c_str() : "No project loaded");
        ImGui::Separator();
        if (ImGui::Button("Open Project"))
        {
            state.request_open_project_dialog = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("New Project"))
        {
            state.request_new_project_dialog = true;
        }
        ImGui::Separator();
        ImGui::Checkbox("Auto-open startup scene", &state.auto_open_startup_scene);
        ImGui::Checkbox("Confirm before delete", &state.confirm_before_delete);
        ImGui::Checkbox("Highlight drop targets", &state.highlight_drop_targets);
    }

    if (ImGui::CollapsingHeader("Viewport", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Show grid overlay", &state.show_grid_overlay);
        ImGui::Checkbox("Snap to grid", &state.snap_to_grid);
        ImGui::SliderFloat("Grid size", &state.grid_size, 8.0f, 128.0f, "%.0f px");
    }

    if (ImGui::CollapsingHeader("Editor", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Wrap text", &state.wrap_editor_text);
        ImGui::Checkbox("Auto-save on focus loss", &state.auto_save_on_focus_loss);
        ImGui::SliderInt("Auto-save interval", &state.auto_save_interval_minutes, 1, 30, "%d min");
        ImGui::SliderFloat("UI scale", &state.ui_scale, 0.8f, 1.5f, "%.2fx");
        ImGui::TextDisabled("UI scale is stored here for future runtime styling support.");
    }

    if (ImGui::CollapsingHeader("Session", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("Open file: %s", state.HasOpenFile() ? state.GetOpenFileDisplayPath().c_str() : "None");
        ImGui::Text("Log entries: %d", static_cast<int>(state.log_messages.size()));
        if (ImGui::Button("Add test log entry"))
        {
            state.AddLog("Settings test message");
        }
    }

    ImGui::End();
}