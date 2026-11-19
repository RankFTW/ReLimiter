#include "scheduler.h"
#include "source_stage.h"
#include "marker_stamps.h"
#include "pacing_resolver.h"
#include "instrumentation_state.h"
#include "fg_divisor.h"
#include "frame_splitting.h"
#include "display_state.h"
#include "vblank_thread.h"
#include "wake_guard.h"
#include "sleep_mode.h"
#include "feedback.h"
#include "nvapi_hooks.h"
#include "pll.h"
#include "enforcement_dispatcher.h"
#include "health.h"
#include "swapchain_manager.h"
#include "native_present_gate.h"
#include "streamline_hooks.h"
#include "focus_lock.h"
#include "blackout.h"
#include "period_governor.h"
#include "source_placement.h"
#include "pacing_sleep.h"
#include "frame_timeline.h"
#include "pacing_telemetry.h"
#include "gpu_queue_law_feed.h"
#include "marker_stamps.h"
#include <cstdio>
#include "logger.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <atomic>

// ── User config ──
std::atomic<int>    g_user_target_fps{0};
std::atomic<int>    g_background_fps{30};
std::atomic<int>    g_fg_off_fps{0};
std::atomic<double> g_actual_frame_time_us{0.0};
std::atomic<uint64_t> g_actual_frame_sample_sequence{0};
std::atomic<double> g_effective_interval_us{0.0};

// ── OSD cadence publication ──
// Changes only on lifecycle invalidation. Plain cadence state remains
// scheduler-owned.
static std::atomic<uint64_t> s_cadence_bypass_generation{0};
static std::atomic<uint64_t> s_published_cadence_generation{0};
static std::atomic<uint32_t> s_published_cadence_epoch{0};
static std::atomic<uint32_t> s_published_cadence_controller_generation{0};
static std::atomic<SchedulerTrigger> s_published_cadence_trigger{
    SchedulerTrigger::NvApiMarker};
static SchedulerCadenceTracker s_telemetry_cadence;

void Scheduler_ResetCadenceReadout() noexcept {
    s_cadence_bypass_generation.fetch_add(1, std::memory_order_seq_cst);
}

// ── Frame identity ──

static bool GetMarkerTimelineContext(
    SchedulerTrigger trigger,
    FrameTimelinePath* timeline_path,
    FrameIdentityDomain* identity_domain)
{
    switch (trigger) {
        case SchedulerTrigger::NvApiMarker:
            *timeline_path = FrameTimelinePath::NvApiMarkers;
            *identity_domain = FrameIdentityDomain::NvApiFrame;
            return true;
        case SchedulerTrigger::PclMarker:
            *timeline_path = FrameTimelinePath::PclMarkers;
            *identity_domain = FrameIdentityDomain::PclFrame;
            return true;
        case SchedulerTrigger::PresentFallback:
            *timeline_path = FrameTimelinePath::PresentBased;
            *identity_domain = FrameIdentityDomain::SyntheticPresentFrame;
            return true;
        default:
            return false;
    }
}

static PacingIdentityDomain ToPacingIdentityDomain(FrameIdentityDomain domain) noexcept
{
    switch (domain) {
        case FrameIdentityDomain::NvApiFrame: return PacingIdentityDomain::NvApiFrame;
        case FrameIdentityDomain::PclFrame: return PacingIdentityDomain::PclFrame;
        case FrameIdentityDomain::SyntheticPresentFrame:
            return PacingIdentityDomain::SyntheticPresentFrame;
        default: return PacingIdentityDomain::Unknown;
    }
}

PacingPlanKey Scheduler_MakePlanKey(uint64_t frame_id, SchedulerTrigger trigger) noexcept
{
    FrameTimelinePath timeline_path{};
    FrameIdentityDomain identity_domain{};
    if (!GetMarkerTimelineContext(trigger, &timeline_path, &identity_domain)) return {};
    PacingPlanKey key{};
    key.identity_domain = ToPacingIdentityDomain(identity_domain);
    if (key.identity_domain == PacingIdentityDomain::Unknown) return {};
    key.producer_epoch = FrameTimeline_CurrentProducerEpoch(identity_domain);
    key.controller_generation = FrameTimeline_CurrentControllerGeneration();
    key.frame_id = frame_id;
    return key;
}

static SchedulerCadenceContext GetCadenceContext(
    const PacingPlanKey& key,
    SchedulerTrigger trigger,
    PacingSourceMarker marker,
    SchedulerCadenceRegime regime,
    int target_fps = 0) noexcept
{
    SchedulerCadenceContext context{};
    context.frame_id = key.frame_id;
    context.producer_epoch = key.producer_epoch;
    context.controller_generation = key.controller_generation;
    context.bypass_generation =
        s_cadence_bypass_generation.load(std::memory_order_seq_cst);
    context.trigger = trigger;
    context.marker = static_cast<uint8_t>(marker);
    context.regime = regime;
    context.target_fps = target_fps;
    return context;
}

static SchedulerCadenceObservation PublishCadence(
    const SchedulerCadenceContext& context,
    int64_t timestamp_qpc) noexcept
{
    const PacingPlanKey current = Scheduler_MakePlanKey(
        context.frame_id, context.trigger);
    const bool generation_current =
        context.producer_epoch == current.producer_epoch &&
        context.controller_generation == current.controller_generation &&
        context.bypass_generation ==
            s_cadence_bypass_generation.load(std::memory_order_seq_cst);
    const SchedulerCadenceObservation observation = s_telemetry_cadence.Observe(
        context, timestamp_qpc, generation_current);

    // Atomic payload plus a bounded sequence read: no reader/writer spinning,
    // no extra owner acquisition, and no torn double on Win32. Sequentially
    // consistent accesses keep the whole publication in one total order.
    g_actual_frame_sample_sequence.fetch_add(1, std::memory_order_seq_cst);
    g_actual_frame_time_us.store(
        observation.valid ? qpc_to_us(observation.interval_qpc) : 0.0,
        std::memory_order_seq_cst);
    s_published_cadence_generation.store(
        context.bypass_generation, std::memory_order_seq_cst);
    s_published_cadence_epoch.store(
        context.producer_epoch, std::memory_order_seq_cst);
    s_published_cadence_controller_generation.store(
        context.controller_generation, std::memory_order_seq_cst);
    s_published_cadence_trigger.store(context.trigger, std::memory_order_seq_cst);
    g_actual_frame_sample_sequence.fetch_add(1, std::memory_order_seq_cst);
    return observation;
}

bool Scheduler_ReadActualFrameSample(SchedulerActualFrameSample* sample) noexcept
{
    if (sample == nullptr) return false;
    *sample = {};
    const uint64_t sequence =
        g_actual_frame_sample_sequence.load(std::memory_order_seq_cst);
    if ((sequence & 1u) != 0) return false;
    const double frame_time_us =
        g_actual_frame_time_us.load(std::memory_order_seq_cst);
    const uint64_t generation =
        s_published_cadence_generation.load(std::memory_order_seq_cst);
    const uint32_t epoch =
        s_published_cadence_epoch.load(std::memory_order_seq_cst);
    const uint32_t controller_generation =
        s_published_cadence_controller_generation.load(std::memory_order_seq_cst);
    const PacingPlanKey current = Scheduler_MakePlanKey(
        0, s_published_cadence_trigger.load(std::memory_order_seq_cst));
    const bool generation_current = generation ==
        s_cadence_bypass_generation.load(std::memory_order_seq_cst) &&
        epoch == current.producer_epoch &&
        controller_generation == current.controller_generation;
    if (sequence !=
        g_actual_frame_sample_sequence.load(std::memory_order_seq_cst)) {
        return false;
    }
    sample->sequence = sequence;
    sample->valid = sequence != 0 && generation_current && frame_time_us > 0.0;
    sample->frame_time_us = sample->valid ? frame_time_us : 0.0;
    return true;
}

// ── Source-stage status (OSD pacing panel): seqlock over atomics ──

namespace {
std::atomic<uint64_t> s_status_sequence{0}; // odd while publishing
std::atomic<bool> s_status_valid{false};
std::atomic<double> s_status_lead_us{0.0};
std::atomic<bool> s_status_lead_warm{false};
std::atomic<uint32_t> s_status_lead_samples{0};
std::atomic<uint32_t> s_status_grid_epoch{0};
std::atomic<uint64_t> s_status_grid_index{0};
std::atomic<uint8_t> s_status_epoch_reason{0};
std::atomic<uint64_t> s_status_stage_events_dropped{0};
std::atomic<bool> s_status_lead_budget_limited{false};
std::atomic<double> s_status_lead_quantile_us{0.0};
std::atomic<double> s_status_lead_floor_us{0.0};
std::atomic<double> s_status_lead_floor_tail_us{0.0};
std::atomic<double> s_status_lead_floor_gpu_us{0.0};
std::atomic<bool> s_status_lead_floor_limited{false};
std::atomic<double> s_status_release_margin_us{0.0};
std::atomic<double> s_status_release_headroom_us{0.0};
std::atomic<bool> s_status_collision_headroom{false};
std::atomic<double> s_status_collision_limit_us{0.0};
std::atomic<uint32_t> s_status_collision_checked{0};
std::atomic<uint32_t> s_status_collision_guarded{0};
std::atomic<bool> s_status_collision_behind_wait{false};
std::atomic<uint32_t> s_status_queue_order_pairs{0};
std::atomic<uint32_t> s_status_release_margin_samples{0};
std::atomic<uint32_t> s_status_release_margin_late{0};
std::atomic<uint32_t> s_status_release_headroom_samples{0};
std::atomic<uint32_t> s_status_release_headroom_late{0};
std::atomic<uint8_t> s_status_source_placement{0};
std::atomic<uint32_t> s_status_driver_limiter_interval_us{0};
}

static void PublishPacingStatus(const SchedulerPacingStatus& status) noexcept
{
    const uint64_t sequence = s_status_sequence.load(std::memory_order_relaxed);
    s_status_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    s_status_valid.store(status.valid, std::memory_order_relaxed);
    s_status_lead_us.store(status.lead_us, std::memory_order_relaxed);
    s_status_lead_warm.store(status.lead_warm, std::memory_order_relaxed);
    s_status_lead_samples.store(status.lead_samples, std::memory_order_relaxed);
    s_status_grid_epoch.store(status.grid_epoch, std::memory_order_relaxed);
    s_status_grid_index.store(status.grid_index, std::memory_order_relaxed);
    s_status_epoch_reason.store(status.epoch_reason, std::memory_order_relaxed);
    s_status_stage_events_dropped.store(status.stage_events_dropped, std::memory_order_relaxed);
    s_status_lead_budget_limited.store(status.lead_budget_limited, std::memory_order_relaxed);
    s_status_lead_quantile_us.store(status.lead_quantile_us, std::memory_order_relaxed);
    s_status_lead_floor_us.store(status.lead_floor_us, std::memory_order_relaxed);
    s_status_lead_floor_tail_us.store(status.lead_floor_tail_us, std::memory_order_relaxed);
    s_status_lead_floor_gpu_us.store(status.lead_floor_gpu_us, std::memory_order_relaxed);
    s_status_lead_floor_limited.store(status.lead_floor_limited, std::memory_order_relaxed);
    s_status_release_margin_us.store(status.release_margin_us, std::memory_order_relaxed);
    s_status_release_headroom_us.store(status.release_headroom_us, std::memory_order_relaxed);
    s_status_collision_headroom.store(status.collision_headroom, std::memory_order_relaxed);
    s_status_collision_limit_us.store(status.collision_limit_us, std::memory_order_relaxed);
    s_status_collision_checked.store(status.collision_checked, std::memory_order_relaxed);
    s_status_collision_guarded.store(status.collision_guarded, std::memory_order_relaxed);
    s_status_collision_behind_wait.store(status.collision_behind_wait, std::memory_order_relaxed);
    s_status_queue_order_pairs.store(status.queue_order_pairs, std::memory_order_relaxed);
    s_status_release_margin_samples.store(status.release_margin_samples, std::memory_order_relaxed);
    s_status_release_margin_late.store(status.release_margin_late, std::memory_order_relaxed);
    s_status_release_headroom_samples.store(status.release_headroom_samples, std::memory_order_relaxed);
    s_status_release_headroom_late.store(status.release_headroom_late, std::memory_order_relaxed);
    s_status_source_placement.store(status.source_placement, std::memory_order_relaxed);
    s_status_driver_limiter_interval_us.store(status.driver_limiter_interval_us, std::memory_order_relaxed);
    s_status_sequence.store(sequence + 2, std::memory_order_release);
}

