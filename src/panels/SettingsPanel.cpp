#include "panels/SettingsPanel.h"

#include "components/BuildSettingsComponent.h"
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

    bool settings_changed = false;
    bool build_changed = false;

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
        settings_changed |= ImGui::Checkbox("Auto-open startup scene", &state.auto_open_startup_scene);
        settings_changed |= ImGui::Checkbox("Confirm before delete", &state.confirm_before_delete);
        settings_changed |= ImGui::Checkbox("Highlight drop targets", &state.highlight_drop_targets);
        ImGui::Spacing();
        if (ImGui::SmallButton("Revert to defaults##project"))
        {
            state.auto_open_startup_scene = true;
            state.confirm_before_delete = true;
            state.highlight_drop_targets = true;
            settings_changed = true;
        }
    }

    if (ImGui::CollapsingHeader("Viewport", ImGuiTreeNodeFlags_DefaultOpen))
    {
        settings_changed |= ImGui::Checkbox("Show grid overlay", &state.show_grid_overlay);
        settings_changed |= ImGui::Checkbox("Snap to grid", &state.snap_to_grid);
        settings_changed |= ImGui::SliderFloat("Grid spacing", &state.grid_size, 0.125f, 16.0f, "%.3f u", ImGuiSliderFlags_Logarithmic);
        settings_changed |= ImGui::SliderFloat("Grid bounds", &state.grid_extent, 8.0f, 1024.0f, "%.1f u", ImGuiSliderFlags_Logarithmic);
        ImGui::TextDisabled("Acts as the minimum viewport grid extent. Large scenes can still expand it further.");
        ImGui::Spacing();
        if (ImGui::SmallButton("Revert to defaults##viewport"))
        {
            state.show_grid_overlay = true;
            state.snap_to_grid = true;
            state.grid_size = 1.0f;
            state.grid_extent = 64.0f;
            settings_changed = true;
        }
    }

    if (ImGui::CollapsingHeader("Editor", ImGuiTreeNodeFlags_DefaultOpen))
    {
        settings_changed |= ImGui::Checkbox("Wrap text", &state.wrap_editor_text);
        settings_changed |= ImGui::Checkbox("Auto-save on focus loss", &state.auto_save_on_focus_loss);
        settings_changed |= ImGui::SliderInt("Auto-save interval", &state.auto_save_interval_minutes, 1, 30, "%d min");
        settings_changed |= ImGui::SliderFloat("UI scale", &state.ui_scale, 0.8f, 1.5f, "%.2fx");
        ImGui::TextDisabled("UI scale is stored here for future runtime styling support.");
        ImGui::Spacing();
        if (ImGui::SmallButton("Revert to defaults##editor"))
        {
            state.wrap_editor_text = false;
            state.auto_save_on_focus_loss = false;
            state.auto_save_interval_minutes = 5;
            state.ui_scale = 1.0f;
            settings_changed = true;
        }
    }

    if (ImGui::CollapsingHeader("Build", ImGuiTreeNodeFlags_DefaultOpen))
    {
        build_changed = BuildSettingsComponent::Render(state);
        ImGui::Spacing();
        if (ImGui::SmallButton("Revert to defaults##build"))
        {
            const std::string project_name = state.HasOpenProject()
                ? state.project_file_path.stem().string()
                : "Game";
            state.build_executable_name = project_name;
            state.build_folder_name = project_name;
            state.build_window_title = project_name;
            state.build_target_platform = EngineBuildPlatform::Windows;
            state.build_output_root.clear();
            state.build_app_icon_path.clear();
            build_changed = true;
        }
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

    if (settings_changed && state.HasOpenProject())
    {
        state.SaveProjectSettings();
    }

    if (build_changed && state.HasOpenProject())
    {
        state.SaveBuildSettingsToProject();
    }

    ImGui::End();
}