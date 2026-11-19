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
    // Nothing queued: empty (inside the frame), boundary idle (before its first submission) and wait idle (a Wait
    // pending with nothing behind it). Each is time the queue had no work, whichever face the interval's excess took.
    const double starve_us = record.empty_us + record.boundary_idle_us + record.wait_idle_us;
    const bool has_margin = (record.valid & PacingGpuQueueRecord::kMinMarginFree) != 0 &&
        NonNegativeFinite(record.min_margin_free_us);
    if (!(starve_us > 0.0) && !has_margin)
        return sample;
    sample.valid = true;
    sample.starve_us = starve_us;
    sample.margin_free_us = has_margin ? record.min_margin_free_us : 0.0;
    // The render queue found by dependency (roles 3 and 4): its hold block is ReLimiter's own order Wait (the gate's
    // hold is on the presenting queue or the frame generation input's source queue), time the queue sat blocked by
    // the pacer's choice and never the GPU's need. Under the order a target-bound frame is held I - Delta whatever
    // the lead is, so a base that kept the block followed the interval it caused (gain 1); it is taken out of the
    // cycle here (the invariant in unified_queue_law.h). N and S below are computed from the record as it is and
    // do not change.
    const bool order_block = UnifiedQueueRole_IsRenderQueueByDependency(record.queue_role);
    sample.cycle_us = order_block ? std::max(0.0, record.cycle_us - record.hold_block_us) : record.cycle_us;
    sample.hold_block_us = order_block ? 0.0 : record.hold_block_us;
    sample.boundary_idle_us = record.boundary_idle_us;
    sample.wait_idle_us = record.wait_idle_us;
    const double raw_need_us =
        record.cycle_us - record.hold_block_us - record.boundary_idle_us - record.wait_idle_us;
    sample.need_us = std::max(0.0, raw_need_us);
    sample.service_us = std::max(0.0, raw_need_us - record.empty_us);
    sample.s_prime_us = starve_us > 0.0 ? -starve_us : sample.margin_free_us;
    // The window judges the kept parts at arrival (Push); alone, all of it is kept.
    sample.lost_us = starve_us + sample.hold_block_us;
    sample.kept_starve_us = starve_us;
    sample.kept_hold_us = sample.hold_block_us;
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
    valid_arrivals_ -= arrivals_[arrival_head_];
    valid_arrivals_ += sample.valid;
    arrivals_[arrival_head_] = sample.valid;
    arrival_head_ = (arrival_head_ + 1) % static_cast<uint32_t>(kWindow);
    if (!sample.valid)
        return;
    measured_ = false;
    UnifiedQueueSample& stored = samples_[sample_head_];
    stored = sample;
    sample_head_ = (sample_head_ + 1) % static_cast<uint32_t>(kWindow);
    if (sample_count_ < kWindow)
        ++sample_count_;
    // The kept parts, judged once against the window as it is now (the header: L and kept): the median loss of
    // the valid samples, this one included (the ring holds exactly them, from index 0). Below it nothing is kept;
    // above it the excess over L, split in proportion to the starvation and the hold block.
    double lost_us[kWindow];
    for (uint32_t i = 0; i < sample_count_; ++i)
        lost_us[i] = samples_[i].lost_us;
    const uint32_t middle = (sample_count_ - 1) / 2;
    std::nth_element(lost_us, lost_us + middle, lost_us + sample_count_);
    const double level_us = lost_us[middle];
    if (level_us > 0.0) {
        const double kept_us = std::max(0.0, stored.lost_us - level_us);
        stored.kept_starve_us = stored.lost_us > 0.0 ? kept_us * (stored.starve_us / stored.lost_us) : 0.0;
        stored.kept_hold_us = std::max(0.0, kept_us - stored.kept_starve_us);
    }
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
    if (sample_count_ < kMinValidInWindow) {
        UnifiedQueueResult result{};
        result.valid_frames = ValidArrivals();
        return result;
    }
    if (!measured_ || measured_j_us_ != j_us || measured_params_.rank != params.rank ||
        measured_params_.allow_us != params.allow_us || measured_params_.need_frames != params.need_frames ||
        measured_params_.hold_share != params.hold_share) {
        Measure(j_us, params);
        measured_j_us_ = j_us;
        measured_params_ = params;
        measured_ = true;
    }
    UnifiedQueueResult result = statistics_;
    result.valid_frames = ValidArrivals();
    result.in_use = result.computable && InUse(planned);
    return result;
}

