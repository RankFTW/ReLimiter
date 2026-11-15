#include "lead_controller.h"

#include <algorithm>
#include <cmath>
#include <functional>
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

// ── Lifecycle ──

void LeadController::Reset(int64_t qpc_frequency) noexcept
{
    for (Bucket& bucket : buckets_) ResetBucket(&bucket);
    for (PendingFrame& frame : pending_) frame = PendingFrame{};
    resume_delays_us_.fill(0.0f);
    resume_delay_head_ = 0;
    resume_delay_count_ = 0;
    for (LeadSampleWindow& calls : call_windows_) calls.Reset();
    for (LeadSampleWindow& work : work_windows_) work.Reset();
    for (LeadSampleWindow& work : idle_work_windows_) work.Reset();
    idle_evidence_ = false;
    work_age_frames_.fill(0);
    for (LateWindow& late : late_arrivals_) late.Reset();
    for (LateWindow& late : late_work_) late.Reset();
    first_submission_window_.Reset();
    fg_report_age_frames_.fill(LEAD_WORK_STALE_FRAMES + 1);
    lifecycle_known_ = false;
    lifecycle_ = PacingPlanKey{};
    newest_index_ = 0;
    ResetSpikeFloor();
    statistics_ = LeadStatistics{};
    qpc_frequency_ = qpc_frequency > 0 ? qpc_frequency : 0;
    if (qpc_frequency <= 0) ++statistics_.invalid_inputs;
    queue_wait_live_ = false; // the owner's SetQueueWaitLive says when the stream is live again
}

void LeadController::ResetBuckets() noexcept
{
    for (Bucket& bucket : buckets_) ResetBucket(&bucket);
    for (LeadSampleWindow& calls : call_windows_) calls.Reset();
    for (LeadSampleWindow& work : work_windows_) work.Reset();
    for (LeadSampleWindow& work : idle_work_windows_) work.Reset();
    work_age_frames_.fill(0);
    for (LateWindow& late : late_arrivals_) late.Reset();
    for (LateWindow& late : late_work_) late.Reset();
    first_submission_window_.Reset();
    fg_report_age_frames_.fill(LEAD_WORK_STALE_FRAMES + 1);
    ResetSpikeFloor();
    // Frames already released (lead_controller.h): no sample, and their later
    // events add nothing to the release-relative windows.
    for (PendingFrame& frame : pending_) {
        if (!frame.occupied || !frame.started) continue;
        frame.finalized = true;
        frame.lead_sampled = true;
        frame.call_recorded = true;
        frame.work_recorded = true;
        frame.late_work_recorded = true;
        frame.spike_recorded = true;
        frame.spike_gpu_recorded = true;
    }
}

void LeadController::ResetSpikeFloor() noexcept
{
    spike_memory_ = SpikeMemory{};
    spike_tail_us_ = 0.0;
    spike_gpu_us_ = {};
    spike_gpu_head_ = 0;
    spike_gpu_count_ = 0;
    spike_gpu_median_us_ = 0.0;
}

// ── Saturation rule (lead_controller.h; unified pacer only) ──
//
// Invariant: a hold may delay the next frame's GPU work only while the GPU has
// slack; it must never lower the GPU's throughput below the start rate. The
// lead sample of a GPU-rung frame minus the queue wait removes the positive
// feedback hold → queue wait → larger lead → more holds.

void LeadController::SetQueueWaitMode(bool enabled) noexcept
{
    if (enabled == queue_wait_mode_) return;
    DropDeferredSamples();
    for (PendingFrame& frame : pending_) {
        frame.z_known = false;
        frame.z_deferred = false;
        frame.z_used_us = 0.0f;
    }
    queue_wait_mode_ = enabled;
    queue_wait_live_ = false;
}

void LeadController::SetQueueWaitLive(bool live) noexcept
{
    if (live == queue_wait_live_) return;
    const bool was_live = queue_wait_live_;
    queue_wait_live_ = live;
    if (was_live && !live) DropDeferredSamples(); // each frame feeds once: no sample for them
}

void LeadController::DropDeferredSamples() noexcept
{
    for (PendingFrame& frame : pending_) {
        if (!frame.occupied || !frame.z_deferred || frame.lead_sampled) continue;
        frame.z_deferred = false;
        frame.lead_sampled = true;
        ++statistics_.queue_wait_dropped;
    }
}

// The one liveness rule (lead_controller.h): at the moment a GPU-rung frame's
// sample would be emitted it waits for z_used while the stream is live, and is
// fed as today otherwise.
//
// The ONE bound of a deferred frame is LEAD_QUEUE_WAIT_LIFETIME_FRAMES (22
// frames, lead_controller.h: min(the law's 32-frame window, the pending
// table's 32 slots − the 8 frames events may open ahead − 2)): the frame and
// the predecessor inputs its sample reads stay in the table that long, and a
// z_used delivered up to that many releases after the frame's own release
// feeds it. A later report feeds nothing: the frame is dropped and counted
// (queue_wait_dropped) and the report is ignored. The legacy 8-frame expiry
// (LEAD_PENDING_EXPIRY_FRAMES) still closes the frame's gate evidence and
// every frame that is not deferred, and applies unchanged with the rule off;
// it no longer ends a deferred frame's sample.
bool LeadController::LeadSampleDefers(const PendingFrame& frame) const noexcept
{
    return queue_wait_mode_ && queue_wait_live_ && !frame.z_known && IsGpuRung(frame.rung);
}

// Exact identity and the frame's state, then z_used stored (z_known): another
// key (epoch, generation), a reused slot, an unreleased, already sampled or
// dropped frame, a frame that already has one, or a non-GPU rung feeds
// nothing. Returns the slot, nullptr when the report is ignored.
LeadController::PendingFrame* LeadController::AcceptQueueWaitUsed(const PacingPlanKey& key, uint64_t grid_index,
    double z_used_us) noexcept
{
    if (!queue_wait_mode_) return nullptr;
    if (!std::isfinite(z_used_us) || z_used_us < 0.0) {
        ++statistics_.invalid_inputs;
        return nullptr;
    }
    PendingFrame& slot = pending_[static_cast<size_t>(grid_index % LEAD_PENDING_CAPACITY)];
    if (!slot.occupied || !slot.started || slot.grid_index != grid_index || slot.key != key ||
        slot.lead_sampled || slot.z_known || !IsGpuRung(slot.rung)) {
        return nullptr;
    }
    slot.z_known = true;
    slot.z_used_us = static_cast<float>(z_used_us);
    return &slot;
}

// The owner records the frame's z_used, then the stage events the sample
// reads (the predecessor's readiness, resume and output), then calls
// FlushQueueWaitSamples: a deferred frame is never emitted before that.
void LeadController::RegisterQueueWaitUsed(const PacingPlanKey& key, uint64_t grid_index,
    double z_used_us) noexcept
{
    AcceptQueueWaitUsed(key, grid_index, z_used_us);
}

