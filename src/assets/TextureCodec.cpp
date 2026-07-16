#include "assets/TextureCodec.h"

#include <bc7decomp.h>

#include <algorithm>
#include <cstring>

#ifndef ENGINE_GAME_BUILD
#include <rdo_bc_encoder.h>
#include <thread>
#if defined(_MSC_VER)
#include <omp.h>
#endif
#endif

namespace
{
constexpr char kEtexMagic[4] = {'E', 'T', 'E', 'X'};
constexpr std::uint32_t kEtexVersion = 1;
constexpr std::size_t kEtexHeaderSize = 4 + 4 + 4 + 4 + 4 + 4 + 4 + 8;
constexpr std::uint32_t kFlagHasAlpha = 1u << 0;

// Practical texture ceiling; anything larger is a corrupt header.
constexpr std::uint32_t kMaxDimension = 1u << 16;

std::uint64_t Bc7BlockBytes(std::uint32_t width, std::uint32_t height)
{
    const std::uint64_t blocks_x = (static_cast<std::uint64_t>(width) + 3) / 4;
    const std::uint64_t blocks_y = (static_cast<std::uint64_t>(height) + 3) / 4;
    return blocks_x * blocks_y * 16;
}

template <typename T>
T ReadPod(const std::uint8_t* bytes)
{
    T value;
    std::memcpy(&value, bytes, sizeof(T));
    return value;
}

template <typename T>
void AppendPod(std::vector<std::uint8_t>& out, const T& value)
{
    const std::size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}
} // namespace

namespace texcodec
{

bool IsEtex(const std::uint8_t* bytes, std::size_t size)
{
    return bytes != nullptr && size >= 4 && std::memcmp(bytes, kEtexMagic, 4) == 0;
}

bool ParseEtex(const std::uint8_t* bytes, std::size_t size, EtexView& out_view)
{
    if (!IsEtex(bytes, size) || size < kEtexHeaderSize)
    {
        return false;
    }

    const std::uint32_t version = ReadPod<std::uint32_t>(bytes + 4);
    if (version != kEtexVersion)
    {
        return false;
    }

    EtexView view;
    view.format = ReadPod<std::uint32_t>(bytes + 8);
    view.width = ReadPod<std::uint32_t>(bytes + 12);
    view.height = ReadPod<std::uint32_t>(bytes + 16);
    view.mip_count = ReadPod<std::uint32_t>(bytes + 20);
    const std::uint32_t flags = ReadPod<std::uint32_t>(bytes + 24);
    const std::uint64_t data_size = ReadPod<std::uint64_t>(bytes + 28);

    if (view.format != kEtexFormatBc7 || view.mip_count != 1)
    {
        return false;
    }
    if (view.width == 0 || view.height == 0 || view.width > kMaxDimension || view.height > kMaxDimension)
    {
        return false;
    }
    if (data_size != Bc7BlockBytes(view.width, view.height) ||
        kEtexHeaderSize + data_size != size)
    {
        return false;
    }

    view.has_alpha = (flags & kFlagHasAlpha) != 0;
    view.block_data = bytes + kEtexHeaderSize;
    view.block_size = static_cast<std::size_t>(data_size);
    out_view = view;
    return true;
}

bool DecodeEtexToRgba8(const std::uint8_t* bytes, std::size_t size,
                       std::vector<std::uint8_t>& out_rgba,
                       int& out_width, int& out_height)
{
    EtexView view;
    if (!ParseEtex(bytes, size, view))
    {
        return false;
    }

    const std::uint32_t width = view.width;
    const std::uint32_t height = view.height;
    const std::uint32_t blocks_x = (width + 3) / 4;
    const std::uint32_t blocks_y = (height + 3) / 4;

    out_rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);

