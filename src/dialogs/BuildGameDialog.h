#pragma once

#include "state/EngineState.h"

class BuildGameDialog
{
public:
    void Open(const EngineState& state);
    bool Render(EngineState& state);

private:
    bool SubmitBuildRequest(EngineState& state);
};