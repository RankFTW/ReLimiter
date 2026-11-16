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
// 176 KB (20 buckets of a 256-sample ring and a 2,048-bin histogram, four
// release → Present call windows, four release → heavy GPU work windows, the
// first-submission window and the start term's two windows of the same shape,
// plus the pending-frame table): keep the instance in static storage or on the
// heap, never on a thread stack.

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
//             whatever their predecessor but an ordered one) − H:
// the form and the headroom of the heavy-work guard, with the interval
// corrected by what was measured and D taken from evidence that exists in this
// state. A minimum is set by the least delayed sample, so the D of a frame that
// queued behind its predecessor's GPU work (larger) does not raise it; if all
// are delayed the limit falls to about the fastest recent readiness minus H and
// nothing is held. A frame after a predecessor planned with present-first
// ordering (below) adds none: its heavy work waited for that Present, so its D
// carries the order's delay (every D of an ordered stretch does, and so does
// their minimum), and a limit built from it would be looser than the heavy
// work allows. The ring is kept per regime like the heavy-work window. The
// lateness is the game's arrival against its planned
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
// A pair whose frame n has a GPU start and no GPU render end in its own report is no pair: the driver lost
// that frame's completion, and the start it then stamps on frame n + 1 is not a measurement of that frame's
// work. In ordered runs (11 runs, 40 pairs that brought the guard back) it was so for 38: the next frame's
// reported start lay 1 to 6 ms before the release, the held frame's own readiness, where the queue probe saw
// that frame's first start 0.5 to 3.3 ms after it (the two others were at hitches); about 0.2% of reports
// lack the end, and where they are complete the next frame's start is 0.27 to 0.47 ms after the release,
// never before. Such a pair enters no window, of either output or regime, and is counted
// (LeadStatistics::concurrency_end_missing); the missing end is the mark, no limit on a time decides it.
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

// Present-first ordering (developer key gpu_wait_present_first, default on; gpu_hold_order.h; design in
// project-docs/002_unified_pacing_timing/foundation/design_present_first_hold.md). The queue order above is
// inferred from the pairs, and a game whose render queue is not its presenting queue (Rogue Core through
// Streamline's proxy) fails it: the next frame's heavy work runs beside the hold, a hold that ends inside
// that work is displayed late, and no limit on the lead separates safe holds from unsafe ones, because when
// that work starts depends on when frame n was ready. The unit imposes the order instead: the render
// queue's next frame waits until a held frame's Present has become runnable. Whether it does for a frame is
// that frame's plan (LeadRequest::ordered, carried as LeadGuardOutcome::ordered and PendingFrame::ordered),
// and it is a fact about the next frame's work: frame n + 1's heavy work runs behind frame n's gate wait
// exactly when frame n was ordered. So the readers at plan time (the guard, the spike floor, the
// diagnostics) take the plan being made, and the readers at frame time that ask whether a frame's work ran
// behind its predecessor's gate wait (the lead sample's carried wait, SampleUs, and the wait for the frame's
// report, LeadSampleWaitsForReport) take the predecessor's plan, whatever the frame's own says: at a flip of
// the interleave's periods or of the unit's able word a frame is judged by the regime of the work it ran
// behind.
// For an ordered plan BehindWait is true while the Reflex reports are current (one has arrived since the last
// reset, work_report_seen_, because the age reads 0 there, which is no report, and the work age is within
// LEAD_WORK_STALE_FRAMES; the heavy-work window need not be warm, the order is imposed, not learned) and the
// pairs of ordered frames show no concurrent one (an empty window does not veto): the guard stands aside and
// the next frame's lead sample leaves out the wait carried from this gate, as in every game that runs behind
// the wait. A concurrent pair among ordered frames brings the guard back at once and keeps it until that pair
// leaves the newest LEAD_CONCURRENCY_SAMPLES, as for the built pairs. Evidence is kept per regime: the pair
// (o_n, g_{n+1}) belongs to frame n, the frame whose gate wait the next frame's GPU start is compared with,
// and goes to the ordered window when frame n's plan was ordered, to the built one otherwise; a D measured
// after an ordered predecessor is no sample of the heavy-work window and none of the late state's ring. So
// the built guard in the periods without ordering is what it is alone, and clean ordered pairs cannot put it
// aside where no order is imposed. Known and accepted for the first build: the carried wait is measured to
// the gate's pass, not to the order's release, so the lead may learn up to the Present's service time (0.3
// to 0.75 ms) too much. Reset and ResetBuckets clear the ordered pairs with the windows, and nothing else
// does: a window left clean by an earlier regime cannot put the guard aside by itself, because a plan is
// ordered only while the unit is able to order (g_gpu_hold_order_able), and a concurrent ordered pair vetoes
// it until that pair leaves the newest LEAD_CONCURRENCY_SAMPLES.

