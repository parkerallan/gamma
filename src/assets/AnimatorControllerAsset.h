#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct AnimatorClipReference
{
    std::string id;
    std::string source_model_path;
    std::string clip_name;
};

struct AnimatorStateDefinition
{
    std::string name;
    std::string clip_id;
    float playback_speed = 1.0f;
    bool loop = true;
};

struct AnimatorTransitionDefinition
{
    std::string from_state;
    std::string to_state;
    std::string condition;
    float blend_duration = 0.15f;
    bool has_exit_time = false;
    float exit_time = 1.0f;
};

struct AnimatorBoneModifier
{
    std::string bone_name;
    std::string modifier_type = "Jiggle";
    float strength = 0.5f;
    float damping = 0.5f;
    float stiffness = 0.5f;
};

struct AnimatorControllerAsset
{
    int version = 1;
    std::string name = "NewAnimator";
    std::string default_state;
    std::vector<AnimatorClipReference> clips;
    std::vector<AnimatorStateDefinition> states;
    std::vector<AnimatorTransitionDefinition> transitions;
    std::vector<AnimatorBoneModifier> bone_modifiers;
};

AnimatorControllerAsset CreateDefaultAnimatorControllerAsset(const std::string& controller_name);
bool LoadAnimatorControllerAsset(const std::filesystem::path& path, AnimatorControllerAsset& out_asset, std::string& out_error);
bool SaveAnimatorControllerAsset(const std::filesystem::path& path, const AnimatorControllerAsset& asset, std::string& out_error);
