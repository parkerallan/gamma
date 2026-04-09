#include "panels/InfoPanel.h"

#include "imgui.h"
#include <stb_image.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace
{
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
}

InfoPanel::~InfoPanel()
{
    Shutdown();
}

void InfoPanel::Shutdown()
{
    ClearTexturePreview();
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

const TextureMetadata& InfoPanel::GetTextureMetadata(const std::filesystem::path& path)
{
    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool cache_valid = has_cached_texture_metadata_ && cached_texture_path_ == path && !error && cached_texture_write_time_ == write_time;
    if (cache_valid)
    {
        return cached_texture_metadata_;
    }

    cached_texture_path_ = path;
    cached_texture_write_time_ = error ? std::filesystem::file_time_type::min() : write_time;
    cached_texture_metadata_ = LoadTextureMetadata(path);
    has_cached_texture_metadata_ = true;
    return cached_texture_metadata_;
}

void InfoPanel::ClearTexturePreview()
{
    if (cached_texture_preview_ != nullptr)
    {
        SDL_DestroyTexture(cached_texture_preview_);
        cached_texture_preview_ = nullptr;
    }

    cached_texture_preview_path_.clear();
    cached_texture_preview_write_time_ = std::filesystem::file_time_type{};
    cached_texture_preview_width_ = 0;
    cached_texture_preview_height_ = 0;
}

SDL_Texture* InfoPanel::GetTexturePreview(const std::filesystem::path& path, SDL_Renderer* renderer)
{
    if (renderer == nullptr)
    {
        ClearTexturePreview();
        return nullptr;
    }

    std::error_code error;
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(path, error);
    const bool cache_valid = cached_texture_preview_ != nullptr && cached_texture_preview_path_ == path && !error && cached_texture_preview_write_time_ == write_time;
    if (cache_valid)
    {
        return cached_texture_preview_;
    }

    ClearTexturePreview();

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        return nullptr;
    }

    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    if (texture == nullptr)
    {
        stbi_image_free(pixels);
        return nullptr;
    }

    const bool updated = SDL_UpdateTexture(texture, nullptr, pixels, width * 4);
    stbi_image_free(pixels);
    if (!updated)
    {
        SDL_DestroyTexture(texture);
        return nullptr;
    }

    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    cached_texture_preview_ = texture;
    cached_texture_preview_path_ = path;
    cached_texture_preview_write_time_ = error ? std::filesystem::file_time_type::min() : write_time;
    cached_texture_preview_width_ = width;
    cached_texture_preview_height_ = height;
    return cached_texture_preview_;
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

void InfoPanel::RenderTextureMetadata(const std::filesystem::path& path, const TextureMetadata& metadata, SDL_Renderer* renderer)
{
    ImGui::Spacing();
    ImGui::SeparatorText("Texture");

    if (!metadata.parsed)
    {
        ImGui::TextUnformatted("Unable to parse texture metadata.");
        if (!metadata.error_message.empty())
        {
            ImGui::TextWrapped("Reason: %s", metadata.error_message.c_str());
        }
        return;
    }

    if (SDL_Texture* preview_texture = GetTexturePreview(path, renderer))
    {
        const float available_width = ImGui::GetContentRegionAvail().x;
        const float max_preview_width = available_width > 0.0f ? available_width : 220.0f;
        const float max_preview_height = 220.0f;
        const float width_scale = max_preview_width / static_cast<float>(cached_texture_preview_width_);
        const float height_scale = max_preview_height / static_cast<float>(cached_texture_preview_height_);
        const float scale = (std::min)(1.0f, (std::min)(width_scale, height_scale));
        const ImVec2 preview_size(
            static_cast<float>(cached_texture_preview_width_) * scale,
            static_cast<float>(cached_texture_preview_height_) * scale);

        ImGui::Image(reinterpret_cast<ImTextureID>(preview_texture), preview_size);
        ImGui::Spacing();
    }

    ImGui::Text("Resolution: %d x %d", metadata.width, metadata.height);
    ImGui::Text("Channels: %d", metadata.channel_count);
    ImGui::Text("Bits/channel: %d", metadata.bits_per_channel);
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
                    }
                    if (material.has_opacity)
                    {
                        ImGui::Text("Opacity: %.2f", material.opacity);
                    }
                    if (material.has_roughness)
                    {
                        ImGui::Text("Roughness: %.2f", material.roughness);
                    }
                    if (material.has_metalness)
                    {
                        ImGui::Text("Metalness: %.2f", material.metalness);
                    }

                    if ((material.base_color.valid || material.emissive_color.valid || material.has_opacity || material.has_roughness || material.has_metalness) && !material.textures.empty())
                    {
                        ImGui::Separator();
                    }

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
                ImGui::Text("Duration: %.2f", animation.duration);
                ImGui::Text("Ticks/sec: %.2f", animation.ticks_per_second);
                ImGui::Text("Channels: %u", animation.channel_count);
                ImGui::Spacing();
            }
        }
    }
}

void InfoPanel::Render(EngineState& state, SDL_Renderer* renderer)
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

    ImGui::TextUnformatted(selected_path.filename().string().c_str());
    ImGui::Separator();
    ImGui::Text("Type: %s", is_directory ? "Folder" : "File");
    ImGui::TextWrapped("Path: %s", state.GetSelectedItemDisplayPath().c_str());
    if (state.HasSelectedSceneObject())
    {
        ImGui::Text("Scene Object: %s", state.selected_scene_object_name.c_str());
    }

    if (!is_directory)
    {
        const std::string extension = selected_path.has_extension() ? selected_path.extension().string() : std::string("None");
        std::error_code size_error;
        const std::uintmax_t file_size = std::filesystem::file_size(selected_path, size_error);
        ImGui::Text("Extension: %s", extension.c_str());
        if (!size_error)
        {
            ImGui::Text("Size: %s", FormatFileSize(file_size).c_str());
        }

        if (ModelMetadata::IsSupportedModelPath(selected_path))
        {
            RenderModelMetadata(GetModelMetadata(selected_path));
        }
        else if (ParsedMaterialMetadata::IsSupportedPath(selected_path))
        {
            RenderMaterialMetadata(GetMaterialMetadata(selected_path));
        }
        else if (TextureMetadata::IsSupportedPath(selected_path))
        {
            RenderTextureMetadata(selected_path, GetTextureMetadata(selected_path), renderer);
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