// In grid order, so a frame's predecessor evidence is complete first: every
// frame whose z_used is registered and whose sample is still to come
// (TryFinalize: own fields and predecessor resolved).
void LeadController::FlushQueueWaitSamples() noexcept
{
    if (!queue_wait_mode_ || !lifecycle_known_) return;
    const uint64_t window = LEAD_PENDING_CAPACITY - 1;
    const uint64_t first = newest_index_ >= window ? newest_index_ - window : 0;
    for (uint64_t index = first; index <= newest_index_; ++index) {
        PendingFrame* frame = Find(index);
        if (frame != nullptr && frame->started && !frame->lead_sampled && frame->z_known) {
            TryFinalize(frame, true);
        }
    }
}

// A direct caller: Register and flush that one frame at once.
void LeadController::OnQueueWaitUsed(const PacingPlanKey& key, uint64_t grid_index, double z_used_us) noexcept
{
    PendingFrame* slot = AcceptQueueWaitUsed(key, grid_index, z_used_us);
    if (slot == nullptr) return;
    if (slot->z_deferred) {
        slot->z_deferred = false;
        EmitLeadSample(slot);
        return;
    }
    TryFinalize(slot); // the sample is still to come: it now carries z_used
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
    // Idle-GPU evidence (lead_controller.h): the heavy-work window this plan reads, selected before anything
    // that asks WorkCurrent (the spike floor's BehindWait, the guard).
    if (request.rung == PacingRung::GpuQueueWait) {
        idle_evidence_ = request.idle_evidence;
        if (guard != nullptr) guard->idle_evidence = request.idle_evidence;
    }
    const double interval_us = request.interval_us;
    const double max_lead_us = LEAD_BUDGET_INTERVALS * interval_us;
    bucket.quantile_us = bucket.window.Quantile(ValidQuantile(request.quantile));
    if (bucket.window.Count() < LEAD_WARM_SAMPLES) {
        bucket.target_us = std::clamp(interval_us / 2.0, LEAD_SEED_MIN_US, LEAD_SEED_MAX_US);
        bucket.lead_us = std::min(bucket.target_us, max_lead_us);
        bucket.budget_us = max_lead_us;
        bucket.budget_limited = false;
        bucket.spike_floor_limited = false;
        return CollisionGuardedLeadUs(request, bucket_index, bucket.lead_us, guard);
    }

    bucket.budget_us = max_lead_us;
    const double own_us = bucket.quantile_us + ValidAllowance(request.allowance_us);
    // The lead is limited to 3 I (the outlier bound); the spike floor
    // (lead_controller.h) is not part of that budget.
    bucket.budget_limited = own_us > max_lead_us;
    if (bucket.budget_limited) ++statistics_.budget_limited_frames;
    if (budget_limited != nullptr) *budget_limited = bucket.budget_limited;
    const double floor_us = request.spike_floor ? SpikeFloorUs(request.rung, request.fg_divisor) : 0.0;
    bucket.spike_floor_limited = floor_us > own_us;
    bucket.target_us = std::max(own_us, floor_us);
    if (!bucket.lead_started) {
        bucket.lead_us = bucket.target_us;
        bucket.lead_started = true;
    } else {
        const double step_us = std::min(LEAD_SLEW_MAX_US, interval_us / kIntervalSlewDivisor);
        bucket.lead_us += std::clamp(bucket.target_us - bucket.lead_us, -step_us, step_us);
    }
    bucket.lead_us = std::clamp(bucket.lead_us, 0.0, max_lead_us);
    return CollisionGuardedLeadUs(request, bucket_index, bucket.lead_us, guard);
}

// The GPU queue wait lead (the bucket's lead) is at most I + Q10(D) − H (release
// headroom) while the work window is warm and current, else at most
// I + Q10(C) − M (release margin) while the call window is warm
// (lead_controller.h). Neither while the newest queue-order pairs show the
// next frame's GPU work running behind the wait: it cannot delay the resume
// there. A tighter limit applies at once; once it relaxes, the lead returns
// to the bucket's lead at the lead slew, never in one step. The guard is the
// limit applied to every frame planned with the GPU queue wait's lead.
double LeadController::CollisionGuardedLeadUs(const LeadRequest& request, size_t bucket_index,
    double lead_us, LeadGuardOutcome* guard) noexcept
{
    Bucket& bucket = buckets_[bucket_index];
    const size_t divisor = bucket_index % LEAD_DIVISOR_BUCKETS;
    double returned_us = lead_us;
    bool judged = false;
    // Late state (lead_controller.h): the arrival lateness of every planned GPU queue wait frame, in plan order.
    if (request.rung == PacingRung::GpuQueueWait) {
        late_arrivals_[divisor].PushConsecutive(request.late_index,
            std::isfinite(request.late_us) && request.late_us > 0.0 ? request.late_us : 0.0);
    }
    if (request.rung == PacingRung::GpuQueueWait && request.collision_guard &&
        !BehindWait(PacingRung::GpuQueueWait, divisor)) {
        double offset_us = 0.0;
        bool headroom = false;
        if (CollisionOffsetUs(divisor, request.release_headroom_us, &offset_us, &headroom)) {
            judged = true;
            // Late state: only where the heavy-work guard would otherwise apply (its window warm and current).
            // A request that only observes gets the verdict, not the limit.
            bool late_state = false;
            bool late_seen = false;
            if (headroom && (request.late_guard || request.late_observe)) {
                double late_offset_us = offset_us;
                if (LateOffsetUs(divisor, request, &late_offset_us)) {
                    late_state = request.late_guard;
                    late_seen = !request.late_guard;
                    if (late_state) offset_us = late_offset_us;
                }
            }
            double limit_us = request.interval_us + offset_us;
            const double release_limit_us = limit_us;
            // Physical terms (lead_controller.h, anchor_offset_us): H − R =
            // L − delta is what the guard limits, so delta joins the limit.
            if (request.anchor_offset_us > 0.0) limit_us += request.anchor_offset_us;
            ++statistics_.collision_checked_frames;
            if (headroom) ++statistics_.headroom_checked_frames;
            statistics_.last_guard_headroom = headroom;
            statistics_.last_collision_limit_us = limit_us;
            if (guard != nullptr) {
                guard->judged = true;
                guard->offset_us = offset_us;
                guard->late_state = late_state;
                guard->late_seen = late_seen;
                guard->release_limit_us = request.late_guard && release_limit_us > 0.0 ? release_limit_us : 0.0;
            }
            returned_us = std::min(returned_us, std::max(0.0, limit_us));
        }
    }
    if (bucket.held_back) {
        const double step_us = std::min(LEAD_SLEW_MAX_US, request.interval_us / kIntervalSlewDivisor);
        returned_us = std::min(returned_us, bucket.returned_lead_us + step_us);
    }
    bucket.held_back = returned_us < lead_us;
    bucket.returned_lead_us = returned_us;
    if (bucket.held_back) {
        if (judged) ++statistics_.collision_guarded_frames;
        if (guard != nullptr) guard->reduction_us = lead_us - returned_us;
    }
    return returned_us;
}