void UnifiedQueueWindow::Measure(double j_us, const UnifiedQueueParams& params) const noexcept
{
    auto& result = statistics_;
    result = {};
    const uint32_t count = sample_count_;

    // The samples newest first: index 0 is the newest valid arrival.
    double cycle_sum_us = 0.0;
    double paced_starve_sum_us = 0.0; // kept starvation of the GPU-paced frames, over the window
    double base_need_sum_us = 0.0;    // the F-term of B (the header), over the newest F
    const uint32_t need_frames_param = static_cast<uint32_t>(std::min<int64_t>(
        std::max<int64_t>(params.need_frames, 1), static_cast<int64_t>(kWindow)));
    const uint32_t need_frames = std::min(need_frames_param, count);
    double need_sum_us = 0.0;
    double service_sum_us = 0.0;
    double newest_min_us = 0.0;     // of s'
    for (uint32_t age = 0; age < count; ++age) {
        const UnifiedQueueSample& sample =
            samples_[(sample_head_ + static_cast<uint32_t>(kWindow) - 1 - age) % static_cast<uint32_t>(kWindow)];
        if (!measured_) sorted_margins_[age] = sample.s_prime_us;
        cycle_sum_us += sample.cycle_us;
        if (sample.paced)
            paced_starve_sum_us += std::min(sample.kept_starve_us, sample.paced_wait_us);
        // (the need's part of B is summed below, once the slack is known)
        need_sum_us += sample.need_us;
        service_sum_us += sample.service_us;
        if (age == 0 || (age < kGuardFrames && sample.s_prime_us < newest_min_us))
            newest_min_us = sample.s_prime_us;
    }
    // The slack the cap leaves, max(0, J - S^), and the share, min(sigma * S^, max(0, S^ - J)): the credit's two
    // refill terms (S^ the 16-frame service), what pays a hold. A hold block up to the slack is paid by the
    // cap's headroom, not by the GPU, so it drops out of the base, as does the level most of the window's frames
    // are blocked at (the interval's own excess, judged at arrival), except what the share funds; a block
    // beyond both stays (no cap or a cap the GPU cannot reach: slack 0, and only the kept part beyond the level
    // and the share's part of the level remain). That is a hold on the watched queue, which the credit and the
    // advance bound. The render queue's order block never gets here: UnifiedQueueSample_FromRecord took it out of
    // the cycle (under the order a target-bound frame is held I - Delta whatever the lead is, and whatever the
    // credit is while its debt is constant; a changing debt moves the hold one for one, h = I - Delta - the
    // change of the advance. A base that kept the block would follow the interval it caused).
    const double j = std::isfinite(j_us) && j_us > 0.0 ? j_us : 0.0;
    const double service_us = service_sum_us / static_cast<double>(count);
    const double slack_us = std::max(0.0, j - service_us);
    const double sigma = std::isfinite(params.hold_share) && params.hold_share > 0.0 ? params.hold_share : 0.0;
    const double share_us = std::min(sigma * service_us, std::max(0.0, service_us - j));
    for (uint32_t age = 0; age < need_frames; ++age) {
        const UnifiedQueueSample& sample =
            samples_[(sample_head_ + static_cast<uint32_t>(kWindow) - 1 - age) % static_cast<uint32_t>(kWindow)];
        // free of loss + kept block beyond the slack + the share's part of the common level
        const double common_hold_us = std::max(0.0, sample.hold_block_us - sample.kept_hold_us);
        base_need_sum_us += sample.cycle_us - sample.starve_us - sample.hold_block_us +
            std::max(0.0, sample.kept_hold_us - slack_us) + std::min(common_hold_us, share_us);
    }
    // B: the need follows the GPU within F frames (the kept hold blocking beyond the cap's slack and the funded
    // share included); the kept starvation of the frames the GPU term paced, as far as their start wait can
    // have caused it, is kept at its 16-frame mean; the starvation of any other frame, a paced frame's beyond
    // its wait, and the level most frames lose, is not B's to carry.
    // F = 16 is the 16-frame mean cycle exactly (no gating, no hold term: the first build's base).
    const double base_us = need_frames_param >= kWindow
        ? cycle_sum_us / static_cast<double>(count)
        : base_need_sum_us / static_cast<double>(need_frames) + paced_starve_sum_us / static_cast<double>(count);
    // Even when this J/F gives an unusable base, another J/F can use these same samples.
    if (!measured_) std::sort(sorted_margins_, sorted_margins_ + count);
    if (!(base_us > 0.0) || !std::isfinite(base_us))
        return;

    const double allow_us = std::isfinite(params.allow_us) && params.allow_us > 0.0 ? params.allow_us : 0.0;
    // The rank is clamped to the number of valid samples in the window.
    const uint32_t rank = static_cast<uint32_t>(
        std::min<int64_t>(std::max<int64_t>(params.rank, 1), static_cast<int64_t>(count)));
    const double ranked_us = sorted_margins_[rank - 1];       // q_gpu
    const double median_us = sorted_margins_[(count - 1) / 2];
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
    // (S^, J, the slack and the share: see above.)

    result.computable = true;
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
    result.refill_slack_us = slack_us;
    result.refill_share_us = share_us;
    result.refill_us = result.refill_slack_us + result.refill_share_us;
}

UnifiedCarryVotes::Vote UnifiedCarryVotes::Judge(double tail_lo_us, double tail_hi_us, double cycle_lo_us,
    double cycle_hi_us) noexcept
{
    if (!std::isfinite(tail_lo_us) || !std::isfinite(tail_hi_us) || !std::isfinite(cycle_lo_us) ||
        !std::isfinite(cycle_hi_us) || tail_lo_us > tail_hi_us || !(cycle_lo_us > 0.0) || cycle_lo_us > cycle_hi_us)
        return Vote::Undecided;
    if (tail_hi_us < 0.5 * cycle_lo_us)
        return Vote::Carried;
    if (tail_lo_us >= 0.5 * cycle_hi_us)
        return Vote::NotCarried;
    return Vote::Undecided;
}

void UnifiedCarryVotes::Reset() noexcept
{
    *this = UnifiedCarryVotes{};
}

void UnifiedCarryVotes::Note(Vote vote, uint64_t index) noexcept
{
    if (vote == Vote::Undecided)
        return;
    const uint8_t not_carried = vote == Vote::NotCarried ? 1 : 0;
    if (count_ == kWindow)
        not_carried_ -= ring_[head_];
    else
        ++count_;
    ring_[head_] = not_carried;
    not_carried_ += not_carried;
    head_ = (head_ + 1) % kWindow;
    if (index > newest_index_)
        newest_index_ = index;
}

bool UnifiedCarryVotes::Refuses(uint64_t planned_index, uint64_t max_age) const noexcept
{
    if (count_ < kNeeded || planned_index > newest_index_ + max_age)
        return false; // no fresh evidence: the law decides
    return 2 * not_carried_ >= count_;
}
