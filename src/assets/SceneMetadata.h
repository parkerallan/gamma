#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

using SceneVector3 = std::array<float, 3>;

struct SceneObjectMetadata
{
    std::string name;
    std::string type;
    SceneVector3 position = {0.0f, 0.0f, 0.0f};
    SceneVector3 rotation = {0.0f, 0.0f, 0.0f};
    SceneVector3 scale = {1.0f, 1.0f, 1.0f};
    std::string model_path;
    std::vector<std::string> script_paths;
    std::vector<std::string> graph_paths;
};

struct SceneMetadata
{
    bool parsed = false;
    std::string error_message;
    std::string scene_name;
    std::vector<SceneObjectMetadata> objects;
};

SceneMetadata LoadSceneMetadata(const std::filesystem::path& scene_path);
bool SetSceneObjectPosition(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& position);
bool SetSceneObjectRotation(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& rotation);
bool SetSceneObjectScale(const std::filesystem::path& scene_path, const std::string& object_name, const SceneVector3& scale);
bool SetSceneObjectModel(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& model_path);
bool ClearSceneObjectModel(const std::filesystem::path& scene_path, const std::string& object_name);
bool AddSceneObjectScript(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& script_path);
bool RemoveSceneObjectScript(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& script_path);
bool AddSceneObjectGraph(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& graph_path);
bool RemoveSceneObjectGraph(const std::filesystem::path& scene_path, const std::string& object_name, const std::filesystem::path& project_root, const std::filesystem::path& graph_path);