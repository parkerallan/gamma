#pragma once

#include "state/EngineState.h"
#include "graph/NodeLibrary.h"

#include <memory>

class WorkspacePanel
{
public:
    WorkspacePanel();
    ~WorkspacePanel();

    WorkspacePanel(const WorkspacePanel&) = delete;
    WorkspacePanel& operator=(const WorkspacePanel&) = delete;

    void Render(EngineState& state);
    void Shutdown();

private:
    std::unique_ptr<ImFlow::ImNodeFlow> graph_;

    ImFlow::ImNodeFlow& GetGraph();
    void RenderSceneViewport();
    void RenderGraphViewport(EngineState& state);
    void RenderEditorViewport(EngineState& state);
    void RenderNodeLibrary();
    void RenderNodeLibrarySection(const GraphNodeDefinition& definition);
    void HandleGraphNodeDrop(EngineState& state);
};