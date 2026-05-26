#pragma once

#include "state/EngineState.h"
#include "render/EffectsPreviewRenderer.h"

class VulkanContext;

class EffectsPanel
{
public:
    void Render(EngineState& state, VulkanContext* vulkan_context);

private:
    EffectsPreviewRenderer preview_renderer_;
};
