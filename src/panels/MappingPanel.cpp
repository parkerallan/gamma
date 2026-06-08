#include "panels/MappingPanel.h"

#include "imgui.h"
#include "input/ControllerMapping.h"
#include "ui/Codicons.h"

#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// The mappable keyboard keys live in input::StandardKeys() so the editor and
// the runtime agree on the exact key list and their SDL scancodes.
static const std::vector<input::KeyDef>& kKeyboardKeys = input::StandardKeys();

int MappingPanel::KeyCount()
{
    return static_cast<int>(input::StandardKeys().size());
}

// ---------------------------------------------------------------------------
// Known controller device names shown in the dropdown
// ---------------------------------------------------------------------------
static void PollConnectedGamepads(std::vector<std::string>& out_names, std::vector<SDL_JoystickID>& out_ids)
{
    out_names.clear();
    out_ids.clear();

    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids)
        return;

    for (int i = 0; i < count; ++i)
    {
        const char* name = SDL_GetGamepadNameForID(ids[i]);
        out_names.push_back(name ? name : "Unknown Gamepad");
        out_ids.push_back(ids[i]);
    }
    SDL_free(ids);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void MappingPanel::StopMapping()
{
    if (active_gamepad_)
    {
        SDL_CloseGamepad(active_gamepad_);
        active_gamepad_ = nullptr;
    }
    mapping_active_     = false;
    mapping_single_row_ = false;
    mapping_row_        = 0;
    prev_button_state_.fill(false);
    prev_axis_state_.fill(0);
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void MappingPanel::Render(EngineState& state)
{
    if (!state.show_mapping_panel)
        return;

    const int key_count = static_cast<int>(kKeyboardKeys.size());
    if (static_cast<int>(state.key_controller_mappings.size()) < key_count)
        state.key_controller_mappings.resize(key_count);

    // Reload mappings from disk when the open project changes.
    if (state.project_root != loaded_project_root_)
    {
        loaded_project_root_ = state.project_root;
        LoadMappings(state);
        mappings_dirty_ = false;
    }

    if (!ImGui::Begin("Mapping", &state.show_mapping_panel))
    {
        ImGui::End();
        return;
    }

    // -----------------------------------------------------------------------
    // Toolbar
    // -----------------------------------------------------------------------
    // Refresh gamepad list when the combo is about to open
    const char* preview = (selected_device_index_ >= 0 && selected_device_index_ < static_cast<int>(device_names_.size()))
        ? device_names_[selected_device_index_].c_str()
        : "Select device...";

    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("##device_select", preview))
    {
        // Re-poll every time the combo opens so newly connected devices appear
        PollConnectedGamepads(device_names_, device_ids_);
        selected_device_index_ = -1; // reset if device list changed

        if (device_names_.empty())
        {
            ImGui::BeginDisabled();
            ImGui::TextUnformatted("No controllers detected");
            ImGui::EndDisabled();
        }
        else
        {
            for (int d = 0; d < static_cast<int>(device_names_.size()); ++d)
            {
                const bool sel = (d == selected_device_index_);
                if (ImGui::Selectable(device_names_[d].c_str(), sel))
                    selected_device_index_ = d;
                if (sel)
                    ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    // "Map Controller Inputs" / "Stop Mapping" button
    const bool has_device = (selected_device_index_ >= 0 && selected_device_index_ < static_cast<int>(device_names_.size()));
    if (has_device || mapping_active_)
    {
        ImGui::SameLine();
        if (mapping_active_)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("Stop Mapping"))
                StopMapping();
            ImGui::PopStyleColor(3);
        }
        else
        {
            if (ImGui::Button("Map All Inputs"))
            {
                StopMapping(); // close any previous session
                const SDL_JoystickID joy_id = device_ids_[selected_device_index_];
                active_gamepad_ = SDL_OpenGamepad(joy_id);
                if (active_gamepad_)
                {
                    // Snapshot current button + axis state
                    SDL_UpdateGamepads();
                    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
                        prev_button_state_[b] = SDL_GetGamepadButton(
                            active_gamepad_, static_cast<SDL_GamepadButton>(b));
                    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
                        prev_axis_state_[a] = SDL_GetGamepadAxis(
                            active_gamepad_, static_cast<SDL_GamepadAxis>(a));
                    mapping_active_     = true;
                    mapping_single_row_ = false;
                    mapping_row_        = 0;
                }
            }

            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("Clear All Inputs"))
            {
                for (auto& m : state.key_controller_mappings)
                    m.clear();
                for (auto& row : custom_rows_)
                    row.second.clear();
                mappings_dirty_ = true;
            }
            ImGui::PopStyleColor(3);
        }
    }

    // Save button pushed to the right
    {
        const float sp       = ImGui::GetStyle().ItemSpacing.x;
        const float save_w   = ImGui::CalcTextSize(ICON_CI_SAVE).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float cur_x    = ImGui::GetCursorPosX();
        const float avail    = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine();
        if (avail > save_w + sp)
            ImGui::SetCursorPosX(cur_x + avail - save_w);
        if (ImGui::Button(ICON_CI_SAVE))
        {
            SaveMappings(state);
            mappings_dirty_ = false;
        }
    }

    ImGui::Separator();

    // -----------------------------------------------------------------------
    // Per-frame controller polling during a mapping session
    // -----------------------------------------------------------------------
    if (mapping_active_ && active_gamepad_)
    {
        SDL_UpdateGamepads();

        const int total_rows = key_count + static_cast<int>(custom_rows_.size());

        // Check escape to cancel
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            StopMapping();
        }
        else
        {
            // Axis threshold: triggers use 0-32767, sticks use -32767..32767
            constexpr Sint16 kAxisThreshold = 16000;

            std::string captured_name;

            // --- Buttons ---
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
            {
                const bool now  = SDL_GetGamepadButton(active_gamepad_, static_cast<SDL_GamepadButton>(b));
                const bool prev = prev_button_state_[b];
                if (now && !prev && captured_name.empty())
                {
                    const char* n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(b));
                    captured_name = n ? n : "unknown";
                }
                prev_button_state_[b] = now;
            }

            // --- Axes (triggers + sticks) ---
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && captured_name.empty(); ++a)
            {
                const Sint16 now  = SDL_GetGamepadAxis(active_gamepad_, static_cast<SDL_GamepadAxis>(a));
                const Sint16 prev = prev_axis_state_[a];
                const char*  base = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(a));

                // Positive direction (includes triggers which only go 0→+)
                if (now > kAxisThreshold && prev <= kAxisThreshold)
                {
                    captured_name = base ? (std::string(base) + "+") : "unknown";
                }
                // Negative direction (sticks only)
                else if (now < -kAxisThreshold && prev >= -kAxisThreshold)
                {
                    captured_name = base ? (std::string(base) + "-") : "unknown";
                }

                prev_axis_state_[a] = now;
            }

            if (!captured_name.empty())
            {
                auto Append = [](std::string& existing, const std::string& input)
                {
                    if (existing.empty())
                        existing = input;
                    else
                        existing += ", " + input;
                };

                if (mapping_row_ < key_count)
                {
                    Append(state.key_controller_mappings[mapping_row_], captured_name);
                }
                else
                {
                    const int ci = mapping_row_ - key_count;
                    if (ci < static_cast<int>(custom_rows_.size()))
                        Append(custom_rows_[ci].second, captured_name);
                }

                mappings_dirty_ = true;
                ++mapping_row_;
                if (mapping_row_ >= total_rows || mapping_single_row_)
                    StopMapping();
            }
        }
    }

    // Helper: open/reopen the gamepad and start mapping at a specific row
    auto StartMappingAt = [&](int row, bool single)
    {
        StopMapping();
        const SDL_JoystickID joy_id = device_ids_[selected_device_index_];
        active_gamepad_ = SDL_OpenGamepad(joy_id);
        if (active_gamepad_)
        {
            SDL_UpdateGamepads();
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
                prev_button_state_[b] = SDL_GetGamepadButton(
                    active_gamepad_, static_cast<SDL_GamepadButton>(b));
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a)
                prev_axis_state_[a] = SDL_GetGamepadAxis(
                    active_gamepad_, static_cast<SDL_GamepadAxis>(a));
            mapping_active_     = true;
            mapping_single_row_ = single;
            mapping_row_        = row;
        }
    };

    // -----------------------------------------------------------------------
    // Mapping table inside a child so the toolbar stays fixed
    // -----------------------------------------------------------------------
    ImGui::BeginChild("##mapping_content", ImVec2(0, 0), true);

    constexpr ImGuiTableFlags table_flags =
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_ScrollY       |
        ImGuiTableFlags_SizingStretchSame;

    if (ImGui::BeginTable("##mapping_table", 2, table_flags))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Keyboard / Mouse", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        // --- Static key rows ---
        for (int i = 0; i < key_count; ++i)
        {
            ImGui::TableNextRow();

            const bool is_active_row = (mapping_active_ && mapping_row_ == i);
            if (is_active_row)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 200, 50, 60));

            // --- Keyboard column: clickable label ---
            ImGui::TableSetColumnIndex(0);
            {
                const ImVec2 cell_min = ImGui::GetCursorScreenPos();
                const float  cell_w   = ImGui::GetContentRegionAvail().x;
                const float  row_h    = ImGui::GetFrameHeight();

                // Invisible selectable covering the whole cell
                char sel_id[32];
                std::snprintf(sel_id, sizeof(sel_id), "##sel_%d", i);
                if (ImGui::Selectable(sel_id, is_active_row,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                    ImVec2(cell_w, row_h)))
                {
                    if (has_device)
                        StartMappingAt(i, true);
                }
                ImGui::SameLine();
                ImGui::SetCursorScreenPos(cell_min); // draw text on top
                ImGui::AlignTextToFramePadding();
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
                ImGui::TextUnformatted(kKeyboardKeys[i].label.data());
                ImGui::PopStyleColor();
            }

            ImGui::TableSetColumnIndex(1);
            char buf[64] = {};
            const std::string& binding = state.key_controller_mappings[i];
            if (binding.size() < sizeof(buf))
                binding.copy(buf, binding.size());
            ImGui::SetNextItemWidth(-FLT_MIN);
            char input_id[32];
            std::snprintf(input_id, sizeof(input_id), "##ctrl_%d", i);
            if (ImGui::InputText(input_id, buf, sizeof(buf)))
            {
                state.key_controller_mappings[i] = buf;
                mappings_dirty_ = true;
            }
            if (binding.empty())
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 mn = ImGui::GetItemRectMin();
                const ImVec2 mx = ImGui::GetItemRectMax();
                const float ty = mn.y + (mx.y - mn.y - ImGui::GetTextLineHeight()) * 0.5f;
                dl->AddText(ImVec2(mn.x + ImGui::GetStyle().FramePadding.x, ty),
                    IM_COL32(128, 128, 128, 180), "Not assigned");
            }
        }

        // --- Custom rows ---
        int row_to_remove = -1;
        const int custom_count = static_cast<int>(custom_rows_.size());
        for (int i = 0; i < custom_count; ++i)
        {
            const bool is_last         = (i == custom_count - 1);
            const bool is_active_custom = (mapping_active_ && mapping_row_ == key_count + i);
            ImGui::TableNextRow();

            if (is_active_custom)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(255, 200, 50, 60));

            // Keyboard column: clickable label + [- btn] [+ btn if last] [key name input]
            ImGui::TableSetColumnIndex(0);

            char rid[32];
            std::snprintf(rid, sizeof(rid), ICON_CI_REMOVE "##rem_%d", i);
            if (ImGui::Button(rid))
                row_to_remove = i;

            if (is_last)
            {
                ImGui::SameLine();
                if (ImGui::Button(ICON_CI_ADD "##add"))
                {
                    custom_rows_.push_back({"", ""});
                    mappings_dirty_ = true;
                }
                ImGui::SameLine();
            }
            else
            {
                ImGui::SameLine();
            }

            char kbuf[64] = {};
            custom_rows_[i].first.copy(kbuf, (std::min)(custom_rows_[i].first.size(), sizeof(kbuf) - 1));
            ImGui::SetNextItemWidth(-FLT_MIN);
            char kid[32];
            std::snprintf(kid, sizeof(kid), "##ckey_%d", i);
            if (ImGui::InputText(kid, kbuf, sizeof(kbuf)))
            {
                custom_rows_[i].first = kbuf;
                mappings_dirty_ = true;
            }

            // Controller binding column
            ImGui::TableSetColumnIndex(1);
            char cbuf[64] = {};
            custom_rows_[i].second.copy(cbuf, (std::min)(custom_rows_[i].second.size(), sizeof(cbuf) - 1));
            ImGui::SetNextItemWidth(-FLT_MIN);
            char cid[32];
            std::snprintf(cid, sizeof(cid), "##cctrl_%d", i);
            if (ImGui::InputText(cid, cbuf, sizeof(cbuf)))
            {
                custom_rows_[i].second = cbuf;
                mappings_dirty_ = true;
            }
            if (custom_rows_[i].second.empty())
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 mn = ImGui::GetItemRectMin();
                const ImVec2 mx = ImGui::GetItemRectMax();
                const float ty = mn.y + (mx.y - mn.y - ImGui::GetTextLineHeight()) * 0.5f;
                dl->AddText(ImVec2(mn.x + ImGui::GetStyle().FramePadding.x, ty),
                    IM_COL32(128, 128, 128, 180), "Not assigned");
            }
        }

        if (row_to_remove >= 0)
        {
            custom_rows_.erase(custom_rows_.begin() + row_to_remove);
            mappings_dirty_ = true;
        }

        // No custom rows yet: show a row with just the + button in the keyboard column
        if (custom_rows_.empty())
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Button(ICON_CI_ADD "##add_first"))
            {
                custom_rows_.push_back({"", ""});
                mappings_dirty_ = true;
            }
        }

        ImGui::EndTable();
    }

    ImGui::EndChild();

    // Auto-persist edits so play-in-editor and standalone builds pick them up
    // without requiring an explicit Save.
    if (mappings_dirty_ && !state.project_root.empty())
    {
        SaveMappings(state);
        mappings_dirty_ = false;
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
void MappingPanel::SaveMappings(EngineState& state) const
{
    if (state.project_root.empty())
        return;

    const std::filesystem::path config_dir = state.project_root / "Config";
    std::error_code ec;
    std::filesystem::create_directories(config_dir, ec);
    if (ec)
    {
        state.AddLog("Failed to create Config directory for input mappings");
        return;
    }

    const std::string contents = input::SerializeMappings(state.key_controller_mappings, custom_rows_);

    const std::filesystem::path file = config_dir / "input_mappings.ini";
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        state.AddLog("Failed to write input mappings file");
        return;
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

void MappingPanel::LoadMappings(EngineState& state)
{
    const int key_count = static_cast<int>(input::StandardKeys().size());
    state.key_controller_mappings.assign(key_count, std::string());
    custom_rows_.clear();

    if (state.project_root.empty())
        return;

    const std::filesystem::path file = state.project_root / "Config" / "input_mappings.ini";
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return;

    const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input::DeserializeMappings(contents, state.key_controller_mappings, custom_rows_);
}
