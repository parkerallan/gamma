#include "panels/InfoPanel.h"

#include "components/SceneObjectAttributesEditor.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
constexpr const char* kFileTreeDragDropPayload = "FILE_TREE_PATH";

std::string FormatFileSize(std::uintmax_t size_in_bytes)
{
    constexpr double kKilobyte = 1024.0;
    constexpr double kMegabyte = 1024.0 * 1024.0;

    char buffer[64]{};
    if (size_in_bytes >= static_cast<std::uintmax_t>(kMegabyte))
    {
        std::snprintf(buffer, sizeof(buffer), "%.2f MB", static_cast<double>(size_in_bytes) / kMegabyte);
        return buffer;
    }

    if (size_in_bytes >= static_cast<std::uintmax_t>(kKilobyte))
    {
        std::snprintf(buffer, sizeof(buffer), "%.2f KB", static_cast<double>(size_in_bytes) / kKilobyte);
        return buffer;
    }

    std::snprintf(buffer, sizeof(buffer), "%llu bytes", static_cast<unsigned long long>(size_in_bytes));
    return buffer;
}

int CountDirectoryItems(const std::filesystem::path& directory_path)
{
    std::error_code error;
    int count = 0;
    for (std::filesystem::directory_iterator it(directory_path, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
    {
        ++count;
    }

    return error ? -1 : count;
}

unsigned int CountTextureReferences(const ModelMetadata& metadata)
{
    unsigned int count = 0;
    for (const ModelMaterialMetadata& material : metadata.materials)
    {
        count += static_cast<unsigned int>(material.textures.size());
    }

    return count;
}

ImVec4 ToImVec4(const ModelColorMetadata& color)
{
    return ImVec4(color.rgba[0], color.rgba[1], color.rgba[2], color.rgba[3]);
}

bool IsSupportedModelAttachment(const std::filesystem::path& path)
{
    return ModelMetadata::IsSupportedModelPath(path);
}

bool IsSupportedScriptAttachment(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value)
    {
        return static_cast<char>(std::tolower(value));
    });

    return extension == ".lua";
}

bool IsSupportedGraphAttachment(const std::filesystem::path& path)
{
    return path.extension() == ".graph";
}

bool SaveSceneObjectVector3Edit(
    EngineState& state,
    const SceneObjectMetadata& object,
    const char* log_label,
    const SceneVector3& value,
    bool (*setter)(const std::filesystem::path&, const std::string&, const SceneVector3&))
{
    if (state.HasOpenFile() && state.open_file_path == state.selected_item_path && state.open_file_dirty)
    {
        state.AddLog(std::string("Save the open scene before editing object ") + log_label);
        return false;
    }

    if (!setter(state.selected_item_path, object.name, value))
    {
        return false;
    }

    if (state.HasOpenFile() && state.open_file_path == state.selected_item_path && !state.open_file_dirty)
    {
        std::ifstream input(state.selected_item_path, std::ios::binary);
        if (input)
        {
            state.saved_file_contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            state.open_file_contents = state.saved_file_contents;
            std::fill(state.editor_buffer.begin(), state.editor_buffer.end(), '\0');
            std::copy(state.open_file_contents.begin(), state.open_file_contents.end(), state.editor_buffer.begin());
            state.open_file_dirty = false;
        }
    }

    state.AddLog(std::string("Updated object ") + log_label + ": " + object.name);
    return true;
}

void UpdateCachedSceneObjectVector3(
    SceneMetadata& scene_metadata,
    const std::string& object_name,
    SceneVector3 SceneObjectMetadata::*field,
    const SceneVector3& value)
{
    const auto object_it = std::find_if(scene_metadata.objects.begin(), scene_metadata.objects.end(), [&](SceneObjectMetadata& object)
    {
        return object.name == object_name;
    });
    if (object_it != scene_metadata.objects.end())
    {
        (*object_it).*field = value;
    }
}

SceneVector3 SnapPositionToGrid(const SceneVector3& value, const EngineState& state)
{
    if (!state.snap_to_grid || state.grid_size <= 0.0f)
    {
        return value;
    }

    SceneVector3 snapped = value;
    for (float& component : snapped)
    {
        component = std::round(component / state.grid_size) * state.grid_size;
    }

    return snapped;
}
}

InfoPanel::~InfoPanel()
{
    Shutdown();
}

bool InfoPanel::InitializeSceneRenderer(VulkanContext* context)
{
    preview_vulkan_context_ = context;
    return preview_vulkan_context_ != nullptr;
}

