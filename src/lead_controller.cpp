#include "lead_controller.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    constexpr size_t kRungCount = static_cast<size_t>(PacingRung::Count);
    constexpr double kMicrosecondsPerSecond = 1'000'000.0;
    constexpr double kIntervalSlewDivisor = 64.0;

    bool IsPositiveFinite(double value) noexcept
    {
        return std::isfinite(value) && value > 0.0;
    }

    bool SameLifecycle(const PacingPlanKey& left, const PacingPlanKey& right) noexcept
    {
        return left.identity_domain == right.identity_domain &&
            left.producer_epoch == right.producer_epoch &&
            left.controller_generation == right.controller_generation;
    }

    bool IsGpuRung(PacingRung rung) noexcept
    {
        return rung == PacingRung::GpuQueueWait || rung == PacingRung::FgInputWait;
    }

    // An OutputRelease ends a hold unless the executor reported that it did
    // not hold (Late, Undeliverable, Missing, ...). Unknown counts as a hold.
    bool IsHeldStatus(PacingPlanStatus status) noexcept
    {
        return status == PacingPlanStatus::Held || status == PacingPlanStatus::Unknown;
    }

    double Overlap(double first_begin, double first_end, double second_begin, double second_end) noexcept
    {
        const double overlap = std::min(first_end, second_end) - std::max(first_begin, second_begin);
        return overlap > 0.0 ? overlap : 0.0;
    }
}

// ── Sample window ──

void LeadSampleWindow::Reset() noexcept
{
    ring_bins_.fill(0);
    bin_counts_.fill(0);
    block_counts_.fill(0);
    beyond_count_ = 0;
    head_ = 0;
    count_ = 0;
}

uint32_t LeadSampleWindow::BinOf(double sample_us) noexcept
{
    if (!(sample_us > 0.0)) return 0;
    const double bin = std::floor(sample_us / LEAD_HISTOGRAM_BIN_US);
    constexpr double kMaxBin = static_cast<double>(std::numeric_limits<uint32_t>::max());
    return bin < kMaxBin ? static_cast<uint32_t>(bin) : std::numeric_limits<uint32_t>::max();
}

double LeadSampleWindow::BinCenterUs(uint32_t bin) noexcept
{
    return (static_cast<double>(bin) + 0.5) * LEAD_HISTOGRAM_BIN_US;
}

void LeadSampleWindow::Add(double sample_us) noexcept
{
    const uint32_t bin = BinOf(sample_us);
    if (count_ == LEAD_WINDOW_CAPACITY) {
        const uint32_t evicted = ring_bins_[head_];
        if (evicted < LEAD_HISTOGRAM_BINS) {
            --bin_counts_[evicted];
            --block_counts_[evicted / kBlockBins];
        } else {
            --beyond_count_;
        }
    } else {
        ++count_;
    }
    ring_bins_[head_] = bin;
    if (bin < LEAD_HISTOGRAM_BINS) {
        ++bin_counts_[bin];
        ++block_counts_[bin / kBlockBins];
    } else {
        ++beyond_count_;
    }
    head_ = (head_ + 1) % static_cast<uint32_t>(LEAD_WINDOW_CAPACITY);
}

double LeadSampleWindow::Quantile(double probability) const noexcept
{
    if (count_ == 0) return 0.0;
    const double clamped = probability > 0.0 ? (probability < 1.0 ? probability : 1.0) : 0.0;
    size_t rank = static_cast<size_t>(std::floor(clamped * static_cast<double>(count_ - 1)));
    if (rank >= count_) rank = count_ - 1;
    const size_t in_histogram = count_ - beyond_count_;
    if (rank >= in_histogram) {
        // Among the samples beyond the histogram: rank their bins from the
        // ring (positions below count_ hold the newest count_ samples).
        std::array<uint32_t, LEAD_WINDOW_CAPACITY> beyond{};
        size_t found = 0;
        for (size_t index = 0; index < count_; ++index) {
            if (ring_bins_[index] >= LEAD_HISTOGRAM_BINS) beyond[found++] = ring_bins_[index];
        }
        const size_t nth = rank - in_histogram;
        std::nth_element(beyond.begin(), beyond.begin() + static_cast<std::ptrdiff_t>(nth),
            beyond.begin() + static_cast<std::ptrdiff_t>(found));
        return BinCenterUs(beyond[nth]);
    }
    // Samples before the current block/bin; the ranked sample is the first
    // position where the running count passes its rank.
    size_t before = 0;
    size_t block = 0;
    while (block + 1 < kBlockCount && before + block_counts_[block] <= rank) {
        before += block_counts_[block];
        ++block;
    }
    size_t bin = block * kBlockBins;
    while (bin + 1 < LEAD_HISTOGRAM_BINS && before + bin_counts_[bin] <= rank) {
        before += bin_counts_[bin];
        ++bin;
    }
    return BinCenterUs(static_cast<uint32_t>(bin));
}

// ── Release margin and headroom ──

void ReleaseMarginLearner::Reset() noexcept
{
    *this = ReleaseMarginLearner{ limits_ };
}

size_t ReleaseMarginLearner::BinOf(double margin_us) noexcept
{
    if (!(margin_us > 0.0)) return 0;
    const double bin = std::floor(margin_us / LEAD_MARGIN_BIN_US);
    return bin >= static_cast<double>(LEAD_MARGIN_BINS - 1) ? LEAD_MARGIN_BINS - 1 : static_cast<size_t>(bin);
}

// More late resumes in the bin than the background share plus
// LEAD_MARGIN_EXCESS_SHARE explains, at z > LEAD_MARGIN_EXCESS_Z.
bool ReleaseMarginLearner::Colliding(size_t bin, double background) const noexcept
{
    const double resumes = resumes_[bin];
    const double late = late_[bin];
    if (resumes < LEAD_MARGIN_MIN_SAMPLES || late < LEAD_MARGIN_MIN_LATE) return false;
    const double share = std::min(0.5, background + LEAD_MARGIN_EXCESS_SHARE);
    return (late - resumes * share) / std::sqrt(resumes * share * (1.0 - share)) > LEAD_MARGIN_EXCESS_Z;
}

int ReleaseMarginLearner::Add(double margin_us, bool late) noexcept
{
    if (!std::isfinite(margin_us)) return 0;
    constexpr double kKeep = 1.0 - 1.0 / LEAD_MARGIN_DECAY_SAMPLES;
    for (size_t bin = 0; bin < LEAD_MARGIN_BINS; ++bin) {
        resumes_[bin] *= kKeep;
        late_[bin] *= kKeep;
    }
    const size_t added = BinOf(margin_us);
    resumes_[added] += 1.0;
    if (late) late_[added] += 1.0;

    // Background: the late share at margins at least 1 ms above M. Nothing
    // is judged before it is measured.
    const size_t background_bin = BinOf(margin_us_ + 1000.0);
    double background_resumes = 0.0;
    double background_late = 0.0;
    for (size_t bin = background_bin; bin < LEAD_MARGIN_BINS; ++bin) {
        background_resumes += resumes_[bin];
        background_late += late_[bin];
    }
    if (background_resumes < LEAD_MARGIN_BACKGROUND_SAMPLES) {
        narrow_run_ = 0;
        return 0;
    }
    const double background = background_late / background_resumes;

    double edge_us = EdgeUs();
    // Widen through the colliding bins upward from the edge, below the
    // background: bins with too few resumes are skipped, and the first
    // measured bin that does not collide ends the scan, so lateness away
    // from the collision window cannot move the edge.
    for (size_t bin = static_cast<size_t>(std::floor(edge_us / LEAD_MARGIN_BIN_US + 0.5));
         bin < background_bin; ++bin) {
        if (resumes_[bin] < LEAD_MARGIN_MIN_SAMPLES) continue;
        if (!Colliding(bin, background)) break;
        edge_us = static_cast<double>(bin + 1) * LEAD_MARGIN_BIN_US;
        narrow_run_ = 0;
    }
    // Narrow one bin after the bin just below the edge stayed clean (well
    // measured, and late at most LEAD_MARGIN_CLEAN_SHARE above the background)
    // or, above the start, held fewer late resumes than a collision needs.
    // The guard keeps releases about a cushion above the edge, so few resumes
    // land below it to measure it clean; without the second test a widening
    // would stand until the game closes instead of fading with its evidence.
    const double below = std::floor(edge_us / LEAD_MARGIN_BIN_US + 0.5) - 1.0;
    const size_t below_bin = below >= 0.0 ? static_cast<size_t>(below) : LEAD_MARGIN_BINS;
    const bool clean = below_bin < LEAD_MARGIN_BINS && resumes_[below_bin] >= LEAD_MARGIN_CLEAN_SAMPLES &&
        late_[below_bin] <= resumes_[below_bin] * (background + LEAD_MARGIN_CLEAN_SHARE);
    const bool faded = below_bin < LEAD_MARGIN_BINS && edge_us > limits_.start_us - LEAD_MARGIN_CUSHION_US &&
        late_[below_bin] < LEAD_MARGIN_MIN_LATE;
    if (clean || faded) {
        if (++narrow_run_ >= LEAD_MARGIN_NARROW_SAMPLES) {
            edge_us -= LEAD_MARGIN_BIN_US;
            narrow_run_ = 0;
        }
    } else {
        narrow_run_ = 0;
    }

    const double margin = std::clamp(edge_us + LEAD_MARGIN_CUSHION_US, limits_.min_us, limits_.max_us);
    const int step = margin > margin_us_ ? 1 : (margin < margin_us_ ? -1 : 0);
    margin_us_ = margin;
    return step;
}

// ── Lifecycle ──

