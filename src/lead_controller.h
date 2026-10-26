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
// 160 KB (20 buckets of a 256-sample ring and a 2,048-bin histogram, four
// release → Present call windows, four release → heavy GPU work windows and
// four hold-room windows of the same shape, plus the pending-frame table):
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
// A waited u − o above this is a late GPU resume: measured and shown (OSD,
// telemetry), never acted on.
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
// the newest LEAD_GPU_READY_GAP_CAPACITY signed gaps from the GPU readiness
// of a frame whose pace the GPU set to its Reflex GPU end.
constexpr size_t LEAD_GPU_PERIOD_RECENT = 5;
constexpr size_t LEAD_GPU_READY_GAP_CAPACITY = 64;
// GPU-paced cadence, held floor: the cap bound's evidence is the newest
// LEAD_CAP_BOUND_FRAMES reported held frames (two 64-bit words; cap bound,
// below).
constexpr uint32_t LEAD_CAP_BOUND_FRAMES = 128;
// Adaptive cap hold room of the FG input wait (scheduler.cpp
// UpdatePeriodGovernor): the newest LEAD_GPU_NEED_CAPACITY per-frame GPU
// needs D (GpuNeedQuantileUs).
constexpr size_t LEAD_GPU_NEED_CAPACITY = 128;
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
constexpr double LEAD_RELEASE_HEADROOM_US = 1000.0; // H, the default of gpu_wait_headroom_us
constexpr double LEAD_WORK_QUANTILE = 0.10;        // low quantile of D
// A heavy-work window without a new sample for this many GPU queue wait
// frames is stale (Reflex reports stopped): the Present-call guard applies.
constexpr uint32_t LEAD_WORK_STALE_FRAMES = 64;

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

