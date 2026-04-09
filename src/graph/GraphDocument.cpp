#include "graph/GraphDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

using json = nlohmann::json;

namespace
{
std::string TrimCopy(std::string value);

GraphDocument LoadJsonGraphDocument(const std::string& contents)
{
    GraphDocument document;

    json root = json::parse(contents, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        document.error_message = "Invalid graph JSON.";
        return document;
    }

    document.graph_name = root.value("name", "");

    const json nodes = root.value("nodes", json::array());
    if (!nodes.is_array())
    {
        document.error_message = "Graph JSON has invalid nodes array.";
        return document;
    }

    for (const json& saved_node : nodes)
    {
        if (!saved_node.is_object())
        {
            document.error_message = "Graph JSON contains an invalid node entry.";
            return document;
        }

        GraphNodeRecord node;
        node.id = saved_node.value("uuid", static_cast<std::uint64_t>(0));
        node.type_key = saved_node.value("type", std::string());

        const json position = saved_node.value("pos", json::array());
        if (position.is_array() && position.size() == 2)
        {
            node.position_x = position[0].get<float>();
            node.position_y = position[1].get<float>();
        }

        for (auto property_it = saved_node.begin(); property_it != saved_node.end(); ++property_it)
        {
            if (property_it.key() == "uuid" || property_it.key() == "type" || property_it.key() == "pos")
            {
                continue;
            }

            GraphNodeProperty property;
            property.key = property_it.key();
            if (property_it->is_string())
            {
                property.value = property_it->get<std::string>();
            }
            else
            {
                property.value = property_it->dump();
            }
            node.properties.push_back(std::move(property));
        }

        document.nodes.push_back(std::move(node));
    }

    const json links = root.value("links", json::array());
    if (!links.is_array())
    {
        document.error_message = "Graph JSON has invalid links array.";
        return document;
    }

    for (const json& saved_link : links)
    {
        if (!saved_link.is_object())
        {
            document.error_message = "Graph JSON contains an invalid link entry.";
            return document;
        }

        GraphLinkRecord link;
        link.source_node_id = saved_link.value("src", static_cast<std::uint64_t>(0));
        link.source_port_index = saved_link.value("srcPort", static_cast<std::uint32_t>(0));
        link.target_node_id = saved_link.value("dst", static_cast<std::uint64_t>(0));
        link.target_port_index = saved_link.value("dstPort", static_cast<std::uint32_t>(0));
        document.links.push_back(link);
    }

    document.parsed = true;
    return document;
}

std::string TrimCopy(std::string value)
{
    const auto is_space = [](unsigned char character)
    {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char character)
    {
        return !is_space(character);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char character)
    {
        return !is_space(character);
    }).base(), value.end());
    return value;
}

}

GraphDocument LoadGraphDocument(const std::filesystem::path& graph_path)
{
    GraphDocument document;

    std::ifstream input(graph_path, std::ios::binary);
    if (!input)
    {
        document.error_message = "Failed to open graph file.";
        return document;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string contents = buffer.str();
    const std::string trimmed = TrimCopy(contents);
    if (trimmed.empty())
    {
        document.error_message = "Graph file is empty.";
        return document;
    }

    document = LoadJsonGraphDocument(contents);

    if (document.parsed && document.graph_name.empty())
    {
        document.graph_name = graph_path.stem().string();
    }
    return document;
}

std::string SerializeGraphDocument(const GraphDocument& document)
{
    json root;
    root["name"] = document.graph_name.empty() ? "Untitled" : document.graph_name;
    root["nodes"] = json::array();

    std::vector<GraphNodeRecord> nodes = document.nodes;
    std::sort(nodes.begin(), nodes.end(), [](const GraphNodeRecord& left, const GraphNodeRecord& right)
    {
        return left.id < right.id;
    });

    for (const GraphNodeRecord& node : nodes)
    {
        json saved_node;
        saved_node["uuid"] = node.id;
        saved_node["type"] = node.type_key;
        saved_node["pos"] = {node.position_x, node.position_y};

        std::vector<GraphNodeProperty> properties = node.properties;
        std::sort(properties.begin(), properties.end(), [](const GraphNodeProperty& left, const GraphNodeProperty& right)
        {
            if (left.key != right.key)
            {
                return left.key < right.key;
            }
            return left.value < right.value;
        });

        for (const GraphNodeProperty& property : properties)
        {
            json property_value = json::parse(property.value, nullptr, false);
            if (property_value.is_discarded())
            {
                saved_node[property.key] = property.value;
            }
            else
            {
                saved_node[property.key] = std::move(property_value);
            }
        }

        root["nodes"].push_back(std::move(saved_node));
    }

    root["links"] = json::array();
    std::vector<GraphLinkRecord> links = document.links;
    std::sort(links.begin(), links.end(), [](const GraphLinkRecord& left, const GraphLinkRecord& right)
    {
        if (left.source_node_id != right.source_node_id)
        {
            return left.source_node_id < right.source_node_id;
        }
        if (left.source_port_index != right.source_port_index)
        {
            return left.source_port_index < right.source_port_index;
        }
        if (left.target_node_id != right.target_node_id)
        {
            return left.target_node_id < right.target_node_id;
        }
        return left.target_port_index < right.target_port_index;
    });

    for (const GraphLinkRecord& link : links)
    {
        root["links"].push_back({
            {"src", link.source_node_id},
            {"srcPort", link.source_port_index},
            {"dst", link.target_node_id},
            {"dstPort", link.target_port_index},
        });
    }

    return root.dump(2) + "\n";
}

bool SaveGraphDocument(const std::filesystem::path& graph_path, const GraphDocument& document)
{
    const std::string serialized = SerializeGraphDocument(document);

    std::ofstream output(graph_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }

    output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
    return static_cast<bool>(output);
}