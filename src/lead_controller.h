#pragma once

// Lead controller. Learns the lead L from the source release to the output
// target per (rung, FG divisor) bucket, from per-frame samples X that exclude
// ReLimiter's own holds, including, where the next frame's GPU work runs
// behind a GPU wait, the time a frame's GPU work waited for the GPU to finish
// the previous frame.
// Pure C++17 without Windows, logger or config
// dependencies. Owned and called only by the scheduler owner; not thread-safe.
//
// Storage is fixed and nothing allocates. sizeof(LeadController) is about
// 165 KB (20 buckets of a 256-sample ring and a 2,048-bin histogram, four
// release → Present call windows, four release → heavy GPU work windows and
// the first-submission window of the same shape, plus the pending-frame table):
// keep the instance in static storage or on the heap, never on a thread stack.

#include "pacing_core_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// ── Limits ──

constexpr size_t LEAD_WINDOW_CAPACITY = 256;       // newest samples kept per bucket
// Samples are ranked in 20 µs bins. Bins below LEAD_HISTOGRAM_BINS (40.96 ms)
// are counted in a histogram; a longer sample keeps its own 20 µs bin in the
// ring and is ranked among the other long ones, so the window has no
// ceiling below the 3 I bound. A histogram whose last bin also held every
// longer sample pinned Gears of War: E-Day's held FG 2× lead at 41.45 ms
// (Q99 of the last bin + 0.5 ms) while its true Q99 was 44–58 ms.
constexpr size_t LEAD_HISTOGRAM_BINS = 2048;
constexpr double LEAD_HISTOGRAM_BIN_US = 20.0;
constexpr uint32_t LEAD_WARM_SAMPLES = 32;
// A GPU idle inside a frame's span of at least this (µs) shows the GPU waited
// inside the frame's work for its own submissions (SampleUs).
constexpr double LEAD_HANDOFF_GPU_IDLE_US = 1000.0;
constexpr size_t LEAD_DIVISOR_BUCKETS = 4;         // FG divisor 1..4
constexpr size_t LEAD_PENDING_CAPACITY = 32;       // frames being assembled, by grid index
constexpr uint64_t LEAD_PENDING_EXPIRY_FRAMES = 8; // frames this far behind the newest release close
constexpr uint64_t LEAD_PENDING_AHEAD_FRAMES = 8;  // events accepted this far ahead of it
// Saturation rule (SetQueueWaitMode; unified pacer only): the ONE lifetime
// bound of a deferred lead sample. The sample of a GPU-rung frame waits for
// the law's z_used, which the law accepts while its ring entry exists and
// the frame is within its 32-frame window (kUnifiedExpireAge,
// unified_pacer.h; mirrored here, the lead has no law dependency). The frame
// and the predecessor inputs its sample reads must stay in the 32-slot
// pending table that long: events open slots LEAD_PENDING_AHEAD_FRAMES ahead
// of the newest release, so frame m's slot is safe through newest − m <=
// 32 − 8 − 1 = 23 and its predecessor's (reused one index earlier) through
// 22, the last distance the frame may be kept. Bound = min(32, LEAD_PENDING_CAPACITY
// − LEAD_PENDING_AHEAD_FRAMES − 2) = 22: a z_used delivered up to that many
// releases after the frame's own release feeds it, a later one feeds
// nothing. The 8-frame expiry above is unchanged with the rule off and for
// every frame that is not deferred.
constexpr uint64_t LEAD_QUEUE_WAIT_LAW_WINDOW_FRAMES = 32;
constexpr uint64_t LEAD_QUEUE_WAIT_TABLE_FRAMES =
    LEAD_PENDING_CAPACITY - LEAD_PENDING_AHEAD_FRAMES - 2;
constexpr uint64_t LEAD_QUEUE_WAIT_LIFETIME_FRAMES =
    LEAD_QUEUE_WAIT_LAW_WINDOW_FRAMES < LEAD_QUEUE_WAIT_TABLE_FRAMES ? LEAD_QUEUE_WAIT_LAW_WINDOW_FRAMES
                                                                      : LEAD_QUEUE_WAIT_TABLE_FRAMES;
static_assert(LEAD_QUEUE_WAIT_LIFETIME_FRAMES + 2 + LEAD_PENDING_AHEAD_FRAMES <= LEAD_PENDING_CAPACITY &&
        LEAD_QUEUE_WAIT_LIFETIME_FRAMES > LEAD_PENDING_EXPIRY_FRAMES,
    "a deferred frame and its predecessor must outlive the 8-frame expiry yet fit the pending table");
constexpr size_t LEAD_RESUME_DELAY_CAPACITY = 128; // recent probe-measured u − o observations
constexpr size_t LEAD_RESUME_MEDIAN_SAMPLES = 32;  // newest u − o in the median (decontamination fallback)
// A waited u − o above this is a late GPU resume: measured and shown (OSD,
// telemetry), never acted on.
constexpr double LEAD_RESUME_LATE_US = 1000.0;
// Spike floor (below): arrivals per block, blocks kept, the rank among their
// maxima that sets the tail, closed blocks and GPU times needed before the
// floor applies, and the GPU times kept (their median is Γ).
constexpr uint32_t LEAD_SPIKE_BLOCK_SAMPLES = 64;
constexpr size_t LEAD_SPIKE_BLOCKS = 32;
constexpr size_t LEAD_SPIKE_TAIL_RANK = 2;
constexpr uint32_t LEAD_SPIKE_WARM_BLOCKS = 4;
constexpr size_t LEAD_SPIKE_GPU_SAMPLES = 32;
static_assert(LEAD_SPIKE_WARM_BLOCKS >= LEAD_SPIKE_TAIL_RANK && LEAD_SPIKE_TAIL_RANK >= 1,
    "the tail rank needs that many closed blocks");
constexpr double LEAD_PRESENT_PLACEMENT_US = 150.0;
constexpr double LEAD_SEED_MIN_US = 1000.0;
constexpr double LEAD_SEED_MAX_US = 8000.0;
constexpr double LEAD_SLEW_MAX_US = 100.0;         // per planned frame; also at most I/64
constexpr double LEAD_LONG_SAMPLE_INTERVALS = 3.0; // samples above 3 I are excluded
// Lead limit: the outlier bound itself. Samples above 3 I never enter
// the window, so a warm lead never needs more; there is no separate latency
// budget that would judge the game's own pipeline.
constexpr double LEAD_BUDGET_INTERVALS = LEAD_LONG_SAMPLE_INTERVALS;

// Collision-aware release (GPU queue wait). Frame n + 1's native
// Present call lands I − L + C after frame n's release Signal o, C being the
// source release → Present call time. A Present call within about 2-2.5 ms
// after the Signal collides with the GPU's resume: in eleven sessions
// 92-100% of late resumes and
// 95-100% of the 5-8 ms GPU stalls fell inside that window, and the late
// share was 0.00-0.27% at margins of 2.5 ms or more. The GPU queue wait lead
// is therefore at most I + Q10(C) − M without current Reflex GPU active times
// (the headroom guard below needs them): a frame that needs a longer lead
// arrives at its gate after the Signal and is shown when ready, instead of
// colliding. M is fixed.
constexpr double LEAD_CALL_QUANTILE = 0.10;        // low quantile of C
constexpr double LEAD_RELEASE_MARGIN_US = 2500.0;  // M

