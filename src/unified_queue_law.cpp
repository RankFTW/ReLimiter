#include "unified_queue_law.h"

#include <algorithm>
#include <cmath>
#include <limits>

// The pure queue law (unified_queue_law.h). No Windows, logger or config dependencies.

namespace
{
    constexpr double kProportionalGain = 1.0 / 32.0; // q's gain: a low order statistic can hold a sample W - 2 frames
    constexpr double kGuardGain = 1.0 / 8.0;         // the fast guard's gain above 2M on the newest four margins
    constexpr double kClampFraction = 1.0 / 8.0;     // P's clamp, of B

    bool NonNegativeFinite(double value) noexcept
    {
        return std::isfinite(value) && value >= 0.0;
    }
}

UnifiedQueueSample UnifiedQueueSample_FromRecord(const PacingGpuQueueRecord& record) noexcept
{
    UnifiedQueueSample sample{};
    constexpr uint32_t kRequired = PacingGpuQueueRecord::kCycle | PacingGpuQueueRecord::kEmpty |
        PacingGpuQueueRecord::kBoundaryIdle | PacingGpuQueueRecord::kHoldBlock | PacingGpuQueueRecord::kWaitIdle;
    if ((record.valid & kRequired) != kRequired)
        return sample;
    if (!NonNegativeFinite(record.empty_us) || !NonNegativeFinite(record.boundary_idle_us) ||
        !NonNegativeFinite(record.hold_block_us) || !NonNegativeFinite(record.wait_idle_us) ||
        !NonNegativeFinite(record.cycle_us))
        return sample;
    const double starve_us = record.empty_us + record.boundary_idle_us;
    const bool has_margin = (record.valid & PacingGpuQueueRecord::kMinMarginFree) != 0 &&
        NonNegativeFinite(record.min_margin_free_us);
    if (!(starve_us > 0.0) && !has_margin)
        return sample;
    sample.valid = true;
    sample.starve_us = starve_us;
    sample.margin_free_us = has_margin ? record.min_margin_free_us : 0.0;
    // The render queue found by dependency: its hold block is ReLimiter's own order Wait (the gate's hold is on
    // the presenting queue), time the queue sat blocked by the pacer's choice and never the GPU's need. Under the
    // order a target-bound frame is held I - Delta whatever the lead is, so a base that kept the block followed
    // the interval it caused (gain 1); it is taken out of the cycle here (the invariant in unified_queue_law.h).
    // N and S below are computed from the record as it is and do not change.
    const bool order_block = record.queue_role == kUnifiedQueueRoleRenderQueue;
    sample.cycle_us = order_block ? std::max(0.0, record.cycle_us - record.hold_block_us) : record.cycle_us;
    sample.hold_block_us = order_block ? 0.0 : record.hold_block_us;
    sample.boundary_idle_us = record.boundary_idle_us;
    sample.wait_idle_us = record.wait_idle_us;
    const double raw_need_us =
        record.cycle_us - record.hold_block_us - record.boundary_idle_us - record.wait_idle_us;
    sample.need_us = std::max(0.0, raw_need_us);
    sample.service_us = std::max(0.0, raw_need_us - record.empty_us);
    sample.s_prime_us = starve_us > 0.0 ? -starve_us : sample.margin_free_us;
    return sample;
}

UnifiedQueuePosition UnifiedQueuePosition_FromRecord(const PacingGpuQueueRecord& record) noexcept
{
    UnifiedQueuePosition position{};
    if (!PacingFrameKey_IsValid(record.key))
        return position;
    position.valid = true;
    position.domain = record.key.domain;
    position.producer_epoch = record.key.producer_epoch;
    position.frame_id = record.key.frame_id;
    return position;
}

void UnifiedQueueWindow::Reset() noexcept
{
    *this = UnifiedQueueWindow{};
}