// Hold room (Adaptive cap): the same inequality as the guard, solved for the
// interval. A judged GPU queue wait frame is held at least h before its
// guarded release once X + h <= L = I + (limit − I), so the interval it needs
// is R = X + h − (Q10(D) − H) (or − (Q10(C) − M)). The period governor keeps
// the interval at least at the Adaptive cap percentile of R over the newest
// 256 judged frames (period_governor.h): Q_fit(X) + h − the guard's current
// offset, so the room uses the guard's own distances, never older ones. h is
// a minimum hold: at 0.5-1 ms of distance holds under 250 µs were 3.9% late
// against at most 0.45% for longer ones. Across the release falls of 20
// sessions the time from the gate to the next frame's heavy GPU work moved
// 0.93 µs per µs of interval while A and D moved −0.01 and +0.06. A frame
// late even against its unguarded lead is the overload's evidence, not the
// hold's, and adds no X to the room.
constexpr double LEAD_ROOM_MIN_HOLD_US = 250.0; // h

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
// for a later release, above), and the wait for frame n − 1 behind the wait
// (SampleUs) starts at w or later.
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
// warm target, max(Q_q + allowance, L_spike): the slew, S, the pre-Sleep
// target and the GPU floor's cap bound follow one lead. It needs no load
// threshold: where the arrival tail is thin it lies below the lead (Gears
// 1.4–9.7 ms below in every capture, CONTROL's light scene within 0.3 ms).
// It is not part of the 3 I budget the Adaptive cap answers, and it did not
// slow the game's render submission (CONTROL p50 7.8–8.0 ms with and without
// it). Reset and ResetBuckets clear it; ResetGpuFloor, RestartRung and
// ClearSlackEvidence keep it (game properties, like F and σ).

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
// most LEAD_HANDOFF_GPU_IDLE_US idle before them) and whose pace the GPU set
// (GpuSetPace below, both frames of the pair), F the median delay from
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
// GpuSetPace classifies a frame as paced by the GPU: it reached ReLimiter's
// GPU wait after its release (the probe's pre-wait time, else readiness,
// later than the Signal) or had no ReLimiter hold at all. A frame whose GPU
// waited at the Signal ends its GPU work after ReLimiter's own Signal, and
// the next frame's work queued behind that wait ends after it, so only pairs
// of frames the GPU set feed P and RecentGpuPeriodUs, and only such frames
// feed the PRESENT_END → GPU end gap: otherwise P measured the cadence
// ReLimiter set and the floor followed it (Rogue Core 130 with an Adaptive
// cap stretch: 83–89 fps at a 9.5 ms interval, median P 11.77 ms against
// 7.4 ms of GPU work). The classifier never switches the floor off for a
// frame: as a per-frame choice between two release depths it read the
// floor's own early target as GPU lateness and alternated every 2–3 frames
// (CONTROL FG 2× at 130: 214 switches in 48 s, SIMULATION_START intervals
// of 23 then 7–8 ms, 171 display jumps per 1,000; with the cadence off 24%
// of the frames already read GPU-set there).
//
// Cap bound (held frames, developer A/B gpu_paced_cadence; GpuFloorCap): the
// floor of a frame the cap holds is at most the cap's own release of it plus
// the GPU's lateness beyond the output's slack:
//   min(Ĝ(n − 1) − (P/2 + F + σ), Ĝ(n − 1) − ḡ + I − L − slack),
// ḡ the median readiness gap (below), so Ĝ(n − 1) − ḡ is frame n − 1's
// predicted anchor; L the cap's lead (the newest plan's, GpuFloorCap::
// lead_us); slack the output spacing slack, half the grid's late tolerance
// of I. With frame n − 1 held at its target the bound is
// S(n) − slack + k (P − I), k the frames Ĝ extrapolates, and P comes only
// from frames the GPU set, so ReLimiter's holds never move it later and the
// floor cannot follow them. The floor's own depth, P/2 + F + σ, is Gears'
// operating point: in a game whose cap runs a deeper queue it sat 13.8 ms
// past S (CONTROL FG 2× at 130: the render thread started 7.6 ms before
// G(n − 1) on the floor against 14.7 ms on the cap, a frame less queued,
// simulation 7.4 / 20 ms p50 / p90 against 4.4 / 8.9). L is the plan's
// own, not a lead measured from frames the floor did not release: such a
// measurement stops once the floor releases every frame, and the cap's lead
// then rose past it (CONTROL FG 2× at 130: L 21 → 31–35 ms within 5 s, the
// measured median stayed at 20–24 ms, so the bound sat 5–7 ms past S before
// any lateness, behind the floor's own 4–5.5 ms, and the floor released
// 92–94% of frames).
// Which depth applies (CapBound): the newest LEAD_CAP_BOUND_FRAMES reported
// held GPU-rung frames whose wait is known, each set when its GPU waited at
// its Signal (a probe at or before it, else readiness before it) and its
// plan did not move its target (LeadFrameStart::gpu_target), whoever
// released it. Bounded once LEAD_WARM_SAMPLES of them are set, the floor's
// own depth (Depth) once the window is full and none is; between the two
// the mode stays. Before either the floor applies at its own depth only
// after a cadence restart of a held frame in this producer epoch
// (NoteCadenceRestart: SourceLate or OutputLate), else not at all
// (GpuFloorEstimate::ineligible); bounded, it also needs ḡ warm and the
// cap's lead, else no floor. Frames the floor released count: evidence from
// frames it did not release is what its own depth removes. The cumulative
// count of those this replaced (32 in the producer epoch bound it for good)
// fell from 61–78% of the held frames to 5–7% in CONTROL Resonant (FG 2× at
// 130) under the own depth, where the waits at an unmoved Signal stayed at
// 50–63% (64–79% bounded, 62–71% without the cadence); Gears of War: E-Day
// (FG 2× at 120) has 7–8% with the cadence alone at either depth and
// 93–97% with the Adaptive cap. Bound for good, Gears' render thread started
// L − 19.7 ms before the GPU's need on the frames the floor released and
// its held-first work followed the lead (850 frames per minute at L 32 ms).
// The exit is at none of the newest 128: at 16 CONTROL switched 12 times in
// one capture; at none, replayed on 24 captures, CONTROL left the bound once
// (0.9 s, every frame late and its GPU work above I) and Gears switched 2–6
// times per capture at its section changes. The floor is
//   Ĝ(n − 1) − (P/2 + F + σ) − φ Δ,
// Δ the gap from the floor's own depth back to the cap bound (0 when the
// bound lies past it or ḡ or the cap's lead is unknown), φ the blend: 1 the
// cap bound, 0 the own depth. After each published plan of a held frame
// whose floor applied (AdvanceCapBound) φ moves toward its mode by at most
// min(LEAD_SLEW_MAX_US, I/64) / Δ, so a switch moves the floor like a lead
// change rather than by Δ at once (CONTROL about 7 ms, Gears 2–4 ms); Δ
// itself moves with its inputs. A held frame without a floor (not eligible,
// or not known) was released at the cap's own S, so φ returns to 1, where
// it also starts after a reset: a floor that applies again starts at the
// bound and reaches the own depth only by those steps. Without a known
// bound (readiness gaps still cold) there is nothing to start from, and a
// restart applies the own depth at once, as it always has. Keeping φ
// through such a stretch let the floor return at a depth no release had
// moved to: after a restart had applied the own depth, a bound waiting for
// warm readiness gaps (CONTROL after an FG change: 10 s and more) came back
// at the own depth once they warmed. Frames not held (the render handoff
// limit) take no bound.
//
// Readiness anchor (as Ĝ, developer A/B gpu_paced_cadence only): the GPU
// waits read back each frame's GPU readiness on ReLimiter's own GPU timeline
// (the FG input prefix, or the GPU queue wait's pre-gate query), long before
// its Reflex report arrives. Its gap to the frame's Reflex GPU end is nearly
// constant, and its sign depends on the wait's place in the frame: the GPU
// end follows the readiness on the FG input wait (frame generation runs
// after the prefix; Gears, FG 2×: +1.45 ms, p10–p90 55 µs) and precedes it
// on the GPU queue wait when the game's measured GPU work ends before the
// Present (Rogue Core −0.21 ms, p10–p90 42 µs; Gears without FG −0.34 ms).
// When a frame newer than the newest report has its readiness and output
// outcome known, Ĝ(n − 1) is that frame's anchor, its readiness or, when
// the GPU wait was queued, the later of its readiness and the release, plus
// the median gap of the newest LEAD_GPU_READY_GAP_CAPACITY frames,
// extrapolated by P: an estimate rather than a bound, so it replaces the
// older report's extrapolation, and the PRESENT_END bound applies on top of
// it only from a newer frame. Only frames whose pace the GPU set add a gap,
// within one interval either side of 0: a frame whose GPU waited at the
// Signal anchors at the release, and on the GPU queue wait its measured GPU
// end does not follow that release, so its gap is ReLimiter's own hold
// (Rogue Core: −0.4 to −1.4 ms). The output status cannot decide it: the GPU
// waits report every queued wait as held, and frames not held alone never
// warmed the window (Gears, FG 2× GPU-paced: 97.5% reached the wait after
// the release). A gap that had to be positive rejected every GPU queue wait
// frame, so the anchor never engaged there.
//
// GPU need (the Adaptive cap hold room of the FG input wait): the GPU
// execution time of a real frame from its Reflex report, the active render
// time plus the AI frame time (LeadGpuReport::work_us, the governor's GPU
// service), of sampled FG input wait frames: execution time does not move
// with ReLimiter's cadence. The readiness gap it replaced (frame n's
// readiness − frame n − 1's anchor) did: a frame the cap holds is ready at
// its own release plus its work, so the gap was the interval less the
// frame's slack, and its quantile held whatever interval the Adaptive cap had
// reached (Gears of War: E-Day, FG 2× at 120 with GPU-paced cadence and the
// Adaptive cap at 75: gap p50/p75 20.1/21.1 ms at a 20.7 ms interval against
// 17.7/17.9 ms of GPU work; the room set the realized interval at +20–36%).
// Execution time overstates the GPU's saturated period by the frame
// generation overlap (Gears 17.93 against 17.48 ms, CONTROL Resonant +1.3%),
// which errs toward holding. Values above LEAD_LONG_SAMPLE_INTERVALS
// intervals are dropped, the newest LEAD_GPU_NEED_CAPACITY form the window,
// another FG divisor slot or producer epoch restarts it, and it reads 0
// while the slot's FG reports are stale (LEAD_WORK_STALE_FRAMES).
//
// Report pairs (frame n's Reflex report and n − 1's, driver clock): a pair
// whose first work reached the driver in the first half of the render
// thread's window shows that the game streams its submission
// (LEAD_HANDOFF_STREAM_PROOF such pairs complete the stream proof,
// LeadController::HandoffStreams) and gives one F. The period, gap, F and σ
// windows clear with the handoff window; the stream proof and the newest GPU
// end stay until a lifecycle reset. A cap or FG divisor change
// (ResetGpuFloor) clears the period, PRESENT_END gap and readiness gap
// windows, the cap-held frames and the cadence restart; F, σ and the anchors
// stay, as they describe the game rather than the cap's output. Until the new
// period window holds LEAD_WARM_SAMPLES, P is the one in use before the
// change (GpuPeriodUs): without it the floor stopped until 32 new pairs
// existed, and a GPU that limits the new cap restarted the cadence on nearly
// every frame meanwhile (CONTROL at 130 after FG was turned off: 11.4 ms of
// GPU work for a 7.69 ms interval, 27 restarts in the 0.54 s before the
// floor returned; the overload guard then took the holds away for 4–7 s).
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