// Release headroom. A late resume is the work after the GPU
// wait queued behind one of the next frame's long command buffers on the same
// GPU engine: the GPU does not preempt them (a GPU trace showed none
// of 1.07 million packets preempted; a waited resume ran 96 µs after the
// running packet completed). The next frame's heavy GPU work starts at
// r − A, its GPU readiness minus its Reflex GPU active render time (within
// −117 / +65 µs, p10 / p90, of the first long command buffer in that trace),
// so D = r − A − s is the source release → heavy GPU work time and the
// headroom of frame n's release is I − L_{n+1} + D_{n+1}. The GPU queue wait
// lead is therefore at most I + Q10(D) − H, H a fixed distance (developer key
// gpu_wait_headroom_us, default 1.0 ms). In all 1.35 million waited resumes
// with probes and Reflex GPU times (47 Rogue Core captures) late resumes
// (u − o > 1 ms) were 0.00-0.05% per 250 µs distance bin from 1 ms on, and
// 99.2% of them were closer than 1 ms; live at H = 1.0 ms 0.006-0.040% of
// waited resumes were late. The distance used to be learned from late
// resumes (widen at once, narrow as old evidence decayed, kept across
// resets). That was a latch: it widened to 1.75 ms on 10 of 123 resumes at a
// session's start and could not narrow, because the guard itself keeps
// releases out of the region below the edge it would have to measure. The
// GPU load dependence the raw table shows (late at 0.5-0.75 ms: 0.16-0.18% at
// A/I <= 0.75, 4.9% at 0.85-0.95) is composition: heavy sections hold more
// frames briefly (holds < 250 µs 1.6% of waited resumes at A/I <= 0.6,
// 32% at 0.85-0.95), and short holds are the late ones at a fixed distance
// (3.9% against 0.04-0.45% for longer holds at 0.5-1 ms); with holds of at
// least 500 µs the load gradient is 0.05-0.75%.
//
// D's samples come from frames whose predecessor's GPU waited at its gate
// (the probe at or before the Signal, else readiness before it). Behind a
// GPU-bound stretch the next frame's heavy work queues behind the previous
// frame's GPU work and D grows with that backlog (Rogue Core: D p10 2.9 ms
// after a waited predecessor, 3.9–5.0 ms after one that was not; a window
// that kept the second kind through an overloaded stretch limited the first
// held frames after it at 11.2–11.6 ms against 9.7 ms in steady state, and
// the GPU resumed late on 9–25 frames per burst). A frame whose predecessor's
// verdict is still pending adds
// its sample when the predecessor's GpuReady or GpuResume event arrives. C
// (the Present call window) is sampled from every GPU queue wait frame: it is
// the guard when D is cold or stale.
constexpr double LEAD_RELEASE_HEADROOM_US = 1000.0; // H, the default of gpu_wait_headroom_us
constexpr double LEAD_WORK_QUANTILE = 0.10;        // low quantile of D
// The heavy-work window is current while it holds LEAD_WARM_SAMPLES D samples
// and at most this many frames planned with the GPU queue wait's lead (held)
// passed since the newest Reflex report of such a frame: the age counts every
// such frame and
// any report of one resets it, whether or not that report adds a D sample,
// so a pause with reports keeps the window current and one without reports
// (Reflex stopped) ends it, and the Present-call guard applies.
constexpr uint32_t LEAD_WORK_STALE_FRAMES = 64;

// Late state (developer key gpu_wait_late_guard, default off). The collision
// guard assumes the next frame is released one interval after this one. A game
// that is slower than its slot for CPU reasons arrives after its planned start:
// its frames are released I + lateness apart, and since none of them is held
// the heavy-work window receives no sample and keeps limiting the lead with
// what it learned at the cap (Rogue Core with the game and render threads in
// series: the limit stayed at 10.33 ms for 110 s while frames were ready 14 to
// 17 ms after their release and the GPU idled 1.5 to 4.5 ms before the next
// frame's heavy work). While each of the newest LEAD_LATE_FRAMES planned GPU
// queue wait frames arrived more than LEAD_LATE_ARRIVAL_US after its planned
// start, the heavy-work window is warm and current and the guard applies, the
// limit is
//   L <= I + (their least arrival lateness)
//          + (the least D of the newest LEAD_LATE_FRAMES reported frames,
//             whatever their predecessor) − H:
// the form and the headroom of the heavy-work guard, with the interval
// corrected by what was measured and D taken from evidence that exists in this
// state. A minimum is set by the least delayed sample, so the D of a frame that
// queued behind its predecessor's GPU work (larger) does not raise it; if all
// are delayed the limit falls to about the fastest recent readiness minus H and
// nothing is held. The lateness is the game's arrival against its planned
// start, not the release (the hard limit's wait and a late wake do not count),
// and the frames must be consecutive plans. One frame on time ends the state in
// that plan. A frame planned in the state gives its successor no sample in the
// heavy-work window, which so stays what on-time frames taught it.
// LEAD_LATE_FRAMES is the horizon after which the guard calls its evidence
// stale; a D that old does not count either.
constexpr size_t LEAD_LATE_FRAMES = LEAD_WORK_STALE_FRAMES;
constexpr double LEAD_LATE_ARRIVAL_US = 100.0; // kUnifiedLateReleaseUs

// Queue order. Both guards assume the next frame's GPU work can
// run while frame n's GPU waits at its gate, so it can occupy the engine when
// the Signal comes. Some games run it behind the wait instead (one queue, in
// order): then it cannot delay the resume, and the hold delays it. The test:
// frame n's GPU waited at the Signal o_n (GPU queue wait: the probe; FG input
// wait: its readiness before the Signal), frame n + 1 had begun submitting
// (RENDERSUBMIT_START <= o_n), and frame n + 1's GPU work started at g_{n+1}
// (its Reflex gpuRenderStartTime through the stable clock map). g_{n+1} <
// o_n − LEAD_CONCURRENCY_TOLERANCE_US shows concurrent work. Each GPU output
// keeps its own pairs. Rogue Core (GPU queue wait): 72-94% of such pairs in
// four captures, guarded or not, from the first or second pair. CONTROL
// Resonant: none of 3,661 GPU queue wait pairs in three captures (g − o at
// least 38 µs), and with the guard off (Output override GPU wait) 2,309
// waited frames above its limit had no late resume while jitter fell from
// 1.91 to 1.04 ms; on the FG input wait g − o was at least 1.8 ms on every
// frame (the frame generation of frame n runs first), as in Gears of War:
// E-Day (none of 508 pairs). An output's verdict is "behind the wait" once
// LEAD_WARM_SAMPLES of its newest LEAD_CONCURRENCY_SAMPLES pairs exist and
// none was concurrent, and only while Reflex reports are current (the GPU
// queue wait: its heavy-work window; the FG input wait: a report within the
// newest LEAD_WORK_STALE_FRAMES frames of the FG divisor slot): without them
// no new pair can show the order changing. One concurrent pair ends it at
// once. Without probes or Reflex reports no pair exists. The verdict does not
// need a pair on every frame: a pair needs a waited frame whose next frame
// had begun submitting by the release, and frames without one cannot collide
// (CONTROL Resonant: gaps of up to 362 GPU queue wait frames between pairs).
// Behind the wait the GPU queue wait's guards stand aside, and on both GPU
// outputs the lead sample leaves out the frame's wait for the GPU to finish
// frame n − 1 (LeadController::SampleUs), except for a frame the cap released
// whose own submissions set its readiness: its report shows the GPU waited
// inside its work for its submissions while its first work streamed. Such a
// sample waits for the frame's Reflex report. Under the guard D cannot show
// this: the next frame's heavy work queues behind the guarded release, so
// Q10(D) rises with the lead (CONTROL: p10 4.7 ms guarded, 5.5 ms at +1.7 ms,
// 8.1 ms at +4.6 ms lead) and never shows the room.
constexpr size_t LEAD_CONCURRENCY_SAMPLES = 64;
// Clock map spread (about 6 µs) and probe mapping; Rogue Core's concurrent
// pairs start p50 0.7-0.9 ms before the Signal.
constexpr double LEAD_CONCURRENCY_TOLERANCE_US = 100.0;