bool Scheduler_ReadPacingStatus(SchedulerPacingStatus* status) noexcept
{
    if (status == nullptr) return false;
    *status = {};
    constexpr int kAttempts = 4;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        const uint64_t begin = s_status_sequence.load(std::memory_order_acquire);
        if (begin == 0) return false; // never published
        if ((begin & 1u) != 0) continue;
        SchedulerPacingStatus copy{};
        copy.valid = s_status_valid.load(std::memory_order_relaxed);
        copy.lead_us = s_status_lead_us.load(std::memory_order_relaxed);
        copy.lead_warm = s_status_lead_warm.load(std::memory_order_relaxed);
        copy.lead_samples = s_status_lead_samples.load(std::memory_order_relaxed);
        copy.grid_epoch = s_status_grid_epoch.load(std::memory_order_relaxed);
        copy.grid_index = s_status_grid_index.load(std::memory_order_relaxed);
        copy.epoch_reason = s_status_epoch_reason.load(std::memory_order_relaxed);
        copy.stage_events_dropped = s_status_stage_events_dropped.load(std::memory_order_relaxed);
        copy.lead_budget_limited = s_status_lead_budget_limited.load(std::memory_order_relaxed);
        copy.lead_quantile_us = s_status_lead_quantile_us.load(std::memory_order_relaxed);
        copy.lead_floor_us = s_status_lead_floor_us.load(std::memory_order_relaxed);
        copy.lead_floor_tail_us = s_status_lead_floor_tail_us.load(std::memory_order_relaxed);
        copy.lead_floor_gpu_us = s_status_lead_floor_gpu_us.load(std::memory_order_relaxed);
        copy.lead_floor_limited = s_status_lead_floor_limited.load(std::memory_order_relaxed);
        copy.release_margin_us = s_status_release_margin_us.load(std::memory_order_relaxed);
        copy.release_headroom_us = s_status_release_headroom_us.load(std::memory_order_relaxed);
        copy.collision_headroom = s_status_collision_headroom.load(std::memory_order_relaxed);
        copy.collision_limit_us = s_status_collision_limit_us.load(std::memory_order_relaxed);
        copy.collision_checked = s_status_collision_checked.load(std::memory_order_relaxed);
        copy.collision_guarded = s_status_collision_guarded.load(std::memory_order_relaxed);
        copy.collision_behind_wait = s_status_collision_behind_wait.load(std::memory_order_relaxed);
        copy.queue_order_pairs = s_status_queue_order_pairs.load(std::memory_order_relaxed);
        copy.release_margin_samples = s_status_release_margin_samples.load(std::memory_order_relaxed);
        copy.release_margin_late = s_status_release_margin_late.load(std::memory_order_relaxed);
        copy.release_headroom_samples = s_status_release_headroom_samples.load(std::memory_order_relaxed);
        copy.release_headroom_late = s_status_release_headroom_late.load(std::memory_order_relaxed);
        copy.source_placement = s_status_source_placement.load(std::memory_order_relaxed);
        copy.driver_limiter_interval_us = s_status_driver_limiter_interval_us.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s_status_sequence.load(std::memory_order_relaxed) != begin) continue;
        *status = copy;
        return true;
    }
    return false;
}

// ── Scheduler owner state ──

namespace {
// Production clock of the source stage: QPC and the existing two-phase sleep
// (waitable timer + hardware spin, adaptive wake guard).
class QpcSourceClock final : public SourceStageClock {
public:
    int64_t Now() noexcept override {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }
    bool WaitUntil(int64_t target_qpc) noexcept override { return TryOwnSleep(target_qpc); }
};

QpcSourceClock s_clock;
SourceStage s_source_stage; // ~125 KB: static storage only
SourceStageLifecycleTracker s_lifecycle_tracker;
SourceStagePeriodTracker s_period_tracker;
uint32_t s_stage_generation = 0;
bool s_stage_initialized = false;
PacingSourceMarker s_last_marker = PacingSourceMarker::Unknown;
bool s_marker_known = false;

// Scheduler owner: the timestamp argument of the OnMarker call in progress.
int64_t s_source_marker_qpc = 0;

bool s_suspended = false;
bool s_was_focused = true;
bool s_background_mode = false;
bool s_was_oled_care_active = false;
bool s_dmfg_state_initialized = false;
bool s_last_dmfg_active = false;

// Period regime (cap and raw FG divisor): a change of either resets the
// adaptation regime.
int s_last_target_fps = -1;
int s_last_fg_divisor_raw = 0;

// Collision-aware release: the guard of the newest judged lead last logged
// (-1 none, 0 margin, 1 headroom).
int s_logged_collision_guard = -1;
// The start term's state last logged (lead_controller.h): -1 none, 0 applies on the release headroom, 1 applies on
// the measured W (the numbers of LeadStartSource), 2 stands aside; 3 more for the FG input wait's output than for the
// GPU queue wait's. A change of output, or to or from standing aside, is always told; among the applying states of
// one output only the move from the headroom to the measured W is told: a fall back is silent, because a stretch where
// the GPU is the limit and almost nothing is held leaves the GPU queue wait's resume window stale for 64 plans at a
// time, and the state stays where it was.
int s_logged_start_limit = -1;
// The first lead sample that left out the game's simulation → render queue
// (lead_controller.h) has been logged. Once per process.
bool s_logged_lead_queue = false;
// The queue-order verdicts last logged (lead_controller.h): the next frame's
// GPU work runs behind the GPU queue wait (the collision guard stands aside)
// or behind the FG input wait (the lead samples leave out the frame's wait
// for the previous frame).
bool s_logged_behind_wait = false;
bool s_logged_fg_behind_wait = false;
// The same verdict for frames planned with present-first ordering (lead_controller.h): they are judged by
// their own evidence, so it is logged apart and the periods of the interleave do not log each other's flips.
bool s_logged_ordered_behind_wait = false;

// Unified owner (unified_pacer.h). The mailbox word is written by
// the Sleep hook outside the owner and consumed once by the owner at the marker. The Sleep
// hook's hard-limit statistics travel in a few slots indexed by the reservation token: each
// slot is a seqlock whose writer takes it with a try-acquire and only for a newer token (a
// retired Sleep cannot overwrite another frame's slot), and the marker uses a snapshot only
// when its token and sequence validate, else it discards it.
std::atomic<uint64_t> s_up_mailbox{0};
PacingPendingWarning s_up_owner_busy, s_up_duplicate_sleep;
constexpr uint32_t kUpStatsSlots = 4;
struct UnifiedSleepStats {
    std::atomic<uint32_t> sequence{0};  // odd while a writer owns the slot
    std::atomic<uint32_t> token{0};     // the reservation the slot describes (0 = none)
    std::atomic<uint32_t> limit_flags{0};
    std::atomic<int32_t> outstanding{-1};
    std::atomic<int64_t> limit_wait_ticks{0};
    std::atomic<int64_t> ready_age_ticks{-1}; // -1 = no completion yet
};
UnifiedSleepStats s_up_stats[kUpStatsSlots];
bool s_up_key = false;             // the key as ResolvePipeline last saw it
PacingTestMode s_up_test_mode = PacingTestMode::Off; // the effective test mode as the ownership transition last saw it
bool s_up_reconcile = false;       // OnMarker reconciles the Sleep-mode tuple: the key is on, or its toggle is not applied yet
bool s_up_idle_regime = false;     // the idle regime as OnMarker last saw it (key on)
double s_up_idle_interval_us = 0.0; // I_idle of the newest OnMarker (0 = not idle; key on)
bool s_up_law_feed_active = false;  // owner: the probe's feed is being drained for the queue law (unified_queue_law)
// Reflex Sleep on the presenting device: written by the Sleep hook (any
// thread), read by the owner for placement availability.
std::atomic<int64_t> s_last_reflex_sleep_qpc{0};
// The driver's Reflex Sleep (Scheduler_EndDriverSleep, Hook_Sleep): the call
// that returned for grid index s_driver_sleep_index, its start and return.
uint64_t s_driver_sleep_index = 0;
int64_t s_driver_sleep_start_qpc = 0;
int64_t s_driver_sleep_end_qpc = 0;

// LFC on the output interval (period_governor.h)
LfcHysteresis s_lfc{};

// DMFG pacing-control input/timestamps (the DMFG caps keep their relative sleeps).
int64_t s_prev_enforcement_ts = 0;
double  s_control_actual_frame_time_us = 0.0;
}

static PacingApi ToPacingApi(ActiveAPI api) noexcept
{
    switch (api) {
        case ActiveAPI::DX9: return PacingApi::DX9;
        case ActiveAPI::DX11: return PacingApi::DX11;
        case ActiveAPI::DX12: return PacingApi::DX12;
        case ActiveAPI::Vulkan: return PacingApi::Vulkan;
        case ActiveAPI::OpenGL: return PacingApi::OpenGL;
        default: return PacingApi::None;
    }
}

static PacingRoute ToPacingRoute(EnforcementPath path) noexcept
{
    switch (path) {
        case EnforcementPath::NvAPIMarkers: return PacingRoute::NvApiMarkers;
        case EnforcementPath::PCLMarkers: return PacingRoute::PclMarkers;
        case EnforcementPath::PresentBased: return PacingRoute::PresentBased;
        default: return PacingRoute::None;
    }
}

// Developer key unified_queue_law: the law's counters (UnifiedQueueLawSummary), logged ONCE per
// transition and cleared: the law switched off, the unified pacer switched off, a lifecycle reset
// of the stage, and the process shutdown (Scheduler_LogQueueLawSummary). Nothing with the key off
// (the counters stay zero) and nothing per frame. Owner thread.
static void LogQueueLawSummary(const char* reason) noexcept
{
    UnifiedPacer& pacer = s_source_stage.Unified();
    const UnifiedQueueLawSummary s = pacer.QueueLawSummary();
    pacer.ClearQueueLawSummary();
    if (s.frames_planned == 0 && s.records_accepted == 0 && s.records_retired == 0 &&
        s.grants_law == 0 && s.grants_report == 0)
        return;
    char credit_note[640];
    credit_note[0] = '\0';
    size_t credit_note_length = 0;
    if (s.holds_settled_orphan != 0)
        credit_note_length += static_cast<size_t>(std::snprintf(credit_note, sizeof(credit_note),
            "; %llu law-granted holds settled from the measured hold (their probe record would not come)",
            static_cast<unsigned long long>(s.holds_settled_orphan)));
    if (s.frames_no_hold_credit != 0 && credit_note_length < sizeof(credit_note))
        credit_note_length += static_cast<size_t>(std::snprintf(credit_note + credit_note_length,
            sizeof(credit_note) - credit_note_length,
            "; %llu frames with the law in use took the report-based credit and holds (render queue found by dependency)",
            static_cast<unsigned long long>(s.frames_no_hold_credit)));
    if (s.frames_not_carried != 0 && credit_note_length < sizeof(credit_note))
        credit_note_length += static_cast<size_t>(std::snprintf(credit_note + credit_note_length,
            sizeof(credit_note) - credit_note_length,
            "; %llu frames the report-based rule decided because the watched queue did not carry the frame",
            static_cast<unsigned long long>(s.frames_not_carried)));
    if (s.grants_saturated != 0 && credit_note_length < sizeof(credit_note))
        credit_note_length += static_cast<size_t>(std::snprintf(credit_note + credit_note_length,
            sizeof(credit_note) - credit_note_length,
            "; %llu frames released at once because the hold credit was at its floor",
            static_cast<unsigned long long>(s.grants_saturated)));
    if ((s.carry_carried != 0 || s.carry_not_carried != 0 || s.carry_undecided != 0) &&
        credit_note_length < sizeof(credit_note))
        std::snprintf(credit_note + credit_note_length, sizeof(credit_note) - credit_note_length,
            "; carry votes %llu carried / %llu not carried / %llu undecided (frames judged against their readiness)",
            static_cast<unsigned long long>(s.carry_carried), static_cast<unsigned long long>(s.carry_not_carried),
            static_cast<unsigned long long>(s.carry_undecided));
    LOG_INFO("Queue law summary (%s): planned %llu, law in use %llu; records %llu accepted (%llu valid, "
             "%llu late), %llu dropped from a retired session; hold grants %llu law / %llu report; holds "
             "debited %llu by record / %llu by the report rule, %llu never debited, %llu lost unmeasured%s",
             reason, static_cast<unsigned long long>(s.frames_planned),
             static_cast<unsigned long long>(s.frames_in_use),
             static_cast<unsigned long long>(s.records_accepted), static_cast<unsigned long long>(s.records_valid),
             static_cast<unsigned long long>(s.records_late), static_cast<unsigned long long>(s.records_retired),
             static_cast<unsigned long long>(s.grants_law), static_cast<unsigned long long>(s.grants_report),
             static_cast<unsigned long long>(s.holds_debited_record),
             static_cast<unsigned long long>(s.holds_debited_report),
             static_cast<unsigned long long>(s.holds_never_debited),
             static_cast<unsigned long long>(s.holds_lost_unmeasured), credit_note);
}

void Scheduler_LogQueueLawSummary() noexcept
{
    SchedulerOwnerGuard owner;
    if (!owner) return; // the owner is busy: no summary (never blocks)
    LogQueueLawSummary("process shutdown");
}

// Unified owner: empties the law and the Sleep hook's mailbox.
// Runs where the adaptation regime resets (cap, FG divisor, DMFG, OLED, focus, lifecycle),
// on suspension and on a regime change; one bool check with the key off.
static void ResetUnifiedLaw() noexcept
{
    if (!s_source_stage.UnifiedEnabled()) return;
    s_source_stage.ResetUnified(FrameTimeline_CurrentControllerGeneration());
    s_up_mailbox.store(0, std::memory_order_release);
    s_up_owner_busy.Clear();
    s_up_duplicate_sleep.Clear();
}

