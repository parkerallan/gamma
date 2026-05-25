#include "components/graph/NodeSpec.h"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace graph
{

// Defined in BuiltinNodes.cpp; called once per registry instance.
void RegisterBuiltinNodes(NodeSpecRegistry& registry);

NodeSpecRegistry& NodeSpecRegistry::Get()
{
    static NodeSpecRegistry instance;
    instance.EnsureBuiltinsRegistered();
    return instance;
}

void NodeSpecRegistry::Register(NodeSpec spec)
{
    const std::string key = spec.type_key;
    const auto it = by_key_.find(key);
    if (it != by_key_.end())
    {
        specs_[it->second] = std::move(spec);
        return;
    }
    by_key_[key] = specs_.size();
    specs_.push_back(std::move(spec));
}

const NodeSpec* NodeSpecRegistry::Find(const std::string& type_key) const
{
    const auto it = by_key_.find(type_key);
    if (it == by_key_.end())
    {
        return nullptr;
    }
    return &specs_[it->second];
}

void NodeSpecRegistry::EnsureBuiltinsRegistered()
{
    if (builtins_registered_)
    {
        return;
    }
    builtins_registered_ = true;
    RegisterBuiltinNodes(*this);
}

// ----------------------------------------------------------------------------
// TranspileContext
// ----------------------------------------------------------------------------

TranspileContext::TranspileContext(const GraphDocument& doc)
    : doc_(doc)
{
}

std::string TranspileContext::NextLocal(const std::string& prefix)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s_%d", prefix.c_str(), local_counter_++);
    return buffer;
}

void TranspileContext::EmitPrelude(const std::string& line)
{
    prelude_ += line;
    if (line.empty() || line.back() != '\n')
    {
        prelude_ += '\n';
    }
}

std::string TranspileContext::TakePrelude()
{
    std::string result;
    result.swap(prelude_);
    return result;
}

std::string TranspileContext::EmitFromExec(const GraphNode& node, const std::string& exec_out_name)
{
    const auto links_out = doc_.LinksFromOutput(node.id, exec_out_name);
    if (links_out.empty())
    {
        return std::string();
    }
    const GraphLink* link = links_out.front();
    const GraphNode* next_node = doc_.FindNode(link->to_node);
    if (next_node == nullptr)
    {
        return std::string();
    }
    const NodeSpec* spec = NodeSpecRegistry::Get().Find(next_node->type_key);
    if (spec == nullptr || !spec->emit)
    {
        SetError("Unknown or unsupported node type: " + next_node->type_key);
        return std::string();
    }
    return spec->emit(*this, *next_node);
}

std::string TranspileContext::EvalInput(const GraphNode& node, const std::string& input_name)
{
    const GraphLink* link = doc_.LinkIntoInput(node.id, input_name);

    PinType pin_type = PinType::Any;
    const NodeSpec* spec = NodeSpecRegistry::Get().Find(node.type_key);
    if (spec != nullptr)
    {
        for (const auto& pin : spec->inputs)
        {
            if (pin.name == input_name)
            {
                pin_type = pin.type;
                break;
            }
        }
    }

    if (link == nullptr)
    {
        std::string literal;
        const auto it = node.input_literals.find(input_name);
        if (it != node.input_literals.end())
        {
            literal = it->second;
        }
        else if (spec != nullptr)
        {
            for (const auto& pin : spec->inputs)
            {
                if (pin.name == input_name)
                {
                    literal = pin.default_literal;
                    break;
                }
            }
        }
        return FormatDefault(pin_type, literal);
    }

    // Memoize per-output-pin so impure data nodes aren't evaluated twice.
    char key_buffer[96];
    std::snprintf(key_buffer, sizeof(key_buffer), "%llu:%s",
        static_cast<unsigned long long>(link->from_node), link->from_pin.c_str());
    const std::string cache_key = key_buffer;
    const auto cached = output_cache_.find(cache_key);
    if (cached != output_cache_.end())
    {
        return cached->second;
    }

    const GraphNode* upstream = doc_.FindNode(link->from_node);
    if (upstream == nullptr)
    {
        return FormatDefault(pin_type, "");
    }
    const NodeSpec* upstream_spec = NodeSpecRegistry::Get().Find(upstream->type_key);
    if (upstream_spec == nullptr || !upstream_spec->emit)
    {
        SetError("Unknown or unsupported node type: " + upstream->type_key);
        return FormatDefault(pin_type, "");
    }

    // For data nodes we expect emit() to return either a Lua expression for
    // single-output nodes, or a small custom protocol: the emit() may inject
    // a prelude `local foo = ...` via ctx and return `foo`. Either way the
    // returned string is the expression for this pin.
    //
    // For multi-output data nodes (e.g. Physics.Raycast returning a table),
    // emit() emits a local once via prelude and returns an expression like
    // `hit_0.x` based on the requested pin. To support this, we set a
    // "requested_output" hint via a thread-local field on the context.
    requested_output_pin_ = link->from_pin;
    const std::string expr = upstream_spec->emit(*this, *upstream);
    requested_output_pin_.clear();

    output_cache_[cache_key] = expr;
    return expr;
}

std::string TranspileContext::EscapeLuaString(const std::string& value)
{
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\': result += "\\\\"; break;
        case '"':  result += "\\\""; break;
        case '\n': result += "\\n";  break;
        case '\r': result += "\\r";  break;
        case '\t': result += "\\t";  break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\%03d", static_cast<int>(static_cast<unsigned char>(ch)));
                result += buf;
            }
            else
            {
                result += ch;
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

std::string TranspileContext::FormatDefault(PinType type, const std::string& literal)
{
    switch (type)
    {
    case PinType::Exec:
        return std::string();
    case PinType::Bool:
        if (literal == "true" || literal == "1") return "true";
        return "false";
    case PinType::Number:
    {
        if (literal.empty()) return "0";
        // Validate it parses as a number; fall back to 0.
        char* end = nullptr;
        std::strtod(literal.c_str(), &end);
        if (end != literal.c_str() && *end == '\0')
        {
            return literal;
        }
        return "0";
    }
    case PinType::String:
    case PinType::Object:
    case PinType::Any:
        return EscapeLuaString(literal);
    case PinType::Vec3:
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
#if defined(_MSC_VER)
        sscanf_s(literal.c_str(), "%f,%f,%f", &x, &y, &z);
#else
        std::sscanf(literal.c_str(), "%f,%f,%f", &x, &y, &z);
#endif
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%g, %g, %g", x, y, z);
        return buf;
    }
    }
    return std::string();
}

} // namespace graph