// Lockstep engines (CONTROL Resonant): the game finishes
// frame n's render submission and Present only after ReLimiter releases
// frame n + 1 (the next release gates the previous frame's Present), so
// frame n's Present follows ReLimiter's waits for later frames. Those waits
// are not the game's time. A later frame k gated frame n when it was
// released no later than frame n's Present-side time (PRESENT_START or the
// Present call); its waits (the source wait and a
// pre-Sleep wait) inside [s_n, that time] are frame n's gated hold. The
// late-output check (source_stage.cpp) judges frame n without it; the lead
// keeps the measured X, because a lockstep
// frame cannot be ready before the release it waits for. In a serial engine
// (Rogue Core) the next wait starts after Present(n): no frame is gated.

// The game's simulation → render queue (CONTROL Resonant, FG 2× with the
// driver's Reflex Sleep bypassed). A pipelined engine's
// simulation thread can finish frame n (SIMULATION_END e_n) while its render
// thread is still inside frame n − 1's Present (PRESENT_END b_{n−1}: 9.5 ms
// of Present back-pressure, p50); the render thread took the frame at
// RENDERSUBMIT_START w_n, 0.46 ms (p50) after b_{n−1}. That wait
// q = max(0, min(w_n, b_{n−1}) − max(s_n, e_n)), w clamped to [s_n, p_n],
// is time the release could have come later without moving the frame's
// readiness, so the FG input wait lead learns X − q. With q in X the lead
// learned a one-frame-deep queue and kept it, because a release one frame
// earlier leaves the frame waiting just as long: X 40.2 ms with q p50
// 15.6 ms on 90% of frames and 18 ms more PC latency than with the driver's
// Sleep. Without q the lead fell to 26.2 ms with the source still
// on the grid. Time after b_{n−1} stays in X (a lockstep frame waits there
// for a later release, above), and the wait for frame n − 1 behind the wait
// (SampleUs) starts at w or later.
// FG input wait only. The FG input wait's back-pressure comes from its own
// GPU gate, and its output point lies a GPU frame after the Present call.
// Other rungs keep X:
// - On source only (FG unqualified) q was the game's own pipeline
//   depth (p50 16 ms on half the frames), and the PRESENT_START output point
//   sits right after it. With q removed, the late-output check's gated-hold
//   forgiveness shrank (p50 29.1 / 19.6 → 7.3 ms) and the grid restarted 292
//   times per 1,000 frames, against none with q kept in X.
// - H1 already removes ReLimiter's own CPU hold (the overlap rule), so
//   the queue left over is the game's as well.
// - On the GPU queue wait (CONTROL with FG off), late resumes concentrated on
//   queued frames (38.5% against 4.8%); that output keeps X.

// Spike floor (developer key lead_spike_floor, default on). Behind the wait
// (queue order, above) the lead covers Q_q of each frame's own work, and a
// game's rare long simulations lie beyond it: Q99 of 256 is the
// fourth-largest sample. While the GPU runs back to back the time a long
// simulation keeps the frame's work from the GPU is lost GPU time the later
// frames inherit. CONTROL Resonant (FG 2× at 130): simulations above 15 ms
// 11–23 per minute against a p50 of 5 ms; at the own-work lead of 29.5 ms the
// render thread took each frame the instant its simulation ended, the GPU
// waited more than 3 ms for a frame's work 8–18 times per minute, and display
// jumps above 4 ms rose to 5.2–9.5 per 1,000 from 1.3–1.6 at the earlier
// 40 ms lead, whose 6–9 ms queue in front of the render thread absorbed them.
// The lead is therefore at least L_spike = Q_tail(a) + Γ:
// - a, the arrival: release → the frame's first GPU work at the driver
//   without the simulation → render queue q (above), (w − s − q) + F_n, F_n
//   the frame's own driverStartTime − renderSubmitStartTime. Only frames
//   whose first work streamed (LeadGpuReport::first_work_streamed), whose
//   render thread did not take them only at or after frame n − 1's Signal
//   (e < o_{n−1} <= w: that timing is ReLimiter's), within 3 I. It holds the
//   game's simulation, render-thread turnaround and first submission, which
//   did not move with the lead (simulation p50 / p99 5.1 / 11.7 ms at L 41,
//   5.05 / 12.1 at L 29.8). Held-first frames (Gears of War: E-Day, first
//   work pinned to the previous Signal) add no arrival.
// - Q_tail: the LEAD_SPIKE_TAIL_RANK-th largest of the maxima of the newest
//   LEAD_SPIKE_BLOCKS closed blocks of LEAD_SPIKE_BLOCK_SAMPLES arrivals
//   (about 31 s at 65 real frames per second): every arrival of that window
//   but the longest is covered, so one isolated long second does not set it.
//   A game whose long simulations recur leaves at most one uncovered per
//   window besides new maxima. The fourth-largest (the lead's own outlier
//   rank, about one frame in a thousand) left the three longest of each
//   window: live in CONTROL Resonant (FG 2× at 130) it cut the GPU's waits
//   above 3 ms from 11.9 to 2.4 per minute without the GPU-paced cadence and
//   19.5 → 6.8 with it, jitter above 2 ms 25.1 → 10.4 and 20.9 → 13.0 per
//   1,000, for 2.2–2.6 ms of PC latency (lead 28.4–28.8 → 31.2–31.3 ms), but
//   the waits left were 22–25 ms simulations 5–9 ms beyond it, 3–5 display
//   jumps each. Replayed, the second-largest added 0.8–3.9 ms of lead in
//   those scenes and removed most of them; the largest also set the lead in
//   Gears of War: E-Day's GPU-paced cadence (40% of frames) and is not used.
// - Γ: the median GPU start → readiness time (r − g) of the newest
//   LEAD_SPIKE_GPU_SAMPLES frames.
// A frame whose work reaches the GPU after the GPU is free is ready a + Γ
// after its release, so a lead of at least L_spike has it ready by its
// target. The floor applies behind the wait once LEAD_SPIKE_WARM_BLOCKS
// blocks and LEAD_SPIKE_GPU_SAMPLES GPU times are known, and enters only the
// warm target, max(Q_q + allowance, L_spike): the slew and S follow one lead.
// It needs no load threshold: where the arrival tail is thin it lies below
// the lead (Gears
// 1.4–9.7 ms below in every capture, CONTROL's light scene within 0.3 ms).
// It is not part of the 3 I budget, and it did not
// slow the game's render submission (CONTROL p50 7.8–8.0 ms with and without
// it). Reset and ResetBuckets clear it.

// Report pairs (frame n's Reflex report and n − 1's, driver clock): a pair
// whose first work reached the driver in the first half of the render
// thread's window shows that the game streams its submission and gives one
// F, its first-submission delay (LeadController::OnReportPair,
// FirstSubmissionUs). The lead sample behind the wait places the frame's first
// work at RENDERSUBMIT_START + F (SampleUs). F is the median of the newest 256
// from LEAD_WARM_SAMPLES; Reset and ResetBuckets clear its window.
struct HandoffBackPressureSample {
    // Frame n, µs on the driver's clock from any common origin.
    double simulation_end_us = 0.0;   // SIMULATION_END
    double render_start_us = 0.0;     // RENDERSUBMIT_START
    double render_end_us = 0.0;       // RENDERSUBMIT_END
    double first_submission_us = 0.0; // driverStartTime: the driver's first work of the frame
    double gpu_start_us = 0.0;        // gpuRenderStartTime
    // Frame n − 1.
    double previous_render_free_us = 0.0; // later of RENDERSUBMIT_END and PRESENT_END
    double previous_gpu_end_us = 0.0;     // gpuRenderEndTime
};
// The pair's first work reached the driver in the first half of the render
// thread's window (valid input only): evidence that the game streams.
bool HandoffBackPressure_Streams(const HandoffBackPressureSample& sample) noexcept;
// One frame's first work (first_submission_us, the report's driverStartTime)
// reached the driver before the middle of its render submission
// [render_start_us, render_end_us]; false for invalid input. Any common
// origin.
bool HandoffFirstWorkStreams(double render_start_us, double render_end_us, double first_submission_us) noexcept;