// Start term (developer key gpu_wait_start_limit, default on; design in
// project-docs/002_unified_pacing_timing/foundation/design_start_gate.md). The built guard above keeps a hold
// from ending in the next frame's heavy GPU work; it says nothing about the game's own flow control, and where it
// stands aside (ordered plans, games whose next GPU work runs behind the wait) nothing did. The game's Present
// does not end (b_{n+1}, the PRESENT_END marker) before the presenting queue has processed the previous frame's
// Present (x_n), and the game asks for its next frame right behind that end (a_{n+2} − b_{n+1}, its arrival at
// ReLimiter). So the start of frame n + 2 cannot come before o_n plus the Present's service (x_n − max(o_n, r_n))
// plus that turnaround, while its start target is s_n + 2 I: a lead above 2 I less those two times makes the game
// late for frame n + 2, and the start law never catches a late release up. A hold that does this on every second
// frame is a saw-tooth in the frame-time graph (Rogue Core at a cap, with the ordered plans' lead at the
// controller's own). The term is a second limit of the same guard, in the same function, through the same slew back
// and the same channel to the executor (PacingFramePlan::lead_limit_us), for every GPU queue wait plan with the
// guard on, ordered or not, whether or not the built guard judged it:
//   limit = 2 I − (S99 + G99),   L <= limit + the plan's anchor delta (the built limit adds it the same way).
// S99 is the 99th percentile of the newest LEAD_WINDOW_CAPACITY Present services x − max(o, r) of held frames: x is
// the hold order unit's stamp (PacingStageEventKind::PresentProcessed) for every held frame it follows, whatever the
// lead. G99 is that of the newest turnarounds a_{n+1} − b_n of every frame, held or not; 0 while there is none. Both
// are measured on frames the limit does not remove, so neither can go stale behind its own effect; the direct sample
// a_{n+2} − o_n could, because a limit built from it removes the frames it learns from (the latch the release
// headroom above records). Samples above LEAD_LONG_SAMPLE_INTERVALS intervals are in neither window, and a service
// below 0 is none. S99 is used once LEAD_WARM_SAMPLES services exist and one arrived among the newest
// LEAD_WORK_STALE_FRAMES frames planned with the GPU queue wait's lead; until then, and where nothing stamps
// services (the key off, Win32, one queue), the release headroom H (LeadRequest::release_headroom_us) stands in:
// the fallback, named so in the statistics and the log.
// 2 I is exact from below at a cap (the interval cannot fall under the cap's). Where the GPU term sets the interval
// it moves from frame to frame and one start can be late by what it fell, which is one late start, not the
// alternation; the limit follows the plan's own interval, so it moves at once when the interval does. When frame
// n + 1 is itself late the limit is stricter than needed, which is safe.
// The term stands aside while the game's own times contradict either link of that chain:
// - A free pair: a frame n with status Held and o_n > p_{n+1} (the next frame's native Present arrived before the
//   release; only such a pair can tell), with b_{n+1} < o_n: the game's Present ended while the frame before was
//   still held, so the game does not wait for a held Present. One among the newest LEAD_START_EVIDENCE_FRAMES telling
//   pairs.
// - An untied start: a_{n+1} < b_n in one of the newest LEAD_START_EVIDENCE_FRAMES frames: the game asks for a frame
//   before its previous Present ended (a game thread running a frame ahead), so the frame a hold reaches is a later
//   one and the term does not describe it.
// Without evidence the term applies: its limit is reached only by a lead already 1.8 intervals long, a game that does
// not wait lifts the term by its own pairs as soon as its lead passes I + C, and a game that does wait is protected
// from its first frame. A tighter limit applies at once, a relaxed one (the evidence returned, the interval grew) is
// returned to at the lead slew, the built limit's rule.
// Each sample and pair is judged once, by whichever callback completes it (the release of the later frame or a stage
// event, in either order), between frames of one lifecycle with consecutive grid indices, both planned with the GPU
// queue wait's rung, one FG divisor and a mode that samples; no pair bridges a gap. A pair and a service need the
// status Held explicitly: the built guard counts Unknown as held (LeadGate), a status this term cannot use, since the
// unit follows the frames the executor held. a is the arrival the owner passes with the release (LeadFrameStart::
// arrival_qpc), b the PresentEnd event of the frame, o and p its OutputRelease and PresentCall events. Reset and
// ResetBuckets clear the windows and rings and fence the frames released before them, so evidence that arrives later
// adds nothing to the new ones. The windows and rings are fed with the key off too; only the limit reads it.
constexpr double LEAD_START_QUANTILE = 0.99;      // S99 and G99
constexpr size_t LEAD_START_EVIDENCE_FRAMES = 64; // newest telling pairs and newest frames the stand-aside reads

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
    // Present-first ordering (LeadRequest::ordered): the plan was made on it. Kept with the frame
    // (PendingFrame::ordered): its pair, its successor's D and its successor's lead sample take that regime.
    bool ordered = false;
    // The limit on the release → target distance, in µs with the anchor's delta not in it, that the executor
    // resolves no target past (PacingFramePlan::lead_limit_us): the lower of the built guard's, I + offset when
    // judged, while developer key gpu_wait_late_guard is on, and the start term's while it was applied (below);
    // 0 when neither. A start limit at or below 0 holds nothing (the lead is 0) and sets none.
    double release_limit_us = 0.0;
    // Start term (above), GPU queue wait plans with the guard on and LeadRequest::start_limit or start_observe:
    // the limit computed for the plan, 2 I − (S + G) in µs on the release → target distance (the anchor's delta not
    // in it), at least 0; 0 where the term stood aside or was not computed.
    double start_limit_us = 0.0;
    // The term ruled the plan (LeadRequest::start_limit, not standing aside): the returned lead is at most
    // start_limit_us plus the anchor's delta. With a limit of 0 it means a limit clamped at 0.
    bool start_applied = false;
    // The term stood aside for the plan: a free pair or an untied start among the newest evidence.
    bool start_aside = false;
    // The service in the limit is the measured S99; false: the release headroom, the fallback.
    bool start_measured = false;
};