// Which estimate of Ĝ(n − 1) set the GPU floor (LeadController::GpuFloor).
enum class GpuFloorAnchor : uint8_t {
    None = 0,       // the floor does not apply
    Report = 1,     // the newest Reflex report's GPU end
    PresentEnd = 2, // the render thread's newest PRESENT_END bound
    Ready = 3       // the newest frame's GPU readiness (readiness anchor)
};
// The held GPU floor's depth (cap bound, above): Cold before the evidence
// window decides, Depth the floor's own, Bounded the cap bound.
enum class CapBoundMode : uint8_t {
    Cold = 0,
    Depth = 1,
    Bounded = 2
};
struct GpuFloorEstimate {
    int64_t floor_qpc = 0; // 0: the floor does not apply
    GpuFloorAnchor anchor = GpuFloorAnchor::None; // the anchor of Ĝ(n − 1), also under the cap bound
    uint32_t frames_ahead = 0; // frames from the anchor's frame to n − 1 (extrapolated by P)
    // Held frames (GpuFloorCap): the cap bound moved floor_qpc earlier than
    // Ĝ(n − 1) − (P/2 + F + σ) (φ Δ > 0).
    bool cap_bound = false;
    // Held frames: the windows were warm but the floor does not apply: Cold
    // without a cadence restart of a held frame in this producer epoch, or
    // bounded without warm readiness gaps or the cap's lead.
    bool ineligible = false;
    // Held frames: the mode, and Δ, how far the cap bound lies before the
    // floor's own depth in µs (>= 0).
    CapBoundMode cap_mode = CapBoundMode::Cold;
    double cap_gap_us = 0.0;
};
// The cap bound's evidence window and blend (LeadController::CapBound).
struct CapBoundStatus {
    CapBoundMode mode = CapBoundMode::Cold;
    uint32_t waited = 0;   // entries whose GPU waited at a Signal their plan did not move
    uint32_t frames = 0;   // entries, at most LEAD_CAP_BOUND_FRAMES
    double blend = 0.0;    // φ
    uint64_t switches = 0; // Depth ↔ Bounded changes since the last reset
};
// A held frame's cap bound inputs (LeadController::GpuFloor, above): its
// grid interval I, the cap's lead L (the newest plan's) and the output
// spacing slack, half the grid's late tolerance of I
// (CadenceGrid_LateToleranceTicks), in µs. interval_us 0: a frame not held,
// no cap bound and no eligibility rule.
struct GpuFloorCap {
    double interval_us = 0.0;
    double lead_us = 0.0;
    double slack_us = 0.0;
};