// ── Inputs and status ──

// The verdict of a GPU-rung frame's gate evidence (GpuGateEvidence, GpuGateVerdict).
enum class LeadGate : uint8_t {
    NotHeld = 0,
    Held = 1,
    Pending = 2, // the evidence is still arriving
    Unknown = 3  // the evidence is gone or cannot arrive
};

// The collision guard's verdict on one lead (NextLeadUs).
struct LeadGuardOutcome {
    bool judged = false;       // a GPU queue wait lead the guard judged (C or D window warm)
    double offset_us = 0.0;    // the limit minus I: Q10(D) − H, or Q10(C) − M
    // How much lower the returned lead is than the bucket's lead (>= 0): the
    // guard's limit, and after the limit relaxed, the lead slewing back up
    // to it.
    double reduction_us = 0.0;
    // Late state (above): the limit came from the newest frames' lateness and
    // heavy-work starts.
    bool late_state = false;
    // Its condition was met while the request only observed it (LeadRequest::late_observe): the limit is the
    // built one.
    bool late_seen = false;
    // When judged: the limit on the release → target distance, I + offset in
    // µs (the anchor's delta is not in it). With the developer key
    // gpu_wait_late_guard the executor resolves no target past it; 0 otherwise.
    double release_limit_us = 0.0;
};

// One Reflex latency report's GPU times for a frame (LeadController::OnGpuReport).
struct LeadGpuReport {
    double active_us = 0.0; // gpuActiveRenderTimeUs: the frame's GPU busy time A (> 0)
    double span_us = 0.0;   // gpuRenderEndTime − gpuRenderStartTime on the driver's clock; 0 unknown
    int64_t start_qpc = 0;  // gpuRenderStartTime as QPC through the stable clock map; 0 unknown
    // The frame's first work reached the driver in the first half of its
    // render submission (HandoffFirstWorkStreams on its own driverStartTime
    // and RENDERSUBMIT_START/END); false when unknown.
    bool first_work_streamed = false;
    // Its driverStartTime − renderSubmitStartTime on the driver's clock in µs
    // (the spike floor's F_n, above); NaN unknown.
    double first_submission_delay_us = std::numeric_limits<double>::quiet_NaN();
};

// The owner's source release of one planned frame (plan fields plus s).
struct LeadFrameStart {
    PacingPlanKey key{};
    uint64_t grid_index = 0;
    PacingRung rung = PacingRung::SourceOnly;
    double fg_divisor = 1.0;
    double interval_us = 0.0;      // the plan's grid interval I
    int64_t source_release_qpc = 0; // s
    // The plan's output target T (> 0); a release without one is invalid.
    int64_t output_target_qpc = 0;
    // Frames outside Vrr and Fixed are tracked for matching but never sampled.
    PacingTimingMode mode = PacingTimingMode::Vrr;
    // H3: the plan's GPU hold reports a post-wait resume probe (u), so the
    // next frame waits for it instead of estimating it from o.
    bool gpu_resume_probe = false;
    // The collision guard's verdict on L (NextLeadUs); a negative or non-finite
    // reduction or a non-finite offset is an invalid release.
    LeadGuardOutcome guard{};
    // ReLimiter's waits for this frame before s: the source wait
    // [wait_start_qpc, wait_end_qpc] and a pre-Sleep wait [pre_wait_start_qpc,
    // pre_wait_end_qpc]; 0 when there was none. They gate earlier frames of a
    // lockstep engine (above).
    int64_t wait_start_qpc = 0;
    int64_t wait_end_qpc = 0;
    int64_t pre_wait_start_qpc = 0;
    int64_t pre_wait_end_qpc = 0;
    // Lead anchor (unified pacer): B, the instant the lead is measured from
    // (H = B + L); 0 none, and an anchor that is not earlier than s is
    // ignored. s (source_release_qpc) keeps its physical meaning in every
    // wait and stage-time check: the wait validation above, the gated hold, C
    // and D, the behind-wait correction, the render queue and the spike
    // arrival's coordinates. The slot stores delta = (s − B) in µs (>= 0; 0
    // without an anchor) and adds it only where a duration measured from s
    // becomes a lead quantity, after the range checks: the lead window's
    // sample (after the behind-wait correction and q) and the spike floor's
    // binned arrival. A lead L then covers a readiness measured from B while
    // every wait check still sees s. Without an anchor on every frame each
    // result is unchanged.
    int64_t lead_anchor_qpc = 0;
    // The owner sets this together with lead_anchor_qpc, only in the unified
    // pacer's branch: the lead anchor is applied only when it is true (the
    // controller compares no anchor value otherwise).
    bool anchored = false;
};

// Everything NextLeadUs needs from the plan being made and the resolved pipeline.
struct LeadRequest {
    PacingRung rung = PacingRung::SourceOnly;
    double fg_divisor = 1.0;
    double interval_us = 0.0;
    PacingSourcePlacement placement = PacingSourcePlacement::Marker;
    PacingTimingMode mode = PacingTimingMode::Vrr;
    double quantile = 0.95;
    double allowance_us = 500.0;
    // GPU queue wait: L at most I + Q10(D) − release_headroom_us once 32 D
    // samples exist and are current, else I + Q10(C) − LEAD_RELEASE_MARGIN_US
    // once 32 C samples exist; neither while the next frame's GPU work runs
    // behind the wait (queue order, above).
    double release_headroom_us = LEAD_RELEASE_HEADROOM_US;
    bool collision_guard = false;
    // Spike floor (above; developer key lead_spike_floor).
    bool spike_floor = true;
    // Lead anchor (above): the planned frame's own delta = (s − B) in µs, 0
    // none (always 0 with the unified pacer off). L itself is learned from
    // the slots' samples, which carry their own deltas; the one place the
    // planned frame's delta is read is the collision guard (GPU queue wait
    // rung), which limits the physical release → target distance H − R =
    // L − delta: the limit is I + offset + delta, added only if this is
    // positive, so the capped target is the same R-relative one for any
    // anchor. Not validated or counted otherwise.
    double anchor_offset_us = 0.0;
    // Late state (above; developer key gpu_wait_late_guard): the planned
    // frame's grid index, how late the game arrived for its planned start in µs
    // (0 on time), and whether the state may set the limit. The two rings are
    // kept with the key off too; only the limit reads the key.
    uint64_t late_index = 0;
    double late_us = 0.0;
    bool late_guard = false;
    // Judge the state's condition without applying it (the off period of developer key
    // gpu_wait_late_guard_interleave_s): LeadGuardOutcome::late_seen.
    bool late_observe = false;
};

struct LeadBucketStatus {
    bool warm = false;
    uint32_t samples = 0;     // samples in the window (at most LEAD_WINDOW_CAPACITY)
    double quantile_us = 0.0; // Q_q at the last request's q
    double target_us = 0.0;   // last target: the seed while cold, Q_q + allowance when warm
    double lead_us = 0.0;     // last lead of the bucket, before the collision guard
    double budget_us = 0.0;   // last limit: 3 I (the outlier bound)
    double median_us = 0.0;   // median X of the window; 0 while cold
    uint64_t excluded_long = 0;
    bool budget_limited = false; // last warm target exceeded the budget
    // The spike floor (above) now, 0 where it does not apply, and whether it
    // set the last warm target.
    double spike_floor_us = 0.0;
    bool spike_floor_limited = false;
};