// Starts a fresh cadence-readout history. Reflex reports admitted before the
// reset describe the old regime: discard them.
static void ResetAdaptationRegime(const char* reason) noexcept
{
    DiscardReflexFrameTimeSamples();
    Scheduler_ResetCadenceReadout();
    s_lfc.Reset();
    ResetUnifiedLaw();
    LOG_INFO("Scheduler: cadence readout reset (%s)", reason);
}

// Logs each switch between the headroom guard and the Present-call margin
// (collision-aware release), each change of the GPU queue wait's and the FG
// input wait's queue-order verdict for the frame's FG divisor, and the first
// lead sample that left out the game's simulation → render queue. The GPU queue wait's verdict is the one of
// the frame's own regime: a frame planned with present-first ordering (lead_controller.h) is judged by the
// pairs of ordered frames and logged apart. The start term's verdict (applies on the measured service, applies
// on the fallback, stands aside) is logged on a change too, whichever of its periods the plan was made in.
static void ObserveCollisionGuard(double fg_divisor, bool ordered) noexcept
{
    const LeadController& lead = s_source_stage.Lead();
    const LeadStatistics statistics = lead.Statistics();
    if (!s_logged_lead_queue && statistics.queued_samples > 0) {
        LOG_INFO("Lead: a sample left out %.0f us of the game's simulation -> render queue "
                 "(sample %.0f us; the render thread was still inside the previous Present)",
                 statistics.last_queue_us, statistics.last_sample_us);
        s_logged_lead_queue = true;
    }
    const bool behind_wait = lead.NextGpuWorkBehindWait(PacingRung::GpuQueueWait, fg_divisor);
    if (ordered) {
        if (behind_wait != s_logged_ordered_behind_wait) {
            if (behind_wait) {
                LOG_INFO("Collision guard: stands aside for ordered frames (the render queue's next frame waits "
                         "for the held frame's Present; %zu waited releases checked, none with the next frame's "
                         "GPU work started before the release)", lead.OrderedConcurrencyPairs());
            } else {
                LOG_INFO("Collision guard: back for ordered frames (%zu of %zu waited releases had the next "
                         "frame's GPU work start before the release, newest %+.0f us, or Reflex reports stopped)",
                         lead.OrderedConcurrentPairs(), lead.OrderedConcurrencyPairs(),
                         statistics.last_ordered_concurrency_us);
            }
            s_logged_ordered_behind_wait = behind_wait;
        }
    } else if (behind_wait != s_logged_behind_wait) {
        if (behind_wait) {
            LOG_INFO("Collision guard: stands aside (in the newest %zu waited releases the next frame's GPU work, "
                     "submitting before the release, started only after it: it runs behind the wait; newest %+.0f us)",
                     lead.ConcurrencyPairs(PacingRung::GpuQueueWait), statistics.last_concurrency_us);
        } else {
            LOG_INFO("Collision guard: back (the next frame's GPU work started %.0f us before a waited release: "
                     "it runs beside the wait)", -statistics.last_concurrency_us);
        }
        s_logged_behind_wait = behind_wait;
    }
    const bool fg_behind_wait = lead.NextGpuWorkBehindWait(PacingRung::FgInputWait, fg_divisor);
    if (fg_behind_wait != s_logged_fg_behind_wait) {
        if (fg_behind_wait) {
            LOG_INFO("Lead: the next frame's GPU work runs behind the FG input wait (newest %zu waited releases; "
                     "newest %+.0f us after the release): lead samples leave out its wait for the previous frame",
                     lead.ConcurrencyPairs(PacingRung::FgInputWait), statistics.last_fg_concurrency_us);
        } else {
            LOG_INFO("Lead: FG input wait lead samples keep the measured readiness (the next frame's GPU work "
                     "ran beside the wait, Reflex reports stopped, or frame generation is off)");
        }
        s_logged_fg_behind_wait = fg_behind_wait;
    }
    if (statistics.start_checked_frames != 0) {
        // The state to tell: stands aside (2), or applies on the best source seen since the term began to apply, per
        // output (+3 on the FG input wait). The wording of an applies line names the source in use when it is said.
        const bool fg = statistics.last_start_rung == PacingRung::FgInputWait;
        const int base = fg ? 3 : 0;
        const int source = static_cast<int>(statistics.last_start_source);
        const int logged = s_logged_start_limit;
        const bool same_output = logged >= base && logged < base + 3;
        int start = base + 2;
        if (!statistics.last_start_aside) {
            start = base + source;
            if (same_output && logged != base + 2 && logged > start) start = logged;
        }
        if (start != s_logged_start_limit) {
            const LeadStartEvidence evidence = lead.StartEvidence(statistics.last_start_rung);
            const char* resume_name = fg ? "the game's Present return after the FG input release"
                                         : "the render queue's resume after the release";
            if (start == base + 2) {
                LOG_INFO("Start limit (%s): stands aside (%zu of the newest %zu telling pairs: the game's Present "
                         "ended before the held frame's release; %zu of the newest %zu frames: the game asked for a "
                         "frame before its previous Present ended%s)",
                         fg ? "FG input wait" : "GPU queue wait", evidence.free_pairs, evidence.telling_pairs,
                         evidence.untied_starts, evidence.frames,
                         fg && !evidence.turnaround_shown ? "; the game's turnaround is not shown yet" : "");
            } else if (start == base + 1) {
                LOG_INFO("Start limit (%s): applies, %.2f ms (two intervals less %s %.2f ms, measured, and the game's "
                         "turnaround %.2f ms)",
                         fg ? "FG input wait" : "GPU queue wait", statistics.last_start_limit_us / 1000.0,
                         resume_name, statistics.last_start_resume_us / 1000.0,
                         statistics.last_start_turnaround_us / 1000.0);
            } else {
                LOG_INFO("Start limit (%s): applies, %.2f ms (two intervals less the release headroom %.2f ms (%s not "
                         "measured yet), and the game's turnaround %.2f ms)",
                         fg ? "FG input wait" : "GPU queue wait", statistics.last_start_limit_us / 1000.0,
                         statistics.last_start_resume_us / 1000.0, resume_name,
                         statistics.last_start_turnaround_us / 1000.0);
            }
            s_logged_start_limit = start;
        }
    }
    if (statistics.collision_checked_frames == 0) return;
    const int guard = statistics.last_guard_headroom ? 1 : 0;
    if (guard == s_logged_collision_guard) return;
    LOG_INFO("Collision guard: %s",
             guard == 1 ? "release headroom to the next frame's heavy GPU work (Reflex GPU active time)"
                        : "release margin to the next Present call (no current Reflex GPU active time)");
    s_logged_collision_guard = guard;
}

// Controller lifecycle. The generation bumps only on API change,
// identity-domain (trigger/route) change and producer-epoch change here;
// Flush(FLUSH_ALL) bumps it for API change, full swapchain teardown and
// device loss. On a new generation the grid, the frame plans and the
// lead controller reset. An enforcement-marker change resets lead buckets only.
static void ApplyLifecycle(SchedulerTrigger trigger, PacingSourceMarker marker) noexcept
{
    SourceStageLifecycle lifecycle{};
    lifecycle.api = ToPacingApi(SwapMgr_GetActiveAPI());
    FrameTimelinePath timeline_path{};
    FrameIdentityDomain identity_domain{};
    if (GetMarkerTimelineContext(trigger, &timeline_path, &identity_domain)) {
        lifecycle.identity_domain = ToPacingIdentityDomain(identity_domain);
        lifecycle.producer_epoch = FrameTimeline_CurrentProducerEpoch(identity_domain);
    }
    if (s_lifecycle_tracker.Observe(lifecycle)) {
        const uint32_t generation = FrameTimeline_BeginControllerGeneration();
        LOG_INFO("Scheduler: lifecycle change (api=%d, domain=%d, producer_epoch=%u) -> "
                 "controller generation %u",
                 static_cast<int>(lifecycle.api),
                 static_cast<int>(lifecycle.identity_domain),
                 lifecycle.producer_epoch, generation);
    }

    const uint32_t generation = FrameTimeline_CurrentControllerGeneration();
    if (!s_stage_initialized || generation != s_stage_generation) {
        LogQueueLawSummary("lifecycle reset");
        s_source_stage.Reset(GetQPCFrequency());
        // The producer epoch or lifecycle restarted the frame ids: the stamps of the old ones go
        // (marker_stamps.h; a reset of an empty ring is one atomic add).
        MarkerStamps_Reset();
        s_period_tracker.Forget();
        s_driver_sleep_index = 0;
        ResetFeedbackAccumulators();
        ApplyFeedbackResetRequests();
        if (s_stage_initialized) {
            ResetAdaptationRegime("controller lifecycle");
        }
        s_stage_generation = generation;
        s_stage_initialized = true;
        LOG_INFO("Scheduler: controller generation %u, cadence grid, frame plans and lead reset",
                 generation);
    }

    if (s_marker_known && marker != s_last_marker) {
        s_source_stage.ResetBuckets();
        LOG_INFO("Scheduler: enforcement marker %d -> %d, lead buckets reset",
                 static_cast<int>(s_last_marker), static_cast<int>(marker));
    }
    s_last_marker = marker;
    s_marker_known = true;
}

// Resolves the pacing pipeline for this OnMarker and publishes it; logs once
// per change. Returns the published snapshot (generation assigned).
static PacingPipelineSnapshot ResolvePipeline(
    SchedulerTrigger trigger,
    const PacingPipelineConfig& config,
    PacingRuntimeState runtime) noexcept
{
    const uint32_t generation = FrameTimeline_CurrentControllerGeneration();
    PacingCapabilities capabilities{};
#ifdef _WIN64
    capabilities.x64 = true;
#endif
    capabilities.api = ToPacingApi(SwapMgr_GetActiveAPI());
    // The route follows the trigger that produced this frame's plan key: the
    // Present fallback is present-based even while the dispatcher still
    // reports its marker path, and PCL markers on a DXGI API are the PCL
    // route (PclFrame keys), not the NvAPI one the dispatcher reports.
    if (trigger == SchedulerTrigger::PresentFallback) capabilities.route = PacingRoute::PresentBased;
    else if (trigger == SchedulerTrigger::PclMarker) capabilities.route = PacingRoute::PclMarkers;
    else capabilities.route = ToPacingRoute(EnfDisp_GetActivePath());
    capabilities.native_route_qualified =
        PacingPipeline_HasCapability(PacingCapability::NativeRoute, generation);
    capabilities.gpu_queue_ready =
        PacingPipeline_HasCapability(PacingCapability::GpuQueue, generation);
    capabilities.fg_input_route_ready =
        PacingPipeline_HasCapability(PacingCapability::FgInputRoute, generation);
    capabilities.fg_separate_presenter =
        PacingPipeline_HasCapability(PacingCapability::FgSeparatePresenter, generation);

    // The unified owner's transition is applied by ApplyUnifiedKeyTransition at the
    // start of the invocation (OnMarker, before the stage events are drained), never here.

    const PacingPipelineSnapshot resolved =
        PacingPipeline_Resolve(config, capabilities, runtime);
    if (PacingPipeline_Publish(resolved)) {
        const PacingPipelineSnapshot published = PacingPipeline_Read();
        char route_diag[1400];
        NativeGate_DiagnosticsText(route_diag, sizeof(route_diag));
        LOG_INFO("Pacing pipeline %u: mode=%s placement=%s rung=%s reason=%s native_route_diag: %s",
                 published.generation,
                 PacingTimingMode_Text(published.mode),
                 PacingSourcePlacement_Text(published.placement),
                 PacingRung_Text(published.rung),
                 PacingFallbackReason_Text(published.reason),
                 route_diag);
        return published;
    }
    return PacingPipeline_Read();
}

// Developer key unified_queue_law: the probe's finished frames to the law, in arrival order,
// before the plan (the owner, from OnMarker's keyed branch and the Sleep hook's reservation,
// both under the owner guard: the feed's one consumer). The records drained here are the newest
// the probe had pushed before this call (each describes a frame one to two frames older than the
// frame about to be planned); the law decides from them, or falls back to the report-based rule
// when fewer than 12 of its newest 16 arrivals are valid or the newest valid one is older than
// 4 planned frames. Law off (the stage's option): one comparison, nothing drained. Entering or
// leaving the law, and every key transition, discards the ring so no stale record is applied.
static void DrainQueueLawFeed(bool law_on) noexcept
{
    if (law_on != s_up_law_feed_active) {
        GpuQueueLawFeed_Reset();
        s_up_law_feed_active = law_on;
    }
    if (!law_on) return;
    // The retirement watermark the probe published (its adopted queue was destroyed) first, on EVERY
    // active drain, never cached: RetireQueueSession is idempotent (an older or already-retired session
    // changes nothing), so after a law off/on or a stage reset the pacer is told again and a late
    // record of the retired session is dropped. The window is emptied, its outstanding grants settled,
    // and records of that session still in the ring are dropped by the pacer when popped below.
    const uint64_t retired = GpuQueueLawFeed_RetiredSession();
    if (retired != 0)
        s_source_stage.RetireQueueSession(retired);
    PacingGpuQueueRecord record;
    while (GpuQueueLawFeed_Pop(&record))
        s_source_stage.OnQueueRecord(record);
}