// ── Inputs and status ──

// The collision guard's verdict on one lead (NextLeadUs).
struct LeadGuardOutcome {
    bool judged = false;       // a GPU queue wait lead the guard judged (C or D window warm)
    double offset_us = 0.0;    // the limit minus I: Q10(D) − H, or Q10(C) − M
    // How much lower the lead is than the unguarded lead (>= 0): the guard's
    // limit, and after the limit relaxed, the lead slewing back up to it.
    double reduction_us = 0.0;
};

// One Reflex latency report's GPU times for a frame (LeadController::OnGpuReport).
struct LeadGpuReport {
    double active_us = 0.0; // gpuActiveRenderTimeUs: the frame's GPU busy time A (> 0)
    double span_us = 0.0;   // gpuRenderEndTime − gpuRenderStartTime on the driver's clock; 0 unknown
    // A plus the AI frame time when the report gave execution-only evidence
    // (the governor's GPU service); 0 otherwise. The FG input wait's GPU need.
    double work_us = 0.0;
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
    // The plan's output target T (> 0): the slack is T − s plus the guard's
    // reduction, less X (LeadStatistics).
    int64_t output_target_qpc = 0;
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
    // Developer A/B gpu_paced_cadence: the frame's earliest target moved its
    // T (CadenceGridPlan::gpu_target): a wait at that Signal is no evidence
    // that the cap held it (cap bound, above).
    bool gpu_target = false;
    // The collision guard's verdict on L (NextLeadUs). The Adaptive cap's
    // slack adds guard.reduction_us, so lateness the guard accepts on
    // purpose is not a missed target; a judged frame adds its X to the hold
    // room when it is not late even so.
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
    // GPU queue wait: L at most I + Q10(D) − release_headroom_us once 32 D
    // samples exist and are current, else I + Q10(C) − LEAD_RELEASE_MARGIN_US
    // once 32 C samples exist; neither while the next frame's GPU work runs
    // behind the wait (queue order, above).
    double release_headroom_us = LEAD_RELEASE_HEADROOM_US;
    bool collision_guard = false;
    // Spike floor (above; developer key lead_spike_floor).
    bool spike_floor = true;
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

// Per-bucket feasibility evidence for the overload guard.
struct LeadFeasibility {
    uint32_t samples = 0;        // slack samples in the window (at most LEAD_SLACK_CAPACITY)
    uint32_t negative_count = 0; // of those, frames ready after their target
    bool budget_limited = false; // the bucket's last lead decision was budget-limited
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
    uint64_t room_samples = 0;       // hold room samples X (judged GPU queue wait frames not late against L + reduction)
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
    // (above: FG input wait, and GPU queue wait under the A/B key), and the
    // newest such q.
    uint64_t queued_samples = 0;
    double last_queue_us = 0.0;
    // Render handoff samples h (above) and the newest one.
    uint64_t handoff_samples = 0;
    double last_handoff_us = 0.0;
    double last_sample_us = 0.0;     // newest sample added to a bucket
    double last_slack_us = 0.0;      // newest counted slack, long samples included
    // Slack samples (one per frame whose readiness was measured, long ones
    // included) and of those, frames ready after their target: the Adaptive
    // cap's output evidence. The slack is T − s + the guard's reduction − X,
    // T the plan's output target and X the measured release → output point
    // without the gated hold. A frame released at S = T − L has L − X; a
    // source released late within the grid's tolerance and a frame the GPU
    // floor released have less than L to T. A floor-released frame's target
    // moved past the grid's only when even its earliest readiness came after
    // it, so a late one did not keep the cap's cadence (GPU-paced cadence:
    // CONTROL FG 2× at 130 85%, Rogue Core at 130 99.7% of floor-released
    // frames late; against L 78% and 99%, but in Gears of War: E-Day 1.7% at
    // a 34.5 ms lead and 93% at the 28.3 ms lead replayed for the own-work
    // sample). Frames released before the evidence restarted
    // (ClearSlackEvidence) are not counted. Draining: late frames whose
    // predecessor (the previous grid index, sample emitted) was late by
    // more. That is the queue behind one late frame running down: the
    // interval is absorbing it, so the Adaptive cap does not count it
    // (Gears of War: E-Day, a 25 ms simulation hitch left its frame and the
    // four queued behind it ready 21.7, 18.9, 17.4, 6.1 and 1.0 ms after
    // their targets, six of the newest 24 from one hitch).
    uint64_t slack_samples = 0;
    uint64_t negative_slack_samples = 0;
    uint64_t draining_late_samples = 0;
    // Spike floor (above): arrivals added, arrivals of GPU-rung frames left
    // out (first work not streamed, Signal-pinned, stage times missing or
    // beyond 3 I), GPU times added, and the newest arrival.
    uint64_t spike_samples = 0;
    uint64_t spike_excluded = 0;
    uint64_t spike_gpu_samples = 0;
    double last_spike_arrival_us = 0.0;
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
    // Enforcement marker change: buckets, C, D, σ, F and the GPU need
    // cleared, and the GPU floor's windows as at ResetGpuFloor but without
    // P. Frames already released produce no sample but still decontaminate
    // the next frame; their later events add nothing to C, D and σ (measured
    // from the old release point) or to the floor's windows. F and the GPU
    // need are the driver's and the GPU's own and still take them.
    void ResetBuckets() noexcept;
    // Every bucket's slack window: an overload-guard evidence restart (a
    // governor step or reset, or a lifecycle, cap, FG divisor or route
    // change), or an infeasible-lead miss consumed by the Adaptive cap. Lead
    // windows, budget flags and the resume history stay. Frames released
    // before it add no slack evidence; their slack still makes the next
    // frame draining.
    void ClearSlackEvidence() noexcept;
    // The rung resumes holding after an overload demotion: each of its
    // sampled buckets restarts its window, and until 32 new samples arrive
    // the lead is the old window's median plus the allowance instead of the
    // cold seed. For the GPU waits that base is at least the warm source-only
    // Q_q (at its last request) of the same FG divisor, which the demoted
    // frames kept current. Other rungs, the resume history and statistics stay.
    void RestartRung(PacingRung rung) noexcept;
    // A cap or FG divisor change (above): the GPU floor's period, PRESENT_END
    // gap and readiness gap windows, its cap-held frames and its cadence
    // restart start again (P carries over until the new period window is
    // warm); F, σ, the stream proof and the anchors stay. Reports of frames
    // released before it add nothing to those windows, and no period pairs
    // a frame from before it with one after.
    void ResetGpuFloor() noexcept;
    // The cadence restarted (SourceLate or OutputLate) on a held frame in
    // producer_epoch: the held floor's eligibility before its cap-held frames
    // (above).
    void NoteCadenceRestart(uint32_t producer_epoch) noexcept;
    bool CadenceRestartSeen(uint32_t producer_epoch) const noexcept;
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
    // its GPU active render time A, span, execution time and first-work
    // verdict, and through the stable clock map the QPC of its
    // gpuRenderStartTime (0 unknown; the queue-order test). Pairs with the
    // pending frame of that key (reports arrive about two to four frames
    // after the source release); a report without one is counted and
    // dropped. The first report of a frame wins. A lead sample behind the
    // wait of a frame the cap released is emitted here (SampleUs).
    void OnGpuReport(uint32_t producer_epoch, uint64_t frame_id, const LeadGpuReport& report) noexcept;
    // The Reflex report pair of NvAPI frame (producer_epoch, frame_id) and
    // its previous frame (above): every pair counts toward the stream proof
    // and, if it streams, its first-submission delay toward F.
    void OnReportPair(uint32_t producer_epoch, uint64_t frame_id,
        const HandoffBackPressureSample& sample) noexcept;
    // NvAPI frame (producer_epoch, frame_id)'s gpuRenderStartTime and
    // gpuRenderEndTime as QPC through the stable Reflex clock map (end 0
    // unknown: ignored) and its PRESENT_END → GPU end gap in µs on the
    // driver's clock (non-finite or not positive: not counted). The GPU
    // floor's anchor; consecutive frames the GPU set and did not wait for
    // (start at most LEAD_HANDOFF_GPU_IDLE_US after the previous end) add
    // their interval to the period window, a frame whose pace the GPU set its
    // PRESENT_END and readiness gaps, and a frame the cap held counts toward
    // the cap bound (above). A new producer epoch restarts the period and
    // PRESENT_END gap windows and the cap-held frames; older frames are ignored.
    void OnGpuRenderEnd(uint32_t producer_epoch, uint64_t frame_id, int64_t gpu_start_qpc,
        int64_t gpu_end_qpc, double present_to_gpu_end_us) noexcept;

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
    // Hold room (above): Q_q of the newest 256 hold-room samples X of the FG
    // divisor plus LEAD_ROOM_MIN_HOLD_US minus the guard's current offset at
    // release_headroom_us (LEAD_RELEASE_HEADROOM_US when not positive and
    // finite), the interval at which a share q of the judged GPU queue wait
    // frames would hold at least h before their guarded release; 0 until 32
    // samples, without a warm guard window, for a non-finite q, after a
    // reset, and while the guard stands aside (queue order).
    double HoldRoomUs(double fg_divisor, double quantile, double release_headroom_us) const noexcept;
    // Render handoff offset d (above): max(0, Q_{1−q}(h) − allowance_us) over
    // the newest 256 samples, 0 before LEAD_HANDOFF_MIN_SAMPLES or for a
    // non-finite q or allowance.
    double HandoffOffsetUs(double quantile, double allowance_us) const noexcept;
    // The GPU floor (above): how long before the previous frame's GPU end the
    // render thread starts, P/2 + F; 0 until P (GpuPeriodUs) and the F window
    // are warm.
    double GpuFloorStartUs() const noexcept;
    // The GPU floor of the frame key (above), with the render thread's newest
    // PRESENT_END of key's stream (frame and QPC; 0 unknown) for the live
    // bound, with ready_anchor (developer A/B gpu_paced_cadence) the
    // readiness anchor, and with a held frame's cap (interval_us > 0) its cap
    // bound and eligibility; floor_qpc 0 when it does not apply (another
    // identity domain or producer epoch, no GPU end within the horizon, a
    // cold window, or a held frame not eligible yet). The estimate names the
    // anchor that set Ĝ(n − 1) and whether the cap bound set the floor.
    GpuFloorEstimate GpuFloor(const PacingPlanKey& key, uint64_t present_end_frame = 0,
        int64_t present_end_qpc = 0, bool ready_anchor = false,
        const GpuFloorCap& cap = GpuFloorCap{}) const noexcept;
    // GPU frame periods (above): the median P of the newest 256 from
    // LEAD_WARM_SAMPLES; before that, after a cap or FG divisor change, the P
    // in use before it, else 0. And the mean of the newest
    // LEAD_GPU_PERIOD_RECENT (0 until that many): the GPU-paced rhythm's
    // period (source_stage.h).
    double GpuPeriodUs() const noexcept;
    double RecentGpuPeriodUs() const noexcept;
    // GPU need window (above): the lower-index quantile sorted[floor(q (n − 1))]
    // of the newest GPU execution times of fg_divisor's slot, 0 until
    // LEAD_WARM_SAMPLES of them, while the window holds another FG divisor's
    // frames, or while that slot's FG reports are stale.
    double GpuNeedQuantileUs(double q, double fg_divisor) const noexcept;
    uint32_t GpuNeedCount() const noexcept { return gpu_need_.count; }
    // Anchor → GPU end gaps in the readiness anchor's window (above).
    uint32_t ReadyGapCount() const noexcept { return ready_gap_count_; }
    // The cap bound's mode, evidence window and blend (above).
    CapBoundStatus CapBound() const noexcept;
    // After each published plan of a held frame (above): with its GPU floor
    // applied, move the blend toward the mode by at most
    // min(LEAD_SLEW_MAX_US, I/64) of gap_us, that floor's
    // GpuFloorEstimate::cap_gap_us; without one the frame was at the cap's
    // release, and the blend returns to 1.
    void AdvanceCapBound(bool floor_applied, double gap_us, double interval_us) noexcept;
    // Lower-index median of the newest LEAD_RESUME_MEDIAN_SAMPLES waited
    // u − o observations (held, probed GPU queue wait frames whose GPU waited
    // at the Signal); 0 before any. A held frame without a probe resumes at
    // o + this (behind the wait, SampleUs).
    double MedianResumeDelayUs() const noexcept;
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
        double interval_us = 0.0;
        int64_t target_qpc = 0;    // the plan's T, for the slack
        double slack_us = 0.0;     // T − s + reduction − X once the sample is emitted (slack_known)
        bool slack_known = false;
        bool slack_fenced = false; // released before the slack evidence restarted: its slack is not counted
        bool floor_fenced = false; // released before the GPU floor's windows restarted: its report adds nothing
        double guard_reduction_us = 0.0; // the collision guard's reduction of L
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
        bool finalized = false; // outcome judged (slack, rings, room) or given up
        bool lead_sampled = false; // lead window sample emitted or given up
        bool closed = false;    // expired: no longer waited on
        bool gpu_resume_probe = false;
        PacingRenderQueue render_queue = PacingRenderQueue::Keep; // GPU queue wait A/B
        bool resume_delay_recorded = false;
        bool gpu_floor_release = false; // LeadFrameStart::gpu_floor_release
        bool resume_waited = false; // the GPU waited at the Signal (w <= o)
        bool resume_late = false;   // and resumed more than LEAD_RESUME_LATE_US after it
        bool call_recorded = false;   // C entered the call window, or given up (ResetBuckets)
        bool margin_recorded = false; // p_{n+1} − o_n entered the margin learner
        double gpu_active_us = 0.0;   // Reflex GPU active render time A; 0 unknown
        double gpu_span_us = 0.0;     // Reflex GPU start → end span on the driver's clock; 0 unknown
        bool work_recorded = false;     // D = r − A − s entered the work window, or given up
        bool headroom_recorded = false; // r_{n+1} − A_{n+1} − o_n entered the headroom learner
        bool first_work_streamed = false; // LeadGpuReport::first_work_streamed
        int64_t gpu_start_qpc = 0;      // Reflex gpuRenderStartTime as QPC; 0 unknown
        bool concurrency_recorded = false; // (o_n, g_{n+1}) entered the queue-order window
        bool release_to_render_recorded = false; // σ of this frame entered its window, or given up
        bool spike_recorded = false;     // its spike floor arrival was decided (added or left out)
        bool spike_gpu_recorded = false; // its r − g was decided
        bool gpu_target = false;         // LeadFrameStart::gpu_target
    };

