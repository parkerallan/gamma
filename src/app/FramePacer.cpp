#include "app/FramePacer.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#endif

FramePacer::FramePacer()
{
#ifdef _WIN32
    // High-resolution waitable timer (Windows 10 1803+). Falls back to a
    // regular waitable timer when unavailable; the spin-finish in
    // WaitForNextFrame absorbs the coarser wake-up.
    waitable_timer_ = CreateWaitableTimerExW(
        nullptr,
        nullptr,
        CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (waitable_timer_ == nullptr)
    {
        waitable_timer_ = CreateWaitableTimerExW(
            nullptr,
            nullptr,
            CREATE_WAITABLE_TIMER_MANUAL_RESET,
            TIMER_ALL_ACCESS);
    }
#endif
}

FramePacer::~FramePacer()
{
#ifdef _WIN32
    if (waitable_timer_ != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(waitable_timer_));
        waitable_timer_ = nullptr;
    }
#endif
}

void FramePacer::WaitForNextFrame(double target_hz)
{
    if (target_hz <= 0.0)
    {
        return;
    }
    const std::uint64_t freq = static_cast<std::uint64_t>(SDL_GetPerformanceFrequency());
    if (freq == 0)
    {
        return;
    }
    const std::uint64_t period_ticks = static_cast<std::uint64_t>(static_cast<double>(freq) / target_hz);
    if (period_ticks == 0)
    {
        return;
    }

#ifdef _WIN32
    // Preferred pacing: block until the next DWM composition. Windowed
    // presents always go through the compositor, so this phase-locks the
    // loop to the *actual* display scan-out regardless of any driver-level
    // vsync override. A free-running CPU deadline cannot do that: a software
    // 60.000 Hz schedule beats against the panel's true ~59.94 Hz, and the
    // GPU-completion phase drifts across the composition point, showing
    // duplicated/skipped frames that read as movement judder.
    if (dwm_flush_usable_)
    {
        if (SUCCEEDED(DwmFlush()))
        {
            const std::uint64_t after_flush = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
            // Detect a compositor that is not actually pacing us (remote
            // sessions, composition quirks): if the full loop keeps running
            // far faster than the target even though DwmFlush "succeeds",
            // hand pacing over to the timer below.
            const std::uint64_t loop_ticks = last_wait_return_ticks_ != 0 && after_flush > last_wait_return_ticks_
                ? after_flush - last_wait_return_ticks_
                : period_ticks;
            if (loop_ticks < period_ticks / 2)
            {
                if (++dwm_not_blocking_streak_ >= 120)
                {
                    dwm_flush_usable_ = false;
                }
            }
            else
            {
                dwm_not_blocking_streak_ = 0;
            }
            last_wait_return_ticks_ = after_flush;
            // Keep the timer schedule parked so it does not fight the
            // compositor's cadence if DwmFlush later becomes unusable.
            scheduled_hz_ = 0.0;
            next_deadline_ticks_ = 0;
            return;
        }
        dwm_flush_usable_ = false;
    }
#endif

    const std::uint64_t now = static_cast<std::uint64_t>(SDL_GetPerformanceCounter());
    last_wait_return_ticks_ = now;

    // (Re)start the schedule on the first call, a rate change, or when the
    // loop fell a full period behind (do not burst to catch up — a burst
    // reads as a warp).
    if (scheduled_hz_ != target_hz
        || next_deadline_ticks_ == 0
        || now >= next_deadline_ticks_ + period_ticks)
    {
        scheduled_hz_ = target_hz;
        next_deadline_ticks_ = now + period_ticks;
        return;
    }

    // Slightly late (deadline passed but by less than a period): keep the
    // phase and skip the wait so one long frame costs exactly one period.
    if (now >= next_deadline_ticks_)
    {
        next_deadline_ticks_ += period_ticks;
        return;
    }

    // Sleep to ~1.5 ms before the deadline, then spin the remainder on the
    // performance counter for sub-0.1 ms accuracy.
    const std::uint64_t slack_ticks = static_cast<std::uint64_t>(0.0015 * static_cast<double>(freq));
    if (next_deadline_ticks_ > now + slack_ticks)
    {
        const std::uint64_t sleep_ticks = next_deadline_ticks_ - slack_ticks - now;
#ifdef _WIN32
        if (waitable_timer_ != nullptr)
        {
            LARGE_INTEGER due_time;
            // Negative = relative time, in 100 ns units.
            due_time.QuadPart = -static_cast<LONGLONG>(sleep_ticks * 10000000ULL / freq);
            if (SetWaitableTimer(static_cast<HANDLE>(waitable_timer_), &due_time, 0, nullptr, nullptr, FALSE))
            {
                WaitForSingleObject(static_cast<HANDLE>(waitable_timer_), INFINITE);
            }
        }
        else
#endif
        {
            SDL_DelayNS(sleep_ticks * 1000000000ULL / freq);
        }
    }

    while (static_cast<std::uint64_t>(SDL_GetPerformanceCounter()) < next_deadline_ticks_)
    {
        SDL_CPUPauseInstruction();
    }
    next_deadline_ticks_ += period_ticks;
}