    bc7decomp::color_rgba block_pixels[16];
    for (std::uint32_t by = 0; by < blocks_y; ++by)
    {
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx)
        {
            const std::uint8_t* block = view.block_data + (static_cast<std::size_t>(by) * blocks_x + bx) * 16;
            if (!bc7decomp::unpack_bc7(block, block_pixels))
            {
                return false;
            }

            const std::uint32_t max_px = std::min(4u, width - bx * 4);
            const std::uint32_t max_py = std::min(4u, height - by * 4);
            for (std::uint32_t py = 0; py < max_py; ++py)
            {
                const std::size_t dst_row =
                    (static_cast<std::size_t>(by * 4 + py) * width + bx * 4) * 4;
                std::memcpy(out_rgba.data() + dst_row, &block_pixels[py * 4],
                            static_cast<std::size_t>(max_px) * 4);
            }
        }
    }

    out_width = static_cast<int>(width);
    out_height = static_cast<int>(height);
    return true;
}

#ifndef ENGINE_GAME_BUILD

bool EncodeBc7Etex(const std::uint8_t* rgba, int width, int height,
                   std::vector<std::uint8_t>& out_etex)
{
    if (rgba == nullptr || width <= 0 || height <= 0 ||
        static_cast<std::uint32_t>(width) > kMaxDimension ||
        static_cast<std::uint32_t>(height) > kMaxDimension)
    {
        return false;
    }

    utils::image_u8 source(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    static_assert(sizeof(utils::color_quad_u8) == 4, "color_quad_u8 must be 4 bytes RGBA");
    std::memcpy(source.get_pixels().data(), rgba,
                static_cast<std::size_t>(width) * height * 4);

    bool has_alpha = false;
    for (std::size_t i = 3, n = static_cast<std::size_t>(width) * height * 4; i < n; i += 4)
    {
        if (rgba[i] < 255)
        {
            has_alpha = true;
            break;
        }
    }

    // The encoder's parallelism is OpenMP-based. Size the thread team
    // explicitly — a stray OMP_NUM_THREADS in the user's environment (seen
    // in the wild set to 1) would otherwise silently serialize the encode.
#if defined(_MSC_VER)
    omp_set_num_threads(static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
#endif

    rdo_bc::rdo_bc_params params;
    params.m_dxgi_format = DXGI_FORMAT_BC7_UNORM;
    // Build-time budget: uber 2 with a reduced partition scan encodes several
    // times faster than the library defaults at a fraction-of-a-dB quality
    // cost. Reduced-entropy mode biases the encoder toward zstd-friendly
    // blocks during the encode itself; the full ert lookback RDO pass
    // (m_rdo_lambda > 0) is skipped — it dominated pack time on 4K+ sources.
    params.m_bc7_uber_level = 2;
    params.m_bc7enc_max_partitions_to_scan = 16;
    params.m_bc7enc_reduce_entropy = true;
    params.m_rdo_lambda = 0.0f;
    params.m_status_output = false;

    rdo_bc::rdo_bc_encoder encoder;
    if (!encoder.init(source, params) || !encoder.encode())
    {
        return false;
    }

    const std::uint64_t block_bytes = Bc7BlockBytes(static_cast<std::uint32_t>(width),
                                                    static_cast<std::uint32_t>(height));
    if (encoder.get_total_blocks_size_in_bytes() != block_bytes)
    {
        return false;
    }

    out_etex.clear();
    out_etex.reserve(kEtexHeaderSize + static_cast<std::size_t>(block_bytes));
    out_etex.insert(out_etex.end(), kEtexMagic, kEtexMagic + 4);
    AppendPod(out_etex, kEtexVersion);
    AppendPod(out_etex, kEtexFormatBc7);
    AppendPod(out_etex, static_cast<std::uint32_t>(width));
    AppendPod(out_etex, static_cast<std::uint32_t>(height));
    AppendPod(out_etex, std::uint32_t{1}); // mip_count
    AppendPod(out_etex, has_alpha ? kFlagHasAlpha : std::uint32_t{0});
    AppendPod(out_etex, block_bytes);
    const auto* blocks = static_cast<const std::uint8_t*>(encoder.get_blocks());
    out_etex.insert(out_etex.end(), blocks, blocks + block_bytes);
    return true;
}

#endif // !ENGINE_GAME_BUILD

} // namespace texcodec