// Unified owner: the owner's transition to the key of THIS invocation's
// configuration snapshot. It runs once per transition, at the start of OnMarker, BEFORE the
// stage events are drained, so that one invocation (drain, resolve, plan) runs under
// ONE value of the key. The rule, for both directions: the events drained by an invocation
// are judged by the rules of that invocation's key, whichever branch planned their frames.
// Key off: a late output restarts the cadence as in 9746673 (OutputLate), no unified report
// is registered or flushed, and the law/lead/mailbox state has been reset. Key on: outputs
// are judged GPU-paced (no OutputLate restart: the grid follows the start), and used reports
// feed the lead. A toggle resets the law and the lead's buckets, and leaves a
// Sleep-mode epoch pending so the first reconcile applies
// the new key's tuple exactly.
static void ApplyUnifiedKeyTransition(const PacingPipelineConfig& config) noexcept
{
    const bool key_changed = config.unified != s_up_key;
    if (!key_changed && config.test_mode == s_up_test_mode) return;
    s_up_key = config.unified;
    s_up_law_feed_active = false; // the next keyed drain discards what the ring holds
    s_up_reconcile = true;
    if (key_changed && !s_up_key) LogQueueLawSummary("unified pacer off"); // s_up_key is the new value: the key went off
    s_source_stage.SetUnifiedEnabled(s_up_key, FrameTimeline_CurrentControllerGeneration());
    s_up_mailbox.store(0, std::memory_order_release);
    s_up_owner_busy.Clear();
    s_up_duplicate_sleep.Clear();
    // The waker is installed before the publication starts (frame_plan.cpp stays free of Windows).
    FramePlan_SetReadyWaker([](const void* address) noexcept { WakeByAddressAll(const_cast<void*>(address)); });
    g_up_ready_publish.store(s_up_key, std::memory_order_relaxed);
    s_source_stage.ResetBuckets();
    s_driver_sleep_index = 0;
    NvAPI_BumpSleepModeEpoch();
    if (key_changed) LOG_INFO("Unified pacer %s", s_up_key ? "on" : "off");
    if (config.test_mode != s_up_test_mode) {
        s_up_test_mode = config.test_mode;
        LOG_INFO("Pacing test mode %s", PacingTestMode_Text(s_up_test_mode));
    }
}


// One source record per observed OnMarker call, pushed after its release, just
// before it returns toward the game. The frame identity is read here.
static void PushSourceRelease(
    const SchedulerCadenceContext& context,
    PacingSourceRecord* record) noexcept
{
    const PacingPlanKey key = Scheduler_MakePlanKey(context.frame_id, context.trigger);
    record->key = PacingPlanKey_Frame(key);
    record->controller_generation = key.controller_generation;
    record->trigger = static_cast<uint8_t>(context.trigger);
    record->marker = context.marker;
    record->regime = static_cast<uint8_t>(context.regime);
    record->marker_qpc = s_source_marker_qpc;
    record->target_fps = context.target_fps;
    // The driver's sleep status as it stood when this frame's Reflex Sleep
    // returned (Hook_Sleep, presenting device): taken once, so a record
    // without a Sleep before it carries none.
    g_pacing_pending_sleep.Take(*record);
    PacingTelemetry_PushSource(*record);
}

static int64_t QpcNow() noexcept
{
    return s_clock.Now();
}

// Source placement and collision guard fields of the OSD status.
static void FillControllerStatus(SchedulerPacingStatus* status) noexcept
{
    const PacingPipelineSnapshot pipeline = PacingPipeline_Read();
    status->source_placement = static_cast<uint8_t>(pipeline.placement);
    status->driver_limiter_interval_us = ReflexDriverLimiter_AppliedUs();
    const LeadController& lead = s_source_stage.Lead();
    const LeadStatistics lead_statistics = lead.Statistics();
    const auto clip = [](uint64_t value) { return static_cast<uint32_t>(std::min<uint64_t>(value, UINT32_MAX)); };
    status->release_margin_us = LEAD_RELEASE_MARGIN_US;
    status->release_headroom_us = pipeline.gpu_wait_headroom_us;
    status->collision_headroom = lead_statistics.collision_checked_frames != 0 && lead_statistics.last_guard_headroom;
    status->collision_limit_us =
        lead_statistics.collision_checked_frames != 0 ? lead_statistics.last_collision_limit_us : 0.0;
    status->collision_checked = clip(lead_statistics.collision_checked_frames);
    status->collision_guarded = clip(lead_statistics.collision_guarded_frames);
    status->collision_behind_wait =
        lead.NextGpuWorkBehindWait(PacingRung::GpuQueueWait,
            s_last_fg_divisor_raw > 0 ? static_cast<double>(s_last_fg_divisor_raw) : 1.0);
    status->queue_order_pairs = static_cast<uint32_t>(lead.ConcurrencyPairs(PacingRung::GpuQueueWait));
    status->release_margin_samples = clip(lead_statistics.margin_samples);
    status->release_margin_late = clip(lead_statistics.margin_late_samples);
    status->release_headroom_samples = clip(lead_statistics.headroom_samples);
    status->release_headroom_late = clip(lead_statistics.headroom_late_samples);
}

static bool IsProcessFocused() noexcept
{
    HWND fg = FocusLock_RealGetForegroundWindow();
    DWORD fg_pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fg_pid);
    return fg_pid == GetCurrentProcessId();
}

// Mode Off: the grid is suspended (in-flight plans resolve Stale) and the
// source releases immediately. One passthrough source record.
static void RunPassthrough(
    const SchedulerCadenceContext& context,
    const PacingPipelineSnapshot& pipeline,
    uint8_t extra_flags,
    double fg_divisor) noexcept
{
    s_source_stage.Suspend();
    const int64_t release = QpcNow();
    s_prev_enforcement_ts = release;
    // No owned wait, as under the driver limiter: the frame's Reflex report
    // matches this release with nothing to subtract. Uncapped, that match is
    // what makes the Reflex AI frame time count as evidence that frame
    // generation presents (the deferred inference in compatibility mode, FrameGeneration_OnPresent).
    if (context.trigger == SchedulerTrigger::NvApiMarker && context.producer_epoch != 0 &&
        context.producer_epoch == FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame)) {
        PublishReflexOwnedSleep(context.frame_id, context.producer_epoch,
            static_cast<PacingSourceMarker>(context.marker), 0.0);
    }
    PublishCadence(context, release);
    if (PacingTelemetry_Active()) {
        PacingSourceRecord source{};
        source.flags = static_cast<uint8_t>(PACING_SOURCE_PASSTHROUGH | extra_flags);
        source.release_qpc = release;
        source.fg_divisor = fg_divisor;
        SourceStage::FillSourceRecord({}, pipeline, &source);
        PushSourceRelease(context, &source);
    }

    SchedulerPacingStatus status{};
    status.stage_events_dropped = FramePlan_DroppedStageEvents();
    FillControllerStatus(&status);
    PublishPacingStatus(status);
}

// ── DMFG (mode Off): passthrough or its internal caps, unchanged ──
static void RunDmfg(
    uint64_t frameID,
    SchedulerTrigger trigger,
    PacingSourceMarker marker,
    uint32_t nvapi_producer_epoch,
    const PacingPipelineSnapshot& pipeline) noexcept
{
    s_source_stage.Suspend();
    const PacingPlanKey key = Scheduler_MakePlanKey(frameID, trigger);

    double dmfg_feedback_interval_us = s_control_actual_frame_time_us;
    if (!(dmfg_feedback_interval_us > 0.0)) {
        const int target_fps = g_user_target_fps.load(std::memory_order_relaxed);
        dmfg_feedback_interval_us = target_fps > 0
            ? 1000000.0 / static_cast<double>(target_fps)
            : 16666.667;
    }
    PollReflexLatencyReport(dmfg_feedback_interval_us);

    // ── DMFG Background FPS cap ──
    if (!IsProcessFocused()) {
        const int bg_fps = g_background_fps.load(std::memory_order_relaxed);
        const SchedulerCadenceContext cadence_context = GetCadenceContext(
            key, trigger, marker, SchedulerCadenceRegime::DmfgBackground, bg_fps);
        double bg_interval_us = 0.0;
        int64_t wake_target = 0;
        bool slept = false;
        if (bg_fps > 0) {
            bg_interval_us = 1000000.0 / static_cast<double>(bg_fps);
            const int64_t now_qpc = QpcNow();
            if (s_prev_enforcement_ts > 0) {
                const double remaining = bg_interval_us - qpc_to_us(now_qpc - s_prev_enforcement_ts);
                if (remaining > 500.0) {
                    wake_target = now_qpc + us_to_qpc(remaining);
                    slept = TryOwnSleep(wake_target);
                }
            }
        }
        const int64_t release = QpcNow();
        s_prev_enforcement_ts = release;
        PublishCadence(cadence_context, release);
        if (!PacingTelemetry_Active()) return;
        PacingSourceRecord source{};
        source.interval_us = bg_interval_us;
        source.wake_target_qpc = wake_target;
        if (slept) source.flags |= PACING_SOURCE_SLEPT;
        source.flags |= PACING_SOURCE_BACKGROUND;
        if (bg_fps <= 0) source.flags |= PACING_SOURCE_PASSTHROUGH;
        source.release_qpc = release;
        SourceStage::FillSourceRecord({}, pipeline, &source);
        PushSourceRelease(cadence_context, &source);
        return;
    }

    // ── DMFG FG-Off FPS cap ──
    const int fg_off_cap = g_fg_off_fps.load(std::memory_order_relaxed);
    if (fg_off_cap > 0 && IsFGDllLoaded() && ComputeFGDivisorRaw() == 1) {
        const SchedulerCadenceContext cadence_context = GetCadenceContext(
            key, trigger, marker, SchedulerCadenceRegime::DmfgFgOffCap, fg_off_cap);
        const double cap_interval_us = 1000000.0 / static_cast<double>(fg_off_cap);
        int64_t wake_target = 0;
        bool slept = false;
        const int64_t now_qpc = QpcNow();
        int64_t own_sleep_start = 0;
        if (s_prev_enforcement_ts > 0) {
            const double remaining = cap_interval_us - qpc_to_us(now_qpc - s_prev_enforcement_ts);
            if (remaining > 500.0) {
                own_sleep_start = now_qpc;
                wake_target = now_qpc + us_to_qpc(remaining);
                slept = TryOwnSleep(wake_target);
            }
        }
        const int64_t release = QpcNow();
        if (own_sleep_start != 0 && nvapi_producer_epoch != 0 &&
            nvapi_producer_epoch == FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame)) {
            PublishReflexOwnedSleep(frameID, nvapi_producer_epoch, marker,
                qpc_to_us(release - own_sleep_start));
        }
        if (s_prev_enforcement_ts > 0) {
            const double frame_time_us = qpc_to_us(release - s_prev_enforcement_ts);
            if (frame_time_us > 0.0) s_control_actual_frame_time_us = frame_time_us;
        }
        s_prev_enforcement_ts = release;
        PublishCadence(cadence_context, release);
        if (!PacingTelemetry_Active()) return;
        PacingSourceRecord source{};
        source.interval_us = cap_interval_us;
        source.wake_target_qpc = wake_target;
        if (slept) source.flags |= PACING_SOURCE_SLEPT;
        source.release_qpc = release;
        SourceStage::FillSourceRecord({}, pipeline, &source);
        PushSourceRelease(cadence_context, &source);
        return;
    }

    // Dynamic DMFG: pure passthrough, no cap from ReLimiter. The user sets
    // the target via NVIDIA App / RHI / Profile Inspector.
    const SchedulerCadenceContext cadence_context = GetCadenceContext(
        key, trigger, marker, SchedulerCadenceRegime::Dmfg);
    const int64_t release = QpcNow();
    const double control_ft = s_prev_enforcement_ts > 0
        ? qpc_to_us(release - s_prev_enforcement_ts) : 0.0;
    s_prev_enforcement_ts = release;
    if (control_ft > 0.0) s_control_actual_frame_time_us = control_ft;
    const SchedulerCadenceObservation cadence = PublishCadence(cadence_context, release);
    if (!PacingTelemetry_Active()) return;
    const double telemetry_ft = qpc_to_us(cadence.interval_qpc);
    const double output_fps = g_output_fps.load(std::memory_order_relaxed);
    const double render_fps = cadence.valid ? 1000000.0 / telemetry_ft : 0.0;
    double real_fg_divisor = 1.0;
    if (output_fps > 1.0 && render_fps > 1.0) {
        const double ratio = output_fps / render_fps;
        if (ratio >= 1.5) real_fg_divisor = ratio;
    }
    PacingSourceRecord source{};
    source.flags = PACING_SOURCE_PASSTHROUGH;
    source.release_qpc = release;
    source.fg_divisor = real_fg_divisor;
    SourceStage::FillSourceRecord({}, pipeline, &source);
    PushSourceRelease(cadence_context, &source);
}

// ── Reflex report drain (VRR and fixed refresh) ──

