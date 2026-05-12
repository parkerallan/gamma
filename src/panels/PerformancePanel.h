#pragma once

#include "render/RuntimeRenderer.h"
#include "state/EngineState.h"

#include <vector>

class PerformancePanel
{
public:
    void Render(EngineState& state, const RuntimeRenderer& runtime_renderer);

private:
    void PushSample(const RuntimeRenderer::RuntimePerformanceStats& stats);
    void ClearHistory();

    static constexpr std::size_t kHistoryCapacity = 240;
    static constexpr float kSampleIntervalSeconds = 0.20f;

    bool show_physics_ = true;
    bool show_scripts_ = true;
    bool show_render_ = true;
    bool show_2d_ = true;
    bool show_animation_ = true;
    bool show_audio_ = true;
    bool show_video_ = true;

    std::vector<float> fps_history_;
    std::vector<float> physics_ms_history_;
    std::vector<float> scripts_ms_history_;
    std::vector<float> render_ms_history_;
    std::vector<float> overlay_2d_ms_history_;
    std::vector<float> animation_ms_history_;
    std::vector<float> audio_ms_history_;
    std::vector<float> video_ms_history_;
    std::vector<float> cpu_ms_history_;
    std::vector<float> gpu_ms_history_;

    float sample_window_seconds_ = 0.0f;
    std::size_t sample_window_count_ = 0;
    float fps_accumulator_ = 0.0f;
    float physics_accumulator_ = 0.0f;
    float scripts_accumulator_ = 0.0f;
    float render_accumulator_ = 0.0f;
    float overlay_2d_accumulator_ = 0.0f;
    float animation_accumulator_ = 0.0f;
    float audio_accumulator_ = 0.0f;
    float video_accumulator_ = 0.0f;
    float cpu_accumulator_ = 0.0f;
    float gpu_accumulator_ = 0.0f;
};
