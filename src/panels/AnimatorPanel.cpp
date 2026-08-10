#include "panels/AnimatorPanel.h"

#include "assets/AnimatorControllerAsset.h"
#include "assets/FaceClipAsset.h"
#include "assets/LipSyncBake.h"
#include "assets/ModelMetadata.h"
#include "imgui.h"
#include "state/EngineState.h"
#include "ui/Codicons.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_set>
#include <string>
#include <vector>

namespace
{
constexpr const char* kFileTreeDragDropPayload = "FILE_TREE_PATH";

std::uint64_t ComputeDirectorySignature(const std::filesystem::path& dir)
{
    if (dir.empty() || !std::filesystem::is_directory(dir))
    {
        return 0;
    }

    std::uint64_t signature = 1;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".anim")
        {
            signature ^= std::hash<std::string>{}(entry.path().filename().string());
            signature *= 1099511628211ull;
        }
    }
    return signature;
}

bool InputTextString(const char* label, std::string& value, ImGuiInputTextFlags flags = 0)
{
    std::vector<char> buffer((std::max)(std::size_t(256), value.size() + 1));
    std::copy(value.begin(), value.end(), buffer.begin());
    buffer[value.size()] = '\0';

    if (!ImGui::InputText(label, buffer.data(), buffer.size(), flags))
    {
        return false;
    }

    value = buffer.data();
    return true;
}

// Editor controls for a Physics-type bone modifier (spring-damper jiggle).
// Returns true when any parameter changed so the caller can mark the
// controller dirty. Other modifier types added to AnimatorBoneModifierType
// get their own Draw*ModifierUI sibling.
bool DrawPhysicsModifierUI(AnimatorBoneModifier& modifier)
{
    bool changed = false;

    // Presets: pick a sensible defaults bundle so users don't
    // have to dial in seven sliders to get a reasonable feel.
    // "Custom" is a no-op so the user can keep their hand-tuned
    // values when reopening the controller. Damping uses the
    // critical-damping fraction interpretation (1 = no bounce).
    struct PresetDef
    {
        const char* name;
        float strength;
        float stiffness;
        float damping;
        float mass;
        float drag;
        float gravity_scale;
        std::array<float, 3> gravity_dir;
        float angle_limit_deg;
        bool affects_children;
    };
    static constexpr std::array<PresetDef, 7> presets{{
        {"Custom",      0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, {0,-1,0},  0.0f, false},
        // Snappy short bob — quick return, slight overshoot.
        {"Hair (short)",     1.0f, 0.85f, 0.45f, 0.4f, 0.04f, 0.0f, {0,-1,0}, 50.0f, true},
        // Longer hair, slower oscillation, modest gravity so the
        // strand sags slightly when the character isn't moving.
        {"Hair (long)",      1.0f, 0.55f, 0.55f, 0.9f, 0.05f, 0.05f, {0,-1,0}, 60.0f, true},
        // Realistic breast jiggle: snappy stiffness so the bone
        // springs back to the rest (animated) position quickly,
        // moderate damping for a brief bounce. NO gravity — the
        // rest pose is already the correct hanging position;
        // adding gravity would just sag it below rest forever.
        {"Breast",           1.0f, 0.95f, 0.45f, 1.0f, 0.04f, 0.0f, {0,-1,0}, 30.0f, true},
        // Light cloth flapping. Gravity here is meaningful since
        // cloth's animated rest is rarely fully draped.
        {"Cloth (light)",    1.0f, 0.35f, 0.85f, 0.4f, 0.10f, 0.15f, {0,-1,0}, 90.0f, true},
        // Heavy fabric — slower, more damped.
        {"Cloth (heavy)",    1.0f, 0.25f, 1.10f, 1.6f, 0.15f, 0.25f, {0,-1,0}, 100.0f, true},
        // Floppy appendage: tail / antenna / ear.
        {"Tail / Antenna",   1.0f, 0.70f, 0.50f, 0.7f, 0.05f, 0.0f, {0,-1,0}, 70.0f, true},
    }};
    static const char* preset_names[presets.size()] = {
        presets[0].name, presets[1].name, presets[2].name, presets[3].name,
        presets[4].name, presets[5].name, presets[6].name,
    };
    // Small helper: append a "(?)" hint after the previous
    // widget. Hover to see a plain-English description.
    auto tooltip = [](const char* text)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    int preset_index = 0;
    if (ImGui::Combo("Preset", &preset_index, preset_names, static_cast<int>(presets.size())))
    {
        if (preset_index > 0)
        {
            const PresetDef& p = presets[static_cast<std::size_t>(preset_index)];
            modifier.strength = p.strength;
            modifier.stiffness = p.stiffness;
            modifier.damping = p.damping;
            modifier.mass = p.mass;
            modifier.drag = p.drag;
            modifier.gravity_scale = p.gravity_scale;
            modifier.gravity_dir = p.gravity_dir;
            modifier.angle_limit_deg = p.angle_limit_deg;
            modifier.affects_children = p.affects_children;
            changed = true;
        }
    }

    if (ImGui::DragFloat("Strength", &modifier.strength, 0.01f, 0.0f, 2.0f, "%.2f"))
    {
        modifier.strength = std::clamp(modifier.strength, 0.0f, 2.0f);
        changed = true;
    }
    tooltip("How much of the simulated bone offset is applied to the final pose.\n"
            "0 = no effect (bone stays glued to the animation).\n"
            "1 = full physics influence.\n"
            "Use values above 1 only to exaggerate motion.");

    if (ImGui::DragFloat("Stiffness", &modifier.stiffness, 0.01f, 0.0f, 1.0f, "%.2f"))
    {
        modifier.stiffness = std::clamp(modifier.stiffness, 0.0f, 1.0f);
        changed = true;
    }
    tooltip("How strongly the bone is pulled back to its rest (animated) position.\n"
            "Low = floppy, slow oscillation (long hair, cloth).\n"
            "High = snappy, quick return (short hair, breast jiggle).");

    if (ImGui::DragFloat("Damping", &modifier.damping, 0.01f, 0.0f, 3.0f, "%.2f"))
    {
        modifier.damping = std::clamp(modifier.damping, 0.0f, 3.0f);
        changed = true;
    }
    tooltip("How quickly oscillation dies out (critical-damping fraction).\n"
            "0 = bouncy / wobbly forever.\n"
            "1 = no bounce, settles in one swing.\n"
            ">1 = overdamped, sluggish return.");

    if (ImGui::DragFloat("Mass", &modifier.mass, 0.01f, 0.001f, 100.0f, "%.3f"))
    {
        modifier.mass = std::max(0.001f, modifier.mass);
        changed = true;
    }
    tooltip("Inertia of the simulated bone.\n"
            "Low = light, reacts instantly (whiskers, ribbons).\n"
            "High = heavy, slow to start and stop (ponytails, thick cloth).");

    if (ImGui::DragFloat("Drag", &modifier.drag, 0.005f, 0.0f, 1.0f, "%.3f"))
    {
        modifier.drag = std::clamp(modifier.drag, 0.0f, 1.0f);
        changed = true;
    }
    tooltip("Air resistance applied to bone velocity each frame.\n"
            "Bleeds off motion smoothly without affecting stiffness/damping tuning.\n"
            "Useful to kill jitter without making the bone feel mushy.");

    if (ImGui::DragFloat("Gravity Scale", &modifier.gravity_scale, 0.01f, 0.0f, 5.0f, "%.2f"))
    {
        modifier.gravity_scale = std::clamp(modifier.gravity_scale, 0.0f, 5.0f);
        changed = true;
    }
    tooltip("Multiplier applied to the Gravity Dir vector (in Advanced).\n"
            "0 = no gravity; rest pose is the resting position.\n"
            "Set above 0 only when the animated bone is NOT already where gravity would settle it (e.g. flag cloth at rest pose horizontal).\n"
            "For body parts (hair/breast/etc) leave at 0 — non-zero will sag the bone below its rest position.");

    if (ImGui::TreeNode("Advanced"))
    {
        if (ImGui::DragFloat3("Gravity Dir", modifier.gravity_dir.data(), 0.01f, -1.0f, 1.0f, "%.2f"))
        {
            changed = true;
        }
        tooltip("World-space direction of gravity for this bone (X, Y, Z).\n"
                "Default (0, -1, 0) is straight down. Only has an effect when Gravity Scale > 0.");

        if (ImGui::DragFloat("Angle Limit", &modifier.angle_limit_deg, 0.5f, 0.0f, 180.0f, "%.1f deg"))
        {
            modifier.angle_limit_deg = std::clamp(modifier.angle_limit_deg, 0.0f, 180.0f);
            changed = true;
        }
        tooltip("Maximum angle (degrees) the bone can deflect from its rest direction before being clamped.\n"
                "Lower = stiffer cone (small wobble).\n"
                "Higher = more freedom of movement.");

        if (ImGui::DragFloat("Radius", &modifier.radius, 0.001f, 0.0f, 1.0f, "%.3f"))
        {
            modifier.radius = std::max(0.0f, modifier.radius);
            changed = true;
        }
        tooltip("Collision radius around the bone in meters.\n"
                "Reserved for future collider support; currently informational.");

        if (ImGui::Checkbox("Affects Children", &modifier.affects_children))
        {
            changed = true;
        }
        tooltip("When enabled, rotating this bone also carries its descendant bones along.\n"
                "Required for chains (hair strands, tails) so children don't tear away from the parent.");
        ImGui::TreePop();
    }

    return changed;
}

