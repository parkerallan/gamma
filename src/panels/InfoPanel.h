#pragma once

#include "assets/AssetMetadata.h"
#include "assets/ModelMetadata.h"
#include "assets/SceneMetadata.h"
#include "panels/TextureInfoRenderer.h"
#include "state/EngineState.h"

#include <filesystem>

class InfoPanel
{
public:
    ~InfoPanel();

    void Render(EngineState& state, VulkanContext* vulkan_context);
    void Shutdown();

private:
    const ModelMetadata& GetModelMetadata(const std::filesystem::path& path);
    const ParsedMaterialMetadata& GetMaterialMetadata(const std::filesystem::path& path);
    const SceneMetadata& GetSceneMetadata(const std::filesystem::path& path);
    const TextureMetadata& GetTextureMetadata(const std::filesystem::path& path);
    bool HandleSceneObjectAttachmentDrop(EngineState& state, const std::filesystem::path& scene_path, const std::string& object_name);
    void RenderSelectedSceneObject(EngineState& state);
    void RenderMaterialMetadata(const ParsedMaterialMetadata& metadata) const;
    void RenderTextureMetadata(const std::filesystem::path& path, const TextureMetadata& metadata, VulkanContext* vulkan_context);
    void RenderModelMetadata(const ModelMetadata& metadata) const;

    std::filesystem::path cached_model_path_;
    std::filesystem::file_time_type cached_model_write_time_{};
    ModelMetadata cached_model_metadata_{};
    bool has_cached_model_metadata_ = false;
    std::filesystem::path cached_material_path_;
    std::filesystem::file_time_type cached_material_write_time_{};
    ParsedMaterialMetadata cached_material_metadata_{};
    bool has_cached_material_metadata_ = false;
    std::filesystem::path cached_scene_path_;
    std::filesystem::file_time_type cached_scene_write_time_{};
    SceneMetadata cached_scene_metadata_{};
    bool has_cached_scene_metadata_ = false;
    std::filesystem::path cached_texture_path_;
    std::filesystem::file_time_type cached_texture_write_time_{};
    TextureMetadata cached_texture_metadata_{};
    bool has_cached_texture_metadata_ = false;
    TextureInfoRenderer texture_info_renderer_{};
};