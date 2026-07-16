#include "vfs/PakArchive.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <system_error>
#include <thread>

namespace
{
constexpr char kMagic[4] = {'E', 'P', 'A', 'K'};
constexpr char kFooterMagic[4] = {'K', 'A', 'P', 'E'};
constexpr std::uint32_t kVersionV1 = 1;
constexpr std::uint32_t kVersionV2 = 2;
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kFooterSize = 20;
constexpr std::uint32_t kMaxPathLength = 4096;

// Compression policy. Level 12 is near the ratio-per-time sweet spot for the
// float-heavy glTF buffers that dominate compressible content; raising it
// costs minutes of build time for single-digit percent gains.
constexpr int kZstdLevel = 12;
// Keep the compressed form only if it saves more than 5%; otherwise store raw
// (this is what lets internally-compressed files like ZIP-EXRs fall back).
constexpr double kKeepCompressedRatio = 0.95;
// Store-list files up to this size are read whole (enables dedup); larger
// ones are streamed raw in chunks so huge videos never sit fully in RAM.
constexpr std::uint64_t kInMemoryCap = 64ull << 20;
constexpr std::size_t kStreamChunk = 8u << 20;

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

// Paks are built from a Windows (case-insensitive) filesystem, and the editor
// resolves assets case-insensitively via NTFS. Matching pak entries the same
// way keeps the built game consistent with the editor — e.g.
// World.LoadScene("main(1)") finds "Scenes/Main(1).scene".
std::string ToLowerAscii(std::string value)
{
    for (char& c : value)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string LowerExtensionOf(const std::string& rel_path)
{
    const std::size_t slash = rel_path.find_last_of('/');
    const std::size_t dot = rel_path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    {
        return {};
    }
    return ToLowerAscii(rel_path.substr(dot));
}

// Extensions whose contents are already compressed at the format level
// (H.264, DEFLATE, DCT, ...). zstd gains ~0% on these, so skip the attempt
// entirely to keep build times down. Applies to AddFile entries only —
// buffer entries (transpiled lua, transcoded textures) always try-compress.
bool IsStoreListExtension(const std::string& lower_ext)
{
    static const char* kStoreExts[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".avif", ".ktx2",
        ".mp4", ".mkv", ".webm", ".mov", ".avi",
        ".mp3", ".ogg", ".opus", ".aac", ".m4a", ".flac",
        ".zip", ".gz", ".zst", ".pak",
    };
    for (const char* ext : kStoreExts)
    {
        if (lower_ext == ext)
        {
            return true;
        }
    }
    return false;
}

std::uint64_t Fnv1a64(const std::uint8_t* data, std::size_t size, std::uint64_t basis)
{
    std::uint64_t hash = basis;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Dedup key: exact size plus two independent 64-bit FNV-1a hashes. With a few
// hundred entries the collision probability is astronomically small; matching
// keys are treated as identical content without a byte-level confirm.
std::string MakeDedupKey(const std::uint8_t* data, std::size_t size)
{
    const std::uint64_t h1 = Fnv1a64(data, size, 14695981039346656037ull);
    const std::uint64_t h2 = Fnv1a64(data, size, 0x9e3779b97f4a7c15ull);
    const std::uint64_t sz = static_cast<std::uint64_t>(size);
    std::string key(24, '\0');
    std::memcpy(key.data(), &sz, 8);
    std::memcpy(key.data() + 8, &h1, 8);
    std::memcpy(key.data() + 16, &h2, 8);
    return key;
}

struct ZstdCCtxDeleter
{
    void operator()(ZSTD_CCtx* ctx) const { ZSTD_freeCCtx(ctx); }
};

template <typename T>
bool ReadPod(std::ifstream& input, T& value)
{
    input.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(input);
}

template <typename T>
void WritePod(std::ofstream& output, const T& value)
{
    output.write(reinterpret_cast<const char*>(&value), sizeof(T));
}
} // namespace

// ---- Writing ----------------------------------------------------------------

bool PakArchive::AddFile(const std::string& rel_path, const std::filesystem::path& source_path)
{
    // Deferred: the file is streamed from disk during Write. Only verify it
    // is readable now so callers keep getting an early failure.
    std::error_code ec;
    const std::uint64_t size = std::filesystem::file_size(source_path, ec);
    if (ec)
    {
        return false;
    }
    std::ifstream probe(source_path, std::ios::binary);
    if (!probe)
    {
        return false;
    }

    Entry entry;
    entry.rel_path = NormalizeRelPath(rel_path);
    entry.source_path = source_path;
    entry.source_size = size;
    entry.from_buffer = false;
    write_entries_.push_back(std::move(entry));
    return true;
}

bool PakArchive::AddBuffer(const std::string& rel_path, const std::vector<std::uint8_t>& data)
{
    Entry entry;
    entry.rel_path = NormalizeRelPath(rel_path);
    entry.data = data;
    entry.source_size = data.size();
    entry.from_buffer = true;
    write_entries_.push_back(std::move(entry));
    return true;
}

bool PakArchive::Write(const std::filesystem::path& output_path,
                       const PakProgressFn& progress,
                       PakWriteStats* stats) const
{
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return false;
    }

    const auto fail_and_cleanup = [&]() {
        output.close();
        std::error_code ec;
        std::filesystem::remove(output_path, ec);
        return false;
    };

    // Header
    output.write(kMagic, 4);
    WritePod(output, kVersionV2);
    WritePod(output, std::uint32_t{0});

    const std::unique_ptr<ZSTD_CCtx, ZstdCCtxDeleter> cctx(ZSTD_createCCtx());
    if (!cctx)
    {
        return fail_and_cleanup();
    }
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, kZstdLevel);
    // Per-frame XXH64 content checksum, verified automatically on decompress.
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_checksumFlag, 1);
    // Best effort; harmlessly rejected when zstd is built single-threaded.
    const unsigned hw = std::thread::hardware_concurrency();
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_nbWorkers,
                           static_cast<int>(std::min(hw > 0 ? hw : 1u, 8u)));

    std::vector<IndexEntry> index;
    index.reserve(write_entries_.size());
    std::unordered_map<std::string, std::size_t> dedup; // content key -> index entry
    std::vector<std::uint8_t> raw_scratch;
    std::vector<std::uint8_t> zstd_scratch;

    std::uint64_t current_offset = kHeaderSize;

    const auto record_stats = [&](const IndexEntry& entry) {
        if (stats == nullptr)
        {
            return;
        }
        stats->total_uncompressed += entry.uncompressed_size;
        auto& ext_totals = stats->per_extension[LowerExtensionOf(entry.rel_path)];
        ext_totals.first += entry.uncompressed_size;
        ext_totals.second += entry.compressed_size;
    };

    const auto notify = [&](std::size_t entry_index, const IndexEntry& entry,
                            std::uint64_t written, bool completed) {
        if (!progress)
        {
            return true;
        }
        PakEntryResult result;
        result.entry_index = entry_index;
        result.entry_count = write_entries_.size();
        result.rel_path = entry.rel_path;
        result.method = entry.method;
        result.uncompressed_size = entry.uncompressed_size;
        result.written_size = written;
        result.completed = completed;
        return progress(result);
    };

    for (std::size_t entry_index = 0; entry_index < write_entries_.size(); ++entry_index)
    {
        const Entry& source = write_entries_[entry_index];

        IndexEntry entry;
        entry.rel_path = source.rel_path;

        const std::string lower_ext = LowerExtensionOf(source.rel_path);
        const bool on_store_list = !source.from_buffer && IsStoreListExtension(lower_ext);

        // ---- Large store-list files: stream raw in chunks, never in RAM ----
        if (on_store_list && source.source_size > kInMemoryCap)
        {
            std::ifstream input(source.source_path, std::ios::binary);
            if (!input)
            {
                return fail_and_cleanup();
            }

            entry.offset = current_offset;
            entry.method = PakCompression::Store;

            raw_scratch.resize(kStreamChunk);
            std::uint64_t written = 0;
            while (input)
            {
                input.read(reinterpret_cast<char*>(raw_scratch.data()),
                           static_cast<std::streamsize>(kStreamChunk));
                const std::streamsize got = input.gcount();
                if (got <= 0)
                {
                    break;
                }
                output.write(reinterpret_cast<const char*>(raw_scratch.data()), got);
                if (!output)
                {
                    return fail_and_cleanup();
                }
                written += static_cast<std::uint64_t>(got);
                entry.compressed_size = written;
                entry.uncompressed_size = written;
                if (!notify(entry_index, entry, written, false))
                {
                    return fail_and_cleanup();
                }
            }
            if (input.bad())
            {
                return fail_and_cleanup();
            }

            current_offset += written;
            record_stats(entry);
            if (stats != nullptr)
            {
                stats->total_written += entry.compressed_size;
                ++stats->stored_count;
            }
            index.push_back(entry);
            if (!notify(entry_index, index.back(), written, true))
            {
                return fail_and_cleanup();
            }
            continue;
        }

        // ---- Everything else: whole-buffer path (bounded by the largest single file) ----
        const std::uint8_t* raw_data = nullptr;
        std::size_t raw_size = 0;
        if (source.from_buffer)
        {
            raw_data = source.data.data();
            raw_size = source.data.size();
        }
        else
        {
            std::ifstream input(source.source_path, std::ios::binary);
            if (!input)
            {
                return fail_and_cleanup();
            }
            input.seekg(0, std::ios::end);
            raw_size = static_cast<std::size_t>(input.tellg());
            input.seekg(0, std::ios::beg);
            raw_scratch.resize(raw_size);
            if (raw_size > 0 &&
                !input.read(reinterpret_cast<char*>(raw_scratch.data()),
                            static_cast<std::streamsize>(raw_size)))
            {
                return fail_and_cleanup();
            }
            raw_data = raw_scratch.data();
        }

        entry.uncompressed_size = raw_size;

        // Dedup: identical content aliases the first copy's data block.
        const std::string dedup_key = MakeDedupKey(raw_data, raw_size);
        const auto dedup_it = dedup.find(dedup_key);
        if (dedup_it != dedup.end())
        {
            const IndexEntry& original = index[dedup_it->second];
            entry.offset = original.offset;
            entry.compressed_size = original.compressed_size;
            entry.method = original.method;
            record_stats(entry);
            if (stats != nullptr)
            {
                ++stats->deduped_count;
            }
            index.push_back(entry);
            if (!notify(entry_index, index.back(), 0, true))
            {
                return fail_and_cleanup();
            }
            continue;
        }

        // Store-vs-compress decision happens before any bytes are written, so
        // every byte lands exactly once at a monotonically increasing offset.
        const std::uint8_t* out_data = raw_data;
        std::size_t out_size = raw_size;
        entry.method = PakCompression::Store;
        if (!on_store_list && raw_size > 0)
        {
            zstd_scratch.resize(ZSTD_compressBound(raw_size));
            const std::size_t compressed = ZSTD_compress2(
                cctx.get(), zstd_scratch.data(), zstd_scratch.size(), raw_data, raw_size);
            if (!ZSTD_isError(compressed) &&
                static_cast<double>(compressed) <=
                    static_cast<double>(raw_size) * kKeepCompressedRatio)
            {
                out_data = zstd_scratch.data();
                out_size = compressed;
                entry.method = PakCompression::Zstd;
            }
        }

        entry.offset = current_offset;
        entry.compressed_size = out_size;
        if (out_size > 0)
        {
            output.write(reinterpret_cast<const char*>(out_data),
                         static_cast<std::streamsize>(out_size));
            if (!output)
            {
                return fail_and_cleanup();
            }
        }
        current_offset += out_size;

        dedup.emplace(dedup_key, index.size());
        record_stats(entry);
        if (stats != nullptr)
        {
            stats->total_written += entry.compressed_size;
            if (entry.method == PakCompression::Zstd)
            {
                ++stats->compressed_count;
            }
            else
            {
                ++stats->stored_count;
            }
        }
        index.push_back(entry);
        if (!notify(entry_index, index.back(), out_size, true))
        {
            return fail_and_cleanup();
        }
    }

    // Index blob
    const std::uint64_t index_offset = current_offset;
    WritePod(output, static_cast<std::uint32_t>(index.size()));
    std::uint64_t index_size = 4;
    for (const IndexEntry& entry : index)
    {
        const std::uint32_t path_length = static_cast<std::uint32_t>(entry.rel_path.size());
        WritePod(output, path_length);
        output.write(entry.rel_path.data(), path_length);
        WritePod(output, entry.offset);
        WritePod(output, entry.compressed_size);
        WritePod(output, entry.uncompressed_size);
        WritePod(output, static_cast<std::uint32_t>(entry.method));
        index_size += 4 + path_length + 8 + 8 + 8 + 4;
    }

    // Footer
    WritePod(output, index_offset);
    WritePod(output, index_size);
    output.write(kFooterMagic, 4);

    if (!output.good())
    {
        return fail_and_cleanup();
    }
    return true;
}

