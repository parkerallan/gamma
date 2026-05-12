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

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

    // Hand the manager raw video file bytes harvested by the async scene
    // preloader (or the runtime pak-bytes warm pass) so the first Update()
    // that opens the corresponding stream avoids a synchronous pak read on
    // the main thread. Keyed by the scene-relative video_path; ownership
    // transfers in. Safe to call before Update() begins.
    void PreloadVideoBytes(const std::string& video_path, std::vector<std::uint8_t> bytes);

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
        AVFrame* decoded_frame = nullptr;       // YUV native frame (or hw-surface) — worker only
        AVFrame* sw_frame = nullptr;            // hw-decoded frame transferred to system memory — worker only
        AVFrame* rgba_frame = nullptr;          // RGBA scaled frame — worker only
        AVPacket* packet = nullptr;             // worker only
        AVBufferRef* hw_device_ctx = nullptr;   // D3D11VA / DXVA2 / etc., null if software
        int hw_pix_fmt = -1;                    // AVPixelFormat the codec emits when hw-decoding
        int sws_src_format = -1;                // AVPixelFormat sws_ctx was created against (worker)
        int video_stream_index = -1;
        int width = 0;
        int height = 0;
        double time_base_seconds = 0.0;
        double playback_time = 0.0;             // main thread: current display clock
        double worker_next_frame_pts = -1.0;    // worker only: PTS of frame currently in decoded_frame
        bool finished_once = false;             // main thread
        bool first_frame_uploaded = false;      // main thread
        SceneObjectVideoPlayMode last_play_mode = SceneObjectVideoPlayMode::Off;
        Scene2DRenderer::GpuTexture frame_texture{};
        std::vector<unsigned char> rgba_scratch; // worker-owned scratch backing rgba_frame
        float volume = 1.0f;
        bool muted = false;

        // ---- Worker thread state ----
        // A small ring of pre-decoded RGBA frames produced by the worker and
        // consumed by the main thread during DecodeAndUpload. Each slot is
        // either empty or holds one full-resolution RGBA frame + its PTS.
        struct ReadyFrame
        {
            std::vector<std::uint8_t> pixels;
            double pts = -1.0;
            bool valid = false;
        };
        static constexpr std::size_t kReadyFrameCount = 3;
        std::array<ReadyFrame, kReadyFrameCount> ready_slots{};
        // Holds the most recently displayed frame's pixels, so the GPU upload
        // path can use a stable pointer outside the worker lock.
        std::vector<std::uint8_t> display_pixels;
        std::mutex worker_mtx;
        std::condition_variable worker_cv;
        std::thread worker_thread;
        std::atomic<bool> worker_should_stop{false};
        std::atomic<bool> worker_should_rewind{false};
        // Set by main when the stream should be feeding frames (either
        // playing, or we still need a first frame even if paused).
        std::atomic<bool> worker_allow_decode{true};
        std::atomic<bool> worker_reached_eof{false};
        // Latest playback clock published by main; worker uses it to know
        // when ready_slots are well ahead of display and it can wait.
        std::atomic<double> worker_display_time{0.0};

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
    // Preloaded raw video file bytes (consumed and removed on first
    // OpenStream for the corresponding video_path).
    std::unordered_map<std::string, std::vector<std::uint8_t>> preloaded_video_bytes_;

    static constexpr std::size_t kMaxStreams = 8;

    void ReleaseStream(VideoStream& stream);
    bool OpenStream(VideoStream& stream, const std::filesystem::path& path_for_logs, bool from_pak);
    bool OpenAudio(VideoStream& stream);
    bool DecodeAllAudio(VideoStream& stream);
    bool DecodeNextFrame(VideoStream& stream);
    void RewindStream(VideoStream& stream);
    void DecodeAndUpload(VideoStream& stream, float delta_time, const SceneObjectVideo2DAttributes& attr);
    void UpdateAudio(VideoStream& stream, const SceneObjectVideo2DAttributes& attr);
    void StartWorker(VideoStream& stream);
    void StopWorker(VideoStream& stream);
    void WorkerLoop(VideoStream& stream);
    bool WorkerDecodeAndStage(VideoStream& stream);
};
