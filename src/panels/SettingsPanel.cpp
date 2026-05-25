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

    if (ImGui::CollapsingHeader("Graph", ImGuiTreeNodeFlags_DefaultOpen))
    {
        settings_changed |= ImGui::Checkbox("Show Transpiled Lua", &state.show_transpiled_lua);
        ImGui::TextDisabled("When enabled, every graph's transpiled Lua is written to\nProject/Graphs/Transpiled/ on Play so the folder appears in the file tree.\nDisabling removes the folder.");
        ImGui::Spacing();
        if (ImGui::SmallButton("Revert to defaults##graph"))
        {
            state.show_transpiled_lua = false;
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
            state.build_target_type = EngineBuildType::Debug;
            state.build_target_platform = EngineBuildPlatform::Windows;
            state.build_output_root.clear();
            state.build_app_icon_path.clear();
            build_changed = true;
        }
    }

    static bool show_debug_options = false;
    ImGui::Checkbox("Show Debug Options", &show_debug_options);

    if (show_debug_options && ImGui::CollapsingHeader("Temporal AA (debug)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        settings_changed |= ImGui::Checkbox("Enable TAA", &state.taa_enabled);

        static const char* kVizModes[] = {
            "0 - Normal output",
            "1 - Motion vectors (color)",
            "2 - Pixel weight (gray)",
            "3 - Jitter offset (color)",
            "4 - Current frame only",
            "5 - History only",
            "6 - Weight decomposition (R=disocc, G=motion, B=sample)",
        };
        const int viz_count = static_cast<int>(sizeof(kVizModes) / sizeof(kVizModes[0]));
        if (state.taa_viz_mode < 0) state.taa_viz_mode = 0;
        if (state.taa_viz_mode >= viz_count) state.taa_viz_mode = viz_count - 1;
        settings_changed |= ImGui::Combo("Visualization", &state.taa_viz_mode, kVizModes, viz_count);

        settings_changed |= ImGui::SliderFloat("Variance scale", &state.taa_variance_scale, 0.0f, 4.0f, "%.3f");
        ImGui::TextDisabled("3x3 NCC clamp width on static pixels. Lower = tighter (less ghosting, more flicker).");

        settings_changed |= ImGui::SliderFloat("Variance scale (moving)", &state.taa_variance_scale_moving, 0.0f, 4.0f, "%.3f");
        ImGui::TextDisabled("Clamp width on fast-moving pixels. Lerped via motion weight. Lower = kills directional flow.");

        settings_changed |= ImGui::SliderFloat("Anti-sparkle", &state.taa_anti_sparkle, 0.0f, 1.0f, "%.3f");
        ImGui::TextDisabled("Per-pixel firefly clamp (flt_taa_anti_sparkle).");

        settings_changed |= ImGui::SliderFloat("History blend (max)", &state.taa_history_blend, 0.0f, 1.0f, "%.3f");
        ImGui::TextDisabled("Max weight of current frame into history (default 0.10 = 10%% current).");

        settings_changed |= ImGui::SliderFloat("Jitter compensation", &state.taa_jitter_compensation, 0.0f, 1.0f, "%.3f");
        ImGui::TextDisabled("0 = legacy motion vectors. 1 = subtract jitter_curr from current UV (live diagnose).");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Adaptive supersampling (undersampled-pixel fix)");
        settings_changed |= ImGui::Checkbox("Enable adaptive sampling", &state.taa_adaptive_enabled);
        ImGui::TextDisabled("Probes 5-tap luma contrast of last frame; high-contrast pixels get extra rays.");
        settings_changed |= ImGui::SliderInt("Max samples per pixel", &state.taa_adaptive_max_samples, 1, 8);
        ImGui::TextDisabled("Each extra sample is a full primary ray (shading + shadows). 2 is usually enough.");
        settings_changed |= ImGui::SliderFloat("Contrast threshold", &state.taa_adaptive_threshold, 0.01f, 1.0f, "%.3f");
        ImGui::TextDisabled("Higher => fewer pixels qualify (cheaper). 0.25 default. Drop to 0.15 if shimmer remains.");
        settings_changed |= ImGui::SliderFloat("Feature preservation", &state.taa_adaptive_preservation, 0.0f, 1.0f, "%.3f");
        ImGui::TextDisabled("0 = pure average (clean, but subpixel strands look semi-transparent).\n1 = bias toward brightest sample where samples disagree (keeps hair opaque, may amplify HDR fireflies).");

        ImGui::Spacing();
        if (ImGui::SmallButton("Revert TAA defaults"))
        {
            state.taa_enabled = true;
            state.taa_viz_mode = 0;
            state.taa_variance_scale = 1.25f;
            state.taa_variance_scale_moving = 0.75f;
            state.taa_anti_sparkle = 0.25f;
            state.taa_history_blend = 0.1f;
            state.taa_jitter_compensation = 0.0f;
            state.taa_adaptive_enabled = false;
            state.taa_adaptive_max_samples = 2;
            state.taa_adaptive_threshold = 0.25f;
            state.taa_adaptive_preservation = 0.7f;
            settings_changed = true;
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