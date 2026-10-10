#pragma once

// Windowed ETW capture-integrity loss for the native PCL collector.
// Pure C++17 without Windows headers so standalone tests include it directly.
//
// Callers pass cumulative loss counters with the local QPC time of the query.
// Each accepted Update() applies, in order:
// 1. Close: an open window closes once the quiet period has elapsed since its
//    last increase (elapsed >= NATIVE_PCL_LOSS_WINDOW_QUIET_PERIOD_MILLISECONDS).
// 2. Open or extend: any counter above its baseline opens a window, or
//    restarts the quiet period of the open one.
// 3. Re-baseline: every counter becomes its own baseline. A counter below its
//    baseline (new ETW session, PresentMon present-tracking reset) therefore
//    re-baselines without opening or extending a window.
// Baselines start at zero, so loss already counted by the first query opens a
// window. Input with zero frequency, zero time, or a time before the previous
// accepted input is rejected and changes nothing, so a pending increase is
// still detected by the next accepted input.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

constexpr uint64_t NATIVE_PCL_LOSS_WINDOW_QUIET_PERIOD_MILLISECONDS = 2000;

// Result of one tracker input. began and ended are both set when the open
// window's quiet period elapsed and a new increase opened the next window.
struct NativePclLossWindowUpdate {
    bool accepted = false; // false: invalid input, nothing changed
    bool began = false;    // this input opened a window
    bool ended = false;    // this input closed the open window
    bool active = false;   // window state after this input
};

template <size_t CounterCount>
class NativePclLossWindow {
public:
    static_assert(CounterCount != 0, "track at least one loss counter");
    using Counters = std::array<uint64_t, CounterCount>;

    NativePclLossWindowUpdate Update(
        const Counters& counters,
        uint64_t now_qpc,
        uint64_t qpc_frequency) noexcept
    {
        NativePclLossWindowUpdate update = Advance(now_qpc, qpc_frequency);
        if (!update.accepted) {
            return update;
        }

        bool increased = false;
        for (size_t index = 0; index < CounterCount; ++index) {
            increased = increased || counters[index] > baselines_[index];
            baselines_[index] = counters[index];
        }
        if (increased) {
            if (!active_) {
                active_ = true;
                update.began = true;
                window_begin_qpc_ = now_qpc;
                ++window_count_;
            }
            last_increase_qpc_ = now_qpc;
        }
        update.active = active_;
        return update;
    }

    // Moves the clock without new counters; closes an elapsed window.
    NativePclLossWindowUpdate Advance(
        uint64_t now_qpc,
        uint64_t qpc_frequency) noexcept
    {
        NativePclLossWindowUpdate update;
        update.active = active_;
        if (qpc_frequency == 0 || now_qpc == 0 || now_qpc < last_input_qpc_) {
            return update;
        }

        update.accepted = true;
        last_input_qpc_ = now_qpc;
        if (active_ &&
            now_qpc - last_increase_qpc_ >= QuietPeriodTicks(qpc_frequency)) {
            active_ = false;
            update.ended = true;
        }
        update.active = active_;
        return update;
    }

    bool IsActive() const noexcept { return active_; }

    // Time of the increase that opened the current or most recent window.
    uint64_t WindowBeginQpc() const noexcept { return window_begin_qpc_; }

    // Windows opened since construction; numbers windows in sparse logs.
    uint64_t WindowCount() const noexcept { return window_count_; }

    // frequency * quiet period / 1000 without intermediate overflow; saturates.
    static uint64_t QuietPeriodTicks(uint64_t qpc_frequency) noexcept
    {
        constexpr uint64_t milliseconds_per_second = 1000;
        constexpr uint64_t quiet_milliseconds =
            NATIVE_PCL_LOSS_WINDOW_QUIET_PERIOD_MILLISECONDS;
        constexpr uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
        const uint64_t ticks_per_millisecond =
            qpc_frequency / milliseconds_per_second;
        const uint64_t remainder_ticks =
            qpc_frequency % milliseconds_per_second * quiet_milliseconds /
            milliseconds_per_second;
        if (ticks_per_millisecond >
            (maximum - remainder_ticks) / quiet_milliseconds) {
            return maximum;
        }
        return ticks_per_millisecond * quiet_milliseconds + remainder_ticks;
    }

private:
    Counters baselines_{};
    uint64_t last_input_qpc_ = 0;
    uint64_t last_increase_qpc_ = 0;
    uint64_t window_begin_qpc_ = 0;
    uint64_t window_count_ = 0;
    bool active_ = false;
};