    // The newest LEAD_SLACK_CAPACITY slack signs of one bucket.
    struct SlackWindow {
        std::array<uint8_t, LEAD_SLACK_CAPACITY> negative{}; // 1 when the frame was ready after its target
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
        double lead_us = 0.0;          // the lead before the collision guard
        double returned_lead_us = 0.0; // the lead NextLeadUs returned last
        bool held_back = false;        // returned_lead_us was below lead_us (the guard or its slew)
        double budget_us = 0.0;
        bool warm_applied = false;
        bool budget_limited = false;
        bool spike_floor_limited = false; // the spike floor set the last warm target
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
    // The GPU, not ReLimiter, set the frame's pace (above): it had no hold,
    // or its GPU reached the wait after the Signal (probe pre-wait time, else
    // readiness, strictly later). A hold without that evidence counts as
    // waited, so missing evidence never feeds P or a gap.
    static bool GpuSetPace(const PendingFrame& frame) noexcept;
    // The pending NvAPI frame (producer_epoch, frame_id), nullptr when it is
    // not pending.
    const PendingFrame* FindNvApiFrame(uint32_t producer_epoch, uint64_t frame_id) const noexcept;
    void NoteReadyAnchor(const PendingFrame& frame) noexcept;
    void RecordReadyGap(const PendingFrame& frame, int64_t gpu_end_qpc) noexcept;
    // Median of the readiness gaps; meaningful from LEAD_WARM_SAMPLES.
    double ReadyGapMedianUs() const noexcept;
    // A reported held frame's cap bound evidence (above).
    void NoteCapBoundEvidence(const PendingFrame& frame) noexcept;
    void RecordGpuNeed(const PendingFrame& frame, double work_us) noexcept;
    // Copies the newest `count` u − o observations (count <= resume_delay_count_).
    void NewestResumeDelays(size_t count, double* delays_us) const noexcept;
    // Collision guard
    void RecordCallTime(PendingFrame* frame) noexcept;
    void RecordReleaseMargin(PendingFrame* frame) noexcept;
    void RecordWorkStart(PendingFrame* frame) noexcept;
    void RecordReleaseHeadroom(PendingFrame* frame) noexcept;
    // σ of a frame not held (GPU floor, above).
    void RecordReleaseToRender(PendingFrame* frame) noexcept;
    // One queue-order pair (o_n, g_{n+1}) of a waited GPU-rung frame n.
    void RecordConcurrency(PendingFrame* frame) noexcept;
    double CollisionGuardedLeadUs(const LeadRequest& request, size_t bucket_index, double lead_us,
        LeadGuardOutcome* guard) noexcept;
    // The guard's limit minus I for an FG divisor slot: Q10(D) − headroom_us
    // while the heavy-work window is warm and current (*headroom true), else
    // Q10(C) − LEAD_RELEASE_MARGIN_US while the call window is warm; false
    // when neither is. One evaluator for the guard and the hold room.
    bool CollisionOffsetUs(size_t divisor, double headroom_us, double* offset_us, bool* headroom) const noexcept;
    bool WorkCurrent(size_t divisor) const noexcept;
    bool BehindWait(PacingRung rung, size_t divisor) const noexcept;
    // F, the median first submission − RENDERSUBMIT_START of the stream
    // proof's pairs (GPU floor, above); 0 until LEAD_WARM_SAMPLES.
    double FirstSubmissionUs() const noexcept;

