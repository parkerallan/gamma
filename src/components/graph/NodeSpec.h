#pragma once

#include "components/graph/GraphDocument.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace graph
{

enum class PinKind
{
    Input,
    Output,
};

enum class PinType
{
    Exec,
    Bool,
    Number,
    String,
    Vec3,
    Object, // alias for String; carries a scene-object name
    Any,    // wildcard, used by variable get/set
};

struct PinSpec
{
    std::string name;
    PinType type = PinType::Exec;
    // For data-input pins only: the default literal shown when no link is connected.
    // Format depends on PinType:
    //   Bool   -> "true" / "false"
    //   Number -> "0", "1.5"
    //   String -> raw string (will be Lua-escaped at emit time)
    //   Vec3   -> "x,y,z"
    //   Object -> raw string (object name)
    std::string default_literal;
};

struct NodeSpec; // fwd
class TranspileContext; // fwd

// emit() callback signature:
//   - For *exec* nodes: returns a Lua statement block (may end with newline).
//     The emitted block is responsible for recursing into its own exec-output
//     pins via ctx.EmitFromExec().
//   - For *pure data* nodes: returns a Lua expression string with no
//     side-effects. If side effects are required, see EmitWithCache below.
using NodeEmitFn = std::function<std::string(TranspileContext& ctx, const GraphNode& node)>;

struct NodeSpec
{
    std::string type_key;         // unique, used in .graph json
    std::string display_name;     // shown in title bar and palette
    std::string category;         // "Events" / "Flow" / "Engine" / ...
    std::vector<PinSpec> inputs;  // exec inputs first, then data inputs
    std::vector<PinSpec> outputs; // exec outputs first, then data outputs
    NodeEmitFn emit;
    // True for nodes with no exec-input pin that act as event handlers
    // (OnStart / OnUpdate / OnTrigger* ...). Their emit() produces a full
    // `function Script:Method() ... end` block.
    bool is_event_entry = false;
    // True for nodes that have one or more exec pins (input or output).
    bool is_exec_node = false;
    // ImGui ABGR tint for the node header.
    unsigned int header_color = 0xff4b6d99u;
};

class NodeSpecRegistry
{
public:
    static NodeSpecRegistry& Get();

    void Register(NodeSpec spec);
    const NodeSpec* Find(const std::string& type_key) const;
    const std::vector<NodeSpec>& All() const { return specs_; }

    // Triggers the one-shot registration of v1 builtin nodes. Safe to call
    // multiple times.
    void EnsureBuiltinsRegistered();

private:
    NodeSpecRegistry() = default;
    std::vector<NodeSpec> specs_;
    std::unordered_map<std::string, std::size_t> by_key_;
    bool builtins_registered_ = false;
};

// Helper used by builtin node emit() callbacks. Constructed by the transpiler
// for a single graph compilation pass.
class TranspileContext
{
public:
    TranspileContext(const GraphDocument& doc);

    const GraphDocument& Document() const { return doc_; }

    // Recursively emits the statement block reached by following the exec-out
    // pin named `exec_out_name` on `node`. Returns an empty string if the pin
    // is unconnected.
    std::string EmitFromExec(const GraphNode& node, const std::string& exec_out_name);

    // Returns a Lua expression that yields the value of the data-input pin
    // named `input_name` on `node`. If a link is connected, recurses into the
    // upstream data node (memoized: side-effectful nodes emit a `local` and
    // reuse it on subsequent calls). Otherwise returns the literal default
    // formatted as Lua.
    std::string EvalInput(const GraphNode& node, const std::string& input_name);

    // Generates a unique Lua local identifier with the given prefix.
    std::string NextLocal(const std::string& prefix);

    // Append a `local` declaration to the *prelude* of the current event
    // function. Used by EvalInput memoization for impure data nodes.
    void EmitPrelude(const std::string& line);

    std::string TakePrelude();

    void SetError(std::string message) { if (error_.empty()) error_ = std::move(message); }
    const std::string& Error() const { return error_; }

    // Escapes a string for safe inclusion as a Lua string literal (returns
    // the value already wrapped in quotes).
    static std::string EscapeLuaString(const std::string& value);

    // Formats a literal value for the given pin type. Returns a Lua expression.
    static std::string FormatDefault(PinType type, const std::string& literal);

    // Which output pin the current emit() call is being asked to produce.
    // Set by EvalInput before invoking an upstream data node's emit(); empty
    // when emit() is invoked through EmitFromExec for an exec-flow node.
    // Multi-output data nodes (e.g. Physics.Raycast) inspect this to decide
    // which expression to return.
    const std::string& RequestedOutputPin() const { return requested_output_pin_; }

private:
    const GraphDocument& doc_;
    std::string error_;
    std::string prelude_;
    int local_counter_ = 0;
    std::string requested_output_pin_;
    // Memoized output expression per (node_id, output_pin_name). Lets impure
    // data nodes (Physics.Raycast, Input.MousePosition, ...) be evaluated
    // exactly once even when referenced from multiple downstream pins.
    std::unordered_map<std::string, std::string> output_cache_;
};

} // namespace graph
