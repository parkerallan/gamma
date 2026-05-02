#include "components/SceneObjectAttributesEditor.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
constexpr SceneObjectAttributeKind kAttachableAttributeKinds[] = {
    SceneObjectAttributeKind::EnvironmentLight,
    SceneObjectAttributeKind::DirectionalLight,
    SceneObjectAttributeKind::PointLight,
    SceneObjectAttributeKind::SpotLight,
    SceneObjectAttributeKind::Camera,
    SceneObjectAttributeKind::Rigidbody,
    SceneObjectAttributeKind::TriggerVolume,
    SceneObjectAttributeKind::Text2D,
    SceneObjectAttributeKind::Image2D,
    SceneObjectAttributeKind::Skybox,
};

constexpr const char* kFileTreeDragDropPayload = "FILE_TREE_PATH";

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

bool RefreshOpenSceneBuffer(EngineState& state)
{
    if (!(state.HasOpenFile() && state.open_file_path == state.selected_item_path && !state.open_file_dirty))
    {
        return true;
    }

    std::ifstream input(state.selected_item_path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    state.saved_file_contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    state.open_file_contents = state.saved_file_contents;
    std::fill(state.editor_buffer.begin(), state.editor_buffer.end(), '\0');
    std::copy(state.open_file_contents.begin(), state.open_file_contents.end(), state.editor_buffer.begin());
    state.open_file_dirty = false;
    return true;
}

bool SaveSceneObjectAttributeEdit(
    EngineState& state,
    const SceneObjectMetadata& object,
    const std::string& label,
    const std::function<bool()>& edit_operation)
{
    if (state.HasOpenFile() && state.open_file_path == state.selected_item_path && state.open_file_dirty)
    {
        state.AddLog("Save the open scene before editing object attributes");
        return false;
    }

    if (!edit_operation())
    {
        return false;
    }

    RefreshOpenSceneBuffer(state);
    state.request_files_tree_refresh = true;
    state.AddLog("Updated object " + label + ": " + object.name);
    return true;
}

bool AddAttributeAttachment(EngineState& state, const SceneObjectMetadata& object, SceneObjectAttributeKind kind)
{
    return SaveSceneObjectAttributeEdit(state, object, "attributes", [&]() 
    {
        return AddSceneObjectAttribute(state.selected_item_path, object.name, kind);
    });
}

bool RemoveAttributeAttachment(EngineState& state, const SceneObjectMetadata& object, std::size_t attribute_index)
{
    return SaveSceneObjectAttributeEdit(state, object, "attributes", [&]()
    {
        return RemoveSceneObjectAttribute(state.selected_item_path, object.name, attribute_index);
    });
}

bool RenderAttributeKindSelector(EngineState& state, const SceneObjectMetadata& object, std::size_t attribute_index, SceneObjectAttributeKind current_kind)
{
    int current_index = 0;
    for (int index = 0; index < static_cast<int>(std::size(kAttachableAttributeKinds)); ++index)
    {
        if (kAttachableAttributeKinds[index] == current_kind)
        {
            current_index = index;
            break;
        }
    }

    bool changed = false;
    if (ImGui::BeginCombo("Type", ToDisplayName(current_kind)))
    {
        for (int index = 0; index < static_cast<int>(std::size(kAttachableAttributeKinds)); ++index)
        {
            const bool selected = index == current_index;
            if (ImGui::Selectable(ToDisplayName(kAttachableAttributeKinds[index]), selected))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "attributes", [&]()
                {
                    return SetSceneObjectAttributeKind(state.selected_item_path, object.name, attribute_index, kAttachableAttributeKinds[index]);
                });
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    return changed;
}

bool RenderAttributeAdder(EngineState& state, const SceneObjectMetadata& object)
{
    static int pending_attribute_kind_index = 0;

    bool changed = false;
    ImGui::PushID("AddAttribute");
    if (ImGui::BeginCombo("Add Attribute", ToDisplayName(kAttachableAttributeKinds[pending_attribute_kind_index])))
    {
        for (int index = 0; index < static_cast<int>(std::size(kAttachableAttributeKinds)); ++index)
        {
            const bool selected = index == pending_attribute_kind_index;
            if (ImGui::Selectable(ToDisplayName(kAttachableAttributeKinds[index]), selected))
            {
                pending_attribute_kind_index = index;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if (ImGui::Button("Attach"))
    {
        changed = AddAttributeAttachment(state, object, kAttachableAttributeKinds[pending_attribute_kind_index]);
    }
    ImGui::PopID();
    return changed;
}

bool RenderCommonLightControls(EngineState& state, const SceneObjectMetadata& object, std::size_t attribute_index, const SceneColor3& color, float intensity)
{
    bool changed = false;

    float color_values[3] = {color[0], color[1], color[2]};
    if (ImGui::ColorEdit3("Color", color_values))
    {
        changed = SaveSceneObjectAttributeEdit(state, object, "attribute color", [&]()
        {
            return SetSceneObjectAttributeColor(state.selected_item_path, object.name, attribute_index, {color_values[0], color_values[1], color_values[2]});
        }) || changed;
    }

    float intensity_value = intensity;
    if (ImGui::DragFloat("Intensity", &intensity_value, 0.05f, 0.0f, 1000.0f, "%.3f"))
    {
        changed = SaveSceneObjectAttributeEdit(state, object, "attribute intensity", [&]()
        {
            return SetSceneObjectAttributeIntensity(state.selected_item_path, object.name, attribute_index, intensity_value);
        }) || changed;
    }

    return changed;
}

bool RenderAttributeSection(
    EngineState& state,
    const SceneObjectMetadata& object,
    const SceneObjectAttribute& attribute,
    std::size_t attribute_index,
    const SceneObjectCameraPreviewCallback& render_camera_preview)
{
    ImGui::PushID(static_cast<int>(attribute_index));
    bool keep_attribute = true;
    if (ImGui::CollapsingHeader(ToDisplayName(attribute.kind), &keep_attribute, ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (attribute.kind != SceneObjectAttributeKind::Rigidbody &&
            attribute.kind != SceneObjectAttributeKind::TriggerVolume &&
            attribute.kind != SceneObjectAttributeKind::Text2D &&
            attribute.kind != SceneObjectAttributeKind::Image2D &&
            attribute.kind != SceneObjectAttributeKind::Skybox)
        {
            if (RenderAttributeKindSelector(state, object, attribute_index, attribute.kind))
            {
                ImGui::PopID();
                return true;
            }
        }

        bool changed = false;
        switch (attribute.kind)
        {
        case SceneObjectAttributeKind::EnvironmentLight:
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");
            changed = RenderCommonLightControls(state, object, attribute_index, attribute.environment_light.color, attribute.environment_light.intensity);
            break;

        case SceneObjectAttributeKind::DirectionalLight:
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");
            ImGui::TextDisabled("Direction uses the object rotation.");
            changed = RenderCommonLightControls(state, object, attribute_index, attribute.directional_light.color, attribute.directional_light.intensity);
            break;

        case SceneObjectAttributeKind::PointLight:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");
            ImGui::TextDisabled("Position uses the object transform. Light emits in all directions.");
            changed = RenderCommonLightControls(state, object, attribute_index, attribute.point_light.color, attribute.point_light.intensity);

            float range = attribute.point_light.range;
            if (ImGui::DragFloat("Range", &range, 0.1f, 0.01f, 1000.0f, "%.3f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "point light range", [&]()
                {
                    return SetSceneObjectAttributeRange(state.selected_item_path, object.name, attribute_index, range);
                }) || changed;
            }

            float source_radius = attribute.point_light.source_radius;
            if (ImGui::DragFloat("Radius", &source_radius, 0.01f, 0.01f, 100.0f, "%.3f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "point light radius", [&]()
                {
                    return SetSceneObjectAttributeSourceRadius(state.selected_item_path, object.name, attribute_index, source_radius);
                }) || changed;
            }

            float halo_intensity = attribute.point_light.halo_intensity;
            if (ImGui::DragFloat("Halo Intensity", &halo_intensity, 0.01f, 0.0f, 10.0f, "%.3f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "point light halo intensity", [&]()
                {
                    return SetSceneObjectAttributeHaloIntensity(state.selected_item_path, object.name, attribute_index, halo_intensity);
                }) || changed;
            }

            float halo_radius = attribute.point_light.halo_radius;
            if (ImGui::DragFloat("Halo Radius", &halo_radius, 0.01f, 0.01f, 20.0f, "%.3f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "point light halo radius", [&]()
                {
                    return SetSceneObjectAttributeHaloRadius(state.selected_item_path, object.name, attribute_index, halo_radius);
                }) || changed;
            }
            break;
        }

        case SceneObjectAttributeKind::SpotLight:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");
            ImGui::TextDisabled("Position and direction use the object transform.");
            changed = RenderCommonLightControls(state, object, attribute_index, attribute.spot_light.color, attribute.spot_light.intensity);

            float range = attribute.spot_light.range;
            if (ImGui::DragFloat("Range", &range, 0.1f, 0.01f, 1000.0f, "%.3f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "spot light range", [&]()
                {
                    return SetSceneObjectAttributeRange(state.selected_item_path, object.name, attribute_index, range);
                }) || changed;
            }

            float inner_cone = attribute.spot_light.inner_cone_degrees;
            if (ImGui::DragFloat("Inner Cone", &inner_cone, 0.25f, 0.1f, 89.0f, "%.3f deg"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "spot light inner cone", [&]()
                {
                    return SetSceneObjectAttributeInnerConeDegrees(state.selected_item_path, object.name, attribute_index, inner_cone);
                }) || changed;
            }

            float outer_cone = attribute.spot_light.outer_cone_degrees;
            if (ImGui::DragFloat("Outer Cone", &outer_cone, 0.25f, 0.1f, 89.0f, "%.3f deg"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "spot light outer cone", [&]()
                {
                    return SetSceneObjectAttributeOuterConeDegrees(state.selected_item_path, object.name, attribute_index, outer_cone);
                }) || changed;
            }
            break;
        }

        case SceneObjectAttributeKind::Camera:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            float field_of_view = attribute.camera.field_of_view_degrees;
            if (ImGui::DragFloat("Field Of View", &field_of_view, 0.25f, 1.0f, 179.0f, "%.3f deg"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "camera field of view", [&]()
                {
                    return SetSceneObjectAttributeFieldOfView(state.selected_item_path, object.name, attribute_index, field_of_view);
                }) || changed;
            }

            float near_clip = attribute.camera.near_clip;
            if (ImGui::DragFloat("Near Clip", &near_clip, 0.001f, 0.001f, 100.0f, "%.3f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "camera near clip", [&]()
                {
                    return SetSceneObjectAttributeNearClip(state.selected_item_path, object.name, attribute_index, near_clip);
                }) || changed;
            }

            float far_clip = attribute.camera.far_clip;
            if (ImGui::DragFloat("Far Clip", &far_clip, 0.1f, 0.1f, 10000.0f, "%.3f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "camera far clip", [&]()
                {
                    return SetSceneObjectAttributeFarClip(state.selected_item_path, object.name, attribute_index, far_clip);
                }) || changed;
            }

            bool active = attribute.camera.active;
            if (ImGui::Checkbox("Set Camera Active", &active))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "camera active", [&]()
                {
                    return SetSceneObjectCameraActive(state.selected_item_path, object.name, attribute_index, active);
                }) || changed;
            }

            if (render_camera_preview)
            {
                render_camera_preview(state, object, attribute_index, attribute);
            }
            break;
        }

        case SceneObjectAttributeKind::Rigidbody:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            const SceneObjectPhysicsShape current_shape = attribute.rigidbody.shape;
            const char* shape_names[] = {"None", "Box", "Sphere", "Capsule", "Mesh"};
            const SceneObjectPhysicsShape shape_values[] = {SceneObjectPhysicsShape::None, SceneObjectPhysicsShape::Box, SceneObjectPhysicsShape::Sphere, SceneObjectPhysicsShape::Capsule, SceneObjectPhysicsShape::Mesh};
            int current_shape_index = 0;
            for (int i = 0; i < 5; ++i)
            {
                if (shape_values[i] == current_shape)
                {
                    current_shape_index = i;
                    break;
                }
            }

            if (ImGui::BeginCombo("Shape", shape_names[current_shape_index]))
            {
                for (int i = 0; i < 5; ++i)
                {
                    const bool selected = i == current_shape_index;
                    if (ImGui::Selectable(shape_names[i], selected))
                    {
                        changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody shape", [&]()
                        {
                            return SetSceneObjectAttributePhysicsShape(state.selected_item_path, object.name, attribute_index, shape_values[i]);
                        }) || changed;

                        if (shape_values[i] == SceneObjectPhysicsShape::Mesh && attribute.rigidbody.is_dynamic)
                        {
                            changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody dynamic", [&]()
                            {
                                return SetSceneObjectAttributePhysicsDynamic(state.selected_item_path, object.name, attribute_index, false);
                            }) || changed;
                        }
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            if (current_shape != SceneObjectPhysicsShape::None)
            {
                const bool supports_dynamic = current_shape != SceneObjectPhysicsShape::Mesh;
                bool is_dynamic = supports_dynamic ? attribute.rigidbody.is_dynamic : false;
                ImGui::BeginDisabled(!supports_dynamic);
                if (ImGui::Checkbox("Dynamic", &is_dynamic) && supports_dynamic)
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody dynamic", [&]()
                    {
                        return SetSceneObjectAttributePhysicsDynamic(state.selected_item_path, object.name, attribute_index, is_dynamic);
                    }) || changed;
                }
                ImGui::EndDisabled();
                if (!supports_dynamic)
                {
                    ImGui::TextDisabled("Mesh colliders are static-only.");
                }

                bool lock_rotation_x = attribute.rigidbody.lock_rotation_x;
                if (ImGui::Checkbox("Lock Rotation X", &lock_rotation_x))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody rotation lock x", [&]()
                    {
                        return SetSceneObjectAttributePhysicsLockRotationX(state.selected_item_path, object.name, attribute_index, lock_rotation_x);
                    }) || changed;
                }

                bool lock_rotation_y = attribute.rigidbody.lock_rotation_y;
                if (ImGui::Checkbox("Lock Rotation Y", &lock_rotation_y))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody rotation lock y", [&]()
                    {
                        return SetSceneObjectAttributePhysicsLockRotationY(state.selected_item_path, object.name, attribute_index, lock_rotation_y);
                    }) || changed;
                }

                bool lock_rotation_z = attribute.rigidbody.lock_rotation_z;
                if (ImGui::Checkbox("Lock Rotation Z", &lock_rotation_z))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody rotation lock z", [&]()
                    {
                        return SetSceneObjectAttributePhysicsLockRotationZ(state.selected_item_path, object.name, attribute_index, lock_rotation_z);
                    }) || changed;
                }

                float mass = attribute.rigidbody.mass;
                if (ImGui::DragFloat("Mass", &mass, 0.01f, 0.001f, 10000.0f, "%.3f"))
                {
                    const float clamped = (std::max)(0.001f, mass);
                    changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody mass", [&]()
                    {
                        return SetSceneObjectAttributePhysicsMass(state.selected_item_path, object.name, attribute_index, clamped);
                    }) || changed;
                }

                float friction = attribute.rigidbody.friction;
                if (ImGui::DragFloat("Friction", &friction, 0.005f, 0.0f, 10.0f, "%.3f"))
                {
                    const float clamped = (std::max)(0.0f, friction);
                    changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody friction", [&]()
                    {
                        return SetSceneObjectAttributePhysicsFriction(state.selected_item_path, object.name, attribute_index, clamped);
                    }) || changed;
                }

                if (current_shape == SceneObjectPhysicsShape::Sphere || current_shape == SceneObjectPhysicsShape::Capsule)
                {
                    float radius = attribute.rigidbody.radius;
                    if (ImGui::DragFloat("Radius", &radius, 0.01f, 0.01f, 10000.0f, "%.3f"))
                    {
                        const float clamped = (std::max)(0.01f, radius);
                        changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody radius", [&]()
                        {
                            return SetSceneObjectAttributePhysicsRadius(state.selected_item_path, object.name, attribute_index, clamped);
                        }) || changed;
                    }

                    if (current_shape == SceneObjectPhysicsShape::Capsule)
                    {
                        float capsule_half_height = attribute.rigidbody.capsule_half_height;
                        if (ImGui::DragFloat("Capsule Half Height", &capsule_half_height, 0.01f, 0.0f, 10000.0f, "%.3f"))
                        {
                            const float clamped = (std::max)(0.0f, capsule_half_height);
                            changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody capsule half height", [&]()
                            {
                                return SetSceneObjectAttributePhysicsCapsuleHalfHeight(state.selected_item_path, object.name, attribute_index, clamped);
                            }) || changed;
                        }
                    }
                }
                else if (current_shape == SceneObjectPhysicsShape::Box)
                {
                    float half_extent[3] = {attribute.rigidbody.half_extent[0], attribute.rigidbody.half_extent[1], attribute.rigidbody.half_extent[2]};
                    if (ImGui::DragFloat3("Half Extent", half_extent, 0.01f, 0.01f, 10000.0f, "%.3f"))
                    {
                        const SceneVector3 clamped = {(std::max)(0.01f, half_extent[0]), (std::max)(0.01f, half_extent[1]), (std::max)(0.01f, half_extent[2])};
                        changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody half extent", [&]()
                        {
                            return SetSceneObjectAttributePhysicsHalfExtent(state.selected_item_path, object.name, attribute_index, clamped);
                        }) || changed;
                    }
                }

                float linear_damping = attribute.rigidbody.linear_damping;
                if (ImGui::DragFloat("Linear Damping", &linear_damping, 0.005f, 0.0f, 10.0f, "%.3f"))
                {
                    const float clamped = (std::max)(0.0f, linear_damping);
                    changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody linear damping", [&]()
                    {
                        return SetSceneObjectAttributePhysicsLinearDamping(state.selected_item_path, object.name, attribute_index, clamped);
                    }) || changed;
                }

                float angular_damping = attribute.rigidbody.angular_damping;
                if (ImGui::DragFloat("Angular Damping", &angular_damping, 0.005f, 0.0f, 10.0f, "%.3f"))
                {
                    const float clamped = (std::max)(0.0f, angular_damping);
                    changed = SaveSceneObjectAttributeEdit(state, object, "rigidbody angular damping", [&]()
                    {
                        return SetSceneObjectAttributePhysicsAngularDamping(state.selected_item_path, object.name, attribute_index, clamped);
                    }) || changed;
                }
            }
            break;
        }

        case SceneObjectAttributeKind::TriggerVolume:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");
            ImGui::TextDisabled("Trigger volumes detect overlap without physical collision response.");

            float half_extent[3] = {
                attribute.trigger_box.half_extent[0],
                attribute.trigger_box.half_extent[1],
                attribute.trigger_box.half_extent[2]};
            if (ImGui::DragFloat3("Half Extent", half_extent, 0.01f, 0.01f, 10000.0f, "%.3f"))
            {
                const SceneVector3 clamped = {
                    (std::max)(0.01f, half_extent[0]),
                    (std::max)(0.01f, half_extent[1]),
                    (std::max)(0.01f, half_extent[2])};
                changed = SaveSceneObjectAttributeEdit(state, object, "trigger half extent", [&]()
                {
                    return SetSceneObjectAttributeTriggerHalfExtent(state.selected_item_path, object.name, attribute_index, clamped);
                }) || changed;
            }

            break;
        }

        case SceneObjectAttributeKind::Text2D:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            const std::string current_font_label = attribute.text_2d.font_path.empty()
                ? std::string("Drop TTF Font Here")
                : std::filesystem::path(attribute.text_2d.font_path).filename().string();
            ImGui::Button(current_font_label.c_str(), ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
                {
                    const char* payload_text = static_cast<const char*>(payload->Data);
                    const std::size_t payload_size = payload->DataSize > 0
                        ? static_cast<std::size_t>(payload->DataSize - 1)
                        : 0;
                    const std::filesystem::path dropped_path(std::string(payload_text, payload_size));
                    if (HasAnyExtension(dropped_path, {".ttf", ".otf"}))
                    {
                        const std::string normalized = NormalizeAssetPath(state, dropped_path);
                        changed = SaveSceneObjectAttributeEdit(state, object, "text 2D font", [&]()
                        {
                            return SetSceneObjectAttributeText2DFontPath(state.selected_item_path, object.name, attribute_index, normalized);
                        }) || changed;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            std::array<char, 4096> text_buffer{};
            const std::string initial_text = attribute.text_2d.text;
            const std::size_t copy_size = (std::min)(initial_text.size(), text_buffer.size() - 1);
            std::copy_n(initial_text.data(), copy_size, text_buffer.data());
            if (ImGui::InputTextMultiline("Text", text_buffer.data(), text_buffer.size(), ImVec2(-1.0f, 120.0f)))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "text 2D text", [&]()
                {
                    return SetSceneObjectAttributeText2DText(state.selected_item_path, object.name, attribute_index, std::string(text_buffer.data()));
                }) || changed;
            }

            float position[2] = {attribute.text_2d.x, attribute.text_2d.y};
            if (ImGui::DragFloat2("Position", position, 1.0f, -100000.0f, 100000.0f, "%.1f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "text 2D position", [&]()
                {
                    return SetSceneObjectAttributeText2DPosition(state.selected_item_path, object.name, attribute_index, position[0], position[1]);
                }) || changed;
            }

            if (attribute.text_2d.lock_aspect_ratio)
            {
                const float safe_height = (std::max)(1.0f, attribute.text_2d.height);
                const float aspect = (std::max)(attribute.text_2d.width / safe_height, 0.0001f);
                float width_locked = attribute.text_2d.width;
                if (ImGui::DragFloat("Size", &width_locked, 1.0f, 1.0f, 100000.0f, "%.1f"))
                {
                    const float clamped_width = (std::max)(1.0f, width_locked);
                    const float clamped_height = (std::max)(1.0f, clamped_width / aspect);
                    changed = SaveSceneObjectAttributeEdit(state, object, "text 2D size", [&]()
                    {
                        return SetSceneObjectAttributeText2DSize(
                            state.selected_item_path,
                            object.name,
                            attribute_index,
                            clamped_width,
                            clamped_height);
                    }) || changed;
                }
            }
            else
            {
                float size[2] = {attribute.text_2d.width, attribute.text_2d.height};
                if (ImGui::DragFloat2("Size", size, 1.0f, 1.0f, 100000.0f, "%.1f"))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "text 2D size", [&]()
                    {
                        return SetSceneObjectAttributeText2DSize(
                            state.selected_item_path,
                            object.name,
                            attribute_index,
                            (std::max)(1.0f, size[0]),
                            (std::max)(1.0f, size[1]));
                    }) || changed;
                }
            }

            bool text_lock_aspect_ratio = attribute.text_2d.lock_aspect_ratio;
            if (ImGui::Checkbox("Lock Aspect Ratio", &text_lock_aspect_ratio))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "text 2D lock aspect ratio", [&]()
                {
                    return SetSceneObjectAttributeText2DLockAspectRatio(state.selected_item_path, object.name, attribute_index, text_lock_aspect_ratio);
                }) || changed;
            }

            float font_size = attribute.text_2d.font_size;
            if (ImGui::DragFloat("Font Size", &font_size, 0.5f, 1.0f, 512.0f, "%.1f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "text 2D font size", [&]()
                {
                    return SetSceneObjectAttributeText2DFontSize(state.selected_item_path, object.name, attribute_index, (std::max)(1.0f, font_size));
                }) || changed;
            }

            float color[3] = {
                attribute.text_2d.color[0],
                attribute.text_2d.color[1],
                attribute.text_2d.color[2]};
            if (ImGui::ColorEdit3("Color", color))
            {
                const SceneColor3 next_color = {color[0], color[1], color[2]};
                changed = SaveSceneObjectAttributeEdit(state, object, "text 2D color", [&]()
                {
                    return SetSceneObjectAttributeText2DColor(state.selected_item_path, object.name, attribute_index, next_color);
                }) || changed;
            }

            float alpha = attribute.text_2d.alpha;
            if (ImGui::DragFloat("Alpha", &alpha, 0.01f, 0.0f, 1.0f, "%.2f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "text 2D alpha", [&]()
                {
                    return SetSceneObjectAttributeText2DAlpha(state.selected_item_path, object.name, attribute_index, std::clamp(alpha, 0.0f, 1.0f));
                }) || changed;
            }

            break;
        }

        case SceneObjectAttributeKind::Image2D:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            const std::string current_image_label = attribute.image_2d.image_path.empty()
                ? std::string("Drop Image Here")
                : std::filesystem::path(attribute.image_2d.image_path).filename().string();
            ImGui::Button(current_image_label.c_str(), ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
                {
                    const char* payload_text = static_cast<const char*>(payload->Data);
                    const std::size_t payload_size = payload->DataSize > 0
                        ? static_cast<std::size_t>(payload->DataSize - 1)
                        : 0;
                    const std::filesystem::path dropped_path(std::string(payload_text, payload_size));
                    if (HasAnyExtension(dropped_path, {".png", ".jpg", ".jpeg", ".bmp", ".tga", ".gif", ".webp"}))
                    {
                        const std::string normalized = NormalizeAssetPath(state, dropped_path);
                        changed = SaveSceneObjectAttributeEdit(state, object, "image 2D path", [&]()
                        {
                            return SetSceneObjectAttributeImage2DImagePath(state.selected_item_path, object.name, attribute_index, normalized);
                        }) || changed;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            float position[2] = {attribute.image_2d.x, attribute.image_2d.y};
            if (ImGui::DragFloat2("Position", position, 1.0f, -100000.0f, 100000.0f, "%.1f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "image 2D position", [&]()
                {
                    return SetSceneObjectAttributeImage2DPosition(state.selected_item_path, object.name, attribute_index, position[0], position[1]);
                }) || changed;
            }

            if (attribute.image_2d.lock_aspect_ratio)
            {
                const float safe_height = (std::max)(1.0f, attribute.image_2d.height);
                const float aspect = (std::max)(attribute.image_2d.width / safe_height, 0.0001f);
                float width_locked = attribute.image_2d.width;
                if (ImGui::DragFloat("Size", &width_locked, 1.0f, 1.0f, 100000.0f, "%.1f"))
                {
                    const float clamped_width = (std::max)(1.0f, width_locked);
                    const float clamped_height = (std::max)(1.0f, clamped_width / aspect);
                    changed = SaveSceneObjectAttributeEdit(state, object, "image 2D size", [&]()
                    {
                        return SetSceneObjectAttributeImage2DSize(
                            state.selected_item_path,
                            object.name,
                            attribute_index,
                            clamped_width,
                            clamped_height);
                    }) || changed;
                }
            }
            else
            {
                float size[2] = {attribute.image_2d.width, attribute.image_2d.height};
                if (ImGui::DragFloat2("Size", size, 1.0f, 1.0f, 100000.0f, "%.1f"))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "image 2D size", [&]()
                    {
                        return SetSceneObjectAttributeImage2DSize(
                            state.selected_item_path,
                            object.name,
                            attribute_index,
                            (std::max)(1.0f, size[0]),
                            (std::max)(1.0f, size[1]));
                    }) || changed;
                }
            }

            bool image_lock_aspect_ratio = attribute.image_2d.lock_aspect_ratio;
            if (ImGui::Checkbox("Lock Aspect Ratio", &image_lock_aspect_ratio))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "image 2D lock aspect ratio", [&]()
                {
                    return SetSceneObjectAttributeImage2DLockAspectRatio(state.selected_item_path, object.name, attribute_index, image_lock_aspect_ratio);
                }) || changed;
            }

            float tint[3] = {
                attribute.image_2d.tint[0],
                attribute.image_2d.tint[1],
                attribute.image_2d.tint[2]};
            if (ImGui::ColorEdit3("Tint", tint))
            {
                const SceneColor3 next_tint = {tint[0], tint[1], tint[2]};
                changed = SaveSceneObjectAttributeEdit(state, object, "image 2D tint", [&]()
                {
                    return SetSceneObjectAttributeImage2DTint(state.selected_item_path, object.name, attribute_index, next_tint);
                }) || changed;
            }

            float alpha = attribute.image_2d.alpha;
            if (ImGui::DragFloat("Alpha", &alpha, 0.01f, 0.0f, 1.0f, "%.2f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "image 2D alpha", [&]()
                {
                    return SetSceneObjectAttributeImage2DAlpha(state.selected_item_path, object.name, attribute_index, std::clamp(alpha, 0.0f, 1.0f));
                }) || changed;
            }

            break;
        }

        case SceneObjectAttributeKind::Skybox:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            const std::string current_image_label = attribute.skybox.image_path.empty()
                ? std::string("Drop Skybox (.hdr/.exr)")
                : std::filesystem::path(attribute.skybox.image_path).filename().string();
            ImGui::Button(current_image_label.c_str(), ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
                {
                    const char* payload_text = static_cast<const char*>(payload->Data);
                    const std::size_t payload_size = payload->DataSize > 0
                        ? static_cast<std::size_t>(payload->DataSize - 1)
                        : 0;
                    const std::filesystem::path dropped_path(std::string(payload_text, payload_size));
                    if (HasAnyExtension(dropped_path, {".hdr", ".exr"}))
                    {
                        const std::string normalized = NormalizeAssetPath(state, dropped_path);
                        changed = SaveSceneObjectAttributeEdit(state, object, "skybox image path", [&]()
                        {
                            return SetSceneObjectAttributeSkyboxImagePath(state.selected_item_path, object.name, attribute_index, normalized);
                        }) || changed;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            break;
        }

        case SceneObjectAttributeKind::None:
        default:
            break;
        }

        if (changed)
        {
            ImGui::PopID();
            return true;
        }
    }

    if (!keep_attribute)
    {
        const bool changed = RemoveAttributeAttachment(state, object, attribute_index);
        ImGui::PopID();
        return changed;
    }

    ImGui::PopID();
    return false;
}
}

bool RenderSceneObjectAttributesEditor(
    EngineState& state,
    const SceneObjectMetadata& object,
    const SceneObjectCameraPreviewCallback& render_camera_preview)
{
    ImGui::Spacing();
    ImGui::SeparatorText("Attributes");

    if (RenderAttributeAdder(state, object))
    {
        return true;
    }

    for (std::size_t attribute_index = 0; attribute_index < object.attributes.size(); ++attribute_index)
    {
        if (RenderAttributeSection(state, object, object.attributes[attribute_index], attribute_index, render_camera_preview))
        {
            return true;
        }
    }

    return false;
}