// Consumes each delayed NvAPI Reflex report once and hands it to the stage's
// models: the unified law (under the key), the lead's GPU report and the
// report pair (lead_controller.h).
// Called once per planned frame, before its plan, at the two interval sites of
// OnMarker.
static void DrainReflexReports(const PacingPipelineConfig& config) noexcept
{
    ReflexFrameTimeSample sample{};
    while (ConsumeReflexFrameTimeSample(&sample)) {
        // Unified owner: the report's signed queue wait from the two
        // raw timestamps the sample carries (gpuRenderStart − osRenderQueueStart, NAN when missing).
        if (config.unified) {
            const double queue_wait_us = sample.gpu_start_driver_us != 0 && sample.os_queue_start_driver_us != 0
                ? static_cast<double>(static_cast<int64_t>(sample.gpu_start_driver_us - sample.os_queue_start_driver_us))
                : std::numeric_limits<double>::quiet_NaN();
            s_source_stage.OnUnifiedReport(PacingIdentityDomain::NvApiFrame, sample.producer_epoch,
                sample.frame_id, queue_wait_us, sample.gpu_active_us, sample.ai_frame_us);
        }
        // The release headroom's heavy-work start r − A, the queue-order
        // test's GPU start and end and the lead sample behind the wait (lead_controller.h).
        if (sample.gpu_active_us > 0.0) {
            LeadGpuReport report{};
            report.active_us = sample.gpu_active_us;
            report.span_us = sample.gpu_render_duration_us;
            report.start_qpc = sample.gpu_start_qpc;
            report.end_qpc = sample.gpu_end_qpc;
            report.first_work_streamed = sample.first_work_streamed;
            report.first_submission_delay_us = sample.first_submission_delay_us;
            s_source_stage.OnGpuReport(sample.producer_epoch, sample.frame_id, report);
        }
        // The first-submission delay window of the lead sample behind the wait (lead_controller.h).
        if (sample.back_pressure_known) {
            s_source_stage.OnReportPair(sample.producer_epoch, sample.frame_id, sample.back_pressure);
        }
    }
}

// The interval a Reflex report poll clamps its stage samples against: the
// effective interval, else the cap's, else 60 FPS.
static double ReportFeedbackIntervalUs() noexcept
{
    const double effective_us = g_effective_interval_us.load(std::memory_order_relaxed);
    if (effective_us > 0.0) return effective_us;
    return PacingPipeline_ReportFeedbackFallbackUs(
        PacingPipeline_TestMode(), g_user_target_fps.load(std::memory_order_relaxed));
}

// VRR cap interval before LFC (I_0 = cap × FG divisor).
static double VrrCapIntervalUs(int target_fps, double fg_divisor) noexcept
{
    return (1000000.0 / static_cast<double>(target_fps)) * fg_divisor;
}

// LFC hysteresis on the cap interval I_0, judged on the output interval I_0 / D.
static double ApplyLfc(double interval, double fg_divisor) noexcept
{
    return s_lfc.Apply(interval, fg_divisor,
        g_floor_interval_us.load(std::memory_order_relaxed),
        g_ceiling_interval_us.load(std::memory_order_relaxed));
}

// The LFC quantum (D VRR ceiling intervals) while LFC is active, else 0. Read
// after ApplyLfc, it is the current frame's LFC state.
static double LfcQuantumUs(double fg_divisor) noexcept
{
    return s_lfc.QuantumUs(fg_divisor, g_ceiling_interval_us.load(std::memory_order_relaxed));
}

// Period identity for PeriodChange epochs: the VRR cap interval from the raw
// FG divisor with the LFC state of the grid interval. The ramped divisor moves
// the grid interval for 50 ms after a toggle; those frames are silent rebases.
// Only a change of cap, raw divisor or LFC state/multiple is a period change.
// Call after ApplyLfc so the LFC state is current.
static double VrrPeriodIdentityUs(int target_fps, int fg_divisor_raw) noexcept
{
    const double divisor = static_cast<double>(fg_divisor_raw > 0 ? fg_divisor_raw : 1);
    const double interval = (1000000.0 / static_cast<double>(target_fps)) * divisor;
    return PeriodGovernor_LfcIntervalUs(interval, LfcQuantumUs(divisor));
}

static double ComputeFixedInterval(double fg_divisor) noexcept
{
    const int target_fps = g_user_target_fps.load(std::memory_order_relaxed);
    if (target_fps > 0) return (1000000.0 / static_cast<double>(target_fps)) * fg_divisor;
    // target_fps=0: pace at the refresh interval (Fixed mode shouldn't be
    // active without a limit, but handle it gracefully).
    return g_ceiling_interval_us.load(std::memory_order_relaxed) * fg_divisor;
}

struct PlannedFrameInput {
    uint64_t frame_id = 0;
    SchedulerTrigger trigger = SchedulerTrigger::NvApiMarker;
    PacingSourceMarker marker = PacingSourceMarker::Unknown;
    SchedulerCadenceRegime regime = SchedulerCadenceRegime::Vrr;
    int target_fps = 0;
    double base_interval_us = 0.0; // I_0: cap × FG divisor, before LFC
    double period_identity_us = 0.0; // raw cap, divisor and LFC; 0 = base_interval_us
    double interval_us = 0.0;      // grid interval
    double fg_divisor = 1.0;
    uint8_t extra_flags = 0;
    CadenceGridFixedReference fixed{}; // Fixed mode: the vblank reference
};

static bool PreSleepAvailable() noexcept;

// The driver's Reflex Sleep runs for this frame: the game called it within
// the last second and the bypass does not apply. Planned frames are paced by
// ReLimiter, so with a cap the bypass applies whenever it is on.
static bool DriverSleepRuns(int target_fps) noexcept
{
    // The unified owner never bypasses the Sleep.
    if (PacingPipeline_Unified()) return PreSleepAvailable();
    return PreSleepAvailable() &&
        !NvAPI_ShouldBypassSleep(PacingPipeline_SleepBypass(), target_fps, true);
}

// Unified owner: the interval terms of I(n) (unified_pacer.h) from the cap,
// the raw frame generation factor N (never the ramped divisor), the VRR ceiling, the game's
// own limit and the idle regime. Owner only. The VRR cap with the cap off is the Reflex VRR
// cap; a fixed refresh with the cap off has no cap term (passthrough upstream).
static UnifiedIntervalTerms BuildUnifiedTerms(PacingTimingMode mode, double idle_interval_us) noexcept
{
    UnifiedIntervalTerms terms{};
    const int divisor = g_resolved_fg_divisor.load(std::memory_order_relaxed);
    const double n = static_cast<double>(divisor > 0 ? divisor : 1);
    terms.idle = mode == PacingTimingMode::Idle;
    if (terms.idle) {
        terms.idle_us = idle_interval_us;
        terms.cap_us = idle_interval_us; // I_idle replaces I_cap
    } else {
        int cap_fps = g_user_target_fps.load(std::memory_order_relaxed);
        const double vrr_fps = UnifiedPacer_VrrCapFps(g_ceiling_hz.load(std::memory_order_relaxed));
        if (cap_fps <= 0 && mode == PacingTimingMode::Vrr) cap_fps = static_cast<int>(vrr_fps);
        if (cap_fps > 0) terms.cap_us = n * 1'000'000.0 / static_cast<double>(cap_fps);
        if (mode == PacingTimingMode::Vrr && vrr_fps > 0.0) terms.vrr_us = n * 1'000'000.0 / vrr_fps;
    }
    // The game's own limit stays in every regime (Idle included): I = max(I_idle, I_game, ...).
    const uint32_t game_us = g_game_requested_interval.load(std::memory_order_relaxed);
    if (game_us > 0) terms.game_us = n * static_cast<double>(game_us);
    return terms;
}

// 30-bit token order (UnifiedMailbox_Pack masks tokens to 30 bits): a is newer than b.
static bool UpTokenNewer(uint32_t a, uint32_t b) noexcept
{
    return static_cast<int32_t>((a - b) << 2) > 0;
}