    // Sample assembly
    bool PreviousResolved(const PendingFrame& frame) const noexcept;
    void TryFinalize(PendingFrame* frame) noexcept;
    void ForceFinalize(PendingFrame* frame) noexcept;
    // The outcome at readiness: render handoff, readiness rings, slack and
    // hold room, all from the measured readiness. The lead window sample
    // follows it, but behind the wait a frame the cap released waits for its
    // Reflex report (LeadSampleWaitsForReport) or closes without one.
    void EmitOutcome(PendingFrame* frame) noexcept;
    void EmitLeadSample(PendingFrame* frame) noexcept;
    bool LeadSampleWaitsForReport(const PendingFrame& frame) const noexcept;
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
    // The game's simulation → render queue q (above), in µs: FG input wait,
    // and GPU queue wait per its render_queue; 0 for other rungs or without
    // e, w, p and the previous frame's b.
    double QueueUs(const PendingFrame& frame, const PendingFrame* previous) const noexcept;
    // q itself, whatever the rung's render queue setting.
    double RenderQueueWaitUs(const PendingFrame& frame, const PendingFrame& previous) const noexcept;
    // Spike floor (above): a frame's arrival once its report came, its r − g
    // once both are known, and a restart of both memories.
    void RecordSpikeArrival(PendingFrame* frame, double first_submission_us) noexcept;
    void RecordSpikeGpu(PendingFrame* frame) noexcept;
    void ResetSpikeFloor() noexcept;
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
    // A GPU-rung frame's GPU waited at its gate (probe w <= o, else held and
    // ready before the Signal): a frame the cap held (cap bound), a queue-order
    // pair's frame n on the FG input wait, the render queue's RemoveHeld.
    static bool PreviousGpuHeld(const PendingFrame& previous) noexcept;
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
    // GPU work times D of GPU queue wait frames per FG divisor, the GPU queue
    // wait releases since each work window's newest sample, and the hold
    // room's X per FG divisor.
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
    // GpuSetPace of the newest report's frame, refreshed by a probe that
    // arrives after the report: the next report's period pair reads it.
    bool gpu_end_gpu_set_ = false;
    LeadSampleWindow gpu_period_window_{};
    // P in use before the newest ResetGpuFloor, until gpu_period_window_ is
    // warm again (0: none; a lifecycle reset or a new producer epoch clears it).
    double gpu_period_carry_us_ = 0.0;
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
    // Cap bound (above): the evidence window (bit i of bits: the entry i
    // places back, newest at bit 0), its mode and blend (1: the cap's
    // release), and the producer epoch of the newest cadence restart (0
    // none).
    struct CapBoundWindow {
        std::array<uint64_t, 2> bits{};
        uint32_t frames = 0;
        uint32_t waited = 0;
        CapBoundMode mode = CapBoundMode::Cold;
        float blend = 1.0f;
        uint64_t switches = 0;
    };
    CapBoundWindow cap_bound_{};
    uint32_t restart_epoch_ = 0;
    // GPU need window (above); divisor = the frames' FG divisor slot, epoch
    // their producer epoch.
    struct GpuNeedWindow {
        std::array<float, LEAD_GPU_NEED_CAPACITY> us{};
        uint32_t head = 0;
        uint32_t count = 0;
        size_t divisor = 0;
        uint32_t epoch = 0;
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
    // The measured readiness (release → GPU ready) of every GPU-rung frame per
    // FG divisor slot: the early side's stand-in until the GPU floor's ring is
    // warm (EarlyLeadUs). The lead samples leave out the wait for frame n − 1
    // behind the wait, so they would put such a frame's target earlier than it
    // can be ready.
    std::array<GpuPacedRing, LEAD_DIVISOR_BUCKETS> gpu_ready_rings_{};
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
    PacingPlanKey lifecycle_{}; // identity domain, producer epoch and generation of the newest release
    uint64_t newest_index_ = 0;
    LeadStatistics statistics_{};
};
