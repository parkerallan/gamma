#pragma once

#include "graph/GraphDocument.h"
#include "state/EngineState.h"

#include <memory>

namespace ImFlow
{
class ImNodeFlow;
}
struct GraphNodeDefinition;

class AnimatorPanel
{
public:
    void Render(EngineState& state);
    void Shutdown();

private:
    std::unique_ptr<ImFlow::ImNodeFlow> graph_;
    GraphDocument current_graph_document_;
    std::string saved_graph_contents_;

    ImFlow::ImNodeFlow& GetGraph();
    bool LoadGraphFile(EngineState& state, const std::filesystem::path& path);
    void HandleGraphSessionRequests(EngineState& state);
    void RebuildGraphFromDocument();
    void SyncGraphDocumentFromUi(EngineState& state);
    void RenderNodeLibrary();
    void RenderNodeLibrarySection(const GraphNodeDefinition& definition);
    void HandleGraphNodeDrop(EngineState& state);
    bool SaveOpenGraph(EngineState& state);
    bool ReloadOpenGraph(EngineState& state);
};