void LeadController::Reset(int64_t qpc_frequency) noexcept
{
    for (Bucket& bucket : buckets_) ResetBucket(&bucket);
    for (PendingFrame& frame : pending_) frame = PendingFrame{};
    resume_delays_us_.fill(0.0);
    resume_delay_head_ = 0;
    resume_delay_count_ = 0;
    for (LeadSampleWindow& calls : call_windows_) calls.Reset();
    for (LeadSampleWindow& work : work_windows_) work.Reset();
    work_age_frames_.fill(0);
    for (LeadSampleWindow& room : room_windows_) room.Reset();
    handoff_window_.Reset();
    handoff_stream_frames_ = 0;
    report_pair_epoch_ = 0;
    report_pair_frame_ = 0;
    gpu_end_epoch_ = 0;
    gpu_end_frame_ = 0;
    gpu_end_qpc_ = 0;
    gpu_period_window_.Reset();
    gpu_after_present_window_.Reset();
    first_submission_window_.Reset();
    release_to_render_window_.Reset();
    lifecycle_known_ = false;
    lifecycle_ = PacingPlanKey{};
    newest_index_ = 0;
    statistics_ = LeadStatistics{};
    qpc_frequency_ = qpc_frequency > 0 ? qpc_frequency : 0;
    if (qpc_frequency <= 0) ++statistics_.invalid_inputs;
}

void LeadController::ResetBuckets() noexcept
{
    for (Bucket& bucket : buckets_) ResetBucket(&bucket);
    for (LeadSampleWindow& calls : call_windows_) calls.Reset();
    for (LeadSampleWindow& work : work_windows_) work.Reset();
    work_age_frames_.fill(0);
    for (LeadSampleWindow& room : room_windows_) room.Reset();
    handoff_window_.Reset();
    gpu_period_window_.Reset();
    gpu_after_present_window_.Reset();
    first_submission_window_.Reset();
    release_to_render_window_.Reset();
    for (PendingFrame& frame : pending_) {
        if (frame.occupied && frame.started) frame.finalized = true;
        frame.lead_sample_waiting = false; // its sample belongs to the old buckets
    }
}

void LeadController::ClearSlackEvidence() noexcept
{
    for (Bucket& bucket : buckets_) bucket.slack = SlackWindow{};
}

// The rung resumes holding after the overload guard demoted it: its
// window describes frames from before the demotion (retries started above
// the frames' current Q95, waited and failed again within seconds). The
// window restarts; until it is warm again the lead
// is the old window's median plus the allowance, not the long-interval seed.
// A GPU wait's base is at least the warm source-only Q_q of the same FG
// divisor: the demoted frames fed that window, and a GPU wait frame is ready
// no earlier than its PRESENT_START. Without it a load rise during the
// demotion resumed the wait from a window learned before it (a lead learned
// while loading was far below the X seen in play).
void LeadController::RestartRung(PacingRung rung) noexcept
{
    size_t first = 0;
    if (!FindBucketIndex(rung, 1.0, &first)) return;
    size_t source_first = 0;
    const bool floored = IsGpuRung(rung) && FindBucketIndex(PacingRung::SourceOnly, 1.0, &source_first);
    for (size_t offset = 0; offset < LEAD_DIVISOR_BUCKETS; ++offset) {
        Bucket& bucket = buckets_[first + offset];
        if (bucket.window.Count() == 0) continue; // never sampled, or already restarted
        double median_us = bucket.window.Count() >= LEAD_WARM_SAMPLES
            ? bucket.window.Quantile(0.5)
            : bucket.reseed_x_us;
        if (floored) {
            const Bucket& source = buckets_[source_first + offset];
            if (source.window.Count() >= LEAD_WARM_SAMPLES) median_us = std::max(median_us, source.quantile_us);
        }
        ResetBucket(&bucket);
        bucket.reseed_x_us = median_us;
        ++statistics_.restarted_buckets;
    }
}

// ── Lead ──

double LeadController::NextLeadUs(const LeadRequest& request, bool* budget_limited,
    LeadGuardOutcome* guard) noexcept
{
    if (budget_limited != nullptr) *budget_limited = false;
    if (guard != nullptr) *guard = LeadGuardOutcome{};
    if (request.placement == PacingSourcePlacement::Present) return LEAD_PRESENT_PLACEMENT_US;
    if (request.placement == PacingSourcePlacement::None ||
        request.mode == PacingTimingMode::Off || request.mode == PacingTimingMode::Idle) {
        return 0.0;
    }
    size_t bucket_index = 0;
    if (request.placement != PacingSourcePlacement::Marker ||
        (request.mode != PacingTimingMode::Vrr && request.mode != PacingTimingMode::Fixed) ||
        !IsPositiveFinite(request.interval_us) ||
        !FindBucketIndex(request.rung, request.fg_divisor, &bucket_index)) {
        ++statistics_.invalid_inputs;
        return 0.0;
    }

    Bucket& bucket = buckets_[bucket_index];
    const double interval_us = request.interval_us;
    const double max_lead_us = LEAD_BUDGET_INTERVALS * interval_us;
    bucket.quantile_us = bucket.window.Quantile(ValidQuantile(request.quantile));
    if (bucket.window.Count() < LEAD_WARM_SAMPLES) {
        bucket.target_us = bucket.reseed_x_us > 0.0
            ? bucket.reseed_x_us + ValidAllowance(request.allowance_us)
            : std::clamp(interval_us / 2.0, LEAD_SEED_MIN_US, LEAD_SEED_MAX_US);
        bucket.lead_us = std::min(bucket.target_us, max_lead_us);
        bucket.budget_us = max_lead_us;
        bucket.budget_limited = false;
        return CollisionGuardedLeadUs(request, bucket_index, bucket.lead_us, guard);
    }

    bucket.budget_us = max_lead_us;
    bucket.target_us = bucket.quantile_us + ValidAllowance(request.allowance_us);
    // GPU wait cap (rung_feasibility.h GpuWaitCap; gpu_wait_budget_us): a
    // typical GPU queue wait frame waits at most the cap before its target.
    if (request.rung == PacingRung::GpuQueueWait && IsPositiveFinite(request.gpu_wait_cap_us)) {
        const double capped_us = bucket.window.Quantile(0.5) + request.gpu_wait_cap_us;
        if (capped_us < bucket.target_us) {
            bucket.target_us = capped_us;
            ++statistics_.gpu_wait_capped_frames;
        }
    }
    // Past 3 I only the Adaptive cap, if the user enabled it, can help.
    bucket.budget_limited = bucket.target_us > max_lead_us;
    if (bucket.budget_limited) ++statistics_.budget_limited_frames;
    if (budget_limited != nullptr) *budget_limited = bucket.budget_limited;
    if (!bucket.warm_applied) {
        bucket.lead_us = bucket.target_us;
        bucket.warm_applied = true;
    } else {
        const double step_us = std::min(LEAD_SLEW_MAX_US, interval_us / kIntervalSlewDivisor);
        bucket.lead_us += std::clamp(bucket.target_us - bucket.lead_us, -step_us, step_us);
    }
    bucket.lead_us = std::clamp(bucket.lead_us, 0.0, max_lead_us);
    return CollisionGuardedLeadUs(request, bucket_index, bucket.lead_us, guard);
}

// The GPU queue wait lead at most I + Q10(D) − H (release headroom) while the
// work window is warm and current, else at most I + Q10(C) − M (release
// margin) while the call window is warm (lead_controller.h). Neither while
// the newest queue-order pairs show the next frame's GPU work running behind
// the wait: it cannot delay the resume there.
double LeadController::CollisionGuardedLeadUs(const LeadRequest& request, size_t bucket_index,
    double lead_us, LeadGuardOutcome* guard) noexcept
{
    if (request.rung != PacingRung::GpuQueueWait || !request.collision_guard || NextGpuWorkBehindWait()) {
        return lead_us;
    }
    const size_t divisor = bucket_index % LEAD_DIVISOR_BUCKETS;
    const LeadSampleWindow& work = work_windows_[divisor];
    const LeadSampleWindow& calls = call_windows_[divisor];
    double limit_us = 0.0;
    if (work.Count() >= LEAD_WARM_SAMPLES && work_age_frames_[divisor] <= LEAD_WORK_STALE_FRAMES) {
        limit_us = request.interval_us + work.Quantile(LEAD_WORK_QUANTILE) - headroom_.MarginUs();
        ++statistics_.headroom_checked_frames;
        statistics_.last_guard_headroom = true;
    } else if (calls.Count() >= LEAD_WARM_SAMPLES) {
        limit_us = request.interval_us + calls.Quantile(LEAD_CALL_QUANTILE) - margin_.MarginUs();
        statistics_.last_guard_headroom = false;
    } else {
        return lead_us;
    }
    ++statistics_.collision_checked_frames;
    statistics_.last_collision_limit_us = limit_us;
    if (guard != nullptr) {
        guard->judged = true;
        guard->offset_us = limit_us - request.interval_us;
    }
    if (lead_us <= limit_us) return lead_us;
    ++statistics_.collision_guarded_frames;
    const double guarded_us = std::max(0.0, limit_us);
    if (guard != nullptr) guard->reduction_us = lead_us - guarded_us;
    return guarded_us;
}

double LeadController::MedianSampleUs(PacingRung rung, double fg_divisor) const noexcept
{
    size_t index = 0;
    if (!FindBucketIndex(rung, fg_divisor, &index)) return 0.0;
    const LeadSampleWindow& window = buckets_[index].window;
    if (window.Count() < LEAD_WARM_SAMPLES) return 0.0;
    return window.Quantile(0.5);
}

