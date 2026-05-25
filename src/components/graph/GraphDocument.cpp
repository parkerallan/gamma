#include "components/graph/GraphDocument.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace graph
{

std::uint64_t GraphDocument::AllocateId()
{
    return next_id_++;
}

GraphNode* GraphDocument::FindNode(std::uint64_t node_id)
{
    for (auto& node : nodes)
    {
        if (node.id == node_id)
        {
            return &node;
        }
    }
    return nullptr;
}

const GraphNode* GraphDocument::FindNode(std::uint64_t node_id) const
{
    for (const auto& node : nodes)
    {
        if (node.id == node_id)
        {
            return &node;
        }
    }
    return nullptr;
}

std::vector<const GraphLink*> GraphDocument::LinksFromOutput(std::uint64_t node_id, const std::string& pin_name) const
{
    std::vector<const GraphLink*> result;
    for (const auto& link : links)
    {
        if (link.from_node == node_id && link.from_pin == pin_name)
        {
            result.push_back(&link);
        }
    }
    return result;
}

const GraphLink* GraphDocument::LinkIntoInput(std::uint64_t node_id, const std::string& pin_name) const
{
    for (const auto& link : links)
    {
        if (link.to_node == node_id && link.to_pin == pin_name)
        {
            return &link;
        }
    }
    return nullptr;
}

bool GraphDocument::HasLinkIntoInput(std::uint64_t node_id, const std::string& pin_name) const
{
    return LinkIntoInput(node_id, pin_name) != nullptr;
}

void GraphDocument::RemoveNode(std::uint64_t node_id)
{
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
        [&](const GraphNode& n) { return n.id == node_id; }), nodes.end());
    links.erase(std::remove_if(links.begin(), links.end(),
        [&](const GraphLink& l) { return l.from_node == node_id || l.to_node == node_id; }),
        links.end());
}

void GraphDocument::RemoveLink(std::uint64_t link_id)
{
    links.erase(std::remove_if(links.begin(), links.end(),
        [&](const GraphLink& l) { return l.id == link_id; }), links.end());
}

bool GraphDocument::LoadFromFile(const std::filesystem::path& path, GraphDocument& out, std::string& error)
{
    out = GraphDocument{};

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        error = "Failed to open graph file: " + path.string();
        return false;
    }

    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::string contents = buffer.str();
    if (contents.empty())
    {
        return true;
    }

    nlohmann::json doc;
    try
    {
        doc = nlohmann::json::parse(contents);
    }
    catch (const std::exception& ex)
    {
        error = std::string("JSON parse error: ") + ex.what();
        return false;
    }

    if (!doc.is_object())
    {
        return true;
    }

    out.next_id_ = doc.value("next_id", static_cast<std::uint64_t>(1));
    out.editor_state = doc.value("editor_state", std::string());

    if (doc.contains("nodes") && doc["nodes"].is_array())
    {
        for (const auto& node_json : doc["nodes"])
        {
            GraphNode node;
            node.id = node_json.value("id", static_cast<std::uint64_t>(0));
            node.type_key = node_json.value("type", std::string());
            if (node_json.contains("pos") && node_json["pos"].is_array() && node_json["pos"].size() == 2)
            {
                node.position_x = node_json["pos"][0].get<float>();
                node.position_y = node_json["pos"][1].get<float>();
            }
            if (node_json.contains("literals") && node_json["literals"].is_object())
            {
                for (auto it = node_json["literals"].begin(); it != node_json["literals"].end(); ++it)
                {
                    if (it.value().is_string())
                    {
                        node.input_literals[it.key()] = it.value().get<std::string>();
                    }
                }
            }
            if (node.id != 0)
            {
                out.next_id_ = std::max(out.next_id_, node.id + 1);
                out.nodes.push_back(std::move(node));
            }
        }
    }

    if (doc.contains("links") && doc["links"].is_array())
    {
        for (const auto& link_json : doc["links"])
        {
            GraphLink link;
            link.id = link_json.value("id", static_cast<std::uint64_t>(0));
            link.from_node = link_json.value("from_node", static_cast<std::uint64_t>(0));
            link.from_pin = link_json.value("from_pin", std::string());
            link.to_node = link_json.value("to_node", static_cast<std::uint64_t>(0));
            link.to_pin = link_json.value("to_pin", std::string());
            if (link.id != 0 && link.from_node != 0 && link.to_node != 0)
            {
                out.next_id_ = std::max(out.next_id_, link.id + 1);
                out.links.push_back(std::move(link));
            }
        }
    }

    return true;
}

std::string GraphDocument::ToJsonString() const
{
    nlohmann::json doc;
    doc["version"] = 1;
    doc["next_id"] = next_id_;
    doc["editor_state"] = editor_state;

    nlohmann::json nodes_json = nlohmann::json::array();
    for (const auto& node : nodes)
    {
        nlohmann::json node_json;
        node_json["id"] = node.id;
        node_json["type"] = node.type_key;
        node_json["pos"] = {node.position_x, node.position_y};
        if (!node.input_literals.empty())
        {
            nlohmann::json literals_json = nlohmann::json::object();
            for (const auto& [pin, value] : node.input_literals)
            {
                literals_json[pin] = value;
            }
            node_json["literals"] = std::move(literals_json);
        }
        nodes_json.push_back(std::move(node_json));
    }
    doc["nodes"] = std::move(nodes_json);

    nlohmann::json links_json = nlohmann::json::array();
    for (const auto& link : links)
    {
        nlohmann::json link_json;
        link_json["id"] = link.id;
        link_json["from_node"] = link.from_node;
        link_json["from_pin"] = link.from_pin;
        link_json["to_node"] = link.to_node;
        link_json["to_pin"] = link.to_pin;
        links_json.push_back(std::move(link_json));
    }
    doc["links"] = std::move(links_json);

    return doc.dump(2);
}

bool GraphDocument::SaveToFile(const std::filesystem::path& path, std::string& error) const
{
    const std::string contents = ToJsonString();

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        error = "Failed to open graph file for writing: " + path.string();
        return false;
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output)
    {
        error = "Failed to write graph file: " + path.string();
        return false;
    }
    return true;
}

} // namespace graph
