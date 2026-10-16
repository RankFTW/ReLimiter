#pragma once

// Lead controller. Learns the lead L from the source release to the output
// target per (rung, FG divisor) bucket, from per-frame samples X that exclude
// ReLimiter's own holds, including GPU holds carried through a busy GPU.
// Pure C++17 without Windows, logger or config
// dependencies. Owned and called only by the scheduler owner; not thread-safe.
//
// Storage is fixed and nothing allocates. sizeof(LeadController) is about
// 160 KB (20 buckets of a 256-sample ring and a 2,048-bin histogram, four
// release → Present call windows, four release → heavy GPU work windows and
// four hold-room windows of the same shape, plus the pending-frame table):
// keep the instance in static storage or on the heap, never on a thread stack.

#include "pacing_core_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

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
constexpr uint32_t LEAD_HANDOFF_MIN_SAMPLES = 16;  // the render handoff offset is 0 until this many samples
// Render handoff GPU floor (below): the GPU idle before a frame that counts
// as the GPU waiting for it (the late-resume unit); report pairs whose first
// work reached the driver in the first half of the render thread's window
// that prove the game streams its submission; how many frames past the
// newest GPU end the floor extrapolates; and the quantile of the gap between
// a frame's PRESENT_END and its GPU end that bounds Ĝ from the render
// thread.
constexpr double LEAD_HANDOFF_GPU_IDLE_US = 1000.0;
constexpr uint32_t LEAD_HANDOFF_STREAM_PROOF = 32;
constexpr uint64_t LEAD_HANDOFF_GPU_HORIZON_FRAMES = 8;
constexpr double LEAD_HANDOFF_GPU_BOUND_QUANTILE = 0.05;
constexpr size_t LEAD_DIVISOR_BUCKETS = 4;         // FG divisor 1..4
constexpr size_t LEAD_PENDING_CAPACITY = 32;       // frames being assembled, by grid index
constexpr uint64_t LEAD_PENDING_EXPIRY_FRAMES = 8; // frames this far behind the newest release close
constexpr uint64_t LEAD_PENDING_AHEAD_FRAMES = 8;  // events accepted this far ahead of it
constexpr size_t LEAD_RESUME_DELAY_CAPACITY = 128; // recent probe-measured u − o observations
constexpr size_t LEAD_RESUME_MEDIAN_SAMPLES = 32;  // newest u − o in the median (decontamination fallback)
// A waited u − o above this is a late GPU resume: the GPU wait cap ladder's
// evidence (rung_feasibility.h GpuWaitCap).
constexpr double LEAD_RESUME_LATE_US = 1000.0;
constexpr size_t LEAD_SLACK_CAPACITY = 120;        // newest slack samples per bucket
// GPU-paced cadence (developer A/B gpu_paced_cadence): the newest X of frames
// whose release the GPU floor set, per output class (GPU waits, CPU outputs),
// for one FG divisor at a time.
constexpr size_t LEAD_GPU_PACED_CAPACITY = 128;
// GPU-paced cadence: the rhythm's period is the mean of the newest
// LEAD_GPU_PERIOD_RECENT GPU frame periods (the driver's own Sleep weighted
// the newest five GPU frames about equally in Gears of War: E-Day; the floor's
// median of 256 follows load changes too slowly). The readiness anchor keeps
// the newest LEAD_GPU_READY_GAP_CAPACITY gaps from a frame's GPU readiness
// (its pre-hold readiness, or the hold's release when the GPU waited) to its
// Reflex GPU end.
constexpr size_t LEAD_GPU_PERIOD_RECENT = 5;
constexpr size_t LEAD_GPU_READY_GAP_CAPACITY = 64;
// GPU-paced hold margin (developer A/B gpu_paced_hold_margin): the newest
// LEAD_GPU_NEED_CAPACITY per-frame GPU needs D (GpuNeedQuantileUs).
constexpr size_t LEAD_GPU_NEED_CAPACITY = 128;
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
// is therefore at most I + Q10(C) − M: a frame that needs a longer lead
// arrives at its gate after the Signal and is shown when ready, instead of
// colliding. M is the release margin learned from the waited resumes.
constexpr double LEAD_CALL_QUANTILE = 0.10;        // low quantile of C
constexpr double LEAD_MARGIN_START_US = 2500.0;    // the margin before evidence (and without probes)
constexpr double LEAD_MARGIN_MIN_US = 1000.0;
constexpr double LEAD_MARGIN_MAX_US = 4000.0;
constexpr double LEAD_MARGIN_CUSHION_US = 500.0;   // margin = collision edge + this
constexpr double LEAD_MARGIN_BIN_US = 250.0;       // edge resolution and step
constexpr size_t LEAD_MARGIN_BINS = 32;            // 0..8 ms; the last bin also holds larger margins
// Evidence decays by 1/32768 per waited resume (half-life about 22,700, a
// few minutes of play): collisions come in heavy stretches (0.5-5% late at
// 1.75-2 ms in a session's first two minutes, 45-97% in its heaviest 30 s
// stretches), so the margin keeps the edge of the recurring heavy stretches
// instead of the calm one in between. A 4,096-resume memory narrowed to
// 2.0-2.25 ms in calm stretches (offline replay).
constexpr double LEAD_MARGIN_DECAY_SAMPLES = 32768.0;
constexpr double LEAD_MARGIN_MIN_SAMPLES = 32.0;   // weighted resumes for a bin to count
constexpr double LEAD_MARGIN_MIN_LATE = 4.0;       // weighted late resumes for a bin to collide
// The background is the late share at margins at least 1 ms above M; the
// learner judges nothing before 256 weighted resumes measure it. A bin
// collides when its late share exceeds the background by more than 2.5
// points, at z > 2.5. Widening scans upward from the edge below the
// background and stops at the first measured bin that does not collide: an
// earlier build jumped 2.5 -> 4.0 ms on 15 late resumes of
// 960 shortly after a focus change (lateness well above the collision
// window), and with M = 4.0 ms 98.8% of frames reached the GPU wait after
// their release (display jitter p50 438 us against 82 at the normal margin).
constexpr double LEAD_MARGIN_BACKGROUND_SAMPLES = 256.0;
constexpr double LEAD_MARGIN_EXCESS_SHARE = 0.025;
constexpr double LEAD_MARGIN_EXCESS_Z = 2.5;
// The bin just below the edge is clean with at least 512 weighted resumes and
// a late share at most 0.5 points above the background. The edge narrows one
// bin after 1,024 consecutive waited resumes with it clean or, above the
// start, with fewer than LEAD_MARGIN_MIN_LATE weighted late resumes left
// there; widening needs no wait. The guard keeps releases about a cushion
// above the edge, so the bin below it seldom collects 512 resumes (Rogue Core
// at H = 2.25 ms: at most 199 weighted in 15 minutes, none late at any
// distance); without the second test a widening never faded.
constexpr double LEAD_MARGIN_CLEAN_SAMPLES = 512.0;
constexpr double LEAD_MARGIN_CLEAN_SHARE = 0.005;
constexpr uint32_t LEAD_MARGIN_NARROW_SAMPLES = 1024;

