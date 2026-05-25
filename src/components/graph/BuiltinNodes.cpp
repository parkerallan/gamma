#include "components/graph/NodeSpec.h"

#include <sstream>
#include <string>

namespace graph
{

namespace
{

// Convenience builders ------------------------------------------------------

PinSpec ExecIn()  { return PinSpec{"In",   PinType::Exec, ""}; }
PinSpec ExecOut(const std::string& name = "Out") { return PinSpec{name, PinType::Exec, ""}; }
PinSpec PinIn (const std::string& name, PinType type, const std::string& def = "")
{
    return PinSpec{name, type, def};
}
PinSpec PinOut(const std::string& name, PinType type)
{
    return PinSpec{name, type, ""};
}

// Emit helpers for event-entry nodes. They produce a complete
// `function Script:<method>(...) ... end\n` block.
std::string EmitEventFunction(TranspileContext& ctx, const GraphNode& node,
    const std::string& method_signature,
    const std::initializer_list<std::pair<std::string, std::string>>& data_out_locals)
{
    std::string body = ctx.EmitFromExec(node, "Then");
    const std::string prelude = ctx.TakePrelude();

    std::string out;
    out += "function Script:" + method_signature + "\n";
    for (const auto& [pin_name, local_name] : data_out_locals)
    {
        (void)pin_name;
        out += "    local " + local_name + " = " + local_name + "_arg\n";
    }
    if (!prelude.empty())
    {
        std::istringstream iss(prelude);
        std::string line;
        while (std::getline(iss, line))
        {
            if (!line.empty()) out += "    " + line + "\n";
        }
    }
    if (!body.empty())
    {
        std::istringstream iss(body);
        std::string line;
        while (std::getline(iss, line))
        {
            if (!line.empty()) out += "    " + line + "\n";
        }
    }
    out += "end\n\n";
    return out;
}

// Simpler event emitter: parameters become locals matching their pin names.
std::string EmitEventFunctionSimple(TranspileContext& ctx, const GraphNode& node,
    const std::string& method_name,
    const std::vector<std::string>& param_names)
{
    std::string sig = method_name + "(";
    for (std::size_t i = 0; i < param_names.size(); ++i)
    {
        if (i) sig += ", ";
        sig += param_names[i];
    }
    sig += ")";

    // Cache each parameter as an output of this event node. The pin name
    // matches the param name so downstream EvalInput retrieves it directly.
    // We do this by inserting a memo entry directly.
    // NOTE: we instead emit the body and rely on the EvalInput cache via
    // emit() returning the param name for the requested output pin (handled
    // in event nodes' emit() callbacks below).

    std::string body = ctx.EmitFromExec(node, "Then");
    const std::string prelude = ctx.TakePrelude();

    std::string out;
    out += "function Script:" + sig + "\n";
    if (!prelude.empty())
    {
        std::istringstream iss(prelude);
        std::string line;
        while (std::getline(iss, line))
        {
            if (!line.empty()) out += "    " + line + "\n";
        }
    }
    if (!body.empty())
    {
        std::istringstream iss(body);
        std::string line;
        while (std::getline(iss, line))
        {
            if (!line.empty()) out += "    " + line + "\n";
        }
    }
    out += "end\n\n";
    return out;
}

void RegisterEventNodes(NodeSpecRegistry& reg)
{
    // OnCreate ----------------------------------------------------------
    // Fires once when the script instance is first loaded, before OnStart.
    {
        NodeSpec s;
        s.type_key = "event.OnCreate";
        s.display_name = "On Create";
        s.category = "Events";
        s.is_event_entry = true;
        s.is_exec_node = true;
        s.header_color = 0xff2f7a2fu;
        s.outputs = { ExecOut("Then") };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return EmitEventFunctionSimple(ctx, node, "OnCreate", {});
        };
        reg.Register(std::move(s));
    }
    // OnStart -----------------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "event.OnStart";
        s.display_name = "On Start";
        s.category = "Events";
        s.is_event_entry = true;
        s.is_exec_node = true;
        s.header_color = 0xff2f7a2fu;
        s.outputs = { ExecOut("Then") };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return EmitEventFunctionSimple(ctx, node, "OnStart", {});
        };
        reg.Register(std::move(s));
    }
    // OnUpdate ----------------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "event.OnUpdate";
        s.display_name = "On Update";
        s.category = "Events";
        s.is_event_entry = true;
        s.is_exec_node = true;
        s.header_color = 0xff2f7a2fu;
        s.outputs = { ExecOut("Then"), PinOut("dt", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            if (!ctx.RequestedOutputPin().empty())
            {
                return "dt";
            }
            return EmitEventFunctionSimple(ctx, node, "OnUpdate", {"dt"});
        };
        reg.Register(std::move(s));
    }
    // OnDestroy ---------------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "event.OnDestroy";
        s.display_name = "On Destroy";
        s.category = "Events";
        s.is_event_entry = true;
        s.is_exec_node = true;
        s.header_color = 0xff2f7a2fu;
        s.outputs = { ExecOut("Then") };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return EmitEventFunctionSimple(ctx, node, "OnDestroy", {});
        };
        reg.Register(std::move(s));
    }
    // OnTriggerEnter / Stay / Exit -------------------------------------
    const std::pair<const char*, const char*> trigger_methods[] = {
        {"event.OnTriggerEnter", "OnTriggerEnter"},
        {"event.OnTriggerStay",  "OnTriggerStay"},
        {"event.OnTriggerExit",  "OnTriggerExit"},
    };
    for (const auto& [key, method] : trigger_methods)
    {
        NodeSpec s;
        s.type_key = key;
        s.display_name = method;
        s.category = "Events";
        s.is_event_entry = true;
        s.is_exec_node = true;
        s.header_color = 0xff2f7a2fu;
        s.outputs = {
            ExecOut("Then"),
            PinOut("object", PinType::String),
            PinOut("other",  PinType::String),
            PinOut("phase",  PinType::String),
        };
        std::string method_name = method;
        s.emit = [method_name](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string& req = ctx.RequestedOutputPin();
            if (req == "object") return "objectName";
            if (req == "other")  return "otherName";
            if (req == "phase")  return "phase";
            return EmitEventFunctionSimple(ctx, node, method_name,
                {"objectName", "otherName", "phase"});
        };
        reg.Register(std::move(s));
    }
}