void PakArchive::Clear()
{
    write_entries_.clear();
    is_open_ = false;
    pak_path_.clear();
    read_entries_.clear();
    lookup_.clear();
}

// ---- Reading ----------------------------------------------------------------

bool PakArchive::ReadIndexV1(std::ifstream& input)
{
    // v1: entry count where v2 keeps the reserved field, table at the head,
    // all data stored raw.
    std::uint32_t entry_count = 0;
    if (!ReadPod(input, entry_count))
    {
        return false;
    }

    read_entries_.reserve(entry_count);
    for (std::uint32_t index = 0; index < entry_count; ++index)
    {
        std::uint32_t path_length = 0;
        if (!ReadPod(input, path_length) || path_length == 0 || path_length > kMaxPathLength)
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

        std::uint64_t size = 0;
        if (!ReadPod(input, entry.offset) || !ReadPod(input, size))
        {
            return false;
        }
        entry.compressed_size = size;
        entry.uncompressed_size = size;
        entry.method = PakCompression::Store;

        read_entries_.push_back(std::move(entry));
    }
    return true;
}

bool PakArchive::ReadIndexV2(std::ifstream& input, std::uint64_t file_size)
{
    if (file_size < kHeaderSize + kFooterSize)
    {
        return false;
    }

    input.seekg(static_cast<std::streamoff>(file_size - kFooterSize));
    std::uint64_t index_offset = 0;
    std::uint64_t index_size = 0;
    char footer_magic[4];
    if (!ReadPod(input, index_offset) || !ReadPod(input, index_size))
    {
        return false;
    }
    input.read(footer_magic, 4);
    if (!input || std::memcmp(footer_magic, kFooterMagic, 4) != 0)
    {
        return false;
    }
    if (index_offset < kHeaderSize || index_offset + index_size + kFooterSize != file_size)
    {
        return false;
    }

    input.seekg(static_cast<std::streamoff>(index_offset));
    std::uint32_t entry_count = 0;
    if (!ReadPod(input, entry_count))
    {
        return false;
    }

    read_entries_.reserve(entry_count);
    for (std::uint32_t index = 0; index < entry_count; ++index)
    {
        std::uint32_t path_length = 0;
        if (!ReadPod(input, path_length) || path_length == 0 || path_length > kMaxPathLength)
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

        std::uint32_t method = 0;
        if (!ReadPod(input, entry.offset) || !ReadPod(input, entry.compressed_size) ||
            !ReadPod(input, entry.uncompressed_size) || !ReadPod(input, method))
        {
            return false;
        }
        if (method > static_cast<std::uint32_t>(PakCompression::Zstd) ||
            entry.offset + entry.compressed_size > index_offset)
        {
            return false;
        }
        entry.method = static_cast<PakCompression>(method);

        read_entries_.push_back(std::move(entry));
    }
    return true;
}