// Release headroom. A late resume is the work after the GPU
// wait queued behind one of the next frame's long command buffers on the same
// GPU engine: the GPU does not preempt them (a GPU trace showed none
// of 1.07 million packets preempted; a waited resume ran 96 µs after the
// running packet completed). The next frame's heavy GPU work starts at
// r − A, its GPU readiness minus its Reflex GPU active render time (within
// −117 / +65 µs, p10 / p90, of the first long command buffer in that trace),
// so D = r − A − s is the source release → heavy GPU work time and the
// headroom of frame n's release is I − L_{n+1} + D_{n+1}. Over five probed
// sessions the late share fell to at most 1.3% from 0.5 ms of headroom and to
// at most 0.5% from 0.75 ms, while the margin to the next Present call that
// did the same ranged from 1.5 to 3.5 ms by scene. The GPU queue wait lead is
// therefore at most I + Q10(D) − H, H the learned headroom; without current
// Reflex GPU active times the Present-call guard above applies. Without GPU
// Wait Probes (no waited resumes) H stays at its start, as M does. In
// a replay of 14 sessions H = 1.0 ms cut late resumes from 1,659 to 523 (nine
// pre-guard sessions) and from 619 to 103 (five recent ones) with about as
// many frames reaching the GPU wait late (14.5 / 12.8% against 13.1 / 12.9%).
constexpr double LEAD_WORK_QUANTILE = 0.10;        // low quantile of D
// A heavy-work window without a new sample for this many GPU queue wait
// frames is stale (Reflex reports stopped): the Present-call guard applies.
constexpr uint32_t LEAD_WORK_STALE_FRAMES = 64;

// Queue order. Both guards assume the next frame's GPU work can
// run while frame n's GPU waits at its gate, so it can occupy the engine when
// the Signal comes. Some games run it behind the wait instead (one queue, in
// order): then it cannot delay the resume, and the hold delays it (the
// carried delay). The test: frame n's GPU waited at the Signal o_n (probe),
// frame n + 1 had begun submitting (RENDERSUBMIT_START <= o_n), and frame n +
// 1's GPU work started at g_{n+1} (its Reflex gpuRenderStartTime through the
// stable clock map). g_{n+1} < o_n − LEAD_CONCURRENCY_TOLERANCE_US shows
// concurrent work. Rogue Core: 72-94% of such pairs in four captures,
// guarded or not, from the first or second pair. CONTROL Resonant: none of
// 3,661 pairs in three captures (g − o at least 38 µs), and with the guard
// off (Output override GPU wait) 2,309 waited frames above its limit had no
// late resume while jitter fell from 1.91 to 1.04 ms. The
// guards therefore stand aside only once LEAD_WARM_SAMPLES of the newest
// LEAD_CONCURRENCY_SAMPLES pairs exist and none was concurrent; one
// concurrent pair restores them at once. Without probes or Reflex reports no
// pair exists and the guards stay. Under the guard D cannot show this: the
// next frame's heavy work queues behind the guarded release, so Q10(D) rises
// with the lead (CONTROL: p10 4.7 ms guarded, 5.5 ms at +1.7 ms, 8.1 ms at
// +4.6 ms lead) and never shows the room.
constexpr size_t LEAD_CONCURRENCY_SAMPLES = 64;
// Clock map spread (about 6 µs) and probe mapping; Rogue Core's concurrent
// pairs start p50 0.7-0.9 ms before the Signal.
constexpr double LEAD_CONCURRENCY_TOLERANCE_US = 100.0;

// Bounds of a learned collision distance: the release margin (to the next
// Present call) or the release headroom (to the next frame's heavy GPU work).
struct CollisionDistanceLimits {
    double start_us;
    double min_us;
    double max_us;
};
constexpr CollisionDistanceLimits LEAD_MARGIN_LIMITS{ LEAD_MARGIN_START_US, LEAD_MARGIN_MIN_US, LEAD_MARGIN_MAX_US };
// 1.0 ms to start: 0.75 ms gave more lead in the replay but more late
// resumes in 8 of 14 sessions, 0.5 ms more than four times as many late
// resumes as the 2.5 ms release margin.
constexpr CollisionDistanceLimits LEAD_HEADROOM_LIMITS{ 1000.0, 750.0, 3000.0 };

// Hold room (Adaptive cap): a GPU queue wait frame the guard
// judged reaches its gate before its guarded release once the interval is at
// least R = X − (limit − I), X minus Q10(D) − H (or Q10(C) − M). The period
// governor keeps the interval at least at the Adaptive cap percentile of R
// (period_governor.h). Across the release falls of 20 sessions the time from
// the gate to the next frame's heavy GPU work moved 0.93 µs per µs of
// interval while A and D moved −0.01 and +0.06: R does not grow with the
// interval it buys. A frame late even against its unguarded lead is the
// overload's evidence, not the hold's, and adds no R.