const LeadSampleWindow* LeadController::WorkWindow(size_t divisor, bool idle_evidence) const noexcept
{
    if (work_age_frames_[divisor] > LEAD_WORK_STALE_FRAMES) return nullptr;
    if (idle_evidence && idle_work_windows_[divisor].Count() >= LEAD_WARM_SAMPLES) {
        return &idle_work_windows_[divisor];
    }
    return work_windows_[divisor].Count() >= LEAD_WARM_SAMPLES ? &work_windows_[divisor] : nullptr;
}

bool LeadController::WorkCurrent(size_t divisor, bool idle_evidence) const noexcept
{
    return WorkWindow(divisor, idle_evidence) != nullptr;
}

bool LeadController::CollisionOffsetUs(size_t divisor, double headroom_us, double* offset_us,
    bool* headroom) const noexcept
{
    if (const LeadSampleWindow* work = WorkWindow(divisor, idle_evidence_)) {
        const double distance_us = IsPositiveFinite(headroom_us) ? headroom_us : LEAD_RELEASE_HEADROOM_US;
        *offset_us = work->Quantile(LEAD_WORK_QUANTILE) - distance_us;
        *headroom = true;
        return true;
    }
    if (call_windows_[divisor].Count() >= LEAD_WARM_SAMPLES) {
        *offset_us = call_windows_[divisor].Quantile(LEAD_CALL_QUANTILE) - LEAD_RELEASE_MARGIN_US;
        *headroom = false;
        return true;
    }
    return false;
}

bool LeadController::LateOffsetUs(size_t divisor, const LeadRequest& request, double* offset_us) const noexcept
{
    const LateWindow& arrivals = late_arrivals_[divisor];
    const LateWindow& work = late_work_[divisor];
    if (!arrivals.Full() || !work.Full()) return false;
    // D no older than twice the horizon: the reports of the newest frames, a few frames behind the plans.
    if (request.late_index < work.OldestIndex() || request.late_index - work.OldestIndex() > 2 * LEAD_LATE_FRAMES) {
        return false;
    }
    const double late_us = arrivals.Min();
    if (!(late_us > LEAD_LATE_ARRIVAL_US)) return false;
    const double distance_us = IsPositiveFinite(request.release_headroom_us)
        ? request.release_headroom_us : LEAD_RELEASE_HEADROOM_US;
    *offset_us = late_us + work.Min() - distance_us;
    return true;
}

double LeadController::MedianSampleUs(PacingRung rung, double fg_divisor) const noexcept
{
    size_t index = 0;
    if (!FindBucketIndex(rung, fg_divisor, &index)) return 0.0;
    const LeadSampleWindow& window = buckets_[index].window;
    if (window.Count() < LEAD_WARM_SAMPLES) return 0.0;
    return window.Quantile(0.5);
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
    status.spike_floor_us = SpikeFloorUs(rung, fg_divisor);
    status.spike_floor_limited = bucket.spike_floor_limited;
    return status;
}