double LeadController::EarlyLeadUs(PacingRung rung, double fg_divisor, double quantile,
    double allowance_us) const noexcept
{
    size_t index = 0;
    if (!std::isfinite(quantile) || !std::isfinite(allowance_us) ||
        !FindBucketIndex(rung, fg_divisor, &index)) {
        return 0.0;
    }
    const LeadSampleWindow& window = buckets_[index].window;
    if (window.Count() < LEAD_WARM_SAMPLES) return 0.0;
    return std::max(0.0, window.Quantile(1.0 - std::clamp(quantile, 0.0, 1.0)) -
        std::max(allowance_us, 0.0));
}

double LeadController::HoldRoomUs(double fg_divisor, double quantile) const noexcept
{
    size_t index = 0;
    if (!std::isfinite(quantile) || !FindBucketIndex(PacingRung::GpuQueueWait, fg_divisor, &index)) return 0.0;
    // No collision limit while the next frame's GPU work runs behind the
    // wait (queue order), so no room to make for it.
    if (NextGpuWorkBehindWait()) return 0.0;
    const LeadSampleWindow& room = room_windows_[index % LEAD_DIVISOR_BUCKETS];
    if (room.Count() < LEAD_WARM_SAMPLES) return 0.0;
    return room.Quantile(std::clamp(quantile, 0.0, 1.0));
}

double LeadController::HandoffOffsetUs(double quantile, double allowance_us) const noexcept
{
    if (!std::isfinite(quantile) || !std::isfinite(allowance_us) ||
        handoff_window_.Count() < LEAD_HANDOFF_MIN_SAMPLES) {
        return 0.0;
    }
    return std::max(0.0, handoff_window_.Quantile(1.0 - std::clamp(quantile, 0.0, 1.0)) -
        std::max(allowance_us, 0.0));
}

double LeadController::GpuFloorStartUs() const noexcept
{
    if (gpu_period_window_.Count() < LEAD_WARM_SAMPLES || first_submission_window_.Count() < LEAD_WARM_SAMPLES) {
        return 0.0;
    }
    return gpu_period_window_.Quantile(0.5) / 2.0 + first_submission_window_.Quantile(0.5);
}

GpuFloorAnchor LeadController::GpuFloorAnchorSnapshot() const noexcept
{
    GpuFloorAnchor anchor;
    const double start_us = GpuFloorStartUs();
    if (!(start_us > 0.0) || qpc_frequency_ <= 0 || gpu_end_epoch_ == 0 || gpu_end_qpc_ <= 0) return anchor;
    anchor.valid = true;
    anchor.identity_domain = static_cast<uint8_t>(PacingIdentityDomain::NvApiFrame);
    anchor.producer_epoch = gpu_end_epoch_;
    anchor.gpu_end_frame = gpu_end_frame_;
    anchor.gpu_end_qpc = gpu_end_qpc_;
    anchor.qpc_frequency = qpc_frequency_;
    anchor.period_us = gpu_period_window_.Quantile(0.5);
    anchor.present_gap_warm = gpu_after_present_window_.Count() >= LEAD_WARM_SAMPLES;
    anchor.present_to_gpu_end_us = anchor.present_gap_warm
        ? gpu_after_present_window_.Quantile(LEAD_HANDOFF_GPU_BOUND_QUANTILE) : 0.0;
    anchor.start_us = start_us;
    anchor.release_to_render_warm = release_to_render_window_.Count() >= LEAD_WARM_SAMPLES;
    anchor.release_to_render_us = anchor.release_to_render_warm ? release_to_render_window_.Quantile(0.5) : 0.0;
    return anchor;
}

int64_t LeadController::GpuFloorQpc(const PacingPlanKey& key, uint64_t present_end_frame,
    int64_t present_end_qpc) const noexcept
{
    // Ĝ(n − 1) − (P/2 + F + σ).
    return gpu_floor::FloorQpc(GpuFloorAnchorSnapshot(), static_cast<uint8_t>(key.identity_domain),
        key.producer_epoch, key.frame_id, present_end_frame, present_end_qpc);
}

int64_t LeadController::GpuRenderStartQpc(const PacingPlanKey& key, uint64_t present_end_frame,
    int64_t present_end_qpc) const noexcept
{
    // Ĝ(n − 1) − (P/2 + F).
    return gpu_floor::RenderStartQpc(GpuFloorAnchorSnapshot(), static_cast<uint8_t>(key.identity_domain),
        key.producer_epoch, key.frame_id, present_end_frame, present_end_qpc);
}

LeadBucketStatus LeadController::BucketStatus(PacingRung rung, double fg_divisor) const noexcept
{
    LeadBucketStatus status{};
    size_t index = 0;
    if (!FindBucketIndex(rung, fg_divisor, &index)) return status;
    const Bucket& bucket = buckets_[index];
    status.samples = bucket.window.Count();
    status.warm = status.samples >= LEAD_WARM_SAMPLES;
    status.quantile_us = bucket.quantile_us;
    status.target_us = bucket.target_us;
    status.lead_us = bucket.lead_us;
    status.budget_us = bucket.budget_us;
    status.median_us = status.warm ? bucket.window.Quantile(0.5) : 0.0;
    status.excluded_long = bucket.excluded_long;
    status.budget_limited = bucket.budget_limited;
    return status;
}

LeadFeasibility LeadController::Feasibility(PacingRung rung, double fg_divisor) const noexcept
{
    LeadFeasibility feasibility{};
    size_t index = 0;
    if (!FindBucketIndex(rung, fg_divisor, &index)) return feasibility;
    const Bucket& bucket = buckets_[index];
    feasibility.samples = bucket.slack.count;
    feasibility.negative_count = bucket.slack.negative_count;
    feasibility.budget_limited = bucket.budget_limited;
    return feasibility;
}

double LeadController::ValidQuantile(double quantile) noexcept
{
    if (std::isfinite(quantile) && quantile >= 0.0 && quantile <= 1.0) return quantile;
    ++statistics_.invalid_inputs;
    return std::isfinite(quantile) ? std::clamp(quantile, 0.0, 1.0) : LeadRequest{}.quantile;
}

double LeadController::ValidAllowance(double allowance_us) noexcept
{
    if (std::isfinite(allowance_us) && allowance_us >= 0.0) return allowance_us;
    ++statistics_.invalid_inputs;
    return std::isfinite(allowance_us) ? 0.0 : LeadRequest{}.allowance_us;
}

// ── Stage inputs ──

void LeadController::OnSourceRelease(const LeadFrameStart& frame) noexcept
{
    size_t bucket_index = 0;
    if (qpc_frequency_ <= 0 || !PacingPlanKey_IsValid(frame.key) || frame.source_release_qpc <= 0 ||
        !IsPositiveFinite(frame.interval_us) || !std::isfinite(frame.lead_us) || frame.lead_us < 0.0 ||
        !std::isfinite(frame.guard.reduction_us) || frame.guard.reduction_us < 0.0 ||
        !std::isfinite(frame.guard.offset_us) ||
        !FindBucketIndex(frame.rung, frame.fg_divisor, &bucket_index)) {
        ++statistics_.invalid_inputs;
        return;
    }

    const uint64_t index = frame.grid_index;
    if (!lifecycle_known_ || !SameLifecycle(frame.key, lifecycle_)) {
        StartLifecycle(frame.key, index, false);
    } else if (index + LEAD_PENDING_EXPIRY_FRAMES <= newest_index_) {
        StartLifecycle(frame.key, index, true); // the grid index restarted
    } else if (index > newest_index_) {
        newest_index_ = index;
        ExpireBehindNewest();
    }

    PendingFrame& slot = pending_[static_cast<size_t>(index % LEAD_PENDING_CAPACITY)];
    if (slot.occupied && slot.grid_index == index && slot.started) {
        ++statistics_.invalid_inputs; // duplicate release
        return;
    }
    if (slot.occupied && (slot.grid_index != index || slot.key != frame.key)) Discard(&slot);
    if (!slot.occupied) {
        slot.occupied = true;
        slot.key = frame.key;
        slot.grid_index = index;
    }
    slot.started = true;
    slot.sampled = frame.mode == PacingTimingMode::Vrr || frame.mode == PacingTimingMode::Fixed;
    slot.finalized = !slot.sampled;
    slot.rung = frame.rung;
    slot.bucket = static_cast<uint8_t>(bucket_index);
    slot.interval_us = frame.interval_us;
    slot.lead_us = frame.lead_us;
    slot.guard_reduction_us = frame.guard.reduction_us;
    slot.guard_offset_us = frame.guard.offset_us;
    slot.guard_judged = frame.guard.judged && frame.rung == PacingRung::GpuQueueWait;
    // ReLimiter's waits before this release (lockstep engines, lead_controller.h).
    const auto valid_wait = [&](int64_t start_qpc, int64_t end_qpc) noexcept {
        return start_qpc > 0 && start_qpc < end_qpc && end_qpc <= frame.source_release_qpc;
    };
    if (valid_wait(frame.wait_start_qpc, frame.wait_end_qpc)) {
        slot.wait_start_qpc = frame.wait_start_qpc;
        slot.wait_end_qpc = frame.wait_end_qpc;
    }
    if (valid_wait(frame.pre_wait_start_qpc, frame.pre_wait_end_qpc)) {
        slot.pre_wait_start_qpc = frame.pre_wait_start_qpc;
        slot.pre_wait_end_qpc = frame.pre_wait_end_qpc;
    }
    slot.gpu_resume_probe = frame.gpu_resume_probe;
    slot.render_queue = frame.render_queue;
    slot.driver_hold_exclusion = frame.driver_hold_exclusion;
    SetField(&slot, FieldSource, frame.source_release_qpc);
    RecordReleaseToRender(&slot);
    if (slot.sampled && slot.rung == PacingRung::GpuQueueWait) {
        uint32_t& age = work_age_frames_[slot.bucket % LEAD_DIVISOR_BUCKETS];
        if (age < UINT32_MAX) ++age;
    }
    RecordCallTime(&slot);
    RecordWorkStart(&slot);
    if (index > 0) RecordReleaseHeadroom(Find(index - 1));
    TryFinalize(&slot);
    TryFinalize(Find(index + 1));
}

