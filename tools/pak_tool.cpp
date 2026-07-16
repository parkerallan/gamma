// Standalone inspector for EPAK archives (v1 and v2).
//
//   pak_tool list <archive.pak>            Print every entry with sizes.
//   pak_tool extract <archive.pak> <dir>   Extract all entries (decompressing).
//   pak_tool pack <input_dir> <archive.pak>  Pack a directory tree (v2).
//
// Built via -DENGINE_BUILD_PAK_TOOL=ON; doubles as the round-trip test harness
// for the pak writer since the repo has no test scaffolding.

#include "assets/TextureCodec.h"
#include "vfs/PakArchive.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cmath>
#include <cstdio>
#include <string>

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        std::fprintf(stderr,
                     "usage:\n"
                     "  pak_tool list <archive.pak>\n"
                     "  pak_tool extract <archive.pak> <output_dir>\n"
                     "  pak_tool pack <input_dir> <archive.pak>\n");
        return 1;
    }

    const std::string command = argv[1];

    if (command == "pack")
    {
        if (argc < 4)
        {
            std::fprintf(stderr, "error: pack requires <input_dir> <archive.pak>\n");
            return 1;
        }
        const std::filesystem::path input_dir = argv[2];
        PakArchive writer;
        std::size_t added = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(input_dir))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const std::string rel =
                std::filesystem::relative(entry.path(), input_dir).generic_string();
            if (!writer.AddFile(rel, entry.path()))
            {
                std::fprintf(stderr, "error: failed to add: %s\n", rel.c_str());
                return 1;
            }
            ++added;
        }
        PakWriteStats stats;
        if (!writer.Write(argv[3], {}, &stats))
        {
            std::fprintf(stderr, "error: failed to write pak\n");
            return 1;
        }
        std::printf("packed %zu files: %llu -> %llu bytes (%u zstd, %u stored, %u deduped)\n",
                    added,
                    static_cast<unsigned long long>(stats.total_uncompressed),
                    static_cast<unsigned long long>(stats.total_written),
                    stats.compressed_count, stats.stored_count, stats.deduped_count);
        return 0;
    }

    if (command == "etex-roundtrip")
    {
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load(argv[2], &width, &height, &channels, 4);
        if (pixels == nullptr)
        {
            std::fprintf(stderr, "error: failed to decode image: %s\n", argv[2]);
            return 1;
        }

        std::vector<std::uint8_t> etex;
        if (!texcodec::EncodeBc7Etex(pixels, width, height, etex))
        {
            std::fprintf(stderr, "error: BC7 encode failed\n");
            return 1;
        }

        std::vector<std::uint8_t> decoded;
        int dec_width = 0;
        int dec_height = 0;
        if (!texcodec::DecodeEtexToRgba8(etex.data(), etex.size(), decoded, dec_width, dec_height) ||
            dec_width != width || dec_height != height)
        {
            std::fprintf(stderr, "error: ETEX decode failed\n");
            return 1;
        }

        double sum_sq = 0.0;
        const std::size_t count = static_cast<std::size_t>(width) * height * 4;
        for (std::size_t i = 0; i < count; ++i)
        {
            const double diff = static_cast<double>(pixels[i]) - static_cast<double>(decoded[i]);
            sum_sq += diff * diff;
        }
        const double mse = sum_sq / static_cast<double>(count);
        const double psnr = mse > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 999.0;
        std::printf("%dx%d -> %zu ETEX bytes (%.2f bpp), PSNR %.2f dB\n",
                    width, height, etex.size(),
                    8.0 * static_cast<double>(etex.size()) / (static_cast<double>(width) * height),
                    psnr);
        stbi_image_free(pixels);
        return 0;
    }

    const std::filesystem::path pak_path = argv[2];

    PakArchive pak;
    if (!pak.Open(pak_path))
    {
        std::fprintf(stderr, "error: failed to open pak: %s\n", pak_path.string().c_str());
        return 1;
    }

    if (command == "list")
    {
        std::uint64_t total = 0;
        const std::vector<std::string> entries = pak.ListEntries();
        for (const std::string& rel_path : entries)
        {
            const std::vector<std::uint8_t> data = pak.ReadEntry(rel_path);
            total += data.size();
            std::printf("%12zu  %s\n", data.size(), rel_path.c_str());
        }
        std::printf("%zu entries, %llu bytes uncompressed\n",
                    entries.size(), static_cast<unsigned long long>(total));
        return 0;
    }

    if (command == "extract")
    {
        if (argc < 4)
        {
            std::fprintf(stderr, "error: extract requires an output directory\n");
            return 1;
        }
        if (!pak.ExtractAll(argv[3]))
        {
            std::fprintf(stderr, "error: one or more entries failed to extract\n");
            return 1;
        }
        std::printf("extracted %zu entries to %s\n", pak.ListEntries().size(), argv[3]);
        return 0;
    }

    std::fprintf(stderr, "error: unknown command: %s\n", command.c_str());
    return 1;
}
