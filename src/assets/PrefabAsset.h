#pragma once

#include "assets/SceneMetadata.h"

#include <filesystem>
#include <string>
#include <vector>

// Single-file prefab store ("Assets/Prefabs/Prefabs.metadata").
// Holds an arbitrary number of named prefab entries. Each entry contains the
// raw text lines describing the prefab object subtree, using the same syntax
// as scene files (Object: / Position: / Attributes: / ...). The raw lines are
// kept verbatim so prefabs can be re-injected into target scenes without
// re-serializing every attribute kind.

struct PrefabEntry
{
    std::string name;             // unique identifier (drag-drop key, display label)
    std::string root_object_name; // first "Object:" name inside this prefab
    // Raw line block belonging to this prefab (excluding the "Prefab:" header).
    // Contains one or more "Object:" blocks. Appended verbatim into the target
    // scene when instantiating, with names uniquified at that point.
    std::vector<std::string> body_lines;
};

struct PrefabMetadata
{
    bool parsed = false;
    std::string error_message;
    std::vector<PrefabEntry> prefabs;
};

bool IsPrefabMetadataFile(const std::filesystem::path& path);

std::filesystem::path GetPrefabsDirectory(const std::filesystem::path& project_root);
std::filesystem::path GetPrefabMetadataPath(const std::filesystem::path& project_root);

// Ensures Assets/Prefabs exists. Returns true iff the directory exists after the call.
bool EnsurePrefabsDirectoryExists(const std::filesystem::path& project_root);

// Loads the prefab store. Missing file -> parsed=true with empty prefabs.
PrefabMetadata LoadPrefabMetadata(const std::filesystem::path& project_root);

// Writes the store to disk, creating Assets/Prefabs if needed.
bool SavePrefabMetadata(const std::filesystem::path& project_root, const PrefabMetadata& metadata);

// Adds the given scene object (and its descendants) as a new prefab entry.
// The prefab name is uniquified within the store. The chosen name is written
// to out_prefab_name on success.
bool AddSceneObjectAsPrefab(
    const std::filesystem::path& project_root,
    const std::filesystem::path& source_scene_path,
    const std::string& object_name,
    std::string* out_prefab_name = nullptr);

// Removes a prefab entry by name. Returns true if an entry was removed.
bool RemovePrefabByName(const std::filesystem::path& project_root, const std::string& prefab_name);

// Instantiates the named prefab into the target scene, optionally under a
// parent object (empty string => scene root). Object names from the prefab are
// uniquified within the destination scene. On success, the resulting root
// object name is written to instantiated_root_name when non-null.
bool InstantiatePrefabIntoScene(
    const std::filesystem::path& project_root,
    const std::string& prefab_name,
    const std::filesystem::path& target_scene_path,
    const std::string& parent_object_name,
    std::string* instantiated_root_name = nullptr);

// Parses the named prefab's body into a synthetic single-object scene and
// returns the root object's full SceneObjectMetadata (transform + every
// attribute kind). Returns true and fills out_object on success. Used by
// the runtime to spawn prefabs with their attributes intact (clouds, lights,
// 2D overlays, audio sources, etc.).
bool LoadPrefabRootMetadata(
    const std::filesystem::path& project_root,
    const std::string& prefab_name,
    SceneObjectMetadata* out_object,
    std::string* out_error = nullptr);
