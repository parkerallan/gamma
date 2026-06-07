#include "assets/PrefabAsset.h"

#include "assets/SceneMetadata.h"
#include "vfs/AssetVFS.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <system_error>

namespace
{
constexpr const char* kPrefabMetadataFilename = "Prefabs.metadata";

std::string TrimCopy(std::string value)
{
    const auto is_space = [](unsigned char character)
    {
        return std::isspace(character) != 0;
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char c)
    {
        return !is_space(c);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char c)
    {
        return !is_space(c);
    }).base(), value.end());
    return value;
}

bool StartsWith(std::string_view value, std::string_view prefix)
{
    return value.rfind(prefix, 0) == 0;
}

std::string ExtractValue(std::string_view line, std::string_view prefix)
{
    return TrimCopy(std::string(line.substr(prefix.size())));
}

std::vector<std::string> ReadAllLines(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return {};
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

// Splits a raw byte buffer into lines (handles \r\n and \n).
std::vector<std::string> SplitBufferIntoLines(const std::vector<std::uint8_t>& buffer)
{
    std::vector<std::string> lines;
    std::string current;
    current.reserve(64);
    for (unsigned char byte : buffer)
    {
        if (byte == '\n')
        {
            lines.push_back(current);
            current.clear();
        }
        else if (byte != '\r')
        {
            current.push_back(static_cast<char>(byte));
        }
    }
    if (!current.empty())
    {
        lines.push_back(current);
    }
    return lines;
}

// Canonical pak-relative key for the single-file prefab store.
constexpr const char* kPrefabMetadataPakKey = "Assets/Prefabs/Prefabs.metadata";

// Reads the prefab metadata file's lines, preferring the VFS asset reader
// (built games stream from assets.pak) and falling back to disk for the
// editor / play-mode case where project_root is a real directory.
std::vector<std::string> ReadPrefabMetadataLines(const std::filesystem::path& project_root)
{
    if (g_asset_reader)
    {
        auto try_vfs = [](const std::string& key) -> std::vector<std::string>
        {
            const auto buffer = g_asset_reader->ReadFile(key);
            if (buffer.empty())
            {
                return {};
            }
            return SplitBufferIntoLines(buffer);
        };

        // Try the absolute (editor) path first if available; the VFS strips
        // drive letters and "content/" prefixes so the same pak key resolves.
        if (!project_root.empty())
        {
            const std::filesystem::path abs_path =
                project_root / "Assets" / "Prefabs" / "Prefabs.metadata";
            std::vector<std::string> lines = try_vfs(abs_path.generic_string());
            if (!lines.empty())
            {
                return lines;
            }
        }

        // Pak-relative key (this is how built games store it).
        std::vector<std::string> lines = try_vfs(kPrefabMetadataPakKey);
        if (!lines.empty())
        {
            return lines;
        }
    }

    if (project_root.empty())
    {
        return {};
    }

    const std::filesystem::path disk_path =
        project_root / "Assets" / "Prefabs" / "Prefabs.metadata";
    std::error_code ec;
    if (!std::filesystem::exists(disk_path, ec))
    {
        return {};
    }
    return ReadAllLines(disk_path);
}

bool WriteAllLines(const std::filesystem::path& path, const std::vector<std::string>& lines)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        output << lines[i];
        if (i + 1 < lines.size())
        {
            output << '\n';
        }
    }
    return static_cast<bool>(output);
}

struct ObjectBlock
{
    std::string name;
    std::size_t start = 0;  // index of "Object:" line
    std::size_t end = 0;    // one past last line of block
};

std::vector<ObjectBlock> CollectObjectBlocks(const std::vector<std::string>& lines)
{
    std::vector<ObjectBlock> blocks;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string trimmed = TrimCopy(lines[i]);
        if (!StartsWith(trimmed, "Object:"))
        {
            continue;
        }
        ObjectBlock block;
        block.name = ExtractValue(trimmed, "Object:");
        block.start = i;
        block.end = lines.size();
        for (std::size_t j = i + 1; j < lines.size(); ++j)
        {
            if (StartsWith(TrimCopy(lines[j]), "Object:"))
            {
                block.end = j;
                break;
            }
        }
        blocks.push_back(block);
    }
    return blocks;
}