double LeadController::SpikeFloorUs(PacingRung rung, double fg_divisor) const noexcept
{
    size_t index = 0;
    if (!IsGpuRung(rung) || !FindBucketIndex(rung, fg_divisor, &index) ||
        !BehindWait(rung, index % LEAD_DIVISOR_BUCKETS) || spike_memory_.count < LEAD_SPIKE_WARM_BLOCKS ||
        spike_gpu_count_ < LEAD_SPIKE_GPU_SAMPLES) {
        return 0.0;
    }
    return spike_tail_us_ + spike_gpu_median_us_;
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
        !IsPositiveFinite(frame.interval_us) || frame.output_target_qpc <= 0 ||
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
    slot.lead_sampled = !slot.sampled;
    slot.rung = frame.rung;
    slot.bucket = static_cast<uint8_t>(bucket_index);
    slot.interval_us = frame.interval_us;
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
    slot.late_state = frame.guard.late_state;
    slot.idle_evidence = frame.guard.idle_evidence;
    // Lead anchor (lead_controller.h, unified pacer only): the owner sets
    // frame.anchored only there, so with the key off this is one not-taken
    // branch on a bool and the slot keeps its zero from the whole-struct
    // reset (slots are only reset that way). s keeps its physical meaning in
    // every check; delta = (s − B) enters only the lead quantities
    // (EmitLeadSample, RecordSpikeArrival). An anchor not
    // earlier than s is none.
    if (frame.anchored && frame.lead_anchor_qpc > 0 && frame.lead_anchor_qpc < frame.source_release_qpc) {
        slot.anchor_delta_us = static_cast<float>(
            MicrosecondsBetween(frame.lead_anchor_qpc, frame.source_release_qpc));
        slot.anchored = slot.anchor_delta_us > 0.0f;
    }
    SetField(&slot, FieldSource, frame.source_release_qpc);
    // The work window's age counts every frame planned with the GPU queue
    // wait's lead (held); the FG input wait's report age counts the frames it held.
    slot.gpu_wait_lead = slot.sampled && frame.rung == PacingRung::GpuQueueWait;
    if (slot.sampled && (slot.gpu_wait_lead || slot.rung == PacingRung::FgInputWait)) {
        uint32_t& age = slot.gpu_wait_lead
            ? work_age_frames_[slot.bucket % LEAD_DIVISOR_BUCKETS]
            : fg_report_age_frames_[slot.bucket % LEAD_DIVISOR_BUCKETS];
        if (age < UINT32_MAX) ++age;
    }
    RecordCallTime(&slot);
    RecordWorkStart(&slot);
    RecordWorkStart(Find(index + 1));
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

    const uint8_t kind = static_cast<uint8_t>(event.kind);
    if (qpc_frequency_ <= 0 || kind == FieldSource || kind >= FieldCount || event.qpc <= 0 ||
        !PacingPlanKey_IsValid(event.key)) {
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

    const Field field = static_cast<Field>(kind);
    if (HasField(*frame, field)) return; // the first value of each kind wins
    SetField(frame, field, event.qpc);
    if (field == FieldOutputRelease) frame->output_status = event.status;
    if (field == FieldGpuResume) frame->gpu_wait_qpc = event.gpu_wait_qpc;
    if (!frame->started) ++frame->early_events;
    RecordResumeDelay(frame);
    RecordCallTime(frame);
    RecordWorkStart(frame);
    // D of frame n + 1 follows this frame's gate verdict (GpuReady, GpuResume).
    RecordWorkStart(Find(index + 1));
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

// The Reflex report of an NvAPI frame arrives two to four frames after its
// source release, while the frame is still in the pending table (32 slots).
void LeadController::OnGpuReport(uint32_t producer_epoch, uint64_t frame_id, const LeadGpuReport& report) noexcept
{
    if (qpc_frequency_ <= 0 || producer_epoch == 0 || !IsPositiveFinite(report.active_us) ||
        !std::isfinite(report.span_us) || report.span_us < 0.0) {
        ++statistics_.invalid_inputs;
        return;
    }
    for (PendingFrame& frame : pending_) {
        if (!frame.occupied || !frame.started || frame.key.identity_domain != PacingIdentityDomain::NvApiFrame ||
            frame.key.producer_epoch != producer_epoch || frame.key.frame_id != frame_id) {
            continue;
        }
        if (frame.gpu_active_us > 0.0) return; // the first report wins
        frame.gpu_active_us = report.active_us;
        frame.gpu_span_us = report.span_us;
        frame.first_work_streamed = report.first_work_streamed;
        frame.gpu_start_qpc = report.start_qpc > 0 ? report.start_qpc : 0;
        // Any report of a frame planned with the GPU queue wait's lead keeps
        // the work window current, whether or not it adds a D sample.
        if (frame.gpu_wait_lead) work_age_frames_[frame.bucket % LEAD_DIVISOR_BUCKETS] = 0;
        if (frame.sampled && frame.rung == PacingRung::FgInputWait) {
            fg_report_age_frames_[frame.bucket % LEAD_DIVISOR_BUCKETS] = 0;
        }
        RecordWorkStart(&frame);
        RecordSpikeArrival(&frame, report.first_submission_delay_us);
        RecordSpikeGpu(&frame);
        if (frame.grid_index > 0) {
            RecordReleaseHeadroom(Find(frame.grid_index - 1));
            RecordConcurrency(Find(frame.grid_index - 1));
        }
        // A lead sample behind the wait waits for this report, after the pair
        // above: a concurrent pair ends the verdict first.
        TryFinalize(&frame);
        return;
    }
    ++statistics_.unmatched_gpu_active;
}

// Report pairs (lead_controller.h).
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
}

bool HandoffFirstWorkStreams(double render_start_us, double render_end_us, double first_submission_us) noexcept
{
    return std::isfinite(render_start_us) && std::isfinite(render_end_us) && std::isfinite(first_submission_us) &&
        render_end_us > render_start_us && first_submission_us < (render_start_us + render_end_us) / 2.0;
}

bool HandoffBackPressure_Streams(const HandoffBackPressureSample& sample) noexcept
{
    return ValidPair(sample) &&
        HandoffFirstWorkStreams(sample.render_start_us, sample.render_end_us, sample.first_submission_us);
}

void LeadController::OnReportPair(uint32_t producer_epoch, uint64_t frame_id,
    const HandoffBackPressureSample& sample) noexcept
{
    if (producer_epoch == 0 || frame_id == 0) {
        ++statistics_.invalid_inputs;
        return;
    }
    if (HandoffBackPressure_Streams(sample)) {
        first_submission_window_.Add(sample.first_submission_us - sample.render_start_us);
    }
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
    bucket->returned_lead_us = 0.0;
    bucket->held_back = false;
    bucket->budget_us = 0.0;
    bucket->lead_started = false;
    bucket->budget_limited = false;
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
        if (!frame.occupied) continue;
        if (frame.closed) {
            // Saturation rule: a frame whose sample is kept for z_used ends at
            // the one lifetime bound (LeadSampleDefers).
            if (queue_wait_mode_ && !frame.lead_sampled &&
                frame.grid_index + LEAD_QUEUE_WAIT_LIFETIME_FRAMES < newest_index_) {
                ForceFinalize(&frame);
            }
            continue;
        }
        if (frame.grid_index + LEAD_PENDING_EXPIRY_FRAMES > newest_index_) continue;
        if (!frame.started) {
            Discard(&frame);
            continue;
        }
        if (!frame.finalized || !frame.lead_sampled) ForceFinalize(&frame);
        frame.closed = true;
        // A probe that did not arrive can no longer decide the gate verdict:
        // the successor's D sample follows the readiness now.
        RecordWorkStart(Find(frame.grid_index + 1));
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
    resume_delays_us_[resume_delay_head_] = static_cast<float>(delay_us);
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
// one release margin observation (measured, not acted on). Frame n + 1 must
// be a paced GPU queue wait frame too: a pair across a rung or mode change
// (focus loss, idle cap, overload) is no evidence about the release timing.
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
    if (!std::isfinite(margin_us)) return;
    ++statistics_.margin_samples;
    if (frame->resume_late) ++statistics_.margin_late_samples;
}

// D = r − A − s of a sampled GPU queue wait frame with its readiness and a
// Reflex GPU active time: the source release → heavy GPU work time, at most
// 3 I. A frame whose GPU work outlasted its readiness span counts as 0. After
// a predecessor planned as built, the built window takes it when that
// predecessor's GPU waited at its gate (lead_controller.h): the verdict may
// arrive after the frame's own evidence, and the sample is added when it does
// (OnStageEvent revisits the successor). After a predecessor planned with the
// idle-GPU evidence (LeadRequest::idle_evidence), that evidence's window takes
// it then too, and also when the heavy work began at least H after the
// predecessor's readiness. No sample when the predecessor is missing, of
// another lifecycle, producer epoch or FG divisor, or planned without the
// GPU queue wait.
void LeadController::RecordWorkStart(PendingFrame* frame) noexcept
{
    if (frame == nullptr || frame->work_recorded || !frame->started || !frame->sampled ||
        frame->rung != PacingRung::GpuQueueWait || !(frame->gpu_active_us > 0.0) ||
        !HasField(*frame, FieldGpuReady)) {
        return;
    }
    const size_t divisor = frame->bucket % LEAD_DIVISOR_BUCKETS;
    const double work_us = MicrosecondsBetween(frame->qpc[FieldSource], frame->qpc[FieldGpuReady]) -
        frame->gpu_active_us;
    const bool in_range = std::isfinite(work_us) && work_us <= LEAD_LONG_SAMPLE_INTERVALS * frame->interval_us;
    // Late state (lead_controller.h): the D of every reported frame, whatever its predecessor.
    if (!frame->late_work_recorded) {
        frame->late_work_recorded = true;
        if (in_range) late_work_[divisor].Push(frame->grid_index, std::max(0.0, work_us));
    }
    const PendingFrame* previous = Previous(*frame);
    LeadGate verdict = LeadGate::NotHeld;
    bool previous_late_state = false;
    bool previous_idle_evidence = false;
    double idle_us = -1.0; // the GPU's idle time in front of this frame's heavy work, where known
    if (previous != nullptr && previous->rung == PacingRung::GpuQueueWait && previous->sampled &&
        previous->bucket % LEAD_DIVISOR_BUCKETS == divisor) {
        verdict = previous->started ? GpuGateVerdict(*previous) : LeadGate::Pending;
        if (verdict == LeadGate::Pending) return;
        previous_late_state = previous->late_state;
        previous_idle_evidence = previous->idle_evidence;
        // The heavy work's start against the predecessor's readiness, D not clamped: a frame whose GPU work
        // began before its own release did not start behind an idle GPU. The predecessor's readiness
        // precedes this frame's own in the event stream; a predecessor that never got one gives no sample.
        if (HasField(*previous, FieldGpuReady)) {
            idle_us = MicrosecondsBetween(previous->qpc[FieldGpuReady], frame->qpc[FieldGpuReady]) -
                frame->gpu_active_us;
        }
    }
    frame->work_recorded = true;
    if (!in_range) return;
    // A predecessor planned in the late state was held under that state's limit: not a waited predecessor
    // here, so the windows stay what on-time frames taught them.
    const bool waited = verdict == LeadGate::Held && !previous_late_state;
    // Each window takes D only after a predecessor planned with its own rule, so each stays what its rule
    // alone learned (lead_controller.h, LeadRequest::idle_evidence).
    if (previous_idle_evidence) {
        if (!waited && !(idle_us >= LEAD_RELEASE_HEADROOM_US)) return;
        idle_work_windows_[divisor].Add(std::max(0.0, work_us));
        ++statistics_.idle_work_samples;
        return;
    }
    if (!waited) return;
    work_windows_[divisor].Add(std::max(0.0, work_us));
    ++statistics_.work_samples;
}

// A waited GPU queue wait resume of frame n and the start of frame n + 1's
// heavy GPU work, r_{n+1} − A_{n+1}: one release headroom observation
// (measured, not acted on). As for the margin, frame n + 1 must be a paced
// GPU queue wait frame; one without a Reflex GPU active time yet is waited for.
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
}