void LeadController::OnStageEvent(const PacingStageEvent& event) noexcept
{
    static_assert(static_cast<int>(PacingStageEventKind::MarkerArrival) == FieldMarkerArrival &&
        static_cast<int>(PacingStageEventKind::NativeArrival) == FieldNativeArrival &&
        static_cast<int>(PacingStageEventKind::PresentCall) == FieldPresentCall &&
        static_cast<int>(PacingStageEventKind::OutputRelease) == FieldOutputRelease &&
        static_cast<int>(PacingStageEventKind::GpuReady) == FieldGpuReady &&
        static_cast<int>(PacingStageEventKind::GpuResume) == FieldGpuResume &&
        static_cast<int>(PacingStageEventKind::RenderSubmitStart) == FieldRenderSubmit &&
        static_cast<int>(PacingStageEventKind::SimulationEnd) == FieldSimulationEnd &&
        static_cast<int>(PacingStageEventKind::PresentEnd) == FieldPresentEnd,
        "Field values must match PacingStageEventKind");
    static_assert(FieldCount <= 16, "field_mask holds one bit per Field");

    const bool start_hold_event = event.kind == PacingStageEventKind::RenderStartHold;
    const bool hold_event = start_hold_event || event.kind == PacingStageEventKind::DriverPresentHold;
    const uint8_t kind = static_cast<uint8_t>(event.kind);
    if (qpc_frequency_ <= 0 || event.qpc <= 0 || !PacingPlanKey_IsValid(event.key) ||
        (hold_event ? event.hold_ticks <= 0 : (kind == FieldSource || kind >= FieldCount))) {
        ++statistics_.invalid_inputs;
        return;
    }

    const uint64_t index = event.grid_index;
    PendingFrame* frame = Find(index);
    if (frame == nullptr) {
        // Only an event within the pending window of the current lifecycle
        // may open a frame ahead of its source release.
        if (lifecycle_known_ && (!SameLifecycle(event.key, lifecycle_) ||
                index + LEAD_PENDING_EXPIRY_FRAMES <= newest_index_ ||
                index > newest_index_ + LEAD_PENDING_AHEAD_FRAMES)) {
            ++statistics_.unmatched_events;
            return;
        }
        frame = &pending_[static_cast<size_t>(index % LEAD_PENDING_CAPACITY)];
        Discard(frame);
        frame->occupied = true;
        frame->key = event.key;
        frame->grid_index = index;
    } else if (frame->key != event.key) {
        ++statistics_.unmatched_events;
        return;
    }

    if (hold_event) {
        // Not a stage time. The render-thread start hold leaves this frame's
        // X and σ (RenderStartHoldUs): the push precedes the frame's
        // RENDERSUBMIT_START event, so it is known before σ and any sample
        // are taken; the first value wins. The driver's Present hold is kept
        // for the next frame's sample (DriverHoldUs); the longest call wins.
        if (start_hold_event) {
            if (frame->render_start_hold_ticks == 0) frame->render_start_hold_ticks = event.hold_ticks;
        } else if (event.hold_ticks > frame->driver_hold_ticks) {
            frame->driver_hold_ticks = event.hold_ticks;
        }
        return;
    }
    const Field field = static_cast<Field>(kind);
    if (HasField(*frame, field)) return; // the first value of each kind wins
    SetField(frame, field, event.qpc);
    if (field == FieldOutputRelease) frame->output_status = event.status;
    if (field == FieldGpuResume) frame->gpu_wait_qpc = event.gpu_wait_qpc;
    if (!frame->started) ++frame->early_events;
    RecordResumeDelay(frame);
    RecordCallTime(frame);
    RecordWorkStart(frame);
    RecordReleaseToRender(frame);
    // p_{n+1} − o_n: this frame's resume, or the next frame's Present call.
    RecordReleaseMargin(frame);
    // r_{n+1} − A_{n+1} − o_n: this frame's resume, or the next frame's readiness.
    RecordReleaseHeadroom(frame);
    if (index > 0) RecordReleaseHeadroom(Find(index - 1));
    if (index > 0) RecordReleaseMargin(Find(index - 1));
    // g_{n+1} − o_n: this frame's resume, or the next frame's submission start.
    RecordConcurrency(frame);
    if (index > 0) RecordConcurrency(Find(index - 1));
    TryFinalize(frame);
    TryFinalize(Find(index + 1));
}

// The Reflex report of an NvAPI frame arrives about three frames after its
// source release, while the frame is still in the pending table (32 slots).
void LeadController::OnGpuActiveTime(uint32_t producer_epoch, uint64_t frame_id, double gpu_active_us,
    int64_t gpu_start_qpc) noexcept
{
    if (qpc_frequency_ <= 0 || producer_epoch == 0 || !IsPositiveFinite(gpu_active_us)) {
        ++statistics_.invalid_inputs;
        return;
    }
    for (PendingFrame& frame : pending_) {
        if (!frame.occupied || !frame.started || frame.key.identity_domain != PacingIdentityDomain::NvApiFrame ||
            frame.key.producer_epoch != producer_epoch || frame.key.frame_id != frame_id) {
            continue;
        }
        if (frame.gpu_active_us > 0.0) return; // the first report wins
        frame.gpu_active_us = gpu_active_us;
        frame.gpu_start_qpc = gpu_start_qpc > 0 ? gpu_start_qpc : 0;
        RecordWorkStart(&frame);
        if (frame.grid_index > 0) {
            RecordReleaseHeadroom(Find(frame.grid_index - 1));
            RecordConcurrency(Find(frame.grid_index - 1));
        }
        return;
    }
    ++statistics_.unmatched_gpu_active;
}

// Report pairs, the held-back wait and the GPU floor (lead_controller.h).
namespace
{
    bool ValidPair(const HandoffBackPressureSample& sample) noexcept
    {
        const double values[] = { sample.simulation_end_us, sample.render_start_us, sample.render_end_us,
            sample.first_submission_us, sample.gpu_start_us, sample.previous_render_free_us,
            sample.previous_gpu_end_us };
        for (const double value : values) {
            if (!std::isfinite(value)) return false;
        }
        return sample.render_end_us > sample.render_start_us;
    }

    bool LateInWindow(const HandoffBackPressureSample& sample) noexcept
    {
        return sample.first_submission_us >= (sample.render_start_us + sample.render_end_us) / 2.0;
    }
}

bool HandoffBackPressure_HeldBack(const HandoffBackPressureSample& sample, bool streams) noexcept
{
    if (!ValidPair(sample)) return false;
    const bool gpu_waited = sample.gpu_start_us - sample.previous_gpu_end_us > LEAD_HANDOFF_GPU_IDLE_US;
    // The render thread went straight from frame n − 1 into frame n: the
    // simulation had ended before it was free.
    const bool render_busy = sample.simulation_end_us < sample.previous_render_free_us;
    return !gpu_waited && LateInWindow(sample) && (render_busy || streams);
}

bool HandoffBackPressure_Streams(const HandoffBackPressureSample& sample) noexcept
{
    return ValidPair(sample) && !LateInWindow(sample);
}

void LeadController::OnReportPair(uint32_t producer_epoch, uint64_t frame_id,
    const HandoffBackPressureSample& sample) noexcept
{
    if (producer_epoch == 0 || frame_id == 0) {
        ++statistics_.invalid_inputs;
        return;
    }
    if (producer_epoch != report_pair_epoch_) {
        report_pair_epoch_ = producer_epoch;
        report_pair_frame_ = frame_id;
    } else if (frame_id > report_pair_frame_) {
        report_pair_frame_ = frame_id;
    }
    if (HandoffBackPressure_Streams(sample)) {
        if (handoff_stream_frames_ < LEAD_HANDOFF_STREAM_PROOF) ++handoff_stream_frames_;
        first_submission_window_.Add(sample.first_submission_us - sample.render_start_us);
    }
    for (PendingFrame& frame : pending_) {
        if (!frame.occupied || !frame.started || frame.key.identity_domain != PacingIdentityDomain::NvApiFrame ||
            frame.key.producer_epoch != producer_epoch || frame.key.frame_id != frame_id) {
            continue;
        }
        if (frame.report_paired) return;
        frame.report_paired = true;
        ++statistics_.report_pairs;
        if (HandoffBackPressure_HeldBack(sample, HandoffStreams())) {
            ++statistics_.held_back_frames;
            if (first_submission_window_.Count() >= LEAD_WARM_SAMPLES) {
                frame.held_back_wait_us = std::max(0.0, sample.first_submission_us - sample.render_start_us -
                    first_submission_window_.Quantile(0.5));
            }
        }
        if (frame.lead_sample_waiting) {
            frame.lead_sample_waiting = false;
            CommitLeadSample(&frame, frame.waiting_lead_sample_us);
        }
        return;
    }
    ++statistics_.unmatched_report_pairs;
}