// Lockstep engines (CONTROL Resonant): the game finishes
// frame n's render submission and Present only after ReLimiter releases
// frame n + 1 (the next release gates the previous frame's Present), so
// frame n's Present follows ReLimiter's waits for later frames. Those waits
// are not the game's time. A later frame k gated frame n when it was
// released no later than frame n's Present-side time (PRESENT_START, the
// native arrival or the Present call); its waits (the source wait and a
// pre-Sleep wait) inside [s_n, that time] are frame n's gated hold. The
// late-output check (source_stage.cpp) and the Adaptive cap's slack evidence
// judge frame n without it; the lead keeps the measured X, because a lockstep
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
// for a later release, above), and the carried delay starts at w or later.
// The slack keeps X: q does not change when the frame was ready.
// FG input wait only. The FG input wait's back-pressure comes from its own
// GPU gate, and its output point lies a GPU frame after the Present call.
// Other rungs keep X:
// - On source only (FG unqualified) q was the game's own pipeline
//   depth (p50 16 ms on half the frames), and the PRESENT_START output point
//   sits right after it. With q removed, the late-output check's gated-hold
//   forgiveness shrank (p50 29.1 / 19.6 → 7.3 ms) and the grid restarted 292
//   times per 1,000 frames, against none with q kept in X.
// - H1 and H2 already remove ReLimiter's own CPU hold (the overlap rule), so
//   the queue left over is the game's as well.
// - On the GPU queue wait (CONTROL with FG off), late resumes concentrated on
//   queued frames (38.5% against 4.8%); that output keeps X unless the
//   developer A/B key gpu_queue_wait_render_queue says otherwise (Remove, or
//   RemoveHeld: only after a frame whose GPU waited at its gate, the
//   condition for a delay ReLimiter's own hold caused). Rogue Core never
//   queues (w before e): q was 0 on every frame of five captures.

// Render handoff offset. A frame
// that is not held is released no earlier than A_n = w_{n−1} + d, where
// w_{n−1} is RENDERSUBMIT_START of the newest released frame: a pipelined
// game then cannot simulate frames that only wait for its render thread
// (CONTROL Resonant FG 2× with the Reflex Sleep bypassed: the
// release of frame n came 28.6 ms before PRESENT_END(n−1)). Each frame n
// released at or after w_{n−1} adds h = (b_{n−1} − w_{n−1}) − (e_n − s_n): how
// much later it could have been released and still ended its simulation
// before the render thread left frame n−1's Present. d = max(0, Q_{1−q}(h) −
// allowance) with the Smoothing lead's q and allowance, 0 until
// LEAD_HANDOFF_MIN_SAMPLES. Every rung samples, so d is warm when holds
// pause. A frame released before w_{n−1} is skipped: its simulation may have
// waited at the handoff. Serial engines (Rogue Core, w before e) give h < 0
// and d = 0, so the limit never binds there.

// Render handoff GPU floor. The offset d keeps the simulation from waiting
// for the render thread, but the render thread can itself be held back by
// the GPU inside its window, where d cannot see it. Gears of War: E-Day FG
// 2× in the overload pause (GPU-bound): when the render thread started frame
// n more than about 11 ms before the GPU finished frame n − 1, the driver
// often received the first of frame n's work only near the end of frame
// n − 1 on the GPU, and the run of held-back frames that followed started
// with a stretched GPU frame (a stutter). The driver's Sleep releases the
// game at the GPU's own rate (simulation starts 17.12 ms apart, p5–p95 spread
// 0.70 ms, GPU period 17.09 ms) and starts the render thread 9.4 ms before
// the previous GPU end: never held back, no long frame. An offset from the
// render thread's handoff only sets how fast that start drifts against the
// GPU, and a term learned from held-back frames found the edge by hitting
// it, one stutter per run. The floor is measured instead: frame n (not
// held) is released no earlier than
//   Ĝ(n − 1) − (P/2 + F + σ),
// so its render thread starts P/2 + F before the GPU finishes frame n − 1
// and its first work reaches the driver half a GPU frame before then:
// midway between none of the frame queued ahead of the GPU (the GPU would
// wait) and all of it (a frame ahead). P is the median GPU frame period
// between the GPU ends of consecutive frames the GPU did not wait for (at
// most LEAD_HANDOFF_GPU_IDLE_US idle before them), F the median delay from
// RENDERSUBMIT_START to the frame's first driver submission over pairs that
// stream (below), σ the median time from a release to its RENDERSUBMIT_START
// over frames not held; newest 256 each, LEAD_WARM_SAMPLES before use. Gears:
// 17.09 / 2 + 0.85 = 9.4 ms, the driver's position. A game whose render
// thread starts later than that against the GPU (CONTROL Resonant 3.9 ms,
// Rogue Core 4.4 ms) is never delayed. Ĝ(n − 1) is the later of
// - the newest Reflex report's gpuRenderEndTime extrapolated to frame n − 1
//   by P (reports reach ReLimiter about two frames after the GPU end, so
//   this spans about four frames), and
// - the render thread's newest PRESENT_END (live) plus the
//   LEAD_HANDOFF_GPU_BOUND_QUANTILE of the reports' PRESENT_END → GPU end
//   gap, extrapolated the same way: a lower bound that sees a frame the GPU
//   held back two frames before its report does. In Gears the gap was
//   13.8–19 ms whether or not the frame was held back; with the bound the
//   prediction's p1 error fell from −8.5 to −4.9 ms.
// The floor applies while the newest report is of the same producer epoch
// and at most LEAD_HANDOFF_GPU_HORIZON_FRAMES before frame n − 1 and the
// windows are warm. In a model of four-frame-old reports it held the start
// at sd 0.58 ms around the target; a free-running cadence at P with
// proportional-integral phase correction reached sd 0.85 ms and fell 2–4
// ms further toward the edge after a 0.5 ms GPU step.
// Readiness anchor (developer A/B gpu_paced_cadence only): the GPU waits read
// back each frame's GPU readiness on ReLimiter's own GPU timeline (the FG
// input prefix, or the GPU queue wait's pre-gate query), long before its
// Reflex report arrives. Its gap to the frame's Reflex GPU end is nearly
// constant (Gears, FG 2×: 1.53 ms, sd 0.12, frame generation included). When
// a frame newer than the newest report has its readiness and output outcome
// known, Ĝ(n − 1) is that frame's anchor, its readiness or, when the GPU
// wait was queued, the later of its readiness and the release, plus the
// median anchor → GPU end gap of the newest LEAD_GPU_READY_GAP_CAPACITY
// frames, extrapolated by P: an estimate rather than a bound, so it replaces
// the older report's extrapolation, and the PRESENT_END bound applies on top
// of it only from a newer frame. Every frame adds a gap from that same
// anchor: the GPU waits report a queued wait as held whether or not the GPU
// reached it first (Gears, FG 2× GPU-paced: 97.5% reached it after the
// release), so gaps of frames not held alone never warmed the window.
//
// GPU need (developer A/B gpu_paced_hold_margin): the GPU time D from the
// release of frame n − 1's output (its anchor above) to frame n's GPU
// readiness, taken when frame n's anchor replaces n − 1's. It is frame
// generation and Present of n − 1 plus frame n's render, with the game's own
// submission waits but without ReLimiter's own holds (the anchor of a held
// frame is its release): a period that included the holds would feed the
// hold margin's interval back into itself. Only consecutive frames of one
// producer epoch count, D above LEAD_LONG_SAMPLE_INTERVALS intervals is
// dropped, the newest LEAD_GPU_NEED_CAPACITY values form the window and a
// different FG divisor slot restarts it.
//
// Report pairs (frame n's Reflex report and n − 1's, driver clock): a pair
// whose first work reached the driver in the first half of the render
// thread's window shows that the game streams its submission
// (LEAD_HANDOFF_STREAM_PROOF such pairs complete the stream proof,
// LeadController::HandoffStreams) and gives one F. The period, gap, F and σ
// windows clear with the handoff window; the stream proof and the newest GPU
// end stay until a lifecycle reset.
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

