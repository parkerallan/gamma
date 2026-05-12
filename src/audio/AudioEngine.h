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

    // Drop all currently-playing instances. Called on scene stop.
    void StopAll();

    // Direct access to the underlying miniaudio engine, for subsystems that
    // need to attach their own non-spatial ma_sound (e.g. video audio tracks).
    // Returns nullptr if not initialized.
    ma_engine* GetEngine() const { return engine_; }

private:
    struct PlayingSound
    {
        std::unique_ptr<ma_sound> sound;
        std::unique_ptr<ma_decoder> decoder;
        std::vector<std::uint8_t> clip_bytes;
        std::string clip_path;
        bool spatialize_3d = true;
    };

    ma_engine* engine_ = nullptr;
    std::unordered_map<SoundHandle, PlayingSound> playing_;
    SoundHandle next_handle_ = 1;
};