// Collects names of object_name and all its descendants in the order they
// appear in the scene metadata (parents before children).
void CollectSubtreeNames(
    const SceneMetadata& scene,
    const std::string& root_name,
    std::vector<std::string>& out_names)
{
    out_names.push_back(root_name);
    for (const SceneObjectMetadata& object : scene.objects)
    {
        if (object.parent_name == root_name)
        {
            CollectSubtreeNames(scene, object.name, out_names);
        }
    }
}

bool NameExists(const std::vector<std::string>& reserved, const std::string& name)
{
    return std::find(reserved.begin(), reserved.end(), name) != reserved.end();
}

std::string BuildUniqueName(
    const SceneMetadata& target_scene,
    const std::string& desired,
    const std::vector<std::string>& reserved)
{
    auto taken = [&](const std::string& candidate) -> bool
    {
        if (candidate.empty())
        {
            return true;
        }
        for (const SceneObjectMetadata& object : target_scene.objects)
        {
            if (object.name == candidate)
            {
                return true;
            }
        }
        return NameExists(reserved, candidate);
    };

    if (!taken(desired))
    {
        return desired;
    }
    for (int suffix = 1; suffix < 100000; ++suffix)
    {
        const std::string candidate = desired + std::to_string(suffix);
        if (!taken(candidate))
        {
            return candidate;
        }
    }
    return {};
}

std::string UniquePrefabName(const PrefabMetadata& store, const std::string& desired)
{
    auto taken = [&](const std::string& candidate) -> bool
    {
        if (candidate.empty())
        {
            return true;
        }
        for (const PrefabEntry& entry : store.prefabs)
        {
            if (entry.name == candidate)
            {
                return true;
            }
        }
        return false;
    };

    const std::string base = desired.empty() ? std::string("Prefab") : desired;
    if (!taken(base))
    {
        return base;
    }
    for (int suffix = 1; suffix < 100000; ++suffix)
    {
        const std::string candidate = base + "_" + std::to_string(suffix);
        if (!taken(candidate))
        {
            return candidate;
        }
    }
    return {};
}
} // namespace

bool IsPrefabMetadataFile(const std::filesystem::path& path)
{
    return path.filename() == kPrefabMetadataFilename;
}

std::filesystem::path GetPrefabsDirectory(const std::filesystem::path& project_root)
{
    if (project_root.empty())
    {
        return {};
    }
    return project_root / "Assets" / "Prefabs";
}

std::filesystem::path GetPrefabMetadataPath(const std::filesystem::path& project_root)
{
    const std::filesystem::path directory = GetPrefabsDirectory(project_root);
    if (directory.empty())
    {
        return {};
    }
    return directory / kPrefabMetadataFilename;
}

bool EnsurePrefabsDirectoryExists(const std::filesystem::path& project_root)
{
    const std::filesystem::path directory = GetPrefabsDirectory(project_root);
    if (directory.empty())
    {
        return false;
    }

    std::error_code error;
    if (std::filesystem::exists(directory, error))
    {
        return std::filesystem::is_directory(directory, error);
    }
    error.clear();
    std::filesystem::create_directories(directory, error);
    return !error;
}