// Which estimate of Ĝ(n − 1) set the GPU floor (LeadController::GpuFloor).
enum class GpuFloorAnchor : uint8_t {
    None = 0,       // the floor does not apply
    Report = 1,     // the newest Reflex report's GPU end
    PresentEnd = 2, // the render thread's newest PRESENT_END bound
    Ready = 3       // the newest frame's GPU readiness (readiness anchor)
};
struct GpuFloorEstimate {
    int64_t floor_qpc = 0; // 0: the floor does not apply
    GpuFloorAnchor anchor = GpuFloorAnchor::None;
    uint32_t frames_ahead = 0; // frames from the anchor's frame to n − 1 (extrapolated by P)
};

// ── Inputs and status ──

// The collision guard's verdict on one lead (NextLeadUs).
struct LeadGuardOutcome {
    bool judged = false;       // a GPU queue wait lead the guard judged (C or D window warm)
    double offset_us = 0.0;    // the limit minus I: Q10(D) − H, or Q10(C) − M
    double reduction_us = 0.0; // how much the guard lowered L (>= 0)
};

// The owner's source release of one planned frame (plan fields plus s).
struct LeadFrameStart {
    PacingPlanKey key{};
    uint64_t grid_index = 0;
    PacingRung rung = PacingRung::SourceOnly;
    double fg_divisor = 1.0;
    double interval_us = 0.0;      // the plan's grid interval I
    int64_t source_release_qpc = 0; // s
    double lead_us = 0.0;          // the plan's lead L; slack = L − X. Finite, >= 0
    // Frames outside Vrr and Fixed are tracked for matching but never sampled.
    PacingTimingMode mode = PacingTimingMode::Vrr;
    // H3: the plan's GPU hold reports a post-wait resume probe (u), so the
    // next frame waits for it instead of estimating it from o.
    bool gpu_resume_probe = false;
    // Developer A/B (gpu_queue_wait_render_queue): whether a GPU queue wait
    // frame's lead sample leaves out the simulation → render queue (above).
    // The FG input wait always does; other rungs never.
    PacingRenderQueue render_queue = PacingRenderQueue::Keep;
    // Developer A/B gpu_paced_cadence: the GPU floor set this frame's release
    // (source_stage.h), so its X measures how soon a frame released there is
    // ready (EarlyLeadUs).
    bool gpu_floor_release = false;
    // The collision guard's verdict on L (NextLeadUs). The Adaptive cap's
    // slack is judged against L + guard.reduction_us, so lateness the guard
    // accepts on purpose is not a missed target; a judged frame adds its
    // hold room X − guard.offset_us.
    LeadGuardOutcome guard{};
    // ReLimiter's waits for this frame before s: the source wait
    // [wait_start_qpc, wait_end_qpc] and a pre-Sleep wait [pre_wait_start_qpc,
    // pre_wait_end_qpc]; 0 when there was none. They gate earlier frames of a
    // lockstep engine (above).
    int64_t wait_start_qpc = 0;
    int64_t wait_end_qpc = 0;
    int64_t pre_wait_start_qpc = 0;
    int64_t pre_wait_end_qpc = 0;
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
    double gpu_wait_cap_us = 0.0; // GPU queue wait: warm target at most median X + this; 0 = uncapped
    // GPU queue wait: L at most I + Q10(D) − release headroom once 32 D
    // samples exist and are current, else I + Q10(C) − release margin once 32
    // C samples exist; neither while the next frame's GPU work runs behind
    // the wait (queue order, above).
    bool collision_guard = false;
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
};

// Per-bucket feasibility evidence for the overload guard.
struct LeadFeasibility {
    uint32_t samples = 0;        // slack samples in the window (at most LEAD_SLACK_CAPACITY)
    uint32_t negative_count = 0; // of those, frames with X > L
    bool budget_limited = false; // the bucket's last lead decision was budget-limited
};

