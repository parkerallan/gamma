#pragma once

// VideoPlaybackManager — owns FFmpeg decoders for SceneObjectVideo2D
// attributes, advances them each frame, and uploads decoded RGBA frames
// into per-stream GpuTextures held inside Scene2DRenderer.
//
// Lookups are keyed by (object name, attribute index) so renaming an
// object resets playback. The render loop calls Update(dt, scene) every
// frame before Scene2DRenderer::CompositeOverlay; CompositeOverlay then
// queries GetFrameTexture() for each Video2D attribute.
//
// Audio playback is not yet wired (Phase 2): the per-stream volume/muted
// fields are honored on the data side but no PCM is fed to a sound device.

#include "render/Scene2DRenderer.h"
#include "assets/SceneMetadata.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class AudioEngine;
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVIOContext;
struct AVBufferRef;
struct SwsContext;
struct SwrContext;
struct ma_sound;

class VideoPlaybackManager
{
public:
    VideoPlaybackManager() = default;
    ~VideoPlaybackManager();

    VideoPlaybackManager(const VideoPlaybackManager&) = delete;
    VideoPlaybackManager& operator=(const VideoPlaybackManager&) = delete;

    bool Initialize(Scene2DRenderer* scene_2d_renderer, AudioEngine* audio_engine = nullptr);
    void Shutdown();

    void Update(
        float delta_time,
        const SceneMetadata& scene_metadata,
        const std::filesystem::path& project_root);

    const Scene2DRenderer::GpuTexture* GetFrameTexture(
        const std::string& object_name,
        std::size_t attribute_index) const;

private:
    struct StreamKey
    {
        std::string object_name;
        std::size_t attribute_index = 0;
        bool operator==(const StreamKey& o) const
        {
            return object_name == o.object_name && attribute_index == o.attribute_index;
        }
    };
    struct StreamKeyHash
    {
        std::size_t operator()(const StreamKey& k) const
        {
            return std::hash<std::string>{}(k.object_name) ^
                   (std::hash<std::size_t>{}(k.attribute_index) + 0x9e3779b9u);
        }
    };

    struct VideoStream
    {
        std::string source_path;
        AVFormatContext* format_ctx = nullptr;
        AVCodecContext* codec_ctx = nullptr;
        SwsContext* sws_ctx = nullptr;
        AVFrame* decoded_frame = nullptr;       // YUV native frame (or hw-surface)
        AVFrame* sw_frame = nullptr;            // hw-decoded frame transferred to system memory
        AVFrame* rgba_frame = nullptr;          // RGBA scaled frame
        AVPacket* packet = nullptr;
        AVBufferRef* hw_device_ctx = nullptr;   // D3D11VA / DXVA2 / etc., null if software
        int hw_pix_fmt = -1;                    // AVPixelFormat the codec emits when hw-decoding
        int sws_src_format = -1;                // AVPixelFormat sws_ctx was created against (lazy)
        int video_stream_index = -1;
        int width = 0;
        int height = 0;
        double time_base_seconds = 0.0;
        double playback_time = 0.0;             // seconds since start of stream
        double next_frame_pts = -1.0;           // PTS of the frame currently in decoded_frame
        bool reached_eof = false;
        bool finished_once = false;
        bool first_frame_uploaded = false;
        SceneObjectVideoPlayMode last_play_mode = SceneObjectVideoPlayMode::Off;
        Scene2DRenderer::GpuTexture frame_texture{};
        std::vector<unsigned char> rgba_scratch;
        float volume = 1.0f;
        bool muted = false;

        // Audio (optional — only populated when a stream has an audio track
        // and an AudioEngine was supplied to Initialize()).
        int audio_stream_index = -1;
        AVCodecContext* audio_codec_ctx = nullptr;
        SwrContext* swr_ctx = nullptr;
        std::vector<float> pcm_buffer;          // interleaved stereo f32
        std::uint32_t pcm_sample_rate = 0;
        std::uint64_t pcm_frame_count = 0;
        void* audio_buffer_ref = nullptr; // ma_audio_buffer_ref* (opaque; miniaudio anonymous-tag typedef)
        ma_sound* audio_sound = nullptr;
        bool audio_started = false;

        // Pak streaming: when present, this owns the raw file bytes and the
        // demuxer reads from them via avio_ctx instead of the filesystem.
        std::vector<std::uint8_t> file_bytes;
        std::size_t file_pos = 0;
        AVIOContext* avio_ctx = nullptr;
    };

    Scene2DRenderer* scene_2d_renderer_ = nullptr;
    AudioEngine* audio_engine_ = nullptr;
    std::unordered_map<StreamKey, std::unique_ptr<VideoStream>, StreamKeyHash> streams_;

    static constexpr std::size_t kMaxStreams = 8;

    void ReleaseStream(VideoStream& stream);
    bool OpenStream(VideoStream& stream, const std::filesystem::path& path_for_logs, bool from_pak);
    bool OpenAudio(VideoStream& stream);
    bool DecodeAllAudio(VideoStream& stream);
    bool DecodeNextFrame(VideoStream& stream);
    void RewindStream(VideoStream& stream);
    void DecodeAndUpload(VideoStream& stream, float delta_time, const SceneObjectVideo2DAttributes& attr);
    void UpdateAudio(VideoStream& stream, const SceneObjectVideo2DAttributes& attr);
};
