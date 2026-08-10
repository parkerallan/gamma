#pragma once

#include "state/EngineState.h"

#include "imgui.h"

#include <vector>

class LogPanel
{
public:
    void Render(EngineState& state);

private:
    // Line selection: drag box-select, shift-range, ctrl-toggle and Ctrl+A, all
    // from ImGui's multi-select. Selection is keyed by applog entry id rather
    // than by row, so toggling a level filter doesn't reshuffle what's
    // highlighted; m_visible maps a drawn row back to its entry index.
    ImGuiSelectionBasicStorage m_selection;
    std::vector<int>           m_visible;
    ImGuiID                    m_first_id = 0;
    int                        m_last_count = 0;
};