void LeadController::OnGpuRenderEnd(uint32_t producer_epoch, uint64_t frame_id, int64_t gpu_start_qpc,
    int64_t gpu_end_qpc, double present_to_gpu_end_us) noexcept
{
    if (producer_epoch == 0 || frame_id == 0 || gpu_end_qpc <= 0 || qpc_frequency_ <= 0) return;
    if (producer_epoch != gpu_end_epoch_) {
        gpu_end_epoch_ = producer_epoch;
        gpu_end_frame_ = 0;
        gpu_end_qpc_ = 0;
        gpu_period_window_.Reset();
        gpu_after_present_window_.Reset();
    }
    if (frame_id <= gpu_end_frame_) return;
    // The GPU's own period: consecutive frames it did not wait for.
    if (gpu_end_frame_ != 0 && frame_id == gpu_end_frame_ + 1 && gpu_end_qpc > gpu_end_qpc_ &&
        gpu_start_qpc > 0 && MicrosecondsBetween(gpu_end_qpc_, gpu_start_qpc) <= LEAD_HANDOFF_GPU_IDLE_US) {
        gpu_period_window_.Add(MicrosecondsBetween(gpu_end_qpc_, gpu_end_qpc));
    }
    if (IsPositiveFinite(present_to_gpu_end_us)) gpu_after_present_window_.Add(present_to_gpu_end_us);
    gpu_end_frame_ = frame_id;
    gpu_end_qpc_ = gpu_end_qpc;
}

// σ: release → RENDERSUBMIT_START of a frame not held, once both are known.
void LeadController::RecordReleaseToRender(PendingFrame* frame) noexcept
{
    if (frame == nullptr || !frame->started || frame->release_to_render_recorded ||
        frame->rung != PacingRung::SourceOnly || !HasField(*frame, FieldSource) ||
        !HasField(*frame, FieldRenderSubmit)) {
        return;
    }
    frame->release_to_render_recorded = true;
    const double release_to_render_us =
        MicrosecondsBetween(frame->qpc[FieldSource], frame->qpc[FieldRenderSubmit]);
    if (release_to_render_us >= 0.0 && release_to_render_us <= LEAD_LONG_SAMPLE_INTERVALS * frame->interval_us) {
        // Without the frame's own start hold, which comes after the release.
        release_to_render_window_.Add(std::max(0.0, release_to_render_us - RenderStartHoldUs(*frame)));
    }
}

double LeadController::RenderStartHoldUs(const PendingFrame& frame) const noexcept
{
    if (frame.render_start_hold_ticks <= 0 || qpc_frequency_ <= 0) return 0.0;
    return MicrosecondsBetween(0, frame.render_start_hold_ticks);
}

double LeadController::DriverHoldUs(const PendingFrame& frame) const noexcept
{
    if (frame.driver_hold_ticks <= 0 || qpc_frequency_ <= 0) return 0.0;
    return MicrosecondsBetween(0, frame.driver_hold_ticks);
}

// ── Buckets ──

bool LeadController::FindBucketIndex(PacingRung rung, double fg_divisor, size_t* index) noexcept
{
    const size_t rung_index = static_cast<size_t>(rung);
    if (rung_index >= kRungCount) return false;
    size_t divisor_index = 0;
    if (std::isfinite(fg_divisor)) {
        const double clamped = std::clamp(fg_divisor, 1.0, static_cast<double>(LEAD_DIVISOR_BUCKETS));
        divisor_index = static_cast<size_t>(std::floor(clamped + 0.5)) - 1;
    }
    *index = rung_index * LEAD_DIVISOR_BUCKETS + divisor_index;
    return true;
}

void LeadController::ResetBucket(Bucket* bucket) noexcept
{
    bucket->window.Reset();
    bucket->excluded_long = 0;
    bucket->quantile_us = 0.0;
    bucket->target_us = 0.0;
    bucket->lead_us = 0.0;
    bucket->budget_us = 0.0;
    bucket->warm_applied = false;
    bucket->budget_limited = false;
    bucket->slack = SlackWindow{};
    bucket->reseed_x_us = 0.0;
}

void LeadController::AddSlack(SlackWindow* window, double slack_us) noexcept
{
    const uint8_t negative = slack_us < 0.0 ? 1 : 0;
    if (window->count == LEAD_SLACK_CAPACITY) {
        window->negative_count -= window->negative[window->head];
    } else {
        ++window->count;
    }
    window->negative[window->head] = negative;
    window->negative_count += negative;
    window->head = (window->head + 1) % static_cast<uint32_t>(LEAD_SLACK_CAPACITY);
}

// ── Pending table ──

uint16_t LeadController::FieldBit(Field field) noexcept
{
    return static_cast<uint16_t>(1u << field);
}

bool LeadController::HasField(const PendingFrame& frame, Field field) noexcept
{
    return (frame.field_mask & FieldBit(field)) != 0;
}

void LeadController::SetField(PendingFrame* frame, Field field, int64_t qpc) noexcept
{
    frame->qpc[field] = qpc;
    frame->field_mask = static_cast<uint16_t>(frame->field_mask | FieldBit(field));
}

LeadController::PendingFrame* LeadController::Find(uint64_t grid_index) noexcept
{
    PendingFrame& slot = pending_[static_cast<size_t>(grid_index % LEAD_PENDING_CAPACITY)];
    return slot.occupied && slot.grid_index == grid_index ? &slot : nullptr;
}

const LeadController::PendingFrame* LeadController::Previous(const PendingFrame& frame) const noexcept
{
    if (frame.grid_index == 0) return nullptr;
    const uint64_t index = frame.grid_index - 1;
    const PendingFrame& slot = pending_[static_cast<size_t>(index % LEAD_PENDING_CAPACITY)];
    if (!slot.occupied || slot.grid_index != index || !SameLifecycle(slot.key, frame.key)) return nullptr;
    return &slot;
}

LeadController::Field LeadController::PresentSideField(PacingRung rung) noexcept
{
    switch (rung) {
    case PacingRung::SourceOnly:
    case PacingRung::MarkerHold:
        return FieldMarkerArrival;
    case PacingRung::NativeHold:
        return FieldNativeArrival;
    default:
        return FieldPresentCall;
    }
}

// Lockstep engines (lead_controller.h): the waits of later frames released
// no later than this frame's Present-side time, inside [s, that time]. Later
// frames are released in order, so the first one released after that time
// ends the scan.
int64_t LeadController::GatedHoldTicks(const PendingFrame& frame) const noexcept
{
    const Field present_field = PresentSideField(frame.rung);
    if (!frame.started || !HasField(frame, FieldSource) || !HasField(frame, present_field)) return 0;
    const int64_t source_qpc = frame.qpc[FieldSource];
    const int64_t present_qpc = frame.qpc[present_field];
    if (present_qpc <= source_qpc) return 0;
    const auto overlap = [&](int64_t begin_qpc, int64_t end_qpc) noexcept {
        const int64_t ticks = std::min(present_qpc, end_qpc) - std::max(source_qpc, begin_qpc);
        return ticks > 0 ? ticks : int64_t{0};
    };
    int64_t gated_ticks = 0;
    for (uint64_t ahead = 1; ahead <= LEAD_PENDING_AHEAD_FRAMES; ++ahead) {
        const uint64_t index = frame.grid_index + ahead;
        const PendingFrame& later = pending_[static_cast<size_t>(index % LEAD_PENDING_CAPACITY)];
        if (!later.occupied || later.grid_index != index || !SameLifecycle(later.key, frame.key) ||
            !later.started) {
            break;
        }
        if (later.qpc[FieldSource] > present_qpc) break;
        if (later.wait_start_qpc > 0) gated_ticks += overlap(later.wait_start_qpc, later.wait_end_qpc);
        if (later.pre_wait_start_qpc > 0) gated_ticks += overlap(later.pre_wait_start_qpc, later.pre_wait_end_qpc);
    }
    return gated_ticks;
}

int64_t LeadController::GatedHoldTicks(const PacingPlanKey& key, uint64_t grid_index) const noexcept
{
    const PendingFrame& slot = pending_[static_cast<size_t>(grid_index % LEAD_PENDING_CAPACITY)];
    if (!slot.occupied || slot.grid_index != grid_index || slot.key != key) return 0;
    return GatedHoldTicks(slot);
}

void LeadController::Discard(PendingFrame* frame) noexcept
{
    if (frame->occupied && !frame->closed) {
        if (!frame->started) statistics_.unmatched_events += frame->early_events;
        else if (!frame->finalized) ++statistics_.incomplete_dropped;
    }
    *frame = PendingFrame{};
}

void LeadController::StartLifecycle(const PacingPlanKey& key, uint64_t grid_index, bool discard_all) noexcept
{
    for (PendingFrame& frame : pending_) {
        if (frame.occupied && (discard_all || !SameLifecycle(frame.key, key))) Discard(&frame);
    }
    lifecycle_known_ = true;
    lifecycle_ = key;
    newest_index_ = grid_index;
    ExpireBehindNewest();
}

// Frames LEAD_PENDING_EXPIRY_FRAMES behind the newest release close: a
// complete frame is sampled with fallbacks, an incomplete one is counted.
// Closing may resolve the frame after it, so pending frames are retried.
void LeadController::ExpireBehindNewest() noexcept
{
    for (PendingFrame& frame : pending_) {
        if (!frame.occupied || frame.closed ||
            frame.grid_index + LEAD_PENDING_EXPIRY_FRAMES > newest_index_) {
            continue;
        }
        if (!frame.started) {
            Discard(&frame);
            continue;
        }
        if (!frame.finalized) ForceFinalize(&frame);
        // Its report pair did not come in time: the sample counts as measured.
        if (frame.lead_sample_waiting) {
            frame.lead_sample_waiting = false;
            ++statistics_.unpaired_lead_samples;
            CommitLeadSample(&frame, frame.waiting_lead_sample_us);
        }
        frame.closed = true;
    }
    for (PendingFrame& frame : pending_) TryFinalize(&frame);
}