struct LeadStatistics {
    uint64_t samples = 0;            // samples added to a bucket
    uint64_t excluded_long = 0;      // samples above 3 I, not added
    uint64_t budget_limited_frames = 0; // lead decisions whose warm target exceeded the budget
    uint64_t gpu_wait_capped_frames = 0; // warm GPU queue wait targets lowered by gpu_wait_cap_us
    // Collision guard: GPU queue wait leads it judged (C or D window warm)
    // and of those, leads it lowered; the headroom guard judged
    // headroom_checked_frames of them, the Present-call guard the rest.
    uint64_t collision_checked_frames = 0;
    uint64_t collision_guarded_frames = 0;
    uint64_t headroom_checked_frames = 0;
    double last_collision_limit_us = 0.0; // the newest judged limit: I + Q10(D) − H or I + Q10(C) − M
    bool last_guard_headroom = false;     // the newest judged lead used the headroom guard
    // Release margin evidence (waited resumes with the next Present call
    // known) and the learner's steps.
    uint64_t margin_samples = 0;
    uint64_t margin_late_samples = 0;
    uint64_t margin_widened = 0;
    uint64_t margin_narrowed = 0;
    // Release headroom: D samples (GPU queue wait frames with r and a Reflex
    // GPU active time), Reflex GPU active times without a pending frame,
    // waited resumes with the next frame's heavy GPU work start known, and
    // the learner's steps.
    uint64_t work_samples = 0;
    uint64_t unmatched_gpu_active = 0;
    uint64_t headroom_samples = 0;
    uint64_t headroom_late_samples = 0;
    uint64_t headroom_widened = 0;
    uint64_t headroom_narrowed = 0;
    // Queue order (above): waited releases paired with the next frame's GPU
    // start, those whose next frame's GPU work started before the release
    // (concurrent), and the newest g_{n+1} − o_n.
    uint64_t concurrency_samples = 0;
    uint64_t concurrent_samples = 0;
    double last_concurrency_us = 0.0;
    uint64_t room_samples = 0;       // hold room samples R (judged GPU queue wait frames not late against L + reduction)
    // Slack samples judged without the gated hold of later frames (lockstep
    // engines), and the newest such hold.
    uint64_t gated_hold_samples = 0;
    double last_gated_hold_us = 0.0;
    uint64_t restarted_buckets = 0;  // buckets restarted by RestartRung (holds resuming)
    uint64_t incomplete_dropped = 0; // planned frames closed without their own stage times
    uint64_t unmatched_events = 0;   // stage events without a matching planned frame
    uint64_t missing_ready = 0;      // H3 frames closed without r
    // Probed held frames not added to the resume history: the GPU reached
    // the Wait after the Signal (u − o is GPU backlog), or the pre-Wait time is unknown.
    uint64_t resume_not_waited = 0;
    // Waited u − o observations (the GPU waited at the Signal), and of those,
    // resumes later than LEAD_RESUME_LATE_US: the GPU wait cap's evidence.
    uint64_t resume_waited_samples = 0;
    uint64_t resume_late_samples = 0;
    uint64_t invalid_inputs = 0;     // rejected frequency, requests, releases and events
    // GPU-rung samples shortened by the delay earlier GPU holds carried
    // through a busy GPU, and the newest such delay.
    uint64_t carried_hold_samples = 0;
    double last_carried_hold_us = 0.0;
    // Lead samples shortened by the game's simulation → render queue q
    // (above: FG input wait, and GPU queue wait under the A/B key), and the
    // newest such q.
    uint64_t queued_samples = 0;
    double last_queue_us = 0.0;
    // Render handoff samples h (above) and the newest one.
    uint64_t handoff_samples = 0;
    double last_handoff_us = 0.0;
    double last_sample_us = 0.0;     // newest sample added to a bucket
    double last_slack_us = 0.0;      // newest slack L − X, long samples included
    // Slack samples (one per frame whose readiness was measured, long ones
    // included) and of those, frames ready after their target (L − X < 0):
    // the Adaptive cap's output evidence.
    uint64_t slack_samples = 0;
    uint64_t negative_slack_samples = 0;
};

// ── Release margin and headroom ──

// Learns a collision distance: the collision edge (the lowest distance whose
// late-resume share matches the background) plus LEAD_MARGIN_CUSHION_US,
// within the limits. The controller runs two: the release margin M, measured
// to the next Present call (p_{n+1} − o_n), and the release headroom H,
// measured to the next frame's heavy GPU work (r_{n+1} − A_{n+1} − o_n).
// Decayed per-bin counts of waited resumes; colliding bins reached upward
// from the edge widen it at once. Each narrowing step needs the bin just
// below the edge to stay, for LEAD_MARGIN_NARROW_SAMPLES resumes, either clean
// or (above the start) with fewer than LEAD_MARGIN_MIN_LATE weighted late
// resumes: widening is quick, a widening fades over minutes with its late
// resumes. Without evidence (no probes) the distance stays at its start.
class ReleaseMarginLearner {
public:
    explicit ReleaseMarginLearner(CollisionDistanceLimits limits = LEAD_MARGIN_LIMITS) noexcept
        : limits_(limits), margin_us_(limits.start_us) {}
    void Reset() noexcept;
    // One waited resume: margin_us = the distance from the release to the
    // collision reference, late = u − o > LEAD_RESUME_LATE_US.
    // Returns +1 when the distance widened, −1 when it narrowed, else 0.
    int Add(double margin_us, bool late) noexcept;
    double MarginUs() const noexcept { return margin_us_; }
    double EdgeUs() const noexcept { return margin_us_ - LEAD_MARGIN_CUSHION_US; }

private:
    static size_t BinOf(double margin_us) noexcept;
    bool Colliding(size_t bin, double background) const noexcept;

