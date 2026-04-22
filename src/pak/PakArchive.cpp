#include "pak/PakArchive.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

namespace
{
constexpr char kMagic[4] = {'E', 'P', 'A', 'K'};
constexpr std::uint32_t kVersion = 1;

std::string NormalizeRelPath(std::string rel_path)
{
    std::replace(rel_path.begin(), rel_path.end(), '\\', '/');
    // Strip any leading separator
    while (!rel_path.empty() && rel_path.front() == '/')
    {
        rel_path.erase(rel_path.begin());
    }
    return rel_path;
}
} // namespace

// ---- Writing ----------------------------------------------------------------

bool PakArchive::AddFile(const std::string& rel_path, const std::filesystem::path& source_path)
{
    std::ifstream input(source_path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    input.seekg(0, std::ios::end);
    const auto file_size = static_cast<std::size_t>(input.tellg());
    input.seekg(0, std::ios::beg);

    Entry entry;
    entry.rel_path = NormalizeRelPath(rel_path);
    entry.data.resize(file_size);
    if (file_size > 0 && !input.read(reinterpret_cast<char*>(entry.data.data()), static_cast<std::streamsize>(file_size)))
    {
        return false;
    }

    write_entries_.push_back(std::move(entry));
    return true;
}

bool PakArchive::AddBuffer(const std::string& rel_path, const std::vector<std::uint8_t>& data)
{
    Entry entry;
    entry.rel_path = NormalizeRelPath(rel_path);
    entry.data = data;
    write_entries_.push_back(std::move(entry));
    return true;
}

bool PakArchive::Write(const std::filesystem::path& output_path) const
{
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }

    // Calculate header + table size so we know the data offset start.
    // Header: magic(4) + version(4) + entry_count(4) = 12 bytes
    // Per entry: path_length(4) + path(N) + offset(8) + size(8) = 20 + N bytes

    std::uint64_t data_section_start = 12;
    for (const Entry& entry : write_entries_)
    {
        data_section_start += 4 + entry.rel_path.size() + 8 + 8;
    }

    // Write magic
    output.write(kMagic, 4);

    // Write version
    const std::uint32_t version = kVersion;
    output.write(reinterpret_cast<const char*>(&version), 4);

    // Write entry count
    const std::uint32_t entry_count = static_cast<std::uint32_t>(write_entries_.size());
    output.write(reinterpret_cast<const char*>(&entry_count), 4);

    // Write table (calculate offsets as we go)
    std::uint64_t current_offset = data_section_start;
    for (const Entry& entry : write_entries_)
    {
        const std::uint32_t path_length = static_cast<std::uint32_t>(entry.rel_path.size());
        output.write(reinterpret_cast<const char*>(&path_length), 4);
        output.write(entry.rel_path.data(), path_length);

        output.write(reinterpret_cast<const char*>(&current_offset), 8);

        const std::uint64_t data_size = static_cast<std::uint64_t>(entry.data.size());
        output.write(reinterpret_cast<const char*>(&data_size), 8);

        current_offset += data_size;
    }

    // Write data blocks
    for (const Entry& entry : write_entries_)
    {
        if (!entry.data.empty())
        {
            output.write(reinterpret_cast<const char*>(entry.data.data()), static_cast<std::streamsize>(entry.data.size()));
        }
    }

    return output.good();
}

void PakArchive::Clear()
{
    write_entries_.clear();
    is_open_ = false;
    pak_path_.clear();
    read_entries_.clear();
}

// ---- Reading ----------------------------------------------------------------

bool PakArchive::Open(const std::filesystem::path& pak_path)
{
    is_open_ = false;
    read_entries_.clear();

    std::ifstream input(pak_path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    // Read and validate magic
    char magic[4];
    input.read(magic, 4);
    if (!input || std::memcmp(magic, kMagic, 4) != 0)
    {
        return false;
    }

    // Read and validate version
    std::uint32_t version = 0;
    input.read(reinterpret_cast<char*>(&version), 4);
    if (!input || version != kVersion)
    {
        return false;
    }

    // Read entry count
    std::uint32_t entry_count = 0;
    input.read(reinterpret_cast<char*>(&entry_count), 4);
    if (!input)
    {
        return false;
    }

    // Read table
    read_entries_.reserve(entry_count);
    for (std::uint32_t index = 0; index < entry_count; ++index)
    {
        std::uint32_t path_length = 0;
        input.read(reinterpret_cast<char*>(&path_length), 4);
        if (!input || path_length == 0 || path_length > 4096)
        {
            return false;
        }

        IndexEntry entry;
        entry.rel_path.resize(path_length);
        input.read(entry.rel_path.data(), path_length);
        if (!input)
        {
            return false;
        }

        input.read(reinterpret_cast<char*>(&entry.offset), 8);
        input.read(reinterpret_cast<char*>(&entry.size), 8);
        if (!input)
        {
            return false;
        }

        read_entries_.push_back(std::move(entry));
    }

    pak_path_ = pak_path;
    is_open_ = true;
    return true;
}

bool PakArchive::Contains(const std::string& rel_path) const
{
    const std::string normalized = NormalizeRelPath(rel_path);
    for (const IndexEntry& entry : read_entries_)
    {
        if (entry.rel_path == normalized)
        {
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> PakArchive::ReadEntry(const std::string& rel_path) const
{
    if (!is_open_)
    {
        return {};
    }

    const std::string normalized = NormalizeRelPath(rel_path);
    const IndexEntry* found = nullptr;
    for (const IndexEntry& entry : read_entries_)
    {
        if (entry.rel_path == normalized)
        {
            found = &entry;
            break;
        }
    }

    if (found == nullptr || found->size == 0)
    {
        return {};
    }

    std::ifstream input(pak_path_, std::ios::binary);
    if (!input)
    {
        return {};
    }

    input.seekg(static_cast<std::streamoff>(found->offset));
    if (!input)
    {
        return {};
    }

    std::vector<std::uint8_t> data(static_cast<std::size_t>(found->size));
    input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(found->size));
    if (!input)
    {
        return {};
    }

    return data;
}

bool PakArchive::ExtractAll(const std::filesystem::path& output_dir) const
{
    if (!is_open_)
    {
        return false;
    }

    bool all_ok = true;
    for (const IndexEntry& entry : read_entries_)
    {
        const std::filesystem::path dest = output_dir / entry.rel_path;

        std::error_code error;
        std::filesystem::create_directories(dest.parent_path(), error);
        if (error)
        {
            all_ok = false;
            continue;
        }

        const std::vector<std::uint8_t> data = ReadEntry(entry.rel_path);
        if (data.empty() && entry.size > 0)
        {
            all_ok = false;
            continue;
        }

        std::ofstream output(dest, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            all_ok = false;
            continue;
        }

        if (!data.empty())
        {
            output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        if (!output.good())
        {
            all_ok = false;
        }
    }

    return all_ok;
}

std::vector<std::string> PakArchive::ListEntries() const
{
    std::vector<std::string> result;
    result.reserve(read_entries_.size());
    for (const IndexEntry& entry : read_entries_)
    {
        result.push_back(entry.rel_path);
    }
    return result;
}