void InfoPanel::BeginFrame()
{
    for (auto& renderer_entry : camera_preview_renderers_)
    {
        if (renderer_entry.second)
        {
            renderer_entry.second->BeginFrame();
        }
    }
}

void InfoPanel::Shutdown()
{
    ClearCameraPreviewRenderers();
    model_asset_cache_.clear();
    active_camera_preview_scope_.clear();
    preview_vulkan_context_ = nullptr;
    image_info_renderer_.Shutdown();
    font_info_renderer_.Shutdown();
}

const ModelMetadata& InfoPanel::GetModelMetadata(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool cache_valid = has_cached_model_metadata_ && cached_model_path_ == path && !error && cached_model_write_time_ == write_time;
    if (cache_valid)
    {
        return cached_model_metadata_;
    }

    cached_model_path_ = path;
    cached_model_write_time_ = error ? std::filesystem::file_time_type::min() : write_time;
    cached_model_metadata_ = LoadModelMetadata(path);
    has_cached_model_metadata_ = true;
    return cached_model_metadata_;
}

const InfoPanel::CachedModelAssetEntry& InfoPanel::GetModelAssetEntry(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    CachedModelAssetEntry& cache_entry = model_asset_cache_[path];
    if (error || cache_entry.write_time != write_time || !cache_entry.asset.loaded)
    {
        cache_entry.write_time = error ? std::filesystem::file_time_type::min() : write_time;
        cache_entry.asset = LoadModelAsset(path);
    }

    return cache_entry;
}

const ParsedMaterialMetadata& InfoPanel::GetMaterialMetadata(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool cache_valid = has_cached_material_metadata_ && cached_material_path_ == path && !error && cached_material_write_time_ == write_time;
    if (cache_valid)
    {
        return cached_material_metadata_;
    }

    cached_material_path_ = path;
    cached_material_write_time_ = error ? std::filesystem::file_time_type::min() : write_time;
    cached_material_metadata_ = LoadMaterialMetadata(path);
    has_cached_material_metadata_ = true;
    return cached_material_metadata_;
}

const SceneMetadata& InfoPanel::GetSceneMetadata(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool cache_valid = has_cached_scene_metadata_ && cached_scene_path_ == path && !error && cached_scene_write_time_ == write_time;
    if (cache_valid)
    {
        return cached_scene_metadata_;
    }

    cached_scene_path_ = path;
    cached_scene_write_time_ = error ? std::filesystem::file_time_type::min() : write_time;
    cached_scene_metadata_ = LoadSceneMetadata(path);
    has_cached_scene_metadata_ = true;
    return cached_scene_metadata_;
}

const ImageMetadata& InfoPanel::GetImageMetadata(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool cache_valid = has_cached_image_metadata_ && cached_image_path_ == path && !error && cached_image_write_time_ == write_time;
    if (cache_valid)
    {
        return cached_image_metadata_;
    }

    cached_image_path_ = path;
    cached_image_write_time_ = error ? std::filesystem::file_time_type::min() : write_time;
    cached_image_metadata_ = LoadImageMetadata(path);
    has_cached_image_metadata_ = true;
    return cached_image_metadata_;
}

const FontMetadata& InfoPanel::GetFontMetadata(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool cache_valid = has_cached_font_metadata_ && cached_font_path_ == path && !error && cached_font_write_time_ == write_time;
    if (cache_valid)
    {
        return cached_font_metadata_;
    }

    cached_font_path_ = path;
    cached_font_write_time_ = error ? std::filesystem::file_time_type::min() : write_time;
    cached_font_metadata_ = LoadFontMetadata(path);
    has_cached_font_metadata_ = true;
    return cached_font_metadata_;
}

SceneViewportRenderer* InfoPanel::GetCameraPreviewRenderer(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index)
{
    if (preview_vulkan_context_ == nullptr)
    {
        return nullptr;
    }

    const std::string renderer_key = scene_path.generic_string() + "::" + object_name + "#" + std::to_string(attribute_index);
    std::unique_ptr<SceneViewportRenderer>& renderer = camera_preview_renderers_[renderer_key];
    if (!renderer)
    {
        renderer = std::make_unique<SceneViewportRenderer>();
        if (!renderer->Initialize(preview_vulkan_context_))
        {
            renderer.reset();
            return nullptr;
        }
    }

    return renderer.get();
}