PrefabMetadata LoadPrefabMetadata(const std::filesystem::path& project_root)
{
    PrefabMetadata metadata;

    const std::vector<std::string> lines = ReadPrefabMetadataLines(project_root);
    if (lines.empty())
    {
        // Empty / missing store is not an error; callers treat zero
        // entries as "no prefabs available".
        metadata.parsed = true;
        return metadata;
    }

    PrefabEntry* current = nullptr;
    for (const std::string& raw_line : lines)
    {
        const std::string trimmed = TrimCopy(raw_line);
        if (StartsWith(trimmed, "Prefab:"))
        {
            PrefabEntry entry;
            entry.name = ExtractValue(trimmed, "Prefab:");
            metadata.prefabs.push_back(std::move(entry));
            current = &metadata.prefabs.back();
            continue;
        }

        if (current == nullptr)
        {
            continue;
        }

        // Capture every non-header line verbatim (including blank separators
        // and indentation). Track the first "Object:" line as the root name.
        if (current->root_object_name.empty() && StartsWith(trimmed, "Object:"))
        {
            current->root_object_name = ExtractValue(trimmed, "Object:");
        }
        current->body_lines.push_back(raw_line);
    }

    // Strip trailing blank lines from every entry for cleaner round-trips.
    for (PrefabEntry& entry : metadata.prefabs)
    {
        while (!entry.body_lines.empty() && TrimCopy(entry.body_lines.back()).empty())
        {
            entry.body_lines.pop_back();
        }
    }

    metadata.parsed = true;
    return metadata;
}

bool SavePrefabMetadata(const std::filesystem::path& project_root, const PrefabMetadata& metadata)
{
    if (!EnsurePrefabsDirectoryExists(project_root))
    {
        return false;
    }
    const std::filesystem::path path = GetPrefabMetadataPath(project_root);
    if (path.empty())
    {
        return false;
    }

    std::vector<std::string> out;
    for (std::size_t i = 0; i < metadata.prefabs.size(); ++i)
    {
        const PrefabEntry& entry = metadata.prefabs[i];
        out.push_back("Prefab: " + entry.name);
        for (const std::string& line : entry.body_lines)
        {
            out.push_back(line);
        }
        if (i + 1 < metadata.prefabs.size())
        {
            out.push_back(std::string());
        }
    }

    return WriteAllLines(path, out);
}

bool AddSceneObjectAsPrefab(
    const std::filesystem::path& project_root,
    const std::filesystem::path& source_scene_path,
    const std::string& object_name,
    std::string* out_prefab_name)
{
    if (project_root.empty() || object_name.empty())
    {
        return false;
    }

    const SceneMetadata source = LoadSceneMetadata(source_scene_path);
    if (!source.parsed)
    {
        return false;
    }
    const auto object_it = std::find_if(source.objects.begin(), source.objects.end(),
        [&](const SceneObjectMetadata& obj) { return obj.name == object_name; });
    if (object_it == source.objects.end())
    {
        return false;
    }

    std::vector<std::string> subtree;
    CollectSubtreeNames(source, object_name, subtree);
    if (subtree.empty())
    {
        return false;
    }

    const std::vector<std::string> scene_lines = ReadAllLines(source_scene_path);
    const std::vector<ObjectBlock> blocks = CollectObjectBlocks(scene_lines);
    if (blocks.empty())
    {
        return false;
    }

    PrefabMetadata store = LoadPrefabMetadata(project_root);
    if (!store.parsed)
    {
        store = PrefabMetadata{};
        store.parsed = true;
    }

    PrefabEntry entry;
    entry.name = UniquePrefabName(store, object_name);
    if (entry.name.empty())
    {
        return false;
    }
    entry.root_object_name = object_name;

    for (const std::string& sub_name : subtree)
    {
        const auto block_it = std::find_if(blocks.begin(), blocks.end(),
            [&](const ObjectBlock& b) { return b.name == sub_name; });
        if (block_it == blocks.end())
        {
            continue;
        }
        const bool is_root = sub_name == object_name;
        for (std::size_t i = block_it->start; i < block_it->end; ++i)
        {
            const std::string& line = scene_lines[i];
            const std::string trimmed = TrimCopy(line);
            // Drop the root's Parent: link; inside the prefab the root has no parent.
            if (is_root && StartsWith(trimmed, "Parent:"))
            {
                continue;
            }
            entry.body_lines.push_back(line);
        }
        if (!entry.body_lines.empty() && !TrimCopy(entry.body_lines.back()).empty())
        {
            entry.body_lines.push_back(std::string());
        }
    }
    while (!entry.body_lines.empty() && TrimCopy(entry.body_lines.back()).empty())
    {
        entry.body_lines.pop_back();
    }

    store.prefabs.push_back(std::move(entry));
    if (!SavePrefabMetadata(project_root, store))
    {
        return false;
    }
    if (out_prefab_name != nullptr)
    {
        *out_prefab_name = store.prefabs.back().name;
    }
    return true;
}

