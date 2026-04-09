#pragma once

#include "ImNodeFlow.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

enum class GraphNodeType
{
    Value,
    Add,
    Preview,
};

struct GraphNodeProperty
{
    std::string key;
    std::string value;
};

class GraphNodeBase : public ImFlow::BaseNode
{
public:
    virtual GraphNodeType GetNodeType() const = 0;
    virtual void SerializeProperties(std::vector<GraphNodeProperty>& properties) const;
    virtual void DeserializeProperties(const std::vector<GraphNodeProperty>& properties);
};

class GraphValueNode : public GraphNodeBase
{
public:
    GraphValueNode();

    void draw() override;
    GraphNodeType GetNodeType() const override;
    void SerializeProperties(std::vector<GraphNodeProperty>& properties) const override;
    void DeserializeProperties(const std::vector<GraphNodeProperty>& properties) override;

private:
    float value_ = 1.0f;
};

class GraphPreviewNode : public GraphNodeBase
{
public:
    GraphPreviewNode();

    void draw() override;
    GraphNodeType GetNodeType() const override;
};

class GraphAddNode : public GraphNodeBase
{
public:
    GraphAddNode();

    void draw() override;
    GraphNodeType GetNodeType() const override;
};

struct GraphNodeDefinition
{
    GraphNodeType type;
    const char* type_key;
    const char* label;
    const char* section_name;
    std::function<std::shared_ptr<GraphNodeBase>(ImFlow::ImNodeFlow&, const ImVec2&)> create_at_grid_position;
    std::function<std::shared_ptr<GraphNodeBase>(ImFlow::ImNodeFlow&, const ImVec2&)> create_at_screen_position;
};

const std::vector<GraphNodeDefinition>& GetGraphNodeDefinitions();
const GraphNodeDefinition* FindGraphNodeDefinition(GraphNodeType node_type);
const GraphNodeDefinition* FindGraphNodeDefinition(std::string_view type_key);
const char* GetGraphNodeLabel(GraphNodeType node_type);
const char* GetGraphNodeTypeKey(GraphNodeType node_type);
bool TryParseGraphNodeType(std::string_view value, GraphNodeType& node_type);
std::shared_ptr<GraphNodeBase> SpawnGraphNode(ImFlow::ImNodeFlow& graph, GraphNodeType node_type, const ImVec2& drop_position);
std::shared_ptr<GraphNodeBase> SpawnGraphNodeAtGridPosition(ImFlow::ImNodeFlow& graph, GraphNodeType node_type, const ImVec2& position);
std::shared_ptr<GraphNodeBase> SpawnGraphNodeAtGridPosition(ImFlow::ImNodeFlow& graph, std::string_view type_key, const ImVec2& position);