void RegisterFlowNodes(NodeSpecRegistry& reg)
{
    // Branch ------------------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "flow.Branch";
        s.display_name = "Branch";
        s.category = "Flow";
        s.is_exec_node = true;
        s.header_color = 0xff9c5db1u;
        s.inputs  = { ExecIn(), PinIn("Condition", PinType::Bool, "false") };
        s.outputs = { ExecOut("True"), ExecOut("False") };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string cond = ctx.EvalInput(node, "Condition");
            const std::string then_body = ctx.EmitFromExec(node, "True");
            const std::string else_body = ctx.EmitFromExec(node, "False");
            std::string out;
            out += "if " + cond + " then\n";
            if (!then_body.empty())
            {
                std::istringstream iss(then_body);
                std::string line;
                while (std::getline(iss, line)) if (!line.empty()) out += "    " + line + "\n";
            }
            if (!else_body.empty())
            {
                out += "else\n";
                std::istringstream iss(else_body);
                std::string line;
                while (std::getline(iss, line)) if (!line.empty()) out += "    " + line + "\n";
            }
            out += "end\n";
            return out;
        };
        reg.Register(std::move(s));
    }
    // While -------------------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "flow.While";
        s.display_name = "While";
        s.category = "Flow";
        s.is_exec_node = true;
        s.header_color = 0xff9c5db1u;
        s.inputs  = { ExecIn(), PinIn("Condition", PinType::Bool, "false") };
        s.outputs = { ExecOut("Loop"), ExecOut("After") };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string cond = ctx.EvalInput(node, "Condition");
            const std::string body = ctx.EmitFromExec(node, "Loop");
            const std::string after = ctx.EmitFromExec(node, "After");
            std::string out;
            out += "while " + cond + " do\n";
            if (!body.empty())
            {
                std::istringstream iss(body);
                std::string line;
                while (std::getline(iss, line)) if (!line.empty()) out += "    " + line + "\n";
            }
            out += "end\n";
            out += after;
            return out;
        };
        reg.Register(std::move(s));
    }
    // ForRange ----------------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "flow.ForRange";
        s.display_name = "For Range";
        s.category = "Flow";
        s.is_exec_node = true;
        s.header_color = 0xff9c5db1u;
        s.inputs  = { ExecIn(),
                      PinIn("Start", PinType::Number, "0"),
                      PinIn("End",   PinType::Number, "10"),
                      PinIn("Step",  PinType::Number, "1") };
        s.outputs = { ExecOut("Body"), ExecOut("After"), PinOut("i", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            if (ctx.RequestedOutputPin() == "i")
            {
                // Loop variable name is well-known; the body emission below
                // declares it. We expose `i_<id>` to keep nested loops safe.
                return std::string("i_") + std::to_string(node.id);
            }
            const std::string start = ctx.EvalInput(node, "Start");
            const std::string end_v = ctx.EvalInput(node, "End");
            const std::string step  = ctx.EvalInput(node, "Step");
            const std::string body  = ctx.EmitFromExec(node, "Body");
            const std::string after = ctx.EmitFromExec(node, "After");
            const std::string iname = std::string("i_") + std::to_string(node.id);
            std::string out;
            out += "for " + iname + " = " + start + ", " + end_v + ", " + step + " do\n";
            if (!body.empty())
            {
                std::istringstream iss(body);
                std::string line;
                while (std::getline(iss, line)) if (!line.empty()) out += "    " + line + "\n";
            }
            out += "end\n";
            out += after;
            return out;
        };
        reg.Register(std::move(s));
    }
    // Sequence (3 exec outs) -------------------------------------------
    {
        NodeSpec s;
        s.type_key = "flow.Sequence";
        s.display_name = "Sequence";
        s.category = "Flow";
        s.is_exec_node = true;
        s.header_color = 0xff9c5db1u;
        s.inputs  = { ExecIn() };
        s.outputs = { ExecOut("Then 0"), ExecOut("Then 1"), ExecOut("Then 2") };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            std::string out;
            out += ctx.EmitFromExec(node, "Then 0");
            out += ctx.EmitFromExec(node, "Then 1");
            out += ctx.EmitFromExec(node, "Then 2");
            return out;
        };
        reg.Register(std::move(s));
    }
}

void RegisterVariableNodes(NodeSpecRegistry& reg)
{
    // SetLocal ----------------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "var.SetLocal";
        s.display_name = "Set Local";
        s.category = "Variables";
        s.is_exec_node = true;
        s.header_color = 0xffb87333u;
        s.inputs  = { ExecIn(),
                      PinIn("Name",  PinType::String, "x"),
                      PinIn("Value", PinType::Any,    "0") };
        s.outputs = { ExecOut("Then") };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            std::string name_lit;
            const auto it = node.input_literals.find("Name");
            if (it != node.input_literals.end()) name_lit = it->second;
            if (name_lit.empty()) name_lit = "x";
            const std::string value = ctx.EvalInput(node, "Value");
            std::string out;
            out += "_locals." + name_lit + " = " + value + "\n";
            out += ctx.EmitFromExec(node, "Then");
            return out;
        };
        reg.Register(std::move(s));
    }
    // GetLocal ----------------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "var.GetLocal";
        s.display_name = "Get Local";
        s.category = "Variables";
        s.header_color = 0xffb87333u;
        s.inputs  = { PinIn("Name", PinType::String, "x") };
        s.outputs = { PinOut("Value", PinType::Any) };
        s.emit = [](TranspileContext& /*ctx*/, const GraphNode& node) -> std::string
        {
            std::string name_lit;
            const auto it = node.input_literals.find("Name");
            if (it != node.input_literals.end()) name_lit = it->second;
            if (name_lit.empty()) name_lit = "x";
            return "_locals." + name_lit;
        };
        reg.Register(std::move(s));
    }
    // SetGlobal ---------------------------------------------------------
    // Writes to a Lua global so the value persists across script callbacks
    // and is visible to hand-written Lua sharing the same VM.
    {
        NodeSpec s;
        s.type_key = "var.SetGlobal";
        s.display_name = "Set Global";
        s.category = "Variables";
        s.is_exec_node = true;
        s.header_color = 0xffb87333u;
        s.inputs  = { ExecIn(),
                      PinIn("Name",  PinType::String, "score"),
                      PinIn("Value", PinType::Any,    "0") };
        s.outputs = { ExecOut("Then") };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            std::string name_lit;
            const auto it = node.input_literals.find("Name");
            if (it != node.input_literals.end()) name_lit = it->second;
            if (name_lit.empty()) name_lit = "score";
            const std::string value = ctx.EvalInput(node, "Value");
            std::string out;
            out += "_G[\"" + name_lit + "\"] = " + value + "\n";
            out += ctx.EmitFromExec(node, "Then");
            return out;
        };
        reg.Register(std::move(s));
    }
    // GetGlobal ---------------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "var.GetGlobal";
        s.display_name = "Get Global";
        s.category = "Variables";
        s.header_color = 0xffb87333u;
        s.inputs  = { PinIn("Name", PinType::String, "score") };
        s.outputs = { PinOut("Value", PinType::Any) };
        s.emit = [](TranspileContext& /*ctx*/, const GraphNode& node) -> std::string
        {
            std::string name_lit;
            const auto it = node.input_literals.find("Name");
            if (it != node.input_literals.end()) name_lit = it->second;
            if (name_lit.empty()) name_lit = "score";
            return "_G[\"" + name_lit + "\"]";
        };
        reg.Register(std::move(s));
    }
}

// Helper: register a binary math operator data node.
void RegisterBinaryOp(NodeSpecRegistry& reg, const std::string& key, const std::string& display,
    const std::string& category, PinType in_type, PinType out_type, const std::string& op,
    unsigned int color)
{
    NodeSpec s;
    s.type_key = key;
    s.display_name = display;
    s.category = category;
    s.header_color = color;
    s.inputs  = { PinIn("A", in_type, "0"), PinIn("B", in_type, "0") };
    s.outputs = { PinOut("Result", out_type) };
    s.emit = [op](TranspileContext& ctx, const GraphNode& node) -> std::string
    {
        const std::string a = ctx.EvalInput(node, "A");
        const std::string b = ctx.EvalInput(node, "B");
        return "(" + a + " " + op + " " + b + ")";
    };
    reg.Register(std::move(s));
}

