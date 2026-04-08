#include "graph/NodeLibrary.h"

#include "imgui.h"

#include <vector>

namespace
{
const std::vector<GraphNodeDefinition> kGraphNodeDefinitions = {
    {GraphNodeType::Value, "Value", "Generators"},
    {GraphNodeType::Add, "Add", "Math"},
    {GraphNodeType::Preview, "Preview", "Outputs"},
};
}

GraphValueNode::GraphValueNode()
{
    setTitle("Value");
    setStyle(ImFlow::NodeStyle::green());
    addOUT<float>("Value", ImFlow::PinStyle::green())->behaviour([this]() { return value_; });
}

void GraphValueNode::draw()
{
    ImGui::SetNextItemWidth(130.0f);
    ImGui::SliderFloat("##GraphValue", &value_, 0.0f, 10.0f, "%.2f");
}

GraphPreviewNode::GraphPreviewNode()
{
    setTitle("Preview");
    setStyle(ImFlow::NodeStyle::brown());
    addIN<float>("Value", 0.0f, ImFlow::ConnectionFilter::SameType(), ImFlow::PinStyle::blue());
}

void GraphPreviewNode::draw()
{
    ImGui::Text("Output: %.2f", getInVal<float>("Value"));
}

GraphAddNode::GraphAddNode()
{
    setTitle("Add");
    setStyle(ImFlow::NodeStyle::cyan());
    addIN<float>("A", 0.0f, ImFlow::ConnectionFilter::SameType(), ImFlow::PinStyle::blue());
    addIN<float>("B", 0.0f, ImFlow::ConnectionFilter::SameType(), ImFlow::PinStyle::blue());
    addOUT<float>("Result", ImFlow::PinStyle::cyan())->behaviour([this]()
    {
        return getInVal<float>("A") + getInVal<float>("B");
    });
}

void GraphAddNode::draw()
{
    ImGui::Text("A + B");
    ImGui::Text("= %.2f", getInVal<float>("A") + getInVal<float>("B"));
}

const std::vector<GraphNodeDefinition>& GetGraphNodeDefinitions()
{
    return kGraphNodeDefinitions;
}

const GraphNodeDefinition* FindGraphNodeDefinition(GraphNodeType node_type)
{
    for (const GraphNodeDefinition& definition : kGraphNodeDefinitions)
    {
        if (definition.type == node_type)
        {
            return &definition;
        }
    }

    return nullptr;
}

const char* GetGraphNodeLabel(GraphNodeType node_type)
{
    if (const GraphNodeDefinition* definition = FindGraphNodeDefinition(node_type))
    {
        return definition->label;
    }

    return "Unknown";
}

void SpawnGraphNode(ImFlow::ImNodeFlow& graph, GraphNodeType node_type, const ImVec2& drop_position)
{
    switch (node_type)
    {
    case GraphNodeType::Value:
        graph.placeNodeAt<GraphValueNode>(drop_position);
        return;
    case GraphNodeType::Add:
        graph.placeNodeAt<GraphAddNode>(drop_position);
        return;
    case GraphNodeType::Preview:
        graph.placeNodeAt<GraphPreviewNode>(drop_position);
        return;
    }
}