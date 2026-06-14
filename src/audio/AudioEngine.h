#pragma once

// AudioEngine — thin wrapper around miniaudio providing 3D-spatialized
// playback for the scene Audio attribute. Owned by RuntimeRenderer.
//
// All public functions are no-ops (returning false / 0 where applicable)
// when the engine has not been initialized so callers don't need to guard
// every site.

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ma_engine;
struct ma_sound;
struct ma_decoder;

class AudioEngine
{
public:
    using SoundHandle = std::uint64_t;
    static constexpr SoundHandle kInvalidHandle = 0;

    struct PlayParams
    {
        std::string clip_path;       // absolute path on disk, .wav/.ogg/.mp3
        // Optional in-memory clip data (used for packed game builds where the
        // file lives inside assets.pak and is not on disk). When non-empty,
        // this is used and clip_path is ignored for the actual decode.
        std::vector<std::uint8_t> clip_bytes;
        float volume = 1.0f;
        float pitch = 1.0f;
        bool loop = false;
        bool spatialize_3d = true;
        float min_distance = 1.0f;
        float max_distance = 50.0f;
        float doppler_factor = 1.0f;
        std::array<float, 3> world_position = {0.0f, 0.0f, 0.0f};
    };

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Initialize();
    void Shutdown();
    bool IsInitialized() const { return engine_ != nullptr; }

    // Listener (the active scene camera). Forward/up are unit vectors.
    void SetListener(
        const std::array<float, 3>& position,
        const std::array<float, 3>& forward,
        const std::array<float, 3>& up);

    // Start a new playing instance. Returns kInvalidHandle on failure.
    SoundHandle PlaySound(const PlayParams& params);

    // Update a currently-playing sound's per-frame parameters. Returns false
    // if the handle is no longer valid (e.g., a one-shot finished naturally).
    bool UpdateSound(SoundHandle handle, const PlayParams& params);

    // Stop and release a sound instance. Safe to call with kInvalidHandle.
    void StopSound(SoundHandle handle);

    // True if the handle still refers to an instance that has not finished.
    bool IsPlaying(SoundHandle handle) const;

    // Current playback position of a sound, in seconds. Returns 0 if the handle
    // is invalid/finished. Used to drive lip-sync curves off the real audio
    // clock so the mouth stays locked to the sound.
    float GetPlaybackSeconds(SoundHandle handle) const;

    // Drop all currently-playing instances. Called on scene stop.
    void StopAll();

    // Direct access to the underlying miniaudio engine, for subsystems that
    // need to attach their own non-spatial ma_sound (e.g. video audio tracks).
    // Returns nullptr if not initialized.
    ma_engine* GetEngine() const { return engine_; }

    // Cache raw clip bytes keyed by path so subsequent PlaySound calls for
    // the same path skip disk / pak I/O. Safe to call from any thread; used
    // by the async scene preloader. Bytes are decoded lazily by miniaudio on
    // playback — there is no upfront PCM decode, which is what made
    // first-PlaySound slow under the old MA_SOUND_FLAG_DECODE path.
    void PreloadClipBytes(const std::string& clip_path, std::vector<std::uint8_t> bytes);

private:
    struct PlayingSound
    {
        std::unique_ptr<ma_sound> sound;
        std::unique_ptr<ma_decoder> decoder;
        std::shared_ptr<const std::vector<std::uint8_t>> clip_bytes;
        std::string clip_path;
        bool spatialize_3d = true;
    };

    // Lookup or insert a shared byte buffer for the given clip path. Reads
    // from disk on the editor path or returns the cached entry from a prior
    // Preload / PlaySound call. Returns null if no bytes could be obtained.
    std::shared_ptr<const std::vector<std::uint8_t>> GetOrLoadClipBytes(
        const std::string& clip_path,
        std::vector<std::uint8_t>* inline_bytes);

    ma_engine* engine_ = nullptr;
    std::unordered_map<SoundHandle, PlayingSound> playing_;
    SoundHandle next_handle_ = 1;
    std::mutex clip_cache_mtx_;
    std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> clip_cache_;
};