void RegisterMathNodes(NodeSpecRegistry& reg)
{
    const unsigned int math_color = 0xff4d8fbcu;
    RegisterBinaryOp(reg, "math.Add", "Add", "Math", PinType::Number, PinType::Number, "+", math_color);
    RegisterBinaryOp(reg, "math.Sub", "Subtract", "Math", PinType::Number, PinType::Number, "-", math_color);
    RegisterBinaryOp(reg, "math.Mul", "Multiply", "Math", PinType::Number, PinType::Number, "*", math_color);
    RegisterBinaryOp(reg, "math.Div", "Divide", "Math", PinType::Number, PinType::Number, "/", math_color);

    const unsigned int cmp_color = 0xff4d7da0u;
    RegisterBinaryOp(reg, "cmp.Eq", "Equal",          "Compare", PinType::Number, PinType::Bool, "==", cmp_color);
    RegisterBinaryOp(reg, "cmp.Neq","Not Equal",      "Compare", PinType::Number, PinType::Bool, "~=", cmp_color);
    RegisterBinaryOp(reg, "cmp.Lt", "Less Than",      "Compare", PinType::Number, PinType::Bool, "<",  cmp_color);
    RegisterBinaryOp(reg, "cmp.Gt", "Greater Than",   "Compare", PinType::Number, PinType::Bool, ">",  cmp_color);
    RegisterBinaryOp(reg, "cmp.Le", "Less Or Equal",  "Compare", PinType::Number, PinType::Bool, "<=", cmp_color);
    RegisterBinaryOp(reg, "cmp.Ge", "Greater Or Equal","Compare", PinType::Number, PinType::Bool, ">=", cmp_color);

    const unsigned int bool_color = 0xff4d7da0u;
    RegisterBinaryOp(reg, "bool.And", "And", "Logic", PinType::Bool, PinType::Bool, "and", bool_color);
    RegisterBinaryOp(reg, "bool.Or",  "Or",  "Logic", PinType::Bool, PinType::Bool, "or",  bool_color);

    {
        NodeSpec s;
        s.type_key = "bool.Not";
        s.display_name = "Not";
        s.category = "Logic";
        s.header_color = bool_color;
        s.inputs  = { PinIn("A", PinType::Bool, "false") };
        s.outputs = { PinOut("Result", PinType::Bool) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return "(not " + ctx.EvalInput(node, "A") + ")";
        };
        reg.Register(std::move(s));
    }

    // Unary math helpers -----------------------------------------------
    auto register_unary = [&](const std::string& key, const std::string& display,
        const std::string& lua_fn)
    {
        NodeSpec s;
        s.type_key = key;
        s.display_name = display;
        s.category = "Math";
        s.header_color = math_color;
        s.inputs  = { PinIn("A", PinType::Number, "0") };
        s.outputs = { PinOut("Result", PinType::Number) };
        s.emit = [lua_fn](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return "(" + lua_fn + "(" + ctx.EvalInput(node, "A") + "))";
        };
        reg.Register(std::move(s));
    };
    register_unary("math.Abs",   "Abs",   "math.abs");
    register_unary("math.Sqrt",  "Sqrt",  "math.sqrt");
    register_unary("math.Floor", "Floor", "math.floor");
    register_unary("math.Ceil",  "Ceil",  "math.ceil");
    register_unary("math.Sin",   "Sin",   "math.sin");
    register_unary("math.Cos",   "Cos",   "math.cos");
    register_unary("math.Tan",   "Tan",   "math.tan");

    {
        NodeSpec s;
        s.type_key = "math.Negate";
        s.display_name = "Negate";
        s.category = "Math";
        s.header_color = math_color;
        s.inputs  = { PinIn("A", PinType::Number, "0") };
        s.outputs = { PinOut("Result", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return "(-(" + ctx.EvalInput(node, "A") + "))";
        };
        reg.Register(std::move(s));
    }
    {
        NodeSpec s;
        s.type_key = "math.Round";
        s.display_name = "Round";
        s.category = "Math";
        s.header_color = math_color;
        s.inputs  = { PinIn("A", PinType::Number, "0") };
        s.outputs = { PinOut("Result", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return "(math.floor((" + ctx.EvalInput(node, "A") + ") + 0.5))";
        };
        reg.Register(std::move(s));
    }

    // Binary math helpers ----------------------------------------------
    auto register_binary_fn = [&](const std::string& key, const std::string& display,
        const std::string& lua_fn)
    {
        NodeSpec s;
        s.type_key = key;
        s.display_name = display;
        s.category = "Math";
        s.header_color = math_color;
        s.inputs  = { PinIn("A", PinType::Number, "0"), PinIn("B", PinType::Number, "0") };
        s.outputs = { PinOut("Result", PinType::Number) };
        s.emit = [lua_fn](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return "(" + lua_fn + "(" + ctx.EvalInput(node, "A") + ", " + ctx.EvalInput(node, "B") + "))";
        };
        reg.Register(std::move(s));
    };
    register_binary_fn("math.Min", "Min", "math.min");
    register_binary_fn("math.Max", "Max", "math.max");
    RegisterBinaryOp(reg, "math.Pow", "Power", "Math", PinType::Number, PinType::Number, "^", math_color);

    // Clamp ------------------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "math.Clamp";
        s.display_name = "Clamp";
        s.category = "Math";
        s.header_color = math_color;
        s.inputs  = { PinIn("Value", PinType::Number, "0"),
                      PinIn("Min",   PinType::Number, "0"),
                      PinIn("Max",   PinType::Number, "1") };
        s.outputs = { PinOut("Result", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string v   = ctx.EvalInput(node, "Value");
            const std::string mn  = ctx.EvalInput(node, "Min");
            const std::string mx  = ctx.EvalInput(node, "Max");
            return "(math.max(" + mn + ", math.min(" + mx + ", " + v + ")))";
        };
        reg.Register(std::move(s));
    }
    // Lerp -------------------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "math.Lerp";
        s.display_name = "Lerp";
        s.category = "Math";
        s.header_color = math_color;
        s.inputs  = { PinIn("A", PinType::Number, "0"),
                      PinIn("B", PinType::Number, "1"),
                      PinIn("T", PinType::Number, "0") };
        s.outputs = { PinOut("Result", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string a = ctx.EvalInput(node, "A");
            const std::string b = ctx.EvalInput(node, "B");
            const std::string t = ctx.EvalInput(node, "T");
            return "((" + a + ") + ((" + b + ") - (" + a + ")) * (" + t + "))";
        };
        reg.Register(std::move(s));
    }
    // Random (0..1) ----------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "math.Random";
        s.display_name = "Random";
        s.category = "Math";
        s.header_color = math_color;
        s.outputs = { PinOut("Result", PinType::Number) };
        s.emit = [](TranspileContext& /*ctx*/, const GraphNode& /*node*/) -> std::string
        {
            return "(math.random())";
        };
        reg.Register(std::move(s));
    }
    // RandomRange (Min..Max float) -------------------------------------
    {
        NodeSpec s;
        s.type_key = "math.RandomRange";
        s.display_name = "Random Range";
        s.category = "Math";
        s.header_color = math_color;
        s.inputs  = { PinIn("Min", PinType::Number, "0"),
                      PinIn("Max", PinType::Number, "1") };
        s.outputs = { PinOut("Result", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string mn = ctx.EvalInput(node, "Min");
            const std::string mx = ctx.EvalInput(node, "Max");
            return "((" + mn + ") + ((" + mx + ") - (" + mn + ")) * math.random())";
        };
        reg.Register(std::move(s));
    }

    // String helpers ---------------------------------------------------
    const unsigned int string_color = 0xff7da37du;
    {
        NodeSpec s;
        s.type_key = "str.Concat";
        s.display_name = "Concat";
        s.category = "String";
        s.header_color = string_color;
        s.inputs  = { PinIn("A", PinType::String, ""), PinIn("B", PinType::String, "") };
        s.outputs = { PinOut("Result", PinType::String) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return "(tostring(" + ctx.EvalInput(node, "A") + ") .. tostring(" + ctx.EvalInput(node, "B") + "))";
        };
        reg.Register(std::move(s));
    }
    {
        NodeSpec s;
        s.type_key = "str.ToString";
        s.display_name = "To String";
        s.category = "String";
        s.header_color = string_color;
        s.inputs  = { PinIn("A", PinType::Any, "0") };
        s.outputs = { PinOut("Result", PinType::String) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return "(tostring(" + ctx.EvalInput(node, "A") + "))";
        };
        reg.Register(std::move(s));
    }
    {
        NodeSpec s;
        s.type_key = "str.ToNumber";
        s.display_name = "To Number";
        s.category = "String";
        s.header_color = string_color;
        s.inputs  = { PinIn("A", PinType::String, "0") };
        s.outputs = { PinOut("Result", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return "(tonumber(" + ctx.EvalInput(node, "A") + ") or 0)";
        };
        reg.Register(std::move(s));
    }
}

void RegisterLiteralNodes(NodeSpecRegistry& reg)
{
    const unsigned int color = 0xff666666u;
    {
        NodeSpec s;
        s.type_key = "const.Number";
        s.display_name = "Number";
        s.category = "Literals";
        s.header_color = color;
        s.inputs  = { PinIn("Value", PinType::Number, "0") };
        s.outputs = { PinOut("Result", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return ctx.EvalInput(node, "Value");
        };
        reg.Register(std::move(s));
    }
    {
        NodeSpec s;
        s.type_key = "const.String";
        s.display_name = "String";
        s.category = "Literals";
        s.header_color = color;
        s.inputs  = { PinIn("Value", PinType::String, "") };
        s.outputs = { PinOut("Result", PinType::String) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return ctx.EvalInput(node, "Value");
        };
        reg.Register(std::move(s));
    }
    {
        NodeSpec s;
        s.type_key = "const.Bool";
        s.display_name = "Bool";
        s.category = "Literals";
        s.header_color = color;
        s.inputs  = { PinIn("Value", PinType::Bool, "false") };
        s.outputs = { PinOut("Result", PinType::Bool) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return ctx.EvalInput(node, "Value");
        };
        reg.Register(std::move(s));
    }
    {
        NodeSpec s;
        s.type_key = "const.Vec3";
        s.display_name = "Vec3";
        s.category = "Literals";
        s.header_color = color;
        s.inputs  = { PinIn("X", PinType::Number, "0"),
                      PinIn("Y", PinType::Number, "0"),
                      PinIn("Z", PinType::Number, "0") };
        // Pseudo: exposes X/Y/Z as data outputs; downstream pins ask for them
        // individually rather than as a packed vec3 (engine APIs take xyz
        // triples directly).
        s.outputs = { PinOut("X", PinType::Number),
                      PinOut("Y", PinType::Number),
                      PinOut("Z", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string& req = ctx.RequestedOutputPin();
            if (req == "X") return ctx.EvalInput(node, "X");
            if (req == "Y") return ctx.EvalInput(node, "Y");
            if (req == "Z") return ctx.EvalInput(node, "Z");
            return "0";
        };
        reg.Register(std::move(s));
    }
}

// Helper to emit an exec-statement node that calls a Lua function with the
// listed pin args, then chains to the "Then" exec output.
NodeSpec MakeCallStatement(const std::string& key, const std::string& display,
    const std::string& category, unsigned int color,
    const std::string& lua_call_template, // e.g. "Engine.Log({0})"
    const std::vector<PinSpec>& data_inputs)
{
    NodeSpec s;
    s.type_key = key;
    s.display_name = display;
    s.category = category;
    s.header_color = color;
    s.is_exec_node = true;
    s.inputs.push_back(ExecIn());
    for (const auto& pin : data_inputs) s.inputs.push_back(pin);
    s.outputs = { ExecOut("Then") };

    std::vector<std::string> pin_names;
    pin_names.reserve(data_inputs.size());
    for (const auto& pin : data_inputs) pin_names.push_back(pin.name);

    s.emit = [lua_call_template, pin_names](TranspileContext& ctx, const GraphNode& node) -> std::string
    {
        std::string call = lua_call_template;
        for (std::size_t i = 0; i < pin_names.size(); ++i)
        {
            const std::string placeholder = "{" + std::to_string(i) + "}";
            const std::string expr = ctx.EvalInput(node, pin_names[i]);
            std::string::size_type pos = 0;
            while ((pos = call.find(placeholder, pos)) != std::string::npos)
            {
                call.replace(pos, placeholder.size(), expr);
                pos += expr.size();
            }
        }
        std::string out = call + "\n";
        out += ctx.EmitFromExec(node, "Then");
        return out;
    };
    return s;
}

// Helper for pure data nodes that wrap a Lua expression like
// `Input.IsKeyDown({0})`. Returns the expression as-is.
NodeSpec MakeCallExpression(const std::string& key, const std::string& display,
    const std::string& category, unsigned int color,
    const std::string& lua_call_template,
    const std::vector<PinSpec>& data_inputs,
    PinType result_type,
    const std::string& result_pin_name = "Result")
{
    NodeSpec s;
    s.type_key = key;
    s.display_name = display;
    s.category = category;
    s.header_color = color;
    s.inputs = data_inputs;
    s.outputs = { PinOut(result_pin_name, result_type) };

    std::vector<std::string> pin_names;
    pin_names.reserve(data_inputs.size());
    for (const auto& pin : data_inputs) pin_names.push_back(pin.name);

    s.emit = [lua_call_template, pin_names](TranspileContext& ctx, const GraphNode& node) -> std::string
    {
        std::string call = lua_call_template;
        for (std::size_t i = 0; i < pin_names.size(); ++i)
        {
            const std::string placeholder = "{" + std::to_string(i) + "}";
            const std::string expr = ctx.EvalInput(node, pin_names[i]);
            std::string::size_type pos = 0;
            while ((pos = call.find(placeholder, pos)) != std::string::npos)
            {
                call.replace(pos, placeholder.size(), expr);
                pos += expr.size();
            }
        }
        return "(" + call + ")";
    };
    return s;
}

void RegisterApiNodes(NodeSpecRegistry& reg)
{
    const unsigned int engine_color  = 0xff3a6fb0u;
    const unsigned int input_color   = 0xff5d8aa8u;
    const unsigned int world_color   = 0xff4a90a4u;
    const unsigned int physics_color = 0xff8b5a2bu;
    const unsigned int audio_color   = 0xff7a4ea0u;
    const unsigned int video_color   = 0xff7a4ea0u;
    const unsigned int attr_color    = 0xff3a6fb0u;

    // Engine ------------------------------------------------------------
    reg.Register(MakeCallStatement("engine.Log", "Log", "Engine", engine_color,
        "Engine.Log({0})",
        { PinIn("Message", PinType::String, "hello") }));

    reg.Register(MakeCallStatement("engine.SetObjectPosition", "Set Object Position", "Engine", engine_color,
        "Engine.SetObjectPosition({0}, {1}, {2}, {3})",
        { PinIn("Name", PinType::Object, "Player"),
          PinIn("X", PinType::Number, "0"),
          PinIn("Y", PinType::Number, "0"),
          PinIn("Z", PinType::Number, "0") }));

    reg.Register(MakeCallStatement("engine.SetObjectRotation", "Set Object Rotation", "Engine", engine_color,
        "Engine.SetObjectRotation({0}, {1}, {2}, {3})",
        { PinIn("Name", PinType::Object, "Player"),
          PinIn("X", PinType::Number, "0"),
          PinIn("Y", PinType::Number, "0"),
          PinIn("Z", PinType::Number, "0") }));

    reg.Register(MakeCallStatement("engine.SetObjectEnabled", "Set Object Enabled", "Engine", engine_color,
        "Engine.SetObjectEnabled({0}, {1})",
        { PinIn("Name",    PinType::Object, "Player"),
          PinIn("Enabled", PinType::Bool,   "true") }));

    reg.Register(MakeCallStatement("engine.SetCameraActive", "Set Camera Active", "Engine", engine_color,
        "Engine.SetCameraActive({0}, {1})",
        { PinIn("Name",    PinType::Object, "MainCamera"),
          PinIn("Enabled", PinType::Bool,   "true") }));

    reg.Register(MakeCallStatement("engine.SetObjectScale", "Set Object Scale", "Engine", engine_color,
        "Engine.SetObjectScale({0}, {1}, {2}, {3})",
        { PinIn("Name", PinType::Object, "Player"),
          PinIn("X",    PinType::Number, "1"),
          PinIn("Y",    PinType::Number, "1"),
          PinIn("Z",    PinType::Number, "1") }));

    reg.Register(MakeCallExpression("engine.GetObjectEnabled", "Get Object Enabled", "Engine", engine_color,
        "Engine.GetObjectEnabled({0})",
        { PinIn("Name", PinType::Object, "Player") },
        PinType::Bool));

    // GetObjectScale (multi-output data) --------------------------------
    {
        NodeSpec s;
        s.type_key = "engine.GetObjectScale";
        s.display_name = "Get Object Scale";
        s.category = "Engine";
        s.header_color = engine_color;
        s.inputs  = { PinIn("Name", PinType::Object, "Player") };
        s.outputs = { PinOut("X", PinType::Number),
                      PinOut("Y", PinType::Number),
                      PinOut("Z", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string& req = ctx.RequestedOutputPin();
            const std::string base = std::string("scale_") + std::to_string(node.id);
            const std::string name_expr = ctx.EvalInput(node, "Name");
            ctx.EmitPrelude("local " + base + "_x, " + base + "_y, " + base + "_z = Engine.GetObjectScale(" + name_expr + ")");
            if (req == "X") return base + "_x";
            if (req == "Y") return base + "_y";
            if (req == "Z") return base + "_z";
            return "1";
        };
        reg.Register(std::move(s));
    }

    // GetObjectPosition (multi-output data) -----------------------------
    {
        NodeSpec s;
        s.type_key = "engine.GetObjectPosition";
        s.display_name = "Get Object Position";
        s.category = "Engine";
        s.header_color = engine_color;
        s.inputs  = { PinIn("Name", PinType::Object, "Player") };
        s.outputs = { PinOut("X", PinType::Number),
                      PinOut("Y", PinType::Number),
                      PinOut("Z", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string& req = ctx.RequestedOutputPin();
            const std::string base = std::string("pos_") + std::to_string(node.id);
            // Emit prelude once. We detect "once" via the cache key existing
            // for any output of this node; here we just always re-issue the
            // local declaration to the prelude — repeated declarations of
            // the same local are valid in Lua.
            const std::string name_expr = ctx.EvalInput(node, "Name");
            ctx.EmitPrelude("local " + base + "_x, " + base + "_y, " + base + "_z = Engine.GetObjectPosition(" + name_expr + ")");
            if (req == "X") return base + "_x";
            if (req == "Y") return base + "_y";
            if (req == "Z") return base + "_z";
            return "0";
        };
        reg.Register(std::move(s));
    }

    // GetObjectRotation -------------------------------------------------
    {
        NodeSpec s;
        s.type_key = "engine.GetObjectRotation";
        s.display_name = "Get Object Rotation";
        s.category = "Engine";
        s.header_color = engine_color;
        s.inputs  = { PinIn("Name", PinType::Object, "Player") };
        s.outputs = { PinOut("X", PinType::Number),
                      PinOut("Y", PinType::Number),
                      PinOut("Z", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string& req = ctx.RequestedOutputPin();
            const std::string base = std::string("rot_") + std::to_string(node.id);
            const std::string name_expr = ctx.EvalInput(node, "Name");
            ctx.EmitPrelude("local " + base + "_x, " + base + "_y, " + base + "_z = Engine.GetObjectRotation(" + name_expr + ")");
            if (req == "X") return base + "_x";
            if (req == "Y") return base + "_y";
            if (req == "Z") return base + "_z";
            return "0";
        };
        reg.Register(std::move(s));
    }

    // Input -------------------------------------------------------------
    reg.Register(MakeCallExpression("input.IsKeyDown", "Is Key Down", "Input", input_color,
        "Input.IsKeyDown({0})",
        { PinIn("Key", PinType::String, "W") },
        PinType::Bool));

    reg.Register(MakeCallExpression("input.WasKeyPressed", "Was Key Pressed", "Input", input_color,
        "Input.WasKeyPressed({0})",
        { PinIn("Key", PinType::String, "Space") },
        PinType::Bool));

    {
        NodeSpec s;
        s.type_key = "input.MousePosition";
        s.display_name = "Mouse Position";
        s.category = "Input";
        s.header_color = input_color;
        s.outputs = { PinOut("X", PinType::Number), PinOut("Y", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string& req = ctx.RequestedOutputPin();
            const std::string base = std::string("mouse_") + std::to_string(node.id);
            ctx.EmitPrelude("local " + base + "_x, " + base + "_y = Input.MousePosition()");
            if (req == "X") return base + "_x";
            if (req == "Y") return base + "_y";
            return "0";
        };
        reg.Register(std::move(s));
    }
    {
        NodeSpec s;
        s.type_key = "input.MouseDelta";
        s.display_name = "Mouse Delta";
        s.category = "Input";
        s.header_color = input_color;
        s.outputs = { PinOut("X", PinType::Number), PinOut("Y", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string& req = ctx.RequestedOutputPin();
            const std::string base = std::string("mouseDelta_") + std::to_string(node.id);
            ctx.EmitPrelude("local " + base + "_x, " + base + "_y = Input.MouseDelta()");
            if (req == "X") return base + "_x";
            if (req == "Y") return base + "_y";
            return "0";
        };
        reg.Register(std::move(s));
    }

    // World -------------------------------------------------------------
    reg.Register(MakeCallStatement("world.Spawn", "Spawn", "World", world_color,
        "World.Spawn({0}, {1}, {2}, {3}, {4}, {5})",
        { PinIn("Name",       PinType::String, "Enemy_01"),
          PinIn("Model",      PinType::String, "Models/Enemy.glb"),
          PinIn("X",          PinType::Number, "0"),
          PinIn("Y",          PinType::Number, "0"),
          PinIn("Z",          PinType::Number, "0"),
          PinIn("Script",     PinType::String, "") }));

    reg.Register(MakeCallStatement("world.SpawnFromObject", "Spawn From Object", "World", world_color,
        "World.SpawnFromObject({0}, {1}, {2}, {3}, {4})",
        { PinIn("Source", PinType::Object, "Template"),
          PinIn("Name",   PinType::String, "Copy_01"),
          PinIn("X",      PinType::Number, "0"),
          PinIn("Y",      PinType::Number, "0"),
          PinIn("Z",      PinType::Number, "0") }));

    reg.Register(MakeCallStatement("world.Destroy", "Destroy", "World", world_color,
        "World.Destroy({0})",
        { PinIn("Name", PinType::Object, "Target") }));

    reg.Register(MakeCallStatement("world.DestroyByPrefix", "Destroy By Prefix", "World", world_color,
        "World.DestroyByPrefix({0})",
        { PinIn("Prefix", PinType::String, "Bullet_") }));

    reg.Register(MakeCallExpression("world.Exists", "Exists", "World", world_color,
        "World.Exists({0})",
        { PinIn("Name", PinType::Object, "Target") },
        PinType::Bool));

    reg.Register(MakeCallStatement("world.Emit", "Emit Event", "World", world_color,
        "World.Emit({0}, {1})",
        { PinIn("Event",   PinType::String, "Damage"),
          PinIn("Payload", PinType::Any,    "") }));

    reg.Register(MakeCallStatement("world.LoadScene", "Load Scene", "World", world_color,
        "World.LoadScene({0})",
        { PinIn("Scene", PinType::String, "Level2") }));

    // Timers ------------------------------------------------------------
    // Helper used by SetTimeout/SetInterval emit() to wrap the downstream
    // exec branch in a Lua closure while keeping the closure's data-node
    // preludes scoped INSIDE the closure body (not hoisted to the outer
    // event function).
    auto emit_timer_node = [](TranspileContext& ctx, const GraphNode& node,
                              const char* api_name,
                              const char* callback_exec_pin,
                              bool expose_timer_id) -> std::string
    {
        const std::string timer_local = std::string("timer_") + std::to_string(node.id);

        // EvalInput path: another node wants the TimerID output.
        if (expose_timer_id && ctx.RequestedOutputPin() == "TimerID")
        {
            return timer_local;
        }

        // Exec path. Evaluate the Seconds input first; any preludes it
        // produces belong to the OUTER event scope.
        const std::string seconds = ctx.EvalInput(node, "Seconds");

        // Snapshot+clear the outer prelude so the recursion into the
        // callback branch starts with an empty prelude bucket.
        const std::string outer_prelude = ctx.TakePrelude();
        const std::string callback_body = ctx.EmitFromExec(node, callback_exec_pin);
        const std::string callback_prelude = ctx.TakePrelude();

        // Restore outer prelude for the enclosing emitter.
        if (!outer_prelude.empty())
        {
            ctx.EmitPrelude(outer_prelude);
        }

        std::string out;
        if (expose_timer_id)
        {
            // Forward-declare the local so closures (e.g. a SetInterval
            // body that clears its own timer) can capture it as an
            // upvalue, and so downstream EvalInput("TimerID") works even
            // when the read appears earlier in the function.
            ctx.EmitPrelude(std::string("local ") + timer_local);
            out += timer_local + " = " + api_name + "(" + seconds
                 + ", function(self)\n";
        }
        else
        {
            out += std::string(api_name) + "(" + seconds + ", function(self)\n";
        }

        // Indent callback prelude + body inside the closure.
        auto append_indented = [&out](const std::string& text)
        {
            if (text.empty()) return;
            std::istringstream iss(text);
            std::string line;
            while (std::getline(iss, line))
            {
                if (!line.empty()) out += "    " + line + "\n";
            }
        };
        append_indented(callback_prelude);
        append_indented(callback_body);

        out += "end)\n";
        return out;
    };

    // Delay (one-shot, no TimerID exposed) ------------------------------
    {
        NodeSpec s;
        s.type_key = "world.Delay";
        s.display_name = "Delay";
        s.category = "World";
        s.is_exec_node = true;
        s.header_color = world_color;
        s.inputs  = { ExecIn(), PinIn("Seconds", PinType::Number, "1.0") };
        s.outputs = { ExecOut("Then") };
        s.emit = [emit_timer_node](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return emit_timer_node(ctx, node, "World.SetTimeout", "Then", false);
        };
        reg.Register(std::move(s));
    }

    // Set Timeout (one-shot, exposes TimerID) ---------------------------
    {
        NodeSpec s;
        s.type_key = "world.SetTimeout";
        s.display_name = "Set Timeout";
        s.category = "World";
        s.is_exec_node = true;
        s.header_color = world_color;
        s.inputs  = { ExecIn(), PinIn("Seconds", PinType::Number, "1.0") };
        s.outputs = { ExecOut("OnFire"), PinOut("TimerID", PinType::Number) };
        s.emit = [emit_timer_node](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return emit_timer_node(ctx, node, "World.SetTimeout", "OnFire", true);
        };
        reg.Register(std::move(s));
    }

    // Set Interval (repeating, exposes TimerID) -------------------------
    {
        NodeSpec s;
        s.type_key = "world.SetInterval";
        s.display_name = "Set Interval";
        s.category = "World";
        s.is_exec_node = true;
        s.header_color = world_color;
        s.inputs  = { ExecIn(), PinIn("Seconds", PinType::Number, "1.0") };
        s.outputs = { ExecOut("OnTick"), PinOut("TimerID", PinType::Number) };
        s.emit = [emit_timer_node](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return emit_timer_node(ctx, node, "World.SetInterval", "OnTick", true);
        };
        reg.Register(std::move(s));
    }

    // Clear Timer -------------------------------------------------------
    reg.Register(MakeCallStatement("world.ClearTimer", "Clear Timer", "World", world_color,
        "World.ClearTimer({0})",
        { PinIn("TimerID", PinType::Number, "0") }));

    // Physics -----------------------------------------------------------
    reg.Register(MakeCallStatement("physics.SetVelocity", "Set Velocity", "Physics", physics_color,
        "Physics.SetVelocity({0}, {1}, {2}, {3})",
        { PinIn("Name", PinType::Object, "Player"),
          PinIn("X", PinType::Number, "0"),
          PinIn("Y", PinType::Number, "0"),
          PinIn("Z", PinType::Number, "0") }));

    reg.Register(MakeCallStatement("physics.AddImpulse", "Add Impulse", "Physics", physics_color,
        "Physics.AddImpulse({0}, {1}, {2}, {3})",
        { PinIn("Name", PinType::Object, "Player"),
          PinIn("X", PinType::Number, "0"),
          PinIn("Y", PinType::Number, "0"),
          PinIn("Z", PinType::Number, "0") }));

    reg.Register(MakeCallStatement("physics.AddForce", "Add Force", "Physics", physics_color,
        "Physics.AddForce({0}, {1}, {2}, {3})",
        { PinIn("Name", PinType::Object, "Player"),
          PinIn("X", PinType::Number, "0"),
          PinIn("Y", PinType::Number, "0"),
          PinIn("Z", PinType::Number, "0") }));

    {
        NodeSpec s;
        s.type_key = "physics.GetVelocity";
        s.display_name = "Get Velocity";
        s.category = "Physics";
        s.header_color = physics_color;
        s.inputs  = { PinIn("Name", PinType::Object, "Player") };
        s.outputs = { PinOut("X", PinType::Number),
                      PinOut("Y", PinType::Number),
                      PinOut("Z", PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string& req = ctx.RequestedOutputPin();
            const std::string base = std::string("vel_") + std::to_string(node.id);
            const std::string name_expr = ctx.EvalInput(node, "Name");
            ctx.EmitPrelude("local " + base + "_x, " + base + "_y, " + base + "_z = Physics.GetVelocity(" + name_expr + ")");
            if (req == "X") return base + "_x";
            if (req == "Y") return base + "_y";
            if (req == "Z") return base + "_z";
            return "0";
        };
        reg.Register(std::move(s));
    }

    // Physics.Raycast --------------------------------------------------
    // Casts a ray and returns a multi-field hit (object name, distance,
    // hit point, surface normal). When the ray misses, the underlying Lua
    // call returns nil, so each output pin guards with `(hit and hit.x) or 0`
    // so disconnected misses don't trip a nil-index error downstream.
    {
        NodeSpec s;
        s.type_key = "physics.Raycast";
        s.display_name = "Raycast";
        s.category = "Physics";
        s.header_color = physics_color;
        s.inputs  = { PinIn("OX", PinType::Number, "0"),
                      PinIn("OY", PinType::Number, "0"),
                      PinIn("OZ", PinType::Number, "0"),
                      PinIn("DX", PinType::Number, "0"),
                      PinIn("DY", PinType::Number, "-1"),
                      PinIn("DZ", PinType::Number, "0"),
                      PinIn("MaxDistance", PinType::Number, "1000") };
        s.outputs = { PinOut("Hit",      PinType::Bool),
                      PinOut("Object",   PinType::String),
                      PinOut("Distance", PinType::Number),
                      PinOut("X",        PinType::Number),
                      PinOut("Y",        PinType::Number),
                      PinOut("Z",        PinType::Number),
                      PinOut("NX",       PinType::Number),
                      PinOut("NY",       PinType::Number),
                      PinOut("NZ",       PinType::Number) };
        s.emit = [](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            const std::string& req = ctx.RequestedOutputPin();
            const std::string base = std::string("hit_") + std::to_string(node.id);
            const std::string ox = ctx.EvalInput(node, "OX");
            const std::string oy = ctx.EvalInput(node, "OY");
            const std::string oz = ctx.EvalInput(node, "OZ");
            const std::string dx = ctx.EvalInput(node, "DX");
            const std::string dy = ctx.EvalInput(node, "DY");
            const std::string dz = ctx.EvalInput(node, "DZ");
            const std::string md = ctx.EvalInput(node, "MaxDistance");
            ctx.EmitPrelude("local " + base + " = Physics.Raycast("
                + ox + ", " + oy + ", " + oz + ", "
                + dx + ", " + dy + ", " + dz + ", " + md + ")");
            if (req == "Hit")      return "(" + base + " ~= nil)";
            if (req == "Object")   return "(" + base + " and " + base + ".object) or \"\"";
            if (req == "Distance") return "(" + base + " and " + base + ".distance) or 0";
            if (req == "X")        return "(" + base + " and " + base + ".x) or 0";
            if (req == "Y")        return "(" + base + " and " + base + ".y) or 0";
            if (req == "Z")        return "(" + base + " and " + base + ".z) or 0";
            if (req == "NX")       return "(" + base + " and " + base + ".nx) or 0";
            if (req == "NY")       return "(" + base + " and " + base + ".ny) or 0";
            if (req == "NZ")       return "(" + base + " and " + base + ".nz) or 0";
            return "nil";
        };
        reg.Register(std::move(s));
    }

    // Time --------------------------------------------------------------
    const unsigned int time_color = 0xff8e7cc3u;
    {
        NodeSpec s;
        s.type_key = "time.DeltaTime";
        s.display_name = "Delta Time";
        s.category = "Time";
        s.header_color = time_color;
        s.outputs = { PinOut("Seconds", PinType::Number) };
        s.emit = [](TranspileContext& /*ctx*/, const GraphNode& /*node*/) -> std::string
        {
            return "Time.DeltaTime";
        };
        reg.Register(std::move(s));
    }
    {
        NodeSpec s;
        s.type_key = "time.TotalTime";
        s.display_name = "Total Time";
        s.category = "Time";
        s.header_color = time_color;
        s.outputs = { PinOut("Seconds", PinType::Number) };
        s.emit = [](TranspileContext& /*ctx*/, const GraphNode& /*node*/) -> std::string
        {
            return "Time.TotalTime";
        };
        reg.Register(std::move(s));
    }

    // Time-namespaced timer nodes (call Time.Delay / Time.Timer /
    // Time.ClearTimer at the Lua level; these share the same registry as
    // World.SetTimeout/SetInterval/ClearTimer).
    {
        NodeSpec s;
        s.type_key = "time.Delay";
        s.display_name = "Delay";
        s.category = "Time";
        s.is_exec_node = true;
        s.header_color = time_color;
        s.inputs  = { ExecIn(), PinIn("Seconds", PinType::Number, "1.0") };
        s.outputs = { ExecOut("Then"), PinOut("TimerID", PinType::Number) };
        s.emit = [emit_timer_node](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return emit_timer_node(ctx, node, "Time.Delay", "Then", true);
        };
        reg.Register(std::move(s));
    }
    {
        NodeSpec s;
        s.type_key = "time.Timer";
        s.display_name = "Timer";
        s.category = "Time";
        s.is_exec_node = true;
        s.header_color = time_color;
        s.inputs  = { ExecIn(), PinIn("Seconds", PinType::Number, "1.0") };
        s.outputs = { ExecOut("OnTick"), PinOut("TimerID", PinType::Number) };
        s.emit = [emit_timer_node](TranspileContext& ctx, const GraphNode& node) -> std::string
        {
            return emit_timer_node(ctx, node, "Time.Timer", "OnTick", true);
        };
        reg.Register(std::move(s));
    }
    reg.Register(MakeCallStatement("time.ClearTimer", "Clear Timer", "Time", time_color,
        "Time.ClearTimer({0})",
        { PinIn("TimerID", PinType::Number, "0") }));

    // Audio (complete) --------------------------------------------------
    reg.Register(MakeCallStatement("audio.Play", "Audio Play", "Audio", audio_color,
        "Audio.Play({0})",
        { PinIn("Name", PinType::Object, "Speaker") }));
    reg.Register(MakeCallStatement("audio.Stop", "Audio Stop", "Audio", audio_color,
        "Audio.Stop({0})",
        { PinIn("Name", PinType::Object, "Speaker") }));
    reg.Register(MakeCallExpression("audio.IsPlaying", "Audio Is Playing", "Audio", audio_color,
        "Audio.IsPlaying({0})",
        { PinIn("Name", PinType::Object, "Speaker") },
        PinType::Bool));
    reg.Register(MakeCallStatement("audio.SetVolume", "Audio Set Volume", "Audio", audio_color,
        "Audio.SetVolume({0}, {1})",
        { PinIn("Name",   PinType::Object, "Speaker"),
          PinIn("Volume", PinType::Number, "1") }));
    reg.Register(MakeCallStatement("audio.SetPitch", "Audio Set Pitch", "Audio", audio_color,
        "Audio.SetPitch({0}, {1})",
        { PinIn("Name",  PinType::Object, "Speaker"),
          PinIn("Pitch", PinType::Number, "1") }));
    reg.Register(MakeCallStatement("audio.SetLoop", "Audio Set Loop", "Audio", audio_color,
        "Audio.SetLoop({0}, {1})",
        { PinIn("Name", PinType::Object, "Speaker"),
          PinIn("Loop", PinType::Bool,   "true") }));

    // Video (complete) --------------------------------------------------
    reg.Register(MakeCallStatement("video.Play", "Video Play", "Video", video_color,
        "Video.Play({0})",
        { PinIn("Name", PinType::Object, "Cinematic") }));
    reg.Register(MakeCallStatement("video.Stop", "Video Stop", "Video", video_color,
        "Video.Stop({0})",
        { PinIn("Name", PinType::Object, "Cinematic") }));
    reg.Register(MakeCallExpression("video.IsPlaying", "Video Is Playing", "Video", video_color,
        "Video.IsPlaying({0})",
        { PinIn("Name", PinType::Object, "Cinematic") },
        PinType::Bool));
    reg.Register(MakeCallStatement("video.SetVolume", "Video Set Volume", "Video", video_color,
        "Video.SetVolume({0}, {1})",
        { PinIn("Name",   PinType::Object, "Cinematic"),
          PinIn("Volume", PinType::Number, "1") }));
    reg.Register(MakeCallStatement("video.SetMuted", "Video Set Muted", "Video", video_color,
        "Video.SetMuted({0}, {1})",
        { PinIn("Name",  PinType::Object, "Cinematic"),
          PinIn("Muted", PinType::Bool,   "true") }));

    // Attribute Set/Get nodes -----------------------------------------
    // ONE combined Set + ONE Get per attribute type. Every writable field of
    // the type is surfaced as a pin on the same node. Setter calls are only
    // emitted for fields whose pins have an actual incoming link, so any
    // disconnected field keeps its current runtime value untouched.
    enum class AccessorKind { Float, Int, Bool, String, Vec2, Vec3 };
    struct AttrField { const char* name; AccessorKind kind; };
    struct AttrType  { const char* table; std::vector<AttrField> fields; };

    auto value_pin_type = [](AccessorKind k) -> PinType {
        switch (k) {
        case AccessorKind::Bool:   return PinType::Bool;
        case AccessorKind::String: return PinType::String;
        default:                    return PinType::Number;
        }
    };
    auto value_default = [](AccessorKind k) -> std::string {
        switch (k) {
        case AccessorKind::Bool:   return "false";
        case AccessorKind::String: return "";
        default:                    return "0";
        }
    };

    const std::vector<AttrType> attr_types = {
        {"EnvironmentLightAttr", {
            {"Color", AccessorKind::Vec3}, {"Intensity", AccessorKind::Float} }},
        {"DirectionalLightAttr", {
            {"Color", AccessorKind::Vec3}, {"Intensity", AccessorKind::Float} }},
        {"PointLightAttr", {
            {"Color", AccessorKind::Vec3}, {"Intensity", AccessorKind::Float},
            {"Range", AccessorKind::Float}, {"Radius", AccessorKind::Float},
            {"HaloIntensity", AccessorKind::Float}, {"HaloRadius", AccessorKind::Float} }},
        {"SpotLightAttr", {
            {"Color", AccessorKind::Vec3}, {"Intensity", AccessorKind::Float},
            {"Range", AccessorKind::Float}, {"InnerCone", AccessorKind::Float},
            {"OuterCone", AccessorKind::Float} }},
        {"CameraAttr", {
            {"FieldOfView", AccessorKind::Float}, {"NearClip", AccessorKind::Float},
            {"FarClip", AccessorKind::Float}, {"Active", AccessorKind::Bool} }},
        {"RigidbodyAttr", {
            {"Shape", AccessorKind::String}, {"Dynamic", AccessorKind::Bool},
            {"LockRotationX", AccessorKind::Bool}, {"LockRotationY", AccessorKind::Bool},
            {"LockRotationZ", AccessorKind::Bool}, {"Mass", AccessorKind::Float},
            {"Friction", AccessorKind::Float}, {"Radius", AccessorKind::Float},
            {"CapsuleHalfHeight", AccessorKind::Float}, {"HalfExtent", AccessorKind::Vec3},
            {"LinearDamping", AccessorKind::Float}, {"AngularDamping", AccessorKind::Float} }},
        {"TriggerVolumeAttr", {
            {"HalfExtent", AccessorKind::Vec3} }},
        {"Text2DAttr", {
            {"FontPath", AccessorKind::String}, {"Text", AccessorKind::String},
            {"Position", AccessorKind::Vec2}, {"Size", AccessorKind::Vec2},
            {"LockAspectRatio", AccessorKind::Bool}, {"FontSize", AccessorKind::Float},
            {"Color", AccessorKind::Vec3}, {"Alpha", AccessorKind::Float},
            {"Priority", AccessorKind::Int} }},
        {"Image2DAttr", {
            {"ImagePath", AccessorKind::String}, {"Position", AccessorKind::Vec2},
            {"Size", AccessorKind::Vec2}, {"LockAspectRatio", AccessorKind::Bool},
            {"StretchToScreen", AccessorKind::Bool}, {"PlayMode", AccessorKind::String},
            {"Tint", AccessorKind::Vec3}, {"Alpha", AccessorKind::Float},
            {"Priority", AccessorKind::Int} }},
        {"Color2DAttr", {
            {"Color", AccessorKind::Vec3}, {"Alpha", AccessorKind::Float},
            {"Priority", AccessorKind::Int} }},
        {"Video2DAttr", {
            {"VideoPath", AccessorKind::String}, {"Position", AccessorKind::Vec2},
            {"Size", AccessorKind::Vec2}, {"LockAspectRatio", AccessorKind::Bool},
            {"StretchToScreen", AccessorKind::Bool}, {"Tint", AccessorKind::Vec3},
            {"Alpha", AccessorKind::Float}, {"Priority", AccessorKind::Int},
            {"PlayMode", AccessorKind::String}, {"Volume", AccessorKind::Float},
            {"Muted", AccessorKind::Bool} }},
        {"SkyboxAttr", {
            {"ImagePath", AccessorKind::String}, {"Rotation", AccessorKind::Float} }},
        {"AudioAttr", {
            {"ClipPath", AccessorKind::String}, {"PlayMode", AccessorKind::String},
            {"Volume", AccessorKind::Float}, {"Loop", AccessorKind::Bool},
            {"Spatialize3D", AccessorKind::Bool}, {"Pitch", AccessorKind::Float},
            {"MinDistance", AccessorKind::Float}, {"MaxDistance", AccessorKind::Float},
            {"DopplerFactor", AccessorKind::Float} }},
        {"Animator", {
            {"ControllerPath", AccessorKind::String}, {"InitialState", AccessorKind::String},
            {"PlaybackSpeed", AccessorKind::Float}, {"AutoPlay", AccessorKind::Bool} }},
    };

    auto pretty_name = [](const std::string& table) -> std::string {
        // Strip a trailing "Attr" suffix so category names read naturally
        // (e.g. "PointLightAttr" -> "PointLight"). Names that don't end in
        // "Attr" (like "Animator") are returned unchanged.
        if (table.size() > 4 && table.compare(table.size() - 4, 4, "Attr") == 0)
            return table.substr(0, table.size() - 4);
        return table;
    };

    for (const AttrType& t : attr_types)
    {
        const std::string category = std::string("Attribute/") + pretty_name(t.table);
        const std::string table   = t.table;
        const std::vector<AttrField> fields = t.fields;

        // Set (exec, all fields as pins) -------------------------------
        {
            NodeSpec s;
            s.type_key = "attr." + table + ".Set";
            s.display_name = "Set";
            s.category = category;
            s.header_color = attr_color;
            s.is_exec_node = true;
            s.inputs.push_back(ExecIn());
            s.inputs.push_back(PinIn("Name", PinType::Object, "Target"));
            for (const AttrField& f : fields)
            {
                if (f.kind == AccessorKind::Vec3) {
                    s.inputs.push_back(PinIn(std::string(f.name) + "X", PinType::Number, "0"));
                    s.inputs.push_back(PinIn(std::string(f.name) + "Y", PinType::Number, "0"));
                    s.inputs.push_back(PinIn(std::string(f.name) + "Z", PinType::Number, "0"));
                } else if (f.kind == AccessorKind::Vec2) {
                    s.inputs.push_back(PinIn(std::string(f.name) + "X", PinType::Number, "0"));
                    s.inputs.push_back(PinIn(std::string(f.name) + "Y", PinType::Number, "0"));
                } else {
                    s.inputs.push_back(PinIn(f.name, value_pin_type(f.kind), value_default(f.kind)));
                }
            }
            s.outputs = { ExecOut("Then") };

            s.emit = [table, fields](TranspileContext& ctx, const GraphNode& node) -> std::string
            {
                const std::string name_expr = ctx.EvalInput(node, "Name");
                std::string out;
                for (const AttrField& f : fields)
                {
                    std::vector<std::string> pins;
                    if (f.kind == AccessorKind::Vec3)
                        pins = { std::string(f.name) + "X", std::string(f.name) + "Y", std::string(f.name) + "Z" };
                    else if (f.kind == AccessorKind::Vec2)
                        pins = { std::string(f.name) + "X", std::string(f.name) + "Y" };
                    else
                        pins = { f.name };

                    bool any_linked = false;
                    for (const auto& p : pins) {
                        if (ctx.Document().HasLinkIntoInput(node.id, p)) { any_linked = true; break; }
                    }
                    if (!any_linked) continue;

                    std::string call = "Engine." + table + "." + f.name + "(" + name_expr;
                    for (const auto& p : pins) call += ", " + ctx.EvalInput(node, p);
                    call += ")\n";
                    out += call;
                }
                out += ctx.EmitFromExec(node, "Then");
                return out;
            };
            reg.Register(std::move(s));
        }

        // Get (data, all fields as outputs) ----------------------------
        {
            NodeSpec s;
            s.type_key = "attr." + table + ".Get";
            s.display_name = "Get";
            s.category = category;
            s.header_color = attr_color;
            s.inputs = { PinIn("Name", PinType::Object, "Target") };
            for (const AttrField& f : fields)
            {
                if (f.kind == AccessorKind::Vec3) {
                    s.outputs.push_back(PinOut(std::string(f.name) + "X", PinType::Number));
                    s.outputs.push_back(PinOut(std::string(f.name) + "Y", PinType::Number));
                    s.outputs.push_back(PinOut(std::string(f.name) + "Z", PinType::Number));
                } else if (f.kind == AccessorKind::Vec2) {
                    s.outputs.push_back(PinOut(std::string(f.name) + "X", PinType::Number));
                    s.outputs.push_back(PinOut(std::string(f.name) + "Y", PinType::Number));
                } else {
                    s.outputs.push_back(PinOut(f.name, value_pin_type(f.kind)));
                }
            }

            s.emit = [table, fields](TranspileContext& ctx, const GraphNode& node) -> std::string
            {
                const std::string& req = ctx.RequestedOutputPin();
                const std::string name_expr = ctx.EvalInput(node, "Name");
                const std::string base = std::string("attr_") + std::to_string(node.id);
                for (const AttrField& f : fields)
                {
                    const std::string fname = f.name;
                    if (f.kind == AccessorKind::Vec3) {
                        if (req == fname + "X" || req == fname + "Y" || req == fname + "Z") {
                            ctx.EmitPrelude("local " + base + "_" + fname + "_x, " + base + "_" + fname + "_y, " + base + "_" + fname + "_z = Engine." + table + "." + fname + "(" + name_expr + ")");
                            if (req == fname + "X") return base + "_" + fname + "_x";
                            if (req == fname + "Y") return base + "_" + fname + "_y";
                            return base + "_" + fname + "_z";
                        }
                    } else if (f.kind == AccessorKind::Vec2) {
                        if (req == fname + "X" || req == fname + "Y") {
                            ctx.EmitPrelude("local " + base + "_" + fname + "_x, " + base + "_" + fname + "_y = Engine." + table + "." + fname + "(" + name_expr + ")");
                            if (req == fname + "X") return base + "_" + fname + "_x";
                            return base + "_" + fname + "_y";
                        }
                    } else {
                        if (req == fname) {
                            ctx.EmitPrelude("local " + base + "_" + fname + "_v = Engine." + table + "." + fname + "(" + name_expr + ")");
                            return base + "_" + fname + "_v";
                        }
                    }
                }
                return "nil";
            };
            reg.Register(std::move(s));
        }
    }

    // Animator specials (non-standard signatures) -----------------------
    // Live alongside the combined Animator Set/Get under Attribute/Animator.
    const std::string anim_cat = "Attribute/Animator";
    reg.Register(MakeCallStatement("attr.Animator.SetBool", "Set Bool", anim_cat, attr_color,
        "Engine.Animator.SetBool({0}, {1}, {2})",
        { PinIn("Name",      PinType::Object, "Target"),
          PinIn("Parameter", PinType::String, "moving"),
          PinIn("Value",     PinType::Bool,   "true") }));
    reg.Register(MakeCallExpression("attr.Animator.GetBool", "Get Bool", anim_cat, attr_color,
        "Engine.Animator.GetBool({0}, {1})",
        { PinIn("Name",      PinType::Object, "Target"),
          PinIn("Parameter", PinType::String, "moving") },
        PinType::Bool));
    reg.Register(MakeCallStatement("attr.Animator.SetTrigger", "Set Trigger", anim_cat, attr_color,
        "Engine.Animator.SetTrigger({0}, {1})",
        { PinIn("Name",    PinType::Object, "Target"),
          PinIn("Trigger", PinType::String, "jump") }));
    reg.Register(MakeCallStatement("attr.Animator.SetState", "Set State", anim_cat, attr_color,
        "Engine.Animator.SetState({0}, {1})",
        { PinIn("Name",  PinType::Object, "Target"),
          PinIn("State", PinType::String, "Idle") }));
    reg.Register(MakeCallExpression("attr.Animator.GetState", "Get State", anim_cat, attr_color,
        "Engine.Animator.GetState({0})",
        { PinIn("Name", PinType::Object, "Target") },
        PinType::String));
    reg.Register(MakeCallExpression("attr.Animator.StateTime", "State Time", anim_cat, attr_color,
        "Engine.Animator.StateTime({0})",
        { PinIn("Name", PinType::Object, "Target") },
        PinType::Number));
    reg.Register(MakeCallStatement("attr.Animator.SetDefaultState", "Set Default State", anim_cat, attr_color,
        "Engine.Animator.SetDefaultState({0}, {1})",
        { PinIn("Name",  PinType::Object, "Target"),
          PinIn("State", PinType::String, "Idle") }));
    reg.Register(MakeCallExpression("attr.Animator.GetDefaultState", "Get Default State", anim_cat, attr_color,
        "Engine.Animator.GetDefaultState({0})",
        { PinIn("Name", PinType::Object, "Target") },
        PinType::String));
}

} // namespace

void RegisterBuiltinNodes(NodeSpecRegistry& registry)
{
    RegisterEventNodes(registry);
    RegisterFlowNodes(registry);
    RegisterVariableNodes(registry);
    RegisterLiteralNodes(registry);
    RegisterMathNodes(registry);
    RegisterApiNodes(registry);
}

} // namespace graph