bool RemovePrefabByName(const std::filesystem::path& project_root, const std::string& prefab_name)
{
    PrefabMetadata store = LoadPrefabMetadata(project_root);
    if (!store.parsed)
    {
        return false;
    }
    const auto it = std::find_if(store.prefabs.begin(), store.prefabs.end(),
        [&](const PrefabEntry& entry) { return entry.name == prefab_name; });
    if (it == store.prefabs.end())
    {
        return false;
    }
    store.prefabs.erase(it);
    return SavePrefabMetadata(project_root, store);
}

bool InstantiatePrefabIntoScene(
    const std::filesystem::path& project_root,
    const std::string& prefab_name,
    const std::filesystem::path& target_scene_path,
    const std::string& parent_object_name,
    std::string* instantiated_root_name)
{
    if (target_scene_path.empty() || prefab_name.empty())
    {
        return false;
    }

    const PrefabMetadata store = LoadPrefabMetadata(project_root);
    const auto entry_it = std::find_if(store.prefabs.begin(), store.prefabs.end(),
        [&](const PrefabEntry& entry) { return entry.name == prefab_name; });
    if (entry_it == store.prefabs.end())
    {
        return false;
    }
    const PrefabEntry& entry = *entry_it;
    if (entry.root_object_name.empty() || entry.body_lines.empty())
    {
        return false;
    }

    // Walk the prefab's body lines, gathering per-object blocks and the
    // declared object names in order.
    const std::vector<ObjectBlock> prefab_blocks = CollectObjectBlocks(entry.body_lines);
    if (prefab_blocks.empty())
    {
        return false;
    }
    std::vector<std::string> ordered_names;
    ordered_names.reserve(prefab_blocks.size());
    for (const ObjectBlock& block : prefab_blocks)
    {
        ordered_names.push_back(block.name);
    }

    SceneMetadata target = LoadSceneMetadata(target_scene_path);
    if (!target.parsed)
    {
        return false;
    }
    if (!parent_object_name.empty())
    {
        const bool parent_exists = std::any_of(target.objects.begin(), target.objects.end(),
            [&](const SceneObjectMetadata& obj) { return obj.name == parent_object_name; });
        if (!parent_exists)
        {
            return false;
        }
    }

    // Build a mapping from prefab object names to scene-unique names.
    std::vector<std::pair<std::string, std::string>> name_map;
    std::vector<std::string> reserved;
    name_map.reserve(ordered_names.size());
    for (const std::string& source_name : ordered_names)
    {
        // Prefer the prefab's own name for the root; if it collides, we still
        // uniquify against the destination scene.
        const std::string unique = BuildUniqueName(target, source_name, reserved);
        if (unique.empty())
        {
            return false;
        }
        reserved.push_back(unique);
        name_map.emplace_back(source_name, unique);
    }
    auto map_name = [&](const std::string& source_name) -> std::string
    {
        for (const auto& pair : name_map)
        {
            if (pair.first == source_name)
            {
                return pair.second;
            }
        }
        return source_name;
    };

    std::vector<std::string> target_lines = ReadAllLines(target_scene_path);
    if (target_lines.empty() && !std::filesystem::exists(target_scene_path))
    {
        return false;
    }

    for (const ObjectBlock& block : prefab_blocks)
    {
        if (!target_lines.empty() && !TrimCopy(target_lines.back()).empty())
        {
            target_lines.push_back(std::string());
        }

        const std::string mapped_name = map_name(block.name);
        const bool is_root = block.name == entry.root_object_name;
        bool wrote_parent_line = false;
        for (std::size_t i = block.start; i < block.end; ++i)
        {
            const std::string& line = entry.body_lines[i];
            const std::string trimmed = TrimCopy(line);
            if (StartsWith(trimmed, "Object:"))
            {
                target_lines.push_back("Object: " + mapped_name);
                continue;
            }
            if (StartsWith(trimmed, "Parent:"))
            {
                if (is_root)
                {
                    if (!parent_object_name.empty())
                    {
                        target_lines.push_back("Parent: " + parent_object_name);
                        wrote_parent_line = true;
                    }
                    // else: drop the line so the object becomes a scene root.
                    continue;
                }
                const std::string source_parent = ExtractValue(trimmed, "Parent:");
                target_lines.push_back("Parent: " + map_name(source_parent));
                continue;
            }
            target_lines.push_back(line);
        }

        if (is_root && !parent_object_name.empty() && !wrote_parent_line)
        {
            target_lines.push_back("Parent: " + parent_object_name);
        }
    }

    if (!WriteAllLines(target_scene_path, target_lines))
    {
        return false;
    }
    if (instantiated_root_name != nullptr)
    {
        *instantiated_root_name = map_name(entry.root_object_name);
    }
    return true;
}