// Sleep hook, no owner: publishes the hard-limit statistics of reservation `token` into its
// slot. The slot is taken with a try-acquire, and only when it holds an older token; a
// contended or newer slot keeps what it has and the marker discards the unvalidated snapshot.
static void PublishUnifiedStats(uint32_t token, const UnifiedLimitResult& limit) noexcept
{
    UnifiedSleepStats& slot = s_up_stats[token % kUpStatsSlots];
    uint32_t sequence = slot.sequence.load(std::memory_order_acquire);
    if ((sequence & 1u) != 0) return;
    const uint32_t held = slot.token.load(std::memory_order_relaxed);
    if (held != 0 && !UpTokenNewer(token, held)) return;
    if (!slot.sequence.compare_exchange_strong(sequence, sequence + 1, std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return; // the sequence moved after the token was read: another writer owns or owned the slot
    }
    std::atomic_thread_fence(std::memory_order_release); // the odd sequence is visible before any field below
    slot.token.store(token, std::memory_order_relaxed);
    slot.limit_flags.store(limit.flags, std::memory_order_relaxed);
    slot.outstanding.store(limit.outstanding, std::memory_order_relaxed);
    slot.limit_wait_ticks.store(limit.wait_ticks, std::memory_order_relaxed);
    slot.ready_age_ticks.store(limit.ready_age_ticks, std::memory_order_relaxed);
    slot.sequence.store(sequence + 2, std::memory_order_release);
}

struct UnifiedStatsSnapshot {
    uint32_t limit_flags = 0;
    int32_t outstanding = -1;
    int64_t limit_wait_ticks = 0;
    int64_t ready_age_ticks = -1;
};

// Owner: the statistics of reservation `token`, true only for one coherent snapshot of that
// token (an absent, replaced or concurrently written slot is false and nothing is used).
static bool ReadUnifiedStats(uint32_t token, UnifiedStatsSnapshot* out) noexcept
{
    const UnifiedSleepStats& slot = s_up_stats[token % kUpStatsSlots];
    const uint32_t before = slot.sequence.load(std::memory_order_acquire);
    if ((before & 1u) != 0 || slot.token.load(std::memory_order_relaxed) != token) return false;
    out->limit_flags = slot.limit_flags.load(std::memory_order_relaxed);
    out->outstanding = slot.outstanding.load(std::memory_order_relaxed);
    out->limit_wait_ticks = slot.limit_wait_ticks.load(std::memory_order_relaxed);
    out->ready_age_ticks = slot.ready_age_ticks.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    return slot.sequence.load(std::memory_order_relaxed) == before;
}

// The unified columns of the frame's source record; empty cells stay unset.
static void FillUnifiedRecord(const SourceStageFrame& frame, PacingSourceRecord* record) noexcept
{
    if (!frame.planned || !frame.unified.valid) return;
    const SourceStageUnifiedFrame& unified = frame.unified;
    const UnifiedStart& start = unified.start;
    const auto birth = static_cast<int64_t>(g_pacing_telemetry_demand.load(std::memory_order_relaxed) >> kPacingWindowShift);
    uint32_t flags = unified.flags;
    if (s_up_owner_busy.Take(birth, record->release_qpc)) flags |= kUpOwnerBusy;
    if (s_up_duplicate_sleep.Take(birth, record->release_qpc)) flags |= kUpDuplicateSleep;
    // The Sleep hook's hard-limit statistics, when this frame consumed that Sleep's word and the
    // snapshot of its token validates; otherwise the cells stay empty.
    UnifiedStatsSnapshot stats{};
    if (start.token != 0 &&
        (unified.kind == UnifiedResolutionKind::Consumed || unified.kind == UnifiedResolutionKind::WaitFailed) &&
        ReadUnifiedStats(start.token, &stats)) {
        flags |= stats.limit_flags;
        record->up_outstanding = stats.outstanding;
        record->up_limit_wait_us = qpc_to_us(stats.limit_wait_ticks);
        if (stats.ready_age_ticks >= 0) {
            record->up_ready_age_valid = 1;
            record->up_ready_age_us = qpc_to_us(stats.ready_age_ticks);
        }
    }
    if (g_unified_driver_pacing.load(std::memory_order_relaxed)) flags |= kUpDriverPaces;
    record->up_valid = 1;
    record->up_grid_target_qpc = start.target.base;
    record->up_interval_us = start.interval_us;
    if (start.gpu_interval_us > 0.0) {
        record->up_gpu_interval_valid = 1;
        record->up_gpu_interval_us = start.gpu_interval_us;
    }
    if (unified.status.service_valid) {
        record->up_gpu_service_valid = 1;
        record->up_gpu_service_us = unified.status.service_us;
    }
    if (unified.status.queue_wait_valid) {
        record->up_queue_wait_valid = 1;
        record->up_queue_wait_raw_us = unified.status.queue_wait_raw_us;
        record->up_queue_wait_us = unified.status.queue_wait_us;
    }
    record->up_flags = flags;
    if (unified.hold_live) { // the signed credit at the plan; not live leaves the cell empty
        record->up_hold_budget_valid = 1;
        record->up_hold_budget_us = unified.hold_credit_us;
    }
    // The newest used report's service sample and GPU wait, as the law computed them at this plan.
    if (unified.status.service_sample_valid) {
        record->up_service_sample_valid = 1;
        record->up_service_sample_us = unified.status.service_sample_us;
    }
    if (unified.status.gpu_wait_valid) {
        record->up_gpu_wait_valid = 1;
        record->up_gpu_wait_us = unified.status.gpu_wait_us;
    }
    if (unified.status.cycle_valid) { // the measured completion spacing C at this plan (hold share on)
        record->up_cycle_valid = 1;
        record->up_cycle_us = unified.status.cycle_us;
    }
    // The queue law (key unified_queue_law): its statistics at this plan, the number of valid
    // frames among the newest 16 arrivals whenever the key is on (-1 = key off).
    record->up_ql_valid_frames = unified.status.ql_valid_frames;
    if (unified.status.ql_valid) {
        record->up_ql_valid = 1;
        record->up_ql_base_us = unified.status.ql_base_us;
        record->up_ql_need_us = unified.status.ql_need_us;
        record->up_ql_service_us = unified.status.ql_service_us;
        record->up_ql_q_us = unified.status.ql_q_us;
        record->up_ql_p_us = unified.status.ql_p_us;
    }
}

// Lead spike floor transitions (lead_controller.h), default log: "on" when the
// floor first sets the newest plan's lead target, "off" once it has not for
// LEAD_WINDOW_CAPACITY plans, so a floor near the own lead does not repeat.
static bool s_lead_floor_logged = false;
static size_t s_lead_floor_idle_plans = 0;

static void NoteLeadSpikeFloor(const SourceStageFrame& frame, const LeadBucketStatus& bucket,
    double allowance_us) noexcept
{
    if (!frame.planned) return;
    const double own_us = bucket.quantile_us + allowance_us;
    if (bucket.spike_floor_limited) {
        s_lead_floor_idle_plans = 0;
        if (s_lead_floor_logged) return;
        s_lead_floor_logged = true;
        const LeadController& lead = s_source_stage.Lead();
        LOG_INFO("Lead spike floor: on at %.1f ms (arrival tail %.1f ms + GPU %.1f ms; own lead %.1f ms), %s FG %gx",
            bucket.spike_floor_us / 1000.0, lead.SpikeTailUs() / 1000.0, lead.SpikeGpuUs() / 1000.0,
            own_us / 1000.0, PacingRung_Text(frame.plan.rung), frame.plan.fg_divisor);
        return;
    }
    if (!s_lead_floor_logged || ++s_lead_floor_idle_plans < LEAD_WINDOW_CAPACITY) return;
    s_lead_floor_logged = false;
    s_lead_floor_idle_plans = 0;
    LOG_INFO("Lead spike floor: off (own lead %.1f ms, floor %.1f ms)", own_us / 1000.0,
        bucket.spike_floor_us / 1000.0);
}

// Plans, publishes, waits, records. Exactly one source record.
static SourceStageFrame RunPlanned(
    const PlannedFrameInput& input,
    const PacingPipelineSnapshot& pipeline,
    const PacingPipelineConfig& config) noexcept
{
    const PacingPlanKey key = Scheduler_MakePlanKey(input.frame_id, input.trigger);
    const SchedulerCadenceContext cadence_context = GetCadenceContext(
        key, input.trigger, input.marker, input.regime, input.target_fps);

    SourceStageRequest request{};
    request.key = key;
    request.marker = input.marker;
    request.pipeline = pipeline;
    request.qpc_frequency = GetQPCFrequency();
    request.interval_us = input.interval_us;
    request.fg_divisor = input.fg_divisor;
    request.period_change = s_period_tracker.Observe(pipeline.mode,
        input.period_identity_us > 0.0 ? input.period_identity_us : input.base_interval_us);
    if (pipeline.mode == PacingTimingMode::Fixed) request.fixed = input.fixed;
    // Collision-aware release.
    request.collision_guard = true;
    // Unified owner, every planned frame of the marker route (placement Unified,
    // or the Marker fallback while the game calls no Reflex Sleep; passthrough, the present
    // route and mode Off excluded): the unified branch of the source stage plans it.
    const bool unified = SourceStage_UnifiedPlans(pipeline, config.unified, s_source_stage.UnifiedEnabled());
    // Lead spike floor (lead_controller.h).
    request.spike_floor = kPacingLeadSpikeFloor;
    // Developer key lead_queue_correction (lead_controller.h): read once per plan, kept by the lead with the frame.
    request.queue_correction = g_lead_queue_correction.load(std::memory_order_relaxed);
    // Developer A/B switches of the collision guard (lead_controller.h): the late state (gpu_wait_late_guard), the
    // start limit (gpu_wait_start_limit) and present-first ordering (gpu_wait_present_first; gpu_hold_order.h), with
    // the interleave key gpu_wait_ab_interleave_s (the periods and what each period does: SourceStage_Switches,
    // source_stage.h). A plan is ordered only while the unit says it can order (g_gpu_hold_order_able), so the queue
    // hooks order a frame exactly when its plan said so. The clock is read only when a key and a period call for it.
    const bool late_key = g_gpu_wait_late_guard.load(std::memory_order_relaxed);
    const bool start_key = g_gpu_wait_start_limit.load(std::memory_order_relaxed);
    const bool ordered_key = g_gpu_wait_present_first.load(std::memory_order_relaxed);
    int64_t period_qpc = 0;
    bool odd_period = false;
    if (late_key || start_key || ordered_key) {
        period_qpc = static_cast<int64_t>(g_gpu_wait_ab_interleave_s.load(
            std::memory_order_relaxed)) * request.qpc_frequency;
        if (period_qpc > 0) odd_period = ((QpcNow() / period_qpc) & 1) != 0;
    }
    const SourceStageSwitches switches = SourceStage_Switches(late_key, start_key, ordered_key,
        period_qpc > 0, odd_period);
    request.late_guard = switches.late_guard;
    request.late_observe = switches.late_observe;
    request.start_limit = switches.start_limit;
    request.start_observe = switches.start_observe;
    request.ordered = switches.ordered && g_gpu_hold_order_able.load(std::memory_order_relaxed);
    // The driver's Reflex Sleep for the index this plan is expected to take,
    // while it runs (the bypass off). The source stage records it for every
    // such frame (source_stage.h).
    if (DriverSleepRuns(input.target_fps)) {
        request.driver_sleep_index = s_driver_sleep_index;
        request.driver_sleep_start_qpc = s_driver_sleep_start_qpc;
        request.driver_sleep_end_qpc = s_driver_sleep_end_qpc;
    }

    if (unified) {
        request.unified.active = true;
        request.unified.terms = BuildUnifiedTerms(pipeline.mode, s_up_idle_interval_us);
        const int raw_divisor = g_resolved_fg_divisor.load(std::memory_order_relaxed);
        request.unified.raw_fg_divisor = raw_divisor > 0 ? raw_divisor : 1;
        request.unified.mailbox = &s_up_mailbox;
    }
    g_effective_interval_us.store(input.interval_us, std::memory_order_relaxed);
    const SourceStageFrame frame = s_source_stage.RunPlannedFrame(request, &s_clock);
    if (unified && frame.planned) g_effective_interval_us.store(frame.plan.interval_us, std::memory_order_relaxed);
    const int64_t release = frame.planned ? frame.release_qpc : QpcNow();

    // A consumed Sleep-hook start wait is the pre-Sleep wait of the record.
    int64_t pre_sleep_end_qpc = 0;
    bool pre_slept = false;
    if (frame.planned && unified && frame.pre_wait_end_qpc != 0) {
        pre_slept = true;
        pre_sleep_end_qpc = frame.pre_wait_end_qpc;
    }

    // Owned source wait for Reflex feedback (NvAPI source frames only).
    if (input.trigger == SchedulerTrigger::NvApiMarker && key.producer_epoch != 0 &&
        key.producer_epoch == FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame)) {
        const double own_sleep_us = frame.slept && frame.wait_end_qpc > frame.wait_start_qpc
            ? qpc_to_us(frame.wait_end_qpc - frame.wait_start_qpc) : 0.0;
        PublishReflexOwnedSleep(input.frame_id, key.producer_epoch, input.marker, own_sleep_us);
        if (frame.planned) {
            ReflexOwnedWaits waits{};
            waits.release_qpc = frame.release_qpc;
            waits.wait_start_qpc = frame.wait_start_qpc;
            waits.wait_end_qpc = frame.wait_end_qpc;
            waits.pre_wait_start_qpc = frame.pre_wait_start_qpc;
            waits.pre_wait_end_qpc = frame.pre_wait_end_qpc;
            waits.arrival_qpc = frame.game_arrival_qpc;
            PublishReflexOwnedWaits(input.frame_id, key.producer_epoch, waits);
        }
    }

    if (s_prev_enforcement_ts > 0) {
        const double control_ft = qpc_to_us(release - s_prev_enforcement_ts);
        if (control_ft > 0.0) s_control_actual_frame_time_us = control_ft;
    }
    s_prev_enforcement_ts = release;
    PublishCadence(cadence_context, release);

    ObserveCollisionGuard(unified && frame.planned ? frame.plan.fg_divisor : input.fg_divisor,
        frame.planned && frame.plan.ordered);

    if (PacingTelemetry_Active()) {
        PacingSourceRecord source{};
        source.flags = input.extra_flags;
        if (!frame.planned) source.flags |= PACING_SOURCE_PASSTHROUGH;
        source.release_qpc = release;
        source.interval_us = input.interval_us;
        source.fg_divisor = input.fg_divisor;
        if (pre_slept) {
            source.flags |= PACING_SOURCE_PRE_SLEEP;
            source.pre_sleep_end_qpc = pre_sleep_end_qpc;
        }
        SourceStage::FillSourceRecord(frame, pipeline, &source);
        if (unified) FillUnifiedRecord(frame, &source);
        PushSourceRelease(cadence_context, &source);
    }

    SchedulerPacingStatus status{};
    status.valid = frame.planned;
    status.lead_budget_limited = frame.lead_budget_limited;
    status.lead_us = frame.plan.lead_us;
    const LeadBucketStatus bucket =
        s_source_stage.Lead().BucketStatus(frame.plan.rung, frame.plan.fg_divisor);
    status.lead_warm = bucket.warm;
    status.lead_samples = bucket.samples;
    status.lead_quantile_us = bucket.quantile_us;
    status.lead_floor_us = bucket.spike_floor_us;
    status.lead_floor_tail_us = s_source_stage.Lead().SpikeTailUs();
    status.lead_floor_gpu_us = s_source_stage.Lead().SpikeGpuUs();
    status.lead_floor_limited = bucket.spike_floor_limited;
    NoteLeadSpikeFloor(frame, bucket, pipeline.allowance_us);
    status.grid_epoch = frame.plan.grid_epoch;
    status.grid_index = frame.plan.grid_index;
    status.epoch_reason = static_cast<uint8_t>(frame.grid_epoch_reason);
    status.stage_events_dropped = FramePlan_DroppedStageEvents();
    FillControllerStatus(&status);
    PublishPacingStatus(status);
    return frame;
}

// Driver-limiter frame: the Reflex driver limiter paces the source,
// so ReLimiter plans nothing (grid suspended, no wait, no hold). The
// interval is what the limiter applies. One source record.
static void RunDriverLimited(
    const PlannedFrameInput& input,
    const PacingPipelineSnapshot& pipeline) noexcept
{
    s_source_stage.Suspend();
    const PacingPlanKey key = Scheduler_MakePlanKey(input.frame_id, input.trigger);
    const SchedulerCadenceContext context = GetCadenceContext(
        key, input.trigger, input.marker, input.regime, input.target_fps);
    const int64_t release = QpcNow();
    if (s_prev_enforcement_ts > 0) {
        const double control_ft = qpc_to_us(release - s_prev_enforcement_ts);
        if (control_ft > 0.0) s_control_actual_frame_time_us = control_ft;
    }
    s_prev_enforcement_ts = release;
    g_effective_interval_us.store(input.interval_us, std::memory_order_relaxed);
    // No owned wait: the Reflex workload demand keeps its CPU stages.
    if (input.trigger == SchedulerTrigger::NvApiMarker && key.producer_epoch != 0 &&
        key.producer_epoch == FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame)) {
        PublishReflexOwnedSleep(input.frame_id, key.producer_epoch, input.marker, 0.0);
    }
    PublishCadence(context, release);
    if (PacingTelemetry_Active()) {
        PacingSourceRecord source{};
        source.flags = static_cast<uint8_t>(PACING_SOURCE_PASSTHROUGH | input.extra_flags);
        source.release_qpc = release;
        source.interval_us = input.interval_us;
        source.fg_divisor = input.fg_divisor;
        SourceStage::FillSourceRecord({}, pipeline, &source);
        PushSourceRelease(context, &source);
    }

    SchedulerPacingStatus status{};
    status.stage_events_dropped = FramePlan_DroppedStageEvents();
    FillControllerStatus(&status);
    PublishPacingStatus(status);
}

// The driver limiter is applied before the frame runs; when it cannot
// be applied (no game parameters, device mismatch, driver failure, Sleep
// bypass) the frame is planned at the marker instead, so a failed limiter
// never leaves the game unpaced.
// Under FG 2× the driver applies minimumIntervalUs to
// presented frames, generated ones included, so the limit is the
// real-frame interval divided by the FG divisor.
static double DriverLimiterIntervalUs(const PlannedFrameInput& input) noexcept
{
    return input.fg_divisor > 1.0 ? input.interval_us / input.fg_divisor : input.interval_us;
}

