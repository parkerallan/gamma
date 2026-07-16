#pragma once

#include <cstdint>

// Paces a render loop to a target rate with a deadline schedule.
//
// Swapchain FIFO blocking cannot be relied on for pacing: driver control
// panels can force vsync off (which turns FIFO into an unthrottled present,
// so the loop free-runs with erratic per-frame dt), and forcing it on makes
// two FIFO swapchains on one queue serialize on vblank. Smooth on-screen
// motion needs the *simulation sample times* to advance evenly, so the loop
// is paced on the CPU instead: sleep on a high-resolution waitable timer to
// just before the deadline, then spin the remainder. A plain sleep is not
// enough — OS scheduler jitter of ~1 ms drifts the submission phase across
// the hardware vblank and reads as slow-cycling stutter (this was observed
// with the earlier SDL_DelayNS deadline in the standalone game loop).
class FramePacer
{
public:
    FramePacer();
    ~FramePacer();

    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // Sleep until the next frame deadline for the given target rate. Call
    // once per loop iteration, after presenting. If the loop is already
    // slower than the target (a genuinely blocking FIFO present, or a slow
    // frame), this returns immediately and resynchronizes the schedule
    // instead of bursting to catch up.
    void WaitForNextFrame(double target_hz);

private:
    std::uint64_t next_deadline_ticks_ = 0;
    std::uint64_t last_wait_return_ticks_ = 0;
    double scheduled_hz_ = 0.0;
    int dwm_not_blocking_streak_ = 0;
    bool dwm_flush_usable_ = true;
    void* waitable_timer_ = nullptr; // Windows HANDLE; unused elsewhere
};
