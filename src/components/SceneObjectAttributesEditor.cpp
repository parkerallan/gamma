#include "components/SceneObjectAttributesEditor.h"

#include "imgui.h"

#include <stb_image.h>

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
    SceneObjectAttributeKind::Model,
    SceneObjectAttributeKind::Script,
    SceneObjectAttributeKind::Graph,
    SceneObjectAttributeKind::Shape3D,
    SceneObjectAttributeKind::EnvironmentLight,
    SceneObjectAttributeKind::DirectionalLight,
    SceneObjectAttributeKind::PointLight,
    SceneObjectAttributeKind::SpotLight,
    SceneObjectAttributeKind::Camera,
    SceneObjectAttributeKind::Rigidbody,
    SceneObjectAttributeKind::TriggerVolume,
    SceneObjectAttributeKind::Animator,
    SceneObjectAttributeKind::Text2D,
    SceneObjectAttributeKind::Image2D,
    SceneObjectAttributeKind::Color2D,
    SceneObjectAttributeKind::Video2D,
    SceneObjectAttributeKind::Skybox,
    SceneObjectAttributeKind::Audio,
    SceneObjectAttributeKind::Effects,
    SceneObjectAttributeKind::Shader,
};

constexpr const char* kFileTreeDragDropPayload = "FILE_TREE_PATH";

struct Shape3DOption
{
    const char* label;
    const char* file_name;
};

constexpr Shape3DOption kShape3DOptions[] = {
    {"Cube", "cube.glb"},
    {"Cylinder", "cylinder.glb"},
    {"Plane", "plane.glb"},
    {"Sphere", "sphere.glb"},
};

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

std::filesystem::path ResolveAssetReadPath(const EngineState& state, const std::filesystem::path& path)
{
    if (path.is_absolute())
    {
        return path;
    }
    if (!state.project_root.empty())
    {
        return state.project_root / path;
    }
    return path;
}

bool TryReadImageDimensions(const EngineState& state, const std::filesystem::path& path, float& out_width, float& out_height)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    const std::filesystem::path resolved_path = ResolveAssetReadPath(state, path);
    if (!stbi_info(resolved_path.string().c_str(), &width, &height, &channels))
    {
        return false;
    }
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    out_width = static_cast<float>(width);
    out_height = static_cast<float>(height);
    return true;
}

std::filesystem::path GetBuiltInShapePath(const char* file_name)
{
    return std::filesystem::path(__FILE__).parent_path().parent_path() / "shapes" / file_name;
}

int FindSelectedShape3DIndex(const SceneObjectMetadata& object)
{
    const std::string selected_file_name = ToLowerAscii(std::filesystem::path(object.model_path).filename().string());
    for (int index = 0; index < static_cast<int>(std::size(kShape3DOptions)); ++index)
    {
        if (selected_file_name == kShape3DOptions[index].file_name)
        {
            return index;
        }
    }

    return 0;
}

std::filesystem::path GetDragDroppedPath()
{
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload);
    if (payload == nullptr)
    {
        return {};
    }

    const char* payload_text = static_cast<const char*>(payload->Data);
    const std::size_t payload_size = payload->DataSize > 0
        ? static_cast<std::size_t>(payload->DataSize - 1)
        : 0;
    return std::filesystem::path(std::string(payload_text, payload_size));
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
        if (!AddSceneObjectAttribute(state.selected_item_path, object.name, kind))
        {
            return false;
        }

        const std::size_t new_attribute_index = object.attributes.size();

        if (kind == SceneObjectAttributeKind::Shape3D)
        {
            if (!SetSceneObjectModel(
                    state.selected_item_path,
                    object.name,
                    state.project_root,
                    GetBuiltInShapePath(kShape3DOptions[0].file_name)))
            {
                return false;
            }
            SetSceneObjectAttributeShape3DPath(
                state.selected_item_path,
                object.name,
                new_attribute_index,
                kShape3DOptions[0].file_name);
            return true;
        }

        if (kind == SceneObjectAttributeKind::Shader)
        {
            SetSceneObjectAttributeShaderType(
                state.selected_item_path,
                object.name,
                new_attribute_index,
                SceneObjectShaderType::Water);
            return true;
        }

        return true;
    });
}