void LeadController::RecordResumeDelay(PendingFrame* frame) noexcept
{
    if (frame->resume_delay_recorded || !HasField(*frame, FieldGpuResume) ||
        !HasField(*frame, FieldOutputRelease) || !IsHeldStatus(frame->output_status)) {
        return;
    }
    frame->resume_delay_recorded = true;
    // u − o is the Signal→resume latency only when the GPU was already
    // waiting at the Signal. A GPU that reached the Wait later resumed on its
    // own backlog, which would bias the decontamination median and the GPU
    // wait cap's late-resume evidence.
    if (frame->gpu_wait_qpc <= 0 || frame->gpu_wait_qpc > frame->qpc[FieldOutputRelease]) {
        ++statistics_.resume_not_waited;
        return;
    }
    const double delay_us = MicrosecondsBetween(frame->qpc[FieldOutputRelease], frame->qpc[FieldGpuResume]);
    resume_delays_us_[resume_delay_head_] = delay_us;
    resume_delay_head_ = (resume_delay_head_ + 1) % LEAD_RESUME_DELAY_CAPACITY;
    if (resume_delay_count_ < LEAD_RESUME_DELAY_CAPACITY) ++resume_delay_count_;
    ++statistics_.resume_waited_samples;
    frame->resume_waited = true;
    frame->resume_late = delay_us > LEAD_RESUME_LATE_US;
    if (frame->resume_late) ++statistics_.resume_late_samples;
}

// C = p − s of a sampled GPU queue wait frame, at most 3 I.
void LeadController::RecordCallTime(PendingFrame* frame) noexcept
{
    if (frame->call_recorded || !frame->started || !frame->sampled ||
        frame->rung != PacingRung::GpuQueueWait || !HasField(*frame, FieldPresentCall)) {
        return;
    }
    frame->call_recorded = true;
    const double call_us = MicrosecondsBetween(frame->qpc[FieldSource], frame->qpc[FieldPresentCall]);
    if (!std::isfinite(call_us) || call_us < 0.0 || call_us > LEAD_LONG_SAMPLE_INTERVALS * frame->interval_us) {
        return;
    }
    call_windows_[frame->bucket % LEAD_DIVISOR_BUCKETS].Add(call_us);
}

// A waited GPU queue wait resume of frame n and frame n + 1's Present call:
// one release margin observation. Frame n + 1 must be a paced GPU queue
// wait frame too: a pair across a rung or mode change (focus loss, idle cap,
// overload) is no evidence about the release timing.
void LeadController::RecordReleaseMargin(PendingFrame* frame) noexcept
{
    if (frame == nullptr || frame->margin_recorded || !frame->resume_waited ||
        frame->rung != PacingRung::GpuQueueWait) {
        return;
    }
    const PendingFrame* next = Find(frame->grid_index + 1);
    if (next == nullptr || !SameLifecycle(next->key, frame->key) || !next->started ||
        !HasField(*next, FieldPresentCall)) {
        return;
    }
    frame->margin_recorded = true;
    if (next->rung != PacingRung::GpuQueueWait || !next->sampled) return;
    const double margin_us = MicrosecondsBetween(frame->qpc[FieldOutputRelease], next->qpc[FieldPresentCall]);
    ++statistics_.margin_samples;
    if (frame->resume_late) ++statistics_.margin_late_samples;
    const int step = margin_.Add(margin_us, frame->resume_late);
    if (step > 0) ++statistics_.margin_widened;
    else if (step < 0) ++statistics_.margin_narrowed;
}

// D = r − A − s of a sampled GPU queue wait frame with its readiness and a
// Reflex GPU active time: the source release → heavy GPU work time, at most
// 3 I. A frame whose GPU work outlasted its readiness span counts as 0.
void LeadController::RecordWorkStart(PendingFrame* frame) noexcept
{
    if (frame->work_recorded || !frame->started || !frame->sampled ||
        frame->rung != PacingRung::GpuQueueWait || !(frame->gpu_active_us > 0.0) ||
        !HasField(*frame, FieldGpuReady)) {
        return;
    }
    frame->work_recorded = true;
    const double work_us = MicrosecondsBetween(frame->qpc[FieldSource], frame->qpc[FieldGpuReady]) -
        frame->gpu_active_us;
    if (!std::isfinite(work_us) || work_us > LEAD_LONG_SAMPLE_INTERVALS * frame->interval_us) return;
    const size_t divisor = frame->bucket % LEAD_DIVISOR_BUCKETS;
    work_windows_[divisor].Add(std::max(0.0, work_us));
    work_age_frames_[divisor] = 0;
    ++statistics_.work_samples;
}

// A waited GPU queue wait resume of frame n and the start of frame n + 1's
// heavy GPU work, r_{n+1} − A_{n+1}: one release headroom observation. As
// for the margin, frame n + 1 must be a paced GPU queue wait frame; one
// without a Reflex GPU active time yet is waited for.
void LeadController::RecordReleaseHeadroom(PendingFrame* frame) noexcept
{
    if (frame == nullptr || frame->headroom_recorded || !frame->resume_waited ||
        frame->rung != PacingRung::GpuQueueWait) {
        return;
    }
    const PendingFrame* next = Find(frame->grid_index + 1);
    if (next == nullptr || !SameLifecycle(next->key, frame->key) || !next->started) return;
    if (next->rung != PacingRung::GpuQueueWait || !next->sampled) {
        frame->headroom_recorded = true;
        return;
    }
    if (!(next->gpu_active_us > 0.0) || !HasField(*next, FieldGpuReady)) return;
    frame->headroom_recorded = true;
    const double headroom_us = MicrosecondsBetween(frame->qpc[FieldOutputRelease], next->qpc[FieldGpuReady]) -
        next->gpu_active_us;
    if (!std::isfinite(headroom_us)) return;
    ++statistics_.headroom_samples;
    if (frame->resume_late) ++statistics_.headroom_late_samples;
    const int step = headroom_.Add(headroom_us, frame->resume_late);
    if (step > 0) ++statistics_.headroom_widened;
    else if (step < 0) ++statistics_.headroom_narrowed;
}

// A waited GPU queue wait release o_n and frame n + 1's GPU start g_{n+1}:
// one queue-order pair (lead_controller.h) when frame n + 1 had begun
// submitting by o_n. As for the headroom, frame n + 1 must be a paced GPU
// queue wait frame; one without its report yet is waited for.
void LeadController::RecordConcurrency(PendingFrame* frame) noexcept
{
    if (frame == nullptr || frame->concurrency_recorded || !frame->resume_waited ||
        frame->rung != PacingRung::GpuQueueWait) {
        return;
    }
    const PendingFrame* next = Find(frame->grid_index + 1);
    if (next == nullptr || !SameLifecycle(next->key, frame->key) || !next->started) return;
    if (next->rung != PacingRung::GpuQueueWait || !next->sampled) {
        frame->concurrency_recorded = true;
        return;
    }
    if (!(next->gpu_active_us > 0.0)) return;
    frame->concurrency_recorded = true;
    const int64_t release_qpc = frame->qpc[FieldOutputRelease];
    // No GPU start (clock map not stable), or nothing submitted before the
    // release that could have run beside the wait: no pair.
    if (next->gpu_start_qpc <= 0 || !HasField(*next, FieldRenderSubmit) ||
        next->qpc[FieldRenderSubmit] > release_qpc) {
        return;
    }
    const double start_us = MicrosecondsBetween(release_qpc, next->gpu_start_qpc);
    if (!std::isfinite(start_us)) return;
    const uint8_t concurrent = start_us < -LEAD_CONCURRENCY_TOLERANCE_US ? 1 : 0;
    uint8_t& slot = concurrency_[concurrency_head_];
    if (concurrency_count_ == LEAD_CONCURRENCY_SAMPLES) concurrent_in_window_ -= slot;
    else ++concurrency_count_;
    slot = concurrent;
    concurrent_in_window_ += concurrent;
    concurrency_head_ = (concurrency_head_ + 1) % LEAD_CONCURRENCY_SAMPLES;
    ++statistics_.concurrency_samples;
    statistics_.concurrent_samples += concurrent;
    statistics_.last_concurrency_us = start_us;
}

bool LeadController::NextGpuWorkBehindWait() const noexcept
{
    return concurrency_count_ >= LEAD_WARM_SAMPLES && concurrent_in_window_ == 0;
}

void LeadController::NewestResumeDelays(size_t count, double* delays_us) const noexcept
{
    for (size_t offset = 0; offset < count; ++offset) {
        const size_t position = (resume_delay_head_ + LEAD_RESUME_DELAY_CAPACITY - 1 - offset) %
            LEAD_RESUME_DELAY_CAPACITY;
        delays_us[offset] = resume_delays_us_[position];
    }
}

// Lower-index median of the newest 32 u − o observations; 0 before any.
double LeadController::MedianResumeDelayUs() const noexcept
{
    const size_t count = std::min(resume_delay_count_, LEAD_RESUME_MEDIAN_SAMPLES);
    if (count == 0) return 0.0;
    std::array<double, LEAD_RESUME_MEDIAN_SAMPLES> newest{};
    NewestResumeDelays(count, newest.data());
    const auto middle = newest.begin() + static_cast<std::ptrdiff_t>((count - 1) / 2);
    std::nth_element(newest.begin(), middle, newest.begin() + static_cast<std::ptrdiff_t>(count));
    return *middle;
}

// ── Sample assembly ──

// Stage times a frame of the rung needs for its own sample.
uint16_t LeadController::OwnFieldMask(PacingRung rung) noexcept
{
    switch (rung) {
    case PacingRung::SourceOnly:
    case PacingRung::MarkerHold:
        return static_cast<uint16_t>(FieldBit(FieldSource) | FieldBit(FieldMarkerArrival));
    case PacingRung::NativeHold:
        return static_cast<uint16_t>(FieldBit(FieldSource) | FieldBit(FieldNativeArrival));
    case PacingRung::GpuQueueWait:
    case PacingRung::FgInputWait:
        return static_cast<uint16_t>(FieldBit(FieldSource) | FieldBit(FieldPresentCall) |
            FieldBit(FieldGpuReady));
    case PacingRung::Count:
        break;
    }
    return 0xFFFF; // unreachable: releases with an invalid rung are rejected
}

