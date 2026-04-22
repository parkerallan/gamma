#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// PAK archive format:
//   Magic:      char[4]  = "EPAK"
//   Version:    uint32_t = 1
//   EntryCount: uint32_t
//   [Table: EntryCount entries]
//     PathLength: uint32_t
//     Path:       char[PathLength]   (relative, forward-slash, no leading /)
//     Offset:     uint64_t           (byte offset into file of this entry's data)
//     DataSize:   uint64_t
//   [Data blocks at their respective offsets]

class PakArchive
{
public:
    // ---- Writing ------------------------------------------------------------

    // Add a file from the filesystem.  rel_path uses forward slashes and is
    // relative to the project root (e.g. "Scenes/Main.scene").
    bool AddFile(const std::string& rel_path, const std::filesystem::path& source_path);

    // Add a file from an in-memory buffer.
    bool AddBuffer(const std::string& rel_path, const std::vector<std::uint8_t>& data);

    // Write all added files to the given output path.
    bool Write(const std::filesystem::path& output_path) const;

    // Remove all entries (reset writer state).
    void Clear();

    // ---- Reading ------------------------------------------------------------

    // Open a PAK file for reading.
    bool Open(const std::filesystem::path& pak_path);

    // Returns true if the archive is open.
    bool IsOpen() const { return is_open_; }

    // Returns true if an entry with the given relative path exists.
    bool Contains(const std::string& rel_path) const;

    // Read the contents of an entry into a byte buffer.  Returns empty on
    // error or if the entry does not exist.
    std::vector<std::uint8_t> ReadEntry(const std::string& rel_path) const;

    // Extract all entries to the given directory, preserving the relative
    // path hierarchy.  Returns false if any entry fails to extract.
    bool ExtractAll(const std::filesystem::path& output_dir) const;

    // Returns the list of all stored relative paths.
    std::vector<std::string> ListEntries() const;

private:
    struct Entry
    {
        std::string rel_path;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
        std::vector<std::uint8_t> data; // populated during write phase
    };

    struct IndexEntry
    {
        std::string rel_path;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };

    // Writer state
    std::vector<Entry> write_entries_;

    // Reader state
    bool is_open_ = false;
    std::filesystem::path pak_path_;
    std::vector<IndexEntry> read_entries_;
};