void UnifiedQueueWindow::Push(const UnifiedQueueSample& sample, const UnifiedQueuePosition& position) noexcept
{
    arrivals_[arrival_head_] = sample.valid;
    arrival_head_ = (arrival_head_ + 1) % static_cast<uint32_t>(kWindow);
    if (!sample.valid)
        return;
    samples_[sample_head_] = sample;
    sample_head_ = (sample_head_ + 1) % static_cast<uint32_t>(kWindow);
    if (sample_count_ < kWindow)
        ++sample_count_;
    frames_since_valid_ = 0;
    last_valid_positioned_ = position.valid;
    if (position.valid) {
        // Only the largest position of a stream counts; a record out of order by position is
        // in the statistics but never makes the window fresher. Another domain or epoch is a
        // new stream.
        const bool same_stream = newest_position_.valid && newest_position_.domain == position.domain &&
            newest_position_.producer_epoch == position.producer_epoch;
        if (!same_stream || position.frame_id > newest_position_.frame_id)
            newest_position_ = position;
    }
}

void UnifiedQueueWindow::NotePlanned() noexcept
{
    if (frames_since_valid_ < UINT32_MAX)
        ++frames_since_valid_;
}

uint32_t UnifiedQueueWindow::ValidArrivals() const noexcept
{
    uint32_t count = 0;
    for (const bool valid : arrivals_)
        count += valid ? 1u : 0u;
    return count;
}

bool UnifiedQueueWindow::InUse(const UnifiedQueuePosition& planned) const noexcept
{
    // Fresh: by position when the newest valid arrival carried one that is comparable with the
    // planned frame's (the planned frame at most kMaxPlannedAge frames past the largest position
    // seen); otherwise by the arrival-count age.
    const bool comparable = last_valid_positioned_ && newest_position_.valid && planned.valid &&
        planned.domain == newest_position_.domain && planned.producer_epoch == newest_position_.producer_epoch;
    const bool fresh = comparable ? planned.frame_id <= newest_position_.frame_id + kMaxPlannedAge
                                  : frames_since_valid_ <= kMaxPlannedAge;
    return sample_count_ >= kMinValidInWindow && ValidArrivals() >= kMinValidArrivals && fresh;
}