// A CPU hold starts at the marker arrival (H1) or the native arrival (H2);
// other rungs hold no CPU thread.
bool LeadController::CpuHoldStartField(PacingRung rung, Field* field) noexcept
{
    if (rung == PacingRung::MarkerHold) *field = FieldMarkerArrival;
    else if (rung == PacingRung::NativeHold) *field = FieldNativeArrival;
    else return false;
    return true;
}

// The previous frame's stage times this frame needs are known, or will never
// arrive: no previous frame in the lifecycle, the previous frame closed, it
// did not hold, or it holds nothing this frame's formula removes.
bool LeadController::PreviousResolved(const PendingFrame& frame) const noexcept
{
    if (frame.rung == PacingRung::SourceOnly) return true;
    const PendingFrame* previous = Previous(frame);
    if (previous == nullptr || previous->closed) return true;
    if (!previous->started) return false; // its release may still arrive

    if (IsGpuRung(frame.rung)) {
        if (!IsGpuRung(previous->rung) || HasField(*previous, FieldGpuResume)) return true;
        if (!HasField(*previous, FieldOutputRelease)) return false;
        return !IsHeldStatus(previous->output_status) || !previous->gpu_resume_probe;
    }
    Field hold_start = FieldCount;
    if (!CpuHoldStartField(previous->rung, &hold_start)) return true;
    if (!HasField(*previous, FieldOutputRelease)) return false;
    return !IsHeldStatus(previous->output_status) || HasField(*previous, hold_start);
}

void LeadController::TryFinalize(PendingFrame* frame) noexcept
{
    if (frame == nullptr || !frame->occupied || !frame->started || frame->finalized) return;
    const uint16_t own = OwnFieldMask(frame->rung);
    if ((frame->field_mask & own) != own || !PreviousResolved(*frame)) return;
    EmitSample(frame);
}

void LeadController::ForceFinalize(PendingFrame* frame) noexcept
{
    const uint16_t own = OwnFieldMask(frame->rung);
    if ((frame->field_mask & own) == own) {
        EmitSample(frame);
        return;
    }
    frame->finalized = true;
    const bool presented = HasField(*frame, FieldPresentCall);
    if (IsGpuRung(frame->rung) && presented) ++statistics_.missing_ready;
    else ++statistics_.incomplete_dropped;
}

void LeadController::EmitSample(PendingFrame* frame) noexcept
{
    frame->finalized = true;
    RecordHandoff(*frame, Previous(*frame));
    double carried_us = 0.0;
    const double sample_us = SampleUs(*frame, Previous(*frame), &frame->own_ready_qpc, &carried_us);
    if (!std::isfinite(sample_us)) {
        ++statistics_.invalid_inputs;
        return;
    }
    if (carried_us > 0.0) {
        ++statistics_.carried_hold_samples;
        statistics_.last_carried_hold_us = carried_us;
    }
    const double render_start_hold_us = RenderStartHoldUs(*frame);
    if (render_start_hold_us > 0.0) {
        ++statistics_.render_start_hold_samples;
        statistics_.last_render_start_hold_us = render_start_hold_us;
    }
    Bucket& bucket = buckets_[frame->bucket];
    // Lockstep engines (lead_controller.h): ReLimiter's waits for later frames
    // that this frame's Present followed are not the game's lateness.
    const int64_t gated_ticks = GatedHoldTicks(*frame);
    const double gated_us = gated_ticks > 0 ? MicrosecondsBetween(0, gated_ticks) : 0.0;
    if (gated_us > 0.0) {
        ++statistics_.gated_hold_samples;
        statistics_.last_gated_hold_us = gated_us;
    }
    // A long sample is excluded from the lead but is a missed target.
    // A frame the collision guard made late on purpose is judged against its
    // unguarded lead, and every frame without its gated hold.
    const double slack_us = frame->lead_us + frame->guard_reduction_us - std::max(0.0, sample_us - gated_us);
    AddSlack(&bucket.slack, slack_us);
    statistics_.last_slack_us = slack_us;
    ++statistics_.slack_samples;
    if (slack_us < 0.0) ++statistics_.negative_slack_samples;
    // Hold room (lead_controller.h): a frame late even against its unguarded
    // lead is the overload's, not the hold's; a gated frame's X contains
    // ReLimiter's own later waits, which would make R grow with the interval.
    if (frame->guard_judged && gated_ticks == 0 && slack_us >= 0.0) {
        room_windows_[frame->bucket % LEAD_DIVISOR_BUCKETS].Add(sample_us - frame->guard_offset_us);
        ++statistics_.room_samples;
    }
    // FG input wait, and the GPU queue wait under the A/B key
    // (lead_controller.h): the lead learns X without the time the finished
    // frame waited for the render thread to leave the previous Present; the
    // slack above keeps it.
    const double queue_us = QueueUs(*frame, Previous(*frame));
    frame->queue_removed_us = queue_us > 0.0 ? queue_us : 0.0;
    // The driver's Present hold under the previous NvAPI frame ID delays this
    // frame's first submission (lead_controller.h); CommitLeadSample takes
    // what q and W did not already remove.
    frame->previous_driver_hold_us = 0.0;
    if (frame->driver_hold_exclusion && IsGpuRung(frame->rung)) {
        const PendingFrame* previous = Previous(*frame);
        if (previous != nullptr && previous->key.identity_domain == PacingIdentityDomain::NvApiFrame &&
            frame->key.identity_domain == PacingIdentityDomain::NvApiFrame &&
            previous->key.producer_epoch == frame->key.producer_epoch &&
            previous->key.frame_id + 1 == frame->key.frame_id) {
            frame->previous_driver_hold_us = DriverHoldUs(*previous);
        }
    }
    if (queue_us > 0.0) {
        ++statistics_.queued_samples;
        statistics_.last_queue_us = queue_us;
    }
    // A frame on a GPU wait also leaves out the time the GPU held its render
    // thread back, which only its Reflex report pair shows (lead_controller.h).
    frame->carried_hold_us = carried_us;
    if (!frame->report_paired && AwaitsReportPair(*frame)) {
        frame->lead_sample_waiting = true;
        frame->waiting_lead_sample_us = sample_us - queue_us;
        return;
    }
    CommitLeadSample(frame, sample_us - queue_us);
}

bool LeadController::AwaitsReportPair(const PendingFrame& frame) const noexcept
{
    return IsGpuRung(frame.rung) && frame.key.identity_domain == PacingIdentityDomain::NvApiFrame &&
        frame.key.producer_epoch == report_pair_epoch_ && report_pair_frame_ != 0 &&
        frame.key.frame_id <= report_pair_frame_ + LEAD_REPORT_PAIR_HORIZON_FRAMES;
}

void LeadController::CommitLeadSample(PendingFrame* frame, double lead_sample_us) noexcept
{
    Bucket& bucket = buckets_[frame->bucket];
    const double held_back_us = std::max(0.0, frame->held_back_wait_us - frame->carried_hold_us);
    if (held_back_us > 0.0) {
        lead_sample_us = std::max(0.0, lead_sample_us - held_back_us);
        ++statistics_.held_back_corrected_samples;
        statistics_.last_held_back_wait_us = held_back_us;
    }
    // The driver's Present hold of the previous frame adds only what q and W
    // did not already remove: max(q + W, H) in all.
    const double driver_extra_us =
        std::max(0.0, frame->previous_driver_hold_us - (frame->queue_removed_us + held_back_us));
    if (driver_extra_us > 0.0) {
        lead_sample_us = std::max(0.0, lead_sample_us - driver_extra_us);
        ++statistics_.driver_hold_corrected_samples;
        statistics_.last_driver_hold_us = frame->previous_driver_hold_us;
    }
    if (lead_sample_us > LEAD_LONG_SAMPLE_INTERVALS * frame->interval_us) {
        ++bucket.excluded_long;
        ++statistics_.excluded_long;
        return;
    }
    bucket.window.Add(lead_sample_us);
    ++statistics_.samples;
    statistics_.last_sample_us = lead_sample_us;
}

