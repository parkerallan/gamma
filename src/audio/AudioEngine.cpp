#include "audio/AudioEngine.h"

#include "miniaudio.h"

#include <SDL3/SDL_log.h>

#include <cmath>
#include <cstring>

namespace
{
constexpr float kSmallEpsilon = 1.0e-6f;

// Linear volume: 1.0 = unchanged, >1 = amplify, <1 = quieter.
float PerceptualVolume(float v)
{
    return v < 0.0f ? 0.0f : v;
}

void NormalizeVec3(std::array<float, 3>& v)
{
    const float length_squared = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (length_squared <= kSmallEpsilon)
    {
        v = {0.0f, 0.0f, 1.0f};
        return;
    }
    const float inv = 1.0f / std::sqrt(length_squared);
    v[0] *= inv;
    v[1] *= inv;
    v[2] *= inv;
}
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
    Shutdown();
}

bool AudioEngine::Initialize()
{
    if (engine_ != nullptr)
    {
        return true;
    }

    engine_ = new ma_engine{};
    ma_engine_config config = ma_engine_config_init();
    // Let miniaudio pick the default device, channel count, sample rate, etc.
    const ma_result result = ma_engine_init(&config, engine_);
    if (result != MA_SUCCESS)
    {
        SDL_Log("ma_engine_init failed: %d (%s)", static_cast<int>(result), ma_result_description(result));
        delete engine_;
        engine_ = nullptr;
        return false;
    }

    return true;
}

void AudioEngine::Shutdown()
{
    if (engine_ == nullptr)
    {
        return;
    }

    StopAll();
    ma_engine_uninit(engine_);
    delete engine_;
    engine_ = nullptr;
}

void AudioEngine::SetListener(
    const std::array<float, 3>& position,
    const std::array<float, 3>& forward,
    const std::array<float, 3>& up)
{
    if (engine_ == nullptr)
    {
        return;
    }

    std::array<float, 3> fwd = forward;
    std::array<float, 3> u = up;
    NormalizeVec3(fwd);
    NormalizeVec3(u);

    ma_engine_listener_set_position(engine_, 0, position[0], position[1], position[2]);
    ma_engine_listener_set_direction(engine_, 0, fwd[0], fwd[1], fwd[2]);
    ma_engine_listener_set_world_up(engine_, 0, u[0], u[1], u[2]);
}

AudioEngine::SoundHandle AudioEngine::PlaySound(const PlayParams& params)
{
    if (engine_ == nullptr || params.clip_path.empty())
    {
        return kInvalidHandle;
    }

    PlayingSound playing;
    playing.sound = std::make_unique<ma_sound>();
    playing.clip_path = params.clip_path;
    playing.spatialize_3d = params.spatialize_3d;

    // MA_SOUND_FLAG_DECODE pre-decodes into memory which is appropriate for
    // typical SFX-sized clips. For large music files miniaudio supports
    // streaming by omitting that flag; we keep things simple for v1.
    const ma_result result = ma_sound_init_from_file(
        engine_,
        params.clip_path.c_str(),
        MA_SOUND_FLAG_DECODE,
        nullptr,
        nullptr,
        playing.sound.get());
    if (result != MA_SUCCESS)
    {
        return kInvalidHandle;
    }

    ma_sound_set_volume(playing.sound.get(), PerceptualVolume(params.volume));
    ma_sound_set_pitch(playing.sound.get(), params.pitch);
    ma_sound_set_looping(playing.sound.get(), params.loop ? MA_TRUE : MA_FALSE);

    if (params.spatialize_3d)
    {
        ma_sound_set_spatialization_enabled(playing.sound.get(), MA_TRUE);
        ma_sound_set_position(
            playing.sound.get(),
            params.world_position[0],
            params.world_position[1],
            params.world_position[2]);
        ma_sound_set_min_distance(playing.sound.get(), params.min_distance);
        ma_sound_set_max_distance(playing.sound.get(), params.max_distance);
        // Inverse falloff (1/distance) preserves audibility well past min_distance
        // while still being directional, so the explicit Volume slider has a
        // clearly perceptible effect at moderate ranges.
        ma_sound_set_attenuation_model(playing.sound.get(), ma_attenuation_model_inverse);
        ma_sound_set_doppler_factor(playing.sound.get(), params.doppler_factor);
    }
    else
    {
        ma_sound_set_spatialization_enabled(playing.sound.get(), MA_FALSE);
    }

    if (ma_sound_start(playing.sound.get()) != MA_SUCCESS)
    {
        ma_sound_uninit(playing.sound.get());
        return kInvalidHandle;
    }

    const SoundHandle handle = next_handle_++;
    if (next_handle_ == kInvalidHandle)
    {
        next_handle_ = 1;
    }
    playing_.emplace(handle, std::move(playing));
    return handle;
}

bool AudioEngine::UpdateSound(SoundHandle handle, const PlayParams& params)
{
    if (engine_ == nullptr || handle == kInvalidHandle)
    {
        return false;
    }

    const auto it = playing_.find(handle);
    if (it == playing_.end())
    {
        return false;
    }

    PlayingSound& playing = it->second;

    // A one-shot (non-looping) sound that has reached its end is finished;
    // tell the caller it is no longer playing so it can clean up.
    if (ma_sound_at_end(playing.sound.get()) && !ma_sound_is_looping(playing.sound.get()))
    {
        ma_sound_uninit(playing.sound.get());
        playing_.erase(it);
        return false;
    }

    ma_sound_set_volume(playing.sound.get(), PerceptualVolume(params.volume));
    ma_sound_set_pitch(playing.sound.get(), params.pitch);
    ma_sound_set_looping(playing.sound.get(), params.loop ? MA_TRUE : MA_FALSE);

    if (params.spatialize_3d)
    {
        ma_sound_set_spatialization_enabled(playing.sound.get(), MA_TRUE);
        ma_sound_set_position(
            playing.sound.get(),
            params.world_position[0],
            params.world_position[1],
            params.world_position[2]);
        ma_sound_set_min_distance(playing.sound.get(), params.min_distance);
        ma_sound_set_max_distance(playing.sound.get(), params.max_distance);
        ma_sound_set_doppler_factor(playing.sound.get(), params.doppler_factor);
    }
    else
    {
        ma_sound_set_spatialization_enabled(playing.sound.get(), MA_FALSE);
    }

    playing.spatialize_3d = params.spatialize_3d;
    return true;
}

void AudioEngine::StopSound(SoundHandle handle)
{
    if (engine_ == nullptr || handle == kInvalidHandle)
    {
        return;
    }

    const auto it = playing_.find(handle);
    if (it == playing_.end())
    {
        return;
    }

    ma_sound_stop(it->second.sound.get());
    ma_sound_uninit(it->second.sound.get());
    playing_.erase(it);
}

bool AudioEngine::IsPlaying(SoundHandle handle) const
{
    if (engine_ == nullptr || handle == kInvalidHandle)
    {
        return false;
    }
    const auto it = playing_.find(handle);
    if (it == playing_.end())
    {
        return false;
    }
    if (ma_sound_at_end(it->second.sound.get()) && !ma_sound_is_looping(it->second.sound.get()))
    {
        return false;
    }
    return true;
}

void AudioEngine::StopAll()
{
    for (auto& [handle, playing] : playing_)
    {
        ma_sound_stop(playing.sound.get());
        ma_sound_uninit(playing.sound.get());
    }
    playing_.clear();
}
