#include "components/SceneObjectAttributesEditor.h"

#include "imgui.h"

#include <algorithm>
#include <functional>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
constexpr SceneObjectAttributeKind kAttachableAttributeKinds[] = {
    SceneObjectAttributeKind::EnvironmentLight,
    SceneObjectAttributeKind::DirectionalLight,
    SceneObjectAttributeKind::SpotLight,
    SceneObjectAttributeKind::Camera,
};

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

bool RenderAttributeSection(EngineState& state, const SceneObjectMetadata& object, const SceneObjectAttribute& attribute, std::size_t attribute_index)
{
    ImGui::PushID(static_cast<int>(attribute_index));
    bool keep_attribute = true;
    if (ImGui::CollapsingHeader(ToDisplayName(attribute.kind), &keep_attribute, ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (RenderAttributeKindSelector(state, object, attribute_index, attribute.kind))
        {
            ImGui::PopID();
            return true;
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

bool RenderSceneObjectAttributesEditor(EngineState& state, const SceneObjectMetadata& object)
{
    ImGui::Spacing();
    ImGui::SeparatorText("Attributes");

    if (RenderAttributeAdder(state, object))
    {
        return true;
    }

    for (std::size_t attribute_index = 0; attribute_index < object.attributes.size(); ++attribute_index)
    {
        if (RenderAttributeSection(state, object, object.attributes[attribute_index], attribute_index))
        {
            return true;
        }
    }

    return false;
}