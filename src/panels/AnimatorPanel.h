#pragma once

#include "state/EngineState.h"

class AnimatorPanel
{
public:
    void Render(EngineState& state);
    void Shutdown();

private:
    void RenderNodeLibrary();
};