// X relative to the frame's source release s:
//   H0: a − s.   H1/H2: (a − s) − overlap([s, a], previous CPU hold), with the
//   native arrival for H2.
//   H3 and FG input wait: max(p, r − c) − s. The frame's GPU work can
//   start at w but not before the GPU is free of frame n − 1: at f =
//   max(r_{n−1}, u_{n−1}) as run, at f' = r'_{n−1} (that frame's own
//   readiness, below) without ReLimiter's holds. c = max(0, max(w, f) −
//   max(w, f')) is the delay the holds carried into this frame through a busy
//   GPU. Without it a GPU near the interval chains every frame to the
//   previous release, X tracks L and the lead climbs to the budget (Gears of
//   War: E-Day with FG, 30 ms leads where 24 ms sufficed). w is
//   RENDERSUBMIT_START (clamped to [s, p]) on the FG input wait and p on the
//   GPU queue wait: Rogue Core's X does not depend on the previous frame's
//   hold (slope about 0 in the archived light captures), and with an early w
//   the chained f' turned the GPU's ordinary overlap of consecutive frames
//   into a fictitious carried delay that kept the lead just below X, so most
//   frames were late.
//   Only a previous frame whose GPU actually waited at its gate
//   (PreviousGpuHeld) can carry a delay; an unheld frame ends the chain, so an
//   underestimated f' cannot sustain itself. Unknown r_{n−1} leaves f' at the
//   start of the frame, which reduces c to max(0, u_{n−1} − w): the earlier
//   formula (p − s) + max(0, r − max(p, u_{n−1})) when w = p. The floor holds
//   because a frame is never ready before its present call.
double LeadController::SampleUs(const PendingFrame& frame, const PendingFrame* previous,
    int64_t* own_ready_qpc, double* carried_us) const noexcept
{
    if (own_ready_qpc != nullptr) *own_ready_qpc = 0;
    if (carried_us != nullptr) *carried_us = 0.0;
    const int64_t source = frame.qpc[FieldSource];
    // The frame's own render-thread start hold (lead_controller.h) is
    // ReLimiter's wait between the release and the arrival, ready and Present
    // times below, so X leaves it out on every rung; no sample goes below 0.
    const double hold_us = RenderStartHoldUs(frame);
    const auto without_hold = [hold_us](double x_us) noexcept {
        return hold_us > 0.0 && std::isfinite(x_us) ? std::max(0.0, x_us - hold_us) : x_us;
    };
    switch (frame.rung) {
    case PacingRung::SourceOnly:
        return without_hold(MicrosecondsBetween(source, frame.qpc[FieldMarkerArrival]));
    case PacingRung::MarkerHold:
    case PacingRung::NativeHold: {
        const Field arrival = frame.rung == PacingRung::MarkerHold ? FieldMarkerArrival : FieldNativeArrival;
        const double arrival_us = MicrosecondsBetween(source, frame.qpc[arrival]);
        double hold_begin_us = 0.0;
        double hold_end_us = 0.0;
        if (!PreviousCpuHold(previous, source, &hold_begin_us, &hold_end_us)) return without_hold(arrival_us);
        return without_hold(arrival_us - Overlap(0.0, arrival_us, hold_begin_us, hold_end_us));
    }
    case PacingRung::GpuQueueWait:
    case PacingRung::FgInputWait: {
        const double present_us = MicrosecondsBetween(source, frame.qpc[FieldPresentCall]);
        const double ready_us = MicrosecondsBetween(source, frame.qpc[FieldGpuReady]);
        double submit_us = present_us;
        if (frame.rung == PacingRung::FgInputWait && HasField(frame, FieldRenderSubmit)) {
            submit_us = std::clamp(MicrosecondsBetween(source, frame.qpc[FieldRenderSubmit]),
                0.0, std::max(present_us, 0.0));
        }
        double carried = 0.0;
        if (previous != nullptr && previous->started && IsGpuRung(previous->rung) &&
            PreviousGpuHeld(*previous)) {
            bool free_known = false;
            double free_us = 0.0;
            double resume_us = 0.0;
            if (PreviousGpuResume(previous, source, &resume_us)) {
                free_us = resume_us;
                free_known = true;
            }
            double own_free_us = -std::numeric_limits<double>::infinity();
            if (HasField(*previous, FieldGpuReady)) {
                const double previous_ready_us = MicrosecondsBetween(source, previous->qpc[FieldGpuReady]);
                free_us = free_known ? std::max(free_us, previous_ready_us) : previous_ready_us;
                free_known = true;
                own_free_us = previous->own_ready_qpc != 0
                    ? std::min(MicrosecondsBetween(source, previous->own_ready_qpc), previous_ready_us)
                    : previous_ready_us;
            }
            if (free_known) {
                carried = std::max(submit_us, free_us) - std::max(submit_us, own_free_us);
                if (!(carried > 0.0)) carried = 0.0;
            }
        }
        // c above is the delay of earlier holds; the frame's own start hold
        // comes off the readiness it is stored with, so the next frame's c
        // takes the delay this hold carried through a busy GPU.
        const double own_ready_us = std::max(present_us, ready_us - carried);
        if (carried_us != nullptr) *carried_us = (ready_us - own_ready_us) > 0.0 ? ready_us - own_ready_us : 0.0;
        const double x_us = without_hold(own_ready_us);
        if (own_ready_qpc != nullptr) {
            *own_ready_qpc = source + static_cast<int64_t>(std::llround(
                x_us * static_cast<double>(qpc_frequency_) / kMicrosecondsPerSecond));
        }
        return x_us;
    }
    case PacingRung::Count:
        break;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// The game's simulation → render queue (lead_controller.h):
// q = max(0, min(w, b_{n−1}) − max(s, e)), with w clamped to [s, p] as in
// SampleUs. A render thread already free before the frame finished
// (b_{n−1} <= e) or one that took it at once (w = e) gives 0. Always on the FG
// input wait; on the GPU queue wait only under the developer A/B setting
// (Remove, or RemoveHeld after a frame whose GPU waited at its gate); never
// on the CPU rungs.
double LeadController::QueueUs(const PendingFrame& frame, const PendingFrame* previous) const noexcept
{
    if (previous == nullptr) return 0.0;
    bool applies = frame.rung == PacingRung::FgInputWait;
    if (frame.rung == PacingRung::GpuQueueWait) {
        applies = frame.render_queue == PacingRenderQueue::Remove ||
            (frame.render_queue == PacingRenderQueue::RemoveHeld && previous->started &&
                IsGpuRung(previous->rung) && PreviousGpuHeld(*previous));
    }
    if (!applies || !HasField(frame, FieldSimulationEnd) || !HasField(frame, FieldRenderSubmit) ||
        !HasField(frame, FieldPresentCall) || !HasField(*previous, FieldPresentEnd)) {
        return 0.0;
    }
    const int64_t source = frame.qpc[FieldSource];
    const double present_us = MicrosecondsBetween(source, frame.qpc[FieldPresentCall]);
    const double submit_us = std::clamp(MicrosecondsBetween(source, frame.qpc[FieldRenderSubmit]),
        0.0, std::max(present_us, 0.0));
    const double end_us = std::max(0.0, MicrosecondsBetween(source, frame.qpc[FieldSimulationEnd]));
    const double busy_until_us = std::min(submit_us,
        MicrosecondsBetween(source, previous->qpc[FieldPresentEnd]));
    const double queue_us = busy_until_us - end_us;
    return queue_us > 0.0 ? queue_us : 0.0;
}

// Render handoff (lead_controller.h): h = (b_{n−1} − w_{n−1}) − (e_n − s_n)
// for a frame released at or after w_{n−1}. Bounded by 3 I like the lead
// samples; negative values count in the window's first bin.
void LeadController::RecordHandoff(const PendingFrame& frame, const PendingFrame* previous) noexcept
{
    if (previous == nullptr || !previous->started || !SameLifecycle(previous->key, frame.key) ||
        !HasField(frame, FieldSimulationEnd) || !HasField(*previous, FieldRenderSubmit) ||
        !HasField(*previous, FieldPresentEnd)) {
        return;
    }
    const int64_t source = frame.qpc[FieldSource];
    const int64_t handoff = previous->qpc[FieldRenderSubmit];
    if (source < handoff) return;
    const double render_us = MicrosecondsBetween(handoff, previous->qpc[FieldPresentEnd]);
    const double simulation_us = MicrosecondsBetween(source, frame.qpc[FieldSimulationEnd]);
    const double sample_us = render_us - simulation_us;
    const double bound_us = LEAD_LONG_SAMPLE_INTERVALS * frame.interval_us;
    if (!std::isfinite(sample_us) || render_us < 0.0 || simulation_us < 0.0 ||
        std::fabs(sample_us) > bound_us) {
        return;
    }
    handoff_window_.Add(sample_us);
    ++statistics_.handoff_samples;
    statistics_.last_handoff_us = sample_us;
}

// The previous GPU-rung frame's GPU waited at its gate: a probe saw it reach
// the Wait no later than the Signal (w <= o), or, without a probe, the frame
// was held and ready before the Signal. Only such a frame's hold can delay
// the next frame.
bool LeadController::PreviousGpuHeld(const PendingFrame& previous) noexcept
{
    if (!HasField(previous, FieldOutputRelease) || !IsHeldStatus(previous.output_status)) return false;
    const int64_t signal_qpc = previous.qpc[FieldOutputRelease];
    if (HasField(previous, FieldGpuResume) && previous.gpu_wait_qpc != 0) {
        return previous.gpu_wait_qpc <= signal_qpc;
    }
    return HasField(previous, FieldGpuReady) && previous.qpc[FieldGpuReady] < signal_qpc;
}

bool LeadController::PreviousCpuHold(const PendingFrame* previous, int64_t origin_qpc,
    double* begin_us, double* end_us) const noexcept
{
    if (previous == nullptr || !previous->started) return false;
    Field hold_start = FieldCount;
    if (!CpuHoldStartField(previous->rung, &hold_start) || !HasField(*previous, hold_start) ||
        !HasField(*previous, FieldOutputRelease) || !IsHeldStatus(previous->output_status)) {
        return false;
    }
    *begin_us = MicrosecondsBetween(origin_qpc, previous->qpc[hold_start]);
    *end_us = MicrosecondsBetween(origin_qpc, previous->qpc[FieldOutputRelease]);
    return true;
}

// u_{n−1}: the probe, else o_{n−1} plus the median u − o, else unknown.
bool LeadController::PreviousGpuResume(const PendingFrame* previous, int64_t origin_qpc,
    double* resume_us) const noexcept
{
    if (previous == nullptr || !previous->started || !IsGpuRung(previous->rung)) return false;
    if (HasField(*previous, FieldGpuResume)) {
        *resume_us = MicrosecondsBetween(origin_qpc, previous->qpc[FieldGpuResume]);
        return true;
    }
    if (!HasField(*previous, FieldOutputRelease) || !IsHeldStatus(previous->output_status)) return false;
    *resume_us = MicrosecondsBetween(origin_qpc, previous->qpc[FieldOutputRelease]) + MedianResumeDelayUs();
    return true;
}

double LeadController::MicrosecondsBetween(int64_t from_qpc, int64_t to_qpc) const noexcept
{
    return static_cast<double>(to_qpc - from_qpc) * kMicrosecondsPerSecond /
        static_cast<double>(qpc_frequency_);
}