struct LeadStatistics {
    uint64_t samples = 0;            // samples added to a bucket
    uint64_t excluded_long = 0;      // samples above 3 I, not added
    uint64_t budget_limited_frames = 0; // lead decisions whose warm target exceeded the budget
    // Collision guard: GPU queue wait leads it judged (C or D window warm)
    // and of those, leads it lowered (its limit or the slew back up to it);
    // the headroom guard judged headroom_checked_frames of them, the
    // Present-call guard the rest.
    uint64_t collision_checked_frames = 0;
    uint64_t collision_guarded_frames = 0;
    uint64_t headroom_checked_frames = 0;
    double last_collision_limit_us = 0.0; // the newest judged limit: I + Q10(D) − H or I + Q10(C) − M
    bool last_guard_headroom = false;     // the newest judged lead used the headroom guard
    // Release margin evidence (waited resumes with the next Present call
    // known; measured, not acted on).
    uint64_t margin_samples = 0;
    uint64_t margin_late_samples = 0;
    // Release headroom: D samples (GPU queue wait frames with r and a Reflex
    // GPU active time), Reflex GPU active times without a pending frame, and
    // waited resumes with the next frame's heavy GPU work start known
    // (measured, not acted on).
    uint64_t work_samples = 0;
    uint64_t unmatched_gpu_active = 0;
    uint64_t headroom_samples = 0;
    uint64_t headroom_late_samples = 0;
    // Queue order (above), GPU queue wait and FG input wait: waited releases
    // paired with the next frame's GPU start, those whose next frame's GPU
    // work started before the release (concurrent), and the newest
    // g_{n+1} − o_n.
    uint64_t concurrency_samples = 0;
    uint64_t concurrent_samples = 0;
    double last_concurrency_us = 0.0;
    uint64_t fg_concurrency_samples = 0;
    uint64_t fg_concurrent_samples = 0;
    double last_fg_concurrency_us = 0.0;
    uint64_t incomplete_dropped = 0; // planned frames closed without their own stage times
    uint64_t unmatched_events = 0;   // stage events without a matching planned frame
    uint64_t missing_ready = 0;      // H3 frames closed without r
    // Probed held frames not added to the resume history: the GPU reached
    // the Wait after the Signal (u − o is GPU backlog), or the pre-Wait time is unknown.
    uint64_t resume_not_waited = 0;
    // Waited u − o observations (the GPU waited at the Signal), and of those,
    // resumes later than LEAD_RESUME_LATE_US.
    uint64_t resume_waited_samples = 0;
    uint64_t resume_late_samples = 0;
    uint64_t invalid_inputs = 0;     // rejected frequency, requests, releases and events
    // GPU-rung samples that left out the frame's wait for the GPU to finish
    // the previous frame (behind the wait, SampleUs), and the newest such wait.
    uint64_t predecessor_wait_samples = 0;
    double last_predecessor_wait_us = 0.0;
    // Behind the wait: samples that kept that wait because the frame's own
    // submissions set its readiness, and the newest wait kept; samples of
    // frames the cap released that closed without their report (the wait left
    // out).
    uint64_t kept_wait_samples = 0;
    double last_kept_wait_us = 0.0;
    uint64_t report_missing_samples = 0;
    // Lead samples shortened by the game's simulation → render queue q
    // (above: FG input wait), and the newest such q.
    uint64_t queued_samples = 0;
    double last_queue_us = 0.0;
    double last_sample_us = 0.0;     // newest sample added to a bucket
    // Spike floor (above): arrivals added, arrivals of GPU-rung frames left
    // out (first work not streamed, Signal-pinned, stage times missing or
    // beyond 3 I), GPU times added, and the newest arrival.
    uint64_t spike_samples = 0;
    uint64_t spike_excluded = 0;
    uint64_t spike_gpu_samples = 0;
    double last_spike_arrival_us = 0.0;
    // Saturation rule (SetQueueWaitMode, below; unified pacer only): GPU-rung
    // lead samples that waited for the law's z_used (deferred), were fed with
    // it (queue_wait_samples; last_queue_wait_removed_us is the newest part
    // removed after the predecessor wait SampleUs already took out), and
    // frames dropped without a sample (the stream stopped being live before
    // z_used, the frame expired without one, a mode change or reset).
    uint64_t queue_wait_deferred = 0;
    uint64_t queue_wait_samples = 0;
    uint64_t queue_wait_dropped = 0;
    double last_queue_wait_removed_us = 0.0;
};

// ── Sample window ──

// The newest LEAD_WINDOW_CAPACITY samples of one bucket as a ring of 20 µs
// bin indices, plus a two-level histogram of those below LEAD_HISTOGRAM_BINS
// and a count of the longer ones.
class LeadSampleWindow {
public:
    void Reset() noexcept;
    // Negative samples count in the first bin; no upper bin limit.
    void Add(double sample_us) noexcept;
    uint32_t Count() const noexcept { return count_; }
    // Lower-index empirical quantile sorted[floor(p * (n - 1))] (pacing_metrics.h)
    // resolved to the center of the bin that holds it, so results are within
    // half a bin of the exact value. 0 when empty. A rank among the samples
    // beyond the histogram is found from the ring.
    double Quantile(double probability) const noexcept;

    // The 20 µs bin of a sample: 0 for non-positive or NaN, at most UINT32_MAX.
    static uint32_t BinOf(double sample_us) noexcept;
    static double BinCenterUs(uint32_t bin) noexcept;

private:
    static constexpr size_t kBlockBins = 32;
    static constexpr size_t kBlockCount = LEAD_HISTOGRAM_BINS / kBlockBins;

    std::array<uint32_t, LEAD_WINDOW_CAPACITY> ring_bins_{};
    std::array<uint16_t, LEAD_HISTOGRAM_BINS> bin_counts_{};
    std::array<uint16_t, kBlockCount> block_counts_{};
    uint32_t beyond_count_ = 0; // ring samples at or beyond LEAD_HISTOGRAM_BINS
    uint32_t head_ = 0; // next write position; the oldest sample once full
    uint32_t count_ = 0;
};

// ── Controller ──