// One Reflex latency report's GPU times for a frame (LeadController::OnGpuReport).
struct LeadGpuReport {
    double active_us = 0.0; // gpuActiveRenderTimeUs: the frame's GPU busy time A (> 0)
    double span_us = 0.0;   // gpuRenderEndTime − gpuRenderStartTime on the driver's clock; 0 unknown
    int64_t start_qpc = 0;  // gpuRenderStartTime as QPC through the stable clock map; 0 unknown
    // gpuRenderEndTime as QPC through the stable clock map; 0 unknown. A report that has a start and no end is
    // a frame whose completion the driver lost (OnGpuReport, RecordConcurrency).
    int64_t end_qpc = 0;
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
    // Start term (above): the game's arrival at ReLimiter for this frame, a: the Reflex Sleep hook's arrival where
    // the unified pacer reserved the start, else the marker's, before any wait of ReLimiter's own. 0 unknown, and an
    // arrival later than s is none; a frame without one gives no turnaround and no untied-start evidence. The
    // legacy planning branch has only its planning site's time, read after the lead's decision and before the source
    // wait: the marker's arrival plus what the plan took up to that read.
    int64_t arrival_qpc = 0;
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
    // gpu_wait_ab_interleave_s): LeadGuardOutcome::late_seen.
    bool late_observe = false;
    // Present-first ordering (developer key gpu_wait_present_first, default on; above): the render queue's
    // next frame waits for this frame's Present. The owner sets it from the key, the interleave's period and
    // the unit's able word (g_gpu_hold_order_able) and hands the same word to the gate as the plan's
    // (PacingFramePlan::ordered). On the GPU queue wait the collision guard then stands aside while the Reflex
    // reports are current and the pairs of ordered frames show none concurrent, whatever the built evidence
    // says. Readers at frame time (the lead sample's queue order) take the predecessor's plan,
    // PendingFrame::ordered of frame n − 1; no other rung reads it.
    bool ordered = false;
    // Start term (above; developer key gpu_wait_start_limit): whether the term may set the limit on the GPU queue
    // wait's lead with the guard on. start_observe computes the limit and the verdict and reports them
    // (LeadGuardOutcome::start_limit_us, start_aside) without lowering the lead (the off periods of developer key
    // gpu_wait_ab_interleave_s); start_limit wins when both are set. With neither the term does not run.
    bool start_limit = false;
    bool start_observe = false;
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
    // Present-first ordering (above), GPU queue wait: the same pairs of the frames planned ordered, which the
    // GPU queue wait's counts above leave out.
    uint64_t ordered_concurrency_samples = 0;
    uint64_t ordered_concurrent_samples = 0;
    double last_ordered_concurrency_us = 0.0;
    // Pairs left out because the waited frame's own report had a GPU start and no GPU render end (the driver
    // lost its completion): in no window, of either output or regime (RecordConcurrency).
    uint64_t concurrency_end_missing = 0;
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
    // Start term (above), GPU queue wait plans with the guard on and the term's key on or observing: plans it
    // computed a limit for, those among them it stood aside for or ran on the fallback service (the release
    // headroom), and the plans it applied to whose returned lead its limit lowered (the limit below the lead the
    // built guard left); the changes from applying to standing aside and back; the telling pairs, free pairs and
    // untied starts judged (each once), and the Present services and turnarounds added to the windows or left out.
    uint64_t start_checked_frames = 0;
    uint64_t start_aside_frames = 0;
    uint64_t start_fallback_frames = 0;
    uint64_t start_lowered_frames = 0;
    uint64_t start_stood_aside = 0;
    uint64_t start_came_back = 0;
    uint64_t start_telling_pairs = 0;
    uint64_t start_free_pairs = 0;
    uint64_t start_untied_starts = 0;
    uint64_t start_service_samples = 0;
    uint64_t start_service_excluded = 0;    // a service below 0 or above 3 I
    uint64_t start_turnaround_samples = 0;
    uint64_t start_turnaround_excluded = 0; // a turnaround above 3 I (a negative one is an untied start)
    // The newest computed plan: its limit (also while standing aside), the service and turnaround in it, whether
    // the service was measured and whether it stood aside.
    double last_start_limit_us = 0.0;
    double last_start_service_us = 0.0;
    double last_start_turnaround_us = 0.0;
    bool last_start_measured = false;
    bool last_start_aside = false;
};