bool RemoveAttributeAttachment(EngineState& state, const SceneObjectMetadata& object, std::size_t attribute_index)
{
    return SaveSceneObjectAttributeEdit(state, object, "attributes", [&]()
    {
        if (attribute_index >= object.attributes.size())
        {
            return false;
        }

        const SceneObjectAttributeKind removed_kind = object.attributes[attribute_index].kind;
        bool updated = RemoveSceneObjectAttribute(state.selected_item_path, object.name, attribute_index);
        if (!updated)
        {
            return false;
        }

        if (removed_kind == SceneObjectAttributeKind::Model)
        {
            updated = ClearSceneObjectModel(state.selected_item_path, object.name) && updated;
        }
        else if (removed_kind == SceneObjectAttributeKind::Shape3D)
        {
            updated = ClearSceneObjectModel(state.selected_item_path, object.name) && updated;
        }
        else if (removed_kind == SceneObjectAttributeKind::Script)
        {
            for (const std::string& script_path : object.script_paths)
            {
                updated = RemoveSceneObjectScript(state.selected_item_path, object.name, state.project_root, state.project_root / script_path) && updated;
            }
        }
        else if (removed_kind == SceneObjectAttributeKind::Graph)
        {
            for (const std::string& graph_path : object.graph_paths)
            {
                updated = RemoveSceneObjectGraph(state.selected_item_path, object.name, state.project_root, state.project_root / graph_path) && updated;
            }
        }

        return updated;
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
            attribute.kind != SceneObjectAttributeKind::Model &&
            attribute.kind != SceneObjectAttributeKind::Script &&
            attribute.kind != SceneObjectAttributeKind::Graph &&
            attribute.kind != SceneObjectAttributeKind::Shape3D &&
            attribute.kind != SceneObjectAttributeKind::Animator &&
            attribute.kind != SceneObjectAttributeKind::Text2D &&
            attribute.kind != SceneObjectAttributeKind::Image2D &&
            attribute.kind != SceneObjectAttributeKind::Color2D &&
            attribute.kind != SceneObjectAttributeKind::Video2D &&
            attribute.kind != SceneObjectAttributeKind::Skybox &&
            attribute.kind != SceneObjectAttributeKind::Effects &&
            attribute.kind != SceneObjectAttributeKind::Shader &&
            attribute.kind != SceneObjectAttributeKind::Camera)
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
        case SceneObjectAttributeKind::Model:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            const std::string current_model_label = object.model_path.empty()
                ? std::string("Drop Model Here")
                : std::filesystem::path(object.model_path).filename().string();
            ImGui::Button(current_model_label.c_str(), ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                const std::filesystem::path dropped_path = GetDragDroppedPath();
                if (!dropped_path.empty())
                {
                    if (HasAnyExtension(dropped_path, {".fbx", ".glb", ".gltf"}))
                    {
                        changed = SaveSceneObjectAttributeEdit(state, object, "model", [&]()
                        {
                            return SetSceneObjectModel(state.selected_item_path, object.name, state.project_root, dropped_path);
                        }) || changed;
                    }
                    else
                    {
                        state.AddLog("Drop a supported model: .fbx, .glb, or .gltf");
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (!object.model_path.empty())
            {
                ImGui::TextWrapped("Path: %s", object.model_path.c_str());

                float model_visual_offset[3] = {
                    object.model_visual_offset[0],
                    object.model_visual_offset[1],
                    object.model_visual_offset[2],
                };
                if (ImGui::DragFloat3("Visual Offset", model_visual_offset, 0.01f))
                {
                    const SceneVector3 updated_model_visual_offset = {
                        model_visual_offset[0],
                        model_visual_offset[1],
                        model_visual_offset[2]};
                    changed = SaveSceneObjectAttributeEdit(state, object, "model visual offset", [&]()
                    {
                        return SetSceneObjectModelVisualOffset(state.selected_item_path, object.name, updated_model_visual_offset);
                    }) || changed;
                }

                ImGui::TextDisabled("Applies to rendering only. Rigidbody collisions use object transform.");
                if (ImGui::Button("Remove Model"))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "model", [&]()
                    {
                        return ClearSceneObjectModel(state.selected_item_path, object.name);
                    }) || changed;
                }
            }
            break;
        }

        case SceneObjectAttributeKind::Script:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            ImGui::Button("Drop Lua Script Here", ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                const std::filesystem::path dropped_path = GetDragDroppedPath();
                if (!dropped_path.empty())
                {
                    if (HasAnyExtension(dropped_path, {".lua"}))
                    {
                        changed = SaveSceneObjectAttributeEdit(state, object, "script", [&]()
                        {
                            return AddSceneObjectScript(state.selected_item_path, object.name, state.project_root, dropped_path);
                        }) || changed;
                    }
                    else
                    {
                        state.AddLog("Drop a supported script: .lua");
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (!object.script_paths.empty())
            {
                ImGui::Text("Attached scripts: %zu", object.script_paths.size());
                for (std::size_t script_index = 0; script_index < object.script_paths.size(); ++script_index)
                {
                    const std::string& script_path = object.script_paths[script_index];
                    ImGui::PushID(static_cast<int>(script_index));
                    ImGui::TextWrapped("%s", script_path.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove"))
                    {
                        changed = SaveSceneObjectAttributeEdit(state, object, "script", [&]()
                        {
                            return RemoveSceneObjectScript(state.selected_item_path, object.name, state.project_root, state.project_root / script_path);
                        }) || changed;
                    }
                    ImGui::PopID();
                    if (changed)
                    {
                        break;
                    }
                }
            }
            break;
        }

        case SceneObjectAttributeKind::Graph:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            ImGui::Button("Drop Graph Here", ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                const std::filesystem::path dropped_path = GetDragDroppedPath();
                if (!dropped_path.empty())
                {
                    if (HasAnyExtension(dropped_path, {".graph"}))
                    {
                        changed = SaveSceneObjectAttributeEdit(state, object, "graph", [&]()
                        {
                            return AddSceneObjectGraph(state.selected_item_path, object.name, state.project_root, dropped_path);
                        }) || changed;
                    }
                    else
                    {
                        state.AddLog("Drop a supported graph: .graph");
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (!object.graph_paths.empty())
            {
                ImGui::Text("Attached graphs: %zu", object.graph_paths.size());
                for (std::size_t graph_index = 0; graph_index < object.graph_paths.size(); ++graph_index)
                {
                    const std::string& graph_path = object.graph_paths[graph_index];
                    ImGui::PushID(static_cast<int>(graph_index));
                    ImGui::TextWrapped("%s", graph_path.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove"))
                    {
                        changed = SaveSceneObjectAttributeEdit(state, object, "graph", [&]()
                        {
                            return RemoveSceneObjectGraph(state.selected_item_path, object.name, state.project_root, state.project_root / graph_path);
                        }) || changed;
                    }
                    ImGui::PopID();
                    if (changed)
                    {
                        break;
                    }
                }
            }
            break;
        }

        case SceneObjectAttributeKind::Shape3D:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            int selected_shape_index = FindSelectedShape3DIndex(object);
            if (ImGui::BeginCombo("Shape", kShape3DOptions[selected_shape_index].label))
            {
                for (int shape_index = 0; shape_index < static_cast<int>(std::size(kShape3DOptions)); ++shape_index)
                {
                    const bool selected = shape_index == selected_shape_index;
                    if (ImGui::Selectable(kShape3DOptions[shape_index].label, selected))
                    {
                        selected_shape_index = shape_index;
                        changed = SaveSceneObjectAttributeEdit(state, object, "3D shape", [&]()
                        {
                            if (!SetSceneObjectModel(
                                    state.selected_item_path,
                                    object.name,
                                    state.project_root,
                                    GetBuiltInShapePath(kShape3DOptions[shape_index].file_name)))
                            {
                                return false;
                            }
                            SetSceneObjectAttributeShape3DPath(
                                state.selected_item_path,
                                object.name,
                                attribute_index,
                                kShape3DOptions[shape_index].file_name);
                            return true;
                        }) || changed;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            if (object.model_path.empty())
            {
                if (ImGui::Button("Add Shape", ImVec2(-1.0f, 0.0f)))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "3D shape", [&]()
                    {
                        if (!SetSceneObjectModel(
                                state.selected_item_path,
                                object.name,
                                state.project_root,
                                GetBuiltInShapePath(kShape3DOptions[selected_shape_index].file_name)))
                        {
                            return false;
                        }
                        SetSceneObjectAttributeShape3DPath(
                            state.selected_item_path,
                            object.name,
                            attribute_index,
                            kShape3DOptions[selected_shape_index].file_name);
                        return true;
                    }) || changed;
                }
            }
            else
            {
                float model_visual_offset[3] = {
                    object.model_visual_offset[0],
                    object.model_visual_offset[1],
                    object.model_visual_offset[2],
                };
                if (ImGui::DragFloat3("Visual Offset", model_visual_offset, 0.01f))
                {
                    const SceneVector3 updated_model_visual_offset = {
                        model_visual_offset[0],
                        model_visual_offset[1],
                        model_visual_offset[2]};
                    changed = SaveSceneObjectAttributeEdit(state, object, "shape visual offset", [&]()
                    {
                        return SetSceneObjectModelVisualOffset(state.selected_item_path, object.name, updated_model_visual_offset);
                    }) || changed;
                }

                if (ImGui::Button("Remove Shape"))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "3D shape", [&]()
                    {
                        return ClearSceneObjectModel(state.selected_item_path, object.name);
                    }) || changed;
                }
            }
            break;
        }

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

            struct CameraTypeOption
            {
                const char* label;
                SceneObjectCameraType type;
            };
            constexpr CameraTypeOption kCameraTypeOptions[] = {
                {"Fixed", SceneObjectCameraType::Fixed},
                {"Follow", SceneObjectCameraType::Follow},
            };

            int selected_camera_type_index = 0;
            for (int i = 0; i < static_cast<int>(std::size(kCameraTypeOptions)); ++i)
            {
                if (kCameraTypeOptions[i].type == attribute.camera.type)
                {
                    selected_camera_type_index = i;
                    break;
                }
            }

            if (ImGui::BeginCombo("Type", kCameraTypeOptions[selected_camera_type_index].label))
            {
                for (int i = 0; i < static_cast<int>(std::size(kCameraTypeOptions)); ++i)
                {
                    const bool selected = i == selected_camera_type_index;
                    if (ImGui::Selectable(kCameraTypeOptions[i].label, selected))
                    {
                        changed = SaveSceneObjectAttributeEdit(state, object, "camera type", [&]()
                        {
                            return SetSceneObjectAttributeCameraType(state.selected_item_path, object.name, attribute_index, kCameraTypeOptions[i].type);
                        }) || changed;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            if (attribute.camera.type == SceneObjectCameraType::Follow)
            {
                const std::string follow_target_label = attribute.camera.follow_target_object.empty()
                    ? std::string("Drop Follow Target Object Here")
                    : attribute.camera.follow_target_object;
                ImGui::Button(follow_target_label.c_str(), ImVec2(-1.0f, 0.0f));
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_OBJECT_PATH"))
                    {
                        const char* payload_text = static_cast<const char*>(payload->Data);
                        const std::size_t payload_size = payload->DataSize > 0
                            ? static_cast<std::size_t>(payload->DataSize - 1)
                            : 0;
                        const std::string payload_string(payload_text, payload_size);
                        const std::size_t separator_index = payload_string.find('\n');
                        std::string dropped_object_name;
                        std::filesystem::path dropped_scene_path;
                        if (separator_index != std::string::npos)
                        {
                            dropped_scene_path = std::filesystem::path(payload_string.substr(0, separator_index));
                            dropped_object_name = payload_string.substr(separator_index + 1);
                        }

                        const bool same_scene = !dropped_scene_path.empty() &&
                            std::filesystem::weakly_canonical(dropped_scene_path) ==
                            std::filesystem::weakly_canonical(state.selected_item_path);
                        const bool different_object = dropped_object_name != object.name;
                        if (same_scene && different_object && !dropped_object_name.empty())
                        {
                            changed = SaveSceneObjectAttributeEdit(state, object, "camera follow target", [&]()
                            {
                                return SetSceneObjectAttributeCameraFollowTarget(state.selected_item_path, object.name, attribute_index, dropped_object_name);
                            }) || changed;
                        }
                        else if (!same_scene)
                        {
                            state.AddLog("Follow target must be from the same scene");
                        }
                        else if (!different_object)
                        {
                            state.AddLog("A camera cannot follow itself");
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                if (!attribute.camera.follow_target_object.empty())
                {
                    if (ImGui::SmallButton("Clear Follow Target"))
                    {
                        changed = SaveSceneObjectAttributeEdit(state, object, "camera follow target", [&]()
                        {
                            return SetSceneObjectAttributeCameraFollowTarget(state.selected_item_path, object.name, attribute_index, std::string());
                        }) || changed;
                    }
                }

                bool follow_lock_position = attribute.camera.follow_lock_position;
                if (ImGui::Checkbox("Lock Position", &follow_lock_position))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "camera follow lock position", [&]()
                    {
                        return SetSceneObjectAttributeCameraFollowLockPosition(state.selected_item_path, object.name, attribute_index, follow_lock_position);
                    }) || changed;
                }

                float follow_offset[3] = {
                    attribute.camera.follow_offset[0],
                    attribute.camera.follow_offset[1],
                    attribute.camera.follow_offset[2],
                };
                if (ImGui::DragFloat3("Follow Offset", follow_offset, 0.05f))
                {
                    const SceneVector3 next_offset = {follow_offset[0], follow_offset[1], follow_offset[2]};
                    changed = SaveSceneObjectAttributeEdit(state, object, "camera follow offset", [&]()
                    {
                        return SetSceneObjectAttributeCameraFollowOffset(state.selected_item_path, object.name, attribute_index, next_offset);
                    }) || changed;
                }

                ImGui::BeginDisabled(follow_lock_position);
                float follow_orbit[2] = {
                    attribute.camera.follow_orbit[0],
                    attribute.camera.follow_orbit[1],
                };
                if (ImGui::DragFloat2("Follow Orbit (Yaw,Pitch)", follow_orbit, 0.5f, -360.0f, 360.0f, "%.2f deg"))
                {
                    const SceneVector3 next_orbit = {follow_orbit[0], follow_orbit[1], 0.0f};
                    changed = SaveSceneObjectAttributeEdit(state, object, "camera follow orbit", [&]()
                    {
                        return SetSceneObjectAttributeCameraFollowOrbit(state.selected_item_path, object.name, attribute_index, next_orbit);
                    }) || changed;
                }
                ImGui::EndDisabled();

                float follow_rotation_offset[3] = {
                    attribute.camera.follow_rotation_offset[0],
                    attribute.camera.follow_rotation_offset[1],
                    attribute.camera.follow_rotation_offset[2],
                };
                if (ImGui::DragFloat3("Follow Rotation Offset", follow_rotation_offset, 0.5f, -360.0f, 360.0f, "%.2f deg"))
                {
                    const SceneVector3 next_rot = {follow_rotation_offset[0], follow_rotation_offset[1], follow_rotation_offset[2]};
                    changed = SaveSceneObjectAttributeEdit(state, object, "camera follow rotation offset", [&]()
                    {
                        return SetSceneObjectAttributeCameraFollowRotationOffset(state.selected_item_path, object.name, attribute_index, next_rot);
                    }) || changed;
                }

                float follow_smoothing = attribute.camera.follow_smoothing;
                if (ImGui::DragFloat("Follow Smoothing", &follow_smoothing, 0.005f, 0.0f, 2.0f, "%.3f s"))
                {
                    const float clamped = (std::max)(0.0f, follow_smoothing);
                    changed = SaveSceneObjectAttributeEdit(state, object, "camera follow smoothing", [&]()
                    {
                        return SetSceneObjectAttributeCameraFollowSmoothing(state.selected_item_path, object.name, attribute_index, clamped);
                    }) || changed;
                }

                ImGui::TextDisabled("Camera world position = follow target world position + Follow Offset. Camera keeps its own rotation. Follow Smoothing is the lag time constant (0 = snap).");
            }

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

        case SceneObjectAttributeKind::Animator:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            const std::string current_controller_label = attribute.animator.controller_path.empty()
                ? std::string("Drop Animator Controller (.anim)")
                : std::filesystem::path(attribute.animator.controller_path).filename().string();
            ImGui::Button(current_controller_label.c_str(), ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
                {
                    const char* payload_text = static_cast<const char*>(payload->Data);
                    const std::size_t payload_size = payload->DataSize > 0
                        ? static_cast<std::size_t>(payload->DataSize - 1)
                        : 0;
                    const std::filesystem::path dropped_path(std::string(payload_text, payload_size));
                    if (HasAnyExtension(dropped_path, {".anim"}))
                    {
                        const std::string normalized = NormalizeAssetPath(state, dropped_path);
                        changed = SaveSceneObjectAttributeEdit(state, object, "animator controller", [&]()
                        {
                            return SetSceneObjectAttributeAnimatorControllerPath(state.selected_item_path, object.name, attribute_index, normalized);
                        }) || changed;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            std::array<char, 256> initial_state_buffer{};
            const std::size_t copy_size = (std::min)(attribute.animator.initial_state.size(), initial_state_buffer.size() - 1);
            std::copy_n(attribute.animator.initial_state.data(), copy_size, initial_state_buffer.data());
            if (ImGui::InputText("Initial State", initial_state_buffer.data(), initial_state_buffer.size()))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "animator initial state", [&]()
                {
                    return SetSceneObjectAttributeAnimatorInitialState(state.selected_item_path, object.name, attribute_index, std::string(initial_state_buffer.data()));
                }) || changed;
            }

            float playback_speed = attribute.animator.playback_speed;
            if (ImGui::DragFloat("Playback Speed", &playback_speed, 0.01f, 0.0f, 5.0f, "%.2f"))
            {
                const float clamped = (std::max)(0.0f, playback_speed);
                changed = SaveSceneObjectAttributeEdit(state, object, "animator playback speed", [&]()
                {
                    return SetSceneObjectAttributeAnimatorPlaybackSpeed(state.selected_item_path, object.name, attribute_index, clamped);
                }) || changed;
            }

            bool auto_play = attribute.animator.auto_play;
            if (ImGui::Checkbox("Auto Play", &auto_play))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "animator auto play", [&]()
                {
                    return SetSceneObjectAttributeAnimatorAutoPlay(state.selected_item_path, object.name, attribute_index, auto_play);
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

            int priority_text = attribute.text_2d.priority;
            if (ImGui::DragInt("Priority", &priority_text))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "text 2D priority", [&]()
                {
                    return SetSceneObjectAttributeText2DPriority(state.selected_item_path, object.name, attribute_index, priority_text);
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
                        float natural_width = 0.0f;
                        float natural_height = 0.0f;
                        const bool has_natural_size = TryReadImageDimensions(state, dropped_path, natural_width, natural_height);
                        changed = SaveSceneObjectAttributeEdit(state, object, "image 2D path", [&]()
                        {
                            bool updated = SetSceneObjectAttributeImage2DImagePath(state.selected_item_path, object.name, attribute_index, normalized);
                            if (has_natural_size)
                            {
                                updated = SetSceneObjectAttributeImage2DSize(
                                    state.selected_item_path,
                                    object.name,
                                    attribute_index,
                                    natural_width,
                                    natural_height) && updated;
                            }
                            return updated;
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

            bool image_stretch = attribute.image_2d.stretch_to_screen;
            if (ImGui::Checkbox("Stretch to Screen", &image_stretch))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "image 2D stretch to screen", [&]()
                {
                    return SetSceneObjectAttributeImage2DStretchToScreen(state.selected_item_path, object.name, attribute_index, image_stretch);
                }) || changed;
            }

            if (HasAnyExtension(std::filesystem::path(attribute.image_2d.image_path), {".gif"}))
            {
                ImGui::TextUnformatted("Play Mode");
                const SceneObjectImagePlayMode image_mode = attribute.image_2d.play_mode;
                auto image_play_mode_radio = [&](const char* label, SceneObjectImagePlayMode mode)
                {
                    if (ImGui::RadioButton(label, image_mode == mode))
                    {
                        changed = SaveSceneObjectAttributeEdit(state, object, "image 2D play mode", [&]()
                        {
                            return SetSceneObjectAttributeImage2DPlayMode(state.selected_item_path, object.name, attribute_index, mode);
                        }) || changed;
                    }
                };
                image_play_mode_radio("Loop", SceneObjectImagePlayMode::Loop);
                ImGui::SameLine();
                image_play_mode_radio("Play Once", SceneObjectImagePlayMode::PlayOnce);
                ImGui::SameLine();
                image_play_mode_radio("Off", SceneObjectImagePlayMode::Off);
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

            int priority_image = attribute.image_2d.priority;
            if (ImGui::DragInt("Priority", &priority_image))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "image 2D priority", [&]()
                {
                    return SetSceneObjectAttributeImage2DPriority(state.selected_item_path, object.name, attribute_index, priority_image);
                }) || changed;
            }

            break;
        }

        case SceneObjectAttributeKind::Color2D:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            float position[2] = {attribute.color_2d.x, attribute.color_2d.y};
            if (ImGui::DragFloat2("Position", position, 1.0f, -100000.0f, 100000.0f, "%.1f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "2D color position", [&]()
                {
                    return SetSceneObjectAttributeColor2DPosition(state.selected_item_path, object.name, attribute_index, position[0], position[1]);
                }) || changed;
            }

            if (attribute.color_2d.lock_aspect_ratio)
            {
                const float safe_height = (std::max)(1.0f, attribute.color_2d.height);
                const float aspect = (std::max)(attribute.color_2d.width / safe_height, 0.0001f);
                float width_locked = attribute.color_2d.width;
                if (ImGui::DragFloat("Size", &width_locked, 1.0f, 1.0f, 100000.0f, "%.1f"))
                {
                    const float clamped_width = (std::max)(1.0f, width_locked);
                    const float clamped_height = (std::max)(1.0f, clamped_width / aspect);
                    changed = SaveSceneObjectAttributeEdit(state, object, "2D color size", [&]()
                    {
                        return SetSceneObjectAttributeColor2DSize(
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
                float size[2] = {attribute.color_2d.width, attribute.color_2d.height};
                if (ImGui::DragFloat2("Size", size, 1.0f, 1.0f, 100000.0f, "%.1f"))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "2D color size", [&]()
                    {
                        return SetSceneObjectAttributeColor2DSize(
                            state.selected_item_path,
                            object.name,
                            attribute_index,
                            (std::max)(1.0f, size[0]),
                            (std::max)(1.0f, size[1]));
                    }) || changed;
                }
            }

            float color[3] = {
                attribute.color_2d.color[0],
                attribute.color_2d.color[1],
                attribute.color_2d.color[2]};
            if (ImGui::ColorEdit3("Color", color))
            {
                const SceneColor3 next_color = {color[0], color[1], color[2]};
                changed = SaveSceneObjectAttributeEdit(state, object, "2D color", [&]()
                {
                    return SetSceneObjectAttributeColor2DColor(state.selected_item_path, object.name, attribute_index, next_color);
                }) || changed;
            }

            float alpha = attribute.color_2d.alpha;
            if (ImGui::DragFloat("Alpha", &alpha, 0.01f, 0.0f, 1.0f, "%.2f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "2D color alpha", [&]()
                {
                    return SetSceneObjectAttributeColor2DAlpha(state.selected_item_path, object.name, attribute_index, std::clamp(alpha, 0.0f, 1.0f));
                }) || changed;
            }

            bool lock_aspect_ratio = attribute.color_2d.lock_aspect_ratio;
            if (ImGui::Checkbox("Lock Aspect Ratio", &lock_aspect_ratio))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "2D color lock aspect ratio", [&]()
                {
                    return SetSceneObjectAttributeColor2DLockAspectRatio(state.selected_item_path, object.name, attribute_index, lock_aspect_ratio);
                }) || changed;
            }

            bool color_stretch = attribute.color_2d.stretch_to_screen;
            if (ImGui::Checkbox("Stretch to Screen", &color_stretch))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "2D color stretch to screen", [&]()
                {
                    return SetSceneObjectAttributeColor2DStretchToScreen(state.selected_item_path, object.name, attribute_index, color_stretch);
                }) || changed;
            }

            int priority_color = attribute.color_2d.priority;
            if (ImGui::DragInt("Priority", &priority_color))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "2D color priority", [&]()
                {
                    return SetSceneObjectAttributeColor2DPriority(state.selected_item_path, object.name, attribute_index, priority_color);
                }) || changed;
            }

            break;
        }

        case SceneObjectAttributeKind::Video2D:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            const std::string current_video_label = attribute.video_2d.video_path.empty()
                ? std::string("Drop Video Here")
                : std::filesystem::path(attribute.video_2d.video_path).filename().string();
            ImGui::Button(current_video_label.c_str(), ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
                {
                    const char* payload_text = static_cast<const char*>(payload->Data);
                    const std::size_t payload_size = payload->DataSize > 0
                        ? static_cast<std::size_t>(payload->DataSize - 1)
                        : 0;
                    const std::filesystem::path dropped_path(std::string(payload_text, payload_size));
                    if (HasAnyExtension(dropped_path, {".mp4", ".mov", ".mkv", ".webm", ".avi", ".mpg", ".mpeg", ".m4v"}))
                    {
                        const std::string normalized = NormalizeAssetPath(state, dropped_path);
                        changed = SaveSceneObjectAttributeEdit(state, object, "video 2D path", [&]()
                        {
                            return SetSceneObjectAttributeVideo2DVideoPath(state.selected_item_path, object.name, attribute_index, normalized);
                        }) || changed;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            float v_position[2] = {attribute.video_2d.x, attribute.video_2d.y};
            if (ImGui::DragFloat2("Position", v_position, 1.0f, -100000.0f, 100000.0f, "%.1f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "video 2D position", [&]()
                {
                    return SetSceneObjectAttributeVideo2DPosition(state.selected_item_path, object.name, attribute_index, v_position[0], v_position[1]);
                }) || changed;
            }

            if (attribute.video_2d.lock_aspect_ratio)
            {
                const float safe_height = (std::max)(1.0f, attribute.video_2d.height);
                const float aspect = (std::max)(attribute.video_2d.width / safe_height, 0.0001f);
                float width_locked = attribute.video_2d.width;
                if (ImGui::DragFloat("Width", &width_locked, 1.0f, 1.0f, 100000.0f, "%.1f"))
                {
                    const float clamped_width = (std::max)(1.0f, width_locked);
                    const float clamped_height = (std::max)(1.0f, clamped_width / aspect);
                    changed = SaveSceneObjectAttributeEdit(state, object, "video 2D size", [&]()
                    {
                        return SetSceneObjectAttributeVideo2DSize(
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
                float v_size[2] = {attribute.video_2d.width, attribute.video_2d.height};
                if (ImGui::DragFloat2("Size", v_size, 1.0f, 1.0f, 100000.0f, "%.1f"))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "video 2D size", [&]()
                    {
                        return SetSceneObjectAttributeVideo2DSize(
                            state.selected_item_path,
                            object.name,
                            attribute_index,
                            (std::max)(1.0f, v_size[0]),
                            (std::max)(1.0f, v_size[1]));
                    }) || changed;
                }
            }

            bool video_lock_aspect_ratio = attribute.video_2d.lock_aspect_ratio;
            if (ImGui::Checkbox("Lock Aspect Ratio", &video_lock_aspect_ratio))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "video 2D lock aspect ratio", [&]()
                {
                    return SetSceneObjectAttributeVideo2DLockAspectRatio(state.selected_item_path, object.name, attribute_index, video_lock_aspect_ratio);
                }) || changed;
            }

            bool video_stretch = attribute.video_2d.stretch_to_screen;
            if (ImGui::Checkbox("Stretch to Screen", &video_stretch))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "video 2D stretch to screen", [&]()
                {
                    return SetSceneObjectAttributeVideo2DStretchToScreen(state.selected_item_path, object.name, attribute_index, video_stretch);
                }) || changed;
            }

            float v_tint[3] = {
                attribute.video_2d.tint[0],
                attribute.video_2d.tint[1],
                attribute.video_2d.tint[2]};
            if (ImGui::ColorEdit3("Tint", v_tint))
            {
                const SceneColor3 next_tint = {v_tint[0], v_tint[1], v_tint[2]};
                changed = SaveSceneObjectAttributeEdit(state, object, "video 2D tint", [&]()
                {
                    return SetSceneObjectAttributeVideo2DTint(state.selected_item_path, object.name, attribute_index, next_tint);
                }) || changed;
            }

            float v_alpha = attribute.video_2d.alpha;
            if (ImGui::DragFloat("Alpha", &v_alpha, 0.01f, 0.0f, 1.0f, "%.2f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "video 2D alpha", [&]()
                {
                    return SetSceneObjectAttributeVideo2DAlpha(state.selected_item_path, object.name, attribute_index, std::clamp(v_alpha, 0.0f, 1.0f));
                }) || changed;
            }

            int v_priority = attribute.video_2d.priority;
            if (ImGui::DragInt("Priority", &v_priority))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "video 2D priority", [&]()
                {
                    return SetSceneObjectAttributeVideo2DPriority(state.selected_item_path, object.name, attribute_index, v_priority);
                }) || changed;
            }

            ImGui::TextUnformatted("Play Mode");
            const SceneObjectVideoPlayMode v_mode = attribute.video_2d.play_mode;
            auto video_play_mode_radio = [&](const char* label, SceneObjectVideoPlayMode mode)
            {
                if (ImGui::RadioButton(label, v_mode == mode))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "video 2D play mode", [&]()
                    {
                        return SetSceneObjectAttributeVideo2DPlayMode(state.selected_item_path, object.name, attribute_index, mode);
                    }) || changed;
                }
            };
            video_play_mode_radio("Loop", SceneObjectVideoPlayMode::Loop);
            ImGui::SameLine();
            video_play_mode_radio("Play Once", SceneObjectVideoPlayMode::PlayOnce);
            ImGui::SameLine();
            video_play_mode_radio("Off", SceneObjectVideoPlayMode::Off);

            float v_volume = attribute.video_2d.volume;
            if (ImGui::DragFloat("Volume", &v_volume, 0.01f, 0.0f, 1.0f, "%.2f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "video 2D volume", [&]()
                {
                    return SetSceneObjectAttributeVideo2DVolume(state.selected_item_path, object.name, attribute_index, std::clamp(v_volume, 0.0f, 1.0f));
                }) || changed;
            }

            bool v_muted = attribute.video_2d.muted;
            if (ImGui::Checkbox("Muted", &v_muted))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "video 2D muted", [&]()
                {
                    return SetSceneObjectAttributeVideo2DMuted(state.selected_item_path, object.name, attribute_index, v_muted);
                }) || changed;
            }

            ImGui::TextDisabled("Audio plays only in Play mode.");
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

            static bool rotation_drag_active = false;
            static std::string rotation_drag_object_name;
            static std::size_t rotation_drag_attribute_index = 0;
            static float rotation_drag_value = 0.0f;

            const bool same_rotation_drag_target =
                rotation_drag_active &&
                rotation_drag_object_name == object.name &&
                rotation_drag_attribute_index == attribute_index;

            float rotation_degrees = same_rotation_drag_target
                ? rotation_drag_value
                : attribute.skybox.rotation_degrees;

            if (ImGui::DragFloat("Rotation (deg)", &rotation_degrees, 0.25f, -3600.0f, 3600.0f, "%.2f"))
            {
                rotation_drag_active = true;
                rotation_drag_object_name = object.name;
                rotation_drag_attribute_index = attribute_index;
                rotation_drag_value = rotation_degrees;
            }

            if (same_rotation_drag_target && ImGui::IsItemDeactivatedAfterEdit())
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "skybox rotation", [&]()
                {
                    return SetSceneObjectAttributeSkyboxRotation(
                        state.selected_item_path,
                        object.name,
                        attribute_index,
                        rotation_drag_value);
                }) || changed;

                rotation_drag_active = false;
                rotation_drag_object_name.clear();
                rotation_drag_attribute_index = 0;
                rotation_drag_value = 0.0f;
            }

            break;
        }

        case SceneObjectAttributeKind::Audio:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            const std::string current_clip_label = attribute.audio.clip_path.empty()
                ? std::string("Drop Audio Clip (.wav/.ogg/.mp3)")
                : std::filesystem::path(attribute.audio.clip_path).filename().string();
            ImGui::Button(current_clip_label.c_str(), ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
                {
                    const char* payload_text = static_cast<const char*>(payload->Data);
                    const std::size_t payload_size = payload->DataSize > 0
                        ? static_cast<std::size_t>(payload->DataSize - 1)
                        : 0;
                    const std::filesystem::path dropped_path(std::string(payload_text, payload_size));
                    if (HasAnyExtension(dropped_path, {".wav", ".ogg", ".mp3"}))
                    {
                        const std::string normalized = NormalizeAssetPath(state, dropped_path);
                        changed = SaveSceneObjectAttributeEdit(state, object, "audio clip path", [&]()
                        {
                            return SetSceneObjectAttributeAudioClipPath(state.selected_item_path, object.name, attribute_index, normalized);
                        }) || changed;
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Play mode radio buttons — On plays when active (autoplays at scene start), Off is silent.
            ImGui::TextUnformatted("Play Mode");
            const SceneObjectAudioPlayMode current_mode = attribute.audio.play_mode;
            auto play_mode_radio = [&](const char* label, SceneObjectAudioPlayMode mode)
            {
                if (ImGui::RadioButton(label, current_mode == mode))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "audio play mode", [&]()
                    {
                        return SetSceneObjectAttributeAudioPlayMode(state.selected_item_path, object.name, attribute_index, mode);
                    }) || changed;
                }
            };
            play_mode_radio("On", SceneObjectAudioPlayMode::On);
            ImGui::SameLine();
            play_mode_radio("Off", SceneObjectAudioPlayMode::Off);

            float volume = attribute.audio.volume;
            if (ImGui::DragFloat("Volume", &volume, 0.02f, 0.0f, 20.0f, "%.2f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "audio volume", [&]()
                {
                    return SetSceneObjectAttributeAudioVolume(state.selected_item_path, object.name, attribute_index, std::clamp(volume, 0.0f, 20.0f));
                }) || changed;
            }

            bool loop = attribute.audio.loop;
            if (ImGui::Checkbox("Loop", &loop))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "audio loop", [&]()
                {
                    return SetSceneObjectAttributeAudioLoop(state.selected_item_path, object.name, attribute_index, loop);
                }) || changed;
            }

            bool spatialize_3d = attribute.audio.spatialize_3d;
            if (ImGui::Checkbox("3D Spatialize", &spatialize_3d))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "audio 3d spatialize", [&]()
                {
                    return SetSceneObjectAttributeAudioSpatialize(state.selected_item_path, object.name, attribute_index, spatialize_3d);
                }) || changed;
            }

            float pitch = attribute.audio.pitch;
            if (ImGui::DragFloat("Pitch", &pitch, 0.01f, 0.1f, 4.0f, "%.2f"))
            {
                changed = SaveSceneObjectAttributeEdit(state, object, "audio pitch", [&]()
                {
                    return SetSceneObjectAttributeAudioPitch(state.selected_item_path, object.name, attribute_index, std::clamp(pitch, 0.1f, 4.0f));
                }) || changed;
            }

            if (attribute.audio.spatialize_3d)
            {
                float min_distance = attribute.audio.min_distance;
                if (ImGui::DragFloat("Min Distance", &min_distance, 0.05f, 0.01f, 10000.0f, "%.2f"))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "audio min distance", [&]()
                    {
                        return SetSceneObjectAttributeAudioMinDistance(state.selected_item_path, object.name, attribute_index, (std::max)(0.01f, min_distance));
                    }) || changed;
                }

                float max_distance = attribute.audio.max_distance;
                if (ImGui::DragFloat("Max Distance", &max_distance, 0.1f, 0.02f, 100000.0f, "%.2f"))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "audio max distance", [&]()
                    {
                        return SetSceneObjectAttributeAudioMaxDistance(state.selected_item_path, object.name, attribute_index, (std::max)(0.02f, max_distance));
                    }) || changed;
                }

                float doppler_factor = attribute.audio.doppler_factor;
                if (ImGui::DragFloat("Doppler Factor", &doppler_factor, 0.01f, 0.0f, 10.0f, "%.2f"))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "audio doppler factor", [&]()
                    {
                        return SetSceneObjectAttributeAudioDopplerFactor(state.selected_item_path, object.name, attribute_index, std::clamp(doppler_factor, 0.0f, 10.0f));
                    }) || changed;
                }
            }

            break;
        }

        case SceneObjectAttributeKind::Effects:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            const std::string current_effect_label = attribute.effects.effect_path.empty()
                ? std::string("Drop Effect (.efk/.efkefc)")
                : std::filesystem::path(attribute.effects.effect_path).filename().string();
            ImGui::Button(current_effect_label.c_str(), ImVec2(-1.0f, 0.0f));
            if (ImGui::BeginDragDropTarget())
            {
                const std::filesystem::path dropped_path = GetDragDroppedPath();
                if (!dropped_path.empty())
                {
                    if (HasAnyExtension(dropped_path, {".efk", ".efkefc"}))
                    {
                        const std::string normalized = NormalizeAssetPath(state, dropped_path);
                        changed = SaveSceneObjectAttributeEdit(state, object, "effect path", [&]()
                        {
                            return SetSceneObjectAttributeEffectsPath(state.selected_item_path, object.name, attribute_index, normalized);
                        }) || changed;
                    }
                    else
                    {
                        state.AddLog("Drop a supported effect: .efk or .efkefc");
                    }
                }
                ImGui::EndDragDropTarget();
            }

            if (!attribute.effects.effect_path.empty())
            {
                ImGui::TextWrapped("Path: %s", attribute.effects.effect_path.c_str());
            }

            ImGui::TextUnformatted("Play Mode");
            const SceneObjectEffectsPlayMode current_mode = attribute.effects.trigger_mode;
            auto effect_play_mode_radio = [&](const char* label, SceneObjectEffectsPlayMode mode)
            {
                if (ImGui::RadioButton(label, current_mode == mode))
                {
                    changed = SaveSceneObjectAttributeEdit(state, object, "effect play mode", [&]()
                    {
                        return SetSceneObjectAttributeEffectsPlayMode(state.selected_item_path, object.name, attribute_index, mode);
                    }) || changed;
                }
            };
            effect_play_mode_radio("Loop", SceneObjectEffectsPlayMode::Loop);
            ImGui::SameLine();
            effect_play_mode_radio("Play Once", SceneObjectEffectsPlayMode::PlayOnce);

            break;
        }

        case SceneObjectAttributeKind::Shader:
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");
            ImGui::TextDisabled("Applies to the object's 3D Shape.");

            struct ShaderTypeOption
            {
                const char* label;
                SceneObjectShaderType type;
            };
            constexpr ShaderTypeOption kShaderTypeOptions[] = {
                {"None", SceneObjectShaderType::None},
                {"Water", SceneObjectShaderType::Water},
                {"Cloud", SceneObjectShaderType::Cloud},
            };

            int selected_shader_index = 0;
            for (int i = 0; i < static_cast<int>(std::size(kShaderTypeOptions)); ++i)
            {
                if (kShaderTypeOptions[i].type == attribute.shader.type)
                {
                    selected_shader_index = i;
                    break;
                }
            }

            if (ImGui::BeginCombo("Type", kShaderTypeOptions[selected_shader_index].label))
            {
                for (int i = 0; i < static_cast<int>(std::size(kShaderTypeOptions)); ++i)
                {
                    const bool selected = i == selected_shader_index;
                    if (ImGui::Selectable(kShaderTypeOptions[i].label, selected))
                    {
                        changed = SaveSceneObjectAttributeEdit(state, object, "shader type", [&]()
                        {
                            return SetSceneObjectAttributeShaderType(state.selected_item_path, object.name, attribute_index, kShaderTypeOptions[i].type);
                        }) || changed;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
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