void InfoPanel::ClearCameraPreviewRenderers()
{
    if (!camera_preview_renderers_.empty() && preview_vulkan_context_ != nullptr)
    {
        preview_vulkan_context_->WaitIdle();
    }

    for (auto& renderer_entry : camera_preview_renderers_)
    {
        if (renderer_entry.second)
        {
            renderer_entry.second->Shutdown();
        }
    }

    camera_preview_renderers_.clear();
}

void InfoPanel::RenderCameraAttributePreview(
    const SceneMetadata& scene_metadata,
    EngineState& state,
    const SceneObjectMetadata& object,
    std::size_t attribute_index,
    const SceneObjectAttribute& attribute)
{
    if (attribute.kind != SceneObjectAttributeKind::Camera)
    {
        return;
    }

    if (!scene_metadata.parsed)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Preview unavailable.");
        return;
    }

    SceneViewportRenderer* renderer = GetCameraPreviewRenderer(state.selected_item_path, object.name, attribute_index);
    if (renderer == nullptr)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Preview renderer unavailable.");
        return;
    }

    renderer->RenderCameraPreview(
        state,
        scene_metadata,
        [this](const std::filesystem::path& path) -> SceneViewportResolvedModel
        {
            const CachedModelAssetEntry& entry = GetModelAssetEntry(path);
            return SceneViewportResolvedModel{&entry.asset, entry.write_time};
        },
        object,
        attribute.camera);
}

void InfoPanel::RenderMaterialMetadata(const ParsedMaterialMetadata& metadata) const
{
    ImGui::Spacing();
    ImGui::SeparatorText("Material");

    if (!metadata.parsed)
    {
        ImGui::TextUnformatted("Unable to parse material metadata.");
        if (!metadata.error_message.empty())
        {
            ImGui::TextWrapped("Reason: %s", metadata.error_message.c_str());
        }
        return;
    }

    ImGui::Text("Material: %s", metadata.material_name.c_str());
    ImGui::Text("Shader: %s", metadata.shader_name.empty() ? "Default" : metadata.shader_name.c_str());
    if (metadata.has_base_color)
    {
        ImGui::TextUnformatted("Base Color");
        ImGui::SameLine();
        ImGui::ColorButton("##ParsedMaterialBaseColor", ImVec4(metadata.base_color[0], metadata.base_color[1], metadata.base_color[2], metadata.base_color[3]), ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f));
    }

    if (metadata.has_emissive_color)
    {
        ImGui::TextUnformatted("Emissive");
        ImGui::SameLine();
        ImGui::ColorButton("##ParsedMaterialEmissive", ImVec4(metadata.emissive_color[0], metadata.emissive_color[1], metadata.emissive_color[2], metadata.emissive_color[3]), ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f));
    }
    if (metadata.has_opacity)
    {
        ImGui::Text("Opacity: %.2f", metadata.opacity);
    }
    if (metadata.has_roughness)
    {
        ImGui::Text("Roughness: %.2f", metadata.roughness);
    }
    if (metadata.has_metalness)
    {
        ImGui::Text("Metalness: %.2f", metadata.metalness);
    }

    if ((metadata.has_base_color || metadata.has_emissive_color || metadata.has_opacity || metadata.has_roughness || metadata.has_metalness) && !metadata.textures.empty())
    {
        ImGui::Separator();
    }

    if (metadata.textures.empty())
    {
        ImGui::TextUnformatted("No texture references.");
    }
    else
    {
        for (const ParsedMaterialTextureReference& texture : metadata.textures)
        {
            ImGui::TextWrapped("%s: %s", texture.slot_label.c_str(), texture.path.c_str());
        }
    }

}

bool InfoPanel::HandleSceneObjectAttachmentDrop(EngineState& state, const std::filesystem::path& scene_path, const std::string& object_name)
{
    bool changed = false;
    if (!ImGui::BeginDragDropTarget())
    {
        return false;
    }

    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFileTreeDragDropPayload))
    {
        const char* source_raw = static_cast<const char*>(payload->Data);
        const std::filesystem::path source_path(source_raw);

        if (state.HasOpenFile() && state.open_file_path == scene_path && state.open_file_dirty)
        {
            state.AddLog("Save the open scene before attaching assets to an object");
        }
        else if (IsSupportedModelAttachment(source_path))
        {
            if (SetSceneObjectModel(scene_path, object_name, state.project_root, source_path))
            {
                state.AddLog("Attached model to object: " + object_name);
                state.OpenTextFile(scene_path);
                has_cached_scene_metadata_ = false;
                changed = true;
            }
        }
        else if (IsSupportedScriptAttachment(source_path))
        {
            if (AddSceneObjectScript(scene_path, object_name, state.project_root, source_path))
            {
                state.AddLog("Attached script to object: " + object_name);
                state.OpenTextFile(scene_path);
                has_cached_scene_metadata_ = false;
                changed = true;
            }
        }
        else if (IsSupportedGraphAttachment(source_path))
        {
            if (AddSceneObjectGraph(scene_path, object_name, state.project_root, source_path))
            {
                state.AddLog("Attached graph to object: " + object_name);
                state.OpenTextFile(scene_path);
                has_cached_scene_metadata_ = false;
                changed = true;
            }
        }
        else
        {
            state.AddLog("Attach supported assets only: .fbx/.glb model, .lua script, or .graph graph");
        }
    }

    ImGui::EndDragDropTarget();
    return changed;
}

