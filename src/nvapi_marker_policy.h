#pragma once

#include <atomic>
#include <cstdint>

// Used only under SchedulerOwnerGuard. Source IDs are monotonic within the
// feedback producer lifetime; reset together with that producer's policy.
class NvApiMarkerPromotion {
public:
    void Reset() noexcept { *this={}; }
    bool Observe(uint64_t frame,int64_t now,int64_t frequency) noexcept {
        if(!frame || now<=0 || frequency<=0) {last_=0;count_=0;return false;}
        if(frame==last_)return false;
        count_=(last_ && frame==last_+1 && now>time_ && now-time_<=frequency/4)?count_+1:1;
        last_=frame;time_=now;
        if(count_>8)count_=8;
        return count_==8 && frame>enforced_;
    }
    bool CanEnforce(uint64_t frame) const noexcept { return frame && frame>enforced_; }
    void Enforced(uint64_t frame) noexcept { if(frame>enforced_)enforced_=frame; }
private:
    uint64_t last_=0,enforced_=0;
    int64_t time_=0;
    uint32_t count_=0;
};

// RTSS can supplement a game's partial native marker stream with
// SIMULATION_START. Do not trust RTSS alone: only admit its nonzero-frame
// SimulationStart after a bounded direct-marker warm-up proves that the game
// is already producing markers and no direct SimulationStart is available.
class NvApiMarkerPolicy {
public:
    static constexpr uint32_t SIMULATION_START_TYPE = 0;
    static constexpr uint32_t DIRECT_MARKER_WARMUP = 32;

    bool ShouldProcess(
        bool from_rtss,
        uint32_t marker_type,
        uint64_t frame_id) noexcept
    {
        if (!from_rtss) {
            const bool valid_simulation_start =
                marker_type == SIMULATION_START_TYPE && frame_id != 0;
            if (valid_simulation_start) {
                direct_simulation_start_seen_.store(
                    true,
                    std::memory_order_release);
            }
            if (frame_id != 0) {
                direct_marker_count_.fetch_add(1, std::memory_order_release);
            }
            if (valid_simulation_start &&
                last_admitted_rtss_frame_id_.load(std::memory_order_acquire) ==
                    frame_id) {
                return false;
            }
            return true;
        }

        if (marker_type != SIMULATION_START_TYPE || frame_id == 0) {
            return false;
        }

        rtss_simulation_start_count_.fetch_add(1, std::memory_order_relaxed);
        if (direct_marker_count_.load(std::memory_order_acquire) <
            DIRECT_MARKER_WARMUP) {
            return false;
        }
        if (direct_simulation_start_seen_.load(std::memory_order_acquire)) {
            return false;
        }

        admitted_rtss_simulation_start_count_.fetch_add(
            1,
            std::memory_order_relaxed);
        last_admitted_rtss_frame_id_.store(frame_id, std::memory_order_release);
        return true;
    }

    void Reset() noexcept
    {
        direct_marker_count_.store(0, std::memory_order_relaxed);
        rtss_simulation_start_count_.store(0, std::memory_order_relaxed);
        admitted_rtss_simulation_start_count_.store(
            0,
            std::memory_order_relaxed);
        last_admitted_rtss_frame_id_.store(0, std::memory_order_relaxed);
        direct_simulation_start_seen_.store(false, std::memory_order_release);
    }

    uint64_t DirectMarkerCount() const noexcept
    {
        return direct_marker_count_.load(std::memory_order_acquire);
    }

    uint64_t RtssSimulationStartCount() const noexcept
    {
        return rtss_simulation_start_count_.load(std::memory_order_relaxed);
    }

    uint64_t AdmittedRtssSimulationStartCount() const noexcept
    {
        return admitted_rtss_simulation_start_count_.load(
            std::memory_order_relaxed);
    }

private:
    std::atomic<uint64_t> direct_marker_count_{ 0 };
    std::atomic<uint64_t> rtss_simulation_start_count_{ 0 };
    std::atomic<uint64_t> admitted_rtss_simulation_start_count_{ 0 };
    std::atomic<uint64_t> last_admitted_rtss_frame_id_{ 0 };
    std::atomic<bool> direct_simulation_start_seen_{ false };
};