class LeadController {
public:
    // Lifecycle change: buckets, pending frames, resume history and
    // statistics cleared.
    // A non-positive frequency is counted and disables sampling until the next Reset.
    void Reset(int64_t qpc_frequency) noexcept;
    // Enforcement marker change: buckets, C, D, F and the spike floor cleared.
    // Frames already released produce no sample but still decontaminate the
    // next frame; their later events add nothing to C or D (measured from the
    // old release point). F still takes the reports that arrive afterwards.
    void ResetBuckets() noexcept;
    // One call per planned frame; applies at most one slew step to the
    // selected bucket and clamps L to [0, 3 I]. Present placement returns
    // 150 µs; None placement and Off or Idle mode return 0 without touching a
    // bucket. *budget_limited (optional) is true when the warm target
    // Q_q + allowance exceeded the budget; cold, seed and non-bucket
    // leads are never budget-limited. With request.collision_guard, a GPU
    // queue wait lead is at most I + Q10(D) − H once 32 current D samples
    // exist, else I + Q10(C) − M once 32 C samples exist, unless the queue
    // order shows the next frame's GPU work behind the wait (not judged); the
    // bucket's slew continues from the unguarded lead, and *guard (optional)
    // receives the guard's verdict: whether it judged the lead, its limit
    // minus I and how much it lowered the lead: the lead it returns slews up
    // to the bucket's lead once the limit relaxed (the reduction is lead −
    // returned).
    double NextLeadUs(const LeadRequest& request, bool* budget_limited = nullptr,
        LeadGuardOutcome* guard = nullptr) noexcept;
    void OnSourceRelease(const LeadFrameStart& frame) noexcept;
    void OnStageEvent(const PacingStageEvent& event) noexcept;
    // The Reflex latency report of NvAPI frame (producer_epoch, frame_id):
    // its GPU active render time A, span and first-work
    // verdict, and through the stable clock map the QPC of its
    // gpuRenderStartTime (0 unknown; the queue-order test). Pairs with the
    // pending frame of that key (reports arrive about two to four frames
    // after the source release); a report without one is counted and
    // dropped. The first report of a frame wins. A lead sample behind the
    // wait of a frame the cap released is emitted here (SampleUs).
    void OnGpuReport(uint32_t producer_epoch, uint64_t frame_id, const LeadGpuReport& report) noexcept;
    // The Reflex report pair of NvAPI frame (producer_epoch, frame_id) and
    // its previous frame (above): a pair that streams adds its first-submission
    // delay to F's window.
    void OnReportPair(uint32_t producer_epoch, uint64_t frame_id,
        const HandoffBackPressureSample& sample) noexcept;
    // Saturation rule (unified pacer only; the owner calls these with the key
    // and never otherwise).
    //
    // Invariant: a hold may delay the next frame's GPU work only while the
    // GPU has slack; it must never lower the GPU's throughput below the start
    // rate. A GPU-rung lead sample contains the time the frame's work waited
    // in the GPU queue, which the holds themselves create: a hold raises that
    // wait, the wait raises the sample and so the lead, and a larger lead
    // holds more (positive feedback). With the mode on the lead sample of
    // frame n is today's sample minus z_used(n), the law's queue wait of the
    // frame's report without the predecessor's hold (max(0, z_raw −
    // hold(pred)); UnifiedPacer::PopUsedReport), anchored as ever (+ delta).
    // A larger z then means fewer holds (negative feedback). With headroom
    // z_used is about 0 and the lead is today's, one feed delay later.
    //
    // Liveness, the one rule (LeadSampleDefers): while the mode is on and the
    // report stream is live (SetQueueWaitLive) at the moment a GPU-rung
    // frame's sample would be emitted, it is deferred until OnQueueWaitUsed;
    // with the stream not live at that moment it is fed as today, at
    // readiness, unsubtracted. A deferred frame is dropped, feeding nothing,
    // when the stream stops being live before its report arrives, or when no
    // report came by LEAD_QUEUE_WAIT_LIFETIME_FRAMES (the ONE bound of a
    // deferred frame, above; the 8-frame expiry no longer ends it). Each
    // frame feeds at most once across transitions both ways. Rungs without a
    // GPU wait (source only, CPU holds) are never deferred: their sample
    // holds no GPU queue wait.
    //
    // No double subtraction: SampleUs already removed d (the frame's wait for
    // frame n − 1 including its hold, predecessor_wait_us) behind the wait,
    // and z_used is the part of the reported queue wait that is not the
    // predecessor's hold, so only max(0, z_used − d) is removed. A mode
    // change resets the deferral state (deferred frames are dropped, the
    // stream counts as not live until SetQueueWaitLive says otherwise).
    void SetQueueWaitMode(bool enabled) noexcept;
    // The law's QueueWaitLive; a live → not live change drops every deferred
    // frame (no sample).
    void SetQueueWaitLive(bool live) noexcept;
    // The law used the report of frame (key, grid_index): z_used_us (>= 0, the
    // value the law stored). The key must equal the slot's key exactly; a
    // missing, stale (already sampled or dropped), cross-epoch or unknown
    // frame is ignored, never a raw sample. Feeds the deferred sample at
    // once, or keeps z_used for a frame whose sample is still to come.
    void OnQueueWaitUsed(const PacingPlanKey& key, uint64_t grid_index, double z_used_us) noexcept;
    // The owner's ordered form (it applies a report that became used while
    // it is still recording the stage events the sample reads): Register
    // stores z_used under the same identity rules without emitting or
    // finalizing anything and leaves a deferred frame deferred, and a stage
    // event does not emit it either; FlushQueueWaitSamples then emits, in grid
    // order, every frame whose z_used is registered and whose own fields and
    // predecessor are resolved (TryFinalize), after the predecessor's
    // readiness, resume and output were recorded: SampleUs then removes the
    // predecessor wait d before z_used is applied. OnQueueWaitUsed is the
    // direct form: Register, then emit that one frame at once.
    void RegisterQueueWaitUsed(const PacingPlanKey& key, uint64_t grid_index, double z_used_us) noexcept;
    void FlushQueueWaitSamples() noexcept;
    bool QueueWaitMode() const noexcept { return queue_wait_mode_; }
    bool QueueWaitLive() const noexcept { return queue_wait_live_; }

    LeadBucketStatus BucketStatus(PacingRung rung, double fg_divisor) const noexcept;
    // Spike floor (above): L_spike for a bucket, 0 where it does not apply
    // (not a GPU wait behind the wait, or the memories not yet warm); the
    // arrival tail Q_tail and Γ it adds, 0 until known.
    double SpikeFloorUs(PacingRung rung, double fg_divisor) const noexcept;
    double SpikeTailUs() const noexcept { return spike_tail_us_; }
    double SpikeGpuUs() const noexcept { return spike_gpu_median_us_; }
    // Median sample X of a warm bucket (release to output point); 0 when the
    // bucket is cold or unknown. The source slack of a plan is L minus this
    // value.
    double MedianSampleUs(PacingRung rung, double fg_divisor) const noexcept;
    // Lower-index median of the newest LEAD_RESUME_MEDIAN_SAMPLES waited
    // u − o observations (held, probed GPU queue wait frames whose GPU waited
    // at the Signal); 0 before any. A held frame without a probe resumes at
    // o + this (behind the wait, SampleUs).
    double MedianResumeDelayUs() const noexcept;
    // Frame (key, grid_index)'s gated hold in QPC ticks (lockstep engines,
    // above): the waits of later pending frames of its lifecycle released no
    // later than its Present-side time, inside [s, that time]; 0 when the
    // frame or that time is unknown.
    int64_t GatedHoldTicks(const PacingPlanKey& key, uint64_t grid_index) const noexcept;
    LeadStatistics Statistics() const noexcept { return statistics_; }
    // Queue order (above) of a GPU output (GpuQueueWait or FgInputWait; false
    // for the others): the newest pairs show the next frame's GPU work
    // running behind the wait and the FG divisor slot's Reflex reports are
    // current. Behind the wait the GPU queue wait's guards stand aside and the
    // output's lead samples leave out the frame's wait for the previous
    // frame. The pairs are kept across Reset: they describe the game's GPU
    // submission, not the lifecycle.
    bool NextGpuWorkBehindWait(PacingRung rung, double fg_divisor) const noexcept;
    size_t ConcurrencyPairs(PacingRung rung) const noexcept;
    size_t ConcurrentPairs(PacingRung rung) const noexcept;

private:
    static constexpr size_t kBucketCount =
        static_cast<size_t>(PacingRung::Count) * LEAD_DIVISOR_BUCKETS;

    // Stage times of a pending frame; 1..9 match PacingStageEventKind.
    enum Field : uint8_t {
        FieldSource = 0,
        FieldMarkerArrival,
        FieldNativeArrival, // reserved: the historical NativeArrival stage event (no producer); keeps the field layout
        FieldPresentCall,
        FieldOutputRelease,
        FieldGpuReady,
        FieldGpuResume,
        FieldRenderSubmit,
        FieldSimulationEnd, // e: GPU waits
        FieldPresentEnd,    // b: GPU waits
        FieldCount
    };