// Editor controls for a Collision-type bone modifier: an oriented box, in
// bone-local space, that tracks the bone. Returns true when any value
// changed. The preview renderer supplies the "Fit to Bone" auto-sizing.
bool DrawCollisionModifierUI(AnimatorBoneModifier& modifier, AnimatorPreviewRenderer& preview_renderer)
{
    bool changed = false;

    auto tooltip = [](const char* text)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };

    ImGui::TextDisabled("Box collider (bone-local space)");

    // Subtype: Trigger fires the owning object's OnTrigger* callbacks
    // (scriptable hitbox); Rigidbody is a kinematic solid that pushes
    // dynamic bodies but is not scriptable.
    static const char* mode_names[] = {"Trigger", "Rigidbody"};
    int mode_index = static_cast<int>(modifier.collision_mode);
    if (ImGui::Combo("Mode", &mode_index, mode_names, IM_ARRAYSIZE(mode_names)))
    {
        modifier.collision_mode = static_cast<AnimatorBoneCollisionMode>(mode_index);
        changed = true;
    }
    tooltip("Trigger: a sensor hitbox that fires this object's OnTriggerEnter/Stay/Exit\n"
            "script callbacks (the bone name is passed as the last argument). No physical push.\n\n"
            "Rigidbody: a kinematic solid that physically shoves dynamic rigidbodies the bone\n"
            "sweeps through. Purely physical — it fires no script callbacks.");

    if (ImGui::Button(ICON_CI_ARROW_BOTH " Fit to Bone"))
    {
        // Re-derive the box from the bone's current pose. No-op (leaves the
        // values as-is) when the bone isn't present in the preview model.
        if (preview_renderer.ComputeBoneFitBox(
                modifier.bone_name, modifier.box_half_extents, modifier.box_center))
        {
            changed = true;
        }
    }
    tooltip("Resize and recenter the box to enclose this bone's segment to its child joints.\n"
            "Uses the bone's pose as currently shown in the preview.");

    if (ImGui::DragFloat3("Half Extents", modifier.box_half_extents.data(), 0.005f, 0.0f, 100.0f, "%.3f"))
    {
        for (float& v : modifier.box_half_extents) { v = std::max(0.0f, v); }
        changed = true;
    }
    tooltip("Half the box size along each bone-local axis (X, Y, Z), in model units.\n"
            "The full box spans twice these values.");

    if (ImGui::DragFloat3("Center Offset", modifier.box_center.data(), 0.005f, -100.0f, 100.0f, "%.3f"))
    {
        changed = true;
    }
    tooltip("Offset of the box center from the bone origin, in bone-local space (X, Y, Z).");

    return changed;
}

std::string BuildDefaultClipId(std::size_t index)
{
    return "Clip_" + std::to_string(index + 1);
}

std::string BuildDefaultStateName(std::size_t index)
{
    return "State_" + std::to_string(index + 1);
}

std::string ToLowerAscii(std::string value)
{
    for (char& ch : value)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool HasAnyExtension(const std::filesystem::path& path, const std::initializer_list<const char*>& extensions)
{
    const std::string ext = ToLowerAscii(path.extension().string());
    for (const char* expected : extensions)
    {
        if (ext == expected)
        {
            return true;
        }
    }
    return false;
}

std::string NormalizeAssetPath(const EngineState& state, const std::filesystem::path& absolute_or_relative)
{
    std::error_code ec;
    const std::filesystem::path project_root = std::filesystem::weakly_canonical(state.project_root, ec);
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(absolute_or_relative, ec);
    if (!ec && !project_root.empty())
    {
        const std::filesystem::path relative = std::filesystem::relative(candidate, project_root, ec);
        const bool escapes_project_root =
            !relative.empty() &&
            relative.begin() != relative.end() &&
            (*relative.begin() == std::filesystem::path(".."));
        if (!ec && !relative.empty() && !escapes_project_root)
        {
            return relative.generic_string();
        }
    }

    if (absolute_or_relative.is_absolute())
    {
        return absolute_or_relative.generic_string();
    }
    return std::filesystem::path(state.project_root / absolute_or_relative).generic_string();
}

std::filesystem::path ResolveProjectPath(const EngineState& state, const std::filesystem::path& path)
{
    if (path.is_absolute())
    {
        return path;
    }

    if (state.project_root.empty())
    {
        return path;
    }

    return state.project_root / path;
}

std::string MakeUniqueName(const std::string& base_name, const std::unordered_set<std::string>& used_names)
{
    if (base_name.empty())
    {
        return {};
    }

    if (used_names.find(base_name) == used_names.end())
    {
        return base_name;
    }

    for (int suffix = 2; suffix < 10000; ++suffix)
    {
        const std::string candidate = base_name + "_" + std::to_string(suffix);
        if (used_names.find(candidate) == used_names.end())
        {
            return candidate;
        }
    }

    return base_name;
}
} // namespace

void AnimatorPanel::RefreshControllerList(const std::filesystem::path& animators_dir)
{
    const std::uint64_t current_signature = ComputeDirectorySignature(animators_dir);
    if (animators_dir == last_scanned_dir_ && current_signature == last_dir_signature_)
    {
        return;
    }

    last_scanned_dir_ = animators_dir;
    last_dir_signature_ = current_signature;

    controller_names_.clear();
    controller_paths_.clear();
    controller_names_.push_back("New +");
    controller_paths_.push_back({});

    if (!animators_dir.empty() && std::filesystem::is_directory(animators_dir))
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(animators_dir, ec))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".anim")
            {
                continue;
            }

            controller_names_.push_back(entry.path().stem().string());
            controller_paths_.push_back(entry.path());
        }
    }
}

