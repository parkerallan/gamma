#include "graph/NodeLibrary.h"

#include "imgui.h"

#include <cstdlib>
#include <vector>

namespace
{
const std::vector<GraphNodeDefinition> kGraphNodeDefinitions = {
    {
        GraphNodeType::Value,
        "Value",
        "Value",
        "Generators",
        [](ImFlow::ImNodeFlow& graph, const ImVec2& position) { return std::static_pointer_cast<GraphNodeBase>(graph.addNode<GraphValueNode>(position)); },
        [](ImFlow::ImNodeFlow& graph, const ImVec2& position) { return std::static_pointer_cast<GraphNodeBase>(graph.placeNodeAt<GraphValueNode>(position)); },
    },
    {
        GraphNodeType::Add,
        "Add",
        "Add",
        "Math",
        [](ImFlow::ImNodeFlow& graph, const ImVec2& position) { return std::static_pointer_cast<GraphNodeBase>(graph.addNode<GraphAddNode>(position)); },
        [](ImFlow::ImNodeFlow& graph, const ImVec2& position) { return std::static_pointer_cast<GraphNodeBase>(graph.placeNodeAt<GraphAddNode>(position)); },
    },
    {
        GraphNodeType::Preview,
        "Preview",
        "Preview",
        "Outputs",
        [](ImFlow::ImNodeFlow& graph, const ImVec2& position) { return std::static_pointer_cast<GraphNodeBase>(graph.addNode<GraphPreviewNode>(position)); },
        [](ImFlow::ImNodeFlow& graph, const ImVec2& position) { return std::static_pointer_cast<GraphNodeBase>(graph.placeNodeAt<GraphPreviewNode>(position)); },
    },
};

const GraphNodeProperty* FindProperty(const std::vector<GraphNodeProperty>& properties, std::string_view key)
{
    for (const GraphNodeProperty& property : properties)
    {
        if (property.key == key)
        {
            return &property;
        }
    }

    return nullptr;
}
}

void GraphNodeBase::SerializeProperties(std::vector<GraphNodeProperty>&) const
{
}

void GraphNodeBase::DeserializeProperties(const std::vector<GraphNodeProperty>&)
{
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

GraphNodeType GraphValueNode::GetNodeType() const
{
    return GraphNodeType::Value;
}

void GraphValueNode::SerializeProperties(std::vector<GraphNodeProperty>& properties) const
{
    properties.push_back({"value", std::to_string(value_)});
}

void GraphValueNode::DeserializeProperties(const std::vector<GraphNodeProperty>& properties)
{
    if (const GraphNodeProperty* property = FindProperty(properties, "value"))
    {
        value_ = std::strtof(property->value.c_str(), nullptr);
    }
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

GraphNodeType GraphPreviewNode::GetNodeType() const
{
    return GraphNodeType::Preview;
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

GraphNodeType GraphAddNode::GetNodeType() const
{
    return GraphNodeType::Add;
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

const GraphNodeDefinition* FindGraphNodeDefinition(std::string_view type_key)
{
    for (const GraphNodeDefinition& definition : kGraphNodeDefinitions)
    {
        if (definition.type_key == type_key)
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

const char* GetGraphNodeTypeKey(GraphNodeType node_type)
{
    if (const GraphNodeDefinition* definition = FindGraphNodeDefinition(node_type))
    {
        return definition->type_key;
    }

    return "Unknown";
}

bool TryParseGraphNodeType(std::string_view value, GraphNodeType& node_type)
{
    if (const GraphNodeDefinition* definition = FindGraphNodeDefinition(value))
    {
        node_type = definition->type;
        return true;
    }

    return false;
}

std::shared_ptr<GraphNodeBase> SpawnGraphNode(ImFlow::ImNodeFlow& graph, GraphNodeType node_type, const ImVec2& drop_position)
{
    if (const GraphNodeDefinition* definition = FindGraphNodeDefinition(node_type))
    {
        return definition->create_at_screen_position(graph, drop_position);
    }

    return nullptr;
}

std::shared_ptr<GraphNodeBase> SpawnGraphNodeAtGridPosition(ImFlow::ImNodeFlow& graph, GraphNodeType node_type, const ImVec2& position)
{
    if (const GraphNodeDefinition* definition = FindGraphNodeDefinition(node_type))
    {
        return definition->create_at_grid_position(graph, position);
    }

    return nullptr;
}

std::shared_ptr<GraphNodeBase> SpawnGraphNodeAtGridPosition(ImFlow::ImNodeFlow& graph, std::string_view type_key, const ImVec2& position)
{
    if (const GraphNodeDefinition* definition = FindGraphNodeDefinition(type_key))
    {
        return definition->create_at_grid_position(graph, position);
    }

    return nullptr;
}