    struct PendingFrame {
        PacingPlanKey key{};
        uint64_t grid_index = 0;
        std::array<int64_t, FieldCount> qpc{};
        int64_t gpu_wait_qpc = 0;  // pre-Wait GPU time of the GpuResume event; 0 unknown
        double interval_us = 0.0;
        bool z_known = false;     // OnQueueWaitUsed arrived before the sample was emitted
        bool z_deferred = false;  // the lead sample waits for z_used (saturation rule)
        float z_used_us = 0.0f;   // the law's z_used of this frame (z_known); placed in the padding
        int64_t wait_start_qpc = 0;     // ReLimiter's source wait before s; 0 none
        int64_t wait_end_qpc = 0;
        int64_t pre_wait_start_qpc = 0; // and its pre-Sleep wait; 0 none
        int64_t pre_wait_end_qpc = 0;
        uint32_t early_events = 0; // events stored before the source release
        uint16_t field_mask = 0;   // bit per Field
        uint8_t bucket = 0;
        PacingRung rung = PacingRung::SourceOnly;
        PacingPlanStatus output_status = PacingPlanStatus::Unknown;
        bool occupied = false;
        bool started = false;   // source release seen
        bool sampled = false;   // Vrr or Fixed: produces a sample
        bool finalized = false; // outcome judged or given up
        bool lead_sampled = false; // lead window sample emitted or given up
        bool closed = false;    // expired: no longer waited on
        bool gpu_resume_probe = false;
        bool resume_delay_recorded = false;
        bool resume_waited = false; // the GPU waited at the Signal (w <= o)
        bool resume_late = false;   // and resumed more than LEAD_RESUME_LATE_US after it
        bool call_recorded = false;   // C entered the call window, or given up (ResetBuckets)
        bool margin_recorded = false; // p_{n+1} − o_n entered the margin learner
        double gpu_active_us = 0.0;   // Reflex GPU active render time A; 0 unknown
        double gpu_span_us = 0.0;     // Reflex GPU start → end span on the driver's clock; 0 unknown
        bool work_recorded = false;     // D = r − A − s entered the work window, or given up
        bool headroom_recorded = false; // r_{n+1} − A_{n+1} − o_n entered the headroom learner
        bool first_work_streamed = false; // LeadGpuReport::first_work_streamed
        // Lead anchor (unified pacer only): (s − B) in µs, stored at the
        // release only when the owner passed an anchor, so a slot is 0 (the
        // whole-struct reset) with the key off. Added to the lead sample and the
        // spike arrival. Placed in
        // the padding between the bools: PendingFrame does not grow.
        bool anchored = false; // LeadFrameStart::anchored with delta > 0: the emit paths add anchor_delta_us
        float anchor_delta_us = 0.0f;
        int64_t gpu_start_qpc = 0;      // Reflex gpuRenderStartTime as QPC; 0 unknown
        bool concurrency_recorded = false; // (o_n, g_{n+1}) entered the queue-order window
        bool spike_recorded = false;     // its spike floor arrival was decided (added or left out)
        bool spike_gpu_recorded = false; // its r − g was decided
        bool gpu_wait_lead = false;      // planned with the GPU queue wait's lead (held)
        bool late_state = false;         // planned in the late state: its successor adds no D to the work window
        bool late_work_recorded = false; // its D entered the late state's ring, or was given up
    };

    // Late state: the newest LEAD_LATE_FRAMES values with the grid index each belongs to.
    struct LateWindow {
        std::array<float, LEAD_LATE_FRAMES> values{};
        std::array<uint64_t, LEAD_LATE_FRAMES> indices{};
        uint32_t head = 0;
        uint32_t count = 0;

        void Reset() noexcept { head = 0; count = 0; }
        bool Full() const noexcept { return count == LEAD_LATE_FRAMES; }
        void Push(uint64_t index, double value) noexcept
        {
            values[head] = static_cast<float>(value);
            indices[head] = index;
            head = (head + 1) % static_cast<uint32_t>(LEAD_LATE_FRAMES);
            if (count < LEAD_LATE_FRAMES) ++count;
        }
        // The entries must be consecutive plans: another index than the newest + 1 starts over.
        void PushConsecutive(uint64_t index, double value) noexcept
        {
            if (count > 0) {
                const uint32_t newest = (head + static_cast<uint32_t>(LEAD_LATE_FRAMES) - 1) %
                    static_cast<uint32_t>(LEAD_LATE_FRAMES);
                if (index != indices[newest] + 1) Reset();
            }
            Push(index, value);
        }
        double Min() const noexcept
        {
            float least = std::numeric_limits<float>::infinity();
            for (uint32_t entry = 0; entry < count; ++entry) {
                if (values[entry] < least) least = values[entry];
            }
            return static_cast<double>(least);
        }
        // The grid index of the oldest entry held (count > 0).
        uint64_t OldestIndex() const noexcept
        {
            return indices[count < LEAD_LATE_FRAMES ? 0 : head];
        }
    };

    struct Bucket {
        LeadSampleWindow window;
        uint64_t excluded_long = 0;
        double quantile_us = 0.0;
        double target_us = 0.0;
        double lead_us = 0.0;          // the lead before the collision guard
        double returned_lead_us = 0.0; // the lead NextLeadUs returned last
        bool held_back = false;        // returned_lead_us was below lead_us (the guard or its slew)
        double budget_us = 0.0;
        bool lead_started = false; // lead_us holds a lead to slew from (a warm bucket)
        bool budget_limited = false;
        bool spike_floor_limited = false; // the spike floor set the last warm target
    };

    static bool FindBucketIndex(PacingRung rung, double fg_divisor, size_t* index) noexcept;
    static void ResetBucket(Bucket* bucket) noexcept;
    static uint16_t FieldBit(Field field) noexcept;
    static bool HasField(const PendingFrame& frame, Field field) noexcept;
    static void SetField(PendingFrame* frame, Field field, int64_t qpc) noexcept;
    static uint16_t OwnFieldMask(PacingRung rung) noexcept;
    static bool CpuHoldStartField(PacingRung rung, Field* field) noexcept;

    // Pending table
    PendingFrame* Find(uint64_t grid_index) noexcept;
    const PendingFrame* Previous(const PendingFrame& frame) const noexcept;
    void Discard(PendingFrame* frame) noexcept;
    void StartLifecycle(const PacingPlanKey& key, uint64_t grid_index, bool discard_all) noexcept;
    void ExpireBehindNewest() noexcept;
    void RecordResumeDelay(PendingFrame* frame) noexcept;
    // Copies the newest `count` u − o observations (count <= resume_delay_count_).
    void NewestResumeDelays(size_t count, double* delays_us) const noexcept;
    // Collision guard
    void RecordCallTime(PendingFrame* frame) noexcept;
    void RecordReleaseMargin(PendingFrame* frame) noexcept;
    void RecordWorkStart(PendingFrame* frame) noexcept;
    void RecordReleaseHeadroom(PendingFrame* frame) noexcept;
    // One queue-order pair (o_n, g_{n+1}) of a waited GPU-rung frame n.
    void RecordConcurrency(PendingFrame* frame) noexcept;
    double CollisionGuardedLeadUs(const LeadRequest& request, size_t bucket_index, double lead_us,
        LeadGuardOutcome* guard) noexcept;
    // The guard's limit minus I for an FG divisor slot: Q10(D) − headroom_us
    // while the heavy-work window is warm and current (*headroom true), else
    // Q10(C) − LEAD_RELEASE_MARGIN_US while the call window is warm; false
    // when neither is.
    bool CollisionOffsetUs(size_t divisor, double headroom_us, double* offset_us, bool* headroom) const noexcept;
    bool WorkCurrent(size_t divisor) const noexcept;
    // Late state (above): true with *offset_us = least arrival lateness + least D − H while the newest
    // LEAD_LATE_FRAMES planned frames all arrived late and as many current D exist; else false, *offset_us kept.
    bool LateOffsetUs(size_t divisor, const LeadRequest& request, double* offset_us) const noexcept;
    bool BehindWait(PacingRung rung, size_t divisor) const noexcept;
    // F, the median first submission − RENDERSUBMIT_START of the report pairs
    // that stream (above); 0 until LEAD_WARM_SAMPLES.
    double FirstSubmissionUs() const noexcept;