// A waited GPU-rung release o_n and frame n + 1's GPU start g_{n+1}: one
// queue-order pair of that output (lead_controller.h) when frame n + 1 had
// begun submitting by o_n. Waited: the probe on the GPU queue wait, the
// readiness before the Signal on the FG input wait (no probe there). As for
// the headroom, frame n + 1 must be a paced frame of the same output; one
// without its report yet is waited for.
void LeadController::RecordConcurrency(PendingFrame* frame) noexcept
{
    if (frame == nullptr || frame->concurrency_recorded || !IsGpuRung(frame->rung)) return;
    const bool fg = frame->rung == PacingRung::FgInputWait;
    if (!(fg ? PreviousGpuHeld(*frame) : frame->resume_waited)) return;
    const PendingFrame* next = Find(frame->grid_index + 1);
    if (next == nullptr || !SameLifecycle(next->key, frame->key) || !next->started) return;
    if (next->rung != frame->rung || !next->sampled) {
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
    QueueOrderWindow& window = queue_order_[fg ? 1 : 0];
    uint8_t& slot = window.pairs[window.head];
    if (window.count == LEAD_CONCURRENCY_SAMPLES) window.concurrent -= slot;
    else ++window.count;
    slot = concurrent;
    window.concurrent += concurrent;
    window.head = (window.head + 1) % LEAD_CONCURRENCY_SAMPLES;
    if (fg) {
        ++statistics_.fg_concurrency_samples;
        statistics_.fg_concurrent_samples += concurrent;
        statistics_.last_fg_concurrency_us = start_us;
    } else {
        ++statistics_.concurrency_samples;
        statistics_.concurrent_samples += concurrent;
        statistics_.last_concurrency_us = start_us;
    }
}

bool LeadController::BehindWait(PacingRung rung, size_t divisor) const noexcept
{
    return BehindWait(rung, divisor, idle_evidence_);
}

bool LeadController::BehindWait(PacingRung rung, size_t divisor, bool idle_evidence) const noexcept
{
    if (!IsGpuRung(rung)) return false;
    const bool fg = rung == PacingRung::FgInputWait;
    const QueueOrderWindow& window = queue_order_[fg ? 1 : 0];
    if (window.count < LEAD_WARM_SAMPLES || window.concurrent != 0) return false;
    return fg ? fg_report_age_frames_[divisor] <= LEAD_WORK_STALE_FRAMES : WorkCurrent(divisor, idle_evidence);
}

bool LeadController::NextGpuWorkBehindWait(PacingRung rung, double fg_divisor) const noexcept
{
    size_t index = 0;
    return IsGpuRung(rung) && FindBucketIndex(rung, fg_divisor, &index) &&
        BehindWait(rung, index % LEAD_DIVISOR_BUCKETS);
}

size_t LeadController::ConcurrencyPairs(PacingRung rung) const noexcept
{
    return IsGpuRung(rung) ? queue_order_[rung == PacingRung::FgInputWait ? 1 : 0].count : 0;
}

size_t LeadController::ConcurrentPairs(PacingRung rung) const noexcept
{
    return IsGpuRung(rung) ? queue_order_[rung == PacingRung::FgInputWait ? 1 : 0].concurrent : 0;
}

double LeadController::FirstSubmissionUs() const noexcept
{
    return first_submission_window_.Count() >= LEAD_WARM_SAMPLES ? first_submission_window_.Quantile(0.5) : 0.0;
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
    case PacingRung::GpuQueueWait:
    case PacingRung::FgInputWait:
        return static_cast<uint16_t>(FieldBit(FieldSource) | FieldBit(FieldPresentCall) |
            FieldBit(FieldGpuReady));
    case PacingRung::NativeHold: // historical rung: no build plans it
    case PacingRung::Count:
        break;
    }
    return 0xFFFF; // unreachable: releases with an invalid rung are rejected
}

