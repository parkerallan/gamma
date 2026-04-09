#pragma once

#include "graph/NodeLibrary.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct GraphNodeRecord
{
    std::uint64_t id = 0;
    std::string type_key;
    float position_x = 0.0f;
    float position_y = 0.0f;
    std::vector<GraphNodeProperty> properties;
};

struct GraphLinkRecord
{
    std::uint64_t source_node_id = 0;
    std::uint32_t source_port_index = 0;
    std::uint64_t target_node_id = 0;
    std::uint32_t target_port_index = 0;
};

struct GraphDocument
{
    bool parsed = false;
    std::string error_message;
    std::string graph_name;
    std::vector<GraphNodeRecord> nodes;
    std::vector<GraphLinkRecord> links;
};

GraphDocument LoadGraphDocument(const std::filesystem::path& graph_path);
std::string SerializeGraphDocument(const GraphDocument& document);
bool SaveGraphDocument(const std::filesystem::path& graph_path, const GraphDocument& document);