    // Sample assembly
    bool PreviousResolved(const PendingFrame& frame) const noexcept;
    // flush: a deferred frame whose z_used is registered may emit (only
    // FlushQueueWaitSamples passes true).
    void TryFinalize(PendingFrame* frame, bool flush = false) noexcept;
    void ForceFinalize(PendingFrame* frame) noexcept;
    // The outcome at readiness: X must be finite (else the frame gives up its
    // lead sample) and the spike floor's r − g is decided. The lead window
    // sample follows it, but behind the wait a frame the cap released waits for
    // its Reflex report (LeadSampleWaitsForReport) or closes without one.
    void EmitOutcome(PendingFrame* frame) noexcept;
    void EmitLeadSample(PendingFrame* frame) noexcept;
    bool LeadSampleWaitsForReport(const PendingFrame& frame) const noexcept;
    // Saturation rule: the frame's lead sample would wait for z_used now (mode
    // on, GPU rung, z_used unknown, stream live).
    bool LeadSampleDefers(const PendingFrame& frame) const noexcept;
    // The frame (key, grid_index) accepting z_used_us (saturation rule): its
    // slot with z_known set, nullptr when the report is ignored.
    PendingFrame* AcceptQueueWaitUsed(const PacingPlanKey& key, uint64_t grid_index, double z_used_us) noexcept;
    // Drops every deferred frame without a sample.
    void DropDeferredSamples() noexcept;
    // The GPU's idle inside the frame's span on the driver's clock: span − A,
    // less the FG input wait's own hold after readiness; false without the
    // frame's report, or on the FG input wait without its release.
    bool GpuIdleInSpanUs(const PendingFrame& frame, double* idle_us) const noexcept;
    // A frame's measured release → output point X (below; the GPU waits:
    // max(p, r) − s), and the lead sample from it: behind the wait without
    // the GPU wait for frame n − 1 (through predecessor_wait_us, 0
    // otherwise), unless the frame's own submissions set its readiness
    // (through kept_wait_us, the wait kept in the sample).
    double MeasuredUs(const PendingFrame& frame, const PendingFrame* previous) const noexcept;
    double SampleUs(const PendingFrame& frame, const PendingFrame* previous, double raw_us,
        double* predecessor_wait_us = nullptr, double* kept_wait_us = nullptr) const noexcept;
    // The game's simulation → render queue q (above), in µs: FG input wait; 0
    // for other rungs or without e, w, p and the previous frame's b.
    double QueueUs(const PendingFrame& frame, const PendingFrame* previous) const noexcept;
    // q itself.
    double RenderQueueWaitUs(const PendingFrame& frame, const PendingFrame& previous) const noexcept;
    // Spike floor (above): a frame's arrival once its report came, its r − g
    // once both are known, and a restart of both memories.
    void RecordSpikeArrival(PendingFrame* frame, double first_submission_us) noexcept;
    void RecordSpikeGpu(PendingFrame* frame) noexcept;
    void ResetSpikeFloor() noexcept;
    // Lockstep engines: waits of later frames that gated this one, in ticks.
    int64_t GatedHoldTicks(const PendingFrame& frame) const noexcept;
    // The frame's CPU Present-side event: PRESENT_START (H0, H1) or the
    // Present call (GPU rungs).
    static Field PresentSideField(PacingRung rung) noexcept;
    bool PreviousCpuHold(const PendingFrame* previous, int64_t origin_qpc,
        double* begin_us, double* end_us) const noexcept;
    bool PreviousGpuResume(const PendingFrame* previous, int64_t origin_qpc,
        double* resume_us) const noexcept;
    // A GPU-rung frame's gate from the evidence present now: the probe (w <= o)
    // once it arrived, else held and ready before the Signal; Pending without
    // its OutputRelease or either. Read once by a consumer that takes a frame at
    // one event (a queue-order pair's frame n on the FG input wait): a probe still
    // to come does not hold it back.
    static LeadGate GpuGateEvidence(const PendingFrame& frame) noexcept;
    static bool PreviousGpuHeld(const PendingFrame& previous) noexcept;
    // The verdict for frames whose decision is sealed once (the D samples of
    // the successor): GpuGateEvidence, but Pending while a resume
    // probe the plan expects (gpu_resume_probe) can still arrive.
    static LeadGate GpuGateVerdict(const PendingFrame& frame) noexcept;
    double MicrosecondsBetween(int64_t from_qpc, int64_t to_qpc) const noexcept;

    // Requests
    double ValidQuantile(double quantile) noexcept;
    double ValidAllowance(double allowance_us) noexcept;

    std::array<Bucket, kBucketCount> buckets_{};
    std::array<PendingFrame, LEAD_PENDING_CAPACITY> pending_{};
    std::array<float, LEAD_RESUME_DELAY_CAPACITY> resume_delays_us_{};
    size_t resume_delay_head_ = 0;
    size_t resume_delay_count_ = 0;
    // Collision guard: release → Present call times C and release → heavy
    // GPU work times D of GPU queue wait frames per FG divisor, and the frames
    // planned with the GPU queue wait's lead since the newest Reflex report
    // of one (the work window's age, LEAD_WORK_STALE_FRAMES).
    std::array<LeadSampleWindow, LEAD_DIVISOR_BUCKETS> call_windows_{};
    std::array<LeadSampleWindow, LEAD_DIVISOR_BUCKETS> work_windows_{};
    std::array<uint32_t, LEAD_DIVISOR_BUCKETS> work_age_frames_{};
    // Late state (above), per FG divisor: the arrival lateness of the newest
    // planned GPU queue wait frames and the D of the newest reported ones.
    std::array<LateWindow, LEAD_DIVISOR_BUCKETS> late_arrivals_{};
    std::array<LateWindow, LEAD_DIVISOR_BUCKETS> late_work_{};
    // First-submission delays F (first submission − RENDERSUBMIT_START, µs) of
    // the report pairs that stream.
    LeadSampleWindow first_submission_window_{};
    // Queue order: the newest LEAD_CONCURRENCY_SAMPLES pairs of each GPU
    // output ([0] GPU queue wait, [1] FG input wait), 1 = concurrent.
    struct QueueOrderWindow {
        std::array<uint8_t, LEAD_CONCURRENCY_SAMPLES> pairs{};
        size_t head = 0;
        size_t count = 0;
        size_t concurrent = 0;
    };
    std::array<QueueOrderWindow, 2> queue_order_{};
    // FG input wait frames since the newest Reflex report of one, per FG
    // divisor slot (LEAD_WORK_STALE_FRAMES + 1 after a reset: stale).
    std::array<uint32_t, LEAD_DIVISOR_BUCKETS> fg_report_age_frames_{};
    // Spike floor (above): the open block's maximum and arrival count, the
    // newest closed blocks' maxima as 20 µs bins, and the tail they give;
    // the newest r − g times and their median.
    struct SpikeMemory {
        std::array<uint16_t, LEAD_SPIKE_BLOCKS> block_max_bins{};
        uint16_t current_max_bin = 0;
        uint16_t current_count = 0;
        uint8_t head = 0;
        uint8_t count = 0;
    };
    SpikeMemory spike_memory_{};
    double spike_tail_us_ = 0.0;
    std::array<float, LEAD_SPIKE_GPU_SAMPLES> spike_gpu_us_{};
    uint8_t spike_gpu_head_ = 0;
    uint8_t spike_gpu_count_ = 0;
    double spike_gpu_median_us_ = 0.0;
    int64_t qpc_frequency_ = 0;
    bool lifecycle_known_ = false;
    // Saturation rule (SetQueueWaitMode / SetQueueWaitLive).
    bool queue_wait_mode_ = false;
    bool queue_wait_live_ = false;
    PacingPlanKey lifecycle_{}; // identity domain, producer epoch and generation of the newest release
    uint64_t newest_index_ = 0;
    LeadStatistics statistics_{};
};