bool PakArchive::Open(const std::filesystem::path& pak_path)
{
    is_open_ = false;
    read_entries_.clear();
    lookup_.clear();

    std::ifstream input(pak_path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    char magic[4];
    input.read(magic, 4);
    if (!input || std::memcmp(magic, kMagic, 4) != 0)
    {
        return false;
    }

    std::uint32_t version = 0;
    if (!ReadPod(input, version))
    {
        return false;
    }

    bool ok = false;
    if (version == kVersionV1)
    {
        ok = ReadIndexV1(input);
    }
    else if (version == kVersionV2)
    {
        std::error_code ec;
        const std::uint64_t file_size = std::filesystem::file_size(pak_path, ec);
        ok = !ec && ReadIndexV2(input, file_size);
    }
    if (!ok)
    {
        read_entries_.clear();
        return false;
    }

    // Case-insensitive lookup map. emplace keeps the first occurrence, which
    // matches the old linear scan's first-match behaviour.
    lookup_.reserve(read_entries_.size());
    for (std::size_t index = 0; index < read_entries_.size(); ++index)
    {
        lookup_.emplace(ToLowerAscii(read_entries_[index].rel_path), index);
    }

    pak_path_ = pak_path;
    is_open_ = true;
    return true;
}

const PakArchive::IndexEntry* PakArchive::FindEntry(const std::string& rel_path) const
{
    const auto it = lookup_.find(ToLowerAscii(NormalizeRelPath(rel_path)));
    if (it == lookup_.end())
    {
        return nullptr;
    }
    return &read_entries_[it->second];
}

bool PakArchive::Contains(const std::string& rel_path) const
{
    return FindEntry(rel_path) != nullptr;
}

std::vector<std::uint8_t> PakArchive::ReadEntry(const std::string& rel_path) const
{
    if (!is_open_)
    {
        return {};
    }

    const IndexEntry* found = FindEntry(rel_path);
    if (found == nullptr || found->uncompressed_size == 0)
    {
        return {};
    }

    // A fresh handle per call keeps concurrent reads (async scene preloader
    // worker threads) safe without locking.
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

    std::vector<std::uint8_t> raw(static_cast<std::size_t>(found->compressed_size));
    if (!input.read(reinterpret_cast<char*>(raw.data()),
                    static_cast<std::streamsize>(raw.size())))
    {
        return {};
    }

    if (found->method == PakCompression::Store)
    {
        return raw;
    }

    std::vector<std::uint8_t> data(static_cast<std::size_t>(found->uncompressed_size));
    const std::size_t decompressed =
        ZSTD_decompress(data.data(), data.size(), raw.data(), raw.size());
    if (ZSTD_isError(decompressed) || decompressed != data.size())
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
        if (data.empty() && entry.uncompressed_size > 0)
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