    CollisionDistanceLimits limits_;
    std::array<double, LEAD_MARGIN_BINS> resumes_{};
    std::array<double, LEAD_MARGIN_BINS> late_{};
    double margin_us_;
    uint32_t narrow_run_ = 0;
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
    // Enforcement marker change: buckets cleared. Frames already released
    // produce no sample but still decontaminate the next frame.
    void ResetBuckets() noexcept;
    // Every bucket's slack window: an overload-guard evidence restart (a
    // governor step or reset, or a lifecycle, cap, FG divisor or route
    // change), or an infeasible-lead miss consumed by the Adaptive cap. Lead
    // windows, budget flags and the resume history stay.
    void ClearSlackEvidence() noexcept;
    // The rung resumes holding after an overload demotion: each of its
    // sampled buckets restarts its window, and until 32 new samples arrive
    // the lead is the old window's median plus the allowance instead of the
    // cold seed. For the GPU waits that base is at least the warm source-only
    // Q_q (at its last request) of the same FG divisor, which the demoted
    // frames kept current. Other rungs, the resume history and statistics stay.
    void RestartRung(PacingRung rung) noexcept;
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
    // minus I and how much it lowered the lead.
    double NextLeadUs(const LeadRequest& request, bool* budget_limited = nullptr,
        LeadGuardOutcome* guard = nullptr) noexcept;
    void OnSourceRelease(const LeadFrameStart& frame) noexcept;
    void OnStageEvent(const PacingStageEvent& event) noexcept;
    // The Reflex latency report of NvAPI frame (producer_epoch, frame_id):
    // its GPU active render time A and, through the stable clock map, the
    // QPC of its gpuRenderStartTime (0 unknown; the queue-order test).
    // Pairs with the pending frame of that key (reports arrive about three
    // frames after the source release); a report without one is counted and
    // dropped.
    void OnGpuActiveTime(uint32_t producer_epoch, uint64_t frame_id, double gpu_active_us,
        int64_t gpu_start_qpc = 0) noexcept;
    // The Reflex report pair of NvAPI frame (producer_epoch, frame_id) and
    // its previous frame (above): every pair counts toward the stream proof
    // and, if it streams, its first-submission delay toward F.
    void OnReportPair(uint32_t producer_epoch, uint64_t frame_id,
        const HandoffBackPressureSample& sample) noexcept;
    // NvAPI frame (producer_epoch, frame_id)'s gpuRenderStartTime and
    // gpuRenderEndTime as QPC through the stable Reflex clock map (end 0
    // unknown: ignored) and its PRESENT_END → GPU end gap in µs on the
    // driver's clock (non-finite or not positive: not counted). The GPU
    // floor's anchor; consecutive frames the GPU did not wait for (start at
    // most LEAD_HANDOFF_GPU_IDLE_US after the previous end) add their interval
    // to the period window, every frame its gap to the gap window. A new
    // producer epoch restarts all three; older frames are ignored.
    void OnGpuRenderEnd(uint32_t producer_epoch, uint64_t frame_id, int64_t gpu_start_qpc,
        int64_t gpu_end_qpc, double present_to_gpu_end_us) noexcept;

    LeadBucketStatus BucketStatus(PacingRung rung, double fg_divisor) const noexcept;
    // Median sample X of a warm bucket (release to output point); 0 when the
    // bucket is cold or unknown. The source slack of a plan is L minus this
    // value.
    double MedianSampleUs(PacingRung rung, double fg_divisor) const noexcept;
    // The early side of the lead, max(0, Q_{1−q}(X) − allowance_us), the
    // mirror of L = Q_q + allowance: how soon after its release a frame is
    // ready at the earliest. The GPU-paced cadence's output target
    // (source_stage.h). Once 32 frames of the rung's output class (GPU waits
    // or CPU outputs) and FG divisor were released by the GPU floor, the X of
    // the newest 128 of them (exact lower-index quantile): the bucket's own
    // samples are of every release, and on the FG input wait leave out the
    // simulation → render queue, so their early side lies far below where a
    // frame released at the floor is ready (Gears of War: E-Day, FG 2× at
    // 120: the cadence never followed the GPU). Before that the bucket's
    // early side, or while a restarted window is cold (RestartRung) the old
    // window's median minus the allowance; 0 for a cold bucket without one,
    // an unknown bucket, or a non-finite q or allowance.
    double EarlyLeadUs(PacingRung rung, double fg_divisor, double quantile, double allowance_us) const noexcept;
    // Hold room: Q_q of R over the newest 256 hold-room samples of the FG
    // divisor, the interval at which a share q of the judged GPU queue wait
    // frames would have reached the GPU wait before their guarded release;
    // 0 until 32 samples, for a non-finite q, after a reset, and while the
    // queue order shows the next frame's GPU work behind the wait.
    double HoldRoomUs(double fg_divisor, double quantile) const noexcept;
    // Render handoff offset d (above): max(0, Q_{1−q}(h) − allowance_us) over
    // the newest 256 samples, 0 before LEAD_HANDOFF_MIN_SAMPLES or for a
    // non-finite q or allowance.
    double HandoffOffsetUs(double quantile, double allowance_us) const noexcept;
    // The GPU floor (above): how long before the previous frame's GPU end the
    // render thread starts, P/2 + F; 0 until the period and F windows are warm.
    double GpuFloorStartUs() const noexcept;
    // The GPU floor of the frame key (above), with the render thread's newest
    // PRESENT_END of key's stream (frame and QPC; 0 unknown) for the live
    // bound and, with ready_anchor (developer A/B gpu_paced_cadence), the
    // readiness anchor; floor_qpc 0 when it does not apply (another identity
    // domain or producer epoch, no GPU end within the horizon, a cold
    // window). The estimate names the anchor that set Ĝ(n − 1).
    GpuFloorEstimate GpuFloor(const PacingPlanKey& key, uint64_t present_end_frame = 0,
        int64_t present_end_qpc = 0, bool ready_anchor = false) const noexcept;
    // GPU frame periods (above): the median P of the newest 256 (0 until
    // LEAD_WARM_SAMPLES), and the mean of the newest LEAD_GPU_PERIOD_RECENT
    // (0 until that many): the GPU-paced rhythm's period (source_stage.h).
    double GpuPeriodUs() const noexcept;
    double RecentGpuPeriodUs() const noexcept;
    // GPU need window (above): the lower-index quantile sorted[floor(q (n − 1))]
    // of the newest D, 0 until LEAD_WARM_SAMPLES of them.
    double GpuNeedQuantileUs(double q) const noexcept;
    uint32_t GpuNeedCount() const noexcept { return gpu_need_.count; }
    // Anchor → GPU end gaps in the readiness anchor's window (above).
    uint32_t ReadyGapCount() const noexcept { return ready_gap_count_; }
    // The stream proof is complete (above).
    bool HandoffStreams() const noexcept { return handoff_stream_frames_ >= LEAD_HANDOFF_STREAM_PROOF; }
    // Handoff samples in the window. At LEAD_HANDOFF_MIN_SAMPLES the game has
    // shown its render thread taking a frame before the next release, which
    // a render thread that waits for the next release never does.
    uint32_t HandoffSamples() const noexcept { return handoff_window_.Count(); }
    // Frame (key, grid_index)'s gated hold in QPC ticks (lockstep engines,
    // above): the waits of later pending frames of its lifecycle released no
    // later than its Present-side time, inside [s, that time]; 0 when the
    // frame or that time is unknown.
    int64_t GatedHoldTicks(const PacingPlanKey& key, uint64_t grid_index) const noexcept;
    // The pending frame (key, grid_index) was released by the GPU floor
    // (LeadFrameStart::gpu_floor_release); false when it is not pending.
    bool GpuFloorReleased(const PacingPlanKey& key, uint64_t grid_index) const noexcept;
    // Slack window and last budget decision of the bucket; empty for an invalid rung.
    LeadFeasibility Feasibility(PacingRung rung, double fg_divisor) const noexcept;
    LeadStatistics Statistics() const noexcept { return statistics_; }
    // The learned release margin M (LEAD_MARGIN_START_US until evidence) and
    // release headroom H (LEAD_HEADROOM_LIMITS.start_us until evidence).
    // Kept across Reset: they describe the GPU scheduler and the game's GPU
    // work, not the lifecycle.
    double ReleaseMarginUs() const noexcept { return margin_.MarginUs(); }
    double ReleaseHeadroomUs() const noexcept { return headroom_.MarginUs(); }
    // Queue order (above): the collision guards stand aside while the newest
    // pairs show the next frame's GPU work running behind the wait. Kept
    // across Reset, like M and H: it describes the game's GPU submission.
    bool NextGpuWorkBehindWait() const noexcept;
    size_t ConcurrencyPairs() const noexcept { return concurrency_count_; }
    size_t ConcurrentPairs() const noexcept { return concurrent_in_window_; }

private:
    static constexpr size_t kBucketCount =
        static_cast<size_t>(PacingRung::Count) * LEAD_DIVISOR_BUCKETS;

