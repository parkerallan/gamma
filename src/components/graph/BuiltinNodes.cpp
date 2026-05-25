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

    reg.Register(MakeCallExpression("world.Exists", "Exists", "World", world_color,
        "World.Exists({0})",
        { PinIn("Name", PinType::Object, "Target") },
        PinType::Bool));

    reg.Register(MakeCallStatement("world.LoadScene", "Load Scene", "World", world_color,
        "World.LoadScene({0})",
        { PinIn("Scene", PinType::String, "Level2") }));

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

    // Audio / Video small samples --------------------------------------
    reg.Register(MakeCallStatement("audio.Play", "Audio Play", "Audio", audio_color,
        "Audio.Play({0})",
        { PinIn("Name", PinType::Object, "Speaker") }));

    reg.Register(MakeCallStatement("video.Play", "Video Play", "Video", video_color,
        "Video.Play({0})",
        { PinIn("Name", PinType::Object, "Cinematic") }));

    // Attribute sample: PointLightAttr.Color (read + write) ------------
    reg.Register(MakeCallStatement("attr.PointLight.SetColor", "Set Point Light Color", "Attr", attr_color,
        "Engine.PointLightAttr.Color({0}, {1}, {2}, {3})",
        { PinIn("Name", PinType::Object, "Lamp"),
          PinIn("R", PinType::Number, "1"),
          PinIn("G", PinType::Number, "1"),
          PinIn("B", PinType::Number, "1") }));
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