// A CPU hold starts at the marker arrival (H1); other rungs hold no CPU thread.
bool LeadController::CpuHoldStartField(PacingRung rung, Field* field) noexcept
{
    if (rung == PacingRung::MarkerHold) *field = FieldMarkerArrival;
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

void LeadController::TryFinalize(PendingFrame* frame, bool flush) noexcept
{
    if (frame == nullptr || !frame->occupied || !frame->started || (frame->finalized && frame->lead_sampled)) return;
    const uint16_t own = OwnFieldMask(frame->rung);
    if ((frame->field_mask & own) != own || !PreviousResolved(*frame)) return;
    if (!frame->finalized) EmitOutcome(frame);
    if (!frame->lead_sampled && !LeadSampleWaitsForReport(*frame)) {
        if (queue_wait_mode_) {
            // Saturation rule: a deferred frame waits for its flush (the
            // registered z_used is applied there, after the predecessor's
            // events); another waits for z_used while the stream is live.
            if (frame->z_deferred ? !flush : LeadSampleDefers(*frame)) {
                if (!frame->z_deferred) {
                    frame->z_deferred = true;
                    ++statistics_.queue_wait_deferred;
                }
                return;
            }
            frame->z_deferred = false;
        }
        EmitLeadSample(frame);
    }
}

void LeadController::ForceFinalize(PendingFrame* frame) noexcept
{
    const uint16_t own = OwnFieldMask(frame->rung);
    if ((frame->field_mask & own) == own) {
        if (!frame->finalized) EmitOutcome(frame);
        if (!frame->lead_sampled) {
            if (queue_wait_mode_ && LeadSampleDefers(*frame)) {
                // Saturation rule: kept for z_used through the one lifetime
                // bound (LeadSampleDefers); past it, without z_used, the
                // frame feeds nothing (never the raw sample).
                if (frame->grid_index + LEAD_QUEUE_WAIT_LIFETIME_FRAMES >= newest_index_) return;
                frame->z_deferred = false;
                frame->lead_sampled = true;
                ++statistics_.queue_wait_dropped;
                return;
            }
            // Closed behind the wait without its report: the wait for frame
            // n − 1 is left out (SampleUs).
            if (LeadSampleWaitsForReport(*frame)) ++statistics_.report_missing_samples;
            EmitLeadSample(frame);
        }
        return;
    }
    frame->finalized = true;
    frame->lead_sampled = true;
    const bool presented = HasField(*frame, FieldPresentCall);
    if (IsGpuRung(frame->rung) && presented) ++statistics_.missing_ready;
    else ++statistics_.incomplete_dropped;
}

// Behind the wait the lead sample of a frame the cap released needs the
// frame's own report: it shows whether the frame's submissions set its
// readiness (SampleUs). Reports arrive two to four frames after the release.
bool LeadController::LeadSampleWaitsForReport(const PendingFrame& frame) const noexcept
{
    return IsGpuRung(frame.rung) && !(frame.gpu_active_us > 0.0) &&
        BehindWait(frame.rung, frame.bucket % LEAD_DIVISOR_BUCKETS, frame.idle_evidence);
}

bool LeadController::GpuIdleInSpanUs(const PendingFrame& frame, double* idle_us) const noexcept
{
    if (!(frame.gpu_active_us > 0.0) || !(frame.gpu_span_us > 0.0)) return false;
    double hold_us = 0.0;
    if (frame.rung == PacingRung::FgInputWait) {
        if (!HasField(frame, FieldOutputRelease) || !HasField(frame, FieldGpuReady)) return false;
        hold_us = std::max(0.0, MicrosecondsBetween(frame.qpc[FieldGpuReady], frame.qpc[FieldOutputRelease]));
    }
    *idle_us = std::max(0.0, frame.gpu_span_us - frame.gpu_active_us - hold_us);
    return true;
}

void LeadController::EmitOutcome(PendingFrame* frame) noexcept
{
    frame->finalized = true;
    const double raw_us = MeasuredUs(*frame, Previous(*frame));
    if (!std::isfinite(raw_us)) {
        frame->lead_sampled = true;
        ++statistics_.invalid_inputs;
        return;
    }
    RecordSpikeGpu(frame);
}

void LeadController::EmitLeadSample(PendingFrame* frame) noexcept
{
    frame->lead_sampled = true;
    const PendingFrame* previous = Previous(*frame);
    const double raw_us = MeasuredUs(*frame, previous);
    double predecessor_wait_us = 0.0;
    double kept_wait_us = 0.0;
    const double sample_us = SampleUs(*frame, previous, raw_us, &predecessor_wait_us, &kept_wait_us);
    if (!std::isfinite(sample_us)) {
        ++statistics_.invalid_inputs;
        return;
    }
    if (predecessor_wait_us > 0.0) {
        ++statistics_.predecessor_wait_samples;
        statistics_.last_predecessor_wait_us = predecessor_wait_us;
    }
    if (kept_wait_us > 0.0) {
        ++statistics_.kept_wait_samples;
        statistics_.last_kept_wait_us = kept_wait_us;
    }
    // FG input wait (lead_controller.h): the lead learns X without the time the
    // finished frame waited for the render thread to leave the previous Present.
    const double queue_us = QueueUs(*frame, previous);
    if (queue_us > 0.0) {
        ++statistics_.queued_samples;
        statistics_.last_queue_us = queue_us;
    }
    Bucket& bucket = buckets_[frame->bucket];
    double lead_sample_us = sample_us - queue_us;
    if (frame->z_known) {
        // Saturation rule (unified pacer only): the lead learns the readiness
        // without the queue wait. SampleUs already removed
        // predecessor_wait_us (d: the wait for frame n − 1 including its hold,
        // behind the wait) and z_used is the part of the reported wait that
        // is not that hold, so only the excess over d is removed here, never
        // the same wait twice. Check: z_used = d removes nothing more.
        const double removed_us = std::max(0.0, static_cast<double>(frame->z_used_us) - predecessor_wait_us);
        lead_sample_us = std::max(0.0, lead_sample_us - removed_us);
        ++statistics_.queue_wait_samples;
        statistics_.last_queue_wait_removed_us = removed_us;
    }
    if (lead_sample_us > LEAD_LONG_SAMPLE_INTERVALS * frame->interval_us) {
        ++bucket.excluded_long;
        ++statistics_.excluded_long;
        return;
    }
    // Lead anchor: after the range check on the sample measured from s, the
    // sample is the readiness measured from B (unified pacer only).
    double window_sample_us = lead_sample_us;
    if (frame->anchored) window_sample_us += static_cast<double>(frame->anchor_delta_us);
    bucket.window.Add(window_sample_us);
    ++statistics_.samples;
    statistics_.last_sample_us = window_sample_us;
}

// X relative to the frame's source release s:
//   H0: a − s.   H1: (a − s) − overlap([s, a], previous CPU hold).
//   H3 and FG input wait: the measured max(p, r) − s.
double LeadController::MeasuredUs(const PendingFrame& frame, const PendingFrame* previous) const noexcept
{
    const int64_t source = frame.qpc[FieldSource];
    switch (frame.rung) {
    case PacingRung::SourceOnly:
        return MicrosecondsBetween(source, frame.qpc[FieldMarkerArrival]);
    case PacingRung::MarkerHold: {
        const double arrival_us = MicrosecondsBetween(source, frame.qpc[FieldMarkerArrival]);
        double hold_begin_us = 0.0;
        double hold_end_us = 0.0;
        return PreviousCpuHold(previous, source, &hold_begin_us, &hold_end_us)
            ? arrival_us - Overlap(0.0, arrival_us, hold_begin_us, hold_end_us)
            : arrival_us;
    }
    case PacingRung::GpuQueueWait:
    case PacingRung::FgInputWait:
        return std::max(MicrosecondsBetween(source, frame.qpc[FieldPresentCall]),
            MicrosecondsBetween(source, frame.qpc[FieldGpuReady]));
    case PacingRung::NativeHold: // historical rung: no build plans it
    case PacingRung::Count:
        break;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// The lead sample from X (raw_us): X itself, except on H3 and the FG input
// wait where this output's queue order says the next frame's GPU work runs
// behind the wait (BehindWait, lead_controller.h): X = max(p, r − d) − s.
// d = max(0, f − w_c) is the time the frame's submitted GPU work waited for
// the GPU to finish frame n − 1: f = max(r_{n−1}, u_{n−1}) is when the GPU
// passed frame n − 1's gate, w_c = min(p, max(s, w + F)) where the frame's
// first GPU work reached the driver (w = RENDERSUBMIT_START, F the median
// first-submission delay; w_c = p without RENDERSUBMIT_START).
//   Behind the wait frame n's GPU work follows ReLimiter's release of frame
//   n − 1: r_n = o_{n−1} + P_n, so the measured X is L + P_n − I and tracks
//   the lead; a lead learned from it never fell (CONTROL Resonant FG 2× at
//   130: 20 ms in a light stretch, 39–43 ms after one heavier second, kept
//   after the load fell; slope of X on L 0.94–0.98). Without d the slope is
//   0.21–0.25, from the FG input call p, pinned about 4 ms after frame
//   n − 1's release while the render thread's Present waits for it, at leads
//   above about 32 ms there. The correction this replaced carried a delay
//   only after a frame whose GPU waited at its gate and only cumulatively,
//   so the 30% of frames after one that did not wait kept the measured X and
//   set the lead's Q99. f counts whether or not frame n − 1 waited: the
//   frame's own work starts where the GPU is free of frame n − 1, whoever
//   set that. The rest of frame n − 1's work after its gate (its frame
//   generation, about 2 ms) stays in X.
//   A frame whose own submissions set its readiness keeps X: r is then its
//   last submission plus the GPU work after it, which follows its own
//   release, so X does not contain o_{n−1} and d did not delay it. Its
//   report shows it: the GPU waited inside its work for its submissions
//   (span − A − the FG input wait's hold inside the span, (o − r)+, at least
//   LEAD_HANDOFF_GPU_IDLE_US) while its first work streamed
//   (HandoffFirstWorkStreams), and the cap released it. Gears of War: E-Day
//   (FG 2× at 120) with the Adaptive cap: the GPU started each cap-held
//   frame 1.7 ms after the gate and idled 2.6–3.1 ms (p50) in it; the render
//   thread started 5 ms after the release, submitted 11.5 ms, and the frame
//   was ready 12.6 ms after its last submission, so X = 28–31 ms p50
//   whatever the gate. With d left out the lead's fixed point was about
//   (Q99(X) + I)/2: 29 ms with half the frames ready after their targets,
//   and the Adaptive cap stretched the interval to +19–43% to buy lead.
//   Starvation alone is not enough: with a long lead Gears' first work
//   reached the driver only at frame n − 1's Signal (17–21 ms after
//   RENDERSUBMIT_START against 0.7–0.9 ms when streaming), and the GPU then
//   caught up with the held submissions; those frames' X tracked the lead
//   and 3.7% of them held it at 45 ms. An unknown hold (no Signal yet) keeps
//   d out. The sample
//   waits for the frame's report (LeadSampleWaitsForReport); a frame closed
//   without one leaves d out. With concurrent or
//   unknown queue order the GPU can start frame n's work beside the wait
//   (Rogue Core), so the wait for frame n − 1 is not frame n's to drop. The
//   floor at p holds because a frame is never ready before its Present call.
double LeadController::SampleUs(const PendingFrame& frame, const PendingFrame* previous, double raw_us,
    double* predecessor_wait_us, double* kept_wait_us) const noexcept
{
    if (predecessor_wait_us != nullptr) *predecessor_wait_us = 0.0;
    if (kept_wait_us != nullptr) *kept_wait_us = 0.0;
    if (!IsGpuRung(frame.rung) || previous == nullptr || !previous->started || !IsGpuRung(previous->rung) ||
        !BehindWait(frame.rung, frame.bucket % LEAD_DIVISOR_BUCKETS, frame.idle_evidence)) {
        return raw_us;
    }
    const int64_t source = frame.qpc[FieldSource];
    const double present_us = MicrosecondsBetween(source, frame.qpc[FieldPresentCall]);
    const double ready_us = MicrosecondsBetween(source, frame.qpc[FieldGpuReady]);
    bool free_known = false;
    double free_us = 0.0;
    double resume_us = 0.0;
    if (PreviousGpuResume(previous, source, &resume_us)) {
        free_us = resume_us;
        free_known = true;
    }
    if (HasField(*previous, FieldGpuReady)) {
        const double previous_ready_us = MicrosecondsBetween(source, previous->qpc[FieldGpuReady]);
        free_us = free_known ? std::max(free_us, previous_ready_us) : previous_ready_us;
        free_known = true;
    }
    if (!free_known) return raw_us;
    double start_us = present_us;
    if (HasField(frame, FieldRenderSubmit)) {
        start_us = std::clamp(MicrosecondsBetween(source, frame.qpc[FieldRenderSubmit]) + FirstSubmissionUs(),
            0.0, std::max(present_us, 0.0));
    }
    const double wait_us = free_us - start_us;
    if (!(wait_us > 0.0)) return raw_us;
    double idle_us = 0.0;
    if (frame.first_work_streamed && GpuIdleInSpanUs(frame, &idle_us) &&
        idle_us >= LEAD_HANDOFF_GPU_IDLE_US) {
        if (kept_wait_us != nullptr) *kept_wait_us = wait_us;
        return raw_us;
    }
    const double sample = std::max(present_us, ready_us - wait_us);
    if (predecessor_wait_us != nullptr) *predecessor_wait_us = raw_us - sample;
    return sample;
}

// The game's simulation → render queue (lead_controller.h):
// q = max(0, min(w, b_{n−1}) − max(s, e)), with w clamped to [s, p] as in
// SampleUs. A render thread already free before the frame finished
// (b_{n−1} <= e) or one that took it at once (w = e) gives 0. Only on the FG
// input wait; never on the GPU queue wait or the CPU rungs.
double LeadController::QueueUs(const PendingFrame& frame, const PendingFrame* previous) const noexcept
{
    if (previous == nullptr || frame.rung != PacingRung::FgInputWait) return 0.0;
    return RenderQueueWaitUs(frame, *previous);
}

double LeadController::RenderQueueWaitUs(const PendingFrame& frame, const PendingFrame& previous) const noexcept
{
    if (!HasField(frame, FieldSimulationEnd) || !HasField(frame, FieldRenderSubmit) ||
        !HasField(frame, FieldPresentCall) || !HasField(previous, FieldPresentEnd)) {
        return 0.0;
    }
    const int64_t source = frame.qpc[FieldSource];
    const double present_us = MicrosecondsBetween(source, frame.qpc[FieldPresentCall]);
    const double submit_us = std::clamp(MicrosecondsBetween(source, frame.qpc[FieldRenderSubmit]),
        0.0, std::max(present_us, 0.0));
    const double end_us = std::max(0.0, MicrosecondsBetween(source, frame.qpc[FieldSimulationEnd]));
    const double busy_until_us = std::min(submit_us,
        MicrosecondsBetween(source, previous.qpc[FieldPresentEnd]));
    const double queue_us = busy_until_us - end_us;
    return queue_us > 0.0 ? queue_us : 0.0;
}

// Spike floor arrival (lead_controller.h): a = (w − s − q) + F_n, decided once
// per frame when its report arrives (the first report wins).
void LeadController::RecordSpikeArrival(PendingFrame* frame, double first_submission_us) noexcept
{
    if (frame->spike_recorded || !frame->sampled || !IsGpuRung(frame->rung)) return;
    frame->spike_recorded = true;
    const PendingFrame* previous = Previous(*frame);
    if (!frame->first_work_streamed || !std::isfinite(first_submission_us) || first_submission_us < 0.0 ||
        previous == nullptr || !previous->started || !HasField(*frame, FieldSimulationEnd) ||
        !HasField(*frame, FieldRenderSubmit) || !HasField(*frame, FieldPresentCall) ||
        !HasField(*previous, FieldPresentEnd)) {
        ++statistics_.spike_excluded;
        return;
    }
    // The render thread took a finished frame only at or after frame n − 1's
    // Signal: that timing is ReLimiter's hold, not the game's.
    if (HasField(*previous, FieldOutputRelease)) {
        const int64_t signal_qpc = previous->qpc[FieldOutputRelease];
        if (frame->qpc[FieldSimulationEnd] < signal_qpc && signal_qpc <= frame->qpc[FieldRenderSubmit]) {
            ++statistics_.spike_excluded;
            return;
        }
    }
    const int64_t source = frame->qpc[FieldSource];
    const double present_us = MicrosecondsBetween(source, frame->qpc[FieldPresentCall]);
    const double submit_us = std::clamp(MicrosecondsBetween(source, frame->qpc[FieldRenderSubmit]),
        0.0, std::max(present_us, 0.0));
    const double arrival_us = submit_us - RenderQueueWaitUs(*frame, *previous) + first_submission_us;
    if (!(arrival_us >= 0.0) || arrival_us > LEAD_LONG_SAMPLE_INTERVALS * frame->interval_us) {
        ++statistics_.spike_excluded;
        return;
    }
    // Lead anchor: the arrival's range check above is on s; the binned arrival
    // is measured from B (unified pacer only).
    double binned_us = arrival_us;
    if (frame->anchored) binned_us += static_cast<double>(frame->anchor_delta_us);
    ++statistics_.spike_samples;
    statistics_.last_spike_arrival_us = binned_us;
    SpikeMemory& memory = spike_memory_;
    const auto bin = static_cast<uint16_t>(std::min(binned_us / LEAD_HISTOGRAM_BIN_US,
        static_cast<double>(std::numeric_limits<uint16_t>::max())));
    if (memory.current_count == 0 || bin > memory.current_max_bin) memory.current_max_bin = bin;
    if (++memory.current_count < LEAD_SPIKE_BLOCK_SAMPLES) return;
    memory.block_max_bins[memory.head] = memory.current_max_bin;
    memory.head = static_cast<uint8_t>((memory.head + 1) % LEAD_SPIKE_BLOCKS);
    if (memory.count < LEAD_SPIKE_BLOCKS) ++memory.count;
    memory.current_count = 0;
    memory.current_max_bin = 0;
    if (memory.count < LEAD_SPIKE_WARM_BLOCKS) return;
    // The closed blocks are positions [0, count) until the ring is full.
    std::array<uint16_t, LEAD_SPIKE_BLOCKS> sorted = memory.block_max_bins;
    const auto rank = sorted.begin() + static_cast<std::ptrdiff_t>(LEAD_SPIKE_TAIL_RANK - 1);
    std::nth_element(sorted.begin(), rank, sorted.begin() + memory.count, std::greater<uint16_t>());
    spike_tail_us_ = (static_cast<double>(*rank) + 0.5) * LEAD_HISTOGRAM_BIN_US;
}

// Spike floor Γ sample (lead_controller.h): r − g once both are known.
void LeadController::RecordSpikeGpu(PendingFrame* frame) noexcept
{
    if (frame->spike_gpu_recorded || !frame->sampled || !IsGpuRung(frame->rung) || frame->gpu_start_qpc <= 0 ||
        !HasField(*frame, FieldGpuReady)) {
        return;
    }
    frame->spike_gpu_recorded = true;
    const double gpu_us = MicrosecondsBetween(frame->gpu_start_qpc, frame->qpc[FieldGpuReady]);
    if (!(gpu_us > 0.0) || gpu_us > LEAD_LONG_SAMPLE_INTERVALS * frame->interval_us) return;
    spike_gpu_us_[spike_gpu_head_] = static_cast<float>(gpu_us);
    spike_gpu_head_ = static_cast<uint8_t>((spike_gpu_head_ + 1) % LEAD_SPIKE_GPU_SAMPLES);
    if (spike_gpu_count_ < LEAD_SPIKE_GPU_SAMPLES) ++spike_gpu_count_;
    ++statistics_.spike_gpu_samples;
    if (spike_gpu_count_ < LEAD_SPIKE_GPU_SAMPLES) return;
    std::array<float, LEAD_SPIKE_GPU_SAMPLES> sorted = spike_gpu_us_;
    const auto median = sorted.begin() + static_cast<std::ptrdiff_t>((LEAD_SPIKE_GPU_SAMPLES - 1) / 2);
    std::nth_element(sorted.begin(), median, sorted.end());
    spike_gpu_median_us_ = static_cast<double>(*median);
}

// The GPU-rung frame's GPU waited at its gate (probe w <= o, else held and
// ready before the Signal), from the evidence present now; Pending until its
// OutputRelease and the probe or readiness arrived. Only such a frame's hold
// can delay the next frame.
LeadGate LeadController::GpuGateEvidence(const PendingFrame& frame) noexcept
{
    if (!HasField(frame, FieldOutputRelease)) return LeadGate::Pending;
    if (!IsHeldStatus(frame.output_status)) return LeadGate::NotHeld;
    const int64_t signal_qpc = frame.qpc[FieldOutputRelease];
    if (HasField(frame, FieldGpuResume) && frame.gpu_wait_qpc != 0) {
        return frame.gpu_wait_qpc <= signal_qpc ? LeadGate::Held : LeadGate::NotHeld;
    }
    if (HasField(frame, FieldGpuReady)) return frame.qpc[FieldGpuReady] < signal_qpc ? LeadGate::Held : LeadGate::NotHeld;
    return LeadGate::Pending;
}

// A held frame whose plan expects a resume probe (gpu_resume_probe) stays
// Pending until the probe event arrives: readiness decides only when the
// probe is unavailable (the event came without a pre-wait time) or can no
// longer arrive (the frame closed), so no D sample is
// sealed on a verdict a later probe could overturn.
LeadGate LeadController::GpuGateVerdict(const PendingFrame& frame) noexcept
{
    if (HasField(frame, FieldOutputRelease) && IsHeldStatus(frame.output_status) && frame.gpu_resume_probe &&
        !HasField(frame, FieldGpuResume) && !frame.closed) {
        return LeadGate::Pending;
    }
    return GpuGateEvidence(frame);
}

bool LeadController::PreviousGpuHeld(const PendingFrame& previous) noexcept
{
    return GpuGateEvidence(previous) == LeadGate::Held;
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
