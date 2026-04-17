#pragma once

#include "assets/AssetMetadata.h"
#include "assets/ModelAsset.h"
#include "assets/ModelMetadata.h"
#include "assets/SceneMetadata.h"
#include "render/FontInfoRenderer.h"
#include "render/SceneViewportRenderer.h"
#include "render/ImageInfoRenderer.h"
#include "state/EngineState.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

class InfoPanel
{
public:
    ~InfoPanel();

    bool InitializeSceneRenderer(VulkanContext* context);
    void BeginFrame();
    void Render(EngineState& state, VulkanContext* vulkan_context);
    void RenderSceneGpuPass();
    void Shutdown();

private:
    struct CachedModelAssetEntry
    {
        std::filesystem::file_time_type write_time{};
        ModelAsset asset{};
    };

    const ModelMetadata& GetModelMetadata(const std::filesystem::path& path);
    const CachedModelAssetEntry& GetModelAssetEntry(const std::filesystem::path& path);
    const ParsedMaterialMetadata& GetMaterialMetadata(const std::filesystem::path& path);
    const SceneMetadata& GetSceneMetadata(const std::filesystem::path& path);
    const ImageMetadata& GetImageMetadata(const std::filesystem::path& path);
    const FontMetadata& GetFontMetadata(const std::filesystem::path& path);
    SceneViewportRenderer* GetCameraPreviewRenderer(const std::filesystem::path& scene_path, const std::string& object_name, std::size_t attribute_index);
    void ClearCameraPreviewRenderers();
    void RenderCameraAttributePreview(
        const SceneMetadata& scene_metadata,
        EngineState& state,
        const SceneObjectMetadata& object,
        std::size_t attribute_index,
        const SceneObjectAttribute& attribute);
    bool HandleSceneObjectAttachmentDrop(EngineState& state, const std::filesystem::path& scene_path, const std::string& object_name);
    void RenderSelectedSceneObject(EngineState& state);
    void RenderMaterialMetadata(const ParsedMaterialMetadata& metadata) const;
    void RenderImageMetadata(const std::filesystem::path& path, const ImageMetadata& metadata, VulkanContext* vulkan_context);
    void RenderFontMetadata(const std::filesystem::path& path, const FontMetadata& metadata, VulkanContext* vulkan_context);
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
    std::filesystem::path cached_image_path_;
    std::filesystem::file_time_type cached_image_write_time_{};
    ImageMetadata cached_image_metadata_{};
    bool has_cached_image_metadata_ = false;
    std::filesystem::path cached_font_path_;
    std::filesystem::file_time_type cached_font_write_time_{};
    FontMetadata cached_font_metadata_{};
    bool has_cached_font_metadata_ = false;
    VulkanContext* preview_vulkan_context_ = nullptr;
    std::string active_camera_preview_scope_;
    std::unordered_map<std::filesystem::path, CachedModelAssetEntry> model_asset_cache_;
    std::unordered_map<std::string, std::unique_ptr<SceneViewportRenderer>> camera_preview_renderers_;
    ImageInfoRenderer image_info_renderer_{};
    FontInfoRenderer font_info_renderer_{};
    float font_preview_size_pixels_ = 34.0f;
};