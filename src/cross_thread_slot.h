#pragma once

// Cross-thread PRESENT_START hand-off (project-docs/002_unified_pacing_timing/
// phase4_design.md section 15, R8). Some engines send PRESENT_START on one
// thread and call Present on another (Gears of War: E-Day: strict
// alternation, Present 0.8-5 ms after its own marker and never after the
// next one). The marker thread publishes its newest marker; a Present on a
// thread without its own marker takes it exactly once; a Present on the
// marker's own thread claims it so no other thread takes it.
//
// Every operation is a bounded try-lock (a few retries over a copy of a few
// hundred bytes): contention never blocks a hook beyond that, it only loses
// one hand-off (the consumer's own checks then fail closed). Holds one
// value; publishing replaces it, so an empty value (a rejected marker) also
// withdraws the previous one. Pure C++17, no Windows dependency.

#include <atomic>
#include <cstdint>

template <typename T>
class CrossThreadSlot {
public:
    // Marker thread: the newest marker (or an empty one, valid == false).
    // Returns its sequence, or 0 when contended (nothing published).
    uint64_t Publish(const T& value, bool valid) noexcept
    {
        if (!TryLock()) return 0;
        const uint64_t sequence = ++next_;
        value_ = value;
        sequence_ = sequence;
        valid_ = valid;
        Unlock();
        return sequence;
    }

    // Updates the published value of `sequence` while it is still the newest.
    template <typename Update>
    void Amend(uint64_t sequence, Update update) noexcept
    {
        if (sequence == 0 || !TryLock()) return;
        if (sequence_ == sequence) update(&value_);
        Unlock();
    }

    // Consumer that has its own same-thread marker of `sequence`.
    void Claim(uint64_t sequence) noexcept
    {
        if (sequence == 0 || !TryLock()) return;
        if (sequence_ == sequence) claimed_ = sequence;
        Unlock();
    }

    // Consumer without its own marker: the newest valid, unclaimed value,
    // exactly once.
    bool Take(T* value) noexcept
    {
        if (value == nullptr || !TryLock()) return false;
        const bool available = valid_ && sequence_ != 0 && sequence_ != claimed_;
        if (available) {
            *value = value_;
            claimed_ = sequence_;
        }
        Unlock();
        return available;
    }

private:
    bool TryLock() noexcept
    {
        for (int attempt = 0; attempt < kLockAttempts; ++attempt) {
            if (!lock_.test_and_set(std::memory_order_acquire)) return true;
        }
        return false;
    }
    static constexpr int kLockAttempts = 64;
    void Unlock() noexcept { lock_.clear(std::memory_order_release); }

    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    T value_{};               // guarded by lock_
    uint64_t next_ = 0;       // guarded
    uint64_t sequence_ = 0;   // guarded: sequence of value_; 0 = none
    uint64_t claimed_ = 0;    // guarded: newest sequence claimed or taken
    bool valid_ = false;      // guarded
};
