#pragma once

#include "graph/GraphDocument.h"

#include <memory>

struct EngineState;
namespace ImFlow
{
class ImNodeFlow;
}
struct GraphNodeDefinition;

class NodeGraphComponent
{
public:
    NodeGraphComponent();
    ~NodeGraphComponent();

    NodeGraphComponent(const NodeGraphComponent&) = delete;
    NodeGraphComponent& operator=(const NodeGraphComponent&) = delete;

    void Render(EngineState& state);
    void Shutdown();
    bool SaveOpenGraph(EngineState& state);
    bool ReloadOpenGraph(EngineState& state);

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
};
