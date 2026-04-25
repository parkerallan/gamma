#pragma once

#include "state/EngineState.h"

class BuildGameDialog
{
public:
    void Open(const EngineState& state);
    bool Render(EngineState& state);

private:
    int build_type_index_ = 0;

    bool SubmitBuildRequest(EngineState& state);
};