// The start term's evidence now (above): the windows' sample counts, S99 and G99 as they stand (0 while a window is
// empty), whether the service is the measured one, and the counts of the newest telling pairs and frames.
struct LeadStartEvidence {
    uint32_t service_samples = 0;
    uint32_t turnaround_samples = 0;
    double service_us = 0.0;
    double turnaround_us = 0.0;
    bool service_measured = false;
    size_t telling_pairs = 0; // newest telling pairs held, at most LEAD_START_EVIDENCE_FRAMES
    size_t free_pairs = 0;    // of them, free
    size_t frames = 0;        // newest frames judged for an untied start, at most LEAD_START_EVIDENCE_FRAMES
    size_t untied_starts = 0; // of them, untied
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
    // statistics cleared, the pairs of ordered frames (present-first
    // ordering, above) and the start term's windows and rings; the built
    // queue-order pairs stay.
    // A non-positive frequency is counted and disables sampling until the next Reset.
    void Reset(int64_t qpc_frequency) noexcept;
    // Enforcement marker change: buckets, C, D, F, the pairs of ordered
    // frames, the start term's windows and rings and the spike floor cleared.
    // Frames already released produce no sample but still decontaminate the
    // next frame; their later events add nothing to C or D (measured from the
    // old release point) nor to the start term's windows. F still takes the
    // reports that arrive afterwards.
    void ResetBuckets() noexcept;
    // One call per planned frame; applies at most one slew step to the
    // selected bucket and clamps L to [0, 3 I]. Present placement returns
    // 150 µs; None placement and Off or Idle mode return 0 without touching a
    // bucket. *budget_limited (optional) is true when the warm target
    // Q_q + allowance exceeded the budget; cold, seed and non-bucket
    // leads are never budget-limited. With request.collision_guard, a GPU
    // queue wait lead is at most I + Q10(D) − H once 32 current D samples
    // exist, else I + Q10(C) − M once 32 C samples exist, unless the queue
    // order shows the next frame's GPU work behind the wait (not judged; for
    // a request that is ordered, present-first ordering above: the Reflex
    // reports are current and no concurrent pair among the ordered frames);
    // the bucket's slew continues from the unguarded lead, and *guard
    // (optional) receives the guard's verdict: whether it judged the lead, its
    // limit minus I and how much it lowered the lead (the lead it returns
    // slews up to the bucket's lead once the limit relaxed; the reduction is
    // lead − returned, the start term's included), request.ordered of a GPU
    // queue wait plan, and the start term's limit and verdict. With
    // request.start_limit a GPU queue wait lead is also at most the start
    // term's limit plus the anchor's delta (above) unless the game's own times
    // make it stand aside, whether or not the built guard judged or stood
    // aside; a tighter limit applies at once, a relaxed one is returned to at
    // the slew.
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
    // submission, not the lifecycle. For the GPU queue wait the verdict is the
    // regime of the newest plan (present-first ordering, above): the pairs of
    // its ordered frames with the reports seen since the last reset and the
    // work age, or the built pairs.
    bool NextGpuWorkBehindWait(PacingRung rung, double fg_divisor) const noexcept;
    size_t ConcurrencyPairs(PacingRung rung) const noexcept;
    size_t ConcurrentPairs(PacingRung rung) const noexcept;
    // Present-first ordering (above): the GPU queue wait's pairs of frames planned ordered, and those of
    // them concurrent. ConcurrencyPairs and ConcurrentPairs count the others.
    size_t OrderedConcurrencyPairs() const noexcept;
    size_t OrderedConcurrentPairs() const noexcept;
    // The start term's evidence now (above).
    LeadStartEvidence StartEvidence() const noexcept;

private:
    static constexpr size_t kBucketCount =
        static_cast<size_t>(PacingRung::Count) * LEAD_DIVISOR_BUCKETS;

