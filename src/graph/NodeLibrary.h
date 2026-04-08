#pragma once

#include "ImNodeFlow.h"

#include <vector>

enum class GraphNodeType
{
    Value,
    Add,
    Preview,
};

class GraphValueNode : public ImFlow::BaseNode
{
public:
    GraphValueNode();

    void draw() override;

private:
    float value_ = 1.0f;
};

class GraphPreviewNode : public ImFlow::BaseNode
{
public:
    GraphPreviewNode();

    void draw() override;
};

class GraphAddNode : public ImFlow::BaseNode
{
public:
    GraphAddNode();

    void draw() override;
};

struct GraphNodeDefinition
{
    GraphNodeType type;
    const char* label;
    const char* section_name;
};

const std::vector<GraphNodeDefinition>& GetGraphNodeDefinitions();
const GraphNodeDefinition* FindGraphNodeDefinition(GraphNodeType node_type);
const char* GetGraphNodeLabel(GraphNodeType node_type);
void SpawnGraphNode(ImFlow::ImNodeFlow& graph, GraphNodeType node_type, const ImVec2& drop_position);