    // Stage times of a pending frame; 1..9 match PacingStageEventKind.
    enum Field : uint8_t {
        FieldSource = 0,
        FieldMarkerArrival,
        FieldNativeArrival,
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
        // GPU rungs: s + X, the readiness without the delay earlier GPU
        // holds carried into this frame; 0 until the sample is emitted.
        int64_t own_ready_qpc = 0;
        double interval_us = 0.0;
        double lead_us = 0.0;      // the plan's L, for the slack
        double guard_reduction_us = 0.0; // the collision guard's reduction of L
        double guard_offset_us = 0.0;    // its limit minus I (guard_judged)
        bool guard_judged = false;       // a judged GPU queue wait lead: adds a hold-room sample
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
        bool finalized = false; // sample emitted or given up
        bool closed = false;    // expired: no longer waited on
        bool gpu_resume_probe = false;
        PacingRenderQueue render_queue = PacingRenderQueue::Keep; // GPU queue wait A/B
        bool resume_delay_recorded = false;
        bool gpu_floor_release = false; // LeadFrameStart::gpu_floor_release
        bool resume_waited = false; // the GPU waited at the Signal (w <= o)
        bool resume_late = false;   // and resumed more than LEAD_RESUME_LATE_US after it
        bool call_recorded = false;   // C entered the call window
        bool margin_recorded = false; // p_{n+1} − o_n entered the margin learner
        double gpu_active_us = 0.0;   // Reflex GPU active render time A; 0 unknown
        bool work_recorded = false;     // D = r − A − s entered the work window
        bool headroom_recorded = false; // r_{n+1} − A_{n+1} − o_n entered the headroom learner
        int64_t gpu_start_qpc = 0;      // Reflex gpuRenderStartTime as QPC; 0 unknown
        bool concurrency_recorded = false; // (o_n, g_{n+1}) entered the queue-order window
        bool release_to_render_recorded = false; // σ of this frame entered its window
    };

    // The newest LEAD_SLACK_CAPACITY slack signs of one bucket.
    struct SlackWindow {
        std::array<uint8_t, LEAD_SLACK_CAPACITY> negative{}; // 1 when X > L
        uint32_t head = 0; // next write position; the oldest sample once full
        uint32_t count = 0;
        uint32_t negative_count = 0;
    };

    struct Bucket {
        LeadSampleWindow window;
        SlackWindow slack;
        uint64_t excluded_long = 0;
        double quantile_us = 0.0;
        double target_us = 0.0;
        double lead_us = 0.0;
        double budget_us = 0.0;
        bool warm_applied = false;
        bool budget_limited = false;
        double reseed_x_us = 0.0; // X base before a RestartRung (median, GPU waits floored): the cold lead's base; 0 = the seed
    };

    static bool FindBucketIndex(PacingRung rung, double fg_divisor, size_t* index) noexcept;
    static void ResetBucket(Bucket* bucket) noexcept;
    static void AddSlack(SlackWindow* window, double slack_us) noexcept;
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
    // Readiness anchor (above): the newest NvAPI frame with its GPU readiness
    // and output outcome known, and each frame's anchor → GPU end gap when its
    // report arrives.
    static int64_t ReadyAnchorQpc(const PendingFrame& frame) noexcept;
    void NoteReadyAnchor(const PendingFrame& frame) noexcept;
    void RecordReadyGap(uint32_t producer_epoch, uint64_t frame_id, int64_t gpu_end_qpc) noexcept;
    void RecordGpuNeed(const PendingFrame& frame) noexcept;
    // Copies the newest `count` u − o observations (count <= resume_delay_count_).
    void NewestResumeDelays(size_t count, double* delays_us) const noexcept;
    double MedianResumeDelayUs() const noexcept;
    // Collision guard
    void RecordCallTime(PendingFrame* frame) noexcept;
    void RecordReleaseMargin(PendingFrame* frame) noexcept;
    void RecordWorkStart(PendingFrame* frame) noexcept;
    void RecordReleaseHeadroom(PendingFrame* frame) noexcept;
    // σ of a frame not held (GPU floor, above).
    void RecordReleaseToRender(PendingFrame* frame) noexcept;
    void RecordConcurrency(PendingFrame* frame) noexcept;
    double CollisionGuardedLeadUs(const LeadRequest& request, size_t bucket_index, double lead_us,
        LeadGuardOutcome* guard) noexcept;

