#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <utility>
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

// What kind of behavior a bone modifier applies. New kinds (IK, look-at,
// constraints, ...) can slot in without reshaping the asset. The integer
// values are persisted indirectly via *TypeToString/*FromString, so keep
// the string mapping stable rather than relying on the numeric order.
enum class AnimatorBoneModifierType
{
    Physics = 0,
    Collision = 1,
};

const char* AnimatorBoneModifierTypeToString(AnimatorBoneModifierType type);
AnimatorBoneModifierType AnimatorBoneModifierTypeFromString(const std::string& text);

// Subtype of a Collision modifier. Trigger = a kinematic sensor that fires
// the owning object's OnTrigger* script callbacks (scriptable hitbox).
// Rigidbody = a kinematic solid that physically pushes dynamic bodies but
// fires no callbacks. Persisted via the string helpers below.
enum class AnimatorBoneCollisionMode
{
    Trigger = 0,
    Rigidbody = 1,
};

const char* AnimatorBoneCollisionModeToString(AnimatorBoneCollisionMode mode);
AnimatorBoneCollisionMode AnimatorBoneCollisionModeFromString(const std::string& text);

struct AnimatorBoneModifier
{
    AnimatorBoneModifierType type = AnimatorBoneModifierType::Physics;
    std::string bone_name;
    // Core spring-damper parameters. Stiffness/damping are normalized
    // [0,1]-ish and converted to internal spring constants at simulate time.
    float strength = 1.0f;   // overall effect multiplier (0 = disabled)
    float damping = 1.0f;    // critical-damping fraction; 1 = no bounce, <1 = oscillatory, >1 = sluggish
    float stiffness = 0.3f;  // spring constant scale; 0 = no return, 1 = snappy
    float mass = 1.0f;       // resistance to acceleration; must be > 0
    float drag = 0.05f;      // simple air drag applied to velocity
    float gravity_scale = 0.0f; // 0 = ignore gravity; 1 = full 9.81 m/s^2
    std::array<float, 3> gravity_dir = {0.0f, -1.0f, 0.0f};
    float angle_limit_deg = 60.0f; // max deflection from animated direction
    float radius = 0.05f;    // for collider pushout (Phase E); harmless otherwise
    bool affects_children = true; // recompute descendant transforms after sim

    // ---- Collision-type parameters -------------------------------------
    // An oriented box attached to the bone's local space. The box follows
    // the bone's animated transform; half_extents/center are in bone-local
    // units. Only meaningful when type == Collision; "Fit to Bone" in the
    // editor seeds these to enclose the bone's segment to its children.
    std::array<float, 3> box_half_extents = {0.05f, 0.05f, 0.05f};
    std::array<float, 3> box_center = {0.0f, 0.0f, 0.0f};
    AnimatorBoneCollisionMode collision_mode = AnimatorBoneCollisionMode::Trigger;
};

// A named facial expression = a sparse set of blendshape (ARKit) target
// weights. Only the targets the pose actually drives are listed; everything
// else is implicitly 0. Weights are usually [0,1] but are not clamped here.
struct FaceExpressionPose
{
    std::string name;
    std::vector<std::pair<std::string, float>> weights; // target name -> weight
};

// Facial-animation configuration on a controller. The Face layer is evaluated
// independently of the skeletal (General) layer. Lip-sync curves layer in on
// top in a later phase.
struct AnimatorFaceConfig
{
    std::string default_pose;             // pose applied at runtime when none is set
    std::vector<FaceExpressionPose> poses;
    // Project-relative paths to baked lip-sync FaceClips (.faceclip) for this
    // controller. A character has one per line of dialogue; any can be played
    // (layered over the expression pose).
    std::vector<std::string> lip_sync_clips;
};

// The 52 ARKit blendshape names, in the canonical ARKit order. Used by the
// Face editor to offer a target palette and as a stable reference set.
const std::vector<std::string>& ArkitBlendshapeNames();

struct AnimatorControllerAsset
{
    int version = 1;
    std::string name = "NewAnimator";
    std::string default_state;
    // Path (project-relative or absolute) to a model file used by the Animator
    // panel for the in-editor skeleton/animation preview. Persisted so the
    // panel's preview rebinds automatically when the controller is reopened.
    std::string preview_model_path;
    std::vector<AnimatorClipReference> clips;
    std::vector<AnimatorStateDefinition> states;
    std::vector<AnimatorTransitionDefinition> transitions;
    std::vector<AnimatorBoneModifier> bone_modifiers;
    AnimatorFaceConfig face;
};

AnimatorControllerAsset CreateDefaultAnimatorControllerAsset(const std::string& controller_name);
bool LoadAnimatorControllerAsset(const std::filesystem::path& path, AnimatorControllerAsset& out_asset, std::string& out_error);
bool SaveAnimatorControllerAsset(const std::filesystem::path& path, const AnimatorControllerAsset& asset, std::string& out_error);
