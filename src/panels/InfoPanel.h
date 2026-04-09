#pragma once

#include "assets/AssetMetadata.h"
#include "assets/ModelMetadata.h"
#include "assets/SceneMetadata.h"
#include "state/EngineState.h"

#include <SDL3/SDL.h>

#include <filesystem>

class InfoPanel
{
public:
    ~InfoPanel();

    void Render(EngineState& state, SDL_Renderer* renderer);
    void Shutdown();

private:
    const ModelMetadata& GetModelMetadata(const std::filesystem::path& path);
    const ParsedMaterialMetadata& GetMaterialMetadata(const std::filesystem::path& path);
    const SceneMetadata& GetSceneMetadata(const std::filesystem::path& path);
    const TextureMetadata& GetTextureMetadata(const std::filesystem::path& path);
    bool HandleSceneObjectAttachmentDrop(EngineState& state, const std::filesystem::path& scene_path, const std::string& object_name, std::string_view attachment_kind);
    void RenderSelectedSceneObject(EngineState& state);
    SDL_Texture* GetTexturePreview(const std::filesystem::path& path, SDL_Renderer* renderer);
    void ClearTexturePreview();
    void RenderMaterialMetadata(const ParsedMaterialMetadata& metadata) const;
    void RenderTextureMetadata(const std::filesystem::path& path, const TextureMetadata& metadata, SDL_Renderer* renderer);
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
    std::filesystem::path cached_texture_preview_path_;
    std::filesystem::file_time_type cached_texture_preview_write_time_{};
    SDL_Texture* cached_texture_preview_ = nullptr;
    int cached_texture_preview_width_ = 0;
    int cached_texture_preview_height_ = 0;
};