    // Sample assembly
    bool PreviousResolved(const PendingFrame& frame) const noexcept;
    void TryFinalize(PendingFrame* frame) noexcept;
    void ForceFinalize(PendingFrame* frame) noexcept;
    void EmitSample(PendingFrame* frame) noexcept;
    double SampleUs(const PendingFrame& frame, const PendingFrame* previous,
        int64_t* own_ready_qpc = nullptr, double* carried_us = nullptr) const noexcept;
    // The game's simulation → render queue q (above), in µs: FG input wait,
    // and GPU queue wait per its render_queue; 0 for other rungs or without
    // e, w, p and the previous frame's b.
    double QueueUs(const PendingFrame& frame, const PendingFrame* previous) const noexcept;
    // Render handoff sample h of frame n (above), when frame n and its
    // previous frame carry s, e and w, b, and frame n was released at or after
    // w_{n−1}.
    void RecordHandoff(const PendingFrame& frame, const PendingFrame* previous) noexcept;
    // Lockstep engines: waits of later frames that gated this one, in ticks.
    int64_t GatedHoldTicks(const PendingFrame& frame) const noexcept;
    // The frame's CPU Present-side event: PRESENT_START (H0, H1), the native
    // arrival (H2) or the Present call (GPU rungs).
    static Field PresentSideField(PacingRung rung) noexcept;
    bool PreviousCpuHold(const PendingFrame* previous, int64_t origin_qpc,
        double* begin_us, double* end_us) const noexcept;
    bool PreviousGpuResume(const PendingFrame* previous, int64_t origin_qpc,
        double* resume_us) const noexcept;
    // The previous GPU-rung frame's GPU waited at its gate (probe w <= o, else
    // held and ready before the Signal): only then can it carry a delay.
    static bool PreviousGpuHeld(const PendingFrame& previous) noexcept;
    double MicrosecondsBetween(int64_t from_qpc, int64_t to_qpc) const noexcept;

    // Requests
    double ValidQuantile(double quantile) noexcept;
    double ValidAllowance(double allowance_us) noexcept;

    std::array<Bucket, kBucketCount> buckets_{};
    std::array<PendingFrame, LEAD_PENDING_CAPACITY> pending_{};
    std::array<double, LEAD_RESUME_DELAY_CAPACITY> resume_delays_us_{};
    size_t resume_delay_head_ = 0;
    size_t resume_delay_count_ = 0;
    // Collision guard: release → Present call times C and release → heavy
    // GPU work times D of GPU queue wait frames per FG divisor, the GPU queue
    // wait releases since each work window's newest sample, the release
    // margin and the release headroom; the hold room R per FG divisor.
    std::array<LeadSampleWindow, LEAD_DIVISOR_BUCKETS> call_windows_{};
    std::array<LeadSampleWindow, LEAD_DIVISOR_BUCKETS> work_windows_{};
    std::array<uint32_t, LEAD_DIVISOR_BUCKETS> work_age_frames_{};
    std::array<LeadSampleWindow, LEAD_DIVISOR_BUCKETS> room_windows_{};
    // Render handoff samples h (above), all rungs and FG divisors.
    LeadSampleWindow handoff_window_{};
    // The render handoff GPU floor (above): pairs that showed streaming
    // submission (saturates at LEAD_HANDOFF_STREAM_PROOF); the newest GPU end
    // (producer epoch, NvAPI frame, QPC); and, in µs, the GPU frame periods P
    // of frames the GPU did not wait for, the PRESENT_END → GPU end gaps, the
    // first-submission delays F of streaming pairs and the release →
    // RENDERSUBMIT_START times σ of frames not held.
    uint32_t handoff_stream_frames_ = 0;
    uint32_t gpu_end_epoch_ = 0;
    uint64_t gpu_end_frame_ = 0;
    int64_t gpu_end_qpc_ = 0;
    LeadSampleWindow gpu_period_window_{};
    LeadSampleWindow gpu_after_present_window_{};
    LeadSampleWindow first_submission_window_{};
    LeadSampleWindow release_to_render_window_{};
    // GPU-paced rhythm: the newest LEAD_GPU_PERIOD_RECENT periods of the
    // period window, in arrival order.
    std::array<double, LEAD_GPU_PERIOD_RECENT> recent_periods_us_{};
    uint32_t recent_period_head_ = 0;
    uint32_t recent_period_count_ = 0;
    // Readiness anchor (above): producer epoch, NvAPI frame and QPC of the
    // newest anchor (ReadyAnchorQpc), and the anchor → GPU end gaps in µs.
    uint32_t ready_epoch_ = 0;
    uint64_t ready_frame_ = 0;
    int64_t ready_qpc_ = 0;
    std::array<float, LEAD_GPU_READY_GAP_CAPACITY> ready_gaps_us_{};
    uint32_t ready_gap_head_ = 0;
    uint32_t ready_gap_count_ = 0;
    // GPU need window (above); divisor = the frames' FG divisor slot.
    struct GpuNeedWindow {
        std::array<float, LEAD_GPU_NEED_CAPACITY> us{};
        uint32_t head = 0;
        uint32_t count = 0;
        size_t divisor = 0;
    };
    GpuNeedWindow gpu_need_{};
    // GPU-paced cadence: X of frames the GPU floor released (EarlyLeadUs),
    // [0] GPU waits, [1] CPU outputs; divisor = the bucket's FG divisor slot.
    struct GpuPacedRing {
        std::array<float, LEAD_GPU_PACED_CAPACITY> x_us{};
        uint32_t head = 0;
        uint32_t count = 0;
        size_t divisor = 0;
    };
    std::array<GpuPacedRing, 2> gpu_paced_rings_{};
    ReleaseMarginLearner margin_{};
    ReleaseMarginLearner headroom_{ LEAD_HEADROOM_LIMITS };
    // Queue order: the newest LEAD_CONCURRENCY_SAMPLES pairs, 1 = concurrent.
    std::array<uint8_t, LEAD_CONCURRENCY_SAMPLES> concurrency_{};
    size_t concurrency_head_ = 0;
    size_t concurrency_count_ = 0;
    size_t concurrent_in_window_ = 0;
    int64_t qpc_frequency_ = 0;
    bool lifecycle_known_ = false;
    PacingPlanKey lifecycle_{}; // identity domain, producer epoch and generation of the newest release
    uint64_t newest_index_ = 0;
    LeadStatistics statistics_{};
};