    // Stage times of a pending frame; 1..10 match PacingStageEventKind.
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
        FieldPresentProcessed, // x: the hold order unit saw the held frame's Present processed (GPU queue wait)
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
        bool ordered = false;            // planned ordered: its pair, its successor's D and sample are that regime's
        bool late_work_recorded = false; // its D entered the late state's ring, or was given up
        bool gpu_end_missing = false;    // its report had a GPU start and no GPU render end (LeadGpuReport::end_qpc)
        // Start term (above): the game's arrival a for this frame (LeadFrameStart::arrival_qpc; 0 unknown) and what
        // has been decided about it, once each: its Present's service, its pair with its successor (telling or not)
        // and its successor's turnaround after its own PRESENT_END. ResetBuckets decides all three for the frames
        // released before it, so their evidence adds nothing afterwards.
        int64_t arrival_qpc = 0;
        bool service_recorded = false;
        bool pair_recorded = false;
        bool turnaround_recorded = false;
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
    // Start term (above). A held frame's Present service x − max(o, r), its pair with the next frame (telling, free)
    // and the turnaround a_{n+1} − b_n with the sign of an untied start: each judged once, by whichever release or
    // stage event completes it, so every callback that may have asks.
    void RecordStartService(PendingFrame* frame) noexcept;
    void RecordStartPair(PendingFrame* frame) noexcept;
    // S99 is in use: LEAD_WARM_SAMPLES services and one among the newest LEAD_WORK_STALE_FRAMES frames planned with
    // the GPU queue wait's lead; else the release headroom stands in.
    bool StartServiceMeasured() const noexcept;
    // The term for a GPU queue wait plan (collision guard on, LeadRequest::start_limit or start_observe): computes
    // the limit and the verdict, reports them in *guard (optional) and the statistics, and returns true with
    // *bound_us, the limit plus the anchor's delta, when the term limits the plan (start_limit, not standing aside).
    bool StartTerm(const LeadRequest& request, LeadGuardOutcome* guard, double* bound_us) noexcept;
    void ResetStartTerm() noexcept;
    double CollisionGuardedLeadUs(const LeadRequest& request, size_t bucket_index, double lead_us,
        LeadGuardOutcome* guard) noexcept;
    // The guard's limit minus I for an FG divisor slot: Q10(D) − headroom_us
    // while the heavy-work window is warm and current (*headroom true), else
    // Q10(C) − LEAD_RELEASE_MARGIN_US while the call window is warm; false
    // when neither is.
    bool CollisionOffsetUs(size_t divisor, double headroom_us, double* offset_us, bool* headroom) const noexcept;
    // The heavy-work window is warm (LEAD_WARM_SAMPLES D samples) and the Reflex reports of frames planned with
    // the GPU queue wait's lead are current (the work age, LEAD_WORK_STALE_FRAMES).
    bool WorkCurrent(size_t divisor) const noexcept;
    // Late state (above): true with *offset_us = least arrival lateness + least D − H while the newest
    // LEAD_LATE_FRAMES planned frames all arrived late and as many current D exist; else false, *offset_us kept.
    bool LateOffsetUs(size_t divisor, const LeadRequest& request, double* offset_us) const noexcept;
    // Queue order (above) of a GPU output. The two-argument form takes the regime of the newest GPU queue wait
    // plan (ordered_), the plan-time readers' (the guard, the spike floor, the diagnostics); the
    // three-argument form the regime the caller names. SampleBehindWait is the frame-time readers': the
    // frame's lead sample and its wait for its report take the regime of its predecessor, the plan whose gate
    // wait the frame's GPU work ran behind (PendingFrame::ordered of frame n − 1; built without one).
    bool BehindWait(PacingRung rung, size_t divisor) const noexcept;
    bool BehindWait(PacingRung rung, size_t divisor, bool ordered) const noexcept;
    bool SampleBehindWait(const PendingFrame& frame, const PendingFrame* previous) const noexcept;
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
    // Whether a Reflex report of a frame planned with the GPU queue wait's lead has arrived since the last reset
    // (Reset, ResetBuckets), per FG divisor. The age above reads 0 after a reset, which is no report: the built
    // verdict also needs a warm heavy-work window, but an ordered plan's does not (BehindWait), so it asks for a
    // report actually seen.
    std::array<bool, LEAD_DIVISOR_BUCKETS> work_report_seen_{};
    // Late state (above), per FG divisor: the arrival lateness of the newest
    // planned GPU queue wait frames and the D of the newest reported ones
    // (none after an ordered predecessor).
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
    // Present-first ordering (above): the pairs of the GPU queue wait's frames planned ordered, kept apart from
    // queue_order_[0], and whether the newest GPU queue wait plan was ordered (what BehindWait's two-argument
    // form and so the plan-time readers take).
    QueueOrderWindow ordered_order_{};
    bool ordered_ = false;
    // Start term (above): the Present services of held frames and the turnarounds of every frame, the frames planned
    // with the GPU queue wait's lead since the newest service (their age, current within LEAD_WORK_STALE_FRAMES),
    // the newest telling pairs (1 = free), the newest frames judged for an untied start (1 = untied), and whether
    // the newest computed plan stood aside (the changes are counted).
    struct EvidenceRing {
        std::array<uint8_t, LEAD_START_EVIDENCE_FRAMES> flags{};
        size_t head = 0;
        size_t count = 0;
        size_t set = 0; // entries that are 1

        void Reset() noexcept { *this = EvidenceRing{}; }
        void Push(bool flag) noexcept
        {
            uint8_t& slot = flags[head];
            if (count == flags.size()) set -= slot;
            else ++count;
            slot = flag ? 1 : 0;
            set += slot;
            head = (head + 1) % flags.size();
        }
    };
    LeadSampleWindow start_service_window_{};
    LeadSampleWindow start_turnaround_window_{};
    uint32_t start_service_age_frames_ = LEAD_WORK_STALE_FRAMES + 1;
    EvidenceRing start_pairs_{};
    EvidenceRing start_untied_{};
    bool start_aside_ = false;
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
