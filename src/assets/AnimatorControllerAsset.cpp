#include "assets/AnimatorControllerAsset.h"
#include "vfs/AssetVFS.h"

#include <nlohmann/json.hpp>

#include <fstream>

using json = nlohmann::json;

namespace
{
float ReadFloat(const json& object, const char* key, float fallback)
{
    if (!object.is_object())
    {
        return fallback;
    }

    const auto it = object.find(key);
    if (it == object.end() || !it->is_number())
    {
        return fallback;
    }

    return it->get<float>();
}

bool ReadBool(const json& object, const char* key, bool fallback)
{
    if (!object.is_object())
    {
        return fallback;
    }

    const auto it = object.find(key);
    if (it == object.end() || !it->is_boolean())
    {
        return fallback;
    }

    return it->get<bool>();
}

std::string ReadString(const json& object, const char* key)
{
    if (!object.is_object())
    {
        return {};
    }

    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
    {
        return {};
    }

    return it->get<std::string>();
}

json BuildClipJson(const AnimatorClipReference& clip)
{
    json data;
    data["id"] = clip.id;
    data["source_model_path"] = clip.source_model_path;
    data["clip_name"] = clip.clip_name;
    return data;
}

json BuildStateJson(const AnimatorStateDefinition& state)
{
    json data;
    data["name"] = state.name;
    data["clip_id"] = state.clip_id;
    data["playback_speed"] = state.playback_speed;
    data["loop"] = state.loop;
    return data;
}

json BuildTransitionJson(const AnimatorTransitionDefinition& transition)
{
    json data;
    data["from_state"] = transition.from_state;
    data["to_state"] = transition.to_state;
    data["condition"] = transition.condition;
    data["blend_duration"] = transition.blend_duration;
    data["has_exit_time"] = transition.has_exit_time;
    data["exit_time"] = transition.exit_time;
    return data;
}

json BuildBoneModifierJson(const AnimatorBoneModifier& modifier)
{
    json data;
    data["bone_name"] = modifier.bone_name;
    data["modifier_type"] = modifier.modifier_type;
    data["strength"] = modifier.strength;
    data["damping"] = modifier.damping;
    data["stiffness"] = modifier.stiffness;
    return data;
}
} // namespace

AnimatorControllerAsset CreateDefaultAnimatorControllerAsset(const std::string& controller_name)
{
    AnimatorControllerAsset asset;
    if (!controller_name.empty())
    {
        asset.name = controller_name;
    }
    return asset;
}

bool LoadAnimatorControllerAsset(const std::filesystem::path& path, AnimatorControllerAsset& out_asset, std::string& out_error)
{
    out_asset = {};
    out_error.clear();

    json root;

    std::ifstream input(path, std::ios::binary);
    if (input)
    {
        root = json::parse(input, nullptr, false);
    }
    else if (g_asset_reader)
    {
        const std::vector<std::uint8_t> bytes = ReadAssetFileAsBytes(path.generic_string());
        if (!bytes.empty())
        {
            root = json::parse(bytes.begin(), bytes.end(), nullptr, false);
        }
    }

    if (root.is_discarded() || !root.is_object())
    {
        out_error = "Failed to open controller file or controller file is not valid JSON.";
        return false;
    }

    out_asset.version = root.value("version", 1);
    out_asset.name = root.value("name", std::string("NewAnimator"));
    out_asset.default_state = root.value("default_state", std::string());

    if (const auto clips_it = root.find("clips"); clips_it != root.end() && clips_it->is_array())
    {
        for (const json& clip_json : *clips_it)
        {
            if (!clip_json.is_object())
            {
                continue;
            }

            AnimatorClipReference clip;
            clip.id = ReadString(clip_json, "id");
            clip.source_model_path = ReadString(clip_json, "source_model_path");
            clip.clip_name = ReadString(clip_json, "clip_name");
            out_asset.clips.push_back(std::move(clip));
        }
    }

    if (const auto states_it = root.find("states"); states_it != root.end() && states_it->is_array())
    {
        for (const json& state_json : *states_it)
        {
            if (!state_json.is_object())
            {
                continue;
            }

            AnimatorStateDefinition state;
            state.name = ReadString(state_json, "name");
            state.clip_id = ReadString(state_json, "clip_id");
            state.playback_speed = ReadFloat(state_json, "playback_speed", 1.0f);
            state.loop = ReadBool(state_json, "loop", true);
            out_asset.states.push_back(std::move(state));
        }
    }

    if (const auto transitions_it = root.find("transitions"); transitions_it != root.end() && transitions_it->is_array())
    {
        for (const json& transition_json : *transitions_it)
        {
            if (!transition_json.is_object())
            {
                continue;
            }

            AnimatorTransitionDefinition transition;
            transition.from_state = ReadString(transition_json, "from_state");
            transition.to_state = ReadString(transition_json, "to_state");
            transition.condition = ReadString(transition_json, "condition");
            transition.blend_duration = ReadFloat(transition_json, "blend_duration", 0.15f);
            transition.has_exit_time = ReadBool(transition_json, "has_exit_time", false);
            transition.exit_time = ReadFloat(transition_json, "exit_time", 1.0f);
            out_asset.transitions.push_back(std::move(transition));
        }
    }

    if (const auto modifiers_it = root.find("bone_modifiers"); modifiers_it != root.end() && modifiers_it->is_array())
    {
        for (const json& modifier_json : *modifiers_it)
        {
            if (!modifier_json.is_object())
            {
                continue;
            }

            AnimatorBoneModifier modifier;
            modifier.bone_name = ReadString(modifier_json, "bone_name");
            modifier.modifier_type = ReadString(modifier_json, "modifier_type");
            if (modifier.modifier_type.empty())
            {
                modifier.modifier_type = "Jiggle";
            }
            modifier.strength = ReadFloat(modifier_json, "strength", 0.5f);
            modifier.damping = ReadFloat(modifier_json, "damping", 0.5f);
            modifier.stiffness = ReadFloat(modifier_json, "stiffness", 0.5f);
            out_asset.bone_modifiers.push_back(std::move(modifier));
        }
    }

    if (out_asset.default_state.empty() && !out_asset.states.empty())
    {
        out_asset.default_state = out_asset.states.front().name;
    }

    return true;
}

bool SaveAnimatorControllerAsset(const std::filesystem::path& path, const AnimatorControllerAsset& asset, std::string& out_error)
{
    out_error.clear();

    json root;
    root["version"] = asset.version;
    root["name"] = asset.name;
    root["default_state"] = asset.default_state;

    root["clips"] = json::array();
    for (const AnimatorClipReference& clip : asset.clips)
    {
        root["clips"].push_back(BuildClipJson(clip));
    }

    root["states"] = json::array();
    for (const AnimatorStateDefinition& state : asset.states)
    {
        root["states"].push_back(BuildStateJson(state));
    }

    root["transitions"] = json::array();
    for (const AnimatorTransitionDefinition& transition : asset.transitions)
    {
        root["transitions"].push_back(BuildTransitionJson(transition));
    }

    root["bone_modifiers"] = json::array();
    for (const AnimatorBoneModifier& modifier : asset.bone_modifiers)
    {
        root["bone_modifiers"].push_back(BuildBoneModifierJson(modifier));
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        out_error = "Failed to open controller for writing.";
        return false;
    }

    output << root.dump(2) << "\n";
    if (!output.good())
    {
        out_error = "Failed while writing controller file.";
        return false;
    }

    return true;
}
