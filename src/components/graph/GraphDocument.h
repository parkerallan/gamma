#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace graph
{

// A single node instance inside a .graph document.
struct GraphNode
{
    std::uint64_t id = 0;
    std::string type_key;            // matches a NodeSpec::type_key
    float position_x = 0.0f;
    float position_y = 0.0f;
    // Per-pin literal values for *disconnected* input pins. Keyed by pin name.
    std::unordered_map<std::string, std::string> input_literals;
};

// Wire between two pins. `from_pin` is an output pin on `from_node`,
// `to_pin` is an input pin on `to_node`.
struct GraphLink
{
    std::uint64_t id = 0;
    std::uint64_t from_node = 0;
    std::string from_pin;
    std::uint64_t to_node = 0;
    std::string to_pin;
};

class GraphDocument
{
public:
    // Returns a new monotonically-increasing id, persisted in the doc so
    // saved/reloaded graphs keep id stability for the imgui-node-editor
    // canvas state.
    std::uint64_t AllocateId();

    // Lookup helpers.
    GraphNode* FindNode(std::uint64_t node_id);
    const GraphNode* FindNode(std::uint64_t node_id) const;
    std::vector<const GraphLink*> LinksFromOutput(std::uint64_t node_id, const std::string& pin_name) const;
    const GraphLink* LinkIntoInput(std::uint64_t node_id, const std::string& pin_name) const;
    bool HasLinkIntoInput(std::uint64_t node_id, const std::string& pin_name) const;

    void RemoveNode(std::uint64_t node_id);
    void RemoveLink(std::uint64_t link_id);

    std::vector<GraphNode> nodes;
    std::vector<GraphLink> links;
    // Opaque canvas state blob produced by ax::NodeEditor::SaveSettings.
    std::string editor_state;

    // Disk IO ------------------------------------------------------------
    // Loads a .graph JSON document. Returns true on success, populates
    // `error` on failure. An empty / `{}` file produces an empty document.
    static bool LoadFromFile(const std::filesystem::path& path, GraphDocument& out, std::string& error);

    // Serializes to JSON and writes atomically. Returns true on success.
    bool SaveToFile(const std::filesystem::path& path, std::string& error) const;

    // Serializes to a JSON string without touching the filesystem.
    std::string ToJsonString() const;

private:
    std::uint64_t next_id_ = 1;
};

} // namespace graph