bool LoadPrefabRootMetadata(
    const std::filesystem::path& project_root,
    const std::string& prefab_name,
    SceneObjectMetadata* out_object,
    std::string* out_error)
{
    if (out_object == nullptr)
    {
        if (out_error != nullptr)
        {
            *out_error = "out_object is null";
        }
        return false;
    }

    const PrefabMetadata store = LoadPrefabMetadata(project_root);
    if (!store.parsed)
    {
        if (out_error != nullptr)
        {
            *out_error = store.error_message.empty() ? "Failed to load prefab store"
                                                    : store.error_message;
        }
        return false;
    }

    const auto entry_it = std::find_if(store.prefabs.begin(), store.prefabs.end(),
        [&prefab_name](const PrefabEntry& e) { return e.name == prefab_name; });
    if (entry_it == store.prefabs.end())
    {
        if (out_error != nullptr)
        {
            *out_error = "Unknown prefab: " + prefab_name;
        }
        return false;
    }

    // Materialize the prefab body into a temporary single-object scene file
    // and parse it with the full SceneMetadata loader. This guarantees every
    // attribute kind is parsed the same way as an authored scene without
    // duplicating ~600 lines of parser logic here.
    std::error_code ec;
    const std::filesystem::path temp_dir = std::filesystem::temp_directory_path(ec);
    if (ec || temp_dir.empty())
    {
        if (out_error != nullptr)
        {
            *out_error = "Failed to resolve temp directory";
        }
        return false;
    }

    static std::atomic<std::uint64_t> s_counter{0};
    const std::uint64_t suffix = ++s_counter;
    const std::filesystem::path temp_scene =
        temp_dir / (std::string("engine_prefab_") + std::to_string(suffix) + ".scene");

    {
        std::ofstream out(temp_scene, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            if (out_error != nullptr)
            {
                *out_error = "Failed to open temp prefab scene for writing";
            }
            return false;
        }
        out << "Scene: Prefab\n\n";
        for (const std::string& line : entry_it->body_lines)
        {
            out << line << '\n';
        }
    }

    const SceneMetadata parsed = LoadSceneMetadata(temp_scene);
    std::filesystem::remove(temp_scene, ec);

    if (!parsed.parsed || parsed.objects.empty())
    {
        if (out_error != nullptr)
        {
            *out_error = parsed.error_message.empty()
                ? "Prefab body parsed to zero objects"
                : parsed.error_message;
        }
        return false;
    }

    // Find the root object (matches the prefab's recorded root name) or
    // fall back to the first object in declaration order.
    const SceneObjectMetadata* root = nullptr;
    if (!entry_it->root_object_name.empty())
    {
        for (const SceneObjectMetadata& obj : parsed.objects)
        {
            if (obj.name == entry_it->root_object_name)
            {
                root = &obj;
                break;
            }
        }
    }
    if (root == nullptr)
    {
        root = &parsed.objects.front();
    }

    *out_object = *root;
    return true;
}