UnifiedQueueResult UnifiedQueueWindow::Compute(double j_us, const UnifiedQueueParams& params,
    const UnifiedQueuePosition& planned) const noexcept
{
    UnifiedQueueResult result{};
    result.valid_frames = ValidArrivals();
    const uint32_t count = sample_count_;
    if (count < kMinValidInWindow)
        return result;

    // The samples newest first: index 0 is the newest valid arrival.
    double sorted[kWindow] = {};     // s'
    double cycle_sum_us = 0.0;
    double paced_starve_sum_us = 0.0; // starve of the GPU-paced frames, over the window
    double base_need_sum_us = 0.0;    // cycle - starve - min(hold block, slack), over the newest F
    const uint32_t need_frames_param = static_cast<uint32_t>(std::min<int64_t>(
        std::max<int64_t>(params.need_frames, 1), static_cast<int64_t>(kWindow)));
    const uint32_t need_frames = std::min(need_frames_param, count);
    double need_sum_us = 0.0;
    double service_sum_us = 0.0;
    double newest_min_us = 0.0;     // of s'
    for (uint32_t age = 0; age < count; ++age) {
        const UnifiedQueueSample& sample =
            samples_[(sample_head_ + static_cast<uint32_t>(kWindow) - 1 - age) % static_cast<uint32_t>(kWindow)];
        sorted[age] = sample.s_prime_us;
        cycle_sum_us += sample.cycle_us;
        if (sample.paced)
            paced_starve_sum_us += std::min(sample.starve_us, sample.paced_wait_us);
        // (the need's part of B is summed below, once the slack is known)
        need_sum_us += sample.need_us;
        service_sum_us += sample.service_us;
        if (age == 0 || (age < kGuardFrames && sample.s_prime_us < newest_min_us))
            newest_min_us = sample.s_prime_us;
    }
    // The slack the cap leaves: max(0, J - S^), the quantity the credit's refill funds holds from
    // (S^ the 16-frame service). A hold up to that slack is paid by the cap's headroom, not by the
    // GPU, so it drops out of the base; the part of a hold above the slack stays (no cap, or a cap
    // the GPU cannot reach: S^ >= J, slack 0, and the base is cycle - starve exactly).
    // That is a hold on the presenting queue, which the credit and the advance bound. The render queue's order
    // block never gets here: UnifiedQueueSample_FromRecord took it out of the cycle (under the order a
    // target-bound frame is held I - Delta whatever the lead is, and whatever the credit is while its debt is
    // constant; a changing debt moves the hold one for one, h = I - Delta - the change of the advance. A base
    // that kept the block would follow the interval it caused).
    const double j = std::isfinite(j_us) && j_us > 0.0 ? j_us : 0.0;
    const double service_us = service_sum_us / static_cast<double>(count);
    const double slack_us = std::max(0.0, j - service_us);
    for (uint32_t age = 0; age < need_frames; ++age) {
        const UnifiedQueueSample& sample =
            samples_[(sample_head_ + static_cast<uint32_t>(kWindow) - 1 - age) % static_cast<uint32_t>(kWindow)];
        // need_free + held = (cycle - starve - hold) + max(0, hold - slack)
        base_need_sum_us += sample.cycle_us - sample.starve_us - std::min(sample.hold_block_us, slack_us);
    }
    // B: the need follows the GPU within F frames (the presenting queue's hold blocking beyond the cap's
    // slack included); the starvation of the frames the GPU term paced, as far as their start wait can
    // have caused it, is kept at its 16-frame mean (it cancels the interval's ratchet), the
    // starvation of any other frame, and a paced frame's beyond its wait, is not B's to carry.
    // F = 16 is the 16-frame mean cycle exactly (no gating, no hold term: the first build's base).
    const double base_us = need_frames_param >= kWindow
        ? cycle_sum_us / static_cast<double>(count)
        : base_need_sum_us / static_cast<double>(need_frames) + paced_starve_sum_us / static_cast<double>(count);
    if (!(base_us > 0.0) || !std::isfinite(base_us))
        return result;
    std::sort(sorted, sorted + count);

    const double allow_us = std::isfinite(params.allow_us) && params.allow_us > 0.0 ? params.allow_us : 0.0;
    // The rank is clamped to the number of valid samples in the window.
    const uint32_t rank = static_cast<uint32_t>(
        std::min<int64_t>(std::max<int64_t>(params.rank, 1), static_cast<int64_t>(count)));
    const double ranked_us = sorted[rank - 1];                // q_gpu
    const double median_us = sorted[(count - 1) / 2];
    const double q_us = std::max(ranked_us, median_us - allow_us);
    const double guard_us = std::max(0.0, newest_min_us - 2.0 * allow_us) * kGuardGain;
    // The positive part of P is bound by the GPU's deficit B - J (the cap rule): at a cap the GPU
    // can sustain, B = J and I_gpu <= J.
    // (J: see above.)
    const double deficit_us = std::max(0.0, base_us - j);
    const double clamp_us = base_us * kClampFraction;
    const double raw_us = q_us * kProportionalGain + guard_us;
    const double upper_us = std::min(clamp_us, deficit_us);
    const double p_us = std::min(std::max(raw_us, -clamp_us), upper_us);

    const double need_us = need_sum_us / static_cast<double>(count);
    // (S^ and J: see above.)
    const double sigma = std::isfinite(params.hold_share) && params.hold_share > 0.0 ? params.hold_share : 0.0;

    result.computable = true;
    result.in_use = InUse(planned);
    result.limited = raw_us > deficit_us && deficit_us < clamp_us;
    result.guard_active = guard_us > 0.0;
    result.base_us = base_us;
    result.need_us = need_us;
    result.service_us = service_us;
    result.q_us = q_us;
    result.p_us = p_us;
    result.guard_us = guard_us;
    result.gpu_interval_us = base_us + p_us;
    // The whole-interval bound (F < 16; F = 16 is B = C, an identity): I_gpu never exceeds the
    // mean cycle of the 16 by more than the GPU's deficit to the cap.
    if (need_frames_param < kWindow) {
        const double mean_cycle_us = cycle_sum_us / static_cast<double>(count);
        const double bound_us = mean_cycle_us + std::max(0.0, mean_cycle_us - j);
        if (bound_us < result.gpu_interval_us) {
            result.gpu_interval_us = bound_us;
            result.bound_active = true;
        }
    }
    // The credit's slack and allowance both use the service S^ (the frames' work without the
    // queue's own empty time): a 3.6 ms starve inside a frame is 3.6 ms of slack, not need. N^
    // (empty time counted as need) is kept for analysis only.
    result.refill_slack_us = std::max(0.0, j - service_us);
    result.refill_share_us = std::min(sigma * service_us, std::max(0.0, service_us - j));
    result.refill_us = result.refill_slack_us + result.refill_share_us;
    return result;
}