static bool ApplyDriverLimiter(double interval_us) noexcept
{
    ReflexDriverLimiter_Update(interval_us, true);
    return ReflexDriverLimiter_AppliedUs() != 0;
}

// ── Reflex Sleep hook: activity and the driver's Sleep ──

void Scheduler_RecordReflexSleep(int64_t now_qpc) noexcept
{
    s_last_reflex_sleep_qpc.store(now_qpc, std::memory_order_relaxed);
}

// Owner: the game called the Reflex Sleep within the last second.
static bool PreSleepAvailable() noexcept
{
    const int64_t last_sleep = s_last_reflex_sleep_qpc.load(std::memory_order_relaxed);
    return last_sleep > 0 &&
        PacingQpc_ElapsedSeconds(last_sleep, QpcNow(), GetQPCFrequency()) < PRE_SLEEP_ACTIVITY_S;
}

bool Scheduler_PacesSource() noexcept
{
    // Unified (unified owner) paces the source at the Sleep hook; the placement can still be
    // published as Unified for the first Sleep after a key-off, before ResolvePipeline republishes.
    const PacingSourcePlacement placement = PacingPipeline_Read().placement;
    return placement == PacingSourcePlacement::Marker ||
        placement == PacingSourcePlacement::Unified ||
        placement == PacingSourcePlacement::Present;
}

// The driver's Sleep for the next grid index returned (Hook_Sleep, presenting
// device, bypass off). The index is the one the next plan takes: the grid's
// newest planned index plus one.
void Scheduler_EndDriverSleep(int64_t start_qpc, int64_t end_qpc) noexcept
{
    s_driver_sleep_index = s_source_stage.Grid().Snapshot().last_planned_index + 1;
    s_driver_sleep_start_qpc = start_qpc;
    s_driver_sleep_end_qpc = end_qpc;
}

// ── Unified pacer: the Sleep hook's side ──

bool Scheduler_UnifiedActive() noexcept
{
    const PacingPipelineSnapshot pipeline = PacingPipeline_Read();
    return pipeline.placement == PacingSourcePlacement::Unified &&
        (pipeline.mode == PacingTimingMode::Vrr || pipeline.mode == PacingTimingMode::Fixed ||
         pipeline.mode == PacingTimingMode::Idle);
}

SchedulerUnifiedReserve Scheduler_UnifiedReserve(int64_t arrival_qpc, SchedulerUnifiedReservation* out) noexcept
{
    if (out == nullptr || !Scheduler_UnifiedActive()) return SchedulerUnifiedReserve::Inactive;
    SchedulerOwnerGuard owner;
    if (!owner) {
        if (PacingTelemetry_Active()) s_up_owner_busy.Record(arrival_qpc);
        return SchedulerUnifiedReserve::Busy;
    }
    // The reservation's I_gpu comes from the records drained now, including those the probe
    // finished after the previous marker's drain.
    DrainQueueLawFeed(s_source_stage.Unified().Options().queue_law);
    if (!s_source_stage.UnifiedEnabled() || !s_stage_initialized) return SchedulerUnifiedReserve::Inactive;
    const PacingPipelineSnapshot pipeline = PacingPipeline_Read();
    UnifiedStart start{};
    const UnifiedReserveStatus status = s_source_stage.Unified().Reserve(
        s_source_stage.Grid().NextIndex(), FrameTimeline_CurrentControllerGeneration(), arrival_qpc,
        BuildUnifiedTerms(pipeline.mode, s_up_idle_interval_us), &start);
    if (status == UnifiedReserveStatus::Duplicate) {
        if (PacingTelemetry_Active()) s_up_duplicate_sleep.Record(arrival_qpc);
        return SchedulerUnifiedReserve::Duplicate;
    }
    if (status != UnifiedReserveStatus::Reserved) return SchedulerUnifiedReserve::Inactive;
    // A reset inside the law (generation change, 250 ms gap) clears the mailbox word too.
    if ((start.flags & kUpReset) != 0) s_up_mailbox.store(0, std::memory_order_release);
    out->token = start.token;
    out->grid_index = start.grid_index;
    out->lifecycle_base = start.lifecycle_base;
    out->target_qpc = UnifiedPacer_TargetTicks(start.target);
    out->interval_ticks = start.interval_ticks;
    out->flags = start.flags;
    return SchedulerUnifiedReserve::Reserved;
}

