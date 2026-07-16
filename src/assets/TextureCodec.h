#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// ETEX: minimal engine container for GPU-compressed textures, produced at
// game-build time by transcoding loose .png/.jpg files and packed into
// assets.pak under the ORIGINAL pak key (e.g. "Assets/Images/foo.png" keeps
// its key but holds ETEX bytes). Runtime loaders sniff the magic before
// falling back to stb_image.
//
// Layout (little-endian):
//   char[4]  magic     = "ETEX"
//   u32      version   = 1
//   u32      format    (1 = BC7)
//   u32      width     (texels)
//   u32      height    (texels)
//   u32      mip_count (always 1 in v1 — the RT shaders sample base LOD only)
//   u32      flags     (bit 0: has alpha < 255)
//   u64      data_size (byte size of the block payload)
//   [BC7 blocks: ((width+3)/4) * ((height+3)/4) * 16 bytes]

namespace texcodec
{

constexpr std::uint32_t kEtexFormatBc7 = 1;

// Bump whenever EncodeBc7Etex's encoder parameters change: the game build's
// texture transcode cache mixes this into its content-hash keys, so stale
// cache entries produced with old settings are ignored automatically.
constexpr std::uint32_t kEtexEncoderSettingsVersion = 2;

struct EtexView
{
    std::uint32_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_count = 0;
    bool has_alpha = false;
    const std::uint8_t* block_data = nullptr; // points into the parsed buffer
    std::size_t block_size = 0;
};

// True if the buffer starts with the ETEX magic.
bool IsEtex(const std::uint8_t* bytes, std::size_t size);

// Validate and parse an ETEX blob. The returned view points into `bytes`.
bool ParseEtex(const std::uint8_t* bytes, std::size_t size, EtexView& out_view);

// CPU fallback for consumers that need raw RGBA8 pixels (2D overlays, skybox,
// anything stb-based). Decodes the BC7 blocks with bc7decomp.
bool DecodeEtexToRgba8(const std::uint8_t* bytes, std::size_t size,
                       std::vector<std::uint8_t>& out_rgba,
                       int& out_width, int& out_height);

#ifndef ENGINE_GAME_BUILD
// Build-time only: encode RGBA8 pixels into a BC7 ETEX blob (RDO-optimized so
// the pak's zstd pass can shrink the blocks further). Not compiled into the
// game so the shipped exe never pulls the OpenMP-based encoder objects.
bool EncodeBc7Etex(const std::uint8_t* rgba, int width, int height,
                   std::vector<std::uint8_t>& out_etex);
#endif

} // namespace texcodec