void AnimatorPanel::EnsureControllerLoaded(const std::filesystem::path& controller_path, EngineState& state)
{
    if (controller_loaded_ && loaded_controller_path_ == controller_path)
    {
        return;
    }

    AnimatorControllerAsset loaded;
    std::string error;
    if (!LoadAnimatorControllerAsset(controller_path, loaded, error))
    {
        state.AddError("Failed to load animator controller: " + state.GetDisplayPath(controller_path) + " (" + error + ")");
        loaded = CreateDefaultAnimatorControllerAsset(controller_path.stem().string());
    }

    controller_ = std::move(loaded);
    loaded_controller_path_ = controller_path;
    controller_loaded_ = true;
    controller_dirty_ = false;
}

bool AnimatorPanel::SaveCurrentController(const std::filesystem::path& controller_path, EngineState& state)
{
    if (!controller_loaded_)
    {
        return false;
    }

    if (controller_.states.empty())
    {
        controller_.states.push_back(AnimatorStateDefinition{BuildDefaultStateName(0), {}, 1.0f, true});
    }
    if (controller_.default_state.empty())
    {
        controller_.default_state = controller_.states.front().name;
    }

    std::string error;
    if (!SaveAnimatorControllerAsset(controller_path, controller_, error))
    {
        state.AddError("Failed to save animator controller: " + state.GetDisplayPath(controller_path) + " (" + error + ")");
        return false;
    }

    loaded_controller_path_ = controller_path;
    controller_dirty_ = false;
    state.AddLog("Saved animator controller: " + controller_path.filename().string());
    return true;
}

void AnimatorPanel::MarkControllerListDirty()
{
    last_scanned_dir_.clear();
    last_dir_signature_ = 0;
}