void Scheduler_UnifiedPublishSleep(const SchedulerUnifiedReservation& reservation,
    UnifiedSleepOutcome outcome, int64_t release_qpc, int64_t arrival_qpc,
    const UnifiedLimitResult& limit) noexcept
{
    if (reservation.token == 0) return;
    PublishUnifiedStats(reservation.token, limit);
    // The word never replaces a newer reservation's: a retired Sleep resuming late finds the
    // slot taken and is discarded.
    const uint64_t word = UnifiedMailbox_Pack(reservation.token, outcome, release_qpc - arrival_qpc);
    uint64_t current = s_up_mailbox.load(std::memory_order_acquire);
    while (current == 0 || !UpTokenNewer(UnifiedMailbox_Token(current), reservation.token & 0x3FFFFFFFu)) {
        if (s_up_mailbox.compare_exchange_weak(current, word, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;
        }
    }
}

// ── Main entry point ──
// Order: owner prologue → stage events → pipeline
// → interval (VRR: I_0 → LFC; fixed: I_0) with the Reflex report drain → plan
// (published before the wait) → wait → release → collision guard readout →
// one source record.
void OnMarker(
    uint64_t frameID,
    int64_t now,
    SchedulerTrigger trigger,
    PacingSourceMarker marker) {
    // Source telemetry reports the argument, not a later re-read of `now`.
    s_source_marker_qpc = now;
    // Callers hold SchedulerOwnerGuard. Apply coalesced feedback resets before
    // any scheduler-owned model is read or mutated for this frame (Flush
    // requests).
    ApplyFeedbackResetRequests();
    // Unified owner, or a toggle that has not applied yet: reconcile the one
    // effective Sleep-mode tuple from the game thread. A key load and a flag
    // load with the key off.
    if (PacingPipeline_Unified() || s_up_reconcile) {
        if (NvAPI_SleepModeReconcilePending()) NvAPI_ReconcileSleepMode();
        else s_up_reconcile = false;
    }
    // Reflex reports not drained by this frame's DrainReflexReports are
    // discarded at exit, except on VRR frames whose end-of-frame poll feeds
    // the next frame's drain.
    struct PendingFeedbackScope {
        bool retain_for_drain = false;
        ~PendingFeedbackScope() noexcept {
            if (!retain_for_drain) DiscardReflexFrameTimeSamples();
        }
    } pending_feedback;
    // The Reflex driver limiter is applied only while this frame
    // resolves to it; every other exit releases it (restores the game's
    // Sleep parameters once).
    struct DriverLimiterScope {
        bool active = false;
        double interval_us = 0.0;
        ~DriverLimiterScope() noexcept { ReflexDriverLimiter_Update(interval_us, active); }
    } driver_limiter;
    const uint32_t nvapi_producer_epoch =
        trigger == SchedulerTrigger::NvApiMarker
            ? FrameTimeline_CurrentProducerEpoch(FrameIdentityDomain::NvApiFrame)
            : 0;

    // Marker flow health every frame. Only real marker triggers keep the
    // dispatcher's marker-path Present fallback suppressed; the fallback's own
    // enforcement calls must not.
    RecordEnforcementMarker();
    if (trigger == SchedulerTrigger::NvApiMarker ||
        trigger == SchedulerTrigger::PclMarker) {
        RecordMarkerTrigger();
    }

    ApplyLifecycle(trigger, marker);
    // One configuration snapshot decides the invocation; `config` is defined once.
    // Settled key-off branch: the direct stage drain, THEN the snapshot, so a
    // configuration published during the drain is seen at the same marker as
    // before. A key published during that drain is applied right after the
    // snapshot (the safety net: that one boundary invocation drained its events
    // under the old rules, the transition's reset follows).
    // Keyed branch (the key on or a transition pending: a key load and a flag, not taken with
    // the key off): snapshot, the key's transition, the drain under the key of that snapshot,
    // so the drain, the resolve and the plan run under one value of the key.
    const PacingPipelineConfig config = [&]() noexcept -> PacingPipelineConfig {
        if (s_up_key || PacingPipeline_Unified()) {
            PacingPipelineConfig snapshot = PacingPipeline_GetConfig();
            ApplyUnifiedKeyTransition(snapshot);
            // Developer keys unified_every_frame_service / unified_queue_target_us: this
            // invocation's snapshot values, set before the drain so the whole invocation uses one
            // value. Keyed branch only; the settled key-off branch never reaches the law.
            if (s_source_stage.Unified().Options().queue_law && !snapshot.unified_queue_law)
                LogQueueLawSummary("law off");
            s_source_stage.SetUnifiedOptions(kPacingUnifiedEveryFrameServiceDefault,
                kPacingUnifiedQueueTargetDefaultUs, snapshot.unified_hold_share,
                snapshot.unified_queue_law, snapshot.unified_queue_rank, snapshot.unified_queue_allow_us,
                snapshot.unified_queue_need_frames);
            DrainQueueLawFeed(snapshot.unified_queue_law);
            s_source_stage.DrainStageEventsUnderKey(s_up_key, FrameTimeline_CurrentControllerGeneration());
            return snapshot;
        }
        s_source_stage.DrainStageEvents();
        PacingPipelineConfig snapshot = PacingPipeline_GetConfig();
        if (snapshot.unified != s_up_key || snapshot.test_mode != s_up_test_mode) ApplyUnifiedKeyTransition(snapshot);
        return snapshot;
    }();
    // Vblank edges feed the fixed-refresh phase tracker on the owner only.
    // Entering fixed refresh starts it over, as at startup: a phase kept from
    // an earlier fixed period is stale, and the grid's ModeChange epoch would
    // snap to it until new edges corrected it.
    const bool vrr = g_pacing_mode.load(std::memory_order_relaxed) == PacingMode::VRR;
    static bool s_tracking_fixed_refresh = false;
    if (!vrr && !s_tracking_fixed_refresh) g_vblank_phase.Reset();
    s_tracking_fixed_refresh = !vrr;
    int64_t latest_vblank_qpc = 0;
    if (VBlank_ConsumeLatest(&latest_vblank_qpc)) {
        g_vblank_phase.IngestVBlank(latest_vblank_qpc, GetQPCFrequency(),
            g_estimated_refresh_us.load(std::memory_order_relaxed));
    }

    PacingRuntimeState runtime{};
    runtime.swapchain_valid = true;
    runtime.vrr = vrr;
    // The unified placement needs SIMULATION_START as the enforcement marker:
    // with a later marker the Sleep precedes the simulation.
    runtime.pre_sleep_available =
        (trigger == SchedulerTrigger::NvApiMarker || trigger == SchedulerTrigger::PclMarker) &&
        marker == PacingSourceMarker::SimulationStart && PreSleepAvailable();
    runtime.driver_limiter_unavailable = ReflexDriverLimiter_Unavailable();

    // Suspended (swapchain invalid, device lost or occluded): passthrough.
    const bool suspended = !IsSwapchainValid();
    if (suspended != s_suspended) {
        s_suspended = suspended;
        if (suspended) LOG_INFO("Scheduler: swapchain invalid, pacing suspended");
        else LOG_INFO("Scheduler: swapchain valid, pacing resumed");
    }
    if (suspended) {
        runtime.swapchain_valid = false;
        ResetUnifiedLaw();
        const PacingPipelineSnapshot pipeline = ResolvePipeline(trigger, config, runtime);
        RunPassthrough(GetCadenceContext(Scheduler_MakePlanKey(frameID, trigger),
            trigger, marker, SchedulerCadenceRegime::Suspended), pipeline, 0, 1.0);
        return;
    }

    // Priority: Suspended > DMFG > OLED cap > background > FG-Off > VRR/Fixed.
    // DMFG and its internal caps are intentionally unaffected by OLED Care.
    const bool dmfg_active = IsDmfgActive();
    const bool dmfg_changed = s_dmfg_state_initialized && dmfg_active != s_last_dmfg_active;
    s_dmfg_state_initialized = true;
    s_last_dmfg_active = dmfg_active;
    if (dmfg_changed) {
        ResetAdaptationRegime(dmfg_active ? "DMFG entered" : "DMFG exited");
        // Unified owner: the Sleep-mode tuple follows DMFG.
        if (config.unified) NvAPI_BumpSleepModeEpoch();
    }
    if (dmfg_active) {
        NativeGate_PublishNoFrameGeneration(false, 0);
        static bool s_dmfg_logged = false;
        if (!s_dmfg_logged) {
            LOG_INFO("DMFG active — passthrough/cap mode");
            s_dmfg_logged = true;
        }
        runtime.dmfg_passthrough = true;
        FrameSplitting_PublishFgDivisor(ComputeFGDivisorRaw());
        RunDmfg(frameID, trigger, marker, nvapi_producer_epoch,
            ResolvePipeline(trigger, config, runtime));
        return;
    }

    // One Reflex report poll per frame while frame generation is loaded:
    // here, before routing, while its divisor is unconfirmed (the report's AI
    // frame time is evidence for it) or on a fixed refresh; otherwise after a
    // frame's plan, or at the VRR passthrough exits below, which plan nothing.
    int fg_divisor_raw = ComputeFGDivisorRaw();
    const bool fg_loaded = IsFGDllLoaded();
    const bool polled_before_routing = fg_loaded && (fg_divisor_raw == 1 || !runtime.vrr);
    if (polled_before_routing) {
        PollReflexLatencyReport(ReportFeedbackIntervalUs());
        fg_divisor_raw = ComputeFGDivisorRaw();
    }
    FrameSplitting_PublishFgDivisor(fg_divisor_raw);
    // This is a resolved scheduler observation, not native-gate actuation;
    // the native gate and GPU readiness read it.
    NativeGate_ObserveFrameGeneration(fg_divisor_raw, FrameTimeline_CurrentControllerGeneration());
    runtime.fg_presenting = fg_divisor_raw > 1;

    const int target_fps = g_user_target_fps.load(std::memory_order_relaxed);
    if (target_fps != s_last_target_fps || fg_divisor_raw != s_last_fg_divisor_raw) {
        if (s_last_target_fps >= 0) {
            LOG_INFO("Scheduler: cap/FG change (fps %d -> %d, FG %d -> %d): grid period change",
                     s_last_target_fps, target_fps, s_last_fg_divisor_raw, fg_divisor_raw);
            ResetAdaptationRegime("cap or FG divisor change");
            s_driver_sleep_index = 0;
        }
        s_last_target_fps = target_fps;
        s_last_fg_divisor_raw = fg_divisor_raw;
    }

    const bool oled_care_active = OLEDCare_IsActive();
    if (oled_care_active != s_was_oled_care_active) {
        s_was_oled_care_active = oled_care_active;
        ResetAdaptationRegime(oled_care_active ? "OLED Care entered" : "OLED Care exited");
        LOG_INFO("OLED Care scheduler cap %s", oled_care_active ? "entered" : "exited");
    }

    // Background FPS cap check. The grid's ModeChange epoch replaces
    // the old full flush on focus regain.
    if (!SwapMgr_GetHWND()) {
        static bool s_hwnd_fallback_warned = false;
        if (!s_hwnd_fallback_warned) {
            LOG_WARN("SwapMgr_GetHWND() returned null — falling back to GetForegroundWindow for focus check");
            s_hwnd_fallback_warned = true;
        }
    }
    const bool focused = IsProcessFocused();
    if (!focused && s_was_focused) {
        s_background_mode = true;
    } else if (focused && !s_was_focused) {
        s_background_mode = false;
        ResetAdaptationRegime("focus regained");
    }
    s_was_focused = focused;

    PlannedFrameInput input{};
    input.frame_id = frameID;
    input.trigger = trigger;
    input.marker = marker;
    input.fg_divisor = static_cast<double>(fg_divisor_raw);
    // A test mode's pass-through exit (reflex_only; driver_limiter where the driver's limiter cannot apply; idle caps
    // in either mode): the frame goes through untouched as at an uncapped VRR frame, its reports polled once.
    const auto run_test_passthrough = [&](SchedulerCadenceRegime regime, int cap_fps, uint8_t extra_flags,
                                           double fg_divisor, const PacingPipelineSnapshot& pipeline) noexcept {
        RunPassthrough(GetCadenceContext(Scheduler_MakePlanKey(frameID, trigger), trigger, marker, regime, cap_fps),
            pipeline, extra_flags, fg_divisor);
        if (fg_loaded && !polled_before_routing) PollReflexLatencyReport(ReportFeedbackIntervalUs());
    };

    // ── Idle caps: OLED Care, background, FG-off (grid in Idle mode, L = 0) ──
    const int background_fps = g_background_fps.load(std::memory_order_relaxed);
    const int fg_off_cap = g_fg_off_fps.load(std::memory_order_relaxed);
    double idle_interval_us = 0.0;
    if (oled_care_active) {
        // Foreground-independent, including target_fps=0 and Win32. A disabled
        // background cap still means 20 FPS here, not uncapped OLED Care.
        input.regime = SchedulerCadenceRegime::OledCare;
        input.target_fps = background_fps;
        idle_interval_us = 1000000.0 / static_cast<double>(background_fps > 0 ? background_fps : 20);
    } else if (s_background_mode) {
        input.regime = SchedulerCadenceRegime::Background;
        input.target_fps = background_fps;
        input.extra_flags = PACING_SOURCE_BACKGROUND;
        if (background_fps > 0) idle_interval_us = 1000000.0 / static_cast<double>(background_fps);
        else runtime.uncapped = true; // disabled background cap: passthrough
    } else if (fg_off_cap > 0 && IsFGDllLoaded() && fg_divisor_raw == 1) {
        // Frame Generation off (menus, pauses, cutscenes): lower cap so the
        // GPU does not ramp up on uncapped non-FG frames.
        input.regime = SchedulerCadenceRegime::FgOffCap;
        input.target_fps = fg_off_cap;
        input.fg_divisor = 1.0;
        idle_interval_us = 1000000.0 / static_cast<double>(fg_off_cap);
    }
    // Unified owner: the idle interval for the next reservation, and a regime
    // change (idle against not) resets the law.
    if (config.unified) {
        const bool unified_idle = idle_interval_us > 0.0;
        s_up_idle_interval_us = idle_interval_us;
        if (unified_idle != s_up_idle_regime) ResetUnifiedLaw();
        s_up_idle_regime = unified_idle;
    }
    if (idle_interval_us > 0.0) {
        runtime.idle_cap = true;
        const PacingPipelineSnapshot pipeline = ResolvePipeline(trigger, config, runtime);
        // A test mode is a foreground instrument: its idle caps resolve to pass-through and run as such.
        if (config.test_mode != PacingTestMode::Off) {
            run_test_passthrough(input.regime, input.target_fps, input.extra_flags, input.fg_divisor, pipeline);
            return;
        }
        input.base_interval_us = idle_interval_us;
        input.interval_us = idle_interval_us;
        RunPlanned(input, pipeline, config);
        return;
    }
    if (runtime.uncapped) {
        RunPassthrough(GetCadenceContext(Scheduler_MakePlanKey(frameID, trigger), trigger,
            marker, input.regime, input.target_fps), ResolvePipeline(trigger, config, runtime),
            input.extra_flags, input.fg_divisor);
        if (fg_loaded && !polled_before_routing) PollReflexLatencyReport(ReportFeedbackIntervalUs());
        return;
    }
    // Test mode reflex_only: ReLimiter paces nothing whatever the saved cap and the timing regime (the cap is not
    // overwritten); the frame goes through and the game's own Reflex runs.
    if (config.test_mode == PacingTestMode::ReflexOnly) {
        runtime.uncapped = true;
        input.fg_divisor = ComputeFGDivisor(); // one call per frame: it advances a ramp
        run_test_passthrough(SchedulerCadenceRegime::Uncapped, target_fps,
            runtime.vrr ? 0 : PACING_SOURCE_FIXED_REFRESH, input.fg_divisor, ResolvePipeline(trigger, config, runtime));
        return;
    }

    // ── Fixed refresh: vblank-snapped grid ──
    if (!runtime.vrr) {
        input.regime = SchedulerCadenceRegime::Fixed;
        input.target_fps = target_fps;
        input.fg_divisor = ComputeFGDivisor(); // one call per frame: it advances a ramp
        input.extra_flags = PACING_SOURCE_FIXED_REFRESH;
        const double interval_us = ComputeFixedInterval(input.fg_divisor);
        if (!std::isfinite(interval_us) || interval_us <= 0.0 || interval_us > 500'000.0) {
            runtime.uncapped = true;
            SchedulerCadenceContext context = GetCadenceContext(
                Scheduler_MakePlanKey(frameID, trigger), trigger, marker,
                SchedulerCadenceRegime::Suspended);
            RunPassthrough(context, ResolvePipeline(trigger, config, runtime),
                PACING_SOURCE_FIXED_REFRESH, input.fg_divisor);
            return;
        }
        PacingPipelineSnapshot pipeline = ResolvePipeline(trigger, config, runtime);
        // Test mode driver_limiter where the driver's limiter cannot apply: pass-through, never a pacing path.
        if (config.test_mode != PacingTestMode::Off && pipeline.mode == PacingTimingMode::Off) {
            run_test_passthrough(SchedulerCadenceRegime::Uncapped, target_fps, PACING_SOURCE_FIXED_REFRESH, input.fg_divisor, pipeline);
            return;
        }
        // I_0 → grid (the whole-refresh snap is the grid's).
        input.fixed = g_vblank_phase.Reference();
        input.base_interval_us = interval_us;
        input.period_identity_us = ComputeFixedInterval(static_cast<double>(fg_divisor_raw));
        // This frame's Reflex reports, before its plan.
        DrainReflexReports(config);
        input.interval_us = interval_us;
        if (pipeline.placement == PacingSourcePlacement::DriverLimiter) {
            if (ApplyDriverLimiter(DriverLimiterIntervalUs(input))) {
                driver_limiter.active = true;
                driver_limiter.interval_us = DriverLimiterIntervalUs(input);
                RunDriverLimited(input, pipeline);
                return;
            }
            runtime.driver_limiter_unavailable = true;
            pipeline = ResolvePipeline(trigger, config, runtime);
            if (config.test_mode != PacingTestMode::Off && pipeline.mode == PacingTimingMode::Off) {
                run_test_passthrough(SchedulerCadenceRegime::Uncapped, target_fps, PACING_SOURCE_FIXED_REFRESH, input.fg_divisor, pipeline);
                return;
            }
        }
        RunPlanned(input, pipeline, config);
        return;
    }

    // ── VRR ──
    // Unified owner: with the cap off VRR is paced at the VRR cap (Hz - Hz^2/3600).
    const int vrr_target_fps = PacingPipeline_VrrTargetFps(config.unified, target_fps, [] {
        return UnifiedPacer_VrrCapFps(g_ceiling_hz.load(std::memory_order_relaxed));
    });
    input.target_fps = vrr_target_fps;
    if (vrr_target_fps <= 0) {
        // No limit: pass through for minimum latency.
        input.regime = SchedulerCadenceRegime::Uncapped;
        input.fg_divisor = ComputeFGDivisor();
        runtime.uncapped = true;
        RunPassthrough(GetCadenceContext(Scheduler_MakePlanKey(frameID, trigger), trigger,
            marker, input.regime, target_fps), ResolvePipeline(trigger, config, runtime),
            0, input.fg_divisor);
        if (fg_loaded && !polled_before_routing) PollReflexLatencyReport(ReportFeedbackIntervalUs());
        return;
    }
    input.regime = SchedulerCadenceRegime::Vrr;
    PacingPipelineSnapshot pipeline = ResolvePipeline(trigger, config, runtime);
    input.fg_divisor = ComputeFGDivisor(); // one call per frame: it advances a ramp
    // Test mode driver_limiter where the driver's limiter cannot apply: pass-through, never a pacing path.
    if (config.test_mode != PacingTestMode::Off && pipeline.mode == PacingTimingMode::Off) {
        run_test_passthrough(SchedulerCadenceRegime::Uncapped, target_fps, 0, input.fg_divisor, pipeline);
        return;
    }
    // I_0 (no LFC) → LFC hysteresis on I_0 → grid interval.
    input.base_interval_us = VrrCapIntervalUs(vrr_target_fps, input.fg_divisor);
    // This frame's Reflex reports, before its plan.
    DrainReflexReports(config);
    // Unified owner: no LFC multiple (the law owns the interval).
    input.interval_us = PacingPipeline_VrrIntervalUs(config.unified, input.base_interval_us, [&](double base) {
        return ApplyLfc(base, input.fg_divisor);
    });
    input.period_identity_us = VrrPeriodIdentityUs(vrr_target_fps, fg_divisor_raw);
    // This frame's reports were drained before the end-of-frame poll; keep
    // that poll's samples for the next frame's drain.
    pending_feedback.retain_for_drain = true;
    if (pipeline.placement == PacingSourcePlacement::DriverLimiter) {
        if (ApplyDriverLimiter(DriverLimiterIntervalUs(input))) {
            driver_limiter.active = true;
            driver_limiter.interval_us = DriverLimiterIntervalUs(input);
            RunDriverLimited(input, pipeline);
            PollReflexLatencyReport(input.interval_us);
            return;
        }
        runtime.driver_limiter_unavailable = true;
        pipeline = ResolvePipeline(trigger, config, runtime);
        if (config.test_mode != PacingTestMode::Off && pipeline.mode == PacingTimingMode::Off) {
            pending_feedback.retain_for_drain = false; // the reports of this frame stay drained; the poll's samples are not kept
            run_test_passthrough(SchedulerCadenceRegime::Uncapped, target_fps, 0, input.fg_divisor, pipeline);
            return;
        }
    }
    RunPlanned(input, pipeline, config);
    PollReflexLatencyReport(input.interval_us);
}
