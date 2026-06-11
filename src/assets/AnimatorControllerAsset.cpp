#include "assets/AnimatorControllerAsset.h"
#include "vfs/AssetVFS.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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
    data["type"] = AnimatorBoneModifierTypeToString(modifier.type);
    data["bone_name"] = modifier.bone_name;
    data["strength"] = modifier.strength;
    data["damping"] = modifier.damping;
    data["stiffness"] = modifier.stiffness;
    data["mass"] = modifier.mass;
    data["drag"] = modifier.drag;
    data["gravity_scale"] = modifier.gravity_scale;
    data["gravity_dir"] = json::array({modifier.gravity_dir[0], modifier.gravity_dir[1], modifier.gravity_dir[2]});
    data["angle_limit_deg"] = modifier.angle_limit_deg;
    data["radius"] = modifier.radius;
    data["affects_children"] = modifier.affects_children;
    data["box_half_extents"] = json::array({modifier.box_half_extents[0], modifier.box_half_extents[1], modifier.box_half_extents[2]});
    data["box_center"] = json::array({modifier.box_center[0], modifier.box_center[1], modifier.box_center[2]});
    data["collision_mode"] = AnimatorBoneCollisionModeToString(modifier.collision_mode);
    return data;
}
} // namespace

const char* AnimatorBoneModifierTypeToString(AnimatorBoneModifierType type)
{
    switch (type)
    {
    case AnimatorBoneModifierType::Physics:
        return "physics";
    case AnimatorBoneModifierType::Collision:
        return "collision";
    }
    return "physics";
}

AnimatorBoneModifierType AnimatorBoneModifierTypeFromString(const std::string& text)
{
    // Unknown or missing types fall back to Physics so controllers saved
    // before the type field existed keep working.
    if (text == "collision")
    {
        return AnimatorBoneModifierType::Collision;
    }
    return AnimatorBoneModifierType::Physics;
}

const char* AnimatorBoneCollisionModeToString(AnimatorBoneCollisionMode mode)
{
    switch (mode)
    {
    case AnimatorBoneCollisionMode::Trigger:
        return "trigger";
    case AnimatorBoneCollisionMode::Rigidbody:
        return "rigidbody";
    }
    return "trigger";
}

AnimatorBoneCollisionMode AnimatorBoneCollisionModeFromString(const std::string& text)
{
    if (text == "rigidbody")
    {
        return AnimatorBoneCollisionMode::Rigidbody;
    }
    return AnimatorBoneCollisionMode::Trigger;
}

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
    out_asset.preview_model_path = root.value("preview_model_path", std::string());

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
            modifier.type = AnimatorBoneModifierTypeFromString(ReadString(modifier_json, "type"));
            modifier.bone_name = ReadString(modifier_json, "bone_name");
            modifier.strength = ReadFloat(modifier_json, "strength", 0.5f);
            modifier.damping = ReadFloat(modifier_json, "damping", 0.5f);
            modifier.stiffness = ReadFloat(modifier_json, "stiffness", 0.5f);
            modifier.mass = std::max(0.001f, ReadFloat(modifier_json, "mass", 1.0f));
            modifier.drag = ReadFloat(modifier_json, "drag", 0.05f);
            modifier.gravity_scale = ReadFloat(modifier_json, "gravity_scale", 0.0f);
            if (const auto gdir_it = modifier_json.find("gravity_dir");
                gdir_it != modifier_json.end() && gdir_it->is_array() && gdir_it->size() == 3)
            {
                modifier.gravity_dir[0] = (*gdir_it)[0].get<float>();
                modifier.gravity_dir[1] = (*gdir_it)[1].get<float>();
                modifier.gravity_dir[2] = (*gdir_it)[2].get<float>();
            }
            modifier.angle_limit_deg = ReadFloat(modifier_json, "angle_limit_deg", 60.0f);
            modifier.radius = ReadFloat(modifier_json, "radius", 0.05f);
            modifier.affects_children = ReadBool(modifier_json, "affects_children", true);
            if (const auto he_it = modifier_json.find("box_half_extents");
                he_it != modifier_json.end() && he_it->is_array() && he_it->size() == 3)
            {
                modifier.box_half_extents[0] = (*he_it)[0].get<float>();
                modifier.box_half_extents[1] = (*he_it)[1].get<float>();
                modifier.box_half_extents[2] = (*he_it)[2].get<float>();
            }
            if (const auto bc_it = modifier_json.find("box_center");
                bc_it != modifier_json.end() && bc_it->is_array() && bc_it->size() == 3)
            {
                modifier.box_center[0] = (*bc_it)[0].get<float>();
                modifier.box_center[1] = (*bc_it)[1].get<float>();
                modifier.box_center[2] = (*bc_it)[2].get<float>();
            }
            modifier.collision_mode = AnimatorBoneCollisionModeFromString(ReadString(modifier_json, "collision_mode"));
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
    root["preview_model_path"] = asset.preview_model_path;

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
