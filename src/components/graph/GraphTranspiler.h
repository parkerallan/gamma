#pragma once

#include <filesystem>
#include <string>

namespace graph
{

class GraphDocument;

// Transpiles a parsed graph document into a Lua script source string.
// Returns true on success and populates `out_lua`. On failure populates
// `error` with a human-readable message.
bool TranspileGraphDocument(const GraphDocument& doc, std::string& out_lua, std::string& error);

// Convenience that loads `graph_path` and transpiles it. Returns false on
// either parse or transpile failure.
bool TranspileGraphFile(const std::filesystem::path& graph_path, std::string& out_lua, std::string& error);

} // namespace graph