void InfoPanel::RenderSelectedSceneObject(EngineState& state)
{
    const SceneMetadata& scene_metadata = GetSceneMetadata(state.selected_item_path);
    if (!scene_metadata.parsed)
    {
        ImGui::Spacing();
        ImGui::SeparatorText("Scene Object");
        ImGui::TextUnformatted("Unable to parse scene object data.");
        if (!scene_metadata.error_message.empty())
        {
            ImGui::TextWrapped("Reason: %s", scene_metadata.error_message.c_str());
        }
        return;
    }

    const auto object_it = std::find_if(scene_metadata.objects.begin(), scene_metadata.objects.end(), [&](const SceneObjectMetadata& object)
    {
        return object.name == state.selected_scene_object_name;
    });
    if (object_it == scene_metadata.objects.end())
    {
        return;
    }

    SceneObjectMetadata selected_object = *object_it;

    const std::string preview_scope = state.selected_item_path.generic_string() + "::" + selected_object.name;
    if (active_camera_preview_scope_ != preview_scope)
    {
        ClearCameraPreviewRenderers();
        active_camera_preview_scope_ = preview_scope;
    }

    ImGui::SeparatorText("Object");
    ImGui::Text("Name: %s", selected_object.name.c_str());

    ImGui::Spacing();
    ImGui::SeparatorText("Transform");

    float position[3] = {selected_object.position[0], selected_object.position[1], selected_object.position[2]};
    if (ImGui::DragFloat3("Position", position, 0.1f))
    {
        const SceneVector3 snapped_position = SnapPositionToGrid({position[0], position[1], position[2]}, state);
        if (SaveSceneObjectVector3Edit(state, selected_object, "position", snapped_position, SetSceneObjectPosition))
        {
            selected_object.position = snapped_position;
            UpdateCachedSceneObjectVector3(cached_scene_metadata_, selected_object.name, &SceneObjectMetadata::position, snapped_position);
        }
    }

    float rotation[3] = {selected_object.rotation[0], selected_object.rotation[1], selected_object.rotation[2]};
    if (ImGui::DragFloat3("Rotation", rotation, 0.5f))
    {
        const SceneVector3 updated_rotation = {rotation[0], rotation[1], rotation[2]};
        if (SaveSceneObjectVector3Edit(state, selected_object, "rotation", updated_rotation, SetSceneObjectRotation))
        {
            selected_object.rotation = updated_rotation;
            UpdateCachedSceneObjectVector3(cached_scene_metadata_, selected_object.name, &SceneObjectMetadata::rotation, updated_rotation);
        }
    }

    float scale[3] = {selected_object.scale[0], selected_object.scale[1], selected_object.scale[2]};
    if (ImGui::DragFloat3("Scale", scale, 0.05f, 0.001f, 1000.0f))
    {
        const SceneVector3 updated_scale = {scale[0], scale[1], scale[2]};
        if (SaveSceneObjectVector3Edit(state, selected_object, "scale", updated_scale, SetSceneObjectScale))
        {
            selected_object.scale = updated_scale;
            UpdateCachedSceneObjectVector3(cached_scene_metadata_, selected_object.name, &SceneObjectMetadata::scale, updated_scale);
        }
    }

    if (RenderSceneObjectAttributesEditor(
            state,
            selected_object,
            [this, &scene_metadata](EngineState& callback_state, const SceneObjectMetadata& callback_object, std::size_t attribute_index, const SceneObjectAttribute& attribute)
            {
                RenderCameraAttributePreview(scene_metadata, callback_state, callback_object, attribute_index, attribute);
            }))
    {
        has_cached_scene_metadata_ = false;
        return;
    }

    ImGui::Spacing();

    if (!selected_object.model_path.empty())
    {
        ImGui::PushID("Model");
        bool keep_model_attachment = true;
        if (ImGui::CollapsingHeader(selected_object.model_path.c_str(), &keep_model_attachment, ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");

            float model_visual_offset[3] = {
                selected_object.model_visual_offset[0],
                selected_object.model_visual_offset[1],
                selected_object.model_visual_offset[2],
            };
            if (ImGui::DragFloat3("Visual Offset", model_visual_offset, 0.01f))
            {
                const SceneVector3 updated_model_visual_offset = {
                    model_visual_offset[0],
                    model_visual_offset[1],
                    model_visual_offset[2]};
                if (SaveSceneObjectVector3Edit(
                        state,
                        selected_object,
                        "model visual offset",
                        updated_model_visual_offset,
                        SetSceneObjectModelVisualOffset))
                {
                    selected_object.model_visual_offset = updated_model_visual_offset;
                    UpdateCachedSceneObjectVector3(cached_scene_metadata_, selected_object.name, &SceneObjectMetadata::model_visual_offset, updated_model_visual_offset);
                }
            }

            ImGui::TextDisabled("Applies to rendering only. Rigidbody collisions use object transform.");
        }
        if (!keep_model_attachment)
        {
            if (state.HasOpenFile() && state.open_file_path == state.selected_item_path && state.open_file_dirty)
            {
                state.AddLog("Save the open scene before removing the object model");
            }
            else if (ClearSceneObjectModel(state.selected_item_path, selected_object.name))
            {
                state.AddLog("Removed model from object: " + selected_object.name);
                state.OpenTextFile(state.selected_item_path);
                has_cached_scene_metadata_ = false;
                ImGui::PopID();
                return;
            }
        }
        ImGui::PopID();
    }

    for (std::size_t script_index = 0; script_index < selected_object.script_paths.size(); ++script_index)
    {
        const std::string& script_path = selected_object.script_paths[script_index];
        ImGui::PushID(static_cast<int>(10000 + script_index));
        bool keep_script_attachment = true;
        if (ImGui::CollapsingHeader(script_path.c_str(), &keep_script_attachment, ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");
            ImGui::TextDisabled("Execution order follows list order.");
        }
        if (!keep_script_attachment)
        {
            if (state.HasOpenFile() && state.open_file_path == state.selected_item_path && state.open_file_dirty)
            {
                state.AddLog("Save the open scene before removing an object script");
            }
            else if (RemoveSceneObjectScript(state.selected_item_path, selected_object.name, state.project_root, state.project_root / script_path))
            {
                state.AddLog("Removed script from object: " + selected_object.name);
                state.OpenTextFile(state.selected_item_path);
                has_cached_scene_metadata_ = false;
                ImGui::PopID();
                return;
            }
        }
        ImGui::PopID();
    }

    for (std::size_t graph_index = 0; graph_index < selected_object.graph_paths.size(); ++graph_index)
    {
        const std::string& graph_path = selected_object.graph_paths[graph_index];
        ImGui::PushID(static_cast<int>(20000 + graph_index));
        bool keep_graph_attachment = true;
        if (ImGui::CollapsingHeader(graph_path.c_str(), &keep_graph_attachment, ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Spacing();
            ImGui::TextUnformatted("Settings");
            ImGui::TextDisabled("Graph assets are loaded on demand.");
        }
        if (!keep_graph_attachment)
        {
            if (state.HasOpenFile() && state.open_file_path == state.selected_item_path && state.open_file_dirty)
            {
                state.AddLog("Save the open scene before removing an object graph");
            }
            else if (RemoveSceneObjectGraph(state.selected_item_path, selected_object.name, state.project_root, state.project_root / graph_path))
            {
                state.AddLog("Removed graph from object: " + selected_object.name);
                state.OpenTextFile(state.selected_item_path);
                has_cached_scene_metadata_ = false;
                ImGui::PopID();
                return;
            }
        }
        ImGui::PopID();
    }

    ImGui::InvisibleButton("##ObjectAttachmentDropTarget", ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y));
    if (HandleSceneObjectAttachmentDrop(state, state.selected_item_path, selected_object.name))
    {
        return;
    }
}

void InfoPanel::RenderImageMetadata(const std::filesystem::path& path, const ImageMetadata& metadata, VulkanContext* vulkan_context)
{
    ImGui::Spacing();
    ImGui::SeparatorText("Image");

    if (!metadata.parsed)
    {
        ImGui::TextUnformatted("Unable to parse image metadata.");
        if (!metadata.error_message.empty())
        {
            ImGui::TextWrapped("Reason: %s", metadata.error_message.c_str());
        }
        return;
    }

    if (ImTextureID preview_image = image_info_renderer_.GetImagePreview(path, vulkan_context))
    {
        const float available_width = ImGui::GetContentRegionAvail().x;
        const float max_preview_width = available_width > 0.0f ? available_width : 220.0f;
        const float max_preview_height = 220.0f;
        const float width_scale = max_preview_width / static_cast<float>(image_info_renderer_.GetPreviewWidth());
        const float height_scale = max_preview_height / static_cast<float>(image_info_renderer_.GetPreviewHeight());
        const float scale = (std::min)(1.0f, (std::min)(width_scale, height_scale));
        const ImVec2 preview_size(
            static_cast<float>(image_info_renderer_.GetPreviewWidth()) * scale,
            static_cast<float>(image_info_renderer_.GetPreviewHeight()) * scale);

        ImGui::Image(preview_image, preview_size);
        ImGui::Spacing();
    }
    else if (image_info_renderer_.IsPreviewLoading(path))
    {
        const float available_width = ImGui::GetContentRegionAvail().x;
        const float preview_width = (std::max)(120.0f, available_width > 0.0f ? available_width : 220.0f);
        const ImVec2 loading_min = ImGui::GetCursorScreenPos();
        const ImVec2 loading_size(preview_width, 120.0f);
        const ImVec2 loading_max(loading_min.x + loading_size.x, loading_min.y + loading_size.y);

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        draw_list->AddRectFilled(loading_min, loading_max, IM_COL32(34, 38, 44, 255), 6.0f);
        draw_list->AddRect(loading_min, loading_max, IM_COL32(82, 88, 98, 255), 6.0f, 0, 1.0f);

        const char* loading_message = "Loading preview...";
        const ImVec2 text_size = ImGui::CalcTextSize(loading_message);
        draw_list->AddText(
            ImVec2(
                loading_min.x + (loading_size.x - text_size.x) * 0.5f,
                loading_min.y + (loading_size.y - text_size.y) * 0.5f),
            IM_COL32(200, 205, 215, 255),
            loading_message);

        ImGui::Dummy(loading_size);
        ImGui::Spacing();
    }

    ImGui::Text("Resolution: %d x %d", metadata.width, metadata.height);
    ImGui::Text("Channels: %d", metadata.channel_count);
    ImGui::Text("Bits/channel: %d", metadata.bits_per_channel);
}

void InfoPanel::RenderFontMetadata(const std::filesystem::path& path, const FontMetadata& metadata, VulkanContext* vulkan_context)
{
    ImGui::Spacing();
    ImGui::SeparatorText("Font");

    if (!metadata.parsed)
    {
        ImGui::TextUnformatted("Unable to parse font metadata.");
        if (!metadata.error_message.empty())
        {
            ImGui::TextWrapped("Reason: %s", metadata.error_message.c_str());
        }
        return;
    }

    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Preview Size", &font_preview_size_pixels_, 18.0f, 72.0f, "%.0f px");

    if (ImTextureID preview_image = font_info_renderer_.GetFontPreview(path, vulkan_context, font_preview_size_pixels_))
    {
        const float available_width = ImGui::GetContentRegionAvail().x;
        const float max_preview_width = available_width > 0.0f ? available_width : 220.0f;
        const float max_preview_height = 420.0f;
        const float width_scale = max_preview_width / static_cast<float>(font_info_renderer_.GetPreviewWidth());
        const float height_scale = max_preview_height / static_cast<float>(font_info_renderer_.GetPreviewHeight());
        const float scale = (std::min)(1.0f, (std::min)(width_scale, height_scale));
        const ImVec2 preview_size(
            static_cast<float>(font_info_renderer_.GetPreviewWidth()) * scale,
            static_cast<float>(font_info_renderer_.GetPreviewHeight()) * scale);

        ImGui::Image(preview_image, preview_size);
        ImGui::Spacing();
    }
    else
    {
        ImGui::TextDisabled("Preview unavailable.");
        ImGui::Spacing();
    }

    ImGui::Text("Glyphs: %d", metadata.glyph_count);
}

void InfoPanel::RenderModelMetadata(const ModelMetadata& metadata) const
{
    ImGui::Spacing();
    ImGui::SeparatorText("Model");

    if (!metadata.parsed)
    {
        ImGui::TextUnformatted("Unable to parse model metadata.");
        if (!metadata.error_message.empty())
        {
            ImGui::TextWrapped("Reason: %s", metadata.error_message.c_str());
        }
        return;
    }

    ImGui::Text("Nodes: %u", metadata.node_count);
    ImGui::Text("Meshes: %u", metadata.mesh_count);
    ImGui::Text("Materials: %u", metadata.material_count);
    ImGui::Text("Texture refs: %u", CountTextureReferences(metadata));
    ImGui::Text("Embedded textures: %u", metadata.embedded_texture_count);
    ImGui::Text("Animations: %u", metadata.animation_count);

    if (ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (metadata.materials.empty())
        {
            ImGui::TextUnformatted("No materials found.");
        }
        else
        {
            for (std::size_t index = 0; index < metadata.materials.size(); ++index)
            {
                const ModelMaterialMetadata& material = metadata.materials[index];
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::TreeNode(material.name.c_str()))
                {
                    if (material.base_color.valid)
                    {
                        ImGui::TextUnformatted("Base Color");
                        ImGui::SameLine();
                        ImGui::ColorButton("##BaseColor", ToImVec4(material.base_color), ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f));
                    }
                    if (material.emissive_color.valid)
                    {
                        ImGui::TextUnformatted("Emissive");
                        ImGui::SameLine();
                        ImGui::ColorButton("##EmissiveColor", ToImVec4(material.emissive_color), ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f));
                        if (material.has_emissive_strength)
                        {
                            ImGui::SameLine();
                            ImGui::Text("(x%.2f)", material.emissive_strength);
                        }
                    }
                    if (material.specular_color.valid)
                    {
                        ImGui::TextUnformatted("Specular Color");
                        ImGui::SameLine();
                        ImGui::ColorButton("##SpecularColor", ToImVec4(material.specular_color), ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f));
                    }
                    if (material.has_specular_factor)
                    {
                        ImGui::Text("Specular Factor: %.2f", material.specular_factor);
                    }
                    if (material.sheen_color.valid)
                    {
                        ImGui::TextUnformatted("Sheen Color");
                        ImGui::SameLine();
                        ImGui::ColorButton("##SheenColor", ToImVec4(material.sheen_color), ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f));
                    }
                    if (material.has_sheen_roughness)
                    {
                        ImGui::Text("Sheen Roughness: %.2f", material.sheen_roughness_factor);
                    }
                    if (material.has_opacity)
                    {
                        ImGui::Text("Opacity: %.2f", material.opacity);
                    }
                    ImGui::Text("Alpha Mode: %s", material.alpha_mode.c_str());
                    if (material.has_alpha_cutoff)
                    {
                        ImGui::Text("Alpha Cutoff: %.2f", material.alpha_cutoff);
                    }
                    if (material.double_sided)
                    {
                        ImGui::TextUnformatted("Double-sided");
                    }
                    if (material.unlit)
                    {
                        ImGui::TextUnformatted("Unlit");
                    }
                    if (material.has_roughness)
                    {
                        ImGui::Text("Roughness: %.2f", material.roughness);
                    }
                    if (material.has_metalness)
                    {
                        ImGui::Text("Metalness: %.2f", material.metalness);
                    }
                    if (material.has_normal_scale)
                    {
                        ImGui::Text("Normal Scale: %.2f", material.normal_scale);
                    }
                    if (material.has_occlusion_strength)
                    {
                        ImGui::Text("Occlusion Strength: %.2f", material.occlusion_strength);
                    }
                    if (material.has_ior)
                    {
                        ImGui::Text("IOR: %.3f", material.index_of_refraction);
                    }
                    if (material.has_transmission)
                    {
                        ImGui::Text("Transmission: %.2f", material.transmission_factor);
                    }
                    if (material.has_iridescence)
                    {
                        ImGui::Text("Iridescence: %.2f", material.iridescence_factor);
                        ImGui::Text("Iridescence IOR: %.3f", material.iridescence_ior);
                        ImGui::Text("Iridescence Thickness: %.0f - %.0f nm", material.iridescence_thickness_min, material.iridescence_thickness_max);
                    }
                    if (material.has_volume)
                    {
                        ImGui::Text("Volume Thickness: %.2f", material.volume_thickness_factor);
                        if (material.attenuation_distance > 0.0f)
                        {
                            ImGui::Text("Attenuation Distance: %.4f", material.attenuation_distance);
                        }
                        if (material.attenuation_color.valid)
                        {
                            ImGui::TextUnformatted("Attenuation Color");
                            ImGui::SameLine();
                            ImGui::ColorButton("##AttenuationColor", ToImVec4(material.attenuation_color), ImGuiColorEditFlags_NoTooltip, ImVec2(18.0f, 18.0f));
                        }
                    }
                    if (material.has_clearcoat)
                    {
                        ImGui::Text("Clearcoat: %.2f", material.clearcoat_factor);
                        ImGui::Text("Clearcoat Roughness: %.2f", material.clearcoat_roughness_factor);
                        ImGui::Text("Clearcoat Normal Scale: %.2f", material.clearcoat_normal_scale);
                    }

                    const char* texture_label = material.textures.empty()
                        ? "Textures (0)"
                        : nullptr;
                    char texture_label_buf[32]{};
                    if (texture_label == nullptr)
                    {
                        std::snprintf(texture_label_buf, sizeof(texture_label_buf), "Textures (%zu)", material.textures.size());
                        texture_label = texture_label_buf;
                    }
                    ImGui::Spacing();
                    if (ImGui::TreeNode(texture_label))
                    {
                        if (material.textures.empty())
                        {
                            ImGui::TextUnformatted("No texture references.");
                        }
                        else
                        {
                            for (const ModelTextureReference& texture : material.textures)
                            {
                                ImGui::TextWrapped("%s: %s", texture.slot_label.c_str(), texture.path.c_str());
                            }
                        }
                        ImGui::TreePop();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
    }

    if (ImGui::CollapsingHeader("Animations", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (metadata.animations.empty())
        {
            ImGui::TextUnformatted("No animations found.");
        }
        else
        {
            for (const ModelAnimationMetadata& animation : metadata.animations)
            {
                ImGui::TextUnformatted(animation.name.c_str());
                const double duration_seconds = (animation.ticks_per_second > 0.0)
                    ? animation.duration / animation.ticks_per_second
                    : animation.duration;
                ImGui::Text("Duration: %.2f s", duration_seconds);
                ImGui::Text("Channels: %u", animation.channel_count);
                ImGui::Spacing();
            }
        }
    }
}

void InfoPanel::Render(EngineState& state, VulkanContext* vulkan_context)
{
    if (!state.show_info_panel)
    {
        return;
    }

    if (!ImGui::Begin("Info", &state.show_info_panel))
    {
        ImGui::End();
        return;
    }

    if (!state.HasOpenProject())
    {
        ImGui::TextUnformatted("No project loaded.");
        ImGui::End();
        return;
    }

    if (!state.HasSelectedItem())
    {
        ImGui::TextUnformatted("No file selected.");
        ImGui::TextWrapped("Select a file or folder in the Files panel to inspect it here.");
        ImGui::End();
        return;
    }

    const std::filesystem::path selected_path = state.selected_item_path;
    const std::error_code exists_error{};
    if (!std::filesystem::exists(selected_path))
    {
        ImGui::TextUnformatted("Selection unavailable.");
        ImGui::TextWrapped("The selected item no longer exists on disk.");
        ImGui::End();
        return;
    }

    const bool is_directory = std::filesystem::is_directory(selected_path);

    if (!state.HasSelectedSceneObject() && !active_camera_preview_scope_.empty())
    {
        ClearCameraPreviewRenderers();
        active_camera_preview_scope_.clear();
    }

    if (state.HasSelectedSceneObject())
    {
        RenderSelectedSceneObject(state);
        ImGui::End();
        return;
    }

    if (!is_directory)
    {
        if (ModelMetadata::IsSupportedModelPath(selected_path))
        {
            RenderModelMetadata(GetModelMetadata(selected_path));
        }
        else if (ParsedMaterialMetadata::IsSupportedPath(selected_path))
        {
            RenderMaterialMetadata(GetMaterialMetadata(selected_path));
        }
        else if (ImageMetadata::IsSupportedPath(selected_path))
        {
            RenderImageMetadata(selected_path, GetImageMetadata(selected_path), vulkan_context);
        }
        else if (FontMetadata::IsSupportedPath(selected_path))
        {
            RenderFontMetadata(selected_path, GetFontMetadata(selected_path), vulkan_context);
        }
    }
    else
    {
        const int item_count = CountDirectoryItems(selected_path);
        if (item_count >= 0)
        {
            ImGui::Text("Items: %d", item_count);
        }
    }

    ImGui::End();
}

void InfoPanel::RenderSceneGpuPass()
{
    for (auto& renderer_entry : camera_preview_renderers_)
    {
        if (renderer_entry.second)
        {
            renderer_entry.second->RenderGpu();
        }
    }
}