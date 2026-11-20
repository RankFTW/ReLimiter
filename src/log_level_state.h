#pragma once

#include <atomic>
#include <cstdint>

// The logger's level as ONE atomic word: the latest REQUESTED level and a temporary FLOOR ("at least this verbose
// until it ends"). The effective level is the more verbose of the two while the floor is active. Used by the stall
// report to keep the log at info for the first minutes (a diagnostic window) without ever overwriting a level that
// was requested meanwhile (the overlay's logging switch): a request only replaces the requested part, ending the
// floor only clears the floor part. Levels are the LogLevel values: 0 error, 1 warn, 2 info, 3 debug (a larger
// value is more verbose). No Windows, no logger: tested by test_hang_report_policy.
//
// Word layout: bits 0-7 the requested level, bits 8-15 the floor level + 1 (0 = no floor). The writer's side is a
// compare-exchange loop (or one fetch_and for the end of the floor); a reader (Log_Write's rejected path) does one
// load and a few arithmetic operations.
class LogLevelState {
public:
    explicit LogLevelState(uint8_t requested = 2) noexcept : word_(requested) {}

    static uint8_t EffectiveOf(uint32_t word) noexcept {
        const uint8_t requested = static_cast<uint8_t>(word & 0xFFu);
        const uint8_t floor_plus_one = static_cast<uint8_t>((word >> 8) & 0xFFu);
        if (floor_plus_one == 0) return requested;
        const uint8_t floor = static_cast<uint8_t>(floor_plus_one - 1u);
        return requested > floor ? requested : floor;
    }

    uint8_t Effective() const noexcept { return EffectiveOf(word_.load(std::memory_order_acquire)); }
    uint8_t Requested() const noexcept { return static_cast<uint8_t>(word_.load(std::memory_order_acquire) & 0xFFu); }
    bool FloorActive() const noexcept { return (word_.load(std::memory_order_acquire) & 0xFF00u) != 0; }

    // A level request (the configuration, the overlay): stored whatever the floor says; the floor only decides what
    // is effective now.
    void Request(uint8_t level) noexcept {
        uint32_t word = word_.load(std::memory_order_relaxed);
        while (!word_.compare_exchange_weak(word, (word & ~0xFFu) | level, std::memory_order_acq_rel)) {}
    }
    // "At least `level` until EndFloor": the request is untouched.
    void BeginFloor(uint8_t level) noexcept {
        uint32_t word = word_.load(std::memory_order_relaxed);
        while (!word_.compare_exchange_weak(word, (word & ~0xFF00u) | ((static_cast<uint32_t>(level) + 1u) << 8),
                                            std::memory_order_acq_rel)) {}
    }
    // One atomic operation; whatever was requested meanwhile becomes effective.
    void EndFloor() noexcept { word_.fetch_and(~0xFF00u, std::memory_order_acq_rel); }
    // A new logger lifetime: this level requested, no floor.
    void Reset(uint8_t requested) noexcept { word_.store(requested, std::memory_order_release); }

private:
    std::atomic<uint32_t> word_;
};
