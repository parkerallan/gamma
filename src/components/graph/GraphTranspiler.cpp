#include "components/graph/GraphTranspiler.h"

#include "components/graph/GraphDocument.h"
#include "components/graph/NodeSpec.h"

#include <sstream>

namespace graph
{

bool TranspileGraphDocument(const GraphDocument& doc, std::string& out_lua, std::string& error)
{
    out_lua.clear();
    error.clear();

    auto& registry = NodeSpecRegistry::Get();

    std::ostringstream header;
    header << "-- Auto-generated from .graph document. Do not edit by hand.\n";
    header << "local Script = {}\n";
    header << "local _locals = {}\n";
    header << "Script._locals = _locals\n\n";

    out_lua = header.str();

    bool any_event = false;
    for (const auto& node : doc.nodes)
    {
        const NodeSpec* spec = registry.Find(node.type_key);
        if (spec == nullptr)
        {
            error = "Unknown node type: " + node.type_key;
            return false;
        }
        if (!spec->is_event_entry || !spec->emit)
        {
            continue;
        }

        any_event = true;
        TranspileContext ctx(doc);
        const std::string event_block = spec->emit(ctx, node);
        if (!ctx.Error().empty())
        {
            error = ctx.Error();
            return false;
        }
        out_lua += event_block;
    }

    if (!any_event)
    {
        out_lua += "function Script:OnStart() end\n";
    }

    out_lua += "\nreturn Script\n";
    return true;
}

bool TranspileGraphFile(const std::filesystem::path& graph_path, std::string& out_lua, std::string& error)
{
    GraphDocument doc;
    if (!GraphDocument::LoadFromFile(graph_path, doc, error))
    {
        return false;
    }
    return TranspileGraphDocument(doc, out_lua, error);
}

} // namespace graph
