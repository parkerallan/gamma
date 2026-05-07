#pragma once

struct EngineState;

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
};