void AnimatorPanel::Render(EngineState& state, VulkanContext* vulkan_context)
{
    vulkan_context_ = vulkan_context;
    if (!state.show_animator_panel)
    {
        return;
    }

    if (!ImGui::Begin("Animator", &state.show_animator_panel))
    {
        ImGui::End();
        return;
    }

    const std::filesystem::path current_animators_dir = state.project_root.empty()
        ? std::filesystem::path()
        : state.project_root / "Assets" / "Animators";

    RefreshControllerList(current_animators_dir);

    if (controller_names_.empty() || controller_paths_.empty())
    {
        controller_names_.push_back("New +");
        controller_paths_.push_back({});
    }

    if (selected_controller_index_ < 0)
    {
        selected_controller_index_ = 0;
    }
    if (selected_controller_index_ >= static_cast<int>(controller_paths_.size()))
    {
        selected_controller_index_ = static_cast<int>(controller_paths_.size()) - 1;
    }

    std::vector<const char*> controller_name_ptrs;
    for (const auto& name : controller_names_)
    {
        controller_name_ptrs.push_back(name.c_str());
    }

    const int previous_index = selected_controller_index_;
    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("##AnimatorGraphSelector", &selected_controller_index_, controller_name_ptrs.data(), static_cast<int>(controller_name_ptrs.size()));
    if (selected_controller_index_ != previous_index)
    {
        if (selected_controller_index_ == 0)
        {
            if (new_controller_name_[0] == '\0')
            {
                std::snprintf(new_controller_name_, sizeof(new_controller_name_), "NewAnimator");
            }
            controller_ = CreateDefaultAnimatorControllerAsset(new_controller_name_);
            loaded_controller_path_.clear();
            controller_loaded_ = true;
            controller_dirty_ = false;
        }
        else
        {
            new_controller_name_[0] = '\0';
            EnsureControllerLoaded(controller_paths_[selected_controller_index_], state);
        }
    }

    const bool is_new_controller = selected_controller_index_ == 0;
    if (!controller_loaded_)
    {
        if (is_new_controller)
        {
            controller_ = CreateDefaultAnimatorControllerAsset(new_controller_name_);
            controller_loaded_ = true;
        }
        else if (selected_controller_index_ >= 0 && selected_controller_index_ < static_cast<int>(controller_paths_.size()))
        {
            EnsureControllerLoaded(controller_paths_[selected_controller_index_], state);
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    if (!is_new_controller)
    {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputTextWithHint("##AnimatorGraphName", "", new_controller_name_, sizeof(new_controller_name_)))
    {
        selected_controller_index_ = 0;
        controller_ = CreateDefaultAnimatorControllerAsset(new_controller_name_);
        loaded_controller_path_.clear();
        controller_loaded_ = true;
        controller_dirty_ = true;
    }
    if (!is_new_controller)
    {
        ImGui::EndDisabled();
    }

    {
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float save_w = ImGui::CalcTextSize(ICON_CI_SAVE).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float build_w = ImGui::CalcTextSize(ICON_CI_RUN_WITH_DEPS).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float play_w = ImGui::CalcTextSize(ICON_CI_DEBUG_START).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        const float toolbar_total = save_w + build_w + play_w + sp * 2.0f;
        const float cur_x = ImGui::GetCursorPosX();
        const float avail = ImGui::GetContentRegionAvail().x;
        if (controller_dirty_)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Unsaved changes");
        }
        ImGui::SameLine();
        if (avail > toolbar_total)
        {
            ImGui::SetCursorPosX(cur_x + avail - toolbar_total);
        }
        if (ImGui::Button(ICON_CI_SAVE))
        {
            if (is_new_controller)
            {
                if (!state.project_root.empty() && new_controller_name_[0] != '\0')
                {
                    const std::filesystem::path graphs_dir = state.project_root / "Assets" / "Animators";
                    std::error_code ec;
                    std::filesystem::create_directories(graphs_dir, ec);

                    controller_.name = new_controller_name_;
                    const std::filesystem::path target_path = graphs_dir / (std::string(new_controller_name_) + ".anim");
                    if (SaveCurrentController(target_path, state))
                    {
                        MarkControllerListDirty();
                        RefreshControllerList(current_animators_dir);
                        for (int index = 1; index < static_cast<int>(controller_paths_.size()); ++index)
                        {
                            if (controller_paths_[index] == target_path)
                            {
                                selected_controller_index_ = index;
                                break;
                            }
                        }
                    }
                }
                else
                {
                    state.AddWarning("Controller name is required.");
                }
            }
            else if (selected_controller_index_ >= 0 && selected_controller_index_ < static_cast<int>(controller_paths_.size()))
            {
                if (!controller_.name.empty())
                {
                    controller_names_[selected_controller_index_] = controller_.name;
                }
                SaveCurrentController(controller_paths_[selected_controller_index_], state);
            }
        }
        ImGui::SameLine();
        if (!state.CanBuildProject()) { ImGui::BeginDisabled(); }
        if (ImGui::Button(ICON_CI_RUN_WITH_DEPS)) { state.TriggerBuildAction(); }
        if (!state.CanBuildProject()) { ImGui::EndDisabled(); }
        ImGui::SameLine();
        if (!state.CanPlayScene()) { ImGui::BeginDisabled(); }
        if (ImGui::Button(ICON_CI_DEBUG_START)) { state.TriggerPlayAction(); }
        if (!state.CanPlayScene()) { ImGui::EndDisabled(); }
    }

    ImGui::Separator();

    EnsurePreviewModelLoaded(state);

    RenderControllerEditor(state);

    ImGui::End();
}

void AnimatorPanel::RenderFaceTab(EngineState& state)
{
    (void)state;
    AnimatorFaceConfig& face = controller_.face;

    // ---- Default pose selector ----
    {
        std::vector<const char*> pose_name_ptrs;
        pose_name_ptrs.push_back("<None>");
        int default_index = 0;
        for (std::size_t i = 0; i < face.poses.size(); ++i)
        {
            pose_name_ptrs.push_back(face.poses[i].name.c_str());
            if (face.poses[i].name == face.default_pose)
            {
                default_index = static_cast<int>(i) + 1;
            }
        }
        if (ImGui::Combo("Default Pose", &default_index, pose_name_ptrs.data(),
                         static_cast<int>(pose_name_ptrs.size())))
        {
            face.default_pose = default_index <= 0
                ? std::string()
                : face.poses[static_cast<std::size_t>(default_index - 1)].name;
            controller_dirty_ = true;
        }
        ImGui::TextDisabled("Applied at runtime when no pose is set by script.");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Expression Poses");

    if (ImGui::Button(ICON_CI_ADD " Add Pose"))
    {
        FaceExpressionPose pose;
        pose.name = "Pose " + std::to_string(face.poses.size() + 1);
        face.poses.push_back(std::move(pose));
        selected_face_pose_index_ = static_cast<int>(face.poses.size()) - 1;
        controller_dirty_ = true;
    }

    // Keep the preview selection in range (poses may have been removed).
    if (selected_face_pose_index_ >= static_cast<int>(face.poses.size()))
    {
        selected_face_pose_index_ = -1;
    }

    const std::vector<std::string>& arkit_names = ArkitBlendshapeNames();

    for (std::size_t index = 0; index < face.poses.size(); ++index)
    {
        FaceExpressionPose& pose = face.poses[index];
        ImGui::PushID(static_cast<int>(index));

        const std::string title = pose.name.empty() ? "Pose" : pose.name;
        // Use a stable per-index ID for the node (not the name) so renaming the
        // pose in the field below doesn't change the node's ID and steal focus
        // after each keystroke.
        if (ImGui::TreeNode(reinterpret_cast<void*>(static_cast<std::uintptr_t>(index)), "%s", title.c_str()))
        {
            if (InputTextString("Name", pose.name))
            {
                controller_dirty_ = true;
            }

            bool previewing = (selected_face_pose_index_ == static_cast<int>(index));
            if (ImGui::Checkbox("Preview in viewport", &previewing))
            {
                selected_face_pose_index_ = previewing ? static_cast<int>(index) : -1;
            }

            const bool is_default = (face.default_pose == pose.name) && !pose.name.empty();
            if (is_default)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(default)");
            }
            else if (ImGui::Button("Set As Default"))
            {
                face.default_pose = pose.name;
                controller_dirty_ = true;
            }

            ImGui::TextDisabled("Blendshape weights");

            // Add a target from the ARKit palette (offers only shapes not yet
            // present on this pose).
            {
                std::vector<const char*> available_ptrs;
                std::vector<int> available_src; // combo entry -> ARKit index
                available_ptrs.push_back("+ Add blendshape...");
                available_src.push_back(-1);
                for (int a = 0; a < static_cast<int>(arkit_names.size()); ++a)
                {
                    bool present = false;
                    for (const auto& w : pose.weights)
                    {
                        if (w.first == arkit_names[a])
                        {
                            present = true;
                            break;
                        }
                    }
                    if (!present)
                    {
                        available_ptrs.push_back(arkit_names[a].c_str());
                        available_src.push_back(a);
                    }
                }
                int add_index = 0;
                if (ImGui::Combo("##addshape", &add_index, available_ptrs.data(),
                                 static_cast<int>(available_ptrs.size())) &&
                    add_index > 0)
                {
                    pose.weights.emplace_back(
                        arkit_names[static_cast<std::size_t>(available_src[static_cast<std::size_t>(add_index)])],
                        1.0f);
                    controller_dirty_ = true;
                }
            }

            for (std::size_t wi = 0; wi < pose.weights.size(); ++wi)
            {
                ImGui::PushID(static_cast<int>(wi));
                if (ImGui::Button(ICON_CI_TRASH))
                {
                    pose.weights.erase(pose.weights.begin() + static_cast<std::ptrdiff_t>(wi));
                    controller_dirty_ = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::SameLine();
                if (ImGui::SliderFloat(pose.weights[wi].first.c_str(), &pose.weights[wi].second,
                                       0.0f, 1.0f, "%.2f"))
                {
                    controller_dirty_ = true;
                }
                ImGui::PopID();
            }

            if (ImGui::Button(ICON_CI_TRASH " Remove Pose"))
            {
                const std::string removed = pose.name;
                face.poses.erase(face.poses.begin() + static_cast<std::ptrdiff_t>(index));
                if (face.default_pose == removed)
                {
                    face.default_pose.clear();
                }
                if (selected_face_pose_index_ == static_cast<int>(index))
                {
                    selected_face_pose_index_ = -1;
                }
                controller_dirty_ = true;
                ImGui::TreePop();
                ImGui::PopID();
                break;
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (face.poses.empty())
    {
        ImGui::TextDisabled("No expression poses yet. Add one to author a facial\n"
                            "expression from ARKit blendshapes.");
    }

    // ---- Lip Sync (offline Rhubarb bake) ----
    ImGui::Separator();
    ImGui::TextUnformatted("Lip Sync (Rhubarb)");

    // Rhubarb is bundled in tools/rhubarb/ and auto-discovered -- no path to set.
    const bool rhubarb_found = !ResolveRhubarbExe().empty();
    if (!rhubarb_found)
    {
        ImGui::TextDisabled("Rhubarb not found -- put it in tools/rhubarb/rhubarb.exe.");
    }

    // WAV to bake: drag a .wav from the Files panel onto this target. The
    // button just shows the current selection -- nothing to type.
    const std::string wav_label = face_bake_wav_path_.empty()
        ? std::string("Drop a .wav here")
        : std::filesystem::path(face_bake_wav_path_).filename().string();
    ImGui::Button(wav_label.c_str(), ImVec2(-1.0f, 0.0f));
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
        {
            const char* payload_text = static_cast<const char*>(payload->Data);
            const std::size_t payload_size = payload->DataSize > 0
                ? static_cast<std::size_t>(payload->DataSize - 1)
                : 0;
            const std::filesystem::path dropped(std::string(payload_text, payload_size));
            face_bake_wav_path_ = ResolveProjectPath(state, dropped).string();
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::BeginDisabled(!rhubarb_found || face_bake_wav_path_.empty());
    const bool bake_clicked = ImGui::Button("Bake Lip-Sync");
    ImGui::EndDisabled();
    if (bake_clicked)
    {
        face_bake_status_.clear();
        const std::filesystem::path wav = ResolveProjectPath(state, std::filesystem::path(face_bake_wav_path_));
        FaceClipAsset clip;
        std::string err;
        if (BakeFaceClipFromAudio(wav, clip, err))
        {
            clip.name = wav.stem().string();
            clip.source_audio_path = NormalizeAssetPath(state, wav);
            std::filesystem::path clip_path = wav;
            clip_path.replace_extension(".faceclip");
            std::string save_err;
            if (SaveFaceClipAsset(clip_path, clip, save_err))
            {
                const std::string rel = NormalizeAssetPath(state, clip_path);
                if (std::find(face.lip_sync_clips.begin(), face.lip_sync_clips.end(), rel) ==
                    face.lip_sync_clips.end())
                {
                    face.lip_sync_clips.push_back(rel);
                }
                controller_dirty_ = true;
                char buf[128];
                std::snprintf(buf, sizeof(buf), "Baked %zu curve(s), %.2fs.",
                              clip.curves.size(), clip.duration_seconds);
                face_bake_status_ = buf;
                state.AddLog("Lip-sync baked: " + state.GetDisplayPath(clip_path));
            }
            else
            {
                face_bake_status_ = "Bake succeeded but save failed: " + save_err;
            }
        }
        else
        {
            face_bake_status_ = "Bake failed: " + err;
            state.AddError("Lip-sync bake failed: " + err);
        }
    }

    if (!face_bake_status_.empty())
    {
        ImGui::TextWrapped("%s", face_bake_status_.c_str());
    }

    if (face.lip_sync_clips.empty())
    {
        ImGui::TextDisabled("No lip-sync clips baked for this controller yet.");
    }
    else
    {
        ImGui::TextDisabled("Clips");
    }

    // Keep the previewing index in range (clips may have been removed).
    if (face_lipsync_clip_index_ >= static_cast<int>(face.lip_sync_clips.size()))
    {
        face_lipsync_clip_index_ = -1;
        face_lipsync_playing_ = false;
    }

    for (std::size_t index = 0; index < face.lip_sync_clips.size(); ++index)
    {
        ImGui::PushID(static_cast<int>(index));
        const bool is_playing = face_lipsync_playing_ &&
                                face_lipsync_clip_index_ == static_cast<int>(index);
        if (ImGui::Button(is_playing ? "Stop" : "Play"))
        {
            if (is_playing)
            {
                face_lipsync_playing_ = false;
            }
            else
            {
                face_lipsync_clip_index_ = static_cast<int>(index);
                face_lipsync_playing_ = true;
                face_lipsync_time_seconds_ = 0.0f;
                face_preview_clip_loaded_.clear(); // force reload of the chosen clip
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_CI_TRASH))
        {
            face.lip_sync_clips.erase(face.lip_sync_clips.begin() + static_cast<std::ptrdiff_t>(index));
            controller_dirty_ = true;
            if (face_lipsync_clip_index_ == static_cast<int>(index))
            {
                face_lipsync_playing_ = false;
                face_lipsync_clip_index_ = -1;
            }
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(std::filesystem::path(face.lip_sync_clips[index]).filename().string().c_str());
        ImGui::PopID();
    }
}

void AnimatorPanel::RenderControllerEditor(EngineState& state)
{
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float total_width = ImGui::GetContentRegionAvail().x;
    const float available_height = ImGui::GetContentRegionAvail().y;
    const float library_height = 220.0f;
    const float top_height = available_height - library_height - spacing;
    const float half_width = (total_width - spacing) * 0.5f;

    ImGui::BeginChild("AnimatorViewport", ImVec2(half_width, top_height), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    RenderPreviewViewport(state);
    ImGui::EndChild();

    ImGui::SameLine(0.0f, spacing);

    ImGui::BeginChild("NodeGraphCanvas", ImVec2(0.0f, top_height), true);
    if (controller_loaded_)
    {
        if (InputTextString("Controller Name", controller_.name))
        {
            controller_dirty_ = true;
        }

        ImGui::BeginTabBar("AnimatorEditorTabs");
        const bool general_tab_open = ImGui::BeginTabItem("General");
        if (general_tab_open)
        {
        if (ImGui::CollapsingHeader("States", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::Button(ICON_CI_ADD " Add State"))
            {
                AnimatorStateDefinition new_state;
                new_state.name = BuildDefaultStateName(controller_.states.size());
                controller_.states.push_back(std::move(new_state));
                if (controller_.default_state.empty())
                {
                    controller_.default_state = controller_.states.back().name;
                }
                controller_dirty_ = true;
            }

            std::vector<std::string> clip_ids;
            clip_ids.reserve(controller_.clips.size() + 1);
            clip_ids.push_back("<None>");
            for (const AnimatorClipReference& clip : controller_.clips)
            {
                clip_ids.push_back(clip.id.empty() ? std::string("<Unnamed>") : clip.id);
            }

            for (std::size_t index = 0; index < controller_.states.size(); ++index)
            {
                AnimatorStateDefinition& state_def = controller_.states[index];
                ImGui::PushID(static_cast<int>(index));
                const std::string title = state_def.name.empty() ? "State" : state_def.name;
                // Stable per-index node ID so renaming the state below doesn't
                // change the node ID and steal focus after each keystroke.
                if (ImGui::TreeNode(reinterpret_cast<void*>(static_cast<std::uintptr_t>(index)), "%s", title.c_str()))
                {
                    if (InputTextString("Name", state_def.name))
                    {
                        controller_dirty_ = true;
                    }

                    int clip_index = 0;
                    for (int clip_slot = 1; clip_slot < static_cast<int>(clip_ids.size()); ++clip_slot)
                    {
                        if (clip_ids[clip_slot] == state_def.clip_id)
                        {
                            clip_index = clip_slot;
                            break;
                        }
                    }

                    std::vector<const char*> clip_name_ptrs;
                    clip_name_ptrs.reserve(clip_ids.size());
                    for (const std::string& clip_id : clip_ids)
                    {
                        clip_name_ptrs.push_back(clip_id.c_str());
                    }

                    if (ImGui::Combo("Clip", &clip_index, clip_name_ptrs.data(), static_cast<int>(clip_name_ptrs.size())))
                    {
                        state_def.clip_id = clip_index <= 0 ? std::string() : clip_ids[clip_index];
                        controller_dirty_ = true;
                    }

                    if (ImGui::DragFloat("Playback Speed", &state_def.playback_speed, 0.01f, 0.0f, 8.0f, "%.2f"))
                    {
                        state_def.playback_speed = (std::max)(0.0f, state_def.playback_speed);
                        controller_dirty_ = true;
                    }
                    if (ImGui::Checkbox("Loop", &state_def.loop))
                    {
                        controller_dirty_ = true;
                    }

                    const bool is_default = controller_.default_state == state_def.name;
                    if (is_default)
                    {
                        ImGui::TextDisabled("Default state");
                    }
                    else if (ImGui::Button("Set As Default"))
                    {
                        controller_.default_state = state_def.name;
                        controller_dirty_ = true;
                    }

                    ImGui::SameLine();
                    if (ImGui::Button(ICON_CI_TRASH " Remove"))
                    {
                        const std::string removed_name = state_def.name;
                        controller_.states.erase(controller_.states.begin() + static_cast<std::ptrdiff_t>(index));
                        if (controller_.default_state == removed_name)
                        {
                            controller_.default_state = controller_.states.empty() ? std::string() : controller_.states.front().name;
                        }
                        controller_dirty_ = true;
                        ImGui::TreePop();
                        ImGui::PopID();
                        break;
                    }

                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }

        if (ImGui::CollapsingHeader("Transitions", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::Button(ICON_CI_ADD " Add Transition"))
            {
                AnimatorTransitionDefinition transition;
                if (!controller_.states.empty())
                {
                    transition.from_state = controller_.states.front().name;
                    transition.to_state = controller_.states.front().name;
                }
                controller_.transitions.push_back(std::move(transition));
                controller_dirty_ = true;
            }

            std::vector<const char*> state_name_ptrs;
            state_name_ptrs.reserve(controller_.states.size());
            for (const AnimatorStateDefinition& state_def : controller_.states)
            {
                state_name_ptrs.push_back(state_def.name.c_str());
            }

            for (std::size_t index = 0; index < controller_.transitions.size(); ++index)
            {
                AnimatorTransitionDefinition& transition = controller_.transitions[index];
                ImGui::PushID(static_cast<int>(index));
                const std::string title = transition.from_state + " -> " + transition.to_state;
                if (ImGui::TreeNode(title.c_str()))
                {
                    int from_index = 0;
                    int to_index = 0;
                    for (int state_index = 0; state_index < static_cast<int>(controller_.states.size()); ++state_index)
                    {
                        if (controller_.states[state_index].name == transition.from_state)
                        {
                            from_index = state_index;
                        }
                        if (controller_.states[state_index].name == transition.to_state)
                        {
                            to_index = state_index;
                        }
                    }

                    if (!state_name_ptrs.empty() && ImGui::Combo("From", &from_index, state_name_ptrs.data(), static_cast<int>(state_name_ptrs.size())))
                    {
                        transition.from_state = controller_.states[static_cast<std::size_t>(from_index)].name;
                        controller_dirty_ = true;
                    }
                    if (!state_name_ptrs.empty() && ImGui::Combo("To", &to_index, state_name_ptrs.data(), static_cast<int>(state_name_ptrs.size())))
                    {
                        transition.to_state = controller_.states[static_cast<std::size_t>(to_index)].name;
                        controller_dirty_ = true;
                    }

                    if (InputTextString("Condition", transition.condition))
                    {
                        controller_dirty_ = true;
                    }

                    if (ImGui::DragFloat("Blend Duration", &transition.blend_duration, 0.01f, 0.0f, 10.0f, "%.2f"))
                    {
                        transition.blend_duration = (std::max)(0.0f, transition.blend_duration);
                        controller_dirty_ = true;
                    }
                    if (ImGui::Checkbox("Has Exit Time", &transition.has_exit_time))
                    {
                        controller_dirty_ = true;
                    }
                    if (transition.has_exit_time && ImGui::DragFloat("Exit Time", &transition.exit_time, 0.01f, 0.0f, 10.0f, "%.2f"))
                    {
                        transition.exit_time = (std::max)(0.0f, transition.exit_time);
                        controller_dirty_ = true;
                    }

                    if (ImGui::Button(ICON_CI_TRASH " Remove Transition"))
                    {
                        controller_.transitions.erase(controller_.transitions.begin() + static_cast<std::ptrdiff_t>(index));
                        controller_dirty_ = true;
                        ImGui::TreePop();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
        } // General tab body
        if (general_tab_open)
        {
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Face"))
        {
            RenderFaceTab(state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::BeginChild("BottomTablePanel", ImVec2(half_width, library_height), true);
    if (controller_loaded_)
    {
        ImGui::TextUnformatted("Bone Modifiers");
        ImGui::Separator();

        // "Add Selected Bone" picks up the joint currently highlighted in the
        // preview viewport.
        const std::string selected_bone = preview_renderer_.SelectedBoneName();
        bool already_listed = false;
        for (const AnimatorBoneModifier& existing : controller_.bone_modifiers)
        {
            if (!selected_bone.empty() && existing.bone_name == selected_bone)
            {
                already_listed = true;
                break;
            }
        }

        const bool can_add_selected = !selected_bone.empty() && !already_listed;
        if (!can_add_selected) { ImGui::BeginDisabled(); }
        if (ImGui::Button(ICON_CI_ADD " Add Selected Bone"))
        {
            AnimatorBoneModifier mod;
            mod.bone_name = selected_bone;
            controller_.bone_modifiers.push_back(std::move(mod));
            controller_dirty_ = true;
        }
        if (!can_add_selected) { ImGui::EndDisabled(); }
        if (!selected_bone.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", selected_bone.c_str());
        }
        else
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(click a bone in the preview)");
        }

        for (std::size_t index = 0; index < controller_.bone_modifiers.size(); ++index)
        {
            AnimatorBoneModifier& modifier = controller_.bone_modifiers[index];
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::TreeNode((modifier.bone_name.empty() ? std::string("Modifier") : modifier.bone_name).c_str()))
            {
                if (InputTextString("Bone", modifier.bone_name))
                {
                    controller_dirty_ = true;
                }

                // Modifier type. Entries must line up with the
                // AnimatorBoneModifierType enum order. Future kinds (IK,
                // look-at, ...) extend this list and branch below.
                static const char* type_names[] = {"Physics", "Collision"};
                int type_index = static_cast<int>(modifier.type);
                if (ImGui::Combo("Type", &type_index, type_names, IM_ARRAYSIZE(type_names)))
                {
                    const auto new_type = static_cast<AnimatorBoneModifierType>(type_index);
                    // When switching to Collision, seed the box to enclose the
                    // bone so the user starts from a sensible fitted volume.
                    if (new_type == AnimatorBoneModifierType::Collision
                        && modifier.type != AnimatorBoneModifierType::Collision)
                    {
                        preview_renderer_.ComputeBoneFitBox(
                            modifier.bone_name, modifier.box_half_extents, modifier.box_center);
                    }
                    modifier.type = new_type;
                    controller_dirty_ = true;
                }

                if (modifier.type == AnimatorBoneModifierType::Physics)
                {
                    if (DrawPhysicsModifierUI(modifier))
                    {
                        controller_dirty_ = true;
                    }
                }
                else if (modifier.type == AnimatorBoneModifierType::Collision)
                {
                    if (DrawCollisionModifierUI(modifier, preview_renderer_))
                    {
                        controller_dirty_ = true;
                    }
                }

                if (ImGui::Button(ICON_CI_TRASH " Remove Modifier"))
                {
                    controller_.bone_modifiers.erase(controller_.bone_modifiers.begin() + static_cast<std::ptrdiff_t>(index));
                    controller_dirty_ = true;
                    ImGui::TreePop();
                    ImGui::PopID();
                    break;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0f, spacing);

    ImGui::BeginChild("NodeLibraryPanel", ImVec2(0.0f, library_height), true);
    RenderNodeLibrary(state);
    ImGui::EndChild();
}

void AnimatorPanel::Shutdown()
{
    controller_loaded_ = false;
    controller_dirty_ = false;
    loaded_controller_path_.clear();
    controller_ = {};
    preview_renderer_.ClearModel();
    last_loaded_preview_path_.clear();
}

bool AnimatorPanel::ImportAnimationsFromModel(const std::filesystem::path& model_path, EngineState& state)
{
    if (!ModelMetadata::IsSupportedModelPath(model_path))
    {
        state.AddWarning("Animator import skipped: unsupported model file.");
        return false;
    }

    const ModelMetadata metadata = LoadModelMetadata(model_path);
    if (!metadata.parsed)
    {
        state.AddError("Animator import failed: " + metadata.error_message);
        return false;
    }

    if (metadata.animations.empty())
    {
        // Still bind this model as the preview/source so bone-physics on the
        // skeleton can run without any animation clips. Create an empty
        // default state if none exists so the runtime activates the animator.
        const std::string source_model_path_no_anim = NormalizeAssetPath(state, model_path);
        bool changed = false;
        // Bind this model as the preview when there's none yet, OR when the
        // current preview path no longer resolves to a file on disk (the model
        // was renamed/moved and re-imported). A still-valid preview is left
        // alone so importing from a separate file doesn't hijack it.
        const bool preview_missing_no_anim = controller_.preview_model_path.empty() ||
            !std::filesystem::exists(ResolveProjectPath(state, std::filesystem::path(controller_.preview_model_path)));
        if (preview_missing_no_anim)
        {
            controller_.preview_model_path = source_model_path_no_anim;
            SetPreviewModelPath(state, model_path);
            changed = true;
        }
        if (controller_.states.empty())
        {
            AnimatorStateDefinition default_state;
            default_state.name = "Default";
            default_state.clip_id.clear();
            default_state.playback_speed = 1.0f;
            default_state.loop = true;
            controller_.states.push_back(std::move(default_state));
            controller_.default_state = "Default";
            changed = true;
        }
        if (changed)
        {
            controller_dirty_ = true;
            state.AddWarning("Animator import: no animations found; bound model as preview/source for bone physics only.");
        }
        else
        {
            state.AddWarning("Animator import: no animations found in model.");
        }
        return changed;
    }

    std::unordered_set<std::string> used_clip_ids;
    for (const AnimatorClipReference& clip : controller_.clips)
    {
        if (!clip.id.empty())
        {
            used_clip_ids.insert(clip.id);
        }
    }

    std::unordered_set<std::string> used_state_names;
    for (const AnimatorStateDefinition& state_def : controller_.states)
    {
        if (!state_def.name.empty())
        {
            used_state_names.insert(state_def.name);
        }
    }

    const std::string source_model_path = NormalizeAssetPath(state, model_path);
    int imported_count = 0;
    for (std::size_t animation_index = 0; animation_index < metadata.animations.size(); ++animation_index)
    {
        const ModelAnimationMetadata& animation = metadata.animations[animation_index];
        const std::string base_name = animation.name.empty() ? ("Animation_" + std::to_string(animation_index + 1)) : animation.name;

        const std::string clip_id = MakeUniqueName(base_name, used_clip_ids);
        used_clip_ids.insert(clip_id);

        AnimatorClipReference clip;
        clip.id = clip_id;
        clip.source_model_path = source_model_path;
        clip.clip_name = animation.name.empty() ? clip_id : animation.name;
        controller_.clips.push_back(std::move(clip));

        AnimatorStateDefinition state_def;
        state_def.name = MakeUniqueName(base_name, used_state_names);
        used_state_names.insert(state_def.name);
        state_def.clip_id = clip_id;
        state_def.playback_speed = 1.0f;
        state_def.loop = true;
        controller_.states.push_back(std::move(state_def));

        ++imported_count;
    }

    if (controller_.default_state.empty() && !controller_.states.empty())
    {
        controller_.default_state = controller_.states.front().name;
    }

    // Re-point the preview when there's none yet, or when the stored one no
    // longer exists on disk (renamed/moved + re-imported). Valid previews are
    // preserved so importing clips from a separate anim file won't hijack them.
    const bool preview_missing = controller_.preview_model_path.empty() ||
        !std::filesystem::exists(ResolveProjectPath(state, std::filesystem::path(controller_.preview_model_path)));
    if (preview_missing && imported_count > 0)
    {
        controller_.preview_model_path = source_model_path;
        SetPreviewModelPath(state, model_path);
    }

    controller_dirty_ = imported_count > 0 || controller_dirty_;
    if (imported_count > 0)
    {
        state.AddLog("Imported " + std::to_string(imported_count) + " animation clips from " + state.GetDisplayPath(model_path));
    }
    return imported_count > 0;
}

void AnimatorPanel::RenderNodeLibrary(EngineState& state)
{
    ImGui::TextUnformatted("Imported Clips");
    ImGui::Separator();

    if (!controller_loaded_)
    {
        ImGui::TextDisabled("No controller selected.");
        return;
    }

    if (ImGui::Button(ICON_CI_ADD " Add Clip"))
    {
        AnimatorClipReference clip;
        clip.id = BuildDefaultClipId(controller_.clips.size());
        controller_.clips.push_back(std::move(clip));
        controller_dirty_ = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("Import Selected Model"))
    {
        const std::filesystem::path selected_path = ResolveProjectPath(state, state.selected_item_path);
        if (selected_path.empty())
        {
            state.AddWarning("Select a model file in the Files panel first.");
        }
        else
        {
            ImportAnimationsFromModel(selected_path, state);
        }
    }

    ImGui::Button("Drop Model (.fbx/.gltf/.glb) Here", ImVec2(-1.0f, 0.0f));
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
        {
            const char* payload_text = static_cast<const char*>(payload->Data);
            const std::size_t payload_size = payload->DataSize > 0
                ? static_cast<std::size_t>(payload->DataSize - 1)
                : 0;
            const std::filesystem::path dropped_path(std::string(payload_text, payload_size));
            if (HasAnyExtension(dropped_path, {".fbx", ".gltf", ".glb"}))
            {
                ImportAnimationsFromModel(ResolveProjectPath(state, dropped_path), state);
            }
            else
            {
                state.AddWarning("Dropped file is not a supported model for animation import.");
            }
        }
        ImGui::EndDragDropTarget();
    }

    for (std::size_t index = 0; index < controller_.clips.size(); ++index)
    {
        AnimatorClipReference& clip = controller_.clips[index];
        ImGui::PushID(static_cast<int>(index));
        const std::string title = clip.id.empty() ? std::string("Clip") : clip.id;
        if (ImGui::TreeNode(title.c_str()))
        {
            if (InputTextString("Clip Id", clip.id))
            {
                controller_dirty_ = true;
            }
            if (InputTextString("Source Model", clip.source_model_path))
            {
                controller_dirty_ = true;
            }
            if (InputTextString("Clip Name", clip.clip_name))
            {
                controller_dirty_ = true;
            }

            if (ImGui::Button(ICON_CI_TRASH " Remove Clip"))
            {
                controller_.clips.erase(controller_.clips.begin() + static_cast<std::ptrdiff_t>(index));
                controller_dirty_ = true;
                ImGui::TreePop();
                ImGui::PopID();
                break;
            }

            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void AnimatorPanel::SetPreviewModelPath(EngineState& state, const std::filesystem::path& absolute_path)
{
    const std::string normalized = NormalizeAssetPath(state, absolute_path);
    if (controller_.preview_model_path != normalized)
    {
        controller_.preview_model_path = normalized;
        controller_dirty_ = true;
    }
    if (preview_renderer_.SetModel(absolute_path))
    {
        last_loaded_preview_path_ = normalized;
    }
    else
    {
        last_loaded_preview_path_ = normalized; // still record to avoid retry-loops; user sees error in viewport
        state.AddError("Animator preview failed to load model: " + preview_renderer_.LastError());
    }
}

void AnimatorPanel::EnsurePreviewModelLoaded(EngineState& state)
{
    if (!controller_loaded_)
    {
        return;
    }

    const std::string& wanted = controller_.preview_model_path;
    if (wanted == last_loaded_preview_path_)
    {
        return;
    }

    if (wanted.empty())
    {
        preview_renderer_.ClearModel();
        last_loaded_preview_path_.clear();
        return;
    }

    const std::filesystem::path absolute = ResolveProjectPath(state, std::filesystem::path(wanted));
    if (preview_renderer_.SetModel(absolute))
    {
        last_loaded_preview_path_ = wanted;
    }
    else
    {
        last_loaded_preview_path_ = wanted; // record to avoid a per-frame retry loop
        state.AddError("Animator preview failed to load model. stored=\"" + wanted +
                     "\" resolved=\"" + absolute.string() +
                     "\" (" + preview_renderer_.LastError() + ")");
    }
}

void AnimatorPanel::RenderPreviewViewport(EngineState& state)
{
    const ImVec2 vp_min = ImGui::GetWindowPos();
    const ImVec2 vp_max = ImVec2(vp_min.x + ImGui::GetWindowSize().x, vp_min.y + ImGui::GetWindowSize().y);

    // Reserve a strip at the bottom for the playback toolbar (two rows:
    // controls + time scrubber).
    const float toolbar_height = ImGui::GetFrameHeight() * 2.0f + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    const ImVec2 canvas_min = vp_min;
    const ImVec2 canvas_max(vp_max.x, std::max(vp_min.y + 1.0f, vp_max.y - toolbar_height));

    // Drive playback before the draw so the first frame after Play shows motion.
    if (preview_renderer_.HasModel())
    {
        preview_renderer_.Tick(ImGui::GetIO().DeltaTime);
    }

    // Hand the current bone-physics list to the renderer so the spring
    // simulation reads the latest parameter values each frame.
    {
        std::vector<AnimatorPreviewRenderer::BonePhysicsParams> physics_list;
        physics_list.reserve(controller_.bone_modifiers.size());
        for (const AnimatorBoneModifier& mod : controller_.bone_modifiers)
        {
            if (mod.type != AnimatorBoneModifierType::Physics)
            {
                continue;
            }
            AnimatorPreviewRenderer::BonePhysicsParams p;
            p.bone_name = mod.bone_name;
            p.strength = mod.strength;
            p.stiffness = mod.stiffness;
            p.damping = mod.damping;
            p.mass = mod.mass;
            p.drag = mod.drag;
            p.gravity_scale = mod.gravity_scale;
            p.gravity_dir = mod.gravity_dir;
            p.angle_limit_deg = mod.angle_limit_deg;
            p.affects_children = mod.affects_children;
            physics_list.push_back(std::move(p));
        }
        preview_renderer_.SetBonePhysics(physics_list);
    }

    // Hand the collision boxes to the renderer so it can draw the wireframe
    // overlays that track each bone's animated pose.
    {
        std::vector<AnimatorPreviewRenderer::BoneCollisionParams> collision_list;
        collision_list.reserve(controller_.bone_modifiers.size());
        for (const AnimatorBoneModifier& mod : controller_.bone_modifiers)
        {
            if (mod.type != AnimatorBoneModifierType::Collision)
            {
                continue;
            }
            AnimatorPreviewRenderer::BoneCollisionParams c;
            c.bone_name = mod.bone_name;
            c.half_extents = mod.box_half_extents;
            c.center = mod.box_center;
            c.mode = static_cast<int>(mod.collision_mode);
            collision_list.push_back(std::move(c));
        }
        preview_renderer_.SetBoneCollisions(collision_list);
    }

    // Push the face expression pose currently selected for preview (or clear
    // it), with a playing lip-sync clip layered on top, so the viewport shows
    // the live blendshape result.
    {
        std::vector<std::pair<std::string, float>> face_weights;
        if (selected_face_pose_index_ >= 0 &&
            selected_face_pose_index_ < static_cast<int>(controller_.face.poses.size()))
        {
            face_weights = controller_.face.poses[static_cast<std::size_t>(selected_face_pose_index_)].weights;
        }

        const bool clip_index_valid =
            face_lipsync_clip_index_ >= 0 &&
            face_lipsync_clip_index_ < static_cast<int>(controller_.face.lip_sync_clips.size());
        if (face_lipsync_playing_ && clip_index_valid)
        {
            const std::string& clip_rel =
                controller_.face.lip_sync_clips[static_cast<std::size_t>(face_lipsync_clip_index_)];

            // Lazy-load the selected clip (and re-load when the selection changes).
            if (face_preview_clip_loaded_ != clip_rel)
            {
                const std::filesystem::path clip_path =
                    ResolveProjectPath(state, std::filesystem::path(clip_rel));
                std::string err;
                if (!LoadFaceClipAsset(clip_path, face_preview_clip_, err))
                {
                    face_preview_clip_ = {};
                }
                face_preview_clip_loaded_ = clip_rel; // avoid per-frame retry
            }

            if (face_preview_clip_.duration_seconds > 0.0f)
            {
                const float dt = (std::min)(ImGui::GetIO().DeltaTime, 0.1f);
                face_lipsync_time_seconds_ += dt;
                while (face_lipsync_time_seconds_ > face_preview_clip_.duration_seconds)
                {
                    face_lipsync_time_seconds_ -= face_preview_clip_.duration_seconds; // loop
                }

                std::vector<std::pair<std::string, float>> lip;
                face_preview_clip_.SampleInto(face_lipsync_time_seconds_, lip);
                // Lip-sync overrides the shapes it drives (jaw/mouth), layered
                // on top of the expression pose.
                for (const auto& lw : lip)
                {
                    bool found = false;
                    for (auto& w : face_weights)
                    {
                        if (w.first == lw.first) { w.second = lw.second; found = true; break; }
                    }
                    if (!found) { face_weights.push_back(lw); }
                }
            }
        }

        preview_renderer_.SetFaceWeights(face_weights);
    }

    preview_renderer_.Render(vulkan_context_, canvas_min, canvas_max);

    // Drop target covering the canvas area: the renderer used an InvisibleButton
    // on this same rect, so begin a drag/drop target on that last item.
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
        {
            const char* payload_text = static_cast<const char*>(payload->Data);
            const std::size_t payload_size = payload->DataSize > 0
                ? static_cast<std::size_t>(payload->DataSize - 1)
                : 0;
            const std::filesystem::path dropped_path(std::string(payload_text, payload_size));
            if (HasAnyExtension(dropped_path, {".fbx", ".gltf", ".glb"}))
            {
                SetPreviewModelPath(state, ResolveProjectPath(state, dropped_path));
            }
            else
            {
                state.AddWarning("Drop a .fbx / .gltf / .glb model into the preview viewport.");
            }
        }
        ImGui::EndDragDropTarget();
    }

    // ---- Toolbar (bottom strip) ----
    ImGui::SetCursorScreenPos(ImVec2(canvas_min.x + 6.0f, canvas_max.y + ImGui::GetStyle().ItemSpacing.y));

    const bool has_model = preview_renderer_.HasModel();
    if (!has_model)
    {
        ImGui::BeginDisabled();
    }

    bool playing = preview_renderer_.IsPlaying();
    if (ImGui::Button(playing ? ICON_CI_DEBUG_PAUSE : ICON_CI_DEBUG_START))
    {
        preview_renderer_.SetPlaying(!playing);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_CI_DEBUG_RESTART))
    {
        preview_renderer_.SetCurrentTime(0.0f);
    }
    ImGui::SameLine();

    // Clip selector.
    const std::vector<std::string>& clip_names = preview_renderer_.ClipNames();
    std::vector<const char*> name_ptrs;
    name_ptrs.reserve(clip_names.size());
    for (const std::string& name : clip_names) { name_ptrs.push_back(name.c_str()); }
    int active_index = 0;
    for (int i = 0; i < static_cast<int>(clip_names.size()); ++i)
    {
        if (clip_names[i] == preview_renderer_.ActiveClip()) { active_index = i; break; }
    }
    ImGui::SetNextItemWidth(160.0f);
    if (!name_ptrs.empty() && ImGui::Combo("##AnimatorPreviewClip", &active_index, name_ptrs.data(), static_cast<int>(name_ptrs.size())))
    {
        preview_renderer_.SetActiveClip(clip_names[static_cast<std::size_t>(active_index)]);
    }
    else if (name_ptrs.empty())
    {
        ImGui::TextDisabled("(no clips)");
    }

    ImGui::SameLine();
    bool show_skeleton = preview_renderer_.ShowSkeleton();
    if (ImGui::Checkbox("Skeleton", &show_skeleton))
    {
        preview_renderer_.SetShowSkeleton(show_skeleton);
    }
    ImGui::SameLine();
    bool show_solid = preview_renderer_.ShowMeshSolid();
    if (ImGui::Checkbox("Mesh", &show_solid))
    {
        preview_renderer_.SetShowMeshSolid(show_solid);
    }
    ImGui::SameLine();
    bool show_texture = preview_renderer_.ShowTexture();
    if (ImGui::Checkbox("Texture", &show_texture))
    {
        preview_renderer_.SetShowTexture(show_texture);
    }

    // Time scrubber on its own line so it gets the full panel width.
    const float duration = preview_renderer_.ClipDurationSeconds();
    if (duration > 0.0f)
    {
        ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - 8.0f));
        float t = preview_renderer_.CurrentTime();
        if (ImGui::SliderFloat("##AnimatorPreviewTime", &t, 0.0f, duration, "%.2fs"))
        {
            preview_renderer_.SetCurrentTime(t);
        }
    }

    if (!has_model)
    {
        ImGui::EndDisabled();
    }
}



