#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// PAK archive format, version 2:
//   Header (offset 0, 12 bytes):
//     Magic:      char[4]  = "EPAK"
//     Version:    uint32_t = 2
//     Reserved:   uint32_t = 0     (v1 stored the entry count here)
//   [Data blocks, back-to-back in write order, starting at offset 12]
//   [Index blob, immediately after the last data block]
//     EntryCount: uint32_t
//     [EntryCount entries]
//       PathLength:       uint32_t  (1..4096)
//       Path:             char[N]   (relative, forward-slash, no leading /)
//       Offset:           uint64_t  (absolute byte offset of this entry's data)
//       CompressedSize:   uint64_t  (bytes physically stored)
//       UncompressedSize: uint64_t  (== CompressedSize when stored raw)
//       Method:           uint32_t  (0 = store, 1 = zstd)
//   Footer (last 20 bytes of file):
//     IndexOffset: uint64_t
//     IndexSize:   uint64_t
//     Magic:       char[4] = "KAPE"
//
// The index lives at the END of the file so the writer can stream data blocks
// straight to disk without buffering the whole archive in memory.
//
// Version 1 archives (index table at the head, all entries stored raw) remain
// readable; the writer always emits version 2.

enum class PakCompression : std::uint32_t
{
    Store = 0,
    Zstd = 1,
};

// Progress payload delivered by PakArchive::Write. For large stored files the
// callback also fires per streamed chunk with completed == false so a build
// cancel can take effect mid-file.
struct PakEntryResult
{
    std::size_t entry_index = 0;
    std::size_t entry_count = 0;
    std::string rel_path;
    PakCompression method = PakCompression::Store;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t written_size = 0;
    bool completed = false;
};

struct PakWriteStats
{
    std::uint64_t total_uncompressed = 0;
    std::uint64_t total_written = 0; // data section only (no header/index)
    std::uint32_t compressed_count = 0;
    std::uint32_t stored_count = 0;
    std::uint32_t deduped_count = 0;
    // Lowercased extension -> {uncompressed bytes, written bytes}
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> per_extension;
};

// Return false to cancel the write; the partial archive is deleted.
using PakProgressFn = std::function<bool(const PakEntryResult&)>;

class PakArchive
{
public:
    // ---- Writing ------------------------------------------------------------

    // Add a file from the filesystem.  rel_path uses forward slashes and is
    // relative to the project root (e.g. "Scenes/Main.scene").  The file is
    // NOT read here; Write streams it from disk.  Returns false if the file
    // does not exist or is unreadable.
    bool AddFile(const std::string& rel_path, const std::filesystem::path& source_path);

    // Add a file from an in-memory buffer.  Buffer entries always go through
    // the try-compress path in Write (no extension-based store list).
    bool AddBuffer(const std::string& rel_path, const std::vector<std::uint8_t>& data);

    // Write all added files to the given output path, compressing entries per
    // the store/compress policy.  progress (optional) is invoked after each
    // entry (and per streamed chunk for large stored files); returning false
    // cancels the write and removes the partial file.  stats (optional)
    // receives aggregate compression statistics.
    bool Write(const std::filesystem::path& output_path,
               const PakProgressFn& progress = {},
               PakWriteStats* stats = nullptr) const;

    // Remove all entries (reset writer state).
    void Clear();

    // ---- Reading ------------------------------------------------------------

    // Open a PAK file for reading.  Accepts version 1 and 2 archives.
    bool Open(const std::filesystem::path& pak_path);

    // Returns true if the archive is open.
    bool IsOpen() const { return is_open_; }

    // Returns true if an entry with the given relative path exists.  Matching
    // is case-insensitive (paks are built from a case-insensitive filesystem).
    bool Contains(const std::string& rel_path) const;

    // Read (and decompress if needed) the contents of an entry.  Returns
    // empty on error or if the entry does not exist.  Thread-safe: each call
    // uses its own file handle.
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
        std::filesystem::path source_path; // empty for buffer entries
        std::uint64_t source_size = 0;
        std::vector<std::uint8_t> data; // populated only for buffer entries
        bool from_buffer = false;
    };

    struct IndexEntry
    {
        std::string rel_path;
        std::uint64_t offset = 0;
        std::uint64_t compressed_size = 0;
        std::uint64_t uncompressed_size = 0;
        PakCompression method = PakCompression::Store;
    };

    bool ReadIndexV1(std::ifstream& input);
    bool ReadIndexV2(std::ifstream& input, std::uint64_t file_size);
    const IndexEntry* FindEntry(const std::string& rel_path) const;

    // Writer state
    std::vector<Entry> write_entries_;

    // Reader state
    bool is_open_ = false;
    std::filesystem::path pak_path_;
    std::vector<IndexEntry> read_entries_;
    // Lowercased rel_path -> index into read_entries_ (case-insensitive lookup).
    std::unordered_map<std::string, std::size_t> lookup_;
};
