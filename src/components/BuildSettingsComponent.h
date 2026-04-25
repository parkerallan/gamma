#pragma once

#include "state/EngineState.h"

class BuildSettingsComponent
{
public:
    // Returns true if any field was modified this frame.
    static bool Render(EngineState& state);
};
